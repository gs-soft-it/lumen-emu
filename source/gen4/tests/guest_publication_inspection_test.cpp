// SPDX-License-Identifier: MIT
#include "video_core/amdgpu/pm4_publication.h"
#include "video_core/guest_publication_inspection.h"
#include "video_core/guest_publication_snapshots.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
using Snapshots = VideoCore::GuestPublicationSnapshots;
constexpr std::uint64_t A = 0x10000, B = 0x20000;
struct Fixture {
    // Two tracked VA pages backed by the same physical page. The snapshot and
    // inspection helpers are production code; Vulkan cache consumers are
    // represented by the observed-page vector in this deterministic test.
    Snapshots::Page physical{};
    Snapshots snapshots;
    VideoCore::GuestPublicationInspection inspection;
    std::size_t reads{}, scans{};
    std::vector<std::uint64_t> invalidated;

    auto Reader() {
        return [this](auto address, auto bytes) {
            CHECK(address == A || address == B);
            ++reads;
            std::copy(physical.begin(), physical.end(), bytes.begin());
        };
    }
    Fixture() {
        snapshots.Track(A, Reader());
        snapshots.Track(B, Reader());
    }
    bool Inspect(bool requested) {
        if (!inspection.BeginInspection(requested, snapshots.HasPendingRefresh())) return false;
        ++scans;
        const auto changed = snapshots.ObserveCpuChanges(Reader());
        invalidated.insert(invalidated.end(), changed.begin(), changed.end());
        return true;
    }
    void Readback(std::uint8_t value) {
        physical[71] = value;
        CHECK(snapshots.RecordBackingWrite(A + 71, 1, true));
        CHECK(snapshots.RecordBackingWrite(B + 71, 1, true));
    }
    void Commit() { snapshots.CommitPublishedMemory(Reader()); }
};

void OpcodeBoundaries() {
    std::size_t admitted{};
    for (unsigned value = 0; value != 256; ++value) {
        const auto op = static_cast<AmdGpu::PM4ItOpcode>(value);
        if (AmdGpu::GraphicsPacketMaySkipHostWriteInspection(op)) {
            CHECK(AmdGpu::GraphicsPacketNeedsPublication(op));
            ++admitted;
        }
    }
    CHECK(admitted == 13);
    CHECK(AmdGpu::GraphicsPacketMaySkipHostWriteInspection(AmdGpu::PM4ItOpcode::Nop, true));
    CHECK(!AmdGpu::GraphicsPacketMaySkipHostWriteInspection(AmdGpu::PM4ItOpcode::Nop, false));
    // The per-branch proof may only widen NOP, never another packet kind.
    for (unsigned value = 0; value != 256; ++value) {
        const auto op = static_cast<AmdGpu::PM4ItOpcode>(value);
        if (op != AmdGpu::PM4ItOpcode::Nop) {
            CHECK(AmdGpu::GraphicsPacketMaySkipHostWriteInspection(op, true) ==
                  AmdGpu::GraphicsPacketMaySkipHostWriteInspection(op, false));
        }
    }
    for (const auto op : {AmdGpu::PM4ItOpcode::Nop, AmdGpu::PM4ItOpcode::DmaData,
                          AmdGpu::PM4ItOpcode::WriteData, AmdGpu::PM4ItOpcode::WaitRegMem,
                          static_cast<AmdGpu::PM4ItOpcode>(0xff)}) {
        CHECK(!AmdGpu::GraphicsPacketMaySkipHostWriteInspection(op));
    }
}

