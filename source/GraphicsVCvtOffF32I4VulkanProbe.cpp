#include "Lumen/VCvtOffF32I4SpirvCore.h"
#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

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
constexpr uint32_t kAmdVendorId = 0x1002u;
constexpr uint32_t kRequiredDeviceId = 0x13c0u;
constexpr uint32_t kAppleVendorId = 0x106bu;
constexpr char kAppleM1DeviceName[] = "Apple M1";
constexpr uint64_t kCompletionTimeoutNanoseconds = 30'000'000'000ull;
constexpr uint32_t kPrefixWords = 4;
constexpr uint32_t kWordsPerCase = 4;
constexpr uint32_t kOutputOffset = 1;
constexpr uint32_t kOutputUnset = 0xbaadf00du;
constexpr size_t kDispatchCount = 3;

enum class RuntimeMode { Amd13c0, NativeMetalAppleM1 };

struct ProbeCase
{
	std::string name;
	uint32_t input;
	uint32_t expected;
};

// Original finite-domain vectors. Inputs are canonical whole signed-I32
// values in [-8,7]; this arithmetic probe grants no guest register-source
// admission and makes no claim about selecting bits from another raw word.
std::vector<ProbeCase> MakeCases()
{
	return {
	    {"minus-eight", 0xfffffff8u, 0xbf000000u},
	    {"minus-seven", 0xfffffff9u, 0xbee00000u},
	    {"minus-six",   0xfffffffau, 0xbec00000u},
	    {"minus-five",  0xfffffffbu, 0xbea00000u},
	    {"minus-four",  0xfffffffcu, 0xbe800000u},
	    {"minus-three", 0xfffffffdu, 0xbe400000u},
	    {"minus-two",   0xfffffffeu, 0xbe000000u},
	    {"minus-one",   0xffffffffu, 0xbd800000u},
	    {"plus-zero",   0x00000000u, 0x00000000u},
	    {"one",         0x00000001u, 0x3d800000u},
	    {"two",         0x00000002u, 0x3e000000u},
	    {"three",       0x00000003u, 0x3e400000u},
	    {"four",        0x00000004u, 0x3e800000u},
	    {"five",        0x00000005u, 0x3ea00000u},
	    {"six",         0x00000006u, 0x3ec00000u},
	    {"seven",       0x00000007u, 0x3ee00000u},
	};
}

uint32_t CanaryForIndex(size_t index)
{
	return 0x5a17c3e9u ^ static_cast<uint32_t>(index * 0x01010101u);
}

std::vector<uint32_t> MakeInitialWords(const std::vector<ProbeCase>& cases)
{
	std::vector<uint32_t> words(kPrefixWords + cases.size() * kWordsPerCase + 4u);
	for (size_t index = 0; index < words.size(); ++index)
	{
		words[index] = CanaryForIndex(index);
	}
	for (size_t index = 0; index < cases.size(); ++index)
	{
		const size_t base = kPrefixWords + index * kWordsPerCase;
		words[base] = cases[index].input;
		words[base + kOutputOffset] = kOutputUnset;
	}
	return words;
}

// The compute wrapper uploads runtime words to exercise the backend;
// production admission and EXEC behavior are verified by production fixtures.
constexpr char kComputeShaderPreamble[] = R"spirv(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %global_id
               OpExecutionMode %main LocalSize 1 1 1
               OpName %source_bits "source_bits"
               OpName %converted "converted"
               OpName %result_bits "result_bits"
               OpDecorate %global_id BuiltIn GlobalInvocationId
               OpDecorate %words ArrayStride 4
               OpMemberDecorate %buffer_block 0 Offset 0
               OpDecorate %buffer_block BufferBlock
               OpDecorate %storage DescriptorSet 0
               OpDecorate %storage Binding 0
       %void = OpTypeVoid
   %function = OpTypeFunction %void
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
      %float = OpTypeFloat 32
     %v3uint = OpTypeVector %uint 3
      %words = OpTypeRuntimeArray %uint
