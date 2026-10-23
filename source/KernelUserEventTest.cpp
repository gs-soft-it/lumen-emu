#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/EventQueue.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

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
#include <thread>
#include <type_traits>

namespace {

namespace Kernel = Lumen::Libs::LibKernel;
namespace QueueApi = Kernel::EventQueue;

using AddUserEventEdgeFunction = int (LUMEN_SYSV_ABI *)(QueueApi::KernelEqueue, int);
static_assert(std::is_same_v<decltype(&QueueApi::KernelAddUserEventEdge), AddUserEventEdgeFunction>);

constexpr uint16_t USER_EVENT_EDGE_MODE = 0x0020;

// Bound a lost wakeup or mutex regression even outside CTest.
class Watchdog final
{
public:
	Watchdog(): m_thread([this] {
		std::unique_lock<std::mutex> lock(m_mutex);
		if (!m_changed.wait_for(lock, std::chrono::seconds(10), [this] { return m_done; }))
		{
			std::fprintf(stderr, "KernelUserEventTest: FAIL: queue operation timed out\n");
			std::fflush(stderr);
			std::_Exit(2);
		}
	}) {}

	~Watchdog()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_done = true;
		}
		m_changed.notify_one();
		m_thread.join();
	}

	Watchdog(const Watchdog&)            = delete;
	Watchdog& operator=(const Watchdog&) = delete;

private:
	std::mutex              m_mutex;
	std::condition_variable m_changed;
	bool                    m_done = false;
	std::thread             m_thread;
};

bool Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "KernelUserEventTest: FAIL: %s\n", message);
	}
	return condition;
}

bool Expect(int actual, int expected, const char* operation)
{
	if (actual != expected)
	{
		std::fprintf(stderr, "KernelUserEventTest: FAIL: %s returned 0x%08x, expected 0x%08x\n", operation,
		             static_cast<unsigned>(actual), static_cast<unsigned>(expected));
		return false;
	}
	return true;
}

// Declare callback state before its QueueOwner so cleanup cannot outlive it.
class QueueOwner final
{
public:
	~QueueOwner()
	{
		if (m_queue != nullptr)
		{
			QueueApi::KernelDeleteEqueue(m_queue);
		}
	}

	bool Create(const char* name)
	{
		return Expect(QueueApi::KernelCreateEqueue(&m_queue, name), 0, "create queue") &&
		       Check(m_queue != nullptr, "successful queue creation returned null");
	}

	QueueApi::KernelEqueue Get() const { return m_queue; }
	void Release() { m_queue = nullptr; }

	bool Delete()
	{
		const int result = QueueApi::KernelDeleteEqueue(m_queue);
		if (result == 0)
		{
			Release();
		}
		return Expect(result, 0, "delete populated queue");
	}

private:
	QueueApi::KernelEqueue m_queue = nullptr;
};

struct CallbackState
{
	QueueApi::KernelEqueue queue           = nullptr;
	uintptr_t             ident           = 0;
	int16_t               filter          = 0;
	unsigned              triggers        = 0;
	unsigned              resets          = 0;
	unsigned              deletions       = 0;
	bool                  identity_ok     = true;
	bool                  deleted_pending = false;
};

CallbackState& StateFor(QueueApi::KernelEqueueEvent* event)
{
	if (event == nullptr || event->filter.data == nullptr)
	{
		std::fprintf(stderr, "KernelUserEventTest: FAIL: synthetic callback lost its state\n");
		std::_Exit(3);
	}
	auto& state = *static_cast<CallbackState*>(event->filter.data);
	state.identity_ok = state.identity_ok && event->event.ident == state.ident && event->event.filter == state.filter &&
	                    event->event.udata == &state && event->event.flags == 0x24;
	return state;
}

// These callbacks define an original synthetic producer, not a user-event ABI.
void SyntheticTrigger(QueueApi::KernelEqueueEvent* event, void* payload)
{
	auto& state = StateFor(event);
	++state.triggers;
	event->triggered = true;
	++event->event.fflags;
	event->event.data = *static_cast<const intptr_t*>(payload);
}

void SyntheticReset(QueueApi::KernelEqueueEvent* event)
{
	++StateFor(event).resets;
	event->triggered    = false;
	event->event.fflags = 0;
	event->event.data   = 0;
}

void SyntheticDelete(QueueApi::KernelEqueue queue, QueueApi::KernelEqueueEvent* event)
{
	auto& state = StateFor(event);
	++state.deletions;
	state.identity_ok     = state.identity_ok && queue == state.queue;
	state.deleted_pending = event->triggered;
}

