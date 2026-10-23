#define LUMEN_ANDROID_FEXCORE_BARRIER_TESTING 1
#include "AndroidFexCoreMutationBarrier.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

using Lumen::Android::FexCoreLab::ExecutionBarrierLease;
using Lumen::Android::FexCoreLab::GuestMemoryMutationBarrier;
using Lumen::Android::FexCoreLab::GuestMemoryMutationScope;
using Lumen::Android::FexCoreLab::NormalizeGuestInvalidationRange;
using Lumen::Android::FexCoreLab::QuiescentHostWaitLease;
using MutationDenialDiagnostic = GuestMemoryMutationBarrier::MutationDenialDiagnostic;
using MutationDenialReason = GuestMemoryMutationBarrier::MutationDenialReason;
using BarrierFailureDiagnostic = GuestMemoryMutationBarrier::BarrierFailureDiagnostic;
using BarrierFailurePhase = GuestMemoryMutationBarrier::BarrierFailurePhase;
using namespace std::chrono_literals;

constexpr auto kEventTimeout = 1500ms;
constexpr auto kMutationTimeout = 2s;

static_assert(!std::is_copy_constructible_v<ExecutionBarrierLease>);
static_assert(!std::is_move_constructible_v<ExecutionBarrierLease>);
static_assert(!std::is_copy_constructible_v<QuiescentHostWaitLease>);
static_assert(!std::is_move_constructible_v<QuiescentHostWaitLease>);
static_assert(std::is_trivial_v<BarrierFailureDiagnostic> &&
              std::is_standard_layout_v<BarrierFailureDiagnostic>);

class ManualEvent final {
public:
    void Signal() noexcept {
        {
            std::lock_guard lock(mutex_);
            signaled_ = true;
        }
        condition_.notify_all();
    }

    bool Wait(
        std::chrono::milliseconds timeout = kEventTimeout) noexcept {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock, timeout, [this] { return signaled_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool signaled_ {};
};

class ProcessWatchdog final {
public:
    ProcessWatchdog()
        : thread_([this] {
              if (!done_.Wait(20s)) {
                  std::fputs(
                      "FAIL: FEX mutation barrier test timed out\n",
                      stderr);
                  std::fflush(stderr);
                  std::_Exit(2);
              }
          }) {
    }

    ProcessWatchdog(const ProcessWatchdog&) = delete;
    ProcessWatchdog& operator=(const ProcessWatchdog&) = delete;

    ~ProcessWatchdog() {
        done_.Signal();
        thread_.join();
    }

private:
    ManualEvent done_;
    std::thread thread_;
};

using Snapshot = GuestMemoryMutationBarrier::TestingSnapshot;

bool SnapshotEquals(const Snapshot& actual,
                    const Snapshot& expected) noexcept {
    return actual.readers == expected.readers &&
        actual.parked == expected.parked &&
        actual.waitingEntrants == expected.waitingEntrants &&
        actual.requestEpoch == expected.requestEpoch &&
        actual.writer == expected.writer &&
        actual.hostCallReaders == expected.hostCallReaders &&
        actual.retainedHostCallExecutions ==
            expected.retainedHostCallExecutions &&
        actual.requestScope == expected.requestScope &&
        actual.fullIntent == expected.fullIntent &&
        actual.nativeDataReaders == expected.nativeDataReaders;
}

// Every state observed here is stable because at least one worker is blocked
// on the corresponding barrier predicate. Polling the locked test snapshot
// avoids scheduler-delay guesses and does not use arbitrary sleeps.
bool WaitForSnapshot(GuestMemoryMutationBarrier& barrier,
                     const Snapshot& expected) noexcept {
    const auto deadline = GuestMemoryMutationBarrier::Clock::now() +
        kEventTimeout;
    do {
        if (SnapshotEquals(barrier.SnapshotForTesting(), expected)) {
            return true;
        }
        std::this_thread::yield();
    } while (GuestMemoryMutationBarrier::Clock::now() < deadline);
    return SnapshotEquals(barrier.SnapshotForTesting(), expected);
}

bool WaitForWaiters(GuestMemoryMutationBarrier& barrier,
                    std::size_t owner, std::size_t queued,
                    std::size_t admission) noexcept {
    const auto deadline = GuestMemoryMutationBarrier::Clock::now() + kEventTimeout;
    do {
        const auto waiters = barrier.WaitersForTesting();
        if (waiters.ownerProgress == owner && waiters.queuedMutation == queued &&
            waiters.admission == admission) return true;
        std::this_thread::yield();
    } while (GuestMemoryMutationBarrier::Clock::now() < deadline);
    return false;
}

bool SuspendAtHostCallBoundary(GuestMemoryMutationBarrier& barrier,
                               bool coalesced) noexcept {
    return coalesced
        ? barrier.CheckpointAndSuspendExecutionForHostCall()
        : (barrier.CheckpointSafepoint() &&
           barrier.SuspendExecutionForHostCall());
}

bool ResumeAtHostCallBoundary(GuestMemoryMutationBarrier& barrier,
                              bool coalesced) noexcept {
    return coalesced
        ? barrier.ResumeExecutionAfterHostCallAndCheckpoint()
        : (barrier.ResumeExecutionAfterHostCall() &&
           barrier.CheckpointSafepoint());
}

bool AdditiveRequestDeniedWithoutArming(GuestMemoryMutationBarrier& barrier) {
    const auto before = barrier.SnapshotForTesting();
    unsigned arms = 0;
    unsigned disarms = 0;
    const auto token = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [&](std::uint64_t) noexcept { ++arms; return true; },
        [&](std::uint64_t) noexcept { ++disarms; }, 0ns);
    if (token != 0) (void)barrier.EndMutation(token);
    return token == 0 && arms == 0 && disarms == 0 &&
        SnapshotEquals(before, barrier.SnapshotForTesting());
}

bool ExecutionReadAdmissionEpochTracksEveryBoundary() {
    GuestMemoryMutationBarrier barrier, foreign;
    bool ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() == 0;
    if (!barrier.EnterExecution()) return false;
    auto epoch = barrier.CurrentThreadExecutionReadAdmissionEpoch();
    ok = epoch != 0 && foreign.CurrentThreadExecutionReadAdmissionEpoch() == 0 && ok;
    if (!barrier.EnterExecution()) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() == epoch && ok;
    barrier.LeaveExecution();
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() == epoch && ok;
    if (!barrier.TryBeginShortHostCall()) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() == 0 && ok;
    if (!barrier.EndShortHostCall()) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() != 0 &&
        barrier.CurrentThreadExecutionReadAdmissionEpoch() != epoch && ok;
    epoch = barrier.CurrentThreadExecutionReadAdmissionEpoch();
    if (!barrier.SuspendExecutionForHostCall()) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() == 0 &&
        barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    const auto wait = barrier.BeginQuiescentHostWait();
    ok = wait != 0 && barrier.CurrentThreadExecutionReadAdmissionEpoch() == 0 && ok;
    if (!barrier.EndQuiescentHostWait(wait)) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() == 0 && ok;
    const auto callback = barrier.BeginFreshCallback();
    if (callback == 0 || !barrier.EnterExecution()) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() != 0 &&
        barrier.CurrentThreadExecutionReadAdmissionEpoch() != epoch && ok;
    epoch = barrier.CurrentThreadExecutionReadAdmissionEpoch();
    barrier.LeaveExecution();
    if (!barrier.EndFreshCallback(callback) || !barrier.ResumeExecutionAfterHostCall()) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() != 0 &&
        barrier.CurrentThreadExecutionReadAdmissionEpoch() != epoch && ok;
    epoch = barrier.CurrentThreadExecutionReadAdmissionEpoch();
    const auto mutation = barrier.BeginMutation([](std::uint64_t) { return true; }, [](std::uint64_t) {}, kMutationTimeout);
    ok = mutation != 0 && barrier.CurrentThreadExecutionReadAdmissionEpoch() == 0 && ok;
    if (!barrier.EndMutation(mutation)) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() != 0 &&
        barrier.CurrentThreadExecutionReadAdmissionEpoch() != epoch && ok;
    epoch = barrier.CurrentThreadExecutionReadAdmissionEpoch();
    // A real peer mutation drains this executor through its actual safepoint.
    std::atomic<bool> armed {}, peer_ok {};
    std::thread writer([&] {
        const auto token = barrier.BeginMutation([&](std::uint64_t) {
            armed.store(true, std::memory_order_release);
            return true;
        }, [](std::uint64_t) {}, kMutationTimeout);
        peer_ok.store(token != 0 && barrier.EndMutation(token), std::memory_order_release);
    });
    const auto deadline = GuestMemoryMutationBarrier::Clock::now() + kEventTimeout;
    while (!armed.load(std::memory_order_acquire) && GuestMemoryMutationBarrier::Clock::now() < deadline)
        std::this_thread::yield();
    const bool checkpoint = armed.load(std::memory_order_acquire) && barrier.CheckpointSafepoint();
    writer.join();
    ok = checkpoint && peer_ok.load(std::memory_order_acquire) &&
        barrier.CurrentThreadExecutionReadAdmissionEpoch() != 0 &&
        barrier.CurrentThreadExecutionReadAdmissionEpoch() != epoch && ok;
    epoch = barrier.CurrentThreadExecutionReadAdmissionEpoch();
    barrier.LeaveExecution();
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() == 0 && ok;
    if (!barrier.EnterExecution()) return false;
    ok = barrier.CurrentThreadExecutionReadAdmissionEpoch() != 0 &&
        barrier.CurrentThreadExecutionReadAdmissionEpoch() != epoch && ok;
    barrier.LeaveExecution();
    return ok;
}

bool HostCallBoundaryStateMatchesOrdinaryPairs() {
    for (const bool coalesced : {false, true}) {
        GuestMemoryMutationBarrier barrier;
        GuestMemoryMutationBarrier other;
        bool ok = !SuspendAtHostCallBoundary(barrier, coalesced) &&
            !ResumeAtHostCallBoundary(barrier, coalesced);
        if (!barrier.EnterExecution()) return false;
        const bool suspended = SuspendAtHostCallBoundary(barrier, coalesced);
        ok = suspended && barrier.CurrentThreadOwnsHostMemoryReadLease() &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1}) && ok;
        ok = !SuspendAtHostCallBoundary(barrier, coalesced) &&
            !SuspendAtHostCallBoundary(other, coalesced) &&
            !ResumeAtHostCallBoundary(other, coalesced) && ok;

        const auto wait = barrier.BeginQuiescentHostWait();
        ok = wait != 0 && !SuspendAtHostCallBoundary(barrier, coalesced) &&
            !ResumeAtHostCallBoundary(barrier, coalesced) &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0}) && ok;
        const bool waitEnded = barrier.EndQuiescentHostWait(wait);
        ok = waitEnded && barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;

        // A retained outer host reader must survive a nested guest/HLE pair.
        // Execution depth two is still invalid for the host-call boundary.
        const bool nestedEntered = barrier.EnterExecution();
        const bool deeperEntered = nestedEntered && barrier.EnterExecution();
        ok = deeperEntered && !SuspendAtHostCallBoundary(barrier, coalesced) &&
            WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1}) && ok;
        if (deeperEntered) barrier.LeaveExecution();
        const bool innerSuspended = SuspendAtHostCallBoundary(barrier, coalesced);
        ok = innerSuspended &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false, 2, 1}) && ok;
        const bool innerResumed = ResumeAtHostCallBoundary(barrier, coalesced);
        ok = innerResumed &&
            WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1}) && ok;
        if (nestedEntered) barrier.LeaveExecution();
        ok = barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
        const bool resumed = ResumeAtHostCallBoundary(barrier, coalesced);
        ok = resumed && WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0}) && ok;

        const auto mutation = barrier.BeginMutation();
        ok = mutation != 0 && !SuspendAtHostCallBoundary(barrier, coalesced) &&
            !ResumeAtHostCallBoundary(barrier, coalesced) && ok;
        const bool mutationEnded = barrier.EndMutation(mutation);
        ok = mutationEnded && ok;
        barrier.LeaveExecution();
        if (!ok || !WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0}) ||
            !WaitForSnapshot(other, {0, 0, 0, 0, false, 0})) return false;
    }
    return true;
}

bool HostCallBoundaryHonorsPendingMutation(
    bool coalesced, bool beforeHostCall, GuestMemoryMutationScope scope,
    bool publish) {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) return false;
    ManualEvent peerReady;
    ManualEvent crossBoundary;
    ManualEvent peerFinished;
    std::atomic_bool peerPrepared {false};
    std::atomic_bool peerPassed {false};
    std::atomic_bool published {false};
    std::atomic_bool disarmed {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        const bool prepared = entered && (beforeHostCall ||
            SuspendAtHostCallBoundary(barrier, coalesced));
        peerPrepared.store(prepared, std::memory_order_release);
        peerReady.Signal();
        const bool requested = crossBoundary.Wait();
        const bool crossed = prepared && requested && (beforeHostCall
            ? SuspendAtHostCallBoundary(barrier, coalesced)
            : ResumeAtHostCallBoundary(barrier, coalesced));
        bool ok = crossed && disarmed.load(std::memory_order_acquire) &&
            published.load(std::memory_order_acquire) == publish;
        if (beforeHostCall && crossed) {
            ok = barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
            const bool resumed = ResumeAtHostCallBoundary(barrier, coalesced);
            ok = resumed && ok;
        }
        if (entered) barrier.LeaveExecution();
        peerPassed.store(ok, std::memory_order_release);
        peerFinished.Signal();
    });

    bool ok = peerReady.Wait() && peerPrepared.load(std::memory_order_acquire);
    std::uint64_t epoch = 0;
    bool boundaryBlocked = false;
    const auto mutation = barrier.BeginMutation(
        scope,
        [&](std::uint64_t value) noexcept {
            epoch = value;
            crossBoundary.Signal();
            // Before HLE the checkpoint parks the guest reader; after HLE,
            // resume drops the host reader before waiting for publication.
            boundaryBlocked = WaitForSnapshot(barrier,
                {1, beforeHostCall ? 1u : 0u, 1, value, false, 0, 0, scope});
            return publish && boundaryBlocked;
        },
        [&](std::uint64_t) noexcept {
            disarmed.store(true, std::memory_order_release);
        }, kMutationTimeout);
    if (publish) {
        ok = mutation != 0 && boundaryBlocked &&
            WaitForSnapshot(barrier,
                {0, beforeHostCall ? 1u : 0u, 1, epoch, true, 0, 0, scope}) && ok;
        const bool ended = mutation != 0 && barrier.PublishAndEndMutation(
            mutation,
            [&]() noexcept { published.store(true, std::memory_order_release); },
            [&](std::uint64_t) noexcept { disarmed.store(true, std::memory_order_release); });
        ok = ended && ok;
    } else {
        ok = mutation == 0 && boundaryBlocked && ok;
    }
    crossBoundary.Signal();
    const bool finished = peerFinished.Wait();
    peer.join();
    ok = finished && peerPassed.load(std::memory_order_acquire) && ok;
    barrier.LeaveExecution();
    return WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0}) && ok;
}

