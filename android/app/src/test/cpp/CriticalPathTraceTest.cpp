// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#include "CriticalPathTrace.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#endif

using namespace Lumen::CriticalPath;

int main(int argc, char** argv) {
    unsigned checks = 0;
    const auto require = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { std::fprintf(stderr, "FAIL %s\n", name); std::exit(1); }
    };
    require(!SelectorEnabled(nullptr) && !SelectorEnabled("") && !SelectorEnabled("0") &&
        !SelectorEnabled("01") && !SelectorEnabled("true") && SelectorEnabled("1"), "strict OFF-default selector");

    const bool expectedStderr = SelectorEnabled(std::getenv("LUMEN_CRITICAL_PATH_STDERR"));
    errno = EDOM;
    require(Detail::AndroidStderrSelected() == expectedStderr && errno == EDOM,
        "Android sink selector exact and errno preserved");
#if defined(__APPLE__) || defined(__linux__)
    require(setenv("LUMEN_CRITICAL_PATH_STDERR", expectedStderr ? "0" : "1", 1) == 0,
        "sink selector mutation fixture");
    require(Detail::AndroidStderrSelected() == expectedStderr,
        "Android sink process latch survives environment mutation");

#if !defined(__ANDROID__)
    // Redirect one descriptor onto a temporary regular file. The production
    // launcher duplicates the same open file description onto fd 1 and fd 2;
    // each record then starts with one write rather than a stdio buffer flush.
    char sinkPath[] = "/tmp/lumen-critical-sink-XXXXXX";
    const int sink = mkstemp(sinkPath);
    const int savedStderr = dup(STDERR_FILENO);
    require(sink >= 0 && savedStderr >= 0 && unlink(sinkPath) == 0,
        "private regular-file sink fixture");
    require(dup2(sink, STDERR_FILENO) == STDERR_FILENO, "fd 2 shares sink open file description");
    const auto failuresBefore = Detail::local.writeFailures;
    errno = EDOM;
    Detail::Write("record-one\n", 11, 12);
    require(errno == EDOM && Detail::local.writeFailures == failuresBefore,
        "complete record write preserves errno");
    require(write(sink, "shared-offset\n", 14) == 14, "shared descriptor fixture write");
    errno = EDOM;
    Detail::Write("record-two\n", 11, 12);
    require(errno == EDOM && Detail::local.writeFailures == failuresBefore,
        "second record write preserves errno");
    require(lseek(sink, 0, SEEK_SET) == 0, "rewind regular-file sink");
    char records[64] {};
    const auto bytes = read(sink, records, sizeof(records));
    require(bytes == 36 && std::strcmp(records, "record-one\nshared-offset\nrecord-two\n") == 0,
        "records append at shared file offset without overwrite");
    errno = EDOM;
    Detail::Write("", 0, 1);
    Detail::Write("bounded", 7, 7);
    Detail::Write("bounded", RecordCapacity, RecordCapacity + 1);
    require(errno == EDOM && Detail::local.writeFailures == failuresBefore + 3,
        "empty truncated and globally oversized records counted without write");
    require(close(STDERR_FILENO) == 0, "closed fd 2 negative fixture");
    errno = EDOM;
    Detail::Write("failed\n", 7, 8);
    const bool failurePreserved = errno == EDOM && Detail::local.writeFailures == failuresBefore + 4;
    const bool restored = dup2(savedStderr, STDERR_FILENO) == STDERR_FILENO;
    require(restored && close(savedStderr) == 0 && close(sink) == 0, "restore stderr after sink fixture");
    require(failurePreserved, "failed sink write counted and errno preserved");
