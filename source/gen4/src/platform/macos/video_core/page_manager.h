// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

class PageManager {
    // PAGE_SIZE and PAGE_BITS conflicts with machine/param.h definitions on freebsd!
    // Use the same page size as the tracker.
    static constexpr size_t PM_PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PM_PAGE_SIZE = TRACKER_BYTES_PER_PAGE;

    // Keep the lock granularity the same as region granularity. (since each regions has
    // itself a lock)
    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);

    bool IsMapped(VAddr address, u64 size) const;
    bool HasReadWatch(VAddr address, u64 size) const;
    u64 HashPage(VAddr address) const;

    /// Updates watches in the pages touching the specified region.
    template <bool track, bool is_read = false>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    /// Updates watches in the pages touching the specified region using a mask.
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    // Called only while the CPU is excluded by the shared runtime publication.
    void InvalidateCpuChanges();
    void CommitPublishedMemory();
    [[nodiscard]] bool HasPendingPublicationRefresh();

    // Write a GPU readback to backing and refresh every tracked VA alias of
    // the physical bytes. Failure propagates without advancing any baseline.
    bool PublishBackingWrite(VAddr address, const void* data, u64 size);

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PM_PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PM_PAGE_SIZE);
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore
