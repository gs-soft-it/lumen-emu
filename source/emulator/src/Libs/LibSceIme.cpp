#include "Emulator/Libs/Ime.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("libSceIme", 1, "libSceIme", 1, 1);

namespace Ime {
namespace {

// Measured console results, campaigns LMNQ92807 and LMNQ92808 (28 September
// 2026, CUH-1216A fw 12.00, keyboard session requested by a homebrew probe):
//   - the module loads with sceSysmoduleLoadModule(0x0095); a first
//     sceImeKeyboardOpen() with the console initial user and a non-null
//     parameter returns 0, opens the session and writes no guest memory,
//     while any open with a live session returns 0x80bc0001;
//   - a null parameter returns 0x80bc0031 before the live-session and user
//     checks (`(1, NULL)` with no session and `(initial, NULL)` with a live
//     session both received it);
//   - the users 0 and -1 return 0x80bc0004, any other positive user that is
//     not the console's initial one (measured 1 and 2) returns 0x80bc0010,
//     both without writes; with a live session the same users receive the
//     live-session refusal instead;
//   - no measured case writes the 256-byte parameter window, including the
//     accepted open (the parameter content is not validated by this call).
// Measured keyboard-resource-id results, campaign LMNQ92809 (28 September
// 2026, same console, two complete probe processes with identical outcomes):
//   - without a live session every call returns 0x80bc0002 and writes
//     nothing, for every user and for a null output (the live-session gate
//     precedes both the null-output check and the user classification);
//   - with a live session the session's user receives 0 and a 12-byte write:
//     the resource id 0x1186ef73 followed by two zero words (identical in
//     every success of both processes); a null output receives 0x80bc0031
//     before the user classification (measured with the session user, 1 and
//     0); any other user (measured 0, -1, 1 and 2) receives 0x80bc0010 with
//     no write, in particular 0 and -1 do not receive the open-time
//     0x80bc0004; after Close the call returns to the no-session refusal;
//   - no measured case writes outside the 12-byte record.
// Measured keyboard-info results, campaigns LMNQ92810 and LMNQ92811 (29
// September 2026, same console, two fresh processes with identical outcomes):
//   - without a live session every call returns 0x80bc0002 and writes
//     nothing, for every measured id and for a null output (the live-session
//     gate precedes both the null-output check and the id classification);
//   - with a live session a null output receives 0x80bc0031 before the id
//     classification (measured with 0 and 2);
//   - with a live session and a valid output only the ids 0 and 1 are
//     accepted (20-value sweep: small ids, powers of two, 0x7fffffff,
//     0x80000000, 0xfffffffe, 0xffffffff and the user-id value 0x1186ef73):
//     each accepted id receives 0 and its own 24-byte record (id 0:
//     0xffffffff followed by five zero words; id 1: 0xffffffff, 1, 0, 0, 0,
//     1); every other id receives -1 and a 36-byte run of zeroes;
//   - no measured case writes outside the published block.
// Measured keyboard-update results, campaigns LMNQ92812 and LMNQ92813 (29
// September 2026, two fresh probe processes with identical outcomes):
//   - every measured `sceImeUpdate(handler)` call returned 0x80bc0002: no
//     session, live session, null and real handler, after Close and after an
//     all-zero open parameter (the second process also ran the title-order
//     chain open, resource id, info before the updates);
//   - the probe handler was never invoked (13 calls, handler count 0) and
//     each call returned in 2-11 microseconds, so the measured call neither
//     blocks nor delivers events without user input;
//   - the refusal did not change the session: after the updates the measured
//     Close still succeeded on the live session.
// The gate that would make an update succeed was not identified; the local
// model therefore reproduces the measured refusal without claiming a
// successful-update semantics (no handler invocation, session latch
// unchanged).
// The LMNQ92813 probe also measured that an all-zero open parameter window is
// refused with 0x80bc0022 while the pattern window is accepted unchanged; the
// local open model keeps the declared null-only check (the parameter content
// is never read), and the interface document records the new limit.
// The local model keeps exactly one offline user (id 1, the identity already
// published by the local UserService profile), one live-session latch and the
// measured value reused unchanged; the interface document declares the local
// choices for the unmeasured combinations, including the fail-closed stop for
// a non-null output the guest cannot receive a write into.
constexpr int32_t kLocalUserId      = 1;
constexpr int32_t kNullParameter    = static_cast<int32_t>(0x80bc0031);
constexpr int32_t kSessionLive      = static_cast<int32_t>(0x80bc0001);
constexpr int32_t kInvalidUserId    = static_cast<int32_t>(0x80bc0004);
constexpr int32_t kUserNotFound     = static_cast<int32_t>(0x80bc0010);
constexpr int32_t kNoLiveSession    = static_cast<int32_t>(0x80bc0002);

// Measured code of every keyboard-update call (LMNQ92812/92813). It equals the
// no-live-session code of the other calls, but the console gate it belongs to
// was not identified: the same value was measured with and without a live
// session, so the local model reuses it as the measured refusal.
constexpr int32_t kUpdateRefused    = static_cast<int32_t>(0x80bc0002);

// First word of the measured 12-byte resource-id record; the console produced
// this value in every success of both measured processes.
constexpr uint32_t kResourceId          = 0x1186ef73u;
constexpr size_t   kResourceIdBytes     = 12;

// Measured keyboard-info record and refusal shapes (LMNQ92810/92811): the
// accepted ids 0 and 1 each publish their own 24-byte record, while every
// other id receives -1 after a 36-byte run of zeroes.
constexpr int32_t  kGetInfoRejected    = static_cast<int32_t>(0xffffffff);
constexpr uint32_t kGetInfoRecord0[6]  = {0xffffffffu, 0u, 0u, 0u, 0u, 0u};
constexpr uint32_t kGetInfoRecord1[6]  = {0xffffffffu, 1u, 0u, 0u, 0u, 1u};
constexpr size_t   kGetInfoRecordBytes = sizeof(kGetInfoRecord0);
constexpr size_t   kGetInfoRejectedBytes = 36;

std::mutex g_call_mutex;
uint64_t g_open_calls = 0;
uint64_t g_resource_id_calls = 0;
uint64_t g_get_info_calls = 0;
uint64_t g_update_calls = 0;
bool g_session_live = false;

void PrintOpenDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=unavailable user=unavailable "
	             "param=unavailable result=disabled rc=unavailable model=local-user-no-device "
	             "console_contract=measured experimental=0 device_model=none resources=0 "
	             "guest_memory=0\n");
	std::fflush(stderr);
}

