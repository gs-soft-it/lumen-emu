#ifndef EMULATOR_LIBS_NP_IN_GAME_MESSAGE_H_
#define EMULATOR_LIBS_NP_IN_GAME_MESSAGE_H_

#include "Emulator/Common.h"

#include <cstdint>

// Original opt-in initialization profile; no messaging transport or console
// contract is claimed.
namespace Lumen::Libs::NpInGameMessage {

enum class ModelResult { Ok, Disabled, UnsupportedParameter, AlreadyInitialized };

struct ModelSnapshot
{
	bool initialized = false;
	uint64_t initialization_token = 0;
};

bool ExperimentalEnabled();
ModelResult InitializeModel(uint64_t initialization_token, const void* parameter);
ModelSnapshot GetModelSnapshot();
void ResetSessionState();

int32_t LUMEN_SYSV_ABI Initialize(uint64_t initialization_token, const void* parameter);

} // namespace Lumen::Libs::NpInGameMessage

#endif
