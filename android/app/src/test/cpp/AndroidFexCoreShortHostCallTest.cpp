#if !defined(LUMEN_ANDROID_FEXCORE_SHORT_HOST_CALL_BENCHMARK_ONLY)
#define LUMEN_ANDROID_FEXCORE_BARRIER_TESTING 1
#endif
#include "AndroidFexCoreMutationBarrier.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Lumen::Android::FexCoreLab::GuestMemoryMutationBarrier;
using Lumen::Android::FexCoreLab::GuestMemoryMutationScope;
using namespace std::chrono_literals;

constexpr auto kEventTimeout = 3s;
constexpr auto kMutationTimeout = 10s;

class ManualEvent final {
public:
    void Signal() noexcept {
        {
            std::lock_guard lock(mutex_);
            signaled_ = true;
        }
        condition_.notify_all();
    }

    bool Wait(std::chrono::milliseconds timeout = kEventTimeout) noexcept {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return signaled_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signaled_ {};
};

class ProcessWatchdog final {
public:
    ProcessWatchdog() : thread_([this] {
        if (!done_.Wait(60s)) {
            std::fputs("FAIL: FEX short host-call test timed out\n", stderr);
            std::fflush(stderr);
            std::_Exit(2);
        }
    }) {}

    ~ProcessWatchdog() {
        done_.Signal();
        thread_.join();
    }

private:
    ManualEvent done_;
    std::thread thread_;
};

#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
using Snapshot = GuestMemoryMutationBarrier::TestingSnapshot;

template<typename Predicate>
bool WaitForSnapshot(GuestMemoryMutationBarrier& barrier,
                     Predicate&& predicate) noexcept {
    const auto deadline = GuestMemoryMutationBarrier::Clock::now() + kEventTimeout;
    do {
        if (predicate(barrier.SnapshotForTesting())) return true;
        std::this_thread::yield();
    } while (GuestMemoryMutationBarrier::Clock::now() < deadline);
    return predicate(barrier.SnapshotForTesting());
}

bool Idle(const Snapshot& state) noexcept {
    return state.readers == 0 && state.parked == 0 &&
        state.waitingEntrants == 0 && state.requestEpoch == 0 &&
        !state.writer && state.hostCallReaders == 0 &&
        state.retainedHostCallExecutions == 0 && !state.fullIntent &&
        state.nativeDataReaders == 0;
}

bool ShortBoundaryKeepsRegisteredExecutionReader() {
    GuestMemoryMutationBarrier barrier;
    GuestMemoryMutationBarrier foreign;
    bool ok = !barrier.TryBeginShortHostCall() && !barrier.EndShortHostCall();
    if (!barrier.EnterExecution()) return false;
    const bool began = barrier.TryBeginShortHostCall();
    const auto during = barrier.SnapshotForTesting();
    ok = began && during.readers == 1 && during.hostCallReaders == 0 &&
        during.parked == 0 && during.retainedHostCallExecutions == 0 &&
        !barrier.CurrentThreadOwnsHostMemoryReadLease() &&
        !barrier.TryBeginShortHostCall() && !foreign.TryBeginShortHostCall() &&
        !foreign.EndShortHostCall() && !barrier.EnterExecution() &&
        !barrier.SuspendExecutionForHostCall() &&
        !barrier.ResumeExecutionAfterHostCall() &&
        !barrier.CheckpointSafepoint() &&
        !barrier.CheckpointAndSuspendExecutionForHostCall() &&
        !barrier.ResumeExecutionAfterHostCallAndCheckpoint() &&
        barrier.BeginQuiescentHostWait() == 0 &&
        barrier.BeginFreshCallback() == 0 &&
        barrier.TryBeginNativeGuestDataLease() == 0 && ok;
    barrier.LeaveExecution();
    ok = barrier.SnapshotForTesting().readers == 1 && ok;
    unsigned arms = 0;
    const auto mutation = barrier.BeginMutation(
        GuestMemoryMutationScope::Exclusive,
        [&](std::uint64_t) noexcept { ++arms; return true; },
        [](std::uint64_t) noexcept {}, 0ns);
    ok = mutation == 0 && arms == 0 && ok;
    if (mutation != 0) (void)barrier.EndMutation(mutation);
    if (began) ok = barrier.EndShortHostCall() && ok;
    ok = !barrier.EndShortHostCall() &&
        barrier.CurrentThreadOwnsExecutionOrMutationLease() &&
        barrier.SnapshotForTesting().readers == 1 && ok;
    const bool second = barrier.TryBeginShortHostCall();
    ok = second && ok;
    if (second) ok = barrier.EndShortHostCall() && ok;
    barrier.LeaveExecution();
    return ok && Idle(barrier.SnapshotForTesting()) &&
        Idle(foreign.SnapshotForTesting());
}

bool BridgeBorrowAndOrdinaryDemotionStayBalanced() {
    GuestMemoryMutationBarrier barrier;
    GuestMemoryMutationBarrier foreign;
    bool ok = barrier.EnsureOrdinaryHostCallAdmission();
    if (!barrier.EnterExecution()) return false;
    const bool began = barrier.TryBeginShortHostCall();
    const bool retained = began && barrier.RetainGuestDataLease();
    ok = retained && !barrier.EndShortHostCall() &&
        !barrier.EnsureOrdinaryHostCallAdmission() &&
        !foreign.RetainGuestDataLease() && !foreign.ReleaseGuestDataLease() &&
        !barrier.TryBeginShortHostCall() && ok;
    if (retained) ok = barrier.ReleaseGuestDataLease() && ok;
    const bool demoted = began && barrier.EnsureOrdinaryHostCallAdmission();
    ok = demoted && barrier.EnsureOrdinaryHostCallAdmission() &&
        barrier.CurrentThreadOwnsHostMemoryReadLease() &&
        barrier.SnapshotForTesting().readers == 0 &&
        barrier.SnapshotForTesting().hostCallReaders == 1 && ok;

    // An asynchronous guest callback must get the same retained outer HLE
    // protection and nested boundary rules as an ordinary host call.
    const bool nested = demoted && barrier.EnterExecution();
    ok = nested && !barrier.TryBeginShortHostCall() &&
        !barrier.EndShortHostCall() && ok;
    const bool nestedSuspended = nested && barrier.CheckpointAndSuspendExecutionForHostCall();
    ok = nestedSuspended && !barrier.EndShortHostCall() &&
        barrier.SnapshotForTesting().hostCallReaders == 2 &&
        barrier.SnapshotForTesting().readers == 0 && ok;
    if (nestedSuspended) ok = barrier.ResumeExecutionAfterHostCallAndCheckpoint() && ok;
    if (nested) barrier.LeaveExecution();

    const auto wait = barrier.BeginQuiescentHostWait();
    ok = wait != 0 && !barrier.EndShortHostCall() &&
        barrier.SnapshotForTesting().hostCallReaders == 0 && ok;
    if (wait != 0) ok = barrier.EndQuiescentHostWait(wait) && ok;
    const bool ordinaryBorrow = barrier.RetainGuestDataLease();
    ok = ordinaryBorrow && !barrier.EndShortHostCall() && ok;
    if (ordinaryBorrow) ok = barrier.ReleaseGuestDataLease() && ok;
    if (demoted) ok = barrier.EndShortHostCall() && ok;
    ok = !barrier.EndShortHostCall() &&
        barrier.SnapshotForTesting().readers == 1 &&
        barrier.SnapshotForTesting().hostCallReaders == 0 && ok;
    const bool again = barrier.TryBeginShortHostCall();
    ok = again && ok;
    if (again) ok = barrier.EndShortHostCall() && ok;
    barrier.LeaveExecution();
    return ok && Idle(barrier.SnapshotForTesting()) && Idle(foreign.SnapshotForTesting());
}

bool DemotedCallbackRetainsHazardAgainstExclusiveWriter() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) return false;
    const bool began = barrier.TryBeginShortHostCall();
    const bool demoted = began && barrier.EnsureOrdinaryHostCallAdmission();
    const bool nested = demoted && barrier.EnterExecution();
    ManualEvent armed;
    std::atomic_bool published {false};
    std::atomic_bool disarmed {false};
    std::atomic_bool writerPassed {false};
    std::thread writer([&] {
        const auto token = barrier.BeginMutation(
            GuestMemoryMutationScope::Exclusive,
            [&](std::uint64_t) noexcept { armed.Signal(); return true; },
            [](std::uint64_t) noexcept {}, kMutationTimeout);
        const bool ended = token != 0 && barrier.PublishAndEndMutation(
            token, [&]() noexcept { published.store(true, std::memory_order_release); },
            [&](std::uint64_t) noexcept { disarmed.store(true, std::memory_order_release); });
        writerPassed.store(ended, std::memory_order_release);
    });
    bool ok = nested && WaitForSnapshot(barrier, [](const Snapshot& state) {
        return state.fullIntent && state.readers == 1 &&
            state.retainedHostCallExecutions == 1 && state.hostCallReaders == 1 &&
            state.requestEpoch == 0 && !state.writer;
    }) && !barrier.TryBeginShortHostCall() && !barrier.EndShortHostCall() &&
        !published.load(std::memory_order_acquire);
    if (nested) barrier.LeaveExecution();
    ok = armed.Wait() && WaitForSnapshot(barrier, [](const Snapshot& state) {
        return state.readers == 0 && state.retainedHostCallExecutions == 0 &&
            state.hostCallReaders == 1 && state.requestEpoch != 0 && !state.writer;
    }) && !published.load(std::memory_order_acquire) && ok;
    if (demoted) ok = barrier.EndShortHostCall() && ok;
    ok = published.load(std::memory_order_acquire) &&
        disarmed.load(std::memory_order_acquire) &&
        barrier.CurrentThreadOwnsExecutionOrMutationLease() && ok;
    barrier.LeaveExecution();
    writer.join();
    return ok && writerPassed.load(std::memory_order_acquire) && Idle(barrier.SnapshotForTesting());
}

