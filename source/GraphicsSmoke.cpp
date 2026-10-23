#include "SDL.h"
#include "SDL_vulkan.h"
#include "Emulator/Graphics/NullSampledImageContract.h"
#include "vulkan/vulkan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
#define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME "VK_KHR_portability_enumeration"
#endif
#ifndef VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001
#endif

namespace
{
constexpr uint64_t kAcquireTimeoutNanoseconds = 100'000'000;
constexpr uint64_t kCompletionTimeoutNanoseconds = 5'000'000'000;
constexpr uint32_t kLineReadbackWidth = 8;
constexpr uint32_t kLineReadbackHeight = 8;
constexpr uint32_t kLineReadbackPitchTexels = 64;
constexpr VkDeviceSize kLineReadbackBytes =
	static_cast<VkDeviceSize>(kLineReadbackPitchTexels) * kLineReadbackHeight * 4;

// Fixed SPIR-V modules keep this gate independent of an SDK shader compiler.
// The vertex shader uses set 0/binding 0 as a storage-buffer position offset;
// the fragment shader really samples a combined image sampler at set 1/binding
// 0. This mirrors the renderer's separate VS-then-PS descriptor-set binds.
constexpr std::array<uint32_t, 192> kVertexShader = {
	0x07230203u, 0x00010300u, 0x00070000u, 0x00000024u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
	0x00000000u, 0x00000001u, 0x0007000fu, 0x00000000u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00000002u,
	0x00000003u, 0x00040047u, 0x00000002u, 0x0000000bu, 0x00000000u, 0x00040047u, 0x00000003u, 0x0000000bu,
	0x0000002au, 0x00050048u, 0x0000001cu, 0x00000000u, 0x00000023u, 0x00000000u, 0x00030047u, 0x0000001cu,
	0x00000002u, 0x00040047u, 0x0000001eu, 0x00000022u, 0x00000000u, 0x00040047u, 0x0000001eu, 0x00000021u,
	0x00000000u, 0x00020013u, 0x00000004u, 0x00030021u, 0x00000005u, 0x00000004u, 0x00030016u, 0x00000006u,
	0x00000020u, 0x00040015u, 0x00000007u, 0x00000020u, 0x00000001u, 0x00040017u, 0x00000008u, 0x00000006u,
	0x00000004u, 0x00040020u, 0x00000009u, 0x00000003u, 0x00000008u, 0x00040020u, 0x0000000au, 0x00000001u,
	0x00000007u, 0x0004002bu, 0x00000007u, 0x0000000bu, 0x00000001u, 0x0004002bu, 0x00000007u, 0x0000000cu,
	0x00000002u, 0x0004002bu, 0x00000006u, 0x0000000du, 0x00000000u, 0x0004002bu, 0x00000006u, 0x0000000eu,
	0x3f800000u, 0x0004002bu, 0x00000006u, 0x0000000fu, 0x40000000u, 0x0003001eu, 0x0000001cu, 0x00000008u,
	0x00040020u, 0x0000001du, 0x0000000cu, 0x0000001cu, 0x00040020u, 0x0000001fu, 0x0000000cu, 0x00000008u,
	0x0004002bu, 0x00000007u, 0x00000020u, 0x00000000u, 0x0004003bu, 0x00000009u, 0x00000002u, 0x00000003u,
	0x0004003bu, 0x0000000au, 0x00000003u, 0x00000001u, 0x0004003bu, 0x0000001du, 0x0000001eu, 0x0000000cu,
	0x00050036u, 0x00000004u, 0x00000001u, 0x00000000u, 0x00000005u, 0x000200f8u, 0x00000010u, 0x0004003du,
	0x00000007u, 0x00000011u, 0x00000003u, 0x000500c4u, 0x00000007u, 0x00000012u, 0x00000011u, 0x0000000bu,
	0x000500c7u, 0x00000007u, 0x00000013u, 0x00000012u, 0x0000000cu, 0x000500c7u, 0x00000007u, 0x00000014u,
	0x00000011u, 0x0000000cu, 0x0004006fu, 0x00000006u, 0x00000015u, 0x00000013u, 0x0004006fu, 0x00000006u,
	0x00000016u, 0x00000014u, 0x00050085u, 0x00000006u, 0x00000017u, 0x00000015u, 0x0000000fu, 0x00050085u,
	0x00000006u, 0x00000018u, 0x00000016u, 0x0000000fu, 0x00050083u, 0x00000006u, 0x00000019u, 0x00000017u,
	0x0000000eu, 0x00050083u, 0x00000006u, 0x0000001au, 0x00000018u, 0x0000000eu, 0x00070050u, 0x00000008u,
	0x0000001bu, 0x00000019u, 0x0000001au, 0x0000000du, 0x0000000eu, 0x00050041u, 0x0000001fu, 0x00000021u,
	0x0000001eu, 0x00000020u, 0x0004003du, 0x00000008u, 0x00000022u, 0x00000021u, 0x00050081u, 0x00000008u,
	0x00000023u, 0x0000001bu, 0x00000022u, 0x0003003eu, 0x00000002u, 0x00000023u, 0x000100fdu, 0x00010038u,
};

constexpr std::array<uint32_t, 105> kFragmentShader = {
	0x07230203u, 0x00010000u, 0x00070000u, 0x00000012u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu,
	0x00000000u, 0x00000001u, 0x0006000fu, 0x00000004u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00000002u,
	0x00030010u, 0x00000001u, 0x00000007u, 0x00040047u, 0x00000002u, 0x0000001eu, 0x00000000u, 0x00040047u,
	0x0000000cu, 0x00000022u, 0x00000001u, 0x00040047u, 0x0000000cu, 0x00000021u, 0x00000000u, 0x00020013u,
	0x00000003u, 0x00030021u, 0x00000004u, 0x00000003u, 0x00030016u, 0x00000005u, 0x00000020u, 0x00040017u,
	0x00000006u, 0x00000005u, 0x00000004u, 0x00040020u, 0x00000007u, 0x00000003u, 0x00000006u, 0x00040017u,
	0x00000008u, 0x00000005u, 0x00000002u, 0x00090019u, 0x00000009u, 0x00000005u, 0x00000001u, 0x00000000u,
	0x00000000u, 0x00000000u, 0x00000001u, 0x00000000u, 0x0003001bu, 0x0000000au, 0x00000009u, 0x00040020u,
	0x0000000bu, 0x00000000u, 0x0000000au, 0x0004003bu, 0x00000007u, 0x00000002u, 0x00000003u, 0x0004003bu,
	0x0000000bu, 0x0000000cu, 0x00000000u, 0x0004002bu, 0x00000005u, 0x0000000du, 0x3f000000u, 0x0005002cu,
	0x00000008u, 0x0000000eu, 0x0000000du, 0x0000000du, 0x00050036u, 0x00000003u, 0x00000001u, 0x00000000u,
	0x00000004u, 0x000200f8u, 0x0000000fu, 0x0004003du, 0x0000000au, 0x00000010u, 0x0000000cu, 0x00050057u,
	0x00000006u, 0x00000011u, 0x00000010u, 0x0000000eu, 0x0003003eu, 0x00000002u, 0x00000011u, 0x000100fdu,
	0x00010038u,
};

bool IsSuccess(VkResult result)
{
	return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
}

bool CanRetryAcquireAfterShow(VkResult result)
{
	return result == VK_NOT_READY || result == VK_TIMEOUT || result == VK_ERROR_OUT_OF_DATE_KHR;
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
{
	return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
		return std::strcmp(extension.extensionName, name) == 0;
	});
}

bool HasExtension(const std::vector<const char*>& extensions, const char* name)
{
	return std::any_of(extensions.begin(), extensions.end(), [name](const char* extension) {
		return extension != nullptr && std::strcmp(extension, name) == 0;
	});
}

