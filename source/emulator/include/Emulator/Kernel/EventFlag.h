#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAG_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAG_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/Pthread.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::EventFlag {

class KernelEventFlagPrivate;

using KernelEventFlag = KernelEventFlagPrivate*;

int LUMEN_SYSV_ABI KernelCreateEventFlag(KernelEventFlag* ef, const char* name, uint32_t attr, uint64_t init_pattern, const void* param);
int LUMEN_SYSV_ABI KernelDeleteEventFlag(KernelEventFlag ef);
int LUMEN_SYSV_ABI KernelWaitEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode, uint64_t* result_pat,
                                      KernelUseconds* timeout);
int LUMEN_SYSV_ABI KernelPollEventFlag(KernelEventFlag ef, uint64_t bit_pattern, uint32_t wait_mode, uint64_t* result_pat);
int LUMEN_SYSV_ABI KernelSetEventFlag(KernelEventFlag ef, uint64_t bit_pattern);
int LUMEN_SYSV_ABI KernelClearEventFlag(KernelEventFlag ef, uint64_t bit_pattern);
int LUMEN_SYSV_ABI KernelCancelEventFlag(KernelEventFlag ef, uint64_t set_pattern, int* num_wait_threads);

// Destroy every session-owned event flag while keeping the process-owned
// registry available for the next guest. The caller must have stopped all
// guest threads.
[[nodiscard]] bool ResetForNextSession();

#if defined(LUMEN_KERNEL_EVENT_FLAG_TEST_HOOK)
// The test owns ef and queries waiters before starting its deletion.
[[nodiscard]] bool EventFlagHasWaitersForTest(KernelEventFlag ef);
[[nodiscard]] bool EventFlagRegistryEmptyForTest();
#endif

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool EventFlagSessionResetContractTest();
#endif

} // namespace Lumen::Libs::LibKernel::EventFlag

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_EVENTFLAG_H_ */
