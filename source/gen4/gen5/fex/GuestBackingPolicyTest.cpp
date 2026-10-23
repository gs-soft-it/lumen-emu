// Lumen ownership/hash fixture, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "graphics/host_gpu/guestBackingPolicy.h"

#include <array>
#include <cstdio>

int main() {
    using namespace Libs::Graphics;
    std::array<std::uint8_t, 4096> guest {};
    for (std::size_t i = 0; i < guest.size(); ++i) guest[i] = static_cast<std::uint8_t>(i);
    auto staged = guest;
    GuestBackingSnapshots snapshots;
    constexpr std::uint64_t address = 0x10000;
    bool ok = snapshots.Compare(address, guest.data(), guest.size()) == BackingComparison::Missing;
    snapshots.Publish(address, staged.data(), staged.size());
    ok &= snapshots.Compare(address, guest.data(), guest.size()) == BackingComparison::Unchanged;
    // A CPU write after staging must compare against the submitted snapshot,
    // including middle bytes that the former edge-only texture hash omitted.
    for (const std::size_t offset : {std::size_t {0}, std::size_t {2048}, std::size_t {4095}}) {
        guest[offset] ^= 1;
        ok &= snapshots.Compare(address, guest.data(), guest.size()) == BackingComparison::Changed;
        ok &= snapshots.Compare(address, staged.data(), staged.size()) == BackingComparison::Unchanged;
        guest[offset] ^= 1;
    }
    guest[2048] ^= 1;
    staged = guest;
    snapshots.Publish(address, staged.data(), staged.size());
    ok &= snapshots.Compare(address, guest.data(), guest.size()) == BackingComparison::Unchanged;
    ok &= snapshots.Compare(address, guest.data(), guest.size() - 1) == BackingComparison::Missing;
    ok &= snapshots.Compare(address + 4096, guest.data(), guest.size()) == BackingComparison::Missing;
    snapshots.Forget(address, staged.size());
    ok &= snapshots.Compare(address, guest.data(), guest.size()) == BackingComparison::Missing;
    snapshots.Publish(address, staged.data(), staged.size());
    snapshots.Forget(address + 2048, 1);
    ok &= snapshots.Compare(address, guest.data(), guest.size()) == BackingComparison::Missing;
    snapshots.Publish(address, staged.data(), staged.size());
    snapshots.Publish(address + 4096, staged.data(), staged.size());
    snapshots.Forget(UINT64_MAX - 1, 8);
    ok &= snapshots.Compare(address, guest.data(), guest.size()) == BackingComparison::Missing;
    ok &= snapshots.Compare(address + 4096, guest.data(), guest.size()) == BackingComparison::Missing;
    // No snapshot can substitute for a completed CPU/GPU publication contract.
    ok &= !GuestGpuWriteOwnershipSupported();
    std::printf("PS5 backing policy: %s; staging baseline, full-page mutations, lifetime removal, GPU writes denied\n",
                ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
