#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURE_COMMON_REFRESH_COPY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURE_COMMON_REFRESH_COPY_H_

#include "Emulator/Graphics/VulkanDispatch.h"

namespace Lumen::Libs::Graphics {

// Admission, object lifetimes and same-queue ordering belong to the caller.
// Both images retain their identity and layout; previous sampled reads must
// finish before the destination is overwritten. Shared by runtime and probe.
inline bool RecordImageSampledRefresh(VkCommandBuffer command, VkImage source, VkImageLayout source_layout,
                                        VkImage destination, VkImageLayout destination_layout,
                                        uint32_t width, uint32_t height)
{
	if (command == VK_NULL_HANDLE || source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || source == destination ||
	    width == 0 || height == 0 ||
	    (source_layout != VK_IMAGE_LAYOUT_GENERAL && source_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
	     source_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
	    destination_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
	{
		return false;
	}
	const auto barrier = [](VkImage image, VkImageLayout before, VkImageLayout after,
	                        VkAccessFlags source_access, VkAccessFlags destination_access)
	{
		VkImageMemoryBarrier result {};
		result.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		result.srcAccessMask = source_access;
		result.dstAccessMask = destination_access;
		result.oldLayout = before;
		result.newLayout = after;
		result.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		result.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		result.image = image;
		result.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		return result;
	};
	const VkAccessFlags memory_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	const VkImageMemoryBarrier before[] = {
	    barrier(source, source_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, memory_access, VK_ACCESS_TRANSFER_READ_BIT),
	    barrier(destination, destination_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, memory_access, VK_ACCESS_TRANSFER_WRITE_BIT),
	};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 2, before);
	VkImageCopy region {};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.extent = {width, height, 1};
	vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	const VkImageMemoryBarrier after[] = {
	    barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_layout, VK_ACCESS_TRANSFER_READ_BIT, memory_access),
	    barrier(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, destination_layout, VK_ACCESS_TRANSFER_WRITE_BIT, memory_access),
	};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     0, 0, nullptr, 0, nullptr, 2, after);
	return true;
}

inline bool RecordStorageSampledRefresh(VkCommandBuffer command, VkImage source, VkImageLayout source_layout,
                                        VkImage destination, VkImageLayout destination_layout,
                                        uint32_t width, uint32_t height)
{
	return source_layout == VK_IMAGE_LAYOUT_GENERAL &&
	       RecordImageSampledRefresh(command, source, source_layout, destination, destination_layout, width, height);
}

inline bool RecordRenderTargetSampledRefresh(VkCommandBuffer command, VkImage source, VkImageLayout source_layout,
                                             VkImage destination, VkImageLayout destination_layout,
                                             uint32_t width, uint32_t height)
{
	return (source_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || source_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) &&
	       RecordImageSampledRefresh(command, source, source_layout, destination, destination_layout, width, height);
}

} // namespace Lumen::Libs::Graphics

#endif
