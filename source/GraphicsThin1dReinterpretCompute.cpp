#include "Emulator/Graphics/TextureThin1dReinterpretCopy.h"
#include "Emulator/Graphics/TextureThin1dReinterpretShader.h"

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
using Lumen::Libs::Graphics::TextureThin1dReinterpretShader::Direction;
using Lumen::Libs::Graphics::TextureThin1dReinterpretShader::PushConstants;
namespace ReinterpretShader = Lumen::Libs::Graphics::TextureThin1dReinterpretShader;

#ifdef __ANDROID__
constexpr const char* kBackend = "android-vulkan";
#else
constexpr const char* kBackend = "native-metal";
#endif

constexpr uint64_t kCompletionTimeoutNanoseconds = 5'000'000'000;
constexpr uint32_t kTileWidth                    = 8;
constexpr uint32_t kTilePixels                   = kTileWidth * kTileWidth;

struct ProbeCase
{
	Direction direction;
	VkFormat source_format;
	VkFormat destination_format;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t source_pitch;
	uint32_t destination_width;
	uint32_t destination_height;
	uint32_t destination_pitch;
	uint32_t source_bytes_per_pixel;
	uint32_t destination_bytes_per_pixel;
	const char* name;
	const char* source_name;
	const char* destination_name;
	size_t expected_function_calls;
};

constexpr ProbeCase kForward {
    Direction::R8ToRgba8,
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    512,
    512,
	512,
    48,
    48,
	48,
    1,
    4,
    "r8-to-rgba8",
    "R8 source",
    "RGBA8 destination",
    4,
};
constexpr ProbeCase kReverse {
    Direction::Rgba8ToR8,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8_UNORM,
    1920,
    1080,
	1920,
    512,
    512,
	512,
    4,
    1,
    "rgba8-to-r8",
    "RGBA8 source",
    "R8 destination",
    0,
};
constexpr ProbeCase kForwardPadded {
    Direction::R8ToRgba8,
    VK_FORMAT_R8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    512,
    512,
	512,
    182,
    5,
	256,
    1,
    4,
    "r8-to-rgba8-pitched-padded",
    "padded R8 source",
    "pitched RGBA8 destination",
    4,
};
constexpr ProbeCase kRgba8Prefix {
    Direction::Rgba8ToRgba8,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    64,
    32,
    64,
    32,
    24,
    32,
    4,
    4,
    "rgba8-to-rgba8-strict-prefix",
    "strict RGBA8 source",
    "strict RGBA8 destination",
    0,
};
constexpr ProbeCase kRgba8PrefixPadded {
    Direction::Rgba8ToRgba8,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R8G8B8A8_UNORM,
    1920,
    1080,
    1920,
    1503,
    46,
    2048,
    4,
    4,
    "rgba8-to-rgba8-pitched-padded-prefix",
    "padded-prefix RGBA8 source",
    "padded-prefix RGBA8 destination",
    0,
};
constexpr ProbeCase kForwardPartial {
    Direction::R8ToRgba8, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
    512, 512, 512, 74, 68, 80, 1, 4,
    "r8-to-rgba8-partial-microtiles", "partial R8 source", "partial RGBA8 destination", 4,
};
constexpr ProbeCase kRgba8PrefixPartial {
    Direction::Rgba8ToRgba8, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
    256, 128, 256, 71, 65, 72, 4, 4,
    "rgba8-to-rgba8-partial-microtiles", "partial-prefix RGBA8 source", "partial-prefix RGBA8 destination", 0,
};
constexpr std::array<ProbeCase, 7> kProbeCases {
    kForward, kReverse, kForwardPadded, kRgba8Prefix, kRgba8PrefixPadded, kForwardPartial, kRgba8PrefixPartial};

constexpr size_t ImageBytes(uint32_t width, uint32_t height, uint32_t bytesPerPixel)
{
	return static_cast<size_t>(width) * height * bytesPerPixel;
}

static_assert(kForward.source_width % kTileWidth == 0 && kForward.source_height % kTileWidth == 0);
static_assert(kForward.destination_width % kTileWidth == 0 && kForward.destination_height % kTileWidth == 0);
static_assert(kReverse.source_width % kTileWidth == 0 && kReverse.source_height % kTileWidth == 0);
static_assert(kReverse.destination_width % kTileWidth == 0 && kReverse.destination_height % kTileWidth == 0);
static_assert(kForwardPadded.source_width % kTileWidth == 0 &&
              kForwardPadded.source_height % kTileWidth == 0);
static_assert(kForwardPadded.destination_width < kForwardPadded.destination_pitch &&
              kForwardPadded.destination_height < kTileWidth);
