#ifndef LUMEN_EMULATOR_GRAPHICS_METAL_VULKAN_INTERNAL_H_
#define LUMEN_EMULATOR_GRAPHICS_METAL_VULKAN_INTERNAL_H_

#if !defined(__APPLE__)
#error "The native Metal Vulkan adapter is available only on Apple targets."
#endif

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_ios.h>
#include <vulkan/vulkan_macos.h>
#include <vulkan/vulkan_metal.h>

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Compile the cache only into the production-shaped native Apple Silicon
// desktop adapter. The exact-one runtime selector remains default-off.
#if defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG) && \
    TARGET_OS_OSX && !TARGET_OS_IPHONE
#define LUMEN_A64_METAL_SHADER_FUNCTION_CACHE_COMPILED 1
#else
#define LUMEN_A64_METAL_SHADER_FUNCTION_CACHE_COMPILED 0
#endif

// Build the depth/stencil cache only into the production-shaped native Apple
// Silicon desktop adapter. Its exact-one runtime selector remains default-off.
#if defined(__APPLE__) && defined(__aarch64__) && defined(NDEBUG) && \
    TARGET_OS_OSX && !TARGET_OS_IPHONE
#define LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED 1
#else
#define LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED 0
#endif

// Vulkan deliberately exposes these objects only as pointers to incomplete
// structs.  This translation layer completes those structs with C++ objects
// that own their corresponding Metal objects under ARC.
struct VkInstance_T;
struct VkPhysicalDevice_T;
struct VkDevice_T;
struct VkQueue_T;
struct VkSurfaceKHR_T;
struct VkSwapchainKHR_T;
struct VkDeviceMemory_T;
struct VkBuffer_T;
struct VkImage_T;
struct VkImageView_T;
struct VkSampler_T;
struct VkCommandPool_T;
struct VkCommandBuffer_T;
struct VkFence_T;
struct VkSemaphore_T;
struct VkEvent_T;
struct VkPipelineCache_T;

