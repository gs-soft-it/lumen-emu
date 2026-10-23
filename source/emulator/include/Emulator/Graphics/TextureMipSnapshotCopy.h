#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURE_MIP_SNAPSHOT_COPY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURE_MIP_SNAPSHOT_COPY_H_

#include "Emulator/Graphics/VulkanDispatch.h"

namespace Lumen::Libs::Graphics {

struct TextureMipSnapshotCopySource
{
	VkImage image;
	VkExtent2D extent;
	VkImageLayout layout;
};

// The caller owns resource admission, queue ordering and lifetime. This small
// command emitter is shared by the admitted runtime path and the GPU probe.
inline bool RecordTextureMipSnapshotCopy(VkCommandBuffer buffer, VkImage destination,
                                         const TextureMipSnapshotCopySource* sources, uint32_t levels)
{
	if (buffer == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || sources == nullptr || levels < 2 || levels >= 16)
	{
		return false;
	}
	VkImageMemoryBarrier before[16] {};
	VkImageMemoryBarrier after[16] {};
	const auto barrier = [](VkImage image, uint32_t count, VkImageLayout old_layout, VkImageLayout new_layout,
	                        VkAccessFlags source_access, VkAccessFlags destination_access)
	{
		VkImageMemoryBarrier value {};
		value.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		value.srcAccessMask = source_access;
		value.dstAccessMask = destination_access;
		value.oldLayout = old_layout;
		value.newLayout = new_layout;
		value.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		value.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		value.image = image;
		value.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, count, 0, 1};
		return value;
	};
	for (uint32_t level = 0; level < levels; ++level)
	{
		const auto& source = sources[level];
		if (source.image == VK_NULL_HANDLE || source.image == destination || source.extent.width == 0 ||
		    source.extent.height == 0 || source.layout != (level == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		                                                          : VK_IMAGE_LAYOUT_GENERAL))
		{
			return false;
		}
		for (uint32_t previous = 0; previous < level; ++previous)
		{
			if (source.image == sources[previous].image) return false;
		}
		before[level] = barrier(source.image, 1, source.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		after[level] = barrier(source.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source.layout,
		                       VK_ACCESS_TRANSFER_READ_BIT, level == 0 ? VK_ACCESS_MEMORY_READ_BIT
		                                                               : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	}
	before[levels] = barrier(destination, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                         0, VK_ACCESS_TRANSFER_WRITE_BIT);
	after[levels] = barrier(destination, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, levels + 1, before);
	for (uint32_t level = 0; level < levels; ++level)
	{
		VkImageCopy region {};
		region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		region.extent = {sources[level].extent.width, sources[level].extent.height, 1};
		vkCmdCopyImage(buffer, sources[level].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		               destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}
	vkCmdPipelineBarrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     0, 0, nullptr, 0, nullptr, levels + 1, after);
	return true;
}

} // namespace Lumen::Libs::Graphics

#endif
