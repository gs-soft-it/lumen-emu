#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/GraphicContext.h"
#include "Emulator/Graphics/GuestDataLeaseRetry.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"

#include "Lumen/Core/VirtualMemory.h"

#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"
#include <xxhash/xxhash.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" void LumenMetalGetCompletionPassReuseStatsV1(uint64_t* logical, uint64_t* physical, uint64_t* merged,
                                                        uint64_t* mergedAfterMarker, uint32_t* enabled);
extern "C" bool LumenMetalResourceAliasesRenderAttachmentForTest(VkImageView attachment, VkImageView sampled, VkBuffer buffer);

namespace {
constexpr uint32_t                          kWidth                        = 64;
constexpr uint32_t                          kHeight                       = 8;
constexpr uint32_t                          kPixelCount                   = kWidth * kHeight;
constexpr VkDeviceSize                      kGuardBytes                   = 256;
constexpr VkDeviceSize                      kPlaneBytes                   = kPixelCount * sizeof(uint32_t);
constexpr VkDeviceSize                      kReadbackPayloadBytes         = 2 * kPlaneBytes + kPixelCount;
constexpr VkDeviceSize                      kReadbackBufferBytes          = kGuardBytes + kReadbackPayloadBytes + kGuardBytes;
constexpr VkDeviceSize                      kVertexBufferBytes            = 9 * 4 * sizeof(float);
constexpr uint64_t                          kCompletionTimeoutNanoseconds = 5'000'000'000ull;
constexpr uint8_t                           kGuardByte                    = 0xa5;
constexpr float                             kNear                         = 0.25f;
constexpr float                             kFar                          = 0.75f;
constexpr std::array<VkFormat, 2>           kFormats {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT_S8_UINT};
constexpr std::array<VkImageAspectFlags, 2> kAspects {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
constexpr std::array<VkImageLayout, 2>      kAttachmentLayouts {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

bool WaitForCompletionEvent(VkDevice device, VkEvent event)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(kCompletionTimeoutNanoseconds);
	for (;;)
	{
		const VkResult status = vkGetEventStatus(device, event);
		if (status == VK_EVENT_SET) return true;
		if (status != VK_EVENT_RESET || std::chrono::steady_clock::now() >= deadline) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

// The vertex harness supplies only full-screen geometry and fixed near/far depth.
// Every fragment module below comes from the runtime Gen4 parser and lowerer.
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

enum PipelineIndex : size_t
{
	CutoutEarly,
	CutoutLate,
	OpaqueNear,
	Background,
	ForcedEarlyControl,
	StencilWriter,
	StencilMaskedColor,
	StencilZeroPreserving,
	PipelineCount
};
struct HostBuffer
{
	VkBuffer       buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void*          mapped = nullptr;
};
struct ProbeState
{
	VkInstance                                    instance       = VK_NULL_HANDLE;
	VkPhysicalDevice                              physicalDevice = VK_NULL_HANDLE;
	VkDevice                                      device         = VK_NULL_HANDLE;
	VkQueue                                       queue          = VK_NULL_HANDLE;
	uint32_t                                      queueFamily    = UINT32_MAX;
	HostBuffer                                    vertices, readback;
	std::array<VkImage, 2>                        images {};
	std::array<VkDeviceMemory, 2>                 imageMemory {};
	std::array<VkImageView, 2>                    views {};
	VkRenderPass                                  renderPass     = VK_NULL_HANDLE;
	VkRenderPass                                  loadRenderPass = VK_NULL_HANDLE;
	VkFramebuffer                                 framebuffer    = VK_NULL_HANDLE;
	VkPipelineLayout                              pipelineLayout = VK_NULL_HANDLE;
	std::array<VkShaderModule, PipelineCount + 1> shaders {};
	std::array<VkPipeline, PipelineCount>         pipelines {};
	VkCommandPool                                 commandPool = VK_NULL_HANDLE;
	VkFence                                       fence       = VK_NULL_HANDLE;
	bool                                          firstDraw   = true;
	~ProbeState()
	{
		if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
		if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
		for (auto p: pipelines)
			if (p != VK_NULL_HANDLE) vkDestroyPipeline(device, p, nullptr);
		for (auto s: shaders)
			if (s != VK_NULL_HANDLE) vkDestroyShaderModule(device, s, nullptr);
		if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if (renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, renderPass, nullptr);
		if (loadRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, loadRenderPass, nullptr);
		for (size_t i = 0; i < images.size(); ++i)
		{
			if (views[i] != VK_NULL_HANDLE) vkDestroyImageView(device, views[i], nullptr);
			if (images[i] != VK_NULL_HANDLE) vkDestroyImage(device, images[i], nullptr);
			if (imageMemory[i] != VK_NULL_HANDLE) vkFreeMemory(device, imageMemory[i], nullptr);
		}
		for (HostBuffer* b: {&vertices, &readback})
		{
			if (b->mapped != nullptr) vkUnmapMemory(device, b->memory);
			if (b->buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, b->buffer, nullptr);
			if (b->memory != VK_NULL_HANDLE) vkFreeMemory(device, b->memory, nullptr);
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

VkResult CreateHostBuffer(ProbeState* state, VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer* output)
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
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result                         = vkAllocateMemory(state->device, &allocationInfo, nullptr, &output->memory);
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
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                         { std::fprintf(stderr, "SPIR-V line %zu: %s\n", position.line, message); });
	return tools.Assemble(assembly, std::strlen(assembly), binary) && tools.Validate(*binary);
}

bool RuntimeFragment(PipelineIndex variant, std::vector<uint32_t>* binary)
{
	using namespace Lumen::Libs::Graphics;
	const bool cutout     = variant == CutoutEarly || variant == CutoutLate || variant == ForcedEarlyControl;
	const bool background = variant == Background;
	// V_MOV_B32 initializes MRT0 v4:v7 before EXEC changes. V_CMPX_LT_F32
	// compares literal 32 with FragCoord.x (v2), leaving only the right half active.
	std::vector<uint32_t> words {
	    0x7e0802ffu, background ? 0u : 0x3f800000u, 0x7e0a0280u, 0x7e0c02ffu, background ? 0x3f800000u : 0u, 0x7e0e02f2u,
	};
	if (cutout)
	{
		words.push_back(0x7c000000u | (0x11u << 17u) | (2u << 9u) | 255u);
		words.push_back(0x42000000u);
	}
	words.push_back(cutout ? 0xf800180fu : 0xf800080fu);
	words.push_back(0x07060504u);
	words.push_back(0xbf810000u);
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	ShaderParse(words.data(), &code);
	ShaderPixelInputInfo input {};
	input.ps_pos_xy  = cutout;
	input.ps_early_z = variant != CutoutLate;
	// Exercise the decoded export itself: missing KILL_ENABLE must not make
	// VM&&DONE unsafe. The state test separately covers KILL_ENABLE metadata.
	input.ps_pixel_kill_enable  = false;
	input.target_output_mode[0] = 9;
	input.shader_mask           = 0xf;
	if (variant == ForcedEarlyControl)
	{
		const auto        source = SpirvGenerateSource(code, nullptr, &input, nullptr);
		std::string       text(source.c_str(), source.Size());
		const std::string origin = "OpExecutionMode %main OriginUpperLeft";
		const auto        offset = text.find(origin);
		if (offset == std::string::npos || text.find("EarlyFragmentTests") != std::string::npos) return false;
		text.insert(offset + origin.size(), "\nOpExecutionMode %main EarlyFragmentTests");
		return AssembleShader(text.c_str(), binary);
	}
	const auto module = ShaderRecompilePS(code, &input);
	binary->assign(module.GetDataConst(), module.GetDataConst() + module.Size());
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	return validator.Validate(*binary);
}
VkResult CreatePipeline(ProbeState* state, PipelineIndex variant, VkPipeline* output,
                         VkShaderModule fragment = VK_NULL_HANDLE, VkPipelineLayout layout = VK_NULL_HANDLE)
{
	std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
	for (uint32_t index = 0; index < stages.size(); ++index)
	{
		stages[index].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[index].stage  = index == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[index].module = state->shaders[index == 0 ? 0 : variant + 1];
		if (index == 1 && fragment != VK_NULL_HANDLE) stages[index].module = fragment;
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
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
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
	rasterization.depthBiasEnable         = VK_FALSE;
	rasterization.depthBiasConstantFactor = 0.0f;
	rasterization.depthBiasClamp          = 0.0f;
	rasterization.depthBiasSlopeFactor    = 0.0f;
	rasterization.lineWidth               = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample {};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depthStencil {};
	depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable  = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;
	const bool stencilVariant = variant == StencilWriter || variant == StencilMaskedColor || variant == StencilZeroPreserving;
	if (stencilVariant)
	{
		depthStencil.depthTestEnable = depthStencil.depthWriteEnable = VK_FALSE;
		depthStencil.stencilTestEnable = VK_TRUE;
		depthStencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
		                      VK_COMPARE_OP_EQUAL, 0xff, 0xff, 0xff};
		depthStencil.back = depthStencil.front;
	}
	if (variant == StencilZeroPreserving)
	{
		depthStencil.front = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
		                      VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};
		depthStencil.back = depthStencil.front;
	}
	VkPipelineColorBlendStateCreateInfo colorBlend {};
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	VkPipelineColorBlendAttachmentState blendAttachment {};
	blendAttachment.colorWriteMask                = variant == StencilWriter || variant == StencilZeroPreserving ? 0 : 0xf;
	colorBlend.attachmentCount                    = 1;
	colorBlend.pAttachments                       = &blendAttachment;
	constexpr std::array<VkDynamicState, 4> dynamicStates {VK_DYNAMIC_STATE_SCISSOR,
	    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
	VkPipelineDynamicStateCreateInfo dynamic {};
	dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = stencilVariant ? dynamicStates.size() : 1;
	dynamic.pDynamicStates    = dynamicStates.data();
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
	pipelineInfo.layout              = layout == VK_NULL_HANDLE ? state->pipelineLayout : layout;
	pipelineInfo.renderPass          = state->renderPass;
	return vkCreateGraphicsPipelines(state->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, output);
}

int Initialize(ProbeState* state, VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM)
{
	auto formats = kFormats;
	formats[0] = colorFormat;
	VkApplicationInfo applicationInfo {};
	applicationInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName = "Lumen pixel discard depth probe";
	applicationInfo.pEngineName      = "Lumen";
	applicationInfo.apiVersion       = VK_API_VERSION_1_1;
	VkInstanceCreateInfo instanceInfo {};
	instanceInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &applicationInfo;
	VkResult result               = vkCreateInstance(&instanceInfo, nullptr, &state->instance);
	if (result != VK_SUCCESS) return Fail("vkCreateInstance failed", result);

	uint32_t physicalDeviceCount = 0;
	result                       = vkEnumeratePhysicalDevices(state->instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0) return Fail("No Native Metal physical device found", result);
	std::array<VkPhysicalDevice, 8> physicalDevices {};
	if (physicalDeviceCount > physicalDevices.size()) return Fail("Native Metal physical-device count exceeds probe capacity");
	result = vkEnumeratePhysicalDevices(state->instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS) return Fail("vkEnumeratePhysicalDevices(list) failed", result);
	for (uint32_t deviceIndex = 0; deviceIndex < physicalDeviceCount; ++deviceIndex)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[deviceIndex], &familyCount, nullptr);
		std::array<VkQueueFamilyProperties, 8> families {};
		if (familyCount > families.size()) continue;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevices[deviceIndex], &familyCount, families.data());
		for (uint32_t family = 0; family < familyCount; ++family)
		{
			constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
			if (families[family].queueCount != 0 && (families[family].queueFlags & required) == required)
			{
				state->physicalDevice = physicalDevices[deviceIndex];
				state->queueFamily    = family;
				break;
			}
		}
		if (state->physicalDevice != VK_NULL_HANDLE) break;
	}
	if (state->physicalDevice == VK_NULL_HANDLE) return Fail("No graphics/transfer Native Metal queue found");
	VkFormatProperties formatProperties {};
	vkGetPhysicalDeviceFormatProperties(state->physicalDevice, VK_FORMAT_D32_SFLOAT_S8_UINT, &formatProperties);
	constexpr VkFormatFeatureFlags requiredFormatFeatures =
	    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if ((formatProperties.optimalTilingFeatures & requiredFormatFeatures) != requiredFormatFeatures)
		return Fail("D32_SFLOAT_S8_UINT lacks depth/transfer-source support");

	constexpr float         priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = state->queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &priority;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos    = &queueInfo;
	result                          = vkCreateDevice(state->physicalDevice, &deviceInfo, nullptr, &state->device);
	if (result != VK_SUCCESS) return Fail("vkCreateDevice failed", result);
	vkGetDeviceQueue(state->device, state->queueFamily, 0, &state->queue);
	if (state->queue == VK_NULL_HANDLE) return Fail("vkGetDeviceQueue returned null");

	result = CreateHostBuffer(state, kVertexBufferBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &state->vertices);
	if (result != VK_SUCCESS) return Fail("CreateHostBuffer(vertices) failed", result);
	result = CreateHostBuffer(state, kReadbackBufferBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &state->readback);
	if (result != VK_SUCCESS) return Fail("CreateHostBuffer(readback) failed", result);

	for (size_t i = 0; i < state->images.size(); ++i)
	{
		VkImageCreateInfo info {};
		info.sType     = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format    = formats[i];
		info.extent    = {kWidth, kHeight, 1};
		info.mipLevels = info.arrayLayers = 1;
		info.samples                      = VK_SAMPLE_COUNT_1_BIT;
		info.tiling                       = VK_IMAGE_TILING_OPTIMAL;
		info.usage =
		    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | (i == 0 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
		                                           : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
		result = vkCreateImage(state->device, &info, nullptr, &state->images[i]);
		if (result != VK_SUCCESS) return Fail("Create attachment image failed", result);
		VkMemoryRequirements requirements {};
		vkGetImageMemoryRequirements(state->device, state->images[i], &requirements);
		VkMemoryAllocateInfo allocation {};
		allocation.sType          = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex =
		    FindMemoryType(state->physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (allocation.memoryTypeIndex == UINT32_MAX) return Fail("No image memory type");
		result = vkAllocateMemory(state->device, &allocation, nullptr, &state->imageMemory[i]);
		if (result != VK_SUCCESS) return Fail("Allocate image memory failed", result);
		result = vkBindImageMemory(state->device, state->images[i], state->imageMemory[i], 0);
		if (result != VK_SUCCESS) return Fail("Bind image memory failed", result);
		VkImageViewCreateInfo view {};
		view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image            = state->images[i];
		view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
		view.format           = formats[i];
		view.subresourceRange = {kAspects[i], 0, 1, 0, 1};
		result                = vkCreateImageView(state->device, &view, nullptr, &state->views[i]);
		if (result != VK_SUCCESS) return Fail("Create image view failed", result);
	}
	std::array<VkAttachmentDescription, 2> attachments {};
	for (size_t i = 0; i < attachments.size(); ++i)
	{
		auto& a          = attachments[i];
		a.format         = formats[i];
		a.samples        = VK_SAMPLE_COUNT_1_BIT;
		a.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
		a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
		a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
		a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.initialLayout = a.finalLayout = kAttachmentLayouts[i];
	}
	VkAttachmentReference color {0, kAttachmentLayouts[0]}, depth {1, kAttachmentLayouts[1]};
	VkSubpassDescription  subpass {};
	subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount    = 1;
	subpass.pColorAttachments       = &color;
	subpass.pDepthStencilAttachment = &depth;
	VkRenderPassCreateInfo pass {};
	pass.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = attachments.size();
	pass.pAttachments    = attachments.data();
	pass.subpassCount    = 1;
	pass.pSubpasses      = &subpass;
	result               = vkCreateRenderPass(state->device, &pass, nullptr, &state->renderPass);
	if (result != VK_SUCCESS) return Fail("Create render pass failed", result);
	for (auto& attachment: attachments)
		attachment.loadOp = attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	result = vkCreateRenderPass(state->device, &pass, nullptr, &state->loadRenderPass);
	if (result != VK_SUCCESS) return Fail("Create load render pass failed", result);
	VkFramebufferCreateInfo framebuffer {};
	framebuffer.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass      = state->renderPass;
	framebuffer.attachmentCount = state->views.size();
	framebuffer.pAttachments    = state->views.data();
	framebuffer.width           = kWidth;
	framebuffer.height          = kHeight;
	framebuffer.layers          = 1;
	result                      = vkCreateFramebuffer(state->device, &framebuffer, nullptr, &state->framebuffer);
	if (result != VK_SUCCESS) return Fail("Create framebuffer failed", result);
	VkPipelineLayoutCreateInfo layout {};
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	result       = vkCreatePipelineLayout(state->device, &layout, nullptr, &state->pipelineLayout);
	if (result != VK_SUCCESS) return Fail("Create pipeline layout failed", result);
	for (size_t i = 0; i < state->shaders.size(); ++i)
	{
		std::vector<uint32_t> binary;
		if (!(i == 0 ? AssembleShader(kVertexShaderAssembly, &binary) : RuntimeFragment(static_cast<PipelineIndex>(i - 1), &binary)))
			return Fail("Runtime fragment or harness vertex SPIR-V validation failed");
		VkShaderModuleCreateInfo shader {};
		shader.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		shader.codeSize = binary.size() * sizeof(uint32_t);
		shader.pCode    = binary.data();
		result          = vkCreateShaderModule(state->device, &shader, nullptr, &state->shaders[i]);
		if (result != VK_SUCCESS) return Fail("Create shader module failed", result);
	}
	for (size_t i = 0; i < state->pipelines.size(); ++i)
	{
		result = CreatePipeline(state, static_cast<PipelineIndex>(i), &state->pipelines[i]);
		if (result != VK_SUCCESS) return Fail("Create graphics pipeline failed", result);
	}
	VkCommandPoolCreateInfo pool {};
	pool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	pool.queueFamilyIndex = state->queueFamily;
	result                = vkCreateCommandPool(state->device, &pool, nullptr, &state->commandPool);
	if (result != VK_SUCCESS) return Fail("Create command pool failed", result);
	VkFenceCreateInfo fence {};
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result      = vkCreateFence(state->device, &fence, nullptr, &state->fence);
	if (result != VK_SUCCESS) return Fail("Create fence failed", result);
	const std::array<std::array<float, 4>, 9> vertices {{
	    {{-1, -1, kNear, 1}},
	    {{3, -1, kNear, 1}},
	    {{-1, 3, kNear, 1}},
	    {{-1, -1, kFar, 1}},
	    {{3, -1, kFar, 1}},
	    {{-1, 3, kFar, 1}},
	    {{-1, -1, kNear, 1}},
	    {{-1, 3, kNear, 1}},
	    {{3, -1, kNear, 1}},
	}};
	std::memcpy(state->vertices.mapped, vertices.data(), sizeof(vertices));
	result = FlushBuffer(*state, state->vertices);
	return result == VK_SUCCESS ? 0 : Fail("Flush vertices failed", result);
}

struct StencilScenario
{
	bool legacyClearWrite;
	bool splitPass;
	bool reverseWinding;
	uint8_t clearValue;
	uint8_t maskBit;
};

struct LateUnsafeDraw
{
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkDescriptorSet set;
	VkBuffer indices;
};

struct UniformZeroScenario
{
	uint8_t reference;
	bool load;
	bool reverseWinding;
};

int DrawAndVerify(ProbeState* state, PipelineIndex foreground, bool nearFirst, const StencilScenario* stencil = nullptr,
                  bool coalescing = false, const LateUnsafeDraw* lateUnsafe = nullptr, VkEvent completionEvent = VK_NULL_HANDLE,
                  const UniformZeroScenario* uniformZero = nullptr)
{
	if (completionEvent != VK_NULL_HANDLE && vkResetEvent(state->device, completionEvent) != VK_SUCCESS)
		return Fail("Reset completion event failed");
	std::memset(state->readback.mapped, kGuardByte, kReadbackBufferBytes);
	VkResult result = FlushBuffer(*state, state->readback);
	if (result != VK_SUCCESS) return Fail("Flush readback failed", result);
	VkCommandBufferAllocateInfo allocation {};
	allocation.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool        = state->commandPool;
	allocation.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	VkCommandBuffer command       = VK_NULL_HANDLE;
	result                        = vkAllocateCommandBuffers(state->device, &allocation, &command);
	if (result != VK_SUCCESS) return Fail("Allocate command buffer failed", result);
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result      = vkBeginCommandBuffer(command, &begin);
	if (result != VK_SUCCESS) return Fail("Begin command buffer failed", result);
	std::array<VkImageMemoryBarrier, 2> barriers {};
	for (size_t i = 0; i < barriers.size(); ++i)
	{
		auto& b               = barriers[i];
		b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.srcAccessMask       = state->firstDraw ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
		b.dstAccessMask       = i == 0 ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		                               : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		b.oldLayout           = state->firstDraw ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		b.newLayout           = kAttachmentLayouts[i];
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image                                       = state->images[i];
		b.subresourceRange                            = {kAspects[i], 0, 1, 0, 1};
	}
	vkCmdPipelineBarrier(command, state->firstDraw ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
	                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
	std::array<VkClearValue, 2> clear {};
	clear[0].color.float32[3]   = 1.0f;
	clear[1].depthStencil.depth = 1.0f;
	if (uniformZero != nullptr) clear[1].depthStencil.depth = 0.0f;
	if (lateUnsafe != nullptr) clear[1].depthStencil.stencil = 0x40;
	if (stencil != nullptr)
	{
		clear[0].color.float32[2] = 1.0f;
		clear[1].depthStencil.stencil = stencil->clearValue;
	}
	VkRenderPassBeginInfo render {};
	render.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	render.renderPass        = uniformZero != nullptr && uniformZero->load ? state->loadRenderPass : state->renderPass;
	render.framebuffer       = state->framebuffer;
	render.renderArea.extent = {kWidth, kHeight};
	render.clearValueCount   = clear.size();
	render.pClearValues      = clear.data();
	vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
	if (coalescing)
	{
		// The clear is an explicit boundary. Only the next two identical LOAD
		// passes are eligible to share their physical encoder.
		vkCmdEndRenderPass(command);
		render.renderPass = state->loadRenderPass;
		vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
	}
	constexpr VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(command, 0, 1, &state->vertices.buffer, &offset);
	const VkRect2D scissor {{0, 0}, {kWidth, kHeight}};
	vkCmdSetScissor(command, 0, 1, &scissor);
	const auto splitCoalescedPass = [&] {
		vkCmdEndRenderPass(command);
		if (completionEvent != VK_NULL_HANDLE)
			vkCmdSetEvent(command, completionEvent, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		// Binding state between logical passes is legal and must not require an
		// active logical render pass merely because an encoder is retained.
		vkCmdBindVertexBuffers(command, 0, 1, &state->vertices.buffer, &offset);
		vkCmdSetScissor(command, 0, 1, &scissor);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipelines[Background]);
		vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
	};
	if (uniformZero != nullptr)
	{
		const VkRect2D partial {{kWidth / 4, kHeight / 4}, {kWidth / 2, kHeight / 2}};
		vkCmdSetScissor(command, 0, 1, &partial);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipelines[StencilZeroPreserving]);
		vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
		vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
		vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, uniformZero->reference);
		vkCmdDraw(command, 3, 1, uniformZero->reverseWinding ? 6 : 0, 0);
	} else if (stencil != nullptr)
	{
		const uint32_t firstVertex = stencil->reverseWinding ? 6 : 0;
		const auto drawStencil = [&](PipelineIndex pipeline, uint32_t compareMask, uint32_t writeMask)
		{
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipelines[pipeline]);
			vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, compareMask);
			vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, writeMask);
			vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
			vkCmdDraw(command, 3, 1, firstVertex, 0);
		};
		// The render-pass load clear already produced the clear value. The
		// ordinary clear draw must not replace it with its stencil reference.
		uint32_t clearMask = 0, clearBack = 0, liveMask = 0, liveBack = 0;
		if (!Lumen::Libs::Graphics::GraphicsRenderStencilClearMasksForTest(true, false, 0xff, 0xff, &clearMask, &clearBack) ||
		    !Lumen::Libs::Graphics::GraphicsRenderStencilClearMasksForTest(false, false, stencil->maskBit, stencil->maskBit,
		                                                               &liveMask, &liveBack) ||
		    clearMask != clearBack || liveMask != liveBack)
			return Fail("Runtime stencil clear masks disagree between faces");
		drawStencil(StencilWriter, 0, stencil->legacyClearWrite ? 0xff : clearMask);
		if (stencil->splitPass)
		{
			vkCmdEndRenderPass(command);
			// Match the production clear-only pass followed by a LOAD pass.
			VkMemoryBarrier preserve {};
			preserve.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			preserve.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			preserve.dstAccessMask = preserve.srcAccessMask | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
			const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
			    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			vkCmdPipelineBarrier(command, stages, stages, 0, 1, &preserve, 0, nullptr, 0, nullptr);
			render.renderPass = state->loadRenderPass;
			vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
		}
		const VkRect2D maskRect {{kWidth / 4, 0}, {kWidth / 2, kHeight}};
		vkCmdSetScissor(command, 0, 1, &maskRect);
		drawStencil(StencilWriter, 0, liveMask);
		if (coalescing) splitCoalescedPass();
		vkCmdSetScissor(command, 0, 1, &scissor);
		drawStencil(StencilMaskedColor, stencil->maskBit, 0);
	} else
	{
		for (uint32_t draw = 0; draw < 2; ++draw)
		{
			if (coalescing && draw == 1) splitCoalescedPass();
			const bool near = (draw == 0) == nearFirst;
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipelines[near ? foreground : Background]);
			vkCmdDraw(command, 3, 1, near ? 0 : 3, 0);
			if (lateUnsafe != nullptr && draw == 0)
			{
				const VkRect2D maskRect {{kWidth / 4, 0}, {kWidth / 2, kHeight}};
				vkCmdSetScissor(command, 0, 1, &maskRect);
				vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipelines[StencilWriter]);
				vkCmdSetStencilCompareMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
				vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0x04);
				vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
				vkCmdDraw(command, 3, 1, 0, 0);
				vkCmdSetScissor(command, 0, 1, &scissor);
			}
		}
		if (lateUnsafe != nullptr)
		{
			// The first draw in this logical pass already merged. A later unsafe
			// resource must reopen with LOAD, preserving all three attachments and
			// replaying the new scissor, vertex/index and descriptor state.
			const VkRect2D whiteRect {{0, 0}, {kWidth / 4, kHeight}};
			vkCmdSetScissor(command, 0, 1, &whiteRect);
			vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, lateUnsafe->layout, 0, 1, &lateUnsafe->set, 0, nullptr);
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, lateUnsafe->pipeline);
			vkCmdBindIndexBuffer(command, lateUnsafe->indices, 0, VK_INDEX_TYPE_UINT16);
			vkCmdDrawIndexed(command, 3, 1, 0, 0, 0);
		}
	}
	vkCmdEndRenderPass(command);
	for (size_t i = 0; i < barriers.size(); ++i)
	{
		auto& b         = barriers[i];
		b.srcAccessMask = i == 0 ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		b.oldLayout     = kAttachmentLayouts[i];
		b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	}
	vkCmdPipelineBarrier(command,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
	                         VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
	for (size_t i = 0; i < state->images.size(); ++i)
	{
		VkBufferImageCopy copy {};
		copy.bufferOffset                = kGuardBytes + i * kPlaneBytes;
		copy.imageSubresource.aspectMask = i == 0 ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent                 = {kWidth, kHeight, 1};
		vkCmdCopyImageToBuffer(command, state->images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state->readback.buffer, 1, &copy);
	}
	VkBufferImageCopy stencilCopy {};
	stencilCopy.bufferOffset = kGuardBytes + 2 * kPlaneBytes;
	stencilCopy.imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
	stencilCopy.imageExtent = {kWidth, kHeight, 1};
	vkCmdCopyImageToBuffer(command, state->images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       state->readback.buffer, 1, &stencilCopy);
	VkBufferMemoryBarrier toHost {};
	toHost.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	toHost.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	toHost.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toHost.buffer                                           = state->readback.buffer;
	toHost.offset                                           = kGuardBytes;
	toHost.size                                             = kReadbackPayloadBytes;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0, nullptr);
	result = vkEndCommandBuffer(command);
	if (result != VK_SUCCESS) return Fail("End command buffer failed", result);
	VkSubmitInfo submit {};
	submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &command;
	if (completionEvent != VK_NULL_HANDLE && vkGetEventStatus(state->device, completionEvent) != VK_EVENT_RESET)
		return Fail("Recording signaled completion event before submit");
	result                    = vkQueueSubmit(state->queue, 1, &submit, state->fence);
	std::vector<uint8_t> readbackAtEvent;
	bool eventReadyBeforeFence = true;
	VkResult eventReadbackResult = VK_SUCCESS;
	if (result == VK_SUCCESS && completionEvent != VK_NULL_HANDLE)
	{
		// Capture the later draw/readback when SET is observed, before a fence
		// wait could hide premature publication. Evaluate this frozen copy only
		// after the fence has made cleanup safe, including on oracle failure.
		eventReadyBeforeFence = WaitForCompletionEvent(state->device, completionEvent);
		if (eventReadyBeforeFence)
		{
			eventReadbackResult = InvalidateBuffer(*state, state->readback);
			if (eventReadbackResult == VK_SUCCESS)
			{
				const auto* data = static_cast<const uint8_t*>(state->readback.mapped);
				readbackAtEvent.assign(data, data + kReadbackBufferBytes);
			}
		}
	}
	if (result == VK_SUCCESS) result = vkWaitForFences(state->device, 1, &state->fence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS) return Fail("Submit/wait failed", result);
	result = vkResetFences(state->device, 1, &state->fence);
	if (result != VK_SUCCESS) return Fail("Reset fence failed", result);
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
	state->firstDraw = false;
	if (!eventReadyBeforeFence) return Fail("Completion event did not signal independently before fence wait");
	if (eventReadbackResult != VK_SUCCESS) return Fail("Invalidate readback at event SET failed", eventReadbackResult);
	if (completionEvent == VK_NULL_HANDLE)
	{
		result = InvalidateBuffer(*state, state->readback);
		if (result != VK_SUCCESS) return Fail("Invalidate readback failed", result);
	}
	const auto* bytes = completionEvent != VK_NULL_HANDLE ? readbackAtEvent.data() : static_cast<const uint8_t*>(state->readback.mapped);
	for (size_t i = 0; i < kGuardBytes; ++i)
		if (bytes[i] != kGuardByte || bytes[kGuardBytes + kReadbackPayloadBytes + i] != kGuardByte) return Fail("Readback guard changed");
	uint32_t mismatches = 0;
	for (uint32_t y = 0; y < kHeight; ++y)
	{
		for (uint32_t x = 0; x < kWidth; ++x)
		{
			const uint32_t               pixel         = y * kWidth + x;
			if (uniformZero != nullptr)
			{
				const bool inside = x >= kWidth / 4 && x < 3 * kWidth / 4 && y >= kHeight / 4 && y < 3 * kHeight / 4;
				uint32_t depthBits = UINT32_MAX;
				std::memcpy(&depthBits, bytes + kGuardBytes + kPlaneBytes + pixel * sizeof(float), sizeof(depthBits));
				const uint8_t actualStencil = bytes[kGuardBytes + 2 * kPlaneBytes + pixel];
				const uint8_t expectedStencil = inside ? uniformZero->reference : 0;
				constexpr std::array<uint8_t, 4> unchangedColor {0, 0, 0, 255};
				if (depthBits != 0 || actualStencil != expectedStencil ||
				    std::memcmp(bytes + kGuardBytes + pixel * 4, unchangedColor.data(), 4) != 0)
				{
					if (mismatches++ < 4)
						std::fprintf(stderr, "uniform-zero draw mismatch: x=%u y=%u depth-raw=%08x stencil=%u expected=%u load=%u reverse=%u\n",
						             x, y, depthBits, actualStencil, expectedStencil, uniformZero->load, uniformZero->reverseWinding);
				}
				continue;
			}
			const bool maskInside = x >= kWidth / 4 && x < 3 * kWidth / 4;
			const uint8_t expectedStencil = lateUnsafe != nullptr ? static_cast<uint8_t>(0x40 | (maskInside ? 0x04 : 0)) : stencil == nullptr ? 0 :
			    static_cast<uint8_t>((stencil->legacyClearWrite ? 0xff : stencil->clearValue) |
			                         (maskInside ? stencil->maskBit : 0));
			const bool                   covered       = stencil != nullptr ? (expectedStencil & stencil->maskBit) != 0 :
			    foreground == OpaqueNear || x >= kWidth / 2;
			const bool                   corrupted     = foreground == ForcedEarlyControl && !covered;
			const bool white = lateUnsafe != nullptr && x < kWidth / 4;
			const std::array<uint8_t, 4> expectedColor = white ? std::array<uint8_t, 4> {255, 255, 255, 255} : covered ? std::array<uint8_t, 4> {255, 0, 0, 255}
			                                             : corrupted && nearFirst ? std::array<uint8_t, 4> {0, 0, 0, 255}
			                                                                      : std::array<uint8_t, 4> {0, 0, 255, 255};
			const float                  expectedDepth = stencil != nullptr ? 1.0f : (white || covered || corrupted ? kNear : kFar);
			float                        depth         = 0;
			std::memcpy(&depth, bytes + kGuardBytes + kPlaneBytes + pixel * sizeof(float), sizeof(float));
			const auto* color = bytes + kGuardBytes + pixel * 4;
			if (std::memcmp(color, expectedColor.data(), 4) != 0 || !std::isfinite(depth) || std::fabs(depth - expectedDepth) > 1e-6f ||
			    ((stencil != nullptr || lateUnsafe != nullptr) && bytes[kGuardBytes + 2 * kPlaneBytes + pixel] != expectedStencil))
			{
				if (mismatches++ < 4)
				{
					std::fprintf(stderr,
					    "color/depth/stencil mismatch: variant=%zu near-first=%u x=%u y=%u rgba=%u,%u,%u,%u "
					    "depth=%.9g expected-depth=%.9g stencil=%u expected-stencil=%u legacy=%u split=%u reverse=%u\n",
					    static_cast<size_t>(foreground), nearFirst, x, y, color[0], color[1], color[2], color[3], depth, expectedDepth,
					    bytes[kGuardBytes + 2 * kPlaneBytes + pixel], expectedStencil,
					    stencil != nullptr && stencil->legacyClearWrite, stencil != nullptr && stencil->splitPass,
					    stencil != nullptr && stencil->reverseWinding);
				}
			}
		}
	}
	return mismatches == 0 ? 0 : 2;
}
} // namespace