static_assert(ImageBytes(kForward.destination_width, kForward.destination_height,
                         kForward.destination_bytes_per_pixel) <
              ImageBytes(kForward.source_width, kForward.source_height, kForward.source_bytes_per_pixel));
static_assert(ImageBytes(kReverse.destination_width, kReverse.destination_height,
                         kReverse.destination_bytes_per_pixel) <
              ImageBytes(kReverse.source_width, kReverse.source_height, kReverse.source_bytes_per_pixel));
static_assert(ImageBytes(kForward.source_width, kForward.source_height, kForward.source_bytes_per_pixel) == 0x40000);
static_assert(ImageBytes(kForward.destination_width, kForward.destination_height,
                         kForward.destination_bytes_per_pixel) == 0x2400);
static_assert(ImageBytes(kReverse.source_width, kReverse.source_height, kReverse.source_bytes_per_pixel) == 0x7e9000);
static_assert(ImageBytes(kReverse.destination_width, kReverse.destination_height,
                         kReverse.destination_bytes_per_pixel) == 0x40000);
static_assert(kRgba8Prefix.source_width % kTileWidth == 0 &&
              kRgba8Prefix.source_height % kTileWidth == 0 &&
              kRgba8Prefix.destination_width % kTileWidth == 0 &&
              kRgba8Prefix.destination_height % kTileWidth == 0);
static_assert(kRgba8Prefix.destination_pitch < kRgba8Prefix.source_pitch &&
              kRgba8Prefix.destination_height > kTileWidth);
static_assert(ImageBytes(kRgba8Prefix.destination_width, kRgba8Prefix.destination_height,
                         kRgba8Prefix.destination_bytes_per_pixel) <
              ImageBytes(kRgba8Prefix.source_width, kRgba8Prefix.source_height,
                         kRgba8Prefix.source_bytes_per_pixel));
static_assert(kRgba8PrefixPadded.source_width % kTileWidth == 0 &&
              kRgba8PrefixPadded.source_height % kTileWidth == 0);
static_assert(kRgba8PrefixPadded.destination_width < kRgba8PrefixPadded.destination_pitch &&
              kRgba8PrefixPadded.destination_pitch > kRgba8PrefixPadded.source_pitch &&
              kRgba8PrefixPadded.destination_height > kTileWidth);
constexpr uint64_t PaddedDestinationAllocationSize(const ProbeCase& probe)
{
	uint64_t size = 0;
	return Lumen::Libs::Graphics::TextureThin1dSingleLevelPaddedRgba8Size(
	           probe.destination_width, probe.destination_height, probe.destination_pitch, true, &size)
	           ? size
	           : 0;
}
static_assert(PaddedDestinationAllocationSize(kForwardPadded) == 0x2000);
static_assert(PaddedDestinationAllocationSize(kRgba8PrefixPadded) == 0x60000);
static_assert(PaddedDestinationAllocationSize(kRgba8PrefixPadded) <
              ImageBytes(kRgba8PrefixPadded.source_width, kRgba8PrefixPadded.source_height,
                         kRgba8PrefixPadded.source_bytes_per_pixel));
constexpr uint64_t PartialDestinationAllocationSize(const ProbeCase& probe)
{
	uint64_t size = 0;
	return Lumen::Libs::Graphics::TextureThin1dSingleLevelRgba8DestinationSize(
	           probe.destination_width, probe.destination_height, probe.destination_pitch, false, &size) ? size : 0;
}
static_assert(PartialDestinationAllocationSize(kForwardPartial) == 0x5a00);
static_assert(PartialDestinationAllocationSize(kRgba8PrefixPartial) == 0x5100);

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
	std::array<VkImage, 2> images {VK_NULL_HANDLE, VK_NULL_HANDLE};
	std::array<VkDeviceMemory, 2> imageMemories {VK_NULL_HANDLE, VK_NULL_HANDLE};
	std::array<VkImageView, 2> imageViews {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkBuffer              uploadBuffer        = VK_NULL_HANDLE;
	VkDeviceMemory        uploadMemory        = VK_NULL_HANDLE;
	void*                 mappedUpload        = nullptr;
	VkBuffer              readbackBuffer      = VK_NULL_HANDLE;
	VkDeviceMemory        readbackMemory      = VK_NULL_HANDLE;
	void*                 mappedReadback      = nullptr;
	VkCommandPool         commandPool         = VK_NULL_HANDLE;
	VkCommandBuffer       commandBuffer       = VK_NULL_HANDLE;
	VkFence               fence               = VK_NULL_HANDLE;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
		if (mappedUpload != nullptr) vkUnmapMemory(device, uploadMemory);
		if (mappedReadback != nullptr) vkUnmapMemory(device, readbackMemory);
		if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
		if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
		if (shaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, shaderModule, nullptr);
		if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		if (descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		for (VkImageView view: imageViews)
		{
			if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
		}
		for (size_t index = 0; index < images.size(); ++index)
		{
			if (images[index] != VK_NULL_HANDLE) vkDestroyImage(device, images[index], nullptr);
			if (imageMemories[index] != VK_NULL_HANDLE) vkFreeMemory(device, imageMemories[index], nullptr);
		}
		if (readbackBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, readbackBuffer, nullptr);
		if (readbackMemory != VK_NULL_HANDLE) vkFreeMemory(device, readbackMemory, nullptr);
		if (uploadBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, uploadBuffer, nullptr);
		if (uploadMemory != VK_NULL_HANDLE) vkFreeMemory(device, uploadMemory, nullptr);
		if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
		if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
	}
};

