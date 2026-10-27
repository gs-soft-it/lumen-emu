#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if defined(LUMEN_PS5_NATIVE_METAL)
#include <SDL3/SDL_metal.h>
#endif

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/systemOverlay.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/presentation/window/windowInternal.h"
#include "kernel/memory.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#if defined(__ANDROID__) && defined(LUMEN_PS5_DIRECT_FEX)
#include "DirectPerformanceOptions.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_platform.h>
#if defined(LUMEN_PS5_NATIVE_METAL)
#include <vulkan/vulkan_metal.h>
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
#endif

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF

namespace Libs::Graphics {

struct VulkanExtensions {
	bool enable_validation_layers = false;

	std::vector<const char*>             required_extensions;
	std::vector<vk::ExtensionProperties> available_extensions;
	std::vector<const char*>             required_layers;
	std::vector<vk::LayerProperties>     available_layers;
};

vk::PhysicalDeviceVulkan11Features WindowContext::RequiredVulkan11Features() noexcept {
	vk::PhysicalDeviceVulkan11Features features {};
	features.storageBuffer16BitAccess = VK_TRUE;
	return features;
}

vk::PhysicalDeviceVulkan12Features WindowContext::RequiredVulkan12Features() noexcept {
	vk::PhysicalDeviceVulkan12Features features {};
	features.samplerMirrorClampToEdge  = VK_TRUE;
	features.timelineSemaphore         = VK_TRUE;
	features.shaderOutputLayer         = VK_TRUE;
	features.shaderOutputViewportIndex = VK_TRUE;
	features.bufferDeviceAddress       = VK_TRUE;
	features.storageBuffer8BitAccess   = VK_TRUE;
	features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	features.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
	features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
	return features;
}

vk::PhysicalDeviceVulkan13Features WindowContext::RequiredVulkan13Features() noexcept {
	vk::PhysicalDeviceVulkan13Features features {};
	features.dynamicRendering = VK_TRUE;
	features.synchronization2 = VK_TRUE;
	return features;
}

static bool HasExtension(const std::vector<vk::ExtensionProperties>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(),
	                   [name](const auto& ext) { return strcmp(ext.extensionName, name) == 0; });
}

static bool HasExtension(const std::vector<const char*>& extensions, const char* name) {
	return std::any_of(extensions.begin(), extensions.end(),
	                   [name](const char* ext) { return strcmp(ext, name) == 0; });
}

static bool HasLayer(const std::vector<vk::LayerProperties>& layers, const char* name) {
	return std::any_of(layers.begin(), layers.end(),
	                   [name](const auto& layer) { return strcmp(layer.layerName, name) == 0; });
}

// A surface can disappear between an Android lifecycle event and a WSI call.
// Only that transient condition is recoverable; device/driver failures stay fatal.
static bool SurfaceQuerySucceeded(vk::Result result, const char* operation,
                                  bool allow_surface_lost) {
#if defined(__ANDROID__)
	if (allow_surface_lost && result == vk::Result::eErrorSurfaceLostKHR) {
		return false;
	}
#endif
	RequireVulkanSuccess(result, operation);
	return true;
}

template <typename T, typename Query>
static bool QuerySurfaceList(std::vector<T>& values, const char* operation,
                             bool allow_surface_lost, Query&& query) {
	for (;;) {
		uint32_t count = 0;
		if (!SurfaceQuerySucceeded(query(&count, nullptr), operation, allow_surface_lost)) {
			return false;
		}
		values.resize(count);
		if (count == 0) {
			return true;
		}
		const auto result = query(&count, values.data());
		if (result == vk::Result::eIncomplete) {
			continue;
		}
		if (!SurfaceQuerySucceeded(result, operation, allow_surface_lost)) {
			return false;
		}
		values.resize(count);
		return true;
	}
}

static bool GetSurfaceCapabilities(vk::PhysicalDevice physical_device, vk::SurfaceKHR surface,
                                   SurfaceCapabilities& r, bool allow_surface_lost = false) {
	SurfaceCapabilities next;
	if (!SurfaceQuerySucceeded(physical_device.getSurfaceCapabilitiesKHR(surface, &next.capabilities),
	                           "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", allow_surface_lost) ||
	    !QuerySurfaceList(next.formats, "vkGetPhysicalDeviceSurfaceFormatsKHR", allow_surface_lost,
	                      [&](uint32_t* count, vk::SurfaceFormatKHR* values) {
		                      return physical_device.getSurfaceFormatsKHR(surface, count, values);
	                      }) ||
	    !QuerySurfaceList(next.present_modes, "vkGetPhysicalDeviceSurfacePresentModesKHR", allow_surface_lost,
	                      [&](uint32_t* count, vk::PresentModeKHR* values) {
		                      return physical_device.getSurfacePresentModesKHR(surface, count, values);
	                      })) {
		return false;
	}
	EXIT_NOT_IMPLEMENTED(next.formats.empty());
	EXIT_NOT_IMPLEMENTED(next.present_modes.empty());
	r = std::move(next);
	return true;
}

static bool CheckFormat(vk::PhysicalDevice device, vk::Format format, bool tile,
                        vk::FormatFeatureFlags features) {
	vk::FormatProperties format_props {};
	device.getFormatProperties(format, &format_props);

	const auto supported_features =
	    (tile ? format_props.optimalTilingFeatures : format_props.linearTilingFeatures);
	return (supported_features & features) == features;
}

static uint32_t VulkanFindQueueFamily(vk::PhysicalDevice device, vk::SurfaceKHR surface) {
	EXIT_IF(device == nullptr);
	EXIT_IF(surface == nullptr);

	uint32_t queue_family_count = 0;
	device.getQueueFamilyProperties(&queue_family_count, nullptr);
	std::vector<vk::QueueFamilyProperties> queue_families(queue_family_count);
	device.getQueueFamilyProperties(&queue_family_count, queue_families.data());

	const auto required = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
	for (uint32_t family = 0; family < queue_family_count; family++) {
		const auto& properties             = queue_families[family];
		vk::Bool32  presentation_supported = VK_FALSE;
		RequireVulkanSuccess(device.getSurfaceSupportKHR(family, surface, &presentation_supported),
		                     "vkGetPhysicalDeviceSurfaceSupportKHR");

		LOGF("\tqueue family: %s [count = %u], [present = %s]\n",
		     vk::to_string(properties.queueFlags).c_str(), properties.queueCount,
		     (presentation_supported == VK_TRUE ? "true" : "false"));
		if (properties.queueCount != 0 && (properties.queueFlags & required) == required &&
		    presentation_supported == VK_TRUE) {
			LOGF("\tselected universal queue family %u\n", family);
			return family;
		}
	}
	return static_cast<uint32_t>(-1);
}

