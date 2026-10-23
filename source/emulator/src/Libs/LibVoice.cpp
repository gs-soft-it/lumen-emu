#include "Emulator/Libs/Voice.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Voice {

LIB_NAME("Voice", "Voice");

namespace {

std::mutex g_mutex;
ModelSnapshot g_state;
std::array<PortSnapshot, PORT_CAPACITY> g_ports;
[[maybe_unused]] uint64_t g_next_port_id = 1;

const char* ResultName(ModelResult result)
{
	switch (result)
	{
		case ModelResult::Ok: return "initialized-local";
		case ModelResult::Disabled: return "disabled";
		case ModelResult::AlreadyInitialized: return "already-initialized";
		case ModelResult::NotInitialized: return "not-initialized";
		case ModelResult::InvalidDescriptor: return "descriptor-unreadable";
		case ModelResult::UnsupportedDescriptor: return "descriptor-type-unsupported";
		case ModelResult::InvalidOutput: return "identifier-unwritable";
		case ModelResult::CapacityExceeded: return "port-capacity-exceeded";
		case ModelResult::IdentifierExhausted: return "identifier-exhausted";
		case ModelResult::InvalidPort: return "port-not-live";
		case ModelResult::AlreadyStarted: return "already-started";
		case ModelResult::AlreadyStopped: return "already-stopped";
		case ModelResult::InvalidInfoOutput: return "info-unwritable";
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
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_VOICE_OFFLINE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
#else
	return false;
#endif
}

ModelResult InitializeModel(const void* configuration, uint32_t initialization_value)
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_state.initialized) return ModelResult::AlreadyInitialized;
	// No record layout or callback contract is assumed from this token.
	g_state = {true, reinterpret_cast<uintptr_t>(configuration), initialization_value};
	return ModelResult::Ok;
}

ModelSnapshot GetModelSnapshot()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_state;
}

ModelResult CreatePortModel(uint64_t output_address, uint64_t descriptor_address)
{
#if !(defined(__aarch64__) || \
      (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64))
	(void)output_address;
	(void)descriptor_address;
	return ModelResult::Disabled;
#else
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_state.initialized) return ModelResult::NotInitialized;

	// Original local format: copy the complete descriptor before publishing an
	// ID, including when output aliases input. The variant stays opaque.
	PortSnapshot candidate;
	if (!Loader::ReadGuestMemory(descriptor_address, candidate.descriptor.data(), candidate.descriptor.size()))
		return ModelResult::InvalidDescriptor;
	uint32_t type = 0;
	for (size_t byte = 0; byte < sizeof(type); ++byte)
		type |= uint32_t(candidate.descriptor[byte]) << (byte * 8);
	if (type > 5) return ModelResult::UnsupportedDescriptor;
	if (!Loader::ValidateGuestMemory(output_address, sizeof(uint32_t), true)) return ModelResult::InvalidOutput;
	auto slot = std::find_if(g_ports.begin(), g_ports.end(), [](const auto& port) { return !port.live; });
	if (slot == g_ports.end()) return ModelResult::CapacityExceeded;
	if (g_next_port_id > UINT32_MAX) return ModelResult::IdentifierExhausted;

	candidate.live = true;
	candidate.id = static_cast<uint32_t>(g_next_port_id);
	std::array<uint8_t, 4> output {};
	for (size_t byte = 0; byte < output.size(); ++byte)
		output[byte] = static_cast<uint8_t>(candidate.id >> (byte * 8));
	*slot = candidate;
	if (!Loader::WriteGuestMemory(output_address, output.data(), output.size()))
	{
		*slot = {};
		return ModelResult::InvalidOutput;
	}
	++g_next_port_id;
	++g_state.active_ports;
	return ModelResult::Ok;
#endif
}

ModelResult DeletePortModel(uint32_t port_id)
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_state.initialized) return ModelResult::NotInitialized;
	auto slot = std::find_if(g_ports.begin(), g_ports.end(), [port_id](const auto& port) { return port.live && port.id == port_id; });
	if (slot == g_ports.end()) return ModelResult::InvalidPort;
	*slot = {};
	--g_state.active_ports;
	return ModelResult::Ok;
}

PortSnapshot GetPortSnapshot(uint32_t port_id)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (const auto& port: g_ports)
		if (port.live && port.id == port_id) return port;
	return {};
}

ModelResult GetPortInfoModel(uint32_t port_id, uint64_t info_address)
{
#if !(defined(__aarch64__) || \
      (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64))
	(void)port_id;
	(void)info_address;
	return ModelResult::Disabled;
