#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Pthread.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::EventQueue {

constexpr int16_t KERNEL_EVFILT_TIMER     = -7;
constexpr int16_t KERNEL_EVFILT_READ      = -1;
constexpr int16_t KERNEL_EVFILT_WRITE     = -2;
constexpr int16_t KERNEL_EVFILT_USER      = -11;
constexpr int16_t KERNEL_EVFILT_FILE      = -4;
constexpr int16_t KERNEL_EVFILT_GRAPHICS  = -14;
constexpr int16_t KERNEL_EVFILT_VIDEO_OUT = -13;
constexpr int16_t KERNEL_EVFILT_HRTIMER   = -15;

class KernelEqueuePrivate;
struct KernelEqueueEvent;

using KernelEqueue = KernelEqueuePrivate*;

// Internal producer identity. The generation distinguishes a new queue that
// reuses a retired handle's address; it is never exposed through the guest ABI.
struct KernelEqueueIdentity
{
	KernelEqueue queue = nullptr;
	uint64_t generation = 0;
};

[[nodiscard]] bool GetEqueueIdentity(KernelEqueue eq, KernelEqueueIdentity* identity);

using trigger_func_t = void (*)(KernelEqueueEvent* event, void* trigger_data);
using reset_func_t   = void (*)(KernelEqueueEvent* event);
using delete_func_t  = void (*)(KernelEqueue eq, KernelEqueueEvent* event);
using registered_event_func_t = void (*)(const KernelEqueueIdentity& identity, const KernelEqueueEvent& event);

struct KernelEvent
{
	uintptr_t ident  = 0;
	int16_t   filter = 0;
	uint16_t  flags  = 0;
	uint32_t  fflags = 0;
	intptr_t  data   = 0;
	void*     udata  = nullptr;
};

struct KernelFilter
{
	void*          data              = nullptr;
	trigger_func_t trigger_func      = nullptr;
	reset_func_t   reset_func        = nullptr;
	delete_func_t  delete_event_func = nullptr;
};

struct KernelEqueueEvent
{
	bool         triggered = false;
	KernelEvent  event;
	KernelFilter filter;
};

int LUMEN_SYSV_ABI KernelAddEvent(KernelEqueue eq, const KernelEqueueEvent& event);
int LUMEN_SYSV_ABI KernelTriggerEvent(KernelEqueue eq, uintptr_t ident, int16_t filter, void* trigger_data);
int LUMEN_SYSV_ABI KernelDeleteEvent(KernelEqueue eq, uintptr_t ident, int16_t filter);

// Verify identity and hold the queue registry across access. Filter callbacks
// must not acquire the registry lock or call these functions recursively.
int AddEventIfRegistered(const KernelEqueueIdentity& identity, const KernelEqueueEvent& event,
                         registered_event_func_t registered = nullptr);
int TriggerEventIfRegistered(const KernelEqueueIdentity& identity, uintptr_t ident, int16_t filter, void* trigger_data);
int DeleteEventIfRegistered(const KernelEqueueIdentity& identity, uintptr_t ident, int16_t filter);

int LUMEN_SYSV_ABI KernelAddUserEvent(KernelEqueue eq, int id);
int LUMEN_SYSV_ABI KernelAddUserEventEdge(KernelEqueue eq, int id);
int LUMEN_SYSV_ABI KernelTriggerUserEvent(KernelEqueue eq, int id, void* udata);
int LUMEN_SYSV_ABI KernelDeleteUserEvent(KernelEqueue eq, int id);

int LUMEN_SYSV_ABI KernelCreateEqueue(KernelEqueue* eq, const char* name);
int LUMEN_SYSV_ABI KernelDeleteEqueue(KernelEqueue eq);
int LUMEN_SYSV_ABI KernelWaitEqueue(KernelEqueue eq, KernelEvent* ev, int num, int* out, const KernelUseconds* timo);

#if defined(LUMEN_KERNEL_USER_EVENT_TEST_HOOK)
[[nodiscard]] bool EventQueueHasWaitersForTest(KernelEqueue eq);
#endif

// Destroy every session-owned event queue while keeping the process-owned
// registry available for the next guest. The caller must have stopped all
// guest threads and must invoke this before resetting queue event producers.
[[nodiscard]] bool ResetForNextSession();

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool EventQueueSessionResetContractTest();
#endif

intptr_t LUMEN_SYSV_ABI  KernelGetEventData(const KernelEvent* ev);
intptr_t LUMEN_SYSV_ABI  KernelGetEventFflags(const KernelEvent* ev);
int LUMEN_SYSV_ABI       KernelGetEventFilter(const KernelEvent* ev);
uintptr_t LUMEN_SYSV_ABI KernelGetEventId(const KernelEvent* ev);
void* LUMEN_SYSV_ABI     KernelGetEventUserData(const KernelEvent* ev);
int LUMEN_SYSV_ABI       KernelGetEventError(const KernelEvent* ev);

} // namespace Lumen::Libs::LibKernel::EventQueue

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTQUEUE_H_ */
