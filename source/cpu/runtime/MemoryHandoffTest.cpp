// Lumen, 2026-10-07. SPDX-License-Identifier: GPL-2.0-only
#include "MemoryHandoff.h"
#include "MemoryHandoffPageCache.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace {
using namespace Lumen::GuestCpu;
struct Access { std::uint64_t address {}, bytes {}; MemoryAccessIntent intent {}; };
std::array<Access, 16> accesses {};
std::size_t calls {};
bool permit = true;
bool Query(std::uint64_t address, std::uint64_t bytes, MemoryAccessIntent intent) noexcept {
    if (calls < accesses.size()) accesses[calls] = {address, bytes, intent};
    ++calls;
    return permit;
}
bool OtherQuery(std::uint64_t, std::uint64_t, MemoryAccessIntent) noexcept { return true; }
bool Check(bool value, const char* label) {
    if (!value) std::fprintf(stderr, "memory-handoff: %s\n", label);
    return value;
}
bool PhysicalAliases() {
    PhysicalMemoryRange result {99, 99, 99};
    bool ok = Check(IntersectPhysicalRanges({7, 32, 64}, {7, 64, 64}, &result) &&
        result.identity == 7 && result.offset == 64 && result.size == 32,
        "different virtual aliases intersect by backing identity and exact offset");
    for (const auto other : {PhysicalMemoryRange {8, 32, 64}, {7, 96, 8},
                            {7, 0, 32}, {7, 64, 0}, {7, UINT64_MAX - 3, 8}}) {
        result = {99, 99, 99};
        ok = Check(!IntersectPhysicalRanges({7, 32, 64}, other, &result) &&
            result.identity == 0 && result.offset == 0 && result.size == 0,
            "unrelated identities, touching/disjoint/empty/overflow intervals clear output") && ok;
    }
    ok = Check(!IntersectPhysicalRanges({0, 32, 64}, {0, 32, 64}, nullptr),
        "missing backing identity cannot authorize an alias") && ok;
    ok = Check(IntersectPhysicalRanges({7, 32, 64}, {7, 40, 4}, nullptr),
        "intersection supports predicate-only queries") && ok;
    return ok;
}
bool GuardsAndIntent() {
    constexpr std::uint64_t page = UINT64_C(0x4b000);
    constexpr std::uint64_t collision = page + (std::uint64_t {MemoryHandoffGuardCount} << MemoryHandoffPageShift);
    bool ok = Check(!MemoryHandoffEnabled() && !NeedsHandoff(page, 8), "unmarked access starts on inline path");
    ok = Check(!RegisterMemoryHandoffQuery(nullptr) && RegisterMemoryHandoffQuery(Query) &&
        RegisterMemoryHandoffQuery(Query) && !RegisterMemoryHandoffQuery(OtherQuery),
        "query registration is stable and never replaces active resolver") && ok;
    ok = Check(PrepareCpuMemoryAccess(page, 8, MemoryAccessIntent::Read) && calls == 0,
        "ordinary unmarked load performs no resolver call") && ok;
    MarkHandoffRange(page + 16, 8);
    ok = Check(NeedsHandoff(page, 1) && NeedsHandoff(collision, 1), "collisions are conservative") && ok;
    MarkHandoffRange(collision, 8, false);
    ok = Check(NeedsHandoff(page, 1) && NeedsHandoff(collision, 1),
        "retiring colliding range never clears another live guard") && ok;
    permit = false;
    ok = Check(!PrepareCpuMemoryAccess(page + 16, 8, MemoryAccessIntent::Read) && calls == 1 &&
        accesses[0].address == page + 16 && accesses[0].bytes == 8 &&
        accesses[0].intent == MemoryAccessIntent::Read, "denial preserves exact read footprint") && ok;
    {
        MemoryHandoffBypass publication;
        MemoryHandoffBypass nested;
        ok = Check(PrepareCpuMemoryAccess(page + 16, 8, MemoryAccessIntent::Write) && calls == 1,
            "producer publication bypass is nested and does not recurse") && ok;
    }
    ok = Check(!PrepareCpuMemoryAccess(page + 16, 8, MemoryAccessIntent::Write) && calls == 2,
        "publication scope restores checking after exit") && ok;
    permit = true;
    std::uint64_t value = 23;
    for (unsigned i = 0; i < 2; ++i) {
        ok = Check(PrepareCpuMemoryAccess(page + 16, sizeof(value), MemoryAccessIntent::Write),
            "same-value CPU store is admitted by write intent") && ok;
        const std::uint64_t same = 23;
        std::memcpy(&value, &same, sizeof(value));
    }
    ok = Check(value == 23 && calls == 4 && accesses[2].intent == MemoryAccessIntent::Write &&
        accesses[3].intent == MemoryAccessIntent::Write,
        "identical stored bytes still publish two independent write intentions") && ok;
    ok = Check(PrepareCpuMemoryAccess(page + 16, 16, MemoryAccessIntent::ReadWrite) &&
        calls == 5 && WritesMemory(accesses[4].intent), "atomics retain read/write intention") && ok;
    // The marked middle page must not disappear when neither endpoint is guarded.
    const auto middle = page + (UINT64_C(8) << MemoryHandoffPageShift);
    MarkHandoffRange(middle + 9, 1);
    ok = Check(!NeedsHandoff(middle - 4096, 1) && !NeedsHandoff(middle + 4096, 1) &&
        NeedsHandoff(middle - 4096, 8193), "REP/HLE spans inspect intervening guarded pages") && ok;
    ok = Check(PrepareCpuMemoryAccess(middle - 4096, 8193, MemoryAccessIntent::Read) &&
        calls == 6 && accesses[5].bytes == 8193, "bulk access resolver receives entire exact footprint") && ok;
    const auto before = calls;
    ok = Check(!PrepareCpuMemoryAccess(page, 0, MemoryAccessIntent::Read) &&
        !PrepareCpuMemoryAccess(UINT64_MAX - 3, 8, MemoryAccessIntent::Write) &&
        !PrepareCpuMemoryAccess(page, 8, static_cast<MemoryAccessIntent>(4)) && calls == before,
        "invalid size/overflow/intent never reaches the resolver") && ok;
    return ok;
}
bool SplitReadGuards() {
    constexpr std::uint64_t input = UINT64_C(0x90000);
    constexpr std::uint64_t foreign = UINT64_C(0x9a000);
    constexpr std::uint64_t untouched = UINT64_C(0xc1000);
    constexpr std::uint64_t collision = input +
        (std::uint64_t {MemoryHandoffGuardCount} << MemoryHandoffPageShift);
    const auto before = calls;
    permit = false;
    MarkHandoffRange(input, 3 * 4096, MemoryAccessIntent::Write);
    MarkHandoffRange(foreign, 4096, MemoryAccessIntent::Read);
    PublishMemoryHandoffGpuDirty(true);
    bool ok = Check(MemoryHandoffReadGuardTable() != MemoryHandoffGuardTable() &&
        MemoryHandoffReadGuardDirectory()->load(std::memory_order_acquire) == MemoryHandoffReadGuardTable(),
        "dirty loads use a separate sticky table through the published directory");
    ok = Check(NeedsHandoff(input, 3 * 4096) &&
        !NeedsHandoff(input, 3 * 4096, MemoryAccessIntent::Read) &&
        PrepareCpuMemoryAccess(input + 4092, 8, MemoryAccessIntent::Read) && calls == before,
        "foreign GPU dirty ownership does not resolve clean input loads across pages") && ok;
    ok = Check(NeedsHandoff(foreign, 8, MemoryAccessIntent::Write) &&
        !PrepareCpuMemoryAccess(foreign, 8, MemoryAccessIntent::Read) && calls == before + 1,
        "read enrollment also watches writes and preserves a dirty reader denial") && ok;
    ok = Check(!PrepareCpuMemoryAccess(input, 8, MemoryAccessIntent::Write) &&
        !PrepareCpuMemoryAccess(input, 8, MemoryAccessIntent::ReadWrite) && calls == before + 3,
        "clean input still resolves stores and RMW while loads bypass") && ok;

    MarkHandoffRange(input + 4096, 4096, MemoryAccessIntent::Read);
    PublishMemoryHandoffGpuDirty(true);
    ok = Check(!PrepareCpuMemoryAccess(input + 4092, 8, MemoryAccessIntent::Read) && calls == before + 4,
        "writer upgrade on the second page catches a crossing read") && ok;
    ok = Check(!NeedsHandoff(input, 1, MemoryAccessIntent::Read) &&
        !NeedsHandoff(input + 8192, 1, MemoryAccessIntent::Read) &&
        NeedsHandoff(input, 8193, MemoryAccessIntent::Read) &&
        !PrepareCpuMemoryAccess(input, 8193, MemoryAccessIntent::Read) && calls == before + 5,
        "bulk read checks an upgraded middle page even when both endpoints are clean") && ok;
    MarkHandoffRange(collision, 1, MemoryAccessIntent::ReadWrite);
    MarkHandoffRange(input, 1, MemoryAccessIntent::Write);
    MarkHandoffRange(input, 1, MemoryAccessIntent::ReadWrite, false);
    ok = Check(NeedsHandoff(input, 1, MemoryAccessIntent::Read) &&
        NeedsHandoff(collision, 1, MemoryAccessIntent::Read),
        "colliding reads stay watched after write-only enrollment or retirement no-op") && ok;

    for (const auto invalid : {0u, 4u, UINT32_MAX}) {
        const auto intent = static_cast<MemoryAccessIntent>(invalid);
        MarkHandoffRange(untouched, 1, intent);
        ok = Check(!NeedsHandoff(untouched, 1) && !NeedsHandoff(untouched, 1, MemoryAccessIntent::Read) &&
            NeedsHandoff(untouched, 1, intent) && !PrepareCpuMemoryAccess(untouched, 1, intent) && calls == before + 5,
            "invalid intent cannot enroll guards or authorize an access") && ok;
    }
    MarkHandoffRange(untouched, 1, false);
    MarkHandoffRange(untouched, 1, MemoryAccessIntent::Read, false);
    ok = Check(!NeedsHandoff(untouched, 1), "both watched=false overloads retain untouched pages") && ok;
    MarkHandoffRange(untouched, 1);
    ok = Check(NeedsHandoff(untouched, 1, MemoryAccessIntent::Read) &&
        NeedsHandoff(untouched, 1, MemoryAccessIntent::Write),
        "default enrollment retains legacy read and write checks") && ok;
    std::printf("memory-handoff split-read clean-input/foreign-dirty/upgrade/cross-page/middle-page/RMW/default/invalid=verified\n");
    return ok;
}
bool CleanReadDirectory() {
    constexpr std::uint64_t page = UINT64_C(0x4b000);
    const auto* directory = MemoryHandoffReadGuardDirectory();
    const auto before = calls;
    permit = false;
    PublishMemoryHandoffGpuDirty(false);
    bool ok = Check(directory->load(std::memory_order_acquire) == nullptr &&
        PrepareCpuMemoryAccess(page + 4092, 8, MemoryAccessIntent::Read) && calls == before,
        "complete clean proof skips a watched cross-page read without resolver") &&
        Check(!PrepareCpuMemoryAccess(page, 8, MemoryAccessIntent::Write) && calls == before + 1 &&
            !PrepareCpuMemoryAccess(page, 8, MemoryAccessIntent::ReadWrite) && calls == before + 2,
            "clean reads never admit a store or RMW without the resolver");

    // Model the caller's existing execution admission, independently from the
    // filter. A writer cannot publish new ownership in the read-check/access
    // gap; a following reader must see the restored conservative directory.
    std::shared_mutex admission;
    std::shared_lock reader(admission);
    std::atomic<bool> writerStarted {}, writerPublished {};
    std::thread writer([&] {
        writerStarted.store(true, std::memory_order_release);
        std::unique_lock exclusive(admission);
        MarkHandoffRange(page + 4096, 4096);
        PublishMemoryHandoffGpuDirty(true);
        writerPublished.store(true, std::memory_order_release);
    });
    while (!writerStarted.load(std::memory_order_acquire)) std::this_thread::yield();
    ok = Check(!writerPublished.load(std::memory_order_acquire) &&
        PrepareCpuMemoryAccess(page + 4092, 8, MemoryAccessIntent::Read) && calls == before + 2,
        "reader retains clean proof until the actual access finishes") && ok;
    reader.unlock();
    writer.join();
    std::shared_lock nextReader(admission);
    ok = Check(writerPublished.load(std::memory_order_acquire) &&
        directory->load(std::memory_order_acquire) == MemoryHandoffReadGuardTable() &&
        !PrepareCpuMemoryAccess(page + 4092, 8, MemoryAccessIntent::Read) && calls == before + 3,
        "readmission observes the new writer and preserves denial") && ok;
    std::printf("memory-handoff read-directory clean/cross-page/write/RMW/republish=verified admission=modeled\n");
    return ok;
}

