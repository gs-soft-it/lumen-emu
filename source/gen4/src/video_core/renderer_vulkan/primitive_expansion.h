// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace Vulkan::PrimitiveExpansion {

// Vulkan ignores a final incomplete patch. Keep the complete-patch output count
// representable by the draw arguments instead of wrapping multiplication.
constexpr std::optional<uint32_t> TriangleVertexCount(uint32_t count, uint32_t patch_size) {
    if ((patch_size != 3 && patch_size != 4) ||
        count / patch_size > std::numeric_limits<uint32_t>::max() / 6) {
        return std::nullopt;
    }
    return count / patch_size * 6;
}

constexpr bool RectVertexRangeFits(uint32_t first_vertex, uint32_t expanded_count) {
    return expanded_count == 0 ||
           uint64_t(first_vertex) + expanded_count - 1 <= std::numeric_limits<uint32_t>::max();
}

constexpr uint32_t QuadSourceIndex(uint32_t output_index) {
    constexpr std::array<uint32_t, 6> corners{0, 1, 2, 0, 2, 3};
    return output_index / 6 * 4 + corners[output_index % 6];
}

// Automatic quad indices are an immutable prefix shared by all draws. Grow in
// complete groups of six indices so the backing can be generated once, without
// changing any buffer still referenced by an earlier draw.
constexpr std::optional<uint32_t> AutomaticQuadIndexCapacity(uint32_t required_index_count,
                                                            uint32_t current_capacity) {
    constexpr uint32_t max_capacity = std::numeric_limits<uint32_t>::max() / 6 * 6;
    if (required_index_count > max_capacity || current_capacity > max_capacity ||
        current_capacity % 6 != 0) {
        return std::nullopt;
    }
    if (required_index_count <= current_capacity) {
        return current_capacity;
    }
    uint64_t capacity = current_capacity == 0 ? 384 : current_capacity;
    while (capacity < required_index_count) {
        const uint64_t doubled = capacity * 2;
        capacity = doubled < max_capacity ? doubled : max_capacity;
    }
    return static_cast<uint32_t>(capacity);
}

// An empty source denotes auto indices. Explicit indices retain their full
// unsigned values; the draw's signed baseVertex is applied later by Vulkan.
inline bool ExpandQuadIndices(uint32_t source_count, std::span<const uint8_t> source,
                              uint32_t index_width, std::span<uint32_t> output) {
    const auto count = TriangleVertexCount(source_count, 4);
    if (!count || output.size() != *count ||
        (index_width != 0 && index_width != 2 && index_width != 4) ||
        (index_width == 0 ? !source.empty()
                          : source.size() != uint64_t(source_count) * index_width)) {
        return false;
    }
    for (uint32_t i = 0; i < *count; ++i) {
        const uint32_t vertex = QuadSourceIndex(i);
        if (index_width == 0) {
            output[i] = vertex;
        } else if (index_width == 2) {
            uint16_t value;
            std::memcpy(&value, source.data() + uint64_t(vertex) * 2, sizeof(value));
            output[i] = value;
        } else {
            std::memcpy(&output[i], source.data() + uint64_t(vertex) * 4, sizeof(uint32_t));
        }
    }
    return true;
}

} // namespace Vulkan::PrimitiveExpansion
