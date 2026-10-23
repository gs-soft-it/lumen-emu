// Lumen modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_PS5_GUEST_EXECUTION_H_
#define LUMEN_PS5_GUEST_EXECUTION_H_

#include "AndroidFexCoreRuntime.h"

#include <cstddef>
#include <cstdint>

namespace Loader::GuestExecution {

using MemoryRead = bool (*)(void* opaque, std::uint64_t address, void* destination,
                           std::size_t size) noexcept;
using MemoryWrite = bool (*)(void* opaque, std::uint64_t address, const void* source,
                            std::size_t size) noexcept;
using CallbackStackPrepare = bool (*)(void* opaque, std::uint64_t rsp,
    std::uint64_t root_return, const std::uint64_t* spills, std::size_t spill_count) noexcept;

struct RunRequest {
    LumenAndroidFexCoreGuestStateV1 state {
        sizeof(LumenAndroidFexCoreGuestStateV1), 1, 0, {}, 0, 0, 0, 0, 0,
    };
    LumenAndroidFexCoreHostCallV2 host_call {};
    LumenAndroidFexCoreExecutableQueryV1 executable_query {};
    void* opaque {};
    MemoryRead read_memory {};
    MemoryWrite write_memory {};
    // Optional embedding-owned writer for callback stack data under an HLE
    // reader. Must prove ownership/rights for the whole frame before copying.
    CallbackStackPrepare prepare_callback_stack {};
    const std::uint64_t* stack_arguments {};
    std::size_t stack_argument_count {};
    // Set only at the lexical outer Invoke boundary, before any FEX lease.
    bool native_root_entry {};
    // Audited QEMU handoff detached the complete outer HLE frame. Stack/TLS
    // preparation uses ordinary native-root admission, with no borrowed reader.
    bool fresh_callback_entry {};
};

struct RunResult {
    std::uint32_t code {LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1};
    LumenAndroidFexCoreGuestStateV1 state {};
#if defined(__APPLE__)
    // Exact translator checkpoint retained for diagnostics. Run remains a
    // fresh-root API; this snapshot does not authorize arbitrary sigreturn.
    LumenAndroidFexCoreGuestStateV3 checkpoint {};
#endif
    // A successful explicit HLE stop is distinct from the guest's root RET.
    bool completed {};
    bool stopped {};
};

// The embedding VM owns every admitted range through this synchronous call.
// RSP is the guest function's entry stack pointer (RSP % 16 == 8). The return
// slot at RSP and Darwin's 16 scratch bytes below it must be readable/writable.
// Memory callbacks validate logical rights before copying; they must not infer
// permission from an arbitrary mapped native address. Darwin additionally
// requires its process-wide FEX data-access query registered by the VM owner.
RunResult Run(const RunRequest& request) noexcept;

// Full engine adapter. These functions are linked only into the PS5 engine,
// not the standalone CPU bridge or Android's PS4 runtime.
bool InitializeRuntime() noexcept;
bool PrepareRuntime() noexcept;
bool IsGuestExecutable(std::uint64_t address) noexcept;
bool Invoke(std::uint64_t target, const std::uint64_t* arguments,
            std::size_t count, std::uint64_t* result,
            std::uint64_t stack_top = 0) noexcept;
// Caller retains only independently live native snapshots/scalars, without
// guest pointers/VM leases/local locks. QEMU detaches and restores the outer
// frame with revalidation; FEX retains its existing nested-root contract.
bool InvokeWithoutGuestBorrow(std::uint64_t target, const std::uint64_t* arguments,
            std::size_t count, std::uint64_t* result) noexcept;
void RequestThreadExit(std::uint64_t value) noexcept;
bool CaptureCurrentGuestState(LumenAndroidFexCoreGuestStateV1* state) noexcept;
bool RuntimeSelfTest() noexcept;
bool MemoryAccessSelfTest() noexcept;
bool GpuBackingSelfTest() noexcept;

enum RuntimeCapability : std::uint32_t {
    EntryAndModuleCalls = 1U << 0,
    GuestTls = 1U << 1,
    TypedHostImports = 1U << 2,
    GuestCallbacks = 1U << 3,
    GuestVirtualMemory = 1U << 4,
    GuestExceptions = 1U << 5,
};
inline constexpr std::uint32_t RequiredRuntimeCapabilities =
    EntryAndModuleCalls | GuestTls | TypedHostImports | GuestCallbacks |
    GuestVirtualMemory
#if !defined(LUMEN_PS5_DIRECT_FEX)
    | GuestExceptions
#endif
    ;
// Direct Gen4 supplies translator signal recovery. PS5 guest exception delivery
// is a separate embedding capability and is not qualified by that recovery.

// One-time publication by a complete PS5 embedding, separate from a synthetic
// CPU probe. No capabilities are published by Run or by a passing smoke test.
bool PublishRuntimeCapabilities(std::uint32_t capabilities) noexcept;
bool FullRuntimeReady() noexcept;
const char* FullRuntimeLimitation() noexcept;

} // namespace Loader::GuestExecution

#endif
