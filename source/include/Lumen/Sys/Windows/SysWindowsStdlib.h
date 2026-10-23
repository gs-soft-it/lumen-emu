#ifndef SYS_WIN32_INCLUDE_LUMEN_SYSSTDLIB_H_
#define SYS_WIN32_INCLUDE_LUMEN_SYSSTDLIB_H_

// IWYU pragma: private

#if LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
//#error "LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS"
#else

namespace Lumen {

inline float sys_strtof(const char *nptr, char **endptr)
{
	return strtof(nptr, endptr); // @suppress("Invalid arguments")
}

inline double sys_strtod(const char *nptr, char **endptr)
{
	return strtod(nptr, endptr); // @suppress("Invalid arguments")
}

inline int32_t sys_strtoi32(const char *nptr, char **endptr, int base)
{
	return strtol(nptr, endptr, base); // @suppress("Invalid arguments")
}

inline uint32_t sys_strtoui32(const char *nptr, char **endptr, int base)
{
	return strtoul(nptr, endptr, base); // @suppress("Invalid arguments")
}

inline int64_t sys_strtoi64(const char *nptr, char **endptr, int base)
{
	return _strtoi64(nptr, endptr, base); // @suppress("Invalid arguments")
}

inline uint64_t sys_strtoui64(const char *nptr, char **endptr, int base)
{
	return _strtoui64(nptr, endptr, base); // @suppress("Invalid arguments")
}

} // namespace Lumen

#endif

#endif /* SYS_WIN32_INCLUDE_LUMEN_SYSSTDLIB_H_ */
