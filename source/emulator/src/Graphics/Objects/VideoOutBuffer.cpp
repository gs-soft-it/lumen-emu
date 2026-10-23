#include "Emulator/Graphics/Objects/VideoOutBuffer.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOutBufferVulkanContract.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// IWYU pragma: no_forward_declare VkImageView_T

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

bool VideoOutBufferReadbackEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_VIDEOOUT_READBACK");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

bool VideoOutBufferObject::SupportsReadback(VideoOutBufferFormat format, uint64_t width, uint64_t height, uint64_t pitch,
	                                        bool tiled, bool neo, uint64_t address, uint64_t size)
{
	if ((format != VideoOutBufferFormat::R8G8B8A8Srgb && format != VideoOutBufferFormat::B8G8R8A8Srgb) ||
	    width == 0 || height == 0 || width > UINT32_MAX || height > UINT32_MAX || pitch > UINT32_MAX ||
	    pitch < width || address == 0 || size == 0 || size > UINT64_MAX - address) return false;
	if (tiled)
	{
		TileSizeAlign layout;
		return TileTryGetDisplay2dThinTextureSize(10, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
		                                        static_cast<uint32_t>(pitch), 1, 10, neo, false, &layout, nullptr, nullptr) &&
		       layout.align != 0 && address % layout.align == 0 && size >= layout.size;
	}
	uint64_t row_bytes = 0, span = 0;
	return address % 256u == 0 &&
	       UtilTryGetImageReadbackSpan(VK_FORMAT_R8G8B8A8_SRGB, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
	                                  static_cast<uint32_t>(pitch), &row_bytes, &span) && span <= size;
}