bool Register(QueueApi::KernelEqueue queue, CallbackState* state, uintptr_t ident, int16_t filter)
{
	state->queue  = queue;
	state->ident  = ident;
	state->filter = filter;
	QueueApi::KernelEqueueEvent event {};
	event.event.ident              = ident;
	event.event.filter             = filter;
	event.event.flags              = 0x24;
	event.event.udata              = state;
	event.filter.data              = state;
	event.filter.trigger_func      = SyntheticTrigger;
	event.filter.reset_func        = SyntheticReset;
	event.filter.delete_event_func = SyntheticDelete;
	return Expect(QueueApi::KernelAddEvent(queue, event), 0, "register synthetic event");
}

bool Trigger(CallbackState* state, intptr_t payload)
{
	return Expect(QueueApi::KernelTriggerEvent(state->queue, state->ident, state->filter, &payload), 0, "trigger synthetic event");
}

struct CapturedOutput
{
	struct Buffer
	{
		uint64_t              before[2];
		QueueApi::KernelEvent events[3];
		uint64_t              after[2];
	} buffer;
	struct Count
	{
		uint32_t before = 0x13572468;
		int      value  = -1;
		uint32_t after  = 0x24681357;
	} count;
	std::array<unsigned char, sizeof(Buffer)> original {};

	CapturedOutput()
	{
		std::memset(&buffer, 0xa5, sizeof(buffer));
		std::memcpy(original.data(), &buffer, sizeof(buffer));
	}

	bool IntactOutside(size_t written) const
	{
		const size_t start = offsetof(Buffer, events);
		const size_t end   = start + written * sizeof(QueueApi::KernelEvent);
		const auto*  bytes = reinterpret_cast<const unsigned char*>(&buffer);
		return Check(std::memcmp(bytes, original.data(), start) == 0 &&
		                 std::memcmp(bytes + end, original.data() + end, sizeof(buffer) - end) == 0 &&
		                 count.before == 0x13572468 && count.after == 0x24681357,
		             "queue retrieval wrote outside its output capacity");
	}
};

bool Matches(const QueueApi::KernelEvent& event, const CallbackState& state, intptr_t payload, uint32_t triggers)
{
	return Check(event.ident == state.ident && event.filter == state.filter && event.flags == 0x24 && event.udata == &state &&
	                 event.data == payload && event.fflags == triggers,
	             "retrieved event lost its key, user data, flags, or synthetic payload");
}

bool EmptyPoll(QueueApi::KernelEqueue queue)
{
	CapturedOutput output;
	const Kernel::KernelUseconds timeout = 0;
	return Expect(QueueApi::KernelWaitEqueue(queue, output.buffer.events, 1, &output.count.value, &timeout),
	              Kernel::KERNEL_ERROR_ETIMEDOUT, "empty poll") &&
	       Check(output.count.value == 0, "empty poll reported events") && output.IntactOutside(0);
}

