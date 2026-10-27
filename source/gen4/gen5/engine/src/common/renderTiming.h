// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Common::RenderTiming {

// Diagnostics only: cumulative counters are sampled on demand, without logging
// from a rendering thread or enabling the performance overlay. The selector is
// process-latched before the first measured operation and is OFF by default.
inline bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_PS5_RENDER_TIMING");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

enum class Counter : std::size_t {
    QueueSubmits,
    QueueLockNs,
    QueueSubmitNs,
    RendererGpuWaits,
    RendererGpuWaitNs,
    PriorityGpuWaits,
    PriorityGpuWaitNs,
    ReleaseMemPackets,
    ReleaseMemInterruptFlushes,
    ReleaseMemInterrupt1,
    ReleaseMemInterrupt2,
    ReleaseMemInterrupt4,
    SyncBufferDownloads,
    SyncDownloadPayloadBytes,
    SyncDownloadStagingBytes,
    SyncDownloadGpuWaitNs,
    SyncDownloadPriorityWaitNs,
    GuestMainBlankPresents,
    GuestMainSurfacePresents,
    GuestMainMissingPresents,
    ColorClearMaterializations,
    VideoOutColorClearMaterializations,
    VideoUploadSnapshots,
    VideoUploadRaces,
    VideoUploadUnstable,
    VideoProbeRegistryFull,
    VideoFramesDelivered,
    VideoFramePollEmpty,
    RecognizedBufferFillDispatches,
    RecognizedBufferFillBytes,
    UniformMetadataFillProofs,
    UniformMetadataHits,
    UniformMetadataMisses,
    Count
};

inline constexpr std::size_t CounterCount = static_cast<std::size_t>(Counter::Count);
inline constexpr std::array<const char*, CounterCount> CounterNames {
    "queueSubmits", "queueLockNs", "queueSubmitNs", "rendererGpuWaits",
    "rendererGpuWaitNs", "priorityGpuWaits", "priorityGpuWaitNs",
    "releaseMemPackets", "releaseMemInterruptFlushes", "releaseMemInterrupt1",
    "releaseMemInterrupt2", "releaseMemInterrupt4", "syncBufferDownloads",
    "syncDownloadPayloadBytes", "syncDownloadStagingBytes", "syncDownloadGpuWaitNs",
    "syncDownloadPriorityWaitNs", "guestMainBlankPresents", "guestMainSurfacePresents",
    "guestMainMissingPresents", "colorClearMaterializations", "videoOutColorClearMaterializations",
    "videoUploadSnapshots", "videoUploadRaces", "videoUploadUnstable", "videoProbeRegistryFull",
    "videoFramesDelivered", "videoFramePollEmpty",
    "recognizedBufferFillDispatches", "recognizedBufferFillBytes",
    "uniformMetadataFillProofs", "uniformMetadataHits", "uniformMetadataMisses"
};

namespace Detail {
static_assert(std::atomic<uint64_t>::is_always_lock_free);
inline std::array<std::atomic<uint64_t>, CounterCount> counters {};
inline thread_local uint64_t* sync_download_wait = nullptr;
} // namespace Detail

inline void Add(Counter counter, uint64_t value = 1) noexcept {
    if (Enabled()) {
        Detail::counters[static_cast<std::size_t>(counter)].fetch_add(value,
                                                                   std::memory_order_relaxed);
    }
}

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

inline TimePoint Begin() noexcept {
    return Enabled() ? Clock::now() : TimePoint {};
}

inline void Finish(Counter duration, TimePoint begin) noexcept {
    if (begin == TimePoint {}) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
    Add(duration, static_cast<uint64_t>(elapsed.count()));
}

