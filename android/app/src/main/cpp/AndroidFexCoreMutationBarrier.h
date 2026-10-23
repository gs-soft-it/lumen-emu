#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

namespace Lumen::Android::FexCoreLab {

struct GuestInvalidationRange {
    std::uint64_t base;
    std::uint64_t size;
};

enum class GuestMemoryMutationScope : std::uint8_t {
    Exclusive,
    AdditiveMapping,
};

constexpr GuestInvalidationRange NormalizeGuestInvalidationRange(
    std::uint64_t base, std::uint64_t size) noexcept {
    if (size != 0 &&
        base > (std::numeric_limits<std::uint64_t>::max)() - (size - 1)) {
        return {0, (std::numeric_limits<std::uint64_t>::max)()};
    }
    return {base, size};
}

// Cooperative reader/writer barrier for immutable guest code.
//
// ExecuteThread instances are JIT readers. A dispatched HLE callback becomes a
// host-memory reader: it no longer executes translated code, but it may still
// dereference guest pointers. Every mutation closes JIT admission and parks JIT
// readers. Destructive mutations also wait for peer host-memory readers;
// provably additive mappings may coexist with them. Guest execution re-entered
// from HLE retains an explicit hazard in addition to the outer frame lease. A
// bounded exclusive intent closes new re-entry and drains those hazards before
// safepoints are armed. The requesting reader is upgraded to the writer. The
// adapter disarms safepoints before the final commit wakes peers, so no
// execution can resume against a protected doorbell.
//
// An explicitly quiescent top-level host wait may temporarily release its
// host-memory reader after copying every guest input and releasing every VM
// lease. Its caller must use only independently live native objects until
// readmission, then revalidate guest pointers before accessing them again.
// Ordinary host calls remain readers, including unclassified blocking calls;
// their bounded mutation wait fails closed rather than deadlocking the runtime.
class GuestMemoryMutationBarrier final {
    // One TLS object keeps ownership and nesting thread-affine without a
    // separate emulated-TLS lookup for each field on Android.
    struct ThreadState {
        GuestMemoryMutationBarrier* executionBarrier_;
        GuestMemoryMutationBarrier* hostCallBarrier_;
        GuestMemoryMutationBarrier* mutationBarrier_;
        GuestMemoryMutationBarrier* requestOwnerBarrier_;
        GuestMemoryMutationBarrier* hostWaitBarrier_;
        GuestMemoryMutationBarrier* nativeDataBarrier_;
        std::uint64_t nativeDataToken_;
        std::uint64_t hostWaitToken_;
        std::uint32_t executionDepth_;
        std::uint32_t hostCallSuspendDepth_;
        std::uint32_t retainedHostCallExecutionDepth_;
        std::uint32_t mutationDepth_;
        std::uint32_t mutationNonce_;
        std::uint64_t mutationEpoch_;
        std::uint64_t requestOwnerEpoch_;
        bool executionCounted_;
        bool guestProgressOwed_;
        bool progressOnReadmission_;
        std::uint64_t executionReadAdmissionEpoch_;
        std::uint32_t hostCallReadersOwned_;
        std::uint32_t guestDataLeasesOwned_;
        bool mutationPublishing_;
        bool mutationUpgradedExecution_;
        bool mutationUpgradedHostCall_;
        GuestMemoryMutationScope mutationScope_;
        GuestMemoryMutationBarrier* freshCallbackBarrier_;
        std::array<std::uint64_t, 32> freshCallbackTokens_;
        std::uint32_t freshCallbackDepth_;
        bool shortHostCall_;
        bool demotedShortHostCall_;
        std::uint64_t shortHostCallEpoch_;
        ThreadState* previousHostReader_;
        ThreadState* nextHostReader_;
        std::atomic<std::uint64_t> diagnosticSequence_;
        std::atomic<std::uint64_t> diagnosticNativeThread_;
        std::atomic<std::uint64_t> diagnosticOperation_;
        std::atomic<const char*> diagnosticName_;
    };

public:
    using Clock = std::chrono::steady_clock;

    GuestMemoryMutationBarrier() noexcept
        : splitNotifications_(SplitNotificationsConfigured()),
          targetedProgress_(TargetedProgressConfigured()) {}

#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
    explicit GuestMemoryMutationBarrier(bool splitNotifications) noexcept
        : splitNotifications_(splitNotifications),
          targetedProgress_(TargetedProgressConfigured()) {}

    GuestMemoryMutationBarrier(bool splitNotifications, bool targetedProgress) noexcept
        : splitNotifications_(splitNotifications), targetedProgress_(targetedProgress) {}
#endif

    // The embedding enables this before publishing JIT code with retirement
    // witnesses. Admission alone is not progress: a woken CPU can still be in
    // PreCompile or returning from a safepoint without retiring an instruction.
    const std::atomic<std::uint32_t>* EnableGuestProgressWitness() noexcept {
        std::lock_guard lock(mutex_);
        guestProgressWitness_ = true;
        return &guestProgressPending_;
    }

    // Called only after completed guest work, never at a block entry, compile
    // checkpoint or safepoint return. The fast path is thread local; another
    // CPU's debt must not force this CPU to acquire the barrier mutex.
    void ReportGuestProgress() noexcept {
        auto& thread = threadState_;
        if (thread.executionBarrier_ != this || !thread.guestProgressOwed_) return;
        std::lock_guard lock(mutex_);
        ClearGuestProgressLocked(thread);
        NotifyMutationProgressLocked();
    }

    enum class MutationDenialReason : std::uint8_t {
        None,
        ShortHostCallActive,
        HostWaitActive,
        GuestDataLeaseActive,
        NativeDataLeaseActive,
        ExclusivePublicationBorrowActive,
        NestedMutationDenied,
        ForeignExecutionBarrier,
        ForeignHostCallBarrier,
        HostReaderOwnershipMismatch,
        NestedExclusiveMutation,
        CompetingExclusiveMutation,
        PendingMutationTimeout,
        RetainedExecutionTimeout,
        NativeDataLeaseTimeout,
        ArmSafepointsFailed,
        RequestEpochChanged,
        QuiescenceTimeout,
        ExecutionUpgradeMismatch,
        HostCallUpgradeMismatch,
    };

    struct HostReaderDiagnostic {
        std::uint64_t nativeThread;
        std::uint64_t operation;
        std::uint32_t readersOwned;
        bool identityStable;
        std::array<char, 96> name;
    };

    // Names are immutable registry storage, alive until all runtime calls end.
    // Updating diagnostic identity grants no admission and takes no barrier lock.
    void SetCurrentHostCallDiagnostic(std::uint64_t nativeThread,
        std::uint64_t operation, const char* name) noexcept {
        auto& thread = threadState_;
        const auto sequence = thread.diagnosticSequence_.load(std::memory_order_seq_cst);
        thread.diagnosticSequence_.store(sequence + 1, std::memory_order_seq_cst);
        thread.diagnosticNativeThread_.store(nativeThread, std::memory_order_seq_cst);
        thread.diagnosticOperation_.store(operation, std::memory_order_seq_cst);
        thread.diagnosticName_.store(name, std::memory_order_seq_cst);
        thread.diagnosticSequence_.store(sequence + 2, std::memory_order_seq_cst);
    }

    // Optional caller-owned failure record; no pointer is retained. Capture
    // ownership and epoch state under the mutex, before cancellation erases it.
    struct MutationDenialDiagnostic {
        MutationDenialReason reason;
        std::uint32_t executionDepth;
        std::uint32_t hostCallDepth;
        std::uint32_t hostCallReadersOwned;
        std::uint32_t retainedExecutionDepth;
        std::uint32_t guestDataLeases;
        std::uint32_t mutationDepth;
        GuestMemoryMutationScope mutationScope;
        bool mutationPublishing;
        bool mutationOwnerMatches;
        bool hostWaitActive;
        bool executionCounted;
        std::size_t readers;
        std::size_t hostCallReaders;
        std::size_t retainedExecutions;
        std::size_t parked;
        std::size_t waitingEntrants;
        std::uint64_t requestEpoch;
        GuestMemoryMutationScope requestScope;
        bool writer;
        bool fullIntent;
        std::array<HostReaderDiagnostic, 32> hostReadersAtDenial;
        std::size_t hostReadersCaptured;
        bool hostReadersTruncated;
    };

    // This classifies an effect-free denial, not permission to drop a reader.
    // The embedding must additionally identify its own top-level HLE run and
    // the caller must release every local lock and guest pointer before waiting.
    static bool MutationDenialAllowsQuiescentRetry(
        const MutationDenialDiagnostic& diagnostic) noexcept {
        return diagnostic.reason == MutationDenialReason::CompetingExclusiveMutation &&
            diagnostic.executionDepth == 0 && !diagnostic.executionCounted &&
            diagnostic.hostCallDepth == 1 && diagnostic.hostCallReadersOwned == 1 &&
            diagnostic.retainedExecutionDepth == 0 && diagnostic.guestDataLeases == 0 &&
            diagnostic.mutationDepth == 0 && !diagnostic.mutationPublishing &&
            !diagnostic.mutationOwnerMatches && !diagnostic.hostWaitActive;
    }

    enum class BarrierFailurePhase : std::uint8_t {
        None,
        ExecutionEntry,
        Checkpoint,
        HostCallSuspend,
        HostCallResume,
    };