int Fail(const char* operation, VkResult result = VK_SUCCESS)
{
	if (result == VK_SUCCESS) std::fprintf(stderr, "%s\n", operation);
	else std::fprintf(stderr, "%s: %d\n", operation, result);
	return 1;
}

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((typeBits & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & required) == required)
			return index;
	}
	return UINT32_MAX;
}

VkResult CreateMappedBuffer(VkPhysicalDevice physicalDevice, VkDevice device, VkDeviceSize size,
	                        VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped)
{
	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size        = size;
	bufferInfo.usage       = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkResult result        = vkCreateBuffer(device, &bufferInfo, nullptr, buffer);
	if (result != VK_SUCCESS) return result;

	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(device, *buffer, &requirements);
	const uint32_t memoryType = FindMemoryType(physicalDevice, requirements.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result                         = vkAllocateMemory(device, &allocationInfo, nullptr, memory);
	if (result != VK_SUCCESS) return result;
	result = vkBindBufferMemory(device, *buffer, *memory, 0);
	if (result != VK_SUCCESS) return result;
	return vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
}

VkResult CreateDeviceLocalImage(VkPhysicalDevice physicalDevice, VkDevice device, VkFormat format,
	                            uint32_t width, uint32_t height, VkImageUsageFlags usage,
	                            VkImage* image, VkDeviceMemory* memory)
{
	VkImageCreateInfo imageInfo {};
	imageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType     = VK_IMAGE_TYPE_2D;
	imageInfo.format        = format;
	imageInfo.extent        = {width, height, 1};
	imageInfo.mipLevels     = 1;
	imageInfo.arrayLayers   = 1;
	imageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage         = usage;
	imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkResult result         = vkCreateImage(device, &imageInfo, nullptr, image);
	if (result != VK_SUCCESS) return result;

	VkMemoryRequirements requirements {};
	vkGetImageMemoryRequirements(device, *image, &requirements);
	const uint32_t memoryType = FindMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (memoryType == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result                         = vkAllocateMemory(device, &allocationInfo, nullptr, memory);
	if (result != VK_SUCCESS) return result;
	return vkBindImageMemory(device, *image, *memory, 0);
}

VkResult CreateImageView(VkDevice device, VkImage image, VkFormat format, VkImageView* view)
{
	VkImageViewCreateInfo viewInfo {};
	viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image                           = image;
	viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format                          = format;
	viewInfo.components                      = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
	                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel   = 0;
	viewInfo.subresourceRange.levelCount     = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount     = 1;
	return vkCreateImageView(device, &viewInfo, nullptr, view);
}

VkImageSubresourceRange ColorRange()
{
	VkImageSubresourceRange range {};
	range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel   = 0;
	range.levelCount     = 1;
	range.baseArrayLayer = 0;
	range.layerCount     = 1;
	return range;
}

uint32_t Thin1dMortonIndex(uint32_t x, uint32_t y)
{
	return (x & 1u) | ((y & 1u) << 1u) | ((x & 2u) << 1u) | ((y & 2u) << 2u) |
	       ((x & 4u) << 2u) | ((y & 4u) << 3u);
}

uint32_t Thin1dOffset(uint32_t x, uint32_t y, uint32_t tilesPerRow, uint32_t bytesPerPixel)
{
	const uint32_t tile = (y / kTileWidth) * tilesPerRow + x / kTileWidth;
	return tile * kTilePixels * bytesPerPixel + Thin1dMortonIndex(x & 7u, y & 7u) * bytesPerPixel;
}

std::vector<uint8_t> MakeSourceImage(const ProbeCase& probe)
{
	std::vector<uint8_t> source(ImageBytes(probe.source_width, probe.source_height,
	                                       probe.source_bytes_per_pixel));
	for (uint32_t y = 0; y < probe.source_height; ++y)
	{
		for (uint32_t x = 0; x < probe.source_width; ++x)
		{
			const size_t linearOffset =
			    (static_cast<size_t>(y) * probe.source_width + x) * probe.source_bytes_per_pixel;
			for (uint32_t channel = 0; channel < probe.source_bytes_per_pixel; ++channel)
			{
				source[linearOffset + channel] =
				    static_cast<uint8_t>(x + y * 73u + (x >> 3u) * 19u + channel * 101u);
			}
		}
	}
	return source;
}

std::vector<uint8_t> MakeExpectedDestination(const ProbeCase& probe, const std::vector<uint8_t>& source)
{
	std::vector<uint8_t> rawSource(source.size(), 0xcd);
	for (uint32_t y = 0; y < probe.source_height; ++y)
	{
		for (uint32_t x = 0; x < probe.source_width; ++x)
		{
			const size_t tiledOffset = Thin1dOffset(x, y, probe.source_pitch / kTileWidth,
			                                              probe.source_bytes_per_pixel);
			const size_t linearOffset =
			    (static_cast<size_t>(y) * probe.source_width + x) * probe.source_bytes_per_pixel;
			std::memcpy(rawSource.data() + tiledOffset, source.data() + linearOffset,
			            probe.source_bytes_per_pixel);
		}
	}

	std::vector<uint8_t> destination(ImageBytes(probe.destination_width, probe.destination_height,
	                                            probe.destination_bytes_per_pixel));
	for (uint32_t y = 0; y < probe.destination_height; ++y)
	{
		for (uint32_t x = 0; x < probe.destination_width; ++x)
		{
			const size_t tiledOffset = Thin1dOffset(x, y, probe.destination_pitch / kTileWidth,
			                                              probe.destination_bytes_per_pixel);
			const size_t linearOffset =
			    (static_cast<size_t>(y) * probe.destination_width + x) * probe.destination_bytes_per_pixel;
			std::memcpy(destination.data() + linearOffset, rawSource.data() + tiledOffset,
			            probe.destination_bytes_per_pixel);
		}
	}
	return destination;
}

bool ContainsAllByteValues(const std::vector<uint8_t>& bytes)
{
	std::array<bool, 256> found {};
	for (uint8_t value: bytes) found[value] = true;
	for (bool present: found)
	{
		if (!present) return false;
	}
	return true;
}

bool MatchesTopLeftRectangle(const ProbeCase& probe, const std::vector<uint8_t>& source,
	                         const std::vector<uint8_t>& destination)
{
	if (probe.source_bytes_per_pixel != probe.destination_bytes_per_pixel ||
	    probe.source_width < probe.destination_width || probe.source_height < probe.destination_height)
	{
		return false;
	}
	const size_t rowBytes = static_cast<size_t>(probe.destination_width) * probe.destination_bytes_per_pixel;
	const size_t sourceRowBytes = static_cast<size_t>(probe.source_width) * probe.source_bytes_per_pixel;
	for (uint32_t row = 0; row < probe.destination_height; ++row)
	{
		if (std::memcmp(destination.data() + row * rowBytes, source.data() + row * sourceRowBytes, rowBytes) != 0)
		{
			return false;
		}
	}
	return true;
}

size_t CountToken(std::string_view text, std::string_view token)
{
	size_t count = 0;
	for (size_t offset = 0; (offset = text.find(token, offset)) != std::string_view::npos; offset += token.size()) ++count;
	return count;
}

bool VerifyBytes(const uint8_t* observed, const std::vector<uint8_t>& expected, const char* label)
{
	size_t mismatches = 0;
	for (size_t index = 0; index < expected.size(); ++index)
	{
		if (observed[index] == expected[index]) continue;
		if (mismatches < 8)
			std::fprintf(stderr, "%s mismatch at byte %zu: actual=%02x expected=%02x\n", label, index,
			             observed[index], expected[index]);
		++mismatches;
	}
	if (mismatches != 0)
	{
		std::fprintf(stderr, "%s verification failed: mismatches=%zu\n", label, mismatches);
		return false;
	}
	return true;
}

bool VerifyCanary(const uint8_t* observed, size_t size, uint8_t expected, const char* label)
{
	for (size_t index = 0; index < size; ++index)
	{
		if (observed[index] == expected) continue;
		std::fprintf(stderr, "%s changed at byte %zu: actual=%02x expected=%02x\n", label, index,
		             observed[index], expected);
		return false;
	}
	return true;
}

int RunProbe(const ProbeCase& probe)
{
	spvtools::SpirvTools spirvTools(SPV_ENV_VULKAN_1_1);
	spirvTools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                              { std::fprintf(stderr, "SPIR-V line %zu column %zu: %s\n", position.line, position.column, message); });
	const auto assembly = ReinterpretShader::GetAssembly(probe.direction);
	if (assembly.data == nullptr || assembly.size == 0)
		return Fail("Thin1D reinterpret shared SPIR-V artifact is missing");
	std::vector<uint32_t> shaderBinary;
	if (!spirvTools.Assemble(assembly.data, assembly.size, &shaderBinary))
		return Fail("Thin1D reinterpret SPIR-V assembly failed");
	if (!spirvTools.Validate(shaderBinary)) return Fail("Thin1D reinterpret SPIR-V validation failed");
	std::string disassembly;
	if (!spirvTools.Disassemble(shaderBinary, &disassembly) ||
	    disassembly.find("OpCapability StorageImageExtendedFormats") == std::string::npos ||
	    (disassembly.find(" 2 R8") != std::string::npos) != (probe.direction != Direction::Rgba8ToRgba8) ||
	    disassembly.find(" 2 Rgba8") == std::string::npos ||
	    CountToken(disassembly, "OpImageRead") != 1 || CountToken(disassembly, "OpImageWrite") != 1 ||
	    CountToken(disassembly, "OpFunctionCall") != probe.expected_function_calls ||
	    CountToken(disassembly, "NonWritable") != 1 ||
	    CountToken(disassembly, "NonReadable") != 1)
		return Fail("Thin1D reinterpret SPIR-V lost its typed storage-image shape");

	const size_t sourceBytes = ImageBytes(probe.source_width, probe.source_height, probe.source_bytes_per_pixel);
	const size_t destinationBytes =
	    ImageBytes(probe.destination_width, probe.destination_height, probe.destination_bytes_per_pixel);
	constexpr size_t readbackGuardBytes = 64;
	const size_t sourceReadbackOffset = readbackGuardBytes;
	const size_t middleReadbackOffset = sourceReadbackOffset + sourceBytes;
	const size_t destinationReadbackOffset = middleReadbackOffset + readbackGuardBytes;
	const size_t readbackBytes = destinationReadbackOffset + destinationBytes + readbackGuardBytes;
	const std::vector<uint8_t> source = MakeSourceImage(probe);
	const std::vector<uint8_t> expectedDestination = MakeExpectedDestination(probe, source);
	if (!ContainsAllByteValues(expectedDestination)) return Fail("Thin1D destination fixture does not cover every byte value");
	if (std::memcmp(expectedDestination.data(), source.data(), destinationBytes) == 0)
		return Fail("Thin1D fixture accidentally matches a naive linear reinterpretation");
	if (probe.direction == Direction::Rgba8ToRgba8 && MatchesTopLeftRectangle(probe, source, expectedDestination))
		return Fail("Thin1D RGBA8 fixture accidentally matches a top-left rectangle copy");

	ProbeState state;
	VkApplicationInfo applicationInfo {};
	applicationInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName   = "Lumen Thin1D reinterpret compute probe";
	applicationInfo.applicationVersion = 1;
	applicationInfo.pEngineName        = "Lumen";
	applicationInfo.engineVersion      = 1;
	applicationInfo.apiVersion         = VK_API_VERSION_1_1;
	VkInstanceCreateInfo instanceInfo {};
	instanceInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &applicationInfo;
	VkResult result               = vkCreateInstance(&instanceInfo, nullptr, &state.instance);
	if (result != VK_SUCCESS) return Fail("vkCreateInstance failed", result);

	uint32_t physicalDeviceCount = 0;
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0) return Fail("No Vulkan physical device found", result);
	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS) return Fail("vkEnumeratePhysicalDevices(list) failed", result);

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t queueFamily = UINT32_MAX;
	for (VkPhysicalDevice candidate: physicalDevices)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
		for (uint32_t family = 0; family < familyCount; ++family)
		{
			// Compute queues support transfer commands even when the optional
			// VK_QUEUE_TRANSFER_BIT is omitted from their reported capabilities.
			if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && families[family].queueCount > 0)
			{
				physicalDevice = candidate;
				queueFamily = family;
				break;
			}
		}
		if (physicalDevice != VK_NULL_HANDLE) break;
	}
	if (physicalDevice == VK_NULL_HANDLE) return Fail("No compute/transfer Vulkan queue found");

	VkPhysicalDeviceProperties deviceProperties {};
	vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
	std::fprintf(stderr, "Thin1D reinterpret device=%s vendor=0x%04x device-id=0x%04x api=%u.%u.%u\n",
	             deviceProperties.deviceName, deviceProperties.vendorID, deviceProperties.deviceID,
	             VK_VERSION_MAJOR(deviceProperties.apiVersion), VK_VERSION_MINOR(deviceProperties.apiVersion),
	             VK_VERSION_PATCH(deviceProperties.apiVersion));

	VkPhysicalDeviceFeatures availableFeatures {};
	vkGetPhysicalDeviceFeatures(physicalDevice, &availableFeatures);
	if (availableFeatures.shaderStorageImageExtendedFormats != VK_TRUE)
		return Fail("Vulkan device lacks shaderStorageImageExtendedFormats");
	for (uint32_t index = 0; index < 2; ++index)
	{
		const VkFormat format = index == 0 ? probe.source_format : probe.destination_format;
		VkFormatProperties properties {};
		vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
		const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
		                                      (index == 0 ? VK_FORMAT_FEATURE_TRANSFER_DST_BIT : 0);
		if ((properties.optimalTilingFeatures & required) != required)
			return Fail(index == 0 ? "Vulkan source lacks storage/transfer support"
			                       : "Vulkan destination lacks storage/transfer support");
	}

	constexpr float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	VkPhysicalDeviceFeatures enabledFeatures {};
	enabledFeatures.shaderStorageImageExtendedFormats = VK_TRUE;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos    = &queueInfo;
	deviceInfo.pEnabledFeatures     = &enabledFeatures;
	result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &state.device);
	if (result != VK_SUCCESS) return Fail("vkCreateDevice failed", result);
	vkGetDeviceQueue(state.device, queueFamily, 0, &state.queue);
	if (state.queue == VK_NULL_HANDLE) return Fail("vkGetDeviceQueue returned null");

	result = CreateMappedBuffer(physicalDevice, state.device, sourceBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                            &state.uploadBuffer, &state.uploadMemory, &state.mappedUpload);
	if (result != VK_SUCCESS) return Fail("CreateMappedBuffer(upload) failed", result);
	result = CreateMappedBuffer(physicalDevice, state.device, readbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                            &state.readbackBuffer, &state.readbackMemory, &state.mappedReadback);
	if (result != VK_SUCCESS) return Fail("CreateMappedBuffer(readback) failed", result);
	std::memcpy(state.mappedUpload, source.data(), source.size());
	std::memset(state.mappedReadback, 0xa5, readbackBytes);
	VkMappedMemoryRange uploadRange {};
	uploadRange.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	uploadRange.memory = state.uploadMemory;
	uploadRange.size   = VK_WHOLE_SIZE;
	result = vkFlushMappedMemoryRanges(state.device, 1, &uploadRange);
	if (result != VK_SUCCESS) return Fail("vkFlushMappedMemoryRanges(upload) failed", result);

	result = CreateDeviceLocalImage(physicalDevice, state.device, probe.source_format,
	                                probe.source_width, probe.source_height,
	                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	                                &state.images[0], &state.imageMemories[0]);
	if (result != VK_SUCCESS) return Fail("CreateDeviceLocalImage(source) failed", result);
	result = CreateDeviceLocalImage(physicalDevice, state.device, probe.destination_format,
	                                probe.destination_width, probe.destination_height,
	                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
	                                &state.images[1], &state.imageMemories[1]);
	if (result != VK_SUCCESS) return Fail("CreateDeviceLocalImage(destination) failed", result);
	result = CreateImageView(state.device, state.images[0], probe.source_format, &state.imageViews[0]);
	if (result != VK_SUCCESS) return Fail("CreateImageView(source) failed", result);
	result = CreateImageView(state.device, state.images[1], probe.destination_format, &state.imageViews[1]);
	if (result != VK_SUCCESS) return Fail("CreateImageView(destination) failed", result);

	std::array<VkDescriptorSetLayoutBinding, 2> bindings {};
	for (uint32_t index = 0; index < bindings.size(); ++index)
	{
		bindings[index].binding         = index;
		bindings[index].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo setLayoutInfo {};
	setLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	setLayoutInfo.pBindings    = bindings.data();
	result = vkCreateDescriptorSetLayout(state.device, &setLayoutInfo, nullptr, &state.descriptorSetLayout);
	if (result != VK_SUCCESS) return Fail("vkCreateDescriptorSetLayout failed", result);
	VkPushConstantRange pushRange {};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.size       = sizeof(PushConstants);
	VkPipelineLayoutCreateInfo pipelineLayoutInfo {};
	pipelineLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount         = 1;
	pipelineLayoutInfo.pSetLayouts            = &state.descriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges    = &pushRange;
	result = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS) return Fail("vkCreatePipelineLayout failed", result);

	VkDescriptorPoolSize poolSize {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
	VkDescriptorPoolCreateInfo poolInfo {};
	poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets       = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes    = &poolSize;
	result = vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS) return Fail("vkCreateDescriptorPool failed", result);
	VkDescriptorSetAllocateInfo setInfo {};
	setInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool     = state.descriptorPool;
	setInfo.descriptorSetCount = 1;
	setInfo.pSetLayouts        = &state.descriptorSetLayout;
	result = vkAllocateDescriptorSets(state.device, &setInfo, &state.descriptorSet);
	if (result != VK_SUCCESS) return Fail("vkAllocateDescriptorSets failed", result);
	std::array<VkDescriptorImageInfo, 2> imageInfos {};
	std::array<VkWriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index)
	{
		imageInfos[index].imageView   = state.imageViews[index];
		imageInfos[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		writes[index].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet          = state.descriptorSet;
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[index].pImageInfo      = &imageInfos[index];
	}
	vkUpdateDescriptorSets(state.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

	VkShaderModuleCreateInfo shaderInfo {};
	shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = shaderBinary.size() * sizeof(uint32_t);
	shaderInfo.pCode    = shaderBinary.data();
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.shaderModule);
	if (result != VK_SUCCESS) return Fail("vkCreateShaderModule failed", result);
	VkPipelineShaderStageCreateInfo stageInfo {};
	stageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = state.shaderModule;
	stageInfo.pName  = "main";
	VkComputePipelineCreateInfo pipelineInfo {};
	pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage  = stageInfo;
	pipelineInfo.layout = state.pipelineLayout;
	result = vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.pipeline);
	if (result != VK_SUCCESS) return Fail("vkCreateComputePipelines failed", result);

	VkCommandPoolCreateInfo commandPoolInfo {};
	commandPoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	commandPoolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(state.device, &commandPoolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS) return Fail("vkCreateCommandPool failed", result);
	VkCommandBufferAllocateInfo commandBufferInfo {};
	commandBufferInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferInfo.commandPool        = state.commandPool;
	commandBufferInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferInfo.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(state.device, &commandBufferInfo, &state.commandBuffer);
	if (result != VK_SUCCESS) return Fail("vkAllocateCommandBuffers failed", result);
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(state.device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS) return Fail("vkCreateFence failed", result);

	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS) return Fail("vkBeginCommandBuffer failed", result);
	VkBufferMemoryBarrier uploadBarrier {};
	uploadBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	uploadBarrier.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
	uploadBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
	uploadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	uploadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	uploadBarrier.buffer              = state.uploadBuffer;
	uploadBarrier.size                = sourceBytes;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 1, &uploadBarrier, 0, nullptr);

	VkImageMemoryBarrier sourceForUpload {};
	sourceForUpload.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	sourceForUpload.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	sourceForUpload.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
	sourceForUpload.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	sourceForUpload.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sourceForUpload.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	sourceForUpload.image                = state.images[0];
	sourceForUpload.subresourceRange     = ColorRange();
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, 1, &sourceForUpload);
	VkBufferImageCopy sourceCopy {};
	sourceCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	sourceCopy.imageExtent      = {probe.source_width, probe.source_height, 1};
	vkCmdCopyBufferToImage(state.commandBuffer, state.uploadBuffer, state.images[0],
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &sourceCopy);

	std::array<VkImageMemoryBarrier, 2> imagesForCompute {};
	imagesForCompute[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imagesForCompute[0].srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	imagesForCompute[0].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
	imagesForCompute[0].oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	imagesForCompute[0].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
	imagesForCompute[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imagesForCompute[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imagesForCompute[0].image                = state.images[0];
	imagesForCompute[0].subresourceRange     = ColorRange();
	imagesForCompute[1]                      = imagesForCompute[0];
	imagesForCompute[1].srcAccessMask        = 0;
	imagesForCompute[1].dstAccessMask        = VK_ACCESS_SHADER_WRITE_BIT;
	imagesForCompute[1].oldLayout            = VK_IMAGE_LAYOUT_UNDEFINED;
	imagesForCompute[1].image                = state.images[1];
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(imagesForCompute.size()), imagesForCompute.data());

	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout,
	                        0, 1, &state.descriptorSet, 0, nullptr);
	const PushConstants push {
	    probe.source_pitch / kTileWidth,
	    probe.destination_pitch / kTileWidth,
	    probe.source_width,
	    probe.source_height,
	    probe.destination_width,
	    probe.destination_height,
	};
	vkCmdPushConstants(state.commandBuffer, state.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
	                   0, sizeof(push), &push);
	vkCmdDispatch(state.commandBuffer, (probe.destination_width + kTileWidth - 1u) / kTileWidth,
	              (probe.destination_height + kTileWidth - 1u) / kTileWidth, 1);

	std::array<VkImageMemoryBarrier, 2> imagesForReadback = imagesForCompute;
	for (size_t index = 0; index < imagesForReadback.size(); ++index)
	{
		imagesForReadback[index].srcAccessMask = index == 0 ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
		imagesForReadback[index].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		imagesForReadback[index].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
		imagesForReadback[index].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
	}
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(imagesForReadback.size()), imagesForReadback.data());
	std::array<VkBufferImageCopy, 2> readbackCopies {};
	readbackCopies[0].bufferOffset                = sourceReadbackOffset;
	readbackCopies[0].imageSubresource            = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	readbackCopies[0].imageExtent                 = {probe.source_width, probe.source_height, 1};
	readbackCopies[1].bufferOffset                = destinationReadbackOffset;
	readbackCopies[1].imageSubresource            = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	readbackCopies[1].imageExtent                 = {probe.destination_width, probe.destination_height, 1};
	for (size_t index = 0; index < state.images.size(); ++index)
		vkCmdCopyImageToBuffer(state.commandBuffer, state.images[index], VK_IMAGE_LAYOUT_GENERAL,
		                       state.readbackBuffer, 1, &readbackCopies[index]);
	VkBufferMemoryBarrier readbackBarrier {};
	readbackBarrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	readbackBarrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	readbackBarrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	readbackBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	readbackBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	readbackBarrier.buffer              = state.readbackBuffer;
	readbackBarrier.size                = readbackBytes;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 0, nullptr, 1, &readbackBarrier, 0, nullptr);
	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS) return Fail("vkEndCommandBuffer failed", result);

	VkSubmitInfo submitInfo {};
	submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers    = &state.commandBuffer;
	result = vkQueueSubmit(state.queue, 1, &submitInfo, state.fence);
	if (result != VK_SUCCESS) return Fail("vkQueueSubmit failed", result);
	result = vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS) return Fail("vkWaitForFences failed", result);

	VkMappedMemoryRange readbackRange {};
	readbackRange.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	readbackRange.memory = state.readbackMemory;
	readbackRange.size   = VK_WHOLE_SIZE;
	result = vkInvalidateMappedMemoryRanges(state.device, 1, &readbackRange);
	if (result != VK_SUCCESS) return Fail("vkInvalidateMappedMemoryRanges(readback) failed", result);
	const auto* observed = static_cast<const uint8_t*>(state.mappedReadback);
	if (!VerifyCanary(observed, readbackGuardBytes, 0xa5, "leading readback canary") ||
	    !VerifyBytes(observed + sourceReadbackOffset, source, probe.source_name) ||
	    !VerifyCanary(observed + middleReadbackOffset, readbackGuardBytes, 0xa5, "middle readback canary") ||
	    !VerifyBytes(observed + destinationReadbackOffset, expectedDestination, probe.destination_name) ||
	    !VerifyCanary(observed + destinationReadbackOffset + destinationBytes, readbackGuardBytes, 0xa5,
	                  "trailing readback canary"))
		return 1;
	return 0;
}

} // namespace