namespace Lumen::MetalVulkan {

// Caller holds device->mutex. Used by the macOS 12 address path and its test.
VkDeviceAddress QueryBufferAddressFromShader(VkDevice device, VkBuffer buffer);

enum class CommandBufferState : uint8_t
{
	Initial,
	Recording,
	Executable,
	Pending,
	Invalid,
};

enum class SwapchainImageState : uint8_t
{
	Available,
	Acquiring,
	Acquired,
	Presenting,
};

// CAMetalLayer::nextDrawable may wait longer than the timeout supplied to
// vkAcquireNextImageKHR.  Keep the native request independent of the Vulkan
// handle so a timed-out acquire can be resumed safely by a later call.
struct DrawableAcquisition
{
	__strong CAMetalLayer* layer = nil;
	__strong id<CAMetalDrawable> drawable = nil;
	std::mutex mutex;
	std::condition_variable condition;
	bool complete = false;
};

struct EncodingContext
{
	__strong id<MTLCommandBuffer> commandBuffer = nil;
	__strong id<MTLRenderCommandEncoder> renderEncoder = nil;
	__strong id<MTLComputeCommandEncoder> computeEncoder = nil;
	__strong id<MTLBlitCommandEncoder> blitEncoder = nil;
	// A logical Vulkan pass may end while its eligible Metal encoder remains
	// available for the next LOAD pass. Real execution boundaries clear this.
	bool logicalRenderPassActive = false;
	bool renderPassReusable = false;
	bool renderPassMergePending = false;
	bool renderPassContainsMergedDraws = false;
	bool retainedCompletionMarker = false;
	bool renderPassMergeAfterMarkerPending = false;
	bool renderPassMergedAfterMarker = false;
	VkRenderPass retainedRenderPass = VK_NULL_HANDLE;
	VkFramebuffer retainedFramebuffer = VK_NULL_HANDLE;
	VkRect2D retainedRenderArea {};
	__strong MTLRenderPassDescriptor* retainedRenderDescriptor = nil;
	uint64_t logicalRenderPasses = 0;
	uint64_t physicalRenderEncoders = 0;
	uint64_t mergedRenderPasses = 0;
	uint64_t mergedAfterCompletionMarker = 0;
	uint64_t resourceRejectedPasses = 0;
};

struct RecordedOperation
{
	const char* name = "unknown";
	std::function<VkResult(EncodingContext&)> encode;
};

struct ReplayFailure
{
	size_t operationIndex = 0;
	const char* operationName = nullptr;
	VkResult result = VK_SUCCESS;
	bool occurredDuringRecording = false;
};

MTLPixelFormat ToMTLPixelFormat(VkFormat format);
MTLTextureType ToMTLTextureType(VkImageType imageType, VkImageViewType viewType, uint32_t arrayLayers);
MTLTextureUsage ToMTLTextureUsage(VkImageUsageFlags usage);
MTLResourceOptions ToMTLResourceOptions(VkMemoryPropertyFlags properties);
MTLStorageMode ToMTLStorageMode(VkMemoryPropertyFlags properties);
VkFormatFeatureFlags FormatFeatures(VkFormat format, VkImageTiling tiling);
VkResult BeginRenderEncoding(EncodingContext& context, MTLRenderPassDescriptor* descriptor);
void EndRenderEncoding(EncodingContext& context, bool preserveAttachments = false);
void EndEncoding(EncodingContext& context, bool preserveAttachments = false);
void ObserveCompletionPassReuse(const EncodingContext& context);
void FinalizeCompletionPassReuse();

VkResult WaitSemaphore(VkSemaphore semaphore, uint64_t timeoutNanoseconds, bool consume = true);
void SignalSemaphore(VkSemaphore semaphore);
void SignalFence(VkFence fence);
void ResetFence(VkFence fence);
bool FenceIsSignaled(VkFence fence);
void MarkDeviceLost(VkDevice device);
bool MetalShaderFunctionCacheRequested();
bool FinalizeMetalShaderFunctionCache();
bool MetalDepthStencilStateCacheRequested();
bool FinalizeMetalDepthStencilStateCache();
bool FinalizeMetalStateOnlyRecordElision();

VkResult EncodeCommandBuffer(VkCommandBuffer commandBuffer, id<MTLCommandQueue> queue,
	                         id<MTLCommandBuffer>* encodedCommandBuffer, ReplayFailure* replayFailure);
void CompleteCommandBuffer(VkCommandBuffer commandBuffer);
// Delivery from a native completion callback. Error publishes device loss
// before any host event query can treat that completion as success.
struct EventState
{
	VkDevice device = VK_NULL_HANDLE;
	std::mutex mutex;
	std::condition_variable condition;
	uint64_t generation = 0;
	bool set = false;
	bool alive = true;
};

void CompleteEvent(const std::shared_ptr<EventState>& event, uint64_t generation, MTLCommandBufferStatus status);

template <typename T>
VkResult CopyEnumeration(const std::vector<T>& values, uint32_t* count, T* output)
{
	if (count == nullptr)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (output == nullptr)
	{
		*count = static_cast<uint32_t>(values.size());
		return VK_SUCCESS;
	}
	const uint32_t capacity = *count;
	const uint32_t copied = capacity < values.size() ? capacity : static_cast<uint32_t>(values.size());
	for (uint32_t index = 0; index < copied; ++index)
	{
		output[index] = values[index];
	}
	*count = copied;
	return copied < values.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

} // namespace Lumen::MetalVulkan

struct VkDebugUtilsMessengerEXT_T
{
	VkInstance instance = VK_NULL_HANDLE;
	PFN_vkDebugUtilsMessengerCallbackEXT callback = nullptr;
	void* userData = nullptr;
	VkDebugUtilsMessageSeverityFlagsEXT severity = 0;
	VkDebugUtilsMessageTypeFlagsEXT types = 0;
};

struct VkInstance_T
{
	std::mutex mutex;
	std::unordered_set<std::string> enabledExtensions;
	std::vector<std::unique_ptr<VkPhysicalDevice_T>> physicalDevices;
	std::unordered_set<VkDebugUtilsMessengerEXT> debugMessengers;
	std::atomic<bool> alive {true};
};

struct VkPhysicalDevice_T
{
	VkInstance instance = VK_NULL_HANDLE;
	__strong id<MTLDevice> metalDevice = nil;
};

struct VkQueue_T
{
	VkDevice device = VK_NULL_HANDLE;
	uint32_t familyIndex = 0;
	uint32_t queueIndex = 0;
	__strong id<MTLCommandQueue> metalQueue = nil;
	std::mutex mutex;
	std::condition_variable idleCondition;
	uint64_t inFlightCount = 0;
	bool shuttingDown = false;
};

struct VkDevice_T
{
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	__strong id<MTLDevice> metalDevice = nil;
	std::mutex mutex;
	// Waiters hold this mutex while inspecting fences and entering the wait.
	// SignalFence releases its fence mutex before taking it, so notification
	// cannot be lost between the predicate scan and condition-variable wait.
	std::mutex fenceCompletionMutex;
	std::condition_variable fenceCompletionCondition;
	std::mutex semaphoreCompletionMutex;
	std::condition_variable semaphoreCompletionCondition;
	std::vector<std::unique_ptr<VkQueue_T>> queues;
	std::unordered_set<VkBuffer> deviceAddressBuffers;
	__strong id<MTLComputePipelineState> addressQueryPipeline = nil;
	__strong id<MTLCommandQueue> addressQueryQueue = nil;
#if LUMEN_A64_METAL_SHADER_FUNCTION_CACHE_COMPILED
	// The concrete cache type is private to MetalVulkanCommands.mm. Keeping its
	// type-erased owner here bounds cached libraries/functions to this device.
	std::shared_ptr<void> shaderFunctionCache;
#endif
#if LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED
	// Keep the concrete Objective-C state owner private to the command
	// implementation while binding the cache lifetime to its VkDevice.
	std::shared_ptr<void> depthStencilStateCache;
#endif
	std::atomic<bool> alive {true};
	std::atomic<bool> lost {false};
};

// Resource handles do not retain their parent VkDevice/VkDeviceMemory in the
// Vulkan API sense.  Callers must honor Vulkan destruction order; the Metal
// objects below are retained independently by ARC for submitted GPU work.

struct VkPipelineCache_T
{
	VkDevice device = VK_NULL_HANDLE;
	// The adapter does not serialize Metal pipeline binaries. An empty cache
	// still owns a valid Vulkan version-one header and participates in the
	// normal create/query/merge/destroy lifecycle.
	std::array<uint8_t, 4 * sizeof(uint32_t) + VK_UUID_SIZE> header {};
};

struct VkSurfaceKHR_T
{
	VkInstance instance = VK_NULL_HANDLE;
	__strong id metalView = nil;
	__strong CAMetalLayer* layer = nil;
	std::mutex mutex;
	std::atomic<bool> alive {true};
};

struct VkDeviceMemory_T
{
	VkDevice device = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	uint32_t memoryTypeIndex = 0;
	VkMemoryPropertyFlags properties = 0;
	__strong id<MTLBuffer> metalBuffer = nil;
	std::mutex mutex;
	uint32_t mapCount = 0;
	std::atomic<bool> alive {true};
};

// Metal has no triangle-fan primitive. Private index buffers therefore keep a
// sparse CPU mirror of bytes whose value is known from recorded buffer copies.
// Sparse ranges avoid allocating a second buffer-sized block for large guest
// allocations while still allowing fan indices to be expanded fail-closed.
struct MetalBufferShadowRange
{
	VkDeviceSize offset = 0;
	std::vector<uint8_t> bytes;
};

struct VkBuffer_T
{
	VkDevice device = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	VkBufferUsageFlags usage = 0;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize memoryOffset = 0;
	VkDeviceAddress gpuAddress = 0;
	__strong id<MTLBuffer> metalBuffer = nil;
	std::mutex mutex;
	std::vector<MetalBufferShadowRange> cpuShadowRanges;
	bool hostGpuWriteSeen = false;
	std::atomic<bool> alive {true};
};

struct VkImage_T
{
	VkDevice device = VK_NULL_HANDLE;
	VkImageType imageType = VK_IMAGE_TYPE_2D;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageCreateFlags flags = 0;
	VkExtent3D extent {};
	uint32_t mipLevels = 1;
	uint32_t arrayLayers = 1;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
	VkImageUsageFlags usage = 0;
	VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize memoryOffset = 0;
	__strong id<MTLTexture> metalTexture = nil;
	__strong id<CAMetalDrawable> drawable = nil;
	VkSwapchainKHR ownerSwapchain = VK_NULL_HANDLE;
	uint32_t swapchainIndex = UINT32_MAX;
	std::mutex mutex;
	std::unordered_set<VkImageView> views;
	std::atomic<bool> alive {true};
};

struct VkImageView_T
{
	VkDevice device = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
	VkComponentMapping components {};
	VkImageSubresourceRange range {};
	__strong id<MTLTexture> metalTexture = nil;
	std::atomic<bool> alive {true};
};

struct VkSampler_T
{
	VkDevice device = VK_NULL_HANDLE;
	__strong id<MTLSamplerState> metalSampler = nil;
	std::atomic<bool> alive {true};
};

struct VkSwapchainKHR_T
{
	VkDevice device = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	VkExtent2D extent {};
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
	std::vector<std::unique_ptr<VkImage_T>> imageStorage;
	std::vector<VkImage> images;
	std::vector<Lumen::MetalVulkan::SwapchainImageState> states;
	std::mutex mutex;
	std::condition_variable imageCondition;
	std::shared_ptr<Lumen::MetalVulkan::DrawableAcquisition> drawableRequest;
	uint32_t drawableRequestImageIndex = UINT32_MAX;
	uint32_t nextSearchIndex = 0;
	uint32_t presentationsInFlight = 0;
	bool retired = false;
	bool destroying = false;
};

struct VkCommandBuffer_T
{
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkCommandBufferUsageFlags usage = 0;
	Lumen::MetalVulkan::CommandBufferState state = Lumen::MetalVulkan::CommandBufferState::Initial;
	VkPipeline recordingPipeline = VK_NULL_HANDLE;
	VkBuffer recordingIndexBuffer = VK_NULL_HANDLE;
	VkDeviceSize recordingIndexOffset = 0;
	VkIndexType recordingIndexType = VK_INDEX_TYPE_UINT16;
	VkResult recordingError = VK_SUCCESS;
	const char* recordingErrorOperation = nullptr;
	std::shared_ptr<void> backendState;
	std::vector<Lumen::MetalVulkan::RecordedOperation> operations;
	std::mutex mutex;
	std::condition_variable completionCondition;
	uint32_t pendingCount = 0;
	std::atomic<bool> alive {true};
};

struct VkCommandPool_T
{
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queueFamilyIndex = 0;
	VkCommandPoolCreateFlags flags = 0;
	std::mutex mutex;
	std::unordered_set<VkCommandBuffer> commandBuffers;
	std::atomic<bool> alive {true};
};

struct VkFence_T
{
	VkDevice device = VK_NULL_HANDLE;
	std::mutex mutex;
	std::condition_variable condition;
	bool signaled = false;
	bool alive = true;
};

struct VkSemaphore_T
{
	VkDevice device = VK_NULL_HANDLE;
	VkSemaphoreType type = VK_SEMAPHORE_TYPE_BINARY;
	__strong id<MTLSharedEvent> sharedEvent = nil;
	std::mutex mutex;
	std::condition_variable condition;
	bool signaled = false;
	bool alive = true;
};

struct VkEvent_T
{
	VkDevice device = VK_NULL_HANDLE;
	// A GPU timeline may complete before Metal invokes its CPU callbacks.
	// Pending callbacks own this state independently of the Vulkan handle.
	std::shared_ptr<Lumen::MetalVulkan::EventState> state;
};

// Implemented by MetalVulkanCommands.mm.  The core resolver advertises only
// these real implementations; an absent operation must resolve to nullptr.
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule*);
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout*);
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool*);
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice, VkDescriptorPool, VkDescriptorPoolResetFlags);
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet*);
VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice, VkDescriptorPool, uint32_t, const VkDescriptorSet*);
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*);
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo*, const VkAllocationCallbacks*, VkRenderPass*);
VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo*, const VkAllocationCallbacks*, VkFramebuffer*);
VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*);
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*);
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents);
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer, const VkRenderingInfo*);
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer);
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo*);
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport*);
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer, uint32_t, const VkViewport*);
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer, uint32_t, const VkRect2D*);
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*, const VkDeviceSize*, const VkDeviceSize*);
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer, VkBool32);
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer, VkBool32);
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer, VkCompareOp);
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer, VkBool32);
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer, VkStencilFaceFlags, VkStencilOp, VkStencilOp, VkStencilOp, VkCompareOp);
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer, VkBool32);
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer, float, float, float);
VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer, VkCullModeFlags);
VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer, VkFrontFace);
VKAPI_ATTR void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer, VkBool32);
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer, VkBool32);
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer, const float[4]);
VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer, VkPrimitiveTopology);
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer, VkBool32);
VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize);
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer, VkBuffer, VkDeviceSize, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*);
VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*);
VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer*, const VkDeviceSize*);
VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkIndexType);
VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*);
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*);
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy*);
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*);
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter);
VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*);
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*);
VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer, VkEvent, VkPipelineStageFlags);
VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer, VkEvent, const VkDependencyInfo*);
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D*);
VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer, float);
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer, VkStencilFaceFlags, uint32_t);
VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteEnableEXT(VkCommandBuffer, uint32_t, const VkBool32*);
}

#endif // LUMEN_EMULATOR_GRAPHICS_METAL_VULKAN_INTERNAL_H_
