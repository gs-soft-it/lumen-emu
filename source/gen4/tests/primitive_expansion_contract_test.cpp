// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/primitive_expansion.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>

int main() {
    using namespace Vulkan::PrimitiveExpansion;
    static_assert(TriangleVertexCount(0, 3) == 0);
    static_assert(TriangleVertexCount(6, 3) == 12);
    static_assert(TriangleVertexCount(8, 4) == 12);
    static_assert(TriangleVertexCount(5, 3) == 6);
    static_assert(TriangleVertexCount(7, 4) == 6);
    static_assert(!TriangleVertexCount(0xffffffffU, 3));
    static_assert(!TriangleVertexCount(0xfffffffcU, 4));
    static_assert(RectVertexRangeFits(0xfffffff4U, 12));
    static_assert(!RectVertexRangeFits(0xfffffff5U, 12));
    static_assert(RectVertexRangeFits(0xffffffffU, 0));

    std::array<uint32_t, 12> output{};
    const std::array<uint32_t, 12> automatic{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    if (!ExpandQuadIndices(8, {}, 0, output) || output != automatic) return 1;
    const std::array<uint16_t, 8> indices16{7, 2, 0xffff, 9, 11, 1, 5, 0};
    const std::array<uint32_t, 12> expected16{7, 2, 0xffff, 7, 0xffff, 9, 11, 1, 5, 11, 5, 0};
    auto bytes16 = std::span{reinterpret_cast<const uint8_t*>(indices16.data()), sizeof(indices16)};
    if (!ExpandQuadIndices(8, bytes16, 2, output) || output != expected16) return 2;
    // UINT_MAX is an ordinary quad-list index: list restart is not enabled.
    const std::array<uint32_t, 8> indices32{0xffffffffU, 9, 5, 7, 0x80000000U, 4, 3, 8};
    const std::array<uint32_t, 12> expected32{0xffffffffU, 9, 5, 0xffffffffU, 5, 7,
                                            0x80000000U, 4, 3, 0x80000000U, 3, 8};
    auto bytes32 = std::span{reinterpret_cast<const uint8_t*>(indices32.data()), sizeof(indices32)};
    if (!ExpandQuadIndices(8, bytes32, 4, output) || output != expected32) return 3;
    std::array<uint32_t, 6> partial{};
    const std::array<uint32_t, 6> expected_partial{0xffffffffU, 9, 5, 0xffffffffU, 5, 7};
    if (!ExpandQuadIndices(7, bytes32.first(28), 4, partial) || partial != expected_partial) return 4;
    const auto unchanged = output;
    if (ExpandQuadIndices(7, bytes32, 4, output) ||
        ExpandQuadIndices(8, bytes32.first(28), 4, output) ||
        ExpandQuadIndices(8, bytes32, 3, output) ||
        ExpandQuadIndices(8, bytes32, 4, std::span{output}.first(11)) || output != unchanged) return 5;

    constexpr uint32_t max_capacity = std::numeric_limits<uint32_t>::max() / 6 * 6;
    static_assert(AutomaticQuadIndexCapacity(0, 0) == 0);
    static_assert(AutomaticQuadIndexCapacity(0, 384) == 384);
    static_assert(AutomaticQuadIndexCapacity(384, 384) == 384);
    static_assert(AutomaticQuadIndexCapacity(383, 384) == 384);
    static_assert(!AutomaticQuadIndexCapacity(max_capacity + 1, 0));
    static_assert(!AutomaticQuadIndexCapacity(1, 5));
    static_assert(!AutomaticQuadIndexCapacity(0, max_capacity + 1));
    static_assert(AutomaticQuadIndexCapacity(max_capacity, max_capacity) == max_capacity);
    // A growth step near UINT_MAX must saturate without overflowing or losing
    // the final complete quad, even when the requested count is not aligned.
    constexpr uint32_t near_max = max_capacity / 2 / 6 * 6 + 6;
    static_assert(AutomaticQuadIndexCapacity(near_max + 1, near_max) == max_capacity);
    static_assert(AutomaticQuadIndexCapacity(max_capacity - 1, 0) == max_capacity);
    const auto small_capacity = AutomaticQuadIndexCapacity(7, 0);
    if (!small_capacity || *small_capacity < 7 || *small_capacity % 6 != 0) return 6;
    const auto grown_capacity = AutomaticQuadIndexCapacity(*small_capacity + 1, *small_capacity);
    if (!grown_capacity || *grown_capacity < uint64_t(*small_capacity) * 2 ||
        *grown_capacity % 6 != 0 ||
        AutomaticQuadIndexCapacity(*small_capacity, *grown_capacity) != grown_capacity) return 7;

    // This mesh size exercises the two final quads that were lost through the
    // native patch path. Cached auto indices must include both, and a larger
    // cache must preserve the complete index prefix used by earlier draws.
    constexpr uint32_t previous_vertices = 4596;
    constexpr uint32_t updated_vertices = 4604;
    static_assert(TriangleVertexCount(updated_vertices, 4) == 6906);
    std::vector<uint32_t> previous(*TriangleVertexCount(previous_vertices, 4));
    std::vector<uint32_t> updated(*TriangleVertexCount(updated_vertices, 4));
    if (!ExpandQuadIndices(previous_vertices, {}, 0, previous) ||
        !ExpandQuadIndices(updated_vertices, {}, 0, updated) ||
        !std::equal(previous.begin(), previous.end(), updated.begin())) return 8;
    constexpr std::array<uint32_t, 12> final_two_quads{
        4596, 4597, 4598, 4596, 4598, 4599, 4600, 4601, 4602, 4600, 4602, 4603};
    if (!std::equal(final_two_quads.begin(), final_two_quads.end(), updated.end() - 12) ||
        *std::max_element(updated.begin(), updated.end()) != updated_vertices - 1) return 9;
    const auto cache_capacity = AutomaticQuadIndexCapacity(updated.size(), *small_capacity);
    if (!cache_capacity || *cache_capacity < updated.size()) return 10;
    std::vector<uint32_t> cached(*cache_capacity);
    if (!ExpandQuadIndices(*cache_capacity / 6 * 4, {}, 0, cached) ||
        !std::equal(updated.begin(), updated.end(), cached.begin()) ||
        AutomaticQuadIndexCapacity(previous.size(), *cache_capacity) != cache_capacity) return 11;

    std::puts("primitive expansion: partial groups, checked cache growth, stable prefixes, final two quads, u16/u32 and auto indices PASS");
}
