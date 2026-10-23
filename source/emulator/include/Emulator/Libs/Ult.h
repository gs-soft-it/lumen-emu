#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_ULT_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_ULT_H_

#include "Emulator/Common.h"

#include <cstdint>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Ult {

// Process-latched local experiment selector. This is deliberately not a
// statement about the console's ULT contract.
[[nodiscard]] bool ExperimentalEnabled();

// Clears only the process-scoped local initialization bit. The selector stays
// latched for the lifetime of the process.
void ResetSessionState();

int32_t LUMEN_SYSV_ABI Initialize();

} // namespace Lumen::Libs::Ult

#endif // LUMEN_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_LIBS_ULT_H_
