#include "Emulator/Kernel/EventQueue.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/LinkList.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Timer.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || defined(LUMEN_FEXCORE_RUNTIME)
#include "Emulator/Loader/RuntimeLinker.h"
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::EventQueue {

LIB_NAME("libkernel", "libkernel");

namespace {

template <typename T>
T ReadGuestEventField(const KernelEvent* event, uint64_t offset)
{
	T value {};
	if (event == nullptr) return value;
	const auto address = reinterpret_cast<uint64_t>(event);
	if (address > UINT64_MAX - offset) return value;
#if defined(LUMEN_FEXCORE_RUNTIME)
	(void)Loader::ReadGuestMemory(address + offset, &value, sizeof(value));
#else
	std::memcpy(&value, reinterpret_cast<const uint8_t*>(event) + offset, sizeof(value));
#endif
	return value;
}

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
void TraceDeliveredEvents(uint64_t destination, const KernelEvent* host_events, int count)
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_KERNEL_EVENT_DELIVERY_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!enabled || count <= 0) return;
	static std::atomic<uint32_t> emitted {0};
	constexpr uint32_t limit = 64;
	for (int index = 0; index < count; ++index)
	{
		uint32_t sequence = emitted.load(std::memory_order_relaxed);
		while (sequence <= limit && !emitted.compare_exchange_weak(sequence, sequence + 1, std::memory_order_relaxed)) {}
		if (sequence > limit) return;
		if (sequence == limit)
		{
			std::fprintf(stderr, "[KERNEL_EVENT_DELIVERY] capped=1 limit=%u\n", limit);
			return;
		}
		const auto& event = host_events[index];
		std::fprintf(stderr,
		             "[KERNEL_EVENT_DELIVERY] sequence=%u thread=%d record=0x%016" PRIx64
		             " index=%d ident=0x%016" PRIx64 " filter=%d flags=0x%04x fflags=0x%08" PRIx32
		             " data=0x%016" PRIx64 " udata_nonnull=%u\n",
		             sequence + 1, Core::Thread::GetThreadIdUnique(), destination + static_cast<uint64_t>(index) * sizeof(KernelEvent),
		             index, static_cast<uint64_t>(event.ident), static_cast<int>(event.filter), static_cast<unsigned>(event.flags),
		             event.fflags, static_cast<uint64_t>(event.data), event.udata != nullptr ? 1u : 0u);
	}
}
#endif

void SleepForObjectProgress(Core::Mutex* mutex)
{
	EXIT_IF(mutex == nullptr);
	mutex->Unlock();
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
	{
		// Destruction owns the queue but must not retain its guest-memory
		// reader while a sleeping waiter is readmitted after a mutation.
		Loader::ScopedGuestHostWait host_wait;
		Core::Thread::SleepMicro(10);
	}
#else
	Core::Thread::SleepMicro(10);
#endif
	mutex->Lock();
}

} // namespace

class KernelEqueuePrivate
{
public:
	KernelEqueuePrivate() = default;
	virtual ~KernelEqueuePrivate();

	LUMEN_CLASS_NO_COPY(KernelEqueuePrivate);

	[[nodiscard]] const String& GetName() const { return m_name; }
	void                        SetName(const String& m_name) { this->m_name = m_name; }

	void AddEvent(const KernelEqueueEvent& event, bool replace_existing = true,
	              const KernelEqueueIdentity* identity = nullptr, registered_event_func_t registered = nullptr);
	bool TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data);
	bool DeleteEvent(uintptr_t ident, int16_t filter);

	int GetTriggeredEvents(KernelEvent* ev, int num);
	int WaitForEvents(KernelEvent* ev, int num, uint32_t micros);
	int SnapshotWaitCapacity(int requested);

#if defined(LUMEN_KERNEL_USER_EVENT_TEST_HOOK)
	bool HasWaitersForTest()
	{
		Core::LockGuard lock(m_mutex);
		return m_waiting_threads != 0;
	}
#endif

private:
	Core::List<KernelEqueueEvent> m_events;
	Core::Mutex                   m_mutex;
	Core::CondVar                 m_cond_var;
	String                        m_name;
	uint32_t                      m_waiting_threads = 0;
	bool                          m_deleted         = false;
};

