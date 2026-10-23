#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t     kWidth = 32, kHeight = 32, kRowPixels = 64, kImageRows = 40;
constexpr VkDeviceSize kGuard            = 16;
constexpr VkDeviceSize kUploadRowBytes   = (kRowPixels / 4) * 16;
constexpr VkDeviceSize kUploadPayload    = kUploadRowBytes * (kImageRows / 4);
constexpr VkDeviceSize kReadbackRowBytes = kRowPixels * 4;
constexpr VkDeviceSize kReadbackPayload  = kReadbackRowBytes * kImageRows;
constexpr uint8_t      kUploadGuard = 0xa5, kUploadPadding = 0xe3, kReadbackGuard = 0x5a, kUnset = 0xcd;
constexpr uint64_t     kTimeout = 5'000'000'000ull;

// Original mode-6 fixture encoder from the separately researched Microsoft
// layout facts. Endpoint fields are R0,R1,G0,G1,B0,B1,A0,A1, then P0/P1.
// Identical endpoints and zero indices produce a known solid block. No BC7
// software decoder or external encoder implementation is incorporated.
constexpr std::array<uint8_t, 16> SolidBlock(const std::array<uint8_t, 4>& high, uint8_t p)
{
	std::array<uint8_t, 16> bytes {};
	bytes[0]     = 0x40;
	uint32_t bit = 7;
	for (uint32_t channel = 0; channel < 4; ++channel)
		for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
			for (uint32_t lane = 0; lane < 7; ++lane, ++bit)
				bytes[bit / 8] |= static_cast<uint8_t>(((high[channel] >> lane) & 1u) << (bit % 8));
	for (uint32_t endpoint = 0; endpoint < 2; ++endpoint, ++bit)
		bytes[bit / 8] |= static_cast<uint8_t>((p & 1u) << (bit % 8));
	return bytes;
}
constexpr std::array<std::array<uint8_t, 4>, 6> kHighEndpoints {
    {{{0, 0, 0, 0}}, {{127, 127, 127, 127}}, {{127, 0, 0, 127}}, {{0, 127, 0, 127}}, {{0, 0, 127, 127}}, {{127, 127, 127, 63}}}};
// Independent expected pixels; these are never obtained by decoding blocks.
constexpr std::array<std::array<uint8_t, 4>, 6> kExpected {
    {{{0, 0, 0, 0}}, {{255, 255, 255, 255}}, {{255, 1, 1, 255}}, {{1, 255, 1, 255}}, {{1, 1, 255, 255}}, {{255, 255, 255, 127}}}};
constexpr uint32_t FixtureIndex(uint32_t x, uint32_t y, uint32_t pass)
{
	return (x + 3 * y + (pass ? 1 : 0)) % kExpected.size();
}
std::array<uint8_t, 16> FixtureBlock(uint32_t x, uint32_t y, uint32_t pass)
{
	const uint32_t index = FixtureIndex(x, y, pass);
	return SolidBlock(kHighEndpoints[index], index == 0 ? 0 : 1);
}
std::array<uint8_t, 4> ExpectedPixel(uint32_t x, uint32_t y, uint32_t pass)
{
	return kExpected[FixtureIndex(x, y, pass)];
}
constexpr bool BasicBlocksMatchPrimaryFacts()
{
	const auto black = SolidBlock(kHighEndpoints[0], 0), white = SolidBlock(kHighEndpoints[1], 1);
	for (uint32_t byte = 0; byte < 16; ++byte)
	{
		if (black[byte] != (byte == 0 ? 0x40 : 0)) return false;
		if (white[byte] != (byte == 0 ? 0xc0 : byte < 8 ? 0xff : byte == 8 ? 1 : 0)) return false;
	}
	return true;
}
static_assert(BasicBlocksMatchPrimaryFacts());

