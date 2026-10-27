// SPDX-License-Identifier: GPL-2.0-only
#include "common/renderTiming.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
void Check(bool value) {
    if (!value) std::abort();
}

void CheckReadbackSamples(bool enabled) {
    using namespace Common::RenderTiming;
    Check(std::strcmp(ReadbackOriginName(ReadbackOrigin::GuestMemoryRead), "guestMemoryRead") == 0);
    Check(std::strcmp(ReadbackOriginName(ReadbackOrigin::GuestMemoryWrite), "guestMemoryWrite") == 0);
    for (const auto origin : {ReadbackOrigin::Other, ReadbackOrigin::GuestReadFault,
                              ReadbackOrigin::GuestWriteFault, ReadbackOrigin::ColorMetadata,
                              ReadbackOrigin::Invalidation, ReadbackOrigin::GuestMemoryRead,
                              ReadbackOrigin::GuestMemoryWrite}) {
        ScopedReadbackOrigin caller(origin);
        for (const bool write : {false, true}) {
            const auto api = write ? ReadbackOrigin::GuestMemoryWrite : ReadbackOrigin::GuestMemoryRead;
            const auto fallback = write ? ReadbackOrigin::GuestWriteFault : ReadbackOrigin::GuestReadFault;
            Check(ReadbackOriginForFault(write) == (enabled && origin == api ? api : fallback));
        }
    }
    Check(CurrentReadbackOrigin() == ReadbackOrigin::Other);
    {
        ScopedReadbackOrigin caller(ReadbackOrigin::GuestMemoryRead);
        Check(CurrentReadbackOrigin() == (enabled ? ReadbackOrigin::GuestMemoryRead : ReadbackOrigin::Other));
        {
            ScopedReadbackOrigin nested(ReadbackOrigin::ColorMetadata);
            Check(CurrentReadbackOrigin() == (enabled ? ReadbackOrigin::ColorMetadata : ReadbackOrigin::Other));
        }
        const auto captured_origin = CurrentReadbackOrigin();
        // This is the explicit capture/restore used by ReadMemory when a fault
        // caller hands its work to the renderer owner via SendCommandSync.
        std::thread owner([captured_origin, enabled] {
            Check(CurrentReadbackOrigin() == ReadbackOrigin::Other);
            {
                ScopedReadbackOrigin restored(captured_origin);
                Check(CurrentReadbackOrigin() == (enabled ? ReadbackOrigin::GuestMemoryRead : ReadbackOrigin::Other));
            }
            Check(CurrentReadbackOrigin() == ReadbackOrigin::Other);
        });
        owner.join();
        Check(CurrentReadbackOrigin() == captured_origin);
    }
    Check(CurrentReadbackOrigin() == ReadbackOrigin::Other);

    const auto record = [](uint64_t sequence) {
        RecordReadback({
            .request_address = 0x800000 + sequence * 0x1000,
            .request_size = sequence % 64 + 1,
            .buffer_address = 0x800000 + sequence * 0x1000 - 0x80,
            .buffer_size = 0x1000,
            .payload_bytes = sequence * 3,
            .staging_bytes = sequence * 3 + 64,
            .native_wait_ns = sequence * 7,
            .origin = sequence % 2 ? ReadbackOrigin::GuestMemoryRead : ReadbackOrigin::GuestMemoryWrite,
            .caller_gpu_thread = sequence % 3 == 0, .requested_dirty = sequence % 5 == 0
        });
    };
    const auto validate = [enabled](const ReadbackSnapshot& snapshot) {
        Check(snapshot.enabled == enabled);
        Check(snapshot.count <= ReadbackCapacity);
        if (!enabled) {
            Check(snapshot.completed == 0 && snapshot.count == 0);
            return;
        }
        uint64_t previous = 0;
        uint64_t previous_time = 0;
        for (std::size_t i = 0; i < snapshot.count; ++i) {
            const auto& sample = snapshot.samples[i];
            Check(sample.sequence > previous && sample.sequence <= snapshot.completed);
            Check(snapshot.completed - sample.sequence < ReadbackCapacity);
            Check(sample.steady_ns > 0 && sample.steady_ns >= previous_time);
            Check(sample.request_address == 0x800000 + sample.sequence * 0x1000);
            Check(sample.request_size == sample.sequence % 64 + 1);
            Check(sample.buffer_address + 0x80 == sample.request_address);
            Check(sample.buffer_size == 0x1000);
            Check(sample.payload_bytes == sample.sequence * 3);
            Check(sample.staging_bytes == sample.payload_bytes + 64);
            Check(sample.native_wait_ns == sample.sequence * 7);
            Check(sample.origin == (sample.sequence % 2 ? ReadbackOrigin::GuestMemoryRead : ReadbackOrigin::GuestMemoryWrite));
            Check(sample.caller_gpu_thread == (sample.sequence % 3 == 0));
            Check(sample.requested_dirty == (sample.sequence % 5 == 0));
            previous = sample.sequence;
            previous_time = sample.steady_ns;
        }
    };

    const auto empty = ReadReadbacks();
    validate(empty);
    Check(empty.completed == 0 && empty.count == 0);
    constexpr uint64_t initial = ReadbackCapacity * 2 + 7;
    for (uint64_t sequence = 1; sequence <= initial; ++sequence) record(sequence);
    const auto wrapped = ReadReadbacks();
    validate(wrapped);
    Check(wrapped.completed == (enabled ? initial : 0));
    Check(wrapped.count == (enabled ? ReadbackCapacity : 0));
    if (enabled) Check(wrapped.samples.front().sequence == initial - ReadbackCapacity + 1);

    // One renderer producer, one independently polling diagnostic consumer.
    // Cross-field relationships catch torn publication while the ring wraps.
    std::atomic_bool done {false};
    constexpr uint64_t concurrent_records = 8192;
    std::thread producer([&] {
        for (uint64_t sequence = initial + 1; sequence <= initial + concurrent_records; ++sequence) {
            record(sequence);
            if (sequence % 32 == 0) std::this_thread::yield();
        }
        done.store(true);
    });
    do {
        validate(ReadReadbacks());
    } while (!done.load());
    producer.join();
    const auto final = ReadReadbacks();
    validate(final);
    Check(final.completed == (enabled ? initial + concurrent_records : 0));
    Check(final.count == (enabled ? ReadbackCapacity : 0));
}
}

