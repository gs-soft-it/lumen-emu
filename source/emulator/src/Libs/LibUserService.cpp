#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/UserService.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("UserService", 1, "UserService", 1, 1);

namespace UserService {

namespace {

std::atomic<bool> g_login_event_delivered {false};

// The colour family publishes a 32-bit SceUserServiceUserColor value into the
// caller's writable dword. Validate the complete output range before any
// write, with a direct host fallback only on the targets where the guest
// shares the host address space.
bool WriteGuestUserColor(int32_t* color, int32_t value)
{
	const uint64_t address = reinterpret_cast<uint64_t>(color);
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::ValidateGuestMemory(address, sizeof(value), true) &&
	       Loader::WriteGuestMemory(address, &value, sizeof(value));
#else
	if (color == nullptr || address > std::numeric_limits<uint64_t>::max() - sizeof(value)) return false;
	std::memcpy(color, &value, sizeof(value));
	return true;
#endif
}

// First value of the documented SceUserServiceUserColor enumeration. The
// offline profile keeps this colour for its single logged-in user; the console
// value depends on the colour stored in the user profile and is not measured.
constexpr int32_t USER_SERVICE_USER_COLOR_BLUE = 0;

// The acceptance is recorded only to make it observable in a run. The bound
// mirrors the other bounded diagnostic traces of the runtime.
constexpr int kTraceLimit = 32;
std::atomic<int> g_user_color_trace_count {0};

} // namespace

struct UserServiceLoginUserIdList
{
	int user_id[4];
};

enum UserServiceEventType
{
	UserServiceEventTypeLogin,
	UserServiceEventTypeLogout
};

struct SceUserServiceEvent
{
	UserServiceEventType event_type;
	int                  user_id;
};

static LUMEN_SYSV_ABI int UserServiceInitialize(const void* /*params*/)
{
	PRINT_NAME();

	return OK;
}

static LUMEN_SYSV_ABI int UserServiceGetInitialUser(int* user_id)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(user_id == nullptr);

	*user_id = 1;

	return OK;
}

static LUMEN_SYSV_ABI int UserServiceGetEvent(SceUserServiceEvent* event)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(event == nullptr);

	if (!g_login_event_delivered.exchange(true, std::memory_order_acq_rel))
	{
		event->event_type = UserServiceEventTypeLogin;
		event->user_id    = 1;
		return OK;
	}

	return USER_SERVICE_ERROR_NO_EVENT;
}

static LUMEN_SYSV_ABI int UserServiceGetLoginUserIdList(UserServiceLoginUserIdList* user_id_list)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(user_id_list == nullptr);

	user_id_list->user_id[0] = 1;
	user_id_list->user_id[1] = -1;
	user_id_list->user_id[2] = -1;
	user_id_list->user_id[3] = -1;

	return OK;
}

static LUMEN_SYSV_ABI int UserServiceGetUserName(int user_id, char* name, size_t size)
{
	EXIT_NOT_IMPLEMENTED(user_id != 1);
	EXIT_NOT_IMPLEMENTED(size < 5);

	int s = snprintf(name, size, "%s", "Lumen");

	EXIT_NOT_IMPLEMENTED(static_cast<size_t>(s) >= size);

	return OK;
}

int32_t LUMEN_SYSV_ABI UserServiceGetUserColor(int32_t user_id, int32_t* color)
{
	PRINT_NAME();

	// Local profile: the offline model keeps one logged-in user (the identity
	// published by UserServiceGetInitialUser and UserServiceGetLoginUserIdList)
	// and answers with the first value of the documented colour enumeration.
	// A rejected output or user publishes nothing; other ids are not logged in.
	if (color == nullptr) return USER_SERVICE_ERROR_INVALID_ARGUMENT;
	if (user_id != 1) return USER_SERVICE_ERROR_NOT_LOGGED_IN;
	if (!WriteGuestUserColor(color, USER_SERVICE_USER_COLOR_BLUE)) return USER_SERVICE_ERROR_INVALID_ARGUMENT;

	const int count = g_user_color_trace_count.fetch_add(1, std::memory_order_relaxed);
	if (count < kTraceLimit)
	{
		std::fprintf(stderr,
		             "[USER_SERVICE_USER_COLOR] call=%d user=%d color=%d model=local-profile console_contract=unverified\n",
		             count + 1, static_cast<int>(user_id), USER_SERVICE_USER_COLOR_BLUE);
		std::fflush(stderr);
	}

	return OK;
}

bool ResetForNextSession()
{
	// Guest execution is idle at the aggregate HLE reset boundary. The atomic
	// also makes the one-login-event contract deterministic if guest callers
	// query concurrently within a session.
	g_login_event_delivered.store(false, std::memory_order_release);
	return true;
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SessionResetContractTest()
{
	SceUserServiceEvent first {};
	SceUserServiceEvent no_event {};
	SceUserServiceEvent second {};
	return ResetForNextSession() && UserServiceGetEvent(&first) == OK && first.event_type == UserServiceEventTypeLogin &&
	       first.user_id == 1 && UserServiceGetEvent(&no_event) == USER_SERVICE_ERROR_NO_EVENT && ResetForNextSession() &&
	       UserServiceGetEvent(&second) == OK && second.event_type == UserServiceEventTypeLogin && second.user_id == 1 &&
	       ResetForNextSession();
}
#endif

} // namespace UserService

LIB_DEFINE(InitUserService_1)
{
	LIB_FUNC("j3YMu1MVNNo", UserService::UserServiceInitialize);
	LIB_FUNC("CdWp0oHWGr0", UserService::UserServiceGetInitialUser);
	LIB_FUNC("yH17Q6NWtVg", UserService::UserServiceGetEvent);
	LIB_FUNC("fPhymKNvK-A", UserService::UserServiceGetLoginUserIdList);
	LIB_FUNC("1xxcMiGu2fo", UserService::UserServiceGetUserName);
	LIB_FUNC_SCALAR("lUoqwTQu4Go", UserService::UserServiceGetUserColor);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