%buffer_block = OpTypeStruct %words
%ptr_uniform_buffer = OpTypePointer Uniform %buffer_block
%ptr_uniform_uint = OpTypePointer Uniform %uint
%ptr_input_v3uint = OpTypePointer Input %v3uint
    %storage = OpVariable %ptr_uniform_buffer Uniform
  %global_id = OpVariable %ptr_input_v3uint Input
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_4 = OpConstant %uint 4
%float_0_062500 = OpConstant %float 0.0625
       %main = OpFunction %void None %function
      %entry = OpLabel
         %gid = OpLoad %v3uint %global_id
  %case_index = OpCompositeExtract %uint %gid 0
 %case_offset = OpIMul %uint %case_index %uint_4
        %base = OpIAdd %uint %case_offset %uint_4
%output_index = OpIAdd %uint %base %uint_1
  %source_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %base
 %source_bits = OpLoad %uint %source_ptr
)spirv";

constexpr char kComputeShaderSuffix[] = R"spirv(
 %result_bits = OpBitcast %uint %converted
  %output_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %output_index
               OpStore %output_ptr %result_bits
               OpReturn
               OpFunctionEnd
)spirv";

struct ShaderProgram
{
	std::vector<uint32_t> binary;
	std::string disassembly;
};

size_t CountToken(std::string_view text, std::string_view token)
{
	size_t count = 0;
	size_t position = 0;
	while ((position = text.find(token, position)) != std::string_view::npos)
	{
		++count;
		position += token.size();
	}
	return count;
}

void ReplaceAll(std::string* text, std::string_view token, std::string_view replacement)
{
	size_t position = 0;
	while ((position = text->find(token, position)) != std::string::npos)
	{
		text->replace(position, token.size(), replacement);
		position += replacement.size();
	}
}

bool HasRequiredShaderShape(std::string_view disassembly)
{
	const size_t input = disassembly.find("%source_bits = OpLoad %uint");
	const size_t conversion = disassembly.find("OpConvertSToF", input);
	const size_t scale = disassembly.find("%converted = OpFMul %float", conversion);
	const size_t bits = disassembly.find("%result_bits = OpBitcast %uint %converted", scale);
	const size_t store = disassembly.find("OpStore", bits);
	return input != std::string_view::npos && conversion != std::string_view::npos &&
	       scale != std::string_view::npos && bits != std::string_view::npos && store != std::string_view::npos &&
	       input < conversion && conversion < scale && scale < bits && bits < store &&
	       CountToken(disassembly, "OpConvertSToF") == 1 && CountToken(disassembly, "OpFMul") == 1 &&
	       CountToken(disassembly, "OpBitcast") == 2 && CountToken(disassembly, "OpLoad %uint") == 1 &&
	       CountToken(disassembly, "OpStore") == 1 && CountToken(disassembly, "OpConstant %float") == 1 &&
	       CountToken(disassembly, "OpConstant %float 0.0625") == 1 &&
	       CountToken(disassembly, "OpTypeFloat 32") == 1 && CountToken(disassembly, "OpTypeInt 32 1") == 1 &&
	       CountToken(disassembly, "OpBitField") == 0 && CountToken(disassembly, "OpShift") == 0 &&
	       CountToken(disassembly, "OpBitwise") == 0 &&
	       CountToken(disassembly, "OpSRem") == 0 && CountToken(disassembly, "OpSMod") == 0 &&
	       CountToken(disassembly, "OpFAdd") == 0 && CountToken(disassembly, "OpFSub") == 0 &&
	       CountToken(disassembly, "OpFDiv") == 0 && CountToken(disassembly, "OpConvertUToF") == 0 &&
	       CountToken(disassembly, "OpExtInst") == 0 && CountToken(disassembly, "OpTypeInt 64") == 0 &&
	       CountToken(disassembly, "SPV_KHR_float_controls") == 0;
}