struct SmokeState
{
	bool sdlInitialized = false;
	SDL_Window* window = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	std::vector<VkImageView> swapchainImageViews;
	std::vector<VkFramebuffer> framebuffers;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkImage offscreenImage = VK_NULL_HANDLE;
	VkDeviceMemory offscreenMemory = VK_NULL_HANDLE;
	VkImageView offscreenImageView = VK_NULL_HANDLE;
	VkFramebuffer offscreenFramebuffer = VK_NULL_HANDLE;
	VkDescriptorSetLayout vertexSetLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout fragmentSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet vertexDescriptorSet = VK_NULL_HANDLE;
	VkDescriptorSet fragmentDescriptorSet = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	VkShaderModule vertexShader = VK_NULL_HANDLE;
	VkShaderModule fragmentShader = VK_NULL_HANDLE;
	VkPipeline graphicsPipeline = VK_NULL_HANDLE;
	VkBuffer vertexStorageBuffer = VK_NULL_HANDLE;
	VkDeviceMemory vertexStorageMemory = VK_NULL_HANDLE;
	VkBuffer sampledStagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory sampledStagingMemory = VK_NULL_HANDLE;
	VkImage sampledImage = VK_NULL_HANDLE;
	VkDeviceMemory sampledMemory = VK_NULL_HANDLE;
	VkImageView sampledImageView = VK_NULL_HANDLE;
	VkSampler sampledSampler = VK_NULL_HANDLE;
	VkBuffer nullSampledReadbackBuffer = VK_NULL_HANDLE;
	VkDeviceMemory nullSampledReadbackMemory = VK_NULL_HANDLE;
	VkBuffer fanIndexStagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory fanIndexStagingMemory = VK_NULL_HANDLE;
	VkBuffer fanIndexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory fanIndexMemory = VK_NULL_HANDLE;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkSemaphore imageAvailable = VK_NULL_HANDLE;
	VkSemaphore renderFinished = VK_NULL_HANDLE;
	VkFence completionFence = VK_NULL_HANDLE;

	~SmokeState()
	{
		if (device != VK_NULL_HANDLE)
		{
			const VkResult idle = vkDeviceWaitIdle(device);
			if (idle != VK_SUCCESS)
			{
				std::fprintf(stderr, "Cleanup vkDeviceWaitIdle failed: %d\n", idle);
			}
		}
		if (completionFence != VK_NULL_HANDLE)
		{
			vkDestroyFence(device, completionFence, nullptr);
		}
		if (renderFinished != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(device, renderFinished, nullptr);
		}
		if (imageAvailable != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(device, imageAvailable, nullptr);
		}
		if (commandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(device, commandPool, nullptr);
		}
		if (fanIndexBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, fanIndexBuffer, nullptr);
		}
		if (fanIndexMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, fanIndexMemory, nullptr);
		}
		if (fanIndexStagingBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, fanIndexStagingBuffer, nullptr);
		}
		if (fanIndexStagingMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, fanIndexStagingMemory, nullptr);
		}
		if (graphicsPipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, graphicsPipeline, nullptr);
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
		if (descriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		}
		if (fragmentSetLayout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device, fragmentSetLayout, nullptr);
		}
		if (vertexSetLayout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device, vertexSetLayout, nullptr);
		}
		if (sampledSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(device, sampledSampler, nullptr);
		}
		if (sampledImageView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(device, sampledImageView, nullptr);
		}
		if (sampledImage != VK_NULL_HANDLE)
		{
			vkDestroyImage(device, sampledImage, nullptr);
		}
		if (sampledMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, sampledMemory, nullptr);
		}
		if (sampledStagingBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, sampledStagingBuffer, nullptr);
		}
		if (sampledStagingMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, sampledStagingMemory, nullptr);
		}
		if (nullSampledReadbackBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, nullSampledReadbackBuffer, nullptr);
		}
		if (nullSampledReadbackMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, nullSampledReadbackMemory, nullptr);
		}
		if (vertexStorageBuffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, vertexStorageBuffer, nullptr);
		}
		if (vertexStorageMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, vertexStorageMemory, nullptr);
		}
		for (VkFramebuffer framebuffer : framebuffers)
		{
			vkDestroyFramebuffer(device, framebuffer, nullptr);
		}
		if (offscreenFramebuffer != VK_NULL_HANDLE)
		{
			vkDestroyFramebuffer(device, offscreenFramebuffer, nullptr);
		}
		if (renderPass != VK_NULL_HANDLE)
		{
			vkDestroyRenderPass(device, renderPass, nullptr);
		}
		for (VkImageView imageView : swapchainImageViews)
		{
			vkDestroyImageView(device, imageView, nullptr);
		}
		if (offscreenImageView != VK_NULL_HANDLE)
		{
			vkDestroyImageView(device, offscreenImageView, nullptr);
		}
		if (offscreenImage != VK_NULL_HANDLE)
		{
			vkDestroyImage(device, offscreenImage, nullptr);
		}
		if (offscreenMemory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, offscreenMemory, nullptr);
		}
		if (swapchain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(device, swapchain, nullptr);
		}
		if (device != VK_NULL_HANDLE)
		{
			vkDestroyDevice(device, nullptr);
		}
		if (surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(instance, surface, nullptr);
		}
		if (instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(instance, nullptr);
		}
		if (window != nullptr)
		{
			SDL_DestroyWindow(window);
		}
		if (sdlInitialized)
		{
			SDL_Quit();
		}
	}
};

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
{
	constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> choices = {
	    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
	    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
	    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
	    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
	};
	for (VkCompositeAlphaFlagBitsKHR choice : choices)
	{
		if ((supported & choice) != 0)
		{
			return choice;
		}
	}
	return static_cast<VkCompositeAlphaFlagBitsKHR>(0);
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

VkResult CreateBufferWithMemory(VkPhysicalDevice physicalDevice, VkDevice device, VkDeviceSize size,
	                            VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer* buffer,
	                            VkDeviceMemory* memory)
{
	VkBufferCreateInfo bufferInfo{};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult result = vkCreateBuffer(device, &bufferInfo, nullptr, buffer);
	if (result != VK_SUCCESS)
	{
		return result;
	}
	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(device, *buffer, &requirements);
	const uint32_t memoryType = FindMemoryType(physicalDevice, requirements.memoryTypeBits, properties);
	if (memoryType == UINT32_MAX)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	VkMemoryAllocateInfo allocation{};
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(device, &allocation, nullptr, memory);
	if (result != VK_SUCCESS)
	{
		return result;
	}
	return vkBindBufferMemory(device, *buffer, *memory, 0);
}
}

