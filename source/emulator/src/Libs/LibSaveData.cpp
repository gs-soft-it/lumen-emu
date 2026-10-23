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
#include "Lumen/Core/VirtualMemory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <sys/attr.h>
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <sys/file.h>
#include <sys/xattr.h>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("SaveData", 1, "SaveData", 1, 1);

namespace SaveData {

// TODO(): specify dir at launcher
static constexpr char32_t SAVE_DATA_DIR[]   = U"_SaveData";
static constexpr char32_t SAVE_DATA_POINT[] = U"/savedata0";

struct SceSaveDataDirName
{
	char data[32];
};

struct SaveDataMountPoint
{
	char data[16];
};

struct SaveDataMount
{
	int         user_id;
	int         pad;
	const char* title_id;
	const char* dir_name;
	const char* fingerprint;
	uint64_t    blocks;
	uint32_t    mount_mode;
	uint8_t     reserved[32];
};

struct SaveDataMount2
{
	int                       user_id;
	int                       pad;
	const SceSaveDataDirName* dir_name;
	uint64_t                  blocks;
	uint32_t                  mount_mode;
	uint8_t                   reserved[32];
	int                       pad2;
};

struct SaveDataMountResult
{
	SaveDataMountPoint mount_point;
	uint64_t           required_blocks;
	uint32_t           unused;
	uint32_t           mount_status;
	uint8_t            reserved[28];
	int                pad;
};

struct SaveDataParam
{
	char     title[128];
	char     sub_title[128];
	char     detail[1024];
	uint32_t user_param;
	int      pad;
	int64_t  mtime;
	uint8_t  reserved[32];
};

struct SaveDataMountInfo
{
	uint64_t blocks;
	uint64_t free_blocks;
	uint8_t  reserved[32];
};

struct SaveDataDirNameSearchCondition
{
	int32_t                    user_id;
	uint32_t                   pad;
	const char*                title_id;
	const SceSaveDataDirName*  dir_name;
	uint32_t                   sort_key;
	uint32_t                   sort_order;
	uint8_t                    reserved[32];
};

struct SaveDataDirNameSearchResult
{
	uint32_t             hit_num;
	uint32_t             pad;
	SceSaveDataDirName*   dir_names;
	uint32_t             dir_names_num;
	uint32_t             set_num;
	SaveDataParam*        params;
	SaveDataMountInfo*    infos;
	uint8_t              reserved[12];
	uint32_t             pad2;
};

struct SaveDataIcon
{
	void*   buf;
	size_t  buf_size;
	size_t  data_size;
	uint8_t reserved[32];
};

// SaveDataMemory has several public declarations in the wild whose reserved
// tail extents disagree. Only the field prefixes below are consumed. This is
// deliberate: the observed ABI agrees on every accessed offset, while reading
// sizeof one disputed outer record could cross a valid guest allocation.
struct SaveDataMemoryData
{
	uint64_t buffer;
	uint64_t size;
	int64_t  offset;
	uint8_t  reserved[40];
};

struct SaveDataMemorySetup2Prefix
{
	uint32_t option;
	int32_t  user_id;
	uint64_t memory_size;
	uint64_t icon_memory_size;
	uint64_t initial_param;
	uint64_t initial_icon;
	uint32_t slot_id;
};

struct SaveDataMemorySetupResult
{
	uint64_t existing_memory_size;
	uint8_t  reserved[16];
};

struct SaveDataMemoryGet2Prefix
{
	int32_t  user_id;
	uint32_t padding;
	uint64_t data;
	uint64_t param;
	uint64_t icon;
	uint32_t slot_id;
};

struct SaveDataMemorySet2Prefix
{
	int32_t  user_id;
	uint32_t padding;
	uint64_t data;
	uint64_t param;
	uint64_t icon;
	uint32_t data_num;
	uint32_t slot_id;
};

struct SaveDataMemorySyncPrefix
{
	int32_t  user_id;
	uint32_t slot_id;
	uint32_t option;
};

static_assert(sizeof(SceSaveDataDirName) == 32);
static_assert(sizeof(SaveDataMount) == 80);
static_assert(offsetof(SaveDataMount, user_id) == 0x00);
static_assert(offsetof(SaveDataMount, title_id) == 0x08);
static_assert(offsetof(SaveDataMount, dir_name) == 0x10);
static_assert(offsetof(SaveDataMount, fingerprint) == 0x18);
static_assert(offsetof(SaveDataMount, blocks) == 0x20);
static_assert(offsetof(SaveDataMount, mount_mode) == 0x28);
static_assert(offsetof(SaveDataMount, reserved) == 0x2c);
static_assert(sizeof(SaveDataMount2) == 64);
static_assert(offsetof(SaveDataMount2, user_id) == 0x00);
static_assert(offsetof(SaveDataMount2, dir_name) == 0x08);
static_assert(offsetof(SaveDataMount2, blocks) == 0x10);
static_assert(offsetof(SaveDataMount2, mount_mode) == 0x18);
static_assert(offsetof(SaveDataMount2, reserved) == 0x1c);
static_assert(offsetof(SaveDataMount2, pad2) == 0x3c);
static_assert(sizeof(SaveDataMountResult) == 64);
static_assert(offsetof(SaveDataMountResult, mount_point) == 0x00);
static_assert(offsetof(SaveDataMountResult, required_blocks) == 0x10);
static_assert(offsetof(SaveDataMountResult, unused) == 0x18);
static_assert(offsetof(SaveDataMountResult, mount_status) == 0x1c);
static_assert(offsetof(SaveDataMountResult, reserved) == 0x20);
static_assert(offsetof(SaveDataMountResult, pad) == 0x3c);
static_assert(sizeof(SaveDataParam) == 1328);
static_assert(offsetof(SaveDataParam, user_param) == 0x500);
static_assert(offsetof(SaveDataParam, mtime) == 0x508);
static_assert(offsetof(SaveDataParam, reserved) == 0x510);
static_assert(sizeof(SaveDataMountInfo) == 48);
static_assert(offsetof(SaveDataMountInfo, blocks) == 0x00);
static_assert(offsetof(SaveDataMountInfo, free_blocks) == 0x08);
static_assert(offsetof(SaveDataMountInfo, reserved) == 0x10);
static_assert(sizeof(SaveDataDirNameSearchCondition) == 64);
static_assert(offsetof(SaveDataDirNameSearchCondition, user_id) == 0x00);
static_assert(offsetof(SaveDataDirNameSearchCondition, pad) == 0x04);
static_assert(offsetof(SaveDataDirNameSearchCondition, title_id) == 0x08);
static_assert(offsetof(SaveDataDirNameSearchCondition, dir_name) == 0x10);
static_assert(offsetof(SaveDataDirNameSearchCondition, sort_key) == 0x18);
static_assert(offsetof(SaveDataDirNameSearchCondition, sort_order) == 0x1c);
static_assert(offsetof(SaveDataDirNameSearchCondition, reserved) == 0x20);
static_assert(sizeof(SaveDataDirNameSearchResult) == 56);
static_assert(offsetof(SaveDataDirNameSearchResult, hit_num) == 0x00);
static_assert(offsetof(SaveDataDirNameSearchResult, pad) == 0x04);
static_assert(offsetof(SaveDataDirNameSearchResult, dir_names) == 0x08);
static_assert(offsetof(SaveDataDirNameSearchResult, dir_names_num) == 0x10);
static_assert(offsetof(SaveDataDirNameSearchResult, set_num) == 0x14);
static_assert(offsetof(SaveDataDirNameSearchResult, params) == 0x18);
static_assert(offsetof(SaveDataDirNameSearchResult, infos) == 0x20);
static_assert(offsetof(SaveDataDirNameSearchResult, reserved) == 0x28);
static_assert(offsetof(SaveDataDirNameSearchResult, pad2) == 0x34);
static_assert(sizeof(SaveDataIcon) == 56);
static_assert(alignof(SaveDataIcon) == 8);
static_assert(offsetof(SaveDataIcon, buf) == 0x00);
static_assert(offsetof(SaveDataIcon, buf_size) == 0x08);
static_assert(offsetof(SaveDataIcon, data_size) == 0x10);
static_assert(offsetof(SaveDataIcon, reserved) == 0x18);
static_assert(sizeof(SaveDataMemoryData) == 64);
static_assert(offsetof(SaveDataMemoryData, buffer) == 0x00);
static_assert(offsetof(SaveDataMemoryData, size) == 0x08);
static_assert(offsetof(SaveDataMemoryData, offset) == 0x10);
static_assert(offsetof(SaveDataMemorySetup2Prefix, memory_size) == 0x08);
static_assert(offsetof(SaveDataMemorySetup2Prefix, initial_param) == 0x18);
static_assert(offsetof(SaveDataMemorySetup2Prefix, initial_icon) == 0x20);
static_assert(offsetof(SaveDataMemorySetup2Prefix, slot_id) == 0x28);
static_assert(sizeof(SaveDataMemorySetupResult) == 24);
static_assert(offsetof(SaveDataMemoryGet2Prefix, data) == 0x08);
static_assert(offsetof(SaveDataMemoryGet2Prefix, param) == 0x10);
static_assert(offsetof(SaveDataMemoryGet2Prefix, icon) == 0x18);
static_assert(offsetof(SaveDataMemoryGet2Prefix, slot_id) == 0x20);
static_assert(sizeof(SaveDataMemorySet2Prefix) == 40);
static_assert(offsetof(SaveDataMemorySet2Prefix, data_num) == 0x20);
static_assert(offsetof(SaveDataMemorySet2Prefix, slot_id) == 0x24);
static_assert(sizeof(SaveDataMemorySyncPrefix) == 12);

namespace {

constexpr uint32_t SAVE_DATA_SORT_NAME        = 0;
constexpr uint32_t SAVE_DATA_SORT_USER_PARAM  = 1;
constexpr uint32_t SAVE_DATA_SORT_BLOCKS      = 2;
constexpr uint32_t SAVE_DATA_SORT_MTIME       = 3;
constexpr uint32_t SAVE_DATA_SORT_FREE_BLOCKS = 5;
constexpr uint32_t SAVE_DATA_SORT_ASCENDING   = 0;
constexpr uint32_t SAVE_DATA_SORT_DESCENDING  = 1;
constexpr uint64_t SAVE_DATA_BLOCKS           = 100000;

constexpr uint32_t SAVE_DATA_PARAM_ALL        = 0;
constexpr uint32_t SAVE_DATA_PARAM_TITLE      = 1;
constexpr uint32_t SAVE_DATA_PARAM_SUB_TITLE  = 2;
constexpr uint32_t SAVE_DATA_PARAM_DETAIL     = 3;
constexpr uint32_t SAVE_DATA_PARAM_USER_PARAM = 4;
constexpr uint32_t SAVE_DATA_PARAM_MTIME      = 5;
constexpr uint32_t SAVE_DATA_PARAM_FIELD_MASK = 0x1fu;

// Preserve the local access/create modes. Bit 0x08 is also admitted when
// opening an existing slot; 0x10 remains restricted to creation. Their
// additional service effects are not modelled by this flat backend.
constexpr uint32_t SAVE_DATA_MOUNT_ACCESS_MASK       = 0x03u;
constexpr uint32_t SAVE_DATA_MOUNT_ACCESS_READ_ONLY  = 0x01u;
constexpr uint32_t SAVE_DATA_MOUNT_ACCESS_READ_WRITE = 0x02u;
constexpr uint32_t SAVE_DATA_MOUNT_CREATE            = 0x04u;
constexpr uint32_t SAVE_DATA_MOUNT_OPEN_OR_CREATE_OPTION = 0x08u;
constexpr uint32_t SAVE_DATA_MOUNT_CREATE_MODIFIERS  = 0x10u;
constexpr uint32_t SAVE_DATA_MOUNT_KNOWN_MASK        = SAVE_DATA_MOUNT_ACCESS_MASK | SAVE_DATA_MOUNT_CREATE |
	                                                    SAVE_DATA_MOUNT_OPEN_OR_CREATE_OPTION | SAVE_DATA_MOUNT_CREATE_MODIFIERS;

std::mutex g_save_data_mount_mutex;

struct SaveDataMemoryKey
{
	std::string title_id;
	int32_t     user_id;
	uint32_t    slot_id;

	bool operator<(const SaveDataMemoryKey& other) const
	{
		if (title_id != other.title_id) return title_id < other.title_id;
		if (user_id != other.user_id) return user_id < other.user_id;
		return slot_id < other.slot_id;
	}
};

struct SaveDataMemorySlot
{
	uint64_t             capacity = 0;
	uint64_t             generation = 0;
	std::vector<uint8_t> data;
	SaveDataParam         param {};
	bool                  has_param = false;
	bool                  storage_indeterminate = false;
};

std::mutex                                      g_save_data_memory_mutex;
std::map<SaveDataMemoryKey, SaveDataMemorySlot> g_save_data_memory_slots;
bool                                            g_save_data_memory_initialized = false;
std::string                                     g_save_data_memory_title_id;

#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
std::string g_save_data_memory_test_title_id;
std::atomic<void (*)()> g_save_data_memory_publish_test_hook {nullptr};
#endif

struct AddressRange
{
	uintptr_t begin;
	uintptr_t end;
};

struct SearchEntry
{
	std::string name;
	int64_t     mtime;
	SaveDataParam param {};
};

// Snapshot inputs before taking a storage lock. FEX outputs retain every
// mapping and complete their handoff before the first API payload byte is
// copied; a later denied output must not leave an earlier payload partial.
bool ReadSaveDataGuestMemory(uint64_t address, void* destination, size_t size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME)
	return Core::VirtualMemory::TryReadGuestData(address, destination, size);
#else
	return Loader::ReadGuestMemory(address, destination, size);
#endif
}

template <typename T>
bool ReadSaveDataGuestValue(const T* source, T* output)
{
	static_assert(std::is_trivially_copyable_v<T>);
	return ReadSaveDataGuestMemory(reinterpret_cast<uint64_t>(source), output, sizeof(*output));
}

class SaveDataGuestOutput final
{
public:
	bool Acquire(void* destination, size_t size)
	{
		address = reinterpret_cast<uint64_t>(destination);
		bytes = size;
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
		return Core::VirtualMemory::TryAcquireGuestDataLease(address, bytes, Core::VirtualMemory::Mode::Write, &lease);
#else
		return Loader::ValidateGuestMemory(address, bytes, true);
#endif
	}

	bool Copy(const void* source, size_t size, size_t offset = 0)
	{
		if (offset > bytes || size > bytes - offset) return false;
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
		std::memcpy(reinterpret_cast<void*>(lease.HostBegin() + offset), source, size);
		return true;
#else
		// The legacy hosts keep their checked Loader translation. This does not
		// qualify concurrent Windows VM mutation without a Core mapping lease.
		return Loader::WriteGuestMemory(address + offset, source, size);
#endif
	}

private:
	uint64_t address = 0;
	size_t bytes = 0;
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
	Core::VirtualMemory::GuestDataLease lease;
#endif
};

bool IsNaturallyAligned(const void* pointer, size_t alignment)
{
	return reinterpret_cast<uintptr_t>(pointer) % alignment == 0;
}

bool MakeAddressRange(const void* pointer, size_t size, AddressRange* range)
{
	const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);

	if (size > std::numeric_limits<uintptr_t>::max() - begin)
	{
		return false;
	}

	range->begin = begin;
	range->end   = begin + size;
	return true;
}

bool RangesOverlap(const AddressRange& left, const AddressRange& right)
{
	return left.begin < right.end && right.begin < left.end;
}

bool MultiplySize(size_t count, size_t element_size, size_t* size)
{
	if (count != 0 && element_size > std::numeric_limits<size_t>::max() / count)
	{
		return false;
	}

	*size = count * element_size;
	return true;
}

template <typename T>
bool MakeArrayRange(T* pointer, uint32_t count, AddressRange* range)
{
	size_t size = 0;

	return pointer != nullptr && IsNaturallyAligned(pointer, alignof(T)) &&
	       MultiplySize(static_cast<size_t>(count), sizeof(T), &size) && MakeAddressRange(pointer, size, range);
}

uint8_t FoldAscii(uint8_t value)
{
	return value >= 'A' && value <= 'Z' ? static_cast<uint8_t>(value + ('a' - 'A')) : value;
}

int CompareBytes(const std::string& left, const std::string& right, bool fold_ascii)
{
	const size_t common = std::min(left.size(), right.size());

	for (size_t i = 0; i < common; i++)
	{
		uint8_t left_value  = static_cast<uint8_t>(left[i]);
		uint8_t right_value = static_cast<uint8_t>(right[i]);

		if (fold_ascii)
		{
			left_value  = FoldAscii(left_value);
			right_value = FoldAscii(right_value);
		}

		if (left_value != right_value)
		{
			return left_value < right_value ? -1 : 1;
		}
	}

	if (left.size() != right.size())
	{
		return left.size() < right.size() ? -1 : 1;
	}

	return 0;
}

