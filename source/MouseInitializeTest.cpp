#include "Lumen/Core/Threads.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/Mouse.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <algorithm>
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

namespace Mouse = Lumen::Libs::Mouse;
using ScalarType = Lumen::Loader::HostCallScalarType;
using InitializeFunction = int32_t (LUMEN_SYSV_ABI*)();
using OpenFunction = int32_t (LUMEN_SYSV_ABI*)(int32_t, int32_t, int32_t, const void*);
using ReadFunction = int32_t (LUMEN_SYSV_ABI*)(int32_t, void*, int32_t);

static_assert(sizeof(int32_t) == 4);
static_assert(std::is_signed_v<int32_t>);
static_assert(std::is_same_v<decltype(&Mouse::Initialize), InitializeFunction>);
static_assert(std::is_same_v<decltype(&Mouse::Open), OpenFunction>);
static_assert(std::is_same_v<decltype(&Mouse::Read), ReadFunction>);

constexpr char SELECTOR[] = "LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE";
constexpr int32_t OPEN_INVALID_USER_RESULT = static_cast<int32_t>(0x809b0081);
constexpr int32_t OPEN_INVALID_USER_ARGUMENT_RESULT = static_cast<int32_t>(0x809b0001);
constexpr int32_t OPEN_INVALID_TYPE_RESULT = static_cast<int32_t>(0x80df0001);
constexpr int32_t OPEN_PORT_BUSY_RESULT = static_cast<int32_t>(0x80df0004);
constexpr int32_t READ_INVALID_ARGUMENT_RESULT = static_cast<int32_t>(0x80df0001);
constexpr int32_t READ_INVALID_HANDLE_RESULT = static_cast<int32_t>(0x80df0003);
constexpr size_t READ_RECORD_SIZE = 40;
constexpr char FIRST_DIAGNOSTIC[] =
    "[MOUSE_INITIALIZE_NO_DEVICE_EXPERIMENT] result=complete-local call=first value=0 "
    "experimental=1 console_contract=measured-first device_model=none host_input=untouched "
    "resources=0 guest_memory=0\n";
constexpr char REPEAT_DIAGNOSTIC[] =
    "[MOUSE_INITIALIZE_NO_DEVICE_EXPERIMENT] result=complete-local call=repeat value=0 "
    "experimental=1 console_contract=measured-first device_model=none host_input=untouched "
    "resources=0 guest_memory=0\n";
constexpr char DISABLED_DIAGNOSTIC[] =
    "[MOUSE_INITIALIZE_NO_DEVICE_EXPERIMENT] result=disabled call=unavailable value=unavailable "
    "experimental=0 console_contract=measured-first device_model=none host_input=untouched "
    "resources=0 guest_memory=0\n";
constexpr char DISABLED_FATAL[] = "Mouse Initialize no-device experiment is disabled";
constexpr char OPEN_DISABLED_DIAGNOSTIC[] =
    "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=unavailable user=unavailable type=unavailable "
    "index=unavailable result=disabled rc=unavailable model=local-user-no-device "
    "console_contract=measured experimental=0 device_model=none resources=0 guest_memory=0\n";
constexpr char OPEN_DISABLED_FATAL[] = "Mouse Open no-device experiment is disabled";
constexpr char READ_DISABLED_DIAGNOSTIC[] =
    "[MOUSE_READ_NO_DEVICE_EXPERIMENT] call=unavailable handle=unavailable num=unavailable "
    "result=disabled rc=unavailable model=local-user-no-device console_contract=measured "
    "experimental=0 device_model=none resources=0 guest_memory=0\n";
constexpr char READ_DISABLED_FATAL[] = "Mouse Read no-device experiment is disabled";

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "MouseInitializeTest: %s\n", message);
	return condition;
}

