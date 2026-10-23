#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Kernel/GuestFile.h"

#include "FileSystemPath.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DateTime.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"
#include "../../../cpu/runtime/MemoryHandoff.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
#include "Emulator/Loader/RuntimeLinker.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::FileSystem {

LIB_NAME("libkernel", "libkernel");

constexpr int DESCRIPTOR_MIN = 3;

class MountPoints
{
public:
	struct MountPair
	{
		String dir;
		String point;
	};

	MountPoints() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~MountPoints() { Clear(); }

	LUMEN_CLASS_NO_COPY(MountPoints);

	void Mount(const String& folder, const String& point);
	void Umount(const String& folder_or_point);
	void Clear();

	[[nodiscard]] String GetRealFilename(const String& mounted_file_name);
	[[nodiscard]] String GetRealDirectory(const String& mounted_directory);

private:
	Vector<MountPair> m_mount_pairs;
	Core::Mutex       m_mutex;
};

struct File
{
	Core::File                   f;
	String                       name;
	String                       real_name;
	std::atomic_bool             opened;
	std::atomic_bool             directory;
	std::atomic_uint32_t         descriptor_flags;
	std::atomic_uint32_t         status_flags;
	Core::Mutex                  mutex;
	Vector<Core::File::DirEntry> dents;
	uint32_t                     dents_index;
};

class FileDescriptors
{
public:
	FileDescriptors() { EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread()); }
	virtual ~FileDescriptors() { CloseAll(); }

	LUMEN_CLASS_NO_COPY(FileDescriptors);

	int   CreateDescriptor();
	void  DeleteDescriptor(int d);
	File* GetFile(int d);
	File* GetFile(const String& real_name);
	void  RenamePath(const String& old_name, const String& new_name, bool directory);
	void  CloseAll();

private:
	Vector<File*> m_files;
	Core::Mutex   m_mutex;
};

constexpr uint32_t GUEST_METADATA_ENTRY_LIMIT = 4096;

struct GuestMetadata
{
	String         real_name;
	bool           has_mode = false;
	uint16_t       mode     = 0;
	bool           has_atim = false;
	KernelTimespec atim {};
	bool           has_mtim = false;
	KernelTimespec mtim {};
	bool           has_ctim = false;
	KernelTimespec ctim {};
};

// chmod/utimes are guest-visible metadata operations. Keeping their state in
// the emulated session avoids changing permissions or timestamps in an
// extracted ROM tree on the host while preserving stat/fstat coherence.
class GuestMetadataOverlay
{
public:
	int SetMode(const String& real_name, uint32_t mode, const KernelTimespec& ctime)
	{
		Core::LockGuard lock(m_mutex);
		auto* entry = FindOrCreateLocked(real_name);
		if (entry == nullptr) return KERNEL_ERROR_ENOMEM;
		entry->has_mode = true;
		entry->mode     = static_cast<uint16_t>(mode & 0007777u);
		entry->has_ctim = true;
		entry->ctim     = ctime;
		return OK;
	}

	int SetTimes(const String& real_name, const KernelTimespec& atime,
	             const KernelTimespec& mtime, const KernelTimespec& ctime)
	{
		Core::LockGuard lock(m_mutex);
		auto* entry = FindOrCreateLocked(real_name);
		if (entry == nullptr) return KERNEL_ERROR_ENOMEM;
		entry->has_atim = true;
		entry->atim     = atime;
		entry->has_mtim = true;
		entry->mtim     = mtime;
		entry->has_ctim = true;
		entry->ctim     = ctime;
		return OK;
	}

	void Apply(const String& real_name, FileStat* stat)
	{
		Core::LockGuard lock(m_mutex);
		const auto index = FindLocked(real_name);
		if (!m_entries.IndexValid(index)) return;
		const auto& entry = m_entries.At(index);
		if (entry.has_mode)
			stat->st_mode = static_cast<uint16_t>((stat->st_mode & ~0007777u) | entry.mode);
		if (entry.has_atim) stat->st_atim = entry.atim;
		if (entry.has_mtim) stat->st_mtim = entry.mtim;
		if (entry.has_ctim) stat->st_ctim = entry.ctim;
	}

	void Erase(const String& real_name, bool directory = false)
	{
		Core::LockGuard lock(m_mutex);
		const String prefix = real_name.FixDirectorySlash();
		for (uint32_t index = m_entries.Size(); index != 0; --index)
		{
			const auto& name = m_entries.At(index - 1).real_name;
			if (name == real_name || (directory && name.StartsWith(prefix)))
				m_entries.RemoveAt(index - 1);
		}
	}

	void Rename(const String& old_name, const String& new_name, bool directory)
	{
		if (old_name == new_name) return;
		Core::LockGuard lock(m_mutex);
		const String old_prefix = old_name.FixDirectorySlash();
		const String new_prefix = new_name.FixDirectorySlash();
		for (uint32_t index = m_entries.Size(); index != 0; --index)
		{
			const auto& name = m_entries.At(index - 1).real_name;
			const bool belongs_to_source = name == old_name || (directory && name.StartsWith(old_prefix));
			const bool belongs_to_destination = name == new_name || (directory && name.StartsWith(new_prefix));
			if (belongs_to_destination && !belongs_to_source) m_entries.RemoveAt(index - 1);
		}
		for (auto& entry: m_entries)
		{
			if (entry.real_name == old_name)
				entry.real_name = new_name;
			else if (directory && entry.real_name.StartsWith(old_prefix))
				entry.real_name = new_prefix + entry.real_name.RemoveFirst(old_prefix.Size());
		}
	}

	void TouchAfterContentChange(const String& real_name, const KernelTimespec& time)
	{
		Core::LockGuard lock(m_mutex);
		const auto index = FindLocked(real_name);
		if (!m_entries.IndexValid(index)) return;
		auto& entry    = m_entries[index];
		entry.has_mtim = true;
		entry.mtim     = time;
		entry.has_ctim = true;
		entry.ctim     = time;
	}

	void Reset()
	{
		Core::LockGuard lock(m_mutex);
		m_entries.Free();
	}

	[[nodiscard]] uint32_t Size()
	{
		Core::LockGuard lock(m_mutex);
		return m_entries.Size();
	}

	[[nodiscard]] uint32_t Capacity()
	{
		Core::LockGuard lock(m_mutex);
		return m_entries.Capacity();
	}

private:
	[[nodiscard]] uint32_t FindLocked(const String& real_name) const
	{
		return m_entries.Find(real_name,
		                      [](const GuestMetadata& entry, const String& name) { return entry.real_name == name; });
	}

	GuestMetadata* FindOrCreateLocked(const String& real_name)
	{
		const auto index = FindLocked(real_name);
		if (m_entries.IndexValid(index)) return &m_entries[index];
		if (m_entries.Size() >= GUEST_METADATA_ENTRY_LIMIT) return nullptr;
		GuestMetadata entry;
		entry.real_name = real_name;
		m_entries.Add(std::move(entry));
		return &m_entries[m_entries.Size() - 1];
	}

