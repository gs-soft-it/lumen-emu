#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Loader/SystemContent.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("AppContent", 1, "AppContentUtil", 1, 1);

namespace AppContent {

struct AppContentInitParam
{
	char reserved[32];
};

struct AppContentBootParam
{
	char     reserved1[4];
	uint32_t attr;
	char     reserved2[32];
};

struct NpUnifiedEntitlementLabel
{
	char data[17];
	char padding[3];
};

struct AppContentAddcontInfo
{
	NpUnifiedEntitlementLabel entitlement_label;
	uint32_t                  status;
};

struct AppContentMountPoint
{
	char data[16];
};

static_assert(sizeof(AppContentMountPoint) == 16);

namespace {

// No permissive public PS4 header located for this service currently provides
// its complete ABI. These are deliberately narrow local-profile results, not
// a claim of complete console error equivalence.
constexpr int32_t APP_CONTENT_ERROR_PARAMETER = static_cast<int32_t>(0x80d90002u);
constexpr int32_t APP_CONTENT_ERROR_INTERNAL  = static_cast<int32_t>(0x80d9000au);
constexpr uint32_t TEMPORARY_DATA_OPTION_NONE   = 0;
constexpr uint32_t TEMPORARY_DATA_OPTION_FORMAT = 1;
constexpr char32_t TEMPORARY_DATA_MOUNT_POINT[] = U"/temp0";
constexpr char TEMPORARY_DATA_MOUNT_TEXT[]      = "/temp0";
constexpr uint64_t TEMPORARY_DATA_DIRECTORY_LIMIT = 1024;

std::mutex g_temporary_data_mutex;
String     g_temporary_data_directory;
uint64_t   g_temporary_data_directory_sequence = 0;

bool IsCurrentTemporaryDataMountLocked()
{
	if (g_temporary_data_directory.IsEmpty()) return false;
	const String probe = U".lumen-appcontent-mount";
	return LibKernel::FileSystem::GetRealFilename(String(TEMPORARY_DATA_MOUNT_POINT).FixDirectorySlash() + probe) ==
	       g_temporary_data_directory.FixDirectorySlash() + probe;
}

bool CreateTemporaryDataDirectoryLocked(String* directory)
{
	for (uint64_t attempt = 0; attempt < TEMPORARY_DATA_DIRECTORY_LIMIT; ++attempt)
	{
		if (g_temporary_data_directory_sequence == std::numeric_limits<uint64_t>::max()) return false;
		char name[96] {};
		const int length = std::snprintf(name, sizeof(name), "_AppContentTemporaryData.%" PRIu64,
		                                 g_temporary_data_directory_sequence++);
		if (length <= 0 || static_cast<size_t>(length) >= sizeof(name)) return false;
		const String candidate = String::FromUtf8(name);
		if (Core::File::CreateDirectory(candidate) && Core::File::IsDirectoryExisting(candidate))
		{
			*directory = candidate;
			return true;
		}
	}
	return false;
}

// The available-space family publishes a 64-bit result and consumes the same
// bounded mount-point record the temporary-data mount publishes. Both helpers
// mirror the guest-memory policy of the mount profile: validate the complete
// range before any read or write, with a direct host fallback only on the
// targets where the guest shares the host address space.
bool ReadGuestMountPointRecord(const AppContentMountPoint* mount_point, AppContentMountPoint* record)
{
	const uint64_t address = reinterpret_cast<uint64_t>(mount_point);
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::ValidateGuestMemory(address, sizeof(*record), false) &&
	       Loader::ReadGuestMemory(address, record, sizeof(*record));
#else
	if (mount_point == nullptr || address > std::numeric_limits<uint64_t>::max() - sizeof(*record)) return false;
	std::memcpy(record, mount_point, sizeof(*record));
	return true;
#endif
}

bool WriteGuestAvailableSpaceKb(uint64_t* available_space_kb, uint64_t value)
{
	const uint64_t address = reinterpret_cast<uint64_t>(available_space_kb);
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::ValidateGuestMemory(address, sizeof(value), true) && Loader::WriteGuestMemory(address, &value, sizeof(value));
#else
	if (available_space_kb == nullptr || address > std::numeric_limits<uint64_t>::max() - sizeof(value)) return false;
	std::memcpy(available_space_kb, &value, sizeof(value));
	return true;
#endif
}

bool WriteTemporaryDataMountPoint(AppContentMountPoint* mount_point)
{
	AppContentMountPoint output {};
	static_assert(sizeof(TEMPORARY_DATA_MOUNT_TEXT) <= sizeof(output.data));
	std::memcpy(output.data, TEMPORARY_DATA_MOUNT_TEXT, sizeof(TEMPORARY_DATA_MOUNT_TEXT));
	const uint64_t address = reinterpret_cast<uint64_t>(mount_point);
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	return Loader::ValidateGuestMemory(address, sizeof(output), true) &&
	       Loader::WriteGuestMemory(address, &output, sizeof(output));
#else
	if (mount_point == nullptr || address > std::numeric_limits<uint64_t>::max() - sizeof(output)) return false;
	std::memcpy(mount_point, &output, sizeof(output));
	return true;
#endif
}

} // namespace

int LUMEN_SYSV_ABI AppContentInitialize(const AppContentInitParam* init_param, AppContentBootParam* boot_param)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(init_param == nullptr);
	EXIT_NOT_IMPLEMENTED(boot_param == nullptr);

