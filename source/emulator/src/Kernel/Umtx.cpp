#include "Emulator/Kernel/Umtx.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::Umtx {

namespace {

using Posix::POSIX_EBUSY;
using Posix::POSIX_EFAULT;
using Posix::POSIX_EINVAL;
using Posix::POSIX_EOVERFLOW;
using Posix::POSIX_ETIMEDOUT;

// Operation ids of the shared FreeBSD/Orbis profile. The anchor is op 19: the
// title's statically linked semaphore code waits with `_umtx_op(&sem->waiters,
// 19, 0, NULL, &timeout)` while its user-space count sits four bytes above the
// wait word, and the surrounding wrappers resolve to the POSIX semaphore
// exports of the same namespace (`sem_wait`, `sem_post`, `sem_trywait`,
// `sem_destroy`), so the profile in use is the POSIX one.
// Ids 0 (LOCK), 1 (UNLOCK), 7 (SET_CEILING) and the read/write family 12-14
// stay outside the covered profile: they either name internal sleep-queue
// operations or a lock protocol this adapter does not reproduce, and they fail
// closed with the numeric trace instead of a guessed semantic.
constexpr int32_t kOpWait = 2;
constexpr int32_t kOpWake = 3;
constexpr int32_t kOpMutexTrylock = 4;
constexpr int32_t kOpMutexLock = 5;
constexpr int32_t kOpMutexUnlock = 6;
constexpr int32_t kOpCvWait = 8;
constexpr int32_t kOpCvSignal = 9;
constexpr int32_t kOpCvBroadcast = 10;
constexpr int32_t kOpWaitUint = 11;
constexpr int32_t kOpWaitUintPrivate = 15;
constexpr int32_t kOpWakePrivate = 16;
constexpr int32_t kOpMutexWait = 17;
constexpr int32_t kOpMutexWake = 18;
constexpr int32_t kOpSemWait = 19;
constexpr int32_t kOpSemWake = 20;
constexpr int32_t kOpNwakePrivate = 21;
constexpr int32_t kOpMutexWake2 = 22;
constexpr int32_t kOpSem2Wait = 23;
constexpr int32_t kOpSem2Wake = 24;

// Bounded host slice of the wait loop: the same model as the other HLE waits
// (semaphores, event flags, sleep). `ScopedGuestHostWait` restores the guest
// reader and delivers a pending guest exception between slices.
constexpr uint32_t kSliceMicros = 10000;

constexpr uint32_t kMaximumTraceRecords = 4096;

struct UmtxObject
{
	Core::Mutex mutex;
	Core::CondVar cond;
	uint64_t generation = 0;
	uint32_t waiters = 0;
};

struct UmtxTable
{
	Core::Mutex lock;
	std::unordered_map<uint64_t, std::shared_ptr<UmtxObject>> objects;
};

UmtxTable& table()
{
	static UmtxTable instance;
	return instance;
}

uint64_t monotonic_ns()
{
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(
	        std::chrono::steady_clock::now().time_since_epoch())
	        .count());
}