    // Optional caller-owned POD, written only on failure under the same lock
    // as the failed transition. No pointer is retained and admission is unchanged.
    struct BarrierFailureDiagnostic {
        BarrierFailurePhase phase;
        std::uint32_t executionDepth;
        std::uint32_t hostCallDepth;
        std::uint32_t hostCallReadersOwned;
        std::uint32_t retainedExecutionDepth;
        std::uint32_t guestDataLeases;
        std::uint32_t mutationDepth;
        GuestMemoryMutationScope mutationScope;
        bool mutationPublishing;
        bool executionOwnerMatches;
        bool hostCallOwnerMatches;
        bool mutationOwnerMatches;
        bool requestOwnerMatches;
        bool hostWaitOwnerMatches;
        bool hostWaitActive;
        bool executionCounted;
        std::uint64_t mutationEpoch;
        std::uint64_t ownerRequestEpoch;
        std::size_t readers;
        std::size_t hostCallReaders;
        std::size_t retainedExecutions;
        std::size_t parked;
        std::size_t waitingEntrants;
        std::uint64_t requestEpoch;
        GuestMemoryMutationScope requestScope;
        bool writer;
        bool fullIntent;
    };

    bool EnterExecution(BarrierFailureDiagnostic* diagnostic = nullptr) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        if (thread.shortHostCall_ || thread.mutationDepth_ != 0 || thread.hostWaitToken_ != 0 ||
            thread.guestDataLeasesOwned_ != 0 || thread.nativeDataToken_ != 0 ||
            (thread.freshCallbackDepth_ != 0 && thread.freshCallbackBarrier_ != this)) {
            RecordFailureLocked(diagnostic, BarrierFailurePhase::ExecutionEntry);
            return false;
        }
        if (thread.executionDepth_ != 0) {
            if (thread.executionBarrier_ != this ||
                thread.executionDepth_ ==
                    (std::numeric_limits<std::uint32_t>::max)()) {
                RecordFailureLocked(diagnostic, BarrierFailurePhase::ExecutionEntry);
                return false;
            }
            ++thread.executionDepth_;
            return true;
        }
        if (thread.hostCallSuspendDepth_ != 0 && thread.hostCallBarrier_ != this) {
            RecordFailureLocked(diagnostic, BarrierFailurePhase::ExecutionEntry);
            return false;
        }
        if (thread.hostCallSuspendDepth_ != 0) {
            if (thread.retainedHostCallExecutionDepth_ ==
                    (std::numeric_limits<std::uint32_t>::max)() ||
                retainedHostCallExecutions_ ==
                    (std::numeric_limits<std::size_t>::max)() ||
                !WaitForNestedExecutionAdmissionLocked(lock)) {
                RecordFailureLocked(diagnostic, BarrierFailurePhase::ExecutionEntry);
                return false;
            }
            ++thread.retainedHostCallExecutionDepth_;
            ++retainedHostCallExecutions_;
        } else {
            if (thread.freshCallbackDepth_ != 0) {
                // A detached callback is a new root, never an exception to a
                // previously reserved exclusive writer's lifetime drain.
                if (fullIntent_ || requestEpoch_ != 0 || writer_) {
                    ++waitingEntrants_;
                    NotifyMutationProgressLocked();
                    WaitForAdmissionConditionLocked(lock, [this] {
                        return !fullIntent_ && requestEpoch_ == 0 && !writer_;
                    });
                    --waitingEntrants_;
                    thread.progressOnReadmission_ = true;
                    NotifyMutationProgressLocked();
                }
            } else WaitForAdmissionLocked(lock);
        }
        thread.executionBarrier_ = this;
        thread.executionDepth_ = 1;
        SetExecutionCounted(thread, true);
        ++readers_;
        NotifyMutationProgressLocked();
        return true;
    }

    void LeaveExecution() noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        if (thread.shortHostCall_ || thread.executionDepth_ == 0 || thread.executionBarrier_ != this) {
            return;
        }
        if (--thread.executionDepth_ != 0) {
            return;
        }

