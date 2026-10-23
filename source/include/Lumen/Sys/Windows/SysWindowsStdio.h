#ifndef SYS_WIN32_INCLUDE_LUMEN_SYSSTDIO_H_
#define SYS_WIN32_INCLUDE_LUMEN_SYSSTDIO_H_

// IWYU pragma: private

#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
//#error "LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS"
#else

//#include <stdio.h>

namespace Lumen {

inline uint32_t sys_vscprintf(const char *format, va_list argptr)
{
	int len = _vscprintf(format, argptr);
	return len < 0 ? 0 : len;
}

inline uint32_t sys_vsnprintf(char *dest, size_t count, const char *format, va_list args)
{
	int len = _vsnprintf_s(dest, count+1, count, format, args);
	return len < 0 ? 0 : len;
}

} // namespace Lumen

#endif

#endif /* SYS_WIN32_INCLUDE_LUMEN_SYSSTDIO_H_ */
