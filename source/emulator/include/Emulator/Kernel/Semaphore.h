#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_SEMAPHORE_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_SEMAPHORE_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Pthread.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::Semaphore {

class KernelSemaPrivate;

using KernelSema = KernelSemaPrivate*;

int LUMEN_SYSV_ABI KernelCreateSema(KernelSema* sem, const char* name, uint32_t attr, int init, int max, void* opt);
int LUMEN_SYSV_ABI KernelDeleteSema(KernelSema sem);
int LUMEN_SYSV_ABI KernelWaitSema(KernelSema sem, int need, KernelUseconds* time);
int LUMEN_SYSV_ABI KernelPollSema(KernelSema sem, int need);
int LUMEN_SYSV_ABI KernelSignalSema(KernelSema sem, int count);
int LUMEN_SYSV_ABI KernelCancelSema(KernelSema sem, int count, int* threads);

// Process-local pthread semaphore handles share the POSIX semaphore backend.
// Unlike POSIX entrypoints these return kernel errors and do not set errno.
int LUMEN_SYSV_ABI PthreadSemInit(KernelSema* sem, int32_t flag, uint32_t value, const char* name);
int LUMEN_SYSV_ABI PthreadSemDestroy(KernelSema* sem);
int LUMEN_SYSV_ABI PthreadSemWait(KernelSema* sem);
int LUMEN_SYSV_ABI PthreadSemTrywait(KernelSema* sem);
int LUMEN_SYSV_ABI PthreadSemTimedwait(KernelSema* sem, uint32_t usec);
int LUMEN_SYSV_ABI PthreadSemPost(KernelSema* sem);
int LUMEN_SYSV_ABI PthreadSemGetvalue(KernelSema* sem, int32_t* value);

// Destroy every session-owned semaphore while keeping the process-owned
// registry available for the next guest. The caller must have stopped all
// guest threads and released subsystem-owned semaphores first.
[[nodiscard]] bool ResetForNextSession();

#if defined(LUMEN_POSIX_SEMAPHORE_TEST_HOOK)
// Original test observer; the caller must retain a live semaphore.
[[nodiscard]] uint32_t SemaphoreWaiterCountForTest(KernelSema sem);
#endif

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool SemaphoreSessionResetContractTest();
#endif

} // namespace Lumen::Libs::LibKernel::Semaphore

namespace Lumen::Libs::Posix {

int LUMEN_SYSV_ABI sem_init(LibKernel::Semaphore::KernelSema* sem, int pshared, unsigned value);
int LUMEN_SYSV_ABI sem_post(LibKernel::Semaphore::KernelSema* sem);
int LUMEN_SYSV_ABI sem_wait(LibKernel::Semaphore::KernelSema* sem);
int LUMEN_SYSV_ABI sem_trywait(LibKernel::Semaphore::KernelSema* sem);
int LUMEN_SYSV_ABI sem_timedwait(LibKernel::Semaphore::KernelSema* sem, const LibKernel::KernelTimespec* deadline);
int LUMEN_SYSV_ABI sem_destroy(LibKernel::Semaphore::KernelSema* sem);

#if defined(LUMEN_POSIX_SEMAPHORE_TEST_HOOK)
// The inline profile keeps no process-side state: the guest record itself is the
// object, so the observer reports whether that record is still usable.
[[nodiscard]] bool InlinePosixSemaphoreRecordSupportedForTest(const void* slot);
[[nodiscard]] bool SemTimedWaitCarryEnvironmentValueEnabledForTest(const char* value);
[[nodiscard]] bool SemTimedWaitNormalizeDeadlineForTest(
    const LibKernel::KernelTimespec& input, bool carry_enabled,
    LibKernel::KernelTimespec* output, bool* carried);
#endif

} // namespace Lumen::Libs::Posix

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_SEMAPHORE_H_ */