int GraphicsPixelDiscardDepthVulkanProbe()
{
	ProbeState state;
	if (Initialize(&state) != 0) return 1;
	for (PipelineIndex variant: {CutoutEarly, CutoutLate, OpaqueNear, ForcedEarlyControl})
		for (bool nearFirst: {true, false})
			if (DrawAndVerify(&state, variant, nearFirst) != 0) return 1;
	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(state.physicalDevice, &properties);
	std::printf("[GFX_PIXEL_DISCARD_DEPTH_VULKAN_PROBE] backend=native-metal device=%s fragment=runtime-gen4 "
	            "color=RGBA8 depth=D32S8 pixels=512 orders=2 early-z=on+off opaque=verified "
	            "discard-color-depth=verified forced-early-corruption=reproduced readback-guards=clean status=clean\n",
	            properties.deviceName);
	return std::fflush(stdout) == 0 ? 0 : 1;
}

int GraphicsStencilClearVulkanProbe()
{
	ProbeState state;
	if (Initialize(&state) != 0) return 1;
	uint32_t scenes = 0;
	for (bool legacy: {false, true})
		for (bool split: {false, true})
			for (bool reverse: {false, true})
				for (uint8_t bit: {uint8_t(1), uint8_t(2)})
					for (uint8_t clearValue: {uint8_t(0), uint8_t(0xa0)})
					{
						const StencilScenario scenario {legacy, split, reverse, clearValue, bit};
						if (DrawAndVerify(&state, StencilMaskedColor, false, &scenario) != 0) return 1;
						++scenes;
					}
	// A clear value that already contains the tested bit must remain visible.
	const StencilScenario prefilled {false, true, false, 1, 1};
	if (DrawAndVerify(&state, StencilMaskedColor, false, &prefilled) != 0) return 1;
	++scenes;
	for (bool reverse: {false, true})
	{
		// A partial draw must preserve the logical +0/0 image, including pixels
		// outside its coverage. A nonzero reference proves that this exact draw
		// reaches stencil; the final LOAD draw must then restore all zeros.
		for (const UniformZeroScenario scenario: {UniformZeroScenario {0, false, reverse},
		                                         UniformZeroScenario {0x5a, true, reverse},
		                                         UniformZeroScenario {0, true, reverse}})
			if (DrawAndVerify(&state, StencilZeroPreserving, false, nullptr, false, nullptr, VK_NULL_HANDLE, &scenario) != 0)
				return 1;
	}
	std::printf("[GFX_UNIFORM_ZERO_DRAW_PROBE] depth-raw=+0 stencil=0 depth-test=off depth-write=off "
	            "stencil-op=always-replace scissor=partial-xy faces=front+back positive-control=0x5a "
	            "load-restore=verified readback-guards=clean status=clean\n");
	std::printf("[GFX_STENCIL_CLEAR_VULKAN_PROBE] backend=native-metal scenes=%u pixels=512 "
	            "clear-mask=runtime-helper faces=front+back pass-split=verified "
	            "stencil-and-color=verified legacy-overflow=reproduced readback-guards=clean status=clean\n", scenes);
	return std::fflush(stdout) == 0 ? 0 : 1;
}