        thread.executionBarrier_ = nullptr;
        if (thread.executionCounted_) {
            SetExecutionCounted(thread, false);
            if (readers_ != 0) {
                --readers_;
            }
        }
        if (thread.retainedHostCallExecutionDepth_ != 0) {
            --thread.retainedHostCallExecutionDepth_;
            if (retainedHostCallExecutions_ != 0) {
                --retainedHostCallExecutions_;
            }
        }
        NotifyMutationProgressLocked();
    }

    // A dispatched HLE callback is outside generated guest execution, but it
    // can still dereference guest memory. Move the thread atomically from the
    // JIT-reader set to the host-memory-reader set. Additive mappings may ignore
    // that second set; destructive mutations may not.
    bool SuspendExecutionForHostCall(BarrierFailureDiagnostic* diagnostic = nullptr) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        return SuspendExecutionForHostCallLocked(lock, thread, diagnostic);
    }

    bool ResumeExecutionAfterHostCall(BarrierFailureDiagnostic* diagnostic = nullptr) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        return ResumeExecutionAfterHostCallLocked(lock, thread, diagnostic);
    }

    // The runtime normally checkpoints immediately before entering HLE and
    // immediately after returning. Keep the same state transitions under one
    // mutex acquisition per boundary. Pending requests still park/wait through
    // the ordinary helpers; no reader protection is dropped between calls.
    bool CheckpointAndSuspendExecutionForHostCall(BarrierFailureDiagnostic* diagnostic = nullptr) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        return CheckpointSafepointLocked(lock, thread, diagnostic) &&
            SuspendExecutionForHostCallLocked(lock, thread, diagnostic);
    }

    bool ResumeExecutionAfterHostCallAndCheckpoint(BarrierFailureDiagnostic* diagnostic = nullptr) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        return ResumeExecutionAfterHostCallLocked(lock, thread, diagnostic) &&
            CheckpointSafepointLocked(lock, thread, diagnostic);
    }

    // Only an audited, bounded HLE may use this path. The runtime admits None,
    // or an explicit synchronous GuestMemory contract solely under EXCLUSIVE
    // publication; blocking, callbacks, retention and VM upgrades require an
    // ordinary demotion before their effects and before taking local locks.
    // Keep the JIT reader
    // already registered by EnterExecution: no unregistered reader is admitted
    // by the atomic check. A writer racing the check still sees that reader
    // under mutex_ and cannot publish until End checkpoints it. This deliberately
    // leaves even additive mappings waiting for the very short call to finish.
    bool TryBeginShortHostCall() noexcept {
        auto& thread = threadState_;
        if (thread.shortHostCall_ || thread.executionBarrier_ != this ||
            thread.executionDepth_ != 1 || !thread.executionCounted_ ||
            thread.hostCallSuspendDepth_ != 0 || thread.hostCallBarrier_ != nullptr ||
            thread.retainedHostCallExecutionDepth_ != 0 || thread.mutationDepth_ != 0 ||
            thread.requestOwnerBarrier_ != nullptr || thread.hostWaitToken_ != 0 ||
            thread.guestDataLeasesOwned_ != 0 || thread.nativeDataToken_ != 0 ||
            thread.freshCallbackDepth_ != 0) return false;
        const auto epoch = shortAdmissionEpoch_.load(std::memory_order_acquire);
        if ((epoch & 1U) != 0) return false;
        AdvanceExecutionReadAdmissionEpoch(thread);
        thread.shortHostCallEpoch_ = epoch;
        thread.shortHostCall_ = true;
        return true;
    }

    bool EndShortHostCall(BarrierFailureDiagnostic* diagnostic = nullptr) noexcept {
        auto& thread = threadState_;
        if (thread.demotedShortHostCall_) {
            std::unique_lock lock(mutex_);
            // Only the original outer boundary may finish a downgrade; an
            // inner HLE/callback must not consume its marker or reader.
            if (thread.hostCallBarrier_ != this || thread.hostCallSuspendDepth_ != 1 ||
                thread.retainedHostCallExecutionDepth_ != 0 ||
                !ResumeExecutionAfterHostCallLocked(lock, thread, diagnostic) ||
                !CheckpointSafepointLocked(lock, thread, diagnostic)) {
                RecordFailureLocked(diagnostic, BarrierFailurePhase::HostCallResume);
                return false;
            }
            thread.demotedShortHostCall_ = false;
            return true;
        }
        if (!thread.shortHostCall_ || thread.executionBarrier_ != this ||
            thread.executionDepth_ != 1 || !thread.executionCounted_ ||
            thread.guestDataLeasesOwned_ != 0) {
            std::lock_guard lock(mutex_);
            RecordFailureLocked(diagnostic, BarrierFailurePhase::HostCallResume);
            return false;
        }
        thread.shortHostCall_ = false;
        AdvanceExecutionReadAdmissionEpoch(thread);
        // A change (including intent cancellation) takes the ordinary locked
        // checkpoint. Publish/disarm remains wholly inside the existing writer
        // transaction; no guest execution is readmitted through this load.
        if (shortAdmissionEpoch_.load(std::memory_order_acquire) ==
            thread.shortHostCallEpoch_) return true;
        return CheckpointSafepoint(diagnostic);
    }

    // The bridge can discover asynchronous work only after initial admission
    // (e.g. a queued guest exception). Downgrade before consuming that work or
    // touching its guest state. The final short-call End balances this ordinary
    // frame after any nested callback has returned. No work is silently deferred.
    bool EnsureOrdinaryHostCallAdmission() noexcept {
        auto& thread = threadState_;
        if (!thread.shortHostCall_) return true;
        if (thread.guestDataLeasesOwned_ != 0) return false;
        std::unique_lock lock(mutex_);
        thread.shortHostCall_ = false;
        if (!CheckpointSafepointLocked(lock, thread, nullptr) ||
            !SuspendExecutionForHostCallLocked(lock, thread, nullptr)) {
            thread.shortHostCall_ = true;
            return false;
        }
        thread.demotedShortHostCall_ = true;
        return true;
    }

    // This is a caller promise, not an automatic classification of HLE. The
    // complete top-level callback must have no outstanding guest-pointer or
    // VM leases, and the wait must not call guest code. Retained outer HLE
    // frames cannot make that promise on behalf of their callers.
    std::uint64_t BeginQuiescentHostWait() noexcept {
        auto& thread = threadState_;
        std::lock_guard lock(mutex_);
        if (thread.hostWaitToken_ != 0 || thread.mutationDepth_ != 0 ||
            thread.guestDataLeasesOwned_ != 0 || thread.nativeDataToken_ != 0 ||
            thread.requestOwnerBarrier_ != nullptr ||
            thread.executionDepth_ != 0 || thread.executionBarrier_ != nullptr ||
            thread.executionCounted_ || thread.hostCallBarrier_ != this ||
            thread.hostCallSuspendDepth_ != 1 || thread.hostCallReadersOwned_ != 1 ||
            thread.retainedHostCallExecutionDepth_ != 0 || hostCallReaders_ == 0 ||
            nextHostWaitToken_ ==
                (std::numeric_limits<std::uint64_t>::max)()) {
            return 0;
        }

        // Do not wait for admission here: releasing the last reader may be
        // precisely what an already pending exclusive request needs.
        thread.hostWaitToken_ = ++nextHostWaitToken_;
        thread.hostWaitBarrier_ = this;
        RemoveHostReaderLocked(thread);
        NotifyMutationProgressLocked();
        return thread.hostWaitToken_;
    }

    bool EndQuiescentHostWait(std::uint64_t token) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        if (token == 0 || token != thread.hostWaitToken_ ||
            thread.hostWaitBarrier_ != this || thread.mutationDepth_ != 0 ||
            thread.executionDepth_ != 0 || thread.executionBarrier_ != nullptr ||
            thread.executionCounted_ || thread.hostCallBarrier_ != this ||
            thread.hostCallSuspendDepth_ != 1 || thread.hostCallReadersOwned_ != 0 ||
            thread.retainedHostCallExecutionDepth_ != 0) {
            return false;
        }

        // Full intent must block pointer access even before safepoints are
        // armed. Ordinary execution admission alone does not cover that phase.
        WaitForHostCallAdmissionLocked(lock, thread);
        AddHostReaderLocked(thread);
        thread.hostWaitToken_ = 0;
        thread.hostWaitBarrier_ = nullptr;
        NotifyMutationProgressLocked();
        return true;
    }

    // Explicit synchronous callback handoff, distinct from a host wait. The
    // caller has copied every guest input to native scalar/snapshot state and
    // holds no VM lease or local lock across the callback. The embedding must
    // revalidate the suspended CPU's code, stack and TLS after readmission.
    // Removing the complete outer HLE frame makes the callback a fresh root;
    // its execution and host calls use ordinary admission, including full
    // writer intent. Neither this token nor a host-wait token lends memory.
    std::uint64_t BeginFreshCallback() noexcept {
        auto& thread = threadState_;
        std::lock_guard lock(mutex_);
        if (thread.hostWaitToken_ != 0 || thread.mutationDepth_ != 0 ||
            thread.guestDataLeasesOwned_ != 0 || thread.nativeDataToken_ != 0 ||
            thread.requestOwnerBarrier_ != nullptr || thread.executionDepth_ != 0 ||
            thread.executionBarrier_ != nullptr || thread.executionCounted_ ||
            thread.hostCallBarrier_ != this || thread.hostCallSuspendDepth_ != 1 ||
            thread.hostCallReadersOwned_ != 1 ||
            thread.retainedHostCallExecutionDepth_ != 0 || hostCallReaders_ == 0 ||
            (thread.freshCallbackDepth_ != 0 && thread.freshCallbackBarrier_ != this) ||
            thread.freshCallbackDepth_ == thread.freshCallbackTokens_.size() ||
            nextFreshCallbackToken_ == (std::numeric_limits<std::uint64_t>::max)())
            return 0;
        const auto token = ++nextFreshCallbackToken_;
        thread.freshCallbackBarrier_ = this;
        thread.freshCallbackTokens_[thread.freshCallbackDepth_++] = token;
        thread.hostCallBarrier_ = nullptr;
        thread.hostCallSuspendDepth_ = 0;
        RemoveHostReaderLocked(thread);
        NotifyMutationProgressLocked();
        return token;
    }

    bool EndFreshCallback(std::uint64_t token) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        if (token == 0 || thread.freshCallbackBarrier_ != this ||
            thread.freshCallbackDepth_ == 0 ||
            thread.freshCallbackTokens_[thread.freshCallbackDepth_ - 1] != token ||
            thread.hostWaitToken_ != 0 || thread.mutationDepth_ != 0 ||
            thread.guestDataLeasesOwned_ != 0 || thread.nativeDataToken_ != 0 ||
            thread.requestOwnerBarrier_ != nullptr || thread.executionDepth_ != 0 ||
            thread.executionBarrier_ != nullptr || thread.executionCounted_ ||
            thread.hostCallBarrier_ != nullptr || thread.hostCallSuspendDepth_ != 0 ||
            thread.hostCallReadersOwned_ != 0 || thread.retainedHostCallExecutionDepth_ != 0)
            return false;
        WaitForHostCallAdmissionLocked(lock, thread);
        thread.hostCallBarrier_ = this;
        thread.hostCallSuspendDepth_ = 1;
        AddHostReaderLocked(thread);
        thread.freshCallbackTokens_[--thread.freshCallbackDepth_] = 0;
        if (thread.freshCallbackDepth_ == 0) thread.freshCallbackBarrier_ = nullptr;
        NotifyMutationProgressLocked();
        return true;
    }

    // Thread-local admission proof only. The calling HLE retains its counted
    // host-memory reader until it explicitly changes this thread's state.
    // Nested HLE frames are eligible; quiescent waits and writer upgrades are
    // not. This must not be replaced by optional diagnostic ActiveRun flags.
    bool CurrentThreadOwnsHostMemoryReadLease() const noexcept {
        auto& thread = threadState_;
        return thread.hostCallBarrier_ == this && thread.hostCallSuspendDepth_ != 0 &&
            thread.hostCallReadersOwned_ != 0 && thread.executionDepth_ == 0 &&
            thread.executionBarrier_ == nullptr && !thread.executionCounted_ &&
            thread.hostWaitToken_ == 0 && thread.hostWaitBarrier_ == nullptr &&
            thread.mutationDepth_ == 0 && thread.requestOwnerBarrier_ == nullptr;
    }

    bool CurrentThreadOwnsExecutionLease() const noexcept {
        const auto& thread = threadState_;
        return thread.executionBarrier_ == this && thread.executionDepth_ != 0 &&
            thread.executionCounted_;
    }

    // Reusable metadata proofs belong only to this counted execution
    // admission. Ordinary HLE readers allow additive mappings and return zero.
    // Saturation permanently disables reuse rather than repeating an epoch.
    std::uint64_t CurrentThreadExecutionReadAdmissionEpoch() const noexcept {
        const auto& thread = threadState_;
        return CurrentThreadOwnsExecutionLease() && !thread.shortHostCall_ && !thread.demotedShortHostCall_ &&
            thread.mutationDepth_ == 0 &&
            thread.requestOwnerBarrier_ == nullptr && thread.hostWaitToken_ == 0 &&
            thread.executionReadAdmissionEpoch_ != (std::numeric_limits<std::uint64_t>::max)()
            ? thread.executionReadAdmissionEpoch_ : 0;
    }

    bool CurrentThreadOwnsExecutionOrMutationLease() const noexcept {
        const auto& thread = threadState_;
        return CurrentThreadOwnsExecutionLease() ||
            (thread.mutationBarrier_ == this && thread.mutationDepth_ != 0 &&
                !thread.mutationPublishing_);
    }

    bool CurrentThreadOwnsShortHostCallLease() const noexcept {
        const auto& thread = threadState_;
        return thread.shortHostCall_ && thread.executionBarrier_ == this &&
            thread.executionDepth_ == 1 && thread.executionCounted_;
    }

    // A Core guest-data borrow retains the existing HLE or short-call execution
    // reader (the bridge still reads its CALL return slot). It creates
    // no independent permission or lifetime authority outside that callback.
    // Until every borrow ends, this thread cannot give up or upgrade its
    // reader, enter guest code, or return across the HLE boundary.
    bool RetainGuestDataLease() noexcept {
        auto& thread = threadState_;
        if ((!CurrentThreadOwnsHostMemoryReadLease() &&
             !(thread.shortHostCall_ && thread.executionBarrier_ == this && thread.executionCounted_)) ||
            thread.guestDataLeasesOwned_ ==
                (std::numeric_limits<std::uint32_t>::max)()) return false;
        ++thread.guestDataLeasesOwned_;
        return true;
    }

    bool ReleaseGuestDataLease() noexcept {
        auto& thread = threadState_;
        if ((!CurrentThreadOwnsHostMemoryReadLease() &&
             !(thread.shortHostCall_ && thread.executionBarrier_ == this && thread.executionCounted_)) ||
            thread.guestDataLeasesOwned_ == 0) return false;
        --thread.guestDataLeasesOwned_;
        return true;
    }

    // Independent native data admission, never a fabricated JIT/HLE reader.
    // This nonblocking pin stabilizes existing mappings while ordinary guest
    // execution continues. It grants no logical CPU/GPU rights and no coherent
    // snapshot of bytes written concurrently by translated code. Every pointer
    // and physical executable alias must still pass its ordinary range proof.
    // Provably additive mappings may coexist; replacing/protect/unmap/reuse
    // mutations wait for every native pin before they arm JIT safepoints.
    std::uint64_t TryBeginNativeGuestDataLease(const char** denial = nullptr,
        bool* mutation_pending = nullptr) noexcept {
        if (denial != nullptr) *denial = nullptr;
        if (mutation_pending != nullptr) *mutation_pending = false;
        auto& thread = threadState_;
        std::lock_guard lock(mutex_);
        const auto reject = [&](const char* reason) -> std::uint64_t {
            if (denial != nullptr) *denial = reason;
            return 0;
        };
        if (thread.nativeDataBarrier_ != nullptr || thread.nativeDataToken_ != 0 ||
            thread.executionDepth_ != 0 || thread.executionBarrier_ != nullptr ||
            thread.executionCounted_ || thread.hostCallSuspendDepth_ != 0 ||
            thread.hostCallBarrier_ != nullptr || thread.hostCallReadersOwned_ != 0 ||
            thread.retainedHostCallExecutionDepth_ != 0 || thread.guestDataLeasesOwned_ != 0 ||
            thread.mutationDepth_ != 0 || thread.mutationBarrier_ != nullptr ||
            thread.requestOwnerBarrier_ != nullptr || thread.hostWaitToken_ != 0 ||
            thread.hostWaitBarrier_ != nullptr) return reject("context-invalid");
        if (fullIntent_ || requestEpoch_ != 0 || writer_) {
            if (mutation_pending != nullptr) *mutation_pending = true;
            return reject("mutation-pending");
        }
        if (nextNativeDataToken_ == (std::numeric_limits<std::uint64_t>::max)() ||
            nativeDataReaders_ == (std::numeric_limits<std::size_t>::max)()) return reject("capacity-exhausted");
        thread.nativeDataBarrier_ = this;
        thread.nativeDataToken_ = ++nextNativeDataToken_;
        ++nativeDataReaders_;
        NotifyMutationProgressLocked();
        return thread.nativeDataToken_;
    }

    bool EndNativeGuestDataLease(std::uint64_t token) noexcept {
        auto& thread = threadState_;
        std::lock_guard lock(mutex_);
        if (token == 0 || thread.nativeDataBarrier_ != this ||
            thread.nativeDataToken_ != token || nativeDataReaders_ == 0) return false;
        thread.nativeDataToken_ = 0;
        thread.nativeDataBarrier_ = nullptr;
        --nativeDataReaders_;
        NotifyMutationProgressLocked();
        return true;
    }

    bool OwnsNativeGuestDataLease(std::uint64_t token) const noexcept {
        const auto& thread = threadState_;
        return token != 0 && thread.nativeDataBarrier_ == this &&
            thread.nativeDataToken_ == token;
    }

    // Transfer a native worker's outer exclusive epoch into mapping lifetime
    // protection without an unprotected gap. Guest execution may resume after
    // disarm, but destructive VM changes still wait for the returned pin. The
    // caller must publish its separate byte-access claims before this transfer;
    // the pin itself grants neither coherent bytes nor CPU/GPU access rights.
    // Rejection is effect-free, leaving the exclusive token with its owner.
    template<typename DisarmSafepoints>
    std::uint64_t DowngradeExclusiveToNativeGuestDataLease(
        std::uint64_t token, DisarmSafepoints&& disarmSafepoints) noexcept {
        auto& thread = threadState_;
        std::uint64_t epoch = 0;
        std::uint64_t nativeToken = 0;
        {
            std::lock_guard lock(mutex_);
            if (!OwnsMutationLocked(token) || !writer_ ||
                thread.mutationDepth_ != 1 ||
                thread.mutationScope_ != GuestMemoryMutationScope::Exclusive ||
                thread.mutationPublishing_ || thread.mutationUpgradedExecution_ ||
                thread.mutationUpgradedHostCall_ ||
                thread.mutationEpoch_ == 0 || requestEpoch_ != thread.mutationEpoch_ ||
                thread.executionDepth_ != 0 || thread.executionBarrier_ != nullptr ||
                thread.executionCounted_ || thread.hostCallSuspendDepth_ != 0 ||
                thread.hostCallBarrier_ != nullptr || thread.hostCallReadersOwned_ != 0 ||
                thread.retainedHostCallExecutionDepth_ != 0 || thread.guestDataLeasesOwned_ != 0 ||
                thread.nativeDataBarrier_ != nullptr || thread.nativeDataToken_ != 0 ||
                thread.hostWaitToken_ != 0 || thread.hostWaitBarrier_ != nullptr ||
                thread.freshCallbackDepth_ != 0 || thread.shortHostCall_ ||
                exclusivePublicationBorrowCount_ != 0 ||
                nextNativeDataToken_ == (std::numeric_limits<std::uint64_t>::max)() ||
                nativeDataReaders_ == (std::numeric_limits<std::size_t>::max)()) return 0;
            epoch = thread.mutationEpoch_;
            nativeToken = ++nextNativeDataToken_;
            ++nativeDataReaders_;
            // No owner API can end/nest this mutation during disarm. Publish
            // the TLS pin only at commit, so a reentrant callback cannot release
            // the reserved mapping protection before execution is readmitted.
            thread.mutationPublishing_ = true;
        }

        disarmSafepoints(epoch);

        std::lock_guard lock(mutex_);
        thread.nativeDataBarrier_ = this;
        thread.nativeDataToken_ = nativeToken;
        thread.mutationPublishing_ = false;
        (void)FinishMutationLocked(); // Validated outer token cannot change during disarm.
        return nativeToken;
    }

    std::uint64_t DowngradeExclusiveToNativeGuestDataLease(std::uint64_t token) noexcept {
        return DowngradeExclusiveToNativeGuestDataLease(token, [](std::uint64_t) noexcept {});
    }

    // Called at a FEX-safe boundary. The execution temporarily stops counting
    // as an active reader for every mutation epoch that is already pending,
    // then readmits itself only after all such epochs have completed.
    bool CheckpointSafepoint(BarrierFailureDiagnostic* diagnostic = nullptr) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        return CheckpointSafepointLocked(lock, thread, diagnostic);
    }

    template<typename ArmSafepoints, typename DisarmSafepoints,
             typename Rep, typename Period>
    std::uint64_t BeginMutation(
        GuestMemoryMutationScope scope,
        ArmSafepoints&& armSafepoints,
        DisarmSafepoints&& disarmSafepoints,
        std::chrono::duration<Rep, Period> timeout,
        MutationDenialDiagnostic* diagnostic = nullptr) noexcept {
        auto& thread = threadState_;
        std::unique_lock lock(mutex_);
        const auto recordDenial = [&](MutationDenialReason reason) noexcept {
            if (diagnostic == nullptr) return;
            *diagnostic = {
                reason, thread.executionDepth_, thread.hostCallSuspendDepth_,
                thread.hostCallReadersOwned_, thread.retainedHostCallExecutionDepth_,
                thread.guestDataLeasesOwned_, thread.mutationDepth_,
                thread.mutationScope_, thread.mutationPublishing_,
                thread.mutationBarrier_ == this, thread.hostWaitToken_ != 0,
                thread.executionCounted_, readers_, hostCallReaders_,
                retainedHostCallExecutions_, parked_, waitingEntrants_,
                requestEpoch_, requestScope_, writer_, fullIntent_, {}, 0, false,
            };
            for (const auto* reader = hostReaderHead_; reader != nullptr;
                 reader = reader->nextHostReader_) {
                if (diagnostic->hostReadersCaptured == diagnostic->hostReadersAtDenial.size()) {
                    diagnostic->hostReadersTruncated = true;
                    break;
                }
                auto& output = diagnostic->hostReadersAtDenial[diagnostic->hostReadersCaptured++];
                output.readersOwned = reader->hostCallReadersOwned_;
                const auto before = reader->diagnosticSequence_.load(std::memory_order_seq_cst);
                output.nativeThread = reader->diagnosticNativeThread_.load(std::memory_order_seq_cst);
                output.operation = reader->diagnosticOperation_.load(std::memory_order_seq_cst);
                const auto* name = reader->diagnosticName_.load(std::memory_order_seq_cst);
                output.identityStable = (before & 1) == 0 && before ==
                    reader->diagnosticSequence_.load(std::memory_order_seq_cst);
                if (output.identityStable && name != nullptr) {
                    std::strncpy(output.name.data(), name, output.name.size() - 1);
                }
            }
        };
        if (thread.shortHostCall_) {
            recordDenial(MutationDenialReason::ShortHostCallActive);
            return 0;
        }
        if (thread.nativeDataToken_ != 0) {
            recordDenial(MutationDenialReason::NativeDataLeaseActive);
            return 0;
        }
        if (thread.hostWaitToken_ != 0 || thread.guestDataLeasesOwned_ != 0) {
            recordDenial(thread.hostWaitToken_ != 0
                ? MutationDenialReason::HostWaitActive
                : MutationDenialReason::GuestDataLeaseActive);
            return 0;
        }
        if (thread.mutationDepth_ != 0) {
            if (exclusivePublicationBorrowCount_ != 0) {
                recordDenial(MutationDenialReason::ExclusivePublicationBorrowActive);
                return 0;
            }
            if (thread.mutationBarrier_ != this || thread.mutationPublishing_ ||
                (thread.mutationScope_ ==
                     GuestMemoryMutationScope::AdditiveMapping &&
                 scope == GuestMemoryMutationScope::Exclusive) ||
                thread.mutationDepth_ ==
                    (std::numeric_limits<std::uint32_t>::max)()) {
                recordDenial(MutationDenialReason::NestedMutationDenied);
                return 0;
            }
            ++thread.mutationDepth_;
            return CurrentMutationToken();
        }
        if (thread.executionDepth_ != 0 && thread.executionBarrier_ != this) {
            recordDenial(MutationDenialReason::ForeignExecutionBarrier);
            return 0;
        }
        if (thread.executionDepth_ == 0 && thread.hostCallSuspendDepth_ != 0 &&
            thread.hostCallBarrier_ != this) {
            recordDenial(MutationDenialReason::ForeignHostCallBarrier);
            return 0;
        }
        if (thread.hostCallReadersOwned_ != thread.hostCallSuspendDepth_) {
            recordDenial(MutationDenialReason::HostReaderOwnershipMismatch);
            return 0;
        }
        // Only the currently executing HLE frame may consent to upgrading its
        // own guest-pointer lease. An outer callback retained across nested
        // guest/HLE execution cannot, so destructive nesting fails closed.
        if (scope == GuestMemoryMutationScope::Exclusive &&
            (thread.retainedHostCallExecutionDepth_ != 0 ||
             (thread.executionDepth_ != 0 && thread.hostCallSuspendDepth_ != 0) ||
             thread.hostCallSuspendDepth_ > 1)) {
            recordDenial(MutationDenialReason::NestedExclusiveMutation);
            return 0;
        }

        const auto deadline = Clock::now() + timeout;

        // A competing JIT mutator parks behind the active epoch. HLE readers
        // never drop guest-pointer leases merely to enter this queue.
        // A notified HLE return, fresh entry or host-wait return has not yet
        // acquired its reader. Drain those admissions as well as parked JIT
        // readers before another writer can close the gate. Otherwise a busy
        // native producer can repeatedly overtake the same awakened CPU.
        // A native mutation requested by this execution is itself an explicit
        // departure from guest execution; waiting for its own JIT witness would
        // deadlock. Peers still owe their independent progress opportunity.
        ClearGuestProgressLocked(thread);
        while (fullIntent_ || requestEpoch_ != 0 || writer_ || parked_ != 0 || admissionWaiters_ != 0 ||
               guestProgressPending_.load(std::memory_order_relaxed) != 0) {
            if (thread.hostCallReadersOwned_ != 0 &&
                (fullIntent_ ||
                 (requestEpoch_ != 0 &&
                  requestScope_ == GuestMemoryMutationScope::Exclusive))) {
                recordDenial(MutationDenialReason::CompetingExclusiveMutation);
                return 0;
            }
            // A completed epoch resets requestScope_ to Exclusive before its
            // parked peers finish readmission. That tail is only a queue to
            // drain, not an active exclusive request against this HLE lease.
            if (thread.executionCounted_ && requestEpoch_ != 0) {
                ParkForPendingMutationLocked(lock, thread);
                ClearGuestProgressLocked(thread);
                if (Clock::now() >= deadline) {
                    recordDenial(MutationDenialReason::PendingMutationTimeout);
                    return 0;
                }
            } else {
                ++waitingEntrants_;
                NotifyMutationProgressLocked();
                // Re-evaluate every phase transition. In particular, a JIT
                // reader waiting on Full intent must wake when intent becomes
                // an exclusive request so the next loop pass can park it.
                const bool phaseChanged = [&] {
                    ConditionWaiter waiter(queuedMutationWaiters_);
                    return condition_.wait_until(lock, deadline) != std::cv_status::timeout;
                }();
                --waitingEntrants_;
                NotifyMutationProgressLocked();
                if (!phaseChanged &&
                    (fullIntent_ || requestEpoch_ != 0 || writer_ ||
                     parked_ != 0 || admissionWaiters_ != 0 ||
                     guestProgressPending_.load(std::memory_order_relaxed) != 0)) {
                    recordDenial(MutationDenialReason::PendingMutationTimeout);
                    return 0;
                }
            }
        }

        if (scope == GuestMemoryMutationScope::Exclusive) {
            fullIntent_ = true;
            NotifyAdmissionPhaseChangedLocked();
            // Native data pins keep every existing mapping stable without
            // parking the CPU. Stop new pins through fullIntent_, then wait
            // here so a VM request cannot arm safepoints until the GPU slice
            // releases its pin. Never upgrade a native pin on its owner thread.
            const bool lifetimeReadersDrained = WaitForOwnerProgressLocked(
                lock, deadline, [this] {
                    return retainedHostCallExecutions_ == 0 && nativeDataReaders_ == 0;
                });
            if (!lifetimeReadersDrained) {
                recordDenial(nativeDataReaders_ != 0
                    ? MutationDenialReason::NativeDataLeaseTimeout
                    : MutationDenialReason::RetainedExecutionTimeout);
                fullIntent_ = false;
                NotifyAdmissionPhaseChangedLocked();
                return 0;
            }
        }

        if (++nextRequestEpoch_ == 0) {
            ++nextRequestEpoch_;
        }
        const std::uint64_t epoch = nextRequestEpoch_;
        requestEpoch_ = epoch;
        requestScope_ = scope;
        thread.requestOwnerBarrier_ = this;
        thread.requestOwnerEpoch_ = epoch;
        fullIntent_ = false;
        NotifyAdmissionPhaseChangedLocked();

        lock.unlock();
        const bool armed = armSafepoints(epoch);
        lock.lock();
        if (!armed || requestEpoch_ != epoch) {
            recordDenial(!armed ? MutationDenialReason::ArmSafepointsFailed
                               : MutationDenialReason::RequestEpochChanged);
            lock.unlock();
            disarmSafepoints(epoch);
            lock.lock();
            CancelRequestLocked(epoch);
            return 0;
        }

        const std::size_t ownerReaders = thread.executionCounted_ ? 1 : 0;
        const std::size_t ownerHostCallReaders =
            scope == GuestMemoryMutationScope::Exclusive &&
                thread.executionDepth_ == 0 && thread.hostCallSuspendDepth_ == 1
            ? 1
            : 0;
        const bool quiesced = WaitForOwnerProgressLocked(
            lock, deadline,
            [this, ownerReaders, ownerHostCallReaders, epoch, scope] {
                return requestEpoch_ != epoch ||
                    (readers_ == ownerReaders &&
                     (scope == GuestMemoryMutationScope::AdditiveMapping ||
                      hostCallReaders_ == ownerHostCallReaders));
            });
        if (!quiesced || requestEpoch_ != epoch) {
            recordDenial(requestEpoch_ != epoch
                ? MutationDenialReason::RequestEpochChanged
                : MutationDenialReason::QuiescenceTimeout);
            lock.unlock();
            disarmSafepoints(epoch);
            lock.lock();
            CancelRequestLocked(epoch);
            return 0;
        }

        thread.mutationUpgradedExecution_ = false;
        thread.mutationUpgradedHostCall_ = false;
        if (thread.executionCounted_) {
            if (readers_ != 1) {
                recordDenial(MutationDenialReason::ExecutionUpgradeMismatch);
                lock.unlock();
                disarmSafepoints(epoch);
                lock.lock();
                CancelRequestLocked(epoch);
                return 0;
            }
            readers_ = 0;
            SetExecutionCounted(thread, false);
            thread.mutationUpgradedExecution_ = true;
        } else if (readers_ != 0) {
            recordDenial(MutationDenialReason::ExecutionUpgradeMismatch);
            lock.unlock();
            disarmSafepoints(epoch);
            lock.lock();
            CancelRequestLocked(epoch);
            return 0;
        }
        if (ownerHostCallReaders != 0) {
            if (hostCallReaders_ != 1 || thread.hostCallReadersOwned_ != 1) {
                recordDenial(MutationDenialReason::HostCallUpgradeMismatch);
                lock.unlock();
                disarmSafepoints(epoch);
                lock.lock();
                CancelRequestLocked(epoch);
                return 0;
            }
            RemoveHostReaderLocked(thread);
            thread.mutationUpgradedHostCall_ = true;
        } else if (scope == GuestMemoryMutationScope::Exclusive &&
                   hostCallReaders_ != 0) {
            recordDenial(MutationDenialReason::HostCallUpgradeMismatch);
            lock.unlock();
            disarmSafepoints(epoch);
            lock.lock();
            CancelRequestLocked(epoch);
            return 0;
        }

        writer_ = true;
        thread.mutationDepth_ = 1;
        thread.mutationBarrier_ = this;
        thread.mutationEpoch_ = epoch;
        thread.mutationScope_ = scope;
        if (++nextMutationNonce_ == 0) {
            ++nextMutationNonce_;
        }
        thread.mutationNonce_ = nextMutationNonce_;
        NotifyAdmissionPhaseChangedLocked();
        return CurrentMutationToken();
    }

    template<typename ArmSafepoints, typename DisarmSafepoints,
             typename Rep, typename Period>
    std::uint64_t BeginMutation(
        ArmSafepoints&& armSafepoints,
        DisarmSafepoints&& disarmSafepoints,
        std::chrono::duration<Rep, Period> timeout) noexcept {
        return BeginMutation(
            GuestMemoryMutationScope::Exclusive,
            static_cast<ArmSafepoints&&>(armSafepoints),
            static_cast<DisarmSafepoints&&>(disarmSafepoints), timeout);
    }

    // Convenience overload retained for uncontended setup/test mutations.
    std::uint64_t BeginMutation() noexcept {
        return BeginMutation(
            [](std::uint64_t) noexcept { return true; },
            [](std::uint64_t) noexcept {},
            std::chrono::nanoseconds::zero());
    }

    bool OwnsMutation(std::uint64_t token) noexcept {
        std::lock_guard lock(mutex_);
        return OwnsMutationLocked(token);
    }

    bool OwnsAdditiveMutation(std::uint64_t token) noexcept {
        std::lock_guard lock(mutex_);
        // Nested additive requests inherit an exclusive outer mutation. Only
        // the token's actual scope may authorize preserving translator state.
        return OwnsMutationLocked(token) &&
            threadState_.mutationScope_ == GuestMemoryMutationScope::AdditiveMapping;
    }

    // Ownership is thread-affine. These fields are written only by the calling
    // thread during admission/commit; another writer cannot revoke its token.
    // This reads no process-wide writer/request state and grants no new lease.
    bool OwnsExclusiveMutation(std::uint64_t token) const noexcept {
        return OwnsMutationLocked(token) &&
            threadState_.mutationScope_ == GuestMemoryMutationScope::Exclusive;
    }

    // A native producer may lend its exclusive epoch to a completion callback.
    // This grants no rights and permits no CPU concurrency. The callback must
    // finish native work and destroy imported objects before releasing the
    // single-use token; neither nested mutation nor publication/readmission can
    // occur meanwhile. Release is deliberately transferable across host threads.
    std::uint64_t RetainExclusivePublicationBorrow() noexcept {
        std::lock_guard lock(mutex_);
        const auto& thread = threadState_;
        if (!writer_ || thread.mutationBarrier_ != this || thread.mutationDepth_ != 1 ||
            thread.mutationScope_ != GuestMemoryMutationScope::Exclusive || thread.mutationPublishing_ ||
            thread.mutationEpoch_ == 0 || requestEpoch_ != thread.mutationEpoch_ ||
            thread.executionDepth_ != 0 || thread.hostCallSuspendDepth_ != 0 ||
            thread.mutationUpgradedExecution_ || thread.mutationUpgradedHostCall_ ||
            exclusivePublicationBorrowCount_ == exclusivePublicationBorrows_.size()) return 0;
        auto token = nextExclusivePublicationBorrow_.load(std::memory_order_relaxed);
        const auto exhausted = (std::numeric_limits<std::uint64_t>::max)();
        while (token != exhausted && !nextExclusivePublicationBorrow_.compare_exchange_weak(
            token, token + 1, std::memory_order_relaxed)) {}
        if (token == exhausted) return 0; // Saturate; no stale token can become current again.
        for (auto& slot : exclusivePublicationBorrows_) {
            if (slot == 0) {
                slot = token;
                ++exclusivePublicationBorrowCount_;
                exclusivePublicationBorrowEpoch_ = thread.mutationEpoch_;
                return token;
            }
        }
        return 0;
    }

    bool ReleaseExclusivePublicationBorrow(std::uint64_t token) noexcept {
        std::lock_guard lock(mutex_);
        if (token == 0 || !writer_ || exclusivePublicationBorrowEpoch_ == 0 ||
            exclusivePublicationBorrowEpoch_ != requestEpoch_) return false;
        for (auto& slot : exclusivePublicationBorrows_) {
            if (slot == token) {
                slot = 0;
                if (--exclusivePublicationBorrowCount_ == 0) exclusivePublicationBorrowEpoch_ = 0;
                return true;
            }
        }
        return false;
    }

    template<typename DisarmSafepoints>
    bool EndMutation(std::uint64_t token,
                     DisarmSafepoints&& disarmSafepoints) noexcept {
        return PublishAndEndMutation(
            token, []() noexcept {},
            static_cast<DisarmSafepoints&&>(disarmSafepoints));
    }

    bool EndMutation(std::uint64_t token) noexcept {
        return EndMutation(token, [](std::uint64_t) noexcept {});
    }

    template<typename Publisher, typename DisarmSafepoints>
    bool PublishAndEndMutation(
        std::uint64_t token, Publisher&& publisher,
        DisarmSafepoints&& disarmSafepoints) noexcept {
        auto& thread = threadState_;
        std::uint64_t epoch = 0;
        bool finalCommit = false;
        {
            std::lock_guard lock(mutex_);
            if (!OwnsMutationLocked(token) || thread.mutationPublishing_ ||
                exclusivePublicationBorrowCount_ != 0) {
                return false;
            }
            thread.mutationPublishing_ = true;
            finalCommit = thread.mutationDepth_ == 1;
            epoch = thread.mutationEpoch_;
        }

        publisher();
        if (finalCommit) {
            // Page protections are part of the safepoint transaction. Remove
            // them while admission is still closed and before notifying peers.
            disarmSafepoints(epoch);
        }

        std::lock_guard lock(mutex_);
        thread.mutationPublishing_ = false;
        if (!OwnsMutationLocked(token)) {
            return false;
        }
        return FinishMutationLocked();
    }

    template<typename Publisher>
    bool PublishAndEndMutation(
        std::uint64_t token, Publisher&& publisher) noexcept {
        return PublishAndEndMutation(
            token, static_cast<Publisher&&>(publisher),
            [](std::uint64_t) noexcept {});
    }

