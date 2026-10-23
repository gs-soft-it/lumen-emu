// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#if defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

namespace Lumen::Performance {

enum class Phase : std::uint8_t {
    RootSetup, ExecutionAdmission, RegisteredTargets, ExecuteEnvelope,
    HostAdmission, HostCall, HostWait, Readmission, Safepoint,
    GpuEpoch, GpuFinish, GpuPublishBuffers, GpuPublishTextures, DirectUnaligned, Count
};
inline constexpr std::array<const char*, static_cast<unsigned>(Phase::Count)> PhaseNames {
    "root_setup", "execution_admission", "registered_targets", "execute_envelope",
    "host_admission", "host_call", "host_wait", "readmission", "safepoint",
    "gpu_epoch", "gpu_finish", "gpu_publish_buffers", "gpu_publish_textures", "direct_unaligned"
};

struct Stamp { std::uint64_t wall {}, cpu {}; bool valid {}; };
struct Interval { Stamp start {}, end {}; };
struct Delta { Phase phase {}; std::uint64_t wall {}, cpu {}; bool valid {}; };

// This bounded POD stack deliberately contains no pointers to C++ automatic
// scopes. FEX can leave a syscall through a signal without C++ unwinding. End
// recovers abandoned descendants; later destructors with consumed tokens do
// nothing. Every charged interval belongs to just one phase on this thread.
class Ledger {
public:
    static constexpr unsigned Capacity = 128;
    Delta Charge(Stamp now) noexcept {
        if (depth_ == 0) return {};
        auto& frame = frames_[depth_ - 1];
        Delta delta {frame.phase, 0, 0, now.valid && frame.start.valid &&
            now.wall >= frame.start.wall && now.cpu >= frame.start.cpu};
        if (delta.valid) { delta.wall = now.wall - frame.start.wall; delta.cpu = now.cpu - frame.start.cpu; }
        frame.start = now;
        return delta;
    }
    std::uint64_t Begin(Phase phase, Stamp now) noexcept {
        if (depth_ == Capacity || sequence_ == UINT64_MAX) { ++overflow_; return 0; }
        const auto token = ++sequence_;
        frames_[depth_++] = {phase, now, token};
        return token;
    }
    Delta End(std::uint64_t token, Stamp now) noexcept {
        if (token == 0) return {};
        unsigned index = depth_;
        while (index != 0 && frames_[index - 1].token != token) --index;
        if (index == 0) return {};
        auto delta = Charge(now);
        abandoned_ += depth_ - index;
        depth_ = index - 1;
        if (depth_ != 0) frames_[depth_ - 1].start = now;
        return delta;
    }
    unsigned Depth() const noexcept { return depth_; }
    std::uint64_t Abandoned() const noexcept { return abandoned_; }
    std::uint64_t Overflow() const noexcept { return overflow_; }
private:
    struct Frame { Phase phase {}; Stamp start {}; std::uint64_t token {}; };
    std::array<Frame, Capacity> frames_ {};
    unsigned depth_ {};
    std::uint64_t sequence_ {}, abandoned_ {}, overflow_ {};
};

struct Totals { std::atomic<std::uint64_t> wall {}, cpu {}, events {}; };
struct JitCacheTotals { std::atomic<std::uint64_t> events {}, value {}, detail {}; };
inline constexpr std::array<const char*, 10> JitCacheNames {
    "buffer_allocate", "buffer_clear", "buffer_adopt", "block_compile", "block_load",
    "invalidate_blocks", "invalidate_request", "restart_far", "restart_buffer", "explicit_clear"
};
struct Data {
    std::array<Totals, static_cast<unsigned>(Phase::Count)> phases {};
    std::array<JitCacheTotals, JitCacheNames.size()> jitCache {};
    std::atomic<bool> jitCacheHooksAvailable {};
    std::atomic<std::uint64_t> cacheMisses {}, cacheReadLockTicks {},
        cacheWriteLockTicks {}, diskMisses {};
    std::atomic<std::uint64_t> alignmentFaults {}, alignmentHandled {},
        compileTicks {}, diskLoadTicks {}, jitCount {}, nativeLoads {}, tickFrequency {},
        clockFailures {}, abandoned {}, overflow {}, lastReport {}, reports {},
        directUnalignedCalls {}, directUnalignedHandled {}, directUnalignedWrites {},
        openScopes {}, nextThread {}, nextGpuEpoch {}, activeGpuEpoch {};
    std::array<std::atomic<std::uint64_t>, 4> faultFamilies {};
    std::array<std::atomic<std::uint64_t>, 17> faultWidths {}, directWidths {};
    std::atomic_flag printing = ATOMIC_FLAG_INIT;
};
inline constinit Data data {};
inline thread_local Ledger ledger;
inline thread_local std::uint64_t threadKey {};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
inline void Report(bool final = false) noexcept;

inline bool Enabled() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_FEX_PERFORMANCE_TRACE");
        bool selected = value != nullptr && std::strcmp(value, "1") == 0;
#if defined(__ANDROID__)
        // The Android diagnostic profile already selects timing/readback
        // reports. Include FEX tracing under that same explicit selector.
        const auto* profile = std::getenv("LUMEN_ANDROID_RUNTIME_PERFORMANCE_PROFILE");
        selected |= profile != nullptr && std::strcmp(profile, "1") == 0;
#endif
        if (selected) {
            std::atexit([] { Report(true); });
            std::at_quick_exit([] { Report(true); });
        }
        return selected;
    }();
    return enabled;
}
inline Stamp Now() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    timespec wall {}, cpu {};
    const bool valid = clock_gettime(CLOCK_MONOTONIC, &wall) == 0 &&
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0;
    if (!valid) data.clockFailures.fetch_add(1, std::memory_order_relaxed);
    return {static_cast<std::uint64_t>(wall.tv_sec) * 1000000000ULL + wall.tv_nsec,
        static_cast<std::uint64_t>(cpu.tv_sec) * 1000000000ULL + cpu.tv_nsec, valid};
