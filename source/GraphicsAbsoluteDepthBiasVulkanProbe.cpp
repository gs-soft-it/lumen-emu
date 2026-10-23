#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#error "The Native Metal absolute depth-bias probe must be compiled with C++ exceptions disabled"
#endif

namespace
{
constexpr uint32_t     kWidth                        = 64;
constexpr uint32_t     kHeight                       = 8;
constexpr uint32_t     kPixelCount                   = kWidth * kHeight;
constexpr VkDeviceSize kDepthBytesPerTexel           = sizeof(float);
constexpr VkDeviceSize kGuardBytes                   = 256;
constexpr VkDeviceSize kReadbackPayloadBytes         = kPixelCount * kDepthBytesPerTexel;
constexpr VkDeviceSize kReadbackBufferBytes          = kGuardBytes + kReadbackPayloadBytes + kGuardBytes;
constexpr uint32_t     kMaxDrawsPerBatch              = 3;
constexpr VkDeviceSize kVertexBufferBytes            = kMaxDrawsPerBatch * 3 * 4 * sizeof(float);
constexpr uint64_t     kCompletionTimeoutNanoseconds = 5'000'000'000ull;
constexpr uint8_t      kGuardByte                    = 0xa5u;
constexpr uint8_t      kReadbackUnsetByte            = 0xcdu;
constexpr float        kOriginalMinDepth             = 0.5f;
constexpr float        kOriginalMaxDepth             = 1.0f;
constexpr float        kAbsoluteOffset               = -0.3f;
constexpr float        kTranslatedMinDepth           = kOriginalMinDepth + kAbsoluteOffset;
constexpr float        kTranslatedMaxDepth           = kOriginalMaxDepth + kAbsoluteOffset;
constexpr float        kSlopeFactor                  = -0.3f;
constexpr float        kGradient                     = 0.125f;
constexpr std::array<float, 3> kBaseDepths {0.125f, 0.375f, 0.625f};

static_assert(kDepthBytesPerTexel == 4);
static_assert(kWidth * kDepthBytesPerTexel == 256);
static_assert(kTranslatedMinDepth > 0.199f && kTranslatedMinDepth < 0.201f);
static_assert(kTranslatedMaxDepth > 0.699f && kTranslatedMaxDepth < 0.701f);

constexpr char kVertexShaderAssembly[] = R"spirv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %in_position %out_position
OpDecorate %in_position Location 0
OpDecorate %out_position BuiltIn Position
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%v4float = OpTypeVector %float 4
%ptr_input_v4float = OpTypePointer Input %v4float
%ptr_output_v4float = OpTypePointer Output %v4float
%in_position = OpVariable %ptr_input_v4float Input
%out_position = OpVariable %ptr_output_v4float Output
%main = OpFunction %void None %function
%entry = OpLabel
%position = OpLoad %v4float %in_position
OpStore %out_position %position
OpReturn
OpFunctionEnd
)spirv";

constexpr char kFragmentShaderAssembly[] = R"spirv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main"
OpExecutionMode %main OriginUpperLeft
%void = OpTypeVoid
%function = OpTypeFunction %void
%main = OpFunction %void None %function
%entry = OpLabel
OpReturn
OpFunctionEnd
)spirv";

struct HostBuffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void*          mapped = nullptr;
};

enum PipelineIndex : size_t
{
	OffOriginal,
	OffTranslated,
	AbsoluteTranslated,
	FormatRelative,
	OcclusionOff,
	OcclusionAbsolute,
	OcclusionRelative,
	PipelineCount,
};

struct DrawSpec
{
	VkPipeline pipeline;
	float      baseDepth;
	float      gradient;
	VkRect2D   scissor;
};

