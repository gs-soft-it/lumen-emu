#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Dialog.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <type_traits>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

namespace Dialog = Lumen::Libs::Dialog;
namespace Commerce = Dialog::NpCommerceDialog;
namespace Error = Dialog::ErrorDialog;
namespace Msg = Dialog::MsgDialog;
namespace Save = Dialog::SaveDataDialog;

using CommercePoll = int32_t (LUMEN_SYSV_ABI*)();
static_assert(sizeof(int32_t) == 4);
static_assert(std::is_signed_v<int32_t>);
static_assert(std::is_same_v<decltype(&Commerce::NpCommerceDialogUpdateStatus), CommercePoll>);

constexpr char kSelector[] = "LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE";
constexpr char kMarkerPrefix[] = "[NP_COMMERCE_DIALOG_OFFLINE_EXPERIMENT]";
constexpr char kMarker[] =
    "[NP_COMMERCE_DIALOG_OFFLINE_EXPERIMENT] operation=update-status result=none-local value=0 experimental=1 "
    "console_contract=unverified dialog=absent ui=0 network=0 purchases=0 entitlements=0\n";

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "NpCommerceDialogTest: %s\n", message);
	return condition;
}

int DuplicateDescriptor(int descriptor)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	return ::_dup(descriptor);
#else
	return ::dup(descriptor);
#endif
}

int RedirectDescriptor(int source, int destination)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	return ::_dup2(source, destination);
#else
	return ::dup2(source, destination);
#endif
}

int CloseDescriptor(int descriptor)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	return ::_close(descriptor);
#else
	return ::close(descriptor);
#endif
}

int FileDescriptor(std::FILE* file)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	return ::_fileno(file);
#else
	return ::fileno(file);
#endif
}

bool SetEnvironmentValue(const char* name, const char* value)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	return ::_putenv_s(name, value) == 0;
#else
	return ::setenv(name, value, 1) == 0;
#endif
}

Lumen::Loader::SymbolResolve Exact(const char* nid, const char* library)
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

Lumen::Loader::SymbolResolve CommerceIdentity(const char* nid = "LR5cwFMMCVE")
{
	return Exact(nid, "libSceNpCommerce");
}

template <typename Operation>
bool CaptureStderr(std::string& output, Operation operation)
{
	output.clear();
	std::fflush(stderr);
	std::FILE* capture = std::tmpfile();
	if (!Check(capture != nullptr, "could not create a local stderr capture file")) return false;

	const int stderr_descriptor = FileDescriptor(stderr);
	const int saved_stderr = DuplicateDescriptor(stderr_descriptor);
	if (saved_stderr < 0)
	{
		std::fclose(capture);
		return Check(false, "could not duplicate stderr");
	}
	if (RedirectDescriptor(FileDescriptor(capture), stderr_descriptor) < 0)
	{
		CloseDescriptor(saved_stderr);
		std::fclose(capture);
		return Check(false, "could not redirect stderr");
	}

	operation();
	std::fflush(stderr);
	const bool restored = RedirectDescriptor(saved_stderr, stderr_descriptor) >= 0;
	CloseDescriptor(saved_stderr);
	if (!restored)
	{
		std::fclose(capture);
		return false;
	}
	if (std::fseek(capture, 0, SEEK_SET) != 0)
	{
		std::fclose(capture);
		return Check(false, "could not rewind stderr capture");
	}

	std::array<char, 256> buffer {};
	for (;;)
	{
		const std::size_t size = std::fread(buffer.data(), 1, buffer.size(), capture);
		output.append(buffer.data(), size);
		if (size != buffer.size()) break;
	}
	const bool read_ok = std::ferror(capture) == 0;
	std::fclose(capture);
	return Check(read_ok, "could not read stderr capture");
}

