#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
constexpr uint64_t kCompletionTimeoutNanoseconds = 5'000'000'000;
constexpr uint32_t kQuietNanBits = 0x7fc12345u;
constexpr uint32_t kFiniteInputBits = 0x40400000u; // 3.0f
constexpr uint32_t kFiniteMultiplyBits = 0x40c00000u; // 6.0f
constexpr uint32_t kFiniteDivideBits = 0x3fc00000u; // 1.5f
constexpr std::array<uint32_t, 6> kInitialWords = {
	kQuietNanBits,
	kFiniteInputBits,
	0x3f000001u,
	0x3f000002u,
	0x3f000003u,
	0x3f000004u,
};

// Original, fixed SPIR-V 1.3 module. One invocation loads the NaN and finite
// control from a storage buffer, then stores nan*0, 0/nan, finite*2 and
// finite/2. Keeping the inputs runtime-visible prevents this backend
// arithmetic gate from being satisfied by shader-side constant evaluation.
constexpr uint32_t kComputeShader[] = {
	0x07230203u, 0x00010300u, 0x00000000u, 0x00000020u, 0x00000000u,
	0x00020011u, 0x00000001u,
	0x0003000eu, 0x00000000u, 0x00000001u,
	0x0005000fu, 0x00000005u, 0x00000012u, 0x6e69616du, 0x00000000u,
	0x00060010u, 0x00000012u, 0x00000011u, 0x00000001u, 0x00000001u, 0x00000001u,
	0x00040047u, 0x0000000du, 0x00000006u, 0x00000004u,
	0x00050048u, 0x0000000eu, 0x00000000u, 0x00000023u, 0x00000000u,
	0x00030047u, 0x0000000eu, 0x00000002u,
	0x00040047u, 0x00000010u, 0x00000022u, 0x00000000u,
	0x00040047u, 0x00000010u, 0x00000021u, 0x00000000u,
	0x00020013u, 0x00000001u,
	0x00030021u, 0x00000002u, 0x00000001u,
	0x00030016u, 0x00000003u, 0x00000020u,
	0x00040015u, 0x00000004u, 0x00000020u, 0x00000000u,
	0x0004002bu, 0x00000004u, 0x00000005u, 0x00000000u,
	0x0004002bu, 0x00000004u, 0x00000006u, 0x00000001u,
	0x0004002bu, 0x00000004u, 0x00000007u, 0x00000002u,
	0x0004002bu, 0x00000004u, 0x00000008u, 0x00000003u,
	0x0004002bu, 0x00000004u, 0x00000009u, 0x00000004u,
	0x0004002bu, 0x00000004u, 0x0000000au, 0x00000005u,
	0x0004002bu, 0x00000003u, 0x0000000bu, 0x00000000u,
	0x0004002bu, 0x00000003u, 0x0000000cu, 0x40000000u,
	0x0003001du, 0x0000000du, 0x00000003u,
	0x0003001eu, 0x0000000eu, 0x0000000du,
	0x00040020u, 0x0000000fu, 0x0000000cu, 0x0000000eu,
	0x0004003bu, 0x0000000fu, 0x00000010u, 0x0000000cu,
	0x00040020u, 0x00000011u, 0x0000000cu, 0x00000003u,
	0x00050036u, 0x00000001u, 0x00000012u, 0x00000000u, 0x00000002u,
	0x000200f8u, 0x00000013u,
	0x00060041u, 0x00000011u, 0x00000014u, 0x00000010u, 0x00000005u, 0x00000005u,
	0x0004003du, 0x00000003u, 0x00000015u, 0x00000014u,
	0x00060041u, 0x00000011u, 0x00000016u, 0x00000010u, 0x00000005u, 0x00000006u,
	0x0004003du, 0x00000003u, 0x00000017u, 0x00000016u,
	0x00050085u, 0x00000003u, 0x00000018u, 0x00000015u, 0x0000000bu,
	0x00050088u, 0x00000003u, 0x00000019u, 0x0000000bu, 0x00000015u,
	0x00050085u, 0x00000003u, 0x0000001au, 0x00000017u, 0x0000000cu,
	0x00050088u, 0x00000003u, 0x0000001bu, 0x00000017u, 0x0000000cu,
	0x00060041u, 0x00000011u, 0x0000001cu, 0x00000010u, 0x00000005u, 0x00000007u,
	0x0003003eu, 0x0000001cu, 0x00000018u,
	0x00060041u, 0x00000011u, 0x0000001du, 0x00000010u, 0x00000005u, 0x00000008u,
	0x0003003eu, 0x0000001du, 0x00000019u,
	0x00060041u, 0x00000011u, 0x0000001eu, 0x00000010u, 0x00000005u, 0x00000009u,
	0x0003003eu, 0x0000001eu, 0x0000001au,
	0x00060041u, 0x00000011u, 0x0000001fu, 0x00000010u, 0x00000005u, 0x0000000au,
	0x0003003eu, 0x0000001fu, 0x0000001bu,
	0x000100fdu,
	0x00010038u,
};

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

