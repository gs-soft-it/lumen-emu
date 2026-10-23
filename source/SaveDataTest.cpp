#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "cpu/runtime/MemoryHandoff.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include <sys/stat.h>
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>
extern char** environ;
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
namespace Lumen::Libs::SaveData {
void SetMetadataCatalogTestHook(void (*hook)());
}
#endif
#endif
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
namespace Lumen::Libs::SaveData {
void SetSaveDataMemoryTitleIdForTest(const char* title_id);
void SetSaveDataMemoryPublishHookForTest(void (*hook)());
bool SetSaveDataMemoryStorageIndeterminateForTest(int32_t user_id, uint32_t slot_id);
bool SaveDataMemoryResidentAdmissionForTest(uint64_t other, uint64_t replacement_size);
void ResetSessionState();
}
#endif

namespace {

namespace fs = std::filesystem;
namespace SaveDataError = Lumen::Libs::SaveData;

constexpr uint8_t CANARY = 0xa7;
constexpr size_t OUTPUT_CAPACITY = 12;
constexpr int32_t SORT_NAME = 0;
constexpr int32_t SORT_USER_PARAM = 1;
constexpr int32_t SORT_BLOCKS = 2;
constexpr int32_t SORT_MTIME = 3;
constexpr int32_t SORT_FREE_BLOCKS = 5;
constexpr int32_t ORDER_ASCENDING = 0;
constexpr int32_t ORDER_DESCENDING = 1;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
constexpr char PARAM_ATTRIBUTE[] = "com.lumen.savedata.params";
std::string g_test_executable;
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
void* g_memory_publish_protect_page = nullptr;
size_t g_memory_publish_protect_size = 0;
int g_memory_publish_protect_result = -1;

void ProtectMemoryParamForPublishTest()
{
	g_memory_publish_protect_result =
	    mprotect(g_memory_publish_protect_page, g_memory_publish_protect_size, PROT_READ);
}
#endif
#endif

struct DirName
{
	char data[32];
};

struct MountPoint
{
	char data[16];
};

struct MountRequest
{
	int32_t     user_id;
	int32_t     padding;
	const char* title_id;
	const char* dir_name;
	const char* fingerprint;
	uint64_t    blocks;
	uint32_t    mount_mode;
	uint8_t     reserved[32];
};

struct Mount2Request
{
	int32_t        user_id;
	int32_t        padding;
	const DirName* dir_name;
	uint64_t       blocks;
	uint32_t       mount_mode;
	uint8_t        reserved[32];
	int32_t        final_padding;
};

struct MountResult
{
	MountPoint mount_point;
	uint64_t   required_blocks;
	uint32_t   padding;
	uint32_t   mount_status;
	uint8_t    reserved[28];
	int32_t    final_padding;
};

struct TitleId
{
	char data[10];
	uint8_t padding[6];
};

struct SearchCondition
{
	int32_t user_id;
	uint32_t padding;
	const TitleId* title_id;
	const DirName* dir_name;
	int32_t sort_key;
	int32_t sort_order;
	uint8_t reserved[32];
};

struct Param
{
	char title[128];
	char subtitle[128];
	char detail[1024];
	uint32_t user_param;
	uint32_t padding;
	int64_t mtime;
	uint8_t reserved[32];
};

struct Icon
{
	void* buf;
	size_t buf_size;
	size_t data_size;
	uint8_t reserved[32];
};

struct Info
{
	uint64_t blocks;
	uint64_t free_blocks;
	uint8_t reserved[32];
};

struct SearchResult
{
	uint32_t total;
	uint32_t padding;
	DirName* dir_names;
	uint32_t capacity;
	uint32_t written;
	Param* params;
	Info* infos;
	uint8_t reserved[12];
	uint32_t final_padding;
};

struct MemoryData
{
	uint64_t buffer;
	uint64_t size;
	int64_t offset;
	uint8_t reserved[40];
};

struct MemorySetup2
{
	uint32_t option;
	int32_t user_id;
	uint64_t memory_size;
	uint64_t icon_memory_size;
	const Param* initial_param;
	const void* initial_icon;
	uint32_t slot_id;
	uint8_t reserved[20];
};

struct MemorySetupResult
{
	uint64_t existing_size;
	uint8_t reserved[16];
};

struct MemoryGet2
{
	int32_t user_id;
	uint32_t padding;
	MemoryData* data;
	Param* param;
	void* icon;
	uint32_t slot_id;
	uint8_t reserved[28];
};

struct MemorySet2Prefix
{
	int32_t user_id;
	uint32_t padding;
	const MemoryData* data;
	const Param* param;
	const void* icon;
	uint32_t data_num;
	uint32_t slot_id;
};

struct MemorySync
{
	int32_t user_id;
	uint32_t slot_id;
	uint32_t option;
	uint8_t reserved[28];
};

using SearchFunction = int32_t (LUMEN_SYSV_ABI *)(const SearchCondition*, SearchResult*);
using SetParamFunction = int32_t (LUMEN_SYSV_ABI *)(const MountPoint*, uint32_t, const void*, size_t);
using GetParamFunction = int32_t (LUMEN_SYSV_ABI *)(const MountPoint*, uint32_t, void*, size_t, size_t*);
using MountFunction = int32_t (LUMEN_SYSV_ABI *)(const MountRequest*, MountResult*);
using Mount2Function = int32_t (LUMEN_SYSV_ABI *)(const Mount2Request*, MountResult*);
using UmountFunction = int32_t (LUMEN_SYSV_ABI *)(const MountPoint*);
using Initialize3Function = int32_t (LUMEN_SYSV_ABI *)(const void*);
using MemorySetup2Function = int32_t (LUMEN_SYSV_ABI *)(const MemorySetup2*, MemorySetupResult*);
using MemoryGet2Function = int32_t (LUMEN_SYSV_ABI *)(const MemoryGet2*);
using MemorySet2Function = int32_t (LUMEN_SYSV_ABI *)(const MemorySet2Prefix*);
using MemorySyncFunction = int32_t (LUMEN_SYSV_ABI *)(const MemorySync*);

static_assert(sizeof(void*) == 8);
static_assert(sizeof(int32_t) == 4 && std::is_signed_v<int32_t>);
static_assert(sizeof(Icon) == 56 && alignof(Icon) == 8);
static_assert(offsetof(Icon, buf) == 0x00);
static_assert(offsetof(Icon, buf_size) == 0x08);
static_assert(offsetof(Icon, data_size) == 0x10);
static_assert(offsetof(Icon, reserved) == 0x18);
static_assert(sizeof(DirName) == 32 && alignof(DirName) == 1);
static_assert(sizeof(TitleId) == 16 && offsetof(TitleId, padding) == 10);
static_assert(sizeof(SearchCondition) == 64 && alignof(SearchCondition) == 8);
static_assert(offsetof(SearchCondition, user_id) == 0x00);
static_assert(offsetof(SearchCondition, padding) == 0x04);
static_assert(offsetof(SearchCondition, title_id) == 0x08);
static_assert(offsetof(SearchCondition, dir_name) == 0x10);
static_assert(offsetof(SearchCondition, sort_key) == 0x18);
static_assert(offsetof(SearchCondition, sort_order) == 0x1c);
static_assert(offsetof(SearchCondition, reserved) == 0x20);
static_assert(sizeof(SearchResult) == 56 && alignof(SearchResult) == 8);
static_assert(offsetof(SearchResult, total) == 0x00);
static_assert(offsetof(SearchResult, padding) == 0x04);
static_assert(offsetof(SearchResult, dir_names) == 0x08);
static_assert(offsetof(SearchResult, capacity) == 0x10);
static_assert(offsetof(SearchResult, written) == 0x14);
static_assert(offsetof(SearchResult, params) == 0x18);
static_assert(offsetof(SearchResult, infos) == 0x20);
static_assert(offsetof(SearchResult, reserved) == 0x28);
static_assert(offsetof(SearchResult, final_padding) == 0x34);
static_assert(sizeof(Param) == 1328 && alignof(Param) == 8);
static_assert(offsetof(Param, title) == 0x000);
static_assert(offsetof(Param, subtitle) == 0x080);
static_assert(offsetof(Param, detail) == 0x100);
static_assert(offsetof(Param, user_param) == 0x500);
static_assert(offsetof(Param, padding) == 0x504);
static_assert(offsetof(Param, mtime) == 0x508);
static_assert(offsetof(Param, reserved) == 0x510);
static_assert(sizeof(Info) == 48 && alignof(Info) == 8);
static_assert(offsetof(Info, blocks) == 0);
static_assert(offsetof(Info, free_blocks) == 8);
static_assert(offsetof(Info, reserved) == 16);
static_assert(std::is_standard_layout_v<SearchCondition> && std::is_trivially_copyable_v<SearchCondition>);
static_assert(std::is_standard_layout_v<SearchResult> && std::is_trivially_copyable_v<SearchResult>);
static_assert(sizeof(MountPoint) == 16 && alignof(MountPoint) == 1);
static_assert(sizeof(MountRequest) == 80 && alignof(MountRequest) == 8);
static_assert(offsetof(MountRequest, user_id) == 0x00);
static_assert(offsetof(MountRequest, padding) == 0x04);
static_assert(offsetof(MountRequest, title_id) == 0x08);
static_assert(offsetof(MountRequest, dir_name) == 0x10);
static_assert(offsetof(MountRequest, fingerprint) == 0x18);
static_assert(offsetof(MountRequest, blocks) == 0x20);
static_assert(offsetof(MountRequest, mount_mode) == 0x28);
static_assert(offsetof(MountRequest, reserved) == 0x2c);
static_assert(sizeof(Mount2Request) == 64 && alignof(Mount2Request) == 8);
static_assert(offsetof(Mount2Request, user_id) == 0x00);
static_assert(offsetof(Mount2Request, padding) == 0x04);
static_assert(offsetof(Mount2Request, dir_name) == 0x08);
static_assert(offsetof(Mount2Request, blocks) == 0x10);
static_assert(offsetof(Mount2Request, mount_mode) == 0x18);
static_assert(offsetof(Mount2Request, reserved) == 0x1c);
static_assert(offsetof(Mount2Request, final_padding) == 0x3c);
static_assert(sizeof(MountResult) == 64 && alignof(MountResult) == 8);
static_assert(offsetof(MountResult, mount_point) == 0x00);
static_assert(offsetof(MountResult, required_blocks) == 0x10);
static_assert(offsetof(MountResult, padding) == 0x18);
static_assert(offsetof(MountResult, mount_status) == 0x1c);
static_assert(offsetof(MountResult, reserved) == 0x20);
static_assert(offsetof(MountResult, final_padding) == 0x3c);
static_assert(std::is_standard_layout_v<MountRequest> && std::is_trivially_copyable_v<MountRequest>);
static_assert(std::is_standard_layout_v<Mount2Request> && std::is_trivially_copyable_v<Mount2Request>);
static_assert(std::is_standard_layout_v<MountResult> && std::is_trivially_copyable_v<MountResult>);
static_assert(sizeof(MemoryData) == 64 && alignof(MemoryData) == 8);
static_assert(offsetof(MemoryData, buffer) == 0 && offsetof(MemoryData, size) == 8 &&
              offsetof(MemoryData, offset) == 16);
static_assert(sizeof(MemorySetup2) == 64 && offsetof(MemorySetup2, memory_size) == 8 &&
              offsetof(MemorySetup2, initial_param) == 24 && offsetof(MemorySetup2, initial_icon) == 32 &&
              offsetof(MemorySetup2, slot_id) == 40);
static_assert(sizeof(MemorySetupResult) == 24 && offsetof(MemorySetupResult, existing_size) == 0);
static_assert(sizeof(MemoryGet2) == 64 && offsetof(MemoryGet2, data) == 8 &&
              offsetof(MemoryGet2, param) == 16 && offsetof(MemoryGet2, icon) == 24 &&
              offsetof(MemoryGet2, slot_id) == 32);
static_assert(sizeof(MemorySet2Prefix) == 40 && offsetof(MemorySet2Prefix, data_num) == 32 &&
              offsetof(MemorySet2Prefix, slot_id) == 36);
static_assert(sizeof(MemorySync) == 40 && offsetof(MemorySync, option) == 8);

SearchFunction g_search = nullptr;
SetParamFunction g_set_param = nullptr;
GetParamFunction g_get_param = nullptr;
MountFunction g_mount = nullptr;
Mount2Function g_mount2 = nullptr;
UmountFunction g_umount = nullptr;
using MountInfoFunction = int32_t (LUMEN_SYSV_ABI *)(const MountPoint*, Info*);
MountInfoFunction g_mount_info = nullptr;
Initialize3Function g_initialize3 = nullptr;
MemorySetup2Function g_memory_setup2 = nullptr;
MemoryGet2Function g_memory_get2 = nullptr;
MemorySet2Function g_memory_set2 = nullptr;
MemorySyncFunction g_memory_sync = nullptr;

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "SaveDataTest: %s\n", message);
	return condition;
}

bool CaseCheck(bool condition, const char* name, const char* detail)
{
	if (!condition) std::fprintf(stderr, "SaveDataTest: %s: %s\n", name, detail);
	return condition;
}

template <typename T>
void Fill(T& value, uint8_t byte = CANARY)
{
	static_assert(std::is_trivially_copyable_v<T>);
	std::memset(static_cast<void*>(&value), byte, sizeof(value));
}

template <typename T>
bool SameBytes(const T& left, const T& right)
{
	static_assert(std::is_trivially_copyable_v<T>);
	return std::memcmp(static_cast<const void*>(&left), static_cast<const void*>(&right), sizeof(left)) == 0;
}

Lumen::Loader::SymbolResolve Exact(const char* nid)
{
	Lumen::Loader::SymbolResolve query {};
	query.name = nid;
	query.library = "libSceSaveData";
	query.library_version = 1;
	query.module = "libSceSaveData";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Lumen::Loader::SymbolType::Func;
	return query;
}

bool TestNegativeIdentity(const Lumen::Loader::SymbolDatabase& symbols)
{
	for (const char* nid : {"dyIhnXq-0SM", "XgvSuIdnMlw", "TywrFKCoLGY", "oQySEUfgXRA",
	                        "QwOO7vegnV8", "cduy9v4YmT4", "wiT9jeC7xPw"})
	{
		const auto exact = Exact(nid);
		for (int version : {0, 2})
		{
			auto wrong = exact;
			wrong.library_version = version;
			if (!Check(symbols.Find(wrong) == nullptr, "wrong SaveData library version resolved")) return false;
			wrong = exact;
			wrong.module_version_major = version;
			if (!Check(symbols.Find(wrong) == nullptr, "wrong SaveData module major resolved")) return false;
			wrong = exact;
			wrong.module_version_minor = version;
			if (!Check(symbols.Find(wrong) == nullptr, "wrong SaveData module minor resolved")) return false;
		}
		for (const char* other : {"libSceSaveDataForToolkit", "libSceSaveDataDialog", "unrelated_save_data_library"})
		{
			auto wrong = exact;
			wrong.library = other;
			if (!Check(symbols.Find(wrong) == nullptr, "wrong SaveData library namespace resolved")) return false;
			wrong = exact;
			wrong.module = other;
			if (!Check(symbols.Find(wrong) == nullptr, "wrong SaveData module namespace resolved")) return false;
		}
		for (auto type : {Lumen::Loader::SymbolType::Unknown, Lumen::Loader::SymbolType::Object,
		                 Lumen::Loader::SymbolType::TlsModule, Lumen::Loader::SymbolType::NoType})
		{
			auto wrong = exact;
			wrong.type = type;
			if (!Check(symbols.Find(wrong) == nullptr, "wrong SaveData symbol type resolved")) return false;
		}
	}
	for (const char* alias : {"sceSaveDataDirNameSearch", "SaveDataDirNameSearch", "sceSaveDataGetParam",
	                         "SaveDataGetParam", "sceSaveDataInitialize3", "SaveDataInitialize3",
	                         "sceSaveDataSetupSaveDataMemory2", "SaveDataSetupSaveDataMemory2",
	                         "sceSaveDataGetSaveDataMemory2", "SaveDataGetSaveDataMemory2",
	                         "sceSaveDataSetSaveDataMemory2", "SaveDataSetSaveDataMemory2",
	                         "sceSaveDataSyncSaveDataMemory", "SaveDataSyncSaveDataMemory",
	                         "unknown-save-data-nid"})
		if (!Check(symbols.Find(Exact(alias)) == nullptr, "textual or unknown SaveData alias resolved")) return false;
	return Check(symbols.StdNameMapSize() == 0 &&
	             symbols.FindByStdName(U"dyIhnXq-0SM") == nullptr &&
	             symbols.FindByStdName(U"XgvSuIdnMlw") == nullptr &&
	             symbols.FindByStdName(U"QwOO7vegnV8") == nullptr &&
	             symbols.FindByStdName(U"sceSaveDataDirNameSearch") == nullptr &&
	             symbols.FindByStdName(U"SaveDataDirNameSearch") == nullptr &&
	             symbols.FindByStdName(U"sceSaveDataGetParam") == nullptr &&
	             symbols.FindByStdName(U"SaveDataGetParam") == nullptr &&
	             symbols.FindByStdName(U"sceSaveDataGetSaveDataMemory2") == nullptr &&
	             symbols.FindByStdName(U"SaveDataGetSaveDataMemory2") == nullptr,
	             "SaveData registration gained a standard-name fallback");
}

bool TestBindings()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libSaveData_1", &symbols), "SaveData registration failed")) return false;
	const auto* search = symbols.Find(Exact("dyIhnXq-0SM"));
	if (!Check(search != nullptr && search->vaddr != 0 &&
	           search->dbg_name == U"SaveData::SaveDataDirNameSearch",
	           "directory search identity or destination mismatch")) return false;
	g_search = reinterpret_cast<SearchFunction>(search->vaddr);
	const auto* set_param = symbols.Find(Exact("85zul--eGXs"));
	if (!Check(set_param != nullptr && set_param->vaddr != 0 &&
	           set_param->dbg_name == U"SaveData::SaveDataSetParam",
	           "set-parameter identity or destination mismatch")) return false;
	g_set_param = reinterpret_cast<SetParamFunction>(set_param->vaddr);
	const auto* get_param = symbols.Find(Exact("XgvSuIdnMlw"));
	if (!Check(get_param != nullptr && get_param->vaddr != 0 &&
	           get_param->dbg_name == U"SaveData::SaveDataGetParam",
	           "get-parameter identity or destination mismatch")) return false;
	g_get_param = reinterpret_cast<GetParamFunction>(get_param->vaddr);
	const auto* mount = symbols.Find(Exact("32HQAQdwM2o"));
	if (!Check(mount != nullptr && mount->vaddr != 0 &&
	           mount->dbg_name == U"SaveData::SaveDataMount",
	           "mount identity or destination mismatch")) return false;
	g_mount = reinterpret_cast<MountFunction>(mount->vaddr);
	const auto* mount2 = symbols.Find(Exact("0z45PIH+SNI"));
	if (!Check(mount2 != nullptr && mount2->vaddr != 0 &&
	           mount2->dbg_name == U"SaveData::SaveDataMount2",
	           "mount2 identity or destination mismatch")) return false;
	g_mount2 = reinterpret_cast<Mount2Function>(mount2->vaddr);
	const auto* umount = symbols.Find(Exact("BMR4F-Uek3E"));
	if (!Check(umount != nullptr && umount->vaddr != 0 &&
	           umount->dbg_name == U"SaveData::SaveDataUmount",
	           "unmount identity or destination mismatch")) return false;
	g_umount = reinterpret_cast<UmountFunction>(umount->vaddr);
	const auto* mount_info = symbols.Find(Exact("65VH0Qaaz6s"));
	if (!Check(mount_info != nullptr && mount_info->vaddr != 0, "mount-info binding missing")) return false;
	g_mount_info = reinterpret_cast<MountInfoFunction>(mount_info->vaddr);

	struct MemoryBinding
	{
		const char* nid;
		const char32_t* destination;
	};
	MemoryBinding memory_bindings[] = {
	    {"TywrFKCoLGY", U"SaveData::SaveDataInitialize3"},
	    {"oQySEUfgXRA", U"SaveData::SaveDataSetupSaveDataMemory2"},
	    {"QwOO7vegnV8", U"SaveData::SaveDataGetSaveDataMemory2"},
	    {"cduy9v4YmT4", U"SaveData::SaveDataSetSaveDataMemory2"},
	    {"wiT9jeC7xPw", U"SaveData::SaveDataSyncSaveDataMemory"},
	};
	uint64_t memory_addresses[std::size(memory_bindings)] {};
	for (size_t i = 0; i < std::size(memory_bindings); ++i)
	{
		const auto* found = symbols.Find(Exact(memory_bindings[i].nid));
		if (!Check(found != nullptr && found->vaddr != 0 && found->dbg_name == memory_bindings[i].destination &&
		           symbols.HostCallScalarMetadataFor(found->vaddr) != nullptr,
		           "SaveDataMemory identity, destination, or scalar ABI mismatch")) return false;
		memory_addresses[i] = found->vaddr;
	}
	g_initialize3 = reinterpret_cast<Initialize3Function>(memory_addresses[0]);
	g_memory_setup2 = reinterpret_cast<MemorySetup2Function>(memory_addresses[1]);
	g_memory_get2 = reinterpret_cast<MemoryGet2Function>(memory_addresses[2]);
	g_memory_set2 = reinterpret_cast<MemorySet2Function>(memory_addresses[3]);
	g_memory_sync = reinterpret_cast<MemorySyncFunction>(memory_addresses[4]);

	struct BaselineBinding
	{
		const char* nid;
		const char32_t* destination;
	};
	constexpr BaselineBinding baseline[] = {
	    {"ZkZhskCPXFw", U"SaveData::SaveDataInitialize"},
	    {"l1NmDeDpNGU", U"SaveData::SaveDataInitialize2"},
	    {"32HQAQdwM2o", U"SaveData::SaveDataMount"},
	    {"0z45PIH+SNI", U"SaveData::SaveDataMount2"},
	    {"BMR4F-Uek3E", U"SaveData::SaveDataUmount"},
	    {"85zul--eGXs", U"SaveData::SaveDataSetParam"},
	    {"65VH0Qaaz6s", U"SaveData::SaveDataGetMountInfo"},
	    {"c88Yy54Mx0w", U"SaveData::SaveDataSaveIcon"},
	};
	std::array<uint64_t, 15> targets {};
	targets[0] = search->vaddr;
	targets[1] = get_param->vaddr;
	for (size_t i = 0; i < std::size(memory_addresses); ++i) targets[i + 2] = memory_addresses[i];
	for (size_t i = 0; i < std::size(baseline); ++i)
	{
		const auto* found = symbols.Find(Exact(baseline[i].nid));
		if (!Check(found != nullptr && found->vaddr != 0 && found->dbg_name == baseline[i].destination,
		           "baseline SaveData binding changed")) return false;
		targets[i + 2 + std::size(memory_addresses)] = found->vaddr;
	}
	std::sort(targets.begin(), targets.end());
	if (!Check(std::adjacent_find(targets.begin(), targets.end()) == targets.end(),
	           "SaveData exports share an unexpected destination")) return false;
	return Check(symbols.HostCallTargetCount() == 15, "unexpected SaveData host-call target count") &&
	       TestNegativeIdentity(symbols);
}

