// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"

#ifdef __unix__
#include "common/adaptive_mutex.h"
#else
#include "common/spin_lock.h"
#endif
#include "common/debug.h"
#include "common/spin_lock.h"
#include "common/types.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/buffer_cache/cpu_write_journal.h"
#include "video_core/page_manager.h"

namespace VideoCore {

#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
using LockType = Common::AdaptiveMutex;
#else
using LockType = Common::SpinLock;
#endif

/**
 * Allows tracking CPU and GPU modification of pages in a contigious 16MB virtual address region.
 * Information is stored in bitsets for spacial locality and fast update of single pages.
 */
class RegionManager {
public:
    explicit RegionManager(PageManager* tracker_, VAddr cpu_addr_)
        : tracker{tracker_}, cpu_addr{cpu_addr_} {
        cpu.Fill();
        gpu.Clear();
        writeable.Fill();
        readable.Fill();
    }
    explicit RegionManager() = default;

    void SetCpuAddress(VAddr new_cpu_addr) {
        cpu_addr = new_cpu_addr;
    }

    VAddr GetCpuAddr() const {
        return cpu_addr;
    }

    u64 CpuGeneration() const {
        return cpu_generation.load(std::memory_order_acquire);
    }

    bool HasPendingCpuWrites() const {
        return has_pending_cpu_writes.load(std::memory_order_acquire);
    }

    void ProcessPendingCpuWrites() {
        if (!HasPendingCpuWrites()) {
            return;
        }
        const RegionBits pending = pending_cpu;
        pending_cpu.Clear();
        has_pending_cpu_writes.store(false, std::memory_order_release);
        UpdateProtection<true, false>();
        for (const auto& [start, end] : pending) {
            const VAddr address = cpu_addr + start * TRACKER_BYTES_PER_PAGE;
            const u64 size = (end - start) * TRACKER_BYTES_PER_PAGE;
            if (!tracker->IsMapped(address, size)) {
                continue;
            }
            for (size_t page = start; page < end; ++page) {
                const VAddr page_address = cpu_addr + page * TRACKER_BYTES_PER_PAGE;
                if (tracker->HashPage(page_address) != (*cpu_hashes)[page]) {
                    ChangeRegionState<Type::CPU, true>(page_address, TRACKER_BYTES_PER_PAGE);
                }
            }
        }
    }

    void MarkCpuWriteFault(VAddr address, u64 size) {
        const size_t offset = address - cpu_addr;
        const size_t first = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t last = std::min<size_t>(
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE),
            NUM_PAGES_PER_REGION);
        bool hot = false;
        if (last > first && last - first <= 4) {
            for (size_t page = first; page < last; ++page) {
                if (writeable.Get(page) || gpu.Get(page)) {
                    continue;
                }
                if (!write_fault_counts) {
                    write_fault_counts = std::make_unique<std::array<u8, NUM_PAGES_PER_REGION>>();
                }
                auto& count = (*write_fault_counts)[page];
                if (count < 3) {
                    ++count;
                }
                if (count == 3) {
                    hot_cpu_pages.Set(page);
                    hot = true;
                }
            }
        }
        if (hot) {
            size_t begin = first;
            size_t end = last;
            for (size_t count = 0; count < 64 && begin != 0; ++count) {
                const size_t page = begin - 1;
                if (!hot_cpu_pages.Get(page) || gpu.Get(page)) {
                    break;
                }
                begin = page;
            }
            for (size_t count = 0; count < 64 && end < NUM_PAGES_PER_REGION; ++count) {
                if (!hot_cpu_pages.Get(end) || gpu.Get(end)) {
                    break;
                }
                ++end;
            }
            address = cpu_addr + begin * TRACKER_BYTES_PER_PAGE;
            size = (end - begin) * TRACKER_BYTES_PER_PAGE;
        }
        ChangeRegionState<Type::CPU, true>(address, size);
    }

    static constexpr size_t SanitizeAddress(size_t address) {
        return static_cast<size_t>(std::max<s64>(static_cast<s64>(address), 0LL));
    }

    template <Type type>
    RegionBits& GetRegionBits() noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    template <Type type>
    const RegionBits& GetRegionBits() const noexcept {
        if constexpr (type == Type::CPU) {
            return cpu;
        } else if constexpr (type == Type::GPU) {
            return gpu;
        }
    }

