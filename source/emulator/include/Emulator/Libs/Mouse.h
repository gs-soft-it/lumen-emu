#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_MOUSE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_MOUSE_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Mouse {

// Process-latched local compatibility selector gating the whole local Mouse
// profile. It does not inspect or connect host input devices.
[[nodiscard]] bool ExperimentalEnabled();

// Clears only the bounded state (diagnostic counter, live index slots, handle
// allocation and the read counter) after guest execution has stopped. The
// selector remains latched and no console Mouse lifecycle is modeled.
void ResetSessionState();

int32_t LUMEN_SYSV_ABI Initialize();

// Measured no-device Open profile (campaign LMNQ92806, 28 September 2026):
// one local offline user (id 1), the two measured index slots, one local
// handle per live slot, the measured errors reused unchanged and the trailing
// parameter never read. The interface document declares the local choices for
// the unmeasured combinations.
int32_t LUMEN_SYSV_ABI Open(int32_t userId, int32_t type, int32_t index, const void* param);

// Measured no-device Read profile (campaigns LMNQ92816/LMNQ92817, 29 September
// 2026): a live handle with num in 1..64 returns 1 and stores the forty zero
// bytes of the device-less record; a null data pointer and a num outside
// 1..64 receive the measured invalid-argument code without writes; every
// handle that is not a live slot receives the measured invalid-handle code
// without writes. The interface document declares the argument-before-handle
// order for the unmeasured combined cases.
int32_t LUMEN_SYSV_ABI Read(int32_t handle, void* data, int32_t num);

} // namespace Lumen::Libs::Mouse

#endif // LUMEN_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_MOUSE_H_
