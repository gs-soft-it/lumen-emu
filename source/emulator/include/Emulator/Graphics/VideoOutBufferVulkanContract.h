#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTBUFFERVULKANCONTRACT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTBUFFERVULKANCONTRACT_H_

#include "Emulator/Graphics/RenderTargetVulkanContract.h"

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace Lumen::Libs::Graphics {

enum class VideoOutSampleView : uint8_t
{
	Unsupported,
	NativeDefault,
	NativeBgra,
	UnormDefault,
	UnormBgra,
};

struct VideoOutSampleViewInfo
{
	VideoOutSampleView view   = VideoOutSampleView::Unsupported;
	VkFormat           format = VK_FORMAT_UNDEFINED;

	[[nodiscard]] constexpr bool IsSupported() const
	{
		return view != VideoOutSampleView::Unsupported && format != VK_FORMAT_UNDEFINED;
	}
};

[[nodiscard]] constexpr VkFormat GetVideoOutUnormViewFormat(VkFormat native_format)
{
	switch (native_format)
	{
		case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
		case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
		default: return VK_FORMAT_UNDEFINED;
	}
}

[[nodiscard]] constexpr VideoOutSampleViewInfo ResolveVideoOutSampleView(
	VkFormat native_format, RenderTextureFormat sampled_format, uint32_t component_swizzle)
{
	VkFormat view_format = VK_FORMAT_UNDEFINED;
	bool     unorm_view  = false;
	switch (sampled_format)
	{
		case RenderTextureFormat::R8G8B8A8Srgb:
			view_format = native_format;
			break;
		case RenderTextureFormat::R8G8B8A8Unorm:
			view_format = GetVideoOutUnormViewFormat(native_format);
			unorm_view  = true;
			break;
		default: return {};
	}

	const auto view_render_format = GetRenderTextureFormatFromVulkan(view_format);
	const auto component_view = SelectRenderTextureSampleView(view_render_format, sampled_format, component_swizzle);
	if (component_view == RenderTextureSampleView::Unsupported)
	{
		return {};
	}
	if (unorm_view)
	{
		return {component_view == RenderTextureSampleView::Default ? VideoOutSampleView::UnormDefault
		                                                              : VideoOutSampleView::UnormBgra,
		        view_format};
	}
	return {component_view == RenderTextureSampleView::Default ? VideoOutSampleView::NativeDefault
	                                                          : VideoOutSampleView::NativeBgra,
	        view_format};
}

[[nodiscard]] constexpr int GetVideoOutSampleViewIndex(VideoOutSampleView view)
{
	switch (view)
	{
		case VideoOutSampleView::NativeDefault: return 0;
		case VideoOutSampleView::NativeBgra: return 1;
		case VideoOutSampleView::UnormDefault: return 2;
		case VideoOutSampleView::UnormBgra: return 3;
		case VideoOutSampleView::Unsupported: return -1;
	}
	return -1;
}

[[nodiscard]] constexpr VkImageUsageFlags GetVideoOutBufferImageUsage(bool sampled_alias)
{
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (sampled_alias)
	{
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}
	return usage;
}

[[nodiscard]] inline VkImageCreateInfo BuildVideoOutBufferImageCreateInfo(
	VkFormat format, uint32_t width, uint32_t height, bool sampled_alias)
{
	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.flags         = sampled_alias ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent        = {width, height, 1};
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.format        = format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	image_info.usage         = GetVideoOutBufferImageUsage(sampled_alias);
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
	return image_info;
}

enum class VideoOutSampleAliasRejection : uint8_t
{
	None,
	NotGraphicsQueue,
	StorageOrWritable,
	RegistrationTiling,
	NeoMismatch,
	DescriptorShape,
	RangeMismatch,
	ExtentMismatch,
	FormatOrSwizzle,
	ImageContract,
	ViewUnavailable,
	LifetimeUnavailable,
	SelfFeedback,
};

struct VideoOutSampleAliasRequest
{
	bool              graphics_queue       = false;
	bool              two_dimensional      = false;
	bool              storage_descriptor   = false;
	bool              read_only_descriptor = false;
	bool              read_only_memory     = false;
	bool              registered_tiled     = false;
	bool              registered_neo       = false;
	bool              requested_neo        = false;
	bool              pow2_pad              = false;
	bool              lifetime_valid        = false;
	bool              self_feedback         = false;
	uint64_t          registered_size       = 0;
	uint64_t          registered_pitch      = 0;
	uint64_t          requested_size        = 0;
	uint64_t          requested_pitch       = 0;
	uint32_t          image_width           = 0;
	uint32_t          image_height          = 0;
	uint32_t          requested_width       = 0;
	uint32_t          requested_height      = 0;
	uint32_t          data_format           = 0;
	uint32_t          number_format         = 0;
	uint32_t          tile_mode             = 0;
	uint32_t          component_swizzle     = 0;
	uint32_t          base_level            = 0;
	uint32_t          levels                = 0;
	VkFormat          image_format           = VK_FORMAT_UNDEFINED;
	VkImageUsageFlags image_usage            = 0;
	VkImageCreateFlags image_flags            = 0;
	uint32_t          image_mip_levels       = 0;
	uint32_t          available_view_mask    = 0;
};

struct VideoOutSampleAliasDecision
{
	VideoOutSampleAliasRejection rejection = VideoOutSampleAliasRejection::FormatOrSwizzle;
	VideoOutSampleViewInfo       sample_view;

	[[nodiscard]] constexpr bool IsSupported() const { return rejection == VideoOutSampleAliasRejection::None; }
};

[[nodiscard]] constexpr VideoOutSampleAliasDecision ResolveVideoOutSampleAlias(
	const VideoOutSampleAliasRequest& request)
{
	if (!request.graphics_queue)
	{
		return {VideoOutSampleAliasRejection::NotGraphicsQueue, {}};
	}
	if (request.storage_descriptor || !request.read_only_descriptor || !request.read_only_memory)
	{
		return {VideoOutSampleAliasRejection::StorageOrWritable, {}};
	}
	if (!request.registered_tiled || request.tile_mode != 10)
	{
		return {VideoOutSampleAliasRejection::RegistrationTiling, {}};
	}
	if (request.registered_neo != request.requested_neo)
	{
		return {VideoOutSampleAliasRejection::NeoMismatch, {}};
	}
	if (!request.two_dimensional || request.base_level != 0 || request.levels != 1 || request.pow2_pad)
	{
		return {VideoOutSampleAliasRejection::DescriptorShape, {}};
	}
	if (request.registered_size == 0 || request.registered_size != request.requested_size ||
	    request.registered_pitch == 0 || request.registered_pitch != request.requested_pitch)
	{
		return {VideoOutSampleAliasRejection::RangeMismatch, {}};
	}
	if (!request.lifetime_valid)
	{
		return {VideoOutSampleAliasRejection::LifetimeUnavailable, {}};
	}
	if (!AreRenderTextureExtentsCompatible(request.image_width, request.image_height,
	                                       request.requested_width, request.requested_height))
	{
		return {VideoOutSampleAliasRejection::ExtentMismatch, {}};
	}
	const auto sampled_format = DecodeSampledRenderTextureFormat(request.data_format, request.number_format);
	const auto sample_view = sampled_format.IsSupported()
	                             ? ResolveVideoOutSampleView(request.image_format, sampled_format.format,
	                                                         request.component_swizzle)
	                             : VideoOutSampleViewInfo {};
	if (request.data_format != 10 || !sample_view.IsSupported())
	{
		return {VideoOutSampleAliasRejection::FormatOrSwizzle, {}};
	}
	if ((request.image_usage & GetVideoOutBufferImageUsage(true)) != GetVideoOutBufferImageUsage(true) ||
	    (request.image_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0 || request.image_mip_levels != 1)
	{
		return {VideoOutSampleAliasRejection::ImageContract, sample_view};
	}
	const int view_index = GetVideoOutSampleViewIndex(sample_view.view);
	if (view_index < 0 || (request.available_view_mask & (1u << static_cast<uint32_t>(view_index))) == 0)
	{
		return {VideoOutSampleAliasRejection::ViewUnavailable, sample_view};
	}
	if (request.self_feedback)
	{
		return {VideoOutSampleAliasRejection::SelfFeedback, sample_view};
	}
	return {VideoOutSampleAliasRejection::None, sample_view};
}

[[nodiscard]] constexpr const char* VideoOutSampleAliasRejectionName(VideoOutSampleAliasRejection rejection)
{
	switch (rejection)
	{
		case VideoOutSampleAliasRejection::None: return "none";
		case VideoOutSampleAliasRejection::NotGraphicsQueue: return "not-graphics-queue";
		case VideoOutSampleAliasRejection::StorageOrWritable: return "storage-or-writable";
		case VideoOutSampleAliasRejection::RegistrationTiling: return "tiling";
		case VideoOutSampleAliasRejection::NeoMismatch: return "neo";
		case VideoOutSampleAliasRejection::DescriptorShape: return "descriptor-shape";
		case VideoOutSampleAliasRejection::RangeMismatch: return "range";
		case VideoOutSampleAliasRejection::ExtentMismatch: return "extent";
		case VideoOutSampleAliasRejection::FormatOrSwizzle: return "format-or-swizzle";
		case VideoOutSampleAliasRejection::ImageContract: return "image-contract";
		case VideoOutSampleAliasRejection::ViewUnavailable: return "view-unavailable";
		case VideoOutSampleAliasRejection::LifetimeUnavailable: return "lifetime-unavailable";
		case VideoOutSampleAliasRejection::SelfFeedback: return "self-feedback";
	}
	return "unknown";
}

} // namespace Lumen::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_VIDEOOUTBUFFERVULKANCONTRACT_H_ */
