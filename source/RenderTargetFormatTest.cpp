#include "Emulator/Graphics/RenderTargetFormat.h"
#include "Emulator/Graphics/RenderTargetVulkanContract.h"
#include "Emulator/Graphics/ShaderColorExportContract.h"

#include <cinttypes>
#include <cstdio>
#include <limits>

#if defined(LUMEN_RENDER_TARGET_VULKAN_PROBE)
#include "vulkan/vulkan.h"

#include <cstring>
#include <vector>

#ifndef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
#define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME "VK_KHR_portability_enumeration"
#endif
#ifndef VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001
#endif
#endif

using Lumen::Libs::Graphics::ArePixelColorExportEncodingsCompatible;
using Lumen::Libs::Graphics::AreRenderTargetCopyFormatsCompatible;
using Lumen::Libs::Graphics::AreRenderTextureExtentsCompatible;
using Lumen::Libs::Graphics::BuildPs4ColorRenderTargetDescriptor;
using Lumen::Libs::Graphics::BuildRenderTextureImageCreateInfo;
using Lumen::Libs::Graphics::CalculateColorRenderTargetByteSize;
using Lumen::Libs::Graphics::CanUseRenderTextureAsSampledAlias;
using Lumen::Libs::Graphics::ColorRenderTargetFormatInfo;
using Lumen::Libs::Graphics::DecodeColorRenderTargetFormat;
using Lumen::Libs::Graphics::DecodePixelColorExportEncoding;
using Lumen::Libs::Graphics::DecodeSampledRenderTextureFormat;
using Lumen::Libs::Graphics::GetRenderTextureFormatFromVulkan;
using Lumen::Libs::Graphics::GetRenderTextureImageUsage;
using Lumen::Libs::Graphics::GetRenderTextureRequiredFormatFeatures;
using Lumen::Libs::Graphics::GetRenderTextureVulkanFormat;
using Lumen::Libs::Graphics::IsPixelColorExportStateSupported;
using Lumen::Libs::Graphics::IsPixelColorExportSupported;
using Lumen::Libs::Graphics::PackRenderTextureSwizzle;
using Lumen::Libs::Graphics::PixelColorExportEncoding;
using Lumen::Libs::Graphics::RenderTextureFormat;
using Lumen::Libs::Graphics::RenderTextureSampleView;
using Lumen::Libs::Graphics::SelectRenderTextureSampleView;

static_assert(static_cast<uint64_t>(RenderTextureFormat::Unknown) == 0);
static_assert(static_cast<uint64_t>(RenderTextureFormat::R8Unorm) == 1);
static_assert(static_cast<uint64_t>(RenderTextureFormat::R8G8B8A8Unorm) == 2);
static_assert(static_cast<uint64_t>(RenderTextureFormat::R8G8B8A8Srgb) == 3);
static_assert(static_cast<uint64_t>(RenderTextureFormat::B8G8R8A8Unorm) == 4);
static_assert(static_cast<uint64_t>(RenderTextureFormat::B8G8R8A8Srgb) == 5);
static_assert(static_cast<uint64_t>(RenderTextureFormat::R8G8Unorm) == 6);

struct FormatCase
{
	uint32_t            data_format;
	uint32_t            number_type;
	uint32_t            component_swap;
	RenderTextureFormat expected_format;
	uint32_t            expected_bytes;
};

