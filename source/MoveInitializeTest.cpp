#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Move.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>

extern char** environ;

namespace {

namespace Move = Lumen::Libs::Move;
using ScalarType = Lumen::Loader::HostCallScalarType;
using InitializeFunction = int32_t (LUMEN_SYSV_ABI*)();
using OpenFunction = int32_t (LUMEN_SYSV_ABI*)(int32_t, int32_t, int32_t, int32_t, const void*);
using GetDeviceInfoFunction = int32_t (LUMEN_SYSV_ABI*)(int32_t, const void*);
using ReadStateRecentFunction = int32_t (LUMEN_SYSV_ABI*)(int32_t, int64_t, const void*, int32_t*);

static_assert(sizeof(int32_t) == 4);
static_assert(std::is_signed_v<int32_t>);
static_assert(std::is_same_v<decltype(&Move::Initialize), InitializeFunction>);
static_assert(std::is_same_v<decltype(&Move::Open), OpenFunction>);
static_assert(std::is_same_v<decltype(&Move::GetDeviceInfo), GetDeviceInfoFunction>);
static_assert(std::is_same_v<decltype(&Move::ReadStateRecent), ReadStateRecentFunction>);

constexpr char SELECTOR[] = "LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE";
constexpr int32_t INIT_REPEAT_RESULT = static_cast<int32_t>(0x80ee0002);
constexpr int32_t OPEN_INVALID_USER_RESULT = static_cast<int32_t>(0x80ee0006);
constexpr int32_t OPEN_PORT_BUSY_RESULT = static_cast<int32_t>(0x80ee0007);
constexpr int32_t GET_DEVICE_INFO_INVALID_OUTPUT_RESULT = static_cast<int32_t>(0x80ee0003);
constexpr int32_t GET_DEVICE_INFO_INVALID_HANDLE_RESULT = static_cast<int32_t>(0x80ee0004);
constexpr int32_t GET_DEVICE_INFO_NO_DEVICE_RESULT = 1;
constexpr int32_t READ_STATE_INVALID_OUTPUT_RESULT = static_cast<int32_t>(0x80ee0003);
constexpr int32_t READ_STATE_INVALID_HANDLE_RESULT = static_cast<int32_t>(0x80ee0004);
constexpr int32_t READ_STATE_NO_DEVICE_RESULT = 1;
constexpr char FIRST_DIAGNOSTIC[] =
    "[MOVE_INITIALIZE_NO_DEVICE_EXPERIMENT] result=complete-local call=first value=0 "
    "experimental=1 console_contract=measured device_model=none resources=0 guest_memory=0\n";
constexpr char REPEAT_DIAGNOSTIC[] =
    "[MOVE_INITIALIZE_NO_DEVICE_EXPERIMENT] result=repeat-measured call=repeat value=80ee0002 "
    "experimental=1 console_contract=measured device_model=none resources=0 guest_memory=0\n";
constexpr char DISABLED_DIAGNOSTIC[] =
    "[MOVE_INITIALIZE_NO_DEVICE_EXPERIMENT] result=disabled call=unavailable value=unavailable "
    "experimental=0 console_contract=measured device_model=none resources=0 guest_memory=0\n";
constexpr char OPEN_DISABLED_DIAGNOSTIC[] =
    "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=unavailable user=unavailable result=disabled "
    "rc=unavailable model=local-user-no-device console_contract=measured experimental=0 "
    "device_model=none resources=0 guest_memory=0\n";
constexpr char INFO_DISABLED_DIAGNOSTIC[] =
    "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=unavailable handle=unavailable result=disabled "
    "rc=unavailable model=local-user-no-device console_contract=measured experimental=0 "
    "device_model=none resources=0 guest_memory=0\n";
constexpr char READ_DISABLED_DIAGNOSTIC[] =
    "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=unavailable handle=unavailable result=disabled "
    "rc=unavailable model=local-user-no-device console_contract=measured experimental=0 device_model=none "
    "resources=0 guest_memory=0\n";
constexpr char DISABLED_FATAL[] = "Move Initialize no-device experiment is disabled";
constexpr char OPEN_DISABLED_FATAL[] = "Move Open no-device experiment is disabled";
constexpr char INFO_DISABLED_FATAL[] = "Move GetDeviceInfo no-device experiment is disabled";
constexpr char READ_DISABLED_FATAL[] = "Move ReadStateRecent no-device experiment is disabled";

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "MoveInitializeTest: %s\n", message);
	return condition;
}

Lumen::Loader::SymbolResolve MoveIdentity(const char* nid = "j1ITE-EoJmE")
{
	Lumen::Loader::SymbolResolve query {};
	query.name                  = nid;
	query.library               = "libSceMove";
	query.library_version       = 1;
	query.module                = "libSceMove";
	query.module_version_major  = 1;
	query.module_version_minor  = 1;
	query.type                  = Lumen::Loader::SymbolType::Func;
	return query;
}

