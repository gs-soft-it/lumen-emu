#include "Emulator/Graphics/Objects/Texture.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/TextureBc7Contract.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/TextureCommonRefreshCopy.h"
#include "Emulator/Graphics/TextureMipSnapshotCopy.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#include <algorithm>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

static VkFormat get_texture_format(uint32_t dfmt, uint32_t nfmt)
{
	if (nfmt == 9 && dfmt == 10)
	{
		return VK_FORMAT_R8G8B8A8_SRGB;
	}
	if (nfmt == 0 && dfmt == 10)
	{
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
	if (nfmt == 0 && dfmt == 1)
	{
		return VK_FORMAT_R8_UNORM;
	}
	if (nfmt == 0 && dfmt == 3)
	{
		return VK_FORMAT_R8G8_UNORM;
	}
	if (nfmt == 9 && dfmt == 37)
	{
		return VK_FORMAT_BC3_SRGB_BLOCK;
	}
	if (nfmt == 0 && dfmt == 37)
	{
		return VK_FORMAT_BC3_UNORM_BLOCK;
	}
	if (nfmt == 0 && dfmt == 36)
	{
		return VK_FORMAT_BC2_UNORM_BLOCK;
	}
	if (nfmt == 0 && dfmt == 35)
	{
		return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	}
	if (nfmt == 0 && dfmt == 41) return VK_FORMAT_BC7_UNORM_BLOCK;
	EXIT("unknown format: nfmt = %u, dfmt = %u\n", nfmt, dfmt);
	return VK_FORMAT_UNDEFINED;
}

static bool ResolveBc7TextureParams(const uint64_t* params, Bc7Thin1dLayout* layout)
{
	return params != nullptr && params[TextureObject::PARAM_FORMAT] == (uint64_t {41} << 8u) &&
	       params[TextureObject::PARAM_POW2_PAD] == 0 && params[TextureObject::PARAM_NEO] <= 1 &&
	       ResolveBc7Thin1dLayout(41, 0, params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u,
	           params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu, params[TextureObject::PARAM_PITCH],
	           params[TextureObject::PARAM_LEVELS] >> 32u, params[TextureObject::PARAM_LEVELS] & 0xffffffffu,
	           params[TextureObject::PARAM_TILE], false, layout);
}

static VkComponentSwizzle get_swizzle(uint8_t s)
{
	switch (s)
	{
		case 0: return VK_COMPONENT_SWIZZLE_ZERO; break;
		case 1: return VK_COMPONENT_SWIZZLE_ONE; break;
		case 4: return VK_COMPONENT_SWIZZLE_R; break;
		case 5: return VK_COMPONENT_SWIZZLE_G; break;
		case 6: return VK_COMPONENT_SWIZZLE_B; break;
		case 7: return VK_COMPONENT_SWIZZLE_A; break;
		case 2:
		case 3:
		default: EXIT("unknown swizzle: %d\n", static_cast<int>(s));
	}
	return VK_COMPONENT_SWIZZLE_IDENTITY;
}

static VkImageUsageFlags get_usage()
{
	VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if (GpuMemoryTextureMipSnapshotEnabled())
	{
		vk_usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}

	return vk_usage;
}

static bool texture_common_refresh_images_valid(bool recording_transfer, const uint64_t* params,
                                                const GpuMemoryObject& source, const GpuMemoryObject& destination)
{
	if (!recording_transfer || params == nullptr || source.type != GpuMemoryObjectType::StorageTexture ||
	    destination.type != GpuMemoryObjectType::Texture || source.obj == nullptr || destination.obj == nullptr ||
	    source.obj == destination.obj || params[TextureObject::PARAM_LEVELS] != 1)
	{
		return false;
	}
	VkFormat format = VK_FORMAT_UNDEFINED;
	if (params[TextureObject::PARAM_FORMAT] == (1u << 8u)) format = VK_FORMAT_R8_UNORM;
	if (params[TextureObject::PARAM_FORMAT] == (10u << 8u)) format = VK_FORMAT_R8G8B8A8_UNORM;
	if (params[TextureObject::PARAM_FORMAT] == ((10u << 8u) | 9u)) format = VK_FORMAT_R8G8B8A8_SRGB;
	const uint32_t width = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	const uint32_t height = params[TextureObject::PARAM_WIDTH_HEIGHT];
	if (format == VK_FORMAT_UNDEFINED || width == 0 || height == 0 || width > INT32_MAX || height > INT32_MAX ||
	    params[TextureObject::PARAM_PITCH] < width || params[TextureObject::PARAM_PITCH] > UINT32_MAX)
	{
		return false;
	}
	const auto* storage = static_cast<const StorageTextureVulkanImage*>(source.obj);
	const auto* sampled = static_cast<const TextureVulkanImage*>(destination.obj);
	if (storage->type != VulkanImageType::StorageTexture || sampled->type != VulkanImageType::Texture ||
	    storage->image == sampled->image || storage->memory.memory == sampled->memory.memory ||
	    storage->memory.unique_id == sampled->memory.unique_id ||
	    storage->layout != VK_IMAGE_LAYOUT_GENERAL || sampled->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
	    storage->descriptor_base_level != 0 || storage->descriptor_levels != 1 ||
	    storage->physical_extent.width != width || storage->physical_extent.height != height ||
	    (storage->usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) !=
	        (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
	    (sampled->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) !=
	        (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
	{
		return false;
	}
	for (const VulkanImage* image: {static_cast<const VulkanImage*>(storage), static_cast<const VulkanImage*>(sampled)})
	{
		if (image->image == nullptr || image->image_view[VulkanImage::VIEW_DEFAULT] == nullptr ||
		    image->memory.memory == nullptr || image->memory.unique_id == 0 || image->mip_levels != 1 ||
		    image->format != format || image->extent.width != width || image->extent.height != height)
		{
			return false;
		}
	}
	return true;
}

bool TextureCommonRefreshImagesValid(const uint64_t* params,
                                    const GpuMemoryObject& source, const GpuMemoryObject& destination)
{
	return GpuMemoryTextureMipSnapshotEnabled() && texture_common_refresh_images_valid(true, params, source, destination);
}

bool TextureCommonRefreshPreflight(CommandBuffer* buffer, const uint64_t* params,
                                    const GpuMemoryObject& source, const GpuMemoryObject& destination)
{
	return GpuMemoryTextureMipSnapshotEnabled() && texture_common_refresh_images_valid(
	    buffer != nullptr && buffer->CanRecordTextureTransfer(), params, source, destination);
}

void TextureCommonRefresh(CommandBuffer* buffer, const uint64_t* params,
                           const GpuMemoryObject& source, const GpuMemoryObject& destination)
{
	EXIT_NOT_IMPLEMENTED(!TextureCommonRefreshPreflight(buffer, params, source, destination));
	const auto* storage = static_cast<const StorageTextureVulkanImage*>(source.obj);
	const auto* sampled = static_cast<const TextureVulkanImage*>(destination.obj);
	EXIT_NOT_IMPLEMENTED(!RecordStorageSampledRefresh(buffer->GetBufferOutsideRenderPass(), storage->image,
	                                                 storage->layout, sampled->image, sampled->layout,
	                                                 sampled->extent.width, sampled->extent.height));
}

static bool render_target_snapshot_image_valid(bool recording, const uint64_t* params, const GpuMemoryObject& source)
{
	if (!recording || params == nullptr || source.type != GpuMemoryObjectType::RenderTexture || source.obj == nullptr ||
	    params[TextureObject::PARAM_FORMAT] != (10u << 8u) || params[TextureObject::PARAM_LEVELS] != 1 ||
	    params[TextureObject::PARAM_TILE] != 14 || params[TextureObject::PARAM_NEO] != 0 ||
	    params[TextureObject::PARAM_POW2_PAD] != 0 || params[TextureObject::PARAM_SWIZZLE] != DstSel(4, 5, 6, 7)) return false;
	const uint32_t width = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	const uint32_t height = params[TextureObject::PARAM_WIDTH_HEIGHT];
	const auto* image = static_cast<const RenderTextureVulkanImage*>(source.obj);
	return width != 0 && height != 0 && width <= INT32_MAX && height <= INT32_MAX &&
	       params[TextureObject::PARAM_PITCH] >= width && params[TextureObject::PARAM_PITCH] <= UINT32_MAX &&
	       image->type == VulkanImageType::RenderTexture && image->image != nullptr && image->image_view[0] != nullptr &&
	       image->memory.memory != nullptr && image->memory.unique_id != 0 && image->mip_levels == 1 &&
	       image->format == VK_FORMAT_R8G8B8A8_UNORM && image->extent.width == width && image->extent.height == height &&
	       (image->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ==
	           (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
	       (image->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || image->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static bool render_target_refresh_images_valid(bool recording, const uint64_t* params,
                                               const GpuMemoryObject& source, const GpuMemoryObject& destination)
{
	if (!render_target_snapshot_image_valid(recording, params, source) || destination.type != GpuMemoryObjectType::Texture ||
	    destination.obj == nullptr || destination.obj == source.obj) return false;
	const auto* render = static_cast<const RenderTextureVulkanImage*>(source.obj);
	const auto* sampled = static_cast<const TextureVulkanImage*>(destination.obj);
	return sampled->type == VulkanImageType::Texture && sampled->image != nullptr && sampled->image_view[0] != nullptr &&
	       sampled->memory.memory != nullptr && sampled->memory.unique_id != 0 && sampled->mip_levels == 1 &&
	       sampled->image != render->image && sampled->memory.memory != render->memory.memory &&
	       sampled->memory.unique_id != render->memory.unique_id && sampled->format == render->format &&
	       sampled->extent.width == render->extent.width && sampled->extent.height == render->extent.height &&
	       sampled->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	       (sampled->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) ==
	           (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

bool TextureRenderTargetSnapshotPreflight(CommandBuffer* buffer, const uint64_t* params, const GpuMemoryObject& source)
{
	return GpuMemoryRenderTextureSnapshotEnabled() && render_target_snapshot_image_valid(
	    buffer != nullptr && buffer->CanRecordTextureTransfer(), params, source);
}

bool TextureRenderTargetRefreshPreflight(CommandBuffer* buffer, const uint64_t* params,
                                        const GpuMemoryObject& source, const GpuMemoryObject& destination)
{
	return GpuMemoryRenderTextureSnapshotEnabled() && render_target_refresh_images_valid(
	    buffer != nullptr && buffer->CanRecordTextureTransfer(), params, source, destination);
}

void TextureRenderTargetRefresh(CommandBuffer* buffer, const uint64_t* params,
                               const GpuMemoryObject& source, const GpuMemoryObject& destination)
{
	EXIT_NOT_IMPLEMENTED(!TextureRenderTargetRefreshPreflight(buffer, params, source, destination));
	const auto* render = static_cast<const RenderTextureVulkanImage*>(source.obj);
	const auto* sampled = static_cast<const TextureVulkanImage*>(destination.obj);
	EXIT_NOT_IMPLEMENTED(!RecordRenderTargetSampledRefresh(buffer->GetBufferOutsideRenderPass(), render->image,
	    render->layout, sampled->image, sampled->layout, sampled->extent.width, sampled->extent.height));
}

static bool texture_mip_snapshot_sources_valid(const uint64_t* params, const Vector<GpuMemoryObject>& sources)
{
	if (params == nullptr || params[TextureObject::PARAM_FORMAT] != (10u << 8u) ||
	    params[TextureObject::PARAM_TILE] != 13 || params[TextureObject::PARAM_NEO] != 0 ||
	    params[TextureObject::PARAM_POW2_PAD] != 1 || params[TextureObject::PARAM_SWIZZLE] != DstSel(4, 5, 6, 7))
	{
		return false;
	}
	const uint32_t width = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	const uint32_t height = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	const uint64_t levels = params[TextureObject::PARAM_LEVELS];
	if (width == 0 || height == 0 || params[TextureObject::PARAM_PITCH] < width ||
	    params[TextureObject::PARAM_PITCH] > UINT32_MAX || levels < 2 || levels >= 16 || levels != sources.Size())
	{
		return false;
	}
	uint32_t max_levels = 1;
	for (uint32_t dimension = std::max(width, height); dimension > 1; dimension >>= 1u)
	{
		++max_levels;
	}
	if (levels > max_levels)
	{
		return false;
	}
	for (uint32_t level = 0; level < levels; ++level)
	{
		const auto& source = sources.At(level);
		const auto expected_type = level == 0 ? GpuMemoryObjectType::Texture : GpuMemoryObjectType::StorageTexture;
		if (source.type != expected_type || source.obj == nullptr)
		{
			return false;
		}
		const auto* image = level == 0 ? static_cast<const VulkanImage*>(static_cast<const TextureVulkanImage*>(source.obj))
		                               : static_cast<const VulkanImage*>(static_cast<const StorageTextureVulkanImage*>(source.obj));
		const uint32_t mip_width = std::max(width >> level, 1u);
		const uint32_t mip_height = std::max(height >> level, 1u);
		const VkImageLayout expected_layout = level == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
		if (image->image == nullptr || image->image_view[VulkanImage::VIEW_DEFAULT] == nullptr ||
		    image->format != VK_FORMAT_R8G8B8A8_UNORM || image->extent.width != mip_width ||
		    image->extent.height != mip_height || image->layout != expected_layout || image->mip_levels != 1 ||
		    (image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 || image->memory.memory == nullptr)
		{
			return false;
		}
		if (level == 0)
		{
			if (image->type != VulkanImageType::Texture || (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0)
			{
				return false;
			}
		} else
		{
			const auto* storage = static_cast<const StorageTextureVulkanImage*>(source.obj);
			if (image->type != VulkanImageType::StorageTexture ||
			    (image->usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0 || storage->descriptor_base_level != level ||
			    storage->descriptor_levels != level + 1u || storage->physical_extent.width != mip_width ||
			    storage->physical_extent.height != mip_height)
			{
				return false;
			}
		}
		for (uint32_t previous = 0; previous < level; ++previous)
		{
			const auto* previous_image = previous == 0
			                                 ? static_cast<const VulkanImage*>(static_cast<const TextureVulkanImage*>(sources.At(previous).obj))
			                                 : static_cast<const VulkanImage*>(static_cast<const StorageTextureVulkanImage*>(sources.At(previous).obj));
			if (source.obj == sources.At(previous).obj || image->image == previous_image->image)
			{
				return false;
			}
		}
	}
	return true;
}

bool TextureMipSnapshotPreflight(CommandBuffer* buffer, const uint64_t* params,
                                 const Vector<GpuMemoryObject>& sources)
{
	return GpuMemoryTextureMipSnapshotEnabled() && buffer != nullptr && buffer->CanRecordTextureTransfer() &&
	       texture_mip_snapshot_sources_valid(params, sources);
}

static void copy_texture_mip_snapshot(CommandBuffer* buffer, const uint64_t* params,
                                      const Vector<GpuMemoryObject>& sources, TextureVulkanImage* destination)
{
	EXIT_NOT_IMPLEMENTED(!TextureMipSnapshotPreflight(buffer, params, sources));
	EXIT_IF(destination == nullptr || destination->image == nullptr || destination->layout != VK_IMAGE_LAYOUT_UNDEFINED ||
	        destination->format != VK_FORMAT_R8G8B8A8_UNORM || destination->mip_levels != sources.Size() ||
	        (destination->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0);
	TextureMipSnapshotCopySource copies[15] {};
	for (uint32_t level = 0; level < sources.Size(); ++level)
	{
		const auto* source = level == 0
		                         ? static_cast<const VulkanImage*>(static_cast<const TextureVulkanImage*>(sources.At(level).obj))
		                         : static_cast<const VulkanImage*>(static_cast<const StorageTextureVulkanImage*>(sources.At(level).obj));
		copies[level] = {source->image, source->extent, source->layout};
	}
	// The tracker admits accesses submitted before this recording on the same
	// graphics queue. The emitter includes both readers and producers in its
	// dependencies, and restores every source's original whole-image layout.
	EXIT_NOT_IMPLEMENTED(!RecordTextureMipSnapshotCopy(buffer->GetBufferOutsideRenderPass(),
	                                                   destination->image, copies, sources.Size()));
	destination->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static bool CheckFormat(GraphicContext* ctx, VkImageCreateInfo* image_info)
{
	VkImageFormatProperties props {};
	if (vkGetPhysicalDeviceImageFormatProperties(ctx->physical_device, image_info->format, image_info->imageType, image_info->tiling,
	                                             image_info->usage, image_info->flags, &props) == VK_ERROR_FORMAT_NOT_SUPPORTED)
	{
		if (image_info->format == VK_FORMAT_R8G8B8A8_SRGB)
		{
			// TODO() convert SRGB -> LINEAR in shader
			image_info->format = VK_FORMAT_R8G8B8A8_UNORM;
			bool result        = CheckFormat(ctx, image_info);
			printf("replace VK_FORMAT_R8G8B8A8_SRGB => VK_FORMAT_R8G8B8A8_UNORM [%s]\n", (!result ? "FAIL" : "SUCCESS"));
			return result;
		}
		if (image_info->format == VK_FORMAT_B8G8R8A8_SRGB)
		{
			// TODO() convert SRGB -> LINEAR in shader
			image_info->format = VK_FORMAT_B8G8R8A8_UNORM;
			bool result        = CheckFormat(ctx, image_info);
			printf("replace VK_FORMAT_B8G8R8A8_SRGB => VK_FORMAT_B8G8R8A8_UNORM [%s]\n", (!result ? "FAIL" : "SUCCESS"));
			return result;
		}
		return false;
	}
	return true;
}

static bool CheckSwizzle(GraphicContext* /*ctx*/, VkImageCreateInfo* image_info, VkComponentMapping* components)
{
	if ((image_info->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
	{
		if (components->r == VK_COMPONENT_SWIZZLE_R && components->g == VK_COMPONENT_SWIZZLE_G && components->b == VK_COMPONENT_SWIZZLE_B &&
		    components->a == VK_COMPONENT_SWIZZLE_A)
		{
			return true;
		}

		if (components->r == VK_COMPONENT_SWIZZLE_B && components->g == VK_COMPONENT_SWIZZLE_G && components->b == VK_COMPONENT_SWIZZLE_R &&
		    components->a == VK_COMPONENT_SWIZZLE_A && image_info->format == VK_FORMAT_R8G8B8A8_SRGB)
		{
			printf("replace VK_FORMAT_R8G8B8A8_SRGB => VK_FORMAT_B8G8R8A8_SRGB\n");

			components->r      = VK_COMPONENT_SWIZZLE_R;
			components->g      = VK_COMPONENT_SWIZZLE_G;
			components->b      = VK_COMPONENT_SWIZZLE_B;
			components->a      = VK_COMPONENT_SWIZZLE_A;
			image_info->format = VK_FORMAT_B8G8R8A8_SRGB;
			return true;
		}

		// TODO() swizzle channels in shader

		return false;
	}
	return true;
}

static void update_impl(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj, const uint64_t* vaddr,
                        const uint64_t* size, int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("TextureObject::update_impl");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = static_cast<TextureVulkanImage*>(obj);

	auto tile   = params[TextureObject::PARAM_TILE];
	auto dfmt   = (params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt   = (params[TextureObject::PARAM_FORMAT]) & 0xffu;
	auto width  = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto levels = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto pitch  = params[TextureObject::PARAM_PITCH];
	bool pow2_pad = params[TextureObject::PARAM_POW2_PAD] != 0;
	bool neo    = Config::IsNeo();

	if (dfmt == 41)
	{
		Bc7Thin1dLayout layout {};
		EXIT_NOT_IMPLEMENTED(!ResolveBc7TextureParams(params, &layout) || *size != layout.bytes ||
		                     vk_obj->format != VK_FORMAT_BC7_UNORM_BLOCK || vk_obj->mip_levels != 1 ||
		                     vk_obj->extent.width != width || vk_obj->extent.height != height);
	}

	VkImageLayout vk_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	EXIT_NOT_IMPLEMENTED(levels >= 16);

	TileSizeOffset level_sizes[16];

	EXIT_NOT_IMPLEMENTED(tile != 8 && tile != 13 && tile != 10);
	if (tile == 10)
	{
		TileSizeAlign layout {};
		EXIT_NOT_IMPLEMENTED(!TileTryGetDisplay2dThinTextureSize(dfmt, nfmt, width, height, pitch, levels, tile,
		                                                       neo, pow2_pad, &layout, nullptr, nullptr) || *size < layout.size);
	}

	TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, nullptr, level_sizes, nullptr, pow2_pad);

	// dbg_test_mipmaps(ctx, VK_FORMAT_BC3_SRGB_BLOCK, 512, 512);

	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	uint32_t mip_pitch  = pitch;

	Vector<BufferImageCopy> regions(levels);
	for (uint32_t i = 0; i < levels; i++)
	{
		EXIT_NOT_IMPLEMENTED(level_sizes[i].size == 0);

		regions[i].offset    = level_sizes[i].offset;
		regions[i].width     = mip_width;
		regions[i].height    = mip_height;
		regions[i].pitch     = mip_pitch;
		regions[i].dst_level = i;
		regions[i].dst_x     = 0;
		regions[i].dst_y     = 0;

		if (mip_width > 1)
		{
			mip_width /= 2;
		}
		if (mip_height > 1)
		{
			mip_height /= 2;
		}
		if (mip_pitch > 1)
		{
			mip_pitch /= 2;
		}
	}

	auto upload = [&](const void* data)
	{
		if (buffer == nullptr)
		{
			UtilFillImage(ctx, vk_obj, data, *size, regions, static_cast<uint64_t>(vk_layout));
		} else
		{
			UtilUploadImage(ctx, buffer, vk_obj, data, *size, regions, static_cast<uint64_t>(vk_layout));
		}
	};

	if (tile == 13 || tile == 10)
	{
		// EXIT_NOT_IMPLEMENTED(pitch != width);
		auto* temp_buf = new uint8_t[*size] {};
		TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), TileMode::TextureTiled, dfmt, nfmt, width, height, pitch, levels,
		                         neo, tile, *size, pow2_pad);
		upload(temp_buf);
		delete[] temp_buf;
	} else if (tile == 8)
	{
		upload(reinterpret_cast<void*>(*vaddr));
	}
}

static void update_func(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	update_impl(ctx, nullptr, params, obj, vaddr, size, vaddr_num);
}

static void update_with_buffer_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj,
                                    const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	EXIT_IF(buffer == nullptr);
	update_impl(ctx, buffer, params, obj, vaddr, size, vaddr_num);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void update2_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj, GpuMemoryScenario scenario,
                         const Vector<GpuMemoryObject>& objects)
{
	LUMEN_PROFILER_BLOCK("TextureObject::update2_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(objects.IsEmpty());

	auto* vk_obj = static_cast<TextureVulkanImage*>(obj);

	auto width  = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto levels = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;

	VkImageLayout vk_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	EXIT_NOT_IMPLEMENTED(levels >= 16);

	uint32_t mip_width  = width;
	uint32_t mip_height = height;

	Vector<ImageImageCopy> regions(levels);

	if (objects.Size() == 1 && objects.At(0).type == GpuMemoryObjectType::StorageTexture && scenario == GpuMemoryScenario::Common)
	{
		auto* src_obj = static_cast<StorageTextureVulkanImage*>(objects.At(0).obj);
		// A single-window storage image places the selected guest mip at host
		// mip 0. Only the explicit snapshot route knows its destination level;
		// the ordinary atlas conversion is restricted to base-zero storage.
		EXIT_NOT_IMPLEMENTED(src_obj == nullptr || src_obj->descriptor_base_level != 0);

		for (uint32_t i = 0; i < levels; i++)
		{
			auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

			regions[i].src_image = src_obj;
			regions[i].src_level = 0;
			regions[i].dst_level = i;
			regions[i].width     = mip_width;
			regions[i].height    = mip_height;
			regions[i].src_x     = mipmap_offset.first;
			regions[i].src_y     = mipmap_offset.second;
			regions[i].dst_x     = 0;
			regions[i].dst_y     = 0;

			if (mip_width > 1)
			{
				mip_width /= 2;
			}
			if (mip_height > 1)
			{
				mip_height /= 2;
			}
		}
	} else if (levels == objects.Size() && scenario == GpuMemoryScenario::Common)
	{
		for (uint32_t i = 0; i < levels; i++)
		{
			const auto& object = objects.At(i);

			EXIT_NOT_IMPLEMENTED(object.type != GpuMemoryObjectType::RenderTexture);

			auto* src_obj = static_cast<RenderTextureVulkanImage*>(object.obj);

			regions[i].src_image = src_obj;
			regions[i].src_level = 0;
			regions[i].dst_level = i;
			regions[i].width     = mip_width;
			regions[i].height    = mip_height;
			regions[i].src_x     = 0;
			regions[i].src_y     = 0;
			regions[i].dst_x     = 0;
			regions[i].dst_y     = 0;

			if (mip_width > 1)
			{
				mip_width /= 2;
			}
			if (mip_height > 1)
			{
				mip_height /= 2;
			}
		}
		//	} else if (objects.Size() >= 2 && objects.At(0).type == GpuMemoryObjectType::StorageBuffer &&
		//	           objects.At(1).type == GpuMemoryObjectType::StorageTexture && scenario == GpuMemoryScenario::GenerateMips)
	} else if (objects.Size() >= 3 && objects.At(0).type == GpuMemoryObjectType::StorageBuffer &&
	           objects.At(1).type == GpuMemoryObjectType::Texture && objects.At(2).type == GpuMemoryObjectType::StorageTexture &&
	           scenario == GpuMemoryScenario::GenerateMips)
	{
		for (uint32_t i = 0; i < levels; i++)
		{
			VulkanImage* src_image = nullptr;
			bool         storage   = false;

			for (const auto& o: objects)
			{
				if (o.type == GpuMemoryObjectType::StorageTexture)
				{
					auto* src_obj = static_cast<StorageTextureVulkanImage*>(o.obj);
					if (mip_width == src_obj->extent.width && mip_height == src_obj->extent.height)
					{
						src_image = src_obj;
						storage   = true;
						break;
					}
				} else if (o.type == GpuMemoryObjectType::RenderTexture)
				{
					auto* src_obj = static_cast<RenderTextureVulkanImage*>(o.obj);
					if (mip_width == src_obj->extent.width && mip_height == src_obj->extent.height)
					{
						src_image = src_obj;
						storage   = false;
						break;
					}
				}
			}

			EXIT_NOT_IMPLEMENTED(src_image == nullptr);

			if (storage)
			{
				auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

				regions[i].src_image = src_image;
				regions[i].src_level = 0;
				regions[i].dst_level = i;
				regions[i].width     = mip_width;
				regions[i].height    = mip_height;
				regions[i].src_x     = mipmap_offset.first;
				regions[i].src_y     = mipmap_offset.second;
				regions[i].dst_x     = 0;
				regions[i].dst_y     = 0;
			} else
			{
				regions[i].src_image = src_image;
				regions[i].src_level = 0;
				regions[i].dst_level = i;
				regions[i].width     = mip_width;
				regions[i].height    = mip_height;
				regions[i].src_x     = 0;
				regions[i].src_y     = 0;
				regions[i].dst_x     = 0;
				regions[i].dst_y     = 0;
			}

			if (mip_width > 1)
			{
				mip_width /= 2;
			}
			if (mip_height > 1)
			{
				mip_height /= 2;
			}
		}
	} else
	{
		LUMEN_NOT_IMPLEMENTED;
	}

	if (buffer == nullptr)
	{
		UtilFillImage(ctx, regions, vk_obj, static_cast<uint64_t>(vk_layout));
	} else
	{
		UtilImageToImage(buffer, regions, vk_obj, static_cast<uint64_t>(vk_layout));
	}
}

static void* create_impl(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size,
                         int vaddr_num, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("TextureObject::Create");

	EXIT_IF(size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);

	auto dfmt       = (params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt       = (params[TextureObject::PARAM_FORMAT]) & 0xffu;
	auto width      = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height     = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto base_level = params[TextureObject::PARAM_LEVELS] >> 32u;
	auto levels     = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto swizzle    = params[TextureObject::PARAM_SWIZZLE];

	if (dfmt == 41)
	{
		Bc7Thin1dLayout layout {};
		EXIT_NOT_IMPLEMENTED(!ResolveBc7TextureParams(params, &layout) || vaddr_num != 1 || *size != layout.bytes);
	}

	VkImageUsageFlags vk_usage = get_usage();

	VkComponentMapping components {};

	components.r = get_swizzle(GetDstSel(swizzle, 0));
	components.g = get_swizzle(GetDstSel(swizzle, 1));
	components.b = get_swizzle(GetDstSel(swizzle, 2));
	components.a = get_swizzle(GetDstSel(swizzle, 3));

	auto pixel_format = get_texture_format(dfmt, nfmt);

	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);

	auto* vk_obj = new TextureVulkanImage;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent.width  = width;
	image_info.extent.height = height;
	image_info.extent.depth  = 1;
	image_info.mipLevels     = levels;
	image_info.arrayLayers   = 1;
	image_info.format        = pixel_format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	image_info.usage         = vk_usage;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

	if (!CheckSwizzle(ctx, &image_info, &components))
	{
		EXIT("swizzle is not supported");
	}

	if (!CheckFormat(ctx, &image_info))
	{
		EXIT("format is not supported");
	}

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = image_info.format;
	vk_obj->usage         = image_info.usage;
	vk_obj->mip_levels    = image_info.mipLevels;
	vk_obj->image         = nullptr;
	vk_obj->layout        = image_info.initialLayout;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem->requirements);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, mem);

	vk_obj->memory = *mem;

	update_impl(ctx, buffer, params, vk_obj, vaddr, size, vaddr_num);

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components                      = components;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = base_level;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;

	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);

	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);

	return vk_obj;
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	return create_impl(ctx, nullptr, params, vaddr, size, vaddr_num, mem);
}

static void* create_with_buffer_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, const uint64_t* vaddr,
                                     const uint64_t* size, int vaddr_num, VulkanMemory* mem)
{
	EXIT_IF(buffer == nullptr);
	return create_impl(ctx, buffer, params, vaddr, size, vaddr_num, mem);
}

static void* create2_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, GpuMemoryScenario scenario,
                          const Vector<GpuMemoryObject>& objects, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("TextureObject::CreateFromObjects");

	EXIT_IF(objects.IsEmpty());
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	// BC7 currently imports only guest-origin opaque blocks, with no GPU alias conversion.
	EXIT_NOT_IMPLEMENTED(((params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu) == 41);
	const bool mip_snapshot = scenario == GpuMemoryScenario::TextureMipSnapshot;
	EXIT_NOT_IMPLEMENTED(mip_snapshot && !TextureMipSnapshotPreflight(buffer, params, objects));
	if (!mip_snapshot)
	{
		for (const auto& object: objects)
		{
			if (object.type == GpuMemoryObjectType::StorageTexture)
			{
				const auto* source = static_cast<const StorageTextureVulkanImage*>(object.obj);
				EXIT_NOT_IMPLEMENTED(source == nullptr || source->descriptor_base_level != 0);
			}
		}
	}

	auto dfmt       = (params[TextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt       = (params[TextureObject::PARAM_FORMAT]) & 0xffu;
	auto width      = params[TextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height     = params[TextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto base_level = params[TextureObject::PARAM_LEVELS] >> 32u;
	auto levels     = params[TextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto swizzle    = params[TextureObject::PARAM_SWIZZLE];

	VkImageUsageFlags vk_usage = get_usage();

	VkComponentMapping components {};

	components.r = get_swizzle(GetDstSel(swizzle, 0));
	components.g = get_swizzle(GetDstSel(swizzle, 1));
	components.b = get_swizzle(GetDstSel(swizzle, 2));
	components.a = get_swizzle(GetDstSel(swizzle, 3));

	auto pixel_format = get_texture_format(dfmt, nfmt);

	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);

	auto* vk_obj = new TextureVulkanImage;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent.width  = width;
	image_info.extent.height = height;
	image_info.extent.depth  = 1;
	image_info.mipLevels     = levels;
	image_info.arrayLayers   = 1;
	image_info.format        = pixel_format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	image_info.usage         = vk_usage;
	image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples       = VK_SAMPLE_COUNT_1_BIT;

	if (!CheckSwizzle(ctx, &image_info, &components))
	{
		EXIT("swizzle is not supported");
	}

	if (!CheckFormat(ctx, &image_info))
	{
		EXIT("format is not supported");
	}
	if (mip_snapshot)
	{
		VkImageFormatProperties properties {};
		const auto result = vkGetPhysicalDeviceImageFormatProperties(ctx->physical_device, image_info.format,
		                                                            image_info.imageType, image_info.tiling,
		                                                            image_info.usage, image_info.flags, &properties);
		EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS || image_info.format != VK_FORMAT_R8G8B8A8_UNORM ||
		                     width > properties.maxExtent.width || height > properties.maxExtent.height ||
		                     levels > properties.maxMipLevels || properties.maxArrayLayers < 1 ||
		                     (properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0);
	}

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = image_info.format;
	vk_obj->usage         = image_info.usage;
	vk_obj->mip_levels    = image_info.mipLevels;
	vk_obj->image         = nullptr;
	vk_obj->layout        = image_info.initialLayout;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem->requirements);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, mem);

	vk_obj->memory = *mem;

	if (mip_snapshot)
	{
		copy_texture_mip_snapshot(buffer, params, objects, vk_obj);
	} else
	{
		update2_func(ctx, buffer, params, vk_obj, scenario, objects);
	}

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components                      = components;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = base_level;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;

	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);

	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("TextureObject::delete_func");

	auto* vk_obj = reinterpret_cast<TextureVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	DeleteDescriptor(vk_obj);

	vkDestroyImageView(ctx->device, vk_obj->image_view[VulkanImage::VIEW_DEFAULT], nullptr);

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

bool TextureObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_PITCH] == other[PARAM_PITCH] &&
	        params[PARAM_WIDTH_HEIGHT] == other[PARAM_WIDTH_HEIGHT] && params[PARAM_LEVELS] == other[PARAM_LEVELS] &&
	        params[PARAM_TILE] == other[PARAM_TILE] && params[PARAM_NEO] == other[PARAM_NEO] &&
	        params[PARAM_SWIZZLE] == other[PARAM_SWIZZLE] && params[PARAM_POW2_PAD] == other[PARAM_POW2_PAD]);
}

GpuObject::create_func_t TextureObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_with_buffer_func_t TextureObject::GetCreateWithBufferFunc() const
{
	return create_with_buffer_func;
}

GpuObject::create_from_objects_func_t TextureObject::GetCreateFromObjectsFunc() const
{
	return create2_func;
}

GpuObject::delete_func_t TextureObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t TextureObject::GetUpdateFunc() const
{
	return update_func;
}

GpuObject::update_with_buffer_func_t TextureObject::GetUpdateWithBufferFunc() const
{
	return update_with_buffer_func;
}

static bool TextureRenderTargetSnapshotPreflightContractTest()
{
	TextureObject request(10, 0, 13, 9, 16, 0, 1, 14, false, DstSel(4, 5, 6, 7), false);
	RenderTextureVulkanImage render;
	TextureVulkanImage sampled;
	const auto set_image = [](VulkanImage& image, uintptr_t id, VkImageUsageFlags usage, VkImageLayout layout) {
		image.image = reinterpret_cast<VkImage>(id);
		image.image_view[0] = reinterpret_cast<VkImageView>(id + 10);
		image.memory.memory = reinterpret_cast<VkDeviceMemory>(id + 20);
		image.memory.unique_id = id;
		image.extent = {13, 9};
		image.mip_levels = 1;
		image.format = VK_FORMAT_R8G8B8A8_UNORM;
		image.usage = usage;
		image.layout = layout;
	};
	set_image(render, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	set_image(sampled, 2, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	const GpuMemoryObject source {GpuMemoryObjectType::RenderTexture, &render};
	const GpuMemoryObject destination {GpuMemoryObjectType::Texture, &sampled};
	const auto valid = [&] { return render_target_refresh_images_valid(true, request.params, source, destination); };
	if (!valid() || render_target_snapshot_image_valid(false, request.params, source) ||
	    render_target_snapshot_image_valid(true, nullptr, source) ||
	    TextureRenderTargetSnapshotPreflight(nullptr, request.params, source)) return false;
	const auto saved_render = render;
	const auto saved_sampled = sampled;
	for (unsigned invalid = 0; invalid < 15; ++invalid)
	{
		render = saved_render; sampled = saved_sampled;
		switch (invalid)
		{
			case 0: render.format = VK_FORMAT_R8G8B8A8_SRGB; break;
			case 1: render.extent.height++; break;
			case 2: render.image_view[0] = nullptr; break;
			case 3: render.usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT; break;
			case 4: render.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; break;
			case 5: render.mip_levels = 2; break;
			case 6: render.layout = VK_IMAGE_LAYOUT_UNDEFINED; break;
			case 7: render.layout = VK_IMAGE_LAYOUT_GENERAL; break;
			case 8: sampled.image = render.image; break;
			case 9: sampled.memory.memory = render.memory.memory; break;
			case 10: sampled.memory.unique_id = render.memory.unique_id; break;
			case 11: sampled.usage &= ~VK_IMAGE_USAGE_TRANSFER_DST_BIT; break;
			case 12: sampled.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; break;
			case 13: sampled.extent.width++; break;
			case 14: sampled.mip_levels = 2; break;
		}
		if (valid()) return false;
	}
	render = saved_render; sampled = saved_sampled;
	render.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (!valid()) return false;
	request.params[TextureObject::PARAM_LEVELS] = (1ull << 32u) | 1u;
	if (valid()) return false;
	request.params[TextureObject::PARAM_LEVELS] = 1;
	const auto command = reinterpret_cast<VkCommandBuffer>(uintptr_t(1));
	return !RecordRenderTargetSampledRefresh(nullptr, render.image, render.layout, sampled.image, sampled.layout, 13, 9) &&
	       !RecordRenderTargetSampledRefresh(command, render.image, VK_IMAGE_LAYOUT_GENERAL, sampled.image, sampled.layout, 13, 9) &&
	       !RecordRenderTargetSampledRefresh(command, render.image, render.layout, render.image, sampled.layout, 13, 9);
}

static bool TextureCommonRefreshPreflightContractTest()
{
	TextureObject request(1, 0, 16, 8, 256, 0, 1, 13, false, DstSel(4, 4, 4, 4), false);
	TextureVulkanImage sampled;
	StorageTextureVulkanImage storage;
	const auto set_image = [](VulkanImage& image, uintptr_t id, VkImageUsageFlags usage, VkImageLayout layout)
	{
		image.image = reinterpret_cast<VkImage>(id);
		image.image_view[0] = reinterpret_cast<VkImageView>(id + 10);
		image.memory.memory = reinterpret_cast<VkDeviceMemory>(id + 20);
		image.memory.unique_id = id;
		image.extent = {16, 8};
		image.mip_levels = 1;
		image.format = VK_FORMAT_R8_UNORM;
		image.usage = usage;
		image.layout = layout;
	};
	set_image(sampled, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	set_image(storage, 2, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_LAYOUT_GENERAL);
	storage.descriptor_levels = 1;
	storage.physical_extent = storage.extent;
	const GpuMemoryObject source {GpuMemoryObjectType::StorageTexture, &storage};
	const GpuMemoryObject destination {GpuMemoryObjectType::Texture, &sampled};
	const auto valid = [&]() { return texture_common_refresh_images_valid(true, request.params, source, destination); };
	if (!valid() || texture_common_refresh_images_valid(false, request.params, source, destination) ||
	    texture_common_refresh_images_valid(true, nullptr, source, destination) ||
	    texture_common_refresh_images_valid(true, request.params, destination, source) ||
	    TextureCommonRefreshPreflight(nullptr, request.params, source, destination)) return false;
	const auto saved_sampled = sampled;
	const auto saved_storage = storage;
	for (unsigned invalid = 0; invalid < 10; ++invalid)
	{
		switch (invalid)
		{
			case 0: sampled.image = nullptr; break;
			case 1: sampled.image_view[0] = nullptr; break;
			case 2: sampled.memory.memory = nullptr; break;
			case 3: sampled.memory.unique_id = 0; break;
			case 4: sampled.image = storage.image; break;
			case 5: sampled.memory.memory = storage.memory.memory; break;
			case 6: sampled.usage &= ~VK_IMAGE_USAGE_TRANSFER_DST_BIT; break;
			case 7: sampled.layout = VK_IMAGE_LAYOUT_GENERAL; break;
			case 8: sampled.mip_levels = 2; break;
			case 9: sampled.extent.width++; break;
		}
		if (valid()) return false;
		sampled = saved_sampled;
	}
	for (unsigned invalid = 0; invalid < 8; ++invalid)
	{
		switch (invalid)
		{
			case 0: storage.image = nullptr; break;
			case 1: storage.descriptor_base_level = 1; break;
			case 2: storage.descriptor_levels = 2; break;
			case 3: storage.physical_extent.height++; break;
			case 4: storage.usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT; break;
			case 5: storage.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; break;
			case 6: storage.format = VK_FORMAT_R8G8B8A8_UNORM; break;
			case 7: storage.memory.unique_id = sampled.memory.unique_id; break;
		}
		if (valid()) return false;
		storage = saved_storage;
	}
	request.params[TextureObject::PARAM_LEVELS] = (1ull << 32u) | 2;
	if (valid()) return false;
	request.params[TextureObject::PARAM_LEVELS] = 1;
	request.params[TextureObject::PARAM_FORMAT] = 37u << 8u;
	if (valid()) return false;
	request.params[TextureObject::PARAM_FORMAT] = 10u << 8u;
	sampled.format = storage.format = VK_FORMAT_R8G8B8A8_UNORM;
	if (!valid()) return false;
	request.params[TextureObject::PARAM_FORMAT] |= 9u;
	sampled.format = storage.format = VK_FORMAT_R8G8B8A8_SRGB;
	if (!valid()) return false;
	request.params[TextureObject::PARAM_WIDTH_HEIGHT] = (1ull << 32u) | 1;
	sampled.extent = storage.extent = storage.physical_extent = {1, 1};
	if (!valid()) return false;
	const auto command = reinterpret_cast<VkCommandBuffer>(uintptr_t(1));
	return !RecordStorageSampledRefresh(nullptr, storage.image, storage.layout, sampled.image, sampled.layout, 1, 1) &&
	       !RecordStorageSampledRefresh(command, storage.image, storage.layout, storage.image, sampled.layout, 1, 1) &&
	       !RecordStorageSampledRefresh(command, storage.image, storage.layout, sampled.image, sampled.layout, 0, 1) &&
	       !RecordStorageSampledRefresh(command, storage.image, VK_IMAGE_LAYOUT_UNDEFINED, sampled.image, sampled.layout, 1, 1);
}

bool TextureMipSnapshotPreflightContractTest()
{
	if (!TextureRenderTargetSnapshotPreflightContractTest()) return false;
	if (!TextureCommonRefreshPreflightContractTest()) return false;
	TextureObject request(10, 0, 1230, 216, 2048, 0, 11, 13, false, DstSel(4, 5, 6, 7), true);
	TextureVulkanImage root;
	StorageTextureVulkanImage storage[10];
	Vector<GpuMemoryObject> sources;
	const auto set_image = [](VulkanImage* image, uintptr_t token, uint32_t width, uint32_t height, bool sampled)
	{
		image->image = reinterpret_cast<VkImage>(token);
		image->image_view[VulkanImage::VIEW_DEFAULT] = reinterpret_cast<VkImageView>(token + 0x1000u);
		image->memory.memory = reinterpret_cast<VkDeviceMemory>(token + 0x2000u);
		image->format = VK_FORMAT_R8G8B8A8_UNORM;
		image->extent = {width, height};
		image->mip_levels = 1;
		image->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (sampled ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT);
		image->layout = sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
	};
	set_image(&root, 1, 1230, 216, true);
	sources.Add({GpuMemoryObjectType::Texture, &root});
	for (uint32_t level = 1; level < 11; ++level)
	{
		auto* image = &storage[level - 1];
		set_image(image, level + 1, std::max(1230u >> level, 1u), std::max(216u >> level, 1u), false);
		image->physical_extent = image->extent;
		image->descriptor_base_level = level;
		image->descriptor_levels = level + 1;
		sources.Add({GpuMemoryObjectType::StorageTexture, image});
	}
	const auto valid = [&]() { return texture_mip_snapshot_sources_valid(request.params, sources); };
	if (!valid() || texture_mip_snapshot_sources_valid(nullptr, sources) ||
	    TextureMipSnapshotPreflight(nullptr, request.params, sources)) return false;

	const auto original_root = root;
	const auto original_storage = storage[0];
	root.usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (valid()) return false;
	root = original_root;
	root.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (valid()) return false;
	root = original_root;
	root.format = VK_FORMAT_R8G8B8A8_SRGB;
	if (valid()) return false;
	root = original_root;
	root.mip_levels = 2;
	if (valid()) return false;
	root = original_root;
	root.memory.memory = nullptr;
	if (valid()) return false;
	root = original_root;
	storage[0].image = root.image;
	if (valid()) return false;
	storage[0] = original_storage;
	storage[0].physical_extent.height += 1;
	if (valid()) return false;
	storage[0] = original_storage;
	storage[0].descriptor_base_level = 0;
	if (valid()) return false;
	storage[0] = original_storage;
	storage[0].descriptor_levels = 3;
	if (valid()) return false;
	storage[0] = original_storage;
	storage[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (valid()) return false;
	storage[0] = original_storage;
	storage[0].extent.width += 1;
	if (valid()) return false;
	storage[0] = original_storage;
	storage[0].usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (valid()) return false;
	storage[0] = original_storage;
	const auto original_peer = sources[1];
	sources[1].type = GpuMemoryObjectType::Texture;
	if (valid()) return false;
	sources[1] = original_peer;
	sources[1].obj = nullptr;
	if (valid()) return false;
	sources[1] = original_peer;
	for (const int param: {TextureObject::PARAM_FORMAT, TextureObject::PARAM_TILE, TextureObject::PARAM_NEO,
	                      TextureObject::PARAM_SWIZZLE, TextureObject::PARAM_POW2_PAD, TextureObject::PARAM_LEVELS})
	{
		request.params[param] ^= 1u;
		if (valid()) return false;
		request.params[param] ^= 1u;
	}
	request.params[TextureObject::PARAM_LEVELS] |= uint64_t(1) << 32u;
	if (valid()) return false;
	request.params[TextureObject::PARAM_LEVELS] = 11;
	request.params[TextureObject::PARAM_PITCH] = 1229;
	if (valid()) return false;
	request.params[TextureObject::PARAM_PITCH] = 2048;
	request.params[TextureObject::PARAM_WIDTH_HEIGHT] = (uint64_t(512) << 32u) | 216u;
	if (valid()) return false;
	request.params[TextureObject::PARAM_WIDTH_HEIGHT] = (uint64_t(1230) << 32u) | 216u;

	return valid();
}

} // namespace Lumen::Libs::Graphics

#endif
