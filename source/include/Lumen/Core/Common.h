#ifndef INCLUDE_LUMEN_CORE_COMMON_H_
#define INCLUDE_LUMEN_CORE_COMMON_H_

#include "lumen_config.h" // IWYU pragma: export

#define LUMEN_FUNC_(prefix, name)      prefix##name
#define LUMEN_PROJECT_MAIN_FUNC_(name) LUMEN_FUNC_(ProjectMain, name)
#define LUMEN_PROJECT_MAIN_FUNC        LUMEN_PROJECT_MAIN_FUNC_(LUMEN_PROJECT_ID)

#if LUMEN_COMPILER != LUMEN_COMPILER_MSVC
#if __cplusplus < 201703L
#undef __cplusplus
#define __cplusplus 201703L
#endif
#endif

#if (LUMEN_COMPILER == LUMEN_COMPILER_MINGW)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage,cert-dcl51-cpp,cert-dcl37-c,bugprone-reserved-identifier)
#define __USE_MINGW_ANSI_STDIO 1
#endif

// IWYU pragma: begin_exports
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
// IWYU pragma: end_exports

#define LUMEN_FORCE_LINK_THIS(x) int force_link_##x = 0;
#define LUMEN_FORCE_LINK_THAT(x)                                                                                                            \
	void force_link_function_##x()                                                                                                         \
	{                                                                                                                                      \
		extern int force_link_##x;                                                                                                         \
		force_link_##x = 1;                                                                                                                \
	}

#define LUMEN_CLASS_NO_COPY(name)                                                                                                           \
public:                                                                                                                                    \
	name(const name&)                = delete; /* NOLINT(bugprone-macro-parentheses) */                                                    \
	name& operator=(const name&)     = delete; /* NOLINT(bugprone-macro-parentheses) */                                                    \
	name(name&&) noexcept            = delete; /* NOLINT(bugprone-macro-parentheses) */                                                    \
	name& operator=(name&&) noexcept = delete; /* NOLINT(bugprone-macro-parentheses) */

#define LUMEN_CLASS_DEFAULT_COPY(name)                                                                                                      \
public:                                                                                                                                    \
	name(const name&)                = default; /* NOLINT(bugprone-macro-parentheses) */                                                   \
	name& operator=(const name&)     = default; /* NOLINT(bugprone-macro-parentheses) */                                                   \
	name(name&&) noexcept            = default; /* NOLINT(bugprone-macro-parentheses) */                                                   \
	name& operator=(name&&) noexcept = default; /* NOLINT(bugprone-macro-parentheses) */

#if (LUMEN_COMPILER == LUMEN_COMPILER_MINGW || LUMEN_COMPILER == LUMEN_COMPILER_GCC)
#define LUMEN_FORMAT_PRINTF(a, b) __attribute__((format(gnu_printf, a, b)))
#elif LUMEN_COMPILER == LUMEN_COMPILER_CLANG
#define LUMEN_FORMAT_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define LUMEN_FORMAT_PRINTF(a, b)
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#include <android/log.h> // IWYU pragma: export
#define LUMEN_LOG_TAG   "LUMEN_Tag"
#define LUMEN_LOGI(...) __android_log_print(ANDROID_LOG_INFO, LUMEN_LOG_TAG, __VA_ARGS__)
#define LUMEN_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LUMEN_LOG_TAG, __VA_ARGS__)
#else
#define LUMEN_LOGI printf
#define LUMEN_LOGE printf
#endif

#endif /* INCLUDE_LUMEN_CORE_COMMON_H_ */
