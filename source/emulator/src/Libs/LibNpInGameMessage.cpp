#include "Emulator/Libs/NpInGameMessage.h"

#include "Emulator/Libs/Libs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::NpInGameMessage {

LIB_NAME("NpManager", "NpManager");

namespace {

std::mutex g_mutex;
ModelSnapshot g_state;

const char* ResultName(ModelResult result)
{
	switch (result)
	{
		case ModelResult::Ok: return "initialized-local";
		case ModelResult::Disabled: return "disabled";
		case ModelResult::UnsupportedParameter: return "non-null-parameter";
		case ModelResult::AlreadyInitialized: return "already-initialized";
	}
	return "unknown";
}

} // namespace

bool ExperimentalEnabled()
{
#if (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && defined(__aarch64__)) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

ModelResult InitializeModel(uint64_t initialization_token, const void* parameter)
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	// There is no supported layout or direction for a non-null parameter.
	if (parameter != nullptr) return ModelResult::UnsupportedParameter;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_state.initialized) return ModelResult::AlreadyInitialized;
	// The scalar is opaque: it never sizes an allocation or addresses memory.
	g_state = {true, initialization_token};
	return ModelResult::Ok;
}

ModelSnapshot GetModelSnapshot()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_state;
}

void ResetSessionState()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_state = {};
}

int32_t LUMEN_SYSV_ABI Initialize(uint64_t initialization_token, const void* parameter)
{
	PRINT_NAME();
	const auto result = InitializeModel(initialization_token, parameter);
	std::fprintf(stderr,
	             "[NP_IN_GAME_MESSAGE_OFFLINE_EXPERIMENT] operation=initialize result=%s "
	             "experimental=1 console_contract=unverified scalar=opaque parameter=null-only networking=0\n",
	             ResultName(result));
	std::fflush(stderr);
	if (result != ModelResult::Ok)
	{
		EXIT("NP in-game messaging offline experiment cannot model initialization: %s", ResultName(result));
		std::abort();
	}
	return 0;
}

} // namespace Lumen::Libs::NpInGameMessage

#endif // LUMEN_EMU_ENABLED
