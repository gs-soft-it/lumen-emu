#include "kernel/eventFlag.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "kernel/memory.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/guestHostWait.h"
#if defined(LUMEN_PS5_FEXCORE)
#include "loader/guestExecution.h"
#include "loader/guestMutationScope.h"
#endif

#include <limits>
#include <memory>
#include <unordered_map>

namespace Libs::LibKernel {
void KernelDispatchPendingSignalForCurrentThread();
}

namespace Libs::LibKernel::EventFlag {

LIB_NAME("libkernel", "libkernel");

class KernelEventFlagPrivate {
public:
	enum class Result { Ok, AlreadyWaiting, TimedOut, Canceled, Deleted };

	enum class ClearMode { None, All, Bits };

	enum class WaitMode { And, Or };

	KernelEventFlagPrivate(const std::string& name, bool single, bool /*fifo*/, uint64_t bits)
	    : m_name(name), m_single_thread(single), m_bits(bits) {}
	virtual ~KernelEventFlagPrivate();

	KYTY_CLASS_NO_COPY(KernelEventFlagPrivate);

	void   Close();
	bool   Set(uint64_t bits);
	bool   Clear(uint64_t bits);
	bool   Cancel(uint64_t bits, int* num_waiting_threads);
	Result Wait(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result,
	            uint32_t* ptr_micros, bool* result_written = nullptr,
	            bool* timeout_written = nullptr);

	Result Poll(uint64_t bits, WaitMode wait_mode, ClearMode clear_mode, uint64_t* result) {
		uint32_t micros = 0;
		return Wait(bits, wait_mode, clear_mode, result, &micros);
	}

private:
	enum class Status { Set, Canceled, Deleted };
	struct ActiveWait {
		ActiveWait* next = nullptr;
		bool terminal = false;
		Result result = Result::Ok;
		uint64_t bits = 0;
	};
	void YieldNative();

