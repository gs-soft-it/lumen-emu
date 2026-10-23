// SPDX-License-Identifier: MIT

#include "fex_hle_bridge.h"
#include "hle_trace.h"
#ifndef LUMEN_GEN4_DIRECT_FEX
#include "GuestCpuRuntime.h"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#ifdef LUMEN_GEN4_DIRECT_FEX
#include <functional>
#include <thread>
#endif

namespace {
// Diagnostic cap: high enough to capture full boot + background-thread HLE activity
// (the 256 default truncated the trace before the game's Fios driver thread ran).
constexpr uint32_t HleTraceLimit = 100000;
std::atomic_uint32_t HleTraceCount{};
#ifdef LUMEN_GEN4_DIRECT_FEX
const bool HleTraceRequested =
    Core::GuestCpu::LumenHostHleTraceRequested(std::getenv("LUMEN_HOST_FEX_HLE_TRACE"));
#endif

thread_local Core::GuestCpu::HleCallFrame* ActiveHleCallFrame{};
#ifndef LUMEN_GEN4_DIRECT_FEX
thread_local const Core::GuestCpu::HleCallAdapter* ActiveHleAdapter{};
#endif

class ActiveHleCallScope final {
public:
#ifdef LUMEN_GEN4_DIRECT_FEX
    explicit ActiveHleCallScope(Core::GuestCpu::HleCallFrame& frame)
        : previous{ActiveHleCallFrame} {
        ActiveHleCallFrame = &frame;
    }
#else
    ActiveHleCallScope(Core::GuestCpu::HleCallFrame& frame,
                      const Core::GuestCpu::HleCallAdapter* adapter)
        : previous{ActiveHleCallFrame}, previousAdapter{ActiveHleAdapter} {
        ActiveHleCallFrame = &frame;
        ActiveHleAdapter = adapter;
        LumenAndroidFexCoreSetHostCallDiagnosticV1(adapter->Operation(), adapter->Name().data());
    }
#endif

    ~ActiveHleCallScope() {
#ifndef LUMEN_GEN4_DIRECT_FEX
        LumenAndroidFexCoreSetHostCallDiagnosticV1(
            previousAdapter != nullptr ? previousAdapter->Operation() : 0,
            previousAdapter != nullptr ? previousAdapter->Name().data() : nullptr);
        ActiveHleAdapter = previousAdapter;
#endif
        ActiveHleCallFrame = previous;
    }

private:
    Core::GuestCpu::HleCallFrame* previous;
#ifndef LUMEN_GEN4_DIRECT_FEX
    const Core::GuestCpu::HleCallAdapter* previousAdapter;
#endif
};
} // namespace

