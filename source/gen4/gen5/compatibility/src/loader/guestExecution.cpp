// Lumen modifications, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "loader/guestExecution.h"
#include "loader/guestMutationScope.h"
#if defined(LUMEN_PS5_DIRECT_FEX)
#include "DirectFexRuntime.h"
#endif
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
#include "QemuProvider.h"
#endif

#include <array>
#include <atomic>
#include <limits>

namespace Loader::GuestExecution {
namespace {

std::atomic<std::uint32_t> runtime_capabilities {};

#if defined(LUMEN_PS5_FEXCORE)
struct ActiveCall {
    const RunRequest* request {};
    bool stopped {};
};

std::uint32_t HostCall(void* opaque, std::uint64_t target, std::uint64_t* gprs,
                       LumenAndroidFexCoreSimdViewV2* simd,
                       std::uint64_t* next_rip) noexcept {
    auto* active = static_cast<ActiveCall*>(opaque);
    if (active == nullptr || active->request == nullptr)
        return LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
    const auto& request = *active->request;
    const auto disposition = request.host_call(request.opaque, target, gprs, simd, next_rip);
    if (disposition == LUMEN_ANDROID_FEXCORE_HOST_CALL_STOP_V1) active->stopped = true;
    return disposition;
}

std::uint32_t ExecutableQuery(void* opaque, std::uint64_t address,
                              std::uint64_t* base, std::uint64_t* size,
                              std::uint32_t* writable) noexcept {
    const auto* active = static_cast<const ActiveCall*>(opaque);
    if (active == nullptr || active->request == nullptr) return 0;
    const auto& request = *active->request;
    return request.executable_query(request.opaque, address, base, size, writable);
}
#endif

} // namespace

RunResult Run(const RunRequest& request) noexcept {
    RunResult result {};
    result.state = request.state;
    if (request.state.structSize != sizeof(request.state) || request.state.abiVersion != 1 ||
        request.state.rip == 0 || request.host_call == nullptr ||
        request.executable_query == nullptr || request.read_memory == nullptr ||
        request.write_memory == nullptr || request.stack_argument_count > 26 ||
        (request.stack_argument_count != 0 && request.stack_arguments == nullptr)) {
        result.code = LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
        return result;
    }
    const auto rsp = request.state.gprs[4];
    if ((rsp & 15U) != 8U || rsp < 16 ||
        rsp > std::numeric_limits<std::uint64_t>::max() - sizeof(std::uint64_t)) {
        result.code = LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
        return result;
    }
    std::uint64_t base {};
    std::uint64_t size {};
    std::uint32_t writable {};
    if (request.executable_query(request.opaque, request.state.rip, &base, &size, &writable) != 1 ||
        size == 0 || base > request.state.rip || request.state.rip - base >= size ||
        size > std::numeric_limits<std::uint64_t>::max() - base || writable > 1) {
        result.code = LUMEN_ANDROID_FEXCORE_RUNTIME_EXECUTABLE_RANGE_DENIED_V1;
        return result;
    }
#if defined(LUMEN_PS5_FEXCORE)
#if defined(LUMEN_PS5_DIRECT_FEX)
    const auto root_return = Lumen::Ps5::DirectFex::RootReturn();
#else
    const auto root_return = LumenAndroidFexCoreRootReturnAddressV1();
#endif
    if (root_return == 0) {
        result.code = LUMEN_ANDROID_FEXCORE_RUNTIME_CONTEXT_FAILED_V1;
        return result;
    }
    // Validate the entire frame before overwriting any return slot or spill.
    // Retained HLE callbacks use the embedding's private-stack proof; root and
    // terminal-fault entry keep the exclusive mapping epoch.
    const auto stack_span = 24u + request.stack_argument_count * sizeof(std::uint64_t);
    if (stack_span > std::numeric_limits<std::uint64_t>::max() - (rsp - 16)) {
        result.code = LUMEN_ANDROID_FEXCORE_RUNTIME_INVALID_ARGUMENT_V1;
        return result;
    }
    if (request.prepare_callback_stack != nullptr) {
        if (request.native_root_entry || request.fresh_callback_entry ||
            !request.prepare_callback_stack(request.opaque, rsp, root_return,
                request.stack_arguments, request.stack_argument_count)) {
            result.code = LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
            return result;
        }
    } else {
        std::array<std::uint8_t, 24 + 26 * sizeof(std::uint64_t)> stack_bytes {};
        GuestMutationScope mutation(request.native_root_entry || request.fresh_callback_entry
            ? GuestMutationScope::Admission::NativeRootEntry
            : GuestMutationScope::Admission::Strict);
#if defined(LUMEN_WITH_QEMU_TCG) && LUMEN_WITH_QEMU_TCG
        if (request.fresh_callback_entry && !Lumen::GuestCpu::QemuValidateFreshCallbackParent()) {
            result.code = LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
            return result;
        }
#endif
        if (!request.read_memory(request.opaque, rsp - 16, stack_bytes.data(), stack_span) ||
            !request.write_memory(request.opaque, rsp - 16, stack_bytes.data(), stack_span) ||
            !request.write_memory(request.opaque, rsp, &root_return, sizeof(root_return)) ||
            (request.stack_argument_count != 0 && !request.write_memory(request.opaque,
                rsp + 8, request.stack_arguments, request.stack_argument_count * 8))) {
            result.code = LUMEN_ANDROID_FEXCORE_RUNTIME_ADMISSION_DENIED_V1;
            return result;
        }
    }
    ActiveCall active {&request};
#if defined(LUMEN_PS5_DIRECT_FEX)
    result.code = Lumen::Ps5::DirectFex::RunGuestV2(&result.state, HostCall, ExecutableQuery, &active);
#elif defined(__APPLE__)
    LumenAndroidFexCoreGuestStateV3 checkpoint {};
    checkpoint.gp = request.state;
    result.code = LumenAndroidFexCoreRunGuestV3(&checkpoint, HostCall, ExecutableQuery, &active);
    result.state = checkpoint.gp;
    result.checkpoint = checkpoint;
#else
    result.code = LumenAndroidFexCoreRunGuestV2(&result.state, HostCall, ExecutableQuery, &active);
#endif
    result.stopped = active.stopped;
    result.completed = result.code == LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1 &&
        !result.stopped && result.state.rip == root_return;
#endif
    return result;
}

bool PublishRuntimeCapabilities(std::uint32_t capabilities) noexcept {
    if (capabilities != RequiredRuntimeCapabilities) return false;
#if defined(LUMEN_PS5_FEXCORE)
    std::uint32_t expected {};
    return runtime_capabilities.compare_exchange_strong(expected, capabilities,
        std::memory_order_release, std::memory_order_acquire) || expected == capabilities;
#else
    return false;
#endif
}

bool FullRuntimeReady() noexcept {
    return runtime_capabilities.load(std::memory_order_acquire) == RequiredRuntimeCapabilities;
}

const char* FullRuntimeLimitation() noexcept {
    return "PS5 ARM64 CPU embedding bootstrap unavailable: initialize the selected translator, "
           "the checked VM owner and typed HLE registry before loading guest code. "
           "CPU embedding readiness does not qualify graphics or gameplay.";
}

} // namespace Loader::GuestExecution
