#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "Lumen/Core/Subsystems.h"

namespace Lumen::Emulator {

LUMEN_SUBSYSTEM_DEFINE(Emulator);

#if LUMEN_PLATFORM == LUMEN_PLATFORM_IOS
// Ends the current guest session without destroying process-owned emulator
// services. Returns true only when every guest pointer/resource was purged.
[[nodiscard]] bool IOSRuntimeResetForNextSession();
#endif

} // namespace Lumen::Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
