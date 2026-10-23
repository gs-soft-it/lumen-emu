// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <boost/container/small_vector.hpp>
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"

namespace VideoCore {

class BufferUploadCache {
public:
    using Generations = boost::container::small_vector<u64, 2>;

    std::optional<std::span<const u64>> Find(VAddr address, u32 size) const {
        if (!ValidRange(address, size)) {
            return std::nullopt;
        }
        for (const auto& entry : entries) {
            if (entry.address == address && entry.size == size) {
                return std::span<const u64>{entry.generations};
            }
        }
        const u64 first_page = address >> TRACKER_PAGE_BITS;
        const u64 last_page = (address + size - 1) >> TRACKER_PAGE_BITS;
        const u64 first_region = address >> TRACKER_HIGHER_PAGE_BITS;
        const u64 last_region = (address + size - 1) >> TRACKER_HIGHER_PAGE_BITS;
        for (const auto& entry : entries) {
            if (!ValidRange(entry.address, entry.size)) {
                continue;
            }
            const VAddr end = entry.address + entry.size - 1;
            if (first_page < (entry.address >> TRACKER_PAGE_BITS) ||
                last_page > (end >> TRACKER_PAGE_BITS)) {
                continue;
            }
            const u64 entry_region = entry.address >> TRACKER_HIGHER_PAGE_BITS;
            const u64 region_count = (end >> TRACKER_HIGHER_PAGE_BITS) - entry_region + 1;
            if (entry.generations.size() != region_count) {
                continue;
            }
            return std::span<const u64>{entry.generations}.subspan(
                first_region - entry_region, last_region - first_region + 1);
        }
        return std::nullopt;
    }

    void Store(VAddr address, u32 size, Generations generations) {
        if (!ValidRange(address, size)) {
            return;
        }
        for (auto& entry : entries) {
            if (entry.address == address && entry.size == size) {
                entry.generations = std::move(generations);
                return;
            }
        }
        Entry entry{address, size, std::move(generations)};
        if (entries.size() < 8) {
            if (entries.size() == 1 && entries.capacity() < 8) {
                entries.reserve(8);
            }
            entries.push_back(std::move(entry));
        } else {
            entries[next_entry] = std::move(entry);
            next_entry = (next_entry + 1) & 7;
        }
    }

    void Clear() {
        entries.clear();
        next_entry = 0;
    }

private:
    static bool ValidRange(VAddr address, u32 size) {
        return size != 0 && address <= std::numeric_limits<u64>::max() - (size - 1);
    }

    struct Entry {
        VAddr address;
        u32 size;
        Generations generations;
    };
    boost::container::small_vector<Entry, 1> entries;
    size_t next_entry{};
};

} // namespace VideoCore
