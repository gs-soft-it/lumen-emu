// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <xxhash.h>
#include "video_core/buffer_cache/buffer_pending_write_resolve.h"
#include "video_core/buffer_cache/buffer_upload_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/stream_upload_cache.h"

namespace {
using namespace VideoCore;
constexpr VAddr Base = 0x400000;
constexpr u64 Page = TRACKER_BYTES_PER_PAGE;
constexpr u64 Region = TRACKER_HIGHER_PAGE_SIZE;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Event {
    enum Kind { Hash, TrackWrite, UntrackWrite, TrackRead, UntrackRead } kind;
    VAddr address;
};
struct HostPages {
    std::vector<u8> data = std::vector<u8>(2 * Region);
    std::vector<bool> mapped = std::vector<bool>(2 * NUM_PAGES_PER_REGION, true);
    std::vector<bool> writable = std::vector<bool>(2 * NUM_PAGES_PER_REGION, true);
    std::vector<Event> events;
    void Reset() {
        std::fill(data.begin(), data.end(), 0);
        std::fill(mapped.begin(), mapped.end(), true);
        std::fill(writable.begin(), writable.end(), true);
        events.clear();
        std::vector<CpuWriteJournal::Range> ignored;
        CpuWriteJournal::Instance().Consume(ignored);
    }
    size_t Hashes() const {
        return std::count_if(events.begin(), events.end(), [](auto e) { return e.kind == Event::Hash; });
    }
} host;

std::vector<CpuWriteJournal::Range> Drain() {
    std::vector<CpuWriteJournal::Range> result;
    Require(!CpuWriteJournal::Instance().Consume(result), "unexpected journal overflow");
    return result;
}

void Upload(RegionManager& region, VAddr address, u64 size) {
    region.ForEachModifiedRange<Type::CPU, true>(address, size, [](VAddr, u64) {});
}

void ThresholdsAndRearm() {
    for (const size_t pages : {size_t{1}, size_t{64}, size_t{65}}) {
        host.Reset();
        PageManager tracker{nullptr};
        RegionManager region{&tracker, Base};
        Upload(region, Base, pages * Page);
        Require(!region.IsRegionModified<Type::CPU>(0, pages * Page), "upload clears CPU dirty");
        Require(region.HasPendingCpuWrites() == (pages <= 64), "original 64-page threshold");
        Require(host.Hashes() == (pages <= 64 ? pages : 0), "hash only small upload");
        Require(host.writable[0] == (pages <= 64), "small upload keeps pending pages writable");
        if (pages <= 64) {
            host.events.clear();
            region.ProcessPendingCpuWrites();
            Require(!region.HasPendingCpuWrites(), "resolver clears pending flag");
            Require(!host.writable[0], "resolver rearms page");
            Require(host.Hashes() == pages, "resolver hashes all selected pages");
            Require(host.events.front().kind == Event::TrackWrite, "rearm precedes comparison");
            Require(region.CpuGeneration() == 1 && Drain().empty(), "unchanged data stays clean");
        }
    }
    host.Reset();
    PageManager tracker{nullptr};
    RegionManager region{&tracker, Base};
    Upload(region, Base, 65 * Page);
    region.ChangeRegionState<Type::CPU, true>(Base, Page);
    region.ChangeRegionState<Type::CPU, true>(Base + 64 * Page, Page);
    host.events.clear();
    Drain();
    Upload(region, Base, 65 * Page);
    Require(region.HasPendingCpuWrites() && host.Hashes() == 2,
            "threshold counts selected pages, not covered span");
}

void MappingAndGpuDirty() {
    host.Reset();
    PageManager tracker{nullptr};
    RegionManager region{&tracker, Base};
    Upload(region, Base, 65 * Page);
    region.ChangeRegionState<Type::CPU, true>(Base, Page);
    region.ChangeRegionState<Type::CPU, true>(Base + 64 * Page, Page);
    host.mapped[64] = false;
    host.events.clear();
    Upload(region, Base, 65 * Page);
    Require(host.Hashes() == 0 && !region.HasPendingCpuWrites(),
            "validate every selected range before arming any page");

    host.Reset();
    RegionManager gpu_region{&tracker, Base};
    gpu_region.ChangeRegionState<Type::GPU, true>(Base, Page);
    Upload(gpu_region, Base, 2 * Page);
    Require(host.Hashes() == 1 && !host.writable[0] && host.writable[1],
            "GPU dirty page excluded from pending CPU capture");
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    host.events.clear();
    gpu_region.ChangeRegionState<Type::GPU, true>(Base + Page, Page);
    Require(!host.writable[1], "GPU write cancels pending page and rearms");
    Require(host.events.front().kind == Event::TrackWrite &&
                host.events.back().kind == Event::TrackRead,
            "pending CPU cancellation rearms before precise GPU read protection");
    EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Disabled);
    host.events.clear();
    gpu_region.ProcessPendingCpuWrites();
    Require(host.Hashes() == 0 && !gpu_region.HasPendingCpuWrites(),
            "cancelled pending mask resolves without a hash");