template <typename Operation>
bool CaptureStderr(std::string* output, Operation operation)
{
	output->clear();
	std::fflush(stderr);
	std::FILE* capture = std::tmpfile();
	if (!Check(capture != nullptr, "could not create stderr capture")) return false;

	const int saved_stderr = ::dup(STDERR_FILENO);
	if (saved_stderr < 0 || ::dup2(::fileno(capture), STDERR_FILENO) < 0)
	{
		if (saved_stderr >= 0) ::close(saved_stderr);
		std::fclose(capture);
		return Check(false, "could not redirect stderr");
	}

	operation();
	std::fflush(stderr);
	const bool restored = ::dup2(saved_stderr, STDERR_FILENO) >= 0;
	::close(saved_stderr);
	if (!restored || std::fseek(capture, 0, SEEK_SET) != 0)
	{
		std::fclose(capture);
		return Check(false, "could not restore or rewind stderr");
	}

	std::array<char, 512> buffer {};
	for (;;)
	{
		const size_t size = std::fread(buffer.data(), 1, buffer.size(), capture);
		output->append(buffer.data(), size);
		if (size != buffer.size()) break;
	}
	const bool read_ok = std::ferror(capture) == 0;
	std::fclose(capture);
	return Check(read_ok, "could not read stderr capture");
}

size_t Count(const std::string& text, const char* needle)
{
	size_t result = 0;
	size_t offset = 0;
	const size_t length = std::strlen(needle);
	while ((offset = text.find(needle, offset)) != std::string::npos)
	{
		++result;
		offset += length;
	}
	return result;
}

const Lumen::Loader::SymbolRecord* FindMove(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(MoveIdentity());
}

const Lumen::Loader::SymbolRecord* FindOpen(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(MoveIdentity("HzC60MfjJxU"));
}

const Lumen::Loader::SymbolRecord* FindInfo(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(MoveIdentity("GWXTyxs4QbE"));
}

const Lumen::Loader::SymbolRecord* FindReadStateRecent(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(MoveIdentity("f2bcpK6kJfg"));
}

bool CheckRegistration(Lumen::Loader::SymbolDatabase* symbols, bool enabled)
{
	const bool selected = Lumen::Libs::Init(U"libMove_1", symbols);
	const auto* init_record = FindMove(*symbols);
	const auto* open_record = FindOpen(*symbols);
	const auto* info_record = FindInfo(*symbols);
	const auto* read_record = FindReadStateRecent(*symbols);
	if (!Check(selected == enabled, "library selector returned the wrong result") ||
	    !Check(enabled ? init_record != nullptr && init_record->vaddr == reinterpret_cast<uint64_t>(&Move::Initialize)
	                   : init_record == nullptr,
	           "exact Initialize binding presence or target does not match the selector") ||
	    !Check(enabled ? open_record != nullptr && open_record->vaddr == reinterpret_cast<uint64_t>(&Move::Open)
	                   : open_record == nullptr,
	           "exact Open binding presence or target does not match the selector") ||
	    !Check(enabled ? info_record != nullptr && info_record->vaddr == reinterpret_cast<uint64_t>(&Move::GetDeviceInfo)
	                   : info_record == nullptr,
	           "exact GetDeviceInfo binding presence or target does not match the selector") ||
	    !Check(enabled
	               ? read_record != nullptr && read_record->vaddr == reinterpret_cast<uint64_t>(&Move::ReadStateRecent)
	               : read_record == nullptr,
	           "exact ReadStateRecent binding presence or target does not match the selector"))
	{
		return false;
	}
	return Check(enabled ? symbols->HostCallTargetCount() == 4 : symbols->HostCallTargetCount() == 0,
	             "isolated Move registration published an unexpected target count");
}

bool TestSelector(const char* expected_value, bool enabled)
{
	const char* actual = std::getenv(SELECTOR);
	if (expected_value == nullptr)
	{
		if (!Check(actual == nullptr, "CTest did not remove the selector")) return false;
	} else if (!Check(actual != nullptr && std::strcmp(actual, expected_value) == 0,
	                  "CTest provided the wrong selector value"))
	{
		return false;
	}
	if (!Check(Move::ExperimentalEnabled() == enabled, "exact-one selector result is wrong")) return false;
	Lumen::Loader::SymbolDatabase symbols;
	return CheckRegistration(&symbols, enabled);
}

bool TestLatch(bool enabled)
{
	const char* value = std::getenv(SELECTOR);
	if (!Check(value != nullptr && std::strcmp(value, enabled ? "1" : "0") == 0,
	           "latch test received the wrong initial selector") ||
	    !Check(Move::ExperimentalEnabled() == enabled, "initial latch value is wrong"))
	{
		return false;
	}

	Lumen::Loader::SymbolDatabase first;
	if (!CheckRegistration(&first, enabled) ||
	    !Check(::setenv(SELECTOR, enabled ? "0" : "1", 1) == 0,
	           "could not change the environment after latching"))
	{
		return false;
	}
	Move::ResetSessionState();
	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate reset failed during latch test") ||
	    !Check(Move::ExperimentalEnabled() == enabled, "reset or environment change altered the process latch"))
	{
		return false;
	}
	Lumen::Loader::SymbolDatabase second;
	if (!CheckRegistration(&second, enabled)) return false;
	if (!enabled) return true;

	int32_t result = -1;
	std::string output;
	return CaptureStderr(&output, [&] { result = Move::Initialize(); }) &&
	       Check(result == 0, "latched enabled wrapper did not return signed32 zero") &&
	       Check(output == FIRST_DIAGNOSTIC, "latched enabled wrapper did not begin a new session");
}

