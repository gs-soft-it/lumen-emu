// Lumen adapter, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_PS5_GUEST_BACKING_POLICY_H_
#define LUMEN_PS5_GUEST_BACKING_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

// A GPU worker acquires ownership through a real exclusive FEX epoch or the
// separately selected native data pin. The pin stabilizes mapping lifetime;
// CPU execution and data bytes remain concurrent in that unqualified experiment.
// In strict async mode CPU-visible owners and callbacks finish before CPU
// readmission. The separately selected CPU-visible experiment retains owners
// across readmission to expose missing CPU/GPU coherence dependencies.
// Native execution has no equivalent CPU quiescence contract yet.
inline thread_local bool GuestGpuPublicationActive = false;
inline bool GuestGpuWriteOwnershipSupported() noexcept {
#if defined(LUMEN_PS5_FEXCORE)
    return GuestGpuPublicationActive;
#else
    return false;
#endif
}

// Process-wide experimental rollout. Retained owners require every physical
// alias to deny CPU access and every VM mutation to publish before changing it.
inline bool GuestAsyncPrivateBackingEnabled() noexcept {
#if defined(LUMEN_PS5_FEXCORE)
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_PS5_GFX_ASYNC_PRIVATE_BACKING");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
#else
    return false;
#endif
}

// Deliberately incomplete CPU/GPU coherence experiment. This does not change
// VM retirement, executable alias admission, or explicit native dependencies.
// It is process-latched and requires the base async selector as well as FEX.
inline bool GuestAsyncCpuVisibleExperimentEnabled() noexcept {
#if defined(LUMEN_PS5_FEXCORE)
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_PS5_GFX_ASYNC_CPU_VISIBLE_EXPERIMENT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return GuestAsyncPrivateBackingEnabled() && enabled;
#else
    return false;
#endif
}