namespace {

std::mutex& equeue_registry_mutex()
{
	static std::mutex mutex;
	return mutex;
}

std::unordered_map<KernelEqueue, uint64_t>& equeue_registry()
{
	static std::unordered_map<KernelEqueue, uint64_t> registry;
	return registry;
}

void register_equeue(KernelEqueue eq)
{
	std::lock_guard lock(equeue_registry_mutex());
	static uint64_t next_generation = 1;
	if (next_generation == 0) EXIT("Kernel event queue generation exhausted");
	equeue_registry().emplace(eq, next_generation++);
}

void unregister_equeue(KernelEqueue eq)
{
	std::lock_guard lock(equeue_registry_mutex());
	equeue_registry().erase(eq);
}

std::vector<KernelEqueue> remove_all_equeues()
{
	std::vector<KernelEqueue> queues;
	{
		std::lock_guard lock(equeue_registry_mutex());
		queues.reserve(equeue_registry().size());
		for (const auto& [queue, generation]: equeue_registry())
		{
			(void)generation;
			queues.push_back(queue);
		}
		equeue_registry().clear();
	}
	return queues;
}

} // namespace

bool GetEqueueIdentity(KernelEqueue eq, KernelEqueueIdentity* identity)
{
	if (identity == nullptr) return false;
	*identity = {};
	std::lock_guard lock(equeue_registry_mutex());
	const auto found = equeue_registry().find(eq);
	if (found == equeue_registry().end()) return false;
	*identity = {eq, found->second};
	return true;
}

static bool IsRegisteredIdentity(const KernelEqueueIdentity& identity)
{
	const auto found = equeue_registry().find(identity.queue);
	return identity.generation != 0 && found != equeue_registry().end() && found->second == identity.generation;
}

int AddEventIfRegistered(const KernelEqueueIdentity& identity, const KernelEqueueEvent& event, registered_event_func_t registered)
{
	std::lock_guard lock(equeue_registry_mutex());
	if (!IsRegisteredIdentity(identity)) return KERNEL_ERROR_EBADF;
	identity.queue->AddEvent(event, true, &identity, registered);
	return OK;
}

int TriggerEventIfRegistered(const KernelEqueueIdentity& identity, uintptr_t ident, int16_t filter, void* trigger_data)
{
	std::lock_guard lock(equeue_registry_mutex());
	if (!IsRegisteredIdentity(identity)) return KERNEL_ERROR_EBADF;
	return identity.queue->TriggerEvent(ident, filter, trigger_data) ? OK : KERNEL_ERROR_ENOENT;
}

int DeleteEventIfRegistered(const KernelEqueueIdentity& identity, uintptr_t ident, int16_t filter)
{
	std::lock_guard lock(equeue_registry_mutex());
	if (!IsRegisteredIdentity(identity)) return KERNEL_ERROR_EBADF;
	return identity.queue->DeleteEvent(ident, filter) ? OK : KERNEL_ERROR_ENOENT;
}

KernelEqueuePrivate::~KernelEqueuePrivate()
{
	Core::LockGuard lock(m_mutex);
	m_deleted = true;
	m_cond_var.SignalAll();
	while (m_waiting_threads != 0)
	{
		SleepForObjectProgress(&m_mutex);
	}

	FOR_LIST(index, m_events)
	{
		auto& event = m_events[index];

		if (event.filter.delete_event_func != nullptr)
		{
			event.filter.delete_event_func(this, &event);
		}
	}
}

int KernelEqueuePrivate::GetTriggeredEvents(KernelEvent* ev, int num)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);

	int ret = 0;

	FOR_LIST(index, m_events)
	{
		auto& event = m_events[index];

		if (event.triggered)
		{
			ev[ret++] = event.event;

			if (event.filter.reset_func != nullptr)
			{
				event.filter.reset_func(&event);
			}

			if (ret >= num)
			{
				break;
			}
		}
	}

	return ret;
}

