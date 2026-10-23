// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/guestExecution.h"

#if defined(LUMEN_PS5_FEXCORE)
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "loader/hostCall.h"
#include "loader/runtimeLinker.h"
#if defined(LUMEN_PS5_DIRECT_FEX)
#include "DirectFexRuntime.h"
#endif


#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>

namespace Libs::LibKernel {
void KernelDispatchPendingSignalForCurrentThread();
}

namespace Loader::GuestExecution {
namespace {
std::atomic<bool> initialized {};
thread_local const std::uint64_t* active_gprs {};
thread_local std::uint64_t active_target {};
thread_local bool thread_exit_requested {};
thread_local std::uint64_t thread_exit_value {};
thread_local unsigned invocation_depth {};

std::uint32_t ExecutableQuery(void* opaque, std::uint64_t address, std::uint64_t* base,
    std::uint64_t* size, std::uint32_t* writable) noexcept {
    if (Libs::LibKernel::Memory::QueryGuestExecutable(opaque, address, base, size, writable) == 1)
        return 1;
    if (Lumen::Ps5::DirectFex::QueryOwnedExecutable(address, base, size)) {
        *writable = 0;
        return 1;
    }
    return 0;
}

std::uint32_t RuntimeHostCall(void*, std::uint64_t target, std::uint64_t* gprs,
    LumenAndroidFexCoreSimdViewV2* simd, std::uint64_t* next_rip) noexcept {
    if (gprs == nullptr || next_rip == nullptr) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    std::uint64_t return_address {};
    // Validate the CALL frame before allowing native service effects.
    if (gprs[4] > UINT64_MAX - 8 ||
        !Libs::LibKernel::Memory::ReadGuestMemory(nullptr, gprs[4], &return_address, 8))
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    const auto* parent = active_gprs;
    const auto parent_target = active_target;
    active_gprs = gprs;
    active_target = return_address;
    bool dispatched = true;
    try {
        Libs::LibKernel::PthreadCheckAsyncCancellation();
        Libs::LibKernel::KernelDispatchPendingSignalForCurrentThread();
        dispatched = DispatchHostCall(target, gprs, simd,
            Libs::LibKernel::Memory::ReadGuestMemory, nullptr);
        Libs::LibKernel::PthreadCheckAsyncCancellation();
    } catch (const ThreadExitSignal&) {
        // Return through the current translated CALL before disposing its native root.
    }
    active_gprs = parent;
    active_target = parent_target;
    if (!dispatched) {
        std::fprintf(stderr, "PS5_HLE_REJECTED target=%#llx rsp=%#llx\n",
            static_cast<unsigned long long>(target), static_cast<unsigned long long>(gprs[4]));
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    }
    if (thread_exit_requested) return LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1;
    gprs[4] += 8;
    *next_rip = return_address;
    return LUMEN_ANDROID_FEXCORE_HOST_CALL_CONTINUE_V1;
}

RunRequest Request(std::uint64_t target, std::uint64_t rsp) noexcept {
    RunRequest request {};
    request.state.rip = target;
    request.state.gprs[4] = rsp;
    // FS addresses the original PS5 thread control block.
    request.state.fsBase = RuntimeLinker::CurrentThreadTlsBase();
    request.host_call = RuntimeHostCall;
    request.executable_query = ExecutableQuery;
    request.read_memory = Libs::LibKernel::Memory::ReadGuestMemory;
    request.write_memory = Libs::LibKernel::Memory::WriteGuestMemory;
    return request;
}
} // namespace

bool InitializeRuntime() noexcept {
    if (initialized.load(std::memory_order_acquire)) return true;
    LumenAndroidFexCoreRuntimeStatusV1 status {};
#if defined(LUMEN_PS5_DIRECT_FEX)
    if (Lumen::Ps5::DirectFex::Probe(&status) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1)
#else
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1)
#endif
        return false;
    initialized.store(true, std::memory_order_release);
    return true;
}

bool PrepareRuntime() noexcept {
    return InitializeRuntime() && HostCallRegistryReady() &&
        PublishRuntimeCapabilities(RequiredRuntimeCapabilities);
}

