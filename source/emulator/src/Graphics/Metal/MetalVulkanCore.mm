#include "MetalVulkanInternal.h"
#include "MetalCullDistance.h"

#include <vulkan/vk_enum_string_helper.h>

#import <TargetConditionals.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <unordered_map>

namespace {

constexpr uint32_t kApiVersion = VK_API_VERSION_1_2;
constexpr uint32_t kQueueFamilyIndex = 0;
constexpr uint32_t kMaximumQueues = 11;
constexpr uint32_t kDefaultSwapchainImageCount = 3;
constexpr VkDeviceSize kResourceAlignment = 256;

constexpr VkMemoryPropertyFlags kSharedMemoryProperties =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

using PresentCompletionCallback = void (*)(void* context);

std::mutex                gPresentCompletionMutex;
PresentCompletionCallback gPresentCompletionCallback = nullptr;
void*                     gPresentCompletionContext = nullptr;

void NotifyPresentCompleted()
{
	PresentCompletionCallback callback = nullptr;
	void* context = nullptr;
	{
		std::lock_guard lock(gPresentCompletionMutex);
		callback = gPresentCompletionCallback;
		context = gPresentCompletionContext;
	}
	if (callback != nullptr)
	{
		callback(context);
	}
}

bool IsSupportedAllocator(const VkAllocationCallbacks* allocator)
{
	// Vulkan allocation callbacks cannot describe construction/destruction of
	// Objective-C++ objects.  Fail closed instead of mixing ownership domains.
	return allocator == nullptr;
}

bool InstanceExtensionSupported(const char* name)
{
	if (name == nullptr)
	{
		return false;
	}
	return std::strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0 ||
	       std::strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0 ||
	       std::strcmp(name, VK_EXT_METAL_SURFACE_EXTENSION_NAME) == 0 ||
#if TARGET_OS_IPHONE
	       std::strcmp(name, VK_MVK_IOS_SURFACE_EXTENSION_NAME) == 0;
#else
	       std::strcmp(name, VK_MVK_MACOS_SURFACE_EXTENSION_NAME) == 0;
#endif
}

bool DeviceExtensionSupported(const char* name)
{
	return name != nullptr &&
	       (std::strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0 ||
	        std::strcmp(name, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0 ||
	        std::strcmp(name, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME) == 0 ||
	        std::strcmp(name, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME) == 0 ||
	        std::strcmp(name, VK_KHR_MAINTENANCE1_EXTENSION_NAME) == 0 ||
	        std::strcmp(name, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) == 0 ||
	        std::strcmp(name, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0 ||
	        std::strcmp(name, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME) == 0 ||
	        std::strcmp(name, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) == 0);
}

VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
{
	return value > std::numeric_limits<VkDeviceSize>::max() - (alignment - 1)
	           ? std::numeric_limits<VkDeviceSize>::max()
	           : (value + alignment - 1) & ~(alignment - 1);
}

uint64_t DeviceWorkingSetSize(id<MTLDevice> device)
{
	uint64_t size = 512ULL * 1024ULL * 1024ULL;
#if TARGET_OS_IPHONE
	if (@available(iOS 16.0, *))
	{
		size = std::max(size, device.recommendedMaxWorkingSetSize);
	}
#else
	size = std::max(size, device.recommendedMaxWorkingSetSize);
#endif
	return size;
}

uint32_t BytesPerPixel(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8_UNORM: return 1;
		case VK_FORMAT_R8G8_UNORM:
		case VK_FORMAT_D16_UNORM: return 2;
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_D32_SFLOAT: return 4;
		case VK_FORMAT_D32_SFLOAT_S8_UINT: return 8;
		case VK_FORMAT_R32G32_SFLOAT: return 8;
		case VK_FORMAT_R32G32B32_SFLOAT: return 12;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
		case VK_FORMAT_R8_UINT: return 1;
		case VK_FORMAT_R8_SINT: return 1;
		case VK_FORMAT_R8_SNORM: return 1;
		case VK_FORMAT_R8G8_UINT: return 2;
		case VK_FORMAT_R8G8_SINT: return 2;
		case VK_FORMAT_R8G8_SNORM: return 2;
		case VK_FORMAT_R8G8B8A8_UINT: return 4;
		case VK_FORMAT_R8G8B8A8_SINT: return 4;
		case VK_FORMAT_R8G8B8A8_SNORM: return 4;
		case VK_FORMAT_R16_UINT: return 2;
		case VK_FORMAT_R16_SINT: return 2;
		case VK_FORMAT_R16_UNORM: return 2;
		case VK_FORMAT_R16_SNORM: return 2;
		case VK_FORMAT_R16_SFLOAT: return 2;
		case VK_FORMAT_R16G16_UINT: return 4;
		case VK_FORMAT_R16G16_SINT: return 4;
		case VK_FORMAT_R16G16_UNORM: return 4;
		case VK_FORMAT_R16G16_SNORM: return 4;
		case VK_FORMAT_R16G16_SFLOAT: return 4;
		case VK_FORMAT_R16G16B16A16_UINT: return 8;
		case VK_FORMAT_R16G16B16A16_SINT: return 8;
		case VK_FORMAT_R16G16B16A16_UNORM: return 8;
		case VK_FORMAT_R16G16B16A16_SNORM: return 8;
		case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
		case VK_FORMAT_R32_UINT: return 4;
		case VK_FORMAT_R32_SINT: return 4;
		case VK_FORMAT_R32_SFLOAT: return 4;
		case VK_FORMAT_R32G32_UINT: return 8;
		case VK_FORMAT_R32G32_SINT: return 8;
		case VK_FORMAT_R32G32B32A32_UINT: return 16;
		case VK_FORMAT_R32G32B32A32_SINT: return 16;
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
		case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 4;
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return 4;
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 4;
		case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return 4;
		case VK_FORMAT_S8_UINT: return 1;
		default: return 0;
	}
}

uint32_t BytesPerCompressedBlock(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
		case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
		case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
		case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
		case VK_FORMAT_BC4_UNORM_BLOCK:
		case VK_FORMAT_BC4_SNORM_BLOCK: return 8;
		case VK_FORMAT_BC2_UNORM_BLOCK:
		case VK_FORMAT_BC2_SRGB_BLOCK:
		case VK_FORMAT_BC3_UNORM_BLOCK:
		case VK_FORMAT_BC3_SRGB_BLOCK:
		case VK_FORMAT_BC5_UNORM_BLOCK:
		case VK_FORMAT_BC5_SNORM_BLOCK:
		case VK_FORMAT_BC6H_UFLOAT_BLOCK:
		case VK_FORMAT_BC6H_SFLOAT_BLOCK:
		case VK_FORMAT_BC7_UNORM_BLOCK:
		case VK_FORMAT_BC7_SRGB_BLOCK: return 16;
		default: return 0;
	}
}

MTLTextureSwizzle ToMTLTextureSwizzle(VkComponentSwizzle swizzle, MTLTextureSwizzle identity)
{
	switch (swizzle)
	{
		case VK_COMPONENT_SWIZZLE_IDENTITY: return identity;
		case VK_COMPONENT_SWIZZLE_ZERO: return MTLTextureSwizzleZero;
		case VK_COMPONENT_SWIZZLE_ONE: return MTLTextureSwizzleOne;
		case VK_COMPONENT_SWIZZLE_R: return MTLTextureSwizzleRed;
		case VK_COMPONENT_SWIZZLE_G: return MTLTextureSwizzleGreen;
		case VK_COMPONENT_SWIZZLE_B: return MTLTextureSwizzleBlue;
		case VK_COMPONENT_SWIZZLE_A: return MTLTextureSwizzleAlpha;
		default: return static_cast<MTLTextureSwizzle>(UINT8_MAX);
	}
}

bool IsIdentityMapping(const VkComponentMapping& mapping)
{
	const auto identity = [](VkComponentSwizzle actual, VkComponentSwizzle explicitIdentity)
	{
		return actual == VK_COMPONENT_SWIZZLE_IDENTITY || actual == explicitIdentity;
	};
	return identity(mapping.r, VK_COMPONENT_SWIZZLE_R) && identity(mapping.g, VK_COMPONENT_SWIZZLE_G) &&
	       identity(mapping.b, VK_COMPONENT_SWIZZLE_B) && identity(mapping.a, VK_COMPONENT_SWIZZLE_A);
}

bool BuildMetalImageView(VkImageView view)
{
	if (view == VK_NULL_HANDLE || view->image == VK_NULL_HANDLE || view->image->metalTexture == nil)
	{
		return false;
	}
	const bool stencilOnly = view->range.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT;
	const bool opaqueBc1 = view->format == VK_FORMAT_BC1_RGB_UNORM_BLOCK || view->format == VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    const MTLPixelFormat pixelFormat = stencilOnly && view->format == VK_FORMAT_D32_SFLOAT_S8_UINT
        ? MTLPixelFormatX32_Stencil8 : Lumen::MetalVulkan::ToMTLPixelFormat(view->format);
    const MTLTextureType viewType = view->image->samples != VK_SAMPLE_COUNT_1_BIT
        ? (view->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample)
        : Lumen::MetalVulkan::ToMTLTextureType(view->image->imageType, view->viewType, view->range.layerCount);
	if (pixelFormat == MTLPixelFormatInvalid)
	{
		return false;
	}
	const uint32_t baseLevel = view->range.baseMipLevel;
	const uint32_t baseLayer = view->range.baseArrayLayer;
	if (baseLevel >= view->image->mipLevels)
	{
		return false;
	}
	const bool volumeAttachment = view->image->imageType == VK_IMAGE_TYPE_3D &&
	    (view->viewType == VK_IMAGE_VIEW_TYPE_2D || view->viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
	const uint32_t availableLayers = volumeAttachment ? std::max(1u, view->image->extent.depth >> baseLevel)
	                                                  : view->image->arrayLayers;
	if (baseLayer >= availableLayers) return false;
	const uint32_t levelCount = view->range.levelCount == VK_REMAINING_MIP_LEVELS
	                                ? view->image->mipLevels - baseLevel
	                                : view->range.levelCount;
	const uint32_t layerCount = view->range.layerCount == VK_REMAINING_ARRAY_LAYERS
	                                ? availableLayers - baseLayer
	                                : view->range.layerCount;
	if (levelCount == 0 || layerCount == 0 || levelCount > view->image->mipLevels - baseLevel ||
	    layerCount > availableLayers - baseLayer)
	{
		return false;
	}
	if (volumeAttachment)
	{
		if ((view->image->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) == 0 || levelCount != 1 ||
		    (view->viewType == VK_IMAGE_VIEW_TYPE_2D && layerCount != 1) || !IsIdentityMapping(view->components))
			return false;
		view->volumeAttachment = true;
		view->range.levelCount = levelCount;
		view->range.layerCount = layerCount;
		view->metalTexture = pixelFormat == view->image->metalTexture.pixelFormat ? view->image->metalTexture :
		    [view->image->metalTexture newTextureViewWithPixelFormat:pixelFormat];
		return view->metalTexture != nil;
	}
	if (!opaqueBc1 && pixelFormat == view->image->metalTexture.pixelFormat && viewType == view->image->metalTexture.textureType && IsIdentityMapping(view->components) && baseLevel == 0 &&
	    levelCount == view->image->mipLevels && baseLayer == 0 && layerCount == view->image->arrayLayers)
	{
		view->metalTexture = view->image->metalTexture;
		return true;
	}
	const auto component = [opaqueBc1](VkComponentSwizzle channel, MTLTextureSwizzle identity) {
		const auto result = ToMTLTextureSwizzle(channel, identity);
		// BC1 RGB has an opaque alpha component, including when the Vulkan
		// swizzle selects that component for a different output channel.
		return opaqueBc1 && result == MTLTextureSwizzleAlpha ? MTLTextureSwizzleOne : result;
	};
	const MTLTextureSwizzleChannels swizzle = MTLTextureSwizzleChannelsMake(
	    component(view->components.r, MTLTextureSwizzleRed),
	    component(view->components.g, MTLTextureSwizzleGreen),
	    component(view->components.b, MTLTextureSwizzleBlue),
	    component(view->components.a, MTLTextureSwizzleAlpha));
	if (swizzle.red == static_cast<MTLTextureSwizzle>(UINT8_MAX) ||
	    swizzle.green == static_cast<MTLTextureSwizzle>(UINT8_MAX) ||
	    swizzle.blue == static_cast<MTLTextureSwizzle>(UINT8_MAX) ||
	    swizzle.alpha == static_cast<MTLTextureSwizzle>(UINT8_MAX))
	{
		return false;
	}
	view->metalTexture = [view->image->metalTexture
	    newTextureViewWithPixelFormat:pixelFormat
	                      textureType:viewType
	                           levels:NSMakeRange(baseLevel, levelCount)
	                           slices:NSMakeRange(baseLayer, layerCount)
	                          swizzle:swizzle];
	return view->metalTexture != nil;
}

VkDeviceSize ImageAllocationSize(const VkImage_T& image)
{
	VkDeviceSize total = 0;
	uint32_t width = image.extent.width;
	uint32_t height = image.extent.height;
	uint32_t depth = image.extent.depth;
	const uint32_t bytes = BytesPerPixel(image.format);
	const uint32_t blockBytes = BytesPerCompressedBlock(image.format);
	for (uint32_t level = 0; level < image.mipLevels; ++level)
	{
		VkDeviceSize levelSize = 0;
		if (blockBytes != 0)
		{
			levelSize = static_cast<VkDeviceSize>((width + 3) / 4) * ((height + 3) / 4) * depth * blockBytes;
		} else
		{
			levelSize = static_cast<VkDeviceSize>(width) * height * depth * bytes;
		}
		const VkDeviceSize multiplier = static_cast<VkDeviceSize>(image.arrayLayers) *
		                                static_cast<VkDeviceSize>(image.samples);
		if (multiplier == 0 || levelSize > std::numeric_limits<VkDeviceSize>::max() / multiplier)
		{
			return std::numeric_limits<VkDeviceSize>::max();
		}
		const VkDeviceSize completeLevelSize = levelSize * multiplier;
		if (total > std::numeric_limits<VkDeviceSize>::max() - completeLevelSize)
		{
			return std::numeric_limits<VkDeviceSize>::max();
		}
		total += completeLevelSize;
		width = std::max(1u, width >> 1);
		height = std::max(1u, height >> 1);
		depth = std::max(1u, depth >> 1);
	}
	return AlignUp(total, kResourceAlignment);
}

VkPhysicalDeviceFeatures SupportedFeatures(id<MTLDevice> device)
{
	VkPhysicalDeviceFeatures features {};
	features.fullDrawIndexUint32 = VK_TRUE;
	features.imageCubeArray = VK_TRUE;
	features.depthClamp = VK_TRUE;
	features.samplerAnisotropy = VK_TRUE;
#if TARGET_OS_IPHONE
	if (@available(iOS 16.4, *))
#else
	if (@available(macOS 11.0, *))
#endif
	{
		features.textureCompressionBC = device.supportsBCTextureCompression ? VK_TRUE : VK_FALSE;
	}
	features.fragmentStoresAndAtomics = VK_TRUE;
	features.vertexPipelineStoresAndAtomics = VK_TRUE;
	features.multiDrawIndirect = VK_TRUE;
	features.independentBlend = VK_TRUE;
	features.dualSrcBlend = VK_TRUE;
	features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
	features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
	features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
	features.shaderStorageImageExtendedFormats = VK_TRUE;
	features.shaderClipDistance = VK_TRUE;
	features.shaderCullDistance = Lumen::MetalVulkan::MetalCullDistanceSupported(device);
	// These types lower directly to MSL long/short/char storage and arithmetic;
	// 64-bit atomics remain separately unavailable.
	features.shaderInt64 = VK_TRUE;
	features.shaderInt16 = VK_TRUE;
	features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
	features.shaderImageGatherExtended = VK_TRUE;
	features.sampleRateShading = VK_TRUE;
	features.depthBiasClamp = VK_TRUE;
	features.multiViewport = VK_TRUE;
	features.tessellationShader = [device supportsFamily:MTLGPUFamilyApple1] ||
	                              [device supportsFamily:MTLGPUFamilyMac2];
	return features;
}

bool RequestedFeaturesSupported(const VkPhysicalDeviceFeatures* requested, id<MTLDevice> device)
{
	if (requested == nullptr)
	{
		return true;
	}
	const VkPhysicalDeviceFeatures supported = SupportedFeatures(device);
	const auto* want = reinterpret_cast<const VkBool32*>(requested);
	const auto* have = reinterpret_cast<const VkBool32*>(&supported);
	for (size_t index = 0; index < sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32); ++index)
	{
		if (want[index] != VK_FALSE && have[index] == VK_FALSE)
		{
			return false;
		}
	}
	return true;
}

id<MTLComputePipelineState> MakeAddressQueryPipeline(id<MTLDevice> device)
{
	// Metal 2.3 can materialize a device pointer in a shader. This keeps the
	// macOS 12 path independent of MTLBuffer.gpuAddress (introduced in macOS 13).
	NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
	                    "kernel void address_query(device const uchar* p [[buffer(0)]], "
	                    "device ulong* result [[buffer(1)]]) { result[0] = reinterpret_cast<ulong>(p); }";
	MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
	options.languageVersion = MTLLanguageVersion2_3;
	NSError* error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
	if (library == nil) return nil;
	id<MTLFunction> function = [library newFunctionWithName:@"address_query"];
	return function == nil ? nil : [device newComputePipelineStateWithFunction:function error:&error];
}

std::vector<VkExtensionProperties> InstanceExtensions()
{
	std::vector<VkExtensionProperties> extensions;
	auto add = [&extensions](const char* name, uint32_t version)
	{
		VkExtensionProperties extension {};
		std::strncpy(extension.extensionName, name, sizeof(extension.extensionName) - 1);
		extension.specVersion = version;
		extensions.push_back(extension);
	};
	add(VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_SURFACE_SPEC_VERSION);
	add(VK_EXT_METAL_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_SPEC_VERSION);
#if TARGET_OS_IPHONE
	add(VK_MVK_IOS_SURFACE_EXTENSION_NAME, VK_MVK_IOS_SURFACE_SPEC_VERSION);
#else
	add(VK_MVK_MACOS_SURFACE_EXTENSION_NAME, VK_MVK_MACOS_SURFACE_SPEC_VERSION);
#endif
	add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_SPEC_VERSION);
	return extensions;
}

std::vector<VkExtensionProperties> DeviceExtensions()
{
	std::vector<VkExtensionProperties> extensions;
	for (const auto* name: {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MAINTENANCE1_EXTENSION_NAME,
	                       VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	                       VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
	                       VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME,
	                       VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
	                       VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME})
	{
		VkExtensionProperties extension {};
		std::strncpy(extension.extensionName, name, sizeof(extension.extensionName) - 1);
		extension.specVersion = 1;
		extensions.push_back(extension);
	}
	return extensions;
}

VkExtent2D SurfaceExtent(VkSurfaceKHR surface)
{
	if (surface == VK_NULL_HANDLE || !surface->alive.load(std::memory_order_acquire) || surface->layer == nil)
	{
		return {};
	}
	const CGSize size = surface->layer.drawableSize;
	return {static_cast<uint32_t>(std::max(0.0, std::floor(size.width))),
	        static_cast<uint32_t>(std::max(0.0, std::floor(size.height)))};
}

bool IsSurfaceUsable(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
	return physicalDevice != VK_NULL_HANDLE && physicalDevice->metalDevice != nil &&
	       surface != VK_NULL_HANDLE && surface->alive.load(std::memory_order_acquire) && surface->layer != nil;
}

void ConfigureSurfaceLayer(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
	if (!IsSurfaceUsable(physicalDevice, surface))
	{
		return;
	}
	surface->layer.device = physicalDevice->metalDevice;
	surface->layer.framebufferOnly = NO;
	surface->layer.allowsNextDrawableTimeout = YES;
	surface->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	surface->layer.opaque = YES;
}

void ReleaseSwapchainDrawableLocked(VkSwapchainKHR swapchain, uint32_t index)
{
	VkImage image = swapchain->images[index];
	std::lock_guard imageLock(image->mutex);
	for (VkImageView view: image->views)
	{
		view->metalTexture = nil;
	}
	image->metalTexture = nil;
	image->drawable = nil;
	swapchain->states[index] = Lumen::MetalVulkan::SwapchainImageState::Available;
}