bool TestExactIdentity()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!CheckRegistration(&symbols, true)) return false;

	for (const char* nid: {"j1ITE-EoJmE", "HzC60MfjJxU", "GWXTyxs4QbE", "f2bcpK6kJfg"})
	{
		const auto exact = MoveIdentity(nid);
		if (!Check(symbols.Find(exact) != nullptr, "approved Move identity did not resolve")) return false;

		for (int version: {0, 2})
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

		for (const char* other: {"Move", "libMove", "SceMove", "libScePad", "libSceMouse", "libkernel"})
		{
			auto wrong = exact;
			wrong.library = other;
			if (!Check(symbols.Find(wrong) == nullptr, "library namespace alias resolved")) return false;
			wrong = exact;
			wrong.module = other;
			if (!Check(symbols.Find(wrong) == nullptr, "module namespace alias resolved")) return false;
		}

		for (auto type: {Lumen::Loader::SymbolType::Unknown, Lumen::Loader::SymbolType::Object,
		                 Lumen::Loader::SymbolType::TlsModule, Lumen::Loader::SymbolType::NoType})
		{
			auto wrong = exact;
			wrong.type = type;
			if (!Check(symbols.Find(wrong) == nullptr, "wrong symbol type resolved")) return false;
		}
	}

	constexpr std::array<const char*, 3> unapproved_siblings {
	    "T8KYHPs1JE8", "IFQwtT2CeY0", "tsZi60H4ypY",
	};
	for (const char* name: unapproved_siblings)
		if (!Check(symbols.Find(MoveIdentity(name)) == nullptr, "an unapproved Move sibling resolved")) return false;
	for (const char* name: {"sceMoveInit", "sceMoveOpen", "sceMoveGetDeviceInfo", "sceMoveReadStateRecent"})
	{
		if (!Check(symbols.Find(MoveIdentity(name)) == nullptr,
		           "a public name resolved as an exact NID alias"))
		{
			return false;
		}
		if (!Check(symbols.FindByStdName(Lumen::Core::String::FromUtf8(name)) == nullptr,
		           "Move gained a standard-name alias"))
		{
			return false;
		}
	}
	if (!Check(symbols.StdNameMapSize() == 0 && symbols.FindByStdName(U"j1ITE-EoJmE") == nullptr &&
	               symbols.FindByStdName(U"HzC60MfjJxU") == nullptr &&
	               symbols.FindByStdName(U"GWXTyxs4QbE") == nullptr &&
	               symbols.FindByStdName(U"f2bcpK6kJfg") == nullptr,
	           "Move gained a standard-name or NID alias"))
	{
		return false;
	}

	for (const char32_t* id: {U"libSceMove_1", U"libMove_0", U"libMove_2"})
	{
		Lumen::Loader::SymbolDatabase wrong_library;
		if (!Check(!Lumen::Libs::Init(id, &wrong_library) && FindMove(wrong_library) == nullptr &&
		               FindOpen(wrong_library) == nullptr && FindInfo(wrong_library) == nullptr &&
		               FindReadStateRecent(wrong_library) == nullptr,
		           "an unsupported loader library identifier was accepted"))
		{
			return false;
		}
	}
	return true;
}

