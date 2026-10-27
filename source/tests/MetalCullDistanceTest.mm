// Reproducible GPU contracts for the production cull-distance mesh generator.
// The Gen5 renderer fixture separately checks all-negative, mixed-sign and zero
// distances through the complete Vulkan adapter, including indexed draws.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../emulator/src/Graphics/Metal/MetalCullDistance.h"
#include <simd/simd.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
using namespace Lumen::MetalVulkan;
constexpr uint32_t Width = 16, Height = 16, LayerBytes = Width * Height * 4;
struct alignas(16) Vertex
{
	simd_float4 position;
	simd_float4 color;
	float cull[2];
	float clip[1];
	uint32_t layer, viewport;
};
static_assert(sizeof(Vertex) == 64);

void Check(bool condition, const char* message)
{
	if (!condition)
	{
		std::fprintf(stderr, "MetalCullDistanceTest: %s\n", message);
		std::exit(1);
	}
}

void Passed(bool condition, const char* message)
{
	Check(condition, message);
	std::printf("MetalCullDistanceTest: %s PASS\n", message);
}

std::vector<Vertex> FullscreenTriangles(uint32_t count)
{
	std::vector<Vertex> vertices(count * 3);
	for (uint32_t primitive = 0; primitive < count; ++primitive)
	{
		vertices[primitive * 3].position = {-1, -1, 0, 1};
		vertices[primitive * 3 + 1].position = {3, -1, 0, 1};
		vertices[primitive * 3 + 2].position = {-1, 3, 0, 1};
		for (uint32_t vertex = 0; vertex < 3; ++vertex)
		{
			vertices[primitive * 3 + vertex].color = {1, 0, 0, 1};
			vertices[primitive * 3 + vertex].clip[0] = 1;
		}
	}
	return vertices;
}

