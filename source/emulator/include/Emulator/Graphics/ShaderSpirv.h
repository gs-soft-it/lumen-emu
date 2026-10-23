#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/String8.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class ShaderCode;
struct ShaderBindResources;
struct ShaderTextureResources;
struct ShaderVertexInputInfo;
struct ShaderPixelInputInfo;
struct ShaderComputeInputInfo;
enum class ShaderType;

enum class ShaderComputeImageMutationClass : uint8_t
{
	None,
	UniqueStorageTarget,
	AdjacentSampledToStorage
};

String8 SpirvGenerateSource(const ShaderCode& code, const ShaderVertexInputInfo* vs_input_info, const ShaderPixelInputInfo* ps_input_info,
                            const ShaderComputeInputInfo* cs_input_info);
String8 SpirvGetEmbeddedVs(uint32_t id);
String8 SpirvGetEmbeddedPs(uint32_t id);
bool    ShaderRectListExperimentEnabled();
bool    ShaderRectListInputSupported(const ShaderVertexInputInfo& input);
bool    ShaderVertexResourceProfileRequired(const ShaderVertexInputInfo& input);
bool    ShaderVertexFetchEffectsSupported(const ShaderCode& code, int fetch_register, int table_register,
                                         uint64_t* written_sgprs, const char** rejection_reason = nullptr);
bool    ShaderVertexResourceProfileSupported(const ShaderCode& code, const ShaderVertexInputInfo& input,
                                            const char** rejection_reason = nullptr, uint32_t* rejection_pc = nullptr);
bool    ShaderRectListProfileSupported(const ShaderCode& code, const ShaderVertexInputInfo& input,
                                      const char** rejection_reason = nullptr, uint32_t* rejection_pc = nullptr);
bool    ShaderTextureOperationsSupported(const ShaderCode& code, const ShaderBindResources* bind);
bool    ShaderComputeRequiresPostDispatchImageStoreBarrier(const ShaderCode& code);
ShaderComputeImageMutationClass ShaderClassifyComputeImageMutation(const ShaderCode& code,
	                                                                const ShaderBindResources& bind);
bool    ShaderTextureDescriptorProfileSupportedPreCache(const ShaderTextureResources& textures, ShaderType shader_type);
constexpr uint32_t SHADER_TEXTURE_STORAGE_MIP_PROFILE_LEGACY_ATLAS          = 1u;
constexpr uint32_t SHADER_TEXTURE_STORAGE_MIP_PROFILE_ADJACENT_SINGLE_WINDOW = 2u;
uint32_t ShaderTextureStorageMipProfileId(const ShaderTextureResources& textures);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderExtendedMappingContractTest();
#endif

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSPIRV_H_ */