bool TestScalarAbi()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!CheckRegistration(&symbols, true)) return false;

	const auto* init_record = FindMove(symbols);
	const auto* open_record = FindOpen(symbols);
	const auto* info_record = FindInfo(symbols);
	const auto* read_record = FindReadStateRecent(symbols);
	if (init_record == nullptr || open_record == nullptr || info_record == nullptr || read_record == nullptr) return false;
	const auto* init_metadata = symbols.HostCallScalarMetadataFor(init_record->vaddr);
	const auto* open_metadata = symbols.HostCallScalarMetadataFor(open_record->vaddr);
	const auto* info_metadata = symbols.HostCallScalarMetadataFor(info_record->vaddr);
	const auto* read_metadata = symbols.HostCallScalarMetadataFor(read_record->vaddr);
	if (!Check(init_metadata != nullptr && init_metadata->arity == 0 && init_metadata->result == ScalarType::Gp &&
	               init_metadata->invoker != nullptr,
	           "zero-argument scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (auto argument: init_metadata->arguments)
		if (!Check(argument == ScalarType::Void, "zero-argument metadata contains an argument")) return false;

	if (!Check(open_metadata != nullptr && open_metadata->arity == 5 && open_metadata->result == ScalarType::Gp &&
	               open_metadata->invoker != nullptr,
	           "five-argument scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (size_t index = 0; index < 5; ++index)
		if (!Check(open_metadata->arguments[index] == ScalarType::Gp, "open metadata contains a non-GP argument")) return false;

	if (!Check(info_metadata != nullptr && info_metadata->arity == 2 && info_metadata->result == ScalarType::Gp &&
	               info_metadata->invoker != nullptr,
	           "two-argument GetDeviceInfo scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (size_t index = 0; index < 2; ++index)
		if (!Check(info_metadata->arguments[index] == ScalarType::Gp,
		           "GetDeviceInfo metadata contains a non-GP argument"))
		{
			return false;
		}

	if (!Check(read_metadata != nullptr && read_metadata->arity == 4 && read_metadata->result == ScalarType::Gp &&
	               read_metadata->invoker != nullptr,
	           "four-argument ReadStateRecent scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (size_t index = 0; index < 4; ++index)
		if (!Check(read_metadata->arguments[index] == ScalarType::Gp,
		           "ReadStateRecent metadata contains a non-GP argument"))
		{
			return false;
		}

	std::array<uint64_t, 8> canaries {
	    UINT64_C(0x1029384756abcdef), UINT64_C(0xfedcba6547382910),
	    UINT64_C(0x1122334455667788), UINT64_C(0x8877665544332211),
	    UINT64_C(0x01030507090b0d0f), UINT64_C(0xf0d0b09070503010),
	    UINT64_C(0x55aa55aa33cc33cc), UINT64_C(0xcc33cc33aa55aa55),
	};
	const auto original = canaries;
	uint64_t invoked = UINT64_MAX;
	Move::ResetSessionState();
	std::string output;
	if (!CaptureStderr(&output, [&] { invoked = init_metadata->invoker(canaries.data()); }) ||
	    !Check(invoked == 0, "scalar invoker did not return zero") ||
	    !Check(canaries == original, "zero-argument invoker mutated synthetic guest canaries") ||
	    !Check(output == FIRST_DIAGNOSTIC, "scalar invoker did not use the first-call wrapper path"))
	{
		return false;
	}

	std::array<uint64_t, 10> open_canaries {};
	open_canaries.fill(UINT64_C(0x5555aaaa12345678));
	const auto open_original = open_canaries;
	output.clear();
	if (!CaptureStderr(&output, [&] {
		    open_canaries[0] = static_cast<uint64_t>(1);
		    open_canaries[1] = 0;
		    open_canaries[2] = 0;
		    open_canaries[3] = 0;
		    open_canaries[4] = UINT64_C(0x7eeeeeee00000000);
		    invoked = open_metadata->invoker(open_canaries.data());
	    }) ||
	    !Check(static_cast<int32_t>(invoked) == 1, "open invoker did not publish the first local handle") ||
	    !Check(std::equal(open_canaries.begin() + 5, open_canaries.end(), open_original.begin() + 5),
	           "open invoker mutated unrelated synthetic guest canaries"))
	{
		return false;
	}
	if (!Check(output ==
	               "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=1 user=1 result=ok handle=1 "
	               "model=local-user-no-device console_contract=measured experimental=1 "
	               "device_model=none resources=0 guest_memory=0\n",
	           "open invoker emitted the wrong bounded diagnostic"))
	{
		return false;
	}

	// GetDeviceInfo: the live handle and a synthetic output pointer that must
	// be neither read nor written.
	std::array<uint64_t, 4> info_canaries {
	    static_cast<uint64_t>(1), UINT64_C(0x7eeeeeee00000000),
	    UINT64_C(0x1122334455667788), UINT64_C(0x8877665544332211),
	};
	const auto info_original = info_canaries;
	invoked = UINT64_MAX;
	output.clear();
	if (!CaptureStderr(&output, [&] { invoked = info_metadata->invoker(info_canaries.data()); }) ||
	    !Check(static_cast<int32_t>(invoked) == GET_DEVICE_INFO_NO_DEVICE_RESULT,
	           "GetDeviceInfo invoker did not return the measured no-device value") ||
	    !Check(info_canaries == info_original,
	           "GetDeviceInfo invoker mutated the synthetic handle or output canaries"))
	{
		return false;
	}
	if (!Check(output ==
	               "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=1 handle=1 result=no-device rc=1 "
	               "model=local-user-no-device console_contract=measured experimental=1 "
	               "device_model=none resources=0 guest_memory=0\n",
	           "GetDeviceInfo invoker emitted the wrong bounded diagnostic"))
	{
		return false;
	}

	// ReadStateRecent: a null count is refused before any handle
	// classification and nothing is written anywhere.
	std::array<uint64_t, 4> read_null_canaries {
	    static_cast<uint64_t>(1), 0, UINT64_C(0x7eeeeeee00000000), 0,
	};
	const auto read_null_original = read_null_canaries;
	invoked = UINT64_MAX;
	output.clear();
	if (!CaptureStderr(&output, [&] { invoked = read_metadata->invoker(read_null_canaries.data()); }) ||
	    !Check(static_cast<int32_t>(invoked) == READ_STATE_INVALID_OUTPUT_RESULT,
	           "ReadStateRecent invoker did not refuse the null count") ||
	    !Check(read_null_canaries == read_null_original,
	           "ReadStateRecent null-count invoker mutated its canaries"))
	{
		return false;
	}
	if (!Check(output ==
	               "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=1 handle=1 result=invalid-output "
	               "rc=80ee0003 model=local-user-no-device console_contract=measured experimental=1 "
	               "device_model=none resources=0 guest_memory=0\n",
	           "ReadStateRecent invoker emitted the wrong invalid-output diagnostic"))
	{
		return false;
	}

	// The measured invalid-handle refusal stores four zero bytes into the
	// count output; the data pointer is never touched.
	int32_t read_count = static_cast<int32_t>(0x12345678);
	std::array<uint64_t, 4> read_canaries {
	    static_cast<uint64_t>(2), 0, UINT64_C(0x7eeeeeee00000000),
	    reinterpret_cast<uint64_t>(&read_count),
	};
	invoked = UINT64_MAX;
	output.clear();
	if (!CaptureStderr(&output, [&] { invoked = read_metadata->invoker(read_canaries.data()); }) ||
	    !Check(static_cast<int32_t>(invoked) == READ_STATE_INVALID_HANDLE_RESULT,
	           "ReadStateRecent invoker did not refuse the non-live handle") ||
	    !Check(read_count == 0, "ReadStateRecent invoker did not store the measured zero count"))
	{
		return false;
	}
	return Check(output ==
	                 "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=2 handle=2 result=invalid-handle "
	                 "rc=80ee0004 model=local-user-no-device console_contract=measured experimental=1 "
	                 "device_model=none resources=0 guest_memory=0\n",
	             "ReadStateRecent invoker emitted the wrong invalid-handle diagnostic");
}

bool TestReadStateRecentProfile()
{
	// Measured console profile (campaign LMNQ92815): a live handle returns 1
	// and writes no guest memory for the measured timestamps; a null count
	// output receives 0x80ee0003 without writes; every non-live handle
	// receives 0x80ee0004 and the refusal stores four zero bytes into the
	// count output. The data window is never read or written.
	Move::ResetSessionState();
	std::string output;
	int32_t result = -1;

	std::array<uint8_t, 120> data_window {};
	data_window.fill(0x5a);
	const auto data_original = data_window;
	int32_t count = 0;
	int32_t call = 1;

	// call=1: a null count is refused before any handle classification;
	// nothing is written anywhere (declared order for the unmeasured
	// combination).
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=1 handle=1 result=invalid-output "
		              "rc=80ee0003 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::ReadStateRecent(1, 0, data_window.data(), nullptr); }))
		{
			return false;
		}
		if (!Check(result == READ_STATE_INVALID_OUTPUT_RESULT,
		           "null count did not receive the measured argument error") ||
		    !Check(output == expected, "null-count read emitted the wrong diagnostic") ||
		    !Check(data_window == data_original, "null-count read wrote the data window"))
		{
			return false;
		}
	}

	// Every non-live handle stores four zero bytes into the count output and
	// leaves the data window untouched.
	for (int32_t handle: {0, -1, 2, std::numeric_limits<int32_t>::max()})
	{
		++call;
		count = static_cast<int32_t>(0x12345678);
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=%d handle=%d result=invalid-handle "
		              "rc=80ee0004 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n",
		              call, handle);
		if (!CaptureStderr(&output,
		                   [&] { result = Move::ReadStateRecent(handle, 0, data_window.data(), &count); }))
		{
			return false;
		}
		if (!Check(result == READ_STATE_INVALID_HANDLE_RESULT,
		           "non-live handle did not receive the measured error") ||
		    !Check(count == 0, "non-live handle refusal did not store the measured zero count") ||
		    !Check(output == expected, "non-live handle emitted the wrong diagnostic") ||
		    !Check(data_window == data_original, "non-live handle read wrote the data window"))
		{
			return false;
		}
	}

	// The open for this profile publishes the local handle; the counters of
	// the two families are independent.
	{
		if (!CaptureStderr(&output, [&] { result = Move::Open(1, 0, 0, 0, nullptr); }) ||
		    !Check(result == 1, "open for the read profile did not publish the local handle"))
		{
			return false;
		}
	}

	// Live handle: the measured no-device value with no writes for the title
	// shape (timestamp 0), the repeat, the measured non-zero timestamp and
	// the accepted null data pointer.
	const auto expect_no_device = [&](int64_t timestamp, const void* data) -> bool {
		++call;
		count = static_cast<int32_t>(0x12345678);
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=%d handle=1 result=no-device "
		              "rc=1 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n",
		              call);
		if (!CaptureStderr(&output, [&] { result = Move::ReadStateRecent(1, timestamp, data, &count); }))
		{
			return false;
		}
		return Check(result == READ_STATE_NO_DEVICE_RESULT,
		             "live handle did not return the measured no-device value") &&
		       Check(count == static_cast<int32_t>(0x12345678), "live-handle read wrote the count output") &&
		       Check(output == expected, "live-handle read emitted the wrong diagnostic") &&
		       Check(data_window == data_original, "live-handle read wrote the data window");
	};
	if (!expect_no_device(0, data_window.data())) return false;
	if (!expect_no_device(0, data_window.data())) return false;
	if (!expect_no_device(1000000, data_window.data())) return false;
	if (!expect_no_device(0, nullptr)) return false;

	// The null count keeps its measured refusal with the live handle.
	++call;
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=%d handle=1 result=invalid-output "
		              "rc=80ee0003 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n",
		              call);
		if (!CaptureStderr(&output, [&] { result = Move::ReadStateRecent(1, 0, data_window.data(), nullptr); }))
		{
			return false;
		}
		if (!Check(result == READ_STATE_INVALID_OUTPUT_RESULT,
		           "live-handle null count did not receive the measured argument error") ||
		    !Check(output == expected, "live-handle null count emitted the wrong diagnostic") ||
		    !Check(data_window == data_original, "live-handle null count wrote the data window"))
		{
			return false;
		}
	}

	// Both outputs null with the live handle keep the same measured refusal.
	++call;
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=%d handle=1 result=invalid-output "
		              "rc=80ee0003 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n",
		              call);
		if (!CaptureStderr(&output, [&] { result = Move::ReadStateRecent(1, 0, nullptr, nullptr); }))
		{
			return false;
		}
		if (!Check(result == READ_STATE_INVALID_OUTPUT_RESULT,
		           "both-null read did not receive the measured argument error") ||
		    !Check(output == expected, "both-null read emitted the wrong diagnostic"))
		{
			return false;
		}
	}

	// The session reset clears the live port and restarts the call counter:
	// the old local handle becomes invalid again and the refusal still stores
	// the measured zero count.
	Move::ResetSessionState();
	{
		count = static_cast<int32_t>(0x12345678);
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=1 handle=1 result=invalid-handle "
		              "rc=80ee0004 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::ReadStateRecent(1, 0, data_window.data(), &count); }))
		{
			return false;
		}
		if (!Check(result == READ_STATE_INVALID_HANDLE_RESULT,
		           "session reset did not clear the read handle") ||
		    !Check(count == 0, "post-reset refusal did not store the measured zero count") ||
		    !Check(output == expected, "post-reset read emitted the wrong diagnostic") ||
		    !Check(data_window == data_original, "post-reset read wrote the data window"))
		{
			return false;
		}
	}
	return Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed in read profile test");
}