    host.Reset();
    RegionManager many_gpu_pages{&tracker, Base};
    many_gpu_pages.ChangeRegionState<Type::GPU, true>(Base, 64 * Page);
    Upload(many_gpu_pages, Base, 65 * Page);
    Require(host.Hashes() == 0 && !many_gpu_pages.HasPendingCpuWrites(),
            "threshold counts pages before excluding GPU dirty pages");

    host.Reset();
    RegionManager removed_region{&tracker, Base};
    Upload(removed_region, Base, Page);
    host.mapped[0] = false;
    host.events.clear();
    removed_region.ProcessPendingCpuWrites();
    Require(host.Hashes() == 0 && !host.writable[0], "unmapped pending range rearms then skips hash");
}

void ChangedDataAndCancellation() {
    host.Reset();
    PageManager tracker{nullptr};
    RegionManager region{&tracker, Base};
    Upload(region, Base, 3 * Page);
    host.data[Page + 7] = 0x7e;
    host.events.clear();
    region.ProcessPendingCpuWrites();
    Require(region.CpuGeneration() == 2, "changed page increments generation once");
    Require(!region.IsRegionModified<Type::CPU>(0, Page) &&
                region.IsRegionModified<Type::CPU>(Page, Page) &&
                !region.IsRegionModified<Type::CPU>(2 * Page, Page), "only changed page becomes dirty");
    Require(!host.writable[0] && host.writable[1] && !host.writable[2],
            "only changed page is made writable again");
    Require(Drain() == std::vector<CpuWriteJournal::Range>{{Base + Page, Page}},
            "changed page records exact page in journal");
    Upload(region, Base + Page, Page);
    region.ChangeRegionState<Type::CPU, true>(Base + Page + 9, 7);
    const auto generation = region.CpuGeneration();
    region.ChangeRegionState<Type::CPU, true>(Base + Page + 9, 7);
    Require(region.CpuGeneration() == generation + 1, "explicit repeat CPU mark still increments");
    Require(Drain() == std::vector<CpuWriteJournal::Range>{{Base + Page + 9, 7},
                                                          {Base + Page + 9, 7}},
            "explicit marks preserve raw range and repetitions");
    host.events.clear();
    region.ProcessPendingCpuWrites();
    Require(host.Hashes() == 0 && host.writable[1], "explicit CPU mark cancels pending hash");

    host.Reset();
    RegionManager partial{&tracker, Base};
    Upload(partial, Base, 3 * Page);
    partial.ChangeRegionState<Type::GPU, true>(Base + Page, Page);
    host.data[2 * Page] = 1;
    host.events.clear();
    partial.ProcessPendingCpuWrites();
    Require(host.Hashes() == 2 && partial.IsRegionModified<Type::CPU>(2 * Page, Page),
            "partial cancellation retains and compares other pages");
}