#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
    struct TestingSnapshot {
        std::size_t readers;
        std::size_t parked;
        std::size_t waitingEntrants;
        std::uint64_t requestEpoch;
        bool writer;
        std::size_t hostCallReaders {};
        std::size_t retainedHostCallExecutions {};
        GuestMemoryMutationScope requestScope {
            GuestMemoryMutationScope::Exclusive};
        bool fullIntent {};
        std::size_t nativeDataReaders {};
    };

    TestingSnapshot SnapshotForTesting() noexcept {
        std::lock_guard lock(mutex_);
        return {readers_, parked_, waitingEntrants_, requestEpoch_, writer_,
                hostCallReaders_, retainedHostCallExecutions_,
                requestScope_, fullIntent_, nativeDataReaders_};
    }

    struct TestingNotifications {
        std::uint64_t mutation;
        std::uint64_t admission;
        std::uint64_t ownerProgress;
        std::uint64_t queuedProgress;
        std::uint64_t actualAdmission;
    };

    TestingNotifications NotificationsForTesting() noexcept {
        std::lock_guard lock(mutex_);
        return {mutationNotifications_, admissionNotifications_,
                ownerProgressNotifications_, queuedProgressNotifications_,
                actualAdmissionNotifications_};
    }

    bool SplitNotificationsForTesting() const noexcept {
        return splitNotifications_;
    }

    bool TargetedProgressForTesting() const noexcept {
        return targetedProgress_;
    }

    struct TestingWaiters {
        std::size_t ownerProgress;
        std::size_t queuedMutation;
        std::size_t admission;
    };

    TestingWaiters WaitersForTesting() noexcept {
        std::lock_guard lock(mutex_);
        return {ownerProgressWaiters_, queuedMutationWaiters_, admissionWaiters_};
    }

    // Hold a real parked peer at the post-epoch scheduling boundary so tests
    // can observe the completed epoch's tail without scheduler timing guesses.
    void SetParkedReadmissionHookForTesting(
        void (*hook)(void*), void* opaque) noexcept {
        std::lock_guard lock(mutex_);
        parkedReadmissionHook_ = hook;
        parkedReadmissionOpaque_ = opaque;
    }

    void SetAdmissionReadmissionHookForTesting(
        void (*hook)(void*), void* opaque) noexcept {
        std::lock_guard lock(mutex_);
        admissionReadmissionHook_ = hook;
        admissionReadmissionOpaque_ = opaque;
    }

    bool WaitForTestingState(
        TestingSnapshot expected,
        std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(mutex_);
        return testingCondition_.wait_for(lock, timeout, [this, expected] {
            return readers_ == expected.readers &&
                parked_ == expected.parked &&
                waitingEntrants_ == expected.waitingEntrants &&
                requestEpoch_ == expected.requestEpoch &&
                writer_ == expected.writer &&
                hostCallReaders_ == expected.hostCallReaders &&
                retainedHostCallExecutions_ ==
                    expected.retainedHostCallExecutions &&
                requestScope_ == expected.requestScope &&
                fullIntent_ == expected.fullIntent &&
                nativeDataReaders_ == expected.nativeDataReaders;
        });
    }