	boot_param->attr = 0;

	return OK;
}

int LUMEN_SYSV_ABI AppContentGetAddcontInfoList(uint32_t service_label, AppContentAddcontInfo* /*list*/, uint32_t list_num,
                                               uint32_t* hit_num)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(hit_num == nullptr);

	printf("\t service_label = %u\n", service_label);
	printf("\t list_num      = %u\n", list_num);

	*hit_num = 0;

	return OK;
}

int LUMEN_SYSV_ABI AppContentAppParamGetInt(uint32_t param_id, int32_t* value)
{
	PRINT_NAME();

	EXIT_NOT_IMPLEMENTED(value == nullptr);

	*value     = 0;
	bool found = false;

	printf("\t param_id = %u\n", param_id);

	switch (param_id)
	{
		case 0:
			*value = 3;
			found  = true;
			break;
		case 1: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_1", value); break;
		case 2: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_2", value); break;
		case 3: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_3", value); break;
		case 4: found = Loader::SystemContentParamSfoGetInt("USER_DEFINED_PARAM_4", value); break;
		default: EXIT("unknown param_id: %u\n", param_id);
	}

	printf("\t value    = %d [%s]\n", *value, found ? "found" : "not found");

	return OK;
}

int32_t LUMEN_SYSV_ABI AppContentTemporaryDataMount2(uint32_t option, AppContentMountPoint* mount_point)
{
	PRINT_NAME();

	if ((option != TEMPORARY_DATA_OPTION_NONE && option != TEMPORARY_DATA_OPTION_FORMAT) || mount_point == nullptr)
		return APP_CONTENT_ERROR_PARAMETER;

	// Prevalidate the complete local output record before creating or changing a
	// mount. A rejected guest pointer therefore cannot consume a directory or
	// disturb the current /temp0 translation.
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	if (!Loader::ValidateGuestMemory(reinterpret_cast<uint64_t>(mount_point), sizeof(*mount_point), true))
		return APP_CONTENT_ERROR_PARAMETER;
#endif

	std::lock_guard<std::mutex> lock(g_temporary_data_mutex);
	const bool current = IsCurrentTemporaryDataMountLocked();
	if (option == TEMPORARY_DATA_OPTION_NONE && current)
	{
		return WriteTemporaryDataMountPoint(mount_point) ? OK : APP_CONTENT_ERROR_PARAMETER;
	}

	String new_directory;
	if (!CreateTemporaryDataDirectoryLocked(&new_directory)) return APP_CONTENT_ERROR_INTERNAL;
	if (!WriteTemporaryDataMountPoint(mount_point))
	{
		(void)Core::File::DeleteDirectory(new_directory);
		return APP_CONTENT_ERROR_PARAMETER;
	}

	// FORMAT rotates /temp0 to a fresh empty directory. The previous session
	// directory is intentionally left private under the already-isolated host
	// working directory instead of recursively deleting guest-controlled data.
	LibKernel::FileSystem::Mount(new_directory, TEMPORARY_DATA_MOUNT_POINT);
	g_temporary_data_directory = std::move(new_directory);
	return OK;
}

int32_t LUMEN_SYSV_ABI AppContentDownloadDataGetAvailableSpaceKb(const AppContentMountPoint* mount_point,
                                                                 uint64_t*                available_space_kb)
{
	PRINT_NAME();

	// Local profile: the runtime offers a single writable volume, the isolated
	// session working directory that also backs the temporary-data directories.
	// Every well-formed mount-point record is answered from that volume instead
	// of reporting the console quota or a fabricated constant. The record is
	// read as a bounded NUL-terminated name; an unreadable, unterminated or
	// empty record is rejected before measuring anything.
	AppContentMountPoint record {};
	if (mount_point == nullptr || available_space_kb == nullptr || !ReadGuestMountPointRecord(mount_point, &record))
		return APP_CONTENT_ERROR_PARAMETER;

	const auto* const terminator = static_cast<const char*>(std::memchr(record.data, 0, sizeof(record.data)));
	if (terminator == nullptr || terminator == record.data) return APP_CONTENT_ERROR_PARAMETER;

	uint64_t available_kb = 0;
	if (!Core::File::GetAvailableSpaceKb(U".", &available_kb)) return APP_CONTENT_ERROR_INTERNAL;

	printf("\t mount_point = %s\n", record.data);
	printf("\t available   = %" PRIu64 " KiB\n", available_kb);

	// The complete output range was checked by the write helper; a rejected
	// write publishes no partial value.
	return WriteGuestAvailableSpaceKb(available_space_kb, available_kb) ? OK : APP_CONTENT_ERROR_PARAMETER;
}

} // namespace AppContent

LIB_DEFINE(InitAppContent_1)
{
	LIB_FUNC("R9lA82OraNs", AppContent::AppContentInitialize);
	LIB_FUNC("xnd8BJzAxmk", AppContent::AppContentGetAddcontInfoList);
	LIB_FUNC("99b82IKXpH4", AppContent::AppContentAppParamGetInt);
	LIB_FUNC_SCALAR("buYbeLOGWmA", AppContent::AppContentTemporaryDataMount2);
	LIB_FUNC_SCALAR("Gl6w5i0JokY", AppContent::AppContentDownloadDataGetAvailableSpaceKb);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
