// Lumen modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_PS5_GUEST_EXECUTION_H_
#define LUMEN_PS5_GUEST_EXECUTION_H_

#include "AndroidFexCoreRuntime.h"

#include <cstddef>
#include <cstdint>

namespace Loader::GuestExecution {
struct ThreadExitSignal {};
void ResetExitedThreadState() noexcept;

using MemoryRead = bool (*)(void* opaque, std::uint64_t address, void* destination,
                           std::size_t size) noexcept;
using MemoryWrite = bool (*)(void* opaque, std::uint64_t address, const void* source,
                            std::size_t size) noexcept;

struct RunRequest {
    LumenAndroidFexCoreGuestStateV1 state {
        sizeof(LumenAndroidFexCoreGuestStateV1), 1, 0, {}, 0, 0, 0, 0, 0,
    };
    LumenAndroidFexCoreHostCallV2 host_call {};
    LumenAndroidFexCoreExecutableQueryV1 executable_query {};
    void* opaque {};
    MemoryRead read_memory {};
    MemoryWrite write_memory {};
    const std::uint64_t* stack_arguments {};
    std::size_t stack_argument_count {};
    // Usable stack owned by a root invocation; nested callbacks may use a
    // different live guest stack and validate their frame through the VM.
    std::uint64_t stack_base {};
    std::uint64_t stack_size {};
};

struct RunResult {
    std::uint32_t code {LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1};
    LumenAndroidFexCoreGuestStateV1 state {};

    // A successful explicit HLE stop is distinct from the guest's root RET.
    bool completed {};
    bool stopped {};
};

// The guest VM validates the entry, stack and stack arguments before execution.
RunResult Run(const RunRequest& request) noexcept;

// Full engine adapter. These functions are linked only into the PS5 engine,
// not the standalone CPU bridge or Android's PS4 runtime.
bool InitializeRuntime() noexcept;
bool PrepareRuntime() noexcept;
bool IsGuestExecutable(std::uint64_t address) noexcept;
bool Invoke(std::uint64_t target, const std::uint64_t* arguments,
            std::size_t count, std::uint64_t* result,
            std::uint64_t stack_top = 0) noexcept;
void RequestThreadExit(std::uint64_t value) noexcept;
bool ThreadExitRequested() noexcept;
bool CaptureCurrentGuestState(LumenAndroidFexCoreGuestStateV1* state) noexcept;

enum RuntimeCapability : std::uint32_t {
    EntryAndModuleCalls = 1U << 0,
    GuestTls = 1U << 1,
    TypedHostImports = 1U << 2,
    GuestCallbacks = 1U << 3,
    GuestVirtualMemory = 1U << 4,
};
inline constexpr std::uint32_t RequiredRuntimeCapabilities =
    EntryAndModuleCalls | GuestTls | TypedHostImports | GuestCallbacks |
    GuestVirtualMemory;
// Direct Gen4 supplies translator signal recovery. PS5 guest exception delivery
// is a separate embedding capability and is not qualified by that recovery.

// One-time publication by a complete PS5 embedding, separate from a synthetic
// CPU probe. No capabilities are published by Run or by a passing smoke test.
bool PublishRuntimeCapabilities(std::uint32_t capabilities) noexcept;
bool FullRuntimeReady() noexcept;
const char* FullRuntimeLimitation() noexcept;

} // namespace Loader::GuestExecution

#endif
