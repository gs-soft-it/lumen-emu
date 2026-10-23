// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/elf_info.h"
#include "common/alignment.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/file_sys/fs.h"
#ifdef LUMEN_GEN4_DIRECT_FEX
#include "core/file_sys/backends/host_fs.h"
#endif
#include "core/fex/fex_guest_engine.h"
#include "core/fex/guest_memory_scope.h"
#include "core/guest_cpu/fex_hle_bridge.h"
#include "core/memory.h"
#include "core/libraries/kernel/orbis_error.h"
#include "video_core/guest_publication_snapshots.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace Core {
// Fixture access is limited to boot metadata and mapping setup/teardown. The
// tested writer, physical-alias enumeration and real shared mappings are the
// production implementations. Public MapMemory requires a bound rasterizer,
// which is unrelated to this writer contract and is deliberately not mocked.
class MemoryBackingAliasTest final {
public:
    static constexpr u64 Granule = 16 * 1024;

    static void InitializeBootMetadata() {
        auto& info = Common::ElfInfo::Instance();
        info.sdk_ver = Common::ElfInfo::FW_900;
        info.initialized = true;
    }

    static VAddr Map(MemoryManager& memory, VAddr hint, PAddr physical, u64 size) {
        Fex::MemoryMutationScope publication;
        CHECK(publication.Valid());
        std::scoped_lock lock{memory.mutex, memory.unmap_mutex};
        const auto generation = memory.mapping_generation.BeginMutation();
        const auto address = memory.SearchFree(hint, size, Granule);
        CHECK(address != static_cast<VAddr>(-1));
        auto area = memory.CreateArea(address, size, MemoryProt::CpuReadWrite,
            MemoryMapFlags::NoCoalesce, VMAType::Direct, "backing alias contract", Granule);
        area->second.phys_areas.emplace(0, PhysicalMemoryArea{
            physical, size, 3, PhysicalMemoryType::Mapped});
        CHECK(memory.impl.Map(address, size, physical) == reinterpret_cast<void*>(address));
        publication.Publish(address, size, true);
        return address;
    }

    static void Unmap(MemoryManager& memory, VAddr address, u64 size) {
        Fex::MemoryMutationScope publication;
        CHECK(publication.Valid());
        std::scoped_lock lock{memory.mutex, memory.unmap_mutex};
        const auto generation = memory.mapping_generation.BeginMutation();
        CHECK(memory.UnmapMemoryImpl(address, size) == 0);
    }

    static VAddr Search(MemoryManager& memory, VAddr hint, u64 size, u32 alignment) {
        std::shared_lock lock{memory.mutex};
        return memory.SearchFree(hint, size, alignment);
    }

    static VAddr Reserve(MemoryManager& memory, VAddr hint, u64 size) {
        Fex::MemoryMutationScope publication;
        CHECK(publication.Valid());
        std::scoped_lock lock{memory.mutex, memory.unmap_mutex};
        const auto generation = memory.mapping_generation.BeginMutation();
        const auto address = memory.SearchFree(hint, size, Granule);
        CHECK(address != static_cast<VAddr>(-1));
        memory.CreateArea(address, size, MemoryProt::NoAccess, MemoryMapFlags::NoFlags,
                          VMAType::Reserved, "module reservation contract", Granule);
        return address;
    }
};
} // namespace Core

