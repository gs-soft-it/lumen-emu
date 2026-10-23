#include "Emulator/Graphics/Objects/StorageTexture.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/TextureThin1dReinterpretCopy.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

bool StorageTextureWindowSupported(uint32_t base_level, uint32_t levels)
{
	if (levels == 0 || levels >= 16)
	{
		return false;
	}
	if (base_level == 0)
	{
		return true;
	}
	return base_level < 15 && levels == base_level + 1;
}

bool StorageTextureUploadLevelIncluded(uint32_t base_level, uint32_t levels, uint32_t level)
{
	if (!StorageTextureWindowSupported(base_level, levels))
	{
		return false;
	}
	return base_level == 0 ? level < levels : level == base_level;
}

static VkFormat get_texture_format(uint32_t dfmt, uint32_t nfmt)
{
	if (nfmt == 0 && dfmt == 1)
	{
		return VK_FORMAT_R8_UNORM;
	}
	if (nfmt == 9 && dfmt == 10)
	{
		return VK_FORMAT_R8G8B8A8_SRGB;
	}
	if (nfmt == 0 && dfmt == 10)
	{
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
	if (nfmt == 9 && dfmt == 37)
	{
		return VK_FORMAT_BC3_SRGB_BLOCK;
	}
	EXIT("unknown format: nfmt = %u, dfmt = %u\n", nfmt, dfmt);
	return VK_FORMAT_UNDEFINED;
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

static VkImageUsageFlags get_usage(bool sampled)
{
	VkImageUsageFlags vk_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

	vk_usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	if (sampled)
	{
		vk_usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}

	return vk_usage;
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
	LUMEN_PROFILER_BLOCK("StorageTextureObject::update_impl");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = static_cast<StorageTextureVulkanImage*>(obj);

	auto tile   = params[StorageTextureObject::PARAM_TILE];
	auto dfmt   = (params[StorageTextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt   = (params[StorageTextureObject::PARAM_FORMAT]) & 0xffu;
	auto width  = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto base_level = params[StorageTextureObject::PARAM_LEVELS] >> 32u;
	auto levels = params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto pitch  = params[StorageTextureObject::PARAM_PITCH];
	bool pow2_pad = params[StorageTextureObject::PARAM_POW2_PAD] != 0;
	bool neo    = Config::IsNeo();

	VkImageLayout vk_layout = VK_IMAGE_LAYOUT_GENERAL;

	EXIT_NOT_IMPLEMENTED(!StorageTextureWindowSupported(base_level, levels));

	EXIT_NOT_IMPLEMENTED(tile != 8 && tile != 13);

	TileSizeOffset level_sizes[16];

	TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, nullptr, level_sizes, nullptr, pow2_pad);

	// dbg_test_mipmaps(ctx, VK_FORMAT_BC3_SRGB_BLOCK, 512, 512);

	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	uint32_t mip_pitch  = pitch;

	EXIT_NOT_IMPLEMENTED(base_level >= levels);
	Vector<BufferImageCopy> regions(base_level == 0 ? levels : 1);
	for (uint32_t i = 0; i < levels; i++)
	{
		EXIT_NOT_IMPLEMENTED(level_sizes[i].size == 0);

		if (StorageTextureUploadLevelIncluded(static_cast<uint32_t>(base_level), static_cast<uint32_t>(levels), i))
		{
			auto& region     = regions[base_level == 0 ? i : 0];
			region.offset    = level_sizes[i].offset;
			region.width     = mip_width;
			region.height    = mip_height;
			region.pitch     = mip_pitch;
			region.dst_level = 0;
			if (base_level == 0)
			{
				auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);
				region.dst_x       = mipmap_offset.first;
				region.dst_y       = mipmap_offset.second;
			} else
			{
				region.dst_x = 0;
				region.dst_y = 0;
			}
		}

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

	if (tile == 13)
	{
		// EXIT_NOT_IMPLEMENTED(pitch != width);
		auto* temp_buf = new uint8_t[*size];
		TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), TileMode::TextureTiled, dfmt, nfmt, width, height, pitch,
		                         levels, neo, tile, *size, pow2_pad);
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

static void* create_impl(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, const uint64_t* vaddr,
                         const uint64_t* size, int vaddr_num, VulkanMemory* mem,
                         GpuMemoryScenario create_scenario = GpuMemoryScenario::Common,
                         const Vector<GpuMemoryObject>* source_objects = nullptr)
{
	LUMEN_PROFILER_BLOCK("StorageTextureObject::Create");

	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	const bool reinterpret_snapshot = create_scenario == GpuMemoryScenario::TextureReinterpretSnapshot;
	EXIT_IF(reinterpret_snapshot ? (buffer == nullptr || source_objects == nullptr || source_objects->Size() != 1)
	                             : (create_scenario != GpuMemoryScenario::Common || size == nullptr || vaddr == nullptr));

	auto dfmt       = (params[StorageTextureObject::PARAM_FORMAT] >> 8u) & 0xffu;
	auto nfmt       = (params[StorageTextureObject::PARAM_FORMAT]) & 0xffu;
	auto width      = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] >> 32u;
	auto height     = params[StorageTextureObject::PARAM_WIDTH_HEIGHT] & 0xffffffffu;
	auto base_level = params[StorageTextureObject::PARAM_LEVELS] >> 32u;
	auto levels     = params[StorageTextureObject::PARAM_LEVELS] & 0xffffffffu;
	auto swizzle    = params[StorageTextureObject::PARAM_SWIZZLE];
	auto pitch      = params[StorageTextureObject::PARAM_PITCH];
	bool pow2_pad   = params[StorageTextureObject::PARAM_POW2_PAD] != 0;

	EXIT_NOT_IMPLEMENTED(!StorageTextureWindowSupported(base_level, levels));
	const bool     single_window = base_level > 0;
	const uint32_t host_width = single_window ? std::max(static_cast<uint32_t>(width >> base_level), 1u)
	                                          : static_cast<uint32_t>(width);
	const uint32_t host_height = single_window ? std::max(static_cast<uint32_t>(height >> base_level), 1u)
	                                           : static_cast<uint32_t>(height);
	if (single_window)
	{
		static std::atomic_bool receipt {false};
		if (!receipt.exchange(true, std::memory_order_relaxed))
		{
			std::fprintf(stderr,
			             "[GFX_STORAGE_SINGLE_MIP_WINDOW] representation=single-level-image base=%llu last=%llu "
			             "host=%ux%u upload=guest-level-only usage=storage+sampled policy=base-equals-last\n",
			             static_cast<unsigned long long>(base_level), static_cast<unsigned long long>(levels - 1),
			             host_width, host_height);
			std::fflush(stderr);
		}
	}

	VkImageUsageFlags vk_usage = get_usage(single_window);

	VkComponentMapping components {};

	components.r = get_swizzle(GetDstSel(swizzle, 0));
	components.g = get_swizzle(GetDstSel(swizzle, 1));
	components.b = get_swizzle(GetDstSel(swizzle, 2));
	components.a = get_swizzle(GetDstSel(swizzle, 3));

	auto pixel_format = get_texture_format(dfmt, nfmt);

	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);
	if (pixel_format == VK_FORMAT_R8_UNORM)
	{
		VkPhysicalDeviceFeatures features {};
		vkGetPhysicalDeviceFeatures(ctx->physical_device, &features);
		EXIT_NOT_IMPLEMENTED(features.shaderStorageImageExtendedFormats != VK_TRUE);
	}
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);

	auto real_height = ((levels > 1) ? height + (height > 1 ? height / 2 : 1) : height);

	auto* vk_obj = new StorageTextureVulkanImage;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent.width  = host_width;
	image_info.extent.height = single_window ? host_height : real_height;
	image_info.extent.depth  = 1;
	image_info.mipLevels     = 1;
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

	vk_obj->extent.width  = host_width;
	vk_obj->extent.height = host_height;
	vk_obj->physical_extent.width       = image_info.extent.width;
	vk_obj->physical_extent.height      = image_info.extent.height;
	vk_obj->descriptor_base_level       = static_cast<uint32_t>(base_level);
	vk_obj->descriptor_levels           = static_cast<uint32_t>(levels);
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
	create_info.subresourceRange.baseMipLevel   = 0;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = VK_REMAINING_MIP_LEVELS;

	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);

	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);

	if (reinterpret_snapshot)
	{
		const auto& source_object = source_objects->At(0);
		EXIT_IF(source_object.type != GpuMemoryObjectType::StorageTexture || source_object.obj == nullptr ||
		        width > UINT32_MAX || height > UINT32_MAX || params[StorageTextureObject::PARAM_PITCH] > UINT32_MAX);
		auto* source = static_cast<StorageTextureVulkanImage*>(source_object.obj);
		auto bytes_per_element = [](VkFormat format) -> uint32_t
		{
			switch (format)
			{
				case VK_FORMAT_R8_UNORM: return 1;
				case VK_FORMAT_R8G8B8A8_UNORM: return 4;
				default: return 0;
			}
		};
		const uint32_t source_bytes_per_element      = bytes_per_element(source->format);
		const uint32_t destination_bytes_per_element = bytes_per_element(vk_obj->format);
		EXIT_IF(source_bytes_per_element == 0 || destination_bytes_per_element == 0 || source->extent.width == 0 ||
		        params[StorageTextureObject::PARAM_PITCH] == 0 ||
		        source->extent.height > UINT64_MAX / source->extent.width ||
		        static_cast<uint64_t>(source->extent.width) * source->extent.height >
		            UINT64_MAX / source_bytes_per_element ||
		        height > UINT64_MAX / params[StorageTextureObject::PARAM_PITCH] ||
		        params[StorageTextureObject::PARAM_PITCH] * height >
		            UINT64_MAX / destination_bytes_per_element);
		const uint64_t source_size = static_cast<uint64_t>(source->extent.width) * source->extent.height *
		                             source_bytes_per_element;
		uint64_t destination_size = 0;
		if (vk_obj->format == VK_FORMAT_R8G8B8A8_UNORM)
		{
			EXIT_IF(width > UINT32_MAX || height > UINT32_MAX || pitch > UINT32_MAX ||
			        !TextureThin1dSingleLevelRgba8DestinationSize(
			            static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(pitch),
			            pow2_pad, &destination_size));
		} else
		{
			EXIT_IF(pow2_pad);
			destination_size = static_cast<uint64_t>(pitch) * height * destination_bytes_per_element;
		}
		const TextureThin1dReinterpretCopyInfo copy {
		    source,
		    vk_obj,
		    source->extent.width,
		    static_cast<uint32_t>(pitch),
		    source_size,
		    destination_size,
		    pow2_pad,
		};
		EXIT_IF(!TextureThin1dReinterpretCopyRecord(ctx, buffer, copy));
	} else
	{
		update_impl(ctx, buffer, params, vk_obj, vaddr, size, vaddr_num);
	}

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

