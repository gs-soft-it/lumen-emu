#pragma once

#import <Metal/Metal.h>

#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace Lumen::MetalVulkan {

inline constexpr uint32_t kCullCaptureBufferIndex = 27;
inline constexpr uint32_t kCullIndexBufferIndex = 23;
inline constexpr uint32_t kCullDrawParamsBufferIndex = 22;
inline constexpr uint32_t kCullPatchCountsBufferIndex = 24;
inline constexpr uint32_t kCullDrawIndexed = 1;
inline constexpr uint32_t kCullDrawPatchDomain = 2;
inline constexpr uint32_t kCullDrawFlipY = 16;
inline constexpr uint32_t kCullDrawNegativeOneToOne = 32;

struct MetalCullOutputField
{
	std::string name;
	// Location, or 0x80000000 | SPIR-V BuiltIn. No inference from symbol names.
	uint32_t key = UINT32_MAX;
	// Flat=1, NoPerspective=2, Centroid=4, Sample=8.
	uint32_t interpolationFlags = 0;
};

// Captured records are indexed by invocation ordinal, never by an unchecked
// guest VertexIndex. Optional local indices connect captured TES outputs.
// Patch-domain mode uses triangle lists with Indexed | PatchDomain: vertexCount
// and indexCount are the per-patch strides, reserved is the patch count, and
// primitiveCount is patchCount * (indexCount / 3). Buffer 24 contains uint4
// {points, indices, error, reserved} for each patch. The caller validates every
// allocation and stride product against UINT32_MAX before dispatch. Buffers 23
// and 24 must be bound even for ordinary draws, where they are not read.
struct MetalCullDrawParams
{
	uint32_t vertexCount = 0;
	uint32_t primitiveCount = 0;
	uint32_t firstPrimitive = 0;
	uint32_t flags = 0;
	uint32_t indexCount = 0;
	uint32_t indexOffset = 0;
	uint32_t vertexOffset = 0;
	uint32_t reserved = 0;
};
static_assert(sizeof(MetalCullDrawParams) == 32);

struct MetalCullMeshShader
{
	__strong id<MTLFunction> function = nil;
	uint32_t cullDistanceCount = 0;
	uint32_t verticesPerPrimitive = 0;
	std::string source;
};

[[nodiscard]] bool MetalCullDistanceSupported(id<MTLDevice> device);
[[nodiscard]] uint32_t MetalCullPrimitiveCount(VkPrimitiveTopology topology, uint32_t vertexCount);

// captureSource is SPIRV-Cross output with vertex_for_tessellation and an
// ordinal output buffer. The original vertex work remains in the capture
// function; the generated mesh function only assembles and culls primitives.
// Unsupported output layouts/topologies fail before any pipeline is admitted.
[[nodiscard]] VkResult CreateMetalCullMeshShader(id<MTLDevice> device,
	const std::string& captureSource, const std::string& outputType,
	const std::vector<MetalCullOutputField>& outputFields, VkPrimitiveTopology topology,
	MetalCullMeshShader* result, std::string* diagnostic = nullptr);

} // namespace Lumen::MetalVulkan
