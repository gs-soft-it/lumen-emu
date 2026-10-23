// Lumen, 2026-10-05. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidFexCoreRuntime.h"
#include "../../source/cpu/runtime/MemoryHandoff.h"

#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

namespace {
// The native runtime allocator exposes this stable guest arena. A single mmap
// hint reserves only fresh fixture pages; never replace any existing mapping.
constexpr std::uint64_t CodeAddress = UINT64_C(0x7000000000);
constexpr std::size_t NestedOffset = 256;
constexpr std::uint32_t Iterations = 4096;
constexpr std::uint64_t NestedResult = 73;
constexpr std::uint64_t HostAddition = 17;

bool Check(bool condition, const char* detail) {
    if (!condition) std::fprintf(stderr, "FexTranslationCacheTest: %s\n", detail);
    return condition;
}


struct Fixture {
    std::size_t page {};
    void* code {MAP_FAILED};
    void* data {MAP_FAILED};
    bool executable {};
    bool uncacheableRelocation {};
    bool denyHandoff {};
    std::uint32_t value {};
    unsigned callbacks {};
    unsigned nestedCalls {};
    unsigned handoffCalls {};
    unsigned handoffNestedCalls {};
    unsigned handoffNestedFaults {};
    bool callbackValid {true};

    std::uint64_t DataAddress() const { return CodeAddress + page; }
    std::uint64_t OuterStack() const { return DataAddress() + page; }
    std::uint64_t NestedStack() const { return DataAddress() + 2 * page; }
    std::uint64_t Output() const { return DataAddress() + 3 * page; }
    bool DataAllows(std::uint64_t address, std::uint64_t size) const {
        const auto first = DataAddress() + page;
        return data != MAP_FAILED && size != 0 && address >= first &&
            address - first < 4 * page && size <= 4 * page - (address - first);
    }
    bool StackAllows(std::uint64_t address, std::uint64_t size, bool nested) const {
        const auto first = nested ? NestedStack() : OuterStack();
        return size != 0 && address >= first && address - first < page &&
            size <= page - (address - first);
    }
    bool MapBeforeRuntime() {
        // Reserve fixture-owned ranges before the first runtime ABI call. No
        // translated reader or code cache exists yet; all later changes use
        // the production mutation barrier.
        const auto hostPage = sysconf(_SC_PAGESIZE);
        if (hostPage != 4096 && hostPage != 16384) return false;
        page = static_cast<std::size_t>(hostPage);
        code = mmap(reinterpret_cast<void*>(CodeAddress), 7 * page,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED || reinterpret_cast<std::uint64_t>(code) != CodeAddress) {
            std::fprintf(stderr, "FexTranslationCacheTest: mmap requested=0x%llx returned=0x%llx errno=%d\n",
                static_cast<unsigned long long>(CodeAddress),
                static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(code)),
                code == MAP_FAILED ? errno : 0);
            if (code != MAP_FAILED) munmap(code, 7 * page);
            code = MAP_FAILED;
            return false;
        }
        data = reinterpret_cast<void*>(DataAddress());
        bool ready = mprotect(data, page, PROT_NONE) == 0 &&
            mprotect(reinterpret_cast<void*>(DataAddress() + 5 * page), page, PROT_NONE) == 0;
        return ready;
    }
    bool Publish(std::uint32_t nextValue) {
        if (code == MAP_FAILED || nextValue < 42 || nextValue > 43) return false;
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (token == 0) return false;
        bool published = mprotect(code, page, PROT_READ | PROT_WRITE) == 0;
        executable = false;
        if (published) {
            // No host pointers are embedded in the x86 bytes. RDI supplies the
            // output buffer and R9 the registered HLE target on each fresh Run.
            std::array<std::uint8_t, 66> program {
                0xb8,0,0,0,0,                // mov eax,value
                0x66,0x0f,0x6e,0xc0,        // movd xmm0,eax
                0x66,0x0f,0x70,0xc0,0x00,   // pshufd xmm0,xmm0,0
                0x66,0x0f,0xef,0xc9,        // pxor xmm1,xmm1
                0x31,0xd2,                  // xor edx,edx
                0xb9,0,0,0,0,               // mov ecx,Iterations
                0x66,0x0f,0xfe,0xc8,        // loop: paddd xmm1,xmm0
                0x01,0xc2,                  // add edx,eax
                0xff,0xc9,                  // dec ecx
                0x75,0xf6,                  // jne loop
                0xf3,0x0f,0x7f,0x0f,        // movdqu [rdi],xmm1
                0x66,0x0f,0x7e,0xc8,        // movd eax,xmm1
                0x01,0xd0,                  // add eax,edx
                0x41,0xf6,0xc0,0x01,        // test r8b,1
                0x74,0x05,                  // jz even
                0x83,0xc0,0x07,             // add eax,7
                0xeb,0x03,                  // jmp invoke
                0x83,0xc0,0x03,             // even: add eax,3
                0x48,0x89,0xc6,             // invoke: mov rsi,rax (second SysV argument)
                0x41,0xff,0xd1,             // call r9
                0xc3,                       // ret
            };
            static_assert(program.size() < NestedOffset);
            std::memcpy(program.data() + 1, &nextValue, sizeof(nextValue));
            std::memcpy(program.data() + 21, &Iterations, sizeof(Iterations));
            std::memset(code, 0xcc, page);
            std::size_t offset = 0;
            if (uncacheableRelocation) {
                // This valid RIP-relative address points into owned RW data,
                // outside the RX file section. Optional cache metadata must
                // reject that relocation without rejecting guest execution.
                // RBX survives the ordinary host-call ABI; caller-scratch R11
                // is also used by the existing HLE call/return bridge.
                std::array<std::uint8_t, 7> relocation {0x48,0x8d,0x1d,0,0,0,0};
                const auto displacement = static_cast<std::int32_t>(Output() - (CodeAddress + relocation.size()));
                std::memcpy(relocation.data() + 3, &displacement, sizeof(displacement));
                std::memcpy(code, relocation.data(), relocation.size());
                offset = relocation.size();
            }
            std::memcpy(static_cast<std::uint8_t*>(code) + offset, program.data(), program.size());
            const std::array<std::uint8_t, 6> nested {0xb8,73,0,0,0,0xc3};
            std::memcpy(static_cast<std::uint8_t*>(code) + NestedOffset,
                nested.data(), nested.size());
            published = mprotect(code, page, PROT_READ | PROT_EXEC) == 0;
            executable = published;
            value = nextValue;
        }
        LumenAndroidFexCoreEndGuestMemoryMutationV1(token, CodeAddress, page, published);
        return published;
    }
    bool Revoke() {
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        if (token == 0) return false;
        const bool changed = mprotect(code, page, PROT_READ) == 0;
        if (changed) executable = false;
        LumenAndroidFexCoreEndGuestMemoryMutationV1(token, CodeAddress, page, changed);
        return changed;
    }
    bool Release() {
        bool released = true;
        if (code != MAP_FAILED) {
            const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (token == 0) return false;
            const bool unmapped = munmap(code, page) == 0;
            if (unmapped) { code = MAP_FAILED; executable = false; }
            LumenAndroidFexCoreEndGuestMemoryMutationV1(token, CodeAddress, page, unmapped);
            released = unmapped && released;
        }
        if (data != MAP_FAILED) {
            const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
            if (token == 0) return false;
            const bool unmapped = munmap(data, 6 * page) == 0;
            if (unmapped) data = MAP_FAILED;
            LumenAndroidFexCoreEndGuestMemoryMutationV1(token, DataAddress(), 6 * page, unmapped);
            released = unmapped && released;
        }
        return released;
    }
};