int main(int argc, char** argv)
{
	constexpr const char* nullSampledImageArgument = "--null-sampled-image-zero";
	constexpr const char* lineListArgument = "--line-list-readback";
	constexpr const char* lineStripArgument = "--line-strip-readback";
	const bool nullSampledImageMode = argc == 2 && std::strcmp(argv[1], nullSampledImageArgument) == 0;
	const bool lineListMode = argc == 2 && std::strcmp(argv[1], lineListArgument) == 0;
	const bool lineStripMode = argc == 2 && std::strcmp(argv[1], lineStripArgument) == 0;
	const bool lineReadbackMode = lineListMode || lineStripMode;
	if (argc != 1 && !nullSampledImageMode && !lineReadbackMode)
	{
		std::fprintf(stderr, "Usage: %s [%s|%s|%s]\n", argv[0], nullSampledImageArgument, lineListArgument,
		             lineStripArgument);
		return 64;
	}
#if !defined(LUMEN_GRAPHICS_SMOKE_NATIVE_METAL)
	if (nullSampledImageMode || lineReadbackMode)
	{
		std::fprintf(stderr, "%s requires a macOS NativeMetal build\n",
		             nullSampledImageMode ? nullSampledImageArgument
		                                  : (lineListMode ? lineListArgument : lineStripArgument));
		return 1;
	}
#endif

	SmokeState state;
	auto fail = [](const char* operation, VkResult result = VK_SUCCESS) {
		if (result == VK_SUCCESS)
		{
			std::fprintf(stderr, "%s\n", operation);
		} else
		{
			std::fprintf(stderr, "%s: %d\n", operation, result);
		}
		return 1;
	};

	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	state.sdlInitialized = true;

	state.window = SDL_CreateWindow("Lumen graphics smoke test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 64, 64,
	                                SDL_WINDOW_HIDDEN | SDL_WINDOW_VULKAN);
	if (state.window == nullptr)
	{
		std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		return 1;
	}

	unsigned int extensionCount = 0;
	if (SDL_Vulkan_GetInstanceExtensions(state.window, &extensionCount, nullptr) == SDL_FALSE || extensionCount == 0)
	{
		std::fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
		return 1;
	}
	std::vector<const char*> extensions(extensionCount);
	if (SDL_Vulkan_GetInstanceExtensions(state.window, &extensionCount, extensions.data()) == SDL_FALSE)
	{
		std::fprintf(stderr, "SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
		return 1;
	}
	extensions.resize(extensionCount);

	uint32_t availableCount = 0;
	VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
	if (result != VK_SUCCESS)
	{
		return fail("vkEnumerateInstanceExtensionProperties(count) failed", result);
	}
	std::vector<VkExtensionProperties> available(availableCount);
	result = vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
	if (result != VK_SUCCESS)
	{
		return fail("vkEnumerateInstanceExtensionProperties(list) failed", result);
	}
	available.resize(availableCount);

	for (const char* required : extensions)
	{
		if (!HasExtension(available, required))
		{
			std::fprintf(stderr, "Required SDL instance extension is unavailable: %s\n", required);
			return 1;
		}
	}
	const bool portability = HasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	if (portability && !HasExtension(extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
	{
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	}

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Lumen graphics smoke test";
	appInfo.applicationVersion = 1;
	appInfo.pEngineName = "Lumen";
	appInfo.engineVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_2;

	VkInstanceCreateInfo instanceInfo{};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;
	instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	instanceInfo.ppEnabledExtensionNames = extensions.data();
	if (portability)
	{
		instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}
	result = vkCreateInstance(&instanceInfo, nullptr, &state.instance);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateInstance failed", result);
	}

	if (SDL_Vulkan_CreateSurface(state.window, state.instance, &state.surface) == SDL_FALSE)
	{
		std::fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
		return 1;
	}

	uint32_t physicalDeviceCount = 0;
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0)
	{
		return fail("No graphics physical device found", result);
	}
	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS)
	{
		return fail("vkEnumeratePhysicalDevices(list) failed", result);
	}
	physicalDevices.resize(physicalDeviceCount);

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t queueFamily = UINT32_MAX;
	for (VkPhysicalDevice candidate : physicalDevices)
	{
		uint32_t deviceExtensionCount = 0;
		result = vkEnumerateDeviceExtensionProperties(candidate, nullptr, &deviceExtensionCount, nullptr);
		if (result != VK_SUCCESS)
		{
			continue;
		}
		std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
		result = vkEnumerateDeviceExtensionProperties(candidate, nullptr, &deviceExtensionCount, deviceExtensions.data());
		if (result != VK_SUCCESS)
		{
			continue;
		}
		deviceExtensions.resize(deviceExtensionCount);
		if (!HasExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
		{
			continue;
		}

		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
		for (uint32_t family = 0; family < familyCount; ++family)
		{
			VkBool32 presentSupported = VK_FALSE;
			result = vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, state.surface, &presentSupported);
			if (result == VK_SUCCESS && presentSupported == VK_TRUE && families[family].queueCount != 0 &&
			    (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
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
		return fail("No graphics/present queue with VK_KHR_swapchain found");
	}

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);
	std::printf("Graphics device: %s (API %u.%u.%u)\n", properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
	            VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));

	constexpr float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{};
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	const char* deviceExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	VkDeviceCreateInfo deviceInfo{};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.enabledExtensionCount = 1;
	deviceInfo.ppEnabledExtensionNames = &deviceExtension;
	result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &state.device);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateDevice failed", result);
	}
	vkGetDeviceQueue(state.device, queueFamily, 0, &state.queue);
	if (state.queue == VK_NULL_HANDLE)
	{
		return fail("vkGetDeviceQueue returned a null queue");
	}

	VkSurfaceCapabilitiesKHR capabilities{};
	result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, state.surface, &capabilities);
	if (result != VK_SUCCESS)
	{
		return fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed", result);
	}
	constexpr VkImageUsageFlags requiredSwapchainUsage =
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if ((capabilities.supportedUsageFlags & requiredSwapchainUsage) != requiredSwapchainUsage)
	{
		return fail("Surface does not support transfer-destination and color-attachment swapchain images");
	}

	uint32_t formatCount = 0;
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, state.surface, &formatCount, nullptr);
	if (result != VK_SUCCESS || formatCount == 0)
	{
		return fail("No surface format found", result);
	}
	std::vector<VkSurfaceFormatKHR> formats(formatCount);
	result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, state.surface, &formatCount, formats.data());
	if (result != VK_SUCCESS)
	{
		return fail("vkGetPhysicalDeviceSurfaceFormatsKHR(list) failed", result);
	}
	formats.resize(formatCount);
	VkSurfaceFormatKHR surfaceFormat = formats.front();
	for (const VkSurfaceFormatKHR& format : formats)
	{
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			surfaceFormat = format;
			break;
		}
	}
	if (lineReadbackMode && surfaceFormat.format != VK_FORMAT_B8G8R8A8_UNORM &&
	    surfaceFormat.format != VK_FORMAT_R8G8B8A8_UNORM)
	{
		return fail("Line readback requires an RGBA8 or BGRA8 UNORM surface");
	}

	uint32_t presentModeCount = 0;
	result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, state.surface, &presentModeCount, nullptr);
	if (result != VK_SUCCESS || presentModeCount == 0)
	{
		return fail("No present mode found", result);
	}
	std::vector<VkPresentModeKHR> presentModes(presentModeCount);
	result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, state.surface, &presentModeCount, presentModes.data());
	if (result != VK_SUCCESS)
	{
		return fail("vkGetPhysicalDeviceSurfacePresentModesKHR(list) failed", result);
	}
	presentModes.resize(presentModeCount);
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;
	if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != presentModes.end())
	{
		presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	}
	else if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.end())
	{
		presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
	}
	else
	{
		return fail("Surface does not expose an uncapped present mode (IMMEDIATE or MAILBOX)");
	}

	VkExtent2D extent = capabilities.currentExtent;
	if (extent.width == UINT32_MAX || extent.height == UINT32_MAX)
	{
		int drawableWidth = 0;
		int drawableHeight = 0;
		SDL_Vulkan_GetDrawableSize(state.window, &drawableWidth, &drawableHeight);
		if (drawableWidth <= 0 || drawableHeight <= 0)
		{
			SDL_GetWindowSize(state.window, &drawableWidth, &drawableHeight);
		}
		extent.width = std::clamp(static_cast<uint32_t>(std::max(drawableWidth, 1)), capabilities.minImageExtent.width,
		                          capabilities.maxImageExtent.width);
		extent.height = std::clamp(static_cast<uint32_t>(std::max(drawableHeight, 1)), capabilities.minImageExtent.height,
		                           capabilities.maxImageExtent.height);
	}
	if (extent.width == 0 || extent.height == 0)
	{
		return fail("Surface returned a zero-sized extent");
	}
	if (lineReadbackMode && (extent.width < kLineReadbackWidth || extent.height < kLineReadbackHeight ||
	                      (extent.width & 1u) != 0 || (extent.height & 1u) != 0))
	{
		return fail("Line readback requires an even surface extent of at least 8x8");
	}

	uint32_t imageCount = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount != 0)
	{
		imageCount = std::min(imageCount, capabilities.maxImageCount);
	}
	imageCount = std::max(imageCount, capabilities.minImageCount);
	const VkCompositeAlphaFlagBitsKHR compositeAlpha = ChooseCompositeAlpha(capabilities.supportedCompositeAlpha);
	if (compositeAlpha == 0)
	{
		return fail("Surface returned no supported composite-alpha mode");
	}

	VkSwapchainCreateInfoKHR swapchainInfo{};
	swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchainInfo.surface = state.surface;
	swapchainInfo.minImageCount = imageCount;
	swapchainInfo.imageFormat = surfaceFormat.format;
	swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
	swapchainInfo.imageExtent = extent;
	swapchainInfo.imageArrayLayers = 1;
	swapchainInfo.imageUsage = requiredSwapchainUsage;
	swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchainInfo.preTransform = capabilities.currentTransform;
	swapchainInfo.compositeAlpha = compositeAlpha;
	swapchainInfo.presentMode = presentMode;
	swapchainInfo.clipped = VK_TRUE;
	result = vkCreateSwapchainKHR(state.device, &swapchainInfo, nullptr, &state.swapchain);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateSwapchainKHR failed", result);
	}

	uint32_t swapchainImageCount = 0;
	result = vkGetSwapchainImagesKHR(state.device, state.swapchain, &swapchainImageCount, nullptr);
	if (result != VK_SUCCESS || swapchainImageCount == 0)
	{
		return fail("vkGetSwapchainImagesKHR(count) failed", result);
	}
	std::vector<VkImage> swapchainImages(swapchainImageCount);
	result = vkGetSwapchainImagesKHR(state.device, state.swapchain, &swapchainImageCount, swapchainImages.data());
	if (result != VK_SUCCESS)
	{
		return fail("vkGetSwapchainImagesKHR(list) failed", result);
	}
	swapchainImages.resize(swapchainImageCount);

	VkAttachmentDescription colorAttachment{};
	colorAttachment.format = surfaceFormat.format;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference colorReference{};
	colorReference.attachment = 0;
	colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorReference;
	VkRenderPassCreateInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassInfo.attachmentCount = 1;
	renderPassInfo.pAttachments = &colorAttachment;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	result = vkCreateRenderPass(state.device, &renderPassInfo, nullptr, &state.renderPass);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateRenderPass for SPIR-V graphics gate failed", result);
	}

	VkDescriptorSetLayoutBinding vertexDescriptorBinding{};
	vertexDescriptorBinding.binding = 0;
	vertexDescriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	vertexDescriptorBinding.descriptorCount = 1;
	vertexDescriptorBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	VkDescriptorSetLayoutCreateInfo vertexSetLayoutInfo{};
	vertexSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	vertexSetLayoutInfo.bindingCount = 1;
	vertexSetLayoutInfo.pBindings = &vertexDescriptorBinding;
	result = vkCreateDescriptorSetLayout(state.device, &vertexSetLayoutInfo, nullptr, &state.vertexSetLayout);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateDescriptorSetLayout(vertex storage) failed", result);
	}

	VkDescriptorSetLayoutBinding fragmentDescriptorBinding{};
	fragmentDescriptorBinding.binding = 0;
	fragmentDescriptorBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	fragmentDescriptorBinding.descriptorCount = 1;
	fragmentDescriptorBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo fragmentSetLayoutInfo{};
	fragmentSetLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	fragmentSetLayoutInfo.bindingCount = 1;
	fragmentSetLayoutInfo.pBindings = &fragmentDescriptorBinding;
	result = vkCreateDescriptorSetLayout(state.device, &fragmentSetLayoutInfo, nullptr, &state.fragmentSetLayout);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateDescriptorSetLayout(fragment combined sampler) failed", result);
	}

	const std::array<VkDescriptorSetLayout, 2> descriptorSetLayouts = {
	    state.vertexSetLayout,
	    state.fragmentSetLayout,
	};
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(descriptorSetLayouts.size());
	pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts.data();
	result = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreatePipelineLayout for separate VS/PS sets failed", result);
	}

	const std::array<VkDescriptorPoolSize, 2> descriptorPoolSizes = {{
	    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
	    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
	}};
	VkDescriptorPoolCreateInfo descriptorPoolInfo{};
	descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	descriptorPoolInfo.maxSets = 2;
	descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(descriptorPoolSizes.size());
	descriptorPoolInfo.pPoolSizes = descriptorPoolSizes.data();
	result = vkCreateDescriptorPool(state.device, &descriptorPoolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateDescriptorPool for separate VS/PS sets failed", result);
	}
	std::array<VkDescriptorSet, 2> descriptorSets{};
	VkDescriptorSetAllocateInfo descriptorSetInfo{};
	descriptorSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetInfo.descriptorPool = state.descriptorPool;
	descriptorSetInfo.descriptorSetCount = static_cast<uint32_t>(descriptorSetLayouts.size());
	descriptorSetInfo.pSetLayouts = descriptorSetLayouts.data();
	result = vkAllocateDescriptorSets(state.device, &descriptorSetInfo, descriptorSets.data());
	if (result != VK_SUCCESS)
	{
		return fail("vkAllocateDescriptorSets for separate VS/PS sets failed", result);
	}
	state.vertexDescriptorSet = descriptorSets[0];
	state.fragmentDescriptorSet = descriptorSets[1];

	VkShaderModuleCreateInfo shaderInfo{};
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = kVertexShader.size() * sizeof(kVertexShader[0]);
	shaderInfo.pCode = kVertexShader.data();
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.vertexShader);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateShaderModule(vertex) failed", result);
	}
	shaderInfo.codeSize = kFragmentShader.size() * sizeof(kFragmentShader[0]);
	shaderInfo.pCode = kFragmentShader.data();
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.fragmentShader);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateShaderModule(fragment) failed", result);
	}

	std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages{};
	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = state.vertexShader;
	shaderStages[0].pName = "main";
	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = state.fragmentShader;
	shaderStages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = lineListMode ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
	                                      : (lineStripMode ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
	                                                       : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
	VkViewport viewport{};
	viewport.width = static_cast<float>(extent.width);
	viewport.height = static_cast<float>(extent.height);
	viewport.maxDepth = 1.0f;
	VkRect2D scissor{};
	scissor.extent = extent;
	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.pViewports = &viewport;
	viewportState.scissorCount = 1;
	viewportState.pScissors = &scissor;
	VkPipelineRasterizationStateCreateInfo rasterization{};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments = &colorBlendAttachment;
	bool depthStencilCacheSmokeProbe = false;
#if defined(LUMEN_GRAPHICS_SMOKE_NATIVE_METAL)
	const char* depthStencilCacheSelector =
	    std::getenv("LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE");
	depthStencilCacheSmokeProbe =
	    depthStencilCacheSelector != nullptr &&
	    depthStencilCacheSelector[0] == '1' &&
	    depthStencilCacheSelector[1] == '\0';
#endif
	VkPipelineDepthStencilStateCreateInfo depthStencilState{};
	depthStencilState.sType =
	    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencilState.depthTestEnable = VK_TRUE;
	depthStencilState.depthWriteEnable = VK_FALSE;
	depthStencilState.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	// This render pass intentionally has no depth/stencil attachment. Keep a
	// real depth-state object for cache miss/hit coverage without asking Metal
	// Validation to execute stencil tests against a nil attachment.
	depthStencilState.stencilTestEnable = VK_FALSE;
	depthStencilState.front.failOp = VK_STENCIL_OP_KEEP;
	depthStencilState.front.passOp = VK_STENCIL_OP_REPLACE;
	depthStencilState.front.depthFailOp = VK_STENCIL_OP_KEEP;
	depthStencilState.front.compareOp = VK_COMPARE_OP_ALWAYS;
	depthStencilState.front.compareMask = 0x0fu;
	depthStencilState.front.writeMask = 0xf0u;
	depthStencilState.front.reference = 1u;
	depthStencilState.back = depthStencilState.front;
	depthStencilState.back.compareMask = 0x33u;
	depthStencilState.back.writeMask = 0xccu;
	depthStencilState.back.reference = 2u;
	std::array<VkDynamicState, 4> dynamicStates{};
	uint32_t dynamicStateCount = 0;
	if (lineReadbackMode)
	{
		dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_LINE_WIDTH;
	}
	if (depthStencilCacheSmokeProbe)
	{
		dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
		dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
		dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
	}
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = dynamicStateCount;
	dynamicState.pDynamicStates = dynamicStates.data();
	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
	pipelineInfo.pStages = shaderStages.data();
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterization;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pColorBlendState = &colorBlend;
	if (dynamicStateCount != 0)
	{
		pipelineInfo.pDynamicState = &dynamicState;
	}
	if (depthStencilCacheSmokeProbe)
	{
		pipelineInfo.pDepthStencilState = &depthStencilState;
	}
	pipelineInfo.layout = state.pipelineLayout;
	pipelineInfo.renderPass = state.renderPass;
	result = vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.graphicsPipeline);
	if (result != VK_SUCCESS)
	{
		return fail("SPIR-V shader translation/native graphics pipeline creation failed", result);
	}