	Vector<GuestMetadata> m_entries;
	Core::Mutex           m_mutex;
};

static MountPoints*          g_mount_points = nullptr;
static FileDescriptors*      g_files        = nullptr;
static GuestMetadataOverlay* g_metadata     = nullptr;

static void sec_to_timespec(KernelTimespec* ts, double sec)
{
	ts->tv_sec  = static_cast<int64_t>(sec);
	ts->tv_nsec = static_cast<int64_t>((sec - static_cast<double>(ts->tv_sec)) * 1000000000.0);
}

static KernelTimespec CurrentGuestTime()
{
	const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
	const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
	const auto nanos   = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed - seconds);
	return {seconds.count(), nanos.count()};
}

static int SnapshotGuestPath(const char* path, std::string* snapshot, const char** result)
{
	if (path == nullptr) return KERNEL_ERROR_EFAULT;
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	constexpr size_t MAX_GUEST_PATH_BYTES = 4096;
	if (!Loader::ReadGuestString(reinterpret_cast<uint64_t>(path), MAX_GUEST_PATH_BYTES + 1,
	                             snapshot, false))
		return KERNEL_ERROR_EFAULT;
	if (snapshot->size() > MAX_GUEST_PATH_BYTES) return KERNEL_ERROR_ENAMETOOLONG;
	*result = snapshot->c_str();
#else
	(void)snapshot;
	*result = path;
#endif
	return OK;
}

static int SnapshotGuestTimevals(const KernelTimeval* times, bool* supplied,
	                              std::array<KernelTimeval, 2>* snapshot)
{
	if (times == nullptr)
	{
		*supplied = false;
		return OK;
	}
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease input;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(times), sizeof(*snapshot),
	        Core::VirtualMemory::Mode::Read, &input))
		return KERNEL_ERROR_EFAULT;
#endif
	std::memcpy(snapshot->data(), times, sizeof(*snapshot));
	for (const auto& time: *snapshot)
	{
		if (time.tv_usec < 0 || time.tv_usec > 999999) return KERNEL_ERROR_EINVAL;
	}
	*supplied = true;
	return OK;
}

static int ResolveExistingRealName(const char* path, String* real_name, bool* directory = nullptr)
{
	const String mounted_name   = String::FromUtf8(path);
	const String file_name      = g_mount_points->GetRealFilename(mounted_name);
	const String directory_name = g_mount_points->GetRealDirectory(mounted_name);
	const bool   is_directory   = Core::File::IsDirectoryExisting(file_name) ||
	                            Core::File::IsDirectoryExisting(directory_name);
	const bool is_file = Core::File::IsFileExisting(file_name);
	if (!is_directory && !is_file) return KERNEL_ERROR_ENOENT;
	if (is_directory && is_file) return KERNEL_ERROR_EINVAL;
	*real_name = is_directory ? directory_name : file_name;
	if (directory != nullptr) *directory = is_directory;
	return OK;
}

int FileDescriptors::CreateDescriptor()
{
	Core::LockGuard lock(m_mutex);

	auto* file      = new File {};
	file->opened    = false;
	file->directory = false;
	file->descriptor_flags = 0;
	file->status_flags     = 0;

	int files_num = static_cast<int>(m_files.Size());
	for (int index = 0; index < files_num; index++)
	{
		if (m_files.At(index) == nullptr)
		{
			m_files[index] = file;
			return index + DESCRIPTOR_MIN;
		}
	}

	m_files.Add(file);
	return static_cast<int>(m_files.Size()) + DESCRIPTOR_MIN - 1;
}

void FileDescriptors::DeleteDescriptor(int d)
{
	Core::LockGuard lock(m_mutex);

	auto index = static_cast<uint32_t>(d - DESCRIPTOR_MIN);

	EXIT_IF(!m_files.IndexValid(index));
	EXIT_IF(m_files.At(index) == nullptr);
	EXIT_IF(m_files.At(index)->opened);

	delete m_files.At(index);
	m_files[index] = nullptr;
}

File* FileDescriptors::GetFile(int d)
{
	Core::LockGuard lock(m_mutex);

	if (d < DESCRIPTOR_MIN)
	{
		return nullptr;
	}

	const auto index = static_cast<uint32_t>(d - DESCRIPTOR_MIN);
	if (!m_files.IndexValid(index))
	{
		return nullptr;
	}

	return m_files.At(index);
}

File* FileDescriptors::GetFile(const String& real_name)
{
	Core::LockGuard lock(m_mutex);

	for (auto* f: m_files)
	{
		if (f != nullptr && f->real_name == real_name)
		{
			return f;
		}
	}

	return nullptr;
}

void FileDescriptors::RenamePath(const String& old_name, const String& new_name, bool directory)
{
	Core::LockGuard lock(m_mutex);
	const String old_prefix = old_name.FixDirectorySlash();
	const String new_prefix = new_name.FixDirectorySlash();
	for (auto* file: m_files)
	{
		if (file == nullptr) continue;
		if (file->real_name == old_name)
			file->real_name = new_name;
		else if (directory && file->real_name.StartsWith(old_prefix))
			file->real_name = new_prefix + file->real_name.RemoveFirst(old_prefix.Size());
	}
}

void FileDescriptors::CloseAll()
{
	Core::LockGuard lock(m_mutex);

	for (auto& f: m_files)
	{
		if (f != nullptr)
		{
			if (f->opened)
			{
				f->f.Close();
			}
			delete f;
			f = nullptr;
		}
	}
	m_files.Clear();
}

void MountPoints::Mount(const String& folder, const String& point)
{
	Core::LockGuard lock(m_mutex);

	auto folder_str = folder.FixDirectorySlash();
	auto point_str  = point.FixDirectorySlash();

	Umount(folder_str);
	Umount(point_str);

	MountPair p;
	p.dir   = folder_str;
	p.point = point_str;

	m_mount_pairs.Add(p);
}

void MountPoints::Umount(const String& folder_or_point)
{
	Core::LockGuard lock(m_mutex);

	auto folder_or_point_str = folder_or_point.FixDirectorySlash();

	if (auto index =
	        m_mount_pairs.Find(folder_or_point_str, [](const MountPair& p, const String& s) { return p.dir == s || p.point == s; });
	    m_mount_pairs.IndexValid(index))
	{
		m_mount_pairs.RemoveAt(index);
	}
}

void MountPoints::Clear()
{
	Core::LockGuard lock(m_mutex);
	m_mount_pairs.Clear();
}

String MountPoints::GetRealFilename(const String& mounted_file_name)
{
	Core::LockGuard lock(m_mutex);

	for (const auto& p: m_mount_pairs)
	{
		const auto translation = Path::TranslateMountedPath(mounted_file_name, p.point, p.dir, false);
		if (translation.matched)
		{
			return translation.path;
		}
	}

	return mounted_file_name;
}