bool TestKeysAndBoundedRetrieval()
{
	std::array<CallbackState, 4> states {};
	QueueOwner queue;
	QueueOwner other;
	if (!queue.Create("synthetic_keys") || !other.Create("synthetic_other_queue") ||
	    !Register(queue.Get(), &states[0], 101, QueueApi::KERNEL_EVFILT_GRAPHICS) ||
	    !Register(queue.Get(), &states[1], 101, QueueApi::KERNEL_EVFILT_VIDEO_OUT) ||
	    !Register(queue.Get(), &states[2], 202, QueueApi::KERNEL_EVFILT_GRAPHICS) ||
	    !Register(other.Get(), &states[3], 101, QueueApi::KERNEL_EVFILT_GRAPHICS) || !EmptyPoll(queue.Get()) ||
	    !Trigger(&states[0], 10) || !Trigger(&states[0], 11) || !Trigger(&states[1], 22) || !Trigger(&states[2], 33))
	{
		return false;
	}

	const std::array<intptr_t, 3> payloads {11, 22, 33};
	std::array<bool, 3>          seen {};
	const Kernel::KernelUseconds timeout = 0;
	for (size_t received = 0; received < seen.size(); ++received)
	{
		CapturedOutput output;
		if (!Expect(QueueApi::KernelWaitEqueue(queue.Get(), output.buffer.events, 1, &output.count.value, &timeout), 0,
		            "bounded retrieval") ||
		    !Check(output.count.value == 1, "bounded retrieval did not return exactly one event") || !output.IntactOutside(1))
		{
			return false;
		}
		const auto& event = output.buffer.events[0];
		size_t index = 0;
		while (index < seen.size() && (states[index].ident != event.ident || states[index].filter != event.filter))
		{
			++index;
		}
		if (!Check(index < seen.size(), "retrieval returned an unregistered key") ||
		    !Check(!seen[index], "retrieval repeated a consumed event") ||
		    !Matches(event, states[index], payloads[index], index == 0 ? 2 : 1) ||
		    !Check(states[index].resets == 1, "retrieval did not reset its event exactly once"))
		{
			return false;
		}
		seen[index] = true;
	}

	if (!EmptyPoll(queue.Get()) || !EmptyPoll(other.Get()) || !Trigger(&states[3], 44))
	{
		return false;
	}
	CapturedOutput output;
	if (!Expect(QueueApi::KernelWaitEqueue(other.Get(), output.buffer.events, 1, &output.count.value, &timeout), 0,
	            "same key in another queue") ||
	    !Check(output.count.value == 1, "other queue returned the wrong count") || !output.IntactOutside(1) ||
	    !Matches(output.buffer.events[0], states[3], 44, 1) || !EmptyPoll(queue.Get()) || !queue.Delete() || !other.Delete())
	{
		return false;
	}
	for (const auto& state: states)
	{
		if (!Check(state.identity_ok && state.resets == 1 && state.deletions == 1,
		           "key-separated callback lifecycle was not preserved"))
		{
			return false;
		}
	}
	return true;
}

template <typename TriggerFunction>
bool WaitAndTrigger(QueueApi::KernelEqueue queue, bool finite, CapturedOutput* output, TriggerFunction trigger)
{
	const Kernel::KernelUseconds timeout = 2000000;
	std::mutex begin_mutex;
	std::condition_variable begin_changed;
	bool begun = false;
	std::atomic<bool> finished {false};
	int result = -1;
	std::thread waiter([&] {
		{
			std::lock_guard<std::mutex> lock(begin_mutex);
			begun = true;
		}
		begin_changed.notify_one();
		result = QueueApi::KernelWaitEqueue(queue, output->buffer.events, 1, &output->count.value, finite ? &timeout : nullptr);
		finished.store(true, std::memory_order_release);
	});
	{
		std::unique_lock<std::mutex> lock(begin_mutex);
		begin_changed.wait(lock, [&] { return begun; });
	}
	// Give the announced waiter time to enter the queue wait. The watchdog
	// bounds a missing notification; this is not a scheduler-order assertion.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const bool returned_early = finished.load(std::memory_order_acquire);
	const bool triggered = trigger();
	waiter.join();
	return triggered && Check(!returned_early, "wait returned before an event was triggered") &&
	       Expect(result, 0, finite ? "finite wait wakeup" : "blocking wait wakeup") &&
	       Check(output->count.value == 1, "woken wait returned the wrong count") && output->IntactOutside(1);
}

bool CheckWake(CallbackState* state, bool finite)
{
	CapturedOutput output;
	return WaitAndTrigger(state->queue, finite, &output, [&] { return Trigger(state, finite ? 55 : 66); }) &&
	       Matches(output.buffer.events[0], *state, finite ? 55 : 66, 1) && EmptyPoll(state->queue);
}

bool TestGenericReplacement()
{
	CallbackState original;
	CallbackState replacement;
	QueueOwner queue;
	if (!queue.Create("synthetic_replacement") ||
	    !Register(queue.Get(), &original, 606, QueueApi::KERNEL_EVFILT_GRAPHICS) || !Trigger(&original, 707) ||
	    !Register(queue.Get(), &replacement, 606, QueueApi::KERNEL_EVFILT_GRAPHICS) || !EmptyPoll(queue.Get()) ||
	    !Check(original.triggers == 1 && original.resets == 0 && original.deletions == 0,
	           "generic replacement consumed or deleted the old pending registration") || !Trigger(&replacement, 808))
	{
		return false;
	}
	CapturedOutput output;
	const Kernel::KernelUseconds timeout = 0;
	if (!Expect(QueueApi::KernelWaitEqueue(queue.Get(), output.buffer.events, 3, &output.count.value, &timeout), 0,
	            "retrieve replacement registration") ||
	    !Check(output.count.value == 1, "generic replacement retained duplicate registrations") || !output.IntactOutside(1) ||
	    !Matches(output.buffer.events[0], replacement, 808, 1) || !EmptyPoll(queue.Get()) || !queue.Delete())
	{
		return false;
	}
	return Check(original.identity_ok && original.triggers == 1 && original.resets == 0 && original.deletions == 0 &&
	                 replacement.identity_ok && replacement.triggers == 1 && replacement.resets == 1 && replacement.deletions == 1,
	             "generic replacement did not transfer callback ownership to the new registration");
}

