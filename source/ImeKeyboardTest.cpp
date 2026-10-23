#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Ime.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <spawn.h>
#include <string>
#include <type_traits>
#include <unistd.h>

extern char** environ;

namespace {

namespace Ime = Lumen::Libs::Ime;
namespace Memory = Lumen::Core::VirtualMemory;
using ScalarType = Lumen::Loader::HostCallScalarType;
using KeyboardOpenFunction = int32_t (LUMEN_SYSV_ABI*)(int32_t, const void*);
using KeyboardGetResourceIdFunction = int32_t (LUMEN_SYSV_ABI*)(int32_t, void*);
using KeyboardGetInfoFunction = int32_t (LUMEN_SYSV_ABI*)(uint32_t, void*);
using KeyboardUpdateFunction = int32_t (LUMEN_SYSV_ABI*)(const void*);

static_assert(sizeof(int32_t) == 4);
static_assert(std::is_signed_v<int32_t>);
static_assert(std::is_same_v<decltype(&Ime::KeyboardOpen), KeyboardOpenFunction>);
static_assert(std::is_same_v<decltype(&Ime::KeyboardGetResourceId), KeyboardGetResourceIdFunction>);
static_assert(std::is_same_v<decltype(&Ime::KeyboardGetInfo), KeyboardGetInfoFunction>);
static_assert(std::is_same_v<decltype(&Ime::KeyboardUpdate), KeyboardUpdateFunction>);

constexpr char SELECTOR[] = "LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE";
constexpr int32_t OPEN_SESSION_LIVE_RESULT = static_cast<int32_t>(0x80bc0001);
constexpr int32_t OPEN_INVALID_USER_ID_RESULT = static_cast<int32_t>(0x80bc0004);
constexpr int32_t OPEN_USER_NOT_FOUND_RESULT = static_cast<int32_t>(0x80bc0010);
constexpr int32_t OPEN_NULL_PARAMETER_RESULT = static_cast<int32_t>(0x80bc0031);
constexpr int32_t RESOURCE_NO_SESSION_RESULT = static_cast<int32_t>(0x80bc0002);
constexpr int32_t RESOURCE_NULL_OUTPUT_RESULT = static_cast<int32_t>(0x80bc0031);
constexpr int32_t RESOURCE_USER_NOT_FOUND_RESULT = static_cast<int32_t>(0x80bc0010);
constexpr char RESOURCE_ID_DISABLED_DIAGNOSTIC[] =
    "[IME_KEYBOARD_GET_RESOURCE_ID_NO_DEVICE_EXPERIMENT] call=unavailable user=unavailable "
    "output=unavailable session=unavailable result=disabled rc=unavailable resource=none "
    "model=local-user-no-device console_contract=measured experimental=0 device_model=none "
    "resources=0 guest_memory=0\n";
constexpr char RESOURCE_ID_DISABLED_FATAL[] =
    "Ime Keyboard Get Resource Id no-device experiment is disabled";
constexpr char RESOURCE_ID_UNWRITABLE_DIAGNOSTIC[] =
    "[IME_KEYBOARD_GET_RESOURCE_ID_NO_DEVICE_EXPERIMENT] call=1 user=1 output=window session=live "
    "result=unwritable-output rc=unavailable resource=none model=local-user-no-device "
    "console_contract=measured experimental=1 device_model=none resources=0 guest_memory=0\n";
constexpr char RESOURCE_ID_UNWRITABLE_FATAL[] = "Not implemented (wrote == false)";
constexpr int32_t GET_INFO_REJECTED_RESULT = static_cast<int32_t>(0xffffffff);
constexpr char GET_INFO_DISABLED_DIAGNOSTIC[] =
    "[IME_KEYBOARD_GET_INFO_NO_DEVICE_EXPERIMENT] call=unavailable id=unavailable "
    "output=unavailable session=unavailable result=disabled rc=unavailable record=none "
    "model=local-user-no-device console_contract=measured experimental=0 device_model=none "
    "resources=0 guest_memory=0\n";
constexpr char GET_INFO_DISABLED_FATAL[] = "Ime Keyboard Get Info no-device experiment is disabled";
constexpr char GET_INFO_UNWRITABLE_DIAGNOSTIC[] =
    "[IME_KEYBOARD_GET_INFO_NO_DEVICE_EXPERIMENT] call=1 id=0 output=window session=live "
    "result=unwritable-output rc=unavailable record=none model=local-user-no-device "
    "console_contract=measured experimental=1 device_model=none resources=0 guest_memory=0\n";
constexpr char GET_INFO_UNWRITABLE_FATAL[] = "Not implemented (wrote == false)";
constexpr int32_t UPDATE_REFUSED_RESULT = static_cast<int32_t>(0x80bc0002);
constexpr char UPDATE_DISABLED_DIAGNOSTIC[] =
    "[IME_KEYBOARD_UPDATE_NO_DEVICE_EXPERIMENT] call=unavailable handler=unavailable "
    "result=disabled rc=unavailable handler_invoked=0 model=local-user-no-device "
    "console_contract=measured experimental=0 device_model=none resources=0 guest_memory=0\n";
constexpr char UPDATE_DISABLED_FATAL[] = "Ime Keyboard Update no-device experiment is disabled";
constexpr char OPEN_OK_DIAGNOSTIC[] =
    "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=1 user=1 param=window result=ok rc=0 "
    "model=local-user-no-device console_contract=measured experimental=1 device_model=none "
    "resources=0 guest_memory=0\n";
constexpr char OPEN_DISABLED_DIAGNOSTIC[] =
    "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=unavailable user=unavailable "
    "param=unavailable result=disabled rc=unavailable model=local-user-no-device "
    "console_contract=measured experimental=0 device_model=none resources=0 guest_memory=0\n";
constexpr char OPEN_DISABLED_FATAL[] = "Ime Keyboard Open no-device experiment is disabled";

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "ImeKeyboardTest: %s\n", message);
	return condition;
}