String MountPoints::GetRealDirectory(const String& mounted_directory)
{
	Core::LockGuard lock(m_mutex);

	for (const auto& p: m_mount_pairs)
	{
		const auto translation = Path::TranslateMountedPath(mounted_directory, p.point, p.dir, true);
		if (translation.matched)
		{
			return translation.path;
		}
	}

	return mounted_directory;
}

LUMEN_SUBSYSTEM_INIT(FileSystem)
{
	g_mount_points = new MountPoints;
	g_files        = new FileDescriptors;
	g_metadata     = new GuestMetadataOverlay;
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(FileSystem)
{
	if (g_files != nullptr)
	{
		g_files->CloseAll();
	}
	if (g_metadata != nullptr)
	{
		g_metadata->Reset();
	}
}

LUMEN_SUBSYSTEM_DESTROY(FileSystem)
{
	(void)ResetForNextSession();
}

bool ResetForNextSession()
{
	if (g_mount_points == nullptr || g_files == nullptr || g_metadata == nullptr)
	{
		return false;
	}

	g_files->CloseAll();
	g_mount_points->Clear();
	g_metadata->Reset();
	return true;
}

void Mount(const String& folder, const String& point)
{
	EXIT_IF(g_mount_points == nullptr);

	g_mount_points->Mount(folder, point);
}

void Umount(const String& folder_or_point)
{
	EXIT_IF(g_mount_points == nullptr);

	g_mount_points->Umount(folder_or_point);
}

String GetRealFilename(const String& mounted_file_name)
{
	EXIT_IF(g_mount_points == nullptr);

	return g_mount_points->GetRealFilename(mounted_file_name);
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool FileSystemSessionResetContractTest()
{
	MountPoints     mounts;
	FileDescriptors files;
	GuestMetadataOverlay metadata;
	const KernelTimespec timestamp {1, 2};
	for (uint32_t index = 0; index < GUEST_METADATA_ENTRY_LIMIT; ++index)
	{
		const std::string name = "/lumen/metadata/" + std::to_string(index);
		if (metadata.SetMode(String::FromUtf8(name.c_str()), index, timestamp) != OK)
			return false;
	}
	if (metadata.Size() != GUEST_METADATA_ENTRY_LIMIT ||
	    metadata.SetMode(U"/lumen/metadata/full", 0600, timestamp) != KERNEL_ERROR_ENOMEM)
		return false;
	metadata.Reset();
	if (metadata.Size() != 0 || metadata.Capacity() != 0 ||
	    metadata.SetMode(U"/lumen/metadata/reused", 0600, timestamp) != OK)
		return false;
	metadata.Reset();

	for (int cycle = 0; cycle < 2; cycle++)
	{
		mounts.Mount(U"/lumen/session", U"/app0");
		const int descriptor = files.CreateDescriptor();
		if (mounts.GetRealFilename(U"/app0/eboot.bin") == String(U"/app0/eboot.bin") ||
		    descriptor != DESCRIPTOR_MIN || files.GetFile(descriptor) == nullptr || files.GetFile(descriptor + 1) != nullptr ||
		    files.GetFile(DESCRIPTOR_MIN - 1) != nullptr || files.GetFile(INT_MIN) != nullptr || files.GetFile(INT_MAX) != nullptr)
		{
			return false;
		}

		files.DeleteDescriptor(descriptor);
		auto* const previous_files    = g_files;
		auto* const previous_metadata = g_metadata;
		g_files                     = &files;
		g_metadata                  = &metadata;
		FileStat ignored_stat {};
		const bool abi_rejected_invalid_descriptors =
		    KernelFstat(descriptor, &ignored_stat) == KERNEL_ERROR_EBADF && KernelFstat(INT_MAX, &ignored_stat) == KERNEL_ERROR_EBADF;
		g_files    = previous_files;
		g_metadata = previous_metadata;
		if (files.GetFile(descriptor) != nullptr || !abi_rejected_invalid_descriptors)
		{
			return false;
		}

		const int reused_descriptor = files.CreateDescriptor();
		if (reused_descriptor != descriptor || files.GetFile(reused_descriptor) == nullptr)
		{
			return false;
		}

		files.CloseAll();
		if (files.GetFile(reused_descriptor) != nullptr)
		{
			return false;
		}
		mounts.Clear();
		if (mounts.GetRealFilename(U"/app0/eboot.bin") != String(U"/app0/eboot.bin"))
		{
			return false;
		}
	}
	return true;
}
#endif

static int ExclusiveCreateKernelError(int error)
{
	switch (error)
	{
		case EEXIST: return KERNEL_ERROR_EEXIST;
		case ENOENT: return KERNEL_ERROR_ENOENT;
		case ENOTDIR: return KERNEL_ERROR_ENOTDIR;
		case EACCES: return KERNEL_ERROR_EACCES;
		case EPERM: return KERNEL_ERROR_EPERM;
		case EROFS: return KERNEL_ERROR_EROFS;
		case EMFILE: return KERNEL_ERROR_EMFILE;
		case ENFILE: return KERNEL_ERROR_ENFILE;
		case ENOSPC: return KERNEL_ERROR_ENOSPC;
		case ENAMETOOLONG: return KERNEL_ERROR_ENAMETOOLONG;
		case ELOOP: return KERNEL_ERROR_ELOOP;
		case ENOMEM: return KERNEL_ERROR_ENOMEM;
		case EINVAL: return KERNEL_ERROR_EINVAL;
		case EINTR: return KERNEL_ERROR_EINTR;
		default: return KERNEL_ERROR_EIO;
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
int LUMEN_SYSV_ABI KernelOpen(const char* path, int flags, uint16_t mode)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr || g_files == nullptr || g_metadata == nullptr);

	if (path == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	std::string path_snapshot;
	if (!Loader::ReadGuestString(reinterpret_cast<uint64_t>(path),
	                             std::numeric_limits<size_t>::max(), &path_snapshot, false))
		return KERNEL_ERROR_EFAULT;
	path = path_snapshot.c_str();
#endif

	auto flags_u = static_cast<uint32_t>(flags);
	const uint32_t original_flags = flags_u;

	printf("\t path = %s\n", path);
	printf("\t flags = %08" PRIx32 "\n", flags_u);
	printf("\t mode = %04" PRIx16 "\n", mode);

	bool nonblock  = (flags_u & 0x0004u) != 0;
	bool append    = (flags_u & 0x0008u) != 0;
	bool fsync     = (flags_u & 0x0080u) != 0;
	bool sync      = (flags_u & 0x0080u) != 0;
	bool creat     = (flags_u & 0x0200u) != 0;
	bool trunc     = (flags_u & 0x0400u) != 0;
	bool excl      = (flags_u & 0x0800u) != 0;
	bool dsync     = (flags_u & 0x1000u) != 0;
	bool direct    = (flags_u & 0x00010000u) != 0;
	bool directory = (flags_u & 0x00020000u) != 0;
	const bool exclusive_create = excl && creat && !directory;
	constexpr uint32_t known_flags = 0x0003u | 0x0004u | 0x0008u | 0x0080u | 0x0200u | 0x0400u |
	                                 0x0800u | 0x1000u | 0x00010000u | 0x00020000u;
	const bool unsupported_flags = append || fsync || sync || dsync || direct ||
	                               (excl && !exclusive_create) || (creat && !trunc && !exclusive_create) ||
	                               (original_flags & ~known_flags) != 0;

	if (unsupported_flags)
	{
		// PRINT_NAME/printf can be silent during controlled replays. Retain
		// only the first rejected flag set, before descriptor or file effects.
		static std::atomic_bool reported {false};
		if (!reported.exchange(true))
		{
			std::fprintf(stderr,
			             "[KERNEL_OPEN_FLAGS_TERMINAL] flags=%08" PRIx32
			             " access=%u append=%u sync=%u excl=%u dsync=%u direct=%u"
			             " creat=%u trunc=%u directory=%u nonblock=%u admission=unchanged file_effects=0\n",
			             original_flags, original_flags & 0x0003u, static_cast<unsigned>(append),
			             static_cast<unsigned>(sync), static_cast<unsigned>(excl), static_cast<unsigned>(dsync),
			             static_cast<unsigned>(direct), static_cast<unsigned>(creat), static_cast<unsigned>(trunc),
			             static_cast<unsigned>(directory), static_cast<unsigned>(nonblock));
		}
	}
	EXIT_NOT_IMPLEMENTED(unsupported_flags);

	EXIT_NOT_IMPLEMENTED(nonblock && !directory);

	flags_u &= 0x3u;

	Core::File::Mode rw_mode = Core::File::Mode::Read;

	switch (flags_u)
	{
		case 0: rw_mode = Core::File::Mode::Read; break;
		case 1: rw_mode = Core::File::Mode::Write; break;
		case 2: rw_mode = Core::File::Mode::ReadWrite; break;
		default: EXIT("invalid flag_u: %u\n", flags_u);
	}

	EXIT_NOT_IMPLEMENTED(directory && rw_mode != Core::File::Mode::Read);
	EXIT_NOT_IMPLEMENTED(directory && (trunc || creat));
	if (trunc && rw_mode == Core::File::Mode::Read) return KERNEL_ERROR_EACCES;

	int   descriptor = g_files->CreateDescriptor();
	auto* file       = g_files->GetFile(descriptor);

	EXIT_IF(file == nullptr || file->opened || file->directory);

	file->name      = path;
	file->real_name = (directory ? g_mount_points->GetRealDirectory(file->name) : g_mount_points->GetRealFilename(file->name));

	// Exclusive creation reaches the native atomic operation before any
	// existence/directory check. The returned open file is the creation handle.
	bool dir_exist = !exclusive_create && Core::File::IsDirectoryExisting(file->real_name);

	if (exclusive_create)
	{
		int error = 0;
		if (!file->f.CreateExclusive(file->real_name, rw_mode, mode, &error))
		{
			g_files->DeleteDescriptor(descriptor);
			return ExclusiveCreateKernelError(error);
		}
		g_metadata->Erase(file->real_name);
	} else if (directory || dir_exist)
	{
		if (!dir_exist)
		{
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_ENOTDIR;
		}

		EXIT_NOT_IMPLEMENTED(!directory && rw_mode != Core::File::Mode::Read);
		EXIT_NOT_IMPLEMENTED(!directory && (trunc || creat));

		file->dents       = Core::File::GetDirEntries(file->real_name);
		file->dents_index = 0;
		file->directory   = true;

		printf("\tOpen dir: " FG_WHITE BOLD "%s" DEFAULT ", entries = %" PRIu32 ", " FG_GREEN "[ok]" FG_DEFAULT "\n",
		       file->real_name.C_Str(), file->dents.Size());

		for (const auto& f: file->dents)
		{
			printf("\t\t%s %s\n", f.is_file ? "[file]" : "[dir ]", f.name.C_Str());
		}
	} else
	{
		const bool existed_before_create = Core::File::IsFileExisting(file->real_name);
		if (!creat && !existed_before_create)
		{
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_ENOENT;
		}

		bool result = false;

		if (creat)
		{
			result = file->f.Create(file->real_name);

			printf("\tCreate: " FG_WHITE BOLD "%s" DEFAULT ", %s\n", file->real_name.C_Str(),
			       (result ? FG_GREEN "[ok]" FG_DEFAULT : FG_RED "[fail]" FG_DEFAULT));
		} else
		{
			result = file->f.Open(file->real_name, rw_mode);

			printf("\tOpen: " FG_WHITE BOLD "%s" DEFAULT ", %s\n", file->real_name.C_Str(),
			       (result ? FG_GREEN "[ok]" FG_DEFAULT : FG_RED "[fail]" FG_DEFAULT));
		}

		if (result && trunc)
		{
			result = file->f.Truncate(0);
		}

		if (!result || file->f.IsInvalid())
		{
			if (!file->f.IsInvalid())
			{
				file->f.Close();
			}
			g_files->DeleteDescriptor(descriptor);
			return KERNEL_ERROR_EACCES;
		}
		if (creat && !existed_before_create) g_metadata->Erase(file->real_name);
	}

	file->descriptor_flags = 0;
	file->status_flags = original_flags & (0x0003u | 0x0004u | 0x00020000u);
	file->opened = true;
	return descriptor;
}

int LUMEN_SYSV_ABI KernelClose(int d)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	EXIT_IF(!file->opened);

	if (!file->directory)
	{
		file->f.Close();
	}

	file->opened = false;

	printf("\tClose: " FG_WHITE BOLD "%s" DEFAULT "\n", file->real_name.C_Str());

	g_files->DeleteDescriptor(d);

	return OK;
}

int LUMEN_SYSV_ABI KernelFcntl(int d, int command, uint64_t argument)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);
	if (d < DESCRIPTOR_MIN) return KERNEL_ERROR_EBADF;
	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened.load()) return KERNEL_ERROR_EBADF;

	// FreeBSD/Orbis fcntl commands and descriptor flag values used by the
	// shared POSIX ABI.
	constexpr int      F_GETFD_COMMAND = 1;
	constexpr int      F_SETFD_COMMAND = 2;
	constexpr int      F_GETFL_COMMAND = 3;
	constexpr int      F_SETFL_COMMAND = 4;
	constexpr uint32_t FD_CLOEXEC_FLAG = 1;
	constexpr uint32_t O_NONBLOCK_FLAG = 0x0004u;
	constexpr uint32_t IMMUTABLE_STATUS_FLAGS = 0x0003u | 0x00020000u;

	switch (command)
	{
		case F_GETFD_COMMAND: return static_cast<int>(file->descriptor_flags.load());
		case F_SETFD_COMMAND:
			if (argument > UINT32_MAX || (static_cast<uint32_t>(argument) & ~FD_CLOEXEC_FLAG) != 0)
				return KERNEL_ERROR_EINVAL;
			file->descriptor_flags = static_cast<uint32_t>(argument);
			return OK;
		case F_GETFL_COMMAND: return static_cast<int>(file->status_flags.load());
		case F_SETFL_COMMAND:
		{
			if (argument > UINT32_MAX) return KERNEL_ERROR_EINVAL;
			const uint32_t requested = static_cast<uint32_t>(argument);
			if ((requested & ~(IMMUTABLE_STATUS_FLAGS | O_NONBLOCK_FLAG)) != 0)
				return KERNEL_ERROR_EINVAL;
			const uint32_t current = file->status_flags.load();
			file->status_flags = (current & ~O_NONBLOCK_FLAG) | (requested & O_NONBLOCK_FLAG);
			return OK;
		}
		default: return KERNEL_ERROR_EINVAL;
	}
}

