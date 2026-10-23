// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
#include "GuestCpuRuntime.h"
#include "QemuProvider.h"

#include <array>
#include <atomic>
#include <cfenv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr auto Ready = LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1;
constexpr auto GuestFault = LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1;
constexpr std::uint64_t GuestPage = 4096, LargeSize = 1024 * 1024;
constexpr std::uint8_t Read = 1, Write = 2, Execute = 4;
std::uint64_t code_base {}, code_size {}, data_base {}, data_size {}, host_page {};
std::uint64_t large_base {};
std::array<std::uint8_t, LargeSize / GuestPage> large_rights {};
unsigned large_data_queries {}, large_code_queries {};
bool whole_read_proved {}, whole_write_proved {};
bool code_denied {};
std::uint64_t denied_data_page {};
std::atomic<std::uint32_t>* exit_flag {};
std::atomic<unsigned> deliveries {};
std::atomic<bool> delivery_fp_valid {true};
unsigned failures {};
unsigned native_jit_mode_changes {};

void ChangeNativeJitMode() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
    // Simulate an unrelated native JIT used by HLE. A resumed or nested CPU
    // root must establish its own execute state after this real MAP_JIT change.
    pthread_jit_write_protect_np(false);
    ++native_jit_mode_changes;
#endif
}

void Check(bool value, const char* description) {
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", description); }
}
bool Contains(std::uint64_t base, std::uint64_t span, std::uint64_t address,
              std::uint64_t size) noexcept {
    return address >= base && size != 0 && address - base < span &&
        size <= span - (address - base);
}
std::uint32_t DataQuery(std::uint64_t address, std::uint64_t size,
                        std::uint32_t write) noexcept {
    if (write > 1) return 0;
    if (denied_data_page != 0 && address < denied_data_page + GuestPage &&
        denied_data_page < address + size) return 0;
    if (Contains(data_base, data_size, address, size)) return 1;
    if (write == 0 && Contains(code_base, code_size, address, size)) return 1;
    if (!Contains(large_base, LargeSize, address, size)) return 0;
    ++large_data_queries;
    const auto first = (address - large_base) / GuestPage;
    const auto last = (address - large_base + size - 1) / GuestPage;
    const auto required = write != 0 ? Write : Read;
    for (auto page = first; page <= last; ++page)
        if ((large_rights[page] & required) == 0) return 0;
    if (address == large_base && size == LargeSize) {
        if (write != 0) whole_write_proved = true;
        else whole_read_proved = true;
    }
    return 1;
}
std::uint32_t CodeQuery(void*, std::uint64_t address, std::uint64_t* base,
    std::uint64_t* size, std::uint32_t* writable) noexcept {
    if (Contains(code_base, code_size, address, 1)) {
        if (code_denied) return 0;
        *base = code_base; *size = code_size; *writable = 0;
        return 1;
    }
    if (!Contains(large_base, LargeSize, address, 1)) return 0;
    ++large_code_queries;
    const auto page = (address - large_base) / GuestPage;
    if ((large_rights[page] & Execute) == 0) return 0;
    // Deliberately return only the proved guest page, as macOS code leases do.
    *base = large_base + page * GuestPage; *size = GuestPage;
    *writable = (large_rights[page] & Write) != 0;
    return 1;
}
void HostTargetIdentity() {}
struct Context {
    bool nested {};
    unsigned calls {};
    bool valid {true};
    bool expected_fault {};
    bool revoke_write {};
    std::uint64_t revoke_address {}, fault_address {}, rax {};
    std::uint32_t fault_signal {};
    bool fresh_callback {}, fresh_mutator {}, fresh_valid {true};
    unsigned revoke_parent {};
    std::uint64_t tls {};
    bool change_jit_mode {};
};
std::uint32_t HostCall(void*, std::uint64_t, std::uint64_t*, std::uint64_t*) noexcept;

std::uint32_t Run(Context& context, std::uint64_t entry, std::uint64_t stack_end) {
    LumenAndroidFexCoreGuestStateV1 state {};
    state.structSize = sizeof(state);
    state.abiVersion = 1;
    state.rip = entry;
    state.gprs[4] = stack_end - 8;
    state.fsBase = context.tls;
    const auto root = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &root, 8);
    const auto result = LumenAndroidFexCoreRunGuestV1(&state, HostCall, CodeQuery, &context);
    if (result != Ready && !context.expected_fault) std::fprintf(stderr,
        "QemuProviderTest: run code=%u rip=%#llx fault=%#llx signal=%u calls=%u\n", result,
        static_cast<unsigned long long>(state.rip), static_cast<unsigned long long>(state.faultAddress),
        state.faultSignal, context.calls);
    context.fault_address = state.faultAddress;
    context.fault_signal = state.faultSignal;
    context.rax = state.gprs[0];
    context.valid = context.valid && (context.expected_fault || state.rip == root);
    return result;
}

std::uint32_t ExtendedHostCall(void* opaque, std::uint64_t target, std::uint64_t* gprs,
    LumenAndroidFexCoreSimdViewV2*, std::uint64_t* next_rip) noexcept {
    return HostCall(opaque, target, gprs, next_rip);
}