bool TestWaitModes()
{
	CallbackState state;
	QueueOwner queue;
	if (!queue.Create("synthetic_wait_modes") || !Register(queue.Get(), &state, 303, QueueApi::KERNEL_EVFILT_GRAPHICS) ||
	    !EmptyPoll(queue.Get()))
	{
		return false;
	}
	CapturedOutput output;
	const Kernel::KernelUseconds timeout = 20000;
	const auto start = std::chrono::steady_clock::now();
	const int result = QueueApi::KernelWaitEqueue(queue.Get(), output.buffer.events, 1, &output.count.value, &timeout);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	if (!Expect(result, Kernel::KERNEL_ERROR_ETIMEDOUT, "finite empty wait") ||
	    !Check(elapsed >= std::chrono::milliseconds(10), "finite empty wait behaved like an immediate poll") ||
	    !Check(output.count.value == 0, "finite empty wait reported an event") || !output.IntactOutside(0) ||
	    !CheckWake(&state, true) || !CheckWake(&state, false) || !queue.Delete())
	{
		return false;
	}
	return Check(state.identity_ok && state.triggers == 2 && state.resets == 2 && state.deletions == 1,
	             "wait modes did not preserve callback lifecycle");
}

bool TestDeleteWithWaiter()
{
	QueueApi::KernelEqueue queue = nullptr;
	if (!Expect(QueueApi::KernelCreateEqueue(&queue, "synthetic_wait_delete"), 0, "create delete-wait queue") ||
	    !Check(queue != nullptr, "delete-wait queue creation returned null"))
	{
		return false;
	}

	CapturedOutput output;
	std::atomic_bool finished {false};
	int wait_result = -1;
	std::thread waiter([&] {
		wait_result = QueueApi::KernelWaitEqueue(queue, output.buffer.events, 1, &output.count.value, nullptr);
		finished.store(true, std::memory_order_release);
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!finished.load(std::memory_order_acquire) && !QueueApi::EventQueueHasWaitersForTest(queue) &&
	       std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	if (finished.load(std::memory_order_acquire) || !QueueApi::EventQueueHasWaitersForTest(queue))
	{
		if (!finished.load(std::memory_order_acquire))
		{
			(void)QueueApi::KernelAddUserEvent(queue, 1);
			(void)QueueApi::KernelTriggerUserEvent(queue, 1, nullptr);
		}
		waiter.join();
		QueueApi::KernelDeleteEqueue(queue);
		return Check(false, "queue waiter never reached the protected wait state");
	}

	const int delete_result = QueueApi::KernelDeleteEqueue(queue);
	waiter.join();
	return Expect(delete_result, 0, "delete queue with waiter") &&
	       Expect(wait_result, Kernel::KERNEL_ERROR_ETIMEDOUT, "wait released by queue deletion") &&
	       Check(output.count.value == 0 && output.IntactOutside(0),
	             "queue deletion changed an empty wait output");
}

bool TestPopulatedDeletion()
{
	std::array<CallbackState, 3> states {};
	QueueOwner queue;
	if (!queue.Create("synthetic_delete"))
	{
		return false;
	}
	for (size_t index = 0; index < states.size(); ++index)
	{
		if (!Register(queue.Get(), &states[index], 400 + index, QueueApi::KERNEL_EVFILT_GRAPHICS))
		{
			return false;
		}
	}
	intptr_t removed_payload = 99;
	if (!Trigger(&states[0], 77) || !Trigger(&states[2], 88) ||
	    !Expect(QueueApi::KernelDeleteEvent(queue.Get(), states[0].ident, states[0].filter), 0, "delete pending event") ||
	    !Expect(QueueApi::KernelDeleteEvent(queue.Get(), states[0].ident, states[0].filter), Kernel::KERNEL_ERROR_ENOENT,
	            "delete removed event") ||
	    !Expect(QueueApi::KernelTriggerEvent(queue.Get(), states[0].ident, states[0].filter, &removed_payload), Kernel::KERNEL_ERROR_ENOENT,
	            "trigger removed event") || !queue.Delete())
	{
		return false;
	}
	for (size_t index = 0; index < states.size(); ++index)
	{
		const auto& state = states[index];
		if (!Check(state.identity_ok && state.deletions == 1 && state.resets == 0 && state.deleted_pending == (index != 1),
		           "populated deletion lost or repeated a callback"))
		{
			return false;
		}
	}
	return true;
}

bool TestPopulatedSessionReset()
{
	for (int cycle = 0; cycle < 2; ++cycle)
	{
		std::array<CallbackState, 3> states {};
		QueueOwner first;
		QueueOwner second;
		if (!first.Create("synthetic_reset_first") || !second.Create("synthetic_reset_second") ||
		    !Register(first.Get(), &states[0], 501, QueueApi::KERNEL_EVFILT_GRAPHICS) ||
		    !Register(first.Get(), &states[1], 502, QueueApi::KERNEL_EVFILT_GRAPHICS) ||
		    !Register(second.Get(), &states[2], 501, QueueApi::KERNEL_EVFILT_GRAPHICS) ||
		    !Trigger(&states[0], 99) || !Trigger(&states[2], 100))
		{
			return false;
		}
		// There are no live producer or waiter threads. Session reset owns the
		// registered queues from this point, while callback state remains alive.
		first.Release();
		second.Release();
		if (!Check(QueueApi::ResetForNextSession(), "populated session reset failed") ||
		    !Check(QueueApi::ResetForNextSession(), "empty repeated session reset failed"))
		{
			return false;
		}
		for (size_t index = 0; index < states.size(); ++index)
		{
			const auto& state = states[index];
			if (!Check(state.identity_ok && state.deletions == 1 && state.resets == 0 && state.deleted_pending == (index != 1),
			           "session reset did not delete each populated registration exactly once"))
			{
				return false;
			}
		}
	}
	return true;
}

bool UserPoll(QueueApi::KernelEqueue queue, CapturedOutput* output, int expected_count)
{
	const Kernel::KernelUseconds timeout = 0;
	return Expect(QueueApi::KernelWaitEqueue(queue, output->buffer.events, 3, &output->count.value, &timeout), 0, "user event poll") &&
	       Check(output->count.value == expected_count, "user poll returned duplicate or missing registrations") &&
	       output->IntactOutside(static_cast<size_t>(expected_count));
}

bool MatchesUser(const QueueApi::KernelEvent& event, uintptr_t expected_id, void* opaque, uint16_t expected_mode = 0)
{
	// Zero fields and negative-ID widening are explicit Lumen choices recorded
	// separately from the reviewed functional facts in the interface artifact.
	return Check(event.ident == expected_id && event.filter == QueueApi::KERNEL_EVFILT_USER && event.flags == expected_mode &&
	                 event.fflags == 0 && event.data == 0 && event.udata == opaque,
	             "user event did not preserve its local ID, mode, zero fields, and latest opaque value");
}

bool TestUserOperations()
{
	QueueOwner queue;
	constexpr int id = 17;
	void* const first  = reinterpret_cast<void*>(uintptr_t {1});
	void* const latest = reinterpret_cast<void*>(uintptr_t {0xfedcba9876543210ULL});
	if (!Expect(QueueApi::KernelAddUserEvent(nullptr, id), Kernel::KERNEL_ERROR_EBADF, "null user add") ||
	    !Expect(QueueApi::KernelTriggerUserEvent(nullptr, id, first), Kernel::KERNEL_ERROR_EBADF, "null user trigger") ||
	    !Expect(QueueApi::KernelDeleteUserEvent(nullptr, id), Kernel::KERNEL_ERROR_EBADF, "null user delete") ||
	    !queue.Create("user_operations") ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), id, first), Kernel::KERNEL_ERROR_ENOENT, "missing user trigger") ||
	    !Expect(QueueApi::KernelDeleteUserEvent(queue.Get(), id), Kernel::KERNEL_ERROR_ENOENT, "missing user delete") ||
	    !Expect(QueueApi::KernelAddUserEvent(queue.Get(), id), 0, "user add") || !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelAddUserEvent(queue.Get(), id), 0, "duplicate inactive user add") || !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), id, first), 0, "first opaque user trigger") ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), id, latest), 0, "replacement opaque user trigger") ||
	    !Expect(QueueApi::KernelAddUserEvent(queue.Get(), id), 0, "duplicate pending user add"))
	{
		return false;
	}
	for (int delivery = 0; delivery < 2; ++delivery)
	{
		CapturedOutput output;
		if (!UserPoll(queue.Get(), &output, 1) || !MatchesUser(output.buffer.events[0], 17, latest))
		{
			return false;
		}
	}
	if (!Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), id, nullptr), 0, "null opaque user trigger"))
	{
		return false;
	}
	CapturedOutput cleared;
	if (!UserPoll(queue.Get(), &cleared, 1) || !MatchesUser(cleared.buffer.events[0], 17, nullptr) ||
	    !Expect(QueueApi::KernelDeleteUserEvent(queue.Get(), id), 0, "delete pending user event") || !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), id, latest), Kernel::KERNEL_ERROR_ENOENT, "trigger deleted user event") ||
	    !Expect(QueueApi::KernelDeleteUserEvent(queue.Get(), id), Kernel::KERNEL_ERROR_ENOENT, "repeat user delete") ||
	    !Expect(QueueApi::KernelAddUserEvent(queue.Get(), id), 0, "re-add user event") || !EmptyPoll(queue.Get()))
	{
		return false;
	}

	// The trigger values above are deliberately not readable addresses. The
	// wake path also checks that a valid payload's pointee is left untouched.
	std::array<unsigned char, 4> payload {0x13, 0x57, 0x9b, 0xdf};
	const auto original = payload;
	CapturedOutput awakened;
	if (!WaitAndTrigger(queue.Get(), false, &awakened, [&] {
		    return Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), id, payload.data()), 0, "wake user waiter");
		}) ||
	    !MatchesUser(awakened.buffer.events[0], 17, payload.data()) ||
	    !Check(payload == original, "user trigger modified the opaque payload pointee"))
	{
		return false;
	}
	CapturedOutput persistent;
	return UserPoll(queue.Get(), &persistent, 1) && MatchesUser(persistent.buffer.events[0], 17, payload.data());
}