#endif

private:
    static void AdvanceExecutionReadAdmissionEpoch(ThreadState& thread) noexcept {
        if (thread.executionReadAdmissionEpoch_ != (std::numeric_limits<std::uint64_t>::max)())
            ++thread.executionReadAdmissionEpoch_;
    }

    void ClearGuestProgressLocked(ThreadState& thread) noexcept {
        if (!thread.guestProgressOwed_) return;
        thread.guestProgressOwed_ = false;
        guestProgressPending_.fetch_sub(1, std::memory_order_release);
    }

    void SetExecutionCounted(ThreadState& thread, bool counted) noexcept {
        if (thread.executionCounted_ != counted) AdvanceExecutionReadAdmissionEpoch(thread);
        thread.executionCounted_ = counted;
        if (!counted) ClearGuestProgressLocked(thread);
        else if (thread.progressOnReadmission_) {
            thread.progressOnReadmission_ = false;
            if (guestProgressWitness_ && !thread.guestProgressOwed_) {
                thread.guestProgressOwed_ = true;
                guestProgressPending_.fetch_add(1, std::memory_order_release);
            }
        }
    }
    // Registration and every notification use mutex_. A waiter registers
    // before condition_variable atomically releases that mutex, so omitting a
    // notification to an empty queue cannot miss a concurrently arriving wait.
    class ConditionWaiter final {
    public:
        explicit ConditionWaiter(std::size_t& count) noexcept : count_(count) {
            ++count_;
        }
        ~ConditionWaiter() { --count_; }
        ConditionWaiter(const ConditionWaiter&) = delete;
        ConditionWaiter& operator=(const ConditionWaiter&) = delete;
    private:
        std::size_t& count_;
    };

    static bool SplitNotificationsConfigured() noexcept {
        // The environment is consulted once per process. Split notifications
        // are the baseline; explicit 0 or malformed values use the ordinary barrier.
        static const bool enabled = [] {
            const auto* value = std::getenv("LUMEN_FEX_BARRIER_SPLIT_NOTIFICATIONS");
            const bool enabled = value == nullptr || std::strcmp(value, "1") == 0;
            std::fprintf(stderr, "FEX_BARRIER_SPLIT_NOTIFICATIONS_READY enabled=%u\n",
                         enabled ? 1u : 0u);
            return enabled;
        }();
        return enabled;
    }

    static bool TargetedProgressConfigured() noexcept {
        static const bool enabled = [] {
            const auto* value = std::getenv("LUMEN_FEX_BARRIER_TARGETED_PROGRESS");
            const bool enabled = value != nullptr && std::strcmp(value, "1") == 0;
            std::fprintf(stderr, "FEX_BARRIER_TARGETED_PROGRESS_READY enabled=%u\n",
                         enabled ? 1u : 0u);
            return enabled;
        }();
        return enabled;
    }

    std::condition_variable& AdmissionConditionLocked() noexcept {
        return splitNotifications_ ? admissionCondition_ : condition_;
    }

    template<typename Predicate>
    void WaitForAdmissionConditionLocked(
        std::unique_lock<std::mutex>& lock, Predicate&& predicate) noexcept {
        ConditionWaiter waiter(admissionWaiters_);
        AdmissionConditionLocked().wait(lock, static_cast<Predicate&&>(predicate));
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
        // Model an awakened waiter that has not yet been scheduled for its
        // reader admission. Keep its queue registration live across the hook.
        if (admissionReadmissionHook_ != nullptr) {
            const auto hook = admissionReadmissionHook_;
            void* const opaque = admissionReadmissionOpaque_;
            lock.unlock();
            hook(opaque);
            lock.lock();
            AdmissionConditionLocked().wait(lock, static_cast<Predicate&&>(predicate));
        }
#endif
    }

    template<typename Deadline, typename Predicate>
    bool WaitForOwnerProgressLocked(
        std::unique_lock<std::mutex>& lock, Deadline deadline,
        Predicate&& predicate) noexcept {
        // Only the reserved full-intent/request owner can wait here. Queued
        // mutations cannot consume its notify_one or compete with its drain.
        ConditionWaiter waiter(ownerProgressWaiters_);
        auto& condition = targetedProgress_ ? ownerProgressCondition_ : condition_;
        return condition.wait_until(lock, deadline, static_cast<Predicate&&>(predicate));
    }

    // Reader count changes can advance a writer's quiescence/lifetime wait,
    // but cannot readmit parked CPU peers while their epoch remains pending.
    // Both queues retain the same mutex and predicates. Intent/epoch changes
    // also wake admission, including retained entries and fresh callbacks.
    void NotifyMutationProgressLocked(bool admissionPhaseChanged = false) noexcept {
        if (targetedProgress_) {
            if (ownerProgressWaiters_ != 0) {
                ownerProgressCondition_.notify_one();
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
                ++ownerProgressNotifications_;
#endif
            }
            // A queued writer needs phase changes, or the final CPU admission
            // returning after a completed epoch. Intermediate reader counts
            // cannot advance its queue predicate; those only concern the owner.
            const bool completedTail = !fullIntent_ && requestEpoch_ == 0 &&
                !writer_ && parked_ == 0 && admissionWaiters_ == 0;
            if ((queuedMutationWaiters_ != 0 &&
                 (admissionPhaseChanged || completedTail)) ||
                (!splitNotifications_ && admissionWaiters_ != 0 &&
                 admissionPhaseChanged)) {
                condition_.notify_all();
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
                ++queuedProgressNotifications_;
                if (!splitNotifications_) ++actualAdmissionNotifications_;
#endif
            }
        } else {
            condition_.notify_all();
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
            ++queuedProgressNotifications_;
            if (!splitNotifications_) ++actualAdmissionNotifications_;
#endif
        }
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
        ++mutationNotifications_;
        if (!splitNotifications_) ++admissionNotifications_;
        testingCondition_.notify_all();
#endif
    }

    void NotifyAdmissionPhaseChangedLocked() noexcept {
        // Even epochs admit short calls; odd epochs force ordinary admission.
        // Written under mutex_, before notifications/arming, and reopened only
        // after cancellation or final publication has disarmed safepoints.
        const bool closed = fullIntent_ || requestEpoch_ != 0 || writer_;
        const auto epoch = shortAdmissionEpoch_.load(std::memory_order_relaxed);
        if (((epoch & 1U) != 0) != closed)
            shortAdmissionEpoch_.store(epoch + 1, std::memory_order_release);
        NotifyMutationProgressLocked(true);
        if (splitNotifications_) {
            if (!targetedProgress_ || admissionWaiters_ != 0) {
                admissionCondition_.notify_all();
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
                ++actualAdmissionNotifications_;
#endif
            }
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
            ++admissionNotifications_;
#endif
        }
    }

    void AddHostReaderLocked(ThreadState& thread) noexcept {
        if (thread.hostCallReadersOwned_++ == 0) {
            thread.previousHostReader_ = nullptr;
            thread.nextHostReader_ = hostReaderHead_;
            if (hostReaderHead_ != nullptr) hostReaderHead_->previousHostReader_ = &thread;
            hostReaderHead_ = &thread;
        }
        ++hostCallReaders_;
    }

    void RemoveHostReaderLocked(ThreadState& thread) noexcept {
        --hostCallReaders_;
        if (--thread.hostCallReadersOwned_ == 0) {
            if (thread.previousHostReader_ != nullptr)
                thread.previousHostReader_->nextHostReader_ = thread.nextHostReader_;
            else hostReaderHead_ = thread.nextHostReader_;
            if (thread.nextHostReader_ != nullptr)
                thread.nextHostReader_->previousHostReader_ = thread.previousHostReader_;
            thread.previousHostReader_ = thread.nextHostReader_ = nullptr;
        }
    }

    void RecordFailureLocked(BarrierFailureDiagnostic* diagnostic,
                             BarrierFailurePhase phase) const noexcept {
        if (diagnostic == nullptr) return;
        const auto& thread = threadState_;
        *diagnostic = {
            phase, thread.executionDepth_, thread.hostCallSuspendDepth_,
            thread.hostCallReadersOwned_, thread.retainedHostCallExecutionDepth_,
            thread.guestDataLeasesOwned_, thread.mutationDepth_,
            thread.mutationScope_, thread.mutationPublishing_,
            thread.executionBarrier_ == this, thread.hostCallBarrier_ == this,
            thread.mutationBarrier_ == this, thread.requestOwnerBarrier_ == this,
            thread.hostWaitBarrier_ == this, thread.hostWaitToken_ != 0,
            thread.executionCounted_, thread.mutationEpoch_, thread.requestOwnerEpoch_,
            readers_, hostCallReaders_, retainedHostCallExecutions_, parked_, waitingEntrants_,
            requestEpoch_, requestScope_, writer_, fullIntent_,
        };
    }

    bool SuspendExecutionForHostCallLocked(
        std::unique_lock<std::mutex>& lock, ThreadState& thread,
        BarrierFailureDiagnostic* diagnostic) noexcept {
        if (thread.shortHostCall_ || thread.mutationDepth_ != 0 || thread.guestDataLeasesOwned_ != 0 ||
            thread.executionDepth_ != 1 ||
            thread.executionBarrier_ != this || !thread.executionCounted_ ||
            (thread.hostCallSuspendDepth_ != 0 && thread.hostCallBarrier_ != this) ||
            thread.hostCallSuspendDepth_ ==
                    (std::numeric_limits<std::uint32_t>::max)()) {
            RecordFailureLocked(diagnostic, BarrierFailurePhase::HostCallSuspend);
            return false;
        }

        thread.executionDepth_ = 0;
        thread.executionBarrier_ = nullptr;
        SetExecutionCounted(thread, false);
        if (readers_ != 0) {
            --readers_;
        }

        // Close the JIT side first. For an exclusive request, a top-level
        // callback must not acquire a new guest-pointer lease until publish;
        // a retained outer lease remains counted and makes that request fail
        // closed on its bounded deadline.
        // Admission keeps this mutex until return or notifies before its
        // wait releases it. The final notify below covers immediate admission.
        WaitForHostCallAdmissionLocked(lock, thread);
        if (thread.hostCallSuspendDepth_ == 0) {
            thread.hostCallBarrier_ = this;
        }
        ++thread.hostCallSuspendDepth_;
        AddHostReaderLocked(thread);
        NotifyMutationProgressLocked();
        return true;
    }

    bool ResumeExecutionAfterHostCallLocked(
        std::unique_lock<std::mutex>& lock, ThreadState& thread,
        BarrierFailureDiagnostic* diagnostic) noexcept {
        if (thread.mutationDepth_ != 0 || thread.guestDataLeasesOwned_ != 0 ||
            thread.executionDepth_ != 0 ||
            thread.executionBarrier_ != nullptr || thread.executionCounted_ ||
            thread.hostCallReadersOwned_ == 0 ||
            thread.hostCallSuspendDepth_ == 0 || thread.hostCallBarrier_ != this) {
            RecordFailureLocked(diagnostic, BarrierFailurePhase::HostCallResume);
            return false;
        }

        // The callback has finished using guest pointers. Stop counting it
        // before waiting, otherwise an exclusive writer could wait on this
        // reader while this reader waits on the writer.
        RemoveHostReaderLocked(thread);
        --thread.hostCallSuspendDepth_;
        if (thread.hostCallSuspendDepth_ == 0) {
            thread.hostCallBarrier_ = nullptr;
        }

        // Admission notifies before its wait can release this mutex; without
        // a wait, the completed reader transition below supplies the notify.
        WaitForAdmissionLocked(lock);
        thread.executionBarrier_ = this;
        thread.executionDepth_ = 1;
        SetExecutionCounted(thread, true);
        ++readers_;
        NotifyMutationProgressLocked();
        return true;
    }

    bool CheckpointSafepointLocked(
        std::unique_lock<std::mutex>& lock, ThreadState& thread,
        BarrierFailureDiagnostic* diagnostic) noexcept {
        if (thread.shortHostCall_ || thread.executionDepth_ == 0 || thread.executionBarrier_ != this ||
            !thread.executionCounted_) {
            RecordFailureLocked(diagnostic, BarrierFailurePhase::Checkpoint);
            return false;
        }
        if (requestEpoch_ == 0 ||
            (thread.requestOwnerBarrier_ == this &&
             thread.requestOwnerEpoch_ == requestEpoch_)) {
            return true;
        }

        ParkForPendingMutationLocked(lock, thread);
        return true;
    }

    bool WaitForNestedExecutionAdmissionLocked(
        std::unique_lock<std::mutex>& lock) noexcept {
        if (!fullIntent_ && requestEpoch_ == 0 && !writer_) {
            return true;
        }
        if (fullIntent_ ||
            requestScope_ == GuestMemoryMutationScope::Exclusive) {
            return false;
        }
        ++waitingEntrants_;
        NotifyMutationProgressLocked();
        WaitForAdmissionConditionLocked(lock, [this] {
            return fullIntent_ ||
                (requestEpoch_ == 0 && !writer_) ||
                requestScope_ == GuestMemoryMutationScope::Exclusive;
        });
        --waitingEntrants_;
        NotifyMutationProgressLocked();
        const bool admitted = !fullIntent_ && requestEpoch_ == 0 && !writer_;
        if (admitted) threadState_.progressOnReadmission_ = true;
        return admitted;
    }

    void WaitForAdmissionLocked(std::unique_lock<std::mutex>& lock) noexcept {
        if (requestEpoch_ == 0 && !writer_) {
            return;
        }
        ++waitingEntrants_;
        NotifyMutationProgressLocked();
        WaitForAdmissionConditionLocked(lock, [this] {
            return requestEpoch_ == 0 && !writer_;
        });
        --waitingEntrants_;
        threadState_.progressOnReadmission_ = true;
        NotifyMutationProgressLocked();
    }

    void WaitForHostCallAdmissionLocked(
        std::unique_lock<std::mutex>& lock, ThreadState& thread) noexcept {
        const bool fullIntentBlocks = fullIntent_ &&
            thread.retainedHostCallExecutionDepth_ == 0;
        if (!fullIntentBlocks &&
            ((requestEpoch_ == 0 && !writer_) ||
             requestScope_ == GuestMemoryMutationScope::AdditiveMapping)) {
            return;
        }
        ++waitingEntrants_;
        NotifyMutationProgressLocked();
        WaitForAdmissionConditionLocked(lock, [this, &thread] {
            const bool intentAllowsHostCall = !fullIntent_ ||
                thread.retainedHostCallExecutionDepth_ != 0;
            return intentAllowsHostCall &&
                ((requestEpoch_ == 0 && !writer_) ||
                 requestScope_ ==
                     GuestMemoryMutationScope::AdditiveMapping);
        });
        --waitingEntrants_;
        NotifyMutationProgressLocked();
    }

    void ParkForPendingMutationLocked(
        std::unique_lock<std::mutex>& lock, ThreadState& thread) noexcept {
        bool parkedExecution = false;
        if (thread.executionCounted_) {
            SetExecutionCounted(thread, false);
            if (readers_ != 0) {
                --readers_;
            }
            parkedExecution = true;
            ++parked_;
            NotifyMutationProgressLocked();
        }

        ++waitingEntrants_;
        NotifyMutationProgressLocked();
        WaitForAdmissionConditionLocked(lock, [this] {
            return requestEpoch_ == 0 && !writer_;
        });
        --waitingEntrants_;
        NotifyMutationProgressLocked();

#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
        if (parkedExecution && parkedReadmissionHook_ != nullptr) {
            const auto hook = parkedReadmissionHook_;
            void* const opaque = parkedReadmissionOpaque_;
            lock.unlock();
            hook(opaque);
            lock.lock();
        }
#endif
        if (parkedExecution) {
            if (parked_ != 0) {
                --parked_;
            }
            ++readers_;
            thread.progressOnReadmission_ = true;
            SetExecutionCounted(thread, true);
            NotifyMutationProgressLocked();
        }
    }

    void CancelRequestLocked(std::uint64_t epoch) noexcept {
        auto& thread = threadState_;
        if (requestEpoch_ != epoch) {
            return;
        }
        requestEpoch_ = 0;
        requestScope_ = GuestMemoryMutationScope::Exclusive;
        if (thread.requestOwnerBarrier_ == this && thread.requestOwnerEpoch_ == epoch) {
            thread.requestOwnerBarrier_ = nullptr;
            thread.requestOwnerEpoch_ = 0;
        }
        NotifyAdmissionPhaseChangedLocked();
    }

    bool FinishMutationLocked() noexcept {
        auto& thread = threadState_;
        if (--thread.mutationDepth_ != 0) {
            return true;
        }
        if (thread.mutationUpgradedExecution_) {
            ++readers_;
            SetExecutionCounted(thread, true);
        }
        if (thread.mutationUpgradedHostCall_) {
            AddHostReaderLocked(thread);
        }
        thread.mutationUpgradedExecution_ = false;
        thread.mutationUpgradedHostCall_ = false;
        thread.mutationNonce_ = 0;
        thread.mutationScope_ = GuestMemoryMutationScope::Exclusive;
        thread.mutationBarrier_ = nullptr;
        writer_ = false;
        const std::uint64_t epoch = thread.mutationEpoch_;
        thread.mutationEpoch_ = 0;
        if (requestEpoch_ == epoch) {
            requestEpoch_ = 0;
            requestScope_ = GuestMemoryMutationScope::Exclusive;
        }
        if (thread.requestOwnerBarrier_ == this && thread.requestOwnerEpoch_ == epoch) {
            thread.requestOwnerBarrier_ = nullptr;
            thread.requestOwnerEpoch_ = 0;
        }
        NotifyAdmissionPhaseChangedLocked();
        return true;
    }

    static std::uint64_t CurrentMutationToken() noexcept {
        auto& thread = threadState_;
        return (static_cast<std::uint64_t>(thread.mutationNonce_) << 32U) |
            thread.mutationDepth_;
    }

    bool OwnsMutationLocked(std::uint64_t token) const noexcept {
        auto& thread = threadState_;
        return token != 0 && thread.mutationDepth_ != 0 &&
            thread.mutationBarrier_ == this && token == CurrentMutationToken();
    }

    std::mutex mutex_;
    // One exclusive writer exists process-wide for this barrier. Fixed capacity
    // gives a conservative fallback instead of allocation during GPU admission.
    std::array<std::uint64_t, 64> exclusivePublicationBorrows_ {};
    std::size_t exclusivePublicationBorrowCount_ {};
    std::uint64_t exclusivePublicationBorrowEpoch_ {};
    inline static std::atomic<std::uint64_t> nextExclusivePublicationBorrow_ {1};
    bool guestProgressWitness_ {};
    std::atomic<std::uint32_t> guestProgressPending_ {};
    static_assert(decltype(guestProgressPending_)::is_always_lock_free);
    std::atomic<std::uint64_t> shortAdmissionEpoch_ {};
    static_assert(decltype(shortAdmissionEpoch_)::is_always_lock_free);
    std::condition_variable condition_;
    std::condition_variable admissionCondition_;
    std::condition_variable ownerProgressCondition_;
    const bool splitNotifications_;
    const bool targetedProgress_;
    std::size_t ownerProgressWaiters_ {};
    std::size_t queuedMutationWaiters_ {};
    std::size_t admissionWaiters_ {};
    std::size_t readers_ {};
    std::size_t parked_ {};
    std::size_t waitingEntrants_ {};
    std::size_t hostCallReaders_ {};
    ThreadState* hostReaderHead_ {};
    std::size_t nativeDataReaders_ {};
    std::size_t retainedHostCallExecutions_ {};
    bool writer_ {};
    bool fullIntent_ {};
    GuestMemoryMutationScope requestScope_ {
        GuestMemoryMutationScope::Exclusive};
    std::uint64_t nextRequestEpoch_ {};
    std::uint64_t requestEpoch_ {};
    std::uint32_t nextMutationNonce_ {};
    std::uint64_t nextHostWaitToken_ {};
    std::uint64_t nextFreshCallbackToken_ {};
    std::uint64_t nextNativeDataToken_ {};
