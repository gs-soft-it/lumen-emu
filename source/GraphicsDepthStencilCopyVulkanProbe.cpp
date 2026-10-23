#include "vulkan/vulkan.h"
#include "emulator/include/Emulator/Graphics/DepthStencilZeroInit.h"
#include "emulator/include/Emulator/Graphics/DepthStencilZeroReadback.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#error "The Native Metal depth/stencil copy probe must be compiled with C++ exceptions disabled"
#endif

namespace
{
constexpr uint32_t     kWidth                         = 7;
constexpr uint32_t     kHeight                        = 3;
constexpr uint32_t     kLayers                        = 2;
constexpr uint32_t     kDepthBytesPerTexel            = 4;
constexpr uint32_t     kStencilBytesPerTexel          = 1;
constexpr uint32_t     kDepthRowLength                = 64;
constexpr uint32_t     kStencilRowLength              = 256;
constexpr uint32_t     kBufferImageHeight             = 5;
constexpr VkDeviceSize kGuardBytes                    = 256;
constexpr VkDeviceSize kDepthBytesPerRow              = kDepthRowLength * kDepthBytesPerTexel;
constexpr VkDeviceSize kStencilBytesPerRow            = kStencilRowLength * kStencilBytesPerTexel;
constexpr VkDeviceSize kDepthBytesPerImage            = kDepthBytesPerRow * kBufferImageHeight;
constexpr VkDeviceSize kStencilBytesPerImage          = kStencilBytesPerRow * kBufferImageHeight;
constexpr VkDeviceSize kDepthPayloadBytes             = kDepthBytesPerImage * kLayers;
constexpr VkDeviceSize kStencilPayloadBytes           = kStencilBytesPerImage * kLayers;
constexpr VkDeviceSize kDepthBufferBytes              = kGuardBytes + kDepthPayloadBytes + kGuardBytes;
constexpr VkDeviceSize kStencilBufferBytes            = kGuardBytes + kStencilPayloadBytes + kGuardBytes;
constexpr uint64_t     kCompletionTimeoutNanoseconds  = 5'000'000'000ull;
constexpr uint8_t      kGuardByte                     = 0xa5u;
constexpr uint8_t      kDepthUploadPaddingByte        = 0x91u;
constexpr uint8_t      kStencilUploadPaddingByte      = 0x63u;
constexpr uint8_t      kReadbackUnsetByte             = 0xcdu;

static_assert(kDepthBytesPerRow == 256);
static_assert(kStencilBytesPerRow == 256);

struct HostBuffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void*          mapped = nullptr;
	VkDeviceSize   size   = 0;
};

struct ProbeState
{
	VkInstance       instance       = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice         device         = VK_NULL_HANDLE;
	VkQueue          queue          = VK_NULL_HANDLE;
	uint32_t         queueFamily    = UINT32_MAX;
	HostBuffer       depthUpload;
	HostBuffer       stencilUpload;
	HostBuffer       depthReadback;
	HostBuffer       stencilReadback;
	VkImage          image          = VK_NULL_HANDLE;
	VkImageView      zeroInitView   = VK_NULL_HANDLE;
	VkDeviceMemory   imageMemory    = VK_NULL_HANDLE;
	VkCommandPool    commandPool    = VK_NULL_HANDLE;
	VkFence          fence          = VK_NULL_HANDLE;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
		if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
		if (zeroInitView != VK_NULL_HANDLE) vkDestroyImageView(device, zeroInitView, nullptr);
		if (image != VK_NULL_HANDLE) vkDestroyImage(device, image, nullptr);
		if (imageMemory != VK_NULL_HANDLE) vkFreeMemory(device, imageMemory, nullptr);
		for (HostBuffer* buffer: {&depthUpload, &stencilUpload, &depthReadback, &stencilReadback})
		{
			if (buffer->mapped != nullptr) vkUnmapMemory(device, buffer->memory);
			if (buffer->buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer->buffer, nullptr);
			if (buffer->memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer->memory, nullptr);
		}
		if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
		if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
	}
};

int Fail(const char* operation, VkResult result = VK_SUCCESS)
{
	if (result == VK_SUCCESS)
		std::fprintf(stderr, "%s\n", operation);
	else
		std::fprintf(stderr, "%s: %d\n", operation, result);
	return 1;
}

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
	                    VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((typeBits & (1u << index)) != 0 &&
		    (properties.memoryTypes[index].propertyFlags & required) == required)
		{
			return index;
		}
	}
	return UINT32_MAX;
}