bool trace_enabled()
{
	static const bool enabled = []
	{
		const char* value = std::getenv("LUMEN_KERNEL_UMTX_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

void trace_record(const char* event, uint64_t address, int32_t op, uint32_t val,
                  uint64_t uaddr1, uint64_t uaddr2, int32_t result, int32_t error,
                  uint32_t waiters)
{
	if (!trace_enabled()) return;
	static std::atomic<uint32_t> records {0};
	if (records.fetch_add(1, std::memory_order_relaxed) >= kMaximumTraceRecords) return;
	std::fprintf(stderr,
	             "[KERNEL_UMTX_TRACE] ns=%llu thread=%d event=%s op=%d obj=0x%016llx val=0x%08x"
	             " uaddr1=0x%016llx uaddr2=0x%016llx result=%d error=%d waiters=%u\n",
	             static_cast<unsigned long long>(monotonic_ns()), Core::Thread::GetThreadIdUnique(),
	             event, op, static_cast<unsigned long long>(address), val,
	             static_cast<unsigned long long>(uaddr1), static_cast<unsigned long long>(uaddr2),
	             result, error, waiters);
	std::fflush(stderr);
}

std::shared_ptr<UmtxObject> acquire_object(uint64_t address)
{
	auto& objects = table();
	Core::LockGuard guard(objects.lock);
	auto& slot = objects.objects[address];
	if (slot == nullptr) slot = std::make_shared<UmtxObject>();
	return slot;
}

// Drop an idle object so the table only grows with addresses that are still
// waiting; a waiter that acquires after the erase gets a fresh object and only
// misses wakes that were already delivered (the caller re-checks its own
// state before sleeping, so a stored wake is never required).
void prune_object(uint64_t address, const std::shared_ptr<UmtxObject>& object)
{
	auto& objects = table();
	Core::LockGuard guard(objects.lock);
	Core::LockGuard object_guard(object->mutex);
	if (object->waiters != 0) return;
	const auto found = objects.objects.find(address);
	if (found == objects.objects.end() || found->second != object) return;
	objects.objects.erase(found);
}

bool read_word(uint64_t address, uint32_t* word)
{
	return Loader::ReadGuestMemory(address, word, sizeof(*word));
}

bool write_word(uint64_t address, uint32_t word)
{
	return Loader::WriteGuestMemory(address, &word, sizeof(word));
}

// Relative timeout the operations carry in `uaddr2`: a bare timespec beside the
// wait word, the same record the title's semaphore code keeps and shrinks after
// every wait. The kernel never writes it; a null pointer waits forever.
bool read_relative_timeout(uint64_t address, uint64_t* deadline_ns, bool* infinite, int* error)
{
	*infinite = address == 0;
	*deadline_ns = UINT64_MAX;
	if (*infinite) return true;
	LibKernel::KernelTimespec record {};
	if (!Loader::ReadGuestMemory(address, &record, sizeof(record)))
	{
		*error = POSIX_EFAULT;
		return false;
	}
	if (record.tv_sec < 0 || record.tv_nsec < 0)
	{
		*error = POSIX_EINVAL;
		return false;
	}
	constexpr uint64_t kMaxSeconds = 1000000000ull;
	const uint64_t seconds = record.tv_sec > static_cast<int64_t>(kMaxSeconds)
	    ? kMaxSeconds : static_cast<uint64_t>(record.tv_sec);
	const uint64_t nanos = seconds * 1000000000ull + static_cast<uint64_t>(record.tv_nsec);
	const uint64_t now = monotonic_ns();
	*deadline_ns = nanos > UINT64_MAX - now ? UINT64_MAX - 1 : now + nanos;
	return true;
}

// Sleep on the object's wake generation in bounded slices. `ready` re-checks
// the operation's predicate under the object lock; `wake_is_success` selects
// whether an advanced generation completes the wait (semaphore profile, where
// the caller re-checks its own count) or only prompts another check (word
// profile, where the word itself must differ).
template <typename Ready>
bool wait_on_object(uint64_t address, uint64_t deadline_ns, bool infinite,
                    bool wake_is_success, Ready&& ready)
{
	const auto object = acquire_object(address);
	bool completed = false;
	object->mutex.Lock();
	const uint64_t observed = object->generation;
	++object->waiters;
	for (;;)
	{
		if (ready())
		{
			completed = true;
			break;
		}
		if (!infinite && monotonic_ns() >= deadline_ns) break;
		uint32_t slice = kSliceMicros;
		if (!infinite)
		{
			const uint64_t now = monotonic_ns();
			const uint64_t remaining_ns = deadline_ns > now ? deadline_ns - now : 0;
			const uint64_t remaining_us = remaining_ns / 1000;
			if (remaining_us < slice) slice = static_cast<uint32_t>(remaining_us);
			if (slice == 0) break;
		}
		{
			Loader::ScopedGuestHostWait host_wait;
			object->cond.WaitFor(&object->mutex, slice);
			object->mutex.Unlock();
		}
		object->mutex.Lock();
		if (wake_is_success && object->generation != observed)
		{
			completed = true;
			break;
		}
	}
	--object->waiters;
	const bool idle = object->waiters == 0;
	object->mutex.Unlock();
	if (idle) prune_object(address, object);
	return completed;
}

void wake_object(uint64_t address)
{
	const auto object = acquire_object(address);
	{
		Core::LockGuard guard(object->mutex);
		++object->generation;
		object->cond.SignalAll();
	}
	prune_object(address, object);
}

// SEM2_WAIT keeps the plain word profile: the caller owns whichever word it
// names and the wait completes when that word differs or a wake arrives.
bool wait_sem2_word(uint64_t address, uint32_t expected, uint64_t deadline_ns, bool infinite)
{
	return wait_on_object(address, deadline_ns, infinite, true,
	                      [&] { uint32_t word = 0; return !read_word(address, &word) || word != expected; });
}

bool wait_word_change(uint64_t address, uint32_t expected, uint64_t deadline_ns, bool infinite)
{
	return wait_on_object(address, deadline_ns, infinite, false,
	                      [&] { uint32_t word = 0; return !read_word(address, &word) || word != expected; });
}

bool wait_mutex_word(uint64_t address, uint64_t deadline_ns, bool infinite)
{
	return wait_on_object(address, deadline_ns, infinite, false,
	                      [&]
	                      {
		                      uint32_t word = 0;
		                      return !read_word(address, &word) || (word & 1u) == 0;
	                      });
}

bool mutex_trylock(uint64_t address)
{
	uint32_t word = 0;
	if (!read_word(address, &word)) return false;
	if ((word & 1u) != 0) return false;
	return write_word(address, word | 1u);
}

bool mutex_unlock(uint64_t address)
{
	uint32_t word = 0;
	if (!read_word(address, &word)) return false;
	if (!write_word(address, word & ~1u)) return false;
	wake_object(address);
	return true;
}

} // namespace

bool LeasedGuestWord::Open(uint64_t address, bool write)
{
	if (address == 0) return false;
#if defined(LUMEN_FEXCORE_RUNTIME) && \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        address, sizeof(uint32_t), write ? Core::VirtualMemory::Mode::Write
	                                         : Core::VirtualMemory::Mode::Read,
	        &m_lease))
	{
		return false;
	}
	m_word = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(m_lease.HostBegin()));
	return m_word != nullptr;