constexpr char kVertexSource[] = R"spirv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %position %vertex_index
OpDecorate %position BuiltIn Position
OpDecorate %vertex_index BuiltIn VertexIndex
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%v4float = OpTypeVector %float 4
%in_int = OpTypePointer Input %int
%out_v4 = OpTypePointer Output %v4float
%int_1 = OpConstant %int 1
%int_2 = OpConstant %int 2
%zero = OpConstant %float 0
%one = OpConstant %float 1
%minus_one = OpConstant %float -1
%three = OpConstant %float 3
%position = OpVariable %out_v4 Output
%vertex_index = OpVariable %in_int Input
%main = OpFunction %void None %fn
%entry = OpLabel
%index = OpLoad %int %vertex_index
%right = OpIEqual %bool %index %int_1
%bottom = OpIEqual %bool %index %int_2
%x = OpSelect %float %right %three %minus_one
%y = OpSelect %float %bottom %three %minus_one
%p = OpCompositeConstruct %v4float %x %y %zero %one
OpStore %position %p
OpReturn
OpFunctionEnd
)spirv";

constexpr char kFragmentSource[] = R"spirv(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %frag_coord %color
OpExecutionMode %main OriginUpperLeft
OpDecorate %frag_coord BuiltIn FragCoord
OpDecorate %color Location 0
OpDecorate %texture DescriptorSet 0
OpDecorate %texture Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v2float = OpTypeVector %float 2
%v4float = OpTypeVector %float 4
%image = OpTypeImage %float 2D 0 0 0 1 Unknown
%sampled = OpTypeSampledImage %image
%uc_sampled = OpTypePointer UniformConstant %sampled
%in_v4 = OpTypePointer Input %v4float
%out_v4 = OpTypePointer Output %v4float
%zero = OpConstant %float 0
%scale = OpConstant %float 0.03125
%texture = OpVariable %uc_sampled UniformConstant
%frag_coord = OpVariable %in_v4 Input
%color = OpVariable %out_v4 Output
%main = OpFunction %void None %fn
%entry = OpLabel
%coord = OpLoad %v4float %frag_coord
%x = OpCompositeExtract %float %coord 0
%y = OpCompositeExtract %float %coord 1
%xy = OpCompositeConstruct %v2float %x %y
%uv = OpVectorTimesScalar %v2float %xy %scale
%tex = OpLoad %sampled %texture
%value = OpImageSampleExplicitLod %v4float %tex %uv Lod %zero
OpStore %color %value
OpReturn
OpFunctionEnd
)spirv";

struct Buffer
{
	VkBuffer       object = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void*          mapped = nullptr;
	VkDeviceSize   size   = 0;
};
struct Image
{
	VkImage        object = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView    view   = VK_NULL_HANDLE;
};
struct State
{
	VkInstance            instance = VK_NULL_HANDLE;
	VkPhysicalDevice      physical = VK_NULL_HANDLE;
	VkDevice              device   = VK_NULL_HANDLE;
	VkQueue               queue    = VK_NULL_HANDLE;
	uint32_t              family   = UINT32_MAX;
	Buffer                upload, readback;
	Image                 sampled, output;
	VkSampler             sampler        = VK_NULL_HANDLE;
	VkDescriptorSetLayout setLayout      = VK_NULL_HANDLE;
	VkDescriptorPool      descriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet       descriptor     = VK_NULL_HANDLE;
	VkRenderPass          renderPass     = VK_NULL_HANDLE;
	VkFramebuffer         framebuffer    = VK_NULL_HANDLE;
	VkPipelineLayout      pipelineLayout = VK_NULL_HANDLE;
	VkPipeline            pipeline       = VK_NULL_HANDLE;
	VkShaderModule        vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
	VkCommandPool         commandPool = VK_NULL_HANDLE;
	VkCommandBuffer       command     = VK_NULL_HANDLE;
	VkFence               fence       = VK_NULL_HANDLE;
	~State()
	{
		if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
		if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
		if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
		if (vertex != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertex, nullptr);
		if (fragment != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragment, nullptr);
		if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if (renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, renderPass, nullptr);
		if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
		if (sampler != VK_NULL_HANDLE) vkDestroySampler(device, sampler, nullptr);
		for (Image* image: {&sampled, &output})
		{
			if (image->view != VK_NULL_HANDLE) vkDestroyImageView(device, image->view, nullptr);
			if (image->object != VK_NULL_HANDLE) vkDestroyImage(device, image->object, nullptr);
			if (image->memory != VK_NULL_HANDLE) vkFreeMemory(device, image->memory, nullptr);
		}
		for (Buffer* buffer: {&upload, &readback})
		{
			if (buffer->mapped != nullptr) vkUnmapMemory(device, buffer->memory);
			if (buffer->object != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer->object, nullptr);
			if (buffer->memory != VK_NULL_HANDLE) vkFreeMemory(device, buffer->memory, nullptr);
		}
		if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
		if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
	}
};

