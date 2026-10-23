// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace VideoCore {

// Keep cache ownership at guest granularity. Host protection is the
// intersection of the logical permissions of all subpages in that mapping.
class HostPageWatch final {
public:
    static constexpr std::size_t GuestPageSize = 4096;
    static constexpr std::uint8_t Read = 1;
    static constexpr std::uint8_t Write = 2;
    static constexpr std::uint8_t Unavailable = 0xff;

    explicit HostPageWatch(std::size_t size) : size{size} {
        assert(size == GuestPageSize || size == 4 * GuestPageSize);
    }

    std::size_t Size() const { return size; }

    template <typename Visit>
    void ForEachHostPage(std::uint64_t address, std::uint64_t length, Visit&& visit) const {
        if (length == 0) return;
        assert(length <= std::numeric_limits<std::uint64_t>::max() - address);
        const auto last = (address + length - 1) & ~(std::uint64_t{size} - 1);
        for (auto page = address & ~(std::uint64_t{size} - 1);; page += size) {
            visit(page);
            if (page == last) break;
        }
    }

    template <typename Query>
    std::uint8_t Permissions(std::uint64_t address, Query&& query) const {
        std::uint8_t result = Read | Write;
        const auto base = address & ~(std::uint64_t{size} - 1);
        for (std::size_t offset = 0; offset < size; offset += GuestPageSize) {
            const auto permissions = query(base + offset);
            if (permissions == Unavailable) return Unavailable;
            result &= permissions;
        }
        // A read watch blocks writes as well, so a write cannot discard a GPU
        // result that has not first been read back into guest backing memory.
        if (!(result & Read)) result &= ~Write;
        return result;
    }

    struct FaultSet {
        std::array<std::uint64_t, 4> pages{};
        std::size_t count{};
    };

    template <typename Query>
    FaultSet FaultPages(std::uint64_t address, bool write, Query&& query) const {
        FaultSet result;
        const auto base = address & ~(std::uint64_t{size} - 1);
        for (std::size_t offset = 0; offset < size; offset += GuestPageSize) {
            const auto page = base + offset;
            const auto watches = query(page);
            if (watches & (write ? Read | Write : Read)) result.pages[result.count++] = page;
        }
        return result;
    }

private:
    const std::size_t size;
};

} // namespace VideoCore
