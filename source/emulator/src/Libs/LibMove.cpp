#include "Emulator/Libs/Move.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("libSceMove", 1, "libSceMove", 1, 1);

namespace Move {
namespace {

// Measured console results, campaigns LMNQ92804 and LMNQ92805 (28 September
// 2026, CUH-1216A fw 12.00, no Move device connected):
//   - a first sceMoveInit() returns 0 and a repeat returns 0x80ee0002;
//   - sceMoveOpen() returns the handle as its signed32 result and writes no
//     guest memory; a user that is not the console's logged-in one receives
//     0x80ee0006 without writes; a second concurrent open while a handle is
//     live receives 0x80ee0007 without consuming a handle (LMNQ92805);
//   - sceMoveGetDeviceInfo() on a live handle returns 1 and writes no guest
//     memory; a null output receives 0x80ee0003 and the handles 0 and -1
//     receive 0x80ee0004, all without writes (LMNQ92805);
//   - sceMoveReadStateRecent() on a live handle returns 1 and writes no
//     guest memory for the measured timestamps; a null count output receives
//     0x80ee0003; the handles 0, -1 and a closed handle receive 0x80ee0004
//     and the refusal stores four zero bytes into the count output; the data
//     pointer is never read or written (LMNQ92815).
// The local model keeps exactly one offline user (id 1), publishes one local
// handle per live port and never reads or writes the guest data windows; the
// only measured guest write is the four zero bytes the read-state refusal
// stores into the count output; the measured errors are reused unchanged.
constexpr int32_t kInitRepeatResult              = static_cast<int32_t>(0x80ee0002);
constexpr int32_t kOpenInvalidUser               = static_cast<int32_t>(0x80ee0006);
constexpr int32_t kOpenPortBusy                  = static_cast<int32_t>(0x80ee0007);
constexpr int32_t kGetDeviceInfoInvalidOutput    = static_cast<int32_t>(0x80ee0003);
constexpr int32_t kGetDeviceInfoInvalidHandle    = static_cast<int32_t>(0x80ee0004);
constexpr int32_t kGetDeviceInfoNoDeviceResult   = 1;
constexpr int32_t kReadStateRecentInvalidOutput  = static_cast<int32_t>(0x80ee0003);
constexpr int32_t kReadStateRecentInvalidHandle  = static_cast<int32_t>(0x80ee0004);
constexpr int32_t kReadStateRecentNoDeviceResult = 1;
constexpr int32_t kLocalUserId                   = 1;

std::mutex g_call_mutex;
bool g_initialized = false;
int32_t g_next_handle = 1;
int32_t g_live_handle = 0;
uint64_t g_open_calls = 0;
uint64_t g_info_calls = 0;
uint64_t g_read_calls = 0;

void PrintInitDiagnostic(const char* result, const char* call, const char* value, bool enabled)
{
	std::fprintf(stderr,
	             "[MOVE_INITIALIZE_NO_DEVICE_EXPERIMENT] result=%s call=%s value=%s "
	             "experimental=%d console_contract=measured device_model=none "
	             "resources=0 guest_memory=0\n",
	             result, call, value, enabled ? 1 : 0);
	std::fflush(stderr);
}

void PrintOpenDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=unavailable user=unavailable "
	             "result=disabled rc=unavailable model=local-user-no-device "
	             "console_contract=measured experimental=0 device_model=none "
	             "resources=0 guest_memory=0\n");
	std::fflush(stderr);
}