Fixture* activeFixture {};
void HostTarget() {}

std::uint32_t Executable(void*, std::uint64_t, std::uint64_t*,
    std::uint64_t*, std::uint32_t*) noexcept;
std::uint32_t NoHostCall(void*, std::uint64_t, std::uint64_t*,
    LumenAndroidFexCoreSimdViewV2*, std::uint64_t*) noexcept;

bool Handoff(std::uint64_t address, std::uint64_t bytes,
             Lumen::GuestCpu::MemoryAccessIntent intent) noexcept {
    if (activeFixture == nullptr) return false;
    auto& fixture = *activeFixture;
    if (address < fixture.Output() || bytes == 0 || address - fixture.Output() >= 16 ||
        bytes > 16 - (address - fixture.Output()) || !Lumen::GuestCpu::WritesMemory(intent)) return false;
    ++fixture.handoffCalls;
    // Reenter through the supported runtime API while the outer generated
    // code is suspended inside its shared callback. Each Run owns a separate
    // FEX frame; both a normal nested return and a nested signal must restore
    // the outer active frame without changing its published precise RIP.
    LumenAndroidFexCoreGuestStateV1 nested {};
    nested.structSize = sizeof(nested);
    nested.abiVersion = 1;
    nested.rip = CodeAddress + NestedOffset;
    nested.gprs[4] = fixture.NestedStack() + fixture.page - sizeof(std::uint64_t);
    const auto sentinel = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(nested.gprs[4]), &sentinel, sizeof(sentinel));
    const auto code = LumenAndroidFexCoreRunGuestV2(&nested, NoHostCall, Executable, &fixture);
    ++fixture.handoffNestedCalls;
    fixture.callbackValid &= code == 0 && nested.gprs[0] == NestedResult;
    nested = {};
    nested.structSize = sizeof(nested);
    nested.abiVersion = 1;
    constexpr auto TrapOffset = NestedOffset + 6; // Existing fixture-owned INT3 padding.
    nested.rip = CodeAddress + TrapOffset;
    nested.gprs[4] = fixture.NestedStack() + fixture.page - sizeof(std::uint64_t);
    const auto fault = LumenAndroidFexCoreRunGuestV2(&nested, NoHostCall, Executable, &fixture);
    ++fixture.handoffNestedFaults;
    fixture.callbackValid &= fault == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
        nested.faultSignal == SIGTRAP && nested.rip == CodeAddress + TrapOffset + 1;
    if (!fixture.callbackValid) std::fprintf(stderr,
        "handoff nested: ready=%u value=%llu fault=%u signal=%u rip=0x%llx\n",
        code, static_cast<unsigned long long>(nested.gprs[0]), fault, nested.faultSignal,
        static_cast<unsigned long long>(nested.rip));
    return fixture.callbackValid && !fixture.denyHandoff;
}