int KernelEqueuePrivate::WaitForEvents(KernelEvent* ev, int num, uint32_t micros)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(num < 1);

	uint32_t    elapsed = 0;
	Core::Timer t;
	t.Start();

	for (;;)
	{
		if (m_deleted)
		{
			return 0;
		}
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
		if (Loader::GuestExecutionStopRequested())
		{
			return 0;
		}
#endif
		int ret = GetTriggeredEvents(ev, num);

		if (ret > 0 || (elapsed >= micros && micros != 0))
		{
			return ret;
		}

		m_waiting_threads++;
		if (micros == 0)
		{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
			{
				Loader::ScopedGuestHostWait host_wait;
				m_cond_var.WaitFor(&m_mutex, 10000);
				// EndQuiescentHostWait may wait for a mapping mutation.  Do not
				// retain the queue mutex while restoring the guest-memory reader:
				// another HLE reader can need this mutex before it can drain.
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
		EXIT_IF(m_waiting_threads == 0);
		m_waiting_threads--;
		m_cond_var.SignalAll();

		elapsed = static_cast<uint32_t>(t.GetTimeS() * 1000000.0);
	}

	return 0;
}

int KernelEqueuePrivate::SnapshotWaitCapacity(int requested)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(requested < 1);

	// A wait may legally return fewer events than the guest-provided capacity. Keep
	// the Android/FEX host-side staging buffer bounded by the queue's current
	// population instead of trusting an arbitrarily large guest count. Reserve one
	// slot for an empty queue because another thread may register an event while the
	// caller is asleep.
	const uint64_t registered = m_events.Size();
	const uint64_t capacity   = std::min<uint64_t>(static_cast<uint64_t>(requested),
	                                               std::max<uint64_t>(registered, 1));
	return static_cast<int>(capacity);
}

void KernelEqueuePrivate::AddEvent(const KernelEqueueEvent& event, bool replace_existing,
                                   const KernelEqueueIdentity* identity, registered_event_func_t registered)
{
	Core::LockGuard lock(m_mutex);

	if (auto index = m_events.Find(event.event.ident, event.event.filter,
	                               [](auto e, auto ident, auto filter) { return e.event.ident == ident && e.event.filter == filter; });
	    m_events.IndexValid(index))
	{
		if (!replace_existing)
		{
			return;
		}
		m_events[index] = event;
	} else
	{
		m_events.Add(event);
	}

	if (registered != nullptr) registered(*identity, event);

	if (event.triggered)
	{
		m_cond_var.Signal();
	}
}

bool KernelEqueuePrivate::TriggerEvent(uintptr_t ident, int16_t filter, void* trigger_data)
{
	Core::LockGuard lock(m_mutex);

	if (auto index = m_events.Find(ident, filter,
	                               [](auto e, auto ident, auto filter) { return e.event.ident == ident && e.event.filter == filter; });
	    m_events.IndexValid(index))
	{
		auto& event = m_events[index];

		if (event.filter.trigger_func != nullptr)
		{
			event.filter.trigger_func(&event, trigger_data);
		} else
		{
			event.triggered = true;
		}

		m_cond_var.Signal();

		return true;
	}

	return false;
}

bool KernelEqueuePrivate::DeleteEvent(uintptr_t ident, int16_t filter)
{
	Core::LockGuard lock(m_mutex);

	if (auto index = m_events.Find(ident, filter,
	                               [](auto e, auto ident, auto filter) { return e.event.ident == ident && e.event.filter == filter; });
	    m_events.IndexValid(index))
	{
		auto& event = m_events[index];

		if (event.filter.delete_event_func != nullptr)
		{
			event.filter.delete_event_func(this, &event);
		}

		m_events.Remove(index);

		return true;
	}

	return false;
}

int LUMEN_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name)
{
	PRINT_NAME();

	if (eq == nullptr || name == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

	*eq = new KernelEqueuePrivate;

	(*eq)->SetName(String::FromUtf8(name));
	register_equeue(*eq);

	printf("\tEqueue create: %s\n", name);

	return OK;
}

int LUMEN_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event)
{
	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	eq->AddEvent(event);

	return OK;
}

int LUMEN_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter, void* trigger_data)
{
	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (!eq->TriggerEvent(ident, filter, trigger_data))
	{
		return KERNEL_ERROR_ENOENT;
	}

	return OK;
}

