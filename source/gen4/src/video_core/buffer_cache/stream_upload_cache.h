// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>
#include <tsl/robin_map.h>
#include "common/types.h"

namespace VideoCore {

class StreamUploadCache {
public:
    static bool UseEpochForSerial(std::string_view serial) {
        return serial == "CUSA01623" || serial == "CUSA01715" || serial == "CUSA01740";
    }

    void SetEpochMode(bool enabled) {
        epoch_mode = enabled;
    }

    void AdvanceEpoch() {
        ++epoch;
    }

    u64 Copy(VAddr address, u32 size, u64 current_tick, auto&& copy_memory, auto&& compare_memory,
             auto&& map, auto&& commit) {
        if (tick != current_tick) {
            snapshot_used = 0;
            tick = current_tick;
        }
        if (entries.size() > 8192 || snapshot_used + size > 64 * 1024 * 1024) {
            entries.clear();
            snapshot_used = 0;
        }
        const u64 key = address ^ (u64{size} << 48);
        auto [iterator, inserted] = entries.try_emplace(key);
        Entry& entry = iterator.value();
        if (!inserted && entry.tick == current_tick && entry.address == address && entry.size == size) {
            if (epoch_mode ? entry.epoch == epoch
                           : compare_memory(address, snapshot.data() + entry.snapshot_offset, size)) {
                return entry.stream_offset;
            }
        }
        if (epoch_mode) {
            const auto [destination, offset] = map(size);
            copy_memory(address, destination, size);
            commit();
            entry = {address, current_tick, epoch, offset, 0, size};
            return offset;
        }
        const u64 snapshot_offset = snapshot_used;
        const u64 required_size = snapshot_offset + size;
        if (snapshot.size() < required_size) {
            snapshot.resize(std::max<u64>(2 * snapshot.size(), required_size));
        }
        copy_memory(address, snapshot.data() + snapshot_offset, size);
        snapshot_used = required_size;
        const auto [destination, offset] = map(size);
        std::memcpy(destination, snapshot.data() + snapshot_offset, size);
        commit();
        entry = {address, current_tick, 0, offset, snapshot_offset, size};
        return offset;
    }

private:
    struct Entry {
        VAddr address{};
        u64 tick{};
        u64 epoch{};
        u64 stream_offset{};
        u64 snapshot_offset{};
        u32 size{};
    };
    struct KeyHash {
        size_t operator()(u64 key) const {
            key = (key ^ (key >> 33)) * 0xff51afd7ed558ccdULL;
            return key ^ (key >> 33);
        }
    };
    tsl::robin_map<u64, Entry, KeyHash, std::equal_to<u64>,
                   std::allocator<std::pair<u64, Entry>>, true> entries;
    std::vector<u8> snapshot;
    u64 snapshot_used{};
    u64 tick{};
    bool epoch_mode{};
    u64 epoch{};
};

} // namespace VideoCore