void GenerationCache() {
    host.Reset();
    PageManager tracker{nullptr};
    auto memory = std::make_unique<MemoryTracker>(tracker);
    BufferUploadCache::Generations generations;
    unsigned uploads = 0;
    memory->ForEachUploadRange(Base + Region - Page, 2 * Page, false,
                               [&](VAddr, u64) { ++uploads; }, [] {}, &generations);
    Require(uploads == 2 && generations == BufferUploadCache::Generations{1, 1},
            "capture generation under each region lock");
    BufferUploadCache cache;
    cache.Store(Base + Region - Page, 2 * Page, generations);
    auto hit = cache.Find(Base + Region, Page);
    Require(hit && hit->size() == 1 && memory->AreRegionGenerationsCurrent(Base + Region, Page, *hit),
            "covering cache uses requested region subspan");
    Require(!memory->AreRegionGenerationsCurrent(Base, Page, {}), "missing generation rejects hit");
    Require(!memory->AreRegionGenerationsCurrent(Base + 2 * Region, Page, std::array<u64, 1>{1}),
            "generation check does not create missing manager");
    memory->MarkRegionAsCpuModified(Base + Region + 12, 4);
    Require(!memory->AreRegionGenerationsCurrent(Base + Region, Page, *hit),
            "CPU mark invalidates generation cache");
    memory->ProcessPendingCpuWrites();
    Require(host.Hashes() == 3, "resolver visits remaining pending region, skips cancelled region");
    memory->ForEachUploadRange(Base + Region, Page, false, [](VAddr, u64) {}, [] {}, &generations);
    Require(generations == BufferUploadCache::Generations{2}, "upload snapshots current generation");
    cache.Store(Base + Region, Page, generations);
    hit = cache.Find(Base + Region, Page);
    Require(hit && (*hit)[0] == 2, "exact match takes precedence over covering old entry");
    cache.Clear();
    Require(!cache.Find(Base + Region, Page), "GPU writer clears upload cache");
}

void PendingWriteResolveSelector() {
    Require(BufferPendingWriteResolveSelectorEnabled(nullptr), "pending resolver default ON");
    Require(!BufferPendingWriteResolveSelectorEnabled("0"), "exact zero disables pending resolver");
    Require(BufferPendingWriteResolveSelectorEnabled("1"), "exact one enables pending resolver");
    for (const auto* value : {"", "00", "01", "10", "true", "false", "invalid", " 0", "0 "}) {
        Require(BufferPendingWriteResolveSelectorEnabled(value),
                "nonexact rollback keeps pending resolver ON");
    }
}

void PendingWriteReuse() {
    constexpr u64 Bytes = 32_KB; // Above the 16 KiB stream-copy path.
    constexpr size_t Position = Page + 16;
    constexpr u32 OldValue = 0x3f800000;
    constexpr u32 NewValue = 0x41200000;
    for (const bool gpu_writer : {false, true}) {
        for (const bool resolve : {false, true}) {
            host.Reset();
            std::memcpy(host.data.data() + Position, &OldValue, sizeof(u32));
            PageManager tracker{nullptr};
            auto memory = std::make_unique<MemoryTracker>(tracker);
            BufferUploadCache cache;
            std::vector<u8> uploaded(Bytes);
            unsigned uploads = 0;
            auto upload = [&](bool is_written) {
                BufferUploadCache::Generations generations;
                std::vector<std::pair<VAddr, u64>> copies;
                memory->ForEachUploadRange(
                    Base, Bytes, is_written,
                    [&](VAddr address, u64 size) { copies.emplace_back(address, size); },
                    [&] {
                        for (auto [address, size] : copies) {
                            std::memcpy(uploaded.data() + address - Base,
                                        host.data.data() + address - Base, size);
                            ++uploads;
                        }
                    }, is_written ? nullptr : &generations);
                if (!is_written) cache.Store(Base, Bytes, std::move(generations));
            };
            upload(false);
            Require(uploads == 1 && host.writable[1], "initial upload leaves writable pending page");
            // The first consumption is complete. This ordered store models an
            // owner WRITE_DATA or an already-published CPU write, not a race.
            std::memcpy(host.data.data() + Position, &NewValue, sizeof(u32));
            if (resolve) {
                Require(memory->ResolvePendingCpuWrites(Base, Bytes) == 1,
                        "resolver detects completed write before reuse");
            }
            const auto cached = cache.Find(Base, Bytes);
            Require(cached.has_value(), "initial upload has generation record");
            const bool current = memory->AreRegionGenerationsCurrent(Base, Bytes, *cached);
            Require(current != resolve, "pending resolution invalidates stale generation");
            Require(memory->IsRegionCpuModified(Base, Bytes) == resolve,
                    "pending resolution publishes CPU dirty before read or write binding");
            if (gpu_writer || !current) upload(gpu_writer);
            u32 observed{};
            std::memcpy(&observed, uploaded.data() + Position, sizeof(u32));
            Require(observed == (resolve ? NewValue : OldValue),
                    "resolver refreshes consumed bytes and disabled path retains witness");
            if (!gpu_writer && !resolve) {
                const auto previous_uploads = uploads;
                upload(false);
                Require(uploads == previous_uploads,
                        "bypassing generation cache alone cannot resolve pending writes");
            }
            if (gpu_writer) {
                Require(memory->IsRegionGpuModified(Base, Bytes), "write binding retains GPU ownership");
                // A later resolve must not overwrite GPU-owned data with CPU backing.
                host.events.clear();
                Require(memory->ResolvePendingCpuWrites(Base, Bytes) == 0 && host.Hashes() == 0 &&
                            !memory->IsRegionCpuModified(Base, Bytes),
                        "GPU ownership cancels pending hashes without CPU upload");
            }
        }
    }
}

