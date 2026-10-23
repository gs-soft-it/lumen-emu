#ifndef LUMEN_EMULATOR_LIBS_NP_PLUS_H_
#define LUMEN_EMULATOR_LIBS_NP_PLUS_H_

#include "Emulator/Common.h"

#include <cstdint>

namespace Lumen::Libs::NpPlus {

// Original, owner-authorized local model; these are not guest NP error values.
enum class ModelResult
{
	Ok,
	Disabled,
	InvalidRequest,
	InvalidDescriptor,
	UnsupportedFormat,
	InvalidOutput,
};

[[nodiscard]] bool ExperimentalEnabled();

[[nodiscard]] ModelResult CheckModel(int32_t request_id, uint64_t descriptor_address, uint64_t result_address);
int32_t LUMEN_SYSV_ABI CheckPlus(int32_t request_id, const void* descriptor, void* result);

} // namespace Lumen::Libs::NpPlus

#endif // LUMEN_EMULATOR_LIBS_NP_PLUS_H_
