// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdlib>
#include "common/logging/log.h"
#include "common/types.h"

namespace VideoCore::ResourcePublicationTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_RESOURCE_PUBLICATION_TRACE");
        const bool result = value && value[0] == '1' && value[1] == '\0';
        LOG_INFO(Render_Vulkan,
                 "[RESOURCE_PUBLICATION_TRACE] enabled={} default=0 category_limit=128",
                 result ? 1 : 0);
        return result;
    }();
    return enabled;
}

// A successful manual draw-window request can renew the new buffer diagnostic
// budgets once per process. The epoch never changes rendering or memory access.
inline std::atomic<u32> manual_capture_epoch{};

inline u32 CaptureEpoch() {
    return manual_capture_epoch.load(std::memory_order_acquire);
}

inline void BeginManualCapture() {
    if (!Enabled()) return;
    u32 expected = 0;
    if (manual_capture_epoch.compare_exchange_strong(expected, 1, std::memory_order_release,
                                                      std::memory_order_relaxed)) {
        LOG_INFO(Render_Vulkan,
                 "RESOURCE_PUBLICATION_CAPTURE capture_epoch=1 trigger=game_only_screenshot "
                 "buffer_budget_per_kind=32 manual_reset_limit=1");
    }
}

enum class Category { ImageExpand, BufferBindTickChange };
inline constexpr u32 EventLimit = 128;

inline u32 ReserveEvent(Category category) {
    if (!Enabled()) return 0;
    static std::atomic<u32> image_events{};
    static std::atomic<u32> buffer_events{};
    auto& counter = category == Category::ImageExpand ? image_events : buffer_events;
    u32 previous = counter.load(std::memory_order_relaxed);
    while (previous < EventLimit) {
        if (counter.compare_exchange_weak(previous, previous + 1, std::memory_order_relaxed)) {
            return previous + 1;
        }
    }
    return 0;
}

// Owner-thread diagnostic state only. It never controls command recording.
struct BufferBindContext {
    u64 vertex_input_tick{};
    u64 vertex_buffers_tick{};
    bool vertex_input_emitted{};
    bool vertex_buffers_emitted{};
};

inline void BufferObtain(const char* kind, VAddr address, u64 size, u64 before, u64 after,
                         const BufferBindContext& context) {
    if (before == after) return;
    const u32 event = ReserveEvent(Category::BufferBindTickChange);
    if (!event) return;
    LOG_WARNING(Render_Vulkan,
                "RESOURCE_PUBLICATION_BUFFER_TICK event={} budget_last={} kind={} addr={:#x} "
                "size={:#x} tick_before={} tick_after={} vertex_input_emitted={} "
                "vertex_input_tick={} vertex_buffers_emitted={} vertex_buffers_tick={} "
                "phase=after_obtain_before_current_bind",
                event, event == EventLimit, kind, address, size, before, after,
                context.vertex_input_emitted, context.vertex_input_tick,
                context.vertex_buffers_emitted, context.vertex_buffers_tick);
}

struct ImageExpansion {
    u32 event{};
    u64 tick_before{};
    u64 tick_after_refresh{};
    u32 source_flags_before{};
    u32 destination_flags_before{};
    u32 destination_flags_after_refresh{};
};

template <typename Image>
inline ImageExpansion BeginImageExpansion(const Image& source, const Image& destination,
                                          u64 tick) {
    const u32 event = ReserveEvent(Category::ImageExpand);
    if (!event) return {};
    return {.event = event,
            .tick_before = tick,
            .source_flags_before = static_cast<u32>(source.flags),
            .destination_flags_before = static_cast<u32>(destination.flags)};
}

template <typename Image>
inline void EndImageExpansion(const ImageExpansion& trace, const Image& source,
                              const Image& destination, u64 tick_after_copy) {
    if (!trace.event) return;
    LOG_WARNING(Render_Vulkan,
                "RESOURCE_PUBLICATION_IMAGE_EXPAND event={} budget_last={} addr={:#x} "
                "src_uid={} dst_uid={} src_flags_before={:#x} src_flags_at_copy={:#x} "
                "src_mips={} src_layers={} dst_mips={} dst_layers={} dst_flags_before={:#x} "
                "dst_flags_after_refresh={:#x} tick_before={} tick_after_refresh={} "
                "tick_after_copy={} order=refresh_new,copy_cached src_refresh=0 "
                "dirty_clear_after_copy=1",
                trace.event, trace.event == EventLimit, source.info.guest_address,
                source.image_uid, destination.image_uid, trace.source_flags_before,
                static_cast<u32>(source.flags), source.info.resources.levels,
                source.info.resources.layers, destination.info.resources.levels,
                destination.info.resources.layers, trace.destination_flags_before,
                trace.destination_flags_after_refresh, trace.tick_before,
                trace.tick_after_refresh, tick_after_copy);
}

} // namespace VideoCore::ResourcePublicationTrace
