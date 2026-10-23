// SPDX-License-Identifier: GPL-2.0-only
#include "common/hostException.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

namespace {
void* mapping {MAP_FAILED};
std::size_t page {};
volatile sig_atomic_t observed {};
volatile sig_atomic_t valid {};

extern "C" __attribute__((naked)) void NativeWrite(void*) {
    asm volatile("mov x9, #0x1234\n"
                 "str x9, [x0]\n"
                 "ret\n");
}

bool Handle(const Common::HostException::ExceptionInfo& info) {
    using namespace Common::HostException;
    observed = observed + 1;
    valid = info.context_architecture == ContextArchitecture::NativeArm64 &&
        info.type == ExceptionType::AccessViolation &&
        info.access_violation_type == AccessViolationType::Write &&
        info.access_violation_vaddr == reinterpret_cast<std::uint64_t>(mapping) &&
        info.exception_address == reinterpret_cast<std::uint64_t>(&NativeWrite) + 4 &&
        info.arm64.pc == info.exception_address && info.arm64.sp != 0 &&
        info.arm64.x[0] == reinterpret_cast<std::uint64_t>(mapping) &&
        info.arm64.x[9] == 0x1234 && info.arm64.esr_valid &&
        info.rax == 0 && info.rsp == 0;
    // A real mapping-protection repair permits the native faulting STR to
    // retry. It does not construct or execute any x86 guest register state.
    return valid && mprotect(mapping, page, PROT_READ | PROT_WRITE) == 0;
}
} // namespace

int main() {
    page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    mapping = mmap(nullptr, page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED || !Common::HostException::InitializeThreadSignalStack() ||
        !Common::HostException::InstallHandler(Handle)) return 1;
    NativeWrite(mapping);
    const bool ok = observed == 1 && valid == 1 &&
        *static_cast<const std::uint64_t*>(mapping) == 0x1234;
    std::printf("LUMEN_PS5_NATIVE_ARM_FAULT_V1 passed=%u observed=%d "
        "nativeArmContext=%d guestContextPublished=0\n", ok, observed, valid);
    munmap(mapping, page);
    return ok ? 0 : 1;
}