struct ProbeState
{
	VkInstance       instance       = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice         device         = VK_NULL_HANDLE;
	VkQueue          queue          = VK_NULL_HANDLE;
	uint32_t         queueFamily    = UINT32_MAX;
	HostBuffer       vertices;
	HostBuffer       readback;
	VkImage          depthImage     = VK_NULL_HANDLE;
	VkDeviceMemory   depthMemory    = VK_NULL_HANDLE;
	VkImageView      depthView      = VK_NULL_HANDLE;
	VkRenderPass     renderPass     = VK_NULL_HANDLE;
	VkFramebuffer    framebuffer    = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	std::array<VkShaderModule, 2> shaders {};
	std::array<VkPipeline, PipelineCount> pipelines {};
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkFence       fence       = VK_NULL_HANDLE;
	VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
		if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
		for (VkPipeline pipeline: pipelines)
		{
			if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
		}
		for (VkShaderModule shader: shaders)
		{
			if (shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, shader, nullptr);
		}
		if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if (renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, renderPass, nullptr);
		if (depthView != VK_NULL_HANDLE) vkDestroyImageView(device, depthView, nullptr);
		if (depthImage != VK_NULL_HANDLE) vkDestroyImage(device, depthImage, nullptr);
		if (depthMemory != VK_NULL_HANDLE) vkFreeMemory(device, depthMemory, nullptr);
		for (HostBuffer* buffer: {&vertices, &readback})
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

VkResult CreateHostBuffer(ProbeState* state, VkDeviceSize size, VkBufferUsageFlags usage,
	                      HostBuffer* output)
{
	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size  = size;
	bufferInfo.usage = usage;
	VkResult result  = vkCreateBuffer(state->device, &bufferInfo, nullptr, &output->buffer);
	if (result != VK_SUCCESS) return result;
	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(state->device, output->buffer, &requirements);
	const uint32_t memoryType = FindMemoryType(state->physicalDevice, requirements.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(state->device, &allocationInfo, nullptr, &output->memory);
	if (result != VK_SUCCESS) return result;
	result = vkBindBufferMemory(state->device, output->buffer, output->memory, 0);
	if (result != VK_SUCCESS) return result;
	return vkMapMemory(state->device, output->memory, 0, VK_WHOLE_SIZE, 0, &output->mapped);
}

VkResult FlushBuffer(const ProbeState& state, const HostBuffer& buffer)
{
	VkMappedMemoryRange range {};
	range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = buffer.memory;
	range.size   = VK_WHOLE_SIZE;
	return vkFlushMappedMemoryRanges(state.device, 1, &range);
}

VkResult InvalidateBuffer(const ProbeState& state, const HostBuffer& buffer)
{
	VkMappedMemoryRange range {};
	range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.memory = buffer.memory;
	range.size   = VK_WHOLE_SIZE;
	return vkInvalidateMappedMemoryRanges(state.device, 1, &range);
}

bool AssembleShader(const char* assembly, std::vector<uint32_t>* binary)
{
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position,
	                            const char* message)
	                         { std::fprintf(stderr, "SPIR-V line %zu: %s\n", position.line, message); });
	return tools.Assemble(assembly, std::strlen(assembly), binary) && tools.Validate(*binary);
}

VkResult CreatePipeline(ProbeState* state, float minDepth, float maxDepth, VkBool32 biasEnabled,
	                    float constantFactor, float slopeFactor, VkPipeline* output,
	                    VkCompareOp compareOp = VK_COMPARE_OP_ALWAYS)
{
	std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
	for (uint32_t index = 0; index < stages.size(); ++index)
	{
		stages[index].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[index].stage  = index == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[index].module = state->shaders[index];
		stages[index].pName  = "main";
	}
	VkVertexInputBindingDescription binding {};
	binding.binding   = 0;
	binding.stride    = 4 * sizeof(float);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	VkVertexInputAttributeDescription attribute {};
	attribute.location = 0;
	attribute.binding  = 0;
	attribute.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
	attribute.offset   = 0;
	VkPipelineVertexInputStateCreateInfo vertexInput {};
	vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInput.vertexBindingDescriptionCount   = 1;
	vertexInput.pVertexBindingDescriptions      = &binding;
	vertexInput.vertexAttributeDescriptionCount = 1;
	vertexInput.pVertexAttributeDescriptions    = &attribute;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly {};
	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport viewport {};
	viewport.width    = static_cast<float>(kWidth);
	viewport.height   = static_cast<float>(kHeight);
	viewport.minDepth = minDepth;
	viewport.maxDepth = maxDepth;
	VkPipelineViewportStateCreateInfo viewportState {};
	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports    = &viewport;
	viewportState.scissorCount  = 1;
	VkPipelineRasterizationStateCreateInfo rasterization {};
	rasterization.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode             = VK_POLYGON_MODE_FILL;
	rasterization.cullMode                = VK_CULL_MODE_NONE;
	rasterization.frontFace               = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.depthBiasEnable         = biasEnabled;
	rasterization.depthBiasConstantFactor = constantFactor;
	rasterization.depthBiasClamp          = 0.0f;
	rasterization.depthBiasSlopeFactor    = slopeFactor;
	rasterization.lineWidth               = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample {};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depthStencil {};
	depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable  = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp   = compareOp;
	VkPipelineColorBlendStateCreateInfo colorBlend {};
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	constexpr VkDynamicState dynamicState = VK_DYNAMIC_STATE_SCISSOR;
	VkPipelineDynamicStateCreateInfo dynamic {};
	dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 1;
	dynamic.pDynamicStates    = &dynamicState;
	VkGraphicsPipelineCreateInfo pipelineInfo {};
	pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount          = static_cast<uint32_t>(stages.size());
	pipelineInfo.pStages             = stages.data();
	pipelineInfo.pVertexInputState   = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState      = &viewportState;
	pipelineInfo.pRasterizationState = &rasterization;
	pipelineInfo.pMultisampleState   = &multisample;
	pipelineInfo.pDepthStencilState  = &depthStencil;
	pipelineInfo.pColorBlendState    = &colorBlend;
	pipelineInfo.pDynamicState       = &dynamic;
	pipelineInfo.layout              = state->pipelineLayout;
	pipelineInfo.renderPass          = state->renderPass;
	return vkCreateGraphicsPipelines(state->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, output);
}

