// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include <xxhash.h>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/fex/guest_memory_scope.h"
#include "video_core/guest_publication_snapshots.h"
#endif
#include "common/assert.h"
#include "common/debug.h"
#include "common/spin_lock.h"
#include "common/div_ceil.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/host_page_watch.h"
#include "video_core/renderer_vulkan/bounded_draw_diagnostics.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#include <unistd.h>
#include "common/adaptive_mutex.h"
#ifdef ENABLE_USERFAULTFD
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "common/error.h"
#endif
#else
#include <windows.h>
#include "common/spin_lock.h"
#endif

#ifdef __linux__
#include "common/adaptive_mutex.h"
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

struct PageManager::Impl {
    struct PageState {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        // Overlapping image views may exceed the fault watcher's seven-bit
        // count. Publication metadata must never wrap and silently untrack.
        u32 num_write_watchers{};
        u32 num_read_watchers{};
#else
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;
#endif

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <s32 delta, bool is_read>
        u32 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;
#if defined(LUMEN_GEN4_DIRECT_FEX)
    Impl(Vulkan::Rasterizer* rasterizer_) : host_pages{static_cast<size_t>(sysconf(_SC_PAGESIZE))} {
        rasterizer = rasterizer_;
        fault_owner = this;
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler, 0);
    }

    void OnMap(VAddr address, size_t size) {
        // PRT ranges can carry logical GPU watches while their physical VM
        // pages are absent. A later pool commit must arm those watches now
        // that the complete native protection granule exists.
        std::scoped_lock lock{watchers_mutex};
        host_pages.ForEachHostPage(address, size, [&](VAddr host_page) {
            for (size_t offset = 0; offset < host_pages.Size(); offset += PM_PAGE_SIZE) {
                if (cached_pages.contains((host_page + offset) >> PM_PAGE_BITS)) {
                    Protect(host_page, host_pages.Size(), Core::MemoryPermission::None);
                    break;
                }
            }
        });
    }

    void OnUnmap(VAddr address, size_t size) {
        // The VM removes its mapping after the cache has retired its watches.
        // Forget metadata only: a partial range must never restore permissions
        // in an adjacent mapping or a host page which is being unmapped.
        std::scoped_lock lock{watchers_mutex};
        const auto end = address + size;
        for (auto it = cached_pages.begin(); it != cached_pages.end();) {
            const auto page = it->first * PM_PAGE_SIZE;
            if (page < end && page + PM_PAGE_SIZE > address) it = cached_pages.erase(it);
            else ++it;
        }
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission) {
        auto* memory = Core::Memory::Instance();
        host_pages.ForEachHostPage(address, size, [&](VAddr host_page) {
            // Never grant permissions forbidden by the current VM. GPU watches
            // are 4 KiB, while Darwin changes a complete 16 KiB host page.
            const auto permissions = host_pages.Permissions(host_page, [&](VAddr page) {
                void* begin{};
                void* end{};
                u32 raw{};
                if (memory->QueryProtection(page, &begin, &end, &raw) != 0 ||
                    reinterpret_cast<VAddr>(begin) > page ||
                    reinterpret_cast<VAddr>(end) - page < PM_PAGE_SIZE)
                    return HostPageWatch::Unavailable;
                const auto logical = static_cast<Core::MemoryProt>(raw);
                u8 allowed{};
                if (True(logical & (Core::MemoryProt::CpuRead | Core::MemoryProt::GpuRead)))
                    allowed |= HostPageWatch::Read;
                if (True(logical & (Core::MemoryProt::CpuWrite | Core::MemoryProt::GpuWrite)))
                    allowed |= HostPageWatch::Read | HostPageWatch::Write;
                if (const auto it = cached_pages.find(page >> PM_PAGE_BITS); it != cached_pages.end()) {
                    if (it->second.num_read_watchers) allowed &= ~HostPageWatch::Read;
                    if (it->second.num_write_watchers) allowed &= ~HostPageWatch::Write;
                }
                return allowed;
            });
            if (permissions == HostPageWatch::Unavailable) return;
            auto native_permissions = Core::MemoryPermission::None;
            if (permissions & HostPageWatch::Read) native_permissions |= Core::MemoryPermission::Read;
            if (permissions & HostPageWatch::Write) native_permissions |= Core::MemoryPermission::Write;
            memory->GetAddressSpace().Protect(host_page, host_pages.Size(), native_permissions);
        });
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        auto& owner = *fault_owner;
        const auto address = reinterpret_cast<VAddr>(fault_address);
        const bool write = Common::IsWriteError(context);
        const auto blocked = [&] {
            std::scoped_lock lock{owner.watchers_mutex};
            return owner.host_pages.FaultPages(address, write, [&](VAddr page) {
                const auto it = owner.cached_pages.find(page >> PM_PAGE_BITS);
                if (it == owner.cached_pages.end()) return u8{};
                return static_cast<u8>((it->second.num_read_watchers ? HostPageWatch::Read : 0) |
                                       (it->second.num_write_watchers ? HostPageWatch::Write : 0));
            });
        }();
        // Resolve every watched subpage, including a neighbor of the actual
        // fault. Otherwise its watch would leave this host page protected and
        // retrying the original instruction would fault forever.
        bool handled = blocked.count != 0;
        for (size_t index = 0; index < blocked.count; ++index) {
            const auto page = blocked.pages[index];
            handled &= write ? rasterizer->InvalidateMemory(page, PM_PAGE_SIZE, true)
                             : rasterizer->ReadMemory(page, PM_PAGE_SIZE);
        }
        return handled;
    }