bool Check(VkResult result, const char* operation)
{
	if (result == VK_SUCCESS) return true;
	std::fprintf(stderr, "%s: %d\n", operation, result);
	return false;
}

uint32_t MemoryType(State& state, uint32_t mask, VkMemoryPropertyFlags flags)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(state.physical, &properties);
	for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
		if ((mask & (1u << i)) != 0 && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
	return UINT32_MAX;
}

bool CreateBuffer(State& state, VkDeviceSize size, VkBufferUsageFlags usage, Buffer* buffer)
{
	buffer->size = size;
	VkBufferCreateInfo info {};
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size  = size;
	info.usage = usage;
	if (!Check(vkCreateBuffer(state.device, &info, nullptr, &buffer->object), "vkCreateBuffer")) return false;
	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(state.device, buffer->object, &requirements);
	const uint32_t type =
	    MemoryType(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX) return false;
	VkMemoryAllocateInfo allocation {};
	allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize  = requirements.size;
	allocation.memoryTypeIndex = type;
	return Check(vkAllocateMemory(state.device, &allocation, nullptr, &buffer->memory), "vkAllocateMemory(buffer)") &&
	       Check(vkBindBufferMemory(state.device, buffer->object, buffer->memory, 0), "vkBindBufferMemory") &&
	       Check(vkMapMemory(state.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped), "vkMapMemory") &&
	       buffer->mapped != nullptr;
}

bool CreateImage(State& state, VkFormat format, VkImageUsageFlags usage, Image* image)
{
	VkImageCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType   = VK_IMAGE_TYPE_2D;
	info.format      = format;
	info.extent      = {kWidth, kHeight, 1};
	info.mipLevels   = 1;
	info.arrayLayers = 1;
	info.samples     = VK_SAMPLE_COUNT_1_BIT;
	info.tiling      = VK_IMAGE_TILING_OPTIMAL;
	info.usage       = usage;
	if (!Check(vkCreateImage(state.device, &info, nullptr, &image->object), "vkCreateImage")) return false;
	VkMemoryRequirements requirements {};
	vkGetImageMemoryRequirements(state.device, image->object, &requirements);
	const uint32_t type = MemoryType(state, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX) return false;
	VkMemoryAllocateInfo allocation {};
	allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize  = requirements.size;
	allocation.memoryTypeIndex = type;
	if (!Check(vkAllocateMemory(state.device, &allocation, nullptr, &image->memory), "vkAllocateMemory(image)") ||
	    !Check(vkBindImageMemory(state.device, image->object, image->memory, 0), "vkBindImageMemory"))
		return false;
	VkImageViewCreateInfo view {};
	view.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image            = image->object;
	view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
	view.format           = format;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return Check(vkCreateImageView(state.device, &view, nullptr, &image->view), "vkCreateImageView");
}

bool Shader(State& state, const char* assembly, VkShaderModule* module)
{
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& pos, const char* message)
	                         { std::fprintf(stderr, "SPIR-V line %zu: %s\n", pos.line, message); });
	std::vector<uint32_t> words;
	if (!tools.Assemble(assembly, std::strlen(assembly), &words) || !tools.Validate(words)) return false;
	VkShaderModuleCreateInfo info {};
	info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = words.size() * sizeof(uint32_t);
	info.pCode    = words.data();
	return Check(vkCreateShaderModule(state.device, &info, nullptr, module), "vkCreateShaderModule");
}