bool InitializeDialogRegistry(Lumen::Loader::SymbolDatabase& symbols)
{
	bool initialized = false;
	std::string output;
	if (!CaptureStderr(output, [&] { initialized = Lumen::Libs::Init(U"libDialog_1", &symbols); })) return false;
	return Check(initialized, "Dialog registration failed") &&
	       Check(output.find(kMarker) == std::string::npos,
	             "Dialog registration emitted the exact NpCommerce marker before a poll") &&
	       Check(output.find(kMarkerPrefix) == std::string::npos,
	             "Dialog registration emitted an NpCommerce diagnostic before a poll");
}

CommercePoll FindCommercePoll(const Lumen::Loader::SymbolDatabase& symbols)
{
	const auto* entry = symbols.Find(CommerceIdentity());
	if (!Check(entry != nullptr, "exact NpCommerce binding is absent")) return nullptr;
	if (!Check(entry->vaddr == reinterpret_cast<uint64_t>(&Commerce::NpCommerceDialogUpdateStatus),
	           "exact NpCommerce binding has the wrong target")) return nullptr;
	return reinterpret_cast<CommercePoll>(entry->vaddr);
}

bool CheckBaselineBindings(const Lumen::Loader::SymbolDatabase& symbols)
{
	struct Binding
	{
		const char* library;
		const char* nid;
		uint64_t address;
	};
	const Binding baseline[] = {
	    {"libSceCommonDialog", "uoUpLGNkygk", reinterpret_cast<uint64_t>(&Dialog::CommonDialog::CommonDialogInitialize)},
	    {"libSceErrorDialog", "WWiGuh9XfgQ", reinterpret_cast<uint64_t>(&Error::ErrorDialogUpdateStatus)},
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
	    {"libSceMsgDialog", "6H-71OdrpXM", reinterpret_cast<uint64_t>(&Msg::MsgDialogProgressBarSetMsg)},
	};
	static_assert(std::size(baseline) == 21);
	for (const auto& binding : baseline)
	{
		const auto* found = symbols.Find(Exact(binding.nid, binding.library));
		if (!Check(found != nullptr && found->vaddr == binding.address,
		           "a baseline Dialog binding changed identity or target")) return false;
	}
	return true;
}

bool CheckCommercePresence(const Lumen::Loader::SymbolDatabase& symbols, bool enabled)
{
	const auto* entry = symbols.Find(CommerceIdentity());
	if (!enabled)
		return Check(entry == nullptr, "NpCommerce binding appeared while disabled") &&
		       Check(symbols.HostCallTargetCount() == 21, "disabled Dialog registry does not contain exactly 21 targets");
	return Check(entry != nullptr && entry->vaddr == reinterpret_cast<uint64_t>(&Commerce::NpCommerceDialogUpdateStatus),
	             "enabled NpCommerce binding identity or target mismatch") &&
	       Check(symbols.HostCallTargetCount() == 22, "enabled Dialog registry does not contain exactly 22 targets");
}

bool TestSignature()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!InitializeDialogRegistry(symbols)) return false;
	CommercePoll poll = &Commerce::NpCommerceDialogUpdateStatus;
	return Check(poll != nullptr && sizeof(decltype(poll())) == sizeof(int32_t),
	             "signed32 zero-argument ABI signature is unavailable");
}

bool TestSelectorCase(const char* expected_value, bool present, bool enabled)
{
	const char* actual = std::getenv(kSelector);
	if (present)
	{
		if (!Check(actual != nullptr && std::strcmp(actual, expected_value) == 0,
		           "CTest did not provide the requested selector value")) return false;
	}
	else if (!Check(actual == nullptr, "CTest did not remove the selector"))
	{
		return false;
	}

	Lumen::Loader::SymbolDatabase symbols;
	return InitializeDialogRegistry(symbols) && CheckBaselineBindings(symbols) && CheckCommercePresence(symbols, enabled);
}

