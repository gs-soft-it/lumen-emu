// SPDX-License-Identifier: GPL-2.0-only
#include "common/videoUploadProbe.h"

#include <cstdio>
#include <thread>

namespace Probe = Common::VideoUploadProbe;
namespace Timing = Common::RenderTiming;
using Counter = Timing::Counter;
static void Check(bool value) { if (!value) std::abort(); }
static uint64_t Value(Counter counter) {
    return Timing::Read().values[static_cast<std::size_t>(counter)];
}

static void CheckConcurrentSlotLifecycle() {
    constexpr uint64_t Generations = 10000;
    constexpr std::array<uint64_t, 3> Queries {0x1080, 0x2000, 0x3080};
    auto handle = Probe::Register(0x1000, 0x100);
    Check(handle.slot != Probe::Capacity);
    const auto slot = handle.slot;
    const auto first_version = Probe::Detail::entries[slot].version.load();
    std::atomic<uint64_t> observed {0};
    std::atomic<bool> done {false};
    std::thread reader([&] {
        uint64_t latest = 0;
        while (!done.load(std::memory_order_acquire)) {
            for (std::size_t query = 0; query < Queries.size(); ++query) {
                const auto sample = Probe::Detail::Read(slot, Queries[query], Queries[query] + 1);
                // Retirement and concurrent publication may legitimately be unstable.
                if (sample.version == 0 || sample.writing || sample.unstable) continue;
                Check(sample.version >= first_version);
                const auto delta = sample.version - first_version;
                // Each lifecycle has exactly one unregister and one register.
                Check(delta % 4 == 0 && delta / 4 <= Generations);
                const bool upper_range = (delta / 4) % 2 != 0;
                Check(sample.hit == (query == (upper_range ? 2u : 0u)));
                if (sample.version > latest) {
                    latest = sample.version;
                    observed.store(latest, std::memory_order_release);
                }
            }
        }
    });
    for (uint64_t generation = 0; generation <= Generations; ++generation) {
        // Ensure every publication is sampled without assuming when either thread runs.
        // The reader keeps sampling while the next retirement/publication proceeds.
        while (observed.load(std::memory_order_acquire) < first_version + generation * 4)
            std::this_thread::yield();
        if (generation == Generations) break;
        Probe::Unregister(handle);
        handle = Probe::Register(generation % 2 == 0 ? 0x3000 : 0x1000, 0x100);
        Check(handle.slot == slot);
    }
    done.store(true, std::memory_order_release);
    reader.join();
    Probe::Unregister(handle);
}

int main(int argc, char** argv) {
    const bool enabled = argc == 2 && std::strcmp(argv[1], "1") == 0;
    Check(setenv("LUMEN_PS5_RENDER_TIMING", enabled ? "1" : "0", 1) == 0);
    Check(Timing::Enabled() == enabled);
    if (!enabled) {
        const auto sequence = Probe::Detail::sequence.load();
        auto handle = Probe::Register(0x1000, 0x100);
        Check(handle.slot == Probe::Capacity);
        {
            Probe::WriteScope write({0});
            Probe::SnapshotScope snapshot(0x1000, 0x100);
        }
        handle = {0};
        Probe::Unregister(handle);
        Check(handle.slot == Probe::Capacity && Probe::Detail::sequence.load() == sequence);
        for (const auto& entry: Probe::Detail::entries)
            Check(entry.version.load() == 0 && entry.begin.load() == 0 && entry.end.load() == 0);
        for (auto value: Timing::Read().values) Check(value == 0);
        std::puts("PASS: video upload probe OFF has no registry mutation or counters");
        return 0;
    }
    Check(Probe::Register(UINT64_MAX - 1, 2).slot == Probe::Capacity);
    auto handle = Probe::Register(0x1000, 0x100);
    Check(handle.slot != Probe::Capacity);
    { Probe::SnapshotScope snapshot(0x1080, 0x20); }
    { Probe::SnapshotScope outside(0x1100, 1); }
    Check(Value(Counter::VideoUploadSnapshots) == 1 && Value(Counter::VideoUploadRaces) == 0);
    {
        Probe::WriteScope writer(handle);
        Probe::SnapshotScope snapshot(0x1000, 0x100);
    }
    {
        Probe::SnapshotScope snapshot(0x1000, 0x100);
        { Probe::WriteScope writer(handle); }
    }
    Check(Value(Counter::VideoUploadRaces) == 2);
    {
        Probe::SnapshotScope snapshot(0x1000, 0x100);
        Probe::Unregister(handle);
        handle = Probe::Register(0x1000, 0x100);
    }
    Check(Value(Counter::VideoUploadRaces) == 3);
    {
        Probe::SnapshotScope outer(0x1080, 0x100);
        Probe::SnapshotScope inner(0x10ff, 1);
    }
    Check(Value(Counter::VideoUploadSnapshots) == 6 && Value(Counter::VideoUploadRaces) == 3);
    {
        Probe::SnapshotScope snapshot(0x1000, 0x100);
        std::thread writer([handle] { Probe::WriteScope write(handle); });
        writer.join();
    }
    Check(Value(Counter::VideoUploadRaces) == 4);
    auto moved = handle;
    handle = {};
    Probe::Unregister(handle);
    { Probe::SnapshotScope snapshot(0x1000, 0x100); }
    Check(Value(Counter::VideoUploadSnapshots) == 8);
    Probe::Unregister(moved);
    { Probe::SnapshotScope retired(0x1000, 0x100); }
    Check(Value(Counter::VideoUploadSnapshots) == 8);
    std::array<Probe::Handle, Probe::Capacity> handles;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        handles[i] = Probe::Register(0x1000 + i * 0x100, 0x100);
        Check(handles[i].slot != Probe::Capacity);
    }
    Check(Probe::Register(0x100000, 0x100).slot == Probe::Capacity);
    Check(Value(Counter::VideoProbeRegistryFull) == 1);
    for (auto& active: handles) Probe::Unregister(active);
    Check(Value(Counter::VideoUploadUnstable) == 0);
    CheckConcurrentSlotLifecycle();
    std::puts("PASS: video upload probe partial/nested ranges, generations, lifecycle, threads and overflow");
}