bool Init(State& state)
{
	VkApplicationInfo app {};
	app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "Lumen BC7 sampled texture probe";
	app.apiVersion       = VK_API_VERSION_1_1;
	VkInstanceCreateInfo instance {};
	instance.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &app;
	if (!Check(vkCreateInstance(&instance, nullptr, &state.instance), "vkCreateInstance")) return false;
	uint32_t count = 0;
	if (!Check(vkEnumeratePhysicalDevices(state.instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)") || count == 0)
		return false;
	std::vector<VkPhysicalDevice> devices(count);
	if (!Check(vkEnumeratePhysicalDevices(state.instance, &count, devices.data()), "vkEnumeratePhysicalDevices(list)")) return false;
	for (VkPhysicalDevice physical: devices)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
		for (uint32_t family = 0; family < familyCount; ++family)
			if (families[family].queueCount != 0 && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
			{
				state.physical = physical;
				state.family   = family;
				break;
			}
		if (state.physical != VK_NULL_HANDLE) break;
	}
	if (state.physical == VK_NULL_HANDLE) return false;
	VkFormatProperties bc7 {}, color {};
	vkGetPhysicalDeviceFormatProperties(state.physical, VK_FORMAT_BC7_UNORM_BLOCK, &bc7);
	vkGetPhysicalDeviceFormatProperties(state.physical, VK_FORMAT_R8G8B8A8_UNORM, &color);
	constexpr VkFormatFeatureFlags sampledFeatures =
	    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
	    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
	constexpr VkFormatFeatureFlags colorFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	constexpr VkFormatFeatureFlags forbidden     = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	                                               VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
	if ((bc7.optimalTilingFeatures & sampledFeatures) != sampledFeatures || (color.optimalTilingFeatures & colorFeatures) != colorFeatures)
	{
		std::fprintf(stderr, "BC7 sampled/transfer-dst or RGBA8 attachment/transfer-src capability absent: bc7=%x color=%x\n",
		             bc7.optimalTilingFeatures, color.optimalTilingFeatures);
		return false;
	}
	if ((bc7.optimalTilingFeatures & forbidden) != 0)
	{
		std::fprintf(stderr, "BC7 advertises unsupported storage/color/blit-destination features\n");
		return false;
	}
	VkImageFormatProperties imageProperties {};
	if (!Check(vkGetPhysicalDeviceImageFormatProperties(
	               state.physical, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
	               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &imageProperties),
	           "vkGetPhysicalDeviceImageFormatProperties(BC7 sampled)") ||
	    imageProperties.maxExtent.width < kWidth || imageProperties.maxExtent.height < kHeight || imageProperties.maxMipLevels < 1 ||
	    imageProperties.maxArrayLayers < 1 || imageProperties.sampleCounts != VK_SAMPLE_COUNT_1_BIT)
		return false;
	for (const auto usage: {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_STORAGE_BIT})
		if (vkGetPhysicalDeviceImageFormatProperties(state.physical, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
		                                             usage, 0, &imageProperties) != VK_ERROR_FORMAT_NOT_SUPPORTED)
		{
			std::fprintf(stderr, "BC7 forbidden image usage was not rejected: %x\n", usage);
			return false;
		}
	if (vkGetPhysicalDeviceImageFormatProperties(state.physical, VK_FORMAT_BC7_UNORM_BLOCK, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR,
	                                             VK_IMAGE_USAGE_SAMPLED_BIT, 0, &imageProperties) != VK_ERROR_FORMAT_NOT_SUPPORTED)
	{
		std::fprintf(stderr, "BC7 linear image tiling was not rejected\n");
		return false;
	}
	const float             priority = 1;
	VkDeviceQueueCreateInfo queue {};
	queue.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = state.family;
	queue.queueCount       = 1;
	queue.pQueuePriorities = &priority;
	VkDeviceCreateInfo device {};
	device.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1;
	device.pQueueCreateInfos    = &queue;
	if (!Check(vkCreateDevice(state.physical, &device, nullptr, &state.device), "vkCreateDevice")) return false;
	vkGetDeviceQueue(state.device, state.family, 0, &state.queue);
	for (const auto samples: {VK_SAMPLE_COUNT_2_BIT, VK_SAMPLE_COUNT_4_BIT})
	{
		VkImageCreateInfo unsupported {};
		unsupported.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		unsupported.imageType   = VK_IMAGE_TYPE_2D;
		unsupported.format      = VK_FORMAT_BC7_UNORM_BLOCK;
		unsupported.extent      = {kWidth, kHeight, 1};
		unsupported.mipLevels   = 1;
		unsupported.arrayLayers = 1;
		unsupported.samples     = samples;
		unsupported.tiling      = VK_IMAGE_TILING_OPTIMAL;
		unsupported.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VkImage        invalid  = VK_NULL_HANDLE;
		const VkResult result   = vkCreateImage(state.device, &unsupported, nullptr, &invalid);
		if (result != VK_ERROR_FORMAT_NOT_SUPPORTED || invalid != VK_NULL_HANDLE)
		{
			if (invalid != VK_NULL_HANDLE) vkDestroyImage(state.device, invalid, nullptr);
			std::fprintf(stderr, "BC7 multisample image was not rejected: samples=%u result=%d\n", samples, result);
			return false;
		}
	}
	if (state.queue == VK_NULL_HANDLE ||
	    !CreateBuffer(state, kGuard * 2 + kUploadPayload, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &state.upload) ||
	    !CreateBuffer(state, kGuard * 2 + kReadbackPayload, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &state.readback) ||
	    !CreateImage(state, VK_FORMAT_BC7_UNORM_BLOCK,
	                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &state.sampled) ||
	    !CreateImage(state, VK_FORMAT_R8G8B8A8_UNORM,
	                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                 &state.output))
		return false;
	VkSamplerCreateInfo sampler {};
	sampler.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter    = VK_FILTER_NEAREST;
	sampler.minFilter    = VK_FILTER_NEAREST;
	sampler.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (!Check(vkCreateSampler(state.device, &sampler, nullptr, &state.sampler), "vkCreateSampler")) return false;
	VkDescriptorSetLayoutBinding binding {};
	binding.binding         = 0;
	binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo layout {};
	layout.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout.bindingCount = 1;
	layout.pBindings    = &binding;
	if (!Check(vkCreateDescriptorSetLayout(state.device, &layout, nullptr, &state.setLayout), "vkCreateDescriptorSetLayout")) return false;
	VkDescriptorPoolSize       poolSize {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
	VkDescriptorPoolCreateInfo pool {};
	pool.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets       = 1;
	pool.poolSizeCount = 1;
	pool.pPoolSizes    = &poolSize;
	if (!Check(vkCreateDescriptorPool(state.device, &pool, nullptr, &state.descriptorPool), "vkCreateDescriptorPool")) return false;
	VkDescriptorSetAllocateInfo allocate {};
	allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool     = state.descriptorPool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts        = &state.setLayout;
	if (!Check(vkAllocateDescriptorSets(state.device, &allocate, &state.descriptor), "vkAllocateDescriptorSets")) return false;
	VkDescriptorImageInfo descriptorImage {state.sampler, state.sampled.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkWriteDescriptorSet  write {};
	write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet          = state.descriptor;
	write.descriptorCount = 1;
	write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo      = &descriptorImage;
	vkUpdateDescriptorSets(state.device, 1, &write, 0, nullptr);
	VkAttachmentDescription attachment {};
	attachment.format         = VK_FORMAT_R8G8B8A8_UNORM;
	attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference reference {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription  subpass {};
	subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments    = &reference;
	VkRenderPassCreateInfo render {};
	render.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	render.attachmentCount = 1;
	render.pAttachments    = &attachment;
	render.subpassCount    = 1;
	render.pSubpasses      = &subpass;
	if (!Check(vkCreateRenderPass(state.device, &render, nullptr, &state.renderPass), "vkCreateRenderPass")) return false;
	VkFramebufferCreateInfo framebuffer {};
	framebuffer.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass      = state.renderPass;
	framebuffer.attachmentCount = 1;
	framebuffer.pAttachments    = &state.output.view;
	framebuffer.width           = kWidth;
	framebuffer.height          = kHeight;
	framebuffer.layers          = 1;
	if (!Check(vkCreateFramebuffer(state.device, &framebuffer, nullptr, &state.framebuffer), "vkCreateFramebuffer")) return false;
	VkPipelineLayoutCreateInfo pipelineLayout {};
	pipelineLayout.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayout.setLayoutCount = 1;
	pipelineLayout.pSetLayouts    = &state.setLayout;
	if (!Check(vkCreatePipelineLayout(state.device, &pipelineLayout, nullptr, &state.pipelineLayout), "vkCreatePipelineLayout") ||
	    !Shader(state, kVertexSource, &state.vertex) || !Shader(state, kFragmentSource, &state.fragment))
		return false;
	std::array<VkPipelineShaderStageCreateInfo, 2> stages {};
	for (auto& stage: stages)
	{
		stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage.pName = "main";
	}
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = state.vertex;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = state.fragment;
	VkPipelineVertexInputStateCreateInfo vertexInput {};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo assembly {};
	assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkViewport                        viewport {0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
	VkRect2D                          scissor {{0, 0}, {kWidth, kHeight}};
	VkPipelineViewportStateCreateInfo viewportState {};
	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports    = &viewport;
	viewportState.scissorCount  = 1;
	viewportState.pScissors     = &scissor;
	VkPipelineRasterizationStateCreateInfo raster {};
	raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode    = VK_CULL_MODE_NONE;
	raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth   = 1;
	VkPipelineMultisampleStateCreateInfo multisample {};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState blendAttachment {};
	blendAttachment.colorWriteMask =
	    VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo blend {};
	blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments    = &blendAttachment;
	VkGraphicsPipelineCreateInfo pipeline {};
	pipeline.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline.stageCount          = 2;
	pipeline.pStages             = stages.data();
	pipeline.pVertexInputState   = &vertexInput;
	pipeline.pInputAssemblyState = &assembly;
	pipeline.pViewportState      = &viewportState;
	pipeline.pRasterizationState = &raster;
	pipeline.pMultisampleState   = &multisample;
	pipeline.pColorBlendState    = &blend;
	pipeline.layout              = state.pipelineLayout;
	pipeline.renderPass          = state.renderPass;
	if (!Check(vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &state.pipeline),
	           "vkCreateGraphicsPipelines"))
		return false;
	VkCommandPoolCreateInfo commandPool {};
	commandPool.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPool.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commandPool.queueFamilyIndex = state.family;
	if (!Check(vkCreateCommandPool(state.device, &commandPool, nullptr, &state.commandPool), "vkCreateCommandPool")) return false;
	VkCommandBufferAllocateInfo commands {};
	commands.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commands.commandPool        = state.commandPool;
	commands.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commands.commandBufferCount = 1;
	VkFenceCreateInfo fence {};
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	return Check(vkAllocateCommandBuffers(state.device, &commands, &state.command), "vkAllocateCommandBuffers") &&
	       Check(vkCreateFence(state.device, &fence, nullptr, &state.fence), "vkCreateFence");
}

void Transition(State& state, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags sourceAccess,
                VkAccessFlags destinationAccess, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage)
{
	VkImageMemoryBarrier barrier {};
	barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask       = sourceAccess;
	barrier.dstAccessMask       = destinationAccess;
	barrier.oldLayout           = oldLayout;
	barrier.newLayout           = newLayout;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image                                             = image;
	barrier.subresourceRange                                  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(state.command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

enum class Operation
{
	DescriptorSample,
	BlitNearest,
	BlitLinear
};
bool Draw(State& state, bool first, uint32_t uploadRowPixels, Operation operation)
{
	if (!first && (!Check(vkResetFences(state.device, 1, &state.fence), "vkResetFences") ||
	               !Check(vkResetCommandPool(state.device, state.commandPool, 0), "vkResetCommandPool")))
		return false;
	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (!Check(vkBeginCommandBuffer(state.command, &begin), "vkBeginCommandBuffer")) return false;
	std::array<VkBufferMemoryBarrier, 2> host {};
	for (auto& barrier: host)
	{
		barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		barrier.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	}
	host[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	host[0].buffer        = state.upload.object;
	host[0].size          = state.upload.size;
	host[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host[1].buffer        = state.readback.object;
	host[1].size          = state.readback.size;
	vkCmdPipelineBarrier(state.command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 2, host.data(), 0,
	                     nullptr);
	Transition(state, state.sampled.object, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, first ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	           first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkBufferImageCopy copy {};
	copy.bufferOffset      = kGuard;
	copy.bufferRowLength   = uploadRowPixels;
	copy.bufferImageHeight = kImageRows;
	copy.imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent       = {kWidth, kHeight, 1};
	vkCmdCopyBufferToImage(state.command, state.upload.object, state.sampled.object, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	Transition(state, state.sampled.object, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	if (operation == Operation::DescriptorSample)
	{
		Transition(state, state.output.object, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, first ? 0 : VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		           first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
		VkClearValue clear {};
		clear.color.float32[0] = 0.25f;
		clear.color.float32[1] = 0.5f;
		clear.color.float32[2] = 0.75f;
		clear.color.float32[3] = 0.25f;
		VkRenderPassBeginInfo render {};
		render.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		render.renderPass        = state.renderPass;
		render.framebuffer       = state.framebuffer;
		render.renderArea.extent = {kWidth, kHeight};
		render.clearValueCount   = 1;
		render.pClearValues      = &clear;
		vkCmdBeginRenderPass(state.command, &render, VK_SUBPASS_CONTENTS_INLINE);
		// Follow the local NativeMetal probe order: the pipeline setter eagerly
		// applies descriptor resources from its recording snapshot.
		vkCmdBindDescriptorSets(state.command, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelineLayout, 0, 1, &state.descriptor, 0, nullptr);
		vkCmdBindPipeline(state.command, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipeline);
		vkCmdDraw(state.command, 3, 1, 0, 0);
		vkCmdEndRenderPass(state.command);
		Transition(state, state.output.object, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
	} else
	{
		Transition(state, state.sampled.object, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		           VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
		Transition(state, state.output.object, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkImageBlit region {};
		region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.dstSubresource = region.srcSubresource;
		region.srcOffsets[1]  = {static_cast<int32_t>(kWidth), static_cast<int32_t>(kHeight), 1};
		region.dstOffsets[1]  = region.srcOffsets[1];
		vkCmdBlitImage(state.command, state.sampled.object, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.output.object,
		               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
		               operation == Operation::BlitNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
		Transition(state, state.output.object, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_TRANSFER_BIT);
		Transition(state, state.sampled.object, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		           VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	}
	copy.bufferRowLength = kRowPixels;
	vkCmdCopyImageToBuffer(state.command, state.output.object, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.readback.object, 1, &copy);
	VkBufferMemoryBarrier readback {};
	readback.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	readback.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	readback.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	readback.srcQueueFamilyIndex = readback.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	readback.buffer                                             = state.readback.object;
	readback.size                                               = state.readback.size;
	vkCmdPipelineBarrier(state.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &readback, 0,
	                     nullptr);
	if (!Check(vkEndCommandBuffer(state.command), "vkEndCommandBuffer")) return false;
	VkSubmitInfo submit {};
	submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &state.command;
	return Check(vkQueueSubmit(state.queue, 1, &submit, state.fence), "vkQueueSubmit") &&
	       Check(vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, kTimeout), "vkWaitForFences");
}

bool Guards(const Buffer& buffer, VkDeviceSize payload, uint8_t byte)
{
	const auto* bytes = static_cast<const uint8_t*>(buffer.mapped);
	for (VkDeviceSize i = 0; i < kGuard; ++i)
		if (bytes[i] != byte || bytes[kGuard + payload + i] != byte) return false;
	return true;
}
} // namespace

int main()
{
	State state;
	if (!Init(state)) return 1;
	for (uint32_t pass = 0; pass < 3; ++pass)
	{
		const uint32_t uploadRowPixels = pass == 1 ? 48 : 64;
		const uint32_t uploadRowBytes  = uploadRowPixels / 4 * 16;
		auto*          upload          = static_cast<uint8_t*>(state.upload.mapped);
		auto*          readback        = static_cast<uint8_t*>(state.readback.mapped);
		std::memset(upload, kUploadGuard, static_cast<size_t>(state.upload.size));
		std::memset(upload + kGuard, kUploadPadding, static_cast<size_t>(kUploadPayload));
		for (uint32_t y = 0; y < kHeight / 4; ++y)
			for (uint32_t x = 0; x < kWidth / 4; ++x)
			{
				const auto block = FixtureBlock(x, y, pass == 1 ? 1 : 0);
				std::memcpy(upload + kGuard + y * uploadRowBytes + x * 16, block.data(), block.size());
			}
		const std::vector<uint8_t> uploadBefore(upload, upload + state.upload.size);
		for (const Operation operation: {Operation::DescriptorSample, Operation::BlitNearest, Operation::BlitLinear})
		{
			std::memset(readback, kReadbackGuard, static_cast<size_t>(state.readback.size));
			std::memset(readback + kGuard, kUnset, static_cast<size_t>(kReadbackPayload));
			std::array<VkMappedMemoryRange, 2> flush {};
			for (auto& range: flush)
			{
				range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
				range.size  = VK_WHOLE_SIZE;
			}
			flush[0].memory = state.upload.memory;
			flush[1].memory = state.readback.memory;
			if (!Check(vkFlushMappedMemoryRanges(state.device, 2, flush.data()), "vkFlushMappedMemoryRanges") ||
			    !Draw(state, pass == 0 && operation == Operation::DescriptorSample, uploadRowPixels, operation))
				return 1;
			VkMappedMemoryRange invalidate {};
			invalidate.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
			invalidate.memory = state.readback.memory;
			invalidate.size   = VK_WHOLE_SIZE;
			if (!Check(vkInvalidateMappedMemoryRanges(state.device, 1, &invalidate), "vkInvalidateMappedMemoryRanges")) return 1;
			if (!Guards(state.upload, kUploadPayload, kUploadGuard) || !Guards(state.readback, kReadbackPayload, kReadbackGuard) ||
			    std::memcmp(upload, uploadBefore.data(), uploadBefore.size()) != 0)
				return 1;
			for (uint32_t y = 0; y < kImageRows; ++y)
				for (uint32_t x = 0; x < kRowPixels; ++x)
				{
					const uint8_t* pixel = readback + kGuard + y * kReadbackRowBytes + x * 4;
					if (x >= kWidth || y >= kHeight)
					{
						for (uint32_t channel = 0; channel < 4; ++channel)
							if (pixel[channel] != kUnset) return 1;
						continue;
					}
					const auto expected = ExpectedPixel(x / 4, y / 4, pass == 1 ? 1 : 0);
					if (std::memcmp(pixel, expected.data(), expected.size()) != 0)
					{
						std::fprintf(stderr, "BC7 mismatch pass=%u operation=%u x=%u y=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n", pass,
						             static_cast<unsigned>(operation), x, y, pixel[0], pixel[1], pixel[2], pixel[3], expected[0],
						             expected[1], expected[2], expected[3]);
						return 1;
					}
				}
		}
	}
	std::fprintf(stderr, "[GFX_BC7_TEXTURE_VULKAN_PROBE] format=BC7_UNORM extent=32x32 passes=3 sequence=A-B-A pixels=9216 colors=6"
	                     " paths=descriptor-nearest,blit-nearest,blit-linear compressed-row-bytes=256,192,256 buffer-offset=16"
	                     " image-height=40 feature-negatives=clean guards=clean padding=unchanged upload=unchanged status=clean\n");
	return 0;
}