bool EffectfulAndNestedContextsRejectShortAdmission() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) return false;
    bool ok = barrier.EnterExecution();
    ok = !barrier.TryBeginShortHostCall() && ok;
    barrier.LeaveExecution();
    const bool suspended = barrier.CheckpointAndSuspendExecutionForHostCall();
    ok = suspended && !barrier.TryBeginShortHostCall() &&
        barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    const bool retained = barrier.RetainGuestDataLease();
    ok = retained && !barrier.TryBeginShortHostCall() && ok;
    if (retained) ok = barrier.ReleaseGuestDataLease() && ok;

    const auto wait = barrier.BeginQuiescentHostWait();
    ok = wait != 0 && !barrier.TryBeginShortHostCall() && ok;
    if (wait != 0) ok = barrier.EndQuiescentHostWait(wait) && ok;

    // A nested guest root still retains its outer HLE memory hazard.
    const bool nested = barrier.EnterExecution();
    ok = nested && !barrier.TryBeginShortHostCall() && ok;
    if (nested) barrier.LeaveExecution();

    const auto callback = barrier.BeginFreshCallback();
    ok = callback != 0 && !barrier.TryBeginShortHostCall() && ok;
    if (callback != 0) {
        const bool entered = barrier.EnterExecution();
        ok = entered && !barrier.TryBeginShortHostCall() && ok;
        if (entered) barrier.LeaveExecution();
        ok = barrier.EndFreshCallback(callback) && ok;
    }
    if (suspended) ok = barrier.ResumeExecutionAfterHostCallAndCheckpoint() && ok;

    const auto mutation = barrier.BeginMutation();
    ok = mutation != 0 && !barrier.TryBeginShortHostCall() && ok;
    if (mutation != 0) ok = barrier.EndMutation(mutation) && ok;
    barrier.LeaveExecution();

    const auto native = barrier.TryBeginNativeGuestDataLease();
    ok = native != 0 && !barrier.TryBeginShortHostCall() && ok;
    if (native != 0) ok = barrier.EndNativeGuestDataLease(native) && ok;
    return ok && Idle(barrier.SnapshotForTesting());
}

