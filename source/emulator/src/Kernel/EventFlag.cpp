#include "Emulator/Kernel/EventFlag.h"

#include <mutex>
#include <unordered_set>
#include <vector>

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Timer.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME))
#include "Emulator/Loader/RuntimeLinker.h"
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::EventFlag {

LIB_NAME("libkernel", "libkernel");

namespace {

void SleepForObjectProgress(Core::Mutex* mutex)
{
	EXIT_IF(mutex == nullptr);
	mutex->Unlock();
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
	{
		// A cancel/delete HLE must not retain its guest-memory reader while
		// waiting for another waiter to be readmitted after an Exclusive
		// mutation.  The object remains alive and no guest pointer is touched.
		Loader::ScopedGuestHostWait host_wait;
		Core::Thread::SleepMicro(10);
	}
#else
	Core::Thread::SleepMicro(10);
#endif
	mutex->Lock();
}

} // namespace

class KernelEventFlagPrivate
{
public:
	enum class Result
	{
		Ok,
		AlreadyWaiting,
		TimedOut,
		Canceled,
		Deleted
	};

	enum class ClearMode
	{
		None,
		All,
		Bits
	};

	enum class WaitMode
	{
		And,
		Or
	};

	KernelEventFlagPrivate(const String& name, bool single, bool /*fifo*/, uint64_t bits)
	    : m_name(name), m_single_thread(single), m_bits(bits) {};
	virtual ~KernelEventFlagPrivate();

	LUMEN_CLASS_NO_COPY(KernelEventFlagPrivate);

	void   Set(uint64_t bits);
	void   Clear(uint64_t bits);
	void   Cancel(uint64_t bits, int* num_waiting_threads);
	Result Wait(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result, uint32_t* ptr_micros);

	Result Poll(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result)
	{
		uint32_t micros = 0;
		return Wait(bits, wait_mode, clear_mode, result, &micros);
	}

#if defined(LUMEN_KERNEL_EVENT_FLAG_TEST_HOOK)
	bool HasWaitersForTest()
	{
		Core::LockGuard lock(m_mutex);
		return m_waiting_threads > 0;
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
	Status        m_status          = Status::Set;
	int           m_waiting_threads = 0;
	String        m_name;
	bool          m_single_thread = false;
	uint64_t      m_bits          = 0;
};

namespace {

std::mutex& event_flag_registry_mutex()
{
	static std::mutex mutex;
	return mutex;
}

std::unordered_set<KernelEventFlag>& event_flag_registry()
{
	static std::unordered_set<KernelEventFlag> registry;
	return registry;
}

void register_event_flag(KernelEventFlag event_flag)
{
	std::lock_guard lock(event_flag_registry_mutex());
	event_flag_registry().insert(event_flag);
}

void unregister_event_flag(KernelEventFlag event_flag)
{
	std::lock_guard lock(event_flag_registry_mutex());
	event_flag_registry().erase(event_flag);
}

std::vector<KernelEventFlag> remove_all_event_flags()
{
	std::vector<KernelEventFlag> event_flags;
	{
		std::lock_guard lock(event_flag_registry_mutex());
		event_flags.reserve(event_flag_registry().size());
		for (auto* event_flag: event_flag_registry())
		{
			event_flags.push_back(event_flag);
		}
		event_flag_registry().clear();
	}
	return event_flags;
}

} // namespace

KernelEventFlagPrivate::~KernelEventFlagPrivate()
{
	Core::LockGuard lock(m_mutex);

	while (m_status != Status::Set)
	{
		SleepForObjectProgress(&m_mutex);
	}

	m_status = Status::Deleted;

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0)
	{
		SleepForObjectProgress(&m_mutex);
	}
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Wait(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result,
                                                            uint32_t* ptr_micros)
{
	Core::LockGuard lock(m_mutex);

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

	if (m_single_thread && m_waiting_threads > 0)
	{
		return Result::AlreadyWaiting;
	}

	while (!((wait_mode == WaitMode::And && (m_bits & bits) == bits) || (wait_mode == WaitMode::Or && (m_bits & bits) != 0)))
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (Loader::GuestExecutionStopRequested())
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			return Result::Canceled;
		}
#endif
		if ((elapsed >= micros && !infinitely))
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			*ptr_micros = 0;
			return Result::TimedOut;
		}

		m_waiting_threads++;
		if (infinitely)
		{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
			{
				Loader::ScopedGuestHostWait host_wait;
				m_cond_var.WaitFor(&m_mutex, 10000);
				// WaitFor returns with the event mutex reacquired.  A pending
				// guest-memory mutation may need another HLE which is blocked on
				// this mutex to leave its reader, so restore our reader without
				// retaining the object lock.  Cancel/Delete yield their own reader
				// while waiting, so this waiter remains a lifetime pin until it has
				// completed readmission and reacquired the object mutex.
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

		m_waiting_threads--;

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);

		if (m_status == Status::Canceled)
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Canceled;
		}

		if (m_status == Status::Deleted)
		{
			if (result != nullptr)
			{
				*result = m_bits;
			}
			if (ptr_micros != nullptr)
			{
				*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			}
			return Result::Deleted;
		}
	}

	if (result != nullptr)
	{
		*result = m_bits;
	}

	if (clear_mode == ClearMode::All)
	{
		m_bits = 0;
	} else if (clear_mode == ClearMode::Bits)
	{
		m_bits &= ~bits;
	}

	if (ptr_micros != nullptr)
	{
		*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
	}

	return Result::Ok;
}

