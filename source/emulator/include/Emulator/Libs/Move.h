#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_MOVE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_MOVE_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Move {

// Process-latched local compatibility selector for the no-device Move profile.
// The historical name still gates the whole profile (Initialize, Open,
// GetDeviceInfo and ReadStateRecent); it does not claim a console Move
// implementation and exposes no host motion-controller device.
[[nodiscard]] bool ExperimentalEnabled();

// Reopens Lumen's supported per-session budget after guest execution has
// stopped: the Initialize latch and the local handle allocation restart. The
// selector remains latched for the lifetime of the process.
void ResetSessionState();

// Measured console profile (campaign LMNQ92804, CUH-1216A fw 12.00, no Move
// device connected): a first call returns 0 and a repeat returns 0x80ee0002.
int32_t LUMEN_SYSV_ABI Initialize();

// Measured console profile of sceMoveOpen on the same campaigns: only the
// modeled local offline user (id 1) is admitted and the call returns the
// handle as its signed32 result, writing no guest memory; every other user
// receives the measured 0x80ee0006 without writes. A second concurrent open
// while a handle is live receives the measured 0x80ee0007 without consuming
// a handle (campaign LMNQ92805). The remaining arguments were not rejected in
// the measured domain (type 0..3, index 0..1, any fourth word, null or
// non-null trailing pointer) and are neither read nor written.
int32_t LUMEN_SYSV_ABI Open(int32_t userId, int32_t type, int32_t index, int32_t reserved, const void* param);

// Measured console profile of sceMoveGetDeviceInfo (campaign LMNQ92805): on a
// live handle with no Move device connected the call returns 1 and writes no
// guest memory; a null output receives 0x80ee0003 and any other non-live
// handle receives 0x80ee0004, also without writes. The model publishes no
// device record and never reads or writes the output window.
int32_t LUMEN_SYSV_ABI GetDeviceInfo(int32_t handle, const void* info);

// Measured console profile of sceMoveReadStateRecent (campaign LMNQ92815): on
// a live handle with no Move device connected the call returns 1 and writes
// no guest memory for the measured timestamps (0 and 1000000); a null count
// output receives 0x80ee0003 without writes; the handles 0, -1 and a closed
// handle receive 0x80ee0004 and the refusal stores four zero bytes into the
// count output. The data pointer and the timestamp are never read or written
// by the model; the null-count check precedes the handle check for the
// unmeasured combination.
int32_t LUMEN_SYSV_ABI ReadStateRecent(int32_t handle, int64_t timestamp, const void* data, int32_t* num);

} // namespace Lumen::Libs::Move

#endif // LUMEN_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_MOVE_H_