Lumen::Loader::SymbolResolve ImeIdentity(const char* nid = "eaFXjfJv3xs")
{
	Lumen::Loader::SymbolResolve query {};
	query.name                  = nid;
	query.library               = "libSceIme";
	query.library_version       = 1;
	query.module                = "libSceIme";
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

const Lumen::Loader::SymbolRecord* FindOpen(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(ImeIdentity());
}

const Lumen::Loader::SymbolRecord* FindResourceId(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(ImeIdentity("dKadqZFgKKQ"));
}

const Lumen::Loader::SymbolRecord* FindGetInfo(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(ImeIdentity("VkqLPArfFdc"));
}

const Lumen::Loader::SymbolRecord* FindUpdate(const Lumen::Loader::SymbolDatabase& symbols)
{
	return symbols.Find(ImeIdentity("-4GCfYdNF1s"));
}

bool CheckRegistration(Lumen::Loader::SymbolDatabase* symbols, bool enabled)
{
	const bool selected = Lumen::Libs::Init(U"libSceIme_1", symbols);
	const auto* open_record = FindOpen(*symbols);
	const auto* resource_record = FindResourceId(*symbols);
	const auto* get_info_record = FindGetInfo(*symbols);
	const auto* update_record = FindUpdate(*symbols);
	if (!Check(selected == enabled, "library selector returned the wrong result") ||
	    !Check(enabled ? open_record != nullptr && open_record->vaddr == reinterpret_cast<uint64_t>(&Ime::KeyboardOpen)
	                   : open_record == nullptr,
	           "exact KeyboardOpen binding presence or target does not match the selector") ||
	    !Check(enabled ? resource_record != nullptr &&
	                         resource_record->vaddr == reinterpret_cast<uint64_t>(&Ime::KeyboardGetResourceId)
	                   : resource_record == nullptr,
	           "exact KeyboardGetResourceId binding presence or target does not match the selector") ||
	    !Check(enabled ? get_info_record != nullptr &&
	                         get_info_record->vaddr == reinterpret_cast<uint64_t>(&Ime::KeyboardGetInfo)
	                   : get_info_record == nullptr,
	           "exact KeyboardGetInfo binding presence or target does not match the selector") ||
	    !Check(enabled ? update_record != nullptr &&
	                         update_record->vaddr == reinterpret_cast<uint64_t>(&Ime::KeyboardUpdate)
	                   : update_record == nullptr,
	           "exact KeyboardUpdate binding presence or target does not match the selector"))
	{
		return false;
	}
	return Check(enabled ? symbols->HostCallTargetCount() == 4 : symbols->HostCallTargetCount() == 0,
	             "isolated Ime registration published an unexpected target count");
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
	if (!Check(Ime::ExperimentalEnabled() == enabled, "exact-one selector result is wrong")) return false;
	Lumen::Loader::SymbolDatabase symbols;
	return CheckRegistration(&symbols, enabled);
}

bool TestLatch(bool enabled)
{
	const char* value = std::getenv(SELECTOR);
	if (!Check(value != nullptr && std::strcmp(value, enabled ? "1" : "0") == 0,
	           "latch test received the wrong initial selector") ||
	    !Check(Ime::ExperimentalEnabled() == enabled, "initial latch value is wrong"))
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
	Ime::ResetSessionState();
	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate reset failed during latch test") ||
	    !Check(Ime::ExperimentalEnabled() == enabled, "reset or environment change altered the process latch"))
	{
		return false;
	}
	Lumen::Loader::SymbolDatabase second;
	if (!CheckRegistration(&second, enabled)) return false;
	if (!enabled) return true;

	int32_t result = -1;
	std::string output;
	return CaptureStderr(&output,
	                     [&] { result = Ime::KeyboardOpen(1, reinterpret_cast<const void*>(UINT64_C(0x7eeeeeee12345678))); }) &&
	       Check(result == 0, "latched enabled wrapper did not return signed32 zero") &&
	       Check(output == OPEN_OK_DIAGNOSTIC, "latched enabled wrapper did not begin a new session");
}

