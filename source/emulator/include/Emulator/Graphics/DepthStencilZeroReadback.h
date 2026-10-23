#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILZEROREADBACK_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILZEROREADBACK_H_

#include <cstdint>
#include <cstring>
#include <vulkan/vulkan.h>
#include "Emulator/Graphics/VulkanDispatch.h"

namespace Lumen::Libs::Graphics {

// Original host-only D32S8 verification. No guest encoding, alias permission,
// metadata decoder or hash is used. The caller proves producer completion and
// retains the image, then completes or discards every recorded command before
// destroying this object. Only logical image texels are inspected; guest tiled
// allocation padding is outside the host image and remains the caller's concern.
class DepthStencilZeroReadback
{
public:
	struct Layout
	{
		uint32_t width = 0, height = 0, depth_pitch = 0, stencil_pitch = 0;
		VkDeviceSize depth_offset = 0, stencil_offset = 0, bytes = 0;
	};
	static constexpr VkDeviceSize MAX_PLANE_BYTES = 16u * 1024u * 1024u;
	static constexpr VkDeviceSize MAX_STAGING_BYTES = 64u * 1024u * 1024u;
	static constexpr VkDeviceSize GUARD_BYTES = 256;
	static constexpr uint8_t UNSET_BYTE = 0xcd;

	DepthStencilZeroReadback() = default;
	DepthStencilZeroReadback(const DepthStencilZeroReadback&) = delete;
	DepthStencilZeroReadback& operator=(const DepthStencilZeroReadback&) = delete;
	~DepthStencilZeroReadback()
	{
		if (m_data != nullptr) vkUnmapMemory(m_device, m_memory);
		if (m_buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_buffer, nullptr);
		if (m_memory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_memory, nullptr);
	}

	[[nodiscard]] static bool ResolveLayout(uint32_t width, uint32_t height, Layout* output)
	{
		if (output == nullptr || width == 0 || height == 0) return false;
		const uint64_t pixels = static_cast<uint64_t>(width) * height;
		if (pixels > MAX_PLANE_BYTES / 4u) return false;
		const uint64_t depth_pitch = (static_cast<uint64_t>(width) + 63u) & ~uint64_t{63};
		const uint64_t stencil_pitch = (static_cast<uint64_t>(width) + 255u) & ~uint64_t{255};
		if (depth_pitch > UINT32_MAX || stencil_pitch > UINT32_MAX) return false;
		const uint64_t depth_bytes = depth_pitch * height * 4u;
		const uint64_t stencil_bytes = stencil_pitch * height;
		const uint64_t stencil_offset = GUARD_BYTES + depth_bytes + GUARD_BYTES;
		const uint64_t bytes = stencil_offset + stencil_bytes + GUARD_BYTES;
		if (bytes > MAX_STAGING_BYTES) return false;
		*output = {width, height, static_cast<uint32_t>(depth_pitch), static_cast<uint32_t>(stencil_pitch),
		           GUARD_BYTES, stencil_offset, bytes};
		return true;
	}