    /**
     * Change the state of a range of pages
     *
     * @param dirty_addr    Base address to mark or unmark as modified
     * @param size          Size in bytes to mark or unmark as modified
     */
    template <Type type, bool enable>
    void ChangeRegionState(u64 dirty_addr, u64 size) noexcept(type == Type::GPU) {
        RENDERER_TRACE;
        const size_t offset = dirty_addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        if constexpr (type == Type::CPU && enable) {
            cpu_generation.fetch_add(1, std::memory_order_release);
            CpuWriteJournal::Instance().Record(dirty_addr, size);
            pending_cpu.UnsetRange(start_page, end_page);
        } else if constexpr (type == Type::GPU && enable) {
            hot_cpu_pages.UnsetRange(start_page, end_page);
            if (HasPendingCpuWrites()) {
                pending_cpu.UnsetRange(start_page, end_page);
                UpdateProtection<true, false>();
            }
        }

        RegionBits& bits = GetRegionBits<type>();
        if constexpr (enable) {
            bits.SetRange(start_page, end_page);
        } else {
            bits.UnsetRange(start_page, end_page);
        }
        if constexpr (type == Type::CPU) {
            UpdateProtection<!enable, false>();
        } else if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise) {
            UpdateProtection<enable, true>();
        }
    }

    /**
     * Loop over each page in the given range, turn off those bits and notify the tracker if
     * needed. Call the given function on each turned off range.
     *
     * @param query_cpu_range Base CPU address to loop over
     * @param size            Size in bytes of the CPU range to loop over
     * @param func            Function to call for each turned off region
     */
    template <Type type, bool clear>
    void ForEachModifiedRange(VAddr query_cpu_range, s64 size, auto&& func) {
        RENDERER_TRACE;
        const size_t offset = query_cpu_range - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return;
        }

        RegionBits& bits = GetRegionBits<type>();
        RegionBits mask(bits, start_page, end_page);

        if constexpr (clear) {
            bits.UnsetRange(start_page, end_page);
            if constexpr (type == Type::CPU) {
                CapturePendingCpuWrites(mask);
                UpdateProtection<true, false>();
            } else if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                UpdateProtection<false, true>();
            }
        }

        for (const auto& [start, end] : mask) {
            func(cpu_addr + start * TRACKER_BYTES_PER_PAGE, (end - start) * TRACKER_BYTES_PER_PAGE);
        }
    }

    /**
     * Returns true when a region has been modified
     *
     * @param offset Offset in bytes from the start of the buffer
     * @param size   Size in bytes of the region to query for modifications
     */
    template <Type type>
    [[nodiscard]] bool IsRegionModified(u64 offset, u64 size) noexcept {
        RENDERER_TRACE;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }

        const RegionBits& bits = GetRegionBits<type>();
        RegionBits test(bits, start_page, end_page);
        return test.Any();
    }

    LockType lock;

private:
    void CapturePendingCpuWrites(const RegionBits& mask) {
        size_t page_count = 0;
        for (const auto& [start, end] : mask) {
            page_count += end - start;
            if (page_count > 64) {
                return;
            }
        }
        if (page_count == 0) {
            return;
        }
        for (const auto& [start, end] : mask) {
            if (!tracker->IsMapped(cpu_addr + start * TRACKER_BYTES_PER_PAGE,
                                   (end - start) * TRACKER_BYTES_PER_PAGE)) {
                return;
            }
        }
        if (!cpu_hashes) {
            cpu_hashes = std::make_unique<std::array<u64, NUM_PAGES_PER_REGION>>();
        }
        for (const auto& [start, end] : mask) {
            for (size_t page = start; page < end; ++page) {
                if (gpu.Get(page)) {
                    continue;
                }
                (*cpu_hashes)[page] = tracker->HashPage(cpu_addr + page * TRACKER_BYTES_PER_PAGE);
                pending_cpu.Set(page);
                has_pending_cpu_writes.store(true, std::memory_order_release);
            }
        }
    }

    /**
     * Notify tracker about changes in the CPU tracking state of a word in the buffer
     *
     * @param word_index   Index to the word to notify to the tracker
     * @param current_bits Current state of the word
     * @param new_bits     New state of the word
     *
     * @tparam track True when the tracker should start tracking the new pages
     */
    template <bool track, bool is_read>
    void UpdateProtection() {
        RENDERER_TRACE;
        RegionBits mask = is_read ? (~gpu ^ readable) : ((cpu | pending_cpu) ^ writeable);
        if (mask.None()) {
            return;
        }
        if constexpr (is_read) {
            readable = ~gpu;
        } else {
            writeable = cpu | pending_cpu;
        }
        tracker->UpdatePageWatchersForRegion<track, is_read>(cpu_addr, mask);
    }

    PageManager* tracker;
    VAddr cpu_addr = 0;
    RegionBits cpu;
    RegionBits gpu;
    RegionBits writeable;
    RegionBits readable;
    RegionBits pending_cpu;
    std::unique_ptr<std::array<u64, NUM_PAGES_PER_REGION>> cpu_hashes;
    RegionBits hot_cpu_pages;
    std::unique_ptr<std::array<u8, NUM_PAGES_PER_REGION>> write_fault_counts;
    std::atomic<bool> has_pending_cpu_writes{};
    std::atomic<u64> cpu_generation{1};
};

} // namespace VideoCore
