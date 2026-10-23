#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERTARGETVULKANCONTRACT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERTARGETVULKANCONTRACT_H_

#include "Emulator/Graphics/RenderTargetFormat.h"

#include <vulkan/vulkan_core.h>

namespace Lumen::Libs::Graphics {

[[nodiscard]] constexpr VkFormat GetRenderTextureVulkanFormat(RenderTextureFormat format)
{
	switch (format)
	{
		case RenderTextureFormat::R8Unorm: return VK_FORMAT_R8_UNORM;
		case RenderTextureFormat::R8G8Unorm: return VK_FORMAT_R8G8_UNORM;
		case RenderTextureFormat::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
		case RenderTextureFormat::R8G8B8A8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
		case RenderTextureFormat::B8G8R8A8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
		case RenderTextureFormat::B8G8R8A8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
		case RenderTextureFormat::Unknown: return VK_FORMAT_UNDEFINED;
	}
	return VK_FORMAT_UNDEFINED;
}

[[nodiscard]] constexpr RenderTextureFormat GetRenderTextureFormatFromVulkan(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8_UNORM: return RenderTextureFormat::R8Unorm;
		case VK_FORMAT_R8G8_UNORM: return RenderTextureFormat::R8G8Unorm;
		case VK_FORMAT_R8G8B8A8_UNORM: return RenderTextureFormat::R8G8B8A8Unorm;
		case VK_FORMAT_R8G8B8A8_SRGB: return RenderTextureFormat::R8G8B8A8Srgb;
		case VK_FORMAT_B8G8R8A8_UNORM: return RenderTextureFormat::B8G8R8A8Unorm;
		case VK_FORMAT_B8G8R8A8_SRGB: return RenderTextureFormat::B8G8R8A8Srgb;
		default: return RenderTextureFormat::Unknown;
	}
}

[[nodiscard]] constexpr VkImageUsageFlags GetRenderTextureImageUsage()
{
	return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	       VK_IMAGE_USAGE_SAMPLED_BIT;
}

[[nodiscard]] constexpr VkFormatFeatureFlags GetRenderTextureRequiredFormatFeatures()
{
	return VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	       VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
	       VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
}

[[nodiscard]] inline VkImageCreateInfo BuildRenderTextureImageCreateInfo(RenderTextureFormat format, uint32_t width, uint32_t height)
{
	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent        = {width, height, 1};
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.format        = GetRenderTextureVulkanFormat(format);
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	image_info.usage         = GetRenderTextureImageUsage();
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
	return image_info;
}

[[nodiscard]] constexpr bool AreRenderTargetCopyFormatsCompatible(VkFormat source, VkFormat destination)
{
	auto texel_block_size = [](VkFormat format) constexpr -> uint32_t {
		switch (format)
		{
			case VK_FORMAT_R8_UNORM: return 1;
			case VK_FORMAT_R8G8_UNORM: return 2;
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB:
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_SRGB: return 4;
			default: return 0;
		}
	};
	const auto source_size      = texel_block_size(source);
	const auto destination_size = texel_block_size(destination);
	return source_size != 0 && source_size == destination_size;
}

} // namespace Lumen::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RENDERTARGETVULKANCONTRACT_H_ */
