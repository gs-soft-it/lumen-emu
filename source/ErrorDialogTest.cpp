#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Dialog.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace {

namespace Dialog = Lumen::Libs::Dialog;
namespace Error = Dialog::ErrorDialog;
namespace Msg = Dialog::MsgDialog;
namespace Save = Dialog::SaveDataDialog;
using ErrorPoll = uint32_t (LUMEN_SYSV_ABI *)();
static_assert(sizeof(uint32_t) == 4);
static_assert(std::is_same_v<decltype(&Error::ErrorDialogUpdateStatus), ErrorPoll>);

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "ErrorDialogTest: %s\n", message);
	return condition;
}

Lumen::Loader::SymbolResolve Exact(const char* nid, const char* library = "libSceErrorDialog")
{
	Lumen::Loader::SymbolResolve query {};
	query.name = nid;
	query.library = library;
	query.library_version = 1;
	query.module = library;
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Lumen::Loader::SymbolType::Func;
	return query;
}

bool TestNegativeIdentity(const Lumen::Loader::SymbolDatabase& symbols)
{
	const auto exact = Exact("WWiGuh9XfgQ");
	constexpr int versions[] = {0, 2};
	for (int version : versions)
	{
		auto wrong = exact;
		wrong.library_version = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong library version resolved")) return false;
		wrong = exact;
		wrong.module_version_major = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module major version resolved")) return false;
		wrong = exact;
		wrong.module_version_minor = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module minor version resolved")) return false;
	}
	constexpr const char* namespaces[] = {
	    "libSceCommonDialog", "libSceMsgDialog", "libSceSaveDataDialog", "libSceErrorDialogForToolkit", "unrelated_dialog_library"};
	for (const char* other : namespaces)
	{
		auto wrong = exact;
		wrong.library = other;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong library namespace resolved")) return false;
		wrong = exact;
		wrong.module = other;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong module namespace resolved")) return false;
		wrong.library = other;
		if (!Check(symbols.Find(wrong) == nullptr, "another dialog gained the ErrorDialog NID")) return false;
	}
	constexpr Lumen::Loader::SymbolType types[] = {
	    Lumen::Loader::SymbolType::Unknown, Lumen::Loader::SymbolType::Object,
	    Lumen::Loader::SymbolType::TlsModule, Lumen::Loader::SymbolType::NoType};
	for (auto type : types)
	{
		auto wrong = exact;
		wrong.type = type;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong symbol type resolved")) return false;
	}
	constexpr const char* missing_nids[] = {"I88KChlynSs", "9XAxK2PMwk8", "M2ZF-ClLhgY", "unknown-error-dialog-nid"};
	for (const char* nid : missing_nids)
		if (!Check(symbols.Find(Exact(nid)) == nullptr, "unimplemented ErrorDialog export resolved")) return false;
	constexpr const char* names[] = {
	    "sceErrorDialogInitialize", "sceErrorDialogTerminate", "sceErrorDialogOpen", "sceErrorDialogUpdateStatus"};
	for (const char* name : names)
		if (!Check(symbols.Find(Exact(name)) == nullptr, "unencoded ErrorDialog name resolved")) return false;
	return Check(symbols.StdNameMapSize() == 0 &&
	             symbols.FindByStdName(U"WWiGuh9XfgQ") == nullptr &&
	             symbols.FindByStdName(U"I88KChlynSs") == nullptr &&
	             symbols.FindByStdName(U"9XAxK2PMwk8") == nullptr &&
	             symbols.FindByStdName(U"M2ZF-ClLhgY") == nullptr &&
	             symbols.FindByStdName(U"sceErrorDialogUpdateStatus") == nullptr &&
	             symbols.FindByStdName(U"sceErrorDialogInitialize") == nullptr &&
	             symbols.FindByStdName(U"sceErrorDialogTerminate") == nullptr &&
	             symbols.FindByStdName(U"sceErrorDialogOpen") == nullptr,
	             "Dialog registration gained a standard-name fallback");
}

