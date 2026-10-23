#include "Emulator/Libs/Mouse.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("libSceMouse", 1, "libSceMouse", 1, 1);

namespace Mouse {
namespace {

// Measured console results, campaign LMNQ92806 (28 September 2026, CUH-1216A
// fw 12.00, no mouse device connected, module loaded explicitly):
//   - sceMouseInit() first call returns 0;
//   - sceMouseOpen() publishes the handle as its signed32 result and writes
//     no guest memory in any measured case; the console initial user opens on
//     (type 0, index 0) and (type 0, index 1), a repeat on the same index
//     receives 0x80df0004, the users 0 and 1 receive 0x809b0081, the user -1
//     receives 0x809b0001 and type 1 receives 0x80df0001 (the type check
//     precedes the live-slot refusal);
//   - sceMouseClose() on a measured handle returns 0 (receipt only; Close is
//     not modeled by this profile).
// Measured console results, campaigns LMNQ92816 and LMNQ92817 (29 September
// 2026, same console and conditions):
//   - sceMouseRead() on a live handle with num in 1..64 returns 1 and stores
//     forty zero bytes (the device-less record) into the data window for
//     every measured num; a null data pointer and num 0, -1 or 65 receive
//     0x80df0001 without writes; the handles 0, -1 and a closed handle
//     receive 0x80df0003 without writes.
// The local model keeps exactly one offline user (id 1, the identity already
// published by the local UserService profile), the two measured index slots,
// one local handle per live slot and never reads the trailing parameter; the
// measured errors are reused unchanged. The interface documents declare the
// local choices for the combinations the campaigns did not measure.
constexpr int32_t kLocalUserId      = 1;
constexpr int32_t kOpenUserNotFound = static_cast<int32_t>(0x809b0081);
constexpr int32_t kOpenUserArgument = static_cast<int32_t>(0x809b0001);
constexpr int32_t kOpenInvalidType  = static_cast<int32_t>(0x80df0001);
constexpr int32_t kOpenPortBusy     = static_cast<int32_t>(0x80df0004);
constexpr int kSlotCount            = 2;

// Measured no-device read profile (LMNQ92816/LMNQ92817).
constexpr int32_t kReadInvalidArgument = static_cast<int32_t>(0x80df0001);
constexpr int32_t kReadInvalidHandle   = static_cast<int32_t>(0x80df0003);
constexpr int32_t kReadMinNum          = 1;
constexpr int32_t kReadMaxNum          = 64;
constexpr int32_t kReadNoDeviceResult  = 1;
constexpr size_t kReadRecordSize       = 40;

std::mutex g_call_mutex;
uint8_t g_diagnostic_calls = 0;
int32_t g_live_slots[kSlotCount] {};
int32_t g_next_handle = 1;
uint64_t g_open_calls = 0;
uint64_t g_read_calls = 0;

void PrintDiagnostic(const char* result, const char* call, const char* value, bool enabled)
{
	std::fprintf(stderr,
	             "[MOUSE_INITIALIZE_NO_DEVICE_EXPERIMENT] result=%s call=%s value=%s "
	             "experimental=%d console_contract=measured-first device_model=none "
	             "host_input=untouched resources=0 guest_memory=0\n",
	             result, call, value, enabled ? 1 : 0);
	std::fflush(stderr);
}

void PrintOpenDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=unavailable user=unavailable "
	             "type=unavailable index=unavailable result=disabled rc=unavailable "
	             "model=local-user-no-device console_contract=measured experimental=0 "
	             "device_model=none resources=0 guest_memory=0\n");
	std::fflush(stderr);
}