bool BuildAndAuditShader(ShaderProgram* program)
{
	std::string arithmetic = Lumen::Libs::Graphics::Detail::kVCvtOffF32I4SpirvArithmetic;
	ReplaceAll(&arithmetic, "<input>", "source_bits");
	ReplaceAll(&arithmetic, "<index>", "probe");
	ReplaceAll(&arithmetic, "<result>", "converted");
	if (arithmetic.find('<') != std::string::npos) return false;
	const std::string assembly = std::string(kComputeShaderPreamble) + arithmetic + kComputeShaderSuffix;
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                         { std::fprintf(stderr, "SPIR-V line %zu column %zu: %s\n", position.line, position.column, message); });
	if (!tools.Assemble(assembly.data(), assembly.size(), &program->binary) || !tools.Validate(program->binary))
	{
		std::fprintf(stderr, "V_CVT_OFF_F32_I4 probe SPIR-V assembly or validation failed\n");
		return false;
	}
	if (!tools.Disassemble(program->binary, &program->disassembly) || !HasRequiredShaderShape(program->disassembly))
	{
		std::fprintf(stderr, "V_CVT_OFF_F32_I4 probe SPIR-V shape audit failed\n");
		return false;
	}
	return true;
}

struct ProbeState
{
	VkInstance instance = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	VkShaderModule shaderModule = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* mapped = nullptr;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
		if (mapped != nullptr) vkUnmapMemory(device, memory);
		if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
		if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
		if (shaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, shaderModule, nullptr);
		if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		if (descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
		if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
		if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
		if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
	}
};

int Fail(const char* operation, VkResult result = VK_SUCCESS)
{
	if (result == VK_SUCCESS) std::fprintf(stderr, "%s\n", operation);
	else std::fprintf(stderr, "%s: VkResult=%d\n", operation, static_cast<int>(result));
	return 1;
}

uint32_t FindMemoryType(VkPhysicalDevice device, uint32_t typeBits, VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(device, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((typeBits & (1u << index)) != 0 &&
		    (properties.memoryTypes[index].propertyFlags & required) == required) return index;
	}
	return UINT32_MAX;
}

bool InputsAndCanariesUnchanged(const std::vector<uint32_t>& initial,
	                            const std::vector<uint32_t>& observed,
	                            size_t caseCount, size_t dispatch)
{
	for (size_t index = 0; index < observed.size(); ++index)
	{
		bool output = false;
		if (index >= kPrefixWords && index < kPrefixWords + caseCount * kWordsPerCase)
		{
			output = (index - kPrefixWords) % kWordsPerCase == kOutputOffset;
		}
		if (!output && observed[index] != initial[index])
		{
			std::fprintf(stderr, "V_CVT_OFF_F32_I4 modified protected word: dispatch=%zu word=%zu\n", dispatch, index);
			return false;
		}
	}
	return true;
}
} // namespace