int LUMEN_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter)
{
	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (!eq->DeleteEvent(ident, filter))
	{
		return KERNEL_ERROR_ENOENT;
	}

	return OK;
}

namespace {

constexpr uint16_t USER_EVENT_EDGE_MODE = 0x0020;

uintptr_t UserEventId(int id)
{
	// User identifiers use their 32-bit identity in the pointer-sized key.
	return static_cast<uint32_t>(id);
}

void TriggerUserEvent(KernelEqueueEvent* event, void* udata)
{
	event->event.udata = udata;
	event->triggered   = true;
}

void ResetEdgeUserEvent(KernelEqueueEvent* event)
{
	event->triggered = false;
}

int AddUserEventRegistration(KernelEqueue eq, int id, uint16_t mode, reset_func_t reset_func)
{
	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	KernelEqueueEvent event {};
	event.event.ident          = UserEventId(id);
	event.event.filter         = KERNEL_EVFILT_USER;
	event.event.flags          = mode;
	event.filter.trigger_func  = TriggerUserEvent;
	event.filter.reset_func    = reset_func;
	// User registrations retain the first mode selected for a key. Duplicate
	// normal or Edge adds preserve pending state and the latest opaque payload.
	eq->AddEvent(event, false);
	return OK;
}

} // namespace

int LUMEN_SYSV_ABI KernelAddUserEvent(KernelEqueue eq, int id)
{
	PRINT_NAME();
	// Normal user events remain pending after delivery. Re-registering the
	// same key preserves its pending state and latest opaque user data.
	return AddUserEventRegistration(eq, id, 0, nullptr);
}

int LUMEN_SYSV_ABI KernelAddUserEventEdge(KernelEqueue eq, int id)
{
	PRINT_NAME();
	return AddUserEventRegistration(eq, id, USER_EVENT_EDGE_MODE, ResetEdgeUserEvent);
}

int LUMEN_SYSV_ABI KernelTriggerUserEvent(KernelEqueue eq, int id, void* udata)
{
	return KernelTriggerEvent(eq, UserEventId(id), KERNEL_EVFILT_USER, udata);
}

int LUMEN_SYSV_ABI KernelDeleteUserEvent(KernelEqueue eq, int id)
{
	PRINT_NAME();
	return KernelDeleteEvent(eq, UserEventId(id), KERNEL_EVFILT_USER);
}

int LUMEN_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq)
{
	PRINT_NAME();

	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	printf("\tEqueue delete: %s\n", eq->GetName().C_Str());

	unregister_equeue(eq);
	delete eq;

	return OK;
}

#if defined(LUMEN_KERNEL_USER_EVENT_TEST_HOOK)
bool EventQueueHasWaitersForTest(KernelEqueue eq)
{
	return eq != nullptr && eq->HasWaitersForTest();
}
#endif

int LUMEN_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out, const KernelUseconds* timo)
{
	// PRINT_NAME();

	if (eq == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (ev == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (num < 1)
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_NOT_IMPLEMENTED(out == nullptr);

	// printf("\tEqueue wait: %s\n", eq->GetName().C_Str());

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
	const int    wait_num    = eq->SnapshotWaitCapacity(num);
	const size_t event_count = static_cast<size_t>(wait_num);
	if (event_count > SIZE_MAX / sizeof(KernelEvent))
	{
		return KERNEL_ERROR_EINVAL;
	}
	const size_t event_bytes = event_count * sizeof(KernelEvent);
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(ev), event_bytes, true) ||
	    !Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(out), sizeof(*out), true))
	{
		return KERNEL_ERROR_EFAULT;
	}
	KernelUseconds host_timo = 0;
	if (timo != nullptr && !Loader::ReadGuestMemory(
	        reinterpret_cast<uint64_t>(timo), &host_timo, sizeof(host_timo)))
	{
		return KERNEL_ERROR_EFAULT;
	}
	std::unique_ptr<KernelEvent[]> host_events(new (std::nothrow) KernelEvent[event_count] {});
	if (host_events == nullptr)
	{
		return KERNEL_ERROR_ENOMEM;
	}
	KernelEvent* wait_events = host_events.get();
	int host_out = 0;
	int* wait_out = &host_out;
	const KernelUseconds* wait_timo = timo != nullptr ? &host_timo : nullptr;
