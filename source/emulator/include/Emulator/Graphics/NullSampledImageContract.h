#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NULLSAMPLEDIMAGECONTRACT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NULLSAMPLEDIMAGECONTRACT_H_

#include <vulkan/vulkan_core.h>

#include <cstdint>

namespace Lumen::Libs::Graphics {

struct NullSampledImageContract
{
	static constexpr uint32_t             width               = 1;
	static constexpr uint32_t             height              = 1;
	static constexpr uint32_t             bytes_per_texel     = 4;
	static constexpr uint32_t             upload_pitch_texels = 64;
	static constexpr uint32_t             upload_bytes        = upload_pitch_texels * bytes_per_texel;
	static constexpr VkFormat             format              = VK_FORMAT_R8G8B8A8_UNORM;
	static constexpr VkImageLayout        final_layout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	static constexpr VkPipelineStageFlags final_stage        = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
};

static_assert(NullSampledImageContract::width == 1 && NullSampledImageContract::height == 1);
static_assert(NullSampledImageContract::upload_bytes == 256);

} // namespace Lumen::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_NULLSAMPLEDIMAGECONTRACT_H_
