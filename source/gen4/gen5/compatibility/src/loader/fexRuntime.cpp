// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/guestExecution.h"

#if defined(LUMEN_PS5_FEXCORE)
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "loader/hostCall.h"
#include "loader/runtimeLinker.h"
#include "loader/guestMutationScope.h"
#include "graphics/host_gpu/guestBackingPolicy.h"
#include "GuestCpuRuntime.h"
#if defined(LUMEN_PS5_DIRECT_FEX)
#include "DirectFexRuntime.h"
#endif
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
#include "QemuProvider.h"
#endif

#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <limits>

namespace Libs::LibKernel {
bool KernelDispatchGuestException(int signal,
    const LumenAndroidFexCoreGuestStateV1* state, std::uint64_t address);
}

namespace Loader::GuestExecution {
namespace {
std::atomic<bool> initialized {};
thread_local const std::uint64_t* active_gprs {};
thread_local std::uint64_t active_target {};
thread_local bool delivering_fault {};
thread_local bool thread_exit_requested {};
thread_local std::uint64_t thread_exit_value {};
thread_local unsigned invocation_depth {};
thread_local unsigned fresh_callback_depth {};
thread_local std::uint64_t fresh_callback_tls {};

std::uint32_t RuntimeHostCall(void*, std::uint64_t target, std::uint64_t* gprs,
    LumenAndroidFexCoreSimdViewV2* simd, std::uint64_t* next_rip) noexcept {
    if (gprs == nullptr || next_rip == nullptr) return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    std::uint64_t return_address {};
    // Validate the CALL frame before allowing native service effects.
    if (gprs[4] > UINT64_MAX - 8 ||
        !Libs::LibKernel::Memory::ReadGuestMemoryForHostCall(nullptr, gprs[4], &return_address, 8))
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    const auto* parent = active_gprs;
    const auto parent_target = active_target;
    active_gprs = gprs;
    active_target = return_address;
    const bool dispatched = DispatchHostCall(target, gprs, simd,
        Libs::LibKernel::Memory::ReadGuestMemoryForHostCall, nullptr);
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
    request.native_root_entry = active_gprs == nullptr && invocation_depth == 1;
    request.fresh_callback_entry = fresh_callback_depth != 0 &&
        fresh_callback_depth == invocation_depth;
    request.state.rip = target;
    request.state.gprs[4] = rsp;
    // This is a lexical root-call boundary, before Run owns any execution
    // lease. Nested callbacks retain their outer HLE reader; only their private
    // stack data may use the synchronous writer instead of a mapping epoch.
    request.state.fsBase = request.fresh_callback_entry ? fresh_callback_tls :
        RuntimeLinker::CurrentThreadTlsBase(request.native_root_entry);
    request.host_call = RuntimeHostCall;
    request.executable_query = Libs::LibKernel::Memory::QueryGuestExecutable;
    request.read_memory = Libs::LibKernel::Memory::ReadGuestMemory;
    request.write_memory = Libs::LibKernel::Memory::WriteGuestMemory;
    // Terminal fault delivery happens after RunGuest has released its HLE lease.
    if (active_gprs != nullptr && !delivering_fault && !request.fresh_callback_entry)
        request.prepare_callback_stack = Libs::LibKernel::Memory::PrepareGuestCallbackStack;
    return request;
}
} // namespace

bool InitializeRuntime() noexcept {
    if (!Libs::Graphics::GuestCpuGpuConcurrencyCanStart(
            Libs::Graphics::GuestAsyncCpuConcurrentExperimentRequested())) {
        std::fprintf(stderr, "PS5_CPU_GPU_CONCURRENCY_ADMISSION_DENIED reason=jit-hle-access-coverage-unavailable vm_pin_is_not_data_coherence=1\n");
        return false;
    }
    if (initialized.load(std::memory_order_acquire)) return true;
#if !defined(LUMEN_PS5_DIRECT_FEX)
    // This renderer publishes GPU writes under its exclusive epoch and does
    // not register the interval handoff ledger. Declare that contract before
    // the first probe can initialize FEX or load/emit native translations.
    if (Lumen::GuestCpu::Selected() == Lumen::GuestCpu::Provider::Fex &&
        LumenAndroidFexCoreConfigureMemoryHandoffV1(LUMEN_FEX_MEMORY_EXCLUSIVE_PUBLICATION_V1) != 1) {
        std::fprintf(stderr, "PS5_MEMORY_COHERENCE_CONFIGURATION_DENIED\n");
        return false;
    }
#endif
    if (!Libs::LibKernel::Memory::InitializeFexMemoryAccess()) return false;
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
    return Libs::LibKernel::Memory::QueryGuestExecutable(nullptr, address, &base, &size, &writable) == 1;
}

void RequestThreadExit(std::uint64_t value) noexcept {
    thread_exit_value = value;
    thread_exit_requested = true;
}

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
    if (stack_top == 0) {
        if (active_gprs != nullptr) {
            // Keep the live parent's red zone and CALL frame intact.
            if (active_gprs[4] < 256) return false;
            stack_top = active_gprs[4] - 256;
        } else {
            std::uint64_t base {}, size {};
            if (!Libs::LibKernel::PthreadGetGuestStack(Libs::LibKernel::PthreadSelf(), &base, &size) ||
                size > UINT64_MAX - base) return false;
            stack_top = base + size;
        }
    }
    const auto spill_count = count > 6 ? count - 6 : 0;
    const auto frame_size = (spill_count + 1) * 8;
    stack_top &= ~std::uint64_t {15};
    if (stack_top < frame_size + 32) return false;
    const auto rsp = ((stack_top - frame_size - 8) & ~std::uint64_t {15}) + 8;
    auto request = Request(target, rsp);
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
        if (run.code == LUMEN_ANDROID_FEXCORE_RUNTIME_GUEST_FAULT_V1 && !delivering_fault) {
            delivering_fault = true;
            const auto* parent = active_gprs;
            active_gprs = run.state.gprs;
            // The guest uses BSD numbering; Linux/Android SIGBUS is 7.
            // Keep the native checkpoint untouched for fault diagnostics.
            const auto guest_signal = run.state.faultSignal == SIGBUS ? 10 :
                                      static_cast<int>(run.state.faultSignal);
            Libs::LibKernel::KernelDispatchGuestException(guest_signal,
                &run.state, run.state.faultAddress);
            active_gprs = parent;
            delivering_fault = false;
        }
        return false;
    }
    *result = run.state.gprs[0];
    return true;
}