bool IsNanBits(uint32_t value)
{
	return (value & 0x7f800000u) == 0x7f800000u && (value & 0x007fffffu) != 0;
}
}

int main()
{
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	if (!validator.Validate(kComputeShader, sizeof(kComputeShader) / sizeof(kComputeShader[0])))
	{
		return Fail("Fixed DX10 clamp backend SPIR-V did not validate");
	}

	ProbeState state;
	VkApplicationInfo applicationInfo{};
	applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName = "Lumen DX10 clamp NaN compute probe";
	applicationInfo.applicationVersion = 1;
	applicationInfo.pEngineName = "Lumen";
	applicationInfo.engineVersion = 1;
	applicationInfo.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo instanceInfo{};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &applicationInfo;
	VkResult result = vkCreateInstance(&instanceInfo, nullptr, &state.instance);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateInstance failed", result);
	}

	uint32_t physicalDeviceCount = 0;
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
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
	uint32_t queueFamily = UINT32_MAX;
	for (VkPhysicalDevice candidate : physicalDevices)
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
				queueFamily = family;
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
	VkDeviceQueueCreateInfo queueInfo{};
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	VkDeviceCreateInfo deviceInfo{};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
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

	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = sizeof(kInitialWords);
	bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(state.device, &bufferInfo, nullptr, &state.buffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateBuffer(storage) failed", result);
	}
	VkMemoryRequirements memoryRequirements{};
	vkGetBufferMemoryRequirements(state.device, state.buffer, &memoryRequirements);
	const uint32_t memoryType = FindMemoryType(
	    physicalDevice, memoryRequirements.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX)
	{
		return Fail("No coherent host-visible memory type for storage buffer");
	}
	VkMemoryAllocateInfo allocationInfo{};
	allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize = memoryRequirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(state.device, &allocationInfo, nullptr, &state.memory);
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
	std::memcpy(state.mapped, kInitialWords.data(), sizeof(kInitialWords));
	VkMappedMemoryRange mappedRange{};
	mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	mappedRange.memory = state.memory;
	mappedRange.size = VK_WHOLE_SIZE;
	result = vkFlushMappedMemoryRanges(state.device, 1, &mappedRange);
	if (result != VK_SUCCESS)
	{
		return Fail("vkFlushMappedMemoryRanges(storage) failed", result);
	}

	VkDescriptorSetLayoutBinding descriptorBinding{};
	descriptorBinding.binding = 0;
	descriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorBinding.descriptorCount = 1;
	descriptorBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
	descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptorLayoutInfo.bindingCount = 1;
	descriptorLayoutInfo.pBindings = &descriptorBinding;
	result = vkCreateDescriptorSetLayout(state.device, &descriptorLayoutInfo, nullptr, &state.descriptorSetLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorSetLayout(storage) failed", result);
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &state.descriptorSetLayout;
	result = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreatePipelineLayout(compute) failed", result);
	}

	VkDescriptorPoolSize poolSize{};
	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = 1;
	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	result = vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorPool(storage) failed", result);
	}
	VkDescriptorSetAllocateInfo descriptorSetInfo{};
	descriptorSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetInfo.descriptorPool = state.descriptorPool;
	descriptorSetInfo.descriptorSetCount = 1;
	descriptorSetInfo.pSetLayouts = &state.descriptorSetLayout;
	result = vkAllocateDescriptorSets(state.device, &descriptorSetInfo, &state.descriptorSet);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateDescriptorSets(storage) failed", result);
	}
	VkDescriptorBufferInfo descriptorBuffer{};
	descriptorBuffer.buffer = state.buffer;
	descriptorBuffer.range = sizeof(kInitialWords);
	VkWriteDescriptorSet descriptorWrite{};
	descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet = state.descriptorSet;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.pBufferInfo = &descriptorBuffer;
	vkUpdateDescriptorSets(state.device, 1, &descriptorWrite, 0, nullptr);

	VkShaderModuleCreateInfo shaderInfo{};
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = sizeof(kComputeShader);
	shaderInfo.pCode = kComputeShader;
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.shaderModule);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateShaderModule(compute) failed", result);
	}
	VkPipelineShaderStageCreateInfo stageInfo{};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = state.shaderModule;
	stageInfo.pName = "main";
	VkComputePipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = state.pipelineLayout;
	result = vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.pipeline);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateComputePipelines failed", result);
	}

	VkCommandPoolCreateInfo commandPoolInfo{};
	commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	commandPoolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(state.device, &commandPoolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateCommandPool failed", result);
	}
	VkCommandBufferAllocateInfo commandBufferInfo{};
	commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferInfo.commandPool = state.commandPool;
	commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferInfo.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(state.device, &commandBufferInfo, &state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateCommandBuffers failed", result);
	}
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(state.device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateFence failed", result);
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
	{
		return Fail("vkBeginCommandBuffer failed", result);
	}
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout, 0, 1,
	                        &state.descriptorSet, 0, nullptr);
	vkCmdDispatch(state.commandBuffer, 1, 1, 1);
	VkBufferMemoryBarrier shaderToHost{};
	shaderToHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	shaderToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	shaderToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	shaderToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.buffer = state.buffer;
	shaderToHost.size = sizeof(kInitialWords);
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
	                     nullptr, 1, &shaderToHost, 0, nullptr);
	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkEndCommandBuffer failed", result);
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &state.commandBuffer;
	result = vkQueueSubmit(state.queue, 1, &submitInfo, state.fence);
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

	std::array<uint32_t, kInitialWords.size()> output{};
	std::memcpy(output.data(), state.mapped, sizeof(output));
	if (output[0] != kQuietNanBits || output[1] != kFiniteInputBits || !IsNanBits(output[2]) ||
	    !IsNanBits(output[3]) || output[4] != kFiniteMultiplyBits || output[5] != kFiniteDivideBits)
	{
		std::fprintf(stderr,
		             "DX10 clamp backend arithmetic mismatch: input_nan=%08x input_finite=%08x mul=%08x div=%08x "
		             "finite_mul=%08x finite_div=%08x\n",
		             output[0], output[1], output[2], output[3], output[4], output[5]);
		return 1;
	}

	const int markerResult = std::printf(
	    "[GFX_DX10_CLAMP_NAN_COMPUTE] backend=native-metal scope=backend-arithmetic-only "
	    "mul_nan=1 div_nan=1 finite_mul=6 finite_div=1.5 status=clean\n");
	if (markerResult <= 0 || std::fflush(stdout) != 0)
	{
		return Fail("Failed to emit DX10 clamp backend marker");
	}
	return 0;
}