bool VideoOutBufferReadbackSupported(const uint64_t* params, const VulkanImage* image, uint64_t address, uint64_t size)
{
	const auto reject = [&](const char* reason) {
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
#endif
		static const bool trace = [] {
			const char* value = std::getenv("LUMEN_GFX_EOP_RESOURCE_TRACE");
			return value != nullptr && std::strcmp(value, "1") == 0;
		}();
		static std::atomic<uint32_t> records {0};
		if (trace && records.fetch_add(1, std::memory_order_relaxed) < 16)
			std::fprintf(stderr, "[GPU_VIDEOOUT_READBACK_CONTRACT] reason=%s image_present=%u native_type=%u"
			                     " native_format=%u native_width=%u native_height=%u native_mips=%u"
			                     " native_usage=%u native_layout=%u address=0x%016" PRIx64 " bytes=%" PRIu64
			                     " params_present=%u format=%" PRIu64 " width=%" PRIu64 " height=%" PRIu64
			                     " pitch=%" PRIu64 " tiled=%" PRIu64 " neo=%" PRIu64 " admission=unchanged\n",
			    reason, image != nullptr ? 1u : 0u, image != nullptr ? static_cast<unsigned>(image->type) : 0u,
			    image != nullptr ? static_cast<unsigned>(image->format) : 0u,
			    image != nullptr ? image->extent.width : 0u, image != nullptr ? image->extent.height : 0u,
			    image != nullptr ? image->mip_levels : 0u, image != nullptr ? image->usage : 0u,
			    image != nullptr ? static_cast<unsigned>(image->layout) : 0u, address, size,
			    params != nullptr ? 1u : 0u, params != nullptr ? params[VideoOutBufferObject::PARAM_FORMAT] : 0u,
			    params != nullptr ? params[VideoOutBufferObject::PARAM_WIDTH] : 0u,
			    params != nullptr ? params[VideoOutBufferObject::PARAM_HEIGHT] : 0u,
			    params != nullptr ? params[VideoOutBufferObject::PARAM_PITCH] : 0u,
			    params != nullptr ? params[VideoOutBufferObject::PARAM_TILED] : 0u,
			    params != nullptr ? params[VideoOutBufferObject::PARAM_NEO] : 0u);
		return false;
	};
	if (params == nullptr || image == nullptr) return reject("missing-contract");
	if (params[VideoOutBufferObject::PARAM_TILED] > 1 || params[VideoOutBufferObject::PARAM_NEO] > 1)
		return reject("guest-flags");
	if (!VideoOutBufferObject::SupportsReadback(static_cast<VideoOutBufferFormat>(params[VideoOutBufferObject::PARAM_FORMAT]),
	        params[VideoOutBufferObject::PARAM_WIDTH], params[VideoOutBufferObject::PARAM_HEIGHT],
	        params[VideoOutBufferObject::PARAM_PITCH], params[VideoOutBufferObject::PARAM_TILED] != 0,
	        params[VideoOutBufferObject::PARAM_NEO] != 0, address, size)) return reject("guest-span-or-format");
	const auto format = params[VideoOutBufferObject::PARAM_FORMAT] == static_cast<uint64_t>(VideoOutBufferFormat::R8G8B8A8Srgb)
	                        ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
	if (image->type != VulkanImageType::VideoOut) return reject("native-type");
	if (image->image == VK_NULL_HANDLE || image->memory.memory == VK_NULL_HANDLE || image->memory.unique_id == 0)
		return reject("native-allocation");
	if (image->format != format || image->extent.width != params[VideoOutBufferObject::PARAM_WIDTH] ||
	    image->extent.height != params[VideoOutBufferObject::PARAM_HEIGHT] || image->mip_levels != 1)
		return reject("native-format-or-extent");
	if ((image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) return reject("native-transfer-source");
	if (image->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) return reject("native-layout");
	return true;
}

static void write_back(GraphicContext* ctx, const uint64_t* params, void* object,
	                   const uint64_t* addresses, const uint64_t* sizes, int count)
{
	EXIT_IF(!VideoOutBufferReadbackEnabled() || ctx == nullptr || object == nullptr ||
	        addresses == nullptr || sizes == nullptr || count != 1);
	auto* image = static_cast<VideoOutVulkanImage*>(object);
	EXIT_NOT_IMPLEMENTED(!VideoOutBufferReadbackSupported(params, image, addresses[0], sizes[0]));
	const auto layout = params[VideoOutBufferObject::PARAM_TILED] != 0 ? ImageReadbackLayout::Display2dThin
	                                                               : ImageReadbackLayout::Linear;
	uint64_t generation = 0;
	EXIT_NOT_IMPLEMENTED(!UtilCanReadbackVideoOutToGuest(ctx, image, addresses[0], sizes[0],
	    static_cast<uint32_t>(params[VideoOutBufferObject::PARAM_PITCH]), layout, params[VideoOutBufferObject::PARAM_NEO] != 0,
	    &generation));
	EXIT_NOT_IMPLEMENTED(!VideoOutBufferWriteBack(ctx, params, object, addresses[0], sizes[0], generation));
}

bool VideoOutBufferWriteBack(GraphicContext* ctx, const uint64_t* params, void* object,
	                          uint64_t address, uint64_t size, uint64_t expected_generation, uint64_t* published_hash,
	                          const VideoOutReadbackContinuation* continuation)
{
	if (published_hash != nullptr) *published_hash = 0;
	auto* image = static_cast<VideoOutVulkanImage*>(object);
	if (!VideoOutBufferReadbackEnabled() || !VideoOutBufferReadbackSupported(params, image, address, size)) return false;
	const auto layout = params[VideoOutBufferObject::PARAM_TILED] != 0 ? ImageReadbackLayout::Display2dThin
	                                                               : ImageReadbackLayout::Linear;
	return UtilReadbackVideoOutToGuest(ctx, image, address, size,
	    static_cast<uint32_t>(params[VideoOutBufferObject::PARAM_PITCH]), layout, params[VideoOutBufferObject::PARAM_NEO] != 0,
	    expected_generation, published_hash, continuation);
}

static bool buffer_is_tiled(uint64_t vaddr, uint64_t size)
{
	if (size == 0)
	{
		return false;
	}
	if ((size & 0x7u) == 0)
	{
		const auto* ptr     = reinterpret_cast<const uint64_t*>(vaddr);
		const auto* ptr_end = reinterpret_cast<const uint64_t*>(vaddr + size);
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

static void update_func(GraphicContext* ctx, const uint64_t* params, void* obj, const uint64_t* vaddr, const uint64_t* size, int vaddr_num)
{
	LUMEN_PROFILER_BLOCK("VideoOutBufferObject::update_func");

	EXIT_IF(obj == nullptr);
	EXIT_IF(ctx == nullptr);
	EXIT_IF(params == nullptr);
	EXIT_IF(vaddr == nullptr || size == nullptr || vaddr_num != 1);

	auto* vk_obj = static_cast<VideoOutVulkanImage*>(obj);

	bool tiled  = (params[VideoOutBufferObject::PARAM_TILED] != 0);
	bool neo    = (params[VideoOutBufferObject::PARAM_NEO] != 0);
	EXIT_IF(params[VideoOutBufferObject::PARAM_PITCH] > UINT32_MAX);
	EXIT_IF(params[VideoOutBufferObject::PARAM_WIDTH] > UINT32_MAX);
	EXIT_IF(params[VideoOutBufferObject::PARAM_HEIGHT] > UINT32_MAX);
	const auto pitch  = static_cast<uint32_t>(params[VideoOutBufferObject::PARAM_PITCH]);
	const auto width  = static_cast<uint32_t>(params[VideoOutBufferObject::PARAM_WIDTH]);
	const auto height = static_cast<uint32_t>(params[VideoOutBufferObject::PARAM_HEIGHT]);

	vk_obj->layout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (tiled && buffer_is_tiled(*vaddr, *size))
	{
		EXIT_NOT_IMPLEMENTED(width != pitch);
		auto* temp_buf = new uint8_t[*size];
		TileConvertTiledToLinear(temp_buf, reinterpret_cast<void*>(*vaddr), TileMode::VideoOutTiled, width, height, neo);
		UtilFillImage(ctx, vk_obj, temp_buf, *size, pitch, static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
		delete[] temp_buf;
	} else
	{
		UtilFillImage(ctx, vk_obj, reinterpret_cast<void*>(*vaddr), *size, pitch,
		              static_cast<uint64_t>(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL));
	}
}

static bool videoout_sampled_alias_supported(GraphicContext* ctx, const VkImageCreateInfo& image_info)
{
	EXIT_IF(ctx == nullptr || ctx->physical_device == VK_NULL_HANDLE);
	VkImageFormatProperties image_properties {};
	if (vkGetPhysicalDeviceImageFormatProperties(ctx->physical_device, image_info.format, image_info.imageType,
	                                             image_info.tiling, image_info.usage, image_info.flags,
	                                             &image_properties) != VK_SUCCESS)
	{
		return false;
	}
	VkFormatProperties native_properties {};
	VkFormatProperties unorm_properties {};
	vkGetPhysicalDeviceFormatProperties(ctx->physical_device, image_info.format, &native_properties);
	const VkFormat unorm_format = GetVideoOutUnormViewFormat(image_info.format);
	if (unorm_format == VK_FORMAT_UNDEFINED)
	{
		return false;
	}
	vkGetPhysicalDeviceFormatProperties(ctx->physical_device, unorm_format, &unorm_properties);
	constexpr VkFormatFeatureFlags native_required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
	                                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	                                                  VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
	                                                  VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	return (native_properties.optimalTilingFeatures & native_required) == native_required &&
	       (unorm_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

static VkResult create_videoout_image_view(GraphicContext* ctx, VideoOutVulkanImage* image, int view_index,
	                                        VkFormat format, bool swap_red_blue)
{
	EXIT_IF(ctx == nullptr || image == nullptr || view_index < 0 || view_index >= VulkanImage::VIEW_MAX);
	VkImageViewCreateInfo create_info {};
	create_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create_info.image                           = image->image;
	create_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	create_info.format                          = format;
	create_info.components.r                    = swap_red_blue ? VK_COMPONENT_SWIZZLE_B : VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.g                    = swap_red_blue ? VK_COMPONENT_SWIZZLE_G : VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.b                    = swap_red_blue ? VK_COMPONENT_SWIZZLE_R : VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.components.a                    = VK_COMPONENT_SWIZZLE_IDENTITY;
	create_info.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	create_info.subresourceRange.baseArrayLayer = 0;
	create_info.subresourceRange.baseMipLevel   = 0;
	create_info.subresourceRange.layerCount     = 1;
	create_info.subresourceRange.levelCount     = 1;
	return vkCreateImageView(ctx->device, &create_info, nullptr, &image->image_view[view_index]);
}

static void* create_func(GraphicContext* ctx, const uint64_t* params, const uint64_t* vaddr, const uint64_t* size, int vaddr_num,
                         VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("VideoOutBufferObject::Create");

	EXIT_IF(vaddr_num != 1 || size == nullptr || vaddr == nullptr);
	EXIT_IF(mem == nullptr);
	EXIT_IF(ctx == nullptr);

	const auto pixel_format = params[VideoOutBufferObject::PARAM_FORMAT];
	EXIT_IF(params[VideoOutBufferObject::PARAM_WIDTH] > UINT32_MAX);
	EXIT_IF(params[VideoOutBufferObject::PARAM_HEIGHT] > UINT32_MAX);
	const auto width  = static_cast<uint32_t>(params[VideoOutBufferObject::PARAM_WIDTH]);
	const auto height = static_cast<uint32_t>(params[VideoOutBufferObject::PARAM_HEIGHT]);

	// EXIT_NOT_IMPLEMENTED(pixel_format != 0x80000000);
	EXIT_NOT_IMPLEMENTED(width == 0);
	EXIT_NOT_IMPLEMENTED(height == 0);

	auto* vk_obj = new VideoOutVulkanImage;

	VkFormat vk_format = VK_FORMAT_UNDEFINED;

	switch (pixel_format)
	{
		case static_cast<uint64_t>(VideoOutBufferFormat::R8G8B8A8Srgb): vk_format = VK_FORMAT_R8G8B8A8_SRGB; break;
		case static_cast<uint64_t>(VideoOutBufferFormat::B8G8R8A8Srgb): vk_format = VK_FORMAT_B8G8R8A8_SRGB; break;
		default: EXIT("unknown format: %" PRIu64 "\n", pixel_format);
	}

	vk_obj->extent.width  = width;
	vk_obj->extent.height = height;
	vk_obj->format        = vk_format;
	vk_obj->image         = nullptr;
	vk_obj->layout        = VK_IMAGE_LAYOUT_UNDEFINED;

	for (auto& view: vk_obj->image_view)
	{
		view = nullptr;
	}

	VkImageCreateInfo image_info = BuildVideoOutBufferImageCreateInfo(vk_obj->format, width, height, true);
	const bool sampled_alias = videoout_sampled_alias_supported(ctx, image_info);
	if (!sampled_alias)
	{
		image_info = BuildVideoOutBufferImageCreateInfo(vk_obj->format, width, height, false);
		static std::atomic_bool reported {false};
		if (!reported.exchange(true, std::memory_order_relaxed))
		{
			std::fprintf(stderr,
			             "[GFX_VIDEOOUT_SAMPLED_CAPABILITY] supported=0 policy=legacy-render-present\n");
			std::fflush(stderr);
		}
	}

	uint32_t queue_families[2] = {ctx->queues[GraphicContext::QUEUE_UTIL].family,
	                              ctx->queues[GraphicContext::QUEUE_GFX].family};
	if (queue_families[0] != queue_families[1])
	{
		image_info.sharingMode           = VK_SHARING_MODE_CONCURRENT;
		image_info.queueFamilyIndexCount = 2;
		image_info.pQueueFamilyIndices   = queue_families;
	}

	vk_obj->usage      = image_info.usage;
	vk_obj->flags      = image_info.flags;
	vk_obj->mip_levels = image_info.mipLevels;
	const auto image_result = vkCreateImage(ctx->device, &image_info, nullptr, &vk_obj->image);
	EXIT_NOT_IMPLEMENTED(image_result != VK_SUCCESS || vk_obj->image == nullptr);

	vkGetImageMemoryRequirements(ctx->device, vk_obj->image, &mem->requirements);

	mem->property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	bool allocated = VulkanAllocate(ctx, mem);

	EXIT_NOT_IMPLEMENTED(!allocated);

	VulkanBindImageMemory(ctx, vk_obj, mem);

	vk_obj->memory = *mem;

	printf("VideoOutBufferObject::Create()\n");
	printf("\t mem->requirements.size = %" PRIu64 "\n", mem->requirements.size);
	printf("\t width                  = %" PRIu32 "\n", width);
	printf("\t height                 = %" PRIu32 "\n", height);
	printf("\t size                   = %" PRIu64 "\n", *size);

	// EXIT_NOT_IMPLEMENTED(mem->requirements.size > *size);

	update_func(ctx, params, vk_obj, vaddr, size, vaddr_num);

	const auto default_view_result = create_videoout_image_view(
	    ctx, vk_obj, VulkanImage::VIEW_DEFAULT, vk_obj->format, false);
	EXIT_NOT_IMPLEMENTED(default_view_result != VK_SUCCESS ||
	                     vk_obj->image_view[VulkanImage::VIEW_DEFAULT] == nullptr);
	if (sampled_alias)
	{
		const VkFormat unorm_format = GetVideoOutUnormViewFormat(vk_obj->format);
		const VkResult optional_results[] = {
		    create_videoout_image_view(ctx, vk_obj, VulkanImage::VIEW_BGRA, vk_obj->format, true),
		    create_videoout_image_view(ctx, vk_obj, VulkanImage::VIEW_VIDEOOUT_UNORM_DEFAULT, unorm_format, false),
		    create_videoout_image_view(ctx, vk_obj, VulkanImage::VIEW_VIDEOOUT_UNORM_BGRA, unorm_format, true),
		};
		if (std::any_of(std::begin(optional_results), std::end(optional_results),
		                [](VkResult result) { return result != VK_SUCCESS; }))
		{
			static std::atomic_bool reported {false};
			if (!reported.exchange(true, std::memory_order_relaxed))
			{
				std::fprintf(stderr,
				             "[GFX_VIDEOOUT_SAMPLED_CAPABILITY] supported=partial reason=image-view "
				             "policy=per-view-fail-closed\n");
				std::fflush(stderr);
			}
		}
	}

	return vk_obj;
}

static void delete_func(GraphicContext* ctx, void* obj, VulkanMemory* mem)
{
	LUMEN_PROFILER_BLOCK("VideoOutBufferObject::delete_func");

	auto* vk_obj = reinterpret_cast<VideoOutVulkanImage*>(obj);

	EXIT_IF(vk_obj == nullptr);
	EXIT_IF(ctx == nullptr);

	// if (vk_obj->framebuffer != nullptr)
	{
		DeleteFramebuffer(vk_obj);
	}
	DeleteDescriptor(vk_obj);

	for (auto& view: vk_obj->image_view)
	{
		if (view != VK_NULL_HANDLE)
		{
			vkDestroyImageView(ctx->device, view, nullptr);
			view = VK_NULL_HANDLE;
		}
	}

	vkDestroyImage(ctx->device, vk_obj->image, nullptr);

	VulkanFree(ctx, mem);

	delete vk_obj;
}

bool VideoOutBufferObject::Equal(const uint64_t* other) const
{
	return (params[PARAM_FORMAT] == other[PARAM_FORMAT] && params[PARAM_WIDTH] == other[PARAM_WIDTH] &&
	        params[PARAM_HEIGHT] == other[PARAM_HEIGHT] && params[PARAM_TILED] == other[PARAM_TILED] &&
	        params[PARAM_PITCH] == other[PARAM_PITCH] && params[PARAM_NEO] == other[PARAM_NEO]);
}

GpuObject::create_func_t VideoOutBufferObject::GetCreateFunc() const
{
	return create_func;
}

GpuObject::delete_func_t VideoOutBufferObject::GetDeleteFunc() const
{
	return delete_func;
}

GpuObject::update_func_t VideoOutBufferObject::GetUpdateFunc() const
{
	return update_func;
}

GpuObject::write_back_func_t VideoOutBufferObject::GetWriteBackFunc() const
{
	// Unsupported shapes must reach preflight and fail, never disappear as an
	// object with no callback while this candidate is enabled.
	return VideoOutBufferReadbackEnabled() ? write_back : nullptr;
}

static bool VideoOutReadbackContractTest()
{
	if (const char* expected = std::getenv("LUMEN_EXPECT_GFX_VIDEOOUT_READBACK"))
	{
		if ((std::strcmp(expected, "0") != 0 && std::strcmp(expected, "1") != 0) ||
		    VideoOutBufferReadbackEnabled() != (std::strcmp(expected, "1") == 0)) return false;
	}
	for (const auto format: {VideoOutBufferFormat::R8G8B8A8Srgb, VideoOutBufferFormat::B8G8R8A8Srgb})
	{
		for (const bool neo: {false, true})
		{
			const uint64_t bytes = neo ? 8847360u : 8355840u;
			if (!VideoOutBufferObject::SupportsReadback(format, 1920, 1080, 1920, true, neo, 0x100000, bytes) ||
			    !VideoOutBufferObject::SupportsReadback(format, 1920, 1080, 1920, true, neo, 0x100000, bytes + 37) ||
			    VideoOutBufferObject::SupportsReadback(format, 1920, 1080, 1920, true, neo, 0x100000, bytes - 1) ||
			    VideoOutBufferObject::SupportsReadback(format, 1920, 1080, 1920, true, neo, 0x100001, bytes)) return false;
		}
		if (!VideoOutBufferObject::SupportsReadback(format, 64, 8, 128, false, false, 0x100000, 3840) ||
		    VideoOutBufferObject::SupportsReadback(format, 64, 8, 128, false, false, 0x100000, 3839) ||
		    VideoOutBufferObject::SupportsReadback(format, 64, 8, 128, true, false, 0x100000, 3840) ||
		    VideoOutBufferObject::SupportsReadback(format, 129, 8, 128, true, false, 0x100000, 32768) ||
		    VideoOutBufferObject::SupportsReadback(format, 64, 8, 96, true, false, 0x100000, 32768) ||
		    VideoOutBufferObject::SupportsReadback(format, 64, 0, 128, true, false, 0x100000, 32768) ||
		    VideoOutBufferObject::SupportsReadback(format, UINT64_MAX, 8, 128, true, false, 0x100000, 32768) ||
		    VideoOutBufferObject::SupportsReadback(format, 64, 8, 128, true, false, 0, 32768) ||
		    VideoOutBufferObject::SupportsReadback(format, 64, 8, 128, true, false, 0x100000, UINT64_MAX)) return false;
	}
	if (VideoOutBufferObject::SupportsReadback(VideoOutBufferFormat::Unknown, 64, 8, 128, true, false, 0x100000, 32768)) return false;
	VideoOutBufferObject description(VideoOutBufferFormat::B8G8R8A8Srgb, 64, 8, true, false, 128);
	VideoOutVulkanImage image;
	image.image = reinterpret_cast<VkImage>(uintptr_t {1});
	image.memory.memory = reinterpret_cast<VkDeviceMemory>(uintptr_t {2});
	image.memory.unique_id = 3;
	image.extent = {64, 8};
	image.mip_levels = 1;
	image.format = VK_FORMAT_B8G8R8A8_SRGB;
	image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	if (!VideoOutBufferReadbackSupported(description.params, &image, 0x100000, 32768)) return false;
	for (unsigned invalid = 0; invalid != 10; ++invalid)
	{
		auto changed = image;
		auto params = description;
		switch (invalid)
		{
			case 0: changed.format = VK_FORMAT_B8G8R8A8_UNORM; break;
			case 1: changed.usage = 0; break;
			case 2: changed.layout = VK_IMAGE_LAYOUT_UNDEFINED; break;
			case 3: changed.memory.unique_id = 0; break;
			case 4: changed.image = VK_NULL_HANDLE; break;
			case 5: changed.extent.width++; break;
			case 6: changed.mip_levels++; break;
			case 7: params.params[VideoOutBufferObject::PARAM_TILED] = 2; break;
			case 8: params.params[VideoOutBufferObject::PARAM_NEO] = 2; break;
			case 9: params.params[VideoOutBufferObject::PARAM_PITCH] = UINT64_MAX; break;
		}
		if (VideoOutBufferReadbackSupported(params.params, &changed, 0x100000, 32768)) return false;
	}
	return (description.GetWriteBackFunc() != nullptr) == VideoOutBufferReadbackEnabled();
}

bool VideoOutBufferSampledAliasContractTest()
{
	if (!VideoOutReadbackContractTest()) return false;
	static_assert(GetVideoOutSampleViewIndex(VideoOutSampleView::NativeDefault) == VulkanImage::VIEW_DEFAULT);
	static_assert(GetVideoOutSampleViewIndex(VideoOutSampleView::NativeBgra) == VulkanImage::VIEW_BGRA);
	static_assert(GetVideoOutSampleViewIndex(VideoOutSampleView::UnormDefault) ==
	              VulkanImage::VIEW_VIDEOOUT_UNORM_DEFAULT);
	static_assert(GetVideoOutSampleViewIndex(VideoOutSampleView::UnormBgra) ==
	              VulkanImage::VIEW_VIDEOOUT_UNORM_BGRA);

	const auto legacy = BuildVideoOutBufferImageCreateInfo(VK_FORMAT_B8G8R8A8_SRGB, 1920, 1080, false);
	const auto sampled = BuildVideoOutBufferImageCreateInfo(VK_FORMAT_B8G8R8A8_SRGB, 1920, 1080, true);
	if (legacy.flags != 0 || legacy.usage != GetVideoOutBufferImageUsage(false) ||
	    sampled.flags != VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT || sampled.usage != GetVideoOutBufferImageUsage(true) ||
	    sampled.mipLevels != 1 || sampled.arrayLayers != 1 || sampled.format != VK_FORMAT_B8G8R8A8_SRGB ||
	    sampled.extent.width != 1920 || sampled.extent.height != 1080 || sampled.extent.depth != 1)
	{
		return false;
	}

	constexpr uint32_t xyzw = PackRenderTextureSwizzle(4, 5, 6, 7);
	constexpr uint32_t zyxw = PackRenderTextureSwizzle(6, 5, 4, 7);
	struct ViewCase
	{
		VkFormat           native;
		RenderTextureFormat sampled;
		uint32_t           swizzle;
		VideoOutSampleView expected;
		VkFormat           expected_format;
	};
	constexpr ViewCase view_cases[] = {
	    {VK_FORMAT_R8G8B8A8_SRGB, RenderTextureFormat::R8G8B8A8Unorm, xyzw,
	     VideoOutSampleView::UnormDefault, VK_FORMAT_R8G8B8A8_UNORM},
	    {VK_FORMAT_R8G8B8A8_SRGB, RenderTextureFormat::R8G8B8A8Unorm, zyxw,
	     VideoOutSampleView::UnormBgra, VK_FORMAT_R8G8B8A8_UNORM},
	    {VK_FORMAT_B8G8R8A8_SRGB, RenderTextureFormat::R8G8B8A8Unorm, xyzw,
	     VideoOutSampleView::UnormBgra, VK_FORMAT_B8G8R8A8_UNORM},
	    {VK_FORMAT_B8G8R8A8_SRGB, RenderTextureFormat::R8G8B8A8Unorm, zyxw,
	     VideoOutSampleView::UnormDefault, VK_FORMAT_B8G8R8A8_UNORM},
	    {VK_FORMAT_R8G8B8A8_SRGB, RenderTextureFormat::R8G8B8A8Srgb, xyzw,
	     VideoOutSampleView::NativeDefault, VK_FORMAT_R8G8B8A8_SRGB},
	    {VK_FORMAT_R8G8B8A8_SRGB, RenderTextureFormat::R8G8B8A8Srgb, zyxw,
	     VideoOutSampleView::NativeBgra, VK_FORMAT_R8G8B8A8_SRGB},
	    {VK_FORMAT_B8G8R8A8_SRGB, RenderTextureFormat::R8G8B8A8Srgb, xyzw,
	     VideoOutSampleView::NativeBgra, VK_FORMAT_B8G8R8A8_SRGB},
	    {VK_FORMAT_B8G8R8A8_SRGB, RenderTextureFormat::R8G8B8A8Srgb, zyxw,
	     VideoOutSampleView::NativeDefault, VK_FORMAT_B8G8R8A8_SRGB},
	};
	for (const auto& test: view_cases)
	{
		const auto view = ResolveVideoOutSampleView(test.native, test.sampled, test.swizzle);
		if (!view.IsSupported() || view.view != test.expected || view.format != test.expected_format)
		{
			return false;
		}
	}
	for (uint32_t swizzle = 0; swizzle <= 0xfffu; ++swizzle)
	{
		const bool supported = ResolveVideoOutSampleView(
		                           VK_FORMAT_B8G8R8A8_SRGB, RenderTextureFormat::R8G8B8A8Unorm, swizzle)
		                           .IsSupported();
		if (supported != (swizzle == xyzw || swizzle == zyxw))
		{
			return false;
		}
	}
	if (ResolveVideoOutSampleView(VK_FORMAT_R8G8B8A8_UNORM, RenderTextureFormat::R8G8B8A8Unorm, xyzw).IsSupported() ||
	    ResolveVideoOutSampleView(VK_FORMAT_B8G8R8A8_SRGB, RenderTextureFormat::R8Unorm, xyzw).IsSupported())
	{
		return false;
	}

	VideoOutSampleAliasRequest request {};
	request.graphics_queue       = true;
	request.two_dimensional      = true;
	request.read_only_descriptor = true;
	request.read_only_memory     = true;
	request.registered_tiled     = true;
	request.registered_neo       = true;
	request.requested_neo        = true;
	request.lifetime_valid       = true;
	request.registered_size      = 0x7f8000;
	request.requested_size       = 0x7f8000;
	request.registered_pitch     = 1920;
	request.requested_pitch      = 1920;
	request.image_width          = 1920;
	request.image_height         = 1080;
	request.requested_width      = 1920;
	request.requested_height     = 1080;
	request.data_format          = 10;
	request.number_format        = 0;
	request.tile_mode            = 10;
	request.component_swizzle    = zyxw;
	request.levels               = 1;
	request.image_format         = VK_FORMAT_B8G8R8A8_SRGB;
	request.image_usage          = GetVideoOutBufferImageUsage(true);
	request.image_flags          = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	request.image_mip_levels     = 1;
	request.available_view_mask  = 0xf;
	const auto accepted = ResolveVideoOutSampleAlias(request);
	if (!accepted.IsSupported() || accepted.sample_view.view != VideoOutSampleView::UnormDefault ||
	    accepted.sample_view.format != VK_FORMAT_B8G8R8A8_UNORM)
	{
		return false;
	}

	auto rejects_as = [&request](VideoOutSampleAliasRejection rejection, auto mutate)
	{
		auto changed = request;
		mutate(&changed);
		return ResolveVideoOutSampleAlias(changed).rejection == rejection;
	};
	return rejects_as(VideoOutSampleAliasRejection::NotGraphicsQueue,
	                  [](auto* value) { value->graphics_queue = false; }) &&
	       rejects_as(VideoOutSampleAliasRejection::StorageOrWritable,
	                  [](auto* value) { value->storage_descriptor = true; }) &&
	       rejects_as(VideoOutSampleAliasRejection::StorageOrWritable,
	                  [](auto* value) { value->read_only_memory = false; }) &&
	       rejects_as(VideoOutSampleAliasRejection::RegistrationTiling,
	                  [](auto* value) { value->registered_tiled = false; }) &&
	       rejects_as(VideoOutSampleAliasRejection::NeoMismatch,
	                  [](auto* value) { value->requested_neo = false; }) &&
	       rejects_as(VideoOutSampleAliasRejection::DescriptorShape,
	                  [](auto* value) { value->two_dimensional = false; }) &&
	       rejects_as(VideoOutSampleAliasRejection::DescriptorShape,
	                  [](auto* value) { value->base_level = 1; }) &&
	       rejects_as(VideoOutSampleAliasRejection::DescriptorShape,
	                  [](auto* value) { value->levels = 2; }) &&
	       rejects_as(VideoOutSampleAliasRejection::RangeMismatch,
	                  [](auto* value) { --value->requested_size; }) &&
	       rejects_as(VideoOutSampleAliasRejection::RangeMismatch,
	                  [](auto* value) { ++value->requested_pitch; }) &&
	       rejects_as(VideoOutSampleAliasRejection::ExtentMismatch,
	                  [](auto* value) { --value->requested_width; }) &&
	       rejects_as(VideoOutSampleAliasRejection::FormatOrSwizzle,
	                  [](auto* value) { value->number_format = 7; }) &&
	       rejects_as(VideoOutSampleAliasRejection::FormatOrSwizzle,
	                  [](auto* value) { value->component_swizzle = 0; }) &&
	       rejects_as(VideoOutSampleAliasRejection::ImageContract,
	                  [](auto* value) { value->image_usage &= ~VK_IMAGE_USAGE_SAMPLED_BIT; }) &&
	       rejects_as(VideoOutSampleAliasRejection::ImageContract,
	                  [](auto* value) { value->image_flags = 0; }) &&
	       rejects_as(VideoOutSampleAliasRejection::ImageContract,
	                  [](auto* value) { value->image_mip_levels = 2; }) &&
	       rejects_as(VideoOutSampleAliasRejection::ViewUnavailable,
	                  [](auto* value) { value->available_view_mask &= ~(1u << VulkanImage::VIEW_VIDEOOUT_UNORM_DEFAULT); }) &&
	       rejects_as(VideoOutSampleAliasRejection::LifetimeUnavailable,
	                  [](auto* value) { value->lifetime_valid = false; }) &&
	       rejects_as(VideoOutSampleAliasRejection::SelfFeedback,
	                  [](auto* value) { value->self_feedback = true; });
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