#else
    data.clockFailures.fetch_add(1, std::memory_order_relaxed);
    return {};
#endif
}
inline void Record(Delta delta) noexcept {
    if (!delta.valid) return;
    auto& total = data.phases[static_cast<unsigned>(delta.phase)];
    total.wall.fetch_add(delta.wall, std::memory_order_relaxed);
    total.cpu.fetch_add(delta.cpu, std::memory_order_relaxed);
}
inline void AlignmentFault(bool handled, std::uint32_t family = 0, std::uint32_t width = 0) noexcept {
    // Called only after a Run latched Enabled outside the signal handler.
    (handled ? data.alignmentHandled : data.alignmentFaults).fetch_add(1, std::memory_order_relaxed);
    if (!handled) {
        data.faultFamilies[family < 4 ? family : 0].fetch_add(1, std::memory_order_relaxed);
        data.faultWidths[width <= 16 ? width : 0].fetch_add(1, std::memory_order_relaxed);
    }
}
inline void DirectUnaligned(bool write, std::uint32_t width, bool handled) noexcept {
    // The caller latches the trace selector before entering guest execution.
    data.directUnalignedCalls.fetch_add(1, std::memory_order_relaxed);
    if (handled) data.directUnalignedHandled.fetch_add(1, std::memory_order_relaxed);
    if (write) data.directUnalignedWrites.fetch_add(1, std::memory_order_relaxed);
    data.directWidths[width <= 16 ? width : 0].fetch_add(1, std::memory_order_relaxed);
}

