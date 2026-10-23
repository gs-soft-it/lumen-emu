// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Libraries::Font::Internal {
// Fixed SysV AMD64 signatures use separate INTEGER and SSE register sequences.
// Floats are not promoted to double and do not consume an integer argument slot.
struct FontScaleCallbackArguments {
    std::array<std::uint64_t, 5> integer{};
    std::array<std::array<std::uint64_t, 2>, 2> xmm{};
    std::size_t count{};
    std::span<const std::uint64_t> Integers() const { return {integer.data(), count}; }
};
inline FontScaleCallbackArguments MakeFontScaleCallbackArguments(
    std::uintptr_t object, bool with_dpi, std::uint32_t dpi_x, std::uint32_t dpi_y,
    float scale_x, float scale_y, std::uintptr_t out_x, std::uintptr_t out_y) {
    FontScaleCallbackArguments result{};
    result.xmm[0][0] = std::bit_cast<std::uint32_t>(scale_x);
    result.xmm[1][0] = std::bit_cast<std::uint32_t>(scale_y);
    if (with_dpi) {
        result.integer = {object, dpi_x, dpi_y, out_x, out_y};
        result.count = 5;
    } else {
        result.integer = {object, out_x, out_y, 0, 0};
        result.count = 3;
    }
    return result;
}
} // namespace Libraries::Font::Internal