#else
	KernelEvent* wait_events = ev;
	int* wait_out = out;
	const KernelUseconds* wait_timo = timo;
	const int wait_num = num;
#endif

	if (wait_timo == nullptr)
	{
		*wait_out = eq->WaitForEvents(wait_events, wait_num, 0);
	}

	if (wait_timo != nullptr)
	{
		if (*wait_timo == 0)
		{
			*wait_out = eq->GetTriggeredEvents(wait_events, wait_num);
		} else
		{
			*wait_out = eq->WaitForEvents(wait_events, wait_num, *wait_timo);
		}
	}

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX) && defined(LUMEN_FEXCORE_RUNTIME)
	const size_t written_event_bytes = static_cast<size_t>(host_out) * sizeof(KernelEvent);
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(out), sizeof(*out), true) ||
	    (written_event_bytes != 0 && !Loader::ValidateGuestMemory(
	         reinterpret_cast<uint64_t>(ev), written_event_bytes, true)))
	{
		return KERNEL_ERROR_EFAULT;
	}
	if ((written_event_bytes != 0 && !Loader::WriteGuestMemory(
	         reinterpret_cast<uint64_t>(ev), host_events.get(), written_event_bytes)) ||
	    !Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(out), &host_out, sizeof(host_out)))
	{
		return KERNEL_ERROR_EFAULT;
	}
	// The queue lock and guest publication leases have ended. Format only the
	// retained host snapshot, never the caller's possibly changed guest bytes.
	TraceDeliveredEvents(reinterpret_cast<uint64_t>(ev), host_events.get(), host_out);
#endif

	if (*wait_out == 0)
	{
		// printf("\ttimedout\n");
		return KERNEL_ERROR_ETIMEDOUT;
	}

	// printf("\treceived %u events\n", *out);

	return OK;
}

bool ResetForNextSession()
{
	for (auto* queue: remove_all_equeues())
	{
		delete queue;
	}

	std::lock_guard lock(equeue_registry_mutex());
	return equeue_registry().empty();
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool EventQueueSessionResetContractTest()
{
	for (int cycle = 0; cycle < 2; cycle++)
	{
		KernelEqueue queue = nullptr;
		if (KernelCreateEqueue(&queue, "lumen_session_reset_test") != OK || queue == nullptr || !ResetForNextSession())
		{
			return false;
		}
	}
	return ResetForNextSession();
}
#endif

intptr_t LUMEN_SYSV_ABI KernelGetEventData(const KernelEvent* ev)
{
	PRINT_NAME();

	return ReadGuestEventField<decltype(KernelEvent::data)>(ev, offsetof(KernelEvent, data));
}

intptr_t LUMEN_SYSV_ABI KernelGetEventFflags(const KernelEvent* ev)
{
	PRINT_NAME();

	return ReadGuestEventField<decltype(KernelEvent::fflags)>(ev, offsetof(KernelEvent, fflags));
}

int LUMEN_SYSV_ABI KernelGetEventFilter(const KernelEvent* ev)
{
	PRINT_NAME();

	return ReadGuestEventField<decltype(KernelEvent::filter)>(ev, offsetof(KernelEvent, filter));
}

uintptr_t LUMEN_SYSV_ABI KernelGetEventId(const KernelEvent* ev)
{
	PRINT_NAME();

	return ReadGuestEventField<decltype(KernelEvent::ident)>(ev, offsetof(KernelEvent, ident));
}

void* LUMEN_SYSV_ABI KernelGetEventUserData(const KernelEvent* ev)
{
	PRINT_NAME();

	return ReadGuestEventField<decltype(KernelEvent::udata)>(ev, offsetof(KernelEvent, udata));
}

int LUMEN_SYSV_ABI KernelGetEventError(const KernelEvent* /*ev*/)
{
	PRINT_NAME();

	LUMEN_NOT_IMPLEMENTED;

	return 0;
}

} // namespace Lumen::Libs::LibKernel::EventQueue

#endif // LUMEN_EMU_ENABLED