bool ShortExitParksUntilPublishAndDisarm(GuestMemoryMutationScope scope) {
    GuestMemoryMutationBarrier barrier;
    ManualEvent readerReady;
    ManualEvent finishShort;
    ManualEvent readerFinished;
    ManualEvent writerArmed;
    ManualEvent writerGranted;
    ManualEvent allowPublish;
    ManualEvent disarmStarted;
    ManualEvent allowDisarm;
    std::atomic_bool readerPrepared {false};
    std::atomic_bool readerReturned {false};
    std::atomic_bool readerPassed {false};
    std::atomic_bool writerPassed {false};
    std::atomic_bool published {false};
    std::atomic_bool disarmed {false};
    std::atomic<std::uint64_t> epoch {0};

    std::thread reader([&] {
        const bool entered = barrier.EnterExecution();
        const bool began = entered && barrier.TryBeginShortHostCall();
        readerPrepared.store(began, std::memory_order_release);
        readerReady.Signal();
        const bool requested = finishShort.Wait();
        const bool ended = began && requested && barrier.EndShortHostCall();
        readerReturned.store(ended, std::memory_order_release);
        bool ok = ended && published.load(std::memory_order_acquire) &&
            disarmed.load(std::memory_order_acquire) &&
            !barrier.CurrentThreadOwnsHostMemoryReadLease() &&
            barrier.CurrentThreadOwnsExecutionOrMutationLease();
        const bool again = ended && barrier.TryBeginShortHostCall();
        ok = again && ok;
        if (again) ok = barrier.EndShortHostCall() && ok;
        if (entered) barrier.LeaveExecution();
        readerPassed.store(ok, std::memory_order_release);
        readerFinished.Signal();
    });
    bool ok = readerReady.Wait() && readerPrepared.load(std::memory_order_acquire);

    std::thread writer([&] {
        const auto token = barrier.BeginMutation(
            scope,
            [&](std::uint64_t value) noexcept {
                epoch.store(value, std::memory_order_release);
                writerArmed.Signal();
                return true;
            },
            [](std::uint64_t) noexcept {}, kMutationTimeout);
        bool pass = token != 0;
        const auto exclusive = barrier.SnapshotForTesting();
        pass = exclusive.writer && exclusive.readers == 0 &&
            exclusive.parked == 1 && exclusive.hostCallReaders == 0 &&
            !readerReturned.load(std::memory_order_acquire) && pass;
        writerGranted.Signal();
        pass = allowPublish.Wait() && pass;
        const bool ended = token != 0 && barrier.PublishAndEndMutation(
            token,
            [&]() noexcept {
                pass = !readerReturned.load(std::memory_order_acquire) && pass;
                published.store(true, std::memory_order_release);
            },
            [&](std::uint64_t value) noexcept {
                const auto state = barrier.SnapshotForTesting();
                pass = value == epoch.load(std::memory_order_acquire) &&
                    state.writer && state.requestEpoch == value &&
                    state.readers == 0 && state.parked == 1 &&
                    !readerReturned.load(std::memory_order_acquire) && pass;
                disarmStarted.Signal();
                pass = allowDisarm.Wait() && pass;
                disarmed.store(true, std::memory_order_release);
            });
        writerPassed.store(ended && pass, std::memory_order_release);
    });

    ok = writerArmed.Wait() && ok;
    ok = WaitForSnapshot(barrier, [&](const Snapshot& state) {
        return state.readers == 1 && state.parked == 0 && !state.writer &&
            state.requestEpoch == epoch.load(std::memory_order_acquire) &&
            state.requestScope == scope && state.hostCallReaders == 0;
    }) && !published.load(std::memory_order_acquire) &&
        !readerReturned.load(std::memory_order_acquire) && ok;
    finishShort.Signal();
    ok = writerGranted.Wait() && !readerReturned.load(std::memory_order_acquire) && ok;
    allowPublish.Signal();
    ok = disarmStarted.Wait() && published.load(std::memory_order_acquire) &&
        !disarmed.load(std::memory_order_acquire) &&
        !readerReturned.load(std::memory_order_acquire) && ok;
    allowDisarm.Signal();
    ok = readerFinished.Wait() && ok;
    writer.join();
    reader.join();
    return ok && readerPassed.load(std::memory_order_acquire) &&
        writerPassed.load(std::memory_order_acquire) &&
        Idle(barrier.SnapshotForTesting());
}

