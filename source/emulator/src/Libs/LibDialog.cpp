#include "Emulator/Common.h"
#include "Emulator/Dialog.h"
#include "Emulator/Libs/ImeDialog.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdlib>
#include <initializer_list>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

namespace {

[[nodiscard]] bool NpCommerceDialogOfflineExperimentEnabled()
{
	static const bool enabled = [] {
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
#else
		return false;
#endif
	}();

	return enabled;
}

} // namespace

namespace LibCommonDialog {

LIB_VERSION("CommonDialog", 1, "CommonDialog", 1, 1);

namespace CommonDialog = Dialog::CommonDialog;

LIB_DEFINE(InitDialog_1_CommonDialog)
{
	LIB_FUNC("uoUpLGNkygk", CommonDialog::CommonDialogInitialize);
}

} // namespace LibCommonDialog

namespace LibImeDialog {

template <auto Function>
void RegisterImeDialogFunction(Loader::SymbolDatabase* symbols, const char* nid, const char32_t* name)
{
	constexpr auto metadata = Loader::MakeHostCallScalarMetadata<Function>();
	const auto target = reinterpret_cast<uint64_t>(Function);
	EXIT_IF(!symbols->RegisterHostCallTarget(target));
	EXIT_IF(!symbols->SetHostCallScalarMetadata(target, metadata));
	for (const char* library: {"ImeDialog", "libSceImeDialog"})
	for (const char* module: {"ImeDialog", "libSceImeDialog"})
	{
		Loader::SymbolResolve symbol {};
		symbol.name = nid; symbol.library = library; symbol.library_version = 1;
		symbol.module = module; symbol.module_version_major = symbol.module_version_minor = 1;
		symbol.type = Loader::SymbolType::Func;
		symbols->AddExact(symbol, target, name);
	}
}

LIB_DEFINE(InitDialog_1_ImeDialog)
{
	namespace Ime = Dialog::ImeDialog;
	RegisterImeDialogFunction<&Ime::ImeDialogInit>(s, "NUeBrN7hzf0", U"ImeDialog::Init");
	RegisterImeDialogFunction<&Ime::ImeDialogGetStatus>(s, "IADmD4tScBY", U"ImeDialog::GetStatus");
	RegisterImeDialogFunction<&Ime::ImeDialogTerm>(s, "gyTyVn+bXMw", U"ImeDialog::Term");
	RegisterImeDialogFunction<&Ime::ImeDialogGetResult>(s, "x01jxu+vxlc", U"ImeDialog::GetResult");
	RegisterImeDialogFunction<&Ime::ImeDialogAbort>(s, "oBmw4xrmfKs", U"ImeDialog::Abort");
	RegisterImeDialogFunction<&Ime::ImeDialogGetPanelSize>(s, "wqsJvRXwl58", U"ImeDialog::GetPanelSize");
	RegisterImeDialogFunction<&Ime::ImeDialogGetPanelSizeExtended>(s, "CRD+jSErEJQ", U"ImeDialog::GetPanelSizeExtended");
	RegisterImeDialogFunction<&Ime::ImeDialogGetPanelPositionAndForm>(s, "8jqzzPioYl8", U"ImeDialog::GetPanelPositionAndForm");
}

} // namespace LibImeDialog

namespace LibErrorDialog {

LIB_VERSION("ErrorDialog", 1, "ErrorDialog", 1, 1);

namespace ErrorDialog = Dialog::ErrorDialog;

LIB_DEFINE(InitDialog_1_ErrorDialog)
{
	LIB_FUNC("WWiGuh9XfgQ", ErrorDialog::ErrorDialogUpdateStatus);
}

} // namespace LibErrorDialog