void PendingWriteResolveScope() {
    host.Reset();
    PageManager tracker{nullptr};
    auto memory = std::make_unique<MemoryTracker>(tracker);
    BufferUploadCache::Generations generations;
    auto upload = [&](VAddr address, u64 size) {
        memory->ForEachUploadRange(address, size, false, [](VAddr, u64) {}, [] {}, &generations);
    };
    Require(memory->ResolvePendingCpuWrites(Base, 0) == 0 &&
                memory->ResolvePendingCpuWrites(Base + 2 * Region, Page) == 0 && host.Hashes() == 0,
            "zero range and absent managers are no-ops");
    Require(!memory->AreRegionGenerationsCurrent(Base + 2 * Region, Page, std::array<u64, 1>{1}),
            "resolver does not allocate absent managers");
    upload(Base, Page);
    upload(Base + Region, Page);
    const auto unchanged_generation = generations;
    host.events.clear();
    Require(memory->ResolvePendingCpuWrites(Base, Page) == 0 && host.Hashes() == 1 &&
                memory->AreRegionGenerationsCurrent(Base, Page, unchanged_generation),
            "unchanged pending data preserves generation cache");
    Require(!host.writable[0] && host.writable[NUM_PAGES_PER_REGION],
            "resolver rearms only intersecting regions");
    host.data[Region + 16] = 0x7e;
    host.events.clear();
    Require(memory->ResolvePendingCpuWrites(Base, Page) == 0 && host.events.empty() &&
                !memory->IsRegionCpuModified(Base + Region, Page),
            "pending writes in another region remain untouched");
    Require(memory->ResolvePendingCpuWrites(Base + Region, Page) == 1 &&
                memory->IsRegionCpuModified(Base + Region, Page),
            "later scoped resolution detects the other region");

    host.Reset();
    memory = std::make_unique<MemoryTracker>(tracker);
    upload(Base + Region - Page, 2 * Page);
    host.data[Region - 16] = 0x31;
    host.data[Region + 16] = 0x62;
    host.events.clear();
    Require(memory->ResolvePendingCpuWrites(Base + Region - Page, 2 * Page) == 2 &&
                host.Hashes() == 2 && memory->IsRegionCpuModified(Base + Region - Page, Page) &&
                memory->IsRegionCpuModified(Base + Region, Page),
            "cross-region resolution visits both changed managers without early return");
}

void CacheBoundsAndReplacement() {
    BufferUploadCache cache;
    cache.Store(Base + 100, 100, {4});
    Require(cache.Find(Base + 1, 4095).has_value(), "cache coverage uses pages, not exact bytes");
    Require(!cache.Find(Base + Page, 1), "cache does not cover a different page");
    cache.Store(Base + Region - Page, 2 * Page, {5});
    Require(!cache.Find(Base + Region + 1, 1), "covering entry must have every region generation");
    Require(cache.Find(Base + Region - Page, 2 * Page).has_value(),
            "exact match returns vector for subsequent generation validation");
    cache.Store(0, 0, {1});
    cache.Store(~u64{0}, 2, {1});
    Require(!cache.Find(0, 0) && !cache.Find(~u64{0}, 2), "reject zero and overflowing range");
    cache.Clear();
    for (u64 i = 0; i < 8; ++i) {
        cache.Store(Base + i * Page, Page, {i + 1});
    }
    cache.Store(Base, Page, {77});
    Require((*cache.Find(Base, Page))[0] == 77, "exact store replaces generations without eviction");
    cache.Store(Base + 8 * Page, Page, {9});
    Require(!cache.Find(Base, Page) && cache.Find(Base + Page, Page), "ninth entry replaces slot zero");
    cache.Store(Base + 9 * Page, Page, {10});
    Require(!cache.Find(Base + Page, Page), "replacement advances to slot one");
    for (u64 i = 10; i < 17; ++i) {
        cache.Store(Base + i * Page, Page, {i + 1});
    }
    Require(!cache.Find(Base + 8 * Page, Page) && cache.Find(Base + 16 * Page, Page),
            "replacement wraps exactly eight slots");
}