bool CallFirstAndExpect()
{
	int32_t result = -1;
	std::string output;
	return CaptureStderr(&output, [&] { result = Move::Initialize(); }) &&
	       Check(result == 0, "first initialization did not return signed32 zero") &&
	       Check(output == FIRST_DIAGNOSTIC, "first initialization emitted the wrong diagnostic");
}

bool TestFirstAndReset()
{
	Move::ResetSessionState();
	Move::ResetSessionState();
	if (!CallFirstAndExpect()) return false;

	Move::ResetSessionState();
	Move::ResetSessionState();
	if (!CallFirstAndExpect() ||
	    !Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed") ||
	    !CallFirstAndExpect() ||
	    !Check(Lumen::Libs::ResetSessionState(), "repeated aggregate HLE reset failed") ||
	    !CallFirstAndExpect())
	{
		return false;
	}
	return Check(Move::ExperimentalEnabled(), "session reset altered the process latch");
}

bool CallRepeatAndExpect()
{
	int32_t result = -1;
	std::string output;
	return CaptureStderr(&output, [&] { result = Move::Initialize(); }) &&
	       Check(result == INIT_REPEAT_RESULT, "repeated initialization did not return the measured error") &&
	       Check(output == REPEAT_DIAGNOSTIC, "repeated initialization emitted the wrong diagnostic");
}