#else
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_state.initialized) return ModelResult::NotInitialized;
	const auto port = std::find_if(g_ports.begin(), g_ports.end(), [port_id](const auto& item) {
		return item.live && item.id == port_id;
	});
	if (port == g_ports.end()) return ModelResult::InvalidPort;

	// Original admin-empty profile: retained type, idle state, no audio or
	// topology. Start is administrative, not evidence of an active stream.
	// Serialize only the selected fields; never read caller storage, follow
	// its nested pointer, or touch the four bytes of candidate tail padding.
	std::array<uint8_t, 28> info {};
	std::copy_n(port->descriptor.begin(), 4, info.begin());
	if (!Loader::ValidateGuestMemory(info_address, info.size(), true) ||
	    !Loader::WriteGuestMemory(info_address, info.data(), info.size()))
		return ModelResult::InvalidInfoOutput;
	return ModelResult::Ok;
#endif
}

ModelResult StartModel()
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_state.initialized) return ModelResult::NotInitialized;
	if (g_state.started) return ModelResult::AlreadyStarted;
	g_state.started = true;
	return ModelResult::Ok;
}

ModelResult StopModel()
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_state.initialized) return ModelResult::NotInitialized;
	if (!g_state.started) return ModelResult::AlreadyStopped;
	g_state.started = false;
	return ModelResult::Ok;
}

ModelResult EndModel()
{
	if (!ExperimentalEnabled()) return ModelResult::Disabled;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_state.initialized) return ModelResult::NotInitialized;
	g_state = {};
	g_ports = {};
	return ModelResult::Ok;
}

void ResetSessionState()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_state = {};
	g_ports = {};
}

int32_t LUMEN_SYSV_ABI Init(const void* configuration, uint32_t initialization_value)
{
	PRINT_NAME();
	const auto result = InitializeModel(configuration, initialization_value);
	std::fprintf(stderr,
	             "[VOICE_OFFLINE_EXPERIMENT] operation=initialize result=%s "
	             "experimental=1 console_contract=unverified configuration=opaque-token scalar=opaque audio=0 networking=0\n",
	             ResultName(result));
	std::fflush(stderr);
	if (result != ModelResult::Ok)
	{
		EXIT("Voice offline experiment cannot model initialization: %s", ResultName(result));
		std::abort();
	}
	return 0;
}

static int32_t FinishPortOperation(const char* operation, ModelResult result,
                                  const char* profile = "descriptor=local24 identifier=local32")
{
	std::fprintf(stderr,
	             "[VOICE_OFFLINE_EXPERIMENT] operation=%s result=%s experimental=1 "
	             "console_contract=unverified %s audio=0 networking=0\n",
	             operation, result == ModelResult::Ok ? "complete-local" : ResultName(result), profile);
	std::fflush(stderr);
	if (result != ModelResult::Ok)
	{
		EXIT("Voice offline experiment cannot model %s: %s", operation, ResultName(result));
		std::abort();
	}
	return 0;
}

int32_t LUMEN_SYSV_ABI CreatePort(uint32_t* port_id, const void* descriptor)
{
	PRINT_NAME();
	return FinishPortOperation("create-port", CreatePortModel(reinterpret_cast<uint64_t>(port_id), reinterpret_cast<uint64_t>(descriptor)));
}

int32_t LUMEN_SYSV_ABI DeletePort(uint32_t port_id)
{
	PRINT_NAME();
	return FinishPortOperation("delete-port", DeletePortModel(port_id));
}

int32_t LUMEN_SYSV_ABI GetPortInfo(uint32_t port_id, void* info)
{
	PRINT_NAME();
	return FinishPortOperation("get-port-info", GetPortInfoModel(port_id, reinterpret_cast<uint64_t>(info)),
	                           "info=local28 state=idle topology=empty");
}

int32_t LUMEN_SYSV_ABI Start(const void* /*configuration*/)
{
	PRINT_NAME();
	return FinishPortOperation("start", StartModel());
}

int32_t LUMEN_SYSV_ABI Stop()
{
	PRINT_NAME();
	return FinishPortOperation("stop", StopModel());
}

int32_t LUMEN_SYSV_ABI End()
{
	PRINT_NAME();
	return FinishPortOperation("end", EndModel());
}

} // namespace Lumen::Libs::Voice

#endif // LUMEN_EMU_ENABLED