bool PendingEpochUsesOrdinaryFallback(GuestMemoryMutationScope scope) {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) return false;
    ManualEvent armed;
    std::atomic_bool published {false};
    std::atomic_bool disarmed {false};
    std::atomic_bool writerPassed {false};
    std::thread writer([&] {
        const auto token = barrier.BeginMutation(
            scope, [&](std::uint64_t) noexcept { armed.Signal(); return true; },
            [](std::uint64_t) noexcept {}, kMutationTimeout);
        const bool ended = token != 0 && barrier.PublishAndEndMutation(
            token, [&]() noexcept { published.store(true, std::memory_order_release); },
            [&](std::uint64_t) noexcept { disarmed.store(true, std::memory_order_release); });
        writerPassed.store(ended, std::memory_order_release);
    });
    bool ok = armed.Wait() && !barrier.TryBeginShortHostCall();
    const bool suspended = barrier.CheckpointAndSuspendExecutionForHostCall();
    ok = suspended && published.load(std::memory_order_acquire) &&
        disarmed.load(std::memory_order_acquire) &&
        barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    if (suspended) ok = barrier.ResumeExecutionAfterHostCallAndCheckpoint() && ok;
    barrier.LeaveExecution();
    writer.join();
    return ok && writerPassed.load(std::memory_order_acquire) &&
        Idle(barrier.SnapshotForTesting());
}

