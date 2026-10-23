// Lumen PS5 diagnostics. SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_PS5_GPU_SLICE_TRACE_H_
#define LUMEN_PS5_GPU_SLICE_TRACE_H_

#include "common/threads.h"
#include "common/timer.h"
#include "common/assert.h"

#include <atomic>
#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace Libs::Graphics {

// A bounded diagnostic prefix of real GPU Process invocations. Nested Finish,
// semaphore and callback waits are inclusive subintervals, never additive frame
// costs. Presentation joins carry an explicit FlipQueue request identity; no
// association is inferred from temporal proximity or a recycled native frame.
struct GpuSliceTrace {
    static constexpr uint64_t Limit = 8192;
    enum class Phase { Finish, NativeWait, PriorityWait };
    struct Counter { uint64_t calls = 0, ns = 0; };

    static bool Enabled() noexcept {
        static const bool enabled = [] {
            const auto* value = std::getenv("LUMEN_PS5_GPU_SLICE_TRACE");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }
    static uint64_t Now() noexcept {
        const auto ticks = Common::Timer::QueryPerformanceCounter();
        static const auto frequency = Common::Timer::QueryPerformanceFrequency();
        // Timer uses CLOCK_MONOTONIC at 1 GHz on Mac/Android. Windows uses QPC;
        // that clock is deliberately labelled separately and must not be joined
        // to a POSIX timeline. Divide first to avoid overflow with long uptime.
        return ticks / frequency * 1000000000ull +
            ticks % frequency * 1000000000ull / frequency;
    }
    static uint64_t DelayMs() noexcept {
        static const uint64_t delay = [] {
            const auto* value = std::getenv("LUMEN_PS5_GPU_SLICE_TRACE_DELAY_MS");
            if (value == nullptr) return uint64_t {0};
            uint64_t result = 0;
            const auto* end = value + std::strlen(value);
            const auto parsed = std::from_chars(value, end, result);
            if (parsed.ec != std::errc {} || parsed.ptr != end || result > 3600000)
                EXIT("Invalid LUMEN_PS5_GPU_SLICE_TRACE_DELAY_MS (expected 0..3600000)\n");
            return result;
        }();
        return delay;
    }
    static const std::filesystem::path& ArmFile() {
        static const auto path = [] {
            const auto* value = std::getenv("LUMEN_PS5_GPU_SLICE_TRACE_ARM_FILE");
            return value == nullptr ? std::filesystem::path {} : std::filesystem::path {value};
        }();
        return path;
    }
    static uint64_t StartOrigin() noexcept {
        if (ArmFile().empty()) {
            static const auto origin = Now();
            return origin;
        }
        static std::atomic<uint64_t> latched_origin {0};
        if (const auto origin = latched_origin.load(std::memory_order_acquire); origin != 0)
            return origin;
        // Read-only until the marker is observed. The second check under the
        // lock ensures even concurrent slice entry performs no filesystem I/O
        // after the latch. Removing the marker later does not re-arm tracing.
        static std::mutex latch_mutex;
        std::lock_guard lock(latch_mutex);
        if (const auto origin = latched_origin.load(std::memory_order_relaxed); origin != 0)
            return origin;
        std::error_code error;
        if (!std::filesystem::exists(ArmFile(), error) || error) return 0;
        const auto origin = Now();
        latched_origin.store(origin, std::memory_order_release);
        return origin;
    }
    inline static thread_local GpuSliceTrace* current = nullptr;

    struct Scope {
        explicit Scope(Phase phase) noexcept: trace(current), kind(phase) {
            if (trace != nullptr) start = Now();
        }
        ~Scope() {
            if (trace == nullptr) return;
            auto& counter = kind == Phase::Finish ? trace->finish :
                kind == Phase::NativeWait ? trace->native_wait : trace->priority_wait;
            ++counter.calls;
            counter.ns += Now() - start;
        }
        GpuSliceTrace* trace;
        Phase kind;
        uint64_t start = 0;
    };

    GpuSliceTrace(uint32_t queue, uint32_t submission_type) noexcept:
        queue_id(queue), type(submission_type) {
        if (!Enabled()) return;
        const auto origin = StartOrigin();
        if (origin == 0) return;
        const auto now = Now();
        if (now - origin < DelayMs() * 1000000ull) return;
        static std::atomic<uint64_t> sequence {0};
        serial = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        if (serial == 1) std::fprintf(stderr,
            "PS5_GPU_SLICE_TRACE_V1 enabled=1 limit=%" PRIu64 " delay_ms=%" PRIu64
            " delay_origin_ns=%" PRIu64
            " arm_file=%u arm_latched_ns=%" PRIu64 " delay_origin=%s"
            " scope=process_slice clock=%s coverage=prefix"
            " nested_phase_totals=inclusive frame_join=explicit_request\n", Limit, DelayMs(), origin,
            ArmFile().empty() ? 0u : 1u, ArmFile().empty() ? uint64_t {0} : origin,
            ArmFile().empty() ? "first_process" : "arm_file",
#if defined(_WIN32)
            "qpc_ns");
#else
            "monotonic_ns");
#endif
        if (serial > Limit) {
            if (serial == Limit + 1) std::fprintf(stderr,
                "PS5_GPU_SLICE_TRACE_V1 limit_reached=1 attempted_prefix=%" PRIu64
                " subsequent_slices=unrecorded census_complete=0\n", Limit);
            return;
        }
        start_ns = now;
        previous = current;
        current = this;
        recording = true;
    }
    ~GpuSliceTrace() { if (recording) current = previous; }
    GpuSliceTrace(const GpuSliceTrace&) = delete;
    GpuSliceTrace& operator=(const GpuSliceTrace&) = delete;