int CompareNames(const std::string& left, const std::string& right)
{
	const int folded = CompareBytes(left, right, true);
	return folded != 0 ? folded : CompareBytes(left, right, false);
}

bool IsValidUtf8(const std::string& value)
{
	const auto continuation = [&value](size_t index) {
		return index < value.size() && (static_cast<uint8_t>(value[index]) & 0xc0u) == 0x80u;
	};

	for (size_t i = 0; i < value.size();)
	{
		const uint8_t first = static_cast<uint8_t>(value[i]);

		if (first <= 0x7fu)
		{
			i++;
			continue;
		}

		if (first >= 0xc2u && first <= 0xdfu)
		{
			if (!continuation(i + 1))
			{
				return false;
			}
			i += 2;
			continue;
		}

		if (first >= 0xe0u && first <= 0xefu)
		{
			if (!continuation(i + 1) || !continuation(i + 2))
			{
				return false;
			}

			const uint8_t second = static_cast<uint8_t>(value[i + 1]);
			if ((first == 0xe0u && second < 0xa0u) || (first == 0xedu && second >= 0xa0u))
			{
				return false;
			}

			i += 3;
			continue;
		}

		if (first >= 0xf0u && first <= 0xf4u)
		{
			if (!continuation(i + 1) || !continuation(i + 2) || !continuation(i + 3))
			{
				return false;
			}

			const uint8_t second = static_cast<uint8_t>(value[i + 1]);
			if ((first == 0xf0u && second < 0x90u) || (first == 0xf4u && second >= 0x90u))
			{
				return false;
			}

			i += 4;
			continue;
		}

		return false;
	}

	return true;
}

bool CopyBoundedString(const char* source, size_t source_size, std::string* result)
{
	result->clear();
	if (source == nullptr) return false;
	const void* const terminator = std::memchr(source, '\0', source_size);
	if (terminator == nullptr) return false;
	result->assign(source, static_cast<const char*>(terminator));
	return IsValidUtf8(*result);
}

void TraceSaveDataValidation(const char* stage, uint64_t value, uint64_t limit)
{
	static const bool enabled = [] {
		const char* flag = std::getenv("LUMEN_X64_HOST_ERROR_TRACE");
		return flag != nullptr && flag[0] == '1' && flag[1] == '\0';
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> count {0};
	if (count.fetch_add(1, std::memory_order_relaxed) >= 256) return;
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	std::fprintf(stderr, "[SAVE_DATA_VALIDATION_TRACE] monotonicMs=%lld stage=%s value=%llu limit=%llu\n",
	    static_cast<long long>(now_ms), stage, static_cast<unsigned long long>(value), static_cast<unsigned long long>(limit));
}

void TraceSaveDataMemory(const char* operation, const char* stage, int32_t result,
	                     uint64_t value0 = 0, uint64_t value1 = 0, uint64_t value2 = 0,
	                     uint64_t value3 = 0, uint64_t value4 = 0)
{
	static const bool enabled = [] {
		const char* flag = std::getenv("LUMEN_SAVE_DATA_MEMORY_TRACE");
		return flag != nullptr && flag[0] == '1' && flag[1] == '\0';
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> count {0};
	const uint32_t index = count.fetch_add(1, std::memory_order_relaxed);
	if (index >= 512) return;
	std::fprintf(stderr,
	    "[SAVE_DATA_MEMORY_TRACE] index=%u operation=%s stage=%s result=%d value0=0x%016" PRIx64
	    " value1=0x%016" PRIx64 " value2=0x%016" PRIx64 " value3=0x%016" PRIx64
	    " value4=0x%016" PRIx64 "\n",
	    index, operation, stage, result, value0, value1, value2, value3, value4);
}

bool ParseSaveDataMountMode(uint32_t mode, bool* create)
{
	if ((mode & ~SAVE_DATA_MOUNT_KNOWN_MASK) != 0)
	{
		return false;
	}

	const uint32_t access    = mode & SAVE_DATA_MOUNT_ACCESS_MASK;
	const bool     is_create = (mode & SAVE_DATA_MOUNT_CREATE) != 0;
	const bool     modifiers = (mode & SAVE_DATA_MOUNT_CREATE_MODIFIERS) != 0;
	if ((access != SAVE_DATA_MOUNT_ACCESS_READ_ONLY && access != SAVE_DATA_MOUNT_ACCESS_READ_WRITE) ||
	    (is_create && access != SAVE_DATA_MOUNT_ACCESS_READ_WRITE) || (!is_create && modifiers))
	{
		return false;
	}

	*create = is_create;
	if (!is_create && (mode & SAVE_DATA_MOUNT_OPEN_OR_CREATE_OPTION) != 0)
		TraceSaveDataValidation("open-option-admitted", mode, 0);
	return true;
}

bool IsValidSaveDataSlotName(const std::string& name)
{
	return !name.empty() && name.size() < sizeof(SceSaveDataDirName) && name != "." && name != ".." &&
	       name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
	       name.compare(0, 4, "sce_") != 0;
}

enum class HostDirectoryKind
{
	Missing,
	Directory,
	Other,
	Error
};

enum class HostDirectoryCreateResult
{
	Created,
	Exists,
	Error
};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS

HostDirectoryKind ClassifyHostDirectory(const std::string& path)
{
	const auto  utf16      = String::FromUtf8(path.c_str()).utf16_str();
	const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(utf16.GetData()));
	if (attributes == INVALID_FILE_ATTRIBUTES)
	{
		const DWORD error = GetLastError();
		return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? HostDirectoryKind::Missing
		                                                                    : HostDirectoryKind::Error;
	}
	if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
	{
		return HostDirectoryKind::Other;
	}
	return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? HostDirectoryKind::Directory : HostDirectoryKind::Other;
}

HostDirectoryCreateResult CreateHostDirectory(const std::string& path)
{
	const auto utf16 = String::FromUtf8(path.c_str()).utf16_str();
	if (CreateDirectoryW(reinterpret_cast<LPCWSTR>(utf16.GetData()), nullptr) != 0)
	{
		return HostDirectoryCreateResult::Created;
	}
	return GetLastError() == ERROR_ALREADY_EXISTS ? HostDirectoryCreateResult::Exists
	                                              : HostDirectoryCreateResult::Error;
}

#else

HostDirectoryKind ClassifyHostDirectory(const std::string& path)
{
	struct stat status {};
	if (lstat(path.c_str(), &status) == 0)
	{
		return S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode) ? HostDirectoryKind::Directory
		                                                           : HostDirectoryKind::Other;
	}
	return errno == ENOENT ? HostDirectoryKind::Missing : HostDirectoryKind::Error;
}

HostDirectoryCreateResult CreateHostDirectory(const std::string& path)
{
	if (mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) == 0)
	{
		return HostDirectoryCreateResult::Created;
	}
	return errno == EEXIST ? HostDirectoryCreateResult::Exists : HostDirectoryCreateResult::Error;
}

#endif

int EnsureSaveDataRootForMount(bool create)
{
	const HostDirectoryKind root = ClassifyHostDirectory("_SaveData");
	if (root == HostDirectoryKind::Directory)
	{
		return OK;
	}
	if (root != HostDirectoryKind::Missing)
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	if (!create)
	{
		return SAVE_DATA_ERROR_NOT_FOUND;
	}

	const HostDirectoryCreateResult created = CreateHostDirectory("_SaveData");
	if (created == HostDirectoryCreateResult::Error ||
	    ClassifyHostDirectory("_SaveData") != HostDirectoryKind::Directory)
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	return OK;
}

int SaveDataMountImpl(const void* request, size_t request_size, size_t request_alignment, const char* dir_name,
	                  uint32_t mount_mode, SaveDataMountResult* mount_result)
{
	if (request == nullptr || mount_result == nullptr || dir_name == nullptr ||
	    !IsNaturallyAligned(request, request_alignment) ||
	    !IsNaturallyAligned(mount_result, alignof(SaveDataMountResult)))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	AddressRange request_range {};
	AddressRange result_range {};
	AddressRange dir_name_range {};
	if (!MakeAddressRange(request, request_size, &request_range) ||
	    !MakeAddressRange(mount_result, sizeof(*mount_result), &result_range) ||
	    !MakeAddressRange(dir_name, sizeof(SceSaveDataDirName), &dir_name_range) ||
	    RangesOverlap(request_range, result_range) || RangesOverlap(dir_name_range, result_range))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	bool create = false;
	if (!ParseSaveDataMountMode(mount_mode, &create))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	SceSaveDataDirName directory {};
	std::string slot_name;
	if (!ReadSaveDataGuestMemory(reinterpret_cast<uint64_t>(dir_name), &directory, sizeof(directory)) ||
	    !CopyBoundedString(directory.data, sizeof(directory), &slot_name) ||
	    !IsValidSaveDataSlotName(slot_name))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}
	SaveDataMountResult output {};
	SaveDataGuestOutput result_output;
	if (!ReadSaveDataGuestValue(mount_result, &output) || !result_output.Acquire(mount_result, sizeof(output)))
		return SAVE_DATA_ERROR_PARAMETER;
	std::lock_guard<std::mutex> lock(g_save_data_mount_mutex);

	const int root_result = EnsureSaveDataRootForMount(create);
	if (root_result != OK)
	{
		return root_result;
	}

	const std::string       slot_path = std::string("_SaveData/") + slot_name;
	const HostDirectoryKind slot      = ClassifyHostDirectory(slot_path);
	uint32_t                mount_status = 0;
	if (create)
	{
		if (slot == HostDirectoryKind::Directory)
		{
			return SAVE_DATA_ERROR_EXISTS;
		}
		if (slot != HostDirectoryKind::Missing)
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}

		const HostDirectoryCreateResult created = CreateHostDirectory(slot_path);
		if (created == HostDirectoryCreateResult::Exists)
		{
			return ClassifyHostDirectory(slot_path) == HostDirectoryKind::Directory ? SAVE_DATA_ERROR_EXISTS
			                                                                        : SAVE_DATA_ERROR_INTERNAL;
		}
		if (created == HostDirectoryCreateResult::Error ||
		    ClassifyHostDirectory(slot_path) != HostDirectoryKind::Directory)
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
		mount_status = 1;
	} else
	{
		if (slot == HostDirectoryKind::Missing)
		{
			return SAVE_DATA_ERROR_NOT_FOUND;
		}
		if (slot != HostDirectoryKind::Directory)
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
	}

	if (ClassifyHostDirectory("_SaveData") != HostDirectoryKind::Directory ||
	    ClassifyHostDirectory(slot_path) != HostDirectoryKind::Directory)
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	std::memset(output.mount_point.data, 0, sizeof(output.mount_point.data));
	constexpr char mount_point[] = "/savedata0";
	static_assert(sizeof(mount_point) <= sizeof(output.mount_point.data));
	std::memcpy(output.mount_point.data, mount_point, sizeof(mount_point));
	output.required_blocks = 0;
	output.mount_status    = mount_status;
	if (!result_output.Copy(&output, sizeof(output))) return SAVE_DATA_ERROR_PARAMETER;

	LibKernel::FileSystem::Mount(String::FromUtf8(slot_path.c_str()), SAVE_DATA_POINT);
	return OK;
}

bool CopyParamString(const void* source, size_t source_size, char* destination, size_t destination_size,
                     bool allow_counted_span = false)
{
	if (source == nullptr || source_size == 0 || source_size > destination_size)
	{
		TraceSaveDataValidation("param-string-size", source_size, destination_size);
		return false;
	}
	const void* const terminator = std::memchr(source, '\0', source_size);
	if (terminator == nullptr && (!allow_counted_span || source_size == destination_size))
	{
		TraceSaveDataValidation("param-string-terminator", source_size, destination_size);
		return false;
	}
	const size_t length = terminator != nullptr
	    ? static_cast<const uint8_t*>(terminator) - static_cast<const uint8_t*>(source) : source_size;
	if (!IsValidUtf8(std::string(static_cast<const char*>(source), length)))
	{
		TraceSaveDataValidation("param-string-utf8", length, source_size);
		return false;
	}
	std::memset(destination, 0, destination_size);
	std::memcpy(destination, source, length);
	return true;
}

struct SaveDataParamUpdate
{
	SaveDataParam value;
	uint32_t fields;
	bool replace_all;
};

bool PrepareSaveDataParamUpdate(uint32_t param_type, const void* param_buf, size_t param_buf_size,
	                            SaveDataParamUpdate* update)
{
	if (param_buf == nullptr) return false;
	std::memset(update, 0, sizeof(*update));
	switch (param_type)
	{
		case SAVE_DATA_PARAM_ALL:
		{
			if (param_buf_size != sizeof(SaveDataParam)) return false;
			SaveDataParam input {};
			std::memcpy(&input, param_buf, sizeof(input));
			if (!CopyParamString(input.title, sizeof(input.title), update->value.title, sizeof(update->value.title)) ||
			    !CopyParamString(input.sub_title, sizeof(input.sub_title), update->value.sub_title,
			                     sizeof(update->value.sub_title)) ||
			    !CopyParamString(input.detail, sizeof(input.detail), update->value.detail,
			                     sizeof(update->value.detail))) return false;
			update->value.user_param = input.user_param;
			update->value.mtime = input.mtime;
			update->fields = SAVE_DATA_PARAM_FIELD_MASK;
			update->replace_all = true;
			return true;
		}
		case SAVE_DATA_PARAM_TITLE:
			update->fields = 1u << (SAVE_DATA_PARAM_TITLE - 1u);
			return CopyParamString(param_buf, param_buf_size, update->value.title, sizeof(update->value.title), true);
		case SAVE_DATA_PARAM_SUB_TITLE:
			update->fields = 1u << (SAVE_DATA_PARAM_SUB_TITLE - 1u);
			return CopyParamString(param_buf, param_buf_size, update->value.sub_title, sizeof(update->value.sub_title), true);
		case SAVE_DATA_PARAM_DETAIL:
			update->fields = 1u << (SAVE_DATA_PARAM_DETAIL - 1u);
			return CopyParamString(param_buf, param_buf_size, update->value.detail, sizeof(update->value.detail), true);
		case SAVE_DATA_PARAM_USER_PARAM:
			if (param_buf_size != sizeof(update->value.user_param)) return false;
			std::memcpy(&update->value.user_param, param_buf, sizeof(update->value.user_param));
			update->fields = 1u << (SAVE_DATA_PARAM_USER_PARAM - 1u);
			return true;
		case SAVE_DATA_PARAM_MTIME:
			if (param_buf_size != sizeof(update->value.mtime)) return false;
			std::memcpy(&update->value.mtime, param_buf, sizeof(update->value.mtime));
			update->fields = 1u << (SAVE_DATA_PARAM_MTIME - 1u);
			return true;
		default: return false;
	}
}

bool WildcardMatches(const std::string& pattern, const std::string& name)
{
	bool previous[33] {};
	bool current[33] {};
	previous[0] = true;

	for (char pattern_char: pattern)
	{
		std::memset(current, 0, sizeof(current));
		const uint8_t pattern_value = static_cast<uint8_t>(pattern_char);

		if (pattern_value == static_cast<uint8_t>('%'))
		{
			current[0] = previous[0];
			for (size_t name_index = 1; name_index <= name.size(); name_index++)
			{
				current[name_index] = previous[name_index] || current[name_index - 1];
			}
		} else
		{
			for (size_t name_index = 1; name_index <= name.size(); name_index++)
			{
				current[name_index] = previous[name_index - 1] &&
				                      (pattern_value == static_cast<uint8_t>('_') ||
				                       FoldAscii(pattern_value) == FoldAscii(static_cast<uint8_t>(name[name_index - 1])));
			}
		}

		std::memcpy(previous, current, sizeof(previous));
	}

	return previous[name.size()];
}

bool CopyFilter(const SceSaveDataDirName* filter, std::string* pattern)
{
	pattern->clear();

	if (filter == nullptr)
	{
		return true;
	}

	AddressRange filter_range {};
	if (!MakeAddressRange(filter, sizeof(*filter), &filter_range))
	{
		return false;
	}

	SceSaveDataDirName input {};
	if (!ReadSaveDataGuestValue(filter, &input)) return false;
	for (char value: input.data)
	{
		if (value == '\0')
		{
			return true;
		}

		pattern->push_back(value);
	}

	return false;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS

constexpr wchar_t SAVE_DATA_ROOT_PATH[] = L"_SaveData";

class WindowsHandle
{
public:
	explicit WindowsHandle(HANDLE handle): m_handle(handle) {}
	~WindowsHandle()
	{
		if (m_handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_handle);
		}
	}

	[[nodiscard]] HANDLE Get() const { return m_handle; }
	[[nodiscard]] bool   IsValid() const { return m_handle != INVALID_HANDLE_VALUE; }

	LUMEN_CLASS_NO_COPY(WindowsHandle);

private:
	HANDLE m_handle;
};

