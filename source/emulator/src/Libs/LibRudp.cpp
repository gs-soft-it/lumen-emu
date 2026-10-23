#include "Emulator/Libs/Rudp.h"

#include "Emulator/Libs/Libs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Rudp {

LIB_NAME("Rudp", "Rudp");

namespace {

std::mutex g_mutex;
ModelSnapshot g_state;

const char* ResultName(ModelResult result)
{
	switch (result)
	{
		case ModelResult::Ok: return "initialized-local";
		case ModelResult::Disabled: return "disabled";
		case ModelResult::AlreadyInitialized: return "already-initialized";
		case ModelResult::NotInitialized: return "not-initialized";
		case ModelResult::AlreadyEnabled: return "already-requested";
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
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_RUDP_OFFLINE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

ModelResult InitializeModel(const void* workspace, uint64_t initialization_value)
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_state.initialized) return ModelResult::AlreadyInitialized;
	// Neither value supplies a memory layout, allocation size or callback.
	g_state = {true, reinterpret_cast<uintptr_t>(workspace), initialization_value};
	return ModelResult::Ok;
}

ModelResult EnableInternalIOThreadModel(uint32_t first_value, uint32_t second_value)
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_state.initialized) return ModelResult::NotInitialized;
	if (g_state.io_requested) return ModelResult::AlreadyEnabled;
	// This records an administrative request; no host I/O worker is started.
	g_state.io_requested = true;
	g_state.io_value_1 = first_value;
	g_state.io_value_2 = second_value;
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

int32_t LUMEN_SYSV_ABI Init(const void* workspace, uint64_t initialization_value)
{
	PRINT_NAME();
	const auto result = InitializeModel(workspace, initialization_value);
	std::fprintf(stderr,
	             "[RUDP_OFFLINE_EXPERIMENT] operation=initialize result=%s "
	             "experimental=1 console_contract=unverified workspace=opaque-token scalar=opaque networking=0\n",
	             ResultName(result));
	std::fflush(stderr);
	if (result != ModelResult::Ok)
	{
		EXIT("RUDP offline experiment cannot model initialization: %s", ResultName(result));
		std::abort();
	}
	return 0;
}

int32_t LUMEN_SYSV_ABI EnableInternalIOThread(uint32_t first_value, uint32_t second_value)
{
	PRINT_NAME();
	const auto result = EnableInternalIOThreadModel(first_value, second_value);
	const char* outcome = result == ModelResult::Ok ? "io-requested-local" : ResultName(result);
	std::fprintf(stderr,
	             "[RUDP_OFFLINE_EXPERIMENT] operation=enable-internal-io result=%s "
	             "experimental=1 console_contract=unverified scalars=opaque host_thread=0 networking=0\n",
	             outcome);
	std::fflush(stderr);
	if (result != ModelResult::Ok)
	{
		EXIT("RUDP offline experiment cannot model internal I/O request: %s", outcome);
		std::abort();
	}
	return 0;
}

} // namespace Lumen::Libs::Rudp

#endif // LUMEN_EMU_ENABLED