namespace {
constexpr uint32_t kColorReadbackPitch = 128;
constexpr size_t kColorReadbackBytes = kColorReadbackPitch * kHeight * 4;
constexpr size_t kColorReadbackTail = 37;
constexpr uint8_t kGuestUnchangedByte = 0x6c;

std::array<uint8_t, 4> ReadbackPattern(uint32_t x, uint32_t y, uint32_t frame)
{
	const uint32_t pattern = (x / 16 + y + frame) % 3;
	if (pattern == 1) return {{255, 0, 0, 255}};
	if (pattern == 2) return {{0, 0, 255, 255}};
	return {{static_cast<uint8_t>(19 + frame * 47), static_cast<uint8_t>(213 - frame * 23),
	         static_cast<uint8_t>(37 + frame * 29), static_cast<uint8_t>(231 - frame * 13)}};
}

bool CheckReadbackRowValidation()
{
	using namespace Lumen::Libs::Graphics;
	for (const auto format: {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
	{
		const uint32_t element = format == VK_FORMAT_R8_UNORM ? 1 : format == VK_FORMAT_R8G8_UNORM ? 2 : 4;
		std::array<uint8_t, 80> source, destination;
		for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>(i + 7);
		destination.fill(kGuestUnchangedByte);
		uint64_t row = 0, span = 0;
		if (!UtilTryGetImageReadbackSpan(format, 3, 2, 5, &row, &span) || row != 3 * element || span != 8 * element ||
		    !UtilCopyReadbackRows(destination.data(), destination.size(), source.data(), source.size(), format, 3, 2, 5)) return false;
		for (size_t i = 0; i < destination.size(); ++i)
		{
			const bool active = i < 3 * element || (i >= 5 * element && i < 8 * element);
			if (destination[i] != (active ? source[i] : kGuestUnchangedByte)) return false;
		}
		const auto before = destination;
		if (UtilCopyReadbackRows(destination.data(), span - 1, source.data(), source.size(), format, 3, 2, 5) ||
		    UtilCopyReadbackRows(destination.data(), destination.size(), source.data(), span - 1, format, 3, 2, 5) ||
		    UtilCopyReadbackRows(destination.data(), destination.size(), source.data(), source.size(), format, 3, 2, 2) ||
		    UtilCopyReadbackRows(destination.data() + 1, destination.size() - 1, destination.data(), destination.size(),
		                         format, 3, 2, 5) || destination != before) return false;
	}
	uint64_t row = 71, span = 83;
	std::array<uint8_t, 32> source {}, destination {};
	destination.fill(kGuestUnchangedByte);
	const auto before = destination;
	if (UtilTryGetImageReadbackSpan(VK_FORMAT_D32_SFLOAT, 1, 1, 1, &row, &span) || row != 71 || span != 83 ||
	    UtilTryGetImageReadbackSpan(VK_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, UINT32_MAX, UINT32_MAX, &row, &span) ||
	    row != 71 || span != 83 ||
	    UtilCopyReadbackRows(destination.data(), destination.size(), source.data(), source.size(), VK_FORMAT_D32_SFLOAT, 1, 1, 1) ||
	    UtilCopyReadbackRows(destination.data(), destination.size(), source.data(), source.size(), VK_FORMAT_R8_UNORM, 0, 1, 1) ||
	    destination != before) return false;
	return true;
}

int DrawColorReadbackAndVerify(ProbeState* state, VkFormat format, uint32_t frame)
{
	using namespace Lumen::Libs::Graphics;
	const bool srgb = format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB;
	const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
	std::memset(state->readback.mapped, kGuardByte, kReadbackBufferBytes);
	VkResult result = FlushBuffer(*state, state->readback);
	if (result != VK_SUCCESS) return Fail("Color readback flush failed", result);
	VkCommandBufferAllocateInfo allocation {};
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = state->commandPool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	VkCommandBuffer command = VK_NULL_HANDLE;
	result = vkAllocateCommandBuffers(state->device, &allocation, &command);
	if (result != VK_SUCCESS) return Fail("Color readback command allocation failed", result);
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(command, &begin);
	if (result != VK_SUCCESS) return Fail("Color readback begin failed", result);
	std::array<VkImageMemoryBarrier, 2> barriers {};
	for (size_t i = 0; i < barriers.size(); ++i)
	{
		auto& barrier = barriers[i];
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = state->firstDraw ? 0 : i == 0 ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = i == 0 ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = state->firstDraw ? VK_IMAGE_LAYOUT_UNDEFINED : i == 0 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : kAttachmentLayouts[i];
		barrier.newLayout = kAttachmentLayouts[i];
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = state->images[i];
		barrier.subresourceRange = {kAspects[i], 0, 1, 0, 1};
	}
	vkCmdPipelineBarrier(command, state->firstDraw ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT :
	                     VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
	                     0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
	std::array<VkClearValue, 2> initial {};
	const auto clearColor = ReadbackPattern(0, frame == 0 ? 0 : 2, frame);
	for (size_t channel = 0; channel < clearColor.size(); ++channel) initial[0].color.float32[channel] = clearColor[channel] / 255.0f;
	initial[1].depthStencil.depth = 1.0f;
	VkRenderPassBeginInfo pass {};
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = state->renderPass;
	pass.framebuffer = state->framebuffer;
	pass.renderArea.extent = {kWidth, kHeight};
	pass.clearValueCount = initial.size();
	pass.pClearValues = initial.data();
	vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	constexpr VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers(command, 0, 1, &state->vertices.buffer, &vertexOffset);
	for (uint32_t y = 0; y < kHeight; ++y)
		for (uint32_t x = 0; x < kWidth; x += 16)
		{
			const uint32_t pattern = (x / 16 + y + frame) % 3;
			if (pattern == 0) continue;
			const VkRect2D rectangle {{static_cast<int32_t>(x), static_cast<int32_t>(y)}, {16, 1}};
			vkCmdSetScissor(command, 0, 1, &rectangle);
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, state->pipelines[pattern == 1 ? OpaqueNear : Background]);
			vkCmdDraw(command, 3, 1, pattern == 1 ? 0 : 3, 0);
		}
	vkCmdEndRenderPass(command);
	barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, barriers.data());
	VkBufferImageCopy copy {};
	copy.bufferOffset = kGuardBytes;
	copy.bufferRowLength = kColorReadbackPitch;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {kWidth, kHeight, 1};
	vkCmdCopyImageToBuffer(command, state->images[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state->readback.buffer, 1, &copy);
	VkBufferMemoryBarrier host {};
	host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = state->readback.buffer;
	host.offset = kGuardBytes;
	host.size = kColorReadbackBytes;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
	result = vkEndCommandBuffer(command);
	if (result != VK_SUCCESS) return Fail("Color readback end failed", result);
	VkSubmitInfo submit {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	result = vkQueueSubmit(state->queue, 1, &submit, state->fence);
	if (result == VK_SUCCESS) result = vkWaitForFences(state->device, 1, &state->fence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS) return Fail("Color readback submit/wait failed", result);
	result = vkResetFences(state->device, 1, &state->fence);
	if (result != VK_SUCCESS) return Fail("Color readback fence reset failed", result);
	vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
	state->firstDraw = false;
	result = InvalidateBuffer(*state, state->readback);
	if (result != VK_SUCCESS) return Fail("Color readback invalidate failed", result);

	std::array<uint8_t, kGuardBytes + kColorReadbackBytes + kColorReadbackTail + kGuardBytes> guest;
	guest.fill(kGuestUnchangedByte);
	const auto* source = static_cast<const uint8_t*>(state->readback.mapped);
	if (!UtilCopyReadbackRows(guest.data() + kGuardBytes, kColorReadbackBytes + kColorReadbackTail,
	                         source + kGuardBytes, kColorReadbackBytes, format, kWidth, kHeight, kColorReadbackPitch))
		return Fail("Runtime row-copy rejected GPU readback");
	for (size_t i = 0; i < kReadbackBufferBytes; ++i)
	{
		const size_t offset = i >= kGuardBytes ? i - kGuardBytes : kColorReadbackBytes;
		const size_t y = offset / (kColorReadbackPitch * 4), x = (offset / 4) % kColorReadbackPitch;
		const bool active = y < kHeight && x < kWidth;
		auto rgba = ReadbackPattern(static_cast<uint32_t>(x), static_cast<uint32_t>(y), frame);
		if (srgb)
			for (unsigned c = 0; c != 3; ++c)
			{
				const double linear = rgba[c] / 255.0;
				rgba[c] = static_cast<uint8_t>(std::lround(255.0 * (linear <= 0.0031308 ? 12.92 * linear :
				                                                    1.055 * std::pow(linear, 1.0 / 2.4) - 0.055)));
			}
		if (bgra) std::swap(rgba[0], rgba[2]);
		const int expected = active ? rgba[offset % 4] : kGuardByte;
		// Permit one code value for the render conversion only. Publication must
		// preserve every byte actually produced by the GPU, without conversion.
		const int tolerance = active && srgb && offset % 4 != 3 ? 1 : 0;
		if (std::abs(static_cast<int>(source[i]) - expected) > tolerance) return Fail("GPU pattern/readback guard mismatch");
		if (i < guest.size() && guest[i] != (active ? source[i] : kGuestUnchangedByte))
			return Fail("Runtime row-copy active texel/padding/guard mismatch");
	}
	std::vector<uint8_t> tiled(kGuardBytes + 32768 + kColorReadbackTail + kGuardBytes, kGuestUnchangedByte);
	if (!TileTryConvertLinearToDisplay2dThin(tiled.data() + kGuardBytes, 32768 + kColorReadbackTail,
	                                       guest.data() + kGuardBytes, kColorReadbackBytes, kWidth, kHeight, kColorReadbackPitch, false))
		return Fail("Runtime display tiling rejected GPU row-copy");
	// Literal local display-order vectors also used by the dedicated Tile test;
	// unlike an inverse-only check, these detect a shared wrong permutation.
	constexpr uint32_t points[][3] = {{0, 0, 0}, {1, 0, 4}, {0, 1, 16}, {0, 2, 64}, {0, 4, 128},
	                                  {7, 7, 252}, {8, 0, 256}, {16, 0, 768}, {32, 0, 1024}};
	for (const auto& point: points)
	{
		const auto* pixel = source + kGuardBytes + (point[1] * kColorReadbackPitch + point[0]) * 4;
		if (std::memcmp(tiled.data() + kGuardBytes + point[2], pixel, 4) != 0)
			return Fail("Display tiled GPU texel fixture mismatch");
	}
	for (size_t i = 0; i < tiled.size(); ++i)
		if ((i < kGuardBytes || i >= kGuardBytes + 32768) && tiled[i] != kGuestUnchangedByte)
			return Fail("Display tiled destination guard/tail changed");
	return 0;
}

VkResult SubmitColorProbeCommand(ProbeState* state, VkCommandBuffer command)
{
	VkResult result = vkEndCommandBuffer(command);
	if (result == VK_SUCCESS)
	{
		VkSubmitInfo submit {};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		result = vkQueueSubmit(state->queue, 1, &submit, state->fence);
	}
	if (result == VK_SUCCESS) result = vkWaitForFences(state->device, 1, &state->fence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result == VK_SUCCESS) result = vkResetFences(state->device, 1, &state->fence);
	if (result == VK_SUCCESS) vkFreeCommandBuffers(state->device, state->commandPool, 1, &command);
	return result;
}

VkResult BeginColorProbeCommand(ProbeState* state, VkCommandBuffer* command)
{
	VkCommandBufferAllocateInfo allocation {};
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = state->commandPool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	VkResult result = vkAllocateCommandBuffers(state->device, &allocation, command);
	if (result != VK_SUCCESS) return result;
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(*command, &begin);
	if (result != VK_SUCCESS) vkFreeCommandBuffers(state->device, state->commandPool, 1, command);
	return result;
}

struct ColorPublicationRead
{
	ProbeState* state = nullptr;
	uint64_t allocation = 0, allocation_size = 0;
	uint32_t mutation = 0, reads = 0;
	VkImageLayout original_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkImageLayout restored_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	bool valid = true;
};

bool ReadColorPublicationImage(void* context, void* destination, uint64_t size)
{
	using namespace Lumen::Libs::Graphics;
	using namespace Lumen::Core::VirtualMemory;
	auto& read = *static_cast<ColorPublicationRead*>(context);
	++read.reads;
	read.valid = read.valid && read.reads == 1 && !CurrentThreadHasMemoryLease() &&
	    (read.original_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
	     read.original_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (!read.valid || destination == nullptr || size < kColorReadbackBytes) return false;
	auto* state = read.state;
	std::memset(state->readback.mapped, kGuardByte, kReadbackBufferBytes);
	VkResult result = FlushBuffer(*state, state->readback);
	VkCommandBuffer command = VK_NULL_HANDLE;
	if (result == VK_SUCCESS) result = BeginColorProbeCommand(state, &command);
	if (result != VK_SUCCESS) return false;
	VkImageMemoryBarrier image {};
	image.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	const bool shader_read = read.original_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	const auto original_access = shader_read ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	const auto original_stages = shader_read ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	image.srcAccessMask = original_access;
	image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	image.oldLayout = read.original_layout;
	image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image.image = state->images[0];
	image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(command, original_stages, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &image);
	VkBufferImageCopy copy {};
	copy.bufferOffset = kGuardBytes;
	copy.bufferRowLength = kColorReadbackPitch;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {kWidth, kHeight, 1};
	vkCmdCopyImageToBuffer(command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state->readback.buffer, 1, &copy);
	VkBufferMemoryBarrier host {};
	host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = state->readback.buffer;
	host.offset = kGuardBytes;
	host.size = kColorReadbackBytes;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 0, nullptr, 1, &host, 0, nullptr);
	image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	image.dstAccessMask = original_access;
	image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image.newLayout = read.original_layout;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, original_stages,
	                     0, 0, nullptr, 0, nullptr, 1, &image);
	result = SubmitColorProbeCommand(state, command);
	if (result != VK_SUCCESS || CurrentThreadHasMemoryLease()) return false;
	read.restored_layout = read.original_layout;
	result = InvalidateBuffer(*state, state->readback);
	if (result != VK_SUCCESS) return false;
	const auto* bytes = static_cast<const uint8_t*>(state->readback.mapped);
	for (uint64_t index = 0; index != kReadbackBufferBytes; ++index)
	{
		const bool active = index >= kGuardBytes && index < kGuardBytes + kColorReadbackBytes &&
		                    (index - kGuardBytes) % (kColorReadbackPitch * 4u) < kWidth * 4u;
		if (!active && bytes[index] != kGuardByte) return false;
	}
	if (!UtilCopyReadbackRows(destination, size, bytes + kGuardBytes, kColorReadbackBytes,
	                         VK_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, kColorReadbackPitch)) return false;
	if (read.mutation == 1)
	{
		const uint64_t unrelated = Alloc(0, 65536, Mode::ReadWrite);
		read.valid = unrelated != 0 && Free(unrelated) && read.valid;
	} else if (read.mutation == 2)
	{
		read.valid = Protect(read.allocation, read.allocation_size, Mode::Read) &&
		             Protect(read.allocation, read.allocation_size, Mode::ReadWrite) && read.valid;
	} else if (read.mutation == 3)
	{
		read.valid = Free(read.allocation) && AllocFixed(read.allocation, read.allocation_size, Mode::ReadWrite) && read.valid;
		if (read.valid) std::memset(reinterpret_cast<void*>(read.allocation), kGuestUnchangedByte, read.allocation_size);
	}
	return read.valid;
}

int VerifyTypedColorPublication(ProbeState* state)
{
	using namespace Lumen::Libs::Graphics;
	using namespace Lumen::Core::VirtualMemory;
	TileSizeAlign tile {};
	if (!TileTryGetColor2dThinTextureSize(10, 0, kWidth, kHeight, kColorReadbackPitch, 1, 14, false, false,
	                                     &tile, nullptr, nullptr) || tile.size != 32768) return Fail("Color tile14 probe layout rejected");
	// The independent row probe already checked these real fragment-produced
	// bytes. Preserve its reference before the typed publisher reuses staging.
	std::vector<uint8_t> linear(tile.size, kGuestUnchangedByte), tiled(tile.size, kGuestUnchangedByte);
	if (!UtilCopyReadbackRows(linear.data(), linear.size(), static_cast<const uint8_t*>(state->readback.mapped) + kGuardBytes,
	                         kColorReadbackBytes, VK_FORMAT_R8G8B8A8_UNORM, kWidth, kHeight, kColorReadbackPitch) ||
	    !TileTryConvertLinearToColor2dThin(tiled.data(), tiled.size(), linear.data(), linear.size(),
	                                     kWidth, kHeight, kColorReadbackPitch, false)) return Fail("Color tile14 GPU reference rejected");
	// Restore the actual native layout after the independent row readback.
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkResult result = BeginColorProbeCommand(state, &command);
	if (result != VK_SUCCESS) return Fail("Color tile14 transition begin failed", result);
	VkImageMemoryBarrier barrier {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = state->images[0];
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &barrier);
	result = SubmitColorProbeCommand(state, command);
	if (result != VK_SUCCESS) return Fail("Color tile14 transition wait failed", result);
	GraphicContext ctx {};
	ctx.instance = state->instance; ctx.physical_device = state->physicalDevice; ctx.device = state->device;
	RenderTextureVulkanImage image;
	image.image = state->images[0]; image.image_view[0] = state->views[0]; image.memory.memory = state->imageMemory[0];
	image.format = VK_FORMAT_R8G8B8A8_UNORM; image.extent = {kWidth, kHeight}; image.mip_levels = 1;
	image.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	constexpr uint64_t allocation_size = 131072, offset = 32768;
	Init();
	for (const auto original_layout: {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL})
	{
		const bool snapshot_pair = original_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		if (snapshot_pair)
		{
			result = BeginColorProbeCommand(state, &command);
			if (result != VK_SUCCESS) return Fail("Color sampled-layout transition begin failed", result);
			barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			                     0, 0, nullptr, 0, nullptr, 1, &barrier);
			result = SubmitColorProbeCommand(state, command);
			if (result != VK_SUCCESS) return Fail("Color sampled-layout transition wait failed", result);
		}
		image.layout = original_layout;
		for (uint32_t mutation = 0; mutation != 4; ++mutation)
		{
			const uint64_t allocation = Alloc(0, allocation_size, Mode::ReadWrite);
			if (allocation == 0) return Fail("Color tile14 managed guest allocation failed");
			struct Owner { uint64_t address; ~Owner() { Free(address); } } owner {allocation};
			std::memset(reinterpret_cast<void*>(allocation), kGuestUnchangedByte, allocation_size);
			const uint64_t address = allocation + offset, original = GetMutationGeneration();
			uint64_t generation = UINT64_MAX;
			GraphicsGuestLeaseResult acquisition;
			if (snapshot_pair)
			{
				if (UtilCanReadbackRenderTextureToGuest(&ctx, &image, address, tile.size, kColorReadbackPitch, false,
				    &generation, original, &acquisition) || generation != UINT64_MAX || acquisition.attempts != 0)
					return Fail("Color sampled layout accepted without exact-pair authority");
			}
			if (!UtilCanReadbackRenderTextureToGuest(&ctx, &image, address, tile.size, kColorReadbackPitch, false,
			                                       &generation, original, &acquisition, snapshot_pair) || generation != original ||
			    CurrentThreadHasMemoryLease()) return Fail("Color tile14 typed preflight failed");
			uint64_t missing_generation = 7;
			if (UtilCanReadbackRenderTextureToGuest(&ctx, &image, address, tile.size, kColorReadbackPitch, false,
			                                      &missing_generation, UINT64_MAX, &acquisition, snapshot_pair) ||
			    missing_generation != UINT64_MAX || acquisition.attempts != 0 || CurrentThreadHasMemoryLease())
				return Fail("Color tile14 missing-token preflight accepted");
			ColorPublicationRead read {state, allocation, allocation_size, mutation};
			read.original_layout = original_layout;
			uint64_t hash = UINT64_MAX;
			const bool published = UtilRenderTexturePublicationContractTest(&ctx, &image, address, tile.size,
			    kColorReadbackPitch, false, original, &hash, ReadColorPublicationImage, &read, snapshot_pair);
			const bool expected = mutation < 2;
			if (published != expected || !read.valid || read.reads != 1 || read.restored_layout != original_layout ||
			    image.layout != original_layout || CurrentThreadHasMemoryLease() ||
			    (expected ? hash != XXH3_64bits(reinterpret_cast<const void*>(address), tile.size) : hash != 0))
				return Fail("Color tile14 token/publication/read-once contract failed");
			for (uint64_t index = 0; index != allocation_size; ++index)
			{
				const uint8_t expected_byte = expected && index >= offset && index < offset + tile.size
				    ? tiled[index - offset] : kGuestUnchangedByte;
				if (*reinterpret_cast<const uint8_t*>(allocation + index) != expected_byte)
					return Fail("Color tile14 managed publication/padding/guards mismatch");
			}
			if (expected)
			{
				std::vector<uint8_t> decoded(tile.size, kGuestUnchangedByte);
				if (!TileTryConvertColor2dThinToLinear(decoded.data(), decoded.size(), reinterpret_cast<const void*>(address),
				                                     tile.size, kWidth, kHeight, kColorReadbackPitch, false) || decoded != linear)
					return Fail("Color tile14 GPU texel decode mismatch");
			}
		}
	}
	// A second native transfer starts from the required restored ShaderRead
	// layout and verifies the same texels. This exercises real later GPU reuse,
	// rather than relying only on the host layout metadata above.
	ColorPublicationRead reuse {state};
	reuse.original_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	std::vector<uint8_t> reused(tile.size, kGuestUnchangedByte);
	if (!ReadColorPublicationImage(&reuse, reused.data(), reused.size()) || reuse.reads != 1 ||
	    reuse.restored_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || reused != linear)
		return Fail("Color sampled-layout native reuse/restoration failed");
	std::printf("[GFX_EOP_COLOR_PUBLICATION_VULKAN_PROBE] backend=native-metal image=RenderTexture/RGBA8 tile=14"
	            " producer=runtime-fragment-draws transfer/fence=real read-per-publication=1 mapping/store/hash=real"
	            " token=original unrelated-VM=accepted protect-restore/remap=no-store padding/guards=preserved"
	            " CP-enrollment=modeled hardware-PS4=unqualified cases=4 status=clean\n");
	std::printf("[GFX_EOP_COLOR_LAYOUT_VULKAN_PROBE] backend=native-metal original-layout=attachment/shader-read"
	            " sampled-layout=exact-pair-authority-required transfer/fence=real cases-per-layout=4 layouts=2"
	            " final-native-reuse=shader-read-transfer-verified restore=original texels/padding/guards=preserved"
	            " CP/pair-enrollment=modeled hardware-PS4=unqualified status=clean\n");
	return 0;
}
} // namespace

int GraphicsColorReadbackRowsVulkanProbe()
{
	if (!CheckReadbackRowValidation()) return Fail("Runtime row-copy validation/padding contract failed");
	for (const auto format: {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
	                        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB})
	{
		ProbeState state;
		if (Initialize(&state, format) != 0) return 1;
		for (uint32_t frame = 0; frame < 2; ++frame)
			if (DrawColorReadbackAndVerify(&state, format, frame) != 0) return 1;
		if (format == VK_FORMAT_R8G8B8A8_UNORM && VerifyTypedColorPublication(&state) != 0) return 1;
	}
	std::printf("[GFX_COLOR_READBACK_ROWS_VULKAN_PROBE] formats=RGBA,BGRA frames-per-format=2 gpu=runtime-fragment-draws "
	            "srgb=RGBA,BGRA publication=byte-exact row-copy=runtime-helper display-tiling=runtime-helper guards=clean validation=reject-before-write "
	            "scope=transfer-and-publication-helpers status=clean\n");
	return std::fflush(stdout) == 0 ? 0 : 1;
}

namespace {
struct CompletionPassStats
{
	uint64_t logical = 0, physical = 0, merged = 0, mergedAfterMarker = 0;
	uint32_t enabled = 0;
	CompletionPassStats() { LumenMetalGetCompletionPassReuseStatsV1(&logical, &physical, &merged, &mergedAfterMarker, &enabled); }
	bool ExpectDelta(const CompletionPassStats& before, uint64_t begins, uint64_t merges, uint64_t markerMerges = 0) const
	{
		if (logical - before.logical == begins && merged - before.merged == merges &&
		    physical - before.physical == begins - merges && mergedAfterMarker - before.mergedAfterMarker == markerMerges) return true;
		std::fprintf(stderr, "completion-reuse counters: logical=%llu physical=%llu merged=%llu after-marker=%llu expected=%llu/%llu/%llu/%llu\n",
		    static_cast<unsigned long long>(logical - before.logical), static_cast<unsigned long long>(physical - before.physical),
		    static_cast<unsigned long long>(merged - before.merged), static_cast<unsigned long long>(mergedAfterMarker - before.mergedAfterMarker),
		    static_cast<unsigned long long>(begins), static_cast<unsigned long long>(begins - merges),
		    static_cast<unsigned long long>(merges), static_cast<unsigned long long>(markerMerges));
		return false;
	}
};

enum class PassBoundary { Same, BindState, Pass, Framebuffer, Area, Clear, DontCareLoad, DontCareStore,
                          Barrier, Copy, Dispatch, Event, EventTop, EventMixed, EventZero, CommandBuffer,
                          WritableStorage, ReadOnlyStorage, InvalidDraw, InvalidEnd, EmptyCarry, EmptyUnsafe, EmptyBarrier };

struct CompletionPassResources
{
	ProbeState* state;
	std::array<VkRenderPass, 3> passes {};
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkEvent event = VK_NULL_HANDLE;
	VkShaderModule shader = VK_NULL_HANDLE;
	VkPipeline compute = VK_NULL_HANDLE;
	VkDescriptorSetLayout storageLayout = VK_NULL_HANDLE;
	VkDescriptorPool storagePool = VK_NULL_HANDLE;
	VkDescriptorSet storageSet = VK_NULL_HANDLE;
	VkPipelineLayout storagePipelineLayout = VK_NULL_HANDLE;
	std::array<VkShaderModule, 2> storageShaders {};
	std::array<VkPipeline, 2> storagePipelines {};
	HostBuffer source, destination, indices;
	bool attachmentLayoutsReady = false;
	~CompletionPassResources()
	{
		const auto device = state->device;
		for (auto pass: passes) if (pass != VK_NULL_HANDLE) vkDestroyRenderPass(device, pass, nullptr);
		if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if (event != VK_NULL_HANDLE) vkDestroyEvent(device, event, nullptr);
		if (compute != VK_NULL_HANDLE) vkDestroyPipeline(device, compute, nullptr);
		if (shader != VK_NULL_HANDLE) vkDestroyShaderModule(device, shader, nullptr);
		for (auto p: storagePipelines) if (p != VK_NULL_HANDLE) vkDestroyPipeline(device, p, nullptr);
		for (auto s: storageShaders) if (s != VK_NULL_HANDLE) vkDestroyShaderModule(device, s, nullptr);
		if (storagePool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, storagePool, nullptr);
		if (storagePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, storagePipelineLayout, nullptr);
		if (storageLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, storageLayout, nullptr);
		for (auto* b: {&source, &destination, &indices})
		{
			if (b->mapped != nullptr) vkUnmapMemory(device, b->memory);
			if (b->buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, b->buffer, nullptr);
			if (b->memory != VK_NULL_HANDLE) vkFreeMemory(device, b->memory, nullptr);
		}
	}
	bool Initialize()
	{
		for (size_t index = 0; index < passes.size(); ++index)
		{
			std::array<VkAttachmentDescription, 2> attachments {};
			for (size_t i = 0; i < attachments.size(); ++i)
			{
				auto& a = attachments[i];
				a.format = kFormats[i]; a.samples = VK_SAMPLE_COUNT_1_BIT;
				a.loadOp = a.stencilLoadOp = index == 1 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
				a.storeOp = a.stencilStoreOp = index == 2 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
				a.initialLayout = a.finalLayout = kAttachmentLayouts[i];
			}
			VkAttachmentReference color {0, kAttachmentLayouts[0]}, depth {1, kAttachmentLayouts[1]};
			VkSubpassDescription subpass {};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color; subpass.pDepthStencilAttachment = &depth;
			VkRenderPassCreateInfo pass {};
			pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			pass.attachmentCount = attachments.size(); pass.pAttachments = attachments.data();
			pass.subpassCount = 1; pass.pSubpasses = &subpass;
			if (vkCreateRenderPass(state->device, &pass, nullptr, &passes[index]) != VK_SUCCESS) return false;
		}
		VkFramebufferCreateInfo fb {};
		fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fb.renderPass = state->loadRenderPass;
		fb.attachmentCount = state->views.size(); fb.pAttachments = state->views.data();
		fb.width = kWidth; fb.height = kHeight; fb.layers = 1;
		if (vkCreateFramebuffer(state->device, &fb, nullptr, &framebuffer) != VK_SUCCESS) return false;
		VkEventCreateInfo e {}; e.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
		if (vkCreateEvent(state->device, &e, nullptr, &event) != VK_SUCCESS) return false;
		if (CreateHostBuffer(state, 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &source) != VK_SUCCESS ||
		    CreateHostBuffer(state, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &destination) != VK_SUCCESS ||
		    CreateHostBuffer(state, 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &indices) != VK_SUCCESS) return false;
		std::fill_n(static_cast<float*>(source.mapped), 64, 1.0f); std::memset(destination.mapped, 0, 256);
		const std::array<uint16_t, 3> triangle {0, 1, 2}; std::memcpy(indices.mapped, triangle.data(), sizeof(triangle));
		if (FlushBuffer(*state, source) != VK_SUCCESS || FlushBuffer(*state, destination) != VK_SUCCESS ||
		    FlushBuffer(*state, indices) != VK_SUCCESS) return false;
		constexpr char computeAssembly[] = R"spirv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%function = OpTypeFunction %void
%main = OpFunction %void None %function
%entry = OpLabel
OpReturn
OpFunctionEnd
)spirv";
		std::vector<uint32_t> words;
		if (!AssembleShader(computeAssembly, &words)) return false;
		VkShaderModuleCreateInfo shaderInfo {}; shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		shaderInfo.codeSize = words.size() * sizeof(uint32_t); shaderInfo.pCode = words.data();
		if (vkCreateShaderModule(state->device, &shaderInfo, nullptr, &shader) != VK_SUCCESS) return false;
		VkComputePipelineCreateInfo pipeline {}; pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipeline.stage.module = shader; pipeline.stage.pName = "main";
		pipeline.layout = state->pipelineLayout;
		if (vkCreateComputePipelines(state->device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &compute) != VK_SUCCESS) return false;
		VkDescriptorSetLayoutBinding binding {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
		VkDescriptorSetLayoutCreateInfo setLayout {}; setLayout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setLayout.bindingCount = 1; setLayout.pBindings = &binding;
		if (vkCreateDescriptorSetLayout(state->device, &setLayout, nullptr, &storageLayout) != VK_SUCCESS) return false;
		VkPipelineLayoutCreateInfo pl {}; pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pl.setLayoutCount = 1; pl.pSetLayouts = &storageLayout;
		if (vkCreatePipelineLayout(state->device, &pl, nullptr, &storagePipelineLayout) != VK_SUCCESS) return false;
		VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
		VkDescriptorPoolCreateInfo pool {}; pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &poolSize;
		if (vkCreateDescriptorPool(state->device, &pool, nullptr, &storagePool) != VK_SUCCESS) return false;
		VkDescriptorSetAllocateInfo set {}; set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		set.descriptorPool = storagePool; set.descriptorSetCount = 1; set.pSetLayouts = &storageLayout;
		if (vkAllocateDescriptorSets(state->device, &set, &storageSet) != VK_SUCCESS) return false;
		VkDescriptorBufferInfo buffer {source.buffer, 0, 256};
		VkWriteDescriptorSet write {}; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = storageSet; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write.pBufferInfo = &buffer; vkUpdateDescriptorSets(state->device, 1, &write, 0, nullptr);
		constexpr char storageAssembly[] = R"spirv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color
OpExecutionMode %main OriginUpperLeft
OpDecorate %color Location 0
OpDecorate %block BufferBlock
OpMemberDecorate %block 0 Offset 0
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 0
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%v4float = OpTypeVector %float 4
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%one = OpConstant %float 1
%block = OpTypeStruct %float
%blockptr = OpTypePointer Uniform %block
%floatptr = OpTypePointer Uniform %float
%colorptr = OpTypePointer Output %v4float
%buffer = OpVariable %blockptr Uniform
%color = OpVariable %colorptr Output
%main = OpFunction %void None %function
%entry = OpLabel
%address = OpAccessChain %floatptr %buffer %zero
%value = OpLoad %float %address
%rgba = OpCompositeConstruct %v4float %value %value %value %one
OpStore %color %rgba
OpReturn
OpFunctionEnd
)spirv";
		for (size_t i = 0; i < storageShaders.size(); ++i)
		{
			std::string assembly(storageAssembly);
			if (i == 1) assembly.insert(assembly.find("%void ="), "OpDecorate %buffer NonWritable\n");
			if (!AssembleShader(assembly.c_str(), &words)) return false;
			shaderInfo.codeSize = words.size() * sizeof(uint32_t); shaderInfo.pCode = words.data();
			if (vkCreateShaderModule(state->device, &shaderInfo, nullptr, &storageShaders[i]) != VK_SUCCESS ||
			    CreatePipeline(state, Background, &storagePipelines[i], storageShaders[i], storagePipelineLayout) != VK_SUCCESS)
				return false;
		}
		return true;
	}
};

int CheckPassBoundary(ProbeState* state, CompletionPassResources* resources, PassBoundary boundary, bool leadingMarker = false)
{
	const CompletionPassStats before;
	VkCommandBuffer command = VK_NULL_HANDLE;
	bool secondPass = false;
	bool markerRecorded = false;
	const bool emptyPass = boundary == PassBoundary::EmptyCarry || boundary == PassBoundary::EmptyUnsafe ||
	                       boundary == PassBoundary::EmptyBarrier;
	if (vkResetEvent(state->device, resources->event) != VK_SUCCESS) return Fail("Boundary event reset failed");
	const auto beginCommand = [&] {
		VkCommandBufferAllocateInfo allocation {}; allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocation.commandPool = state->commandPool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(state->device, &allocation, &command) != VK_SUCCESS) return false;
		VkCommandBufferBeginInfo begin {}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		return vkBeginCommandBuffer(command, &begin) == VK_SUCCESS;
	};
	const auto finishCommand = [&](bool invalid) {
		VkResult result = vkEndCommandBuffer(command);
		if (result != VK_SUCCESS) return false;
		if (markerRecorded && vkGetEventStatus(state->device, resources->event) != VK_EVENT_RESET) return false;
		VkSubmitInfo submit {}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
		result = vkQueueSubmit(state->queue, 1, &submit, state->fence);
		bool accepted = false;
		if (invalid) accepted = result == VK_ERROR_INITIALIZATION_FAILED;
		else if (result == VK_SUCCESS)
		{
			const bool readyBeforeFence = !markerRecorded || WaitForCompletionEvent(state->device, resources->event);
			const VkResult waited = vkWaitForFences(state->device, 1, &state->fence, VK_TRUE, kCompletionTimeoutNanoseconds);
			const VkResult reset = waited == VK_SUCCESS ? vkResetFences(state->device, 1, &state->fence) : waited;
			accepted = readyBeforeFence && waited == VK_SUCCESS && reset == VK_SUCCESS;
		}
		if (markerRecorded)
			accepted = accepted && vkGetEventStatus(state->device, resources->event) == (invalid ? VK_EVENT_RESET : VK_EVENT_SET);
		markerRecorded = false;
		vkFreeCommandBuffers(state->device, state->commandPool, 1, &command); command = VK_NULL_HANDLE;
		return accepted;
	};
	const auto bindState = [&] {
		constexpr VkDeviceSize offset = 0;
		const VkRect2D scissor {{0, 0}, {kWidth, kHeight}};
		vkCmdBindVertexBuffers(command, 0, 1, &state->vertices.buffer, &offset);
		const bool storage = secondPass && (boundary == PassBoundary::WritableStorage || boundary == PassBoundary::ReadOnlyStorage ||
		                                    boundary == PassBoundary::EmptyUnsafe);
		if (storage) vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, resources->storagePipelineLayout,
		                                   0, 1, &resources->storageSet, 0, nullptr);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, storage ?
		    resources->storagePipelines[boundary == PassBoundary::ReadOnlyStorage ? 1 : 0] : state->pipelines[Background]);
		vkCmdSetScissor(command, 0, 1, &scissor);
	};
	if (!beginCommand()) return Fail("Coalescing boundary command allocation failed");
	if (!resources->attachmentLayoutsReady)
	{
		std::array<VkImageMemoryBarrier, 2> barriers {};
		for (size_t i = 0; i < barriers.size(); ++i)
		{
			auto& b = barriers[i]; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b.dstAccessMask = i == 0 ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT :
			                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = kAttachmentLayouts[i];
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = state->images[i]; b.subresourceRange = {kAspects[i], 0, 1, 0, 1};
		}
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
		                     0, 0, nullptr, 0, nullptr, barriers.size(), barriers.data());
		resources->attachmentLayoutsReady = true;
	}
	std::array<VkClearValue, 2> clears {}; clears[1].depthStencil.depth = 1;
	VkRenderPassBeginInfo render {}; render.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	// DONT_CARE uses the same pass object on both sides so identity alone
	// cannot explain a refusal to merge. Contents of discarded aspects are not
	// compared; the positive pixel scenes provide the defined-output oracle.
	render.renderPass = boundary == PassBoundary::DontCareLoad ? resources->passes[1] :
	                    boundary == PassBoundary::DontCareStore ? resources->passes[2] : state->loadRenderPass;
	render.framebuffer = state->framebuffer; render.renderArea.extent = {kWidth, kHeight};
	render.clearValueCount = clears.size(); render.pClearValues = clears.data();
	vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
	bindState(); vkCmdDraw(command, 3, 1, 3, 0); vkCmdEndRenderPass(command);
	if (leadingMarker)
	{
		vkCmdSetEvent(command, resources->event, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
		markerRecorded = true;
	}
	if (emptyPass)
	{
		// No draw consumes the marker here. It must carry into the following
		// logical pass, where the eventual draw or barrier decides its fate.
		vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
		vkCmdEndRenderPass(command);
	}
	if (boundary == PassBoundary::InvalidDraw || boundary == PassBoundary::InvalidEnd)
	{
		if (boundary == PassBoundary::InvalidDraw) vkCmdDraw(command, 3, 1, 3, 0);
		else vkCmdEndRenderPass(command);
		return finishCommand(true) && CompletionPassStats().ExpectDelta(before, 0, 0) ? 0 : Fail("Logical pass validation regression");
	}
	switch (boundary)
	{
		case PassBoundary::Pass: render.renderPass = resources->passes[0]; break;
		case PassBoundary::Framebuffer: render.framebuffer = resources->framebuffer; break;
		case PassBoundary::Area: render.renderArea.extent.width = kWidth / 2; break;
		case PassBoundary::Clear: render.renderPass = state->renderPass; break;
		case PassBoundary::Barrier:
		case PassBoundary::EmptyBarrier:
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			                     0, 0, nullptr, 0, nullptr, 0, nullptr); break;
		case PassBoundary::Copy: {
			const VkBufferCopy copy {0, 0, 256};
			vkCmdCopyBuffer(command, resources->source.buffer, resources->destination.buffer, 1, &copy); break;
		}
		case PassBoundary::Dispatch:
			vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, resources->compute);
			vkCmdDispatch(command, 1, 1, 1); break;
		case PassBoundary::Event:
		case PassBoundary::EventTop:
		case PassBoundary::EventMixed:
		case PassBoundary::EventZero: {
			const VkPipelineStageFlags stages = boundary == PassBoundary::Event ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT :
			    boundary == PassBoundary::EventTop ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT :
			    boundary == PassBoundary::EventMixed ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : 0;
			vkCmdSetEvent(command, resources->event, stages);
			markerRecorded = true;
			break;
		}
		case PassBoundary::CommandBuffer:
			if (!finishCommand(false) || !beginCommand()) return Fail("Command-buffer boundary submission failed");
			break;
		default: break;
	}
	secondPass = true;
	if (boundary == PassBoundary::BindState) bindState();
	vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
	bindState(); vkCmdDraw(command, 3, 1, 3, 0); vkCmdEndRenderPass(command);
	if (!finishCommand(false)) return Fail("Coalescing boundary submit failed");
	if (boundary == PassBoundary::Copy && (InvalidateBuffer(*state, resources->destination) != VK_SUCCESS ||
	    std::memcmp(resources->source.mapped, resources->destination.mapped, 256) != 0)) return Fail("Boundary copy did not execute");
	const uint64_t merges = !before.enabled ? 0 : boundary == PassBoundary::EmptyCarry ? 2 :
	    emptyPass || boundary == PassBoundary::Same || boundary == PassBoundary::BindState ||
	    boundary == PassBoundary::ReadOnlyStorage || boundary == PassBoundary::Event ? 1 : 0;
	const uint64_t markerMerges = before.enabled && (boundary == PassBoundary::EmptyCarry ||
	    (!emptyPass && (leadingMarker || boundary == PassBoundary::Event) && merges != 0)) ? 1 : 0;
	return CompletionPassStats().ExpectDelta(before, emptyPass ? 3 : 2, merges, markerMerges) ? 0 :
	       Fail("Incorrect physical encoder boundary");
}