namespace Core::GuestCpu {

bool IsGuestHleCallActive() noexcept { return ActiveHleCallFrame != nullptr; }

bool ValidateHlePointer(const void* pointer, std::size_t size, bool writable) noexcept {
    if (ActiveHleCallFrame == nullptr) return true;
    if (pointer == nullptr || size == 0 || ActiveHleCallFrame->validate_range == nullptr) return false;
    try {
        return ActiveHleCallFrame->validate_range(ActiveHleCallFrame->validate_context,
            reinterpret_cast<std::uintptr_t>(pointer), size, writable);
    } catch (...) { return false; }
}

bool PublishHostRange(const void* pointer, std::size_t size, bool writable) {
    if (ActiveHleCallFrame == nullptr || pointer == nullptr ||
        ActiveHleCallFrame->publish_host_range == nullptr) {
        return false;
    }
    return ActiveHleCallFrame->publish_host_range(
        ActiveHleCallFrame->host_range_context, reinterpret_cast<std::uintptr_t>(pointer), size,
        writable);
}

bool RevokeHostRange(const void* pointer) {
    if (ActiveHleCallFrame == nullptr || pointer == nullptr ||
        ActiveHleCallFrame->revoke_host_range == nullptr) {
        return false;
    }
    return ActiveHleCallFrame->revoke_host_range(ActiveHleCallFrame->host_range_context,
                                                  reinterpret_cast<std::uintptr_t>(pointer));
}

HleGuestBridge::HleGuestBridge(HleCallRegistry& registry_, RangeValidator validator_,
                               void* validator_context_, FailureReporter reporter_,
                               void* reporter_context_, ExecutableRangeQuery executable_query_,
                               void* executable_query_context_)
    : registry{registry_}, validator{validator_}, validator_context{validator_context_},
      reporter{reporter_}, reporter_context{reporter_context_},
      executable_query{executable_query_}, executable_query_context{executable_query_context_} {}

Fex::EngineResult<bool> HleGuestBridge::Invoke(HleCallFrame& frame) {
    const auto* adapter = registry.FindRaw(frame.operation);
    if (adapter == nullptr) {
        const HleCallFailure failure{ENOSYS, "unregistered HLE operation"};
        Report(failure);
        return Fex::EngineFailure{Fex::EngineStage::Bridge, failure.error};
    }
    // Tracing is opt-in and bounded.
#ifdef LUMEN_GEN4_DIRECT_FEX
    const bool trace_requested = HleTraceRequested;
    auto trace_index = HleTraceLimit;
    if (trace_requested) {
        trace_index = HleTraceCount.load(std::memory_order_relaxed);
    }
#else
    static const bool trace_requested = LumenHostHleTraceRequested(std::getenv("LUMEN_HOST_FEX_HLE_TRACE"));
    auto trace_index = HleTraceCount.load(std::memory_order_relaxed);
#endif
    if (trace_requested && trace_index < HleTraceLimit) {
        trace_index = HleTraceCount.fetch_add(1, std::memory_order_relaxed);
    }
    const bool trace = trace_requested && trace_index < HleTraceLimit;
#ifdef LUMEN_GEN4_DIRECT_FEX
    const auto trace_thread = trace ? std::hash<std::thread::id>{}(std::this_thread::get_id()) : 0;
#endif
    if (trace) {
#ifdef LUMEN_GEN4_DIRECT_FEX
        std::fprintf(stderr,
                     "LUMEN_HOST_FEX_HLE_BEGIN index=%u thread=%zx operation=%llu name=%.*s "
                     "rsp=%#llx rdi=%#llx rsi=%#llx rdx=%#llx rcx=%#llx r8=%#llx r9=%#llx\n",
                     trace_index, trace_thread, static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.rsp),
                     static_cast<unsigned long long>(frame.gpr[7]),
                     static_cast<unsigned long long>(frame.gpr[6]),
                     static_cast<unsigned long long>(frame.gpr[2]),
                     static_cast<unsigned long long>(frame.gpr[1]),
                     static_cast<unsigned long long>(frame.gpr[8]),
                     static_cast<unsigned long long>(frame.gpr[9]));
#else
        std::fprintf(stderr,
                     "LUMEN_HOST_FEX_HLE_BEGIN index=%u operation=%llu name=%.*s rsp=%#llx\n",
                     trace_index, static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.rsp));
#endif
    }
    frame.validate_range = ValidateRange;
    frame.validate_context = this;
    frame.publish_host_range = PublishHostRange;
    frame.revoke_host_range = RevokeHostRange;
    frame.host_range_context = this;
#ifdef LUMEN_GEN4_DIRECT_FEX
    const ActiveHleCallScope active_frame{frame};
#else
    const ActiveHleCallScope active_frame{frame, adapter};