bool HostCallBoundaryPairsHonorPublicationAndCancellation() {
    for (const bool coalesced : {false, true}) {
        for (const bool beforeHostCall : {false, true}) {
            for (const auto scope : {GuestMemoryMutationScope::Exclusive,
                                     GuestMemoryMutationScope::AdditiveMapping}) {
                for (const bool publish : {false, true}) {
                    if (!HostCallBoundaryHonorsPendingMutation(
                            coalesced, beforeHostCall, scope, publish)) {
                        std::fprintf(stderr,
                            "FAIL: host-call pair coalesced=%d before=%d scope=%d publish=%d\n",
                            coalesced, beforeHostCall, static_cast<int>(scope), publish);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool CooperativeUpgradeParksPeerAndBlocksNewEntrant() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) {
        return false;
    }

    ManualEvent peerReady;
    ManualEvent peerCheckpoint;
    ManualEvent peerFinished;
    ManualEvent entrantStart;
    ManualEvent entrantAttempting;
    ManualEvent entrantFinished;
    std::atomic_bool disarmed {false};
    std::atomic_bool peerEntered {false};
    std::atomic_bool peerCheckpointed {false};
    std::atomic_bool peerObservedDisarm {false};
    std::atomic_bool entrantEntered {false};
    std::atomic_bool entrantObservedDisarm {false};

    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        peerEntered.store(entered, std::memory_order_release);
        peerReady.Signal();
        if (entered && peerCheckpoint.Wait()) {
            peerCheckpointed.store(
                barrier.CheckpointSafepoint(),
                std::memory_order_release);
            peerObservedDisarm.store(
                disarmed.load(std::memory_order_acquire),
                std::memory_order_release);
            barrier.LeaveExecution();
        } else if (entered) {
            barrier.LeaveExecution();
        }
        peerFinished.Signal();
    });

    std::thread entrant([&] {
        if (!entrantStart.Wait()) {
            entrantFinished.Signal();
            return;
        }
        entrantAttempting.Signal();
        const bool entered = barrier.EnterExecution();
        entrantEntered.store(entered, std::memory_order_release);
        entrantObservedDisarm.store(
            disarmed.load(std::memory_order_acquire),
            std::memory_order_release);
        if (entered) {
            barrier.LeaveExecution();
        }
        entrantFinished.Signal();
    });

    bool armObservedQuiescence = false;
    bool publisherWasExclusive = false;
    bool disarmWasExclusive = false;
    std::uint64_t armedEpoch = 0;
    std::uint64_t disarmedEpoch = 0;
    std::uint32_t armCalls = 0;
    std::uint32_t disarmCalls = 0;

    bool ok = peerReady.Wait() &&
        peerEntered.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {2, 0, 0, 0, false});
    const std::uint64_t token = barrier.BeginMutation(
        [&](std::uint64_t epoch) noexcept {
            ++armCalls;
            armedEpoch = epoch;
            entrantStart.Signal();
            const bool entrantIsWaiting = entrantAttempting.Wait() &&
                WaitForSnapshot(barrier, {2, 0, 1, epoch, false});
            peerCheckpoint.Signal();
            const bool peerIsParked = WaitForSnapshot(
                barrier, {1, 1, 2, epoch, false});
            armObservedQuiescence =
                entrantIsWaiting && peerIsParked;
            return armObservedQuiescence;
        },
        [&](std::uint64_t epoch) noexcept {
            ++disarmCalls;
            disarmedEpoch = epoch;
            disarmWasExclusive = SnapshotEquals(
                barrier.SnapshotForTesting(),
                {0, 1, 2, epoch, true});
            disarmed.store(true, std::memory_order_release);
        },
        kMutationTimeout);

    ok = ok && token != 0 && armedEpoch != 0 &&
        armCalls == 1 && armObservedQuiescence &&
        WaitForSnapshot(barrier, {0, 1, 2, armedEpoch, true});
    bool ended = false;
    if (token != 0) {
        ended = barrier.PublishAndEndMutation(
            token,
            [&]() noexcept {
                publisherWasExclusive = SnapshotEquals(
                    barrier.SnapshotForTesting(),
                    {0, 1, 2, armedEpoch, true});
            },
            [&](std::uint64_t epoch) noexcept {
                ++disarmCalls;
                disarmedEpoch = epoch;
                disarmWasExclusive = SnapshotEquals(
                    barrier.SnapshotForTesting(),
                    {0, 1, 2, epoch, true});
                disarmed.store(true, std::memory_order_release);
            });
    }

    entrantStart.Signal();
    peerCheckpoint.Signal();
    const bool workersFinished =
        peerFinished.Wait() && entrantFinished.Wait();
    peer.join();
    entrant.join();

    ok = ok && ended && publisherWasExclusive && disarmWasExclusive &&
        disarmCalls == 1 && disarmedEpoch == armedEpoch &&
        workersFinished &&
        peerCheckpointed.load(std::memory_order_acquire) &&
        peerObservedDisarm.load(std::memory_order_acquire) &&
        entrantEntered.load(std::memory_order_acquire) &&
        entrantObservedDisarm.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool NestedExecutionAndMutationKeepOneReader() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() || !barrier.EnterExecution() ||
        !WaitForSnapshot(barrier, {1, 0, 0, 0, false})) {
        barrier.LeaveExecution();
        barrier.LeaveExecution();
        return false;
    }

    std::uint64_t epoch = 0;
    std::uint32_t armCalls = 0;
    std::uint32_t disarmCalls = 0;
    std::atomic_bool disarmed {false};
    const std::uint64_t outer = barrier.BeginMutation(
        [&](std::uint64_t value) noexcept {
            ++armCalls;
            epoch = value;
            return true;
        },
        [&](std::uint64_t) noexcept {
            ++disarmCalls;
            disarmed.store(true, std::memory_order_release);
        },
        kMutationTimeout);
    const std::uint64_t nested = barrier.BeginMutation();

    ManualEvent entrantStart;
    ManualEvent entrantAttempting;
    ManualEvent entrantFinished;
    std::atomic_bool entrantEntered {false};
    std::atomic_bool entrantObservedDisarm {false};
    std::thread entrant([&] {
        if (entrantStart.Wait()) {
            entrantAttempting.Signal();
            const bool entered = barrier.EnterExecution();
            entrantEntered.store(entered, std::memory_order_release);
            entrantObservedDisarm.store(
                disarmed.load(std::memory_order_acquire),
                std::memory_order_release);
            if (entered) {
                barrier.LeaveExecution();
            }
        }
        entrantFinished.Signal();
    });
    entrantStart.Signal();
    const bool entrantIsWaiting = entrantAttempting.Wait() &&
        WaitForSnapshot(barrier, {0, 0, 1, epoch, true});

    bool ok = outer != 0 && nested != 0 && epoch != 0 &&
        armCalls == 1 && entrantIsWaiting &&
        !barrier.OwnsMutation(outer) && barrier.OwnsMutation(nested);
    const bool nestedEnded = nested != 0 && barrier.EndMutation(
        nested, [&](std::uint64_t) noexcept { ++disarmCalls; });
    ok = ok && nestedEnded && disarmCalls == 0 &&
        barrier.OwnsMutation(outer) &&
        WaitForSnapshot(barrier, {0, 0, 1, epoch, true});
    const bool outerEnded = outer != 0 && barrier.EndMutation(
        outer, [&](std::uint64_t endedEpoch) noexcept {
            ++disarmCalls;
            disarmed.store(endedEpoch == epoch,
                           std::memory_order_release);
        });

    const bool entrantDone = entrantFinished.Wait();
    entrant.join();
    ok = ok && outerEnded && entrantDone && disarmCalls == 1 &&
        entrantEntered.load(std::memory_order_acquire) &&
        entrantObservedDisarm.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false});

    barrier.LeaveExecution();
    ok = ok && WaitForSnapshot(barrier, {1, 0, 0, 0, false});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool AdditiveMappingCanPassHostMemoryReader() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) {
        return false;
    }

    ManualEvent peerSuspended;
    ManualEvent peerResume;
    ManualEvent peerResumeAttempting;
    ManualEvent peerFinished;
    std::atomic_bool peerEntered {false};
    std::atomic_bool peerSuspensionSucceeded {false};
    std::atomic_bool peerResumeSucceeded {false};
    std::atomic_bool peerObservedDisarm {false};
    std::atomic_bool disarmed {false};

    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        peerEntered.store(entered, std::memory_order_release);
        const bool suspended = entered &&
            barrier.SuspendExecutionForHostCall();
        peerSuspensionSucceeded.store(suspended, std::memory_order_release);
        peerSuspended.Signal();
        if (suspended && peerResume.Wait()) {
            peerResumeAttempting.Signal();
            const bool resumed = barrier.ResumeExecutionAfterHostCall();
            peerResumeSucceeded.store(resumed, std::memory_order_release);
            peerObservedDisarm.store(
                disarmed.load(std::memory_order_acquire),
                std::memory_order_release);
        }
        if (entered) {
            barrier.LeaveExecution();
        }
        peerFinished.Signal();
    });

    bool ok = peerSuspended.Wait() &&
        peerEntered.load(std::memory_order_acquire) &&
        peerSuspensionSucceeded.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1});

    std::uint64_t epoch = 0;
    std::uint64_t disarmedEpoch = 0;
    const std::uint64_t token = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [&](std::uint64_t value) noexcept {
            epoch = value;
            return true;
        },
        [&](std::uint64_t value) noexcept {
            disarmedEpoch = value;
            disarmed.store(true, std::memory_order_release);
        },
        kMutationTimeout);
    ok = ok && token != 0 && epoch != 0 &&
        WaitForSnapshot(
            barrier,
            {0, 0, 0, epoch, true, 1, 0,
             GuestMemoryMutationScope::AdditiveMapping});

    peerResume.Signal();
    ok = ok && peerResumeAttempting.Wait() &&
        WaitForSnapshot(
            barrier,
            {0, 0, 1, epoch, true, 0, 0,
             GuestMemoryMutationScope::AdditiveMapping});

    const bool ended = token != 0 && barrier.PublishAndEndMutation(
        token, []() noexcept {},
        [&](std::uint64_t value) noexcept {
            disarmedEpoch = value;
            disarmed.store(true, std::memory_order_release);
        });
    const bool peerDone = peerFinished.Wait();
    peer.join();

    ok = ok && ended && peerDone && disarmedEpoch == epoch &&
        peerResumeSucceeded.load(std::memory_order_acquire) &&
        peerObservedDisarm.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool AdditiveHostOwnerCanMapAcrossPeerHostCall() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall()) {
        return false;
    }

    ManualEvent peerSuspended;
    ManualEvent peerResume;
    ManualEvent peerFinished;
    std::atomic_bool peerReady {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        const bool suspended = entered &&
            barrier.SuspendExecutionForHostCall();
        peerReady.store(entered && suspended, std::memory_order_release);
        peerSuspended.Signal();
        if (suspended && peerResume.Wait()) {
            (void)barrier.ResumeExecutionAfterHostCall();
        }
        if (entered) {
            barrier.LeaveExecution();
        }
        peerFinished.Signal();
    });

    bool ok = peerSuspended.Wait() &&
        peerReady.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 2});
    std::uint64_t epoch = 0;
    std::uint64_t disarmedEpoch = 0;
    const std::uint64_t token = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [&](std::uint64_t value) noexcept {
            epoch = value;
            return true;
        },
        [&](std::uint64_t value) noexcept { disarmedEpoch = value; },
        kMutationTimeout);
    ok = ok && token != 0 && epoch != 0 &&
        WaitForSnapshot(
            barrier,
            {0, 0, 0, epoch, true, 2, 0,
             GuestMemoryMutationScope::AdditiveMapping});
    const bool ended = token != 0 && barrier.EndMutation(
        token,
        [&](std::uint64_t value) noexcept { disarmedEpoch = value; });
    ok = ok && ended && disarmedEpoch == epoch &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 2});

    peerResume.Signal();
    const bool peerDone = peerFinished.Wait();
    peer.join();
    ok = ok && peerDone &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1}) &&
        barrier.ResumeExecutionAfterHostCall() &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool AdditiveHostOwnerWaitsForCompletedEpochTail() {
    bool allPassed = true;
    for (const auto firstScope : {GuestMemoryMutationScope::AdditiveMapping,
                                 GuestMemoryMutationScope::Exclusive}) {
        GuestMemoryMutationBarrier barrier;
        if (!barrier.EnterExecution() ||
            !barrier.SuspendExecutionForHostCall()) return false;

        struct ParkedTail {
            ManualEvent entered;
            ManualEvent release;
            std::atomic_bool released {false};
        } tail;
        barrier.SetParkedReadmissionHookForTesting(
            [](void* opaque) {
                auto& value = *static_cast<ParkedTail*>(opaque);
                value.entered.Signal();
                value.released.store(value.release.Wait(3s),
                                     std::memory_order_release);
            }, &tail);

        ManualEvent peerEntered;
        ManualEvent peerCheckpoint;
        std::atomic_bool peerSucceeded {false};
        std::thread peer([&] {
            const bool entered = barrier.EnterExecution();
            peerEntered.Signal();
            const bool checkpointed = entered && peerCheckpoint.Wait() &&
                barrier.CheckpointSafepoint();
            if (entered) barrier.LeaveExecution();
            peerSucceeded.store(checkpointed, std::memory_order_release);
        });

        bool ok = peerEntered.Wait() &&
            WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1});
        std::uint64_t firstEpoch = 0;
        const auto firstToken = barrier.BeginMutation(
            firstScope,
            [&](std::uint64_t epoch) noexcept {
                firstEpoch = epoch;
                peerCheckpoint.Signal();
                return true;
            }, [](std::uint64_t) noexcept {}, kMutationTimeout);
        ok = firstToken != 0 && firstEpoch != 0 && ok;
        const bool firstEnded = firstToken != 0 && barrier.EndMutation(
            firstToken, [](std::uint64_t) noexcept {});
        // In production this tail exists between EndMutation notifying a
        // parked peer and that peer being scheduled to reacquire the mutex.
        ok = firstEnded && tail.entered.Wait() &&
            WaitForSnapshot(barrier, {0, 1, 0, 0, false, 1}) && ok;

        std::atomic_bool waitedForTail {false};
        std::thread releaser([&] {
            waitedForTail.store(
                WaitForSnapshot(barrier, {0, 1, 1, 0, false, 1}),
                std::memory_order_release);
            tail.release.Signal();
        });
        const auto secondToken = barrier.BeginMutation(
            GuestMemoryMutationScope::AdditiveMapping,
            [](std::uint64_t) noexcept { return true; },
            [](std::uint64_t) noexcept {}, kMutationTimeout);
        const bool secondEnded = secondToken != 0 && barrier.EndMutation(
            secondToken, [](std::uint64_t) noexcept {});
        releaser.join();
        peerCheckpoint.Signal();
        tail.release.Signal();
        peer.join();
        const bool queued = waitedForTail.load(std::memory_order_acquire);
        ok = queued && secondToken != 0 && secondEnded &&
            tail.released.load(std::memory_order_acquire) &&
            peerSucceeded.load(std::memory_order_acquire) &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1}) && ok;
        const bool resumed = barrier.ResumeExecutionAfterHostCall();
        barrier.LeaveExecution();
        ok = resumed && WaitForSnapshot(barrier, {0, 0, 0, 0, false}) && ok;
        std::printf("completed-epoch-tail first=%s queued=%u acquired=%u result=%s\n",
                    firstScope == GuestMemoryMutationScope::AdditiveMapping
                        ? "additive" : "exclusive",
                    queued ? 1u : 0u, secondToken != 0 ? 1u : 0u,
                    ok ? "PASS" : "FAIL");
        allPassed = ok && allPassed;
    }
    return allPassed;
}