bool TestEdgeUserOperations()
{
	QueueOwner queue;
	constexpr int edge_id   = 18;
	constexpr int normal_id = 19;
	void* const first       = reinterpret_cast<void*>(uintptr_t {0x1234});
	void* const latest      = reinterpret_cast<void*>(uintptr_t {0x5678});
	if (!Expect(QueueApi::KernelAddUserEventEdge(nullptr, edge_id), Kernel::KERNEL_ERROR_EBADF, "null Edge user add") ||
	    !queue.Create("edge_user_operations") ||
	    !Expect(QueueApi::KernelAddUserEventEdge(queue.Get(), edge_id), 0, "Edge user add") || !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelAddUserEventEdge(queue.Get(), edge_id), 0, "duplicate inactive Edge user add") ||
	    !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), edge_id, first), 0, "first Edge user trigger") ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), edge_id, latest), 0, "replacement Edge user trigger") ||
	    !Expect(QueueApi::KernelAddUserEventEdge(queue.Get(), edge_id), 0, "duplicate pending Edge user add") ||
	    !Expect(QueueApi::KernelAddUserEvent(queue.Get(), edge_id), 0, "normal duplicate of Edge user event"))
	{
		return false;
	}

	CapturedOutput delivered;
	if (!UserPoll(queue.Get(), &delivered, 1) ||
	    !MatchesUser(delivered.buffer.events[0], edge_id, latest, USER_EVENT_EDGE_MODE) || !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelAddUserEventEdge(queue.Get(), edge_id), 0, "duplicate consumed Edge user add") ||
	    !Expect(QueueApi::KernelAddUserEvent(queue.Get(), edge_id), 0, "normal duplicate of consumed Edge user event") ||
	    !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), edge_id, first), 0, "retrigger consumed Edge user event"))
	{
		return false;
	}

	CapturedOutput retriggered;
	if (!UserPoll(queue.Get(), &retriggered, 1) ||
	    !MatchesUser(retriggered.buffer.events[0], edge_id, first, USER_EVENT_EDGE_MODE) || !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelDeleteUserEvent(queue.Get(), edge_id), 0, "delete consumed Edge user event") ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), edge_id, latest), Kernel::KERNEL_ERROR_ENOENT,
	            "trigger deleted Edge user event") ||
	    !Expect(QueueApi::KernelDeleteUserEvent(queue.Get(), edge_id), Kernel::KERNEL_ERROR_ENOENT,
	            "repeat Edge user delete") ||
	    !Expect(QueueApi::KernelAddUserEventEdge(queue.Get(), edge_id), 0, "re-add Edge user event") || !EmptyPoll(queue.Get()))
	{
		return false;
	}

	CapturedOutput awakened;
	if (!WaitAndTrigger(queue.Get(), false, &awakened, [&] {
		    return Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), edge_id, latest), 0, "wake Edge user waiter");
	    }) ||
	    !MatchesUser(awakened.buffer.events[0], edge_id, latest, USER_EVENT_EDGE_MODE) || !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelDeleteUserEvent(queue.Get(), edge_id), 0, "delete awakened Edge user event") ||
	    !Expect(QueueApi::KernelAddUserEvent(queue.Get(), normal_id), 0, "normal user add before Edge duplicate") ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), normal_id, latest), 0, "normal user trigger before Edge duplicate") ||
	    !Expect(QueueApi::KernelAddUserEventEdge(queue.Get(), normal_id), 0, "Edge duplicate of normal user event"))
	{
		return false;
	}

	for (int delivery = 0; delivery < 2; ++delivery)
	{
		CapturedOutput persistent;
		if (!UserPoll(queue.Get(), &persistent, 1) || !MatchesUser(persistent.buffer.events[0], normal_id, latest))
		{
			return false;
		}
	}
	return Expect(QueueApi::KernelDeleteUserEvent(queue.Get(), normal_id), 0, "delete persistent normal user event") &&
	       EmptyPoll(queue.Get()) && queue.Delete();
}