static void* create_from_objects_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params,
	                                  GpuMemoryScenario scenario, const Vector<GpuMemoryObject>& objects,
	                                  VulkanMemory* mem)
{
	EXIT_IF(scenario != GpuMemoryScenario::TextureReinterpretSnapshot);
	return create_impl(ctx, buffer, params, nullptr, nullptr, 0, mem, scenario, &objects);
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("StorageTextureObject::delete_func");

	auto* vk_obj = reinterpret_cast<StorageTextureVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	DeleteDescriptor(vk_obj);

	vkDestroyImageView(ctx->device, vk_obj->image_view[VulkanImage::VIEW_DEFAULT], nullptr);

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

bool StorageTextureObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_PITCH] == other[PARAM_PITCH] &&
	        params[PARAM_WIDTH_HEIGHT] == other[PARAM_WIDTH_HEIGHT] && params[PARAM_LEVELS] == other[PARAM_LEVELS] &&
	        params[PARAM_TILE] == other[PARAM_TILE] && params[PARAM_NEO] == other[PARAM_NEO] &&
	        params[PARAM_SWIZZLE] == other[PARAM_SWIZZLE] && params[PARAM_POW2_PAD] == other[PARAM_POW2_PAD]);
}

GpuObject::create_func_t StorageTextureObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_with_buffer_func_t StorageTextureObject::GetCreateWithBufferFunc() const
{
	return create_with_buffer_func;
}

GpuObject::create_from_objects_func_t StorageTextureObject::GetCreateFromObjectsFunc() const
{
	return create_from_objects_func;
}

GpuObject::delete_func_t StorageTextureObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t StorageTextureObject::GetUpdateFunc() const
{
	return update_func;
}

GpuObject::update_with_buffer_func_t StorageTextureObject::GetUpdateWithBufferFunc() const
{
	return update_with_buffer_func;
}

} // namespace Lumen::Libs::Graphics

#endif
