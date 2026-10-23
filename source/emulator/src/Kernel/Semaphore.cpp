#include "Emulator/Kernel/Semaphore.h"

#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Timer.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Kernel/Umtx.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::Semaphore {

LIB_NAME("libkernel", "libkernel");

namespace {

void TraceSemaphore(const char* family, const char* operation, const void* slot,
                    KernelSema handle, int error, uint32_t initial = 0)
{
	static const bool enabled = []
	{
		const char* value = std::getenv("LUMEN_POSIX_SEMAPHORE_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> records {0};
	uint32_t count = records.load(std::memory_order_relaxed);
	for (;;)
	{
		if (count >= 64) return;
		if (records.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) break;
	}
	// Only report arguments and the handle already read by the operation.
	// In particular, an unregistered value is never dereferenced for tracing.
	std::fprintf(stderr,
	             "[POSIX_SEMAPHORE_TRACE_V1] family=%s operation=%s slot=%p"
	             " handle=%p error=%d initial=%u\n",
	             family, operation, slot, static_cast<void*>(handle), error, initial);
}

void TraceInlineSemaphoreCandidate(const char* operation, const void* slot)
{
	static const bool enabled = []
	{
		const char* value = std::getenv("LUMEN_POSIX_SEMAPHORE_OBJECT_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	if (!enabled || slot == nullptr) return;
	static std::atomic<uint32_t> records {0};
	uint32_t count = records.load(std::memory_order_relaxed);
	for (;;)
	{
		if (count >= 64) return;
		if (records.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) break;
	}
	std::array<uint32_t, 4> words {};
#if defined(__aarch64__)
	const bool readable = Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(slot),
	                                               words.data(), sizeof(words));
#else
	const bool readable = false;
#endif
	if (!readable) words.fill(0);
	std::fprintf(stderr,
	             "[POSIX_SEMAPHORE_OBJECT_V1] operation=%s slot=%p readable16=%u"
	             " word0=0x%08x word1=0x%08x word2=0x%08x word3=0x%08x\n",
	             operation, slot, readable ? 1U : 0U, words[0], words[1], words[2], words[3]);
	std::fflush(stderr);
}

void SleepForObjectProgress(Core::Mutex* mutex)
{
	EXIT_IF(mutex == nullptr);
	mutex->Unlock();
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
	{
		// Keep waiter tracking as the object's lifetime pin.  A cancel/delete
		// HLE instead yields its guest-memory reader while it waits, allowing an
		// Exclusive mutation and waiter readmission to complete without a cycle.
		Loader::ScopedGuestHostWait host_wait;
		Core::Thread::SleepMicro(10);
	}
#else
	Core::Thread::SleepMicro(10);
#endif
	mutex->Lock();
}

} // namespace

class KernelSemaPrivate
{
public:
	enum class Result
	{
		Ok,
		TimedOut,
		Canceled,
		Deleted,
		InvalCount
	};

	KernelSemaPrivate(const String& name, bool /*fifo*/, int init_count, int max_count)
	    : m_name(name), /*m_fifo_order(fifo),*/ m_count(init_count), m_init_count(init_count), m_max_count(max_count) {};
	virtual ~KernelSemaPrivate();

	LUMEN_CLASS_NO_COPY(KernelSemaPrivate);

	Result Cancel(int set_count, int* num_waiting_threads);
	Result Signal(int signal_count);
	Result Wait(int need_count, uint32_t* ptr_micros);

	Result Poll(int need_count)
	{
		uint32_t micros = 0;
		return Wait(need_count, &micros);
	}

	[[nodiscard]] const String& GetName() const { return m_name; }

	int Count()
	{
		Core::LockGuard lock(m_mutex);
		return m_count > 0 ? m_count : 0;
	}

	bool HasWaiters()
	{
		Core::LockGuard lock(m_mutex);
		return m_status != Status::Set || !m_waiting_threads.IsEmpty();
	}

#if defined(LUMEN_POSIX_SEMAPHORE_TEST_HOOK)
	uint32_t WaiterCountForTest()
	{
		Core::LockGuard lock(m_mutex);
		return m_waiting_threads.Size();
	}
#endif

private:
	enum class Status
	{
		Set,
		Canceled,
		Deleted
	};

	Core::Mutex   m_mutex;
	Core::CondVar m_cond_var;
	String        m_name;
	Status        m_status = Status::Set;
	Vector<int>   m_waiting_threads;
	// bool          m_fifo_order;
	int m_count;
	int m_init_count;
	int m_max_count;
};

namespace {

std::mutex& sema_registry_mutex()
{
	static std::mutex mutex;
	return mutex;
}

std::unordered_set<KernelSema>& sema_registry()
{
	static std::unordered_set<KernelSema> registry;
	return registry;
}

// The console `sem_t` is the sixteen-byte record the guest publishes itself:
// {magic | registration handle, waiters hint, count, flags}. The count field is
// the semaphore's state and the waiters word is the address the console waits
// on, so the POSIX entry points and a title's own statically linked semaphore
// code operate on the same object with no second counter to keep in step.
struct InlinePosixSemaphoreRecord
{
	uint32_t magic;
	uint32_t has_waiters;
	uint32_t count;
	uint32_t flags;
};

static_assert(sizeof(InlinePosixSemaphoreRecord) == 16 && alignof(InlinePosixSemaphoreRecord) == 4);

void register_sema(KernelSema sema)
{
	std::lock_guard lock(sema_registry_mutex());
	sema_registry().insert(sema);
}

void unregister_sema(KernelSema sema)
{
	std::lock_guard lock(sema_registry_mutex());
	sema_registry().erase(sema);
}

bool is_registered_sema(KernelSema sema)
{
	std::lock_guard lock(sema_registry_mutex());
	return sema_registry().count(sema) != 0;
}

std::vector<KernelSema> remove_all_semas()
{
	std::vector<KernelSema> semaphores;
	{
		std::lock_guard lock(sema_registry_mutex());
		semaphores.reserve(sema_registry().size());
		for (auto* semaphore: sema_registry())
		{
			semaphores.push_back(semaphore);
		}
		sema_registry().clear();
	}
	return semaphores;
}

} // namespace

#if defined(LUMEN_POSIX_SEMAPHORE_TEST_HOOK)
uint32_t SemaphoreWaiterCountForTest(KernelSema sem)
{
	return sem->WaiterCountForTest();
}
#endif

KernelSemaPrivate::~KernelSemaPrivate()
{
	Core::LockGuard lock(m_mutex);

	while (m_status != Status::Set)
	{
		SleepForObjectProgress(&m_mutex);
	}

	m_status = Status::Deleted;

	m_cond_var.SignalAll();

	while (!m_waiting_threads.IsEmpty())
	{
		SleepForObjectProgress(&m_mutex);
	}
}

KernelSemaPrivate::Result KernelSemaPrivate::Cancel(int set_count, int* num_waiting_threads)
{
	Core::LockGuard lock(m_mutex);

	if (set_count > m_max_count)
	{
		return Result::InvalCount;
	}

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set)
	{
		SleepForObjectProgress(&m_mutex);
	}

	if (num_waiting_threads != nullptr)
	{
		*num_waiting_threads = static_cast<int>(m_waiting_threads.Size());
	}

	m_status = Status::Canceled;

	m_count = (set_count < 0 ? m_init_count : set_count);

	m_cond_var.SignalAll();

	while (!m_waiting_threads.IsEmpty())
	{
		SleepForObjectProgress(&m_mutex);
	}

	m_status = Status::Set;

	return Result::Ok;
}

KernelSemaPrivate::Result KernelSemaPrivate::Signal(int signal_count)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set)
	{
		SleepForObjectProgress(&m_mutex);
	}