bool CheckAttachmentAliases(ProbeState* state)
{
	struct Aliases
	{
		VkDevice device;
		VkImage image = VK_NULL_HANDLE;
		VkImageView sameImageView = VK_NULL_HANDLE, sharedMemoryView = VK_NULL_HANDLE;
		VkBuffer buffer = VK_NULL_HANDLE;
		~Aliases()
		{
			if (sameImageView != VK_NULL_HANDLE) vkDestroyImageView(device, sameImageView, nullptr);
			if (sharedMemoryView != VK_NULL_HANDLE) vkDestroyImageView(device, sharedMemoryView, nullptr);
			if (image != VK_NULL_HANDLE) vkDestroyImage(device, image, nullptr);
			if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
		}
	} aliases {state->device};
	VkImageViewCreateInfo view {}; view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = state->images[0]; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = kFormats[0];
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(state->device, &view, nullptr, &aliases.sameImageView) != VK_SUCCESS) return false;
	VkImageCreateInfo image {}; image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D; image.format = kFormats[0]; image.extent = {kWidth, kHeight, 1};
	image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (vkCreateImage(state->device, &image, nullptr, &aliases.image) != VK_SUCCESS ||
	    vkBindImageMemory(state->device, aliases.image, state->imageMemory[0], 0) != VK_SUCCESS) return false;
	view.image = aliases.image;
	if (vkCreateImageView(state->device, &view, nullptr, &aliases.sharedMemoryView) != VK_SUCCESS) return false;
	VkBufferCreateInfo buffer {}; buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = 256; buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (vkCreateBuffer(state->device, &buffer, nullptr, &aliases.buffer) != VK_SUCCESS ||
	    vkBindBufferMemory(state->device, aliases.buffer, state->imageMemory[0], 0) != VK_SUCCESS) return false;
	// These are real adapter handles passed to the production draw predicate.
	// No invalid attachment-feedback draw is submitted to the GPU.
	return LumenMetalResourceAliasesRenderAttachmentForTest(state->views[0], aliases.sameImageView, VK_NULL_HANDLE) &&
	       LumenMetalResourceAliasesRenderAttachmentForTest(state->views[0], aliases.sharedMemoryView, VK_NULL_HANDLE) &&
	       LumenMetalResourceAliasesRenderAttachmentForTest(state->views[0], VK_NULL_HANDLE, aliases.buffer) &&
	       !LumenMetalResourceAliasesRenderAttachmentForTest(state->views[0], state->views[1], VK_NULL_HANDLE) &&
	       !LumenMetalResourceAliasesRenderAttachmentForTest(state->views[0], VK_NULL_HANDLE, state->vertices.buffer);
}
} // namespace