bool TestExactIdentity()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!CheckRegistration(&symbols, true)) return false;

	for (const char* nid: {"eaFXjfJv3xs", "dKadqZFgKKQ", "VkqLPArfFdc", "-4GCfYdNF1s"})
	{
		const auto exact = ImeIdentity(nid);
		if (!Check(symbols.Find(exact) != nullptr, "an approved Ime identity did not resolve")) return false;

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

		for (const char* other: {"Ime", "libIme", "SceIme", "libSceMouse", "libSceMove", "libkernel"})
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

	// The measured close sibling and the public names stay unmapped: this
	// profile publishes exactly the four measured identities.
	if (!Check(symbols.Find(ImeIdentity("PMVehSlfZ94")) == nullptr, "an unapproved Ime sibling resolved")) return false;
	if (!Check(symbols.Find(ImeIdentity("sceImeKeyboardOpen")) == nullptr &&
	               symbols.Find(ImeIdentity("sceImeKeyboardGetResourceId")) == nullptr &&
	               symbols.Find(ImeIdentity("sceImeKeyboardGetInfo")) == nullptr &&
	               symbols.Find(ImeIdentity("sceImeUpdate")) == nullptr,
	           "a public name resolved as an exact NID alias"))
	{
		return false;
	}
	if (!Check(symbols.StdNameMapSize() == 0 && symbols.FindByStdName(U"sceImeKeyboardOpen") == nullptr &&
	               symbols.FindByStdName(U"sceImeKeyboardGetResourceId") == nullptr &&
	               symbols.FindByStdName(U"sceImeKeyboardGetInfo") == nullptr &&
	               symbols.FindByStdName(U"sceImeUpdate") == nullptr &&
	               symbols.FindByStdName(U"eaFXjfJv3xs") == nullptr && symbols.FindByStdName(U"dKadqZFgKKQ") == nullptr &&
	               symbols.FindByStdName(U"VkqLPArfFdc") == nullptr &&
	               symbols.FindByStdName(U"-4GCfYdNF1s") == nullptr,
	           "Ime gained a standard-name or NID alias"))
	{
		return false;
	}

	for (const char32_t* id: {U"libIme_1", U"libSceIme_0", U"libSceIme_2"})
	{
		Lumen::Loader::SymbolDatabase wrong_library;
		if (!Check(!Lumen::Libs::Init(id, &wrong_library) && FindOpen(wrong_library) == nullptr &&
		               FindResourceId(wrong_library) == nullptr && FindGetInfo(wrong_library) == nullptr &&
		               FindUpdate(wrong_library) == nullptr,
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
	const auto* record = FindOpen(symbols);
	if (record == nullptr) return false;
	const auto* metadata = symbols.HostCallScalarMetadataFor(record->vaddr);
	if (!Check(metadata != nullptr && metadata->arity == 2 && metadata->result == ScalarType::Gp &&
	               metadata->invoker != nullptr,
	           "two-argument scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (size_t index = 0; index < 2; ++index)
		if (!Check(metadata->arguments[index] == ScalarType::Gp,
		           "KeyboardOpen metadata contains a non-GP argument"))
		{
			return false;
		}

	// The invoker receives the two GP arguments and must publish the measured
	// result without touching any other synthetic word; the pointer value is
	// only compared against null and is never dereferenced.
	std::array<uint64_t, 6> canaries {};
	canaries.fill(UINT64_C(0x1029384756abcdef));
	canaries[0] = 1;
	canaries[1] = UINT64_C(0x7eeeeeee12345678);
	const auto expected = canaries;
	uint64_t invoked = UINT64_MAX;
	Ime::ResetSessionState();
	std::string output;
	if (!CaptureStderr(&output, [&] { invoked = metadata->invoker(canaries.data()); }) ||
	    !Check(invoked == 0, "scalar invoker did not return zero") ||
	    !Check(canaries == expected, "scalar invoker mutated synthetic guest words"))
	{
		return false;
	}
	if (!Check(output == OPEN_OK_DIAGNOSTIC, "scalar invoker did not use the measured accept path")) return false;

	// The resource-id identity publishes the same two-GP shape; its invoker
	// preserves the measured no-session gate without touching the synthetic
	// words (the output pointer is only touched on the measured write path).
	const auto* resource_record = FindResourceId(symbols);
	if (resource_record == nullptr) return false;
	const auto* resource_metadata = symbols.HostCallScalarMetadataFor(resource_record->vaddr);
	if (!Check(resource_metadata != nullptr && resource_metadata->arity == 2 &&
	               resource_metadata->result == ScalarType::Gp && resource_metadata->invoker != nullptr,
	           "resource-id scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (size_t index = 0; index < 2; ++index)
		if (!Check(resource_metadata->arguments[index] == ScalarType::Gp,
		           "KeyboardGetResourceId metadata contains a non-GP argument"))
		{
			return false;
		}
	Ime::ResetSessionState();
	canaries.fill(UINT64_C(0x1029384756abcdef));
	canaries[0] = 1;
	canaries[1] = UINT64_C(0x7eeeeeee12345678);
	const auto resource_expected = canaries;
	char       resource_expected_line[512] {};
	std::snprintf(resource_expected_line, sizeof(resource_expected_line),
	              "[IME_KEYBOARD_GET_RESOURCE_ID_NO_DEVICE_EXPERIMENT] call=1 user=1 output=window "
	              "session=none result=no-session rc=80bc0002 resource=none model=local-user-no-device "
	              "console_contract=measured experimental=1 device_model=none resources=0 guest_memory=0\n");
	if (!CaptureStderr(&output, [&] { invoked = resource_metadata->invoker(canaries.data()); }) ||
	    !Check(static_cast<int32_t>(invoked) == RESOURCE_NO_SESSION_RESULT,
	           "resource-id invoker did not preserve the measured no-session gate") ||
	    !Check(canaries == resource_expected, "resource-id invoker mutated synthetic guest words"))
	{
		return false;
	}
	if (!Check(output == resource_expected_line, "resource-id invoker emitted the wrong diagnostic")) return false;

	// The keyboard-info identity publishes the same two-GP shape; without a
	// live session its invoker returns the measured session code and does not
	// touch the synthetic words (the output pointer is only touched on the
	// measured write paths).
	const auto* get_info_record = FindGetInfo(symbols);
	if (get_info_record == nullptr) return false;
	const auto* get_info_metadata = symbols.HostCallScalarMetadataFor(get_info_record->vaddr);
	if (!Check(get_info_metadata != nullptr && get_info_metadata->arity == 2 &&
	               get_info_metadata->result == ScalarType::Gp && get_info_metadata->invoker != nullptr,
	           "keyboard-info scalar metadata is missing or wrong"))
	{
		return false;
	}
	for (size_t index = 0; index < 2; ++index)
		if (!Check(get_info_metadata->arguments[index] == ScalarType::Gp,
		           "KeyboardGetInfo metadata contains a non-GP argument"))
		{
			return false;
		}
	Ime::ResetSessionState();
	canaries.fill(UINT64_C(0x1029384756abcdef));
	canaries[0] = 0;
	canaries[1] = UINT64_C(0x7eeeeeee12345678);
	const auto get_info_expected = canaries;
	char       get_info_expected_line[512] {};
	std::snprintf(get_info_expected_line, sizeof(get_info_expected_line),
	              "[IME_KEYBOARD_GET_INFO_NO_DEVICE_EXPERIMENT] call=1 id=0 output=window "
	              "session=none result=no-session rc=80bc0002 record=none model=local-user-no-device "
	              "console_contract=measured experimental=1 device_model=none resources=0 guest_memory=0\n");
	if (!CaptureStderr(&output, [&] { invoked = get_info_metadata->invoker(canaries.data()); }) ||
	    !Check(static_cast<int32_t>(invoked) == RESOURCE_NO_SESSION_RESULT,
	           "keyboard-info invoker did not preserve the measured no-session gate") ||
	    !Check(canaries == get_info_expected, "keyboard-info invoker mutated synthetic guest words"))
	{
		return false;
	}
	if (!Check(output == get_info_expected_line, "keyboard-info invoker emitted the wrong diagnostic"))
	{
		return false;
	}

	// The keyboard-update identity publishes a one-GP shape; every call
	// returns the measured refusal without touching the synthetic words (the
	// handler pointer is only compared against null).
	const auto* update_record = FindUpdate(symbols);
	if (update_record == nullptr) return false;
	const auto* update_metadata = symbols.HostCallScalarMetadataFor(update_record->vaddr);
	if (!Check(update_metadata != nullptr && update_metadata->arity == 1 &&
	               update_metadata->result == ScalarType::Gp && update_metadata->invoker != nullptr,
	           "keyboard-update scalar metadata is missing or wrong"))
	{
		return false;
	}
	if (!Check(update_metadata->arguments[0] == ScalarType::Gp,
	           "KeyboardUpdate metadata contains a non-GP argument"))
	{
		return false;
	}
	Ime::ResetSessionState();
	canaries.fill(UINT64_C(0x1029384756abcdef));
	canaries[0] = UINT64_C(0x7eeeeeee12345678);
	const auto update_expected = canaries;
	char       update_expected_line[512] {};
	std::snprintf(update_expected_line, sizeof(update_expected_line),
	              "[IME_KEYBOARD_UPDATE_NO_DEVICE_EXPERIMENT] call=1 handler=function "
	              "result=refused rc=80bc0002 handler_invoked=0 model=local-user-no-device "
	              "console_contract=measured experimental=1 device_model=none resources=0 guest_memory=0\n");
	if (!CaptureStderr(&output, [&] { invoked = update_metadata->invoker(canaries.data()); }) ||
	    !Check(static_cast<int32_t>(invoked) == UPDATE_REFUSED_RESULT,
	           "keyboard-update invoker did not return the measured refusal") ||
	    !Check(canaries == update_expected, "keyboard-update invoker mutated synthetic guest words"))
	{
		return false;
	}
	return Check(output == update_expected_line, "keyboard-update invoker emitted the wrong diagnostic");
}

bool TestOpenProfile()
{
	// Measured console profile (campaigns LMNQ92807/92808): the local user
	// opens a single session that is refused afterwards with the measured
	// live-session code; the null parameter precedes that refusal; the users
	// 0 and -1 receive the measured invalid-user-id code, other positive
	// users the measured user-not-found code, all without any guest write.
	Ime::ResetSessionState();
	std::string output;
	int32_t result = -1;
	int32_t call = 0;

	std::array<uint8_t, 64> window {};
	window.fill(0x5a);
	const auto original = window;

	// The local user opens; the 256-byte parameter window keeps its pattern.
	++call;
	{
		char expected[512] {};
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=1 param=window result=ok "
		              "rc=0 model=local-user-no-device console_contract=measured experimental=1 "
		              "device_model=none resources=0 guest_memory=0\n",
		              call);
		window = original;
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, window.data()); })) return false;
		if (!Check(result == 0, "local user open did not return zero") ||
		    !Check(output == expected, "local user open emitted the wrong diagnostic") ||
		    !Check(window == original, "local user open wrote through the parameter pointer"))
		{
			return false;
		}
	}

	// Live-session refusal; the null parameter precedes it.
	{
		char expected[512] {};
		++call;
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=1 param=window "
		              "result=session-live rc=80bc0001 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none resources=0 "
		              "guest_memory=0\n",
		              call);
		window = original;
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, window.data()); })) return false;
		if (!Check(result == OPEN_SESSION_LIVE_RESULT, "live session did not receive the measured refusal") ||
		    !Check(output == expected, "session-live open emitted the wrong diagnostic") ||
		    !Check(window == original, "session-live open wrote through the parameter pointer"))
		{
			return false;
		}
	}
	{
		char expected[512] {};
		++call;
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=1 param=null "
		              "result=invalid-param rc=80bc0031 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none resources=0 "
		              "guest_memory=0\n",
		              call);
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, nullptr); })) return false;
		if (!Check(result == OPEN_NULL_PARAMETER_RESULT, "null parameter did not precede the live session") ||
		    !Check(output == expected, "null-parameter open emitted the wrong diagnostic"))
		{
			return false;
		}
	}

	// Clean-state user classification: 0 and -1 invalid, other positive users
	// not found on the local single-user console; the unmeasured `(0, NULL)`
	// combination follows the declared parameter-first order.
	Ime::ResetSessionState();
	call = 0;
	for (int32_t user: {0, -1})
	{
		char expected[512] {};
		++call;
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=%d param=window "
		              "result=invalid-user-id rc=80bc0004 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none resources=0 "
		              "guest_memory=0\n",
		              call, user);
		window = original;
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(user, window.data()); })) return false;
		if (!Check(result == OPEN_INVALID_USER_ID_RESULT, "invalid user id did not receive the measured code") ||
		    !Check(output == expected, "invalid user id emitted the wrong diagnostic") ||
		    !Check(window == original, "invalid user id wrote through the parameter pointer"))
		{
			return false;
		}
	}
	for (int32_t user: {2, std::numeric_limits<int32_t>::max()})
	{
		char expected[512] {};
		++call;
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=%d param=window "
		              "result=user-not-found rc=80bc0010 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none resources=0 "
		              "guest_memory=0\n",
		              call, user);
		window = original;
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(user, window.data()); })) return false;
		if (!Check(result == OPEN_USER_NOT_FOUND_RESULT, "unknown user did not receive the measured code") ||
		    !Check(output == expected, "unknown user emitted the wrong diagnostic") ||
		    !Check(window == original, "unknown user wrote through the parameter pointer"))
		{
			return false;
		}
	}
	{
		char expected[512] {};
		++call;
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=%d user=0 param=null "
		              "result=invalid-param rc=80bc0031 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none resources=0 "
		              "guest_memory=0\n",
		              call);
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(0, nullptr); })) return false;
		if (!Check(result == OPEN_NULL_PARAMETER_RESULT,
		           "the declared parameter-first order did not survive an invalid user") ||
		    !Check(output == expected, "invalid user with null parameter emitted the wrong diagnostic"))
		{
			return false;
		}
	}

	// The session latch clears with the bounded reset and the aggregate reset.
	Ime::ResetSessionState();
	{
		if (!CaptureStderr(&output,
		                   [&] { result = Ime::KeyboardOpen(1, reinterpret_cast<const void*>(UINT64_C(0x7eeeeeee12345678))); }) ||
		    !Check(result == 0 && output == OPEN_OK_DIAGNOSTIC,
		           "direct reset did not clear the live session"))
		{
			return false;
		}
	}
	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed")) return false;
	return CaptureStderr(&output,
	                     [&] { result = Ime::KeyboardOpen(1, reinterpret_cast<const void*>(UINT64_C(0x7eeeeeee12345678))); }) &&
	       Check(result == 0 && output == OPEN_OK_DIAGNOSTIC,
	             "aggregate reset did not clear the live session");
}

