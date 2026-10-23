// Lumen modification, 2026-10-02: normalize the Android FEX gate before type declarations.
#ifndef EMULATOR_INCLUDE_EMULATOR_COMMON_H_
#define EMULATOR_INCLUDE_EMULATOR_COMMON_H_

#include "Lumen/Core/Common.h"

// All emulator headers must see the same backend before declaring conditional
// members. Defining this only in RuntimeLinker.h made the Umtx lease depend on
// include order in Android translation units.
#if defined(LUMEN_ANDROID_FEXCORE_RUNTIME) && !defined(LUMEN_FEXCORE_RUNTIME)
#define LUMEN_FEXCORE_RUNTIME 1
#endif

#if LUMEN_ABI == LUMEN_ABI_X86_64
#define LUMEN_EMU_HOST_X64
#elif LUMEN_ABI == LUMEN_ABI_ARM64_V8A
#define LUMEN_EMU_HOST_ARM64
#endif

#if (LUMEN_COMPILER == LUMEN_COMPILER_CLANG || LUMEN_COMPILER == LUMEN_COMPILER_MINGW || LUMEN_COMPILER == LUMEN_COMPILER_GCC) &&               \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID || LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX ||       \
     LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS) &&                                                        \
    LUMEN_BITNESS == 64 && LUMEN_ENDIAN == LUMEN_ENDIAN_LITTLE &&                                                                             \
    (defined(LUMEN_EMU_HOST_X64) || defined(LUMEN_EMU_HOST_ARM64)) && LUMEN_PROJECT == LUMEN_PROJECT_EMULATOR
#define LUMEN_EMU_ENABLED
#endif

#ifdef LUMEN_EMU_ENABLED

#include "Emulator/Log.h" // IWYU pragma: export

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LUMEN_MS_ABI __attribute__((ms_abi))

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LUMEN_SYSV_ABI __attribute__((sysv_abi))

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_COMMON_H_ */