bool NestedHostCallSuspensionsBalanceReentrantExecution() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1}) ||
        !barrier.EnterExecution() ||
        !WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1}) ||
        !barrier.SuspendExecutionForHostCall() ||
        !WaitForSnapshot(barrier, {0, 0, 0, 0, false, 2, 1}) ||
        !barrier.ResumeExecutionAfterHostCall() ||
        !WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1})) {
        return false;
    }
    barrier.LeaveExecution();
    if (!WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1}) ||
        !barrier.ResumeExecutionAfterHostCall() ||
        !WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0})) {
        return false;
    }
    barrier.LeaveExecution();
    return WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool ExclusiveOwnerCanUpgradeTopLevelHostCall() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1})) {
        return false;
    }

    std::uint64_t epoch = 0;
    std::uint64_t disarmedEpoch = 0;
    std::uint32_t armCalls = 0;
    std::uint32_t disarmCalls = 0;
    bool publisherWasExclusive = false;
    bool disarmWasExclusive = false;
    const std::uint64_t token = barrier.BeginMutation(
        [&](std::uint64_t value) noexcept {
            ++armCalls;
            epoch = value;
            return true;
        },
        [&](std::uint64_t value) noexcept {
            ++disarmCalls;
            disarmedEpoch = value;
        },
        kMutationTimeout);

    bool ok = token != 0 && epoch != 0 && armCalls == 1 &&
        WaitForSnapshot(barrier, {0, 0, 0, epoch, true, 0});
    bool ended = false;
    if (token != 0) {
        ended = barrier.PublishAndEndMutation(
            token,
            [&]() noexcept {
                publisherWasExclusive = SnapshotEquals(
                    barrier.SnapshotForTesting(),
                    {0, 0, 0, epoch, true, 0});
            },
            [&](std::uint64_t value) noexcept {
                ++disarmCalls;
                disarmedEpoch = value;
                disarmWasExclusive = SnapshotEquals(
                    barrier.SnapshotForTesting(),
                    {0, 0, 0, epoch, true, 0});
            });
    }

    ok = ok && ended && publisherWasExclusive && disarmWasExclusive &&
        disarmCalls == 1 && disarmedEpoch == epoch &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});
    const bool resumed = barrier.ResumeExecutionAfterHostCall();
    ok = ok && resumed &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool ExclusiveOwnerFailsFastAcrossRetainedHostCall() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !barrier.EnterExecution() ||
        !WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1})) {
        return false;
    }

    std::uint32_t armCalls = 0;
    std::uint32_t disarmCalls = 0;
    MutationDenialDiagnostic diagnostic {};
    const auto tryExclusive = [&]() noexcept {
        return barrier.BeginMutation(
            GuestMemoryMutationScope::Exclusive,
            [&](std::uint64_t) noexcept {
                ++armCalls;
                return true;
            },
            [&](std::uint64_t) noexcept { ++disarmCalls; },
            kMutationTimeout, &diagnostic);
    };

    const std::uint64_t nestedJitToken = tryExclusive();
    bool ok = nestedJitToken == 0 && armCalls == 0 &&
        disarmCalls == 0 &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1});
    if (nestedJitToken != 0) {
        ok = barrier.EndMutation(nestedJitToken) && ok;
    }

    const bool innerSuspended = barrier.SuspendExecutionForHostCall();
    ok = ok && innerSuspended &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 2, 1});
    std::uint64_t innerHostToken = 0;
    if (innerSuspended) {
        innerHostToken = tryExclusive();
        ok = ok && innerHostToken == 0 && armCalls == 0 &&
            disarmCalls == 0 &&
            diagnostic.reason == MutationDenialReason::NestedExclusiveMutation &&
            diagnostic.executionDepth == 0 && diagnostic.hostCallDepth == 2 &&
            diagnostic.hostCallReadersOwned == 2 && diagnostic.retainedExecutionDepth == 1 &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false, 2, 1});
        if (innerHostToken != 0) {
            ok = barrier.EndMutation(innerHostToken) && ok;
        }
    }

    const bool innerResumed = !innerSuspended ||
        barrier.ResumeExecutionAfterHostCall();
    ok = ok && innerResumed;
    barrier.LeaveExecution();
    ok = ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});
    const bool outerResumed = barrier.ResumeExecutionAfterHostCall();
    ok = ok && outerResumed &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool ExclusivePendingRejectsNestedEntryThenDrainsHostCall() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1})) {
        return false;
    }

    ManualEvent armEntered;
    ManualEvent releaseArm;
    ManualEvent writerFinished;
    std::atomic_bool writerAcquired {false};
    std::atomic_bool writerEnded {false};
    std::atomic_bool publisherWasExclusive {false};
    std::atomic_bool disarmWasExclusive {false};
    std::atomic_uint64_t epoch {0};
    std::atomic_uint64_t disarmedEpoch {0};
    std::thread writer([&] {
        const std::uint64_t token = barrier.BeginMutation(
            [&](std::uint64_t value) noexcept {
                epoch.store(value, std::memory_order_release);
                armEntered.Signal();
                return releaseArm.Wait();
            },
            [&](std::uint64_t value) noexcept {
                disarmedEpoch.store(value, std::memory_order_release);
            },
            kMutationTimeout);
        writerAcquired.store(token != 0, std::memory_order_release);
        if (token != 0) {
            const bool ended = barrier.PublishAndEndMutation(
                token,
                [&]() noexcept {
                    const std::uint64_t activeEpoch =
                        epoch.load(std::memory_order_acquire);
                    publisherWasExclusive.store(
                        SnapshotEquals(
                            barrier.SnapshotForTesting(),
                            {0, 0, 1, activeEpoch, true, 0}),
                        std::memory_order_release);
                },
                [&](std::uint64_t value) noexcept {
                    disarmedEpoch.store(value, std::memory_order_release);
                    disarmWasExclusive.store(
                        SnapshotEquals(
                            barrier.SnapshotForTesting(),
                            {0, 0, 1, value, true, 0}),
                        std::memory_order_release);
                });
            writerEnded.store(ended, std::memory_order_release);
        }
        writerFinished.Signal();
    });

    bool ok = armEntered.Wait();
    const std::uint64_t armedEpoch = epoch.load(std::memory_order_acquire);
    ok = ok && armedEpoch != 0 &&
        WaitForSnapshot(
            barrier,
            {0, 0, 0, armedEpoch, false, 1, 0,
             GuestMemoryMutationScope::Exclusive});
    ok = AdditiveRequestDeniedWithoutArming(barrier) && ok;
    BarrierFailureDiagnostic admissionDiagnostic {};
    const bool nestedEntered = barrier.EnterExecution(&admissionDiagnostic);
    ok = ok && !nestedEntered &&
        admissionDiagnostic.phase == BarrierFailurePhase::ExecutionEntry &&
        admissionDiagnostic.requestEpoch == armedEpoch &&
        admissionDiagnostic.requestScope == GuestMemoryMutationScope::Exclusive &&
        admissionDiagnostic.hostCallDepth == 1 && admissionDiagnostic.hostCallReadersOwned == 1 &&
        admissionDiagnostic.hostCallOwnerMatches && !admissionDiagnostic.executionCounted &&
        WaitForSnapshot(
            barrier,
            {0, 0, 0, armedEpoch, false, 1, 0,
             GuestMemoryMutationScope::Exclusive});
    if (nestedEntered) {
        barrier.LeaveExecution();
    }

    releaseArm.Signal();
    const bool resumed = barrier.ResumeExecutionAfterHostCall();
    const bool writerDone = writerFinished.Wait();
    writer.join();

    ok = ok && resumed && writerDone &&
        writerAcquired.load(std::memory_order_acquire) &&
        writerEnded.load(std::memory_order_acquire) &&
        publisherWasExclusive.load(std::memory_order_acquire) &&
        disarmWasExclusive.load(std::memory_order_acquire) &&
        disarmedEpoch.load(std::memory_order_acquire) == armedEpoch &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool ExclusiveIntentDrainsRetainedExecutionBeforeArm() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !barrier.EnterExecution() ||
        !WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1})) {
        return false;
    }

    ManualEvent armEntered;
    ManualEvent releaseArm;
    ManualEvent writerFinished;
    std::atomic_bool writerAcquired {false};
    std::atomic_bool writerEnded {false};
    std::atomic_uint64_t epoch {0};
    std::atomic_uint64_t disarmedEpoch {0};
    std::thread writer([&] {
        const std::uint64_t token = barrier.BeginMutation(
            [&](std::uint64_t value) noexcept {
                epoch.store(value, std::memory_order_release);
                armEntered.Signal();
                return releaseArm.Wait();
            },
            [&](std::uint64_t value) noexcept {
                disarmedEpoch.store(value, std::memory_order_release);
            },
            kMutationTimeout);
        writerAcquired.store(token != 0, std::memory_order_release);
        if (token != 0) {
            writerEnded.store(
                barrier.EndMutation(
                    token,
                    [&](std::uint64_t value) noexcept {
                        disarmedEpoch.store(value,
                                              std::memory_order_release);
                    }),
                std::memory_order_release);
        }
        writerFinished.Signal();
    });

    bool ok = WaitForSnapshot(
        barrier,
        {1, 0, 0, 0, false, 1, 1,
         GuestMemoryMutationScope::Exclusive, true}) &&
        !armEntered.Wait(0ms);
    ok = AdditiveRequestDeniedWithoutArming(barrier) && ok;

    // Returning from the already-running nested guest drains the retained
    // HLE hazard. Only then may Full publish its request and arm safepoints.
    barrier.LeaveExecution();
    ok = ok && armEntered.Wait();
    const std::uint64_t armedEpoch = epoch.load(std::memory_order_acquire);
    ok = ok && armedEpoch != 0 &&
        WaitForSnapshot(
            barrier,
            {0, 0, 0, armedEpoch, false, 1, 0,
             GuestMemoryMutationScope::Exclusive, false});
    const bool nestedEntered = barrier.EnterExecution();
    ok = ok && !nestedEntered;
    if (nestedEntered) {
        barrier.LeaveExecution();
    }

    releaseArm.Signal();
    const bool resumed = barrier.ResumeExecutionAfterHostCall();
    const bool writerDone = writerFinished.Wait();
    writer.join();
    ok = ok && resumed && writerDone &&
        writerAcquired.load(std::memory_order_acquire) &&
        writerEnded.load(std::memory_order_acquire) &&
        disarmedEpoch.load(std::memory_order_acquire) == armedEpoch &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool ExclusiveIntentTimeoutDoesNotArmSafepoints() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !barrier.EnterExecution() ||
        !WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1})) {
        return false;
    }

    std::atomic_uint32_t armCalls {0};
    std::atomic_uint32_t disarmCalls {0};
    std::atomic_uint64_t token {UINT64_MAX};
    std::thread writer([&] {
        token.store(
            barrier.BeginMutation(
                [&](std::uint64_t) noexcept {
                    armCalls.fetch_add(1, std::memory_order_relaxed);
                    return true;
                },
                [&](std::uint64_t) noexcept {
                    disarmCalls.fetch_add(1, std::memory_order_relaxed);
                },
                0ns),
            std::memory_order_release);
    });
    writer.join();

    bool ok = token.load(std::memory_order_acquire) == 0 &&
        armCalls.load(std::memory_order_acquire) == 0 &&
        disarmCalls.load(std::memory_order_acquire) == 0 &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1});
    barrier.LeaveExecution();
    ok = ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1}) &&
        barrier.ResumeExecutionAfterHostCall();
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool AdditiveOwnerCanUpgradeNestedExecution() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !barrier.EnterExecution() ||
        !WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1})) {
        return false;
    }

    std::uint64_t epoch = 0;
    std::uint64_t disarmedEpoch = 0;
    std::uint32_t armCalls = 0;
    std::uint32_t disarmCalls = 0;
    bool publisherPreservedOuterHostCall = false;
    const std::uint64_t token = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [&](std::uint64_t value) noexcept {
            ++armCalls;
            epoch = value;
            return true;
        },
        [&](std::uint64_t value) noexcept {
            ++disarmCalls;
            disarmedEpoch = value;
        },
        kMutationTimeout);

    bool ok = token != 0 && epoch != 0 && armCalls == 1 &&
        WaitForSnapshot(
            barrier,
            {0, 0, 0, epoch, true, 1, 1,
             GuestMemoryMutationScope::AdditiveMapping});
    bool ended = false;
    if (token != 0) {
        ended = barrier.PublishAndEndMutation(
            token,
            [&]() noexcept {
                publisherPreservedOuterHostCall = SnapshotEquals(
                    barrier.SnapshotForTesting(),
                    {0, 0, 0, epoch, true, 1, 1,
                     GuestMemoryMutationScope::AdditiveMapping});
            },
            [&](std::uint64_t value) noexcept {
                ++disarmCalls;
                disarmedEpoch = value;
            });
    }

    ok = ok && ended && publisherPreservedOuterHostCall &&
        disarmCalls == 1 && disarmedEpoch == epoch &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1, 1});
    barrier.LeaveExecution();
    ok = ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});
    const bool outerResumed = barrier.ResumeExecutionAfterHostCall();
    ok = ok && outerResumed &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool ExclusiveMutationWaitsForHostMemoryReader() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) {
        return false;
    }

    ManualEvent peerSuspended;
    ManualEvent peerResume;
    ManualEvent peerResumeAttempting;
    ManualEvent peerFinished;
    std::atomic_bool peerEntered {false};
    std::atomic_bool peerResumed {false};
    std::atomic_bool peerObservedDisarm {false};
    std::atomic_bool disarmed {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        peerEntered.store(entered, std::memory_order_release);
        const bool suspended = entered &&
            barrier.SuspendExecutionForHostCall();
        peerSuspended.Signal();
        if (suspended && peerResume.Wait()) {
            peerResumeAttempting.Signal();
            peerResumed.store(
                barrier.ResumeExecutionAfterHostCall(),
                std::memory_order_release);
            peerObservedDisarm.store(
                disarmed.load(std::memory_order_acquire),
                std::memory_order_release);
        }
        if (entered) {
            barrier.LeaveExecution();
        }
        peerFinished.Signal();
    });

    bool ok = peerSuspended.Wait() &&
        peerEntered.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1});

    std::uint64_t timedOutEpoch = 0;
    std::uint64_t timedOutDisarm = 0;
    const std::uint64_t denied = barrier.BeginMutation(
        [&](std::uint64_t epoch) noexcept {
            timedOutEpoch = epoch;
            return true;
        },
        [&](std::uint64_t epoch) noexcept { timedOutDisarm = epoch; },
        0ns);
    ok = ok && denied == 0 && timedOutEpoch != 0 &&
        timedOutDisarm == timedOutEpoch &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1});

    std::uint64_t epoch = 0;
    std::uint64_t disarmedEpoch = 0;
    bool peerStoppedReading = false;
    const std::uint64_t token = barrier.BeginMutation(
        [&](std::uint64_t value) noexcept {
            epoch = value;
            peerResume.Signal();
            peerStoppedReading = peerResumeAttempting.Wait() &&
                WaitForSnapshot(barrier, {1, 0, 1, epoch, false, 0});
            return peerStoppedReading;
        },
        [&](std::uint64_t value) noexcept {
            disarmedEpoch = value;
            disarmed.store(true, std::memory_order_release);
        },
        kMutationTimeout);
    ok = ok && token != 0 && peerStoppedReading &&
        WaitForSnapshot(barrier, {0, 0, 1, epoch, true, 0});

    const bool ended = token != 0 && barrier.PublishAndEndMutation(
        token, []() noexcept {},
        [&](std::uint64_t value) noexcept {
            disarmedEpoch = value;
            disarmed.store(true, std::memory_order_release);
        });
    const bool peerDone = peerFinished.Wait();
    peer.join();

    ok = ok && ended && peerDone && disarmedEpoch == epoch &&
        peerResumed.load(std::memory_order_acquire) &&
        peerObservedDisarm.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool QuiescentWaitTokensAndContextAreChecked() {
    GuestMemoryMutationBarrier barrier;
    GuestMemoryMutationBarrier other;
    bool ok = barrier.BeginQuiescentHostWait() == 0 &&
        !barrier.EndQuiescentHostWait(0);
    const auto initialMutation = barrier.BeginMutation();
    ok = ok && initialMutation != 0 && barrier.BeginQuiescentHostWait() == 0;
    const bool initialMutationEnded = barrier.EndMutation(initialMutation);
    ok = ok && initialMutationEnded;
    if (!barrier.EnterExecution()) return false;
    ok = ok && barrier.BeginQuiescentHostWait() == 0;
    if (!barrier.SuspendExecutionForHostCall()) {
        barrier.LeaveExecution();
        return false;
    }

    const auto first = barrier.BeginQuiescentHostWait();
    std::atomic_bool foreignRejected {false};
    std::thread foreign([&] {
        foreignRejected.store(!barrier.EndQuiescentHostWait(first),
                              std::memory_order_release);
    });
    foreign.join();
    ok = ok && first != 0 &&
        foreignRejected.load(std::memory_order_acquire) &&
        !other.EndQuiescentHostWait(first) &&
        !barrier.EndQuiescentHostWait(0) &&
        !barrier.EndQuiescentHostWait(first + 1) &&
        barrier.BeginQuiescentHostWait() == 0 &&
        !barrier.EnterExecution() && !other.EnterExecution() &&
        !barrier.SuspendExecutionForHostCall() &&
        !barrier.ResumeExecutionAfterHostCall() &&
        !barrier.CheckpointSafepoint() &&
        barrier.BeginMutation() == 0 && other.BeginMutation() == 0 &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
    const bool firstEnded = barrier.EndQuiescentHostWait(first);
    ok = ok && firstEnded && !barrier.EndQuiescentHostWait(first) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});

    const auto second = barrier.BeginQuiescentHostWait();
    ok = ok && second != 0 && second != first &&
        !barrier.EndQuiescentHostWait(first);
    const bool secondEnded = barrier.EndQuiescentHostWait(second);
    ok = ok && secondEnded;
    // A writer's arm callback runs before mutationDepth becomes nonzero, but
    // cannot lend away the host reader that BeginMutation is upgrading.
    bool armWaitRejected = false;
    const auto hostMutation = barrier.BeginMutation(
        [&](std::uint64_t) noexcept {
            armWaitRejected = barrier.BeginQuiescentHostWait() == 0;
            return armWaitRejected;
        }, [](std::uint64_t) noexcept {}, 0ns);
    ok = ok && armWaitRejected && hostMutation != 0 &&
        barrier.BeginQuiescentHostWait() == 0;
    const bool hostMutationEnded = barrier.EndMutation(hostMutation);
    ok = ok && hostMutationEnded;
    {
        QuiescentHostWaitLease wait(barrier);
        QuiescentHostWaitLease nested(barrier);
        ok = ok && wait.Acquired() && !nested.Acquired() &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
    }
    ok = ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});
    const bool resumed = barrier.ResumeExecutionAfterHostCall();
    barrier.LeaveExecution();
    return ok && resumed &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0}) &&
        WaitForSnapshot(other, {0, 0, 0, 0, false, 0});
}

bool HostMemoryReadOwnershipTracksCallingContext() {
    GuestMemoryMutationBarrier barrier;
    GuestMemoryMutationBarrier other;
    bool ok = !barrier.CurrentThreadOwnsHostMemoryReadLease() &&
        !other.CurrentThreadOwnsHostMemoryReadLease();
    if (!barrier.EnterExecution()) return false;
    ok = !barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    if (!barrier.SuspendExecutionForHostCall()) {
        barrier.LeaveExecution();
        return false;
    }
    ok = barrier.CurrentThreadOwnsHostMemoryReadLease() &&
        !other.CurrentThreadOwnsHostMemoryReadLease() && ok;
    std::atomic_bool foreignDenied {false};
    std::thread foreign([&] {
        foreignDenied.store(!barrier.CurrentThreadOwnsHostMemoryReadLease(),
                            std::memory_order_release);
    });
    foreign.join();
    ok = foreignDenied.load(std::memory_order_acquire) && ok;

    const bool nestedEntered = barrier.EnterExecution();
    ok = nestedEntered && !barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    if (nestedEntered) {
        const bool nestedHost = barrier.SuspendExecutionForHostCall();
        ok = nestedHost && barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
        if (nestedHost) {
            const bool resumed = barrier.ResumeExecutionAfterHostCall();
            ok = resumed && !barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
        }
        barrier.LeaveExecution();
    }
    ok = barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    const auto wait = barrier.BeginQuiescentHostWait();
    ok = wait != 0 && !barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    if (wait != 0) {
        const bool ended = barrier.EndQuiescentHostWait(wait);
        ok = ended && barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    }

    bool deniedDuringRequest = false;
    const auto mutation = barrier.BeginMutation(
        [&](std::uint64_t) noexcept {
            deniedDuringRequest = !barrier.CurrentThreadOwnsHostMemoryReadLease();
            return true;
        }, [](std::uint64_t) noexcept {}, kMutationTimeout);
    ok = mutation != 0 && deniedDuringRequest &&
        !barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    if (mutation != 0) {
        const bool ended = barrier.EndMutation(mutation);
        ok = ended && barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    }
    const bool resumed = barrier.ResumeExecutionAfterHostCall();
    ok = resumed && !barrier.CurrentThreadOwnsHostMemoryReadLease() && ok;
    barrier.LeaveExecution();
    return !barrier.CurrentThreadOwnsHostMemoryReadLease() &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0}) && ok;
}