bool VerifyReadbackGuards(const ProbeState& state)
{
	const auto* bytes = static_cast<const uint8_t*>(state.readback.mapped);
	uint32_t mismatches = 0;
	for (VkDeviceSize offset = 0; offset < kGuardBytes; ++offset)
	{
		if (bytes[offset] != kGuardByte) ++mismatches;
	}
	for (VkDeviceSize offset = kGuardBytes + kReadbackPayloadBytes; offset < kReadbackBufferBytes;
	     ++offset)
	{
		if (bytes[offset] != kGuardByte) ++mismatches;
	}
	if (mismatches != 0)
	{
		std::fprintf(stderr, "depth readback guards changed: mismatches=%u\n", mismatches);
		return false;
	}
	return true;
}

int DrawBatchAndReadback(ProbeState* state, const DrawSpec* draws, uint32_t drawCount,
	                     std::array<float, kPixelCount>* output)
{
	if (drawCount == 0 || drawCount > kMaxDrawsPerBatch)
		return Fail("Depth draw batch exceeds probe capacity");
	std::array<std::array<float, 4>, kMaxDrawsPerBatch * 3> vertices {};
	for (uint32_t index = 0; index < drawCount; ++index)
	{
		const auto& draw = draws[index];
		vertices[index * 3]     = {-1.0f, -1.0f, draw.baseDepth, 1.0f};
		vertices[index * 3 + 1] = {3.0f, -1.0f, draw.baseDepth + 2.0f * draw.gradient, 1.0f};
		vertices[index * 3 + 2] = {-1.0f, 3.0f, draw.baseDepth, 1.0f};
	}
	std::memcpy(state->vertices.mapped, vertices.data(), sizeof(vertices));
	VkResult result = FlushBuffer(*state, state->vertices);
	if (result != VK_SUCCESS) return Fail("Flush vertex buffer failed", result);
	auto* readbackBytes = static_cast<uint8_t*>(state->readback.mapped);
	std::memset(readbackBytes, kGuardByte, static_cast<size_t>(kReadbackBufferBytes));
	std::memset(readbackBytes + kGuardBytes, kReadbackUnsetByte,
	            static_cast<size_t>(kReadbackPayloadBytes));
	result = FlushBuffer(*state, state->readback);
	if (result != VK_SUCCESS) return Fail("Flush readback buffer failed", result);

	VkCommandBufferAllocateInfo allocationInfo {};
	allocationInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocationInfo.commandPool        = state->commandPool;
	allocationInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocationInfo.commandBufferCount = 1;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	result = vkAllocateCommandBuffers(state->device, &allocationInfo, &commandBuffer);
	if (result != VK_SUCCESS) return Fail("vkAllocateCommandBuffers failed", result);
	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
	{
		vkFreeCommandBuffers(state->device, state->commandPool, 1, &commandBuffer);
		return Fail("vkBeginCommandBuffer failed", result);
	}

	VkImageMemoryBarrier toAttachment {};
	toAttachment.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toAttachment.srcAccessMask       = state->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
	toAttachment.dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
	                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	toAttachment.oldLayout           = state->imageLayout;
	toAttachment.newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toAttachment.image               = state->depthImage;
	toAttachment.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(commandBuffer,
	                     state->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
	                                                                   : VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &toAttachment);

	VkClearValue clearValue {};
	clearValue.depthStencil.depth   = 1.0f;
	clearValue.depthStencil.stencil = 0;
	VkRenderPassBeginInfo renderBegin {};
	renderBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderBegin.renderPass        = state->renderPass;
	renderBegin.framebuffer       = state->framebuffer;
	renderBegin.renderArea.extent = {kWidth, kHeight};
	renderBegin.clearValueCount   = 1;
	renderBegin.pClearValues      = &clearValue;
	vkCmdBeginRenderPass(commandBuffer, &renderBegin, VK_SUBPASS_CONTENTS_INLINE);
	constexpr VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers(commandBuffer, 0, 1, &state->vertices.buffer, &vertexOffset);
	for (uint32_t index = 0; index < drawCount; ++index)
	{
		vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, draws[index].pipeline);
		vkCmdSetScissor(commandBuffer, 0, 1, &draws[index].scissor);
		vkCmdDraw(commandBuffer, 3, 1, index * 3, 0);
	}
	vkCmdEndRenderPass(commandBuffer);

	VkImageMemoryBarrier toTransfer {};
	toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toTransfer.srcAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
	toTransfer.oldLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.image               = state->depthImage;
	toTransfer.subresourceRange    = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
	VkBufferImageCopy copy {};
	copy.bufferOffset                = kGuardBytes;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent                 = {kWidth, kHeight, 1};
	vkCmdCopyImageToBuffer(commandBuffer, state->depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       state->readback.buffer, 1, &copy);
	VkBufferMemoryBarrier toHost {};
	toHost.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	toHost.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	toHost.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toHost.buffer              = state->readback.buffer;
	toHost.offset              = kGuardBytes;
	toHost.size                = kReadbackPayloadBytes;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 0, nullptr, 1, &toHost, 0, nullptr);
	result = vkEndCommandBuffer(commandBuffer);
	if (result == VK_SUCCESS)
	{
		VkSubmitInfo submitInfo {};
		submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers    = &commandBuffer;
		result = vkQueueSubmit(state->queue, 1, &submitInfo, state->fence);
	}
	if (result == VK_SUCCESS)
		result = vkWaitForFences(state->device, 1, &state->fence, VK_TRUE,
		                         kCompletionTimeoutNanoseconds);
	if (result == VK_SUCCESS) result = vkResetFences(state->device, 1, &state->fence);
	if (result == VK_SUCCESS) state->imageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &commandBuffer);
	if (result != VK_SUCCESS) return Fail("Depth draw/readback submission failed", result);
	result = InvalidateBuffer(*state, state->readback);
	if (result != VK_SUCCESS) return Fail("Invalidate readback buffer failed", result);
	if (!VerifyReadbackGuards(*state)) return 2;
	std::memcpy(output->data(), readbackBytes + kGuardBytes, static_cast<size_t>(kReadbackPayloadBytes));
	return 0;
}

