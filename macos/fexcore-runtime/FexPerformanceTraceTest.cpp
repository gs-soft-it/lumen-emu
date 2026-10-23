// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#include "FexPerformanceTrace.h"
#include "FexHleTrace.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <thread>

using namespace Lumen::Performance;

int main() {
    unsigned checks = 0;
    const auto require = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
    };
    const auto stamp = [](std::uint64_t n) { return Stamp {n * 10, n, true}; };
    Ledger accounting;
    const auto root = accounting.Begin(Phase::ExecuteEnvelope, stamp(1));
    auto delta = accounting.Charge(stamp(3));
    require(delta.valid && delta.wall == 20 && delta.cpu == 2, "root exclusive interval");
    const auto hle = accounting.Begin(Phase::HostCall, stamp(3));
    delta = accounting.Charge(stamp(5));
    require(delta.phase == Phase::HostCall && delta.wall == 20, "HLE exclusive interval");
    const auto child = accounting.Begin(Phase::ExecuteEnvelope, stamp(5));
    delta = accounting.End(child, stamp(8));
    require(delta.phase == Phase::ExecuteEnvelope && delta.wall == 30, "nested root excludes parent HLE");
    delta = accounting.End(hle, stamp(10));
    require(delta.phase == Phase::HostCall && delta.wall == 20, "parent restarts after child");
    delta = accounting.End(root, stamp(11));
    require(delta.phase == Phase::ExecuteEnvelope && delta.wall == 10 && accounting.Depth() == 0,
        "sum of exclusive intervals equals outer elapsed");

    Ledger helperAccounting;
    const auto helperRoot = helperAccounting.Begin(Phase::ExecuteEnvelope, stamp(1));
    auto executionBeforeHelper = helperAccounting.Charge(stamp(3));
    const auto helper = helperAccounting.Begin(Phase::DirectUnaligned, stamp(3));
    delta = helperAccounting.End(helper, stamp(8));
    require(delta.phase == Phase::DirectUnaligned && delta.wall == 50 && delta.cpu == 5,
        "direct helper has its own wall and CPU interval");
    delta = helperAccounting.End(helperRoot, stamp(10));
    require(executionBeforeHelper.wall + delta.wall == 40 &&
        executionBeforeHelper.cpu + delta.cpu == 4 && helperAccounting.Depth() == 0,
        "execution envelope excludes direct helper time");

    const auto forceRoot = accounting.Begin(Phase::ExecuteEnvelope, stamp(12));
    accounting.Charge(stamp(13));
    const auto abandoned = accounting.Begin(Phase::HostCall, stamp(13));
    delta = accounting.End(forceRoot, stamp(15));
    require(delta.phase == Phase::HostCall && delta.wall == 20 && accounting.Depth() == 0 &&
        accounting.Abandoned() == 1, "nonlocal FEX stop recovers abandoned scope");
    require(!accounting.End(abandoned, stamp(16)).valid && accounting.Depth() == 0,
        "late destructor cannot revive old root");
    const auto nextRoot = accounting.Begin(Phase::ExecuteEnvelope, stamp(17));
    require(accounting.End(nextRoot, stamp(19)).wall == 20 && accounting.Depth() == 0,
        "success after denied root starts balanced");
    const auto invalidClock = accounting.Begin(Phase::HostWait, stamp(20));
    require(!accounting.End(invalidClock, {0, 0, false}).valid,
        "failed clocks do not invent an interval");
    const auto regressingClock = accounting.Begin(Phase::Readmission, stamp(22));
    require(!accounting.End(regressingClock, stamp(21)).valid,
        "regressing clocks do not underflow");

    std::array<std::uint64_t, Ledger::Capacity> tokens;
    for (auto& token : tokens) token = accounting.Begin(Phase::HostCall, stamp(23));
    require(accounting.Begin(Phase::HostCall, stamp(24)) == 0 && accounting.Overflow() == 1,
        "bounded scope stack records overflow");
    accounting.End(tokens.front(), stamp(25));
    require(accounting.Depth() == 0 && accounting.Abandoned() == Ledger::Capacity,
        "outer unwind recovers bounded stack");

    const bool enabled = Enabled();
    const auto directPhase = static_cast<unsigned>(Phase::DirectUnaligned);
    const auto directEvents = data.phases[directPhase].events.load();
    const auto directAbandoned = data.abandoned.load();
    {
        Scope execution(Phase::ExecuteEnvelope);
        std::optional<Scope> helperTiming;
        if (enabled) helperTiming.emplace(Phase::DirectUnaligned);
        execution.End(); // Same ledger recovery as a nonlocal FEX stop.
        require(ledger.Depth() == 0 && data.openScopes.load() == 0,
            "nonlocal direct helper exit drains nested scope census");
    }
    require(data.phases[directPhase].events.load() == directEvents + (enabled ? 1U : 0U) &&
        data.abandoned.load() == directAbandoned + (enabled ? 1U : 0U) &&
        ledger.Depth() == 0 && data.openScopes.load() == 0,
        "direct helper OFF is inert and a late helper destructor cannot revive the ledger");
    HleWindow hleWindow;
    const auto firstHle = hleWindow.Begin(0x1000, stamp(1));
    hleWindow.Admit(firstHle, HleWindow::Admission::Short);
    hleWindow.Charge(firstHle, HleWindow::Phase::Admission, stamp(1), stamp(3));
    hleWindow.Charge(firstHle, HleWindow::Phase::Body, stamp(3), stamp(7));
    hleWindow.Charge(firstHle, HleWindow::Phase::Readmission, stamp(7), stamp(9));
    hleWindow.End(firstHle, false, true);
    require(hleWindow.Entry(firstHle).calls == 1 && hleWindow.Entry(firstHle).finished == 1 &&
        hleWindow.Entry(firstHle).short_calls == 1 && hleWindow.Entry(firstHle).short_completed == 1 &&
        hleWindow.Entry(firstHle).short_demoted == 0 && hleWindow.Entry(firstHle).time[1].wall == 40 &&
        hleWindow.Entry(firstHle).time[1].cpu == 4 && hleWindow.Pending() == 0,
        "target census retains inclusive timing and admitted category");
    const auto repeatedHle = hleWindow.Begin(0x1000, stamp(10));
    hleWindow.Admit(repeatedHle, HleWindow::Admission::ShortFallback);
    hleWindow.Charge(repeatedHle, HleWindow::Phase::Admission, stamp(10), stamp(9));
    hleWindow.End(repeatedHle, true);
    require(repeatedHle == firstHle && hleWindow.Entry(firstHle).short_fallback == 1 &&
        hleWindow.Entry(firstHle).ordinary_calls == 1 && hleWindow.Entry(firstHle).failed == 1 &&
        hleWindow.Entry(firstHle).clock_failures == 1 && hleWindow.Entry(firstHle).time[0].wall == 20,
        "failed fallback and invalid clocks do not fabricate target time");
    const auto demotedHle = hleWindow.Begin(0x1000, stamp(11));
    hleWindow.Admit(demotedHle, HleWindow::Admission::Short);
    hleWindow.Demote(demotedHle);
    hleWindow.End(demotedHle, false);
    require(hleWindow.Entry(firstHle).short_calls == 2 &&
        hleWindow.Entry(firstHle).short_completed == 1 && hleWindow.Entry(firstHle).short_demoted == 1,
        "short entry with ordinary body is never counted as completed short execution");
    for (unsigned i = 1; i != HleWindow::Capacity; ++i) {
        const auto slot = hleWindow.Begin(0x1000 + 4 * i, stamp(11));
        require(slot != HleWindow::Overflow, "bounded target collisions preserve distinct targets");
        hleWindow.Admit(slot, HleWindow::Admission::Ordinary);
        hleWindow.End(slot, false);
    }
    const auto overflowHle = hleWindow.Begin(0x100000, stamp(12));
    require(overflowHle == HleWindow::Overflow && hleWindow.Entry(overflowHle).address == 0 &&
        hleWindow.Entry(overflowHle).calls == 1 && hleWindow.Pending() == 1 &&
        !hleWindow.Due({HleWindow::IntervalNs + 100, 100, true}),
        "overflow is explicit and a nonlocal stop remains pending before final reporting");
    hleWindow.End(overflowHle, true);
    require(hleWindow.Due({HleWindow::IntervalNs + 100, 100, true}),
        "only a completed target window becomes reportable");
    Interval observedInterval;
    {
        Scope scope(Phase::HostCall, 0, &observedInterval);
    }
    require(enabled ? observedInterval.start.valid && observedInterval.end.valid &&
        observedInterval.end.wall >= observedInterval.start.wall : !observedInterval.start.valid,
        "target intervals reuse scope timestamps only with tracing enabled");
    require(!data.jitCacheHooksAvailable.load(), "cache hooks are unavailable until the adapter registers them");
    MarkJitCacheHooksAvailable();
    CacheStats(3, 11, 17, 2);
    JitCacheEvent(6, 5, 4096);
    JitCacheEvent(0, 99, 99);
    JitCacheEvent(static_cast<std::uint32_t>(JitCacheNames.size()) + 1, 99, 99);
    require(data.jitCacheHooksAvailable.load() == enabled,
        "cache availability is explicit and remains untouched while OFF");
    require(data.cacheMisses.load() == (enabled ? 3U : 0U) &&
        data.cacheReadLockTicks.load() == (enabled ? 11U : 0U) &&
        data.cacheWriteLockTicks.load() == (enabled ? 17U : 0U) &&
        data.diskMisses.load() == (enabled ? 2U : 0U),
        "existing SHM cache counters stay distinct and are gated while OFF");
    require(data.jitCache[5].events.load() == (enabled ? 1U : 0U) &&
        data.jitCache[5].value.load() == (enabled ? 5U : 0U) &&
        data.jitCache[5].detail.load() == (enabled ? 4096U : 0U),
        "invalidation events distinguish requests from blocks actually erased");
    const auto wallBefore = data.phases[static_cast<unsigned>(Phase::HostCall)].wall.load();
    {
        Scope timing(Phase::HostCall);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    const auto wallAfter = data.phases[static_cast<unsigned>(Phase::HostCall)].wall.load();
    require(enabled ? wallAfter > wallBefore : wallAfter == wallBefore,
        "selector keeps OFF counters untouched");
    require(ledger.Depth() == 0, "real scope balanced");
    std::thread peer([&] {
        Scope timing(Phase::ExecuteEnvelope);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    });
    peer.join();
    require(ledger.Depth() == 0, "peer has independent ledger");
    require(data.openScopes.load() == 0, "cross-thread completed scopes drain census");
    if (enabled) {
        Scope epoch(Phase::GpuEpoch, 1234);
        const auto gpuEpoch = data.activeGpuEpoch.load();
        require(gpuEpoch != 0,
            "GPU scope receives process sequence");
        if (TimelineEnabled()) {
            const auto ticket = timelineSequence.load();
            const auto& producer = timeline[(ticket - 1) % TimelineCapacity];
            require(producer.gpuEpoch.load() == gpuEpoch && producer.mutationEpoch.load() == 1234,
                "only producer publishes exact mutation correlation");
        }
        { Scope publication(Phase::GpuPublishTextures); }
        require(data.activeGpuEpoch.load() == gpuEpoch && data.openScopes.load() == 1,
            "nested publication preserves producer epoch and census");
        epoch.End();
        require(data.activeGpuEpoch.load() == 0 && data.openScopes.load() == 0,
            "completed epoch clears correlation without leaking scopes");
        AlignmentFault(false, 2, 8); AlignmentFault(true);
        DirectUnaligned(true, 8, true);
        require(data.faultFamilies[2].load() == 1 && data.faultWidths[8].load() == 1 &&
            data.directUnalignedCalls.load() == 1 && data.directUnalignedHandled.load() == 1 &&
            data.directUnalignedWrites.load() == 1, "signal and direct access counts remain distinct");
        if (TimelineEnabled()) {
            const auto beforeSequence = timelineSequence.load();
            std::array<std::thread, 4> producers;
            for (auto& producer : producers) producer = std::thread([] {
                for (unsigned i = 0; i != 2048; ++i)
                    Timeline(Phase::Readmission, i + 1, Now(), (i & 1) == 0, 7);
            });
            for (auto& producer : producers) producer.join();
            require(timelineSequence.load() == beforeSequence + 8192,
                "bounded concurrent timeline reserves every event without blocking");
            Report(true);
            require(timelineReported == timelineSequence.load() && timelineDropped.load() != 0,
                "overflow is explicit and snapshot cursor advances");
            require(data.nextThread.load() >= 5, "timeline identifies distinct producer threads");
            const auto delayed = timelineSequence.fetch_add(1) + 1;
            const auto droppedBefore = timelineDropped.load();
            Report(true);
            require(timelineReported == delayed && timelineDropped.load() == droppedBefore + 1,
                "in-flight reservation is counted once when snapshot passes it");
            const auto fresh = timelineSequence.fetch_add(TimelineCapacity) + TimelineCapacity;
            PublishTimeline(fresh, 99, Phase::Readmission, 2, Now(), true, 8);
            PublishTimeline(delayed, 98, Phase::HostCall, 1, Now(), true, 7);
            require(timeline[(fresh - 1) % TimelineCapacity].ticket.load() == fresh,
                "descheduled stale producer cannot overwrite newer generation");
            Report(true);
            const auto droppedAfter = timelineDropped.load();
            Report(true);
            require(timelineDropped.load() == droppedAfter,
                "repeated snapshots do not recount losses");
        }
    }
    std::printf("FEX_PERFORMANCE_TRACE_TEST checks=%u enabled=%u PASS\n", checks, enabled ? 1U : 0U);
}
