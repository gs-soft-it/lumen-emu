// SPDX-License-Identifier: GPL-2.0-only
#include "common/hostException.h"
#include "DirectFexRuntime.h"

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {
namespace Direct = Lumen::Ps5::DirectFex;
using Common::HostException::AccessViolationType;
constexpr std::size_t Page = 4096;
void* watched = MAP_FAILED;
AccessViolationType expected_access = AccessViolationType::Unknown;
std::uintptr_t expected_native_pc {};
volatile sig_atomic_t faults {};
volatile sig_atomic_t valid_faults {};

extern "C" __attribute__((naked)) std::uint64_t ReadProtected(void*) {
    asm volatile("ldr x0, [x0]\nret\n");
}

extern "C" __attribute__((naked)) void WriteProtected(void*, std::uint64_t) {
    asm volatile("str x1, [x0]\nret\n");
}

bool RepairFault(const Common::HostException::ExceptionInfo& info) {
    faults = faults + 1;
    if (info.type != Common::HostException::ExceptionType::AccessViolation ||
        info.access_violation_type != expected_access ||
        info.access_violation_vaddr != reinterpret_cast<std::uintptr_t>(watched) ||
        info.exception_address == 0 ||
        (expected_native_pc != 0 && info.exception_address != expected_native_pc)) return false;
    valid_faults = valid_faults + 1;
    const auto permission = expected_access == AccessViolationType::Write
        ? PROT_READ | PROT_WRITE : PROT_READ;
    // Repair the actual mapping and retry the faulting instruction. This fixture
    // tests signal routing, not renderer cache publication or GPU readback.
    return ::mprotect(watched, Page, permission) == 0;
}

bool Protect(AccessViolationType access, std::uintptr_t native_pc = 0) {
    expected_access = access;
    expected_native_pc = native_pc;
    faults = 0;
    valid_faults = 0;
    return ::mprotect(watched, Page,
        access == AccessViolationType::Write ? PROT_READ : PROT_NONE) == 0;
}

struct GuestFixture {
    std::uintptr_t code {};
    std::uintptr_t stack {};
};

std::uint32_t QueryCode(void* opaque, std::uint64_t address, std::uint64_t* base,
                       std::uint64_t* size, std::uint32_t* writable) noexcept {
    const auto& fixture = *static_cast<const GuestFixture*>(opaque);
    if (address < fixture.code || address - fixture.code >= Page) return 0;
    *base = fixture.code;
    *size = Page;
    *writable = 0;
    return 1;
}

std::uint32_t UnexpectedHostCall(void*, std::uint64_t, std::uint64_t*,
                                LumenAndroidFexCoreSimdViewV2*, std::uint64_t*) noexcept {
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

bool ExecuteGuest(GuestFixture& fixture, std::size_t offset, std::uint64_t expected) {
    LumenAndroidFexCoreGuestStateV1 state {sizeof(state), 1, fixture.code + offset};
    state.gprs[4] = fixture.stack + Page - 24;
    const auto sentinel = Direct::RootReturn();
    std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &sentinel, sizeof(sentinel));
    const auto status = Direct::RunGuestV2(&state, UnexpectedHostCall, QueryCode, &fixture);
    return status == 0 && state.rip == sentinel && state.gprs[0] == expected &&
        faults == 1 && valid_faults == 1;
}
} // namespace

int main() {
    if (::sysconf(_SC_PAGESIZE) != Page) return 1;
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (Direct::Probe(&status) != 0 ||
        !Common::HostException::InitializeThreadSignalStack() ||
        !Common::HostException::InstallHandler(RepairFault)) return 2;

    watched = ::mmap(nullptr, Page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (watched == MAP_FAILED) return 3;
    constexpr std::uint64_t Initial = 0x1122334455667788ULL;
    constexpr std::uint64_t Written = 0x8877665544332211ULL;
    *static_cast<std::uint64_t*>(watched) = Initial;
    if (!Protect(AccessViolationType::Read, reinterpret_cast<std::uintptr_t>(&ReadProtected))) return 4;
    const bool native_read = ReadProtected(watched) == Initial && faults == 1 && valid_faults == 1;
    if (!Protect(AccessViolationType::Write, reinterpret_cast<std::uintptr_t>(&WriteProtected))) return 5;
    WriteProtected(watched, Written);
    const bool native_write = *static_cast<std::uint64_t*>(watched) == Written &&
        faults == 1 && valid_faults == 1;

    void* code = ::mmap(nullptr, Page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* stack = ::mmap(nullptr, Page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED || stack == MAP_FAILED) return 6;
    const auto address = reinterpret_cast<std::uint64_t>(watched);
    // movabs rax,watched; mov rax,[rax]; ret
    std::array<unsigned char, 14> reader {0x48,0xb8,0,0,0,0,0,0,0,0,0x48,0x8b,0x00,0xc3};
    std::memcpy(reader.data() + 2, &address, sizeof(address));
    // movabs rax,watched; movabs rcx,Initial; mov [rax],rcx; mov rax,rcx; ret
    std::array<unsigned char, 27> writer {
        0x48,0xb8,0,0,0,0,0,0,0,0,0x48,0xb9,0,0,0,0,0,0,0,0,
        0x48,0x89,0x08,0x48,0x89,0xc8,0xc3};
    std::memcpy(writer.data() + 2, &address, sizeof(address));
    std::memcpy(writer.data() + 12, &Initial, sizeof(Initial));
    std::memcpy(code, reader.data(), reader.size());
    std::memcpy(static_cast<unsigned char*>(code) + 128, writer.data(), writer.size());
    if (::mprotect(code, Page, PROT_READ) != 0) return 7;
    GuestFixture fixture {reinterpret_cast<std::uintptr_t>(code), reinterpret_cast<std::uintptr_t>(stack)};
    if (!Protect(AccessViolationType::Read)) return 8;
    const bool guest_read = ExecuteGuest(fixture, 0, Written);
    if (!Protect(AccessViolationType::Write)) return 9;
    const bool guest_write = ExecuteGuest(fixture, 128, Initial) &&
        *static_cast<const std::uint64_t*>(watched) == Initial;

    std::printf("PS5_DIRECT_HOST_FAULT_V1 native_read=%u native_write=%u guest_read=%u "
                "guest_write=%u repaired_mappings=1 renderer_readback_qualified=0\n",
                native_read, native_write, guest_read, guest_write);
    if (!Direct::BeginCodeMutation()) return 10;
    const bool released = ::munmap(code, Page) == 0 && ::munmap(stack, Page) == 0 &&
        ::munmap(watched, Page) == 0;
    const bool invalidated = Direct::EndCodeMutation(fixture.code, Page, true);
    return native_read && native_write && guest_read && guest_write && released && invalidated ? 0 : 11;
}
