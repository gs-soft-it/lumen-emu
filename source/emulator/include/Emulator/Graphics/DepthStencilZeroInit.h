#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILZEROINIT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILZEROINIT_H_

#include <cstdint>
#include <vulkan/vulkan.h>
#include "Emulator/Graphics/VulkanDispatch.h"

namespace Lumen::Libs::Graphics {

// Host-only initialization of one D32S8 mip/layer. The caller supplies a view
// covering both aspects and waits for the recorded commands before destruction.
// This does not validate guest content, admit aliases, or decode any metadata.
class DepthStencilZeroInit
{
public:
	DepthStencilZeroInit() = default;
	DepthStencilZeroInit(const DepthStencilZeroInit&) = delete;
	DepthStencilZeroInit& operator=(const DepthStencilZeroInit&) = delete;

	~DepthStencilZeroInit()
	{
		if (m_framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
		if (m_render_pass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_render_pass, nullptr);
	}

	[[nodiscard]] static constexpr bool Supports(VkFormat format, uint32_t width, uint32_t height)
	{
		return format == VK_FORMAT_D32_SFLOAT_S8_UINT && width != 0 && height != 0;
	}

	[[nodiscard]] VkResult Create(VkDevice device, VkImageView view, VkFormat format, uint32_t width, uint32_t height)
	{
		if (device == VK_NULL_HANDLE || view == VK_NULL_HANDLE || m_device != VK_NULL_HANDLE ||
		    !Supports(format, width, height)) return VK_ERROR_INITIALIZATION_FAILED;
		m_device = device;
		m_width = width;
		m_height = height;
		VkAttachmentDescription attachment {};
		attachment.format = format;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		VkAttachmentReference reference {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.pDepthStencilAttachment = &reference;
		VkSubpassDependency dependencies[2] {};
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[0].dstStageMask = dependencies[0].srcStageMask;
		dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = dependencies[0].srcStageMask;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		VkRenderPassCreateInfo pass_info {};
		pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		pass_info.attachmentCount = 1;
		pass_info.pAttachments = &attachment;
		pass_info.subpassCount = 1;
		pass_info.pSubpasses = &subpass;
		pass_info.dependencyCount = 2;
		pass_info.pDependencies = dependencies;
		VkResult result = vkCreateRenderPass(device, &pass_info, nullptr, &m_render_pass);
		if (result != VK_SUCCESS) return result;
		VkFramebufferCreateInfo framebuffer_info {};
		framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer_info.renderPass = m_render_pass;
		framebuffer_info.attachmentCount = 1;
		framebuffer_info.pAttachments = &view;
		framebuffer_info.width = width;
		framebuffer_info.height = height;
		framebuffer_info.layers = 1;
		return vkCreateFramebuffer(device, &framebuffer_info, nullptr, &m_framebuffer);
	}

	// Discard the previous contents of mip 0/layer 0 and initialize both aspects.
	// Production uses this only for a new image before its first consumer.
	[[nodiscard]] bool Record(VkCommandBuffer command, VkImage image) const
	{
		if (command == VK_NULL_HANDLE || image == VK_NULL_HANDLE || m_framebuffer == VK_NULL_HANDLE) return false;
		VkImageMemoryBarrier barrier {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
		                     0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkClearValue clear {};
		clear.depthStencil = {0.0f, 0};
		VkRenderPassBeginInfo begin {};
		begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		begin.renderPass = m_render_pass;
		begin.framebuffer = m_framebuffer;
		begin.renderArea.extent = {m_width, m_height};
		begin.clearValueCount = 1;
		begin.pClearValues = &clear;
		vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(command);
		return true;
	}

private:
	VkDevice m_device = VK_NULL_HANDLE;
	VkRenderPass m_render_pass = VK_NULL_HANDLE;
	VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
	uint32_t m_width = 0;
	uint32_t m_height = 0;
};

} // namespace Lumen::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSTENCILZEROINIT_H_