VkResult CreateHostBuffer(ProbeState& state, VkDeviceSize size, VkBufferUsageFlags usage,
	                      HostBuffer* output)
{
	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size  = size;
	bufferInfo.usage = usage;
	VkResult result  = vkCreateBuffer(state.device, &bufferInfo, nullptr, &output->buffer);
	if (result != VK_SUCCESS) return result;

	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(state.device, output->buffer, &requirements);
	const uint32_t memoryType = FindMemoryType(state.physicalDevice, requirements.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(state.device, &allocationInfo, nullptr, &output->memory);
	if (result != VK_SUCCESS) return result;
	result = vkBindBufferMemory(state.device, output->buffer, output->memory, 0);
	if (result != VK_SUCCESS) return result;
	result = vkMapMemory(state.device, output->memory, 0, VK_WHOLE_SIZE, 0, &output->mapped);
	if (result != VK_SUCCESS) return result;
	output->size = size;
	return VK_SUCCESS;
}

VkResult FlushHostBuffer(const ProbeState& state, const HostBuffer& buffer)
{
	VkMappedMemoryRange range {};
	range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = buffer.memory;
	range.size   = VK_WHOLE_SIZE;
	return vkFlushMappedMemoryRanges(state.device, 1, &range);
}

VkResult InvalidateHostBuffer(const ProbeState& state, const HostBuffer& buffer)
{
	VkMappedMemoryRange range {};
	range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = buffer.memory;
	range.size   = VK_WHOLE_SIZE;
	return vkInvalidateMappedMemoryRanges(state.device, 1, &range);
}

void ResetReadback(HostBuffer* buffer, VkDeviceSize payloadBytes)
{
	auto* bytes = static_cast<uint8_t*>(buffer->mapped);
	std::memset(bytes, kGuardByte, static_cast<size_t>(buffer->size));
	std::memset(bytes + kGuardBytes, kReadbackUnsetByte, static_cast<size_t>(payloadBytes));
}

float DepthValue(uint32_t layer, uint32_t y, uint32_t x)
{
	const uint32_t ordinal = 1 + layer * kWidth * kHeight + y * kWidth + x;
	return static_cast<float>(ordinal) / 64.0f;
}

uint8_t StencilValue(uint32_t layer, uint32_t y, uint32_t x)
{
	const uint32_t ordinal = layer * kWidth * kHeight + y * kWidth + x;
	return static_cast<uint8_t>((17u + ordinal * 29u) & 0xffu);
}

void InitializeFixtures(ProbeState* state)
{
	auto* depth = static_cast<uint8_t*>(state->depthUpload.mapped);
	std::memset(depth, kGuardByte, static_cast<size_t>(state->depthUpload.size));
	std::memset(depth + kGuardBytes, kDepthUploadPaddingByte, static_cast<size_t>(kDepthPayloadBytes));
	for (uint32_t layer = 0; layer < kLayers; ++layer)
	{
		for (uint32_t y = 0; y < kHeight; ++y)
		{
			for (uint32_t x = 0; x < kWidth; ++x)
			{
				const float value = DepthValue(layer, y, x);
				const VkDeviceSize offset = kGuardBytes + layer * kDepthBytesPerImage +
				                            y * kDepthBytesPerRow + x * kDepthBytesPerTexel;
				std::memcpy(depth + offset, &value, sizeof(value));
			}
		}
	}

	auto* stencil = static_cast<uint8_t*>(state->stencilUpload.mapped);
	std::memset(stencil, kGuardByte, static_cast<size_t>(state->stencilUpload.size));
	std::memset(stencil + kGuardBytes, kStencilUploadPaddingByte,
	            static_cast<size_t>(kStencilPayloadBytes));
	for (uint32_t layer = 0; layer < kLayers; ++layer)
	{
		for (uint32_t y = 0; y < kHeight; ++y)
		{
			for (uint32_t x = 0; x < kWidth; ++x)
			{
				const VkDeviceSize offset = kGuardBytes + layer * kStencilBytesPerImage +
				                            y * kStencilBytesPerRow + x * kStencilBytesPerTexel;
				stencil[offset] = StencilValue(layer, y, x);
			}
		}
	}
	ResetReadback(&state->depthReadback, kDepthPayloadBytes);
	ResetReadback(&state->stencilReadback, kStencilPayloadBytes);
}

bool VerifyGuards(const HostBuffer& buffer, VkDeviceSize payloadBytes, const char* label)
{
	const auto* bytes = static_cast<const uint8_t*>(buffer.mapped);
	uint32_t mismatches = 0;
	for (VkDeviceSize offset = 0; offset < kGuardBytes; ++offset)
	{
		if (bytes[offset] != kGuardByte) ++mismatches;
	}
	for (VkDeviceSize offset = kGuardBytes + payloadBytes; offset < buffer.size; ++offset)
	{
		if (bytes[offset] != kGuardByte) ++mismatches;
	}
	if (mismatches != 0)
	{
		std::fprintf(stderr, "%s guard verification failed: mismatches=%u\n", label, mismatches);
		return false;
	}
	return true;
}

bool VerifyDepthReadback(const HostBuffer& buffer, bool firstLayerZero = false)
{
	if (!VerifyGuards(buffer, kDepthPayloadBytes, "depth readback")) return false;
	const auto* bytes = static_cast<const uint8_t*>(buffer.mapped);
	uint32_t mismatches = 0;
	for (uint32_t layer = 0; layer < kLayers; ++layer)
	{
		for (uint32_t y = 0; y < kBufferImageHeight; ++y)
		{
			for (uint32_t x = 0; x < kDepthRowLength; ++x)
			{
				const VkDeviceSize offset = kGuardBytes + layer * kDepthBytesPerImage +
				                            y * kDepthBytesPerRow + x * kDepthBytesPerTexel;
				if (y < kHeight && x < kWidth)
				{
					const float expected = firstLayerZero && layer == 0 ? 0.0f : DepthValue(layer, y, x);
					uint32_t expectedBits = 0, actualBits = 0;
					std::memcpy(&expectedBits, &expected, sizeof(expectedBits));
					std::memcpy(&actualBits, bytes + offset, sizeof(actualBits));
					if (actualBits != expectedBits) ++mismatches;
				} else
				{
					for (uint32_t byte = 0; byte < kDepthBytesPerTexel; ++byte)
					{
						if (bytes[offset + byte] != kReadbackUnsetByte) ++mismatches;
					}
				}
			}
		}
	}
	if (mismatches != 0)
	{
		std::fprintf(stderr, "depth payload/padding verification failed: mismatches=%u\n", mismatches);
		return false;
	}
	return true;
}

bool VerifyStencilReadback(const HostBuffer& buffer, bool firstLayerZero = false)
{
	if (!VerifyGuards(buffer, kStencilPayloadBytes, "stencil readback")) return false;
	const auto* bytes = static_cast<const uint8_t*>(buffer.mapped);
	uint32_t mismatches = 0;
	for (uint32_t layer = 0; layer < kLayers; ++layer)
	{
		for (uint32_t y = 0; y < kBufferImageHeight; ++y)
		{
			for (uint32_t x = 0; x < kStencilRowLength; ++x)
			{
				const VkDeviceSize offset = kGuardBytes + layer * kStencilBytesPerImage +
				                            y * kStencilBytesPerRow + x * kStencilBytesPerTexel;
				const uint8_t expected = y < kHeight && x < kWidth
				    ? (firstLayerZero && layer == 0 ? 0 : StencilValue(layer, y, x)) : kReadbackUnsetByte;
				if (bytes[offset] != expected) ++mismatches;
			}
		}
	}
	if (mismatches != 0)
	{
		std::fprintf(stderr, "stencil payload/padding verification failed: mismatches=%u\n", mismatches);
		return false;
	}
	return true;
}

VkBufferImageCopy MakeCopy(VkImageAspectFlags aspect, bool depth)
{
	VkBufferImageCopy copy {};
	copy.bufferOffset                    = kGuardBytes;
	copy.bufferRowLength                 = depth ? kDepthRowLength : kStencilRowLength;
	copy.bufferImageHeight               = kBufferImageHeight;
	copy.imageSubresource.aspectMask     = aspect;
	copy.imageSubresource.mipLevel       = 0;
	copy.imageSubresource.baseArrayLayer = 0;
	copy.imageSubresource.layerCount     = kLayers;
	copy.imageExtent                     = {kWidth, kHeight, 1};
	return copy;
}

VkImageMemoryBarrier MakeImageBarrier(const ProbeState& state, VkImageLayout oldLayout,
	                                  VkImageLayout newLayout, VkAccessFlags sourceAccess,
	                                  VkAccessFlags destinationAccess)
{
	VkImageMemoryBarrier barrier {};
	barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask                   = sourceAccess;
	barrier.dstAccessMask                   = destinationAccess;
	barrier.oldLayout                       = oldLayout;
	barrier.newLayout                       = newLayout;
	barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                           = state.image;
	barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	barrier.subresourceRange.baseMipLevel   = 0;
	barrier.subresourceRange.levelCount     = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount     = kLayers;
	return barrier;
}

VkResult BeginOneTimeCommand(const ProbeState& state, VkCommandBuffer* commandBuffer)
{
	VkCommandBufferAllocateInfo allocationInfo {};
	allocationInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocationInfo.commandPool        = state.commandPool;
	allocationInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocationInfo.commandBufferCount = 1;
	VkResult result = vkAllocateCommandBuffers(state.device, &allocationInfo, commandBuffer);
	if (result != VK_SUCCESS) return result;
	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	return vkBeginCommandBuffer(*commandBuffer, &beginInfo);
}

VkResult SubmitAndWait(ProbeState* state, VkCommandBuffer commandBuffer)
{
	VkResult result = vkEndCommandBuffer(commandBuffer);
	if (result != VK_SUCCESS) return result;
	VkSubmitInfo submitInfo {};
	submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount   = 1;
	submitInfo.pCommandBuffers      = &commandBuffer;
	result = vkQueueSubmit(state->queue, 1, &submitInfo, state->fence);
	if (result != VK_SUCCESS) return result;
	result = vkWaitForFences(state->device, 1, &state->fence, VK_TRUE,
	                         kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS) return result;
	return vkResetFences(state->device, 1, &state->fence);
}

VkResult TransitionImage(ProbeState* state, VkImageLayout oldLayout, VkImageLayout newLayout,
	                     VkAccessFlags sourceAccess, VkAccessFlags destinationAccess)
{
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkResult result = BeginOneTimeCommand(*state, &commandBuffer);
	if (result != VK_SUCCESS) return result;
	const VkImageMemoryBarrier barrier =
	    MakeImageBarrier(*state, oldLayout, newLayout, sourceAccess, destinationAccess);
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &barrier);
	result = SubmitAndWait(state, commandBuffer);
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &commandBuffer);
	return result;
}

