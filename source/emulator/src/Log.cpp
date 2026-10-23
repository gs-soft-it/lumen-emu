#include "Emulator/Log.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Subsystems.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Config.h"

#include <cstdio>
#include <cstdlib>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
// IWYU pragma: no_include <handleapi.h>
// IWYU pragma: no_include <minwindef.h>
// IWYU pragma: no_include <processenv.h>
// IWYU pragma: no_include <winbase.h>
// IWYU pragma: no_include <wincon.h>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen {

namespace Log {

static bool                     g_log_initialized    = false;
static Core::Mutex*             g_mutex              = nullptr;
static Direction                g_dir                = Direction::Console;
static Core::File*              g_file               = nullptr;
static bool                     g_colored_printf     = false;
static thread_local Core::File* g_thread_local_file  = nullptr;
static Vector<Core::File*>*     g_thread_local_files = nullptr;

static bool EnableVTMode()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	// Set output mode to handle virtual terminal sequences
	HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
	if (h == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	DWORD dw_mode = 0;
	if (GetConsoleMode(h, &dw_mode) == 0)
	{
		return false;
	}

	dw_mode |= static_cast<DWORD>(ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	return (SetConsoleMode(h, dw_mode) != 0);
#endif
	return true;
}

bool IsColoredPrintf()
{
	return g_colored_printf;
}

String RemoveColors(const String& str)
{
	uint32_t start = 0;
	String   ret;
	for (;;)
	{
		auto index = str.FindIndex(U'\x1b', start);
		if (!str.IndexValid(index))
		{
			ret += str.Mid(start);
			break;
		}
		ret += str.Mid(start, index - start);
		index = str.FindIndex(U'm', index);
		if (!str.IndexValid(index))
		{
			break;
		}
		start = index + 1;
	}
	return ret;
}

static void Close()
{
	if (g_log_initialized)
	{
		g_mutex->Lock();
		if (g_dir == Direction::File && g_file != nullptr)
		{
			g_file->Flush();
			g_file->Close();
			delete g_file;
			g_file = nullptr;
		}
		if (g_dir == Direction::Directory && !g_thread_local_files->IsEmpty())
		{
			for (auto* file: *g_thread_local_files)
			{
				file->Flush();
				file->Close();
				delete file;
			}
			g_thread_local_files->Clear();
		}
		g_mutex->Unlock();
	}
}

LUMEN_SUBSYSTEM_INIT(Log)
{
	if (!g_log_initialized)
	{
		g_mutex           = new Core::Mutex;
		g_log_initialized = true;
	}

	auto dir = Config::GetPrintfDirection();
	SetDirection(dir);
	if (dir == Log::Direction::File)
	{
		SetOutputFile(Config::GetPrintfOutputFile());
	}

	g_thread_local_files = new Vector<Core::File*>;
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Log)
{
	Close();
}

LUMEN_SUBSYSTEM_DESTROY(Log)
{
	Close();
}

void SetDirection(Direction dir)
{
	EXIT_IF(!Log::g_log_initialized);
	EXIT_IF(!Core::Thread::IsMainThread());

	if (dir == Direction::Console)
	{
		g_colored_printf = EnableVTMode();

		if (!g_colored_printf)
		{
			::printf("Colored printf is not supported\n");
		}
	} else
	{
		g_colored_printf = false;
	}

	g_dir = dir;
}

Direction GetDirection()
{
	EXIT_IF(!Log::g_log_initialized);

	return g_dir;
}

void SetOutputFile(const String& file_name, Core::File::Encoding enc)
{
	EXIT_IF(!Log::g_log_initialized);
	EXIT_IF(!Core::Thread::IsMainThread());
	EXIT_IF(Log::g_dir != Log::Direction::File);
	EXIT_IF(Log::g_file != nullptr);

	g_file = new Core::File;
	g_file->Create(file_name);

	if (g_file->IsInvalid())
	{
		::printf("Can't create log file: %s\n", file_name.C_Str());
		delete g_file;
		g_file = nullptr;
	} else
	{
		g_file->SetEncoding(enc);
		g_file->WriteBOM();
	}
}

void SetOutputThreadLocalFile(const String& file_name, Core::File::Encoding enc)
{
	EXIT_IF(!Log::g_log_initialized);
	EXIT_IF(Log::g_dir != Log::Direction::Directory);
	EXIT_IF(Log::g_thread_local_file != nullptr);
	EXIT_IF(g_thread_local_files == nullptr);

	Core::File::CreateDirectories(file_name.DirectoryWithoutFilename());

	g_thread_local_file = new Core::File;
	g_thread_local_file->Create(file_name);

	if (g_thread_local_file->IsInvalid())
	{
		::printf("Can't create log file: %s\n", file_name.C_Str());
		delete g_thread_local_file;
		g_thread_local_file = nullptr;
	} else
	{
		g_thread_local_file->SetEncoding(enc);
		g_thread_local_file->WriteBOM();
	}

	g_mutex->Lock();
	g_thread_local_files->Add(g_thread_local_file);
	g_mutex->Unlock();
}

void CreateThreadLocalFile()
{
	auto file_name = String::FromPrintf("%s/%d.txt", Config::GetPrintfOutputFolder().C_Str(), Core::Thread::GetThreadIdUnique());
	SetOutputThreadLocalFile(file_name, Core::File::Encoding::Utf8);
}

} // namespace Log

void emu_printf(const char* format, ...)
{
	EXIT_IF(!Log::g_log_initialized);

	EXIT_IF(Log::g_mutex == nullptr);

	Log::g_mutex->Lock();
	{
		va_list args {};
		va_start(args, format);
		String s;
		s.Printf(format, args);
		va_end(args);

		if (!Log::g_colored_printf)
		{
			s = Log::RemoveColors(s);
		}

		::printf("%s", s.C_Str());

		if (Log::g_dir == Log::Direction::File && Log::g_file != nullptr)
		{
			Log::g_file->Write(s);
		}
	}
	Log::g_mutex->Unlock();
}

void printf(const char* format, ...)
{
	EXIT_IF(!Log::g_log_initialized);

	if (Log::g_dir == Log::Direction::Silent)
	{
		return;
	}

	EXIT_IF(Log::g_mutex == nullptr);

	va_list args {};
	va_start(args, format);
	String s;
	s.Printf(format, args);
	va_end(args);

	if (!Log::g_colored_printf)
	{
		s = Log::RemoveColors(s);
	}

	if (Log::g_dir == Log::Direction::Console)
	{
		Log::g_mutex->Lock();
		::printf("%s", s.C_Str());
		Log::g_mutex->Unlock();
	} else if (Log::g_dir == Log::Direction::File)
	{
		Log::g_mutex->Lock();
		// UnexpectedShutdown can close the file while another thread waits here.
		// Check its lifetime under the same lock used by Close().
		if (Log::g_file != nullptr)
		{
			Log::g_file->Write(s);
		}
		Log::g_mutex->Unlock();
	} else if (Log::g_dir == Log::Direction::Directory)
	{
		if (Log::g_thread_local_file == nullptr)
		{
			Log::CreateThreadLocalFile();
		}
		if (Log::g_thread_local_file != nullptr)
		{
			Log::g_thread_local_file->Write(s);
		}
	}
}

// Cache per evitare chiamate ripetute a getenv
static bool g_custom_log_checked = false;
static bool g_custom_log_enabled = false;

namespace Log {
bool IsCustomLogEnabled()
{
	if (!g_custom_log_checked)
	{
		g_custom_log_enabled = (std::getenv("LUMEN_CUSTOM_LOG") != nullptr);
		g_custom_log_checked = true;
	}
	return g_custom_log_enabled;
}
} // namespace Log

void dbg_printf(const char* format, ...)
{
	va_list args {};
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fflush(stderr);
}

} // namespace Lumen

#endif // LUMEN_EMU_ENABLED