#endif
#endif

    Detail::Aggregates aggregate;
    const Stamp begin {100, 20, true}, end {2100, 50, true};
    require(aggregate.Observe(Reason::HleCall, 0x1000, begin, end), "completed valid interval");
    auto& phase = aggregate.phases[static_cast<unsigned>(Reason::HleCall)];
    require(phase.calls == 1 && phase.wall == 2000 && phase.cpu == 30 && phase.histogram[1] == 1,
        "wall and CPU kept separately with inclusive histogram");
    require(aggregate.observed && aggregate.firstWall == 100 && aggregate.lastWall == 2100,
        "cumulative totals expose actual observation bounds");
    require(!aggregate.Observe(Reason::HleCall, 0x1000, end, begin) && phase.calls == 1 &&
        aggregate.invalidIntervals == 1, "regressing wall does not charge");
    require(!aggregate.Observe(Reason::HleCall, 0x1000, begin, {2200, 19, true}) && phase.calls == 1,
        "regressing CPU does not charge");
    require(!aggregate.Observe(Reason::HleCall, 0x1000, {}, end), "clock failure does not charge");
    require(!aggregate.Observe(Reason::Count, 0, begin, end) && aggregate.invalidReasons == 1,
        "invalid reason bounded before indexing");
    require(Detail::Bucket(0) == 0 && Detail::Bucket(999) == 0 && Detail::Bucket(1000) == 1 &&
        Detail::Bucket(999999) == 3 && Detail::Bucket(1000000) == 4 &&
        Detail::Bucket(UINT64_MAX) == 7, "histogram exact edges");

    // Fill using deliberately colliding target hashes. An overflow target never
    // replaces an existing target or disappears from the authoritative phase total.
    for (unsigned i = 1; i != TargetCapacity; ++i)
        require(aggregate.Observe(Reason::HleCall, 0x1000 + i * 0x100000, begin, end), "bounded target insertion");
    require(aggregate.Observe(Reason::HleCall, 0x999, begin, end) && aggregate.targetOverflow == 1 &&
        phase.calls == TargetCapacity + 1, "target overflow retains reason total");
    require(aggregate.Observe(Reason::HleReadmission, 0x1000, begin, end) &&
        aggregate.targetOverflow == 1, "existing target remains reachable after overflow");
    unsigned matching = 0;
    for (const auto& target : aggregate.targets)
        if (target.used && target.target == 0x1000) {
            ++matching;
            require(target.phases[1].calls == 1 && target.phases[2].calls == 1,
                "per-target phases independent");
        }
    require(matching == 1, "collision probing retains unique identities");

    Detail::Metrics saturated;
    saturated.calls = UINT64_MAX;
    saturated.wall = UINT64_MAX - 1;
    saturated.Observe(2, 0);
    require(saturated.calls == UINT64_MAX && saturated.wall == UINT64_MAX && saturated.overflow == 2,
        "overflow saturates rather than wrapping");

    Detail::RateLimit rate;
    for (unsigned i = 0; i != DetailPerSecond; ++i) require(rate.Admit(2000000000ULL), "rate capacity");
    require(!rate.Admit(2000000000ULL) && rate.droppedRate == 1, "per-second bound counts loss");
    require(!rate.Admit(1000000000ULL) && rate.droppedRate == 2, "regression cannot reopen rate window");
    require(rate.Admit(3000000000ULL), "next second reopens window");
    rate.emitted = DetailPerThread;
    require(!rate.Admit(4000000000ULL) && rate.droppedLifetime == 1, "lifetime bound counts loss");

    Detail::PollCadence cadence;
    require(cadence.Eligible(), "first arm query polls immediately");
    for (unsigned i = 1; i != 1024; ++i) require(!cadence.Eligible(), "pre-arm clock checks amortized");
    require(cadence.Eligible(), "1024th subsequent query can check arm clock");
    Detail::PollGate gate;
    require(gate.TryPoll(100) && !gate.TryPoll(100) && !gate.TryPoll(1000000099),
        "one global stat admission per second");
    require(gate.TryPoll(1000000100), "next poll deadline inclusive");
    Detail::PollGate concurrentGate;
    std::atomic<unsigned> admitted {};
    std::array<std::thread, 8> pollers;
    for (auto& poller : pollers) poller = std::thread([&] {
        if (concurrentGate.TryPoll(100)) admitted.fetch_add(1);
    });
    for (auto& poller : pollers) poller.join();
    require(admitted.load() == 1, "competing threads have one poll winner");
    Detail::PollGate saturatedGate;
    require(saturatedGate.TryPoll(UINT64_MAX) && !saturatedGate.TryPoll(UINT64_MAX),
        "poll deadline overflow cannot reopen gate");

#if defined(__APPLE__) || defined(__linux__)
    if (argc == 2 && std::strcmp(argv[1], "--deferred") == 0) {
        char directory[] = "/tmp/lumen-critical-arm-XXXXXX";
        require(mkdtemp(directory) != nullptr, "private arming fixture directory");
        char path[256];
        require(std::snprintf(path, sizeof(path), "%s/token", directory) < static_cast<int>(sizeof(path)),
            "arming fixture path bounded");
        require(setenv("LUMEN_CRITICAL_PATH_TRACE", "1", 1) == 0 &&
            setenv("LUMEN_CRITICAL_PATH_ARM_FILE", path, 1) == 0, "deferred selector prepared before first query");
        errno = EDOM;
        require(!Enabled() && errno == EDOM, "missing arm file denies and preserves errno");
        Scope preArm(Reason::HleCall, 999);
        for (unsigned i = 0; i != 2048; ++i) {
            Scope before(Reason::HleCall, i + 1);
            Event(Reason::GpuSubmissionAccepted, i + 1);
            require(Frame(Reason::AcceptedPresent, i + 1) == 0, "pre-arm frame does not consume sequence");
            SetFrame(i + 1);
            FlushThread();
        }
        require(Detail::local.aggregate.phases[1].calls == 0 &&
            Detail::local.aggregate.targetOverflow == 0 && !Detail::local.aggregate.observed &&
            Detail::local.rate.emitted == 0 && Detail::local.reports == 0 &&
            Detail::local.sequence == 0 && currentFrame.load() == 0 && nextFrame.load() == 0,
            "pre-arm leaves all diagnostic budgets totals and frames untouched");
        require(mkdir(path, 0700) == 0, "directory token negative fixture");
        std::this_thread::sleep_for(std::chrono::milliseconds(1050));
        for (unsigned i = 0; i != 2048; ++i) require(!Enabled(), "directory is not a regular arm file");
        require(rmdir(path) == 0, "remove negative token");
        const auto file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        require(file >= 0 && close(file) == 0, "regular empty token fixture");
        std::this_thread::sleep_for(std::chrono::milliseconds(1050));
        bool armed = false;
        errno = EDOM;
        for (unsigned i = 0; i != 2048 && !armed; ++i) armed = Enabled();
        require(armed && errno == EDOM && Detail::armedAtNs.load() != 0, "positive regular stat arms once");
        preArm.End();
        require(Detail::local.aggregate.phases[1].calls == 0, "pre-arm scope ending after arm stays excluded");
        const auto armedStamp = Detail::armedAtNs.load();
        const auto pollDeadline = Detail::armPollGate.next.load();
        require(unlink(path) == 0 && rmdir(directory) == 0, "remove successful token after activation");
        require(setenv("LUMEN_CRITICAL_PATH_TRACE", "0", 1) == 0 &&
            setenv("LUMEN_CRITICAL_PATH_ARM_FILE", "/does/not/exist", 1) == 0, "mutate environment after latch");
        for (unsigned i = 0; i != 4096; ++i) require(Enabled(), "armed latch survives file removal and environment edits");
        require(Detail::armedAtNs.load() == armedStamp && Detail::armPollGate.next.load() == pollDeadline,
            "no arming poll work after activation");
    }