bool QuiescentWaitRejectsRetainedOuterHostCall() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall()) return false;
    bool ok = barrier.BeginQuiescentHostWait() == 0 &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 2, 1});
    const bool innerResumed = barrier.ResumeExecutionAfterHostCall();
    barrier.LeaveExecution();
    const auto token = barrier.BeginQuiescentHostWait();
    ok = ok && innerResumed && token != 0 &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
    const bool ended = barrier.EndQuiescentHostWait(token);
    const bool outerResumed = barrier.ResumeExecutionAfterHostCall();
    barrier.LeaveExecution();
    return ok && ended && outerResumed &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool QuiescentWaitReadmissionFollowsPublishAndDisarm() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) return false;
    ManualEvent waiting;
    ManualEvent wake;
    ManualEvent readmitting;
    std::atomic_bool acquired {false};
    std::atomic_bool resumed {false};
    std::atomic_bool published {false};
    std::atomic_bool disarmed {false};
    std::atomic_bool observedCommit {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        const bool suspended = entered && barrier.SuspendExecutionForHostCall();
        {
            QuiescentHostWaitLease wait(barrier);
            acquired.store(suspended && wait.Acquired(), std::memory_order_release);
            waiting.Signal();
            (void)wake.Wait();
            readmitting.Signal();
        }
        observedCommit.store(published.load(std::memory_order_acquire) &&
                             disarmed.load(std::memory_order_acquire),
                             std::memory_order_release);
        resumed.store(suspended && barrier.ResumeExecutionAfterHostCall(),
                      std::memory_order_release);
        if (entered) barrier.LeaveExecution();
    });
    bool ok = waiting.Wait() && acquired.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0});
    std::uint64_t epoch = 0;
    const auto mutation = barrier.BeginMutation(
        [&](std::uint64_t value) noexcept { epoch = value; return true; },
        [](std::uint64_t) noexcept {}, kMutationTimeout);
    wake.Signal();
    ok = ok && mutation != 0 && readmitting.Wait() &&
        WaitForSnapshot(barrier, {0, 0, 1, epoch, true, 0});
    bool stillBlockedDuringPublish = false;
    bool stillBlockedDuringDisarm = false;
    const bool ended = mutation != 0 && barrier.PublishAndEndMutation(
        mutation,
        [&] {
            stillBlockedDuringPublish = SnapshotEquals(
                barrier.SnapshotForTesting(), {0, 0, 1, epoch, true, 0});
            published.store(true, std::memory_order_release);
        },
        [&](std::uint64_t) noexcept {
            stillBlockedDuringDisarm = SnapshotEquals(
                barrier.SnapshotForTesting(), {0, 0, 1, epoch, true, 0});
            disarmed.store(true, std::memory_order_release);
        });
    peer.join();
    barrier.LeaveExecution();
    return ok && ended && stillBlockedDuringPublish && stillBlockedDuringDisarm &&
        resumed.load(std::memory_order_acquire) &&
        observedCommit.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool QuiescentWaitCanDrainAnAlreadyPendingExclusiveRequest() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) return false;
    ManualEvent suspended;
    ManualEvent beginWait;
    ManualEvent waiting;
    ManualEvent wake;
    std::atomic_bool acquired {false};
    std::atomic_bool resumed {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        const bool host = entered && barrier.SuspendExecutionForHostCall();
        suspended.Signal();
        (void)beginWait.Wait();
        const auto token = barrier.BeginQuiescentHostWait();
        acquired.store(host && token != 0, std::memory_order_release);
        waiting.Signal();
        (void)wake.Wait();
        const bool ended = barrier.EndQuiescentHostWait(token);
        resumed.store(ended && barrier.ResumeExecutionAfterHostCall(),
                      std::memory_order_release);
        if (entered) barrier.LeaveExecution();
    });
    bool ok = suspended.Wait() &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false, 1});
    std::uint64_t epoch = 0;
    bool drained = false;
    const auto mutation = barrier.BeginMutation(
        [&](std::uint64_t value) noexcept {
            epoch = value;
            beginWait.Signal();
            drained = waiting.Wait() &&
                acquired.load(std::memory_order_acquire) &&
                WaitForSnapshot(barrier, {1, 0, 0, epoch, false, 0});
            return drained;
        }, [](std::uint64_t) noexcept {}, kMutationTimeout);
    ok = ok && drained && mutation != 0 &&
        WaitForSnapshot(barrier, {0, 0, 0, epoch, true, 0});
    const bool ended = mutation != 0 && barrier.EndMutation(mutation);
    beginWait.Signal();
    wake.Signal();
    peer.join();
    barrier.LeaveExecution();
    return ok && ended && resumed.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool QuiescentWaitMayReadmitDuringAdditiveMapping() {
    GuestMemoryMutationBarrier barrier;
    ManualEvent waiting;
    ManualEvent wake;
    ManualEvent readmitted;
    ManualEvent finishHostCall;
    std::atomic_bool acquired {false};
    std::atomic_bool waitEnded {false};
    std::atomic_bool resumed {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        const bool suspended = entered && barrier.SuspendExecutionForHostCall();
        const auto token = barrier.BeginQuiescentHostWait();
        acquired.store(suspended && token != 0, std::memory_order_release);
        waiting.Signal();
        (void)wake.Wait();
        waitEnded.store(barrier.EndQuiescentHostWait(token), std::memory_order_release);
        readmitted.Signal();
        (void)finishHostCall.Wait();
        resumed.store(suspended && barrier.ResumeExecutionAfterHostCall(),
                      std::memory_order_release);
        if (entered) barrier.LeaveExecution();
    });
    bool ok = waiting.Wait() && acquired.load(std::memory_order_acquire);
    const auto mutation = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [](std::uint64_t) noexcept { return true; },
        [](std::uint64_t) noexcept {}, kMutationTimeout);
    wake.Signal();
    ok = ok && mutation != 0 && readmitted.Wait() &&
        waitEnded.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 1, true, 1, 0,
                                 GuestMemoryMutationScope::AdditiveMapping});
    const bool ended = mutation != 0 && barrier.EndMutation(mutation);
    finishHostCall.Signal();
    peer.join();
    return ok && ended && resumed.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool QuiescentWaitReadmissionHonorsFullIntent() {
    GuestMemoryMutationBarrier barrier;
    // Keep an outer HLE lease across nested guest execution so a writer must
    // remain in full intent before it can arm any safepoints.
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall() ||
        !barrier.EnterExecution()) return false;
    ManualEvent waiting;
    ManualEvent wake;
    ManualEvent readmitting;
    ManualEvent armed;
    std::atomic_bool acquired {false};
    std::atomic_bool resumed {false};
    std::atomic_bool writerEnded {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        const bool suspended = entered && barrier.SuspendExecutionForHostCall();
        const auto token = barrier.BeginQuiescentHostWait();
        acquired.store(suspended && token != 0, std::memory_order_release);
        waiting.Signal();
        (void)wake.Wait();
        readmitting.Signal();
        const bool ended = barrier.EndQuiescentHostWait(token);
        resumed.store(ended && barrier.ResumeExecutionAfterHostCall(),
                      std::memory_order_release);
        if (entered) barrier.LeaveExecution();
    });
    bool ok = waiting.Wait() && acquired.load(std::memory_order_acquire);
    std::thread writer([&] {
        const auto token = barrier.BeginMutation(
            [&](std::uint64_t) noexcept { armed.Signal(); return true; },
            [](std::uint64_t) noexcept {}, kMutationTimeout);
        writerEnded.store(token != 0 && barrier.EndMutation(token),
                          std::memory_order_release);
    });
    ok = ok && WaitForSnapshot(
        barrier, {1, 0, 0, 0, false, 1, 1,
                  GuestMemoryMutationScope::Exclusive, true});
    wake.Signal();
    ok = ok && readmitting.Wait() && WaitForSnapshot(
        barrier, {1, 0, 1, 0, false, 1, 1,
                  GuestMemoryMutationScope::Exclusive, true});
    barrier.LeaveExecution();
    // Keep the outer host reader until the writer has advanced beyond intent;
    // then the ordinary HLE return atomically drains it and waits for publish.
    const bool wasArmed = armed.Wait();
    const bool rootResumed = barrier.ResumeExecutionAfterHostCall();
    barrier.LeaveExecution();
    writer.join();
    peer.join();
    return ok && wasArmed && rootResumed &&
        writerEnded.load(std::memory_order_acquire) &&
        resumed.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool QuiescentWaitDoesNotExemptOtherHostMemoryReaders() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() ||
        !barrier.SuspendExecutionForHostCall()) return false;
    const auto wait = barrier.BeginQuiescentHostWait();
    ManualEvent peerReady;
    ManualEvent peerResume;
    std::atomic_bool peerResumed {false};
    std::atomic_bool denied {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        const bool suspended = entered && barrier.SuspendExecutionForHostCall();
        peerReady.Signal();
        (void)peerResume.Wait();
        peerResumed.store(suspended && barrier.ResumeExecutionAfterHostCall(),
                          std::memory_order_release);
        if (entered) barrier.LeaveExecution();
    });
    bool ok = wait != 0 && peerReady.Wait() &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});
    std::thread writer([&] {
        std::uint64_t armed = 0;
        std::uint64_t disarmed = 0;
        const auto token = barrier.BeginMutation(
            [&](std::uint64_t value) noexcept { armed = value; return true; },
            [&](std::uint64_t value) noexcept { disarmed = value; }, 0ns);
        denied.store(token == 0 && armed != 0 && disarmed == armed,
                     std::memory_order_release);
        if (token != 0) (void)barrier.EndMutation(token);
    });
    writer.join();
    const bool waitEnded = barrier.EndQuiescentHostWait(wait);
    ok = ok && denied.load(std::memory_order_acquire) && waitEnded &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 2});
    peerResume.Signal();
    peer.join();
    const bool rootResumed = barrier.ResumeExecutionAfterHostCall();
    barrier.LeaveExecution();
    return ok && rootResumed && peerResumed.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool ExclusiveRetirementRetryRequiresEffectFreeDenialAndFreshAdmission() {
    bool passed = true;
    for (const bool fullIntent : {false, true}) {
        GuestMemoryMutationBarrier barrier;
        if (!barrier.EnterExecution() || !barrier.SuspendExecutionForHostCall()) return false;
        ManualEvent pinReady, releasePin, armEntered;
        std::atomic_bool pinPassed {true}, writerPassed {false};
        std::atomic_uint32_t publication {0};
        std::thread pin;
        if (fullIntent) {
            pin = std::thread([&] {
                const auto token = barrier.TryBeginNativeGuestDataLease();
                pinPassed.store(token != 0, std::memory_order_release);
                pinReady.Signal();
                const bool released = releasePin.Wait();
                pinPassed.store(token != 0 && released && barrier.EndNativeGuestDataLease(token),
                    std::memory_order_release);
            });
            passed = pinReady.Wait() && pinPassed.load(std::memory_order_acquire) && passed;
        }
        std::thread writer([&] {
            const auto token = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
                [&](std::uint64_t) noexcept { armEntered.Signal(); return true; },
                [](std::uint64_t) noexcept {}, kMutationTimeout);
            if (token == 0) return;
            const auto state = barrier.SnapshotForTesting();
            const bool alone = state.writer && state.readers == 0 && state.hostCallReaders == 0;
            const bool ended = barrier.PublishAndEndMutation(token, [&]() noexcept {
                publication.store(1, std::memory_order_release);
            }, [](std::uint64_t) noexcept {});
            writerPassed.store(alone && ended, std::memory_order_release);
        });
        passed = (fullIntent ? WaitForSnapshot(barrier,
            {0, 0, 0, 0, false, 1, 0, GuestMemoryMutationScope::Exclusive, true, 1}) :
            armEntered.Wait()) && passed;
        std::uint32_t deniedArms = 0;
        MutationDenialDiagnostic diagnostic {};
        const auto denied = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
            [&](std::uint64_t) noexcept { ++deniedArms; return true; },
            [](std::uint64_t) noexcept {}, kMutationTimeout, &diagnostic);
        passed = denied == 0 && deniedArms == 0 && publication.load(std::memory_order_acquire) == 0 &&
            GuestMemoryMutationBarrier::MutationDenialAllowsQuiescentRetry(diagnostic) &&
            diagnostic.fullIntent == fullIntent && barrier.CurrentThreadOwnsHostMemoryReadLease() && passed;
        // Even this proven competing writer cannot lend or release a live borrow.
        const bool borrowed = barrier.RetainGuestDataLease();
        MutationDenialDiagnostic borrowedDenial {};
        const auto borrowedToken = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
            [](std::uint64_t) noexcept { return true; }, [](std::uint64_t) noexcept {},
            0ns, &borrowedDenial);
        passed = borrowed && borrowedToken == 0 && barrier.BeginQuiescentHostWait() == 0 &&
            borrowedDenial.reason == MutationDenialReason::GuestDataLeaseActive &&
            !GuestMemoryMutationBarrier::MutationDenialAllowsQuiescentRetry(borrowedDenial) && passed;
        if (borrowed) passed = barrier.ReleaseGuestDataLease() && passed;
        const auto wait = barrier.BeginQuiescentHostWait();
        passed = wait != 0 && !barrier.CurrentThreadOwnsHostMemoryReadLease() && passed;
        releasePin.Signal();
        if (wait != 0) passed = barrier.EndQuiescentHostWait(wait) && passed;
        writer.join();
        if (pin.joinable()) pin.join();
        passed = pinPassed.load(std::memory_order_acquire) && writerPassed.load(std::memory_order_acquire) &&
            publication.load(std::memory_order_acquire) == 1 && barrier.CurrentThreadOwnsHostMemoryReadLease() && passed;
        const auto retried = barrier.BeginMutation();
        passed = retried != 0 && publication.load(std::memory_order_acquire) == 1 && passed;
        if (retried != 0) passed = barrier.PublishAndEndMutation(retried, [&]() noexcept {
            publication.store(2, std::memory_order_release);
        }, [](std::uint64_t) noexcept {}) && passed;
        passed = barrier.ResumeExecutionAfterHostCall() && passed;
        barrier.LeaveExecution();
        passed = publication.load(std::memory_order_acquire) == 2 &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false}) && passed;
        std::printf("FEX_BARRIER_TLS_RETIREMENT full_intent=%u effect_free_denial=%u borrowed_denial=%u publications=%u result=%s\n",
            fullIntent ? 1u : 0u,
            GuestMemoryMutationBarrier::MutationDenialAllowsQuiescentRetry(diagnostic) ? 1u : 0u,
            borrowedDenial.reason == MutationDenialReason::GuestDataLeaseActive ? 1u : 0u,
            publication.load(), passed ? "PASS" : "FAIL");
    }
    return passed;
}

bool AdditiveWriterCannotNestExclusiveMutation() {
    GuestMemoryMutationBarrier barrier;
    ManualEvent peerSuspended;
    ManualEvent peerResume;
    ManualEvent peerFinished;
    std::atomic_bool peerEntered {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        peerEntered.store(entered, std::memory_order_release);
        const bool suspended = entered &&
            barrier.SuspendExecutionForHostCall();
        peerSuspended.Signal();
        if (suspended && peerResume.Wait()) {
            (void)barrier.ResumeExecutionAfterHostCall();
        }
        if (entered) {
            barrier.LeaveExecution();
        }
        peerFinished.Signal();
    });

    bool ok = peerSuspended.Wait() &&
        peerEntered.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});
    const std::uint64_t outer = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [](std::uint64_t) noexcept { return true; },
        [](std::uint64_t) noexcept {}, kMutationTimeout);
    const std::uint64_t rejectedExclusive = barrier.BeginMutation();
    const std::uint64_t nestedAdditive = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [](std::uint64_t) noexcept { return true; },
        [](std::uint64_t) noexcept {}, kMutationTimeout);
    ok = ok && outer != 0 && rejectedExclusive == 0 &&
        nestedAdditive != 0 && barrier.OwnsMutation(nestedAdditive) &&
        WaitForSnapshot(
            barrier,
            {0, 0, 0, 1, true, 1, 0,
             GuestMemoryMutationScope::AdditiveMapping});
    const bool nestedEnded = nestedAdditive != 0 &&
        barrier.EndMutation(nestedAdditive);
    const bool outerEnded = outer != 0 && barrier.EndMutation(outer);

    peerResume.Signal();
    const bool peerDone = peerFinished.Wait();
    peer.join();
    return ok && nestedEnded && outerEnded && peerDone &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0});
}