// On failure out_device is null and out_rejections says why each device was skipped.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void VulkanFindPhysicalDevice(vk::Instance instance, vk::SurfaceKHR surface,
                                     const std::vector<const char*>& device_extensions,
                                     SurfaceCapabilities&            out_capabilities,
                                     vk::PhysicalDevice& out_device, uint32_t& out_queue_family,
                                     std::string& out_rejections) {
	EXIT_IF(instance == nullptr);
	EXIT_IF(surface == nullptr);

	auto devices = EnumerateVulkan<vk::PhysicalDevice>(
	    "vkEnumeratePhysicalDevices", [&](uint32_t* count, vk::PhysicalDevice* values) {
		    return instance.enumeratePhysicalDevices(count, values);
	    });
	EXIT_NOT_IMPLEMENTED(devices.empty());

	if (Config::GetGpuIndex() >= 0) {
		if (static_cast<size_t>(Config::GetGpuIndex()) < devices.size()) {
			devices = {devices[Config::GetGpuIndex()]};
		} else {
			LOGF("Vulkan GPU index %d is unavailable; selecting automatically\n",
			     Config::GetGpuIndex());
		}
	}

	vk::PhysicalDevice  best_device       = nullptr;
	uint32_t            best_queue_family = static_cast<uint32_t>(-1);
	SurfaceCapabilities best_capabilities;

	for (const auto& device: devices) {
		bool skip_device = false;

		vk::PhysicalDeviceProperties device_properties {};
		device.getProperties(&device_properties);

		const auto reject = [&](const std::string& reason) {
			LOGF("%s\n", reason.c_str());
			out_rejections +=
			    skip_device ? "; " : fmt::format("\n  {}: ", device_properties.deviceName.data());
			out_rejections += reason;
			skip_device = true;
		};

		LOGF("Vulkan device: %s\n", device_properties.deviceName.data());
		if (device_properties.apiVersion < VULKAN_TARGET_API_VERSION) {
			reject(fmt::format("Vulkan {}.{} is required, but device supports only {}.{}.{}",
			                   VK_VERSION_MAJOR(VULKAN_TARGET_API_VERSION),
			                   VK_VERSION_MINOR(VULKAN_TARGET_API_VERSION),
			                   VK_VERSION_MAJOR(device_properties.apiVersion),
			                   VK_VERSION_MINOR(device_properties.apiVersion),
			                   VK_VERSION_PATCH(device_properties.apiVersion)));
			continue;
		}

		vk::PhysicalDeviceFeatures2 device_features2 {};

		vk::PhysicalDeviceVulkan13Features features13 {};

		vk::PhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
		vk::PhysicalDeviceImageViewMinLodFeaturesEXT  image_view_min_lod {};
		color_write_ext.pNext = &image_view_min_lod;

		vk::PhysicalDeviceDepthClipEnableFeaturesEXT depth_clip_enable {};
		depth_clip_enable.pNext = &color_write_ext;

		vk::PhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
		depth_clip_control.pNext = &depth_clip_enable;

		vk::PhysicalDeviceVulkan12Features features12 {};
		vk::PhysicalDeviceVulkan11Features features11 {};
		vk::PhysicalDeviceRobustness2FeaturesEXT robustness2 {};
#if defined(__APPLE__)
		features12.pNext = &depth_clip_control;
#else
		vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR fragment_barycentric {};
		fragment_barycentric.pNext = &depth_clip_control;
		features12.pNext           = &fragment_barycentric;
#endif
		robustness2.pNext      = features12.pNext;
		features11.pNext       = &robustness2;
		features12.pNext       = &features11;
		features13.pNext       = &features12;
		device_features2.pNext = &features13;

		device.getFeatures2(&device_features2);
		const auto required_features12 = WindowContext::RequiredVulkan12Features();
		const auto required_features13 = WindowContext::RequiredVulkan13Features();

		const auto queue_family = VulkanFindQueueFamily(device, surface);
		if (queue_family == static_cast<uint32_t>(-1)) {
			reject("No universal graphics, compute, and presentation queue");
		}

		const auto check_feature = [&](vk::Bool32 supported, const char* name,
		                               vk::Bool32 required = VK_TRUE) {
			if (required == VK_TRUE && supported != VK_TRUE) {
				reject(fmt::format("{} is not supported", name));
			}
		};

#if defined(__APPLE__)
		if (color_write_ext.colorWriteEnable != VK_TRUE) {
			LOGF("colorWriteEnable is not supported\n");
		}
#else
		check_feature(color_write_ext.colorWriteEnable, "colorWriteEnable");
#endif
#if !defined(LUMEN_PS5_NATIVE_METAL)
		check_feature(image_view_min_lod.minLod, "image view minLod");
#else
		// Fractional view LOD is admitted at image-view creation when used.
#endif

		check_feature(depth_clip_control.depthClipControl, "depthClipControl");
#if defined(__APPLE__)
		if (depth_clip_enable.depthClipEnable != VK_TRUE) {
			LOGF("depthClipEnable is not supported\n");
		}
#else
		check_feature(depth_clip_enable.depthClipEnable, "depthClipEnable");
#endif
#if !defined(__APPLE__)
		check_feature(device_features2.features.depthClamp, "depthClamp");
#endif

		check_feature(features12.samplerMirrorClampToEdge, "samplerMirrorClampToEdge",
		              required_features12.samplerMirrorClampToEdge);
		check_feature(features11.storageBuffer16BitAccess, "storageBuffer16BitAccess",
		              WindowContext::RequiredVulkan11Features().storageBuffer16BitAccess);
		check_feature(features12.storageBuffer8BitAccess, "storageBuffer8BitAccess",
		              required_features12.storageBuffer8BitAccess);
		check_feature(features12.timelineSemaphore, "timelineSemaphore",
		              required_features12.timelineSemaphore);
		check_feature(features12.shaderOutputLayer, "shaderOutputLayer",
		              required_features12.shaderOutputLayer);
		check_feature(features12.shaderOutputViewportIndex, "shaderOutputViewportIndex",
		              required_features12.shaderOutputViewportIndex);
		check_feature(features12.bufferDeviceAddress, "bufferDeviceAddress",
		              required_features12.bufferDeviceAddress);
		check_feature(features12.shaderBufferInt64Atomics, "shaderBufferInt64Atomics",
		              required_features12.shaderBufferInt64Atomics);
		check_feature(features12.shaderSampledImageArrayNonUniformIndexing,
		              "shaderSampledImageArrayNonUniformIndexing",
		              required_features12.shaderSampledImageArrayNonUniformIndexing);
		check_feature(features12.shaderStorageImageArrayNonUniformIndexing,
		              "shaderStorageImageArrayNonUniformIndexing",
		              required_features12.shaderStorageImageArrayNonUniformIndexing);
		check_feature(features12.shaderStorageBufferArrayNonUniformIndexing,
		              "shaderStorageBufferArrayNonUniformIndexing",
		              required_features12.shaderStorageBufferArrayNonUniformIndexing);
#if !defined(LUMEN_PS5_NATIVE_METAL)
		check_feature(features13.robustImageAccess, "robustImageAccess");
		check_feature(robustness2.nullDescriptor, "nullDescriptor");
#else
		// Generated image fetch/store/atomic operations use explicit bounds when unavailable.
		// Null descriptors are lowered to zero/no-op shader operations when unavailable.
#endif
		check_feature(features13.dynamicRendering, "dynamicRendering",
		              required_features13.dynamicRendering);
		check_feature(features13.synchronization2, "synchronization2",
		              required_features13.synchronization2);
#if !defined(LUMEN_PS5_NATIVE_METAL)
		check_feature(device_features2.features.sampleRateShading, "sampleRateShading");
#endif
		check_feature(device_features2.features.depthBiasClamp, "depthBiasClamp");
		check_feature(device_features2.features.shaderClipDistance, "shaderClipDistance");
#if !defined(LUMEN_PS5_NATIVE_METAL)
		check_feature(device_features2.features.shaderCullDistance, "shaderCullDistance");
		check_feature(device_features2.features.largePoints, "largePoints");
		check_feature(device_features2.features.multiViewport, "multiViewport");
		check_feature(device_features2.features.fillModeNonSolid, "fillModeNonSolid");
#else
		// Point-size and non-solid polygon modes are admitted at pipeline creation.
#endif

		check_feature(device_features2.features.fragmentStoresAndAtomics,
		              "fragmentStoresAndAtomics");

		check_feature(device_features2.features.samplerAnisotropy, "samplerAnisotropy");
#if !defined(LUMEN_PS5_NATIVE_METAL)
		check_feature(device_features2.features.robustBufferAccess, "robustBufferAccess");
#else
		// The direct compiler checks SSBO/GDS/LDS ranges and sparse BDA mappings explicitly.
#endif
#if defined(__APPLE__)
		if (device_features2.features.depthBounds != VK_TRUE) {
			LOGF("depthBounds is not supported\n");
		}
#else
		check_feature(device_features2.features.depthBounds, "depthBounds");
#endif
		check_feature(device_features2.features.shaderStorageImageWriteWithoutFormat,
		              "shaderStorageImageWriteWithoutFormat");
#if !defined(LUMEN_PS5_NATIVE_METAL)
		check_feature(device_features2.features.shaderImageGatherExtended,
		              "shaderImageGatherExtended");
#endif
		check_feature(device_features2.features.independentBlend, "independentBlend");
#if !defined(LUMEN_PS5_NATIVE_METAL)
		check_feature(device_features2.features.tessellationShader, "tessellationShader");
#else
		// Tessellation and rect-list expansion are checked when their pipeline is requested.
#endif

		if (!skip_device) {
			auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
			    "vkEnumerateDeviceExtensionProperties",
			    [&](uint32_t* count, vk::ExtensionProperties* values) {
				    return device.enumerateDeviceExtensionProperties(nullptr, count, values);
			    });
			EXIT_NOT_IMPLEMENTED(available_extensions.empty());

			for (const char* ext: device_extensions) {
				if (!HasExtension(available_extensions, ext)) {
					reject(fmt::format("{} is not supported", ext));
				}
			}

			if (skip_device) {
				for (const auto& ext: available_extensions) {
					LOGF("Vulkan available extension: %s, version = %u\n", ext.extensionName.data(),
					     ext.specVersion);
				}
			}
		}

		SurfaceCapabilities candidate_capabilities;
		if (!skip_device) {
			GetSurfaceCapabilities(device, surface, candidate_capabilities);

			if (!(candidate_capabilities.capabilities.supportedUsageFlags &
			      vk::ImageUsageFlagBits::eTransferDst)) {
				reject("Surface cannot be destination of blit");
			}
		}

		const auto check_format = [&](vk::Format format, vk::FormatFeatureFlags features,
		                              const char* usage) {
			if (!skip_device && !CheckFormat(device, format, true, features)) {
				reject(fmt::format("Format vk::Format::e{} cannot be used as {}",
				                   vk::to_string(format), usage));
			}
		};

		const auto depth_features = vk::FormatFeatureFlagBits::eDepthStencilAttachment;
		const auto texture_features =
		    vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst;
		const auto storage_features =
		    vk::FormatFeatureFlagBits::eStorageImage | vk::FormatFeatureFlagBits::eTransferDst;

		check_format(vk::Format::eD32Sfloat, depth_features, "depth buffer");
		check_format(vk::Format::eD32SfloatS8Uint, depth_features, "depth buffer");
		check_format(vk::Format::eD16Unorm, depth_features, "depth buffer");
		check_format(vk::Format::eBc3SrgbBlock, texture_features, "texture");
		check_format(vk::Format::eR8G8B8A8Srgb, texture_features, "texture");
		check_format(vk::Format::eR8Unorm, texture_features, "texture");
		check_format(vk::Format::eR8G8Unorm, texture_features, "texture");

		if (!skip_device &&
		    !CheckFormat(device, vk::Format::eR8G8B8A8Srgb, true, storage_features)) {
			LOGF("Format vk::Format::eR8G8B8A8Srgb cannot be used as storage image\n");
			check_format(vk::Format::eR8G8B8A8Unorm, storage_features, "storage image");
		}

		if (!skip_device &&
		    !CheckFormat(device, vk::Format::eB8G8R8A8Srgb, true, storage_features)) {
			LOGF("Format vk::Format::eB8G8R8A8Srgb cannot be used as storage image\n");
			check_format(vk::Format::eB8G8R8A8Unorm, storage_features, "storage image");
		}

		if (!skip_device && device_properties.limits.maxSamplerAnisotropy < 16.0f) {
			reject("maxSamplerAnisotropy < 16.0f");
		}

		if (skip_device) {
			continue;
		}

		if (best_device == nullptr ||
		    device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
			best_device       = device;
			best_queue_family = queue_family;
			best_capabilities = std::move(candidate_capabilities);
		}
	}

	out_device       = best_device;
	out_queue_family = best_queue_family;
	if (best_device != nullptr) {
		out_capabilities = std::move(best_capabilities);
	}
}