    HostPageWatch host_pages;
    inline static Impl* fault_owner{};
#elif defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU)
    Impl(Vulkan::Rasterizer* rasterizer_) { rasterizer = rasterizer_; }

    void OnMap(VAddr, size_t) {}

    void OnUnmap(VAddr address, size_t size) {
        std::scoped_lock lock{snapshot_mutex};
        snapshots.UnmapRange(address, size);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        ASSERT_MSG(Core::Fex::MemoryMutationScope::Active(), "Page tracking outside publication");
        // Logical watchers are cache metadata. Host page permissions belong to
        // the VM and are never repurposed to discover CPU/GPU writes.
        std::scoped_lock lock{snapshot_mutex};
        for (VAddr page = address; page < address + size; page += PM_PAGE_SIZE) {
            if (perms == Core::MemoryPermission::ReadWrite) {
                snapshots.Untrack(page);
            } else {
                snapshots.Track(page, [](VAddr address, auto bytes) {
                    Core::Memory::Instance()->CopySparseMemory(address, bytes.data(), bytes.size());
                });
            }
        }
    }

    void InvalidateCpuChanges() {
        ASSERT(Core::Fex::MemoryMutationScope::Active());
        std::vector<std::uint64_t> changed;
        {
            std::scoped_lock lock{snapshot_mutex};
            if (Vulkan::DrawDiagnostic::Active())
                Vulkan::DrawDiagnostic::PublicationScan(snapshots.TrackedPageCount());
            changed = snapshots.ObserveCpuChanges([](VAddr address, auto bytes) {
                Core::Memory::Instance()->CopySparseMemory(address, bytes.data(), bytes.size());
            });
        }
        // Invalidation updates watcher counts, so no snapshot lock is held.
        for (const auto page : changed) rasterizer->InvalidateMemory(page, PM_PAGE_SIZE);
    }

    void CommitPublishedMemory() {
        ASSERT(Core::Fex::MemoryMutationScope::Active());
        std::scoped_lock lock{snapshot_mutex};
        snapshots.CommitPublishedMemory([](VAddr address, auto bytes) {
            Core::Memory::Instance()->CopySparseMemory(address, bytes.data(), bytes.size());
        });
    }

