#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
constexpr uint32_t     kWidth                        = 256;
constexpr uint32_t     kHeight                       = 1;
constexpr uint32_t     kVertexBytes                  = 4;
constexpr VkDeviceSize kGuardBytes                   = 64;
constexpr VkDeviceSize kVertexPayloadBytes           = kWidth * kVertexBytes;
constexpr VkDeviceSize kVertexBufferBytes            = kGuardBytes + kVertexPayloadBytes + kGuardBytes;
constexpr VkDeviceSize kPixelBytes                   = sizeof(uint32_t) * 4;
constexpr VkDeviceSize kReadbackPayloadBytes         = kWidth * kHeight * kPixelBytes;
constexpr VkDeviceSize kReadbackBufferBytes          = kGuardBytes + kReadbackPayloadBytes + kGuardBytes;
constexpr uint64_t     kCompletionTimeoutNanoseconds = 5'000'000'000ull;
constexpr uint8_t      kVertexGuardByte               = 0xa5u;
constexpr uint8_t      kReadbackGuardByte             = 0x5au;
constexpr uint8_t      kReadbackUnsetByte             = 0xcdu;

constexpr char kVertexShaderAssembly[] = R"spirv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %out_position %out_point_size %in_vertex_index %in_value %out_value
OpDecorate %out_position BuiltIn Position
OpDecorate %out_point_size BuiltIn PointSize
OpDecorate %in_vertex_index BuiltIn VertexIndex
OpDecorate %in_value Location 0
OpDecorate %out_value Location 0
OpDecorate %out_value Flat
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 1
%v3float = OpTypeVector %float 3
%v4float = OpTypeVector %float 4
%ptr_input_int = OpTypePointer Input %int
%ptr_input_v3float = OpTypePointer Input %v3float
%ptr_output_float = OpTypePointer Output %float
%ptr_output_v3float = OpTypePointer Output %v3float
%ptr_output_v4float = OpTypePointer Output %v4float
%float_0 = OpConstant %float 0
%float_1 = OpConstant %float 1
%pixel_scale = OpConstant %float 0.0078125
%pixel_bias = OpConstant %float -0.99609375
%out_position = OpVariable %ptr_output_v4float Output
%out_point_size = OpVariable %ptr_output_float Output
%in_vertex_index = OpVariable %ptr_input_int Input
%in_value = OpVariable %ptr_input_v3float Input
%out_value = OpVariable %ptr_output_v3float Output
%main = OpFunction %void None %function
%entry = OpLabel
%vertex_index = OpLoad %int %in_vertex_index
%vertex_index_float = OpConvertSToF %float %vertex_index
%scaled_index = OpFMul %float %vertex_index_float %pixel_scale
%position_x = OpFAdd %float %scaled_index %pixel_bias
%position = OpCompositeConstruct %v4float %position_x %float_0 %float_0 %float_1
%value = OpLoad %v3float %in_value
OpStore %out_position %position
OpStore %out_point_size %float_1
OpStore %out_value %value
OpReturn
OpFunctionEnd
)spirv";

constexpr char kFragmentShaderAssembly[] = R"spirv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %in_value %out_color
OpExecutionMode %main OriginUpperLeft
OpDecorate %in_value Location 0
OpDecorate %in_value Flat
OpDecorate %out_color Location 0
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%v3float = OpTypeVector %float 3
%v4float = OpTypeVector %float 4
%ptr_input_v3float = OpTypePointer Input %v3float
%ptr_output_v4float = OpTypePointer Output %v4float
%float_1 = OpConstant %float 1
%in_value = OpVariable %ptr_input_v3float Input
%out_color = OpVariable %ptr_output_v4float Output
%main = OpFunction %void None %function
%entry = OpLabel
%value = OpLoad %v3float %in_value
%x = OpCompositeExtract %float %value 0
%y = OpCompositeExtract %float %value 1
%z = OpCompositeExtract %float %value 2
%color = OpCompositeConstruct %v4float %x %y %z %float_1
OpStore %out_color %color
OpReturn
OpFunctionEnd
)spirv";