bool CompetingGuestMutatorsSerialize() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) {
        return false;
    }

    ManualEvent peerReady;
    ManualEvent startPeerMutation;
    ManualEvent peerArmed;
    ManualEvent peerFinished;
    std::atomic_bool peerEntered {false};
    std::atomic_bool peerMutationAcquired {false};
    std::atomic_bool peerMutationEnded {false};
    std::atomic_bool peerPublisherSerialized {false};
    std::atomic_uint32_t publishersInFlight {0};
    std::atomic_uint32_t publishSequence {0};
    std::atomic_uint32_t firstPublishOrder {0};
    std::atomic_uint32_t secondPublishOrder {0};
    std::atomic_uint64_t secondEpoch {0};
    std::atomic_uint64_t secondDisarmedEpoch {0};

    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        peerEntered.store(entered, std::memory_order_release);
        peerReady.Signal();
        if (!entered || !startPeerMutation.Wait()) {
            if (entered) {
                barrier.LeaveExecution();
            }
            peerFinished.Signal();
            return;
        }

        const std::uint64_t token = barrier.BeginMutation(
            [&](std::uint64_t epoch) noexcept {
                secondEpoch.store(epoch, std::memory_order_release);
                peerArmed.Signal();
                return true;
            },
            [&](std::uint64_t epoch) noexcept {
                secondDisarmedEpoch.store(epoch,
                                          std::memory_order_release);
            },
            kMutationTimeout);
        peerMutationAcquired.store(token != 0, std::memory_order_release);
        if (token != 0) {
            const bool ended = barrier.PublishAndEndMutation(
                token,
                [&]() noexcept {
                    const bool alone =
                        publishersInFlight.fetch_add(
                            1, std::memory_order_acq_rel) == 0;
                    const std::uint32_t order =
                        publishSequence.fetch_add(
                            1, std::memory_order_acq_rel) + 1;
                    secondPublishOrder.store(order,
                                             std::memory_order_release);
                    peerPublisherSerialized.store(
                        alone &&
                            firstPublishOrder.load(
                                std::memory_order_acquire) == 1,
                        std::memory_order_release);
                    publishersInFlight.fetch_sub(
                        1, std::memory_order_acq_rel);
                },
                [&](std::uint64_t epoch) noexcept {
                    secondDisarmedEpoch.store(
                        epoch, std::memory_order_release);
                });
            peerMutationEnded.store(ended, std::memory_order_release);
        }
        barrier.LeaveExecution();
        peerFinished.Signal();
    });

    bool peerParkedForFirst = false;
    bool firstPublisherExclusive = false;
    std::uint64_t firstEpoch = 0;
    std::uint64_t firstDisarmedEpoch = 0;
    bool ok = peerReady.Wait() &&
        peerEntered.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {2, 0, 0, 0, false});
    const std::uint64_t firstToken = barrier.BeginMutation(
        [&](std::uint64_t epoch) noexcept {
            firstEpoch = epoch;
            startPeerMutation.Signal();
            peerParkedForFirst = WaitForSnapshot(
                barrier, {1, 1, 1, epoch, false});
            return peerParkedForFirst;
        },
        [&](std::uint64_t epoch) noexcept {
            firstDisarmedEpoch = epoch;
        },
        kMutationTimeout);
    ok = ok && firstToken != 0 && firstEpoch != 0 &&
        peerParkedForFirst &&
        WaitForSnapshot(barrier, {0, 1, 1, firstEpoch, true});

    bool firstEnded = false;
    if (firstToken != 0) {
        firstEnded = barrier.PublishAndEndMutation(
            firstToken,
            [&]() noexcept {
                const bool alone = publishersInFlight.fetch_add(
                    1, std::memory_order_acq_rel) == 0;
                const std::uint32_t order = publishSequence.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
                firstPublishOrder.store(order, std::memory_order_release);
                firstPublisherExclusive = alone && order == 1;
                publishersInFlight.fetch_sub(1, std::memory_order_acq_rel);
            },
            [&](std::uint64_t epoch) noexcept {
                firstDisarmedEpoch = epoch;
            });
    }

    const bool secondWasArmed = peerArmed.Wait();
    const std::uint64_t observedSecondEpoch =
        secondEpoch.load(std::memory_order_acquire);
    const bool ownerCheckpointed =
        secondWasArmed && barrier.CheckpointSafepoint();
    const bool peerDone = peerFinished.Wait();
    peer.join();

    ok = ok && firstEnded && firstPublisherExclusive &&
        firstDisarmedEpoch == firstEpoch && secondWasArmed &&
        observedSecondEpoch > firstEpoch && ownerCheckpointed && peerDone &&
        peerMutationAcquired.load(std::memory_order_acquire) &&
        peerMutationEnded.load(std::memory_order_acquire) &&
        peerPublisherSerialized.load(std::memory_order_acquire) &&
        firstPublishOrder.load(std::memory_order_acquire) == 1 &&
        secondPublishOrder.load(std::memory_order_acquire) == 2 &&
        secondDisarmedEpoch.load(std::memory_order_acquire) ==
            observedSecondEpoch &&
        publishersInFlight.load(std::memory_order_acquire) == 0 &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool TimeoutIsFailClosedAndLateCheckpointIsHarmless() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution()) {
        return false;
    }

    ManualEvent peerReady;
    ManualEvent lateCheckpoint;
    ManualEvent lateCheckpointDone;
    ManualEvent secondCheckpoint;
    ManualEvent peerFinished;
    std::atomic_bool peerEntered {false};
    std::atomic_bool lateCheckpointResult {false};
    std::atomic_bool secondCheckpointResult {false};
    std::thread peer([&] {
        const bool entered = barrier.EnterExecution();
        peerEntered.store(entered, std::memory_order_release);
        peerReady.Signal();
        if (entered && lateCheckpoint.Wait()) {
            lateCheckpointResult.store(
                barrier.CheckpointSafepoint(),
                std::memory_order_release);
            lateCheckpointDone.Signal();
            if (secondCheckpoint.Wait()) {
                secondCheckpointResult.store(
                    barrier.CheckpointSafepoint(),
                    std::memory_order_release);
            }
            barrier.LeaveExecution();
        } else if (entered) {
            barrier.LeaveExecution();
        }
        peerFinished.Signal();
    });

    std::uint64_t timedOutEpoch = 0;
    std::uint64_t timedOutDisarmedEpoch = 0;
    MutationDenialDiagnostic diagnostic {};
    bool ok = peerReady.Wait() &&
        peerEntered.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {2, 0, 0, 0, false});
    const std::uint64_t denied = barrier.BeginMutation(
        GuestMemoryMutationScope::Exclusive,
        [&](std::uint64_t epoch) noexcept {
            timedOutEpoch = epoch;
            return true;
        },
        [&](std::uint64_t epoch) noexcept {
            timedOutDisarmedEpoch = epoch;
        },
        0ns, &diagnostic);
    ok = ok && denied == 0 && timedOutEpoch != 0 &&
        timedOutDisarmedEpoch == timedOutEpoch &&
        diagnostic.reason == MutationDenialReason::QuiescenceTimeout &&
        diagnostic.requestEpoch == timedOutEpoch && diagnostic.readers == 2 &&
        diagnostic.requestScope == GuestMemoryMutationScope::Exclusive && !diagnostic.writer &&
        WaitForSnapshot(barrier, {2, 0, 0, 0, false});

    lateCheckpoint.Signal();
    ok = ok && lateCheckpointDone.Wait() &&
        lateCheckpointResult.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {2, 0, 0, 0, false});

    std::uint64_t secondEpoch = 0;
    std::uint64_t secondDisarmedEpoch = 0;
    bool peerParked = false;
    const std::uint64_t admitted = barrier.BeginMutation(
        [&](std::uint64_t epoch) noexcept {
            secondEpoch = epoch;
            secondCheckpoint.Signal();
            peerParked = WaitForSnapshot(
                barrier, {1, 1, 1, epoch, false});
            return peerParked;
        },
        [&](std::uint64_t epoch) noexcept {
            secondDisarmedEpoch = epoch;
        },
        kMutationTimeout);
    ok = ok && admitted != 0 && peerParked &&
        secondEpoch > timedOutEpoch &&
        WaitForSnapshot(barrier, {0, 1, 1, secondEpoch, true});
    const bool ended = admitted != 0 && barrier.EndMutation(
        admitted, [&](std::uint64_t epoch) noexcept {
            secondDisarmedEpoch = epoch;
        });

    const bool peerDone = peerFinished.Wait();
    peer.join();
    ok = ok && ended && peerDone &&
        secondCheckpointResult.load(std::memory_order_acquire) &&
        secondDisarmedEpoch == secondEpoch &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false});
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool MutationTokensAreThreadAffineAndSingleUse() {
    GuestMemoryMutationBarrier barrier;
    const std::uint64_t writer = barrier.BeginMutation();
    if (writer == 0 || !barrier.OwnsMutation(writer)) {
        return false;
    }
    const std::uint64_t wrong = writer ^ (UINT64_C(1) << 40U);
    if (barrier.OwnsMutation(wrong) || barrier.EndMutation(wrong)) {
        return false;
    }

    std::atomic_bool crossThreadOwned {true};
    std::atomic_bool crossThreadEnded {true};
    std::thread crossThread([&] {
        crossThreadOwned.store(
            barrier.OwnsMutation(writer), std::memory_order_release);
        crossThreadEnded.store(
            barrier.EndMutation(writer), std::memory_order_release);
    });
    crossThread.join();
    if (crossThreadOwned.load(std::memory_order_acquire) ||
        crossThreadEnded.load(std::memory_order_acquire) ||
        !barrier.OwnsMutation(writer)) {
        return false;
    }

    bool published = false;
    bool reentryDenied = false;
    std::uint32_t disarmCalls = 0;
    const bool ended = barrier.PublishAndEndMutation(
        writer,
        [&]() noexcept {
            published = true;
            reentryDenied = barrier.BeginMutation() == 0;
        },
        [&](std::uint64_t) noexcept { ++disarmCalls; });
    return ended && published && reentryDenied && disarmCalls == 1 &&
        !barrier.OwnsMutation(writer) && !barrier.EndMutation(writer) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool CompetingMutationQueueHonorsDeadlineBeforeArm() {
    GuestMemoryMutationBarrier barrier;
    const std::uint64_t owner = barrier.BeginMutation();
    if (owner == 0) {
        return false;
    }

    std::atomic_uint32_t armCalls {0};
    std::atomic_uint32_t disarmCalls {0};
    std::atomic_uint64_t contender {UINT64_MAX};
    std::thread peer([&] {
        contender.store(
            barrier.BeginMutation(
                [&](std::uint64_t) noexcept {
                    armCalls.fetch_add(1, std::memory_order_relaxed);
                    return true;
                },
                [&](std::uint64_t) noexcept {
                    disarmCalls.fetch_add(1, std::memory_order_relaxed);
                },
                0ns),
            std::memory_order_release);
    });
    peer.join();

    const bool deniedBeforeArm =
        contender.load(std::memory_order_acquire) == 0 &&
        armCalls.load(std::memory_order_acquire) == 0 &&
        disarmCalls.load(std::memory_order_acquire) == 0;
    return barrier.EndMutation(owner) && deniedBeforeArm &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool ThreadLocalStateCannotCrossBarrierInstances() {
    GuestMemoryMutationBarrier first;
    GuestMemoryMutationBarrier second;
    if (!first.EnterExecution()) {
        return false;
    }
    const bool secondDenied = !second.EnterExecution() &&
        second.BeginMutation() == 0;
    first.LeaveExecution();
    if (!second.EnterExecution()) {
        return false;
    }
    second.LeaveExecution();
    return secondDenied &&
        WaitForSnapshot(first, {0, 0, 0, 0, false}) &&
        WaitForSnapshot(second, {0, 0, 0, 0, false});
}

bool ExclusivePublicationBorrowKeepsEpochAndVmClosed() {
    GuestMemoryMutationBarrier barrier, foreign;
    if (barrier.RetainExclusivePublicationBorrow() != 0 ||
        barrier.ReleaseExclusivePublicationBorrow(0)) return false;
    const auto additive = barrier.BeginMutation(GuestMemoryMutationScope::AdditiveMapping,
        [](std::uint64_t) noexcept { return true; }, [](std::uint64_t) noexcept {}, 0ns);
    if (additive == 0 || barrier.RetainExclusivePublicationBorrow() != 0 ||
        !barrier.EndMutation(additive)) return false;
    if (!barrier.EnterExecution()) return false;
    const auto guest_writer = barrier.BeginMutation();
    if (guest_writer == 0 || barrier.RetainExclusivePublicationBorrow() != 0 ||
        !barrier.EndMutation(guest_writer)) return false;
    if (!barrier.SuspendExecutionForHostCall()) return false;
    const auto hle_writer = barrier.BeginMutation();
    if (hle_writer == 0 || barrier.RetainExclusivePublicationBorrow() != 0 ||
        !barrier.EndMutation(hle_writer) || !barrier.ResumeExecutionAfterHostCall()) return false;
    barrier.LeaveExecution();
    const auto owner = barrier.BeginMutation();
    const auto nested = barrier.BeginMutation();
    if (owner == 0 || nested == 0 || barrier.RetainExclusivePublicationBorrow() != 0 ||
        !barrier.EndMutation(nested)) return false;
    std::array<std::uint64_t, 64> borrows {};
    bool ok = true;
    for (auto& token : borrows) {
        token = barrier.RetainExclusivePublicationBorrow();
        ok = token != 0 && ok;
    }
    ok = barrier.RetainExclusivePublicationBorrow() == 0 &&
        !foreign.ReleaseExclusivePublicationBorrow(borrows[0]) && ok;
    bool published = false;
    std::uint32_t disarmed = 0;
    ok = !barrier.PublishAndEndMutation(owner, [&] { published = true; },
        [&](std::uint64_t) { ++disarmed; }) && !published && disarmed == 0 &&
        barrier.OwnsMutation(owner) && barrier.BeginMutation() == 0 &&
        !barrier.EnterExecution() && ok;
    const auto epoch = barrier.SnapshotForTesting().requestEpoch;
    std::atomic_bool entered {false};
    std::thread reader([&] {
        if (barrier.EnterExecution()) {
            entered.store(true, std::memory_order_release);
            barrier.LeaveExecution();
        }
    });
    ok = WaitForSnapshot(barrier, {0, 0, 1, epoch, true}) &&
        !entered.load(std::memory_order_acquire) && ok;
    bool released = true;
    std::thread completion([&] {
        for (const auto token : borrows)
            released = barrier.ReleaseExclusivePublicationBorrow(token) && released;
        released = !barrier.ReleaseExclusivePublicationBorrow(borrows[0]) && released;
    });
    completion.join();
    const bool still_blocked = !entered.load(std::memory_order_acquire);
    const bool ended = barrier.PublishAndEndMutation(owner, [&] { published = true; },
        [&](std::uint64_t) { ++disarmed; });
    ok = released && still_blocked && ended && published && disarmed == 1 && ok;
    reader.join();
    ok = entered.load(std::memory_order_acquire) && ok;
    const auto next_owner = barrier.BeginMutation();
    const auto fresh = barrier.RetainExclusivePublicationBorrow();
    ok = next_owner != 0 && fresh != 0 && fresh != borrows[0] &&
        !barrier.ReleaseExclusivePublicationBorrow(borrows[0]) &&
        barrier.ReleaseExclusivePublicationBorrow(fresh) && barrier.EndMutation(next_owner) && ok;
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool GuestDataLeaseRequiresOwningHostCallReader() {
    GuestMemoryMutationBarrier barrier;
    GuestMemoryMutationBarrier other;
    if (barrier.RetainGuestDataLease() || barrier.ReleaseGuestDataLease() ||
        !barrier.EnterExecution()) return false;
    bool ok = !barrier.RetainGuestDataLease() &&
        !barrier.ReleaseGuestDataLease();
    if (!barrier.SuspendExecutionForHostCall()) return false;
    ok = !other.RetainGuestDataLease() && !other.ReleaseGuestDataLease() && ok;
    if (!barrier.RetainGuestDataLease()) return false;
    std::atomic_bool peerDenied {false};
    std::thread peer([&] {
        peerDenied.store(!barrier.RetainGuestDataLease() &&
                            !barrier.ReleaseGuestDataLease(),
                        std::memory_order_release);
    });
    peer.join();
    ok = peerDenied.load(std::memory_order_acquire) &&
        !other.ReleaseGuestDataLease() &&
        barrier.ReleaseGuestDataLease() &&
        !barrier.ReleaseGuestDataLease() && ok;
    const bool resumed = barrier.ResumeExecutionAfterHostCall();
    ok = resumed && !barrier.RetainGuestDataLease() && ok;
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool GuestDataLeasesPreventReaderSurrenderAndUpgrade() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() || !barrier.SuspendExecutionForHostCall() ||
        !barrier.RetainGuestDataLease() || !barrier.RetainGuestDataLease())
        return false;
    unsigned armCalls = 0;
    unsigned disarmCalls = 0;
    const auto forbiddenTransitions = [&](std::uint32_t expectedBorrowCount) noexcept {
        MutationDenialDiagnostic diagnostic {};
        BarrierFailureDiagnostic readmissionDiagnostic {};
        const auto arm = [&](std::uint64_t) noexcept {
            ++armCalls;
            return true;
        };
        const auto disarm = [&](std::uint64_t) noexcept { ++disarmCalls; };
        return barrier.BeginQuiescentHostWait() == 0 &&
            !barrier.EnterExecution() &&
            !barrier.ResumeExecutionAfterHostCall() &&
            !barrier.ResumeExecutionAfterHostCallAndCheckpoint(&readmissionDiagnostic) &&
            readmissionDiagnostic.phase == BarrierFailurePhase::HostCallResume &&
            readmissionDiagnostic.guestDataLeases == expectedBorrowCount &&
            readmissionDiagnostic.hostCallOwnerMatches &&
            readmissionDiagnostic.hostCallReadersOwned == 1 &&
            barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
                                  arm, disarm, 0ns, &diagnostic) == 0 &&
            barrier.BeginMutation(GuestMemoryMutationScope::AdditiveMapping,
                                  arm, disarm, 0ns, &diagnostic) == 0 &&
            diagnostic.reason == MutationDenialReason::GuestDataLeaseActive &&
            diagnostic.guestDataLeases == expectedBorrowCount &&
            diagnostic.hostCallReadersOwned == 1 && diagnostic.hostCallDepth == 1 &&
            armCalls == 0 && disarmCalls == 0 &&
            barrier.CurrentThreadOwnsHostMemoryReadLease() &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});
    };
    bool ok = forbiddenTransitions(2);
    const bool firstReleased = barrier.ReleaseGuestDataLease();
    ok = firstReleased && forbiddenTransitions(1) && ok;
    const bool lastReleased = barrier.ReleaseGuestDataLease();
    ok = lastReleased && ok;
    const auto wait = barrier.BeginQuiescentHostWait();
    ok = wait != 0 && !barrier.RetainGuestDataLease() &&
        barrier.EndQuiescentHostWait(wait) && ok;
    const auto mutation = barrier.BeginMutation();
    ok = mutation != 0 && !barrier.RetainGuestDataLease() &&
        barrier.EndMutation(mutation) && ok;
    const bool resumed = barrier.ResumeExecutionAfterHostCallAndCheckpoint();
    ok = resumed && WaitForSnapshot(barrier, {1, 0, 0, 0, false}) && ok;
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool GuestDataLeaseKeepsPeerWriterBehindHostCallReader() {
    GuestMemoryMutationBarrier barrier;
    if (!barrier.EnterExecution() || !barrier.SuspendExecutionForHostCall() ||
        !barrier.RetainGuestDataLease()) return false;

    ManualEvent armEntered;
    ManualEvent releaseArm;
    std::atomic_uint64_t epoch {0};
    std::atomic_bool writerAcquired {false};
    std::atomic_bool writerEnded {false};
    std::thread writer([&] {
        const auto mutation = barrier.BeginMutation(
            [&](std::uint64_t value) noexcept {
                epoch.store(value, std::memory_order_release);
                armEntered.Signal();
                return releaseArm.Wait();
            },
            [](std::uint64_t) noexcept {}, kMutationTimeout);
        writerAcquired.store(mutation != 0, std::memory_order_release);
        if (mutation != 0)
            writerEnded.store(barrier.EndMutation(mutation),
                              std::memory_order_release);
    });
    bool ok = armEntered.Wait();
    const auto pendingEpoch = epoch.load(std::memory_order_acquire);
    ok = pendingEpoch != 0 &&
        WaitForSnapshot(barrier, {0, 0, 0, pendingEpoch, false, 1}) &&
        !barrier.ResumeExecutionAfterHostCall() &&
        barrier.RetainGuestDataLease() &&
        barrier.ReleaseGuestDataLease() &&
        !barrier.ResumeExecutionAfterHostCall() && ok;
    const bool released = barrier.ReleaseGuestDataLease();
    // Explicit release ends only this borrow. The original HLE reader stays
    // counted until the callback returns, still excluding the peer writer.
    ok = released && !writerAcquired.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, pendingEpoch, false, 1}) && ok;
    releaseArm.Signal();
    const bool resumed = barrier.ResumeExecutionAfterHostCallAndCheckpoint();
    writer.join();
    ok = resumed && writerAcquired.load(std::memory_order_acquire) &&
        writerEnded.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {1, 0, 0, 0, false}) && ok;
    barrier.LeaveExecution();
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool NativeDowngradeRejectsUnsupportedOwnersWithoutEffects() {
    GuestMemoryMutationBarrier barrier;
    GuestMemoryMutationBarrier other;
    unsigned disarms = 0;
    const auto disarm = [&](std::uint64_t) noexcept { ++disarms; };
    const auto deny = [&](std::uint64_t token) {
        const auto before = barrier.SnapshotForTesting();
        return barrier.DowngradeExclusiveToNativeGuestDataLease(token, disarm) == 0 &&
            SnapshotEquals(barrier.SnapshotForTesting(), before) && disarms == 0;
    };
    bool okay = deny(0);
    const auto outer = barrier.BeginMutation();
    if (outer == 0) return false;
    okay = deny(outer + 1) && other.DowngradeExclusiveToNativeGuestDataLease(outer) == 0 && okay;
    bool foreign_denied = false;
    std::thread foreign([&] { foreign_denied = barrier.DowngradeExclusiveToNativeGuestDataLease(outer) == 0; });
    foreign.join();
    okay = foreign_denied && okay;
    const auto nested = barrier.BeginMutation();
    okay = nested != 0 && deny(nested) && deny(outer) && okay;
    okay = barrier.EndMutation(nested) && okay;
    const auto borrow = barrier.RetainExclusivePublicationBorrow();
    okay = borrow != 0 && deny(outer) && barrier.ReleaseExclusivePublicationBorrow(borrow) && okay;
    bool publishing_denied = false;
    okay = barrier.PublishAndEndMutation(outer, [&] { publishing_denied = deny(outer); }) &&
        publishing_denied && okay;

    const auto additive = barrier.BeginMutation(GuestMemoryMutationScope::AdditiveMapping,
        [](std::uint64_t) noexcept { return true; }, [](std::uint64_t) noexcept {}, 0ns);
    okay = additive != 0 && deny(additive) && barrier.EndMutation(additive) && okay;
    if (!barrier.EnterExecution()) return false;
    const auto execution_upgrade = barrier.BeginMutation();
    okay = execution_upgrade != 0 && deny(execution_upgrade) && barrier.EndMutation(execution_upgrade) && okay;
    if (!barrier.SuspendExecutionForHostCall()) return false;
    const auto host_upgrade = barrier.BeginMutation();
    okay = host_upgrade != 0 && deny(host_upgrade) && barrier.EndMutation(host_upgrade) &&
        barrier.ResumeExecutionAfterHostCall() && okay;
    barrier.LeaveExecution();
    return okay && disarms == 0 && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool NativeDowngradePinsBeforeDisarmAndAllowsCpuProgress() {
    GuestMemoryMutationBarrier barrier;
    const auto exclusive = barrier.BeginMutation();
    if (exclusive == 0) return false;
    ManualEvent cpu_entered, progress_allowed, cpu_progress, leave_allowed;
    std::atomic_bool cpu_okay {false};
    std::thread cpu([&] {
        const bool entered = barrier.EnterExecution();
        cpu_entered.Signal();
        if (!entered) return;
        const bool progress = progress_allowed.Wait() && barrier.CheckpointSafepoint() &&
            barrier.CurrentThreadOwnsExecutionLease();
        cpu_progress.Signal();
        const bool leave = leave_allowed.Wait();
        barrier.LeaveExecution();
        cpu_okay.store(progress && leave, std::memory_order_release);
    });
    auto expected = barrier.SnapshotForTesting();
    expected.waitingEntrants = 1;
    bool okay = WaitForSnapshot(barrier, expected);
    bool disarm_checked = false;
    const auto pin = barrier.DowngradeExclusiveToNativeGuestDataLease(exclusive,
        [&](std::uint64_t epoch) noexcept {
            const auto state = barrier.SnapshotForTesting();
            // The mapping pin already exists while all CPU admission is still
            // closed. Reentrant mutation APIs cannot consume the outer owner.
            disarm_checked = epoch != 0 && state.writer && state.requestEpoch == epoch &&
                state.readers == 0 && state.nativeDataReaders == 1 &&
                barrier.BeginMutation() == 0 && !barrier.EndMutation(exclusive) &&
                barrier.DowngradeExclusiveToNativeGuestDataLease(exclusive) == 0 &&
                !barrier.OwnsNativeGuestDataLease(1) && !barrier.EndNativeGuestDataLease(1);
        });
    okay = pin != 0 && disarm_checked && !barrier.OwnsMutation(exclusive) &&
        !barrier.EndMutation(exclusive) && barrier.OwnsNativeGuestDataLease(pin) &&
        !barrier.OwnsNativeGuestDataLease(pin + 1) && cpu_entered.Wait() && okay;
    bool foreign_denied = false;
    std::thread foreign([&] {
        foreign_denied = !barrier.OwnsNativeGuestDataLease(pin) && !barrier.EndNativeGuestDataLease(pin);
    });
    foreign.join();
    okay = foreign_denied && okay;

    std::atomic_uint arms {0};
    std::atomic_bool writer_done {false};
    std::thread writer([&] {
        const auto token = barrier.BeginMutation([&](std::uint64_t) noexcept {
            arms.fetch_add(1, std::memory_order_release);
            return true;
        }, [](std::uint64_t) noexcept {}, kMutationTimeout);
        writer_done.store(token != 0 && barrier.EndMutation(token), std::memory_order_release);
    });
    okay = WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0, 0,
        GuestMemoryMutationScope::Exclusive, true, 1}) && okay;
    progress_allowed.Signal();
    okay = cpu_progress.Wait() && arms.load(std::memory_order_acquire) == 0 &&
        !writer_done.load(std::memory_order_acquire) && okay;
    // A range owner must release its claims and this pin before asking for any
    // new exclusive epoch. The waiting CPU can then leave independently.
    okay = barrier.EndNativeGuestDataLease(pin) && !barrier.OwnsNativeGuestDataLease(pin) &&
        !barrier.EndNativeGuestDataLease(pin) && okay;
    leave_allowed.Signal();
    cpu.join();
    writer.join();
    const auto next = barrier.TryBeginNativeGuestDataLease();
    okay = next != 0 && next != pin && !barrier.OwnsNativeGuestDataLease(pin) &&
        barrier.EndNativeGuestDataLease(next) && okay;
    return okay && cpu_okay.load(std::memory_order_acquire) && writer_done.load(std::memory_order_acquire) &&
        arms.load(std::memory_order_acquire) == 1 && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool NativeDataLeaseChecksContextAffinityAndNonce() {
    GuestMemoryMutationBarrier barrier;
    GuestMemoryMutationBarrier other;
    bool retryable = true;
    const auto first = barrier.TryBeginNativeGuestDataLease(nullptr, &retryable);
    if (first == 0 || retryable) return false;
    unsigned arms = 0;
    MutationDenialDiagnostic diagnostic {};
    const auto arm = [&](std::uint64_t) noexcept { ++arms; return true; };
    const auto disarm = [](std::uint64_t) noexcept {};
    const char* native_denial = nullptr;
    retryable = true;
    bool ok = barrier.TryBeginNativeGuestDataLease(&native_denial, &retryable) == 0 && !retryable &&
        native_denial != nullptr && std::string_view(native_denial) == "context-invalid" &&
        other.TryBeginNativeGuestDataLease() == 0 && !barrier.EnterExecution() &&
        !barrier.RetainGuestDataLease() && barrier.BeginQuiescentHostWait() == 0 &&
        barrier.BeginMutation(GuestMemoryMutationScope::Exclusive, arm, disarm, 0ns,
                              &diagnostic) == 0 &&
        diagnostic.reason == MutationDenialReason::NativeDataLeaseActive &&
        barrier.BeginMutation(GuestMemoryMutationScope::AdditiveMapping, arm, disarm, 0ns,
                              &diagnostic) == 0 && arms == 0 &&
        !other.EndNativeGuestDataLease(first) &&
        !barrier.EndNativeGuestDataLease(first + 1) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0, 0,
            GuestMemoryMutationScope::Exclusive, false, 1});
    std::atomic_bool crossThreadDenied {false};
    std::thread peer([&] {
        crossThreadDenied.store(!barrier.EndNativeGuestDataLease(first),
                                std::memory_order_release);
    });
    peer.join();
    ok = crossThreadDenied.load(std::memory_order_acquire) &&
        barrier.EndNativeGuestDataLease(first) &&
        !barrier.EndNativeGuestDataLease(first) && ok;
    const auto second = barrier.TryBeginNativeGuestDataLease();
    ok = second != 0 && second != first &&
        !barrier.EndNativeGuestDataLease(first) &&
        barrier.EndNativeGuestDataLease(second) && ok;
    if (!barrier.EnterExecution()) return false;
    ok = barrier.TryBeginNativeGuestDataLease() == 0 && ok;
    if (!barrier.SuspendExecutionForHostCall()) return false;
    ok = barrier.TryBeginNativeGuestDataLease() == 0 && ok;
    const auto wait = barrier.BeginQuiescentHostWait();
    ok = wait != 0 && barrier.TryBeginNativeGuestDataLease() == 0 &&
        barrier.EndQuiescentHostWait(wait) && barrier.ResumeExecutionAfterHostCall() && ok;
    barrier.LeaveExecution();
    const auto mutation = barrier.BeginMutation();
    ok = mutation != 0 && barrier.TryBeginNativeGuestDataLease() == 0 &&
        barrier.EndMutation(mutation) && ok;
    return ok && WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool NativeDataLeaseKeepsCpuRunningAndVmBehindPin() {
    GuestMemoryMutationBarrier barrier;
    const auto pin = barrier.TryBeginNativeGuestDataLease();
    if (pin == 0) return false;
    ManualEvent cpuEntered;
    std::atomic_bool stopCpu {false};
    std::atomic_bool cpuSucceeded {false};
    std::atomic_uint64_t cpuSteps {0};
    std::thread cpu([&] {
        const bool entered = barrier.EnterExecution();
        cpuEntered.Signal();
        if (!entered) return;
        bool checkpoints = true;
        while (!stopCpu.load(std::memory_order_acquire)) {
            if (!barrier.CheckpointSafepoint()) { checkpoints = false; break; }
            cpuSteps.fetch_add(1, std::memory_order_release);
            std::this_thread::yield();
        }
        barrier.LeaveExecution();
        cpuSucceeded.store(checkpoints, std::memory_order_release);
    });
    bool ok = cpuEntered.Wait();
    std::atomic_uint32_t arms {0};
    std::atomic_bool vmEffect {false};
    std::atomic_bool writerClosed {false};
    std::thread writer([&] {
        const auto mutation = barrier.BeginMutation(
            [&](std::uint64_t) noexcept {
                arms.fetch_add(1, std::memory_order_release);
                return true;
            }, [](std::uint64_t) noexcept {}, kMutationTimeout);
        if (mutation != 0) {
            vmEffect.store(true, std::memory_order_release);
            writerClosed.store(barrier.EndMutation(mutation), std::memory_order_release);
        }
    });
    // Full intent stops new native pins but does not arm CPU safepoints while
    // the old pin is live. This is a stable state, independent of sleep timing.
    ok = WaitForSnapshot(barrier, {1, 0, 0, 0, false, 0, 0,
        GuestMemoryMutationScope::Exclusive, true, 1}) && ok;
    // Existing invalid contexts never become retryable just because an
    // unrelated writer also has full intent pending.
    bool context_retryable = true;
    ok = barrier.TryBeginNativeGuestDataLease(nullptr, &context_retryable) == 0 &&
        !context_retryable && ok;
    const auto before = cpuSteps.load(std::memory_order_acquire);
    const auto deadline = GuestMemoryMutationBarrier::Clock::now() + kEventTimeout;
    while (cpuSteps.load(std::memory_order_acquire) == before &&
           GuestMemoryMutationBarrier::Clock::now() < deadline) std::this_thread::yield();
    ok = cpuSteps.load(std::memory_order_acquire) > before &&
        arms.load(std::memory_order_acquire) == 0 && !vmEffect.load(std::memory_order_acquire) && ok;
    std::atomic_bool newPinDenied {false};
    std::thread entrant([&] {
        const char* reason = nullptr;
        bool retryable = false;
        const auto denied = barrier.TryBeginNativeGuestDataLease(&reason, &retryable);
        newPinDenied.store(denied == 0 && reason != nullptr &&
            std::string_view(reason) == "mutation-pending" && retryable, std::memory_order_release);
        if (denied != 0) (void)barrier.EndNativeGuestDataLease(denied);
    });
    entrant.join();
    ok = newPinDenied.load(std::memory_order_acquire) &&
        barrier.EndNativeGuestDataLease(pin) && ok;
    writer.join();
    stopCpu.store(true, std::memory_order_release);
    cpu.join();
    return ok && arms.load(std::memory_order_acquire) == 1 &&
        vmEffect.load(std::memory_order_acquire) && writerClosed.load(std::memory_order_acquire) &&
        cpuSucceeded.load(std::memory_order_acquire) &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool NativeDataLeaseAllowsAdditiveButExclusiveTimeoutHasNoEffects() {
    GuestMemoryMutationBarrier barrier;
    const auto pin = barrier.TryBeginNativeGuestDataLease();
    if (pin == 0) return false;
    std::atomic_bool additiveSucceeded {false};
    std::atomic_bool exclusiveDeniedBeforeArm {false};
    std::thread peer([&] {
        unsigned arms = 0;
        const auto arm = [&](std::uint64_t) noexcept { ++arms; return true; };
        const auto disarm = [](std::uint64_t) noexcept {};
        const auto additive = barrier.BeginMutation(
            GuestMemoryMutationScope::AdditiveMapping, arm, disarm, 0ns);
        additiveSucceeded.store(additive != 0 && barrier.EndMutation(additive),
                                std::memory_order_release);
        arms = 0;
        MutationDenialDiagnostic diagnostic {};
        const auto exclusive = barrier.BeginMutation(
            GuestMemoryMutationScope::Exclusive, arm, disarm, 0ns, &diagnostic);
        exclusiveDeniedBeforeArm.store(exclusive == 0 && arms == 0 &&
            diagnostic.reason == MutationDenialReason::NativeDataLeaseTimeout,
            std::memory_order_release);
        if (exclusive != 0) (void)barrier.EndMutation(exclusive);
    });
    peer.join();
    const bool preserved = WaitForSnapshot(barrier, {0, 0, 0, 0, false, 0, 0,
        GuestMemoryMutationScope::Exclusive, false, 1});
    const bool released = barrier.EndNativeGuestDataLease(pin);
    return additiveSucceeded.load(std::memory_order_acquire) &&
        exclusiveDeniedBeforeArm.load(std::memory_order_acquire) && preserved && released &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool FreshCallbackDrainsWriterAndChecksLifo() {
    GuestMemoryMutationBarrier barrier;
    ExecutionBarrierLease outer(barrier);
    if (!outer.Acquired() || !barrier.SuspendExecutionForHostCall()) return false;
    if (!barrier.RetainGuestDataLease()) return false;
    bool pass = barrier.BeginFreshCallback() == 0;
    pass = barrier.ReleaseGuestDataLease() && pass;
    ManualEvent pin_ready, release_pin, writer_ready, release_writer;
    bool pin_valid {}, writer_valid {}, observer_valid {};
    std::thread pin([&] {
        const auto token = barrier.TryBeginNativeGuestDataLease();
        pin_valid = token != 0;
        pin_ready.Signal();
        pin_valid = release_pin.Wait() && pin_valid;
        pin_valid = barrier.EndNativeGuestDataLease(token) && pin_valid;
    });
    pass = pin_ready.Wait() && pass;
    std::thread writer([&] {
        const auto token = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
            [](std::uint64_t) noexcept { return true; },
            [](std::uint64_t) noexcept {}, kMutationTimeout);
        writer_valid = token != 0;
        writer_ready.Signal();
        writer_valid = release_writer.Wait() && writer_valid;
        if (token != 0) writer_valid = barrier.EndMutation(token) && writer_valid;
    });
    const auto wait_for = [&](auto predicate) {
        const auto deadline = GuestMemoryMutationBarrier::Clock::now() + kEventTimeout;
        do {
            if (predicate(barrier.SnapshotForTesting())) return true;
            std::this_thread::yield();
        } while (GuestMemoryMutationBarrier::Clock::now() < deadline);
        return predicate(barrier.SnapshotForTesting());
    };
    pass = wait_for([](const auto& state) {
        return state.fullIntent && state.nativeDataReaders == 1 && state.hostCallReaders == 1;
    }) && pass;
    const auto token = barrier.BeginFreshCallback();
    pass = token != 0 && !barrier.CurrentThreadOwnsHostMemoryReadLease() && pass;
    GuestMemoryMutationBarrier foreign;
    pass = !foreign.EnterExecution() && !foreign.EndFreshCallback(token) && pass;
    bool foreign_thread_denied {};
    std::thread wrong_thread([&] { foreign_thread_denied = !barrier.EndFreshCallback(token); });
    wrong_thread.join();
    pass = foreign_thread_denied && pass;
    std::thread observer([&] {
        observer_valid = wait_for([](const auto& state) {
            return state.fullIntent && state.waitingEntrants != 0 && state.hostCallReaders == 0;
        });
        release_pin.Signal();
        observer_valid = writer_ready.Wait() && observer_valid;
        observer_valid = wait_for([](const auto& state) {
            return state.writer && state.waitingEntrants != 0;
        }) && observer_valid;
        release_writer.Signal();
    });
    const bool entered = barrier.EnterExecution();
    pass = entered && pass;
    observer.join(); pin.join(); writer.join();
    pass = pin_valid && writer_valid && observer_valid && pass;
    if (entered) {
        pass = barrier.SuspendExecutionForHostCall() && pass;
        const auto inner = barrier.BeginFreshCallback();
        pass = inner != 0 && !barrier.EndFreshCallback(token) && pass;
        pass = barrier.EndFreshCallback(inner) && !barrier.EndFreshCallback(inner) && pass;
        pass = barrier.ResumeExecutionAfterHostCall() && pass;
        barrier.LeaveExecution();
    }
    pass = barrier.EndFreshCallback(token) && !barrier.EndFreshCallback(token) && pass;
    pass = barrier.CurrentThreadOwnsHostMemoryReadLease() && pass;
    pass = barrier.ResumeExecutionAfterHostCall() && pass;
    return pass;
}

bool FreshCallbackDepthIsBoundedAndRestoresEveryFrame() {
    GuestMemoryMutationBarrier barrier;
    ExecutionBarrierLease outer(barrier);
    if (!outer.Acquired() || !barrier.SuspendExecutionForHostCall()) return false;
    std::array<std::uint64_t, 32> tokens {};
    bool pass = true;
    for (auto& token: tokens) {
        token = barrier.BeginFreshCallback();
        if (token == 0 || !barrier.EnterExecution() || !barrier.SuspendExecutionForHostCall())
            return false;
    }
    pass = barrier.BeginFreshCallback() == 0 && barrier.CurrentThreadOwnsHostMemoryReadLease();
    for (auto iterator = tokens.rbegin(); iterator != tokens.rend(); ++iterator) {
        pass = barrier.ResumeExecutionAfterHostCall() && pass;
        barrier.LeaveExecution();
        pass = barrier.EndFreshCallback(*iterator) && pass;
    }
    return barrier.ResumeExecutionAfterHostCall() && pass;
}

bool CompletedEpochAdmitsWaitingCpuBeforeNextWriter() {
    enum class Entry { NewExecution, HostCallReturn, HostWaitReturn, FreshCallback };
    for (const bool split : {false, true}) {
        for (const bool targeted : {false, true}) {
            for (const auto entry : {Entry::NewExecution, Entry::HostCallReturn,
                                     Entry::HostWaitReturn, Entry::FreshCallback}) {
                GuestMemoryMutationBarrier barrier(split, targeted);
                struct ReadmissionGate {
                    ManualEvent entered, release;
                    std::atomic_bool passed {true};
                } gate;
                barrier.SetAdmissionReadmissionHookForTesting([](void* opaque) {
                    auto& held = *static_cast<ReadmissionGate*>(opaque);
                    held.entered.Signal();
                    if (!held.release.Wait()) held.passed.store(false, std::memory_order_release);
                }, &gate);
                ManualEvent prepared, tryAdmission, admitted, releaseCpu, writerArmed;
                std::atomic_bool setupPassed {false}, cpuPassed {false}, writersPassed {true};
                std::atomic_uint32_t completed {0}, arms {0}, activeWriters {0};
                std::thread cpu([&] {
                    bool ok = true;
                    std::uint64_t hostWait = 0, callback = 0;
                    if (entry != Entry::NewExecution) {
                        ok = barrier.EnterExecution() && barrier.SuspendExecutionForHostCall();
                        if (ok && entry == Entry::HostWaitReturn) {
                            hostWait = barrier.BeginQuiescentHostWait();
                            ok = hostWait != 0;
                        }
                        if (ok && entry == Entry::FreshCallback) {
                            callback = barrier.BeginFreshCallback();
                            ok = callback != 0;
                        }
                    }
                    setupPassed.store(ok, std::memory_order_release);
                    prepared.Signal();
                    ok = tryAdmission.Wait() && ok;
                    if (ok) {
                        switch (entry) {
                        case Entry::NewExecution:
                        case Entry::FreshCallback: ok = barrier.EnterExecution(); break;
                        case Entry::HostCallReturn: ok = barrier.ResumeExecutionAfterHostCall(); break;
                        case Entry::HostWaitReturn: ok = barrier.EndQuiescentHostWait(hostWait); break;
                        }
                    }
                    const bool ownsReader = ok;
                    ok = activeWriters.load(std::memory_order_acquire) == 0 && ok;
                    admitted.Signal();
                    ok = releaseCpu.Wait() && ok;
                    if (ownsReader) {
                        if (entry == Entry::HostWaitReturn)
                            ok = barrier.ResumeExecutionAfterHostCall() && ok;
                        barrier.LeaveExecution();
                        if (entry == Entry::FreshCallback) {
                            ok = barrier.EndFreshCallback(callback) && ok;
                            ok = barrier.ResumeExecutionAfterHostCall() && ok;
                            barrier.LeaveExecution();
                        }
                    }
                    cpuPassed.store(ok, std::memory_order_release);
                });
                bool ok = prepared.Wait() && setupPassed.load(std::memory_order_acquire);
                const auto first = barrier.BeginMutation(
                    [&](std::uint64_t) noexcept {
                        // Returning HLE releases its existing reader only after
                        // this request has closed execution admission.
                        if (entry == Entry::HostCallReturn) tryAdmission.Signal();
                        return true;
                    }, [](std::uint64_t) noexcept {}, kMutationTimeout);
                tryAdmission.Signal();
                ok = first != 0 && WaitForWaiters(barrier, 0, 0, 1) && ok;
                if (first != 0) ok = barrier.EndMutation(first) && ok;
                ok = gate.entered.Wait() && WaitForSnapshot(barrier, {0, 0, 1, 0, false}) && ok;

                // With no parked JIT reader, the old barrier admits this writer
                // immediately and overtakes the already-notified CPU. Holding
                // its admission registration makes that regression deterministic.
                unsigned earlyArms = 0, earlyDisarms = 0;
                MutationDenialDiagnostic denial {};
                const auto overtaking = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
                    [&](std::uint64_t) noexcept { ++earlyArms; return true; },
                    [&](std::uint64_t) noexcept { ++earlyDisarms; }, 0ns, &denial);
                ok = overtaking == 0 && earlyArms == 0 && earlyDisarms == 0 &&
                    denial.reason == MutationDenialReason::PendingMutationTimeout &&
                    WaitForSnapshot(barrier, {0, 0, 1, 0, false}) && ok;
                if (overtaking != 0) (void)barrier.EndMutation(overtaking);
                if (!ok) {
                    gate.release.Signal();
                    releaseCpu.Signal();
                    cpu.join();
                    std::fprintf(stderr, "FEX_BARRIER_ADMISSION_FAIRNESS split=%u targeted=%u entry=%u overtaken\n",
                        split ? 1u : 0u, targeted ? 1u : 0u, static_cast<unsigned>(entry));
                    return false;
                }

                std::array<std::thread, 2> writers;
                for (auto& writer : writers) writer = std::thread([&] {
                    const auto token = barrier.BeginMutation(
                        [&](std::uint64_t) noexcept {
                            arms.fetch_add(1, std::memory_order_release);
                            writerArmed.Signal();
                            return true;
                        }, [](std::uint64_t) noexcept {}, kMutationTimeout);
                    bool writerOk = token != 0;
                    if (token != 0) {
                        writerOk = activeWriters.fetch_add(1, std::memory_order_acq_rel) == 0;
                        // Nested ownership must remain unaffected by queued
                        // admissions and must not arm another epoch.
                        const auto nested = barrier.BeginMutation();
                        writerOk = nested == token + 1 && writerOk;
                        if (nested != 0) writerOk = barrier.EndMutation(nested) && writerOk;
                        activeWriters.fetch_sub(1, std::memory_order_release);
                        writerOk = barrier.EndMutation(token) && writerOk;
                    }
                    if (!writerOk) writersPassed.store(false, std::memory_order_release);
                    completed.fetch_add(1, std::memory_order_release);
                });
                ok = WaitForWaiters(barrier, 0, 2, 1) && arms.load(std::memory_order_acquire) == 0 && ok;
                gate.release.Signal();
                ok = admitted.Wait() && writerArmed.Wait() && ok;
                releaseCpu.Signal();
                cpu.join();
                for (auto& writer : writers) writer.join();
                ok = gate.passed.load(std::memory_order_acquire) && cpuPassed.load(std::memory_order_acquire) &&
                    writersPassed.load(std::memory_order_acquire) && completed.load(std::memory_order_acquire) == 2 &&
                    arms.load(std::memory_order_acquire) == 2 && activeWriters.load(std::memory_order_acquire) == 0 &&
                    WaitForSnapshot(barrier, {0, 0, 0, 0, false}) && WaitForWaiters(barrier, 0, 0, 0) && ok;
                if (!ok) std::fprintf(stderr, "FEX_BARRIER_ADMISSION_FAIRNESS split=%u targeted=%u entry=%u FAIL\n",
                    split ? 1u : 0u, targeted ? 1u : 0u, static_cast<unsigned>(entry));
                if (!ok) return false;
            }
        }
    }
    return true;
}