bool IsGuestExecutable(std::uint64_t address) noexcept {
    std::uint64_t base {}, size {};
    std::uint32_t writable {};
    return ExecutableQuery(nullptr, address, &base, &size, &writable) == 1;
}

void ResetExitedThreadState() noexcept {
    active_gprs = nullptr;
    active_target = 0;
    invocation_depth = 0;
    thread_exit_requested = false;
}

void RequestThreadExit(std::uint64_t value) noexcept {
    thread_exit_value = value;
    thread_exit_requested = true;
}

bool ThreadExitRequested() noexcept { return thread_exit_requested; }

bool CaptureCurrentGuestState(LumenAndroidFexCoreGuestStateV1* state) noexcept {
    if (state == nullptr || active_gprs == nullptr) return false;
    *state = {sizeof(*state), 1, active_target, {},
        RuntimeLinker::CurrentThreadTlsBase(), 0, 0, 0, 0};
    std::memcpy(state->gprs, active_gprs, sizeof(state->gprs));
    // This is a CALL-boundary observation for queued signals, not the ARM64
    // HLE address or a suspended-instruction resume checkpoint.
    state->gprs[4] += 8;
    return true;
}

bool Invoke(std::uint64_t target, const std::uint64_t* arguments, std::size_t count,
    std::uint64_t* result, std::uint64_t stack_top) noexcept {
    if (!initialized.load(std::memory_order_acquire) || target == 0 || count > 32 ||
        (count != 0 && arguments == nullptr) || result == nullptr) return false;
    struct Depth {
        Depth() { ++invocation_depth; }
        ~Depth() { if (--invocation_depth == 0) thread_exit_requested = false; }
    } depth;
    std::uint64_t stack_base {}, stack_size {};
    if (active_gprs == nullptr &&
        (!Libs::LibKernel::PthreadGetGuestStack(Libs::LibKernel::PthreadSelf(),
             &stack_base, &stack_size) || stack_base == 0 || stack_size == 0 ||
         stack_size > UINT64_MAX - stack_base)) return false;
    if (stack_top == 0) {
        if (active_gprs != nullptr) {
            // Keep the live parent's red zone and CALL frame intact.
            if (active_gprs[4] < 256) return false;
            stack_top = active_gprs[4] - 256;
        } else {
            stack_top = stack_base + stack_size;
        }
    }
    const auto spill_count = count > 6 ? count - 6 : 0;
    const auto frame_size = (spill_count + 1) * 8;
    stack_top &= ~std::uint64_t {15};
    if (stack_top < frame_size + 32) return false;
    const auto rsp = ((stack_top - frame_size - 8) & ~std::uint64_t {15}) + 8;
    // A root may deliberately start below the top (main-entry arguments), but
    // its CALL frame must stay inside this thread's usable stack.
    if (stack_size != 0 && (rsp < stack_base || rsp - stack_base < 16 ||
        rsp - stack_base > stack_size || frame_size > stack_size - (rsp - stack_base)))
        return false;
    auto request = Request(target, rsp);
    request.stack_base = stack_base;
    request.stack_size = stack_size;
    constexpr std::array<unsigned, 6> registers {7, 6, 2, 1, 8, 9};
    for (std::size_t i = 0; i < count && i < registers.size(); ++i)
        request.state.gprs[registers[i]] = arguments[i];
    request.stack_arguments = spill_count != 0 ? arguments + 6 : nullptr;
    request.stack_argument_count = spill_count;
    const auto run = Run(request);
    if (run.stopped && thread_exit_requested && run.code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) {
        *result = thread_exit_value;
        return true;
    }
    if (!run.completed) {
        std::fprintf(stderr, "PS5_GUEST_STOP code=%u rip=%#llx rsp=%#llx signal=%u fault=%#llx\n",
            run.code, static_cast<unsigned long long>(run.state.rip),
            static_cast<unsigned long long>(run.state.gprs[4]), run.state.faultSignal,
            static_cast<unsigned long long>(run.state.faultAddress));
        return false;
    }
    *result = run.state.gprs[0];
    return true;
}


} // namespace Loader::GuestExecution
#endif