int DrawAndReadback(ProbeState* state, VkPipeline pipeline, float baseDepth, float gradient,
	                std::array<float, kPixelCount>* output)
{
	const DrawSpec draw {pipeline, baseDepth, gradient, {{0, 0}, {kWidth, kHeight}}};
	return DrawBatchAndReadback(state, &draw, 1, output);
}

bool VerifyCase(float baseDepth, const std::array<float, kPixelCount>& offOriginal,
	            const std::array<float, kPixelCount>& offTranslated,
	            const std::array<float, kPixelCount>& absoluteTranslated,
	            const std::array<float, kPixelCount>& formatRelative)
{
	constexpr float analyticTolerance = 2.0e-6f;
	constexpr float deltaTolerance    = 5.0e-7f;
	const float depthScale = kOriginalMaxDepth - kOriginalMinDepth;
	const float expectedSlopeDelta = kSlopeFactor * std::fabs(depthScale * kGradient / kWidth);
	uint32_t mismatches = 0;
	float maxOffError = 0.0f;
	float maxAbsoluteDeltaError = 0.0f;
	float maxSlopeError = 0.0f;
	float maxRelativeError = 0.0f;
	for (uint32_t y = 0; y < kHeight; ++y)
	{
		for (uint32_t x = 0; x < kWidth; ++x)
		{
			const size_t index = y * kWidth + x;
			const float ndcDepth = baseDepth + kGradient * (static_cast<float>(x) + 0.5f) / kWidth;
			const float expectedOff = kOriginalMinDepth + depthScale * ndcDepth;
			const float expectedTranslated = kTranslatedMinDepth + depthScale * ndcDepth;
			const float offError = std::fabs(offOriginal[index] - expectedOff);
			const float translatedError = std::fabs(offTranslated[index] - expectedTranslated);
			const float absoluteDeltaError =
			    std::fabs((offTranslated[index] - offOriginal[index]) - kAbsoluteOffset);
			const float slopeError =
			    std::fabs((absoluteTranslated[index] - offTranslated[index]) - expectedSlopeDelta);
			const float totalDeltaError = std::fabs(
			    (absoluteTranslated[index] - offOriginal[index]) - (kAbsoluteOffset + expectedSlopeDelta));
			const float relativeUnit = std::ldexp(1.0f, std::ilogb(offOriginal[index]) - 23);
			const float relativeError =
			    std::fabs((formatRelative[index] - offOriginal[index]) + relativeUnit);
			maxOffError = std::max(maxOffError, std::max(offError, translatedError));
			maxAbsoluteDeltaError =
			    std::max(maxAbsoluteDeltaError, std::max(absoluteDeltaError, totalDeltaError));
			maxSlopeError = std::max(maxSlopeError, slopeError);
			maxRelativeError = std::max(maxRelativeError, relativeError);
			const bool valid = std::isfinite(offOriginal[index]) && std::isfinite(offTranslated[index]) &&
			                   std::isfinite(absoluteTranslated[index]) &&
			                   std::isfinite(formatRelative[index]) && offError <= analyticTolerance &&
			                   translatedError <= analyticTolerance && absoluteDeltaError <= deltaTolerance &&
			                   slopeError <= deltaTolerance && totalDeltaError <= deltaTolerance &&
			                   relativeError <= relativeUnit * 0.25f + 1.0e-8f;
			if (!valid)
			{
				if (mismatches < 8)
				{
					std::fprintf(stderr,
					             "base=%.6f x=%u y=%u off=%.9g translated=%.9g absolute=%.9g "
					             "relative=%.9g errors(off=%.3g abs=%.3g slope=%.3g rel=%.3g)\n",
					             baseDepth, x, y, offOriginal[index], offTranslated[index],
					             absoluteTranslated[index], formatRelative[index], offError,
					             absoluteDeltaError, slopeError, relativeError);
				}
				++mismatches;
			}
		}
	}
	std::printf(
	    "[GFX_ABSOLUTE_DEPTH_BIAS_CASE] base=%.3f pixels=%u max_off_error=%.9g "
	    "max_absolute_delta_error=%.9g max_slope_error=%.9g max_relative_error=%.9g status=%s\n",
	    baseDepth, kPixelCount, maxOffError, maxAbsoluteDeltaError, maxSlopeError,
	    maxRelativeError, mismatches == 0 ? "clean" : "mismatch");
	return mismatches == 0;
}