// Diagnostic-only bounded timeline. Atomic fields avoid races when a slow
// reader overlaps a producer wrapping the ring. Sequentially consistent
// generation checks reject overwritten records instead of joining epochs.
struct TimelineEntry {
    std::atomic_flag writer = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> ticket {}, thread {}, token {}, wall {}, cpu {}, gpuEpoch {}, mutationEpoch {}, kind {}, phase {};
};
inline constexpr unsigned TimelineCapacity = 1024;
inline std::array<TimelineEntry, TimelineCapacity> timeline {};
inline std::atomic<std::uint64_t> timelineSequence {}, timelineDropped {};
inline std::uint64_t timelineReported {};
inline bool TimelineEnabled() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_FEX_PERFORMANCE_TIMELINE");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
inline void PublishTimeline(std::uint64_t ticket, std::uint64_t thread, Phase phase,
    std::uint64_t token, Stamp now, bool begin, std::uint64_t gpuEpoch, std::uint64_t mutationEpoch = 0) noexcept {
    auto& entry = timeline[(ticket - 1) % TimelineCapacity];
    if (entry.writer.test_and_set(std::memory_order_acquire)) return;
    // A producer may be descheduled after reserving its ticket. It must not
    // overwrite the newer generation published while it was asleep.
    if (entry.ticket.load() >= ticket) {
        entry.writer.clear(std::memory_order_release); return;
    }
    entry.ticket.store(0);
    entry.thread.store(thread); entry.token.store(token);
    entry.wall.store(now.wall); entry.cpu.store(now.cpu); entry.gpuEpoch.store(gpuEpoch);
    entry.mutationEpoch.store(mutationEpoch);
    entry.kind.store(begin ? 1 : 0); entry.phase.store(static_cast<unsigned>(phase));
    entry.ticket.store(ticket);
    entry.writer.clear(std::memory_order_release);
}
inline void Timeline(Phase phase, std::uint64_t token, Stamp now, bool begin, std::uint64_t gpuEpoch, std::uint64_t mutationEpoch = 0) noexcept {
    if (!TimelineEnabled() || !now.valid) return;
    if (threadKey == 0) threadKey = data.nextThread.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto ticket = timelineSequence.fetch_add(1, std::memory_order_relaxed) + 1;
    PublishTimeline(ticket, threadKey, phase, token, now, begin, gpuEpoch, mutationEpoch);
}
inline void ReportTimeline(unsigned long long process, std::uint64_t snapshot) noexcept {
    if (!TimelineEnabled()) return;
    const auto end = timelineSequence.load(std::memory_order_acquire);
    auto start = timelineReported + 1;
    if (end >= start && end - start >= TimelineCapacity) {
        timelineDropped.fetch_add(end - start + 1 - TimelineCapacity, std::memory_order_relaxed);
        start = end - TimelineCapacity + 1;
    }
    for (auto ticket = start; ticket <= end; ++ticket) {
        const auto& entry = timeline[(ticket - 1) % TimelineCapacity];
        if (entry.ticket.load() != ticket) {
            // Includes a reserved but not yet published ticket. Advancing the
            // cursor loses it deliberately and counts that loss exactly once.
            timelineDropped.fetch_add(1, std::memory_order_relaxed); continue;
        }
        const auto thread = entry.thread.load(), token = entry.token.load();
        const auto wall = entry.wall.load(), cpu = entry.cpu.load(), epoch = entry.gpuEpoch.load();
        const auto mutation = entry.mutationEpoch.load();
        const auto kind = entry.kind.load(), phase = entry.phase.load();
        if (entry.ticket.load() != ticket || phase >= PhaseNames.size()) {
            timelineDropped.fetch_add(1, std::memory_order_relaxed); continue;
        }
        std::fprintf(stderr, "FEX_PERFORMANCE_TIMELINE_V1 pid=%llu snapshot=%llu sequence=%llu "
            "thread=%llu token=%llu phase=%s begin=%llu monotonic_ns=%llu thread_cpu_ns=%llu "
            "gpu_scope=%llu mutation_epoch=%llu diagnostic_only=1 bounded=1\n", process,
            static_cast<unsigned long long>(snapshot), static_cast<unsigned long long>(ticket),
            static_cast<unsigned long long>(thread), static_cast<unsigned long long>(token), PhaseNames[phase],
            static_cast<unsigned long long>(kind), static_cast<unsigned long long>(wall),
            static_cast<unsigned long long>(cpu), static_cast<unsigned long long>(epoch),
            static_cast<unsigned long long>(mutation));
    }
    timelineReported = end;
}
inline void CompileStats(std::uint64_t compileTicks, std::uint64_t diskTicks,
    std::uint64_t count, std::uint64_t loads, std::uint64_t frequency) noexcept {
    data.compileTicks.fetch_add(compileTicks, std::memory_order_relaxed);
    data.diskLoadTicks.fetch_add(diskTicks, std::memory_order_relaxed);
    data.jitCount.fetch_add(count, std::memory_order_relaxed);
    data.nativeLoads.fetch_add(loads, std::memory_order_relaxed);
    data.tickFrequency.store(frequency, std::memory_order_relaxed);
}

inline void MarkJitCacheHooksAvailable() noexcept {
    // Set by an adapter built with the matching overlay, even if no event fires.
    // Absence of an event must not masquerade as absence of instrumentation.
    if (Enabled()) data.jitCacheHooksAvailable.store(true, std::memory_order_relaxed);
}