	const int64_t next_count = static_cast<int64_t>(m_count) + signal_count;
	if (next_count > m_max_count || next_count < std::numeric_limits<int>::min())
	{
		return Result::InvalCount;
	}

	m_count = static_cast<int>(next_count);

	m_cond_var.SignalAll();

	return Result::Ok;
}

KernelSemaPrivate::Result KernelSemaPrivate::Wait(int need_count, uint32_t* ptr_micros)
{
	Core::LockGuard lock(m_mutex);

	if (need_count < 1 || need_count > m_max_count)
	{
		return Result::InvalCount;
	}

	uint32_t micros     = 0;
	bool     infinitely = true;
	if (ptr_micros != nullptr)
	{
		micros     = *ptr_micros;
		infinitely = false;
	}

	uint32_t    elapsed = 0;
	Core::Timer t;
	t.Start();

	int id = Core::Thread::GetThreadIdUnique();

	while (m_count < need_count)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (Loader::GuestExecutionStopRequested())
		{
			return Result::Canceled;
		}
#endif
		if ((elapsed >= micros && !infinitely))
		{
			*ptr_micros = 0;
			return Result::TimedOut;
		}

		m_waiting_threads.Add(id);
		if (infinitely)
		{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
			{
				Loader::ScopedGuestHostWait host_wait;
				m_cond_var.WaitFor(&m_mutex, 10000);
				// WaitFor reacquires m_mutex.  Drop it before host_wait restores
				// the guest reader so a different HLE can finish draining through
				// this semaphore while an exclusive mutation is pending.  The
				// waiter stays registered as a lifetime pin; Cancel/Delete yield
				// their own reader while waiting for it to finish readmission.
				m_mutex.Unlock();
			}
			m_mutex.Lock();
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
			m_cond_var.WaitFor(&m_mutex, 10000);
#else
			m_cond_var.Wait(&m_mutex);
#endif
		} else
		{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
			const uint32_t remaining = micros - elapsed;
			{
				Loader::ScopedGuestHostWait host_wait;
				m_cond_var.WaitFor(&m_mutex, remaining > 10000 ? 10000 : remaining);
				m_mutex.Unlock();
			}
			m_mutex.Lock();
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
			const uint32_t remaining = micros - elapsed;
			m_cond_var.WaitFor(&m_mutex, remaining > 10000 ? 10000 : remaining);
#else
			m_cond_var.WaitFor(&m_mutex, micros - elapsed);
#endif
		}

		m_waiting_threads.Remove(id);

		const double elapsed_micros = t.GetTimeS() * 1000000.0;
		elapsed = elapsed_micros >= static_cast<double>(UINT32_MAX)
		    ? UINT32_MAX : static_cast<uint32_t>(elapsed_micros);

		if (m_status == Status::Canceled)
		{
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Canceled;
		}

		if (m_status == Status::Deleted)
		{
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Deleted;
		}
	}

	m_count -= need_count;

	if (ptr_micros != nullptr)
	{
		*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
	}

	return Result::Ok;
}

int LUMEN_SYSV_ABI KernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(sem == nullptr);

	if (name == nullptr || init < 0 || init > max || opt != nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	bool fifo = false;

	switch (attr)
	{
		case 0x01: fifo = true; break;
		case 0x02:
		default: fifo = false; break;
	}

	*sem = new KernelSemaPrivate(String::FromUtf8(name), fifo, init, max);
	register_sema(*sem);

	printf("\t Semaphore create: %s, %d, %d\n", name, init, max);

	return OK;
}

int LUMEN_SYSV_ABI KernelDeleteSema(KernelSema sem)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	unregister_sema(sem);
	delete sem;

	return OK;
}

