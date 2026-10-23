#ifndef EMULATOR_INCLUDE_EMULATOR_PROFILER_H_
#define EMULATOR_INCLUDE_EMULATOR_PROFILER_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Subsystems.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

#if LUMEN_ENABLE_EASY_PROFILER
#define BUILD_WITH_EASY_PROFILER
#else
// EasyProfiler keeps a header-only no-op API when instrumentation is disabled.
// The static library remains available so profiler-facing code keeps compiling.
#define DISABLE_EASY_PROFILER
#endif
#define EASY_PROFILER_STATIC

#include <easy/profiler.h> // IWYU pragma: export

//
#include "easy/details/profiler_aux.h"    // IWYU pragma: export
#include "easy/details/profiler_colors.h" // IWYU pragma: export

#include <cwchar> // IWYU pragma: export

// IWYU pragma: no_include "easy/profiler.h"

#if LUMEN_COMPILER == LUMEN_COMPILER_MSVC
#define LUMEN_PROFILER_BLOCK(f, ...) EASY_BLOCK(f, __VA_ARGS__);
#else
#define LUMEN_PROFILER_BLOCK(f, s...) EASY_BLOCK(f, ##s);
#endif

#if LUMEN_COMPILER == LUMEN_COMPILER_MSVC
#define LUMEN_PROFILER_FUNCTION(f, ...) EASY_FUNCTION(f, __VA_ARGS__);
#else
#define LUMEN_PROFILER_FUNCTION(f, s...) EASY_FUNCTION(f, ##s);
#endif

#define LUMEN_PROFILER_END_BLOCK EASY_END_BLOCK

#define LUMEN_PROFILER_THREAD(f) EASY_THREAD(f)

namespace Lumen::Profiler {

LUMEN_SUBSYSTEM_DEFINE(Profiler);

} // namespace Lumen::Profiler

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_PROFILER_H_ */