#endif
    const auto result = adapter->Invoke(frame);
    if (const auto* failure = std::get_if<HleCallFailure>(&result)) {
        if (trace) {
#ifdef LUMEN_GEN4_DIRECT_FEX
            std::fprintf(stderr,
                         "LUMEN_HOST_FEX_HLE_END index=%u thread=%zx operation=%llu name=%.*s error=%d\n",
                         trace_index, trace_thread, static_cast<unsigned long long>(frame.operation),
                         static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                         failure->error);
#else
            std::fprintf(stderr,
                         "LUMEN_HOST_FEX_HLE_END index=%u operation=%llu name=%.*s error=%d\n",
                         trace_index, static_cast<unsigned long long>(frame.operation),
                         static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                         failure->error);
#endif
        }
        Report(*failure);
        return Fex::EngineFailure{Fex::EngineStage::Bridge, failure->error};
    }
    if (trace) {
#ifdef LUMEN_GEN4_DIRECT_FEX
        std::fprintf(stderr,
                     "LUMEN_HOST_FEX_HLE_END index=%u thread=%zx operation=%llu name=%.*s rax=%#llx\n",
                     trace_index, trace_thread, static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.gpr[0]));
#else
        std::fprintf(stderr,
                     "LUMEN_HOST_FEX_HLE_END index=%u operation=%llu name=%.*s rax=%#llx\n",
                     trace_index, static_cast<unsigned long long>(frame.operation),
                     static_cast<int>(adapter->Name().size()), adapter->Name().data(),
                     static_cast<unsigned long long>(frame.gpr[0]));
#endif
    }
    return true;
}

std::optional<GuestExecutionRange> HleGuestBridge::QueryExecutableRange(
    std::uintptr_t address) {
    if (executable_query == nullptr) {
        return std::nullopt;
    }
    return executable_query(executable_query_context, address);
}

#ifndef LUMEN_GEN4_DIRECT_FEX
bool HleGuestBridge::QueryMemoryAccess(std::uintptr_t address, std::size_t size,
                                     bool writable) {
    return ValidateRange(this, address, size, writable);
}
#endif

bool HleGuestBridge::ValidateRange(void* context, std::uintptr_t address, std::size_t size,
                                   bool writable) {
    if (context == nullptr) {
        return false;
    }
    const auto* bridge = static_cast<const HleGuestBridge*>(context);
    if (bridge->validator != nullptr &&
        bridge->validator(bridge->validator_context, address, size, writable)) {
        return true;
    }
    return bridge->ValidatePublishedHostRange(address, size, writable);
}

bool HleGuestBridge::PublishHostRange(void* context, std::uintptr_t address, std::size_t size,
                                      bool writable) {
    if (context == nullptr || address == 0 || size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    auto* bridge = static_cast<HleGuestBridge*>(context);
    const HostRange range{address, address + size, writable};
    std::unique_lock lock{bridge->host_range_mutex};
    const bool overlaps = std::ranges::any_of(bridge->host_ranges, [&](const HostRange& existing) {
        return range.begin < existing.end && existing.begin < range.end;
    });
    if (overlaps) {
        return false;
    }
    bridge->host_ranges.push_back(range);
    return true;
}

bool HleGuestBridge::RevokeHostRange(void* context, std::uintptr_t address) {
    if (context == nullptr || address == 0) {
        return false;
    }
    auto* bridge = static_cast<HleGuestBridge*>(context);
    std::unique_lock lock{bridge->host_range_mutex};
    const auto range = std::ranges::find(bridge->host_ranges, address, &HostRange::begin);
    if (range == bridge->host_ranges.end()) {
        return false;
    }
    bridge->host_ranges.erase(range);
    return true;
}

bool HleGuestBridge::ValidatePublishedHostRange(std::uintptr_t address, std::size_t size,
                                                bool writable) const {
    if (address == 0 || size == 0 || address > UINTPTR_MAX - size) {
        return false;
    }
    const auto end = address + size;
    std::shared_lock lock{host_range_mutex};
    return std::ranges::any_of(host_ranges, [&](const HostRange& range) {
        return range.begin <= address && end <= range.end && (!writable || range.writable);
    });
}

void HleGuestBridge::Report(const HleCallFailure& failure) const {
    if (reporter != nullptr) {
        reporter(reporter_context, failure);
    }
}

} // namespace Core::GuestCpu