bool TestInitProfile()
{
	// Measured console profile (campaign LMNQ92804): first call zero, every
	// repeat 0x80ee0002; a session reset reopens the first-call budget.
	Move::ResetSessionState();
	if (!CallFirstAndExpect() || !CallRepeatAndExpect() || !CallRepeatAndExpect()) return false;
	Move::ResetSessionState();
	return CallFirstAndExpect() &&
	       Check(Lumen::Libs::ResetSessionState(), "aggregate reset failed in init profile test") &&
	       CallFirstAndExpect();
}

bool TestConcurrentInit()
{
	Move::ResetSessionState();
	std::array<int32_t, 2> results {-1, -1};
	std::atomic<int> ready {0};
	std::atomic<bool> start {false};
	std::array<std::thread, 2> workers;
	for (size_t index = 0; index < workers.size(); ++index)
	{
		workers[index] = std::thread([&, index] {
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = Move::Initialize();
		});
	}
	while (ready.load(std::memory_order_acquire) != static_cast<int>(workers.size()))
		std::this_thread::yield();
	start.store(true, std::memory_order_release);
	for (auto& worker: workers) worker.join();

	int firsts = 0;
	int repeats = 0;
	for (int32_t result: results)
	{
		firsts += result == 0 ? 1 : 0;
		repeats += result == INIT_REPEAT_RESULT ? 1 : 0;
	}
	return Check(firsts == 1 && repeats == 1,
	             "concurrent initialization did not split into one first call and one measured repeat");
}

bool TestOpenProfile()
{
	// Measured console profile (campaigns LMNQ92804 and LMNQ92805): only the
	// modeled local user is admitted and a first open publishes the next local
	// handle as the signed32 result; every other user receives 0x80ee0006
	// without consuming a handle, and a second concurrent open while the
	// handle is live receives the measured 0x80ee0007 (LMNQ92805). No case
	// writes through the trailing pointer or reads its contents.
	Move::ResetSessionState();
	std::string output;
	int32_t result = -1;

	std::array<uint8_t, 64> window {};
	window.fill(0x5a);
	const auto original = window;
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=1 user=1 result=ok handle=1 "
		              "model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::Open(1, 0, 0, 0, window.data()); })) return false;
		if (!Check(result == 1, "local user open did not publish the first handle") ||
		    !Check(output == expected, "local user open emitted the wrong diagnostic") ||
		    !Check(window == original, "local user open wrote through the trailing pointer"))
		{
			return false;
		}
	}

	// The live handle refuses a concurrent second open with the measured code
	// and the refusal consumes no handle.
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=2 user=1 result=port-busy "
		              "rc=80ee0007 model=local-user-no-device console_contract=measured "
		              "experimental=1 device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::Open(1, 3, 1, 0, window.data()); })) return false;
		if (!Check(result == OPEN_PORT_BUSY_RESULT,
		           "second concurrent open did not receive the measured refusal") ||
		    !Check(output == expected, "port-busy open emitted the wrong diagnostic") ||
		    !Check(window == original, "port-busy open wrote through the trailing pointer"))
		{
			return false;
		}
	}

	int32_t call = 2;
	for (int32_t user: {0, 2, -1, std::numeric_limits<int32_t>::max()})
	{
		++call;
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=%d result=invalid-user "
		              "rc=80ee0006 model=local-user-no-device console_contract=measured "
		              "experimental=1 device_model=none resources=0 guest_memory=0\n",
		              call, user);
		if (!CaptureStderr(&output, [&] { result = Move::Open(user, 0, 0, 0, window.data()); })) return false;
		if (!Check(result == OPEN_INVALID_USER_RESULT, "unmodeled user did not receive the measured error") ||
		    !Check(output == expected, "unmodeled user emitted the wrong diagnostic") ||
		    !Check(window == original, "unmodeled user open wrote through the trailing pointer"))
		{
			return false;
		}
	}

	// The session reset restarts the first-call budget, the handle allocation
	// and the live port; the null trailing pointer is accepted, as measured.
	Move::ResetSessionState();
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=1 user=1 result=ok handle=1 "
		              "model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::Open(1, 3, 1, 0, nullptr); })) return false;
		if (!Check(result == 1, "session reset did not restart handle allocation") ||
		    !Check(output == expected, "null-pointer open emitted the wrong diagnostic"))
		{
			return false;
		}
	}
	return Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed in open profile test");
}