bool CheckRepeatedPolls(CommercePoll poll)
{
	std::array<int32_t, 8> results {};
	std::string first_call_stderr;
	std::string later_calls_stderr;
	if (!CaptureStderr(first_call_stderr, [&] { results[0] = poll(); })) return false;
	if (!CaptureStderr(later_calls_stderr, [&] {
		for (std::size_t index = 1; index < results.size(); ++index) results[index] = poll();
	})) return false;
	for (int32_t result : results)
		if (!Check(result == int32_t {0}, "NpCommerce local-absence poll did not return signed32 zero")) return false;
	return Check(first_call_stderr == kMarker,
	             "the first NpCommerce poll did not emit exactly one exact marker") &&
	       Check(later_calls_stderr.empty(),
	             "a later NpCommerce poll re-emitted the once-per-process marker");
}

bool TestLatch(bool enabled)
{
	const char* initial = std::getenv(kSelector);
	const char* expected_initial = enabled ? "1" : "0";
	if (!Check(initial != nullptr && std::strcmp(initial, expected_initial) == 0,
	           "CTest provided the wrong initial latch selector")) return false;

	Lumen::Loader::SymbolDatabase first;
	if (!InitializeDialogRegistry(first) || !CheckCommercePresence(first, enabled)) return false;
	CommercePoll first_poll = enabled ? FindCommercePoll(first) : nullptr;
	if (enabled && first_poll == nullptr) return false;

	if (!Check(SetEnvironmentValue(kSelector, enabled ? "0" : "1"),
	           "could not change selector after first evaluation")) return false;
	Lumen::Loader::SymbolDatabase second;
	if (!InitializeDialogRegistry(second) || !CheckCommercePresence(second, enabled)) return false;
	if (!enabled) return true;

	CommercePoll second_poll = FindCommercePoll(second);
	if (second_poll == nullptr) return false;
	std::array<int32_t, 3> results {};
	std::string first_call_stderr;
	std::string later_calls_stderr;
	if (!CaptureStderr(first_call_stderr, [&] { results[0] = first_poll(); })) return false;
	if (!CaptureStderr(later_calls_stderr, [&] {
		results[1] = second_poll();
		results[2] = first_poll();
	})) return false;
	return Check(results[0] == 0 && results[1] == 0 && results[2] == 0,
	             "latched enabled poll did not remain signed32 zero") &&
	       Check(first_call_stderr == kMarker,
	             "the first enabled latch poll did not emit exactly one exact marker") &&
	       Check(later_calls_stderr.empty(),
	             "a later enabled latch poll re-emitted the once-per-process marker");
}

bool TestExactBinding()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!InitializeDialogRegistry(symbols)) return false;
	return FindCommercePoll(symbols) != nullptr && Check(symbols.HostCallTargetCount() == 22,
	                                                    "exact binding did not add one Dialog host-call target");
}

bool TestNegativeIdentity()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!InitializeDialogRegistry(symbols) || FindCommercePoll(symbols) == nullptr) return false;
	const auto exact = CommerceIdentity();
	constexpr int wrong_versions[] = {0, 2};
	for (int version : wrong_versions)
	{
		auto wrong = exact;
		wrong.library_version = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong NpCommerce library version resolved")) return false;
		wrong = exact;
		wrong.module_version_major = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong NpCommerce module major version resolved")) return false;
		wrong = exact;
		wrong.module_version_minor = version;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong NpCommerce module minor version resolved")) return false;
	}
	constexpr const char* other_namespaces[] = {
	    "NpCommerce", "libSceCommonDialog", "libSceErrorDialog", "libSceMsgDialog", "libSceSaveDataDialog"};
	for (const char* other : other_namespaces)
	{
		auto wrong = exact;
		wrong.library = other;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong NpCommerce library namespace resolved")) return false;
		wrong = exact;
		wrong.module = other;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong NpCommerce module namespace resolved")) return false;
	}
	constexpr Lumen::Loader::SymbolType wrong_types[] = {
	    Lumen::Loader::SymbolType::Unknown,
	    Lumen::Loader::SymbolType::Object,
	    Lumen::Loader::SymbolType::TlsModule,
	    Lumen::Loader::SymbolType::NoType,
	};
	for (auto type : wrong_types)
	{
		auto wrong = exact;
		wrong.type = type;
		if (!Check(symbols.Find(wrong) == nullptr, "wrong NpCommerce symbol type resolved")) return false;
	}
	auto plain_name = exact;
	plain_name.name = "sceNpCommerceDialogUpdateStatus";
	if (!Check(symbols.Find(plain_name) == nullptr, "plain NpCommerce function name resolved")) return false;
	return Check(symbols.StdNameMapSize() == 0 &&
	                 symbols.FindByStdName(U"LR5cwFMMCVE") == nullptr &&
	                 symbols.FindByStdName(U"sceNpCommerceDialogUpdateStatus") == nullptr,
	             "NpCommerce registration gained a standard-name fallback");
}