int main()
{
	const auto unsupported = ReinterpretShader::GetAssembly(Direction::Unsupported);
	if (unsupported.data != nullptr || unsupported.size != 0)
		return Fail("Thin1D reinterpret unsupported direction is not fail-closed");
	for (const auto& probe: kProbeCases)
	{
		std::fprintf(stderr, "Thin1D reinterpret probe direction=%s\n", probe.name);
		if (RunProbe(probe) != 0) return 1;
	}

	const int marker = std::printf(
	    "[GFX_THIN1D_REINTERPRET_COMPUTE] backend=%s "
	    "directions=r8-unorm:512x512->rgba8-unorm:48x48+rgba8-unorm:1920x1080->r8-unorm:512x512+"
	    "r8-unorm:512x512->rgba8-unorm:182x5/pitch256+"
	    "rgba8-unorm:64x32/pitch64->rgba8-unorm:32x24/pitch32+"
	    "rgba8-unorm:1920x1080/pitch1920->rgba8-unorm:1503x46/pitch2048 "
	    "partial-directions=r8-unorm:512x512->rgba8-unorm:74x68/pitch80+"
	    "rgba8-unorm:256x128/pitch256->rgba8-unorm:71x65/pitch72 "
	    "artifacts=runtime-shared bindings=storage+storage layout=thin1d-prefix byte-domain=all-256-both "
	    "padded-destination=logical-texels-only pitch-change=raw-microtile-order "
	    "canaries=clean source=unchanged destination=byte-exact "
	    "lifetime=through-fence status=clean\n", kBackend);
	if (marker <= 0 || std::fflush(stdout) != 0) return Fail("Failed to emit Thin1D reinterpret marker");
	return 0;
}