bool TestBindings(ErrorPoll& poll)
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libDialog_1", &symbols), "Dialog registration failed")) return false;
	const auto* entry = symbols.Find(Exact("WWiGuh9XfgQ"));
	if (!Check(entry != nullptr && entry->vaddr == reinterpret_cast<uint64_t>(&Error::ErrorDialogUpdateStatus) &&
	           entry->dbg_name == U"ErrorDialog::ErrorDialogUpdateStatus", "ErrorDialog identity or target mismatch")) return false;
	poll = reinterpret_cast<ErrorPoll>(entry->vaddr);

	struct Binding { const char* library; const char* nid; uint64_t address; };
	const Binding existing[] = {
	    {"libSceCommonDialog", "uoUpLGNkygk", reinterpret_cast<uint64_t>(&Dialog::CommonDialog::CommonDialogInitialize)},
	    {"libSceSaveDataDialog", "s9e3+YpRnzw", reinterpret_cast<uint64_t>(&Save::SaveDataDialogInitialize)},
	    {"libSceSaveDataDialog", "4tPhsP6FpDI", reinterpret_cast<uint64_t>(&Save::SaveDataDialogOpen)},
	    {"libSceSaveDataDialog", "ERKzksauAJA", reinterpret_cast<uint64_t>(&Save::SaveDataDialogGetStatus)},
	    {"libSceSaveDataDialog", "yEiJ-qqr6Cg", reinterpret_cast<uint64_t>(&Save::SaveDataDialogGetResult)},
	    {"libSceSaveDataDialog", "KK3Bdg1RWK0", reinterpret_cast<uint64_t>(&Save::SaveDataDialogUpdateStatus)},
	    {"libSceSaveDataDialog", "YuH2FA7azqQ", reinterpret_cast<uint64_t>(&Save::SaveDataDialogTerminate)},
	    {"libSceSaveDataDialog", "hay1CfTmLyA", reinterpret_cast<uint64_t>(&Save::SaveDataDialogProgressBarSetValue)},
	    {"libSceSaveDataDialog", "V-uEeFKARJU", reinterpret_cast<uint64_t>(&Save::SaveDataDialogProgressBarInc)},
	    {"libSceSaveDataDialog", "fH46Lag88XY", reinterpret_cast<uint64_t>(&Save::SaveDataDialogClose)},
	    {"libSceMsgDialog", "lDqxaY1UbEo", reinterpret_cast<uint64_t>(&Msg::MsgDialogInitialize)},
	    {"libSceMsgDialog", "b06Hh0DPEaE", reinterpret_cast<uint64_t>(&Msg::MsgDialogOpen)},
	    {"libSceMsgDialog", "CWVW78Qc3fI", reinterpret_cast<uint64_t>(&Msg::MsgDialogGetStatus)},
	    {"libSceMsgDialog", "Lr8ovHH9l6A", reinterpret_cast<uint64_t>(&Msg::MsgDialogGetResult)},
	    {"libSceMsgDialog", "6fIC3XKt2k0", reinterpret_cast<uint64_t>(&Msg::MsgDialogUpdateStatus)},
	    {"libSceMsgDialog", "ePw-kqZmelo", reinterpret_cast<uint64_t>(&Msg::MsgDialogTerminate)},
	    {"libSceMsgDialog", "HTrcDKlFKuM", reinterpret_cast<uint64_t>(&Msg::MsgDialogClose)},
	    {"libSceMsgDialog", "wTpfglkmv34", reinterpret_cast<uint64_t>(&Msg::MsgDialogProgressBarSetValue)},
	    {"libSceMsgDialog", "Gc5k1qcK4fs", reinterpret_cast<uint64_t>(&Msg::MsgDialogProgressBarInc)},
	    {"libSceMsgDialog", "6H-71OdrpXM", reinterpret_cast<uint64_t>(&Msg::MsgDialogProgressBarSetMsg)}};
	for (const auto& binding : existing)
	{
		const auto* found = symbols.Find(Exact(binding.nid, binding.library));
		if (!Check(found != nullptr && found->vaddr == binding.address, "existing Dialog binding changed")) return false;
	}
	return Check(symbols.HostCallTargetCount() == 21, "unexpected Dialog host-call target count") && TestNegativeIdentity(symbols);
}

bool TestIndependentStates(ErrorPoll poll)
{
	// These transitions describe the existing Lumen Msg/SaveData services.
	// They do not assert console transitions or initialize an ErrorDialog.
	const auto observe = [poll](int msg, int save) {
		if (!Check(Msg::MsgDialogUpdateStatus() == msg && Save::SaveDataDialogUpdateStatus() == save,
		           "existing dialog fixture did not reach its expected state")) return false;
		if (!Check(poll() == 0u, "absent ErrorDialog did not report local NONE")) return false;
		return Check(Msg::MsgDialogUpdateStatus() == msg && Save::SaveDataDialogUpdateStatus() == save,
		             "ErrorDialog polling changed another dialog's state");
	};
	if (!observe(0, 0)) return false;
	if (!Check(Dialog::CommonDialog::CommonDialogInitialize() == 0, "CommonDialog initialization failed") || !observe(0, 0)) return false;
	if (!Check(Msg::MsgDialogInitialize() == 0, "MsgDialog initialization failed") || !observe(1, 0)) return false;
	if (!Check(Save::SaveDataDialogInitialize() == 0, "SaveDataDialog initialization failed") || !observe(1, 1)) return false;
	if (!Check(Msg::MsgDialogOpen(nullptr) == 0, "MsgDialog open failed") || !observe(3, 1)) return false;
	if (!Check(Save::SaveDataDialogOpen(nullptr) == 0, "SaveDataDialog open failed") || !observe(3, 3)) return false;
	if (!Check(Msg::MsgDialogClose() == 0, "MsgDialog close failed") || !observe(0, 3)) return false;
	if (!Check(Save::SaveDataDialogClose() == 0, "SaveDataDialog close failed") || !observe(0, 0)) return false;
	if (!Check(Save::SaveDataDialogInitialize() == 0, "SaveDataDialog reinitialization failed") || !observe(0, 1)) return false;
	if (!Check(Save::SaveDataDialogOpen(nullptr) == 0, "SaveDataDialog reopen failed") || !observe(0, 3)) return false;
	if (!Check(Msg::MsgDialogInitialize() == 0, "MsgDialog reinitialization failed") || !observe(1, 3)) return false;
	if (!Check(Save::SaveDataDialogTerminate() == 0, "SaveDataDialog termination failed") || !observe(1, 0)) return false;
	return Check(Msg::MsgDialogTerminate() == 0, "MsgDialog termination failed") && observe(0, 0);
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	ErrorPoll poll = nullptr;
	if (!TestBindings(poll) || !TestIndependentStates(poll)) return 1;
	std::puts("ErrorDialogTest: PASS");
	return 0;
}
