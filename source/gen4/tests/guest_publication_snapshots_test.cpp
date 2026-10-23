// SPDX-License-Identifier: MIT
#include "video_core/guest_publication_snapshots.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
using Snapshots = VideoCore::GuestPublicationSnapshots;
constexpr std::uint64_t A = 0x10000, B = A + Snapshots::PageSize, C = B + Snapshots::PageSize;
struct Backing {
    std::map<std::uint64_t, Snapshots::Page> pages;
    std::size_t reads{};
    void Read(std::uint64_t page, std::span<std::uint8_t, Snapshots::PageSize> output) {
        ++reads;
        const auto it = pages.find(page);
        CHECK(it != pages.end());
        std::copy(it->second.begin(), it->second.end(), output.begin());
    }
    auto Reader() {
        return [this](auto address, auto bytes) { Read(address, bytes); };
    }
};

void UnchangedAndCpuWrites() {
    Backing memory{{{A, {}}, {B, {}}, {C, {}}}};
    Snapshots snapshots;
    for (const auto page : {A, B, C}) snapshots.Track(page, memory.Reader());
    const auto before_scan = memory.reads;
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()).empty());
    CHECK(memory.reads == before_scan + 3);
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 0);
    CHECK(memory.reads == before_scan + 3); // No second copy of unchanged pages.

    memory.pages[B][23] = 0x5a;
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()) == std::vector<std::uint64_t>{B});
    const auto before_commit = memory.reads;
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 1);
    CHECK(memory.reads == before_commit + 1);
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()).empty());
}

void ReadbackOverlapAndFailure() {
    Backing memory{{{A, {}}, {B, {}}, {C, {}}}};
    Snapshots snapshots;
    for (const auto page : {A, B, C}) snapshots.Track(page, memory.Reader());
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()).empty());

    // An unaligned readback spans exactly two 4 KiB snapshot pages.
    memory.pages[A].back() = 0xa1;
    memory.pages[B].front() = 0xb2;
    CHECK(snapshots.RecordBackingWrite(B - 1, 2, true));
    // Overlapping image/buffer aliases record the same physical page once.
    CHECK(snapshots.RecordBackingWrite(B, 1, true));
    const auto before_commit = memory.reads;
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 2);
    CHECK(memory.reads == before_commit + 2);
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()).empty());

    CHECK(!snapshots.RecordBackingWrite(C, 4, false));
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 0);
    CHECK(snapshots.RecordBackingWrite(B, 0, true));
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 0);

    // Even if a failed writer changed bytes before reporting failure, it
    // cannot hide them by advancing the baseline. Production keeps its abort.
    memory.pages[C][0] = 0x7c;
    CHECK(!snapshots.RecordBackingWrite(C, 1, false));
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 0);
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()) == std::vector<std::uint64_t>{C});
}

void TrackingAndRemapBetweenScanAndCommit() {
    Backing memory{{{A, {}}, {B, {}}, {C, {}}}};
    Snapshots snapshots;
    snapshots.Track(A, memory.Reader());
    memory.pages[A][0] = 1;
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()) == std::vector<std::uint64_t>{A});
    const auto read_count = memory.reads;
    snapshots.Track(A, memory.Reader()); // Another alias keeps the prior baseline.
    CHECK(memory.reads == read_count);
    snapshots.Untrack(A);
    memory.pages.erase(A);
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 0); // No dangling read.

    // Same VA, different mapping: old pending metadata must not be inherited.
    memory.pages[A].fill(0x42);
    snapshots.Track(A, memory.Reader());
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 0);
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()).empty());
    memory.pages[A][1] = 0x43;
    CHECK(snapshots.RecordBackingWrite(A + 1, 1, true));
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 1);
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()).empty());

    // A write before the first watcher is already present in its new baseline.
    memory.pages[B][9] = 9;
    CHECK(snapshots.RecordBackingWrite(B + 9, 1, true));
    snapshots.Track(B, memory.Reader());
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 0);
    // A write after adding the watcher must refresh that new snapshot.
    snapshots.Track(C, memory.Reader());
    memory.pages[C][9] = 8;
    CHECK(snapshots.RecordBackingWrite(C + 9, 1, true));
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 1);

    CHECK(snapshots.RecordBackingWrite(B - 1, 2, true));
    snapshots.UnmapRange(B - 1, 2); // Partial overlap removes A and B, not C.
    memory.pages.erase(A);
    memory.pages.erase(B);
    CHECK(snapshots.CommitPublishedMemory(memory.Reader()) == 0);
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()).empty());
    memory.pages[C][2] = 2;
    CHECK(snapshots.ObserveCpuChanges(memory.Reader()) == std::vector<std::uint64_t>{C});
}

void DistinctVirtualAliasesAndConservativeObservation() {
    Snapshots snapshots;
    Snapshots::Page physical{}, replacement{};
    bool second_alias_remapped{};
    const auto read = [&](std::uint64_t address, auto bytes) {
        CHECK(address == A || address == B);
        const auto& page = address == B && second_alias_remapped ? replacement : physical;
        std::copy(page.begin(), page.end(), bytes.begin());
    };
    snapshots.Track(A, read);
    snapshots.Track(B, read); // Distinct virtual pages share one physical page.
    CHECK(snapshots.ObserveCpuChanges(read).empty());
    physical[5] = 0x91;
    // These are the two aliases returned by the physical writer. Recording
    // only the requested VA would leave the second alias with a stale baseline.
    const std::array aliases{std::pair{A + 5, std::uint64_t{1}},
                             std::pair{B + 5, std::uint64_t{1}}};
    for (const auto& [address, size] : aliases)
        CHECK(snapshots.RecordBackingWrite(address, size, true));
    CHECK(snapshots.CommitPublishedMemory(read) == 2);
    CHECK(snapshots.ObserveCpuChanges(read).empty());

    CHECK(snapshots.RecordBackingWrite(B, 1, true));
    snapshots.UnmapRange(B, Snapshots::PageSize);
    second_alias_remapped = true;
    replacement.fill(0x37);
    snapshots.Track(B, read);
    physical[5] = 0x92;
    CHECK(snapshots.RecordBackingWrite(A + 5, 1, true));
    CHECK(snapshots.CommitPublishedMemory(read) == 1); // No pending old B identity.
    CHECK(snapshots.ObserveCpuChanges(read).empty());

    // A native write absent from the explicit report is not hidden. Its old
    // baseline survives, so the next mandatory scan still detects the change.
    physical[7] = 0x94;
    CHECK(snapshots.CommitPublishedMemory(read) == 0);
    CHECK(snapshots.ObserveCpuChanges(read) == std::vector<std::uint64_t>{A});
}
} // namespace

int main() {
    UnchangedAndCpuWrites();
    ReadbackOverlapAndFailure();
    TrackingAndRemapBetweenScanAndCommit();
    DistinctVirtualAliasesAndConservativeObservation();
    std::puts("GuestPublicationSnapshotsTest: PASS unchanged-copy=0 cpu-write=1 readback-overlap=1 failed-write=1 tracking=1 alias-remap=1 physical-alias=1 conservative-native-write=1");
}