int LUMEN_SYSV_ABI KernelFsync(int d)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);
	if (d < DESCRIPTOR_MIN) return KERNEL_ERROR_EBADF;
	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened.load()) return KERNEL_ERROR_EBADF;
	if (file->directory.load()) return KERNEL_ERROR_EISDIR;
	Core::LockGuard lock(file->mutex);
	if (file->f.IsInvalid()) return KERNEL_ERROR_EIO;
	return file->f.Flush() ? OK : KERNEL_ERROR_EIO;
}

int LUMEN_SYSV_ABI KernelFtruncate(int d, int64_t length)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr || g_metadata == nullptr);
	if (d < DESCRIPTOR_MIN) return KERNEL_ERROR_EBADF;
	if (length < 0) return KERNEL_ERROR_EINVAL;
	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened.load()) return KERNEL_ERROR_EBADF;
	if (file->directory.load()) return KERNEL_ERROR_EISDIR;
	if ((file->status_flags.load() & 0x0003u) == 0) return KERNEL_ERROR_EBADF;
	const String real_name = file->real_name;
	{
		Core::LockGuard lock(file->mutex);
		if (file->f.IsInvalid()) return KERNEL_ERROR_EIO;
		if (!file->f.Truncate(static_cast<uint64_t>(length))) return KERNEL_ERROR_EIO;
	}
	g_metadata->TouchAfterContentChange(real_name, CurrentGuestTime());
	return OK;
}

