// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "common/renderTiming.h"

namespace Common::VideoUploadProbe {

inline constexpr std::size_t Capacity = 64;
struct Handle { std::size_t slot = Capacity; };

namespace Detail {
struct Entry {
    std::atomic<uint64_t> version {0}, begin {0}, end {0};
};
inline std::array<Entry, Capacity> entries;
inline std::atomic<uint64_t> sequence {1};
inline uint64_t Next(bool writing) {
    return ((sequence.fetch_add(1, std::memory_order_relaxed) + 1) << 1) | writing;
}
struct Sample { uint64_t version; bool hit, writing, unstable; };
inline Sample Read(std::size_t index, uint64_t begin, uint64_t end) {
    const auto& entry = entries[index];
    const auto first = entry.version.load(std::memory_order_acquire);
    if (first == 0) return {0, false, false, false};
    const auto low = entry.begin.load(std::memory_order_relaxed);
    const auto high = entry.end.load(std::memory_order_relaxed);
    // Keep both bounds reads before validating the publication version during
    // slot reuse. An acquire load below only orders accesses that follow it.
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto last = entry.version.load(std::memory_order_acquire);
    return {last, low < end && begin < high, (last & 1) != 0,
            first != last || first == 1};
}
} // namespace Detail

inline Handle Register(uint64_t address, uint64_t size) {
    if (!RenderTiming::Enabled() || address == 0 || size == 0 || size > UINT64_MAX - address)
        return {};
    for (std::size_t i = 0; i < Capacity; ++i) {
        auto& entry = Detail::entries[i];
        uint64_t free = 0;
        if (!entry.version.compare_exchange_strong(free, 1, std::memory_order_acq_rel)) continue;
        entry.begin.store(address, std::memory_order_relaxed);
        entry.end.store(address + size, std::memory_order_relaxed);
        entry.version.store(Detail::Next(false), std::memory_order_release);
        return {i};
    }
    RenderTiming::Add(RenderTiming::Counter::VideoProbeRegistryFull);
    return {};
}

inline void Unregister(Handle& handle) {
    const auto index = handle.slot;
    handle = {};
    if (!RenderTiming::Enabled() || index == Capacity) return;
    auto& entry = Detail::entries[index];
    entry.version.exchange(Detail::Next(true), std::memory_order_seq_cst);
    entry.version.store(0, std::memory_order_release);
}

// Only metadata is synchronized. Pixel writes and staging copies retain their
// existing scheduling, with no mutex, retry, wait, or extra guest-memory read.
class WriteScope final {
public:
    explicit WriteScope(Handle handle)
        : m_index(RenderTiming::Enabled() ? handle.slot : Capacity) {
        if (m_index != Capacity)
            Detail::entries[m_index].version.exchange(Detail::Next(true), std::memory_order_seq_cst);
    }
    ~WriteScope() {
        if (m_index != Capacity)
            Detail::entries[m_index].version.store(Detail::Next(false), std::memory_order_release);
    }
    WriteScope(const WriteScope&) = delete;
    WriteScope& operator=(const WriteScope&) = delete;
private:
    std::size_t m_index;
};

class SnapshotScope final {
public:
    SnapshotScope(uint64_t address, uint64_t size) : m_begin(address), m_end(address + size) {
        if (!RenderTiming::Enabled() || size == 0 || size > UINT64_MAX - address) return;
        m_enabled = true;
        for (std::size_t i = 0; i < Capacity; ++i)
            m_before[i] = Detail::Read(i, m_begin, m_end);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    ~SnapshotScope() {
        if (!m_enabled) return;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        bool seen = false, raced = false, unstable = false;
        for (std::size_t i = 0; i < Capacity; ++i) {
            const auto after = Detail::Read(i, m_begin, m_end);
            const auto before = m_before[i];
            unstable |= before.unstable || after.unstable;
            if (!before.hit && !after.hit) continue;
            seen = true;
            raced |= before.writing || after.writing || before.version != after.version ||
                     before.unstable || after.unstable;
        }
        using Counter = RenderTiming::Counter;
        if (seen) RenderTiming::Add(Counter::VideoUploadSnapshots);
        if (seen && raced) RenderTiming::Add(Counter::VideoUploadRaces);
        if (unstable) RenderTiming::Add(Counter::VideoUploadUnstable);
    }
    SnapshotScope(const SnapshotScope&) = delete;
    SnapshotScope& operator=(const SnapshotScope&) = delete;
private:
    // Intentionally uninitialized when OFF: no registry scan or array clearing.
    std::array<Detail::Sample, Capacity> m_before;
    uint64_t m_begin, m_end;
    bool m_enabled = false;
};

} // namespace Common::VideoUploadProbe
