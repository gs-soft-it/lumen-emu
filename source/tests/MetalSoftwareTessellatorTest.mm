// Standalone: xcrun clang++ -std=c++20 -fobjc-arc -framework Foundation
// -framework Metal MetalSoftwareTessellatorTest.mm -o /tmp/tess-test && /tmp/tess-test
// Optional LUMEN_TESS_REFERENCE_HEADER enables point/index comparison with the
// original upstream fixed-point C implementation behind a CPU address-space shim.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include "../emulator/src/Graphics/Metal/MetalSoftwareTessellatorSource.inc"
#ifdef LUMEN_TESS_REFERENCE_HEADER
#include LUMEN_TESS_REFERENCE_HEADER
#endif

namespace {
constexpr uint32_t Guard = 0xa5e17eed;
constexpr uint32_t PointStride = 4225 + 8;
constexpr uint32_t IndexStride = 24576 + 8;
struct Config { uint32_t patches, factor_stride, point_stride, index_stride, domain, partition, ccw, reserved; };
struct ExpandConfig { uint32_t patches, input_stride, output_stride, domain; };
struct Counts { uint32_t points, indices, status, reserved; };
static_assert(sizeof(Config) == 32 && sizeof(ExpandConfig) == 16 && sizeof(Counts) == 16);
void Check(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "MetalSoftwareTessellatorTest: %s\n", message); std::exit(1); }
}
float HalfRound(float value) { return float(_Float16(value)); }
}