static int wait_sema_host(KernelSema sem, int need, KernelUseconds* time)
{
	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	auto result = sem->Wait(need, time);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount: ret = KERNEL_ERROR_EINVAL; break;
		case KernelSemaPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelSemaPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}

	return ret;
}

int LUMEN_SYSV_ABI KernelWaitSema(KernelSema sem, int need, KernelUseconds* time)
{
	PRINT_NAME();

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
	KernelUseconds host_time = 0;
	if (time != nullptr &&
	    (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(time), &host_time, sizeof(host_time)) ||
	     !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(time), sizeof(*time), true)))
	{
		return KERNEL_ERROR_EFAULT;
	}
	const int result = wait_sema_host(sem, need, time != nullptr ? &host_time : nullptr);
	if (time != nullptr &&
	    (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(time), sizeof(*time), true) ||
	     !Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(time), &host_time, sizeof(host_time))))
	{
		return KERNEL_ERROR_EFAULT;
	}
	return result;
#else
	return wait_sema_host(sem, need, time);
#endif
}

int LUMEN_SYSV_ABI KernelPollSema(KernelSema sem, int need)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	printf("\t Semaphore poll: %s, %d\n", sem->GetName().C_Str(), need);

	auto result = sem->Poll(need);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount: ret = KERNEL_ERROR_EINVAL; break;
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EBUSY; break;
	}

	return ret;
}

int LUMEN_SYSV_ABI KernelSignalSema(KernelSema sem, int count)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	printf("\t Semaphore signal: %s, %d\n", sem->GetName().C_Str(), count);

	auto result = sem->Signal(count);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount:
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EINVAL; break;
	}

	return ret;
}

int LUMEN_SYSV_ABI KernelCancelSema(KernelSema sem, int count, int* threads)
{
	PRINT_NAME();

	if (sem == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	auto result = sem->Cancel(count, threads);

	int ret = OK;

	switch (result)
	{
		case KernelSemaPrivate::Result::Ok: ret = OK; break;
		case KernelSemaPrivate::Result::InvalCount:
		case KernelSemaPrivate::Result::TimedOut:
		case KernelSemaPrivate::Result::Canceled:
		case KernelSemaPrivate::Result::Deleted: ret = KERNEL_ERROR_EINVAL; break;
	}

	return ret;
}

namespace {

int read_pthread_semaphore(KernelSema* slot, KernelSema* handle)
{
	static_assert(sizeof(KernelSema) == 8);
	const auto finish = [&](int error)
	{
		TraceSemaphore("pthread", "read", slot, *handle, error);
		return error;
	};
	if (slot == nullptr) return finish(KERNEL_ERROR_EINVAL);
#if defined(__aarch64__)
	if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(slot), handle, sizeof(*handle)))
		return finish(KERNEL_ERROR_EFAULT);
#else
	std::memcpy(handle, slot, sizeof(*handle));
#endif
	return finish(*handle != nullptr && is_registered_sema(*handle) ? OK : KERNEL_ERROR_EINVAL);
}

bool can_write_pthread_output(const void* output, size_t size)
{
#if defined(__aarch64__)
	return Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(output), size, true);
#else
	return output != nullptr;
#endif
}

bool write_pthread_output(void* output, const void* value, size_t size)
{
#if defined(__aarch64__)
	return Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(output), value, size);
#else
	std::memcpy(output, value, size);
	return true;
#endif
}

} // namespace

int LUMEN_SYSV_ABI PthreadSemInit(KernelSema* sem, int32_t flag, uint32_t value, const char* /*name*/)
{
	PRINT_NAME();
	constexpr uint32_t maximum = std::numeric_limits<int32_t>::max();
	if (flag != 0 || value > maximum || sem == nullptr) return KERNEL_ERROR_EINVAL;
	if (!can_write_pthread_output(sem, sizeof(*sem))) return KERNEL_ERROR_EFAULT;
	// The optional name is diagnostic, not an identifier for a named or
	// process-shared object. This backend supports the process-local flag 0.
	KernelSema created = nullptr;
	const int result = KernelCreateSema(&created, "pthread-semaphore", 1, static_cast<int>(value),
	                                    static_cast<int>(maximum), nullptr);
	if (result != OK) return result;
	if (!write_pthread_output(sem, &created, sizeof(created)))
	{
		KernelDeleteSema(created);
		return KERNEL_ERROR_EFAULT;
	}
	TraceSemaphore("pthread", "init", sem, created, OK, value);
	return OK;
}

int LUMEN_SYSV_ABI PthreadSemDestroy(KernelSema* sem)
{
	PRINT_NAME();
	KernelSema handle = nullptr;
	const int error = read_pthread_semaphore(sem, &handle);
	if (error != OK) return error;
	if (!can_write_pthread_output(sem, sizeof(*sem))) return KERNEL_ERROR_EFAULT;
	// As with the POSIX entrypoints, the caller owns the handle lifetime and
	// must not race new operations with destruction. Refuse observed waiters
	// rather than destroying a semaphore still used by an existing wait.
	if (handle->HasWaiters()) return KERNEL_ERROR_EBUSY;
	KernelSema empty = nullptr;
	if (!write_pthread_output(sem, &empty, sizeof(empty))) return KERNEL_ERROR_EFAULT;
	return KernelDeleteSema(handle);
}

int LUMEN_SYSV_ABI PthreadSemWait(KernelSema* sem)
{
	PRINT_NAME();
	KernelSema handle = nullptr;
	const int error = read_pthread_semaphore(sem, &handle);
	const int result = error == OK ? wait_sema_host(handle, 1, nullptr) : error;
	TraceSemaphore("pthread", "wait", sem, handle, result);
	return result;
}