	Common::Mutex   m_mutex;
	Common::CondVar m_cond_var;
	Status          m_status          = Status::Set;
	int             m_waiting_threads = 0;
	std::string     m_name;
	bool            m_single_thread = false;
	uint64_t        m_bits          = 0;
	ActiveWait*     m_active_waits = nullptr;
};

namespace {

// A public handle is opaque and never reused. Every API pins its native owner
// before releasing an HLE reader, including peers waiting for Cancel to drain.
// Delete revokes new admission without freeing state used by admitted peers.
using EventFlagRef = std::shared_ptr<KernelEventFlagPrivate>;
Common::Mutex g_event_flags_mutex;
std::unordered_map<KernelEventFlag, EventFlagRef> g_event_flags;
uintptr_t g_next_event_flag = 1;

struct NativeUnlockScope {
	explicit NativeUnlockScope(Common::Mutex& mutex): mutex(mutex) { mutex.Unlock(); }
	~NativeUnlockScope() { mutex.Lock(); }
	Common::Mutex& mutex;
};

struct NativeWaiterRegistration {
	explicit NativeWaiterRegistration(int& count): count(count) { ++count; }
	~NativeWaiterRegistration() { --count; }
	int& count;
};

EventFlagRef PinEventFlag(KernelEventFlag ef) {
	Common::LockGuard lock(g_event_flags_mutex);
	const auto entry = g_event_flags.find(ef);
	return entry != g_event_flags.end() ? entry->second : EventFlagRef {};
}

struct EventFlagWaitMode {
	KernelEventFlagPrivate::WaitMode  wait  = KernelEventFlagPrivate::WaitMode::And;
	KernelEventFlagPrivate::ClearMode clear = KernelEventFlagPrivate::ClearMode::None;
};

EventFlagWaitMode DecodeEventFlagWaitMode(uint32_t wait_mode) {
	EventFlagWaitMode mode;

	switch (wait_mode & 0xfu) {
		case 0x01: mode.wait = KernelEventFlagPrivate::WaitMode::And; break;
		case 0x02: mode.wait = KernelEventFlagPrivate::WaitMode::Or; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	switch (wait_mode & 0xf0u) {
		case 0x00: mode.clear = KernelEventFlagPrivate::ClearMode::None; break;
		case 0x10: mode.clear = KernelEventFlagPrivate::ClearMode::All; break;
		case 0x20: mode.clear = KernelEventFlagPrivate::ClearMode::Bits; break;
		default: EXIT("unknown mode: %u\n", wait_mode);
	}

	return mode;
}

} // namespace

KernelEventFlagPrivate::~KernelEventFlagPrivate() {
	Close();
}

void KernelEventFlagPrivate::YieldNative() {
	// The public API retains an owner, and its inputs/output snapshots are
	// native owned. No object mutex may cross barrier readmission.
	NativeUnlockScope unlock(m_mutex);
	{
		Loader::GuestExecution::GuestHostWaitScope wait;
		Common::Thread::SleepMicro(10);
	}
}

void KernelEventFlagPrivate::Close() {
	Common::LockGuard lock(m_mutex);
	if (m_status == Status::Deleted) return;

	while (m_status == Status::Canceled) {
		YieldNative();
	}

	m_status = Status::Deleted;
	for (auto* active = m_active_waits; active != nullptr; active = active->next) {
		if (!active->terminal) {
			active->terminal = true;
			active->result = Result::Deleted;
			active->bits = m_bits;
		}
	}

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0) {
		YieldNative();
	}
}

KernelEventFlagPrivate::Result KernelEventFlagPrivate::Wait(uint64_t bits, WaitMode wait_mode,
                                                            ClearMode clear_mode, uint64_t* result,
                                                            uint32_t* ptr_micros,
                                                            bool* result_written,
                                                            bool* timeout_written) {
	Common::LockGuard lock(m_mutex);
	if (result_written != nullptr) *result_written = false;
	if (timeout_written != nullptr) *timeout_written = false;

	uint32_t micros     = 0;
	bool     infinitely = true;
	if (ptr_micros != nullptr) {
		micros     = *ptr_micros;
		infinitely = false;
	}

	uint32_t      elapsed = 0;
	Common::Timer t;
	t.Start();

	auto update_timeout = [&]() {
		if (ptr_micros != nullptr) {
			*ptr_micros = (elapsed >= micros ? 0 : micros - elapsed);
			if (timeout_written != nullptr) *timeout_written = true;
		}
	};
	auto update_result = [&]() {
		if (result != nullptr) {
			*result = m_bits;
			if (result_written != nullptr) *result_written = true;
		}
	};
	if (m_status == Status::Deleted) return Result::Deleted;

	if (m_single_thread && m_active_waits != nullptr) {
		return Result::AlreadyWaiting;
	}
	ActiveWait operation;
	// Logical admission survives signal callbacks, while the separate native
	// waiter count controls Cancel/Delete drain. Each operation snapshots the
	// first terminal transition; later cancels cannot overwrite its result.
	struct ActiveRegistration {
		ActiveRegistration(ActiveWait*& head, ActiveWait& operation): head(head), operation(operation) {
			operation.next = head;
			head = &operation;
		}
		~ActiveRegistration() {
			auto** current = &head;
			while (*current != nullptr && *current != &operation) current = &(*current)->next;
			if (*current == &operation) *current = operation.next;
		}
		ActiveWait*& head;
		ActiveWait& operation;
	} active(m_active_waits, operation);
	auto terminal_result = [&]() {
		if (result != nullptr) {
			*result = operation.bits;
			if (result_written != nullptr) *result_written = true;
		}
		update_timeout();
		return operation.result;
	};
	if (m_status == Status::Canceled) {
		operation.bits = m_bits;
		operation.result = Result::Canceled;
		return terminal_result();
	}

	while (!((wait_mode == WaitMode::And && (m_bits & bits) == bits) ||
	         (wait_mode == WaitMode::Or && (m_bits & bits) != 0))) {
		if ((elapsed >= micros && !infinitely)) {
			update_result();
			update_timeout();
			return Result::TimedOut;
		}

#if defined(LUMEN_PS5_FEXCORE)
		bool timed_out = false;
#endif
		{
		NativeWaiterRegistration registration(m_waiting_threads);
#if defined(LUMEN_PS5_FEXCORE)
		// The waiter count pins this native object through wake and readmission.
		// Guest outputs/timeout are staged by the public API; neither its local
		// lock nor borrowed guest memory crosses this native-only wait.
		{
			NativeUnlockScope unlock(m_mutex);
			Loader::GuestExecution::GuestHostWaitScope wait;
			Common::LockGuard native_wait_lock(m_mutex);
			// Set/Cancel/Delete can run while the mutex is released. Rechecking
			// both status and bits here prevents a notification being lost.
			if (m_status == Status::Set &&
			    !((wait_mode == WaitMode::And && (m_bits & bits) == bits) ||
			      (wait_mode == WaitMode::Or && (m_bits & bits) != 0))) {
				constexpr uint32_t poll_micros = 10000;
				timed_out = !m_cond_var.WaitFor(&m_mutex, infinitely ? poll_micros : micros - elapsed);
			}
		} // Native lock, then EndWait, then relock the caller's mutex.
#else
		if (infinitely) {
			m_cond_var.Wait(&m_mutex);
		} else {
			m_cond_var.WaitFor(&m_mutex, micros - elapsed);
		}
#endif
		} // Unregister while holding the mutex, including exception unwinding.

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);

		if (operation.terminal) return terminal_result();
		// Consume a satisfied event under this same mutex before any signal
		// callback can clear/set/cancel it. Only nonterminal waits poll signals.
		if ((wait_mode == WaitMode::And && (m_bits & bits) == bits) ||
		    (wait_mode == WaitMode::Or && (m_bits & bits) != 0)) break;
#if defined(LUMEN_PS5_FEXCORE)
		if (infinitely && timed_out) {
			// Baseline CondVar::Wait polls signals only after its 10ms timeout;
			// WaitFor does not. Preserve that behavior after readmission only.
			// A signal callback can exit/throw or Cancel/Delete this same flag.
			// The operation owner keeps it alive, but no native waiter or mutex
			// may be retained across that callback. Relock before unwinding.
			NativeUnlockScope unlock(m_mutex);
			LibKernel::KernelDispatchPendingSignalForCurrentThread();
		}
		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);
		if (operation.terminal) return terminal_result();
#endif
	}

	update_result();
	update_timeout();

	switch (clear_mode) {
		case ClearMode::All: m_bits = 0; break;
		case ClearMode::Bits: m_bits &= ~bits; break;
		case ClearMode::None: break;
	}

	return Result::Ok;
}