VkResult RoundTrip(ProbeState* state, VkImageLayout oldLayout, bool upload)
{
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkResult result = BeginOneTimeCommand(*state, &commandBuffer);
	if (result != VK_SUCCESS) return result;

	if (upload)
	{
		const VkImageMemoryBarrier toDestination =
		    MakeImageBarrier(*state, oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		                     VK_ACCESS_TRANSFER_WRITE_BIT);
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		                     &toDestination);
		const VkBufferImageCopy depthUpload = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
		vkCmdCopyBufferToImage(commandBuffer, state->depthUpload.buffer, state->image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depthUpload);
		const VkBufferImageCopy stencilUpload = MakeCopy(VK_IMAGE_ASPECT_STENCIL_BIT, false);
		vkCmdCopyBufferToImage(commandBuffer, state->stencilUpload.buffer, state->image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &stencilUpload);
		const VkImageMemoryBarrier toSource =
		    MakeImageBarrier(*state, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		                     VK_ACCESS_TRANSFER_READ_BIT);
		vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSource);
	}

	const VkBufferImageCopy depthReadback = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
	vkCmdCopyImageToBuffer(commandBuffer, state->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       state->depthReadback.buffer, 1, &depthReadback);
	const VkBufferImageCopy stencilReadback = MakeCopy(VK_IMAGE_ASPECT_STENCIL_BIT, false);
	vkCmdCopyImageToBuffer(commandBuffer, state->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       state->stencilReadback.buffer, 1, &stencilReadback);

	std::array<VkBufferMemoryBarrier, 2> hostBarriers {};
	for (VkBufferMemoryBarrier& barrier: hostBarriers)
	{
		barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.offset              = 0;
		barrier.size                = VK_WHOLE_SIZE;
	}
	hostBarriers[0].buffer = state->depthReadback.buffer;
	hostBarriers[1].buffer = state->stencilReadback.buffer;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 0, nullptr, static_cast<uint32_t>(hostBarriers.size()),
	                     hostBarriers.data(), 0, nullptr);

	result = SubmitAndWait(state, commandBuffer);
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &commandBuffer);
	return result;
}

