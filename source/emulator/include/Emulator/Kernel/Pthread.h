#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"

// IWYU pragma: no_include <pthread.h>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Loader {
struct Program;
} // namespace Lumen::Loader

namespace Lumen::Libs {

namespace LibKernel {

LUMEN_SUBSYSTEM_DEFINE(Pthread);

struct PthreadAttrPrivate;
struct PthreadPrivate;
struct PthreadMutexPrivate;
struct PthreadMutexattrPrivate;
struct PthreadRwlockPrivate;
struct PthreadRwlockattrPrivate;
struct PthreadCondattrPrivate;
struct PthreadCondPrivate;

struct KernelTimespec
{
	int64_t tv_sec;
	int64_t tv_nsec;
};

struct KernelTimeval
{
	int64_t tv_sec;
	int64_t tv_usec;
};

using PthreadAttr       = PthreadAttrPrivate*;
using Pthread           = PthreadPrivate*;
using KernelCpumask     = uint64_t;
using PthreadMutex      = PthreadMutexPrivate*;
using PthreadMutexattr  = PthreadMutexattrPrivate*;
struct KernelSchedParam
{
	int sched_priority;
};
using PthreadRwlock     = PthreadRwlockPrivate*;
using PthreadRwlockattr = PthreadRwlockattrPrivate*;
using KernelUseconds    = unsigned int;
using PthreadCondattr   = PthreadCondattrPrivate*;
using PthreadCond       = PthreadCondPrivate*;
using KernelClockid     = int32_t;
using PthreadKey        = int;

using pthread_entry_func_t          = LUMEN_SYSV_ABI void* (*)(void*);
using thread_dtors_func_t           = LUMEN_SYSV_ABI void (*)();
using pthread_key_destructor_func_t = LUMEN_SYSV_ABI void (*)(void*);
using pthread_once_init_func_t      = LUMEN_SYSV_ABI void (*)();
using pthread_cleanup_func_t        = LUMEN_SYSV_ABI void (*)(void*);

struct alignas(uint64_t) PthreadCleanupInfo
{
	uint64_t opaque[8];
};

void PthreadInitSelfForMainThread();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
void PthreadFinishSelfForMainThread();
#endif
void PthreadDeleteStaticObjects(Loader::Program* program);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
// Called only after cooperative stop has made guest execution idle. Reaps
// every host pthread wrapper without invoking guest destructors and clears
// guest-owned TLS key/callback state while keeping the process reaper and
// default pthread attributes alive for the next runtime session.
[[nodiscard]] bool PthreadResetForNextSession();
#endif

int LUMEN_SYSV_ABI PthreadMutexattrInit(PthreadMutexattr* attr);
int LUMEN_SYSV_ABI PthreadMutexattrDestroy(PthreadMutexattr* attr);
int LUMEN_SYSV_ABI PthreadMutexattrSettype(PthreadMutexattr* attr, int type);
int LUMEN_SYSV_ABI PthreadMutexattrSetprotocol(PthreadMutexattr* attr, int protocol);
int LUMEN_SYSV_ABI PthreadMutexInit(PthreadMutex* mutex, const PthreadMutexattr* attr, const char* name);
int LUMEN_SYSV_ABI PthreadMutexDestroy(PthreadMutex* mutex);
int LUMEN_SYSV_ABI PthreadMutexLock(PthreadMutex* mutex);
int LUMEN_SYSV_ABI PthreadMutexTrylock(PthreadMutex* mutex);
int LUMEN_SYSV_ABI PthreadMutexTimedlock(PthreadMutex* mutex, uint64_t usec);
int LUMEN_SYSV_ABI PthreadMutexUnlock(PthreadMutex* mutex);

Pthread LUMEN_SYSV_ABI PthreadSelf();
int LUMEN_SYSV_ABI     PthreadCreate(Pthread* thread, const PthreadAttr* attr, pthread_entry_func_t entry, void* arg, const char* name);
int LUMEN_SYSV_ABI     PthreadDetach(Pthread thread);
int LUMEN_SYSV_ABI     PthreadJoin(Pthread thread, void** value);
#if defined(LUMEN_PTHREAD_EXIT_TEST_HOOK)
using PthreadExitTestHook = void (*)(Pthread);
bool PthreadBackingAllocationContractTest();
bool PthreadStaticMutexInitializationContractTest();
bool PthreadMutexDirectoryForTest();
bool PthreadMutexDirectoryContractTest();
void PthreadSetExitTestHooks(PthreadExitTestHook after_cleanup, PthreadExitTestHook before_native_join);
void PthreadReapDetachedThreadsForTest();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
uint64_t LUMEN_SYSV_ABI PthreadBackingAllocationFexContractTest(uint64_t scenario);
uint64_t LUMEN_SYSV_ABI PthreadMutexInitializationFexContractTest(uint64_t scenario);
// Host-only rendezvous after a native sleep slice, while its wait is active.
using PthreadSleepSliceTestHook = void (*)(uint64_t);
void PthreadSetSleepSliceTestHook(PthreadSleepSliceTestHook hook);
#endif
#endif
int LUMEN_SYSV_ABI     PthreadCancel(Pthread thread);
int LUMEN_SYSV_ABI     PthreadSetcancelstate(int state, int* old_state);
int LUMEN_SYSV_ABI     PthreadSetcanceltype(int type, int* old_type);
int LUMEN_SYSV_ABI     PthreadGetprio(Pthread thread, int* prio);
int LUMEN_SYSV_ABI     PthreadGetschedparam(Pthread thread, uint32_t* policy, KernelSchedParam* param);
int LUMEN_SYSV_ABI     PthreadSetschedparam(Pthread thread, uint32_t policy, const KernelSchedParam* param);
int LUMEN_SYSV_ABI     PthreadSetprio(Pthread thread, int prio);
void LUMEN_SYSV_ABI    PthreadTestcancel();
int LUMEN_SYSV_ABI     PthreadGetaffinity(Pthread thread, KernelCpumask* mask);
int LUMEN_SYSV_ABI     PthreadSetaffinity(Pthread thread, KernelCpumask mask);
// Application metadata mode, independent of host scheduling and thread masks.
[[nodiscard]] int32_t AppCpuModeFromAttributes(uint32_t attributes);
int32_t LUMEN_SYSV_ABI KernelGetCpumode();
void LUMEN_SYSV_ABI    PthreadExit(void* value);
void LUMEN_SYSV_ABI    PthreadCleanupPush(pthread_cleanup_func_t routine, void* argument,
                                          PthreadCleanupInfo* info);
void LUMEN_SYSV_ABI    PthreadCleanupPop(int execute);
// True only while this thread owns run_thread's registered cleanup boundary.
bool PthreadHasManagedExitBoundary();
int LUMEN_SYSV_ABI     PthreadEqual(Pthread thread1, Pthread thread2);
int LUMEN_SYSV_ABI     PthreadGetname(Pthread thread, char* name);
int LUMEN_SYSV_ABI     PthreadRename(Pthread thread, const char* name);
void LUMEN_SYSV_ABI    PthreadYield();
int LUMEN_SYSV_ABI     PthreadGetthreadid();

int LUMEN_SYSV_ABI          KernelUsleep(KernelUseconds microseconds);
unsigned int LUMEN_SYSV_ABI KernelSleep(unsigned int seconds);
int LUMEN_SYSV_ABI          KernelNanosleep(const KernelTimespec* rqtp, KernelTimespec* rmtp);

int LUMEN_SYSV_ABI   PthreadKeyCreate(PthreadKey* key, pthread_key_destructor_func_t destructor);
int LUMEN_SYSV_ABI   PthreadKeyDelete(PthreadKey key);
int LUMEN_SYSV_ABI   PthreadSetspecific(PthreadKey key, void* value);
void* LUMEN_SYSV_ABI PthreadGetspecific(PthreadKey key);
int LUMEN_SYSV_ABI   PthreadOnce(const void* once_control, pthread_once_init_func_t init_routine);

void LUMEN_SYSV_ABI KernelSetThreadDtors(thread_dtors_func_t dtors);

int LUMEN_SYSV_ABI PthreadAttrInit(PthreadAttr* attr);
int LUMEN_SYSV_ABI PthreadAttrDestroy(PthreadAttr* attr);
int LUMEN_SYSV_ABI PthreadAttrGet(Pthread thread, PthreadAttr* attr);
int LUMEN_SYSV_ABI PthreadAttrGetaffinity(const PthreadAttr* attr, KernelCpumask* mask);
int LUMEN_SYSV_ABI PthreadAttrGetdetachstate(const PthreadAttr* attr, int* state);
int LUMEN_SYSV_ABI PthreadAttrGetguardsize(const PthreadAttr* attr, size_t* guard_size);
int LUMEN_SYSV_ABI PthreadAttrGetinheritsched(const PthreadAttr* attr, int* inherit_sched);
int LUMEN_SYSV_ABI PthreadAttrGetschedparam(const PthreadAttr* attr, KernelSchedParam* param);
int LUMEN_SYSV_ABI PthreadAttrGetschedpolicy(const PthreadAttr* attr, int* policy);
int LUMEN_SYSV_ABI PthreadAttrGetstack(const PthreadAttr* __restrict attr, void** __restrict stack_addr, size_t* __restrict stack_size);
int LUMEN_SYSV_ABI PthreadAttrGetstackaddr(const PthreadAttr* attr, void** stack_addr);
int LUMEN_SYSV_ABI PthreadAttrGetstacksize(const PthreadAttr* attr, size_t* stack_size);
#if defined(LUMEN_PTHREAD_ATTRIBUTE_TEST_HOOK)
bool PthreadAttributeHostOutputContractTest();
#endif
int LUMEN_SYSV_ABI PthreadAttrSetaffinity(PthreadAttr* attr, KernelCpumask mask);
int LUMEN_SYSV_ABI PthreadAttrSetdetachstate(PthreadAttr* attr, int state);
int LUMEN_SYSV_ABI PthreadAttrSetguardsize(PthreadAttr* attr, size_t guard_size);
int LUMEN_SYSV_ABI PthreadAttrSetinheritsched(PthreadAttr* attr, int inherit_sched);
int LUMEN_SYSV_ABI PthreadAttrSetschedparam(PthreadAttr* attr, const KernelSchedParam* param);
int LUMEN_SYSV_ABI PthreadAttrSetschedpolicy(PthreadAttr* attr, int policy);
int LUMEN_SYSV_ABI PthreadAttrSetstack(PthreadAttr* attr, void* addr, size_t size);
int LUMEN_SYSV_ABI PthreadAttrSetstackaddr(PthreadAttr* attr, void* addr);
int LUMEN_SYSV_ABI PthreadAttrSetstacksize(PthreadAttr* attr, size_t stack_size);

int LUMEN_SYSV_ABI PthreadRwlockDestroy(PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI PthreadRwlockInit(PthreadRwlock* rwlock, const PthreadRwlockattr* attr, const char* name);
int LUMEN_SYSV_ABI PthreadRwlockRdlock(PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI PthreadRwlockTimedrdlock(PthreadRwlock* rwlock, KernelUseconds usec);
int LUMEN_SYSV_ABI PthreadRwlockTimedwrlock(PthreadRwlock* rwlock, KernelUseconds usec);
int LUMEN_SYSV_ABI PthreadRwlockTryrdlock(PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI PthreadRwlockTrywrlock(PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI PthreadRwlockUnlock(PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI PthreadRwlockWrlock(PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI PthreadRwlockattrDestroy(PthreadRwlockattr* attr);
int LUMEN_SYSV_ABI PthreadRwlockattrInit(PthreadRwlockattr* attr);
int LUMEN_SYSV_ABI PthreadRwlockattrGettype(PthreadRwlockattr* attr, int* type);
int LUMEN_SYSV_ABI PthreadRwlockattrSettype(PthreadRwlockattr* attr, int type);

int LUMEN_SYSV_ABI PthreadCondattrDestroy(PthreadCondattr* attr);
int LUMEN_SYSV_ABI PthreadCondattrInit(PthreadCondattr* attr);
int LUMEN_SYSV_ABI PthreadCondBroadcast(PthreadCond* cond);
int LUMEN_SYSV_ABI PthreadCondDestroy(PthreadCond* cond);
int LUMEN_SYSV_ABI PthreadCondInit(PthreadCond* cond, const PthreadCondattr* attr, const char* name);
int LUMEN_SYSV_ABI PthreadCondSignal(PthreadCond* cond);
int LUMEN_SYSV_ABI PthreadCondSignalto(PthreadCond* cond, Pthread thread);
int LUMEN_SYSV_ABI PthreadCondTimedwait(PthreadCond* cond, PthreadMutex* mutex, KernelUseconds usec);
int LUMEN_SYSV_ABI PthreadCondTimedwaitAbsolute(PthreadCond* cond, PthreadMutex* mutex,
                                                const KernelTimespec* deadline);
int LUMEN_SYSV_ABI PthreadCondWait(PthreadCond* cond, PthreadMutex* mutex);
#if defined(LUMEN_PTHREAD_CONDITION_TEST_HOOK)
// Thread-local injection before the generation check; true after a slice.
using PthreadConditionWaitTestHook = void (*)(PthreadCond, bool);
void PthreadSetConditionWaitTestHook(PthreadConditionWaitTestHook hook);
#endif

int LUMEN_SYSV_ABI      KernelClockGetres(KernelClockid clock_id, KernelTimespec* tp);
int LUMEN_SYSV_ABI      KernelClockGettime(KernelClockid clock_id, KernelTimespec* tp);
// Native callers own the output storage. Only the guest ABI entry above
// acquires guest write authority; internal deadline sampling needs none.
int KernelClockGettimeHost(KernelClockid clock_id, KernelTimespec& value);
#if defined(LUMEN_POSIX_SEMAPHORE_TEST_HOOK)
uint64_t PthreadArgumentLeaseAcquisitionsForTest();
#endif
int LUMEN_SYSV_ABI      KernelGettimeofday(KernelTimeval* tp);
uint64_t LUMEN_SYSV_ABI KernelGetTscFrequency();
uint64_t LUMEN_SYSV_ABI KernelReadTsc();
uint64_t LUMEN_SYSV_ABI KernelGetProcessTime();
uint64_t LUMEN_SYSV_ABI KernelGetProcessTimeCounter();
uint64_t LUMEN_SYSV_ABI KernelGetProcessTimeCounterFrequency();

// PS4 exception delivery (libkernel_unity). KernelRaiseException resolves the
// guest pthread handle through the live pool and queues one pending exception
// on the target thread; the target consumes it at its next HLE boundary or
// between blocking host-wait slices, running the installed handler as a nested
// guest call and resuming the interrupted import or wait when the handler
// returns. Delivery requires a reconstructed interrupted GP state, so it is
// limited to the arm64 hosts whose guest-call path supports nested guest calls.
//
// The handler receives (signum, ucontext) with the PS4 layout the Unity GC
// callback reads: mcontext at +0x40 with the GP registers from mcontext+0x08,
// mc_rip at +0xe0 and mc_rsp at +0xf8. The console structure is larger than the
// fields read by the handler; the delivery zeroes the complete buffer and
// publishes only the reconstructed state.
//
// Verified consumers of the delivered buffer (read-only audit of the title
// modules, 27 September 2026): the installed wrapper reads only mc_rsp at
// +0xf8, the game callback copies [+0x00, +0xd8) into its thread record, and
// the collector scans exactly the 15 GP words at +0x48..+0xbf plus the stack
// below mc_rsp. A zero in every other field (header, mc_onstack,
// trap/segment/error, mc_cs, mc_rflags, mc_ss, FP state) cannot influence the
// collected roots.
constexpr size_t kExceptionContextSize    = 0x500;
constexpr size_t kExceptionMcontextOffset = 0x40;
constexpr size_t kExceptionMcRdiOffset    = kExceptionMcontextOffset + 0x08;
constexpr size_t kExceptionMcRsiOffset    = kExceptionMcontextOffset + 0x10;
constexpr size_t kExceptionMcRdxOffset    = kExceptionMcontextOffset + 0x18;
constexpr size_t kExceptionMcRcxOffset    = kExceptionMcontextOffset + 0x20;
constexpr size_t kExceptionMcR8Offset     = kExceptionMcontextOffset + 0x28;
constexpr size_t kExceptionMcR9Offset     = kExceptionMcontextOffset + 0x30;
constexpr size_t kExceptionMcRaxOffset    = kExceptionMcontextOffset + 0x38;
constexpr size_t kExceptionMcRbxOffset    = kExceptionMcontextOffset + 0x40;
constexpr size_t kExceptionMcRbpOffset    = kExceptionMcontextOffset + 0x48;
constexpr size_t kExceptionMcR10Offset    = kExceptionMcontextOffset + 0x50;
constexpr size_t kExceptionMcR11Offset    = kExceptionMcontextOffset + 0x58;
constexpr size_t kExceptionMcR12Offset    = kExceptionMcontextOffset + 0x60;
constexpr size_t kExceptionMcR13Offset    = kExceptionMcontextOffset + 0x68;
constexpr size_t kExceptionMcR14Offset    = kExceptionMcontextOffset + 0x70;
constexpr size_t kExceptionMcR15Offset    = kExceptionMcontextOffset + 0x78;
constexpr size_t kExceptionMcRipOffset    = kExceptionMcontextOffset + 0xa0;
constexpr size_t kExceptionMcRspOffset    = kExceptionMcontextOffset + 0xb8;

// Handler registered for one allowed signal, or 0 when none is installed.
uint64_t KernelExceptionHandlerForSignal(int32_t signum);
// Bounded numeric diagnostic writer shared with the libkernel_unity trace.
void KernelTraceExceptionEvent(const char* operation, int32_t signum, uint64_t thread, uint64_t handler, int32_t result);
// Queue one pending exception on the live thread resolved from the guest
// handle. Returns OK, or ESRCH for an unknown, stale or exiting handle. A
// target executing translated code is additionally armed at its next
// generated-code boundary through the FEX interrupt page; a target without an
// active run keeps the ordinary HLE boundary and host-wait slice routes.
int32_t PthreadQueueExceptionForHandle(uint64_t handle, int32_t signum);
// Run one queued exception handler on the calling thread when the thread is a
// guest thread with a pending request and an interrupted GP state is available.
// `interrupted_gprs` supplies the GP state directly (x86-64 register order) and
// may be null to use the active host call boundary of this thread. The request
// stays queued while the interrupted state or the signal route is unavailable.
// An armed generated-code boundary for the same request is cancelled once a
// handler ran here. Returns true when a handler ran.
bool PthreadDeliverPendingException(const uint64_t* interrupted_gprs = nullptr);
// Run one queued exception handler on the calling thread with an explicit
// generated-code boundary state: `gpr` follows the x86-64 order (0=rax ..
// 4=rsp .. 15=r15) and `resume_rip` is the reconstructed guest resume point of
// the interrupted code. The request stays queued while another handler is
// already active on this thread and is consumed exactly once otherwise.
// Returns true when a handler ran.
bool PthreadDeliverPendingExceptionAtBoundary(const uint64_t gpr[16], uint64_t resume_rip);
// Reconstruct the PS4 ucontext bytes from the interrupted GP state. `gpr`
// follows the x86-64 order used by the FEX host-call boundary (0=rax ..
// 4=rsp .. 7=rdi .. 15=r15).
void PthreadBuildExceptionContext(const uint64_t gpr[16], uint64_t resume_rip, uint8_t* context);

// Guest pthread handle (the emulated pthread_t) of the calling thread, or
// zero when the calling thread has no published guest pthread wrapper. The
// runtime publishes it in the FreeBSD thread control block's pthread slot
// (`fs:[0x10]`) so guest code that reads thread state through the TCB observes
// a live object instead of a null pointer.
uint64_t PthreadGuestSelfHandle();

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
// Local capture diagnostic (selector LUMEN_MACOS_VM_ROOT_SNAPSHOT): seal this
// guest thread's most recent boundary state into its own published slot so a
// deliver/handled point on another thread can report every guest thread's
// captured registers and stack window without taking the pthread pool lock.
// `gpr` follows the x86-64 order (0=rax .. 4=rsp .. 7=rdi .. 15=r15) and
// `stackBase`/`stackSize` bound the private x86-64 stack allocation that owns
// RSP. A no-op when the selector is off, the caller is not a guest thread or
// every slot is taken.
void KernelGuestThreadPublishBoundaryState(const uint64_t gpr[16], uint64_t rip,
                                           uint64_t hostCall, uint64_t stackBase,
                                           uint64_t stackSize);
#endif

} // namespace LibKernel

namespace Posix {

int LUMEN_SYSV_ABI   pthread_attr_destroy(LibKernel::PthreadAttr* attr);
int LUMEN_SYSV_ABI   pthread_attr_init(LibKernel::PthreadAttr* attr);
int LUMEN_SYSV_ABI   pthread_attr_setstacksize(LibKernel::PthreadAttr* attr, size_t stack_size);
int LUMEN_SYSV_ABI   pthread_cancel(LibKernel::Pthread thread);
int LUMEN_SYSV_ABI   pthread_detach(LibKernel::Pthread thread);
int LUMEN_SYSV_ABI   pthread_equal(LibKernel::Pthread left, LibKernel::Pthread right);
void LUMEN_SYSV_ABI  pthread_exit(void* value);
int LUMEN_SYSV_ABI   pthread_getschedparam(LibKernel::Pthread thread, uint32_t* policy, LibKernel::KernelSchedParam* param);
LibKernel::Pthread LUMEN_SYSV_ABI pthread_self();
int LUMEN_SYSV_ABI   pthread_create(LibKernel::Pthread* thread, const LibKernel::PthreadAttr* attr, LibKernel::pthread_entry_func_t entry,
                                   void* arg);
int LUMEN_SYSV_ABI   pthread_join(LibKernel::Pthread thread, void** value);
int LUMEN_SYSV_ABI   pthread_setschedparam(LibKernel::Pthread thread, uint32_t policy, const LibKernel::KernelSchedParam* param);
int LUMEN_SYSV_ABI   pthread_cond_broadcast(LibKernel::PthreadCond* cond);
int LUMEN_SYSV_ABI   pthread_cond_destroy(LibKernel::PthreadCond* cond);
int LUMEN_SYSV_ABI   pthread_cond_init(LibKernel::PthreadCond* cond, const LibKernel::PthreadCondattr* attr);
int LUMEN_SYSV_ABI   pthread_cond_signal(LibKernel::PthreadCond* cond);
int LUMEN_SYSV_ABI   pthread_cond_wait(LibKernel::PthreadCond* cond, LibKernel::PthreadMutex* mutex);
int LUMEN_SYSV_ABI   pthread_mutex_lock(LibKernel::PthreadMutex* mutex);
int LUMEN_SYSV_ABI   pthread_mutex_trylock(LibKernel::PthreadMutex* mutex);
int LUMEN_SYSV_ABI   pthread_mutex_unlock(LibKernel::PthreadMutex* mutex);
int LUMEN_SYSV_ABI   pthread_rwlock_rdlock(LibKernel::PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI   pthread_rwlock_unlock(LibKernel::PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI   pthread_rwlock_wrlock(LibKernel::PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI   pthread_key_create(LibKernel::PthreadKey* key, LibKernel::pthread_key_destructor_func_t destructor);
int LUMEN_SYSV_ABI   pthread_key_delete(LibKernel::PthreadKey key);
int LUMEN_SYSV_ABI   pthread_setspecific(LibKernel::PthreadKey key, void* value);
void* LUMEN_SYSV_ABI pthread_getspecific(LibKernel::PthreadKey key);
int LUMEN_SYSV_ABI   pthread_mutex_destroy(LibKernel::PthreadMutex* mutex);
int LUMEN_SYSV_ABI   pthread_mutex_init(LibKernel::PthreadMutex* mutex, const LibKernel::PthreadMutexattr* attr);
int LUMEN_SYSV_ABI   pthread_mutexattr_init(LibKernel::PthreadMutexattr* attr);
int LUMEN_SYSV_ABI   pthread_mutexattr_settype(LibKernel::PthreadMutexattr* attr, int type);
int LUMEN_SYSV_ABI   pthread_mutexattr_setprotocol(LibKernel::PthreadMutexattr* attr, int protocol);
int LUMEN_SYSV_ABI   pthread_mutexattr_destroy(LibKernel::PthreadMutexattr* attr);
int LUMEN_SYSV_ABI   pthread_rwlock_destroy(LibKernel::PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI   pthread_rwlock_init(LibKernel::PthreadRwlock* rwlock, const LibKernel::PthreadRwlockattr* attr);
int LUMEN_SYSV_ABI   pthread_rwlock_tryrdlock(LibKernel::PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI   pthread_rwlock_trywrlock(LibKernel::PthreadRwlock* rwlock);
int LUMEN_SYSV_ABI   pthread_rwlockattr_destroy(LibKernel::PthreadRwlockattr* attr);
int LUMEN_SYSV_ABI   pthread_rwlockattr_init(LibKernel::PthreadRwlockattr* attr);

} // namespace Posix

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_PTHREAD_H_ */