bool SplitNotificationsPreserveContendedEpochProgress() {
    constexpr unsigned cpuCount = 6;
    constexpr unsigned writerCount = 3;
    constexpr unsigned epochsPerWriter = 32;
    bool passed = true;
    for (const bool split : {false, true}) {
        GuestMemoryMutationBarrier barrier(split);
        ManualEvent startWriters, firstArmed, firstOwned, releaseFirst;
        std::atomic_uint32_t entered {0}, writersReady {0}, arms {0}, disarms {0};
        std::atomic_uint32_t completed {0}, published {0}, cpuSteps {0};
        std::atomic_uint32_t activeWriters {0};
        std::atomic_bool stop {false}, workersPassed {true};
        std::array<std::thread, cpuCount> cpus;
        std::array<std::thread, writerCount> writers;
        for (auto& cpu : cpus) cpu = std::thread([&] {
            const bool admitted = barrier.EnterExecution();
            entered.fetch_add(1, std::memory_order_release);
            if (!admitted || !firstArmed.Wait()) {
                workersPassed.store(false, std::memory_order_release);
                if (admitted) barrier.LeaveExecution();
                return;
            }
            while (!stop.load(std::memory_order_acquire)) {
                if (!barrier.CheckpointSafepoint() ||
                    activeWriters.load(std::memory_order_acquire) != 0) {
                    workersPassed.store(false, std::memory_order_release);
                    break;
                }
                const auto step = cpuSteps.fetch_add(1, std::memory_order_release);
                if ((step & 7u) == 0) {
                    const bool suspended = barrier.CheckpointAndSuspendExecutionForHostCall();
                    const auto wait = suspended ? barrier.BeginQuiescentHostWait() : 0;
                    std::this_thread::yield();
                    if (!suspended || wait == 0 || !barrier.EndQuiescentHostWait(wait) ||
                        !barrier.ResumeExecutionAfterHostCallAndCheckpoint()) {
                        workersPassed.store(false, std::memory_order_release);
                        break;
                    }
                }
                std::this_thread::yield();
            }
            barrier.LeaveExecution();
        });
        for (auto& writer : writers) writer = std::thread([&] {
            writersReady.fetch_add(1, std::memory_order_release);
            if (!startWriters.Wait()) {
                workersPassed.store(false, std::memory_order_release);
                return;
            }
            for (unsigned epoch = 0; epoch != epochsPerWriter; ++epoch) {
                const auto token = barrier.BeginMutation(
                    [&](std::uint64_t) noexcept {
                        if (arms.fetch_add(1, std::memory_order_acq_rel) == 0) firstArmed.Signal();
                        return true;
                    }, [](std::uint64_t) noexcept {}, kMutationTimeout);
                if (token == 0 || activeWriters.fetch_add(1, std::memory_order_acq_rel) != 0) {
                    workersPassed.store(false, std::memory_order_release);
                    break;
                }
                if (published.load(std::memory_order_acquire) == 0) {
                    firstOwned.Signal();
                    if (!releaseFirst.Wait()) workersPassed.store(false, std::memory_order_release);
                }
                const bool closed = barrier.PublishAndEndMutation(token, [&]() noexcept {
                    published.fetch_add(1, std::memory_order_release);
                    activeWriters.fetch_sub(1, std::memory_order_release);
                }, [&](std::uint64_t) noexcept { disarms.fetch_add(1, std::memory_order_release); });
                if (!closed) {
                    workersPassed.store(false, std::memory_order_release);
                    break;
                }
            }
            completed.fetch_add(1, std::memory_order_release);
        });
        const auto readyDeadline = GuestMemoryMutationBarrier::Clock::now() + kEventTimeout;
        while ((entered.load(std::memory_order_acquire) != cpuCount ||
                writersReady.load(std::memory_order_acquire) != writerCount) &&
               GuestMemoryMutationBarrier::Clock::now() < readyDeadline) std::this_thread::yield();
        bool ok = entered.load(std::memory_order_acquire) == cpuCount &&
            writersReady.load(std::memory_order_acquire) == writerCount;
        startWriters.Signal();
        ok = firstOwned.Wait() && ok;
        // The first writer is deliberately held: all six CPU readers really
        // parked on epoch 1, and both peer writers queued on that same epoch.
        ok = WaitForSnapshot(barrier, {0, cpuCount, cpuCount + writerCount - 1,
            1, true}) && ok;
        const auto before = barrier.NotificationsForTesting();
        ok = activeWriters.load(std::memory_order_acquire) == 1 &&
            published.load(std::memory_order_acquire) == 0 &&
            cpuSteps.load(std::memory_order_acquire) == 0 &&
            (split ? before.admission < before.mutation : before.admission == before.mutation) && ok;
        releaseFirst.Signal();
        for (auto& writer : writers) writer.join();
        stop.store(true, std::memory_order_release);
        for (auto& cpu : cpus) cpu.join();
        const auto after = barrier.NotificationsForTesting();
        ok = workersPassed.load(std::memory_order_acquire) &&
            completed.load(std::memory_order_acquire) == writerCount &&
            published.load(std::memory_order_acquire) == writerCount * epochsPerWriter &&
            arms.load(std::memory_order_acquire) == writerCount * epochsPerWriter &&
            disarms.load(std::memory_order_acquire) == writerCount * epochsPerWriter &&
            activeWriters.load(std::memory_order_acquire) == 0 &&
            cpuSteps.load(std::memory_order_acquire) != 0 &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false}) &&
            WaitForWaiters(barrier, 0, 0, 0) &&
            (split ? after.admission < after.mutation : after.admission == after.mutation) && ok;
        if (barrier.TargetedProgressForTesting()) {
            ok = after.ownerProgress + after.queuedProgress < after.mutation && ok;
        }
        std::printf("FEX_BARRIER_SPLIT_STRESS enabled=%u targeted=%u epochs=%u cpuSteps=%u mutationNotify=%llu admissionNotify=%llu ownerNotify=%llu queueNotify=%llu actualAdmissionNotify=%llu result=%s\n",
            split ? 1u : 0u, barrier.TargetedProgressForTesting() ? 1u : 0u,
            published.load(), cpuSteps.load(),
            static_cast<unsigned long long>(after.mutation),
            static_cast<unsigned long long>(after.admission),
            static_cast<unsigned long long>(after.ownerProgress),
            static_cast<unsigned long long>(after.queuedProgress),
            static_cast<unsigned long long>(after.actualAdmission), ok ? "PASS" : "FAIL");
        passed &= ok;
    }
    return passed;
}

