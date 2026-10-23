#ifndef EMULATOR_LIBS_REMOTEPLAY_H_
#define EMULATOR_LIBS_REMOTEPLAY_H_

#include "Emulator/Common.h"

#include <cstdint>

// An opt-in local model, not a console Remoteplay implementation.
namespace Lumen::Libs::Remoteplay {

enum class ModelResult { Ok, Disabled, InvalidWorkspace, AlreadyInitialized, NotInitialized };
enum class Phase { Stopped, Prohibited, Approved };

struct ModelSnapshot
{
	Phase phase = Phase::Stopped;
	uint64_t workspace_address = 0;
	uint64_t workspace_size = 0;
};

bool ExperimentalEnabled();
ModelResult InitializeModel(uint64_t workspace_address, uint64_t workspace_size);
ModelResult SetApprovalModel(bool approved);
ModelResult TerminateModel();
ModelSnapshot GetModelSnapshot();
void ResetSessionState();

int32_t LUMEN_SYSV_ABI Initialize(void* workspace, uint64_t workspace_size);
int32_t LUMEN_SYSV_ABI Approve();
int32_t LUMEN_SYSV_ABI Prohibit();
int32_t LUMEN_SYSV_ABI Terminate();

} // namespace Lumen::Libs::Remoteplay

#endif