enum class OcclusionMode : uint32_t
{
	Off,
	Absolute,
	Relative,
};

constexpr std::array<PipelineIndex, 3> kOcclusionPipelines {
    OcclusionOff, OcclusionAbsolute, OcclusionRelative};
constexpr std::array<const char*, 3> kOcclusionModeNames {"off", "absolute", "format-relative"};
constexpr float kObjectBaseDepth = 0.75f;
constexpr float kOccluderBaseDepth = 0.5f;
constexpr VkRect2D kBodyScissor {{8, 4}, {48, 4}};
constexpr VkRect2D kLidScissor {{8, 0}, {48, 4}};
constexpr VkRect2D kOccluderScissor {{16, 0}, {32, 8}};

bool VerifyOcclusionCase(const std::array<float, kPixelCount>& depths, OcclusionMode mode,
                         bool coplanar, const char* order)
{
	constexpr float tolerance = 2.0e-6f;
	const float depthScale = kOriginalMaxDepth - kOriginalMinDepth;
	const float slopeDelta = kSlopeFactor * std::fabs(depthScale * kGradient / kWidth);
	uint32_t mismatches = 0;
	float maxError = 0.0f;
	for (uint32_t y = 0; y < kHeight; ++y)
	{
		for (uint32_t x = 0; x < kWidth; ++x)
		{
			// Two scissored faces are the projected body/lid of the synthetic object.
			// The wall covers their center; both uncovered edges prove the faces were drawn.
			const bool object = x >= 8 && x < 56;
			const bool wall = x >= 16 && x < 48;
			const float ndc = kObjectBaseDepth + kGradient * (static_cast<float>(x) + 0.5f) / kWidth;
			const float objectDepth = kOriginalMinDepth + depthScale * ndc;
			float expected = 1.0f;
			if (object)
			{
				expected = objectDepth;
				if (y < 4 && mode != OcclusionMode::Off)
				{
					const float constantDelta = mode == OcclusionMode::Absolute
					                                ? kAbsoluteOffset
					                                : -0.3f * std::ldexp(1.0f, std::ilogb(objectDepth) - 23);
					expected += constantDelta + slopeDelta;
				}
			}
			if (wall)
			{
				const float wallDepth = coplanar ? objectDepth
				                                 : kOriginalMinDepth + depthScale * kOccluderBaseDepth;
				expected = std::min(expected, wallDepth);
			}
			const float actual = depths[y * kWidth + x];
			const float error = std::fabs(actual - expected);
			maxError = std::max(maxError, error);
			if (!std::isfinite(actual) || error > tolerance)
			{
				if (mismatches < 8)
					std::fprintf(stderr,
					             "occlusion scene=%s mode=%s order=%s x=%u y=%u actual=%.9g "
					             "expected=%.9g error=%.3g\n",
					             coplanar ? "coplanar" : "separated",
					             kOcclusionModeNames[static_cast<uint32_t>(mode)], order, x, y, actual,
					             expected, error);
				++mismatches;
			}
		}
	}
	std::printf("[GFX_DEPTH_BIAS_OCCLUSION_CASE] scene=%s mode=%s order=%s pixels=%u "
	            "max_error=%.9g status=%s\n",
	            coplanar ? "coplanar" : "separated",
	            kOcclusionModeNames[static_cast<uint32_t>(mode)], order, kPixelCount, maxError,
	            mismatches == 0 ? "clean" : "mismatch");
	return mismatches == 0;
}