int main(int argc, char** argv) {
    using namespace Common::RenderTiming;
    const char* supplied = argc == 2 ? argv[1] : nullptr;
    if (supplied) Check(setenv("LUMEN_PS5_RENDER_TIMING", supplied, 1) == 0);
    else Check(unsetenv("LUMEN_PS5_RENDER_TIMING") == 0);
    const bool expected = supplied && std::strcmp(supplied, "1") == 0;
    Check(Enabled() == expected);
    // A later environment mutation must not split the counters' enabled state.
    Check(setenv("LUMEN_PS5_RENDER_TIMING", expected ? "0" : "1", 1) == 0);
    Check(Enabled() == expected);
    const auto before = Read();
    Check(before.enabled == expected);
    for (auto value : before.values) Check(value == 0);
    const auto begin = Begin();
    Check((begin != TimePoint {}) == expected);
    Finish(Counter::QueueSubmitNs, begin);
    {
        ScopedWait renderer(false);
    }
    {
        ScopedWait completion(true);
    }
    const auto counter_value = [](Counter counter) {
        return Read().values[static_cast<std::size_t>(counter)];
    };
    {
        ScopedSyncDownloadGpuWait download;
        {
            ScopedSyncDownloadGpuWait nested;
            const auto nested_before = counter_value(Counter::SyncDownloadGpuWaitNs);
            {
                ScopedWait native_wait(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Check(nested.ElapsedNs() == counter_value(Counter::SyncDownloadGpuWaitNs) - nested_before);
        }
        const auto attributed = counter_value(Counter::SyncDownloadGpuWaitNs);
        Check((attributed > 0) == expected);
        Check(download.ElapsedNs() == attributed);
        // A different rendering thread must not inherit the attribution.
        std::thread unrelated([] { ScopedWait native_wait(false); });
        unrelated.join();
        Check(counter_value(Counter::SyncDownloadGpuWaitNs) == attributed);
        const auto renderer_before = counter_value(Counter::RendererGpuWaitNs);
        {
            ScopedWait after_nested(false);
        }
        Check(counter_value(Counter::SyncDownloadGpuWaitNs) - attributed ==
              counter_value(Counter::RendererGpuWaitNs) - renderer_before);
        const auto before_priority = counter_value(Counter::SyncDownloadGpuWaitNs);
        {
            ScopedWait priority(true);
        }
        Check(counter_value(Counter::SyncDownloadGpuWaitNs) == before_priority);
        Check(download.ElapsedNs() == counter_value(Counter::SyncDownloadGpuWaitNs));
    }
    const auto attributed = counter_value(Counter::SyncDownloadGpuWaitNs);
    {
        ScopedWait after_download(false);
    }
    Check(counter_value(Counter::SyncDownloadGpuWaitNs) == attributed);
    const auto priority_begin = Begin();
    Finish(Counter::SyncDownloadPriorityWaitNs, priority_begin);
    constexpr unsigned workers = 4;
    constexpr unsigned iterations = 4000;
    std::array<std::thread, workers> threads;
    for (auto& thread : threads) {
        thread = std::thread([] {
            for (unsigned i = 0; i < iterations; ++i) {
                Add(Counter::QueueSubmits);
                Add(Counter::ReleaseMemPackets, 3);
                Add(Counter::SyncBufferDownloads);
                Add(Counter::SyncDownloadPayloadBytes, 9);
                Add(Counter::SyncDownloadStagingBytes, 64);
                Add(Counter::GuestMainBlankPresents);
                Add(Counter::GuestMainSurfacePresents, 2);
                Add(Counter::GuestMainMissingPresents, 3);
                Add(Counter::ColorClearMaterializations, 4);
                Add(Counter::VideoOutColorClearMaterializations);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    const auto after = Read();
    Check(after.steady_ns >= before.steady_ns);
    const auto value = [&](Counter counter) { return after.values[static_cast<std::size_t>(counter)]; };
    Check(value(Counter::QueueSubmits) == (expected ? workers * iterations : 0));
    Check(value(Counter::ReleaseMemPackets) == (expected ? workers * iterations * 3 : 0));
    Check(value(Counter::RendererGpuWaits) == (expected ? 5 : 0));
    Check(value(Counter::PriorityGpuWaits) == (expected ? 2 : 0));
    Check(value(Counter::SyncBufferDownloads) == (expected ? workers * iterations : 0));
    Check(value(Counter::SyncDownloadPayloadBytes) == (expected ? workers * iterations * 9 : 0));
    Check(value(Counter::SyncDownloadStagingBytes) == (expected ? workers * iterations * 64 : 0));
    Check(value(Counter::GuestMainBlankPresents) == (expected ? workers * iterations : 0));
    Check(value(Counter::GuestMainSurfacePresents) == (expected ? workers * iterations * 2 : 0));
    Check(value(Counter::GuestMainMissingPresents) == (expected ? workers * iterations * 3 : 0));
    Check(value(Counter::ColorClearMaterializations) == (expected ? workers * iterations * 4 : 0));
    Check(value(Counter::VideoOutColorClearMaterializations) == (expected ? workers * iterations : 0));
    if (!expected) {
        for (auto count : after.values) Check(count == 0);
    }
    CheckReadbackSamples(expected);
    std::puts("PASS: render timing opt-in/latch, OFF counters, nested/thread-local readback wait attribution, wait lanes, concurrent byte totals, readback origins and atomic ring wrap/snapshots");
}