void PrintOpenDiagnostic(uint64_t call, int32_t user, const char* param, const char* result, const char* rc)
{
	std::fprintf(stderr,
	             "[IME_KEYBOARD_OPEN_NO_DEVICE_EXPERIMENT] call=%llu user=%d param=%s "
	             "result=%s rc=%s model=local-user-no-device console_contract=measured "
	             "experimental=1 device_model=none resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), user, param, result, rc);
	std::fflush(stderr);
}

void PrintResourceIdDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[IME_KEYBOARD_GET_RESOURCE_ID_NO_DEVICE_EXPERIMENT] call=unavailable "
	             "user=unavailable output=unavailable session=unavailable result=disabled "
	             "rc=unavailable resource=none model=local-user-no-device "
	             "console_contract=measured experimental=0 device_model=none resources=0 "
	             "guest_memory=0\n");
	std::fflush(stderr);
}

void PrintResourceIdDiagnostic(uint64_t call, int32_t user, const char* output, const char* session,
                               const char* result, const char* rc, const char* resource, int guest_bytes)
{
	std::fprintf(stderr,
	             "[IME_KEYBOARD_GET_RESOURCE_ID_NO_DEVICE_EXPERIMENT] call=%llu user=%d "
	             "output=%s session=%s result=%s rc=%s resource=%s "
	             "model=local-user-no-device console_contract=measured experimental=1 "
	             "device_model=none resources=0 guest_memory=%d\n",
	             static_cast<unsigned long long>(call), user, output, session, result, rc, resource,
	             guest_bytes);
	std::fflush(stderr);
}

void PrintGetInfoDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[IME_KEYBOARD_GET_INFO_NO_DEVICE_EXPERIMENT] call=unavailable id=unavailable "
	             "output=unavailable session=unavailable result=disabled rc=unavailable record=none "
	             "model=local-user-no-device console_contract=measured experimental=0 device_model=none "
	             "resources=0 guest_memory=0\n");
	std::fflush(stderr);
}