int LUMEN_SYSV_ABI PthreadSemTrywait(KernelSema* sem)
{
	PRINT_NAME();
	KernelSema handle = nullptr;
	const int error = read_pthread_semaphore(sem, &handle);
	if (error != OK) return error;
	KernelUseconds timeout = 0;
	const int result = wait_sema_host(handle, 1, &timeout);
	const int mapped = result == KERNEL_ERROR_ETIMEDOUT ? KERNEL_ERROR_EAGAIN : result;
	TraceSemaphore("pthread", "trywait", sem, handle, mapped);
	return mapped;
}

int LUMEN_SYSV_ABI PthreadSemTimedwait(KernelSema* sem, uint32_t usec)
{
	PRINT_NAME();
	KernelSema handle = nullptr;
	const int error = read_pthread_semaphore(sem, &handle);
	// This is a relative microsecond duration passed by value, not the POSIX
	// absolute timespec. Keep it on the host side of the guest-memory boundary.
	const int result = error == OK ? wait_sema_host(handle, 1, &usec) : error;
	TraceSemaphore("pthread", "timedwait", sem, handle, result);
	return result;
}

int LUMEN_SYSV_ABI PthreadSemPost(KernelSema* sem)
{
	PRINT_NAME();
	KernelSema handle = nullptr;
	const int error = read_pthread_semaphore(sem, &handle);
	if (error != OK) return error;
	const int result = KernelSignalSema(handle, 1);
	const int mapped = result == KERNEL_ERROR_EINVAL ? KERNEL_ERROR_EOVERFLOW : result;
	TraceSemaphore("pthread", "post", sem, handle, mapped);
	return mapped;
}

int LUMEN_SYSV_ABI PthreadSemGetvalue(KernelSema* sem, int32_t* value)
{
	PRINT_NAME();
	KernelSema handle = nullptr;
	const int error = read_pthread_semaphore(sem, &handle);
	if (error != OK) return error;
	if (value == nullptr) return KERNEL_ERROR_EINVAL;
	if (!can_write_pthread_output(value, sizeof(*value))) return KERNEL_ERROR_EFAULT;
	const int32_t count = handle->Count();
	return write_pthread_output(value, &count, sizeof(count)) ? OK : KERNEL_ERROR_EFAULT;
}

bool ResetForNextSession()
{
	// The inline profile keeps no process-side state: its objects are the guest
	// sem_t records themselves, so only the owned kernel semaphores reset here.
	// Guest threads are already stopped by this lifecycle contract.
	const std::vector<KernelSema> semaphores = remove_all_semas();
	for (auto* semaphore: semaphores)
	{
		delete semaphore;
	}

	std::lock_guard lock(sema_registry_mutex());
	return sema_registry().empty();
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SemaphoreSessionResetContractTest()
{
	for (int cycle = 0; cycle < 2; cycle++)
	{
		KernelSema semaphore = nullptr;
		if (KernelCreateSema(&semaphore, "lumen_session_reset_test", 0x01, 0, 1, nullptr) != OK || semaphore == nullptr ||
		    !ResetForNextSession())
		{
			return false;
		}
	}
	return ResetForNextSession();
}
#endif

} // namespace Lumen::Libs::LibKernel::Semaphore

