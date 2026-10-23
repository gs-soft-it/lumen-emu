#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"
#include "emulator/include/Emulator/Graphics/TextureMipSnapshotCopy.h"
#include "emulator/include/Emulator/Graphics/TextureCommonRefreshCopy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr uint64_t kCompletionTimeoutNanoseconds = 5'000'000'000;
constexpr uint32_t kGuestWidth                   = 1230;
constexpr uint32_t kGuestHeight                  = 216;
constexpr uint32_t kS1Width                      = kGuestWidth >> 1u;
constexpr uint32_t kS1Height                     = kGuestHeight >> 1u;
constexpr uint32_t kS2Width                      = kGuestWidth >> 2u;
constexpr uint32_t kS2Height                     = kGuestHeight >> 2u;

constexpr uint32_t kSeedX      = 7;
constexpr uint32_t kSeedY      = 5;
constexpr uint32_t kS1WriteX   = 17;
constexpr uint32_t kS1WriteY   = 23;
constexpr uint32_t kS2WriteX   = 11;
constexpr uint32_t kS2WriteY   = 13;

constexpr VkImageUsageFlags kWindowUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT;

static_assert(kS1Width == 615 && kS1Height == 108);
static_assert(kS2Width == 307 && kS2Height == 54);
static_assert(kSeedX < kS2Width && kSeedY < kS2Height);
static_assert(kS1WriteX < kS1Width && kS1WriteY < kS1Height);
static_assert(kS2WriteX < kS2Width && kS2WriteY < kS2Height);

struct Pixel
{
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t alpha;
};

struct PushCoordinates
{
	int32_t source_x;
	int32_t source_y;
	int32_t destination_x;
	int32_t destination_y;
};

static_assert(sizeof(Pixel) == 4);
static_assert(sizeof(PushCoordinates) == 16);

constexpr Pixel kS1InitialPixel {0x11, 0x22, 0x33, 0x44};
constexpr Pixel kS2InitialPixel {0xa1, 0xb2, 0xc3, 0xd4};
constexpr Pixel kSeedPixel {0x55, 0x66, 0x77, 0x88};

// One pipeline is deliberately reused in both directions. Dispatch S1 samples
// the seed from S2 and writes S1. After an image-only GENERAL -> GENERAL
// dependency, dispatch S2 samples that newly-written S1 texel and writes S2.
constexpr char kComputeShaderAssembly[] = R"spirv(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %destination_image DescriptorSet 0
               OpDecorate %destination_image Binding 0
               OpDecorate %destination_image NonReadable
               OpDecorate %source_image DescriptorSet 0
               OpDecorate %source_image Binding 1
               OpMemberDecorate %coordinate_block 0 Offset 0
               OpMemberDecorate %coordinate_block 1 Offset 4
               OpMemberDecorate %coordinate_block 2 Offset 8
               OpMemberDecorate %coordinate_block 3 Offset 12
               OpDecorate %coordinate_block Block

       %void = OpTypeVoid
   %function = OpTypeFunction %void
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
      %float = OpTypeFloat 32
      %v2int = OpTypeVector %int 2
    %v4float = OpTypeVector %float 4
%destination_image_type = OpTypeImage %float 2D 0 0 0 2 Rgba8
%source_image_type = OpTypeImage %float 2D 0 0 0 1 Unknown
%ptr_uniform_constant_destination = OpTypePointer UniformConstant %destination_image_type
%ptr_uniform_constant_source = OpTypePointer UniformConstant %source_image_type
%destination_image = OpVariable %ptr_uniform_constant_destination UniformConstant
%source_image = OpVariable %ptr_uniform_constant_source UniformConstant
%coordinate_block = OpTypeStruct %int %int %int %int
%ptr_push_coordinate_block = OpTypePointer PushConstant %coordinate_block
%ptr_push_int = OpTypePointer PushConstant %int
%coordinates = OpVariable %ptr_push_coordinate_block PushConstant

     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
      %int_0 = OpConstant %int 0

       %main = OpFunction %void None %function
      %entry = OpLabel
%source_x_ptr = OpAccessChain %ptr_push_int %coordinates %uint_0
%source_y_ptr = OpAccessChain %ptr_push_int %coordinates %uint_1
%destination_x_ptr = OpAccessChain %ptr_push_int %coordinates %uint_2
%destination_y_ptr = OpAccessChain %ptr_push_int %coordinates %uint_3
   %source_x = OpLoad %int %source_x_ptr
   %source_y = OpLoad %int %source_y_ptr
%destination_x = OpLoad %int %destination_x_ptr
%destination_y = OpLoad %int %destination_y_ptr
%source_coord = OpCompositeConstruct %v2int %source_x %source_y
%destination_coord = OpCompositeConstruct %v2int %destination_x %destination_y
%source = OpLoad %source_image_type %source_image
      %color = OpImageFetch %v4float %source %source_coord Lod %int_0
%destination = OpLoad %destination_image_type %destination_image
               OpImageWrite %destination %destination_coord %color
               OpReturn
               OpFunctionEnd
)spirv";