namespace LibNpCommerce {

LIB_VERSION("libSceNpCommerce", 1, "libSceNpCommerce", 1, 1);

namespace NpCommerceDialog = Dialog::NpCommerceDialog;

LIB_DEFINE(InitDialog_1_NpCommerce)
{
	Loader::SymbolResolve sr {};
	sr.name                  = "LR5cwFMMCVE";
	sr.library               = g_library;
	sr.library_version       = g_library_version;
	sr.module                = g_module;
	sr.module_version_major  = g_module_version_major;
	sr.module_version_minor  = g_module_version_minor;
	sr.type                  = Loader::SymbolType::Func;
	const auto func          = reinterpret_cast<uint64_t>(NpCommerceDialog::NpCommerceDialogUpdateStatus);
	const char32_t* dbg_name = U"NpCommerceDialog::NpCommerceDialogUpdateStatus";
	if (!s->RegisterHostCallTarget(func))
	{
		EXIT("Unable to register host-call target for %s", "LR5cwFMMCVE");
		return;
	}
	s->AddExact(sr, func, dbg_name);
}

} // namespace LibNpCommerce

namespace LibSaveDataDialog {

LIB_VERSION("SaveDataDialog", 1, "SaveDataDialog", 1, 1);

namespace SaveDataDialog = Dialog::SaveDataDialog;

LIB_DEFINE(InitDialog_1_SaveDataDialog)
{
	// Core dialog functions
	LIB_FUNC("s9e3+YpRnzw", SaveDataDialog::SaveDataDialogInitialize);
	LIB_FUNC("4tPhsP6FpDI", SaveDataDialog::SaveDataDialogOpen);
	LIB_FUNC("ERKzksauAJA", SaveDataDialog::SaveDataDialogGetStatus);
	LIB_FUNC("yEiJ-qqr6Cg", SaveDataDialog::SaveDataDialogGetResult);
	LIB_FUNC("KK3Bdg1RWK0", SaveDataDialog::SaveDataDialogUpdateStatus);
	LIB_FUNC("YuH2FA7azqQ", SaveDataDialog::SaveDataDialogTerminate);
	// Progress bar functions
	LIB_FUNC("hay1CfTmLyA", SaveDataDialog::SaveDataDialogProgressBarSetValue);
	LIB_FUNC("V-uEeFKARJU", SaveDataDialog::SaveDataDialogProgressBarInc);
	// Close function  
	LIB_FUNC("fH46Lag88XY", SaveDataDialog::SaveDataDialogClose);
}

} // namespace LibSaveDataDialog

namespace LibMsgDialog {

LIB_VERSION("MsgDialog", 1, "MsgDialog", 1, 1);

namespace MsgDialog = Dialog::MsgDialog;

LIB_DEFINE(InitDialog_1_MsgDialog)
{
	LIB_FUNC("lDqxaY1UbEo", MsgDialog::MsgDialogInitialize);
	LIB_FUNC("b06Hh0DPEaE", MsgDialog::MsgDialogOpen);
	LIB_FUNC("CWVW78Qc3fI", MsgDialog::MsgDialogGetStatus);
	LIB_FUNC("Lr8ovHH9l6A", MsgDialog::MsgDialogGetResult);
	LIB_FUNC("6fIC3XKt2k0", MsgDialog::MsgDialogUpdateStatus);
	LIB_FUNC("ePw-kqZmelo", MsgDialog::MsgDialogTerminate);
	LIB_FUNC("HTrcDKlFKuM", MsgDialog::MsgDialogClose);
	LIB_FUNC("wTpfglkmv34", MsgDialog::MsgDialogProgressBarSetValue);
	LIB_FUNC("Gc5k1qcK4fs", MsgDialog::MsgDialogProgressBarInc);
	LIB_FUNC("6H-71OdrpXM", MsgDialog::MsgDialogProgressBarSetMsg);
}

} // namespace LibMsgDialog

LIB_DEFINE(InitDialog_1)
{
	LibCommonDialog::InitDialog_1_CommonDialog(s);
	LibImeDialog::InitDialog_1_ImeDialog(s);
	LibErrorDialog::InitDialog_1_ErrorDialog(s);
	if (NpCommerceDialogOfflineExperimentEnabled())
	{
		LibNpCommerce::InitDialog_1_NpCommerce(s);
	}
	LibSaveDataDialog::InitDialog_1_SaveDataDialog(s);
	LibMsgDialog::InitDialog_1_MsgDialog(s);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