// Attribute only the native blocking wait measured by ScopedWait, excluding
// the implicit submission inside CommandScheduler::Wait. Nesting restores the
// previous attribution; diagnostics OFF neither reads nor writes TLS.
class ScopedSyncDownloadGpuWait final {
public:
    ScopedSyncDownloadGpuWait() noexcept : m_enabled(Enabled()) {
        if (m_enabled) {
            m_previous = Detail::sync_download_wait;
            Detail::sync_download_wait = &m_elapsed;
        }
    }
    ~ScopedSyncDownloadGpuWait() {
        if (m_enabled) {
            Detail::sync_download_wait = m_previous;
            if (m_previous) *m_previous += m_elapsed;
        }
    }
    [[nodiscard]] uint64_t ElapsedNs() const noexcept { return m_elapsed; }
    ScopedSyncDownloadGpuWait(const ScopedSyncDownloadGpuWait&) = delete;
    ScopedSyncDownloadGpuWait& operator=(const ScopedSyncDownloadGpuWait&) = delete;

private:
    bool m_enabled;
    uint64_t* m_previous = nullptr;
    uint64_t m_elapsed = 0;
};

class ScopedWait final {
public:
    explicit ScopedWait(bool priority) noexcept : m_priority(priority), m_begin(Begin()) {}
    ~ScopedWait() {
        if (m_begin == TimePoint {}) return;
        const auto elapsed = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_begin).count());
        Add(m_priority ? Counter::PriorityGpuWaitNs : Counter::RendererGpuWaitNs, elapsed);
        if (!m_priority && Detail::sync_download_wait) {
            Add(Counter::SyncDownloadGpuWaitNs, elapsed);
            *Detail::sync_download_wait += elapsed;
        }
        Add(m_priority ? Counter::PriorityGpuWaits : Counter::RendererGpuWaits);
    }
    ScopedWait(const ScopedWait&) = delete;
    ScopedWait& operator=(const ScopedWait&) = delete;

private:
    bool m_priority;
    TimePoint m_begin;
};

enum class ReadbackOrigin : uint64_t {
    Other,
    GuestReadFault,
    GuestWriteFault,
    ColorMetadata,
    Invalidation,
    GuestMemoryRead,
    GuestMemoryWrite
};

inline const char* ReadbackOriginName(ReadbackOrigin origin) noexcept {
    switch (origin) {
    case ReadbackOrigin::GuestReadFault: return "guestReadFault";
    case ReadbackOrigin::GuestWriteFault: return "guestWriteFault";
    case ReadbackOrigin::ColorMetadata: return "colorMetadata";
    case ReadbackOrigin::Invalidation: return "invalidation";
    case ReadbackOrigin::GuestMemoryRead: return "guestMemoryRead";
    case ReadbackOrigin::GuestMemoryWrite: return "guestMemoryWrite";
    default: return "other";
    }
}

namespace Detail {
inline thread_local ReadbackOrigin readback_origin = ReadbackOrigin::Other;
} // namespace Detail

inline ReadbackOrigin CurrentReadbackOrigin() noexcept {
    return Enabled() ? Detail::readback_origin : ReadbackOrigin::Other;
}

inline ReadbackOrigin ReadbackOriginForFault(bool write) noexcept {
    const auto current = CurrentReadbackOrigin();
    // Attribute only the matching side of the API's copy. Its other pointer
    // can also fault and must retain ordinary fault attribution.
    const auto api_origin = write ? ReadbackOrigin::GuestMemoryWrite : ReadbackOrigin::GuestMemoryRead;
    return current == api_origin ? current
                                : write ? ReadbackOrigin::GuestWriteFault : ReadbackOrigin::GuestReadFault;
}

class ScopedReadbackOrigin final {
public:
    explicit ScopedReadbackOrigin(ReadbackOrigin origin) noexcept : m_enabled(Enabled()) {
        if (m_enabled) {
            m_previous = Detail::readback_origin;
            Detail::readback_origin = origin;
        }
    }
    ~ScopedReadbackOrigin() {
        if (m_enabled) Detail::readback_origin = m_previous;
    }
    ScopedReadbackOrigin(const ScopedReadbackOrigin&) = delete;
    ScopedReadbackOrigin& operator=(const ScopedReadbackOrigin&) = delete;

private:
    bool m_enabled;
    ReadbackOrigin m_previous = ReadbackOrigin::Other;
};

inline constexpr std::size_t ReadbackCapacity = 128;

