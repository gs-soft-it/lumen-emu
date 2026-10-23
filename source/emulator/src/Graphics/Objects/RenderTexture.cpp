#include "Emulator/Graphics/Objects/RenderTexture.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/RenderTargetVulkanContract.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

#include <memory>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

bool RenderTextureObject::SupportsDisplay2dReadback(RenderTextureFormat format, uint32_t width, uint32_t height,
                                                    uint32_t pitch, bool neo, uint64_t address, uint64_t size)
{
	if (format != RenderTextureFormat::R8G8B8A8Unorm && format != RenderTextureFormat::B8G8R8A8Unorm) return false;
	TileSizeAlign layout {};
	return address != 0 && size <= UINT64_MAX - address &&
	       TileTryGetDisplay2dThinTextureSize(10, 0, width, height, pitch, 1, 10, neo, false, &layout, nullptr, nullptr) &&
	       layout.align != 0 && address % layout.align == 0 && size >= layout.size;
}

static bool buffer_is_tiled(uint64_t vaddr, uint64_t size)
{
	if ((size & 0x7u) == 0)
	{
		const auto* ptr     = reinterpret_cast<const uint64_t*>(vaddr);
		const auto* ptr_end = reinterpret_cast<const uint64_t*>(vaddr + size / 8);
		for (uint64_t element = *ptr; ptr < ptr_end; ptr++)
		{
			if (element != *ptr)
			{
				return true;
			}
		}
		return false;
	}
	return true;
}