static vk::Device VulkanCreateDevice(GraphicContext& graphics,
	                                 const std::vector<const char*>& device_extensions) {
	const auto physical_device = graphics.physical_device;
	const auto queue_family    = graphics.queue_family;
	graphics.fragment_shader_barycentric_enabled = false;
	graphics.geometry_shader_enabled             = false;
	graphics.barycentric_fallback_enabled         = false;
	EXIT_IF(physical_device == nullptr);
	EXIT_IF(queue_family == static_cast<uint32_t>(-1));

	const float               queue_priority = 1.0f;
	vk::DeviceQueueCreateInfo queue_create_info {};
	queue_create_info.queueFamilyIndex = queue_family;
	queue_create_info.queueCount       = 1;
	queue_create_info.pQueuePriorities = &queue_priority;

	vk::PhysicalDeviceColorWriteEnableFeaturesEXT color_write_ext {};
	color_write_ext.colorWriteEnable = VK_TRUE;

	vk::PhysicalDeviceDepthClipEnableFeaturesEXT depth_clip_enable {};
	depth_clip_enable.pNext = &color_write_ext;
	depth_clip_enable.depthClipEnable = VK_TRUE;

	vk::PhysicalDeviceDepthClipControlFeaturesEXT depth_clip_control {};
	vk::PhysicalDeviceImageViewMinLodFeaturesEXT  image_view_min_lod {};
	const bool image_view_min_lod_extension =
	    HasExtension(device_extensions, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
	depth_clip_control.pNext = image_view_min_lod_extension ? &image_view_min_lod : nullptr;
	// MoltenVK lacks VK_EXT_depth_clip_enable and VK_EXT_color_write_enable, so drop those
	// feature structs from the chain on macOS (the renderer falls back to default depth
	// clipping and static color-write masks).
#if !defined(__APPLE__)
	if (image_view_min_lod_extension) image_view_min_lod.pNext = &depth_clip_enable;
	else depth_clip_control.pNext = &depth_clip_enable;
#endif
	depth_clip_control.depthClipControl = VK_TRUE;

	const bool workgroup_layout_extension =
	    HasExtension(device_extensions, VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
	vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR supported_workgroup_layout {};
	vk::PhysicalDeviceVulkan12Features supported_features12 {};
	supported_features12.pNext = workgroup_layout_extension ? &supported_workgroup_layout : nullptr;
	vk::PhysicalDeviceVulkan13Features supported_features13 {};
	supported_features13.pNext = &supported_features12;

	vk::PhysicalDeviceRobustness2FeaturesEXT supported_robustness2 {};
	supported_robustness2.pNext = supported_features13.pNext;
	supported_features13.pNext = &supported_robustness2;

	const bool mesh_extension = HasExtension(device_extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
	vk::PhysicalDeviceMeshShaderFeaturesEXT supported_mesh {};
	supported_mesh.pNext = &supported_features13;
	vk::PhysicalDeviceFeatures2 supported_features2 {};
	supported_features2.pNext = mesh_extension ? static_cast<void*>(&supported_mesh)
	                                           : static_cast<void*>(&supported_features13);
	const bool feedback_extensions =
	    HasExtension(device_extensions, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
	    HasExtension(device_extensions, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
	vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT feedback_layout {};
	vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT feedback_dynamic {};
	if (feedback_extensions) {
		feedback_dynamic.pNext = supported_features2.pNext;
		feedback_layout.pNext  = &feedback_dynamic;
		supported_features2.pNext = &feedback_layout;
	}
	const bool provoking_extension =
	    HasExtension(device_extensions, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
	vk::PhysicalDeviceProvokingVertexFeaturesEXT provoking_vertex {};
	if (provoking_extension) {
		provoking_vertex.pNext = supported_features2.pNext;
		supported_features2.pNext = &provoking_vertex;
	}
	const bool maintenance8_extension =
	    HasExtension(device_extensions, VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
	vk::PhysicalDeviceMaintenance8FeaturesKHR maintenance8 {};
	if (maintenance8_extension) {
		maintenance8.pNext = supported_features2.pNext;
		supported_features2.pNext = &maintenance8;
	}
	const bool image_atomic_int64_extension =
	    HasExtension(device_extensions, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
	vk::PhysicalDeviceShaderImageAtomicInt64FeaturesEXT image_atomic_int64 {};
	if (image_atomic_int64_extension) {
		image_atomic_int64.pNext = supported_features2.pNext;
		supported_features2.pNext = &image_atomic_int64;
	}
	const bool barycentric_extension =
	    HasExtension(device_extensions, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR supported_barycentric {};
	if (barycentric_extension) {
		supported_barycentric.pNext = supported_features2.pNext;
		supported_features2.pNext   = &supported_barycentric;
	}
	vk::PhysicalDeviceImageViewMinLodFeaturesEXT supported_min_lod {};
	if (image_view_min_lod_extension) {
		supported_min_lod.pNext = supported_features2.pNext;
		supported_features2.pNext = &supported_min_lod;
	}
	physical_device.getFeatures2(&supported_features2);
	image_view_min_lod.minLod = supported_min_lod.minLod;
	graphics.image_view_min_lod_enabled = image_view_min_lod.minLod;
	graphics.robust_image_access_enabled = supported_features13.robustImageAccess;
	const bool barycentric_supported =
	    barycentric_extension && supported_barycentric.fragmentShaderBarycentric == VK_TRUE;
#if defined(__ANDROID__) && defined(LUMEN_PS5_DIRECT_FEX)
	const auto& direct_options = Lumen::Ps5::DirectPerformance::Get();
	if (!direct_options.valid) {
		EXIT("PS5_GPU_BARYCENTRIC_FALLBACK_DENIED reason=invalid-selector\n");
	}
	const bool barycentric_fallback_requested = direct_options.barycentric_fallback;
	const bool geometry_supported = supported_features2.features.geometryShader == VK_TRUE;
	const bool enable_barycentric_fallback =
	    barycentric_fallback_requested && geometry_supported && !barycentric_supported;
#endif
	graphics.shader_image_int64_atomics_enabled = image_atomic_int64.shaderImageInt64Atomics;

	auto features12 = WindowContext::RequiredVulkan12Features();
	// Buffer atomics are required by particular shaders, not by every game.
	// Preserve native 64-bit atomic semantics only when the device supports them.
	features12.shaderBufferInt64Atomics = supported_features12.shaderBufferInt64Atomics;
	features12.shaderSharedInt64Atomics = supported_features12.shaderSharedInt64Atomics;
	vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR workgroup_layout {};
	workgroup_layout.workgroupMemoryExplicitLayout =
	    supported_workgroup_layout.workgroupMemoryExplicitLayout;
	workgroup_layout.pNext = &depth_clip_control;
	features12.pNext = workgroup_layout_extension ? static_cast<void*>(&workgroup_layout)
	                                             : static_cast<void*>(&depth_clip_control);
	auto features11 = WindowContext::RequiredVulkan11Features();
	features11.pNext = features12.pNext;
	features12.pNext = &features11;
	if (!features12.shaderSharedInt64Atomics || !workgroup_layout.workgroupMemoryExplicitLayout) {
		Log::WriteToConsoleAndLog(fmt::format(
		    "WARNING: Native 64-bit LDS atomics are unavailable: shaderSharedInt64Atomics={}, "
		    "workgroupMemoryExplicitLayout={}. Continuing with the selected Vulkan device.\n",
		    features12.shaderSharedInt64Atomics != VK_FALSE,
		    workgroup_layout.workgroupMemoryExplicitLayout != VK_FALSE));
	}
	graphics.mesh_shader_enabled = mesh_extension && supported_mesh.meshShader;

	vk::PhysicalDeviceSubgroupSizeControlProperties subgroup_size_control {};

	vk::PhysicalDeviceVulkan11Properties properties11 {};
	properties11.pNext = &subgroup_size_control;

	vk::PhysicalDeviceFloatControlsProperties float_controls {};
	float_controls.pNext = &properties11;
	vk::PhysicalDeviceProperties2 properties2 {};
	properties2.pNext = &float_controls;

	if (graphics.mesh_shader_enabled) {
		subgroup_size_control.pNext = &graphics.mesh_shader_properties;
	}
	physical_device.getProperties2(&properties2);
	EXIT_NOT_IMPLEMENTED(!(properties11.subgroupSupportedOperations & vk::SubgroupFeatureFlagBits::eVote));

	graphics.subgroup_size                 = properties11.subgroupSize;
	graphics.min_subgroup_size             = subgroup_size_control.minSubgroupSize;
	graphics.max_subgroup_size             = subgroup_size_control.maxSubgroupSize;
	graphics.max_compute_workgroup_subgroups = subgroup_size_control.maxComputeWorkgroupSubgroups;
	graphics.required_subgroup_size_stages = subgroup_size_control.requiredSubgroupSizeStages;
	graphics.compute_subgroup_size_control_enabled =
	    supported_features13.subgroupSizeControl == VK_TRUE &&
	    (graphics.required_subgroup_size_stages & vk::ShaderStageFlagBits::eCompute) &&
	    subgroup_size_control.minSubgroupSize <= 64 &&
	    subgroup_size_control.maxSubgroupSize >= 64;
	graphics.mesh_subgroup_size_control_enabled = graphics.mesh_shader_enabled &&
	    supported_features13.subgroupSizeControl == VK_TRUE &&
	    bool(graphics.required_subgroup_size_stages & vk::ShaderStageFlagBits::eMeshEXT);
	graphics.fragment_subgroup_size_control_enabled =
	    supported_features13.subgroupSizeControl == VK_TRUE &&
	    bool(graphics.required_subgroup_size_stages & vk::ShaderStageFlagBits::eFragment);

	LOGF("Vulkan subgroup: default=%u min=%u max=%u stages=0x%08x size_control=%s wave64=%s\n",
	     graphics.subgroup_size, graphics.min_subgroup_size, graphics.max_subgroup_size,
	     static_cast<vk::ShaderStageFlags::MaskType>(graphics.required_subgroup_size_stages),
	     graphics.compute_subgroup_size_control_enabled ? "true" : "false",
	     graphics.SupportsComputeWave64() ? "true" : "false");
	LOGF("Vulkan mesh subgroup size control: %s\n",
	     graphics.mesh_subgroup_size_control_enabled ? "enabled" : "unavailable");
	LOGF("Vulkan fragment subgroup size control: %s\n",
	     graphics.fragment_subgroup_size_control_enabled ? "enabled" : "unavailable");
	graphics.provoking_vertex_last_enabled = provoking_extension && provoking_vertex.provokingVertexLast;
	graphics.attachment_feedback_loop_enabled =
	    feedback_extensions && feedback_layout.attachmentFeedbackLoopLayout &&
	    feedback_dynamic.attachmentFeedbackLoopDynamicState;
	graphics.maintenance8_enabled = maintenance8_extension && maintenance8.maintenance8;
	LOGF("Vulkan maintenance8 support: %s\n",
	     graphics.maintenance8_enabled ? "true" : "false");
	LOGF("Vulkan depth feedback support: %s\n",
	     graphics.attachment_feedback_loop_enabled ? "true" : "false");
	if (graphics.mesh_shader_enabled) {
		LOGF("Vulkan MeshEXT: invocations=%u vertices=%u primitives=%u shared=%u\n",
		     graphics.mesh_shader_properties.maxMeshWorkGroupInvocations,
		     graphics.mesh_shader_properties.maxMeshOutputVertices,
		     graphics.mesh_shader_properties.maxMeshOutputPrimitives,
		     graphics.mesh_shader_properties.maxMeshSharedMemorySize);
	}
	// VulkanFindPhysicalDevice already checked the required creation features. These
	// requirements are specific to this creation path and are not part of device selection.
	EXIT_NOT_IMPLEMENTED(supported_features2.features.shaderInt64 != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(supported_features2.features.vertexPipelineStoresAndAtomics != VK_TRUE);
	EXIT_NOT_IMPLEMENTED(supported_features2.features.dualSrcBlend != VK_TRUE);
	vk::PhysicalDeviceFeatures device_features {};
#if defined(__ANDROID__) && defined(LUMEN_PS5_DIRECT_FEX)
	// Geometry is optional and only requested for the explicit fallback path.
	device_features.geometryShader = enable_barycentric_fallback ? VK_TRUE : VK_FALSE;
#endif
	device_features.fragmentStoresAndAtomics = VK_TRUE;
	device_features.samplerAnisotropy        = VK_TRUE;
	device_features.robustBufferAccess       = supported_features2.features.robustBufferAccess;
#if !defined(__APPLE__)
	device_features.depthBounds = VK_TRUE; // unsupported by MoltenVK
	device_features.depthClamp  = VK_TRUE;
#endif
	device_features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
	device_features.shaderImageGatherExtended            = supported_features2.features.shaderImageGatherExtended;
	device_features.independentBlend                     = VK_TRUE;
	device_features.dualSrcBlend                         = VK_TRUE;
	device_features.tessellationShader = supported_features2.features.tessellationShader;
	device_features.sampleRateShading                    = supported_features2.features.sampleRateShading;
	device_features.depthBiasClamp                       = VK_TRUE;
	device_features.shaderClipDistance                   = VK_TRUE;
	device_features.shaderCullDistance                   = supported_features2.features.shaderCullDistance;
	device_features.largePoints = supported_features2.features.largePoints;
	device_features.multiViewport                        = supported_features2.features.multiViewport;
	device_features.fillModeNonSolid = supported_features2.features.fillModeNonSolid;
	device_features.vertexPipelineStoresAndAtomics       = VK_TRUE;
	graphics.sample_rate_shading_enabled = device_features.sampleRateShading;
	graphics.shader_cull_distance_enabled = device_features.shaderCullDistance;
	graphics.shader_image_gather_extended_enabled = device_features.shaderImageGatherExtended;
	graphics.multi_viewport_enabled = device_features.multiViewport;
	graphics.large_points_enabled = device_features.largePoints;
	graphics.fill_mode_non_solid_enabled = device_features.fillModeNonSolid;
	graphics.tessellation_shader_enabled = device_features.tessellationShader;
	device_features.shaderInt64 = VK_TRUE;
	device_features.shaderFloat64 =
	    supported_features2.features.shaderFloat64 &&
	    float_controls.shaderSignedZeroInfNanPreserveFloat64;
	// if (device_features.shaderFloat64 && !float_controls.shaderDenormPreserveFloat64) {
	// 	Log::WriteToConsoleAndLog(
	// 	    "WARNING: Vulkan device does not guarantee FP64 denormal preservation; "
	// 	    "continuing with native FP64 arithmetic. Very small values may be flushed to zero.\n");
	// }

	vk::PhysicalDeviceRobustness2FeaturesEXT robustness2 {};
#if defined(__APPLE__)
	robustness2.pNext = &features12;
#else
	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR fragment_barycentric {};
	if (barycentric_supported) {
		fragment_barycentric.pNext                     = &features12;
		fragment_barycentric.fragmentShaderBarycentric = VK_TRUE;
		robustness2.pNext                              = &fragment_barycentric;
	} else {
		robustness2.pNext = &features12;
	}
#endif
	robustness2.robustBufferAccess2 = supported_robustness2.robustBufferAccess2;
	robustness2.robustImageAccess2  = supported_robustness2.robustImageAccess2;
	robustness2.nullDescriptor = HasExtension(device_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)
	    ? supported_robustness2.nullDescriptor : VK_FALSE;
	graphics.null_descriptor_enabled = robustness2.nullDescriptor == VK_TRUE;
	graphics.null_descriptor_fallback_enabled = !graphics.null_descriptor_enabled;

	auto features13 = WindowContext::RequiredVulkan13Features();
	features13.pNext = HasExtension(device_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)
	                       ? static_cast<void*>(&robustness2) : robustness2.pNext;
	features13.robustImageAccess   = supported_features13.robustImageAccess;
	features13.subgroupSizeControl =
	    (graphics.compute_subgroup_size_control_enabled || graphics.mesh_subgroup_size_control_enabled ||
	     graphics.fragment_subgroup_size_control_enabled)
	        ? VK_TRUE : VK_FALSE;

	LOGF("Vulkan robustness: robustImageAccess=%s robustImageAccess2=%s\n",
	     features13.robustImageAccess == VK_TRUE ? "true" : "false",
	     robustness2.robustImageAccess2 == VK_TRUE ? "true" : "false");

	void* core_feature_chain = &features13;
#if defined(LUMEN_PS5_NATIVE_METAL)
	// Native adapter exposes the Vulkan 1.2 contract plus individual extensions.
	vk::PhysicalDeviceDynamicRenderingFeaturesKHR dynamic_rendering {};
	dynamic_rendering.dynamicRendering = features13.dynamicRendering;
	dynamic_rendering.pNext = features13.pNext;
	vk::PhysicalDeviceSynchronization2FeaturesKHR synchronization2 {};
	synchronization2.synchronization2 = features13.synchronization2;
	synchronization2.pNext = &dynamic_rendering;
	vk::PhysicalDeviceImageRobustnessFeaturesEXT image_robustness {};
	image_robustness.robustImageAccess = HasExtension(device_extensions, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME)
	    ? supported_features13.robustImageAccess : VK_FALSE;
	image_robustness.pNext = &synchronization2;
	core_feature_chain = image_robustness.robustImageAccess
	    ? static_cast<void*>(&image_robustness) : static_cast<void*>(&synchronization2);
	graphics.robust_image_access_enabled = image_robustness.robustImageAccess;
#endif
	vk::DeviceCreateInfo create_info {};
	vk::PhysicalDeviceMeshShaderFeaturesEXT mesh_features {};
	mesh_features.pNext                 = core_feature_chain;
	mesh_features.meshShader            = graphics.mesh_shader_enabled;
	feedback_dynamic.pNext =
	    mesh_extension ? static_cast<void*>(&mesh_features) : core_feature_chain;
	create_info.pNext = graphics.attachment_feedback_loop_enabled
	                        ? static_cast<void*>(&feedback_layout)
	                        : feedback_dynamic.pNext;
	if (graphics.provoking_vertex_last_enabled) {
		provoking_vertex.pNext = const_cast<void*>(create_info.pNext);
		provoking_vertex.transformFeedbackPreservesProvokingVertex = VK_FALSE;
		create_info.pNext = &provoking_vertex;
	}
	if (graphics.shader_image_int64_atomics_enabled) {
		image_atomic_int64.pNext = const_cast<void*>(create_info.pNext);
		image_atomic_int64.sparseImageInt64Atomics = VK_FALSE;
		create_info.pNext = &image_atomic_int64;
	}
	if (graphics.maintenance8_enabled) {
		maintenance8.pNext = const_cast<void*>(create_info.pNext);
		create_info.pNext = &maintenance8;
	}
	create_info.pQueueCreateInfos       = &queue_create_info;
	create_info.queueCreateInfoCount    = 1;
	create_info.enabledExtensionCount   = static_cast<uint32_t>(device_extensions.size());
	create_info.ppEnabledExtensionNames = device_extensions.data();
	create_info.pEnabledFeatures        = &device_features;

	vk::Device device = nullptr;

	auto result = physical_device.createDevice(&create_info, nullptr, &device);
	if (result != vk::Result::eSuccess) {
		LOGF("vkCreateDevice failed: %s\n", vk::to_string(result).c_str());
		return nullptr;
	}
	// Record the feature actually enabled in the successful device-create chain.
	graphics.shader_buffer_int64_atomics_enabled = features12.shaderBufferInt64Atomics == VK_TRUE;
#if !defined(__APPLE__)
	graphics.fragment_shader_barycentric_enabled = barycentric_supported;
#endif
	graphics.geometry_shader_enabled = device_features.geometryShader == VK_TRUE;
#if defined(__ANDROID__) && defined(LUMEN_PS5_DIRECT_FEX)
	graphics.barycentric_fallback_enabled = enable_barycentric_fallback;
	LOGF("PS5_GPU_BARYCENTRIC_FALLBACK requested=%u geometrySupported=%u "
	     "nativeBarycentric=%u geometryEnabled=%u enabled=%u\n",
	     barycentric_fallback_requested ? 1u : 0u, geometry_supported ? 1u : 0u,
	     graphics.fragment_shader_barycentric_enabled ? 1u : 0u,
	     graphics.geometry_shader_enabled ? 1u : 0u,
	     graphics.barycentric_fallback_enabled ? 1u : 0u);
#endif
	LOGF("Fragment shader barycentric: %s; final SPIR-V admission is enforced\n",
	     graphics.fragment_shader_barycentric_enabled ? "enabled" : "unavailable");

	return device;
}

static void VulkanGetExtensions(VulkanExtensions& r) {
#if defined(LUMEN_PS5_NATIVE_METAL)
	r.required_extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME};
#else
	uint32_t required_extensions_count = 0;

	const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&required_extensions_count);
	EXIT_NOT_IMPLEMENTED(extensions == nullptr);
	EXIT_NOT_IMPLEMENTED(required_extensions_count == 0);
	r.required_extensions.assign(extensions, extensions + required_extensions_count);
#endif

	r.available_extensions =
	    EnumerateVulkan<vk::ExtensionProperties>( // @suppress("Ambiguous problem")
	        "vkEnumerateInstanceExtensionProperties",
	        [](uint32_t* count, vk::ExtensionProperties* values) {
		        return vk::enumerateInstanceExtensionProperties(nullptr, count, values);
	        });

	r.enable_validation_layers = Config::VulkanValidationEnabled();
	if (Config::HdrEnabled() &&
	    HasExtension(r.available_extensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) &&
	    !HasExtension(r.required_extensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
		r.required_extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
	}

	if (HasExtension(r.available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
		r.required_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	} else {
		r.enable_validation_layers = false;
	}

	for (const char* ext: r.required_extensions) {
		LOGF("Vulkan required extension: %s\n", ext);
	}

	for (const auto& ext: r.available_extensions) {
		LOGF("Vulkan available extension: %s, version = %u\n", ext.extensionName.data(),
		     ext.specVersion);
	}

	r.available_layers = EnumerateVulkan<vk::LayerProperties>( // @suppress("Ambiguous problem")
	    "vkEnumerateInstanceLayerProperties", [](uint32_t* count, vk::LayerProperties* values) {
		    return vk::enumerateInstanceLayerProperties(count, values);
	    });

	for (const auto& l: r.available_layers) {
		LOGF("Vulkan available layer: %s, specVersion = %u, implVersion = %u, %s\n",
		     l.layerName.data(), l.specVersion, l.implementationVersion, l.description.data());
	}

	r.required_layers = {"VK_LAYER_KHRONOS_validation"};

	if (r.enable_validation_layers) {
		for (const char* l: r.required_layers) {
			if (!HasLayer(r.available_layers, l)) {
				LOGF("no validation layer: %s\n", l);
				r.enable_validation_layers = false;
				break;
			}
		}
	}

	if (r.enable_validation_layers) {
		auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
		    "vkEnumerateInstanceExtensionProperties",
		    [](uint32_t* count, vk::ExtensionProperties* values) {
			    return vk::enumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
			                                                    count, values);
		    });

		for (const auto& ext: available_extensions) {
			LOGF("VK_LAYER_KHRONOS_validation available extension: %s, version = %u\n",
			     ext.extensionName.data(), ext.specVersion);
		}

		if (HasExtension(available_extensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
			r.required_extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
		} else {
			r.enable_validation_layers = false;
		}
	}
}

static VKAPI_ATTR vk::Bool32 VKAPI_CALL VulkanDebugMessengerCallback(
    vk::DebugUtilsMessageSeverityFlagBitsEXT      message_severity,
    vk::DebugUtilsMessageTypeFlagsEXT             message_types,
    const vk::DebugUtilsMessengerCallbackDataEXT* callback_data, void* /*user_data*/) {
	EXIT_IF(callback_data == nullptr);
	EXIT_IF(callback_data->pMessage == nullptr);

	const char*     severity_str   = nullptr;
	fmt::text_style severity_style = Log::Color::Default;
	bool            skip           = false;
	bool            error          = false;
	bool            debug_printf   = false;
	switch (message_severity) {
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose:
			severity_str   = "V";
			severity_style = Log::Color::BrightWhite;
			skip           = true;
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo:
			if ((message_types & vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation) &&
			    Config::SpirvDebugPrintfEnabled() && callback_data->pMessageIdName != nullptr &&
			    strcmp(callback_data->pMessageIdName, "UNASSIGNED-DEBUG-PRINTF") == 0) {
				debug_printf   = true;
				severity_style = Log::Color::BrightYellow;
				skip           = true;
			} else {
				severity_str   = "I";
				severity_style = Log::Color::Default;
				skip           = true;
			}
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning:
			severity_str   = "W";
			severity_style = Log::Color::Red;
			break;
		case vk::DebugUtilsMessageSeverityFlagBitsEXT::eError:
			severity_str   = "E";
			severity_style = Log::Color::BrightRed;
			// Only validation errors are fatal; GENERAL-type errors can come
			// from unrelated loader/layer issues (e.g. a broken overlay).
			error = static_cast<bool>(message_types &
			                          vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation);
			break;
		default: severity_str = "?";
	}

	if (error) {
		EXIT_COLOR(severity_style, "[Vulkan][%s][%u]: %s\n", severity_str,
		           static_cast<uint32_t>(message_types), callback_data->pMessage);
	}

	if (!skip) {
		LOGF_COLOR(severity_style, "[Vulkan][%s][%u]: %s\n", severity_str,
		           static_cast<uint32_t>(message_types), callback_data->pMessage);
	}

	if (debug_printf) {
		auto strs = Common::Split(std::string(callback_data->pMessage), '|');
		if (!strs.empty()) {
			LOGF_COLOR(severity_style, "%s\n", strs[strs.size() - 1].c_str());
		}
	}

	return VK_FALSE;
}

static VKAPI_ATTR vk::Result VKAPI_CALL VulkanCreateDebugUtilsMessengerEXT(
    vk::Instance instance, const vk::DebugUtilsMessengerCreateInfoEXT* create_info,
    const vk::AllocationCallbacks* allocator, vk::DebugUtilsMessengerEXT* messenger) {
	EXIT_IF(instance == nullptr);

	if (auto func = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDebugUtilsMessengerEXT; func != nullptr) {
		return instance.createDebugUtilsMessengerEXT(create_info, allocator, messenger);
	}
	return vk::Result::eErrorExtensionNotPresent;
}

static void VulkanCheckInstanceVersion() {
	uint32_t version = VK_API_VERSION_1_0;

	if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion != nullptr) {
		auto result = vk::enumerateInstanceVersion(&version);
		if (result != vk::Result::eSuccess) {
			EXIT("Could not query Vulkan loader version: %s\n", vk::to_string(result).c_str());
		}
	}

	LOGF("Vulkan loader version: %u.%u.%u\n", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
	     VK_VERSION_PATCH(version));
	if (version < VULKAN_TARGET_API_VERSION) {
		EXIT("Vulkan %u.%u is required, but loader supports only %u.%u.%u\n",
		     VK_VERSION_MAJOR(VULKAN_TARGET_API_VERSION),
		     VK_VERSION_MINOR(VULKAN_TARGET_API_VERSION), VK_VERSION_MAJOR(version),
		     VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
	}
}

#if defined(LUMEN_PS5_NATIVE_METAL)
static void CreateNativeMetalSurface(WindowContext& context) {
	if (context.metal_view == nullptr) context.metal_view = SDL_Metal_CreateView(context.window);
	EXIT_IF(context.metal_view == nullptr);
	VkMetalSurfaceCreateInfoEXT create {};
	create.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
	create.pLayer = static_cast<const CAMetalLayer*>(SDL_Metal_GetLayer(context.metal_view));
	auto create_surface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
	    vkGetInstanceProcAddr(context.graphic_ctx.instance, "vkCreateMetalSurfaceEXT"));
	EXIT_IF(create_surface == nullptr);
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	RequireVulkanSuccess(static_cast<vk::Result>(create_surface(
	    context.graphic_ctx.instance, &create, nullptr, &surface)), "vkCreateMetalSurfaceEXT");
	context.surface = surface;
}
#endif

void WindowContext::CreateVulkan() {
	EXIT_IF(window == nullptr);
	EXIT_IF(graphic_ctx.instance != nullptr);
	EXIT_IF(graphic_ctx.physical_device != nullptr);
	EXIT_IF(graphic_ctx.device != nullptr);
	EXIT_IF(surface != nullptr);

#if defined(LUMEN_PS5_NATIVE_METAL)
	auto get_instance_proc_addr = &vkGetInstanceProcAddr;
#else
	auto get_instance_proc_addr =
	    reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
#endif
	if (get_instance_proc_addr == nullptr) {
		EXIT("Could not load Vulkan: %s\n", SDL_GetError());
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

	VulkanExtensions r;
	VulkanGetExtensions(r);
	VulkanCheckInstanceVersion();

	vk::ApplicationInfo app_info {};
	app_info.pApplicationName   = "Kyty";
	app_info.applicationVersion = 1;
	app_info.pEngineName        = "Kyty";
	app_info.engineVersion      = 1;
	app_info.apiVersion         = VULKAN_TARGET_API_VERSION; // NOLINT

	if (Config::SpirvDebugPrintfEnabled() && Config::GpuAssistedValidationEnabled()) {
		EXIT("--spirv-debug-printf and --gpu-assisted-validation are mutually exclusive\n");
	}

	vk::ValidationFeatureEnableEXT enabled_features[3]    = {};
	uint32_t                       enabled_features_count = 0;
#ifdef KYTY_ENABLE_BEST_PRACTICES
	enabled_features[enabled_features_count++] = vk::ValidationFeatureEnableEXT::eBestPractices;
#endif
#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (Config::SpirvDebugPrintfEnabled()) {
		enabled_features[enabled_features_count++] = vk::ValidationFeatureEnableEXT::eDebugPrintf;
	}
#endif
	if (Config::GpuAssistedValidationEnabled()) {
		enabled_features[enabled_features_count++] = vk::ValidationFeatureEnableEXT::eGpuAssisted;
		LOGF("Vulkan GPU-assisted validation is enabled; expect a large slowdown\n");
		std::printf("Vulkan GPU-assisted validation is enabled; expect a large slowdown\n");
		std::fflush(stdout);
	}

	vk::ValidationFeaturesEXT validation_features {};
	validation_features.enabledValidationFeatureCount  = enabled_features_count;
	validation_features.pEnabledValidationFeatures     = enabled_features;

	vk::DebugUtilsMessengerCreateInfoEXT dbg_create_info {};
	dbg_create_info.pNext           = &validation_features;
	dbg_create_info.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
	                                  vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
	dbg_create_info.messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
	                                  vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
	                                  vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
	dbg_create_info.pfnUserCallback = VulkanDebugMessengerCallback;

	vk::InstanceCreateInfo inst_info {};
	inst_info.pNext                   = (r.enable_validation_layers ? &dbg_create_info : nullptr);
#if defined(__APPLE__)
	// MoltenVK requires VK_KHR_portability_enumeration + flag to surface
	// portability devices. Without this, enumeratePhysicalDevices hides the
	// MoltenVK adapter on some driver versions.
	if (HasExtension(r.available_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
		if (!HasExtension(r.required_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
			r.required_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		}
		inst_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
		LOGF("Vulkan instance: enabled %s\n", VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	}
#endif
	inst_info.pApplicationInfo        = &app_info;
	inst_info.enabledExtensionCount   = static_cast<uint32_t>(r.required_extensions.size());
	inst_info.ppEnabledExtensionNames = r.required_extensions.data();
	inst_info.enabledLayerCount =
	    (r.enable_validation_layers ? static_cast<uint32_t>(r.required_layers.size()) : 0);
	inst_info.ppEnabledLayerNames =
	    (r.enable_validation_layers ? r.required_layers.data() : nullptr);

	const vk::Result result = vk::createInstance(&inst_info, nullptr, &graphic_ctx.instance);
	switch (result) {
		case vk::Result::eSuccess: break;
		case vk::Result::eErrorIncompatibleDriver:
			EXIT("Unable to find a compatible Vulkan Driver");
		default: EXIT("Could not create a Vulkan instance (for unknown reasons)");
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphic_ctx.instance);

	if (r.enable_validation_layers) {
		dbg_create_info.pNext = nullptr;
		if (VulkanCreateDebugUtilsMessengerEXT(graphic_ctx.instance, &dbg_create_info, nullptr,
		                                       &graphic_ctx.debug_messenger) !=
		    vk::Result::eSuccess) {
			EXIT("Could not create debug messenger");
		}
	}

#if defined(LUMEN_PS5_NATIVE_METAL)
	CreateNativeMetalSurface(*this);
#else
	vk::SurfaceKHR::CType native_surface = VK_NULL_HANDLE;
	if (!SDL_Vulkan_CreateSurface(window, static_cast<vk::Instance::CType>(graphic_ctx.instance),
	                              nullptr, &native_surface)) {
		EXIT("Could not create a Vulkan surface");
	}
	surface = native_surface;
#endif

	std::vector<const char*> device_extensions = {
	    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
	    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	    "VK_KHR_maintenance1"};
#if defined(LUMEN_PS5_NATIVE_METAL)
	device_extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
	device_extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
#else
	device_extensions.push_back(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
	device_extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
#endif

#if defined(__APPLE__) && !defined(LUMEN_PS5_NATIVE_METAL)
	// MoltenVK lacks VK_EXT_depth_clip_enable and VK_EXT_color_write_enable; the renderer
	// falls back to default depth clipping and static color-write masks on macOS. It also
	// requires VK_KHR_portability_subset per the Vulkan portability spec.
	device_extensions.push_back("VK_KHR_portability_subset");
#elif !defined(__APPLE__)
	device_extensions.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
	device_extensions.push_back(VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME);
#endif

#ifdef KYTY_ENABLE_DEBUG_PRINTF
	if (Config::SpirvDebugPrintfEnabled()) {
		device_extensions.push_back(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
	}
#endif

	std::string rejected_devices;
	VulkanFindPhysicalDevice(graphic_ctx.instance, surface, device_extensions, surface_capabilities,
	                         graphic_ctx.physical_device, graphic_ctx.queue_family,
	                         rejected_devices);

	if (graphic_ctx.physical_device == nullptr) {
		EXIT("Could not find suitable device:%s", rejected_devices.c_str());
	}

	vk::PhysicalDevicePushDescriptorProperties push_descriptor_properties {};
	vk::PhysicalDeviceProperties2              physical_device_properties {};
	physical_device_properties.pNext = &push_descriptor_properties;
	graphic_ctx.physical_device.getProperties2(&physical_device_properties);
	graphic_ctx.physical_device_properties = physical_device_properties.properties;
	graphic_ctx.max_push_descriptors       = push_descriptor_properties.maxPushDescriptors;
	graphic_ctx.physical_device.getMemoryProperties(&graphic_ctx.physical_device_memory_properties);
	const auto& device_properties = graphic_ctx.GetPhysicalDeviceProperties();
	const auto optional_extensions = EnumerateVulkan<vk::ExtensionProperties>(
	    "vkEnumerateDeviceExtensionProperties", [&](uint32_t* count, vk::ExtensionProperties* values) {
		    return graphic_ctx.physical_device.enumerateDeviceExtensionProperties(nullptr, count, values);
	    });
	if (HasExtension(optional_extensions, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME)) {
		device_extensions.push_back(VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME);
	}
	if (HasExtension(optional_extensions, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME) &&
	    !HasExtension(device_extensions, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME)) {
		device_extensions.push_back(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
	}
	if (HasExtension(optional_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) &&
	    !HasExtension(device_extensions, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
		device_extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
	}


	LOGF("Select device: %s\n", device_properties.deviceName.data());

	const vk::PhysicalDeviceImageFormatInfo2 block_texel_view_info {
	    .format = vk::Format::eBc1RgbaUnormBlock,
	    .type = vk::ImageType::e2D,
	    .tiling = vk::ImageTiling::eOptimal,
	    .usage = vk::ImageUsageFlagBits::eSampled,
	    .flags = vk::ImageCreateFlagBits::eBlockTexelViewCompatible,
	};
	const auto block_texel_view_props =
	    graphic_ctx.physical_device.getImageFormatProperties2(block_texel_view_info);
	graphic_ctx.supports_block_texel_view = block_texel_view_props.result == vk::Result::eSuccess;
	LOGF("Block Texel View support: %s\n", graphic_ctx.supports_block_texel_view ? "Yes" : "No");

	{
		auto available_extensions = EnumerateVulkan<vk::ExtensionProperties>(
		    "vkEnumerateDeviceExtensionProperties",
		    [&](uint32_t* count, vk::ExtensionProperties* values) {
			    return graphic_ctx.physical_device.enumerateDeviceExtensionProperties(
			        nullptr, count, values);
		    });

		if (HasExtension(available_extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
			device_extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
			graphic_ctx.memory_budget_ext_enabled = true;
		}
		for (const auto* extension: {VK_KHR_MAINTENANCE_8_EXTENSION_NAME,
		                             VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME,
		                             VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME,
		                             VK_EXT_MESH_SHADER_EXTENSION_NAME,
		                             VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME,
		                             VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME,
		                             VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME}) {
			if (HasExtension(available_extensions, extension)) {
				device_extensions.push_back(extension);
			}
		}
		if (HasExtension(available_extensions, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME) &&
		    HasExtension(available_extensions, VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME)) {
			device_extensions.push_back(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
			device_extensions.push_back(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
		}
	}

	graphic_ctx.device = VulkanCreateDevice(graphic_ctx, device_extensions);
	if (graphic_ctx.device == nullptr) {
		EXIT("Could not create device");
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(graphic_ctx.device);
	graphic_ctx.device.getQueue(graphic_ctx.queue_family, 0, &graphic_ctx.queue);
	EXIT_IF(graphic_ctx.queue == nullptr);

	if (!graphic_ctx.CreateAllocator()) {
		EXIT("Could not create Vulkan memory allocator");
	}

	render_context = std::make_unique<RenderContext>(graphic_ctx);
	LibKernel::Memory::InstallGpuResources(render_context.get());
	presenter = std::make_unique<Presenter>(*this);
}

bool WindowContext::IsPresentationSurfaceAvailable() const {
#if defined(__ANDROID__)
	return SDL_GetPointerProperty(SDL_GetWindowProperties(window),
	                              SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr) != nullptr;
#else
	return true;
#endif
}

bool WindowContext::RefreshSurfaceCapabilities() {
	EXIT_IF(graphic_ctx.physical_device == nullptr || surface == nullptr);
	return GetSurfaceCapabilities(graphic_ctx.physical_device, surface, surface_capabilities, true);
}

bool WindowContext::RecreateSurface() {
	EXIT_IF(window == nullptr || graphic_ctx.instance == nullptr);
	if (surface != nullptr) {
		graphic_ctx.instance.destroySurfaceKHR(surface, nullptr);
		surface = nullptr;
	}
#if defined(LUMEN_PS5_NATIVE_METAL)
	CreateNativeMetalSurface(*this);
#else
#if defined(__ANDROID__)
	// SDL clears this property before releasing its ANativeWindow. Keep the
	// properties locked through creation so surfaceDestroyed cannot release the
	// native window between our availability check and SDL's Vulkan call.
	const auto properties = SDL_GetWindowProperties(window);
	EXIT_IF(!SDL_LockProperties(properties));
	if (SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr) == nullptr) {
		SDL_UnlockProperties(properties);
		return false;
	}
#endif
	vk::SurfaceKHR::CType native_surface = VK_NULL_HANDLE;
	const bool created = SDL_Vulkan_CreateSurface(
	    window, static_cast<vk::Instance::CType>(graphic_ctx.instance), nullptr, &native_surface);
#if defined(__ANDROID__)
	SDL_UnlockProperties(properties);
#endif
	if (!created) {
		EXIT("Could not recreate the Vulkan surface: %s\n", SDL_GetError());
	}
	surface = native_surface;
#endif
	return true;
}

WindowContext::~WindowContext() {
	ShutdownSystemOverlayInput();
	presenter.reset();
	LibKernel::Memory::InstallGpuResources(nullptr);
	render_context.reset();

	if (graphic_ctx.device != nullptr) {
		RequireVulkanSuccess(graphic_ctx.device.waitIdle(), "wait for Vulkan device shutdown");
		graphic_ctx.DestroyAllocator();
		graphic_ctx.device.destroy(nullptr);
		graphic_ctx.device = nullptr;
		graphic_ctx.queue  = nullptr;
	}
	if (surface != nullptr) {
		graphic_ctx.instance.destroySurfaceKHR(surface, nullptr);
		surface = nullptr;
	}
	if (graphic_ctx.debug_messenger != nullptr) {
		graphic_ctx.instance.destroyDebugUtilsMessengerEXT(graphic_ctx.debug_messenger, nullptr);
		graphic_ctx.debug_messenger = nullptr;
	}
	if (graphic_ctx.instance != nullptr) {
		graphic_ctx.instance.destroy(nullptr);
		graphic_ctx.instance        = nullptr;
		graphic_ctx.physical_device = nullptr;
	}
#if defined(LUMEN_PS5_NATIVE_METAL)
	if (metal_view != nullptr) {
		SDL_Metal_DestroyView(metal_view);
		metal_view = nullptr;
	}
#endif
	if (window != nullptr) {
		SDL_DestroyWindow(window);
		window = nullptr;
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

} // namespace Libs::Graphics