Lumen::Loader::SymbolResolve MouseIdentity(const char* nid = "Qs0wWulgl7U")
{
	Lumen::Loader::SymbolResolve query {};
	query.name                  = nid;
	query.library               = "libSceMouse";
	query.library_version       = 1;
	query.module                = "libSceMouse";
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

const Lumen::Loader::SymbolRecord* FindMouse(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(MouseIdentity());
}

const Lumen::Loader::SymbolRecord* FindOpen(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(MouseIdentity("RaqxZIf6DvE"));
}

const Lumen::Loader::SymbolRecord* FindRead(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(MouseIdentity("x8qnXqh-tiM"));
}

bool CheckRegistration(Lumen::Loader::SymbolDatabase* symbols, bool enabled)
{
	const bool selected = Lumen::Libs::Init(U"libMouse_1", symbols);
	const auto* record = FindMouse(*symbols);
	const auto* open_record = FindOpen(*symbols);
	const auto* read_record = FindRead(*symbols);
	if (!Check(selected == enabled, "library selector returned the wrong result") ||
	    !Check(enabled ? record != nullptr && record->vaddr == reinterpret_cast<uint64_t>(&Mouse::Initialize)
	                   : record == nullptr,
	           "exact Initialize binding presence or target does not match the selector") ||
	    !Check(enabled ? open_record != nullptr && open_record->vaddr == reinterpret_cast<uint64_t>(&Mouse::Open)
	                   : open_record == nullptr,
	           "exact Open binding presence or target does not match the selector") ||
	    !Check(enabled ? read_record != nullptr && read_record->vaddr == reinterpret_cast<uint64_t>(&Mouse::Read)
	                   : read_record == nullptr,
	           "exact Read binding presence or target does not match the selector"))
	{
		return false;
	}
	return Check(enabled ? symbols->HostCallTargetCount() == 3 : symbols->HostCallTargetCount() == 0,
	             "isolated Mouse registration published an unexpected target count");
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
	if (!Check(Mouse::ExperimentalEnabled() == enabled, "exact-one selector result is wrong")) return false;
	Lumen::Loader::SymbolDatabase symbols;
	return CheckRegistration(&symbols, enabled);
}

bool TestLatch(bool enabled)
{
	const char* value = std::getenv(SELECTOR);
	if (!Check(value != nullptr && std::strcmp(value, enabled ? "1" : "0") == 0,
	           "latch test received the wrong initial selector") ||
	    !Check(Mouse::ExperimentalEnabled() == enabled, "initial latch value is wrong"))
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
	Mouse::ResetSessionState();
	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate reset failed during latch test") ||
	    !Check(Mouse::ExperimentalEnabled() == enabled, "reset or environment change altered the process latch"))
	{
		return false;
	}
	Lumen::Loader::SymbolDatabase second;
	if (!CheckRegistration(&second, enabled)) return false;
	if (!enabled) return true;

	int32_t result = -1;
	std::string output;
	return CaptureStderr(&output, [&] { result = Mouse::Initialize(); }) &&
	       Check(result == 0, "latched enabled wrapper did not return signed32 zero") &&
	       Check(output == FIRST_DIAGNOSTIC, "latched enabled wrapper did not begin a new session");
}