int64_t LUMEN_SYSV_ABI KernelRead(int d, void* buf, size_t nbytes)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if ((file->status_flags.load() & 0x0003u) == 1) return KERNEL_ERROR_EBADF;

	EXIT_NOT_IMPLEMENTED(file->directory);

	EXIT_IF(!file->opened);

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease destination;
	if (nbytes != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(buf), nbytes, Core::VirtualMemory::Mode::Write, &destination))
		return KERNEL_ERROR_EFAULT;
	GuestCpu::MemoryHandoffBypass prepared;
#endif

	file->mutex.Lock();

	bool       is_invalid  = file->f.IsInvalid();
	const auto read_result = GuestFile::Read(file->f, buf, static_cast<uint32_t>(nbytes));

	file->mutex.Unlock();

	if (read_result.error == GuestFile::ReadError::InvalidAddress)
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (is_invalid)
	{
		printf("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	printf("\tRead %u bytes from: " FG_WHITE BOLD "%s" DEFAULT "\n", read_result.bytes_read, file->real_name.C_Str());

	return read_result.bytes_read;
}

int64_t LUMEN_SYSV_ABI KernelWrite(int d, const void* buf, size_t nbytes)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if ((file->status_flags.load() & 0x0003u) == 0) return KERNEL_ERROR_EBADF;

	EXIT_NOT_IMPLEMENTED(file->directory);

	EXIT_IF(!file->opened);

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease source;
	if (nbytes != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(buf), nbytes, Core::VirtualMemory::Mode::Read, &source))
		return KERNEL_ERROR_EFAULT;
	GuestCpu::MemoryHandoffBypass prepared;
#endif

	file->mutex.Lock();

	bool       is_invalid = file->f.IsInvalid();
	const auto write_result = GuestFile::Write(file->f, buf, static_cast<uint32_t>(nbytes));

	file->mutex.Unlock();

	if (write_result.error == GuestFile::WriteError::InvalidAddress)
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (is_invalid)
	{
		printf("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	printf("\tWrite %u bytes to: " FG_WHITE BOLD "%s" DEFAULT "\n", write_result.bytes_written, file->real_name.C_Str());

	return write_result.bytes_written;
}

int64_t LUMEN_SYSV_ABI KernelPread(int d, void* buf, size_t nbytes, int64_t offset)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if ((file->status_flags.load() & 0x0003u) == 1) return KERNEL_ERROR_EBADF;

	EXIT_NOT_IMPLEMENTED(file->directory);

	EXIT_IF(!file->opened);

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease destination;
	if (nbytes != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(buf), nbytes, Core::VirtualMemory::Mode::Write, &destination))
		return KERNEL_ERROR_EFAULT;
	GuestCpu::MemoryHandoffBypass prepared;
#endif

	const bool profile_pread = GuestFile::PreadProfileEnabled();
	const auto mutex_start =
	    profile_pread ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
	file->mutex.Lock();
	if (profile_pread)
	{
		GuestFile::PreadProfileRecordMutexWait(
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - mutex_start)
		                              .count()));
	}

	bool       is_invalid  = file->f.IsInvalid();
	const auto read_result = GuestFile::Pread(file->f, buf, static_cast<uint32_t>(nbytes), static_cast<uint64_t>(offset));

	file->mutex.Unlock();

	if (read_result.error == GuestFile::ReadError::InvalidAddress)
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (read_result.error == GuestFile::ReadError::SeekFailed)
	{
		return KERNEL_ERROR_EIO;
	}
	if (read_result.error == GuestFile::ReadError::ReadFailed)
	{
		return KERNEL_ERROR_EIO;
	}
	if (is_invalid)
	{
		printf("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	printf("\tRead %u bytes (pos = %" PRId64 ") from: " FG_WHITE BOLD "%s" DEFAULT "\n", read_result.bytes_read, offset,
	       file->real_name.C_Str());

	return read_result.bytes_read;
}