int Run() API_AVAILABLE(macos(13.0), ios(16.0))
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	Check(MetalCullDistanceSupported(device), "Metal 3 cull-distance backend unavailable");
	const std::string source = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Captured
{
    float4 position;
    float4 color;
    float cull[2];
    float clip[1];
    uint layer;
    uint viewport;
};
)MSL";
	const std::vector<MetalCullOutputField> fields = {
	    {"position", 0x80000000u, 0}, {"color", 0, 1}, {"cull", 0x80000004u, 0},
	    {"clip", 0x80000003u, 0}, {"layer", 0x80000009u, 0}, {"viewport", 0x8000000au, 0}};
	MetalCullMeshShader shader;
	std::string diagnostic;
	const auto status = CreateMetalCullMeshShader(device, source, "Captured", fields,
	    VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, &shader, &diagnostic);
	Check(status == VK_SUCCESS, diagnostic.c_str());

	NSError* error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:@"#include <metal_stdlib>\n"
	    "using namespace metal;\n"
	    "struct FragmentInput { float4 color [[user(locn0), flat]]; };\n"
	    "fragment float4 fragment_main(FragmentInput input [[stage_in]], uint primitive [[primitive_id]],\n"
	    "constant uint& mode [[buffer(0)]]) {\n"
	    "    if (mode == 2) return float4(input.color.r, float(primitive) / 255.0, 0, 1);\n"
	    "    return mode == 1 ? float4(float(primitive) / 255.0, 0, 0, 1) : input.color;\n"
	    "}\n" options:nil error:&error];
	Check(library != nil, error.localizedDescription.UTF8String);
	MTLMeshRenderPipelineDescriptor* descriptor = [MTLMeshRenderPipelineDescriptor new];
	descriptor.meshFunction = shader.function;
	descriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
	descriptor.maxTotalThreadsPerMeshThreadgroup = 1;
	descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	id<MTLRenderPipelineState> plain = [device newRenderPipelineStateWithMeshDescriptor:descriptor
	    options:MTLPipelineOptionNone reflection:nil error:&error];
	Check(plain != nil, error.localizedDescription.UTF8String);
	const auto topologyPipeline = [&](VkPrimitiveTopology topology) {
		MetalCullMeshShader topologyShader;
		const auto result = CreateMetalCullMeshShader(device, source, "Captured", fields, topology,
		    &topologyShader, &diagnostic);
		Check(result == VK_SUCCESS, diagnostic.c_str());
		MTLMeshRenderPipelineDescriptor* topologyDescriptor = [descriptor copy];
		topologyDescriptor.meshFunction = topologyShader.function;
		id<MTLRenderPipelineState> native = [device newRenderPipelineStateWithMeshDescriptor:topologyDescriptor
		    options:MTLPipelineOptionNone reflection:nil error:&error];
		Check(native != nil, error.localizedDescription.UTF8String);
		return native;
	};
	id<MTLRenderPipelineState> strip = topologyPipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
	id<MTLRenderPipelineState> fan = topologyPipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
	descriptor.colorAttachments[0].blendingEnabled = YES;
	descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
	descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	id<MTLRenderPipelineState> blend = [device newRenderPipelineStateWithMeshDescriptor:descriptor
	    options:MTLPipelineOptionNone reflection:nil error:&error];
	Check(blend != nil, error.localizedDescription.UTF8String);

	MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor new];
	textureDescriptor.textureType = MTLTextureType2DArray;
	textureDescriptor.arrayLength = 2;
	textureDescriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
	textureDescriptor.width = Width;
	textureDescriptor.height = Height;
	textureDescriptor.storageMode = MTLStorageModeShared;
	textureDescriptor.usage = MTLTextureUsageRenderTarget;
	id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
	id<MTLCommandQueue> queue = [device newCommandQueue];
	const uint32_t dummyWords[4] = {};
	id<MTLBuffer> dummy = [device newBufferWithBytes:dummyWords length:sizeof(dummyWords)
	    options:MTLResourceStorageModeShared];
	Check(texture && queue && dummy, "render resources allocation failed");
	std::array<uint8_t, LayerBytes * 2> pixels {};

	const auto render = [&](const std::vector<Vertex>& vertices, uint32_t flags = 0, uint32_t mode = 0,
	                       bool blending = false, uint32_t firstPrimitive = 0, bool splitViewports = false,
	                       VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	                       bool cullBack = false, bool reverseWinding = false) {
		id<MTLBuffer> captured = [device newBufferWithBytes:vertices.data() length:vertices.size() * sizeof(Vertex)
		    options:MTLResourceStorageModeShared];
		Check(captured != nil, "capture allocation failed");
		id<MTLCommandBuffer> command = [queue commandBuffer];
		MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor new];
		pass.colorAttachments[0].texture = texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
		pass.renderTargetArrayLength = 2;
		id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
		[encoder setRenderPipelineState:topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ? strip :
		    topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ? fan : blending ? blend : plain];
		[encoder setCullMode:cullBack ? MTLCullModeBack : MTLCullModeNone];
		[encoder setFrontFacingWinding:reverseWinding ? MTLWindingClockwise : MTLWindingCounterClockwise];
		[encoder setMeshBuffer:captured offset:0 atIndex:kCullCaptureBufferIndex];
		[encoder setMeshBuffer:dummy offset:0 atIndex:kCullIndexBufferIndex];
		[encoder setMeshBuffer:dummy offset:0 atIndex:kCullPatchCountsBufferIndex];
		const MetalCullDrawParams parameters {uint32_t(vertices.size()), MetalCullPrimitiveCount(topology, uint32_t(vertices.size())),
		    firstPrimitive, flags};
		[encoder setMeshBytes:&parameters length:sizeof(parameters) atIndex:kCullDrawParamsBufferIndex];
		[encoder setFragmentBytes:&mode length:sizeof(mode) atIndex:0];
		const MTLViewport viewports[2] = {{0, 0, splitViewports ? 8.0 : 16.0, 16, 0, 1}, {8, 0, 8, 16, 0, 1}};
		[encoder setViewports:viewports count:2];
		[encoder drawMeshThreadgroups:MTLSizeMake(parameters.primitiveCount, 1, 1)
		    threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
		[encoder endEncoding];
		[command commit];
		[command waitUntilCompleted];
		Check(command.status == MTLCommandBufferStatusCompleted, command.error.localizedDescription.UTF8String);
		for (uint32_t layer = 0; layer < 2; ++layer)
			[texture getBytes:pixels.data() + LayerBytes * layer bytesPerRow:Width * 4 bytesPerImage:LayerBytes
			    fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0 slice:layer];
	};

	auto vertices = FullscreenTriangles(1);
	vertices[1].color = {0, 1, 0, 1};
	vertices[2].color = {0, 0, 1, 1};
	render(vertices);
	bool valid = true;
	for (uint32_t pixel = 0; pixel < Width * Height; ++pixel)
		valid &= pixels[pixel * 4] == 255 && pixels[pixel * 4 + 1] == 0 && pixels[pixel * 4 + 2] == 0;
	Passed(valid, "flat first provoking vertex");

	vertices = FullscreenTriangles(2);
	render(vertices, 0, 1, false, 17);
	valid = true;
	for (uint32_t pixel = 0; pixel < Width * Height; ++pixel) valid &= pixels[pixel * 4] == 18;
	Passed(valid, "PrimitiveID survives ordered mesh groups");

	vertices = FullscreenTriangles(1);
	for (auto& vertex: vertices) vertex.clip[0] = vertex.position.x;
	render(vertices);
	valid = true;
	for (uint32_t y = 0; y < Height; ++y)
		for (uint32_t x = 0; x < Width; ++x) valid &= pixels[(y * Width + x) * 4] == (x >= Width / 2 ? 255 : 0);
	Passed(valid, "ClipDistance clips a surviving primitive");

	vertices = FullscreenTriangles(1);
	for (auto& vertex: vertices) { vertex.layer = 1; vertex.viewport = 1; }
	render(vertices, 0, 0, false, 0, true);
	valid = true;
	for (uint32_t layer = 0; layer < 2; ++layer)
		for (uint32_t y = 0; y < Height; ++y)
			for (uint32_t x = 0; x < Width; ++x)
				valid &= pixels[layer * LayerBytes + (y * Width + x) * 4] == (layer == 1 && x >= Width / 2 ? 255 : 0);
	Passed(valid, "viewport and render target layer");

	vertices = FullscreenTriangles(1);
	for (auto& vertex: vertices) vertex.position.z = -0.5f;
	render(vertices);
	valid = true;
	for (uint32_t pixel = 0; pixel < Width * Height; ++pixel) valid &= pixels[pixel * 4] == 0;
	Passed(valid, "Metal zero to one depth clips negative z");
	render(vertices, kCullDrawNegativeOneToOne);
	valid = true;
	for (uint32_t pixel = 0; pixel < Width * Height; ++pixel) valid &= pixels[pixel * 4] == 255;
	Passed(valid, "negative one to one conversion");

	vertices = FullscreenTriangles(1);
	vertices[1].position = {1, -1, 0, 1};
	vertices[2].position = {-1, 1, 0, 1};
	render(vertices);
	const auto unflipped = pixels;
	render(vertices, kCullDrawFlipY);
	valid = true;
	for (uint32_t y = 0; y < Height; ++y)
		for (uint32_t x = 0; x < Width; ++x)
			valid &= pixels[(y * Width + x) * 4] == unflipped[((Height - 1 - y) * Width + x) * 4];
	Passed(valid, "reflected position Y flip");

	vertices = FullscreenTriangles(32);
	float expected[3] = {};
	for (uint32_t primitive = 0; primitive < 32; ++primitive)
	{
		const float color[3] = {float((primitive * 13) % 17) / 16.0f, float((primitive * 7) % 11) / 10.0f,
		    float(primitive % 3) / 2.0f};
		for (uint32_t vertex = 0; vertex < 3; ++vertex)
			vertices[primitive * 3 + vertex].color = {color[0], color[1], color[2], 0.25f};
		for (uint32_t component = 0; component < 3; ++component)
			expected[component] = color[component] * 0.25f + expected[component] * 0.75f;
	}
	render(vertices, 0, 0, true);
	valid = true;
	for (uint32_t pixel = 0; pixel < Width * Height; ++pixel)
		for (uint32_t component = 0; component < 3; ++component)
			valid &= std::abs(float(pixels[pixel * 4 + component]) / 255.0f - expected[component]) < 0.012f;
	Passed(valid, "32 overlapping primitives preserve blend order");

	for (const auto topology: {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN})
	{
		vertices.assign(4, {});
		vertices[0].position = {-1, -1, 0, 1};
		vertices[1].position = {1, -1, 0, 1};
		vertices[2].position = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ?
		    simd_float4 {-1, 1, 0, 1} : simd_float4 {1, 1, 0, 1};
		vertices[3].position = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ?
		    simd_float4 {1, 1, 0, 1} : simd_float4 {-1, 1, 0, 1};
		for (uint32_t vertex = 0; vertex < 4; ++vertex)
		{
			vertices[vertex].clip[0] = 1;
			vertices[vertex].color = {float(vertex + 1) / 5.0f, 0, 0, 1};
		}
		render(vertices, 0, 2, false, 7, false, topology, true);
		valid = true;
		for (uint32_t y = 0; y < Height; ++y)
			for (uint32_t x = 0; x < Width; ++x)
			{
				// Avoid only the shared edge: Vulkan's provoking vertex and winding
				// are independent of which primitive owns a sample on that edge.
				if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ? x == y : x + y == Width - 1) continue;
				const uint32_t primitive = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ?
				    (x > y ? 1 : 0) : (x + y < Width - 1 ? 1 : 0);
				const uint32_t provoking = primitive + (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ? 1 : 0);
				const auto pixel = (y * Width + x) * 4;
				valid &= pixels[pixel] == (provoking + 1) * 51 && pixels[pixel + 1] == 7 + primitive && pixels[pixel + 3] == 255;
			}
		Passed(valid, topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ?
		    "strip flat, PrimitiveID and consistent front winding" : "fan flat, PrimitiveID and consistent front winding");
		render(vertices, 0, 2, false, 7, false, topology, true, true);
		valid = true;
		for (uint32_t pixel = 0; pixel < Width * Height; ++pixel) valid &= pixels[pixel * 4 + 3] == 0;
		Passed(valid, topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ?
		    "strip opposite winding culls both primitives" : "fan opposite winding culls both primitives");
	}
	return 0;
}
} // namespace

int main()
{
	@autoreleasepool
	{
		if (@available(macOS 13.0, iOS 16.0, *)) return Run();
		std::fprintf(stderr, "MetalCullDistanceTest requires Metal 3 / macOS 13 or newer\n");
		return 1;
	}
}