    bool HasPendingPublicationRefresh() {
        ASSERT(Core::Fex::MemoryMutationScope::Active());
        std::scoped_lock lock{snapshot_mutex};
        return snapshots.HasPendingRefresh();
    }

    bool RecordBackingWrite(VAddr address, u64 size, bool succeeded) {
        if (!succeeded) return false;
        ASSERT(Core::Fex::MemoryMutationScope::Active());
        std::scoped_lock lock{snapshot_mutex};
        return snapshots.RecordBackingWrite(address, size, true);
    }

    std::mutex snapshot_mutex;
    GuestPublicationSnapshots snapshots;
#elif defined(ENABLE_USERFAULTFD)
    Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
        uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        ASSERT_MSG(uffd != -1, "{}", Common::GetLastErrorMsg());

        // Request uffdio features from kernel.
        uffdio_api api;
        api.api = UFFD_API;
        api.features = UFFD_FEATURE_THREAD_ID;
        const int ret = ioctl(uffd, UFFDIO_API, &api);
        ASSERT(ret == 0 && api.api == UFFD_API);

        // Create uffd handler thread
        ufd_thread = std::jthread([&](std::stop_token token) { UffdHandler(token); });
    }

    void OnMap(VAddr address, size_t size) {
        uffdio_register reg;
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_REGISTER, &reg);
        ASSERT_MSG(ret != -1, "Uffdio register failed");
    }

    void OnUnmap(VAddr address, size_t size) {
        uffdio_range range;
        range.start = address;
        range.len = size;
        const int ret = ioctl(uffd, UFFDIO_UNREGISTER, &range);
        ASSERT_MSG(ret != -1, "Uffdio unregister failed");
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        bool allow_write = True(perms & Core::MemoryPermission::Write);
        uffdio_writeprotect wp;
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? 0 : UFFDIO_WRITEPROTECT_MODE_WP;
        const int ret = ioctl(uffd, UFFDIO_WRITEPROTECT, &wp);
        ASSERT_MSG(ret != -1, "Uffdio writeprotect failed with error: {}",
                   Common::GetLastErrorMsg());
    }

    void UffdHandler(std::stop_token token) {
        while (!token.stop_requested()) {
            pollfd pollfd;
            pollfd.fd = uffd;
            pollfd.events = POLLIN;

            // Block until the descriptor is ready for data reads.
            const int pollres = poll(&pollfd, 1, -1);
            switch (pollres) {
            case -1:
                perror("Poll userfaultfd");
                continue;
                break;
            case 0:
                continue;
            case 1:
                break;
            default:
                UNREACHABLE_MSG("Unexpected number of descriptors {} out of poll", pollres);
            }

            // We don't want an error condition to have occured.
            ASSERT_MSG(!(pollfd.revents & POLLERR), "POLLERR on userfaultfd");

            // We waited until there is data to read, we don't care about anything else.
            if (!(pollfd.revents & POLLIN)) {
                continue;
            }

            // Read message from kernel.
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            ASSERT_MSG(readret != -1 || errno == EAGAIN, "Unexpected result of uffd read");
            if (errno == EAGAIN) {
                continue;
            }
            ASSERT_MSG(readret == sizeof(msg), "Unexpected short read, exiting");
            ASSERT(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP);

            // Notify rasterizer about the fault.
            const VAddr addr = msg.arg.pagefault.address;
            rasterizer->InvalidateMemory(addr, 1, true);
        }
    }

    std::jthread ufd_thread;
    int uffd;
#else
    Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void OnMap(VAddr address, size_t size) {
        // No-op
    }

    void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        if (Common::IsWriteError(context)) {
            return rasterizer->InvalidateMemory(addr, 8, true);
        } else {
            return rasterizer->ReadMemory(addr, 8);
        }
        return false;
    }
