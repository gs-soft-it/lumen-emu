#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/Timer.h" // IWYU pragma: keep

#include <cinttypes>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifdef LUMEN_EMU_ENABLED

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLED g_print_name

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLE(flag) PRINT_NAME_ENABLED = flag;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_DEFINE(name) void name(Loader::SymbolDatabase* s)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_NAME(l, m)                                                                                                                     \
	[[maybe_unused]] static thread_local bool PRINT_NAME_ENABLED = true;                                                                   \
	static constexpr char                     g_library[]        = l;                                                                      \
	static constexpr char                     g_module[]         = m;
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_VERSION(l, lv, m, mv1, mv2)                                                                                                    \
	LIB_NAME(l, m);                                                                                                                        \
	static constexpr int g_library_version      = lv;                                                                                      \
	static constexpr int g_module_version_major = mv1;                                                                                     \
	static constexpr int g_module_version_minor = mv2;
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_USING(n)                                                                                                                       \
	using n::g_library;                                                                                                                    \
	using n::g_library_version;                                                                                                            \
	using n::g_module;                                                                                                                     \
	using n::g_module_version_major;                                                                                                       \
	using n::g_module_version_minor;
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_LOAD(name) name(s)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_CHECK(ids, name)                                                                                                               \
	if (id == (ids))                                                                                                                       \
	{                                                                                                                                      \
		LIB_LOAD(name);                                                                                                                    \
		return true;                                                                                                                       \
	}
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_ADD(n, f, t)                                                                                                                   \
	{                                                                                                                                      \
		Loader::SymbolResolve sr {};                                                                                                       \
		sr.name                  = n;                                                                                                      \
		sr.library               = g_library;                                                                                              \
		sr.library_version       = g_library_version;                                                                                      \
		sr.module                = g_module;                                                                                               \
		sr.module_version_major  = g_module_version_major;                                                                                 \
		sr.module_version_minor  = g_module_version_minor;                                                                                 \
		sr.type                  = t;                                                                                                      \
		auto            func     = reinterpret_cast<uint64_t>(f);                                                                          \
		const char32_t* dbg_name = U"" #f;                                                                                                 \
		printf("[LIB_ADD] %s[%s_v%d][%s_v%d.%d][%s]\n", n, g_library, g_library_version, g_module, g_module_version_major, g_module_version_minor, t == Loader::SymbolType::Func ? "Func" : "Object"); \
		if (sr.type == Loader::SymbolType::Func && !s->RegisterHostCallTarget(func))                                                       \
		{                                                                                                                                  \
			EXIT("Unable to register host-call target for %s", n);                                                                         \
			return;                                                                                                                          \
		}                                                                                                                                  \
		s->Add(sr, func, dbg_name);                                                                                                        \
	}
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_OBJECT(n, f) LIB_ADD(n, f, Loader::SymbolType::Object)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC(n, f) LIB_ADD(n, f, Loader::SymbolType::Func)
// Opt in only exports with a verified, fixed scalar signature. Legacy exports
// retain their existing bridge and registration contract.
#define LIB_FUNC_SCALAR(n, f)                                                                                                             \
	{                                                                                                                                      \
		constexpr auto scalar_metadata = Loader::MakeHostCallScalarMetadata<&f>();                                                        \
		LIB_ADD(n, f, Loader::SymbolType::Func);                                                                                            \
		EXIT_IF(!s->SetHostCallScalarMetadata(reinterpret_cast<uint64_t>(f), scalar_metadata));                                            \
	}
// Explicit service effects are separate from ABI metadata. Unclassified
// exports retain conservative admission, including legacy eight-GP adapters.
#define LIB_FUNC_EFFECTS(n, f, effects) \
	{ LIB_FUNC(n, f); EXIT_IF(!s->SetHostCallEffects(reinterpret_cast<uint64_t>(f), effects)); }
#define LIB_FUNC_SCALAR_EFFECTS(n, f, effects) \
	{ LIB_FUNC_SCALAR(n, f); EXIT_IF(!s->SetHostCallEffects(reinterpret_cast<uint64_t>(f), effects)); }
// Register a fixed general-purpose HLE ABI whose arity exceeds the legacy
// eight-word bridge. Variadic/vector ABIs remain legacy and out of scope.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC_GP_ARITY(n, f, gp_arity)                                                                                                  \
	{                                                                                                                                      \
		constexpr auto host_call_metadata = Loader::MakeHostCallGpMetadata<&f>();                                                          \
		static_assert((gp_arity) > 8 && (gp_arity) <= 12);                                                                                  \
		static_assert(host_call_metadata.arity == (gp_arity));                                                                             \
		LIB_ADD(n, f, Loader::SymbolType::Func);                                                                                            \
		EXIT_IF(!s->SetHostCallGpMetadata(reinterpret_cast<uint64_t>(f), host_call_metadata));                                              \
	}

// Macro to register a function with a standard name (POSIX-style, for OpenOrbis homebrew)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC_STD(std_name, f)                                                                                                          \
	{                                                                                                                                      \
		auto            func     = reinterpret_cast<uint64_t>(f);                                                                          \
		const char32_t* dbg_name = U"" #f;                                                                                                 \
		if (!s->RegisterHostCallTarget(func))                                                                                              \
		{                                                                                                                                  \
			EXIT("Unable to register standard host-call target for %s", std_name);                                                          \
			return;                                                                                                                          \
		}                                                                                                                                  \
		s->AddByStdName(Lumen::Core::String::FromUtf8(std_name), func, dbg_name);                                                           \
	}

// Get return address (caller) - works on MSVC and GCC/Clang
#if defined(_MSC_VER)
#define GET_RETURN_ADDRESS() reinterpret_cast<uint64_t>(_ReturnAddress())
#else
#define GET_RETURN_ADDRESS() reinterpret_cast<uint64_t>(__builtin_return_address(0))
#endif

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME()                                                                                                                       \
	if (PRINT_NAME_ENABLED && Lumen::Libs::TraceOrLogEnabled())                                                                             \
	{                                                                                                                                      \
		uint64_t _caller_addr = GET_RETURN_ADDRESS();                                                                                      \
		Lumen::Libs::TraceCallWithAddr(__func__, _caller_addr);                                                                             \
		if (Lumen::Log::GetDirection() != Lumen::Log::Direction::Silent)                                                                     \
		{                                                                                                                                  \
			Lumen::printf(FG_CYAN "[%d][%s] %s::%s::%s() [caller: 0x%016" PRIx64 "]" DEFAULT "\n", Core::Thread::GetThreadIdUnique(),       \
			             Loader::Timer::GetTime().ToString("HH24:MI:SS.FFF").C_Str(), g_library, g_module, __func__, _caller_addr);        \
		}                                                                                                                                  \
	}

namespace Lumen {

namespace Loader {
class SymbolDatabase;
} // namespace Loader

namespace Libs {

void TraceCall(const char* name);
void TraceCallWithAddr(const char* name, uint64_t caller_addr);
void DumpLastCalls();
bool TraceOrLogEnabled();

bool Init(const String& id, Loader::SymbolDatabase* s);
void InitAll(Loader::SymbolDatabase* s);

// Clears HLE state that may retain guest pointers or per-title counters. The
// process-lifetime symbol implementations remain registered and are ready to
// populate the next session's SymbolDatabase.
[[nodiscard]] bool ResetSessionState();

// Completes the memory phase after Kernel::Memory has released all guest
// mappings and detached its instrumentation callbacks.
[[nodiscard]] bool ResetMemoryInstrumentationSessionState();

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool HleSessionResetContractTest();
#endif

} // namespace Libs
} // namespace Lumen

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_ */