bool TestSessionReset()
{
	Ime::ResetSessionState();
	std::string output;
	int32_t result = -1;
	if (!CaptureStderr(&output,
	                   [&] { result = Ime::KeyboardOpen(1, reinterpret_cast<const void*>(UINT64_C(0x7eeeeeee12345678))); }) ||
	    !Check(result == 0 && output == OPEN_OK_DIAGNOSTIC, "first open did not begin a session"))
	{
		return false;
	}
	if (!CaptureStderr(&output,
	                   [&] { result = Ime::KeyboardOpen(1, reinterpret_cast<const void*>(UINT64_C(0x7eeeeeee12345678))); }) ||
	    !Check(result == OPEN_SESSION_LIVE_RESULT, "second open did not observe the live session"))
	{
		return false;
	}

	Ime::ResetSessionState();
	return CaptureStderr(&output,
	                     [&] { result = Ime::KeyboardOpen(1, reinterpret_cast<const void*>(UINT64_C(0x7eeeeeee12345678))); }) &&
	       Check(result == 0 && output == OPEN_OK_DIAGNOSTIC,
	             "direct reset did not restart the bounded session state");
}

bool TestInitAll(bool enabled)
{
	Lumen::Loader::SymbolDatabase symbols;
	Lumen::Libs::InitAll(&symbols);
	const auto* open_record = FindOpen(symbols);
	const auto* resource_record = FindResourceId(symbols);
	const auto* get_info_record = FindGetInfo(symbols);
	const auto* update_record = FindUpdate(symbols);
	if (!Check(symbols.HostCallRegistrySealed(), "InitAll did not seal the authoritative registry")) return false;
	if (!enabled)
		return Check(open_record == nullptr && resource_record == nullptr && get_info_record == nullptr &&
		                 update_record == nullptr,
		             "InitAll exposed Ime while disabled");
	return Check(open_record != nullptr && open_record->vaddr == reinterpret_cast<uint64_t>(&Ime::KeyboardOpen),
	             "InitAll omitted or misbound the exact KeyboardOpen export") &&
	       Check(symbols.HostCallScalarMetadataFor(open_record->vaddr) != nullptr,
	             "InitAll omitted the KeyboardOpen scalar metadata") &&
	       Check(resource_record != nullptr &&
	                 resource_record->vaddr == reinterpret_cast<uint64_t>(&Ime::KeyboardGetResourceId),
	             "InitAll omitted or misbound the exact KeyboardGetResourceId export") &&
	       Check(symbols.HostCallScalarMetadataFor(resource_record->vaddr) != nullptr,
	             "InitAll omitted the KeyboardGetResourceId scalar metadata") &&
	       Check(get_info_record != nullptr &&
	                 get_info_record->vaddr == reinterpret_cast<uint64_t>(&Ime::KeyboardGetInfo),
	             "InitAll omitted or misbound the exact KeyboardGetInfo export") &&
	       Check(symbols.HostCallScalarMetadataFor(get_info_record->vaddr) != nullptr,
	             "InitAll omitted the KeyboardGetInfo scalar metadata") &&
	       Check(update_record != nullptr &&
	                 update_record->vaddr == reinterpret_cast<uint64_t>(&Ime::KeyboardUpdate),
	             "InitAll omitted or misbound the exact KeyboardUpdate export") &&
	       Check(symbols.HostCallScalarMetadataFor(update_record->vaddr) != nullptr,
	             "InitAll omitted the KeyboardUpdate scalar metadata");
}