namespace Lumen::Libs::Posix {

LIB_NAME("Posix", "libkernel");

namespace {

constexpr int64_t k_nanoseconds_per_second = 1000000000;
constexpr uint32_t k_inline_posix_semaphore_magic = 0xffff736dU;

bool inline_posix_semaphore_environment_value_enabled(const char* value)
{
	return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool inline_posix_semaphore_enabled()
{
	static const bool enabled = inline_posix_semaphore_environment_value_enabled(
	    std::getenv("LUMEN_EXPERIMENTAL_POSIX_INLINE_SEMAPHORE"));
	return enabled;
}

bool read_inline_posix_semaphore_record(
	const void* slot, LibKernel::Semaphore::InlinePosixSemaphoreRecord* record)
{
	if (slot == nullptr || record == nullptr) return false;
#if defined(__aarch64__) || \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(slot), record, sizeof(*record));
#else
	std::memcpy(record, slot, sizeof(*record));
	return true;
#endif
}

bool can_write_inline_posix_semaphore_record(const void* slot)
{
	if (slot == nullptr) return false;
#if defined(__aarch64__) || \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(slot),
	                                   sizeof(LibKernel::Semaphore::InlinePosixSemaphoreRecord), true);
#else
	return true;
#endif
}

bool write_inline_posix_semaphore_record(
	void* slot, const LibKernel::Semaphore::InlinePosixSemaphoreRecord& record)
{
	if (slot == nullptr) return false;
#if defined(__aarch64__) || \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(slot), &record, sizeof(record));
#else
	std::memcpy(slot, &record, sizeof(record));
	return true;
#endif
}

// A slot is usable when its low half carries the console semaphore magic (the
// high half holds the console registration handle of named semaphores, which
// this backend does not interpret), the object is not shared and the signed
// count is not negative. The count and waiters hint stay mutable guest state.
bool inline_posix_semaphore_record_supported(
	const LibKernel::Semaphore::InlinePosixSemaphoreRecord& record)
{
	return (record.magic & 0xffffU) == (k_inline_posix_semaphore_magic & 0xffffU) &&
	       record.flags == 0 && static_cast<int32_t>(record.count) >= 0;
}

int read_inline_posix_semaphore_state(void* slot, LibKernel::Semaphore::InlinePosixSemaphoreRecord* record)
{
	using namespace LibKernel::Semaphore;
	if (slot == nullptr || record == nullptr) return POSIX_EINVAL;
	if (!read_inline_posix_semaphore_record(slot, record) ||
	    !can_write_inline_posix_semaphore_record(slot)) return POSIX_EFAULT;
	if (!inline_posix_semaphore_record_supported(*record)) return POSIX_EINVAL;
	return 0;
}

// The console semaphore profile lives in the umtx adapter so the guest's own
// `_umtx_op` wait and these entry points share one object table and one waiter
// hint. Take consumes a token when the count is positive; release publishes a
// token and wakes every waiter of the record's waiters word.
bool take_inline_posix_semaphore_token(void* slot, bool* taken, int* error)
{
	return LibKernel::Umtx::TakeSemaphoreToken(reinterpret_cast<uint64_t>(slot), taken, error);
}

int release_inline_posix_semaphore_token(void* slot)
{
	return LibKernel::Umtx::ReleaseSemaphoreToken(reinterpret_cast<uint64_t>(slot));
}

int initialize_inline_posix_semaphore(void* slot, unsigned value)
{
	using namespace LibKernel::Semaphore;
	if (slot == nullptr) return POSIX_EINVAL;
	if (!can_write_inline_posix_semaphore_record(slot)) return POSIX_EFAULT;
	const InlinePosixSemaphoreRecord record {
	    k_inline_posix_semaphore_magic, 0, static_cast<uint32_t>(value), 0};
	return write_inline_posix_semaphore_record(slot, record) ? 0 : POSIX_EFAULT;
}

int inline_posix_semaphore_deadline_interval_us(const LibKernel::KernelTimespec& deadline)
{
	LibKernel::KernelTimespec now {};
	if (LibKernel::KernelClockGettimeHost(0, now) != OK) return -POSIX_EFAULT;
	const bool expired = deadline.tv_sec < now.tv_sec ||
	    (deadline.tv_sec == now.tv_sec && deadline.tv_nsec <= now.tv_nsec);
	if (expired) return 0;
	const uint64_t seconds = static_cast<uint64_t>(deadline.tv_sec) - static_cast<uint64_t>(now.tv_sec);
	if (seconds >= 2) return 1000000;
	const int64_t nanos = static_cast<int64_t>(seconds) * 1000000000 + deadline.tv_nsec - now.tv_nsec;
	const int64_t micros = (nanos + 999) / 1000;
	return static_cast<int>(micros > 1000000 ? 1000000 : micros);
}

bool sem_timedwait_carry_environment_value_enabled(const char* value)
{
	return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool sem_timedwait_carry_enabled()
{
	static const bool enabled = sem_timedwait_carry_environment_value_enabled(
	    std::getenv("LUMEN_EXPERIMENTAL_POSIX_SEM_TIMEDWAIT_CARRY"));
	return enabled;
}

bool normalize_sem_timedwait_deadline(const LibKernel::KernelTimespec& input, bool carry_enabled,
	                                  LibKernel::KernelTimespec* output, bool* carried)
{
	*output  = input;
	*carried = false;
	if (input.tv_nsec >= 0 && input.tv_nsec < k_nanoseconds_per_second) return true;
	if (!carry_enabled || input.tv_nsec != k_nanoseconds_per_second ||
	    input.tv_sec == std::numeric_limits<int64_t>::max())
	{
		return false;
	}
	output->tv_sec += 1;
	output->tv_nsec = 0;
	*carried        = true;
	return true;
}

void trace_sem_timedwait_failure(const char* phase, bool inline_route, const void* slot, const void* deadline,
                                int error, const LibKernel::KernelTimespec& supplied, bool supplied_valid,
                                bool carry_known, bool carry_enabled)
{
	if (error == POSIX_ETIMEDOUT || error == POSIX_EINTR) return;
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_POSIX_SEMAPHORE_FAILURE_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> records {0};
	uint32_t count = records.load(std::memory_order_relaxed);
	for (;;)
	{
		if (count >= 16) return;
		if (records.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) break;
	}
	// All values are arguments or snapshots already obtained by the operation.
	// No guest reread or semaphore lock is needed; fail() writes guest errno last.
	const int saved_errno = errno;
	std::fprintf(stderr,
	             "[POSIX_SEMAPHORE_TIMEDWAIT_FAILURE] record=%u cap=16 phase=%s route=%s"
	             " slot=%p deadline=%p error=%d supplied_valid=%u seconds=%lld nanoseconds=%lld"
	             " carry_known=%u carry_enabled=%u\n",
	             count + 1, phase, inline_route ? "inline" : "handle", slot, deadline, error,
	             supplied_valid ? 1u : 0u, static_cast<long long>(supplied.tv_sec),
	             static_cast<long long>(supplied.tv_nsec), carry_known ? 1u : 0u, carry_enabled ? 1u : 0u);
	std::fflush(stderr);
	errno = saved_errno;
}

void publish_sem_timedwait_carry_receipt(const LibKernel::KernelTimespec& input,
	                                     const LibKernel::KernelTimespec& output)
{
	static std::once_flag receipt;
	std::call_once(receipt, [&] {
		std::fprintf(stderr,
		             "[POSIX_SEMAPHORE_TIMEDWAIT_CARRY] selector=1 seconds_before=%lld "
		             "nanoseconds_before=%lld seconds_after=%lld nanoseconds_after=%lld "
		             "policy=exact-one-billion-carry cap=1\n",
		             static_cast<long long>(input.tv_sec), static_cast<long long>(input.tv_nsec),
		             static_cast<long long>(output.tv_sec), static_cast<long long>(output.tv_nsec));
		std::fflush(stderr);
	});
}

} // namespace

#if defined(LUMEN_POSIX_SEMAPHORE_TEST_HOOK)
// The inline profile keeps no process-side state, so the observer hook reports
// whether the guest record itself is still a usable semaphore object.
bool InlinePosixSemaphoreRecordSupportedForTest(const void* slot)
{
	LibKernel::Semaphore::InlinePosixSemaphoreRecord record {};
	if (slot == nullptr || !read_inline_posix_semaphore_record(slot, &record)) return false;
	return inline_posix_semaphore_record_supported(record);
}

bool InlinePosixSemaphoreEnvironmentValueEnabledForTest(const char* value)
{
	return inline_posix_semaphore_environment_value_enabled(value);
}

bool SemTimedWaitCarryEnvironmentValueEnabledForTest(const char* value)
{
	return sem_timedwait_carry_environment_value_enabled(value);
}

bool SemTimedWaitNormalizeDeadlineForTest(const LibKernel::KernelTimespec& input, bool carry_enabled,
	                                      LibKernel::KernelTimespec* output, bool* carried)
{
	return normalize_sem_timedwait_deadline(input, carry_enabled, output, carried);
}
#endif

static int read_semaphore_slot(LibKernel::Semaphore::KernelSema* sem,
	                           LibKernel::Semaphore::KernelSema* handle,
	                           const char* operation)
{
	using namespace LibKernel::Semaphore;
	static_assert(sizeof(KernelSema) == 8);
	const auto finish = [&](int error)
	{
		TraceSemaphore("posix", "read", sem, *handle, error);
		return error;
	};
	if (sem == nullptr) return finish(POSIX_EINVAL);
#if defined(__aarch64__)
	if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(sem), handle, sizeof(*handle)))
		return finish(POSIX_EFAULT);
#else
	std::memcpy(handle, sem, sizeof(*handle));
#endif
	if (*handle == nullptr || !is_registered_sema(*handle))
	{
		TraceInlineSemaphoreCandidate(operation, sem);
		return finish(POSIX_EINVAL);
	}
	return finish(0);
}