bool TestSiblingAbsence()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!InitializeDialogRegistry(symbols) || FindCommercePoll(symbols) == nullptr) return false;
	constexpr const char* siblings[] = {"0aR2aWmQal4", "DfSCDRA3EjY", "r42bWcQbtZY", "m-I92Ab50W8"};
	for (const char* nid : siblings)
		if (!Check(symbols.Find(CommerceIdentity(nid)) == nullptr, "an unapproved NpCommerce sibling resolved")) return false;
	return true;
}

bool TestBaseline(bool enabled)
{
	Lumen::Loader::SymbolDatabase symbols;
	return InitializeDialogRegistry(symbols) && CheckBaselineBindings(symbols) && CheckCommercePresence(symbols, enabled);
}

bool TestPolling()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!InitializeDialogRegistry(symbols)) return false;
	CommercePoll poll = FindCommercePoll(symbols);
	if (poll == nullptr) return false;
	return CheckRepeatedPolls(poll);
}

bool CaptureOnePoll(CommercePoll poll, bool first_poll)
{
	int32_t result = -1;
	std::string captured;
	if (!CaptureStderr(captured, [&] { result = poll(); })) return false;
	return Check(result == int32_t {0}, "isolated NpCommerce poll did not return signed32 zero") &&
	       Check(captured == (first_poll ? kMarker : ""),
	             "isolated polling violated the exact once-per-process marker contract");
}

constexpr std::size_t kOpaqueMsgResultPayloadBytes = 256;

struct DialogResultObservation
{
	int msg_result_call = 0;
	alignas(std::max_align_t) std::array<std::uint8_t, kOpaqueMsgResultPayloadBytes> msg_result_payload {};
	int save_result_call = 0;
	Save::SaveDataDialogResult save_result_payload {};
};

DialogResultObservation ConsumeMsgAndSaveResults()
{
	DialogResultObservation observation;
	// MsgDialog's public result parameter is opaque. Compare caller-owned bytes
	// without assigning any fields or console meaning to them.
	observation.msg_result_payload.fill(std::uint8_t {0xa5});
	std::memset(&observation.save_result_payload, 0xa5, sizeof(observation.save_result_payload));
	observation.msg_result_call = Msg::MsgDialogGetResult(observation.msg_result_payload.data());
	observation.save_result_call = Save::SaveDataDialogGetResult(&observation.save_result_payload);
	return observation;
}

bool SameSaveResultPayload(const Save::SaveDataDialogResult& left,
	                       const Save::SaveDataDialogResult& right)
{
	return std::memcmp(&left, &right, sizeof(left)) == 0;
}

enum class IsolationFixture
{
	Initial,
	MsgInitialized,
	SaveInitialized,
	MsgOpened,
	SaveOpened,
	MsgClosed,
	SaveClosed,
	SaveReinitialized,
	SaveReopened,
	MsgReinitialized,
	SaveTerminated,
	MsgTerminated,
};

