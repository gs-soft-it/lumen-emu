#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERPARSE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERPARSE_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class ShaderCode;

bool ShaderVMacLegacyF32ExperimentEnabled();
bool ShaderVMacLegacyF32FormSupported(const ShaderCode& code, uint32_t src0, uint32_t src1, uint32_t dst, bool sdwa, bool dpp,
                                      uint32_t source_abs, uint32_t source_neg, uint32_t omod, bool clamp);
bool ShaderVRsqClampF32ExperimentEnabled();
bool ShaderVRsqClampF32FormSupported(const ShaderCode& code, uint32_t src0, uint32_t dst);
bool ShaderVMulLegacyF32FormSupported(const ShaderCode& code, bool sdwa, bool dpp, uint32_t source_abs, uint32_t source_neg, uint32_t omod,
                                      bool clamp, uint32_t unused_src2);
void ShaderParse(const uint32_t* src, ShaderCode* dst);
// Returns consumed DWORDs, or zero when the owned span cannot contain a
// complete parse, or when S_TTRACEDATA lacks its disabled/zero contract.
// Other unknown/unsupported encodings retain the ordinary errors.
uint32_t ShaderTryParseBounded(const uint32_t* src, uint32_t source_dwords, ShaderCode* dst);

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERPARSE_H_ */