int main(int argc, char** argv)
{
	bool shaderOnly = false;
	RuntimeMode runtimeMode = RuntimeMode::Amd13c0;
	if (argc == 2 && std::strcmp(argv[1], "--shader-only") == 0) shaderOnly = true;
	else if (argc == 2 && std::strcmp(argv[1], "--native-metal-apple-m1") == 0)
	{
#if defined(LUMEN_VCVT_OFF_F32_I4_PROBE_NATIVE_METAL_APPLE_M1)
		runtimeMode = RuntimeMode::NativeMetalAppleM1;
#else
		return Fail("--native-metal-apple-m1 is unavailable in this build");
#endif
	}
	else if (argc != 1)
	{
		return Fail("usage: lumen_graphics_vcvt_off_f32_i4_vulkan_probe "
		            "[--shader-only|--native-metal-apple-m1]");
	}

	ShaderProgram shader;
	if (!BuildAndAuditShader(&shader)) return 1;
	const std::vector<ProbeCase> cases = MakeCases();
	if (shaderOnly)
	{
		std::printf("[GFX_VCVT_OFF_F32_I4_VULKAN_PROBE] mode=shader-only scope=diagnostic-production-core "
		            "core=shared-signed-convert-scale spirv=assembled+validated+shape-audited "
		            "domain=all-known-signed4 nibble-extraction=none "
		            "cases=%zu vulkan-created=no status=clean\n", cases.size());
		return std::fflush(stdout) == 0 ? 0 : 1;
	}

	ProbeState state;
	VkApplicationInfo appInfo {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Lumen V_CVT_OFF_F32_I4 Vulkan diagnostic probe";
	appInfo.applicationVersion = 1;
	appInfo.pEngineName = "Lumen";
	appInfo.engineVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo instanceInfo {};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;
	VkResult result = vkCreateInstance(&instanceInfo, nullptr, &state.instance);
	if (result != VK_SUCCESS) return Fail("vkCreateInstance failed", result);

	uint32_t physicalDeviceCount = 0;
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0) return Fail("No Vulkan physical device found", result);
	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS) return Fail("vkEnumeratePhysicalDevices(list) failed", result);
	physicalDevices.resize(physicalDeviceCount);

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties selectedProperties {};
	const bool nativeMetal = runtimeMode == RuntimeMode::NativeMetalAppleM1;
	const char* reportedType = nullptr;
	if (nativeMetal)
	{
		if (physicalDevices.size() != 1) return Fail("Native Metal Apple M1 probe requires exactly one Vulkan device");
		physicalDevice = physicalDevices.front();
		vkGetPhysicalDeviceProperties(physicalDevice, &selectedProperties);
		if (selectedProperties.vendorID != kAppleVendorId ||
		    std::strcmp(selectedProperties.deviceName, kAppleM1DeviceName) != 0)
			return Fail("Native Metal probe device is not the unique Apple M1 adapter");
		if (selectedProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) reportedType = "integrated";
		else if (selectedProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) reportedType = "discrete";
		else return Fail("Native Metal Apple M1 device type is unsupported");
	}
	else
	{
		uint32_t exactCount = 0;
		for (VkPhysicalDevice candidate: physicalDevices)
		{
			VkPhysicalDeviceProperties properties {};
			vkGetPhysicalDeviceProperties(candidate, &properties);
			if (properties.vendorID == kAmdVendorId && properties.deviceID == kRequiredDeviceId)
			{
				++exactCount;
				physicalDevice = candidate;
				selectedProperties = properties;
			}
		}
		if (exactCount != 1) return Fail(exactCount == 0 ? "Required AMD device was not found" : "Required AMD device is ambiguous");
		if (selectedProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
			return Fail("Required AMD device is not reported as an integrated GPU");
	}
	if (VK_API_VERSION_MAJOR(selectedProperties.apiVersion) < 1 ||
	    (VK_API_VERSION_MAJOR(selectedProperties.apiVersion) == 1 && VK_API_VERSION_MINOR(selectedProperties.apiVersion) < 2))
		return Fail("Selected device does not expose Vulkan 1.2");

	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
	uint32_t queueFamily = UINT32_MAX;
	for (uint32_t index = 0; index < familyCount; ++index)
	{
		if (families[index].queueCount != 0 && (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
		{
			queueFamily = index;
			break;
		}
	}
	if (queueFamily == UINT32_MAX) return Fail("Selected device has no compute queue");

	constexpr float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &state.device);
	if (result != VK_SUCCESS) return Fail("vkCreateDevice failed", result);
	vkGetDeviceQueue(state.device, queueFamily, 0, &state.queue);
	if (state.queue == VK_NULL_HANDLE) return Fail("vkGetDeviceQueue returned null");

	const std::vector<uint32_t> initial = MakeInitialWords(cases);
	const uint32_t runtimeCaseCount = static_cast<uint32_t>(cases.size());
	const VkDeviceSize bufferSize = initial.size() * sizeof(uint32_t);
	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = bufferSize;
	bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(state.device, &bufferInfo, nullptr, &state.buffer);
	if (result != VK_SUCCESS) return Fail("vkCreateBuffer failed", result);
	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(state.device, state.buffer, &requirements);
	const uint32_t memoryType = FindMemoryType(physicalDevice, requirements.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX) return Fail("No coherent host-visible memory type");
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(state.device, &allocationInfo, nullptr, &state.memory);
	if (result != VK_SUCCESS) return Fail("vkAllocateMemory failed", result);
	result = vkBindBufferMemory(state.device, state.buffer, state.memory, 0);
	if (result != VK_SUCCESS) return Fail("vkBindBufferMemory failed", result);
	result = vkMapMemory(state.device, state.memory, 0, VK_WHOLE_SIZE, 0, &state.mapped);
	if (result != VK_SUCCESS || state.mapped == nullptr) return Fail("vkMapMemory failed", result);

	VkDescriptorSetLayoutBinding binding {};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo layoutInfo {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = &binding;
	result = vkCreateDescriptorSetLayout(state.device, &layoutInfo, nullptr, &state.descriptorSetLayout);
	if (result != VK_SUCCESS) return Fail("vkCreateDescriptorSetLayout failed", result);
	VkPipelineLayoutCreateInfo pipelineLayoutInfo {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &state.descriptorSetLayout;
	result = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS) return Fail("vkCreatePipelineLayout failed", result);

	VkDescriptorPoolSize poolSize {};
	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = 1;
	VkDescriptorPoolCreateInfo poolInfo {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	result = vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS) return Fail("vkCreateDescriptorPool failed", result);
	VkDescriptorSetAllocateInfo setInfo {};
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool = state.descriptorPool;
	setInfo.descriptorSetCount = 1;
	setInfo.pSetLayouts = &state.descriptorSetLayout;
	result = vkAllocateDescriptorSets(state.device, &setInfo, &state.descriptorSet);
	if (result != VK_SUCCESS) return Fail("vkAllocateDescriptorSets failed", result);
	VkDescriptorBufferInfo descriptorBuffer {state.buffer, 0, bufferSize};
	VkWriteDescriptorSet descriptorWrite {};
	descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet = state.descriptorSet;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.pBufferInfo = &descriptorBuffer;
	vkUpdateDescriptorSets(state.device, 1, &descriptorWrite, 0, nullptr);

	VkShaderModuleCreateInfo shaderInfo {};
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = shader.binary.size() * sizeof(uint32_t);
	shaderInfo.pCode = shader.binary.data();
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.shaderModule);
	if (result != VK_SUCCESS) return Fail("vkCreateShaderModule failed", result);
	VkPipelineShaderStageCreateInfo stageInfo {};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = state.shaderModule;
	stageInfo.pName = "main";
	VkComputePipelineCreateInfo pipelineInfo {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = state.pipelineLayout;
	result = vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.pipeline);
	if (result != VK_SUCCESS) return Fail("vkCreateComputePipelines failed", result);

	VkCommandPoolCreateInfo commandPoolInfo {};
	commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	commandPoolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(state.device, &commandPoolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS) return Fail("vkCreateCommandPool failed", result);
	VkCommandBufferAllocateInfo commandBufferInfo {};
	commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferInfo.commandPool = state.commandPool;
	commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferInfo.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(state.device, &commandBufferInfo, &state.commandBuffer);
	if (result != VK_SUCCESS) return Fail("vkAllocateCommandBuffers failed", result);
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(state.device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS) return Fail("vkCreateFence failed", result);

	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	result = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS) return Fail("vkBeginCommandBuffer failed", result);
	VkBufferMemoryBarrier hostToShader {};
	hostToShader.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	hostToShader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	hostToShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	hostToShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostToShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostToShader.buffer = state.buffer;
	hostToShader.size = bufferSize;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     0, 0, nullptr, 1, &hostToShader, 0, nullptr);
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout,
	                        0, 1, &state.descriptorSet, 0, nullptr);
	vkCmdDispatch(state.commandBuffer, runtimeCaseCount, 1, 1);
	VkBufferMemoryBarrier shaderToHost {};
	shaderToHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	shaderToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	shaderToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	shaderToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.buffer = state.buffer;
	shaderToHost.size = bufferSize;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 0, nullptr, 1, &shaderToHost, 0, nullptr);
	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS) return Fail("vkEndCommandBuffer failed", result);

	VkMappedMemoryRange mappedRange {};
	mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	mappedRange.memory = state.memory;
	mappedRange.size = VK_WHOLE_SIZE;
	size_t submission = 0;
	auto submitAndRead = [&](const std::vector<uint32_t>& inputWords, std::vector<uint32_t>* observed)
	{
		std::memcpy(state.mapped, inputWords.data(), static_cast<size_t>(bufferSize));
		result = vkFlushMappedMemoryRanges(state.device, 1, &mappedRange);
		if (result != VK_SUCCESS) return false;
		if (submission != 0)
		{
			result = vkResetFences(state.device, 1, &state.fence);
			if (result != VK_SUCCESS) return false;
		}
		VkSubmitInfo submitInfo {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &state.commandBuffer;
		result = vkQueueSubmit(state.queue, 1, &submitInfo, state.fence);
		if (result != VK_SUCCESS) return false;
		result = vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, kCompletionTimeoutNanoseconds);
		if (result != VK_SUCCESS) return false;
		result = vkInvalidateMappedMemoryRanges(state.device, 1, &mappedRange);
		if (result != VK_SUCCESS) return false;
		observed->resize(inputWords.size());
		std::memcpy(observed->data(), state.mapped, static_cast<size_t>(bufferSize));
		++submission;
		return true;
	};

	std::vector<std::vector<uint32_t>> outputs(kDispatchCount, std::vector<uint32_t>(cases.size()));
	std::vector<uint32_t> observed(initial.size());
	for (size_t dispatch = 0; dispatch < kDispatchCount; ++dispatch)
	{
		if (!submitAndRead(initial, &observed)) return Fail("V_CVT_OFF_F32_I4 GPU submission failed", result);
		if (!InputsAndCanariesUnchanged(initial, observed, cases.size(), dispatch)) return 1;
		for (size_t index = 0; index < cases.size(); ++index)
		{
			const uint32_t value = observed[kPrefixWords + index * kWordsPerCase + kOutputOffset];
			if (value == kOutputUnset || value != cases[index].expected)
			{
				std::fprintf(stderr, "V_CVT_OFF_F32_I4 mismatch: dispatch=%zu case=%s input=%08x output=%08x expected=%08x\n",
				             dispatch, cases[index].name.c_str(), cases[index].input, value, cases[index].expected);
				return 1;
			}
			outputs[dispatch][index] = value;
		}
	}
	for (size_t dispatch = 1; dispatch < kDispatchCount; ++dispatch)
		if (outputs[dispatch] != outputs[0]) return Fail("V_CVT_OFF_F32_I4 probe is nondeterministic");

	if (nativeMetal)
	{
		std::printf("[GFX_VCVT_OFF_F32_I4_VULKAN_PROBE] mode=runtime scope=diagnostic-production-core "
		            "core=shared-signed-convert-scale backend=native-metal adapter=static host=apple-m1 "
		            "vendor=0x%04x reported-type=%s translation=black-box domain=all-known-signed4 "
		            "nibble-extraction=none result=bit-exact expected=all dispatches=%zu "
		            "inputs=unchanged canaries=unchanged deterministic=yes cases=%zu status=clean\n",
		            selectedProperties.vendorID, reportedType, kDispatchCount, cases.size());
	}
	else
	{
		std::printf("[GFX_VCVT_OFF_F32_I4_VULKAN_PROBE] mode=runtime scope=diagnostic-production-core "
		            "core=shared-signed-convert-scale backend=vulkan vendor=0x%04x device=0x%04x "
		            "domain=all-known-signed4 nibble-extraction=none result=bit-exact "
		            "expected=all dispatches=%zu inputs=unchanged canaries=unchanged deterministic=yes "
		            "cases=%zu status=clean\n", selectedProperties.vendorID, selectedProperties.deviceID,
		            kDispatchCount, cases.size());
	}
	return std::fflush(stdout) == 0 ? 0 : 1;
}
