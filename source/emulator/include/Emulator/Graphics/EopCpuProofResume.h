#ifndef EMULATOR_GRAPHICS_EOP_CPU_PROOF_RESUME_H_
#define EMULATOR_GRAPHICS_EOP_CPU_PROOF_RESUME_H_

#include "Lumen/Core/Threads.h"

namespace Lumen::Libs::Graphics {

struct EopCpuProofResult
{
	bool complete = false;
	bool unavailable = false;
	const char* reason = nullptr;
};

// This scope belongs only to EOP -> materializer -> WriteBack: each owns one
// processor/tracker level. RunLock remains held. It is not a general mutex
// unlock helper and must not be used by ordinary writeback or nested callbacks.
template <typename UnlockProcessor, typename LockProcessor, typename Backoff>
void WithEopCpuProofLocksReleased(Core::Mutex& tracker, UnlockProcessor unlock_processor,
                                  LockProcessor lock_processor, Backoff backoff)
{
	struct ReleasedLocks
	{
		Core::Mutex& tracker;
		LockProcessor& lock_processor;
		~ReleasedLocks()
		{
			lock_processor();
			lock_processor();
			tracker.Lock();
			tracker.Lock();
		}
	};
	tracker.Unlock();
	tracker.Unlock();
	unlock_processor();
	unlock_processor();
	ReleasedLocks release {tracker, lock_processor};
	backoff();
}

// After WriteBack returns, the EOP materializer owns only its original one
// processor/tracker level. Release that exact pair for a failed CPU acquisition;
// the completed GPU prefix, callback cursor and batch RunLock stay retained.
// This scope cannot be used inside WriteBack or an ordinary nested callback.
template <typename UnlockProcessor, typename LockProcessor, typename Backoff>
void WithEopMaterializedCpuProofLocksReleased(Core::Mutex& tracker, UnlockProcessor unlock_processor,
                                              LockProcessor lock_processor, Backoff backoff)
{
	struct ReleasedLocks
	{
		Core::Mutex& tracker;
		LockProcessor& lock_processor;
		~ReleasedLocks()
		{
			lock_processor();
			tracker.Lock();
		}
	};
	tracker.Unlock();
	unlock_processor();
	ReleasedLocks release {tracker, lock_processor};
	backoff();
}

// Preserve the caller's phase/cursor and prepared GPU proof. Only a failed CPU
// acquisition can yield; neither a successful scan nor an effect is replayed.
// Backoffs share a single budget across all proof boundaries in one WriteBack.
template <typename Proof, typename Validate, typename Yield, typename LeaseHeld>
EopCpuProofResult ResumeEopCpuProof(Proof proof, Validate validate, Yield yield,
                                   LeaseHeld lease_held, uint32_t* backoffs)
{
	bool deferred = false;
	for (;;)
	{
		const auto result = deferred ? EopCpuProofResult {false, true, "continuity-lease-unavailable"} : proof();
		if (result.complete || !result.unavailable) return result;
		if (lease_held()) return {false, false, "retry-memory-lease-held"};
		const auto before = validate();
		if (!before.complete && !before.unavailable) return before;
		if (*backoffs >= 15) return {false, false, "cpu-proof-resume-exhausted"};
		++*backoffs;
		yield();
		const auto after = validate();
		if (!after.complete && !after.unavailable) return after;
		deferred = !after.complete;
	}
}

} // namespace Lumen::Libs::Graphics

#endif