int LUMEN_SYSV_ABI sem_post(LibKernel::Semaphore::KernelSema* sem)
{
	PRINT_NAME();
	using namespace LibKernel::Semaphore;
	KernelSema handle = nullptr;
	auto fail = [&](int error)
	{
		TraceSemaphore("posix", "post", sem, handle, error);
		*GetErrorAddr() = error;
		return -1;
	};
	if (inline_posix_semaphore_enabled())
	{
		InlinePosixSemaphoreRecord record {};
		int error = read_inline_posix_semaphore_state(sem, &record);
		if (error == 0) error = release_inline_posix_semaphore_token(sem);
		TraceSemaphore("posix-inline", "post", sem, nullptr, error);
		if (error != 0)
		{
			*GetErrorAddr() = error;
			return -1;
		}
		return 0;
	}
	const int slot_error = read_semaphore_slot(sem, &handle, "post");
	if (slot_error != 0) return fail(slot_error);
	// A fixed release of one on a live POSIX semaphore can only reach EINVAL
	// here when its configured capacity would be exceeded.
	const int result = KernelSignalSema(handle, 1);
	if (result == LibKernel::KERNEL_ERROR_EINVAL) return fail(POSIX_EOVERFLOW);
	// The registry and kernel mutexes make this an ordinary guest-thread path;
	// async-signal-safe reentry is outside the current backend contract.
	if (result != OK) return fail(LibKernel::KernelToPosix(result));
	TraceSemaphore("posix", "post", sem, handle, 0);
	return 0;
}

int LUMEN_SYSV_ABI sem_wait(LibKernel::Semaphore::KernelSema* sem)
{
	PRINT_NAME();
	using namespace LibKernel::Semaphore;
	KernelSema handle = nullptr;
	auto fail = [&](int error)
	{
		TraceSemaphore("posix", "wait", sem, handle, error);
		*GetErrorAddr() = error;
		return -1;
	};
	if (inline_posix_semaphore_enabled())
	{
		// The record itself is the semaphore: consume its count when positive,
		// otherwise sleep on the console wait key until a release wakes it and
		// re-read the count. The wait loop delivers pending guest exceptions
		// between slices like every other HLE wait.
		InlinePosixSemaphoreRecord record {};
		const uint64_t wait_word = reinterpret_cast<uint64_t>(sem) +
		    offsetof(InlinePosixSemaphoreRecord, has_waiters);
		for (;;)
		{
			int error = read_inline_posix_semaphore_state(sem, &record);
			if (error != 0)
			{
				TraceSemaphore("posix-inline", "wait", sem, nullptr, error);
				*GetErrorAddr() = error;
				return -1;
			}
			bool taken = false;
			int  take_error = 0;
			if (!take_inline_posix_semaphore_token(sem, &taken, &take_error))
			{
				TraceSemaphore("posix-inline", "wait", sem, nullptr, take_error);
				*GetErrorAddr() = take_error;
				return -1;
			}
			if (taken)
			{
				TraceSemaphore("posix-inline", "wait", sem, nullptr, 0);
				return 0;
			}
			(void)LibKernel::Umtx::WaitSemaphoreWaitersWord(wait_word, 0, true);
		}
	}
	const int slot_error = read_semaphore_slot(sem, &handle, "wait");
	if (slot_error != 0) return fail(slot_error);
	// The caller owns the semaphore lifetime throughout the wait. Registry
	// membership rejects invalid values but does not permit concurrent deletion.
	const int result = KernelWaitSema(handle, 1, nullptr);
	if (result != OK) return fail(LibKernel::KernelToPosix(result));
	TraceSemaphore("posix", "wait", sem, handle, 0);
	return 0;
}

