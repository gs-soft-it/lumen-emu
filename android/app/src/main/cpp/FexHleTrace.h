// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "FexPerformanceTrace.h"

namespace Lumen::Performance {

// Owner-thread diagnostics only. No registry lock, allocation, or shared atomic
// is added to a host call. A nonlocal FEX stop leaves an explicit pending call;
// it never relies on unwinding a C++ scope. Timings include nested work/waits
// and are not exclusive critical-path time or frame-aligned measurements.
class HleWindow {
public:
    static constexpr unsigned Capacity = 64;
    static constexpr unsigned Overflow = Capacity;
    static constexpr std::uint64_t IntervalNs = 2000000000ULL;
    enum class Phase : unsigned { Admission, Body, Readmission };
    enum class Admission : unsigned { Ordinary, Short, ShortFallback };
    struct Time { std::uint64_t wall {}, cpu {}; };
    struct Target {
        std::uint64_t address {}, calls {}, finished {}, failed {};
        std::uint64_t short_calls {}, short_fallback {}, ordinary_calls {}, clock_failures {};
        std::uint64_t short_completed {}, short_demoted {};
        std::array<Time, 3> time {};
    };

    unsigned Begin(std::uint64_t target, Stamp now) noexcept {
        intervals_ = {};
        if (attempts_ == 0) start_ = now;
        ++attempts_;
        auto index = static_cast<unsigned>(((target >> 2) ^ (target >> 10)) & (Capacity - 1));
        unsigned slot = Overflow;
        if (target != 0) {
            for (unsigned probe = 0; probe != Capacity; ++probe) {
                auto& entry = targets_[index];
                if (entry.address == 0 || entry.address == target) {
                    entry.address = target;
                    slot = index;
                    break;
                }
                index = (index + 1) & (Capacity - 1);
            }
        }
        ++targets_[slot].calls;
        return slot;
    }
    void Admit(unsigned slot, Admission admission) noexcept {
        auto& entry = targets_[slot];
        if (admission == Admission::Short) ++entry.short_calls;
        else {
            ++entry.ordinary_calls;
            if (admission == Admission::ShortFallback) ++entry.short_fallback;
        }
    }
    void Demote(unsigned slot) noexcept { ++targets_[slot].short_demoted; }
    void Charge(unsigned slot, Phase phase, Stamp start, Stamp end) noexcept {
        if (!start_.valid && start.valid) start_ = start;
        auto& entry = targets_[slot];
        if (!start.valid || !end.valid || end.wall < start.wall || end.cpu < start.cpu) {
            ++entry.clock_failures;
            return;
        }
        auto& time = entry.time[static_cast<unsigned>(phase)];
        time.wall += end.wall - start.wall;
        time.cpu += end.cpu - start.cpu;
    }
    void End(unsigned slot, bool failed, bool completed_short = false) noexcept {
        ++finished_;
        auto& entry = targets_[slot];
        ++entry.finished;
        if (completed_short) ++entry.short_completed;
        if (failed) { ++entry.failed; ++failed_; }
    }
    bool Due(Stamp now) const noexcept {
        return attempts_ != 0 && finished_ == attempts_ && now.valid && start_.valid &&
            now.wall >= start_.wall && now.wall - start_.wall >= IntervalNs;
    }
    const Target& Entry(unsigned slot) const noexcept { return targets_[slot]; }
    Interval* ActiveInterval(Phase phase) noexcept {
        return &intervals_[static_cast<unsigned>(phase)];
    }
    std::uint64_t Attempts() const noexcept { return attempts_; }
    std::uint64_t Pending() const noexcept { return attempts_ - finished_; }

    void Report(std::uint64_t run, std::uint64_t thread, Stamp end, bool final) noexcept {
        if (attempts_ == 0 || (!final && !Due(end))) return;
        const auto window = ++window_;
#if defined(__APPLE__) || defined(__linux__)
        const auto process = static_cast<unsigned long long>(getpid());
#else
        const auto process = 0ULL;
#endif
        std::fprintf(stderr, "FEX_HLE_WINDOW_V1 pid=%llu run=%llu thread=%llu window=%llu "
            "start_ns=%llu end_ns=%llu attempts=%llu finished=%llu failed=%llu pending=%llu "
            "overflow_calls=%llu clock_valid=%u final=%u inclusive_thread_time=1 frame_aligned=0\n",
            process, U(run), U(thread), U(window), U(start_.wall), U(end.wall),
            U(attempts_), U(finished_), U(failed_), U(Pending()), U(targets_[Overflow].calls),
            start_.valid && end.valid && end.wall >= start_.wall ? 1U : 0U, final ? 1U : 0U);
        for (const auto& entry : targets_) {
            if (entry.calls == 0) continue;
            std::fprintf(stderr, "FEX_HLE_TARGET_V1 pid=%llu run=%llu thread=%llu window=%llu "
                "start_ns=%llu end_ns=%llu target=%#llx calls=%llu finished=%llu failed=%llu "
                "short_calls=%llu short_fallback=%llu ordinary_calls=%llu "
                "short_completed=%llu short_demoted=%llu short_calls_are_entries=1 "
                "admission_wall_ns=%llu admission_cpu_ns=%llu body_wall_ns=%llu body_cpu_ns=%llu "
                "readmission_wall_ns=%llu readmission_cpu_ns=%llu clock_failures=%llu "
                "inclusive_thread_time=1 frame_aligned=0\n",
                process, U(run), U(thread), U(window), U(start_.wall), U(end.wall), U(entry.address),
                U(entry.calls), U(entry.finished), U(entry.failed), U(entry.short_calls),
                U(entry.short_fallback), U(entry.ordinary_calls), U(entry.short_completed),
                U(entry.short_demoted), U(entry.time[0].wall),
                U(entry.time[0].cpu), U(entry.time[1].wall), U(entry.time[1].cpu),
                U(entry.time[2].wall), U(entry.time[2].cpu), U(entry.clock_failures));
        }
        targets_ = {};
        start_ = {};
        attempts_ = finished_ = failed_ = 0;
    }
private:
    static unsigned long long U(std::uint64_t value) noexcept { return value; }
    std::array<Target, Capacity + 1> targets_ {};
    std::array<Interval, 3> intervals_ {};
    Stamp start_ {};
    std::uint64_t attempts_ {}, finished_ {}, failed_ {}, window_ {};
};

} // namespace Lumen::Performance
