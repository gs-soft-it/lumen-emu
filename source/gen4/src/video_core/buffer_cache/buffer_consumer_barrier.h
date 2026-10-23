// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include <optional>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace VideoCore {

constexpr bool BufferConsumerBarrierSelectorEnabled(const char* value) {
    return !value || value[0] != '0' || value[1] != '\0';
}

constexpr bool BufferShaderAccessBarrierSelectorEnabled(const char* value) {
    return value && value[0] == '1' && value[1] == '\0';
}

// Publish GPU writes to fixed-function consumers by default on every Gen4 host.
// Only exact "0" selects the previous behavior for rollback.
inline bool BufferConsumerBarrierEnabled() {
    static const bool enabled = BufferConsumerBarrierSelectorEnabled(
        std::getenv("LUMEN_BUFFER_CONSUMER_BARRIER"));
    return enabled;
}

// Descriptor synchronization remains a separate, default-OFF experiment.
inline bool BufferShaderAccessBarrierEnabled() {
    static const bool enabled = BufferShaderAccessBarrierSelectorEnabled(
        std::getenv("LUMEN_BUFFER_SHADER_ACCESS_BARRIER"));
    return enabled;
}

enum class BufferConsumer { Vertex, Index, DrawIndirect, DrawCount, DispatchIndirect, Count };

constexpr const char* BufferConsumerName(BufferConsumer consumer) {
    switch (consumer) {
    case BufferConsumer::Vertex: return "vertex";
    case BufferConsumer::Index: return "index";
    case BufferConsumer::DrawIndirect: return "draw_indirect";
    case BufferConsumer::DrawCount: return "draw_count";
    case BufferConsumer::DispatchIndirect: return "dispatch_indirect";
    default: return "invalid";
    }
}

inline vk::AccessFlags2 ShaderBufferAccessMask(bool is_written, bool enabled) {
    if (!is_written) return vk::AccessFlagBits2::eShaderRead;
    return enabled ? vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite
                   : vk::AccessFlags2{vk::AccessFlagBits2::eShaderWrite};
}

inline bool BufferAccessNeedsBarrier(vk::AccessFlags2 previous_access,
                                    vk::PipelineStageFlagBits2 previous_stage,
                                    vk::AccessFlags2 next_access,
                                    vk::PipelineStageFlagBits2 next_stage, bool enabled) {
    if (previous_access != next_access || previous_stage != next_stage) return true;
    // Repeated reads may share visibility. Repeated writes still require an
    // execution/memory dependency (including shader read-modify-write buffers).
    constexpr auto tracked_writes = vk::AccessFlagBits2::eShaderWrite |
                                    vk::AccessFlagBits2::eTransferWrite |
                                    vk::AccessFlagBits2::eHostWrite |
                                    vk::AccessFlagBits2::eMemoryWrite;
    return enabled && bool(next_access & tracked_writes);
}

// All descriptors in a draw/dispatch execute after the preparation barriers.
// A later read-only alias must not erase an earlier writable use of that command.
// This value lives inside Buffer, so SlotVector relocation cannot invalidate it.
struct ShaderBufferCommandAccess {
    u64 command_id{};
    vk::AccessFlags2 access{};
    u64 read_shader{};
    u64 write_shader{};
    bool has_read_only{};
    bool has_write{};

    vk::AccessFlags2 Add(u64 next_command, vk::AccessFlags2 requested, u64 shader) {
        if (command_id != next_command) {
            *this = {};
            command_id = next_command;
        }
        access |= requested;
        if (requested & vk::AccessFlagBits2::eShaderWrite) {
            if (!has_write) write_shader = shader;
            has_write = true;
        } else {
            if (!has_read_only) read_shader = shader;
            has_read_only = true;
        }
        return access;
    }
};

inline std::optional<vk::BufferMemoryBarrier2> UpdateBufferBarrier(
    vk::AccessFlags2& previous_access, vk::PipelineStageFlagBits2& previous_stage,
    vk::AccessFlags2 next_access, vk::PipelineStageFlagBits2 next_stage,
    vk::Buffer buffer, vk::DeviceSize offset, vk::DeviceSize size, bool enabled) {
    if (!BufferAccessNeedsBarrier(previous_access, previous_stage, next_access, next_stage,
                                 enabled)) {
        return {};
    }
    const vk::BufferMemoryBarrier2 barrier{
        .srcStageMask = previous_stage,
        .srcAccessMask = previous_access,
        .dstStageMask = next_stage,
        .dstAccessMask = next_access,
        .buffer = buffer,
        .offset = offset,
        .size = size,
    };
    previous_access = next_access;
    previous_stage = next_stage;
    return barrier;
}

template <typename Barriers>
void AppendShaderBufferBarrier(Barriers& barriers, const vk::BufferMemoryBarrier2& barrier) {
    for (auto& existing : barriers) {
        // Shader barriers currently cover the whole actual backing buffer.
        // Require an identical range so this helper cannot widen future subranges.
        if (existing.buffer == barrier.buffer && existing.offset == barrier.offset &&
            existing.size == barrier.size &&
            existing.srcQueueFamilyIndex == barrier.srcQueueFamilyIndex &&
            existing.dstQueueFamilyIndex == barrier.dstQueueFamilyIndex) {
            existing.srcStageMask |= barrier.srcStageMask;
            existing.srcAccessMask |= barrier.srcAccessMask;
            existing.dstStageMask |= barrier.dstStageMask;
            existing.dstAccessMask |= barrier.dstAccessMask;
            return;
        }
    }
    barriers.emplace_back(barrier);
}

constexpr bool BufferConsumerRangeFits(vk::DeviceSize buffer_size, vk::DeviceSize offset,
                                       vk::DeviceSize size) {
    return size != 0 && offset <= buffer_size && size <= buffer_size - offset;
}

inline std::optional<vk::BufferMemoryBarrier2> GetBufferConsumerBarrier(
    bool enabled, bool gpu_modified, BufferConsumer consumer, vk::Buffer buffer,
    vk::DeviceSize offset, vk::DeviceSize size) {
    if (!enabled || !gpu_modified || !buffer || size == 0 || consumer == BufferConsumer::Count) {
        return {};
    }
    const bool vertex = consumer == BufferConsumer::Vertex;
    const bool index = consumer == BufferConsumer::Index;
    // Descriptor barriers are accumulated before fixed inputs and emitted
    // afterwards. Their tracked state is already updated, so it cannot be used
    // as the source scope here. Publish any preceding GPU writer directly.
    return vk::BufferMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vertex || index ? vk::PipelineStageFlagBits2::eVertexInput
                                       : vk::PipelineStageFlagBits2::eDrawIndirect,
        .dstAccessMask = vertex ? vk::AccessFlagBits2::eVertexAttributeRead
                               : index ? vk::AccessFlagBits2::eIndexRead
                                       : vk::AccessFlagBits2::eIndirectCommandRead,
        .buffer = buffer,
        .offset = offset,
        .size = size,
    };
}

} // namespace VideoCore