bool TargetedProgressSkipsEmptyQueues() {
    GuestMemoryMutationBarrier barrier(true, true);
    bool passed = barrier.EnterExecution();
    for (unsigned boundary = 0; passed && boundary != 128; ++boundary) {
        passed = barrier.CheckpointAndSuspendExecutionForHostCall();
        const auto wait = passed ? barrier.BeginQuiescentHostWait() : 0;
        passed = wait != 0 && barrier.EndQuiescentHostWait(wait) && passed;
        passed = barrier.ResumeExecutionAfterHostCallAndCheckpoint() && passed;
    }
    const auto token = barrier.BeginMutation();
    passed = token != 0 && barrier.EndMutation(token) && passed;
    barrier.LeaveExecution();
    const auto notifications = barrier.NotificationsForTesting();
    const auto waiters = barrier.WaitersForTesting();
    return passed && notifications.mutation != 0 && notifications.admission != 0 &&
        notifications.ownerProgress == 0 && notifications.queuedProgress == 0 &&
        notifications.actualAdmission == 0 && waiters.ownerProgress == 0 &&
        waiters.queuedMutation == 0 && waiters.admission == 0 &&
        WaitForSnapshot(barrier, {0, 0, 0, 0, false});
}

bool TargetedProgressIsolatesOwnerDrainFromQueuedWriter() {
    bool passed = true;
    for (const bool split : {false, true}) {
        GuestMemoryMutationBarrier barrier(split, true);
        ManualEvent hostReady, releaseHost, armed, ownerReady, releaseOwner;
        bool hostPassed = false, ownerPassed = false, queuedPassed = false;
        std::thread host([&] {
            const bool entered = barrier.EnterExecution();
            const bool suspended = entered && barrier.SuspendExecutionForHostCall();
            hostReady.Signal();
            const bool released = releaseHost.Wait();
            const bool resumed = suspended && released && barrier.ResumeExecutionAfterHostCall();
            hostPassed = resumed;
            if (entered && (!suspended || resumed)) barrier.LeaveExecution();
        });
        bool ok = hostReady.Wait() && WaitForSnapshot(barrier, {0, 0, 0, 0, false, 1});
        std::thread owner([&] {
            const auto token = barrier.BeginMutation(
                [&](std::uint64_t) noexcept { armed.Signal(); return true; },
                [](std::uint64_t) noexcept {}, kMutationTimeout);
            ownerReady.Signal();
            const bool released = releaseOwner.Wait();
            ownerPassed = token != 0 && released && barrier.EndMutation(token);
        });
        ok = armed.Wait() && ok;
        std::thread queued([&] {
            const auto token = barrier.BeginMutation(
                [](std::uint64_t) noexcept { return true; },
                [](std::uint64_t) noexcept {}, kMutationTimeout);
            queuedPassed = token != 0 && barrier.EndMutation(token);
        });
        ok = WaitForWaiters(barrier, 1, 1, 0) && ok;
        const auto before = barrier.NotificationsForTesting();
        releaseHost.Signal();
        ok = ownerReady.Wait() && WaitForWaiters(barrier, 0, 1, 1) && ok;
        const auto afterDrain = barrier.NotificationsForTesting();
        // Reader release and entry to its admission wait notify only the
        // owner. The single writer-acquisition phase then wakes queued peers.
        ok = afterDrain.ownerProgress > before.ownerProgress &&
            afterDrain.queuedProgress == before.queuedProgress + 1 &&
            afterDrain.actualAdmission <= before.actualAdmission + 1 && ok;
        releaseOwner.Signal();
        owner.join();
        host.join();
        queued.join();
        passed = ok && hostPassed && ownerPassed && queuedPassed &&
            WaitForWaiters(barrier, 0, 0, 0) &&
            WaitForSnapshot(barrier, {0, 0, 0, 0, false}) && passed;
    }
    return passed;
}