struct ProbeState
{
	VkInstance       instance        = VK_NULL_HANDLE;
	VkDevice         device          = VK_NULL_HANDLE;
	VkQueue          queue           = VK_NULL_HANDLE;
	VkBuffer         vertexBuffer    = VK_NULL_HANDLE;
	VkDeviceMemory   vertexMemory    = VK_NULL_HANDLE;
	void*            vertexMapped    = nullptr;
	VkBuffer         readbackBuffer  = VK_NULL_HANDLE;
	VkDeviceMemory   readbackMemory  = VK_NULL_HANDLE;
	void*            readbackMapped  = nullptr;
	VkImage          image           = VK_NULL_HANDLE;
	VkDeviceMemory   imageMemory     = VK_NULL_HANDLE;
	VkImageView      imageView       = VK_NULL_HANDLE;
	VkRenderPass     renderPass      = VK_NULL_HANDLE;
	VkFramebuffer    framebuffer     = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout  = VK_NULL_HANDLE;
	VkShaderModule   vertexShader    = VK_NULL_HANDLE;
	VkShaderModule   fragmentShader  = VK_NULL_HANDLE;
	VkPipeline       controlPipelineA = VK_NULL_HANDLE;
	VkPipeline       pipeline        = VK_NULL_HANDLE;
	VkPipeline       controlPipelineB = VK_NULL_HANDLE;
	VkCommandPool    commandPool     = VK_NULL_HANDLE;
	VkCommandBuffer  commandBuffer   = VK_NULL_HANDLE;
	VkFence          completionFence = VK_NULL_HANDLE;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(device);
		}
		if (vertexMapped != nullptr)
		{
			vkUnmapMemory(device, vertexMemory);
		}
		if (readbackMapped != nullptr)
		{
			vkUnmapMemory(device, readbackMemory);
		}
		if (completionFence != VK_NULL_HANDLE)
		{
			vkDestroyFence(device, completionFence, nullptr);
		}
		if (commandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(device, commandPool, nullptr);
		}
		if (pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, pipeline, nullptr);
		}
		if (controlPipelineB != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, controlPipelineB, nullptr);
		}
		if (controlPipelineA != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, controlPipelineA, nullptr);
		}
		if (fragmentShader != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, fragmentShader, nullptr);
		}
		if (vertexShader != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, vertexShader, nullptr);
		}
		if (pipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		}
		if (framebuffer != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(device, framebuffer, nullptr);
		}
		if (renderPass != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(device, renderPass, nullptr);
		}
		if (imageView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(device, imageView, nullptr);
		}
		if (image != VK_NULL_HANDLE)
		{
			vkDestroyImage(device, image, nullptr);
		}
		if (imageMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, imageMemory, nullptr);
		}
		if (readbackBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, readbackBuffer, nullptr);
		}
		if (readbackMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, readbackMemory, nullptr);
		}
		if (vertexBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, vertexBuffer, nullptr);
		}
		if (vertexMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, vertexMemory, nullptr);
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

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
	                    VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties{};
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

bool AssembleShader(const char* source, std::vector<uint32_t>* binary)
{
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position,
	                            const char* message) {
		std::fprintf(stderr, "SPIR-V line %zu: %s\n", position.line, message);
	});
	return tools.Assemble(source, std::strlen(source), binary) && tools.Validate(*binary);
}