bool TestResourceIdProfile()
{
	// Measured console profile (campaign LMNQ92809, two complete probe
	// processes with identical outcomes): the live-session gate precedes the
	// null-output refusal, which precedes the user classification; the
	// session's user receives the measured 12-byte record (resource id
	// 0x1186ef73 and two zero words) and every other user, including 0 and
	// -1, receives the measured user-not-found code; every refusal leaves the
	// output untouched.
	Ime::ResetSessionState();
	std::string output;
	int32_t     result       = -1;
	int32_t     resource_call = 0;

	constexpr uint64_t unit = 0x4000;
	struct Mapping
	{
		uint64_t address = Memory::Alloc(0, 2 * unit, Memory::Mode::ReadWrite);
		~Mapping()
		{
			if (address != 0) Memory::Free(address);
		}
	} mapping;
	if (!Check(mapping.address != 0, "resource-id fixture allocation failed")) return false;

	constexpr size_t record_bytes = 12;
	constexpr size_t guard_before = 8;
	constexpr size_t window_bytes = guard_before + record_bytes + 52;
	uint8_t* const   window_base  = reinterpret_cast<uint8_t*>(mapping.address + unit + 64 - guard_before);
	uint8_t* const   record_start = window_base + guard_before;
	const auto       refill       = [&] { std::memset(window_base, 0xa5, window_bytes); };
	const auto record_matches = [&] {
		static constexpr uint8_t expected[record_bytes] = {0x73, 0xef, 0x86, 0x11};
		if (std::memcmp(record_start, expected, sizeof(expected)) != 0) return false;
		for (size_t i = sizeof(expected); i < record_bytes; ++i)
			if (record_start[i] != 0) return false;
		return true;
	};
	const auto guards_intact = [&] {
		for (size_t i = 0; i < guard_before; ++i)
			if (window_base[i] != 0xa5) return false;
		for (size_t i = guard_before + record_bytes; i < window_bytes; ++i)
			if (window_base[i] != 0xa5) return false;
		return true;
	};

	// The first leased guest write publishes a one-time VM snapshot status
	// line; run one complete write cycle first so the measured diagnostics
	// below stay exact.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, record_start); }) ||
	    !Check(result == 0, "resource-id warm-up open failed") ||
	    !CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, record_start); }))
	{
		return false;
	}
	if (!Check(result == 0, "resource-id warm-up write failed")) return false;
	Ime::ResetSessionState();
	resource_call = 0;

	char expected[512] {};
	const auto expect_line = [&](int call, int user, const char* form, const char* session, const char* res,
	                             const char* rc, const char* resource, int guest_bytes) {
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_GET_RESOURCE_ID_NO_DEVICE_EXPERIMENT] call=%d user=%d output=%s "
		              "session=%s result=%s rc=%s resource=%s model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none resources=0 "
		              "guest_memory=%d\n",
		              call, user, form, session, res, rc, resource, guest_bytes);
	};

	// No session: the live-session gate precedes both the null-output check
	// and the user classification.
	refill();
	++resource_call;
	expect_line(resource_call, 1, "window", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, record_start); })) return false;
	if (!Check(result == RESOURCE_NO_SESSION_RESULT, "resource id without a session did not receive the measured gate code") ||
	    !Check(output == expected, "no-session query emitted the wrong diagnostic") ||
	    !Check(guards_intact(), "no-session query wrote through the output pointer"))
	{
		return false;
	}
	++resource_call;
	expect_line(resource_call, 1, "null", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, nullptr); })) return false;
	if (!Check(result == RESOURCE_NO_SESSION_RESULT, "the session gate did not precede the null-output check") ||
	    !Check(output == expected, "no-session null output emitted the wrong diagnostic"))
	{
		return false;
	}
	refill();
	++resource_call;
	expect_line(resource_call, 0, "window", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(0, record_start); })) return false;
	if (!Check(result == RESOURCE_NO_SESSION_RESULT, "the session gate did not precede the user classification") ||
	    !Check(output == expected, "no-session invalid user emitted the wrong diagnostic") ||
	    !Check(guards_intact(), "no-session invalid user wrote through the output pointer"))
	{
		return false;
	}

	// Live session: the measured 12-byte record for the session's user.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, record_start); }) ||
	    !Check(result == 0 && output == OPEN_OK_DIAGNOSTIC, "resource-id fixture open did not start a session"))
	{
		return false;
	}
	refill();
	++resource_call;
	expect_line(resource_call, 1, "window", "live", "ok", "0", "1186ef73", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, record_start); })) return false;
	if (!Check(result == 0, "session user query did not succeed") ||
	    !Check(output == expected, "session user query emitted the wrong diagnostic") ||
	    !Check(record_matches(), "session user query did not publish the measured record") ||
	    !Check(guards_intact(), "session user query wrote outside the measured 12-byte record"))
	{
		return false;
	}
	// Repeats publish the same measured record.
	refill();
	++resource_call;
	expect_line(resource_call, 1, "window", "live", "ok", "0", "1186ef73", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, record_start); })) return false;
	if (!Check(result == 0 && output == expected && record_matches() && guards_intact(),
	           "repeated resource-id query is not stable"))
	{
		return false;
	}

	// The null output precedes the user classification but follows the
	// session gate; measured with the session user, 1 and 0.
	++resource_call;
	expect_line(resource_call, 1, "null", "live", "invalid-output", "80bc0031", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, nullptr); })) return false;
	if (!Check(result == RESOURCE_NULL_OUTPUT_RESULT, "live null output did not receive the measured refusal") ||
	    !Check(output == expected, "live null output emitted the wrong diagnostic"))
	{
		return false;
	}
	++resource_call;
	expect_line(resource_call, 0, "null", "live", "invalid-output", "80bc0031", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(0, nullptr); })) return false;
	if (!Check(result == RESOURCE_NULL_OUTPUT_RESULT, "the null-output check did not precede the user classification") ||
	    !Check(output == expected, "live null output for an invalid user emitted the wrong diagnostic"))
	{
		return false;
	}

	// Every other user receives the measured user-not-found code without
	// writes, including 0 and -1 (which receive 0x80bc0004 on Open instead).
	for (int32_t user: {0, -1, 2})
	{
		refill();
		++resource_call;
		expect_line(resource_call, user, "window", "live", "user-not-found", "80bc0010", "none", 0);
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(user, record_start); })) return false;
		if (!Check(result == RESOURCE_USER_NOT_FOUND_RESULT, "live non-owner user did not receive the measured code") ||
		    !Check(output == expected, "live non-owner user emitted the wrong diagnostic") ||
		    !Check(guards_intact(), "live non-owner user wrote through the output pointer"))
		{
			return false;
		}
	}

	// The refusals did not disturb the live session.
	refill();
	++resource_call;
	expect_line(resource_call, 1, "window", "live", "ok", "0", "1186ef73", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, record_start); })) return false;
	if (!Check(result == 0 && output == expected && record_matches() && guards_intact(),
	           "refusals disturbed the live session or its record"))
	{
		return false;
	}

	// The bounded reset clears the session (post-close equivalent) and both
	// counters, and a new session publishes the measured record again.
	Ime::ResetSessionState();
	refill();
	resource_call = 0;
	++resource_call;
	expect_line(resource_call, 1, "window", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, record_start); })) return false;
	if (!Check(result == RESOURCE_NO_SESSION_RESULT && output == expected && guards_intact(),
	           "direct reset did not clear the live session for the resource query"))
	{
		return false;
	}
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, record_start); }) ||
	    !Check(result == 0 && output == OPEN_OK_DIAGNOSTIC, "reopen after reset did not start a session"))
	{
		return false;
	}
	refill();
	++resource_call;
	expect_line(resource_call, 1, "window", "live", "ok", "0", "1186ef73", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, record_start); })) return false;
	if (!Check(result == 0 && output == expected && record_matches() && guards_intact(),
	           "reopened session did not publish the measured record"))
	{
		return false;
	}

	// The aggregate HLE reset clears the live session as well.
	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed")) return false;
	refill();
	resource_call = 0;
	++resource_call;
	expect_line(resource_call, 1, "window", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, record_start); })) return false;
	return Check(result == RESOURCE_NO_SESSION_RESULT && output == expected && guards_intact(),
	             "aggregate reset did not clear the live session for the resource query");
}

