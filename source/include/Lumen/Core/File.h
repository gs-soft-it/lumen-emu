#ifndef INCLUDE_LUMEN_CORE_FILE_H_
#define INCLUDE_LUMEN_CORE_FILE_H_

#include "Lumen/Core/ByteBuffer.h"
#include "Lumen/Core/Common.h"
#include "Lumen/Core/DateTime.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Vector.h"

struct SDL_RWops;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef CopyFile
#undef CopyFile
#endif
#ifdef MoveFile
#undef MoveFile
#endif
#endif

namespace Lumen::Core {

// SUBSYSTEM_DEFINE(File);
void core_file_init();

class File
{
public:
	enum class Encoding
	{
		Unknown,
		Utf8,
		Utf16BE,
		Utf16LE,
		Utf32BE,
		Utf32LE
	};

	// Free space of the filesystem hosting an existing path, floored to KiB.
	// A missing path or an unsupported host reports failure instead of a value.
	[[nodiscard]] static bool GetAvailableSpaceKb(const String& path, uint64_t* available_space_kb);

	enum class Mode
	{
		Read,
		Write,
		ReadWrite,
		WriteRead
	};

	struct FindInfo
	{
		String   path_with_name;
		String   rel_path_with_name;
		DateTime last_access_time;
		DateTime last_write_time;
		uint64_t size;
	};

	struct DirEntry
	{
		String name;
		bool   is_file;
	};

	File();
	explicit File(const String& name);
	File(const String& name, Mode mode);
	virtual ~File();

	bool Create(const String& name);
	// Atomically create a new native file and retain that open file. Existing
	// files are never opened or truncated. error_number receives zero on
	// success or a host errno value on failure (also normalized on Windows).
	bool CreateExclusive(const String& name, Mode mode, uint16_t permissions, int* error_number = nullptr);
	bool Open(const String& name, Mode mode);
	bool OpenInMem(void* buf, uint32_t buf_size);
	bool OpenInMem(ByteBuffer& buf); // NOLINT(google-runtime-references)
	bool CreateInMem();

	void Close();

	bool Flush();

	[[nodiscard]] uint64_t Size() const;
	[[nodiscard]] uint64_t Remaining() const;

	bool                   Seek(uint64_t offset);
	[[nodiscard]] uint64_t Tell() const;

	bool Truncate(uint64_t size);

	[[nodiscard]] bool IsInvalid() const;

	[[nodiscard]] bool IsEOF() const { return Tell() >= Size(); }

	void GetLastAccessAndWriteTimeUTC(DateTime* access, DateTime* write);

	void       Read(void* data, uint32_t size, uint32_t* bytes_read = nullptr);
	bool       ReadAt(void* data, uint32_t size, uint64_t offset, uint32_t* bytes_read = nullptr);
	ByteBuffer Read(uint32_t size);
	void       Write(const void* data, uint32_t size, uint32_t* bytes_written = nullptr);
	void       Write(const ByteBuffer& buf, uint32_t* bytes_written = nullptr);
	void       ReadR(void* data, uint32_t size);
	void       WriteR(const void* data, uint32_t size);

	void                   Write(const String& str, uint32_t* bytes_written = nullptr);
	void                   WriteBOM();
	String                 ReadLine();
	String                 ReadWholeString();
	[[nodiscard]] Encoding GetEncoding() const;
	void                   SetEncoding(Encoding e);
	void                   DetectEncoding();

	void Printf(const char* format, ...) LUMEN_FORMAT_PRINTF(2, 3);
	void PrintfLF(const char* format, ...) LUMEN_FORMAT_PRINTF(2, 3);

	ByteBuffer ReadWholeBuffer();

	static uint64_t Size(const String& name);
	static String   Read(const String& name, Encoding e);

	static bool IsDirectoryExisting(const String& path);
	static bool IsFileExisting(const String& name);
	static bool IsAssetFileExisting(const String& name);
	static bool CreateDirectory(const String& path);
	static bool CreateDirectories(const String& path);
	static bool DeleteDirectory(const String& path);
	static bool DeleteDirectories(const String& path);
	static bool DeleteFile(const String& name);

	static DateTime GetLastAccessTimeUTC(const String& name);
	static DateTime GetLastWriteTimeUTC(const String& name);
	static void     GetLastAccessAndWriteTimeUTC(const String& name, DateTime* access, DateTime* write);
	static bool     SetLastAccessTimeUTC(const String& name, const DateTime& dt);
	static bool     SetLastWriteTimeUTC(const String& name, const DateTime& dt);
	static bool     SetLastAccessAndWriteTimeUTC(const String& name, const DateTime& access, const DateTime& write);

	static Vector<FindInfo> FindFiles(const String& path);
	static Vector<DirEntry> GetDirEntries(const String& path);
	static bool             CopyFile(const String& src, const String& dst);
	static bool             MoveFile(const String& src, const String& dst);
	static void             RemoveReadonly(const String& name);
	static void             SyncDirectories(const String& src_dir, const String& dst_dir, bool del_dst = true);

	static void   SetAssetsDir(const String& dir);
	static String GetAssetsDir();
	static void   SetAssetsSubDir(const String& dir);
	static String GetAssetsSubDir();

	SDL_RWops* CreateSdlRWops();

	LUMEN_CLASS_NO_COPY(File);

private:
	struct FilePrivate;

	String       m_file_name;
	FilePrivate* m_p;
};

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_FILE_H_ */