bool KernelEventFlagPrivate::Set(uint64_t bits) {
	Common::LockGuard lock(m_mutex);
	while (m_status == Status::Canceled) {
		YieldNative();
	}
	if (m_status == Status::Deleted) return false;

	m_bits |= bits;

	m_cond_var.SignalAll();
	return true;
}

bool KernelEventFlagPrivate::Clear(uint64_t bits) {
	Common::LockGuard lock(m_mutex);
	while (m_status == Status::Canceled) {
		YieldNative();
	}
	if (m_status == Status::Deleted) return false;

	m_bits &= bits;
	return true;
}

bool KernelEventFlagPrivate::Cancel(uint64_t bits, int* num_waiting_threads) {
	Common::LockGuard lock(m_mutex);
	while (m_status == Status::Canceled) {
		YieldNative();
	}
	if (m_status == Status::Deleted) return false;

	if (num_waiting_threads != nullptr) {
		*num_waiting_threads = 0;
		for (auto* active = m_active_waits; active != nullptr; active = active->next)
			++*num_waiting_threads;
	}

	m_status = Status::Canceled;
	m_bits   = bits;
	for (auto* active = m_active_waits; active != nullptr; active = active->next) {
		if (!active->terminal) {
			active->terminal = true;
			active->result = Result::Canceled;
			active->bits = bits;
		}
	}

	m_cond_var.SignalAll();

	while (m_waiting_threads > 0) {
		YieldNative();
	}

	m_status = Status::Set;
	return true;
}

