#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_IME_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_IME_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Ime {

// Process-latched local compatibility selector gating the whole local Ime
// profile. It does not touch host input devices, system keyboards or any
// keyboard UI surface.
[[nodiscard]] bool ExperimentalEnabled();

// Clears only the bounded state (diagnostic counters and the live keyboard
// session latch) after guest execution has stopped. The selector remains
// latched and no console Ime lifecycle is modeled beyond the measured calls
// (open, resource id, info and update).
void ResetSessionState();

// Measured keyboard-open profile (campaigns LMNQ92807 and LMNQ92808, 28
// September 2026): one local offline user (id 1), a single live session
// latch, the measured error codes reused unchanged, the parameter never read
// beyond the null check and no guest writes in any measured case. The
// interface document declares the local choices for the unmeasured
// combinations.
int32_t LUMEN_SYSV_ABI KeyboardOpen(int32_t userId, const void* param);

// Measured keyboard-resource-id profile (campaign LMNQ92809, 28 September
// 2026): with a live session owned by its user the console returns 0 and
// writes a 12-byte record whose first word is the measured resource id
// 0x1186ef73 (identical across the two measured processes and six successful
// calls); the live-session gate precedes the null-output refusal
// (0x80bc0031), which precedes the user classification (any user other than
// the session owner receives 0x80bc0010, including 0 and -1); without a live
// session every call receives 0x80bc0002; no measured case writes outside
// the 12-byte record. The local model keeps the single offline user (id 1)
// as the session owner, reuses the measured value and stops fail-closed when
// a non-null output is not writable (unmeasured combination).
int32_t LUMEN_SYSV_ABI KeyboardGetResourceId(int32_t userId, void* resourceIdArray);

// Measured keyboard-info profile (campaigns LMNQ92810/92811, 29 September
// 2026): the live-session gate (0x80bc0002, no writes) precedes the
// null-output refusal (0x80bc0031), which precedes the id classification;
// with a live session and a valid output only the ids 0 and 1 are accepted
// (20-value sweep including 0x7fffffff, 0x80000000 and the user-id value
// 0x1186ef73) and each publishes its own 24-byte record (id 0: 0xffffffff
// followed by five zero words; id 1: 0xffffffff, 1, 0, 0, 0, 1), while every
// other id receives -1 with a 36-byte run of zeroes; no measured case writes
// outside the published block. The local model publishes the measured
// records unchanged and stops fail-closed when a non-null output is not
// writable (unmeasured combination).
int32_t LUMEN_SYSV_ABI KeyboardGetInfo(uint32_t resourceId, void* info);

// Measured keyboard-update profile (campaigns LMNQ92812 and LMNQ92813, 29
// September 2026, two fresh probe processes): every measured call - without a
// live session, with one (also in the title-order chain open, resource id,
// info), after Close and after an all-zero open parameter - returned the same
// code 0x80bc0002, never invoked the probe handler (13 calls, handler count
// 0) and returned in 2-11 microseconds (the handler pointer is never read
// beyond the null comparison). The gate that would make an update succeed was
// not identified, so the local model reproduces the measured refusal: it
// returns 0x80bc0002, leaves the session latch unchanged and never invokes
// the guest handler.
int32_t LUMEN_SYSV_ABI KeyboardUpdate(const void* handler);

} // namespace Lumen::Libs::Ime

#endif // LUMEN_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_IME_H_