void PrintGetInfoDiagnostic(uint64_t call, uint32_t resourceId, const char* output, const char* session,
                            const char* result, const char* rc, const char* record, int guest_bytes)
{
	std::fprintf(stderr,
	             "[IME_KEYBOARD_GET_INFO_NO_DEVICE_EXPERIMENT] call=%llu id=%u "
	             "output=%s session=%s result=%s rc=%s record=%s "
	             "model=local-user-no-device console_contract=measured experimental=1 "
	             "device_model=none resources=0 guest_memory=%d\n",
	             static_cast<unsigned long long>(call), resourceId, output, session, result, rc, record,
	             guest_bytes);
	std::fflush(stderr);
}

void PrintUpdateDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[IME_KEYBOARD_UPDATE_NO_DEVICE_EXPERIMENT] call=unavailable handler=unavailable "
	             "result=disabled rc=unavailable handler_invoked=0 model=local-user-no-device "
	             "console_contract=measured experimental=0 device_model=none resources=0 "
	             "guest_memory=0\n");
	std::fflush(stderr);
}

void PrintUpdateDiagnostic(uint64_t call, const char* handler)
{
	std::fprintf(stderr,
	             "[IME_KEYBOARD_UPDATE_NO_DEVICE_EXPERIMENT] call=%llu handler=%s "
	             "result=refused rc=80bc0002 handler_invoked=0 "
	             "model=local-user-no-device console_contract=measured experimental=1 "
	             "device_model=none resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), handler);
	std::fflush(stderr);
}

// Bounded guest write of a measured block. The guest range is validated
// before the write on the FEX targets and on Windows x64; the caller treats a
// failure as the unmeasured non-null, unwritable output.
bool WriteMeasuredBlock(void* output, const void* source, size_t bytes)
{
	const uint64_t address = reinterpret_cast<uint64_t>(output);
#if defined(__aarch64__) || \
	(LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::ValidateGuestMemory(address, bytes, true) &&
	       Loader::WriteGuestMemory(address, source, bytes);
#else
	if (output == nullptr || address > UINT64_MAX - bytes) return false;
	std::memcpy(output, source, bytes);
	return true;
#endif
}

} // namespace

bool ExperimentalEnabled()
{
	static const bool enabled = [] {
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE");
		return value != nullptr && std::strcmp(value, "1") == 0;
#else
		return false;
#endif
	}();
	return enabled;
}

void ResetSessionState()
{
	std::lock_guard lock(g_call_mutex);
	g_open_calls = 0;
	g_resource_id_calls = 0;
	g_get_info_calls = 0;
	g_update_calls = 0;
	g_session_live = false;
}

int32_t LUMEN_SYSV_ABI KeyboardOpen(const int32_t userId, const void* const param)
{
	if (!ExperimentalEnabled())
	{
		PrintOpenDisabledDiagnostic();
		EXIT("Ime Keyboard Open no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_open_calls;
	// Measured order (LMNQ92807/92808): the null-parameter refusal precedes
	// the live-session refusal, which precedes the user classification; the
	// parameter content is never read and no guest memory is written.
	if (param == nullptr)
	{
		PrintOpenDiagnostic(call, userId, "null", "invalid-param", "80bc0031");
		return kNullParameter;
	}
	if (g_session_live)
	{
		PrintOpenDiagnostic(call, userId, "window", "session-live", "80bc0001");
		return kSessionLive;
	}
	if (userId <= 0)
	{
		PrintOpenDiagnostic(call, userId, "window", "invalid-user-id", "80bc0004");
		return kInvalidUserId;
	}
	if (userId != kLocalUserId)
	{
		PrintOpenDiagnostic(call, userId, "window", "user-not-found", "80bc0010");
		return kUserNotFound;
	}

	g_session_live = true;
	PrintOpenDiagnostic(call, userId, "window", "ok", "0");
	return 0;
}

int32_t LUMEN_SYSV_ABI KeyboardGetResourceId(const int32_t userId, void* const resourceIdArray)
{
	if (!ExperimentalEnabled())
	{
		PrintResourceIdDisabledDiagnostic();
		EXIT("Ime Keyboard Get Resource Id no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_resource_id_calls;
	// Measured order (LMNQ92809): the live-session gate precedes the
	// null-output refusal, which precedes the user classification; only the
	// session's user is served, and 0 and -1 receive the same user-not-found
	// code as any other non-owner on this call.
	if (!g_session_live)
	{
		PrintResourceIdDiagnostic(call, userId, resourceIdArray == nullptr ? "null" : "window", "none",
		                          "no-session", "80bc0002", "none", 0);
		return kNoLiveSession;
	}
	if (resourceIdArray == nullptr)
	{
		PrintResourceIdDiagnostic(call, userId, "null", "live", "invalid-output", "80bc0031", "none", 0);
		return kNullParameter;
	}
	if (userId != kLocalUserId)
	{
		PrintResourceIdDiagnostic(call, userId, "window", "live", "user-not-found", "80bc0010", "none", 0);
		return kUserNotFound;
	}

	const uint32_t record[kResourceIdBytes / sizeof(uint32_t)] = {kResourceId, 0u, 0u};
	const bool wrote = WriteMeasuredBlock(resourceIdArray, record, kResourceIdBytes);
	if (!wrote)
	{
		// The console behaviour for a non-null, unwritable output was not
		// measured; record the stop reason and fail closed instead of
		// publishing an invented code.
		PrintResourceIdDiagnostic(call, userId, "window", "live", "unwritable-output", "unavailable", "none", 0);
		EXIT_NOT_IMPLEMENTED(wrote == false);
	}

	PrintResourceIdDiagnostic(call, userId, "window", "live", "ok", "0", "1186ef73",
	                          static_cast<int>(kResourceIdBytes));
	return 0;
}

// Measured keyboard-info profile (campaigns LMNQ92810/92811, 29 September
// 2026): the live-session gate precedes the null-output refusal, which
// precedes the id classification; only the ids 0 and 1 are accepted and each
// publishes its own 24-byte record, while every other id receives -1 with a
// 36-byte run of zeroes; no measured case writes outside the published block.
// The local model publishes the measured records unchanged and stops
// fail-closed when a non-null output is not writable (unmeasured combination).
int32_t LUMEN_SYSV_ABI KeyboardGetInfo(const uint32_t resourceId, void* const info)
{
	if (!ExperimentalEnabled())
	{
		PrintGetInfoDisabledDiagnostic();
		EXIT("Ime Keyboard Get Info no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_get_info_calls;
	// Measured order (LMNQ92810/92811): the live-session gate precedes both
	// the null-output refusal and the id classification.
	if (!g_session_live)
	{
		PrintGetInfoDiagnostic(call, resourceId, info == nullptr ? "null" : "window", "none", "no-session",
		                       "80bc0002", "none", 0);
		return kNoLiveSession;
	}
	if (info == nullptr)
	{
		PrintGetInfoDiagnostic(call, resourceId, "null", "live", "invalid-output", "80bc0031", "none", 0);
		return kNullParameter;
	}
	if (resourceId <= 1u)
	{
		const uint32_t* record       = resourceId == 0u ? kGetInfoRecord0 : kGetInfoRecord1;
		const char*     record_name  = resourceId == 0u ? "0" : "1";
		const bool      wrote        = WriteMeasuredBlock(info, record, kGetInfoRecordBytes);
		if (!wrote)
		{
			// The console behaviour for a non-null, unwritable output was not
			// measured; record the stop reason and fail closed instead of
			// publishing an invented code.
			PrintGetInfoDiagnostic(call, resourceId, "window", "live", "unwritable-output", "unavailable",
			                       "none", 0);
			EXIT_NOT_IMPLEMENTED(wrote == false);
		}
		PrintGetInfoDiagnostic(call, resourceId, "window", "live", "ok", "0", record_name,
		                       static_cast<int>(kGetInfoRecordBytes));
		return 0;
	}

	// Measured refusal for every id outside the accepted set: -1 and a 36-byte
	// run of zeroes inside the output window.
	const uint32_t zeroes[kGetInfoRejectedBytes / sizeof(uint32_t)] = {};
	const bool     wrote = WriteMeasuredBlock(info, zeroes, kGetInfoRejectedBytes);
	if (!wrote)
	{
		PrintGetInfoDiagnostic(call, resourceId, "window", "live", "unwritable-output", "unavailable", "none", 0);
		EXIT_NOT_IMPLEMENTED(wrote == false);
	}
	PrintGetInfoDiagnostic(call, resourceId, "window", "live", "unknown-resource", "ffffffff", "zeros",
	                       static_cast<int>(kGetInfoRejectedBytes));
	return kGetInfoRejected;
}

// Measured keyboard-update profile (campaigns LMNQ92812/92813, 29 September
// 2026): every measured call returned 0x80bc0002 - with and without a live
// session, with a null and a real handler, after Close and after the
// title-order chain (open, resource id, info) - and the probe handler was
// never invoked (13 calls, 0 invocations, 2-11 us per call). The gate that
// would make an update succeed was not identified; the local model reproduces
// the measured refusal, leaves the session latch unchanged and never invokes
// the guest handler (the pointer is only compared against null).
int32_t LUMEN_SYSV_ABI KeyboardUpdate(const void* handler)
{
	if (!ExperimentalEnabled())
	{
		PrintUpdateDisabledDiagnostic();
		EXIT("Ime Keyboard Update no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_update_calls;
	PrintUpdateDiagnostic(call, handler == nullptr ? "null" : "function");
	return kUpdateRefused;
}

} // namespace Ime

LIB_DEFINE(InitIme_1)
{
	if (!Ime::ExperimentalEnabled()) return;

	Loader::SymbolResolve symbol {};
	symbol.library               = g_library;
	symbol.library_version       = g_library_version;
	symbol.module                = g_module;
	symbol.module_version_major  = g_module_version_major;
	symbol.module_version_minor  = g_module_version_minor;
	symbol.type                  = Loader::SymbolType::Func;

	symbol.name                  = "eaFXjfJv3xs";
	const auto open_target       = reinterpret_cast<uint64_t>(Ime::KeyboardOpen);
	const char32_t* open_name    = U"Ime::KeyboardOpen";
	constexpr auto open_metadata = Loader::MakeHostCallScalarMetadata<&Ime::KeyboardOpen>();

	if (!s->RegisterHostCallTarget(open_target))
	{
		EXIT("Unable to register host-call target for %s", "eaFXjfJv3xs");
		return;
	}
	if (!s->SetHostCallScalarMetadata(open_target, open_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "eaFXjfJv3xs");
		return;
	}
	s->AddExact(symbol, open_target, open_name);

	symbol.name                  = "dKadqZFgKKQ";
	const auto resource_target   = reinterpret_cast<uint64_t>(Ime::KeyboardGetResourceId);
	const char32_t* resource_name = U"Ime::KeyboardGetResourceId";
	constexpr auto resource_metadata =
	    Loader::MakeHostCallScalarMetadata<&Ime::KeyboardGetResourceId>();

	if (!s->RegisterHostCallTarget(resource_target))
	{
		EXIT("Unable to register host-call target for %s", "dKadqZFgKKQ");
		return;
	}
	if (!s->SetHostCallScalarMetadata(resource_target, resource_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "dKadqZFgKKQ");
		return;
	}
	s->AddExact(symbol, resource_target, resource_name);

	symbol.name                 = "VkqLPArfFdc";
	const auto get_info_target  = reinterpret_cast<uint64_t>(Ime::KeyboardGetInfo);
	const char32_t* get_info_name = U"Ime::KeyboardGetInfo";
	constexpr auto get_info_metadata = Loader::MakeHostCallScalarMetadata<&Ime::KeyboardGetInfo>();

	if (!s->RegisterHostCallTarget(get_info_target))
	{
		EXIT("Unable to register host-call target for %s", "VkqLPArfFdc");
		return;
	}
	if (!s->SetHostCallScalarMetadata(get_info_target, get_info_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "VkqLPArfFdc");
		return;
	}
	s->AddExact(symbol, get_info_target, get_info_name);

	symbol.name                = "-4GCfYdNF1s";
	const auto update_target   = reinterpret_cast<uint64_t>(Ime::KeyboardUpdate);
	const char32_t* update_name = U"Ime::KeyboardUpdate";
	constexpr auto update_metadata = Loader::MakeHostCallScalarMetadata<&Ime::KeyboardUpdate>();

	if (!s->RegisterHostCallTarget(update_target))
	{
		EXIT("Unable to register host-call target for %s", "-4GCfYdNF1s");
		return;
	}
	if (!s->SetHostCallScalarMetadata(update_target, update_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "-4GCfYdNF1s");
		return;
	}
	s->AddExact(symbol, update_target, update_name);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