int KYTY_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr,
                                        uint64_t init_pattern, const void* param) {
	PRINT_NAME();

	if (ef == nullptr || name == nullptr) {
		return KERNEL_ERROR_EINVAL;
	}

	if (param != nullptr || (attr & ~0x33u) != 0) {
		return KERNEL_ERROR_EINVAL;
	}

	bool single = true;
	bool fifo   = true;

	switch (attr & 0x0fu) {
		case 0x00:
		case 0x01: fifo = true; break;
		case 0x02: fifo = false; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	switch (attr & 0xf0u) {
		case 0x00:
		case 0x10: single = true; break;
		case 0x20: single = false; break;
		default: return KERNEL_ERROR_EINVAL;
	}

	{
		Common::LockGuard lock(g_event_flags_mutex);
		if (g_next_event_flag == std::numeric_limits<uintptr_t>::max())
			EXIT("event flag handle space exhausted\n");
		const auto handle = reinterpret_cast<KernelEventFlag>(g_next_event_flag++);
		g_event_flags.emplace(handle,
		    std::make_shared<KernelEventFlagPrivate>(std::string(name), single, fifo, init_pattern));
		*ef = handle;
	}

	LOGF("\tEventFlag create: %s\n", name);

	return OK;
}

int KYTY_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag ef) {
	PRINT_NAME();

	EventFlagRef owner;
	{
		Common::LockGuard lock(g_event_flags_mutex);
		const auto entry = g_event_flags.find(ef);
		if (entry == g_event_flags.end()) return KERNEL_ERROR_ESRCH;
		owner = std::move(entry->second);
		g_event_flags.erase(entry);
	}

	owner->Close();

	return OK;
}