inline void CacheStats(std::uint64_t misses, std::uint64_t readLockTicks,
    std::uint64_t writeLockTicks, std::uint64_t diskMisses) noexcept {
    if (!Enabled()) return;
    // These are already accumulated by FEX with ThreadStats attached. No new
    // timing operation or generated-code hook is added to collect them.
    data.cacheMisses.fetch_add(misses, std::memory_order_relaxed);
    data.cacheReadLockTicks.fetch_add(readLockTicks, std::memory_order_relaxed);
    data.cacheWriteLockTicks.fetch_add(writeLockTicks, std::memory_order_relaxed);
    data.diskMisses.fetch_add(diskMisses, std::memory_order_relaxed);
}

inline void JitCacheEvent(std::uint32_t kind, std::uint64_t value, std::uint64_t detail) noexcept {
    // The overlay calls this only from compilation/cache maintenance. Product
    // defaults do no atomic accounting and generated guest code has no hook.
    if (!Enabled() || kind == 0 || kind > JitCacheNames.size()) return;
    auto& total = data.jitCache[kind - 1];
    total.events.fetch_add(1, std::memory_order_relaxed);
    total.value.fetch_add(value, std::memory_order_relaxed);
    total.detail.fetch_add(detail, std::memory_order_relaxed);
}

