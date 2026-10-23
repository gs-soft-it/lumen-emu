#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr uint64_t kCompletionTimeoutNanoseconds = 5'000'000'000;
constexpr uint32_t kUnsetOutput                  = 0xa5a5a5a5u;

struct ProbeCase
{
	uint32_t lhs;
	uint32_t rhs;
	uint32_t output;
};

static_assert(sizeof(ProbeCase) == 3 * sizeof(uint32_t));

// These operands are uploaded at runtime so the backend must execute UMin.
// The signed-boundary and max/low pairs distinguish UMin from SMin; reversed
// pairs verify operand symmetry, while equal pairs cover both ends of the
// unsigned domain.
constexpr std::array<ProbeCase, 12> kInitialCases = {{
    {0x00000000u, 0x00000000u, kUnsetOutput},
    {0xffffffffu, 0xffffffffu, kUnsetOutput},
    {0x00000000u, 0xffffffffu, kUnsetOutput},
    {0xffffffffu, 0x00000000u, kUnsetOutput},
    {0x00000001u, 0xffffffffu, kUnsetOutput},
    {0xffffffffu, 0x00000001u, kUnsetOutput},
    {0x7fffffffu, 0x80000000u, kUnsetOutput},
    {0x80000000u, 0x7fffffffu, kUnsetOutput},
    {0x80000000u, 0xffffffffu, kUnsetOutput},
    {0xffffffffu, 0x80000000u, kUnsetOutput},
    {0x80000000u, 0x80000000u, kUnsetOutput},
    {0x7fffffffu, 0x7fffffffu, kUnsetOutput},
}};

constexpr std::array<uint32_t, kInitialCases.size()> kExpectedOutputs = {
    0x00000000u,
    0xffffffffu,
    0x00000000u,
    0x00000000u,
    0x00000001u,
    0x00000001u,
    0x7fffffffu,
    0x7fffffffu,
    0x80000000u,
    0x80000000u,
    0x80000000u,
    0x7fffffffu,
};

constexpr std::array<const char*, kInitialCases.size()> kCaseNames = {
    "equal-zero",
    "equal-max",
    "zero-max",
    "zero-max-swapped",
    "one-max",
    "one-max-swapped",
    "signed-boundary",
    "signed-boundary-swapped",
    "upper-half-max",
    "upper-half-max-swapped",
    "equal-upper-half",
    "equal-signed-max",
};

constexpr char kComputeShaderAssembly[] = R"spirv(
               OpCapability Shader
       %glsl = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %global_id
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %global_id BuiltIn GlobalInvocationId
               OpDecorate %words ArrayStride 4
               OpMemberDecorate %buffer_block 0 Offset 0
               OpDecorate %buffer_block BufferBlock
               OpDecorate %storage DescriptorSet 0
               OpDecorate %storage Binding 0

       %void = OpTypeVoid
   %function = OpTypeFunction %void
       %uint = OpTypeInt 32 0
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
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3

       %main = OpFunction %void None %function
      %entry = OpLabel
         %gid = OpLoad %v3uint %global_id
  %case_index = OpCompositeExtract %uint %gid 0
        %base = OpIMul %uint %case_index %uint_3
   %rhs_index = OpIAdd %uint %base %uint_1
%output_index = OpIAdd %uint %base %uint_2
     %lhs_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %base
     %rhs_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %rhs_index
     %lhs_u32 = OpLoad %uint %lhs_ptr
     %rhs_u32 = OpLoad %uint %rhs_ptr
     %minimum = OpExtInst %uint %glsl UMin %lhs_u32 %rhs_u32
  %output_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %output_index
               OpStore %output_ptr %minimum
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
	VkDescriptorSet       descriptorSet       = VK_NULL_HANDLE;
	VkPipelineLayout      pipelineLayout      = VK_NULL_HANDLE;
	VkShaderModule        shaderModule        = VK_NULL_HANDLE;
	VkPipeline            pipeline            = VK_NULL_HANDLE;
	VkBuffer              buffer              = VK_NULL_HANDLE;
	VkDeviceMemory        memory              = VK_NULL_HANDLE;
	void*                 mapped              = nullptr;
	VkCommandPool         commandPool         = VK_NULL_HANDLE;
	VkCommandBuffer       commandBuffer       = VK_NULL_HANDLE;
	VkFence               fence               = VK_NULL_HANDLE;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(device);
		}
		if (mapped != nullptr)
		{
			vkUnmapMemory(device, memory);
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
		if (buffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, buffer, nullptr);
		}
		if (memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, memory, nullptr);
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