bool InvalidationRangesCannotWrap() {
    constexpr auto unchanged = NormalizeGuestInvalidationRange(0x1000, 8);
    constexpr auto wrapped = NormalizeGuestInvalidationRange(UINT64_MAX - 3, 8);
    constexpr auto full = NormalizeGuestInvalidationRange(0, UINT64_MAX);
    constexpr auto empty = NormalizeGuestInvalidationRange(UINT64_MAX, 0);
    return unchanged.base == 0x1000 && unchanged.size == 8 &&
        wrapped.base == 0 && wrapped.size == UINT64_MAX &&
        full.base == 0 && full.size == UINT64_MAX &&
        empty.base == UINT64_MAX && empty.size == 0;
}

bool AdditiveScopeQueryChecksCurrentTokenAndInheritedScope() {
    GuestMemoryMutationBarrier barrier;
    const auto exclusive = barrier.BeginMutation();
    if (exclusive == 0) return false;
    const auto nested = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [](std::uint64_t) noexcept { return true; },
        [](std::uint64_t) noexcept {}, kMutationTimeout);
    bool pass = nested != 0 && barrier.OwnsMutation(nested) &&
        !barrier.OwnsAdditiveMutation(nested) &&
        !barrier.OwnsAdditiveMutation(exclusive);
    pass = nested != 0 && barrier.EndMutation(nested) && pass;
    pass = !barrier.OwnsAdditiveMutation(exclusive) &&
        barrier.EndMutation(exclusive) && pass;
    const auto additive = barrier.BeginMutation(
        GuestMemoryMutationScope::AdditiveMapping,
        [](std::uint64_t) noexcept { return true; },
        [](std::uint64_t) noexcept {}, kMutationTimeout);
    pass = additive != 0 && barrier.OwnsAdditiveMutation(additive) &&
        !barrier.OwnsAdditiveMutation(exclusive) && pass;
    GuestMemoryMutationBarrier foreign;
    pass = !foreign.OwnsAdditiveMutation(additive) && pass;
    bool peer_denied = false;
    std::thread peer([&] { peer_denied = !barrier.OwnsAdditiveMutation(additive); });
    peer.join();
    pass = peer_denied && barrier.EndMutation(additive) &&
        !barrier.OwnsAdditiveMutation(additive) && pass;
    return pass;
}

// The CPU is deliberately held after readmission: even two successful
// checkpoints cannot stand in for a retired guest block. All orchestration
// uses events and barrier queue witnesses, not scheduling sleeps.
bool GuestProgressWitnessBlocksZeroWorkRearming() {
    bool pass = true;
    for (const bool split : {false, true}) for (const bool targeted : {false, true}) {
        for (unsigned action = 0; action != 4; ++action) {
            GuestMemoryMutationBarrier barrier(split, targeted);
            const auto* pending = barrier.EnableGuestProgressWitness();
            const auto first = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
                [](std::uint64_t) { return true; }, [](std::uint64_t) {}, kMutationTimeout);
            ManualEvent ready, go, arm, published;
            bool cpuOk = true, writerOk = true;
            std::thread cpu([&] {
                cpuOk = barrier.EnterExecution();
                cpuOk = barrier.CheckpointSafepoint() && barrier.CheckpointSafepoint() && cpuOk;
                ready.Signal();
                cpuOk = go.Wait() && cpuOk;
                if (action == 0) {
                    barrier.ReportGuestProgress();
                    cpuOk = arm.Wait() && barrier.CheckpointSafepoint() && cpuOk;
                    barrier.LeaveExecution();
                } else if (action == 1) {
                    barrier.LeaveExecution();
                } else if (action == 2) {
                    cpuOk = barrier.SuspendExecutionForHostCall() && cpuOk;
                    cpuOk = arm.Wait() && cpuOk;
                    const auto wait = barrier.BeginQuiescentHostWait();
                    cpuOk = wait != 0 && published.Wait() && cpuOk;
                    cpuOk = barrier.EndQuiescentHostWait(wait) &&
                        barrier.ResumeExecutionAfterHostCall() && cpuOk;
                    barrier.LeaveExecution();
                } else {
                    const auto own = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
                        [](std::uint64_t) { return true; }, [](std::uint64_t) {}, kMutationTimeout);
                    cpuOk = own != 0 && barrier.EndMutation(own) && cpuOk;
                    barrier.LeaveExecution();
                }
            });
            pass = first != 0 && WaitForWaiters(barrier, 0, 0, 1) && pass;
            pass = barrier.EndMutation(first) && ready.Wait() && pending->load() == 1 && pass;
            // An unrelated thread's "progress" cannot pay this reader's debt.
            barrier.ReportGuestProgress();
            unsigned arms = 0;
            MutationDenialDiagnostic denial {};
            const auto denied = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
                [&](std::uint64_t) { ++arms; return true; }, [](std::uint64_t) {}, 5ms, &denial);
            const auto blocked = barrier.SnapshotForTesting();
            pass = denied == 0 && arms == 0 && !blocked.fullIntent && blocked.requestEpoch == 0 &&
                pending->load() == 1 && denial.reason == MutationDenialReason::PendingMutationTimeout && pass;
            std::thread writer;
            if (action != 3) {
                writer = std::thread([&] {
                    const auto token = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
                        [&](std::uint64_t) { arm.Signal(); return true; }, [](std::uint64_t) {}, kMutationTimeout);
                    writerOk = token != 0 && barrier.EndMutation(token);
                    published.Signal();
                });
                pass = WaitForWaiters(barrier, 0, 1, 0) && pass;
                const auto queued = barrier.SnapshotForTesting();
                pass = queued.requestEpoch == 0 && !queued.fullIntent && pending->load() == 1 && pass;
            }
            go.Signal();
            cpu.join();
            if (writer.joinable()) writer.join();
            pass = cpuOk && writerOk && pending->load() == 0 && pass;
        }
    }
    return pass;
}

bool CancelledEpochStillRequiresGuestProgress() {
    GuestMemoryMutationBarrier barrier;
    const auto* pending = barrier.EnableGuestProgressWitness();
    ManualEvent arm, cancel, readmitted, exit;
    bool readerOk = true, writerOk = true;
    std::thread writer([&] {
        const auto token = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
            [&](std::uint64_t) { arm.Signal(); cancel.Wait(); return false; },
            [](std::uint64_t) {}, kMutationTimeout);
        writerOk = token == 0;
    });
    bool pass = arm.Wait();
    std::thread reader([&] {
        readerOk = barrier.EnterExecution();
        readmitted.Signal();
        readerOk = exit.Wait() && readerOk;
        barrier.LeaveExecution();
    });
    pass = WaitForWaiters(barrier, 0, 0, 1) && pass;
    cancel.Signal();
    writer.join();
    pass = readmitted.Wait() && pending->load() == 1 && pass;
    unsigned arms = 0;
    const auto denied = barrier.BeginMutation(GuestMemoryMutationScope::AdditiveMapping,
        [&](std::uint64_t) { ++arms; return true; }, [](std::uint64_t) {}, 5ms);
    pass = denied == 0 && arms == 0 && pass;
    exit.Signal();
    reader.join();
    return pass && readerOk && writerOk && pending->load() == 0;
}

bool MutationDenialCapturesOnlyRetainedHostReaders() {
    GuestMemoryMutationBarrier barrier;
    MutationDenialDiagnostic diagnostic{};
    const auto capture = [&] {
        bool denied{};
        diagnostic = {};
        std::thread publisher([&] {
            const auto token = barrier.BeginMutation(GuestMemoryMutationScope::Exclusive,
                [](std::uint64_t) { return true; }, [](std::uint64_t) {}, 0ns, &diagnostic);
            denied = token == 0;
            if (token != 0) (void)barrier.EndMutation(token);
        });
        publisher.join();
        return denied;
    };
    if (!barrier.EnterExecution() || !barrier.SuspendExecutionForHostCall()) return false;
    barrier.SetCurrentHostCallDiagnostic(123, 456, "contended-native-rwlock");
    bool ok = capture() && diagnostic.reason == MutationDenialReason::QuiescenceTimeout &&
        diagnostic.hostCallReaders == 1 && diagnostic.hostReadersCaptured == 1 &&
        !diagnostic.hostReadersTruncated;
    const auto& first = diagnostic.hostReadersAtDenial[0];
    ok = first.nativeThread == 123 && first.operation == 456 && first.readersOwned == 1 &&
        first.identityStable && std::strcmp(first.name.data(), "contended-native-rwlock") == 0 && ok;

    // Nested frames share the thread's census entry but retain both leases.
    if (!barrier.EnterExecution() || !barrier.SuspendExecutionForHostCall()) return false;
    barrier.SetCurrentHostCallDiagnostic(123, 789, "nested-callback");
    ok = capture() && diagnostic.hostCallReaders == 2 && diagnostic.hostReadersCaptured == 1 &&
        diagnostic.hostReadersAtDenial[0].readersOwned == 2 &&
        diagnostic.hostReadersAtDenial[0].operation == 789 && ok;
    if (!barrier.ResumeExecutionAfterHostCall()) return false;
    barrier.LeaveExecution();
    barrier.SetCurrentHostCallDiagnostic(123, 456, "contended-native-rwlock");

    // Donated waits/callbacks and upgraded writers must never be blamed as
    // remaining readers. Ending each transition must restore the census.
    const auto wait = barrier.BeginQuiescentHostWait();
    if (wait == 0) return false;
    ok = !capture() && diagnostic.hostReadersCaptured == 0 && ok;
    if (!barrier.EndQuiescentHostWait(wait)) return false;
    const auto fresh = barrier.BeginFreshCallback();
    if (fresh == 0) return false;
    ok = !capture() && diagnostic.hostReadersCaptured == 0 && ok;
    if (!barrier.EndFreshCallback(fresh)) return false;
    const auto writer = barrier.BeginMutation();
    if (writer == 0) return false;
    ok = capture() && diagnostic.hostCallReaders == 0 &&
        diagnostic.hostReadersCaptured == 0 && ok;
    if (!barrier.EndMutation(writer)) return false;
    ok = capture() && diagnostic.hostCallReaders == 1 && diagnostic.hostReadersCaptured == 1 &&
        diagnostic.hostReadersAtDenial[0].operation == 456 && ok;
    barrier.SetCurrentHostCallDiagnostic(0, 0, nullptr);
    if (!barrier.ResumeExecutionAfterHostCall()) return false;
    barrier.LeaveExecution();
    return !capture() && ok;
}

struct TestCase {
    const char* name;
    bool (*run)();
};

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--targeted-selector-check") == 0 &&
        (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0)) {
        const bool expected = std::strcmp(argv[2], "1") == 0;
        GuestMemoryMutationBarrier first;
        const bool changed = setenv("LUMEN_FEX_BARRIER_TARGETED_PROGRESS",
                                    expected ? "0" : "1", 1) == 0;
        GuestMemoryMutationBarrier second;
        const bool passed = changed && first.TargetedProgressForTesting() == expected &&
            second.TargetedProgressForTesting() == expected;
        std::printf("FEX_BARRIER_TARGETED_SELECTOR expected=%u latched=%u result=%s\n",
            expected ? 1u : 0u, second.TargetedProgressForTesting() ? 1u : 0u,
            passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    if (argc == 3 && std::strcmp(argv[1], "--selector-check") == 0 &&
        (std::strcmp(argv[2], "0") == 0 || std::strcmp(argv[2], "1") == 0)) {
        const bool expected = std::strcmp(argv[2], "1") == 0;
        GuestMemoryMutationBarrier first;
        const bool changed = setenv("LUMEN_FEX_BARRIER_SPLIT_NOTIFICATIONS",
                                    expected ? "0" : "1", 1) == 0;
        GuestMemoryMutationBarrier second;
        const bool passed = changed && first.SplitNotificationsForTesting() == expected &&
            second.SplitNotificationsForTesting() == expected;
        std::printf("FEX_BARRIER_SPLIT_SELECTOR expected=%u latched=%u result=%s\n",
            expected ? 1u : 0u, second.SplitNotificationsForTesting() ? 1u : 0u,
            passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    if (argc != 1) return 2;
    ProcessWatchdog watchdog;
    constexpr TestCase tests[] {
        {"mutation denial exact host-reader census", &MutationDenialCapturesOnlyRetainedHostReaders},
        {"retired guest progress before writer rearming", &GuestProgressWitnessBlocksZeroWorkRearming},
        {"cancelled epoch progress before writer rearming", &CancelledEpochStillRequiresGuestProgress},
        {"execution read admission epoch", &ExecutionReadAdmissionEpochTracksEveryBoundary},
        {"host-call boundary pair state", &HostCallBoundaryStateMatchesOrdinaryPairs},
        {"host-call boundary publication and cancellation", &HostCallBoundaryPairsHonorPublicationAndCancellation},
        {"cooperative upgrade", &CooperativeUpgradeParksPeerAndBlocksNewEntrant},
        {"nested execution and mutation", &NestedExecutionAndMutationKeepOneReader},
        {"additive mapping with HLE reader", &AdditiveMappingCanPassHostMemoryReader},
        {"additive HLE owner with HLE peer", &AdditiveHostOwnerCanMapAcrossPeerHostCall},
        {"additive HLE owner after completed epoch", &AdditiveHostOwnerWaitsForCompletedEpochTail},
        {"exclusive owner top-level HLE", &ExclusiveOwnerCanUpgradeTopLevelHostCall},
        {"exclusive owner retained HLE fails fast", &ExclusiveOwnerFailsFastAcrossRetainedHostCall},
        {"exclusive pending rejects nested entry", &ExclusivePendingRejectsNestedEntryThenDrainsHostCall},
        {"exclusive intent drains retained execution", &ExclusiveIntentDrainsRetainedExecutionBeforeArm},
        {"exclusive intent timeout before arm", &ExclusiveIntentTimeoutDoesNotArmSafepoints},
        {"additive owner nested execution", &AdditiveOwnerCanUpgradeNestedExecution},
        {"exclusive mutation with HLE reader", &ExclusiveMutationWaitsForHostMemoryReader},
        {"quiescent wait token and context", &QuiescentWaitTokensAndContextAreChecked},
        {"host memory read ownership context", &HostMemoryReadOwnershipTracksCallingContext},
        {"quiescent wait retained HLE rejection", &QuiescentWaitRejectsRetainedOuterHostCall},
        {"quiescent wait publish before readmission", &QuiescentWaitReadmissionFollowsPublishAndDisarm},
        {"quiescent wait drains pending exclusive request", &QuiescentWaitCanDrainAnAlreadyPendingExclusiveRequest},
        {"quiescent wait additive mapping admission", &QuiescentWaitMayReadmitDuringAdditiveMapping},
        {"quiescent wait full intent admission", &QuiescentWaitReadmissionHonorsFullIntent},
        {"quiescent wait preserves other HLE readers", &QuiescentWaitDoesNotExemptOtherHostMemoryReaders},
        {"nested host-call suspension", &NestedHostCallSuspensionsBalanceReentrantExecution},
        {"nested mutation scope", &AdditiveWriterCannotNestExclusiveMutation},
        {"exclusive retirement effect-free denial and fresh admission", &ExclusiveRetirementRetryRequiresEffectFreeDenialAndFreshAdmission},
        {"competing mutators", &CompetingGuestMutatorsSerialize},
        {"timeout and late checkpoint", &TimeoutIsFailClosedAndLateCheckpointIsHarmless},
        {"mutation token affinity", &MutationTokensAreThreadAffineAndSingleUse},
        {"competing mutation queue deadline", &CompetingMutationQueueHonorsDeadlineBeforeArm},
        {"cross-barrier TLS isolation", &ThreadLocalStateCannotCrossBarrierInstances},
        {"guest data lease owning HLE reader", &GuestDataLeaseRequiresOwningHostCallReader},
        {"exclusive publication borrow closure, lifetime and transfer", &ExclusivePublicationBorrowKeepsEpochAndVmClosed},
        {"guest data leases prevent reader surrender and upgrade", &GuestDataLeasesPreventReaderSurrenderAndUpgrade},
        {"guest data lease excludes peer writer until HLE return", &GuestDataLeaseKeepsPeerWriterBehindHostCallReader},
        {"native data lease context affinity and nonce", &NativeDataLeaseChecksContextAffinityAndNonce},
        {"native downgrade effect-free unsupported owner rejection", &NativeDowngradeRejectsUnsupportedOwnersWithoutEffects},
        {"native downgrade atomic pin, disarm and CPU progress", &NativeDowngradePinsBeforeDisarmAndAllowsCpuProgress},
        {"native data lease CPU progress and VM lifetime", &NativeDataLeaseKeepsCpuRunningAndVmBehindPin},
        {"native data lease additive and exclusive timeout", &NativeDataLeaseAllowsAdditiveButExclusiveTimeoutHasNoEffects},
        {"fresh callback writer intent, borrow, affinity and LIFO", &FreshCallbackDrainsWriterAndChecksLifo},
        {"fresh callback bounded depth and full restore", &FreshCallbackDepthIsBoundedAndRestoresEveryFrame},
        {"completed epoch admits waiting CPU before next writer", &CompletedEpochAdmitsWaitingCpuBeforeNextWriter},
        {"split notifications with contended writers and parked CPUs", &SplitNotificationsPreserveContendedEpochProgress},
        {"targeted progress skips empty queues", &TargetedProgressSkipsEmptyQueues},
        {"targeted progress isolates owner drain from queued writer", &TargetedProgressIsolatesOwnerDrainFromQueuedWriter},
        {"invalidation range normalization", &InvalidationRangesCannotWrap},
        {"additive scope exact token and inherited scope", &AdditiveScopeQueryChecksCurrentTokenAndInheritedScope},
    };

    bool passed = true;
    for (const auto& test : tests) {
        const bool testPassed = test.run();
        if (!testPassed) {
            std::fprintf(stderr, "FAIL: %s\n", test.name);
        }
        passed = passed && testPassed;
    }

    std::puts(passed
        ? "PASS: FEX cooperative immutable-RX mutation barrier"
        : "FAIL: FEX cooperative immutable-RX mutation barrier");
    return passed ? 0 : 1;
}
