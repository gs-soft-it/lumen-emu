// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>

namespace Lumen::Ps5::DirectFex {

// Only immutable, process-owned veneer return addresses may be remembered.
// Zero is a miss, so a target registered later can become visible immediately.
class VeneerReturnCache {
public:
    std::uint64_t Find(std::uint64_t target) const noexcept {
        const auto& entry = entries_[Index(target)];
        return entry.target == target ? entry.stub : 0;
    }

    void Remember(std::uint64_t target, std::uint64_t stub) noexcept {
        if (stub != 0) entries_[Index(target)] = {target, stub};
    }

private:
    struct Entry { std::uint64_t target {}, stub {}; };
    static constexpr std::size_t Index(std::uint64_t target) noexcept {
        return ((target >> 2) ^ (target >> 10)) & 31;
    }
    std::array<Entry, 32> entries_ {};
};

} // namespace Lumen::Ps5::DirectFex