bool InvokeWithoutGuestBorrow(std::uint64_t target, const std::uint64_t* arguments,
    std::size_t count, std::uint64_t* result) noexcept {
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
    if (Lumen::GuestCpu::Selected() == Lumen::GuestCpu::Provider::QemuTcg &&
        active_gprs != nullptr && !delivering_fault) {
        // Capture native scalar inputs while the original HLE reader is still
        // counted. The TLS getter must use the already prepared thread block.
        const auto tls = RuntimeLinker::CurrentThreadTlsBase();
        std::uint64_t stack_base {}, stack_size {}, tls_base {}, tls_size {};
        if (!Libs::LibKernel::PthreadGetOwnedGuestStack(&stack_base, &stack_size) ||
            !RuntimeLinker::CurrentThreadTlsExtent(&tls_base, &tls_size)) return false;
        const auto token = Lumen::GuestCpu::QemuBeginFreshCallback(
            stack_base, stack_size, tls_base, tls_size);
        if (token == 0) return false;
        const auto parent_depth = fresh_callback_depth;
        const auto parent_tls = fresh_callback_tls;
        fresh_callback_depth = invocation_depth + 1;
        fresh_callback_tls = tls;
        const bool called = Invoke(target, arguments, count, result);
        fresh_callback_depth = parent_depth;
        fresh_callback_tls = parent_tls;
        const bool restored = Lumen::GuestCpu::QemuEndFreshCallback(token);
        return restored && called;
    }
#endif
    return Invoke(target, arguments, count, result);
}
} // namespace Loader::GuestExecution
#endif