int KYTY_SYSV_ABI KernelWaitEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat, KernelUseconds* timeout) {
	PRINT_NAME();

	auto owner = PinEventFlag(ef);
	if (!owner) {
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto mode   = DecodeEventFlagWaitMode(wait_mode);
#if defined(LUMEN_PS5_FEXCORE)
	LumenAndroidFexCoreGuestStateV1 state {};
	const bool guest_output = Loader::GuestExecution::CaptureCurrentGuestState(&state);
	uint64_t native_result = 0;
	KernelUseconds native_timeout = 0;
	if (guest_output) {
		if ((result_pat != nullptr &&
		     !Memory::QueryGuestAccess(reinterpret_cast<uint64_t>(result_pat), sizeof(*result_pat),
		                               Common::VirtualMemory::Mode::Write)) ||
		    (timeout != nullptr &&
		     (!Memory::ReadGuestMemory(nullptr, reinterpret_cast<uint64_t>(timeout),
		                               &native_timeout, sizeof(native_timeout)) ||
		      !Memory::QueryGuestAccess(reinterpret_cast<uint64_t>(timeout), sizeof(*timeout),
		                                Common::VirtualMemory::Mode::Write)))) {
			return KERNEL_ERROR_EFAULT;
		}
	} else if (timeout != nullptr) {
		native_timeout = *timeout;
	}
	bool result_written = false;
	bool timeout_written = false;
	const auto result = owner->Wait(bit_pattern, mode.wait, mode.clear,
	    result_pat != nullptr ? &native_result : nullptr,
	    timeout != nullptr ? &native_timeout : nullptr, &result_written, &timeout_written);
	if (guest_output && (result_written || timeout_written)) {
		Loader::GuestExecution::GuestMutationScope epoch(
		    Loader::GuestExecution::GuestMutationScope::Admission::FreshScalarEntry);
		// Revocation while asleep must fail before either guest output is changed.
		if ((result_written && !Memory::QueryGuestAccess(reinterpret_cast<uint64_t>(result_pat),
		        sizeof(*result_pat), Common::VirtualMemory::Mode::Write)) ||
		    (timeout_written && !Memory::QueryGuestAccess(reinterpret_cast<uint64_t>(timeout),
		        sizeof(*timeout), Common::VirtualMemory::Mode::Write)) ||
		    (result_written && !Memory::WriteGuestMemory(nullptr,
		        reinterpret_cast<uint64_t>(result_pat), &native_result, sizeof(native_result))) ||
		    (timeout_written && !Memory::WriteGuestMemory(nullptr,
		        reinterpret_cast<uint64_t>(timeout), &native_timeout, sizeof(native_timeout)))) {
			return KERNEL_ERROR_EFAULT;
		}
	} else if (!guest_output) {
		if (result_written) *result_pat = native_result;
		if (timeout_written) *timeout = native_timeout;
	}
#else
	auto       result = owner->Wait(bit_pattern, mode.wait, mode.clear, result_pat, timeout);
#endif

	int ret = OK;

	switch (result) {
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut: ret = KERNEL_ERROR_ETIMEDOUT; break;
		case KernelEventFlagPrivate::Result::Canceled: ret = KERNEL_ERROR_ECANCELED; break;
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EACCES; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode,
                                      uint64_t* result_pat) {
	PRINT_NAME();

	auto owner = PinEventFlag(ef);
	if (!owner) {
		return KERNEL_ERROR_ESRCH;
	}

	if (bit_pattern == 0) {
		return KERNEL_ERROR_EINVAL;
	}

	const auto mode   = DecodeEventFlagWaitMode(wait_mode);
	auto       result = owner->Poll(bit_pattern, mode.wait, mode.clear, result_pat);

	int ret = OK;

	switch (result) {
		case KernelEventFlagPrivate::Result::Ok: ret = OK; break;
		case KernelEventFlagPrivate::Result::AlreadyWaiting: ret = KERNEL_ERROR_EPERM; break;
		case KernelEventFlagPrivate::Result::TimedOut:
		case KernelEventFlagPrivate::Result::Canceled:
		case KernelEventFlagPrivate::Result::Deleted: ret = KERNEL_ERROR_EBUSY; break;
	}

	return ret;
}

int KYTY_SYSV_ABI KernelSetEventFlag(KernelEventFlag ef, uint64_t bit_pattern) {
	PRINT_NAME();

	auto owner = PinEventFlag(ef);
	if (!owner) {
		return KERNEL_ERROR_ESRCH;
	}

	return owner->Set(bit_pattern) ? OK : KERNEL_ERROR_ESRCH;
}

int KYTY_SYSV_ABI KernelClearEventFlag(KernelEventFlag ef, uint64_t bit_pattern) {
	PRINT_NAME();

	auto owner = PinEventFlag(ef);
	if (!owner) {
		return KERNEL_ERROR_ESRCH;
	}

	return owner->Clear(bit_pattern) ? OK : KERNEL_ERROR_ESRCH;
}

int KYTY_SYSV_ABI KernelCancelEventFlag(KernelEventFlag ef, uint64_t set_pattern,
                                        int* num_wait_threads) {
	PRINT_NAME();

	auto owner = PinEventFlag(ef);
	if (!owner) {
		return KERNEL_ERROR_ESRCH;
	}

#if defined(LUMEN_PS5_FEXCORE)
	LumenAndroidFexCoreGuestStateV1 state {};
	const bool guest_output = Loader::GuestExecution::CaptureCurrentGuestState(&state);
	if (guest_output && num_wait_threads != nullptr &&
	    !Memory::QueryGuestAccess(reinterpret_cast<uint64_t>(num_wait_threads),
	        sizeof(*num_wait_threads), Common::VirtualMemory::Mode::Write)) {
		return KERNEL_ERROR_EFAULT;
	}
	int native_waiters = 0;
	if (!owner->Cancel(set_pattern, num_wait_threads != nullptr ? &native_waiters : nullptr))
		return KERNEL_ERROR_ESRCH;
	if (num_wait_threads != nullptr) {
		if (guest_output) {
			Loader::GuestExecution::GuestMutationScope epoch(
			    Loader::GuestExecution::GuestMutationScope::Admission::FreshScalarEntry);
			if (!Memory::QueryGuestAccess(reinterpret_cast<uint64_t>(num_wait_threads),
			        sizeof(*num_wait_threads), Common::VirtualMemory::Mode::Write) ||
			    !Memory::WriteGuestMemory(nullptr, reinterpret_cast<uint64_t>(num_wait_threads),
			                              &native_waiters, sizeof(native_waiters)))
				return KERNEL_ERROR_EFAULT;
		} else {
			*num_wait_threads = native_waiters;
		}
	}
#else
	if (!owner->Cancel(set_pattern, num_wait_threads)) return KERNEL_ERROR_ESRCH;
#endif

	return OK;
}

} // namespace Libs::LibKernel::EventFlag