static bool CheckFormatContract()
{
	constexpr FormatCase cases[] = {
	    {0xa, 0x0, 0x0, RenderTextureFormat::R8G8B8A8Unorm, 4}, {0xa, 0x6, 0x0, RenderTextureFormat::R8G8B8A8Srgb, 4},
	    {0xa, 0x0, 0x1, RenderTextureFormat::B8G8R8A8Unorm, 4}, {0xa, 0x6, 0x1, RenderTextureFormat::B8G8R8A8Srgb, 4},
	    {0x1, 0x0, 0x0, RenderTextureFormat::R8Unorm, 1},       {0x3, 0x0, 0x0, RenderTextureFormat::R8G8Unorm, 2},
	};

	for (const auto& test: cases)
	{
		const auto actual = DecodeColorRenderTargetFormat(test.data_format, test.number_type, test.component_swap);
		if (!actual.IsSupported() || actual.format != test.expected_format || actual.bytes_per_element != test.expected_bytes)
		{
			std::fprintf(stderr, "format %u/%u/%u decoded as ordinal=%" PRIu64 " bytes=%u\n", test.data_format, test.number_type,
			             test.component_swap, static_cast<uint64_t>(actual.format), actual.bytes_per_element);
			return false;
		}
	}

	for (uint32_t data_format = 0; data_format < 32; ++data_format)
	{
		for (uint32_t number_type = 0; number_type < 8; ++number_type)
		{
			for (uint32_t component_swap = 0; component_swap < 4; ++component_swap)
			{
				const FormatCase* expected = nullptr;
				for (const auto& test: cases)
				{
					if (test.data_format == data_format && test.number_type == number_type && test.component_swap == component_swap)
					{
						expected = &test;
						break;
					}
				}

				const auto actual         = DecodeColorRenderTargetFormat(data_format, number_type, component_swap);
				const bool should_support = expected != nullptr;
				if (actual.IsSupported() != should_support ||
				    (should_support && (actual.format != expected->expected_format || actual.bytes_per_element != expected->expected_bytes)))
				{
					std::fprintf(stderr, "PS4 fail-closed mismatch for %u/%u/%u\n", data_format, number_type, component_swap);
					return false;
				}
			}
		}
	}

	const auto r8   = DecodeColorRenderTargetFormat(0x1, 0x0, 0x0);
	const auto r8g8 = DecodeColorRenderTargetFormat(0x3, 0x0, 0x0);
	const auto rgba = DecodeColorRenderTargetFormat(0xa, 0x0, 0x0);
	if (CalculateColorRenderTargetByteSize(0xffff, r8) != 0x400000u || CalculateColorRenderTargetByteSize(0xffff, r8g8) != 0x800000u ||
	    CalculateColorRenderTargetByteSize(0xffff, rgba) != 0x1000000u)
	{
		std::fprintf(stderr, "color slice byte sizing is not format-aware\n");
		return false;
	}
	const auto restart = BuildPs4ColorRenderTargetDescriptor(0x3, 0x0, 0x0, 0xffff);
	if (!restart.IsSupported() || restart.format.format != RenderTextureFormat::R8G8Unorm || restart.format.bytes_per_element != 2 ||
	    restart.byte_size != 0x800000u)
	{
		std::fprintf(stderr, "Restart descriptor did not converge on R8G8/0x800000\n");
		return false;
	}

	constexpr ColorRenderTargetFormatInfo unsupported {};
	constexpr ColorRenderTargetFormatInfo four_byte {RenderTextureFormat::R8G8B8A8Unorm, 4};
	constexpr uint64_t                    max_size = (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1u) * 64u * 4u;
	if (CalculateColorRenderTargetByteSize(0, unsupported) != 0 ||
	    CalculateColorRenderTargetByteSize(std::numeric_limits<uint32_t>::max(), four_byte) != max_size)
	{
		std::fprintf(stderr, "color slice boundary sizing failed\n");
		return false;
	}

	return true;
}

