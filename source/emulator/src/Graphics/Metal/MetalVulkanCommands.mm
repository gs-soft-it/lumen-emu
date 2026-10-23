#include "MetalVulkanInternal.h"

#import <TargetConditionals.h>

#include <spirv_msl.hpp>

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#error "Native Metal must be compiled with C++ exceptions disabled"
#endif

#ifndef SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
#error "SPIRV-Cross consumers must use the no-exception error policy"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kPushConstantBufferIndex = 30;
constexpr uint32_t kMaximumMetalBufferIndex = 30;
constexpr uint32_t kMaximumMetalTextureIndex = 127;
constexpr uint32_t kMaximumMetalSamplerIndex = 15;

class MetalSpirvCompiler final: public spirv_cross::CompilerMSL
{
public:
	using spirv_cross::CompilerMSL::CompilerMSL;

protected:
	std::string type_to_array_glsl(const spirv_cross::SPIRType& type, uint32_t variableId) override
	{
		// SPIRV-Cross's native-array path treats declaration IDs as SPIRVariable,
		// including the SPIRUndef constants emitted at global scope. Undef has no
		// storage or resource binding; preserve its type and array dimensions while
		// omitting the variable-only lookup. Real resource variables keep their IDs.
		if (maybe_get<spirv_cross::SPIRUndef>(variableId) != nullptr)
		{
			variableId = 0;
		}
		return spirv_cross::CompilerMSL::type_to_array_glsl(type, variableId);
	}
};

bool CompletionPassReuseRequested()
{
#if defined(__aarch64__) && TARGET_OS_OSX && !TARGET_OS_IPHONE
	static const bool requested = [] {
		const char* value = std::getenv("LUMEN_METAL_COMPLETION_PASS_REUSE");
		return value == nullptr || (value[0] == '1' && value[1] == '\0');
	}();
	return requested;
#else
	return false;
#endif
}

