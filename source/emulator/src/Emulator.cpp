#include "Emulator/Emulator.h"

#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"
#include "Emulator/StartupTrace.h"

namespace Lumen::Emulator {

#ifdef LUMEN_EMU_ENABLED
void lumen_reg();
#endif

LUMEN_SUBSYSTEM_INIT(Emulator)
{
#ifdef LUMEN_EMU_ENABLED
	lumen_reg();
#endif
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Emulator)
{
	// Fatal exits bypass automatic destructors. This subsystem hook runs from
	// Core's fail-closed shutdown path and preserves the bounded startup epoch
	// when failure occurs before a successful first present.
	StartupTrace::Flush();
}

LUMEN_SUBSYSTEM_DESTROY(Emulator) {}

} // namespace Lumen::Emulator