static bool CheckShaderColorExportContract()
{
	uint32_t supported_pairs = 0;
	for (uint32_t mode = 0; mode < 16; ++mode)
	{
		for (uint32_t mask = 0; mask <= 0xffff; ++mask)
		{
			PixelColorExportEncoding expected = PixelColorExportEncoding::Unsupported;
			if (mode == 4u && mask == 0x3u)
			{
				expected = PixelColorExportEncoding::Mrt0Rg16FloatCompressed;
			} else if (mode == 4u && mask == 0xfu)
			{
				expected = PixelColorExportEncoding::Mrt0Rgba16FloatCompressed;
			} else if (mode == 9u && mask == 0xfu)
			{
				expected = PixelColorExportEncoding::Mrt0Rgba32Float;
			}

			const auto actual = DecodePixelColorExportEncoding(mode, mask);
			if (actual != expected || IsPixelColorExportSupported(actual) != (expected != PixelColorExportEncoding::Unsupported))
			{
				std::fprintf(stderr, "pixel color-export contract mismatch for mode=%u mask=0x%x\n", mode, mask);
				return false;
			}
			if (IsPixelColorExportSupported(actual))
			{
				++supported_pairs;
			}
		}
	}

	if (supported_pairs != 3 || DecodePixelColorExportEncoding(4u, 0x3u) != PixelColorExportEncoding::Mrt0Rg16FloatCompressed ||
	    DecodePixelColorExportEncoding(4u, 0xfu) != PixelColorExportEncoding::Mrt0Rgba16FloatCompressed ||
	    IsPixelColorExportSupported(DecodePixelColorExportEncoding(4u, 0xcu)) ||
	    IsPixelColorExportSupported(DecodePixelColorExportEncoding(9u, 0x3u)) ||
	    IsPixelColorExportSupported(DecodePixelColorExportEncoding(4u, 0xfffu)) ||
	    IsPixelColorExportSupported(DecodePixelColorExportEncoding(4u, 0x10000u)) ||
	    IsPixelColorExportSupported(DecodePixelColorExportEncoding(4u, 0x10000000u)) ||
	    IsPixelColorExportSupported(DecodePixelColorExportEncoding(4u, 0xffffffffu)) || !IsPixelColorExportStateSupported(true, 0u, 0u) ||
	    IsPixelColorExportStateSupported(false, 0u, 0u))
	{
		std::fprintf(stderr, "pixel color-export allowlist is not fail-closed\n");
		return false;
	}

	constexpr PixelColorExportEncoding encodings[] = {
	    PixelColorExportEncoding::Unsupported,
	    PixelColorExportEncoding::Mrt0Rg16FloatCompressed,
	    PixelColorExportEncoding::Mrt0Rgba16FloatCompressed,
	    PixelColorExportEncoding::Mrt0Rgba32Float,
	};
	for (const auto register_encoding: encodings)
	{
		for (const auto instruction_encoding: encodings)
		{
			const bool expected = register_encoding != PixelColorExportEncoding::Unsupported && register_encoding == instruction_encoding;
			if (ArePixelColorExportEncodingsCompatible(register_encoding, instruction_encoding) != expected)
			{
				std::fprintf(stderr, "pixel color-export register/instruction compatibility mismatch\n");
				return false;
			}
		}
	}

	return true;
}