VkResult InitializeFirstLayerToZero(ProbeState* state)
{
	VkImageViewCreateInfo viewInfo {};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = state->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	VkResult result = vkCreateImageView(state->device, &viewInfo, nullptr, &state->zeroInitView);
	if (result != VK_SUCCESS) return result;
	Lumen::Libs::Graphics::DepthStencilZeroInit initialization;
	result = initialization.Create(state->device, state->zeroInitView, viewInfo.format, kWidth, kHeight);
	if (result != VK_SUCCESS) return result;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	result = BeginOneTimeCommand(*state, &commandBuffer);
	if (result != VK_SUCCESS) return result;
	if (!initialization.Record(commandBuffer, state->image)) return VK_ERROR_INITIALIZATION_FAILED;
	result = SubmitAndWait(state, commandBuffer);
	if (result != VK_SUCCESS) vkDeviceWaitIdle(state->device);
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &commandBuffer);
	if (result != VK_SUCCESS) return result;

	// Only layer 0 changed layout/content. Layer 1 retains its original
	// nonzero depth/stencil fixture and transfer-source layout.
	result = BeginOneTimeCommand(*state, &commandBuffer);
	if (result != VK_SUCCESS) return result;
	VkImageMemoryBarrier barrier = MakeImageBarrier(*state, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	                                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	result = SubmitAndWait(state, commandBuffer);
	if (result != VK_SUCCESS) vkDeviceWaitIdle(state->device);
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &commandBuffer);
	return result;
}

bool VerifyFirstLayerZero(ProbeState* state, const char* expected_rejection = nullptr,
                          VkImageLayout original_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
{
	// Exercise the exact production staging, recording and integral verifier.
	// Layer 1 is outside its profile and keeps its transfer-source layout.
	using Lumen::Libs::Graphics::DepthStencilZeroReadback;
	DepthStencilZeroReadback readback;
	VkResult result = readback.Create(state->physicalDevice, state->device, kWidth, kHeight);
	if (result != VK_SUCCESS) { Fail("Create production zero readback failed", result); return false; }
	VkCommandBuffer command = VK_NULL_HANDLE;
	result = BeginOneTimeCommand(*state, &command);
	if (result != VK_SUCCESS) { Fail("Begin production zero readback failed", result); return false; }
	for (const auto invalid_layout: {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
	                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL})
	{
		if (readback.Record(command, state->image, invalid_layout))
		{
			vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
			Fail("Unsupported production zero readback layout was accepted");
			return false;
		}
	}
	const bool read_only = original_layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	const VkAccessFlags original_access = read_only ?
	    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT :
	    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	const VkPipelineStageFlags original_stages = read_only ?
	    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT :
	    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	VkImageMemoryBarrier to_original = MakeImageBarrier(*state, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    original_layout, VK_ACCESS_TRANSFER_READ_BIT, original_access);
	to_original.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    original_stages, 0, 0, nullptr, 0, nullptr, 1, &to_original);
	if (!readback.Record(command, state->image, original_layout))
	{
		vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
		Fail("Record production zero readback failed");
		return false;
	}
	result = SubmitAndWait(state, command);
	if (result != VK_SUCCESS)
	{
		// An unsuccessful wait never qualifies contents and must not let local
		// command references outlive the staging allocation.
		const VkResult idle = vkDeviceWaitIdle(state->device);
		if (idle != VK_SUCCESS) { Fail("Production readback retirement could not be proved", idle); std::abort(); }
		vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
		Fail("Production zero readback fence failed", result);
		return false;
	}
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
	const char* rejection = nullptr;
	const bool verified = readback.Verify(&rejection);
	const bool expected = expected_rejection == nullptr ? verified :
	    !verified && rejection != nullptr && std::strcmp(rejection, expected_rejection) == 0;
	if (!expected)
	{
		std::fprintf(stderr, "Production zero verifier unexpected status: expected=%s observed=%s\n",
		    expected_rejection != nullptr ? expected_rejection : "zero", rejection != nullptr ? rejection : "zero");
		return false;
	}
	if (read_only)
	{
		// The next completed readback begins directly in READ_ONLY. This exercises
		// the production restore path without an intervening layout transition.
		result = BeginOneTimeCommand(*state, &command);
		if (result != VK_SUCCESS) { Fail("Begin consecutive read-only zero readback failed", result); return false; }
		if (!readback.Record(command, state->image, original_layout))
		{
			vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
			Fail("Record consecutive read-only zero readback failed");
			return false;
		}
		result = SubmitAndWait(state, command);
		if (result != VK_SUCCESS)
		{
			const VkResult idle = vkDeviceWaitIdle(state->device);
			if (idle != VK_SUCCESS) { Fail("Read-only readback retirement could not be proved", idle); std::abort(); }
		}
		vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
		if (result != VK_SUCCESS) { Fail("Consecutive read-only zero readback fence failed", result); return false; }
		rejection = nullptr;
		const bool verified_again = readback.Verify(&rejection);
		if (expected_rejection == nullptr ? !verified_again :
		    verified_again || rejection == nullptr || std::strcmp(rejection, expected_rejection) != 0)
		{
			Fail("Consecutive read-only zero readback returned different content proof");
			return false;
		}
	}
	// Restore the probe's source layout to inspect the untouched second layer.
	result = BeginOneTimeCommand(*state, &command);
	if (result != VK_SUCCESS) { Fail("Begin post-verification layout restore failed", result); return false; }
	VkImageMemoryBarrier restore = MakeImageBarrier(*state, original_layout,
	    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, original_access, VK_ACCESS_TRANSFER_READ_BIT);
	restore.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, original_stages,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &restore);
	result = SubmitAndWait(state, command);
	if (result != VK_SUCCESS)
	{
		const VkResult idle = vkDeviceWaitIdle(state->device);
		if (idle != VK_SUCCESS) { Fail("Restore retirement could not be proved", idle); std::abort(); }
	}
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
	if (result != VK_SUCCESS) { Fail("Production readback layout restore failed", result); return false; }
	return true;
}

