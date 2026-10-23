#ifndef INCLUDE_LUMEN_CONFIG_H_
#define INCLUDE_LUMEN_CONFIG_H_

#define LUMEN_PLATFORM_WINDOWS 1
#define LUMEN_PLATFORM_ANDROID 2
#define LUMEN_PLATFORM_OSX     3
#define LUMEN_PLATFORM_IOS     4
#define LUMEN_PLATFORM_LINUX   5

#define LUMEN_COMPILER_MINGW 1
#define LUMEN_COMPILER_MSVC  2
#define LUMEN_COMPILER_GCC   3
#define LUMEN_COMPILER_CLANG 4

#define LUMEN_LINKER_LINK     1
#define LUMEN_LINKER_LD       2
#define LUMEN_LINKER_LLD      3
#define LUMEN_LINKER_LLD_LINK 4

#define LUMEN_BUILD_DEBUG   1
#define LUMEN_BUILD_RELEASE 2

#define LUMEN_ENDIAN_BIG    1
#define LUMEN_ENDIAN_LITTLE 2

#define LUMEN_ABI_ARMEABI     1
#define LUMEN_ABI_ARMEABI_V7A 2
#define LUMEN_ABI_ARM64_V8A   3
#define LUMEN_ABI_X86         4
#define LUMEN_ABI_X86_64      5
#define LUMEN_ABI_MIPS        6
#define LUMEN_ABI_MIPS64      7

#define LUMEN_ARM_FLOAT_SOFT 1
#define LUMEN_ARM_FLOAT_HARD 2

#define LUMEN_PROJECT_EMPTY       0
#define LUMEN_PROJECT_EMULATOR    1
#define LUMEN_PROJECT_BUILD_TOOLS 2

#include "cmake_config.h"

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define LUMEN_ABI LUMEN_ABI_ARM64_V8A
#elif defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#define LUMEN_ABI LUMEN_ABI_X86_64
#elif defined(__i386__) || defined(_M_IX86)
#define LUMEN_ABI LUMEN_ABI_X86
#elif defined(__arm__) || defined(_M_ARM)
#define LUMEN_ABI LUMEN_ABI_ARMEABI_V7A
#elif defined(__mips64)
#define LUMEN_ABI LUMEN_ABI_MIPS64
#elif defined(__mips__)
#define LUMEN_ABI LUMEN_ABI_MIPS
#else
#error "Unsupported host ABI"
#endif

#if (LUMEN_ABI == LUMEN_ABI_X86_64 || LUMEN_ABI == LUMEN_ABI_ARM64_V8A || LUMEN_ABI == LUMEN_ABI_MIPS64) && LUMEN_BITNESS != 64
#error "Host ABI and pointer size disagree"
#endif

#if (LUMEN_ABI == LUMEN_ABI_X86 || LUMEN_ABI == LUMEN_ABI_ARMEABI || LUMEN_ABI == LUMEN_ABI_ARMEABI_V7A || LUMEN_ABI == LUMEN_ABI_MIPS) && \
    LUMEN_BITNESS != 32
#error "Host ABI and pointer size disagree"
#endif

#endif /* INCLUDE_LUMEN_CONFIG_H_ */