static bool CheckVulkanContract()
{
	constexpr VkImageUsageFlags expected_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	constexpr VkFormatFeatureFlags expected_features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	                                                   VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
	                                                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	if (GetRenderTextureImageUsage() != expected_usage || GetRenderTextureRequiredFormatFeatures() != expected_features)
	{
		std::fprintf(stderr, "production render-target usage/features drifted from the preregistered contract\n");
		return false;
	}

	constexpr RenderTextureFormat formats[] = {
	    RenderTextureFormat::R8Unorm,      RenderTextureFormat::R8G8Unorm,     RenderTextureFormat::R8G8B8A8Unorm,
	    RenderTextureFormat::R8G8B8A8Srgb, RenderTextureFormat::B8G8R8A8Unorm, RenderTextureFormat::B8G8R8A8Srgb,
	};
	for (const auto format: formats)
	{
		const auto vulkan = GetRenderTextureVulkanFormat(format);
		if (vulkan == VK_FORMAT_UNDEFINED || GetRenderTextureFormatFromVulkan(vulkan) != format)
		{
			std::fprintf(stderr, "render-target Vulkan mapping is not reversible for ordinal=%" PRIu64 "\n", static_cast<uint64_t>(format));
			return false;
		}
	}

	const auto image = BuildRenderTextureImageCreateInfo(RenderTextureFormat::R8G8Unorm, 1024, 4096);
	if (image.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || image.imageType != VK_IMAGE_TYPE_2D || image.format != VK_FORMAT_R8G8_UNORM ||
	    image.extent.width != 1024 || image.extent.height != 4096 || image.extent.depth != 1 || image.mipLevels != 1 ||
	    image.arrayLayers != 1 || image.tiling != VK_IMAGE_TILING_OPTIMAL || image.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED ||
	    image.usage != expected_usage || image.sharingMode != VK_SHARING_MODE_EXCLUSIVE || image.samples != VK_SAMPLE_COUNT_1_BIT)
	{
		std::fprintf(stderr, "production R8G8 image descriptor mismatch\n");
		return false;
	}

	if (!AreRenderTargetCopyFormatsCompatible(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM) ||
	    !AreRenderTargetCopyFormatsCompatible(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM) ||
	    AreRenderTargetCopyFormatsCompatible(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8_UNORM) ||
	    AreRenderTargetCopyFormatsCompatible(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_R8G8B8A8_UNORM))
	{
		std::fprintf(stderr, "render-target copy compatibility is not fail-closed\n");
		return false;
	}

	const auto sampled_r8   = DecodeSampledRenderTextureFormat(1, 0);
	const auto sampled_r8g8 = DecodeSampledRenderTextureFormat(3, 0);
	const auto sampled_rgba = DecodeSampledRenderTextureFormat(10, 0);
	if (SelectRenderTextureSampleView(RenderTextureFormat::R8Unorm, sampled_r8.format, PackRenderTextureSwizzle(4, 0, 0, 1)) !=
	        RenderTextureSampleView::Default ||
	    SelectRenderTextureSampleView(RenderTextureFormat::R8G8Unorm, sampled_r8g8.format, PackRenderTextureSwizzle(4, 5, 0, 1)) !=
	        RenderTextureSampleView::Default ||
	    SelectRenderTextureSampleView(RenderTextureFormat::R8G8Unorm, sampled_r8g8.format, PackRenderTextureSwizzle(5, 4, 0, 1)) !=
	        RenderTextureSampleView::Unsupported ||
	    SelectRenderTextureSampleView(RenderTextureFormat::R8G8B8A8Unorm, sampled_rgba.format, PackRenderTextureSwizzle(6, 5, 4, 7)) !=
	        RenderTextureSampleView::Bgra ||
	    SelectRenderTextureSampleView(RenderTextureFormat::B8G8R8A8Unorm, sampled_rgba.format, PackRenderTextureSwizzle(4, 5, 6, 7)) !=
	        RenderTextureSampleView::Bgra ||
	    SelectRenderTextureSampleView(RenderTextureFormat::B8G8R8A8Unorm, sampled_rgba.format, PackRenderTextureSwizzle(6, 5, 4, 7)) !=
	        RenderTextureSampleView::Default ||
	    SelectRenderTextureSampleView(RenderTextureFormat::R8G8B8A8Unorm, sampled_r8g8.format, PackRenderTextureSwizzle(4, 5, 0, 1)) !=
	        RenderTextureSampleView::Unsupported ||
	    SelectRenderTextureSampleView(RenderTextureFormat::Unknown, RenderTextureFormat::Unknown, PackRenderTextureSwizzle(4, 5, 6, 7)) !=
	        RenderTextureSampleView::Unsupported ||
	    !AreRenderTextureExtentsCompatible(1024, 4096, 1024, 4096) || AreRenderTextureExtentsCompatible(1024, 4096, 2048, 2048) ||
	    !CanUseRenderTextureAsSampledAlias(false, true) || CanUseRenderTextureAsSampledAlias(true, true) ||
	    CanUseRenderTextureAsSampledAlias(false, false))
	{
		std::fprintf(stderr, "render-target sample alias contract mismatch\n");
		return false;
	}

	return true;
}