template <typename T, size_t N>
struct GuardedRecords
{
	std::array<uint8_t, 16> before;
	std::array<T, N> records;
	std::array<uint8_t, 16> after;
};

struct OutputState
{
	SearchResult result;
	GuardedRecords<DirName, OUTPUT_CAPACITY> names;
	GuardedRecords<Param, OUTPUT_CAPACITY> params;
	GuardedRecords<Info, OUTPUT_CAPACITY> infos;

	void Prepare(uint32_t capacity, bool with_names = true, bool with_params = true, bool with_infos = true)
	{
		Fill(*this);
		result.dir_names = with_names ? names.records.data() : nullptr;
		result.capacity = capacity;
		result.params = with_params ? params.records.data() : nullptr;
		result.infos = with_infos ? infos.records.data() : nullptr;
	}
};

static_assert(std::is_trivially_copyable_v<OutputState>);

DirName MakeName(std::string_view text)
{
	DirName value {};
	if (text.size() < sizeof(value.data)) std::memcpy(value.data, text.data(), text.size());
	return value;
}

SearchCondition MakeCondition(const DirName* filter = nullptr, int32_t key = SORT_NAME,
	                          int32_t order = ORDER_ASCENDING)
{
	SearchCondition condition {};
	condition.user_id = -17;
	condition.padding = 0x5a5a5a5a;
	condition.dir_name = filter;
	condition.sort_key = key;
	condition.sort_order = order;
	std::memset(condition.reserved, 0x5d, sizeof(condition.reserved));
	return condition;
}

struct Entry
{
	std::string name;
	int64_t mtime;
};

int CompareBytes(std::string_view left, std::string_view right, bool fold_ascii)
{
	const size_t common = std::min(left.size(), right.size());
	for (size_t i = 0; i < common; ++i)
	{
		auto l = static_cast<uint8_t>(left[i]);
		auto r = static_cast<uint8_t>(right[i]);
		if (fold_ascii)
		{
			if (l >= 'A' && l <= 'Z') l = static_cast<uint8_t>(l + ('a' - 'A'));
			if (r >= 'A' && r <= 'Z') r = static_cast<uint8_t>(r + ('a' - 'A'));
		}
		if (l != r) return l < r ? -1 : 1;
	}
	if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
	return 0;
}

int CompareNames(std::string_view left, std::string_view right)
{
	const int folded = CompareBytes(left, right, true);
	return folded != 0 ? folded : CompareBytes(left, right, false);
}

std::vector<Entry> Ordered(std::vector<Entry> entries, int32_t key, int32_t order)
{
	std::sort(entries.begin(), entries.end(), [key, order](const Entry& left, const Entry& right) {
		int comparison = 0;
		if (key == SORT_MTIME && left.mtime != right.mtime) comparison = left.mtime < right.mtime ? -1 : 1;
		else comparison = CompareNames(left.name, right.name);
		return order == ORDER_ASCENDING ? comparison < 0 : comparison > 0;
	});
	return entries;
}

bool InvokeSuccess(const SearchCondition& condition, const std::vector<Entry>& ordered, uint32_t capacity,
	               const char* name, bool with_names = true, bool with_params = true, bool with_infos = true)
{
	if (!CaseCheck(capacity <= OUTPUT_CAPACITY, name, "test capacity exceeds fixture")) return false;
	OutputState actual;
	actual.Prepare(capacity, with_names, with_params, with_infos);
	OutputState expected = actual;
	const SearchCondition condition_before = condition;
	const uint32_t written = std::min<uint32_t>(static_cast<uint32_t>(ordered.size()), capacity);
	expected.result.total = static_cast<uint32_t>(ordered.size());
	expected.result.written = written;
	for (uint32_t i = 0; i < written; ++i)
	{
		if (with_names)
		{
			std::memset(&expected.names.records[i], 0, sizeof(DirName));
			std::memcpy(expected.names.records[i].data, ordered[i].name.data(), ordered[i].name.size());
		}
		if (with_params)
		{
			std::memset(&expected.params.records[i], 0, sizeof(Param));
			expected.params.records[i].mtime = ordered[i].mtime;
		}
		if (with_infos)
		{
			std::memset(&expected.infos.records[i], 0, sizeof(Info));
			expected.infos.records[i].blocks = 100000;
			expected.infos.records[i].free_blocks = 100000;
		}
	}
	const int32_t result = g_search(&condition, &actual.result);
	return CaseCheck(result == 0, name, "success call returned an error") &&
	       CaseCheck(SameBytes(condition, condition_before), name, "condition was modified") &&
	       CaseCheck(SameBytes(actual, expected), name, "result, record, reserved, or canary bytes differ");
}

bool ExpectError(const SearchCondition& condition, OutputState& output, int32_t error, const char* name)
{
	const SearchCondition condition_before = condition;
	const OutputState output_before = output;
	const int32_t result = g_search(&condition, &output.result);
	return CaseCheck(result == error, name, "wrong error code") &&
	       CaseCheck(SameBytes(condition, condition_before), name, "condition was modified on error") &&
	       CaseCheck(SameBytes(output, output_before), name, "output changed on error");
}

class Workspace
{
public:
	bool Init(const fs::path& parent)
	{
		std::error_code error;
		m_original = fs::current_path(error);
		if (error) return Check(false, "cannot read initial working directory");
		fs::create_directories(parent, error);
		if (error) return Check(false, "cannot create build-owned fixture parent");
		const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
		for (unsigned attempt = 0; attempt < 1024; ++attempt)
		{
			m_path = parent / ("run-" + std::to_string(seed) + "-" + std::to_string(attempt));
			if (fs::create_directory(m_path, error)) break;
			if (error) return Check(false, "cannot create isolated fixture directory");
			m_path.clear();
		}
		if (m_path.empty()) return Check(false, "cannot reserve a unique fixture directory");
		fs::current_path(m_path, error);
		if (error) return Check(false, "cannot enter isolated fixture directory");
		m_ready = true;
		return true;
	}

	~Workspace()
	{
		if (!m_ready) return;
		std::error_code ignored;
		fs::current_path(m_original, ignored);
		fs::remove_all(m_path, ignored);
	}

private:
	fs::path m_original;
	fs::path m_path;
	bool m_ready = false;
};

fs::path Utf8Path(std::string_view value)
{
	return fs::u8path(value.begin(), value.end());
}

bool RemoveRoot()
{
	const fs::path root = "_SaveData";
	std::error_code error;
	const auto status = fs::symlink_status(root, error);
	if (error == std::errc::no_such_file_or_directory) return true;
	if (error) return Check(false, "cannot inspect isolated SaveData root");
	if (fs::is_symlink(status)) fs::remove(root, error);
	else fs::remove_all(root, error);
	return Check(!error, "cannot reset isolated SaveData root");
}

int64_t UnixMtime(const fs::path& path, bool& ok)
{
#if defined(_WIN32)
	struct _stat64 status {};
	ok = _wstat64(path.c_str(), &status) == 0;
	return ok ? static_cast<int64_t>(status.st_mtime) : 0;
#else
	struct stat status {};
	ok = ::stat(path.c_str(), &status) == 0;
	if (!ok) return 0;
#if defined(__APPLE__)
	const auto seconds = status.st_mtimespec.tv_sec;
	const auto nanoseconds = status.st_mtimespec.tv_nsec;
#else
	const auto seconds = status.st_mtim.tv_sec;
	const auto nanoseconds = status.st_mtim.tv_nsec;
#endif
	return static_cast<int64_t>(seconds) + (seconds < 0 && nanoseconds != 0 ? 1 : 0);
#endif
}

bool TestRootShapes()
{
	if (!RemoveRoot()) return false;
	auto condition = MakeCondition();
	if (!InvokeSuccess(condition, {}, 3, "absent root")) return false;
	if (!Check(!fs::exists("_SaveData"), "absent-root search created the root")) return false;
	std::error_code error;
	fs::create_directory("_SaveData", error);
	if (error || !InvokeSuccess(condition, {}, 3, "empty root")) return false;
	if (!RemoveRoot()) return false;
	{
		std::ofstream root("_SaveData", std::ios::binary);
		root << "fixture";
	}
	OutputState output;
	output.Prepare(2);
	if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_INTERNAL, "non-directory root")) return false;
	if (!RemoveRoot()) return false;
	fs::create_directory("root-link-target", error);
	if (error) return Check(false, "cannot create root-link target");
	fs::create_directory_symlink(fs::absolute("root-link-target"), "_SaveData", error);
	if (error) return Check(false, "cannot create required root symlink fixture");
	output.Prepare(2);
	if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_INTERNAL, "symlink root")) return false;
	return RemoveRoot();
}

struct Catalog
{
	std::vector<Entry> entries;
	std::vector<std::pair<fs::path, fs::file_time_type>> timestamps;
	std::string limit_name;
	std::string root_file_contents;
};

bool CreateCatalog(Catalog& catalog)
{
	if (!RemoveRoot()) return false;
	std::error_code error;
	fs::create_directory("_SaveData", error);
	if (error) return Check(false, "cannot create catalog root");
	catalog.limit_name.assign(31, 'z');
	const std::vector<std::string> required = {
	    "Alpha", "ALpHa2", "aXc", "beta", "gamma9", "Q", std::string("u-") + "\xc3\xa9", catalog.limit_name};
	for (const auto& name : required)
	{
		fs::create_directory(fs::path("_SaveData") / Utf8Path(name), error);
		if (error) return Check(false, "cannot create a catalog directory");
		catalog.entries.push_back({name, 0});
	}
	// Same-folded names are possible only on a case-sensitive fixture volume.
	const bool lower_alpha_created = fs::create_directory(fs::path("_SaveData") / "alpha", error);
	if (error) return Check(false, "cannot probe case-sensitive fixture behavior");
	if (lower_alpha_created) catalog.entries.push_back({"alpha", 0});
	fs::create_directory(fs::path("_SaveData") / std::string(32, 'x'), error);
	if (error) return Check(false, "cannot create over-limit directory fixture");
	fs::create_directory(fs::path("_SaveData") / "sce_hidden", error);
	if (error) return Check(false, "cannot create internal-namespace fixture");
	fs::create_directories(fs::path("_SaveData") / "Alpha" / "nested-only", error);
	if (error) return Check(false, "cannot create nested directory fixture");
	{
		std::ofstream file(fs::path("_SaveData") / "ordinary-file", std::ios::binary);
		file << "catalog-file";
	}
	{
		std::ofstream file(fs::path("_SaveData") / "Alpha" / "inside.bin", std::ios::binary);
		file << "inside-directory";
	}
	fs::create_directory("entry-link-target", error);
	if (error) return Check(false, "cannot create entry-link target");
	fs::create_directory_symlink(fs::absolute("entry-link-target"), fs::path("_SaveData") / "linked-directory", error);
	if (error) return Check(false, "cannot create required entry symlink fixture");
	fs::create_directory_symlink(fs::absolute("missing-link-target"), fs::path("_SaveData") / "broken-directory-link", error);
	if (error) return Check(false, "cannot create required broken symlink fixture");

	const auto base = fs::file_time_type::clock::now() - std::chrono::hours(200);
	for (size_t i = 0; i < catalog.entries.size(); ++i)
	{
		const auto stamp = base + std::chrono::hours(static_cast<int64_t>(i) * 3);
		fs::last_write_time(fs::path("_SaveData") / Utf8Path(catalog.entries[i].name), stamp, error);
		if (error) return Check(false, "cannot set catalog directory mtime");
	}
	if (lower_alpha_created)
	{
		const auto alpha = std::find_if(catalog.entries.begin(), catalog.entries.end(), [](const Entry& entry) {
			return entry.name == "Alpha";
		});
		const auto lower = std::find_if(catalog.entries.begin(), catalog.entries.end(), [](const Entry& entry) {
			return entry.name == "alpha";
		});
		fs::last_write_time(fs::path("_SaveData") / "alpha",
		                    fs::last_write_time(fs::path("_SaveData") / "Alpha", error), error);
		if (error || alpha == catalog.entries.end() || lower == catalog.entries.end())
			return Check(false, "cannot create equal-mtime name tie");
	}
	for (auto& entry : catalog.entries)
	{
		bool ok = false;
		entry.mtime = UnixMtime(fs::path("_SaveData") / Utf8Path(entry.name), ok);
		if (!ok) return Check(false, "cannot read catalog Unix mtime");
	}
	for (const auto& path : {fs::path("_SaveData"), fs::path("_SaveData") / "ordinary-file",
	                         fs::path("_SaveData") / "Alpha", fs::path("_SaveData") / "Alpha" / "inside.bin"})
	{
		catalog.timestamps.emplace_back(path, fs::last_write_time(path, error));
		if (error) return Check(false, "cannot snapshot read-only fixture timestamp");
	}
	catalog.root_file_contents = "catalog-file";
	return true;
}

std::vector<Entry> Select(const Catalog& catalog, std::initializer_list<std::string_view> names)
{
	std::vector<Entry> selected;
	for (const auto& entry : catalog.entries)
		if (std::find(names.begin(), names.end(), std::string_view(entry.name)) != names.end()) selected.push_back(entry);
	return selected;
}

bool RunFilter(const Catalog& catalog, std::string_view pattern, std::vector<Entry> selected, const char* name)
{
	const DirName filter = MakeName(pattern);
	const auto condition = MakeCondition(&filter);
	return InvokeSuccess(condition, Ordered(std::move(selected), SORT_NAME, ORDER_ASCENDING), OUTPUT_CAPACITY, name);
}

bool TestFiltersAndOrdering(const Catalog& catalog)
{
	const auto all_ascending = Ordered(catalog.entries, SORT_NAME, ORDER_ASCENDING);
	if (!InvokeSuccess(MakeCondition(), all_ascending, OUTPUT_CAPACITY, "null filter")) return false;
	const DirName empty {};
	if (!InvokeSuccess(MakeCondition(&empty), all_ascending, OUTPUT_CAPACITY, "empty filter")) return false;
	if (!RunFilter(catalog, "%", catalog.entries, "percent wildcard")) return false;
	if (!RunFilter(catalog, "_", Select(catalog, {"Q"}), "single-byte wildcard")) return false;
	if (!RunFilter(catalog, "A_C", Select(catalog, {"aXc"}), "ASCII-folded underscore wildcard")) return false;
	if (!RunFilter(catalog, "%9", Select(catalog, {"gamma9"}), "suffix wildcard")) return false;
	if (!RunFilter(catalog, "u-__", Select(catalog, {std::string_view("u-\xc3\xa9")}), "UTF-8 byte wildcard")) return false;
	if (!RunFilter(catalog, "u-_", {}, "UTF-8 is not one byte")) return false;
	if (!RunFilter(catalog, "aLpHa2", Select(catalog, {"ALpHa2"}), "ASCII case folding")) return false;
	if (!RunFilter(catalog, catalog.limit_name, Select(catalog, {catalog.limit_name}), "31-byte name and filter")) return false;

	for (int32_t key : {SORT_NAME, SORT_USER_PARAM, SORT_BLOCKS, SORT_FREE_BLOCKS})
	{
		if (!InvokeSuccess(MakeCondition(nullptr, key, ORDER_ASCENDING),
		                   Ordered(catalog.entries, key, ORDER_ASCENDING), OUTPUT_CAPACITY,
		                   "ascending deterministic order")) return false;
		if (!InvokeSuccess(MakeCondition(nullptr, key, ORDER_DESCENDING),
		                   Ordered(catalog.entries, key, ORDER_DESCENDING), OUTPUT_CAPACITY,
		                   "descending deterministic order")) return false;
	}
	if (!InvokeSuccess(MakeCondition(nullptr, SORT_MTIME, ORDER_ASCENDING),
	                   Ordered(catalog.entries, SORT_MTIME, ORDER_ASCENDING), OUTPUT_CAPACITY,
	                   "ascending mtime order")) return false;
	if (!InvokeSuccess(MakeCondition(nullptr, SORT_MTIME, ORDER_DESCENDING),
	                   Ordered(catalog.entries, SORT_MTIME, ORDER_DESCENDING), OUTPUT_CAPACITY,
	                   "descending mtime order")) return false;
	return true;
}

bool TestCapacityAndOutputs(const Catalog& catalog)
{
	const auto ordered = Ordered(catalog.entries, SORT_NAME, ORDER_ASCENDING);
	if (!InvokeSuccess(MakeCondition(), ordered, 0, "zero capacity", false, false, false)) return false;
	if (!InvokeSuccess(MakeCondition(), ordered, 2, "truncated capacity")) return false;
	const DirName filter = MakeName("gamma9");
	const auto one = Ordered(Select(catalog, {"gamma9"}), SORT_NAME, ORDER_ASCENDING);
	if (!InvokeSuccess(MakeCondition(&filter), one, 1, "optional parameter and information arrays", true, false, false))
		return false;
	if (!InvokeSuccess(MakeCondition(&filter), one, 1, "optional parameter array", true, false, true)) return false;
	if (!InvokeSuccess(MakeCondition(&filter), one, 1, "optional information array", true, true, false)) return false;
	for (int32_t user : {std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()})
	{
		auto condition = MakeCondition(&filter);
		condition.user_id = user;
		condition.title_id = reinterpret_cast<const TitleId*>(uintptr_t {1});
		if (!InvokeSuccess(condition, one, 1, "ignored user and inaccessible title id")) return false;
	}
	return true;
}

bool TestParameterErrors()
{
	const DirName all = MakeName("%");
	auto condition = MakeCondition(&all);
	OutputState output;
	output.Prepare(1);
	const OutputState before_null_condition = output;
	if (!Check(g_search(nullptr, &output.result) == SaveDataError::SAVE_DATA_ERROR_PARAMETER &&
	           SameBytes(output, before_null_condition), "null condition changed output or returned wrong error")) return false;
	const SearchCondition before_null_result = condition;
	if (!Check(g_search(&condition, nullptr) == SaveDataError::SAVE_DATA_ERROR_PARAMETER &&
	           SameBytes(condition, before_null_result), "null result changed input or returned wrong error")) return false;

	DirName unterminated;
	std::memset(unterminated.data, 'x', sizeof(unterminated.data));
	condition = MakeCondition(&unterminated);
	output.Prepare(1);
	if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "unterminated filter")) return false;
	for (int32_t key : {-1, 4, 6, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()})
	{
		condition = MakeCondition(&all, key, ORDER_ASCENDING);
		output.Prepare(1);
		if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "invalid sort key")) return false;
	}
	for (int32_t order : {-1, 2, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()})
	{
		condition = MakeCondition(&all, SORT_NAME, order);
		output.Prepare(1);
		if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "invalid sort order")) return false;
	}
	condition = MakeCondition(&all);
	output.Prepare(1, false, true, true);
	if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "required name array is null")) return false;

	alignas(SearchCondition) std::array<uint8_t, sizeof(SearchCondition) + 8> condition_bytes;
	condition_bytes.fill(CANARY);
	const auto valid_condition = MakeCondition(&all);
	std::memcpy(condition_bytes.data() + 1, &valid_condition, sizeof(valid_condition));
	const auto condition_bytes_before = condition_bytes;
	output.Prepare(1);
	const auto output_before = output;
	const auto* misaligned_condition = reinterpret_cast<const SearchCondition*>(condition_bytes.data() + 1);
	if (!Check(g_search(misaligned_condition, &output.result) == SaveDataError::SAVE_DATA_ERROR_PARAMETER &&
	           condition_bytes == condition_bytes_before && SameBytes(output, output_before),
	           "misaligned condition was accepted or changed bytes")) return false;

	alignas(SearchResult) std::array<uint8_t, sizeof(SearchResult) + 8> result_bytes;
	result_bytes.fill(CANARY);
	const auto result_bytes_before = result_bytes;
	if (!Check(g_search(&valid_condition, reinterpret_cast<SearchResult*>(result_bytes.data() + 1)) ==
	               SaveDataError::SAVE_DATA_ERROR_PARAMETER && result_bytes == result_bytes_before,
	           "misaligned result was accepted or changed bytes")) return false;

	alignas(Param) std::array<uint8_t, sizeof(Param) + 8> array_bytes;
	array_bytes.fill(CANARY);
	output.Prepare(1);
	output.result.params = reinterpret_cast<Param*>(array_bytes.data() + 1);
	if (!ExpectError(valid_condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "misaligned parameter array")) return false;
	output.Prepare(1);
	output.result.infos = reinterpret_cast<Info*>(array_bytes.data() + 1);
	if (!ExpectError(valid_condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "misaligned information array")) return false;

	const uintptr_t maximum = std::numeric_limits<uintptr_t>::max();
	output.Prepare(1);
	output.result.dir_names = reinterpret_cast<DirName*>(maximum);
	if (!ExpectError(valid_condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "name array address overflow")) return false;
	output.Prepare(1);
	output.result.params = reinterpret_cast<Param*>(maximum & ~(uintptr_t(alignof(Param)) - 1));
	if (!ExpectError(valid_condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "parameter array address overflow")) return false;
	output.Prepare(1);
	output.result.infos = reinterpret_cast<Info*>(maximum & ~(uintptr_t(alignof(Info)) - 1));
	if (!ExpectError(valid_condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "information array address overflow")) return false;
	return true;
}

