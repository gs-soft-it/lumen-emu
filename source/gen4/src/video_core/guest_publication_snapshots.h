// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VideoCore {

// Private cache baselines, used only while guest execution is excluded by a
// publication. The owner serializes all calls. Host page permissions and guest
// visibility are never changed by this bookkeeping.
class GuestPublicationSnapshots final {
public:
    static constexpr std::size_t PageSize = 4096;
    using Page = std::array<std::uint8_t, PageSize>;

    template <typename ReadPage>
    void Track(std::uint64_t page, ReadPage&& read_page) {
        assert(page % PageSize == 0);
        if (auto [it, inserted] = snapshots.try_emplace(page); inserted)
            read_page(page, std::span{it->second});
    }

    void Untrack(std::uint64_t page) {
        snapshots.erase(page);
        refresh_pages.erase(page);
    }

    void UnmapRange(std::uint64_t address, std::uint64_t size) {
        if (size == 0) return;
        assert(size <= std::numeric_limits<std::uint64_t>::max() - address);
        const auto first = address & ~(std::uint64_t{PageSize} - 1);
        const auto last = (address + size - 1) & ~(std::uint64_t{PageSize} - 1);
        for (auto it = snapshots.begin(); it != snapshots.end();) {
            if (first <= it->first && it->first <= last) {
                refresh_pages.erase(it->first);
                it = snapshots.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Record all changed pages before the owner invalidates their caches. A
    // watcher may survive invalidation, in which case its baseline needs the
    // final published bytes. Untrack/remap removes the old pending identity.
    template <typename ReadPage>
    std::vector<std::uint64_t> ObserveCpuChanges(ReadPage&& read_page) {
        std::vector<std::uint64_t> changed;
        Page bytes{};
        for (const auto& [page, baseline] : snapshots) {
            read_page(page, std::span{bytes});
            if (bytes != baseline) {
                changed.push_back(page);
                refresh_pages.insert(page);
            }
        }
        return changed;
    }

    // Call with the actual backing-write result. A failed write remains a
    // failure for the caller and cannot silently advance any baseline.
    bool RecordBackingWrite(std::uint64_t address, std::uint64_t size, bool succeeded) {
        if (!succeeded || size == 0) return succeeded;
        assert(size <= std::numeric_limits<std::uint64_t>::max() - address);
        const auto first = address & ~(std::uint64_t{PageSize} - 1);
        const auto last = (address + size - 1) & ~(std::uint64_t{PageSize} - 1);
        for (auto page = first;; page += PageSize) {
            if (snapshots.contains(page)) refresh_pages.insert(page);
            if (page == last) break;
        }
        return true;
    }

    [[nodiscard]] std::size_t TrackedPageCount() const { return snapshots.size(); }

    [[nodiscard]] bool HasPendingRefresh() const { return !refresh_pages.empty(); }

    template <typename ReadPage>
    std::size_t CommitPublishedMemory(ReadPage&& read_page) {
        std::size_t copied{};
        for (const auto page : refresh_pages) {
            if (const auto it = snapshots.find(page); it != snapshots.end()) {
                read_page(page, std::span{it->second});
                ++copied;
            }
        }
        refresh_pages.clear();
        return copied;
    }

private:
    std::unordered_map<std::uint64_t, Page> snapshots;
    std::unordered_set<std::uint64_t> refresh_pages;
};

} // namespace VideoCore
