// Primitive assembly shared by the renderer and synthetic contract tests.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace Libs::Graphics::PrimitiveAssembly {

// Bound transient CPU/GPU storage. Expansion never splits a draw: splitting
// would restart PrimitiveId and change shader-visible values.
inline constexpr uint64_t MaxTriangleListBytes = 64ull * 1024 * 1024;

struct TriangleList {
    bool success = false;
    const char* reason = "";
    std::vector<uint32_t> indices;
};

namespace Detail {
template <typename ReadIndex>
TriangleList ExpandStrip(uint32_t count, ReadIndex read, bool provoking_last,
                        bool restart, uint32_t restart_index, uint64_t max_bytes) {
    uint64_t output_count = 0;
    uint32_t run = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (restart && read(i) == restart_index) {
            run = 0;
        } else if (++run >= 3) {
            output_count += 3;
            if (output_count > UINT32_MAX || output_count > max_bytes / sizeof(uint32_t))
                return {false, "triangle-strip expansion exceeds index count or upload capacity", {}};
        }
    }
    TriangleList result {true, "", {}};
    result.indices.reserve(static_cast<std::size_t>(output_count));
    uint32_t a = 0, b = 0;
    run = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t c = read(i);
        if (restart && c == restart_index) {
            run = 0;
            continue;
        }
        if (run >= 2) {
            // Odd triangles are cyclic rotations with the same winding.
            // Keep the provoking vertex in slot 0 (FIRST) or 2 (LAST), and
            // the barycentric basis in the corresponding Vulkan convention.
            if ((run & 1u) == 0)
                result.indices.insert(result.indices.end(), {a, b, c});
            else if (provoking_last)
                result.indices.insert(result.indices.end(), {b, a, c});
            else
                result.indices.insert(result.indices.end(), {a, c, b});
        }
        // Degenerate triangles still advance parity and PrimitiveId.
        a = b;
        b = c;
        ++run;
    }
    return result;
}
} // namespace Detail

inline TriangleList TriangleStripToList(std::span<const uint8_t> bytes, uint32_t count,
                                       uint32_t element_size, bool provoking_last,
                                       bool restart, uint64_t max_bytes = MaxTriangleListBytes) {
    if ((element_size != 1 && element_size != 2 && element_size != 4) ||
        static_cast<uint64_t>(count) * element_size != bytes.size())
        return {false, "invalid triangle-strip index byte range", {}};
    const auto read = [&](uint32_t i) {
        uint32_t index = 0;
        std::memcpy(&index, bytes.data() + static_cast<uint64_t>(i) * element_size, element_size);
        return index;
    };
    return Detail::ExpandStrip(count, read, provoking_last, restart,
                               UINT32_MAX >> ((4 - element_size) * 8), max_bytes);
}

inline TriangleList TriangleStripToListAuto(uint32_t count, bool provoking_last,
                                           uint64_t max_bytes = MaxTriangleListBytes) {
    // Reject oversized auto draws before iterating or allocating.
    const uint64_t output_count = count < 3 ? 0 : 3ull * (count - 2);
    if (output_count > UINT32_MAX || output_count > max_bytes / sizeof(uint32_t))
        return {false, "triangle-strip expansion exceeds index count or upload capacity", {}};
    return Detail::ExpandStrip(count, [](uint32_t i) { return i; }, provoking_last,
                               false, 0, max_bytes);
}

} // namespace Libs::Graphics::PrimitiveAssembly