VkResult WriteLastFirstLayerPixel(ProbeState* state, uint32_t depth_word, uint8_t stencil)
{
	std::memcpy(static_cast<uint8_t*>(state->depthUpload.mapped) + kGuardBytes, &depth_word, sizeof(depth_word));
	static_cast<uint8_t*>(state->stencilUpload.mapped)[kGuardBytes] = stencil;
	VkResult result = FlushHostBuffer(*state, state->depthUpload);
	if (result != VK_SUCCESS) return result;
	result = FlushHostBuffer(*state, state->stencilUpload);
	if (result != VK_SUCCESS) return result;
	VkCommandBuffer command = VK_NULL_HANDLE;
	result = BeginOneTimeCommand(*state, &command);
	if (result != VK_SUCCESS) return result;
	VkImageMemoryBarrier destination = MakeImageBarrier(*state, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	destination.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, nullptr, 0, nullptr, 1, &destination);
	VkBufferImageCopy depth_copy = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
	depth_copy.imageSubresource.layerCount = 1;
	depth_copy.imageOffset = {static_cast<int32_t>(kWidth - 1), static_cast<int32_t>(kHeight - 1), 0};
	depth_copy.imageExtent = {1, 1, 1};
	VkBufferImageCopy stencil_copy = depth_copy;
	stencil_copy.bufferRowLength = kStencilRowLength;
	stencil_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
	vkCmdCopyBufferToImage(command, state->depthUpload.buffer, state->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depth_copy);
	vkCmdCopyBufferToImage(command, state->stencilUpload.buffer, state->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &stencil_copy);
	VkImageMemoryBarrier source = destination;
	source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	source.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0, 0, nullptr, 0, nullptr, 1, &source);
	result = SubmitAndWait(state, command);
	if (result != VK_SUCCESS)
	{
		const VkResult idle = vkDeviceWaitIdle(state->device);
		if (idle != VK_SUCCESS) { Fail("Pixel-write retirement could not be proved", idle); std::abort(); }
	}
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
	return result;
}

enum class CopyDirection
{
	BufferToImage,
	ImageToBuffer,
};

bool ExpectAspectRejected(ProbeState* state, CopyDirection direction,
	                      VkImageAspectFlags aspectMask, const char* label)
{
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkResult result = BeginOneTimeCommand(*state, &commandBuffer);
	if (result != VK_SUCCESS)
	{
		Fail("BeginOneTimeCommand(invalid aspect) failed", result);
		return false;
	}
	const VkBufferImageCopy copy = MakeCopy(aspectMask, true);
	if (direction == CopyDirection::BufferToImage)
	{
		vkCmdCopyBufferToImage(commandBuffer, state->depthUpload.buffer, state->image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	} else
	{
		vkCmdCopyImageToBuffer(commandBuffer, state->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       state->depthReadback.buffer, 1, &copy);
	}

	const VkResult endResult = vkEndCommandBuffer(commandBuffer);
	const char* rejectionStage = "end";
	if (endResult == VK_SUCCESS)
	{
		VkSubmitInfo submitInfo {};
		submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers    = &commandBuffer;
		result         = vkQueueSubmit(state->queue, 1, &submitInfo, VK_NULL_HANDLE);
		rejectionStage = "submit";
	} else
	{
		result = endResult;
	}
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &commandBuffer);
	if (result != VK_ERROR_FORMAT_NOT_SUPPORTED && result != VK_ERROR_INITIALIZATION_FAILED)
	{
		std::fprintf(stderr, "%s was not rejected as an invalid aspect: %d\n",
		             label, result);
		return false;
	}
	std::printf("[GFX_DEPTH_STENCIL_COPY_INVALID] case=%s stage=%s result=invalid-aspect\n", label,
	            rejectionStage);
	return true;
}

bool ExpectFootprintRejected(ProbeState* state, CopyDirection direction,
	                         const VkBufferImageCopy& copy, const char* label)
{
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkResult result = BeginOneTimeCommand(*state, &commandBuffer);
	if (result != VK_SUCCESS)
	{
		Fail("BeginOneTimeCommand(invalid footprint) failed", result);
		return false;
	}
	if (direction == CopyDirection::BufferToImage)
	{
		vkCmdCopyBufferToImage(commandBuffer, state->depthUpload.buffer, state->image,
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	} else
	{
		vkCmdCopyImageToBuffer(commandBuffer, state->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       state->depthReadback.buffer, 1, &copy);
	}

	const VkResult endResult = vkEndCommandBuffer(commandBuffer);
	const char* rejectionStage = "end";
	if (endResult == VK_SUCCESS)
	{
		VkSubmitInfo submitInfo {};
		submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers    = &commandBuffer;
		result         = vkQueueSubmit(state->queue, 1, &submitInfo, VK_NULL_HANDLE);
		rejectionStage = "submit";
	} else
	{
		result = endResult;
	}
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &commandBuffer);
	if (result != VK_ERROR_INITIALIZATION_FAILED)
	{
		std::fprintf(stderr, "%s was not rejected with VK_ERROR_INITIALIZATION_FAILED: %d\n",
		             label, result);
		return false;
	}
	std::printf("[GFX_DEPTH_STENCIL_COPY_INVALID] case=%s stage=%s result=invalid-footprint\n",
	            label, rejectionStage);
	return true;
}

bool BufferIsReset(const HostBuffer& buffer, VkDeviceSize payloadBytes, const char* label)
{
	if (!VerifyGuards(buffer, payloadBytes, label)) return false;
	const auto* bytes = static_cast<const uint8_t*>(buffer.mapped);
	for (VkDeviceSize offset = 0; offset < payloadBytes; ++offset)
	{
		if (bytes[kGuardBytes + offset] == kReadbackUnsetByte) continue;
		std::fprintf(stderr, "%s changed at payload byte %llu\n", label,
		             static_cast<unsigned long long>(offset));
		return false;
	}
	return true;
}