int LUMEN_SYSV_ABI sem_trywait(LibKernel::Semaphore::KernelSema* sem)
{
	PRINT_NAME();
	using namespace LibKernel::Semaphore;
	if (inline_posix_semaphore_enabled())
	{
		InlinePosixSemaphoreRecord record {};
		int error = read_inline_posix_semaphore_state(sem, &record);
		bool taken = false;
		int  take_error = 0;
		if (error == 0 && !take_inline_posix_semaphore_token(sem, &taken, &take_error)) error = take_error;
		if (error == 0 && !taken) error = POSIX_EAGAIN;
		TraceSemaphore("posix-inline", "trywait", sem, nullptr, error);
		if (error != 0)
		{
			*GetErrorAddr() = error;
			return -1;
		}
		return 0;
	}
	KernelSema handle = nullptr;
	const auto fail = [&](int error)
	{
		TraceSemaphore("posix", "trywait", sem, handle, error);
		*GetErrorAddr() = error;
		return -1;
	};
	const int slot_error = read_semaphore_slot(sem, &handle, "trywait");
	if (slot_error != 0) return fail(slot_error);
	const int result = KernelPollSema(handle, 1);
	if (result == OK)
	{
		TraceSemaphore("posix", "trywait", sem, handle, 0);
		return 0;
	}
	return fail(result == LibKernel::KERNEL_ERROR_EBUSY ? POSIX_EAGAIN : LibKernel::KernelToPosix(result));
}

int LUMEN_SYSV_ABI sem_timedwait(LibKernel::Semaphore::KernelSema* sem, const LibKernel::KernelTimespec* deadline)
{
	PRINT_NAME();
	using namespace LibKernel::Semaphore;
	static_assert(sizeof(LibKernel::KernelTimespec) == 16 && alignof(LibKernel::KernelTimespec) == 8);
	static_assert(offsetof(LibKernel::KernelTimespec, tv_sec) == 0 && offsetof(LibKernel::KernelTimespec, tv_nsec) == 8);
	KernelSema handle = nullptr;
	const bool inline_route = inline_posix_semaphore_enabled();
	LibKernel::KernelTimespec supplied {};
	bool supplied_valid = false;
	bool carry_known = false;
	bool carry_enabled = false;
	auto fail = [&](int error, const char* phase)
	{
		TraceSemaphore("posix", "timedwait", sem, handle, error);
		trace_sem_timedwait_failure(phase, inline_route, sem, deadline, error, supplied, supplied_valid,
		                           carry_known, carry_enabled);
		*GetErrorAddr() = error;
		return -1;
	};
	if (inline_route)
	{
		InlinePosixSemaphoreRecord record {};
		int error = read_inline_posix_semaphore_state(sem, &record);
		if (error != 0) return fail(error, "initial-slot-state");
		// An immediately available token succeeds without inspecting the deadline,
		// exactly like the handle path: only a busy object reaches the null check
		// and the guest deadline read.
		bool immediate_token = false;
		int  immediate_error = 0;
		if (!take_inline_posix_semaphore_token(sem, &immediate_token, &immediate_error))
			return fail(immediate_error, "initial-token");
		if (immediate_token) return 0;
		if (deadline == nullptr) return fail(POSIX_EINVAL, "deadline-null");
		LibKernel::KernelTimespec absolute {};
#if defined(__aarch64__)
		if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(deadline), &absolute, sizeof(absolute)))
			return fail(POSIX_EFAULT, "deadline-read");
#else
		std::memcpy(&absolute, deadline, sizeof(absolute));
#endif
		supplied = absolute;
		supplied_valid = true;
		carry_enabled = sem_timedwait_carry_enabled();
		carry_known = true;
		bool carried = false;
		if (!normalize_sem_timedwait_deadline(supplied, carry_enabled, &absolute, &carried))
			return fail(POSIX_EINVAL, "deadline-normalization");
		if (carried) publish_sem_timedwait_carry_receipt(supplied, absolute);
		const uint64_t wait_word = reinterpret_cast<uint64_t>(sem) +
		    offsetof(InlinePosixSemaphoreRecord, has_waiters);
		for (;;)
		{
			error = read_inline_posix_semaphore_state(sem, &record);
			if (error != 0) return fail(error, "wait-slot-state");
			bool taken = false;
			int  take_error = 0;
			if (!take_inline_posix_semaphore_token(sem, &taken, &take_error)) return fail(take_error, "wait-token");
			if (taken) return 0;
			// Re-sample REALTIME before each bounded relative wait, exactly like
			// the handle path, so the deadline stays absolute.
			const int interval = inline_posix_semaphore_deadline_interval_us(absolute);
			if (interval < 0) return fail(-interval, "clock-sample");
			if (interval == 0) return fail(POSIX_ETIMEDOUT, "deadline-expired");
			(void)LibKernel::Umtx::WaitSemaphoreWaitersWord(wait_word, static_cast<uint32_t>(interval), false);
		}
	}
	const int slot_error = read_semaphore_slot(sem, &handle, "timedwait");
	if (slot_error != 0) return fail(slot_error, "initial-slot-state");
	const int immediate = KernelPollSema(handle, 1);
	if (immediate == OK)
	{
		TraceSemaphore("posix", "timedwait", sem, handle, 0);
		return 0;
	}
	if (immediate != LibKernel::KERNEL_ERROR_EBUSY) return fail(LibKernel::KernelToPosix(immediate), "initial-poll");
	if (deadline == nullptr) return fail(POSIX_EINVAL, "deadline-null");
	LibKernel::KernelTimespec absolute {};
#if defined(__aarch64__)
	if (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(deadline), &absolute, sizeof(absolute)))
		return fail(POSIX_EFAULT, "deadline-read");
#else
	std::memcpy(&absolute, deadline, sizeof(absolute));
