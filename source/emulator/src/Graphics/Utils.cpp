#include "Emulator/Graphics/Utils.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/Vector.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/DepthStencilZeroInit.h"
#include "Emulator/Graphics/DepthStencilZeroReadback.h"
#include "Emulator/Graphics/EopCpuProofResume.h"
#include "Emulator/Graphics/GuestDataLeaseRetry.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/UploadStagingArena.h"
#include "Emulator/Profiler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <vector>
#include <xxhash/xxhash.h>
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
#include "Lumen/Core/Threads.h"
#include <thread>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

static void get_image_layout_source(VkImageLayout layout, VkAccessFlags* access, VkPipelineStageFlags* stages);
static void get_image_layout_destination(VkImageLayout layout, VkAccessFlags* access, VkPipelineStageFlags* stages);

static void record_image_layout(VkCommandBuffer buffer, VkImage image, uint32_t base_level, uint32_t levels,
	                            VkImageAspectFlags aspect_mask, VkImageLayout old_image_layout,
	                            VkImageLayout new_image_layout, VkAccessFlags src_access_mask,
	                            VkAccessFlags dst_access_mask, VkPipelineStageFlags src_stages,
	                            VkPipelineStageFlags dst_stages)
{
	EXIT_IF(buffer == nullptr);
	EXIT_IF(image == nullptr);

	VkImageMemoryBarrier image_memory_barrier {};
	image_memory_barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	image_memory_barrier.pNext                           = nullptr;
	image_memory_barrier.srcAccessMask                   = src_access_mask;
	image_memory_barrier.dstAccessMask                   = dst_access_mask;
	image_memory_barrier.oldLayout                       = old_image_layout;
	image_memory_barrier.newLayout                       = new_image_layout;
	image_memory_barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	image_memory_barrier.image                           = image;
	image_memory_barrier.subresourceRange.aspectMask     = aspect_mask;
	image_memory_barrier.subresourceRange.baseMipLevel   = base_level;
	image_memory_barrier.subresourceRange.levelCount     = levels;
	image_memory_barrier.subresourceRange.baseArrayLayer = 0;
	image_memory_barrier.subresourceRange.layerCount     = 1;

	vkCmdPipelineBarrier(buffer, src_stages, dst_stages, 0, 0, nullptr, 0, nullptr, 1, &image_memory_barrier);
}

static void set_image_layout(VkCommandBuffer buffer, VulkanImage* dst_image, uint32_t base_level, uint32_t levels,
	                         VkImageAspectFlags aspect_mask, VkImageLayout old_image_layout, VkImageLayout new_image_layout,
	                         VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask, VkPipelineStageFlags src_stages,
	                         VkPipelineStageFlags dst_stages)
{
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(old_image_layout != VK_IMAGE_LAYOUT_UNDEFINED && dst_image->layout != old_image_layout);
	record_image_layout(buffer, dst_image->image, base_level, levels, aspect_mask, old_image_layout,
	                    new_image_layout, src_access_mask, dst_access_mask, src_stages, dst_stages);

	dst_image->layout = new_image_layout;
}

static void set_image_layout(VkCommandBuffer buffer, VulkanImage* dst_image, uint32_t base_level, uint32_t levels,
                             VkImageAspectFlags aspect_mask, VkImageLayout old_image_layout, VkImageLayout new_image_layout)
{
	auto get_scope = [](VkImageLayout layout, VkAccessFlags* access, VkPipelineStageFlags* stages)
	{
		switch (layout)
		{
			case VK_IMAGE_LAYOUT_UNDEFINED:
				*access = 0;
				*stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
				break;
			case VK_IMAGE_LAYOUT_PREINITIALIZED:
				*access = VK_ACCESS_HOST_WRITE_BIT;
				*stages = VK_PIPELINE_STAGE_HOST_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				*access = VK_ACCESS_TRANSFER_READ_BIT;
				*stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
				*access = VK_ACCESS_TRANSFER_WRITE_BIT;
				*stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;
			default:
				// Legacy callers can use transfer-only utility queues, where explicit
				// graphics/compute stages are invalid. These scopes cover the commands
				// supported by that queue; callers still own cross-queue synchronization.
				*access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
				*stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
				break;
		}
	};
	VkAccessFlags         src_access_mask = 0;
	VkAccessFlags         dst_access_mask = 0;
	VkPipelineStageFlags src_stages      = 0;
	VkPipelineStageFlags dst_stages      = 0;
	get_scope(old_image_layout, &src_access_mask, &src_stages);
	get_scope(new_image_layout, &dst_access_mask, &dst_stages);

	set_image_layout(buffer, dst_image, base_level, levels, aspect_mask, old_image_layout, new_image_layout, src_access_mask,
	                 dst_access_mask, src_stages, dst_stages);
}

void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, uint32_t src_pitch, VulkanImage* dst_image, uint64_t dst_layout)
{
	EXIT_IF(src_buffer == nullptr);
	EXIT_IF(src_buffer->buffer == nullptr);
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(dst_image->image == nullptr);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	set_image_layout(vk_buffer, dst_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, dst_image->layout,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkBufferImageCopy region {};
	region.bufferOffset      = 0;
	region.bufferRowLength   = (src_pitch != dst_image->extent.width ? src_pitch : 0);
	region.bufferImageHeight = 0;

	region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel       = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount     = 1;

	region.imageOffset = {0, 0, 0};
	region.imageExtent = {dst_image->extent.width, dst_image->extent.height, 1};

	vkCmdCopyBufferToImage(vk_buffer, src_buffer->buffer, dst_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	set_image_layout(vk_buffer, dst_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 static_cast<VkImageLayout>(dst_layout));
}

void UtilImageToBuffer(CommandBuffer* buffer, VulkanImage* src_image, VulkanBuffer* dst_buffer, uint32_t dst_pitch, uint64_t src_layout)
{
	EXIT_IF(dst_buffer == nullptr);
	EXIT_IF(dst_buffer->buffer == nullptr);
	EXIT_IF(src_image == nullptr);
	EXIT_IF(src_image->image == nullptr);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();
	const auto restore_layout = static_cast<VkImageLayout>(src_layout);
	const bool canonical_videoout = src_image->type == VulkanImageType::VideoOut;
	const auto current_layout = canonical_videoout ? restore_layout : src_image->layout;
	EXIT_IF(canonical_videoout && restore_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	VkAccessFlags         source_access = 0;
	VkPipelineStageFlags source_stages = 0;
	get_image_layout_source(current_layout, &source_access, &source_stages);
	if (canonical_videoout)
	{
		record_image_layout(vk_buffer, src_image->image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, current_layout,
		                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_access, VK_ACCESS_TRANSFER_READ_BIT,
		                    source_stages, VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else
	{
		set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, current_layout,
		                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_access, VK_ACCESS_TRANSFER_READ_BIT,
		                 source_stages, VK_PIPELINE_STAGE_TRANSFER_BIT);
	}

	VkBufferImageCopy region {};
	region.bufferOffset      = 0;
	region.bufferRowLength   = (dst_pitch != src_image->extent.width ? dst_pitch : 0);
	region.bufferImageHeight = 0;

	region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel       = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount     = 1;

	region.imageOffset = {0, 0, 0};
	region.imageExtent = {src_image->extent.width, src_image->extent.height, 1};

	vkCmdCopyImageToBuffer(vk_buffer, src_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buffer->buffer, 1, &region);

	VkBufferMemoryBarrier host_barrier {};
	host_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	host_barrier.pNext               = nullptr;
	host_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	host_barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host_barrier.buffer               = dst_buffer->buffer;
	host_barrier.offset               = 0;
	host_barrier.size                 = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
	                     &host_barrier, 0, nullptr);

	VkAccessFlags         destination_access = 0;
	VkPipelineStageFlags destination_stages = 0;
	get_image_layout_destination(restore_layout, &destination_access, &destination_stages);
	if (canonical_videoout)
	{
		record_image_layout(vk_buffer, src_image->image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT,
		                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, restore_layout, VK_ACCESS_TRANSFER_READ_BIT,
		                    destination_access, VK_PIPELINE_STAGE_TRANSFER_BIT, destination_stages);
	} else
	{
		set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT,
		                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, restore_layout, VK_ACCESS_TRANSFER_READ_BIT,
		                 destination_access, VK_PIPELINE_STAGE_TRANSFER_BIT, destination_stages);
	}
}

void UtilBufferToImage(CommandBuffer* buffer, VulkanBuffer* src_buffer, VulkanImage* dst_image, const Vector<BufferImageCopy>& regions,
                       uint64_t dst_layout)
{
	EXIT_IF(src_buffer == nullptr);
	EXIT_IF(src_buffer->buffer == nullptr);
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(dst_image->image == nullptr);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	EXIT_NOT_IMPLEMENTED(regions.Size() >= 16);

	VkBufferImageCopy region[16];

	uint32_t index = 0;
	for (const auto& r: regions)
	{
		region[index].bufferOffset                    = r.offset;
		region[index].bufferRowLength                 = (r.width != r.pitch ? r.pitch : 0);
		region[index].bufferImageHeight               = 0;
		region[index].imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region[index].imageSubresource.mipLevel       = r.dst_level;
		region[index].imageSubresource.baseArrayLayer = 0;
		region[index].imageSubresource.layerCount     = 1;
		region[index].imageOffset                     = {r.dst_x, r.dst_y, 0};
		region[index].imageExtent                     = {r.width, r.height, 1};
		index++;
	}

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT, dst_image->layout,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	vkCmdCopyBufferToImage(vk_buffer, src_buffer->buffer, dst_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, index, region);

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 static_cast<VkImageLayout>(dst_layout));
}

void UtilImageToImage(CommandBuffer* buffer, const Vector<ImageImageCopy>& regions, VulkanImage* dst_image, uint64_t dst_layout)
{
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(dst_image->image == nullptr);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	EXIT_NOT_IMPLEMENTED(regions.Size() >= 16);

	VkAccessFlags         dst_source_access = 0;
	VkPipelineStageFlags dst_source_stages = 0;
	get_image_layout_source(dst_image->layout, &dst_source_access, &dst_source_stages);

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT, dst_image->layout,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_source_access, VK_ACCESS_TRANSFER_WRITE_BIT, dst_source_stages,
	                 VK_PIPELINE_STAGE_TRANSFER_BIT);

	for (const auto& r: regions)
	{
		VkImageCopy region;

		auto src_layout = r.src_image->layout;
		VkAccessFlags         src_access = 0;
		VkPipelineStageFlags src_stages = 0;
		get_image_layout_source(src_layout, &src_access, &src_stages);

		region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.mipLevel       = r.src_level;
		region.srcSubresource.baseArrayLayer = 0;
		region.srcSubresource.layerCount     = 1;
		region.srcOffset                     = {r.src_x, r.src_y, 0};
		region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.mipLevel       = r.dst_level;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount     = 1;
		region.dstOffset                     = {r.dst_x, r.dst_y, 0};
		region.extent                        = {r.width, r.height, 1};

		set_image_layout(vk_buffer, r.src_image, r.src_level, 1, VK_IMAGE_ASPECT_COLOR_BIT, src_layout,
		                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_access, VK_ACCESS_TRANSFER_READ_BIT, src_stages,
		                 VK_PIPELINE_STAGE_TRANSFER_BIT);

		vkCmdCopyImage(vk_buffer, r.src_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_image->image,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		VkAccessFlags         src_destination_access = 0;
		VkPipelineStageFlags src_destination_stages = 0;
		get_image_layout_destination(src_layout, &src_destination_access, &src_destination_stages);
		set_image_layout(vk_buffer, r.src_image, r.src_level, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                 src_layout, VK_ACCESS_TRANSFER_READ_BIT, src_destination_access, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                 src_destination_stages);
	}

	VkAccessFlags         dst_destination_access = 0;
	VkPipelineStageFlags dst_destination_stages = 0;
	get_image_layout_destination(static_cast<VkImageLayout>(dst_layout), &dst_destination_access, &dst_destination_stages);
	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<VkImageLayout>(dst_layout), VK_ACCESS_TRANSFER_WRITE_BIT,
	                 dst_destination_access, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_destination_stages);
}

BlitDestinationRect UtilComputeBlitDestinationRect(uint32_t source_width, uint32_t source_height,
                                                   uint32_t destination_width, uint32_t destination_height,
                                                   bool preserve_aspect_ratio)
{
	BlitDestinationRect destination {0, 0, destination_width, destination_height};
	if (!preserve_aspect_ratio || source_width == 0 || source_height == 0 || destination_width == 0 ||
	    destination_height == 0)
	{
		return destination;
	}

	const uint64_t source_cross      = static_cast<uint64_t>(source_width) * destination_height;
	const uint64_t destination_cross = static_cast<uint64_t>(destination_width) * source_height;
	if (source_cross > destination_cross)
	{
		destination.height = static_cast<uint32_t>(static_cast<uint64_t>(destination_width) * source_height / source_width);
		destination.height = destination.height > 0 ? destination.height : 1;
		destination.y      = (destination_height - destination.height) / 2;
	} else if (source_cross < destination_cross)
	{
		destination.width = static_cast<uint32_t>(static_cast<uint64_t>(destination_height) * source_width / source_height);
		destination.width = destination.width > 0 ? destination.width : 1;
		destination.x     = (destination_width - destination.width) / 2;
	}
	return destination;
}

