#ifndef EMULATOR_LIBS_VOICE_H_
#define EMULATOR_LIBS_VOICE_H_

#include "Emulator/Common.h"

#include <array>
#include <cstdint>

// Original administrative profile; Init tokens are opaque and ports use
// the local descriptor format.
// No audio service or network transport is supplied.
namespace Lumen::Libs::Voice {

enum class ModelResult
{
	Ok, Disabled, AlreadyInitialized, NotInitialized, InvalidDescriptor,
	UnsupportedDescriptor, InvalidOutput, CapacityExceeded, IdentifierExhausted, InvalidPort,
	AlreadyStarted, AlreadyStopped, InvalidInfoOutput
};

constexpr uint32_t PORT_CAPACITY = 64;

struct PortSnapshot
{
	bool live = false;
	uint32_t id = 0;
	std::array<uint8_t, 24> descriptor {};
};

struct ModelSnapshot
{
	bool initialized = false;
	uintptr_t configuration_token = 0;
	uint32_t initialization_value = 0;
	uint32_t active_ports = 0;
	bool started = false;
};

bool ExperimentalEnabled();
ModelResult InitializeModel(const void* configuration, uint32_t initialization_value);
ModelSnapshot GetModelSnapshot();
ModelResult CreatePortModel(uint64_t output_address, uint64_t descriptor_address);
ModelResult DeletePortModel(uint32_t port_id);
PortSnapshot GetPortSnapshot(uint32_t port_id);
ModelResult GetPortInfoModel(uint32_t port_id, uint64_t info_address);
ModelResult StartModel();
ModelResult StopModel();
ModelResult EndModel();
void ResetSessionState();

int32_t LUMEN_SYSV_ABI Init(const void* configuration, uint32_t initialization_value);
int32_t LUMEN_SYSV_ABI CreatePort(uint32_t* port_id, const void* descriptor);
int32_t LUMEN_SYSV_ABI DeletePort(uint32_t port_id);
int32_t LUMEN_SYSV_ABI GetPortInfo(uint32_t port_id, void* info);
int32_t LUMEN_SYSV_ABI Start(const void* configuration);
int32_t LUMEN_SYSV_ABI Stop();
int32_t LUMEN_SYSV_ABI End();

} // namespace Lumen::Libs::Voice

#endif