bool CreateHostBuffer(ProbeState& state, VkPhysicalDevice physicalDevice, VkDeviceSize size,
	                  VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped)
{
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size        = size;
	bufferInfo.usage       = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult result = vkCreateBuffer(state.device, &bufferInfo, nullptr, buffer);
	if (result != VK_SUCCESS)
	{
		std::fprintf(stderr, "vkCreateBuffer failed: %d\n", result);
		return false;
	}

	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(state.device, *buffer, &requirements);
	const uint32_t memoryType = FindMemoryType(
	    physicalDevice, requirements.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX)
	{
		std::fprintf(stderr, "No coherent host-visible buffer memory type\n");
		return false;
	}
	VkMemoryAllocateInfo allocation{};
	allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize  = requirements.size;
	allocation.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(state.device, &allocation, nullptr, memory);
	if (result != VK_SUCCESS)
	{
		std::fprintf(stderr, "vkAllocateMemory(buffer) failed: %d\n", result);
		return false;
	}
	result = vkBindBufferMemory(state.device, *buffer, *memory, 0);
	if (result != VK_SUCCESS)
	{
		std::fprintf(stderr, "vkBindBufferMemory failed: %d\n", result);
		return false;
	}
	result = vkMapMemory(state.device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
	if (result != VK_SUCCESS || *mapped == nullptr)
	{
		std::fprintf(stderr, "vkMapMemory(buffer) failed: %d\n", result);
		return false;
	}
	return true;
}

VkResult FlushMemory(VkDevice device, VkDeviceMemory memory)
{
	VkMappedMemoryRange range{};
	range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = memory;
	range.size   = VK_WHOLE_SIZE;
	return vkFlushMappedMemoryRanges(device, 1, &range);
}

VkResult InvalidateMemory(VkDevice device, VkDeviceMemory memory)
{
	VkMappedMemoryRange range{};
	range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = memory;
	range.size   = VK_WHOLE_SIZE;
	return vkInvalidateMappedMemoryRanges(device, 1, &range);
}

constexpr int HighestSetBit(uint32_t value)
{
	int bit = -1;
	while (value != 0)
	{
		value >>= 1u;
		++bit;
	}
	return bit;
}

// Exact binary32 round-to-nearest-even oracle for max(signed_byte / 127, -1).
// For magnitudes 1..126, the odd denominator means an exact half-way remainder
// is impossible, so twice-the-remainder selects the rounded significand.
constexpr uint32_t Snorm8Bits(uint8_t byte)
{
	const int32_t signedValue = byte < 0x80u ? static_cast<int32_t>(byte) :
	                                                static_cast<int32_t>(byte) - 256;
	if (signedValue <= -127)
	{
		return 0xbf800000u;
	}
	if (signedValue == 0)
	{
		return 0u;
	}
	if (signedValue == 127)
	{
		return 0x3f800000u;
	}
	const uint32_t sign      = signedValue < 0 ? 0x80000000u : 0u;
	const uint32_t magnitude = static_cast<uint32_t>(signedValue < 0 ? -signedValue : signedValue);
	const int      exponent  = HighestSetBit(magnitude) - 7;
	const uint64_t numerator = static_cast<uint64_t>(magnitude) << static_cast<unsigned>(23 - exponent);
	uint32_t       significand = static_cast<uint32_t>(numerator / 127u);
	const uint32_t remainder   = static_cast<uint32_t>(numerator % 127u);
	if (remainder * 2u > 127u)
	{
		++significand;
	}
	return sign | (static_cast<uint32_t>(exponent + 127) << 23u) |
	       (significand - 0x00800000u);
}

void PrepareVertexInput(void* mapped, uint32_t pass)
{
	auto* bytes = static_cast<uint8_t*>(mapped);
	std::memset(bytes, kVertexGuardByte, static_cast<size_t>(kVertexBufferBytes));
	for (uint32_t index = 0; index < kWidth; ++index)
	{
		const size_t offset = static_cast<size_t>(kGuardBytes) + index * kVertexBytes;
		bytes[offset + 0]   = static_cast<uint8_t>(index);
		bytes[offset + 1]   = static_cast<uint8_t>(index + 85u);
		bytes[offset + 2]   = static_cast<uint8_t>(index + 170u);
		const uint8_t w     = static_cast<uint8_t>(index * 73u + 19u);
		bytes[offset + 3]   = pass == 0 ? w : static_cast<uint8_t>(w ^ 0xffu);
	}
}

bool VerifyGuards(const uint8_t* bytes, VkDeviceSize payloadBytes, uint8_t guard)
{
	for (VkDeviceSize index = 0; index < kGuardBytes; ++index)
	{
		if (bytes[index] != guard || bytes[kGuardBytes + payloadBytes + index] != guard)
		{
			return false;
		}
	}
	return true;
}

int RecordSubmitAndWait(ProbeState& state, bool firstPass)
{
	VkResult result;
	if (!firstPass)
	{
		result = vkResetFences(state.device, 1, &state.completionFence);
		if (result != VK_SUCCESS)
		{
			return Fail("vkResetFences failed", result);
		}
		result = vkResetCommandPool(state.device, state.commandPool, 0);
		if (result != VK_SUCCESS)
		{
			return Fail("vkResetCommandPool failed", result);
		}
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
	{
		return Fail("vkBeginCommandBuffer failed", result);
	}

	std::array<VkBufferMemoryBarrier, 2> hostBarriers{};
	hostBarriers[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	hostBarriers[0].srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
	hostBarriers[0].dstAccessMask       = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
	hostBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostBarriers[0].buffer              = state.vertexBuffer;
	hostBarriers[0].size                = kVertexBufferBytes;
	hostBarriers[1]                     = hostBarriers[0];
	hostBarriers[1].dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	hostBarriers[1].buffer              = state.readbackBuffer;
	hostBarriers[1].size                = kReadbackBufferBytes;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_HOST_BIT,
	                     VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
	                     0, nullptr, static_cast<uint32_t>(hostBarriers.size()), hostBarriers.data(), 0, nullptr);

	VkImageMemoryBarrier toColor{};
	toColor.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toColor.srcAccessMask       = firstPass ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
	toColor.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toColor.oldLayout           = firstPass ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toColor.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toColor.image               = state.image;
	toColor.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	toColor.subresourceRange.baseMipLevel   = 0;
	toColor.subresourceRange.levelCount     = 1;
	toColor.subresourceRange.baseArrayLayer = 0;
	toColor.subresourceRange.layerCount     = 1;
	vkCmdPipelineBarrier(state.commandBuffer,
	                     firstPass ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &toColor);

	VkRenderPassBeginInfo renderBegin{};
	renderBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderBegin.renderPass        = state.renderPass;
	renderBegin.framebuffer       = state.framebuffer;
	renderBegin.renderArea.extent = {kWidth, kHeight};
	VkClearValue clear{};
	clear.color.float32[0]       = 0.25f;
	clear.color.float32[1]       = -0.25f;
	clear.color.float32[2]       = 0.5f;
	clear.color.float32[3]       = 0.0f;
	renderBegin.clearValueCount  = 1;
	renderBegin.pClearValues     = &clear;
	vkCmdBeginRenderPass(state.commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipeline);
	const VkDeviceSize vertexOffset = kGuardBytes;
	vkCmdBindVertexBuffers(state.commandBuffer, 0, 1, &state.vertexBuffer, &vertexOffset);
	vkCmdDraw(state.commandBuffer, kWidth, 1, 0, 0);
	vkCmdEndRenderPass(state.commandBuffer);

	VkImageMemoryBarrier toTransfer{};
	toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toTransfer.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
	toTransfer.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.image               = state.image;
	toTransfer.subresourceRange    = toColor.subresourceRange;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

	VkBufferImageCopy copy{};
	copy.bufferOffset                    = kGuardBytes;
	copy.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.baseArrayLayer = 0;
	copy.imageSubresource.layerCount     = 1;
	copy.imageOffset                     = {0, 0, 0};
	copy.imageExtent                     = {kWidth, kHeight, 1};
	vkCmdCopyImageToBuffer(state.commandBuffer, state.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       state.readbackBuffer, 1, &copy);

	VkBufferMemoryBarrier toHost{};
	toHost.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	toHost.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	toHost.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toHost.buffer              = state.readbackBuffer;
	toHost.offset              = kGuardBytes;
	toHost.size                = kReadbackPayloadBytes;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0, nullptr);

	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkEndCommandBuffer failed", result);
	}
	VkSubmitInfo submit{};
	submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &state.commandBuffer;
	result = vkQueueSubmit(state.queue, 1, &submit, state.completionFence);
	if (result != VK_SUCCESS)
	{
		return Fail("vkQueueSubmit failed", result);
	}
	result = vkWaitForFences(state.device, 1, &state.completionFence, VK_TRUE,
	                         kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS)
	{
		return Fail("vkWaitForFences failed", result);
	}
	return 0;
}
}