bool TestExactIdentity()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!CheckRegistration(&symbols, true)) return false;
	for (const char* nid: {"Qs0wWulgl7U", "RaqxZIf6DvE", "x8qnXqh-tiM"})
	{
		const auto exact = MouseIdentity(nid);
		if (!Check(symbols.Find(exact) != nullptr, "an approved Mouse identity did not resolve")) return false;

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

		for (const char* other: {"Mouse", "libMouse", "SceMouse", "libSceMove", "libScePad", "libkernel"})
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

	for (const char* sibling: {"cAnT0Rw-IwU"})
		if (!Check(symbols.Find(MouseIdentity(sibling)) == nullptr, "an unapproved Mouse sibling resolved")) return false;
	if (!Check(symbols.Find(MouseIdentity("sceMouseInit")) == nullptr &&
	               symbols.Find(MouseIdentity("sceMouseOpen")) == nullptr &&
	               symbols.Find(MouseIdentity("sceMouseRead")) == nullptr,
	           "a public name resolved as an exact NID alias"))
	{
		return false;
	}
	if (!Check(symbols.StdNameMapSize() == 0 && symbols.FindByStdName(U"sceMouseInit") == nullptr &&
	               symbols.FindByStdName(U"Qs0wWulgl7U") == nullptr &&
	               symbols.FindByStdName(U"sceMouseOpen") == nullptr &&
	               symbols.FindByStdName(U"RaqxZIf6DvE") == nullptr &&
	               symbols.FindByStdName(U"sceMouseRead") == nullptr &&
	               symbols.FindByStdName(U"x8qnXqh-tiM") == nullptr,
	           "Mouse gained a standard-name or NID alias"))
	{
		return false;
	}

	for (const char32_t* id: {U"libSceMouse_1", U"libMouse_0", U"libMouse_2"})
	{
		Lumen::Loader::SymbolDatabase wrong_library;
		if (!Check(!Lumen::Libs::Init(id, &wrong_library) && FindMouse(wrong_library) == nullptr &&
		               FindOpen(wrong_library) == nullptr && FindRead(wrong_library) == nullptr,
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
	const auto* record = FindMouse(symbols);
	const auto* open_record = FindOpen(symbols);
	if (record == nullptr || open_record == nullptr) return false;
	const auto* metadata = symbols.HostCallScalarMetadataFor(record->vaddr);
	const auto* open_metadata = symbols.HostCallScalarMetadataFor(open_record->vaddr);
	if (!Check(metadata != nullptr && metadata->arity == 0 && metadata->result == ScalarType::Gp &&
	               metadata->invoker != nullptr,
	           "zero-argument scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (auto argument: metadata->arguments)
		if (!Check(argument == ScalarType::Void, "zero-argument metadata contains an argument")) return false;

	if (!Check(open_metadata != nullptr && open_metadata->arity == 4 &&
	               open_metadata->result == ScalarType::Gp && open_metadata->invoker != nullptr,
	           "four-argument Open scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (size_t index = 0; index < 4; ++index)
		if (!Check(open_metadata->arguments[index] == ScalarType::Gp,
		           "Open metadata contains a non-GP argument"))
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
	Mouse::ResetSessionState();
	std::string output;
	if (!CaptureStderr(&output, [&] { invoked = metadata->invoker(canaries.data()); }) ||
	    !Check(invoked == 0, "scalar invoker did not return zero") ||
	    !Check(canaries == original, "zero-argument invoker mutated synthetic guest canaries") ||
	    !Check(output == FIRST_DIAGNOSTIC, "scalar invoker did not use the first-call wrapper path"))
	{
		return false;
	}

	// Open: the local user with the first measured slot and a synthetic
	// parameter pointer that must be neither read nor written.
	std::array<uint64_t, 10> open_canaries {};
	open_canaries.fill(UINT64_C(0x5555aaaa12345678));
	const auto open_original = open_canaries;
	output.clear();
	if (!CaptureStderr(&output, [&] {
		    open_canaries[0] = static_cast<uint64_t>(1);
		    open_canaries[1] = 0;
		    open_canaries[2] = 0;
		    open_canaries[3] = UINT64_C(0x7eeeeeee00000000);
		    invoked = open_metadata->invoker(open_canaries.data());
	    }) ||
	    !Check(static_cast<int32_t>(invoked) == 1, "open invoker did not publish the first local handle") ||
	    !Check(std::equal(open_canaries.begin() + 4, open_canaries.end(), open_original.begin() + 4),
	           "open invoker mutated unrelated synthetic guest canaries"))
	{
		return false;
	}
	if (!Check(output ==
	               "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=1 user=1 type=0 index=0 result=ok "
	               "handle=1 rc=0 model=local-user-no-device console_contract=measured "
	               "experimental=1 device_model=none resources=0 guest_memory=0\n",
	           "open invoker emitted the wrong bounded diagnostic"))
	{
		return false;
	}

	// Read: the measured three-scalar shape (handle, data pointer, num) and an
	// invoker call that stores the forty zero bytes of the device-less record
	// through the pointer argument; handle 1 is alive from the open above.
	const auto* read_record = FindRead(symbols);
	if (read_record == nullptr) return false;
	const auto* read_metadata = symbols.HostCallScalarMetadataFor(read_record->vaddr);
	if (!Check(read_metadata != nullptr && read_metadata->arity == 3 &&
	               read_metadata->result == ScalarType::Gp && read_metadata->invoker != nullptr,
	           "three-argument Read scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (size_t index = 0; index < 3; ++index)
		if (!Check(read_metadata->arguments[index] == ScalarType::Gp,
		           "Read metadata contains a non-GP argument"))
		{
			return false;
		}

	std::array<uint8_t, 64> read_window {};
	read_window.fill(0x5a);
	std::array<uint64_t, 8> read_canaries {};
	read_canaries[0] = 1;
	read_canaries[1] = reinterpret_cast<uint64_t>(read_window.data());
	read_canaries[2] = 64;
	output.clear();
	if (!CaptureStderr(&output, [&] { invoked = read_metadata->invoker(read_canaries.data()); }) ||
	    !Check(static_cast<int32_t>(invoked) == 1, "read invoker did not return the no-device result") ||
	    !Check(std::all_of(read_window.begin(), read_window.begin() + READ_RECORD_SIZE,
	                       [](uint8_t value) { return value == 0; }),
	           "read invoker did not store the measured zero record") ||
	    !Check(std::all_of(read_window.begin() + READ_RECORD_SIZE, read_window.end(),
	                       [](uint8_t value) { return value == 0x5a; }),
	           "read invoker wrote outside the measured record window"))
	{
		return false;
	}
	return Check(output ==
	                 "[MOUSE_READ_NO_DEVICE_EXPERIMENT] call=1 handle=1 num=64 result=no-device "
	                 "rc=1 model=local-user-no-device console_contract=measured experimental=1 "
	                 "device_model=none resources=0 guest_memory=0\n",
	             "read invoker emitted the wrong bounded diagnostic");
}

bool TestCallsAndReset()
{
	Mouse::ResetSessionState();
	std::atomic<bool> valid {true};
	std::string output;
	if (!CaptureStderr(&output, [&] {
		    for (size_t call = 0; call < 128; ++call)
			    if (Mouse::Initialize() != 0) valid.store(false, std::memory_order_relaxed);
	    }) ||
	    !Check(valid.load(std::memory_order_relaxed), "first or repeated initialization did not return zero") ||
	    !Check(output == std::string(FIRST_DIAGNOSTIC) + REPEAT_DIAGNOSTIC,
	           "first/repeat diagnostics were not exact and bounded"))
	{
		return false;
	}

	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed")) return false;
	int32_t result = -1;
	if (!CaptureStderr(&output, [&] { result = Mouse::Initialize(); }) ||
	    !Check(result == 0 && output == FIRST_DIAGNOSTIC,
	           "aggregate reset did not clear only the diagnostic session state"))
	{
		return false;
	}
	Mouse::ResetSessionState();
	return CaptureStderr(&output, [&] { result = Mouse::Initialize(); }) &&
	       Check(result == 0 && output == FIRST_DIAGNOSTIC,
	             "direct reset did not clear the diagnostic session state");
}

bool TestConcurrent()
{
	Mouse::ResetSessionState();
	std::atomic<int> ready {0};
	std::atomic<bool> start {false};
	std::atomic<bool> valid {true};
	std::array<std::thread, 8> workers;
	std::string output;
	if (!CaptureStderr(&output, [&] {
		    for (auto& worker: workers)
		    {
			    worker = std::thread([&] {
				    ready.fetch_add(1, std::memory_order_release);
				    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
				    for (size_t call = 0; call < 128; ++call)
					    if (Mouse::Initialize() != 0) valid.store(false, std::memory_order_relaxed);
			    });
		    }
		    while (ready.load(std::memory_order_acquire) != static_cast<int>(workers.size()))
			    std::this_thread::yield();
		    start.store(true, std::memory_order_release);
		    for (auto& worker: workers) worker.join();
	    }))
	{
		return false;
	}
	return Check(valid.load(std::memory_order_relaxed), "a concurrent initialization did not return zero") &&
	       Check(Count(output, FIRST_DIAGNOSTIC) == 1 && Count(output, REPEAT_DIAGNOSTIC) == 1 &&
	               output.size() == std::strlen(FIRST_DIAGNOSTIC) + std::strlen(REPEAT_DIAGNOSTIC),
	             "concurrent initialization diagnostics were not deterministic and bounded");
}

bool TestOpenProfile()
{
	// Measured console profile (campaign LMNQ92806): only the modeled local
	// user is admitted and a first open publishes the next local handle as the
	// signed32 result; the two measured index slots are independent, a repeat
	// on a live slot receives the measured 0x80df0004, the unmodeled users
	// receive the measured user errors, an out-of-domain type receives the
	// measured 0x80df0001, and no case reads or writes the trailing parameter.
	Mouse::ResetSessionState();
	std::string output;
	int32_t result = -1;

	std::array<uint8_t, 64> window {};
	window.fill(0x5a);
	const auto original = window;
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=1 user=1 type=0 index=0 result=ok "
		              "handle=1 rc=0 model=local-user-no-device console_contract=measured "
		              "experimental=1 device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Mouse::Open(1, 0, 0, window.data()); })) return false;
		if (!Check(result == 1, "local user open did not publish the first handle") ||
		    !Check(output == expected, "local user open emitted the wrong diagnostic") ||
		    !Check(window == original, "local user open wrote through the trailing pointer"))
		{
			return false;
		}
	}

	// A repeat on the live slot refuses with the measured code and consumes no
	// handle; the second measured slot stays independent.
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=2 user=1 type=0 index=0 "
		              "result=port-busy handle=0 rc=80df0004 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Mouse::Open(1, 0, 0, window.data()); })) return false;
		if (!Check(result == OPEN_PORT_BUSY_RESULT, "live slot did not receive the measured refusal") ||
		    !Check(output == expected, "port-busy open emitted the wrong diagnostic") ||
		    !Check(window == original, "port-busy open wrote through the trailing pointer"))
		{
			return false;
		}
	}
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=3 user=1 type=0 index=1 result=ok "
		              "handle=2 rc=0 model=local-user-no-device console_contract=measured "
		              "experimental=1 device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Mouse::Open(1, 0, 1, window.data()); })) return false;
		if (!Check(result == 2, "second measured slot did not publish a new handle") ||
		    !Check(output == expected, "second-slot open emitted the wrong diagnostic") ||
		    !Check(window == original, "second-slot open wrote through the trailing pointer"))
		{
			return false;
		}
	}

	// Unmodeled users receive the measured user errors without consuming
	// handles; the invalid-argument code stays reserved for negative ids, as
	// measured for -1.
	int32_t call = 3;
	for (int32_t user: {0, 2, -1, std::numeric_limits<int32_t>::max()})
	{
		++call;
		const bool argument = user < 0;
		const int32_t expected_result = argument ? OPEN_INVALID_USER_ARGUMENT_RESULT : OPEN_INVALID_USER_RESULT;
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=%d type=0 index=0 "
		              "result=invalid-user handle=0 rc=%s model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n",
		              call, user, argument ? "809b0001" : "809b0081");
		if (!CaptureStderr(&output, [&] { result = Mouse::Open(user, 0, 0, window.data()); })) return false;
		if (!Check(result == expected_result, "unmodeled user did not receive the measured error") ||
		    !Check(output == expected, "unmodeled user emitted the wrong diagnostic") ||
		    !Check(window == original, "unmodeled user open wrote through the trailing pointer"))
		{
			return false;
		}
	}

	// The type domain and index domains outside the measured ones: the declared
	// local mapping to the measured invalid-argument code.
	{
		++call;
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=1 type=1 index=0 "
		              "result=invalid-type handle=0 rc=80df0001 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n",
		              call);
		if (!CaptureStderr(&output, [&] { result = Mouse::Open(1, 1, 0, window.data()); })) return false;
		if (!Check(result == OPEN_INVALID_TYPE_RESULT, "out-of-domain type did not receive the measured error") ||
		    !Check(output == expected, "invalid-type open emitted the wrong diagnostic") ||
		    !Check(window == original, "invalid-type open wrote through the trailing pointer"))
		{
			return false;
		}
	}
	for (int32_t index: {-1, 2, std::numeric_limits<int32_t>::max()})
	{
		++call;
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=1 type=0 index=%d "
		              "result=invalid-index handle=0 rc=80df0001 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n",
		              call, index);
		if (!CaptureStderr(&output, [&] { result = Mouse::Open(1, 0, index, window.data()); })) return false;
		if (!Check(result == OPEN_INVALID_TYPE_RESULT, "out-of-domain index did not receive the declared error") ||
		    !Check(output == expected, "invalid-index open emitted the wrong diagnostic") ||
		    !Check(window == original, "invalid-index open wrote through the trailing pointer"))
		{
			return false;
		}
	}

	// The trailing parameter is never read: the null variant is classified by
	// the same measured rules (the campaign could not separate the null
	// parameter from the live-slot refusal; the interface declares the local
	// reading) and the session reset restarts handles and slots.
	{
		++call;
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=1 type=0 index=0 "
		              "result=port-busy handle=0 rc=80df0004 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n",
		              call);
		if (!CaptureStderr(&output, [&] { result = Mouse::Open(1, 0, 0, nullptr); })) return false;
		if (!Check(result == OPEN_PORT_BUSY_RESULT,
		           "null-parameter open did not follow the measured live-slot rule") ||
		    !Check(output == expected, "null-parameter open emitted the wrong diagnostic"))
		{
			return false;
		}
	}

	Mouse::ResetSessionState();
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=1 user=1 type=0 index=0 result=ok "
		              "handle=1 rc=0 model=local-user-no-device console_contract=measured "
		              "experimental=1 device_model=none resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Mouse::Open(1, 0, 0, nullptr); })) return false;
		if (!Check(result == 1, "session reset did not restart handle allocation") ||
		    !Check(output == expected, "post-reset null-parameter open emitted the wrong diagnostic"))
		{
			return false;
		}
	}
	return Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed in open profile test");
}

bool TestReadProfile()
{
	// Measured console profile (campaigns LMNQ92816/LMNQ92817): a live handle
	// with num in 1..64 returns 1 and stores forty zero bytes (the device-less
	// record) for every measured num, num=1 included; a null data pointer and
	// a num outside 1..64 receive the measured 0x80df0001 without writes;
	// every handle that is not a live slot receives the measured 0x80df0003
	// without writes.
	Mouse::ResetSessionState();
	std::string output;
	int32_t result = -1;

	std::array<uint8_t, 64> window {};
	window.fill(0x5a);
	const auto original = window;

	// A never-opened handle: measured invalid-handle code, no writes.
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_READ_NO_DEVICE_EXPERIMENT] call=1 handle=1 num=64 "
		              "result=invalid-handle rc=80df0003 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Mouse::Read(1, window.data(), 64); })) return false;
		if (!Check(result == READ_INVALID_HANDLE_RESULT,
		           "never-opened handle did not receive the measured error") ||
		    !Check(output == expected, "never-opened handle emitted the wrong diagnostic") ||
		    !Check(window == original, "never-opened handle read wrote the record"))
		{
			return false;
		}
	}

	// Both measured slots stay live (handles 1 and 2).
	{
		std::string opens;
		if (!CaptureStderr(&opens, [&] {
			    (void)Mouse::Open(1, 0, 0, nullptr);
			    (void)Mouse::Open(1, 0, 1, nullptr);
		    }))
		{
			return false;
		}
	}

	// Title shape: a live handle with num 64 returns 1 and exactly the
	// measured forty zero bytes; the tail of the window stays untouched.
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_READ_NO_DEVICE_EXPERIMENT] call=2 handle=1 num=64 "
		              "result=no-device rc=1 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n");
		window.fill(0x5a);
		if (!CaptureStderr(&output, [&] { result = Mouse::Read(1, window.data(), 64); })) return false;
		if (!Check(result == 1, "live handle did not return the no-device result") ||
		    !Check(output == expected, "live handle emitted the wrong diagnostic") ||
		    !Check(std::all_of(window.begin(), window.begin() + READ_RECORD_SIZE,
		                       [](uint8_t value) { return value == 0; }),
		           "live handle did not store the measured zero record") ||
		    !Check(std::all_of(window.begin() + READ_RECORD_SIZE, window.end(),
		                       [](uint8_t value) { return value == 0x5a; }),
		           "live handle read wrote outside the measured record window"))
		{
			return false;
		}
	}

	// Repeat, second live slot and the measured num=1 shape: byte-identical
	// record and result.
	for (const auto& shape: {std::pair<int32_t, int32_t> {1, 64}, {1, 1}, {2, 64}})
	{
		window.fill(0x5a);
		if (!CaptureStderr(&output, [&] { result = Mouse::Read(shape.first, window.data(), shape.second); }))
			return false;
		if (!Check(result == 1, "accepted read did not return the no-device result") ||
		    !Check(std::all_of(window.begin(), window.begin() + READ_RECORD_SIZE,
		                       [](uint8_t value) { return value == 0; }),
		           "accepted read did not store the measured zero record") ||
		    !Check(std::all_of(window.begin() + READ_RECORD_SIZE, window.end(),
		                       [](uint8_t value) { return value == 0x5a; }),
		           "accepted read wrote outside the measured record window"))
		{
			return false;
		}
	}

	// The measured argument refusals: a null data pointer and a num outside
	// 1..64 receive 0x80df0001 without writes.
	{
		const int32_t nums[] = {0, -1, 65, std::numeric_limits<int32_t>::max()};
		for (const int32_t num: nums)
		{
			window.fill(0x5a);
			if (!CaptureStderr(&output, [&] { result = Mouse::Read(1, window.data(), num); }))
				return false;
			if (!Check(result == READ_INVALID_ARGUMENT_RESULT,
			           "out-of-domain num did not receive the measured error") ||
			    !Check(std::equal(window.begin(), window.end(), original.begin()),
			           "out-of-domain num wrote the window"))
			{
				return false;
			}
		}
		window.fill(0x5a);
		if (!CaptureStderr(&output, [&] { result = Mouse::Read(1, nullptr, 64); })) return false;
		if (!Check(result == READ_INVALID_ARGUMENT_RESULT,
		           "null data pointer did not receive the measured error") ||
		    !Check(std::equal(window.begin(), window.end(), original.begin()),
		           "null data pointer read touched the window"))
		{
			return false;
		}
	}

	// The measured handle refusals: 0 and -1 receive 0x80df0003 without
	// writes.
	for (const int32_t handle: {0, -1, std::numeric_limits<int32_t>::max(), -12345})
	{
		window.fill(0x5a);
		if (!CaptureStderr(&output, [&] { result = Mouse::Read(handle, window.data(), 64); }))
			return false;
		if (!Check(result == READ_INVALID_HANDLE_RESULT,
		           "invalid handle did not receive the measured error") ||
		    !Check(std::equal(window.begin(), window.end(), original.begin()),
		           "invalid handle read wrote the window"))
		{
			return false;
		}
	}

	// The session reset clears the live slots: the same request follows the
	// measured invalid-handle path and a new open restarts the handles.
	Mouse::ResetSessionState();
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_READ_NO_DEVICE_EXPERIMENT] call=1 handle=1 num=64 "
		              "result=invalid-handle rc=80df0003 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n");
		if (!CaptureStderr(&output, [&] { result = Mouse::Read(1, window.data(), 64); })) return false;
		if (!Check(result == READ_INVALID_HANDLE_RESULT,
		           "reset session kept a stale live handle") ||
		    !Check(output == expected, "post-reset invalid handle emitted the wrong diagnostic"))
		{
			return false;
		}
	}
	{
		std::string opens;
		if (!CaptureStderr(&opens, [&] { result = Mouse::Open(1, 0, 0, nullptr); })) return false;
		if (!Check(result == 1, "post-reset open did not restart handle allocation")) return false;
	}
	{
		char expected[256] {};
		std::snprintf(expected, sizeof(expected),
		              "[MOUSE_READ_NO_DEVICE_EXPERIMENT] call=2 handle=1 num=64 "
		              "result=no-device rc=1 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none "
		              "resources=0 guest_memory=0\n");
		window.fill(0x5a);
		if (!CaptureStderr(&output, [&] { result = Mouse::Read(1, window.data(), 64); })) return false;
		if (!Check(result == 1, "post-reset read did not accept the new live handle") ||
		    !Check(output == expected, "post-reset read emitted the wrong diagnostic") ||
		    !Check(std::all_of(window.begin(), window.begin() + READ_RECORD_SIZE,
		                       [](uint8_t value) { return value == 0; }),
		           "post-reset read did not store the measured record"))
		{
			return false;
		}
	}
	return Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed in read profile test");
}