	[[nodiscard]] VkResult Create(VkPhysicalDevice physical_device, VkDevice device, uint32_t width, uint32_t height)
	{
		if (physical_device == VK_NULL_HANDLE || device == VK_NULL_HANDLE || m_device != VK_NULL_HANDLE ||
		    !ResolveLayout(width, height, &m_layout)) return VK_ERROR_INITIALIZATION_FAILED;
		m_device = device;
		VkBufferCreateInfo buffer_info {};
		buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size = m_layout.bytes;
		buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkBuffer candidate_buffer = VK_NULL_HANDLE;
		VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &candidate_buffer);
		if (result != VK_SUCCESS) return result;
		m_buffer = candidate_buffer;
		VkMemoryRequirements requirements {};
		vkGetBufferMemoryRequirements(device, m_buffer, &requirements);
		VkPhysicalDeviceMemoryProperties properties {};
		vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
		if (properties.memoryTypeCount > VK_MAX_MEMORY_TYPES) return VK_ERROR_INITIALIZATION_FAILED;
		bool allocated = false;
		uint32_t attempted = 0;
		for (uint32_t tier = 0; tier != 2 && !allocated; ++tier)
		{
			const VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			    (tier == 0 ? static_cast<VkMemoryPropertyFlags>(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : 0u);
			for (uint32_t type = 0; type < properties.memoryTypeCount; ++type)
			{
				const auto bit = uint32_t{1} << type;
				if ((requirements.memoryTypeBits & bit) == 0 || (attempted & bit) != 0 ||
				    (properties.memoryTypes[type].propertyFlags & required) != required) continue;
				attempted |= bit;
				VkMemoryAllocateInfo allocation {};
				allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
				allocation.allocationSize = requirements.size;
				allocation.memoryTypeIndex = type;
				VkDeviceMemory candidate = VK_NULL_HANDLE;
				result = vkAllocateMemory(device, &allocation, nullptr, &candidate);
				if (result == VK_SUCCESS)
				{
					m_memory = candidate;
					m_coherent = (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
					allocated = true;
					break;
				}
			}
		}
		if (!allocated) return attempted == 0 ? VK_ERROR_FEATURE_NOT_PRESENT : result;
		result = vkBindBufferMemory(device, m_buffer, m_memory, 0);
		if (result != VK_SUCCESS) return result;
		void* candidate_data = nullptr;
		result = vkMapMemory(device, m_memory, 0, VK_WHOLE_SIZE, 0, &candidate_data);
		if (result != VK_SUCCESS) return result;
		if (candidate_data == nullptr) return VK_ERROR_MEMORY_MAP_FAILED;
		m_data = candidate_data;
		std::memset(m_data, UNSET_BYTE, static_cast<size_t>(m_layout.bytes));
		if (!m_coherent)
		{
			VkMappedMemoryRange range {};
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = m_memory;
			range.size = VK_WHOLE_SIZE;
			result = vkFlushMappedMemoryRanges(device, 1, &range);
		}
		return result;
	}

	// Both aspects transition together; copies use their separate Vulkan element
	// sizes. The explicit original layout is restored in the same command buffer.
	// The caller proves that layout; this helper never infers alias permission.
	[[nodiscard]] bool Record(VkCommandBuffer command, VkImage image,
	                          VkImageLayout original_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) const
	{
		if (command == VK_NULL_HANDLE || image == VK_NULL_HANDLE || m_data == nullptr ||
		    (original_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL &&
		     original_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) return false;
		const bool read_only = original_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		const VkAccessFlags original_access = read_only ?
		    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT :
		    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		const VkPipelineStageFlags original_stages = read_only ?
		    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT :
		    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		VkBufferMemoryBarrier buffer_barrier {};
		buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		buffer_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		buffer_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		buffer_barrier.buffer = m_buffer;
		buffer_barrier.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     0, 0, nullptr, 1, &buffer_barrier, 0, nullptr);
		VkImageMemoryBarrier image_barrier {};
		image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_barrier.srcAccessMask = original_access;
		image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		image_barrier.oldLayout = original_layout;
		image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_barrier.image = image;
		image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		image_barrier.subresourceRange.levelCount = 1;
		image_barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(command, original_stages, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &image_barrier);
		VkBufferImageCopy copies[2] {};
		for (auto& copy: copies)
		{
			copy.imageSubresource.layerCount = 1;
			copy.imageExtent = {m_layout.width, m_layout.height, 1};
		}
		copies[0].bufferOffset = m_layout.depth_offset;
		copies[0].bufferRowLength = m_layout.depth_pitch;
		copies[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		copies[1].bufferOffset = m_layout.stencil_offset;
		copies[1].bufferRowLength = m_layout.stencil_pitch;
		copies[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
		vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 2, copies);
		buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
		                     0, 0, nullptr, 1, &buffer_barrier, 0, nullptr);
		image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		image_barrier.dstAccessMask = original_access;
		image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		image_barrier.newLayout = original_layout;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, original_stages,
		                     0, 0, nullptr, 0, nullptr, 1, &image_barrier);
		return true;
	}

	// Must be called only after a successful fence for Record's command buffer.
	[[nodiscard]] bool Verify(const char** rejection = nullptr) const
	{
		const auto reject = [rejection](const char* reason) { if (rejection != nullptr) *rejection = reason; return false; };
		if (rejection != nullptr) *rejection = nullptr;
		if (m_data == nullptr) return reject("readback-unmapped");
		if (!m_coherent)
		{
			VkMappedMemoryRange range {};
			range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			range.memory = m_memory;
			range.size = VK_WHOLE_SIZE;
			if (vkInvalidateMappedMemoryRanges(m_device, 1, &range) != VK_SUCCESS) return reject("readback-invalidate");
		}
		const auto* data = static_cast<const uint8_t*>(m_data);
		for (uint32_t y = 0; y < m_layout.height; ++y)
		{
			const auto* depth = data + m_layout.depth_offset + static_cast<uint64_t>(y) * m_layout.depth_pitch * 4u;
			const auto* stencil = data + m_layout.stencil_offset + static_cast<uint64_t>(y) * m_layout.stencil_pitch;
			for (uint32_t x = 0; x < m_layout.width; ++x)
			{
				uint32_t word = 0;
				std::memcpy(&word, depth + static_cast<uint64_t>(x) * 4u, sizeof(word));
				if (word != 0) return reject("depth-nonzero");
				if (stencil[x] != 0) return reject("stencil-nonzero");
			}
			for (uint64_t x = static_cast<uint64_t>(m_layout.width) * 4u; x < static_cast<uint64_t>(m_layout.depth_pitch) * 4u; ++x)
				if (depth[x] != UNSET_BYTE) return reject("readback-padding");
			for (uint32_t x = m_layout.width; x < m_layout.stencil_pitch; ++x)
				if (stencil[x] != UNSET_BYTE) return reject("readback-padding");
		}
		const VkDeviceSize starts[] = {0, m_layout.stencil_offset - GUARD_BYTES, m_layout.bytes - GUARD_BYTES};
		for (const auto start: starts)
			for (VkDeviceSize offset = 0; offset < GUARD_BYTES; ++offset)
				if (data[start + offset] != UNSET_BYTE) return reject("readback-guard");
		return true;
	}

private:
	VkDevice m_device = VK_NULL_HANDLE;
	VkBuffer m_buffer = VK_NULL_HANDLE;
	VkDeviceMemory m_memory = VK_NULL_HANDLE;
	void* m_data = nullptr;
	bool m_coherent = false;
	Layout m_layout;
};

} // namespace Lumen::Libs::Graphics
#endif
