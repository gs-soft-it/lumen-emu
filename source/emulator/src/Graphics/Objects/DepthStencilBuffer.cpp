#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"

#include "Lumen/Core/DbgAssert.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Profiler.h"

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

static void update_func(GraphicContext* /*ctx*/, const uint64_t* /*params*/, void* /*obj*/, const uint64_t* /*vaddr*/,
                        const uint64_t* /*size*/, int /*vaddr_num*/)
{
	LUMEN_PROFILER_BLOCK("DepthStencilBufferObject::update_func");
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("DepthStencilBufferObject::Create");

	EXIT_IF(size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	auto pixel_format = static_cast<VkFormat>(params[DepthStencilBufferObject::PARAM_FORMAT]);
	auto width        = params[DepthStencilBufferObject::PARAM_WIDTH];
	auto height       = params[DepthStencilBufferObject::PARAM_HEIGHT];
	bool htile        = params[DepthStencilBufferObject::PARAM_HTILE] != 0;
	bool sampled      = params[DepthStencilBufferObject::PARAM_USAGE] == 1;
	bool uniform_zero_import = params[DepthStencilBufferObject::PARAM_UNIFORM_ZERO_IMPORT] == 1;

	EXIT_NOT_IMPLEMENTED(pixel_format == VK_FORMAT_UNDEFINED);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);
	EXIT_NOT_IMPLEMENTED(params[DepthStencilBufferObject::PARAM_UNIFORM_ZERO_IMPORT] > 1 ||
	                     params[DepthStencilBufferObject::PARAM_RESERVED] != 0 ||
	                     params[DepthStencilBufferObject::PARAM_UNIFORM_ZERO_REQUIRED] > 1 ||
	                     (params[DepthStencilBufferObject::PARAM_UNIFORM_ZERO_REQUIRED] != 0 && !uniform_zero_import));
	EXIT_NOT_IMPLEMENTED(uniform_zero_import && pixel_format != VK_FORMAT_D32_SFLOAT_S8_UINT);

	auto* vk_obj = new DepthStencilVulkanImage;

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = pixel_format;
	vk_obj->image         = nullptr;
	vk_obj->layout        = VK_IMAGE_LAYOUT_UNDEFINED;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	vk_obj->compressed = !htile;

	VkImageCreateInfo image_info {};
	image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext         = nullptr;
	image_info.flags         = 0;
	image_info.imageType     = VK_IMAGE_TYPE_2D;
	image_info.extent.width  = vk_obj->extent.width;
	image_info.extent.height = vk_obj->extent.height;
	image_info.extent.depth  = 1;
	image_info.mipLevels     = 1;
	image_info.arrayLayers   = 1;
	image_info.format        = vk_obj->format;
	image_info.tiling        = VK_IMAGE_TILING_OPTIMAL;
	image_info.initialLayout = vk_obj->layout;
	image_info.usage         = static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) |
	                   (sampled ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT) : static_cast<VkImageUsageFlags>(0)) |
	                   (uniform_zero_import ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) : 0u);
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.samples     = VK_SAMPLE_COUNT_1_BIT;
	vk_obj->usage          = image_info.usage;
	vk_obj->mip_levels     = image_info.mipLevels;

	vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);

	EXIT_NOT_IMPLEMENTED(vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem->requirements);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, mem);

	vk_obj->memory = *mem;

	// EXIT_NOT_IMPLEMENTED(mem->requirements.size > *size);

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.pNext                           = nullptr;
	create_info.flags                           = 0;
	create_info.image                           = vk_obj->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = vk_obj->format;
	create_info.components.r                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.g                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.b                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	// The render-pass view covers the complete attachment. Without
	// separateDepthStencilLayouts, combined formats must transition and attach
	// depth and stencil together; a depth-only sampling view is created below.
	create_info.subresourceRange.aspectMask     = UtilGetDepthStencilAspectMask(vk_obj->format);
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = 0;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = 1;

	vkCreateImageView(ctx->device, &create_info, nullptr, &vk_obj->image_view[VulkanImage::VIEW_DEFAULT]);

	VkImageViewCreateInfo create_info2 {};
	create_info2.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info2.pNext                           = nullptr;
	create_info2.flags                           = 0;
	create_info2.image                           = vk_obj->image;
	create_info2.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info2.format                          = vk_obj->format;
	create_info2.components.r                    = VK_COMPONENT_SWIZZLE_R;
	create_info2.components.g                    = VK_COMPONENT_SWIZZLE_R;
	create_info2.components.b                    = VK_COMPONENT_SWIZZLE_R;
	create_info2.components.a                    = VK_COMPONENT_SWIZZLE_R;
	create_info2.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
	create_info2.subresourceRange.baseArrayLayer = 0;
	create_info2.subresourceRange.baseMipLevel   = 0;
	create_info2.subresourceRange.layerCount     = 1;
	create_info2.subresourceRange.levelCount     = 1;

	vkCreateImageView(ctx->device, &create_info2, nullptr, &vk_obj->image_view[VulkanImage::VIEW_DEPTH_TEXTURE]);

	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);
	EXIT_NOT_IMPLEMENTED(vk_obj->image_view[VulkanImage::VIEW_DEPTH_TEXTURE] == nullptr);

	if (uniform_zero_import)
	{
		UtilInitializeDepthStencilZero(ctx, vk_obj);
	} else
	{
		UtilSetDepthLayoutOptimal(vk_obj);
	}

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("DepthStencilBufferObject::delete_func");

	auto* vk_obj = reinterpret_cast<DepthStencilVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	DeleteFramebuffer(vk_obj);

	vkDestroyImageView(ctx->device, vk_obj->image_view[VulkanImage::VIEW_DEPTH_TEXTURE], nullptr);
	vkDestroyImageView(ctx->device, vk_obj->image_view[VulkanImage::VIEW_DEFAULT], nullptr);

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

bool DepthStencilBufferObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_WIDTH] == other[PARAM_WIDTH] &&
	        params[PARAM_HEIGHT] == other[PARAM_HEIGHT] && params[PARAM_HTILE] == other[PARAM_HTILE] &&
	        params[PARAM_NEO] == other[PARAM_NEO] && params[PARAM_USAGE] == other[PARAM_USAGE] &&
	        params[PARAM_UNIFORM_ZERO_IMPORT] == other[PARAM_UNIFORM_ZERO_IMPORT] &&
	        params[PARAM_RESERVED] == other[PARAM_RESERVED] &&
	        params[PARAM_UNIFORM_ZERO_REQUIRED] == other[PARAM_UNIFORM_ZERO_REQUIRED]);
}

GpuObject::create_func_t DepthStencilBufferObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::delete_func_t DepthStencilBufferObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t DepthStencilBufferObject::GetUpdateFunc() const
{
	return update_func;
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