void KernelEventFlagPrivate::Set(uint64_t bits)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set)
	{
		SleepForObjectProgress(&m_mutex);
	}

	m_bits |= bits;

	m_cond_var.SignalAll();
}

void KernelEventFlagPrivate::Clear(uint64_t bits)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set)
	{
		SleepForObjectProgress(&m_mutex);
	}

	m_bits &= bits;
}

void KernelEventFlagPrivate::Cancel(uint64_t bits, int* num_waiting_threads)
{
	Core::LockGuard lock(m_mutex);

	EXIT_NOT_IMPLEMENTED(m_status == Status::Deleted);

	while (m_status != Status::Set)
	{
		SleepForObjectProgress(&m_mutex);
	}

	if (num_waiting_threads != nullptr)
	{
		*num_waiting_threads = m_waiting_threads;
	}

	m_status = Status::Canceled;
	m_bits   = bits;

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0)
	{
		SleepForObjectProgress(&m_mutex);
	}

	m_status = Status::Set;
}

int LUMEN_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr, uint64_t init_pattern, const void* param)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(param != nullptr);

	if (ef == nullptr || name == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	bool single = true;
	bool fifo   = true;

	switch (attr)
	{
		case 0x01:
		case 0x10:
		case 0x11:
			single = true;
			fifo   = true;
			break;
		case 0x20:
		case 0x21:
			single = false;
			fifo   = true;
			break;
		case 0x22:
			single = false;
			fifo   = false;
			break;
		default: EXIT("unknown attr: %u\n", attr);
	}

	*ef = new KernelEventFlagPrivate(String::FromUtf8(name), single, fifo, init_pattern);
	register_event_flag(*ef);

	printf("\tEventFlag create: %s\n", name);

	return OK;
}