HANDLE OpenSaveDataRoot()
{
	return CreateFileW(SAVE_DATA_ROOT_PATH, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
	                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                   FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

bool IsMissingWindowsError(DWORD error)
{
	return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

uint64_t FileTimeValue(const FILETIME& value)
{
	ULARGE_INTEGER converted {};
	converted.LowPart  = value.dwLowDateTime;
	converted.HighPart = value.dwHighDateTime;
	return converted.QuadPart;
}

uint64_t FileTimeValue(const LARGE_INTEGER& value)
{
	ULARGE_INTEGER converted {};
	converted.LowPart  = value.LowPart;
	converted.HighPart = static_cast<DWORD>(value.HighPart);
	return converted.QuadPart;
}

bool WindowsTimeToUnix(uint64_t value, int64_t* seconds)
{
	constexpr uint64_t WINDOWS_UNIX_EPOCH = 116444736000000000ull;
	constexpr uint64_t TICKS_PER_SECOND   = 10000000ull;

	if (value >= WINDOWS_UNIX_EPOCH)
	{
		const uint64_t result = (value - WINDOWS_UNIX_EPOCH) / TICKS_PER_SECOND;
		if (result > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		{
			return false;
		}
		*seconds = static_cast<int64_t>(result);
	} else
	{
		const uint64_t result = (WINDOWS_UNIX_EPOCH - value) / TICKS_PER_SECOND;
		if (result > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		{
			return false;
		}
		*seconds = -static_cast<int64_t>(result);
	}

	return true;
}

struct WindowsRootSnapshot
{
	DWORD    attributes;
	DWORD    volume_serial;
	DWORD    file_index_high;
	DWORD    file_index_low;
	DWORD    links;
	uint64_t creation_time;
	uint64_t write_time;
	uint64_t change_time;
	uint64_t size;
};

bool ReadWindowsRootSnapshot(HANDLE handle, WindowsRootSnapshot* snapshot)
{
	BY_HANDLE_FILE_INFORMATION information {};
	FILE_BASIC_INFO            basic {};
	if (!GetFileInformationByHandle(handle, &information) ||
	    !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
	{
		return false;
	}

	snapshot->attributes      = information.dwFileAttributes;
	snapshot->volume_serial   = information.dwVolumeSerialNumber;
	snapshot->file_index_high = information.nFileIndexHigh;
	snapshot->file_index_low  = information.nFileIndexLow;
	snapshot->links           = information.nNumberOfLinks;
	snapshot->creation_time   = FileTimeValue(information.ftCreationTime);
	snapshot->write_time      = FileTimeValue(information.ftLastWriteTime);
	snapshot->change_time     = FileTimeValue(basic.ChangeTime);
	snapshot->size            = (static_cast<uint64_t>(information.nFileSizeHigh) << 32u) | information.nFileSizeLow;
	return true;
}

bool IsRealWindowsDirectory(const WindowsRootSnapshot& snapshot)
{
	return (snapshot.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
	       (snapshot.attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool SameWindowsRoot(const WindowsRootSnapshot& left, const WindowsRootSnapshot& right)
{
	return left.attributes == right.attributes && left.volume_serial == right.volume_serial &&
	       left.file_index_high == right.file_index_high && left.file_index_low == right.file_index_low &&
	       left.links == right.links && left.creation_time == right.creation_time && left.write_time == right.write_time &&
	       left.change_time == right.change_time && left.size == right.size;
}

bool WideNameToUtf8(const wchar_t* value, size_t length, std::string* result)
{
	if (length > static_cast<size_t>(std::numeric_limits<int>::max()))
	{
		return false;
	}

	const int input_length = static_cast<int>(length);
	const int output_size  = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, input_length, nullptr, 0, nullptr, nullptr);
	if (output_size <= 0)
	{
		return false;
	}

	result->resize(static_cast<size_t>(output_size));
	return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, input_length, result->data(), output_size, nullptr,
	                           nullptr) == output_size &&
	       result->find('\0') == std::string::npos;
}

struct WindowsObservedDirectory
{
	SearchEntry entry;
	int64_t     file_id;
	uint64_t    write_time;
	uint64_t    change_time;
	DWORD       attributes;
};

int EnumerateWindowsDirectories(HANDLE root, bool restart, std::vector<WindowsObservedDirectory>* directories)
{
	std::vector<uint8_t> buffer(64 * 1024);
	auto info_class = restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;

	for (;;)
	{
		if (!GetFileInformationByHandleEx(root, info_class, buffer.data(), static_cast<DWORD>(buffer.size())))
		{
			return GetLastError() == ERROR_NO_MORE_FILES ? OK : SAVE_DATA_ERROR_INTERNAL;
		}
		info_class = FileIdBothDirectoryInfo;

		size_t offset = 0;
		for (;;)
		{
			constexpr size_t HEADER_SIZE = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
			if (offset > buffer.size() - HEADER_SIZE)
			{
				return SAVE_DATA_ERROR_INTERNAL;
			}

			const auto* information = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
			if (information->FileNameLength % sizeof(wchar_t) != 0 ||
			    information->FileNameLength > buffer.size() - offset - HEADER_SIZE)
			{
				return SAVE_DATA_ERROR_INTERNAL;
			}

			if ((information->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
			    (information->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
			{
				std::string name;
				if (!WideNameToUtf8(information->FileName, information->FileNameLength / sizeof(wchar_t), &name))
				{
					return SAVE_DATA_ERROR_INTERNAL;
				}

				if (!name.empty() && name != "." && name != ".." && name.size() < sizeof(SceSaveDataDirName) &&
				    name.compare(0, 4, "sce_") != 0)
				{
					WindowsObservedDirectory observed {};
					observed.entry.name  = std::move(name);
					observed.file_id     = information->FileId.QuadPart;
					observed.write_time  = FileTimeValue(information->LastWriteTime);
					observed.change_time = FileTimeValue(information->ChangeTime);
					observed.attributes  = information->FileAttributes;
					if (!WindowsTimeToUnix(observed.write_time, &observed.entry.mtime))
					{
						return SAVE_DATA_ERROR_INTERNAL;
					}
					directories->push_back(std::move(observed));
				}
			}

			if (information->NextEntryOffset == 0)
			{
				break;
			}
			if (information->NextEntryOffset < HEADER_SIZE + information->FileNameLength ||
			    information->NextEntryOffset > buffer.size() - offset)
			{
				return SAVE_DATA_ERROR_INTERNAL;
			}
			offset += information->NextEntryOffset;
		}
	}
}

bool SameWindowsDirectories(std::vector<WindowsObservedDirectory>* left, std::vector<WindowsObservedDirectory>* right)
{
	const auto less = [](const WindowsObservedDirectory& first, const WindowsObservedDirectory& second) {
		return CompareBytes(first.entry.name, second.entry.name, false) < 0;
	};
	std::sort(left->begin(), left->end(), less);
	std::sort(right->begin(), right->end(), less);

	if (left->size() != right->size())
	{
		return false;
	}

	for (size_t i = 0; i < left->size(); i++)
	{
		const auto& first  = (*left)[i];
		const auto& second = (*right)[i];
		if (first.entry.name != second.entry.name || first.entry.mtime != second.entry.mtime ||
		    first.file_id != second.file_id || first.write_time != second.write_time ||
		    first.change_time != second.change_time || first.attributes != second.attributes)
		{
			return false;
		}
	}

	return true;
}

int CollectSearchEntries(const std::string& pattern, std::vector<SearchEntry>* entries)
{
	WindowsHandle root(OpenSaveDataRoot());
	if (!root.IsValid())
	{
		const DWORD first_error = GetLastError();
		if (!IsMissingWindowsError(first_error))
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}

		WindowsHandle verify(OpenSaveDataRoot());
		return !verify.IsValid() && IsMissingWindowsError(GetLastError()) ? OK : SAVE_DATA_ERROR_INTERNAL;
	}

	WindowsRootSnapshot root_before {};
	if (!ReadWindowsRootSnapshot(root.Get(), &root_before) || !IsRealWindowsDirectory(root_before))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	std::vector<WindowsObservedDirectory> first_scan;
	std::vector<WindowsObservedDirectory> second_scan;
	int result = EnumerateWindowsDirectories(root.Get(), false, &first_scan);
	if (result != OK)
	{
		return result;
	}
	result = EnumerateWindowsDirectories(root.Get(), true, &second_scan);
	if (result != OK || !SameWindowsDirectories(&first_scan, &second_scan))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	WindowsRootSnapshot root_after {};
	if (!ReadWindowsRootSnapshot(root.Get(), &root_after) || !IsRealWindowsDirectory(root_after) ||
	    !SameWindowsRoot(root_before, root_after))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	WindowsHandle verify(OpenSaveDataRoot());
	WindowsRootSnapshot path_snapshot {};
	if (!verify.IsValid() || !ReadWindowsRootSnapshot(verify.Get(), &path_snapshot) ||
	    !IsRealWindowsDirectory(path_snapshot) || !SameWindowsRoot(root_after, path_snapshot))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	for (auto& observed: second_scan)
	{
		if (pattern.empty() || WildcardMatches(pattern, observed.entry.name))
		{
			if (entries->size() == std::numeric_limits<uint32_t>::max())
			{
				return SAVE_DATA_ERROR_INTERNAL;
			}
			entries->push_back(std::move(observed.entry));
		}
	}

	return OK;
}

int ReadSaveDataSlotMtime(const std::string& slot_name, int64_t* mtime)
{
	WindowsHandle root(OpenSaveDataRoot());
	if (!root.IsValid())
	{
		return IsMissingWindowsError(GetLastError()) ? SAVE_DATA_ERROR_NOT_MOUNTED : SAVE_DATA_ERROR_INTERNAL;
	}
	WindowsRootSnapshot root_before {};
	if (!ReadWindowsRootSnapshot(root.Get(), &root_before) || !IsRealWindowsDirectory(root_before))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	const std::string path = std::string("_SaveData/") + slot_name;
	const auto utf16 = String::FromUtf8(path.c_str()).utf16_str();
	WindowsHandle directory(CreateFileW(reinterpret_cast<LPCWSTR>(utf16.GetData()), FILE_READ_ATTRIBUTES,
	                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
	                                    nullptr));
	const DWORD directory_error = directory.IsValid() ? ERROR_SUCCESS : GetLastError();
	WindowsRootSnapshot before {};
	if (directory.IsValid() && (!ReadWindowsRootSnapshot(directory.Get(), &before) || !IsRealWindowsDirectory(before) ||
	                            !WindowsTimeToUnix(before.write_time, mtime)))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	WindowsHandle root_verify(OpenSaveDataRoot());
	WindowsRootSnapshot root_after {};
	WindowsRootSnapshot root_path {};
	if (!ReadWindowsRootSnapshot(root.Get(), &root_after) || !IsRealWindowsDirectory(root_after) ||
	    !SameWindowsRoot(root_before, root_after) || !root_verify.IsValid() ||
	    !ReadWindowsRootSnapshot(root_verify.Get(), &root_path) || !IsRealWindowsDirectory(root_path) ||
	    !SameWindowsRoot(root_after, root_path))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	if (!directory.IsValid())
	{
		return IsMissingWindowsError(directory_error) ? SAVE_DATA_ERROR_NOT_MOUNTED : SAVE_DATA_ERROR_INTERNAL;
	}

	WindowsHandle verify(CreateFileW(reinterpret_cast<LPCWSTR>(utf16.GetData()), FILE_READ_ATTRIBUTES,
	                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	WindowsRootSnapshot after {};
	if (!verify.IsValid() || !ReadWindowsRootSnapshot(verify.Get(), &after) || !IsRealWindowsDirectory(after) ||
	    !SameWindowsRoot(before, after))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	return OK;
}

#else

class PosixDirectory
{
public:
	explicit PosixDirectory(DIR* directory): m_directory(directory) {}
	~PosixDirectory()
	{
		if (m_directory != nullptr)
		{
			closedir(m_directory);
		}
	}

	[[nodiscard]] DIR* Get() const { return m_directory; }

	LUMEN_CLASS_NO_COPY(PosixDirectory);

private:
	DIR* m_directory;
};

int64_t StatMtimeSeconds(const struct stat& value)
{
#if defined(__APPLE__)
	return static_cast<int64_t>(value.st_mtimespec.tv_sec);
#else
	return static_cast<int64_t>(value.st_mtim.tv_sec);
#endif
}

long StatMtimeNanoseconds(const struct stat& value)
{
#if defined(__APPLE__)
	return value.st_mtimespec.tv_nsec;
#else
	return value.st_mtim.tv_nsec;
#endif
}

bool SameMtime(const struct stat& left, const struct stat& right)
{
	return StatMtimeSeconds(left) == StatMtimeSeconds(right) &&
	       StatMtimeNanoseconds(left) == StatMtimeNanoseconds(right);
}

bool SameCtime(const struct stat& left, const struct stat& right)
{
#if defined(__APPLE__)
	return left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec &&
	       left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
	return left.st_ctim.tv_sec == right.st_ctim.tv_sec && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
}

bool SamePosixIdentity(const struct stat& left, const struct stat& right)
{
	return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool SamePosixEntry(const struct stat& left, const struct stat& right)
{
	return SamePosixIdentity(left, right) && left.st_mode == right.st_mode && SameMtime(left, right) &&
	       SameCtime(left, right);
}

bool SamePosixRoot(const struct stat& left, const struct stat& right)
{
	return SamePosixEntry(left, right) && left.st_nlink == right.st_nlink && left.st_uid == right.st_uid &&
	       left.st_gid == right.st_gid && left.st_size == right.st_size;
}

bool PosixTimeToUnix(const struct stat& value, int64_t* seconds)
{
	static_assert(sizeof(decltype(value.st_mtime)) <= sizeof(int64_t));
	static_assert(std::numeric_limits<decltype(value.st_mtime)>::is_signed);

	const long nanoseconds = StatMtimeNanoseconds(value);
	if (nanoseconds < 0 || nanoseconds >= 1000000000L)
	{
		return false;
	}

	*seconds = StatMtimeSeconds(value);
	if (*seconds < 0 && nanoseconds != 0)
	{
		(*seconds)++;
	}
	return true;
}

struct PosixObservedDirectory
{
	SearchEntry entry;
	struct stat status;
};

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
int ReadSaveDataSlotParam(const std::string& slot_name, SaveDataParam* snapshot,
	                     const struct stat* expected_root = nullptr, const struct stat* expected_slot = nullptr);
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
std::atomic<void (*)()> g_metadata_catalog_test_hook {nullptr};
#endif
#endif

int CollectSearchEntries(const std::string& pattern, std::vector<SearchEntry>* entries)
{
	int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	const int root_fd = open("_SaveData", flags);
	if (root_fd < 0)
	{
		const int first_error = errno;
		if (first_error != ENOENT)
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}

		struct stat verify_status {};
		return lstat("_SaveData", &verify_status) < 0 && errno == ENOENT ? OK : SAVE_DATA_ERROR_INTERNAL;
	}

	DIR* const raw_directory = fdopendir(root_fd);
	if (raw_directory == nullptr)
	{
		close(root_fd);
		return SAVE_DATA_ERROR_INTERNAL;
	}
	PosixDirectory directory(raw_directory);
	const int      directory_fd = dirfd(directory.Get());
	if (directory_fd < 0)
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	struct stat root_before {};
	if (fstat(directory_fd, &root_before) != 0 || !S_ISDIR(root_before.st_mode))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	std::vector<PosixObservedDirectory> observed_directories;
	for (;;)
	{
		errno = 0;
		dirent* const directory_entry = readdir(directory.Get());
		if (directory_entry == nullptr)
		{
			if (errno != 0)
			{
				return SAVE_DATA_ERROR_INTERNAL;
			}
			break;
		}

		const std::string name(directory_entry->d_name);
		if (name == "." || name == "..")
		{
			continue;
		}

		struct stat entry_status {};
		if (fstatat(directory_fd, name.c_str(), &entry_status, AT_SYMLINK_NOFOLLOW) != 0)
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
		if (!S_ISDIR(entry_status.st_mode) || S_ISLNK(entry_status.st_mode))
		{
			continue;
		}
		if (!IsValidUtf8(name))
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
		if (name.empty() || name.size() >= sizeof(SceSaveDataDirName) || name.compare(0, 4, "sce_") == 0)
		{
			continue;
		}

		PosixObservedDirectory observed {};
		observed.entry.name = name;
		observed.status     = entry_status;
		if (!PosixTimeToUnix(entry_status, &observed.entry.mtime))
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
		observed_directories.push_back(std::move(observed));
	}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
	if (const auto hook = g_metadata_catalog_test_hook.exchange(nullptr)) hook();
#endif
	for (auto& observed: observed_directories)
	{
		const int metadata_result = ReadSaveDataSlotParam(observed.entry.name, &observed.entry.param,
		                                               &root_before, &observed.status);
		if (metadata_result != OK) return SAVE_DATA_ERROR_INTERNAL;
		observed.entry.mtime = observed.entry.param.mtime;
	}
#endif
	for (const auto& observed: observed_directories)
	{
		struct stat current_status {};
		if (fstatat(directory_fd, observed.entry.name.c_str(), &current_status, AT_SYMLINK_NOFOLLOW) != 0 ||
		    !S_ISDIR(current_status.st_mode) || S_ISLNK(current_status.st_mode) ||
		    !SamePosixEntry(observed.status, current_status))
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
	}

	struct stat root_after {};
	struct stat path_status {};
	if (fstat(directory_fd, &root_after) != 0 || !S_ISDIR(root_after.st_mode) ||
	    !SamePosixRoot(root_before, root_after) || lstat("_SaveData", &path_status) != 0 ||
	    !S_ISDIR(path_status.st_mode) || S_ISLNK(path_status.st_mode) || !SamePosixRoot(root_after, path_status))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	for (auto& observed: observed_directories)
	{
		if (pattern.empty() || WildcardMatches(pattern, observed.entry.name))
		{
			if (entries->size() == std::numeric_limits<uint32_t>::max())
			{
				return SAVE_DATA_ERROR_INTERNAL;
			}
			entries->push_back(std::move(observed.entry));
		}
	}

	return OK;
}

#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
int ReadSaveDataSlotMtime(const std::string& slot_name, int64_t* mtime)
{
	int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	const int root_fd = open("_SaveData", flags);
	if (root_fd < 0)
	{
		return errno == ENOENT ? SAVE_DATA_ERROR_NOT_MOUNTED : SAVE_DATA_ERROR_INTERNAL;
	}
	DIR* const raw_directory = fdopendir(root_fd);
	if (raw_directory == nullptr)
	{
		close(root_fd);
		return SAVE_DATA_ERROR_INTERNAL;
	}
	PosixDirectory directory(raw_directory);
	const int directory_fd = dirfd(directory.Get());
	if (directory_fd < 0)
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	struct stat root_before {};
	struct stat slot_before {};
	if (fstat(directory_fd, &root_before) != 0 || !S_ISDIR(root_before.st_mode))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	if (fstatat(directory_fd, slot_name.c_str(), &slot_before, AT_SYMLINK_NOFOLLOW) != 0)
	{
		return errno == ENOENT ? SAVE_DATA_ERROR_NOT_MOUNTED : SAVE_DATA_ERROR_INTERNAL;
	}
	if (!S_ISDIR(slot_before.st_mode) || S_ISLNK(slot_before.st_mode) || !PosixTimeToUnix(slot_before, mtime))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}

	struct stat slot_after {};
	struct stat root_after {};
	struct stat path_status {};
	if (fstatat(directory_fd, slot_name.c_str(), &slot_after, AT_SYMLINK_NOFOLLOW) != 0 ||
	    !S_ISDIR(slot_after.st_mode) || S_ISLNK(slot_after.st_mode) || !SamePosixEntry(slot_before, slot_after) ||
	    fstat(directory_fd, &root_after) != 0 || !S_ISDIR(root_after.st_mode) ||
	    !SamePosixRoot(root_before, root_after) || lstat("_SaveData", &path_status) != 0 ||
	    !S_ISDIR(path_status.st_mode) || S_ISLNK(path_status.st_mode) || !SamePosixRoot(root_after, path_status))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	return OK;
}

#endif

#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID

// Native directory attributes are outside the guest file namespace. On macOS,
// require native volume support rather than permitting an AppleDouble fallback.
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
constexpr char SAVE_DATA_PARAM_ATTRIBUTE[] = "com.lumen.savedata.params";
#else
constexpr char SAVE_DATA_PARAM_ATTRIBUTE[] = "user.com.lumen.savedata.params";
#endif
constexpr uint8_t SAVE_DATA_PARAM_MAGIC[8] = {'L', 'U', 'M', 'E', 'N', 'S', 'D', 'P'};

struct NativeSaveDataRecord
{
	uint8_t magic[8];
	uint32_t version;
	uint32_t fields;
	SaveDataParam value;
};
static_assert(sizeof(NativeSaveDataRecord) == 1344);

ssize_t ReadSaveDataParamAttribute(int fd, void* value, size_t size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	return fgetxattr(fd, SAVE_DATA_PARAM_ATTRIBUTE, value, size, 0, 0);
#else
	return fgetxattr(fd, SAVE_DATA_PARAM_ATTRIBUTE, value, size);
#endif
}

int WriteSaveDataParamAttribute(int fd, const void* value, size_t size)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	return fsetxattr(fd, SAVE_DATA_PARAM_ATTRIBUTE, value, size, 0, 0);
#else
	return fsetxattr(fd, SAVE_DATA_PARAM_ATTRIBUTE, value, size, 0);
#endif
}

bool IsMissingSaveDataParamAttribute(int error)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	return error == ENOATTR;
#else
	return error == ENODATA;
#endif
}

void ApplySaveDataParamFields(const SaveDataParam& source, uint32_t fields, SaveDataParam* destination)
{
	if ((fields & (1u << (SAVE_DATA_PARAM_TITLE - 1u))) != 0)
		std::memcpy(destination->title, source.title, sizeof(destination->title));
	if ((fields & (1u << (SAVE_DATA_PARAM_SUB_TITLE - 1u))) != 0)
		std::memcpy(destination->sub_title, source.sub_title, sizeof(destination->sub_title));
	if ((fields & (1u << (SAVE_DATA_PARAM_DETAIL - 1u))) != 0)
		std::memcpy(destination->detail, source.detail, sizeof(destination->detail));
	if ((fields & (1u << (SAVE_DATA_PARAM_USER_PARAM - 1u))) != 0) destination->user_param = source.user_param;
	if ((fields & (1u << (SAVE_DATA_PARAM_MTIME - 1u))) != 0) destination->mtime = source.mtime;
}

class NativeSaveDataSlot
{
public:
	NativeSaveDataSlot() = default;
	NativeSaveDataSlot(const NativeSaveDataSlot&) = delete;
	NativeSaveDataSlot& operator=(const NativeSaveDataSlot&) = delete;
	~NativeSaveDataSlot()
	{
		if (m_slot_fd >= 0) close(m_slot_fd);
		if (m_root_fd >= 0) close(m_root_fd);
	}

	int Open(const std::string& name, bool writing, const struct stat* expected_root = nullptr,
	         const struct stat* expected_slot = nullptr)
	{
		m_name = name;
		const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
		m_root_fd = open("_SaveData", flags);
		if (m_root_fd < 0) return errno == ENOENT ? SAVE_DATA_ERROR_NOT_MOUNTED : SAVE_DATA_ERROR_INTERNAL;
		if (fstat(m_root_fd, &m_root_status) != 0 || !S_ISDIR(m_root_status.st_mode)) return SAVE_DATA_ERROR_INTERNAL;
		m_slot_fd = openat(m_root_fd, name.c_str(), flags);
		if (m_slot_fd < 0) return errno == ENOENT ? SAVE_DATA_ERROR_NOT_MOUNTED : SAVE_DATA_ERROR_INTERNAL;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		struct attrlist query {};
		query.bitmapcount = ATTR_BIT_MAP_COUNT;
		query.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
		struct VolumeCapabilities { uint32_t length; vol_capabilities_attr_t value; } capabilities {};
		constexpr uint32_t required = VOL_CAP_INT_EXTENDED_ATTR | VOL_CAP_INT_FLOCK;
		if (fgetattrlist(m_slot_fd, &query, &capabilities, sizeof(capabilities), 0) != 0 ||
		    capabilities.length != sizeof(capabilities) ||
		    (capabilities.value.valid[VOL_CAPABILITIES_INTERFACES] & required) != required ||
		    (capabilities.value.capabilities[VOL_CAPABILITIES_INTERFACES] & required) != required)
			return SAVE_DATA_ERROR_INTERNAL;
#endif
		int result;
		do { result = flock(m_slot_fd, writing ? LOCK_EX : LOCK_SH); } while (result != 0 && errno == EINTR);
		if (result != 0 || fstat(m_slot_fd, &m_slot_status) != 0 || !S_ISDIR(m_slot_status.st_mode))
			return SAVE_DATA_ERROR_INTERNAL;
		if ((expected_root != nullptr && !SamePosixRoot(*expected_root, m_root_status)) ||
		    (expected_slot != nullptr && !SamePosixEntry(*expected_slot, m_slot_status)))
			return SAVE_DATA_ERROR_INTERNAL;
		return Validate(false);
	}

	int Read(NativeSaveDataRecord* record, SaveDataParam* snapshot)
	{
		std::memset(record, 0, sizeof(*record));
		std::memset(snapshot, 0, sizeof(*snapshot));
		if (!PosixTimeToUnix(m_slot_status, &snapshot->mtime)) return SAVE_DATA_ERROR_INTERNAL;
		const ssize_t count = ReadSaveDataParamAttribute(m_slot_fd, record, sizeof(*record));
		if (count < 0)
		{
			if (!IsMissingSaveDataParamAttribute(errno)) return SAVE_DATA_ERROR_INTERNAL;
			// Legacy slots have no recoverable title. Do not infer it from files.
			std::memset(record, 0, sizeof(*record));
			return Validate(false);
		}
		SaveDataParamUpdate validated {};
		if (count != sizeof(*record) || std::memcmp(record->magic, SAVE_DATA_PARAM_MAGIC, sizeof(record->magic)) != 0 ||
		    record->version != 1 || record->fields == 0 || (record->fields & ~SAVE_DATA_PARAM_FIELD_MASK) != 0 ||
		    !PrepareSaveDataParamUpdate(SAVE_DATA_PARAM_ALL, &record->value, sizeof(record->value), &validated) ||
		    std::memcmp(&validated.value, &record->value, sizeof(record->value)) != 0)
			return SAVE_DATA_ERROR_INTERNAL;
		ApplySaveDataParamFields(record->value, record->fields, snapshot);
		return Validate(false);
	}

	int Write(const NativeSaveDataRecord& record)
	{
		if (Validate(false) != OK) return SAVE_DATA_ERROR_INTERNAL;
		if (WriteSaveDataParamAttribute(m_slot_fd, &record, sizeof(record)) != 0)
			return SAVE_DATA_ERROR_INTERNAL;
		return Validate(true);
	}

private:
	int Validate(bool after_write) const
	{
		struct stat root_now {}, root_path {}, slot_now {}, slot_path {};
		if (fstat(m_root_fd, &root_now) != 0 || lstat("_SaveData", &root_path) != 0 ||
		    !S_ISDIR(root_path.st_mode) || !SamePosixRoot(m_root_status, root_now) || !SamePosixRoot(root_now, root_path) ||
		    fstat(m_slot_fd, &slot_now) != 0 || fstatat(m_root_fd, m_name.c_str(), &slot_path, AT_SYMLINK_NOFOLLOW) != 0 ||
		    !S_ISDIR(slot_path.st_mode) || !SamePosixEntry(slot_now, slot_path) ||
		    !SamePosixIdentity(m_slot_status, slot_now) || m_slot_status.st_mode != slot_now.st_mode ||
		    m_slot_status.st_uid != slot_now.st_uid || m_slot_status.st_gid != slot_now.st_gid ||
		    !SameMtime(m_slot_status, slot_now) || (!after_write && !SameCtime(m_slot_status, slot_now)))
			return SAVE_DATA_ERROR_INTERNAL;
		return OK;
	}

	int m_root_fd = -1;
	int m_slot_fd = -1;
	std::string m_name;
	struct stat m_root_status {}, m_slot_status {};
};

int ReadSaveDataSlotParam(const std::string& slot_name, SaveDataParam* snapshot,
	                     const struct stat* expected_root, const struct stat* expected_slot)
{
	NativeSaveDataSlot slot;
	const int opened = slot.Open(slot_name, false, expected_root, expected_slot);
	if (opened != OK) return opened;
	NativeSaveDataRecord record {};
	return slot.Read(&record, snapshot);
}

int WriteSaveDataSlotParam(const std::string& slot_name, const SaveDataParamUpdate& update)
{
	NativeSaveDataSlot slot;
	const int opened = slot.Open(slot_name, true);
	if (opened != OK) return opened;
	NativeSaveDataRecord record {};
	SaveDataParam snapshot {};
	const int read_result = slot.Read(&record, &snapshot);
	if (read_result != OK) return read_result;
	if (update.replace_all) std::memset(&record, 0, sizeof(record));
	std::memcpy(record.magic, SAVE_DATA_PARAM_MAGIC, sizeof(record.magic));
	record.version = 1;
	record.fields |= update.fields;
	ApplySaveDataParamFields(update.value, update.fields, &record.value);
	return slot.Write(record);
}

#else

int ReadSaveDataSlotParam(const std::string& slot_name, SaveDataParam* snapshot)
{
	std::memset(snapshot, 0, sizeof(*snapshot));
	return ReadSaveDataSlotMtime(slot_name, &snapshot->mtime);
}

#endif

constexpr size_t   SAVE_DATA_MEMORY_HEADER_SIZE = 64;
constexpr uint32_t SAVE_DATA_MEMORY_FORMAT_VERSION = 1;
constexpr uint32_t SAVE_DATA_MEMORY_HAS_PARAM = 1u;
constexpr uint32_t SAVE_DATA_MEMORY_SETUP_OPTION_MASK = 0x7u;
constexpr uint8_t  SAVE_DATA_MEMORY_MAGIC[8] = {'L', 'U', 'M', 'E', 'N', 'S', 'D', 'M'};
// General host-safety quota for this in-memory service. It is intentionally
// title-independent and prevents an untrusted guest size from committing the
// process to an unbounded allocation.
constexpr uint64_t SAVE_DATA_MEMORY_CAPACITY_LIMIT = 32ull * 1024ull * 1024ull;
// Bound the fixed per-slot state as well as payload bytes. Empty slots still
// allocate a map node, a bounded title key and a SaveDataParam, so a payload-
// only quota would permit unbounded guest-driven host allocation.
constexpr size_t   SAVE_DATA_MEMORY_SLOT_LIMIT = 1024;
// A title may attach several slots. Bound the resident payload independently
// of the per-slot capacity so repeated Setup/Set calls cannot grow the process
// without limit. This excludes temporary transactional copies, whose bounded
// worst case is documented beside the interface contract.
constexpr uint64_t SAVE_DATA_MEMORY_RESIDENT_LIMIT = 128ull * 1024ull * 1024ull;
constexpr uint64_t SAVE_DATA_MEMORY_CONTAINER_LIMIT = SAVE_DATA_MEMORY_HEADER_SIZE +
	                                                   SAVE_DATA_MEMORY_CAPACITY_LIMIT +
	                                                   sizeof(SaveDataParam);

std::atomic<uint64_t> g_save_data_memory_temp_sequence {1};

bool AddSize(size_t left, size_t right, size_t* result)
{
	if (right > std::numeric_limits<size_t>::max() - left) return false;
	*result = left + right;
	return true;
}

void StoreLe32(uint8_t* destination, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i) destination[i] = static_cast<uint8_t>(value >> (i * 8u));
}

void StoreLe64(uint8_t* destination, uint64_t value)
{
	for (unsigned i = 0; i < 8; ++i) destination[i] = static_cast<uint8_t>(value >> (i * 8u));
}

uint32_t LoadLe32(const uint8_t* source)
{
	uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(source[i]) << (i * 8u);
	return value;
}

uint64_t LoadLe64(const uint8_t* source)
{
	uint64_t value = 0;
	for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(source[i]) << (i * 8u);
	return value;
}

uint64_t HashBytes(uint64_t hash, const uint8_t* bytes, size_t size)
{
	constexpr uint64_t FNV_PRIME = 1099511628211ull;
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= FNV_PRIME;
	}
	return hash;
}

uint64_t SaveDataMemoryChecksum(const std::vector<uint8_t>& container)
{
	constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
	uint64_t hash = HashBytes(FNV_OFFSET, container.data(), 48);
	hash = HashBytes(hash, container.data() + 56, SAVE_DATA_MEMORY_HEADER_SIZE - 56);
	return HashBytes(hash, container.data() + SAVE_DATA_MEMORY_HEADER_SIZE,
	                 container.size() - SAVE_DATA_MEMORY_HEADER_SIZE);
}

bool SerializeSaveDataMemory(const SaveDataMemorySlot& slot, uint64_t generation,
	                         std::vector<uint8_t>* container)
{
	if (slot.data.size() > SAVE_DATA_MEMORY_CAPACITY_LIMIT) return false;
	const size_t param_size = slot.has_param ? sizeof(SaveDataParam) : 0;
	size_t total_size = 0;
	if (!AddSize(SAVE_DATA_MEMORY_HEADER_SIZE, slot.data.size(), &total_size) ||
	    !AddSize(total_size, param_size, &total_size) ||
	    total_size > SAVE_DATA_MEMORY_CONTAINER_LIMIT) return false;

	container->assign(total_size, 0);
	std::memcpy(container->data(), SAVE_DATA_MEMORY_MAGIC, sizeof(SAVE_DATA_MEMORY_MAGIC));
	StoreLe32(container->data() + 8, SAVE_DATA_MEMORY_FORMAT_VERSION);
	StoreLe32(container->data() + 12, static_cast<uint32_t>(SAVE_DATA_MEMORY_HEADER_SIZE));
	StoreLe64(container->data() + 16, static_cast<uint64_t>(slot.data.size()));
	StoreLe64(container->data() + 24, generation);
	StoreLe32(container->data() + 32, slot.has_param ? SAVE_DATA_MEMORY_HAS_PARAM : 0);
	StoreLe32(container->data() + 36, static_cast<uint32_t>(param_size));
	if (!slot.data.empty())
		std::memcpy(container->data() + SAVE_DATA_MEMORY_HEADER_SIZE, slot.data.data(), slot.data.size());
	if (slot.has_param)
		std::memcpy(container->data() + SAVE_DATA_MEMORY_HEADER_SIZE + slot.data.size(), &slot.param,
		            sizeof(slot.param));
	StoreLe64(container->data() + 48, SaveDataMemoryChecksum(*container));
	return true;
}

int InspectSaveDataMemoryContainer(const std::vector<uint8_t>& container,
	                               size_t* data_size, uint64_t* generation,
	                               bool* has_param)
{
	if (container.size() < SAVE_DATA_MEMORY_HEADER_SIZE ||
	    std::memcmp(container.data(), SAVE_DATA_MEMORY_MAGIC, sizeof(SAVE_DATA_MEMORY_MAGIC)) != 0 ||
	    LoadLe32(container.data() + 8) != SAVE_DATA_MEMORY_FORMAT_VERSION ||
	    LoadLe32(container.data() + 12) != SAVE_DATA_MEMORY_HEADER_SIZE || LoadLe64(container.data() + 40) != 0 ||
	    LoadLe64(container.data() + 56) != 0)
		return SAVE_DATA_ERROR_INTERNAL;

	const uint64_t data_size64 = LoadLe64(container.data() + 16);
	const uint64_t stored_generation = LoadLe64(container.data() + 24);
	const uint32_t flags = LoadLe32(container.data() + 32);
	const uint32_t param_size = LoadLe32(container.data() + 36);
	if ((flags & ~SAVE_DATA_MEMORY_HAS_PARAM) != 0 ||
	    param_size != ((flags & SAVE_DATA_MEMORY_HAS_PARAM) != 0 ? sizeof(SaveDataParam) : 0) ||
	    data_size64 > SAVE_DATA_MEMORY_CAPACITY_LIMIT ||
	    data_size64 > std::numeric_limits<size_t>::max()) return SAVE_DATA_ERROR_INTERNAL;

	const size_t stored_data_size = static_cast<size_t>(data_size64);
	size_t expected_size = 0;
	if (!AddSize(SAVE_DATA_MEMORY_HEADER_SIZE, stored_data_size, &expected_size) ||
	    !AddSize(expected_size, param_size, &expected_size) || expected_size != container.size() ||
	    LoadLe64(container.data() + 48) != SaveDataMemoryChecksum(container))
		return SAVE_DATA_ERROR_INTERNAL;
	*data_size = stored_data_size;
	*generation = stored_generation;
	*has_param = (flags & SAVE_DATA_MEMORY_HAS_PARAM) != 0;
	return OK;
}

int ParseSaveDataMemory(std::vector<uint8_t>* container, SaveDataMemorySlot* slot)
{
	size_t data_size = 0;
	uint64_t generation = 0;
	bool has_param = false;
	const int inspect_result = InspectSaveDataMemoryContainer(*container, &data_size, &generation, &has_param);
	if (inspect_result != OK) return inspect_result;

	slot->generation = generation;
	slot->has_param = has_param;
	std::memset(&slot->param, 0, sizeof(slot->param));
	if (slot->has_param)
		std::memcpy(&slot->param, container->data() + SAVE_DATA_MEMORY_HEADER_SIZE + data_size,
		            sizeof(slot->param));
	if (data_size != 0)
		std::memmove(container->data(), container->data() + SAVE_DATA_MEMORY_HEADER_SIZE, data_size);
	container->resize(data_size);
	slot->data = std::move(*container);
	return OK;
}

std::string HexComponent(const std::string& value)
{
	constexpr char HEX[] = "0123456789abcdef";
	std::string result;
	result.reserve(value.size() * 2);
	for (const unsigned char byte: value)
	{
		result.push_back(HEX[byte >> 4u]);
		result.push_back(HEX[byte & 0x0fu]);
	}
	return result;
}

std::string UserComponent(int32_t user_id)
{
	constexpr char HEX[] = "0123456789abcdef";
	const uint32_t value = static_cast<uint32_t>(user_id);
	std::string result(8, '0');
	for (unsigned i = 0; i < 8; ++i) result[7 - i] = HEX[(value >> (i * 4u)) & 0x0fu];
	return result;
}

std::string SaveDataMemoryFileName(uint32_t slot_id)
{
	return std::string("slot-") + std::to_string(slot_id) + ".bin";
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS

std::string SaveDataMemoryDirectoryPath(const SaveDataMemoryKey& key)
{
	return std::string("_SaveData/sce_memory/") + HexComponent(key.title_id) + "/" + UserComponent(key.user_id);
}

bool EnsureWindowsMemoryDirectory(const SaveDataMemoryKey& key)
{
	std::string path;
	for (const std::string& component: {std::string("_SaveData"), std::string("sce_memory"),
	                                   HexComponent(key.title_id), UserComponent(key.user_id)})
	{
		path = path.empty() ? component : path + "/" + component;
		const HostDirectoryKind kind = ClassifyHostDirectory(path);
		if (kind == HostDirectoryKind::Directory) continue;
		if (kind != HostDirectoryKind::Missing || CreateHostDirectory(path) == HostDirectoryCreateResult::Error ||
		    ClassifyHostDirectory(path) != HostDirectoryKind::Directory) return false;
	}
	return true;
}

int ReadSaveDataMemoryContainerPath(const std::string& path, std::vector<uint8_t>* bytes, bool* exists)
{
	*exists = false;
	bytes->clear();
	const auto wide = String::FromUtf8(path.c_str()).utf16_str();
	WindowsHandle file(CreateFileW(reinterpret_cast<LPCWSTR>(wide.GetData()), GENERIC_READ,
	                               FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
	                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (!file.IsValid()) return IsMissingWindowsError(GetLastError()) ? OK : SAVE_DATA_ERROR_INTERNAL;
	BY_HANDLE_FILE_INFORMATION before {};
	if (!GetFileInformationByHandle(file.Get(), &before) ||
	    (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
		return SAVE_DATA_ERROR_INTERNAL;
	const uint64_t size64 = (static_cast<uint64_t>(before.nFileSizeHigh) << 32u) | before.nFileSizeLow;
	if (size64 > std::numeric_limits<size_t>::max() || size64 > SAVE_DATA_MEMORY_CONTAINER_LIMIT)
		return SAVE_DATA_ERROR_INTERNAL;
	bytes->resize(static_cast<size_t>(size64));
	for (size_t offset = 0; offset < bytes->size();)
	{
		const DWORD amount = static_cast<DWORD>(std::min<size_t>(bytes->size() - offset, MAXDWORD));
		DWORD read = 0;
		if (!ReadFile(file.Get(), bytes->data() + offset, amount, &read, nullptr) || read == 0)
			return SAVE_DATA_ERROR_INTERNAL;
		offset += read;
	}
	BY_HANDLE_FILE_INFORMATION after {};
	if (!GetFileInformationByHandle(file.Get(), &after) || before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
	    before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow ||
	    before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow ||
	    FileTimeValue(before.ftLastWriteTime) != FileTimeValue(after.ftLastWriteTime))
		return SAVE_DATA_ERROR_INTERNAL;
	*exists = true;
	return OK;
}

int ReadSaveDataMemoryContainer(const SaveDataMemoryKey& key, std::vector<uint8_t>* bytes, bool* exists)
{
	const std::string path = SaveDataMemoryDirectoryPath(key) + "/" + SaveDataMemoryFileName(key.slot_id);
	return ReadSaveDataMemoryContainerPath(path, bytes, exists);
}

int WriteSaveDataMemoryContainer(const SaveDataMemoryKey& key, uint64_t expected_generation,
	                             const std::vector<uint8_t>& bytes)
{
	if (!EnsureWindowsMemoryDirectory(key)) return SAVE_DATA_ERROR_INTERNAL;
	const std::string directory = SaveDataMemoryDirectoryPath(key);
	const std::string target = directory + "/" + SaveDataMemoryFileName(key.slot_id);
	const std::string lock_path = directory + "/.lumen-save-memory.lock";
	const auto wide_lock = String::FromUtf8(lock_path.c_str()).utf16_str();
	WindowsHandle transaction_lock(CreateFileW(reinterpret_cast<LPCWSTR>(wide_lock.GetData()),
	                                           GENERIC_READ | GENERIC_WRITE,
	                                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
	                                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	if (!transaction_lock.IsValid()) return SAVE_DATA_ERROR_INTERNAL;
	BY_HANDLE_FILE_INFORMATION lock_information {};
	if (!GetFileInformationByHandle(transaction_lock.Get(), &lock_information) ||
	    (lock_information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
		return SAVE_DATA_ERROR_INTERNAL;
	OVERLAPPED lock_overlapped {};
	if (LockFileEx(transaction_lock.Get(), LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &lock_overlapped) == 0)
		return SAVE_DATA_ERROR_INTERNAL;
	{
		std::vector<uint8_t> current;
		bool current_exists = false;
		const int read_result = ReadSaveDataMemoryContainerPath(target, &current, &current_exists);
		if (read_result != OK) return read_result;
		if (current_exists)
		{
			size_t current_size = 0;
			uint64_t current_generation = 0;
			bool current_has_param = false;
			const int inspect_result = InspectSaveDataMemoryContainer(
			    current, &current_size, &current_generation, &current_has_param);
			(void)current_size;
			(void)current_has_param;
			if (inspect_result != OK || current_generation != expected_generation)
				return SAVE_DATA_ERROR_INTERNAL;
		} else if (expected_generation != 0)
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
	}
	const uint64_t sequence = g_save_data_memory_temp_sequence.fetch_add(1, std::memory_order_relaxed);
	const std::string temporary = target + ".tmp-" + std::to_string(GetCurrentProcessId()) + "-" +
	                              std::to_string(sequence);
	const auto wide_temp = String::FromUtf8(temporary.c_str()).utf16_str();
	HANDLE raw = CreateFileW(reinterpret_cast<LPCWSTR>(wide_temp.GetData()), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
	                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (raw == INVALID_HANDLE_VALUE) return SAVE_DATA_ERROR_INTERNAL;
	bool ok = true;
	for (size_t offset = 0; ok && offset < bytes.size();)
	{
		const DWORD amount = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, MAXDWORD));
		DWORD written = 0;
		ok = WriteFile(raw, bytes.data() + offset, amount, &written, nullptr) != 0 && written != 0;
		offset += written;
	}
	ok = ok && FlushFileBuffers(raw) != 0;
	ok = CloseHandle(raw) != 0 && ok;
	const auto wide_target = String::FromUtf8(target.c_str()).utf16_str();
	if (ok)
	{
		const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(wide_target.GetData()));
		ok = attributes == INVALID_FILE_ATTRIBUTES ||
		     (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
	}
	if (ok)
		ok = MoveFileExW(reinterpret_cast<LPCWSTR>(wide_temp.GetData()),
		                 reinterpret_cast<LPCWSTR>(wide_target.GetData()),
		                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
	if (!ok) DeleteFileW(reinterpret_cast<LPCWSTR>(wide_temp.GetData()));
	return ok ? OK : SAVE_DATA_ERROR_INTERNAL;
}

#else

class SaveDataMemoryFd
{
public:
	SaveDataMemoryFd() = default;
	explicit SaveDataMemoryFd(int fd): m_fd(fd) {}
	~SaveDataMemoryFd() { if (m_fd >= 0) close(m_fd); }
	SaveDataMemoryFd(const SaveDataMemoryFd&) = delete;
	SaveDataMemoryFd& operator=(const SaveDataMemoryFd&) = delete;
	SaveDataMemoryFd(SaveDataMemoryFd&& other) noexcept: m_fd(other.m_fd) { other.m_fd = -1; }
	SaveDataMemoryFd& operator=(SaveDataMemoryFd&& other) noexcept
	{
		if (this != &other)
		{
			if (m_fd >= 0) close(m_fd);
			m_fd = other.m_fd;
			other.m_fd = -1;
		}
		return *this;
	}
	int Get() const { return m_fd; }
	bool IsValid() const { return m_fd >= 0; }
private:
	int m_fd = -1;
};

int OpenSaveDataMemoryDirectory(const SaveDataMemoryKey& key, bool create, SaveDataMemoryFd* directory,
	                            bool* exists)
{
	*exists = false;
	int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	SaveDataMemoryFd current(open(".", flags));
	if (!current.IsValid()) return SAVE_DATA_ERROR_INTERNAL;
	const std::array<std::string, 4> components = {
	    "_SaveData", "sce_memory", HexComponent(key.title_id), UserComponent(key.user_id)};
	for (const auto& component: components)
	{
		int next = openat(current.Get(), component.c_str(), flags);
		if (next < 0 && errno == ENOENT)
		{
			if (!create) return OK;
			if (mkdirat(current.Get(), component.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) != 0 && errno != EEXIST)
				return SAVE_DATA_ERROR_INTERNAL;
			next = openat(current.Get(), component.c_str(), flags);
			if (next < 0) return SAVE_DATA_ERROR_INTERNAL;
			int sync_result;
			do { sync_result = fsync(current.Get()); } while (sync_result != 0 && errno == EINTR);
			if (sync_result != 0)
			{
				close(next);
				return SAVE_DATA_ERROR_INTERNAL;
			}
		}
		if (next < 0) return SAVE_DATA_ERROR_INTERNAL;
		struct stat status {};
		if (fstat(next, &status) != 0 || !S_ISDIR(status.st_mode))
		{
			close(next);
			return SAVE_DATA_ERROR_INTERNAL;
		}
		current = SaveDataMemoryFd(next);
	}
	*directory = std::move(current);
	*exists = true;
	return OK;
}

int LockSaveDataMemoryDirectory(int fd, int operation)
{
	int result;
	do { result = flock(fd, operation); } while (result != 0 && errno == EINTR);
	return result == 0 ? OK : SAVE_DATA_ERROR_INTERNAL;
}

int ReadSaveDataMemoryContainerAt(int directory_fd, const SaveDataMemoryKey& key,
	                              std::vector<uint8_t>* bytes, bool* exists)
{
	*exists = false;
	bytes->clear();
	int flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	SaveDataMemoryFd file(openat(directory_fd, SaveDataMemoryFileName(key.slot_id).c_str(), flags));
	if (!file.IsValid()) return errno == ENOENT ? OK : SAVE_DATA_ERROR_INTERNAL;
	struct stat before {};
	if (fstat(file.Get(), &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
	    static_cast<uint64_t>(before.st_size) > std::numeric_limits<size_t>::max() ||
	    static_cast<uint64_t>(before.st_size) > SAVE_DATA_MEMORY_CONTAINER_LIMIT)
		return SAVE_DATA_ERROR_INTERNAL;
	bytes->resize(static_cast<size_t>(before.st_size));
	for (size_t offset = 0; offset < bytes->size();)
	{
		const ssize_t count = read(file.Get(), bytes->data() + offset, bytes->size() - offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return SAVE_DATA_ERROR_INTERNAL;
		offset += static_cast<size_t>(count);
	}
	struct stat after {};
	if (fstat(file.Get(), &after) != 0 || !SamePosixEntry(before, after) || before.st_size != after.st_size)
		return SAVE_DATA_ERROR_INTERNAL;
	*exists = true;
	return OK;
}

int ReadSaveDataMemoryContainer(const SaveDataMemoryKey& key, std::vector<uint8_t>* bytes, bool* exists)
{
	SaveDataMemoryFd directory;
	bool directory_exists = false;
	const int directory_result = OpenSaveDataMemoryDirectory(key, false, &directory, &directory_exists);
	if (directory_result != OK || !directory_exists)
	{
		*exists = false;
		bytes->clear();
		return directory_result;
	}
	if (LockSaveDataMemoryDirectory(directory.Get(), LOCK_SH) != OK) return SAVE_DATA_ERROR_INTERNAL;
	return ReadSaveDataMemoryContainerAt(directory.Get(), key, bytes, exists);
}

int WriteSaveDataMemoryContainer(const SaveDataMemoryKey& key, uint64_t expected_generation,
	                             const std::vector<uint8_t>& bytes)
{
	SaveDataMemoryFd directory;
	bool directory_exists = false;
	const int directory_result = OpenSaveDataMemoryDirectory(key, true, &directory, &directory_exists);
	if (directory_result != OK || !directory_exists) return SAVE_DATA_ERROR_INTERNAL;
	if (LockSaveDataMemoryDirectory(directory.Get(), LOCK_EX) != OK) return SAVE_DATA_ERROR_INTERNAL;
	{
		std::vector<uint8_t> current;
		bool current_exists = false;
		const int read_result = ReadSaveDataMemoryContainerAt(directory.Get(), key, &current, &current_exists);
		if (read_result != OK) return read_result;
		if (current_exists)
		{
			size_t current_size = 0;
			uint64_t current_generation = 0;
			bool current_has_param = false;
			const int inspect_result = InspectSaveDataMemoryContainer(
			    current, &current_size, &current_generation, &current_has_param);
			(void)current_size;
			(void)current_has_param;
			if (inspect_result != OK || current_generation != expected_generation)
				return SAVE_DATA_ERROR_INTERNAL;
		} else if (expected_generation != 0)
		{
			return SAVE_DATA_ERROR_INTERNAL;
		}
	}
	const std::string target = SaveDataMemoryFileName(key.slot_id);
	const uint64_t sequence = g_save_data_memory_temp_sequence.fetch_add(1, std::memory_order_relaxed);
	const std::string temporary = target + ".tmp-" + std::to_string(static_cast<uint64_t>(getpid())) + "-" +
	                              std::to_string(sequence);
	int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	SaveDataMemoryFd file(openat(directory.Get(), temporary.c_str(), flags, S_IRUSR | S_IWUSR));
	if (!file.IsValid()) return SAVE_DATA_ERROR_INTERNAL;
	bool ok = true;
	for (size_t offset = 0; ok && offset < bytes.size();)
	{
		const ssize_t count = write(file.Get(), bytes.data() + offset, bytes.size() - offset);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) ok = false;
		else offset += static_cast<size_t>(count);
	}
	if (ok)
	{
		int result;
		do { result = fsync(file.Get()); } while (result != 0 && errno == EINTR);
		ok = result == 0;
	}
	struct stat target_status {};
	if (ok && fstatat(directory.Get(), target.c_str(), &target_status, AT_SYMLINK_NOFOLLOW) == 0)
		ok = S_ISREG(target_status.st_mode) && !S_ISLNK(target_status.st_mode);
	else if (ok && errno != ENOENT)
		ok = false;
	if (ok) ok = renameat(directory.Get(), temporary.c_str(), directory.Get(), target.c_str()) == 0;
	if (ok)
	{
		int result;
		do { result = fsync(directory.Get()); } while (result != 0 && errno == EINTR);
		ok = result == 0;
	}
	if (!ok) unlinkat(directory.Get(), temporary.c_str(), 0);
	return ok ? OK : SAVE_DATA_ERROR_INTERNAL;
}

#endif

int LoadSaveDataMemorySlot(const SaveDataMemoryKey& key, uint64_t capacity, SaveDataMemorySlot* slot,
	                       uint64_t* existing_size, bool* store_exists)
{
	std::vector<uint8_t> container;
	bool exists = false;
	const int read_result = ReadSaveDataMemoryContainer(key, &container, &exists);
	if (read_result != OK) return read_result;
	*slot = SaveDataMemorySlot {};
	slot->capacity = capacity;
	*existing_size = 0;
	*store_exists = exists;
	if (!exists) return OK;
	const int parse_result = ParseSaveDataMemory(&container, slot);
	if (parse_result != OK) return parse_result;
	*existing_size = static_cast<uint64_t>(slot->data.size());
	// The current attachment capacity limits guest-visible ranges, not durable
	// bytes already committed by a larger attachment. Keeping the full tail is
	// essential: a later Sync or partial Set must never truncate that data.
	slot->capacity = capacity;
	return OK;
}

int PersistSaveDataMemorySlot(const SaveDataMemoryKey& key, SaveDataMemorySlot* slot)
{
	if (slot->generation == std::numeric_limits<uint64_t>::max()) return SAVE_DATA_ERROR_INTERNAL;
	const uint64_t next_generation = slot->generation + 1;
	std::vector<uint8_t> container;
	if (!SerializeSaveDataMemory(*slot, next_generation, &container)) return SAVE_DATA_ERROR_OUT_OF_MEMORY;
	const int write_result = WriteSaveDataMemoryContainer(key, slot->generation, container);
	if (write_result == OK) slot->generation = next_generation;
	return write_result;
}

int ResolveSaveDataParamSlot(const SaveDataMountPoint* mount_point, std::string* mount_name, std::string* slot_name)
{
	if (!CopyBoundedString(mount_point->data, sizeof(mount_point->data), mount_name) || mount_name->empty() || mount_name->front() != '/')
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	const String mounted_path = String::FromUtf8(mount_name->c_str()).FixDirectorySlash();
	const String real_path    = LibKernel::FileSystem::GetRealFilename(mounted_path).FixDirectorySlash();
	const String root_path    = String(SAVE_DATA_DIR).FixDirectorySlash();
	if (real_path == mounted_path || !real_path.StartsWith(root_path) || real_path.Size() <= root_path.Size() + 1)
	{
		return SAVE_DATA_ERROR_NOT_MOUNTED;
	}

	const String slot = real_path.RemoveFirst(root_path.Size()).RemoveLast(1);
	if (slot.IsEmpty() || slot == U"." || slot == U".." || slot.ContainsChar(U'/') || slot.ContainsChar(U'\\'))
	{
		return SAVE_DATA_ERROR_NOT_MOUNTED;
	}

	const auto utf8 = slot.utf8_str();
	slot_name->assign(utf8.GetData());
	return slot_name->empty() || slot_name->size() >= sizeof(SceSaveDataDirName) || !IsValidUtf8(*slot_name)
	    ? SAVE_DATA_ERROR_NOT_MOUNTED : OK;
}

bool SelectSaveDataParam(const SaveDataParam& param, uint32_t param_type, const void** data, size_t* size)
{
	const auto select_string = [data, size](const char* value, size_t capacity) {
		const void* const terminator = std::memchr(value, '\0', capacity);
		if (terminator == nullptr) return false;
		*data = value;
		*size = static_cast<const char*>(terminator) - value + 1;
		return true;
	};
	switch (param_type)
	{
		case SAVE_DATA_PARAM_ALL:
			*data = &param;
			*size = sizeof(param);
			return true;
		case SAVE_DATA_PARAM_TITLE:
			return select_string(param.title, sizeof(param.title));
		case SAVE_DATA_PARAM_SUB_TITLE:
			return select_string(param.sub_title, sizeof(param.sub_title));
		case SAVE_DATA_PARAM_DETAIL:
			return select_string(param.detail, sizeof(param.detail));
		case SAVE_DATA_PARAM_USER_PARAM:
			*data = &param.user_param;
			*size = sizeof(param.user_param);
			return true;
		case SAVE_DATA_PARAM_MTIME:
			*data = &param.mtime;
			*size = sizeof(param.mtime);
			return true;
		default: return false;
	}
}

int PrepareSaveDataParamSnapshot(const SaveDataMountPoint* mount_point, SaveDataParam* snapshot)
{
	std::string mount_name;
	std::string slot_name;
	const int resolve_result = ResolveSaveDataParamSlot(mount_point, &mount_name, &slot_name);
	if (resolve_result != OK)
	{
		return resolve_result;
	}

	return ReadSaveDataSlotParam(slot_name, snapshot);
}

int CompareSearchEntries(const SearchEntry& left, const SearchEntry& right, uint32_t sort_key)
{
	if (sort_key == SAVE_DATA_SORT_USER_PARAM && left.param.user_param != right.param.user_param)
	{
		return left.param.user_param < right.param.user_param ? -1 : 1;
	}
	if (sort_key == SAVE_DATA_SORT_MTIME && left.mtime != right.mtime)
	{
		return left.mtime < right.mtime ? -1 : 1;
	}

	return CompareNames(left.name, right.name);
}

int SaveDataDirNameSearchImpl(const SaveDataDirNameSearchCondition* condition, SaveDataDirNameSearchResult* result)
{
	if (condition == nullptr || result == nullptr || !IsNaturallyAligned(condition, alignof(SaveDataDirNameSearchCondition)) ||
	    !IsNaturallyAligned(result, alignof(SaveDataDirNameSearchResult)))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	AddressRange condition_range {};
	AddressRange result_range {};
	if (!MakeAddressRange(condition, sizeof(*condition), &condition_range) ||
	    !MakeAddressRange(result, sizeof(*result), &result_range) || RangesOverlap(condition_range, result_range))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	SaveDataDirNameSearchCondition input {};
	SaveDataDirNameSearchResult result_input {};
	if (!ReadSaveDataGuestValue(condition, &input) || !ReadSaveDataGuestValue(result, &result_input))
		return SAVE_DATA_ERROR_PARAMETER;
	const uint32_t sort_key   = input.sort_key;
	const uint32_t sort_order = input.sort_order;
	if ((sort_key != SAVE_DATA_SORT_NAME && sort_key != SAVE_DATA_SORT_USER_PARAM &&
	     sort_key != SAVE_DATA_SORT_BLOCKS && sort_key != SAVE_DATA_SORT_MTIME &&
	     sort_key != SAVE_DATA_SORT_FREE_BLOCKS) ||
	    (sort_order != SAVE_DATA_SORT_ASCENDING && sort_order != SAVE_DATA_SORT_DESCENDING))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	std::string pattern;
	if (!CopyFilter(input.dir_name, &pattern))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	SceSaveDataDirName* const dir_names     = result_input.dir_names;
	const uint32_t            dir_names_num = result_input.dir_names_num;
	SaveDataParam* const      params        = result_input.params;
	SaveDataMountInfo* const  infos         = result_input.infos;

	size_t unused_size = 0;
	if (!MultiplySize(static_cast<size_t>(dir_names_num), sizeof(SceSaveDataDirName), &unused_size) ||
	    !MultiplySize(static_cast<size_t>(dir_names_num), sizeof(SaveDataParam), &unused_size) ||
	    !MultiplySize(static_cast<size_t>(dir_names_num), sizeof(SaveDataMountInfo), &unused_size))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	std::vector<SearchEntry> entries;
	int scan_result = OK;
	{
		std::lock_guard<std::mutex> lock(g_save_data_mount_mutex);
		scan_result = CollectSearchEntries(pattern, &entries);
	}
	if (scan_result != OK)
	{
		return scan_result;
	}
#if LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID
	for (auto& entry: entries)
	{
		entry.param.mtime = entry.mtime;
	}
#endif

	std::sort(entries.begin(), entries.end(), [sort_key, sort_order](const SearchEntry& left, const SearchEntry& right) {
		const int comparison = CompareSearchEntries(left, right, sort_key);
		return sort_order == SAVE_DATA_SORT_ASCENDING ? comparison < 0 : comparison > 0;
	});

	const uint32_t hit_num = static_cast<uint32_t>(entries.size());
	const uint32_t set_num = std::min(hit_num, dir_names_num);

	AddressRange dir_names_range {};
	AddressRange params_range {};
	AddressRange infos_range {};
	const bool   uses_params = set_num != 0 && params != nullptr;
	const bool   uses_infos  = set_num != 0 && infos != nullptr;

	if (set_num != 0)
	{
		if (!MakeArrayRange(dir_names, set_num, &dir_names_range) || RangesOverlap(dir_names_range, condition_range) ||
		    RangesOverlap(dir_names_range, result_range))
		{
			return SAVE_DATA_ERROR_PARAMETER;
		}

		if (uses_params &&
		    (!MakeArrayRange(params, set_num, &params_range) || RangesOverlap(params_range, condition_range) ||
		     RangesOverlap(params_range, result_range) || RangesOverlap(params_range, dir_names_range)))
		{
			return SAVE_DATA_ERROR_PARAMETER;
		}

		if (uses_infos &&
		    (!MakeArrayRange(infos, set_num, &infos_range) || RangesOverlap(infos_range, condition_range) ||
		     RangesOverlap(infos_range, result_range) || RangesOverlap(infos_range, dir_names_range) ||
		     (uses_params && RangesOverlap(infos_range, params_range))))
		{
			return SAVE_DATA_ERROR_PARAMETER;
		}
	}

	std::vector<SceSaveDataDirName> output_names(set_num);
	std::vector<SaveDataParam>      output_params(uses_params ? set_num : 0);
	std::vector<SaveDataMountInfo>  output_infos(uses_infos ? set_num : 0);

	if (set_num != 0)
	{
		std::memset(output_names.data(), 0, output_names.size() * sizeof(output_names[0]));
		if (uses_params)
		{
			std::memset(output_params.data(), 0, output_params.size() * sizeof(output_params[0]));
		}
		if (uses_infos)
		{
			std::memset(output_infos.data(), 0, output_infos.size() * sizeof(output_infos[0]));
		}

		for (uint32_t i = 0; i < set_num; i++)
		{
			std::memcpy(output_names[i].data, entries[i].name.data(), entries[i].name.size());
			if (uses_params)
			{
				output_params[i] = entries[i].param;
			}
			if (uses_infos)
			{
				output_infos[i].blocks      = SAVE_DATA_BLOCKS;
				output_infos[i].free_blocks = SAVE_DATA_BLOCKS;
			}
		}
	}

	SaveDataGuestOutput result_output, names_output, params_output, infos_output;
	if (!result_output.Acquire(result, sizeof(*result)) ||
	    (set_num != 0 && !names_output.Acquire(dir_names, output_names.size() * sizeof(output_names[0]))) ||
	    (uses_params && !params_output.Acquire(params, output_params.size() * sizeof(output_params[0]))) ||
	    (uses_infos && !infos_output.Acquire(infos, output_infos.size() * sizeof(output_infos[0]))))
		return SAVE_DATA_ERROR_PARAMETER;
	if (set_num != 0)
	{
		if (!names_output.Copy(output_names.data(), output_names.size() * sizeof(output_names[0])))
			return SAVE_DATA_ERROR_PARAMETER;
		if (uses_params)
		{
			if (!params_output.Copy(output_params.data(), output_params.size() * sizeof(output_params[0])))
				return SAVE_DATA_ERROR_PARAMETER;
		}
		if (uses_infos)
		{
			if (!infos_output.Copy(output_infos.data(), output_infos.size() * sizeof(output_infos[0])))
				return SAVE_DATA_ERROR_PARAMETER;
		}
	}

	if (!result_output.Copy(&hit_num, sizeof(hit_num), offsetof(SaveDataDirNameSearchResult, hit_num)) ||
	    !result_output.Copy(&set_num, sizeof(set_num), offsetof(SaveDataDirNameSearchResult, set_num)))
		return SAVE_DATA_ERROR_PARAMETER;
	return OK;
}

struct GuestSpan
{
	uint64_t begin = 0;
	uint64_t end = 0;
};

bool MakeGuestSpan(uint64_t address, uint64_t size, bool write, GuestSpan* span)
{
	if (size == 0)
	{
		span->begin = address;
		span->end = address;
		return true;
	}
	if (address == 0 || size > std::numeric_limits<size_t>::max() || address > UINT64_MAX - size ||
	    !Loader::ValidateGuestMemory(address, static_cast<size_t>(size), write)) return false;
	span->begin = address;
	span->end = address + size;
	return true;
}

bool GuestSpansOverlap(const GuestSpan& left, const GuestSpan& right)
{
	return left.begin < right.end && right.begin < left.end;
}

bool GuestAddressAligned(uint64_t address, size_t alignment)
{
	return address != 0 && address % alignment == 0;
}

template <typename T>
bool ReadGuestPrefix(uint64_t address, size_t size, size_t alignment, T* output, GuestSpan* span)
{
	static_assert(std::is_trivially_copyable_v<T>);
	if (size > sizeof(T) || !GuestAddressAligned(address, alignment) ||
	    !MakeGuestSpan(address, size, false, span)) return false;
	std::memset(output, 0, sizeof(*output));
	return Loader::ReadGuestMemory(address, output, size);
}

bool ReadSaveDataMemoryTitle(std::string* title_id)
{
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
	if (!g_save_data_memory_test_title_id.empty())
	{
		*title_id = g_save_data_memory_test_title_id;
		return true;
	}
	#endif
	char value[128] {};
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", value, sizeof(value))) return false;
	const void* terminator = std::memchr(value, '\0', sizeof(value));
	if (terminator == nullptr) return false;
	const size_t length = static_cast<const char*>(terminator) - value;
	if (length == 0 || !IsValidUtf8(std::string(value, length))) return false;
	title_id->assign(value, length);
	return true;
}

SaveDataMemoryKey CurrentSaveDataMemoryKey(int32_t user_id, uint32_t slot_id)
{
	return SaveDataMemoryKey {g_save_data_memory_title_id, user_id, slot_id};
}

uint64_t SaveDataMemoryResidentBytes(const SaveDataMemorySlot* excluded = nullptr)
{
	uint64_t total = 0;
	for (const auto& [key, slot]: g_save_data_memory_slots)
	{
		(void)key;
		if (&slot == excluded) continue;
		if (slot.data.capacity() > SAVE_DATA_MEMORY_RESIDENT_LIMIT - total)
			return SAVE_DATA_MEMORY_RESIDENT_LIMIT + 1;
		total += static_cast<uint64_t>(slot.data.capacity());
	}
	return total;
}

bool AdmitSaveDataMemoryResidentTotal(uint64_t other, uint64_t replacement_size)
{
	return other <= SAVE_DATA_MEMORY_RESIDENT_LIMIT &&
	       replacement_size <= SAVE_DATA_MEMORY_RESIDENT_LIMIT - other;
}

bool AdmitSaveDataMemoryResidentSize(const SaveDataMemorySlot* replaced, uint64_t replacement_size)
{
	return AdmitSaveDataMemoryResidentTotal(
	    SaveDataMemoryResidentBytes(replaced), replacement_size);
}

int64_t CurrentSaveDataMemoryTime()
{
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
	    std::chrono::system_clock::now().time_since_epoch()).count();
	if (seconds > std::numeric_limits<int64_t>::max()) return std::numeric_limits<int64_t>::max();
	if (seconds < std::numeric_limits<int64_t>::min()) return std::numeric_limits<int64_t>::min();
	return static_cast<int64_t>(seconds);
}

SaveDataParam SanitizeSaveDataMemoryParam(const SaveDataParam& input)
{
	SaveDataParam result {};
	std::memcpy(result.title, input.title, sizeof(result.title));
	std::memcpy(result.sub_title, input.sub_title, sizeof(result.sub_title));
	std::memcpy(result.detail, input.detail, sizeof(result.detail));
	result.user_param = input.user_param;
	result.mtime = CurrentSaveDataMemoryTime();
	return result;
}

int ReconcileSaveDataMemorySlot(const SaveDataMemoryKey& key, SaveDataMemorySlot* slot)
{
	if (!slot->storage_indeterminate) return OK;
	SaveDataMemorySlot durable;
	uint64_t existing_size = 0;
	bool store_exists = false;
	const int result = LoadSaveDataMemorySlot(key, slot->capacity, &durable, &existing_size, &store_exists);
	if (result != OK) return result;
	if (store_exists)
	{
		if (!AdmitSaveDataMemoryResidentSize(slot, durable.data.capacity()))
			return SAVE_DATA_ERROR_OUT_OF_MEMORY;
		durable.capacity = slot->capacity;
		*slot = std::move(durable);
	} else
	{
		if (slot->generation == 0)
		{
			// A failed first commit leaves the pre-transaction attachment
			// authoritative in memory. In particular, preserve an initial Param
			// so the guest may retry Set/Sync without a surprising cache mutation.
			slot->storage_indeterminate = false;
		} else
		{
			// A previously durable generation disappeared. Reconcile to that
			// absence instead of retaining a stale generation that can never
			// satisfy the next compare-and-swap.
			const uint64_t capacity = slot->capacity;
			std::fill(slot->data.begin(), slot->data.end(), 0);
			*slot = SaveDataMemorySlot {};
			slot->capacity = capacity;
		}
	}
	return OK;
}

int CommitSaveDataMemorySlot(const SaveDataMemoryKey& key, SaveDataMemorySlot* candidate)
{
	return PersistSaveDataMemorySlot(key, candidate);
}

int SaveDataMemorySetup2Impl(uint64_t setup_address, uint64_t result_address)
{
	std::lock_guard<std::mutex> lock(g_save_data_memory_mutex);
	if (!g_save_data_memory_initialized) return SAVE_DATA_ERROR_NOT_INITIALIZED;

	constexpr size_t SETUP_PREFIX_SIZE = offsetof(SaveDataMemorySetup2Prefix, slot_id) + sizeof(uint32_t);
	SaveDataMemorySetup2Prefix input {};
	GuestSpan setup_span {};
	GuestSpan result_span {};
	if (!ReadGuestPrefix(setup_address, SETUP_PREFIX_SIZE, alignof(uint64_t), &input, &setup_span) ||
	    !GuestAddressAligned(result_address, alignof(uint64_t)) ||
	    !MakeGuestSpan(result_address, sizeof(SaveDataMemorySetupResult), true, &result_span) ||
	    GuestSpansOverlap(setup_span, result_span))
		return SAVE_DATA_ERROR_PARAMETER;
	TraceSaveDataMemory("Setup2", "request", OK, static_cast<uint32_t>(input.user_id), input.slot_id,
	                    input.option, input.memory_size, input.icon_memory_size);
	TraceSaveDataMemory("Setup2", "nested", OK, input.initial_param != 0,
	                    input.initial_icon != 0);
	const SaveDataMemoryKey key = CurrentSaveDataMemoryKey(input.user_id, input.slot_id);
	if (g_save_data_memory_slots.find(key) != g_save_data_memory_slots.end())
		return SAVE_DATA_ERROR_BUSY;
	if ((input.option & ~SAVE_DATA_MEMORY_SETUP_OPTION_MASK) != 0 || input.initial_icon != 0)
		return SAVE_DATA_ERROR_PARAMETER;
	if (input.memory_size > SAVE_DATA_MEMORY_CAPACITY_LIMIT) return SAVE_DATA_ERROR_OUT_OF_MEMORY;
	if (g_save_data_memory_slots.size() >= SAVE_DATA_MEMORY_SLOT_LIMIT)
		return SAVE_DATA_ERROR_OUT_OF_MEMORY;

	SaveDataParam initial_param {};
	GuestSpan initial_param_span {};
	if (input.initial_param != 0)
	{
		if (!GuestAddressAligned(input.initial_param, alignof(SaveDataParam)) ||
		    !MakeGuestSpan(input.initial_param, sizeof(initial_param), false, &initial_param_span) ||
		    GuestSpansOverlap(initial_param_span, result_span) ||
		    !Loader::ReadGuestMemory(input.initial_param, &initial_param, sizeof(initial_param)))
			return SAVE_DATA_ERROR_PARAMETER;
	}

	SaveDataMemorySlot slot;
	uint64_t existing_size = 0;
	bool store_exists = false;
	const int load_result = LoadSaveDataMemorySlot(key, input.memory_size, &slot, &existing_size, &store_exists);
	if (load_result != OK) return load_result;
	if (!AdmitSaveDataMemoryResidentSize(nullptr, slot.data.capacity()))
		return SAVE_DATA_ERROR_OUT_OF_MEMORY;
	if (!store_exists && input.initial_param != 0)
	{
		slot.param = SanitizeSaveDataMemoryParam(initial_param);
		slot.has_param = true;
	}

	auto inserted = g_save_data_memory_slots.emplace(key, std::move(slot)).first;
	SaveDataMemorySetupResult output {};
	output.existing_memory_size = existing_size;
	if (!Loader::WriteGuestMemory(result_address, &output, sizeof(output)))
	{
		g_save_data_memory_slots.erase(inserted);
		return SAVE_DATA_ERROR_PARAMETER;
	}
	return OK;
}

bool PublishSaveDataMemoryGetOutputs(const SaveDataMemorySlot& slot,
	                                 const SaveDataMemoryData& data_request,
	                                 uint64_t data_offset, uint64_t data_size,
	                                 uint64_t param_address,
	                                 const SaveDataParam& param_output)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	Core::VirtualMemory::GuestDataLease data_lease;
	Core::VirtualMemory::GuestDataLease param_lease;
	if (data_size != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        data_request.buffer, data_size, Core::VirtualMemory::Mode::Write, &data_lease))
		return false;
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
	if (const auto hook = g_save_data_memory_publish_test_hook.exchange(nullptr)) hook();
#endif
	if (param_address != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        param_address, sizeof(SaveDataParam), Core::VirtualMemory::Mode::Write, &param_lease))
		return false;
	if (data_size != 0)
	{
		auto* destination = reinterpret_cast<uint8_t*>(data_lease.HostBegin());
		std::memset(destination, 0, static_cast<size_t>(data_size));
		if (data_offset < slot.data.size())
		{
			const size_t available = std::min<size_t>(static_cast<size_t>(data_size),
			    slot.data.size() - static_cast<size_t>(data_offset));
			std::memcpy(destination, slot.data.data() + static_cast<size_t>(data_offset), available);
		}
	}
	if (param_address != 0)
		std::memcpy(reinterpret_cast<void*>(param_lease.HostBegin()), &param_output, sizeof(param_output));
	return true;
#else
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
	if (const auto hook = g_save_data_memory_publish_test_hook.exchange(nullptr)) hook();
#endif
	// Android's active FEX HLE reader pins mapping lifetime through the whole
	// callback. Windows currently has no equivalent Core lease, so this path is
	// prevalidated again before the first publication and remains explicitly
	// unqualified against concurrent Windows VM mutation.
	if ((data_size != 0 && !Loader::ValidateGuestMemory(
	        data_request.buffer, static_cast<size_t>(data_size), true)) ||
	    (param_address != 0 && !Loader::ValidateGuestMemory(
	        param_address, sizeof(SaveDataParam), true))) return false;
	std::vector<uint8_t> data_output(static_cast<size_t>(data_size), 0);
	if (!data_output.empty() && data_offset < slot.data.size())
	{
		const size_t available = std::min<size_t>(data_output.size(),
		    slot.data.size() - static_cast<size_t>(data_offset));
		std::memcpy(data_output.data(), slot.data.data() + static_cast<size_t>(data_offset), available);
	}
	if (!data_output.empty() &&
	    !Loader::WriteGuestMemory(data_request.buffer, data_output.data(), data_output.size())) return false;
	return param_address == 0 ||
	       Loader::WriteGuestMemory(param_address, &param_output, sizeof(param_output));
#endif
}

int SaveDataMemoryGet2Impl(uint64_t request_address)
{
	std::lock_guard<std::mutex> lock(g_save_data_memory_mutex);
	if (!g_save_data_memory_initialized) return SAVE_DATA_ERROR_NOT_INITIALIZED;

	constexpr size_t GET_PREFIX_SIZE = offsetof(SaveDataMemoryGet2Prefix, slot_id) + sizeof(uint32_t);
	SaveDataMemoryGet2Prefix request {};
	GuestSpan request_span {};
	if (!ReadGuestPrefix(request_address, GET_PREFIX_SIZE, alignof(uint64_t), &request, &request_span))
		return SAVE_DATA_ERROR_PARAMETER;
	TraceSaveDataMemory("Get2", "request", OK, static_cast<uint32_t>(request.user_id), request.slot_id,
	                    request.data != 0, request.param != 0, request.icon != 0);
	const SaveDataMemoryKey key = CurrentSaveDataMemoryKey(request.user_id, request.slot_id);
	auto found = g_save_data_memory_slots.find(key);
	if (found == g_save_data_memory_slots.end()) return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	if (request.icon != 0) return SAVE_DATA_ERROR_PARAMETER;
	const int reconcile_result = ReconcileSaveDataMemorySlot(key, &found->second);
	if (reconcile_result != OK) return reconcile_result;

	SaveDataMemoryData data_request {};
	GuestSpan data_descriptor_span {};
	GuestSpan data_output_span {};
	uint64_t data_size = 0;
	uint64_t data_offset = 0;
	if (request.data != 0)
	{
		if (!ReadGuestPrefix(request.data, sizeof(data_request), alignof(uint64_t), &data_request,
		                     &data_descriptor_span) || data_request.offset < 0)
			return SAVE_DATA_ERROR_PARAMETER;
		TraceSaveDataMemory("Get2", "data", OK, data_request.buffer, data_request.size,
		                    static_cast<uint64_t>(data_request.offset));
		data_offset = static_cast<uint64_t>(data_request.offset);
		data_size = data_request.size;
		if ((data_size != 0 && data_request.buffer == 0) || data_size > std::numeric_limits<size_t>::max() ||
		    data_offset > found->second.capacity || data_size > found->second.capacity - data_offset ||
		    !MakeGuestSpan(data_request.buffer, data_size, true, &data_output_span) ||
		    GuestSpansOverlap(data_output_span, request_span) ||
		    GuestSpansOverlap(data_output_span, data_descriptor_span))
			return SAVE_DATA_ERROR_PARAMETER;
	}

	GuestSpan param_output_span {};
	if (request.param != 0)
	{
		if (!GuestAddressAligned(request.param, alignof(SaveDataParam)) ||
		    !MakeGuestSpan(request.param, sizeof(SaveDataParam), true, &param_output_span) ||
		    GuestSpansOverlap(param_output_span, request_span) ||
		    GuestSpansOverlap(param_output_span, data_descriptor_span) ||
		    GuestSpansOverlap(param_output_span, data_output_span))
			return SAVE_DATA_ERROR_PARAMETER;
	}

	SaveDataParam param_output {};
	if (found->second.has_param) param_output = found->second.param;
	if (!PublishSaveDataMemoryGetOutputs(
	        found->second, data_request, data_offset, data_size, request.param, param_output))
		return SAVE_DATA_ERROR_PARAMETER;
	return OK;
}

int SaveDataMemorySet2Impl(uint64_t request_address)
{
	std::lock_guard<std::mutex> lock(g_save_data_memory_mutex);
	if (!g_save_data_memory_initialized) return SAVE_DATA_ERROR_NOT_INITIALIZED;

	constexpr size_t SET_PREFIX_SIZE = offsetof(SaveDataMemorySet2Prefix, slot_id) + sizeof(uint32_t);
	static_assert(SET_PREFIX_SIZE == 40);
	SaveDataMemorySet2Prefix request {};
	GuestSpan request_span {};
	if (!ReadGuestPrefix(request_address, SET_PREFIX_SIZE, alignof(uint64_t), &request, &request_span))
		return SAVE_DATA_ERROR_PARAMETER;
	TraceSaveDataMemory("Set2", "request", OK, static_cast<uint32_t>(request.user_id), request.slot_id,
	                    request.data_num, request.data != 0, (request.param != 0 ? 1ull : 0ull) |
	                    (request.icon != 0 ? 2ull : 0ull));
	const SaveDataMemoryKey key = CurrentSaveDataMemoryKey(request.user_id, request.slot_id);
	auto found = g_save_data_memory_slots.find(key);
	if (found == g_save_data_memory_slots.end()) return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	if (request.icon != 0 || request.data_num > 1) return SAVE_DATA_ERROR_PARAMETER;
	const int reconcile_result = ReconcileSaveDataMemorySlot(key, &found->second);
	if (reconcile_result != OK) return reconcile_result;

	SaveDataMemoryData data_request {};
	GuestSpan data_descriptor_span {};
	uint64_t data_offset = 0;
	if (request.data != 0)
	{
		if (!ReadGuestPrefix(request.data, sizeof(data_request), alignof(uint64_t), &data_request,
		                     &data_descriptor_span) || data_request.offset < 0)
			return SAVE_DATA_ERROR_PARAMETER;
		TraceSaveDataMemory("Set2", "data", OK, data_request.buffer, data_request.size,
		                    static_cast<uint64_t>(data_request.offset));
		data_offset = static_cast<uint64_t>(data_request.offset);
		if ((data_request.size != 0 && data_request.buffer == 0) ||
		    data_request.size > std::numeric_limits<size_t>::max() || data_offset > found->second.capacity ||
		    data_request.size > found->second.capacity - data_offset ||
		    (data_request.size != 0 && !Loader::ValidateGuestMemory(data_request.buffer,
		       static_cast<size_t>(data_request.size), false))) return SAVE_DATA_ERROR_PARAMETER;
	}

	SaveDataParam param_input {};
	if (request.param != 0 &&
	    (!GuestAddressAligned(request.param, alignof(SaveDataParam)) ||
	     !Loader::ValidateGuestMemory(request.param, sizeof(param_input), false) ||
	     !Loader::ReadGuestMemory(request.param, &param_input, sizeof(param_input))))
		return SAVE_DATA_ERROR_PARAMETER;

	size_t replacement_size = found->second.data.size();
	if (request.data != 0)
		replacement_size = std::max(replacement_size,
		                            static_cast<size_t>(data_offset + data_request.size));
	if (!AdmitSaveDataMemoryResidentSize(&found->second, replacement_size))
		return SAVE_DATA_ERROR_OUT_OF_MEMORY;
	SaveDataMemorySlot candidate;
	candidate.capacity = found->second.capacity;
	candidate.generation = found->second.generation;
	candidate.param = found->second.param;
	candidate.has_param = found->second.has_param;
	candidate.data.assign(replacement_size, 0);
	if (!found->second.data.empty())
		std::memcpy(candidate.data.data(), found->second.data.data(), found->second.data.size());
	if (request.data != 0)
	{
		if (data_request.size != 0 &&
		    !Loader::ReadGuestMemory(data_request.buffer,
		                             candidate.data.data() + static_cast<size_t>(data_offset),
		                             static_cast<size_t>(data_request.size)))
			return SAVE_DATA_ERROR_PARAMETER;
	}
	if (request.param != 0)
	{
		candidate.param = SanitizeSaveDataMemoryParam(param_input);
		candidate.has_param = true;
	}
	const int commit_result = CommitSaveDataMemorySlot(key, &candidate);
	if (commit_result != OK)
	{
		found->second.storage_indeterminate = true;
		return commit_result;
	}
	found->second = std::move(candidate);
	return OK;
}

int SaveDataMemorySyncImpl(uint64_t request_address)
{
	std::lock_guard<std::mutex> lock(g_save_data_memory_mutex);
	if (!g_save_data_memory_initialized) return SAVE_DATA_ERROR_NOT_INITIALIZED;
	SaveDataMemorySyncPrefix request {};
	GuestSpan request_span {};
	if (!ReadGuestPrefix(request_address, sizeof(request), alignof(uint32_t), &request, &request_span))
		return SAVE_DATA_ERROR_PARAMETER;
	TraceSaveDataMemory("Sync", "request", OK, static_cast<uint32_t>(request.user_id), request.slot_id,
	                    request.option);
	const SaveDataMemoryKey key = CurrentSaveDataMemoryKey(request.user_id, request.slot_id);
	auto found = g_save_data_memory_slots.find(key);
	if (found == g_save_data_memory_slots.end()) return SAVE_DATA_ERROR_MEMORY_NOT_READY;
	if (request.option != 0) return SAVE_DATA_ERROR_PARAMETER;
	const int reconcile_result = ReconcileSaveDataMemorySlot(key, &found->second);
	if (reconcile_result != OK) return reconcile_result;
	SaveDataMemorySlot candidate = found->second;
	const int commit_result = CommitSaveDataMemorySlot(key, &candidate);
	if (commit_result != OK)
	{
		found->second.storage_indeterminate = true;
		return commit_result;
	}
	found->second = std::move(candidate);
	return OK;
}

} // namespace

#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
    defined(LUMEN_SAVE_DATA_TEST_HOOK)
void SetMetadataCatalogTestHook(void (*hook)())
{
	g_metadata_catalog_test_hook.store(hook);
}
#endif

#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
void SetSaveDataMemoryTitleIdForTest(const char* title_id)
{
	std::lock_guard<std::mutex> lock(g_save_data_memory_mutex);
	g_save_data_memory_test_title_id = title_id != nullptr ? title_id : "";
}

void SetSaveDataMemoryPublishHookForTest(void (*hook)())
{
	g_save_data_memory_publish_test_hook.store(hook);
}

bool SetSaveDataMemoryStorageIndeterminateForTest(int32_t user_id, uint32_t slot_id)
{
	std::lock_guard<std::mutex> lock(g_save_data_memory_mutex);
	const auto found = g_save_data_memory_slots.find(CurrentSaveDataMemoryKey(user_id, slot_id));
	if (found == g_save_data_memory_slots.end()) return false;
	found->second.storage_indeterminate = true;
	return true;
}

bool SaveDataMemoryResidentAdmissionForTest(uint64_t other, uint64_t replacement_size)
{
	return AdmitSaveDataMemoryResidentTotal(other, replacement_size);
}
#endif

void ResetSessionState()
{
	std::lock_guard<std::mutex> lock(g_save_data_memory_mutex);
	for (auto& [key, slot]: g_save_data_memory_slots)
	{
		(void)key;
		std::fill(slot.data.begin(), slot.data.end(), 0);
		std::memset(&slot.param, 0, sizeof(slot.param));
	}
	g_save_data_memory_slots.clear();
	g_save_data_memory_title_id.clear();
	g_save_data_memory_initialized = false;
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
	g_save_data_memory_publish_test_hook.store(nullptr);
#endif
}

int LUMEN_SYSV_ABI SaveDataInitialize(const void* /*init*/)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(init != nullptr);

	return OK;
}

int LUMEN_SYSV_ABI SaveDataInitialize2(const void* /*init*/)
{
	PRINT_NAME();

	// EXIT_NOT_IMPLEMENTED(init != nullptr);

	return OK;
}

int LUMEN_SYSV_ABI SaveDataInitialize3(const void* /*init*/)
{
	PRINT_NAME();
	std::lock_guard<std::mutex> lock(g_save_data_memory_mutex);
	if (g_save_data_memory_initialized)
	{
		TraceSaveDataMemory("Initialize3", "result", OK);
		return OK;
	}
	std::string title_id;
	if (!ReadSaveDataMemoryTitle(&title_id))
	{
		TraceSaveDataMemory("Initialize3", "result", SAVE_DATA_ERROR_INTERNAL);
		return SAVE_DATA_ERROR_INTERNAL;
	}
	g_save_data_memory_title_id = std::move(title_id);
	g_save_data_memory_initialized = true;
	TraceSaveDataMemory("Initialize3", "result", OK);
	return OK;
}

int LUMEN_SYSV_ABI SaveDataSetupSaveDataMemory2(const SaveDataMemorySetup2Prefix* setup,
	                                             SaveDataMemorySetupResult* result)
{
	PRINT_NAME();
	const int call_result = SaveDataMemorySetup2Impl(reinterpret_cast<uint64_t>(setup),
	                                                reinterpret_cast<uint64_t>(result));
	TraceSaveDataMemory("Setup2", "result", call_result, reinterpret_cast<uint64_t>(setup),
	                    reinterpret_cast<uint64_t>(result));
	return call_result;
}

int LUMEN_SYSV_ABI SaveDataGetSaveDataMemory2(const SaveDataMemoryGet2Prefix* request)
{
	PRINT_NAME();
	const int result = SaveDataMemoryGet2Impl(reinterpret_cast<uint64_t>(request));
	TraceSaveDataMemory("Get2", "result", result, reinterpret_cast<uint64_t>(request));
	return result;
}

int LUMEN_SYSV_ABI SaveDataSetSaveDataMemory2(const SaveDataMemorySet2Prefix* request)
{
	PRINT_NAME();
	const int result = SaveDataMemorySet2Impl(reinterpret_cast<uint64_t>(request));
	TraceSaveDataMemory("Set2", "result", result, reinterpret_cast<uint64_t>(request));
	return result;
}

int LUMEN_SYSV_ABI SaveDataSyncSaveDataMemory(const SaveDataMemorySyncPrefix* request)
{
	PRINT_NAME();
	const int result = SaveDataMemorySyncImpl(reinterpret_cast<uint64_t>(request));
	TraceSaveDataMemory("Sync", "result", result, reinterpret_cast<uint64_t>(request));
	return result;
}

int LUMEN_SYSV_ABI SaveDataMount(const SaveDataMount* mount, SaveDataMountResult* mount_result)
{
	PRINT_NAME();

	AddressRange request_range {};
	if (mount == nullptr || !IsNaturallyAligned(mount, alignof(struct SaveDataMount)) ||
	    !MakeAddressRange(mount, sizeof(*mount), &request_range))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	struct SaveDataMount input {};
	if (!ReadSaveDataGuestValue(mount, &input)) return SAVE_DATA_ERROR_PARAMETER;
	return SaveDataMountImpl(mount, sizeof(*mount), alignof(struct SaveDataMount), input.dir_name, input.mount_mode,
	                         mount_result);
}

int LUMEN_SYSV_ABI SaveDataMount2(const SaveDataMount2* mount, SaveDataMountResult* mount_result)
{
	PRINT_NAME();

	AddressRange request_range {};
	if (mount == nullptr || !IsNaturallyAligned(mount, alignof(struct SaveDataMount2)) ||
	    !MakeAddressRange(mount, sizeof(*mount), &request_range))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	struct SaveDataMount2 input {};
	if (!ReadSaveDataGuestValue(mount, &input)) return SAVE_DATA_ERROR_PARAMETER;
	const char* const dir_name = reinterpret_cast<const char*>(input.dir_name);
	return SaveDataMountImpl(mount, sizeof(*mount), alignof(struct SaveDataMount2), dir_name, input.mount_mode,
	                         mount_result);
}

int LUMEN_SYSV_ABI SaveDataUmount(const SaveDataMountPoint* mount_point)
{
	PRINT_NAME();

	SaveDataMountPoint input {};
	std::string mount_name;
	if (!ReadSaveDataGuestValue(mount_point, &input) ||
	    !CopyBoundedString(input.data, sizeof(input.data), &mount_name))
		return SAVE_DATA_ERROR_PARAMETER;
	std::lock_guard<std::mutex> lock(g_save_data_mount_mutex);

	printf("\t mount_point = %s\n", mount_name.c_str());

	LibKernel::FileSystem::Umount(String::FromUtf8(mount_name.c_str()));

	return OK;
}

int LUMEN_SYSV_ABI SaveDataSetParam(const SaveDataMountPoint* mount_point, uint32_t param_type, const void* param_buf, size_t param_buf_size)
{
	PRINT_NAME();

	SaveDataMountPoint mount_input {};
	std::array<uint8_t, sizeof(SaveDataParam)> param_input {};
	if (!ReadSaveDataGuestValue(mount_point, &mount_input) || param_buf_size > param_input.size() ||
	    !ReadSaveDataGuestMemory(reinterpret_cast<uint64_t>(param_buf), param_input.data(), param_buf_size))
		return SAVE_DATA_ERROR_PARAMETER;
	SaveDataParamUpdate update {};
	if (!PrepareSaveDataParamUpdate(param_type, param_input.data(), param_buf_size, &update))
	{
		TraceSaveDataValidation("param-update-rejected", param_type, param_buf_size);
		return SAVE_DATA_ERROR_PARAMETER;
	}
	std::lock_guard<std::mutex> lock(g_save_data_mount_mutex);

	std::string mount_name;
	std::string slot_name;
	const int resolve_result = ResolveSaveDataParamSlot(&mount_input, &mount_name, &slot_name);
	if (resolve_result != OK)
	{
		TraceSaveDataValidation("param-mount-rejected", static_cast<uint32_t>(resolve_result), 0);
		return resolve_result;
	}

	printf("\t mount_point    = %s\n", mount_name.c_str());
	printf("\t param_type     = %u\n", param_type);
	printf("\t param_buf_size = %" PRIu64 "\n", static_cast<uint64_t>(param_buf_size));
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	return WriteSaveDataSlotParam(slot_name, update);
#else
	// Other hosts retain the existing non-persistent local adapter.
	return OK;
#endif
}

int LUMEN_SYSV_ABI SaveDataGetParam(const SaveDataMountPoint* mount_point, uint32_t param_type, void* param_buf,
	                                 size_t param_buf_size, size_t* got_size)
{
	PRINT_NAME();

	SaveDataParam empty_snapshot {};
	const void*   unused_data = nullptr;
	size_t        required_size = 0;
	if (mount_point == nullptr || param_buf == nullptr || got_size == nullptr ||
	    !SelectSaveDataParam(empty_snapshot, param_type, &unused_data, &required_size) ||
	    param_buf_size < required_size)
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	AddressRange mount_range {};
	AddressRange param_range {};
	AddressRange got_size_range {};
	if (!MakeAddressRange(mount_point, sizeof(*mount_point), &mount_range) ||
	    !MakeAddressRange(param_buf, param_buf_size, &param_range) ||
	    !MakeAddressRange(got_size, sizeof(*got_size), &got_size_range) ||
	    RangesOverlap(mount_range, param_range) || RangesOverlap(mount_range, got_size_range) ||
	    RangesOverlap(param_range, got_size_range))
	{
		return SAVE_DATA_ERROR_PARAMETER;
	}

	SaveDataMountPoint mount_input {};
	if (!ReadSaveDataGuestValue(mount_point, &mount_input)) return SAVE_DATA_ERROR_PARAMETER;
	SaveDataParam snapshot {};
	int snapshot_result = OK;
	{
		std::lock_guard<std::mutex> lock(g_save_data_mount_mutex);
		snapshot_result = PrepareSaveDataParamSnapshot(&mount_input, &snapshot);
	}
	if (snapshot_result != OK)
	{
		return snapshot_result;
	}

	const void* data = nullptr;
	size_t      size = 0;
	if (!SelectSaveDataParam(snapshot, param_type, &data, &size))
	{
		return SAVE_DATA_ERROR_INTERNAL;
	}
	if (size > param_buf_size) return SAVE_DATA_ERROR_PARAMETER;

	SaveDataGuestOutput param_output, size_output;
	if (!param_output.Acquire(param_buf, size) || !size_output.Acquire(got_size, sizeof(size)) ||
	    !param_output.Copy(data, size) || !size_output.Copy(&size, sizeof(size)))
		return SAVE_DATA_ERROR_PARAMETER;
	return OK;
}

int LUMEN_SYSV_ABI SaveDataGetMountInfo(const SaveDataMountPoint* mount_point, SaveDataMountInfo* info)
{
	PRINT_NAME();

	if (mount_point == nullptr || info == nullptr) return SAVE_DATA_ERROR_PARAMETER;
	const std::array<uint64_t, 2> values {100000, 100000};
	SaveDataGuestOutput output;
	if (!output.Acquire(info, sizeof(values)) || !output.Copy(values.data(), sizeof(values)))
		return SAVE_DATA_ERROR_PARAMETER;

	return OK;
}

int LUMEN_SYSV_ABI SaveDataDirNameSearch(const SaveDataDirNameSearchCondition* condition,
	                                     SaveDataDirNameSearchResult* result)
{
	PRINT_NAME();
	return SaveDataDirNameSearchImpl(condition, result);
}

int LUMEN_SYSV_ABI SaveDataSaveIcon(const SaveDataMountPoint* mount_point, const SaveDataIcon* icon)
{
	SaveDataIcon input {};
	if (mount_point == nullptr || !ReadSaveDataGuestValue(icon, &input)) return SAVE_DATA_ERROR_PARAMETER;

	printf("\t buf       = %016" PRIx64 "\n", reinterpret_cast<uint64_t>(input.buf));
	printf("\t buf_size  = %" PRIu64 "\n", input.buf_size);
	printf("\t data_size = %" PRIu64 "\n", input.data_size);

	return OK;
}

} // namespace SaveData

LIB_DEFINE(InitSaveData_1)
{
	LIB_FUNC("ZkZhskCPXFw", SaveData::SaveDataInitialize);
	LIB_FUNC("l1NmDeDpNGU", SaveData::SaveDataInitialize2);
	LIB_FUNC_SCALAR("TywrFKCoLGY", SaveData::SaveDataInitialize3);
	LIB_FUNC_SCALAR("oQySEUfgXRA", SaveData::SaveDataSetupSaveDataMemory2);
	LIB_FUNC_SCALAR("QwOO7vegnV8", SaveData::SaveDataGetSaveDataMemory2);
	LIB_FUNC_SCALAR("cduy9v4YmT4", SaveData::SaveDataSetSaveDataMemory2);
	LIB_FUNC_SCALAR("wiT9jeC7xPw", SaveData::SaveDataSyncSaveDataMemory);
	LIB_FUNC("32HQAQdwM2o", SaveData::SaveDataMount);
	LIB_FUNC("0z45PIH+SNI", SaveData::SaveDataMount2);
	LIB_FUNC("BMR4F-Uek3E", SaveData::SaveDataUmount);
	LIB_FUNC("85zul--eGXs", SaveData::SaveDataSetParam);
	LIB_FUNC("XgvSuIdnMlw", SaveData::SaveDataGetParam);
	LIB_FUNC("65VH0Qaaz6s", SaveData::SaveDataGetMountInfo);
	LIB_FUNC("dyIhnXq-0SM", SaveData::SaveDataDirNameSearch);
	LIB_FUNC("c88Yy54Mx0w", SaveData::SaveDataSaveIcon);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