void NativePacketsKeepPublicationAndWriterGuards() {
    Fixture f;
    std::size_t ends{}, published{};
    auto end_packet = [&](AmdGpu::PM4ItOpcode opcode, bool native_nop = false) {
        CHECK(AmdGpu::GraphicsPacketNeedsPublication(opcode));
        ++ends;
        const bool scanned = f.Inspect(
            !AmdGpu::GraphicsPacketMaySkipHostWriteInspection(opcode, native_nop));
        // Model the existing End's readback phase after its inspection phase.
        // It must run even when the packet body needed no full scan.
        f.Readback(static_cast<std::uint8_t>(++published));
        f.Commit();
        return scanned;
    };
    CHECK(f.Inspect(true)); // Outer CPU admission is always conservative.
    f.Commit();
    // Padding, enabled/disabled debug markers and ignored NOP payloads all
    // reach native-only decoder branches; none removes its End publication.
    for (unsigned branch = 0; branch != 4; ++branch) {
        CHECK(!end_packet(AmdGpu::PM4ItOpcode::Nop, true));
    }
    CHECK(!end_packet(AmdGpu::PM4ItOpcode::IncrementDeCounter));
    CHECK(!end_packet(AmdGpu::PM4ItOpcode::PfpSyncMe));
    // Compute shaders can write native GPU resources. Their graphics-queue
    // packets still execute End's GPU readback, Finish and commit phases.
    CHECK(!end_packet(AmdGpu::PM4ItOpcode::DispatchDirect));
    CHECK(!end_packet(AmdGpu::PM4ItOpcode::DispatchIndirect));
    CHECK(!end_packet(AmdGpu::PM4ItOpcode::AcquireMem));
    CHECK(f.invalidated.empty());
    CHECK(ends == 9 && published == 9 && f.scans == 1);

    f.Readback(0x70); // A reported alias write before the packet's End.
    CHECK(end_packet(AmdGpu::PM4ItOpcode::Nop, true));
    CHECK(f.invalidated.size() == 2);
    f.invalidated.clear();
    {
        const auto callback = f.inspection.TrackNestedHostCallback();
        CHECK(f.Inspect(false));
        f.Commit();
        f.physical[73] = 0x71; // Unreported write after a callback's own scan.
    }
    CHECK(end_packet(AmdGpu::PM4ItOpcode::Nop, true));
    CHECK(f.invalidated.size() == 2);
    f.invalidated.clear();

    // PatchedFlip invokes IRQ callbacks directly, outside ExecuteCommand's
    // nested-callback scope. Its decoder must leave the proof false.
    f.physical[74] = 0x72;
    CHECK(end_packet(AmdGpu::PM4ItOpcode::Nop, false));
    CHECK(f.invalidated.size() == 2);
    CHECK(ends == 12 && published == 12);
    CHECK(f.Inspect(true)); // Outer end also remains conservative.
    f.Commit();
    f.invalidated.clear();
    f.physical[75] = 0x73; // Next admitted guest CPU phase.
    CHECK(f.Inspect(true));
    CHECK(f.invalidated.size() == 2);
}

