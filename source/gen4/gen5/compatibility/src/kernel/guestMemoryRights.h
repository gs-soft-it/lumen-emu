// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace Libs::LibKernel::Memory::GuestRights {

inline constexpr std::uint32_t Read = 1;
inline constexpr std::uint32_t Write = 2;
inline constexpr std::uint32_t Execute = 4;

struct Range {
    std::uint64_t base {};
    std::uint64_t size {};
    std::uint32_t rights {};
    // Published only for private Stack mappings without GPU or executable
    // access. This is a mapping property, not a lifetime/reader capability.
    bool private_stack {};
};

// The owner publishes sorted, non-overlapping committed intervals. Reservations
// and GPU permissions never grant CPU access. The Prospero CPU-write bit means RW.
inline const Range* Find(std::span<const Range> ranges, std::uint64_t address) noexcept {
    std::size_t first = 0;
    std::size_t count = ranges.size();
    while (count != 0) {
        const auto step = count / 2;
        const auto middle = first + step;
        if (ranges[middle].base <= address) {
            first = middle + 1;
            count -= step + 1;
        } else {
            count = step;
        }
    }
    if (first == 0) return nullptr;
    const auto& range = ranges[first - 1];
    return address - range.base < range.size ? &range : nullptr;
}

inline bool Allows(std::span<const Range> ranges, std::uint64_t address,
                   std::uint64_t size, std::uint32_t requested) noexcept {
    if (address == 0 || size == 0 || size > UINT64_MAX - address ||
        requested == 0 || (requested & ~(Read | Write | Execute)) != 0) return false;
    const auto end = address + size;
    const auto* range = Find(ranges, address);
    while (address < end) {
        if (range == nullptr || address < range->base ||
            address - range->base >= range->size ||
            (range->rights & requested) != requested) return false;
        const auto remaining = range->size - (address - range->base);
        address += remaining < end - address ? remaining : end - address;
        if (address == end) return true;
        ++range;
        if (range == ranges.data() + ranges.size() || range->base != address) return false;
    }
    return false;
}

inline bool AllowsOwnedStackRead(std::span<const Range> ranges,
                                std::uint64_t address, std::uint64_t size,
                                std::uint64_t owned_base, std::uint64_t owned_size) noexcept {
    if (owned_base == 0 || owned_size == 0 || owned_size > UINT64_MAX - owned_base ||
        address < owned_base || address - owned_base >= owned_size || size == 0 ||
        size > owned_size - (address - owned_base)) return false;
    const auto end = address + size;
    const auto* range = Find(ranges, address);
    while (address < end) {
        if (range == nullptr || address < range->base ||
            address - range->base >= range->size || !range->private_stack ||
            (range->rights & Read) == 0 || (range->rights & Execute) != 0) return false;
        const auto remaining = range->size - (address - range->base);
        address += remaining < end - address ? remaining : end - address;
        if (address == end) return true;
        ++range;
        if (range == ranges.data() + ranges.size() || range->base != address) return false;
    }
    return false;
}

} // namespace Libs::LibKernel::Memory::GuestRights