#else
	if (!Loader::ValidateGuestMemory(address, sizeof(uint32_t), write)) return false;
	m_word = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(address));
	return true;
#endif
}

void WakeSemaphoreWaitersWord(uint64_t waiters_word)
{
	wake_object(waiters_word);
}

// Publish or clear the waiter hint with a bounded borrow. The wait itself must
// not keep a VM lease: a lease owned across a host wait holds the VM reader
// gate open, so a pending mutation could never drain its readers and the whole
// guest would stall (the quiescent-host-wait contract in
// Lumen/Core/VirtualMemory.h). Every access revalidates the word, and a stale
// positive hint only costs a spurious wake, which the waiter tolerates because
// it re-checks its own count on every slice.
void AdjustSemaphoreWaiterHint(uint64_t waiters_word, bool add)
{
	LeasedGuestWord hint;
	if (!hint.Open(waiters_word, true)) return;
	if (add)
	{
		__atomic_fetch_add(hint.Word(), 1U, __ATOMIC_ACQ_REL);
		return;
	}
	uint32_t observed = __atomic_load_n(hint.Word(), __ATOMIC_ACQUIRE);
	while (!__atomic_compare_exchange_n(hint.Word(), &observed,
	                                    observed != 0 ? observed - 1 : 0, false,
	                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
	{
	}
}

bool WaitSemaphoreWaitersWord(uint64_t waiters_word, uint32_t relative_micros, bool infinite)
{
	// Mark the wait before touching the object: the release path reads the hint
	// and only then may decide to skip the wake.
	AdjustSemaphoreWaiterHint(waiters_word, true);
	const uint64_t deadline_ns = infinite
	    ? UINT64_MAX
	    : monotonic_ns() + static_cast<uint64_t>(relative_micros) * 1000ull;
	const bool completed = wait_on_object(
	    waiters_word, deadline_ns, infinite, true,
	    [&]
	    {
		    uint32_t count = 0;
		    return !read_word(waiters_word + sizeof(uint32_t), &count) || static_cast<int32_t>(count) > 0;
	    });
	AdjustSemaphoreWaiterHint(waiters_word, false);
	return completed;
}

bool TakeSemaphoreToken(uint64_t slot, bool* taken, int* error)
{
	if (taken == nullptr || error == nullptr) return false;
	*taken = false;
	*error = 0;
	LeasedGuestWord count;
	if (!count.Open(slot + 2 * sizeof(uint32_t), true))
	{
		*error = POSIX_EFAULT;
		return false;
	}
	uint32_t value = __atomic_load_n(count.Word(), __ATOMIC_ACQUIRE);
	while (static_cast<int32_t>(value) > 0)
	{
		if (__atomic_compare_exchange_n(count.Word(), &value, value - 1, false,
		                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		{
			*taken = true;
			break;
		}
	}
	return true;
}

int ReleaseSemaphoreToken(uint64_t slot)
{
	LeasedGuestWord count;
	if (!count.Open(slot + 2 * sizeof(uint32_t), true)) return POSIX_EFAULT;
	constexpr uint32_t maximum = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
	uint32_t value = __atomic_load_n(count.Word(), __ATOMIC_ACQUIRE);
	for (;;)
	{
		if (value >= maximum) return POSIX_EOVERFLOW;
		if (__atomic_compare_exchange_n(count.Word(), &value, value + 1, false,
		                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		{
			break;
		}
	}
	wake_object(slot + sizeof(uint32_t));
	return 0;
}

int LUMEN_SYSV_ABI UmtxOp(uint64_t obj, int32_t op, uint32_t val, uint64_t uaddr1, uint64_t uaddr2)
{
	using Posix::GetErrorAddr;
	const auto finish = [&](int result, int error, uint32_t waiters = 0)
	{
		trace_record("op", obj, op, val, uaddr1, uaddr2, result, error, waiters);
		if (result != 0) *GetErrorAddr() = error;
		return result;
	};
	if (obj == 0) return finish(-1, POSIX_EINVAL);
	if (!Loader::ValidateGuestMemory(obj, sizeof(uint32_t), true))
		return finish(-1, POSIX_EFAULT);

	uint64_t deadline_ns = UINT64_MAX;
	bool infinite = false;
	int timeout_error = 0;
	if (op == kOpSemWait || op == kOpSem2Wait || op == kOpWait || op == kOpWaitUint ||
	    op == kOpWaitUintPrivate || op == kOpMutexWait || op == kOpCvWait)
	{
		if (!read_relative_timeout(uaddr2, &deadline_ns, &infinite, &timeout_error))
			return finish(-1, timeout_error);
	}

	switch (op)
	{
	case kOpWait:
	case kOpWaitUint:
	case kOpWaitUintPrivate:
		if (!wait_word_change(obj, val, deadline_ns, infinite)) return finish(-1, POSIX_ETIMEDOUT);
		return finish(0, 0);

	case kOpMutexWait:
		if (!wait_mutex_word(obj, deadline_ns, infinite)) return finish(-1, POSIX_ETIMEDOUT);
		return finish(0, 0);

	case kOpSemWait:
	{
		// The covered console profile: `obj` is the sem_t waiters word and the
		// token count sits four bytes above it. The wait publishes the hint and
		// re-checks the count every slice, so a release that skips the explicit
		// wake still cannot strand the waiter.
		const uint64_t now = monotonic_ns();
		const uint64_t remaining_ns = infinite || deadline_ns <= now ? 0 : deadline_ns - now;
		const uint64_t remaining_us = remaining_ns / 1000;
		const uint32_t budget = remaining_us > UINT32_MAX ? UINT32_MAX
		                                                  : static_cast<uint32_t>(remaining_us);
		if (!WaitSemaphoreWaitersWord(obj, budget, infinite)) return finish(-1, POSIX_ETIMEDOUT);
		return finish(0, 0);
	}

	case kOpSem2Wait:
		if (!wait_sem2_word(obj, val, deadline_ns, infinite)) return finish(-1, POSIX_ETIMEDOUT);
		return finish(0, 0);

	case kOpCvWait:
	{
		// Release the caller's mutex word, wait for a wake on the condition
		// word and re-acquire the mutex before returning, the standard
		// condition-variable contract the console implements over the same
		// words. The mutex profile is the flat 0/1 word: the ownership bits of
		// robust mutexes are not reproduced here (documented boundary).
		if (uaddr1 == 0 || !Loader::ValidateGuestMemory(uaddr1, sizeof(uint32_t), true))
			return finish(-1, POSIX_EFAULT);
		if (!write_word(uaddr1, 0) || !Loader::ValidateGuestMemory(uaddr1, sizeof(uint32_t), false))
			return finish(-1, POSIX_EFAULT);
		wake_object(uaddr1);
		const bool signalled = wait_sem2_word(obj, val, deadline_ns, infinite);
		while (!mutex_trylock(uaddr1))
		{
			if (!wait_mutex_word(uaddr1, deadline_ns, infinite)) break;
		}
		if (!signalled) return finish(-1, POSIX_ETIMEDOUT);
		return finish(0, 0);
	}

	case kOpWake:
	case kOpWakePrivate:
	case kOpMutexWake:
	case kOpMutexWake2:
	case kOpSemWake:
	case kOpSem2Wake:
	case kOpCvSignal:
	case kOpCvBroadcast:
		wake_object(obj);
		return finish(0, 0);

	case kOpNwakePrivate:
	{
		// The value names how many addresses the caller lists in `uaddr1`.
		// Each listed address wakes like the ordinary wake above.
		const uint32_t count = val == 0 ? 1 : (val > 64 ? 64 : val);
		if (uaddr1 == 0 || !Loader::ValidateGuestMemory(uaddr1, count * sizeof(uint64_t), false))
			return finish(-1, POSIX_EFAULT);
		for (uint32_t index = 0; index < count; ++index)
		{
			uint64_t address = 0;
			if (!Loader::ReadGuestMemory(uaddr1 + index * sizeof(uint64_t), &address, sizeof(address)))
				return finish(-1, POSIX_EFAULT);
			if (address != 0) wake_object(address);
		}
		return finish(0, 0);
	}

	case kOpMutexTrylock:
		if (!mutex_trylock(obj)) return finish(-1, POSIX_EBUSY);
		return finish(0, 0);

	case kOpMutexLock:
	{
		// The console mutex lock carries no timeout: the wait stays infinite
		// until the word frees, mirroring the blocking contract of the call.
		for (;;)
		{
			if (mutex_trylock(obj)) return finish(0, 0);
			if (!wait_mutex_word(obj, UINT64_MAX, true)) return finish(-1, POSIX_ETIMEDOUT);
		}
	}

	case kOpMutexUnlock:
		if (!mutex_unlock(obj)) return finish(-1, POSIX_EFAULT);
		return finish(0, 0);

	default:
		// Operations outside the covered profile fail closed with the numeric
		// record so the run names the exact op before anything else observes a
		// guessed semantic.
		return finish(-1, POSIX_EINVAL);
	}
}

} // namespace Lumen::Libs::LibKernel::Umtx

#endif // LUMEN_EMU_ENABLED