bool ExclusiveIntentClosesShortGateBeforeArming() {
    GuestMemoryMutationBarrier barrier;
    ManualEvent nativeReady;
    ManualEvent releaseNative;
    ManualEvent armed;
    std::atomic_bool nativePassed {false};
    std::atomic_bool writerPassed {false};
    std::atomic_bool disarmed {false};
    std::thread native([&] {
        const auto pin = barrier.TryBeginNativeGuestDataLease();
        nativeReady.Signal();
        const bool released = releaseNative.Wait();
        nativePassed.store(pin != 0 && released && barrier.EndNativeGuestDataLease(pin),
                           std::memory_order_release);
    });
    bool ok = nativeReady.Wait() && barrier.EnterExecution();
    const bool shortBegan = barrier.TryBeginShortHostCall();
    ok = shortBegan && ok;
    std::thread writer([&] {
        const auto token = barrier.BeginMutation(
            GuestMemoryMutationScope::Exclusive,
            [&](std::uint64_t) noexcept { armed.Signal(); return true; },
            [](std::uint64_t) noexcept {}, kMutationTimeout);
        writerPassed.store(token != 0 && barrier.EndMutation(
            token, [&](std::uint64_t) noexcept { disarmed.store(true, std::memory_order_release); }),
            std::memory_order_release);
    });
    ok = WaitForSnapshot(barrier, [](const Snapshot& state) {
        return state.fullIntent && state.requestEpoch == 0 && !state.writer &&
            state.readers == 1 && state.nativeDataReaders == 1;
    }) && !barrier.TryBeginShortHostCall() && ok;
    // Before arming, short exit may return to the already registered guest
    // reader. Full intent must still reject another short call; the writer
    // cannot publish until that reader reaches the subsequently armed epoch.
    if (shortBegan) ok = barrier.EndShortHostCall() && ok;
    ok = !barrier.TryBeginShortHostCall() &&
        barrier.SnapshotForTesting().readers == 1 && ok;
    releaseNative.Signal();
    native.join();
    ok = armed.Wait() && !barrier.TryBeginShortHostCall() && ok;
    const bool suspended = barrier.CheckpointAndSuspendExecutionForHostCall();
    ok = suspended && disarmed.load(std::memory_order_acquire) && ok;
    if (suspended) ok = barrier.ResumeExecutionAfterHostCallAndCheckpoint() && ok;
    barrier.LeaveExecution();
    writer.join();
    return ok && nativePassed.load(std::memory_order_acquire) &&
        writerPassed.load(std::memory_order_acquire) && Idle(barrier.SnapshotForTesting());
}

bool CancelledEpochReopensShortAdmission(bool armSucceeds) {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) return false;
    const bool began = barrier.TryBeginShortHostCall();
    if (!began) { barrier.LeaveExecution(); return false; }
    ManualEvent armed;
    ManualEvent allowArmReturn;
    std::atomic_bool disarmed {false};
    std::atomic_bool writerDenied {false};
    std::thread writer([&] {
        const auto token = barrier.BeginMutation(
            GuestMemoryMutationScope::Exclusive,
            [&](std::uint64_t) noexcept {
                armed.Signal();
                return allowArmReturn.Wait() && armSucceeds;
            },
            [&](std::uint64_t) noexcept { disarmed.store(true, std::memory_order_release); },
            armSucceeds ? 20ms : kMutationTimeout);
        writerDenied.store(token == 0, std::memory_order_release);
        if (token != 0) (void)barrier.EndMutation(token);
    });
    bool ok = armed.Wait() && !barrier.TryBeginShortHostCall();
    allowArmReturn.Signal();
    // Keep the reader counted until the failed arm or timeout has cancelled
    // its request. A cleared mutex-protected request must reopen the epoch gate.
    writer.join();
    ok = writerDenied.load(std::memory_order_acquire) &&
        disarmed.load(std::memory_order_acquire) &&
        barrier.EndShortHostCall() && ok;
    const bool again = barrier.TryBeginShortHostCall();
    ok = again && ok;
    if (again) ok = barrier.EndShortHostCall() && ok;
    barrier.LeaveExecution();
    return ok && Idle(barrier.SnapshotForTesting());
}