bool PrepareIsolationFixture(IsolationFixture fixture)
{
	if (fixture == IsolationFixture::Initial) return true;

	if (!Check(Msg::MsgDialogInitialize() == 0, "MsgDialog fixture initialization failed")) return false;
	if (fixture == IsolationFixture::MsgInitialized) return true;

	if (!Check(Save::SaveDataDialogInitialize() == 0, "SaveDataDialog fixture initialization failed")) return false;
	if (fixture == IsolationFixture::SaveInitialized) return true;

	if (!Check(Msg::MsgDialogOpen(nullptr) == 0, "MsgDialog fixture open failed")) return false;
	if (fixture == IsolationFixture::MsgOpened) return true;

	if (!Check(Save::SaveDataDialogOpen(nullptr) == 0, "SaveDataDialog fixture open failed")) return false;
	if (fixture == IsolationFixture::SaveOpened) return true;

	if (!Check(Msg::MsgDialogClose() == 0, "MsgDialog fixture close failed")) return false;
	if (fixture == IsolationFixture::MsgClosed) return true;

	if (!Check(Save::SaveDataDialogClose() == 0, "SaveDataDialog fixture close failed")) return false;
	if (fixture == IsolationFixture::SaveClosed) return true;

	if (!Check(Save::SaveDataDialogInitialize() == 0, "SaveDataDialog fixture reinitialization failed")) return false;
	if (fixture == IsolationFixture::SaveReinitialized) return true;

	if (!Check(Save::SaveDataDialogOpen(nullptr) == 0, "SaveDataDialog fixture reopen failed")) return false;
	if (fixture == IsolationFixture::SaveReopened) return true;

	if (!Check(Msg::MsgDialogInitialize() == 0, "MsgDialog fixture reinitialization failed")) return false;
	if (fixture == IsolationFixture::MsgReinitialized) return true;

	if (!Check(Save::SaveDataDialogTerminate() == 0, "SaveDataDialog fixture termination failed")) return false;
	if (fixture == IsolationFixture::SaveTerminated) return true;

	if (!Check(Msg::MsgDialogTerminate() == 0, "MsgDialog fixture termination failed")) return false;
	return Check(fixture == IsolationFixture::MsgTerminated, "unknown isolation fixture");
}

bool CheckIsolationCheckpoint(CommercePoll poll, bool& first_poll, IsolationFixture fixture,
	                          int expected_msg_status, int expected_save_status)
{
	if (!PrepareIsolationFixture(fixture)) return false;
	const int control_msg_status = Msg::MsgDialogGetStatus();
	const int control_save_status = Save::SaveDataDialogGetStatus();
	const DialogResultObservation control_result = ConsumeMsgAndSaveResults();
	if (!Check(control_msg_status == expected_msg_status && control_save_status == expected_save_status,
	           "control fixture did not reach its expected local status codes") ||
	    !Check(Msg::MsgDialogGetStatus() == 0 && Save::SaveDataDialogGetStatus() == 0,
	           "control GetResult calls did not return the fixtures to their local baseline")) return false;

	if (!PrepareIsolationFixture(fixture)) return false;
	const int treatment_msg_before = Msg::MsgDialogGetStatus();
	const int treatment_save_before = Save::SaveDataDialogGetStatus();
	const uint32_t error_before = Error::ErrorDialogUpdateStatus();
	if (!CaptureOnePoll(poll, first_poll)) return false;
	first_poll = false;
	const int treatment_msg_after = Msg::MsgDialogGetStatus();
	const int treatment_save_after = Save::SaveDataDialogGetStatus();
	const uint32_t error_after = Error::ErrorDialogUpdateStatus();
	const DialogResultObservation treatment_result = ConsumeMsgAndSaveResults();
	const int treatment_msg_reset = Msg::MsgDialogGetStatus();
	const int treatment_save_reset = Save::SaveDataDialogGetStatus();

	return Check(treatment_msg_before == control_msg_status &&
	                 treatment_save_before == control_save_status,
	             "control and treatment fixtures did not have equivalent local status codes") &&
	       Check(treatment_msg_after == treatment_msg_before,
	             "NpCommerce polling changed MsgDialog status") &&
	       Check(treatment_save_after == treatment_save_before,
	             "NpCommerce polling changed SaveDataDialog status") &&
	       Check(treatment_result.msg_result_call == control_result.msg_result_call,
	             "NpCommerce polling changed the MsgDialog GetResult return") &&
	       Check(treatment_result.msg_result_payload == control_result.msg_result_payload,
	             "NpCommerce polling changed the opaque MsgDialog result bytes") &&
	       Check(treatment_result.save_result_call == control_result.save_result_call,
	             "NpCommerce polling changed the SaveDataDialog GetResult return") &&
	       Check(SameSaveResultPayload(treatment_result.save_result_payload,
	                                   control_result.save_result_payload),
	             "NpCommerce polling changed the SaveDataDialog result bytes") &&
	       Check(error_before == 0u, "ErrorDialog fixture did not retain its independent local value") &&
	       Check(error_after == error_before,
	             "NpCommerce polling changed ErrorDialog independently observed state") &&
	       Check(treatment_msg_reset == 0 && treatment_save_reset == 0,
	             "treatment GetResult calls did not return the fixtures to their local baseline");
}