bool RunQuantumState(std::uint64_t stack_end, std::uint64_t output,
                     std::uint8_t pattern, std::uint32_t rounding) {
    LumenAndroidFexCoreGuestStateV3 state {};
    state.gp.rip = code_base + 896;
    state.gp.gprs[4] = stack_end - 8;
    state.gp.gprs[0] = 41;
    state.gp.gprs[1] = 1000000;
    state.gp.gprs[2] = pattern;
    state.gp.gprs[3] = 0x1234567800000000ULL | pattern;
    state.gp.gprs[8] = output;
    state.mxcsr = 0x1f80 | rounding;
    state.x87ControlWord = 0x037f | (rounding >> 3);
    std::memset(state.vectorRegisters[0], pattern, sizeof(state.vectorRegisters[0]));
    const std::uint32_t x87_integer = pattern;
    std::memcpy(reinterpret_cast<void*>(output + 32), &x87_integer, sizeof(x87_integer));
    const auto root = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(state.gp.gprs[4]), &root, 8);
    Context context;
    const auto result = LumenAndroidFexCoreRunGuestV3(&state, ExtendedHostCall, CodeQuery, &context);
    bool vector_valid = true;
    for (const auto byte: state.vectorRegisters[0]) vector_valid = vector_valid && byte == pattern;
    const auto* stored = reinterpret_cast<const std::uint8_t*>(output);
    for (unsigned index = 0; index < 16; ++index) vector_valid = vector_valid && stored[index] == pattern;
    double x87_value {};
    std::memcpy(&x87_value, stored + 16, sizeof(x87_value));
    return result == Ready && state.gp.rip == root && context.calls == 0 &&
        state.gp.gprs[0] == 42 && state.gp.gprs[1] == 0 && state.gp.gprs[2] == 1000000 + pattern &&
        state.gp.gprs[3] == (0x1234567800000000ULL | pattern) && vector_valid && x87_value == pattern &&
        state.mxcsr == (0x1f80 | rounding) && state.x87ControlWord == (0x037f | (rounding >> 3));
}