bool FailedArmDisarmsBeforeReadmittingShortExit() {
    GuestMemoryMutationBarrier barrier;
    ManualEvent readerReady;
    ManualEvent finishShort;
    ManualEvent armed;
    ManualEvent allowArmReturn;
    ManualEvent disarmStarted;
    ManualEvent allowDisarm;
    std::atomic_bool readerPrepared {false};
    std::atomic_bool readerReturned {false};
    std::atomic_bool readerPassed {false};
    std::atomic_bool writerDenied {false};
    std::atomic_bool disarmed {false};
    std::thread reader([&] {
        const bool entered = barrier.EnterExecution();
        const bool began = entered && barrier.TryBeginShortHostCall();
        readerPrepared.store(began, std::memory_order_release);
        readerReady.Signal();
        const bool requested = finishShort.Wait();
        const bool ended = began && requested && barrier.EndShortHostCall();
        readerReturned.store(ended, std::memory_order_release);
        bool ok = ended && disarmed.load(std::memory_order_acquire);
        const bool again = ended && barrier.TryBeginShortHostCall();
        ok = again && ok;
        if (again) ok = barrier.EndShortHostCall() && ok;
        if (entered) barrier.LeaveExecution();
        readerPassed.store(ok, std::memory_order_release);
    });
    bool ok = readerReady.Wait() && readerPrepared.load(std::memory_order_acquire);
    std::thread writer([&] {
        const auto token = barrier.BeginMutation(
            GuestMemoryMutationScope::Exclusive,
            [&](std::uint64_t) noexcept {
                armed.Signal();
                (void)allowArmReturn.Wait();
                return false;
            },
            [&](std::uint64_t) noexcept {
                disarmStarted.Signal();
                (void)allowDisarm.Wait();
                disarmed.store(true, std::memory_order_release);
            }, kMutationTimeout);
        writerDenied.store(token == 0, std::memory_order_release);
        if (token != 0) (void)barrier.EndMutation(token);
    });
    ok = armed.Wait() && ok;
    finishShort.Signal();
    ok = WaitForSnapshot(barrier, [](const Snapshot& state) {
        return state.readers == 0 && state.parked == 1 &&
            state.requestEpoch != 0 && !state.writer;
    }) && !readerReturned.load(std::memory_order_acquire) && ok;
    allowArmReturn.Signal();
    ok = disarmStarted.Wait() && !readerReturned.load(std::memory_order_acquire) && ok;
    allowDisarm.Signal();
    writer.join();
    reader.join();
    return ok && writerDenied.load(std::memory_order_acquire) &&
        readerPassed.load(std::memory_order_acquire) && Idle(barrier.SnapshotForTesting());
}

