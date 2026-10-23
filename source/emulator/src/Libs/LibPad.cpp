#include "Emulator/Common.h"
#include "Emulator/Controller.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("Pad", 1, "Pad", 1, 1);

LIB_DEFINE(InitPad_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("hv1luiJrqQM", Controller::PadInit);
	LIB_FUNC_SCALAR("xk0AcarP3V4", Controller::PadOpen);
	LIB_FUNC("clVvL4ZDntw", Controller::PadSetMotionSensorState);
	LIB_FUNC_SCALAR("gjP9-KQzoUk", Controller::PadGetControllerInformation);
	LIB_FUNC_SCALAR("AcslpN1jHR8", Controller::PadDeviceClassGetExtendedInformation);
	LIB_FUNC("YndgXqQVV7c", Controller::PadReadState);
	LIB_FUNC("q1cHNfGycLI", Controller::PadRead);
	LIB_FUNC("yFVnOdGxvZY", Controller::PadSetVibration);
	LIB_FUNC("DscD1i9HX1w", Controller::PadResetLightBar);
	LIB_FUNC("RR4novUEENY", Controller::PadSetLightBar);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