void CompletePresent(VkSwapchainKHR swapchain, uint32_t index)
{
	std::lock_guard lock(swapchain->mutex);
	if (index < swapchain->images.size())
	{
		ReleaseSwapchainDrawableLocked(swapchain, index);
	}
	if (swapchain->presentationsInFlight != 0)
	{
		--swapchain->presentationsInFlight;
	}
	swapchain->imageCondition.notify_all();
}

template <typename Predicate>
VkResult WaitWithNanosecondTimeout(std::condition_variable& condition, std::unique_lock<std::mutex>& lock,
	                              uint64_t timeout, Predicate predicate)
{
	if (predicate())
	{
		return VK_SUCCESS;
	}
	if (timeout == 0)
	{
		return VK_TIMEOUT;
	}
	if (timeout == UINT64_MAX)
	{
		condition.wait(lock, predicate);
		return VK_SUCCESS;
	}
	// Saturate very large finite deadlines instead of overflowing the signed
	// clock representation or accidentally treating a finite timeout as zero.
	const auto now = std::chrono::steady_clock::now();
	const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
	    std::chrono::steady_clock::time_point::max() - now).count();
	const auto deadline = timeout >= static_cast<uint64_t>(remaining) ? std::chrono::steady_clock::time_point::max() :
	    now + std::chrono::nanoseconds(static_cast<int64_t>(timeout));
	return condition.wait_until(lock, deadline, predicate) ? VK_SUCCESS : VK_TIMEOUT;
}

uint64_t RemainingTimeout(uint64_t timeout, std::chrono::steady_clock::time_point start)
{
	if (timeout == UINT64_MAX)
	{
		return UINT64_MAX;
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
	                         std::chrono::steady_clock::now() - start)
	                         .count();
	if (elapsed <= 0)
	{
		return timeout;
	}
	const uint64_t elapsedNanoseconds = static_cast<uint64_t>(elapsed);
	return elapsedNanoseconds >= timeout ? 0 : timeout - elapsedNanoseconds;
}

VkResult AcquireTimeoutResult(uint64_t timeout)
{
	return timeout == 0 ? VK_NOT_READY : VK_TIMEOUT;
}

void ObserveMetalCompletion(VkDevice device, id<MTLCommandBuffer> commandBuffer)
{
	if (device == VK_NULL_HANDLE || commandBuffer == nil || commandBuffer.status != MTLCommandBufferStatusError)
	{
		return;
	}
	Lumen::MetalVulkan::MarkDeviceLost(device);
	NSLog(@"Lumen Metal command buffer failed: %@", commandBuffer.error);
}

void ResetCommandRecordingState(VkCommandBuffer commandBuffer)
{
	commandBuffer->operations.clear();
	commandBuffer->backendState.reset();
	commandBuffer->recordingPipeline = VK_NULL_HANDLE;
	commandBuffer->recordingIndexBuffer = VK_NULL_HANDLE;
	commandBuffer->recordingIndexOffset = 0;
	commandBuffer->recordingIndexType = VK_INDEX_TYPE_UINT16;
	commandBuffer->recordingError = VK_SUCCESS;
	commandBuffer->recordingErrorOperation = nullptr;
}

PFN_vkVoidFunction ResolveFunction(const char* name);

bool IsGlobalFunction(const char* name)
{
	return name != nullptr &&
	       (std::strcmp(name, "vkGetInstanceProcAddr") == 0 || std::strcmp(name, "vkCreateInstance") == 0 ||
	        std::strcmp(name, "vkEnumerateInstanceVersion") == 0 ||
	        std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0 ||
	        std::strcmp(name, "vkEnumerateInstanceLayerProperties") == 0);
}

} // namespace