bool ExecutionPageProofReuse() {
    constexpr std::uint64_t page = UINT64_C(0x54000);
    MemoryHandoffPageCache cache;
    std::uint64_t epoch = 1, proofs = 0;
    // Physical alias peers cover the first and last bytes of the same page.
    // A request at +8 must still reject the remote peer's dirty/pending state.
    bool alias_dirty = false, first_pending = false, alias_pending = false;
    bool shape = true;
    const auto proof = [&](std::uint64_t base, std::uint64_t bytes, MemoryAccessIntent intent) noexcept {
        ++proofs;
        shape = shape && base == page && bytes == 4096;
        return !alias_dirty && (!WritesMemory(intent) || (first_pending && alias_pending));
    };
    bool ok = Check(cache.Try(epoch, page + 8, 4, MemoryAccessIntent::Read, proof) && proofs == 1,
        "whole-page read proof examines clean physical peers") &&
        Check(!cache.Try(epoch, page + 8, 4, MemoryAccessIntent::Write, proof) && proofs == 2,
        "read permission cannot admit first store before all peers are pending");
    first_pending = true;
    for (unsigned i = 0; i < 64; ++i)
        ok = Check(!cache.Try(epoch, page + 8, 4, MemoryAccessIntent::ReadWrite, proof),
            "pending first peer cannot authorize still-valid physical alias upload") && ok;
    alias_pending = true;
    // Retry deferral never grants a store; the exact-range path remains live.
    unsigned misses = 0;
    while (!cache.Try(epoch, page + 8, 4, MemoryAccessIntent::Write, proof) && misses < 64) ++misses;
    ok = Check(misses < 64 && shape, "all-peer write proof eventually retries over entire page") && ok;
    const auto repeated_before = proofs;
    std::uint64_t value = 23;
    for (unsigned i = 0; i < 64; ++i) {
        ok = Check(cache.Try(epoch, page + 8 + (i % 8) * 8, 8, MemoryAccessIntent::ReadWrite, proof),
            "same-value repeated stores retain pending intent within one execution admission") && ok;
        const std::uint64_t same = 23;
        std::memcpy(&value, &same, sizeof(value));
    }
    ok = Check(value == 23 && proofs == repeated_before,
        "64 admitted stores avoid 64 backing/tracker collectors") && ok;
    ++epoch; // Upload/readmission rearmed both peers, even for equal bytes.
    first_pending = alias_pending = false;
    ok = Check(!cache.Try(epoch, page + 8, 4, MemoryAccessIntent::ReadWrite, proof),
        "new execution admission invalidates an earlier same-value-upload write proof") && ok;
    ++epoch;
    alias_dirty = true;
    ok = Check(!cache.Try(epoch, page + 8, 4, MemoryAccessIntent::Read, proof),
        "new dirty alias outside requested bytes invalidates prior read proof") && ok;
    const auto invalid_before = proofs;
    ok = Check(!cache.Try(0, page + 8, 4, MemoryAccessIntent::Read, proof) &&
        !cache.Try(epoch, page + 4092, 8, MemoryAccessIntent::Read, proof) &&
        !cache.Try(epoch, page, 0, MemoryAccessIntent::Read, proof) &&
        !cache.Try(epoch, UINT64_MAX - 1, 4, MemoryAccessIntent::Write, proof) &&
        !cache.Try(epoch, page, 8, static_cast<MemoryAccessIntent>(0), proof) && proofs == invalid_before,
        "absent execution, cross-page, empty, overflow and invalid intent retain ordinary path") && ok;
    std::printf("memory-handoff page-cache repeated_stores=64 collectors_avoided=64 hits=%llu page_proofs=%llu "
                "deferred_proofs=%llu physical_peers=modeled\n",
        static_cast<unsigned long long>(cache.GetCounters().hits),
        static_cast<unsigned long long>(cache.GetCounters().page_proofs),
        static_cast<unsigned long long>(cache.GetCounters().deferred_proofs));
    return ok;
}