bool TestIsolation()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!InitializeDialogRegistry(symbols)) return false;
	CommercePoll poll = FindCommercePoll(symbols);
	if (poll == nullptr) return false;

	bool first_poll = true;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::Initial, 0, 0)) return false;
	if (!Check(Dialog::CommonDialog::CommonDialogInitialize() == 0, "CommonDialog initialization failed") ||
	    !CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::Initial, 0, 0)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::MsgInitialized, 1, 0)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::SaveInitialized, 1, 1)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::MsgOpened, 3, 1)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::SaveOpened, 3, 3)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::MsgClosed, 0, 3)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::SaveClosed, 0, 0)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::SaveReinitialized, 0, 1)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::SaveReopened, 0, 3)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::MsgReinitialized, 1, 3)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::SaveTerminated, 1, 0)) return false;
	if (!CheckIsolationCheckpoint(poll, first_poll, IsolationFixture::MsgTerminated, 0, 0)) return false;
	return true;
}

bool RunCase(const char* name)
{
	if (std::strcmp(name, "signature") == 0) return TestSignature();
	if (std::strcmp(name, "selector-absent") == 0) return TestSelectorCase(nullptr, false, false);
	if (std::strcmp(name, "selector-zero") == 0) return TestSelectorCase("0", true, false);
	if (std::strcmp(name, "selector-empty") == 0) return TestSelectorCase("", true, false);
	if (std::strcmp(name, "selector-whitespace") == 0) return TestSelectorCase(" ", true, false);
	if (std::strcmp(name, "selector-malformed") == 0) return TestSelectorCase("01", true, false);
	if (std::strcmp(name, "selector-word") == 0) return TestSelectorCase("true", true, false);
	if (std::strcmp(name, "selector-one") == 0) return TestSelectorCase("1", true, true);
	if (std::strcmp(name, "latch-disabled") == 0) return TestLatch(false);
	if (std::strcmp(name, "latch-enabled") == 0) return TestLatch(true);
	if (std::strcmp(name, "exact-binding") == 0) return TestExactBinding();
	if (std::strcmp(name, "negative-identity") == 0) return TestNegativeIdentity();
	if (std::strcmp(name, "siblings-absent") == 0) return TestSiblingAbsence();
	if (std::strcmp(name, "baseline-disabled") == 0) return TestBaseline(false);
	if (std::strcmp(name, "baseline-enabled") == 0) return TestBaseline(true);
	if (std::strcmp(name, "polling") == 0) return TestPolling();
	if (std::strcmp(name, "isolation") == 0) return TestIsolation();
	return Check(false, "unknown test case");
}

} // namespace

int main(int argc, char** argv)
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (!Check(argc == 2, "expected exactly one test-case argument") || !RunCase(argv[1])) return 1;
	std::puts("NpCommerceDialogTest: PASS");
	return 0;
}
