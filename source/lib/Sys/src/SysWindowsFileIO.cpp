#include "Lumen/Sys/Windows/SysWindowsFileIO.h"

#include "Lumen/Core/Common.h"

#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
//#error "LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS"
#else

#include "Lumen/Core/MemoryAlloc.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Sys/SysFileIO.h"
#include "Lumen/Sys/SysTimer.h"

#include <windows.h> // IWYU pragma: keep

#include <cerrno>
#include <new>

// IWYU pragma: no_include <fileapi.h>
// IWYU pragma: no_include <handleapi.h>
// IWYU pragma: no_include <minwinbase.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <winbase.h>

namespace Lumen {

using Core::mem_free;
using Core::mem_realloc;

static inline HANDLE LUMEN_INVALID_HANDLE_VALUE()
{
	return INVALID_HANDLE_VALUE; // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
}

static DWORD get_cache_access_type(sys_file_cache_type_t t);

static HANDLE open_read_at_handle(HANDLE handle, DWORD share_mode, sys_file_cache_type_t cache_type)
{
	if (handle == nullptr || handle == LUMEN_INVALID_HANDLE_VALUE())
	{
		return LUMEN_INVALID_HANDLE_VALUE();
	}
	return ReOpenFile(handle, GENERIC_READ, share_mode, get_cache_access_type(cache_type));
}

bool sys_file_io_init()
{
	return true;
}

static DWORD get_cache_access_type(sys_file_cache_type_t t)
{
	if (t == SYS_FILE_CACHE_RANDOM_ACCESS)
	{
		return FILE_FLAG_RANDOM_ACCESS;
	}

	if (t == SYS_FILE_CACHE_SEQUENTIAL_SCAN)
	{
		return SYS_FILE_CACHE_SEQUENTIAL_SCAN;
	}

	return SYS_FILE_CACHE_AUTO;
}

void sys_file_read(void* data, uint32_t size, sys_file_t& f, uint32_t* bytes_read)
{
	if (f.type == SYS_FILE_FILE)
	{
		DWORD w = 0;
		ReadFile(f.handle, data, size, &w, nullptr);
		if (bytes_read != nullptr)
		{
			*bytes_read = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT)
	{
		uint32_t s = size;
		if (f.buf->size != 0u)
		{
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l)
			{
				s = l;
			}
		}
		std::memcpy(data, f.buf->ptr, s);
		f.buf->ptr += s;
		if (bytes_read != nullptr)
		{
			*bytes_read = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN)
	{
		uint32_t s = size;
		if (f.buf->size != 0u)
		{
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l)
			{
				s = l;
			}
		} else
		{
			s = 0;
		}
		std::memcpy(data, f.buf->ptr, s);
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
		if (f.read_at_handle == nullptr || f.read_at_handle == LUMEN_INVALID_HANDLE_VALUE())
		{
			return false;
		}

		OVERLAPPED position {};
		position.Offset     = static_cast<DWORD>(offset);
		position.OffsetHigh = static_cast<DWORD>(offset >> 32u);
		DWORD current       = 0;
		const BOOL ok       = ReadFile(f.read_at_handle, data, size, &current, &position);
		if (ok == 0 && GetLastError() != ERROR_HANDLE_EOF)
		{
			return false;
		}
		if (bytes_read != nullptr)
		{
			*bytes_read = current;
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
		DWORD w = 0;
		WriteFile(f.handle, data, size, &w, nullptr);
		if (bytes_written != nullptr)
		{
			*bytes_written = w;
		}
	} else if (f.type == SYS_FILE_MEMORY_STAT)
	{
		uint32_t s = size;
		if (f.buf->size != 0u)
		{
			uint32_t l = f.buf->size - (f.buf->ptr - f.buf->base);
			if (s > l)
			{
				s = l;
			}
		}
		std::memcpy(f.buf->ptr, data, s);
		f.buf->ptr += s;
		if (bytes_written != nullptr)
		{
			*bytes_written = s;
		}
	} else if (f.type == SYS_FILE_MEMORY_DYN)
	{
		uint32_t pos = f.buf->ptr - f.buf->base;
		if (f.buf->size < pos + size)
		{
			f.buf->base = static_cast<uint8_t*>(mem_realloc(f.buf->base, pos + size));
			f.buf->ptr  = f.buf->base + pos;
			f.buf->size = pos + size;
		}
		std::memcpy(f.buf->ptr, data, size);
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

	HANDLE h_file = nullptr;
	h_file        = CreateFileW(reinterpret_cast<LPCWSTR>(file_name.utf16_str().GetData()),
	                            static_cast<DWORD>(GENERIC_READ) | static_cast<DWORD>(GENERIC_WRITE),
	                            static_cast<DWORD>(FILE_SHARE_READ) | static_cast<DWORD>(FILE_SHARE_WRITE), nullptr, CREATE_ALWAYS,
	                            FILE_ATTRIBUTE_NORMAL, nullptr);

	ret->handle = h_file;
	ret->read_at_handle =
	    open_read_at_handle(h_file, static_cast<DWORD>(FILE_SHARE_READ) | static_cast<DWORD>(FILE_SHARE_WRITE), SYS_FILE_CACHE_AUTO);
	ret->type   = SYS_FILE_FILE;

	return ret;
}

static int exclusive_create_errno(DWORD error)
{
	switch (error)
	{
		case ERROR_FILE_EXISTS:
		case ERROR_ALREADY_EXISTS: return EEXIST;
		case ERROR_FILE_NOT_FOUND:
		case ERROR_PATH_NOT_FOUND: return ENOENT;
		case ERROR_ACCESS_DENIED:
		case ERROR_SHARING_VIOLATION: return EACCES;
		case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
		case ERROR_DISK_FULL:
		case ERROR_HANDLE_DISK_FULL: return ENOSPC;
		case ERROR_WRITE_PROTECT: return EROFS;
		case ERROR_NOT_ENOUGH_MEMORY:
		case ERROR_OUTOFMEMORY: return ENOMEM;
		case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
		case ERROR_DIRECTORY: return ENOTDIR;
		case ERROR_INVALID_NAME:
		case ERROR_INVALID_PARAMETER: return EINVAL;
		default: return EIO;
	}
}

sys_file_t* sys_file_create_exclusive(const String& file_name, bool read, bool write, uint16_t permissions,
                                    int* error_number)
{
	(void)permissions; // Windows uses the inherited native ACL, not POSIX mode bits.
	if (error_number != nullptr) *error_number = 0;
	const auto reject = [error_number](int error) -> sys_file_t* {
		if (error_number != nullptr) *error_number = error;
		return nullptr;
	};
	if (!read && !write) return reject(EINVAL);
	auto* ret = new (std::nothrow) sys_file_t;
	if (ret == nullptr) return reject(ENOMEM);
	const DWORD access = (read ? static_cast<DWORD>(GENERIC_READ) : 0u) |
	                     (write ? static_cast<DWORD>(GENERIC_WRITE) : 0u);
	const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(file_name.utf16_str().GetData()), access,
	                                 share, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (handle == LUMEN_INVALID_HANDLE_VALUE())
	{
		const DWORD error = GetLastError();
		delete ret;
		return reject(exclusive_create_errno(error));
	}
	ret->handle = handle;
	ret->read_at_handle = LUMEN_INVALID_HANDLE_VALUE();
	if (read)
	{
		// The existing positional-read implementation needs a separate cursor.
		// ReOpenFile uses this retained file handle, never the mutable pathname.
		ret->read_at_handle = open_read_at_handle(handle, share, SYS_FILE_CACHE_AUTO);
		if (ret->read_at_handle == LUMEN_INVALID_HANDLE_VALUE())
		{
			const DWORD error = GetLastError();
			CloseHandle(handle);
			delete ret;
			return reject(exclusive_create_errno(error));
		}
	}
	ret->type = SYS_FILE_FILE;
	return ret;
}

sys_file_t* sys_file_open_r(const String& file_name, sys_file_cache_type_t cache_type)
{
	auto* ret = new sys_file_t;

	HANDLE h_file = nullptr;
	h_file = CreateFileW(reinterpret_cast<LPCWSTR>(file_name.utf16_str().GetData()), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                     get_cache_access_type(cache_type), nullptr);

	if (h_file == LUMEN_INVALID_HANDLE_VALUE())
	{
		ret->type = SYS_FILE_ERROR;
	} else
	{
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;
	ret->read_at_handle = open_read_at_handle(h_file, FILE_SHARE_READ, cache_type);

	return ret;
}

sys_file_t* sys_file_open(uint8_t* buf, uint32_t buf_size)
{
	auto* ret = new sys_file_t;

	ret->type           = SYS_FILE_MEMORY_STAT;
	ret->read_at_handle = nullptr;
	ret->buf            = new sys_file_mem_buf_t;
	ret->buf->base      = buf;
	ret->buf->ptr       = buf;
	ret->buf->size      = buf_size;

	return ret;
}

sys_file_t* sys_file_create()
{
	auto* ret = new sys_file_t;

	ret->type           = SYS_FILE_MEMORY_DYN;
	ret->read_at_handle = nullptr;
	ret->buf            = new sys_file_mem_buf_t;
	ret->buf->base      = nullptr;
	ret->buf->ptr       = nullptr;
	ret->buf->size      = 0;

	return ret;
}

sys_file_t* sys_file_open_w(const String& file_name, sys_file_cache_type_t cache_type)
{
	auto* ret = new sys_file_t;

	HANDLE h_file = nullptr;
	h_file        = CreateFileW(reinterpret_cast<LPCWSTR>(file_name.utf16_str().GetData()), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
	                            get_cache_access_type(cache_type), nullptr);

	if (h_file == LUMEN_INVALID_HANDLE_VALUE())
	{
		ret->type = SYS_FILE_ERROR;
	} else
	{
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;
	ret->read_at_handle = LUMEN_INVALID_HANDLE_VALUE();

	return ret;
}

sys_file_t* sys_file_open_rw(const String& file_name, sys_file_cache_type_t cache_type)
{
	auto* ret = new sys_file_t;

	HANDLE h_file = nullptr;
	h_file        = CreateFileW(reinterpret_cast<LPCWSTR>(file_name.utf16_str().GetData()),
	                            static_cast<DWORD>(GENERIC_READ) | static_cast<DWORD>(GENERIC_WRITE),
	                            static_cast<DWORD>(FILE_SHARE_READ) | static_cast<DWORD>(FILE_SHARE_WRITE), nullptr, OPEN_EXISTING,
	                            get_cache_access_type(cache_type), nullptr);

	if (h_file == LUMEN_INVALID_HANDLE_VALUE())
	{
		ret->type = SYS_FILE_ERROR;
	} else
	{
		ret->type = SYS_FILE_FILE;
	}

	ret->handle = h_file;
	ret->read_at_handle =
	    open_read_at_handle(h_file, static_cast<DWORD>(FILE_SHARE_READ) | static_cast<DWORD>(FILE_SHARE_WRITE), cache_type);

	return ret;
}

void sys_file_close(sys_file_t* f)
{
	if (f->type == SYS_FILE_FILE)
	{
		if (f->read_at_handle != nullptr && f->read_at_handle != LUMEN_INVALID_HANDLE_VALUE())
		{
			CloseHandle(f->read_at_handle);
		}
		CloseHandle(f->handle);
	} else if (f->type == SYS_FILE_MEMORY_STAT)
	{
		delete f->buf;
	} else if (f->type == SYS_FILE_MEMORY_DYN)
	{
		mem_free(f->buf->base);
		delete f->buf;
	}

	// f.type = SYS_FILE_ERROR;
	delete f;
}

uint64_t sys_file_size(sys_file_t& f)
{
	if (f.type == SYS_FILE_FILE)
	{
		LARGE_INTEGER s;
		GetFileSizeEx(f.handle, &s);
		return s.QuadPart;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN)
	{
		return f.buf->size;
	}

	return 0;
}

uint64_t sys_file_size(const String& file_name)
{
	LARGE_INTEGER             s;
	WIN32_FILE_ATTRIBUTE_DATA a;

	if (GetFileAttributesExW(reinterpret_cast<LPCWSTR>(file_name.utf16_str().GetData()), GetFileExInfoStandard, &a) == 0)
	{
		return 0;
	}

	s.HighPart = static_cast<LONG>(a.nFileSizeHigh);
	s.LowPart  = a.nFileSizeLow;

	return s.QuadPart;
}

bool sys_file_truncate(sys_file_t& f, uint64_t size)
{
	bool ok = false;
	if (f.type == SYS_FILE_FILE)
	{
		LARGE_INTEGER s {};
		LARGE_INTEGER r {};
		s.QuadPart = 0;
		SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
		s.QuadPart = static_cast<LONGLONG>(size);
		ok         = (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0 && SetEndOfFile(f.handle) != 0);
		SetFilePointerEx(f.handle, r, nullptr, FILE_BEGIN);
	}

	return ok;
}

bool sys_file_seek(sys_file_t& f, uint64_t offset)
{
	bool ok = true;
	if (f.type == SYS_FILE_FILE)
	{
		LARGE_INTEGER s;
		s.QuadPart = static_cast<LONGLONG>(offset);
		ok         = (SetFilePointerEx(f.handle, s, nullptr, FILE_BEGIN) != 0);
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
		LARGE_INTEGER s {};
		LARGE_INTEGER r {};
		s.QuadPart = 0;
		SetFilePointerEx(f.handle, s, &r, FILE_CURRENT);
		// printf("tell: %u\n", r.QuadPart);
		return r.QuadPart;
	}

	if (f.type == SYS_FILE_MEMORY_STAT || f.type == SYS_FILE_MEMORY_DYN)
	{
		return f.buf->ptr - f.buf->base;
	}

	return 0;
}

bool sys_file_is_error(sys_file_t& f)
{
	return f.type == SYS_FILE_ERROR || (f.type == SYS_FILE_FILE && f.handle == LUMEN_INVALID_HANDLE_VALUE());
}

bool sys_file_is_directory_existing(const String& path)
{
	DWORD a = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16_str().GetData()));
	return a != INVALID_FILE_ATTRIBUTES && ((a & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) != 0u);
}

bool sys_file_is_file_existing(const String& name)
{
	DWORD a = GetFileAttributesW(reinterpret_cast<LPCWSTR>(name.utf16_str().GetData()));
	return a != INVALID_FILE_ATTRIBUTES && ((a & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) == 0u);
}

bool sys_file_get_available_space_kb(const String& path, uint64_t* available_space_kb)
{
	if (available_space_kb == nullptr)
	{
		return false;
	}

	ULARGE_INTEGER available
	{
	};

	if (GetDiskFreeSpaceExW(reinterpret_cast<LPCWSTR>(path.utf16_str().GetData()), &available, nullptr, nullptr) == 0)
	{
		return false;
	}

	*available_space_kb = static_cast<uint64_t>(available.QuadPart) / 1024;
	return true;
}

bool sys_file_create_directory(const String& path)
{
	return CreateDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16_str().GetData()), nullptr) != 0;
}

bool sys_file_delete_directory(const String& path)
{
	return RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16_str().GetData())) != 0;
}