bool TestUserIdentifiersAndFilterIsolation()
{
	CallbackState synthetic;
	QueueOwner queue;
	const std::array<int, 3> ids {0, std::numeric_limits<int>::min(), -1};
	const std::array<uintptr_t, 3> delivered_ids {0, 0x80000000ULL, 0xffffffffULL};
	std::array<unsigned char, 3> opaque {0x12, 0x34, 0x56};
	if (!queue.Create("user_identifiers") || !Register(queue.Get(), &synthetic, 0, QueueApi::KERNEL_EVFILT_GRAPHICS) ||
	    !Expect(QueueApi::KernelAddUserEvent(queue.Get(), 0), 0, "user add beside another filter") || !Trigger(&synthetic, 71))
	{
		return false;
	}
	CapturedOutput other_filter;
	if (!UserPoll(queue.Get(), &other_filter, 1) || !Matches(other_filter.buffer.events[0], synthetic, 71, 1) ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), 0, opaque.data()), 0, "isolated user trigger") ||
	    !Expect(QueueApi::KernelDeleteUserEvent(queue.Get(), 0), 0, "isolated user delete") ||
	    !Trigger(&synthetic, 72))
	{
		return false;
	}
	CapturedOutput surviving_filter;
	if (!UserPoll(queue.Get(), &surviving_filter, 1) || !Matches(surviving_filter.buffer.events[0], synthetic, 72, 1))
	{
		return false;
	}
	for (size_t index = 0; index < ids.size(); ++index)
	{
		if (!Expect(QueueApi::KernelAddUserEvent(queue.Get(), ids[index]), 0, "add signed user identifier") ||
		    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), ids[index], &opaque[index]), 0, "trigger signed user identifier"))
		{
			return false;
		}
	}
	CapturedOutput output;
	if (!UserPoll(queue.Get(), &output, 3))
	{
		return false;
	}
	std::array<bool, 3> seen {};
	for (const auto& event: output.buffer.events)
	{
		size_t index = 0;
		while (index < ids.size() && event.ident != delivered_ids[index])
		{
			++index;
		}
		if (!Check(index < ids.size(), "signed user ID had an unexpected upper-bit extension") ||
		    !Check(!seen[index], "distinct signed user IDs collided") || !MatchesUser(event, delivered_ids[index], &opaque[index]))
		{
			return false;
		}
		seen[index] = true;
	}
	return queue.Delete() && Check(synthetic.identity_ok && synthetic.triggers == 2 && synthetic.resets == 2 && synthetic.deletions == 1,
	                               "user operations disturbed another filter's callback lifecycle");
}