static void update_impl(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj, const uint64_t* vaddr,
                        const uint64_t* size, int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("RenderTextureObject::update_impl");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = static_cast<RenderTextureVulkanImage*>(obj);

	bool tiled = (params[RenderTextureObject::PARAM_TILED] != 0);
	// bool neo    = (params[RenderTextureObject::PARAM_NEO] != 0);
	auto pitch = params[RenderTextureObject::PARAM_PITCH];
	auto width = params[RenderTextureObject::PARAM_WIDTH];
	auto height = params[RenderTextureObject::PARAM_HEIGHT];
	if (tiled && params[RenderTextureObject::PARAM_TILE_MODE] == 14 && vk_obj->format == VK_FORMAT_R8G8B8A8_UNORM)
	{
		// A range handoff can make this attachment CPU authoritative. Restore
		// its native image from the same tile-14 byte interpretation on rebinding.
		auto linear = std::make_unique<uint8_t[]>(*size);
		EXIT_IF(!TileTryConvertColor2dThinToLinear(linear.get(), *size, reinterpret_cast<const void*>(*vaddr), *size,
		    width, height, pitch, params[RenderTextureObject::PARAM_NEO] != 0));
		if (buffer == nullptr)
			UtilFillImage(ctx, vk_obj, linear.get(), *size, pitch, static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
		else
			UtilUploadImage(ctx, buffer, vk_obj, linear.get(), *size, pitch, static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
		return;
	}

	if (tiled && params[RenderTextureObject::PARAM_TILE_MODE] == 10 &&
	    RenderTextureObject::SupportsDisplay2dReadback(static_cast<RenderTextureFormat>(params[RenderTextureObject::PARAM_FORMAT]),
	                                                 width, height, pitch, params[RenderTextureObject::PARAM_NEO] != 0,
	                                                 *vaddr, *size))
	{
		// A guest upload can follow a GPU publication to the same tiled slice.
		// Preserve the guest bytes and upload a separate linear snapshot.
		auto linear = std::make_unique<uint8_t[]>(*size);
		EXIT_IF(!TileTryConvertDisplay2dThinToLinear(linear.get(), *size, reinterpret_cast<const void*>(*vaddr), *size,
		                                           width, height, pitch, params[RenderTextureObject::PARAM_NEO] != 0));
		if (buffer == nullptr)
			UtilFillImage(ctx, vk_obj, linear.get(), *size, pitch, static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
		else
			UtilUploadImage(ctx, buffer, vk_obj, linear.get(), *size, pitch,
			                static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
		return;
	}

	if (tiled && buffer_is_tiled(*vaddr, *size))
	{
		EXIT_NOT_IMPLEMENTED(width != pitch);
		auto* temp_buf = new uint8_t[*size];
		LUMEN_NOT_IMPLEMENTED;
		// TODO()
		// TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), TileMode::VideoOutTiled, width, height, neo);
		// UtilFillImage(ctx, vk_obj, temp_buf, *size, pitch);
		delete[] temp_buf;
	} else
	{
		if (buffer == nullptr)
		{
			UtilFillImage(ctx, vk_obj, reinterpret_cast<void*>(*vaddr), *size, pitch,
			              static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
		} else
		{
			UtilUploadImage(ctx, buffer, vk_obj, reinterpret_cast<void*>(*vaddr), *size, pitch,
			                static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
		}
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

static void update2_func(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, void* obj, GpuMemoryScenario scenario,
                         const Vector<GpuMemoryObject>& objects)
{
	LUMEN_PROFILER_BLOCK("RenderTextureObject::update_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(objects.IsEmpty());

	auto* vk_obj = static_cast<RenderTextureVulkanImage*>(obj);

	// bool tiled = (params[RenderTextureObject::PARAM_TILED] != 0);
	// bool neo    = (params[RenderTextureObject::PARAM_NEO] != 0);
	// auto pitch  = params[RenderTextureObject::PARAM_PITCH];
	auto width  = params[RenderTextureObject::PARAM_WIDTH];
	auto height = params[RenderTextureObject::PARAM_HEIGHT];

	vk_obj->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	//	if (objects.Size() == 2 && objects.At(0).type == GpuMemoryObjectType::StorageBuffer &&
	//	    objects.At(1).type == GpuMemoryObjectType::StorageTexture && scenario == GpuMemoryScenario::GenerateMips)
	//	{
	//		auto* src_obj = static_cast<StorageTextureVulkanImage*>(objects.At(1).obj);
	if (objects.Size() == 3 && objects.At(0).type == GpuMemoryObjectType::StorageBuffer &&
	    objects.At(1).type == GpuMemoryObjectType::Texture && objects.At(2).type == GpuMemoryObjectType::StorageTexture &&
	    scenario == GpuMemoryScenario::GenerateMips)
	{
		auto* src_obj = static_cast<StorageTextureVulkanImage*>(objects.At(2).obj);
		EXIT_IF(src_obj == nullptr);
		EXIT_NOT_IMPLEMENTED(src_obj->descriptor_base_level != 0);
		EXIT_NOT_IMPLEMENTED(!AreRenderTargetCopyFormatsCompatible(src_obj->format, vk_obj->format));

		uint32_t mip_width  = src_obj->extent.width;
		uint32_t mip_height = src_obj->extent.height;

		Vector<ImageImageCopy> regions(1);

		bool updated = false;

		for (uint32_t i = 0; i < 16 && !updated; i++)
		{
			if (mip_width == width && mip_height == height)
			{
				auto mipmap_offset = UtilCalcMipmapOffset(i, width, height);

				regions[0].src_image = src_obj;
				regions[0].src_level = 0;
				regions[0].dst_level = 0;
				regions[0].width     = mip_width;
				regions[0].height    = mip_height;
				regions[0].src_x     = mipmap_offset.first;
				regions[0].src_y     = mipmap_offset.second;
				regions[0].dst_x     = 0;
				regions[0].dst_y     = 0;

				if (buffer == nullptr)
				{
					UtilFillImage(ctx, regions, vk_obj, static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
				} else
				{
					UtilImageToImage(buffer, regions, vk_obj, static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
				}

				updated = true;
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

		EXIT_NOT_IMPLEMENTED(!updated);
	} else
	{
		LUMEN_NOT_IMPLEMENTED;
	}
}

static void create_image_views(GraphicContext* ctx, RenderTextureVulkanImage* vk_obj, RenderTextureFormat render_format)
{
	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = 0;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = 1;

	const auto default_result =
	    vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);
	EXIT_NOT_IMPLEMENTED(default_result != VK_SUCCESS || vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);

	const bool four_channels = render_format == RenderTextureFormat::R8G8B8A8Unorm ||
	                           render_format == RenderTextureFormat::R8G8B8A8Srgb ||
	                           render_format == RenderTextureFormat::B8G8R8A8Unorm ||
	                           render_format == RenderTextureFormat::B8G8R8A8Srgb;
	if (four_channels)
	{
		create_info.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
		                          VK_COMPONENT_SWIZZLE_IDENTITY};
		const auto bgra_result =
		    vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_BGRA]);
		EXIT_NOT_IMPLEMENTED(bgra_result != VK_SUCCESS || vk_obj->image_view[VulkanImage::VIEW_BGRA] == nullptr);
	}
}

static void* create_impl(GraphicContext* ctx, CommandBuffer* buffer, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size,
                         int vaddr_num, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("RenderTextureObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto pixel_format = params[RenderTextureObject::PARAM_FORMAT];
	auto width        = params[RenderTextureObject::PARAM_WIDTH];
	auto height       = params[RenderTextureObject::PARAM_HEIGHT];

	const auto     render_format = static_cast<RenderTextureFormat>(pixel_format);
	const VkFormat vk_format     = GetRenderTextureVulkanFormat(render_format);

	EXIT_NOT_IMPLEMENTED(vk_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);

	auto* vk_obj = new RenderTextureVulkanImage;

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = vk_format;
	vk_obj->image         = nullptr;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	const auto image_info = BuildRenderTextureImageCreateInfo(render_format, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	vk_obj->usage = image_info.usage;
	vk_obj->mip_levels = image_info.mipLevels;

	const auto image_result = vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(image_result != VK_SUCCESS || vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem->requirements);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, mem);

	vk_obj->memory = *mem;

	printf("RenderTextureObject::Create()\n");
	printf("\t mem->requirements.size = %" PRIu64 "\n", mem->requirements.size);
	printf("\t width                  = %" PRIu64 "\n", width);
	printf("\t height                 = %" PRIu64 "\n", height);
	printf("\t size                   = %" PRIu64 "\n", *size);

	// EXIT_NOT_IMPLEMENTED(mem->requirements.size > *size);

	update_impl(ctx, buffer, params, vk_obj, vaddr, size, vaddr_num);
	create_image_views(ctx, vk_obj, render_format);

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
	LUMEN_PROFILER_BLOCK("RenderTextureObject::CreateFromObjects");

	EXIT_IF(objects.IsEmpty());
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto pixel_format = params[RenderTextureObject::PARAM_FORMAT];
	auto width        = params[RenderTextureObject::PARAM_WIDTH];
	auto height       = params[RenderTextureObject::PARAM_HEIGHT];

	const auto     render_format = static_cast<RenderTextureFormat>(pixel_format);
	const VkFormat vk_format     = GetRenderTextureVulkanFormat(render_format);

	EXIT_NOT_IMPLEMENTED(vk_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);
	EXIT_NOT_IMPLEMENTED(objects.Size() != 3 || objects.At(0).type != GpuMemoryObjectType::StorageBuffer ||
	                     objects.At(1).type != GpuMemoryObjectType::Texture ||
	                     objects.At(2).type != GpuMemoryObjectType::StorageTexture || scenario != GpuMemoryScenario::GenerateMips);
	const auto* source = static_cast<const StorageTextureVulkanImage*>(objects.At(2).obj);
	EXIT_IF(source == nullptr);
	EXIT_NOT_IMPLEMENTED(source->descriptor_base_level != 0);
	EXIT_NOT_IMPLEMENTED(!AreRenderTargetCopyFormatsCompatible(source->format, vk_format));

	auto* vk_obj = new RenderTextureVulkanImage;

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = vk_format;
	vk_obj->image         = nullptr;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	const auto image_info = BuildRenderTextureImageCreateInfo(render_format, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
	vk_obj->usage = image_info.usage;
	vk_obj->mip_levels = image_info.mipLevels;

	const auto image_result = vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(image_result != VK_SUCCESS || vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem->requirements);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, mem);

	vk_obj->memory = *mem;

	printf("RenderTextureObject::CreateFromObjects()\n");
	printf("\t mem->requirements.size = %" PRIu64 "\n", mem->requirements.size);
	printf("\t width                  = %" PRIu64 "\n", width);
	printf("\t height                 = %" PRIu64 "\n", height);
	// printf("\t size                   = %" PRIu64 "\n", *size);

	// EXIT_NOT_IMPLEMENTED(mem->requirements.size > *size);

	update2_func(ctx, buffer, params, vk_obj, scenario, objects);
	create_image_views(ctx, vk_obj, render_format);

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("RenderTextureObject::delete_func");

	auto* vk_obj = reinterpret_cast<RenderTextureVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	DeleteDescriptor(vk_obj);

	DeleteFramebuffer(vk_obj);

	for (auto& view: vk_obj->image_view)
	{
		if (view != nullptr)
		{
			vkDestroyImageView(ctx->device, view, nullptr);
			view = nullptr;
		}
	}

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

static void write_back(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("RenderTextureObject::write_back");

	EXIT_IF(ctx == nullptr);
	EXIT_IF(obj == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	bool tiled = (params[RenderTextureObject::PARAM_TILED] != 0);
	auto pitch = params[RenderTextureObject::PARAM_PITCH];
	auto width = params[RenderTextureObject::PARAM_WIDTH];

	EXIT_IF(!(params[RenderTextureObject::PARAM_WRITE_BACK] != 0));
	auto* vk_obj = reinterpret_cast<RenderTextureVulkanImage*>(obj);
	if (tiled && params[RenderTextureObject::PARAM_TILE_MODE] == 10)
	{
		const auto height = params[RenderTextureObject::PARAM_HEIGHT];
		const bool neo = params[RenderTextureObject::PARAM_NEO] != 0;
		EXIT_IF(!RenderTextureObject::SupportsDisplay2dReadback(
		    static_cast<RenderTextureFormat>(params[RenderTextureObject::PARAM_FORMAT]), width, height, pitch, neo, *vaddr, *size));
		EXIT_IF(vk_obj->extent.width != width || vk_obj->extent.height != height);
		UtilFillBuffer(ctx, reinterpret_cast<void*>(*vaddr), *size, pitch, vk_obj, static_cast<uint64_t>(vk_obj->layout),
		               ImageReadbackLayout::Display2dThin, neo);
		return;
	}

	EXIT_NOT_IMPLEMENTED(tiled);
	EXIT_NOT_IMPLEMENTED(width != pitch);

	UtilFillBuffer(ctx, reinterpret_cast<void*>(*vaddr), *size, pitch, vk_obj,
	               static_cast<uint64_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

bool RenderTextureObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_WIDTH] == other[PARAM_WIDTH] &&
	        params[PARAM_HEIGHT] == other[PARAM_HEIGHT] && params[PARAM_TILED] == other[PARAM_TILED] &&
	        params[PARAM_NEO] == other[PARAM_NEO] && params[PARAM_PITCH] == other[PARAM_PITCH] &&
	        params[PARAM_WRITE_BACK] == other[PARAM_WRITE_BACK] && params[PARAM_TILE_MODE] == other[PARAM_TILE_MODE]);
}

bool RenderTextureObjectParameterContractTest()
{
	RenderTextureObject object(RenderTextureFormat::R8G8B8A8Unorm, 480, 270, true, false, 512, false, 10);
	// The shared host array also carries metadata for other resource types.
	// Perturb every parameter belonging to this render-texture description.
	constexpr int parameter_count = RenderTextureObject::PARAM_TILE_MODE + 1;
	static_assert(parameter_count <= GpuObject::PARAMS_MAX);
	uint64_t             candidate[GpuObject::PARAMS_MAX] {};
	for (int index = 0; index < GpuObject::PARAMS_MAX; ++index)
	{
		candidate[index] = object.params[index];
	}
	if (!object.Equal(candidate) || object.access_kind != GpuMemoryAccessKind::ColorAttachmentReadWrite)
	{
		return false;
	}
	for (int index = 0; index < parameter_count; ++index)
	{
		candidate[index] ^= 1u;
		if (object.Equal(candidate))
		{
			return false;
		}
		candidate[index] = object.params[index];
	}
	for (const bool neo: {false, true})
	{
		const uint64_t bytes = neo ? 65536 : 32768;
		for (const auto format: {RenderTextureFormat::R8G8B8A8Unorm, RenderTextureFormat::B8G8R8A8Unorm})
		{
			if (!RenderTextureObject::SupportsDisplay2dReadback(format, 64, 64, 128, neo, 0x100000, bytes) ||
			    !RenderTextureObject::SupportsDisplay2dReadback(format, 64, 64, 128, neo, 0x100000, bytes + 37) ||
			    RenderTextureObject::SupportsDisplay2dReadback(format, 64, 64, 128, neo, 0x100000, bytes - 1) ||
			    RenderTextureObject::SupportsDisplay2dReadback(format, 64, 64, 128, neo, 0x100001, bytes) ||
			    RenderTextureObject::SupportsDisplay2dReadback(format, 64, 64, 128, neo, 0, bytes) ||
			    RenderTextureObject::SupportsDisplay2dReadback(format, 129, 64, 128, neo, 0x100000, bytes) ||
			    RenderTextureObject::SupportsDisplay2dReadback(format, 64, 0, 128, neo, 0x100000, bytes) ||
			    RenderTextureObject::SupportsDisplay2dReadback(format, 64, 64, 96, neo, 0x100000, bytes) ||
			    RenderTextureObject::SupportsDisplay2dReadback(format, 64, 64, 128, neo, 0x100000, UINT64_MAX)) return false;
		}
	}
	for (const auto format: {RenderTextureFormat::Unknown, RenderTextureFormat::R8Unorm, RenderTextureFormat::R8G8Unorm,
	                         RenderTextureFormat::R8G8B8A8Srgb, RenderTextureFormat::B8G8R8A8Srgb})
	{
		if (RenderTextureObject::SupportsDisplay2dReadback(format, 64, 64, 128, false, 0x100000, 32768)) return false;
	}
	return true;
}

GpuObject::create_func_t RenderTextureObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::create_with_buffer_func_t RenderTextureObject::GetCreateWithBufferFunc() const
{
	return create_with_buffer_func;
}

GpuObject::create_from_objects_func_t RenderTextureObject::GetCreateFromObjectsFunc() const
{
	return create2_func;
}

GpuObject::delete_func_t RenderTextureObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t RenderTextureObject::GetUpdateFunc() const
{
	return update_func;
}

GpuObject::update_with_buffer_func_t RenderTextureObject::GetUpdateWithBufferFunc() const
{
	return update_with_buffer_func;
}

GpuObject::write_back_func_t RenderTextureObject::GetWriteBackFunc() const
{
	bool wb = (params[RenderTextureObject::PARAM_WRITE_BACK] != 0);
	return (wb ? write_back : nullptr);
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