#if defined(LUMEN_RENDER_TARGET_VULKAN_PROBE)
static bool CheckVulkanCapability()
{
	uint32_t extension_count = 0;
	if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr) != VK_SUCCESS)
	{
		std::fprintf(stderr, "could not enumerate Vulkan instance extensions\n");
		return false;
	}

	std::vector<VkExtensionProperties> available(extension_count);
	if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available.data()) != VK_SUCCESS)
	{
		std::fprintf(stderr, "could not read Vulkan instance extensions\n");
		return false;
	}

	bool portability = false;
	for (const auto& extension: available)
	{
		if (std::strcmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0)
		{
			portability = true;
			break;
		}
	}

	const char*       enabled_extensions[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
	VkApplicationInfo application {};
	application.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName   = "Lumen render-target format test";
	application.applicationVersion = 1;
	application.pEngineName        = "Lumen";
	application.engineVersion      = 1;
	application.apiVersion         = VK_API_VERSION_1_1;

	VkInstanceCreateInfo create_info {};
	create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	create_info.pApplicationInfo        = &application;
	create_info.enabledExtensionCount   = portability ? 1u : 0u;
	create_info.ppEnabledExtensionNames = portability ? enabled_extensions : nullptr;
	create_info.flags                   = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;

	VkInstance instance      = VK_NULL_HANDLE;
	const auto create_result = vkCreateInstance(&create_info, nullptr, &instance);
	if (create_result != VK_SUCCESS)
	{
		std::fprintf(stderr, "vkCreateInstance failed: %d\n", static_cast<int>(create_result));
		return false;
	}

	uint32_t device_count = 0;
	if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0)
	{
		std::fprintf(stderr, "no Vulkan physical device is available\n");
		vkDestroyInstance(instance, nullptr);
		return false;
	}

	std::vector<VkPhysicalDevice> devices(device_count);
	vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
	constexpr VkFormatFeatureFlags required_features = GetRenderTextureRequiredFormatFeatures();

	bool supported = false;
	for (const auto device: devices)
	{
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(device, &properties);

		VkFormatProperties format_properties {};
		vkGetPhysicalDeviceFormatProperties(device, GetRenderTextureVulkanFormat(RenderTextureFormat::R8G8Unorm), &format_properties);

		VkImageFormatProperties image_properties {};
		const auto              fixture = BuildRenderTextureImageCreateInfo(RenderTextureFormat::R8G8Unorm, 1024, 4096);
		const auto image_result = vkGetPhysicalDeviceImageFormatProperties(device, fixture.format, fixture.imageType, fixture.tiling,
		                                                                   fixture.usage, fixture.flags, &image_properties);
		const bool features_ok  = (format_properties.optimalTilingFeatures & required_features) == required_features;
		const bool limits_ok    = image_result == VK_SUCCESS && image_properties.maxExtent.width >= fixture.extent.width &&
		                       image_properties.maxExtent.height >= fixture.extent.height &&
		                       image_properties.maxExtent.depth >= fixture.extent.depth && image_properties.maxMipLevels >= 1 &&
		                       image_properties.maxArrayLayers >= 1 && (image_properties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 &&
		                       image_properties.maxResourceSize >= 0x800000u;
		std::printf("Vulkan R8G8 color target device=%s features=%s image=%s limits=%s\n", properties.deviceName,
		            features_ok ? "ok" : "missing", image_result == VK_SUCCESS ? "ok" : "unsupported", limits_ok ? "ok" : "insufficient");
		supported = supported || (features_ok && limits_ok);
	}

	vkDestroyInstance(instance, nullptr);
	return supported;
}
#endif

int main()
{
	if (!CheckFormatContract())
	{
		return 1;
	}
	if (!CheckVulkanContract())
	{
		return 1;
	}
	if (!CheckShaderColorExportContract())
	{
		return 1;
	}

#if defined(LUMEN_RENDER_TARGET_VULKAN_PROBE)
	if (!CheckVulkanCapability())
	{
		std::fprintf(stderr, "VK_FORMAT_R8G8_UNORM lacks the production render-target usage\n");
		return 1;
	}
#endif

	std::printf("Render-target-format descriptor=ok legacy=ok r8g8=ok gen4-only=ok size=0x800000 alias=ok shader-export=ok");
#if defined(LUMEN_RENDER_TARGET_VULKAN_PROBE)
	std::printf(" vulkan=ok");
#endif
	std::printf("\n");
	return 0;
}