void PrintOpenDiagnostic(uint64_t call, const char* result, const char* rc, int32_t user,
                         int32_t type, int32_t index, int32_t handle)
{
	std::fprintf(stderr,
	             "[MOUSE_OPEN_NO_DEVICE_EXPERIMENT] call=%llu user=%d type=%d index=%d "
	             "result=%s handle=%d rc=%s model=local-user-no-device "
	             "console_contract=measured experimental=1 device_model=none "
	             "resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), user, type, index, result, handle, rc);
	std::fflush(stderr);
}

void PrintReadDisabledDiagnostic()
{
	std::fprintf(stderr,
	             "[MOUSE_READ_NO_DEVICE_EXPERIMENT] call=unavailable handle=unavailable "
	             "num=unavailable result=disabled rc=unavailable model=local-user-no-device "
	             "console_contract=measured experimental=0 device_model=none resources=0 "
	             "guest_memory=0\n");
	std::fflush(stderr);
}

void PrintReadDiagnostic(uint64_t call, int32_t handle, int32_t num, const char* result, const char* rc)
{
	std::fprintf(stderr,
	             "[MOUSE_READ_NO_DEVICE_EXPERIMENT] call=%llu handle=%d num=%d result=%s "
	             "rc=%s model=local-user-no-device console_contract=measured experimental=1 "
	             "device_model=none resources=0 guest_memory=0\n",
	             static_cast<unsigned long long>(call), handle, num, result, rc);
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
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE");
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
	g_diagnostic_calls = 0;
	for (int32_t& slot: g_live_slots) slot = 0;
	g_next_handle = 1;
	g_open_calls = 0;
	g_read_calls = 0;
}

int32_t LUMEN_SYSV_ABI Initialize()
{
	if (!ExperimentalEnabled())
	{
		PrintDiagnostic("disabled", "unavailable", "unavailable", false);
		EXIT("Mouse Initialize no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	if (g_diagnostic_calls < 2)
	{
		PrintDiagnostic("complete-local", g_diagnostic_calls == 0 ? "first" : "repeat", "0", true);
		++g_diagnostic_calls;
	}
	return int32_t {0};
}

int32_t LUMEN_SYSV_ABI Open(const int32_t userId, const int32_t type, const int32_t index,
                            const void* /*param*/)
{
	if (!ExperimentalEnabled())
	{
		PrintOpenDisabledDiagnostic();
		EXIT("Mouse Open no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_open_calls;
	// Measured order (LMNQ92806): the user check precedes the live-slot
	// refusal (the users 0 and -1 were rejected while a handle was live) and
	// the type check precedes it too (type 1 received its own code with the
	// same live handle). The rejected index domain and the null parameter are
	// unmeasured combinations; the local model maps any index outside the two
	// measured slots to the measured invalid-argument code and never reads
	// the parameter, as declared in the interface document.
	if (userId != kLocalUserId)
	{
		const bool argument = userId < 0;
		PrintOpenDiagnostic(call, "invalid-user", argument ? "809b0001" : "809b0081", userId, type,
		                    index, 0);
		return argument ? kOpenUserArgument : kOpenUserNotFound;
	}
	if (type != 0)
	{
		PrintOpenDiagnostic(call, "invalid-type", "80df0001", userId, type, index, 0);
		return kOpenInvalidType;
	}
	if (index < 0 || index >= kSlotCount)
	{
		PrintOpenDiagnostic(call, "invalid-index", "80df0001", userId, type, index, 0);
		return kOpenInvalidType;
	}
	if (g_live_slots[index] != 0)
	{
		PrintOpenDiagnostic(call, "port-busy", "80df0004", userId, type, index, 0);
		return kOpenPortBusy;
	}

	const int32_t handle = g_next_handle++;
	g_live_slots[index] = handle;
	PrintOpenDiagnostic(call, "ok", "0", userId, type, index, handle);
	return handle;
}

int32_t LUMEN_SYSV_ABI Read(const int32_t handle, void* data, const int32_t num)
{
	if (!ExperimentalEnabled())
	{
		PrintReadDisabledDiagnostic();
		EXIT("Mouse Read no-device experiment is disabled");
		std::abort();
	}

	std::lock_guard lock(g_call_mutex);
	const uint64_t call = ++g_read_calls;
	// Measured on the console with no device (LMNQ92816/LMNQ92817): num 1..64
	// and a live handle return 1 with the same forty zero bytes stored as the
	// device-less record (num=1 stores forty bytes too); a null data pointer
	// and a num outside 1..64 receive 0x80df0001 without writes; handle 0, -1
	// and a closed handle receive 0x80df0003 without writes. The
	// argument-before-handle order for the unmeasured combined cases is a
	// declared local choice, mirroring the Move read profile.
	if (data == nullptr || num < kReadMinNum || num > kReadMaxNum)
	{
		PrintReadDiagnostic(call, handle, num, "invalid-argument", "80df0001");
		return kReadInvalidArgument;
	}

	bool live = false;
	for (const int32_t slot: g_live_slots) live = live || (slot != 0 && slot == handle);
	if (!live)
	{
		PrintReadDiagnostic(call, handle, num, "invalid-handle", "80df0003");
		return kReadInvalidHandle;
	}

	std::memset(data, 0, kReadRecordSize);
	PrintReadDiagnostic(call, handle, num, "no-device", "1");
	return kReadNoDeviceResult;
}

} // namespace Mouse

LIB_DEFINE(InitMouse_1)
{
	if (!Mouse::ExperimentalEnabled()) return;

	Loader::SymbolResolve symbol {};
	symbol.library               = g_library;
	symbol.library_version       = g_library_version;
	symbol.module                = g_module;
	symbol.module_version_major  = g_module_version_major;
	symbol.module_version_minor  = g_module_version_minor;
	symbol.type                  = Loader::SymbolType::Func;

	symbol.name                  = "Qs0wWulgl7U";
	const auto init_target       = reinterpret_cast<uint64_t>(Mouse::Initialize);
	const char32_t* init_name    = U"Mouse::Initialize";
	constexpr auto init_metadata = Loader::MakeHostCallScalarMetadata<&Mouse::Initialize>();

	if (!s->RegisterHostCallTarget(init_target))
	{
		EXIT("Unable to register host-call target for %s", "Qs0wWulgl7U");
		return;
	}
	if (!s->SetHostCallScalarMetadata(init_target, init_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "Qs0wWulgl7U");
		return;
	}
	s->AddExact(symbol, init_target, init_name);

	symbol.name                  = "RaqxZIf6DvE";
	const auto open_target       = reinterpret_cast<uint64_t>(Mouse::Open);
	const char32_t* open_name    = U"Mouse::Open";
	constexpr auto open_metadata = Loader::MakeHostCallScalarMetadata<&Mouse::Open>();

	if (!s->RegisterHostCallTarget(open_target))
	{
		EXIT("Unable to register host-call target for %s", "RaqxZIf6DvE");
		return;
	}
	if (!s->SetHostCallScalarMetadata(open_target, open_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "RaqxZIf6DvE");
		return;
	}
	s->AddExact(symbol, open_target, open_name);

	symbol.name                  = "x8qnXqh-tiM";
	const auto read_target       = reinterpret_cast<uint64_t>(Mouse::Read);
	const char32_t* read_name    = U"Mouse::Read";
	constexpr auto read_metadata = Loader::MakeHostCallScalarMetadata<&Mouse::Read>();

	if (!s->RegisterHostCallTarget(read_target))
	{
		EXIT("Unable to register host-call target for %s", "x8qnXqh-tiM");
		return;
	}
	if (!s->SetHostCallScalarMetadata(read_target, read_metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "x8qnXqh-tiM");
		return;
	}
	s->AddExact(symbol, read_target, read_name);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