bool LargeMappedInterval() {
    // Saturate last: earlier fixtures require untouched read buckets. A
    // write-only full-table range must not accidentally enroll every reader.
    constexpr std::uint64_t untouched = UINT64_C(0xfe000000);
    const auto bytes = std::uint64_t {MemoryHandoffGuardCount} << MemoryHandoffPageShift;
    MarkHandoffRange(0, bytes, MemoryAccessIntent::Write);
    bool complete = true;
    for (std::size_t index = 0; index < MemoryHandoffGuardCount; ++index)
        complete = NeedsHandoff(static_cast<std::uint64_t>(index) << MemoryHandoffPageShift, 1) && complete;
    bool ok = Check(complete && !NeedsHandoff(untouched, 1, MemoryAccessIntent::Read),
        "large write-only interval covers every ordinary guard without enrolling untouched reads");
    MarkHandoffRange(0, bytes, MemoryAccessIntent::Read);
    complete = true;
    for (std::size_t index = 0; index < MemoryHandoffGuardCount; ++index)
        complete = NeedsHandoff(static_cast<std::uint64_t>(index) << MemoryHandoffPageShift,
            1, MemoryAccessIntent::Read) && complete;
    return Check(complete, "large read interval covers every sticky read guard") && ok;
}

bool OwnedGuardLifecycle() {
    constexpr std::uint64_t first = UINT64_C(0x120000), alias = UINT64_C(0x130000), remap = UINT64_C(0x140000);
    constexpr std::uint64_t collision = first + (std::uint64_t {MemoryHandoffGuardCount} << MemoryHandoffPageShift);
    const PhysicalMemoryRange backing {201, 0, 8192};
    const auto input = CreateMemoryHandoffOwner(), writer = CreateMemoryHandoffOwner();
    bool ok = Check(input != 0 && writer > input && ConfigureMemoryHandoffRegistration(true) &&
        !ConfigureMemoryHandoffRegistration(false), "live owner/query freezes the enabled capability");
    ok = Check(EnrollMemoryHandoffPhysicalRange(input, {201, 100, 4200}, MemoryAccessIntent::Write) &&
        MapMemoryHandoffOwnerPhysicalAlias(input, backing, first) && MapMemoryHandoffOwnerPhysicalAlias(input, backing, alias),
        "owned physical span enrolls all accepted aliases") && ok;
    for (unsigned i = 0; i < 32; ++i)
        ok = Check(EnrollMemoryHandoffPhysicalRange(input, {201, 100, 4200}, MemoryAccessIntent::Write) &&
            MapMemoryHandoffOwnerPhysicalAlias(input, backing, first), "repeated enrollment does not multiply ownership") && ok;
    ok = Check(NeedsHandoff(first, 8192, MemoryAccessIntent::Write) &&
        NeedsHandoff(alias + 4096, 1, MemoryAccessIntent::Write) &&
        !NeedsHandoff(first, 8192, MemoryAccessIntent::Read), "clean input guards writes across both pages only") && ok;
    ok = Check(EnrollMemoryHandoffPhysicalRange(writer, {202, 0, 4096}, MemoryAccessIntent::Read) &&
        MapMemoryHandoffOwnerPhysicalAlias(writer, {202, 0, 4096}, collision) &&
        NeedsHandoff(first, 1, MemoryAccessIntent::Read), "independent physical owner shares a colliding bucket") && ok;
    ok = Check(ReleaseMemoryHandoffOwner(writer) && !ReleaseMemoryHandoffOwner(writer) &&
        !NeedsHandoff(first, 1, MemoryAccessIntent::Read) && NeedsHandoff(first, 1, MemoryAccessIntent::Write),
        "retiring the colliding reader preserves the live write owner and rejects double release") && ok;

    ok = Check(EnrollMemoryHandoffPhysicalRange(input, {201, 100, 4200}, MemoryAccessIntent::Read) &&
        MapMemoryHandoffOwnerPhysicalAlias(input, backing, first) && MapMemoryHandoffOwnerPhysicalAlias(input, backing, alias) &&
        MapMemoryHandoffPhysicalAlias({201, 4096, 4096}, remap) &&
        NeedsHandoff(first, 1, MemoryAccessIntent::Read) && NeedsHandoff(alias, 8192, MemoryAccessIntent::Read) &&
        NeedsHandoff(remap + 100, 1, MemoryAccessIntent::Read),
        "read upgrade and offset remap preserve all live owner's physical intentions") && ok;
    const auto reused = CreateMemoryHandoffOwner();
    ok = Check(EnrollMemoryHandoffPhysicalRange(reused, {203, 0, 4096}, MemoryAccessIntent::Write) &&
        MapMemoryHandoffOwnerPhysicalAlias(reused, {203, 0, 4096}, alias) && ReleaseMemoryHandoffOwner(input) &&
        !NeedsHandoff(first, 8192) && !NeedsHandoff(remap, 4096) &&
        NeedsHandoff(alias, 1, MemoryAccessIntent::Write) && !NeedsHandoff(alias, 8192, MemoryAccessIntent::Read),
        "retirement removes old/remapped VAs but preserves another lifetime reusing an address") && ok;
    ok = Check(ReleaseMemoryHandoffOwner(reused) && !NeedsHandoff(alias, 8192),
        "last owner retirement eliminates historical guards") && ok;

    const auto legacy_peer = CreateMemoryHandoffOwner();
    ok = Check(EnrollMemoryHandoffPhysicalRange(legacy_peer, {204, 0, 4096}, MemoryAccessIntent::Write) &&
        MapMemoryHandoffOwnerPhysicalAlias(legacy_peer, {204, 0, 4096}, first), "owned guard can share a legacy bucket") && ok;
    MarkHandoffRange(collision, 1, MemoryAccessIntent::Read);
    ok = Check(ReleaseMemoryHandoffOwner(legacy_peer) && NeedsHandoff(first, 1, MemoryAccessIntent::Read) &&
        NeedsHandoff(first, 1, MemoryAccessIntent::Write), "owner release never clears an untracked legacy source") && ok;
    const auto invalid = CreateMemoryHandoffOwner();
    ok = Check(!EnrollMemoryHandoffPhysicalRange(invalid, {0, 0, 1}, MemoryAccessIntent::Read) &&
        !EnrollMemoryHandoffPhysicalRange(invalid, {205, UINT64_MAX, 1}, MemoryAccessIntent::Read) &&
        !EnrollMemoryHandoffPhysicalRange(writer, {205, 0, 1}, MemoryAccessIntent::Write) &&
        !MapMemoryHandoffPhysicalAlias({205, UINT64_MAX, 1}, remap) &&
        !MapMemoryHandoffPhysicalAlias({205, 0, 4096}, UINT64_MAX - 1) &&
        !MapMemoryHandoffOwnerPhysicalAlias(writer, {205, 0, 1}, remap) &&
        !MapMemoryHandoffOwnerPhysicalAlias(invalid, {205, 0, 1}, remap) &&
        !ReleaseMemoryHandoffOwner(UINT64_MAX) && !NeedsHandoff(remap, 4096) && ReleaseMemoryHandoffOwner(invalid),
        "unknown owner/backing and malformed alias cannot alter live or untouched guards") && ok;
    std::uint32_t count = UINT32_MAX - 1;
    std::uint8_t sticky = 0;
    HandoffDetail::IncrementCount(count, sticky);
    HandoffDetail::IncrementCount(count, sticky);
    HandoffDetail::DecrementCount(count);
    ok = Check(count == UINT32_MAX && sticky != 0, "saturated owner counts remain conservatively guarded") && ok;
    std::printf("memory-handoff owner-lifecycle collision/alias/remap/upgrade/reuse/legacy/invalid/saturation=%s\n", ok ? "PASS" : "FAIL");
    return ok;
}
}

int main() {
    const bool ok = PhysicalAliases() && GuardsAndIntent() && SplitReadGuards() &&
        CleanReadDirectory() && ExecutionPageProofReuse() && OwnedGuardLifecycle() && LargeMappedInterval();
    std::printf("memory-handoff aliases/intervals/collisions/intents/overflow=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
