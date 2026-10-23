#ifndef SYS_LINUX_INCLUDE_LUMEN_SYSSTDLIB_H_
#define SYS_LINUX_INCLUDE_LUMEN_SYSSTDLIB_H_

// IWYU pragma: private

#if LUMEN_PLATFORM != LUMEN_PLATFORM_ANDROID && LUMEN_PLATFORM != LUMEN_PLATFORM_LINUX && LUMEN_PLATFORM != LUMEN_PLATFORM_OSX && \
    LUMEN_PLATFORM != LUMEN_PLATFORM_IOS
//#error "LUMEN_PLATFORM != LUMEN_PLATFORM_LINUX"
#else

namespace Lumen {

inline float sys_strtof(const char* nptr, char** endptr)
{
	return strtof(nptr, endptr);
}

inline double sys_strtod(const char* nptr, char** endptr)
{
	return strtod(nptr, endptr);
}

inline int32_t sys_strtoi32(const char* nptr, char** endptr, int base)
{
	int64_t r = strtol(nptr, endptr, base);
	if (r >= static_cast<int64_t>(INT32_MAX))
	{
		return INT32_MAX;
	}
	if (r <= static_cast<int64_t>(INT32_MIN))
	{
		return INT32_MIN;
	}
	return static_cast<int32_t>(r);
}

inline uint32_t sys_strtoui32(const char* nptr, char** endptr, int base)
{
	return strtoul(nptr, endptr, base);
}

inline int64_t sys_strtoi64(const char* nptr, char** endptr, int base)
{
	return strtoll(nptr, endptr, base);
}

inline uint64_t sys_strtoui64(const char* nptr, char** endptr, int base)
{
	return strtoull(nptr, endptr, base);
}

} // namespace Lumen

#endif

#endif /* SYS_LINUX_INCLUDE_LUMEN_SYSSTDLIB_H_ */