bool CompletionPassTraceRequested()
{
	static const bool requested = [] {
		const char* value = std::getenv("LUMEN_METAL_COMPLETION_PASS_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return requested;
}

struct CompletionPassReuseCounters
{
	std::atomic<uint64_t> commandBuffers {0};
	std::atomic<uint64_t> logical {0};
	std::atomic<uint64_t> physical {0};
	std::atomic<uint64_t> merged {0};
	std::atomic<uint64_t> mergedAfterMarker {0};
	std::atomic<uint64_t> resourceRejected {0};
};

CompletionPassReuseCounters gCompletionPassReuseCounters;

void PrintCompletionPassReuseCounters(const char* phase)
{
	const auto& counters = gCompletionPassReuseCounters;
	std::fprintf(stderr,
	    "[METAL_COMPLETION_PASS_REUSE] phase=%s enabled=%u command_buffers=%llu logical=%llu physical=%llu merged=%llu merged_after_completion_marker=%llu resource_rejected=%llu\n",
	    phase, CompletionPassReuseRequested(),
	    static_cast<unsigned long long>(counters.commandBuffers.load(std::memory_order_relaxed)),
	    static_cast<unsigned long long>(counters.logical.load(std::memory_order_relaxed)),
	    static_cast<unsigned long long>(counters.physical.load(std::memory_order_relaxed)),
	    static_cast<unsigned long long>(counters.merged.load(std::memory_order_relaxed)),
	    static_cast<unsigned long long>(counters.mergedAfterMarker.load(std::memory_order_relaxed)),
	    static_cast<unsigned long long>(counters.resourceRejected.load(std::memory_order_relaxed)));
	std::fflush(stderr);
}

#if defined(__APPLE__) && defined(__aarch64__) && TARGET_OS_OSX && \
    !TARGET_OS_IPHONE && defined(NDEBUG)
#define LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION_COMPILED 1
#else
#define LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION_COMPILED 0
#endif

struct DescriptorValue
{
	VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceSize offset = 0;
	VkDeviceSize range = 0;
	VkImageView imageView = VK_NULL_HANDLE;
	VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkSampler sampler = VK_NULL_HANDLE;
	bool immutableSampler = false;
};

struct StageBinding
{
	uint32_t set = 0;
	uint32_t binding = 0;
	uint32_t count = 0;
	uint32_t metalBufferIndex = UINT32_MAX;
	uint32_t metalTextureIndex = UINT32_MAX;
	uint32_t metalSamplerIndex = UINT32_MAX;
	VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
	VkShaderStageFlagBits stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
};

struct DynamicStencilState
{
	uint32_t frontCompareMask = UINT32_MAX;
	uint32_t backCompareMask = UINT32_MAX;
	uint32_t frontWriteMask = UINT32_MAX;
	uint32_t backWriteMask = UINT32_MAX;
	uint32_t frontReference = 0;
	uint32_t backReference = 0;
};

struct ReplayState
{
	VkPipeline graphicsPipeline = VK_NULL_HANDLE;
	VkPipeline computePipeline = VK_NULL_HANDLE;
	std::array<VkDescriptorSet, 8> graphicsSets {};
	std::array<VkDescriptorSet, 8> computeSets {};
	std::array<VkBuffer, 16> vertexBuffers {};
	std::array<VkDeviceSize, 16> vertexOffsets {};
	VkBuffer indexBuffer = VK_NULL_HANDLE;
	VkDeviceSize indexOffset = 0;
	VkIndexType indexType = VK_INDEX_TYPE_UINT16;
	std::vector<uint8_t> vertexPushConstants;
	std::vector<uint8_t> fragmentPushConstants;
	std::vector<uint8_t> computePushConstants;
	DynamicStencilState stencil;
	VkRect2D scissor {};
	VkViewport viewport {};
	bool viewportSet = false;
	std::array<VkDeviceSize, 16> vertexStrides {};
	VkBool32 depthTest = false, depthWrite = false, stencilTest = false, depthBiasEnable = false;
	VkBool32 rasterizerDiscard = false, primitiveRestart = false;
	VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
	VkStencilOpState frontStencil {}, backStencil {};
	VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
	VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	std::array<float, 4> blendConstants {};
	float depthBiasConstant = 0, depthBiasSlope = 0, depthBiasClamp = 0;
	bool scissorSet = false;
	bool colorWriteEnabled = true;
	float lineWidth = 1.0f;
};

#if LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION_COMPILED
struct MetalStateOnlyRecordElisionTelemetry
{
	std::atomic<bool> stateOnlyElided {false};
	std::atomic<bool> snapshotConsumed {false};
	std::once_flag finalizeOnce;
	bool finalizeResult = true;
};

MetalStateOnlyRecordElisionTelemetry gMetalStateOnlyRecordElisionTelemetry;
#endif

bool MetalStateOnlyRecordElisionSelectorRequested()
{
#if LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION_COMPILED
	// Freeze the exact-one selector before the first command-buffer mutation.
	// Absent, empty, zero and malformed values retain the legacy replay path.
	static const bool requested = []()
	{
		const char* value = std::getenv("LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return requested;
#else
	return false;
#endif
}

MTLLoadAction ToMTLLoadAction(VkAttachmentLoadOp operation)
{
	switch (operation)
	{
		case VK_ATTACHMENT_LOAD_OP_LOAD: return MTLLoadActionLoad;
		case VK_ATTACHMENT_LOAD_OP_CLEAR: return MTLLoadActionClear;
		case VK_ATTACHMENT_LOAD_OP_DONT_CARE: return MTLLoadActionDontCare;
		default: return MTLLoadActionDontCare;
	}
}

MTLStoreAction ToMTLStoreAction(VkAttachmentStoreOp operation)
{
	return operation == VK_ATTACHMENT_STORE_OP_STORE ? MTLStoreActionStore : MTLStoreActionDontCare;
}

MTLCompareFunction ToMTLCompareFunction(VkCompareOp operation)
{
	switch (operation)
	{
		case VK_COMPARE_OP_NEVER: return MTLCompareFunctionNever;
		case VK_COMPARE_OP_LESS: return MTLCompareFunctionLess;
		case VK_COMPARE_OP_EQUAL: return MTLCompareFunctionEqual;
		case VK_COMPARE_OP_LESS_OR_EQUAL: return MTLCompareFunctionLessEqual;
		case VK_COMPARE_OP_GREATER: return MTLCompareFunctionGreater;
		case VK_COMPARE_OP_NOT_EQUAL: return MTLCompareFunctionNotEqual;
		case VK_COMPARE_OP_GREATER_OR_EQUAL: return MTLCompareFunctionGreaterEqual;
		case VK_COMPARE_OP_ALWAYS: return MTLCompareFunctionAlways;
		default: return MTLCompareFunctionAlways;
	}
}

MTLStencilOperation ToMTLStencilOperation(VkStencilOp operation)
{
	switch (operation)
	{
		case VK_STENCIL_OP_KEEP: return MTLStencilOperationKeep;
		case VK_STENCIL_OP_ZERO: return MTLStencilOperationZero;
		case VK_STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
		case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return MTLStencilOperationIncrementClamp;
		case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return MTLStencilOperationDecrementClamp;
		case VK_STENCIL_OP_INVERT: return MTLStencilOperationInvert;
		case VK_STENCIL_OP_INCREMENT_AND_WRAP: return MTLStencilOperationIncrementWrap;
		case VK_STENCIL_OP_DECREMENT_AND_WRAP: return MTLStencilOperationDecrementWrap;
		default: return MTLStencilOperationKeep;
	}
}

MTLBlendFactor ToMTLBlendFactor(VkBlendFactor factor)
{
	switch (factor)
	{
		case VK_BLEND_FACTOR_ZERO: return MTLBlendFactorZero;
		case VK_BLEND_FACTOR_ONE: return MTLBlendFactorOne;
		case VK_BLEND_FACTOR_SRC_COLOR: return MTLBlendFactorSourceColor;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
		case VK_BLEND_FACTOR_DST_COLOR: return MTLBlendFactorDestinationColor;
		case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
		case VK_BLEND_FACTOR_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
		case VK_BLEND_FACTOR_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
		case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
		case VK_BLEND_FACTOR_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
		case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
		case VK_BLEND_FACTOR_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
		case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
		case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
		case VK_BLEND_FACTOR_SRC1_COLOR: return MTLBlendFactorSource1Color;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
		case VK_BLEND_FACTOR_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
		default: return MTLBlendFactorZero;
	}
}

MTLBlendOperation ToMTLBlendOperation(VkBlendOp operation)
{
	switch (operation)
	{
		case VK_BLEND_OP_ADD: return MTLBlendOperationAdd;
		case VK_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
		case VK_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
		case VK_BLEND_OP_MIN: return MTLBlendOperationMin;
		case VK_BLEND_OP_MAX: return MTLBlendOperationMax;
		default: return MTLBlendOperationAdd;
	}
}

MTLColorWriteMask ToMTLColorWriteMask(VkColorComponentFlags flags)
{
	MTLColorWriteMask mask = MTLColorWriteMaskNone;
	if ((flags & VK_COLOR_COMPONENT_R_BIT) != 0) mask |= MTLColorWriteMaskRed;
	if ((flags & VK_COLOR_COMPONENT_G_BIT) != 0) mask |= MTLColorWriteMaskGreen;
	if ((flags & VK_COLOR_COMPONENT_B_BIT) != 0) mask |= MTLColorWriteMaskBlue;
	if ((flags & VK_COLOR_COMPONENT_A_BIT) != 0) mask |= MTLColorWriteMaskAlpha;
	return mask;
}

constexpr MTLVertexFormat ToMTLVertexFormat(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R32_SFLOAT: return MTLVertexFormatFloat;
		case VK_FORMAT_R32G32_SFLOAT: return MTLVertexFormatFloat2;
		case VK_FORMAT_R32G32B32_SFLOAT: return MTLVertexFormatFloat3;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return MTLVertexFormatFloat4;
		case VK_FORMAT_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
#if !TARGET_OS_IPHONE
		case VK_FORMAT_R8G8B8A8_SNORM: return MTLVertexFormatChar4Normalized;
#endif
		case VK_FORMAT_R16G16_SINT: return MTLVertexFormatShort2;
		case VK_FORMAT_R16G16B16A16_SINT: return MTLVertexFormatShort4;
		default: return MTLVertexFormatInvalid;
	}
}

static_assert(ToMTLVertexFormat(VK_FORMAT_R16G16_SINT) == MTLVertexFormatShort2);
static_assert(ToMTLVertexFormat(VK_FORMAT_R16G16B16A16_SINT) == MTLVertexFormatShort4);
static_assert(ToMTLVertexFormat(VK_FORMAT_R16G16_SNORM) == MTLVertexFormatInvalid);
static_assert(ToMTLVertexFormat(VK_FORMAT_R16G16_SSCALED) == MTLVertexFormatInvalid);
static_assert(ToMTLVertexFormat(VK_FORMAT_R16G16B16A16_SNORM) == MTLVertexFormatInvalid);
static_assert(ToMTLVertexFormat(VK_FORMAT_R16G16B16A16_SSCALED) == MTLVertexFormatInvalid);
static_assert(ToMTLVertexFormat(VK_FORMAT_R32G32_SFLOAT) == MTLVertexFormatFloat2);
static_assert(ToMTLVertexFormat(VK_FORMAT_R8G8B8A8_UNORM) == MTLVertexFormatUChar4Normalized);
#if !TARGET_OS_IPHONE
static_assert(ToMTLVertexFormat(VK_FORMAT_R8G8B8A8_SNORM) == MTLVertexFormatChar4Normalized);
#else
static_assert(ToMTLVertexFormat(VK_FORMAT_R8G8B8A8_SNORM) == MTLVertexFormatInvalid);
#endif

MTLPrimitiveType ToMTLPrimitiveType(VkPrimitiveTopology topology)
{
	switch (topology)
	{
		case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return MTLPrimitiveTypePoint;
		case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return MTLPrimitiveTypeLine;
		case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return MTLPrimitiveTypeTriangle;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
		default: return MTLPrimitiveTypeTriangle;
	}
}

MTLIndexType ToMTLIndexType(VkIndexType type)
{
	return type == VK_INDEX_TYPE_UINT32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
}

uint32_t IndexTypeSize(VkIndexType type)
{
	return type == VK_INDEX_TYPE_UINT32 ? 4u : 2u;
}

bool IsDepthFormat(VkFormat format)
{
	return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ||
	       format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool HasStencil(VkFormat format)
{
	return format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

uint32_t BytesPerPixel(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_R8_UNORM:
		case VK_FORMAT_S8_UINT: return 1;
		case VK_FORMAT_R8G8_UNORM:
		case VK_FORMAT_D16_UNORM: return 2;
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_R32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT: return 4;
		case VK_FORMAT_R32G32_SFLOAT:
		case VK_FORMAT_D32_SFLOAT_S8_UINT: return 8;
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
		case VK_FORMAT_R32G32_UINT: return 8;
		case VK_FORMAT_R32G32_SINT: return 8;
		case VK_FORMAT_R32G32B32A32_UINT: return 16;
		case VK_FORMAT_R32G32B32A32_SINT: return 16;
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
		case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 4;
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return 4;
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 4;
		case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return 4;
		default: return 0;
	}
}

uint32_t BytesPerBlock(VkFormat format)
{
	switch (format)
	{
		case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return 8;
		case VK_FORMAT_BC2_UNORM_BLOCK:
		case VK_FORMAT_BC3_UNORM_BLOCK:
		case VK_FORMAT_BC3_SRGB_BLOCK:
		case VK_FORMAT_BC7_UNORM_BLOCK: return 16;
		default: return 0;
	}
}

struct LinearLayout
{
	NSUInteger bytesPerRow = 0;
	NSUInteger bytesPerImage = 0;
	VkDeviceSize layerStride = 0;
	VkDeviceSize requiredSpan = 0;
};

struct BufferImageTransfer
{
	VkFormat      linearFormat = VK_FORMAT_UNDEFINED;
	MTLBlitOption options      = MTLBlitOptionNone;
	bool          supported    = false;
};

constexpr BufferImageTransfer ResolveBufferImageTransfer(VkFormat imageFormat, VkImageAspectFlags aspectMask)
{
	if (imageFormat != VK_FORMAT_D32_SFLOAT_S8_UINT)
		return {imageFormat, MTLBlitOptionNone, true};
	if (aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT)
		return {VK_FORMAT_D32_SFLOAT, MTLBlitOptionDepthFromDepthStencil, true};
	if (aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT)
		return {VK_FORMAT_S8_UINT, MTLBlitOptionStencilFromDepthStencil, true};
	return {};
}

static_assert(ResolveBufferImageTransfer(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT).linearFormat ==
	          VK_FORMAT_D32_SFLOAT);
static_assert(ResolveBufferImageTransfer(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT).options ==
	          MTLBlitOptionDepthFromDepthStencil);
static_assert(ResolveBufferImageTransfer(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT).linearFormat ==
	          VK_FORMAT_S8_UINT);
static_assert(ResolveBufferImageTransfer(VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_STENCIL_BIT).options ==
	          MTLBlitOptionStencilFromDepthStencil);
static_assert(!ResolveBufferImageTransfer(VK_FORMAT_D32_SFLOAT_S8_UINT, 0).supported);
static_assert(!ResolveBufferImageTransfer(VK_FORMAT_D32_SFLOAT_S8_UINT,
	                                      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT).supported);

bool TryMultiplyDeviceSize(VkDeviceSize left, VkDeviceSize right, VkDeviceSize* result)
{
	if (result == nullptr || (left != 0 && right > std::numeric_limits<VkDeviceSize>::max() / left))
		return false;
	*result = left * right;
	return true;
}

bool TryAddDeviceSize(VkDeviceSize left, VkDeviceSize right, VkDeviceSize* result)
{
	if (result == nullptr || right > std::numeric_limits<VkDeviceSize>::max() - left)
		return false;
	*result = left + right;
	return true;
}

VkDeviceSize DivideRoundUp(VkDeviceSize value, VkDeviceSize divisor)
{
	return value / divisor + (value % divisor != 0 ? 1 : 0);
}

bool BuildLinearLayout(VkFormat format, const VkBufferImageCopy& copy, LinearLayout* result)
{
	if (result == nullptr || copy.imageExtent.width == 0 || copy.imageExtent.height == 0 ||
	    copy.imageExtent.depth == 0 || copy.imageSubresource.layerCount == 0 ||
	    (copy.bufferRowLength != 0 && copy.bufferRowLength < copy.imageExtent.width) ||
	    (copy.bufferImageHeight != 0 && copy.bufferImageHeight < copy.imageExtent.height))
		return false;

	const VkDeviceSize rowLength = copy.bufferRowLength == 0 ? copy.imageExtent.width : copy.bufferRowLength;
	const VkDeviceSize imageHeight =
	    copy.bufferImageHeight == 0 ? copy.imageExtent.height : copy.bufferImageHeight;
	const uint32_t blockBytes = BytesPerBlock(format);
	const uint32_t pixelBytes = BytesPerPixel(format);
	if (blockBytes == 0 && pixelBytes == 0)
		return false;

	VkDeviceSize rowElements = rowLength;
	VkDeviceSize imageRows = imageHeight;
	VkDeviceSize copiedRowElements = copy.imageExtent.width;
	VkDeviceSize copiedRows = copy.imageExtent.height;
	if (blockBytes != 0)
	{
		if ((copy.bufferRowLength != 0 && copy.bufferRowLength % 4u != 0) ||
		    (copy.bufferImageHeight != 0 && copy.bufferImageHeight % 4u != 0))
			return false;
		rowElements = DivideRoundUp(rowLength, 4);
		imageRows = DivideRoundUp(imageHeight, 4);
		copiedRowElements = DivideRoundUp(copy.imageExtent.width, 4);
		copiedRows = DivideRoundUp(copy.imageExtent.height, 4);
	}

	const VkDeviceSize elementBytes = blockBytes != 0 ? blockBytes : pixelBytes;
	VkDeviceSize rowBytes = 0;
	VkDeviceSize imageBytes = 0;
	VkDeviceSize copiedRowBytes = 0;
	VkDeviceSize depthStride = 0;
	VkDeviceSize planeCount = 0;
	VkDeviceSize precedingPlanesBytes = 0;
	VkDeviceSize precedingRowsBytes = 0;
	VkDeviceSize requiredSpan = 0;
	if (!TryMultiplyDeviceSize(rowElements, elementBytes, &rowBytes) ||
	    !TryMultiplyDeviceSize(rowBytes, imageRows, &imageBytes) ||
	    !TryMultiplyDeviceSize(copiedRowElements, elementBytes, &copiedRowBytes) ||
	    !TryMultiplyDeviceSize(imageBytes, copy.imageExtent.depth, &depthStride) ||
	    !TryMultiplyDeviceSize(copy.imageSubresource.layerCount, copy.imageExtent.depth, &planeCount) ||
	    !TryMultiplyDeviceSize(planeCount - 1, imageBytes, &precedingPlanesBytes) ||
	    !TryMultiplyDeviceSize(copiedRows - 1, rowBytes, &precedingRowsBytes) ||
	    !TryAddDeviceSize(precedingPlanesBytes, precedingRowsBytes, &requiredSpan) ||
	    !TryAddDeviceSize(requiredSpan, copiedRowBytes, &requiredSpan) ||
	    rowBytes > std::numeric_limits<NSUInteger>::max() || imageBytes > std::numeric_limits<NSUInteger>::max())
		return false;

	*result = {static_cast<NSUInteger>(rowBytes), static_cast<NSUInteger>(imageBytes), depthStride, requiredSpan};
	return true;
}

bool ImageAspectMatchesFormat(VkFormat format, VkImageAspectFlags aspectMask)
{
	if (format == VK_FORMAT_D32_SFLOAT_S8_UINT)
		return aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT || aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT;
	if (IsDepthFormat(format))
		return aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT;
	if (HasStencil(format))
		return aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT;
	return aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
}

VkExtent3D MipExtent(VkImage image, uint32_t mipLevel)
{
	const auto dimension = [mipLevel](uint32_t value)
	{
		return mipLevel < std::numeric_limits<uint32_t>::digits ? std::max(1u, value >> mipLevel) : 1u;
	};
	return {dimension(image->extent.width), dimension(image->extent.height), dimension(image->extent.depth)};
}

VkResult ValidateBufferImageCopy(VkBuffer buffer, VkImage image, const VkBufferImageCopy& copy,
	                             BufferImageTransfer* transfer, LinearLayout* layout)
{
	if (buffer == VK_NULL_HANDLE || image == VK_NULL_HANDLE ||
	    !buffer->alive.load(std::memory_order_acquire) || !image->alive.load(std::memory_order_acquire) ||
	    image->samples != VK_SAMPLE_COUNT_1_BIT || copy.imageSubresource.mipLevel >= image->mipLevels ||
	    copy.imageSubresource.layerCount == 0 || copy.imageSubresource.baseArrayLayer >= image->arrayLayers ||
	    copy.imageSubresource.layerCount > image->arrayLayers - copy.imageSubresource.baseArrayLayer ||
	    copy.imageOffset.x < 0 || copy.imageOffset.y < 0 || copy.imageOffset.z < 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (!ImageAspectMatchesFormat(image->format, copy.imageSubresource.aspectMask))
		return VK_ERROR_INITIALIZATION_FAILED;

	const VkExtent3D mipExtent = MipExtent(image, copy.imageSubresource.mipLevel);
	if (copy.imageExtent.width == 0 || copy.imageExtent.height == 0 || copy.imageExtent.depth == 0 ||
	    static_cast<uint64_t>(copy.imageOffset.x) + copy.imageExtent.width > mipExtent.width ||
	    static_cast<uint64_t>(copy.imageOffset.y) + copy.imageExtent.height > mipExtent.height ||
	    static_cast<uint64_t>(copy.imageOffset.z) + copy.imageExtent.depth > mipExtent.depth ||
	    (image->imageType == VK_IMAGE_TYPE_1D &&
	     (copy.imageOffset.y != 0 || copy.imageOffset.z != 0 || copy.imageExtent.height != 1 ||
	      copy.imageExtent.depth != 1)) ||
	    (image->imageType == VK_IMAGE_TYPE_2D && (copy.imageOffset.z != 0 || copy.imageExtent.depth != 1)) ||
	    (image->imageType == VK_IMAGE_TYPE_3D &&
	     (copy.imageSubresource.baseArrayLayer != 0 || copy.imageSubresource.layerCount != 1)))
		return VK_ERROR_INITIALIZATION_FAILED;

	const uint32_t blockBytes = BytesPerBlock(image->format);
	if (blockBytes != 0 &&
	    ((static_cast<uint32_t>(copy.imageOffset.x) % 4u) != 0 ||
	     (static_cast<uint32_t>(copy.imageOffset.y) % 4u) != 0 ||
	     (copy.imageExtent.width % 4u != 0 &&
	      static_cast<uint64_t>(copy.imageOffset.x) + copy.imageExtent.width != mipExtent.width) ||
	     (copy.imageExtent.height % 4u != 0 &&
	      static_cast<uint64_t>(copy.imageOffset.y) + copy.imageExtent.height != mipExtent.height)))
		return VK_ERROR_INITIALIZATION_FAILED;

	const BufferImageTransfer resolvedTransfer =
	    ResolveBufferImageTransfer(image->format, copy.imageSubresource.aspectMask);
	LinearLayout resolvedLayout {};
	if (!resolvedTransfer.supported)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	const uint32_t elementBytes = std::max(BytesPerBlock(resolvedTransfer.linearFormat),
	                                       BytesPerPixel(resolvedTransfer.linearFormat));
	if (copy.bufferOffset % 4u != 0 || elementBytes == 0 || copy.bufferOffset % elementBytes != 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (!BuildLinearLayout(resolvedTransfer.linearFormat, copy, &resolvedLayout) ||
	    copy.bufferOffset > buffer->size || resolvedLayout.requiredSpan > buffer->size - copy.bufferOffset)
		return VK_ERROR_INITIALIZATION_FAILED;

	*transfer = resolvedTransfer;
	*layout = resolvedLayout;
	return VK_SUCCESS;
}

VkResult ValidateCommandBufferForRecording(VkCommandBuffer commandBuffer)
{
	if (commandBuffer == VK_NULL_HANDLE || !commandBuffer->alive.load(std::memory_order_acquire))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return commandBuffer->state == Lumen::MetalVulkan::CommandBufferState::Recording
	           ? VK_SUCCESS
	           : VK_ERROR_INITIALIZATION_FAILED;
}

void RecordError(VkCommandBuffer commandBuffer, const char* operationName, VkResult error)
{
	if (commandBuffer == VK_NULL_HANDLE || error == VK_SUCCESS)
	{
		return;
	}
	std::lock_guard lock(commandBuffer->mutex);
	if (commandBuffer->recordingError == VK_SUCCESS)
	{
		commandBuffer->recordingError = error;
		commandBuffer->recordingErrorOperation = operationName;
	}
}

template <typename Operation>
void Record(VkCommandBuffer commandBuffer, const char* operationName, Operation&& operation)
{
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return;
	}
	std::lock_guard lock(commandBuffer->mutex);
	if (ValidateCommandBufferForRecording(commandBuffer) != VK_SUCCESS)
	{
		if (commandBuffer->recordingError == VK_SUCCESS)
		{
			commandBuffer->recordingError = VK_ERROR_INITIALIZATION_FAILED;
			commandBuffer->recordingErrorOperation = operationName;
		}
		return;
	}
	commandBuffer->operations.push_back(
	    {operationName != nullptr ? operationName : "unknown", std::forward<Operation>(operation)});
}

bool TryElideMetalStateOnlyRecord(VkCommandBuffer commandBuffer, const char* operationName)
{
#if LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION_COMPILED
	if (!MetalStateOnlyRecordElisionSelectorRequested()) return false;
	if (commandBuffer == VK_NULL_HANDLE) return true;

	// State mutation remains externally synchronized by the Vulkan contract.
	// Retain Record's internal lifecycle/error check while omitting only the
	// redundant ReplayState copy and std::function operation.
	std::lock_guard lock(commandBuffer->mutex);
	if (ValidateCommandBufferForRecording(commandBuffer) != VK_SUCCESS)
	{
		if (commandBuffer->recordingError == VK_SUCCESS)
		{
			commandBuffer->recordingError = VK_ERROR_INITIALIZATION_FAILED;
			commandBuffer->recordingErrorOperation = operationName;
		}
	} else
	{
		gMetalStateOnlyRecordElisionTelemetry.stateOnlyElided.store(true, std::memory_order_relaxed);
	}
	return true;
#else
	(void)commandBuffer;
	(void)operationName;
	return false;
#endif
}

void ObserveMetalStateSnapshotConsumer()
{
#if LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION_COMPILED
	if (MetalStateOnlyRecordElisionSelectorRequested())
	{
		gMetalStateOnlyRecordElisionTelemetry.snapshotConsumed.store(true, std::memory_order_relaxed);
	}
#endif
}

std::shared_ptr<ReplayState> GetReplayState(VkCommandBuffer commandBuffer)
{
	if (commandBuffer == VK_NULL_HANDLE)
	{
		return {};
	}
	std::lock_guard lock(commandBuffer->mutex);
	if (!commandBuffer->backendState)
	{
		commandBuffer->backendState = std::make_shared<ReplayState>();
	}
	return std::static_pointer_cast<ReplayState>(commandBuffer->backendState);
}

VkDeviceSize BufferMetalOffset(VkBuffer buffer, VkDeviceSize offset)
{
	return buffer != VK_NULL_HANDLE ? buffer->memoryOffset + offset : 0;
}

bool BufferRangeInBounds(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
{
	return buffer != VK_NULL_HANDLE && offset <= buffer->size && size <= buffer->size - offset;
}

bool BufferIsHostVisible(VkBuffer buffer)
{
	return buffer != VK_NULL_HANDLE && buffer->memory != VK_NULL_HANDLE &&
	       (buffer->memory->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

void InvalidateShadowRangeLocked(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
{
	if (size == 0)
	{
		return;
	}
	const VkDeviceSize end = offset + size;
	buffer->cpuShadowRanges.erase(
	    std::remove_if(buffer->cpuShadowRanges.begin(), buffer->cpuShadowRanges.end(),
	                   [offset, end](const MetalBufferShadowRange& range)
	                   {
		                   const VkDeviceSize rangeEnd = range.offset + range.bytes.size();
		                   return range.offset < end && offset < rangeEnd;
	                   }),
	    buffer->cpuShadowRanges.end());
}

VkResult InvalidateGpuBufferWrite(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
{
	if (!BufferRangeInBounds(buffer, offset, size))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::lock_guard lock(buffer->mutex);
	if (BufferIsHostVisible(buffer))
	{
		buffer->hostGpuWriteSeen = true;
		buffer->cpuShadowRanges.clear();
	} else
	{
		InvalidateShadowRangeLocked(buffer, offset, size);
	}
	return VK_SUCCESS;
}

VkResult ReplaceShadowRangeLocked(VkBuffer buffer, VkDeviceSize offset, const std::vector<uint8_t>& bytes)
{
	if (bytes.empty())
	{
		return VK_SUCCESS;
	}
	if (bytes.size() > std::numeric_limits<VkDeviceSize>::max() ||
	    !BufferRangeInBounds(buffer, offset, static_cast<VkDeviceSize>(bytes.size())))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	const VkDeviceSize end = offset + bytes.size();
	std::vector<MetalBufferShadowRange> replacement;
	replacement.reserve(buffer->cpuShadowRanges.size() + 2);
	for (const MetalBufferShadowRange& range: buffer->cpuShadowRanges)
	{
		const VkDeviceSize rangeEnd = range.offset + range.bytes.size();
		if (rangeEnd <= offset || range.offset >= end)
		{
			replacement.push_back(range);
			continue;
		}
		if (range.offset < offset)
		{
			MetalBufferShadowRange left;
			left.offset = range.offset;
			left.bytes.assign(range.bytes.begin(), range.bytes.begin() + static_cast<size_t>(offset - range.offset));
			replacement.push_back(std::move(left));
		}
		if (rangeEnd > end)
		{
			MetalBufferShadowRange right;
			right.offset = end;
			right.bytes.assign(range.bytes.begin() + static_cast<size_t>(end - range.offset), range.bytes.end());
			replacement.push_back(std::move(right));
		}
	}
	replacement.push_back(MetalBufferShadowRange {offset, bytes});
	std::sort(replacement.begin(), replacement.end(), [](const MetalBufferShadowRange& left,
	                                                    const MetalBufferShadowRange& right)
	{
		return left.offset < right.offset;
	});
	buffer->cpuShadowRanges.swap(replacement);
	return VK_SUCCESS;
}

VkResult ReadBufferBytes(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, std::vector<uint8_t>* bytes)
{
	if (bytes == nullptr || size > std::numeric_limits<size_t>::max() || !BufferRangeInBounds(buffer, offset, size))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	std::lock_guard lock(buffer->mutex);
	bytes->resize(static_cast<size_t>(size));
	if (size == 0)
	{
		return VK_SUCCESS;
	}
	if (BufferIsHostVisible(buffer))
	{
		// A host-visible destination previously written by a GPU command cannot
		// be read here without a queue synchronization point. Keep this path
		// fail-closed instead of expanding stale indices.
		if (buffer->hostGpuWriteSeen || buffer->metalBuffer == nil || buffer->metalBuffer.contents == nullptr)
		{
			return VK_ERROR_FEATURE_NOT_PRESENT;
		}
		const VkDeviceSize metalOffset = BufferMetalOffset(buffer, offset);
		if (metalOffset > buffer->metalBuffer.length || size > buffer->metalBuffer.length - metalOffset)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		std::memcpy(bytes->data(), static_cast<const uint8_t*>(buffer->metalBuffer.contents) + metalOffset,
		            static_cast<size_t>(size));
		return VK_SUCCESS;
	}

	VkDeviceSize cursor = offset;
	const VkDeviceSize end = offset + size;
	for (const MetalBufferShadowRange& range: buffer->cpuShadowRanges)
	{
		const VkDeviceSize rangeEnd = range.offset + range.bytes.size();
		if (rangeEnd <= cursor)
		{
			continue;
		}
		if (range.offset > cursor)
		{
			break;
		}
		const VkDeviceSize copiedEnd = std::min(end, rangeEnd);
		const size_t copiedSize = static_cast<size_t>(copiedEnd - cursor);
		std::memcpy(bytes->data() + static_cast<size_t>(cursor - offset),
		            range.bytes.data() + static_cast<size_t>(cursor - range.offset), copiedSize);
		cursor = copiedEnd;
		if (cursor == end)
		{
			return VK_SUCCESS;
		}
	}
	return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult MirrorBufferCopy(VkBuffer source, VkBuffer destination, const VkBufferCopy& copy)
{
	std::vector<uint8_t> bytes;
	const VkResult readResult = ReadBufferBytes(source, copy.srcOffset, copy.size, &bytes);
	std::lock_guard destinationLock(destination->mutex);
	if (BufferIsHostVisible(destination))
	{
		destination->hostGpuWriteSeen = true;
		destination->cpuShadowRanges.clear();
		return VK_SUCCESS;
	}
	if (readResult != VK_SUCCESS)
	{
		InvalidateShadowRangeLocked(destination, copy.dstOffset, copy.size);
		return VK_SUCCESS;
	}
	return ReplaceShadowRangeLocked(destination, copy.dstOffset, bytes);
}

VkResult CreateNonIndexedFanBuffer(VkDevice device, uint32_t vertexCount, uint32_t firstVertex,
	                               id<MTLBuffer>* indexBuffer, NSUInteger* indexCount)
{
	if (device == VK_NULL_HANDLE || indexBuffer == nullptr || indexCount == nullptr)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*indexBuffer = nil;
	*indexCount = 0;
	if (vertexCount < 3)
	{
		return VK_SUCCESS;
	}
	if (vertexCount - 1 > std::numeric_limits<uint32_t>::max() - firstVertex)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	const uint64_t expandedCount = static_cast<uint64_t>(vertexCount - 2) * 3;
	if (expandedCount > std::numeric_limits<NSUInteger>::max() ||
	    expandedCount > std::numeric_limits<NSUInteger>::max() / sizeof(uint32_t))
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	const NSUInteger length = static_cast<NSUInteger>(expandedCount) * sizeof(uint32_t);
	id<MTLBuffer> expanded = [device->metalDevice newBufferWithLength:length options:MTLResourceStorageModeShared];
	if (expanded == nil || expanded.contents == nullptr)
	{
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	auto* output = static_cast<uint32_t*>(expanded.contents);
	for (uint32_t triangle = 0; triangle < vertexCount - 2; ++triangle)
	{
		output[triangle * 3] = firstVertex;
		output[triangle * 3 + 1] = firstVertex + triangle + 1;
		output[triangle * 3 + 2] = firstVertex + triangle + 2;
	}
	expanded.label = @"Lumen triangle-fan expansion (non-indexed)";
	*indexBuffer = expanded;
	*indexCount = static_cast<NSUInteger>(expandedCount);
	return VK_SUCCESS;
}

VkResult CreateIndexedFanBuffer(VkDevice device, VkBuffer sourceBuffer, VkDeviceSize sourceOffset,
	                            uint32_t sourceIndexCount, VkIndexType indexType,
	                            id<MTLBuffer>* indexBuffer, NSUInteger* indexCount)
{
	if (device == VK_NULL_HANDLE || sourceBuffer == VK_NULL_HANDLE || indexBuffer == nullptr || indexCount == nullptr ||
	    (indexType != VK_INDEX_TYPE_UINT16 && indexType != VK_INDEX_TYPE_UINT32))
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	*indexBuffer = nil;
	*indexCount = 0;
	if (sourceIndexCount < 3)
	{
		return VK_SUCCESS;
	}
	const uint32_t indexSize = IndexTypeSize(indexType);
	const uint64_t sourceSize = static_cast<uint64_t>(sourceIndexCount) * indexSize;
	const uint64_t expandedCount = static_cast<uint64_t>(sourceIndexCount - 2) * 3;
	if (sourceSize > std::numeric_limits<VkDeviceSize>::max() ||
	    expandedCount > std::numeric_limits<NSUInteger>::max() ||
	    expandedCount > std::numeric_limits<NSUInteger>::max() / indexSize)
	{
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}
	std::vector<uint8_t> source;
	const VkResult readResult = ReadBufferBytes(sourceBuffer, sourceOffset, sourceSize, &source);
	if (readResult != VK_SUCCESS)
	{
		return readResult;
	}
	const NSUInteger length = static_cast<NSUInteger>(expandedCount) * indexSize;
	id<MTLBuffer> expanded = [device->metalDevice newBufferWithLength:length options:MTLResourceStorageModeShared];
	if (expanded == nil || expanded.contents == nullptr)
	{
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
	auto copyIndex = [indexSize, &source, expanded](uint64_t destinationIndex, uint32_t sourceIndex)
	{
		std::memcpy(static_cast<uint8_t*>(expanded.contents) + destinationIndex * indexSize,
		            source.data() + static_cast<size_t>(sourceIndex) * indexSize, indexSize);
	};
	for (uint32_t triangle = 0; triangle < sourceIndexCount - 2; ++triangle)
	{
		copyIndex(static_cast<uint64_t>(triangle) * 3, 0);
		copyIndex(static_cast<uint64_t>(triangle) * 3 + 1, triangle + 1);
		copyIndex(static_cast<uint64_t>(triangle) * 3 + 2, triangle + 2);
	}
	expanded.label = @"Lumen triangle-fan expansion (indexed)";
	*indexBuffer = expanded;
	*indexCount = static_cast<NSUInteger>(expandedCount);
	return VK_SUCCESS;
}

bool DescriptorUsesBuffer(VkDescriptorType type)
{
	return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
	       type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

bool DescriptorUsesTexture(VkDescriptorType type)
{
	return type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
	       type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

bool DescriptorUsesSampler(VkDescriptorType type)
{
	return type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

bool MetalIndexInRange(uint32_t baseIndex, uint32_t arrayIndex, uint32_t maximumIndex)
{
	return baseIndex != UINT32_MAX && arrayIndex <= maximumIndex && baseIndex <= maximumIndex - arrayIndex;
}

spv::ExecutionModel ToExecutionModel(VkShaderStageFlagBits stage)
{
	switch (stage)
	{
		case VK_SHADER_STAGE_VERTEX_BIT: return spv::ExecutionModelVertex;
		case VK_SHADER_STAGE_FRAGMENT_BIT: return spv::ExecutionModelFragment;
		case VK_SHADER_STAGE_COMPUTE_BIT: return spv::ExecutionModelGLCompute;
		default: return spv::ExecutionModelMax;
	}
}

void PrintMetalError(const char* operation, NSError* error)
{
	const char* description = error.localizedDescription.UTF8String;
	std::fprintf(stderr, "[LUMEN_METAL] %s failed: %s\n", operation,
	             description != nullptr ? description : "unknown Metal error");
}

} // namespace

struct VkShaderModule_T
{
	VkDevice device = VK_NULL_HANDLE;
	std::vector<uint32_t> words;
	std::atomic<bool> alive {true};
};

struct VkDescriptorSetLayout_T
{
	VkDevice device = VK_NULL_HANDLE;
	std::vector<VkDescriptorSetLayoutBinding> bindings;
	std::map<uint32_t, std::vector<VkSampler>> immutableSamplers;
	std::atomic<bool> alive {true};
};

struct VkDescriptorSet_T;

struct VkDescriptorPool_T
{
	VkDevice device = VK_NULL_HANDLE;
	uint32_t maximumSets = 0;
	VkDescriptorPoolCreateFlags flags = 0;
	std::mutex mutex;
	std::unordered_set<VkDescriptorSet> sets;
	std::atomic<bool> alive {true};
};

struct VkDescriptorSet_T
{
	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkDescriptorSetLayout layout = VK_NULL_HANDLE;
	std::mutex mutex;
	std::unordered_map<uint32_t, std::vector<DescriptorValue>> values;
	std::atomic<bool> alive {true};
};

struct VkPipelineLayout_T
{
	VkDevice device = VK_NULL_HANDLE;
	std::vector<VkDescriptorSetLayout> setLayouts;
	std::vector<VkPushConstantRange> pushConstantRanges;
	std::atomic<bool> alive {true};
};

struct VkRenderPass_T
{
	VkDevice device = VK_NULL_HANDLE;
	std::vector<VkAttachmentDescription> attachments;
	VkAttachmentReference colorAttachment {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
	VkAttachmentReference depthAttachment {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
	bool hasDepthAttachment = false;
	bool passReuseEligible = false;
	std::atomic<bool> alive {true};
};

struct VkFramebuffer_T
{
	VkDevice device = VK_NULL_HANDLE;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	std::vector<VkImageView> attachments;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t layers = 0;
	bool passReuseEligible = false;
	std::atomic<bool> alive {true};
};

struct VkPipeline_T
{
	VkDevice device = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_MAX_ENUM;
	__strong id<MTLRenderPipelineState> renderState = nil;
	__strong id<MTLRenderPipelineState> renderStateNoColor = nil;
	__strong MTLRenderPipelineDescriptor* renderDescriptor = nil;
	__strong id<MTLFunction> vertexFunctionFlipped = nil;
	std::unordered_set<VkDynamicState> dynamicStates;
	struct RenderVariant { __strong id<MTLRenderPipelineState> state = nil; };
	std::map<std::array<uint64_t, 17>, RenderVariant> renderVariants;
	std::mutex renderVariantMutex;
	__strong id<MTLComputePipelineState> computeState = nil;
	VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	MTLCullMode cullMode = MTLCullModeNone;
	MTLWinding winding = MTLWindingCounterClockwise;
	bool depthClamp = false;
	float depthBiasConstantFactor = 0.0f;
	float depthBiasSlopeFactor = 0.0f;
	float depthBiasClamp = 0.0f;
	VkViewport viewport {};
	bool viewportDynamic = false;
	VkRect2D scissor {};
	bool scissorDynamic = false;
	std::array<float, 4> blendConstants {};
	VkPipelineDepthStencilStateCreateInfo depthStencil {};
	bool hasDepthStencil = false;
#if LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED
	bool stencilCompareMaskDynamic = false;
	bool stencilWriteMaskDynamic = false;
	bool stencilReferenceDynamic = false;
#endif
	std::vector<StageBinding> stageBindings;
	uint32_t vertexPushConstantSize = 0;
	uint32_t fragmentPushConstantSize = 0;
	uint32_t computePushConstantSize = 0;
	bool passReuseResourcesReadOnly = false;
	uint32_t vertexBufferBase = 0;
	std::array<bool, 16> vertexBindingUsed {};
	MTLSize computeThreads {1, 1, 1};
	std::atomic<bool> alive {true};
};

namespace {

bool RenderPassAllowsReuse(const VkRenderPassCreateInfo& info)
{
	const auto& subpass = info.pSubpasses[0];
	if (info.pNext != nullptr || info.flags != 0 || subpass.flags != 0 ||
	    subpass.preserveAttachmentCount != 0 || subpass.inputAttachmentCount != 0 ||
	    subpass.pResolveAttachments != nullptr || info.dependencyCount > 1)
		return false;
	if (info.dependencyCount == 1)
	{
		if (info.pDependencies == nullptr) return false;
		const auto& dependency = info.pDependencies[0];
		constexpr VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
		    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		constexpr VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		constexpr VkAccessFlags reads = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		// Only the runtime's attachment-local external dependency is subsumed
		// by ordered draws in one encoder. Shader/transfer dependencies are not.
		if (dependency.srcSubpass != VK_SUBPASS_EXTERNAL || dependency.dstSubpass != 0 ||
		    dependency.srcStageMask != stages || dependency.dstStageMask != stages ||
		    dependency.srcAccessMask != writes || dependency.dstAccessMask != (reads | writes) ||
		    dependency.dependencyFlags != VK_DEPENDENCY_BY_REGION_BIT)
			return false;
	}
	bool hasAttachment = false;
	const auto accepts = [&](const VkAttachmentReference& reference, bool depth) {
		if (reference.attachment == VK_ATTACHMENT_UNUSED) return true;
		if (reference.attachment >= info.attachmentCount) return false;
		hasAttachment = true;
		const auto& attachment = info.pAttachments[reference.attachment];
		const VkImageLayout layout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
		                                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		if (attachment.flags != 0 || attachment.samples != VK_SAMPLE_COUNT_1_BIT ||
		    reference.layout != layout || attachment.initialLayout != layout || attachment.finalLayout != layout)
			return false;
		if ((!depth || IsDepthFormat(attachment.format)) &&
		    (attachment.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD || attachment.storeOp != VK_ATTACHMENT_STORE_OP_STORE))
			return false;
		return !depth || !HasStencil(attachment.format) ||
		       (attachment.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD && attachment.stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE);
	};
	if (subpass.colorAttachmentCount != 0 &&
	    (subpass.pColorAttachments == nullptr || !accepts(subpass.pColorAttachments[0], false))) return false;
	if (subpass.pDepthStencilAttachment != nullptr && !accepts(*subpass.pDepthStencilAttachment, true)) return false;
	return hasAttachment;
}

bool ShaderResourcesAllowPassReuse(const VkPipelineShaderStageCreateInfo& stage)
{
	// The local Gen4 lowerer explicitly decorates read-only storage buffers.
	// Unknown/member-only writability and all storage images stay conservative.
	spirv_cross::CompilerMSL compiler(stage.module->words);
	compiler.set_entry_point(stage.pName, ToExecutionModel(static_cast<VkShaderStageFlagBits>(stage.stage)));
	const auto resources = compiler.get_shader_resources();
	if (!resources.storage_images.empty() || !resources.atomic_counters.empty() || !resources.subpass_inputs.empty()) return false;
	for (const auto& buffer: resources.storage_buffers)
		if (!compiler.has_decoration(buffer.id, spv::DecorationNonWritable)) return false;
	return true;
}

bool ResourceAliasesRenderAttachment(VkImageView attachment, VkImageView sampled, VkBuffer buffer)
{
	if (attachment == VK_NULL_HANDLE || attachment->image == VK_NULL_HANDLE) return false;
	const auto image = attachment->image;
	if (buffer != VK_NULL_HANDLE && image->memory != VK_NULL_HANDLE && image->memory == buffer->memory) return true;
	if (sampled == VK_NULL_HANDLE || sampled->image == VK_NULL_HANDLE) return false;
	return image == sampled->image ||
	       (image->memory != VK_NULL_HANDLE && image->memory == sampled->image->memory) ||
	       (attachment->metalTexture != nil && attachment->metalTexture == sampled->metalTexture) ||
	       (image->metalTexture != nil && image->metalTexture == sampled->image->metalTexture);
}

bool DrawResourcesAllowPassReuse(const Lumen::MetalVulkan::EncodingContext& context,
	                            const std::shared_ptr<ReplayState>& state)
{
	if (!state->graphicsPipeline->passReuseResourcesReadOnly) return false;
	const auto aliasesAttachment = [&](VkImageView image, VkBuffer buffer) {
		for (const auto attachment: context.retainedFramebuffer->attachments)
			if (ResourceAliasesRenderAttachment(attachment, image, buffer)) return true;
		return false;
	};
	for (size_t index = 0; index < state->vertexBuffers.size(); ++index)
		if (state->graphicsPipeline->vertexBindingUsed[index] && aliasesAttachment(VK_NULL_HANDLE, state->vertexBuffers[index])) return false;
	if (aliasesAttachment(VK_NULL_HANDLE, state->indexBuffer)) return false;
	for (const auto& mapped: state->graphicsPipeline->stageBindings)
	{
		if (!DescriptorUsesTexture(mapped.type) && !DescriptorUsesBuffer(mapped.type)) continue;
		if (mapped.set >= state->graphicsSets.size() || state->graphicsSets[mapped.set] == VK_NULL_HANDLE) return false;
		VkDescriptorSet set = state->graphicsSets[mapped.set];
		std::lock_guard lock(set->mutex);
		const auto found = set->values.find(mapped.binding);
		if (found == set->values.end() || found->second.size() < mapped.count) return false;
		for (uint32_t index = 0; index < mapped.count; ++index)
		{
			const auto& value = found->second[index];
			if (DescriptorUsesTexture(mapped.type) && (value.imageView == VK_NULL_HANDLE || value.imageView->image == VK_NULL_HANDLE)) return false;
			if (DescriptorUsesBuffer(mapped.type) && value.buffer == VK_NULL_HANDLE) return false;
			// Whole-allocation rejection also covers different images/views and
			// read-only buffers aliasing an attachment, irrespective of offsets.
			if (aliasesAttachment(value.imageView, value.buffer)) return false;
		}
	}
	return true;
}

VkResult PrepareReusablePassDraw(Lumen::MetalVulkan::EncodingContext& context, const std::shared_ptr<ReplayState>& state)
{
	if (!context.renderPassReusable) return VK_SUCCESS;
	if (!DrawResourcesAllowPassReuse(context, state))
	{
		++context.resourceRejectedPasses;
		context.renderPassReusable = false;
		if (context.renderPassMergePending || context.renderPassContainsMergedDraws)
		{
			// Descriptor/pipeline binds can follow BeginRenderPass. Restore the
			// LOAD boundary before an unsafe draw instead of trusting Begin state.
			Lumen::MetalVulkan::EndRenderEncoding(context);
			const auto result = Lumen::MetalVulkan::BeginRenderEncoding(context, context.retainedRenderDescriptor);
			if (result != VK_SUCCESS) return result;
			++context.physicalRenderEncoders;
			if (context.renderPassContainsMergedDraws) --context.mergedRenderPasses;
			if (context.renderPassMergedAfterMarker) --context.mergedAfterCompletionMarker;
		}
		context.renderPassMergePending = false;
		context.renderPassContainsMergedDraws = false;
		context.renderPassMergeAfterMarkerPending = false;
		context.renderPassMergedAfterMarker = false;
		context.retainedCompletionMarker = false;
		return VK_SUCCESS;
	}
	if (context.renderPassMergePending)
	{
		++context.mergedRenderPasses;
		if (context.renderPassMergeAfterMarkerPending) ++context.mergedAfterCompletionMarker;
		context.renderPassMergedAfterMarker = context.renderPassMergeAfterMarkerPending;
		context.renderPassMergeAfterMarkerPending = false;
		context.renderPassMergePending = false;
		context.renderPassContainsMergedDraws = true;
	}
	return VK_SUCCESS;
}

struct ResolvedStencilState
{
	uint32_t frontCompareMask = UINT32_MAX;
	uint32_t backCompareMask = UINT32_MAX;
	uint32_t frontWriteMask = UINT32_MAX;
	uint32_t backWriteMask = UINT32_MAX;
	uint32_t frontReference = 0;
	uint32_t backReference = 0;
};

ResolvedStencilState PreserveLegacyStencilState(
	const DynamicStencilState& dynamic)
{
	ResolvedStencilState resolved;
	resolved.frontCompareMask = dynamic.frontCompareMask;
	resolved.backCompareMask = dynamic.backCompareMask;
	resolved.frontWriteMask = dynamic.frontWriteMask;
	resolved.backWriteMask = dynamic.backWriteMask;
	resolved.frontReference = dynamic.frontReference;
	resolved.backReference = dynamic.backReference;
	return resolved;
}

#if LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED
ResolvedStencilState ResolveStencilState(VkPipeline pipeline,
	                                      const DynamicStencilState& dynamic)
{
	const auto& source = pipeline->depthStencil;
	ResolvedStencilState resolved;
	resolved.frontCompareMask = pipeline->stencilCompareMaskDynamic
	                              ? dynamic.frontCompareMask
	                              : source.front.compareMask;
	resolved.backCompareMask = pipeline->stencilCompareMaskDynamic
	                             ? dynamic.backCompareMask
	                             : source.back.compareMask;
	resolved.frontWriteMask = pipeline->stencilWriteMaskDynamic
	                            ? dynamic.frontWriteMask
	                            : source.front.writeMask;
	resolved.backWriteMask = pipeline->stencilWriteMaskDynamic
	                           ? dynamic.backWriteMask
	                           : source.back.writeMask;
	resolved.frontReference = pipeline->stencilReferenceDynamic
	                            ? dynamic.frontReference
	                            : source.front.reference;
	resolved.backReference = pipeline->stencilReferenceDynamic
	                           ? dynamic.backReference
	                           : source.back.reference;
	return resolved;
}

struct MetalStencilFaceKey
{
	uint32_t compareFunction = 0;
	uint32_t stencilFailureOperation = 0;
	uint32_t depthFailureOperation = 0;
	uint32_t depthStencilPassOperation = 0;
	uint32_t readMask = UINT32_MAX;
	uint32_t writeMask = UINT32_MAX;

	bool operator==(const MetalStencilFaceKey& other) const
	{
		return compareFunction == other.compareFunction &&
		       stencilFailureOperation == other.stencilFailureOperation &&
		       depthFailureOperation == other.depthFailureOperation &&
		       depthStencilPassOperation == other.depthStencilPassOperation &&
		       readMask == other.readMask && writeMask == other.writeMask;
	}
};

struct MetalDepthStencilStateKey
{
	uint64_t digest = 0;
	uint32_t depthCompareFunction = 0;
	bool depthWriteEnabled = false;
	bool stencilEnabled = false;
	MetalStencilFaceKey front;
	MetalStencilFaceKey back;

	bool operator==(const MetalDepthStencilStateKey& other) const
	{
		// The digest only selects a bucket. Every Metal descriptor input is
		// compared so a hash collision cannot alias two device states.
		return digest == other.digest &&
		       depthCompareFunction == other.depthCompareFunction &&
		       depthWriteEnabled == other.depthWriteEnabled &&
		       stencilEnabled == other.stencilEnabled && front == other.front &&
		       back == other.back;
	}
};

struct MetalDepthStencilStateKeyHash
{
	size_t operator()(const MetalDepthStencilStateKey& key) const
	{
		return static_cast<size_t>(key.digest);
	}
};

struct CachedMetalDepthStencilState
{
	__strong id<MTLDepthStencilState> state = nil;
};

struct MetalDepthStencilStateCache
{
	std::mutex mutex;
	std::unordered_map<MetalDepthStencilStateKey,
	                   CachedMetalDepthStencilState,
	                   MetalDepthStencilStateKeyHash>
	    entries;
};

struct MetalDepthStencilStateCacheTelemetry
{
	std::atomic<uint64_t> lookup {0};
	std::atomic<uint64_t> hit {0};
	std::atomic<uint64_t> miss {0};
	std::atomic<uint64_t> create {0};
	std::atomic<uint64_t> error {0};
	std::once_flag finalizeOnce;
	bool finalizeResult = true;
};

MetalDepthStencilStateCacheTelemetry gMetalDepthStencilStateCacheTelemetry;

bool MetalDepthStencilStateCacheSelectorRequested()
{
	// Cache identity and ownership are a process contract. Latch only the
	// exact value "1" before the first lookup; every other value stays inert.
	static const bool requested = []()
	{
		const char* value =
		    std::getenv("LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return requested;
}

std::shared_ptr<MetalDepthStencilStateCache> GetMetalDepthStencilStateCache(
	VkDevice device)
{
	if (device == VK_NULL_HANDLE) return {};
	std::lock_guard lock(device->mutex);
	if (!device->depthStencilStateCache)
	{
		device->depthStencilStateCache =
		    std::make_shared<MetalDepthStencilStateCache>();
	}
	return std::static_pointer_cast<MetalDepthStencilStateCache>(
	    device->depthStencilStateCache);
}

void HashDepthStencilValue(uint64_t* digest, uint32_t value)
{
	for (unsigned shift = 0; shift != 32; shift += 8)
	{
		*digest ^= static_cast<uint8_t>(value >> shift);
		*digest *= UINT64_C(1099511628211);
	}
}

MetalStencilFaceKey BuildMetalStencilFaceKey(const VkStencilOpState& source,
	                                          uint32_t readMask,
	                                          uint32_t writeMask)
{
	MetalStencilFaceKey key;
	key.compareFunction = static_cast<uint32_t>(
	    ToMTLCompareFunction(source.compareOp));
	key.stencilFailureOperation = static_cast<uint32_t>(
	    ToMTLStencilOperation(source.failOp));
	key.depthFailureOperation = static_cast<uint32_t>(
	    ToMTLStencilOperation(source.depthFailOp));
	key.depthStencilPassOperation = static_cast<uint32_t>(
	    ToMTLStencilOperation(source.passOp));
	key.readMask = readMask;
	key.writeMask = writeMask;
	return key;
}

MetalDepthStencilStateKey BuildMetalDepthStencilStateKey(
	const VkPipelineDepthStencilStateCreateInfo& source,
	const ResolvedStencilState& resolved)
{
	// References are encoder-dynamic in Metal, so they are intentionally absent
	// from descriptor identity. Compare/write masks must remain in the key.
	MetalDepthStencilStateKey key;
	key.depthCompareFunction = static_cast<uint32_t>(
	    source.depthTestEnable ? ToMTLCompareFunction(source.depthCompareOp)
	                           : MTLCompareFunctionAlways);
	key.depthWriteEnabled = source.depthWriteEnable == VK_TRUE;
	key.stencilEnabled = source.stencilTestEnable == VK_TRUE;
	if (key.stencilEnabled)
	{
		key.front = BuildMetalStencilFaceKey(
		    source.front, resolved.frontCompareMask,
		    resolved.frontWriteMask);
		key.back = BuildMetalStencilFaceKey(
		    source.back, resolved.backCompareMask,
		    resolved.backWriteMask);
	}

	uint64_t digest = UINT64_C(14695981039346656037);
	HashDepthStencilValue(&digest, key.depthCompareFunction);
	HashDepthStencilValue(&digest, key.depthWriteEnabled ? 1u : 0u);
	HashDepthStencilValue(&digest, key.stencilEnabled ? 1u : 0u);
	const auto hashFace = [&digest](const MetalStencilFaceKey& face)
	{
		HashDepthStencilValue(&digest, face.compareFunction);
		HashDepthStencilValue(&digest, face.stencilFailureOperation);
		HashDepthStencilValue(&digest, face.depthFailureOperation);
		HashDepthStencilValue(&digest, face.depthStencilPassOperation);
		HashDepthStencilValue(&digest, face.readMask);
		HashDepthStencilValue(&digest, face.writeMask);
	};
	hashFace(key.front);
	hashFace(key.back);
	key.digest = digest;
	return key;
}
#endif

#if LUMEN_A64_METAL_SHADER_FUNCTION_CACHE_COMPILED
struct MetalShaderFunctionKey
{
	uint64_t digest = 0;
	std::vector<uint32_t> spirv;
	std::string entry;
	std::vector<StageBinding> bindings;
	VkShaderStageFlagBits stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
	uint32_t vertexInputFourComponentMask = 0;
	bool flipVertexY = false;
	bool safeMath = false;
	bool iosPlatform = false;

	bool operator==(const MetalShaderFunctionKey& other) const;
};

struct MetalShaderFunctionKeyHash
{
	size_t operator()(const MetalShaderFunctionKey& key) const
	{
		return static_cast<size_t>(key.digest);
	}
};

struct CachedMetalShaderFunction
{
	__strong id<MTLLibrary> library = nil;
	__strong id<MTLFunction> function = nil;
	MTLSize localSize {1, 1, 1};
	uint32_t pushConstantSize = 0;
};

struct MetalShaderFunctionCache
{
	std::mutex mutex;
	std::unordered_map<MetalShaderFunctionKey, CachedMetalShaderFunction,
	                   MetalShaderFunctionKeyHash>
	    entries;
};

struct MetalShaderFunctionCacheTelemetry
{
	std::atomic<bool> lookup {false};
	std::atomic<bool> hit {false};
	std::atomic<bool> compile {false};
	std::once_flag finalizeOnce;
	bool finalizeResult = true;
};

MetalShaderFunctionCacheTelemetry gMetalShaderFunctionCacheTelemetry;

bool StageBindingEqual(const StageBinding& left, const StageBinding& right)
{
	return left.set == right.set && left.binding == right.binding &&
	       left.count == right.count &&
	       left.metalBufferIndex == right.metalBufferIndex &&
	       left.metalTextureIndex == right.metalTextureIndex &&
	       left.metalSamplerIndex == right.metalSamplerIndex &&
	       left.type == right.type && left.stage == right.stage;
}

bool StageBindingsEqual(const std::vector<StageBinding>& left,
	                    const std::vector<StageBinding>& right)
{
	if (left.size() != right.size()) return false;
	for (size_t index = 0; index < left.size(); ++index)
	{
		if (!StageBindingEqual(left[index], right[index])) return false;
	}
	return true;
}

bool MetalShaderFunctionKey::operator==(
	const MetalShaderFunctionKey& other) const
{
	// digest is only a bucket accelerator. Full source and compile-contract
	// equality remains mandatory so a hash collision can never alias shaders.
	return digest == other.digest && stage == other.stage &&
	       vertexInputFourComponentMask == other.vertexInputFourComponentMask &&
	       flipVertexY == other.flipVertexY && safeMath == other.safeMath &&
	       iosPlatform == other.iosPlatform && entry == other.entry &&
	       spirv == other.spirv && StageBindingsEqual(bindings, other.bindings);
}

void HashMetalShaderBytes(uint64_t* digest, const void* bytes, size_t size)
{
	const auto* source = static_cast<const uint8_t*>(bytes);
	for (size_t index = 0; index < size; ++index)
	{
		*digest ^= source[index];
		*digest *= UINT64_C(1099511628211);
	}
}

template <typename Value>
void HashMetalShaderValue(uint64_t* digest, const Value& value)
{
	HashMetalShaderBytes(digest, &value, sizeof(value));
}

bool MetalShaderFunctionCacheSelectorRequested()
{
	// Cache ownership and function identity are process contracts. Freeze the
	// exact-one selector before the first lookup; malformed values stay inert.
	static const bool requested = []()
	{
		const char* value =
		    std::getenv("LUMEN_A64_METAL_SHADER_FUNCTION_CACHE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return requested;
}

std::shared_ptr<MetalShaderFunctionCache> GetMetalShaderFunctionCache(
	VkDevice device)
{
	if (device == VK_NULL_HANDLE) return {};
	std::lock_guard lock(device->mutex);
	if (!device->shaderFunctionCache)
	{
		device->shaderFunctionCache =
		    std::make_shared<MetalShaderFunctionCache>();
	}
	return std::static_pointer_cast<MetalShaderFunctionCache>(
	    device->shaderFunctionCache);
}

bool MetalSafeMathAvailable()
{
	if (@available(macOS 15.0, iOS 18.0, *)) return true;
	return false;
}

MetalShaderFunctionKey BuildMetalShaderFunctionKey(
	VkShaderModule module, const VkPipelineShaderStageCreateInfo& stageInfo,
	const std::vector<StageBinding>& bindings,
	uint32_t vertexInputFourComponentMask, bool flipVertexY, bool safeMath)
{
	MetalShaderFunctionKey key;
	key.spirv = module->words;
	key.entry = stageInfo.pName;
	key.bindings = bindings;
	key.stage = static_cast<VkShaderStageFlagBits>(stageInfo.stage);
	key.vertexInputFourComponentMask = vertexInputFourComponentMask;
	key.flipVertexY = flipVertexY;
	key.safeMath = safeMath;
	key.iosPlatform = TARGET_OS_IPHONE != 0;

	uint64_t digest = UINT64_C(14695981039346656037);
	const size_t spirvSize = key.spirv.size();
	HashMetalShaderValue(&digest, spirvSize);
	HashMetalShaderBytes(&digest, key.spirv.data(),
	                     key.spirv.size() * sizeof(key.spirv[0]));
	const size_t entrySize = key.entry.size();
	HashMetalShaderValue(&digest, entrySize);
	HashMetalShaderBytes(&digest, key.entry.data(), key.entry.size());
	const size_t bindingCount = key.bindings.size();
	HashMetalShaderValue(&digest, bindingCount);
	for (const auto& binding: key.bindings)
	{
		HashMetalShaderValue(&digest, binding.set);
		HashMetalShaderValue(&digest, binding.binding);
		HashMetalShaderValue(&digest, binding.count);
		HashMetalShaderValue(&digest, binding.metalBufferIndex);
		HashMetalShaderValue(&digest, binding.metalTextureIndex);
		HashMetalShaderValue(&digest, binding.metalSamplerIndex);
		HashMetalShaderValue(&digest, binding.type);
		HashMetalShaderValue(&digest, binding.stage);
	}
	HashMetalShaderValue(&digest, key.stage);
	HashMetalShaderValue(&digest, key.vertexInputFourComponentMask);
	HashMetalShaderValue(&digest, key.flipVertexY);
	HashMetalShaderValue(&digest, key.safeMath);
	HashMetalShaderValue(&digest, key.iosPlatform);
	key.digest = digest;
	return key;
}
#endif

std::vector<StageBinding> BuildStageBindings(VkPipelineLayout layout, VkShaderStageFlagBits stage)
{
	std::vector<StageBinding> result;
	if (layout == VK_NULL_HANDLE)
	{
		return result;
	}
	uint32_t bufferIndex = 0;
	uint32_t textureIndex = 0;
	uint32_t samplerIndex = 0;
	for (uint32_t setIndex = 0; setIndex < layout->setLayouts.size(); ++setIndex)
	{
		VkDescriptorSetLayout setLayout = layout->setLayouts[setIndex];
		if (setLayout == VK_NULL_HANDLE)
		{
			continue;
		}
		for (const auto& binding: setLayout->bindings)
		{
			if ((binding.stageFlags & stage) == 0)
			{
				continue;
			}
			StageBinding mapped;
			mapped.set = setIndex;
			mapped.binding = binding.binding;
			mapped.count = binding.descriptorCount;
			mapped.type = binding.descriptorType;
			mapped.stage = stage;
			const bool usesBuffer = DescriptorUsesBuffer(binding.descriptorType);
			const bool usesTexture = DescriptorUsesTexture(binding.descriptorType);
			const bool usesSampler = DescriptorUsesSampler(binding.descriptorType);
			if (!usesBuffer && !usesTexture && !usesSampler)
			{
				continue;
			}
			if (usesBuffer)
			{
				mapped.metalBufferIndex = bufferIndex;
				bufferIndex += binding.descriptorCount;
			}
			if (usesTexture)
			{
				mapped.metalTextureIndex = textureIndex;
				textureIndex += binding.descriptorCount;
			}
			if (usesSampler)
			{
				mapped.metalSamplerIndex = samplerIndex;
				samplerIndex += binding.descriptorCount;
			}
			result.push_back(mapped);
		}
	}
	return result;
}

bool MetalBindingIndicesInRange(const std::vector<StageBinding>& bindings)
{
	for (const auto& binding: bindings)
	{
		if (binding.count == 0)
		{
			return false;
		}
		const uint32_t lastArrayIndex = binding.count - 1;
		if ((DescriptorUsesBuffer(binding.type) &&
		     !MetalIndexInRange(binding.metalBufferIndex, lastArrayIndex, kPushConstantBufferIndex - 1)) ||
		    (DescriptorUsesTexture(binding.type) &&
		     !MetalIndexInRange(binding.metalTextureIndex, lastArrayIndex, kMaximumMetalTextureIndex)) ||
		    (DescriptorUsesSampler(binding.type) &&
		     !MetalIndexInRange(binding.metalSamplerIndex, lastArrayIndex, kMaximumMetalSamplerIndex)))
		{
			return false;
		}
	}
	return true;
}

uint32_t MetalBufferCount(const std::vector<StageBinding>& bindings)
{
	uint32_t count = 0;
	for (const auto& binding: bindings)
	{
		if (DescriptorUsesBuffer(binding.type))
		{
			count = std::max(count, binding.metalBufferIndex + binding.count);
		}
	}
	return count;
}

VkResult CompileMetalFunction(VkDevice device, VkShaderModule module, VkPipelineLayout layout,
	                          const VkPipelineShaderStageCreateInfo& stageInfo,
	                          id<MTLFunction>* function, std::vector<StageBinding>* bindings,
	                          MTLSize* localSize, uint32_t* pushConstantSize, uint32_t vertexInputFourComponentMask,
	                          bool flipVertexY)
{
	if (device == VK_NULL_HANDLE || module == VK_NULL_HANDLE || function == nullptr || bindings == nullptr ||
	    !module->alive.load(std::memory_order_acquire) || stageInfo.pName == nullptr)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	const auto stage = static_cast<VkShaderStageFlagBits>(stageInfo.stage);
	const spv::ExecutionModel model = ToExecutionModel(stage);
	if (model == spv::ExecutionModelMax)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	*bindings = BuildStageBindings(layout, stage);
	if (!MetalBindingIndicesInRange(*bindings))
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

#if LUMEN_A64_METAL_SHADER_FUNCTION_CACHE_COMPILED
	const bool cacheRequested = MetalShaderFunctionCacheSelectorRequested();
	const bool safeMath = cacheRequested && MetalSafeMathAvailable();
	std::shared_ptr<MetalShaderFunctionCache> cache;
	MetalShaderFunctionKey cacheKey;
	if (cacheRequested)
	{
		gMetalShaderFunctionCacheTelemetry.lookup.store(
		    true, std::memory_order_relaxed);
		cache = GetMetalShaderFunctionCache(device);
		cacheKey = BuildMetalShaderFunctionKey(
		    module, stageInfo, *bindings, vertexInputFourComponentMask,
		    stage == VK_SHADER_STAGE_VERTEX_BIT && flipVertexY, safeMath);
		std::lock_guard cacheLock(cache->mutex);
		const auto found = cache->entries.find(cacheKey);
		if (found != cache->entries.end())
		{
			gMetalShaderFunctionCacheTelemetry.hit.store(
			    true, std::memory_order_relaxed);
			*function = found->second.function;
			if (localSize != nullptr) *localSize = found->second.localSize;
			*pushConstantSize = found->second.pushConstantSize;
			return VK_SUCCESS;
		}
	}
#endif

	MetalSpirvCompiler compiler(module->words);
	compiler.set_entry_point(stageInfo.pName, model);
	uint32_t compiledPushConstantSize = 0;
	for (const auto& resource: compiler.get_shader_resources().push_constant_buffers)
	{
		const size_t size = compiler.get_declared_struct_size(compiler.get_type(resource.base_type_id));
		if (size > 4096) return VK_ERROR_FEATURE_NOT_PRESENT;
		compiledPushConstantSize = std::max(compiledPushConstantSize, static_cast<uint32_t>(size));
	}
	auto commonOptions = compiler.get_common_options();
	commonOptions.vertex.flip_vert_y = stage == VK_SHADER_STAGE_VERTEX_BIT && flipVertexY;
	compiler.set_common_options(commonOptions);
	auto mslOptions = compiler.get_msl_options();
	mslOptions.platform = TARGET_OS_IPHONE ? spirv_cross::CompilerMSL::Options::iOS
	                                      : spirv_cross::CompilerMSL::Options::macOS;
	mslOptions.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 3);
	mslOptions.enable_decoration_binding = false;
	mslOptions.force_native_arrays = true;
	compiler.set_msl_options(mslOptions);
	if (stage == VK_SHADER_STAGE_VERTEX_BIT)
	{
		// Metal Char4Normalized must enter the stage as float4. SPIRV-Cross
		// keeps the Vulkan/SPIR-V input at its logical width and narrows XYZ.
		for (uint32_t location = 0; location < 31; ++location)
		{
			if ((vertexInputFourComponentMask & (1u << location)) == 0) continue;
			spirv_cross::MSLShaderInterfaceVariable input;
			input.location = location;
			input.vecsize = 4;
			compiler.add_msl_shader_input(input);
		}
	}

	for (const auto& mapped: *bindings)
	{
		spirv_cross::MSLResourceBinding resource;
		resource.stage = model;
		resource.desc_set = mapped.set;
		resource.binding = mapped.binding;
		resource.count = mapped.count;
		if (DescriptorUsesBuffer(mapped.type)) resource.msl_buffer = mapped.metalBufferIndex;
		if (DescriptorUsesTexture(mapped.type)) resource.msl_texture = mapped.metalTextureIndex;
		if (DescriptorUsesSampler(mapped.type)) resource.msl_sampler = mapped.metalSamplerIndex;
		compiler.add_msl_resource_binding(resource);
	}

	spirv_cross::MSLResourceBinding pushConstants;
	pushConstants.stage = model;
	pushConstants.desc_set = spirv_cross::kPushConstDescSet;
	pushConstants.binding = spirv_cross::kPushConstBinding;
	pushConstants.msl_buffer = kPushConstantBufferIndex;
	compiler.add_msl_resource_binding(pushConstants);

	MTLSize compiledLocalSize {1, 1, 1};
	if (stage == VK_SHADER_STAGE_COMPUTE_BIT)
	{
		compiledLocalSize.width = std::max(1u, compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0));
		compiledLocalSize.height = std::max(1u, compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1));
		compiledLocalSize.depth = std::max(1u, compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2));
		if (localSize != nullptr) *localSize = compiledLocalSize;
	}

	const std::string source = compiler.compile();
	const std::string entry = compiler.get_cleansed_entry_point_name(stageInfo.pName, model);
	NSString* sourceString = [[NSString alloc] initWithBytes:source.data()
	                                                 length:source.size()
	                                               encoding:NSUTF8StringEncoding];
	if (sourceString == nil)
	{
		return VK_ERROR_INVALID_SHADER_NV;
	}
	MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
	options.languageVersion = MTLLanguageVersion2_3;
	if (@available(macOS 15.0, iOS 18.0, *))
	{
		options.mathMode = MTLMathModeSafe;
	} else
	{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		options.fastMathEnabled = NO;
#pragma clang diagnostic pop
	}
	NSError* error = nil;
	id<MTLLibrary> library = [device->metalDevice newLibraryWithSource:sourceString options:options error:&error];
	if (library == nil)
	{
		PrintMetalError("newLibraryWithSource", error);
		return VK_ERROR_INVALID_SHADER_NV;
	}
	NSString* entryString = [[NSString alloc] initWithBytes:entry.data()
	                                                length:entry.size()
	                                              encoding:NSUTF8StringEncoding];
	id<MTLFunction> compiledFunction = [library newFunctionWithName:entryString];
	if (compiledFunction == nil) return VK_ERROR_INVALID_SHADER_NV;

#if LUMEN_A64_METAL_SHADER_FUNCTION_CACHE_COMPILED
	if (cacheRequested)
	{
		gMetalShaderFunctionCacheTelemetry.compile.store(
		    true, std::memory_order_relaxed);
		CachedMetalShaderFunction compiled;
		compiled.library = library;
		compiled.function = compiledFunction;
		compiled.localSize = compiledLocalSize;
		compiled.pushConstantSize = compiledPushConstantSize;
		std::lock_guard cacheLock(cache->mutex);
		const auto inserted = cache->entries.emplace(std::move(cacheKey),
		                                             std::move(compiled));
		if (!inserted.second)
		{
			// A competing pipeline compiled the same full key while the costly
			// Metal compiler ran outside the device-cache lock.
			gMetalShaderFunctionCacheTelemetry.hit.store(
			    true, std::memory_order_relaxed);
			compiledFunction = inserted.first->second.function;
			if (localSize != nullptr)
			{
				*localSize = inserted.first->second.localSize;
			}
		}
	}
#endif
	*function = compiledFunction;
	*pushConstantSize = compiledPushConstantSize;
	return VK_SUCCESS;
}

id<MTLDepthStencilState> CreateDepthStencilStateUncached(
	VkPipeline pipeline, const ResolvedStencilState& resolved,
	const VkPipelineDepthStencilStateCreateInfo* dynamicSource = nullptr)
{
	if (pipeline == VK_NULL_HANDLE || !pipeline->hasDepthStencil)
	{
		return nil;
	}
	const auto& source = dynamicSource ? *dynamicSource : pipeline->depthStencil;
	MTLDepthStencilDescriptor* descriptor = [[MTLDepthStencilDescriptor alloc] init];
	descriptor.depthCompareFunction = source.depthTestEnable ? ToMTLCompareFunction(source.depthCompareOp)
	                                                       : MTLCompareFunctionAlways;
	descriptor.depthWriteEnabled = source.depthTestEnable && source.depthWriteEnable;
	if (source.stencilTestEnable)
	{
		auto makeStencil = [](const VkStencilOpState& state, uint32_t readMask, uint32_t writeMask)
		{
			MTLStencilDescriptor* stencil = [[MTLStencilDescriptor alloc] init];
			stencil.stencilCompareFunction = ToMTLCompareFunction(state.compareOp);
			stencil.stencilFailureOperation = ToMTLStencilOperation(state.failOp);
			stencil.depthFailureOperation = ToMTLStencilOperation(state.depthFailOp);
			stencil.depthStencilPassOperation = ToMTLStencilOperation(state.passOp);
			stencil.readMask = readMask;
			stencil.writeMask = writeMask;
			return stencil;
		};
		descriptor.frontFaceStencil =
		    makeStencil(source.front, resolved.frontCompareMask,
		                resolved.frontWriteMask);
		descriptor.backFaceStencil =
		    makeStencil(source.back, resolved.backCompareMask,
		                resolved.backWriteMask);
	}
	return [pipeline->device->metalDevice newDepthStencilStateWithDescriptor:descriptor];
}

id<MTLDepthStencilState> GetDepthStencilState(
	VkPipeline pipeline, const ResolvedStencilState& resolved)
{
#if LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED
	if (MetalDepthStencilStateCacheSelectorRequested())
	{
		auto& telemetry = gMetalDepthStencilStateCacheTelemetry;
		telemetry.lookup.fetch_add(1, std::memory_order_relaxed);
		auto cache = GetMetalDepthStencilStateCache(pipeline->device);
		if (!cache)
		{
			telemetry.error.fetch_add(1, std::memory_order_relaxed);
			return nil;
		}
		const MetalDepthStencilStateKey key = BuildMetalDepthStencilStateKey(
		    pipeline->depthStencil, resolved);
		std::lock_guard cacheLock(cache->mutex);
		const auto found = cache->entries.find(key);
		if (found != cache->entries.end())
		{
			telemetry.hit.fetch_add(1, std::memory_order_relaxed);
			return found->second.state;
		}

		telemetry.miss.fetch_add(1, std::memory_order_relaxed);
		id<MTLDepthStencilState> created =
		    CreateDepthStencilStateUncached(pipeline, resolved);
		if (created == nil)
		{
			telemetry.error.fetch_add(1, std::memory_order_relaxed);
			return nil;
		}
		CachedMetalDepthStencilState entry;
		entry.state = created;
		cache->entries.emplace(key, std::move(entry));
		telemetry.create.fetch_add(1, std::memory_order_relaxed);
		return created;
	}
#endif
	return CreateDepthStencilStateUncached(pipeline, resolved);
}

VkResult BindDescriptorResources(Lumen::MetalVulkan::EncodingContext& context, const std::shared_ptr<ReplayState>& state,
	                             VkPipelineBindPoint bindPoint);

void DeclareDeviceAddressResources(VkDevice device, Lumen::MetalVulkan::EncodingContext& context)
{
	// Physical pointers can reach buffers absent from the descriptor set. Metal
	// needs explicit residency/hazard declarations for every such allocation.
	std::lock_guard lock(device->mutex);
	for (const auto buffer : device->deviceAddressBuffers)
	{
		if (buffer->metalBuffer == nil) continue;
		if (context.renderEncoder != nil)
			[context.renderEncoder useResource:buffer->metalBuffer usage:MTLResourceUsageRead | MTLResourceUsageWrite
			                          stages:MTLRenderStageVertex | MTLRenderStageFragment];
		if (context.computeEncoder != nil)
			[context.computeEncoder useResource:buffer->metalBuffer usage:MTLResourceUsageRead | MTLResourceUsageWrite];
		std::lock_guard bufferLock(buffer->mutex);
		buffer->hostGpuWriteSeen = true;
		buffer->cpuShadowRanges.clear();
	}
}

id<MTLRenderPipelineState> ResolveRenderState(VkPipeline pipeline, const ReplayState& state)
{
	const bool dynamicStride = pipeline->dynamicStates.count(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE) != 0;
	const bool dynamicDiscard = pipeline->dynamicStates.count(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE) != 0;
	const bool dynamicCull = pipeline->dynamicStates.count(VK_DYNAMIC_STATE_CULL_MODE) != 0;
	if (!pipeline->viewportDynamic && !dynamicStride && !dynamicDiscard && !dynamicCull)
		return state.colorWriteEnabled ? pipeline->renderState : pipeline->renderStateNoColor;
	std::array<uint64_t, 17> key{};
	if (dynamicStride)
		for (size_t i = 0; i < state.vertexStrides.size(); ++i)
			if (pipeline->vertexBindingUsed[i]) key[i] = state.vertexStrides[i];
	const bool flipY = pipeline->viewportDynamic && state.viewport.height >= 0;
	const auto topology = pipeline->dynamicStates.count(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) ? state.topology : pipeline->topology;
    const bool triangles = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    const bool discard = (dynamicDiscard && state.rasterizerDiscard) ||
                         (dynamicCull && triangles && state.cullMode == VK_CULL_MODE_FRONT_AND_BACK);
	key[16] = (state.colorWriteEnabled ? 1 : 0) | (flipY ? 2 : 0) | (discard ? 4 : 0);
	std::lock_guard lock(pipeline->renderVariantMutex);
	const auto found = pipeline->renderVariants.find(key);
	if (found != pipeline->renderVariants.end()) return found->second.state;
	MTLRenderPipelineDescriptor* descriptor = [pipeline->renderDescriptor copy];
	if (flipY) descriptor.vertexFunction = pipeline->vertexFunctionFlipped;
	if (discard) descriptor.rasterizationEnabled = NO;
	if (dynamicStride)
	{
		descriptor.vertexDescriptor = [descriptor.vertexDescriptor copy];
		for (size_t i = 0; i < state.vertexStrides.size(); ++i)
		{
			if (!pipeline->vertexBindingUsed[i]) continue;
			auto* layout = descriptor.vertexDescriptor.layouts[pipeline->vertexBufferBase + i];
			layout.stride = state.vertexStrides[i];
			if (state.vertexStrides[i] == 0) layout.stepFunction = MTLVertexStepFunctionConstant;
			else if (layout.stepFunction == MTLVertexStepFunctionConstant) layout.stepFunction = MTLVertexStepFunctionPerVertex;
		}
	}
	if (!state.colorWriteEnabled)
		for (size_t i = 0; i < 8; ++i) descriptor.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
	NSError* error = nil;
	id<MTLRenderPipelineState> native = [pipeline->device->metalDevice newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (native == nil) { PrintMetalError("dynamic render pipeline", error); return nil; }
	pipeline->renderVariants.emplace(key, VkPipeline_T::RenderVariant{native});
	return native;
}

template <typename Bind>
VkResult BindPushConstants(const std::vector<uint8_t>& constants, uint32_t declaredSize, Bind&& bind)
{
	const size_t requiredSize = std::max(constants.size(), static_cast<size_t>(declaredSize));
	if (requiredSize > 4096) return VK_ERROR_FEATURE_NOT_PRESENT;
	if (requiredSize == 0) return VK_SUCCESS;
	// Metal validates the complete argument struct, including its trailing
	// alignment, even when Vulkan only updates some of the shader's members.
	const size_t uploadSize = (requiredSize + 15) & ~size_t {15};
	if (constants.size() == uploadSize)
	{
		bind(constants.data(), uploadSize);
	} else
	{
		std::array<uint8_t, 4096> padded {};
		if (!constants.empty()) std::memcpy(padded.data(), constants.data(), constants.size());
		bind(padded.data(), uploadSize);
	}
	return VK_SUCCESS;
}

VkResult ApplyGraphicsState(Lumen::MetalVulkan::EncodingContext& context, const std::shared_ptr<ReplayState>& state,
	                       bool drawConsumer = false)
{
	// Vulkan setters may precede every other required state/resource binding.
	// Preserve their recorded snapshots, but consume native state only at a
	// draw, after its resource hazard gate and complete binding snapshot.
	if (!drawConsumer) return VK_SUCCESS;
	if (context.renderEncoder == nil || !context.logicalRenderPassActive || context.renderPassMergePending ||
	    state->graphicsPipeline == VK_NULL_HANDLE)
	{
		return VK_SUCCESS;
	}
	VkPipeline pipeline = state->graphicsPipeline;
	if (!pipeline->alive.load(std::memory_order_acquire))
	{
		return VK_ERROR_DEVICE_LOST;
	}
	if (pipeline->viewportDynamic && !state->viewportSet) return drawConsumer ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
	id<MTLRenderPipelineState> renderState = ResolveRenderState(pipeline, *state);
	if (renderState == nil)
	{
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}
	[context.renderEncoder setRenderPipelineState:renderState];
	DeclareDeviceAddressResources(pipeline->device, context);
	const auto dynamic = [&](VkDynamicState value) { return pipeline->dynamicStates.count(value) != 0; };
	const auto cull = dynamic(VK_DYNAMIC_STATE_CULL_MODE)
	    ? (state->cullMode == VK_CULL_MODE_FRONT_BIT ? MTLCullModeFront :
	       state->cullMode == VK_CULL_MODE_BACK_BIT ? MTLCullModeBack : MTLCullModeNone) : pipeline->cullMode;
	[context.renderEncoder setCullMode:cull];
	[context.renderEncoder setFrontFacingWinding:dynamic(VK_DYNAMIC_STATE_FRONT_FACE)
	    ? (state->frontFace == VK_FRONT_FACE_CLOCKWISE ? MTLWindingClockwise : MTLWindingCounterClockwise) : pipeline->winding];
	[context.renderEncoder setDepthClipMode:pipeline->depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
	// Static pipeline state must also reset bias when the preceding draw used
	// it. Disabled pipelines store zeroes, independently of ignored inputs.
	const bool bias = !dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE) || state->depthBiasEnable;
	[context.renderEncoder setDepthBias:!bias ? 0 : dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS) ? state->depthBiasConstant : pipeline->depthBiasConstantFactor
	                            slopeScale:!bias ? 0 : dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS) ? state->depthBiasSlope : pipeline->depthBiasSlopeFactor
	                                 clamp:!bias ? 0 : dynamic(VK_DYNAMIC_STATE_DEPTH_BIAS) ? state->depthBiasClamp : pipeline->depthBiasClamp];
	const auto& blend = dynamic(VK_DYNAMIC_STATE_BLEND_CONSTANTS) ? state->blendConstants : pipeline->blendConstants;
	[context.renderEncoder setBlendColorRed:blend[0] green:blend[1] blue:blend[2] alpha:blend[3]];
	auto sourceViewport = pipeline->viewportDynamic ? state->viewport : pipeline->viewport;
	if (sourceViewport.height < 0) { sourceViewport.y += sourceViewport.height; sourceViewport.height = -sourceViewport.height; }
	MTLViewport viewport {sourceViewport.x, sourceViewport.y, sourceViewport.width,
	                      sourceViewport.height, sourceViewport.minDepth, sourceViewport.maxDepth};
	[context.renderEncoder setViewport:viewport];
	// Bind/state-only records may precede vkCmdSetScissor. Draw snapshots require
	// the dynamic value; until then there is no scissor state to apply.
	if (!pipeline->scissorDynamic || state->scissorSet)
	{
		const auto& rectangle = pipeline->scissorDynamic ? state->scissor : pipeline->scissor;
		MTLScissorRect scissor {static_cast<NSUInteger>(std::max(0, rectangle.offset.x)),
		                        static_cast<NSUInteger>(std::max(0, rectangle.offset.y)),
		                        rectangle.extent.width, rectangle.extent.height};
		[context.renderEncoder setScissorRect:scissor];
	}
	if (pipeline->hasDepthStencil)
	{
		ResolvedStencilState resolved = PreserveLegacyStencilState(state->stencil);
#if LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED
		if (MetalDepthStencilStateCacheSelectorRequested())
		{
			resolved = ResolveStencilState(pipeline, state->stencil);
		}
#endif
		auto depth = pipeline->depthStencil;
		const bool dynamicDepth = dynamic(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE) || dynamic(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE) ||
		                          dynamic(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP) || dynamic(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE) ||
		                          dynamic(VK_DYNAMIC_STATE_STENCIL_OP);
		if (dynamic(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)) depth.depthTestEnable = state->depthTest;
		if (dynamic(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE)) depth.depthWriteEnable = state->depthWrite;
		if (dynamic(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP)) depth.depthCompareOp = state->depthCompare;
		if (dynamic(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE)) depth.stencilTestEnable = state->stencilTest;
		if (dynamic(VK_DYNAMIC_STATE_STENCIL_OP)) { depth.front = state->frontStencil; depth.back = state->backStencil; }
		id<MTLDepthStencilState> depthState = dynamicDepth ? CreateDepthStencilStateUncached(pipeline, resolved, &depth)
		                                                : GetDepthStencilState(pipeline, resolved);
		if (depthState == nil)
		{
			return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		}
		[context.renderEncoder setDepthStencilState:depthState];
		[context.renderEncoder setStencilFrontReferenceValue:resolved.frontReference
		                                backReferenceValue:resolved.backReference];
	}
	for (uint32_t index = 0; index < state->vertexBuffers.size(); ++index)
	{
		if (!pipeline->vertexBindingUsed[index]) continue;
		VkBuffer buffer = state->vertexBuffers[index];
		if (buffer != VK_NULL_HANDLE && buffer->metalBuffer != nil)
		{
			const uint32_t metalIndex = pipeline->vertexBufferBase + index;
			if (metalIndex > kMaximumMetalBufferIndex)
			{
				return VK_ERROR_FEATURE_NOT_PRESENT;
			}
			[context.renderEncoder setVertexBuffer:buffer->metalBuffer
			                                offset:BufferMetalOffset(buffer, state->vertexOffsets[index])
			                               atIndex:metalIndex];
		}
	}
	const auto vertexPushResult = BindPushConstants(state->vertexPushConstants, pipeline->vertexPushConstantSize,
	    [&](const void* data, size_t size) {
		    [context.renderEncoder setVertexBytes:data length:size atIndex:kPushConstantBufferIndex];
	    });
	if (vertexPushResult != VK_SUCCESS) return vertexPushResult;
	const auto fragmentPushResult = BindPushConstants(state->fragmentPushConstants, pipeline->fragmentPushConstantSize,
	    [&](const void* data, size_t size) {
		    [context.renderEncoder setFragmentBytes:data length:size atIndex:kPushConstantBufferIndex];
	    });
	if (fragmentPushResult != VK_SUCCESS) return fragmentPushResult;
	return BindDescriptorResources(context, state, VK_PIPELINE_BIND_POINT_GRAPHICS);
}

VkResult ApplyComputeState(Lumen::MetalVulkan::EncodingContext& context, const std::shared_ptr<ReplayState>& state,
	                      bool dispatchConsumer = false)
{
	if (!dispatchConsumer) return VK_SUCCESS;
	if (context.computeEncoder == nil || state->computePipeline == VK_NULL_HANDLE)
	{
		return VK_SUCCESS;
	}
	VkPipeline pipeline = state->computePipeline;
	if (pipeline->computeState == nil || !pipeline->alive.load(std::memory_order_acquire))
	{
		return VK_ERROR_DEVICE_LOST;
	}
	[context.computeEncoder setComputePipelineState:pipeline->computeState];
	DeclareDeviceAddressResources(pipeline->device, context);
	const auto pushResult = BindPushConstants(state->computePushConstants, pipeline->computePushConstantSize,
	    [&](const void* data, size_t size) {
		    [context.computeEncoder setBytes:data length:size atIndex:kPushConstantBufferIndex];
	    });
	if (pushResult != VK_SUCCESS) return pushResult;
	return BindDescriptorResources(context, state, VK_PIPELINE_BIND_POINT_COMPUTE);
}

VkResult BindDescriptorResources(Lumen::MetalVulkan::EncodingContext& context, const std::shared_ptr<ReplayState>& state,
	                             VkPipelineBindPoint bindPoint)
{
	VkPipeline pipeline = bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? state->graphicsPipeline : state->computePipeline;
	if (pipeline == VK_NULL_HANDLE)
	{
		return VK_SUCCESS;
	}
	const auto& sets = bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? state->graphicsSets : state->computeSets;
	for (const auto& mapped: pipeline->stageBindings)
	{
		if (mapped.set >= sets.size() || sets[mapped.set] == VK_NULL_HANDLE)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		VkDescriptorSet set = sets[mapped.set];
		std::lock_guard lock(set->mutex);
		auto found = set->values.find(mapped.binding);
		if (found == set->values.end() || found->second.size() < mapped.count)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		for (uint32_t index = 0; index < mapped.count; ++index)
		{
			const auto& value = found->second[index];
			if (DescriptorUsesBuffer(mapped.type))
			{
				if (!MetalIndexInRange(mapped.metalBufferIndex, index, kMaximumMetalBufferIndex))
				{
					return VK_ERROR_INITIALIZATION_FAILED;
				}
				if (value.buffer == VK_NULL_HANDLE || value.buffer->metalBuffer == nil)
				{
					return VK_ERROR_INITIALIZATION_FAILED;
				}
				const uint32_t metalIndex = mapped.metalBufferIndex + index;
				const NSUInteger offset = BufferMetalOffset(value.buffer, value.offset);
				if (mapped.stage == VK_SHADER_STAGE_VERTEX_BIT)
					[context.renderEncoder setVertexBuffer:value.buffer->metalBuffer offset:offset atIndex:metalIndex];
				else if (mapped.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
					[context.renderEncoder setFragmentBuffer:value.buffer->metalBuffer offset:offset atIndex:metalIndex];
				else
					[context.computeEncoder setBuffer:value.buffer->metalBuffer offset:offset atIndex:metalIndex];
			}
			if (DescriptorUsesTexture(mapped.type))
			{
				if (!MetalIndexInRange(mapped.metalTextureIndex, index, kMaximumMetalTextureIndex))
				{
					return VK_ERROR_INITIALIZATION_FAILED;
				}
				id<MTLTexture> texture = value.imageView != VK_NULL_HANDLE ? value.imageView->metalTexture : nil;
				if (texture == nil)
				{
					return VK_ERROR_INITIALIZATION_FAILED;
				}
				const uint32_t metalIndex = mapped.metalTextureIndex + index;
				if (mapped.stage == VK_SHADER_STAGE_VERTEX_BIT)
					[context.renderEncoder setVertexTexture:texture atIndex:metalIndex];
				else if (mapped.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
					[context.renderEncoder setFragmentTexture:texture atIndex:metalIndex];
				else
					[context.computeEncoder setTexture:texture atIndex:metalIndex];
			}
			if (DescriptorUsesSampler(mapped.type))
			{
				if (!MetalIndexInRange(mapped.metalSamplerIndex, index, kMaximumMetalSamplerIndex))
				{
					return VK_ERROR_INITIALIZATION_FAILED;
				}
				id<MTLSamplerState> sampler = value.sampler != VK_NULL_HANDLE ? value.sampler->metalSampler : nil;
				if (sampler == nil)
				{
					return VK_ERROR_INITIALIZATION_FAILED;
				}
				const uint32_t metalIndex = mapped.metalSamplerIndex + index;
				if (mapped.stage == VK_SHADER_STAGE_VERTEX_BIT)
					[context.renderEncoder setVertexSamplerState:sampler atIndex:metalIndex];
				else if (mapped.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
					[context.renderEncoder setFragmentSamplerState:sampler atIndex:metalIndex];
				else
					[context.computeEncoder setSamplerState:sampler atIndex:metalIndex];
			}
		}
	}
	return VK_SUCCESS;
}

VkResult InvalidateComputeStorageShadows(const std::shared_ptr<ReplayState>& state)
{
	if (state == nullptr || state->computePipeline == VK_NULL_HANDLE)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	for (const StageBinding& mapped: state->computePipeline->stageBindings)
	{
		if (mapped.stage != VK_SHADER_STAGE_COMPUTE_BIT ||
		    (mapped.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
		     mapped.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC))
		{
			continue;
		}
		if (mapped.set >= state->computeSets.size() || state->computeSets[mapped.set] == VK_NULL_HANDLE)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		VkDescriptorSet set = state->computeSets[mapped.set];
		std::lock_guard setLock(set->mutex);
		auto found = set->values.find(mapped.binding);
		if (found == set->values.end() || found->second.size() < mapped.count)
		{
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		for (uint32_t index = 0; index < mapped.count; ++index)
		{
			const DescriptorValue& value = found->second[index];
			if (value.buffer == VK_NULL_HANDLE || value.offset > value.buffer->size)
			{
				return VK_ERROR_INITIALIZATION_FAILED;
			}
			const VkDeviceSize range = value.range == VK_WHOLE_SIZE ? value.buffer->size - value.offset : value.range;
			const VkResult invalidate = InvalidateGpuBufferWrite(value.buffer, value.offset, range);
			if (invalidate != VK_SUCCESS)
			{
				return invalidate;
			}
		}
	}
	return VK_SUCCESS;
}

} // namespace

namespace Lumen::MetalVulkan {

void CompleteEvent(const std::shared_ptr<EventState>& event, uint64_t generation, MTLCommandBufferStatus status)
{
	if (!event || event->device == VK_NULL_HANDLE) return;
	std::lock_guard lock(event->mutex);
	if (status == MTLCommandBufferStatusError)
	{
		// Do not depend on the ordering of this handler and the queue's
		// completion/fence handler. A failed command must not publish a label.
		MarkDeviceLost(event->device);
		event->condition.notify_all();
		return;
	}
	if (status == MTLCommandBufferStatusCompleted && event->alive && event->generation == generation &&
	    !event->device->lost.load(std::memory_order_acquire))
	{
		event->set = true;
		event->condition.notify_all();
	}
}

void ObserveCompletionPassReuse(const EncodingContext& context)
{
	auto& counters = gCompletionPassReuseCounters;
	counters.logical.fetch_add(context.logicalRenderPasses, std::memory_order_relaxed);
	counters.physical.fetch_add(context.physicalRenderEncoders, std::memory_order_relaxed);
	counters.merged.fetch_add(context.mergedRenderPasses, std::memory_order_relaxed);
	counters.mergedAfterMarker.fetch_add(context.mergedAfterCompletionMarker, std::memory_order_relaxed);
	counters.resourceRejected.fetch_add(context.resourceRejectedPasses, std::memory_order_relaxed);
	const uint64_t buffers = counters.commandBuffers.fetch_add(1, std::memory_order_relaxed) + 1;
	// At most 21 progress records per process; ordinary timing runs leave trace off.
	if (CompletionPassTraceRequested() && buffers <= (1u << 20) && (buffers & (buffers - 1)) == 0)
		PrintCompletionPassReuseCounters("encode");
}

void FinalizeCompletionPassReuse()
{
	if (!CompletionPassReuseRequested() && !CompletionPassTraceRequested()) return;
	static std::once_flag finalizeOnce;
	std::call_once(finalizeOnce, [] { PrintCompletionPassReuseCounters("final"); });
}

bool MetalShaderFunctionCacheRequested()
{
#if LUMEN_A64_METAL_SHADER_FUNCTION_CACHE_COMPILED
	return MetalShaderFunctionCacheSelectorRequested();
#else
	return false;
#endif
}

bool FinalizeMetalShaderFunctionCache()
{
#if LUMEN_A64_METAL_SHADER_FUNCTION_CACHE_COMPILED
	if (!MetalShaderFunctionCacheSelectorRequested()) return true;
	auto& telemetry = gMetalShaderFunctionCacheTelemetry;
	std::call_once(telemetry.finalizeOnce, []()
	{
		auto& telemetry = gMetalShaderFunctionCacheTelemetry;
		const bool lookup = telemetry.lookup.load(std::memory_order_acquire);
		const bool hit = telemetry.hit.load(std::memory_order_acquire);
		const bool compile = telemetry.compile.load(std::memory_order_acquire);
		const bool requested = MetalShaderFunctionCacheSelectorRequested();
		const bool coverage = lookup && hit && compile && requested;
		telemetry.finalizeResult = coverage;
		std::fprintf(stderr,
		    "[A64_METAL_SHADER_FUNCTION_CACHE_FINAL] lookup=%s hit=%s "
		    "compile=%s requested=%s status=%s coverage=%s\n",
		    lookup ? "positive" : "zero", hit ? "positive" : "zero",
		    compile ? "positive" : "zero", requested ? "positive" : "zero",
		    coverage ? "clean" : "invalid", coverage ? "positive" : "zero");
		std::fflush(stderr);
	});
	return telemetry.finalizeResult;
#else
	return true;
#endif
}

bool MetalDepthStencilStateCacheRequested()
{
#if LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED
	return MetalDepthStencilStateCacheSelectorRequested();
#else
	return false;
#endif
}

bool FinalizeMetalDepthStencilStateCache()
{
#if LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED
	if (!MetalDepthStencilStateCacheSelectorRequested()) return true;
	auto& telemetry = gMetalDepthStencilStateCacheTelemetry;
	std::call_once(telemetry.finalizeOnce, []()
	{
		auto& telemetry = gMetalDepthStencilStateCacheTelemetry;
		const uint64_t lookup =
		    telemetry.lookup.load(std::memory_order_acquire);
		const uint64_t hit = telemetry.hit.load(std::memory_order_acquire);
		const uint64_t miss = telemetry.miss.load(std::memory_order_acquire);
		const uint64_t create =
		    telemetry.create.load(std::memory_order_acquire);
		const uint64_t error =
		    telemetry.error.load(std::memory_order_acquire);
		const bool requested = MetalDepthStencilStateCacheSelectorRequested();
		constexpr bool compiled =
		    LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED != 0;
		const bool createMatch = miss != 0 && create == miss;
		const bool coverage = requested && compiled && lookup != 0 &&
		                      hit != 0 && miss != 0 && createMatch && error == 0;
		telemetry.finalizeResult = coverage;
		std::fprintf(
		    stderr,
		    "[A64_METAL_DEPTH_STENCIL_STATE_CACHE_TELEMETRY] "
		    "lookup=%llu hit=%llu miss=%llu create=%llu error=%llu\n",
		    static_cast<unsigned long long>(lookup),
		    static_cast<unsigned long long>(hit),
		    static_cast<unsigned long long>(miss),
		    static_cast<unsigned long long>(create),
		    static_cast<unsigned long long>(error));
		std::fprintf(
		    stderr,
		    "[A64_METAL_DEPTH_STENCIL_STATE_CACHE_FINAL] "
		    "requested=positive compiled=positive lookup=%s hit=%s miss=%s "
		    "create_match=%s status=%s coverage=%s\n",
		    lookup != 0 ? "positive" : "zero",
		    hit != 0 ? "positive" : "zero",
		    miss != 0 ? "positive" : "zero",
		    createMatch ? "positive" : "mismatch",
		    coverage ? "clean" : "invalid",
		    coverage ? "positive" : "zero");
		std::fflush(stderr);
	});
	return telemetry.finalizeResult;
#else
	return true;
#endif
}

bool FinalizeMetalStateOnlyRecordElision()
{
#if LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION_COMPILED
	if (!MetalStateOnlyRecordElisionSelectorRequested()) return true;
	auto& telemetry = gMetalStateOnlyRecordElisionTelemetry;
	std::call_once(telemetry.finalizeOnce, []()
	{
		auto& telemetry = gMetalStateOnlyRecordElisionTelemetry;
		const bool stateOnly = telemetry.stateOnlyElided.load(std::memory_order_acquire);
		const bool snapshot = telemetry.snapshotConsumed.load(std::memory_order_acquire);
		const bool coverage = stateOnly && snapshot;
		telemetry.finalizeResult = coverage;
		std::fprintf(stderr,
		    "[A64_METAL_STATE_ONLY_RECORD_ELISION_FINAL] requested=positive "
		    "effective=positive state_only=%s snapshot_consumed=%s status=%s coverage=%s\n",
		    stateOnly ? "positive" : "zero", snapshot ? "positive" : "zero",
		    coverage ? "clean" : "invalid", coverage ? "positive" : "zero");
		std::fflush(stderr);
	});
	return telemetry.finalizeResult;
#else
	return true;
#endif
}

} // namespace Lumen::MetalVulkan

extern "C" void LumenMetalGetCompletionPassReuseStatsV1(uint64_t* logical, uint64_t* physical,
	                                                   uint64_t* merged, uint64_t* merged_after_marker, uint32_t* enabled)
{
	const auto& counters = gCompletionPassReuseCounters;
	if (logical != nullptr) *logical = counters.logical.load(std::memory_order_relaxed);
	if (physical != nullptr) *physical = counters.physical.load(std::memory_order_relaxed);
	if (merged != nullptr) *merged = counters.merged.load(std::memory_order_relaxed);
	if (merged_after_marker != nullptr) *merged_after_marker = counters.mergedAfterMarker.load(std::memory_order_relaxed);
	if (enabled != nullptr) *enabled = CompletionPassReuseRequested() ? 1u : 0u;
}

extern "C" bool LumenMetalResourceAliasesRenderAttachmentForTest(VkImageView attachment, VkImageView sampled, VkBuffer buffer)
{
	return ResourceAliasesRenderAttachment(attachment, sampled, buffer);
}

extern "C" bool LumenMetalFinalizeCompletionPassReuse()
{
	Lumen::MetalVulkan::FinalizeCompletionPassReuse();
	return true;
}

extern "C" bool LumenMetalFinalizeShaderFunctionCache()
{
	return Lumen::MetalVulkan::FinalizeMetalShaderFunctionCache();
}

extern "C" bool LumenMetalFinalizeDepthStencilStateCache()
{
	return Lumen::MetalVulkan::FinalizeMetalDepthStencilStateCache();
}

extern "C" bool LumenMetalFinalizeStateOnlyRecordElision()
{
	return Lumen::MetalVulkan::FinalizeMetalStateOnlyRecordElision();
}

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* createInfo,
	                                                 const VkAllocationCallbacks* allocator, VkShaderModule* shaderModule)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || shaderModule == nullptr || allocator != nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO || createInfo->pCode == nullptr ||
	    createInfo->codeSize == 0 || (createInfo->codeSize % sizeof(uint32_t)) != 0 ||
	    createInfo->pCode[0] != spv::MagicNumber)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	auto created = std::unique_ptr<VkShaderModule_T>(new (std::nothrow) VkShaderModule_T);
	if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->device = device;
	created->words.assign(createInfo->pCode, createInfo->pCode + createInfo->codeSize / sizeof(uint32_t));
	*shaderModule = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule shaderModule, const VkAllocationCallbacks* allocator)
{
	if (shaderModule == VK_NULL_HANDLE || allocator != nullptr) return;
	shaderModule->alive.store(false, std::memory_order_release);
	delete shaderModule;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device,
	                                                        const VkDescriptorSetLayoutCreateInfo* createInfo,
	                                                        const VkAllocationCallbacks* allocator,
	                                                        VkDescriptorSetLayout* setLayout)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || setLayout == nullptr || allocator != nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO ||
	    (createInfo->bindingCount != 0 && createInfo->pBindings == nullptr))
		return VK_ERROR_INITIALIZATION_FAILED;
	auto created = std::unique_ptr<VkDescriptorSetLayout_T>(new (std::nothrow) VkDescriptorSetLayout_T);
	if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->device = device;
	created->bindings.assign(createInfo->pBindings, createInfo->pBindings + createInfo->bindingCount);
	std::sort(created->bindings.begin(), created->bindings.end(),
	          [](const auto& left, const auto& right) { return left.binding < right.binding; });
	for (auto& binding: created->bindings)
	{
		if (binding.descriptorCount == 0)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		if (!DescriptorUsesBuffer(binding.descriptorType) && !DescriptorUsesTexture(binding.descriptorType) &&
		    !DescriptorUsesSampler(binding.descriptorType))
			return VK_ERROR_FEATURE_NOT_PRESENT;
		if (DescriptorUsesSampler(binding.descriptorType) && binding.pImmutableSamplers != nullptr)
		{
			auto& samplers = created->immutableSamplers[binding.binding];
			samplers.assign(binding.pImmutableSamplers, binding.pImmutableSamplers + binding.descriptorCount);
			for (VkSampler sampler: samplers)
				if (sampler == VK_NULL_HANDLE || sampler->device != device ||
				    !sampler->alive.load(std::memory_order_acquire))
					return VK_ERROR_INITIALIZATION_FAILED;
			// Vulkan copies the handle array; the caller retains sampler ownership.
			binding.pImmutableSamplers = samplers.data();
		} else
		{
			// The member is ignored for descriptor types without a sampler.
			binding.pImmutableSamplers = nullptr;
		}
	}
	*setLayout = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout setLayout,
	                                                    const VkAllocationCallbacks* allocator)
{
	if (setLayout == VK_NULL_HANDLE || allocator != nullptr) return;
	setLayout->alive.store(false, std::memory_order_release);
	delete setLayout;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* createInfo,
	                                                  const VkAllocationCallbacks* allocator, VkDescriptorPool* descriptorPool)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || descriptorPool == nullptr || allocator != nullptr ||
	    createInfo->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO || createInfo->maxSets == 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	auto created = std::unique_ptr<VkDescriptorPool_T>(new (std::nothrow) VkDescriptorPool_T);
	if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->device = device;
	created->maximumSets = createInfo->maxSets;
	created->flags = createInfo->flags;
	*descriptorPool = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice, VkDescriptorPool descriptorPool,
	                                               const VkAllocationCallbacks* allocator)
{
	if (descriptorPool == VK_NULL_HANDLE || allocator != nullptr) return;
	{
		std::lock_guard lock(descriptorPool->mutex);
		descriptorPool->alive.store(false, std::memory_order_release);
		for (VkDescriptorSet set: descriptorPool->sets)
		{
			set->alive.store(false, std::memory_order_release);
			delete set;
		}
		descriptorPool->sets.clear();
	}
	delete descriptorPool;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* allocateInfo,
	                                                    VkDescriptorSet* descriptorSets)
{
	if (device == VK_NULL_HANDLE || allocateInfo == nullptr || descriptorSets == nullptr ||
	    allocateInfo->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO ||
	    allocateInfo->descriptorPool == VK_NULL_HANDLE || allocateInfo->pSetLayouts == nullptr)
		return VK_ERROR_INITIALIZATION_FAILED;
	VkDescriptorPool pool = allocateInfo->descriptorPool;
	std::lock_guard lock(pool->mutex);
	if (!pool->alive.load(std::memory_order_acquire) ||
	    pool->sets.size() + allocateInfo->descriptorSetCount > pool->maximumSets)
		return VK_ERROR_OUT_OF_POOL_MEMORY;
	std::vector<VkDescriptorSet> allocated;
	allocated.reserve(allocateInfo->descriptorSetCount);
	for (uint32_t index = 0; index < allocateInfo->descriptorSetCount; ++index)
	{
		if (allocateInfo->pSetLayouts[index] == VK_NULL_HANDLE)
		{
			for (auto set: allocated) delete set;
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		auto set = std::unique_ptr<VkDescriptorSet_T>(new (std::nothrow) VkDescriptorSet_T);
		if (!set)
		{
			for (auto old: allocated) delete old;
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
		set->pool = pool;
		set->layout = allocateInfo->pSetLayouts[index];
		for (const auto& binding: set->layout->bindings)
		{
			auto& values = set->values[binding.binding];
			values.resize(binding.descriptorCount);
			for (uint32_t element = 0; element < binding.descriptorCount; ++element)
			{
				values[element].type = binding.descriptorType;
				if (binding.pImmutableSamplers != nullptr)
				{
					values[element].sampler = binding.pImmutableSamplers[element];
					values[element].immutableSampler = true;
				}
			}
		}
		allocated.push_back(set.release());
	}
	for (uint32_t index = 0; index < allocated.size(); ++index)
	{
		pool->sets.insert(allocated[index]);
		descriptorSets[index] = allocated[index];
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice device, VkDescriptorPool descriptorPool,
	                                                VkDescriptorPoolResetFlags flags)
{
	if (device == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE || descriptorPool->device != device || flags != 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (device->lost.load(std::memory_order_acquire)) return VK_ERROR_DEVICE_LOST;
	// The caller must complete every submitted use before resetting the pool.
	// Reset invalidates all sets, including pools without FREE_DESCRIPTOR_SET.
	std::lock_guard lock(descriptorPool->mutex);
	if (!descriptorPool->alive.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
	for (VkDescriptorSet set: descriptorPool->sets)
	{
		set->alive.store(false, std::memory_order_release);
		delete set;
	}
	descriptorPool->sets.clear();
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice, VkDescriptorPool descriptorPool, uint32_t descriptorSetCount,
	                                                const VkDescriptorSet* descriptorSets)
{
	if (descriptorPool == VK_NULL_HANDLE || (descriptorSetCount != 0 && descriptorSets == nullptr))
		return VK_ERROR_INITIALIZATION_FAILED;
	if ((descriptorPool->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) == 0)
		return VK_ERROR_FRAGMENTED_POOL;
	std::lock_guard lock(descriptorPool->mutex);
	for (uint32_t index = 0; index < descriptorSetCount; ++index)
	{
		VkDescriptorSet set = descriptorSets[index];
		if (set == VK_NULL_HANDLE || descriptorPool->sets.erase(set) != 1)
			return VK_ERROR_INITIALIZATION_FAILED;
		set->alive.store(false, std::memory_order_release);
		delete set;
	}
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t writeCount, const VkWriteDescriptorSet* writes,
	                                              uint32_t copyCount, const VkCopyDescriptorSet* copies)
{
	if ((writeCount != 0 && writes == nullptr) || (copyCount != 0 && copies == nullptr)) return;
	for (uint32_t writeIndex = 0; writeIndex < writeCount; ++writeIndex)
	{
		const auto& write = writes[writeIndex];
		if (write.dstSet == VK_NULL_HANDLE) continue;
		std::lock_guard lock(write.dstSet->mutex);
		auto found = write.dstSet->values.find(write.dstBinding);
		if (found == write.dstSet->values.end() || write.dstArrayElement + write.descriptorCount > found->second.size()) continue;
		for (uint32_t index = 0; index < write.descriptorCount; ++index)
		{
			auto& value = found->second[write.dstArrayElement + index];
			const VkSampler immutableSampler = value.immutableSampler ? value.sampler : VK_NULL_HANDLE;
			value = {};
			value.type = write.descriptorType;
			value.immutableSampler = immutableSampler != VK_NULL_HANDLE;
			value.sampler = immutableSampler;
			if (DescriptorUsesBuffer(write.descriptorType) && write.pBufferInfo != nullptr)
			{
				value.buffer = write.pBufferInfo[index].buffer;
				value.offset = write.pBufferInfo[index].offset;
				value.range = write.pBufferInfo[index].range;
			}
			if (DescriptorUsesTexture(write.descriptorType) && write.pImageInfo != nullptr)
			{
				value.imageView = write.pImageInfo[index].imageView;
				value.imageLayout = write.pImageInfo[index].imageLayout;
			}
			if (DescriptorUsesSampler(write.descriptorType) && !value.immutableSampler && write.pImageInfo != nullptr)
				value.sampler = write.pImageInfo[index].sampler;
		}
	}
	for (uint32_t copyIndex = 0; copyIndex < copyCount; ++copyIndex)
	{
		const auto& copy = copies[copyIndex];
		if (copy.srcSet == VK_NULL_HANDLE || copy.dstSet == VK_NULL_HANDLE) continue;
		auto copyValues = [&copy]
		{
			auto source = copy.srcSet->values.find(copy.srcBinding);
			auto destination = copy.dstSet->values.find(copy.dstBinding);
			if (source == copy.srcSet->values.end() || destination == copy.dstSet->values.end()) return;
			std::vector<DescriptorValue> snapshot;
			for (uint32_t index = 0; index < copy.descriptorCount; ++index)
			{
				if (copy.srcArrayElement + index >= source->second.size()) break;
				snapshot.push_back(source->second[copy.srcArrayElement + index]);
			}
			for (uint32_t index = 0; index < snapshot.size(); ++index)
			{
				if (copy.dstArrayElement + index >= destination->second.size()) break;
				auto& value = destination->second[copy.dstArrayElement + index];
				const VkSampler immutableSampler = value.immutableSampler ? value.sampler : VK_NULL_HANDLE;
				value = snapshot[index];
				// Immutability belongs to the destination layout, not the source.
				value.immutableSampler = immutableSampler != VK_NULL_HANDLE;
				if (value.immutableSampler) value.sampler = immutableSampler;
			}
		};
		if (copy.srcSet == copy.dstSet)
		{
			std::lock_guard lock(copy.srcSet->mutex);
			copyValues();
		} else
		{
			std::scoped_lock lock(copy.srcSet->mutex, copy.dstSet->mutex);
			copyValues();
		}
	}
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* createInfo,
	                                                   const VkAllocationCallbacks* allocator, VkPipelineLayout* pipelineLayout)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || pipelineLayout == nullptr || allocator != nullptr ||
	    (createInfo->setLayoutCount != 0 && createInfo->pSetLayouts == nullptr) ||
	    (createInfo->pushConstantRangeCount != 0 && createInfo->pPushConstantRanges == nullptr))
		return VK_ERROR_INITIALIZATION_FAILED;
	auto created = std::unique_ptr<VkPipelineLayout_T>(new (std::nothrow) VkPipelineLayout_T);
	if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->device = device;
	created->setLayouts.assign(createInfo->pSetLayouts, createInfo->pSetLayouts + createInfo->setLayoutCount);
	created->pushConstantRanges.assign(createInfo->pPushConstantRanges,
	                                  createInfo->pPushConstantRanges + createInfo->pushConstantRangeCount);
	*pipelineLayout = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout pipelineLayout,
	                                               const VkAllocationCallbacks* allocator)
{
	if (pipelineLayout == VK_NULL_HANDLE || allocator != nullptr) return;
	pipelineLayout->alive.store(false, std::memory_order_release);
	delete pipelineLayout;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* createInfo,
	                                              const VkAllocationCallbacks* allocator, VkRenderPass* renderPass)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || renderPass == nullptr || allocator != nullptr ||
	    createInfo->subpassCount != 1 || createInfo->pSubpasses == nullptr ||
	    (createInfo->attachmentCount != 0 && createInfo->pAttachments == nullptr))
		return VK_ERROR_INITIALIZATION_FAILED;
	const auto& subpass = createInfo->pSubpasses[0];
	if (subpass.pipelineBindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS || subpass.colorAttachmentCount > 1 ||
	    subpass.inputAttachmentCount != 0 || subpass.pResolveAttachments != nullptr)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	auto created = std::unique_ptr<VkRenderPass_T>(new (std::nothrow) VkRenderPass_T);
	if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->device = device;
	created->attachments.assign(createInfo->pAttachments, createInfo->pAttachments + createInfo->attachmentCount);
	created->passReuseEligible = CompletionPassReuseRequested() && RenderPassAllowsReuse(*createInfo);
	if (subpass.colorAttachmentCount == 1 && subpass.pColorAttachments != nullptr)
		created->colorAttachment = subpass.pColorAttachments[0];
	if (subpass.pDepthStencilAttachment != nullptr)
	{
		created->depthAttachment = *subpass.pDepthStencilAttachment;
		created->hasDepthAttachment = created->depthAttachment.attachment != VK_ATTACHMENT_UNUSED;
	}
	*renderPass = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice, VkRenderPass renderPass, const VkAllocationCallbacks* allocator)
{
	if (renderPass == VK_NULL_HANDLE || allocator != nullptr) return;
	renderPass->alive.store(false, std::memory_order_release);
	delete renderPass;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* createInfo,
	                                               const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer)
{
	if (device == VK_NULL_HANDLE || createInfo == nullptr || framebuffer == nullptr || allocator != nullptr ||
	    createInfo->renderPass == VK_NULL_HANDLE || createInfo->pAttachments == nullptr || createInfo->layers != 1)
		return VK_ERROR_INITIALIZATION_FAILED;
	auto created = std::unique_ptr<VkFramebuffer_T>(new (std::nothrow) VkFramebuffer_T);
	if (!created) return VK_ERROR_OUT_OF_HOST_MEMORY;
	created->device = device;
	created->renderPass = createInfo->renderPass;
	created->attachments.assign(createInfo->pAttachments, createInfo->pAttachments + createInfo->attachmentCount);
	created->width = createInfo->width;
	created->height = createInfo->height;
	created->layers = createInfo->layers;
	created->passReuseEligible = createInfo->pNext == nullptr && createInfo->flags == 0;
	*framebuffer = created.release();
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice, VkFramebuffer framebuffer, const VkAllocationCallbacks* allocator)
{
	if (framebuffer == VK_NULL_HANDLE || allocator != nullptr) return;
	framebuffer->alive.store(false, std::memory_order_release);
	delete framebuffer;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache,
	                                                     uint32_t createInfoCount,
	                                                     const VkGraphicsPipelineCreateInfo* createInfos,
	                                                     const VkAllocationCallbacks* allocator,
	                                                     VkPipeline* pipelines)
{
	if (device == VK_NULL_HANDLE || (pipelineCache != VK_NULL_HANDLE && pipelineCache->device != device) ||
	    createInfoCount == 0 || createInfos == nullptr ||
	    pipelines == nullptr || allocator != nullptr)
		return VK_ERROR_INITIALIZATION_FAILED;
	std::fill(pipelines, pipelines + createInfoCount, VK_NULL_HANDLE);
	for (uint32_t pipelineIndex = 0; pipelineIndex < createInfoCount; ++pipelineIndex)
	{
		const auto& info = createInfos[pipelineIndex];
		const VkPipelineRenderingCreateInfo* rendering = nullptr;
		for (auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next != nullptr; next = next->pNext)
			if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
				rendering = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(next);
		if (info.sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO || info.stageCount == 0 || info.stageCount > 2 || info.pStages == nullptr ||
		    info.layout == VK_NULL_HANDLE || (info.renderPass == VK_NULL_HANDLE && rendering == nullptr) || info.subpass != 0 ||
		    info.pVertexInputState == nullptr || info.pInputAssemblyState == nullptr || info.pViewportState == nullptr ||
		    info.pRasterizationState == nullptr || info.pMultisampleState == nullptr || info.pColorBlendState == nullptr ||
		    info.pViewportState->viewportCount > 1 || info.pViewportState->scissorCount > 1 ||
		    ![device->metalDevice supportsTextureSampleCount:info.pMultisampleState->rasterizationSamples] ||
		    info.pRasterizationState->polygonMode != VK_POLYGON_MODE_FILL ||
		    (info.pInputAssemblyState->primitiveRestartEnable == VK_TRUE &&
             info.pInputAssemblyState->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP &&
             info.pInputAssemblyState->topology != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP))
		{
			std::fprintf(stderr,
			    "vkCreateGraphicsPipelines: unsupported state stages=%u topology=%d vertexInput=%u "
			    "viewports=%u scissors=%u samples=%u polygonMode=%d primitiveRestart=%u\n",
			    info.stageCount,
			    info.pInputAssemblyState ? static_cast<int>(info.pInputAssemblyState->topology) : -1,
			    info.pVertexInputState != nullptr,
			    info.pViewportState ? info.pViewportState->viewportCount : 0,
			    info.pViewportState ? info.pViewportState->scissorCount : 0,
			    info.pMultisampleState ? static_cast<uint32_t>(info.pMultisampleState->rasterizationSamples) : 0,
			    info.pRasterizationState ? static_cast<int>(info.pRasterizationState->polygonMode) : -1,
			    info.pInputAssemblyState ? info.pInputAssemblyState->primitiveRestartEnable : 0);
			return VK_ERROR_FEATURE_NOT_PRESENT;
		}
		if ((info.pVertexInputState->vertexBindingDescriptionCount != 0 &&
		     info.pVertexInputState->pVertexBindingDescriptions == nullptr) ||
		    (info.pVertexInputState->vertexAttributeDescriptionCount != 0 &&
		     info.pVertexInputState->pVertexAttributeDescriptions == nullptr))
			return VK_ERROR_INITIALIZATION_FAILED;
		uint32_t vertexInputFourComponentMask = 0;
		for (uint32_t index = 0; index < info.pVertexInputState->vertexAttributeDescriptionCount; ++index)
		{
			const auto& attribute = info.pVertexInputState->pVertexAttributeDescriptions[index];
			if (attribute.location >= 31 || attribute.binding >= 16 ||
			    ToMTLVertexFormat(attribute.format) == MTLVertexFormatInvalid)
				return VK_ERROR_FORMAT_NOT_SUPPORTED;
			if (attribute.format == VK_FORMAT_R8G8B8A8_SNORM)
				vertexInputFourComponentMask |= 1u << attribute.location;
		}

		bool scissorDynamic = false;
		bool viewportDynamic = false;
		if (info.pDynamicState != nullptr)
		{
			if (info.pDynamicState->dynamicStateCount != 0 && info.pDynamicState->pDynamicStates == nullptr)
				return VK_ERROR_INITIALIZATION_FAILED;
			for (uint32_t index = 0; index < info.pDynamicState->dynamicStateCount; ++index)
			{
				if (info.pDynamicState->pDynamicStates[index] == VK_DYNAMIC_STATE_SCISSOR) scissorDynamic = true;
				if (info.pDynamicState->pDynamicStates[index] == VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT) scissorDynamic = true;
				if (info.pDynamicState->pDynamicStates[index] == VK_DYNAMIC_STATE_VIEWPORT ||
				    info.pDynamicState->pDynamicStates[index] == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) viewportDynamic = true;
			}
		}
		if (!scissorDynamic && info.pViewportState->pScissors == nullptr) return VK_ERROR_FEATURE_NOT_PRESENT;
		if (!viewportDynamic && info.pViewportState->pViewports == nullptr) return VK_ERROR_FEATURE_NOT_PRESENT;

		auto pipeline = std::unique_ptr<VkPipeline_T>(new (std::nothrow) VkPipeline_T);
		if (!pipeline) return VK_ERROR_OUT_OF_HOST_MEMORY;
		pipeline->device = device;
		pipeline->layout = info.layout;
		pipeline->renderPass = info.renderPass;
		pipeline->bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		pipeline->topology = info.pInputAssemblyState->topology;
		if (pipeline->topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST &&
		    pipeline->topology != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
		    pipeline->topology != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP &&
		    pipeline->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
		    pipeline->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP &&
		    pipeline->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		pipeline->viewportDynamic = viewportDynamic;
		if (info.pDynamicState != nullptr)
			pipeline->dynamicStates.insert(info.pDynamicState->pDynamicStates,
			                               info.pDynamicState->pDynamicStates + info.pDynamicState->dynamicStateCount);
		if (!viewportDynamic) pipeline->viewport = info.pViewportState->pViewports[0];
		const bool flipVertexY = !viewportDynamic && pipeline->viewport.height >= 0.0f;
		pipeline->scissorDynamic = scissorDynamic;
		if (!scissorDynamic) pipeline->scissor = info.pViewportState->pScissors[0];
		if (pipeline->viewport.height < 0.0f)
		{
			pipeline->viewport.y += pipeline->viewport.height;
			pipeline->viewport.height = -pipeline->viewport.height;
		}
		const auto& rasterization = *info.pRasterizationState;
		pipeline->depthClamp = rasterization.depthClampEnable == VK_TRUE;
		if (rasterization.depthBiasEnable)
		{
			if (!std::isfinite(rasterization.depthBiasConstantFactor) ||
			    !std::isfinite(rasterization.depthBiasSlopeFactor) || !std::isfinite(rasterization.depthBiasClamp))
				return VK_ERROR_INITIALIZATION_FAILED;
			// depthBiasClamp is not an advertised device feature. Extended bias
			// representations likewise need their own implementation and tests.
			if (rasterization.depthBiasClamp != 0.0f || rasterization.pNext != nullptr)
				return VK_ERROR_FEATURE_NOT_PRESENT;
			pipeline->depthBiasConstantFactor = rasterization.depthBiasConstantFactor;
			pipeline->depthBiasSlopeFactor = rasterization.depthBiasSlopeFactor;
			pipeline->depthBiasClamp = rasterization.depthBiasClamp;
		}
		pipeline->cullMode = rasterization.cullMode == VK_CULL_MODE_FRONT_BIT ? MTLCullModeFront
		                   : rasterization.cullMode == VK_CULL_MODE_BACK_BIT ? MTLCullModeBack
		                                                                         : MTLCullModeNone;
		pipeline->winding = rasterization.frontFace == VK_FRONT_FACE_CLOCKWISE ? MTLWindingClockwise
		                                                                         : MTLWindingCounterClockwise;
		if (info.pColorBlendState->logicOpEnable || info.pColorBlendState->attachmentCount > 8)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		for (uint32_t index = 0; index < 4; ++index)
			pipeline->blendConstants[index] = info.pColorBlendState->blendConstants[index];
#if LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE_COMPILED
		if (MetalDepthStencilStateCacheSelectorRequested() &&
		    info.pDynamicState != nullptr &&
		    info.pDynamicState->pDynamicStates != nullptr)
		{
			for (uint32_t dynamicIndex = 0;
			     dynamicIndex < info.pDynamicState->dynamicStateCount;
			     ++dynamicIndex)
			{
				switch (info.pDynamicState->pDynamicStates[dynamicIndex])
				{
					case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
						pipeline->stencilCompareMaskDynamic = true;
						break;
					case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
						pipeline->stencilWriteMaskDynamic = true;
						break;
					case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
						pipeline->stencilReferenceDynamic = true;
						break;
					default: break;
				}
			}
		}
#endif
		if (info.pDepthStencilState != nullptr)
		{
			pipeline->depthStencil = *info.pDepthStencilState;
			pipeline->hasDepthStencil = true;
			if (pipeline->depthStencil.depthBoundsTestEnable)
				return VK_ERROR_FEATURE_NOT_PRESENT;
		}

		id<MTLFunction> vertexFunction = nil;
		id<MTLFunction> fragmentFunction = nil;
		std::vector<StageBinding> vertexBindings;
		std::vector<StageBinding> fragmentBindings;
		pipeline->passReuseResourcesReadOnly = CompletionPassReuseRequested();
		for (uint32_t stageIndex = 0; stageIndex < info.stageCount; ++stageIndex)
		{
			const auto& stage = info.pStages[stageIndex];
			if (stage.pSpecializationInfo != nullptr || stage.module == VK_NULL_HANDLE)
				return VK_ERROR_FEATURE_NOT_PRESENT;
			VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
			if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT)
				result = CompileMetalFunction(device, stage.module, info.layout, stage, &vertexFunction, &vertexBindings, nullptr,
				                              &pipeline->vertexPushConstantSize, vertexInputFourComponentMask, flipVertexY);
			else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT)
				result = CompileMetalFunction(device, stage.module, info.layout, stage, &fragmentFunction, &fragmentBindings, nullptr,
				                              &pipeline->fragmentPushConstantSize, 0, false);
			if (result != VK_SUCCESS) return result;
			if (pipeline->passReuseResourcesReadOnly && !ShaderResourcesAllowPassReuse(stage))
				pipeline->passReuseResourcesReadOnly = false;
		}
		if (vertexFunction == nil) return VK_ERROR_INVALID_SHADER_NV;
		if (viewportDynamic)
		{
			for (uint32_t stageIndex = 0; stageIndex < info.stageCount; ++stageIndex)
			{
				const auto& stage = info.pStages[stageIndex];
				if (stage.stage != VK_SHADER_STAGE_VERTEX_BIT) continue;
				std::vector<StageBinding> flippedBindings;
				id<MTLFunction> flippedFunction = nil;
				const auto result = CompileMetalFunction(device, stage.module, info.layout, stage,
				    &flippedFunction, &flippedBindings, nullptr, &pipeline->vertexPushConstantSize, vertexInputFourComponentMask, true);
				if (result != VK_SUCCESS) return result;
				pipeline->vertexFunctionFlipped = flippedFunction;
			}
		}
		pipeline->stageBindings = vertexBindings;
		pipeline->stageBindings.insert(pipeline->stageBindings.end(), fragmentBindings.begin(), fragmentBindings.end());
		pipeline->vertexBufferBase = MetalBufferCount(vertexBindings);

		MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
		descriptor.vertexFunction = vertexFunction;
		descriptor.fragmentFunction = fragmentFunction;
		descriptor.rasterSampleCount = info.pMultisampleState->rasterizationSamples;
		// Vulkan's FRONT_AND_BACK mode discards every polygon. Metal has no
		// equivalent MTLCullMode value; disabling rasterization for triangle
		// topologies preserves the result instead of incorrectly translating it
		// to MTLCullModeNone. Face culling does not apply to point/line topology.
		const bool triangleTopology = pipeline->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
		                              pipeline->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
		                              pipeline->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
		descriptor.rasterizationEnabled = !(triangleTopology && rasterization.cullMode == VK_CULL_MODE_FRONT_AND_BACK);
		MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];
		for (uint32_t index = 0; index < info.pVertexInputState->vertexBindingDescriptionCount; ++index)
		{
			const auto& binding = info.pVertexInputState->pVertexBindingDescriptions[index];
			if (binding.binding >= 16 || binding.inputRate != VK_VERTEX_INPUT_RATE_VERTEX)
				return VK_ERROR_FEATURE_NOT_PRESENT;
			pipeline->vertexBindingUsed[binding.binding] = true;
			const uint32_t metalIndex = pipeline->vertexBufferBase + binding.binding;
			if (metalIndex >= kPushConstantBufferIndex) return VK_ERROR_FEATURE_NOT_PRESENT;
			vertexDescriptor.layouts[metalIndex].stride = binding.stride;
			vertexDescriptor.layouts[metalIndex].stepFunction = binding.stride == 0 ? MTLVertexStepFunctionConstant : MTLVertexStepFunctionPerVertex;
			vertexDescriptor.layouts[metalIndex].stepRate = 1;
		}
		for (uint32_t index = 0; index < info.pVertexInputState->vertexAttributeDescriptionCount; ++index)
		{
			const auto& attribute = info.pVertexInputState->pVertexAttributeDescriptions[index];
			const MTLVertexFormat format = ToMTLVertexFormat(attribute.format);
			if (attribute.location >= 31 || attribute.binding >= 16 || format == MTLVertexFormatInvalid)
				return VK_ERROR_FORMAT_NOT_SUPPORTED;
			vertexDescriptor.attributes[attribute.location].format = format;
			vertexDescriptor.attributes[attribute.location].offset = attribute.offset;
			const uint32_t metalIndex = pipeline->vertexBufferBase + attribute.binding;
			if (metalIndex >= kPushConstantBufferIndex) return VK_ERROR_FEATURE_NOT_PRESENT;
			vertexDescriptor.attributes[attribute.location].bufferIndex = metalIndex;
		}
		descriptor.vertexDescriptor = vertexDescriptor;
		std::vector<VkFormat> colorFormats;
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
		if (info.renderPass != VK_NULL_HANDLE)
		{
			const auto& pass = *info.renderPass;
			if (pass.colorAttachment.attachment != VK_ATTACHMENT_UNUSED)
			{
				if (pass.colorAttachment.attachment >= pass.attachments.size()) return VK_ERROR_INITIALIZATION_FAILED;
				colorFormats.push_back(pass.attachments[pass.colorAttachment.attachment].format);
			}
			if (pass.hasDepthAttachment)
			{
				if (pass.depthAttachment.attachment >= pass.attachments.size()) return VK_ERROR_INITIALIZATION_FAILED;
				const auto format = pass.attachments[pass.depthAttachment.attachment].format;
				if (IsDepthFormat(format)) depthFormat = format;
				if (HasStencil(format)) stencilFormat = format;
			}
		}
		else
		{
			if (rendering->viewMask != 0 || rendering->colorAttachmentCount > 8 ||
			    (rendering->colorAttachmentCount != 0 && rendering->pColorAttachmentFormats == nullptr))
				return VK_ERROR_FEATURE_NOT_PRESENT;
			if (rendering->colorAttachmentCount != 0)
				colorFormats.assign(rendering->pColorAttachmentFormats,
				                    rendering->pColorAttachmentFormats + rendering->colorAttachmentCount);
			depthFormat = rendering->depthAttachmentFormat;
			stencilFormat = rendering->stencilAttachmentFormat;
		}
		if (info.pColorBlendState->attachmentCount != colorFormats.size() ||
		    (!colorFormats.empty() && info.pColorBlendState->pAttachments == nullptr))
			return VK_ERROR_INITIALIZATION_FAILED;
		for (size_t i = 0; i < colorFormats.size(); ++i)
		{
			if (colorFormats[i] == VK_FORMAT_UNDEFINED) continue;
			const auto& blend = info.pColorBlendState->pAttachments[i];
			auto* color = descriptor.colorAttachments[i];
			color.pixelFormat = Lumen::MetalVulkan::ToMTLPixelFormat(colorFormats[i]);
			if (color.pixelFormat == MTLPixelFormatInvalid) return VK_ERROR_FORMAT_NOT_SUPPORTED;
			color.writeMask = ToMTLColorWriteMask(blend.colorWriteMask);
			color.blendingEnabled = blend.blendEnable == VK_TRUE;
			color.sourceRGBBlendFactor = ToMTLBlendFactor(blend.srcColorBlendFactor);
			color.destinationRGBBlendFactor = ToMTLBlendFactor(blend.dstColorBlendFactor);
			color.rgbBlendOperation = ToMTLBlendOperation(blend.colorBlendOp);
			color.sourceAlphaBlendFactor = ToMTLBlendFactor(blend.srcAlphaBlendFactor);
			color.destinationAlphaBlendFactor = ToMTLBlendFactor(blend.dstAlphaBlendFactor);
			color.alphaBlendOperation = ToMTLBlendOperation(blend.alphaBlendOp);
		}
		if (depthFormat != VK_FORMAT_UNDEFINED) descriptor.depthAttachmentPixelFormat = Lumen::MetalVulkan::ToMTLPixelFormat(depthFormat);
		if (stencilFormat != VK_FORMAT_UNDEFINED) descriptor.stencilAttachmentPixelFormat = Lumen::MetalVulkan::ToMTLPixelFormat(stencilFormat);
		pipeline->renderDescriptor = [descriptor copy];
		NSError* error = nil;
		pipeline->renderState = [device->metalDevice newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (pipeline->renderState == nil)
		{
			PrintMetalError("newRenderPipelineState", error);
			return VK_ERROR_INVALID_SHADER_NV;
		}
		MTLRenderPipelineDescriptor* noColorDescriptor = [descriptor copy];
		for (size_t i = 0; i < colorFormats.size(); ++i) noColorDescriptor.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
		pipeline->renderStateNoColor = [device->metalDevice newRenderPipelineStateWithDescriptor:noColorDescriptor error:&error];
		if (pipeline->renderStateNoColor == nil)
		{
			PrintMetalError("newRenderPipelineState(no-color)", error);
			return VK_ERROR_INVALID_SHADER_NV;
		}
		pipelines[pipelineIndex] = pipeline.release();
	}
	return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache,
	                                                    uint32_t createInfoCount,
	                                                    const VkComputePipelineCreateInfo* createInfos,
	                                                    const VkAllocationCallbacks* allocator,
	                                                    VkPipeline* pipelines)
{
	if (device == VK_NULL_HANDLE || (pipelineCache != VK_NULL_HANDLE && pipelineCache->device != device) ||
	    createInfoCount == 0 || createInfos == nullptr ||
	    pipelines == nullptr || allocator != nullptr)
		return VK_ERROR_INITIALIZATION_FAILED;
	std::fill(pipelines, pipelines + createInfoCount, VK_NULL_HANDLE);
	for (uint32_t index = 0; index < createInfoCount; ++index)
	{
		const auto& info = createInfos[index];
		if (info.sType != VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO || info.layout == VK_NULL_HANDLE ||
		    info.stage.stage != VK_SHADER_STAGE_COMPUTE_BIT || info.stage.module == VK_NULL_HANDLE ||
		    info.stage.pSpecializationInfo != nullptr)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		auto pipeline = std::unique_ptr<VkPipeline_T>(new (std::nothrow) VkPipeline_T);
		if (!pipeline) return VK_ERROR_OUT_OF_HOST_MEMORY;
		pipeline->device = device;
		pipeline->layout = info.layout;
		pipeline->bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
		id<MTLFunction> function = nil;
		const VkResult result = CompileMetalFunction(device, info.stage.module, info.layout, info.stage, &function,
		                                             &pipeline->stageBindings, &pipeline->computeThreads,
		                                             &pipeline->computePushConstantSize, 0, false);
		if (result != VK_SUCCESS) return result;
		NSError* error = nil;
		pipeline->computeState = [device->metalDevice newComputePipelineStateWithFunction:function error:&error];
		if (pipeline->computeState == nil)
		{
			PrintMetalError("newComputePipelineState", error);
			return VK_ERROR_INVALID_SHADER_NV;
		}
		pipelines[index] = pipeline.release();
	}
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks* allocator)
{
	if (pipeline == VK_NULL_HANDLE || allocator != nullptr) return;
	pipeline->alive.store(false, std::memory_order_release);
	delete pipeline;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, VkPipeline pipeline)
{
	if (pipeline == VK_NULL_HANDLE || pipeline->bindPoint != bindPoint)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
	{
		state->graphicsPipeline = pipeline;
		// Conservative lifetime boundary: after binding a static-scissor pipeline,
		// a later dynamic pipeline requires a new setter in this command buffer.
		if (!pipeline->scissorDynamic) state->scissorSet = false;
		commandBuffer->recordingPipeline = pipeline;
	} else if (bindPoint == VK_PIPELINE_BIND_POINT_COMPUTE)
	{
		state->computePipeline = pipeline;
	} else
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	if (TryElideMetalStateOnlyRecord(commandBuffer, __func__)) return;
	auto snapshot = std::make_shared<ReplayState>(*state);
	Record(commandBuffer, __func__, [snapshot, bindPoint](Lumen::MetalVulkan::EncodingContext& context)
	{
		return bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? ApplyGraphicsState(context, snapshot)
		                                                   : ApplyComputeState(context, snapshot);
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint,
	                                               VkPipelineLayout, uint32_t firstSet, uint32_t descriptorSetCount,
	                                               const VkDescriptorSet* descriptorSets, uint32_t dynamicOffsetCount,
	                                               const uint32_t*)
{
	if ((descriptorSetCount != 0 && descriptorSets == nullptr) || dynamicOffsetCount != 0 || firstSet + descriptorSetCount > 8)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	if (bindPoint != VK_PIPELINE_BIND_POINT_GRAPHICS && bindPoint != VK_PIPELINE_BIND_POINT_COMPUTE)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	if (ValidateCommandBufferForRecording(commandBuffer) != VK_SUCCESS)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	auto& sets = bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS ? state->graphicsSets : state->computeSets;
	for (uint32_t index = 0; index < descriptorSetCount; ++index)
		sets[firstSet + index] = descriptorSets[index];
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer commandBuffer, VkPipelineLayout,
	                                          VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size,
	                                          const void* values)
{
	if ((size != 0 && values == nullptr) || offset > 4096 || size > 4096 - offset)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	auto update = [offset, size, values](std::vector<uint8_t>& destination)
	{
		const uint32_t paddedSize = (offset + size + 15u) & ~15u;
		if (destination.size() < paddedSize) destination.resize(paddedSize, 0);
		if (size != 0) std::memcpy(destination.data() + offset, values, size);
	};
	if ((stageFlags & VK_SHADER_STAGE_VERTEX_BIT) != 0) update(state->vertexPushConstants);
	if ((stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT) != 0) update(state->fragmentPushConstants);
	if ((stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) != 0) update(state->computePushConstants);
	if (TryElideMetalStateOnlyRecord(commandBuffer, __func__)) return;
	auto snapshot = std::make_shared<ReplayState>(*state);
	Record(commandBuffer, __func__, [snapshot, stageFlags](Lumen::MetalVulkan::EncodingContext& context)
	{
		if ((stageFlags & (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) != 0)
		{
			const VkResult result = ApplyGraphicsState(context, snapshot);
			if (result != VK_SUCCESS) return result;
		}
		return (stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) != 0 ? ApplyComputeState(context, snapshot) : VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer commandBuffer, uint32_t firstBinding,
	                                              uint32_t bindingCount, const VkBuffer* buffers,
	                                              const VkDeviceSize* offsets)
{
	if ((bindingCount != 0 && (buffers == nullptr || offsets == nullptr)) || firstBinding + bindingCount > 16)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	for (uint32_t index = 0; index < bindingCount; ++index)
	{
		state->vertexBuffers[firstBinding + index] = buffers[index];
		state->vertexOffsets[firstBinding + index] = offsets[index];
	}
	if (TryElideMetalStateOnlyRecord(commandBuffer, __func__)) return;
	auto snapshot = std::make_shared<ReplayState>(*state);
	Record(commandBuffer, __func__, [snapshot](Lumen::MetalVulkan::EncodingContext& context) { return ApplyGraphicsState(context, snapshot); });
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer commandBuffer, uint32_t firstBinding,
    uint32_t bindingCount, const VkBuffer* buffers, const VkDeviceSize* offsets,
    const VkDeviceSize* sizes, const VkDeviceSize* strides)
{
	if (firstBinding > 16 || bindingCount > 16 - firstBinding ||
	    (bindingCount != 0 && (buffers == nullptr || offsets == nullptr)))
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	for (uint32_t i = 0; i < bindingCount; ++i)
		if (buffers[i] == VK_NULL_HANDLE || offsets[i] > buffers[i]->size ||
		    (sizes != nullptr && sizes[i] != VK_WHOLE_SIZE && sizes[i] > buffers[i]->size - offsets[i]))
		{
			RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
			return;
		}
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	if (strides != nullptr)
		for (uint32_t i = 0; i < bindingCount; ++i) state->vertexStrides[firstBinding + i] = strides[i];
	vkCmdBindVertexBuffers(commandBuffer, firstBinding, bindingCount, buffers, offsets);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer commandBuffer, uint32_t first,
                                          uint32_t count, const VkViewport* values)
{
	if (first != 0 || count != 1 || values == nullptr || values[0].width <= 0 || values[0].height == 0)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	if (auto state = GetReplayState(commandBuffer)) { state->viewport = values[0]; state->viewportSet = true; }
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer commandBuffer, uint32_t count, const VkViewport* values)
{ vkCmdSetViewport(commandBuffer, 0, count, values); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer commandBuffer, uint32_t count, const VkRect2D* values)
{ vkCmdSetScissor(commandBuffer, 0, count, values); }

#define LUMEN_DYNAMIC_SETTER(function, type, member) \
    VKAPI_ATTR void VKAPI_CALL function(VkCommandBuffer commandBuffer, type value) \
    { if (auto state = GetReplayState(commandBuffer)) state->member = value; }
LUMEN_DYNAMIC_SETTER(vkCmdSetDepthTestEnable, VkBool32, depthTest)
LUMEN_DYNAMIC_SETTER(vkCmdSetDepthWriteEnable, VkBool32, depthWrite)
LUMEN_DYNAMIC_SETTER(vkCmdSetDepthCompareOp, VkCompareOp, depthCompare)
LUMEN_DYNAMIC_SETTER(vkCmdSetStencilTestEnable, VkBool32, stencilTest)
LUMEN_DYNAMIC_SETTER(vkCmdSetDepthBiasEnable, VkBool32, depthBiasEnable)
LUMEN_DYNAMIC_SETTER(vkCmdSetCullMode, VkCullModeFlags, cullMode)
LUMEN_DYNAMIC_SETTER(vkCmdSetFrontFace, VkFrontFace, frontFace)
LUMEN_DYNAMIC_SETTER(vkCmdSetRasterizerDiscardEnable, VkBool32, rasterizerDiscard)
LUMEN_DYNAMIC_SETTER(vkCmdSetPrimitiveRestartEnable, VkBool32, primitiveRestart)
LUMEN_DYNAMIC_SETTER(vkCmdSetPrimitiveTopology, VkPrimitiveTopology, topology)
#undef LUMEN_DYNAMIC_SETTER

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer commandBuffer, VkBool32 enabled)
{
	if (enabled) RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer commandBuffer, float constant, float clamp, float slope)
{
	if (auto state = GetReplayState(commandBuffer)) {
		state->depthBiasConstant = constant; state->depthBiasSlope = slope; state->depthBiasClamp = clamp;
	}
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer commandBuffer, const float values[4])
{
	if (values == nullptr) { RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return; }
	if (auto state = GetReplayState(commandBuffer)) std::copy(values, values + 4, state->blendConstants.begin());
}
VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer commandBuffer, VkStencilFaceFlags faces,
    VkStencilOp fail, VkStencilOp pass, VkStencilOp depthFail, VkCompareOp compare)
{
	if (auto state = GetReplayState(commandBuffer)) {
		const auto set = [&](VkStencilOpState& target) {
			target.failOp = fail; target.passOp = pass; target.depthFailOp = depthFail; target.compareOp = compare;
		};
		if (faces & VK_STENCIL_FACE_FRONT_BIT) set(state->frontStencil);
		if (faces & VK_STENCIL_FACE_BACK_BIT) set(state->backStencil);
	}
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
	                                            VkIndexType indexType)
{
	if (buffer == VK_NULL_HANDLE || (indexType != VK_INDEX_TYPE_UINT16 && indexType != VK_INDEX_TYPE_UINT32) ||
	    (buffer->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) == 0 || offset >= buffer->size ||
	    (offset % IndexTypeSize(indexType)) != 0)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	state->indexBuffer = buffer;
	state->indexOffset = offset;
	state->indexType = indexType;
	commandBuffer->recordingIndexBuffer = buffer;
	commandBuffer->recordingIndexOffset = offset;
	commandBuffer->recordingIndexType = indexType;
}

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
	                                 uint32_t firstVertex, uint32_t firstInstance)
{
	auto recordingState = GetReplayState(commandBuffer);
	if (!recordingState || recordingState->graphicsPipeline == VK_NULL_HANDLE) { RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return; }
	if (recordingState->graphicsPipeline->scissorDynamic && !recordingState->scissorSet)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	auto state = std::make_shared<ReplayState>(*recordingState);
	Record(commandBuffer, __func__, [state, vertexCount, instanceCount, firstVertex, firstInstance](Lumen::MetalVulkan::EncodingContext& context)
	{
		if (context.renderEncoder == nil || !context.logicalRenderPassActive) return VK_ERROR_INITIALIZATION_FAILED;
		const VkResult prepared = PrepareReusablePassDraw(context, state);
		if (prepared != VK_SUCCESS) return prepared;
		const VkResult result = ApplyGraphicsState(context, state, true);
		if (result != VK_SUCCESS) return result;
		ObserveMetalStateSnapshotConsumer();
		if ((state->graphicsPipeline->dynamicStates.count(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) ? state->topology : state->graphicsPipeline->topology) == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
		{
			if (vertexCount < 3 || instanceCount == 0) return VK_SUCCESS;
			id<MTLBuffer> fanIndexBuffer = nil;
			NSUInteger fanIndexCount = 0;
			const VkResult fanResult = CreateNonIndexedFanBuffer(state->graphicsPipeline->device, vertexCount,
			                                                    firstVertex, &fanIndexBuffer, &fanIndexCount);
			if (fanResult != VK_SUCCESS) return fanResult;
			[context.renderEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
			                                 indexCount:fanIndexCount indexType:MTLIndexTypeUInt32
			                               indexBuffer:fanIndexBuffer indexBufferOffset:0
			                               instanceCount:instanceCount baseVertex:0 baseInstance:firstInstance];
			return VK_SUCCESS;
		}
		[context.renderEncoder drawPrimitives:ToMTLPrimitiveType((state->graphicsPipeline->dynamicStates.count(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) ? state->topology : state->graphicsPipeline->topology))
		                           vertexStart:firstVertex vertexCount:vertexCount instanceCount:instanceCount
		                            baseInstance:firstInstance];
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
	                                        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance)
{
	auto recordingState = GetReplayState(commandBuffer);
	if (!recordingState || recordingState->graphicsPipeline == VK_NULL_HANDLE || recordingState->indexBuffer == VK_NULL_HANDLE)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	if (recordingState->graphicsPipeline->scissorDynamic && !recordingState->scissorSet)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	auto state = std::make_shared<ReplayState>(*recordingState);
	Record(commandBuffer, __func__, [state, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance]
	       (Lumen::MetalVulkan::EncodingContext& context)
	{
		if (context.renderEncoder == nil || !context.logicalRenderPassActive || state->indexBuffer->metalBuffer == nil)
			return VK_ERROR_INITIALIZATION_FAILED;
		const VkResult prepared = PrepareReusablePassDraw(context, state);
		if (prepared != VK_SUCCESS) return prepared;
		const VkResult result = ApplyGraphicsState(context, state, true);
		if (result != VK_SUCCESS) return result;
		ObserveMetalStateSnapshotConsumer();
		const VkDeviceSize indexSize = IndexTypeSize(state->indexType);
		const VkDeviceSize firstIndexOffset = static_cast<VkDeviceSize>(firstIndex) * indexSize;
		if (firstIndexOffset > std::numeric_limits<VkDeviceSize>::max() - state->indexOffset)
			return VK_ERROR_INITIALIZATION_FAILED;
		const VkDeviceSize logicalOffset = state->indexOffset + firstIndexOffset;
		const VkDeviceSize logicalSize = static_cast<VkDeviceSize>(indexCount) * indexSize;
		if (!BufferRangeInBounds(state->indexBuffer, logicalOffset, logicalSize))
			return VK_ERROR_INITIALIZATION_FAILED;
		if ((state->graphicsPipeline->dynamicStates.count(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) ? state->topology : state->graphicsPipeline->topology) == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
		{
			if (indexCount < 3 || instanceCount == 0) return VK_SUCCESS;
			id<MTLBuffer> fanIndexBuffer = nil;
			NSUInteger fanIndexCount = 0;
			const VkResult fanResult = CreateIndexedFanBuffer(state->graphicsPipeline->device, state->indexBuffer,
			                                                 logicalOffset, indexCount, state->indexType,
			                                                 &fanIndexBuffer, &fanIndexCount);
			if (fanResult != VK_SUCCESS) return fanResult;
			[context.renderEncoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
			                                 indexCount:fanIndexCount indexType:ToMTLIndexType(state->indexType)
			                               indexBuffer:fanIndexBuffer indexBufferOffset:0
			                               instanceCount:instanceCount baseVertex:vertexOffset baseInstance:firstInstance];
			return VK_SUCCESS;
		}
		const VkDeviceSize metalOffset = BufferMetalOffset(state->indexBuffer, logicalOffset);
		if (metalOffset > std::numeric_limits<NSUInteger>::max()) return VK_ERROR_INITIALIZATION_FAILED;
		const NSUInteger offset = static_cast<NSUInteger>(metalOffset);
		[context.renderEncoder drawIndexedPrimitives:ToMTLPrimitiveType((state->graphicsPipeline->dynamicStates.count(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) ? state->topology : state->graphicsPipeline->topology))
		                                 indexCount:indexCount indexType:ToMTLIndexType(state->indexType)
		                               indexBuffer:state->indexBuffer->metalBuffer indexBufferOffset:offset
		                               instanceCount:instanceCount baseVertex:vertexOffset baseInstance:firstInstance];
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX,
	                                     uint32_t groupCountY, uint32_t groupCountZ)
{
	auto recordingState = GetReplayState(commandBuffer);
	if (!recordingState || recordingState->computePipeline == VK_NULL_HANDLE) { RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return; }
	auto state = std::make_shared<ReplayState>(*recordingState);
	Record(commandBuffer, __func__, [state, groupCountX, groupCountY, groupCountZ](Lumen::MetalVulkan::EncodingContext& context)
	{
		Lumen::MetalVulkan::EndEncoding(context);
		context.computeEncoder = [context.commandBuffer computeCommandEncoder];
		if (context.computeEncoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		const VkResult result = ApplyComputeState(context, state, true);
		if (result != VK_SUCCESS) return result;
		ObserveMetalStateSnapshotConsumer();
		if (groupCountX != 0 && groupCountY != 0 && groupCountZ != 0)
		{
			const VkResult invalidate = InvalidateComputeStorageShadows(state);
			if (invalidate != VK_SUCCESS) return invalidate;
		}
		[context.computeEncoder dispatchThreadgroups:MTLSizeMake(groupCountX, groupCountY, groupCountZ)
		                         threadsPerThreadgroup:state->computePipeline->computeThreads];
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset)
{
	auto recording = GetReplayState(commandBuffer);
	if (!recording || recording->computePipeline == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE ||
	    !BufferRangeInBounds(buffer, offset, sizeof(VkDispatchIndirectCommand))) {
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return;
	}
	auto state = std::make_shared<ReplayState>(*recording);
	Record(commandBuffer, __func__, [state, buffer, offset](Lumen::MetalVulkan::EncodingContext& context) -> VkResult {
		Lumen::MetalVulkan::EndEncoding(context);
		context.computeEncoder = [context.commandBuffer computeCommandEncoder];
		if (context.computeEncoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		if (const auto result = ApplyComputeState(context, state, true); result != VK_SUCCESS) return result;
		[context.computeEncoder dispatchThreadgroupsWithIndirectBuffer:buffer->metalBuffer
		                                         indirectBufferOffset:BufferMetalOffset(buffer, offset)
		                                        threadsPerThreadgroup:state->computePipeline->computeThreads];
		return VK_SUCCESS;
	});
}

static void DrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                         uint32_t count, uint32_t stride, bool indexed, VkBuffer countBuffer = VK_NULL_HANDLE, VkDeviceSize countOffset = 0)
{
	auto recording = GetReplayState(commandBuffer);
	const size_t commandSize = indexed ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
	const uint64_t size = count == 0 ? 0 : uint64_t(count - 1) * stride + commandSize;
	if (!recording || recording->graphicsPipeline == VK_NULL_HANDLE || buffer == VK_NULL_HANDLE ||
	    (indexed && recording->indexBuffer == VK_NULL_HANDLE) ||
	    (count > 1 && stride < commandSize) || !BufferRangeInBounds(buffer, offset, size) ||
        (countBuffer != VK_NULL_HANDLE && !BufferRangeInBounds(countBuffer, countOffset, sizeof(uint32_t)))) {
		RecordError(commandBuffer, indexed ? "vkCmdDrawIndexedIndirect" : "vkCmdDrawIndirect", VK_ERROR_INITIALIZATION_FAILED); return;
	}
	auto state = std::make_shared<ReplayState>(*recording);
	Record(commandBuffer, indexed ? "vkCmdDrawIndexedIndirect" : "vkCmdDrawIndirect",
	       [state, buffer, offset, count, stride, indexed, countBuffer, countOffset](Lumen::MetalVulkan::EncodingContext& context) -> VkResult {
		if (context.renderEncoder == nil || !context.logicalRenderPassActive) return VK_ERROR_INITIALIZATION_FAILED;
		if ((state->graphicsPipeline->dynamicStates.count(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) ? state->topology : state->graphicsPipeline->topology) == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) return VK_ERROR_FEATURE_NOT_PRESENT;
        if (count == 0) return VK_SUCCESS;
        id<MTLBuffer> arguments = buffer->metalBuffer;
        VkDeviceSize argumentsBase = BufferMetalOffset(buffer, offset);
        uint32_t argumentStride = stride;
        if (countBuffer != VK_NULL_HANDLE) {
            // Count is a GPU-produced value. Mask excess commands on the GPU;
            // never inspect it through a host mapping or submit a guessed count.
            static std::mutex cacheMutex;
            static std::unordered_map<uintptr_t, id<MTLComputePipelineState>> cache;
            const auto device = buffer->device;
            id<MTLComputePipelineState> maskPipeline = nil;
            {
                std::lock_guard lock(cacheMutex);
                auto& cached = cache[reinterpret_cast<uintptr_t>((__bridge void*)device->metalDevice)];
                if (cached == nil) {
                    NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                        "kernel void mask_args(device const uint* src [[buffer(0)]],device const uint* count [[buffer(1)]],device uint* dst [[buffer(2)]],constant uint2& layout [[buffer(3)]],uint i [[thread_position_in_grid]]) {"
                        "for(uint j=0;j<layout.y;++j)dst[i*layout.y+j]=i<count[0]?src[i*layout.x+j]:0;}";
                    NSError* error = nil;
                    id<MTLLibrary> library = [device->metalDevice newLibraryWithSource:source options:nil error:&error];
                    if (library == nil) { PrintMetalError("indirect count shader", error); return VK_ERROR_INVALID_SHADER_NV; }
                    cached = [device->metalDevice newComputePipelineStateWithFunction:[library newFunctionWithName:@"mask_args"] error:&error];
                    if (cached == nil) { PrintMetalError("indirect count pipeline", error); return VK_ERROR_INVALID_SHADER_NV; }
                }
                maskPipeline = cached;
            }
            MTLRenderPassDescriptor* pass = [context.retainedRenderDescriptor copy];
            if (pass == nil) return VK_ERROR_INITIALIZATION_FAILED;
            const auto area = context.retainedRenderArea;
            for (NSUInteger i=0;i<8;++i) if (pass.colorAttachments[i].texture != nil) {
                pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
            }
            if (pass.depthAttachment.texture != nil) pass.depthAttachment.loadAction = MTLLoadActionLoad;
            if (pass.stencilAttachment.texture != nil) pass.stencilAttachment.loadAction = MTLLoadActionLoad;
            Lumen::MetalVulkan::EndEncoding(context, true);
            const uint32_t words = indexed ? 5 : 4;
            arguments = [device->metalDevice newBufferWithLength:uint64_t(count)*words*4 options:MTLResourceStorageModePrivate];
            auto encoder = [context.commandBuffer computeCommandEncoder];
            if (arguments == nil || encoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            [encoder setComputePipelineState:maskPipeline];
            [encoder setBuffer:buffer->metalBuffer offset:argumentsBase atIndex:0];
            [encoder setBuffer:countBuffer->metalBuffer offset:BufferMetalOffset(countBuffer,countOffset) atIndex:1];
            [encoder setBuffer:arguments offset:0 atIndex:2];
            const uint32_t layout[2] = {stride/4,words};
            [encoder setBytes:layout length:sizeof(layout) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(std::min<uint32_t>(count,64),1,1)];
            [encoder endEncoding];
            if (const auto result = Lumen::MetalVulkan::BeginRenderEncoding(context, pass); result != VK_SUCCESS) return result;
            context.logicalRenderPassActive = true;
            context.retainedRenderDescriptor = pass;
            context.retainedRenderArea = area;
            ++context.physicalRenderEncoders;
            argumentsBase = 0; argumentStride = words*4;
        }
		if (const auto result = PrepareReusablePassDraw(context, state); result != VK_SUCCESS) return result;
		if (const auto result = ApplyGraphicsState(context, state, true); result != VK_SUCCESS) return result;
		for (uint32_t i = 0; i < count; ++i) {
			const auto argumentOffset = argumentsBase + uint64_t(i) * argumentStride;
			const auto primitive = ToMTLPrimitiveType((state->graphicsPipeline->dynamicStates.count(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) ? state->topology : state->graphicsPipeline->topology));
			if (indexed)
				[context.renderEncoder drawIndexedPrimitives:primitive indexType:ToMTLIndexType(state->indexType)
				                               indexBuffer:state->indexBuffer->metalBuffer
				                         indexBufferOffset:BufferMetalOffset(state->indexBuffer, state->indexOffset)
				                            indirectBuffer:arguments indirectBufferOffset:argumentOffset];
			else [context.renderEncoder drawPrimitives:primitive indirectBuffer:arguments indirectBufferOffset:argumentOffset];
		}
		return VK_SUCCESS;
	});
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                            VkDeviceSize offset, uint32_t count, uint32_t stride)
{ DrawIndirect(commandBuffer, buffer, offset, count, stride, false); }
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                                   VkDeviceSize offset, uint32_t count, uint32_t stride)
{ DrawIndirect(commandBuffer, buffer, offset, count, stride, true); }

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                                 VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride)
{
    if (countBuffer == VK_NULL_HANDLE || (countOffset & 3) || (offset & 3) || (stride & 3)) {
        RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return;
    }
    DrawIndirect(commandBuffer, buffer, offset, maximum, stride, false, countBuffer, countOffset);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                                        VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride)
{
    if (countBuffer == VK_NULL_HANDLE || (countOffset & 3) || (offset & 3) || (stride & 3)) {
        RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return;
    }
    DrawIndirect(commandBuffer, buffer, offset, maximum, stride, true, countBuffer, countOffset);
}

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer commandBuffer, VkBuffer buffer,
                                          VkDeviceSize offset, VkDeviceSize size, uint32_t value)
{
	if (buffer == VK_NULL_HANDLE || offset > buffer->size) { RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return; }
	if (size == VK_WHOLE_SIZE) size = (buffer->size - offset) & ~VkDeviceSize(3);
	if ((offset & 3) != 0 || (size & 3) != 0 || !BufferRangeInBounds(buffer, offset, size)) {
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return;
	}
	Record(commandBuffer, __func__, [buffer, offset, size, value](Lumen::MetalVulkan::EncodingContext& context) -> VkResult {
		if (size == 0) return VK_SUCCESS;
		Lumen::MetalVulkan::EndEncoding(context);
		const size_t chunkSize = std::min<uint64_t>(size, 65536);
		std::vector<uint32_t> words(chunkSize / 4, value);
		id<MTLBuffer> source = [buffer->device->metalDevice newBufferWithBytes:words.data() length:chunkSize options:MTLResourceStorageModeShared];
		context.blitEncoder = [context.commandBuffer blitCommandEncoder];
		if (source == nil || context.blitEncoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		for (uint64_t done = 0; done < size; done += chunkSize)
			[context.blitEncoder copyFromBuffer:source sourceOffset:0 toBuffer:buffer->metalBuffer
			                  destinationOffset:BufferMetalOffset(buffer, offset + done) size:std::min<uint64_t>(chunkSize, size - done)];
		std::lock_guard lock(buffer->mutex);
		buffer->hostGpuWriteSeen = true;
		buffer->cpuShadowRanges.clear();
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* beginInfo,
	                                            VkSubpassContents contents)
{
	if (beginInfo == nullptr || beginInfo->renderPass == VK_NULL_HANDLE || beginInfo->framebuffer == VK_NULL_HANDLE ||
	    contents != VK_SUBPASS_CONTENTS_INLINE)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	VkRenderPass renderPass = beginInfo->renderPass;
	VkFramebuffer framebuffer = beginInfo->framebuffer;
	const VkRect2D renderArea = beginInfo->renderArea;
	const bool ordinaryBegin = beginInfo->pNext == nullptr;
	std::vector<VkClearValue> clearValues;
	if (beginInfo->clearValueCount != 0)
	{
		if (beginInfo->pClearValues == nullptr) { RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return; }
		clearValues.assign(beginInfo->pClearValues, beginInfo->pClearValues + beginInfo->clearValueCount);
	}
	Record(commandBuffer, __func__, [renderPass, framebuffer, renderArea, ordinaryBegin, clearValues = std::move(clearValues)]
	       (Lumen::MetalVulkan::EncodingContext& context)
	{
		if (context.logicalRenderPassActive) return VK_ERROR_INITIALIZATION_FAILED;
		if (!renderPass->alive.load(std::memory_order_acquire) || !framebuffer->alive.load(std::memory_order_acquire))
			return VK_ERROR_DEVICE_LOST;
		const bool eligible = renderPass->passReuseEligible && framebuffer->passReuseEligible && ordinaryBegin &&
		    renderArea.offset.x == 0 && renderArea.offset.y == 0 &&
		    renderArea.extent.width == framebuffer->width && renderArea.extent.height == framebuffer->height;
		bool reuse = eligible && context.renderPassReusable && context.renderEncoder != nil &&
		    context.retainedRenderPass == renderPass && context.retainedFramebuffer == framebuffer &&
		    context.retainedRenderArea.offset.x == renderArea.offset.x && context.retainedRenderArea.offset.y == renderArea.offset.y &&
		    context.retainedRenderArea.extent.width == renderArea.extent.width &&
		    context.retainedRenderArea.extent.height == renderArea.extent.height;
		if (!reuse) Lumen::MetalVulkan::EndEncoding(context);
		MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
		auto resolveView = [framebuffer](uint32_t attachment) -> id<MTLTexture>
		{
			if (attachment >= framebuffer->attachments.size() || framebuffer->attachments[attachment] == VK_NULL_HANDLE)
				return nil;
			VkImageView view = framebuffer->attachments[attachment];
			if (view->metalTexture == nil && view->image != VK_NULL_HANDLE)
				view->metalTexture = view->image->metalTexture;
			return view->metalTexture;
		};
		if (renderPass->colorAttachment.attachment != VK_ATTACHMENT_UNUSED)
		{
			const uint32_t attachment = renderPass->colorAttachment.attachment;
			if (attachment >= renderPass->attachments.size()) return VK_ERROR_INITIALIZATION_FAILED;
			auto* color = descriptor.colorAttachments[0];
			color.texture = resolveView(attachment);
			if (color.texture == nil) return VK_ERROR_INITIALIZATION_FAILED;
			const auto& source = renderPass->attachments[attachment];
			color.loadAction = ToMTLLoadAction(source.loadOp);
			color.storeAction = ToMTLStoreAction(source.storeOp);
			if (source.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && attachment < clearValues.size())
			{
				const auto& value = clearValues[attachment].color.float32;
				color.clearColor = MTLClearColorMake(value[0], value[1], value[2], value[3]);
			}
		}
		if (renderPass->hasDepthAttachment)
		{
			const uint32_t attachment = renderPass->depthAttachment.attachment;
			if (attachment >= renderPass->attachments.size()) return VK_ERROR_INITIALIZATION_FAILED;
			const auto& source = renderPass->attachments[attachment];
			id<MTLTexture> texture = resolveView(attachment);
			if (texture == nil) return VK_ERROR_INITIALIZATION_FAILED;
			if (IsDepthFormat(source.format))
			{
				descriptor.depthAttachment.texture = texture;
				descriptor.depthAttachment.loadAction = ToMTLLoadAction(source.loadOp);
				descriptor.depthAttachment.storeAction = ToMTLStoreAction(source.storeOp);
				if (source.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && attachment < clearValues.size())
					descriptor.depthAttachment.clearDepth = clearValues[attachment].depthStencil.depth;
			}
			if (HasStencil(source.format))
			{
				descriptor.stencilAttachment.texture = texture;
				descriptor.stencilAttachment.loadAction = ToMTLLoadAction(source.stencilLoadOp);
				descriptor.stencilAttachment.storeAction = ToMTLStoreAction(source.stencilStoreOp);
				if (source.stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && attachment < clearValues.size())
					descriptor.stencilAttachment.clearStencil = clearValues[attachment].depthStencil.stencil;
			}
		}
		// Swapchain view rebinding must not make an equal Vulkan handle reuse
		// a different native attachment. Ordinary image views are immutable.
		if (reuse && (descriptor.colorAttachments[0].texture != context.retainedRenderDescriptor.colorAttachments[0].texture ||
		              descriptor.depthAttachment.texture != context.retainedRenderDescriptor.depthAttachment.texture ||
		              descriptor.stencilAttachment.texture != context.retainedRenderDescriptor.stencilAttachment.texture))
		{
			reuse = false;
			Lumen::MetalVulkan::EndEncoding(context);
		}
		if (!reuse)
		{
			const auto result = Lumen::MetalVulkan::BeginRenderEncoding(context, descriptor);
			if (result != VK_SUCCESS) return result;
			++context.physicalRenderEncoders;
		}
		++context.logicalRenderPasses;
		context.logicalRenderPassActive = true;
		context.renderPassReusable = eligible;
		context.renderPassMergePending = reuse;
		context.renderPassContainsMergedDraws = false;
		context.renderPassMergeAfterMarkerPending = reuse && context.retainedCompletionMarker;
		context.renderPassMergedAfterMarker = false;
		context.retainedCompletionMarker = false;
		context.retainedRenderPass = renderPass;
		context.retainedFramebuffer = framebuffer;
		context.retainedRenderArea = renderArea;
		context.retainedRenderDescriptor = descriptor;
		MTLViewport viewport {static_cast<double>(renderArea.offset.x), static_cast<double>(renderArea.offset.y),
		                      static_cast<double>(renderArea.extent.width), static_cast<double>(renderArea.extent.height), 0.0, 1.0};
		[context.renderEncoder setViewport:viewport];
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* info)
{
	if (info == nullptr || info->sType != VK_STRUCTURE_TYPE_RENDERING_INFO || info->pNext != nullptr ||
	    info->flags != 0 || info->viewMask != 0 || info->layerCount == 0 || info->colorAttachmentCount > 8 ||
	    (info->colorAttachmentCount != 0 && info->pColorAttachments == nullptr))
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	std::vector<VkRenderingAttachmentInfo> colors;
	if (info->colorAttachmentCount != 0)
		colors.assign(info->pColorAttachments, info->pColorAttachments + info->colorAttachmentCount);
	const auto depth = info->pDepthAttachment ? *info->pDepthAttachment : VkRenderingAttachmentInfo{};
	const auto stencil = info->pStencilAttachment ? *info->pStencilAttachment : VkRenderingAttachmentInfo{};
	const auto area = info->renderArea;
	const auto layers = info->layerCount;
	Record(commandBuffer, __func__, [colors = std::move(colors), depth, stencil, area, layers]
	       (Lumen::MetalVulkan::EncodingContext& context) -> VkResult
	{
		if (context.logicalRenderPassActive) return VK_ERROR_INITIALIZATION_FAILED;
		Lumen::MetalVulkan::EndEncoding(context);
		MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
		descriptor.renderTargetArrayLength = layers;
		descriptor.renderTargetWidth = area.offset.x + area.extent.width;
		descriptor.renderTargetHeight = area.offset.y + area.extent.height;
		auto apply = [](const VkRenderingAttachmentInfo& source, MTLRenderPassAttachmentDescriptor* target) -> VkResult {
			if (source.imageView == VK_NULL_HANDLE) return VK_SUCCESS;
			if (source.pNext != nullptr || !source.imageView->alive.load(std::memory_order_acquire))
				return VK_ERROR_FEATURE_NOT_PRESENT;
			const auto view = source.imageView;
			target.texture = view->metalTexture != nil ? view->metalTexture : view->image->metalTexture;
			if (target.texture == nil) return VK_ERROR_INITIALIZATION_FAILED;
			target.loadAction = ToMTLLoadAction(source.loadOp);
			target.storeAction = ToMTLStoreAction(source.storeOp);
			if (source.resolveMode != VK_RESOLVE_MODE_NONE)
			{
				if (source.resolveMode != VK_RESOLVE_MODE_AVERAGE_BIT || source.resolveImageView == VK_NULL_HANDLE)
					return VK_ERROR_FEATURE_NOT_PRESENT;
				target.resolveTexture = source.resolveImageView->metalTexture;
				if (target.resolveTexture == nil) return VK_ERROR_INITIALIZATION_FAILED;
				target.storeAction = source.storeOp == VK_ATTACHMENT_STORE_OP_STORE
				                         ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionMultisampleResolve;
			}
			return VK_SUCCESS;
		};
		for (size_t i = 0; i < colors.size(); ++i)
		{
			auto* attachment = descriptor.colorAttachments[i];
			if (const auto result = apply(colors[i], attachment); result != VK_SUCCESS) return result;
			const auto* clear = colors[i].clearValue.color.float32;
			attachment.clearColor = MTLClearColorMake(clear[0], clear[1], clear[2], clear[3]);
		}
		if (depth.resolveMode != VK_RESOLVE_MODE_NONE || stencil.resolveMode != VK_RESOLVE_MODE_NONE)
			return VK_ERROR_FEATURE_NOT_PRESENT;
		if (const auto result = apply(depth, descriptor.depthAttachment); result != VK_SUCCESS) return result;
		if (const auto result = apply(stencil, descriptor.stencilAttachment); result != VK_SUCCESS) return result;
		descriptor.depthAttachment.clearDepth = depth.clearValue.depthStencil.depth;
		descriptor.stencilAttachment.clearStencil = stencil.clearValue.depthStencil.stencil;
		if (const auto result = Lumen::MetalVulkan::BeginRenderEncoding(context, descriptor); result != VK_SUCCESS) return result;
		context.logicalRenderPassActive = true;
		context.retainedRenderArea = area;
        context.retainedRenderDescriptor = descriptor;
		++context.logicalRenderPasses;
		++context.physicalRenderEncoders;
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer commandBuffer)
{
	Record(commandBuffer, __func__, [](Lumen::MetalVulkan::EncodingContext& context) -> VkResult {
		if (!context.logicalRenderPassActive) return VK_ERROR_INITIALIZATION_FAILED;
		Lumen::MetalVulkan::EndEncoding(context);
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
	Record(commandBuffer, __func__, [](Lumen::MetalVulkan::EncodingContext& context)
	{
		if (context.renderEncoder == nil || !context.logicalRenderPassActive) return VK_ERROR_INITIALIZATION_FAILED;
		if (context.renderPassReusable)
		{
			// An empty LOAD pass has no resource hazard and can also be retained.
			if (context.renderPassMergePending) ++context.mergedRenderPasses;
			// Empty LOAD passes do not claim a marker-crossing draw. Carry the
			// marker until a later compatible draw confirms the physical reuse.
			if (context.renderPassMergeAfterMarkerPending) context.retainedCompletionMarker = true;
			context.logicalRenderPassActive = false;
			context.renderPassMergePending = false;
			context.renderPassContainsMergedDraws = false;
			context.renderPassMergeAfterMarkerPending = false;
			context.renderPassMergedAfterMarker = false;
		} else
		{
			Lumen::MetalVulkan::EndEncoding(context);
		}
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer commandBuffer, VkBuffer source, VkBuffer destination,
	                                       uint32_t regionCount, const VkBufferCopy* regions)
{
	if (source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || regionCount == 0 || regions == nullptr)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	for (uint32_t index = 0; index < regionCount; ++index)
	{
		if (regions[index].size == 0 || !BufferRangeInBounds(source, regions[index].srcOffset, regions[index].size) ||
		    !BufferRangeInBounds(destination, regions[index].dstOffset, regions[index].size))
		{
			RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
			return;
		}
	}
	std::vector<VkBufferCopy> copies(regions, regions + regionCount);
	Record(commandBuffer, __func__, [source, destination, copies = std::move(copies)](Lumen::MetalVulkan::EncodingContext& context)
	{
		Lumen::MetalVulkan::EndEncoding(context);
		if (source->metalBuffer == nil || destination->metalBuffer == nil) return VK_ERROR_INITIALIZATION_FAILED;
		context.blitEncoder = [context.commandBuffer blitCommandEncoder];
		if (context.blitEncoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		for (const auto& copy: copies)
		{
			const VkResult mirrorResult = MirrorBufferCopy(source, destination, copy);
			if (mirrorResult != VK_SUCCESS) return mirrorResult;
			[context.blitEncoder copyFromBuffer:source->metalBuffer sourceOffset:BufferMetalOffset(source, copy.srcOffset)
			                             toBuffer:destination->metalBuffer destinationOffset:BufferMetalOffset(destination, copy.dstOffset)
			                                 size:copy.size];
		}
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer commandBuffer, VkBuffer source, VkImage destination,
	                                              VkImageLayout, uint32_t regionCount, const VkBufferImageCopy* regions)
{
	if (source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || regionCount == 0 || regions == nullptr)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	struct PlannedCopy
	{
		VkBufferImageCopy copy {};
		BufferImageTransfer transfer {};
		LinearLayout layout {};
	};
	std::vector<PlannedCopy> copies;
	copies.reserve(regionCount);
	for (uint32_t index = 0; index < regionCount; ++index)
	{
		PlannedCopy planned {};
		planned.copy = regions[index];
		const VkResult validation =
		    ValidateBufferImageCopy(source, destination, planned.copy, &planned.transfer, &planned.layout);
		if (validation != VK_SUCCESS)
		{
			RecordError(commandBuffer, __func__, validation);
			return;
		}
		copies.push_back(planned);
	}
	Record(commandBuffer, __func__, [source, destination, copies = std::move(copies)](Lumen::MetalVulkan::EncodingContext& context)
	{
		Lumen::MetalVulkan::EndEncoding(context);
		if (source->metalBuffer == nil || destination->metalTexture == nil) return VK_ERROR_INITIALIZATION_FAILED;
		context.blitEncoder = [context.commandBuffer blitCommandEncoder];
		if (context.blitEncoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		for (const auto& planned: copies)
		{
			const auto& copy = planned.copy;
			const auto& layout = planned.layout;
			if (copy.bufferOffset > std::numeric_limits<VkDeviceSize>::max() - source->memoryOffset)
				return VK_ERROR_INITIALIZATION_FAILED;
			const VkDeviceSize metalBase = BufferMetalOffset(source, copy.bufferOffset);
			if (metalBase > std::numeric_limits<NSUInteger>::max() || metalBase > source->metalBuffer.length ||
			    layout.requiredSpan > source->metalBuffer.length - metalBase)
				return VK_ERROR_INITIALIZATION_FAILED;
			for (uint32_t layer = 0; layer < copy.imageSubresource.layerCount; ++layer)
			{
				const VkDeviceSize layerOffset = metalBase + static_cast<VkDeviceSize>(layer) * layout.layerStride;
				const NSUInteger metalBytesPerImage = copy.imageExtent.depth > 1 ? layout.bytesPerImage : 0;
				[context.blitEncoder copyFromBuffer:source->metalBuffer
				                         sourceOffset:static_cast<NSUInteger>(layerOffset)
				                    sourceBytesPerRow:layout.bytesPerRow sourceBytesPerImage:metalBytesPerImage
				                         sourceSize:MTLSizeMake(copy.imageExtent.width, copy.imageExtent.height, copy.imageExtent.depth)
				                              toTexture:destination->metalTexture
				                       destinationSlice:copy.imageSubresource.baseArrayLayer + layer
				                       destinationLevel:copy.imageSubresource.mipLevel
				                      destinationOrigin:MTLOriginMake(copy.imageOffset.x, copy.imageOffset.y, copy.imageOffset.z)
				                               options:planned.transfer.options];
			}
		}
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer commandBuffer, VkImage source, VkImageLayout,
	                                              VkBuffer destination, uint32_t regionCount,
	                                              const VkBufferImageCopy* regions)
{
	if (source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || regionCount == 0 || regions == nullptr)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	struct PlannedCopy
	{
		VkBufferImageCopy copy {};
		BufferImageTransfer transfer {};
		LinearLayout layout {};
	};
	std::vector<PlannedCopy> copies;
	copies.reserve(regionCount);
	for (uint32_t index = 0; index < regionCount; ++index)
	{
		PlannedCopy planned {};
		planned.copy = regions[index];
		const VkResult validation =
		    ValidateBufferImageCopy(destination, source, planned.copy, &planned.transfer, &planned.layout);
		if (validation != VK_SUCCESS)
		{
			RecordError(commandBuffer, __func__, validation);
			return;
		}
		copies.push_back(planned);
	}
	Record(commandBuffer, __func__, [source, destination, copies = std::move(copies)](Lumen::MetalVulkan::EncodingContext& context)
	{
		Lumen::MetalVulkan::EndEncoding(context);
		if (source->metalTexture == nil || destination->metalBuffer == nil) return VK_ERROR_INITIALIZATION_FAILED;
		context.blitEncoder = [context.commandBuffer blitCommandEncoder];
		if (context.blitEncoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		for (const auto& planned: copies)
		{
			const auto& copy = planned.copy;
			const auto& layout = planned.layout;
			const VkResult invalidate = InvalidateGpuBufferWrite(destination, copy.bufferOffset, layout.requiredSpan);
			if (invalidate != VK_SUCCESS) return invalidate;
			if (copy.bufferOffset > std::numeric_limits<VkDeviceSize>::max() - destination->memoryOffset)
				return VK_ERROR_INITIALIZATION_FAILED;
			const VkDeviceSize metalBase = BufferMetalOffset(destination, copy.bufferOffset);
			if (metalBase > std::numeric_limits<NSUInteger>::max() || metalBase > destination->metalBuffer.length ||
			    layout.requiredSpan > destination->metalBuffer.length - metalBase)
				return VK_ERROR_INITIALIZATION_FAILED;
			for (uint32_t layer = 0; layer < copy.imageSubresource.layerCount; ++layer)
			{
				const VkDeviceSize layerOffset = metalBase + static_cast<VkDeviceSize>(layer) * layout.layerStride;
				const NSUInteger metalBytesPerImage = copy.imageExtent.depth > 1 ? layout.bytesPerImage : 0;
				[context.blitEncoder copyFromTexture:source->metalTexture
				                          sourceSlice:copy.imageSubresource.baseArrayLayer + layer
				                          sourceLevel:copy.imageSubresource.mipLevel
				                         sourceOrigin:MTLOriginMake(copy.imageOffset.x, copy.imageOffset.y, copy.imageOffset.z)
				                           sourceSize:MTLSizeMake(copy.imageExtent.width, copy.imageExtent.height, copy.imageExtent.depth)
				                             toBuffer:destination->metalBuffer
				                    destinationOffset:static_cast<NSUInteger>(layerOffset)
				               destinationBytesPerRow:layout.bytesPerRow destinationBytesPerImage:metalBytesPerImage
				                               options:planned.transfer.options];
			}
		}
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer commandBuffer, VkImage source, VkImageLayout,
	                                      VkImage destination, VkImageLayout, uint32_t regionCount,
	                                      const VkImageCopy* regions)
{
	if (source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || regionCount == 0 || regions == nullptr)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	std::vector<VkImageCopy> copies(regions, regions + regionCount);
	Record(commandBuffer, __func__, [source, destination, copies = std::move(copies)](Lumen::MetalVulkan::EncodingContext& context)
	{
		Lumen::MetalVulkan::EndEncoding(context);
		if (source->metalTexture == nil || destination->metalTexture == nil) return VK_ERROR_INITIALIZATION_FAILED;
		context.blitEncoder = [context.commandBuffer blitCommandEncoder];
		if (context.blitEncoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		for (const auto& copy: copies)
		{
			for (uint32_t layer = 0; layer < copy.srcSubresource.layerCount; ++layer)
			{
				[context.blitEncoder copyFromTexture:source->metalTexture
				                          sourceSlice:copy.srcSubresource.baseArrayLayer + layer sourceLevel:copy.srcSubresource.mipLevel
				                         sourceOrigin:MTLOriginMake(copy.srcOffset.x, copy.srcOffset.y, copy.srcOffset.z)
				                           sourceSize:MTLSizeMake(copy.extent.width, copy.extent.height, copy.extent.depth)
				                            toTexture:destination->metalTexture
				                     destinationSlice:copy.dstSubresource.baseArrayLayer + layer destinationLevel:copy.dstSubresource.mipLevel
				                    destinationOrigin:MTLOriginMake(copy.dstOffset.x, copy.dstOffset.y, copy.dstOffset.z)];
			}
		}
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdResolveImage(VkCommandBuffer commandBuffer, VkImage source, VkImageLayout,
                                            VkImage destination, VkImageLayout, uint32_t regionCount,
                                            const VkImageResolve* regions)
{
    if (source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || regions == nullptr || regionCount == 0 ||
        source->samples == VK_SAMPLE_COUNT_1_BIT || destination->samples != VK_SAMPLE_COUNT_1_BIT ||
        source->format != destination->format || IsDepthFormat(source->format) || HasStencil(source->format)) {
        RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return;
    }
    std::vector<VkImageResolve> copies(regions, regions + regionCount);
    Record(commandBuffer, __func__, [source, destination, copies = std::move(copies)]
           (Lumen::MetalVulkan::EncodingContext& context) -> VkResult {
        Lumen::MetalVulkan::EndEncoding(context);
        if (source->metalTexture == nil || destination->metalTexture == nil) return VK_ERROR_INITIALIZATION_FAILED;
        auto* textureInfo = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source->metalTexture.pixelFormat
                                                                            width:source->extent.width height:source->extent.height mipmapped:NO];
        textureInfo.usage = MTLTextureUsageRenderTarget; textureInfo.storageMode = MTLStorageModePrivate;
        id<MTLTexture> resolved = [source->device->metalDevice newTextureWithDescriptor:textureInfo];
        if (resolved == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        for (const auto& copy : copies) {
            if (copy.srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || copy.dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
                copy.srcSubresource.mipLevel != 0 || copy.srcSubresource.layerCount != copy.dstSubresource.layerCount ||
                copy.extent.depth != 1 || copy.srcOffset.z != 0 || copy.dstOffset.z != 0)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            for (uint32_t layer = 0; layer < copy.srcSubresource.layerCount; ++layer) {
                auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                auto* color = pass.colorAttachments[0];
                color.texture = source->metalTexture; color.slice = copy.srcSubresource.baseArrayLayer + layer;
                color.resolveTexture = resolved; color.loadAction = MTLLoadActionLoad;
                color.storeAction = MTLStoreActionStoreAndMultisampleResolve;
                id<MTLRenderCommandEncoder> encoder = [context.commandBuffer renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                [encoder endEncoding];
                id<MTLBlitCommandEncoder> blit = [context.commandBuffer blitCommandEncoder];
                if (blit == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
                [blit copyFromTexture:resolved sourceSlice:0 sourceLevel:0
                         sourceOrigin:MTLOriginMake(copy.srcOffset.x, copy.srcOffset.y, 0)
                           sourceSize:MTLSizeMake(copy.extent.width, copy.extent.height, 1)
                            toTexture:destination->metalTexture destinationSlice:copy.dstSubresource.baseArrayLayer + layer
                     destinationLevel:copy.dstSubresource.mipLevel
                    destinationOrigin:MTLOriginMake(copy.dstOffset.x, copy.dstOffset.y, 0)];
                [blit endEncoding];
            }
        }
        return VK_SUCCESS;
    });
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage source, VkImageLayout,
	                                      VkImage destination, VkImageLayout, uint32_t regionCount,
	                                      const VkImageBlit* regions, VkFilter filter)
{
	if (source == VK_NULL_HANDLE || destination == VK_NULL_HANDLE || regionCount == 0 || regions == nullptr ||
	    (filter != VK_FILTER_NEAREST && filter != VK_FILTER_LINEAR))
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	std::vector<VkImageBlit> blits(regions, regions + regionCount);
	Record(commandBuffer, __func__, [source, destination, blits = std::move(blits), filter]
	       (Lumen::MetalVulkan::EncodingContext& context)
	{
		Lumen::MetalVulkan::EndEncoding(context);
		if (source->metalTexture == nil || destination->metalTexture == nil) return VK_ERROR_INITIALIZATION_FAILED;
		static std::mutex cacheMutex;
		static std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelineCache;
		static const char* shaderSource = R"metal(
#include <metal_stdlib>
using namespace metal;
struct BlitVertexOut { float4 position [[position]]; float2 uv; };
vertex BlitVertexOut lumen_blit_vertex(uint vertexId [[vertex_id]]) {
    const float2 positions[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    BlitVertexOut out; out.position = float4(positions[vertexId], 0.0, 1.0);
    out.uv = positions[vertexId] * float2(0.5, -0.5) + 0.5; return out;
}
fragment float4 lumen_blit_fragment(BlitVertexOut in [[stage_in]], texture2d<float> source [[texture(0)]],
                                    sampler sourceSampler [[sampler(0)]]) {
    return source.sample(sourceSampler, in.uv);
}
)metal";
		const uint64_t cacheKey = (reinterpret_cast<uintptr_t>((__bridge void*)destination->device->metalDevice) << 16) ^
		                          static_cast<uint64_t>(destination->metalTexture.pixelFormat);
		id<MTLRenderPipelineState> pipeline = nil;
		{
			std::lock_guard lock(cacheMutex);
			auto found = pipelineCache.find(cacheKey);
			if (found != pipelineCache.end()) pipeline = found->second;
			if (pipeline == nil)
			{
				NSError* error = nil;
				NSString* sourceString = [NSString stringWithUTF8String:shaderSource];
				id<MTLLibrary> library = [destination->device->metalDevice newLibraryWithSource:sourceString options:nil error:&error];
				if (library == nil) { PrintMetalError("blit shader", error); return VK_ERROR_INVALID_SHADER_NV; }
				MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
				descriptor.vertexFunction = [library newFunctionWithName:@"lumen_blit_vertex"];
				descriptor.fragmentFunction = [library newFunctionWithName:@"lumen_blit_fragment"];
				descriptor.colorAttachments[0].pixelFormat = destination->metalTexture.pixelFormat;
				pipeline = [destination->device->metalDevice newRenderPipelineStateWithDescriptor:descriptor error:&error];
				if (pipeline == nil) { PrintMetalError("blit pipeline", error); return VK_ERROR_INVALID_SHADER_NV; }
				pipelineCache.emplace(cacheKey, pipeline);
			}
		}
		MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
		samplerDescriptor.minFilter = samplerDescriptor.magFilter = filter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear
		                                                                                       : MTLSamplerMinMagFilterNearest;
		samplerDescriptor.sAddressMode = samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
		id<MTLSamplerState> sampler = [destination->device->metalDevice newSamplerStateWithDescriptor:samplerDescriptor];
		if (sampler == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
		for (const auto& blit: blits)
		{
			if (blit.srcOffsets[0].z != 0 || blit.srcOffsets[1].z != 1 || blit.dstOffsets[0].z != 0 || blit.dstOffsets[1].z != 1 ||
			    blit.srcSubresource.layerCount != 1 || blit.dstSubresource.layerCount != 1 ||
			    blit.srcOffsets[0].x != 0 || blit.srcOffsets[0].y != 0 ||
			    blit.srcOffsets[1].x != static_cast<int32_t>(source->extent.width) ||
			    blit.srcOffsets[1].y != static_cast<int32_t>(source->extent.height))
				return VK_ERROR_FEATURE_NOT_PRESENT;
			MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = destination->metalTexture;
			pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> encoder = [context.commandBuffer renderCommandEncoderWithDescriptor:pass];
			if (encoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
			[encoder setRenderPipelineState:pipeline];
			[encoder setFragmentTexture:source->metalTexture atIndex:0];
			[encoder setFragmentSamplerState:sampler atIndex:0];
			const double width = std::abs(blit.dstOffsets[1].x - blit.dstOffsets[0].x);
			const double height = std::abs(blit.dstOffsets[1].y - blit.dstOffsets[0].y);
			[encoder setViewport:MTLViewport {static_cast<double>(std::min(blit.dstOffsets[0].x, blit.dstOffsets[1].x)),
			                                   static_cast<double>(std::min(blit.dstOffsets[0].y, blit.dstOffsets[1].y)),
			                                   width, height, 0.0, 1.0}];
			[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[encoder endEncoding];
		}
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout,
	                                            const VkClearColorValue* color, uint32_t rangeCount,
	                                            const VkImageSubresourceRange* ranges)
{
	if (image == VK_NULL_HANDLE || color == nullptr || rangeCount == 0 || ranges == nullptr)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	const VkClearColorValue clear = *color;
	std::vector<VkImageSubresourceRange> copiedRanges(ranges, ranges + rangeCount);
	Record(commandBuffer, __func__, [image, clear, copiedRanges = std::move(copiedRanges)]
	       (Lumen::MetalVulkan::EncodingContext& context)
	{
		Lumen::MetalVulkan::EndEncoding(context);
		if (image->metalTexture == nil) return VK_ERROR_INITIALIZATION_FAILED;
		for (const auto& range: copiedRanges)
		{
			uint32_t levelCount = range.levelCount == VK_REMAINING_MIP_LEVELS ? image->mipLevels - range.baseMipLevel : range.levelCount;
			uint32_t layerCount = range.layerCount == VK_REMAINING_ARRAY_LAYERS ? image->arrayLayers - range.baseArrayLayer : range.layerCount;
			if (range.baseMipLevel != 0 || levelCount != 1 || range.baseArrayLayer != 0 || layerCount != 1)
				return VK_ERROR_FEATURE_NOT_PRESENT;
			MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = image->metalTexture;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(clear.float32[0], clear.float32[1], clear.float32[2], clear.float32[3]);
			id<MTLRenderCommandEncoder> encoder = [context.commandBuffer renderCommandEncoderWithDescriptor:pass];
			if (encoder == nil) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
			[encoder endEncoding];
		}
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer commandBuffer, VkPipelineStageFlags,
	                                            VkPipelineStageFlags, VkDependencyFlags dependencyFlags,
	                                            uint32_t memoryBarrierCount, const VkMemoryBarrier* memoryBarriers,
	                                            uint32_t bufferBarrierCount, const VkBufferMemoryBarrier* bufferBarriers,
	                                            uint32_t imageBarrierCount,
	                                            const VkImageMemoryBarrier* imageBarriers)
{
	if ((dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT) != 0 ||
	    (memoryBarrierCount != 0 && memoryBarriers == nullptr) ||
	    (bufferBarrierCount != 0 && bufferBarriers == nullptr) ||
	    (imageBarrierCount != 0 && imageBarriers == nullptr))
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	Record(commandBuffer, __func__, [](Lumen::MetalVulkan::EncodingContext& context)
	{
		// Metal's tracked hazards order resources within a command buffer. Ending
		// the current encoder establishes a global execution/visibility boundary,
		// which also satisfies the narrower BY_REGION dependency used by Present.
		Lumen::MetalVulkan::EndEncoding(context);
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* info)
{
	if (info == nullptr || info->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext != nullptr ||
	    (info->dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT) != 0 ||
	    (info->memoryBarrierCount != 0 && info->pMemoryBarriers == nullptr) ||
	    (info->bufferMemoryBarrierCount != 0 && info->pBufferMemoryBarriers == nullptr) ||
	    (info->imageMemoryBarrierCount != 0 && info->pImageMemoryBarriers == nullptr))
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	// All allocations use Metal's tracked hazard mode. Closing an encoder
	// establishes a full execution and visibility boundary, conservatively
	// stronger than the requested stages/access masks, including BY_REGION.
	Record(commandBuffer, __func__, [](Lumen::MetalVulkan::EncodingContext& context) -> VkResult {
		if (context.logicalRenderPassActive) return VK_ERROR_FEATURE_NOT_PRESENT;
		Lumen::MetalVulkan::EndEncoding(context);
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
	if (event == VK_NULL_HANDLE) { RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED); return; }
	auto eventState = event->state;
	Record(commandBuffer, __func__, [eventState, stageMask](Lumen::MetalVulkan::EncodingContext& context)
	{
		// Capture on each encoding: host reset after recording and resubmission
		// of the same executable command buffer both use the current generation.
		uint64_t generation;
		{
			std::lock_guard lock(eventState->mutex);
			if (!eventState->alive) return VK_ERROR_INITIALIZATION_FAILED;
			generation = eventState->generation;
		}
		// This adapter signals events only at completion of the entire native
		// command buffer. An exact BOTTOM marker between eligible logical passes
		// therefore needs no encoder boundary; its host notification stays here.
		const bool completionMarker = CompletionPassReuseRequested() && stageMask == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT &&
		    !context.logicalRenderPassActive && context.renderPassReusable && context.renderEncoder != nil &&
		    context.computeEncoder == nil && context.blitEncoder == nil;
		if (completionMarker)
			context.retainedCompletionMarker = true;
		else
			Lumen::MetalVulkan::EndEncoding(context);
		[context.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed)
		{
			Lumen::MetalVulkan::CompleteEvent(eventState, generation, completed.status);
		}];
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo* info)
{
	const VkResult recording = ValidateCommandBufferForRecording(commandBuffer);
	if (recording != VK_SUCCESS) { RecordError(commandBuffer, __func__, recording); return; }
	if (event == VK_NULL_HANDLE || event->device != commandBuffer->pool->device)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	if (info == nullptr || info->sType != VK_STRUCTURE_TYPE_DEPENDENCY_INFO || info->pNext != nullptr ||
	    (info->dependencyFlags & ~VK_DEPENDENCY_BY_REGION_BIT) != 0 ||
	    (info->memoryBarrierCount != 0 && info->pMemoryBarriers == nullptr) ||
	    (info->bufferMemoryBarrierCount != 0 && info->pBufferMemoryBarriers == nullptr) ||
	    (info->imageMemoryBarrierCount != 0 && info->pImageMemoryBarriers == nullptr))
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	// Metal's tracked hazards and the encoder boundary make the source writes
	// available. Signal only at native completion, as for vkCmdSetEvent, so a
	// successful host event poll also observes the completed GPU writes.
	// No caller-owned dependency arrays are retained for deferred encoding.
	auto eventState = event->state;
	Record(commandBuffer, __func__, [eventState](Lumen::MetalVulkan::EncodingContext& context) -> VkResult
	{
		if (context.logicalRenderPassActive) return VK_ERROR_FEATURE_NOT_PRESENT;
		uint64_t generation;
		{
			std::lock_guard lock(eventState->mutex);
			if (!eventState->alive) return VK_ERROR_INITIALIZATION_FAILED;
			generation = eventState->generation;
		}
		Lumen::MetalVulkan::EndEncoding(context);
		[context.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed)
		{
			Lumen::MetalVulkan::CompleteEvent(eventState, generation, completed.status);
		}];
		return VK_SUCCESS;
	});
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer commandBuffer, uint32_t firstScissor,
	                                      uint32_t scissorCount, const VkRect2D* scissors)
{
	const VkResult recording = ValidateCommandBufferForRecording(commandBuffer);
	if (recording != VK_SUCCESS) { RecordError(commandBuffer, __func__, recording); return; }
	if (firstScissor != 0 || scissorCount != 1)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	if (scissors == nullptr)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	const VkRect2D rectangle = scissors[0];
	if (rectangle.offset.x < 0 || rectangle.offset.y < 0 ||
	    static_cast<uint64_t>(rectangle.offset.x) + rectangle.extent.width > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
	    static_cast<uint64_t>(rectangle.offset.y) + rectangle.extent.height > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
	{
		RecordError(commandBuffer, __func__, VK_ERROR_INITIALIZATION_FAILED);
		return;
	}
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	state->scissor = rectangle;
	state->scissorSet = true;
	// Scissor is consumed only by draws, which already freeze a full ReplayState.
	// Keep no caller pointer and no eager graphics replay requiring other state.
	// Empty extents still execute the draw and its vertex shader.
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
	if (lineWidth != 1.0f)
	{
		std::fprintf(stderr, "vkCmdSetLineWidth: unsupported width=%g\n", static_cast<double>(lineWidth));
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	auto state = GetReplayState(commandBuffer);
	if (state) state->lineWidth = lineWidth;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
	                                                  uint32_t compareMask)
{
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	if ((faceMask & VK_STENCIL_FACE_FRONT_BIT) != 0) state->stencil.frontCompareMask = compareMask;
	if ((faceMask & VK_STENCIL_FACE_BACK_BIT) != 0) state->stencil.backCompareMask = compareMask;
	if (TryElideMetalStateOnlyRecord(commandBuffer, __func__)) return;
	auto snapshot = std::make_shared<ReplayState>(*state);
	Record(commandBuffer, __func__, [snapshot](Lumen::MetalVulkan::EncodingContext& context) { return ApplyGraphicsState(context, snapshot); });
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
	                                                uint32_t writeMask)
{
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	if ((faceMask & VK_STENCIL_FACE_FRONT_BIT) != 0) state->stencil.frontWriteMask = writeMask;
	if ((faceMask & VK_STENCIL_FACE_BACK_BIT) != 0) state->stencil.backWriteMask = writeMask;
	if (TryElideMetalStateOnlyRecord(commandBuffer, __func__)) return;
	auto snapshot = std::make_shared<ReplayState>(*state);
	Record(commandBuffer, __func__, [snapshot](Lumen::MetalVulkan::EncodingContext& context) { return ApplyGraphicsState(context, snapshot); });
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask,
	                                                uint32_t reference)
{
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	if ((faceMask & VK_STENCIL_FACE_FRONT_BIT) != 0) state->stencil.frontReference = reference;
	if ((faceMask & VK_STENCIL_FACE_BACK_BIT) != 0) state->stencil.backReference = reference;
	if (TryElideMetalStateOnlyRecord(commandBuffer, __func__)) return;
	auto snapshot = std::make_shared<ReplayState>(*state);
	Record(commandBuffer, __func__, [snapshot](Lumen::MetalVulkan::EncodingContext& context) { return ApplyGraphicsState(context, snapshot); });
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetColorWriteEnableEXT(VkCommandBuffer commandBuffer, uint32_t attachmentCount,
	                                                   const VkBool32* colorWriteEnables)
{
	if (attachmentCount != 1 || colorWriteEnables == nullptr)
	{
		RecordError(commandBuffer, __func__, VK_ERROR_FEATURE_NOT_PRESENT);
		return;
	}
	auto state = GetReplayState(commandBuffer);
	if (!state) return;
	state->colorWriteEnabled = colorWriteEnables[0] == VK_TRUE;
	if (TryElideMetalStateOnlyRecord(commandBuffer, __func__)) return;
	auto snapshot = std::make_shared<ReplayState>(*state);
	Record(commandBuffer, __func__, [snapshot](Lumen::MetalVulkan::EncodingContext& context) { return ApplyGraphicsState(context, snapshot); });
}

} // extern "C"