std::uint32_t HostCall(void* opaque, std::uint64_t target, std::uint64_t* gprs,
                       std::uint64_t* next_rip) noexcept {
    auto& context = *static_cast<Context*>(opaque);
    if (target != reinterpret_cast<std::uintptr_t>(&HostTargetIdentity))
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    if (context.change_jit_mode) ChangeNativeJitMode();
    const auto expected = context.nested ? FE_UPWARD : FE_DOWNWARD;
    context.valid = context.valid && std::fegetround() == expected;
    if (context.fresh_callback) {
        using namespace Lumen::GuestCpu;
        const auto begin = [&] {
            return QemuBeginFreshCallback(data_base, host_page,
                context.tls != 0 ? data_base + 3 * host_page : 0,
                context.tls != 0 ? 2 * host_page : 0);
        };
        context.fresh_valid = context.fresh_valid &&
            LumenAndroidFexCoreRetainGuestDataLeaseV1() == 1 && begin() == 0 &&
            LumenAndroidFexCoreReleaseGuestDataLeaseV1() == 1;
        const auto token = begin();
        if (token == 0) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
        context.fresh_valid = context.fresh_valid && !QemuEndFreshCallback(token + 1);
        bool foreign_rejected {};
        std::thread foreign([&] { foreign_rejected = !QemuEndFreshCallback(token); });
        foreign.join();
        const auto peer_mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        context.fresh_valid = context.fresh_valid && foreign_rejected && peer_mutation != 0;
        if (peer_mutation != 0) {
            // Ordinary callback frame data writes share the parent's complete
            // stack allocation. They invalidate CPU caches, not map identity.
            const std::uint64_t marker = 0x1234;
            std::memcpy(reinterpret_cast<void*>(gprs[4] - 256), &marker, sizeof(marker));
            LumenAndroidFexCoreEndGuestMemoryMutationV1(peer_mutation, gprs[4] - 256, 8, 1);
        }
        context.fresh_valid = context.fresh_valid && std::fesetround(FE_UPWARD) == 0;
        if (context.revoke_parent <= 6) {
            Context nested {.nested = true, .fresh_mutator = true};
            context.fresh_valid = context.fresh_valid &&
                Run(nested, code_base + 64, gprs[4] - 256) == Ready &&
                nested.valid && nested.calls == 1 && nested.fresh_valid &&
                std::fegetround() == FE_UPWARD;
        }
        if (context.revoke_parent != 0) {
            const auto mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            context.fresh_valid = context.fresh_valid && mutation != 0;
            if (mutation != 0) {
                const auto kind = (context.revoke_parent - 1) % 3 + 1;
                if (kind == 1) code_denied = true;
                else if (kind == 2) denied_data_page = gprs[4] & ~(GuestPage - 1);
                else denied_data_page = context.tls & ~(GuestPage - 1);
                const auto revoked_base = kind == 1 ? code_base : denied_data_page;
                const auto revoked_size = kind == 1 ? code_size : GuestPage;
                QemuNotifyMappingChange(revoked_base, revoked_size);
                if (context.revoke_parent > 3) {
                    // Simulate replacement/regrant at identical virtual
                    // addresses and identical final permission proofs (ABA).
                    code_denied = false; denied_data_page = 0;
                    QemuNotifyMappingChange(revoked_base, revoked_size);
                }
                context.fresh_valid = context.fresh_valid && !QemuValidateFreshCallbackParent();
                LumenAndroidFexCoreEndGuestMemoryMutationV1(mutation,
                    revoked_base, revoked_size, 1);
            }
        }
        if (context.revoke_parent >= 7) {
            // No stack preparation writes precede this provider entry. Same
            // VA/regranted rights must deny the child before any guest HLE or
            // instruction can touch the captured replacement mappings.
            LumenAndroidFexCoreGuestStateV1 child {};
            child.structSize = sizeof(child); child.abiVersion = 1;
            child.rip = code_base + 64; child.gprs[4] = gprs[4] - 256;
            child.fsBase = context.tls;
            Context nested {.nested = true};
            const auto denied = LumenAndroidFexCoreRunGuestV1(&child, HostCall, CodeQuery, &nested);
            std::uint64_t marker {};
            std::memcpy(&marker, reinterpret_cast<const void*>(child.gprs[4]), sizeof(marker));
            context.fresh_valid = context.fresh_valid &&
                denied == LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1 &&
                nested.calls == 0 && marker == 0x1234;
        }
        const bool restored = QemuEndFreshCallback(token);
        context.fresh_valid = context.fresh_valid &&
            restored == (context.revoke_parent == 0) && !QemuEndFreshCallback(token);
        if (!restored) {
            ++context.calls;
            return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;
        }
    } else if (context.fresh_mutator) {
        const auto mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        context.fresh_valid = context.fresh_valid && mutation != 0;
        if (mutation != 0) LumenAndroidFexCoreEndGuestMemoryMutationV1(mutation, 0, 0, 0);
    } else if (context.revoke_write) {
        const auto mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (mutation == 0) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
        large_rights[(context.revoke_address - large_base) / GuestPage] = Read;
        LumenAndroidFexCoreEndGuestMemoryMutationV1(mutation, context.revoke_address, GuestPage, 1);
    } else if (!context.nested && context.calls == 0) {
        context.valid = context.valid && std::fesetround(FE_UPWARD) == 0;
        Context nested {.nested = true, .change_jit_mode = context.change_jit_mode};
        const auto result = Run(nested, code_base + 64, data_base + 2 * host_page);
        context.valid = context.valid && result == Ready && nested.valid && nested.calls == 1 &&
            std::fegetround() == FE_UPWARD;
    } else {
        context.valid = context.valid && std::fesetround(FE_TONEAREST) == 0;
    }
    ++context.calls;
    std::memcpy(next_rip, reinterpret_cast<const void*>(gprs[4]), 8);
    gprs[4] += 8;
    if (context.change_jit_mode) ChangeNativeJitMode();
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

std::uint32_t Delivery(const std::uint64_t*, std::uint64_t) noexcept {
    delivery_fp_valid.store(std::fegetround() == FE_DOWNWARD &&
        std::fesetround(FE_UPWARD) == 0, std::memory_order_release);
    deliveries.fetch_add(1, std::memory_order_release);
    exit_flag->store(1, std::memory_order_release);
    // A consumed request can legitimately have no pending guest handler.
    return 0;
}

void EmitCall(std::uint8_t*& cursor) {
    *cursor++ = 0x48; *cursor++ = 0xb8; // movabs rax, registered HLE target
    const auto target = reinterpret_cast<std::uintptr_t>(&HostTargetIdentity);
    std::memcpy(cursor, &target, 8); cursor += 8;
    *cursor++ = 0xff; *cursor++ = 0xd0; // call rax
}

void EmitAddress(std::uint8_t*& cursor, std::uint64_t address) {
    *cursor++ = 0x48; *cursor++ = 0xb8; // movabs rax, data address
    std::memcpy(cursor, &address, 8); cursor += 8;
}

void ChangeRights(const std::array<std::uint8_t, LargeSize / GuestPage>& rights) {
    const auto mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (mutation == 0) std::_Exit(2);
    large_rights = rights;
    LumenAndroidFexCoreEndGuestMemoryMutationV1(mutation, large_base, LargeSize, 1);
}
} // namespace