namespace Lumen::MetalVulkan {

VkDeviceAddress QueryBufferAddressFromShader(VkDevice device, VkBuffer buffer)
{
	if (device->addressQueryPipeline == nil) device->addressQueryPipeline = MakeAddressQueryPipeline(device->metalDevice);
	if (device->addressQueryQueue == nil) device->addressQueryQueue = [device->metalDevice newCommandQueue];
	if (device->addressQueryPipeline == nil || device->addressQueryQueue == nil) return 0;
	id<MTLBuffer> result = [device->metalDevice newBufferWithLength:sizeof(uint64_t) options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> command = [device->addressQueryQueue commandBuffer];
	id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
	if (result == nil || command == nil || encoder == nil) return 0;
	[encoder setComputePipelineState:device->addressQueryPipeline];
	[encoder setBuffer:buffer->metalBuffer offset:buffer->memoryOffset atIndex:0];
	[encoder setBuffer:result offset:0 atIndex:1];
	[encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
	[encoder endEncoding];
	[command commit];
	[command waitUntilCompleted];
	if (command.status != MTLCommandBufferStatusCompleted) return 0;
	VkDeviceAddress address{};
	std::memcpy(&address, result.contents, sizeof(address));
	return address;
}

MTLPixelFormat ToMTLPixelFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8_UINT: return MTLPixelFormatR8Uint;
		case VK_FORMAT_R8_SINT: return MTLPixelFormatR8Sint;
		case VK_FORMAT_R8_SNORM: return MTLPixelFormatR8Snorm;
		case VK_FORMAT_R8G8_UINT: return MTLPixelFormatRG8Uint;
		case VK_FORMAT_R8G8_SINT: return MTLPixelFormatRG8Sint;
		case VK_FORMAT_R8G8_SNORM: return MTLPixelFormatRG8Snorm;
		case VK_FORMAT_R8G8B8A8_UINT: return MTLPixelFormatRGBA8Uint;
		case VK_FORMAT_R8G8B8A8_SINT: return MTLPixelFormatRGBA8Sint;
		case VK_FORMAT_R8G8B8A8_SNORM: return MTLPixelFormatRGBA8Snorm;
		case VK_FORMAT_R16_UINT: return MTLPixelFormatR16Uint;
		case VK_FORMAT_R16_SINT: return MTLPixelFormatR16Sint;
		case VK_FORMAT_R16_UNORM: return MTLPixelFormatR16Unorm;
		case VK_FORMAT_R16_SNORM: return MTLPixelFormatR16Snorm;
		case VK_FORMAT_R16_SFLOAT: return MTLPixelFormatR16Float;
		case VK_FORMAT_R16G16_UINT: return MTLPixelFormatRG16Uint;
		case VK_FORMAT_R16G16_SINT: return MTLPixelFormatRG16Sint;
		case VK_FORMAT_R16G16_UNORM: return MTLPixelFormatRG16Unorm;
		case VK_FORMAT_R16G16_SNORM: return MTLPixelFormatRG16Snorm;
		case VK_FORMAT_R16G16_SFLOAT: return MTLPixelFormatRG16Float;
		case VK_FORMAT_R16G16B16A16_UINT: return MTLPixelFormatRGBA16Uint;
		case VK_FORMAT_R16G16B16A16_SINT: return MTLPixelFormatRGBA16Sint;
		case VK_FORMAT_R16G16B16A16_UNORM: return MTLPixelFormatRGBA16Unorm;
		case VK_FORMAT_R16G16B16A16_SNORM: return MTLPixelFormatRGBA16Snorm;
		case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
		case VK_FORMAT_R32_UINT: return MTLPixelFormatR32Uint;
		case VK_FORMAT_R32_SINT: return MTLPixelFormatR32Sint;
		case VK_FORMAT_R32_SFLOAT: return MTLPixelFormatR32Float;
		case VK_FORMAT_R32G32_UINT: return MTLPixelFormatRG32Uint;
		case VK_FORMAT_R32G32_SINT: return MTLPixelFormatRG32Sint;
		case VK_FORMAT_R32G32B32A32_UINT: return MTLPixelFormatRGBA32Uint;
		case VK_FORMAT_R32G32B32A32_SINT: return MTLPixelFormatRGBA32Sint;
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return MTLPixelFormatRGB10A2Unorm;
		case VK_FORMAT_A2B10G10R10_UINT_PACK32: return MTLPixelFormatRGB10A2Uint;
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return MTLPixelFormatBGR10A2Unorm;
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return MTLPixelFormatRG11B10Float;
		case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return MTLPixelFormatRGB9E5Float;
		case VK_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
		case VK_FORMAT_R8G8_UNORM: return MTLPixelFormatRG8Unorm;
		case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
		case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
		case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
		case VK_FORMAT_B8G8R8A8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
		case VK_FORMAT_R32G32_SFLOAT: return MTLPixelFormatRG32Float;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return MTLPixelFormatRGBA32Float;
		case VK_FORMAT_D16_UNORM: return MTLPixelFormatDepth16Unorm;
		case VK_FORMAT_D32_SFLOAT: return MTLPixelFormatDepth32Float;
		case VK_FORMAT_D32_SFLOAT_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
		case VK_FORMAT_S8_UINT: return MTLPixelFormatStencil8;
		case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
		case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
		case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
		case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
		case VK_FORMAT_BC2_UNORM_BLOCK:
		case VK_FORMAT_BC2_SRGB_BLOCK:
		case VK_FORMAT_BC3_UNORM_BLOCK:
		case VK_FORMAT_BC3_SRGB_BLOCK:
		case VK_FORMAT_BC4_UNORM_BLOCK:
		case VK_FORMAT_BC4_SNORM_BLOCK:
		case VK_FORMAT_BC5_UNORM_BLOCK:
		case VK_FORMAT_BC5_SNORM_BLOCK:
		case VK_FORMAT_BC6H_UFLOAT_BLOCK:
		case VK_FORMAT_BC6H_SFLOAT_BLOCK:
		case VK_FORMAT_BC7_UNORM_BLOCK:
		case VK_FORMAT_BC7_SRGB_BLOCK:
#if TARGET_OS_IPHONE
			if (@available(iOS 16.4, *))
#endif
			{
				switch (format)
				{
					case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return MTLPixelFormatBC1_RGBA;
					case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return MTLPixelFormatBC1_RGBA_sRGB;
					case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return MTLPixelFormatBC1_RGBA;
					case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return MTLPixelFormatBC1_RGBA_sRGB;
					case VK_FORMAT_BC2_UNORM_BLOCK: return MTLPixelFormatBC2_RGBA;
					case VK_FORMAT_BC2_SRGB_BLOCK: return MTLPixelFormatBC2_RGBA_sRGB;
					case VK_FORMAT_BC3_UNORM_BLOCK: return MTLPixelFormatBC3_RGBA;
					case VK_FORMAT_BC3_SRGB_BLOCK: return MTLPixelFormatBC3_RGBA_sRGB;
					case VK_FORMAT_BC4_UNORM_BLOCK: return MTLPixelFormatBC4_RUnorm;
					case VK_FORMAT_BC4_SNORM_BLOCK: return MTLPixelFormatBC4_RSnorm;
					case VK_FORMAT_BC5_UNORM_BLOCK: return MTLPixelFormatBC5_RGUnorm;
					case VK_FORMAT_BC5_SNORM_BLOCK: return MTLPixelFormatBC5_RGSnorm;
					case VK_FORMAT_BC6H_UFLOAT_BLOCK: return MTLPixelFormatBC6H_RGBUfloat;
					case VK_FORMAT_BC6H_SFLOAT_BLOCK: return MTLPixelFormatBC6H_RGBFloat;
					case VK_FORMAT_BC7_UNORM_BLOCK: return MTLPixelFormatBC7_RGBAUnorm;
					case VK_FORMAT_BC7_SRGB_BLOCK: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
					default: break;
				}
			}
			return MTLPixelFormatInvalid;
		default: return MTLPixelFormatInvalid;
	}
}

MTLTextureType ToMTLTextureType(VkImageType imageType, VkImageViewType viewType, uint32_t arrayLayers)
{
	switch (viewType)
	{
		case VK_IMAGE_VIEW_TYPE_1D: return MTLTextureType1D;
		case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return MTLTextureType1DArray;
		case VK_IMAGE_VIEW_TYPE_2D: return MTLTextureType2D;
		case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return MTLTextureType2DArray;
		case VK_IMAGE_VIEW_TYPE_3D: return MTLTextureType3D;
		case VK_IMAGE_VIEW_TYPE_CUBE: return MTLTextureTypeCube;
		case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return MTLTextureTypeCubeArray;
		default: break;
	}
	if (imageType == VK_IMAGE_TYPE_1D)
	{
		return arrayLayers > 1 ? MTLTextureType1DArray : MTLTextureType1D;
	}
	if (imageType == VK_IMAGE_TYPE_3D)
	{
		return MTLTextureType3D;
	}
	return arrayLayers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
}

MTLTextureUsage ToMTLTextureUsage(VkImageUsageFlags usage)
{
	MTLTextureUsage result = MTLTextureUsageUnknown;
	// vkCmdBlitImage is implemented by the Metal adapter as a render pass that
	// samples its transfer source.  Reflect that implementation detail in the
	// native usage contract even when the Vulkan image was not otherwise
	// declared as sampled (the VideoOut presentation image is such a source).
	if ((usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
	              VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) != 0)
	{
		result |= MTLTextureUsageShaderRead;
	}
	if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
	{
		result |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	}
	if ((usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0)
	{
		result |= MTLTextureUsageRenderTarget;
	}
	return result;
}

MTLStorageMode ToMTLStorageMode(VkMemoryPropertyFlags properties)
{
#if TARGET_OS_IPHONE
	return (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 ? MTLStorageModeShared : MTLStorageModePrivate;
#else
	return (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 ? MTLStorageModeShared : MTLStorageModePrivate;
#endif
}

MTLResourceOptions ToMTLResourceOptions(VkMemoryPropertyFlags properties)
{
	MTLResourceOptions options = static_cast<MTLResourceOptions>(ToMTLStorageMode(properties) << MTLResourceStorageModeShift);
#if !TARGET_OS_IPHONE
	if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
	{
		options |= (properties & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0 ? MTLResourceCPUCacheModeDefaultCache
		                                                                : MTLResourceCPUCacheModeWriteCombined;
	}
#endif
	return options;
}

VkFormatFeatureFlags FormatFeatures(VkFormat format, VkImageTiling tiling)
{
	// Linear Vulkan images require explicit row-pitch/mapping semantics.  This
	// foundation exposes buffers for CPU transfers and intentionally does not
	// claim linear-image support until that contract is implemented.
	if (tiling == VK_IMAGE_TILING_LINEAR)
	{
		return 0;
	}
	if (ToMTLPixelFormat(format) == MTLPixelFormatInvalid || format == VK_FORMAT_R32G32B32_SFLOAT)
	{
		return 0;
	}
	VkFormatFeatureFlags features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	const bool depth = format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ||
	                   format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_S8_UINT;
	const bool compressed = BytesPerCompressedBlock(format) != 0;
	const bool integerFormat =
        (format >= VK_FORMAT_R8_UINT && format <= VK_FORMAT_R8_SINT) ||
        (format >= VK_FORMAT_R8G8_UINT && format <= VK_FORMAT_R8G8_SINT) ||
        (format >= VK_FORMAT_R8G8B8A8_UINT && format <= VK_FORMAT_R8G8B8A8_SINT) ||
        (format >= VK_FORMAT_R16_UINT && format <= VK_FORMAT_R16_SINT) ||
        (format >= VK_FORMAT_R16G16_UINT && format <= VK_FORMAT_R16G16_SINT) ||
        (format >= VK_FORMAT_R16G16B16A16_UINT && format <= VK_FORMAT_R16G16B16A16_SINT) ||
        format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R32_SINT ||
        format == VK_FORMAT_R32G32_UINT || format == VK_FORMAT_R32G32_SINT ||
        format == VK_FORMAT_R32G32B32A32_UINT || format == VK_FORMAT_R32G32B32A32_SINT ||
        format == VK_FORMAT_A2B10G10R10_UINT_PACK32;
    const bool srgb = format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB ||
	                  format == VK_FORMAT_BC3_SRGB_BLOCK;
	if (depth)
	{
		features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	} else
	{
		features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (!integerFormat) features |= VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		// BC textures can be sampled by the blit renderer, but cannot be
		// compressed render targets or storage images.
		if (!compressed && !integerFormat) features |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
		if (!compressed)
		{
			features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
            if (!integerFormat) features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
			if (!srgb)
			{
				features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			}
		}
	}
	return features;
}

constexpr VkImageCreateFlags kSupportedImageCreateFlags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT |
    VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;

bool IsSupportedMutableImageViewPair(VkFormat imageFormat, VkFormat viewFormat)
{
    if (imageFormat == viewFormat) return true;
	if (BytesPerCompressedBlock(imageFormat) != 0 || BytesPerCompressedBlock(viewFormat) != 0)
	{
		// Only the two encodings in the same BC family share a Metal view
		// class. Equal block byte counts do not permit BC2/BC5/etc aliasing.
		return imageFormat >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && imageFormat <= VK_FORMAT_BC7_SRGB_BLOCK &&
		       viewFormat >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && viewFormat <= VK_FORMAT_BC7_SRGB_BLOCK &&
		       (imageFormat - VK_FORMAT_BC1_RGB_UNORM_BLOCK) / 2 == (viewFormat - VK_FORMAT_BC1_RGB_UNORM_BLOCK) / 2;
	}
    const auto depth = [](VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT; };
    if (depth(imageFormat) || depth(viewFormat)) return false;
    const auto bytes = BytesPerPixel(imageFormat);
    return bytes != 0 && bytes == BytesPerPixel(viewFormat);
}

bool ImageCreateFlagsSupported(VkFormat format, VkImageCreateFlags flags)
{
    return (flags & ~kSupportedImageCreateFlags) == 0 && ToMTLPixelFormat(format) != MTLPixelFormatInvalid;
}

bool ImageUsageSupported(VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage)
{
	const VkFormatFeatureFlags available = FormatFeatures(format, tiling);
	return available != 0 &&
	       ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0 || (available & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0) &&
	       ((usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0 || (available & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0) &&
	       ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ||
	        (available & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0) &&
	       ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0 ||
	        (available & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) &&
	       ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 || (available & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) != 0) &&
	       ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0 || (available & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0);
}

constexpr bool IsVertexBufferFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R32_SFLOAT:
		case VK_FORMAT_R32G32_SFLOAT:
		case VK_FORMAT_R32G32B32_SFLOAT:
		case VK_FORMAT_R32G32B32A32_SFLOAT:
		case VK_FORMAT_R8G8B8A8_UNORM:
#if !TARGET_OS_IPHONE
		case VK_FORMAT_R8G8B8A8_SNORM:
#endif
		case VK_FORMAT_R16G16_SINT: return true;
		case VK_FORMAT_R16G16B16A16_SINT: return true;
		default: return false;
	}
}

static_assert(IsVertexBufferFormat(VK_FORMAT_R16G16_SINT));
static_assert(IsVertexBufferFormat(VK_FORMAT_R16G16B16A16_SINT));
static_assert(IsVertexBufferFormat(VK_FORMAT_R32G32B32_SFLOAT));
#if !TARGET_OS_IPHONE
static_assert(IsVertexBufferFormat(VK_FORMAT_R8G8B8A8_SNORM));
#else
static_assert(!IsVertexBufferFormat(VK_FORMAT_R8G8B8A8_SNORM));
#endif
static_assert(!IsVertexBufferFormat(VK_FORMAT_R16G16_SNORM));
static_assert(!IsVertexBufferFormat(VK_FORMAT_R16G16_SSCALED));
static_assert(!IsVertexBufferFormat(VK_FORMAT_R16G16B16A16_SNORM));
static_assert(!IsVertexBufferFormat(VK_FORMAT_R16G16B16A16_SSCALED));

VkResult BeginRenderEncoding(EncodingContext& context, MTLRenderPassDescriptor* descriptor)
{
	if (context.renderEncoder != nil || descriptor == nil) return VK_ERROR_INITIALIZATION_FAILED;
	// A counted indirect draw can interrupt this logical pass for GPU argument
	// preparation. Metal only permits choosing a preserving store at that point
	// if the encoder was created with an unknown store action.
	MTLRenderPassDescriptor* deferred = [descriptor copy];
	for (NSUInteger index = 0; index < 8; ++index)
	{
		if (deferred.colorAttachments[index].texture != nil)
			deferred.colorAttachments[index].storeAction = MTLStoreActionUnknown;
	}
	if (deferred.depthAttachment.texture != nil) deferred.depthAttachment.storeAction = MTLStoreActionUnknown;
	if (deferred.stencilAttachment.texture != nil) deferred.stencilAttachment.storeAction = MTLStoreActionUnknown;
	context.renderEncoder = [context.commandBuffer renderCommandEncoderWithDescriptor:deferred];
	if (context.renderEncoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	context.retainedRenderDescriptor = descriptor;
	return VK_SUCCESS;
}

void EndRenderEncoding(EncodingContext& context, bool preserveAttachments)
{
	if (context.renderEncoder == nil) return;
	const auto descriptor = context.retainedRenderDescriptor;
	const auto storeAction = [preserveAttachments](MTLRenderPassAttachmentDescriptor* attachment)
	{
		if (!preserveAttachments) return attachment.storeAction;
		// Metal requires a resolve action while a resolve texture is attached.
		// Vulkan permits that target to change throughout the render pass; the
		// final encoder still resolves after every draw. Preserve the MSAA
		// source as well so a counted-draw split can resume with LOAD.
		return attachment.resolveTexture != nil ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore;
	};
	for (NSUInteger index = 0; index < 8; ++index)
	{
		const auto attachment = descriptor.colorAttachments[index];
		if (attachment.texture != nil)
			[context.renderEncoder setColorStoreAction:storeAction(attachment)
			                                  atIndex:index];
	}
	if (descriptor.depthAttachment.texture != nil)
		[context.renderEncoder setDepthStoreAction:storeAction(descriptor.depthAttachment)];
	if (descriptor.stencilAttachment.texture != nil)
		[context.renderEncoder setStencilStoreAction:storeAction(descriptor.stencilAttachment)];
	[context.renderEncoder endEncoding];
	context.renderEncoder = nil;
}

void EndEncoding(EncodingContext& context, bool preserveAttachments)
{
	EndRenderEncoding(context, preserveAttachments);
	context.logicalRenderPassActive = false;
	context.renderPassReusable = false;
	context.renderPassMergePending = false;
	context.renderPassContainsMergedDraws = false;
	context.retainedCompletionMarker = false;
	context.renderPassMergeAfterMarkerPending = false;
	context.renderPassMergedAfterMarker = false;
	context.retainedRenderPass = VK_NULL_HANDLE;
	context.retainedFramebuffer = VK_NULL_HANDLE;
	context.retainedRenderDescriptor = nil;
	if (context.computeEncoder != nil)
	{
		[context.computeEncoder endEncoding];
		context.computeEncoder = nil;
	}
	if (context.blitEncoder != nil)
	{
		[context.blitEncoder endEncoding];
		context.blitEncoder = nil;
	}
}

VkResult WaitSemaphore(VkSemaphore semaphore, uint64_t timeoutNanoseconds, bool consume)
{
	if (semaphore == VK_NULL_HANDLE)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::unique_lock lock(semaphore->mutex);
	const VkResult result = WaitWithNanosecondTimeout(semaphore->condition, lock, timeoutNanoseconds,
	                                                [semaphore] { return semaphore->signaled || !semaphore->alive; });
	if (result != VK_SUCCESS)
	{
		return result;
	}
	if (!semaphore->alive)
	{
		return VK_ERROR_DEVICE_LOST;
	}
	if (consume)
	{
		semaphore->signaled = false;
	}
	return VK_SUCCESS;
}

void SignalSemaphore(VkSemaphore semaphore)
{
	if (semaphore == VK_NULL_HANDLE)
	{
		return;
	}
	std::lock_guard lock(semaphore->mutex);
	if (semaphore->alive)
	{
		semaphore->signaled = true;
		semaphore->condition.notify_all();
	}
}

void NotifyTimelineSemaphore(VkSemaphore semaphore)
{
	// Use the same lock as the host wait predicate to avoid losing a completion
	// between reading the shared-event counter and entering the condition wait.
	std::lock_guard lock(semaphore->device->semaphoreCompletionMutex);
	semaphore->device->semaphoreCompletionCondition.notify_all();
}

void SignalFence(VkFence fence)
{
	if (fence == VK_NULL_HANDLE)
	{
		return;
	}
	const VkDevice device = fence->device;
	{
		std::lock_guard lock(fence->mutex);
		if (!fence->alive) return;
		fence->signaled = true;
		fence->condition.notify_all();
	}
	// The waiter takes device completion -> fence. Release the fence lock
	// before this handshake to avoid ABBA and close the scan-to-wait race.
	{
		std::lock_guard lock(device->fenceCompletionMutex);
	}
	device->fenceCompletionCondition.notify_all();
	{
		std::lock_guard lock(device->semaphoreCompletionMutex);
		device->semaphoreCompletionCondition.notify_all();
	}
}

void MarkDeviceLost(VkDevice device)
{
	if (device == VK_NULL_HANDLE) return;
	{
		std::lock_guard lock(device->fenceCompletionMutex);
		device->lost.store(true, std::memory_order_release);
	}
	// Wake every fence set, including fences submitted on a different queue
	// that will never receive their own successful completion notification.
	device->fenceCompletionCondition.notify_all();
}

void ResetFence(VkFence fence)
{
	if (fence == VK_NULL_HANDLE)
	{
		return;
	}
	std::lock_guard lock(fence->mutex);
	fence->signaled = false;
}

bool FenceIsSignaled(VkFence fence)
{
	if (fence == VK_NULL_HANDLE)
	{
		return false;
	}
	std::lock_guard lock(fence->mutex);
	return fence->alive && fence->signaled;
}

VkResult EncodeCommandBuffer(VkCommandBuffer commandBuffer, id<MTLCommandQueue> queue,
	                         id<MTLCommandBuffer>* encodedCommandBuffer, ReplayFailure* replayFailure)
{
	if (replayFailure != nullptr)
	{
		*replayFailure = {};
	}
	if (commandBuffer == VK_NULL_HANDLE || queue == nil || encodedCommandBuffer == nullptr)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::lock_guard lock(commandBuffer->mutex);
	if (!commandBuffer->alive.load(std::memory_order_acquire) ||
	    commandBuffer->state != CommandBufferState::Executable || commandBuffer->recordingError != VK_SUCCESS)
	{
		const VkResult result = commandBuffer->recordingError != VK_SUCCESS ? commandBuffer->recordingError
		                                                                   : VK_ERROR_INITIALIZATION_FAILED;
		if (replayFailure != nullptr)
		{
			replayFailure->operationIndex = commandBuffer->operations.size();
			replayFailure->operationName = commandBuffer->recordingErrorOperation != nullptr
			                                   ? commandBuffer->recordingErrorOperation
			                                   : "command-buffer-state";
			replayFailure->result = result;
			replayFailure->occurredDuringRecording = commandBuffer->recordingError != VK_SUCCESS;
		}
		return result;
	}

	EncodingContext context;
	context.commandBuffer = [queue commandBuffer];
	if (context.commandBuffer == nil)
	{
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	for (size_t operationIndex = 0; operationIndex < commandBuffer->operations.size(); ++operationIndex)
	{
		auto& operation = commandBuffer->operations[operationIndex];
		const VkResult result = operation.encode(context);
		if (result != VK_SUCCESS)
		{
			if (replayFailure != nullptr)
			{
				replayFailure->operationIndex = operationIndex;
				replayFailure->operationName = operation.name;
				replayFailure->result = result;
			}
			EndEncoding(context);
			return result;
		}
	}
	EndEncoding(context);
	ObserveCompletionPassReuse(context);
	commandBuffer->state = CommandBufferState::Pending;
	++commandBuffer->pendingCount;
	*encodedCommandBuffer = context.commandBuffer;
	return VK_SUCCESS;
}

void CompleteCommandBuffer(VkCommandBuffer commandBuffer)
{
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return;
	}
	std::lock_guard lock(commandBuffer->mutex);
	if (commandBuffer->pendingCount != 0)
	{
		--commandBuffer->pendingCount;
	}
	if (commandBuffer->pendingCount == 0 && commandBuffer->state == CommandBufferState::Pending)
	{
		commandBuffer->state = (commandBuffer->usage & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0
		                           ? CommandBufferState::Invalid
		                           : CommandBufferState::Executable;
	}
	commandBuffer->completionCondition.notify_all();
}

} // namespace Lumen::MetalVulkan

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* apiVersion)
{
	if (apiVersion == nullptr)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*apiVersion = kApiVersion;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* layerName, uint32_t* propertyCount,
	                                                                 VkExtensionProperties* properties)
{
	if (layerName != nullptr)
	{
		if (propertyCount != nullptr)
		{
			*propertyCount = 0;
		}
		return VK_ERROR_LAYER_NOT_PRESENT;
	}
	return Lumen::MetalVulkan::CopyEnumeration(InstanceExtensions(), propertyCount, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* propertyCount, VkLayerProperties* properties)
{
	const std::vector<VkLayerProperties> noLayers;
	return Lumen::MetalVulkan::CopyEnumeration(noLayers, propertyCount, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* createInfo,
	                                            const VkAllocationCallbacks* allocator, VkInstance* instance)
{
	if (createInfo == nullptr || instance == nullptr || createInfo->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
	    !IsSupportedAllocator(allocator))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*instance = VK_NULL_HANDLE;
	if (createInfo->enabledLayerCount != 0)
	{
		return VK_ERROR_LAYER_NOT_PRESENT;
	}
	for (uint32_t index = 0; index < createInfo->enabledExtensionCount; ++index)
	{
		if (createInfo->ppEnabledExtensionNames == nullptr ||
		    !InstanceExtensionSupported(createInfo->ppEnabledExtensionNames[index]))
		{
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}
	}
	if (createInfo->pApplicationInfo != nullptr && createInfo->pApplicationInfo->apiVersion > kApiVersion)
	{
		return VK_ERROR_INCOMPATIBLE_DRIVER;
	}

	auto created = std::unique_ptr<VkInstance_T>(new (std::nothrow) VkInstance_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	for (uint32_t index = 0; index < createInfo->enabledExtensionCount; ++index)
	{
		created->enabledExtensions.emplace(createInfo->ppEnabledExtensionNames[index]);
	}

	NSArray<id<MTLDevice>>* devices = nil;
#if TARGET_OS_IPHONE
	id<MTLDevice> systemDevice = MTLCreateSystemDefaultDevice();
	if (systemDevice != nil)
	{
		devices = @[systemDevice];
	}
#else
	devices = MTLCopyAllDevices();
#endif
	for (id<MTLDevice> device in devices)
	{
		auto physical = std::unique_ptr<VkPhysicalDevice_T>(new (std::nothrow) VkPhysicalDevice_T);
		if (!physical)
		{
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		physical->instance = created.get();
		physical->metalDevice = device;
		created->physicalDevices.push_back(std::move(physical));
	}
	if (created->physicalDevices.empty())
	{
		id<MTLDevice> defaultDevice = MTLCreateSystemDefaultDevice();
		if (defaultDevice == nil)
		{
			return VK_ERROR_INCOMPATIBLE_DRIVER;
		}
		auto physical = std::unique_ptr<VkPhysicalDevice_T>(new (std::nothrow) VkPhysicalDevice_T);
		if (!physical)
		{
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		physical->instance = created.get();
		physical->metalDevice = defaultDevice;
		created->physicalDevices.push_back(std::move(physical));
	}
	*instance = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
{
	if (instance == VK_NULL_HANDLE || !IsSupportedAllocator(allocator))
	{
		return;
	}
	{
		std::lock_guard lock(instance->mutex);
		instance->alive.store(false, std::memory_order_release);
		for (VkDebugUtilsMessengerEXT messenger: instance->debugMessengers)
		{
			delete messenger;
		}
		instance->debugMessengers.clear();
	}
	delete instance;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* physicalDeviceCount,
	                                                     VkPhysicalDevice* physicalDevices)
{
	if (instance == VK_NULL_HANDLE || !instance->alive.load(std::memory_order_acquire))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::vector<VkPhysicalDevice> values;
	values.reserve(instance->physicalDevices.size());
	for (const auto& physical: instance->physicalDevices)
	{
		values.push_back(physical.get());
	}
	return Lumen::MetalVulkan::CopyEnumeration(values, physicalDeviceCount, physicalDevices);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* features)
{
	if (physicalDevice != VK_NULL_HANDLE && features != nullptr)
	{
		*features = SupportedFeatures(physicalDevice->metalDevice);
	}
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* features)
{
	if (physicalDevice != VK_NULL_HANDLE && features != nullptr)
	{
		features->features = SupportedFeatures(physicalDevice->metalDevice);
		for (auto* next = static_cast<VkBaseOutStructure*>(features->pNext); next != nullptr; next = next->pNext)
		{
			if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
			{
				auto* feature = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(next);
				void* tail = feature->pNext;
				*feature = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, tail};
				feature->storageBuffer16BitAccess = VK_TRUE;
			}
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
			{
				auto* feature = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(next);
				void* tail = feature->pNext;
				*feature = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, tail};
				feature->timelineSemaphore = VK_TRUE;
                feature->drawIndirectCount = VK_TRUE;
				feature->bufferDeviceAddress = MakeAddressQueryPipeline(physicalDevice->metalDevice) != nil;
				feature->samplerMirrorClampToEdge = VK_TRUE;
				feature->storageBuffer8BitAccess = VK_TRUE;
				feature->shaderInt8 = VK_TRUE;
				feature->shaderFloat16 = VK_TRUE;
				feature->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
				feature->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
				feature->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
				feature->shaderOutputLayer = VK_TRUE;
				feature->shaderOutputViewportIndex = VK_TRUE;
			}
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
			{
				auto* feature = reinterpret_cast<VkPhysicalDeviceVulkan13Features*>(next);
				void* tail = feature->pNext;
				*feature = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, tail};
				feature->dynamicRendering = VK_TRUE;
				feature->synchronization2 = VK_TRUE;
				// Apple GPU texture reads return zero outside the view and writes
				// are discarded. This does not imply robust buffer pointer access.
				feature->robustImageAccess = [physicalDevice->metalDevice supportsFamily:MTLGPUFamilyApple1];
			}
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES)
				reinterpret_cast<VkPhysicalDeviceImageRobustnessFeatures*>(next)->robustImageAccess =
				    [physicalDevice->metalDevice supportsFamily:MTLGPUFamilyApple1];
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT)
				reinterpret_cast<VkPhysicalDeviceDepthClipControlFeaturesEXT*>(next)->depthClipControl = VK_TRUE;
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
			{
				reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(next)->timelineSemaphore = VK_TRUE;
			}
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)
				reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeatures*>(next)->dynamicRendering = VK_TRUE;
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES)
				reinterpret_cast<VkPhysicalDeviceSynchronization2Features*>(next)->synchronization2 = VK_TRUE;
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT)
				reinterpret_cast<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*>(next)->extendedDynamicState = VK_TRUE;
			else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT)
			{
				auto* feature = reinterpret_cast<VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*>(next);
				feature->extendedDynamicState2 = VK_TRUE;
				feature->extendedDynamicState2LogicOp = VK_FALSE;
				feature->extendedDynamicState2PatchControlPoints = VK_FALSE;
			}
		}
	}
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
	                                                    VkPhysicalDeviceProperties* properties)
{
	if (physicalDevice == VK_NULL_HANDLE || properties == nullptr)
	{
		return;
	}
	*properties = {};
	properties->apiVersion = kApiVersion;
	properties->driverVersion = VK_MAKE_VERSION(1, 0, 0);
	properties->vendorID = 0x106B;
	properties->deviceID = static_cast<uint32_t>(physicalDevice->metalDevice.registryID);
#if TARGET_OS_IPHONE
	properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
#else
	properties->deviceType = physicalDevice->metalDevice.isLowPower ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
	                                                               : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
#endif
	const char* name = physicalDevice->metalDevice.name.UTF8String;
	std::strncpy(properties->deviceName, name != nullptr ? name : "Apple Metal GPU",
	             sizeof(properties->deviceName) - 1);
	const uint64_t registry = physicalDevice->metalDevice.registryID;
	std::memcpy(properties->pipelineCacheUUID, &registry,
	            std::min(sizeof(registry), sizeof(properties->pipelineCacheUUID)));

	auto& limits = properties->limits;
	limits.maxImageDimension1D = 16384;
	limits.maxImageDimension2D = 16384;
	limits.maxImageDimension3D = 2048;
	limits.maxImageDimensionCube = 16384;
	limits.maxImageArrayLayers = 2048;
	limits.maxTexelBufferElements = 1u << 27;
	limits.maxUniformBufferRange = 1u << 30;
	limits.maxStorageBufferRange = 1u << 30;
	limits.maxPushConstantsSize = 4096;
	limits.maxMemoryAllocationCount = 4096;
	limits.maxSamplerAllocationCount = 4096;
	limits.bufferImageGranularity = kResourceAlignment;
	limits.maxBoundDescriptorSets = 8;
	limits.maxPerStageDescriptorSamplers = 16;
	limits.maxPerStageDescriptorUniformBuffers = 29;
	limits.maxPerStageDescriptorStorageBuffers = 29;
	limits.maxPerStageDescriptorSampledImages = 128;
	limits.maxPerStageDescriptorStorageImages = 8;
	limits.maxPerStageResources = 128;
	limits.maxDescriptorSetSamplers = 64;
	limits.maxDescriptorSetUniformBuffers = 124;
	limits.maxDescriptorSetStorageBuffers = 124;
	limits.maxDescriptorSetSampledImages = 128;
	limits.maxDescriptorSetStorageImages = 32;
	limits.maxVertexInputAttributes = 31;
	limits.maxVertexInputBindings = 31;
	limits.maxVertexInputAttributeOffset = 2047;
	limits.maxVertexInputBindingStride = 2048;
	limits.maxVertexOutputComponents = 124;
	limits.maxTessellationGenerationLevel = 64;
	limits.maxTessellationPatchSize = 32;
	limits.maxTessellationControlPerVertexInputComponents = 128;
	limits.maxTessellationControlPerVertexOutputComponents = 128;
	limits.maxTessellationControlPerPatchOutputComponents = 120;
	limits.maxTessellationControlTotalOutputComponents = 4096;
	limits.maxTessellationEvaluationInputComponents = 128;
	limits.maxTessellationEvaluationOutputComponents = 128;
	limits.maxClipDistances = 8;
	limits.maxCullDistances = 8;
	limits.maxCombinedClipAndCullDistances = 8;
	limits.maxFragmentInputComponents = 124;
	limits.maxFragmentOutputAttachments = 8;
	limits.maxFragmentDualSrcAttachments = 1;
	limits.maxFragmentCombinedOutputResources = 128;
	limits.maxComputeSharedMemorySize = 32768;
	limits.maxComputeWorkGroupCount[0] = limits.maxComputeWorkGroupCount[1] = limits.maxComputeWorkGroupCount[2] = 65535;
	limits.maxComputeWorkGroupInvocations = 1024;
	limits.maxComputeWorkGroupSize[0] = 1024;
	limits.maxComputeWorkGroupSize[1] = 1024;
	limits.maxComputeWorkGroupSize[2] = 64;
	limits.maxDrawIndexedIndexValue = UINT32_MAX;
	limits.maxDrawIndirectCount = UINT32_MAX;
	limits.maxSamplerLodBias = 16.0f;
	limits.maxSamplerAnisotropy = 16.0f;
	limits.maxViewports = 16;
	limits.maxViewportDimensions[0] = limits.maxViewportDimensions[1] = 16384;
	limits.viewportBoundsRange[0] = -32768.0f;
	limits.viewportBoundsRange[1] = 32767.0f;
	limits.minMemoryMapAlignment = 64;
	limits.minTexelBufferOffsetAlignment = kResourceAlignment;
	limits.minUniformBufferOffsetAlignment = kResourceAlignment;
	limits.minStorageBufferOffsetAlignment = 16;
	limits.maxFramebufferWidth = limits.maxFramebufferHeight = 16384;
	limits.maxFramebufferLayers = 2048;
	limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
	limits.framebufferDepthSampleCounts = limits.framebufferColorSampleCounts;
	limits.framebufferStencilSampleCounts = limits.framebufferColorSampleCounts;
	limits.framebufferNoAttachmentsSampleCounts = limits.framebufferColorSampleCounts;
	limits.maxColorAttachments = 8;
	limits.sampledImageColorSampleCounts = limits.framebufferColorSampleCounts;
	limits.sampledImageIntegerSampleCounts = limits.framebufferColorSampleCounts;
	limits.sampledImageDepthSampleCounts = limits.framebufferColorSampleCounts;
	limits.sampledImageStencilSampleCounts = limits.framebufferColorSampleCounts;
	limits.storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT;
	limits.maxSampleMaskWords = 1;
	limits.timestampComputeAndGraphics = VK_TRUE;
	limits.timestampPeriod = 1.0f;
	limits.pointSizeRange[0] = limits.lineWidthRange[0] = 1.0f;
	limits.pointSizeRange[1] = limits.lineWidthRange[1] = 1.0f;
	limits.pointSizeGranularity = limits.lineWidthGranularity = 1.0f;
	limits.standardSampleLocations = VK_TRUE;
	limits.optimalBufferCopyOffsetAlignment = 4;
	limits.optimalBufferCopyRowPitchAlignment = 256;
	limits.nonCoherentAtomSize = 64;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
	                                                               uint32_t* propertyCount,
	                                                               VkQueueFamilyProperties* properties)
{
	if (propertyCount == nullptr)
	{
		return;
	}
	if (physicalDevice == VK_NULL_HANDLE)
	{
		*propertyCount = 0;
		return;
	}
	if (properties == nullptr)
	{
		*propertyCount = 1;
		return;
	}
	if (*propertyCount != 0)
	{
		properties[0] = {};
		properties[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
		properties[0].queueCount = kMaximumQueues;
		properties[0].timestampValidBits = 64;
		properties[0].minImageTransferGranularity = {1, 1, 1};
		*propertyCount = 1;
	}
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
	                                                          VkPhysicalDeviceMemoryProperties* properties)
{
	if (physicalDevice == VK_NULL_HANDLE || properties == nullptr)
	{
		return;
	}
	*properties = {};
	properties->memoryHeapCount = 1;
	properties->memoryHeaps[0].size = DeviceWorkingSetSize(physicalDevice->metalDevice);
	properties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
	properties->memoryTypeCount = 2;
	properties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	properties->memoryTypes[0].heapIndex = 0;
	properties->memoryTypes[1].propertyFlags = kSharedMemoryProperties;
	properties->memoryTypes[1].heapIndex = 0;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
	                                                          VkFormatProperties* properties)
{
	if (physicalDevice == VK_NULL_HANDLE || properties == nullptr)
	{
		return;
	}
	properties->linearTilingFeatures = Lumen::MetalVulkan::FormatFeatures(format, VK_IMAGE_TILING_LINEAR);
	properties->optimalTilingFeatures = Lumen::MetalVulkan::FormatFeatures(format, VK_IMAGE_TILING_OPTIMAL);
	if (BytesPerCompressedBlock(format) != 0 &&
	    SupportedFeatures(physicalDevice->metalDevice).textureCompressionBC == VK_FALSE)
	{
		properties->linearTilingFeatures = 0;
		properties->optimalTilingFeatures = 0;
	}
	properties->bufferFeatures = properties->linearTilingFeatures |
	                             (Lumen::MetalVulkan::IsVertexBufferFormat(format) ? VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT : 0);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format,
	                                                                   VkImageType type, VkImageTiling tiling,
	                                                                   VkImageUsageFlags usage, VkImageCreateFlags flags,
	                                                                   VkImageFormatProperties* properties)
{
	if (physicalDevice == VK_NULL_HANDLE || properties == nullptr ||
	    (type != VK_IMAGE_TYPE_1D && type != VK_IMAGE_TYPE_2D && type != VK_IMAGE_TYPE_3D) ||
	    ((flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) != 0 && type != VK_IMAGE_TYPE_3D) ||
	    (type == VK_IMAGE_TYPE_3D && (Lumen::MetalVulkan::FormatFeatures(format, tiling) &
	                              VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) ||
	    (BytesPerCompressedBlock(format) != 0 && type != VK_IMAGE_TYPE_2D) ||
	    !Lumen::MetalVulkan::ImageCreateFlagsSupported(format, flags) ||
	    !Lumen::MetalVulkan::ImageUsageSupported(
        (flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) && format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM :
        (flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) && format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : format, tiling, usage))
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	if (BytesPerCompressedBlock(format) != 0 &&
	    SupportedFeatures(physicalDevice->metalDevice).textureCompressionBC == VK_FALSE)
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	*properties = {};
	properties->maxExtent = type == VK_IMAGE_TYPE_3D ? VkExtent3D {2048, 2048, 2048} : VkExtent3D {16384, 16384, 1};
	properties->maxMipLevels = type == VK_IMAGE_TYPE_3D ? 12 : 15;
	properties->maxArrayLayers = type == VK_IMAGE_TYPE_3D ? 1 : 2048;
	properties->sampleCounts = BytesPerCompressedBlock(format) != 0 || type != VK_IMAGE_TYPE_2D ? VK_SAMPLE_COUNT_1_BIT
	    : VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT;
	properties->maxResourceSize = DeviceWorkingSetSize(physicalDevice->metalDevice);
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                         VkPhysicalDeviceProperties2* properties)
{
	if (physicalDevice == VK_NULL_HANDLE || properties == nullptr) return;
	vkGetPhysicalDeviceProperties(physicalDevice, &properties->properties);
	const uint32_t subgroupSize = uint32_t(MakeAddressQueryPipeline(physicalDevice->metalDevice).threadExecutionWidth);
	// CompilerMSL lowers these SPIR-V operations to Metal simd_* intrinsics,
	// including ballot masks and AllEqual. Keep the Vulkan 1.1 aggregate and
	// standalone query identical; neither implies subgroup-size control.
	const VkSubgroupFeatureFlags subgroupOperations = VK_SUBGROUP_FEATURE_BASIC_BIT |
	    VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
	    VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
	    VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT;
	const VkShaderStageFlags subgroupStages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	for (auto* next = static_cast<VkBaseOutStructure*>(properties->pNext); next != nullptr; next = next->pNext)
	{
		if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES)
			reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreProperties*>(next)->maxTimelineSemaphoreValueDifference = UINT64_MAX;
		else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR)
			reinterpret_cast<VkPhysicalDevicePushDescriptorPropertiesKHR*>(next)->maxPushDescriptors = 128;
		else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES) {
			auto* extended = reinterpret_cast<VkPhysicalDeviceVulkan11Properties*>(next);
			extended->subgroupSize = subgroupSize;
			extended->subgroupSupportedStages = subgroupStages;
			extended->subgroupSupportedOperations = subgroupOperations;
			extended->subgroupQuadOperationsInAllStages = VK_FALSE;
			extended->maxMemoryAllocationSize = DeviceWorkingSetSize(physicalDevice->metalDevice);
		}
		else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES) {
			auto* extended = reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(next);
			extended->subgroupSize = subgroupSize;
			extended->supportedStages = subgroupStages;
			extended->supportedOperations = subgroupOperations;
			extended->quadOperationsInAllStages = VK_FALSE;
		}
		else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES) {
			auto* extended = reinterpret_cast<VkPhysicalDeviceSubgroupSizeControlProperties*>(next);
			extended->minSubgroupSize = subgroupSize;
			extended->maxSubgroupSize = subgroupSize;
			extended->maxComputeWorkgroupSubgroups = subgroupSize ? properties->properties.limits.maxComputeWorkGroupInvocations / subgroupSize : 0;
			extended->requiredSubgroupSizeStages = 0;
		}
		else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
			auto* extended = reinterpret_cast<VkPhysicalDeviceVulkan12Properties*>(next);
			std::strncpy(extended->driverName, "Lumen Native Metal", sizeof(extended->driverName) - 1);
			extended->maxTimelineSemaphoreValueDifference = UINT64_MAX;
		}
	}
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                                               VkPhysicalDeviceMemoryProperties2* properties)
{
	if (properties != nullptr) vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties->memoryProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format,
                                                               VkFormatProperties2* properties)
{
	if (properties == nullptr) return;
	vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties->formatProperties);
	for (auto* next = static_cast<VkBaseOutStructure*>(properties->pNext); next != nullptr; next = next->pNext)
	{
		if (next->sType != VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) continue;
		auto* extended = reinterpret_cast<VkFormatProperties3*>(next);
		extended->linearTilingFeatures = properties->formatProperties.linearTilingFeatures;
		extended->optimalTilingFeatures = properties->formatProperties.optimalTilingFeatures;
		extended->bufferFeatures = properties->formatProperties.bufferFeatures;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceImageFormatInfo2* info, VkImageFormatProperties2* properties)
{
	if (info == nullptr || properties == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
	return vkGetPhysicalDeviceImageFormatProperties(physicalDevice, info->format, info->type, info->tiling,
	                                               info->usage, info->flags, &properties->imageFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* layerName,
	                                                               uint32_t* propertyCount,
	                                                               VkExtensionProperties* properties)
{
	if (physicalDevice == VK_NULL_HANDLE)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (layerName != nullptr)
	{
		if (propertyCount != nullptr)
		{
			*propertyCount = 0;
		}
		return VK_ERROR_LAYER_NOT_PRESENT;
	}
	return Lumen::MetalVulkan::CopyEnumeration(DeviceExtensions(), propertyCount, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* createInfo,
	                                          const VkAllocationCallbacks* allocator, VkDevice* device)
{
	if (physicalDevice == VK_NULL_HANDLE || createInfo == nullptr || device == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || !IsSupportedAllocator(allocator))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*device = VK_NULL_HANDLE;
	if (createInfo->enabledLayerCount != 0)
	{
		return VK_ERROR_LAYER_NOT_PRESENT;
	}
	for (uint32_t index = 0; index < createInfo->enabledExtensionCount; ++index)
	{
		if (createInfo->ppEnabledExtensionNames == nullptr ||
		    !DeviceExtensionSupported(createInfo->ppEnabledExtensionNames[index]))
		{
			return VK_ERROR_EXTENSION_NOT_PRESENT;
		}
	}
	if (!RequestedFeaturesSupported(createInfo->pEnabledFeatures, physicalDevice->metalDevice) ||
	    createInfo->queueCreateInfoCount == 0 ||
	    createInfo->pQueueCreateInfos == nullptr)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

    for (auto* next = static_cast<const VkBaseInStructure*>(createInfo->pNext); next != nullptr; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            if (!RequestedFeaturesSupported(&reinterpret_cast<const VkPhysicalDeviceFeatures2*>(next)->features,
                                            physicalDevice->metalDevice)) return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            const auto* requested = reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(next);
            VkPhysicalDeviceVulkan11Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported};
            vkGetPhysicalDeviceFeatures2(physicalDevice, &query);
            const auto* values = &requested->storageBuffer16BitAccess;
            const auto* have = &supported.storageBuffer16BitAccess;
            constexpr auto count = (offsetof(VkPhysicalDeviceVulkan11Features, shaderDrawParameters) - offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess)) / sizeof(VkBool32) + 1;
            for (size_t i = 0; i < count; ++i) if (values[i] && !have[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            const auto* requested = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(next);
            VkPhysicalDeviceVulkan12Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported};
            vkGetPhysicalDeviceFeatures2(physicalDevice, &query);
            const auto* want = &requested->samplerMirrorClampToEdge;
            const auto* have = &supported.samplerMirrorClampToEdge;
            constexpr auto count = (offsetof(VkPhysicalDeviceVulkan12Features, subgroupBroadcastDynamicId) - offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge)) / sizeof(VkBool32) + 1;
            for (size_t i = 0; i < count; ++i) if (want[i] && !have[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT) {
            const auto* requested = reinterpret_cast<const VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*>(next);
            if (requested->extendedDynamicState2LogicOp || requested->extendedDynamicState2PatchControlPoints) return VK_ERROR_FEATURE_NOT_PRESENT;
		} else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES) {
			const auto* requested = reinterpret_cast<const VkPhysicalDeviceImageRobustnessFeatures*>(next);
			if (requested->robustImageAccess && ![physicalDevice->metalDevice supportsFamily:MTLGPUFamilyApple1])
				return VK_ERROR_FEATURE_NOT_PRESENT;
		} else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
			const auto* requested = reinterpret_cast<const VkPhysicalDeviceVulkan13Features*>(next);
			VkPhysicalDeviceVulkan13Features supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
			VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &supported};
			vkGetPhysicalDeviceFeatures2(physicalDevice, &query);
			const auto* want = &requested->robustImageAccess;
			const auto* have = &supported.robustImageAccess;
			constexpr auto count = (offsetof(VkPhysicalDeviceVulkan13Features, maintenance4) - offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess)) / sizeof(VkBool32) + 1;
			for (size_t i = 0; i < count; ++i) if (want[i] && !have[i]) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }

	uint32_t queueCount = 0;
	for (uint32_t index = 0; index < createInfo->queueCreateInfoCount; ++index)
	{
		const auto& request = createInfo->pQueueCreateInfos[index];
		if (request.sType != VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || request.queueFamilyIndex != kQueueFamilyIndex ||
		    request.queueCount == 0 || request.pQueuePriorities == nullptr || queueCount + request.queueCount > kMaximumQueues)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		queueCount += request.queueCount;
	}

	auto created = std::unique_ptr<VkDevice_T>(new (std::nothrow) VkDevice_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->physicalDevice = physicalDevice;
	created->metalDevice = physicalDevice->metalDevice;
	for (uint32_t index = 0; index < queueCount; ++index)
	{
		auto queue = std::unique_ptr<VkQueue_T>(new (std::nothrow) VkQueue_T);
		if (!queue)
		{
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		queue->device = created.get();
		queue->familyIndex = kQueueFamilyIndex;
		queue->queueIndex = index;
		queue->metalQueue = [created->metalDevice newCommandQueue];
		if (queue->metalQueue == nil)
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
		created->queues.push_back(std::move(queue));
	}
	*device = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
	if (device == VK_NULL_HANDLE || !IsSupportedAllocator(allocator))
	{
		return;
	}
	vkDeviceWaitIdle(device);
	Lumen::MetalVulkan::FinalizeMetalShaderFunctionCache();
	Lumen::MetalVulkan::FinalizeMetalDepthStencilStateCache();
	Lumen::MetalVulkan::FinalizeMetalStateOnlyRecordElision();
	Lumen::MetalVulkan::FinalizeCompletionPassReuse();
	device->alive.store(false, std::memory_order_release);
	for (auto& queue: device->queues)
	{
		std::lock_guard lock(queue->mutex);
		queue->shuttingDown = true;
		queue->idleCondition.notify_all();
		queue->metalQueue = nil;
	}
	delete device;
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* queue)
{
	if (queue == nullptr)
	{
		return;
	}
	*queue = VK_NULL_HANDLE;
	if (device != VK_NULL_HANDLE && device->alive.load(std::memory_order_acquire) &&
	    queueFamilyIndex == kQueueFamilyIndex && queueIndex < device->queues.size())
	{
		*queue = device->queues[queueIndex].get();
	}
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name)
{
	return device == VK_NULL_HANDLE || !device->alive.load(std::memory_order_acquire) ? nullptr : ResolveFunction(name);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo* createInfo,
	                                                  const VkAllocationCallbacks* allocator, VkPipelineCache* cache)
{
	if (cache != nullptr) *cache = VK_NULL_HANDLE;
	if (device == VK_NULL_HANDLE || !device->alive.load(std::memory_order_acquire) || createInfo == nullptr ||
	    cache == nullptr || createInfo->sType != VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO ||
	    createInfo->pNext != nullptr || createInfo->flags != 0 || !IsSupportedAllocator(allocator) ||
	    (createInfo->initialDataSize != 0 && createInfo->pInitialData == nullptr))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	auto created = std::unique_ptr<VkPipelineCache_T>(new (std::nothrow) VkPipelineCache_T);
	if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->device = device;
	VkPhysicalDeviceProperties properties {};
	vkGetPhysicalDeviceProperties(device->physicalDevice, &properties);
	auto writeWord = [&](size_t offset, uint32_t value) {
		for (size_t byte = 0; byte < sizeof(value); ++byte)
			created->header[offset + byte] = static_cast<uint8_t>(value >> (8 * byte));
	};
	writeWord(0, static_cast<uint32_t>(created->header.size()));
	writeWord(4, VK_PIPELINE_CACHE_HEADER_VERSION_ONE);
	writeWord(8, properties.vendorID);
	writeWord(12, properties.deviceID);
	std::memcpy(created->header.data() + 16, properties.pipelineCacheUUID, VK_UUID_SIZE);
	// No Metal binary payload is imported or exported. Compatible initial data
	// represents the same empty cache; incompatible data also starts empty.
	*cache = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache cache,
	                                               const VkAllocationCallbacks* allocator)
{
	if (cache == VK_NULL_HANDLE || cache->device != device || !IsSupportedAllocator(allocator)) return;
	delete cache;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache cache,
	                                                  size_t* dataSize, void* data)
{
	if (device == VK_NULL_HANDLE || cache == VK_NULL_HANDLE || cache->device != device || dataSize == nullptr)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (data == nullptr)
	{
		*dataSize = cache->header.size();
		return VK_SUCCESS;
	}
	// Vulkan requires a complete header; a shorter buffer receives no bytes.
	if (*dataSize < cache->header.size())
	{
		*dataSize = 0;
		return VK_INCOMPLETE;
	}
	std::memcpy(data, cache->header.data(), cache->header.size());
	*dataSize = cache->header.size();
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice device, VkPipelineCache destination,
	                                                  uint32_t sourceCount, const VkPipelineCache* sources)
{
	if (device == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || destination->device != device ||
	    (sourceCount != 0 && sources == nullptr))
		return VK_ERROR_INITIALIZATION_FAILED;
	for (uint32_t index = 0; index < sourceCount; ++index)
		if (sources[index] == VK_NULL_HANDLE || sources[index] == destination || sources[index]->device != device)
			return VK_ERROR_INITIALIZATION_FAILED;
	// All caches contain only their identical device header; there is no
	// serialized Metal payload to merge or mutable state to synchronize.
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDebugUtilsMessengerEXT(VkInstance instance,
	                                                          const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
	                                                          const VkAllocationCallbacks* allocator,
	                                                          VkDebugUtilsMessengerEXT* messenger)
{
	if (instance == VK_NULL_HANDLE || createInfo == nullptr || messenger == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT ||
	    createInfo->pfnUserCallback == nullptr || !IsSupportedAllocator(allocator) ||
	    instance->enabledExtensions.count(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
	{
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	auto created = std::unique_ptr<VkDebugUtilsMessengerEXT_T>(new (std::nothrow) VkDebugUtilsMessengerEXT_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->instance = instance;
	created->callback = createInfo->pfnUserCallback;
	created->userData = createInfo->pUserData;
	created->severity = createInfo->messageSeverity;
	created->types = createInfo->messageType;
	{
		std::lock_guard lock(instance->mutex);
		instance->debugMessengers.insert(created.get());
	}
	*messenger = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger,
	                                                       const VkAllocationCallbacks* allocator)
{
	if (instance == VK_NULL_HANDLE || messenger == VK_NULL_HANDLE || !IsSupportedAllocator(allocator))
	{
		return;
	}
	std::lock_guard lock(instance->mutex);
	if (instance->debugMessengers.erase(messenger) != 0)
	{
		delete messenger;
	}
}

static VkResult CreateSurface(VkInstance instance, const void* view, const VkAllocationCallbacks* allocator,
	                         VkSurfaceKHR* surface)
{
	if (instance == VK_NULL_HANDLE || view == nullptr || surface == nullptr || !IsSupportedAllocator(allocator))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*surface = VK_NULL_HANDLE;
	auto created = std::unique_ptr<VkSurfaceKHR_T>(new (std::nothrow) VkSurfaceKHR_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	id metalView = (__bridge id)view;
	CALayer* viewLayer = [metalView respondsToSelector:@selector(layer)] ? [metalView layer] : nil;
	CAMetalLayer* layer = [viewLayer isKindOfClass:CAMetalLayer.class] ? static_cast<CAMetalLayer*>(viewLayer) : nil;
	if (layer == nil)
	{
		return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
	}
	created->instance = instance;
	created->metalView = metalView;
	created->layer = layer;
	*surface = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateMacOSSurfaceMVK(VkInstance instance,
	                                                  const VkMacOSSurfaceCreateInfoMVK* createInfo,
	                                                  const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface)
{
#if TARGET_OS_IPHONE
	(void)instance; (void)createInfo; (void)allocator; (void)surface;
	return VK_ERROR_EXTENSION_NOT_PRESENT;
#else
	if (createInfo == nullptr || createInfo->sType != VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK ||
	    instance == VK_NULL_HANDLE || instance->enabledExtensions.count(VK_MVK_MACOS_SURFACE_EXTENSION_NAME) == 0)
	{
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	return CreateSurface(instance, createInfo->pView, allocator, surface);
#endif
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateMetalSurfaceEXT(VkInstance instance,
	                                                  const VkMetalSurfaceCreateInfoEXT* createInfo,
	                                                  const VkAllocationCallbacks* allocator,
	                                                  VkSurfaceKHR* surface)
{
	if (surface != nullptr) *surface = VK_NULL_HANDLE;
	if (instance == VK_NULL_HANDLE || createInfo == nullptr || surface == nullptr ||
	    !IsSupportedAllocator(allocator) || createInfo->sType != VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT ||
	    createInfo->flags != 0 || createInfo->pLayer == nullptr ||
	    instance->enabledExtensions.count(VK_EXT_METAL_SURFACE_EXTENSION_NAME) == 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	CAMetalLayer* layer = const_cast<CAMetalLayer*>(createInfo->pLayer);
	if (![layer isKindOfClass:CAMetalLayer.class]) return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
	auto created = std::unique_ptr<VkSurfaceKHR_T>(new (std::nothrow) VkSurfaceKHR_T);
	if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->instance = instance;
	created->layer = layer;
	*surface = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateIOSSurfaceMVK(VkInstance instance,
	                                                const VkIOSSurfaceCreateInfoMVK* createInfo,
	                                                const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface)
{
#if TARGET_OS_IPHONE
	if (createInfo == nullptr || createInfo->sType != VK_STRUCTURE_TYPE_IOS_SURFACE_CREATE_INFO_MVK ||
	    instance == VK_NULL_HANDLE || instance->enabledExtensions.count(VK_MVK_IOS_SURFACE_EXTENSION_NAME) == 0)
	{
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
	return CreateSurface(instance, createInfo->pView, allocator, surface);
#else
	(void)instance; (void)createInfo; (void)allocator; (void)surface;
	return VK_ERROR_EXTENSION_NOT_PRESENT;
#endif
}

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface,
	                                          const VkAllocationCallbacks* allocator)
{
	if (surface == VK_NULL_HANDLE || !IsSupportedAllocator(allocator) ||
	    (instance != VK_NULL_HANDLE && surface->instance != instance))
	{
		return;
	}
	surface->alive.store(false, std::memory_order_release);
	surface->layer = nil;
	surface->metalView = nil;
	delete surface;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
	                                                               uint32_t queueFamilyIndex, VkSurfaceKHR surface,
	                                                               VkBool32* supported)
{
	if (supported == nullptr || physicalDevice == VK_NULL_HANDLE || surface == VK_NULL_HANDLE)
	{
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	*supported = queueFamilyIndex == kQueueFamilyIndex && IsSurfaceUsable(physicalDevice, surface) ? VK_TRUE : VK_FALSE;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice,
	                                                                    VkSurfaceKHR surface,
	                                                                    VkSurfaceCapabilitiesKHR* capabilities)
{
	if (!IsSurfaceUsable(physicalDevice, surface) || capabilities == nullptr)
	{
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	ConfigureSurfaceLayer(physicalDevice, surface);
	*capabilities = {};
	capabilities->minImageCount = 2;
	capabilities->maxImageCount = 3;
	capabilities->currentExtent = SurfaceExtent(surface);
	if (capabilities->currentExtent.width == 0 || capabilities->currentExtent.height == 0)
	{
		capabilities->currentExtent = {UINT32_MAX, UINT32_MAX};
	}
	capabilities->minImageExtent = {1, 1};
	capabilities->maxImageExtent = {16384, 16384};
	capabilities->maxImageArrayLayers = 1;
	capabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	capabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	capabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
	                                                               uint32_t* surfaceFormatCount,
	                                                               VkSurfaceFormatKHR* surfaceFormats)
{
	if (!IsSurfaceUsable(physicalDevice, surface))
	{
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	const std::vector<VkSurfaceFormatKHR> formats = {
	    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
	};
	return Lumen::MetalVulkan::CopyEnumeration(formats, surfaceFormatCount, surfaceFormats);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
	                                                                    VkSurfaceKHR surface,
	                                                                    uint32_t* presentModeCount,
	                                                                    VkPresentModeKHR* presentModes)
{
	if (!IsSurfaceUsable(physicalDevice, surface))
	{
		return VK_ERROR_SURFACE_LOST_KHR;
	}
#if TARGET_OS_IPHONE
	const std::vector<VkPresentModeKHR> modes = {VK_PRESENT_MODE_FIFO_KHR};
#else
	const std::vector<VkPresentModeKHR> modes = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
#endif
	return Lumen::MetalVulkan::CopyEnumeration(modes, presentModeCount, presentModes);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo,
	                                                const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || swapchain == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR || !IsSupportedAllocator(allocator) ||
	    createInfo->surface == VK_NULL_HANDLE || !createInfo->surface->alive.load(std::memory_order_acquire))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*swapchain = VK_NULL_HANDLE;
	if (createInfo->oldSwapchain != VK_NULL_HANDLE && createInfo->oldSwapchain->device != device)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (createInfo->imageFormat != VK_FORMAT_B8G8R8A8_UNORM && createInfo->imageFormat != VK_FORMAT_B8G8R8A8_SRGB)
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
#if TARGET_OS_IPHONE
	const bool presentModeSupported = createInfo->presentMode == VK_PRESENT_MODE_FIFO_KHR;
#else
	const bool presentModeSupported = createInfo->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ||
	                                  createInfo->presentMode == VK_PRESENT_MODE_FIFO_KHR;
#endif
	if (createInfo->imageExtent.width == 0 || createInfo->imageExtent.height == 0 || createInfo->imageArrayLayers != 1 ||
	    !presentModeSupported || createInfo->minImageCount < 2 || createInfo->minImageCount > 3)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	ConfigureSurfaceLayer(device->physicalDevice, createInfo->surface);
	CAMetalLayer* layer = createInfo->surface->layer;
	layer.pixelFormat = Lumen::MetalVulkan::ToMTLPixelFormat(createInfo->imageFormat);
	layer.drawableSize = CGSizeMake(createInfo->imageExtent.width, createInfo->imageExtent.height);
	layer.maximumDrawableCount = std::clamp(createInfo->minImageCount, 2u, 3u);
	layer.framebufferOnly = (createInfo->imageUsage & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) == 0;
#if !TARGET_OS_IPHONE
	layer.displaySyncEnabled = createInfo->presentMode != VK_PRESENT_MODE_IMMEDIATE_KHR;
#endif

	auto created = std::unique_ptr<VkSwapchainKHR_T>(new (std::nothrow) VkSwapchainKHR_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->surface = createInfo->surface;
	created->format = createInfo->imageFormat;
	created->colorSpace = createInfo->imageColorSpace;
	created->extent = createInfo->imageExtent;
	created->presentMode = createInfo->presentMode;
	const uint32_t imageCount = std::clamp(createInfo->minImageCount, 2u, kDefaultSwapchainImageCount);
	created->states.assign(imageCount, Lumen::MetalVulkan::SwapchainImageState::Available);
	for (uint32_t index = 0; index < imageCount; ++index)
	{
		auto image = std::unique_ptr<VkImage_T>(new (std::nothrow) VkImage_T);
		if (!image)
		{
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		image->device = device;
		image->imageType = VK_IMAGE_TYPE_2D;
		image->format = createInfo->imageFormat;
		image->extent = {createInfo->imageExtent.width, createInfo->imageExtent.height, 1};
		image->usage = createInfo->imageUsage;
		image->ownerSwapchain = created.get();
		image->swapchainIndex = index;
		created->images.push_back(image.get());
		created->imageStorage.push_back(std::move(image));
	}
	if (createInfo->oldSwapchain != VK_NULL_HANDLE)
	{
		std::lock_guard oldLock(createInfo->oldSwapchain->mutex);
		createInfo->oldSwapchain->retired = true;
		createInfo->oldSwapchain->imageCondition.notify_all();
	}
	*swapchain = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
	                                            const VkAllocationCallbacks* allocator)
{
	if (swapchain == VK_NULL_HANDLE || !IsSupportedAllocator(allocator) || swapchain->device != device)
	{
		return;
	}
	std::unique_lock lock(swapchain->mutex);
	swapchain->destroying = true;
	swapchain->retired = true;
	swapchain->drawableRequest.reset();
	swapchain->drawableRequestImageIndex = UINT32_MAX;
	swapchain->imageCondition.notify_all();
	swapchain->imageCondition.wait(lock, [swapchain] { return swapchain->presentationsInFlight == 0; });
	for (uint32_t index = 0; index < swapchain->images.size(); ++index)
	{
		ReleaseSwapchainDrawableLocked(swapchain, index);
	}
	lock.unlock();
	delete swapchain;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
	                                                  uint32_t* swapchainImageCount, VkImage* swapchainImages)
{
	if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE || swapchain->device != device)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return Lumen::MetalVulkan::CopyEnumeration(swapchain->images, swapchainImageCount, swapchainImages);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
	                                                VkSemaphore semaphore, VkFence fence, uint32_t* imageIndex)
{
	if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE || imageIndex == nullptr || swapchain->device != device ||
	    (semaphore == VK_NULL_HANDLE && fence == VK_NULL_HANDLE) ||
	    (semaphore != VK_NULL_HANDLE && semaphore->device != device) ||
	    (fence != VK_NULL_HANDLE && fence->device != device))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (device->lost.load(std::memory_order_acquire))
	{
		return VK_ERROR_DEVICE_LOST;
	}
	const auto acquireStart = std::chrono::steady_clock::now();
	std::unique_lock lock(swapchain->mutex);
	auto available = [swapchain]
	{
		return swapchain->retired || swapchain->destroying || swapchain->drawableRequest != nullptr ||
		       std::find(swapchain->states.begin(), swapchain->states.end(),
		                 Lumen::MetalVulkan::SwapchainImageState::Available) != swapchain->states.end();
	};
	const VkResult waitResult = WaitWithNanosecondTimeout(swapchain->imageCondition, lock,
	                                                    RemainingTimeout(timeout, acquireStart), available);
	if (waitResult != VK_SUCCESS)
	{
		return AcquireTimeoutResult(timeout);
	}
	if (swapchain->retired || swapchain->destroying || !swapchain->surface->alive.load(std::memory_order_acquire))
	{
		return VK_ERROR_OUT_OF_DATE_KHR;
	}

	uint32_t selected = swapchain->drawableRequestImageIndex;
	auto request = swapchain->drawableRequest;
	if (request == nullptr)
	{
		selected = UINT32_MAX;
		for (uint32_t offset = 0; offset < swapchain->states.size(); ++offset)
		{
			const uint32_t index = (swapchain->nextSearchIndex + offset) % swapchain->states.size();
			if (swapchain->states[index] == Lumen::MetalVulkan::SwapchainImageState::Available)
			{
				selected = index;
				break;
			}
		}
		if (selected == UINT32_MAX)
		{
			return VK_NOT_READY;
		}
		request = std::make_shared<Lumen::MetalVulkan::DrawableAcquisition>();
		request->layer = swapchain->surface->layer;
		if (request->layer == nil)
		{
			return VK_ERROR_OUT_OF_DATE_KHR;
		}
		swapchain->states[selected] = Lumen::MetalVulkan::SwapchainImageState::Acquiring;
		swapchain->drawableRequest = request;
		swapchain->drawableRequestImageIndex = selected;
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
			@autoreleasepool
			{
				id<CAMetalDrawable> drawable = [request->layer nextDrawable];
				std::lock_guard requestLock(request->mutex);
				request->drawable = drawable;
				request->complete = true;
				request->condition.notify_all();
			}
		});
	}
	if (selected >= swapchain->states.size() ||
	    swapchain->states[selected] != Lumen::MetalVulkan::SwapchainImageState::Acquiring)
	{
		swapchain->drawableRequest.reset();
		swapchain->drawableRequestImageIndex = UINT32_MAX;
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	id<CAMetalDrawable> drawable = nil;
	{
		std::unique_lock requestLock(request->mutex);
		const VkResult drawableWait = WaitWithNanosecondTimeout(
		    request->condition, requestLock, RemainingTimeout(timeout, acquireStart),
		    [request] { return request->complete; });
		if (drawableWait != VK_SUCCESS)
		{
			return AcquireTimeoutResult(timeout);
		}
		drawable = request->drawable;
	}
	swapchain->drawableRequest.reset();
	swapchain->drawableRequestImageIndex = UINT32_MAX;
	if (drawable == nil || swapchain->retired || swapchain->destroying ||
	    !swapchain->surface->alive.load(std::memory_order_acquire))
	{
		swapchain->states[selected] = Lumen::MetalVulkan::SwapchainImageState::Available;
		swapchain->imageCondition.notify_all();
		return VK_ERROR_OUT_OF_DATE_KHR;
	}
	VkImage image = swapchain->images[selected];
	{
		std::lock_guard imageLock(image->mutex);
		image->drawable = drawable;
		image->metalTexture = drawable.texture;
		for (VkImageView view: image->views)
		{
			if (!BuildMetalImageView(view))
			{
				for (VkImageView rollbackView: image->views)
				{
					rollbackView->metalTexture = nil;
				}
				image->metalTexture = nil;
				image->drawable = nil;
				swapchain->states[selected] = Lumen::MetalVulkan::SwapchainImageState::Available;
				swapchain->imageCondition.notify_all();
				return VK_ERROR_FORMAT_NOT_SUPPORTED;
			}
		}
	}
	swapchain->states[selected] = Lumen::MetalVulkan::SwapchainImageState::Acquired;
	swapchain->nextSearchIndex = (selected + 1) % swapchain->states.size();
	*imageIndex = selected;
	lock.unlock();
	Lumen::MetalVulkan::SignalSemaphore(semaphore);
	Lumen::MetalVulkan::SignalFence(fence);
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* presentInfo)
{
	if (queue == VK_NULL_HANDLE || presentInfo == nullptr || presentInfo->sType != VK_STRUCTURE_TYPE_PRESENT_INFO_KHR)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if ((presentInfo->waitSemaphoreCount != 0 && presentInfo->pWaitSemaphores == nullptr) ||
	    (presentInfo->swapchainCount != 0 &&
	     (presentInfo->pSwapchains == nullptr || presentInfo->pImageIndices == nullptr)))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (queue->device->lost.load(std::memory_order_acquire))
	{
		return VK_ERROR_DEVICE_LOST;
	}
	for (uint32_t index = 0; index < presentInfo->waitSemaphoreCount; ++index)
	{
		if (presentInfo->pWaitSemaphores[index] == VK_NULL_HANDLE ||
		    presentInfo->pWaitSemaphores[index]->device != queue->device)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		const VkResult wait = Lumen::MetalVulkan::WaitSemaphore(presentInfo->pWaitSemaphores[index], UINT64_MAX);
		if (wait != VK_SUCCESS)
		{
			return wait;
		}
		if (queue->device->lost.load(std::memory_order_acquire))
		{
			return VK_ERROR_DEVICE_LOST;
		}
	}

	VkResult aggregate = VK_SUCCESS;
	for (uint32_t index = 0; index < presentInfo->swapchainCount; ++index)
	{
		VkResult result = VK_SUCCESS;
		VkSwapchainKHR swapchain = presentInfo->pSwapchains[index];
		const uint32_t imageIndex = presentInfo->pImageIndices[index];
		id<CAMetalDrawable> drawable = nil;
		if (swapchain == VK_NULL_HANDLE || swapchain->device != queue->device)
		{
			result = VK_ERROR_INITIALIZATION_FAILED;
			if (presentInfo->pResults != nullptr)
			{
				presentInfo->pResults[index] = result;
			}
			if (aggregate == VK_SUCCESS)
			{
				aggregate = result;
			}
			continue;
		}
		{
			std::lock_guard lock(swapchain->mutex);
			if (swapchain->retired || swapchain->destroying || imageIndex >= swapchain->images.size() ||
			    swapchain->states[imageIndex] != Lumen::MetalVulkan::SwapchainImageState::Acquired)
			{
				result = VK_ERROR_OUT_OF_DATE_KHR;
			} else
			{
				drawable = swapchain->images[imageIndex]->drawable;
				if (drawable == nil)
				{
					result = VK_ERROR_OUT_OF_DATE_KHR;
				} else
				{
					swapchain->states[imageIndex] = Lumen::MetalVulkan::SwapchainImageState::Presenting;
					++swapchain->presentationsInFlight;
				}
			}
		}
		if (result == VK_SUCCESS)
		{
			id<MTLCommandBuffer> commandBuffer = [queue->metalQueue commandBuffer];
			if (commandBuffer == nil)
			{
				CompletePresent(swapchain, imageIndex);
				result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
			} else
			{
				{
					std::lock_guard queueLock(queue->mutex);
					++queue->inFlightCount;
				}
#if !TARGET_OS_SIMULATOR
				[(id<MTLDrawable>)drawable addPresentedHandler:^(__unused id<MTLDrawable> presentedDrawable) {
					NotifyPresentCompleted();
				}];
#endif
				[commandBuffer presentDrawable:drawable];
				[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
					ObserveMetalCompletion(queue->device, completed);
					CompletePresent(swapchain, imageIndex);
					std::lock_guard queueLock(queue->mutex);
					if (queue->inFlightCount != 0)
					{
						--queue->inFlightCount;
					}
					queue->idleCondition.notify_all();
				}];
				[commandBuffer commit];
			}
		}
		if (presentInfo->pResults != nullptr)
		{
			presentInfo->pResults[index] = result;
		}
		if (result != VK_SUCCESS && aggregate == VK_SUCCESS)
		{
			aggregate = result;
		}
	}
	return aggregate;
}

extern "C" void LumenMetalSetPresentCompletionCallback(PresentCompletionCallback callback, void* context)
{
	std::lock_guard lock(gPresentCompletionMutex);
	gPresentCompletionCallback = callback;
	gPresentCompletionContext = context;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* allocateInfo,
	                                            const VkAllocationCallbacks* allocator, VkDeviceMemory* memory)
{
	if (device == VK_NULL_HANDLE || allocateInfo == nullptr || memory == nullptr ||
	    allocateInfo->sType != VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO || !IsSupportedAllocator(allocator) ||
	    allocateInfo->allocationSize == 0 || allocateInfo->memoryTypeIndex > 1)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*memory = VK_NULL_HANDLE;
	auto created = std::unique_ptr<VkDeviceMemory_T>(new (std::nothrow) VkDeviceMemory_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->size = allocateInfo->allocationSize;
	created->memoryTypeIndex = allocateInfo->memoryTypeIndex;
	created->properties = allocateInfo->memoryTypeIndex == 0 ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : kSharedMemoryProperties;
	// Host-visible allocations need their coherent backing immediately for
	// vkMapMemory. Device-local allocations acquire a backing buffer lazily if
	// they are bound to a VkBuffer; image bindings use native Metal textures.
	if (allocateInfo->memoryTypeIndex == 1)
	{
		created->metalBuffer = [device->metalDevice newBufferWithLength:static_cast<NSUInteger>(created->size)
		                                                       options:MTLResourceStorageModeShared];
		if (created->metalBuffer == nil)
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
	}
	*memory = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator)
{
	if (memory == VK_NULL_HANDLE || memory->device != device || !IsSupportedAllocator(allocator))
	{
		return;
	}
	memory->alive.store(false, std::memory_order_release);
	memory->metalBuffer = nil;
	delete memory;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
	                                       VkMemoryMapFlags flags, void** data)
{
	if (device == VK_NULL_HANDLE || memory == VK_NULL_HANDLE || memory->device != device || data == nullptr || flags != 0 ||
	    (memory->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 || offset >= memory->size ||
	    (size != VK_WHOLE_SIZE && size > memory->size - offset))
	{
		return VK_ERROR_MEMORY_MAP_FAILED;
	}
	std::lock_guard lock(memory->mutex);
	if (memory->mapCount != 0 || size == 0)
	{
		return VK_ERROR_MEMORY_MAP_FAILED;
	}
	void* contents = memory->metalBuffer.contents;
	if (contents == nullptr)
	{
		return VK_ERROR_MEMORY_MAP_FAILED;
	}
	++memory->mapCount;
	*data = static_cast<std::byte*>(contents) + offset;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
	if (memory == VK_NULL_HANDLE || memory->device != device)
	{
		return;
	}
	std::lock_guard lock(memory->mutex);
	if (memory->mapCount != 0)
	{
		--memory->mapCount;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice device, uint32_t rangeCount,
	                                                    const VkMappedMemoryRange* ranges)
{
	if (device == VK_NULL_HANDLE || (rangeCount != 0 && ranges == nullptr))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	for (uint32_t index = 0; index < rangeCount; ++index)
	{
		const auto& range = ranges[index];
		if (range.sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE || range.memory == VK_NULL_HANDLE ||
		    range.memory->device != device || (range.memory->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0 ||
		    range.offset > range.memory->size ||
		    (range.size != VK_WHOLE_SIZE && (range.size == 0 || range.size > range.memory->size - range.offset)))
		{
			return VK_ERROR_MEMORY_MAP_FAILED;
		}
		std::lock_guard memoryLock(range.memory->mutex);
		if (range.memory->mapCount == 0)
		{
			return VK_ERROR_MEMORY_MAP_FAILED;
		}
	}
	return VK_SUCCESS; // All host-visible memory advertised by this adapter is coherent.
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t rangeCount,
	                                                         const VkMappedMemoryRange* ranges)
{
	return vkFlushMappedMemoryRanges(device, rangeCount, ranges);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo* createInfo,
	                                          const VkAllocationCallbacks* allocator, VkBuffer* buffer)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || buffer == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || !IsSupportedAllocator(allocator) || createInfo->size == 0)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	auto created = std::unique_ptr<VkBuffer_T>(new (std::nothrow) VkBuffer_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->size = createInfo->size;
	created->usage = createInfo->usage;
	*buffer = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator)
{
	if (buffer == VK_NULL_HANDLE || buffer->device != device || !IsSupportedAllocator(allocator))
	{
		return;
	}
	{
		std::lock_guard lock(device->mutex);
		device->deviceAddressBuffers.erase(buffer);
	}
	buffer->alive.store(false, std::memory_order_release);
	buffer->metalBuffer = nil;
	delete buffer;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements* requirements)
{
	if (device == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE || requirements == nullptr || buffer->device != device)
	{
		return;
	}
	requirements->size = AlignUp(buffer->size, kResourceAlignment);
	requirements->alignment = kResourceAlignment;
	requirements->memoryTypeBits = 0x3;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
	if (device == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE || memory == VK_NULL_HANDLE || buffer->device != device ||
	    memory->device != device || offset % kResourceAlignment != 0 || offset > memory->size || buffer->size > memory->size - offset)
	{
		return VK_ERROR_MEMORY_MAP_FAILED;
	}
	std::lock_guard lock(buffer->mutex);
	buffer->memory = memory;
	buffer->memoryOffset = offset;
	{
		std::lock_guard memoryLock(memory->mutex);
		if (memory->metalBuffer == nil)
		{
			memory->metalBuffer = [device->metalDevice newBufferWithLength:static_cast<NSUInteger>(memory->size)
			                                                    options:Lumen::MetalVulkan::ToMTLResourceOptions(
			                                                                memory->properties)];
			if (memory->metalBuffer == nil)
			{
				buffer->memory = VK_NULL_HANDLE;
				buffer->memoryOffset = 0;
				return VK_ERROR_OUT_OF_DEVICE_MEMORY;
			}
		}
	}
	buffer->metalBuffer = memory->metalBuffer;
	return VK_SUCCESS;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device,
                                                              const VkBufferDeviceAddressInfo* info)
{
	if (device == VK_NULL_HANDLE || info == nullptr || info->buffer == VK_NULL_HANDLE ||
	    info->buffer->device != device || info->buffer->metalBuffer == nil ||
	    (info->buffer->usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) == 0) return 0;
	std::lock_guard lock(device->mutex);
	const auto buffer = info->buffer;
	if (buffer->gpuAddress != 0) return buffer->gpuAddress;
#if TARGET_OS_OSX
	if (@available(macOS 13.0, *))
#else
	if (@available(iOS 16.0, *))
#endif
	{
		buffer->gpuAddress = buffer->metalBuffer.gpuAddress + buffer->memoryOffset;
	}
	else
	{
		buffer->gpuAddress = Lumen::MetalVulkan::QueryBufferAddressFromShader(device, buffer);
	}
	if (buffer->gpuAddress != 0) device->deviceAddressBuffers.insert(buffer);
	return buffer->gpuAddress;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2* info,
                                                        VkMemoryRequirements2* requirements)
{
	if (info == nullptr || requirements == nullptr) return;
	vkGetBufferMemoryRequirements(device, info->buffer, &requirements->memoryRequirements);
	for (auto* next = static_cast<VkBaseOutStructure*>(requirements->pNext); next != nullptr; next = next->pNext)
		if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
			auto* dedicated = reinterpret_cast<VkMemoryDedicatedRequirements*>(next);
			dedicated->prefersDedicatedAllocation = VK_FALSE; dedicated->requiresDedicatedAllocation = VK_FALSE;
		}
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo* infos)
{
	if (count != 0 && infos == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
	for (uint32_t i = 0; i < count; ++i)
		if (const auto result = vkBindBufferMemory(device, infos[i].buffer, infos[i].memory, infos[i].memoryOffset); result != VK_SUCCESS) return result;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo* createInfo,
	                                         const VkAllocationCallbacks* allocator, VkImage* image)
{
	if (image != nullptr)
	{
		*image = VK_NULL_HANDLE;
	}
	if (device == VK_NULL_HANDLE || createInfo == nullptr || image == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || !IsSupportedAllocator(allocator) ||
	    createInfo->extent.width == 0 || createInfo->extent.height == 0 || createInfo->extent.depth == 0 ||
	    createInfo->mipLevels == 0 || createInfo->arrayLayers == 0 ||
	    createInfo->tiling != VK_IMAGE_TILING_OPTIMAL ||
	    (createInfo->samples != VK_SAMPLE_COUNT_1_BIT && createInfo->samples != VK_SAMPLE_COUNT_2_BIT &&
	     createInfo->samples != VK_SAMPLE_COUNT_4_BIT) ||
	    Lumen::MetalVulkan::ToMTLPixelFormat(createInfo->format) == MTLPixelFormatInvalid)
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	VkImageFormatProperties imageProperties {};
	if (vkGetPhysicalDeviceImageFormatProperties(device->physicalDevice, createInfo->format, createInfo->imageType,
	                                             createInfo->tiling, createInfo->usage, createInfo->flags,
	                                             &imageProperties) != VK_SUCCESS ||
	    createInfo->extent.width > imageProperties.maxExtent.width ||
	    createInfo->extent.height > imageProperties.maxExtent.height ||
	    createInfo->extent.depth > imageProperties.maxExtent.depth ||
	    createInfo->mipLevels > imageProperties.maxMipLevels ||
	    createInfo->arrayLayers > imageProperties.maxArrayLayers ||
	    (imageProperties.sampleCounts & createInfo->samples) == 0)
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	auto created = std::unique_ptr<VkImage_T>(new (std::nothrow) VkImage_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->imageType = createInfo->imageType;
	created->format = createInfo->format;
	created->flags = createInfo->flags;
	created->extent = createInfo->extent;
	created->mipLevels = createInfo->mipLevels;
	created->arrayLayers = createInfo->arrayLayers;
	created->samples = createInfo->samples;
	created->tiling = createInfo->tiling;
	created->usage = createInfo->usage;
	created->initialLayout = createInfo->initialLayout;
	*image = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator)
{
	if (image == VK_NULL_HANDLE || image->device != device || image->ownerSwapchain != VK_NULL_HANDLE ||
	    !IsSupportedAllocator(allocator))
	{
		return;
	}
	image->alive.store(false, std::memory_order_release);
	image->metalTexture = nil;
	delete image;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements* requirements)
{
	if (device == VK_NULL_HANDLE || image == VK_NULL_HANDLE || requirements == nullptr || image->device != device)
	{
		return;
	}
	requirements->size = ImageAllocationSize(*image);
	requirements->alignment = kResourceAlignment;
	requirements->memoryTypeBits = 0x1;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
{
	if (device == VK_NULL_HANDLE || image == VK_NULL_HANDLE || memory == VK_NULL_HANDLE || image->device != device ||
	    memory->device != device || image->ownerSwapchain != VK_NULL_HANDLE || offset % kResourceAlignment != 0 ||
	    offset > memory->size || ImageAllocationSize(*image) > memory->size - offset)
	{
		return VK_ERROR_MEMORY_MAP_FAILED;
	}
	MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
	if (image->samples != VK_SAMPLE_COUNT_1_BIT)
	{
		if (image->imageType != VK_IMAGE_TYPE_2D || image->mipLevels != 1)
		{
			return VK_ERROR_FEATURE_NOT_PRESENT;
		}
		descriptor.textureType = image->arrayLayers > 1 ? MTLTextureType2DMultisampleArray
		                                                 : MTLTextureType2DMultisample;
	} else
	{
		descriptor.textureType = Lumen::MetalVulkan::ToMTLTextureType(image->imageType, VK_IMAGE_VIEW_TYPE_MAX_ENUM,
		                                                            image->arrayLayers);
	}
	descriptor.pixelFormat = Lumen::MetalVulkan::ToMTLPixelFormat(image->format);
    if ((image->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) && (image->usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
        if (image->format == VK_FORMAT_R8G8B8A8_SRGB) descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
        if (image->format == VK_FORMAT_B8G8R8A8_SRGB) descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
    }
	descriptor.width = image->extent.width;
	descriptor.height = image->extent.height;
	descriptor.depth = image->extent.depth;
	descriptor.mipmapLevelCount = image->mipLevels;
	descriptor.arrayLength = image->arrayLayers;
	descriptor.sampleCount = static_cast<NSUInteger>(image->samples);
	descriptor.usage = Lumen::MetalVulkan::ToMTLTextureUsage(image->usage) | MTLTextureUsagePixelFormatView;
	// Vulkan transfer clears use Metal attachment load actions, even for an
	// image which is never bound as a Vulkan framebuffer. Compressed formats
	// are deliberately excluded because Metal cannot render into them.
	if ((image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 &&
	    (Lumen::MetalVulkan::FormatFeatures(image->format, VK_IMAGE_TILING_OPTIMAL) &
	     (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0)
		descriptor.usage |= MTLTextureUsageRenderTarget;
	descriptor.storageMode = Lumen::MetalVulkan::ToMTLStorageMode(memory->properties);
	std::lock_guard lock(image->mutex);
	image->metalTexture = [device->metalDevice newTextureWithDescriptor:descriptor];
	if (image->metalTexture == nil)
	{
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	image->memory = memory;
	image->memoryOffset = offset;
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2* info,
                                                        VkMemoryRequirements2* requirements)
{
	if (info == nullptr || requirements == nullptr) return;
	vkGetImageMemoryRequirements(device, info->image, &requirements->memoryRequirements);
	for (auto* next = static_cast<VkBaseOutStructure*>(requirements->pNext); next != nullptr; next = next->pNext)
		if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
			auto* dedicated = reinterpret_cast<VkMemoryDedicatedRequirements*>(next);
			dedicated->prefersDedicatedAllocation = VK_FALSE; dedicated->requiresDedicatedAllocation = VK_FALSE;
		}
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory2(VkDevice device, uint32_t count, const VkBindImageMemoryInfo* infos)
{
	if (count != 0 && infos == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
	for (uint32_t i = 0; i < count; ++i)
		if (const auto result = vkBindImageMemory(device, infos[i].image, infos[i].memory, infos[i].memoryOffset); result != VK_SUCCESS) return result;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceToolProperties(VkPhysicalDevice physical, uint32_t* count,
                                                               VkPhysicalDeviceToolProperties*)
{
	if (physical == VK_NULL_HANDLE || count == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
	*count = 0;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetDebugUtilsObjectNameEXT(VkDevice device, const VkDebugUtilsObjectNameInfoEXT* info)
{
	if (device == VK_NULL_HANDLE || info == nullptr || info->objectHandle == 0) return VK_ERROR_INITIALIZATION_FAILED;
	NSString* name = info->pObjectName ? [NSString stringWithUTF8String:info->pObjectName] : nil;
	switch (info->objectType) {
	case VK_OBJECT_TYPE_BUFFER: reinterpret_cast<VkBuffer>(info->objectHandle)->metalBuffer.label = name; break;
	case VK_OBJECT_TYPE_IMAGE: reinterpret_cast<VkImage>(info->objectHandle)->metalTexture.label = name; break;
	case VK_OBJECT_TYPE_IMAGE_VIEW: reinterpret_cast<VkImageView>(info->objectHandle)->metalTexture.label = name; break;
	case VK_OBJECT_TYPE_QUEUE: reinterpret_cast<VkQueue>(info->objectHandle)->metalQueue.label = name; break;
	default: break;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* createInfo,
	                                             const VkAllocationCallbacks* allocator, VkImageView* view)
{
	if (view != nullptr)
	{
		*view = VK_NULL_HANDLE;
	}
	if (device == VK_NULL_HANDLE || createInfo == nullptr || view == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO || !IsSupportedAllocator(allocator) ||
	    createInfo->image == VK_NULL_HANDLE || createInfo->image->device != device ||
	    (createInfo->image->metalTexture == nil && createInfo->image->ownerSwapchain == VK_NULL_HANDLE))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkImageUsageFlags viewUsage = createInfo->image->usage;
    for (auto* next = static_cast<const VkBaseInStructure*>(createInfo->pNext); next != nullptr; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO)
            viewUsage = reinterpret_cast<const VkImageViewUsageCreateInfo*>(next)->usage;
		else if (next->sType == VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT &&
		         reinterpret_cast<const VkImageViewMinLodCreateInfoEXT*>(next)->minLod != 0.0f)
			return VK_ERROR_FEATURE_NOT_PRESENT;
	}
    const bool alternateFormat = createInfo->format != createInfo->image->format;
	if ((alternateFormat &&
	     (((createInfo->image->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) == 0) ||
	      !Lumen::MetalVulkan::IsSupportedMutableImageViewPair(createInfo->image->format, createInfo->format))) ||
	    !Lumen::MetalVulkan::ImageUsageSupported(createInfo->format, createInfo->image->tiling,
	                                            viewUsage))
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	MTLPixelFormat pixelFormat = Lumen::MetalVulkan::ToMTLPixelFormat(createInfo->format);
	if (pixelFormat == MTLPixelFormatInvalid)
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	auto created = std::unique_ptr<VkImageView_T>(new (std::nothrow) VkImageView_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->image = createInfo->image;
	created->format = createInfo->format;
	created->viewType = createInfo->viewType;
	created->components = createInfo->components;
	created->range = createInfo->subresourceRange;
	{
		std::lock_guard imageLock(createInfo->image->mutex);
		if (createInfo->image->metalTexture != nil && !BuildMetalImageView(created.get()))
		{
			return VK_ERROR_FORMAT_NOT_SUPPORTED;
		}
		createInfo->image->views.insert(created.get());
	}
	if (createInfo->image->metalTexture != nil && created->metalTexture == nil)
	{
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}
	*view = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator)
{
	if (view == VK_NULL_HANDLE || view->device != device || !IsSupportedAllocator(allocator))
	{
		return;
	}
	if (view->image != VK_NULL_HANDLE)
	{
		std::lock_guard imageLock(view->image->mutex);
		view->image->views.erase(view);
	}
	view->alive.store(false, std::memory_order_release);
	view->metalTexture = nil;
	delete view;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo* createInfo,
	                                           const VkAllocationCallbacks* allocator, VkSampler* sampler)
{
	if (sampler != nullptr)
	{
		*sampler = VK_NULL_HANDLE;
	}
	if (device == VK_NULL_HANDLE || createInfo == nullptr || sampler == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO || !IsSupportedAllocator(allocator))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (createInfo->unnormalizedCoordinates == VK_TRUE &&
	    (createInfo->minFilter != createInfo->magFilter || createInfo->mipmapMode != VK_SAMPLER_MIPMAP_MODE_NEAREST ||
	     createInfo->minLod != 0.0f || createInfo->maxLod != 0.0f || createInfo->anisotropyEnable == VK_TRUE ||
	     createInfo->compareEnable == VK_TRUE ||
	     (createInfo->addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
	      createInfo->addressModeU != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) ||
	     (createInfo->addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE &&
	      createInfo->addressModeV != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)))
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
	descriptor.minFilter = createInfo->minFilter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	descriptor.magFilter = createInfo->magFilter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
	descriptor.mipFilter = createInfo->unnormalizedCoordinates == VK_TRUE
	                           ? MTLSamplerMipFilterNotMipmapped
	                           : (createInfo->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? MTLSamplerMipFilterLinear
	                                                                                     : MTLSamplerMipFilterNearest);
	auto addressMode = [](VkSamplerAddressMode mode)
	{
		switch (mode)
		{
			case VK_SAMPLER_ADDRESS_MODE_REPEAT: return MTLSamplerAddressModeRepeat;
			case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
			case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
			case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
			case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return MTLSamplerAddressModeMirrorClampToEdge;
			default: return MTLSamplerAddressModeClampToEdge;
		}
	};
	descriptor.sAddressMode = addressMode(createInfo->addressModeU);
	descriptor.tAddressMode = addressMode(createInfo->addressModeV);
	descriptor.rAddressMode = addressMode(createInfo->addressModeW);
	descriptor.normalizedCoordinates = createInfo->unnormalizedCoordinates == VK_FALSE;
	descriptor.lodMinClamp = createInfo->minLod;
	descriptor.lodMaxClamp = createInfo->maxLod;
	descriptor.maxAnisotropy = createInfo->anisotropyEnable ? std::clamp(static_cast<NSUInteger>(createInfo->maxAnisotropy),
	                                                                  NSUInteger(1), NSUInteger(16)) : 1;
	if (createInfo->compareEnable)
	{
		descriptor.compareFunction = static_cast<MTLCompareFunction>(createInfo->compareOp);
	}
	switch (createInfo->borderColor)
	{
		case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
		case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK: descriptor.borderColor = MTLSamplerBorderColorTransparentBlack; break;
		case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
		case VK_BORDER_COLOR_INT_OPAQUE_BLACK: descriptor.borderColor = MTLSamplerBorderColorOpaqueBlack; break;
		case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
		case VK_BORDER_COLOR_INT_OPAQUE_WHITE: descriptor.borderColor = MTLSamplerBorderColorOpaqueWhite; break;
		default: return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	if (createInfo->mipLodBias != 0.0f)
	{
		if (@available(macOS 26.0, iOS 26.0, *))
		{
			descriptor.lodBias = createInfo->mipLodBias;
		} else
		{
			return VK_ERROR_FEATURE_NOT_PRESENT;
		}
	}
	auto created = std::unique_ptr<VkSampler_T>(new (std::nothrow) VkSampler_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->metalSampler = [device->metalDevice newSamplerStateWithDescriptor:descriptor];
	if (created->metalSampler == nil)
	{
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	*sampler = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* allocator)
{
	if (sampler == VK_NULL_HANDLE || sampler->device != device || !IsSupportedAllocator(allocator))
	{
		return;
	}
	sampler->alive.store(false, std::memory_order_release);
	sampler->metalSampler = nil;
	delete sampler;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* createInfo,
	                                               const VkAllocationCallbacks* allocator, VkCommandPool* commandPool)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || commandPool == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO || !IsSupportedAllocator(allocator) ||
	    createInfo->queueFamilyIndex != kQueueFamilyIndex)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	auto created = std::unique_ptr<VkCommandPool_T>(new (std::nothrow) VkCommandPool_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->queueFamilyIndex = createInfo->queueFamilyIndex;
	created->flags = createInfo->flags;
	*commandPool = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice device, VkCommandPool commandPool,
	                                           const VkAllocationCallbacks* allocator)
{
	if (commandPool == VK_NULL_HANDLE || commandPool->device != device || !IsSupportedAllocator(allocator))
	{
		return;
	}
	std::vector<VkCommandBuffer> buffers;
	{
		std::lock_guard lock(commandPool->mutex);
		buffers.assign(commandPool->commandBuffers.begin(), commandPool->commandBuffers.end());
		commandPool->commandBuffers.clear();
		commandPool->alive.store(false, std::memory_order_release);
	}
	for (VkCommandBuffer buffer: buffers)
	{
		std::unique_lock bufferLock(buffer->mutex);
		buffer->completionCondition.wait(bufferLock, [buffer] { return buffer->pendingCount == 0; });
		buffer->alive.store(false, std::memory_order_release);
		bufferLock.unlock();
		delete buffer;
	}
	delete commandPool;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice device, VkCommandPool commandPool,
	                                              VkCommandPoolResetFlags flags)
{
	if (device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE || commandPool->device != device ||
	    !commandPool->alive.load(std::memory_order_acquire))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::lock_guard poolLock(commandPool->mutex);
	for (VkCommandBuffer buffer: commandPool->commandBuffers)
	{
		std::lock_guard bufferLock(buffer->mutex);
		if (buffer->state == Lumen::MetalVulkan::CommandBufferState::Pending)
		{
			return VK_NOT_READY;
		}
	}
	for (VkCommandBuffer buffer: commandPool->commandBuffers)
	{
		std::lock_guard bufferLock(buffer->mutex);
		ResetCommandRecordingState(buffer);
		buffer->usage = 0;
		buffer->state = Lumen::MetalVulkan::CommandBufferState::Initial;
	}
	(void)flags;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* allocateInfo,
	                                                   VkCommandBuffer* commandBuffers)
{
	if (device == VK_NULL_HANDLE || allocateInfo == nullptr || commandBuffers == nullptr ||
	    allocateInfo->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO || allocateInfo->commandPool == VK_NULL_HANDLE ||
	    allocateInfo->commandPool->device != device || allocateInfo->commandBufferCount == 0 ||
	    allocateInfo->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::vector<std::unique_ptr<VkCommandBuffer_T>> created;
	created.reserve(allocateInfo->commandBufferCount);
	for (uint32_t index = 0; index < allocateInfo->commandBufferCount; ++index)
	{
		auto buffer = std::unique_ptr<VkCommandBuffer_T>(new (std::nothrow) VkCommandBuffer_T);
		if (!buffer)
		{
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		buffer->pool = allocateInfo->commandPool;
		buffer->level = allocateInfo->level;
		created.push_back(std::move(buffer));
	}
	std::lock_guard lock(allocateInfo->commandPool->mutex);
	for (uint32_t index = 0; index < allocateInfo->commandBufferCount; ++index)
	{
		commandBuffers[index] = created[index].release();
		allocateInfo->commandPool->commandBuffers.insert(commandBuffers[index]);
	}
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool, uint32_t commandBufferCount,
	                                           const VkCommandBuffer* commandBuffers)
{
	if (device == VK_NULL_HANDLE || commandPool == VK_NULL_HANDLE || commandPool->device != device ||
	    (commandBufferCount != 0 && commandBuffers == nullptr))
	{
		return;
	}
	for (uint32_t index = 0; index < commandBufferCount; ++index)
	{
		VkCommandBuffer buffer = commandBuffers[index];
		if (buffer == VK_NULL_HANDLE || buffer->pool != commandPool)
		{
			continue;
		}
		{
			std::lock_guard poolLock(commandPool->mutex);
			if (commandPool->commandBuffers.erase(buffer) == 0)
			{
				continue;
			}
		}
		std::unique_lock bufferLock(buffer->mutex);
		buffer->completionCondition.wait(bufferLock, [buffer] { return buffer->pendingCount == 0; });
		buffer->alive.store(false, std::memory_order_release);
		bufferLock.unlock();
		delete buffer;
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer commandBuffer,
	                                                const VkCommandBufferBeginInfo* beginInfo)
{
	if (commandBuffer == VK_NULL_HANDLE || beginInfo == nullptr ||
	    beginInfo->sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || commandBuffer->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if ((beginInfo->flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) != 0)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	std::lock_guard lock(commandBuffer->mutex);
	if (commandBuffer->state == Lumen::MetalVulkan::CommandBufferState::Pending)
	{
		return VK_NOT_READY;
	}
	ResetCommandRecordingState(commandBuffer);
	commandBuffer->usage = beginInfo->flags;
	commandBuffer->state = Lumen::MetalVulkan::CommandBufferState::Recording;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::lock_guard lock(commandBuffer->mutex);
	if (commandBuffer->state != Lumen::MetalVulkan::CommandBufferState::Recording)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (commandBuffer->recordingError != VK_SUCCESS)
	{
		std::fprintf(stderr, "vkEndCommandBuffer: recording operation=%s failed result=%d\n",
		             commandBuffer->recordingErrorOperation != nullptr ? commandBuffer->recordingErrorOperation : "unknown",
		             static_cast<int>(commandBuffer->recordingError));
		commandBuffer->state = Lumen::MetalVulkan::CommandBufferState::Invalid;
		return commandBuffer->recordingError;
	}
	commandBuffer->state = Lumen::MetalVulkan::CommandBufferState::Executable;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
{
	if (commandBuffer == VK_NULL_HANDLE || (commandBuffer->pool->flags & VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == 0)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	std::lock_guard lock(commandBuffer->mutex);
	if (commandBuffer->state == Lumen::MetalVulkan::CommandBufferState::Pending)
	{
		return VK_NOT_READY;
	}
	ResetCommandRecordingState(commandBuffer);
	commandBuffer->usage = 0;
	commandBuffer->state = Lumen::MetalVulkan::CommandBufferState::Initial;
	(void)flags;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence)
{
	if (queue == VK_NULL_HANDLE || (submitCount != 0 && submits == nullptr) ||
	    (fence != VK_NULL_HANDLE && fence->device != queue->device))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (queue->device->lost.load(std::memory_order_acquire))
	{
		return VK_ERROR_DEVICE_LOST;
	}
	if (fence != VK_NULL_HANDLE)
	{
		Lumen::MetalVulkan::ResetFence(fence);
	}
	if (submitCount == 0)
	{
		Lumen::MetalVulkan::SignalFence(fence);
		return VK_SUCCESS;
	}

	for (uint32_t submitIndex = 0; submitIndex < submitCount; ++submitIndex)
	{
		const VkSubmitInfo& submit = submits[submitIndex];
		const VkTimelineSemaphoreSubmitInfo* timeline = nullptr;
		for (auto* next = static_cast<const VkBaseInStructure*>(submit.pNext); next != nullptr; next = next->pNext)
			if (next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
				timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(next);
		if (submit.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO ||
		    (submit.waitSemaphoreCount != 0 && submit.pWaitSemaphores == nullptr) ||
		    (submit.commandBufferCount != 0 && submit.pCommandBuffers == nullptr) ||
		    (submit.signalSemaphoreCount != 0 && submit.pSignalSemaphores == nullptr))
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		if (timeline != nullptr &&
		    ((timeline->waitSemaphoreValueCount != 0 &&
		      (timeline->waitSemaphoreValueCount != submit.waitSemaphoreCount || timeline->pWaitSemaphoreValues == nullptr)) ||
		     (timeline->signalSemaphoreValueCount != 0 &&
		      (timeline->signalSemaphoreValueCount != submit.signalSemaphoreCount || timeline->pSignalSemaphoreValues == nullptr))))
			return VK_ERROR_INITIALIZATION_FAILED;
		id<MTLCommandBuffer> timelineWaits = nil;
		for (uint32_t semaphoreIndex = 0; semaphoreIndex < submit.waitSemaphoreCount; ++semaphoreIndex)
		{
			if (submit.pWaitSemaphores[semaphoreIndex] == VK_NULL_HANDLE ||
			    submit.pWaitSemaphores[semaphoreIndex]->device != queue->device)
			{
				return VK_ERROR_INITIALIZATION_FAILED;
			}
			const auto semaphore = submit.pWaitSemaphores[semaphoreIndex];
			if (semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE)
			{
				if (timeline == nullptr || timeline->waitSemaphoreValueCount != submit.waitSemaphoreCount)
					return VK_ERROR_INITIALIZATION_FAILED;
				if (timelineWaits == nil) timelineWaits = [queue->metalQueue commandBuffer];
				if (timelineWaits == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
				[timelineWaits encodeWaitForEvent:semaphore->sharedEvent
				                             value:timeline->pWaitSemaphoreValues[semaphoreIndex]];
				continue;
			}
			const VkResult wait = Lumen::MetalVulkan::WaitSemaphore(submit.pWaitSemaphores[semaphoreIndex], UINT64_MAX);
			if (wait != VK_SUCCESS)
			{
				return wait;
			}
			if (queue->device->lost.load(std::memory_order_acquire))
			{
				return VK_ERROR_DEVICE_LOST;
			}
		}
		// A timeline wait must be queued, not performed on the submitting CPU:
		// Vulkan permits another queue or host thread to supply its future signal.
		if (timelineWaits != nil) [timelineWaits commit];
		auto signalSemaphores = std::make_shared<std::vector<VkSemaphore>>();
		auto signalValues = std::make_shared<std::vector<uint64_t>>(submit.signalSemaphoreCount, 0);
		if (submit.signalSemaphoreCount != 0)
		{
			for (uint32_t semaphoreIndex = 0; semaphoreIndex < submit.signalSemaphoreCount; ++semaphoreIndex)
			{
				if (submit.pSignalSemaphores[semaphoreIndex] == VK_NULL_HANDLE ||
				    submit.pSignalSemaphores[semaphoreIndex]->device != queue->device)
				{
					return VK_ERROR_INITIALIZATION_FAILED;
				}
				if (submit.pSignalSemaphores[semaphoreIndex]->type == VK_SEMAPHORE_TYPE_TIMELINE)
				{
					if (timeline == nullptr || timeline->signalSemaphoreValueCount != submit.signalSemaphoreCount)
						return VK_ERROR_INITIALIZATION_FAILED;
					(*signalValues)[semaphoreIndex] = timeline->pSignalSemaphoreValues[semaphoreIndex];
				}
			}
			signalSemaphores->assign(submit.pSignalSemaphores,
			                         submit.pSignalSemaphores + submit.signalSemaphoreCount);
		}
		const bool isFinalSubmit = submitIndex + 1 == submitCount;
		if (submit.commandBufferCount == 0)
		{
			id<MTLCommandBuffer> encoded = [queue->metalQueue commandBuffer];
			if (encoded == nil)
			{
				return VK_ERROR_OUT_OF_DEVICE_MEMORY;
			}
			for (size_t i = 0; i < signalSemaphores->size(); ++i)
				if ((*signalSemaphores)[i]->type == VK_SEMAPHORE_TYPE_TIMELINE)
					[encoded encodeSignalEvent:(*signalSemaphores)[i]->sharedEvent value:(*signalValues)[i]];
			{
				std::lock_guard queueLock(queue->mutex);
				++queue->inFlightCount;
			}
			[encoded addCompletedHandler:^(id<MTLCommandBuffer> completed) {
				ObserveMetalCompletion(queue->device, completed);
				for (VkSemaphore semaphore: *signalSemaphores)
				{
					if (semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE)
						Lumen::MetalVulkan::NotifyTimelineSemaphore(semaphore);
					else Lumen::MetalVulkan::SignalSemaphore(semaphore);
				}
				if (isFinalSubmit)
				{
					Lumen::MetalVulkan::SignalFence(fence);
				}
				std::lock_guard queueLock(queue->mutex);
				if (queue->inFlightCount != 0)
				{
					--queue->inFlightCount;
				}
				queue->idleCondition.notify_all();
			}];
			[encoded commit];
			continue;
		}
		for (uint32_t commandIndex = 0; commandIndex < submit.commandBufferCount; ++commandIndex)
		{
			VkCommandBuffer commandBuffer = submit.pCommandBuffers[commandIndex];
			if (commandBuffer == VK_NULL_HANDLE || commandBuffer->pool == VK_NULL_HANDLE ||
			    commandBuffer->pool->device != queue->device)
			{
				return VK_ERROR_INITIALIZATION_FAILED;
			}
			id<MTLCommandBuffer> encoded = nil;
			Lumen::MetalVulkan::ReplayFailure replayFailure;
			const VkResult encode = Lumen::MetalVulkan::EncodeCommandBuffer(commandBuffer, queue->metalQueue, &encoded,
			                                                                    &replayFailure);
			if (encode != VK_SUCCESS)
			{
				if (replayFailure.operationName != nullptr)
				{
					std::fprintf(stderr,
					             "[LUMEN_METAL] vkQueueSubmit replay failed: submit[%u] commandBuffer[%u] "
					             "%s[%zu]=%s VkResult=%s (%d)\n",
					             submitIndex, commandIndex,
					             replayFailure.occurredDuringRecording ? "recording" : "operation",
					             replayFailure.operationIndex, replayFailure.operationName,
					             string_VkResult(replayFailure.result), static_cast<int>(replayFailure.result));
				}
				return encode;
			}
			const bool isFinalCommandInSubmit = commandIndex + 1 == submit.commandBufferCount;
			if (isFinalCommandInSubmit)
				for (size_t i = 0; i < signalSemaphores->size(); ++i)
					if ((*signalSemaphores)[i]->type == VK_SEMAPHORE_TYPE_TIMELINE)
						[encoded encodeSignalEvent:(*signalSemaphores)[i]->sharedEvent value:(*signalValues)[i]];
			{
				std::lock_guard queueLock(queue->mutex);
				++queue->inFlightCount;
			}
			[encoded addCompletedHandler:^(id<MTLCommandBuffer> completed) {
				ObserveMetalCompletion(queue->device, completed);
				Lumen::MetalVulkan::CompleteCommandBuffer(commandBuffer);
				if (isFinalCommandInSubmit)
				{
					for (VkSemaphore semaphore: *signalSemaphores)
					{
						if (semaphore->type == VK_SEMAPHORE_TYPE_TIMELINE)
							Lumen::MetalVulkan::NotifyTimelineSemaphore(semaphore);
						else Lumen::MetalVulkan::SignalSemaphore(semaphore);
					}
					if (isFinalSubmit)
					{
						Lumen::MetalVulkan::SignalFence(fence);
					}
				}
				std::lock_guard queueLock(queue->mutex);
				if (queue->inFlightCount != 0)
				{
					--queue->inFlightCount;
				}
				queue->idleCondition.notify_all();
			}];
			[encoded commit];
		}
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
	if (queue == VK_NULL_HANDLE)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::unique_lock lock(queue->mutex);
	queue->idleCondition.wait(lock, [queue] { return queue->inFlightCount == 0 || queue->shuttingDown; });
	return queue->shuttingDown || queue->device->lost.load(std::memory_order_acquire) ? VK_ERROR_DEVICE_LOST
	                                                                              : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
	if (device == VK_NULL_HANDLE)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	VkResult aggregate = VK_SUCCESS;
	for (auto& queue: device->queues)
	{
		const VkResult result = vkQueueWaitIdle(queue.get());
		if (result != VK_SUCCESS)
		{
			aggregate = result;
		}
	}
	return device->lost.load(std::memory_order_acquire) ? VK_ERROR_DEVICE_LOST : aggregate;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice device, const VkFenceCreateInfo* createInfo,
	                                         const VkAllocationCallbacks* allocator, VkFence* fence)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || fence == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_FENCE_CREATE_INFO || !IsSupportedAllocator(allocator))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	auto created = std::unique_ptr<VkFence_T>(new (std::nothrow) VkFence_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->signaled = (createInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
	*fence = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* allocator)
{
	if (fence == VK_NULL_HANDLE || fence->device != device || !IsSupportedAllocator(allocator))
	{
		return;
	}
	{
		std::lock_guard lock(fence->mutex);
		fence->alive = false;
		fence->condition.notify_all();
	}
	delete fence;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence)
{
	if (fence == VK_NULL_HANDLE || fence->device != device)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (device->lost.load(std::memory_order_acquire))
	{
		return VK_ERROR_DEVICE_LOST;
	}
	return Lumen::MetalVulkan::FenceIsSignaled(fence) ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence* fences)
{
	if (device == VK_NULL_HANDLE || (fenceCount != 0 && fences == nullptr))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	for (uint32_t index = 0; index < fenceCount; ++index)
	{
		if (fences[index] == VK_NULL_HANDLE || fences[index]->device != device)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		Lumen::MetalVulkan::ResetFence(fences[index]);
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice device, uint32_t fenceCount, const VkFence* fences,
	                                          VkBool32 waitAll, uint64_t timeout)
{
	if (device == VK_NULL_HANDLE || fenceCount == 0 || fences == nullptr)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	for (uint32_t index = 0; index < fenceCount; ++index)
	{
		if (fences[index] == VK_NULL_HANDLE || fences[index]->device != device)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
	}
	std::unique_lock lock(device->fenceCompletionMutex);
	const auto complete = [&] {
		if (device->lost.load(std::memory_order_acquire)) return true;
		uint32_t signaled = 0;
		for (uint32_t index = 0; index < fenceCount; ++index)
			signaled += Lumen::MetalVulkan::FenceIsSignaled(fences[index]) ? 1u : 0u;
		return (waitAll == VK_TRUE && signaled == fenceCount) || (waitAll == VK_FALSE && signaled != 0);
	};
	const VkResult result = WaitWithNanosecondTimeout(device->fenceCompletionCondition, lock, timeout, complete);
	return device->lost.load(std::memory_order_acquire) ? VK_ERROR_DEVICE_LOST : result;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* createInfo,
	                                             const VkAllocationCallbacks* allocator, VkSemaphore* semaphore)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || semaphore == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO || !IsSupportedAllocator(allocator))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	auto created = std::unique_ptr<VkSemaphore_T>(new (std::nothrow) VkSemaphore_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	for (auto* next = static_cast<const VkBaseInStructure*>(createInfo->pNext); next != nullptr; next = next->pNext)
	{
		if (next->sType != VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) continue;
		const auto* type = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(next);
		if (type->semaphoreType != VK_SEMAPHORE_TYPE_TIMELINE && type->semaphoreType != VK_SEMAPHORE_TYPE_BINARY)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		created->type = type->semaphoreType;
		if (type->semaphoreType == VK_SEMAPHORE_TYPE_BINARY && type->initialValue != 0)
			return VK_ERROR_INITIALIZATION_FAILED;
		if (type->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE)
		{
			created->sharedEvent = [device->metalDevice newSharedEvent];
			if (created->sharedEvent == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
			created->sharedEvent.signaledValue = type->initialValue;
		}
	}
	*semaphore = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSemaphoreCounterValue(VkDevice device, VkSemaphore semaphore,
	                                                      uint64_t* value)
{
	if (device == VK_NULL_HANDLE || semaphore == VK_NULL_HANDLE || semaphore->device != device ||
	    semaphore->type != VK_SEMAPHORE_TYPE_TIMELINE || value == nullptr)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (device->lost.load(std::memory_order_acquire)) return VK_ERROR_DEVICE_LOST;
	*value = semaphore->sharedEvent.signaledValue;
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo* info)
{
	if (device == VK_NULL_HANDLE || info == nullptr || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO ||
	    info->semaphore == VK_NULL_HANDLE || info->semaphore->device != device ||
	    info->semaphore->type != VK_SEMAPHORE_TYPE_TIMELINE)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (device->lost.load(std::memory_order_acquire)) return VK_ERROR_DEVICE_LOST;
	std::lock_guard lock(device->semaphoreCompletionMutex);
	if (info->value <= info->semaphore->sharedEvent.signaledValue) return VK_ERROR_INITIALIZATION_FAILED;
	info->semaphore->sharedEvent.signaledValue = info->value;
	device->semaphoreCompletionCondition.notify_all();
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout)
{
	if (device == VK_NULL_HANDLE || info == nullptr || info->sType != VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO ||
	    info->semaphoreCount == 0 || info->pSemaphores == nullptr || info->pValues == nullptr ||
	    (info->flags & ~VK_SEMAPHORE_WAIT_ANY_BIT) != 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	for (uint32_t i = 0; i < info->semaphoreCount; ++i)
		if (info->pSemaphores[i] == VK_NULL_HANDLE || info->pSemaphores[i]->device != device ||
		    info->pSemaphores[i]->type != VK_SEMAPHORE_TYPE_TIMELINE)
			return VK_ERROR_INITIALIZATION_FAILED;
	std::unique_lock lock(device->semaphoreCompletionMutex);
	const auto complete = [&] {
		if (device->lost.load(std::memory_order_acquire)) return true;
		uint32_t ready = 0;
		for (uint32_t i = 0; i < info->semaphoreCount; ++i)
			ready += info->pSemaphores[i]->sharedEvent.signaledValue >= info->pValues[i] ? 1 : 0;
		return (info->flags & VK_SEMAPHORE_WAIT_ANY_BIT) ? ready != 0 : ready == info->semaphoreCount;
	};
	const auto result = WaitWithNanosecondTimeout(device->semaphoreCompletionCondition, lock, timeout, complete);
	return device->lost.load(std::memory_order_acquire) ? VK_ERROR_DEVICE_LOST : result;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice device, VkSemaphore semaphore,
	                                         const VkAllocationCallbacks* allocator)
{
	if (semaphore == VK_NULL_HANDLE || semaphore->device != device || !IsSupportedAllocator(allocator))
	{
		return;
	}
	{
		std::lock_guard lock(semaphore->mutex);
		semaphore->alive = false;
		semaphore->condition.notify_all();
	}
	delete semaphore;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateEvent(VkDevice device, const VkEventCreateInfo* createInfo,
	                                         const VkAllocationCallbacks* allocator, VkEvent* event)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || event == nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_EVENT_CREATE_INFO || !IsSupportedAllocator(allocator))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	auto created = std::unique_ptr<VkEvent_T>(new (std::nothrow) VkEvent_T);
	if (!created)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	created->device = device;
	created->state.reset(new (std::nothrow) Lumen::MetalVulkan::EventState);
	if (!created->state) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->state->device = device;
	*event = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyEvent(VkDevice device, VkEvent event, const VkAllocationCallbacks* allocator)
{
	if (event == VK_NULL_HANDLE || event->device != device || !IsSupportedAllocator(allocator))
	{
		return;
	}
	{
		std::lock_guard lock(event->state->mutex);
		event->state->alive = false;
		event->state->condition.notify_all();
	}
	delete event;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetEventStatus(VkDevice device, VkEvent event)
{
	if (device == VK_NULL_HANDLE || event == VK_NULL_HANDLE || event->device != device)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::lock_guard lock(event->state->mutex);
	if (device->lost.load(std::memory_order_acquire)) return VK_ERROR_DEVICE_LOST;
	return event->state->set ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetEvent(VkDevice device, VkEvent event)
{
	if (event == VK_NULL_HANDLE || event->device != device)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::lock_guard lock(event->state->mutex);
	event->state->set = true;
	event->state->condition.notify_all();
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetEvent(VkDevice device, VkEvent event)
{
	if (event == VK_NULL_HANDLE || event->device != device)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::lock_guard lock(event->state->mutex);
	// The GPU timeline can retire a use before its CPU callback runs. Never
	// allow that old callback to signal a newly reset/reused event.
	++event->state->generation;
	event->state->set = false;
	return VK_SUCCESS;
}

} // extern "C"

namespace {

#define LUMEN_VK_ENTRY(name) {#name, reinterpret_cast<PFN_vkVoidFunction>(name)}

PFN_vkVoidFunction ResolveFunction(const char* name)
{
	if (name == nullptr)
	{
		return nullptr;
	}
	static const std::unordered_map<std::string, PFN_vkVoidFunction> functions = {
	    LUMEN_VK_ENTRY(vkGetInstanceProcAddr), LUMEN_VK_ENTRY(vkGetDeviceProcAddr),
	    LUMEN_VK_ENTRY(vkEnumerateInstanceVersion), LUMEN_VK_ENTRY(vkEnumerateInstanceExtensionProperties),
	    LUMEN_VK_ENTRY(vkEnumerateInstanceLayerProperties), LUMEN_VK_ENTRY(vkCreateInstance),
	    LUMEN_VK_ENTRY(vkDestroyInstance), LUMEN_VK_ENTRY(vkEnumeratePhysicalDevices),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceFeatures), LUMEN_VK_ENTRY(vkGetPhysicalDeviceFeatures2),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceProperties2), LUMEN_VK_ENTRY(vkGetPhysicalDeviceMemoryProperties2),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceFormatProperties2), LUMEN_VK_ENTRY(vkGetPhysicalDeviceImageFormatProperties2),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceProperties), LUMEN_VK_ENTRY(vkGetPhysicalDeviceQueueFamilyProperties),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceMemoryProperties), LUMEN_VK_ENTRY(vkGetPhysicalDeviceFormatProperties),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceImageFormatProperties), LUMEN_VK_ENTRY(vkEnumerateDeviceExtensionProperties),
	    LUMEN_VK_ENTRY(vkCreateDevice), LUMEN_VK_ENTRY(vkDestroyDevice), LUMEN_VK_ENTRY(vkGetDeviceQueue),
	    LUMEN_VK_ENTRY(vkCreateDebugUtilsMessengerEXT), LUMEN_VK_ENTRY(vkDestroyDebugUtilsMessengerEXT),
	    LUMEN_VK_ENTRY(vkCreateMacOSSurfaceMVK), LUMEN_VK_ENTRY(vkCreateIOSSurfaceMVK),
	    LUMEN_VK_ENTRY(vkCreateMetalSurfaceEXT),
	    LUMEN_VK_ENTRY(vkDestroySurfaceKHR), LUMEN_VK_ENTRY(vkGetPhysicalDeviceSurfaceSupportKHR),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR), LUMEN_VK_ENTRY(vkGetPhysicalDeviceSurfaceFormatsKHR),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceSurfacePresentModesKHR), LUMEN_VK_ENTRY(vkCreateSwapchainKHR),
	    LUMEN_VK_ENTRY(vkDestroySwapchainKHR), LUMEN_VK_ENTRY(vkGetSwapchainImagesKHR),
	    LUMEN_VK_ENTRY(vkAcquireNextImageKHR), LUMEN_VK_ENTRY(vkQueuePresentKHR),
	    LUMEN_VK_ENTRY(vkAllocateMemory), LUMEN_VK_ENTRY(vkFreeMemory), LUMEN_VK_ENTRY(vkMapMemory),
	    LUMEN_VK_ENTRY(vkUnmapMemory), LUMEN_VK_ENTRY(vkFlushMappedMemoryRanges),
	    LUMEN_VK_ENTRY(vkInvalidateMappedMemoryRanges), LUMEN_VK_ENTRY(vkCreateBuffer),
	    LUMEN_VK_ENTRY(vkDestroyBuffer), LUMEN_VK_ENTRY(vkGetBufferMemoryRequirements),
	    LUMEN_VK_ENTRY(vkGetBufferDeviceAddress),
	    LUMEN_VK_ENTRY(vkGetBufferMemoryRequirements2), LUMEN_VK_ENTRY(vkBindBufferMemory2),
	    LUMEN_VK_ENTRY(vkGetImageMemoryRequirements2), LUMEN_VK_ENTRY(vkBindImageMemory2),
	    LUMEN_VK_ENTRY(vkGetPhysicalDeviceToolProperties), LUMEN_VK_ENTRY(vkSetDebugUtilsObjectNameEXT),
	    LUMEN_VK_ENTRY(vkBindBufferMemory), LUMEN_VK_ENTRY(vkCreateImage), LUMEN_VK_ENTRY(vkDestroyImage),
	    LUMEN_VK_ENTRY(vkGetImageMemoryRequirements), LUMEN_VK_ENTRY(vkBindImageMemory),
	    LUMEN_VK_ENTRY(vkCreateImageView), LUMEN_VK_ENTRY(vkDestroyImageView), LUMEN_VK_ENTRY(vkCreateSampler),
	    LUMEN_VK_ENTRY(vkDestroySampler), LUMEN_VK_ENTRY(vkCreateCommandPool), LUMEN_VK_ENTRY(vkDestroyCommandPool),
	    LUMEN_VK_ENTRY(vkResetCommandPool),
	    LUMEN_VK_ENTRY(vkAllocateCommandBuffers), LUMEN_VK_ENTRY(vkFreeCommandBuffers),
	    LUMEN_VK_ENTRY(vkBeginCommandBuffer), LUMEN_VK_ENTRY(vkEndCommandBuffer),
	    LUMEN_VK_ENTRY(vkResetCommandBuffer), LUMEN_VK_ENTRY(vkQueueSubmit), LUMEN_VK_ENTRY(vkQueueWaitIdle),
	    LUMEN_VK_ENTRY(vkDeviceWaitIdle), LUMEN_VK_ENTRY(vkCreateFence), LUMEN_VK_ENTRY(vkDestroyFence),
	    LUMEN_VK_ENTRY(vkGetFenceStatus), LUMEN_VK_ENTRY(vkResetFences), LUMEN_VK_ENTRY(vkWaitForFences),
	    LUMEN_VK_ENTRY(vkCreateSemaphore), LUMEN_VK_ENTRY(vkDestroySemaphore), LUMEN_VK_ENTRY(vkCreateEvent),
	    LUMEN_VK_ENTRY(vkGetSemaphoreCounterValue), LUMEN_VK_ENTRY(vkWaitSemaphores), LUMEN_VK_ENTRY(vkSignalSemaphore),
	    LUMEN_VK_ENTRY(vkDestroyEvent), LUMEN_VK_ENTRY(vkGetEventStatus), LUMEN_VK_ENTRY(vkSetEvent),
	    LUMEN_VK_ENTRY(vkResetEvent), LUMEN_VK_ENTRY(vkCreateShaderModule), LUMEN_VK_ENTRY(vkDestroyShaderModule),
	    LUMEN_VK_ENTRY(vkCreateDescriptorSetLayout), LUMEN_VK_ENTRY(vkDestroyDescriptorSetLayout),
	    LUMEN_VK_ENTRY(vkCreateDescriptorPool), LUMEN_VK_ENTRY(vkDestroyDescriptorPool),
	    LUMEN_VK_ENTRY(vkResetDescriptorPool),
	    LUMEN_VK_ENTRY(vkAllocateDescriptorSets), LUMEN_VK_ENTRY(vkFreeDescriptorSets),
	    LUMEN_VK_ENTRY(vkUpdateDescriptorSets), LUMEN_VK_ENTRY(vkCreatePipelineLayout),
	    LUMEN_VK_ENTRY(vkDestroyPipelineLayout), LUMEN_VK_ENTRY(vkCreateRenderPass),
	    LUMEN_VK_ENTRY(vkDestroyRenderPass), LUMEN_VK_ENTRY(vkCreateFramebuffer),
	    LUMEN_VK_ENTRY(vkDestroyFramebuffer), LUMEN_VK_ENTRY(vkCreateGraphicsPipelines),
	    LUMEN_VK_ENTRY(vkCreateComputePipelines), LUMEN_VK_ENTRY(vkDestroyPipeline),
	    LUMEN_VK_ENTRY(vkCreatePipelineCache), LUMEN_VK_ENTRY(vkDestroyPipelineCache),
	    LUMEN_VK_ENTRY(vkGetPipelineCacheData), LUMEN_VK_ENTRY(vkMergePipelineCaches),
	    LUMEN_VK_ENTRY(vkCmdBeginRenderPass), LUMEN_VK_ENTRY(vkCmdEndRenderPass),
	    LUMEN_VK_ENTRY(vkCmdBeginRendering), LUMEN_VK_ENTRY(vkCmdEndRendering), LUMEN_VK_ENTRY(vkCmdPipelineBarrier2),
	    LUMEN_VK_ENTRY(vkCmdSetViewport), LUMEN_VK_ENTRY(vkCmdSetViewportWithCount), LUMEN_VK_ENTRY(vkCmdSetScissorWithCount),
	    LUMEN_VK_ENTRY(vkCmdBindVertexBuffers2), LUMEN_VK_ENTRY(vkCmdSetDepthTestEnable), LUMEN_VK_ENTRY(vkCmdSetDepthWriteEnable),
	    LUMEN_VK_ENTRY(vkCmdSetDepthCompareOp), LUMEN_VK_ENTRY(vkCmdSetStencilTestEnable), LUMEN_VK_ENTRY(vkCmdSetStencilOp),
	    LUMEN_VK_ENTRY(vkCmdSetDepthBiasEnable), LUMEN_VK_ENTRY(vkCmdSetDepthBias), LUMEN_VK_ENTRY(vkCmdSetCullMode),
	    LUMEN_VK_ENTRY(vkCmdSetFrontFace), LUMEN_VK_ENTRY(vkCmdSetRasterizerDiscardEnable),
	    LUMEN_VK_ENTRY(vkCmdSetPrimitiveRestartEnable), LUMEN_VK_ENTRY(vkCmdSetBlendConstants),
	    LUMEN_VK_ENTRY(vkCmdSetPrimitiveTopology), LUMEN_VK_ENTRY(vkCmdSetDepthBoundsTestEnable),
	    LUMEN_VK_ENTRY(vkCmdFillBuffer), LUMEN_VK_ENTRY(vkCmdDispatchIndirect),
	    LUMEN_VK_ENTRY(vkCmdDrawIndirect), LUMEN_VK_ENTRY(vkCmdDrawIndexedIndirect),
        LUMEN_VK_ENTRY(vkCmdDrawIndirectCount), LUMEN_VK_ENTRY(vkCmdDrawIndexedIndirectCount),
	    LUMEN_VK_ENTRY(vkCmdBindPipeline), LUMEN_VK_ENTRY(vkCmdBindDescriptorSets),
	    LUMEN_VK_ENTRY(vkCmdPushDescriptorSetKHR),
	    LUMEN_VK_ENTRY(vkCmdPushConstants), LUMEN_VK_ENTRY(vkCmdBindVertexBuffers),
	    LUMEN_VK_ENTRY(vkCmdBindIndexBuffer), LUMEN_VK_ENTRY(vkCmdDraw), LUMEN_VK_ENTRY(vkCmdDrawIndexed),
	    LUMEN_VK_ENTRY(vkCmdDispatch), LUMEN_VK_ENTRY(vkCmdCopyBuffer), LUMEN_VK_ENTRY(vkCmdCopyBufferToImage),
	    LUMEN_VK_ENTRY(vkCmdCopyImageToBuffer), LUMEN_VK_ENTRY(vkCmdCopyImage), LUMEN_VK_ENTRY(vkCmdBlitImage),
        LUMEN_VK_ENTRY(vkCmdResolveImage),
	    LUMEN_VK_ENTRY(vkCmdClearColorImage), LUMEN_VK_ENTRY(vkCmdPipelineBarrier), LUMEN_VK_ENTRY(vkCmdSetEvent),
	    LUMEN_VK_ENTRY(vkCmdClearDepthStencilImage),
	    LUMEN_VK_ENTRY(vkCmdSetEvent2),
	    LUMEN_VK_ENTRY(vkCmdSetScissor), LUMEN_VK_ENTRY(vkCmdSetLineWidth), LUMEN_VK_ENTRY(vkCmdSetStencilCompareMask),
	    LUMEN_VK_ENTRY(vkCmdSetStencilWriteMask), LUMEN_VK_ENTRY(vkCmdSetStencilReference),
	    LUMEN_VK_ENTRY(vkCmdSetColorWriteEnableEXT),
	};
	static const std::unordered_map<std::string, const char*> aliases = {
        {"vkGetPhysicalDeviceFeatures2KHR", "vkGetPhysicalDeviceFeatures2"},
        {"vkGetPhysicalDeviceProperties2KHR", "vkGetPhysicalDeviceProperties2"},
        {"vkGetPhysicalDeviceMemoryProperties2KHR", "vkGetPhysicalDeviceMemoryProperties2"},
        {"vkGetPhysicalDeviceFormatProperties2KHR", "vkGetPhysicalDeviceFormatProperties2"},
        {"vkGetPhysicalDeviceImageFormatProperties2KHR", "vkGetPhysicalDeviceImageFormatProperties2"},
        {"vkGetBufferMemoryRequirements2KHR", "vkGetBufferMemoryRequirements2"},
        {"vkGetImageMemoryRequirements2KHR", "vkGetImageMemoryRequirements2"},
        {"vkBindBufferMemory2KHR", "vkBindBufferMemory2"},
        {"vkBindImageMemory2KHR", "vkBindImageMemory2"},
        {"vkGetBufferDeviceAddressKHR", "vkGetBufferDeviceAddress"},
        {"vkGetSemaphoreCounterValueKHR", "vkGetSemaphoreCounterValue"},
        {"vkWaitSemaphoresKHR", "vkWaitSemaphores"},
        {"vkSignalSemaphoreKHR", "vkSignalSemaphore"},
        {"vkCmdBeginRenderingKHR", "vkCmdBeginRendering"},
        {"vkCmdDrawIndirectCountKHR", "vkCmdDrawIndirectCount"},
        {"vkCmdDrawIndexedIndirectCountKHR", "vkCmdDrawIndexedIndirectCount"},
        {"vkCmdEndRenderingKHR", "vkCmdEndRendering"},
        {"vkCmdPipelineBarrier2KHR", "vkCmdPipelineBarrier2"},
        {"vkCmdSetEvent2KHR", "vkCmdSetEvent2"},
        {"vkCmdSetCullModeEXT", "vkCmdSetCullMode"},
        {"vkCmdSetFrontFaceEXT", "vkCmdSetFrontFace"},
        {"vkCmdSetPrimitiveTopologyEXT", "vkCmdSetPrimitiveTopology"},
        {"vkCmdSetViewportWithCountEXT", "vkCmdSetViewportWithCount"},
        {"vkCmdSetScissorWithCountEXT", "vkCmdSetScissorWithCount"},
        {"vkCmdBindVertexBuffers2EXT", "vkCmdBindVertexBuffers2"},
        {"vkCmdSetDepthTestEnableEXT", "vkCmdSetDepthTestEnable"},
        {"vkCmdSetDepthWriteEnableEXT", "vkCmdSetDepthWriteEnable"},
        {"vkCmdSetDepthCompareOpEXT", "vkCmdSetDepthCompareOp"},
        {"vkCmdSetDepthBoundsTestEnableEXT", "vkCmdSetDepthBoundsTestEnable"},
        {"vkCmdSetStencilTestEnableEXT", "vkCmdSetStencilTestEnable"},
        {"vkCmdSetStencilOpEXT", "vkCmdSetStencilOp"},
        {"vkCmdSetRasterizerDiscardEnableEXT", "vkCmdSetRasterizerDiscardEnable"},
        {"vkCmdSetDepthBiasEnableEXT", "vkCmdSetDepthBiasEnable"},
        {"vkCmdSetPrimitiveRestartEnableEXT", "vkCmdSetPrimitiveRestartEnable"},
    };
    if (const auto alias = aliases.find(name); alias != aliases.end()) name = alias->second;
	const auto found = functions.find(name);
	return found == functions.end() ? nullptr : found->second;
}

#undef LUMEN_VK_ENTRY

} // namespace

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name)
{
	if (instance == VK_NULL_HANDLE)
	{
		return IsGlobalFunction(name) ? ResolveFunction(name) : nullptr;
	}
	if (instance != VK_NULL_HANDLE && !instance->alive.load(std::memory_order_acquire))
	{
		return nullptr;
	}
	return ResolveFunction(name);
}