bool RunOcclusionCases(ProbeState* state)
{
	bool clean = true;
	uint32_t cases = 0;
	const DrawSpec body {state->pipelines[OcclusionOff], kObjectBaseDepth, kGradient, kBodyScissor};
	for (bool coplanar: {false, true})
	{
		const DrawSpec wall {state->pipelines[OcclusionOff],
		                     coplanar ? kObjectBaseDepth : kOccluderBaseDepth,
		                     coplanar ? kGradient : 0.0f, kOccluderScissor};
		for (OcclusionMode mode: {OcclusionMode::Off, OcclusionMode::Absolute, OcclusionMode::Relative})
		{
			const DrawSpec lid {
			    state->pipelines[kOcclusionPipelines[static_cast<uint32_t>(mode)]],
			    kObjectBaseDepth, kGradient, kLidScissor};
			for (bool wallFirst: {true, false})
			{
				const std::array<DrawSpec, 3> draws = wallFirst ? std::array<DrawSpec, 3> {wall, body, lid}
				                                                : std::array<DrawSpec, 3> {body, lid, wall};
				std::array<float, kPixelCount> depths {};
				if (DrawBatchAndReadback(state, draws.data(), static_cast<uint32_t>(draws.size()), &depths) != 0)
					return false;
				clean = VerifyOcclusionCase(depths, mode, coplanar,
				                            wallFirst ? "wall-first" : "wall-last") && clean;
				++cases;
			}
		}
	}
	// No render-pass boundary or clear occurs between the biased lid and unbiased body.
	// Their uncovered edges expose leaked slope/viewport state as well as lost occlusion.
	const DrawSpec wall {state->pipelines[OcclusionOff], kOccluderBaseDepth, 0.0f, kOccluderScissor};
	for (OcclusionMode mode: {OcclusionMode::Absolute, OcclusionMode::Relative})
	{
		const DrawSpec lid {state->pipelines[kOcclusionPipelines[static_cast<uint32_t>(mode)]],
		                    kObjectBaseDepth, kGradient, kLidScissor};
		const std::array<DrawSpec, 3> draws {wall, lid, body};
		std::array<float, kPixelCount> depths {};
		if (DrawBatchAndReadback(state, draws.data(), static_cast<uint32_t>(draws.size()), &depths) != 0)
			return false;
		clean = VerifyOcclusionCase(depths, mode, false, "on-to-off") && clean;
		++cases;
	}
	std::printf("[GFX_DEPTH_BIAS_OCCLUSION] format=D32_SFLOAT_S8_UINT compare=less-or-equal "
	            "cases=%u scenes=separated+coplanar orders=both transitions=absolute-to-off+relative-to-off "
	            "status=%s\n", cases, clean ? "clean" : "mismatch");
	return clean;
}