bool TestUserExports()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "libkernel selection failed"))
	{
		return false;
	}
	struct Export { const char* nid; uint64_t address; };
	const std::array<Export, 8> exports {{
	    {"4R6-OvI2cEA", reinterpret_cast<uint64_t>(&QueueApi::KernelAddUserEvent)},
	    {"WDszmSbWuDk", reinterpret_cast<uint64_t>(&QueueApi::KernelAddUserEventEdge)},
	    {"F6e0kwo4cnk", reinterpret_cast<uint64_t>(&QueueApi::KernelTriggerUserEvent)},
	    {"LJDwdSNTnDg", reinterpret_cast<uint64_t>(&QueueApi::KernelDeleteUserEvent)},
	    {"D0OdFMjp46I", reinterpret_cast<uint64_t>(&QueueApi::KernelCreateEqueue)},
	    {"jpFjmgAC5AE", reinterpret_cast<uint64_t>(&QueueApi::KernelDeleteEqueue)},
	    {"fzyMKs9kim0", reinterpret_cast<uint64_t>(&QueueApi::KernelWaitEqueue)},
	    {"vz+pg2zdopI", reinterpret_cast<uint64_t>(&QueueApi::KernelGetEventUserData)},
	}};
	Lumen::Loader::SymbolResolve query {};
	query.library = "libkernel";
	query.library_version = 1;
	query.module = "libkernel";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Lumen::Loader::SymbolType::Func;
	for (const auto& expected: exports)
	{
		query.name = expected.nid;
		const auto* symbol = symbols.Find(query);
		if (!Check(symbol != nullptr && symbol->vaddr == expected.address, "user or existing queue export has the wrong binding"))
		{
			return false;
		}
	}

	query.name = "WDszmSbWuDk";
	const auto* edge = symbols.Find(query);
	const auto* scalar = edge == nullptr ? nullptr : symbols.HostCallScalarMetadataFor(edge->vaddr);
	using ScalarType = Lumen::Loader::HostCallScalarType;
	if (!Check(scalar != nullptr && scalar->arity == 2 && scalar->arguments[0] == ScalarType::Gp &&
	               scalar->arguments[1] == ScalarType::Gp && scalar->result == ScalarType::Gp && scalar->invoker != nullptr,
	           "Edge user add scalar ABI metadata differs"))
	{
		return false;
	}

	constexpr int scalar_id = -37;
	std::array<uint64_t, 2> arguments {0, static_cast<uint32_t>(scalar_id)};
	const uint64_t expected_badf = static_cast<uint32_t>(Kernel::KERNEL_ERROR_EBADF);
	if (!Check(scalar->invoker(arguments.data()) == expected_badf, "Edge scalar invoker did not preserve EBADF"))
	{
		return false;
	}

	QueueOwner queue;
	if (!queue.Create("edge_scalar_invoker"))
	{
		return false;
	}
	arguments[0] = reinterpret_cast<uint64_t>(queue.Get());
	if (!Check(scalar->invoker(arguments.data()) == 0, "Edge scalar invoker did not add the registration") ||
	    !EmptyPoll(queue.Get()) ||
	    !Expect(QueueApi::KernelTriggerUserEvent(queue.Get(), scalar_id, &arguments), 0, "trigger scalar-added Edge user event"))
	{
		return false;
	}
	CapturedOutput output;
	return UserPoll(queue.Get(), &output, 1) &&
	       MatchesUser(output.buffer.events[0], static_cast<uint32_t>(scalar_id), &arguments, USER_EVENT_EDGE_MODE) &&
	       EmptyPoll(queue.Get()) && queue.Delete();
}

} // namespace

int main(int argc, char** argv)
{
	const char* mode = argc == 2 ? argv[1] : "all";
	if (argc > 2 || (std::strcmp(mode, "all") != 0 && std::strcmp(mode, "generic") != 0 && std::strcmp(mode, "user") != 0))
	{
		std::fprintf(stderr, "usage: KernelUserEventTest [all|generic|user]\n");
		return 1;
	}
	Watchdog watchdog;
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);

	if (std::strcmp(mode, "user") != 0 &&
	    (!TestKeysAndBoundedRetrieval() || !TestGenericReplacement() || !TestWaitModes() ||
	     !TestDeleteWithWaiter() || !TestPopulatedDeletion() || !TestPopulatedSessionReset()))
	{
		return 1;
	}
	if (std::strcmp(mode, "generic") != 0 &&
	    (!TestUserOperations() || !TestEdgeUserOperations() || !TestUserIdentifiersAndFilterIsolation() || !TestUserExports()))
	{
		return 1;
	}
	std::printf("KernelUserEventTest: PASS: %s\n", mode);
	return 0;
}
