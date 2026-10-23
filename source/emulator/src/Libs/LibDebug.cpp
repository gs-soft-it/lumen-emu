#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

namespace LibRazorCpu {

LIB_VERSION("RazorCpu", 1, "RazorCpu", 1, 1);

static LUMEN_SYSV_ABI uint32_t RazorCpuIsCapturing()
{
	PRINT_NAME();

	return 0;
}

LIB_DEFINE(InitLibRazorCpu_1)
{
	LIB_FUNC("EboejOQvLL4", LibRazorCpu::RazorCpuIsCapturing);
}

} // namespace LibRazorCpu

LIB_DEFINE(InitDebug_1)
{
	LibRazorCpu::InitLibRazorCpu_1(s);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