int Initialize(ProbeState* state)
{
	VkApplicationInfo applicationInfo {};
	applicationInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName = "Lumen absolute depth bias Vulkan probe";
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
			constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
			if (families[family].queueCount != 0 &&
			    (families[family].queueFlags & required) == required)
			{
				state->physicalDevice = physicalDevices[deviceIndex];
				state->queueFamily    = family;
				break;
			}
		}
		if (state->physicalDevice != VK_NULL_HANDLE) break;
	}
	if (state->physicalDevice == VK_NULL_HANDLE)
		return Fail("No graphics/transfer Native Metal queue found");
	VkFormatProperties formatProperties {};
	vkGetPhysicalDeviceFormatProperties(state->physicalDevice, VK_FORMAT_D32_SFLOAT_S8_UINT,
	                                    &formatProperties);
	constexpr VkFormatFeatureFlags requiredFormatFeatures =
	    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if ((formatProperties.optimalTilingFeatures & requiredFormatFeatures) != requiredFormatFeatures)
		return Fail("D32_SFLOAT_S8_UINT lacks depth/transfer-source support");

	constexpr float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = state->queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &priority;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos    = &queueInfo;
	result = vkCreateDevice(state->physicalDevice, &deviceInfo, nullptr, &state->device);
	if (result != VK_SUCCESS) return Fail("vkCreateDevice failed", result);
	vkGetDeviceQueue(state->device, state->queueFamily, 0, &state->queue);
	if (state->queue == VK_NULL_HANDLE) return Fail("vkGetDeviceQueue returned null");

	result = CreateHostBuffer(state, kVertexBufferBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                          &state->vertices);
	if (result != VK_SUCCESS) return Fail("CreateHostBuffer(vertices) failed", result);
	result = CreateHostBuffer(state, kReadbackBufferBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                          &state->readback);
	if (result != VK_SUCCESS) return Fail("CreateHostBuffer(readback) failed", result);

	VkImageCreateInfo imageInfo {};
	imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType     = VK_IMAGE_TYPE_2D;
	imageInfo.format        = VK_FORMAT_D32_SFLOAT_S8_UINT;
	imageInfo.extent        = {kWidth, kHeight, 1};
	imageInfo.mipLevels     = 1;
	imageInfo.arrayLayers   = 1;
	imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(state->device, &imageInfo, nullptr, &state->depthImage);
	if (result != VK_SUCCESS) return Fail("vkCreateImage(D32S8) failed", result);
	VkMemoryRequirements imageRequirements {};
	vkGetImageMemoryRequirements(state->device, state->depthImage, &imageRequirements);
	const uint32_t imageMemoryType = FindMemoryType(state->physicalDevice,
	                                                imageRequirements.memoryTypeBits,
	                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (imageMemoryType == UINT32_MAX) return Fail("No device-local memory type for D32S8 image");
	VkMemoryAllocateInfo imageAllocation {};
	imageAllocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	imageAllocation.allocationSize  = imageRequirements.size;
	imageAllocation.memoryTypeIndex = imageMemoryType;
	result = vkAllocateMemory(state->device, &imageAllocation, nullptr, &state->depthMemory);
	if (result != VK_SUCCESS) return Fail("vkAllocateMemory(D32S8) failed", result);
	result = vkBindImageMemory(state->device, state->depthImage, state->depthMemory, 0);
	if (result != VK_SUCCESS) return Fail("vkBindImageMemory(D32S8) failed", result);
	VkImageViewCreateInfo viewInfo {};
	viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image                           = state->depthImage;
	viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format                          = VK_FORMAT_D32_SFLOAT_S8_UINT;
	viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	viewInfo.subresourceRange.baseMipLevel   = 0;
	viewInfo.subresourceRange.levelCount     = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount     = 1;
	result = vkCreateImageView(state->device, &viewInfo, nullptr, &state->depthView);
	if (result != VK_SUCCESS) return Fail("vkCreateImageView(D32S8) failed", result);

	VkAttachmentDescription attachment {};
	attachment.format         = VK_FORMAT_D32_SFLOAT_S8_UINT;
	attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	attachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkAttachmentReference attachmentReference {};
	attachmentReference.attachment = 0;
	attachmentReference.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	VkSubpassDescription subpass {};
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.pDepthStencilAttachment = &attachmentReference;
	VkRenderPassCreateInfo renderPassInfo {};
	renderPassInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 1;
	renderPassInfo.pAttachments    = &attachment;
	renderPassInfo.subpassCount    = 1;
	renderPassInfo.pSubpasses      = &subpass;
	result = vkCreateRenderPass(state->device, &renderPassInfo, nullptr, &state->renderPass);
	if (result != VK_SUCCESS) return Fail("vkCreateRenderPass failed", result);
	VkFramebufferCreateInfo framebufferInfo {};
	framebufferInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass      = state->renderPass;
	framebufferInfo.attachmentCount = 1;
	framebufferInfo.pAttachments    = &state->depthView;
	framebufferInfo.width           = kWidth;
	framebufferInfo.height          = kHeight;
	framebufferInfo.layers          = 1;
	result = vkCreateFramebuffer(state->device, &framebufferInfo, nullptr, &state->framebuffer);
	if (result != VK_SUCCESS) return Fail("vkCreateFramebuffer failed", result);
	VkPipelineLayoutCreateInfo layoutInfo {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	result = vkCreatePipelineLayout(state->device, &layoutInfo, nullptr, &state->pipelineLayout);
	if (result != VK_SUCCESS) return Fail("vkCreatePipelineLayout failed", result);

	for (uint32_t index = 0; index < state->shaders.size(); ++index)
	{
		std::vector<uint32_t> binary;
		if (!AssembleShader(index == 0 ? kVertexShaderAssembly : kFragmentShaderAssembly, &binary))
			return Fail("SPIR-V assembly/validation failed");
		VkShaderModuleCreateInfo shaderInfo {};
		shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		shaderInfo.codeSize = binary.size() * sizeof(uint32_t);
		shaderInfo.pCode    = binary.data();
		result = vkCreateShaderModule(state->device, &shaderInfo, nullptr, &state->shaders[index]);
		if (result != VK_SUCCESS) return Fail("vkCreateShaderModule failed", result);
	}
	result = CreatePipeline(state, kOriginalMinDepth, kOriginalMaxDepth, VK_FALSE, 0.0f, 0.0f,
	                        &state->pipelines[OffOriginal]);
	if (result != VK_SUCCESS) return Fail("CreatePipeline(off original) failed", result);
	result = CreatePipeline(state, kTranslatedMinDepth, kTranslatedMaxDepth, VK_FALSE, 0.0f, 0.0f,
	                        &state->pipelines[OffTranslated]);
	if (result != VK_SUCCESS) return Fail("CreatePipeline(off translated) failed", result);
	result = CreatePipeline(state, kTranslatedMinDepth, kTranslatedMaxDepth, VK_TRUE, 0.0f,
	                        kSlopeFactor, &state->pipelines[AbsoluteTranslated]);
	if (result != VK_SUCCESS) return Fail("CreatePipeline(absolute translated) failed", result);
	result = CreatePipeline(state, kOriginalMinDepth, kOriginalMaxDepth, VK_TRUE, -1.0f, 0.0f,
	                        &state->pipelines[FormatRelative]);
	if (result != VK_SUCCESS) return Fail("CreatePipeline(format relative) failed", result);
	result = CreatePipeline(state, kOriginalMinDepth, kOriginalMaxDepth, VK_FALSE, 0.0f, 0.0f,
	                        &state->pipelines[OcclusionOff], VK_COMPARE_OP_LESS_OR_EQUAL);
	if (result != VK_SUCCESS) return Fail("CreatePipeline(occlusion off) failed", result);
	result = CreatePipeline(state, kTranslatedMinDepth, kTranslatedMaxDepth, VK_TRUE, 0.0f,
	                        kSlopeFactor, &state->pipelines[OcclusionAbsolute], VK_COMPARE_OP_LESS_OR_EQUAL);
	if (result != VK_SUCCESS) return Fail("CreatePipeline(occlusion absolute) failed", result);
	result = CreatePipeline(state, kOriginalMinDepth, kOriginalMaxDepth, VK_TRUE, -0.3f,
	                        kSlopeFactor, &state->pipelines[OcclusionRelative], VK_COMPARE_OP_LESS_OR_EQUAL);
	if (result != VK_SUCCESS) return Fail("CreatePipeline(occlusion relative) failed", result);

	VkCommandPoolCreateInfo commandPoolInfo {};
	commandPoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	commandPoolInfo.queueFamilyIndex = state->queueFamily;
	result = vkCreateCommandPool(state->device, &commandPoolInfo, nullptr, &state->commandPool);
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
	ProbeState state;
	if (Initialize(&state) != 0) return 1;
	bool clean = true;
	for (float baseDepth: kBaseDepths)
	{
		std::array<float, kPixelCount> offOriginal {};
		std::array<float, kPixelCount> offTranslated {};
		std::array<float, kPixelCount> absoluteTranslated {};
		std::array<float, kPixelCount> formatRelative {};
		if (DrawAndReadback(&state, state.pipelines[OffOriginal], baseDepth, kGradient,
		                    &offOriginal) != 0 ||
		    DrawAndReadback(&state, state.pipelines[OffTranslated], baseDepth, kGradient,
		                    &offTranslated) != 0 ||
		    DrawAndReadback(&state, state.pipelines[AbsoluteTranslated], baseDepth, kGradient,
		                    &absoluteTranslated) != 0 ||
		    DrawAndReadback(&state, state.pipelines[FormatRelative], baseDepth, kGradient,
		                    &formatRelative) != 0)
		{
			return 1;
		}
		clean = VerifyCase(baseDepth, offOriginal, offTranslated, absoluteTranslated,
		                   formatRelative) && clean;
	}
	if (!clean || !RunOcclusionCases(&state)) return 2;
	const int marker = std::printf(
	    "[GFX_ABSOLUTE_DEPTH_BIAS_VULKAN_PROBE] format=D32_SFLOAT_S8_UINT "
	    "viewport_original=0.5:1 viewport_translated=0.2:0.7 absolute_offset=-0.3 "
	    "native_constant=0 native_slope=-0.3 format_relative_constant=-1 bases=3 "
	    "modes=on+off+format-relative status=clean\n");
	if (marker <= 0 || std::fflush(stdout) != 0)
		return Fail("Failed to emit absolute depth-bias marker");
	return 0;
}