bool TestOverlaps(const Catalog& catalog)
{
	const DirName all = MakeName("%");
	auto condition = MakeCondition(&all);
	OutputState output;
	output.Prepare(1);
	output.result.dir_names = reinterpret_cast<DirName*>(&output.result);
	if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "result and name output overlap")) return false;
	output.Prepare(1);
	output.result.dir_names = reinterpret_cast<DirName*>(&condition);
	const auto condition_before = condition;
	const auto output_before = output;
	if (!Check(g_search(&condition, &output.result) == SaveDataError::SAVE_DATA_ERROR_PARAMETER &&
	           SameBytes(condition, condition_before) && SameBytes(output, output_before),
	           "condition and name output overlap was accepted or changed bytes")) return false;
	output.Prepare(1);
	output.result.dir_names = reinterpret_cast<DirName*>(output.params.records.data());
	if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "name and parameter outputs overlap")) return false;
	output.Prepare(1);
	output.result.dir_names = reinterpret_cast<DirName*>(output.infos.records.data());
	if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "name and information outputs overlap")) return false;
	output.Prepare(1);
	output.result.params = reinterpret_cast<Param*>(output.infos.records.data());
	if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, "parameter and information outputs overlap")) return false;

	alignas(8) std::array<uint8_t, 4096> arena;
	arena.fill(CANARY);
	SearchResult result;
	Fill(result);
	result.dir_names = reinterpret_cast<DirName*>(arena.data());
	result.capacity = 2;
	result.params = reinterpret_cast<Param*>(arena.data() + sizeof(DirName));
	result.infos = nullptr;
	const DirName one_filter = MakeName("gamma9");
	auto one_condition = MakeCondition(&one_filter);
	auto expected_arena = arena;
	std::memset(expected_arena.data(), 0, sizeof(DirName));
	std::memcpy(expected_arena.data(), "gamma9", 6);
	std::memset(expected_arena.data() + sizeof(DirName), 0, sizeof(Param));
	const auto found = std::find_if(catalog.entries.begin(), catalog.entries.end(), [](const Entry& entry) {
		return entry.name == "gamma9";
	});
	if (!Check(found != catalog.entries.end(), "missing one-record overlap fixture")) return false;
	std::memcpy(expected_arena.data() + sizeof(DirName) + offsetof(Param, mtime), &found->mtime, sizeof(found->mtime));
	SearchResult expected_result = result;
	expected_result.total = 1;
	expected_result.written = 1;
	if (!Check(g_search(&one_condition, &result) == 0 && SameBytes(result, expected_result) && arena == expected_arena,
	           "disjoint effective one-record intervals failed")) return false;

	arena.fill(CANARY);
	Fill(result);
	result.dir_names = reinterpret_cast<DirName*>(arena.data());
	result.capacity = 2;
	result.params = reinterpret_cast<Param*>(arena.data() + sizeof(DirName));
	result.infos = nullptr;
	const auto arena_before = arena;
	const auto result_before = result;
	if (!Check(g_search(&condition, &result) == SaveDataError::SAVE_DATA_ERROR_PARAMETER &&
	           arena == arena_before && SameBytes(result, result_before),
	           "overlapping effective two-record intervals changed output")) return false;

	SearchCondition same_condition = MakeCondition(&all);
	const auto same_before = same_condition;
	if (!Check(g_search(&same_condition, reinterpret_cast<SearchResult*>(&same_condition)) ==
	               SaveDataError::SAVE_DATA_ERROR_PARAMETER && SameBytes(same_condition, same_before),
	           "condition/result overlap was accepted or changed bytes")) return false;
	return true;
}

bool TestReadOnlyFixture(const Catalog& catalog)
{
	std::error_code error;
	for (const auto& snapshot : catalog.timestamps)
	{
		if (!Check(fs::last_write_time(snapshot.first, error) == snapshot.second && !error,
		           "search changed a fixture last-write timestamp")) return false;
	}
	std::ifstream file(fs::path("_SaveData") / "ordinary-file", std::ios::binary);
	const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return Check(contents == catalog.root_file_contents, "search changed fixture file contents");
}

bool TestInvalidUtf8Entry()
{
#if defined(_WIN32)
	return true;
#else
	std::string invalid_name = "invalid-";
	invalid_name.push_back(static_cast<char>(0xff));
	std::error_code error;
	const fs::path invalid_path = fs::path("_SaveData") / fs::path(invalid_name);
	fs::create_directory(invalid_path, error);
	if (error)
	{
		std::puts("SaveDataTest: invalid UTF-8 host-name case skipped; fixture filesystem rejected the byte sequence");
		return true;
	}
	OutputState output;
	output.Prepare(1);
	const auto condition = MakeCondition();
	const bool result = ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_INTERNAL,
	                                "invalid UTF-8 host entry");
	fs::remove(invalid_path, error);
	return result && Check(!error, "cannot remove invalid UTF-8 filename fixture");
#endif
}

MountPoint MakeMountPoint(std::string_view value)
{
	MountPoint point {};
	if (value.size() < sizeof(point.data)) std::memcpy(point.data, value.data(), value.size());
	return point;
}

enum class MountApi
{
	Mount,
	Mount2,
};

constexpr std::array<uint32_t, 4> OPEN_MOUNT_MODES = {1, 2, 9, 10};
constexpr std::array<uint32_t, 4> CREATE_MOUNT_MODES = {6, 14, 22, 30};

struct MountInputs
{
	DirName       dir_name;
	MountRequest  mount;
	Mount2Request mount2;
};

static_assert(std::is_trivially_copyable_v<MountInputs>);

const char* MountApiName(MountApi api)
{
	return api == MountApi::Mount ? "Mount" : "Mount2";
}

void PrepareMountInputs(MountInputs& inputs, std::string_view dir_name, uint32_t mode)
{
	Fill(inputs);
	if (dir_name.size() < sizeof(inputs.dir_name.data))
	{
		std::memcpy(inputs.dir_name.data, dir_name.data(), dir_name.size());
		inputs.dir_name.data[dir_name.size()] = '\0';
	}
	inputs.mount.user_id       = std::numeric_limits<int32_t>::min();
	inputs.mount.title_id      = reinterpret_cast<const char*>(uintptr_t {1});
	inputs.mount.dir_name      = inputs.dir_name.data;
	inputs.mount.fingerprint   = reinterpret_cast<const char*>(uintptr_t {2});
	inputs.mount.blocks        = std::numeric_limits<uint64_t>::max();
	inputs.mount.mount_mode    = mode;
	inputs.mount2.user_id      = std::numeric_limits<int32_t>::max();
	inputs.mount2.dir_name     = &inputs.dir_name;
	inputs.mount2.blocks       = std::numeric_limits<uint64_t>::max() - 1;
	inputs.mount2.mount_mode   = mode;
}

int32_t InvokeMount(MountApi api, const MountInputs& inputs, MountResult* result)
{
	return api == MountApi::Mount ? g_mount(&inputs.mount, result) : g_mount2(&inputs.mount2, result);
}

bool MappingIs(const Lumen::String& expected, const char* name, const char* detail)
{
	return CaseCheck(Lumen::Libs::LibKernel::FileSystem::GetRealFilename(U"/savedata0/probe.bin") == expected,
	                 name, detail);
}

bool MappingIsAbsent(const char* name, const char* detail)
{
	return MappingIs(Lumen::String(U"/savedata0/probe.bin"), name, detail);
}

bool MappingTargetsSlot(std::string_view slot, const char* name)
{
	const std::string slot_string(slot);
	const Lumen::String expected = Lumen::String(U"_SaveData/") + Lumen::String::FromUtf8(slot_string.c_str()) +
	                               U"/probe.bin";
	return MappingIs(expected, name, "successful mount did not publish the slot mapping");
}

bool ResetMountMapping(const char* name)
{
	return CaseCheck(Lumen::Libs::LibKernel::FileSystem::ResetForNextSession(), name,
	                 "FileSystem session reset failed") &&
	       MappingIsAbsent(name, "FileSystem session reset retained a mount mapping");
}

bool PrepareSentinelMapping(const char* name)
{
	if (!ResetMountMapping(name)) return false;
	std::error_code error;
	fs::create_directories("mapping-sentinel", error);
	if (!CaseCheck(!error, name, "cannot create mapping sentinel directory")) return false;
	Lumen::Libs::LibKernel::FileSystem::Mount(U"mapping-sentinel", U"/savedata0");
	return MappingIs(Lumen::String(U"mapping-sentinel/probe.bin"), name,
	                 "cannot establish the preexisting mapping sentinel");
}

bool SentinelMappingIsUnchanged(const char* name)
{
	return MappingIs(Lumen::String(U"mapping-sentinel/probe.bin"), name,
	                 "error path changed the preexisting mount mapping");
}

bool MountOutputIsSuccess(const GuardedRecords<MountResult, 1>& actual,
	                      const GuardedRecords<MountResult, 1>& before, uint32_t status, const char* name)
{
	auto expected = before;
	constexpr char expected_point[] = "/savedata0";
	std::memset(expected.records[0].mount_point.data, 0, sizeof(expected.records[0].mount_point.data));
	std::memcpy(expected.records[0].mount_point.data, expected_point, sizeof(expected_point));
	expected.records[0].required_blocks = 0;
	expected.records[0].mount_status    = status;
	return CaseCheck(SameBytes(actual, expected), name,
	                 "success changed unexpected result, reserved, or adjacent canary bytes");
}

bool RunMountSuccess(MountApi api, std::string_view slot, uint32_t mode, uint32_t expected_status,
	                 const char* name)
{
	MountInputs inputs;
	PrepareMountInputs(inputs, slot, mode);
	if (!PrepareSentinelMapping(name)) return false;
	const MountInputs inputs_before = inputs;
	GuardedRecords<MountResult, 1> result;
	Fill(result);
	const auto result_before = result;
	const int32_t call_result = InvokeMount(api, inputs, &result.records[0]);
	if (!CaseCheck(call_result == 0, name, "valid mount returned an error") ||
	    !CaseCheck(SameBytes(inputs, inputs_before), name, "valid mount changed its input") ||
	    !MountOutputIsSuccess(result, result_before, expected_status, name) ||
	    !CaseCheck(fs::is_directory(fs::path("_SaveData") / Utf8Path(slot)), name,
	               "successful mount did not leave a real slot directory") ||
	    !MappingTargetsSlot(slot, name))
		return false;
	const char integration_title[] = "mount integration";
	if (!CaseCheck(g_set_param(&result.records[0].mount_point, 1, integration_title, sizeof(integration_title)) == 0, name,
	               "SetParam did not resolve the mapping published by mount"))
		return false;

	const MountPoint point_before = result.records[0].mount_point;
	if (!CaseCheck(g_umount(&result.records[0].mount_point) == 0, name, "SaveDataUmount returned an error") ||
	    !CaseCheck(SameBytes(result.records[0].mount_point, point_before), name, "SaveDataUmount changed its input") ||
	    !MappingIsAbsent(name, "SaveDataUmount retained the slot mapping"))
		return false;

	Lumen::Libs::LibKernel::FileSystem::Mount(U"mapping-sentinel", U"/savedata0");
	return ResetMountMapping(name);
}

bool ExpectMountError(MountApi api, const MountInputs& inputs, MountResult& output, int32_t expected_error,
	                  const char* name)
{
	const MountInputs inputs_before = inputs;
	const MountResult output_before = output;
	const int32_t result = InvokeMount(api, inputs, &output);
	return CaseCheck(result == expected_error, name, "mount returned the wrong error") &&
	       CaseCheck(SameBytes(inputs, inputs_before), name, "mount changed its input on error") &&
	       CaseCheck(SameBytes(output, output_before), name, "mount changed output on error") &&
	       SentinelMappingIsUnchanged(name);
}

bool ReadFileEquals(const fs::path& path, std::string_view expected)
{
	std::ifstream file(path, std::ios::binary);
	const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	return file.is_open() && !file.bad() && contents == expected;
}

bool TestValidMountModes()
{
	for (const MountApi api : {MountApi::Mount, MountApi::Mount2})
	{
		for (const uint32_t mode : OPEN_MOUNT_MODES)
		{
			const std::string name = std::string(MountApiName(api)) + " open mode " + std::to_string(mode);
			if (!ResetMountMapping(name.c_str()) || !RemoveRoot()) return false;
			const std::string slot = "OpenSlot" + std::to_string(mode);
			std::error_code error;
			fs::create_directories(fs::path("_SaveData") / slot, error);
			if (!CaseCheck(!error, name.c_str(), "cannot create open-mode slot fixture")) return false;
			const fs::path marker_path = fs::path("_SaveData") / slot / "preserve.bin";
			constexpr std::string_view marker_contents = "preserve-opened-slot";
			{
				std::ofstream marker(marker_path, std::ios::binary);
				marker << marker_contents;
				if (!CaseCheck(static_cast<bool>(marker), name.c_str(), "cannot write open-mode content fixture"))
					return false;
			}
			if (!RunMountSuccess(api, slot, mode, 0, name.c_str()) ||
			    !CaseCheck(ReadFileEquals(marker_path, marker_contents), name.c_str(),
			               "opening an existing slot changed its contents") || !RemoveRoot())
				return false;
		}
		for (const uint32_t mode : CREATE_MOUNT_MODES)
		{
			for (const bool root_exists : {false, true})
			{
				const std::string name = std::string(MountApiName(api)) + " create mode " + std::to_string(mode) +
				                         (root_exists ? " root-present" : " root-absent");
				if (!ResetMountMapping(name.c_str()) || !RemoveRoot()) return false;
				if (root_exists)
				{
					std::error_code error;
					fs::create_directory("_SaveData", error);
					if (error) return CaseCheck(false, name.c_str(), "cannot create create-mode root fixture");
				}
				const std::string slot = "CreateSlot" + std::to_string(mode);
				if (!RunMountSuccess(api, slot, mode, 1, name.c_str()) || !RemoveRoot()) return false;
			}
		}
	}
	return true;
}

bool TestMountNamePolicyIsNarrow()
{
	for (const MountApi api : {MountApi::Mount, MountApi::Mount2})
	{
		const std::string name = std::string(MountApiName(api)) + " case-sensitive internal prefix";
		if (!ResetMountMapping(name.c_str()) || !RemoveRoot() ||
		    !RunMountSuccess(api, "SCE_visible", 6, 1, name.c_str()) || !RemoveRoot())
			return false;
	}
	return true;
}

bool TestMountExistsAndNotFound()
{
	for (const MountApi api : {MountApi::Mount, MountApi::Mount2})
	{
		for (const uint32_t mode : CREATE_MOUNT_MODES)
		{
			const std::string name = std::string(MountApiName(api)) + " existing create mode " +
			                         std::to_string(mode);
			if (!RemoveRoot()) return false;
			std::error_code error;
			const fs::path slot = fs::path("_SaveData") / "ExistingSlot";
			fs::create_directories(slot, error);
			if (error) return CaseCheck(false, name.c_str(), "cannot create existing slot fixture");
			{
				std::ofstream file(slot / "preserve.bin", std::ios::binary);
				file << "preserve-existing-slot";
			}
			if (!PrepareSentinelMapping(name.c_str())) return false;
			MountInputs inputs;
			PrepareMountInputs(inputs, "ExistingSlot", mode);
			MountResult output;
			Fill(output);
			if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_EXISTS, name.c_str()) ||
			    !CaseCheck(ReadFileEquals(slot / "preserve.bin", "preserve-existing-slot"), name.c_str(),
			               "existing-slot error changed fixture contents") ||
			    !ResetMountMapping(name.c_str()))
				return false;
		}

		for (const uint32_t mode : OPEN_MOUNT_MODES)
		{
			for (const bool root_exists : {false, true})
			{
				const std::string name = std::string(MountApiName(api)) + " missing open mode " +
				                         std::to_string(mode) + (root_exists ? " root-present" : " root-absent");
				if (!RemoveRoot()) return false;
				std::error_code error;
				if (root_exists)
				{
					fs::create_directory("_SaveData", error);
					if (error) return CaseCheck(false, name.c_str(), "cannot create missing-slot root fixture");
				}
				if (!PrepareSentinelMapping(name.c_str())) return false;
				MountInputs inputs;
				PrepareMountInputs(inputs, "MissingSlot", mode);
				MountResult output;
				Fill(output);
				if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_NOT_FOUND, name.c_str()) ||
				    !CaseCheck(fs::exists("_SaveData") == root_exists, name.c_str(),
				               "open of a missing slot changed root existence") ||
				    !CaseCheck(!fs::exists(fs::path("_SaveData") / "MissingSlot"), name.c_str(),
				               "open of a missing slot created it") ||
				    !ResetMountMapping(name.c_str()))
					return false;
			}
		}
	}
	return RemoveRoot();
}

enum class InvalidMountShape
{
	RootFile,
	RootSymlink,
	SlotFile,
	SlotSymlink,
};

const char* InvalidMountShapeName(InvalidMountShape shape)
{
	switch (shape)
	{
		case InvalidMountShape::RootFile: return "root-file";
		case InvalidMountShape::RootSymlink: return "root-symlink";
		case InvalidMountShape::SlotFile: return "slot-file";
		case InvalidMountShape::SlotSymlink: return "slot-symlink";
	}
	return "unknown-shape";
}

bool PrepareInvalidMountShape(InvalidMountShape shape, const char* name)
{
	if (!RemoveRoot()) return false;
	std::error_code error;
	fs::remove_all("shape-target", error);
	if (error) return CaseCheck(false, name, "cannot clear shape target");
	if (shape == InvalidMountShape::RootFile)
	{
		std::ofstream file("_SaveData", std::ios::binary);
		file << "root-file";
		return CaseCheck(static_cast<bool>(file), name, "cannot create root-file fixture");
	}
	if (shape == InvalidMountShape::RootSymlink)
	{
		fs::create_directory("shape-target", error);
		if (!error)
		{
			std::ofstream marker(fs::path("shape-target") / "preserve.bin", std::ios::binary);
			marker << "preserve-symlink-target";
			if (!marker) return CaseCheck(false, name, "cannot create root-symlink target marker");
		}
		if (!error) fs::create_directory_symlink(fs::absolute("shape-target"), "_SaveData", error);
		return CaseCheck(!error, name, "cannot create root-symlink fixture");
	}

	fs::create_directory("_SaveData", error);
	if (error) return CaseCheck(false, name, "cannot create slot-shape root fixture");
	if (shape == InvalidMountShape::SlotFile)
	{
		std::ofstream file(fs::path("_SaveData") / "ShapeSlot", std::ios::binary);
		file << "slot-file";
		return CaseCheck(static_cast<bool>(file), name, "cannot create slot-file fixture");
	}
	fs::create_directory("shape-target", error);
	if (!error)
	{
		std::ofstream marker(fs::path("shape-target") / "preserve.bin", std::ios::binary);
		marker << "preserve-symlink-target";
		if (!marker) return CaseCheck(false, name, "cannot create slot-symlink target marker");
	}
	if (!error)
		fs::create_directory_symlink(fs::absolute("shape-target"), fs::path("_SaveData") / "ShapeSlot", error);
	return CaseCheck(!error, name, "cannot create slot-symlink fixture");
}

