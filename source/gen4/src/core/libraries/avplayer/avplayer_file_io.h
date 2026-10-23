// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <atomic>
#include <vector>

#include "core/libraries/avplayer/avplayer_read.h"

namespace Libraries::AvPlayer {

// A replacement table represents one file. An overlapping invocation must not
// wait: the current guest callback may itself be waiting for that invocation.
// The owner keeps its buffers alive until the callback and copy have completed.
class AvPlayerFileOperation final {
public:
    explicit AvPlayerFileOperation(std::atomic_flag& active)
        : active{active}, acquired{!active.test_and_set(std::memory_order_acquire)} {}
    ~AvPlayerFileOperation() {
        if (acquired) active.clear(std::memory_order_release);
    }
    AvPlayerFileOperation(const AvPlayerFileOperation&) = delete;
    AvPlayerFileOperation& operator=(const AvPlayerFileOperation&) = delete;
    explicit operator bool() const { return acquired; }
private:
    std::atomic_flag& active;
    bool acquired;
};

// Build privately and publish only a complete file. read fills the guest bounce
// buffer; copy validates and transfers it while native memory admission is held.
template <typename Read, typename Copy>
bool AvPlayerPreloadFile(u64 expected, Read&& read, Copy&& copy, std::vector<u8>& cache) {
    std::vector<u8> pending;
    if (expected > 0 && expected < 256 * 1024 * 1024) {
        pending.reserve(static_cast<size_t>(expected));
    }
    std::array<u8, kAvPlayerGuestReadChunkMax> chunk;
    u64 position = 0;
    while (expected == 0 || position < expected) {
        const auto requested = static_cast<u32>(expected == 0 ? chunk.size() :
            std::min<u64>(chunk.size(), expected - position));
        const s32 count = read(position, requested);
        if (count < 0 || count > static_cast<s32>(requested)) return false;
        if (count == 0) {
            if (expected != 0) return false;
            break;
        }
        if (!copy(chunk.data(), static_cast<u32>(count))) return false;
        pending.insert(pending.end(), chunk.data(), chunk.data() + count);
        position += static_cast<u32>(count);
    }
    cache.swap(pending);
    return true;
}

} // namespace Libraries::AvPlayer