// A native VM pin does not cover translated loads/stores or raw HLE accesses.
// No provider currently supplies a verified, complete pre-access/handoff
// contract for those operations (including atomics, SIMD, REP and aliases).
// This is an implemented-coverage fact, never an environment override.
inline constexpr bool GuestCpuGpuConcurrentAccessCoverageAvailable = false;
inline constexpr bool GuestCpuGpuConcurrencyCanStart(bool requested) noexcept {
    return !requested || GuestCpuGpuConcurrentAccessCoverageAvailable;
}
inline bool GuestAsyncCpuConcurrentExperimentRequested() noexcept {
    static const bool requested = [] {
        const auto* value = std::getenv("LUMEN_PS5_GFX_ASYNC_CPU_CONCURRENT_EXPERIMENT");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return requested;
}

// Archived concurrency experiment, process-latched and OFF by default.
// VM lifetime/executable aliases remain protected. Ordinary translated stores
// are neither intercepted nor ordered against GPU snapshots/publication. This
// flag supplies no missing CPU/GPU data-coherence contract. Initialization
// explicitly rejects a request until complete provider coverage is available;
// parsing the request here must never silently convert it into exclusive mode.
inline bool GuestAsyncCpuConcurrentExperimentEnabled() noexcept {
#if defined(LUMEN_PS5_FEXCORE)
    return GuestAsyncCpuVisibleExperimentEnabled() && GuestAsyncCpuConcurrentExperimentRequested();
#else
    return false;
#endif
}

enum class BackingComparison { Missing, Unchanged, Changed };

// Process-latched rollback, shared by every guest. A native caller without an
// exclusive publication epoch must still take the conservative upload path.
inline bool GuestBufferSnapshotReuseEnabled() noexcept {
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_PS5_BUFFER_SNAPSHOT_REUSE");
        return value == nullptr || std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// A real exclusive epoch or native data pin stabilizes GC backing lifetime.
// Selected dirty owners still publish before retirement, and the
// submission's final tail applies the selected visibility policy. Retirement
// always publishes its selected owners before their backing can disappear.
inline bool GuestGcPublicationDeferred() noexcept {
    if (!GuestGpuWriteOwnershipSupported()) return false;
    static const bool enabled = [] {
        const auto* value = std::getenv("LUMEN_PS5_GFX_DEFER_GC_PUBLICATION");
        return value == nullptr || std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

enum class BackingRevalidation {
    KeepCpuState,
    MarkCpuModified,
    KeepGpuOwner,
    RejectGpuOwner,
};

inline constexpr BackingRevalidation ClassifyBackingRevalidation(
    BackingComparison comparison, bool gpu_owned, bool reuse_unchanged) noexcept {
    if (gpu_owned) {
        return comparison == BackingComparison::Unchanged
            ? BackingRevalidation::KeepGpuOwner : BackingRevalidation::RejectGpuOwner;
    }
    // Keeping the state never clears explicit CPU invalidation. The hash is
    // evidence about staged content, not a replacement for lifetime/dirty state.
    return reuse_unchanged && comparison == BackingComparison::Unchanged
        ? BackingRevalidation::KeepCpuState : BackingRevalidation::MarkCpuModified;
}

class GuestBackingSnapshots {
public:
    // Publish only bytes from the very staging copy consumed by the GPU.
    void Publish(std::uint64_t address, const void* staged, std::size_t size) {
        Snapshot snapshot {size, XXH3_64bits(staged, size), {}};
        if (GuestAsyncCpuVisibleExperimentEnabled() &&
            !GuestAsyncCpuConcurrentExperimentEnabled()) {
            const auto* bytes = static_cast<const uint8_t*>(staged);
            snapshot.bytes.assign(bytes, bytes + size);
        }
        snapshots.insert_or_assign(address, std::move(snapshot));
    }
    // Exact completed native download bytes were copied to the same backing.
    // Caller proves whole-page coverage, stable owner/mapping and EXCLUSIVE;
    // async byte-reconciliation is deliberately ineligible for this witness.
    void PublishCompletedDownloadHash(std::uint64_t address, std::size_t size,
                                      std::uint64_t hash) {
        snapshots.insert_or_assign(address, Snapshot {size, hash, {}, true});
    }
    bool IsPublicationWitness(std::uint64_t address, std::size_t size) const {
        const auto it = snapshots.find(address);
        return it != snapshots.end() && it->second.size == size && it->second.publication_witness;
    }
    void ConsumePublicationWitness(std::uint64_t address) {
        const auto it = snapshots.find(address);
        if (it != snapshots.end()) it->second.publication_witness = false;
    }
    BackingComparison Compare(std::uint64_t address, const void* current, std::size_t size) const {
        const auto previous = snapshots.find(address);
        if (previous == snapshots.end() || previous->second.size != size) return BackingComparison::Missing;
        return previous->second.hash == XXH3_64bits(current, size)
            ? BackingComparison::Unchanged : BackingComparison::Changed;
    }
    bool HasSnapshot(std::uint64_t address, std::size_t size) const {
        const auto previous = snapshots.find(address);
        return previous != snapshots.end() && previous->second.size == size;
    }
    // The caller hashed the same complete span afresh under backing locks.
    // This preserves the existing hash comparison; it adds no new authority.
    BackingComparison CompareHash(std::uint64_t address, std::size_t size, std::uint64_t hash) const {
        const auto previous = snapshots.find(address);
        if (previous == snapshots.end() || previous->second.size != size) return BackingComparison::Missing;
        return previous->second.hash == hash
            ? BackingComparison::Unchanged : BackingComparison::Changed;
    }
    bool CopyPublishedBytes(std::uint64_t address, void* destination, std::size_t size) const {
        const auto previous = snapshots.find(address);
        if (destination == nullptr || previous == snapshots.end() ||
            previous->second.size != size || previous->second.bytes.size() != size) return false;
        std::memcpy(destination, previous->second.bytes.data(), size);
        return true;
    }
    void Forget(std::uint64_t address, std::uint64_t size) {
        if (size == 0) return;
        if (size > UINT64_MAX - address) {
            snapshots.clear();
            return;
        }
        const auto end = address + size;
        for (auto it = snapshots.begin(); it != snapshots.end();) {
            if (it->second.size > UINT64_MAX - it->first ||
                (it->first < end && address < it->first + it->second.size))
                it = snapshots.erase(it);
            else ++it;
        }
    }
private:
    struct Snapshot { std::size_t size; std::uint64_t hash; std::vector<uint8_t> bytes; bool publication_witness = false; };
    std::unordered_map<std::uint64_t, Snapshot> snapshots;
};

} // namespace Libs::Graphics
#endif