#endif

    bool HasReadWatch(VAddr address, u64 size) {
        if (size == 0) return false;
        constexpr u64 AddressLimit = u64{1} << ADDRESS_BITS;
        if (address >= AddressLimit || size > AddressLimit - address) return true;
        const auto first = address >> PM_PAGE_BITS;
        const auto end = Common::DivCeil(address + size, PM_PAGE_SIZE);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        std::scoped_lock lock{watchers_mutex};
        for (auto page = first; page < end; ++page) {
            const auto it = cached_pages.find(page);
            if (it != cached_pages.end() && it->second.num_read_watchers) return true;
        }
#else
        const auto lock_start = locks.begin() + (first / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(end, PAGES_PER_LOCK);
        Common::RangeLockGuard lock{lock_start, lock_end};
        for (auto page = first; page < end; ++page) {
            if (cached_pages[page].num_read_watchers) return true;
        }
#endif
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        ASSERT(Core::Fex::MemoryMutationScope::Active());
        std::scoped_lock lk{watchers_mutex};
#else
        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);
#endif

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u32 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
            if (state.Perms() == Core::MemoryPermission::ReadWrite) cached_pages.erase(page);
#endif
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
        ASSERT(Core::Fex::MemoryMutationScope::Active());
        std::scoped_lock lk{watchers_mutex};
#else
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
#endif
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u32 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
                if (state.Perms() == Core::MemoryPermission::ReadWrite)
                    cached_pages.erase(base_page + page);
#endif
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
            if (state.Perms() == Core::MemoryPermission::ReadWrite)
                cached_pages.erase(base_page + page);
#endif
        }

        // Add pending (un)protect action
        release_pending();
    }

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    std::mutex watchers_mutex;
    std::unordered_map<size_t, PageState> cached_pages;
#else
    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
#endif
};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_)
    : impl{std::make_unique<Impl>(rasterizer_)} {}

PageManager::~PageManager() = default;

bool PageManager::IsMapped(VAddr address, u64 size) const {
    return impl->rasterizer->IsMapped(address, size);
}

bool PageManager::HasReadWatch(VAddr address, u64 size) const {
    return impl->HasReadWatch(address, size);
}

u64 PageManager::HashPage(VAddr address) const {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    ASSERT(Core::Fex::MemoryMutationScope::Active());
    std::array<u8, PM_PAGE_SIZE> bytes;
#ifdef LUMEN_GEN4_DIRECT_FEX
    // RegionManager only hashes CPU-owned pages, while holding its lock. A
    // GPU-owned neighboring subpage can still protect this host page; hashing
    // its backing avoids re-entering the same cache lock through a read fault.
    Core::Memory::Instance()->CopySparseMemoryFromBacking(address, bytes.data(), bytes.size());
#else
    Core::Memory::Instance()->CopySparseMemory(address, bytes.data(), bytes.size());
#endif
    return XXH3_64bits(bytes.data(), bytes.size());
#else
    return XXH3_64bits(reinterpret_cast<const void*>(address), PM_PAGE_SIZE);
#endif
}

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

void PageManager::InvalidateCpuChanges() {
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
    impl->InvalidateCpuChanges();
#endif
}

void PageManager::CommitPublishedMemory() {
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
    impl->CommitPublishedMemory();
#endif
}

bool PageManager::HasPendingPublicationRefresh() {
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
    return impl->HasPendingPublicationRefresh();
#else
    return false;
#endif
}

bool PageManager::PublishBackingWrite(VAddr address, const void* data, u64 size) {
#if defined(LUMEN_GEN4_ENABLE_FEX_GUEST_CPU) && !defined(LUMEN_GEN4_DIRECT_FEX)
    std::vector<std::pair<VAddr, u64>> written_aliases;
    if (!Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(address), data, size,
                                                   &written_aliases)) return false;
    // The VM lock is released before taking the snapshot lock. Use the exact
    // alias ranges published by the writer, including distinct virtual views.
    for (const auto& [alias, length] : written_aliases)
        impl->RecordBackingWrite(alias, length, true);
    return true;
#else
    return Core::Memory::Instance()->TryWriteBacking(reinterpret_cast<void*>(address), data, size);
#endif
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, is_read>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<true, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