bool InvalidMountShapeIsUnchanged(InvalidMountShape shape, const char* name)
{
	std::error_code error;
	const fs::file_status root_status = fs::symlink_status("_SaveData", error);
	if (!CaseCheck(!error, name, "cannot inspect root shape after error")) return false;
	switch (shape)
	{
		case InvalidMountShape::RootFile:
			return CaseCheck(fs::is_regular_file(root_status) && ReadFileEquals("_SaveData", "root-file"), name,
			                 "root-file error changed the fixture");
		case InvalidMountShape::RootSymlink:
			return CaseCheck(fs::is_symlink(root_status) &&
			                     ReadFileEquals(fs::path("shape-target") / "preserve.bin", "preserve-symlink-target"),
			                 name, "root-symlink error changed the link or its target");
		case InvalidMountShape::SlotFile:
			return CaseCheck(fs::is_directory(root_status) &&
			                     fs::is_regular_file(fs::symlink_status(fs::path("_SaveData") / "ShapeSlot", error)) &&
			                     !error && ReadFileEquals(fs::path("_SaveData") / "ShapeSlot", "slot-file"),
			                 name, "slot-file error changed the fixture");
		case InvalidMountShape::SlotSymlink:
			return CaseCheck(fs::is_directory(root_status) &&
			                     fs::is_symlink(fs::symlink_status(fs::path("_SaveData") / "ShapeSlot", error)) && !error &&
			                     ReadFileEquals(fs::path("shape-target") / "preserve.bin", "preserve-symlink-target"),
			                 name, "slot-symlink error changed the link or its target");
	}
	return false;
}

bool TestMountFilesystemShapes()
{
	for (const MountApi api : {MountApi::Mount, MountApi::Mount2})
	{
		for (const uint32_t mode : {uint32_t {1}, uint32_t {6}})
		{
			for (const InvalidMountShape shape : {InvalidMountShape::RootFile, InvalidMountShape::RootSymlink,
			                                          InvalidMountShape::SlotFile, InvalidMountShape::SlotSymlink})
			{
				const std::string name = std::string(MountApiName(api)) + " " + InvalidMountShapeName(shape) +
				                         " mode " + std::to_string(mode);
				if (!PrepareInvalidMountShape(shape, name.c_str()) || !PrepareSentinelMapping(name.c_str())) return false;
				MountInputs inputs;
				PrepareMountInputs(inputs, "ShapeSlot", mode);
				MountResult output;
				Fill(output);
				if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_INTERNAL, name.c_str()) ||
				    !InvalidMountShapeIsUnchanged(shape, name.c_str()) || !ResetMountMapping(name.c_str()))
					return false;
			}
		}
	}
	std::error_code ignored;
	fs::remove_all("shape-target", ignored);
	return RemoveRoot();
}

bool PreflightFixtureIsUnchanged(const char* name)
{
	return CaseCheck(ReadFileEquals("_SaveData", "preflight-root-file"), name,
	                 "parameter error touched the filesystem fixture") &&
	       SentinelMappingIsUnchanged(name);
}

bool TestMountParameterPreflight()
{
	if (!RemoveRoot()) return false;
	{
		std::ofstream file("_SaveData", std::ios::binary);
		file << "preflight-root-file";
		if (!Check(static_cast<bool>(file), "cannot create mount preflight root-file fixture")) return false;
	}
	if (!PrepareSentinelMapping("mount parameter preflight")) return false;

	for (const MountApi api : {MountApi::Mount, MountApi::Mount2})
	{
		const std::string prefix = MountApiName(api);
		MountInputs valid;
		PrepareMountInputs(valid, "ValidSlot", 2);

		{
			const std::string name = prefix + " null request";
			MountResult output;
			Fill(output);
			const MountResult before = output;
			const int32_t result = api == MountApi::Mount ? g_mount(nullptr, &output) : g_mount2(nullptr, &output);
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !CaseCheck(SameBytes(output, before), name.c_str(), "null request changed output") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " null result";
			const MountInputs before = valid;
			const int32_t result = InvokeMount(api, valid, nullptr);
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !CaseCheck(SameBytes(valid, before), name.c_str(), "null result changed input") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " null directory";
			MountInputs inputs = valid;
			if (api == MountApi::Mount) inputs.mount.dir_name = nullptr;
			else inputs.mount2.dir_name = nullptr;
			MountResult output;
			Fill(output);
			if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str()) ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}

		const std::array<std::pair<std::string, const char*>, 7> invalid_names = {{
		    {"", "empty directory"},
		    {".", "dot directory"},
		    {"..", "dotdot directory"},
		    {"bad/name", "forward-separator directory"},
		    {"bad\\name", "backward-separator directory"},
		    {"sce_hidden", "internal-prefix directory"},
		    {std::string("bad-") + static_cast<char>(0xff), "invalid-UTF8 directory"},
		}};
		for (const auto& invalid : invalid_names)
		{
			const std::string name = prefix + " " + invalid.second;
			MountInputs inputs;
			PrepareMountInputs(inputs, invalid.first, 2);
			MountResult output;
			Fill(output);
			if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str()) ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " unterminated directory";
			MountInputs inputs;
			PrepareMountInputs(inputs, "temporary", 2);
			std::memset(inputs.dir_name.data, 'x', sizeof(inputs.dir_name.data));
			MountResult output;
			Fill(output);
			if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str()) ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}

		for (uint32_t mode = 0; mode < 32; ++mode)
		{
			if (std::find(OPEN_MOUNT_MODES.begin(), OPEN_MOUNT_MODES.end(), mode) != OPEN_MOUNT_MODES.end() ||
			    std::find(CREATE_MOUNT_MODES.begin(), CREATE_MOUNT_MODES.end(), mode) != CREATE_MOUNT_MODES.end())
				continue;
			const std::string name = prefix + " invalid mode " + std::to_string(mode);
			MountInputs inputs;
			PrepareMountInputs(inputs, "ValidSlot", mode);
			MountResult output;
			Fill(output);
			if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str()) ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		for (const uint32_t mode : {uint32_t {32}, uint32_t {33}, uint32_t {34}, uint32_t {38},
		                            uint32_t {41}, uint32_t {42}, uint32_t {0x80000002u},
		                            uint32_t {0x80000009u}, uint32_t {0x8000000au},
		                            std::numeric_limits<uint32_t>::max()})
		{
			const std::string name = prefix + " unknown-bit mode " + std::to_string(mode);
			MountInputs inputs;
			PrepareMountInputs(inputs, "ValidSlot", mode);
			MountResult output;
			Fill(output);
			if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str()) ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}

		if (api == MountApi::Mount)
		{
			const std::string name = prefix + " misaligned request";
			alignas(MountRequest) std::array<uint8_t, sizeof(MountRequest) + 8> bytes;
			bytes.fill(CANARY);
			std::memcpy(bytes.data() + 1, &valid.mount, sizeof(valid.mount));
			const auto bytes_before = bytes;
			MountResult output;
			Fill(output);
			const MountResult output_before = output;
			const int32_t result = g_mount(reinterpret_cast<const MountRequest*>(bytes.data() + 1), &output);
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !CaseCheck(bytes == bytes_before && SameBytes(output, output_before), name.c_str(),
			               "misaligned request changed bytes") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		else
		{
			const std::string name = prefix + " misaligned request";
			alignas(Mount2Request) std::array<uint8_t, sizeof(Mount2Request) + 8> bytes;
			bytes.fill(CANARY);
			std::memcpy(bytes.data() + 1, &valid.mount2, sizeof(valid.mount2));
			const auto bytes_before = bytes;
			MountResult output;
			Fill(output);
			const MountResult output_before = output;
			const int32_t result = g_mount2(reinterpret_cast<const Mount2Request*>(bytes.data() + 1), &output);
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !CaseCheck(bytes == bytes_before && SameBytes(output, output_before), name.c_str(),
			               "misaligned request changed bytes") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " misaligned result";
			alignas(MountResult) std::array<uint8_t, sizeof(MountResult) + 8> bytes;
			bytes.fill(CANARY);
			const auto bytes_before = bytes;
			const MountInputs input_before = valid;
			const int32_t result = InvokeMount(api, valid, reinterpret_cast<MountResult*>(bytes.data() + 1));
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !CaseCheck(bytes == bytes_before && SameBytes(valid, input_before), name.c_str(),
			               "misaligned result changed bytes") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " request range overflow";
			MountResult output;
			Fill(output);
			const MountResult output_before = output;
			const uintptr_t maximum = std::numeric_limits<uintptr_t>::max();
			const int32_t result = api == MountApi::Mount ?
			                           g_mount(reinterpret_cast<const MountRequest*>(
			                                       maximum & ~(uintptr_t(alignof(MountRequest)) - 1)),
			                                   &output) :
			                           g_mount2(reinterpret_cast<const Mount2Request*>(
			                                        maximum & ~(uintptr_t(alignof(Mount2Request)) - 1)),
			                                    &output);
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !CaseCheck(SameBytes(output, output_before), name.c_str(), "overflowing request changed output") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " result range overflow";
			const MountInputs input_before = valid;
			const uintptr_t maximum = std::numeric_limits<uintptr_t>::max();
			auto* const output = reinterpret_cast<MountResult*>(
			    maximum & ~(uintptr_t(alignof(MountResult)) - 1));
			const int32_t result = InvokeMount(api, valid, output);
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !CaseCheck(SameBytes(valid, input_before), name.c_str(), "overflowing result changed input") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " directory range overflow";
			MountInputs inputs = valid;
			const uintptr_t maximum = std::numeric_limits<uintptr_t>::max();
			if (api == MountApi::Mount) inputs.mount.dir_name = reinterpret_cast<const char*>(maximum);
			else inputs.mount2.dir_name = reinterpret_cast<const DirName*>(maximum);
			MountResult output;
			Fill(output);
			if (!ExpectMountError(api, inputs, output, SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str()) ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " request-result overlap";
			alignas(MountRequest) std::array<uint8_t, sizeof(MountRequest)> bytes;
			bytes.fill(CANARY);
			int32_t result = 0;
			if (api == MountApi::Mount)
			{
				std::memcpy(bytes.data(), &valid.mount, sizeof(valid.mount));
				const auto bytes_before = bytes;
				result = g_mount(reinterpret_cast<const MountRequest*>(bytes.data()),
				                 reinterpret_cast<MountResult*>(bytes.data()));
				if (!CaseCheck(bytes == bytes_before, name.c_str(), "overlap changed shared bytes")) return false;
			}
			else
			{
				std::memcpy(bytes.data(), &valid.mount2, sizeof(valid.mount2));
				const auto bytes_before = bytes;
				result = g_mount2(reinterpret_cast<const Mount2Request*>(bytes.data()),
				                  reinterpret_cast<MountResult*>(bytes.data()));
				if (!CaseCheck(bytes == bytes_before, name.c_str(), "overlap changed shared bytes")) return false;
			}
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
		{
			const std::string name = prefix + " directory-result overlap";
			MountResult output;
			Fill(output);
			std::memcpy(output.mount_point.data, "OverlapSlot", sizeof("OverlapSlot"));
			MountRequest mount = valid.mount;
			Mount2Request mount2 = valid.mount2;
			mount.dir_name = output.mount_point.data;
			mount2.dir_name = reinterpret_cast<const DirName*>(&output);
			const MountRequest mount_before = mount;
			const Mount2Request mount2_before = mount2;
			const MountResult output_before = output;
			const int32_t result = api == MountApi::Mount ? g_mount(&mount, &output) : g_mount2(&mount2, &output);
			if (!CaseCheck(result == SaveDataError::SAVE_DATA_ERROR_PARAMETER, name.c_str(), "wrong error") ||
			    !CaseCheck(SameBytes(mount, mount_before) && SameBytes(mount2, mount2_before) &&
			                   SameBytes(output, output_before), name.c_str(), "directory-result overlap changed bytes") ||
			    !PreflightFixtureIsUnchanged(name.c_str()))
				return false;
		}
	}

	return ResetMountMapping("mount parameter preflight cleanup") && RemoveRoot();
}

bool TestSetParamCountedGuardPage(const MountPoint& point)
{
	namespace VM = Lumen::Core::VirtualMemory;
	constexpr uint64_t page_size = 16 * 1024;
	const uint64_t mapping = VM::AllocAligned(0, 2 * page_size, VM::Mode::ReadWrite, page_size);
	if (!Check(mapping != 0, "cannot allocate counted-string guard fixture")) return false;
	struct Cleanup
	{
		uint64_t mapping;
		~Cleanup() { VM::Free(mapping); }
	} cleanup {mapping};
	if (!Check(VM::Protect(mapping + page_size, page_size, VM::Mode::NoAccess),
	           "cannot protect counted-string guard page"))
		return false;

	const std::array<uint8_t, 3> utf8 = {0xe2, 0x82, 0xac};
	auto* const span = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(mapping + page_size - utf8.size()));
	std::memcpy(span, utf8.data(), utf8.size());
	for (uint32_t type = 1; type <= 3; ++type)
	{
		const std::string name = "guarded counted parameter type " + std::to_string(type);
		if (!CaseCheck(g_set_param(&point, type, span, utf8.size()) == 0, name.c_str(),
		               "UTF-8 span ending at a guard page failed") ||
		    !CaseCheck(std::memcmp(span, utf8.data(), utf8.size()) == 0, name.c_str(),
		               "successful guarded call changed its input") ||
		    !CaseCheck(g_set_param(&point, type, span, utf8.size() - 1) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		               name.c_str(), "truncated UTF-8 span was accepted") ||
		    !CaseCheck(std::memcmp(span, utf8.data(), utf8.size()) == 0, name.c_str(),
		               "rejected guarded call changed its input"))
			return false;
	}
	return true;
}

bool TestSetParamValidation()
{
	if (!RemoveRoot()) return false;
	std::error_code error;
	const fs::path  root = "_SaveData";
	const fs::path  slot = root / "ParamSlot";
	fs::create_directories(slot, error);
	if (error) return Check(false, "cannot create set-parameter slot");

	Lumen::Libs::LibKernel::FileSystem::Mount(U"_SaveData/ParamSlot", U"/savedata0");
	const MountPoint point = MakeMountPoint("/savedata0");
	const auto root_time = fs::last_write_time(root, error);
	if (error) return Check(false, "cannot snapshot set-parameter root timestamp");
	const auto slot_time = fs::last_write_time(slot, error);
	if (error) return Check(false, "cannot snapshot set-parameter slot timestamp");

	std::array<char, 33> title {};
	std::memcpy(title.data(), "01234567890123456789012345678901", 32);
	const char     subtitle[] = "portable subtitle";
	const char     detail[]   = "portable detail";
	const uint32_t user_param = 0x89abcdefu;
	const int64_t  mtime      = -123456789;
	Param all;
	Fill(all, 0x6b);
	std::memset(all.title, 0, sizeof(all.title));
	std::memset(all.subtitle, 0, sizeof(all.subtitle));
	std::memset(all.detail, 0, sizeof(all.detail));
	std::memcpy(all.title, "replacement", sizeof("replacement"));
	std::memcpy(all.subtitle, "all subtitle", sizeof("all subtitle"));
	std::memcpy(all.detail, "all detail", sizeof("all detail"));

	if (!Check(g_set_param(&point, 1, title.data(), title.size()) == 0,
	           "type 1 with the observed 33-byte shape failed") ||
	    !Check(g_set_param(&point, 2, subtitle, sizeof(subtitle)) == 0, "type 2 update failed") ||
	    !Check(g_set_param(&point, 3, detail, sizeof(detail)) == 0, "type 3 update failed") ||
	    !Check(g_set_param(&point, 4, &user_param, sizeof(user_param)) == 0, "type 4 update failed") ||
	    !Check(g_set_param(&point, 5, &mtime, sizeof(mtime)) == 0, "type 5 update failed") ||
	    !Check(g_set_param(&point, 0, &all, sizeof(all)) == 0, "full parameter update failed"))
		return false;

	size_t root_entries = 0;
	for (const auto& ignored : fs::directory_iterator(root, error))
	{
		(void)ignored;
		root_entries++;
	}
	if (!Check(!error && root_entries == 1 && fs::is_empty(slot, error) && !error,
	           "set-parameter created guest-visible or adjacent host data") ||
	    !Check(fs::last_write_time(root, error) == root_time && !error &&
	               fs::last_write_time(slot, error) == slot_time && !error,
	           "set-parameter changed save-data timestamps"))
		return false;

	MountPoint unterminated;
	std::memset(unterminated.data, 'x', sizeof(unterminated.data));
	const MountPoint     unmounted = MakeMountPoint("/not-mounted");
	std::array<char, 33> no_terminator;
	no_terminator.fill('x');
	const std::array<char, 3>   invalid_utf8 = {'x', static_cast<char>(0xff), '\0'};
	const std::array<char, 129> too_large_string {};
	Param invalid_all = all;
	std::memset(invalid_all.title, 'x', sizeof(invalid_all.title));
	if (!Check(g_set_param(nullptr, 1, title.data(), title.size()) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "null mount point returned the wrong error") ||
	    !Check(g_set_param(&point, 1, nullptr, title.size()) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "null parameter buffer returned the wrong error") ||
	    !Check(g_set_param(&unterminated, 1, title.data(), title.size()) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "unterminated mount point returned the wrong error") ||
	    !Check(g_set_param(&unmounted, 1, title.data(), title.size()) == SaveDataError::SAVE_DATA_ERROR_NOT_MOUNTED,
	           "unresolved mount returned the wrong error") ||
	    !Check(g_set_param(&point, 6, title.data(), title.size()) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "unknown parameter type returned the wrong error") ||
	    !Check(g_set_param(&point, 1, no_terminator.data(), no_terminator.size()) == 0,
	           "33-byte counted title without a terminator failed") ||
	    !Check(std::all_of(no_terminator.begin(), no_terminator.end(), [](char value) { return value == 'x'; }),
	           "counted title input was changed") ||
	    !Check(g_set_param(&point, 1, invalid_utf8.data(), invalid_utf8.size()) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "invalid UTF-8 title was accepted") ||
	    !Check(g_set_param(&point, 1, too_large_string.data(), too_large_string.size()) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "oversized title was accepted") ||
	    !Check(g_set_param(&point, 4, &user_param, sizeof(user_param) - 1) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "wrong scalar size was accepted") ||
	    !Check(g_set_param(&point, 0, &all, sizeof(all) - 1) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "short full record was accepted") ||
	    !Check(g_set_param(&point, 0, &all, sizeof(all) + 1) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "oversized full record was accepted") ||
	    !Check(g_set_param(&point, 0, &invalid_all, sizeof(invalid_all)) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	           "unterminated full-record title was accepted"))
		return false;

	std::array<char, 23> counted_subtitle;
	counted_subtitle.fill('s');
	for (const size_t size : {size_t {8}, counted_subtitle.size()})
	{
		if (!Check(g_set_param(&point, 2, counted_subtitle.data(), size) == 0,
		           "8/23-byte counted subtitle without a terminator failed"))
			return false;
	}
	if (!Check(std::all_of(counted_subtitle.begin(), counted_subtitle.end(), [](char value) { return value == 's'; }),
	           "counted subtitle input was changed"))
		return false;

	const std::array<size_t, 3> string_capacities = {sizeof(all.title), sizeof(all.subtitle), sizeof(all.detail)};
	for (uint32_t type = 1; type <= string_capacities.size(); ++type)
	{
		const size_t capacity = string_capacities[type - 1];
		const std::string name = "counted parameter type " + std::to_string(type);
		std::vector<char> bytes(capacity + 1, 'q');
		const auto before = bytes;
		if (!CaseCheck(g_set_param(&point, type, bytes.data(), capacity - 1) == 0, name.c_str(),
		               "counted capacity-minus-one string failed") ||
		    !CaseCheck(g_set_param(&point, type, bytes.data(), capacity) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		               name.c_str(), "full-capacity string without a terminator was accepted") ||
		    !CaseCheck(g_set_param(&point, type, bytes.data(), capacity + 1) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		               name.c_str(), "oversized counted string was accepted") ||
		    !CaseCheck(g_set_param(&point, type, bytes.data(), 0) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		               name.c_str(), "zero-byte string was accepted") ||
		    !CaseCheck(bytes == before, name.c_str(), "counted string input was changed"))
			return false;

		bytes[capacity - 1] = '\0';
		const auto terminated_before = bytes;
		const std::array<char, 3> utf8 = {static_cast<char>(0xe2), static_cast<char>(0x82), static_cast<char>(0xac)};
		const std::array<char, 3> terminated_prefix = {'p', '\0', static_cast<char>(0xff)};
		if (!CaseCheck(g_set_param(&point, type, bytes.data(), capacity) == 0, name.c_str(),
		               "full-capacity string with a final terminator failed") ||
		    !CaseCheck(bytes == terminated_before, name.c_str(), "terminated string input was changed") ||
		    !CaseCheck(g_set_param(&point, type, utf8.data(), utf8.size()) == 0, name.c_str(),
		               "valid counted UTF-8 string failed") ||
		    !CaseCheck(g_set_param(&point, type, utf8.data(), utf8.size() - 1) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		               name.c_str(), "UTF-8 continued beyond the supplied byte count") ||
		    !CaseCheck(g_set_param(&point, type, invalid_utf8.data(), invalid_utf8.size() - 1) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		               name.c_str(), "invalid counted UTF-8 string was accepted") ||
		    !CaseCheck(g_set_param(&point, type, terminated_prefix.data(), terminated_prefix.size()) == 0,
		               name.c_str(), "terminated prefix included bytes after its terminator"))
			return false;
	}

	if (!TestSetParamCountedGuardPage(point)) return false;

	for (const bool detail_field : {false, true})
	{
		Param invalid_record = all;
		if (detail_field) std::memset(invalid_record.detail, 'd', sizeof(invalid_record.detail));
		else std::memset(invalid_record.subtitle, 's', sizeof(invalid_record.subtitle));
		const Param before = invalid_record;
		if (!Check(g_set_param(&point, 0, &invalid_record, sizeof(invalid_record)) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "unterminated full-record subtitle/detail was accepted") ||
		    !Check(SameBytes(invalid_record, before), "invalid full record input was changed"))
			return false;
	}

	root_entries = 0;
	for (const auto& ignored : fs::directory_iterator(root, error))
	{
		(void)ignored;
		root_entries++;
	}
	const bool unchanged = Check(!error && root_entries == 1 && fs::is_empty(slot, error) && !error &&
	                                 fs::last_write_time(root, error) == root_time && !error &&
	                                 fs::last_write_time(slot, error) == slot_time && !error,
	                             "invalid set-parameter call changed the save-data tree");
	Lumen::Libs::LibKernel::FileSystem::Umount(U"/savedata0");
	return unchanged && RemoveRoot();
}