#if defined(__APPLE__)
std::uint32_t DataAccess(std::uint64_t address, std::uint64_t size,
    std::uint32_t write) noexcept {
    if (activeFixture == nullptr || write > 1 || size == 0) return 0;
    if (activeFixture->DataAllows(address, size)) return 1;
    return write == 0 && activeFixture->code != MAP_FAILED && address >= CodeAddress &&
        address - CodeAddress < activeFixture->page &&
        size <= activeFixture->page - (address - CodeAddress) ? 1U : 0U;
}
#endif

std::uint32_t Executable(void* opaque, std::uint64_t address,
    std::uint64_t* base, std::uint64_t* size, std::uint32_t* writable) noexcept {
    const auto& fixture = *static_cast<Fixture*>(opaque);
    if (!fixture.executable || address < CodeAddress || address - CodeAddress >= fixture.page)
        return 0;
    *base = CodeAddress;
    *size = fixture.page;
    *writable = 0;
    return 1;
}

std::uint32_t NoHostCall(void*, std::uint64_t, std::uint64_t*,
    LumenAndroidFexCoreSimdViewV2*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

std::uint32_t HostCall(void* opaque, std::uint64_t target,
    std::uint64_t* gprs, LumenAndroidFexCoreSimdViewV2* simd,
    std::uint64_t* next) noexcept {
    auto& fixture = *static_cast<Fixture*>(opaque);
    ++fixture.callbacks;
    fixture.callbackValid = fixture.callbackValid &&
        target == reinterpret_cast<std::uint64_t>(&HostTarget) &&
        fixture.StackAllows(gprs[4], sizeof(*next), false) && simd != nullptr &&
        simd->registerCount == 16 && simd->registerBytes == 16 &&
        simd->registerStride >= 16 && simd->registers != nullptr;
    if (!fixture.callbackValid) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    std::array<std::uint32_t, 4> lanes {};
    std::memcpy(lanes.data(), simd->registers + simd->registerStride, sizeof(lanes));
    for (const auto lane : lanes)
        fixture.callbackValid = fixture.callbackValid && lane == fixture.value * Iterations;
    LumenAndroidFexCoreGuestStateV1 nested {};
    nested.structSize = sizeof(nested);
    nested.abiVersion = 1;
    nested.rip = CodeAddress + NestedOffset;
    nested.gprs[4] = fixture.NestedStack() + fixture.page - sizeof(std::uint64_t);
    const auto sentinel = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(nested.gprs[4]), &sentinel, sizeof(sentinel));
    const auto nestedCode = LumenAndroidFexCoreRunGuestV2(
        &nested, NoHostCall, Executable, &fixture);
    ++fixture.nestedCalls;
    fixture.callbackValid = fixture.callbackValid && nestedCode == 0 &&
        nested.gprs[0] == NestedResult;
    // The checksum is a real SysV argument. Incoming RAX is bridge scratch
    // on Android, so reading it here would test an unsupported calling ABI.
    gprs[0] = gprs[6] + NestedResult + HostAddition;
    const char* returnSelector = std::getenv("LUMEN_FEX_HLE_RETURN_GUEST");
    if (returnSelector != nullptr && std::strcmp(returnSelector, "1") == 0)
        return fixture.callbackValid ? LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1 :
            LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    std::memcpy(next, reinterpret_cast<void*>(gprs[4]), sizeof(*next));
    gprs[4] += sizeof(*next);
    return fixture.callbackValid ? LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1 :
        LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

struct Result {
    std::uint32_t code {};
    std::uint64_t checksum {};
    std::uint64_t expected {};
    std::uint64_t elapsedNs {};
    std::uint64_t relocationTarget {};
    unsigned callbacks {};
    unsigned nestedCalls {};
    unsigned handoffCalls {};
    unsigned handoffNestedFaults {};
    bool valid {};
};

Result Run(Fixture& fixture, bool expectDenied = false) {
    fixture.callbacks = fixture.nestedCalls = fixture.handoffCalls = 0;
    fixture.handoffNestedCalls = fixture.handoffNestedFaults = 0;
    fixture.callbackValid = true;
    std::memset(reinterpret_cast<void*>(fixture.Output()), 0, 16);
    LumenAndroidFexCoreGuestStateV1 guest {};
    guest.structSize = sizeof(guest);
    guest.abiVersion = 1;
    guest.rip = CodeAddress;
    guest.gprs[4] = fixture.OuterStack() + fixture.page - sizeof(std::uint64_t);
    guest.gprs[7] = fixture.Output(); // RDI
    guest.gprs[8] = fixture.value;   // R8
    guest.gprs[9] = reinterpret_cast<std::uint64_t>(&HostTarget); // R9
    const auto sentinel = LumenAndroidFexCoreRootReturnAddressV1();
    std::memcpy(reinterpret_cast<void*>(guest.gprs[4]), &sentinel, sizeof(sentinel));
    const auto start = std::chrono::steady_clock::now();
    Result result;
    result.code = LumenAndroidFexCoreRunGuestV2(&guest, HostCall, Executable, &fixture);
    result.elapsedNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
    result.checksum = guest.gprs[0];
    result.relocationTarget = guest.gprs[3];
    result.callbacks = fixture.callbacks;
    result.nestedCalls = fixture.nestedCalls;
    result.handoffCalls = fixture.handoffCalls;
    result.handoffNestedFaults = fixture.handoffNestedFaults;
    result.expected = UINT64_C(2) * fixture.value * Iterations +
        (fixture.value & 1 ? 7 : 3) + NestedResult + HostAddition;
    if (expectDenied) {
        result.valid = result.code == LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1 &&
            fixture.callbacks == 0 && fixture.nestedCalls == 0;
    } else if (fixture.denyHandoff) {
        const auto storeRip = CodeAddress + 35 + (fixture.uncacheableRelocation ? 7 : 0);
        result.valid = result.code == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 &&
            guest.faultSignal == SIGTRAP && guest.rip == storeRip && fixture.callbackValid &&
            fixture.callbacks == 0 && fixture.handoffCalls == 1 && fixture.handoffNestedCalls == 1 &&
            fixture.handoffNestedFaults == 1;
        const std::array<std::uint8_t, 16> zero {};
        result.valid &= std::memcmp(reinterpret_cast<const void*>(fixture.Output()), zero.data(), zero.size()) == 0;
        if (!result.valid) std::fprintf(stderr,
            "outer denial: code=%u signal=%u rip=0x%llx expected=0x%llx valid=%u host=%u handoff=%u nested=%u faults=%u\n",
            result.code, guest.faultSignal, static_cast<unsigned long long>(guest.rip),
            static_cast<unsigned long long>(storeRip), fixture.callbackValid ? 1U : 0U,
            fixture.callbacks, fixture.handoffCalls, fixture.handoffNestedCalls, fixture.handoffNestedFaults);
    } else {
        std::array<std::uint32_t, 4> output {};
        std::memcpy(output.data(), reinterpret_cast<void*>(fixture.Output()), sizeof(output));
        result.valid = result.code == 0 && result.checksum == result.expected &&
            fixture.callbackValid && fixture.callbacks == 1 && fixture.nestedCalls == 1 && fixture.handoffCalls != 0 &&
            fixture.handoffNestedCalls == fixture.handoffCalls && fixture.handoffNestedFaults == fixture.handoffCalls;
        for (const auto lane : output) result.valid = result.valid && lane == fixture.value * Iterations;
        if (fixture.uncacheableRelocation) result.valid = result.valid && result.relocationTarget == fixture.Output();
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    unsigned value = 42;
    bool freshness = false;
    bool revoke = false;
    bool quickExit = false;
    bool waitCheckpoint = false;
    bool uncacheableRelocation = false;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--value") == 0 && index + 1 < argc) {
            const char* selected = argv[++index];
            if (std::strcmp(selected, "42") != 0 && std::strcmp(selected, "43") != 0) return 2;
            value = static_cast<unsigned>(std::atoi(selected));
        } else if (std::strcmp(argv[index], "--freshness") == 0) freshness = true;
        else if (std::strcmp(argv[index], "--revoke") == 0) revoke = true;
        else if (std::strcmp(argv[index], "--quick-exit") == 0) quickExit = true;
        else if (std::strcmp(argv[index], "--wait-checkpoint") == 0) waitCheckpoint = true;
        else if (std::strcmp(argv[index], "--uncacheable-relocation") == 0) uncacheableRelocation = true;
        else return 2;
    }
    if (freshness && value != 42) return 2;
    if ((quickExit && waitCheckpoint) || ((quickExit || waitCheckpoint) && (freshness || revoke))) return 2;
    Fixture fixture;
    fixture.uncacheableRelocation = uncacheableRelocation;
    if (!Check(fixture.MapBeforeRuntime(), "fixed-address mapping hint was not admitted before runtime")) return 2;
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    const auto admitted = LumenAndroidFexCoreRuntimeProbeV1(&status);
    if (admitted != 0) {
        std::printf("{\"schema\":\"LUMEN_FEX_TRANSLATION_CACHE_TEST_V1\",\"pass\":false,\"admission_code\":%u}\n", admitted);
        return 1;
    }
    activeFixture = &fixture;
    bool valid = true;
#if defined(__APPLE__)
    valid = Check(LumenMacOSFexCoreRegisterDataAccessQueryV1(DataAccess) == 1,
        "register bounded data proof");
#endif
    valid = valid && Check(LumenAndroidFexCoreRegisterHostCallTargetV1(
            reinterpret_cast<std::uint64_t>(&HostTarget)) == 1, "register HLE target") &&
        Check(fixture.Publish(value), "immutable RX publication");
    valid = valid && Check(Lumen::GuestCpu::RegisterMemoryHandoffQuery(Handoff), "register handoff oracle");
    if (valid) {
        const auto token = LumenAndroidFexCoreBeginGuestMemoryMutationV1();
        valid = Check(token != 0, "watch output under exclusive epoch");
        if (token != 0) {
            Lumen::GuestCpu::MarkHandoffRange(fixture.Output(), 16);
            valid = LumenAndroidFexCoreEndGuestMemoryMutationV2(token, 0, 0, 0, nullptr, nullptr) == 1 && valid;
        }
    }
    Result first, changed, denied;
    if (valid) valid = Check((first = Run(fixture)).valid, "scalar/SIMD/nested callback checksum");
    if (valid && freshness) valid = Check(fixture.Publish(43), "changed bytes publication") &&
        Check((changed = Run(fixture)).valid, "old native translation survived mutation");
    if (valid) {
        fixture.denyHandoff = true;
        valid = Check(Run(fixture).valid, "outer precise denial RIP survives nested normal and signal return");
        fixture.denyHandoff = false;
    }
    if (valid && revoke) valid = Check(fixture.Revoke(), "revoke execution") &&
        Check((denied = Run(fixture, true)).valid, "revoked execution did not fail closed");
    if (!quickExit && !waitCheckpoint && (fixture.code != MAP_FAILED || fixture.data != MAP_FAILED))
        valid = Check(fixture.Release(), "mapping release and invalidation") && valid;
    activeFixture = nullptr;
    std::printf("{\"schema\":\"LUMEN_FEX_TRANSLATION_CACHE_TEST_V1\",\"pass\":%s,"
        "\"admission_code\":%u,\"value\":%u,\"code_address\":\"0x%llx\","
        "\"checksum\":%llu,\"expected_checksum\":%llu,\"run_ns\":%llu,"
        "\"callbacks\":%u,\"nested_calls\":%u,\"handoff_calls\":%u,\"handoff_nested_faults\":%u,"
        "\"freshness\":%s,\"changed_checksum\":%llu,\"changed_expected_checksum\":%llu,"
        "\"revoked\":%s,\"revoked_code\":%u,\"exit_mode\":\"%s\","
        "\"uncacheable_relocation\":%s,\"relocation_target\":%llu}\n",
        valid ? "true" : "false", admitted, value,
        static_cast<unsigned long long>(CodeAddress),
        static_cast<unsigned long long>(first.checksum),
        static_cast<unsigned long long>(first.expected),
        static_cast<unsigned long long>(first.elapsedNs), first.callbacks, first.nestedCalls,
        first.handoffCalls, first.handoffNestedFaults,
        freshness ? "true" : "false",
        static_cast<unsigned long long>(changed.checksum),
        static_cast<unsigned long long>(changed.expected), revoke ? "true" : "false", denied.code,
        quickExit ? "quick_exit" : waitCheckpoint ? "checkpoint_abrupt" : "normal",
        uncacheableRelocation ? "true" : "false", static_cast<unsigned long long>(first.relocationTarget));
    std::fflush(stdout);
    if (valid && quickExit) {
        // Allow the asynchronous writer to complete this small workload; this
        // does not promise queue draining or persistence of unfinished records.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::quick_exit(0);
    }
    if (valid && waitCheckpoint) {
        // The periodic publication interval is 30 seconds. Exit without any
        // C++ destruction or quick-exit handler to prove completed publication.
        std::this_thread::sleep_for(std::chrono::seconds(31));
        std::_Exit(0);
    }
    return valid ? 0 : 1;
}