#if defined(LUMEN_GRAPHICS_SMOKE_NATIVE_METAL)
	const char* shaderFunctionCacheSelector =
	    std::getenv("LUMEN_A64_METAL_SHADER_FUNCTION_CACHE");
	if (shaderFunctionCacheSelector != nullptr &&
	    shaderFunctionCacheSelector[0] == '1' &&
	    shaderFunctionCacheSelector[1] == '\0')
	{
		VkPipeline duplicatePipeline = VK_NULL_HANDLE;
		result = vkCreateGraphicsPipelines(state.device, VK_NULL_HANDLE, 1,
		                                   &pipelineInfo, nullptr,
		                                   &duplicatePipeline);
		if (result != VK_SUCCESS)
		{
			if (duplicatePipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(state.device, duplicatePipeline, nullptr);
			}
			return fail("Native Metal shader-function cache hit probe failed",
			            result);
		}
		vkDestroyPipeline(state.device, duplicatePipeline, nullptr);
	}
#endif

	VkImageCreateInfo offscreenInfo{};
	offscreenInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	offscreenInfo.imageType = VK_IMAGE_TYPE_2D;
	offscreenInfo.format = surfaceFormat.format;
	offscreenInfo.extent = {extent.width, extent.height, 1};
	offscreenInfo.mipLevels = 1;
	offscreenInfo.arrayLayers = 1;
	offscreenInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	offscreenInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	offscreenInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	offscreenInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	offscreenInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(state.device, &offscreenInfo, nullptr, &state.offscreenImage);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateImage(offscreen color/transfer source) failed", result);
	}
	VkMemoryRequirements offscreenRequirements{};
	vkGetImageMemoryRequirements(state.device, state.offscreenImage, &offscreenRequirements);
	const uint32_t offscreenMemoryType =
	    FindMemoryType(physicalDevice, offscreenRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (offscreenMemoryType == UINT32_MAX)
	{
		return fail("No device-local memory type for offscreen image");
	}
	VkMemoryAllocateInfo offscreenAllocation{};
	offscreenAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	offscreenAllocation.allocationSize = offscreenRequirements.size;
	offscreenAllocation.memoryTypeIndex = offscreenMemoryType;
	result = vkAllocateMemory(state.device, &offscreenAllocation, nullptr, &state.offscreenMemory);
	if (result != VK_SUCCESS)
	{
		return fail("vkAllocateMemory(offscreen) failed", result);
	}
	result = vkBindImageMemory(state.device, state.offscreenImage, state.offscreenMemory, 0);
	if (result != VK_SUCCESS)
	{
		return fail("vkBindImageMemory(offscreen) failed", result);
	}
	VkImageViewCreateInfo offscreenViewInfo{};
	offscreenViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	offscreenViewInfo.image = state.offscreenImage;
	offscreenViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	offscreenViewInfo.format = surfaceFormat.format;
	offscreenViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	offscreenViewInfo.subresourceRange.levelCount = 1;
	offscreenViewInfo.subresourceRange.layerCount = 1;
	result = vkCreateImageView(state.device, &offscreenViewInfo, nullptr, &state.offscreenImageView);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateImageView(offscreen) failed", result);
	}
	VkFramebufferCreateInfo offscreenFramebufferInfo{};
	offscreenFramebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	offscreenFramebufferInfo.renderPass = state.renderPass;
	offscreenFramebufferInfo.attachmentCount = 1;
	offscreenFramebufferInfo.pAttachments = &state.offscreenImageView;
	offscreenFramebufferInfo.width = extent.width;
	offscreenFramebufferInfo.height = extent.height;
	offscreenFramebufferInfo.layers = 1;
	result = vkCreateFramebuffer(state.device, &offscreenFramebufferInfo, nullptr, &state.offscreenFramebuffer);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateFramebuffer(offscreen) failed", result);
	}

	const std::array<float, 4> vertexPositionOffset =
	    lineReadbackMode ? std::array<float, 4>{-1.0f, -1.0f, 0.0f, 0.0f}
	                     : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
	result = CreateBufferWithMemory(
	    physicalDevice, state.device, sizeof(vertexPositionOffset), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &state.vertexStorageBuffer,
	    &state.vertexStorageMemory);
	if (result != VK_SUCCESS)
	{
		return fail("CreateBufferWithMemory(vertex storage descriptor) failed", result);
	}
	void* mappedVertexOffset = nullptr;
	result = vkMapMemory(state.device, state.vertexStorageMemory, 0, sizeof(vertexPositionOffset), 0,
	                     &mappedVertexOffset);
	if (result != VK_SUCCESS || mappedVertexOffset == nullptr)
	{
		return fail("vkMapMemory(vertex storage descriptor) failed", result);
	}
	std::memcpy(mappedVertexOffset, vertexPositionOffset.data(), sizeof(vertexPositionOffset));
	vkUnmapMemory(state.device, state.vertexStorageMemory);

	using Lumen::Libs::Graphics::NullSampledImageContract;
	const bool zeroSampledTextureMode = nullSampledImageMode || lineReadbackMode;
	const bool readbackMode = zeroSampledTextureMode;
	const uint32_t sampledWidth = zeroSampledTextureMode ? NullSampledImageContract::width : 64;
	const uint32_t sampledHeight = zeroSampledTextureMode ? NullSampledImageContract::height : 64;
	const VkDeviceSize sampledByteSize = zeroSampledTextureMode
	                                         ? NullSampledImageContract::upload_bytes
	                                         : static_cast<VkDeviceSize>(sampledWidth) * sampledHeight *
	                                               NullSampledImageContract::bytes_per_texel;
	std::vector<uint8_t> sampledPixels(static_cast<size_t>(sampledByteSize), 0);
	for (size_t pixel = 0; !zeroSampledTextureMode && pixel < sampledPixels.size();
	     pixel += NullSampledImageContract::bytes_per_texel)
	{
		sampledPixels[pixel + 0] = 0x20;
		sampledPixels[pixel + 1] = 0x90;
		sampledPixels[pixel + 2] = 0xe0;
		sampledPixels[pixel + 3] = 0xff;
	}
	result = CreateBufferWithMemory(
	    physicalDevice, state.device, sampledByteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &state.sampledStagingBuffer,
	    &state.sampledStagingMemory);
	if (result != VK_SUCCESS)
	{
		return fail("CreateBufferWithMemory(sampled texture staging) failed", result);
	}
	void* mappedSampledPixels = nullptr;
	result = vkMapMemory(state.device, state.sampledStagingMemory, 0, sampledByteSize, 0, &mappedSampledPixels);
	if (result != VK_SUCCESS || mappedSampledPixels == nullptr)
	{
		return fail("vkMapMemory(sampled texture staging) failed", result);
	}
	std::memcpy(mappedSampledPixels, sampledPixels.data(), sampledPixels.size());
	vkUnmapMemory(state.device, state.sampledStagingMemory);

	VkImageCreateInfo sampledImageInfo{};
	sampledImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	sampledImageInfo.imageType = VK_IMAGE_TYPE_2D;
	sampledImageInfo.format = NullSampledImageContract::format;
	sampledImageInfo.extent = {sampledWidth, sampledHeight, 1};
	sampledImageInfo.mipLevels = 1;
	sampledImageInfo.arrayLayers = 1;
	sampledImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	sampledImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	sampledImageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	sampledImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sampledImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(state.device, &sampledImageInfo, nullptr, &state.sampledImage);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateImage(real sampled texture) failed", result);
	}
	VkMemoryRequirements sampledRequirements{};
	vkGetImageMemoryRequirements(state.device, state.sampledImage, &sampledRequirements);
	const uint32_t sampledMemoryType =
	    FindMemoryType(physicalDevice, sampledRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (sampledMemoryType == UINT32_MAX)
	{
		return fail("No device-local memory type for sampled texture");
	}
	VkMemoryAllocateInfo sampledAllocation{};
	sampledAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	sampledAllocation.allocationSize = sampledRequirements.size;
	sampledAllocation.memoryTypeIndex = sampledMemoryType;
	result = vkAllocateMemory(state.device, &sampledAllocation, nullptr, &state.sampledMemory);
	if (result != VK_SUCCESS)
	{
		return fail("vkAllocateMemory(real sampled texture) failed", result);
	}
	result = vkBindImageMemory(state.device, state.sampledImage, state.sampledMemory, 0);
	if (result != VK_SUCCESS)
	{
		return fail("vkBindImageMemory(real sampled texture) failed", result);
	}
	VkImageViewCreateInfo sampledViewInfo{};
	sampledViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	sampledViewInfo.image = state.sampledImage;
	sampledViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	sampledViewInfo.format = sampledImageInfo.format;
	sampledViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	sampledViewInfo.subresourceRange.levelCount = 1;
	sampledViewInfo.subresourceRange.layerCount = 1;
	result = vkCreateImageView(state.device, &sampledViewInfo, nullptr, &state.sampledImageView);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateImageView(real sampled texture) failed", result);
	}
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	result = vkCreateSampler(state.device, &samplerInfo, nullptr, &state.sampledSampler);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateSampler(real combined image sampler) failed", result);
	}
	const VkDeviceSize readbackBytes = lineReadbackMode ? kLineReadbackBytes : NullSampledImageContract::upload_bytes;
	if (readbackMode)
	{
		result = CreateBufferWithMemory(
		    physicalDevice, state.device, readbackBytes,
		    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		    &state.nullSampledReadbackBuffer, &state.nullSampledReadbackMemory);
		if (result != VK_SUCCESS)
		{
			return fail("CreateBufferWithMemory(null sampled image readback) failed", result);
		}
		void* mappedReadbackSentinel = nullptr;
		result = vkMapMemory(state.device, state.nullSampledReadbackMemory, 0, readbackBytes, 0,
		                     &mappedReadbackSentinel);
		if (result != VK_SUCCESS || mappedReadbackSentinel == nullptr)
		{
			return fail("vkMapMemory(null sampled image readback sentinel) failed", result);
		}
		std::memset(mappedReadbackSentinel, 0xa5, static_cast<size_t>(readbackBytes));
		vkUnmapMemory(state.device, state.nullSampledReadbackMemory);
	}

	VkDescriptorBufferInfo vertexBufferDescriptor{};
	vertexBufferDescriptor.buffer = state.vertexStorageBuffer;
	vertexBufferDescriptor.range = sizeof(vertexPositionOffset);
	VkDescriptorImageInfo sampledImageDescriptor{};
	sampledImageDescriptor.sampler = state.sampledSampler;
	sampledImageDescriptor.imageView = state.sampledImageView;
	sampledImageDescriptor.imageLayout = NullSampledImageContract::final_layout;
	std::array<VkWriteDescriptorSet, 2> descriptorWrites{};
	descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrites[0].dstSet = state.vertexDescriptorSet;
	descriptorWrites[0].descriptorCount = 1;
	descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrites[0].pBufferInfo = &vertexBufferDescriptor;
	descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrites[1].dstSet = state.fragmentDescriptorSet;
	descriptorWrites[1].descriptorCount = 1;
	descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorWrites[1].pImageInfo = &sampledImageDescriptor;
	vkUpdateDescriptorSets(state.device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0,
	                       nullptr);

	constexpr std::array<uint16_t, 4> fanIndices = {0, 1, 2, 3};
	VkBufferCreateInfo stagingInfo{};
	stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	stagingInfo.size = sizeof(fanIndices);
	stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(state.device, &stagingInfo, nullptr, &state.fanIndexStagingBuffer);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateBuffer(fan staging) failed", result);
	}
	VkMemoryRequirements stagingRequirements{};
	vkGetBufferMemoryRequirements(state.device, state.fanIndexStagingBuffer, &stagingRequirements);
	const uint32_t stagingMemoryType = FindMemoryType(
	    physicalDevice, stagingRequirements.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (stagingMemoryType == UINT32_MAX)
	{
		return fail("No coherent host-visible memory type for fan staging buffer");
	}
	VkMemoryAllocateInfo stagingAllocation{};
	stagingAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	stagingAllocation.allocationSize = stagingRequirements.size;
	stagingAllocation.memoryTypeIndex = stagingMemoryType;
	result = vkAllocateMemory(state.device, &stagingAllocation, nullptr, &state.fanIndexStagingMemory);
	if (result != VK_SUCCESS)
	{
		return fail("vkAllocateMemory(fan staging) failed", result);
	}
	result = vkBindBufferMemory(state.device, state.fanIndexStagingBuffer, state.fanIndexStagingMemory, 0);
	if (result != VK_SUCCESS)
	{
		return fail("vkBindBufferMemory(fan staging) failed", result);
	}
	void* mappedFanIndices = nullptr;
	result = vkMapMemory(state.device, state.fanIndexStagingMemory, 0, sizeof(fanIndices), 0, &mappedFanIndices);
	if (result != VK_SUCCESS || mappedFanIndices == nullptr)
	{
		return fail("vkMapMemory(fan staging) failed", result);
	}
	std::memcpy(mappedFanIndices, fanIndices.data(), sizeof(fanIndices));
	vkUnmapMemory(state.device, state.fanIndexStagingMemory);

	VkBufferCreateInfo indexInfo{};
	indexInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	indexInfo.size = sizeof(fanIndices);
	indexInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	indexInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(state.device, &indexInfo, nullptr, &state.fanIndexBuffer);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateBuffer(device-local fan indices) failed", result);
	}
	VkMemoryRequirements indexRequirements{};
	vkGetBufferMemoryRequirements(state.device, state.fanIndexBuffer, &indexRequirements);
	const uint32_t indexMemoryType =
	    FindMemoryType(physicalDevice, indexRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (indexMemoryType == UINT32_MAX)
	{
		return fail("No device-local memory type for fan index buffer");
	}
	VkMemoryAllocateInfo indexAllocation{};
	indexAllocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	indexAllocation.allocationSize = indexRequirements.size;
	indexAllocation.memoryTypeIndex = indexMemoryType;
	result = vkAllocateMemory(state.device, &indexAllocation, nullptr, &state.fanIndexMemory);
	if (result != VK_SUCCESS)
	{
		return fail("vkAllocateMemory(device-local fan indices) failed", result);
	}
	result = vkBindBufferMemory(state.device, state.fanIndexBuffer, state.fanIndexMemory, 0);
	if (result != VK_SUCCESS)
	{
		return fail("vkBindBufferMemory(device-local fan indices) failed", result);
	}

	state.swapchainImageViews.resize(swapchainImages.size(), VK_NULL_HANDLE);
	state.framebuffers.resize(swapchainImages.size(), VK_NULL_HANDLE);
	for (size_t image = 0; image < swapchainImages.size(); ++image)
	{
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = swapchainImages[image];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = surfaceFormat.format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		result = vkCreateImageView(state.device, &viewInfo, nullptr, &state.swapchainImageViews[image]);
		if (result != VK_SUCCESS)
		{
			return fail("vkCreateImageView(swapchain) failed", result);
		}
		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = state.renderPass;
		framebufferInfo.attachmentCount = 1;
		framebufferInfo.pAttachments = &state.swapchainImageViews[image];
		framebufferInfo.width = extent.width;
		framebufferInfo.height = extent.height;
		framebufferInfo.layers = 1;
		result = vkCreateFramebuffer(state.device, &framebufferInfo, nullptr, &state.framebuffers[image]);
		if (result != VK_SUCCESS)
		{
			return fail("vkCreateFramebuffer(swapchain) failed", result);
		}
	}

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	poolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(state.device, &poolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateCommandPool failed", result);
	}
	VkCommandBufferAllocateInfo commandInfo{};
	commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandInfo.commandPool = state.commandPool;
	commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandInfo.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(state.device, &commandInfo, &state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return fail("vkAllocateCommandBuffers failed", result);
	}

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	result = vkCreateSemaphore(state.device, &semaphoreInfo, nullptr, &state.imageAvailable);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateSemaphore(image available) failed", result);
	}
	result = vkCreateSemaphore(state.device, &semaphoreInfo, nullptr, &state.renderFinished);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateSemaphore(render finished) failed", result);
	}
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(state.device, &fenceInfo, nullptr, &state.completionFence);
	if (result != VK_SUCCESS)
	{
		return fail("vkCreateFence failed", result);
	}

	uint32_t imageIndex = 0;
	result = vkAcquireNextImageKHR(state.device, state.swapchain, kAcquireTimeoutNanoseconds, state.imageAvailable,
	                               VK_NULL_HANDLE, &imageIndex);
	bool showRequired = false;
	if (!IsSuccess(result) && CanRetryAcquireAfterShow(result))
	{
		showRequired = true;
		SDL_ShowWindow(state.window);
		SDL_RaiseWindow(state.window);
		for (uint32_t attempt = 0; attempt < 120 && CanRetryAcquireAfterShow(result); ++attempt)
		{
			SDL_PumpEvents();
			SDL_Delay(16);
			result = vkAcquireNextImageKHR(state.device, state.swapchain, 0, state.imageAvailable, VK_NULL_HANDLE,
			                               &imageIndex);
		}
	}
	if (!IsSuccess(result))
	{
		return fail(showRequired ? "vkAcquireNextImageKHR failed after SDL_ShowWindow fallback"
		                         : "vkAcquireNextImageKHR failed",
		            result);
	}
	if (imageIndex >= swapchainImages.size())
	{
		return fail("vkAcquireNextImageKHR returned an out-of-range image index");
	}
	std::printf("Hidden-window acquire: %s SDL_ShowWindow\n", showRequired ? "required" : "did not require");

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
	{
		return fail("vkBeginCommandBuffer failed", result);
	}

	VkBufferCopy fanIndexCopy{};
	fanIndexCopy.size = sizeof(uint16_t) * 4;
	vkCmdCopyBuffer(state.commandBuffer, state.fanIndexStagingBuffer, state.fanIndexBuffer, 1, &fanIndexCopy);

	VkImageMemoryBarrier sampledToTransfer{};
	sampledToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	sampledToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	sampledToTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	sampledToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	sampledToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sampledToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sampledToTransfer.image = state.sampledImage;
	sampledToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	sampledToTransfer.subresourceRange.levelCount = 1;
	sampledToTransfer.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &sampledToTransfer);
	VkBufferImageCopy sampledCopy{};
	sampledCopy.bufferRowLength = nullSampledImageMode ? NullSampledImageContract::upload_pitch_texels : 0;
	sampledCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	sampledCopy.imageSubresource.layerCount = 1;
	sampledCopy.imageExtent = {sampledWidth, sampledHeight, 1};
	vkCmdCopyBufferToImage(state.commandBuffer, state.sampledStagingBuffer, state.sampledImage,
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &sampledCopy);
	VkImageMemoryBarrier sampledToShader = sampledToTransfer;
	sampledToShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	sampledToShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	sampledToShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	sampledToShader.newLayout = NullSampledImageContract::final_layout;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     NullSampledImageContract::final_stage, 0, 0, nullptr, 0, nullptr, 1, &sampledToShader);

	VkImageMemoryBarrier offscreenToColor{};
	offscreenToColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	offscreenToColor.srcAccessMask = 0;
	offscreenToColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	offscreenToColor.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	offscreenToColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	offscreenToColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	offscreenToColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	offscreenToColor.image = state.offscreenImage;
	offscreenToColor.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	offscreenToColor.subresourceRange.levelCount = 1;
	offscreenToColor.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
	                     &offscreenToColor);
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state.graphicsPipeline);
	// Match GraphicsRender's runtime order before BeginRenderPass: update set 0
	// first, then set 1; the draw consumes the resulting complete state.
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelineLayout, 0, 1,
	                        &state.vertexDescriptorSet, 0, nullptr);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipelineLayout, 1, 1,
	                        &state.fragmentDescriptorSet, 0, nullptr);

	VkRenderPassBeginInfo renderPassBegin{};
	renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassBegin.renderPass = state.renderPass;
	renderPassBegin.framebuffer = state.offscreenFramebuffer;
	renderPassBegin.renderArea.extent = extent;
	VkClearValue renderClear{};
	renderClear.color.float32[0] = 0.0625f;
	renderClear.color.float32[1] = 0.125f;
	renderClear.color.float32[2] = 0.25f;
	renderClear.color.float32[3] = 1.0f;
	renderPassBegin.clearValueCount = 1;
	renderPassBegin.pClearValues = &renderClear;
	vkCmdBeginRenderPass(state.commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
	if (lineReadbackMode)
	{
		// Match the renderer's dynamic line-width path while staying within the
		// core feature set: wideLines is not enabled for this device.
		vkCmdSetLineWidth(state.commandBuffer, 1.0f);
	}
	if (depthStencilCacheSmokeProbe)
	{
		// These dynamic mutations exercise the resolver and state-only elision.
		// The first draw creates the resolved descriptor; the indexed draw reuses
		// it and proves the device-local depth/stencil cache hit.
		vkCmdSetStencilCompareMask(state.commandBuffer,
		                           VK_STENCIL_FACE_FRONT_AND_BACK,
		                           0x5au);
		vkCmdSetStencilWriteMask(state.commandBuffer,
		                         VK_STENCIL_FACE_FRONT_AND_BACK,
		                         0xa5u);
		vkCmdSetStencilReference(state.commandBuffer,
		                         VK_STENCIL_FACE_FRONT_AND_BACK, 7u);
	}
	// Starting the line-list draw at vertex 1 reuses the visible 1->2 segment
	// from the line-strip probe. The third vertex covers the valid unpaired-tail
	// behavior of a line list without changing that segment's pixel oracle.
	vkCmdDraw(state.commandBuffer, lineListMode ? 3u : (lineStripMode ? 3u : 4u), 1, lineListMode ? 1u : 0u, 0);
	if (!lineReadbackMode)
	{
		vkCmdBindIndexBuffer(state.commandBuffer, state.fanIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(state.commandBuffer, 4, 1, 0, 0, 0);
	}
	vkCmdEndRenderPass(state.commandBuffer);

	std::array<VkImageMemoryBarrier, 2> toBlit{};
	toBlit[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toBlit[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toBlit[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toBlit[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toBlit[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toBlit[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toBlit[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toBlit[0].image = state.offscreenImage;
	toBlit[0].subresourceRange = offscreenToColor.subresourceRange;
	toBlit[1] = toBlit[0];
	toBlit[1].srcAccessMask = 0;
	toBlit[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toBlit[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toBlit[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toBlit[1].image = swapchainImages[imageIndex];
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
	                     static_cast<uint32_t>(toBlit.size()), toBlit.data());
	if (readbackMode)
	{
		VkBufferImageCopy readbackCopy{};
		readbackCopy.bufferRowLength = lineReadbackMode ? kLineReadbackPitchTexels
		                                              : NullSampledImageContract::upload_pitch_texels;
		readbackCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		readbackCopy.imageSubresource.layerCount = 1;
		readbackCopy.imageOffset = {
		    static_cast<int32_t>(extent.width / 2 - (lineReadbackMode ? kLineReadbackWidth / 2 : 0)),
		    static_cast<int32_t>(extent.height / 2 - (lineReadbackMode ? kLineReadbackHeight / 2 : 0)), 0};
		readbackCopy.imageExtent = {lineReadbackMode ? kLineReadbackWidth : NullSampledImageContract::width,
		                            lineReadbackMode ? kLineReadbackHeight : NullSampledImageContract::height, 1};
		vkCmdCopyImageToBuffer(state.commandBuffer, state.offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                       state.nullSampledReadbackBuffer, 1, &readbackCopy);

		VkBufferMemoryBarrier readbackToHost{};
		readbackToHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		readbackToHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		readbackToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		readbackToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		readbackToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		readbackToHost.buffer = state.nullSampledReadbackBuffer;
		readbackToHost.size = readbackBytes;
		vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
		                     nullptr, 1, &readbackToHost, 0, nullptr);
	}

	VkImageBlit blit{};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	blit.dstOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
	vkCmdBlitImage(state.commandBuffer, state.offscreenImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

	VkImageMemoryBarrier toPresent = toBlit[1];
	toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toPresent.dstAccessMask = 0;
	toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return fail("vkEndCommandBuffer failed", result);
	}

	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &state.imageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &state.commandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &state.renderFinished;
	result = vkQueueSubmit(state.queue, 1, &submitInfo, state.completionFence);
	if (result != VK_SUCCESS)
	{
		return fail("vkQueueSubmit failed", result);
	}
	result = vkWaitForFences(state.device, 1, &state.completionFence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS)
	{
		return fail("vkWaitForFences failed", result);
	}
	std::array<uint8_t, NullSampledImageContract::bytes_per_texel> sampledRgba{};
	uint32_t lineBlackPixels = 0;
	uint32_t lineActiveRows = 0;
	uint32_t lineClearPixels = 0;
	uint32_t lineUnexpectedPixels = 0;
	if (readbackMode)
	{
		void* mappedReadback = nullptr;
		result = vkMapMemory(state.device, state.nullSampledReadbackMemory, 0, readbackBytes, 0,
		                     &mappedReadback);
		if (result != VK_SUCCESS || mappedReadback == nullptr)
		{
			return fail("vkMapMemory(null sampled image readback) failed", result);
		}
		if (nullSampledImageMode)
		{
			std::memcpy(sampledRgba.data(), mappedReadback, sampledRgba.size());
		} else
		{
			const auto* bytes = static_cast<const uint8_t*>(mappedReadback);
			const std::array<uint8_t, 4> clearPixel =
			    surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM
			        ? std::array<uint8_t, 4>{0x40, 0x20, 0x10, 0xff}
			        : std::array<uint8_t, 4>{0x10, 0x20, 0x40, 0xff};
			for (uint32_t y = 0; y < kLineReadbackHeight; ++y)
			{
				uint32_t rowBlackPixels = 0;
				for (uint32_t x = 0; x < kLineReadbackWidth; ++x)
				{
					const size_t offset = (static_cast<size_t>(y) * kLineReadbackPitchTexels + x) * 4;
					if (bytes[offset] == 0 && bytes[offset + 1] == 0 && bytes[offset + 2] == 0 && bytes[offset + 3] == 0)
					{
						++rowBlackPixels;
						++lineBlackPixels;
					} else if (std::equal(clearPixel.begin(), clearPixel.end(), bytes + offset))
					{
						++lineClearPixels;
					} else
					{
						++lineUnexpectedPixels;
					}
				}
				if (rowBlackPixels == 1) ++lineActiveRows;
				for (uint32_t x = kLineReadbackWidth; x < kLineReadbackPitchTexels; ++x)
				{
					const size_t offset = (static_cast<size_t>(y) * kLineReadbackPitchTexels + x) * 4;
					if (bytes[offset] != 0xa5 || bytes[offset + 1] != 0xa5 || bytes[offset + 2] != 0xa5 || bytes[offset + 3] != 0xa5)
					{
						vkUnmapMemory(state.device, state.nullSampledReadbackMemory);
						return fail("Line readback row padding was modified");
					}
				}
			}
		}
		vkUnmapMemory(state.device, state.nullSampledReadbackMemory);
		if (nullSampledImageMode &&
		    !std::all_of(sampledRgba.begin(), sampledRgba.end(), [](uint8_t value) { return value == 0; }))
		{
			std::fprintf(stderr,
			             "Null sampled image readback was not zero: sampled_rgba=%02x%02x%02x%02x\n",
			             static_cast<unsigned>(sampledRgba[0]), static_cast<unsigned>(sampledRgba[1]),
			             static_cast<unsigned>(sampledRgba[2]), static_cast<unsigned>(sampledRgba[3]));
			return 1;
		}
		if (lineReadbackMode &&
		    (lineBlackPixels != 8 || lineActiveRows != 8 || lineClearPixels != 56 || lineUnexpectedPixels != 0))
		{
			std::fprintf(stderr,
			             "Line readback mismatch: topology=%s black_pixels=%u active_rows=%u clear_pixels=%u unexpected_pixels=%u\n",
			             lineListMode ? "line-list" : "line-strip", lineBlackPixels, lineActiveRows, lineClearPixels,
			             lineUnexpectedPixels);
			return 1;
		}
	}

	VkResult perSwapchainResult = VK_SUCCESS;
	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &state.renderFinished;
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &state.swapchain;
	presentInfo.pImageIndices = &imageIndex;
	presentInfo.pResults = &perSwapchainResult;
	result = vkQueuePresentKHR(state.queue, &presentInfo);
	if (!IsSuccess(result) || !IsSuccess(perSwapchainResult))
	{
		return fail("vkQueuePresentKHR failed", result != VK_SUCCESS ? result : perSwapchainResult);
	}
	result = vkQueueWaitIdle(state.queue);
	if (result != VK_SUCCESS)
	{
		return fail("vkQueueWaitIdle after present failed", result);
	}

	if (nullSampledImageMode)
	{
		const int markerResult =
		    std::printf("[GFX_NULL_SAMPLED_BACKEND_TEST] backend=native-metal format=rgba8-unorm width=%u height=%u"
		                " upload_pitch=%u upload_bytes=%u sampled_rgba=%02x%02x%02x%02x status=clean\n",
		                NullSampledImageContract::width, NullSampledImageContract::height,
		                NullSampledImageContract::upload_pitch_texels, NullSampledImageContract::upload_bytes,
		                static_cast<unsigned>(sampledRgba[0]), static_cast<unsigned>(sampledRgba[1]),
		                static_cast<unsigned>(sampledRgba[2]), static_cast<unsigned>(sampledRgba[3]));
		if (markerResult <= 0 || std::fflush(stdout) != 0)
		{
			return fail("Failed to emit null sampled image backend marker");
		}
	} else if (lineReadbackMode)
	{
		const int markerResult =
		    std::printf("[%s] backend=native-metal topology=%s"
		                " draw=non-indexed vertices=%u first_vertex=%u readback=%ux%u black_pixels=%u clear_pixels=%u"
		                " active_rows=%u unexpected_pixels=%u padding=clean status=clean\n",
		                lineListMode ? "GFX_AUTO_INDEX_LINE_LIST_BACKEND_TEST"
		                             : "GFX_AUTO_INDEX_LINE_STRIP_BACKEND_TEST",
		                lineListMode ? "line-list" : "line-strip", 3u, lineListMode ? 1u : 0u,
		                kLineReadbackWidth, kLineReadbackHeight, lineBlackPixels, lineClearPixels, lineActiveRows,
		                lineUnexpectedPixels);
		if (markerResult <= 0 || std::fflush(stdout) != 0)
		{
			return fail("Failed to emit line backend marker");
		}
	} else
	{
		std::puts("Graphics smoke test passed: separate VS/PS descriptor binds, real combined sampled draw, fan draws, "
		          "offscreen blit, submit fence and present completed");
	}
	return 0;
}
