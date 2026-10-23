// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#endif

// Opt-in completed-span diagnostics. Never use from a signal handler. Spans
// are inclusive, so nested/thread totals cannot be added into a frame budget.
// Frame hints identify the contemporaneous presentation event, not causality.
// object/dependency/value retain the producer's explicit numeric identities;
// zero means unknown. Only a matching producer event can establish an edge.
// ARM_FILE absence keeps immediate tracing. A configured regular file arms
// tracing for the process lifetime. Before arming, only every 1024th trace query
// per thread checks the clock, and at most one stat per second is admitted
// process-wide. There is no wall-time activation bound without trace queries;
// pre-arm spans are excluded entirely, even when they finish after activation.
namespace Lumen::CriticalPath {

enum class Reason : std::uint8_t {
    HleAdmission, HleCall, HleReadmission, GpuCommandRun, GpuBufferFlush,
    GpuBufferWait, GpuIncrementDe, GpuEopWait, GpuCeWait, GpuDeWait,
    GpuFenceWait, WindowPublishFrame, VulkanAcquire, VulkanSubmit,
    VulkanPresent, GuestFlip, AcceptedPresent, GpuSubmissionAccepted,
    GpuCeProgress, GpuDeProgress, GpuFenceReuseWait, GpuEopPublicationWait,
    GpuRegisterWait, GpuFenceCompleted, GpuReadback, GpuDependencyWait,
    GpuHandoffAdmission, GpuHandoffHeld, GpuHandoffRelease, GpuResourcePreflight,
    GpuHandoffTexelCacheHit, GpuHandoffNativeReuse, GpuEmptyFlushSkipped, Count
};
inline constexpr std::array<const char*, static_cast<unsigned>(Reason::Count)> ReasonNames {
    "hle_admission", "hle_call", "hle_readmission", "gpu_command_run", "gpu_buffer_flush",
    "gpu_buffer_wait", "gpu_increment_de", "gpu_eop_wait", "gpu_ce_wait", "gpu_de_wait",
    "gpu_fence_wait", "window_publish_frame", "vulkan_acquire", "vulkan_submit",
    "vulkan_present", "guest_flip", "accepted_present", "gpu_submission_accepted",
    "gpu_ce_progress", "gpu_de_progress", "gpu_fence_reuse_wait", "gpu_eop_publication_wait",
    "gpu_register_wait", "gpu_fence_completed", "gpu_readback", "gpu_dependency_wait",
    "gpu_handoff_admission", "gpu_handoff_held", "gpu_handoff_release", "gpu_resource_preflight",
    "gpu_handoff_texel_cache_hit", "gpu_handoff_native_reuse", "gpu_empty_flush_skipped"
};
inline constexpr unsigned TargetCapacity = 64;
inline constexpr std::uint64_t LongSpanNs = 1000000;
inline constexpr std::uint64_t ReportPeriodNs = 5000000000ULL;
inline constexpr unsigned DetailPerSecond = 64;
inline constexpr unsigned DetailPerThread = 4096;
inline constexpr unsigned ReportsPerThread = 128;
inline constexpr unsigned RecordCapacity = 1536;
inline std::atomic<std::uint64_t> currentFrame {}, nextFrame {};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

inline bool SelectorEnabled(const char* value) noexcept {
    return value != nullptr && std::strcmp(value, "1") == 0;
}
namespace Detail {
struct PreserveErrno {
    int value {errno};
    ~PreserveErrno() { errno = value; }
};
// Android defaults to logcat. Runtime launchers may select their already-owned
// redirected fd 2 before the first write. A lock-free process latch keeps later
// environment changes from splitting a trace across sinks.
inline std::atomic<unsigned> stderrSinkSelection {};
static_assert(std::atomic<unsigned>::is_always_lock_free);
inline bool AndroidStderrSelected() noexcept {
    PreserveErrno preserve;
    auto selection = stderrSinkSelection.load(std::memory_order_relaxed);
    if (selection == 0) {
        const unsigned desired = SelectorEnabled(std::getenv("LUMEN_CRITICAL_PATH_STDERR")) ? 2U : 1U;
        if (stderrSinkSelection.compare_exchange_strong(selection, desired, std::memory_order_relaxed))
            selection = desired;
    }
    return selection == 2;
}
inline void Write(const char* line, int length, unsigned capacity) noexcept;
struct ArmingConfig {
    bool enabled {}, deferred {}, valid {true};
    std::array<char, 4096> path {};
};
inline const ArmingConfig& Config() noexcept {
    // Runtime launchers must publish ARM_FILE before the first trace query.
    // Copy the host-owned path once: getenv storage is not retained or reread.
    static const ArmingConfig config = [] {
        PreserveErrno preserve;
        ArmingConfig selected;
        selected.enabled = SelectorEnabled(std::getenv("LUMEN_CRITICAL_PATH_TRACE"));
        const auto* path = std::getenv("LUMEN_CRITICAL_PATH_ARM_FILE");
        if (path == nullptr) return selected;
        selected.deferred = true;
        std::size_t length = 0;
        while (length != selected.path.size() && path[length] != '\0') ++length;
        selected.valid = length != 0 && length < selected.path.size();
        if (selected.valid) std::memcpy(selected.path.data(), path, length + 1);
        return selected;
    }();
    return config;
}
struct PollCadence {
    unsigned remaining {};
    bool Eligible() noexcept {
        if (remaining != 0) { --remaining; return false; }
        remaining = 1023;
        return true;
    }
};
struct PollGate {
    std::atomic<std::uint64_t> next {};
    bool TryPoll(std::uint64_t now) noexcept {
        auto deadline = next.load(std::memory_order_relaxed);
        if (now < deadline || deadline == UINT64_MAX) return false;
        const auto following = now > UINT64_MAX - 1000000000ULL
            ? UINT64_MAX : now + 1000000000ULL;
        // One nonblocking attempt: losing a concurrent poll waits for its next
        // cadence opportunity. The deadline bounds stat calls process-wide.
        return next.compare_exchange_strong(deadline, following, std::memory_order_relaxed);
    }
};
inline PollGate armPollGate;
inline std::atomic<std::uint64_t> armedAtNs {};
inline thread_local PollCadence armCadence;
inline std::uint64_t ArmClock() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    PreserveErrno preserve;
    timespec wall {};
    if (clock_gettime(CLOCK_MONOTONIC, &wall) != 0 || wall.tv_sec < 0) return 0;
    return static_cast<std::uint64_t>(wall.tv_sec) * 1000000000ULL + wall.tv_nsec;
#else
    return 0;
#endif
}
inline void ArmedMarker(std::uint64_t stamp) noexcept {
#if defined(__APPLE__) || defined(__linux__)
    PreserveErrno preserve;
    char line[384];
    const auto length = std::snprintf(line, sizeof(line),
        "LUMEN_CRITICAL_PATH_ARM_V1 pid=%llu monotonic_ns=%llu source=regular_file "
        "process_latched=1 polling_per_thread_calls=1024 stat_interval_ns=1000000000 diagnostic_only=1\n",
        static_cast<unsigned long long>(getpid()), static_cast<unsigned long long>(stamp));
    // Every later completion/report also carries armed_ns, so a failed marker
    // never authorizes a causal inference. Use the same selected sink for all
    // record types and retain failures in the calling thread's next summary.
    Write(line, length, sizeof(line));
#else
    (void)stamp;
#endif
}
inline bool DeferredEnabled(const ArmingConfig& config) noexcept {
    if (armedAtNs.load(std::memory_order_acquire) != 0) return true;
    if (!config.valid || !armCadence.Eligible()) return false;
#if defined(__APPLE__) || defined(__linux__)
    PreserveErrno preserve;
    const auto now = ArmClock();
    if (now == 0 || !armPollGate.TryPoll(now))
        return armedAtNs.load(std::memory_order_acquire) != 0;
    struct stat metadata {};
    if (stat(config.path.data(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) return false;
    const auto observed = ArmClock();
    if (observed == 0) return false;
    std::uint64_t expected = 0;
    if (armedAtNs.compare_exchange_strong(expected, observed, std::memory_order_release,
                                         std::memory_order_relaxed)) ArmedMarker(observed);
    return armedAtNs.load(std::memory_order_acquire) != 0;
#else
    return false;
#endif
}
} // namespace Detail

inline bool Enabled() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    const auto& config = Detail::Config();
    return config.enabled && (!config.deferred || Detail::DeferredEnabled(config));
#else
    return false;
#endif
}
struct Stamp { std::uint64_t wall {}, cpu {}; bool valid {}; };
inline Stamp Now() noexcept {
#if defined(__APPLE__) || defined(__linux__)
    const int savedErrno = errno;
    timespec wall {}, cpu {};
    const bool valid = clock_gettime(CLOCK_MONOTONIC, &wall) == 0 &&
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0 &&
        wall.tv_sec >= 0 && cpu.tv_sec >= 0;
    errno = savedErrno;
    if (!valid) return {};
    return {static_cast<std::uint64_t>(wall.tv_sec) * 1000000000ULL + wall.tv_nsec,
        static_cast<std::uint64_t>(cpu.tv_sec) * 1000000000ULL + cpu.tv_nsec, true};
#else
    return {};
#endif
}

namespace Detail {
inline bool Add(std::uint64_t& destination, std::uint64_t value = 1) noexcept {
    if (value > UINT64_MAX - destination) { destination = UINT64_MAX; return false; }
    destination += value;
    return true;
}
inline unsigned Bucket(std::uint64_t ns) noexcept {
    constexpr std::array<std::uint64_t, 7> edges {
        1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000
    };
    unsigned bucket = 0;
    while (bucket != edges.size() && ns >= edges[bucket]) ++bucket;
    return bucket;
}
struct Metrics {
    std::uint64_t calls {}, wall {}, cpu {}, maxWall {}, overflow {};
    std::array<std::uint64_t, 8> histogram {};
    void Observe(std::uint64_t wallNs, std::uint64_t cpuNs) noexcept {
        if (!Add(calls)) Add(overflow);
        if (!Add(wall, wallNs)) Add(overflow);
        if (!Add(cpu, cpuNs)) Add(overflow);
        if (!Add(histogram[Bucket(wallNs)])) Add(overflow);
        if (wallNs > maxWall) maxWall = wallNs;
    }
};
struct Target {
    bool used {};
    std::uint64_t target {};
    std::array<Metrics, 3> phases {};
};
struct Aggregates {
    std::array<Metrics, ReasonNames.size()> phases {};
    std::array<Target, TargetCapacity> targets {};
    std::uint64_t invalidIntervals {}, targetOverflow {}, invalidReasons {}, firstWall {}, lastWall {};
    bool observed {};
    bool Observe(Reason reason, std::uint64_t object, Stamp begin, Stamp end) noexcept {
        const auto index = static_cast<unsigned>(reason);
        if (index >= phases.size()) { Add(invalidReasons); return false; }
        if (!begin.valid || !end.valid || end.wall < begin.wall || end.cpu < begin.cpu) {
            Add(invalidIntervals); return false;
        }
        const auto wall = end.wall - begin.wall, cpu = end.cpu - begin.cpu;
        if (!observed || begin.wall < firstWall) firstWall = begin.wall;
        if (!observed || end.wall > lastWall) lastWall = end.wall;
        observed = true;
        phases[index].Observe(wall, cpu);
        if (index >= 3) return true;
        auto slot = static_cast<unsigned>(((object >> 2) ^ (object >> 10)) % TargetCapacity);
        for (unsigned probe = 0; probe != TargetCapacity; ++probe) {
            auto& target = targets[(slot + probe) % TargetCapacity];
            if (!target.used) { target.used = true; target.target = object; }
            if (target.target == object) { target.phases[index].Observe(wall, cpu); return true; }
        }
        // Phase totals still include calls whose target does not fit the table.
        Add(targetOverflow);
        return true;
    }
};
struct RateLimit {
    std::uint64_t second {};
    unsigned inSecond {}, emitted {};
    std::uint64_t droppedRate {}, droppedLifetime {};
    bool Admit(std::uint64_t wallNs) noexcept {
        if (emitted >= DetailPerThread) { Add(droppedLifetime); return false; }
        const auto nowSecond = wallNs / 1000000000ULL;
        // A clock regression must not reset the rate window.
        if (nowSecond > second) { second = nowSecond; inSecond = 0; }
        if (inSecond >= DetailPerSecond) { Add(droppedRate); return false; }
        ++inSecond; ++emitted;
        return true;
    }
};
struct ThreadData {
    Aggregates aggregate;
    RateLimit rate;
    std::uint64_t tid {}, pid {}, sequence {}, lastReport {},
        droppedEvents {}, droppedSpans {}, writeFailures {}, reportSuppressed {}, frameOverflow {};
    unsigned reports {};
    bool finalReported {};
};
inline thread_local ThreadData local;
inline void Identity() noexcept {
    PreserveErrno preserve;
    if (local.tid != 0) return;
#if defined(__APPLE__) || defined(__linux__)
    local.pid = static_cast<std::uint64_t>(getpid());
#endif
#if defined(__APPLE__)
    if (pthread_threadid_np(nullptr, &local.tid) != 0) local.tid = 0;
#elif defined(__linux__)
    const auto tid = syscall(SYS_gettid);
    if (tid > 0) local.tid = static_cast<std::uint64_t>(tid);
#endif
}
inline void Write(const char* line, int length, unsigned capacity) noexcept {
    PreserveErrno preserve;
#if defined(__ANDROID__)
    const bool stderrSelected = AndroidStderrSelected();
#endif
    if (length <= 0 || static_cast<unsigned>(length) >= capacity ||
        static_cast<unsigned>(length) >= RecordCapacity) { Add(local.writeFailures); return; }
#if defined(__ANDROID__)
    if (!stderrSelected) {
        if (__android_log_write(ANDROID_LOG_INFO, "LumenCriticalPath", line) < 0) Add(local.writeFailures);
        return;
    }
#endif
#if defined(__APPLE__) || defined(__linux__)
    unsigned written = 0;
    while (written != static_cast<unsigned>(length)) {
        const auto result = ::write(STDERR_FILENO, line + written, static_cast<unsigned>(length) - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) { Add(local.writeFailures); return; }
        written += static_cast<unsigned>(result);
    }
#else
    Add(local.writeFailures);
#endif
}
inline void Completed(Reason reason, std::uint64_t object, std::uint64_t dependency,
    std::uint64_t value, Stamp begin, Stamp end, std::uint64_t frameBegin, bool event,
    std::uint64_t frameEvent = 0) noexcept {
    PreserveErrno preserve;
    const auto index = static_cast<unsigned>(reason);
    if (index >= ReasonNames.size() || !begin.valid || !end.valid ||
        end.wall < begin.wall || end.cpu < begin.cpu) return;
    if (!local.rate.Admit(end.wall)) {
        Add(event ? local.droppedEvents : local.droppedSpans);
        return;
    }
    Identity();
    Add(local.sequence);
    char line[RecordCapacity];
    const auto length = std::snprintf(line, sizeof(line),
        "LUMEN_CRITICAL_PATH_COMPLETE_V1 pid=%llu tid=%llu sequence=%llu kind=%s reason=%s "
        "object=%llu dependency=%llu value=%llu start_ns=%llu end_ns=%llu wall_ns=%llu "
        "thread_cpu_start_ns=%llu thread_cpu_end_ns=%llu thread_cpu_ns=%llu "
        "frame_hint_start=%llu frame_hint_end=%llu frame_event_id=%llu frame_hint_noncausal=1 inclusive=1 "
        "arming_deferred=%u armed_ns=%llu "
        "completed_only=1 census_complete=0 long_threshold_ns=%llu detail_per_second=%u "
        "detail_per_thread=%u dropped_spans=%llu dropped_events=%llu dropped_rate=%llu "
        "dropped_lifetime=%llu diagnostic_only=1\n",
        static_cast<unsigned long long>(local.pid), static_cast<unsigned long long>(local.tid),
        static_cast<unsigned long long>(local.sequence), event ? "event" : "span", ReasonNames[index],
        static_cast<unsigned long long>(object), static_cast<unsigned long long>(dependency),
        static_cast<unsigned long long>(value), static_cast<unsigned long long>(begin.wall),
        static_cast<unsigned long long>(end.wall), static_cast<unsigned long long>(end.wall - begin.wall),
        static_cast<unsigned long long>(begin.cpu), static_cast<unsigned long long>(end.cpu),
        static_cast<unsigned long long>(end.cpu - begin.cpu), static_cast<unsigned long long>(frameBegin),
        static_cast<unsigned long long>(currentFrame.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(frameEvent),
        Config().deferred ? 1U : 0U, static_cast<unsigned long long>(armedAtNs.load(std::memory_order_acquire)),
        static_cast<unsigned long long>(LongSpanNs), DetailPerSecond, DetailPerThread,
        static_cast<unsigned long long>(local.droppedSpans), static_cast<unsigned long long>(local.droppedEvents),
        static_cast<unsigned long long>(local.rate.droppedRate), static_cast<unsigned long long>(local.rate.droppedLifetime));
    Write(line, length, sizeof(line));
}
inline void MetricLine(const char* kind, unsigned index, std::uint64_t target,
    const Metrics& metric) noexcept {
    PreserveErrno preserve;
    char line[RecordCapacity];
    const auto& h = metric.histogram;
    const auto length = std::snprintf(line, sizeof(line),
        "LUMEN_CRITICAL_PATH_TOTAL_V1 pid=%llu tid=%llu report=%u kind=%s reason=%s target=%llu "
        "calls=%llu thread_wall_ns=%llu thread_cpu_ns=%llu max_wall_ns=%llu "
        "hist_lt1us=%llu hist_lt10us=%llu hist_lt100us=%llu hist_lt1ms=%llu "
        "hist_lt10ms=%llu hist_lt100ms=%llu hist_lt1s=%llu hist_ge1s=%llu "
        "counter_overflow=%llu cumulative=1 inclusive=1 thread_time_sum=1 census_complete=0 diagnostic_only=1\n",
        static_cast<unsigned long long>(local.pid), static_cast<unsigned long long>(local.tid), local.reports,
        kind, ReasonNames[index], static_cast<unsigned long long>(target),
        static_cast<unsigned long long>(metric.calls), static_cast<unsigned long long>(metric.wall),
        static_cast<unsigned long long>(metric.cpu), static_cast<unsigned long long>(metric.maxWall),
        static_cast<unsigned long long>(h[0]), static_cast<unsigned long long>(h[1]),
        static_cast<unsigned long long>(h[2]), static_cast<unsigned long long>(h[3]),
        static_cast<unsigned long long>(h[4]), static_cast<unsigned long long>(h[5]),
        static_cast<unsigned long long>(h[6]), static_cast<unsigned long long>(h[7]),
        static_cast<unsigned long long>(metric.overflow));
    Write(line, length, sizeof(line));
}
inline void ReportAt(Stamp now, bool final) noexcept {
    PreserveErrno preserve;
    if (!now.valid) { Add(local.aggregate.invalidIntervals); return; }
    if (local.lastReport == 0) local.lastReport = now.wall;
    if (!final && (now.wall < local.lastReport || now.wall - local.lastReport < ReportPeriodNs)) return;
    local.lastReport = now.wall;
    if ((final && local.finalReported) || (!final && local.reports >= ReportsPerThread)) {
        Add(local.reportSuppressed); return;
    }
    if (final) local.finalReported = true;
    Identity();
    ++local.reports;
    char line[RecordCapacity];
    const auto length = std::snprintf(line, sizeof(line),
        "LUMEN_CRITICAL_PATH_SUMMARY_V1 pid=%llu tid=%llu report=%u monotonic_ns=%llu "
        "aggregation_start_ns=%llu aggregation_end_ns=%llu arming_deferred=%u armed_ns=%llu "
        "target_capacity=%u target_overflow_calls=%llu invalid_intervals=%llu invalid_reasons=%llu "
        "detail_emitted=%u dropped_spans=%llu dropped_events=%llu dropped_rate=%llu dropped_lifetime=%llu "
        "write_failures=%llu reports_suppressed=%llu frame_overflow=%llu report_period_ns=%llu "
        "reports_per_thread=%u final_report_extra=1 final=%u completed_only=1 census_complete=0 frame_hint_noncausal=1 diagnostic_only=1\n",
        static_cast<unsigned long long>(local.pid), static_cast<unsigned long long>(local.tid), local.reports,
        static_cast<unsigned long long>(now.wall), static_cast<unsigned long long>(local.aggregate.firstWall),
        static_cast<unsigned long long>(local.aggregate.lastWall), Config().deferred ? 1U : 0U,
        static_cast<unsigned long long>(armedAtNs.load(std::memory_order_acquire)), TargetCapacity,
        static_cast<unsigned long long>(local.aggregate.targetOverflow),
        static_cast<unsigned long long>(local.aggregate.invalidIntervals),
        static_cast<unsigned long long>(local.aggregate.invalidReasons), local.rate.emitted,
        static_cast<unsigned long long>(local.droppedSpans), static_cast<unsigned long long>(local.droppedEvents),
        static_cast<unsigned long long>(local.rate.droppedRate), static_cast<unsigned long long>(local.rate.droppedLifetime),
        static_cast<unsigned long long>(local.writeFailures), static_cast<unsigned long long>(local.reportSuppressed),
        static_cast<unsigned long long>(local.frameOverflow), static_cast<unsigned long long>(ReportPeriodNs),
        ReportsPerThread, final ? 1U : 0U);
    Write(line, length, sizeof(line));
    for (unsigned i = 0; i != local.aggregate.phases.size(); ++i)
        if (local.aggregate.phases[i].calls != 0) MetricLine("reason", i, 0, local.aggregate.phases[i]);
    for (const auto& target : local.aggregate.targets)
        if (target.used) for (unsigned i = 0; i != target.phases.size(); ++i)
            if (target.phases[i].calls != 0) MetricLine("hle_target", i, target.target, target.phases[i]);
}
} // namespace Detail

inline void SetFrame(std::uint64_t frame) noexcept {
    if (Enabled()) currentFrame.store(frame, std::memory_order_relaxed);
}
inline void FlushThread(bool final = false) noexcept {
    if (Enabled()) Detail::ReportAt(Now(), final);
}
inline void Event(Reason reason, std::uint64_t object = 0,
    std::uint64_t dependency = 0, std::uint64_t value = 0) noexcept {
    if (!Enabled()) return;
    const auto now = Now();
    if (Detail::local.aggregate.Observe(reason, object, now, now))
        Detail::Completed(reason, object, dependency, value, now, now,
            currentFrame.load(std::memory_order_relaxed), true);
    Detail::ReportAt(now, false);
}
inline std::uint64_t Frame(Reason reason, std::uint64_t object = 0,
    std::uint64_t dependency = 0, std::uint64_t value = 0) noexcept {
    if (!Enabled()) return 0;
    auto previous = nextFrame.load(std::memory_order_relaxed);
    do {
        if (previous == UINT64_MAX) { Detail::Add(Detail::local.frameOverflow); return 0; }
    } while (!nextFrame.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed));
    const auto frame = previous + 1;
    SetFrame(frame);
    const auto now = Now();
    if (Detail::local.aggregate.Observe(reason, object, now, now))
        Detail::Completed(reason, object, dependency, value, now, now, frame, true, frame);
    Detail::ReportAt(now, false);
    return frame;
}
class Scope {
public:
    explicit Scope(Reason reason, std::uint64_t object = 0, std::uint64_t dependency = 0) noexcept
        : reason_(reason), object_(object), dependency_(dependency) {
        if (!Enabled()) return;
        begin_ = Now();
        frame_ = currentFrame.load(std::memory_order_relaxed);
        active_ = true;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() { End(); }
    void End() noexcept {
        if (!active_) return;
        active_ = false;
        const auto end = Now();
        if (Detail::local.aggregate.Observe(reason_, object_, begin_, end) && end.wall - begin_.wall >= LongSpanNs)
            Detail::Completed(reason_, object_, dependency_, 0, begin_, end, frame_, false);
        Detail::ReportAt(end, false);
    }
private:
    Reason reason_;
    std::uint64_t object_ {}, dependency_ {}, frame_ {};
    Stamp begin_;
    bool active_ {};
};

} // namespace Lumen::CriticalPath