bool HasUnsignedMinimumShape(std::string_view disassembly)
{
	return disassembly.find("OpExtInstImport \"GLSL.std.450\"") != std::string_view::npos &&
	       CountToken(disassembly, " UMin ") == 1 && CountToken(disassembly, "OpLoad %uint") == 2 &&
	       CountToken(disassembly, "OpStore") == 1 && disassembly.find(" SMin ") == std::string_view::npos &&
	       disassembly.find(" FMin ") == std::string_view::npos;
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
		return Fail("V_MIN_U32 backend SPIR-V assembly failed");
	}
	if (!spirvTools.Validate(shaderBinary))
	{
		return Fail("V_MIN_U32 backend SPIR-V validation failed");
	}
	std::string disassembly;
	if (!spirvTools.Disassemble(shaderBinary, &disassembly) || !HasUnsignedMinimumShape(disassembly))
	{
		return Fail("V_MIN_U32 backend SPIR-V lost its single unsigned-minimum shape");
	}

	ProbeState state;
	VkApplicationInfo applicationInfo {};
	applicationInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName   = "Lumen V_MIN_U32 compute probe";
	applicationInfo.applicationVersion = 1;
	applicationInfo.pEngineName        = "Lumen";
	applicationInfo.engineVersion      = 1;
	applicationInfo.apiVersion         = VK_API_VERSION_1_2;
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

	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size        = sizeof(kInitialCases);
	bufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result                 = vkCreateBuffer(state.device, &bufferInfo, nullptr, &state.buffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateBuffer(storage) failed", result);
	}
	VkMemoryRequirements memoryRequirements {};
	vkGetBufferMemoryRequirements(state.device, state.buffer, &memoryRequirements);
	const uint32_t memoryType = FindMemoryType(physicalDevice, memoryRequirements.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX)
	{
		return Fail("No coherent host-visible memory type for storage buffer");
	}
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = memoryRequirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result                         = vkAllocateMemory(state.device, &allocationInfo, nullptr, &state.memory);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateMemory(storage) failed", result);
	}
	result = vkBindBufferMemory(state.device, state.buffer, state.memory, 0);
	if (result != VK_SUCCESS)
	{
		return Fail("vkBindBufferMemory(storage) failed", result);
	}
	result = vkMapMemory(state.device, state.memory, 0, VK_WHOLE_SIZE, 0, &state.mapped);
	if (result != VK_SUCCESS || state.mapped == nullptr)
	{
		return Fail("vkMapMemory(storage) failed", result);
	}
	std::memcpy(state.mapped, kInitialCases.data(), sizeof(kInitialCases));
	VkMappedMemoryRange mappedRange {};
	mappedRange.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	mappedRange.memory = state.memory;
	mappedRange.size   = VK_WHOLE_SIZE;
	result             = vkFlushMappedMemoryRanges(state.device, 1, &mappedRange);
	if (result != VK_SUCCESS)
	{
		return Fail("vkFlushMappedMemoryRanges(storage) failed", result);
	}

	VkDescriptorSetLayoutBinding descriptorBinding {};
	descriptorBinding.binding         = 0;
	descriptorBinding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorBinding.descriptorCount = 1;
	descriptorBinding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo {};
	descriptorLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptorLayoutInfo.bindingCount = 1;
	descriptorLayoutInfo.pBindings    = &descriptorBinding;
	result = vkCreateDescriptorSetLayout(state.device, &descriptorLayoutInfo, nullptr, &state.descriptorSetLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorSetLayout(storage) failed", result);
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo {};
	pipelineLayoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts    = &state.descriptorSetLayout;
	result                            = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreatePipelineLayout(compute) failed", result);
	}

	VkDescriptorPoolSize poolSize {};
	poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = 1;
	VkDescriptorPoolCreateInfo poolInfo {};
	poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets       = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes    = &poolSize;
	result                 = vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorPool(storage) failed", result);
	}
	VkDescriptorSetAllocateInfo descriptorSetInfo {};
	descriptorSetInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetInfo.descriptorPool     = state.descriptorPool;
	descriptorSetInfo.descriptorSetCount = 1;
	descriptorSetInfo.pSetLayouts        = &state.descriptorSetLayout;
	result                               = vkAllocateDescriptorSets(state.device, &descriptorSetInfo, &state.descriptorSet);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateDescriptorSets(storage) failed", result);
	}
	VkDescriptorBufferInfo descriptorBuffer {};
	descriptorBuffer.buffer = state.buffer;
	descriptorBuffer.range  = sizeof(kInitialCases);
	VkWriteDescriptorSet descriptorWrite {};
	descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet          = state.descriptorSet;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.pBufferInfo     = &descriptorBuffer;
	vkUpdateDescriptorSets(state.device, 1, &descriptorWrite, 0, nullptr);

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
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout, 0, 1, &state.descriptorSet, 0,
	                        nullptr);
	vkCmdDispatch(state.commandBuffer, static_cast<uint32_t>(kInitialCases.size()), 1, 1);
	VkBufferMemoryBarrier shaderToHost {};
	shaderToHost.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	shaderToHost.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
	shaderToHost.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	shaderToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.buffer              = state.buffer;
	shaderToHost.size                = sizeof(kInitialCases);
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
	                     &shaderToHost, 0, nullptr);
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
	result = vkInvalidateMappedMemoryRanges(state.device, 1, &mappedRange);
	if (result != VK_SUCCESS)
	{
		return Fail("vkInvalidateMappedMemoryRanges(storage) failed", result);
	}

	std::array<ProbeCase, kInitialCases.size()> observed {};
	std::memcpy(observed.data(), state.mapped, sizeof(observed));
	bool allCasesPassed = true;
	for (size_t index = 0; index < observed.size(); ++index)
	{
		if (observed[index].lhs != kInitialCases[index].lhs || observed[index].rhs != kInitialCases[index].rhs ||
		    observed[index].output != kExpectedOutputs[index])
		{
			std::fprintf(stderr,
			             "V_MIN_U32 backend mismatch: case=%s index=%zu lhs=%08x/%08x rhs=%08x/%08x "
			             "output=%08x/%08x\n",
			             kCaseNames[index], index, observed[index].lhs, kInitialCases[index].lhs, observed[index].rhs,
			             kInitialCases[index].rhs, observed[index].output, kExpectedOutputs[index]);
			allCasesPassed = false;
		}
	}
	if (!allCasesPassed)
	{
		return 1;
	}

	const int markerResult =
	    std::printf("[GFX_VMIN_U32_COMPUTE] backend=native-metal scope=backend-arithmetic-only operation=umin "
	                "input=runtime-storage spirv-validation=clean signed-boundary=verified swapped=verified "
	                "equal=verified zero-max=verified cases=%zu status=clean\n",
	                kInitialCases.size());
	if (markerResult <= 0 || std::fflush(stdout) != 0)
	{
		return Fail("Failed to emit V_MIN_U32 backend marker");
	}
	return 0;
}