void NativeNopDeferralKeepsWritersAndOuterBoundaries() {
    Fixture f;
    std::size_t ends{}, finishes{}, deferred{};
    bool pending_gpu_write = false;
    auto can_defer = [&] {
        return f.inspection.CanDeferPublication(f.snapshots.HasPendingRefresh());
    };
    auto end_publication = [&](bool inspect = true) {
        ++ends;
        f.Inspect(inspect);
        if (pending_gpu_write) {
            f.Readback(0x64); // Model the retained GPU readback phase.
            pending_gpu_write = false;
        }
        ++finishes;
        f.Commit();
    };
    auto native_nop = [&] {
        if (can_defer()) {
            ++deferred;
            return;
        }
        end_publication(false); // Same scan request as the native NOP decoder.
    };
    CHECK(f.Inspect(true)); // Mandatory Begin; CPU is excluded for this resume.
    for (unsigned nop = 0; nop != 670; ++nop) native_nop();
    CHECK(deferred == 670 && ends == 0 && finishes == 0);
    CHECK(f.scans == 1);

    f.Readback(0x61); // An alias backing write prevents eliding its boundary.
    CHECK(!can_defer());
    CHECK(!can_defer()); // The query cannot consume pending refresh.
    native_nop();
    CHECK(ends == 1 && finishes == 1);
    CHECK(f.invalidated.size() == 2);
    f.invalidated.clear();
    CHECK(can_defer());

    {
        const auto callback = f.inspection.TrackNestedHostCallback();
        CHECK(!can_defer());
        CHECK(!can_defer()); // The query cannot consume the entry marker.
        CHECK(f.Inspect(false)); // An internal scan consumes only the marker.
        f.Commit();
        CHECK(!can_defer()); // Active callback still forbids deferral.
        pending_gpu_write = true; // An opaque callback can enqueue GPU writes.
        f.physical[81] = 0x62; // And issue unreported native stores.
        native_nop();
        CHECK(!pending_gpu_write && f.physical[71] == 0x64);
        CHECK(f.invalidated.size() == 2);
        f.invalidated.clear();
        CHECK(!can_defer()); // End inside the callback cannot drop its depth.
        f.physical[82] = 0x63; // Store after the callback's internal End.
    }
    CHECK(!f.snapshots.HasPendingRefresh());
    CHECK(!can_defer());
    CHECK(!can_defer()); // Nor consume the return marker.
    native_nop();
    CHECK(f.invalidated.size() == 2);
    f.invalidated.clear();
    CHECK(ends == 3 && finishes == 3 && can_defer());

    // Even when a trailing NOP defers, a coroutine yield always retains the
    // unconditional outer End before the CPU is allowed to execute again.
    native_nop();
    end_publication();
    CHECK(ends == 4 && finishes == 4);
    f.physical[83] = 0x65; // Guest CPU writes between resumes.
    CHECK(f.Inspect(true)); // Next outer Begin sees both tracked aliases.
    CHECK(f.invalidated.size() == 2);
    CHECK(!can_defer()); // Begin's pending baseline also forces the next End.
    native_nop();
    CHECK(ends == 5 && finishes == 5 && can_defer());
    native_nop();
    end_publication(); // Task completion retains outer End too.
    CHECK(ends == 6 && finishes == 6 && !pending_gpu_write);
}

void PureDrawThenCpuAdmission() {
    Fixture f;
    CHECK(f.Inspect(true)); // Outer admission always scans.
    f.Commit();
    const auto outer_callback = f.inspection.TrackNestedHostCallback(false);
    for (unsigned draw = 0; draw < 4; ++draw) {
        const auto reads_before = f.reads;
        CHECK(!f.Inspect(false));
        CHECK(f.reads == reads_before); // No page traversal in pure-draw scan phase.
        f.Readback(static_cast<std::uint8_t>(draw + 1)); // Existing End readback phase.
        f.Commit(); // Publication bytes remain the baseline of both aliases.
        CHECK(!f.snapshots.HasPendingRefresh());
    }
    CHECK(f.invalidated.empty());
    CHECK(f.scans == 1);
    CHECK(f.Inspect(true)); // Outer end remains unconditional even after a draw.
    f.Commit();
    f.physical[93] = 0x7a; // Guest CPU writes only after the publication was released.
    CHECK(f.Inspect(true)); // Next outer Begin must observe both aliases.
    CHECK(f.invalidated.size() == 2);
}

void ReadbackInsideDrawRetainsCacheInvalidations() {
    Fixture f;
    CHECK(f.Inspect(true));
    f.Commit();
    f.Readback(0x55); // E.g. depth/stencil backing write before draw End.
    CHECK(f.Inspect(false)); // Alias refresh requires conservative full observation.
    CHECK(f.invalidated.size() == 2);
    f.Commit();
    CHECK(!f.Inspect(false));
}

void OpaqueCallbackWithoutWriteReporting() {
    Fixture f;
    CHECK(f.Inspect(true));
    f.Commit();
    {
        const auto callback = f.inspection.TrackNestedHostCallback();
        f.physical[17] = 0x62; // Deliberately not reported through RecordBackingWrite.
    }
    CHECK(!f.snapshots.HasPendingRefresh());
    CHECK(!AmdGpu::GraphicsPacketNeedsPublication(AmdGpu::PM4ItOpcode::SetContextReg));
    // State-only packets do not call inspection and cannot clear the callback.
    CHECK(f.Inspect(false));
    CHECK(f.invalidated.size() == 2);
    f.Commit();
    CHECK(!f.Inspect(false));
}