#else
    (void)argc; (void)argv;
#endif

    const bool enabled = Enabled();
    const auto originalCalls = Detail::local.aggregate.phases[1].calls;
    {
        Scope shortSpan(Reason::HleCall, 100);
        shortSpan.End(); shortSpan.End();
    }
    require(Detail::local.aggregate.phases[1].calls == originalCalls + (enabled ? 1 : 0),
        "OFF counters untouched and End idempotent");
    const auto detailsBefore = Detail::local.rate.emitted;
    errno = EDOM;
    {
        Scope wait(Reason::GpuFenceWait, 7, 123);
        require(errno == EDOM, "scope begin preserves errno");
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        errno = EDOM;
        wait.End();
    }
    require(Detail::local.rate.emitted == detailsBefore + (enabled ? 1 : 0),
        "long complete span emitted once");
    require(errno == EDOM, "scope clocks and emission preserve errno");
    if (enabled) {
        const auto& wait = Detail::local.aggregate.phases[static_cast<unsigned>(Reason::GpuFenceWait)];
        require(wait.wall >= LongSpanNs && wait.cpu < wait.wall, "native wait wall exceeds thread CPU");
    }

    SetFrame(42);
    Event(Reason::GpuSubmissionAccepted, 123, 0, 8);
    require(errno == EDOM, "instant event preserves errno");
    require(currentFrame.load() == (enabled ? 42 : 0), "ordinary event does not advance frame hint");
    const auto frame = Frame(Reason::AcceptedPresent, 123, 0, 8);
    require(frame == (enabled ? 1 : 0) && currentFrame.load() == frame,
        "accepted frame explicit sequence distinct from caller hint");
    std::uint64_t peerTid = 0;
    std::thread peer([&] {
        Scope timing(Reason::HleAdmission, 456);
        timing.End();
        if (enabled) {
            Detail::Identity(); peerTid = Detail::local.tid;
            require(Detail::local.aggregate.phases[0].calls == 1 &&
                Detail::local.aggregate.phases[1].calls == 0, "peer totals isolated");
        }
    });
    peer.join();
    if (enabled) {
        Detail::Identity();
        require(peerTid != 0 && peerTid != Detail::local.tid, "native thread identity differs");
        nextFrame.store(UINT64_MAX);
        require(Frame(Reason::AcceptedPresent) == 0 && nextFrame.load() == UINT64_MAX &&
            Detail::local.frameOverflow == 1, "frame sequence overflow denied without wrap");
        Detail::local.reports = ReportsPerThread;
        Detail::local.lastReport = 1;
        Detail::ReportAt({ReportPeriodNs + 1, 0, true}, false);
        require(Detail::local.reportSuppressed == 1 && Detail::local.reports == ReportsPerThread,
            "periodic report lifetime bounded");
        FlushThread(true);
        require(Detail::local.reports == ReportsPerThread + 1 && Detail::local.finalReported,
            "one final report exposes terminal drops");
        FlushThread(true);
        require(Detail::local.reports == ReportsPerThread + 1 && Detail::local.reportSuppressed == 2,
            "repeated final report cannot bypass bound");
    } else {
        FlushThread(true);
        require(Detail::local.reports == 0 && Detail::local.tid == 0 && Detail::local.sequence == 0,
            "OFF no emission identity or report work");
    }
    std::printf("CRITICAL_PATH_TRACE_TEST checks=%u enabled=%u PASS\n", checks, enabled ? 1U : 0U);
}