bool WritersAndShortReadersStress() {
    GuestMemoryMutationBarrier barrier;
    constexpr unsigned kReaders = 4;
    constexpr unsigned kCalls = 50000;
    constexpr unsigned kMutations = 1000;
    std::atomic_uint ready {0};
    std::atomic_uint activeShort {0};
    std::atomic_bool activeWriter {false};
    std::atomic_bool passed {true};
    std::atomic<std::uint64_t> shortCalls {0};
    std::atomic<std::uint64_t> fallbackCalls {0};
    ManualEvent start;
    std::array<std::thread, kReaders> readers;
    for (auto& reader : readers) reader = std::thread([&] {
        const bool entered = barrier.EnterExecution();
        if (!entered) passed.store(false, std::memory_order_relaxed);
        ready.fetch_add(1, std::memory_order_release);
        if (!start.Wait()) passed.store(false, std::memory_order_relaxed);
        for (unsigned call = 0; entered && call < kCalls; ++call) {
            if (barrier.TryBeginShortHostCall()) {
                activeShort.fetch_add(1, std::memory_order_seq_cst);
                if (activeWriter.load(std::memory_order_seq_cst))
                    passed.store(false, std::memory_order_relaxed);
                if ((call & 63u) == 0) std::this_thread::yield();
                if (activeWriter.load(std::memory_order_seq_cst))
                    passed.store(false, std::memory_order_relaxed);
                activeShort.fetch_sub(1, std::memory_order_seq_cst);
                if (!barrier.EndShortHostCall())
                    passed.store(false, std::memory_order_relaxed);
                shortCalls.fetch_add(1, std::memory_order_relaxed);
            } else {
                const bool suspended = barrier.CheckpointAndSuspendExecutionForHostCall();
                const bool resumed = suspended && barrier.ResumeExecutionAfterHostCallAndCheckpoint();
                if (!resumed) passed.store(false, std::memory_order_relaxed);
                fallbackCalls.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (entered) barrier.LeaveExecution();
    });
    const auto deadline = GuestMemoryMutationBarrier::Clock::now() + kEventTimeout;
    while (ready.load(std::memory_order_acquire) != kReaders &&
           GuestMemoryMutationBarrier::Clock::now() < deadline) std::this_thread::yield();
    bool ok = ready.load(std::memory_order_acquire) == kReaders;
    std::thread writer([&] {
        if (!start.Wait()) passed.store(false, std::memory_order_relaxed);
        for (unsigned mutation = 0; mutation < kMutations; ++mutation) {
            const auto scope = (mutation & 1u) == 0
                ? GuestMemoryMutationScope::Exclusive
                : GuestMemoryMutationScope::AdditiveMapping;
            const auto token = barrier.BeginMutation(
                scope, [](std::uint64_t) noexcept { return true; },
                [](std::uint64_t) noexcept {}, kMutationTimeout);
            if (token == 0) { passed.store(false, std::memory_order_relaxed); break; }
            activeWriter.store(true, std::memory_order_seq_cst);
            if (activeShort.load(std::memory_order_seq_cst) != 0)
                passed.store(false, std::memory_order_relaxed);
            std::this_thread::yield();
            if (activeShort.load(std::memory_order_seq_cst) != 0)
                passed.store(false, std::memory_order_relaxed);
            activeWriter.store(false, std::memory_order_seq_cst);
            if (!barrier.EndMutation(token)) passed.store(false, std::memory_order_relaxed);
        }
    });
    start.Signal();
    writer.join();
    for (auto& reader : readers) reader.join();
    const auto shorts = shortCalls.load(std::memory_order_relaxed);
    const auto fallbacks = fallbackCalls.load(std::memory_order_relaxed);
    std::printf("STRESS: readers=%u mutations=%u short=%llu fallback=%llu\n",
                kReaders, kMutations, static_cast<unsigned long long>(shorts),
                static_cast<unsigned long long>(fallbacks));
    return ok && passed.load(std::memory_order_relaxed) &&
        shorts + fallbacks == std::uint64_t{kReaders} * kCalls && shorts != 0 &&
        activeShort.load(std::memory_order_relaxed) == 0 && Idle(barrier.SnapshotForTesting());
}
#endif

bool Benchmark(unsigned threads, bool shortPath) {
    constexpr unsigned kIterations = 2000000;
    GuestMemoryMutationBarrier barrier;
    std::atomic_uint ready {0};
    std::atomic_bool passed {true};
    std::atomic<std::uint64_t> checksum {0};
    ManualEvent start;
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < threads; ++index) workers.emplace_back([&, index] {
        const bool entered = barrier.EnterExecution();
        if (!entered) passed.store(false, std::memory_order_relaxed);
        ready.fetch_add(1, std::memory_order_release);
        if (!start.Wait()) passed.store(false, std::memory_order_relaxed);
        std::uint64_t result = index + 1;
        for (unsigned call = 0; entered && call < kIterations; ++call) {
            const bool began = shortPath ? barrier.TryBeginShortHostCall()
                : barrier.CheckpointAndSuspendExecutionForHostCall();
            // The same native scalar body for both paths. No guest pointers,
            // VM changes, waits, callbacks or retained objects are involved.
            std::atomic_signal_fence(std::memory_order_seq_cst);
            result = result * 6364136223846793005ull + call + 1;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const bool ended = began && (shortPath ? barrier.EndShortHostCall()
                : barrier.ResumeExecutionAfterHostCallAndCheckpoint());
            if (!ended) passed.store(false, std::memory_order_relaxed);
        }
        checksum.fetch_add(result, std::memory_order_relaxed);
        if (entered) barrier.LeaveExecution();
    });
    const auto deadline = GuestMemoryMutationBarrier::Clock::now() + kEventTimeout;
    while (ready.load(std::memory_order_acquire) != threads &&
           GuestMemoryMutationBarrier::Clock::now() < deadline) std::this_thread::yield();
    const bool allReady = ready.load(std::memory_order_acquire) == threads;
    const auto began = GuestMemoryMutationBarrier::Clock::now();
    start.Signal();
    for (auto& worker : workers) worker.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        GuestMemoryMutationBarrier::Clock::now() - began).count();
    const auto calls = std::uint64_t{threads} * kIterations;
    std::printf("BENCH: mode=%s threads=%u calls=%llu wall_ns_per_call=%.3f checksum=%llu\n",
                shortPath ? "ON_short" : "OFF_coalesced", threads,
                static_cast<unsigned long long>(calls), static_cast<double>(elapsed) / calls,
                static_cast<unsigned long long>(checksum.load(std::memory_order_relaxed)));
    bool ok = allReady && passed.load(std::memory_order_relaxed);
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
    ok = Idle(barrier.SnapshotForTesting()) && ok;
#endif
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    ProcessWatchdog watchdog;
    bool benchmark = argc == 2 && std::string_view(argv[1]) == "--benchmark";
    if (argc > 2 || (argc == 2 && !benchmark)) {
        std::fputs("usage: AndroidFexCoreShortHostCallTest [--benchmark]\n", stderr);
        return 2;
    }
#if defined(LUMEN_ANDROID_FEXCORE_SHORT_HOST_CALL_BENCHMARK_ONLY)
    benchmark = true;
#endif
    if (benchmark) {
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
        std::puts("BENCH: instrumentation=testing; compile BENCHMARK_ONLY for production boundaries");
#else
        std::puts("BENCH: instrumentation=production; native boundary cost, no game FPS measurement");
#endif
        for (unsigned round = 0; round < 5; ++round) {
            std::printf("BENCH: round=%u\n", round + 1);
            for (const auto threads : {1u, 4u}) {
                const bool firstShort = (round & 1u) != 0;
                if (!Benchmark(threads, firstShort) || !Benchmark(threads, !firstShort)) return 1;
            }
        }
        return 0;
    }

#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
    struct TestCase { const char* name; bool (*run)(); };
    const std::array tests {
        TestCase {"short boundary keeps the registered execution reader", ShortBoundaryKeepsRegisteredExecutionReader},
        TestCase {"bridge borrow and ordinary demotion stay balanced", BridgeBorrowAndOrdinaryDemotionStayBalanced},
        TestCase {"demoted callback retains its hazard against exclusive writer", DemotedCallbackRetainsHazardAgainstExclusiveWriter},
        TestCase {"effectful and nested contexts reject short admission", EffectfulAndNestedContextsRejectShortAdmission},
        TestCase {"exclusive writer waits for short exit, publication and disarm", [] {
            return ShortExitParksUntilPublishAndDisarm(GuestMemoryMutationScope::Exclusive); }},
        TestCase {"additive writer waits for short exit, publication and disarm", [] {
            return ShortExitParksUntilPublishAndDisarm(GuestMemoryMutationScope::AdditiveMapping); }},
        TestCase {"pending exclusive epoch uses ordinary fallback", [] {
            return PendingEpochUsesOrdinaryFallback(GuestMemoryMutationScope::Exclusive); }},
        TestCase {"pending additive epoch uses ordinary fallback", [] {
            return PendingEpochUsesOrdinaryFallback(GuestMemoryMutationScope::AdditiveMapping); }},
        TestCase {"exclusive intent closes the short gate before arming", ExclusiveIntentClosesShortGateBeforeArming},
        TestCase {"failed arm reopens short admission after disarm", [] { return CancelledEpochReopensShortAdmission(false); }},
        TestCase {"quiescence timeout reopens short admission after disarm", [] { return CancelledEpochReopensShortAdmission(true); }},
        TestCase {"failed arm disarms before readmitting short exit", FailedArmDisarmsBeforeReadmittingShortExit},
        TestCase {"concurrent writers and short readers", WritersAndShortReadersStress},
    };
    for (const auto& test : tests) {
        const bool passed = test.run();
        std::printf("%s: %s\n", passed ? "PASS" : "FAIL", test.name);
        if (!passed) return 1;
    }
    std::printf("PASS: %zu short host-call tests\n", tests.size());
#endif
    return 0;
}