void PrintOpenDiagnostic(uint64_t call, int32_t user, int32_t handle)
{
	std::fprintf(stderr,
	             "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=%llu user=%d result=ok "
	             "handle=%d model=local-user-no-device console_contract=measured "
	             "experimental=1 device_model=none resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), user, handle);
	std::fflush(stderr);
}

void PrintOpenInvalidUserDiagnostic(uint64_t call, int32_t user)
{
	std::fprintf(stderr,
	             "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=%llu user=%d result=invalid-user "
	             "rc=80ee0006 model=local-user-no-device console_contract=measured "
	             "experimental=1 device_model=none resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), user);
	std::fflush(stderr);
}

void PrintOpenBusyDiagnostic(uint64_t call, int32_t user)
{
	std::fprintf(stderr,
	             "[MOVE_OPEN_NO_DEVICE_EXPERIMENT] call=%llu user=%d result=port-busy "
	             "rc=80ee0007 model=local-user-no-device console_contract=measured "
	             "experimental=1 device_model=none resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), user);
	std::fflush(stderr);
}

void PrintGetDeviceInfoDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=unavailable "
	             "handle=unavailable result=disabled rc=unavailable "
	             "model=local-user-no-device console_contract=measured experimental=0 "
	             "device_model=none resources=0 guest_memory=0\n");
	std::fflush(stderr);
}

void PrintGetDeviceInfoDiagnostic(uint64_t call, int32_t handle, const char* result, const char* rc)
{
	std::fprintf(stderr,
	             "[MOVE_GET_DEVICE_INFO_NO_DEVICE_EXPERIMENT] call=%llu handle=%d "
	             "result=%s rc=%s model=local-user-no-device console_contract=measured "
	             "experimental=1 device_model=none resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), handle, result, rc);
	std::fflush(stderr);
}

void PrintReadStateRecentDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=unavailable "
	             "handle=unavailable result=disabled rc=unavailable "
	             "model=local-user-no-device console_contract=measured experimental=0 "
	             "device_model=none resources=0 guest_memory=0\n");
	std::fflush(stderr);
}

void PrintReadStateRecentDiagnostic(uint64_t call, int32_t handle, const char* result, const char* rc)
{
	std::fprintf(stderr,
	             "[MOVE_READ_STATE_RECENT_NO_DEVICE_EXPERIMENT] call=%llu handle=%d "
	             "result=%s rc=%s model=local-user-no-device console_contract=measured "
	             "experimental=1 device_model=none resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), handle, result, rc);
	std::fflush(stderr);
}

} // namespace