struct ProbeState
{
	VkInstance            instance            = VK_NULL_HANDLE;
	VkDevice              device              = VK_NULL_HANDLE;
	VkQueue               queue               = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool      descriptorPool      = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, 2> descriptorSets {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkPipelineLayout      pipelineLayout      = VK_NULL_HANDLE;
	VkShaderModule        shaderModule        = VK_NULL_HANDLE;
	VkPipeline            pipeline            = VK_NULL_HANDLE;
	std::array<VkImage, 2> images {VK_NULL_HANDLE, VK_NULL_HANDLE};
	std::array<VkDeviceMemory, 2> imageMemories {VK_NULL_HANDLE, VK_NULL_HANDLE};
	std::array<VkImageView, 2> imageViews {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkBuffer              uploadBuffer        = VK_NULL_HANDLE;
	VkDeviceMemory        uploadMemory        = VK_NULL_HANDLE;
	void*                 mappedUpload        = nullptr;
	VkBuffer              readbackBuffer      = VK_NULL_HANDLE;
	VkDeviceMemory        readbackMemory      = VK_NULL_HANDLE;
	void*                 mappedReadback      = nullptr;
	VkCommandPool         commandPool         = VK_NULL_HANDLE;
	VkCommandBuffer       commandBuffer       = VK_NULL_HANDLE;
	VkFence               fence               = VK_NULL_HANDLE;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(device);
		}
		if (mappedUpload != nullptr)
		{
			vkUnmapMemory(device, uploadMemory);
		}
		if (mappedReadback != nullptr)
		{
			vkUnmapMemory(device, readbackMemory);
		}
		if (fence != VK_NULL_HANDLE)
		{
			vkDestroyFence(device, fence, nullptr);
		}
		if (commandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(device, commandPool, nullptr);
		}
		if (pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, pipeline, nullptr);
		}
		if (shaderModule != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
		}
		if (pipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		}
		if (descriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		}
		if (descriptorSetLayout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		}
		for (VkImageView view: imageViews)
		{
			if (view != VK_NULL_HANDLE)
			{
				vkDestroyImageView(device, view, nullptr);
			}
		}
		for (size_t index = 0; index < images.size(); ++index)
		{
			if (images[index] != VK_NULL_HANDLE)
			{
				vkDestroyImage(device, images[index], nullptr);
			}
			if (imageMemories[index] != VK_NULL_HANDLE)
			{
				vkFreeMemory(device, imageMemories[index], nullptr);
			}
		}
		if (readbackBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, readbackBuffer, nullptr);
		}
		if (readbackMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, readbackMemory, nullptr);
		}
		if (uploadBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, uploadBuffer, nullptr);
		}
		if (uploadMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, uploadMemory, nullptr);
		}
		if (device != VK_NULL_HANDLE)
		{
			vkDestroyDevice(device, nullptr);
		}
		if (instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(instance, nullptr);
		}
	}
};

int Fail(const char* operation, VkResult result = VK_SUCCESS)
{
	if (result == VK_SUCCESS)
	{
		std::fprintf(stderr, "%s\n", operation);
	} else
	{
		std::fprintf(stderr, "%s: %d\n", operation, result);
	}
	return 1;
}

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((typeBits & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & required) == required)
		{
			return index;
		}
	}
	return UINT32_MAX;
}