int main()
{
	static_assert(Snorm8Bits(0x80u) == 0xbf800000u);
	static_assert(Snorm8Bits(0x81u) == 0xbf800000u);
	static_assert(Snorm8Bits(0x82u) == 0xbf7dfbf8u);
	static_assert(Snorm8Bits(0xffu) == 0xbc010204u);
	static_assert(Snorm8Bits(0x00u) == 0x00000000u);
	static_assert(Snorm8Bits(0x01u) == 0x3c010204u);
	static_assert(Snorm8Bits(0x7eu) == 0x3f7dfbf8u);
	static_assert(Snorm8Bits(0x7fu) == 0x3f800000u);

	std::vector<uint32_t> vertexBinary;
	std::vector<uint32_t> fragmentBinary;
	if (!AssembleShader(kVertexShaderAssembly, &vertexBinary) ||
	    !AssembleShader(kFragmentShaderAssembly, &fragmentBinary))
	{
		return Fail("Vertex SNORM8x4/vec3 SPIR-V assembly or validation failed");
	}
	if (setenv("LUMEN_A64_METAL_SHADER_FUNCTION_CACHE", "1", 1) != 0)
	{
		return Fail("Could not enable the Native Metal shader-function cache");
	}

	ProbeState state;
	VkApplicationInfo application{};
	application.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName   = "Lumen SNORM8x4 to vec3 vertex probe";
	application.applicationVersion = 1;
	application.pEngineName        = "Lumen";
	application.engineVersion      = 1;
	application.apiVersion         = VK_API_VERSION_1_1;
	VkInstanceCreateInfo instanceInfo{};
	instanceInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &application;
	VkResult result = vkCreateInstance(&instanceInfo, nullptr, &state.instance);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateInstance failed", result);
	}

	uint32_t physicalDeviceCount = 0;
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0)
	{
		return Fail("No Native Metal physical device found", result);
	}
	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS)
	{
		return Fail("vkEnumeratePhysicalDevices(list) failed", result);
	}
	physicalDevices.resize(physicalDeviceCount);

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t queueFamily = UINT32_MAX;
	for (VkPhysicalDevice candidate : physicalDevices)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
		for (uint32_t family = 0; family < familyCount; ++family)
		{
			if (families[family].queueCount != 0 && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
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
		return Fail("No graphics-capable Native Metal queue found");
	}

	VkFormatProperties vertexFormatProperties{};
	vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R8G8B8A8_SNORM,
	                                    &vertexFormatProperties);
	if ((vertexFormatProperties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) == 0)
	{
		return Fail("VK_FORMAT_R8G8B8A8_SNORM is not exposed as a vertex-buffer format");
	}
	VkFormatProperties colorFormatProperties{};
	vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R32G32B32A32_SFLOAT,
	                                    &colorFormatProperties);
	constexpr VkFormatFeatureFlags requiredColorFeatures =
	    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if ((colorFormatProperties.optimalTilingFeatures & requiredColorFeatures) != requiredColorFeatures)
	{
		return Fail("VK_FORMAT_R32G32B32A32_SFLOAT lacks color-attachment/transfer-source support");
	}

	constexpr float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{};
	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	VkDeviceCreateInfo deviceInfo{};
	deviceInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount    = 1;
	deviceInfo.pQueueCreateInfos       = &queueInfo;
	result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &state.device);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDevice failed", result);
	}
	vkGetDeviceQueue(state.device, queueFamily, 0, &state.queue);
	if (state.queue == VK_NULL_HANDLE)
	{
		return Fail("vkGetDeviceQueue returned a null queue");
	}

	if (!CreateHostBuffer(state, physicalDevice, kVertexBufferBytes,
	                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &state.vertexBuffer,
	                      &state.vertexMemory, &state.vertexMapped) ||
	    !CreateHostBuffer(state, physicalDevice, kReadbackBufferBytes,
	                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, &state.readbackBuffer,
	                      &state.readbackMemory, &state.readbackMapped))
	{
		return 1;
	}

	VkImageCreateInfo imageInfo{};
	imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType     = VK_IMAGE_TYPE_2D;
	imageInfo.format        = VK_FORMAT_R32G32B32A32_SFLOAT;
	imageInfo.extent        = {kWidth, kHeight, 1};
	imageInfo.mipLevels     = 1;
	imageInfo.arrayLayers   = 1;
	imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(state.device, &imageInfo, nullptr, &state.image);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateImage(RGBA32F) failed", result);
	}
	VkMemoryRequirements imageRequirements{};
	vkGetImageMemoryRequirements(state.device, state.image, &imageRequirements);
	const uint32_t imageMemoryType = FindMemoryType(
	    physicalDevice, imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (imageMemoryType == UINT32_MAX)
	{
		return Fail("No device-local memory type for RGBA32F image");
	}
	VkMemoryAllocateInfo imageAllocation{};
	imageAllocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	imageAllocation.allocationSize  = imageRequirements.size;
	imageAllocation.memoryTypeIndex = imageMemoryType;
	result = vkAllocateMemory(state.device, &imageAllocation, nullptr, &state.imageMemory);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateMemory(image) failed", result);
	}
	result = vkBindImageMemory(state.device, state.image, state.imageMemory, 0);
	if (result != VK_SUCCESS)
	{
		return Fail("vkBindImageMemory failed", result);
	}

	VkImageViewCreateInfo imageViewInfo{};
	imageViewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	imageViewInfo.image                           = state.image;
	imageViewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	imageViewInfo.format                          = VK_FORMAT_R32G32B32A32_SFLOAT;
	imageViewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	imageViewInfo.subresourceRange.baseMipLevel   = 0;
	imageViewInfo.subresourceRange.levelCount     = 1;
	imageViewInfo.subresourceRange.baseArrayLayer = 0;
	imageViewInfo.subresourceRange.layerCount     = 1;
	result = vkCreateImageView(state.device, &imageViewInfo, nullptr, &state.imageView);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateImageView(RGBA32F) failed", result);
	}

	VkAttachmentDescription attachment{};
	attachment.format         = VK_FORMAT_R32G32B32A32_SFLOAT;
	attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference colorReference{};
	colorReference.attachment = 0;
	colorReference.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments    = &colorReference;
	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 1;
	renderPassInfo.pAttachments    = &attachment;
	renderPassInfo.subpassCount    = 1;
	renderPassInfo.pSubpasses      = &subpass;
	result = vkCreateRenderPass(state.device, &renderPassInfo, nullptr, &state.renderPass);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateRenderPass(RGBA32F) failed", result);
	}

	VkFramebufferCreateInfo framebufferInfo{};
	framebufferInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass      = state.renderPass;
	framebufferInfo.attachmentCount = 1;
	framebufferInfo.pAttachments    = &state.imageView;
	framebufferInfo.width           = kWidth;
	framebufferInfo.height          = kHeight;
	framebufferInfo.layers          = 1;
	result = vkCreateFramebuffer(state.device, &framebufferInfo, nullptr, &state.framebuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateFramebuffer(RGBA32F) failed", result);
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	result = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreatePipelineLayout failed", result);
	}

	VkShaderModuleCreateInfo shaderInfo{};
	shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = vertexBinary.size() * sizeof(vertexBinary[0]);
	shaderInfo.pCode    = vertexBinary.data();
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.vertexShader);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateShaderModule(vertex vec3) failed", result);
	}
	shaderInfo.codeSize = fragmentBinary.size() * sizeof(fragmentBinary[0]);
	shaderInfo.pCode    = fragmentBinary.data();
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.fragmentShader);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateShaderModule(fragment vec3) failed", result);
	}

	std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = state.vertexShader;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = state.fragmentShader;
	stages[1].pName  = "main";
	VkVertexInputBindingDescription binding{};
	binding.binding   = 0;
	binding.stride    = sizeof(float) * 3;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	VkVertexInputAttributeDescription attribute{};
	attribute.location = 0;
	attribute.binding  = 0;
	attribute.format   = VK_FORMAT_R32G32B32_SFLOAT;
	attribute.offset   = 0;
	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount   = 1;
	vertexInput.pVertexBindingDescriptions      = &binding;
	vertexInput.vertexAttributeDescriptionCount = 1;
	vertexInput.pVertexAttributeDescriptions    = &attribute;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
	VkViewport viewport{};
	viewport.width    = static_cast<float>(kWidth);
	viewport.height   = static_cast<float>(kHeight);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	VkRect2D scissor{};
	scissor.extent = {kWidth, kHeight};
	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports    = &viewport;
	viewportState.scissorCount  = 1;
	viewportState.pScissors     = &scissor;
	VkPipelineRasterizationStateCreateInfo rasterization{};
	rasterization.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode    = VK_CULL_MODE_NONE;
	rasterization.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth   = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState blendAttachment{};
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blend{};
	blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments    = &blendAttachment;
	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount          = static_cast<uint32_t>(stages.size());
	pipelineInfo.pStages             = stages.data();
	pipelineInfo.pVertexInputState   = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState      = &viewportState;
	pipelineInfo.pRasterizationState = &rasterization;
	pipelineInfo.pMultisampleState   = &multisample;
	pipelineInfo.pColorBlendState    = &blend;
	pipelineInfo.layout              = state.pipelineLayout;
	pipelineInfo.renderPass          = state.renderPass;
	pipelineInfo.subpass             = 0;
	// A-B-A pipeline creation is deliberate. Both variants use the exact same
	// logical vec3 SPIR-V. Native Metal must nevertheless keep the ordinary
	// float3 stage-in function distinct from the SNORM8x4 function whose host
	// interface is widened to four components before narrowing to guest XYZ.
	result = vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
	                                   &state.controlPipelineA);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateGraphicsPipelines(float3 control A) failed", result);
	}
	binding.stride  = kVertexBytes;
	attribute.format = VK_FORMAT_R8G8B8A8_SNORM;
	result = vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
	                                   &state.pipeline);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateGraphicsPipelines(SNORM8x4 to vec3) failed", result);
	}
	binding.stride   = sizeof(float) * 3;
	attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
	result = vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
	                                   &state.controlPipelineB);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateGraphicsPipelines(float3 control B) failed", result);
	}

	VkCommandPoolCreateInfo commandPoolInfo{};
	commandPoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
	                                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commandPoolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(state.device, &commandPoolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateCommandPool failed", result);
	}
	VkCommandBufferAllocateInfo commandBufferInfo{};
	commandBufferInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferInfo.commandPool        = state.commandPool;
	commandBufferInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferInfo.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(state.device, &commandBufferInfo, &state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateCommandBuffers failed", result);
	}
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(state.device, &fenceInfo, nullptr, &state.completionFence);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateFence failed", result);
	}

	std::array<uint32_t, kWidth * 4> firstOutput{};
	for (uint32_t pass = 0; pass < 2; ++pass)
	{
		PrepareVertexInput(state.vertexMapped, pass);
		std::vector<uint8_t> vertexSnapshot(static_cast<size_t>(kVertexBufferBytes));
		std::memcpy(vertexSnapshot.data(), state.vertexMapped, vertexSnapshot.size());
		auto* readbackBytes = static_cast<uint8_t*>(state.readbackMapped);
		std::memset(readbackBytes, kReadbackGuardByte, static_cast<size_t>(kGuardBytes));
		std::memset(readbackBytes + kGuardBytes, kReadbackUnsetByte,
		            static_cast<size_t>(kReadbackPayloadBytes));
		std::memset(readbackBytes + kGuardBytes + kReadbackPayloadBytes, kReadbackGuardByte,
		            static_cast<size_t>(kGuardBytes));
		result = FlushMemory(state.device, state.vertexMemory);
		if (result != VK_SUCCESS)
		{
			return Fail("vkFlushMappedMemoryRanges(vertex) failed", result);
		}
		result = FlushMemory(state.device, state.readbackMemory);
		if (result != VK_SUCCESS)
		{
			return Fail("vkFlushMappedMemoryRanges(readback) failed", result);
		}
		if (RecordSubmitAndWait(state, pass == 0) != 0)
		{
			return 1;
		}
		result = InvalidateMemory(state.device, state.vertexMemory);
		if (result != VK_SUCCESS)
		{
			return Fail("vkInvalidateMappedMemoryRanges(vertex) failed", result);
		}
		result = InvalidateMemory(state.device, state.readbackMemory);
		if (result != VK_SUCCESS)
		{
			return Fail("vkInvalidateMappedMemoryRanges(readback) failed", result);
		}
		if (std::memcmp(vertexSnapshot.data(), state.vertexMapped, vertexSnapshot.size()) != 0 ||
		    !VerifyGuards(static_cast<const uint8_t*>(state.vertexMapped), kVertexPayloadBytes,
		                  kVertexGuardByte))
		{
			return Fail("Vertex input payload or guards changed during draw");
		}
		if (!VerifyGuards(readbackBytes, kReadbackPayloadBytes, kReadbackGuardByte))
		{
			return Fail("Readback copy changed a prefix or suffix guard");
		}

		std::array<uint32_t, kWidth * 4> output{};
		std::memcpy(output.data(), readbackBytes + kGuardBytes, static_cast<size_t>(kReadbackPayloadBytes));
		const auto* vertices = static_cast<const uint8_t*>(state.vertexMapped) + kGuardBytes;
		for (uint32_t pixel = 0; pixel < kWidth; ++pixel)
		{
			const size_t vertexOffset = pixel * kVertexBytes;
			const size_t outputOffset = pixel * 4u;
			const std::array<uint32_t, 4> expected = {
			    Snorm8Bits(vertices[vertexOffset + 0]),
			    Snorm8Bits(vertices[vertexOffset + 1]),
			    Snorm8Bits(vertices[vertexOffset + 2]),
			    0x3f800000u,
			};
			for (uint32_t component = 0; component < 4; ++component)
			{
				if (output[outputOffset + component] != expected[component])
				{
					std::fprintf(stderr,
					             "SNORM8x4/vec3 mismatch: pass=%u pixel=%u component=%u byte=%02x "
					             "actual=%08x expected=%08x w=%02x\n",
					             pass, pixel, component,
					             component < 3 ? vertices[vertexOffset + component] : 0u,
					             output[outputOffset + component], expected[component],
					             vertices[vertexOffset + 3]);
					return 1;
				}
			}
		}
		if (pass == 0)
		{
			firstOutput = output;
		} else if (output != firstOutput)
		{
			return Fail("Changing only W changed a vec3 shader result");
		}
	}

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);
	const int markerResult = std::printf(
	    "[GFX_VERTEX_SNORM8X4_VEC3_VULKAN_PROBE] backend=native-metal device=%s "
	    "format=R8G8B8A8_SNORM shader-input=v3float metal-stage-in=v4-narrow-xyz "
	    "bytes-per-element=4 values-per-component=256 w-independence=verified "
	    "shader-function-cache-identity=A-B-A input-guards=verified "
	    "readback-guards=verified status=clean\n",
	    properties.deviceName);
	if (markerResult <= 0 || std::fflush(stdout) != 0)
	{
		return Fail("Failed to emit vertex SNORM8x4/vec3 marker");
	}
	return 0;
}