bool TestGetInfoProfile()
{
	// Measured console profile (campaigns LMNQ92810 and LMNQ92811, two fresh
	// probe processes with identical outcomes): the live-session gate precedes
	// the null-output refusal, which precedes the id classification; only the
	// ids 0 and 1 are accepted (20-value sweep) and each publishes its own
	// 24-byte record, while every other id receives -1 with a 36-byte run of
	// zeroes; every refusal without a write leaves the output untouched.
	Ime::ResetSessionState();
	std::string output;
	int32_t     result        = -1;
	int32_t     get_info_call = 0;

	constexpr uint64_t unit = 0x4000;
	struct Mapping
	{
		uint64_t address = Memory::Alloc(0, 2 * unit, Memory::Mode::ReadWrite);
		~Mapping()
		{
			if (address != 0) Memory::Free(address);
		}
	} mapping;
	if (!Check(mapping.address != 0, "keyboard-info fixture allocation failed")) return false;

	constexpr size_t record_bytes   = 24;
	constexpr size_t rejected_bytes = 36;
	constexpr size_t guard_before   = 8;
	constexpr size_t window_bytes   = guard_before + rejected_bytes + 44;
	uint8_t* const   window_base    = reinterpret_cast<uint8_t*>(mapping.address + unit + 64 - guard_before);
	uint8_t* const   record_start   = window_base + guard_before;
	const auto       refill         = [&] { std::memset(window_base, 0xa5, window_bytes); };
	const auto       pattern_range  = [&](size_t from, size_t to) {
		for (size_t i = from; i < to; ++i)
			if (window_base[i] != 0xa5) return false;
		return true;
	};
	const auto no_writes = [&] { return pattern_range(0, window_bytes); };
	const auto record_matches = [&](const uint8_t (&expected)[record_bytes]) {
		if (std::memcmp(record_start, expected, record_bytes) != 0) return false;
		return pattern_range(0, guard_before) && pattern_range(guard_before + record_bytes, window_bytes);
	};
	const auto rejected_matches = [&] {
		for (size_t i = 0; i < rejected_bytes; ++i)
			if (record_start[i] != 0) return false;
		return pattern_range(0, guard_before) && pattern_range(guard_before + rejected_bytes, window_bytes);
	};

	const uint8_t record0[record_bytes] = {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0,
	                                       0,    0,    0,    0,    0, 0, 0, 0, 0, 0, 0, 0};
	const uint8_t record1[record_bytes] = {0xff, 0xff, 0xff, 0xff, 1, 0, 0, 0, 0, 0, 0, 0,
	                                       0,    0,    0,    0,    0, 0, 0, 0, 1, 0, 0, 0};

	// The first leased guest write publishes a one-time VM snapshot status
	// line; run one complete write cycle first so the measured diagnostics
	// below stay exact.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, record_start); }) ||
	    !Check(result == 0, "keyboard-info warm-up open failed") ||
	    !CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, record_start); }))
	{
		return false;
	}
	if (!Check(result == 0, "keyboard-info warm-up write failed")) return false;
	Ime::ResetSessionState();
	get_info_call = 0;

	char expected[512] {};
	const auto expect_line = [&](int call, uint32_t id, const char* form, const char* session, const char* res,
	                             const char* rc, const char* record, int guest_bytes) {
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_GET_INFO_NO_DEVICE_EXPERIMENT] call=%d id=%u output=%s "
		              "session=%s result=%s rc=%s record=%s model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none resources=0 "
		              "guest_memory=%d\n",
		              call, id, form, session, res, rc, record, guest_bytes);
	};

	// No session: the live-session gate precedes both the null-output check
	// and the id classification.
	refill();
	++get_info_call;
	expect_line(get_info_call, 0u, "window", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, record_start); })) return false;
	if (!Check(result == RESOURCE_NO_SESSION_RESULT, "keyboard info without a session did not receive the measured gate code") ||
	    !Check(output == expected, "no-session info emitted the wrong diagnostic") ||
	    !Check(no_writes(), "no-session info wrote through the output pointer"))
	{
		return false;
	}
	++get_info_call;
	expect_line(get_info_call, 0u, "null", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, nullptr); })) return false;
	if (!Check(result == RESOURCE_NO_SESSION_RESULT, "the session gate did not precede the null-output check") ||
	    !Check(output == expected, "no-session null info emitted the wrong diagnostic"))
	{
		return false;
	}
	refill();
	++get_info_call;
	expect_line(get_info_call, 2u, "window", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(2u, record_start); })) return false;
	if (!Check(result == RESOURCE_NO_SESSION_RESULT, "the session gate did not precede the id classification") ||
	    !Check(output == expected, "no-session refused id emitted the wrong diagnostic") ||
	    !Check(no_writes(), "no-session refused id wrote through the output pointer"))
	{
		return false;
	}

	// Live session: the two accepted ids publish their own measured records.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, record_start); }) ||
	    !Check(result == 0 && output == OPEN_OK_DIAGNOSTIC, "keyboard-info fixture open did not start a session"))
	{
		return false;
	}
	refill();
	++get_info_call;
	expect_line(get_info_call, 0u, "window", "live", "ok", "0", "0", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, record_start); })) return false;
	if (!Check(result == 0, "accepted id 0 did not succeed") ||
	    !Check(output == expected, "accepted id 0 emitted the wrong diagnostic") ||
	    !Check(record_matches(record0), "accepted id 0 did not publish the measured record"))
	{
		return false;
	}
	refill();
	++get_info_call;
	expect_line(get_info_call, 0u, "window", "live", "ok", "0", "0", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, record_start); })) return false;
	if (!Check(result == 0 && output == expected && record_matches(record0),
	           "repeated id 0 query is not stable"))
	{
		return false;
	}
	refill();
	++get_info_call;
	expect_line(get_info_call, 1u, "window", "live", "ok", "0", "1", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(1u, record_start); })) return false;
	if (!Check(result == 0, "accepted id 1 did not succeed") ||
	    !Check(output == expected, "accepted id 1 emitted the wrong diagnostic") ||
	    !Check(record_matches(record1), "accepted id 1 did not publish the measured record"))
	{
		return false;
	}

	// Every other id receives the measured refusal: -1 and 36 zeroed bytes.
	for (uint32_t id: {2u, 0x1186ef73u, 0xffffffffu})
	{
		refill();
		++get_info_call;
		expect_line(get_info_call, id, "window", "live", "unknown-resource", "ffffffff", "zeros",
		            static_cast<int>(rejected_bytes));
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(id, record_start); })) return false;
		if (!Check(result == GET_INFO_REJECTED_RESULT, "refused id did not receive the measured code") ||
		    !Check(output == expected, "refused id emitted the wrong diagnostic") ||
		    !Check(rejected_matches(), "refused id did not publish the measured zeroed block"))
		{
			return false;
		}
	}

	// The null output precedes the id classification but follows the session
	// gate; measured with 0 and 2.
	for (uint32_t id: {0u, 2u})
	{
		++get_info_call;
		expect_line(get_info_call, id, "null", "live", "invalid-output", "80bc0031", "none", 0);
		if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(id, nullptr); })) return false;
		if (!Check(result == RESOURCE_NULL_OUTPUT_RESULT, "live null output did not receive the measured refusal") ||
		    !Check(output == expected, "live null output emitted the wrong diagnostic"))
		{
			return false;
		}
	}

	// The refusals did not disturb the live session.
	refill();
	++get_info_call;
	expect_line(get_info_call, 0u, "window", "live", "ok", "0", "0", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, record_start); })) return false;
	if (!Check(result == 0 && output == expected && record_matches(record0),
	           "refusals disturbed the live session or its record"))
	{
		return false;
	}

	// The bounded reset clears the session and both counters, and a new
	// session publishes the measured record again.
	Ime::ResetSessionState();
	refill();
	get_info_call = 0;
	++get_info_call;
	expect_line(get_info_call, 0u, "window", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, record_start); })) return false;
	if (!Check(result == RESOURCE_NO_SESSION_RESULT && output == expected && no_writes(),
	           "direct reset did not clear the live session for the info query"))
	{
		return false;
	}
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, record_start); }) ||
	    !Check(result == 0 && output == OPEN_OK_DIAGNOSTIC, "reopen after reset did not start a session"))
	{
		return false;
	}
	refill();
	++get_info_call;
	expect_line(get_info_call, 0u, "window", "live", "ok", "0", "0", static_cast<int>(record_bytes));
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, record_start); })) return false;
	if (!Check(result == 0 && output == expected && record_matches(record0),
	           "reopened session did not publish the measured record"))
	{
		return false;
	}

	// The aggregate HLE reset clears the live session as well.
	if (!Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed")) return false;
	refill();
	get_info_call = 0;
	++get_info_call;
	expect_line(get_info_call, 0u, "window", "none", "no-session", "80bc0002", "none", 0);
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, record_start); })) return false;
	return Check(result == RESOURCE_NO_SESSION_RESULT && output == expected && no_writes(),
	             "aggregate reset did not clear the live session for the info query");
}

