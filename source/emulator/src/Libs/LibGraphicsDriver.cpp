#include "Emulator/Common.h"
#include "Emulator/Graphics/Graphics.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

namespace LibGen4 {

LIB_VERSION("GraphicsDriver", 1, "GraphicsDriver", 1, 1);

namespace Gen4 = Graphics::Gen4;

LIB_DEFINE(InitGraphicsDriver_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC_SCALAR("5udAm+6boVg", Gen4::GraphicsCreateWorkloadStream);
	LIB_FUNC_SCALAR("UtObDRQiGbs", Gen4::GraphicsDestroyWorkloadStream);
	LIB_FUNC_SCALAR("ihxrbsoSKWc", Gen4::GraphicsBeginWorkload);
	LIB_FUNC_SCALAR("Fa3x75OOLRA", Gen4::GraphicsEndWorkload);
	LIB_FUNC_SCALAR("jRcI8VcgTz4", Gen4::GraphicsSubmitCommandBuffersForWorkload);
	LIB_FUNC_SCALAR("Ga6r7H6Y0RI", Gen4::GraphicsSubmitAndFlipCommandBuffersForWorkload);
	LIB_FUNC_SCALAR("byXlqupd8cE", Gen4::GraphicsDingDong);

	LIB_FUNC("gAhCn6UiU4Y", Gen4::GraphicsSetVsShader);
	LIB_FUNC("V31V01UiScY", Gen4::GraphicsUpdateVsShader);
	LIB_FUNC("bQVd5YzCal0", Gen4::GraphicsSetPsShader);
	LIB_FUNC("5uFKckiJYRM", Gen4::GraphicsSetPsShader350);
	LIB_FUNC("4MgRw-bVNQU", Gen4::GraphicsUpdatePsShader);
	LIB_FUNC("mLVL7N7BVBg", Gen4::GraphicsUpdatePsShader350);
	LIB_FUNC("KXltnCwEJHQ", Gen4::GraphicsSetCsShader);
	LIB_FUNC("Kx-h-nWQJ8A", Gen4::GraphicsSetCsShaderWithModifier);
	LIB_FUNC("HlTPoZ-oY7Y", Gen4::GraphicsDrawIndex);
	LIB_FUNC("GGsn7jMTxw4", Gen4::GraphicsDrawIndexAuto);
	LIB_FUNC("zwY0YV91TTI", Gen4::GraphicsSubmitCommandBuffers);
	LIB_FUNC_GP_ARITY("xbxNatawohc", Gen4::GraphicsSubmitAndFlipCommandBuffers, 9);
	LIB_FUNC("yvZ73uQUqrk", Gen4::GraphicsSubmitDone);
	LIB_FUNC("b08AgtPlHPg", Gen4::GraphicsAreSubmitsAllowed);
	LIB_FUNC("iBt3Oe00Kvc", Gen4::GraphicsFlushMemory);
	LIB_FUNC("b0xyllnVY-I", Gen4::GraphicsAddEqEvent);
	LIB_FUNC("PVT+fuoS9gU", Gen4::GraphicsDeleteEqEvent);
	LIB_FUNC("Idffwf3yh8s", Gen4::GraphicsDrawInitDefaultHardwareState);
	LIB_FUNC("QhnyReteJ1M", Gen4::GraphicsDrawInitDefaultHardwareState175);
	LIB_FUNC("0H2vBYbTLHI", Gen4::GraphicsDrawInitDefaultHardwareState200);
	LIB_FUNC("yb2cRhagD1I", Gen4::GraphicsDrawInitDefaultHardwareState350);
	LIB_FUNC("nF6bFRUBRAU", Gen4::GraphicsDispatchInitDefaultHardwareState);
	LIB_FUNC("1qXLHIpROPE", Gen4::GraphicsInsertWaitFlipDone);
	LIB_FUNC("0BzLGljcwBo", Gen4::GraphicsDispatchDirect);
	LIB_FUNC("29oKvKXzEZo", Gen4::GraphicsMapComputeQueue);
	LIB_FUNC("ArSg-TGinhk", Gen4::GraphicsUnmapComputeQueue);
	LIB_FUNC("ffrNQOshows", Gen4::GraphicsComputeWaitOnAddress);
	LIB_FUNC("bX5IbRvECXk", Gen4::GraphicsDingDong);
	LIB_FUNC("W1Etj-jlW7Y", Gen4::GraphicsInsertPushMarker);
	LIB_FUNC("7qZVNgEu+SY", Gen4::GraphicsInsertPopMarker);
	LIB_FUNC_SCALAR("jiItzS6+22g", Gen4::GraphicsInsertSetMarker);
	LIB_FUNC_SCALAR("cFCp0NX8wf0", Gen4::GraphicsSetVgtControl);
	LIB_FUNC("+AFvOEXrKJk", Gen4::GraphicsSetEmbeddedVsShader);
	LIB_FUNC_SCALAR("X9Omw9dwv5M", Gen4::GraphicsSetEmbeddedPsShader);
	LIB_FUNC("ZFqKFl23aMc", Gen4::GraphicsRegisterOwner);
	LIB_FUNC("nvEwfYAImTs", Gen4::GraphicsRegisterResource);
	LIB_FUNC_SCALAR("yhFCnaz5daw", Gen4::GraphicsUnregisterAllResourcesForOwner);
	LIB_FUNC_SCALAR("fhKwCVVj9nk", Gen4::GraphicsUnregisterOwnerAndResources);
	LIB_FUNC_SCALAR("k8EXkhIP+lM", Gen4::GraphicsUnregisterResource);
	LIB_FUNC("Fwvh++m9IQI", Gen4::GraphicsGetGpuCoreClockFrequency);
	LIB_FUNC("jg33rEKLfVs", Gen4::GraphicsIsUserPaEnabled);
	LIB_FUNC("ln33zjBrfjk", Gen4::GraphicsGetTheTessellationFactorRingBufferBaseAddress);
}

} // namespace LibGen4

LIB_DEFINE(InitGraphicsDriver_1)
{
	LibGen4::InitGraphicsDriver_1(s);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