int Initialize(ProbeState* state)
{
	VkApplicationInfo applicationInfo {};
	applicationInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName = "Lumen D32S8 buffer/image copy probe";
	applicationInfo.pEngineName      = "Lumen";
	applicationInfo.apiVersion       = VK_API_VERSION_1_1;
	VkInstanceCreateInfo instanceInfo {};
	instanceInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &applicationInfo;
	VkResult result = vkCreateInstance(&instanceInfo, nullptr, &state->instance);
	if (result != VK_SUCCESS) return Fail("vkCreateInstance failed", result);

	uint32_t physicalDeviceCount = 0;
	result = vkEnumeratePhysicalDevices(state->instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0)
		return Fail("No Native Metal physical device found", result);
	std::array<VkPhysicalDevice, 8> physicalDevices {};
	if (physicalDeviceCount > physicalDevices.size())
		return Fail("Native Metal physical-device count exceeds probe capacity");
	result = vkEnumeratePhysicalDevices(state->instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS) return Fail("vkEnumeratePhysicalDevices(list) failed", result);
	for (uint32_t deviceIndex = 0; deviceIndex < physicalDeviceCount; ++deviceIndex)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[deviceIndex], &familyCount, nullptr);
		std::array<VkQueueFamilyProperties, 8> families {};
		if (familyCount > families.size()) continue;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[deviceIndex], &familyCount,
		                                         families.data());
		for (uint32_t family = 0; family < familyCount; ++family)
		{
			if (families[family].queueCount != 0 &&
			    (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
			{
				state->physicalDevice = physicalDevices[deviceIndex];
				state->queueFamily    = family;
				break;
			}
		}
		if (state->physicalDevice != VK_NULL_HANDLE) break;
	}
	if (state->physicalDevice == VK_NULL_HANDLE)
		return Fail("No graphics-capable Native Metal queue found");

	VkFormatProperties formatProperties {};
	vkGetPhysicalDeviceFormatProperties(state->physicalDevice, VK_FORMAT_D32_SFLOAT_S8_UINT,
	                                    &formatProperties);
	constexpr VkFormatFeatureFlags requiredFeatures =
	    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if ((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
		return Fail("D32_SFLOAT_S8_UINT lacks transfer source/destination or attachment support");

	constexpr float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = state->queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &priority;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount    = 1;
	deviceInfo.pQueueCreateInfos       = &queueInfo;
	result = vkCreateDevice(state->physicalDevice, &deviceInfo, nullptr, &state->device);
	if (result != VK_SUCCESS) return Fail("vkCreateDevice failed", result);
	vkGetDeviceQueue(state->device, state->queueFamily, 0, &state->queue);
	if (state->queue == VK_NULL_HANDLE) return Fail("vkGetDeviceQueue returned null");

	result = CreateHostBuffer(*state, kDepthBufferBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                          &state->depthUpload);
	if (result != VK_SUCCESS) return Fail("CreateHostBuffer(depth upload) failed", result);
	result = CreateHostBuffer(*state, kStencilBufferBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                          &state->stencilUpload);
	if (result != VK_SUCCESS) return Fail("CreateHostBuffer(stencil upload) failed", result);
	result = CreateHostBuffer(*state, kDepthBufferBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                          &state->depthReadback);
	if (result != VK_SUCCESS) return Fail("CreateHostBuffer(depth readback) failed", result);
	result = CreateHostBuffer(*state, kStencilBufferBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                          &state->stencilReadback);
	if (result != VK_SUCCESS) return Fail("CreateHostBuffer(stencil readback) failed", result);

	VkImageCreateInfo imageInfo {};
	imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType     = VK_IMAGE_TYPE_2D;
	imageInfo.format        = VK_FORMAT_D32_SFLOAT_S8_UINT;
	imageInfo.extent        = {kWidth, kHeight, 1};
	imageInfo.mipLevels     = 1;
	imageInfo.arrayLayers   = kLayers;
	imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(state->device, &imageInfo, nullptr, &state->image);
	if (result != VK_SUCCESS) return Fail("vkCreateImage(D32S8) failed", result);
	VkMemoryRequirements imageRequirements {};
	vkGetImageMemoryRequirements(state->device, state->image, &imageRequirements);
	const uint32_t imageMemoryType = FindMemoryType(state->physicalDevice,
	                                                imageRequirements.memoryTypeBits,
	                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (imageMemoryType == UINT32_MAX) return Fail("No device-local memory type for D32S8 image");
	VkMemoryAllocateInfo imageAllocation {};
	imageAllocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	imageAllocation.allocationSize  = imageRequirements.size;
	imageAllocation.memoryTypeIndex = imageMemoryType;
	result = vkAllocateMemory(state->device, &imageAllocation, nullptr, &state->imageMemory);
	if (result != VK_SUCCESS) return Fail("vkAllocateMemory(D32S8) failed", result);
	result = vkBindImageMemory(state->device, state->image, state->imageMemory, 0);
	if (result != VK_SUCCESS) return Fail("vkBindImageMemory(D32S8) failed", result);

	VkCommandPoolCreateInfo poolInfo {};
	poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = state->queueFamily;
	result = vkCreateCommandPool(state->device, &poolInfo, nullptr, &state->commandPool);
	if (result != VK_SUCCESS) return Fail("vkCreateCommandPool failed", result);
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(state->device, &fenceInfo, nullptr, &state->fence);
	if (result != VK_SUCCESS) return Fail("vkCreateFence failed", result);
	return 0;
}
} // namespace

int main()
{
	using Lumen::Libs::Graphics::DepthStencilZeroInit;
	using Lumen::Libs::Graphics::DepthStencilZeroReadback;
	DepthStencilZeroReadback::Layout readback_layout;
	if (!DepthStencilZeroReadback::ResolveLayout(kWidth, kHeight, &readback_layout) ||
	    readback_layout.depth_pitch != 64 || readback_layout.stencil_pitch != 256 ||
	    readback_layout.depth_offset != 256 || readback_layout.stencil_offset != 1280 || readback_layout.bytes != 2304)
		return Fail("Uniform-zero host readback layout validation failed");
	const auto saved_layout = readback_layout;
	for (const auto extent: {std::array<uint32_t, 2>{0, kHeight}, {kWidth, 0}, {UINT32_MAX, UINT32_MAX},
	                        {4194305u, 1u}, {1u, 4194304u}})
	{
		if (DepthStencilZeroReadback::ResolveLayout(extent[0], extent[1], &readback_layout) ||
		    std::memcmp(&readback_layout, &saved_layout, sizeof(readback_layout)) != 0)
			return Fail("Invalid host readback footprint was accepted or mutated output");
	}
	if (!DepthStencilZeroInit::Supports(VK_FORMAT_D32_SFLOAT_S8_UINT, kWidth, kHeight) ||
	    DepthStencilZeroInit::Supports(VK_FORMAT_D32_SFLOAT, kWidth, kHeight) ||
	    DepthStencilZeroInit::Supports(VK_FORMAT_S8_UINT, kWidth, kHeight) ||
	    DepthStencilZeroInit::Supports(VK_FORMAT_D32_SFLOAT_S8_UINT, 0, kHeight) ||
	    DepthStencilZeroInit::Supports(VK_FORMAT_D32_SFLOAT_S8_UINT, kWidth, 0))
		return Fail("Uniform-zero host initialization profile validation failed");
	DepthStencilZeroInit invalid;
	if (invalid.Create(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_FORMAT_D32_SFLOAT_S8_UINT, kWidth, kHeight) == VK_SUCCESS ||
	    invalid.Record(VK_NULL_HANDLE, VK_NULL_HANDLE)) return Fail("Invalid uniform-zero initialization was accepted");
	ProbeState state;
	if (Initialize(&state) != 0) return 1;
	InitializeFixtures(&state);
	for (const HostBuffer* buffer: {&state.depthUpload, &state.stencilUpload,
	                               &state.depthReadback, &state.stencilReadback})
	{
		const VkResult result = FlushHostBuffer(state, *buffer);
		if (result != VK_SUCCESS) return Fail("vkFlushMappedMemoryRanges failed", result);
	}

	VkResult result = RoundTrip(&state, VK_IMAGE_LAYOUT_UNDEFINED, true);
	if (result != VK_SUCCESS) return Fail("Initial depth/stencil round trip failed", result);
	result = InvalidateHostBuffer(state, state.depthReadback);
	if (result != VK_SUCCESS) return Fail("Invalidate depth readback failed", result);
	result = InvalidateHostBuffer(state, state.stencilReadback);
	if (result != VK_SUCCESS) return Fail("Invalidate stencil readback failed", result);
	if (!VerifyGuards(state.depthUpload, kDepthPayloadBytes, "depth upload") ||
	    !VerifyGuards(state.stencilUpload, kStencilPayloadBytes, "stencil upload") ||
	    !VerifyDepthReadback(state.depthReadback) ||
	    !VerifyStencilReadback(state.stencilReadback))
	{
		return 2;
	}
	if (!VerifyFirstLayerZero(&state, "depth-nonzero") ||
	    !VerifyFirstLayerZero(&state, "depth-nonzero", VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) return 2;

	result = TransitionImage(&state, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
	                         VK_ACCESS_TRANSFER_WRITE_BIT);
	if (result != VK_SUCCESS) return Fail("Transition image to transfer destination failed", result);
	uint32_t invalidMasks = 0;
	if (ExpectAspectRejected(&state, CopyDirection::BufferToImage, 0,
	                         "buffer-to-image/aspect-zero"))
		++invalidMasks;
	if (ExpectAspectRejected(&state, CopyDirection::BufferToImage,
	                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
	                         "buffer-to-image/aspect-combined"))
		++invalidMasks;
	if (invalidMasks != 2) return 2;
	uint32_t invalidFootprints = 0;
	VkBufferImageCopy shortRow = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
	shortRow.bufferRowLength = kWidth - 1;
	if (ExpectFootprintRejected(&state, CopyDirection::BufferToImage, shortRow,
	                            "buffer-to-image/row-length-short"))
		++invalidFootprints;
	VkBufferImageCopy shortImage = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
	shortImage.bufferImageHeight = kHeight - 1;
	if (ExpectFootprintRejected(&state, CopyDirection::BufferToImage, shortImage,
	                            "buffer-to-image/image-height-short"))
		++invalidFootprints;
	VkBufferImageCopy nearEnd = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
	nearEnd.bufferOffset = state.depthUpload.size - kDepthBytesPerTexel;
	if (ExpectFootprintRejected(&state, CopyDirection::BufferToImage, nearEnd,
	                            "buffer-to-image/buffer-near-end"))
		++invalidFootprints;
	if (invalidFootprints != 3) return 2;

	result = TransitionImage(&state, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
	                         VK_ACCESS_TRANSFER_READ_BIT);
	if (result != VK_SUCCESS) return Fail("Transition image to transfer source failed", result);
	ResetReadback(&state.depthReadback, kDepthPayloadBytes);
	ResetReadback(&state.stencilReadback, kStencilPayloadBytes);
	result = FlushHostBuffer(state, state.depthReadback);
	if (result != VK_SUCCESS) return Fail("Flush reset depth readback failed", result);
	result = FlushHostBuffer(state, state.stencilReadback);
	if (result != VK_SUCCESS) return Fail("Flush reset stencil readback failed", result);
	if (ExpectAspectRejected(&state, CopyDirection::ImageToBuffer, 0,
	                         "image-to-buffer/aspect-zero"))
		++invalidMasks;
	if (!BufferIsReset(state.depthReadback, kDepthPayloadBytes,
	                   "image-to-buffer/aspect-zero destination"))
		return 2;
	if (ExpectAspectRejected(&state, CopyDirection::ImageToBuffer,
	                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
	                         "image-to-buffer/aspect-combined"))
		++invalidMasks;
	if (!BufferIsReset(state.depthReadback, kDepthPayloadBytes,
	                   "image-to-buffer/aspect-combined destination"))
		return 2;
	if (invalidMasks != 4) return 2;
	shortRow = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
	shortRow.bufferRowLength = kWidth - 1;
	if (ExpectFootprintRejected(&state, CopyDirection::ImageToBuffer, shortRow,
	                            "image-to-buffer/row-length-short"))
		++invalidFootprints;
	shortImage = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
	shortImage.bufferImageHeight = kHeight - 1;
	if (ExpectFootprintRejected(&state, CopyDirection::ImageToBuffer, shortImage,
	                            "image-to-buffer/image-height-short"))
		++invalidFootprints;
	nearEnd = MakeCopy(VK_IMAGE_ASPECT_DEPTH_BIT, true);
	nearEnd.bufferOffset = state.depthReadback.size - kDepthBytesPerTexel;
	if (ExpectFootprintRejected(&state, CopyDirection::ImageToBuffer, nearEnd,
	                            "image-to-buffer/buffer-near-end"))
		++invalidFootprints;
	if (!BufferIsReset(state.depthReadback, kDepthPayloadBytes,
	                   "image-to-buffer/invalid-footprint destination"))
		return 2;
	if (invalidFootprints != 6) return 2;

	result = RoundTrip(&state, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false);
	if (result != VK_SUCCESS) return Fail("Post-rejection depth/stencil readback failed", result);
	result = InvalidateHostBuffer(state, state.depthReadback);
	if (result != VK_SUCCESS) return Fail("Invalidate final depth readback failed", result);
	result = InvalidateHostBuffer(state, state.stencilReadback);
	if (result != VK_SUCCESS) return Fail("Invalidate final stencil readback failed", result);
	if (!VerifyDepthReadback(state.depthReadback) || !VerifyStencilReadback(state.stencilReadback))
		return 2;

	result = InitializeFirstLayerToZero(&state);
	if (result != VK_SUCCESS) return Fail("Uniform-zero depth/stencil initialization failed", result);
	ResetReadback(&state.depthReadback, kDepthPayloadBytes);
	ResetReadback(&state.stencilReadback, kStencilPayloadBytes);
	result = FlushHostBuffer(state, state.depthReadback);
	if (result != VK_SUCCESS) return Fail("Flush zero-init depth readback failed", result);
	result = FlushHostBuffer(state, state.stencilReadback);
	if (result != VK_SUCCESS) return Fail("Flush zero-init stencil readback failed", result);
	result = RoundTrip(&state, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false);
	if (result != VK_SUCCESS) return Fail("Uniform-zero depth/stencil readback failed", result);
	result = InvalidateHostBuffer(state, state.depthReadback);
	if (result != VK_SUCCESS) return Fail("Invalidate zero-init depth readback failed", result);
	result = InvalidateHostBuffer(state, state.stencilReadback);
	if (result != VK_SUCCESS) return Fail("Invalidate zero-init stencil readback failed", result);
	if (!VerifyDepthReadback(state.depthReadback, true) || !VerifyStencilReadback(state.stencilReadback, true) ||
	    !VerifyGuards(state.depthUpload, kDepthPayloadBytes, "zero-init depth upload") ||
	    !VerifyGuards(state.stencilUpload, kStencilPayloadBytes, "zero-init stencil upload")) return 2;
	std::printf("[GFX_DEPTH_STENCIL_ZERO_INIT_PROBE] format=D32_SFLOAT_S8_UINT depth_raw=+0 stencil=0"
	            " pixels=%u nonzero-fixture=verified other-layer=preserved padding=preserved status=clean\n", kWidth * kHeight);
	if (!VerifyFirstLayerZero(&state) || !VerifyFirstLayerZero(&state) ||
	    !VerifyFirstLayerZero(&state, nullptr, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) return 2;
	uint32_t nonzero_depth_cases = 0;
	for (const uint32_t word: {0x3f000000u, 0x80000000u, 0x7fc00000u, 0x7f800000u})
	{
		result = WriteLastFirstLayerPixel(&state, word, 0);
		if (result != VK_SUCCESS) return Fail("Write negative depth readback fixture failed", result);
		if (!VerifyFirstLayerZero(&state, "depth-nonzero") ||
		    !VerifyFirstLayerZero(&state, "depth-nonzero", VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) return 2;
		++nonzero_depth_cases;
	}
	result = WriteLastFirstLayerPixel(&state, 0, 1);
	if (result != VK_SUCCESS) return Fail("Write negative stencil readback fixture failed", result);
	if (!VerifyFirstLayerZero(&state, "stencil-nonzero") ||
	    !VerifyFirstLayerZero(&state, "stencil-nonzero", VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) return 2;
	result = WriteLastFirstLayerPixel(&state, 0, 0);
	if (result != VK_SUCCESS) return Fail("Restore final pixel to uniform zero failed", result);
	if (!VerifyFirstLayerZero(&state) ||
	    !VerifyFirstLayerZero(&state, nullptr, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)) return 2;
	ResetReadback(&state.depthReadback, kDepthPayloadBytes);
	ResetReadback(&state.stencilReadback, kStencilPayloadBytes);
	result = FlushHostBuffer(state, state.depthReadback);
	if (result != VK_SUCCESS) return Fail("Flush verifier depth readback failed", result);
	result = FlushHostBuffer(state, state.stencilReadback);
	if (result != VK_SUCCESS) return Fail("Flush verifier stencil readback failed", result);
	result = RoundTrip(&state, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false);
	if (result != VK_SUCCESS) return Fail("Final production-verifier preservation readback failed", result);
	result = InvalidateHostBuffer(state, state.depthReadback);
	if (result != VK_SUCCESS) return Fail("Invalidate verifier depth readback failed", result);
	result = InvalidateHostBuffer(state, state.stencilReadback);
	if (result != VK_SUCCESS) return Fail("Invalidate verifier stencil readback failed", result);
	if (!VerifyDepthReadback(state.depthReadback, true) || !VerifyStencilReadback(state.stencilReadback, true)) return 2;
	std::printf("[GFX_DEPTH_STENCIL_ZERO_READBACK_PROBE] format=D32_SFLOAT_S8_UINT pixels=%u"
	            " integral=verified depth_negative_cases=%u stencil_negative_cases=1 last-pixel=verified"
	            " original-layouts=attachment/read-only consecutive-read-only=verified invalid-layouts=4"
	            " layout-restored=verified other-layer=preserved padding=preserved status=clean\n",
	            kWidth * kHeight, nonzero_depth_cases);

	const int marker = std::printf(
	    "[GFX_DEPTH_STENCIL_COPY_VULKAN_PROBE] format=D32_SFLOAT_S8_UINT depth_bpt=4 "
	    "stencil_bpt=1 directions=2 invalid_masks=4 invalid_footprints=6 status=clean\n");
	if (marker <= 0 || std::fflush(stdout) != 0) return Fail("Failed to emit depth/stencil copy marker");
	return 0;
}