int LUMEN_SYSV_ABI KernelReadMappedRange(int d, void* guest_buffer, size_t nbytes, int64_t offset, uint64_t* bytes_read)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (bytes_read == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	*bytes_read = 0;

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (guest_buffer == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if ((file->status_flags.load() & 0x0003u) == 1) return KERNEL_ERROR_EBADF;

	if (file->directory)
	{
		return KERNEL_ERROR_EISDIR;
	}

	EXIT_IF(!file->opened);

	// A file-backed mapping reads as zero past the end of the file, so the
	// whole destination is zeroed before the fill; a recycled flexible-memory
	// block must not leak stale bytes into the mapping either. The fill then
	// reuses the guarded staging read path shared with KernelPread.
#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease zero_lease;
	if (nbytes != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(guest_buffer), nbytes, Core::VirtualMemory::Mode::Write, &zero_lease))
	{
		return KERNEL_ERROR_EFAULT;
	}
	GuestCpu::MemoryHandoffBypass prepared;
#endif
	std::memset(guest_buffer, 0, nbytes);

	uint64_t filled = 0;
	while (filled < nbytes)
	{
		const uint32_t request =
		    static_cast<uint32_t>(std::min<uint64_t>(nbytes - filled, UINT64_C(64) * 1024 * 1024));

		file->mutex.Lock();
		const bool is_invalid  = file->f.IsInvalid();
		const auto read_result = GuestFile::Pread(file->f, static_cast<uint8_t*>(guest_buffer) + filled, request,
		                                          static_cast<uint64_t>(offset) + filled);
		file->mutex.Unlock();

		if (read_result.error == GuestFile::ReadError::InvalidAddress)
		{
			return KERNEL_ERROR_EFAULT;
		}
		if (read_result.error == GuestFile::ReadError::SeekFailed ||
		    read_result.error == GuestFile::ReadError::ReadFailed)
		{
			return KERNEL_ERROR_EIO;
		}
		if (is_invalid)
		{
			printf("\tfile is invalid\n");
			return KERNEL_ERROR_EIO;
		}

		filled += read_result.bytes_read;
		if (read_result.bytes_read < request)
		{
			break;
		}
	}

	*bytes_read = filled;
	return OK;
}

int64_t LUMEN_SYSV_ABI KernelPwrite(int d, const void* buf, size_t nbytes, int64_t offset)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	if (offset < 0)
	{
		return KERNEL_ERROR_EINVAL;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if ((file->status_flags.load() & 0x0003u) == 0) return KERNEL_ERROR_EBADF;

	EXIT_NOT_IMPLEMENTED(file->directory);

	EXIT_IF(!file->opened);

	EXIT_NOT_IMPLEMENTED(nbytes > UINT_MAX);

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease source;
	if (nbytes != 0 && !Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(buf), nbytes, Core::VirtualMemory::Mode::Read, &source))
		return KERNEL_ERROR_EFAULT;
	GuestCpu::MemoryHandoffBypass prepared;
#endif

	file->mutex.Lock();

	bool       is_invalid = file->f.IsInvalid();
	const auto write_result = GuestFile::Pwrite(
	    file->f, buf, static_cast<uint32_t>(nbytes), static_cast<uint64_t>(offset));

	file->mutex.Unlock();

	if (write_result.error == GuestFile::WriteError::InvalidAddress)
	{
		return KERNEL_ERROR_EFAULT;
	}
	if (write_result.error == GuestFile::WriteError::SeekFailed)
	{
		return KERNEL_ERROR_EIO;
	}
	if (is_invalid)
	{
		printf("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	printf("\tWrite %u bytes (pos = %" PRId64 ") to: " FG_WHITE BOLD "%s" DEFAULT "\n", write_result.bytes_written, offset, file->real_name.C_Str());

	return write_result.bytes_written;
}

int64_t LUMEN_SYSV_ABI KernelLseek(int d, int64_t offset, int whence)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	EXIT_NOT_IMPLEMENTED(file->directory);

	EXIT_IF(!file->opened);

	bool    is_invalid = false;
	int64_t pos        = 0;
	{
		Core::LockGuard lock(file->mutex);
		is_invalid = file->f.IsInvalid();

		if (whence == 1)
		{
			offset = static_cast<int64_t>(file->f.Tell()) + offset;
			whence = 0;
		}

		if (whence == 2)
		{
			offset = static_cast<int64_t>(file->f.Size()) + offset;
			whence = 0;
		}

		EXIT_NOT_IMPLEMENTED(whence != 0);

		if (offset < 0)
		{
			return KERNEL_ERROR_EINVAL;
		}

		file->f.Seek(offset);
		pos = static_cast<int64_t>(file->f.Tell());

		EXIT_IF(pos != offset);
	}

	if (is_invalid)
	{
		printf("\tfile is invalid\n");
		return KERNEL_ERROR_EIO;
	}

	printf("\tLseek (pos = %" PRId64 ") to: " FG_WHITE BOLD "%s" DEFAULT "\n", offset, file->real_name.C_Str());

	return pos;
}

int LUMEN_SYSV_ABI KernelStat(const char* path, FileStat* sb)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr || g_metadata == nullptr);

	if (path == nullptr || sb == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	std::string path_snapshot;
	if (!Loader::ReadGuestString(reinterpret_cast<uint64_t>(path),
	                             std::numeric_limits<size_t>::max(), &path_snapshot, false))
		return KERNEL_ERROR_EFAULT;
	path = path_snapshot.c_str();
#endif

	printf("\t KernelStat: %s\n", path);

	String path_s         = String::FromUtf8(path);
	auto   real_file_name = g_mount_points->GetRealFilename(path_s);
	auto   real_directory = g_mount_points->GetRealDirectory(path_s);

	bool is_dir  = Core::File::IsDirectoryExisting(real_file_name) || Core::File::IsDirectoryExisting(real_directory);
	bool is_file = Core::File::IsFileExisting(real_file_name);

	if (!is_dir && !is_file)
	{
		printf("\t file not found\n");
		return KERNEL_ERROR_ENOENT;
	}

	EXIT_NOT_IMPLEMENTED(is_dir && is_file);
	const String& real_name = is_dir ? real_directory : real_file_name;

	// Build a complete result before borrowing the destination. No VM reader
	// is held while filesystem locks are acquired.
	FileStat result {};
	auto* destination = sb;
	sb = &result;

	sb->st_mode = 0000777u | (is_dir ? 0040000u : 0100000u);

	Core::DateTime at;
	Core::DateTime wt;

	if (is_dir)
	{
		sb->st_size    = 0;
		sb->st_blksize = 512;
		sb->st_blocks  = 0;
	} else
	{
		sb->st_size    = static_cast<int64_t>(Core::File::Size(real_file_name));
		sb->st_blksize = 512;
		sb->st_blocks  = (sb->st_size + 511) / 512;

		Core::File::GetLastAccessAndWriteTimeUTC(real_file_name, &at, &wt);
	}

	sec_to_timespec(&sb->st_atim, at.ToUnix());
	sec_to_timespec(&sb->st_mtim, wt.ToUnix());
	sb->st_ctim     = sb->st_atim;
	sb->st_birthtim = sb->st_mtim;
	g_metadata->Apply(real_name, sb);

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease output;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(destination), sizeof(result), Core::VirtualMemory::Mode::Write, &output))
		return KERNEL_ERROR_EFAULT;