int LUMEN_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag ef)
{
	PRINT_NAME();

	if (ef == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	unregister_event_flag(ef);
	delete ef;

	return OK;
}

int LUMEN_SYSV_ABI KernelWaitEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode, uint64_t* result_pat,
                                      KernelUseconds* timeout)
{
	PRINT_NAME();

	if (ef == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelEventFlagPrivate::WaitMode  wait  = KernelEventFlagPrivate::WaitMode::And;
	KernelEventFlagPrivate::ClearMode clear = KernelEventFlagPrivate::ClearMode::None;

	switch (wait_mode & 0xfu)
	{
		case 0x01: wait = KernelEventFlagPrivate::WaitMode::And; break;
		case 0x02: wait = KernelEventFlagPrivate::WaitMode::Or; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	switch (wait_mode & 0xf0u)
	{
		case 0x00: clear = KernelEventFlagPrivate::ClearMode::None; break;
		case 0x10: clear = KernelEventFlagPrivate::ClearMode::All; break;
		case 0x20: clear = KernelEventFlagPrivate::ClearMode::Bits; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
	uint64_t host_result = 0;
	KernelUseconds host_timeout = 0;
	if (result_pat != nullptr && !Loader::ValidateGuestMemory(
	        reinterpret_cast<uint64_t>(result_pat), sizeof(*result_pat), true))
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (timeout != nullptr &&
	    (!Loader::ReadGuestMemory(reinterpret_cast<uint64_t>(timeout), &host_timeout, sizeof(host_timeout)) ||
	     !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(timeout), sizeof(*timeout), true)))
	{
		return KERNEL_ERROR_EFAULT;
	}
	auto result = ef->Wait(bit_pattern, wait, clear,
	                       result_pat != nullptr ? &host_result : nullptr,
	                       timeout != nullptr ? &host_timeout : nullptr);
	// A single-wait flag rejects a second waiter before touching either
	// output. Preserve that contract when the blocking call uses host-owned
	// snapshots while its guest-memory reader is yielded.
	const bool publish_outputs = result != KernelEventFlagPrivate::Result::AlreadyWaiting;
	if (publish_outputs &&
	    ((result_pat != nullptr && !Loader::ValidateGuestMemory(
	         reinterpret_cast<uint64_t>(result_pat), sizeof(*result_pat), true)) ||
	    (timeout != nullptr && !Loader::ValidateGuestMemory(
	         reinterpret_cast<uint64_t>(timeout), sizeof(*timeout), true))))
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (publish_outputs &&
	    ((result_pat != nullptr && !Loader::WriteGuestMemory(
	         reinterpret_cast<uint64_t>(result_pat), &host_result, sizeof(host_result))) ||
	    (timeout != nullptr && !Loader::WriteGuestMemory(
	         reinterpret_cast<uint64_t>(timeout), &host_timeout, sizeof(host_timeout)))))
	{
		return KERNEL_ERROR_EFAULT;
	}
#else
	auto result = ef->Wait(bit_pattern, wait, clear, result_pat, timeout);
#endif

	int ret = OK;

	switch (result)
	{
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelEventFlagPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}

	return ret;
}

int LUMEN_SYSV_ABI KernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode, uint64_t* result_pat)
{
	PRINT_NAME();

	if (ef == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	KernelEventFlagPrivate::WaitMode  wait  = KernelEventFlagPrivate::WaitMode::And;
	KernelEventFlagPrivate::ClearMode clear = KernelEventFlagPrivate::ClearMode::None;

	switch (wait_mode & 0xfu)
	{
		case 0x01: wait = KernelEventFlagPrivate::WaitMode::And; break;
		case 0x02: wait = KernelEventFlagPrivate::WaitMode::Or; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	switch (wait_mode & 0xf0u)
	{
		case 0x00: clear = KernelEventFlagPrivate::ClearMode::None; break;
		case 0x10: clear = KernelEventFlagPrivate::ClearMode::All; break;
		case 0x20: clear = KernelEventFlagPrivate::ClearMode::Bits; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	auto result = ef->Poll(bit_pattern, wait, clear, result_pat);

	int ret = OK;

	switch (result)
	{
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut:
		case KernelEventFlagPrivate::Result::Canceled:
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EBUSY; break;
	}

	return ret;
}

int LUMEN_SYSV_ABI KernelSetEventFlag(KernelEventFlag ef, uint64_t bit_pattern)
{
	PRINT_NAME();

	if (ef == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	ef->Set(bit_pattern);

	return OK;
}

int LUMEN_SYSV_ABI KernelClearEventFlag(KernelEventFlag ef, uint64_t bit_pattern)
{
	PRINT_NAME();

	if (ef == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	ef->Clear(bit_pattern);

	return OK;
}

int LUMEN_SYSV_ABI KernelCancelEventFlag(KernelEventFlag ef, uint64_t set_pattern, int* num_wait_threads)
{
	PRINT_NAME();

	if (ef == nullptr)
	{
		return KERNEL_ERROR_ESRCH;
	}

	ef->Cancel(set_pattern, num_wait_threads);

	return OK;
}

bool ResetForNextSession()
{
	for (auto* event_flag: remove_all_event_flags())
	{
		delete event_flag;
	}

	std::lock_guard lock(event_flag_registry_mutex());
	return event_flag_registry().empty();
}

#if defined(LUMEN_KERNEL_EVENT_FLAG_TEST_HOOK)
bool EventFlagHasWaitersForTest(KernelEventFlag ef)
{
	return ef != nullptr && ef->HasWaitersForTest();
}

bool EventFlagRegistryEmptyForTest()
{
	std::lock_guard lock(event_flag_registry_mutex());
	return event_flag_registry().empty();
}
#endif

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool EventFlagSessionResetContractTest()
{
	for (int cycle = 0; cycle < 2; cycle++)
	{
		KernelEventFlag event_flag = nullptr;
		if (KernelCreateEventFlag(&event_flag, "lumen_session_reset_test", 0x10, 0, nullptr) != OK || event_flag == nullptr ||
		    !ResetForNextSession())
		{
			return false;
		}
	}
	return ResetForNextSession();
}
#endif

} // namespace Lumen::Libs::LibKernel::EventFlag

#endif // LUMEN_EMU_ENABLED