bool sys_file_delete_file(const String& name)
{
	return DeleteFileW(reinterpret_cast<LPCWSTR>(name.utf16_str().GetData())) != 0;
}

bool sys_file_flush(sys_file_t& f)
{
	if (f.type == SYS_FILE_FILE && f.handle != LUMEN_INVALID_HANDLE_VALUE())
	{
		return (FlushFileBuffers(f.handle) != 0);
	}

	return false;
}

SysFileTimeStruct sys_file_get_last_access_time_utc(const String& name)
{
	SysFileTimeStruct r {};
	sys_file_t*       f = sys_file_open_r(name);
	r.is_invalid        = (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, &r.time, nullptr) == 0));
	sys_file_close(f);
	return r;
}

SysFileTimeStruct sys_file_get_last_write_time_utc(const String& name)
{
	SysFileTimeStruct r {};
	sys_file_t*       f = sys_file_open_r(name);
	r.is_invalid        = (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, nullptr, &r.time) == 0));
	sys_file_close(f);
	return r;
}

void sys_file_get_last_access_and_write_time_utc(const String& name, SysFileTimeStruct& a, SysFileTimeStruct& w)
{
	// TODO() open file with dwDesiredAccess = 0
	// TODO() open directory
	sys_file_t* f = sys_file_open_r(name);
	a.is_invalid = w.is_invalid = (f->type == SYS_FILE_ERROR || (GetFileTime(f->handle, nullptr, &a.time, &w.time) == 0));
	sys_file_close(f);
}