int GraphicsCompletionPassReuseVulkanProbe()
{
	const char* expectedText = std::getenv("LUMEN_EXPECT_METAL_COMPLETION_PASS_REUSE");
	if (expectedText == nullptr || (std::strcmp(expectedText, "0") != 0 && std::strcmp(expectedText, "1") != 0))
		return Fail("Missing completion-pass selector oracle");
	const uint32_t expected = std::strcmp(expectedText, "1") == 0 ? 1 : 0;
	if (CompletionPassStats().enabled != expected) return Fail("Completion-pass selector mismatch");
	if (setenv("LUMEN_METAL_COMPLETION_PASS_REUSE", expected ? "0" : "1", 1) != 0 ||
	    CompletionPassStats().enabled != expected) return Fail("Completion-pass selector is not latched");
	ProbeState state;
	if (Initialize(&state) != 0) return 1;
	CompletionPassResources resources {&state};
	if (!resources.Initialize()) return Fail("Completion-pass boundary resources failed");
	for (bool withMarker: {false, true})
	{
		const VkEvent event = withMarker ? resources.event : VK_NULL_HANDLE;
		for (bool nearFirst: {false, true})
		{
			const CompletionPassStats before;
			if (DrawAndVerify(&state, CutoutLate, nearFirst, nullptr, true, nullptr, event) != 0 ||
			    !CompletionPassStats().ExpectDelta(before, 3, expected, withMarker ? expected : 0))
				return Fail("Completion-pass color/depth output failed");
		}
		for (bool reverse: {false, true})
		{
			const CompletionPassStats before;
			const StencilScenario stencil {false, false, reverse, 0x40, 0x04};
			if (DrawAndVerify(&state, OpaqueNear, true, &stencil, true, nullptr, event) != 0 ||
			    !CompletionPassStats().ExpectDelta(before, 3, expected, withMarker ? expected : 0))
				return Fail("Completion-pass stencil output failed");
		}
		{
			const CompletionPassStats before;
			const LateUnsafeDraw late {resources.storagePipelines[0], resources.storagePipelineLayout, resources.storageSet,
			                           resources.indices.buffer};
			if (DrawAndVerify(&state, CutoutLate, false, nullptr, true, &late, event) != 0 ||
			    !CompletionPassStats().ExpectDelta(before, 3, 0, 0))
				return Fail("Late unsafe indexed draw did not preserve attachments/state or retract marker merge");
		}
	}
	for (const auto boundary: {PassBoundary::Same, PassBoundary::BindState, PassBoundary::Pass, PassBoundary::Framebuffer,
	    PassBoundary::Area, PassBoundary::Clear, PassBoundary::DontCareLoad, PassBoundary::DontCareStore, PassBoundary::Barrier,
	    PassBoundary::Copy, PassBoundary::Dispatch, PassBoundary::Event, PassBoundary::EventTop, PassBoundary::EventMixed,
	    PassBoundary::EventZero, PassBoundary::CommandBuffer,
	    PassBoundary::WritableStorage, PassBoundary::ReadOnlyStorage,
	    PassBoundary::InvalidDraw, PassBoundary::InvalidEnd})
	{
		if (CheckPassBoundary(&state, &resources, boundary) != 0)
		{
			std::fprintf(stderr, "Completion-pass boundary case=%u failed\n", static_cast<unsigned>(boundary));
			return 1;
		}
	}
	// A completion marker cannot bypass a later barrier or attachment/resource
	// rejection, nor make an illegal command valid outside a logical pass.
	for (const auto boundary: {PassBoundary::Same, PassBoundary::BindState, PassBoundary::Pass, PassBoundary::Framebuffer,
	    PassBoundary::Area, PassBoundary::Clear, PassBoundary::DontCareLoad, PassBoundary::DontCareStore, PassBoundary::Barrier,
	    PassBoundary::Copy, PassBoundary::Dispatch, PassBoundary::CommandBuffer,
	    PassBoundary::WritableStorage, PassBoundary::ReadOnlyStorage, PassBoundary::InvalidDraw, PassBoundary::InvalidEnd})
	{
		if (CheckPassBoundary(&state, &resources, boundary, true) != 0)
		{
			std::fprintf(stderr, "Completion-marker boundary case=%u failed\n", static_cast<unsigned>(boundary));
			return 1;
		}
	}
	for (const auto boundary: {PassBoundary::EmptyCarry, PassBoundary::EmptyUnsafe, PassBoundary::EmptyBarrier})
	{
		if (CheckPassBoundary(&state, &resources, boundary, true) != 0)
		{
			std::fprintf(stderr, "Completion-marker empty-pass case=%u failed\n", static_cast<unsigned>(boundary));
			return 1;
		}
	}
	if (!CheckAttachmentAliases(&state)) return Fail("Attachment alias production predicate failed");
	std::printf("[GFX_COMPLETION_PASS_REUSE_PROBE] enabled=%u latched=1 pixel-scenes=10 color-depth-stencil=verified "
	            "physical-encoder-counters=verified actual-marker-merges=verified boundary-cases=39 empty-pass-carry=verified storage-policy=verified "
	            "late-indexed-hazard=verified attachment-aliases=verified event-reset-submit-complete=verified "
	            "event-before-fence=verified later-readback-at-event=verified "
	            "logical-pass-negatives=verified status=clean\n", expected);
	return std::fflush(stdout) == 0 ? 0 : 1;
}
