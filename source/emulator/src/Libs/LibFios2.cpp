#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/GuestFile.h"

#include <cinttypes>  // For PRId64
#include <array>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <map>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("Fios2", 1, "Fios2", 1, 1);

namespace Fios2 {

// Safe string helper: reject pointers in the null guard region (below 64KB)
// The game sometimes passes invalid pointers like 0x64 (field offset from NULL struct)
// which are non-NULL but unreadable. Our VEH would silently redirect the read,
// corrupting CRT internal state and causing heap corruption (0xc0000374).
static inline const char* safe_str(const char* s)
{
	return (s && reinterpret_cast<uintptr_t>(s) > 0x10000) ? s : "(null/invalid)";
}

// Error codes
constexpr int SCE_FIOS_OK = 0;
constexpr int SCE_FIOS_ERROR_BAD_HANDLE = -2142633983;
constexpr int SCE_FIOS_ERROR_NOT_FOUND = -2142633981;
constexpr int SCE_FIOS_ERROR_BAD_PTR = static_cast<int>(0x80820006u);
constexpr int SCE_FIOS_ERROR_BAD_INDEX = static_cast<int>(0x8082001cu);
constexpr int SCE_FIOS_ERROR_EVENT_NOT_HANDLED = static_cast<int>(0x8082001du);

using SceFiosIOFilterCallback = LUMEN_SYSV_ABI int (*)(void* context, int event, void* parameters);

static int InvokeIOFilterCallback(SceFiosIOFilterCallback callback, void* context, int event, void* parameters)
{
#if defined(LUMEN_EMU_HOST_ARM64)
	if (Loader::IsNativeHostCallTarget(reinterpret_cast<uint64_t>(callback))) return callback(context, event, parameters);
	return static_cast<int>(Loader::RunGuestX64(reinterpret_cast<uint64_t>(callback), reinterpret_cast<uint64_t>(context),
	                                            static_cast<uint64_t>(static_cast<int64_t>(event)),
	                                            reinterpret_cast<uint64_t>(parameters)));
#else
	return callback(context, event, parameters);
#endif
}

struct SceFiosIOFilterEntry
{
	SceFiosIOFilterCallback callback = nullptr;
	void*                   context  = nullptr;
};

static std::mutex                             g_io_filters_mutex;
static std::array<SceFiosIOFilterEntry, 256> g_io_filters;

// Normalize FIOS2 paths: //app0/ -> /app0/, //data/ -> /data/, etc.
static String NormalizeFios2Path(const char* path)
{
	if (path == nullptr) return String();
	String s = String::FromUtf8(path);
	// FIOS2 uses // prefix for virtual paths, but mount points use single /
	if (s.Size() >= 2 && s.At(0) == U'/' && s.At(1) == U'/')
	{
		s = s.RemoveAt(0); // Remove the extra leading /
	}
	return s;
}

// FIOS2 file handle management
// FIOS2 uses its own file handles separate from kernel file descriptors
struct Fios2FileHandle
{
	Core::File file;
	String path;
	String real_path;
	bool is_open;
	int64_t size;
	// Directory listing support
	int64_t dir_read_index;
	Vector<Core::File::DirEntry> dir_entries;
};

static std::mutex g_fios2_handles_mutex;
static std::map<int64_t, Fios2FileHandle*> g_fios2_handles;
static int64_t g_next_fios2_handle = 100; // Start at 100 to distinguish from kernel FDs

void ResetSessionState()
{
	{
		std::lock_guard<std::mutex> lock(g_fios2_handles_mutex);
		for (auto& [id, handle]: g_fios2_handles)
		{
			(void)id;
			if (handle != nullptr)
			{
				if (handle->is_open)
				{
					handle->file.Close();
					handle->is_open = false;
				}
				delete handle;
			}
		}
		g_fios2_handles.clear();
		g_next_fios2_handle = 100;
	}

	std::lock_guard<std::mutex> filter_lock(g_io_filters_mutex);
	for (auto& filter: g_io_filters)
	{
		filter = {};
	}
}

static int64_t AllocateFios2Handle()
{
	std::lock_guard<std::mutex> lock(g_fios2_handles_mutex);
	return g_next_fios2_handle++;
}

static Fios2FileHandle* GetFios2Handle(int64_t fh)
{
	std::lock_guard<std::mutex> lock(g_fios2_handles_mutex);
	auto it = g_fios2_handles.find(fh);
	if (it != g_fios2_handles.end())
	{
		return it->second;
	}
	return nullptr;
}

static void RegisterFios2Handle(int64_t fh, Fios2FileHandle* handle)
{
	std::lock_guard<std::mutex> lock(g_fios2_handles_mutex);
	g_fios2_handles[fh] = handle;
}

static void FreeFios2Handle(int64_t fh)
{
	std::lock_guard<std::mutex> lock(g_fios2_handles_mutex);
	auto it = g_fios2_handles.find(fh);
	if (it != g_fios2_handles.end())
	{
		delete it->second;
		g_fios2_handles.erase(it);
	}
}

// Opaque types
using SceFiosOp = int64_t;
using SceFiosOpCallback = void*;

struct SceFiosParams
{
	uint32_t initialized : 1;
	uint32_t paramsSize : 15;
	uint32_t pathMax : 16;
	uint32_t profiling;
	void* opStorage;
	uint32_t opStorageSize;
	void* chunkStorage;
	uint32_t chunkStorageSize;
	void* fhStorage;
	uint32_t fhStorageSize;
	void* dhStorage;
	uint32_t dhStorageSize;
	void* pCache;
	void* pVprintf;
	void* pMemcpy;
	int threadAffinity[4];
	int threadPriority[4];
	uint32_t memoryAlign;
	uint32_t minAutoBufferSize;
	int stackSize[4];
};

int LUMEN_SYSV_ABI sceFiosInitialize(const SceFiosParams* params)
{
	PRINT_NAME();
	
	printf("\t params = %p\n", params);
	if (params)
	{
		printf("\t   paramsSize = %u\n", params->paramsSize);
		printf("\t   pathMax = %u\n", params->pathMax);
	}
	
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosTerminate()
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosIsInitialized(void* params)
{
	PRINT_NAME();
	return 1; // FIOS2 is initialized
}

int LUMEN_SYSV_ABI sceFiosIOFilterAdd(int index, SceFiosIOFilterCallback callback, void* context)
{
	PRINT_NAME();
	printf("\t index = %d, callback = %p, context = %p\n", index, reinterpret_cast<void*>(callback), context);

	if (callback == nullptr)
	{
		return SCE_FIOS_ERROR_BAD_PTR;
	}
	if (index < 0 || index >= static_cast<int>(g_io_filters.size()))
	{
		return SCE_FIOS_ERROR_BAD_INDEX;
	}

	const int init_result = InvokeIOFilterCallback(callback, context, 0, nullptr);
	if (init_result != SCE_FIOS_OK && init_result != SCE_FIOS_ERROR_EVENT_NOT_HANDLED)
	{
		return init_result;
	}

	bool occupied = false;
	{
		std::lock_guard<std::mutex> lock(g_io_filters_mutex);
		auto&                       entry = g_io_filters[static_cast<size_t>(index)];
		if (entry.callback != nullptr)
		{
			occupied = true;
		} else
		{
			entry = {callback, context};
		}
	}
	if (occupied)
	{
		InvokeIOFilterCallback(callback, context, 1, nullptr);
		return SCE_FIOS_ERROR_BAD_INDEX;
	}
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosIOFilterRemove(int index)
{
	PRINT_NAME();
	printf("\t index = %d\n", index);
	if (index < 0 || index >= static_cast<int>(g_io_filters.size()))
	{
		return SCE_FIOS_ERROR_BAD_INDEX;
	}

	SceFiosIOFilterEntry entry;
	{
		std::lock_guard<std::mutex> lock(g_io_filters_mutex);
		auto&                       registered = g_io_filters[static_cast<size_t>(index)];
		if (registered.callback == nullptr)
		{
			return SCE_FIOS_ERROR_BAD_INDEX;
		}
		entry      = registered;
		registered = {};
	}

	const int result = InvokeIOFilterCallback(entry.callback, entry.context, 1, nullptr);
	return (result == SCE_FIOS_ERROR_EVENT_NOT_HANDLED ? SCE_FIOS_OK : result);
}

int LUMEN_SYSV_ABI sceFiosIOFilterGetInfo(int index, SceFiosIOFilterCallback* callback, void** context)
{
	PRINT_NAME();
	if (index < 0 || index >= static_cast<int>(g_io_filters.size()))
	{
		return SCE_FIOS_ERROR_BAD_INDEX;
	}

	std::lock_guard<std::mutex> lock(g_io_filters_mutex);
	const auto&                 entry = g_io_filters[static_cast<size_t>(index)];
	if (entry.callback == nullptr)
	{
		return SCE_FIOS_ERROR_BAD_INDEX;
	}
	if (callback != nullptr)
	{
		*callback = entry.callback;
	}
	if (context != nullptr)
	{
		*context = entry.context;
	}
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosUpdateParameters(const SceFiosParams* params)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

void LUMEN_SYSV_ABI sceFiosSetGlobalDefaultOpAttr(void* attr)
{
	PRINT_NAME();
}

int LUMEN_SYSV_ABI sceFiosGetGlobalDefaultOpAttr(void* attr)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

// Overlay functions
int LUMEN_SYSV_ABI sceFiosOverlayAdd(void* overlay, int* outId)
{
	PRINT_NAME();
	if (outId) *outId = 1; // Dummy ID
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosOverlayRemove(int id)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

// Archive functions
int LUMEN_SYSV_ABI sceFiosArchiveGetMountBufferSize(void* archive, const char* path, void* params)
{
	PRINT_NAME();
	return 0;
}

int LUMEN_SYSV_ABI sceFiosArchiveMount(void* archive, const char* path, void* mount, void* buffer, uint32_t bufferSize)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosArchiveUnmount(void* archive)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

// Debug name functions
const char* LUMEN_SYSV_ABI sceFiosDebugDumpError(int error, char* buffer, uint32_t bufferSize)
{
	PRINT_NAME();
	return "FIOS_STUB_ERROR";
}

// Date/Time functions
int LUMEN_SYSV_ABI sceFiosDateToComponents(int64_t date, void* components)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

// Op functions
int LUMEN_SYSV_ABI sceFiosOpGetActualCount(SceFiosOp op)
{
	PRINT_NAME();
	return 0;
}

int LUMEN_SYSV_ABI sceFiosOpGetRequestCount(SceFiosOp op)
{
	PRINT_NAME();
	return 0;
}

bool LUMEN_SYSV_ABI sceFiosOpIsDone(SceFiosOp op)
{
	PRINT_NAME();
	return true; // Always done since we're stubbing
}

int LUMEN_SYSV_ABI sceFiosOpWait(SceFiosOp op)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosOpDelete(SceFiosOp op)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosOpCancel(SceFiosOp op)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosOpGetError(SceFiosOp op)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

int64_t LUMEN_SYSV_ABI sceFiosOpSyncWait(SceFiosOp op)
{
	PRINT_NAME();
	return 0;
}

// DCache functions
void LUMEN_SYSV_ABI sceFiosDCacheFlush(void)
{
	PRINT_NAME();
}

void LUMEN_SYSV_ABI sceFiosDCacheDisable(void)
{
	PRINT_NAME();
}

// File exists
bool LUMEN_SYSV_ABI sceFiosExistsSync(void* attr, const char* path)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	
	if (path == nullptr)
	{
		return false;
	}
	
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	bool exists = Core::File::IsFileExisting(real_path) || Core::File::IsDirectoryExisting(real_path);
	printf("\t real_path = %s, exists = %s\n", real_path.C_Str(), exists ? "true" : "false");
	
	return exists;
}

// Directory exists
bool LUMEN_SYSV_ABI sceFiosDirectoryExistsSync(void* attr, const char* path)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	
	if (path == nullptr)
	{
		return false;
	}
	
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	bool exists = Core::File::IsDirectoryExisting(real_path);
	printf("\t real_path = %s, exists = %s\n", real_path.C_Str(), exists ? "true" : "false");
	
	return exists;
}

// File stat
int LUMEN_SYSV_ABI sceFiosStatSync(void* attr, const char* path, void* status)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	
	if (path == nullptr || status == nullptr)
	{
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	// SceFiosStat structure (simplified):
	// offset 0: int64_t fileSize
	// offset 8: int64_t accessDate (in microseconds since epoch)
	// offset 16: int64_t modificationDate
	// offset 24: int64_t creationDate
	// offset 32: uint32_t statFlags (bit 0 = is_file, bit 1 = is_directory)
	// offset 36: uint32_t reserved
	// offset 40: int64_t uid   
	// Total size ~48 bytes
	
	bool is_file = Core::File::IsFileExisting(real_path);
	bool is_dir = Core::File::IsDirectoryExisting(real_path);
	
	if (!is_file && !is_dir)
	{
		printf("\t NOT FOUND: real_path = %s\n", real_path.C_Str());
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	// Zero out the status structure
	memset(status, 0, 48);
	
	int64_t file_size = 0;
	if (is_file)
	{
		Core::File f;
		if (f.Open(real_path, Core::File::Mode::Read))
		{
			file_size = f.Size();
			f.Close();
		}
	}
	
	// Fill in the stat structure
	auto* stat_bytes = static_cast<uint8_t*>(status);
	*reinterpret_cast<int64_t*>(stat_bytes + 0) = file_size;  // fileSize
	// dates left as 0
	uint32_t flags = 0;
	if (is_file) flags |= 1;   // bit 0 = regular file
	if (is_dir)  flags |= 2;   // bit 1 = directory
	*reinterpret_cast<uint32_t*>(stat_bytes + 32) = flags;
	
	printf("\t SUCCESS: real_path = %s, size = %" PRId64 ", flags = 0x%x\n", real_path.C_Str(), file_size, flags);
	
	return SCE_FIOS_OK;
}

// Get suspend count
uint64_t LUMEN_SYSV_ABI sceFiosGetSuspendCount(void)
{
	PRINT_NAME();
	return 0;
}

// Suspend/Resume
int LUMEN_SYSV_ABI sceFiosSuspend(void)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosResume(void)
{
	PRINT_NAME();
	return SCE_FIOS_OK;
}

// Path operations
int LUMEN_SYSV_ABI sceFiosPathNormalize(const char* path, char* outPath, uint32_t outPathSize)
{
	PRINT_NAME();
	if (path != nullptr && outPath != nullptr && outPathSize > 0)
	{
		size_t len = strlen(path);
		if (len >= outPathSize) len = outPathSize - 1;
		memcpy(outPath, path, len);
		outPath[len] = '\0';
	}
	return SCE_FIOS_OK;
}

// File handle functions
bool LUMEN_SYSV_ABI sceFiosFHIsValidHandle(int64_t fh)
{
	PRINT_NAME();
	auto* handle = GetFios2Handle(fh);
	return (handle != nullptr && handle->is_open);
}

// File read/write sync
int LUMEN_SYSV_ABI sceFiosFHReadSync(void* attr, int64_t fh, void* data, int64_t size)
{
	PRINT_NAME();
	printf("\t fh = %" PRId64 ", size = %" PRId64 "\n", fh, size);
	
	auto* handle = GetFios2Handle(fh);
	if (handle == nullptr || !handle->is_open)
	{
		printf("\t ERROR: Invalid handle!\n");
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	if (size < 0 || static_cast<uint64_t>(size) > UINT32_MAX)
	{
		return SCE_FIOS_ERROR_BAD_PTR;
	}
	const auto read_result = GuestFile::Read(handle->file, data, static_cast<uint32_t>(size));
	if (read_result.error != GuestFile::ReadError::None)
	{
		return SCE_FIOS_ERROR_BAD_PTR;
	}
	
	printf("\t SUCCESS: bytes_read = %u\n", read_result.bytes_read);
	return static_cast<int>(read_result.bytes_read);
}

int LUMEN_SYSV_ABI sceFiosFHWriteSync(void* attr, int64_t fh, const void* data, int64_t size)
{
	PRINT_NAME();
	return SCE_FIOS_ERROR_BAD_HANDLE;
}

int LUMEN_SYSV_ABI sceFiosFHCloseSync(void* attr, int64_t fh)
{
	PRINT_NAME();
	printf("\t fh = %" PRId64 "\n", fh);
	
	auto* handle = GetFios2Handle(fh);
	if (handle != nullptr && handle->is_open)
	{
		handle->file.Close();
		handle->is_open = false;
	}
	FreeFios2Handle(fh);
	
	return SCE_FIOS_OK;
}

int64_t LUMEN_SYSV_ABI sceFiosFHOpenSync(void* attr, int64_t* outFh, const char* path, void* params)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	
	if (outFh) *outFh = -1;
	
	if (path == nullptr)
	{
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	// Translate the mounted path to a real filesystem path
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	printf("\t real_path = %s\n", real_path.C_Str());
	
	// Create a new FIOS2 file handle
	auto* handle = new Fios2FileHandle();
	handle->path = mounted_path;
	handle->real_path = real_path;
	handle->is_open = false;
	handle->size = 0;
	
	// Try to open the file
	if (!handle->file.Open(real_path, Core::File::Mode::Read))
	{
		printf("\t ERROR: Failed to open file!\n");
		delete handle;
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	handle->is_open = true;
	handle->size = handle->file.Size();
	
	// Allocate a FIOS2 handle
	int64_t fh = AllocateFios2Handle();
	RegisterFios2Handle(fh, handle);
	
	if (outFh) *outFh = fh;
	
	printf("\t SUCCESS: fh = %" PRId64 ", size = %" PRId64 "\n", fh, handle->size);
	
	return SCE_FIOS_OK;
}

int64_t LUMEN_SYSV_ABI sceFiosFHSeek(int64_t fh, int64_t offset, int whence)
{
	PRINT_NAME();
	printf("\t fh = %" PRId64 ", offset = %" PRId64 ", whence = %d\n", fh, offset, whence);
	
	auto* handle = GetFios2Handle(fh);
	if (handle == nullptr || !handle->is_open)
	{
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	// Calculate absolute position based on whence
	uint64_t new_pos = 0;
	switch (whence)
	{
		case 0: // SEEK_SET - from beginning
			new_pos = static_cast<uint64_t>(offset);
			break;
		case 1: // SEEK_CUR - from current position
			new_pos = handle->file.Tell() + static_cast<uint64_t>(offset);
			break;
		case 2: // SEEK_END - from end
			new_pos = handle->size - static_cast<uint64_t>(-offset);
			break;
		default:
			return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	if (!handle->file.Seek(new_pos))
	{
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	return static_cast<int64_t>(handle->file.Tell());
}

int64_t LUMEN_SYSV_ABI sceFiosFHTell(int64_t fh)
{
	PRINT_NAME();
	
	auto* handle = GetFios2Handle(fh);
	if (handle == nullptr || !handle->is_open)
	{
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	return handle->file.Tell();
}

int64_t LUMEN_SYSV_ABI sceFiosFHGetSize(int64_t fh)
{
	PRINT_NAME();
	printf("\t fh = %" PRId64 "\n", fh);
	
	auto* handle = GetFios2Handle(fh);
	if (handle == nullptr || !handle->is_open)
	{
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	printf("\t SUCCESS: size = %" PRId64 "\n", handle->size);
	return handle->size;
}

// File read/write (non-FH) - reads entire file by path
int64_t LUMEN_SYSV_ABI sceFiosFileReadSync(void* attr, const char* path, void* data, int64_t size)
{
	PRINT_NAME();
	printf("\t path = %s, size = %" PRId64 "\n", safe_str(path), size);
	
	if (path == nullptr || data == nullptr)
	{
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	Core::File f;
	if (!f.Open(real_path, Core::File::Mode::Read))
	{
		printf("\t ERROR: Failed to open file: %s\n", real_path.C_Str());
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	if (size < 0 || static_cast<uint64_t>(size) > UINT32_MAX)
	{
		f.Close();
		return SCE_FIOS_ERROR_BAD_PTR;
	}
	int64_t    file_size   = f.Size();
	int64_t    read_size   = (size < file_size) ? size : file_size;
	const auto read_result = GuestFile::Read(f, data, static_cast<uint32_t>(read_size));
	f.Close();
	if (read_result.error != GuestFile::ReadError::None)
	{
		return SCE_FIOS_ERROR_BAD_PTR;
	}
	
	printf("\t SUCCESS: read %" PRIu32 " bytes from %s\n", read_result.bytes_read, real_path.C_Str());
	return static_cast<int64_t>(read_result.bytes_read);
}

int64_t LUMEN_SYSV_ABI sceFiosFileGetSizeSync(void* attr, const char* path, int64_t* outSize)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	
	if (path == nullptr)
	{
		if (outSize) *outSize = 0;
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	Core::File f;
	if (!f.Open(real_path, Core::File::Mode::Read))
	{
		printf("\t ERROR: Failed to open file: %s\n", real_path.C_Str());
		if (outSize) *outSize = 0;
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	int64_t file_size = f.Size();
	f.Close();
	
	if (outSize) *outSize = file_size;
	printf("\t SUCCESS: size = %" PRId64 " for %s\n", file_size, real_path.C_Str());
	return SCE_FIOS_OK;
}

// Directory operations
int LUMEN_SYSV_ABI sceFiosDHOpenSync(void* attr, int64_t* outDh, const char* path, void* buffer, uint32_t bufSize)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	
	if (outDh) *outDh = -1;
	
	if (path == nullptr)
	{
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	if (!Core::File::IsDirectoryExisting(real_path))
	{
		printf("\t ERROR: Directory not found: %s\n", real_path.C_Str());
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	// Get directory entries
	auto entries = Core::File::GetDirEntries(real_path);
	
	// Store as a FIOS2 directory handle (reuse file handle infrastructure)
	auto* handle = new Fios2FileHandle();
	handle->path = mounted_path;
	handle->real_path = real_path;
	handle->is_open = true;
	handle->size = static_cast<int64_t>(entries.Size());
	handle->dir_read_index = 0;
	handle->dir_entries = entries;
	
	int64_t dh = AllocateFios2Handle();
	RegisterFios2Handle(dh, handle);
	
	if (outDh) *outDh = dh;
	
	printf("\t SUCCESS: dh = %" PRId64 ", entries = %" PRId64 "\n", dh, handle->size);
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosDHCloseSync(void* attr, int64_t dh)
{
	PRINT_NAME();
	printf("\t dh = %" PRId64 "\n", dh);
	
	auto* handle = GetFios2Handle(dh);
	if (handle != nullptr)
	{
		handle->is_open = false;
		handle->dir_entries.Clear();
	}
	FreeFios2Handle(dh);
	
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosDHReadSync(void* attr, int64_t dh, void* entry)
{
	PRINT_NAME();
	
	auto* handle = GetFios2Handle(dh);
	if (handle == nullptr || !handle->is_open)
	{
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	if (handle->dir_read_index >= static_cast<int64_t>(handle->dir_entries.Size()))
	{
		// No more entries
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	// SceFiosDirEntry structure:
	// offset 0: SceFiosStat (48 bytes) - file status
	// offset 48: char filename[256] - entry name
	// Total: 304 bytes
	auto* entry_bytes = static_cast<uint8_t*>(entry);
	memset(entry_bytes, 0, 304);
	
	const auto& dir_entry = handle->dir_entries.At(handle->dir_read_index);
	handle->dir_read_index++;
	
	// Set stat flags
	uint32_t flags = 0;
	if (dir_entry.is_file) flags |= 1;
	else flags |= 2; // directory
	*reinterpret_cast<uint32_t*>(entry_bytes + 32) = flags;
	
	// Copy filename
	auto name_utf8 = dir_entry.name.utf8_str();
	strncpy(reinterpret_cast<char*>(entry_bytes + 48), name_utf8.GetData(), 255);
	
	printf("\t entry[%" PRId64 "]: %s (%s)\n", handle->dir_read_index - 1, name_utf8.GetData(), dir_entry.is_file ? "file" : "dir");
	
	return SCE_FIOS_OK;
}

// Open a file through the PSN opaque FIOS interface.
int64_t LUMEN_SYSV_ABI sceFiosPsnOpaqueOpenSync(void* attr, int64_t* outFh, const char* path, void* params)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	
	if (outFh) *outFh = -1;
	
	if (path == nullptr)
	{
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	
	// Translate the mounted path to a real filesystem path
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	printf("\t real_path = %s\n", real_path.C_Str());
	
	// Create a new FIOS2 file handle
	auto* handle = new Fios2FileHandle();
	handle->path = mounted_path;
	handle->real_path = real_path;
	handle->is_open = false;
	handle->size = 0;
	
	// Try to open the file
	if (!handle->file.Open(real_path, Core::File::Mode::Read))
	{
		printf("\t ERROR: Failed to open file!\n");
		delete handle;
		return SCE_FIOS_ERROR_NOT_FOUND;
	}
	
	handle->is_open = true;
	handle->size = handle->file.Size();
	
	// Allocate a FIOS2 handle
	int64_t fh = AllocateFios2Handle();
	RegisterFios2Handle(fh, handle);
	
	if (outFh) *outFh = fh;
	
	printf("\t SUCCESS: fh = %" PRId64 ", size = %" PRId64 "\n", fh, handle->size);
	
	return SCE_FIOS_OK;
}

// Other sync functions
bool LUMEN_SYSV_ABI sceFiosFileExistsSync(void* attr, const char* path)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	
	if (path == nullptr)
	{
		return false;
	}
	
	// Translate the mounted path to a real filesystem path
	String mounted_path = NormalizeFios2Path(path);
	String real_path = LibKernel::FileSystem::GetRealFilename(mounted_path);
	
	bool exists = Core::File::IsFileExisting(real_path);
	printf("\t real_path = %s, exists = %s\n", real_path.C_Str(), exists ? "true" : "false");
	
	return exists;
}

int LUMEN_SYSV_ABI sceFiosFileDeleteSync(void* attr, const char* path)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	return SCE_FIOS_ERROR_BAD_HANDLE;
}

int LUMEN_SYSV_ABI sceFiosDirectoryCreateSync(void* attr, const char* path)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	return SCE_FIOS_OK;
}

int LUMEN_SYSV_ABI sceFiosRenameSync(void* attr, const char* oldPath, const char* newPath)
{
	PRINT_NAME();
	return SCE_FIOS_ERROR_BAD_HANDLE;
}

int LUMEN_SYSV_ABI sceFiosResolveSync(void* attr, const char* path, char* out, uint32_t outSize)
{
	PRINT_NAME();
	printf("\t path = %s\n", safe_str(path));
	if (out && outSize > 0 && path)
	{
		strncpy(out, path, outSize - 1);
		out[outSize - 1] = '\0';
	}
	return SCE_FIOS_OK;
}

// sceFiosOpGetBuffer - NID -q4M5WX0Jrs
// Gets the buffer associated with an operation
// According to ps4libdoc: SceFiosOp -> void*
void* LUMEN_SYSV_ABI sceFiosOpGetBuffer(SceFiosOp op)
{
	PRINT_NAME();
	printf("\t op = %" PRId64 "\n", op);
	return nullptr; // No buffer for stubbed ops
}

// sceFiosOpGetOffset - NID aKxpgAMSJ04
// Gets the file offset associated with an operation
// According to ps4libdoc: SceFiosOp -> int64_t
int64_t LUMEN_SYSV_ABI sceFiosOpGetOffset(SceFiosOp op)
{
	PRINT_NAME();
	printf("\t op = %" PRId64 "\n", op);
	return 0; // Return 0 offset for stubbed ops
}

// sceFiosOpSyncWaitForIO - NID 2wvqS7Odb6M  
// Waits for I/O portion of operation to complete
// According to ps4libdoc: SceFiosOp -> int
int LUMEN_SYSV_ABI sceFiosOpSyncWaitForIO(SceFiosOp op)
{
	PRINT_NAME();
	printf("\t op = %" PRId64 "\n", op);
	return SCE_FIOS_OK; // Immediately done for stubbed ops
}

// sceFiosOpReschedule - NID 3HAgZPl1v+4 (CRITICAL - called just before stack_chk_fail)
// Reschedules an operation with new priority
// According to ps4libdoc: (SceFiosOp, int64_t, SceFiosOpAttr*) -> int
int LUMEN_SYSV_ABI sceFiosOpReschedule(SceFiosOp op, int64_t newDeadline, void* pAttr)
{
	PRINT_NAME();
	printf("\t op = %" PRId64 ", newDeadline = %" PRId64 ", pAttr = %p\n", op, newDeadline, pAttr);
	return SCE_FIOS_OK; // Success - op is rescheduled (no-op for stubs)
}

// sceFiosFHPreadSync - NID rR8wq7YFRZs
// Positioned read from file handle (synchronous)
// According to ps4libdoc: (SceFiosOpAttr*, SceFiosFH, void*, int64_t, int64_t) -> int64_t
int64_t LUMEN_SYSV_ABI sceFiosFHPreadSync(void* pAttr, int64_t fh, void* pBuf, int64_t length, int64_t offset)
{
	PRINT_NAME();
	printf("\t pAttr = %p, fh = %" PRId64 ", pBuf = %p, length = %" PRId64 ", offset = %" PRId64 "\n", 
		   pAttr, fh, pBuf, length, offset);

	auto* handle = GetFios2Handle(fh);
	if (handle == nullptr || !handle->is_open)
	{
		printf("\t ERROR: Invalid handle!\n");
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}

	if (pBuf == nullptr || length < 0 || static_cast<uint64_t>(length) > UINT32_MAX || offset < 0)
	{
		return SCE_FIOS_ERROR_BAD_PTR;
	}

	const auto read_result =
	    GuestFile::Pread(handle->file, pBuf, static_cast<uint32_t>(length), static_cast<uint64_t>(offset));
	if (read_result.error == GuestFile::ReadError::InvalidAddress)
	{
		return SCE_FIOS_ERROR_BAD_PTR;
	}
	if (read_result.error == GuestFile::ReadError::SeekFailed)
	{
		printf("\t ERROR: Seek failed!\n");
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}
	if (read_result.error == GuestFile::ReadError::ReadFailed)
	{
		printf("\t ERROR: Read failed!\n");
		return SCE_FIOS_ERROR_BAD_HANDLE;
	}

	printf("\t SUCCESS: bytes_read = %u\n", read_result.bytes_read);
	return static_cast<int64_t>(read_result.bytes_read);
}

// sceFiosFHGetPath - NID SnoQQWnGK9I
// Gets the path of an open file handle
// According to ps4libdoc: SceFiosFH -> const char*
const char* LUMEN_SYSV_ABI sceFiosFHGetPath(int64_t fh)
{
	PRINT_NAME();
	printf("\t fh = %" PRId64 "\n", fh);
	auto* handle = GetFios2Handle(fh);
	if (handle != nullptr)
	{
		return handle->path.C_Str();
	}
	return "";
}

// sceFiosOpGetPath - NID +FRvKknUj1I
// Gets the path associated with an operation
// According to ps4libdoc: SceFiosOp -> const char*
const char* LUMEN_SYSV_ABI sceFiosOpGetPath(SceFiosOp op)
{
	PRINT_NAME();
	printf("\t op = %" PRId64 "\n", op);
	return ""; // Empty path for stubbed ops
}

} // namespace Fios2

LIB_DEFINE(InitFios2_1)
{
	printf("[DEBUG] InitFios2_1 called!\n");
	fflush(stdout);
	
	// Main initialization
	LIB_FUNC("wAKZ-det+yo", Fios2::sceFiosInitialize);
	LIB_FUNC("rLGWPRHqGVQ", Fios2::sceFiosTerminate);
	LIB_FUNC("deCGJZ5rvuI", Fios2::sceFiosIsInitialized);
	
	// IO Filter functions
	LIB_FUNC("J3cOHDN7ZFQ", Fios2::sceFiosIOFilterAdd);
	LIB_FUNC("lgITuBsRo2o", Fios2::sceFiosIOFilterAdd);  // alternate NID (SDK version)
	LIB_FUNC("6Tw7WXLCi3I", Fios2::sceFiosIOFilterRemove);
	LIB_FUNC("3jM-WoVKfRc", Fios2::sceFiosIOFilterGetInfo);
	
	// Parameters
	LIB_FUNC("bvofhI3kNYs", Fios2::sceFiosUpdateParameters);
	LIB_FUNC("jqPdV2Zzs10", Fios2::sceFiosSetGlobalDefaultOpAttr);
	LIB_FUNC("1-cYWvPkKnA", Fios2::sceFiosGetGlobalDefaultOpAttr);
	
	// Overlay
	LIB_FUNC("FbcMpvFKNVY", Fios2::sceFiosOverlayAdd);
	LIB_FUNC("Bz73LBsGjqY", Fios2::sceFiosOverlayRemove);
	
	// Archive
	LIB_FUNC("lJq8ZGR0cSE", Fios2::sceFiosArchiveGetMountBufferSize);
	LIB_FUNC("ERgBYSXv3lY", Fios2::sceFiosArchiveMount);
	LIB_FUNC("mZIg2hY-QJg", Fios2::sceFiosArchiveUnmount);
	
	// Debug
	LIB_FUNC("vJZZwRRgpOo", Fios2::sceFiosDebugDumpError);
	
	// Date/Time
	LIB_FUNC("q2VY-1OGutA", Fios2::sceFiosDateToComponents);
	
	// Op functions
	LIB_FUNC("WkJLAKePpDI", Fios2::sceFiosOpGetActualCount);
	LIB_FUNC("eeYLIhbW8uw", Fios2::sceFiosOpGetRequestCount);
	LIB_FUNC("aUI+qxR9Nqk", Fios2::sceFiosOpIsDone);
	LIB_FUNC("MqVl5L5GvQo", Fios2::sceFiosOpWait);
	LIB_FUNC("Dg5K4c+YTIY", Fios2::sceFiosOpDelete);
	LIB_FUNC("VaxWwKqRPPA", Fios2::sceFiosOpCancel);
	LIB_FUNC("W8ZCnq+hOWE", Fios2::sceFiosOpGetError);
	LIB_FUNC("W1Am+pA4Bno", Fios2::sceFiosOpSyncWait);
	
	// DCache
	LIB_FUNC("Nd5zl9Uqnr8", Fios2::sceFiosDCacheFlush);
	LIB_FUNC("KX4MMGjoTKk", Fios2::sceFiosDCacheDisable);
	
	// Exists/Stat
	LIB_FUNC("py5-xVWMcVU", Fios2::sceFiosExistsSync);
	LIB_FUNC("UJ85udDa6LQ", Fios2::sceFiosDirectoryExistsSync);
	LIB_FUNC("cL+2yjH+GXE", Fios2::sceFiosStatSync);
	
	// Suspend
	LIB_FUNC("wWv7FJK+yOE", Fios2::sceFiosGetSuspendCount);
	LIB_FUNC("URVYOTP7-DE", Fios2::sceFiosSuspend);
	LIB_FUNC("8N0PmMk6Giw", Fios2::sceFiosResume);
	
	// Path
	LIB_FUNC("Q6ZwR0G7Wsc", Fios2::sceFiosPathNormalize);
	
	// File handle functions
	LIB_FUNC("TXABsmiiqto", Fios2::sceFiosFHIsValidHandle);
	LIB_FUNC("VFOjcbSp+rg", Fios2::sceFiosFHReadSync);
	LIB_FUNC("8GuqkOKtzzM", Fios2::sceFiosFHWriteSync);
	LIB_FUNC("F5H3F+gTJPk", Fios2::sceFiosFHCloseSync);
	LIB_FUNC("-TE0x5RSCQI", Fios2::sceFiosFHOpenSync);
	LIB_FUNC("Gm6cTUZrOeQ", Fios2::sceFiosFHSeek);
	LIB_FUNC("t8+i9o7p30Q", Fios2::sceFiosFHTell);
	LIB_FUNC("oRAJ7Qu6sT8", Fios2::sceFiosFHGetSize);
	
	// File read/write
	LIB_FUNC("3gSAbAmH2s4", Fios2::sceFiosFileReadSync);
	LIB_FUNC("3qWowPt5g6s", Fios2::sceFiosFileGetSizeSync);
	
	// Directory operations
	LIB_FUNC("K2dHsVy9mAo", Fios2::sceFiosDHOpenSync);
	LIB_FUNC("Rnc7KkZgQ+4", Fios2::sceFiosDHCloseSync);
	LIB_FUNC("-3EYNmk4H1E", Fios2::sceFiosDHReadSync);
	
	// PSN-related
	LIB_FUNC("b44anV2D7K0", Fios2::sceFiosPsnOpaqueOpenSync);
	
	// Other sync functions
	LIB_FUNC("e+5-7Fwe22E", Fios2::sceFiosFileExistsSync);
	LIB_FUNC("waoKiGNRBcw", Fios2::sceFiosFileDeleteSync);
	LIB_FUNC("MlxvzNFN9FI", Fios2::sceFiosDirectoryCreateSync);
	LIB_FUNC("4Gv0oMcQ3LU", Fios2::sceFiosRenameSync);
	LIB_FUNC("VE6cMfpSQ4g", Fios2::sceFiosResolveSync);
	
	// FIOS2 functions with correct signatures (previously unknown)
	LIB_FUNC("-q4M5WX0Jrs", Fios2::sceFiosOpGetBuffer);
	LIB_FUNC("aKxpgAMSJ04", Fios2::sceFiosOpGetOffset);
	LIB_FUNC("2wvqS7Odb6M", Fios2::sceFiosOpSyncWaitForIO);
	LIB_FUNC("3HAgZPl1v+4", Fios2::sceFiosOpReschedule);
	LIB_FUNC("rR8wq7YFRZs", Fios2::sceFiosFHPreadSync);
	LIB_FUNC("SnoQQWnGK9I", Fios2::sceFiosFHGetPath);
	LIB_FUNC("+FRvKknUj1I", Fios2::sceFiosOpGetPath);
	LIB_FUNC("AOujSGqU+ms", Fios2::sceFiosFHCloseSync);

	// Alternate NIDs (correct SDK version hashes)
	LIB_FUNC("aKWnlLIobGA", Fios2::sceFiosIsInitialized);
	LIB_FUNC("ahIXyuwF0-o", Fios2::sceFiosIOFilterRemove);
	LIB_FUNC("gHxxOQxKW3E", Fios2::sceFiosIOFilterGetInfo);
	LIB_FUNC("l4OVTpJCyQI", Fios2::sceFiosUpdateParameters);
	LIB_FUNC("2JtEQrjeUR4", Fios2::sceFiosSetGlobalDefaultOpAttr);
	LIB_FUNC("xiTqbFc7w+w", Fios2::sceFiosGetGlobalDefaultOpAttr);
	LIB_FUNC("MuMnDaXBTm0", Fios2::sceFiosOverlayRemove);
	LIB_FUNC("ERmiOK9VT0g", Fios2::sceFiosArchiveGetMountBufferSize);
	LIB_FUNC("pIU8u6VsLM8", Fios2::sceFiosArchiveMount);
	LIB_FUNC("YfTBBU5nONQ", Fios2::sceFiosArchiveUnmount);
	LIB_FUNC("8BLW2WiSIuI", Fios2::sceFiosDebugDumpError);
	LIB_FUNC("Rm+hiwvSnxw", Fios2::sceFiosDateToComponents);
	LIB_FUNC("51jH2C+it2s", Fios2::sceFiosOpGetRequestCount);
	LIB_FUNC("5cyEcilO-J0", Fios2::sceFiosOpDelete);
	LIB_FUNC("r-cf2mbp9DI", Fios2::sceFiosDCacheFlush);
	LIB_FUNC("crDYY81sPb4", Fios2::sceFiosDCacheDisable);
	LIB_FUNC("2ZaHWy3IhKQ", Fios2::sceFiosExistsSync);
	LIB_FUNC("OOuvHKTu4Oc", Fios2::sceFiosDirectoryExistsSync);
	LIB_FUNC("jayvY07C5dk", Fios2::sceFiosStatSync);
	LIB_FUNC("Lxgze5-E3uQ", Fios2::sceFiosGetSuspendCount);
	LIB_FUNC("tMYHEVj4j+o", Fios2::sceFiosSuspend);
	LIB_FUNC("vKL406KPJRs", Fios2::sceFiosResume);
	LIB_FUNC("M8m6+Xnz9Ds", Fios2::sceFiosPathNormalize);
	LIB_FUNC("TjZehu+tLSc", Fios2::sceFiosFHIsValidHandle);
	LIB_FUNC("Bn2ZF4ZjeuQ", Fios2::sceFiosFHReadSync);
	LIB_FUNC("Kl-TbrDU9YM", Fios2::sceFiosFHWriteSync);
	LIB_FUNC("xReSebwKApA", Fios2::sceFiosFHSeek);
	LIB_FUNC("MrRFrdgpsx8", Fios2::sceFiosFHTell);
	LIB_FUNC("FdjoqFQOlt0", Fios2::sceFiosFHGetSize);
	LIB_FUNC("kVMxSiYD6tc", Fios2::sceFiosFileReadSync);
	LIB_FUNC("zF8-CRvRXnM", Fios2::sceFiosFileGetSizeSync);
	LIB_FUNC("GGqucL9F+YI", Fios2::sceFiosDHOpenSync);
	LIB_FUNC("0-p4O8FINmU", Fios2::sceFiosDHCloseSync);
	LIB_FUNC("odjOGg8harg", Fios2::sceFiosDHReadSync);
	LIB_FUNC("Bi1MyH3ConE", Fios2::sceFiosPsnOpaqueOpenSync);
	LIB_FUNC("NwOHMRM2Ppw", Fios2::sceFiosFileExistsSync);
	LIB_FUNC("bDupEgbQ6Fk", Fios2::sceFiosFileDeleteSync);
	LIB_FUNC("nWuza0ZdfqA", Fios2::sceFiosDirectoryCreateSync);
	LIB_FUNC("G-39lsdSgXo", Fios2::sceFiosRenameSync);
	LIB_FUNC("oiK6kLAtAIE", Fios2::sceFiosResolveSync);
	LIB_FUNC("52bk-Ucb5s0", Fios2::sceFiosOpGetBuffer);
	LIB_FUNC("J9ujmUpwWAc", Fios2::sceFiosOpGetOffset);
	LIB_FUNC("nP-LabYxHbk", Fios2::sceFiosOpSyncWaitForIO);
	LIB_FUNC("DX1JP95j3Gg", Fios2::sceFiosOpReschedule);
	LIB_FUNC("2m9+Opco-hk", Fios2::sceFiosFHPreadSync);
	LIB_FUNC("Mif6IzBa0hc", Fios2::sceFiosFHGetPath);
	LIB_FUNC("4fZAv0vbycQ", Fios2::sceFiosOpGetPath);
	LIB_FUNC("ZzE6WN+QBbE", Fios2::sceFiosFHCloseSync);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