void sys_file_get_last_access_and_write_time_utc(sys_file_t& f, SysFileTimeStruct& a, SysFileTimeStruct& w)
{
	if (f.type == SYS_FILE_FILE)
	{
		a.is_invalid = w.is_invalid = (GetFileTime(f.handle, nullptr, &a.time, &w.time) == 0);
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

	bool ok = true;

	sys_file_t* f = sys_file_open_w(name);

	ok = !(f->type == SYS_FILE_ERROR || (SetFileTime(f->handle, nullptr, &access.time, nullptr) == 0));

	sys_file_close(f);

	return ok;
}

bool sys_file_set_last_write_time_utc(const String& name, SysFileTimeStruct& write)
{
	if (write.is_invalid)
	{
		return false;
	}

	bool ok = true;

	sys_file_t* f = sys_file_open_w(name);

	ok = !(f->type == SYS_FILE_ERROR || (SetFileTime(f->handle, nullptr, nullptr, &write.time) == 0));

	sys_file_close(f);

	return ok;
}

bool sys_file_set_last_access_and_write_time_utc(const String& name, SysFileTimeStruct& access, SysFileTimeStruct& write)
{
	if (access.is_invalid || write.is_invalid)
	{
		return false;
	}

	bool ok = true;

	sys_file_t* f = sys_file_open_w(name);

	ok = !(f->type == SYS_FILE_ERROR || (SetFileTime(f->handle, nullptr, &access.time, &write.time) == 0));

	sys_file_close(f);

	return ok;
}

void sys_file_find_files(const String& path, Vector<sys_file_find_t>& out)
{
	String real_path = path.ReplaceChar(U'\\', U'/');
	if (!real_path.EndsWith(U"/"))
	{
		real_path += U"/";
	}

	String pattern = real_path + U"*";

	HANDLE           h = nullptr;
	WIN32_FIND_DATAW data;

	h = FindFirstFileW(reinterpret_cast<LPCWSTR>(pattern.utf16_str().GetData()), &data);

	if (h == LUMEN_INVALID_HANDLE_VALUE())
	{
		return;
	}

	do
	{
		String file_name = String::FromUtf16(reinterpret_cast<char16_t*>(data.cFileName));

		if (file_name == U"." || file_name == U"..")
		{
			continue;
		}

		if ((data.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) != 0u)
		{
			sys_file_find_files(real_path + file_name, out);
		} else
		{
			sys_file_find_t r {};

			r.path_with_name              = real_path + file_name;
			r.size                        = (static_cast<uint64_t>(data.nFileSizeHigh) << 32u) + static_cast<uint64_t>(data.nFileSizeLow);
			r.last_access_time.is_invalid = false;
			r.last_access_time.time       = data.ftLastAccessTime;
			r.last_write_time.is_invalid  = false;
			r.last_write_time.time        = data.ftLastWriteTime;

			out.Add(r);
		}

	} while (FindNextFileW(h, &data) != 0);

	FindClose(h);
}

void sys_file_get_dents(const String& path, Lumen::Vector<sys_dir_entry_t>& out)
{
	String real_path = path.ReplaceChar(U'\\', U'/');
	if (!real_path.EndsWith(U"/"))
	{
		real_path += U"/";
	}

	String pattern = real_path + U"*";

	HANDLE           h = nullptr;
	WIN32_FIND_DATAW data;

	h = FindFirstFileW(reinterpret_cast<LPCWSTR>(pattern.utf16_str().GetData()), &data);

	if (h == LUMEN_INVALID_HANDLE_VALUE())
	{
		return;
	}

	do
	{
		String file_name = String::FromUtf16(reinterpret_cast<char16_t*>(data.cFileName));

		sys_dir_entry_t r {};

		r.is_file = ((data.dwFileAttributes & static_cast<DWORD>(FILE_ATTRIBUTE_DIRECTORY)) == 0u);
		r.name    = file_name;

		out.Add(r);

	} while (FindNextFileW(h, &data) != 0);

	FindClose(h);
}

bool sys_file_copy_file(const String& src, const String& dst)
{
	return CopyFileW(reinterpret_cast<LPCWSTR>(src.utf16_str().GetData()), reinterpret_cast<LPCWSTR>(dst.utf16_str().GetData()), FALSE) !=
	       0;
}

bool sys_file_move_file(const String& src, const String& dst)
{
	return MoveFileW(reinterpret_cast<LPCWSTR>(src.utf16_str().GetData()), reinterpret_cast<LPCWSTR>(dst.utf16_str().GetData())) != 0;
}

void sys_file_remove_readonly(const String& name)
{
	String::Utf16 s = name.utf16_str();
	SetFileAttributesW(reinterpret_cast<LPCWSTR>(s.GetData()),
	                   GetFileAttributesW(reinterpret_cast<LPCWSTR>(s.GetData())) & (~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY)));
}

} // namespace Lumen

#endif