bool SelectExpectedParam(const Param& param, uint32_t type, const void** data, size_t* size)
{
	switch (type)
	{
		case 0:
			*data = &param;
			*size = sizeof(param);
			return true;
		case 1:
			*data = param.title;
			*size = std::strlen(param.title) + 1;
			return true;
		case 2:
			*data = param.subtitle;
			*size = std::strlen(param.subtitle) + 1;
			return true;
		case 3:
			*data = param.detail;
			*size = std::strlen(param.detail) + 1;
			return true;
		case 4:
			*data = &param.user_param;
			*size = sizeof(param.user_param);
			return true;
		case 5:
			*data = &param.mtime;
			*size = sizeof(param.mtime);
			return true;
		default: return false;
	}
}

bool ExpectGetParamSuccess(const MountPoint& point, uint32_t type, size_t capacity, const Param& snapshot,
	                        const char* name)
{
	constexpr size_t GUARD_SIZE = 16;
	std::array<uint8_t, sizeof(Param) + GUARD_SIZE * 3> bytes;
	bytes.fill(CANARY);
	const auto bytes_before = bytes;
	GuardedRecords<size_t, 1> got_size;
	Fill(got_size);
	const auto got_size_before = got_size;
	const MountPoint point_before = point;

	const void* expected_data = nullptr;
	size_t expected_size = 0;
	if (!CaseCheck(SelectExpectedParam(snapshot, type, &expected_data, &expected_size), name,
	               "test has no expected parameter projection") ||
	    !CaseCheck(capacity >= expected_size && capacity <= bytes.size() - GUARD_SIZE, name,
	               "test capacity does not fit its guarded buffer"))
		return false;

	auto expected_bytes = bytes_before;
	std::memcpy(expected_bytes.data() + GUARD_SIZE, expected_data, expected_size);
	auto expected_got_size = got_size_before;
	expected_got_size.records[0] = expected_size;

	const int32_t result = g_get_param(&point, type, bytes.data() + GUARD_SIZE, capacity,
	                                   &got_size.records[0]);
	return CaseCheck(result == 0, name, "valid get-parameter call returned an error") &&
	       CaseCheck(SameBytes(point, point_before), name, "get-parameter call changed its mount point") &&
	       CaseCheck(bytes == expected_bytes, name, "get-parameter output or adjacent canary bytes differ") &&
	       CaseCheck(SameBytes(got_size, expected_got_size), name,
	                 "get-parameter size or adjacent canary bytes differ");
}

bool ExpectGetParamError(const MountPoint* point, uint32_t type, size_t capacity, int32_t expected_error,
	                      const char* name)
{
	std::array<uint8_t, sizeof(Param) + 16> bytes;
	bytes.fill(CANARY);
	const auto bytes_before = bytes;
	GuardedRecords<size_t, 1> got_size;
	Fill(got_size);
	const auto got_size_before = got_size;
	MountPoint point_before {};
	if (point != nullptr) point_before = *point;

	const int32_t result = g_get_param(point, type, bytes.data(), capacity, &got_size.records[0]);
	return CaseCheck(result == expected_error, name, "get-parameter call returned the wrong error") &&
	       CaseCheck(point == nullptr || SameBytes(*point, point_before), name,
	                 "get-parameter error changed its mount point") &&
	       CaseCheck(bytes == bytes_before, name, "get-parameter error changed its output") &&
	       CaseCheck(SameBytes(got_size, got_size_before), name, "get-parameter error changed its size output");
}