namespace {
using Alias = std::pair<VAddr, u64>;
bool NoGuestAccess(void*, std::uintptr_t, std::size_t, bool) { return false; }
std::optional<Core::GuestExecutionRange> NoExecutable(void*, std::uintptr_t) {
    return std::nullopt;
}

void CheckAliases(std::vector<Alias> actual, std::vector<Alias> expected) {
    std::ranges::sort(actual);
    std::ranges::sort(expected);
    CHECK(actual == expected);
}

void CheckDarwinModulePlacement(Core::MemoryManager& memory) {
#if defined(__APPLE__) && defined(LUMEN_GEN4_HOST_RUNTIME)
    using Fixture = Core::MemoryBackingAliasTest;
    constexpr auto G = Fixture::Granule;
    constexpr VAddr ModuleHint = 0x800000000ULL;
    constexpr VAddr UserBegin = 0x7000000000ULL;
    constexpr VAddr UserEnd = 0x8000000000ULL;
    constexpr u64 Size = 32ULL * 1024 * 1024;
    CHECK(memory.GetAddressSpace().UserVirtualBase() == UserBegin);
    CHECK(memory.GetAddressSpace().UserVirtualSize() == UserEnd - UserBegin);
    CHECK(!memory.IsValidMapping(0x1000000000ULL)); // Native GPU carveout.
    CHECK(!memory.IsValidMapping(UserBegin - G, G));
    CHECK(!memory.IsValidMapping(UserEnd));
    CHECK(Fixture::Search(memory, UserEnd, G, G) == static_cast<VAddr>(-1));

    // Exercise the loader's search/reservation sequence at its usual hint.
    // Native host holes must be skipped without losing the high user window.
    const auto reserved = Fixture::Reserve(memory, ModuleHint, Size);
    CHECK(reserved >= UserBegin && reserved <= UserEnd - Size);
    Libraries::Kernel::OrbisVirtualQueryInfo info{};
    CHECK(memory.VirtualQuery(reserved, 0, &info) == 0 && !info.is_committed &&
          info.start == reserved && info.end == reserved + Size && info.protection == 0);
    const auto next = Fixture::Search(memory, ModuleHint, Size, G);
    CHECK(next != static_cast<VAddr>(-1) && next >= reserved + Size);
    Fixture::Unmap(memory, reserved, Size);
    CHECK(Fixture::Search(memory, ModuleHint, Size, G) == reserved);

    // Real shared mappings prove that the high range is owned by the address
    // space and usable by the same FEX publication path as the low aliases.
    const auto physical = memory.Allocate(G, memory.GetTotalDirectSize(), Size, G, 3);
    CHECK(physical != static_cast<PAddr>(-1));
    const auto a = Fixture::Map(memory, ModuleHint, physical, Size);
    const auto b = Fixture::Map(memory, a + Size, physical, Size);
    CHECK(a == reserved && b >= a + Size && b <= UserEnd - Size);
    const u8 first = 0x6D, last = 0xB7;
    CHECK(memory.TryWriteBacking(reinterpret_cast<void*>(a), &first, 1));
    CHECK(memory.TryWriteBacking(reinterpret_cast<void*>(a + Size - 1), &last, 1));
    CHECK(*reinterpret_cast<const u8*>(b) == first);
    CHECK(*reinterpret_cast<const u8*>(b + Size - 1) == last);
    Fixture::Unmap(memory, b, Size);
    Fixture::Unmap(memory, a, Size);
    CHECK(memory.Free(physical, Size, false) == 0);
    std::puts("Darwin module hint fallback and high user-window alias contracts PASS");
#endif
}

void CheckDisjointRegions(Core::MemoryManager& memory, const std::filesystem::path& path) {
    using Fixture = Core::MemoryBackingAliasTest;
    constexpr auto G = Fixture::Granule;
    const auto regions = memory.GetAddressSpace().GetUsableRegions();
    CHECK(!regions.empty());
    const auto first = regions.begin()->lower();
    CHECK(first >= memory.GetAddressSpace().SystemManagedVirtualBase());
    CHECK(Fixture::Search(memory, 0, G, G) == first);
    CHECK(Fixture::Search(memory, first - 1, G, G) == first);
    CHECK(!memory.IsValidMapping(first - 1));
    CHECK(!memory.IsValidMapping(first - 1, 1));
    CHECK(!memory.IsValidMapping(first, std::numeric_limits<u64>::max()));
    CHECK(Fixture::Search(memory, std::numeric_limits<VAddr>::max(), G, G) ==
          static_cast<VAddr>(-1));

    // Use a real host-owned hole between two reserved VM regions. No metadata
    // or writer behavior is replaced to manufacture the disjoint-map case.
    VAddr hole{}, next{};
    for (auto previous = regions.begin(), current = std::next(previous);
         current != regions.end(); previous = current++) {
        if (previous->upper() < current->lower() &&
            previous->upper() - previous->lower() >= G &&
            current->upper() - current->lower() >= G) {
            hole = previous->upper();
            next = current->lower();
            break;
        }
    }
    if (hole == 0) {
        // Some hosts reserve one contiguous guest span. Keep the ordinary real
        // VM and alias cases; this case needs an actual host reservation hole.
        std::printf("DisjointRegion contracts SKIP reason=no-host-reservation-hole regions=%zu\n",
                    static_cast<size_t>(regions.iterative_size()));
        return;
    }
    CHECK(next > hole);
    CHECK(Fixture::Search(memory, hole, G, G) == next);
    CHECK(Fixture::Search(memory, hole - 1, G, G) == next); // Alignment enters hole.
    CHECK(!memory.IsValidMapping(hole));
    CHECK(!memory.IsValidMapping(hole, 1));
    CHECK(!memory.IsValidMapping(hole - 1, 2));
    const auto last_end = std::prev(regions.end())->upper();
    CHECK(!memory.IsValidMapping(last_end));
    CHECK(Fixture::Search(memory, last_end - 1, G, G) == static_cast<VAddr>(-1));

    const auto physical = memory.Allocate(G, memory.GetTotalDirectSize(), G, G, 3);
    CHECK(physical != static_cast<PAddr>(-1));
    const auto a = Fixture::Map(memory, hole - G, physical, G);
    const auto b = Fixture::Map(memory, next, physical, G);
    CHECK(a == hole - G && b == next);
    const u8 marker = 0x7e;
    CHECK(memory.TryWriteBacking(reinterpret_cast<void*>(a), &marker, 1));
    CHECK(memory.ClampRangeSize(a, 1ULL << 30) == G);
    CHECK(memory.GetMappedRangeSize(a, 1ULL << 30) == G);
    CHECK(memory.GetMappedRangeSize(hole - 8, 16) == 8);
    CHECK(memory.GetMappedRangeSize(hole, 16) == 0);
    CHECK(memory.GetMappedRangeSize(next, G) == G);

    Libraries::Kernel::OrbisVirtualQueryInfo info{};
    CHECK(memory.VirtualQuery(0, 0, &info) == ORBIS_KERNEL_ERROR_EACCES);
    CHECK(memory.VirtualQuery(0, 1, &info) == 0 && info.start == a);
    CHECK(memory.VirtualQuery(hole, 0, &info) == ORBIS_KERNEL_ERROR_EACCES);
    CHECK(memory.VirtualQuery(hole, 1, &info) == 0 && info.start == b);
    CHECK(memory.VirtualQuery(last_end, 0, &info) == ORBIS_KERNEL_ERROR_EACCES);
    CHECK(memory.VirtualQuery(last_end, 1, &info) == ORBIS_KERNEL_ERROR_EACCES);
    CHECK(memory.VirtualQuery(a, 0, &info) == 0 && info.end == hole);

    auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
    const auto fd = handles->CreateHandle();
    auto* file = handles->GetFile(fd);
#ifdef LUMEN_GEN4_DIRECT_FEX
    CHECK(file != nullptr);
    file->handle = std::make_unique<Core::FileSys::HostFile>(
        path / "fixed-range.bin", Common::FS::FileAccessMode::Create, false);
    CHECK(file->IsBackendOpen());
    CHECK(file->GetHostFile()->SetSize(G));
#else
    CHECK(file && file->f.Open(path / "fixed-range.bin", Common::FS::FileAccessMode::Create) == 0);
    CHECK(file->f.SetSize(G));
#endif
    const auto generation = memory.MappingGeneration();
    for (const auto flags : {Core::MemoryMapFlags::Fixed,
                            Core::MemoryMapFlags::Fixed | Core::MemoryMapFlags::NoOverwrite}) {
        for (const auto [address, size] : std::array<Alias, 3>{{{0, G}, {hole, G}, {a, 2 * G}}}) {
            void* output = reinterpret_cast<void*>(0x1234);
            CHECK(memory.MapMemory(&output, address, size, Core::MemoryProt::CpuReadWrite,
                flags, Core::VMAType::Reserved) == ORBIS_KERNEL_ERROR_ENOMEM);
            CHECK(output == reinterpret_cast<void*>(0x1234));
            CHECK(memory.MapFile(&output, address, size, Core::MemoryProt::CpuRead,
                flags, fd, 0) == ORBIS_KERNEL_ERROR_ENOMEM);
            CHECK(output == reinterpret_cast<void*>(0x1234));
        }
    }
    CHECK(memory.MappingGeneration() == generation);
    CHECK(*reinterpret_cast<const u8*>(a) == marker && *reinterpret_cast<const u8*>(b) == marker);
    CHECK(memory.VirtualQuery(a, 0, &info) == 0 && info.end == hole);
    handles->DeleteHandle(fd);
    Fixture::Unmap(memory, b, G);
    Fixture::Unmap(memory, a, G);
    CHECK(memory.Free(physical, G, false) == 0);
}

void CheckMappedRanges(Core::MemoryManager& memory) {
    using Fixture = Core::MemoryBackingAliasTest;
    constexpr auto G = Fixture::Granule;
    constexpr u64 PreferredPart = 512ULL * 1024 * 1024;
    VAddr hint{};
    u64 available{};
    for (const auto& region : memory.GetAddressSpace().GetUsableRegions()) {
        const auto aligned = Common::AlignUp(region.lower(), G);
        if (aligned < region.upper() && region.upper() - aligned > available) {
            hint = aligned;
            available = region.upper() - aligned;
        }
    }
    CHECK(hint != 0 && available >= 4 * G);
    const u64 Part = std::min<u64>(PreferredPart, Common::AlignDown((available - G) / 3, G));
    const bool covers_incomplete_cache = Part == PreferredPart;
    const auto physical = memory.Allocate(G, memory.GetTotalDirectSize(), 3 * Part, G, 3);
    CHECK(physical != static_cast<PAddr>(-1));
    // Sparse shared mappings exercise real VM metadata without touching their
    // backing. Hosts with a contiguous region over 1.5 GiB also cover the cache's
    // partial-scan branch; smaller host regions retain the mapping/lifetime cases.
    const auto a = Fixture::Map(memory, hint, physical, Part);
    const auto b = Fixture::Map(memory, a + Part, physical + Part, Part);
    const auto c = Fixture::Map(memory, b + Part, physical + 2 * Part, Part);
    CHECK(b == a + Part && c == b + Part);
    CHECK(memory.GetMappedRangeSize(0, 16) == 0);
    CHECK(memory.GetMappedRangeSize(std::numeric_limits<VAddr>::max(), 16) == 0);
    CHECK(memory.GetMappedRangeSize(a, 0) == 0);
    CHECK(memory.GetMappedRangeSize(a, 16) == 16);
    CHECK(memory.GetMappedRangeSize(b - 8, 16) == 16);
    // On a sufficiently large host region, the first query cached only 1 GiB,
    // with another mapped VMA following it. This query must extend that entry.
    CHECK(memory.GetMappedRangeSize(a, 2 * Part + G) == 2 * Part + G);
    CHECK(memory.GetMappedRangeSize(a, 4 * Part) == 3 * Part);
    CHECK(memory.GetMappedRangeSize(c + Part - 8, 16) == 8);
    CHECK(memory.GetMappedRangeSize(c + Part, 16) == 0);
    // A cached complete range must be invalidated when a middle VMA is removed.
    Fixture::Unmap(memory, b, Part);
    CHECK(memory.GetMappedRangeSize(a, 4 * Part) == Part);
    CHECK(memory.GetMappedRangeSize(b, 16) == 0);
    CHECK(memory.GetMappedRangeSize(b - 8, 16) == 8);
    CHECK(memory.GetMappedRangeSize(c, Part) == Part);
    CHECK(Fixture::Map(memory, b, physical + Part, Part) == b);
    CHECK(memory.GetMappedRangeSize(a, 4 * Part) == 3 * Part);
    Fixture::Unmap(memory, c, Part);
    Fixture::Unmap(memory, b, Part);
    Fixture::Unmap(memory, a, Part);
    CHECK(memory.GetMappedRangeSize(a, 16) == 0);
    CHECK(memory.Free(physical, 3 * Part, false) == 0);
    std::printf("MappedRange real-VM contracts PASS part=%llu incomplete-cache-extension=%s\n",
                static_cast<unsigned long long>(Part),
                covers_incomplete_cache ? "covered" : "host-region-too-small");
}

void CheckSparseCompare(Core::MemoryManager& memory) {
    using Fixture = Core::MemoryBackingAliasTest;
    constexpr auto G = Fixture::Granule;
    const auto physical = memory.Allocate(G, memory.GetTotalDirectSize(), 3 * G, G, 3);
    CHECK(physical != static_cast<PAddr>(-1));
    VAddr hint{};
    for (const auto& region : memory.GetAddressSpace().GetUsableRegions()) {
        const auto aligned = Common::AlignUp(region.lower(), G);
        if (aligned < region.upper() && region.upper() - aligned >= 3 * G) {
            hint = aligned;
            break;
        }
    }
    CHECK(hint != 0);
    const auto a = Fixture::Map(memory, hint, physical, G);
    const auto b = Fixture::Map(memory, a + G, physical + G, G);
    const auto c = Fixture::Map(memory, b + G, physical + 2 * G, G);
    CHECK(b == a + G && c == b + G);
    std::vector<u8> bytes(3 * G, 0x5a);
    CHECK(memory.TryWriteBacking(reinterpret_cast<void*>(a), bytes.data(), bytes.size()));
    CHECK(!memory.CompareSparseMemory(0, bytes.data(), 0));
    CHECK(!memory.CompareSparseMemory(std::numeric_limits<VAddr>::max(), bytes.data(), 1));
    CHECK(memory.CompareSparseMemory(a, bytes.data(), 0));
    CHECK(memory.CompareSparseMemory(a, bytes.data(), bytes.size()));
    CHECK(memory.GetMappedRangeSize(a, bytes.size()) == bytes.size());
    CHECK(memory.CompareSparseMemory(a + G - 8, bytes.data(), 16));
    bytes.back() ^= 1;
    CHECK(!memory.CompareSparseMemory(a, bytes.data(), bytes.size()));
    bytes.back() ^= 1;

    // Internal cache reads must tolerate canonical views protected by GPU
    // watches, including physical/VMA boundaries, without invoking a fault.
    memory.GetAddressSpace().Protect(a, 3 * G, Core::MemoryPermission::None);
    std::vector<u8> copied(bytes.size());
    memory.CopySparseMemoryFromBacking(a, copied.data(), copied.size());
    CHECK(copied == bytes);
    CHECK(memory.CompareSparseMemoryFromBacking(a, bytes.data(), bytes.size()));
    CHECK(memory.CompareSparseMemoryFromBacking(a + G - 8, bytes.data(), 16));
    bytes.back() ^= 1;
    CHECK(!memory.CompareSparseMemoryFromBacking(a, bytes.data(), bytes.size()));
    bytes.back() ^= 1;
    CHECK(!memory.CompareSparseMemoryFromBacking(0, bytes.data(), 0));
    CHECK(!memory.CompareSparseMemoryFromBacking(std::numeric_limits<VAddr>::max(),
                                                bytes.data(), 1));
    memory.GetAddressSpace().Protect(a, 3 * G, Core::MemoryPermission::ReadWrite);

    // A warmed mapped-range entry must be invalid after unmapping. Sparse
    // comparison reads zeroes for the hole and must not dereference that page.
    Fixture::Unmap(memory, b, G);
    CHECK(!memory.CompareSparseMemory(a, bytes.data(), bytes.size()));
    std::fill(bytes.begin() + G, bytes.begin() + 2 * G, 0);
    CHECK(memory.CompareSparseMemory(a, bytes.data(), bytes.size()));
    CHECK(memory.CompareSparseMemoryFromBacking(a, bytes.data(), bytes.size()));
    memory.CopySparseMemoryFromBacking(a, copied.data(), copied.size());
    CHECK(copied == bytes);
    CHECK(memory.CompareSparseMemory(b, bytes.data() + G, G));
    CHECK(memory.CompareSparseMemory(b, bytes.data(), 0));
    bytes[G + 7] = 1;
    CHECK(!memory.CompareSparseMemory(a, bytes.data(), bytes.size()));
    bytes[G + 7] = 0;
    CHECK(Fixture::Map(memory, b, physical + G, G) == b);
    CHECK(!memory.CompareSparseMemory(a, bytes.data(), bytes.size()));
    std::fill(bytes.begin() + G, bytes.begin() + 2 * G, 0x5a);
    CHECK(memory.CompareSparseMemory(a, bytes.data(), bytes.size()));
    Fixture::Unmap(memory, c, G);
    Fixture::Unmap(memory, b, G);
    Fixture::Unmap(memory, a, G);
    CHECK(memory.Free(physical, 3 * G, false) == 0);
    std::puts("Sparse compare production VM contracts PASS: cached ranges, byte changes, holes, invalid bases and mapping generation");
}

void CheckBacking(Core::MemoryManager& memory) {
    using Fixture = Core::MemoryBackingAliasTest;
    constexpr auto G = Fixture::Granule;
    const auto physical = memory.Allocate(G, memory.GetTotalDirectSize(), 3 * G, G, 3);
    CHECK(physical != static_cast<PAddr>(-1));
    // Select one reserved gap large enough for both aliases and the deliberate
    // hole; the host's shared cache can cover the usual guest mapping hint.
    VAddr hint{};
    for (const auto& region : memory.GetAddressSpace().GetUsableRegions()) {
        const auto aligned = Common::AlignUp(region.lower(), G);
        if (aligned < region.upper() && region.upper() - aligned >= 6 * G) {
            hint = aligned;
            break;
        }
    }
    CHECK(hint != 0);
    const auto a = Fixture::Map(memory, hint, physical, 2 * G);
    // Leave a real unmapped gap after A. B aliases only the second half of A,
    // plus a physical page not present in A, to test clipping of alias ranges.
    const auto b = Fixture::Map(memory, a + 4 * G, physical + G, 2 * G);
    CHECK(b >= a + 4 * G);
    const std::vector<u8> initial(2 * G, 0x11);
    CHECK(memory.TryWriteBacking(reinterpret_cast<void*>(a), initial.data(), initial.size()));
    CHECK(memory.TryWriteBacking(reinterpret_cast<void*>(b), initial.data(), initial.size()));

    VideoCore::GuestPublicationSnapshots snapshots;
    const auto read = [&](VAddr address, auto bytes) {
        memory.CopySparseMemory(address, bytes.data(), bytes.size());
    };
    constexpr auto P = VideoCore::GuestPublicationSnapshots::PageSize;
    const std::array<VAddr, 5> tracked_pages{a + G - P, a + G, b, a + 2 * G - P, b + G - P};
    for (const auto page : tracked_pages)
        snapshots.Track(page, read);
    CHECK(snapshots.ObserveCpuChanges(read).empty());

    std::array<u8, 16> payload{};
    for (std::size_t i = 0; i != payload.size(); ++i) payload[i] = 0xa0 + i;
    std::vector<Alias> aliases{{0xdead, 77}}; // Writer must clear prior output.
    const auto start = a + G - 8;
    const bool success = memory.TryWriteBacking(reinterpret_cast<void*>(start),
        payload.data(), payload.size(), &aliases);
    CHECK(success);
    CheckAliases(aliases, {{start, 16}, {b, 8}});
    CHECK(std::equal(payload.begin(), payload.end(), reinterpret_cast<const u8*>(start)));
    CHECK(std::equal(payload.begin() + 8, payload.end(), reinterpret_cast<const u8*>(b)));
    CHECK(*reinterpret_cast<const u8*>(start - 1) == 0x11);
    CHECK(*reinterpret_cast<const u8*>(b + 8) == 0x11);
    if (success) for (const auto& [address, size] : aliases)
        CHECK(snapshots.RecordBackingWrite(address, size, true));
    CHECK(snapshots.CommitPublishedMemory(read) == 3);
    CHECK(snapshots.ObserveCpuChanges(read).empty());

    // The first half reaches physical backing; the second half lies in the
    // unmapped gap. Production reports failure with partial alias output.
    std::array<u8, 32> failed_payload{};
    failed_payload.fill(0xd3);
    const auto partial_start = a + 2 * G - 16;
    const bool complete = memory.TryWriteBacking(reinterpret_cast<void*>(partial_start),
        failed_payload.data(), failed_payload.size(), &aliases);
    CHECK(!complete);
    CheckAliases(aliases, {{partial_start, 16}, {b + G - 16, 16}});
    CHECK(std::all_of(reinterpret_cast<const u8*>(partial_start),
                      reinterpret_cast<const u8*>(partial_start + 16),
                      [](u8 value) { return value == 0xd3; }));
    CHECK(std::all_of(reinterpret_cast<const u8*>(b + G - 16),
                      reinterpret_cast<const u8*>(b + G),
                      [](u8 value) { return value == 0xd3; }));
    // Match PublishBackingWrite's success gate: partial output cannot advance
    // baselines. Production retains its fatal assertion on the failed write.
    if (complete) for (const auto& [address, size] : aliases)
        CHECK(snapshots.RecordBackingWrite(address, size, true));
    CHECK(snapshots.CommitPublishedMemory(read) == 0);
    auto changed = snapshots.ObserveCpuChanges(read);
    std::ranges::sort(changed);
    std::vector<std::uint64_t> expected{a + 2 * G - P, b + G - P};
    std::ranges::sort(expected);
    CHECK(changed == expected);

    aliases = {{a, 1}};
    CHECK(!memory.TryWriteBacking(reinterpret_cast<void*>(a + 2 * G),
        failed_payload.data(), 1, &aliases));
    CHECK(aliases.empty());

    Fixture::Unmap(memory, b, 2 * G);
    Fixture::Unmap(memory, a, 2 * G);
    CHECK(memory.Free(physical, 3 * G, false) == 0);
}
} // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("lumen-backing-alias-contract-" + std::to_string(getpid()));
    Common::FS::InitializeUserPaths(path);
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    Common::Log::Setup("backing-alias-contract.log");
    Core::MemoryBackingAliasTest::InitializeBootMetadata();
    Core::GuestCpu::HleCallRegistry registry;
    Core::GuestCpu::HleGuestBridge bridge{registry, NoGuestAccess, nullptr, nullptr, nullptr,
        NoExecutable, nullptr};
    {
        // Reserve the guest address space before FEX creates its host mappings,
        // matching the production boot order.
        Core::MemoryManager memory;
        auto result = Core::Fex::GuestEngine::Create(bridge);
        CHECK(std::holds_alternative<std::unique_ptr<Core::Fex::GuestEngine>>(result));
        auto engine = std::move(std::get<std::unique_ptr<Core::Fex::GuestEngine>>(result));
        CheckDarwinModulePlacement(memory);
        CheckDisjointRegions(memory, path);
        CheckBacking(memory);
        CheckMappedRanges(memory);
        CheckSparseCompare(memory);
        CHECK(std::holds_alternative<bool>(engine->Shutdown()));
    }
    Common::Log::Shutdown();
    std::filesystem::remove_all(path);
    std::puts("Production VM contracts PASS: disjoint search/query/validation, fixed-map rejection before effects, contiguous clamp, cached GPU mapped ranges and generation invalidation, clipped physical aliases, real mapped bytes, partial failure and success-gated snapshot consumption");
}
