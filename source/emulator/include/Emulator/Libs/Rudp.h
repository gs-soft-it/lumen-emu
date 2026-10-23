#ifndef EMULATOR_LIBS_RUDP_H_
#define EMULATOR_LIBS_RUDP_H_

#include "Emulator/Common.h"

#include <cstdint>

// Original administrative experiment. No pointer access or RUDP transport.

namespace Lumen::Libs::Rudp {

enum class ModelResult { Ok, Disabled, AlreadyInitialized, NotInitialized, AlreadyEnabled };

struct ModelSnapshot
{
	bool initialized = false;
	uintptr_t workspace_token = 0;
	uint64_t initialization_value = 0;
	bool io_requested = false;
	uint32_t io_value_1 = 0;
	uint32_t io_value_2 = 0;
};

bool ExperimentalEnabled();
ModelResult InitializeModel(const void* workspace, uint64_t initialization_value);
ModelResult EnableInternalIOThreadModel(uint32_t first_value, uint32_t second_value);
ModelSnapshot GetModelSnapshot();
void ResetSessionState();

int32_t LUMEN_SYSV_ABI Init(const void* workspace, uint64_t initialization_value);
int32_t LUMEN_SYSV_ABI EnableInternalIOThread(uint32_t first_value, uint32_t second_value);

} // namespace Lumen::Libs::Rudp

#endif