#endif
	std::memcpy(destination, &result, sizeof(result));
	return OK;
}

int LUMEN_SYSV_ABI KernelFstat(int d, FileStat* sb)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr || g_metadata == nullptr);

	if (d < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (sb == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(d);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	EXIT_IF(!file->opened);

	printf("\tKernelFstat: %s\n", file->real_name.C_Str());
	const String real_name = file->real_name;

	FileStat result {};
	auto* destination = sb;
	sb = &result;

	sb->st_mode = 0000777u | (file->directory ? 0040000u : 0100000u);

	Core::DateTime at;
	Core::DateTime wt;

	if (!file->directory)
	{
		file->mutex.Lock();

		bool is_invalid = file->f.IsInvalid();
		auto size       = file->f.Size();
		file->f.GetLastAccessAndWriteTimeUTC(&at, &wt);

		file->mutex.Unlock();

		if (is_invalid)
		{
			printf("\tfile is invalid\n");
			return KERNEL_ERROR_EIO;
		}

		sb->st_size    = static_cast<int64_t>(size);
		sb->st_blksize = 512;
		sb->st_blocks  = (sb->st_size + 511) / 512;
	} else
	{
		sb->st_size    = 0;
		sb->st_blksize = 512;
		sb->st_blocks  = 0;
	}

	sec_to_timespec(&sb->st_atim, at.ToUnix());
	sec_to_timespec(&sb->st_mtim, wt.ToUnix());
	sb->st_ctim     = sb->st_atim;
	sb->st_birthtim = sb->st_mtim;
	g_metadata->Apply(real_name, sb);

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease output;
	if (!Core::VirtualMemory::TryAcquireGuestDataLease(
	        reinterpret_cast<uint64_t>(destination), sizeof(result), Core::VirtualMemory::Mode::Write, &output))
		return KERNEL_ERROR_EFAULT;
#endif
	std::memcpy(destination, &result, sizeof(result));
	return OK;
}

int LUMEN_SYSV_ABI KernelFchmod(int d, uint32_t mode)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr || g_metadata == nullptr);
	if (d < DESCRIPTOR_MIN) return KERNEL_ERROR_EBADF;
	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened.load()) return KERNEL_ERROR_EBADF;
	const String real_name = file->real_name;
	return g_metadata->SetMode(real_name, mode, CurrentGuestTime());
}

int LUMEN_SYSV_ABI KernelChmod(const char* path, uint32_t mode)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr || g_metadata == nullptr);
	std::string path_snapshot;
	const char* checked_path = nullptr;
	if (const int error = SnapshotGuestPath(path, &path_snapshot, &checked_path); error != OK)
		return error;
	String real_name;
	if (const int error = ResolveExistingRealName(checked_path, &real_name); error != OK)
		return error;
	return g_metadata->SetMode(real_name, mode, CurrentGuestTime());
}

int LUMEN_SYSV_ABI KernelUtimes(const char* path, const KernelTimeval* times)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr || g_metadata == nullptr);
	std::string path_snapshot;
	const char* checked_path = nullptr;
	if (const int error = SnapshotGuestPath(path, &path_snapshot, &checked_path); error != OK)
		return error;
	bool supplied = false;
	std::array<KernelTimeval, 2> times_snapshot {};
	if (const int error = SnapshotGuestTimevals(times, &supplied, &times_snapshot); error != OK)
		return error;
	String real_name;
	if (const int error = ResolveExistingRealName(checked_path, &real_name); error != OK)
		return error;
	const KernelTimespec now = CurrentGuestTime();
	const KernelTimespec atime = supplied
	    ? KernelTimespec {times_snapshot[0].tv_sec, times_snapshot[0].tv_usec * 1000}
	    : now;
	const KernelTimespec mtime = supplied
	    ? KernelTimespec {times_snapshot[1].tv_sec, times_snapshot[1].tv_usec * 1000}
	    : now;
	return g_metadata->SetTimes(real_name, atime, mtime, now);
}

int LUMEN_SYSV_ABI KernelFutimes(int d, const KernelTimeval* times)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr || g_metadata == nullptr);
	if (d < DESCRIPTOR_MIN) return KERNEL_ERROR_EBADF;
	auto* file = g_files->GetFile(d);
	if (file == nullptr || !file->opened.load()) return KERNEL_ERROR_EBADF;
	const String real_name = file->real_name;
	bool supplied = false;
	std::array<KernelTimeval, 2> times_snapshot {};
	if (const int error = SnapshotGuestTimevals(times, &supplied, &times_snapshot); error != OK)
		return error;
	const KernelTimespec now = CurrentGuestTime();
	const KernelTimespec atime = supplied
	    ? KernelTimespec {times_snapshot[0].tv_sec, times_snapshot[0].tv_usec * 1000}
	    : now;
	const KernelTimespec mtime = supplied
	    ? KernelTimespec {times_snapshot[1].tv_sec, times_snapshot[1].tv_usec * 1000}
	    : now;
	return g_metadata->SetTimes(real_name, atime, mtime, now);
}

int LUMEN_SYSV_ABI KernelUnlink(const char* path)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr);
	EXIT_IF(g_files == nullptr || g_metadata == nullptr);

	if (path == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	std::string path_snapshot;
	if (!Loader::ReadGuestString(reinterpret_cast<uint64_t>(path),
	                             std::numeric_limits<size_t>::max(), &path_snapshot, false))
		return KERNEL_ERROR_EFAULT;
	path = path_snapshot.c_str();
#endif

	auto path_s         = String::FromUtf8(path);
	auto real_file_name = g_mount_points->GetRealFilename(path_s);
	auto real_directory = g_mount_points->GetRealDirectory(path_s);

	if (g_files->GetFile(real_file_name) != nullptr || g_files->GetFile(real_directory) != nullptr)
	{
		return KERNEL_ERROR_EBUSY;
	}

	bool is_dir  = Core::File::IsDirectoryExisting(real_file_name) || Core::File::IsDirectoryExisting(real_directory);
	bool is_file = Core::File::IsFileExisting(real_file_name);

	if (is_dir)
	{
		return KERNEL_ERROR_EPERM;
	}

	if (!is_file)
	{
		return KERNEL_ERROR_ENOENT;
	}

	bool ok = Core::File::DeleteFile(real_file_name);

	if (!ok)
	{
		return KERNEL_ERROR_EIO;
	}
	g_metadata->Erase(real_file_name);

	printf("\tKernelUnlink: %s\n", path);

	return OK;
}