bool TestUpdateProfile()
{
	// Measured console profile (campaigns LMNQ92812 and LMNQ92813, two fresh
	// probe processes): every update call returned 0x80bc0002 - without a
	// live session, with one (also in the title-order chain open, resource
	// id, info), after Close and after an all-zero open parameter - the probe
	// handler was never invoked (13 calls, handler count 0) and each call
	// returned in 2-11 microseconds. The local model reproduces the measured
	// refusal, never invokes the handler and leaves the session latch
	// unchanged; the successful-update gate was not identified.
	Ime::ResetSessionState();
	std::string output;
	int32_t     result      = -1;
	int32_t     update_call = 0;

	constexpr uint64_t unit = 0x4000;
	struct Mapping
	{
		uint64_t address = Memory::Alloc(0, 2 * unit, Memory::Mode::ReadWrite);
		~Mapping()
		{
			if (address != 0) Memory::Free(address);
		}
	} mapping;
	if (!Check(mapping.address != 0, "keyboard-update fixture allocation failed")) return false;

	constexpr size_t window_bytes = 128;
	uint8_t* const   window_base  = reinterpret_cast<uint8_t*>(mapping.address + unit + 64);
	const auto       refill       = [&] { std::memset(window_base, 0xa5, window_bytes); };
	const auto       pattern_intact = [&] {
		for (size_t i = 0; i < window_bytes; ++i)
			if (window_base[i] != 0xa5) return false;
		return true;
	};

	// The first leased guest write publishes a one-time VM snapshot status
	// line; run one complete write cycle first so the measured diagnostics
	// below stay exact.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, window_base); }) ||
	    !Check(result == 0, "keyboard-update warm-up open failed") ||
	    !CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, window_base); }))
	{
		return false;
	}
	if (!Check(result == 0, "keyboard-update warm-up write failed")) return false;
	Ime::ResetSessionState();
	update_call = 0;

	char expected[512] {};
	const auto expect_line = [&](int call, const char* handler) {
		std::snprintf(expected, sizeof(expected),
		              "[IME_KEYBOARD_UPDATE_NO_DEVICE_EXPERIMENT] call=%d handler=%s "
		              "result=refused rc=80bc0002 handler_invoked=0 model=local-user-no-device "
		              "console_contract=measured experimental=1 device_model=none resources=0 "
		              "guest_memory=0\n",
		              call, handler);
	};

	// No session: both handler shapes receive the measured refusal and no
	// guest byte changes.
	refill();
	++update_call;
	expect_line(update_call, "null");
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardUpdate(nullptr); })) return false;
	if (!Check(result == UPDATE_REFUSED_RESULT, "update without a session did not receive the measured refusal") ||
	    !Check(output == expected, "no-session null update emitted the wrong diagnostic") ||
	    !Check(pattern_intact(), "no-session null update wrote guest memory"))
	{
		return false;
	}
	++update_call;
	expect_line(update_call, "function");
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardUpdate(window_base); })) return false;
	if (!Check(result == UPDATE_REFUSED_RESULT && output == expected && pattern_intact(),
	           "no-session function update is not the measured refusal"))
	{
		return false;
	}

	// Live session (the measured local user): the same refusal, and the
	// session survives the update.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, window_base); }) ||
	    !Check(result == 0 && output == OPEN_OK_DIAGNOSTIC, "update fixture open did not start a session"))
	{
		return false;
	}
	refill();
	++update_call;
	expect_line(update_call, "function");
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardUpdate(window_base); })) return false;
	if (!Check(result == UPDATE_REFUSED_RESULT && output == expected && pattern_intact(),
	           "live update is not the measured refusal"))
	{
		return false;
	}

	// The title-order chain (resource id and info before the update) behaves
	// the same; the measured queries still succeed and the session stays live.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, window_base); }) ||
	    !Check(result == 0, "update-chain resource id did not succeed") ||
	    !CaptureStderr(&output, [&] { result = Ime::KeyboardGetInfo(0u, window_base); }) ||
	    !Check(result == 0, "update-chain info did not succeed"))
	{
		return false;
	}
	refill();
	++update_call;
	expect_line(update_call, "null");
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardUpdate(nullptr); })) return false;
	if (!Check(result == UPDATE_REFUSED_RESULT && output == expected && pattern_intact(),
	           "chain update is not the measured refusal"))
	{
		return false;
	}
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, window_base); }) ||
	    !Check(result == 0, "the update refusal disturbed the live session"))
	{
		return false;
	}

	// The bounded reset clears the session and the update counter; the local
	// no-session state matches the measured after-close refusal.
	Ime::ResetSessionState();
	refill();
	update_call = 0;
	++update_call;
	expect_line(update_call, "function");
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardUpdate(window_base); })) return false;
	if (!Check(result == UPDATE_REFUSED_RESULT && output == expected && pattern_intact(),
	           "post-close update is not the measured refusal"))
	{
		return false;
	}

	// The aggregate HLE reset clears the session and the counter as well.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardOpen(1, window_base); }) ||
	    !Check(result == 0, "aggregate-reset update fixture open failed") ||
	    !Check(Lumen::Libs::ResetSessionState(), "aggregate HLE reset failed"))
	{
		return false;
	}
	refill();
	update_call = 0;
	++update_call;
	expect_line(update_call, "function");
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardUpdate(window_base); })) return false;
	if (!Check(result == UPDATE_REFUSED_RESULT && output == expected && pattern_intact(),
	           "aggregate-reset update is not the measured refusal"))
	{
		return false;
	}
	// The aggregated reset also cleared the live session latch.
	if (!CaptureStderr(&output, [&] { result = Ime::KeyboardGetResourceId(1, window_base); })) return false;
	return Check(result == RESOURCE_NO_SESSION_RESULT,
	             "aggregate reset did not clear the live session for the update step");
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
	if (std::strcmp(name, "open-profile") == 0) return TestOpenProfile();
	if (std::strcmp(name, "session-reset") == 0) return TestSessionReset();
	if (std::strcmp(name, "resource-id-profile") == 0) return TestResourceIdProfile();
	if (std::strcmp(name, "get-info-profile") == 0) return TestGetInfoProfile();
	if (std::strcmp(name, "update-profile") == 0) return TestUpdateProfile();
	if (std::strcmp(name, "init-all-disabled") == 0) return TestInitAll(false);
	if (std::strcmp(name, "init-all-enabled") == 0) return TestInitAll(true);
	if (std::strcmp(name, "fatal-open-absent") == 0)
		return SpawnFatalChild(executable, "fatal-open-disabled-child", OPEN_DISABLED_DIAGNOSTIC,
		                       OPEN_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-open-zero") == 0)
		return SpawnFatalChild(executable, "fatal-open-disabled-child", OPEN_DISABLED_DIAGNOSTIC,
		                       OPEN_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-resource-id-absent") == 0)
		return SpawnFatalChild(executable, "fatal-resource-id-disabled-child", RESOURCE_ID_DISABLED_DIAGNOSTIC,
		                       RESOURCE_ID_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-resource-id-zero") == 0)
		return SpawnFatalChild(executable, "fatal-resource-id-disabled-child", RESOURCE_ID_DISABLED_DIAGNOSTIC,
		                       RESOURCE_ID_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-resource-id-unwritable") == 0)
		return SpawnFatalChild(executable, "fatal-resource-id-unwritable-child", RESOURCE_ID_UNWRITABLE_DIAGNOSTIC,
		                       RESOURCE_ID_UNWRITABLE_FATAL);
	if (std::strcmp(name, "fatal-get-info-absent") == 0)
		return SpawnFatalChild(executable, "fatal-get-info-disabled-child", GET_INFO_DISABLED_DIAGNOSTIC,
		                       GET_INFO_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-get-info-zero") == 0)
		return SpawnFatalChild(executable, "fatal-get-info-disabled-child", GET_INFO_DISABLED_DIAGNOSTIC,
		                       GET_INFO_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-get-info-unwritable") == 0)
		return SpawnFatalChild(executable, "fatal-get-info-unwritable-child", GET_INFO_UNWRITABLE_DIAGNOSTIC,
		                       GET_INFO_UNWRITABLE_FATAL);
	if (std::strcmp(name, "fatal-update-absent") == 0)
		return SpawnFatalChild(executable, "fatal-update-disabled-child", UPDATE_DISABLED_DIAGNOSTIC,
		                       UPDATE_DISABLED_FATAL);
	if (std::strcmp(name, "fatal-update-zero") == 0)
		return SpawnFatalChild(executable, "fatal-update-disabled-child", UPDATE_DISABLED_DIAGNOSTIC,
		                       UPDATE_DISABLED_FATAL);
	return Check(false, "unknown test case");
}

} // namespace

int main(int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "fatal-open-disabled-child") == 0)
	{
		(void)Ime::KeyboardOpen(1, nullptr);
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-resource-id-disabled-child") == 0)
	{
		(void)Ime::KeyboardGetResourceId(1, nullptr);
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-resource-id-unwritable-child") == 0)
	{
		// The unmeasured non-null, unwritable output must stop fail-closed:
		// the child opens one session and passes a read-only guest page.
		Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
		Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
		Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
		Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
		Memory::Init();
		const uint64_t read_only = Memory::Alloc(0, 0x4000, Memory::Mode::ReadWrite);
		if (read_only == 0 || !Memory::Protect(read_only, 0x4000, Memory::Mode::Read))
		{
			std::fprintf(stderr, "ImeKeyboardTest: read-only fixture failed in child\n");
			return 4;
		}
		std::array<uint8_t, 64> window {};
		window.fill(0x5a);
		(void)Ime::KeyboardOpen(1, window.data());
		(void)Ime::KeyboardGetResourceId(1, reinterpret_cast<void*>(read_only + 128));
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-get-info-disabled-child") == 0)
	{
		(void)Ime::KeyboardGetInfo(0u, nullptr);
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-update-disabled-child") == 0)
	{
		(void)Ime::KeyboardUpdate(nullptr);
		return 4;
	}
	if (argc == 2 && std::strcmp(argv[1], "fatal-get-info-unwritable-child") == 0)
	{
		// The unmeasured non-null, unwritable output must stop fail-closed:
		// the child opens one session and passes a read-only guest page.
		Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
		Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
		Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
		Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
		Memory::Init();
		const uint64_t read_only = Memory::Alloc(0, 0x4000, Memory::Mode::ReadWrite);
		if (read_only == 0 || !Memory::Protect(read_only, 0x4000, Memory::Mode::Read))
		{
			std::fprintf(stderr, "ImeKeyboardTest: read-only fixture failed in child\n");
			return 4;
		}
		std::array<uint8_t, 64> window {};
		window.fill(0x5a);
		(void)Ime::KeyboardOpen(1, window.data());
		(void)Ime::KeyboardGetInfo(0u, reinterpret_cast<void*>(read_only + 128));
		return 4;
	}

	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	if (!Check(argc == 2, "expected exactly one test case") || !RunCase(argv[1], argv[0])) return 1;
	std::puts("ImeKeyboardTest: PASS");
	return 0;
}