void ReentrantCallbackDuringInspection() {
    Fixture f;
    CHECK(f.Inspect(true));
    // A callback from cache invalidation occurs after ObserveCpuChanges took
    // its page snapshots; resetting the guard after the scan would lose it.
    {
        const auto callback = f.inspection.TrackNestedHostCallback();
        f.physical[18] = 0x63;
    }
    f.Commit();
    CHECK(f.Inspect(false));
    CHECK(f.invalidated.size() == 2);
    f.Commit();
    CHECK(!f.Inspect(false));
}

void UntrackedAliasCannotEraseOtherAliasOrOpaqueCallback() {
    Fixture f;
    CHECK(f.Inspect(true));
    f.Commit();
    f.Readback(0x23);
    f.snapshots.Untrack(A);
    CHECK(f.snapshots.HasPendingRefresh());
    CHECK(f.Inspect(false));
    CHECK(f.invalidated == std::vector<std::uint64_t>{B});
    f.Commit();
    {
        const auto callback = f.inspection.TrackNestedHostCallback();
        f.snapshots.UnmapRange(B, Snapshots::PageSize);
    }
    CHECK(!f.snapshots.HasPendingRefresh());
    CHECK(f.Inspect(false)); // A remap/untrack cannot clear the opaque-writer guard.
}

void CallbackInspectionThenRawWriteAndNestedScopes() {
    Fixture f;
    CHECK(f.Inspect(true));
    f.Commit();
    {
        const auto callback = f.inspection.TrackNestedHostCallback();
        CHECK(f.Inspect(false)); // Consume entry marker inside the callback.
        f.Commit();
        CHECK(f.Inspect(false)); // Active depth still forbids skipping.
        {
            const auto nested_callback = f.inspection.TrackNestedHostCallback();
            CHECK(f.Inspect(false));
            f.Commit();
        }
        CHECK(f.Inspect(false)); // Consume inner return marker.
        f.Commit();
        CHECK(f.Inspect(false)); // Inner return cannot drop the outer depth.
        f.physical[51] = 0x73; // Raw write after the last internal scan.
        CHECK(!f.snapshots.HasPendingRefresh());
        CHECK(f.Inspect(false)); // A draw inside the callback still sees it.
        CHECK(f.invalidated.size() == 2);
        f.Commit();
        f.invalidated.clear();
        f.physical[52] = 0x74; // Another raw write immediately before return.
    }
    CHECK(!f.snapshots.HasPendingRefresh());
    CHECK(f.Inspect(false)); // Return marker must require final observation.
    CHECK(f.invalidated.size() == 2);
    f.Commit();
    CHECK(!f.Inspect(false)); // No leaked active depth after both scopes close.
}

void CallbackExceptionalExit() {
    Fixture f;
    CHECK(f.Inspect(true));
    f.Commit();
    try {
        const auto callback = f.inspection.TrackNestedHostCallback();
        CHECK(f.Inspect(false));
        f.Commit();
        f.physical[61] = 0x75;
        throw 7;
    } catch (int value) {
        CHECK(value == 7);
    }
    CHECK(f.Inspect(false));
    CHECK(f.invalidated.size() == 2);
    f.Commit();
    CHECK(!f.Inspect(false)); // RAII balanced depth on exceptional return too.
}
}

int main() {
    OpcodeBoundaries();
    NativePacketsKeepPublicationAndWriterGuards();
    NativeNopDeferralKeepsWritersAndOuterBoundaries();
    PureDrawThenCpuAdmission();
    ReadbackInsideDrawRetainsCacheInvalidations();
    OpaqueCallbackWithoutWriteReporting();
    ReentrantCallbackDuringInspection();
    UntrackedAliasCannotEraseOtherAliasOrOpaqueCallback();
    CallbackInspectionThenRawWriteAndNestedScopes();
    CallbackExceptionalExit();
    std::puts("PASS guarded publication inspection: 8 draw + 5 other gfx opcodes; per-branch NOP; non-consuming native NOP deferral; pending writer and outer boundaries retained; pure draw; outer CPU admission; pre-End readback aliases; opaque/reentrant callbacks; untrack/remap; scan then raw write; nested depth; exceptional exit");
}