struct ReadbackSample {
    uint64_t sequence = 0;
    uint64_t steady_ns = 0;
    uint64_t request_address = 0;
    uint64_t request_size = 0;
    uint64_t buffer_address = 0;
    uint64_t buffer_size = 0;
    uint64_t payload_bytes = 0;
    uint64_t staging_bytes = 0;
    // Native blocking wait only, identical to the existing sync-download wait
    // counter. Excludes submission, priority callbacks and CPU publication.
    uint64_t native_wait_ns = 0;
    ReadbackOrigin origin = ReadbackOrigin::Other;
    bool caller_gpu_thread = false;
    bool requested_dirty = false;
};

struct ReadbackSnapshot {
    bool enabled = false;
    uint64_t completed = 0;
    std::size_t count = 0;
    std::array<ReadbackSample, ReadbackCapacity> samples {};
};

namespace Detail {
struct AtomicReadbackSample {
    std::atomic<uint64_t> stamp {};
    std::array<std::atomic<uint64_t>, 11> values {};
};
inline std::array<AtomicReadbackSample, ReadbackCapacity> readbacks {};
inline std::atomic<uint64_t> readbacks_completed {};
} // namespace Detail

// The sole producer is the renderer owner, after a nonempty synchronous
// download has been published. A snapshot reader never blocks that owner.
// Sequentially consistent atomic fields make the stamp validation a complete
// ordering check, including during a wrap; no non-atomic data races or retries.
inline void RecordReadback(ReadbackSample sample) noexcept {
    if (!Enabled()) return;
    const auto sequence = Detail::readbacks_completed.load(std::memory_order_relaxed) + 1;
    const auto steady_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    auto& slot = Detail::readbacks[(sequence - 1) % ReadbackCapacity];
    const std::array<uint64_t, 11> values {
        steady_ns, sample.request_address, sample.request_size,
        sample.buffer_address, sample.buffer_size, sample.payload_bytes,
        sample.staging_bytes, sample.native_wait_ns, static_cast<uint64_t>(sample.origin),
        static_cast<uint64_t>(sample.caller_gpu_thread), static_cast<uint64_t>(sample.requested_dirty)
    };
    slot.stamp.store(sequence * 2 - 1);
    for (std::size_t i = 0; i < values.size(); ++i) slot.values[i].store(values[i]);
    slot.stamp.store(sequence * 2);
    Detail::readbacks_completed.store(sequence);
}

// Valid records are oldest first. An entry overwritten while being sampled is
// omitted, so count may be below min(completed, capacity) during concurrent use.
// This optional diagnostic snapshot is separate from the cumulative counters.
inline ReadbackSnapshot ReadReadbacks() noexcept {
    ReadbackSnapshot result {};
    result.enabled = Enabled();
    if (!result.enabled) return result;
    result.completed = Detail::readbacks_completed.load();
    const auto first = result.completed > ReadbackCapacity ? result.completed - ReadbackCapacity + 1 : 1;
    for (auto sequence = first; sequence <= result.completed; ++sequence) {
        const auto& slot = Detail::readbacks[(sequence - 1) % ReadbackCapacity];
        const auto stamp = slot.stamp.load();
        if (stamp != sequence * 2) continue;
        std::array<uint64_t, 11> values {};
        for (std::size_t i = 0; i < values.size(); ++i) values[i] = slot.values[i].load();
        if (slot.stamp.load() != stamp) continue;
        result.samples[result.count++] = {
            sequence, values[0], values[1], values[2], values[3], values[4],
            values[5], values[6], values[7], static_cast<ReadbackOrigin>(values[8]),
            values[9] != 0, values[10] != 0
        };
    }
    return result;
}

struct Snapshot {
    bool enabled;
    uint64_t steady_ns;
    std::array<uint64_t, CounterCount> values;
};

// Each counter is atomic; the group is intentionally not a transactional frame
// snapshot. Wait totals belong to distinct lanes and may overlap in wall time.
inline Snapshot Read() noexcept {
    Snapshot result {Enabled(), static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()), {}};
    for (std::size_t i = 0; i < CounterCount; ++i) {
        result.values[i] = Detail::counters[i].load(std::memory_order_relaxed);
    }
    return result;
}

} // namespace Common::RenderTiming