void HotFaultPages() {
    host.Reset();
    PageManager tracker{nullptr};
    RegionManager region{&tracker, Base};
    auto fault = [&](size_t page, size_t size = Page) {
        region.ChangeRegionState<Type::CPU, false>(Base + page * Page, size);
        Drain();
        region.MarkCpuWriteFault(Base + page * Page, size);
        return Drain();
    };
    for (int repeat = 0; repeat < 3; ++repeat) {
        Require(fault(10) == std::vector<CpuWriteJournal::Range>{{Base + 10 * Page, Page}},
                "isolated hot page keeps original page range");
    }
    for (int repeat = 0; repeat < 2; ++repeat) {
        Require(fault(11) == std::vector<CpuWriteJournal::Range>{{Base + 11 * Page, Page}},
                "first two faults do not expand");
    }
    Require(fault(11) == std::vector<CpuWriteJournal::Range>{{Base + 10 * Page, 2 * Page}},
            "third protected CPU fault coalesces adjacent hot page");
    region.ChangeRegionState<Type::GPU, true>(Base + 10 * Page, Page);
    Require(fault(11) == std::vector<CpuWriteJournal::Range>{{Base + 11 * Page, Page}},
            "GPU dirty page clears hot bit and stops expansion");
    region.ChangeRegionState<Type::GPU, false>(Base + 10 * Page, Page);
    Require(fault(10) == std::vector<CpuWriteJournal::Range>{{Base + 10 * Page, 2 * Page}},
            "GPU mark clears hot bit but retains saturated fault count");

    host.Reset();
    RegionManager wide{&tracker, Base};
    for (int repeat = 0; repeat < 3; ++repeat) {
        wide.ChangeRegionState<Type::CPU, false>(Base + 20 * Page, 5 * Page);
        wide.MarkCpuWriteFault(Base + 20 * Page, 5 * Page);
    }
    wide.ChangeRegionState<Type::CPU, false>(Base + 20 * Page, Page);
    Drain();
    wide.MarkCpuWriteFault(Base + 20 * Page + 7, 8);
    Require(Drain() == std::vector<CpuWriteJournal::Range>{{Base + 20 * Page + 7, 8}},
            "more than four pages bypass hot training and preserve raw fault range");

    host.Reset();
    RegionManager four{&tracker, Base};
    for (int repeat = 0; repeat < 3; ++repeat) {
        four.ChangeRegionState<Type::CPU, false>(Base + 20 * Page, 4 * Page);
        four.MarkCpuWriteFault(Base + 20 * Page, 4 * Page);
    }
    four.ChangeRegionState<Type::CPU, false>(Base + 20 * Page, Page);
    Drain();
    four.MarkCpuWriteFault(Base + 20 * Page + 7, 8);
    Require(Drain() == std::vector<CpuWriteJournal::Range>{{Base + 20 * Page, 4 * Page}},
            "exactly four pages train the original hot fault policy");

    host.Reset();
    RegionManager bounded{&tracker, Base};
    for (size_t page = 0; page < 140; ++page) {
        for (int repeat = 0; repeat < 3; ++repeat) {
            bounded.ChangeRegionState<Type::CPU, false>(Base + page * Page, Page);
            bounded.MarkCpuWriteFault(Base + page * Page, Page);
        }
    }
    Drain();
    bounded.ChangeRegionState<Type::CPU, false>(Base + 70 * Page, Page);
    bounded.MarkCpuWriteFault(Base + 70 * Page, Page);
    Require(Drain() == std::vector<CpuWriteJournal::Range>{{Base + 6 * Page, 129 * Page}},
            "hot fault coalesces at most64pages on each side");
    bounded.ChangeRegionState<Type::GPU, true>(Base + 71 * Page, Page);
    bounded.ChangeRegionState<Type::CPU, false>(Base + 70 * Page, Page);
    bounded.MarkCpuWriteFault(Base + 70 * Page, Page);
    Require(Drain() == std::vector<CpuWriteJournal::Range>{{Base + 6 * Page, 65 * Page}},
            "GPU neighbour stops right coalescing");
}