#if defined(LUMEN_ANDROID_FEXCORE_BARRIER_TESTING)
    std::condition_variable testingCondition_;
    std::uint64_t mutationNotifications_ {};
    std::uint64_t admissionNotifications_ {};
    std::uint64_t ownerProgressNotifications_ {};
    std::uint64_t queuedProgressNotifications_ {};
    std::uint64_t actualAdmissionNotifications_ {};
    void (*parkedReadmissionHook_)(void*) {};
    void* parkedReadmissionOpaque_ {};
    void (*admissionReadmissionHook_)(void*) {};
    void* admissionReadmissionOpaque_ {};
#endif

    static inline thread_local ThreadState threadState_ {};
};

class ExecutionBarrierLease final {
public:
    explicit ExecutionBarrierLease(GuestMemoryMutationBarrier& barrier,
        GuestMemoryMutationBarrier::BarrierFailureDiagnostic* diagnostic = nullptr) noexcept
        : barrier_(barrier), acquired_(barrier_.EnterExecution(diagnostic)) {
    }

    ExecutionBarrierLease(const ExecutionBarrierLease&) = delete;
    ExecutionBarrierLease& operator=(const ExecutionBarrierLease&) = delete;
    ExecutionBarrierLease(ExecutionBarrierLease&&) = delete;
    ExecutionBarrierLease& operator=(ExecutionBarrierLease&&) = delete;

    ~ExecutionBarrierLease() {
        if (acquired_) {
            barrier_.LeaveExecution();
        }
    }

    bool Acquired() const noexcept {
        return acquired_;
    }

private:
    GuestMemoryMutationBarrier& barrier_;
    bool acquired_ {};
};

class QuiescentHostWaitLease final {
public:
    explicit QuiescentHostWaitLease(
        GuestMemoryMutationBarrier& barrier) noexcept
        : barrier_(barrier), token_(barrier_.BeginQuiescentHostWait()) {
    }

    QuiescentHostWaitLease(const QuiescentHostWaitLease&) = delete;
    QuiescentHostWaitLease& operator=(const QuiescentHostWaitLease&) = delete;
    QuiescentHostWaitLease(QuiescentHostWaitLease&&) = delete;
    QuiescentHostWaitLease& operator=(QuiescentHostWaitLease&&) = delete;

    ~QuiescentHostWaitLease() {
        if (token_ != 0) {
            (void)barrier_.EndQuiescentHostWait(token_);
        }
    }

    bool Acquired() const noexcept {
        return token_ != 0;
    }

private:
    GuestMemoryMutationBarrier& barrier_;
    const std::uint64_t token_ {};
};

} // namespace Lumen::Android::FexCoreLab
