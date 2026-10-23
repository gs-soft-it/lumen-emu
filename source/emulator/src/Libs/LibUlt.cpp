#include "Emulator/Libs/Ult.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("libSceUlt", 1, "libSceUlt", 1, 1);

namespace Ult {
namespace {

std::atomic<bool> g_initialized {false};

} // namespace

bool ExperimentalEnabled()
{
	static const bool enabled = [] {
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
    LUMEN_ABI == LUMEN_ABI_ARM64_V8A
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_ULT_INITIALIZE");
		return value != nullptr && std::strcmp(value, "1") == 0;
#else
		return false;
#endif
	}();
	return enabled;
}

void ResetSessionState()
{
	g_initialized.store(false, std::memory_order_release);
}

int32_t LUMEN_SYSV_ABI Initialize()
{
	if (!ExperimentalEnabled())
	{
		std::fprintf(stderr,
		             "[ULT_INITIALIZE_LOCAL_EXPERIMENT] result=disabled experimental=0 "
		             "console_contract=unverified resources=0 guest_memory=0\n");
		std::fflush(stderr);
		EXIT("ULT Initialize local experiment is disabled");
		std::abort();
	}

	const bool repeat = g_initialized.exchange(true, std::memory_order_acq_rel);
	std::fprintf(stderr,
	             "[ULT_INITIALIZE_LOCAL_EXPERIMENT] result=complete-local call=%s "
	             "experimental=1 console_contract=unverified resources=0 guest_memory=0\n",
	             repeat ? "repeat" : "first");
	std::fflush(stderr);
	return 0;
}

} // namespace Ult

LIB_DEFINE(InitUlt_1)
{
	if (!Ult::ExperimentalEnabled()) return;

	Loader::SymbolResolve symbol {};
	symbol.name                  = "hZIg1EWGsHM";
	symbol.library               = g_library;
	symbol.library_version       = g_library_version;
	symbol.module                = g_module;
	symbol.module_version_major  = g_module_version_major;
	symbol.module_version_minor  = g_module_version_minor;
	symbol.type                  = Loader::SymbolType::Func;
	const auto target            = reinterpret_cast<uint64_t>(Ult::Initialize);
	const char32_t* debug_name   = U"Ult::Initialize";
	constexpr auto metadata      = Loader::MakeHostCallScalarMetadata<&Ult::Initialize>();

	if (!s->RegisterHostCallTarget(target))
	{
		EXIT("Unable to register host-call target for %s", "hZIg1EWGsHM");
		return;
	}
	if (!s->SetHostCallScalarMetadata(target, metadata))
	{
		EXIT("Unable to register scalar host-call metadata for %s", "hZIg1EWGsHM");
		return;
	}
	s->AddExact(symbol, target, debug_name);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