inline void Report(bool final) noexcept {
    if (!Enabled()) return;
    if (data.printing.test_and_set(std::memory_order_acquire)) return;
    const auto now = Now();
    const auto previous = data.lastReport.load(std::memory_order_relaxed);
    if (!now.valid || (!final &&
        (now.wall <= previous || now.wall - previous < 5000000000ULL))) {
        data.printing.clear(std::memory_order_release); return;
    }
    data.lastReport.store(now.wall, std::memory_order_relaxed);
    Record(ledger.Charge(now));
    const auto report = data.reports.fetch_add(1, std::memory_order_relaxed) + 1;
#if defined(__APPLE__) || defined(__linux__)
    const auto process = static_cast<unsigned long long>(getpid());
#else
    const auto process = 0ULL;
#endif
    for (unsigned i = 0; i != PhaseNames.size(); ++i) {
        const auto& total = data.phases[i];
        std::fprintf(stderr, "FEX_PERFORMANCE_V1 pid=%llu snapshot=%llu monotonic_ns=%llu phase=%s "
            "thread_wall_ns=%llu thread_cpu_ns=%llu events=%llu diagnostic_only=1 thread_time_sum=1 "
            "async_snapshot=1 completed_intervals=1 final=%u\n",
            process, static_cast<unsigned long long>(report), static_cast<unsigned long long>(now.wall), PhaseNames[i],
            static_cast<unsigned long long>(total.wall.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(total.cpu.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(total.events.load(std::memory_order_relaxed)), final ? 1U : 0U);
    }
    std::fprintf(stderr, "FEX_PERFORMANCE_CORE_V1 pid=%llu snapshot=%llu compile_block_inclusive_ticks=%llu "
        "disk_load_inclusive_ticks=%llu tick_frequency=%llu jit_compilations=%llu native_loads=%llu "
        "alignment_faults=%llu alignment_handled=%llu abandoned_scopes=%llu overflow=%llu clock_failures=%llu "
        "stats_sampled_at_owner_boundaries=1\n", process, static_cast<unsigned long long>(report),
        static_cast<unsigned long long>(data.compileTicks.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.diskLoadTicks.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.tickFrequency.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.jitCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.nativeLoads.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.alignmentFaults.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.alignmentHandled.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.abandoned.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.overflow.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.clockFailures.load(std::memory_order_relaxed)));
    std::fprintf(stderr, "FEX_PERFORMANCE_CACHE_V1 pid=%llu snapshot=%llu "
        "lookup_l1_misses=%llu cache_read_lock_ticks=%llu cache_write_lock_ticks=%llu disk_cache_misses=%llu "
        "jit_cache_hooks_available=%u stats_sampled_at_owner_boundaries=1 "
        "jit_count_includes_race_attempts=1 block_compile_counts_completed_emissions=1\n",
        process, static_cast<unsigned long long>(report),
        static_cast<unsigned long long>(data.cacheMisses.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.cacheReadLockTicks.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.cacheWriteLockTicks.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.diskMisses.load(std::memory_order_relaxed)),
        data.jitCacheHooksAvailable.load(std::memory_order_relaxed) ? 1U : 0U);
    for (unsigned i = 0; i != JitCacheNames.size(); ++i) {
        const auto& total = data.jitCache[i];
        std::fprintf(stderr, "FEX_PERFORMANCE_JIT_CACHE_V1 pid=%llu snapshot=%llu event=%s "
            "events=%llu value_sum=%llu detail_sum=%llu diagnostic_only=1 async_snapshot=1 final=%u\n",
            process, static_cast<unsigned long long>(report), JitCacheNames[i],
            static_cast<unsigned long long>(total.events.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(total.value.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(total.detail.load(std::memory_order_relaxed)), final ? 1U : 0U);
    }
    ReportTimeline(process, report);
    std::fprintf(stderr, "FEX_PERFORMANCE_ACCESS_V1 pid=%llu snapshot=%llu direct_calls=%llu direct_handled=%llu "
        "direct_writes=%llu open_scopes=%llu timeline_dropped=%llu census_complete=0 diagnostic_only=1\n",
        process, static_cast<unsigned long long>(report),
        static_cast<unsigned long long>(data.directUnalignedCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.directUnalignedHandled.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.directUnalignedWrites.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(data.openScopes.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(timelineDropped.load(std::memory_order_relaxed)));
    for (unsigned width : {0U, 1U, 2U, 4U, 8U, 16U}) {
        std::fprintf(stderr, "FEX_PERFORMANCE_WIDTH_V1 pid=%llu snapshot=%llu width=%u signal_faults=%llu "
            "direct_calls=%llu diagnostic_only=1\n", process, static_cast<unsigned long long>(report), width,
            static_cast<unsigned long long>(data.faultWidths[width].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(data.directWidths[width].load(std::memory_order_relaxed)));
    }
    for (unsigned family = 0; family != data.faultFamilies.size(); ++family) {
        std::fprintf(stderr, "FEX_PERFORMANCE_FAULT_FAMILY_V1 pid=%llu snapshot=%llu family=%u faults=%llu "
            "diagnostic_only=1\n", process, static_cast<unsigned long long>(report), family,
            static_cast<unsigned long long>(data.faultFamilies[family].load(std::memory_order_relaxed)));
    }
    data.printing.clear(std::memory_order_release);
}

class Scope {
public:
    explicit Scope(Phase phase, std::uint64_t mutationEpoch = 0,
                   Interval* inclusiveInterval = nullptr) noexcept {
        if (!Enabled()) return;
        const auto now = Now();
        Record(ledger.Charge(now));
        const auto before = ledger.Overflow();
        token_ = ledger.Begin(phase, now);
        data.overflow.fetch_add(ledger.Overflow() - before, std::memory_order_relaxed);
        if (token_ != 0) {
            inclusiveInterval_ = inclusiveInterval;
            if (inclusiveInterval_ != nullptr) inclusiveInterval_->start = now;
            phase_ = phase;
            data.openScopes.fetch_add(1, std::memory_order_relaxed);
            data.phases[static_cast<unsigned>(phase)].events.fetch_add(1, std::memory_order_relaxed);
            // Only the GPU producer records the actual mutation token. Other
            // threads capture one atomic scope ID and join that producer event;
            // no separately loaded token can create a mismatched correlation.
            if (phase == Phase::GpuEpoch) {
                gpuEpoch_ = data.nextGpuEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
                data.activeGpuEpoch.store(gpuEpoch_, std::memory_order_relaxed);
                mutationEpoch_ = mutationEpoch;
            } else {
                gpuEpoch_ = data.activeGpuEpoch.load(std::memory_order_relaxed);
            }
            Timeline(phase, token_, now, true, gpuEpoch_, mutationEpoch_);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { End(); }
    void End() noexcept {
        if (token_ == 0) return;
        const auto before = ledger.Abandoned();
        const auto depth = ledger.Depth();
        const auto now = Now();
        Record(ledger.End(token_, now));
        const auto removed = depth - ledger.Depth();
        if (removed != 0) {
            if (inclusiveInterval_ != nullptr) inclusiveInterval_->end = now;
            data.openScopes.fetch_sub(removed, std::memory_order_relaxed);
            Timeline(phase_, token_, now, false, gpuEpoch_, mutationEpoch_);
            if (phase_ == Phase::GpuEpoch) {
                auto epoch = gpuEpoch_;
                data.activeGpuEpoch.compare_exchange_strong(epoch, 0, std::memory_order_relaxed);
            }
        }
        data.abandoned.fetch_add(ledger.Abandoned() - before, std::memory_order_relaxed);
        token_ = 0;
        Report();
    }
private:
    Interval* inclusiveInterval_ {};
    std::uint64_t token_ {};
    std::uint64_t gpuEpoch_ {}, mutationEpoch_ {};
    Phase phase_ {};
};

} // namespace Lumen::Performance