bool ExperimentalEnabled()
{
	static const bool enabled = [] {
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE");
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
	g_initialized = false;
	g_next_handle = 1;
	g_live_handle = 0;
	g_open_calls = 0;
	g_info_calls = 0;
	g_read_calls = 0;
}

int32_t LUMEN_SYSV_ABI Initialize()
{
	if (!ExperimentalEnabled())
	{
		PrintInitDiagnostic("disabled", "unavailable", "unavailable", false);
		EXIT("Move Initialize no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	if (g_initialized)
	{
		PrintInitDiagnostic("repeat-measured", "repeat", "80ee0002", true);
		return kInitRepeatResult;
	}

	g_initialized = true;
	PrintInitDiagnostic("complete-local", "first", "0", true);
	return 0;
}

int32_t LUMEN_SYSV_ABI Open(const int32_t userId, int32_t /*type*/, int32_t /*index*/,
                            int32_t /*reserved*/, const void* /*param*/)
{
	if (!ExperimentalEnabled())
	{
		PrintOpenDisabledDiagnostic();
		EXIT("Move Open no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_open_calls;
	if (userId != kLocalUserId)
	{
		PrintOpenInvalidUserDiagnostic(call, userId);
		return kOpenInvalidUser;
	}
	// Measured refusal of the second concurrent open (LMNQ92805): with a live
	// handle the console admits no further port and consumes no handle. The
	// user check stays first (the measured refusal used the valid user); the
	// unmeasured combination is declared in the interface document.
	if (g_live_handle != 0)
	{
		PrintOpenBusyDiagnostic(call, userId);
		return kOpenPortBusy;
	}

	const int32_t handle = g_next_handle++;
	g_live_handle = handle;
	PrintOpenDiagnostic(call, userId, handle);
	return handle;
}

int32_t LUMEN_SYSV_ABI GetDeviceInfo(const int32_t handle, const void* info)
{
	if (!ExperimentalEnabled())
	{
		PrintGetDeviceInfoDisabledDiagnostic();
		EXIT("Move GetDeviceInfo no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_info_calls;
	// Measured on the console (LMNQ92805) with a live handle: null output
	// 0x80ee0003, handles 0 and -1 0x80ee0004, live handle 1 without writes.
	// The null check is declared first for the unmeasured null+invalid
	// combination; the local handle space is the published one.
	if (info == nullptr)
	{
		PrintGetDeviceInfoDiagnostic(call, handle, "invalid-output", "80ee0003");
		return kGetDeviceInfoInvalidOutput;
	}
	if (g_live_handle == 0 || handle != g_live_handle)
	{
		PrintGetDeviceInfoDiagnostic(call, handle, "invalid-handle", "80ee0004");
		return kGetDeviceInfoInvalidHandle;
	}

	PrintGetDeviceInfoDiagnostic(call, handle, "no-device", "1");
	return kGetDeviceInfoNoDeviceResult;
}

int32_t LUMEN_SYSV_ABI ReadStateRecent(const int32_t handle, const int64_t /*timestamp*/, const void* /*data*/,
                                       int32_t* num)
{
	if (!ExperimentalEnabled())
	{
		PrintReadStateRecentDisabledDiagnostic();
		EXIT("Move ReadStateRecent no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_read_calls;
	// Measured on the console (LMNQ92815) with no device: only the count
	// output is validated (a null count receives 0x80ee0003 without writes);
	// the timestamp and the data pointer are never read or written. The
	// null-count check is declared first for the unmeasured null+invalid
	// combination, mirroring the GetDeviceInfo profile.
	if (num == nullptr)
	{
		PrintReadStateRecentDiagnostic(call, handle, "invalid-output", "80ee0003");
		return kReadStateRecentInvalidOutput;
	}
	if (g_live_handle == 0 || handle != g_live_handle)
	{
		// Measured refusal: four zero bytes are stored into the count output.
		*num = 0;
		PrintReadStateRecentDiagnostic(call, handle, "invalid-handle", "80ee0004");
		return kReadStateRecentInvalidHandle;
	}

	PrintReadStateRecentDiagnostic(call, handle, "no-device", "1");
	return kReadStateRecentNoDeviceResult;
}

} // namespace Move

LIB_DEFINE(InitMove_1)
{
	if (!Move::ExperimentalEnabled()) return;

	Loader::SymbolResolve symbol {};
	symbol.library               = g_library;
	symbol.library_version       = g_library_version;
	symbol.module                = g_module;
	symbol.module_version_major  = g_module_version_major;
	symbol.module_version_minor  = g_module_version_minor;
	symbol.type                  = Loader::SymbolType::Func;

	symbol.name                  = "j1ITE-EoJmE";
	const auto init_target       = reinterpret_cast<uint64_t>(Move::Initialize);
	const char32_t* init_name    = U"Move::Initialize";
	constexpr auto init_metadata = Loader::MakeHostCallScalarMetadata<&Move::Initialize>();

	if (!s->RegisterHostCallTarget(init_target))
	{
		EXIT("Unable to register host-call target for %s", "j1ITE-EoJmE");
		return;
	}
	if (!s->SetHostCallScalarMetadata(init_target, init_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "j1ITE-EoJmE");
		return;
	}
	s->AddExact(symbol, init_target, init_name);

	symbol.name                  = "HzC60MfjJxU";
	const auto open_target       = reinterpret_cast<uint64_t>(Move::Open);
	const char32_t* open_name    = U"Move::Open";
	constexpr auto open_metadata = Loader::MakeHostCallScalarMetadata<&Move::Open>();

	if (!s->RegisterHostCallTarget(open_target))
	{
		EXIT("Unable to register host-call target for %s", "HzC60MfjJxU");
		return;
	}
	if (!s->SetHostCallScalarMetadata(open_target, open_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "HzC60MfjJxU");
		return;
	}
	s->AddExact(symbol, open_target, open_name);

	symbol.name                  = "GWXTyxs4QbE";
	const auto info_target       = reinterpret_cast<uint64_t>(Move::GetDeviceInfo);
	const char32_t* info_name    = U"Move::GetDeviceInfo";
	constexpr auto info_metadata = Loader::MakeHostCallScalarMetadata<&Move::GetDeviceInfo>();

	if (!s->RegisterHostCallTarget(info_target))
	{
		EXIT("Unable to register host-call target for %s", "GWXTyxs4QbE");
		return;
	}
	if (!s->SetHostCallScalarMetadata(info_target, info_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "GWXTyxs4QbE");
		return;
	}
	s->AddExact(symbol, info_target, info_name);

	symbol.name                  = "f2bcpK6kJfg";
	const auto read_target       = reinterpret_cast<uint64_t>(Move::ReadStateRecent);
	const char32_t* read_name    = U"Move::ReadStateRecent";
	constexpr auto read_metadata = Loader::MakeHostCallScalarMetadata<&Move::ReadStateRecent>();

	if (!s->RegisterHostCallTarget(read_target))
	{
		EXIT("Unable to register host-call target for %s", "f2bcpK6kJfg");
		return;
	}
	if (!s->SetHostCallScalarMetadata(read_target, read_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "f2bcpK6kJfg");
		return;
	}
	s->AddExact(symbol, read_target, read_name);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
