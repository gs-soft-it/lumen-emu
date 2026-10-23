#include "Emulator/Common.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

namespace LibGen4 {

LIB_VERSION("VideoOut", 1, "VideoOut", 0, 0);

LIB_DEFINE(InitVideoOut_1)
{
	PRINT_NAME_ENABLE(true);

	LIB_FUNC_SCALAR("Up36PTk687E", VideoOut::VideoOutOpen);
	if (VideoOut::VideoOutModeInitEnabled())
	{
		LIB_FUNC_SCALAR("pjkDsgxli6c", VideoOut::VideoOutModeSetAny);
	}
	LIB_FUNC_SCALAR("uquVH4-Du78", VideoOut::VideoOutClose);
	LIB_FUNC_SCALAR("kGVLc3htQE8", VideoOut::VideoOutGetDeviceCapabilityInfo);
	LIB_FUNC_SCALAR("zgXifHT9ErY", VideoOut::VideoOutIsFlipPending);
	LIB_FUNC_SCALAR("N1bEoJ4SRw4", VideoOut::VideoOutConfigureOutputMode);
	LIB_FUNC_SCALAR("6kPnj51T62Y", VideoOut::VideoOutGetResolutionStatus);
	LIB_FUNC_SCALAR("i6-sR91Wt-4", VideoOut::VideoOutSetBufferAttribute);
	LIB_FUNC_SCALAR("CBiu4mCE1DA", VideoOut::VideoOutSetFlipRate);
	LIB_FUNC_SCALAR("HXzjK9yI30k", VideoOut::VideoOutAddFlipEvent);
	LIB_FUNC_SCALAR("Xru92wHJRmg", VideoOut::VideoOutAddVblankEvent);
	if (VideoOut::VideoOutEventDataEnabled())
	{
		LIB_FUNC_SCALAR("rWUTcKdkUzQ", VideoOut::VideoOutGetEventData);
	}
	LIB_FUNC_SCALAR("w3BY+tAEiQY", VideoOut::VideoOutRegisterBuffers);
	LIB_FUNC_SCALAR("U46NwOiJpys", VideoOut::VideoOutSubmitFlip);
	LIB_FUNC_SCALAR("SbU3dwp80lQ", VideoOut::VideoOutGetFlipStatus);
	LIB_FUNC_SCALAR("1FZBKy8HeNU", VideoOut::VideoOutGetVblankStatus);
	LIB_FUNC_SCALAR("MTxxrOCeSig", VideoOut::VideoOutSetWindowModeMargins);
#if !defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && \
     defined(LUMEN_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
	LIB_FUNC_SCALAR("DYhhWbJSeRg", VideoOut::VideoOutColorSettingsSetGamma);
	LIB_FUNC_SCALAR("pv9CI5VC+R0", VideoOut::VideoOutAdjustColor);
#endif
}

} // namespace LibGen4

LIB_DEFINE(InitVideoOut_1)
{
	LibGen4::InitVideoOut_1(s);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