void UtilBlitImage(CommandBuffer* buffer, VulkanImage* src_image, VulkanSwapchain* dst_swapchain,
                   bool preserve_aspect_ratio)
{
	EXIT_IF(src_image == nullptr);
	EXIT_IF(src_image->image == nullptr);
	EXIT_IF(dst_swapchain == nullptr);
	EXIT_IF(dst_swapchain->swapchain_images == nullptr);
	EXIT_IF(dst_swapchain->swapchain_image_layouts == nullptr);
	EXIT_IF(dst_swapchain->current_index >= dst_swapchain->swapchain_images_count);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	VulkanImage swapchain_image(VulkanImageType::Unknown);

	const bool canonical_videoout = src_image->type == VulkanImageType::VideoOut;
	const auto src_layout       = canonical_videoout ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : src_image->layout;
	const auto swapchain_layout = dst_swapchain->swapchain_image_layouts[dst_swapchain->current_index];

	EXIT_NOT_IMPLEMENTED(src_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	EXIT_NOT_IMPLEMENTED(swapchain_layout != VK_IMAGE_LAYOUT_UNDEFINED && swapchain_layout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	swapchain_image.image  = dst_swapchain->swapchain_images[dst_swapchain->current_index];
	swapchain_image.layout = swapchain_layout;

	if (canonical_videoout)
	{
		record_image_layout(vk_buffer, src_image->image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, src_layout,
		                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                    VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else
	{
		set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, src_layout,
		                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                 VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                 VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	// Match the acquire semaphore's TRANSFER wait before the layout transition.
	set_image_layout(vk_buffer, &swapchain_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, swapchain_layout,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                 VK_PIPELINE_STAGE_TRANSFER_BIT);

	const auto destination = UtilComputeBlitDestinationRect(
	    src_image->extent.width, src_image->extent.height,
	    dst_swapchain->swapchain_extent.width, dst_swapchain->swapchain_extent.height,
	    preserve_aspect_ratio);

	const bool has_letterbox = destination.x != 0 || destination.y != 0 ||
	                           destination.width != dst_swapchain->swapchain_extent.width ||
	                           destination.height != dst_swapchain->swapchain_extent.height;
	if (has_letterbox)
	{
		VkClearColorValue black {};
		black.float32[3] = 1.0f;
		VkImageSubresourceRange range {};
		range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		range.baseMipLevel   = 0;
		range.levelCount     = 1;
		range.baseArrayLayer = 0;
		range.layerCount     = 1;
		vkCmdClearColorImage(vk_buffer, swapchain_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
		set_image_layout(vk_buffer, &swapchain_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT,
		                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                 VK_PIPELINE_STAGE_TRANSFER_BIT);
	}

	VkImageBlit region {};
	region.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.mipLevel       = 0;
	region.srcSubresource.baseArrayLayer = 0;
	region.srcSubresource.layerCount     = 1;
	region.srcOffsets[0].x               = 0;
	region.srcOffsets[0].y               = 0;
	region.srcOffsets[0].z               = 0;
	region.srcOffsets[1].x               = static_cast<int>(src_image->extent.width);
	region.srcOffsets[1].y               = static_cast<int>(src_image->extent.height);
	region.srcOffsets[1].z               = 1;
	region.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	region.dstSubresource.mipLevel       = 0;
	region.dstSubresource.baseArrayLayer = 0;
	region.dstSubresource.layerCount     = 1;
	region.dstOffsets[0].x               = static_cast<int32_t>(destination.x);
	region.dstOffsets[0].y               = static_cast<int32_t>(destination.y);
	region.dstOffsets[0].z               = 0;
	region.dstOffsets[1].x               = static_cast<int32_t>(destination.x + destination.width);
	region.dstOffsets[1].y               = static_cast<int32_t>(destination.y + destination.height);
	region.dstOffsets[1].z               = 1;

	vkCmdBlitImage(vk_buffer, src_image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_image.image,
	               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);

	if (canonical_videoout)
	{
		record_image_layout(vk_buffer, src_image->image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT,
		                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_layout, VK_ACCESS_TRANSFER_READ_BIT,
		                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	} else
	{
		set_image_layout(vk_buffer, src_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT,
		                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_layout, VK_ACCESS_TRANSFER_READ_BIT,
		                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
	}
	set_image_layout(vk_buffer, &swapchain_image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	dst_swapchain->swapchain_image_layouts[dst_swapchain->current_index] = swapchain_image.layout;
}

static bool readback_cached_override(const char* value, bool default_enabled)
{
	return default_enabled && !(value != nullptr && value[0] == '0' && value[1] == '\0');
}

static bool readback_cached_enabled()
{
#if ((LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
     LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	constexpr bool default_enabled = true;
#else
	constexpr bool default_enabled = false;
#endif
	static const bool enabled = readback_cached_override(std::getenv("LUMEN_VULKAN_READBACK_HOST_CACHED"), default_enabled);
	return enabled;
}

template <typename Allocate>
static bool allocate_readback_by_preference(const VkPhysicalDeviceMemoryProperties& properties,
	                                         uint32_t compatible_types, bool cached, Allocate&& allocate)
{
	if (properties.memoryTypeCount > VK_MAX_MEMORY_TYPES) return false;
	constexpr auto visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	constexpr auto host_cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	const VkMemoryPropertyFlags preferences[] = {visible | coherent | host_cached, visible | host_cached, visible | coherent};
	uint32_t attempted = 0;
	for (uint32_t preference = cached ? 0u : 2u; preference < 3; ++preference)
	{
		const auto required = preferences[preference];
		for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
		{
			const auto bit = uint32_t{1} << index;
			if ((compatible_types & bit) != 0 && (attempted & bit) == 0 &&
			    (properties.memoryTypes[index].propertyFlags & required) == required)
			{
				// A flags-only fallback could select the same failed cached type.
				// Each exact type is attempted once, then broader tiers may try others.
				attempted |= bit;
				if (allocate(index, required)) return true;
			}
		}
	}
	return false;
}

static bool allocate_readback_memory(GraphicContext* ctx, VulkanMemory* memory)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &properties);
	const bool enabled = readback_cached_enabled();
	if (!allocate_readback_by_preference(properties, memory->requirements.memoryTypeBits, enabled,
	    [&](uint32_t index, VkMemoryPropertyFlags required) {
		    memory->property = required;
		    return VulkanTryAllocate(ctx, memory, index);
	    })) return false;
	EXIT_IF(memory->type >= properties.memoryTypeCount || memory->offset != 0);
	// Record the actual flags, so cached noncoherent memory is invalidated.
	memory->property = properties.memoryTypes[memory->type].propertyFlags;
	static std::atomic_bool reported {false};
	if (!reported.exchange(true, std::memory_order_relaxed))
		std::fprintf(stderr, "[GFX_READBACK_MEMORY] cached_preferred=%u type=%u flags=0x%x compatible_types=0x%x"
		                     " cached=%u coherent=%u bytes=%" PRIu64 " scope=first-readback-buffer\n",
		             enabled ? 1u : 0u, memory->type, memory->property, memory->requirements.memoryTypeBits,
		             (memory->property & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0 ? 1u : 0u,
		             (memory->property & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0 ? 1u : 0u, memory->requirements.size);
	return true;
}

static void create_buffer(GraphicContext* gctx, uint64_t size, VulkanBuffer* buffer, bool readback)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(gctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->buffer != nullptr);

	VkBufferCreateInfo buffer_info {};
	buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size        = size;
	buffer_info.usage       = buffer->usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	vkCreateBuffer(gctx->device, &buffer_info, nullptr, &buffer->buffer);
	EXIT_NOT_IMPLEMENTED(buffer->buffer == nullptr);

	vkGetBufferMemoryRequirements(gctx->device, buffer->buffer, &buffer->memory.requirements);

	bool allocated = readback ? allocate_readback_memory(gctx, &buffer->memory) : VulkanAllocate(gctx, &buffer->memory);
	EXIT_NOT_IMPLEMENTED(!allocated);

	// vkBindBufferMemory(gctx->device, buffer->buffer, buffer->memory.memory, buffer->memory.offset);
	VulkanBindBufferMemory(gctx, buffer, &buffer->memory);
}

void VulkanCreateBuffer(GraphicContext* gctx, uint64_t size, VulkanBuffer* buffer)
{
	create_buffer(gctx, size, buffer, false);
}

namespace {
constexpr uint64_t READBACK_STAGING_MAX_BYTES = 64u * 1024u * 1024u;

bool copy_readback_image(void* dst, uint64_t dst_size, const void* src, uint64_t src_size, VkFormat format,
                         uint32_t width, uint32_t height, uint32_t pitch, ImageReadbackLayout layout, bool neo)
{
	if (layout == ImageReadbackLayout::Linear)
		return UtilCopyReadbackRows(dst, dst_size, src, src_size, format, width, height, pitch);
	if (layout == ImageReadbackLayout::Color2dThin)
		return format == VK_FORMAT_R8G8B8A8_UNORM &&
		       TileTryConvertLinearToColor2dThin(dst, dst_size, src, src_size, width, height, pitch, neo);
	if (layout != ImageReadbackLayout::Display2dThin ||
	    (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM &&
	     format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_B8G8R8A8_SRGB)) return false;
	// The transfer initialized every logical texel in mapped staging. Read it
	// directly, preserving guest padding without allocating/zeroing/copying a
	// second full-size linear image. The converter preflights before any write.
	return TileTryConvertLinearToDisplay2dThin(dst, dst_size, src, src_size, width, height, pitch, neo);
}

bool direct_tiled_readback_contract_test()
{
	constexpr uint32_t width = 13, height = 9, pitch = 128;
	constexpr size_t guard = 17;
	for (const bool neo: {false, true})
	{
		TileSizeAlign tile {};
		if (!TileTryGetDisplay2dThinTextureSize(10, 0, width, height, pitch, 1, 10, neo, false,
		                                     &tile, nullptr, nullptr)) return false;
		for (const auto format: {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
		                        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB})
		{
			std::vector<uint8_t> source(tile.size + 2 * guard, 0xe7);
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width * 4; ++x)
					source[guard + y * pitch * 4 + x] = static_cast<uint8_t>(y * 37 + x * 13);
			const auto original = source;
			std::vector<uint8_t> linear(tile.size, 0x39), reference(tile.size + 2 * guard, 0xa5);
			auto direct = reference;
			if (!UtilCopyReadbackRows(linear.data(), tile.size, source.data() + guard, tile.size,
			                         format, width, height, pitch) ||
			    !TileTryConvertLinearToDisplay2dThin(reference.data() + guard, tile.size, linear.data(), tile.size,
			                                        width, height, pitch, neo) ||
			    !copy_readback_image(direct.data() + guard, tile.size, source.data() + guard, tile.size,
			                         format, width, height, pitch, ImageReadbackLayout::Display2dThin, neo) ||
			    direct != reference || source != original) return false;
			const auto unchanged = direct;
			if (copy_readback_image(direct.data() + guard, tile.size, source.data() + guard, tile.size,
			                        VK_FORMAT_R8_UNORM, width, height, pitch, ImageReadbackLayout::Display2dThin, neo) ||
			    copy_readback_image(direct.data() + guard, tile.size - 1, source.data() + guard, tile.size,
			                        format, width, height, pitch, ImageReadbackLayout::Display2dThin, neo) ||
			    copy_readback_image(direct.data() + guard, tile.size, source.data() + guard, 1,
			                        format, width, height, pitch, ImageReadbackLayout::Display2dThin, neo) ||
			    copy_readback_image(direct.data() + guard, tile.size, direct.data() + guard, tile.size,
			                        format, width, height, pitch, ImageReadbackLayout::Display2dThin, neo) ||
			    direct != unchanged || source != original) return false;
		}
	}
	std::fprintf(stderr, "[GFX_READBACK_DIRECT_TILE_TEST] rgba-bgra=pass neo-base=pass padded-partial=pass"
	                     " canaries=pass source-unchanged=pass invalid-no-output=pass status=clean\n");
	return true;
}

bool readback_staging_reuse_override(const char* value)
{
	return value == nullptr || (value[0] == '1' && value[1] == '\0');
}

bool readback_staging_reuse_enabled()
{
	static const bool enabled = readback_staging_reuse_override(std::getenv("LUMEN_VULKAN_READBACK_STAGING_REUSE"));
	return enabled;
}

enum class ReadbackStagingAction { Transient, Allocate, Reuse };

// One slot bounds retained memory across all contexts. An overlapping call or
// foreign context never waits for the slot; it keeps the dedicated allocation
// path. Active transient calls also prevent teardown of a live readback.
struct ReadbackStagingPolicy
{
	const void* owner = nullptr;
	VkDevice device = nullptr;
	uint64_t capacity = 0;
	uint64_t allocation_bytes = 0;
	uint64_t requested = 0;
	uint64_t active = 0;
	bool leased = false;

	ReadbackStagingAction Begin(const void* context, VkDevice current_device, uint64_t bytes, bool enabled)
	{
		++active;
		if (!enabled || context == nullptr || current_device == nullptr || bytes == 0 ||
		    bytes > READBACK_STAGING_MAX_BYTES || leased ||
		    (owner != nullptr && (owner != context || device != current_device))) return ReadbackStagingAction::Transient;
		owner = context;
		device = current_device;
		leased = true;
		requested = bytes;
		return capacity >= bytes ? ReadbackStagingAction::Reuse : ReadbackStagingAction::Allocate;
	}

	bool CanRetain(uint64_t buffer_bytes, uint64_t memory_bytes) const
	{
		return leased && buffer_bytes >= requested && buffer_bytes <= READBACK_STAGING_MAX_BYTES &&
		       memory_bytes >= buffer_bytes && memory_bytes <= READBACK_STAGING_MAX_BYTES;
	}

	bool FinishLease(uint64_t buffer_bytes, uint64_t memory_bytes)
	{
		EXIT_IF(!leased || active == 0);
		const bool retain = CanRetain(buffer_bytes, memory_bytes);
		leased = false;
		requested = 0;
		--active;
		capacity = retain ? buffer_bytes : 0;
		allocation_bytes = retain ? memory_bytes : 0;
		if (!retain) { owner = nullptr; device = nullptr; }
		return retain;
	}

	void FinishTransient()
	{
		EXIT_IF(active == 0 || (leased && active == 1));
		--active;
	}

	bool Reset(const void* context, VkDevice current_device)
	{
		if (active != 0 || (owner == context && device != current_device)) return false;
		if (owner == context) { owner = nullptr; device = nullptr; capacity = 0; allocation_bytes = 0; }
		return true;
	}
};

struct ReadbackStagingCache
{
	std::mutex mutex;
	ReadbackStagingPolicy policy;
	VulkanBuffer buffer;
	uint64_t completed = 0, allocations = 0, reuses = 0, transients = 0;

	void ReportLocked(const char* phase) const
	{
		std::fprintf(stderr, "[GFX_READBACK_STAGING] phase=%s enabled=%u completed=%" PRIu64
		             " allocations=%" PRIu64 " reuses=%" PRIu64 " transients=%" PRIu64
		             " capacity=%" PRIu64 " allocation_bytes=%" PRIu64 " limit=%" PRIu64 " active=%" PRIu64 "\n",
		             phase, readback_staging_reuse_enabled() ? 1u : 0u, completed, allocations, reuses, transients,
		             policy.capacity, policy.allocation_bytes, READBACK_STAGING_MAX_BYTES, policy.active);
	}
};

// Vulkan ownership is released explicitly at graphics session reset, never
// from a static destructor after the device or allocator statistics expire.
ReadbackStagingCache& g_readback_staging = *new ReadbackStagingCache;

class ReadbackStagingLease
{
public:
	ReadbackStagingLease(GraphicContext* ctx, uint64_t size): m_ctx(ctx), m_device(ctx->device), m_size(size)
	{
		ReadbackStagingAction action;
		{
			std::lock_guard<std::mutex> lock(g_readback_staging.mutex);
			action = g_readback_staging.policy.Begin(ctx, m_device, size, readback_staging_reuse_enabled());
			m_pooled = action != ReadbackStagingAction::Transient;
			if (m_pooled) m_buffer = std::exchange(g_readback_staging.buffer, VulkanBuffer{});
			if (action == ReadbackStagingAction::Reuse)
			{
				m_size = g_readback_staging.policy.capacity;
				++g_readback_staging.reuses;
			} else if (!m_pooled) { ++g_readback_staging.transients; }
		}
		if (action != ReadbackStagingAction::Reuse)
		{
			// The preceding lease already completed its fence and unmapped. Do
			// not hold the slot mutex while calling the driver or descriptor cache.
			if (m_buffer.buffer != nullptr) VulkanDeleteBuffer(ctx, &m_buffer);
			m_buffer = {};
			m_buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			m_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			create_buffer(ctx, size, &m_buffer, true);
			std::lock_guard<std::mutex> lock(g_readback_staging.mutex);
			++g_readback_staging.allocations;
		}
		EXIT_IF(m_buffer.buffer == nullptr || m_buffer.memory.memory == nullptr);
	}

	ReadbackStagingLease(const ReadbackStagingLease&) = delete;
	ReadbackStagingLease& operator=(const ReadbackStagingLease&) = delete;

	~ReadbackStagingLease()
	{
		EXIT_IF(m_ctx->device != m_device);
		bool retain = false;
		if (m_pooled)
		{
			std::lock_guard<std::mutex> lock(g_readback_staging.mutex);
			retain = g_readback_staging.policy.CanRetain(m_size, m_buffer.memory.requirements.size);
		}
		// Destruction happens before the active-call count reaches zero, so a
		// simultaneous reset cannot destroy the device while a transient frees.
		if (!retain) VulkanDeleteBuffer(m_ctx, &m_buffer);
		std::lock_guard<std::mutex> lock(g_readback_staging.mutex);
		if (m_pooled)
		{
			EXIT_IF(g_readback_staging.policy.FinishLease(m_size, m_buffer.memory.requirements.size) != retain);
			EXIT_IF(g_readback_staging.buffer.buffer != nullptr);
			if (retain) g_readback_staging.buffer = std::exchange(m_buffer, VulkanBuffer{});
		} else { g_readback_staging.policy.FinishTransient(); }
		const uint64_t completed = ++g_readback_staging.completed;
		if ((completed & (completed - 1)) == 0) g_readback_staging.ReportLocked("completed");
	}

	VulkanBuffer* Get() { return &m_buffer; }

private:
	GraphicContext* m_ctx;
	VkDevice m_device;
	uint64_t m_size;
	bool m_pooled = false;
	VulkanBuffer m_buffer;
};

bool readback_staging_policy_contract_test()
{
	const int context_a = 1, context_b = 2;
	const auto device_a = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(1));
	const auto device_b = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(2));
	using Action = ReadbackStagingAction;
	ReadbackStagingPolicy policy;
	if (!policy.Reset(&context_a, device_a) || policy.Begin(&context_a, device_a, 4096, true) != Action::Allocate ||
	    policy.Reset(&context_a, device_a) || policy.CanRetain(2048, 4096) ||
	    policy.CanRetain(4096, READBACK_STAGING_MAX_BYTES + 1)) return false;
	if (policy.Begin(&context_a, device_a, 4096, true) != Action::Transient || policy.active != 2) return false;
	policy.FinishTransient();
	if (!policy.FinishLease(4096, 8192) || policy.active != 0 || policy.capacity != 4096) return false;
	if (policy.Begin(&context_a, device_a, 2048, true) != Action::Reuse ||
	    !policy.FinishLease(4096, 8192)) return false;
	// A dedicated concurrent call may outlive the pooled one. It must still
	// block reset, without preventing another call from borrowing the idle slot.
	if (policy.Begin(&context_a, device_a, 4096, true) != Action::Reuse ||
	    policy.Begin(&context_a, device_a, 4096, true) != Action::Transient ||
	    !policy.FinishLease(4096, 8192) || policy.Reset(&context_a, device_a) ||
	    policy.Begin(&context_a, device_a, 4096, true) != Action::Reuse) return false;
	policy.FinishTransient();
	if (!policy.FinishLease(4096, 8192)) return false;
	if (policy.Begin(&context_a, device_a, 8192, true) != Action::Allocate ||
	    !policy.FinishLease(8192, 8192) || policy.capacity != 8192) return false;
	if (policy.Begin(&context_b, device_a, 4096, true) != Action::Transient) return false;
	policy.FinishTransient();
	if (policy.Begin(&context_a, device_b, 4096, true) != Action::Transient) return false;
	policy.FinishTransient();
	if (policy.Reset(&context_a, device_b) || !policy.Reset(&context_b, device_a) || policy.owner != &context_a) return false;
	for (const uint64_t bytes: {uint64_t{0}, READBACK_STAGING_MAX_BYTES + 1, UINT64_MAX})
	{
		if (policy.Begin(&context_a, device_a, bytes, true) != Action::Transient || policy.Reset(&context_a, device_a)) return false;
		policy.FinishTransient();
	}
	if (policy.Begin(&context_a, device_a, 4096, false) != Action::Transient) return false;
	policy.FinishTransient();
	if (!policy.Reset(&context_a, device_a) || policy.owner != nullptr || policy.capacity != 0 ||
	    policy.Begin(&context_b, device_b, READBACK_STAGING_MAX_BYTES, true) != Action::Allocate ||
	    policy.FinishLease(READBACK_STAGING_MAX_BYTES, READBACK_STAGING_MAX_BYTES + 4096) || policy.owner != nullptr ||
	    policy.Begin(&context_a, device_a, READBACK_STAGING_MAX_BYTES, true) != Action::Allocate ||
	    !policy.FinishLease(READBACK_STAGING_MAX_BYTES, READBACK_STAGING_MAX_BYTES) ||
	    !policy.Reset(&context_a, device_a)) return false;
	const char* disabled[] = {"", "0", "01", "10", "true"};
	for (const auto* value: disabled) if (readback_staging_reuse_override(value)) return false;
	if (!readback_staging_reuse_override(nullptr) || !readback_staging_reuse_override("1")) return false;
	std::fprintf(stderr, "[GFX_READBACK_STAGING_TEST] reuse=pass growth=pass concurrent-transient=pass owner-device=pass"
	                     " byte-cap=pass allocation-cap=pass reset-busy=pass reset-reacquire=pass rollback=pass status=clean\n");
	return true;
}
} // namespace

bool UtilResetReadbackStaging(GraphicContext* ctx)
{
	if (ctx == nullptr || ctx->device == nullptr) return false;
	VulkanBuffer retired;
	{
		std::lock_guard<std::mutex> lock(g_readback_staging.mutex);
		const bool owned = g_readback_staging.policy.owner == ctx;
		if (!g_readback_staging.policy.Reset(ctx, ctx->device)) return false;
		if (owned) retired = std::exchange(g_readback_staging.buffer, VulkanBuffer{});
		if (g_readback_staging.completed != 0) g_readback_staging.ReportLocked("reset");
		g_readback_staging.completed = g_readback_staging.allocations = g_readback_staging.reuses = g_readback_staging.transients = 0;
	}
	if (retired.buffer != nullptr) VulkanDeleteBuffer(ctx, &retired);
	return true;
}

static bool videoout_publication_contract_test();
static bool color_publication_contract_test();
static bool image_regions_host_readback_layout(const VulkanImage* image, const Vector<BufferImageCopy>& regions,
    uint64_t bytes, int producer_queue, VkBufferImageCopy* copies)
{
	const int queue = producer_queue < 0 ? GraphicContext::QUEUE_UTIL : producer_queue;
	if (image == nullptr || copies == nullptr ||
	    queue < 0 || queue >= GraphicContext::QUEUES_NUM ||
	    bytes == 0 || bytes > READBACK_STAGING_MAX_BYTES || regions.IsEmpty() || regions.Size() > 16 ||
	    image->image == VK_NULL_HANDLE || image->mip_levels != 1 ||
	    (image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 || image->layout == VK_IMAGE_LAYOUT_UNDEFINED) return false;
	uint32_t physical_width = image->extent.width, physical_height = image->extent.height;
	if (image->type == VulkanImageType::StorageTexture)
	{
		const auto* storage = static_cast<const StorageTextureVulkanImage*>(image);
		physical_width = storage->physical_extent.width; physical_height = storage->physical_extent.height;
	}
	uint32_t index = 0;
	for (const auto& region: regions)
	{
		uint64_t row = 0, span = 0;
		if (region.dst_level != 0 || !UtilTryGetImageReadbackSpan(image->format, region.width, region.height, region.pitch, &row, &span) ||
		    region.offset > bytes || span > bytes - region.offset || region.dst_x > physical_width ||
		    region.width > physical_width - region.dst_x || region.dst_y > physical_height ||
		    region.height > physical_height - region.dst_y) return false;
		auto& copy = copies[index++];
		copy.bufferOffset = region.offset;
		copy.bufferRowLength = region.pitch;
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageOffset = {static_cast<int32_t>(region.dst_x), static_cast<int32_t>(region.dst_y), 0};
		copy.imageExtent = {region.width, region.height, 1};
	}
	return true;
}

bool UtilCanReadbackImageRegionsToHost(const VulkanImage* image, const Vector<BufferImageCopy>& regions,
    uint64_t bytes, int producer_queue)
{
	VkBufferImageCopy copies[16] {};
	return image_regions_host_readback_layout(image, regions, bytes, producer_queue, copies);
}

bool UtilReadbackImageRegionsToHost(GraphicContext* ctx, const VulkanImage* image,
    const Vector<BufferImageCopy>& regions, uint64_t bytes, std::vector<uint8_t>* output, int producer_queue)
{
	VkBufferImageCopy copies[16] {};
	if (ctx == nullptr || output == nullptr || output->size() != bytes ||
	    !image_regions_host_readback_layout(image, regions, bytes, producer_queue, copies)) return false;
	const int queue = producer_queue < 0 ? GraphicContext::QUEUE_UTIL : producer_queue;
	ReadbackStagingLease staging(ctx, bytes);
	auto* target = staging.Get();
	CommandBuffer command(queue);
	if (command.IsInvalid()) return false;
	command.Begin();
	auto native = command.GetBufferOutsideRenderPass();
	record_image_layout(native, image->image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, image->layout,
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyImageToBuffer(native, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->buffer, regions.Size(), copies);
	VkBufferMemoryBarrier host {}; host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = target->buffer; host.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(native, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
	record_image_layout(native, image->image, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    image->layout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	command.End(); command.Execute(); command.WaitForFence();
	void* mapped = nullptr;
	if (vkMapMemory(ctx->device, target->memory.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || mapped == nullptr) return false;
	if ((target->memory.property & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
	{
		VkMappedMemoryRange range {}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = target->memory.memory; range.size = VK_WHOLE_SIZE;
		if (vkInvalidateMappedMemoryRanges(ctx->device, 1, &range) != VK_SUCCESS)
		{ VulkanUnmapMemory(ctx, &target->memory); return false; }
	}
	for (const auto& region: regions)
	{
		uint64_t row = 0, span = 0;
		UtilTryGetImageReadbackSpan(image->format, region.width, region.height, region.pitch, &row, &span);
		const uint64_t stride = uint64_t(region.pitch) * (row / region.width);
		for (uint32_t y = 0; y < region.height; ++y)
			std::memcpy(output->data() + region.offset + y * stride, static_cast<const uint8_t*>(mapped) + region.offset + y * stride, row);
	}
	VulkanUnmapMemory(ctx, &target->memory);
	return true;
}

static bool texture_mips_host_readback_contract_test();

static bool image_regions_host_readback_contract_test()
{
	StorageTextureVulkanImage image {};
	image.type = VulkanImageType::StorageTexture;
	image.image = reinterpret_cast<VkImage>(uintptr_t {0x1000});
	image.mip_levels = 1; image.format = VK_FORMAT_R8G8B8A8_UNORM;
	image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT; image.layout = VK_IMAGE_LAYOUT_GENERAL;
	image.extent = {8, 4}; image.physical_extent = {8, 4};
	BufferImageCopy region {};
	region.offset = 16; region.width = 3; region.height = 2; region.pitch = 4;
	region.dst_x = 1; region.dst_y = 1;
	Vector<BufferImageCopy> regions; regions.Add(region);
	if (!UtilCanReadbackImageRegionsToHost(&image, regions, 64, GraphicContext::QUEUE_GFX)) return false;
	for (unsigned invalid = 0; invalid < 8; ++invalid)
	{
		auto changed = image;
		Vector<BufferImageCopy> query; query.Add(region);
		int queue = GraphicContext::QUEUE_GFX;
		switch (invalid)
		{
			case 0: changed.usage = 0; break;
			case 1: changed.layout = VK_IMAGE_LAYOUT_UNDEFINED; break;
			case 2: changed.physical_extent.width = 3; break;
			case 3: changed.format = VK_FORMAT_UNDEFINED; break;
			case 4: changed.mip_levels = 2; break;
			case 5: queue = GraphicContext::QUEUES_NUM; break;
			case 6: query[0].offset = UINT32_MAX; break;
			case 7: query[0].dst_level = 1; break;
		}
		if (UtilCanReadbackImageRegionsToHost(&changed, query, 64, queue)) return false;
	}
	return true;
}

bool VulkanReadbackMemoryContractTest()
{
	if (!direct_tiled_readback_contract_test()) return false;
	if (!readback_staging_policy_contract_test()) return false;
	if (!texture_mips_host_readback_contract_test()) return false;
	if (!image_regions_host_readback_contract_test()) return false;
	if (!videoout_publication_contract_test() || !color_publication_contract_test()) return false;
	constexpr auto visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
	constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	constexpr auto cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	VkPhysicalDeviceMemoryProperties properties {};
	properties.memoryTypeCount = 4;
	properties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	properties.memoryTypes[1].propertyFlags = visible | coherent;
	properties.memoryTypes[2].propertyFlags = visible | cached;
	properties.memoryTypes[3].propertyFlags = visible | cached | coherent;
	const auto choose = [&](uint32_t bits, bool prefer_cached, uint32_t expected_index, VkMemoryPropertyFlags expected_flags) {
		uint32_t calls = 0, selected_index = UINT32_MAX;
		VkMemoryPropertyFlags selected_flags = 0;
		const bool allocated = allocate_readback_by_preference(properties, bits, prefer_cached,
		    [&](uint32_t index, VkMemoryPropertyFlags flags) {
			    ++calls;
			    selected_index = index;
			    selected_flags = flags;
			    return true;
		    });
		return allocated == (expected_index != UINT32_MAX) && calls == (allocated ? 1u : 0u) &&
		       selected_index == expected_index && selected_flags == expected_flags;
	};
	if (!choose(15, true, 3, visible | cached | coherent) || !choose(7, true, 2, visible | cached) ||
	    !choose(3, true, 1, visible | coherent) || !choose(15, false, 1, visible | coherent) ||
	    !choose(1, true, UINT32_MAX, 0) || !choose(0, true, UINT32_MAX, 0)) return false;
	const char* enabled_values[] = {nullptr, "", "1", "00", "false"};
	for (const char* value: enabled_values)
		if (!readback_cached_override(value, true) || readback_cached_override(value, false)) return false;
	if (readback_cached_override("0", true) || readback_cached_override("0", false)) return false;
	uint32_t calls = 0;
	bool first_preference_correct = false;
	const bool fallback = allocate_readback_by_preference(properties, 15, true, [&](uint32_t index, VkMemoryPropertyFlags flags) {
		++calls;
		if (calls == 1) first_preference_correct = index == 3 && flags == (visible | cached | coherent);
		return calls == 3 && index == 1 && flags == (visible | coherent);
	});
	if (!fallback || calls != 3 || !first_preference_correct) return false;
	calls = 0;
	bool cached_order_correct = true;
	if (!allocate_readback_by_preference(properties, 15, true, [&](uint32_t index, VkMemoryPropertyFlags flags) {
		    cached_order_correct = cached_order_correct &&
		        (calls == 0 ? index == 3 && flags == (visible | cached | coherent)
		                    : calls == 1 && index == 2 && flags == (visible | cached));
		    ++calls;
		    return index == 2;
	    }) || calls != 2 || !cached_order_correct) return false;
	calls = 0;
	if (allocate_readback_by_preference(properties, 4, true, [&](uint32_t, VkMemoryPropertyFlags) { ++calls; return false; }) ||
	    calls != 1) return false; // Cached-only type failed; no coherent fallback is compatible.
	calls = 0;
	if (allocate_readback_by_preference(properties, 1, true, [&](uint32_t, VkMemoryPropertyFlags) { ++calls; return true; }) ||
	    calls != 0) return false;
	calls = 0;
	if (!allocate_readback_by_preference(properties, 15, true, [&](uint32_t, VkMemoryPropertyFlags) { ++calls; return true; }) ||
	    calls != 1) return false;
	// Cached coherent memory can precede uncached coherent in the device list.
	// A failed preferred allocation must not hide the later compatible type.
	properties.memoryTypes[0].propertyFlags = visible | cached | coherent;
	properties.memoryTypes[3].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	calls = 0;
	bool order_correct = true;
	if (!allocate_readback_by_preference(properties, 3, true, [&](uint32_t index, VkMemoryPropertyFlags) {
		    order_correct = order_correct && index == calls;
		    ++calls;
		    return index == 1;
	    }) || calls != 2 || !order_correct) return false;
	calls = 0;
	if (allocate_readback_by_preference(properties, 3, true, [&](uint32_t, VkMemoryPropertyFlags) { ++calls; return false; }) ||
	    calls != 2) return false; // Failed types are attempted once, including the all-failed case.
	properties.memoryTypes[3].propertyFlags = visible | cached | coherent;
	constexpr uint32_t expected_order[] = {0, 3, 2, 1};
	calls = 0;
	order_correct = true;
	if (allocate_readback_by_preference(properties, 15, true, [&](uint32_t index, VkMemoryPropertyFlags) {
		    order_correct = order_correct && calls < 4 && index == expected_order[calls];
		    ++calls;
		    return false;
	    }) || calls != 4 || !order_correct) return false;
	properties.memoryTypeCount = 32;
	properties.memoryTypes[31].propertyFlags = visible | cached;
	if (!choose(uint32_t{1} << 31, true, 31, visible | cached)) return false;
	properties.memoryTypeCount = VK_MAX_MEMORY_TYPES + 1;
	if (!choose(UINT32_MAX, true, UINT32_MAX, 0)) return false;
	std::fprintf(stderr, "[GFX_READBACK_MEMORY_TEST] cached-coherent=pass cached-noncoherent=pass coherent-fallback=pass"
	                     " compatible-mask=pass allocation-failure=pass reversed-order-fallback=pass exact-zero-rollback=pass status=clean\n");
	return true;
}

void VulkanDeleteBuffer(GraphicContext* gctx, VulkanBuffer* buffer)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(buffer == nullptr);
	EXIT_IF(gctx == nullptr);

	DeleteDescriptor(buffer);

	vkDestroyBuffer(gctx->device, buffer->buffer, nullptr);
	VulkanFree(gctx, &buffer->memory);
	buffer->buffer = nullptr;
}

static void delete_upload_buffer(GraphicContext* ctx, void* obj)
{
	auto* buffer = static_cast<VulkanBuffer*>(obj);

	EXIT_IF(ctx == nullptr);
	EXIT_IF(buffer == nullptr);
	EXIT_IF(buffer->buffer == nullptr);

	vkDestroyBuffer(ctx->device, buffer->buffer, nullptr);
	VulkanFree(ctx, &buffer->memory);
	delete buffer;
}

namespace {
struct UploadSlab
{
	GraphicContext* owner = nullptr;
	VkDevice device = nullptr;
	uint32_t queue_family = UINT32_MAX;
	VulkanBuffer buffer;
	void* mapped = nullptr;
	UploadStagingCursor cursor {UploadStagingPoolPolicy::SlabBytes, 0};
	UploadStagingPoolPolicy::Lease lease;
};

struct UploadStagingPool
{
	std::mutex mutex;
	UploadStagingPoolPolicy policy;
	std::array<UploadSlab*, UploadStagingPoolPolicy::MaxSlabs> slabs {};
	uint64_t allocations = 0, reuses = 0, slices = 0, fallbacks = 0;
	void ReportLocked(const char* phase) const
	{
		std::fprintf(stderr, "[GFX_UPLOAD_STAGING] allocations=%" PRIu64 " reuses=%" PRIu64
		    " slices=%" PRIu64 " dedicated=%" PRIu64 " resident_bytes=%" PRIu64 " limit=%" PRIu64 " phase=%s\n",
		    allocations, reuses, slices, fallbacks, policy.Bytes(), UploadStagingPoolPolicy::MaxBytes, phase);
	}
};

// Explicitly drained with the live device; no Vulkan calls from static teardown.
UploadStagingPool& g_upload_staging = *new UploadStagingPool;

bool upload_staging_enabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_VULKAN_UPLOAD_STAGING_REUSE");
		return value == nullptr || std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

void delete_upload_slab(UploadSlab* slab)
{
	EXIT_IF(slab == nullptr || slab->owner == nullptr || slab->owner->device != slab->device);
	if (slab->mapped != nullptr) VulkanUnmapMemory(slab->owner, &slab->buffer.memory);
	if (slab->buffer.buffer != nullptr) vkDestroyBuffer(slab->device, slab->buffer.buffer, nullptr);
	if (slab->buffer.memory.memory != nullptr) VulkanFree(slab->owner, &slab->buffer.memory);
	delete slab;
}

UploadSlab* acquire_upload_slab(GraphicContext* ctx, uint32_t queue_family)
{
	{
		std::lock_guard<std::mutex> lock(g_upload_staging.mutex);
		for (uint32_t i = 0; i < UploadStagingPoolPolicy::MaxSlabs; ++i)
		{
			auto* slab = g_upload_staging.slabs[i];
			if (slab == nullptr || slab->owner != ctx || slab->device != ctx->device || slab->queue_family != queue_family ||
			    !g_upload_staging.policy.Available(i)) continue;
			slab->lease = g_upload_staging.policy.Acquire(i);
			EXIT_IF(!slab->lease.Valid());
			slab->cursor.used = 0;
			++g_upload_staging.reuses;
			return slab;
		}
		if (g_upload_staging.policy.Bytes() >= UploadStagingPoolPolicy::MaxBytes) return nullptr;
	}
	auto* slab = new UploadSlab;
	slab->owner = ctx;
	slab->device = ctx->device;
	slab->queue_family = queue_family;
	slab->buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	slab->buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VkBufferCreateInfo info {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = UploadStagingPoolPolicy::SlabBytes;
	info.usage = slab->buffer.usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(ctx->device, &info, nullptr, &slab->buffer.buffer) != VK_SUCCESS || slab->buffer.buffer == nullptr)
	{ delete_upload_slab(slab); return nullptr; }
	vkGetBufferMemoryRequirements(ctx->device, slab->buffer.buffer, &slab->buffer.memory.requirements);
	{
		std::lock_guard<std::mutex> lock(g_upload_staging.mutex);
		// Count the actual native allocation, including driver alignment, and
		// reserve before allocating/mapping. An active reservation blocks reset.
		slab->lease = g_upload_staging.policy.Reserve(slab->buffer.memory.requirements.size);
	}
	if (!slab->lease.Valid()) { delete_upload_slab(slab); return nullptr; }
	const bool allocated = VulkanTryAllocate(ctx, &slab->buffer.memory);
	const bool bound = allocated && vkBindBufferMemory(ctx->device, slab->buffer.buffer,
	    slab->buffer.memory.memory, slab->buffer.memory.offset) == VK_SUCCESS;
	const bool mapped = bound && vkMapMemory(ctx->device, slab->buffer.memory.memory, slab->buffer.memory.offset,
	    VK_WHOLE_SIZE, 0, &slab->mapped) == VK_SUCCESS && slab->mapped != nullptr;
	if (!mapped)
	{
		const auto lease = slab->lease;
		delete_upload_slab(slab);
		std::lock_guard<std::mutex> lock(g_upload_staging.mutex);
		EXIT_IF(!g_upload_staging.policy.CancelReservation(lease));
		return nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(g_upload_staging.mutex);
		EXIT_IF(g_upload_staging.slabs[slab->lease.slot] != nullptr || !g_upload_staging.policy.Busy(slab->lease));
		g_upload_staging.slabs[slab->lease.slot] = slab;
		++g_upload_staging.allocations;
	}
	return slab;
}

struct UploadSlice { VulkanBuffer* buffer; uint64_t offset; };
} // namespace

struct UploadRecordingBatch
{
	GraphicContext* owner = nullptr;
	VkDevice device = nullptr;
	uint32_t queue_family = UINT32_MAX;
	std::vector<UploadSlab*> slabs;
};

UploadRecordingBatch* UtilCreateUploadRecordingBatch(GraphicContext* ctx, uint32_t queue_family)
{
	EXIT_IF(ctx == nullptr || ctx->device == nullptr || queue_family == UINT32_MAX);
	return new UploadRecordingBatch {ctx, ctx->device, queue_family, {}};
}

void UtilReleaseUploadRecordingBatch(GraphicContext* ctx, UploadRecordingBatch* batch)
{
	EXIT_IF(ctx == nullptr || batch == nullptr || batch->owner != ctx || batch->device != ctx->device);
	{
		std::lock_guard<std::mutex> lock(g_upload_staging.mutex);
		for (auto* slab: batch->slabs)
		{
			EXIT_IF(slab->owner != ctx || slab->device != ctx->device || slab->queue_family != batch->queue_family ||
			        g_upload_staging.slabs[slab->lease.slot] != slab ||
			        !g_upload_staging.policy.Complete(slab->lease));
		}
	}
	delete batch;
}

bool UtilResetUploadStaging(GraphicContext* ctx)
{
	if (ctx == nullptr || ctx->device == nullptr) return false;
	std::vector<UploadSlab*> retired;
	{
		std::lock_guard<std::mutex> lock(g_upload_staging.mutex);
		if (!g_upload_staging.policy.CanReset()) return false;
		for (uint32_t i = 0; i < UploadStagingPoolPolicy::MaxSlabs; ++i)
		{
			auto* slab = g_upload_staging.slabs[i];
			if (slab == nullptr || slab->owner != ctx) continue;
			if (slab->device != ctx->device) return false;
		}
		for (uint32_t i = 0; i < UploadStagingPoolPolicy::MaxSlabs; ++i)
		{
			auto* slab = g_upload_staging.slabs[i];
			if (slab == nullptr || slab->owner != ctx) continue;
			EXIT_IF(!g_upload_staging.policy.RemoveAvailable(i));
			retired.push_back(std::exchange(g_upload_staging.slabs[i], nullptr));
		}
		if (g_upload_staging.allocations != 0 || g_upload_staging.fallbacks != 0) g_upload_staging.ReportLocked("reset");
	}
	for (auto* slab: retired) delete_upload_slab(slab);
	return true;
}

namespace {
bool try_copy_upload_batch(GraphicContext* ctx, UploadRecordingBatch* batch, const void* src, uint64_t bytes,
                           uint64_t alignment, UploadSlice* slice)
{
	if (ctx == nullptr || batch == nullptr || src == nullptr || slice == nullptr || bytes == 0 ||
	    bytes > UploadStagingPoolPolicy::SlabBytes || alignment == 0 || batch->owner != ctx || batch->device != ctx->device)
		return false;
	uint64_t offset = 0;
	auto* slab = batch->slabs.empty() ? nullptr : batch->slabs.back();
	bool allocated = slab != nullptr && slab->cursor.Allocate(bytes, alignment, &offset);
	if (!allocated && (slab = acquire_upload_slab(ctx, batch->queue_family)) != nullptr)
	{
		batch->slabs.push_back(slab);
		allocated = slab->cursor.Allocate(bytes, alignment, &offset);
		EXIT_IF(!allocated);
	}
	if (!allocated) return false;
	std::memcpy(static_cast<uint8_t*>(slab->mapped) + offset, src, static_cast<size_t>(bytes));
	{
		std::lock_guard<std::mutex> lock(g_upload_staging.mutex);
		++g_upload_staging.slices;
		if ((g_upload_staging.slices & (g_upload_staging.slices - 1)) == 0) g_upload_staging.ReportLocked("slice");
	}
	*slice = {&slab->buffer, offset};
	return true;
}

UploadSlice copy_upload_slice(GraphicContext* ctx, CommandBuffer* command, const void* src, uint64_t bytes,
                              uint64_t alignment)
{
	EXIT_IF(ctx == nullptr || command == nullptr || src == nullptr || bytes == 0 || bytes > SIZE_MAX);
	if (upload_staging_enabled() && bytes <= UploadStagingPoolPolicy::SlabBytes)
	{
		auto* batch = command->GetUploadRecordingBatch();
		EXIT_IF(batch == nullptr || batch->owner != ctx || batch->device != ctx->device);
		UploadSlice slice {};
		if (try_copy_upload_batch(ctx, batch, src, bytes, alignment, &slice)) return slice;
	}
	// Oversized transfers, full pool and native allocation/map failures retain
	// the existing dedicated allocation and its normal command retirement.
	auto* dedicated = new VulkanBuffer;
	dedicated->usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	dedicated->memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, bytes, dedicated);
	void* mapped = nullptr;
	const auto result = vkMapMemory(ctx->device, dedicated->memory.memory, dedicated->memory.offset,
	    VK_WHOLE_SIZE, 0, &mapped);
	EXIT_NOT_IMPLEMENTED(result != VK_SUCCESS || mapped == nullptr);
	std::memcpy(mapped, src, static_cast<size_t>(bytes));
	VulkanUnmapMemory(ctx, &dedicated->memory);
	command->Retire(dedicated, delete_upload_buffer);
	{
		std::lock_guard<std::mutex> lock(g_upload_staging.mutex);
		++g_upload_staging.fallbacks;
		if ((g_upload_staging.fallbacks & (g_upload_staging.fallbacks - 1)) == 0) g_upload_staging.ReportLocked("dedicated");
	}
	return {dedicated, 0};
}
} // namespace

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool UtilUploadStagingCopyForTest(GraphicContext* ctx, UploadRecordingBatch* batch, const void* src, uint64_t bytes,
                                  uint64_t alignment, VkBuffer* buffer, uint64_t* offset)
{
	if (buffer == nullptr || offset == nullptr) return false;
	UploadSlice slice {};
	if (!try_copy_upload_batch(ctx, batch, src, bytes, alignment, &slice)) return false;
	*buffer = slice.buffer->buffer;
	*offset = slice.offset;
	return true;
}
#endif

void UtilUploadVertexInputBuffer(GraphicContext* ctx, CommandBuffer* buffer, VulkanBuffer* dst_buffer,
                                 const void* src_data, uint64_t size)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());
	EXIT_IF(dst_buffer == nullptr || dst_buffer->buffer == nullptr);
	EXIT_IF(src_data == nullptr || size == 0);
	EXIT_IF((dst_buffer->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0);

	VkAccessFlags input_access = 0;
	if ((dst_buffer->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) != 0)
	{
		input_access |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
	}
	if ((dst_buffer->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0)
	{
		input_access |= VK_ACCESS_INDEX_READ_BIT;
	}
	EXIT_IF(input_access == 0);

	const auto staging = copy_upload_slice(ctx, buffer, src_data, size, 256);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();
	VkBufferCopy copy {};
	copy.srcOffset = staging.offset;
	copy.size = size;
	// The later queue submission publishes the coherent host writes. Keep the
	// GPU copy and its first vertex/index reads on this same command stream.
	vkCmdCopyBuffer(vk_buffer, staging.buffer->buffer, dst_buffer->buffer, 1, &copy);

	VkBufferMemoryBarrier input_barrier {};
	input_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	input_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	input_barrier.dstAccessMask       = input_access;
	input_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	input_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	input_barrier.buffer             = dst_buffer->buffer;
	input_barrier.offset             = 0;
	input_barrier.size               = size;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
	                     0, 0, nullptr, 1, &input_barrier, 0, nullptr);
}

static void get_image_layout_source(VkImageLayout layout, VkAccessFlags* access, VkPipelineStageFlags* stages)
{
	EXIT_IF(access == nullptr);
	EXIT_IF(stages == nullptr);

	switch (layout)
	{
		case VK_IMAGE_LAYOUT_UNDEFINED:
			*access = 0;
			*stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			break;
		case VK_IMAGE_LAYOUT_PREINITIALIZED:
			*access = VK_ACCESS_HOST_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_HOST_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			*access = VK_ACCESS_TRANSFER_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			*access = VK_ACCESS_TRANSFER_READ_BIT;
			*stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
			break;
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			*access = VK_ACCESS_SHADER_READ_BIT;
			*stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			*access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;
		case VK_IMAGE_LAYOUT_GENERAL:
			*access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		default:
			*access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			break;
	}
}

static void get_image_layout_destination(VkImageLayout layout, VkAccessFlags* access, VkPipelineStageFlags* stages)
{
	EXIT_IF(access == nullptr);
	EXIT_IF(stages == nullptr);

	switch (layout)
	{
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			*access = VK_ACCESS_SHADER_READ_BIT;
			*stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
			          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			*access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			break;
		case VK_IMAGE_LAYOUT_GENERAL:
			*access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
			break;
		default:
			*access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			*stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			break;
	}
}

void UtilUploadImage(GraphicContext* ctx, CommandBuffer* buffer, VulkanImage* dst_image, const void* src_data, uint64_t size,
                     uint32_t src_pitch, uint64_t dst_layout)
{
	EXIT_IF(dst_image == nullptr);

	Vector<BufferImageCopy> regions(1);
	regions[0].offset    = 0;
	regions[0].pitch     = src_pitch;
	regions[0].dst_level = 0;
	regions[0].width     = dst_image->extent.width;
	regions[0].height    = dst_image->extent.height;
	regions[0].dst_x     = 0;
	regions[0].dst_y     = 0;

	UtilUploadImage(ctx, buffer, dst_image, src_data, size, regions, dst_layout);
}

void UtilUploadImageForShaderStage(GraphicContext* ctx, CommandBuffer* buffer, VulkanImage* dst_image, const void* src_data,
                                   uint64_t size, uint32_t src_pitch, uint64_t dst_layout, uint64_t dst_stage)
{
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(dst_layout != static_cast<uint64_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
	EXIT_IF(dst_stage == 0);

	EXIT_IF(ctx == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());
	EXIT_IF(dst_image->image == nullptr);
	EXIT_IF(src_data == nullptr);

	// 768 is divisible by 4 and all core uncompressed/compressed texel
	// block byte sizes, including three-component formats.
	const auto staging = copy_upload_slice(ctx, buffer, src_data, size, 768);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	VkBufferMemoryBarrier staging_barrier {};
	staging_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	staging_barrier.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
	staging_barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
	staging_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	staging_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	staging_barrier.buffer               = staging.buffer->buffer;
	staging_barrier.offset               = staging.offset;
	staging_barrier.size                 = size;
	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
	                     &staging_barrier, 0, nullptr);

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT, dst_image->layout,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                 VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkBufferImageCopy copy {};
	copy.bufferOffset                = staging.offset;
	copy.bufferRowLength             = src_pitch != dst_image->extent.width ? src_pitch : 0;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent                 = {dst_image->extent.width, dst_image->extent.height, 1};
	vkCmdCopyBufferToImage(vk_buffer, staging.buffer->buffer, dst_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                 static_cast<VkPipelineStageFlags>(dst_stage));
}

void UtilUploadImage(GraphicContext* ctx, CommandBuffer* buffer, VulkanImage* dst_image, const void* src_data, uint64_t size,
                     const Vector<BufferImageCopy>& regions, uint64_t dst_layout)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(buffer == nullptr || buffer->IsInvalid());
	EXIT_IF(dst_image == nullptr || dst_image->image == nullptr);
	EXIT_IF(src_data == nullptr);
	EXIT_IF(regions.IsEmpty() || regions.Size() >= 16);

	// 768 is divisible by 4 and all core uncompressed/compressed texel
	// block byte sizes, including three-component formats.
	const auto staging = copy_upload_slice(ctx, buffer, src_data, size, 768);

	auto* vk_buffer = buffer->GetBufferOutsideRenderPass();

	VkBufferMemoryBarrier staging_barrier {};
	staging_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	staging_barrier.pNext               = nullptr;
	staging_barrier.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
	staging_barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
	staging_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	staging_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	staging_barrier.buffer               = staging.buffer->buffer;
	staging_barrier.offset               = staging.offset;
	staging_barrier.size                 = size;

	vkCmdPipelineBarrier(vk_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &staging_barrier, 0,
	                     nullptr);

	VkAccessFlags         source_access = 0;
	VkPipelineStageFlags source_stages = 0;
	get_image_layout_source(dst_image->layout, &source_access, &source_stages);

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT, dst_image->layout,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, source_access, VK_ACCESS_TRANSFER_WRITE_BIT, source_stages,
	                 VK_PIPELINE_STAGE_TRANSFER_BIT);

	VkBufferImageCopy copy_regions[16] = {};
	uint32_t          index            = 0;
	for (const auto& r: regions)
	{
		copy_regions[index].bufferOffset                    = staging.offset + r.offset;
		copy_regions[index].bufferRowLength                 = (r.width != r.pitch ? r.pitch : 0);
		copy_regions[index].bufferImageHeight               = 0;
		copy_regions[index].imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
		copy_regions[index].imageSubresource.mipLevel       = r.dst_level;
		copy_regions[index].imageSubresource.baseArrayLayer = 0;
		copy_regions[index].imageSubresource.layerCount     = 1;
		copy_regions[index].imageOffset                     = {r.dst_x, r.dst_y, 0};
		copy_regions[index].imageExtent                     = {r.width, r.height, 1};
		index++;
	}

	vkCmdCopyBufferToImage(vk_buffer, staging.buffer->buffer, dst_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, index, copy_regions);

	VkAccessFlags         destination_access = 0;
	VkPipelineStageFlags destination_stages = 0;
	get_image_layout_destination(static_cast<VkImageLayout>(dst_layout), &destination_access, &destination_stages);

	set_image_layout(vk_buffer, dst_image, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_ASPECT_COLOR_BIT,
	                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<VkImageLayout>(dst_layout), VK_ACCESS_TRANSFER_WRITE_BIT,
	                 destination_access, VK_PIPELINE_STAGE_TRANSFER_BIT, destination_stages);

}

void UtilFillImage(GraphicContext* ctx, VulkanImage* dst_image, const void* src_data, uint64_t size, uint32_t src_pitch,
                   uint64_t dst_layout)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(dst_image == nullptr);
	EXIT_IF(src_data == nullptr);

	VulkanBuffer staging_buffer {};
	staging_buffer.usage           = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	staging_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, size, &staging_buffer);

	void* data = nullptr;
	VulkanMapMemory(ctx, &staging_buffer.memory, &data);
	std::memcpy(data, src_data, size);
	VulkanUnmapMemory(ctx, &staging_buffer.memory);

	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();
	UtilBufferToImage(&buffer, &staging_buffer, src_pitch, dst_image, dst_layout);
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();

	VulkanDeleteBuffer(ctx, &staging_buffer);
}

bool UtilTryGetImageReadbackSpan(VkFormat format, uint32_t width, uint32_t height, uint32_t pitch,
                                uint64_t* row_bytes, uint64_t* span)
{
	uint64_t element_bytes = 0;
	switch (format)
	{
		case VK_FORMAT_R8_UNORM: element_bytes = 1; break;
		case VK_FORMAT_R8G8_UNORM: element_bytes = 2; break;
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB: element_bytes = 4; break;
		default: return false;
	}
	if (width == 0 || height == 0 || pitch < width || row_bytes == nullptr || span == nullptr) return false;
	const uint64_t row = static_cast<uint64_t>(width) * element_bytes;
	const uint64_t stride = static_cast<uint64_t>(pitch) * element_bytes;
	if (height - 1u > (UINT64_MAX - row) / stride) return false;
	*row_bytes = row;
	*span = static_cast<uint64_t>(height - 1u) * stride + row;
	return true;
}

bool UtilCopyReadbackRows(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                          VkFormat format, uint32_t width, uint32_t height, uint32_t pitch)
{
	uint64_t row = 0;
	uint64_t span = 0;
	if (dst == nullptr || src == nullptr || !UtilTryGetImageReadbackSpan(format, width, height, pitch, &row, &span) ||
	    span > dst_size || span > src_size) return false;
	const auto destination = reinterpret_cast<uintptr_t>(dst);
	const auto source = reinterpret_cast<uintptr_t>(src);
	if (span > UINTPTR_MAX - destination || span > UINTPTR_MAX - source ||
	    (destination <= source ? source - destination < span : destination - source < span)) return false;
	const uint64_t stride = static_cast<uint64_t>(pitch) * (row / width);
	for (uint32_t y = 0; y < height; ++y)
	{
		std::memcpy(static_cast<uint8_t*>(dst) + y * stride, static_cast<const uint8_t*>(src) + y * stride, row);
	}
	return true;
}

bool UtilCopyReadbackImage(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                           VkFormat format, uint32_t width, uint32_t height, uint32_t pitch,
                           ImageReadbackLayout layout, bool neo)
{
	return copy_readback_image(dst, dst_size, src, src_size, format, width, height, pitch, layout, neo);
}

static bool image_readback_profile_enabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_GFX_READBACK_PROFILE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

static void record_image_readback_profile(const std::chrono::steady_clock::time_point (&points)[8],
                                          uint64_t size, uint64_t texel_bytes, uint64_t allocation_bytes,
                                          uint32_t width, uint32_t height, uint32_t pitch, VkFormat format,
                                          VkMemoryPropertyFlags memory_flags, ImageReadbackLayout layout)
{
	struct Stats
	{
		uint64_t calls = 0, total_calls = 0, bytes = 0, texel_bytes = 0, allocation_bytes = 0, max_bytes = 0;
		uint64_t tiled = 0, noncoherent = 0, ns[7] {};
	};
	static thread_local Stats stats;
	++stats.calls;
	++stats.total_calls;
	stats.bytes += size;
	stats.texel_bytes += texel_bytes;
	stats.allocation_bytes += allocation_bytes;
	if (size > stats.max_bytes) stats.max_bytes = size;
	stats.tiled += layout == ImageReadbackLayout::Display2dThin ? 1u : 0u;
	stats.noncoherent += (memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0 ? 1u : 0u;
	for (int phase = 0; phase < 7; ++phase)
		stats.ns[phase] += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		    points[phase + 1] - points[phase]).count());
	if (stats.calls != 128) return;
	// Window sums are CPU wall time, including the real driver/fence waits.
	// No additional GPU synchronization or timestamps are introduced.
	std::fprintf(stderr,
	             "[GFX_READBACK_PROFILE] scope=image window_calls=%" PRIu64 " total_calls=%" PRIu64
	             " requested_bytes=%" PRIu64 " texel_bytes=%" PRIu64 " allocation_bytes=%" PRIu64
	             " max_bytes=%" PRIu64 " tiled=%" PRIu64 " noncoherent=%" PRIu64
	             " staging_ms=%.3f record_ms=%.3f submit_ms=%.3f fence_ms=%.3f map_invalidate_ms=%.3f"
	             " copy_retile_ms=%.3f unmap_ms=%.3f last_width=%u last_height=%u last_pitch=%u"
	             " last_format=%u last_memory_flags=0x%x timing=window-sum-cpu-wall\n",
	             stats.calls, stats.total_calls, stats.bytes, stats.texel_bytes, stats.allocation_bytes,
	             stats.max_bytes, stats.tiled, stats.noncoherent,
	             stats.ns[0] / 1.0e6, stats.ns[1] / 1.0e6, stats.ns[2] / 1.0e6, stats.ns[3] / 1.0e6,
	             stats.ns[4] / 1.0e6, stats.ns[5] / 1.0e6, stats.ns[6] / 1.0e6,
	             width, height, pitch, static_cast<unsigned>(format), static_cast<unsigned>(memory_flags));
	const uint64_t total_calls = stats.total_calls;
	stats = {};
	stats.total_calls = total_calls;
}

static void fill_buffer_on_queue(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch,
                                  VulkanImage* src_image, uint64_t src_layout, ImageReadbackLayout output_layout,
                                  bool neo, int queue)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_IF(ctx == nullptr);
	EXIT_IF(src_image == nullptr);
	EXIT_IF(dst_data == nullptr);
	EXIT_NOT_IMPLEMENTED(output_layout != ImageReadbackLayout::Linear &&
	                     output_layout != ImageReadbackLayout::Display2dThin &&
	                     output_layout != ImageReadbackLayout::Color2dThin);
	EXIT_NOT_IMPLEMENTED(output_layout == ImageReadbackLayout::Color2dThin && src_image->format != VK_FORMAT_R8G8B8A8_UNORM);
	EXIT_NOT_IMPLEMENTED(output_layout == ImageReadbackLayout::Display2dThin &&
	                     src_image->format != VK_FORMAT_R8G8B8A8_UNORM && src_image->format != VK_FORMAT_B8G8R8A8_UNORM &&
	                     src_image->format != VK_FORMAT_R8G8B8A8_SRGB && src_image->format != VK_FORMAT_B8G8R8A8_SRGB);
	uint64_t row_bytes = 0;
	uint64_t span = 0;
	EXIT_NOT_IMPLEMENTED(!UtilTryGetImageReadbackSpan(src_image->format, src_image->extent.width, src_image->extent.height,
	                                               dst_pitch, &row_bytes, &span) || span > size);
	const bool profile = image_readback_profile_enabled();
	std::chrono::steady_clock::time_point profile_points[8];
	if (profile) profile_points[0] = std::chrono::steady_clock::now();

	// Return the staging lease only after the utility command buffer has been
	// destroyed/reset below, not merely after its fence and the CPU read.
	ReadbackStagingLease staging(ctx, size);
	auto* staging_buffer = staging.Get();
	if (profile) profile_points[1] = std::chrono::steady_clock::now();

	CommandBuffer buffer(queue);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();
	UtilImageToBuffer(&buffer, src_image, staging_buffer, dst_pitch, src_layout);
	buffer.End();
	if (profile) profile_points[2] = std::chrono::steady_clock::now();
	buffer.Execute();
	if (profile) profile_points[3] = std::chrono::steady_clock::now();
	buffer.WaitForFence();
	if (profile) profile_points[4] = std::chrono::steady_clock::now();

	void* data = nullptr;
	// This staging memory is a dedicated allocation at offset zero. Mapping
	// and invalidating it in full satisfies nonCoherentAtomSize even when its
	// allocation size is not an atom multiple; no neighbouring memory is touched.
	EXIT_IF(staging_buffer->memory.offset != 0);
	EXIT_IF(vkMapMemory(ctx->device, staging_buffer->memory.memory, 0, VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS || data == nullptr);
	if ((staging_buffer->memory.property & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
	{
		VkMappedMemoryRange range {};
		range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = staging_buffer->memory.memory;
		range.size   = VK_WHOLE_SIZE;
		EXIT_IF(vkInvalidateMappedMemoryRanges(ctx->device, 1, &range) != VK_SUCCESS);
	}
	if (profile) profile_points[5] = std::chrono::steady_clock::now();
	// Image-to-buffer writes only logical texels. Padding and the unused tail
	// of the staging allocation are unspecified and must never replace guest bytes.
	EXIT_IF(!copy_readback_image(dst_data, size, data, size, src_image->format,
	                             src_image->extent.width, src_image->extent.height, dst_pitch, output_layout, neo));
	if (profile) profile_points[6] = std::chrono::steady_clock::now();
	VulkanUnmapMemory(ctx, &staging_buffer->memory);
	if (profile)
	{
		profile_points[7] = std::chrono::steady_clock::now();
		record_image_readback_profile(profile_points, size, row_bytes * src_image->extent.height,
		                              staging_buffer->memory.requirements.size, src_image->extent.width,
		                              src_image->extent.height, dst_pitch, src_image->format,
		                              staging_buffer->memory.property, output_layout);
	}
	if (output_layout == ImageReadbackLayout::Display2dThin)
	{
		static std::atomic_bool reported {false};
		if (!reported.exchange(true, std::memory_order_relaxed))
			std::fprintf(stderr, "[GFX_READBACK_DIRECT_TILE] source=mapped-staging destination=guest padding=preserved\n");
	}
}

void UtilFillBuffer(GraphicContext* ctx, void* dst_data, uint64_t size, uint32_t dst_pitch, VulkanImage* src_image,
                    uint64_t src_layout, ImageReadbackLayout output_layout, bool neo, int producer_queue)
{
	const int queue = producer_queue < 0 ? GraphicContext::QUEUE_UTIL : producer_queue;
	EXIT_IF(queue < 0 || queue >= GraphicContext::QUEUES_NUM);
	fill_buffer_on_queue(ctx, dst_data, size, dst_pitch, src_image, src_layout, output_layout, neo, queue);
}

namespace {
struct TextureMipsHostReadbackLayout
{
	std::array<VkBufferImageCopy, 15> copies {};
	uint32_t levels = 0;
	uint64_t bytes = 0;
};

bool texture_mips_host_readback_layout(const VulkanImage* image, uint32_t pitch, bool pow2_pad, uint64_t bytes,
                                       TextureMipsHostReadbackLayout* output)
{
	constexpr auto required_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	constexpr auto allowed_usage = required_usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (image == nullptr || output == nullptr || image->type != VulkanImageType::Texture ||
	    image->image == VK_NULL_HANDLE || image->image_view[VulkanImage::VIEW_DEFAULT] == VK_NULL_HANDLE ||
	    image->format != VK_FORMAT_R8G8B8A8_UNORM || image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
	    (image->usage & required_usage) != required_usage || (image->usage & ~allowed_usage) != 0 || image->flags != 0 ||
	    image->mip_levels < 2 || image->mip_levels > 15 || image->memory.memory == VK_NULL_HANDLE ||
	    image->memory.unique_id == 0 || image->memory.offset != 0 || image->memory.requirements.size == 0 ||
	    (image->memory.property & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0 ||
	    bytes == 0 || bytes > READBACK_STAGING_MAX_BYTES) return false;
	const auto width = image->extent.width, height = image->extent.height;
	TileSizeAlign proven {};
	TileSizeOffset proven_levels[16] {};
	// Gate the resolver before invoking the general table function: unsupported
	// descriptors must never reach its legacy guessed-linear fallback.
	if (!TileTryGetThin1dPow2PaddedTextureSize(10, 0, width, height, pitch, image->mip_levels, 13, pow2_pad,
	    &proven, proven_levels, nullptr) || proven.size != bytes) return false;
	TileSizeAlign actual {};
	TileSizeOffset actual_levels[16] {};
	TileGetTextureSize(10, 0, width, height, pitch, image->mip_levels, 13, false,
	                   &actual, actual_levels, nullptr, pow2_pad);
	if (actual.size != proven.size || actual.align != proven.align) return false;
	TextureMipsHostReadbackLayout result {};
	result.levels = image->mip_levels;
	result.bytes = bytes;
	uint64_t previous_end = 0, logical_bytes = 0;
	for (uint32_t level = 0; level < result.levels; ++level)
	{
		const auto& footprint = actual_levels[level];
		const uint32_t mip_width = std::max(width >> level, 1u);
		const uint32_t mip_height = std::max(height >> level, 1u);
		const uint32_t mip_pitch = std::max(pitch >> level, 1u);
		uint64_t row = 0, span = 0;
		if (footprint.offset != proven_levels[level].offset || footprint.size != proven_levels[level].size ||
		    footprint.offset != previous_end || footprint.offset % 4u != 0 || footprint.size == 0 ||
		    footprint.offset > bytes || footprint.size > bytes - footprint.offset ||
		    !UtilTryGetImageReadbackSpan(image->format, mip_width, mip_height, mip_pitch, &row, &span) ||
		    span > footprint.size) return false;
		previous_end = uint64_t(footprint.offset) + footprint.size;
		logical_bytes += row * mip_height;
		auto& copy = result.copies[level];
		copy.bufferOffset = footprint.offset;
		copy.bufferRowLength = mip_pitch;
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		copy.imageExtent = {mip_width, mip_height, 1};
	}
	if (previous_end != bytes || logical_bytes > image->memory.requirements.size) return false;
	*output = result;
	return true;
}

// The GPU leaves row padding, mip padding and the staging tail unspecified.
// Never export those bytes, even from an already completed cached allocation.
void copy_texture_mips_host_readback_rows(std::vector<uint8_t>* output, const void* mapped,
                                         const TextureMipsHostReadbackLayout& layout)
{
	std::vector<uint8_t> prepared(static_cast<size_t>(layout.bytes), 0);
	const auto* source = static_cast<const uint8_t*>(mapped);
	for (uint32_t level = 0; level < layout.levels; ++level)
	{
		const auto& copy = layout.copies[level];
		for (uint32_t y = 0; y < copy.imageExtent.height; ++y)
		{
			const uint64_t offset = copy.bufferOffset + uint64_t(y) * copy.bufferRowLength * 4u;
			std::memcpy(prepared.data() + offset, source + offset, uint64_t(copy.imageExtent.width) * 4u);
		}
	}
	output->swap(prepared);
}
} // namespace

bool UtilCanReadbackTextureMipsToHost(const VulkanImage* image, uint32_t pitch, bool pow2_pad, uint64_t bytes)
{
	TextureMipsHostReadbackLayout layout;
	return texture_mips_host_readback_layout(image, pitch, pow2_pad, bytes, &layout);
}

bool UtilReadbackTextureMipsToHost(GraphicContext* ctx, const VulkanImage* image, uint32_t pitch, bool pow2_pad,
                                  uint64_t bytes, std::vector<uint8_t>* output, bool admitted_handoff, int producer_queue)
{
	TextureMipsHostReadbackLayout layout;
	const int queue = producer_queue < 0 ? GraphicContext::QUEUE_GFX : producer_queue;
	if (ctx == nullptr || ctx->device == VK_NULL_HANDLE || output == nullptr ||
	    queue < 0 || queue >= GraphicContext::QUEUES_NUM ||
	    !texture_mips_host_readback_layout(image, pitch, pow2_pad, bytes, &layout)) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	if (!admitted_handoff && Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
#else
	(void)admitted_handoff;
#endif
	// Native lifetime is retained by the caller, independently of this metadata
	// copy. No tracker reference or guest address is consulted during the wait.
	const VulkanImage source = *image;
	ReadbackStagingLease staging(ctx, bytes);
	auto* staging_buffer = staging.Get();
	if (staging_buffer->memory.offset != 0) return false;
	// Destruction/reset of the command precedes the staging lease's return.
	CommandBuffer buffer(queue);
	if (buffer.IsInvalid()) return false;
	buffer.Begin();
	auto command = buffer.GetBufferOutsideRenderPass();
	record_image_layout(command, source.image, 0, layout.levels, VK_IMAGE_ASPECT_COLOR_BIT, source.layout,
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
	    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkCmdCopyImageToBuffer(command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buffer->buffer,
	                       layout.levels, layout.copies.data());
	VkBufferMemoryBarrier host {};
	host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = staging_buffer->buffer;
	host.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
	                     0, nullptr, 1, &host, 0, nullptr);
	record_image_layout(command, source.image, 0, layout.levels, VK_IMAGE_ASPECT_COLOR_BIT,
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout, VK_ACCESS_TRANSFER_READ_BIT,
	    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
	void* mapped = nullptr;
	if (vkMapMemory(ctx->device, staging_buffer->memory.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
	    mapped == nullptr) return false;
	if ((staging_buffer->memory.property & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0)
	{
		VkMappedMemoryRange range {};
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = staging_buffer->memory.memory;
		range.size = VK_WHOLE_SIZE;
		if (vkInvalidateMappedMemoryRanges(ctx->device, 1, &range) != VK_SUCCESS)
		{
			VulkanUnmapMemory(ctx, &staging_buffer->memory);
			return false;
		}
	}
	copy_texture_mips_host_readback_rows(output, mapped, layout);
	VulkanUnmapMemory(ctx, &staging_buffer->memory);
	return true;
}

static bool texture_mips_host_readback_contract_test()
{
	TextureVulkanImage image;
	image.image = reinterpret_cast<VkImage>(uintptr_t {1});
	image.image_view[VulkanImage::VIEW_DEFAULT] = reinterpret_cast<VkImageView>(uintptr_t {2});
	image.format = VK_FORMAT_R8G8B8A8_UNORM;
	image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	image.memory.memory = reinterpret_cast<VkDeviceMemory>(uintptr_t {3});
	image.memory.unique_id = 1;
	image.memory.property = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	for (const auto shape: {std::array<uint32_t, 4> {373, 83, 512, 9}, {19, 7, 32, 5}, {64, 32, 64, 7}})
	{
		const auto width = shape[0], height = shape[1], pitch = shape[2], levels = shape[3];
		image.extent = {width, height};
		image.mip_levels = levels;
		TileSizeAlign footprint {};
		if (!TileTryGetThin1dPow2PaddedTextureSize(10, 0, width, height, pitch, levels, 13, true,
		    &footprint, nullptr, nullptr)) return false;
		image.memory.requirements.size = footprint.size;
		TextureMipsHostReadbackLayout layout;
		if (!texture_mips_host_readback_layout(&image, pitch, true, footprint.size, &layout) ||
		    !UtilCanReadbackTextureMipsToHost(&image, pitch, true, footprint.size) || layout.levels != levels ||
		    layout.bytes != footprint.size) return false;
		if (width == 373)
		{
			constexpr uint32_t offsets[] = {0, 262144, 327680, 344064, 348160, 349184, 349696, 349952, 350208};
			if (layout.bytes != 350464) return false;
			for (uint32_t level = 0; level < levels; ++level)
				if (layout.copies[level].bufferOffset != offsets[level]) return false;
		}
		std::vector<uint8_t> mapped(footprint.size, 0xdd), expected(footprint.size, 0), exported {1, 2, 3};
		for (uint32_t level = 0; level < levels; ++level)
		{
			const auto& copy = layout.copies[level];
			if (copy.bufferRowLength != std::max(pitch >> level, 1u) || copy.bufferImageHeight != 0 ||
			    copy.imageExtent.width != std::max(width >> level, 1u) ||
			    copy.imageExtent.height != std::max(height >> level, 1u) || copy.imageExtent.depth != 1 ||
			    copy.imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
			    copy.imageSubresource.mipLevel != level || copy.imageSubresource.baseArrayLayer != 0 ||
			    copy.imageSubresource.layerCount != 1 || copy.imageOffset.x != 0 || copy.imageOffset.y != 0 ||
			    copy.imageOffset.z != 0) return false;
			for (uint32_t y = 0; y < copy.imageExtent.height; ++y)
			for (uint32_t x = 0; x < copy.imageExtent.width * 4u; ++x)
			{
				const uint64_t offset = copy.bufferOffset + uint64_t(y) * copy.bufferRowLength * 4u + x;
				mapped[offset] = expected[offset] = static_cast<uint8_t>((level * 17u + y * 11u + x + 3u) % 251u);
			}
		}
		copy_texture_mips_host_readback_rows(&exported, mapped.data(), layout);
		if (exported != expected) return false; // Every unspecified staging byte was excluded.
		std::vector<uint8_t> tiled(footprint.size, 0xa5), recovered(footprint.size, 0);
		TileConvertLinearToTiled(tiled.data(), exported.data(), TileMode::TextureTiled, 10, 0,
		    width, height, pitch, levels, false, 13, footprint.size, true);
		TileConvertTiledToLinear(recovered.data(), tiled.data(), TileMode::TextureTiled, 10, 0,
		    width, height, pitch, levels, false, 13, footprint.size, true);
		if (recovered != expected) return false;
		for (uint32_t fault = 0; fault < 25; ++fault)
		{
			auto bad = image;
			uint32_t requested_pitch = pitch;
			uint64_t requested_bytes = footprint.size;
			bool pow2_pad = true;
			switch (fault)
			{
				case 0: bad.type = VulkanImageType::StorageTexture; break;
				case 1: bad.image = VK_NULL_HANDLE; break;
				case 2: bad.image_view[VulkanImage::VIEW_DEFAULT] = VK_NULL_HANDLE; break;
				case 3: bad.format = VK_FORMAT_R8G8B8A8_SRGB; break;
				case 4: bad.layout = VK_IMAGE_LAYOUT_GENERAL; break;
				case 5: bad.usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT; break;
				case 6: bad.usage &= ~VK_IMAGE_USAGE_SAMPLED_BIT; break;
				case 7: bad.usage |= VK_IMAGE_USAGE_STORAGE_BIT; break;
				case 8: bad.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
				case 9: bad.mip_levels = 1; break;
				case 10: bad.mip_levels = 16; break;
				case 11: bad.memory.memory = VK_NULL_HANDLE; break;
				case 12: bad.memory.unique_id = 0; break;
				case 13: bad.memory.offset = 4; break;
				case 14: bad.memory.requirements.size = 1; break;
				case 15: bad.memory.property = 0; break;
				case 16: bad.extent.width = 0; break;
				case 17: requested_pitch++; break;
				case 18: pow2_pad = false; break;
				case 19: requested_bytes--; break;
				case 20: requested_bytes++; break;
				case 21: bad.extent.height = 0; break;
				case 22: bad.extent.width = UINT32_MAX; break;
				case 23: bad.extent.height = UINT32_MAX; break;
				case 24: requested_pitch = UINT32_MAX; break;
			}
			if (UtilCanReadbackTextureMipsToHost(&bad, requested_pitch, pow2_pad, requested_bytes)) return false;
		}
		for (const auto bytes: {UINT64_C(0), READBACK_STAGING_MAX_BYTES + 1, UINT64_MAX})
			if (UtilCanReadbackTextureMipsToHost(&image, pitch, true, bytes)) return false;
		const auto retained = exported;
		if (UtilReadbackTextureMipsToHost(nullptr, &image, pitch, true, footprint.size, &exported) ||
		    exported != retained || UtilCanReadbackTextureMipsToHost(nullptr, pitch, true, footprint.size)) return false;
	}
	std::fprintf(stderr, "[GFX_TEXTURE_MIPS_HOST_READBACK_TEST] layouts=3 mip-offsets/pitches=checked"
	    " all-mip-retile-roundtrip=pass staging-padding=excluded invalid-native/layout/capacity=reject"
	    " failure-output=unchanged guest-access=none gpu-execution=not-tested\n");
	return true;
}

template <typename Acquire = GraphicsGuestLeaseAcquire>
static bool borrow_videoout_destination(uint64_t address, uint64_t size, uint64_t expected_generation,
                                       Core::VirtualMemory::GuestDataLease* lease, bool require_read,
                                       GraphicsGuestLeaseResult* result, Acquire acquire = {})
{
	if (result != nullptr) *result = {};
	if (size > READBACK_STAGING_MAX_BYTES)
	{
		if (result != nullptr) result->reason = "destination-shape";
		return false;
	}
	const auto mode = require_read ? Core::VirtualMemory::Mode::ReadWrite : Core::VirtualMemory::Mode::Write;
	if (expected_generation == UINT64_MAX)
		return TryAcquireGraphicsGuestCaptureLease(address, size, mode, lease, result, acquire);
	return TryAcquireGraphicsGuestContinuityLease(address, size, mode, expected_generation, lease, result, acquire);
}

enum class GuestImagePublicationProfile { VideoOut, Color2dThinRenderTexture };

template <typename Acquire = GraphicsGuestLeaseAcquire>
static bool can_readback_image_to_guest(GraphicContext* ctx, VulkanImage* image, uint64_t address, uint64_t size,
                                         uint32_t pitch, ImageReadbackLayout layout, bool neo, uint64_t* generation,
                                         const char** rejection, uint64_t expected_generation = UINT64_MAX,
                                         GraphicsGuestLeaseResult* result = nullptr, Acquire acquire = {},
                                         GuestImagePublicationProfile profile = GuestImagePublicationProfile::VideoOut,
                                         bool snapshot_pair = false)
{
	if (result != nullptr) *result = {};
	const auto reject = [rejection, result](const char* reason) {
		if (rejection != nullptr) *rejection = reason;
		if (result != nullptr) result->reason = reason;
		return false;
	};
	if (generation == nullptr) return reject("generation-output");
	*generation = UINT64_MAX;
	const bool color = profile == GuestImagePublicationProfile::Color2dThinRenderTexture;
	if (color && expected_generation == UINT64_MAX) return reject("mapping-generation-unavailable");
	const bool shader_read_layout = color && snapshot_pair && image != nullptr &&
	                                image->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
	                                (image->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
	if (ctx == nullptr || image == nullptr || image->image == VK_NULL_HANDLE ||
	    image->type != (color ? VulkanImageType::RenderTexture : VulkanImageType::VideoOut) ||
	    image->mip_levels != 1 || (image->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && !shader_read_layout) ||
	    (image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
	    (color ? image->format != VK_FORMAT_R8G8B8A8_UNORM :
	        (image->format != VK_FORMAT_R8G8B8A8_SRGB && image->format != VK_FORMAT_B8G8R8A8_SRGB)))
		return reject("image-contract");
	uint64_t row = 0, span = 0;
	if (!UtilTryGetImageReadbackSpan(image->format, image->extent.width, image->extent.height, pitch, &row, &span) ||
	    span > size) return reject("linear-footprint");
	if (color)
	{
		TileSizeAlign tile {};
		if (layout != ImageReadbackLayout::Color2dThin ||
		    !TileTryGetColor2dThinTextureSize(10, 0, image->extent.width, image->extent.height, pitch, 1, 14, neo, false,
		                                    &tile, nullptr, nullptr) || tile.size != size)
			return reject("color-tiled-footprint");
	} else if (layout == ImageReadbackLayout::Display2dThin)
	{
		TileSizeAlign tile {};
		if (!TileTryGetDisplay2dThinTextureSize(10, 0, image->extent.width, image->extent.height, pitch, 1, 10, neo, false,
		                                     &tile, nullptr, nullptr) || tile.size > size) return reject("tiled-footprint");
	} else if (layout != ImageReadbackLayout::Linear) return reject("layout");
	Core::VirtualMemory::GuestDataLease lease;
	GraphicsGuestLeaseResult local_result;
	if (!borrow_videoout_destination(address, size, expected_generation, &lease, color, &local_result, acquire))
	{
		if (result != nullptr) *result = local_result;
		if (rejection != nullptr) *rejection = local_result.reason;
		return false;
	}
	if (result != nullptr) *result = local_result;
	// A new capture starts at its successful lease. Revalidation retains the
	// original token even when unrelated VM activity advances the global count.
	*generation = expected_generation == UINT64_MAX ? lease.MutationGeneration() : expected_generation;
	return true;
}

bool UtilCanReadbackVideoOutToGuest(GraphicContext* ctx, VideoOutVulkanImage* image, uint64_t address, uint64_t size,
                                  uint32_t pitch, ImageReadbackLayout layout, bool neo, uint64_t* generation,
                                  uint64_t expected_generation, GraphicsGuestLeaseResult* output)
{
	const char* reason = "unknown";
	GraphicsGuestLeaseResult result;
	const bool supported = can_readback_image_to_guest(ctx, image, address, size, pitch, layout, neo, generation,
	                                                    &reason, expected_generation, &result);
	if (output != nullptr) *output = result;
	if (!supported)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		// The helper released its lease, but a nested caller may still own one.
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
#endif
		static std::atomic<uint32_t> records {0};
		if (records.fetch_add(1, std::memory_order_relaxed) < 32)
			std::fprintf(stderr, "[GFX_VIDEOOUT_PREFLIGHT_BLOCK] reason=%s address=0x%016" PRIx64
			                     " bytes=%" PRIu64 " current_generation=%" PRIu64 " lease_attempts=%u\n",
			             reason, address, size, Core::VirtualMemory::GetMutationGeneration(), result.attempts);
	}
	return supported;
}

struct VideoOutPublicationFailure
{
	const char* phase = "preflight";
	const char* reason = "unknown";
	uint64_t observed_generation = UINT64_MAX;
	uint32_t attempts = 0;
};

struct PreparedImageReadback
{
	enum class Phase { Preflight, HostPrepared, Published };
	// Host-owned metadata/staging survive a qualified EOP/render-full CPU pause.
	// No object/vector reference or newly captured mapping token is retained.
	VulkanImage image;
	uint64_t original_generation;
	std::vector<uint8_t> bytes;
	Phase phase = Phase::Preflight;
};

template <typename Proof>
static EopCpuProofResult continue_videoout_cpu_proof(const VideoOutReadbackContinuation* continuation,
                                                    const char* phase, Proof&& proof)
{
	if (continuation == nullptr || continuation->resume == nullptr) return proof();
	using ProofType = std::remove_reference_t<Proof>;
	return continuation->resume(continuation->context, phase, [](void* context) {
		return (*static_cast<ProofType*>(context))();
	}, &proof);
}

static EopCpuProofResult videoout_cpu_acquisition_result(bool complete, const GraphicsGuestLeaseResult& result)
{
	return {complete, !complete && result.reason != nullptr && result.attempts == 16 &&
	                      std::strcmp(result.reason, "lease-acquisition-retry-exhausted") == 0, result.reason};
}

template <typename Read, typename Acquire = GraphicsGuestLeaseAcquire>
static bool publish_videoout_readback(GraphicContext* ctx, VulkanImage* image, uint64_t address, uint64_t size,
                                     uint32_t pitch, ImageReadbackLayout layout, bool neo, uint64_t expected_generation,
                                     Read&& read, uint64_t* published_hash = nullptr,
	                                     VideoOutPublicationFailure* failure = nullptr, Acquire acquire = {},
	                                     const VideoOutReadbackContinuation* continuation = nullptr,
                                     GuestImagePublicationProfile profile = GuestImagePublicationProfile::VideoOut,
                                     bool snapshot_pair = false)
{
	const auto reject = [failure](const char* phase, const char* reason, uint64_t observed_generation, uint32_t attempts = 0) {
		if (failure != nullptr) *failure = {phase, reason, observed_generation, attempts};
		return false;
	};
	if (published_hash != nullptr) *published_hash = 0;
	if (expected_generation == UINT64_MAX) return reject("preflight", "mapping-generation-unavailable", UINT64_MAX);
	if (image == nullptr) return reject("preflight", "image-contract", UINT64_MAX);
	PreparedImageReadback prepared {*image, expected_generation, {}};
	uint64_t current_generation = UINT64_MAX;
	const char* reason = "unknown";
	GraphicsGuestLeaseResult result;
	const auto entry = continue_videoout_cpu_proof(continuation, "preflight", [&] {
		if (prepared.phase != PreparedImageReadback::Phase::Preflight)
			return EopCpuProofResult {false, false, "prepared-phase-changed"};
		const bool complete = can_readback_image_to_guest(ctx, &prepared.image, address, size, pitch, layout, neo,
		    &current_generation, &reason, prepared.original_generation, &result, acquire, profile, snapshot_pair);
		return videoout_cpu_acquisition_result(complete, result);
	});
	if (!entry.complete) return reject("preflight", entry.reason, result.observed_generation, result.attempts);
	if (current_generation != expected_generation) return reject("preflight", "generation-changed", current_generation);
	// Only host-owned storage is reachable during the GPU wait. Ordinary guest
	// stores are not excluded by a mapping lease; publication changes texels only.
	prepared.bytes.resize(static_cast<size_t>(size));
	if (profile == GuestImagePublicationProfile::Color2dThinRenderTexture && image->layout != prepared.image.layout)
		return reject("readback", "color-original-layout-changed", UINT64_MAX);
	if (!read(prepared.bytes.data())) return reject("readback", "gpu-readback", UINT64_MAX);
	if (profile == GuestImagePublicationProfile::Color2dThinRenderTexture && image->layout != prepared.image.layout)
		return reject("readback", "color-original-layout-not-restored", UINT64_MAX);
	prepared.phase = PreparedImageReadback::Phase::HostPrepared;
	Core::VirtualMemory::GuestDataLease lease;
	const auto publication = continue_videoout_cpu_proof(continuation, "publication", [&] {
		if (prepared.phase != PreparedImageReadback::Phase::HostPrepared)
			return EopCpuProofResult {false, false, "prepared-phase-changed"};
		if (profile == GuestImagePublicationProfile::Color2dThinRenderTexture && image->layout != prepared.image.layout)
			return EopCpuProofResult {false, false, "color-original-layout-changed"};
		const bool complete = borrow_videoout_destination(address, size, prepared.original_generation, &lease,
		    published_hash != nullptr, &result, acquire);
		return videoout_cpu_acquisition_result(complete, result);
	});
	if (!publication.complete) return reject("publication", publication.reason, result.observed_generation, result.attempts);
	// The original mapping token was proved under this exact destination lease.
	// Retain the lease through both texel publication and the full-block hash.
	if (!copy_readback_image(reinterpret_cast<void*>(address), size, prepared.bytes.data(), size, prepared.image.format,
	                         prepared.image.extent.width, prepared.image.extent.height, pitch, layout, neo))
		return reject("publication", "copy-profile", lease.MutationGeneration());
	// The full allocation includes preserved padding. Keep its hash in the same
	// mapping lease as publication; a second nonblocking borrow can fail after
	// the real export has already completed because a VM writer is now pending.
	if (published_hash != nullptr)
		*published_hash = XXH3_64bits(reinterpret_cast<const void*>(address), static_cast<size_t>(size));
	prepared.phase = PreparedImageReadback::Phase::Published;
	return true;
}


bool UtilCanReadbackRenderTextureToGuest(GraphicContext* ctx, RenderTextureVulkanImage* image, uint64_t address, uint64_t size,
                                       uint32_t pitch, bool neo, uint64_t* generation, uint64_t expected_generation,
                                       GraphicsGuestLeaseResult* output, bool snapshot_pair)
{
	const char* reason = "unknown";
	GraphicsGuestLeaseResult result;
	const bool supported = can_readback_image_to_guest(ctx, image, address, size, pitch, ImageReadbackLayout::Color2dThin,
	    neo, generation, &reason, expected_generation, &result, GraphicsGuestLeaseAcquire {},
	    GuestImagePublicationProfile::Color2dThinRenderTexture, snapshot_pair);
	if (output != nullptr) *output = result;
	return supported;
}

bool UtilReadbackRenderTextureToGuest(GraphicContext* ctx, RenderTextureVulkanImage* image, uint64_t address, uint64_t size,
                                    uint32_t pitch, bool neo, uint64_t expected_generation, uint64_t* published_hash,
                                    const VideoOutReadbackContinuation* continuation, bool snapshot_pair)
{
	VideoOutPublicationFailure failure;
	const auto original_layout = image != nullptr ? image->layout : VK_IMAGE_LAYOUT_UNDEFINED;
	const bool published = publish_videoout_readback(ctx, image, address, size, pitch, ImageReadbackLayout::Color2dThin,
	    neo, expected_generation, [&](void* snapshot) {
		    // Queue ordering and layout match the completed color producer. All VM
		    // leases ended before the once-only transfer/fence; retile is CPU only.
		    fill_buffer_on_queue(ctx, snapshot, size, pitch, image, original_layout,
		                         ImageReadbackLayout::Linear, false, GraphicContext::QUEUE_GFX);
		    return true;
	    }, published_hash, &failure, GraphicsGuestLeaseAcquire {}, continuation,
	    GuestImagePublicationProfile::Color2dThinRenderTexture, snapshot_pair);
	if (!published)
	{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (Core::VirtualMemory::CurrentThreadHasMemoryLease()) return false;
#endif
		std::fprintf(stderr, "[GFX_EOP_COLOR_PUBLICATION_BLOCK] phase=%s reason=%s bytes=%" PRIu64
		                     " original_generation=%" PRIu64 " observed_generation=%" PRIu64
		                     " lease_attempts=%u gpu_replays=0 callback_replays=0\n",
		    failure.phase, failure.reason, size, expected_generation, failure.observed_generation, failure.attempts);
	}
	return published;
}

#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool UtilRenderTexturePublicationContractTest(GraphicContext* ctx, RenderTextureVulkanImage* image, uint64_t address,
    uint64_t size, uint32_t pitch, bool neo, uint64_t original_generation, uint64_t* published_hash,
    bool (*read)(void* context, void* staging, uint64_t size), void* read_context, bool snapshot_pair)
{
	if (read == nullptr) { if (published_hash != nullptr) *published_hash = 0; return false; }
	return publish_videoout_readback(ctx, image, address, size, pitch, ImageReadbackLayout::Color2dThin, neo,
	    original_generation, [&](void* staging) { return read(read_context, staging, size); }, published_hash,
	    nullptr, GraphicsGuestLeaseAcquire {}, nullptr, GuestImagePublicationProfile::Color2dThinRenderTexture, snapshot_pair);
}

bool UtilVideoOutPreparedRenderFullContractTest(GraphicContext* ctx, VideoOutVulkanImage* image,
    uint64_t address, uint64_t size, uint64_t original_generation, VideoOutPreparedContractFault fault, bool exhaust,
    const VideoOutReadbackContinuation* continuation, VideoOutPreparedContractObservation* observation)
{
	using namespace Core::VirtualMemory;
	if (observation == nullptr || image == nullptr || size != 65536) return false;
	*observation = {};
	bool valid = true;
	const auto acquire = [&](uint64_t begin, uint64_t bytes, Mode mode, GuestDataLease* output) {
		const bool entry = observation->reads == 0;
		auto& calls = entry ? observation->entry_acquisitions : observation->publication_acquisitions;
		++calls;
		valid = valid && begin == address && bytes == size && mode == (entry ? Mode::Write : Mode::ReadWrite);
		const bool selected = fault == VideoOutPreparedContractFault::Both ||
		    (entry ? fault == VideoOutPreparedContractFault::Entry : fault == VideoOutPreparedContractFault::Publication);
		if (selected && (calls <= 16 || exhaust)) return false;
		return TryAcquireGuestDataLease(begin, bytes, mode, output, false);
	};
	VideoOutPublicationFailure failure;
	observation->published = publish_videoout_readback(ctx, image, address, size, 128, ImageReadbackLayout::Linear, false,
	    original_generation, [&](void* snapshot) {
		    ++observation->reads;
		    valid = valid && observation->reads == 1 && !CurrentThreadHasMemoryLease();
		    std::memset(snapshot, 0x32, static_cast<size_t>(size));
		    return true;
	    }, &observation->published_hash, &failure, acquire, continuation);
	observation->failure_phase = failure.phase;
	observation->reason = failure.reason;
	valid = valid && !CurrentThreadHasMemoryLease() && observation->reads <= 1;
	// Rejecting the prepared publication leaves every guest byte intact. A
	// successful publication modifies only active texels and hashes preserved
	// padding within the single destination lease used by the production writer.
	std::vector<uint8_t> expected(static_cast<size_t>(size), 0xa5);
	if (observation->published)
		for (uint32_t row = 0; row < 9; ++row) std::fill_n(expected.data() + row * 512, 52, uint8_t {0x32});
	valid = valid && std::memcmp(reinterpret_cast<const void*>(address), expected.data(), static_cast<size_t>(size)) == 0;
	if (observation->published)
		valid = valid && observation->reads == 1 && observation->published_hash == XXH3_64bits(expected.data(), expected.size());
	else valid = valid && observation->published_hash == 0;
	return valid;
}
#endif

bool UtilReadbackVideoOutToGuest(GraphicContext* ctx, VideoOutVulkanImage* image, uint64_t address, uint64_t size,
                               uint32_t pitch, ImageReadbackLayout layout, bool neo, uint64_t expected_generation,
	                               uint64_t* published_hash, const VideoOutReadbackContinuation* continuation)
{
	VideoOutPublicationFailure failure;
	const bool published = publish_videoout_readback(ctx, image, address, size, pitch, layout, neo, expected_generation, [&](void* snapshot)
	{
		// Window's blit uses the same queue and canonical layout. A separate UTIL
		// queue would allow concurrent image transitions even after the CP fence.
		fill_buffer_on_queue(ctx, snapshot, size, pitch, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		                     ImageReadbackLayout::Linear, false, GraphicContext::QUEUE_GFX);
		return true;
	}, published_hash, &failure, GraphicsGuestLeaseAcquire {}, continuation);
	if (!published)
	{
		// The helper's leases are gone. Report saved metadata only; never query
		// guest bytes or a subsystem while diagnosing a refused publication.
		static std::atomic<uint32_t> records {0};
		if (records.fetch_add(1, std::memory_order_relaxed) < 32)
			std::fprintf(stderr, "[GFX_VIDEOOUT_PUBLICATION_BLOCK] phase=%s reason=%s"
			                     " address=0x%016" PRIx64 " bytes=%" PRIu64 " expected_generation=%" PRIu64
			                     " observed_generation=%" PRIu64 " current_generation=%" PRIu64 " lease_attempts=%u\n",
			             failure.phase, failure.reason, address, size, expected_generation,
			             failure.observed_generation, Core::VirtualMemory::GetMutationGeneration(), failure.attempts);
	}
	return published;
}

static bool color_publication_contract_test()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	constexpr uint64_t allocation_bytes = 131072, offset = 128;
	const uint64_t allocation = Alloc(0, allocation_bytes, Mode::ReadWrite);
	if (allocation == 0) return false;
	struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {allocation};
	const uint64_t address = allocation + offset;
	GraphicContext ctx {};
	for (const bool neo: {false, true})
	{
		TileSizeAlign tile {};
		if (!TileTryGetColor2dThinTextureSize(10, 0, 13, 9, 128, 1, 14, neo, false, &tile, nullptr, nullptr)) return false;
		RenderTextureVulkanImage image {};
		image.image = reinterpret_cast<VkImage>(uintptr_t {1}); // Native completion/CP enrollment modeled here.
		image.format = VK_FORMAT_R8G8B8A8_UNORM; image.extent = {13, 9}; image.mip_levels = 1;
		image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		image.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		std::vector<uint8_t> linear(tile.size, 0xd7), expected(allocation_bytes, 0xa5);
		for (uint32_t y = 0; y < 9; ++y)
			for (uint32_t x = 0; x < 13; ++x)
				for (uint32_t channel = 0; channel < 4; ++channel)
					linear[(y * 128 + x) * 4 + channel] = static_cast<uint8_t>(x * 11 + y * 17 + channel * 31);
		if (!copy_readback_image(expected.data() + offset, tile.size, linear.data(), tile.size, image.format,
		    13, 9, 128, ImageReadbackLayout::Color2dThin, neo)) return false;
		uint32_t borrows = 0;
		const auto acquire = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* output) {
			++borrows;
			return TryAcquireGuestDataLease(begin, size, mode, output, false);
		};
		for (const bool snapshot_pair: {false, true})
		for (uint32_t negative = 0; negative < 9; ++negative)
		{
			auto bad = image; uint64_t generation = 17;
			uint64_t size = tile.size, token = GetMutationGeneration(); uint32_t pitch = 128;
			switch (negative)
			{
				case 0: token = UINT64_MAX; break;
				case 1: bad.type = VulkanImageType::VideoOut; break;
				case 2: bad.format = VK_FORMAT_R8G8B8A8_SRGB; break;
				case 3: bad.mip_levels = 2; break;
				case 4: bad.layout = VK_IMAGE_LAYOUT_GENERAL; break;
				case 5: bad.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; break;
				case 6: --size; break;
				case 7: ++size; break;
				case 8: --pitch; break;
			}
			const char* reason = nullptr; GraphicsGuestLeaseResult result; const uint32_t before = borrows;
			if (can_readback_image_to_guest(&ctx, &bad, address, size, pitch, ImageReadbackLayout::Color2dThin,
			    neo, &generation, &reason, token, &result, acquire, GuestImagePublicationProfile::Color2dThinRenderTexture, snapshot_pair) ||
			    generation != UINT64_MAX || borrows != before || reason == nullptr) return false;
		}
		uint64_t generation = 17; GraphicsGuestLeaseResult result;
		if (UtilCanReadbackRenderTextureToGuest(&ctx, &image, address, tile.size, 128, neo, &generation, UINT64_MAX, &result) ||
		    generation != UINT64_MAX || result.attempts != 0 || result.reason == nullptr ||
		    std::strcmp(result.reason, "mapping-generation-unavailable") != 0) return false;
		if (UtilCanReadbackRenderTextureToGuest(&ctx, &image, address, tile.size, 128, neo, nullptr, GetMutationGeneration(), &result) ||
		    result.attempts != 0) return false;
		for (const auto layout: {VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_UNDEFINED,
		                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL})
		{
			auto bad = image; bad.layout = layout;
			for (const bool snapshot_pair: {false, true})
			{
				const uint32_t before = borrows; generation = 17; const char* reason = nullptr;
				const bool supported = can_readback_image_to_guest(&ctx, &bad, address, tile.size, 128,
				    ImageReadbackLayout::Color2dThin, neo, &generation, &reason, GetMutationGeneration(), &result, acquire,
				    GuestImagePublicationProfile::Color2dThinRenderTexture, snapshot_pair);
				const bool expected_support = snapshot_pair && layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				if (supported != expected_support || borrows != before + uint32_t(expected_support) || CurrentThreadHasMemoryLease() ||
				    (expected_support ? generation == UINT64_MAX : generation != UINT64_MAX || reason == nullptr)) return false;
			}
		}
		// The extra layout authority belongs only to RenderTexture. Even the
		// internal flag cannot admit a sampled-layout VideoOut publication.
		{
			auto unsampleable = image; unsampleable.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			unsampleable.usage &= ~VK_IMAGE_USAGE_SAMPLED_BIT;
			const uint32_t before = borrows; generation = 17; const char* reason = nullptr;
			if (can_readback_image_to_guest(&ctx, &unsampleable, address, tile.size, 128, ImageReadbackLayout::Color2dThin,
			    neo, &generation, &reason, GetMutationGeneration(), &result, acquire,
			    GuestImagePublicationProfile::Color2dThinRenderTexture, true) || generation != UINT64_MAX || borrows != before) return false;
			auto bad = image; bad.type = VulkanImageType::VideoOut; bad.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			bad.format = VK_FORMAT_R8G8B8A8_SRGB;
			generation = 17; reason = nullptr;
			if (can_readback_image_to_guest(&ctx, &bad, address, tile.size, 128, ImageReadbackLayout::Linear,
			    neo, &generation, &reason, GetMutationGeneration(), &result, acquire, GuestImagePublicationProfile::VideoOut, true) ||
			    generation != UINT64_MAX || borrows != before || reason == nullptr) return false;
		}
		for (const auto original_layout: {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL})
		{
			const bool snapshot_pair = original_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			for (uint32_t scenario = 0; scenario < 10; ++scenario)
			{
				image.layout = original_layout;
				std::memset(reinterpret_cast<void*>(allocation), 0xa5, allocation_bytes);
				const uint64_t original = GetMutationGeneration();
				uint32_t reads = 0, publication_borrows = 0, pauses = 0; bool valid = true;
				const auto publication_acquire = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* output) {
					valid = valid && begin == address && size == tile.size && mode == Mode::ReadWrite;
					if (reads != 0)
					{
						++publication_borrows;
						if (((scenario == 5 || scenario == 9) && publication_borrows <= 16) || scenario == 6) return false;
					}
					return TryAcquireGuestDataLease(begin, size, mode, output, false);
				};
				auto resume = [&](const char* phase, VideoOutReadbackContinuation::Proof proof, void* context) {
					auto result = proof(context);
					if (scenario == 8 && std::strcmp(phase, "preflight") == 0 && result.complete)
						image.layout = VK_IMAGE_LAYOUT_GENERAL;
					if (result.unavailable)
					{
						++pauses; valid = valid && !CurrentThreadHasMemoryLease();
						if (scenario == 9) image.layout = VK_IMAGE_LAYOUT_GENERAL;
						result = proof(context); // CPU acquisition only; production scope/prefix gates tested by GpuMemory.
					}
					return result;
				};
				using Resume = decltype(resume);
				const VideoOutReadbackContinuation continuation {&resume,
				    [](void* context, const char* phase, VideoOutReadbackContinuation::Proof proof, void* proof_context) {
					    return (*static_cast<Resume*>(context))(phase, proof, proof_context);
				    }};
				uint64_t hash = 17;
				const bool published = publish_videoout_readback(&ctx, &image, address, tile.size, 128, ImageReadbackLayout::Color2dThin,
				    neo, original, [&](void* staging) {
					    ++reads; valid = valid && reads == 1 && !CurrentThreadHasMemoryLease();
					    std::memcpy(staging, linear.data(), tile.size);
					    if (scenario == 1) { const auto unrelated = Alloc(0, 65536, Mode::ReadWrite); valid = unrelated != 0 && Free(unrelated) && valid; }
					    if (scenario == 2) valid = Protect(allocation, allocation_bytes, Mode::Read) && Protect(allocation, allocation_bytes, Mode::ReadWrite) && valid;
					    if (scenario == 3)
					    {
						    valid = Free(allocation) && AllocFixed(allocation, allocation_bytes, Mode::ReadWrite) && valid;
						    if (valid) std::memset(reinterpret_cast<void*>(allocation), 0xa5, allocation_bytes);
					    }
					    if (scenario == 7) image.layout = VK_IMAGE_LAYOUT_GENERAL;
					    return scenario != 4;
				    }, &hash, nullptr, publication_acquire, &continuation, GuestImagePublicationProfile::Color2dThinRenderTexture, snapshot_pair);
				const bool success = scenario == 0 || scenario == 1 || scenario == 5;
				if (!valid || published != success || reads != uint32_t(scenario != 8) || CurrentThreadHasMemoryLease() ||
				    pauses != uint32_t(scenario == 5 || scenario == 6 || scenario == 9) ||
				    (success && image.layout != original_layout)) return false;
				if (success)
				{
					if (std::memcmp(reinterpret_cast<void*>(allocation), expected.data(), allocation_bytes) != 0 ||
					    hash != XXH3_64bits(expected.data() + offset, tile.size)) return false;
				} else
				{
					if (hash != 0 || !std::all_of(reinterpret_cast<uint8_t*>(allocation), reinterpret_cast<uint8_t*>(allocation + allocation_bytes),
					    [](uint8_t value) { return value == 0xa5; })) return false;
				}
			}
		}
	}
	std::fprintf(stderr, "[GFX_EOP_COLOR_PUBLICATION_TEST] actual_VM=protected original_token=required"
	                     " fresh_capture/type/format/mip/layout/footprint=reject-before-borrow"
	                     " publish/padding/hash=full-range protect-restore/remap/read-fail/exhaust=no-store"
	                     " publication_retry=CPU-only native_reads=once GPU/CP-enrollment=modeled\n");
	std::fprintf(stderr, "[GFX_EOP_COLOR_LAYOUT_TEST] default/disjoint/VideoOut=attachment-only exact-pair=attachment-or-shader-read"
	                     " original-layout=retained bad-layouts=reject-before-borrow layout-change-before-read/after-read/during-retry=no-store"
	                     " both-layouts/base/Neo=checked publication-retry=CPU-only GPU-transfer=modeled\n");
#endif
	return true;
}

static bool videoout_publication_contract_test()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Core::VirtualMemory;
	Init();
	constexpr uint64_t bytes = 65536;
	const uint64_t address = Alloc(0, bytes, Mode::ReadWrite);
	if (address == 0) return false;
	struct Allocation { uint64_t address; ~Allocation() { if (address != 0) Free(address); } } allocation {address};
	auto* destination = reinterpret_cast<uint8_t*>(address);
	GraphicContext ctx {};
	VideoOutVulkanImage image;
	image.image = reinterpret_cast<VkImage>(uintptr_t {1}); // No driver operation in this fixture.
	image.format = VK_FORMAT_B8G8R8A8_SRGB;
	image.extent = {13, 9};
	image.mip_levels = 1;
	image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	uint64_t generation = UINT64_MAX;
	const auto capture = [&] { return UtilCanReadbackVideoOutToGuest(&ctx, &image, address, bytes, 128,
	                                                              ImageReadbackLayout::Linear, false, &generation); };
	GraphicsGuestLeaseResult public_result;
	if (!UtilCanReadbackVideoOutToGuest(&ctx, &image, address, bytes, 128, ImageReadbackLayout::Linear, false,
	                                  &generation, UINT64_MAX, &public_result) || public_result.reason != nullptr ||
	    public_result.attempts != 1 || public_result.observed_generation != generation || CurrentThreadHasMemoryLease()) return false;
	const auto publish = [&](auto&& read, uint64_t* published_hash = nullptr) {
		return publish_videoout_readback(&ctx, &image, address, bytes, 128, ImageReadbackLayout::Linear, false,
		                                 generation, std::forward<decltype(read)>(read), published_hash);
	};
	for (uint8_t value: {uint8_t {0x32}, uint8_t {0xe7}})
	{
		std::memset(destination, 0xa5, bytes);
		uint64_t published_hash = 0;
		if (!capture() || !publish([&](void* snapshot) {
			    if (CurrentThreadHasMemoryLease()) return false;
			    std::memset(snapshot, value, bytes);
			    return true;
		    }, &published_hash) || CurrentThreadHasMemoryLease()) return false;
		std::array<uint8_t, bytes> expected;
		for (uint64_t i = 0; i != bytes; ++i)
		{
			expected[i] = i / 512 < 9 && i % 512 < 52 ? value : 0xa5;
			if (destination[i] != expected[i]) return false;
		}
		if (published_hash != XXH3_64bits(expected.data(), expected.size())) return false;
	}
	// Unrelated VM allocation during the single staged GPU read does not renew
	// or revoke this destination's original mapping token.
	std::memset(destination, 0xa5, bytes);
	uint64_t unrelated = 0;
	uint32_t independent_reads = 0;
	uint64_t independent_hash = 0;
	if (!capture()) return false;
	const uint64_t independent_generation = generation;
	const bool independent_published = publish([&](void* snapshot) {
		++independent_reads;
		if (CurrentThreadHasMemoryLease()) return false;
		unrelated = Alloc(0, bytes, Mode::ReadWrite);
		std::memset(snapshot, 0x32, bytes);
		return unrelated != 0;
	}, &independent_hash);
	if (unrelated == 0 || !Free(unrelated) || !independent_published || independent_reads != 1 ||
	    generation != independent_generation || CurrentThreadHasMemoryLease()) return false;
	std::array<uint8_t, bytes> independent_expected;
	for (uint64_t i = 0; i < bytes; ++i)
	{
		independent_expected[i] = i / 512 < 9 && i % 512 < 52 ? 0x32 : 0xa5;
		if (destination[i] != independent_expected[i]) return false;
	}
	if (independent_hash != XXH3_64bits(independent_expected.data(), independent_expected.size())) return false;
	// A mutation during the simulated GPU wait must leave all guest bytes intact.
	std::memset(destination, 0xa5, bytes);
	uint64_t published_hash = UINT64_MAX;
	if (!capture() || publish([&](void*) { return Protect(address, bytes, Mode::Read); }, &published_hash) ||
	    published_hash != 0) return false;
	if (!Protect(address, bytes, Mode::ReadWrite)) return false;
	for (uint64_t i = 0; i != bytes; ++i) if (destination[i] != 0xa5) return false;
	for (const bool restore_rights: {false, true})
	{
		if (!capture()) return false;
		published_hash = UINT64_MAX;
		if (publish([&](void*) {
			    return restore_rights ? Protect(address, bytes, Mode::Read) && Protect(address, bytes, Mode::ReadWrite)
			                          : Protect(address, bytes, Mode::ReadWrite);
		    }, &published_hash) || published_hash != 0 || CurrentThreadHasMemoryLease()) return false;
		for (uint64_t i = 0; i < bytes; ++i) if (destination[i] != 0xa5) return false;
	}
	bool read_called = false;
	published_hash = UINT64_MAX;
	if (publish([&](void*) { read_called = true; return true; }, &published_hash) || read_called ||
	    published_hash != 0) return false;
	if (!capture()) return false;
	published_hash = UINT64_MAX;
	if (publish([&](void*) {
		    if (!Free(address)) return false;
		    allocation.address = 0;
		    if (!AllocFixed(address, bytes, Mode::ReadWrite)) return false;
		    allocation.address = address;
		    std::memset(destination, 0x97, bytes);
		    return true;
	    }, &published_hash) || allocation.address != address || published_hash != 0) return false;
	for (uint64_t i = 0; i != bytes; ++i) if (destination[i] != 0x97) return false;
	// Hashing must require read access before any texel is copied. A write-only
	// destination retains the existing no-hash writer route but cannot export
	// the tracker hash through an unchecked read.
	if (!Protect(address, bytes, Mode::Write) || !capture()) return false;
	published_hash = UINT64_MAX;
	if (publish([&](void* snapshot) { std::memset(snapshot, 0x42, bytes); return true; }, &published_hash) ||
	    published_hash != 0 || CurrentThreadHasMemoryLease() || !Protect(address, bytes, Mode::ReadWrite)) return false;
	for (uint64_t i = 0; i != bytes; ++i) if (destination[i] != 0x97) return false;
	// Native buffers can be borrowed by the VM API but are not owned guest mappings.
	std::array<uint8_t, 8192> native {};
	if (UtilCanReadbackVideoOutToGuest(&ctx, &image, reinterpret_cast<uint64_t>(native.data()), native.size(), 128,
	                                 ImageReadbackLayout::Linear, false, &generation, UINT64_MAX, &public_result) ||
	    public_result.reason == nullptr || std::strcmp(public_result.reason, "destination-unowned") != 0 ||
	    public_result.attempts != 1 || generation != UINT64_MAX || CurrentThreadHasMemoryLease()) return false;
	if (!Protect(address + 16384, 16384, Mode::Read) || capture() ||
	    !Protect(address + 16384, 16384, Mode::ReadWrite)) return false;
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	// Inject only an acquisition outcome after validating the real mapping.
	// The staged readback and guest publication remain the production paths.
	Core::Mutex processor_mutex, tracker_mutex;
	for (const bool retry_preflight: {true, false})
	{
		for (const uint32_t scenario: {0u, 1u, 2u, 3u}) // recover, exhaust, relevant revoke, unrelated mutation
		{
			std::memset(destination, 0xa5, bytes);
			if (!capture()) return false;
			const uint64_t original_generation = generation;
			uint32_t read_calls = 0, preflight_calls = 0, publication_calls = 0;
			uint64_t unrelated_retry = 0;
			bool valid = true;
			const auto acquire = [&](uint64_t begin, uint64_t size, Mode mode, GuestDataLease* lease) {
				const bool preflight = read_calls == 0;
				auto& calls = preflight ? preflight_calls : publication_calls;
				++calls;
				valid = valid && begin == address && size == bytes && generation == original_generation &&
				        mode == (preflight ? Mode::Write : Mode::ReadWrite) && !CurrentThreadHasMemoryLease();
				for (uint64_t i = 0; i < bytes; ++i) valid = valid && destination[i] == 0xa5;
				bool processor_available = true, tracker_available = true;
				std::thread observer([&] {
					processor_available = processor_mutex.TryLock();
					if (processor_available) processor_mutex.Unlock();
					tracker_available = tracker_mutex.TryLock();
					if (tracker_available) tracker_mutex.Unlock();
				});
				observer.join();
				valid = valid && !processor_available && !tracker_available;
				if (!TryAcquireGuestDataLease(begin, size, mode, lease, false)) { valid = false; return false; }
				if (preflight != retry_preflight) return true;
				if (scenario == 2 && calls == 1)
				{
					ReleaseGuestDataLease(lease);
					valid = Protect(address, bytes, Mode::Read) && Protect(address, bytes, Mode::ReadWrite) && valid;
					return false;
				}
				if (scenario == 3 && calls == 1)
				{
					ReleaseGuestDataLease(lease);
					unrelated_retry = Alloc(0, bytes, Mode::ReadWrite);
					valid = unrelated_retry != 0 && valid;
					return false;
				}
				return (scenario == 0 || scenario == 2 || scenario == 3) && calls > 1;
			};
			VideoOutPublicationFailure failure;
			published_hash = UINT64_MAX;
			bool published;
			{
				Core::LockGuard processor_lock(processor_mutex);
				Core::LockGuard tracker_lock(tracker_mutex);
				published = publish_videoout_readback(&ctx, &image, address, bytes, 128, ImageReadbackLayout::Linear,
				    false, original_generation, [&](void* snapshot) {
					    ++read_calls;
					    valid = valid && !CurrentThreadHasMemoryLease();
					    std::memset(snapshot, 0x32, bytes);
					    return true;
				    }, &published_hash, &failure, acquire);
			}
			if (unrelated_retry != 0 && !Free(unrelated_retry)) return false;
			const bool success = scenario == 0 || scenario == 3;
			const uint32_t selected_calls = retry_preflight ? preflight_calls : publication_calls;
			const uint32_t expected_calls = scenario == 1 ? 16u : 2u;
			if (!valid || published != success || selected_calls != expected_calls || generation != original_generation ||
			    read_calls != (!success && retry_preflight ? 0u : 1u) || CurrentThreadHasMemoryLease()) return false;
			std::array<uint8_t, bytes> expected;
			for (uint64_t i = 0; i < bytes; ++i)
			{
				expected[i] = success && i / 512 < 9 && i % 512 < 52 ? 0x32 : 0xa5;
				if (destination[i] != expected[i]) return false;
			}
			if (success)
			{
				if (published_hash != XXH3_64bits(expected.data(), expected.size())) return false;
			} else if (published_hash != 0 || failure.attempts != expected_calls ||
			           std::strcmp(failure.phase, retry_preflight ? "preflight" : "publication") != 0 ||
			           std::strcmp(failure.reason, scenario == 1 ? "lease-acquisition-retry-exhausted" : "mapping-continuity-changed") != 0)
				return false;
			if (retry_preflight && scenario == 1)
			{
				// The exhausted pure preflight had no staged read or guest store.
				// A later admitted phase retains its original token and performs
				// the single prepared read once. Publication failures above remain
				// final and never enter this resumption boundary.
				if (read_calls != 0 || generation != original_generation) return false;
				const bool resumed = publish_videoout_readback(&ctx, &image, address, bytes, 128,
				    ImageReadbackLayout::Linear, false, original_generation, [&](void* snapshot) {
					    ++read_calls;
					    if (CurrentThreadHasMemoryLease()) return false;
					    for (uint64_t i = 0; i < bytes; ++i) if (destination[i] != 0xa5) return false;
					    std::memset(snapshot, 0x32, bytes);
					    return true;
				    }, &published_hash);
				if (!resumed || read_calls != 1 || generation != original_generation || CurrentThreadHasMemoryLease()) return false;
				for (uint64_t i = 0; i < bytes; ++i)
				{
					expected[i] = i / 512 < 9 && i % 512 < 52 ? 0x32 : 0xa5;
					if (destination[i] != expected[i]) return false;
				}
				if (published_hash != XXH3_64bits(expected.data(), expected.size())) return false;
			}
		}
	}
	std::fprintf(stderr, "[GFX_VIDEOOUT_LEASE_RETRY_TEST] phases=preflight/publication recover=2 exhaust=16"
	                     " relevant-mutation=reject unrelated-mutation/retry=accepted readback-count=once copy-before-admission=none"
	                     " pure-preflight-resume=once late-refusal=no-replay original-ticket=retained hash=same-lease caller-locks=retained sleeps=0\n");
	// A qualified EOP scope can yield after precise local exhaustion without
	// repeating the staging read or any earlier callback. Exercise real recursive
	// processor/tracker mutexes, the retained RunLock and real mapping proofs.
	enum class ResumeScenario { Recover, Exhaust, ProtectRestore, Unrelated, Remap, NativeChanged, HeldLease, SharedBudget };
	Core::Mutex run_mutex;
	for (const bool retry_preflight: {true, false})
		for (const auto scenario: {ResumeScenario::Recover, ResumeScenario::Exhaust, ResumeScenario::ProtectRestore,
		        ResumeScenario::Unrelated, ResumeScenario::Remap, ResumeScenario::NativeChanged,
		        ResumeScenario::HeldLease, ResumeScenario::SharedBudget})
		{
			std::memset(destination, 0xa5, bytes);
			if (!capture()) return false;
			const uint64_t original_generation = generation;
			const auto original_image = image;
			uint32_t reads = 0, preflight_calls = 0, publication_calls = 0, pauses = 0;
			uint32_t backoffs = scenario == ResumeScenario::SharedBudget ? 14u : 0u;
			uint64_t unrelated_resume = 0;
			bool valid = true;
			// An earlier callback's sentinel must survive every continuation.
			uint32_t prior_effects = 1;
			const auto acquire = [&](uint64_t begin, uint64_t count, Mode mode, GuestDataLease* output) {
				const bool preflight = reads == 0;
				auto& calls = preflight ? preflight_calls : publication_calls;
				++calls;
				valid = valid && begin == address && count == bytes && generation == original_generation && prior_effects == 1 &&
				        mode == (preflight ? Mode::Write : Mode::ReadWrite);
				for (uint64_t i = 0; i < bytes; ++i) valid = valid && destination[i] == 0xa5;
				const bool selected = scenario == ResumeScenario::SharedBudget || scenario == ResumeScenario::HeldLease ||
				    preflight == retry_preflight;
				if (selected && (calls <= 16 || scenario == ResumeScenario::Exhaust ||
				    (scenario == ResumeScenario::SharedBudget && !preflight))) return false;
				return TryAcquireGuestDataLease(begin, count, mode, output, false);
			};
			auto resume = [&](const char*, VideoOutReadbackContinuation::Proof proof, void* proof_context) {
				const auto validate = [&]() -> EopCpuProofResult {
					valid = valid && !CurrentThreadHasMemoryLease() && prior_effects == 1;
					if (std::memcmp(&image, &original_image, sizeof(image)) != 0)
						return {false, false, "fixture-native-changed"};
					GuestDataLease continuity;
					GraphicsGuestLeaseResult observation;
					const bool complete = TryAcquireGraphicsGuestContinuityLease(address, bytes, Mode::Write,
					    original_generation, &continuity, &observation);
					ReleaseGuestDataLease(&continuity);
					return videoout_cpu_acquisition_result(complete, observation);
				};
				return ResumeEopCpuProof([&] { return proof(proof_context); }, validate, [&] {
					WithEopCpuProofLocksReleased(tracker_mutex, [&] { processor_mutex.Unlock(); },
					    [&] { processor_mutex.Lock(); }, [&] {
						    ++pauses;
						    valid = valid && !CurrentThreadHasMemoryLease() && prior_effects == 1;
						    bool processor_available = false, tracker_available = false, run_available = true;
						    std::thread observer([&] {
							    processor_available = processor_mutex.TryLock();
							    if (processor_available) processor_mutex.Unlock();
							    tracker_available = tracker_mutex.TryLock();
							    if (tracker_available) tracker_mutex.Unlock();
							    run_available = run_mutex.TryLock();
							    if (run_available) run_mutex.Unlock();
						    });
						    observer.join();
						    valid = valid && processor_available && tracker_available && !run_available;
						    if (scenario == ResumeScenario::ProtectRestore)
							    valid = Protect(address, bytes, Mode::Read) && Protect(address, bytes, Mode::ReadWrite) && valid;
						    if (scenario == ResumeScenario::Unrelated && pauses == 1)
						    {
							    unrelated_resume = Alloc(0, bytes, Mode::ReadWrite);
							    valid = unrelated_resume != 0 && valid;
						    }
						    if (scenario == ResumeScenario::Remap)
						    {
							    valid = Free(address) && valid;
							    allocation.address = 0;
							    valid = AllocFixed(address, bytes, Mode::ReadWrite) && valid;
							    allocation.address = address;
							    std::memset(destination, 0xa5, bytes);
						    }
						    if (scenario == ResumeScenario::NativeChanged) ++image.extent.width;
					    });
				}, [] { return CurrentThreadHasMemoryLease(); }, &backoffs);
			};
			using Resume = decltype(resume);
			VideoOutReadbackContinuation continuation {&resume,
			    [](void* context, const char* phase, VideoOutReadbackContinuation::Proof proof, void* proof_context) {
				    return (*static_cast<Resume*>(context))(phase, proof, proof_context);
			    }};
			VideoOutPublicationFailure failure;
			published_hash = UINT64_MAX;
			bool published;
			{
				Core::LockGuard run_lock(run_mutex), processor_lock1(processor_mutex), processor_lock2(processor_mutex),
				    tracker_lock1(tracker_mutex), tracker_lock2(tracker_mutex);
				GuestDataLease external;
				if (scenario == ResumeScenario::HeldLease && !TryAcquireGuestDataLease(address, 8, Mode::Read, &external, false))
					return false;
				published = publish_videoout_readback(&ctx, &image, address, bytes, 128, ImageReadbackLayout::Linear,
				    false, original_generation, [&](void* snapshot) {
					    ++reads;
					    valid = valid && reads == 1 && !CurrentThreadHasMemoryLease() && prior_effects == 1;
					    std::memset(snapshot, 0x32, bytes);
					    return true;
				    }, &published_hash, &failure, acquire, &continuation);
				ReleaseGuestDataLease(&external);
				bool processor_available = true, tracker_available = true;
				std::thread observer([&] {
					processor_available = processor_mutex.TryLock();
					if (processor_available) processor_mutex.Unlock();
					tracker_available = tracker_mutex.TryLock();
					if (tracker_available) tracker_mutex.Unlock();
				});
				observer.join();
				valid = valid && !processor_available && !tracker_available;
			}
			image = original_image;
			if (unrelated_resume != 0 && !Free(unrelated_resume)) return false;
			const bool success = scenario == ResumeScenario::Recover || scenario == ResumeScenario::Unrelated;
			const uint32_t selected_calls = retry_preflight ? preflight_calls : publication_calls;
			if (!valid || published != success || prior_effects != 1 || generation != original_generation || CurrentThreadHasMemoryLease())
				return false;
			if (scenario == ResumeScenario::HeldLease)
			{
				if (pauses != 0 || reads != 0 || preflight_calls != 1 || failure.reason == nullptr ||
				    std::strcmp(failure.reason, "retry-memory-lease-held") != 0) return false;
			} else if (scenario == ResumeScenario::SharedBudget)
			{
				if (pauses != 1 || backoffs != 15 || reads != 1 || preflight_calls != 17 || publication_calls != 16 ||
				    failure.reason == nullptr || std::strcmp(failure.reason, "cpu-proof-resume-exhausted") != 0) return false;
			} else
			{
				const uint32_t expected_calls = success ? 17u : scenario == ResumeScenario::Exhaust ? 256u : 16u;
				const uint32_t expected_pauses = scenario == ResumeScenario::Exhaust ? 15u : 1u;
				if (selected_calls != expected_calls || pauses != expected_pauses || backoffs != expected_pauses ||
				    reads != (!success && retry_preflight ? 0u : 1u)) return false;
			}
			std::array<uint8_t, bytes> expected;
			for (uint64_t i = 0; i < bytes; ++i)
			{
				expected[i] = success && i / 512 < 9 && i % 512 < 52 ? 0x32 : 0xa5;
				if (destination[i] != expected[i]) return false;
			}
			if (published_hash != (success ? XXH3_64bits(expected.data(), expected.size()) : 0)) return false;
		}
	std::fprintf(stderr, "[GFX_VIDEOOUT_CPU_RESUME_TEST] boundaries=entry/publication recover=17 exhaust=256"
	                     " pause_budget=shared15 locks=two-level/runlock-retained original-token=retained"
	                     " protect/restore/remap/native/held-lease=reject unrelated=accepted staging=once"
	                     " previous-effects=once copy-before-admission=none hash/padding=pass\n");
#endif
	std::fprintf(stderr, "[GFX_VIDEOOUT_PUBLICATION_TEST] original-mapping-token=pass unrelated-wait-allocation=accepted"
	                     " protect/restore/remap=reject partial-readonly=reject"
	                     " unmanaged=reject lease-during-wait=none padding=preserved publication-hash=same-lease"
	                     " hash-includes-padding=pass unreadable-hash=reject failed-hash=zero frames=2 status=clean\n");
#endif
	return true;
}

VkImageAspectFlags UtilGetDepthStencilAspectMask(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
		default: EXIT("Unsupported depth/stencil format: %d\n", static_cast<int>(format));
	}
	return 0;
}

void UtilSetDepthLayoutOptimal(DepthStencilVulkanImage* image)
{
	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();

	auto* vk_buffer = buffer.GetBufferOutsideRenderPass();

	set_image_layout(vk_buffer, image, 0, 1, UtilGetDepthStencilAspectMask(image->format), VK_IMAGE_LAYOUT_UNDEFINED,
	                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
}

void UtilInitializeDepthStencilZero(GraphicContext* ctx, DepthStencilVulkanImage* image)
{
	EXIT_IF(ctx == nullptr || image == nullptr);
	EXIT_IF(image->layout != VK_IMAGE_LAYOUT_UNDEFINED);
	DepthStencilZeroInit initialization;
	EXIT_NOT_IMPLEMENTED(initialization.Create(ctx->device, image->image_view[VulkanImage::VIEW_DEFAULT], image->format,
	                                          image->extent.width, image->extent.height) != VK_SUCCESS);
	// Utility queues may be transfer-only. A render-pass clear requires the
	// graphics queue; the image is new and has no prior queue owner/consumer.
	CommandBuffer buffer(GraphicContext::QUEUE_GFX);
	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());
	buffer.Begin();
	EXIT_IF(!initialization.Record(buffer.GetBufferOutsideRenderPass(), image->image));
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
	image->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

bool UtilVerifyDepthStencilZero(GraphicContext* ctx, DepthStencilVulkanImage* image, int queue,
                                const char** rejection, VkImageLayout expected_layout)
{
	const auto reject = [rejection](const char* reason) {
		if (rejection != nullptr) *rejection = reason;
		return false;
	};
	if (rejection != nullptr) *rejection = nullptr;
	if (ctx == nullptr || ctx->device == VK_NULL_HANDLE || ctx->physical_device == VK_NULL_HANDLE ||
	    queue != GraphicContext::QUEUE_GFX || ctx->queues[queue].vk_queue == VK_NULL_HANDLE ||
	    ctx->queues[queue].family == UINT32_MAX) return reject("readback-context");
	if ((expected_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
	     expected_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) ||
	    image == nullptr || image->type != VulkanImageType::DepthStencil || image->image == VK_NULL_HANDLE ||
	    image->format != VK_FORMAT_D32_SFLOAT_S8_UINT || image->mip_levels != 1 ||
	    image->layout != expected_layout ||
	    (image->usage & (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) !=
	        (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
		return reject("readback-image-contract");
	DepthStencilZeroReadback::Layout layout;
	if (!DepthStencilZeroReadback::ResolveLayout(image->extent.width, image->extent.height, &layout))
		return reject("readback-footprint");
	VkFormatProperties format_properties {};
	vkGetPhysicalDeviceFormatProperties(ctx->physical_device, image->format, &format_properties);
	if ((format_properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) == 0)
		return reject("readback-format-capability");
	DepthStencilZeroReadback readback;
	if (readback.Create(ctx->physical_device, ctx->device, image->extent.width, image->extent.height) != VK_SUCCESS)
		return reject("readback-staging");
	// Destroy/discard command references before the readback allocation. A
	// submitted command may be destroyed only after fence or queue-idle proof.
	struct Commands
	{
		VkDevice device;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool submitted = false, completed = false;
		~Commands()
		{
			EXIT_IF(submitted && !completed);
			if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
			if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		}
	} commands {ctx->device};
	VkCommandPoolCreateInfo pool_info {};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool_info.queueFamilyIndex = ctx->queues[queue].family;
	VkCommandPool candidate_pool = VK_NULL_HANDLE;
	if (vkCreateCommandPool(ctx->device, &pool_info, nullptr, &candidate_pool) != VK_SUCCESS)
		return reject("readback-command-pool");
	commands.pool = candidate_pool;
	VkFenceCreateInfo fence_info {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence candidate_fence = VK_NULL_HANDLE;
	if (vkCreateFence(ctx->device, &fence_info, nullptr, &candidate_fence) != VK_SUCCESS)
		return reject("readback-fence-create");
	commands.fence = candidate_fence;
	VkCommandBufferAllocateInfo allocation {};
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = commands.pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	VkCommandBuffer command = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(ctx->device, &allocation, &command) != VK_SUCCESS)
		return reject("readback-command-allocate");
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS || !readback.Record(command, image->image, expected_layout) ||
	    vkEndCommandBuffer(command) != VK_SUCCESS) return reject("readback-command-record");
	VkSubmitInfo submit {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	const auto& original_queue = ctx->queues[queue];
	if (original_queue.mutex != nullptr) original_queue.mutex->Lock();
	const auto submit_result = vkQueueSubmit(original_queue.vk_queue, 1, &submit, commands.fence);
	if (original_queue.mutex != nullptr) original_queue.mutex->Unlock();
	EXIT_NOT_IMPLEMENTED(submit_result == VK_ERROR_DEVICE_LOST);
	if (submit_result != VK_SUCCESS) return reject("readback-submit");
	commands.submitted = true;
	const auto wait_result = vkWaitForFences(ctx->device, 1, &commands.fence, VK_TRUE, UINT64_MAX);
	if (wait_result != VK_SUCCESS)
	{
		// A failed wait is never content authority. Prove safe retirement, or
		// terminate before releasing allocations still referenced by GPU work.
		if (original_queue.mutex != nullptr) original_queue.mutex->Lock();
		const auto idle_result = vkQueueWaitIdle(original_queue.vk_queue);
		if (original_queue.mutex != nullptr) original_queue.mutex->Unlock();
		EXIT_NOT_IMPLEMENTED(idle_result != VK_SUCCESS);
		commands.completed = true;
		return reject("readback-fence");
	}
	commands.completed = true;
	return readback.Verify(rejection);
}

void UtilSetImageLayoutOptimal(VulkanImage* image)
{
	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();

	auto* vk_buffer = buffer.GetBufferOutsideRenderPass();

	VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT;

	set_image_layout(vk_buffer, image, 0, 1, aspect_mask, image->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
}

void UtilFillImage(GraphicContext* ctx, VulkanImage* image, const void* src_data, uint64_t size, const Vector<BufferImageCopy>& regions,
                   uint64_t dst_layout)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(image == nullptr);

	VulkanBuffer staging_buffer {};
	staging_buffer.usage           = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	staging_buffer.memory.property = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	VulkanCreateBuffer(ctx, size, &staging_buffer);

	void* data = nullptr;
	VulkanMapMemory(ctx, &staging_buffer.memory, &data);
	std::memcpy(data, src_data, size);
	VulkanUnmapMemory(ctx, &staging_buffer.memory);

	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();
	UtilBufferToImage(&buffer, &staging_buffer, image, regions, dst_layout);
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();

	VulkanDeleteBuffer(ctx, &staging_buffer);
}

void UtilFillImage(GraphicContext* ctx, const Vector<ImageImageCopy>& regions, VulkanImage* dst_image, uint64_t dst_layout)
{
	EXIT_IF(ctx == nullptr);
	EXIT_IF(dst_image == nullptr);

	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();
	UtilImageToImage(&buffer, regions, dst_image, dst_layout);
	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
}

void UtilCopyBuffer(VulkanBuffer* src_buffer, VulkanBuffer* dst_buffer, uint64_t size)
{
	EXIT_IF(src_buffer == nullptr);
	EXIT_IF(src_buffer->buffer == nullptr);
	EXIT_IF(dst_buffer == nullptr);
	EXIT_IF(dst_buffer->buffer == nullptr);

	CommandBuffer buffer(GraphicContext::QUEUE_UTIL);
	// buffer.SetQueue(GraphicContext::QUEUE_UTIL);

	EXIT_NOT_IMPLEMENTED(buffer.IsInvalid());

	buffer.Begin();

	auto* vk_buffer = buffer.GetBufferOutsideRenderPass();

	VkBufferCopy copy_region {};
	copy_region.srcOffset = 0;
	copy_region.dstOffset = 0;
	copy_region.size      = size;

	vkCmdCopyBuffer(vk_buffer, src_buffer->buffer, dst_buffer->buffer, 1, &copy_region);

	buffer.End();
	buffer.Execute();
	buffer.WaitForFence();
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