int LUMEN_SYSV_ABI KernelRename(const char* old_path, const char* new_path)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr || g_files == nullptr || g_metadata == nullptr);
	std::string old_snapshot;
	std::string new_snapshot;
	const char* checked_old = nullptr;
	const char* checked_new = nullptr;
	if (const int error = SnapshotGuestPath(old_path, &old_snapshot, &checked_old); error != OK)
		return error;
	if (const int error = SnapshotGuestPath(new_path, &new_snapshot, &checked_new); error != OK)
		return error;

	String real_old;
	bool   source_is_directory = false;
	if (const int error = ResolveExistingRealName(checked_old, &real_old, &source_is_directory); error != OK)
		return error;
	const String mounted_new = String::FromUtf8(checked_new);
	const String real_new = source_is_directory ? g_mount_points->GetRealDirectory(mounted_new)
	                                            : g_mount_points->GetRealFilename(mounted_new);
	if (real_old == real_new) return OK;

	const bool destination_is_directory = Core::File::IsDirectoryExisting(real_new);
	const bool destination_is_file      = Core::File::IsFileExisting(real_new);
	// Core::File::MoveFile historically removes an existing destination before
	// moving. Fail closed here so a later host rename failure cannot destroy a
	// guest file that was already present.
	if (destination_is_directory || destination_is_file) return KERNEL_ERROR_EEXIST;
	if (g_files->GetFile(real_new) != nullptr) return KERNEL_ERROR_EBUSY;
	if (!Core::File::MoveFile(real_old, real_new)) return KERNEL_ERROR_EIO;

	g_files->RenamePath(real_old, real_new, source_is_directory);
	g_metadata->Rename(real_old, real_new, source_is_directory);
	return OK;
}

int LUMEN_SYSV_ABI KernelRmdir(const char* path)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr || g_files == nullptr || g_metadata == nullptr);
	std::string path_snapshot;
	const char* checked_path = nullptr;
	if (const int error = SnapshotGuestPath(path, &path_snapshot, &checked_path); error != OK)
		return error;
	String real_name;
	bool   is_directory = false;
	if (const int error = ResolveExistingRealName(checked_path, &real_name, &is_directory); error != OK)
		return error;
	if (!is_directory) return KERNEL_ERROR_ENOTDIR;
	if (g_files->GetFile(real_name) != nullptr) return KERNEL_ERROR_EBUSY;
	for (const auto& entry: Core::File::GetDirEntries(real_name))
	{
		if (entry.name != U"." && entry.name != U"..") return KERNEL_ERROR_ENOTEMPTY;
	}
	if (!Core::File::DeleteDirectory(real_name)) return KERNEL_ERROR_EIO;
	g_metadata->Erase(real_name, true);
	return OK;
}

int LUMEN_SYSV_ABI KernelGetdirentries(int fd, char* buf, int nbytes, int64_t* basep)
{
	PRINT_NAME();

	EXIT_IF(g_files == nullptr);

	if (fd < DESCRIPTOR_MIN)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (buf == nullptr)
	{
		return KERNEL_ERROR_EFAULT;
	}

	auto* file = g_files->GetFile(fd);

	if (file == nullptr)
	{
		return KERNEL_ERROR_EBADF;
	}

	if (!file->directory || nbytes < 512 || file->dents_index > file->dents.Size())
	{
		return KERNEL_ERROR_EINVAL;
	}

	EXIT_IF(!file->opened);

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease output;
	Core::VirtualMemory::GuestDataLease position;
	// Validate every output before writing basep or advancing the directory
	// cursor. At EOF no directory record is consumed or published.
	if ((file->dents_index != file->dents.Size() &&
	     !Core::VirtualMemory::TryAcquireGuestDataLease(
	         reinterpret_cast<uint64_t>(buf), 512, Core::VirtualMemory::Mode::Write, &output)) ||
	    (basep != nullptr && !Core::VirtualMemory::TryAcquireGuestDataLease(
	         reinterpret_cast<uint64_t>(basep), sizeof(*basep), Core::VirtualMemory::Mode::Write, &position)))
		return KERNEL_ERROR_EFAULT;
#endif

	printf("\t dir    = %s\n", file->real_name.C_Str());
	printf("\t nbytes = %d\n", nbytes);
	printf("\t index = %d\n", file->dents_index);

	if (basep != nullptr)
	{
		const int64_t index = file->dents_index;
		std::memcpy(basep, &index, sizeof(index));
	}

	if (file->dents_index == file->dents.Size())
	{
		return 0;
	}

	const auto& entry = file->dents.At(file->dents_index);

	auto str      = entry.name.utf8_str();
	auto str_size = str.Size() - 1;
	EXIT_NOT_IMPLEMENTED(str_size > 255);

	printf("\t name  = %s\n", str.GetDataConst());

	std::array<char, 264> record {};
	const uint32_t hash = entry.name.Hash();
	const uint16_t length = 512;
	std::memcpy(record.data(), &hash, sizeof(hash));
	std::memcpy(record.data() + 4, &length, sizeof(length));
	record[6] = entry.is_file ? 8 : 4;
	record[7] = static_cast<char>(str_size);
	std::strncpy(record.data() + 8, str.GetDataConst(), 255);
	std::memcpy(buf, record.data(), record.size());
	file->dents_index++;

	return 512;
}

int LUMEN_SYSV_ABI KernelGetdents(int fd, char* buf, int nbytes)
{
	PRINT_NAME();

	return KernelGetdirentries(fd, buf, nbytes, nullptr);
}

int LUMEN_SYSV_ABI KernelMkdir(const char* path, uint16_t mode)
{
	PRINT_NAME();

	EXIT_IF(g_mount_points == nullptr || g_files == nullptr || g_metadata == nullptr);

	if (path == nullptr)
	{
		return KERNEL_ERROR_EINVAL;
	}

#if defined(LUMEN_FEXCORE_RUNTIME) && (defined(__APPLE__) || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	std::string path_snapshot;
	if (!Loader::ReadGuestString(reinterpret_cast<uint64_t>(path),
	                             std::numeric_limits<size_t>::max(), &path_snapshot, false))
		return KERNEL_ERROR_EFAULT;
	path = path_snapshot.c_str();
#endif

	printf("\t path = %s\n", path);
	printf("\t mode = %04" PRIx16 "\n", mode);

	String real_name = g_mount_points->GetRealDirectory(String::FromUtf8(path));

	if (Core::File::IsDirectoryExisting(real_name))
	{
		return KERNEL_ERROR_EEXIST;
	}

	if (!Core::File::CreateDirectory(real_name))
	{
		return KERNEL_ERROR_EIO;
	}

	if (!Core::File::IsDirectoryExisting(real_name))
	{
		return KERNEL_ERROR_ENOENT;
	}
	g_metadata->Erase(real_name);

	return OK;
}

} // namespace Lumen::Libs::LibKernel::FileSystem

#endif // LUMEN_EMU_ENABLED
