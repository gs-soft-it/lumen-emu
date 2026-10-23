#ifndef LUMEN_EMULATOR_LIBS_NP_PARENTAL_H_
#define LUMEN_EMULATOR_LIBS_NP_PARENTAL_H_

#include "Emulator/Common.h"

#include <cstdint>

namespace Lumen::Libs::NpParental {

// Original local outcomes, not guest NP error values or a console contract.
enum class ModelResult
{
	Ok,
	Disabled,
	InvalidRequest,
	InvalidIdentifier,
	InvalidAgeOutput,
	InvalidRestrictionsOutput,
	OverlappingOutputs,
};

[[nodiscard]] bool ExperimentalEnabled();
[[nodiscard]] ModelResult QueryModel(int32_t request_id, uint64_t identifier_address,
                                     uint64_t age_address, uint64_t restrictions_address);
int32_t LUMEN_SYSV_ABI GetParentalControlInfo(int32_t request_id, const void* identifier, void* age, void* restrictions);

} // namespace Lumen::Libs::NpParental

#endif // LUMEN_EMULATOR_LIBS_NP_PARENTAL_H_