    void Admitted(uint64_t epoch, uint64_t tick) noexcept {
        if (!recording) return;
        admitted_ns = Now();
        epoch_token = epoch;
        tick_begin = tick;
    }
    void BeginTail() noexcept { if (recording) tail_ns = Now(); }
    void BeginClose() noexcept { if (recording) close_ns = Now(); }
    [[nodiscard]] uint64_t Sequence() const noexcept { return recording ? serial : 0; }
    [[nodiscard]] uint64_t Epoch() const noexcept { return epoch_token; }
    void End(bool complete, uint64_t tick) noexcept {
        if (!recording) return;
        const auto end_ns = Now();
        // Detach before logging so diagnostics cannot account for themselves.
        current = previous;
        recording = false;
        std::fprintf(stderr,
            "PS5_GPU_SLICE_TIMING_V1 sequence=%" PRIu64 " tid=%d"
            " start_ns=%" PRIu64 " end_ns=%" PRIu64
            " submit_id=%" PRIu64 " queue_id=%u type=%u epoch_token=%" PRIu64
            " scheduler_tick_begin=%" PRIu64 " scheduler_tick_end=%" PRIu64
            " admission_ns=%" PRIu64 " process_ns=%" PRIu64
            " finish_slice_ns=%" PRIu64 " close_ns=%" PRIu64
            " finish_calls=%" PRIu64 " finish_ns=%" PRIu64
            " native_wait_calls=%" PRIu64 " native_wait_ns=%" PRIu64
            " priority_wait_calls=%" PRIu64 " priority_wait_ns=%" PRIu64
            " native_submits=%" PRIu64 " buffer_readback_bytes=%" PRIu64
            " buffer_hash_considered=%" PRIu64 " buffer_hash_attempts=%" PRIu64
            " buffer_hash_pages=%" PRIu64 " buffer_hash_bytes=%" PRIu64
            " buffer_hash_copy_avoided_bytes=%" PRIu64 " buffer_hash_fallbacks=%" PRIu64
            " buffer_hash_fallback_mask=%" PRIu64 " buffer_revalidation_copy_bytes=%" PRIu64
            " image_readback_bytes=%" PRIu64 " image_seed_bytes=%" PRIu64
            " image_cpu_copyout_bytes=%" PRIu64 " image_host_import_considered=%" PRIu64
            " image_host_import_attempts=%" PRIu64 " image_host_import_bytes=%" PRIu64
            " image_host_import_fallbacks=%" PRIu64 " image_host_import_denial_mask=%" PRIu64
            " image_host_import_batches=%" PRIu64 " image_host_import_batch_owners=%" PRIu64
            " image_host_import_batch_cap_stops=%" PRIu64
            " image_host_import_drain_ns=%" PRIu64 " image_host_import_ns=%" PRIu64
            " image_owner_trace_records=%" PRIu64 " image_owner_trace_drops=%" PRIu64
            " image_binding_trace_records=%" PRIu64 " image_binding_trace_drops=%" PRIu64
            " queued_submissions=%u ready_queue_heads=%u pending_commands=%u"
            " private_eligible=%d complete=%u\n",
            serial, Common::Thread::GetThreadIdUnique(), start_ns, end_ns,
            submit_id, queue_id, type, epoch_token, tick_begin, tick,
            admitted_ns - start_ns, tail_ns - admitted_ns, close_ns - tail_ns,
            end_ns - close_ns, finish.calls, finish.ns, native_wait.calls,
            native_wait.ns, priority_wait.calls, priority_wait.ns, native_submits,
            buffer_readback_bytes, buffer_hash_considered, buffer_hash_attempts,
            buffer_hash_pages, buffer_hash_bytes, buffer_hash_copy_avoided_bytes,
            buffer_hash_fallbacks, buffer_hash_fallback_mask, buffer_revalidation_copy_bytes,
            image_readback_bytes, image_seed_bytes,
            image_cpu_copyout_bytes, image_host_import_considered,
            image_host_import_attempts, image_host_import_bytes,
            image_host_import_fallbacks, image_host_import_denial_mask,
            image_host_import_batches, image_host_import_batch_owners, image_host_import_batch_cap_stops,
            image_host_import_drain_ns, image_host_import_ns,
            image_owner_trace_records, image_owner_trace_drops,
            image_binding_trace_records, image_binding_trace_drops,
            queued_submissions, ready_queue_heads, pending_commands,
            private_eligible, complete ? 1u : 0u);
    }