bool TestInitAll(bool enabled)
{
	Lumen::Loader::SymbolDatabase symbols;
	Lumen::Libs::InitAll(&symbols);
	const auto* record = FindMouse(symbols);
	const auto* open_record = FindOpen(symbols);
	const auto* read_record = FindRead(symbols);
	if (!Check(symbols.HostCallRegistrySealed(), "InitAll did not seal the authoritative registry")) return false;
	if (!enabled)
		return Check(record == nullptr && open_record == nullptr && read_record == nullptr,
		             "InitAll exposed Mouse while disabled");
	return Check(record != nullptr && record->vaddr == reinterpret_cast<uint64_t>(&Mouse::Initialize),
	             "InitAll omitted or misbound the exact Mouse Initialize export") &&
	       Check(open_record != nullptr && open_record->vaddr == reinterpret_cast<uint64_t>(&Mouse::Open),
	             "InitAll omitted or misbound the exact Mouse Open export") &&
	       Check(read_record != nullptr && read_record->vaddr == reinterpret_cast<uint64_t>(&Mouse::Read),
	             "InitAll omitted or misbound the exact Mouse Read export") &&
	       Check(symbols.HostCallScalarMetadataFor(record->vaddr) != nullptr &&
	                 symbols.HostCallScalarMetadataFor(open_record->vaddr) != nullptr &&
	                 symbols.HostCallScalarMetadataFor(read_record->vaddr) != nullptr,
	             "InitAll omitted Mouse scalar metadata");
}