bool TestGetParamValidation()
{
	const char* const setup_name = "get-parameter setup";
	if (!ResetMountMapping(setup_name) || !RemoveRoot()) return false;
	std::error_code error;
	const fs::path root = "_SaveData";
	const fs::path slot = root / "ReadParamSlot";
	const fs::path marker_path = slot / "preserve.bin";
	fs::create_directories(slot, error);
	if (error) return Check(false, "cannot create get-parameter slot");
	{
		std::ofstream marker(marker_path, std::ios::binary);
		marker << "get-parameter-read-only";
		if (!Check(static_cast<bool>(marker), "cannot create get-parameter marker")) return false;
	}
	const auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(77);
	fs::last_write_time(slot, stamp, error);
	if (error) return Check(false, "cannot set get-parameter slot mtime");
	bool mtime_ok = false;
	const int64_t expected_mtime = UnixMtime(slot, mtime_ok);
	if (!Check(mtime_ok, "cannot read get-parameter slot mtime")) return false;

	const auto root_time = fs::last_write_time(root, error);
	if (error) return Check(false, "cannot snapshot get-parameter root timestamp");
	const auto slot_time = fs::last_write_time(slot, error);
	if (error) return Check(false, "cannot snapshot get-parameter slot timestamp");
	Lumen::Libs::LibKernel::FileSystem::Mount(U"_SaveData/ReadParamSlot", U"/savedata0");
	const MountPoint point = MakeMountPoint("/savedata0");

	Param expected_snapshot {};
	expected_snapshot.mtime = expected_mtime;
	const DirName filter = MakeName("ReadParamSlot");
	if (!InvokeSuccess(MakeCondition(&filter), {{"ReadParamSlot", expected_mtime}}, 1,
	                   "get-parameter mtime agrees with directory search"))
		return false;
	const fs::path case_alias = root / "readparamslot";
	const bool case_alias_available = fs::is_directory(case_alias, error);
	if (error) return Check(false, "cannot probe get-parameter host case semantics");
	if (case_alias_available)
	{
		Lumen::Libs::LibKernel::FileSystem::Mount(U"_SaveData/readparamslot", U"/savedata0");
		if (!ExpectGetParamSuccess(point, 0, sizeof(Param), expected_snapshot,
		                           "get-parameter follows host case semantics"))
			return false;
		Lumen::Libs::LibKernel::FileSystem::Mount(U"_SaveData/ReadParamSlot", U"/savedata0");
	}

	if (!ExpectGetParamSuccess(point, 0, sizeof(Param), expected_snapshot,
	                           "observed full get-parameter shape") ||
	    !ExpectGetParamSuccess(point, 0, sizeof(Param) + 8, expected_snapshot,
	                           "full get-parameter oversized capacity"))
		return false;
	for (uint32_t type = 1; type <= 5; ++type)
	{
		const void* expected_data = nullptr;
		size_t expected_size = 0;
		if (!SelectExpectedParam(expected_snapshot, type, &expected_data, &expected_size)) return false;
		const std::string name = "get-parameter type " + std::to_string(type);
		if (!ExpectGetParamSuccess(point, type, expected_size, expected_snapshot, name.c_str())) return false;
	}
	if (!ExpectGetParamSuccess(point, 1, sizeof(expected_snapshot.title), expected_snapshot,
	                           "empty title leaves unused capacity unchanged") ||
	    !ExpectGetParamSuccess(point, 2, sizeof(expected_snapshot.subtitle), expected_snapshot,
	                           "empty subtitle leaves unused capacity unchanged") ||
	    !ExpectGetParamSuccess(point, 3, sizeof(expected_snapshot.detail), expected_snapshot,
	                           "empty detail leaves unused capacity unchanged"))
		return false;

	const char replacement_title[] = "persisted title";
	const int64_t replacement_mtime = expected_mtime - 1000;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	std::memcpy(expected_snapshot.title, replacement_title, sizeof(replacement_title));
	expected_snapshot.mtime = replacement_mtime;
#endif
	if (!Check(g_set_param(&point, 1, replacement_title, sizeof(replacement_title)) == 0,
	           "set-before-get title failed") ||
	    !Check(g_set_param(&point, 5, &replacement_mtime, sizeof(replacement_mtime)) == 0,
	           "set-before-get mtime failed") ||
	    !ExpectGetParamSuccess(point, 0, sizeof(Param), expected_snapshot,
	                           "get-parameter follows the host metadata contract"))
		return false;

	for (uint32_t type = 0; type <= 5; ++type)
	{
		const void* expected_data = nullptr;
		size_t expected_size = 0;
		if (!SelectExpectedParam(expected_snapshot, type, &expected_data, &expected_size)) return false;
		const std::string name = "short get-parameter type " + std::to_string(type);
		if (!ExpectGetParamError(&point, type, expected_size - 1, SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		                         name.c_str())) return false;
	}
	const MountPoint unmounted = MakeMountPoint("/not-mounted");
	MountPoint unterminated;
	std::memset(unterminated.data, 'x', sizeof(unterminated.data));
	if (!ExpectGetParamError(nullptr, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	                         "null get-parameter mount") ||
	    !ExpectGetParamError(&point, 6, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	                         "unknown get-parameter type") ||
	    !ExpectGetParamError(&unterminated, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_PARAMETER,
	                         "unterminated get-parameter mount") ||
	    !ExpectGetParamError(&unmounted, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_NOT_MOUNTED,
	                         "unresolved get-parameter mount"))
		return false;

	{
		std::array<uint8_t, sizeof(Param)> output;
		output.fill(CANARY);
		const auto output_before = output;
		GuardedRecords<size_t, 1> got_size;
		Fill(got_size);
		const auto got_size_before = got_size;
		if (!Check(g_get_param(&point, 0, nullptr, sizeof(Param), &got_size.records[0]) ==
		               SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "null get-parameter buffer returned the wrong error") ||
		    !Check(g_get_param(&point, 0, output.data(), sizeof(Param), nullptr) ==
		               SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "null get-parameter size returned the wrong error") ||
		    !Check(output == output_before && SameBytes(got_size, got_size_before),
		           "null get-parameter output changed accessible outputs"))
			return false;
	}

	{
		alignas(size_t) std::array<uint8_t, 32> shared;
		shared.fill(CANARY);
		const auto shared_before = shared;
		if (!Check(g_get_param(&point, 1, shared.data(), shared.size(),
		                       reinterpret_cast<size_t*>(shared.data() + 8)) ==
		               SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "overlapping get-parameter outputs returned the wrong error") ||
		    !Check(shared == shared_before, "overlapping get-parameter outputs changed bytes"))
			return false;
	}
	{
		alignas(size_t) MountPoint shared_point = point;
		const MountPoint shared_before = shared_point;
		GuardedRecords<size_t, 1> got_size;
		Fill(got_size);
		const auto got_size_before = got_size;
		if (!Check(g_get_param(&shared_point, 1, shared_point.data, sizeof(shared_point.data),
		                       &got_size.records[0]) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "mount-output overlap returned the wrong error") ||
		    !Check(SameBytes(shared_point, shared_before) && SameBytes(got_size, got_size_before),
		           "mount-output overlap changed bytes") ||
		    !Check(g_get_param(&shared_point, 1, got_size.before.data(), got_size.before.size(),
		                       reinterpret_cast<size_t*>(shared_point.data)) ==
		               SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "mount-size overlap returned the wrong error") ||
		    !Check(SameBytes(shared_point, shared_before) && SameBytes(got_size, got_size_before),
		           "mount-size overlap changed bytes"))
			return false;
	}

	{
		std::array<uint8_t, sizeof(Param)> output;
		output.fill(CANARY);
		const auto output_before = output;
		GuardedRecords<size_t, 1> got_size;
		Fill(got_size);
		const auto got_size_before = got_size;
		const uintptr_t maximum = std::numeric_limits<uintptr_t>::max();
		if (!Check(g_get_param(reinterpret_cast<const MountPoint*>(maximum), 0, output.data(), output.size(),
		                       &got_size.records[0]) == SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "overflowing get-parameter mount returned the wrong error") ||
		    !Check(g_get_param(&point, 1, reinterpret_cast<void*>(maximum), 1, &got_size.records[0]) ==
		               SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "overflowing get-parameter buffer returned the wrong error") ||
		    !Check(g_get_param(&point, 0, output.data(), output.size(), reinterpret_cast<size_t*>(maximum)) ==
		               SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		           "overflowing get-parameter size returned the wrong error") ||
		    !Check(output == output_before && SameBytes(got_size, got_size_before),
		           "overflowing get-parameter range changed accessible outputs"))
			return false;
	}

	if (!Check(ReadFileEquals(marker_path, "get-parameter-read-only"),
	           "get-parameter changed the slot marker") ||
	    !Check(fs::last_write_time(root, error) == root_time && !error &&
	               fs::last_write_time(slot, error) == slot_time && !error,
	           "get-parameter or set-before-get changed save-data timestamps"))
		return false;

	fs::remove_all(root, error);
	if (error) return Check(false, "cannot remove get-parameter root for missing-slot case");
	if (!ExpectGetParamError(&point, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_NOT_MOUNTED,
	                         "mounted get-parameter slot disappeared"))
		return false;
	{
		std::ofstream root_file(root, std::ios::binary);
		root_file << "not a directory";
	}
	if (!ExpectGetParamError(&point, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_INTERNAL,
	                         "invalid get-parameter root shape"))
		return false;
	if (!RemoveRoot()) return false;
	const fs::path link_target = "get-parameter-root-link-target";
	fs::create_directories(link_target / "ReadParamSlot", error);
	if (error) return Check(false, "cannot create get-parameter root-link target");
	fs::create_directory_symlink(fs::absolute(link_target), root, error);
	if (error) return Check(false, "cannot create get-parameter root-link fixture");
	if (!ExpectGetParamError(&point, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_INTERNAL,
	                         "get-parameter root link is not followed"))
		return false;
	if (!RemoveRoot()) return false;
	fs::remove_all(link_target, error);
	if (error) return Check(false, "cannot remove get-parameter root-link target");

	return ResetMountMapping("get-parameter cleanup") && RemoveRoot();
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX

Param PersistentParamFixture()
{
	Param param {};
	std::memcpy(param.title, "A saved title", sizeof("A saved title"));
	std::memcpy(param.subtitle, "A saved subtitle", sizeof("A saved subtitle"));
	std::memcpy(param.detail, "A saved detail", sizeof("A saved detail"));
	param.user_param = 73;
	param.mtime = -1357911;
	return param;
}

bool MemoryGenerationRaceChild(const char* mode)
{
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
	const bool writer_a = std::strcmp(mode, "memory-a") == 0;
	if (!writer_a && std::strcmp(mode, "memory-b") != 0) return false;
	using namespace Lumen::Libs::SaveData;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_RACE");
	if (g_initialize3(nullptr) != 0) return false;
	MemorySetup2 setup {};
	setup.user_id = 7;
	setup.memory_size = 64;
	setup.slot_id = 1;
	MemorySetupResult setup_result {};
	if (g_memory_setup2(&setup, &setup_result) != 0 || setup_result.existing_size != 1) return false;
	const char* ready_name = writer_a ? "memory-race-ready-a" : "memory-race-ready-b";
	{
		std::ofstream ready(ready_name, std::ios::binary);
		ready << "ready";
		if (!ready) return false;
	}
	bool released = false;
	for (unsigned attempt = 0; attempt < 5000; ++attempt)
	{
		std::error_code error;
		if (fs::exists("memory-race-go", error) && !error)
		{
			released = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (!released) return false;
	const uint8_t value = writer_a ? 0x41 : 0x42;
	MemoryData data {};
	data.buffer = reinterpret_cast<uint64_t>(&value);
	data.size = 1;
	MemorySet2Prefix set {};
	set.user_id = 7;
	set.data = &data;
	set.slot_id = 1;
	const int32_t result = g_memory_set2(&set);
	const char* result_name = writer_a ? "memory-race-result-a" : "memory-race-result-b";
	{
		std::ofstream result_file(result_name, std::ios::binary);
		result_file << result;
		if (!result_file || (result != 0 && result != SAVE_DATA_ERROR_INTERNAL)) return false;
	}
	bool check_released = false;
	for (unsigned attempt = 0; attempt < 5000; ++attempt)
	{
		std::error_code error;
		if (fs::exists("memory-race-check", error) && !error)
		{
			check_released = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (!check_released) return false;
	uint8_t observed = 0;
	MemoryData read_data {};
	read_data.buffer = reinterpret_cast<uint64_t>(&observed);
	read_data.size = 1;
	MemoryGet2 get {};
	get.user_id = 7;
	get.data = &read_data;
	get.slot_id = 1;
	if (g_memory_get2(&get) != 0) return false;
	const char* observed_name = writer_a ? "memory-race-observed-a" : "memory-race-observed-b";
	std::ofstream observed_file(observed_name, std::ios::binary);
	observed_file << static_cast<unsigned>(observed);
	return static_cast<bool>(observed_file);
#else
	(void)mode;
	return false;
#endif
}

bool MetadataChild(const char* mode)
{
	if (std::strncmp(mode, "memory-", 7) == 0) return MemoryGenerationRaceChild(mode);
	Lumen::Libs::LibKernel::FileSystem::Mount(U"_SaveData/PersistSlot", U"/savedata0");
	const MountPoint point = MakeMountPoint("/savedata0");
	if (std::strcmp(mode, "read") == 0)
		return ExpectGetParamSuccess(point, 0, sizeof(Param), PersistentParamFixture(), "fresh process metadata read");
	const bool title = std::strcmp(mode, "title") == 0;
	if (!title && std::strcmp(mode, "subtitle") != 0) return false;
	const char* value = title ? "process title" : "process subtitle";
	for (unsigned i = 0; i < 24; ++i)
		if (g_set_param(&point, title ? 1 : 2, value, std::strlen(value)) != 0) return false;
	return true;
}

bool SpawnMetadataChild(const char* mode, pid_t* pid)
{
	char* const arguments[] = {g_test_executable.data(), const_cast<char*>("--metadata-child"), const_cast<char*>(mode), nullptr};
	return Check(posix_spawn(pid, g_test_executable.c_str(), nullptr, nullptr, arguments, environ) == 0,
	             "cannot start metadata fixture subprocess");
}

bool WaitMetadataChild(pid_t pid)
{
	int status = 0;
	pid_t result;
	do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
	return Check(result == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "metadata fixture subprocess failed");
}

#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
bool RunSaveDataMemoryGenerationRace()
{
	using namespace Lumen::Libs::SaveData;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_RACE");
	if (!RemoveRoot() || !CaseCheck(g_initialize3(nullptr) == 0, "memory generation",
	                               "race fixture initialization failed")) return false;
	MemorySetup2 setup {};
	setup.user_id = 7;
	setup.memory_size = 64;
	setup.slot_id = 1;
	MemorySetupResult setup_result {};
	if (!CaseCheck(g_memory_setup2(&setup, &setup_result) == 0, "memory generation",
	               "race fixture setup failed")) return false;
	const uint8_t initial_value = 0x30;
	MemoryData initial_data {};
	initial_data.buffer = reinterpret_cast<uint64_t>(&initial_value);
	initial_data.size = 1;
	MemorySet2Prefix initial_set {};
	initial_set.user_id = 7;
	initial_set.data = &initial_data;
	initial_set.slot_id = 1;
	if (!CaseCheck(g_memory_set2(&initial_set) == 0, "memory generation",
	               "race fixture initial commit failed")) return false;
	ResetSessionState();
	for (const char* marker: {"memory-race-ready-a", "memory-race-ready-b", "memory-race-go",
	                         "memory-race-check", "memory-race-result-a", "memory-race-result-b",
	                         "memory-race-observed-a", "memory-race-observed-b"})
	{
		std::error_code error;
		fs::remove(marker, error);
		if (error) return CaseCheck(false, "memory generation", "cannot clear a stale race marker");
	}

	pid_t writer_a = -1;
	pid_t writer_b = -1;
	if (!SpawnMetadataChild("memory-a", &writer_a) || !SpawnMetadataChild("memory-b", &writer_b))
	{
		std::ofstream release("memory-race-go", std::ios::binary);
		std::ofstream check("memory-race-check", std::ios::binary);
		if (writer_a > 0) (void)WaitMetadataChild(writer_a);
		if (writer_b > 0) (void)WaitMetadataChild(writer_b);
		return false;
	}
	bool both_ready = false;
	for (unsigned attempt = 0; attempt < 5000; ++attempt)
	{
		std::error_code first_error;
		std::error_code second_error;
		if (fs::exists("memory-race-ready-a", first_error) && !first_error &&
		    fs::exists("memory-race-ready-b", second_error) && !second_error)
		{
			both_ready = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	{
		std::ofstream release("memory-race-go", std::ios::binary);
		release << "go";
		if (!release) both_ready = false;
	}
	int32_t result_a = std::numeric_limits<int32_t>::max();
	int32_t result_b = std::numeric_limits<int32_t>::max();
	bool both_set_results = false;
	for (unsigned attempt = 0; attempt < 5000; ++attempt)
	{
		std::ifstream first("memory-race-result-a", std::ios::binary);
		std::ifstream second("memory-race-result-b", std::ios::binary);
		int32_t candidate_a = std::numeric_limits<int32_t>::max();
		int32_t candidate_b = std::numeric_limits<int32_t>::max();
		if (first >> candidate_a && second >> candidate_b)
		{
			result_a = candidate_a;
			result_b = candidate_b;
			both_set_results = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	{
		std::ofstream check("memory-race-check", std::ios::binary);
		check << "check";
		if (!check) both_set_results = false;
	}
	const bool writer_a_ok = WaitMetadataChild(writer_a);
	const bool writer_b_ok = WaitMetadataChild(writer_b);
	const bool children_ok = writer_a_ok && writer_b_ok;
	const bool one_winner =
	    (result_a == 0 && result_b == SAVE_DATA_ERROR_INTERNAL) ||
	    (result_b == 0 && result_a == SAVE_DATA_ERROR_INTERNAL);
	unsigned observed_a = 0;
	unsigned observed_b = 0;
	{
		std::ifstream first("memory-race-observed-a", std::ios::binary);
		std::ifstream second("memory-race-observed-b", std::ios::binary);
		first >> observed_a;
		second >> observed_b;
		if (!first || !second) both_set_results = false;
	}
	const uint8_t expected = result_a == 0 ? 0x41 : 0x42;
	const bool both_reloaded_winner = observed_a == expected && observed_b == expected;
	if (!CaseCheck(both_ready && both_set_results && children_ok && one_winner && both_reloaded_winner,
	               "memory generation",
	               "two stale writers did not produce exactly one committed generation")) return false;

	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_RACE");
	if (!CaseCheck(g_initialize3(nullptr) == 0, "memory generation",
	               "post-race initialization failed")) return false;
	setup_result = {};
	if (!CaseCheck(g_memory_setup2(&setup, &setup_result) == 0 && setup_result.existing_size == 1,
	               "memory generation", "post-race store is invalid")) return false;
	uint8_t observed = 0;
	MemoryData read_data {};
	read_data.buffer = reinterpret_cast<uint64_t>(&observed);
	read_data.size = 1;
	MemoryGet2 get {};
	get.user_id = 7;
	get.data = &read_data;
	get.slot_id = 1;
	const bool durable_winner = g_memory_get2(&get) == 0 && observed == expected;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest(nullptr);
	return CaseCheck(durable_winner, "memory generation", "winning generation was not durable") && RemoveRoot();
}
#endif

#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
bool g_catalog_swap_root = false;
bool g_catalog_swap_ok = false;

void SwapMetadataCatalogFixture()
{
	std::error_code error;
	if (g_catalog_swap_root)
	{
		fs::rename("_SaveData", "catalog-original-root", error);
		if (!error) fs::rename("catalog-replacement-root", "_SaveData", error);
	} else
	{
		fs::rename("_SaveData/ScanSlot", "_SaveData/OriginalSlot", error);
		if (!error) fs::rename("catalog-replacement-slot", "_SaveData/ScanSlot", error);
	}
	g_catalog_swap_ok = !error;
}

bool TestMetadataCatalogIdentity()
{
	for (const bool swap_root: {false, true})
	{
		if (!RemoveRoot()) return false;
		std::error_code error;
		fs::create_directories("_SaveData/ScanSlot", error);
		const fs::path replacement = swap_root ? fs::path("catalog-replacement-root") / "ScanSlot" : fs::path("catalog-replacement-slot");
		if (!error) fs::create_directories(replacement, error);
		if (!Check(!error, "cannot prepare catalog identity fixture")) return false;
		g_catalog_swap_root = swap_root;
		g_catalog_swap_ok = false;
		OutputState output;
		output.Prepare(1);
		const auto condition = MakeCondition();
		Lumen::Libs::SaveData::SetMetadataCatalogTestHook(&SwapMetadataCatalogFixture);
		const bool rejected = ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_INTERNAL,
		                                 swap_root ? "catalog root replaced before metadata" : "catalog slot replaced before metadata");
		Lumen::Libs::SaveData::SetMetadataCatalogTestHook(nullptr);
		if (!Check(g_catalog_swap_ok && rejected, "catalog identity change published mixed outputs")) return false;
		fs::remove_all("catalog-original-root", error);
		if (error) return Check(false, "cannot remove original catalog fixture");
	}
	return RemoveRoot();
}
#endif

bool TestPrivateMetadataPersistence()
{
	if (!ResetMountMapping("metadata setup") || !RemoveRoot()) return false;
	const fs::path slot = fs::path("_SaveData") / "PersistSlot";
	std::error_code error;
	fs::create_directories(slot, error);
	if (!Check(!error, "cannot create metadata fixture slot")) return false;
	Lumen::Libs::LibKernel::FileSystem::Mount(U"_SaveData/PersistSlot", U"/savedata0");
	const MountPoint point = MakeMountPoint("/savedata0");
	Param expected = PersistentParamFixture();
	if (!Check(g_set_param(&point, 0, &expected, sizeof(expected)) == 0, "cannot persist full metadata fixture")) return false;
	for (uint32_t type = 0; type <= 5; ++type)
	{
		const void* data = nullptr;
		size_t size = 0;
		if (!SelectExpectedParam(expected, type, &data, &size) ||
		    !ExpectGetParamSuccess(point, type, size, expected, "metadata type round-trip") ||
		    !ExpectGetParamError(&point, type, size - 1, SaveDataError::SAVE_DATA_ERROR_PARAMETER,
		                         "metadata exact string/scalar capacity")) return false;
	}
	if (!Check(g_umount(&point) == 0, "metadata remount unmount failed")) return false;
	Lumen::Libs::LibKernel::FileSystem::Mount(U"_SaveData/PersistSlot", U"/savedata0");
	if (!ExpectGetParamSuccess(point, 0, sizeof(Param), expected, "metadata survives remount")) return false;
	pid_t child = -1;
	if (!SpawnMetadataChild("read", &child) || !WaitMetadataChild(child)) return false;

	const DirName filter = MakeName("PersistSlot");
	const auto condition = MakeCondition(&filter);
	OutputState output;
	output.Prepare(1);
	if (!Check(g_search(&condition, &output.result) == 0 && output.result.total == 1 && output.result.written == 1 &&
	               SameBytes(output.params.records[0], expected), "directory search lost persisted metadata")) return false;

	pid_t title_child = -1, subtitle_child = -1;
	if (!SpawnMetadataChild("title", &title_child)) return false;
	if (!SpawnMetadataChild("subtitle", &subtitle_child))
	{
		(void)WaitMetadataChild(title_child);
		return false;
	}
	std::atomic<bool> threads_ok {true};
	std::thread scalar_writer([&] {
		for (uint32_t value = 0; value < 48; ++value)
			if (g_set_param(&point, 4, &value, sizeof(value)) != 0) threads_ok.store(false);
	});
	std::thread detail_writer([&] {
		for (unsigned i = 0; i < 48; ++i)
			if (g_set_param(&point, 3, "thread detail", sizeof("thread detail")) != 0) threads_ok.store(false);
	});
	scalar_writer.join();
	detail_writer.join();
	const bool title_ok = WaitMetadataChild(title_child);
	const bool subtitle_ok = WaitMetadataChild(subtitle_child);
	if (!Check(title_ok && subtitle_ok && threads_ok.load(), "concurrent metadata update failed")) return false;
	std::memset(expected.title, 0, sizeof(expected.title));
	std::memcpy(expected.title, "process title", sizeof("process title"));
	std::memset(expected.subtitle, 0, sizeof(expected.subtitle));
	std::memcpy(expected.subtitle, "process subtitle", sizeof("process subtitle"));
	std::memset(expected.detail, 0, sizeof(expected.detail));
	std::memcpy(expected.detail, "thread detail", sizeof("thread detail"));
	expected.user_param = 47;
	if (!ExpectGetParamSuccess(point, 0, sizeof(Param), expected, "thread/process updates retain independent fields")) return false;

	std::vector<uint8_t> stored(16 + sizeof(Param));
	if (!Check(getxattr(slot.c_str(), PARAM_ATTRIBUTE, stored.data(), stored.size(), 0, 0) ==
	               static_cast<ssize_t>(stored.size()), "metadata is not stored as a bounded native attribute")) return false;
	for (unsigned shape = 0; shape < 7; ++shape)
	{
		auto malformed = stored;
		switch (shape)
		{
			case 0: malformed.pop_back(); break;
			case 1: malformed.push_back(0); break;
			case 2: malformed[0] ^= 0xff; break;
			case 3: malformed[8] = 2; break;
			case 4: malformed[12] = 0x80; break;
			case 5: std::fill_n(malformed.begin() + 16, sizeof(Param::title), 'x'); break;
			case 6: malformed[16] = 0xff; break;
		}
		if (!Check(setxattr(slot.c_str(), PARAM_ATTRIBUTE, malformed.data(), malformed.size(), 0, 0) == 0,
		           "cannot create malformed metadata fixture") ||
		    !ExpectGetParamError(&point, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_INTERNAL,
		                         "malformed metadata output is atomic")) return false;
		output.Prepare(1);
		if (!ExpectError(condition, output, SaveDataError::SAVE_DATA_ERROR_INTERNAL, "malformed search metadata") ||
		    !Check(g_set_param(&point, 1, "replacement", sizeof("replacement")) == SaveDataError::SAVE_DATA_ERROR_INTERNAL,
		           "parameter write overwrote malformed metadata")) return false;
		std::vector<uint8_t> after(malformed.size());
		if (!Check(getxattr(slot.c_str(), PARAM_ATTRIBUTE, after.data(), after.size(), 0, 0) ==
		               static_cast<ssize_t>(after.size()) && after == malformed, "failed write changed malformed metadata")) return false;
	}
	if (!Check(setxattr(slot.c_str(), PARAM_ATTRIBUTE, stored.data(), stored.size(), 0, 0) == 0,
	           "cannot restore metadata fixture")) return false;

	fs::rename(slot, fs::path("_SaveData") / "RenamedSlot", error);
	if (!Check(!error, "cannot rename metadata slot") ||
	    !ExpectGetParamError(&point, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_NOT_MOUNTED,
	                         "stale mount after slot rename") ||
	    !Check(g_set_param(&point, 1, "stale", sizeof("stale")) == SaveDataError::SAVE_DATA_ERROR_NOT_MOUNTED,
	           "stale mount modified renamed metadata")) return false;
	fs::create_directory(slot, error);
	bool mtime_ok = false;
	Param empty {};
	empty.mtime = UnixMtime(slot, mtime_ok);
	if (!Check(!error && mtime_ok, "cannot create replacement slot") ||
	    !ExpectGetParamSuccess(point, 0, sizeof(Param), empty, "replacement slot does not inherit old metadata")) return false;
	fs::remove(slot, error);
	if (!error) fs::create_directory_symlink("RenamedSlot", slot, error);
	if (!Check(!error, "cannot create metadata slot symlink") ||
	    !ExpectGetParamError(&point, 0, sizeof(Param), SaveDataError::SAVE_DATA_ERROR_INTERNAL,
	                         "metadata read does not follow slot symlink") ||
	    !Check(g_set_param(&point, 1, "symlink", sizeof("symlink")) == SaveDataError::SAVE_DATA_ERROR_INTERNAL,
	           "metadata write followed slot symlink")) return false;
	Lumen::Libs::LibKernel::FileSystem::Mount(U"_SaveData/RenamedSlot", U"/savedata0");
	if (!ExpectGetParamSuccess(point, 0, sizeof(Param), expected, "metadata follows directory identity after rename")) return false;
	return ResetMountMapping("metadata cleanup") && RemoveRoot();
}

#endif

#if defined(LUMEN_SAVE_DATA_TEST_HOOK)

bool MemoryParamMatches(const Param& actual, const Param& expected)
{
	const std::array<uint8_t, sizeof(actual.reserved)> zero_reserved {};
	return std::memcmp(actual.title, expected.title, sizeof(actual.title)) == 0 &&
	       std::memcmp(actual.subtitle, expected.subtitle, sizeof(actual.subtitle)) == 0 &&
	       std::memcmp(actual.detail, expected.detail, sizeof(actual.detail)) == 0 &&
	       actual.user_param == expected.user_param && actual.padding == 0 && actual.mtime > 0 &&
	       std::memcmp(actual.reserved, zero_reserved.data(), zero_reserved.size()) == 0;
}

Param MakeMemoryParam(const char* title, uint32_t user_param)
{
	Param param {};
	std::memcpy(param.title, title, std::strlen(title) + 1);
	std::memcpy(param.subtitle, "memory subtitle", sizeof("memory subtitle"));
	std::memcpy(param.detail, "memory detail", sizeof("memory detail"));
	param.user_param = user_param;
	param.padding = 0xfeedbeef;
	param.mtime = -1234567;
	std::memset(param.reserved, 0x7b, sizeof(param.reserved));
	return param;
}

void StoreMemoryLe32(uint8_t* destination, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i) destination[i] = static_cast<uint8_t>(value >> (i * 8u));
}

void StoreMemoryLe64(uint8_t* destination, uint64_t value)
{
	for (unsigned i = 0; i < 8; ++i) destination[i] = static_cast<uint8_t>(value >> (i * 8u));
}

uint64_t HashMemoryBytes(uint64_t hash, const uint8_t* bytes, size_t size)
{
	constexpr uint64_t FNV_PRIME = 1099511628211ull;
	for (size_t i = 0; i < size; ++i)
	{
		hash ^= bytes[i];
		hash *= FNV_PRIME;
	}
	return hash;
}

std::string HexMemoryComponent(std::string_view value)
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

std::string HexMemoryUser(int32_t user_id)
{
	constexpr char HEX[] = "0123456789abcdef";
	const uint32_t value = static_cast<uint32_t>(user_id);
	std::string result(8, '0');
	for (unsigned i = 0; i < 8; ++i) result[7 - i] = HEX[(value >> (i * 4u)) & 0x0fu];
	return result;
}

bool WriteOversizedMemoryContainerFixture(const char* title_id, int32_t user_id, uint32_t slot_id)
{
	constexpr size_t HEADER_SIZE = 64;
	constexpr size_t CAPACITY_LIMIT = 32ull * 1024ull * 1024ull;
	constexpr uint8_t MAGIC[8] = {'L', 'U', 'M', 'E', 'N', 'S', 'D', 'M'};
	const size_t data_size = CAPACITY_LIMIT + 1;
	std::vector<uint8_t> container(HEADER_SIZE + data_size, 0);
	std::memcpy(container.data(), MAGIC, sizeof(MAGIC));
	StoreMemoryLe32(container.data() + 8, 1);
	StoreMemoryLe32(container.data() + 12, HEADER_SIZE);
	StoreMemoryLe64(container.data() + 16, data_size);
	StoreMemoryLe64(container.data() + 24, 1);
	constexpr uint64_t FNV_OFFSET = 1469598103934665603ull;
	uint64_t checksum = HashMemoryBytes(FNV_OFFSET, container.data(), 48);
	checksum = HashMemoryBytes(checksum, container.data() + 56, HEADER_SIZE - 56);
	checksum = HashMemoryBytes(checksum, container.data() + HEADER_SIZE, data_size);
	StoreMemoryLe64(container.data() + 48, checksum);

	const fs::path directory = fs::path("_SaveData") / "sce_memory" /
	                           HexMemoryComponent(title_id) / HexMemoryUser(user_id);
	std::error_code error;
	fs::create_directories(directory, error);
	if (error) return CaseCheck(false, "memory storage", "cannot create oversized fixture directory");
	const fs::path path = directory / (std::string("slot-") + std::to_string(slot_id) + ".bin");
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(reinterpret_cast<const char*>(container.data()),
	             static_cast<std::streamsize>(container.size()));
	output.close();
	return CaseCheck(!output.fail(), "memory storage",
	                 "cannot write oversized checksummed fixture");
}

bool TestSaveDataMemoryValidation()
{
	using namespace Lumen::Libs::SaveData;
	MemorySetup2 setup {};
	setup.user_id = 66;
	setup.memory_size = 64;
	setup.slot_id = 77;
	MemorySetupResult result;
	Fill(result);
	const MemorySetupResult result_before = result;

	alignas(8) std::array<uint8_t, sizeof(MemorySetup2) + 8> misaligned_setup {};
	std::memcpy(misaligned_setup.data() + 1, &setup, sizeof(setup));
	if (!CaseCheck(g_memory_setup2(reinterpret_cast<const MemorySetup2*>(misaligned_setup.data() + 1),
	                              &result) == SAVE_DATA_ERROR_PARAMETER &&
	               SameBytes(result, result_before), "memory validation",
	               "misaligned Setup request changed output")) return false;
	if (!CaseCheck(g_memory_setup2(reinterpret_cast<const MemorySetup2*>(UINTPTR_MAX - 7), &result) ==
	                   SAVE_DATA_ERROR_PARAMETER && SameBytes(result, result_before),
	               "memory validation", "overflowing Setup request changed output")) return false;

	alignas(8) std::array<uint8_t, sizeof(MemorySetupResult) + 8> misaligned_result;
	misaligned_result.fill(CANARY);
	const auto misaligned_result_before = misaligned_result;
	if (!CaseCheck(g_memory_setup2(&setup, reinterpret_cast<MemorySetupResult*>(
	                                      misaligned_result.data() + 1)) == SAVE_DATA_ERROR_PARAMETER &&
	               misaligned_result == misaligned_result_before, "memory validation",
	               "misaligned Setup result was modified")) return false;

	alignas(8) std::array<uint8_t, 96> setup_overlap {};
	std::memcpy(setup_overlap.data(), &setup, sizeof(setup));
	const auto setup_overlap_before = setup_overlap;
	if (!CaseCheck(g_memory_setup2(reinterpret_cast<const MemorySetup2*>(setup_overlap.data()),
	                              reinterpret_cast<MemorySetupResult*>(setup_overlap.data() + 8)) ==
	                   SAVE_DATA_ERROR_PARAMETER && setup_overlap == setup_overlap_before,
	               "memory validation", "overlapping Setup/result mutated the request")) return false;

	MemorySetup2 nested_setup = setup;
	alignas(8) std::array<uint8_t, sizeof(Param) + 8> nested_param {};
	nested_setup.initial_param = reinterpret_cast<const Param*>(nested_param.data() + 1);
	if (!CaseCheck(g_memory_setup2(&nested_setup, &result) == SAVE_DATA_ERROR_PARAMETER &&
	               SameBytes(result, result_before), "memory validation",
	               "misaligned initial Param changed Setup output")) return false;
	nested_setup.initial_param = reinterpret_cast<const Param*>(UINTPTR_MAX - 7);
	if (!CaseCheck(g_memory_setup2(&nested_setup, &result) == SAVE_DATA_ERROR_PARAMETER &&
	               SameBytes(result, result_before), "memory validation",
	               "overflowing initial Param changed Setup output")) return false;
	if (!CaseCheck(g_memory_setup2(&setup, reinterpret_cast<MemorySetupResult*>(UINTPTR_MAX - 7)) ==
	                   SAVE_DATA_ERROR_PARAMETER,
	               "memory validation", "overflowing Setup result was accepted")) return false;

	result = {};
	if (!CaseCheck(g_memory_setup2(&setup, &result) == 0, "memory validation",
	               "validation fixture Setup failed")) return false;

	MemoryGet2 get {};
	get.user_id = setup.user_id;
	get.slot_id = setup.slot_id;
	alignas(8) std::array<uint8_t, sizeof(MemoryGet2) + 8> misaligned_get {};
	std::memcpy(misaligned_get.data() + 1, &get, sizeof(get));
	if (!CaseCheck(g_memory_get2(reinterpret_cast<const MemoryGet2*>(misaligned_get.data() + 1)) ==
	                   SAVE_DATA_ERROR_PARAMETER,
	               "memory validation", "misaligned Get request was accepted")) return false;
	if (!CaseCheck(g_memory_get2(reinterpret_cast<const MemoryGet2*>(UINTPTR_MAX - 7)) ==
	                   SAVE_DATA_ERROR_PARAMETER,
	               "memory validation", "overflowing Get request was accepted")) return false;

	alignas(8) std::array<uint8_t, sizeof(MemoryData) + 8> descriptor_storage {};
	get.data = reinterpret_cast<MemoryData*>(descriptor_storage.data() + 1);
	if (!CaseCheck(g_memory_get2(&get) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "misaligned Get Data descriptor was accepted")) return false;
	MemoryData data {};
	data.buffer = UINT64_MAX - 7;
	data.size = 16;
	get.data = &data;
	if (!CaseCheck(g_memory_get2(&get) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "overflowing Get Data span was accepted")) return false;
	data.buffer = reinterpret_cast<uint64_t>(&get);
	data.size = 8;
	if (!CaseCheck(g_memory_get2(&get) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "Get output overlapping its request was accepted")) return false;
	data.buffer = reinterpret_cast<uint64_t>(&data);
	if (!CaseCheck(g_memory_get2(&get) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "Get output overlapping its descriptor was accepted")) return false;

	alignas(8) std::array<uint8_t, sizeof(Param) + 8> param_storage;
	param_storage.fill(CANARY);
	const auto param_storage_before = param_storage;
	get.data = nullptr;
	get.param = reinterpret_cast<Param*>(param_storage.data() + 1);
	if (!CaseCheck(g_memory_get2(&get) == SAVE_DATA_ERROR_PARAMETER &&
	               param_storage == param_storage_before, "memory validation",
	               "misaligned Get Param was modified")) return false;
	get.param = reinterpret_cast<Param*>(UINTPTR_MAX - 7);
	if (!CaseCheck(g_memory_get2(&get) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "overflowing Get Param was accepted")) return false;
	get.param = reinterpret_cast<Param*>(&get);
	if (!CaseCheck(g_memory_get2(&get) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "Get Param overlapping its request was accepted")) return false;
	alignas(8) Param overlapping_outputs;
	Fill(overlapping_outputs);
	data = {};
	data.buffer = reinterpret_cast<uint64_t>(&overlapping_outputs);
	data.size = 16;
	get.data = &data;
	get.param = &overlapping_outputs;
	if (!CaseCheck(g_memory_get2(&get) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "overlapping Get Data/Param outputs were accepted")) return false;

	MemorySet2Prefix set {};
	set.user_id = setup.user_id;
	set.slot_id = setup.slot_id;
	alignas(8) std::array<uint8_t, sizeof(MemorySet2Prefix) + 8> misaligned_set {};
	std::memcpy(misaligned_set.data() + 1, &set, sizeof(set));
	if (!CaseCheck(g_memory_set2(reinterpret_cast<const MemorySet2Prefix*>(misaligned_set.data() + 1)) ==
	                   SAVE_DATA_ERROR_PARAMETER,
	               "memory validation", "misaligned Set request was accepted")) return false;
	if (!CaseCheck(g_memory_set2(reinterpret_cast<const MemorySet2Prefix*>(UINTPTR_MAX - 7)) ==
	                   SAVE_DATA_ERROR_PARAMETER,
	               "memory validation", "overflowing Set request was accepted")) return false;
	set.data = reinterpret_cast<const MemoryData*>(descriptor_storage.data() + 1);
	if (!CaseCheck(g_memory_set2(&set) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "misaligned Set Data descriptor was accepted")) return false;
	data.buffer = UINT64_MAX - 7;
	data.size = 16;
	set.data = &data;
	if (!CaseCheck(g_memory_set2(&set) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "overflowing Set Data span was accepted")) return false;
	set.data = nullptr;
	set.param = reinterpret_cast<const Param*>(param_storage.data() + 1);
	if (!CaseCheck(g_memory_set2(&set) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "misaligned Set Param was accepted")) return false;
	set.param = reinterpret_cast<const Param*>(UINTPTR_MAX - 7);
	if (!CaseCheck(g_memory_set2(&set) == SAVE_DATA_ERROR_PARAMETER, "memory validation",
	               "overflowing Set Param was accepted")) return false;

	MemorySync sync {};
	sync.user_id = setup.user_id;
	sync.slot_id = setup.slot_id;
	alignas(8) std::array<uint8_t, sizeof(MemorySync) + 8> misaligned_sync {};
	std::memcpy(misaligned_sync.data() + 1, &sync, sizeof(sync));
	if (!CaseCheck(g_memory_sync(reinterpret_cast<const MemorySync*>(misaligned_sync.data() + 1)) ==
	                   SAVE_DATA_ERROR_PARAMETER,
	               "memory validation", "misaligned Sync request was accepted")) return false;
	if (!CaseCheck(g_memory_sync(reinterpret_cast<const MemorySync*>(UINTPTR_MAX - 3)) ==
	                   SAVE_DATA_ERROR_PARAMETER,
	               "memory validation", "overflowing Sync request was accepted")) return false;

	std::array<uint8_t, 16> unchanged_data;
	unchanged_data.fill(CANARY);
	Param unchanged_param;
	Fill(unchanged_param);
	data = {};
	data.buffer = reinterpret_cast<uint64_t>(unchanged_data.data());
	data.size = unchanged_data.size();
	get.data = &data;
	get.param = &unchanged_param;
	return CaseCheck(g_memory_get2(&get) == 0 &&
	                 std::all_of(unchanged_data.begin(), unchanged_data.end(),
	                             [](uint8_t value) { return value == 0; }) &&
	                 SameBytes(unchanged_param, Param {}), "memory validation",
	                 "rejected validation cases mutated slot state");
}

bool TestSaveDataMemory()
{
	using namespace Lumen::Libs::SaveData;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_A");
	if (!RemoveRoot()) return false;

	if (!CaseCheck(g_memory_get2(nullptr) == SAVE_DATA_ERROR_NOT_INITIALIZED,
	               "memory lifecycle", "uninitialized did not win over null")) return false;
	if (!CaseCheck(g_initialize3(nullptr) == 0, "memory lifecycle", "Initialize3 failed")) return false;
	if (!CaseCheck(g_memory_get2(nullptr) == SAVE_DATA_ERROR_PARAMETER,
	               "memory lifecycle", "initialized null request was not rejected")) return false;

	MemoryGet2 missing {};
	missing.user_id = 42;
	missing.slot_id = 3;
	if (!CaseCheck(g_memory_get2(&missing) == SAVE_DATA_ERROR_MEMORY_NOT_READY,
	               "memory lifecycle", "missing ready slot returned the wrong error")) return false;
	constexpr uint64_t RESIDENT_LIMIT = 128ull * 1024ull * 1024ull;
	if (!CaseCheck(SaveDataMemoryResidentAdmissionForTest(0, RESIDENT_LIMIT) &&
	               SaveDataMemoryResidentAdmissionForTest(RESIDENT_LIMIT - 1, 1) &&
	               !SaveDataMemoryResidentAdmissionForTest(RESIDENT_LIMIT - 1, 2) &&
	               !SaveDataMemoryResidentAdmissionForTest(RESIDENT_LIMIT + 1, 0),
	               "memory setup", "aggregate resident quota boundary is incorrect")) return false;
	if (!TestSaveDataMemoryValidation()) return false;

	constexpr const char* OVERSIZED_TITLE = "CUSA_TEST_OVERSIZED";
	constexpr int32_t OVERSIZED_USER = 74;
	constexpr uint32_t OVERSIZED_SLOT = 11;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest(OVERSIZED_TITLE);
	if (!RemoveRoot() || !WriteOversizedMemoryContainerFixture(
	                         OVERSIZED_TITLE, OVERSIZED_USER, OVERSIZED_SLOT) ||
	    !CaseCheck(g_initialize3(nullptr) == 0, "memory storage",
	               "oversized fixture initialization failed")) return false;
	MemorySetup2 malformed_setup {};
	malformed_setup.user_id = OVERSIZED_USER;
	malformed_setup.memory_size = 32ull * 1024ull * 1024ull;
	malformed_setup.slot_id = OVERSIZED_SLOT;
	MemorySetupResult malformed_result;
	Fill(malformed_result);
	const MemorySetupResult malformed_before = malformed_result;
	MemoryGet2 malformed_get {};
	malformed_get.user_id = OVERSIZED_USER;
	malformed_get.slot_id = OVERSIZED_SLOT;
	if (!CaseCheck(g_memory_setup2(&malformed_setup, &malformed_result) == SAVE_DATA_ERROR_INTERNAL &&
	               SameBytes(malformed_result, malformed_before) &&
	               g_memory_get2(&malformed_get) == SAVE_DATA_ERROR_MEMORY_NOT_READY,
	               "memory storage", "oversized checksummed container was admitted")) return false;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_A");
	if (!RemoveRoot() || !CaseCheck(g_initialize3(nullptr) == 0, "memory storage",
	                               "post-malformed initialization failed")) return false;

	MemorySetupResult null_result;
	Fill(null_result);
	const MemorySetupResult null_result_before = null_result;
	if (!CaseCheck(g_memory_setup2(nullptr, &null_result) == SAVE_DATA_ERROR_PARAMETER &&
	               SameBytes(null_result, null_result_before), "memory setup", "null setup changed output")) return false;
	MemorySetup2 oversized_setup {};
	oversized_setup.user_id = 42;
	oversized_setup.memory_size = 32ull * 1024ull * 1024ull + 1;
	oversized_setup.slot_id = 99;
	MemorySetupResult oversized_result;
	Fill(oversized_result);
	const MemorySetupResult oversized_before = oversized_result;
	if (!CaseCheck(g_memory_setup2(&oversized_setup, &oversized_result) == SAVE_DATA_ERROR_OUT_OF_MEMORY &&
	               SameBytes(oversized_result, oversized_before), "memory setup",
	               "capacity admission did not fail before output publication")) return false;
	MemorySetup2 advisory_setup {};
	advisory_setup.option = 5;
	advisory_setup.user_id = 41;
	advisory_setup.memory_size = 32ull * 1024ull * 1024ull;
	advisory_setup.icon_memory_size = 0x1c800;
	advisory_setup.slot_id = 2;
	MemoryGet2 advisory_get {};
	advisory_get.user_id = advisory_setup.user_id;
	advisory_get.slot_id = advisory_setup.slot_id;
	if (!CaseCheck(g_memory_get2(&advisory_get) == SAVE_DATA_ERROR_MEMORY_NOT_READY,
	               "memory setup", "observed option fixture was ready before Setup")) return false;
	MemorySetupResult advisory_result;
	Fill(advisory_result);
	const MemorySetupResult empty_setup_result {};
	if (!CaseCheck(g_memory_setup2(&advisory_setup, &advisory_result) == 0 &&
	               SameBytes(advisory_result, empty_setup_result) && g_memory_get2(&advisory_get) == 0,
	               "memory setup",
	               "declared option bits or advisory Icon capacity were rejected")) return false;
	MemorySetup2 busy_invalid_setup = advisory_setup;
	busy_invalid_setup.option = 8;
	busy_invalid_setup.initial_icon = reinterpret_cast<const void*>(uintptr_t {8});
	MemorySetupResult busy_invalid_result;
	Fill(busy_invalid_result);
	const MemorySetupResult busy_invalid_before = busy_invalid_result;
	if (!CaseCheck(g_memory_setup2(&busy_invalid_setup, &busy_invalid_result) == SAVE_DATA_ERROR_BUSY &&
	               SameBytes(busy_invalid_result, busy_invalid_before), "memory setup",
	               "ready-slot BUSY did not precede nested option/Icon validation")) return false;
	for (uint32_t option = 0; option <= 7; ++option)
	{
		MemorySetup2 declared_option_setup {};
		declared_option_setup.option = option;
		declared_option_setup.user_id = 41;
		declared_option_setup.icon_memory_size = option == 0 ? 0 :
		    (option == 7 ? std::numeric_limits<uint64_t>::max() : 4);
		declared_option_setup.slot_id = 100 + option;
		MemorySetupResult declared_option_result;
		Fill(declared_option_result);
		if (!CaseCheck(g_memory_setup2(&declared_option_setup, &declared_option_result) == 0 &&
		               SameBytes(declared_option_result, empty_setup_result), "memory setup",
		               "a declared option combination or advisory Icon size was rejected")) return false;
	}
	MemorySetup2 unknown_option_setup = advisory_setup;
	unknown_option_setup.option = 8;
	unknown_option_setup.slot_id = 4;
	MemorySetupResult unknown_option_result;
	Fill(unknown_option_result);
	const MemorySetupResult unknown_option_before = unknown_option_result;
	if (!CaseCheck(g_memory_setup2(&unknown_option_setup, &unknown_option_result) == SAVE_DATA_ERROR_PARAMETER &&
	               SameBytes(unknown_option_result, unknown_option_before), "memory setup",
	               "unknown option bits did not fail closed")) return false;
	unknown_option_setup.option = 0x80000000u;
	unknown_option_setup.slot_id = 6;
	Fill(unknown_option_result);
	if (!CaseCheck(g_memory_setup2(&unknown_option_setup, &unknown_option_result) == SAVE_DATA_ERROR_PARAMETER &&
	               SameBytes(unknown_option_result, unknown_option_before), "memory setup",
	               "high unknown option bit did not fail closed")) return false;
	MemorySetup2 unlocked_oversized_setup = oversized_setup;
	unlocked_oversized_setup.option = 4;
	unlocked_oversized_setup.slot_id = 7;
	Fill(oversized_result);
	if (!CaseCheck(g_memory_setup2(&unlocked_oversized_setup, &oversized_result) == SAVE_DATA_ERROR_OUT_OF_MEMORY &&
	               SameBytes(oversized_result, oversized_before), "memory setup",
	               "UNLOCK_LIMITATIONS bypassed the host capacity cap")) return false;
	MemorySetup2 initial_icon_setup = advisory_setup;
	initial_icon_setup.option = 0;
	initial_icon_setup.slot_id = 5;
	initial_icon_setup.initial_icon = reinterpret_cast<const void*>(uintptr_t {8});
	MemorySetupResult initial_icon_result;
	Fill(initial_icon_result);
	const MemorySetupResult initial_icon_before = initial_icon_result;
	if (!CaseCheck(g_memory_setup2(&initial_icon_setup, &initial_icon_result) == SAVE_DATA_ERROR_PARAMETER &&
	               SameBytes(initial_icon_result, initial_icon_before), "memory setup",
	               "unqualified initial Icon did not fail closed")) return false;

	// Empty attachments also own a map node, a title key and a full Param.
	// Exercise the independent fixed-state quota rather than relying only on
	// the payload-byte admission limit.
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_A");
	if (!CaseCheck(g_initialize3(nullptr) == 0, "memory setup", "quota session initialization failed"))
		return false;
	MemorySetup2 quota_setup {};
	quota_setup.user_id = 73;
	MemorySetupResult quota_result {};
	for (uint32_t slot_id = 0; slot_id < 1024; ++slot_id)
	{
		quota_setup.slot_id = slot_id;
		if (!CaseCheck(g_memory_setup2(&quota_setup, &quota_result) == 0,
		               "memory setup", "fixed-state slot quota rejected an admitted slot")) return false;
	}
	quota_setup.slot_id = 1024;
	Fill(quota_result);
	const MemorySetupResult quota_before = quota_result;
	if (!CaseCheck(g_memory_setup2(&quota_setup, &quota_result) == SAVE_DATA_ERROR_OUT_OF_MEMORY &&
	               SameBytes(quota_result, quota_before), "memory setup",
	               "fixed-state slot quota did not bound empty attachments")) return false;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_A");
	if (!CaseCheck(g_initialize3(nullptr) == 0, "memory setup", "post-quota initialization failed"))
		return false;

	Param initial_param = MakeMemoryParam("initial memory title", 17);
	MemorySetup2 setup {};
	setup.user_id = 42;
	setup.memory_size = 64;
	setup.initial_param = &initial_param;
	setup.slot_id = 3;
	MemorySetupResult setup_result;
	Fill(setup_result);
	MemorySetupResult expected_setup {};
	if (!CaseCheck(g_memory_setup2(&setup, &setup_result) == 0 && SameBytes(setup_result, expected_setup),
	               "memory setup", "new store result was not deterministic")) return false;
	MemorySetupResult duplicate_result;
	Fill(duplicate_result);
	const MemorySetupResult duplicate_before = duplicate_result;
	if (!CaseCheck(g_memory_setup2(&setup, &duplicate_result) == SAVE_DATA_ERROR_BUSY &&
	               SameBytes(duplicate_result, duplicate_before), "memory setup", "duplicate setup was not atomic")) return false;

	std::array<uint8_t, 16> read_buffer;
	read_buffer.fill(CANARY);
	MemoryData read_data {};
	read_data.buffer = reinterpret_cast<uint64_t>(read_buffer.data());
	read_data.size = read_buffer.size();
	Param read_param;
	Fill(read_param);
	MemoryGet2 get {};
	get.user_id = 42;
	get.data = &read_data;
	get.param = &read_param;
	get.slot_id = 3;
	if (!CaseCheck(g_memory_get2(&get) == 0 &&
	               std::all_of(read_buffer.begin(), read_buffer.end(), [](uint8_t value) { return value == 0; }) &&
	               MemoryParamMatches(read_param, initial_param),
	               "memory get", "new-store zero tail or initial Param mismatch")) return false;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	const long publish_page_size_value = sysconf(_SC_PAGESIZE);
	const size_t publish_page_size = publish_page_size_value > 0 ?
	    static_cast<size_t>(publish_page_size_value) : 0;
	void* publish_data_page = publish_page_size != 0 ?
	    mmap(nullptr, publish_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0) : MAP_FAILED;
	void* publish_param_page = publish_page_size != 0 ?
	    mmap(nullptr, publish_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0) : MAP_FAILED;
	bool publish_atomic = publish_data_page != MAP_FAILED && publish_param_page != MAP_FAILED;
	if (publish_atomic)
	{
		auto* publish_data = static_cast<uint8_t*>(publish_data_page);
		auto* publish_param = static_cast<Param*>(publish_param_page);
		std::memset(publish_data, CANARY, 16);
		std::memset(publish_param, CANARY, sizeof(*publish_param));
		MemoryData publish_descriptor {};
		publish_descriptor.buffer = reinterpret_cast<uint64_t>(publish_data);
		publish_descriptor.size = 16;
		MemoryGet2 publish_get {};
		publish_get.user_id = 42;
		publish_get.data = &publish_descriptor;
		publish_get.param = publish_param;
		publish_get.slot_id = 3;
		g_memory_publish_protect_page = publish_param_page;
		g_memory_publish_protect_size = publish_page_size;
		g_memory_publish_protect_result = -1;
		SetSaveDataMemoryPublishHookForTest(&ProtectMemoryParamForPublishTest);
		const int32_t publish_result = g_memory_get2(&publish_get);
		SetSaveDataMemoryPublishHookForTest(nullptr);
		const int restore_result = mprotect(publish_param_page, publish_page_size, PROT_READ | PROT_WRITE);
		publish_atomic = g_memory_publish_protect_result == 0 &&
		    publish_result == SAVE_DATA_ERROR_PARAMETER && restore_result == 0 &&
		    std::all_of(publish_data, publish_data + 16,
		                [](uint8_t value) { return value == CANARY; }) &&
		    std::all_of(reinterpret_cast<const uint8_t*>(publish_param),
		                reinterpret_cast<const uint8_t*>(publish_param) + sizeof(*publish_param),
		                [](uint8_t value) { return value == CANARY; });
	}
	SetSaveDataMemoryPublishHookForTest(nullptr);
	g_memory_publish_protect_page = nullptr;
	g_memory_publish_protect_size = 0;
	if (publish_data_page != MAP_FAILED) munmap(publish_data_page, publish_page_size);
	if (publish_param_page != MAP_FAILED) munmap(publish_param_page, publish_page_size);
	if (!CaseCheck(publish_atomic, "memory get", "multi-output publication was partial after second lease failure"))
		return false;
#endif

	std::array<uint8_t, 8> boundary_output;
	boundary_output.fill(CANARY);
	MemoryData boundary_data {};
	boundary_data.buffer = reinterpret_cast<uint64_t>(boundary_output.data());
	boundary_data.size = boundary_output.size();
	boundary_data.offset = 60;
	MemoryGet2 boundary_get {};
	boundary_get.user_id = 42;
	boundary_get.data = &boundary_data;
	boundary_get.slot_id = 3;
	const auto boundary_before = boundary_output;
	if (!CaseCheck(g_memory_get2(&boundary_get) == SAVE_DATA_ERROR_PARAMETER && boundary_output == boundary_before,
	               "memory get", "capacity crossing changed output")) return false;
	boundary_data.offset = -1;
	if (!CaseCheck(g_memory_get2(&boundary_get) == SAVE_DATA_ERROR_PARAMETER && boundary_output == boundary_before,
	               "memory get", "negative offset changed output")) return false;
	read_buffer.fill(CANARY);
	Fill(read_param);
	const auto icon_data_before = read_buffer;
	const Param icon_param_before = read_param;
	MemoryGet2 icon_get = get;
	icon_get.icon = reinterpret_cast<void*>(uintptr_t {8});
	if (!CaseCheck(g_memory_get2(&icon_get) == SAVE_DATA_ERROR_PARAMETER &&
	               read_buffer == icon_data_before && SameBytes(read_param, icon_param_before),
	               "memory get", "unsupported Icon partially published Data or Param")) return false;

	const std::array<uint8_t, 4> write_bytes = {0x10, 0x20, 0x30, 0x40};
	MemoryData write_data {};
	write_data.buffer = reinterpret_cast<uint64_t>(write_bytes.data());
	write_data.size = write_bytes.size();
	write_data.offset = 4;
	Param persisted_param = MakeMemoryParam("persisted memory title", 91);
	MemorySet2Prefix set {};
	set.user_id = 42;
	set.data = &write_data;
	set.param = &persisted_param;
	set.data_num = 0;
	set.slot_id = 3;
	if (!CaseCheck(g_memory_set2(&set) == 0, "memory set", "data+Param transaction failed")) return false;
	MemorySet2Prefix multi = set;
	multi.data_num = 2;
	if (!CaseCheck(g_memory_set2(&multi) == SAVE_DATA_ERROR_PARAMETER,
	               "memory set", "unqualified multi-record path did not fail closed")) return false;
	MemorySet2Prefix icon_set = set;
	const std::array<uint8_t, 4> rejected_bytes = {0x91, 0x92, 0x93, 0x94};
	MemoryData rejected_data = write_data;
	rejected_data.buffer = reinterpret_cast<uint64_t>(rejected_bytes.data());
	Param rejected_param = MakeMemoryParam("must not persist", 333);
	icon_set.data = &rejected_data;
	icon_set.param = &rejected_param;
	icon_set.icon = reinterpret_cast<const void*>(uintptr_t {8});
	if (!CaseCheck(g_memory_set2(&icon_set) == SAVE_DATA_ERROR_PARAMETER,
	               "memory set", "unsupported Icon write did not fail closed transactionally")) return false;

	read_buffer.fill(CANARY);
	Fill(read_param);
	if (!CaseCheck(g_memory_get2(&get) == 0 &&
	               std::all_of(read_buffer.begin(), read_buffer.begin() + 4, [](uint8_t value) { return value == 0; }) &&
	               std::equal(write_bytes.begin(), write_bytes.end(), read_buffer.begin() + 4) &&
	               std::all_of(read_buffer.begin() + 8, read_buffer.end(), [](uint8_t value) { return value == 0; }) &&
	               MemoryParamMatches(read_param, persisted_param),
	               "memory get", "committed bytes or Param mismatch")) return false;
	const Param first_persisted_read = read_param;

	// Set is the durability boundary: discard every in-memory slot without Sync,
	// then attach the same identity again and require the committed state.
	if (!CaseCheck(Lumen::Libs::ResetSessionState(), "memory persistence",
	               "aggregate session reset failed")) return false;
	if (!CaseCheck(g_initialize3(nullptr) == 0 && g_memory_get2(&missing) == SAVE_DATA_ERROR_MEMORY_NOT_READY,
	               "memory persistence", "session reset retained a ready cache")) return false;
	Param ignored_initial = MakeMemoryParam("must not replace durable Param", 222);
	setup.initial_param = &ignored_initial;
	Fill(setup_result);
	expected_setup.existing_size = 8;
	if (!CaseCheck(g_memory_setup2(&setup, &setup_result) == 0 && SameBytes(setup_result, expected_setup),
	               "memory persistence", "logical existing size was not restored")) return false;
	read_buffer.fill(CANARY);
	Fill(read_param);
	if (!CaseCheck(g_memory_get2(&get) == 0 &&
	               std::equal(write_bytes.begin(), write_bytes.end(), read_buffer.begin() + 4) &&
	               MemoryParamMatches(read_param, persisted_param) && read_param.mtime == first_persisted_read.mtime,
	               "memory persistence", "Set did not survive a fresh engine session")) return false;

	MemorySync sync {};
	sync.user_id = 42;
	sync.slot_id = 3;
	MemorySync unsupported_sync = sync;
	unsupported_sync.option = 1;
	if (!CaseCheck(g_memory_sync(&unsupported_sync) == SAVE_DATA_ERROR_PARAMETER && g_memory_sync(&sync) == 0,
	               "memory sync", "option boundary or durable reflush failed")) return false;

	// Reattaching an existing store with a smaller guest-visible capacity must
	// preserve its hidden durable tail across both a partial Set and Sync.
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_A");
	if (!CaseCheck(g_initialize3(nullptr) == 0, "memory persistence",
	               "small-capacity session initialization failed")) return false;
	MemorySetup2 small_setup = setup;
	small_setup.memory_size = 4;
	small_setup.initial_param = nullptr;
	Fill(setup_result);
	expected_setup.existing_size = 8;
	if (!CaseCheck(g_memory_setup2(&small_setup, &setup_result) == 0 &&
	               SameBytes(setup_result, expected_setup), "memory persistence",
	               "smaller attachment did not report the complete durable size")) return false;
	const std::array<uint8_t, 2> prefix_write_bytes = {0xa1, 0xa2};
	MemoryData prefix_write_data {};
	prefix_write_data.buffer = reinterpret_cast<uint64_t>(prefix_write_bytes.data());
	prefix_write_data.size = prefix_write_bytes.size();
	MemorySet2Prefix prefix_set {};
	prefix_set.user_id = 42;
	prefix_set.data = &prefix_write_data;
	prefix_set.slot_id = 3;
	if (!CaseCheck(g_memory_set2(&prefix_set) == 0 && g_memory_sync(&sync) == 0,
	               "memory persistence", "small-capacity Set or Sync failed")) return false;

	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_A");
	if (!CaseCheck(g_initialize3(nullptr) == 0, "memory persistence",
	               "tail-verification session initialization failed")) return false;
	setup.memory_size = 64;
	setup.initial_param = nullptr;
	Fill(setup_result);
	if (!CaseCheck(g_memory_setup2(&setup, &setup_result) == 0 &&
	               SameBytes(setup_result, expected_setup), "memory persistence",
	               "full-capacity reattachment failed after smaller attachment")) return false;
	read_buffer.fill(CANARY);
	if (!CaseCheck(g_memory_get2(&get) == 0 && read_buffer[0] == prefix_write_bytes[0] &&
	               read_buffer[1] == prefix_write_bytes[1] && read_buffer[2] == 0 && read_buffer[3] == 0 &&
	               std::equal(write_bytes.begin(), write_bytes.end(), read_buffer.begin() + 4),
	               "memory persistence", "smaller attachment truncated the durable tail")) return false;

	MemoryData empty_data {};
	empty_data.offset = 64;
	MemoryGet2 empty_get {};
	empty_get.user_id = 42;
	empty_get.data = &empty_data;
	empty_get.slot_id = 3;
	if (!CaseCheck(g_memory_get2(&empty_get) == 0, "memory get", "zero-sized null buffer was rejected")) return false;

	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_B");
	if (!CaseCheck(g_initialize3(nullptr) == 0, "memory identity", "second title initialization failed")) return false;
	setup.initial_param = nullptr;
	Fill(setup_result);
	expected_setup = MemorySetupResult {};
	if (!CaseCheck(g_memory_setup2(&setup, &setup_result) == 0 && SameBytes(setup_result, expected_setup),
	               "memory identity", "title identity leaked a prior store")) return false;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_A");
	if (!CaseCheck(g_initialize3(nullptr) == 0, "memory identity", "original title reinitialization failed")) return false;
	Fill(setup_result);
	expected_setup.existing_size = 8;
	if (!CaseCheck(g_memory_setup2(&setup, &setup_result) == 0 && SameBytes(setup_result, expected_setup),
	               "memory identity", "original title store was not isolated and recoverable")) return false;

	// A failed first commit has no durable generation to reload. Reconciliation
	// must preserve the pre-transaction attachment, including initial Param, so
	// a subsequent Sync can retry it.
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_RECONCILE_NEW");
	if (!RemoveRoot() || !CaseCheck(g_initialize3(nullptr) == 0, "memory reconciliation",
	                               "new-store fixture initialization failed")) return false;
	Param retry_param = MakeMemoryParam("retry initial Param", 515);
	MemorySetup2 retry_setup {};
	retry_setup.user_id = 87;
	retry_setup.memory_size = 16;
	retry_setup.initial_param = &retry_param;
	retry_setup.slot_id = 8;
	MemorySetupResult retry_result {};
	if (!CaseCheck(g_memory_setup2(&retry_setup, &retry_result) == 0 &&
	               SetSaveDataMemoryStorageIndeterminateForTest(retry_setup.user_id, retry_setup.slot_id),
	               "memory reconciliation", "cannot create first-commit retry fixture")) return false;
	Param retry_read;
	Fill(retry_read);
	MemoryGet2 retry_get {};
	retry_get.user_id = retry_setup.user_id;
	retry_get.param = &retry_read;
	retry_get.slot_id = retry_setup.slot_id;
	if (!CaseCheck(g_memory_get2(&retry_get) == 0 && MemoryParamMatches(retry_read, retry_param),
	               "memory reconciliation", "failed first commit discarded initial Param")) return false;
	MemorySync retry_sync {};
	retry_sync.user_id = retry_setup.user_id;
	retry_sync.slot_id = retry_setup.slot_id;
	if (!CaseCheck(g_memory_sync(&retry_sync) == 0, "memory reconciliation",
	               "first commit could not be retried")) return false;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_RECONCILE_NEW");
	retry_setup.initial_param = nullptr;
	if (!CaseCheck(g_initialize3(nullptr) == 0 &&
	               g_memory_setup2(&retry_setup, &retry_result) == 0,
	               "memory reconciliation", "retried first commit could not be attached")) return false;
	Fill(retry_read);
	if (!CaseCheck(g_memory_get2(&retry_get) == 0 && MemoryParamMatches(retry_read, retry_param),
	               "memory reconciliation", "retried initial Param was not durable")) return false;

	// If an uncertain commit is reconciled after the host store disappeared,
	// the cached generation and payload must reset to the durable absence so a
	// subsequent Set can create generation one instead of looping INTERNAL.
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_RECONCILE");
	if (!RemoveRoot() || !CaseCheck(g_initialize3(nullptr) == 0, "memory reconciliation",
	                               "fixture initialization failed")) return false;
	MemorySetup2 reconcile_setup {};
	reconcile_setup.user_id = 88;
	reconcile_setup.memory_size = 16;
	reconcile_setup.slot_id = 9;
	MemorySetupResult reconcile_result {};
	if (!CaseCheck(g_memory_setup2(&reconcile_setup, &reconcile_result) == 0,
	               "memory reconciliation", "fixture setup failed")) return false;
	const uint8_t old_reconcile_value = 0x55;
	MemoryData reconcile_write {};
	reconcile_write.buffer = reinterpret_cast<uint64_t>(&old_reconcile_value);
	reconcile_write.size = 1;
	MemorySet2Prefix reconcile_set {};
	reconcile_set.user_id = reconcile_setup.user_id;
	reconcile_set.data = &reconcile_write;
	reconcile_set.slot_id = reconcile_setup.slot_id;
	if (!CaseCheck(g_memory_set2(&reconcile_set) == 0 &&
	               SetSaveDataMemoryStorageIndeterminateForTest(
	                   reconcile_setup.user_id, reconcile_setup.slot_id) && RemoveRoot(),
	               "memory reconciliation", "cannot create indeterminate missing-store fixture")) return false;
	uint8_t reconciled_value = CANARY;
	MemoryData reconcile_read {};
	reconcile_read.buffer = reinterpret_cast<uint64_t>(&reconciled_value);
	reconcile_read.size = 1;
	MemoryGet2 reconcile_get {};
	reconcile_get.user_id = reconcile_setup.user_id;
	reconcile_get.data = &reconcile_read;
	reconcile_get.slot_id = reconcile_setup.slot_id;
	if (!CaseCheck(g_memory_get2(&reconcile_get) == 0 && reconciled_value == 0,
	               "memory reconciliation", "missing durable store retained stale cached state")) return false;
	const uint8_t new_reconcile_value = 0x66;
	reconcile_write.buffer = reinterpret_cast<uint64_t>(&new_reconcile_value);
	if (!CaseCheck(g_memory_set2(&reconcile_set) == 0, "memory reconciliation",
	               "missing durable generation could not be recreated")) return false;
	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest("CUSA_TEST_RECONCILE");
	if (!CaseCheck(g_initialize3(nullptr) == 0 &&
	               g_memory_setup2(&reconcile_setup, &reconcile_result) == 0,
	               "memory reconciliation", "recreated generation could not be attached")) return false;
	reconciled_value = CANARY;
	if (!CaseCheck(g_memory_get2(&reconcile_get) == 0 && reconciled_value == new_reconcile_value,
	               "memory reconciliation", "recreated generation was not durable")) return false;

	ResetSessionState();
	SetSaveDataMemoryTitleIdForTest(nullptr);
	if (!RemoveRoot()) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	return RunSaveDataMemoryGenerationRace();
#else
	return true;
#endif
}

#endif

} // namespace

#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
namespace {
struct SaveDataHandoffProbe
{
	uint64_t publish_address = 0, deny_address = 0;
	const void* publication = nullptr;
	size_t publication_size = 0;
	unsigned reads = 0, writes = 0, publications = 0, denials = 0;
} g_legacy_handoff;

bool LegacySaveDataHandoff(uint64_t address, uint64_t bytes, Lumen::GuestCpu::MemoryAccessIntent intent) noexcept
{
	if (Lumen::GuestCpu::WritesMemory(intent))
	{
		++g_legacy_handoff.writes;
		if (address == g_legacy_handoff.deny_address)
		{
			++g_legacy_handoff.denials;
			return false;
		}
	} else
	{
		++g_legacy_handoff.reads;
		if (address == g_legacy_handoff.publish_address && g_legacy_handoff.publication != nullptr &&
		    bytes == g_legacy_handoff.publication_size)
		{
			std::memcpy(reinterpret_cast<void*>(address), g_legacy_handoff.publication, static_cast<size_t>(bytes));
			g_legacy_handoff.publication = nullptr;
			++g_legacy_handoff.publications;
		}
	}
	return true;
}

bool TestLegacySaveDataGuestHandoff()
{
	using namespace Lumen::GuestCpu;
	if (!Check(RegisterMemoryHandoffQuery(LegacySaveDataHandoff), "cannot install isolated handoff test provider"))
		return false;
	PublishMemoryHandoffGpuDirty(true);
	struct Arena
	{
		MountInputs mount;
		MountResult mounted;
		MountPoint point;
		char title[128];
		Param param;
		size_t got_size;
		SearchCondition condition;
		OutputState output;
		Info info;
	} arena {};
	MarkHandoffRange(reinterpret_cast<uint64_t>(&arena), sizeof(arena));
	const DirName published_name = MakeName("PublishedSlot");
	for (const auto api: {MountApi::Mount, MountApi::Mount2})
	{
		if (!ResetMountMapping("guest handoff mount") || !RemoveRoot()) return false;
		PrepareMountInputs(arena.mount, "StaleInput", 6);
		Fill(arena.mounted);
		const MountResult before = arena.mounted;
		g_legacy_handoff = {};
		g_legacy_handoff.publish_address = reinterpret_cast<uint64_t>(&arena.mount.dir_name);
		g_legacy_handoff.publication = &published_name;
		g_legacy_handoff.publication_size = sizeof(published_name);
		if (!Check(InvokeMount(api, arena.mount, &arena.mounted) == 0 &&
		           g_legacy_handoff.publications == 1 && g_legacy_handoff.reads >= 3 &&
		           g_legacy_handoff.writes != 0 && fs::is_directory("_SaveData/PublishedSlot") &&
		           !fs::exists("_SaveData/StaleInput") && MappingTargetsSlot("PublishedSlot", "guest handoff mount"),
		           "mount bypassed request/directory/result handoff")) return false;
		MountResult expected = before;
		std::memset(expected.mount_point.data, 0, sizeof(expected.mount_point.data));
		std::memcpy(expected.mount_point.data, "/savedata0", sizeof("/savedata0"));
		expected.required_blocks = 0;
		expected.mount_status = 1;
		if (!Check(SameBytes(arena.mounted, expected), "leased mount changed reserved bytes")) return false;
	}
	arena.point = MakeMountPoint("/savedata0");
	std::memset(arena.title, 0, sizeof(arena.title));
	std::memcpy(arena.title, "stale", sizeof("stale"));
	const std::array<char, 128> published_title = [] {
		std::array<char, 128> value {};
		std::memcpy(value.data(), "Published title", sizeof("Published title"));
		return value;
	}();
	g_legacy_handoff.publish_address = reinterpret_cast<uint64_t>(arena.title);
	g_legacy_handoff.publication = published_title.data();
	g_legacy_handoff.publication_size = published_title.size();
	if (!Check(g_set_param(&arena.point, 1, arena.title, sizeof(arena.title)) == 0 &&
	           g_legacy_handoff.publications == 2, "SetParam bypassed its input snapshot handoff")) return false;
	Fill(arena.param);
	arena.got_size = SIZE_MAX;
	const Param param_before = arena.param;
	g_legacy_handoff.deny_address = reinterpret_cast<uint64_t>(&arena.got_size);
	if (!Check(g_get_param(&arena.point, 0, &arena.param, sizeof(arena.param), &arena.got_size) ==
	               SaveDataError::SAVE_DATA_ERROR_PARAMETER && SameBytes(arena.param, param_before) &&
	           arena.got_size == SIZE_MAX && g_legacy_handoff.denials == 1,
	           "GetParam published partial bytes after second-output handoff denial")) return false;
	g_legacy_handoff.deny_address = 0;
	if (!Check(g_get_param(&arena.point, 0, &arena.param, sizeof(arena.param), &arena.got_size) == 0 &&
	           arena.got_size == sizeof(arena.param) &&
	           std::memcmp(arena.param.title, published_title.data(), published_title.size()) == 0,
	           "GetParam did not publish the prepared metadata snapshot")) return false;
	arena.condition = MakeCondition();
	for (unsigned denied_output = 0; denied_output != 4; ++denied_output)
	{
		arena.output.Prepare(1);
		const OutputState before = arena.output;
		const std::array<uint64_t, 4> destinations {
		    reinterpret_cast<uint64_t>(&arena.output.result),
		    reinterpret_cast<uint64_t>(arena.output.names.records.data()),
		    reinterpret_cast<uint64_t>(arena.output.params.records.data()),
		    reinterpret_cast<uint64_t>(arena.output.infos.records.data())};
		g_legacy_handoff.deny_address = destinations[denied_output];
		const unsigned denials_before = g_legacy_handoff.denials;
		if (!Check(g_search(&arena.condition, &arena.output.result) == SaveDataError::SAVE_DATA_ERROR_PARAMETER &&
		           SameBytes(arena.output, before) && g_legacy_handoff.denials == denials_before + 1,
		           "search published partial outputs after a mapping handoff denial")) return false;
	}
	g_legacy_handoff.deny_address = 0;
	if (!Check(g_search(&arena.condition, &arena.output.result) == 0 && arena.output.result.total == 1 &&
	           arena.output.result.written == 1 &&
	           std::memcmp(arena.output.names.records[0].data, published_name.data, sizeof(published_name)) == 0,
	           "leased search did not retain its effective array lengths")) return false;
	Fill(arena.info);
	Info expected_info = arena.info;
	expected_info.blocks = expected_info.free_blocks = 100000;
	const unsigned writes_before = g_legacy_handoff.writes;
	if (!Check(g_mount_info(&arena.point, &arena.info) == 0 && SameBytes(arena.info, expected_info) &&
	           g_legacy_handoff.writes == writes_before + 1,
	           "mount-info bypassed handoff or modified reserved bytes")) return false;

	const long page_size_value = sysconf(_SC_PAGESIZE);
	if (!Check(page_size_value > 0, "cannot query guest memory rights fixture page size")) return false;
	const size_t page_size = static_cast<size_t>(page_size_value);
	void* page = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (!Check(page != MAP_FAILED, "cannot create guest memory rights fixture")) return false;
	std::memset(page, CANARY, page_size);
	bool rights_ok = mprotect(page, page_size, PROT_READ) == 0;
	PrepareMountInputs(arena.mount, "DeniedSlot", 6);
	rights_ok = rights_ok && g_mount2(&arena.mount.mount2, static_cast<MountResult*>(page)) ==
	    SaveDataError::SAVE_DATA_ERROR_PARAMETER && !fs::exists("_SaveData/DeniedSlot");
	Fill(arena.param);
	const Param rights_param_before = arena.param;
	rights_ok = rights_ok && g_get_param(&arena.point, 0, &arena.param, sizeof(arena.param),
	    static_cast<size_t*>(page)) == SaveDataError::SAVE_DATA_ERROR_PARAMETER &&
	    SameBytes(arena.param, rights_param_before);
	rights_ok = rights_ok && mprotect(page, page_size, PROT_NONE) == 0;
	Fill(arena.mounted);
	const MountResult result_before = arena.mounted;
	rights_ok = rights_ok && g_mount2(static_cast<const Mount2Request*>(page), &arena.mounted) ==
	    SaveDataError::SAVE_DATA_ERROR_PARAMETER && SameBytes(arena.mounted, result_before);
	munmap(page, page_size);
	if (!Check(rights_ok, "invalid/read-only SaveData memory was accessed or caused filesystem effects")) return false;
	g_legacy_handoff = {};
	std::puts("SaveData guest handoff: real Core leases; input publication, multi-output denial, rights and reserved bytes=PASS");
	return ResetMountMapping("guest handoff cleanup") && RemoveRoot();
}
} // namespace
#endif

int main(int argc, char** argv)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	const bool metadata_child = argc == 3 && std::strcmp(argv[1], "--metadata-child") == 0;
	if (!Check(argc == 2 || metadata_child, "expected the build-owned fixture parent argument")) return 1;
	g_test_executable = fs::absolute(argv[0]).string();
#else
	if (!Check(argc == 2, "expected the build-owned fixture parent argument")) return 1;
#endif
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Libs::LibKernel::FileSystem::FileSystemSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (!TestBindings()) return 1;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (metadata_child) return MetadataChild(argv[2]) ? 0 : 1;
#endif
	Workspace workspace;
	if (!workspace.Init(fs::path(argv[1]))) return 1;
	if (!TestValidMountModes() || !TestMountNamePolicyIsNarrow() || !TestMountExistsAndNotFound() ||
	    !TestMountFilesystemShapes() ||
	    !TestMountParameterPreflight())
		return 1;
	if (!TestRootShapes()) return 1;
	Catalog catalog;
	if (!CreateCatalog(catalog) || !TestFiltersAndOrdering(catalog) || !TestCapacityAndOutputs(catalog) ||
	    !TestParameterErrors() || !TestOverlaps(catalog) || !TestReadOnlyFixture(catalog) ||
	    !TestInvalidUtf8Entry() || !TestSetParamValidation() || !TestGetParamValidation()) return 1;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	if (!TestPrivateMetadataPersistence()) return 1;
#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
	if (!TestMetadataCatalogIdentity()) return 1;
#endif
#endif

#if defined(LUMEN_SAVE_DATA_TEST_HOOK)
	if (!TestSaveDataMemory()) return 1;
#endif
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(LUMEN_FEXCORE_RUNTIME)
	if (!TestLegacySaveDataGuestHandoff()) return 1;
#endif
	std::puts("SaveDataTest: PASS");
	return 0;
}