bool TestDeviceInfoProfile()
{
	// Measured console profile (campaign LMNQ92805): a live handle returns 1
	// and publishes no device record; a null output receives the measured
	// 0x80ee0003 and every non-live handle receives 0x80ee0004, all without
	// reading or writing the output window.
	Move::ResetSessionState();
	std::string output;
	int32_t result = -1;

	std::array<uint8_t, 64> window {};
	window.fill(0x5a);
	const auto original = window;
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=1 handle=1 result=invalid-handle "
		              "rc=80ee0004 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::GetDeviceInfo(1, window.data()); })) return false;
		if (!Check(result == GET_DEVICE_INFO_INVALID_HANDLE_RESULT,
		           "info without a live port did not reject the handle") ||
		    !Check(output == expected, "pre-open info emitted the wrong diagnostic") ||
		    !Check(window == original, "pre-open info wrote the output window"))
		{
			return false;
		}
	}

	// The null output is classified before the handle, as declared for the
	// unmeasured combination.
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=2 handle=0 result=invalid-output "
		              "rc=80ee0003 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::GetDeviceInfo(0, nullptr); })) return false;
		if (!Check(result == GET_DEVICE_INFO_INVALID_OUTPUT_RESULT,
		           "null output did not receive the measured argument error") ||
		    !Check(output == expected, "null-output info emitted the wrong diagnostic"))
		{
			return false;
		}
	}

	// A live handle publishes no record and returns the measured value; the
	// repeat is identical.
	{
		if (!CaptureStderr(&output, [&] { result = Move::Open(1, 0, 0, 0, nullptr); }) ||
		    !Check(result == 1, "open for the info profile did not publish the local handle"))
		{
			return false;
		}
	}
	for (int repeat = 1; repeat <= 2; ++repeat)
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=%d handle=1 result=no-device "
		              "rc=1 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n",
		              repeat + 2);
		if (!CaptureStderr(&output, [&] { result = Move::GetDeviceInfo(1, window.data()); })) return false;
		if (!Check(result == GET_DEVICE_INFO_NO_DEVICE_RESULT,
		           "live handle did not return the measured no-device value") ||
		    !Check(output == expected, "live-handle info emitted the wrong diagnostic") ||
		    !Check(window == original, "live-handle info wrote the output window"))
		{
			return false;
		}
	}

	// The null output with the live handle, and the measured invalid handles.
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=5 handle=1 result=invalid-output "
		              "rc=80ee0003 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::GetDeviceInfo(1, nullptr); })) return false;
		if (!Check(result == GET_DEVICE_INFO_INVALID_OUTPUT_RESULT,
		           "live-handle null output did not receive the measured argument error") ||
		    !Check(output == expected, "live-handle null output emitted the wrong diagnostic"))
		{
			return false;
		}
	}
	int32_t call = 5;
	for (int32_t handle: {0, -1, 2, std::numeric_limits<int32_t>::max()})
	{
		++call;
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=%d handle=%d result=invalid-handle "
		              "rc=80ee0004 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n",
		              call, handle);
		if (!CaptureStderr(&output, [&] { result = Move::GetDeviceInfo(handle, window.data()); })) return false;
		if (!Check(result == GET_DEVICE_INFO_INVALID_HANDLE_RESULT,
		           "non-live handle did not receive the measured error") ||
		    !Check(output == expected, "non-live handle emitted the wrong diagnostic") ||
		    !Check(window == original, "non-live handle info wrote the output window"))
		{
			return false;
		}
	}

	// The session reset clears the live port: the old local handle becomes
	// invalid again.
	Move::ResetSessionState();
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=1 handle=1 result=invalid-handle "
		              "rc=80ee0004 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Move::GetDeviceInfo(1, window.data()); })) return false;
		if (!Check(result == GET_DEVICE_INFO_INVALID_HANDLE_RESULT,
		           "session reset did not clear the live port") ||
		    !Check(output == expected, "post-reset info emitted the wrong diagnostic") ||
		    !Check(window == original, "post-reset info wrote the output window"))
		{
			return false;
		}
	}
	return Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed in info profile test");
}

bool TestInitAll(bool enabled)
{
	Lumen::Loader::SymbolDatabase symbols;
	Lumen::Libs::InitAll(&symbols);
	const auto* init_record = FindMove(symbols);
	const auto* open_record = FindOpen(symbols);
	const auto* info_record = FindInfo(symbols);
	const auto* read_record = FindReadStateRecent(symbols);
	if (!Check(symbols.HostCallRegistrySealed(), "InitAll did not seal the authoritative registry")) return false;
	if (!enabled)
		return Check(init_record == nullptr && open_record == nullptr && info_record == nullptr && read_record == nullptr,
		             "InitAll exposed Move while disabled");
	return Check(init_record != nullptr && init_record->vaddr == reinterpret_cast<uint64_t>(&Move::Initialize),
	             "InitAll omitted or misbound the exact Move Initialize export") &&
	       Check(open_record != nullptr && open_record->vaddr == reinterpret_cast<uint64_t>(&Move::Open),
	             "InitAll omitted or misbound the exact Move Open export") &&
	       Check(info_record != nullptr && info_record->vaddr == reinterpret_cast<uint64_t>(&Move::GetDeviceInfo),
	             "InitAll omitted or misbound the exact Move GetDeviceInfo export") &&
	       Check(read_record != nullptr && read_record->vaddr == reinterpret_cast<uint64_t>(&Move::ReadStateRecent),
	             "InitAll omitted or misbound the exact Move ReadStateRecent export") &&
	       Check(symbols.HostCallScalarMetadataFor(init_record->vaddr) != nullptr &&
	                 symbols.HostCallScalarMetadataFor(open_record->vaddr) != nullptr &&
	                 symbols.HostCallScalarMetadataFor(info_record->vaddr) != nullptr &&
	                 symbols.HostCallScalarMetadataFor(read_record->vaddr) != nullptr,
	             "InitAll omitted Move scalar metadata");
}