void StreamUploads() {
    Require(StreamUploadCache::UseEpochForSerial("CUSA01623") &&
                StreamUploadCache::UseEpochForSerial("CUSA01715") &&
                StreamUploadCache::UseEpochForSerial("CUSA01740") &&
                !StreamUploadCache::UseEpochForSerial("CUSA00000") &&
                !StreamUploadCache::UseEpochForSerial("CUSA016230") &&
                !StreamUploadCache::UseEpochForSerial("cusa01623"),
            "only original exact serials select epoch policy");
    StreamUploadCache cache;
    std::array<u8, 16384> guest{};
    std::array<u8, 16384> mapped{};
    u64 copies = 0, compares = 0, maps = 0, commits = 0;
    std::string order;
    auto copy = [&](VAddr, u8* destination, u64 size) {
        ++copies;
        order += 'C';
        std::memcpy(destination, guest.data(), size);
    };
    auto compare = [&](VAddr, const u8* expected, u64 size) {
        ++compares;
        order += 'E';
        return std::memcmp(expected, guest.data(), size) == 0;
    };
    auto map = [&](u64) {
        ++maps;
        order += 'M';
        return std::pair{mapped.data(), maps * 256};
    };
    auto commit = [&] { ++commits; order += 'K'; };
    auto obtain = [&](VAddr address = Base, u32 size = 8, u64 tick = 1) {
        return cache.Copy(address, size, tick, copy, compare, map, commit);
    };
    const auto first = obtain();
    Require(order == "CMK" && copies == 1 && maps == 1 && commits == 1,
            "general path snapshots before mapping and committing");
    order.clear();
    Require(obtain() == first && order == "E" && copies == 1 && maps == 1,
            "equal snapshot reuses original stream offset");
    guest[0] = 1;
    order.clear();
    const auto second = obtain();
    Require(second != first && order == "ECMK" && copies == 2,
            "changed snapshot allocates and copies new stream data");
    cache.AdvanceEpoch();
    Require(obtain() == second && copies == 2, "general path survives epoch advance with comparison");
    Require(obtain(Base, 8, 2) != second && copies == 3, "current tick change invalidates stream offset");

    StreamUploadCache epoch_cache;
    epoch_cache.SetEpochMode(true);
    order.clear();
    const auto epoch_first = epoch_cache.Copy(Base, 8, 1, copy, compare, map, commit);
    Require(order == "MCK", "epoch path maps before sparse copy and commit");
    guest[0] = 2;
    order.clear();
    Require(epoch_cache.Copy(Base, 8, 1, copy, compare, map, commit) == epoch_first && order.empty(),
            "epoch path reuses within one resume without comparing changed bytes");
    epoch_cache.AdvanceEpoch();
    Require(epoch_cache.Copy(Base, 8, 1, copy, compare, map, commit) != epoch_first && order == "MCK",
            "next coroutine resume forces fresh epoch upload");

    StreamUploadCache collisions;
    const auto before_collision = copies;
    collisions.Copy(1, 1, 1, copy, compare, map, commit);
    collisions.Copy(1 ^ (u64{3} << 48), 2, 1, copy, compare, map, commit);
    collisions.Copy(1, 1, 1, copy, compare, map, commit);
    Require(copies == before_collision + 3, "key collision still checks address and size");

    StreamUploadCache count_cache;
    for (u64 i = 0; i < 8192; ++i) count_cache.Copy(Base + i, 1, 1, copy, compare, map, commit);
    auto before = copies;
    count_cache.Copy(Base, 1, 1, copy, compare, map, commit);
    Require(copies == before, "8192 entries remain reusable");
    count_cache.Copy(Base + 8192, 1, 1, copy, compare, map, commit);
    before = copies;
    count_cache.Copy(Base, 1, 1, copy, compare, map, commit);
    Require(copies == before + 1, "cache clears on next lookup after8193 entries");

    StreamUploadCache byte_cache;
    for (u64 i = 0; i < 4095; ++i) byte_cache.Copy(Base + i, 16384, 1, copy, compare, map, commit);
    before = copies;
    byte_cache.Copy(Base, 16384, 1, copy, compare, map, commit);
    Require(copies == before, "snapshot budget permits exactly64MiB including query");
    byte_cache.Copy(Base + 4095, 16384, 1, copy, compare, map, commit);
    before = copies;
    byte_cache.Copy(Base, 16384, 1, copy, compare, map, commit);
    Require(copies == before + 1, "budget overflow clears before even a cache hit");
}