VkResult CreateMappedBuffer(VkPhysicalDevice physicalDevice, VkDevice device, VkDeviceSize size,
                            VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped)
{
	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size        = size;
	bufferInfo.usage       = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult result        = vkCreateBuffer(device, &bufferInfo, nullptr, buffer);
	if (result != VK_SUCCESS)
	{
		return result;
	}

	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(device, *buffer, &requirements);
	const uint32_t memoryType = FindMemoryType(physicalDevice, requirements.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result                         = vkAllocateMemory(device, &allocationInfo, nullptr, memory);
	if (result != VK_SUCCESS)
	{
		return result;
	}
	result = vkBindBufferMemory(device, *buffer, *memory, 0);
	if (result != VK_SUCCESS)
	{
		return result;
	}
	return vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
}

VkResult CreateDeviceLocalImage(VkPhysicalDevice physicalDevice, VkDevice device, uint32_t width, uint32_t height,
                                VkImage* image, VkDeviceMemory* memory, uint32_t levels = 1,
                                VkImageUsageFlags usage = kWindowUsage)
{
	VkImageCreateInfo imageInfo {};
	imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType     = VK_IMAGE_TYPE_2D;
	imageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.extent        = {width, height, 1};
	imageInfo.mipLevels     = levels;
	imageInfo.arrayLayers   = 1;
	imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage         = usage;
	imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkResult result         = vkCreateImage(device, &imageInfo, nullptr, image);
	if (result != VK_SUCCESS)
	{
		return result;
	}

	VkMemoryRequirements requirements {};
	vkGetImageMemoryRequirements(device, *image, &requirements);
	const uint32_t memoryType =
	    FindMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (memoryType == UINT32_MAX)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result                         = vkAllocateMemory(device, &allocationInfo, nullptr, memory);
	if (result != VK_SUCCESS)
	{
		return result;
	}
	return vkBindImageMemory(device, *image, *memory, 0);
}

VkImageSubresourceRange ColorRange()
{
	VkImageSubresourceRange range {};
	range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel   = 0;
	range.levelCount     = 1;
	range.baseArrayLayer = 0;
	range.layerCount     = 1;
	return range;
}

struct MipSnapshotProbe
{
	VkDevice device;
	std::array<VkImage, 4> images {};
	std::array<VkDeviceMemory, 4> memories {};
	VkBuffer upload = VK_NULL_HANDLE;
	VkBuffer readback = VK_NULL_HANDLE;
	VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
	VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
	void* uploadData = nullptr;
	void* readbackData = nullptr;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	~MipSnapshotProbe()
	{
		vkDeviceWaitIdle(device);
		if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
		if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (uploadData != nullptr) vkUnmapMemory(device, uploadMemory);
		if (readbackData != nullptr) vkUnmapMemory(device, readbackMemory);
		if (upload != VK_NULL_HANDLE) vkDestroyBuffer(device, upload, nullptr);
		if (readback != VK_NULL_HANDLE) vkDestroyBuffer(device, readback, nullptr);
		if (uploadMemory != VK_NULL_HANDLE) vkFreeMemory(device, uploadMemory, nullptr);
		if (readbackMemory != VK_NULL_HANDLE) vkFreeMemory(device, readbackMemory, nullptr);
		for (size_t index = 0; index < images.size(); ++index)
		{
			if (images[index] != VK_NULL_HANDLE) vkDestroyImage(device, images[index], nullptr);
			if (memories[index] != VK_NULL_HANDLE) vkFreeMemory(device, memories[index], nullptr);
		}
	}
};

int VerifyMipSnapshotCopy(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, uint32_t queueFamily)
{
	MipSnapshotProbe state {device};
	constexpr std::array<VkExtent2D, 3> extents {{{5, 3}, {2, 1}, {1, 1}}};
	constexpr VkDeviceSize slotBytes = 256;
	constexpr VkDeviceSize uploadBytes = 3 * slotBytes;
	constexpr VkDeviceSize readbackBytes = 6 * slotBytes;
	for (size_t index = 0; index < state.images.size(); ++index)
	{
		const auto extent = extents[index == 3 ? 0 : index];
		const auto result = CreateDeviceLocalImage(physicalDevice, device, extent.width, extent.height,
		                                           &state.images[index], &state.memories[index], index == 3 ? 3 : 1);
		if (result != VK_SUCCESS) return Fail("Mip snapshot image allocation failed", result);
	}
	auto result = CreateMappedBuffer(physicalDevice, device, uploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                                  &state.upload, &state.uploadMemory, &state.uploadData);
	if (result != VK_SUCCESS) return Fail("Mip snapshot upload allocation failed", result);
	result = CreateMappedBuffer(physicalDevice, device, readbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                             &state.readback, &state.readbackMemory, &state.readbackData);
	if (result != VK_SUCCESS) return Fail("Mip snapshot readback allocation failed", result);
	std::memset(state.uploadData, 0xab, uploadBytes);
	std::memset(state.readbackData, 0xa5, readbackBytes);
	for (uint32_t level = 0; level < 3; ++level)
	{
		auto* pixels = reinterpret_cast<Pixel*>(static_cast<uint8_t*>(state.uploadData) + level * slotBytes);
		for (uint32_t y = 0; y < extents[level].height; ++y)
		{
			for (uint32_t x = 0; x < extents[level].width; ++x)
			{
				pixels[y * extents[level].width + x] = {static_cast<uint8_t>(0x10 + level),
				                                       static_cast<uint8_t>(0x20 + x),
				                                       static_cast<uint8_t>(0x30 + y), 0xff};
			}
		}
	}
	VkMappedMemoryRange flush {};
	flush.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	flush.memory = state.uploadMemory;
	flush.size = VK_WHOLE_SIZE;
	result = vkFlushMappedMemoryRanges(device, 1, &flush);
	if (result != VK_SUCCESS) return Fail("Mip snapshot upload flush failed", result);
	VkCommandPoolCreateInfo poolInfo {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(device, &poolInfo, nullptr, &state.pool);
	if (result != VK_SUCCESS) return Fail("Mip snapshot command pool failed", result);
	std::array<VkCommandBuffer, 2> commands {};
	VkCommandBufferAllocateInfo allocate {};
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = state.pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = commands.size();
	result = vkAllocateCommandBuffers(device, &allocate, commands.data());
	if (result != VK_SUCCESS) return Fail("Mip snapshot command buffers failed", result);
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS) return Fail("Mip snapshot fence failed", result);
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(commands[0], &begin);
	if (result != VK_SUCCESS) return Fail("Mip producer begin failed", result);
	VkBufferMemoryBarrier hostToUpload {};
	hostToUpload.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	hostToUpload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	hostToUpload.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	hostToUpload.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostToUpload.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostToUpload.buffer = state.upload;
	hostToUpload.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(commands[0], VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 1, &hostToUpload, 0, nullptr);
	std::array<VkImageMemoryBarrier, 3> sourceBarriers {};
	std::array<Lumen::Libs::Graphics::TextureMipSnapshotCopySource, 3> sources {};
	for (uint32_t level = 0; level < 3; ++level)
	{
		auto& barrier = sourceBarriers[level];
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = state.images[level];
		barrier.subresourceRange = ColorRange();
		sources[level] = {state.images[level], extents[level], level == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
		                                                               : VK_IMAGE_LAYOUT_GENERAL};
	}
	vkCmdPipelineBarrier(commands[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, sourceBarriers.size(), sourceBarriers.data());
	for (uint32_t level = 0; level < 3; ++level)
	{
		VkBufferImageCopy copy {};
		copy.bufferOffset = level * slotBytes;
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {extents[level].width, extents[level].height, 1};
		vkCmdCopyBufferToImage(commands[0], state.upload, state.images[level], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		sourceBarriers[level].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		sourceBarriers[level].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		sourceBarriers[level].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		sourceBarriers[level].newLayout = sources[level].layout;
	}
	vkCmdPipelineBarrier(commands[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                     0, 0, nullptr, 0, nullptr, sourceBarriers.size(), sourceBarriers.data());
	result = vkEndCommandBuffer(commands[0]);
	if (result != VK_SUCCESS) return Fail("Mip producer end failed", result);
	VkSubmitInfo submit {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &commands[0];
	result = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	if (result != VK_SUCCESS) return Fail("Mip producer submit failed", result);
	// Deliberately do not wait on the producer. The runtime emitter supplies the
	// dependency between two submissions to this same queue.
	result = vkBeginCommandBuffer(commands[1], &begin);
	if (result != VK_SUCCESS) return Fail("Mip snapshot begin failed", result);
	if (!Lumen::Libs::Graphics::RecordTextureMipSnapshotCopy(commands[1], state.images[3], sources.data(), sources.size()))
	{
		return Fail("Mip snapshot emitter rejected valid source plan");
	}
	std::array<VkImageMemoryBarrier, 4> readBarriers {};
	for (uint32_t image = 0; image < 4; ++image)
	{
		auto& barrier = readBarriers[image];
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = image == 3 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : sources[image].layout;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = state.images[image];
		barrier.subresourceRange = ColorRange();
		barrier.subresourceRange.levelCount = image == 3 ? 3 : 1;
	}
	vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, readBarriers.size(), readBarriers.data());
	for (uint32_t level = 0; level < 3; ++level)
	{
		VkBufferImageCopy copy {};
		copy.bufferOffset = level * slotBytes;
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {extents[level].width, extents[level].height, 1};
		vkCmdCopyImageToBuffer(commands[1], state.images[level], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.readback, 1, &copy);
		copy.bufferOffset = (level + 3) * slotBytes;
		copy.imageSubresource.mipLevel = level;
		vkCmdCopyImageToBuffer(commands[1], state.images[3], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.readback, 1, &copy);
	}
	VkBufferMemoryBarrier toHost = hostToUpload;
	toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	toHost.buffer = state.readback;
	vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 0, nullptr, 1, &toHost, 0, nullptr);
	result = vkEndCommandBuffer(commands[1]);
	if (result != VK_SUCCESS) return Fail("Mip snapshot end failed", result);
	submit.pCommandBuffers = &commands[1];
	result = vkQueueSubmit(queue, 1, &submit, state.fence);
	if (result != VK_SUCCESS) return Fail("Mip snapshot submit failed", result);
	result = vkWaitForFences(device, 1, &state.fence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS) return Fail("Mip snapshot wait failed", result);
	flush.memory = state.readbackMemory;
	result = vkInvalidateMappedMemoryRanges(device, 1, &flush);
	if (result != VK_SUCCESS) return Fail("Mip snapshot invalidate failed", result);
	for (uint32_t slot = 0; slot < 6; ++slot)
	{
		const uint32_t level = slot % 3;
		const size_t bytes = extents[level].width * extents[level].height * sizeof(Pixel);
		const auto* expected = static_cast<const uint8_t*>(state.uploadData) + level * slotBytes;
		const auto* observed = static_cast<const uint8_t*>(state.readbackData) + slot * slotBytes;
		if (std::memcmp(expected, observed, bytes) != 0) return Fail("Mip snapshot source/destination bytes differ");
		for (size_t index = bytes; index < slotBytes; ++index)
		{
			if (observed[index] != 0xa5) return Fail("Mip snapshot readback canary modified");
		}
	}
	std::printf("[GFX_TEXTURE_MIP_SNAPSHOT_COPY] backend=native-metal levels=3 extents=5x3,2x1,1x1 "
	            "producer-wait=none source-bytes=verified destination-bytes=verified canary=verified status=clean\n");
	return 0;
}

VkResult CreateImageView(VkDevice device, VkImage image, VkImageView* view)
{
	VkImageViewCreateInfo viewInfo {};
	viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image            = image;
	viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.subresourceRange = ColorRange();
	return vkCreateImageView(device, &viewInfo, nullptr, view);
}

int VerifyRenderTargetSampledRefresh(VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, uint32_t queueFamily)
{
	MipSnapshotProbe state {device};
	struct Attachment
	{
		VkDevice device;
		VkImageView view = VK_NULL_HANDLE;
		VkRenderPass pass = VK_NULL_HANDLE;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		~Attachment()
		{
			vkDeviceWaitIdle(device);
			if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
			if (pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, pass, nullptr);
			if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
		}
	} attachment {device};
	constexpr uint32_t width = 5, height = 3;
	constexpr VkDeviceSize slotBytes = 256, prefix = 16, bytes = 4 * slotBytes;
	for (unsigned index = 0; index < 2; ++index)
	{
		const auto result = CreateDeviceLocalImage(physicalDevice, device, width, height, &state.images[index],
		    &state.memories[index], 1, kWindowUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		if (result != VK_SUCCESS) return Fail("Render sampled image allocation failed", result);
	}
	auto result = CreateImageView(device, state.images[0], &attachment.view);
	if (result != VK_SUCCESS) return Fail("Render sampled attachment view failed", result);
	VkAttachmentDescription color {};
	color.format = VK_FORMAT_R8G8B8A8_UNORM;
	color.samples = VK_SAMPLE_COUNT_1_BIT;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference reference {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	VkRenderPassCreateInfo passInfo {};
	passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	passInfo.attachmentCount = 1;
	passInfo.pAttachments = &color;
	passInfo.subpassCount = 1;
	passInfo.pSubpasses = &subpass;
	result = vkCreateRenderPass(device, &passInfo, nullptr, &attachment.pass);
	if (result != VK_SUCCESS) return Fail("Render sampled pass failed", result);
	VkFramebufferCreateInfo framebufferInfo {};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = attachment.pass;
	framebufferInfo.attachmentCount = 1;
	framebufferInfo.pAttachments = &attachment.view;
	framebufferInfo.width = width;
	framebufferInfo.height = height;
	framebufferInfo.layers = 1;
	result = vkCreateFramebuffer(device, &framebufferInfo, nullptr, &attachment.framebuffer);
	if (result != VK_SUCCESS) return Fail("Render sampled framebuffer failed", result);
	result = CreateMappedBuffer(physicalDevice, device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    &state.readback, &state.readbackMemory, &state.readbackData);
	if (result != VK_SUCCESS) return Fail("Render sampled readback failed", result);
	std::memset(state.readbackData, 0xa5, bytes);
	VkMappedMemoryRange mapped {};
	mapped.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	mapped.memory = state.readbackMemory;
	mapped.size = VK_WHOLE_SIZE;
	result = vkFlushMappedMemoryRanges(device, 1, &mapped);
	if (result != VK_SUCCESS) return Fail("Render sampled canary flush failed", result);
	VkCommandPoolCreateInfo poolInfo {};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(device, &poolInfo, nullptr, &state.pool);
	if (result != VK_SUCCESS) return Fail("Render sampled command pool failed", result);
	std::array<VkCommandBuffer, 2> commands {};
	VkCommandBufferAllocateInfo allocate {};
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = state.pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = commands.size();
	result = vkAllocateCommandBuffers(device, &allocate, commands.data());
	if (result != VK_SUCCESS) return Fail("Render sampled command buffers failed", result);
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS) return Fail("Render sampled fence failed", result);
	const auto transition = [](VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after) {
		VkImageMemoryBarrier barrier {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.oldLayout = before;
		barrier.newLayout = after;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange = ColorRange();
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		    0, 0, nullptr, 0, nullptr, 1, &barrier);
	};
	for (unsigned epoch = 0; epoch < commands.size(); ++epoch)
	{
		const auto command = commands[epoch];
		VkCommandBufferBeginInfo begin {};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		result = vkBeginCommandBuffer(command, &begin);
		if (result != VK_SUCCESS) return Fail("Render sampled recording failed", result);
		if (epoch == 0)
		{
			transition(command, state.images[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			transition(command, state.images[1], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		VkClearValue clear {};
		clear.color.float32[epoch] = 1.0f;
		clear.color.float32[3] = 1.0f;
		VkRenderPassBeginInfo passBegin {};
		passBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		passBegin.renderPass = attachment.pass;
		passBegin.framebuffer = attachment.framebuffer;
		passBegin.renderArea.extent = {width, height};
		passBegin.clearValueCount = 1;
		passBegin.pClearValues = &clear;
		vkCmdBeginRenderPass(command, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(command);
		const auto sourceLayout = epoch == 0 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if (epoch != 0) transition(command, state.images[0], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, sourceLayout);
		if (!Lumen::Libs::Graphics::RecordRenderTargetSampledRefresh(command, state.images[0], sourceLayout,
		    state.images[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, width, height)) return Fail("Render sampled emitter rejected");
		for (unsigned index = 0; index < 2; ++index)
		{
			const auto layout = index == 0 ? sourceLayout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			transition(command, state.images[index], layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			VkBufferImageCopy copy {};
			copy.bufferOffset = (epoch * 2 + index) * slotBytes + prefix;
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageExtent = {width, height, 1};
			vkCmdCopyImageToBuffer(command, state.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.readback, 1, &copy);
			transition(command, state.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
		}
		VkBufferMemoryBarrier toHost {};
		toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toHost.buffer = state.readback;
		toHost.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0, nullptr);
		result = vkEndCommandBuffer(command);
		if (result != VK_SUCCESS) return Fail("Render sampled end recording failed", result);
		VkSubmitInfo submit {};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		result = vkQueueSubmit(queue, 1, &submit, epoch == 1 ? state.fence : VK_NULL_HANDLE);
		if (result != VK_SUCCESS) return Fail("Render sampled submit failed", result);
	}
	result = vkWaitForFences(device, 1, &state.fence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS) return Fail("Render sampled completion failed", result);
	result = vkInvalidateMappedMemoryRanges(device, 1, &mapped);
	if (result != VK_SUCCESS) return Fail("Render sampled invalidate failed", result);
	for (unsigned slot = 0; slot < 4; ++slot)
	{
		const Pixel expected = slot < 2 ? Pixel {255, 0, 0, 255} : Pixel {0, 255, 0, 255};
		const auto* observed = static_cast<const uint8_t*>(state.readbackData) + slot * slotBytes;
		for (size_t byte = 0; byte < slotBytes; ++byte)
		{
			const uint8_t value = byte >= prefix && byte < prefix + width * height * sizeof(Pixel)
			    ? reinterpret_cast<const uint8_t*>(&expected)[(byte - prefix) % sizeof(Pixel)] : 0xa5;
			if (observed[byte] != value) return Fail("Render sampled pixel/canary mismatch");
		}
	}
	std::printf("[GFX_RENDER_TARGET_SAMPLED_REFRESH_PROBE] epochs=2 actual-color-attachment=1 source=verified sampled=verified "
	    "layouts=color-attachment+shader-read restored=1 canary=clean queue=single no-intermediate-wait=1 status=clean\n");
	return 0;
}

size_t CountToken(std::string_view text, std::string_view token)
{
	size_t count    = 0;
	size_t position = 0;
	while ((position = text.find(token, position)) != std::string_view::npos)
	{
		++count;
		position += token.size();
	}
	return count;
}

bool HasSingleWindowChainShape(std::string_view disassembly)
{
	return CountToken(disassembly, "OpImageFetch") == 1 && CountToken(disassembly, "OpImageWrite") == 1 &&
	       CountToken(disassembly, "OpImageRead") == 0 && CountToken(disassembly, "PushConstant") >= 2;
}

bool SamePixel(const Pixel& lhs, const Pixel& rhs)
{
	return std::memcmp(&lhs, &rhs, sizeof(Pixel)) == 0;
}

bool VerifyImage(const Pixel* observed, const std::vector<Pixel>& expected, const char* imageName)
{
	size_t mismatchCount = 0;
	for (size_t index = 0; index < expected.size(); ++index)
	{
		if (!SamePixel(observed[index], expected[index]))
		{
			if (mismatchCount < 8)
			{
				std::fprintf(stderr,
				             "Storage single-window mismatch: image=%s index=%zu actual=%02x%02x%02x%02x "
				             "expected=%02x%02x%02x%02x\n",
				             imageName, index, observed[index].red, observed[index].green, observed[index].blue,
				             observed[index].alpha, expected[index].red, expected[index].green, expected[index].blue,
				             expected[index].alpha);
			}
			++mismatchCount;
		}
	}
	if (mismatchCount != 0)
	{
		std::fprintf(stderr, "Storage single-window verification failed: image=%s mismatches=%zu\n", imageName,
		             mismatchCount);
		return false;
	}
	return true;
}
} // namespace

int main()
{
	spvtools::SpirvTools spirvTools(SPV_ENV_VULKAN_1_1);
	spirvTools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                              { std::fprintf(stderr, "SPIR-V line %zu column %zu: %s\n", position.line, position.column, message); });

	std::vector<uint32_t> shaderBinary;
	if (!spirvTools.Assemble(kComputeShaderAssembly, sizeof(kComputeShaderAssembly) - 1, &shaderBinary))
	{
		return Fail("Storage single-window SPIR-V assembly failed");
	}
	if (!spirvTools.Validate(shaderBinary))
	{
		return Fail("Storage single-window SPIR-V validation failed");
	}
	std::string disassembly;
	if (!spirvTools.Disassemble(shaderBinary, &disassembly) || !HasSingleWindowChainShape(disassembly))
	{
		return Fail("Storage single-window SPIR-V lost its sample/write shape");
	}

	ProbeState state;
	VkApplicationInfo applicationInfo {};
	applicationInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName   = "Lumen storage single-window compute probe";
	applicationInfo.applicationVersion = 1;
	applicationInfo.pEngineName        = "Lumen";
	applicationInfo.engineVersion      = 1;
	applicationInfo.apiVersion         = VK_API_VERSION_1_1;
	VkInstanceCreateInfo instanceInfo {};
	instanceInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &applicationInfo;
	VkResult result               = vkCreateInstance(&instanceInfo, nullptr, &state.instance);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateInstance failed", result);
	}

	uint32_t physicalDeviceCount = 0;
	result                       = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0)
	{
		return Fail("No NativeMetal physical device found", result);
	}
	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS)
	{
		return Fail("vkEnumeratePhysicalDevices(list) failed", result);
	}
	physicalDevices.resize(physicalDeviceCount);

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t         queueFamily    = UINT32_MAX;
	for (VkPhysicalDevice candidate: physicalDevices)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
		for (uint32_t family = 0; family < familyCount; ++family)
		{
			if (families[family].queueCount != 0 && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
			{
				physicalDevice = candidate;
				queueFamily    = family;
				break;
			}
		}
		if (physicalDevice != VK_NULL_HANDLE)
		{
			break;
		}
	}
	if (physicalDevice == VK_NULL_HANDLE)
	{
		return Fail("No compute-capable NativeMetal queue found");
	}

	VkFormatProperties formatProperties {};
	vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R8G8B8A8_UNORM, &formatProperties);
	constexpr VkFormatFeatureFlags requiredFormatFeatures =
	    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
	    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	if ((formatProperties.optimalTilingFeatures & requiredFormatFeatures) != requiredFormatFeatures)
	{
		return Fail("NativeMetal RGBA8 lacks combined sampled/storage/transfer image support");
	}

	constexpr float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos    = &queueInfo;
	result                          = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &state.device);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDevice failed", result);
	}
	vkGetDeviceQueue(state.device, queueFamily, 0, &state.queue);
	if (state.queue == VK_NULL_HANDLE)
	{
		return Fail("vkGetDeviceQueue returned a null queue");
	}

	constexpr VkDeviceSize s1Bytes = static_cast<VkDeviceSize>(kS1Width) * kS1Height * sizeof(Pixel);
	constexpr VkDeviceSize s2Bytes = static_cast<VkDeviceSize>(kS2Width) * kS2Height * sizeof(Pixel);
	constexpr VkDeviceSize totalBytes = s1Bytes + s2Bytes;
	result = CreateMappedBuffer(physicalDevice, state.device, totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                            &state.uploadBuffer, &state.uploadMemory, &state.mappedUpload);
	if (result != VK_SUCCESS)
	{
		return Fail("CreateMappedBuffer(upload) failed", result);
	}
	result = CreateMappedBuffer(physicalDevice, state.device, totalBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                            &state.readbackBuffer, &state.readbackMemory, &state.mappedReadback);
	if (result != VK_SUCCESS)
	{
		return Fail("CreateMappedBuffer(readback) failed", result);
	}

	std::vector<Pixel> initialS1(static_cast<size_t>(kS1Width) * kS1Height, kS1InitialPixel);
	std::vector<Pixel> initialS2(static_cast<size_t>(kS2Width) * kS2Height, kS2InitialPixel);
	initialS2[static_cast<size_t>(kSeedY) * kS2Width + kSeedX] = kSeedPixel;
	auto* mappedUploadBytes = static_cast<uint8_t*>(state.mappedUpload);
	std::memcpy(mappedUploadBytes, initialS1.data(), static_cast<size_t>(s1Bytes));
	std::memcpy(mappedUploadBytes + s1Bytes, initialS2.data(), static_cast<size_t>(s2Bytes));
	std::memset(state.mappedReadback, 0xa5, static_cast<size_t>(totalBytes));

	VkMappedMemoryRange uploadRange {};
	uploadRange.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	uploadRange.memory = state.uploadMemory;
	uploadRange.size   = VK_WHOLE_SIZE;
	result             = vkFlushMappedMemoryRanges(state.device, 1, &uploadRange);
	if (result != VK_SUCCESS)
	{
		return Fail("vkFlushMappedMemoryRanges(upload) failed", result);
	}

	result = CreateDeviceLocalImage(physicalDevice, state.device, kS1Width, kS1Height, &state.images[0],
	                                &state.imageMemories[0]);
	if (result != VK_SUCCESS)
	{
		return Fail("CreateDeviceLocalImage(S1) failed", result);
	}
	result = CreateDeviceLocalImage(physicalDevice, state.device, kS2Width, kS2Height, &state.images[1],
	                                &state.imageMemories[1]);
	if (result != VK_SUCCESS)
	{
		return Fail("CreateDeviceLocalImage(S2) failed", result);
	}
	for (size_t index = 0; index < state.images.size(); ++index)
	{
		result = CreateImageView(state.device, state.images[index], &state.imageViews[index]);
		if (result != VK_SUCCESS)
		{
			return Fail(index == 0 ? "CreateImageView(S1) failed" : "CreateImageView(S2) failed", result);
		}
	}

	std::array<VkDescriptorSetLayoutBinding, 2> descriptorBindings {};
	descriptorBindings[0].binding         = 0;
	descriptorBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	descriptorBindings[0].descriptorCount = 1;
	descriptorBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	descriptorBindings[1].binding         = 1;
	descriptorBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	descriptorBindings[1].descriptorCount = 1;
	descriptorBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo {};
	descriptorLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptorLayoutInfo.bindingCount = static_cast<uint32_t>(descriptorBindings.size());
	descriptorLayoutInfo.pBindings    = descriptorBindings.data();
	result = vkCreateDescriptorSetLayout(state.device, &descriptorLayoutInfo, nullptr, &state.descriptorSetLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorSetLayout(single-window) failed", result);
	}

	VkPushConstantRange pushRange {};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset     = 0;
	pushRange.size       = sizeof(PushCoordinates);
	VkPipelineLayoutCreateInfo pipelineLayoutInfo {};
	pipelineLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount         = 1;
	pipelineLayoutInfo.pSetLayouts            = &state.descriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges    = &pushRange;
	result = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreatePipelineLayout(compute) failed", result);
	}

	std::array<VkDescriptorPoolSize, 2> poolSizes {};
	poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[0].descriptorCount = 2;
	poolSizes[1].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	poolSizes[1].descriptorCount = 2;
	VkDescriptorPoolCreateInfo poolInfo {};
	poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets       = 2;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes    = poolSizes.data();
	result                 = vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorPool(single-window) failed", result);
	}
	std::array<VkDescriptorSetLayout, 2> setLayouts {state.descriptorSetLayout, state.descriptorSetLayout};
	VkDescriptorSetAllocateInfo descriptorSetInfo {};
	descriptorSetInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetInfo.descriptorPool     = state.descriptorPool;
	descriptorSetInfo.descriptorSetCount = static_cast<uint32_t>(setLayouts.size());
	descriptorSetInfo.pSetLayouts        = setLayouts.data();
	result = vkAllocateDescriptorSets(state.device, &descriptorSetInfo, state.descriptorSets.data());
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateDescriptorSets(single-window) failed", result);
	}

	std::array<VkDescriptorImageInfo, 4> descriptorImages {};
	descriptorImages[0].imageView   = state.imageViews[0];
	descriptorImages[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	descriptorImages[1].imageView   = state.imageViews[1];
	descriptorImages[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	descriptorImages[2].imageView   = state.imageViews[1];
	descriptorImages[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	descriptorImages[3].imageView   = state.imageViews[0];
	descriptorImages[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	std::array<VkWriteDescriptorSet, 4> descriptorWrites {};
	for (size_t index = 0; index < descriptorWrites.size(); ++index)
	{
		descriptorWrites[index].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		descriptorWrites[index].dstSet          = state.descriptorSets[index / 2];
		descriptorWrites[index].dstBinding      = static_cast<uint32_t>(index % 2);
		descriptorWrites[index].descriptorCount = 1;
		descriptorWrites[index].descriptorType =
		    index % 2 == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		descriptorWrites[index].pImageInfo = &descriptorImages[index];
	}
	vkUpdateDescriptorSets(state.device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);

	VkShaderModuleCreateInfo shaderInfo {};
	shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = shaderBinary.size() * sizeof(uint32_t);
	shaderInfo.pCode    = shaderBinary.data();
	result              = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.shaderModule);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateShaderModule(compute) failed", result);
	}
	VkPipelineShaderStageCreateInfo stageInfo {};
	stageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = state.shaderModule;
	stageInfo.pName  = "main";
	VkComputePipelineCreateInfo pipelineInfo {};
	pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage  = stageInfo;
	pipelineInfo.layout = state.pipelineLayout;
	result              = vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.pipeline);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateComputePipelines failed", result);
	}

	VkCommandPoolCreateInfo commandPoolInfo {};
	commandPoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	commandPoolInfo.queueFamilyIndex = queueFamily;
	result                           = vkCreateCommandPool(state.device, &commandPoolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateCommandPool failed", result);
	}
	VkCommandBufferAllocateInfo commandBufferInfo {};
	commandBufferInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferInfo.commandPool        = state.commandPool;
	commandBufferInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferInfo.commandBufferCount = 1;
	result                               = vkAllocateCommandBuffers(state.device, &commandBufferInfo, &state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateCommandBuffers failed", result);
	}
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result          = vkCreateFence(state.device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateFence failed", result);
	}

	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result          = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
	{
		return Fail("vkBeginCommandBuffer failed", result);
	}

	VkBufferMemoryBarrier uploadForTransfer {};
	uploadForTransfer.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	uploadForTransfer.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
	uploadForTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
	uploadForTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	uploadForTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	uploadForTransfer.buffer              = state.uploadBuffer;
	uploadForTransfer.size                = totalBytes;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 1, &uploadForTransfer, 0, nullptr);

	std::array<VkImageMemoryBarrier, 2> imagesToTransfer {};
	for (size_t index = 0; index < imagesToTransfer.size(); ++index)
	{
		imagesToTransfer[index].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		imagesToTransfer[index].dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
		imagesToTransfer[index].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
		imagesToTransfer[index].newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		imagesToTransfer[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imagesToTransfer[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		imagesToTransfer[index].image                = state.images[index];
		imagesToTransfer[index].subresourceRange     = ColorRange();
	}
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, static_cast<uint32_t>(imagesToTransfer.size()), imagesToTransfer.data());

	std::array<VkBufferImageCopy, 2> uploadCopies {};
	uploadCopies[0].bufferOffset                 = 0;
	uploadCopies[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	uploadCopies[0].imageSubresource.layerCount = 1;
	uploadCopies[0].imageExtent                 = {kS1Width, kS1Height, 1};
	uploadCopies[1].bufferOffset                 = s1Bytes;
	uploadCopies[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	uploadCopies[1].imageSubresource.layerCount = 1;
	uploadCopies[1].imageExtent                 = {kS2Width, kS2Height, 1};
	for (size_t index = 0; index < state.images.size(); ++index)
	{
		vkCmdCopyBufferToImage(state.commandBuffer, state.uploadBuffer, state.images[index],
		                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uploadCopies[index]);
	}

	std::array<VkImageMemoryBarrier, 2> imagesForCompute = imagesToTransfer;
	for (auto& barrier: imagesForCompute)
	{
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
	}
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
	                     0, nullptr, 0, nullptr, static_cast<uint32_t>(imagesForCompute.size()),
	                     imagesForCompute.data());

	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout, 0, 1,
	                        &state.descriptorSets[0], 0, nullptr);
	const PushCoordinates s1Dispatch {
	    static_cast<int32_t>(kSeedX),
	    static_cast<int32_t>(kSeedY),
	    static_cast<int32_t>(kS1WriteX),
	    static_cast<int32_t>(kS1WriteY),
	};
	vkCmdPushConstants(state.commandBuffer, state.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(s1Dispatch),
	                   &s1Dispatch);
	vkCmdDispatch(state.commandBuffer, 1, 1, 1);

	std::array<VkImageMemoryBarrier, 2> chainDependencies {};
	for (size_t index = 0; index < chainDependencies.size(); ++index)
	{
		chainDependencies[index].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		chainDependencies[index].oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
		chainDependencies[index].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
		chainDependencies[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		chainDependencies[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		chainDependencies[index].image                = state.images[index];
		chainDependencies[index].subresourceRange     = ColorRange();
	}
	chainDependencies[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	chainDependencies[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	chainDependencies[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	chainDependencies[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
	                     static_cast<uint32_t>(chainDependencies.size()), chainDependencies.data());

	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout, 0, 1,
	                        &state.descriptorSets[1], 0, nullptr);
	const PushCoordinates s2Dispatch {
	    static_cast<int32_t>(kS1WriteX),
	    static_cast<int32_t>(kS1WriteY),
	    static_cast<int32_t>(kS2WriteX),
	    static_cast<int32_t>(kS2WriteY),
	};
	vkCmdPushConstants(state.commandBuffer, state.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(s2Dispatch),
	                   &s2Dispatch);
	vkCmdDispatch(state.commandBuffer, 1, 1, 1);

	std::array<VkImageMemoryBarrier, 2> imagesForReadback = chainDependencies;
	imagesForReadback[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	imagesForReadback[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	imagesForReadback[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	imagesForReadback[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
	                     0, nullptr, 0, nullptr, static_cast<uint32_t>(imagesForReadback.size()),
	                     imagesForReadback.data());

	std::array<VkBufferImageCopy, 2> readbackCopies = uploadCopies;
	readbackCopies[0].bufferOffset = 0;
	readbackCopies[1].bufferOffset = s1Bytes;
	for (size_t index = 0; index < state.images.size(); ++index)
	{
		vkCmdCopyImageToBuffer(state.commandBuffer, state.images[index], VK_IMAGE_LAYOUT_GENERAL,
		                       state.readbackBuffer, 1, &readbackCopies[index]);
	}

	VkBufferMemoryBarrier readbackToHost {};
	readbackToHost.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	readbackToHost.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	readbackToHost.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	readbackToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	readbackToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	readbackToHost.buffer              = state.readbackBuffer;
	readbackToHost.size                = totalBytes;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
	                     1, &readbackToHost, 0, nullptr);
	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkEndCommandBuffer failed", result);
	}

	VkSubmitInfo submitInfo {};
	submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers    = &state.commandBuffer;
	result                        = vkQueueSubmit(state.queue, 1, &submitInfo, state.fence);
	if (result != VK_SUCCESS)
	{
		return Fail("vkQueueSubmit failed", result);
	}
	result = vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS)
	{
		return Fail("vkWaitForFences failed", result);
	}

	VkMappedMemoryRange readbackRange {};
	readbackRange.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	readbackRange.memory = state.readbackMemory;
	readbackRange.size   = VK_WHOLE_SIZE;
	result               = vkInvalidateMappedMemoryRanges(state.device, 1, &readbackRange);
	if (result != VK_SUCCESS)
	{
		return Fail("vkInvalidateMappedMemoryRanges(readback) failed", result);
	}

	std::vector<Pixel> expectedS1 = initialS1;
	std::vector<Pixel> expectedS2 = initialS2;
	expectedS1[static_cast<size_t>(kS1WriteY) * kS1Width + kS1WriteX] = kSeedPixel;
	expectedS2[static_cast<size_t>(kS2WriteY) * kS2Width + kS2WriteX] = kSeedPixel;
	const auto* observedS1 = static_cast<const Pixel*>(state.mappedReadback);
	const auto* observedS2 = reinterpret_cast<const Pixel*>(static_cast<const uint8_t*>(state.mappedReadback) + s1Bytes);
	if (!VerifyImage(observedS1, expectedS1, "S1") || !VerifyImage(observedS2, expectedS2, "S2"))
	{
		return 1;
	}

	if (VerifyMipSnapshotCopy(physicalDevice, state.device, state.queue, queueFamily) != 0) return 1;
	if (VerifyRenderTargetSampledRefresh(physicalDevice, state.device, state.queue, queueFamily) != 0) return 1;

	const int markerResult =
	    std::printf("[GFX_STORAGE_SINGLE_MIP_WINDOW_COMPUTE] backend=native-metal format=rgba8-unorm "
	                "guest=1230x216 s1=615x108 s2=307x54 mip-levels=single "
	                "usage=transfer-src+transfer-dst+storage+sampled "
	                "chain=s1-write->general-barrier->sample-s1->write-s2 layouts=general "
	                "image-copy=none queue=single s1=verified s2=verified spirv-validation=clean status=clean\n");
	if (markerResult <= 0 || std::fflush(stdout) != 0)
	{
		return Fail("Failed to emit storage single-window marker");
	}
	return 0;
}