#endif
	supplied = absolute;
	supplied_valid = true;
	carry_enabled = sem_timedwait_carry_enabled();
	carry_known = true;
	bool carried = false;
	if (!normalize_sem_timedwait_deadline(supplied, carry_enabled, &absolute, &carried))
		return fail(POSIX_EINVAL, "deadline-normalization");
	if (carried) publish_sem_timedwait_carry_receipt(supplied, absolute);
	for (;;)
	{
		LibKernel::KernelTimespec now {};
		const int clock_result = LibKernel::KernelClockGettimeHost(0, now);
		if (clock_result != OK) return fail(LibKernel::KernelToPosix(clock_result), "clock-sample");
		const bool expired = absolute.tv_sec < now.tv_sec ||
		    (absolute.tv_sec == now.tv_sec && absolute.tv_nsec <= now.tv_nsec);
		LibKernel::KernelUseconds interval = 0;
		if (!expired)
		{
			// Unsigned subtraction represents even the full signed-seconds span.
			// Cap before multiplication and include the nanosecond borrow.
			const uint64_t seconds = static_cast<uint64_t>(absolute.tv_sec) - static_cast<uint64_t>(now.tv_sec);
			if (seconds >= 2)
			{
				interval = 1000000;
			} else
			{
				const int64_t nanos = static_cast<int64_t>(seconds) * 1000000000 + absolute.tv_nsec - now.tv_nsec;
				const int64_t micros = (nanos + 999) / 1000;
				interval = static_cast<LibKernel::KernelUseconds>(micros > 1000000 ? 1000000 : micros);
			}
		}
		// Re-sample REALTIME after each bounded relative timeout. A successful
		// kernel wait has consumed its token and must never become a timeout.
		const int result = wait_sema_host(handle, 1, &interval);
		if (result == OK)
		{
			TraceSemaphore("posix", "timedwait", sem, handle, 0);
			return 0;
		}
		if (result != LibKernel::KERNEL_ERROR_ETIMEDOUT) return fail(LibKernel::KernelToPosix(result), "kernel-wait");
		if (expired) return fail(POSIX_ETIMEDOUT, "deadline-expired");
	}
}

int LUMEN_SYSV_ABI sem_init(LibKernel::Semaphore::KernelSema* sem, int pshared, unsigned value)
{
	PRINT_NAME();
	using namespace LibKernel::Semaphore;
	static_assert(sizeof(KernelSema) == 8 && sizeof(int) == 4 && sizeof(unsigned) == 4);
	constexpr unsigned max_value = std::numeric_limits<int>::max();
	auto fail = [](int error) { *GetErrorAddr() = error; return -1; };
	if (value > max_value) return fail(POSIX_EINVAL);
	// The session-owned kernel registry supports sharing between guest threads.
	// Sharing an object with another process is outside this backend's scope.
	if (pshared != 0) return fail(POSIX_ENOSYS);
	if (sem == nullptr) return fail(POSIX_EINVAL);
	if (inline_posix_semaphore_enabled())
	{
		const int inline_result = initialize_inline_posix_semaphore(sem, value);
		if (inline_result != 0) return fail(inline_result);
		TraceSemaphore("posix-inline", "init", sem, nullptr, 0, value);
		return 0;
	}
#if defined(__aarch64__)
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(sem), sizeof(*sem), true))
		return fail(POSIX_EFAULT);
#endif
	KernelSema created = nullptr;
	const int result = KernelCreateSema(&created, "posix-semaphore", 1, static_cast<int>(value),
	                                    static_cast<int>(max_value), nullptr);
	if (result != OK) return fail(LibKernel::KernelToPosix(result));
#if defined(__aarch64__)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(sem), &created, sizeof(created)))
	{
		KernelDeleteSema(created);
		return fail(POSIX_EFAULT);
	}
#else
	std::memcpy(sem, &created, sizeof(created));
#endif
	TraceSemaphore("posix", "init", sem, created, 0, value);
	return 0;
}

int LUMEN_SYSV_ABI sem_destroy(LibKernel::Semaphore::KernelSema* sem)
{
	PRINT_NAME();
	using namespace LibKernel::Semaphore;
	if (inline_posix_semaphore_enabled())
	{
		// Destroying an object that still has waiters reports EBUSY, like the
		// console, and the cleared record leaves the address ready for reuse.
		InlinePosixSemaphoreRecord record {};
		int error = read_inline_posix_semaphore_state(sem, &record);
		if (error == 0 && record.has_waiters != 0) error = POSIX_EBUSY;
		if (error == 0)
		{
			const InlinePosixSemaphoreRecord cleared {};
			if (!write_inline_posix_semaphore_record(sem, cleared)) error = POSIX_EFAULT;
		}
		TraceSemaphore("posix-inline", "destroy", sem, nullptr, error);
		if (error != 0)
		{
			*GetErrorAddr() = error;
			return -1;
		}
		return 0;
	}
	KernelSema handle = nullptr;
	const auto fail = [&](int error)
	{
		TraceSemaphore("posix", "destroy", sem, handle, error);
		*GetErrorAddr() = error;
		return -1;
	};
	const int slot_error = read_semaphore_slot(sem, &handle, "destroy");
	if (slot_error != 0) return fail(slot_error);
	const int result = KernelDeleteSema(handle);
	if (result != OK) return fail(LibKernel::KernelToPosix(result));
	KernelSema cleared = nullptr;
#if defined(__aarch64__)
	if (!Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(sem), &cleared, sizeof(cleared)))
		return fail(POSIX_EFAULT);
#else
	std::memcpy(sem, &cleared, sizeof(cleared));
#endif
	TraceSemaphore("posix", "destroy", sem, handle, 0);
	return 0;
}

} // namespace Lumen::Libs::Posix

#endif