int main() {
 @autoreleasepool {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  Check(device != nil, "no Metal device");
  MTLCompileOptions *options = [MTLCompileOptions new];
  options.languageVersion = MTLLanguageVersion3_0;
  if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
  else options.fastMathEnabled = NO;
  NSError *error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:
      [NSString stringWithUTF8String:LumenSoftwareTessellatorSource()] options:options error:&error];
  if (!library) { std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
  id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:
      [library newFunctionWithName:@"lumen_tessellate_domain"] error:&error];
  Check(pipeline != nil, "domain pipeline compile failed");
  id<MTLComputePipelineState> expansion = [device newComputePipelineStateWithFunction:
      [library newFunctionWithName:@"lumen_expand_tess_factors"] error:&error];
  Check(expansion != nil, "factor expansion pipeline compile failed");
  id<MTLCommandQueue> queue = [device newCommandQueue];
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const std::vector<std::array<float, 6>> factors{
      {0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2},
      {3, 3, 3, 3, 3, 3}, {2.5f, 2.5f, 2.5f, 2.5f, 2.5f, 2.5f},
      {1, 2.5f, 4, 6, 3.5f, 2.5f}, {64, 64, 64, 64, 64, 64},
      {64, 1, 2, 63, 31.25f, 2.5f}, {-1, 3, 3, 3, 3, 3},
      {nan, 3, 3, 3, 3, 3}, {2, 3, 4, 5, nan, nan}};
  uint32_t checked_patches = 0;
  for (uint32_t domain = 0; domain < 2; ++domain) {
   for (uint32_t partition = 0; partition < 3; ++partition) {
    for (uint32_t ccw = 0; ccw < 2; ++ccw) {
      const uint32_t patch_count = uint32_t(factors.size());
      Config config{patch_count, 8, PointStride, IndexStride, domain, partition, ccw, 0};
      ExpandConfig expand_config{patch_count, 8, 8, domain};
      std::vector<uint16_t> half_words(8 + patch_count * 8 + 8, 0x3555);
      std::vector<float> reference_factors(patch_count * 8, 0);
      for (uint32_t p = 0; p < patch_count; ++p) {
        for (uint32_t field = 0; field < (domain == 0 ? 4u : 6u); ++field) {
          const auto factor = factors[p][domain == 0 && field == 3 ? 4 : field];
          half_words[8 + p * 8 + field] = std::bit_cast<uint16_t>(_Float16(factor));
        }
        for (uint32_t field = 0; field < 6; ++field)
          reference_factors[p * 8 + field] = domain == 0 && (field == 3 || field == 5)
              ? 0.f : HalfRound(factors[p][field]);
      }
      std::vector<uint32_t> factor_words(4 + patch_count * 8 + 4, Guard);
      std::vector<uint32_t> point_words(4 + patch_count * PointStride * 2 + 4, Guard);
      std::vector<uint32_t> index_words(4 + patch_count * IndexStride + 4, Guard);
      std::vector<uint32_t> count_words(4 + patch_count * 4 + 4, Guard);
      id<MTLBuffer> input = [device newBufferWithBytes:half_words.data()
          length:half_words.size() * 2 options:MTLResourceStorageModeShared];
      id<MTLBuffer> expanded = [device newBufferWithBytes:factor_words.data()
          length:factor_words.size() * 4 options:MTLResourceStorageModeShared];
      id<MTLBuffer> points = [device newBufferWithBytes:point_words.data()
          length:point_words.size() * 4 options:MTLResourceStorageModeShared];
      id<MTLBuffer> indices = [device newBufferWithBytes:index_words.data()
          length:index_words.size() * 4 options:MTLResourceStorageModeShared];
      id<MTLBuffer> counts = [device newBufferWithBytes:count_words.data()
          length:count_words.size() * 4 options:MTLResourceStorageModeShared];
      Check(input && expanded && points && indices && counts, "allocation failed");
      id<MTLCommandBuffer> command = [queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:expansion];
      [encoder setBuffer:input offset:16 atIndex:0];
      [encoder setBuffer:expanded offset:16 atIndex:1];
      [encoder setBytes:&expand_config length:sizeof(expand_config) atIndex:2];
      [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder endEncoding];
      encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:pipeline];
      [encoder setBuffer:expanded offset:16 atIndex:0];
      [encoder setBuffer:points offset:16 atIndex:1];
      [encoder setBuffer:indices offset:16 atIndex:2];
      [encoder setBuffer:counts offset:16 atIndex:3];
      [encoder setBytes:&config length:sizeof(config) atIndex:4];
      [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
      if (command.status != MTLCommandBufferStatusCompleted) {
        std::fprintf(stderr, "GPU failure: %s\n", command.error.localizedDescription.UTF8String);
        return 1;
      }
      Check(std::memcmp(input.contents, half_words.data(), half_words.size() * 2) == 0,
            "input factors or input canaries changed");
      const auto *point_bits = static_cast<const uint32_t *>(points.contents);
      const auto *index_bits = static_cast<const uint32_t *>(indices.contents);
      const auto *factor_bits = static_cast<const uint32_t *>(expanded.contents);
      const auto *count_bits = static_cast<const uint32_t *>(counts.contents);
      for (auto pair : {std::pair{point_bits, point_words.size()},
                        std::pair{index_bits, index_words.size()},
                        std::pair{factor_bits, factor_words.size()},
                        std::pair{count_bits, count_words.size()}}) {
        for (uint32_t i = 0; i < 4; ++i)
          Check(pair.first[i] == Guard && pair.first[pair.second - 1 - i] == Guard,
                "global buffer canary changed");
      }
      const auto *metadata = reinterpret_cast<const Counts *>(count_bits + 4);
#ifdef LUMEN_TESS_REFERENCE_HEADER
      std::vector<LumenTessReference::poly_tess_point> reference_points(patch_count * PointStride);
      std::vector<uint32_t> reference_indices(patch_count * IndexStride);
      std::vector<LumenTessReference::uint4> reference_counts(patch_count);
      LumenTessReference::poly_tess_params params{reference_factors.data(), reference_points.data(),
          reference_indices.data(), reference_counts.data(), 8, PointStride, IndexStride,
          LumenTessReference::poly_tess_partitioning(partition), 0};
#endif
      for (uint32_t p = 0; p < patch_count; ++p) {
        const auto meta = metadata[p];
        Check(meta.status == 0 && meta.reserved == 0 && meta.points <= 4225 &&
                  meta.indices <= 24576 && meta.indices % 3 == 0,
              "invalid count, capacity or status");
        for (uint32_t field = 0; field < 8; ++field) {
          const auto actual = factor_bits[4 + p * 8 + field];
          if (field >= 6) Check(actual == Guard, "factor expansion overwrote padding");
          else Check(actual == std::bit_cast<uint32_t>(reference_factors[p * 8 + field]) ||
                         (std::isnan(std::bit_cast<float>(actual)) &&
                          std::isnan(reference_factors[p * 8 + field])), "factor expansion changed ordering/value");
        }
        const auto *coords = reinterpret_cast<const std::array<float, 2> *>(point_bits + 4) + p * PointStride;
        const auto *patch_indices = index_bits + 4 + p * IndexStride;
        for (uint32_t i = meta.points * 2; i < PointStride * 2; ++i)
          Check(point_bits[4 + p * PointStride * 2 + i] == Guard, "point write crossed used count or patch slice");
        for (uint32_t i = meta.indices; i < IndexStride; ++i)
          Check(patch_indices[i] == Guard, "index write crossed used count or patch slice");
        for (uint32_t i = 0; i < meta.points; ++i)
          Check(std::isfinite(coords[i][0]) && std::isfinite(coords[i][1]) &&
                    coords[i][0] >= 0 && coords[i][1] >= 0 && coords[i][0] <= 1 && coords[i][1] <= 1 &&
                    (domain != 0 || coords[i][0] + coords[i][1] <= 1.00002f), "invalid domain coordinate");
        double area = 0;
        for (uint32_t i = 0; i < meta.indices; i += 3) {
          Check(patch_indices[i] < meta.points && patch_indices[i + 1] < meta.points &&
                    patch_indices[i + 2] < meta.points, "triangle references a point outside its patch");
          const auto a = coords[patch_indices[i]], b = coords[patch_indices[i + 1]], c = coords[patch_indices[i + 2]];
          const double signed_area = 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
          // Fractional-odd inner factors clamped to 1+epsilon can collapse a
          // transition triangle at 16.16 precision. The exact reference check
          // below still requires every coordinate and index to match.
          constexpr double fixed_point_area_tolerance = 1e-8;
          Check(ccw ? signed_area <= fixed_point_area_tolerance
                    : signed_area >= -fixed_point_area_tolerance,
                "nondegenerate triangle winding changed within a patch");
          area += std::abs(signed_area);
        }
        const bool culled = p == 0 || p == 8 || p == 9;
        Check(culled ? (meta.points == 0 && meta.indices == 0) :
                  (meta.points != 0 && std::abs(area - (domain == 0 ? 0.5 : 1.0)) < 0.0001),
              "zero/negative/NaN culling or complete patch coverage failed");
#ifdef LUMEN_TESS_REFERENCE_HEADER
        if (domain == 0) LumenTessReference::poly_tess_tri_process(&params, p, LumenTessReference::POLY_TESS_MODE_WITH_COUNTS);
        else LumenTessReference::poly_tess_quad_process(&params, p, LumenTessReference::POLY_TESS_MODE_WITH_COUNTS);
        Check(meta.points == reference_counts[p].x && meta.indices == reference_counts[p].y,
              "GPU counts differ from original fixed-point CPU reference");
        for (uint32_t i = 0; i < meta.points; ++i) {
          const auto expected = reference_points[p * PointStride + i];
          Check(coords[i][0] == float(expected.u) / 65536.f && coords[i][1] == float(expected.v) / 65536.f,
                "GPU coordinates differ from original fixed-point CPU reference");
        }
        for (uint32_t i = 0; i < meta.indices; ++i) {
          const uint32_t reference_index = ccw && i % 3 != 0 ? i + (i % 3 == 1 ? 1 : -1) : i;
          Check(patch_indices[i] == reference_indices[p * IndexStride + reference_index],
                "GPU connectivity differs from original CPU reference");
        }
#endif
        ++checked_patches;
        std::printf("patch domain=%u partition=%u ccw=%u case=%u points=%u indices=%u area=%.8f\n",
                    domain, partition, ccw, p, meta.points, meta.indices, area);
      }
      // Reuse the same pipeline/buffers with an undersized stride: no output
      // writes are permitted, and every patch must report the failure on GPU.
      std::memcpy(points.contents, point_words.data(), point_words.size() * 4);
      std::memcpy(indices.contents, index_words.data(), index_words.size() * 4);
      config.point_stride = 4224;
      command = [queue commandBuffer];
      encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:pipeline];
      [encoder setBuffer:expanded offset:16 atIndex:0];
      [encoder setBuffer:points offset:16 atIndex:1];
      [encoder setBuffer:indices offset:16 atIndex:2];
      [encoder setBuffer:counts offset:16 atIndex:3];
      [encoder setBytes:&config length:sizeof(config) atIndex:4];
      [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
      [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
      Check(command.status == MTLCommandBufferStatusCompleted, "invalid-config dispatch failed");
      Check(std::memcmp(points.contents, point_words.data(), point_words.size() * 4) == 0 &&
                std::memcmp(indices.contents, index_words.data(), index_words.size() * 4) == 0,
            "undersized output configuration wrote point/index buffers");
      for (uint32_t p = 0; p < patch_count; ++p)
        Check(metadata[p].status == 1 && metadata[p].points == 0 && metadata[p].indices == 0,
              "undersized output did not report bounded failure");
    }
   }
  }
  std::printf("MetalSoftwareTessellatorTest PASS patches=%u partitions=odd/even/equal domains=tri/quad half-expansion=1 guards=1 bounded-failure=1 reference=%u\n",
      checked_patches,
#ifdef LUMEN_TESS_REFERENCE_HEADER
      1u
#else
      0u
#endif
  );
  return 0;
 }
}