    bool recording = false;
    uint64_t submit_id = 0;
    uint32_t queued_submissions = 0, ready_queue_heads = 0, pending_commands = 0;
    int private_eligible = -1;
    uint64_t native_submits = 0, buffer_readback_bytes = 0;
    uint64_t buffer_hash_considered = 0, buffer_hash_attempts = 0, buffer_hash_pages = 0;
    uint64_t buffer_hash_bytes = 0, buffer_hash_copy_avoided_bytes = 0;
    uint64_t buffer_hash_fallbacks = 0, buffer_hash_fallback_mask = 0, buffer_revalidation_copy_bytes = 0;
    uint64_t image_readback_bytes = 0, image_seed_bytes = 0;
    uint64_t image_cpu_copyout_bytes = 0, image_host_import_considered = 0;
    uint64_t image_host_import_attempts = 0, image_host_import_bytes = 0;
    uint64_t image_host_import_fallbacks = 0, image_host_import_denial_mask = 0;
    uint64_t image_host_import_batches = 0, image_host_import_batch_owners = 0, image_host_import_batch_cap_stops = 0;
    uint64_t image_host_import_drain_ns = 0, image_host_import_ns = 0;
    uint64_t image_owner_trace_records = 0, image_owner_trace_drops = 0;
    uint64_t image_binding_trace_records = 0, image_binding_trace_drops = 0;
    Counter finish, native_wait, priority_wait;

private:
    GpuSliceTrace* previous = nullptr;
    uint64_t serial = 0, start_ns = 0, admitted_ns = 0, tail_ns = 0, close_ns = 0;
    uint64_t epoch_token = 0, tick_begin = 0;
    uint32_t queue_id, type;
};

} // namespace Libs::Graphics
#endif