bool SpawnFatalChild(const char* executable, const char* child_case, const char* expected_diagnostic,
                     const char* expected_fatal)
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
	             "disabled wrapper did not use the explicit fatal exit") &&
	       Check(Count(output, expected_diagnostic) == 1 &&
	                 output.find(expected_fatal) != std::string::npos,
	             "disabled wrapper omitted or duplicated its bounded marker") &&
	       Check(output.size() < 4096 && output.find("0x") == std::string::npos,
	             "disabled diagnostic was unbounded or exposed an address");
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
	if (std::strcmp(name, "calls-reset") == 0) return TestCallsAndReset();
	if (std::strcmp(name, "concurrent") == 0) return TestConcurrent();
	if (std::strcmp(name, "open-profile") == 0) return TestOpenProfile();
	if (std::strcmp(name, "read-profile") == 0) return TestReadProfile();
	if (std::strcmp(name, "init-all-disabled") == 0) return TestInitAll(false);
	if (std::strcmp(name, "init-all-enabled") == 0) return TestInitAll(true);
	if (std::strcmp(name, "fatal-disabled") == 0)
		return SpawnFatalChild(executable, "fatal-disabled-child", DISABLED_DIAGNOSTIC, DISABLED_FATAL);
	if (std::strcmp(name, "fatal-open-disabled") == 0)
		return SpawnFatalChild(executable, "fatal-open-disabled-child", OPEN_DISABLED_DIAGNOSTIC,
		                       OPEN_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-read-disabled") == 0)
		return SpawnFatalChild(executable, "fatal-read-disabled-child", READ_DISABLED_DIAGNOSTIC,
		                       READ_DISABLED_FATAL);
	return Check(false, "unknown test case");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "fatal-disabled-child") == 0)
	{
		(void)Mouse::Initialize();
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-open-disabled-child") == 0)
	{
		(void)Mouse::Open(1, 0, 0, nullptr);
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-read-disabled-child") == 0)
	{
		(void)Mouse::Read(1, nullptr, 64);
		return 4;
	}

	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (!Check(argc == 2, "expected exactly one test case") || !RunCase(argv[1], argv[0])) return 1;
	std::puts("MouseInitializeTest: PASS");
	return 0;
}
