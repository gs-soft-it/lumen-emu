#include "Emulator/Libs/Remoteplay.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("Remoteplay", 1, "Remoteplay", 0, 0);

namespace Remoteplay {
namespace {

std::mutex g_mutex;
ModelSnapshot g_state;

const char* ResultName(ModelResult result)
{
	switch (result)
	{
		case ModelResult::Ok: return "ok";
		case ModelResult::Disabled: return "disabled";
		case ModelResult::InvalidWorkspace: return "invalid-workspace-token";
		case ModelResult::AlreadyInitialized: return "already-initialized";
		case ModelResult::NotInitialized: return "not-initialized";
	}
	return "unknown";
}

int32_t FinishCall(const char* operation, ModelResult result)
{
	std::fprintf(stderr,
	             "[REMOTEPLAY_OFFLINE_EXPERIMENT] operation=%s result=%s "
	             "experimental=1 console_contract=unverified streaming=0\n",
	             operation, ResultName(result));
	std::fflush(stderr);
	if (result != ModelResult::Ok)
	{
		// No service-specific guest errno has been established for these cases.
		EXIT("Remoteplay offline experiment cannot model %s: %s", operation, ResultName(result));
		std::abort();
	}
	return 0;
}

} // namespace

bool ExperimentalEnabled()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A && \
     defined(LUMEN_ANDROID_FEXCORE_RUNTIME))
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

ModelResult InitializeModel(uint64_t workspace_address, uint64_t workspace_size)
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_state.phase != Phase::Stopped) return ModelResult::AlreadyInitialized;
	if (workspace_address == 0 || workspace_size == 0 ||
	    workspace_size > std::numeric_limits<uint64_t>::max() - workspace_address)
		return ModelResult::InvalidWorkspace;
	// This model stores only an opaque token. No guest memory is accessed.
	g_state = {Phase::Prohibited, workspace_address, workspace_size};
	return ModelResult::Ok;
}

ModelResult SetApprovalModel(bool approved)
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_state.phase == Phase::Stopped) return ModelResult::NotInitialized;
	g_state.phase = approved ? Phase::Approved : Phase::Prohibited;
	return ModelResult::Ok;
}

ModelResult TerminateModel()
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_state.phase == Phase::Stopped) return ModelResult::NotInitialized;
	g_state = {};
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

int32_t LUMEN_SYSV_ABI Initialize(void* workspace, uint64_t workspace_size)
{
	PRINT_NAME();
	return FinishCall("initialize", InitializeModel(reinterpret_cast<uintptr_t>(workspace), workspace_size));
}

int32_t LUMEN_SYSV_ABI Approve()
{
	PRINT_NAME();
	return FinishCall("approve", SetApprovalModel(true));
}

int32_t LUMEN_SYSV_ABI Prohibit()
{
	PRINT_NAME();
	return FinishCall("prohibit", SetApprovalModel(false));
}

int32_t LUMEN_SYSV_ABI Terminate()
{
	PRINT_NAME();
	return FinishCall("terminate", TerminateModel());
}

} // namespace Remoteplay

LIB_DEFINE(InitRemoteplay_1)
{
	if (!Remoteplay::ExperimentalEnabled()) return;
	std::fprintf(stderr,
	             "[REMOTEPLAY_OFFLINE_EXPERIMENT] enabled=1 exports=4 "
	             "experimental=1 console_contract=unverified workspace=opaque-token streaming=0\n");
	LIB_FUNC("k1SwgkMSOM8", Remoteplay::Initialize);
	LIB_FUNC("xQeIryTX7dY", Remoteplay::Approve);
	LIB_FUNC("mrNh78tBpmg", Remoteplay::Prohibit);
	LIB_FUNC("BOwybKVa3Do", Remoteplay::Terminate);
}

} // namespace Lumen::Libs

#endif