bool SpawnFatalChild(const char* executable, const char* child_case, bool expect_first,
	                 const char* expected_diagnostic, const char* expected_fatal)
{
	int descriptors[2] {};
	if (!Check(::pipe(descriptors) == 0, "fatal-child pipe failed")) return false;
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
	if (error == 0) error = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
	if (error == 0) error = posix_spawn_file_actions_addclose(&actions, descriptors[1]);

	pid_t child = 0;
	char* arguments[] {const_cast<char*>(executable), const_cast<char*>(child_case), nullptr};
	if (error == 0) error = posix_spawn(&child, executable, &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	::close(descriptors[1]);
	if (error != 0)
	{
		::close(descriptors[0]);
		return Check(false, "fatal child could not be spawned");
	}

	std::string output;
	std::array<char, 512> buffer {};
	bool read_ok = true;
	for (;;)
	{
		const ssize_t size = ::read(descriptors[0], buffer.data(), buffer.size());
		if (size > 0) output.append(buffer.data(), static_cast<size_t>(size));
		else if (size == 0) break;
		else if (errno != EINTR)
		{
			read_ok = false;
			break;
		}
	}
	::close(descriptors[0]);
	int status = 0;
	pid_t waited;
	do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
	return Check(read_ok && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff),
	             "wrapper did not use the explicit fatal exit") &&
	       Check(Count(output, expected_diagnostic) == 1 &&
	                 (!expect_first || Count(output, FIRST_DIAGNOSTIC) == 1) &&
	                 output.find(expected_fatal) != std::string::npos,
	             "fatal wrapper omitted or duplicated its bounded marker") &&
	       Check(output.size() < 4096 && output.find("0x") == std::string::npos,
	             "fatal diagnostic was unbounded or exposed an address");
}

bool RunCase(const char* name, const char* executable)
{
	if (std::strcmp(name, "selector-absent") == 0) return TestSelector(nullptr, false);
	if (std::strcmp(name, "selector-zero") == 0) return TestSelector("0", false);
	if (std::strcmp(name, "selector-empty") == 0) return TestSelector("", false);
	if (std::strcmp(name, "selector-malformed") == 0) return TestSelector("01", false);
	if (std::strcmp(name, "selector-whitespace") == 0) return TestSelector("1 ", false);
	if (std::strcmp(name, "selector-word") == 0) return TestSelector("true", false);
	if (std::strcmp(name, "selector-two") == 0) return TestSelector("2", false);
	if (std::strcmp(name, "selector-one") == 0) return TestSelector("1", true);
	if (std::strcmp(name, "latch-disabled") == 0) return TestLatch(false);
	if (std::strcmp(name, "latch-enabled") == 0) return TestLatch(true);
	if (std::strcmp(name, "exact-identity") == 0) return TestExactIdentity();
	if (std::strcmp(name, "scalar-abi") == 0) return TestScalarAbi();
	if (std::strcmp(name, "first-reset") == 0) return TestFirstAndReset();
	if (std::strcmp(name, "init-profile") == 0) return TestInitProfile();
	if (std::strcmp(name, "concurrent-init") == 0) return TestConcurrentInit();
	if (std::strcmp(name, "open-profile") == 0) return TestOpenProfile();
	if (std::strcmp(name, "device-info-profile") == 0) return TestDeviceInfoProfile();
	if (std::strcmp(name, "read-state-recent-profile") == 0) return TestReadStateRecentProfile();
	if (std::strcmp(name, "init-all-disabled") == 0) return TestInitAll(false);
	if (std::strcmp(name, "init-all-enabled") == 0) return TestInitAll(true);
	if (std::strcmp(name, "fatal-disabled") == 0)
		return SpawnFatalChild(executable, "fatal-disabled-child", false, DISABLED_DIAGNOSTIC,
		                       DISABLED_FATAL);
	if (std::strcmp(name, "fatal-open-disabled") == 0)
		return SpawnFatalChild(executable, "fatal-open-disabled-child", false, OPEN_DISABLED_DIAGNOSTIC,
		                       OPEN_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-info-disabled") == 0)
		return SpawnFatalChild(executable, "fatal-info-disabled-child", false, INFO_DISABLED_DIAGNOSTIC,
		                       INFO_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-read-disabled") == 0)
		return SpawnFatalChild(executable, "fatal-read-disabled-child", false, READ_DISABLED_DIAGNOSTIC,
		                       READ_DISABLED_FATAL);
	return Check(false, "unknown test case");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "fatal-disabled-child") == 0)
	{
		(void)Move::Initialize();
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-open-disabled-child") == 0)
	{
		(void)Move::Open(1, 0, 0, 0, nullptr);
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-info-disabled-child") == 0)
	{
		(void)Move::GetDeviceInfo(1, nullptr);
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-read-disabled-child") == 0)
	{
		(void)Move::ReadStateRecent(1, 0, nullptr, nullptr);
		return 4;
	}

	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (!Check(argc == 2, "expected exactly one test case") || !RunCase(argv[1], argv[0])) return 1;
	std::puts("MoveInitializeTest: PASS");
	return 0;
}