void JournalCapacityAndConcurrency() {
    host.Reset();
    auto& journal = CpuWriteJournal::Instance();
    for (u64 i = 0; i < 4096; ++i) {
        journal.Record(i, i + 1);
    }
    auto entries = Drain();
    Require(entries.size() == 4096 && entries.back() == CpuWriteJournal::Range{4095, 4096},
            "4096 journal entries are retained");
    for (u64 i = 0; i < 4097; ++i) {
        journal.Record(i, 1);
    }
    journal.Record(99999, 1);
    Require(journal.Consume(entries) && entries.empty(), "4097th record clears and latches overflow");
    journal.Record(0, 0);
    Require(Drain() == std::vector<CpuWriteJournal::Range>{{0, 0}},
            "consume resets overflow and journal does not rewrite raw ranges");
    std::atomic<unsigned> done{};
    std::array<std::thread, 4> writers;
    for (u64 worker = 0; worker < writers.size(); ++worker) {
        writers[worker] = std::thread([&, worker] {
            for (u64 i = 0; i < 250; ++i) journal.Record(worker * 250 + i, 1);
            ++done;
        });
    }
    std::vector<CpuWriteJournal::Range> collected;
    while (done.load() != writers.size()) {
        auto batch = Drain();
        collected.insert(collected.end(), batch.begin(), batch.end());
        std::this_thread::yield();
    }
    for (auto& writer : writers) writer.join();
    auto last = Drain();
    collected.insert(collected.end(), last.begin(), last.end());
    std::sort(collected.begin(), collected.end(), [](auto a, auto b) { return a.address < b.address; });
    Require(collected.size() == 1000, "concurrent consume loses no write");
    for (u64 i = 0; i < collected.size(); ++i) {
        Require(collected[i] == CpuWriteJournal::Range{i, 1}, "concurrent records remain unique");
    }
}
} // namespace

// Only host mapping/protection and settings are doubles. The region policy, memory tracker,
// generation cache, bitsets and journal above are production implementations.
EmulatorSettingsImpl::EmulatorSettingsImpl() = default;
EmulatorSettingsImpl::~EmulatorSettingsImpl() = default;
std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
    static auto settings = std::make_shared<EmulatorSettingsImpl>();
    return settings;
}
namespace VideoCore {
struct PageManager::Impl {};
PageManager::PageManager(Vulkan::Rasterizer*) : impl{std::make_unique<Impl>()} {}
PageManager::~PageManager() = default;
bool PageManager::IsMapped(VAddr address, u64 size) const {
    if (address < Base || size == 0 || size > host.data.size() ||
        address - Base > host.data.size() - size) return false;
    const size_t first = (address - Base) / Page;
    const size_t last = (address - Base + size - 1) / Page;
    for (size_t page = first; page <= last; ++page) if (!host.mapped[page]) return false;
    return true;
}
u64 PageManager::HashPage(VAddr address) const {
    Require(IsMapped(address, Page), "hash requires mapped page");
    host.events.push_back({Event::Hash, address});
    return XXH3_64bits(host.data.data() + address - Base, Page);
}
template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr address, RegionBits& mask) const {
    for (const auto& [first, last] : mask) {
        for (size_t page = first; page < last; ++page) {
            const VAddr current = address + page * Page;
            host.events.push_back({is_read ? (track ? Event::TrackRead : Event::UntrackRead)
                                         : (track ? Event::TrackWrite : Event::UntrackWrite), current});
            if constexpr (!is_read) host.writable[(current - Base) / Page] = !track;
        }
    }
}
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr, RegionBits&) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr, RegionBits&) const;
} // namespace VideoCore

int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            Require(BufferPendingWriteResolveEnabled() == (argv[1][0] == '1'),
                    "pending resolver process selector differs from expected");
            std::cout << "pending resolver process selector PASS\n";
            return 0;
        }
        ThresholdsAndRearm();
        MappingAndGpuDirty();
        ChangedDataAndCancellation();
        GenerationCache();
        PendingWriteResolveSelector();
        PendingWriteReuse();
        PendingWriteResolveScope();
        CacheBoundsAndReplacement();
        JournalCapacityAndConcurrency();
        HotFaultPages();
        StreamUploads();
        std::cout << "CPU upload policy, generation cache and journal contracts PASS\n";
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
