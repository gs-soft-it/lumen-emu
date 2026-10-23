#include "Lumen/Core/Common.h"

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_LINUX && LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && \
    LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
//#error "LUMEN_PLATFORM != LUMEN_PLATFORM_LINUX"
#else

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/MemoryAlloc.h"
#include "Lumen/Core/String.h"
#include "Lumen/Sys/SysFileIO.h"
#include "Lumen/Sys/SysTimer.h"

#include "SDL_system.h"

#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utime.h>

namespace Lumen {

template <typename T>
class Vector;

static String* g_internal_files_dir = nullptr;

static String get_internal_name(const String& name)
{
	return name.StartsWith(U"/") ? name : *g_internal_files_dir + U"/" + name;
}

bool sys_file_io_init()
{
	g_internal_files_dir = new String();

	*g_internal_files_dir = U".";

	return !g_internal_files_dir->IsEmpty();
}

void sys_file_read(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read)
{
	if (f.type == SYS_FILE_FILE)
	{
		size_t w = fread(data, 1, size, f.f);
		if (bytes_read != nullptr)
		{
			// fread cannot return more than the uint32_t-sized request.
			*bytes_read = static_cast<uint32_t>(w);
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT)
	{
		uint32_t s = size;
		if (f.buf->size != 0)
		{
			uint32_t l = f.buf->size - static_cast<uint32_t>(f.buf->ptr - f.buf->base);
			if (s > l)
			{
				s = l;
			}
		}
		memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr)
		{
			*bytes_read = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN)
	{
		uint32_t s = size;
		if (f.buf->size != 0)
		{
			uint32_t l = f.buf->size - static_cast<uint32_t>(f.buf->ptr - f.buf->base);
			if (s > l)
			{
				s = l;
			}
		} else
		{
			s = 0;
		}
		memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr)
		{
			*bytes_read = s;
		}
	}
}

bool sys_file_read_at(void* data, uint32_t size, uint64_t offset, sys_file_t& f, uint32_t* bytes_read)
{
	if (bytes_read != nullptr)
	{
		*bytes_read = 0;
	}

	if (f.type == SYS_FILE_FILE)
	{
		if (f.f == nullptr || offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
		{
			return false;
		}

		ssize_t result = 0;
		do
		{
			result = pread(fileno(f.f), data, size, static_cast<off_t>(offset));
		} while (result < 0 && errno == EINTR);

		if (result < 0)
		{
			return false;
		}
		if (bytes_read != nullptr)
		{
			*bytes_read = static_cast<uint32_t>(result);
		}
		return true;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN)
	{
		const uint32_t current =
		    offset < f.buf->size ? (std::min)(size, f.buf->size - static_cast<uint32_t>(offset)) : 0;
		if (current != 0)
		{
			std::memcpy(data, f.buf->base + offset, current);
		}
		if (bytes_read != nullptr)
		{
			*bytes_read = current;
		}
		return true;
	}

	return false;
}

void sys_file_write(const void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_written)
{
	if (f.type == SYS_FILE_FILE)
	{
		size_t w = fwrite(data, 1, size, f.f);
		if (bytes_written != nullptr)
		{
			// fwrite cannot return more than the uint32_t-sized request.
			*bytes_written = static_cast<uint32_t>(w);
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT)
	{
		uint32_t s = size;
		if (f.buf->size != 0)
		{
			uint32_t l = f.buf->size - static_cast<uint32_t>(f.buf->ptr - f.buf->base);
			if (s > l)
			{
				s = l;
			}
		}
		memcpy(f.buf->ptr, data, s);
		f.buf->ptr += s;
		if (bytes_written != nullptr)
		{
			*bytes_written = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN)
	{
		uint32_t pos = static_cast<uint32_t>(f.buf->ptr - f.buf->base);
		if (f.buf->size < pos + size)
		{
			f.buf->base = static_cast<uint8_t*>(Core::mem_realloc(f.buf->base, pos + size));
			f.buf->ptr  = f.buf->base + pos;
			f.buf->size = pos + size;
		}
		memcpy(f.buf->ptr, data, size);
		f.buf->ptr += size;
		if (bytes_written != nullptr)
		{
			*bytes_written = size;
		}
	}
}

void sys_file_read_r(void* data, uint32_t size, sys_file_t& f)
{
	// DWORD w;
	// ReadFile(f, data, size, &w, 0);

	sys_file_read(data, size, f);

	for (uint32_t i = 0; i < size / 2; i++)
	{
		char t                                   = (static_cast<char*>(data))[i];
		(static_cast<char*>(data))[i]            = (static_cast<char*>(data))[size - i - 1];
		(static_cast<char*>(data))[size - i - 1] = t;
	}
}

void sys_file_write_r(const void* data, uint32_t size, sys_file_t& f)
{
	char* data_r = new char[size];
	for (uint32_t i = 0; i < size; i++)
	{
		data_r[i] = (static_cast<const char*>(data))[size - i - 1];
	}

	sys_file_write(data_r, size, f);

	delete[] data_r;
}

void sys_file_write(uint32_t n, sys_file_t& f)
{
	sys_file_write(&n, 4, f);
}

void sys_file_write_r(uint32_t n, sys_file_t& f)
{
	sys_file_write_r(&n, 4, f);
}

sys_file_t* sys_file_create(const String& file_name)
{
	auto* ret = new sys_file_t;

	String real_name = get_internal_name(file_name);

	ret->f = fopen(real_name.utf8_str().GetData(), "w+");

	if (ret->f == nullptr)
	{
		printf("can't create file: %s\n", real_name.utf8_str().GetData());
	}

	ret->type = SYS_FILE_FILE;

	return ret;
}

sys_file_t* sys_file_create_exclusive(const String& file_name, bool read, bool write, uint16_t permissions,
                                    int* error_number)
{
	if (error_number != nullptr) *error_number = 0;
	const auto reject = [error_number](int error) -> sys_file_t* {
		if (error_number != nullptr) *error_number = error;
		return nullptr;
	};
	if (!read && !write) return reject(EINVAL);
	auto* ret = new (std::nothrow) sys_file_t;
	if (ret == nullptr) return reject(ENOMEM);
	const String real_name = get_internal_name(file_name);
	const int access = read && write ? O_RDWR : (write ? O_WRONLY : O_RDONLY);
	const int fd = ::open(real_name.utf8_str().GetData(), access | O_CREAT | O_EXCL,
	                      static_cast<mode_t>(permissions & 0777u));
	if (fd < 0)
	{
		const int error = errno;
		delete ret;
		return reject(error);
	}
	// fdopen adopts this descriptor; it neither reopens the pathname nor
	// truncates the file. All later I/O addresses the atomically created file.
	ret->f = ::fdopen(fd, read && write ? "r+" : (write ? "w" : "r"));
	if (ret->f == nullptr)
	{
		const int error = errno;
		::close(fd);
		delete ret;
		return reject(error);
	}
	// Positional reads use pread on this same descriptor. Keep new native
	// handles unbuffered so a completed Write is visible to ReadAt/Pread
	// without requiring the caller to flush the stdio stream first.
	if (::setvbuf(ret->f, nullptr, _IONBF, 0) != 0)
	{
		::fclose(ret->f);
		delete ret;
		return reject(EIO);
	}
	ret->type = SYS_FILE_FILE;
	return ret;
}

sys_file_t* sys_file_open_r(const String& file_name, sys_file_cache_type_t /*cache_type*/)
{
	auto* ret = new sys_file_t;

	ret->type = SYS_FILE_FILE;

	String internal_name = get_internal_name(file_name);
	;

	FILE* f = fopen(internal_name.utf8_str().GetData(), "r");

	if (f == nullptr)
	{
		ret->type = SYS_FILE_ERROR;
	}

	ret->f = f;

	return ret;
}

sys_file_t* sys_file_open(uint8_t* buf, uint32_t buf_size)
{
	auto* ret = new sys_file_t;

	ret->type      = SYS_FILE_MEMORY_STAT;
	ret->buf       = new sys_file_mem_buf_t;
	ret->buf->base = buf;
	ret->buf->ptr  = buf;
	ret->buf->size = buf_size;

	return ret;
}

sys_file_t* sys_file_create()
{
	auto* ret = new sys_file_t;

	ret->type      = SYS_FILE_MEMORY_DYN;
	ret->buf       = new sys_file_mem_buf_t;
	ret->buf->base = nullptr;
	ret->buf->ptr  = nullptr;
	ret->buf->size = 0;

	return ret;
}

sys_file_t* sys_file_open_w(const String& file_name, sys_file_cache_type_t /*cache_type*/)
{
	auto* ret = new sys_file_t;

	String real_name = get_internal_name(file_name);
	;

	FILE* f = fopen(real_name.utf8_str().GetData(), "r+");

	if (f == nullptr)
	{
		ret->type = SYS_FILE_ERROR;
	} else
	{
		ret->type = SYS_FILE_FILE;
	}

	ret->f = f;

	return ret;
}

sys_file_t* sys_file_open_rw(const String& file_name, sys_file_cache_type_t /*cache_type*/)
{
	auto* ret = new sys_file_t;

	String real_name = get_internal_name(file_name);

	FILE* f = fopen(real_name.utf8_str().GetData(), "r+");

	if (f == nullptr)
	{
		ret->type = SYS_FILE_ERROR;
	} else
	{
		ret->type = SYS_FILE_FILE;
	}

	ret->f = f;

	return ret;
}

void sys_file_close(sys_file_t* f)
{
	[[maybe_unused]] int result = 0;

	if (f->type == SYS_FILE_FILE && f->f != nullptr)
	{
		result = fclose(f->f);
	} else if (f->type == SYS_FILE_MEMORY_STAT)
	{
		delete f->buf;
	} else if (f->type == SYS_FILE_MEMORY_DYN)
	{
		Core::mem_free(f->buf->base);
		delete f->buf;
	}

	// f.type = SYS_FILE_ERROR;
	delete f;
}

uint64_t sys_file_size(sys_file_t& f)
{
	[[maybe_unused]] int result = 0;

	if (f.type == SYS_FILE_FILE)
	{
		const long pos = ftell(f.f);
		if (pos < 0)
		{
			return 0;
		}
		result          = fseek(f.f, 0, SEEK_END);
		const long size = ftell(f.f);
		result          = fseek(f.f, pos, SEEK_SET);
		return size >= 0 ? static_cast<uint64_t>(size) : 0;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN)
	{
		return f.buf->size;
	}

	return 0;
}

uint64_t sys_file_size(const String& file_name)
{
	sys_file_t* f    = sys_file_open_r(file_name);
	uint64_t    size = sys_file_size(*f);
	sys_file_close(f);
	return size;
}

bool sys_file_truncate(sys_file_t& f, uint64_t size)
{
	if (f.type != SYS_FILE_FILE || f.f == nullptr || size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
	{
		return false;
	}

	// Keep the stream and its descriptor synchronized before changing the
	// underlying file size. ftruncate(), like the Windows implementation,
	// preserves the current file position even when it ends beyond the new EOF.
	if (fflush(f.f) != 0)
	{
		return false;
	}

	const int fd = fileno(f.f);
	return fd >= 0 && ftruncate(fd, static_cast<off_t>(size)) == 0;
}

bool sys_file_seek(sys_file_t& f, uint64_t offset)
{
	bool ok = true;
	if (f.type == SYS_FILE_FILE)
	{
		ok = (fseek(f.f, static_cast<int64_t>(offset), SEEK_SET) == 0);
		//		LARGE_INTEGER s;
		//		s.QuadPart = offset;
		//		SetFilePointerEx(f.handle, s, 0, FILE_BEGIN);
		// printf("seek: %u\n", offset);
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN)
	{
		f.buf->ptr = f.buf->base + offset;
	}
	return ok;
}

uint64_t sys_file_tell(sys_file_t& f)
{
	if (f.type == SYS_FILE_FILE)
	{
		return ftell(f.f);
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN)
	{
		return f.buf->ptr - f.buf->base;
	}

	return 0;
}

bool sys_file_is_error(sys_file_t& f)
{
	return f.type == SYS_FILE_ERROR || (f.type == SYS_FILE_FILE && f.f == nullptr);
}

bool sys_file_is_directory_existing(const String& path)
{
	String real_name = get_internal_name(path);

	struct stat s
	{
	};

	if (0 != stat(real_name.utf8_str().GetData(), &s))
	{
		return false;
	}

	return S_ISDIR(s.st_mode); // NOLINT
}

bool sys_file_is_file_existing(const String& name)
{
	String real_name = get_internal_name(name);

	struct stat s
	{
	};

	if (0 != stat(real_name.utf8_str().GetData(), &s))
	{
		return false;
	}

	return !S_ISDIR(s.st_mode); // NOLINT
}

bool sys_file_get_available_space_kb(const String& path, uint64_t* available_space_kb)
{
	if (available_space_kb == nullptr)
	{
		return false;
	}

	String real_name = get_internal_name(path);

	struct statvfs vfs
	{
	};

	if (0 != statvfs(real_name.utf8_str().GetData(), &vfs))
	{
		return false;
	}

	// f_frsize is the block unit of f_bavail; f_bavail is 32-bit on Darwin, so
	// the product is promoted before any truncation could apply. No branch is
	// added for a zero f_frsize: a filesystem reporting it yields zero space.
	const uint64_t bytes = static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);

	*available_space_kb = bytes / 1024;
	return true;
}

bool sys_file_create_directory(const String& path)
{
	String real_name = get_internal_name(path);

	mode_t m = S_IRWXU | S_IRWXG | S_IRWXO; // NOLINT

	String::Utf8 u = real_name.utf8_str();

	int r = mkdir(u.GetDataConst(), m);

	if (r != 0)
	{
		int e = errno;
		if (e == EEXIST)
		{
			printf("mkdir(%s, %" PRIx32 ") failed: The named file exists\n", real_name.C_Str(), static_cast<uint32_t>(m));
		} else
		{
			printf("mkdir(%s, %" PRIx32 ") failed: %d\n", real_name.C_Str(), static_cast<uint32_t>(m), e);
			return false;
		}
	}

	return true;
}

bool sys_file_delete_directory(const String& path)
{
	String real_name = get_internal_name(path);

	return 0 == remove(real_name.utf8_str().GetData());
}

bool sys_file_delete_file(const String& name)
{
	String real_name = get_internal_name(name);

	return 0 == unlink(real_name.utf8_str().GetData());
}

bool sys_file_flush(sys_file_t& f)
{
	if (f.type == SYS_FILE_FILE && f.f != nullptr)
	{
		return (fflush(f.f) == 0);
	}

	return false;
}

SysFileTimeStruct sys_file_get_last_access_time_utc(const String& name)
{
	SysFileTimeStruct r {};

	String real_name = get_internal_name(name);

	struct stat s
	{
	};

	if (0 != stat(real_name.utf8_str().GetData(), &s))
	{
		r.is_invalid = true;
	} else
	{
		r.is_invalid = false;
		r.time       = s.st_atime;
	}

	return r;
}

SysFileTimeStruct sys_file_get_last_write_time_utc(const String& name)
{
	SysFileTimeStruct r {};

	String real_name = get_internal_name(name);

	struct stat s
	{
	};

	if (0 != stat(real_name.utf8_str().GetData(), &s))
	{
		r.is_invalid = true;
	} else
	{
		r.is_invalid = false;
		r.time       = s.st_mtime;
	}

	return r;
}

void sys_file_get_last_access_and_write_time_utc(const String& name, SysFileTimeStruct& a, SysFileTimeStruct& w)
{
	String real_name = get_internal_name(name);

	struct stat s
	{
	};

	if (0 != stat(real_name.utf8_str().GetData(), &s))
	{
		a.is_invalid = true;
		w.is_invalid = true;
	} else
	{
		a.is_invalid = false;
		w.is_invalid = false;
		a.time       = s.st_atime;
		w.time       = s.st_mtime;
	}
}

void sys_file_get_last_access_and_write_time_utc(sys_file_t& f, SysFileTimeStruct& a, SysFileTimeStruct& w)
{
	if (f.type == SYS_FILE_FILE && f.f != nullptr)
	{
		struct stat s {};

		int result = 0;
		do
		{
			result = fstat(fileno(f.f), &s);
		} while (result != 0 && errno == EINTR);

		a.is_invalid = w.is_invalid = (result != 0);
		if (result == 0)
		{
			a.time = s.st_atime;
			w.time = s.st_mtime;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN)
	{
		SysTimeStruct t {};
		sys_get_system_time_utc(t);
		sys_system_to_file_time_utc(t, a);
		sys_system_to_file_time_utc(t, w);
	} else
	{
		a.is_invalid = w.is_invalid = true;
	}
}

bool sys_file_set_last_access_time_utc(const String& name, SysFileTimeStruct& access)
{
	if (access.is_invalid)
	{
		return false;
	}

	String real_name = get_internal_name(name);

	struct stat s
	{
	};

	String::Utf8 n = real_name.utf8_str();

	if (0 != stat(n.GetData(), &s))
	{
		return false;
	}

	utimbuf times {};
	times.actime  = access.time;
	times.modtime = s.st_mtime;

	return !(0 != utime(n.GetData(), &times));

	//	if (0 != stat(n.GetData(), &s))
	//	{
	//		return false;
	//	}
	//
	//	times.actime += times.actime - s.st_atime;
	//	times.modtime += times.modtime - s.st_mtime;
	//
	//	if (0 != utime(n.GetData(), &times))
	//	{
	//		return false;
	//	}
}

bool sys_file_set_last_write_time_utc(const String& name, SysFileTimeStruct& write)
{
	if (write.is_invalid)
	{
		return false;
	}

	String real_name = get_internal_name(name);

	struct stat s
	{
	};

	String::Utf8 n = real_name.utf8_str();

	if (0 != stat(n.GetData(), &s))
	{
		return false;
	}

	utimbuf times {};
	times.actime  = s.st_atime;
	times.modtime = write.time;

	return !(0 != utime(n.GetData(), &times));

	//	if (0 != stat(n.GetData(), &s))
	//	{
	//		return false;
	//	}
	//
	//	times.actime += times.actime - s.st_atime;
	//	times.modtime += times.modtime - s.st_mtime;
	//
	//	if (0 != utime(n.GetData(), &times))
	//	{
	//		return false;
	//	}
}

bool sys_file_set_last_access_and_write_time_utc(const String& name, SysFileTimeStruct& access, SysFileTimeStruct& write)
{
	if (access.is_invalid || write.is_invalid)
	{
		return false;
	}

	String real_name = get_internal_name(name);

	struct stat s
	{
	};

	String::Utf8 n = real_name.utf8_str();

	if (0 != stat(n.GetData(), &s))
	{
		return false;
	}

	utimbuf times {};
	times.actime  = access.time;
	times.modtime = write.time;

	return !(0 != utime(n.GetData(), &times));

	//	if (0 != stat(n.GetData(), &s))
	//	{
	//		return false;
	//	}
	//
	//	times.actime += times.actime - s.st_atime;
	//	times.modtime += times.modtime - s.st_mtime;
	//
	//	if (0 != utime(n.GetData(), &times))
	//	{
	//		return false;
	//	}
}

void sys_file_find_files(const String& /*path*/, Vector<sys_file_find_t>& /*out*/)
{
	EXIT("not implemented\n");
}

void sys_file_get_dents(const String& path, Lumen::Vector<sys_dir_entry_t>& out)
{
	String real_path = get_internal_name(path).ReplaceChar(U'\\', U'/');
	if (!real_path.EndsWith(U"/"))
	{
		real_path += U"/";
	}

	String::Utf8 real_path_utf8 = real_path.utf8_str();
	DIR*         directory      = opendir(real_path_utf8.GetDataConst());
	if (directory == nullptr)
	{
		return;
	}

	while (const dirent* entry = readdir(directory))
	{
		const String name       = String::FromUtf8(entry->d_name);
		const String entry_path = real_path + name;

		struct stat entry_stat
		{
		};

		sys_dir_entry_t result {};
		result.name    = name;
		result.is_file = stat(entry_path.utf8_str().GetDataConst(), &entry_stat) != 0 || !S_ISDIR(entry_stat.st_mode);
		out.Add(result);
	}

	closedir(directory);
}

bool sys_file_copy_file(const String& /*src*/, const String& /*dst*/)
{
	EXIT("not implemented\n");
	return false;
}

bool sys_file_move_file(const String& src, const String& dst)
{
	const auto real_src = get_internal_name(src).ReplaceChar(U'\\', U'/').utf8_str();
	const auto real_dst = get_internal_name(dst).ReplaceChar(U'\\', U'/').utf8_str();
	return std::rename(real_src.GetDataConst(), real_dst.GetDataConst()) == 0;
}

void sys_file_remove_readonly(const String& /*name*/)
{
	EXIT("not implemented\n");
}

} // namespace Lumen

#endif