int main() {
    std::signal(SIGALRM, +[](int) { std::_Exit(2); });
    alarm(30);
    std::fenv_t original {};
    if (std::fegetenv(&original) != 0) return 2;
    host_page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    if (host_page < 4096 || host_page % 4096 != 0) return 2;
    code_size = host_page;
    data_size = 5 * host_page;
    void* code = mmap(nullptr, code_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    void* data = mmap(nullptr, data_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    void* large_allocation = mmap(nullptr, 2 * LargeSize, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (code == MAP_FAILED || data == MAP_FAILED || large_allocation == MAP_FAILED) return 2;
    code_base = reinterpret_cast<std::uintptr_t>(code);
    data_base = reinterpret_cast<std::uintptr_t>(data);
    large_base = (reinterpret_cast<std::uintptr_t>(large_allocation) + LargeSize - 1) & ~(LargeSize - 1);
    large_rights.fill(Read | Write);
    exit_flag = new (static_cast<std::uint8_t*>(data) + 2 * host_page) std::atomic<std::uint32_t>(0);
    auto* spin_started = new (static_cast<std::uint8_t*>(data) + 2 * host_page + 64)
        std::atomic<std::uint32_t>(0);
    auto* spin_dependency = new (static_cast<std::uint8_t*>(data) + 2 * host_page + 68)
        std::atomic<std::uint32_t>(0);
    auto* cursor = static_cast<std::uint8_t*>(code);
    EmitCall(cursor); EmitCall(cursor); *cursor = 0xc3;
    cursor = static_cast<std::uint8_t*>(code) + 64;
    EmitCall(cursor); *cursor = 0xc3;
    cursor = static_cast<std::uint8_t*>(code) + 128;
    *cursor++ = 0x48; *cursor++ = 0xb8;
    const auto flag_address = reinterpret_cast<std::uintptr_t>(exit_flag);
    std::memcpy(cursor, &flag_address, 8); cursor += 8;
    const std::uint8_t loop[] {0x8b, 0x08, 0x85, 0xc9, 0x74, 0xfa, 0xc3};
    std::memcpy(cursor, loop, sizeof(loop));
    cursor = static_cast<std::uint8_t*>(code) + 256;
    EmitAddress(cursor, large_base);
    *cursor++ = 0xb9; // mov ecx, number of guest pages
    const std::uint32_t page_count = LargeSize / GuestPage;
    std::memcpy(cursor, &page_count, 4); cursor += 4;
    // Read a page before its first write, then touch every page in the region.
    const std::uint8_t page_loop[] {0x48, 0x8b, 0x10, 0x48, 0x83, 0xc2, 0x01,
        0x48, 0x89, 0x10, 0x48, 0x05, 0x00, 0x10, 0x00, 0x00,
        0x83, 0xe9, 0x01, 0x75, 0xeb, 0xc3};
    std::memcpy(cursor, page_loop, sizeof(page_loop));
    cursor = static_cast<std::uint8_t*>(code) + 320;
    EmitAddress(cursor, large_base + 4 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x00; *cursor = 0xc3; // read RX
    cursor = static_cast<std::uint8_t*>(code) + 352;
    EmitAddress(cursor, large_base + 4 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0xc7; *cursor++ = 0x00;
    const std::uint32_t replacement = 99;
    std::memcpy(cursor, &replacement, 4); cursor += 4; *cursor = 0xc3; // write RX
    cursor = static_cast<std::uint8_t*>(code) + 384;
    EmitAddress(cursor, large_base + 8 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x00; *cursor = 0xc3; // hole read
    cursor = static_cast<std::uint8_t*>(code) + 416;
    EmitAddress(cursor, large_base + 12 * GuestPage);
    *cursor++ = 0xff; *cursor = 0xe0; // jmp to RW data page
    cursor = static_cast<std::uint8_t*>(code) + 448;
    EmitAddress(cursor, large_base + 16 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x10; // warm mapping before HLE
    EmitCall(cursor);
    EmitAddress(cursor, large_base + 16 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0xc7; *cursor++ = 0x00;
    std::memcpy(cursor, &replacement, 4); cursor += 4; *cursor = 0xc3;
    cursor = static_cast<std::uint8_t*>(code) + 512;
    EmitAddress(cursor, large_base + 5 * GuestPage);
    const std::uint8_t adjacent_rw[] {0x48, 0x8b, 0x10, 0x48, 0x83, 0xc2, 0x01,
        0x48, 0x89, 0x10, 0xc3};
    std::memcpy(cursor, adjacent_rw, sizeof(adjacent_rw));
    cursor = static_cast<std::uint8_t*>(code) + 576;
    EmitAddress(cursor, large_base);
    *cursor++ = 0xb9;
    std::memcpy(cursor, &page_count, 4); cursor += 4;
    const std::uint8_t read_loop[] {0x48, 0x8b, 0x10, 0x48, 0x05, 0x00, 0x10, 0x00, 0x00,
        0x83, 0xe9, 0x01, 0x75, 0xf2};
    std::memcpy(cursor, read_loop, sizeof(read_loop)); cursor += sizeof(read_loop);
    EmitAddress(cursor, large_base);
    *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x00; *cursor = 0xc3;
    cursor = static_cast<std::uint8_t*>(code) + 640;
    EmitAddress(cursor, reinterpret_cast<std::uintptr_t>(spin_started));
    *cursor++ = 0xc7; *cursor++ = 0x00;
    const std::uint32_t started_value = 1;
    std::memcpy(cursor, &started_value, 4); cursor += 4;
    EmitAddress(cursor, reinterpret_cast<std::uintptr_t>(spin_dependency));
    const std::uint8_t dependency_loop[] {0x83, 0x38, 0x01, 0x75, 0xfb, 0xc3};
    std::memcpy(cursor, dependency_loop, sizeof(dependency_loop));
    cursor = static_cast<std::uint8_t*>(code) + 704;
    EmitAddress(cursor, reinterpret_cast<std::uintptr_t>(spin_dependency));
    *cursor++ = 0xc7; *cursor++ = 0x00;
    std::memcpy(cursor, &started_value, 4); cursor += 4; *cursor = 0xc3;
    cursor = static_cast<std::uint8_t*>(code) + 768;
    EmitAddress(cursor, large_base + 20 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x10; // warm view before external revoke
    EmitAddress(cursor, reinterpret_cast<std::uintptr_t>(spin_started));
    *cursor++ = 0xc7; *cursor++ = 0x00;
    std::memcpy(cursor, &started_value, 4); cursor += 4;
    EmitAddress(cursor, reinterpret_cast<std::uintptr_t>(spin_dependency));
    std::memcpy(cursor, dependency_loop, sizeof(dependency_loop) - 1); cursor += sizeof(dependency_loop) - 1;
    EmitAddress(cursor, large_base + 20 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x00; *cursor = 0xc3;
    cursor = static_cast<std::uint8_t*>(code) + 896;
    // Carry, all YMM bytes, an x87 stack value and per-root FP controls span
    // many scheduling boundaries. LEA/LOOP deliberately preserve carry.
    const std::uint8_t quantum_state[] {0x41, 0xdb, 0x40, 0x20, 0xf9, // fild dword [r8+32]; stc
        0x48, 0x8d, 0x52, 0x01, 0xe2, 0xfa, // lea rdx,[rdx+1]; loop
        0x48, 0x83, 0xd0, 0x00, // adc rax,0
        0xf3, 0x41, 0x0f, 0x7f, 0x00, // movdqu [r8],xmm0
        0x41, 0xdd, 0x58, 0x10, 0xc3}; // fstp qword [r8+16]; ret
    std::memcpy(cursor, quantum_state, sizeof(quantum_state));
    cursor = static_cast<std::uint8_t*>(code) + 960;
    EmitAddress(cursor, large_base + 4 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x10; // old admitted half
    EmitAddress(cursor, large_base + 132 * GuestPage);
    *cursor++ = 0x48; *cursor++ = 0x8b; *cursor++ = 0x00; *cursor = 0xc3; // new half
    if (mprotect(code, code_size, PROT_READ | PROT_EXEC) != 0) return 2;
    using namespace Lumen::GuestCpu;
    unsetenv("LUMEN_CPU_TRANSLATOR");
    Check(Selected() == Provider::Fex && !SelectionLocked(),
        "build including QEMU defaults to FEX before admission");
    if (Select("qemu-tcg") != SelectionResult::Selected) return 2;
    LumenAndroidFexCoreGuestTscV1 uninitialized_clock {};
    Check(LumenAndroidFexCoreQueryGuestTscV1(&uninitialized_clock) ==
        LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1 && uninitialized_clock.frequency == 0,
        "native clock query does not initialize QEMU");
    if (!RegisterMemoryAccessQuery(DataQuery) ||
        LumenAndroidFexCoreRegisterHostCallTargetV1(
            reinterpret_cast<std::uintptr_t>(&HostTargetIdentity)) == 0 ||
        LumenAndroidFexCoreRegisterBoundaryDeliveryV1(Delivery) == 0) return 2;

    Check(std::fesetround(FE_DOWNWARD) == 0, "native FP setup");
    Context outer {.change_jit_mode = true};
    Check(Run(outer, code_base, data_base + host_page) == Ready, "HLE/nested roots return");
    Check(outer.valid && outer.calls == 2, "HLE entry and nested native FP isolation");
    Check(std::fegetround() == FE_DOWNWARD, "root restores native caller FP");

    Context warm_outer {.change_jit_mode = true};
    Check(Run(warm_outer, code_base, data_base + host_page) == Ready &&
        warm_outer.valid && warm_outer.calls == 2 && std::fegetround() == FE_DOWNWARD,
        "warm CPU and nested roots resume after HLE changes native MAP_JIT mode");
#if defined(__APPLE__) && defined(__aarch64__)
    Check(native_jit_mode_changes == 12,
        "cold and warm HLE/nested fixtures perform real native JIT write transitions");
#endif
    std::fprintf(stderr, "QEMU_PROVIDER_JIT_SCOPE_FIXTURE_V1 native_mode_changes=%u "
        "cold_and_warm_nested_hle=checked audible_quality=unmeasured\n", native_jit_mode_changes);

    Context fresh {.fresh_callback = true};
    Check(Run(fresh, code_base + 64, data_base + host_page) == Ready &&
        fresh.valid && fresh.calls == 1 && fresh.fresh_valid && std::fegetround() == FE_DOWNWARD,
        "fresh callback rejects borrow and foreign/stale tokens, permits exclusive mutation and restores native FP");
    for (const auto revoked: {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U}) {
        Context parent {.expected_fault = true, .fresh_callback = true,
            .revoke_parent = revoked, .tls = data_base + 4 * host_page};
        Check(Run(parent, code_base + 64, data_base + host_page) ==
                LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1 && parent.fresh_valid &&
                parent.calls == 1 && std::fegetround() == FE_DOWNWARD,
            "fresh callback denies parent CPU resume after code/stack/TLS revoke or same-address replacement");
        const auto reset = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (reset == 0) return 2;
        code_denied = false; denied_data_page = 0;
        LumenAndroidFexCoreEndGuestMemoryMutationV1(reset, 0, 0, 1);
    }

    std::atomic<std::uint64_t> worker_id {};
    std::uint32_t worker_result {};
    bool worker_fp_valid {}, worker_return_valid {};
    std::thread worker([&] {
        worker_fp_valid = std::fesetround(FE_DOWNWARD) == 0;
        const auto self = pthread_self();
        std::uint64_t identity {};
        static_assert(sizeof(self) <= sizeof(identity));
        std::memcpy(&identity, &self, sizeof(self));
        worker_id.store(identity, std::memory_order_release);
        Context context;
        worker_result = Run(context, code_base + 128, data_base + host_page);
        worker_return_valid = context.valid;
        worker_fp_valid = worker_fp_valid && std::fegetround() == FE_DOWNWARD;
    });
    std::uint64_t token {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (token == 0 && std::chrono::steady_clock::now() < deadline) {
        token = LumenAndroidFexCoreRequestGuestBoundaryInterruptV1(worker_id.load(std::memory_order_acquire));
        if (token == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (token == 0) exit_flag->store(1, std::memory_order_release);
    worker.join();
    Check(token != 0 && deliveries.load(std::memory_order_acquire) == 1, "boundary request delivered once");
    Check(worker_result == Ready && worker_return_valid, "zero delivery result resumes guest execution");
    Check(LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(token) == 0 &&
        LumenAndroidFexCoreCancelGuestBoundaryInterruptV1(UINT64_MAX) == 0,
        "consumed and unknown interrupt requests are not reported as cancelled");
    Check(delivery_fp_valid.load(std::memory_order_acquire) && worker_fp_valid,
        "boundary delivery restores native caller FP");

    // A real guest reader cannot finish until a second guest CPU writes RAM.
    // The writer's first Open occurs on a new native thread while the reader
    // is executing. No HLE or user interrupt supplies either CPU's progress.
    std::uint32_t spin_reader_result {}, spin_writer_result {};
    bool spin_reader_valid {}, spin_writer_valid {};
    std::atomic<bool> spin_reader_done {}, spin_writer_done {};
    std::thread spin_reader([&] {
        Context context;
        spin_reader_result = Run(context, code_base + 640, data_base + host_page);
        spin_reader_valid = context.valid;
        spin_reader_done.store(true, std::memory_order_release);
    });
    const auto reader_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (spin_started->load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < reader_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool reader_started = spin_started->load(std::memory_order_acquire) == 1;
    std::thread spin_writer([&] {
        Context context;
        spin_writer_result = Run(context, code_base + 704, data_base + 2 * host_page);
        spin_writer_valid = context.valid;
        spin_writer_done.store(true, std::memory_order_release);
    });
    const auto pair_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!spin_reader_done.load(std::memory_order_acquire) ||
            !spin_writer_done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < pair_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool pair_completed = spin_reader_done.load(std::memory_order_acquire) &&
        spin_writer_done.load(std::memory_order_acquire);
    // Safety cleanup for the negative control. Its native store is never
    // counted as a guest writer and makes the progress assertion fail.
    if (!pair_completed) spin_dependency->store(1, std::memory_order_release);
    spin_reader.join(); spin_writer.join();
    Check(reader_started && pair_completed && spin_reader_result == Ready && spin_writer_result == Ready &&
        spin_reader_valid && spin_writer_valid && spin_dependency->load(std::memory_order_acquire) == 1,
        "independent guest spin roots allow the first-Open peer CPU to satisfy the RAM dependency");
    Check(deliveries.load(std::memory_order_acquire) == 1,
        "CPU scheduling quanta never deliver a guest exception request");

    spin_started->store(0, std::memory_order_release);
    spin_dependency->store(0, std::memory_order_release);
    spin_reader_done.store(false, std::memory_order_release);
    spin_writer_done.store(false, std::memory_order_release);
    Context revoked_reader {.expected_fault = true};
    std::thread mutation_reader([&] {
        spin_reader_result = Run(revoked_reader, code_base + 768, data_base + host_page);
        spin_reader_done.store(true, std::memory_order_release);
    });
    const auto mutation_start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (spin_started->load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < mutation_start_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool mutation_reader_started = spin_started->load(std::memory_order_acquire) == 1;
    auto revoked_rights = large_rights;
    revoked_rights[20] = 0;
    const auto active_mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    Check(active_mutation != 0, "external writer quiesces a CPU-only guest loop");
    if (active_mutation != 0) {
        large_rights = revoked_rights;
        LumenAndroidFexCoreEndGuestMemoryMutationV1(active_mutation, large_base + 20 * GuestPage, GuestPage, 1);
    }
    std::thread mutation_writer([&] {
        Context context;
        spin_writer_result = Run(context, code_base + 704, data_base + 2 * host_page);
        spin_writer_valid = context.valid;
        spin_writer_done.store(true, std::memory_order_release);
    });
    const auto mutation_pair_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!spin_reader_done.load(std::memory_order_acquire) ||
            !spin_writer_done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < mutation_pair_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool mutation_pair_completed = spin_reader_done.load(std::memory_order_acquire) &&
        spin_writer_done.load(std::memory_order_acquire);
    if (!mutation_pair_completed) spin_dependency->store(1, std::memory_order_release);
    mutation_reader.join(); mutation_writer.join();
    Check(mutation_reader_started && mutation_pair_completed && active_mutation != 0 &&
        spin_reader_result == GuestFault && revoked_reader.fault_signal == SIGSEGV &&
        revoked_reader.fault_address == large_base + 20 * GuestPage &&
        spin_writer_result == Ready && spin_writer_valid,
        "CPU-only roots resume fairly after mutation and cannot reuse a revoked RAM view");
    revoked_rights[20] = Read | Write;
    ChangeRights(revoked_rights);

    bool first_quantum_state {}, second_quantum_state {}, first_quantum_fp {}, second_quantum_fp {};
    std::thread first_quantum([&] {
        first_quantum_fp = std::fesetround(FE_DOWNWARD) == 0;
        first_quantum_state = RunQuantumState(data_base + 4 * host_page,
            data_base + 2 * host_page + 128, 0x35, 0x2000);
        first_quantum_fp = first_quantum_fp && std::fegetround() == FE_DOWNWARD;
    });
    std::thread second_quantum([&] {
        second_quantum_fp = std::fesetround(FE_UPWARD) == 0;
        second_quantum_state = RunQuantumState(data_base + 5 * host_page,
            data_base + 2 * host_page + 192, 0xa9, 0x4000);
        second_quantum_fp = second_quantum_fp && std::fegetround() == FE_UPWARD;
    });
    first_quantum.join(); second_quantum.join();
    Check(first_quantum_state && second_quantum_state && first_quantum_fp && second_quantum_fp,
        "concurrent CPU quanta preserve separate GP/carry/YMM/x87 and native/guest FP state");
    Check(deliveries.load(std::memory_order_acquire) == 1,
        "mutation and state scheduling boundaries never consume or synthesize a guest delivery");

    large_data_queries = large_code_queries = 0;
    whole_read_proved = whole_write_proved = false;
    Context large;
    Check(Run(large, code_base + 256, data_base + host_page) == Ready && large.valid,
        "real CPU reads then writes every page of a large RW region");
    bool values_valid = true;
    for (std::uint64_t page = 0; page < LargeSize / GuestPage; ++page)
        if (*reinterpret_cast<const std::uint64_t*>(large_base + page * GuestPage) != 1)
            values_valid = false;
    Check(values_valid, "all 256 guest stores reached owned RAM");
    Check(whole_read_proved && whole_write_proved && large_code_queries >= LargeSize / GuestPage,
        "large view proves full data interval and checks every negative executable bit");
    Check(large_data_queries < 1024 && large_code_queries < 512,
        "ActiveRun reuses the proved uniform view across page faults");
    Context next_root;
    Check(Run(next_root, code_base + 320, data_base + host_page) == Ready && next_root.rax == 1,
        "a later root chooses the identical uniform extent already mapped by the core");

    const auto uniform_rights = large_rights;
    auto growing_rights = uniform_rights;
    for (std::size_t page = growing_rights.size() / 2; page < growing_rights.size(); ++page)
        growing_rights[page] = 0;
    ChangeRights(growing_rights);
    Context old_half;
    Check(Run(old_half, code_base + 320, data_base + host_page) == Ready && old_half.rax == 1,
        "the initial RW half admits a 512 KiB view beside a logical hole");
    const auto additive = LumenAndroidFexCoreBeginGuestMemoryMutationV2(
        LUMEN_ANDROID_FEXCORE_GUEST_MEMORY_MUTATION_ADDITIVE_MAPPING_V2);
    if (additive == 0) return 2;
    large_rights = uniform_rights;
    Check(LumenAndroidFexCoreEndGuestMemoryMutationV2(additive,
        large_base + LargeSize / 2, LargeSize / 2, 1, nullptr, nullptr) == 1,
        "adding the disjoint RW half publishes under an actual additive lease");
    Context grown_view;
    Check(Run(grown_view, code_base + 960, data_base + host_page) == Ready &&
        grown_view.valid && grown_view.rax == 1,
        "a dyadic proof that grows to 1 MiB after an additive mapping preserves old and new accesses");
    ChangeRights(uniform_rights);

    auto late_executable = large_rights;
    late_executable[192] = Read | Write | Execute;
    ChangeRights(late_executable);
    large_data_queries = large_code_queries = 0;
    Context overlapping_candidates;
    Check(Run(overlapping_candidates, code_base + 320, data_base + host_page) == Ready &&
        overlapping_candidates.valid && overlapping_candidates.rax == 1,
        "a late executable page rejects the larger RW span while the smaller RW span remains usable");
    // The 1 MiB candidate reaches page 192 before its EXEC mismatch. Its
    // first 128 pages are then revisited for the accepted 512 KiB candidate.
    // Each unique page needs one code and two data proofs; four extra data
    // proofs cover the two complete candidate intervals. Without reuse this
    // layout requires 322 code and 650 data callbacks.
    Check(large_code_queries == 193 && large_data_queries == 390,
        "overlapping dyadic candidates reuse only their current query's complete page proofs");
    std::fprintf(stderr, "QEMU_PROVIDER_PERMISSION_PROBES_V1 unique_pages=193 "
        "code_queries=%u data_queries=%u code_control_expected=322 "
        "data_control_expected=650 elapsed_speed=unmeasured\n",
        large_code_queries, large_data_queries);
    Context denied_after_retry {.expected_fault = true};
    Check(Run(denied_after_retry, code_base + 416, data_base + host_page) == GuestFault &&
        denied_after_retry.fault_signal == SIGSEGV &&
        denied_after_retry.fault_address == large_base + 12 * GuestPage,
        "rejecting a larger mixed span cannot grant EXEC to its accepted smaller RW span");

    auto mixed = large_rights;
    mixed[192] = Read | Write;
    mixed[4] = Read | Execute;
    mixed[8] = 0;
    ChangeRights(mixed);
    Context read_rx;
    Check(Run(read_rx, code_base + 320, data_base + host_page) == Ready && read_rx.rax == 1,
        "mixed RX page remains readable without widening neighboring rights");
    Context adjacent;
    Check(Run(adjacent, code_base + 512, data_base + host_page) == Ready && adjacent.valid &&
        *reinterpret_cast<const std::uint64_t*>(large_base + 5 * GuestPage) == 2,
        "adjacent RW read and write remain valid after the RX page was mapped");
    Context write_rx {.expected_fault = true};
    Check(Run(write_rx, code_base + 352, data_base + host_page) == GuestFault &&
        write_rx.fault_signal == SIGSEGV && write_rx.fault_address == large_base + 4 * GuestPage &&
        *reinterpret_cast<const std::uint64_t*>(large_base + 4 * GuestPage) == 1,
        "mixed RX write is denied and backing RAM is unchanged");
    Context hole {.expected_fault = true};
    Check(Run(hole, code_base + 384, data_base + host_page) == GuestFault &&
        hole.fault_address == large_base + 8 * GuestPage,
        "sparse logical hole remains denied despite readable host allocation");
    Context execute_rw {.expected_fault = true};
    Check(Run(execute_rw, code_base + 416, data_base + host_page) == GuestFault &&
        execute_rw.fault_address == large_base + 12 * GuestPage,
        "non-executable RW page is denied on a real guest jump");

    std::array<std::uint8_t, LargeSize / GuestPage> fragmented;
    for (std::size_t page = 0; page < fragmented.size(); ++page)
        fragmented[page] = Read | (page % 2 != 0 ? Execute : 0);
    ChangeRights(fragmented);
    Context cache_capacity;
    Check(Run(cache_capacity, code_base + 576, data_base + host_page) == Ready &&
        cache_capacity.valid && cache_capacity.rax == 1,
        "more than 256 disjoint views reset the bounded cache and recover identical core extents");

    std::array<std::uint8_t, LargeSize / GuestPage> uniform;
    uniform.fill(Read | Write);
    ChangeRights(uniform);
    Context revoke {.expected_fault = true, .revoke_write = true,
        .revoke_address = large_base + 16 * GuestPage};
    Check(Run(revoke, code_base + 448, data_base + host_page) == GuestFault &&
        revoke.valid && revoke.calls == 1 && revoke.fault_signal == SIGSEGV &&
        revoke.fault_address == revoke.revoke_address &&
        *reinterpret_cast<const std::uint64_t*>(revoke.revoke_address) == 1,
        "HLE writer revocation clears the suspended root cache before resuming");
    Check(std::fegetround() == FE_DOWNWARD, "mapping and writer faults preserve native FP");

    const auto empty_mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (empty_mutation == 0) return 2;
    bool empty_release_called = false;
    Check(LumenAndroidFexCoreEndGuestMemoryMutationV2(empty_mutation, 0, 0, 1,
        +[](void* opaque) noexcept { *static_cast<bool*>(opaque) = true; },
        &empty_release_called) == 1 && empty_release_called,
        "committed zero-range mutation closes and releases its local gate without invalidation");
    Context after_empty;
    Check(Run(after_empty, code_base + 320, data_base + host_page) == Ready && after_empty.rax == 1,
        "CPU remains usable after the committed zero-range closure");

    const auto mutation = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
    if (mutation == 0) return 2;
    LumenAndroidFexCoreEndGuestMemoryMutationV1(mutation, code_base, code_size, 1);
    munmap(code, code_size); munmap(data, data_size);
    munmap(large_allocation, 2 * LargeSize);
    std::fesetenv(&original);
    alarm(0);
    if (failures != 0) return 1;
    std::puts("QemuProviderTest: PASS HLE/nested/fresh callbacks/native JIT cold-warm scope/native FP/boundary/fair CPU roots/quantum GP-SIMD-x87/mutation/RW batching/mixed rights/hole/revocation/ABA/pre-entry denial");
    return 0;
}
