#include "Lumen/Core/DbgAssert.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Debug.h"
#include "Lumen/Core/Subsystems.h"

#include <cstdarg>

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include <windows.h> // IWYU pragma: keep
// IWYU pragma: no_include <debugapi.h>
#endif

namespace Lumen::Core {

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_BUILD == LUMEN_BUILD_DEBUG && LUMEN_COMPILER == LUMEN_COMPILER_CLANG
constexpr int PRINT_STACK_FROM = 4;
#else
constexpr int PRINT_STACK_FROM = 2;
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
int IsDebuggerPresent()
{
	bool  dbg = false;
	FILE* f   = fopen("/proc/self/status", "r");

	if (f != nullptr)
	{
		char str[1024];

		while (feof(f) == 0)
		{
			str[1023] = '\0';
			int pid   = 0;

			[[maybe_unused]] auto* result = fgets(str, 1023, f);
			if (sscanf(str, "TracerPid: %d", &pid) == 1) // NOLINT
			{
				dbg = (pid != 0);
				break;
			}
		}

		[[maybe_unused]] auto result = fclose(f);
	}

	return (dbg ? 1 : 0);
}
#endif

void dbg_print_stack()
{
	DebugStack s;
	DebugStack::Trace(&s);

	printf("--- Stack Trace ---\n");
	s.Print(PRINT_STACK_FROM);
}

static void dbg_vprint_error(const char* format, va_list args)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	va_list android_args {};
	va_copy(android_args, args);
#endif
	std::vfprintf(stderr, format, args);
	std::fflush(stderr);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	__android_log_vprint(ANDROID_LOG_ERROR, LUMEN_LOG_TAG, format, android_args);
	va_end(android_args);
#endif
}

static void dbg_print_error(const char* format, ...) LUMEN_FORMAT_PRINTF(1, 2);

static void dbg_print_error(const char* format, ...)
{
	va_list args {};
	va_start(args, format);
	dbg_vprint_error(format, args);
	va_end(args);
}

int dbg_assert_handler(char const* expr, char const* file, int line)
{
	dbg_print_stack();
	dbg_print_error("--- Fatal Error ---\n");
	dbg_print_error("Assertion (%s) failed in %s:%d\n", expr, file, line);
	SubsystemsListSingleton::Instance()->ShutdownAll();
	return 1;
}

int dbg_exit_if_handler(char const* expr, char const* file, int line)
{
	dbg_print_stack();
	dbg_print_error("--- Fatal Error ---\n");
	dbg_print_error("Error: condition (%s) is true in %s:%d\n", expr, file, line);
	SubsystemsListSingleton::Instance()->ShutdownAll();
	return 1;
}

int dbg_not_implemented_handler(char const* expr, char const* file, int line)
{
	dbg_print_stack();
	dbg_print_error("--- Fatal Error ---\n");
	dbg_print_error("Not implemented (%s) in %s:%d\n", expr, file, line);
	SubsystemsListSingleton::Instance()->ShutdownAll();
	return 1;
}

int dbg_exit_handler(char const* file, int line, const char* f, ...)
{
	va_list args {};
	va_start(args, f);

	dbg_print_stack();

	dbg_print_error("--- Error ---\n");
	dbg_vprint_error(f, args);
	dbg_print_error(" in %s:%d\n", file, line);

	SubsystemsListSingleton::Instance()->ShutdownAll();

	va_end(args);

	return 1;
}

void dbg_exit(int status)
{
	// _Exit intentionally bypasses normal stdio cleanup. Flush explicitly so
	// fatal diagnostics are not lost when stdout/stderr are redirected.
	std::fflush(nullptr);
	std::_Exit(status);
}

bool dbg_is_debugger_present()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS || LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX
	return !(IsDebuggerPresent() == 0);
#endif
	return false;
}

} // namespace Lumen::Core
