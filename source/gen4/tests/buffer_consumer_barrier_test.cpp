// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/buffer_cache/buffer_consumer_barrier.h"

#include <array>
#include <cstdio>
#include <limits>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #condition); return 1; \
} } while (false)

int main(int argc, char** argv) {
    using namespace VideoCore;
    if (argc == 3) {
        CHECK(BufferConsumerBarrierEnabled() == (argv[1][0] == '1'));
        CHECK(BufferShaderAccessBarrierEnabled() == (argv[2][0] == '1'));
        std::puts("independent buffer barrier selectors: PASS");
        return 0;
    }
    using Access = vk::AccessFlagBits2;
    using Stage = vk::PipelineStageFlagBits2;
    CHECK(BufferConsumerBarrierSelectorEnabled(nullptr));
    CHECK(!BufferConsumerBarrierSelectorEnabled("0"));
    CHECK(BufferConsumerBarrierSelectorEnabled("1"));
    for (const char* value : {"", "00", "01", "10", "true", "false", "invalid", " 0", "0 "}) {
        CHECK(BufferConsumerBarrierSelectorEnabled(value));
    }
    CHECK(!BufferShaderAccessBarrierSelectorEnabled(nullptr));
    CHECK(BufferShaderAccessBarrierSelectorEnabled("1"));
    for (const char* value : {"", "0", "00", "01", "10", "true", "false", "invalid", " 1", "1 "}) {
        CHECK(!BufferShaderAccessBarrierSelectorEnabled(value));
    }

    const auto buffer = vk::Buffer{reinterpret_cast<VkBuffer>(uintptr_t{1})};
    CHECK(!GetBufferConsumerBarrier(false, true, BufferConsumer::Vertex, buffer, 96, 64));
    CHECK(!GetBufferConsumerBarrier(true, false, BufferConsumer::Vertex, buffer, 96, 64));
    CHECK(!GetBufferConsumerBarrier(true, true, BufferConsumer::Vertex, {}, 96, 64));
    CHECK(!GetBufferConsumerBarrier(true, true, BufferConsumer::Vertex, buffer, 96, 0));
    CHECK(BufferConsumerRangeFits(256, 96, 64));
    CHECK(BufferConsumerRangeFits(256, 128, 128));
    CHECK(!BufferConsumerRangeFits(256, 257, 1));
    CHECK(!BufferConsumerRangeFits(256, 255, 2));
    CHECK(!BufferConsumerRangeFits(256, 0, 0));
    CHECK(!BufferConsumerRangeFits(256, std::numeric_limits<vk::DeviceSize>::max(), 2));

    // Witness: compute writes version 7; descriptor preparation changes the
    // tracked access to ShaderRead, but its dependency is still queued. A
    // fixed-input barrier derived from that new read state loses the writer.
    vk::AccessFlags2 tracked_access = Access::eShaderWrite;
    CHECK(BufferAccessNeedsBarrier(tracked_access, Stage::eAllCommands,
                                  Access::eShaderRead, Stage::eAllCommands, true));
    const vk::BufferMemoryBarrier2 deferred_descriptor{
        .srcStageMask = Stage::eAllCommands,
        .srcAccessMask = tracked_access,
        .dstStageMask = Stage::eAllCommands,
        .dstAccessMask = Access::eShaderRead,
        .buffer = buffer,
        .offset = 96,
        .size = 64,
    };
    tracked_access = Access::eShaderRead;
    const auto fixed = GetBufferConsumerBarrier(true, true, BufferConsumer::Vertex,
                                                buffer, 96, 64);
    CHECK(fixed && fixed->srcAccessMask == Access::eMemoryWrite);
    CHECK(fixed->srcStageMask == Stage::eAllCommands);
    CHECK(fixed->dstStageMask == Stage::eVertexInput);
    CHECK(fixed->dstAccessMask == Access::eVertexAttributeRead);
    CHECK(fixed->buffer == buffer && fixed->offset == 96 && fixed->size == 64);
    CHECK(tracked_access == Access::eShaderRead); // no mutation of queued state

    // A small visibility model verifies the dependency reaches the actual
    // consumer even when the descriptor dependency is emitted later. It does
    // not stand in for a Vulkan execution test.
    auto vertex_visible_version = [](const vk::BufferMemoryBarrier2& barrier) {
        const bool publishes_writer = bool(barrier.srcAccessMask &
                                          (Access::eMemoryWrite | Access::eShaderWrite));
        const bool targets_vertex = bool(barrier.dstAccessMask & Access::eVertexAttributeRead);
        return publishes_writer && targets_vertex ? 7 : 0;
    };
    CHECK(vertex_visible_version(deferred_descriptor) == 0);
    auto wrong = *fixed;
    wrong.srcAccessMask = tracked_access;
    CHECK(vertex_visible_version(wrong) == 0);
    CHECK(vertex_visible_version(*fixed) == 7);

    for (const auto consumer : {BufferConsumer::Index, BufferConsumer::DrawIndirect,
                               BufferConsumer::DrawCount, BufferConsumer::DispatchIndirect}) {
        const auto barrier = GetBufferConsumerBarrier(true, true, consumer, buffer, 32, 12);
        CHECK(barrier && barrier->offset == 32 && barrier->size == 12);
        CHECK(barrier->srcAccessMask == Access::eMemoryWrite);
        CHECK(barrier->dstAccessMask == (consumer == BufferConsumer::Index
                                            ? Access::eIndexRead : Access::eIndirectCommandRead));
        CHECK(barrier->dstStageMask == (consumer == BufferConsumer::Index
                                           ? Stage::eVertexInput : Stage::eDrawIndirect));
    }

    const auto read = ShaderBufferAccessMask(false, true);
    const auto rmw = ShaderBufferAccessMask(true, true);
    CHECK(read == Access::eShaderRead);
    CHECK(rmw == (Access::eShaderRead | Access::eShaderWrite));
    CHECK(ShaderBufferAccessMask(true, false) == Access::eShaderWrite);
    CHECK(!BufferAccessNeedsBarrier(read, Stage::eAllCommands, read, Stage::eAllCommands, true));
    CHECK(BufferAccessNeedsBarrier(rmw, Stage::eAllCommands, rmw, Stage::eAllCommands, true));
    CHECK(!BufferAccessNeedsBarrier(rmw, Stage::eAllCommands, rmw, Stage::eAllCommands, false));
    CHECK(BufferAccessNeedsBarrier(Access::eShaderWrite, Stage::eAllCommands,
                                  Access::eShaderWrite, Stage::eAllCommands, true));
    CHECK(BufferAccessNeedsBarrier(Access::eTransferWrite, Stage::eTransfer,
                                  Access::eTransferWrite, Stage::eTransfer, true));
    CHECK(BufferAccessNeedsBarrier(read, Stage::eAllCommands, rmw, Stage::eAllCommands, true));

    // Execute the same command accumulator, transition and coalescing helpers
    // used by Buffer/Rasterizer, without creating a Vulkan device. Binding A's
    // descriptors is not execution: every resulting dependency is before A.
    struct TrackedResource {
        vk::Buffer handle;
        vk::AccessFlags2 access;
        Stage stage{Stage::eAllCommands};
        ShaderBufferCommandAccess command;

        std::optional<vk::BufferMemoryBarrier2> Prepare(u64 id, vk::AccessFlags2 requested,
                                                       bool union_enabled = true) {
            const auto destination = union_enabled ? command.Add(id, requested, 0x1234)
                                                   : requested;
            return UpdateBufferBarrier(access, stage, destination, Stage::eAllCommands,
                                       handle, 0, 4096, true);
        }
    };
    const auto publishes_shader_write = [](const auto& barrier) {
        return barrier && bool(barrier->srcAccessMask &
                               (Access::eShaderWrite | Access::eMemoryWrite)) &&
               bool(barrier->dstAccessMask & Access::eShaderRead);
    };
    // Regression witness: with candidate2's per-descriptor policy, W then R in
    // A leaves R state. B's read gets no dependency on the write performed by A.
    TrackedResource previous_policy{buffer, read};
    previous_policy.Prepare(1, rmw, false);
    previous_policy.Prepare(1, read, false);
    CHECK(!publishes_shader_write(previous_policy.Prepare(2, read, false)));

    TrackedResource write_then_read{buffer, read};
    std::vector<vk::BufferMemoryBarrier2> queued;
    for (const auto requested : {rmw, read}) {
        if (const auto barrier = write_then_read.Prepare(1, requested)) {
            AppendShaderBufferBarrier(queued, *barrier);
        }
    }
    CHECK(queued.size() == 1);
    CHECK(write_then_read.access == rmw && queued[0].dstAccessMask == rmw);
    CHECK(write_then_read.command.has_write && write_then_read.command.has_read_only);
    CHECK(publishes_shader_write(write_then_read.Prepare(2, read)));
    CHECK(write_then_read.command.access == read); // a new command resets the union
    CHECK(!write_then_read.command.has_write);

    // Reverse descriptor order with a real preceding writer. The single
    // pre-A dependency must retain its write source and both A destinations.
    TrackedResource read_then_write{buffer, Access::eShaderWrite};
    queued.clear();
    for (const auto requested : {read, rmw}) {
        if (const auto barrier = read_then_write.Prepare(3, requested)) {
            AppendShaderBufferBarrier(queued, *barrier);
        }
    }
    CHECK(queued.size() == 1);
    CHECK(bool(queued[0].srcAccessMask & Access::eShaderWrite));
    CHECK(queued[0].dstAccessMask == rmw && read_then_write.access == rmw);
    CHECK(publishes_shader_write(read_then_write.Prepare(4, read)));

    TrackedResource repeated_write{buffer, rmw};
    CHECK(publishes_shader_write(repeated_write.Prepare(5, rmw)));
    CHECK(publishes_shader_write(repeated_write.Prepare(6, rmw)));
    TrackedResource only_read{buffer, read};
    CHECK(!only_read.Prepare(7, read));
    CHECK(!only_read.Prepare(7, read));
    CHECK(!only_read.Prepare(8, read));

    const auto other_buffer = vk::Buffer{reinterpret_cast<VkBuffer>(uintptr_t{2})};
    TrackedResource separate{other_buffer, read};
    CHECK(!separate.Prepare(6, read));
    CHECK(separate.command.access == read && !separate.command.has_write);
    queued.clear();
    AppendShaderBufferBarrier(queued, *repeated_write.Prepare(9, rmw));
    AppendShaderBufferBarrier(queued, *separate.Prepare(9, rmw));
    CHECK(queued.size() == 2 && queued[0].buffer != queued[1].buffer);

    // JoinOverlap records its transfer dependencies immediately during prepare
    // and ends in MemoryRead|MemoryWrite. They must not reset the shader union.
    TrackedResource transfer_between{buffer, read};
    transfer_between.Prepare(10, rmw);
    const auto general = Access::eMemoryRead | Access::eMemoryWrite;
    CHECK(UpdateBufferBarrier(transfer_between.access, transfer_between.stage,
                              Access::eTransferRead, Stage::eTransfer, buffer, 0, 4096, true));
    CHECK(UpdateBufferBarrier(transfer_between.access, transfer_between.stage,
                              general, Stage::eAllCommands, buffer, 0, 4096, true));
    const auto after_transfer = transfer_between.Prepare(10, read);
    CHECK(after_transfer && after_transfer->srcAccessMask == general);
    CHECK(after_transfer->dstAccessMask == rmw);
    CHECK(publishes_shader_write(transfer_between.Prepare(11, read)));

    TrackedResource transfer_after{buffer, read};
    transfer_after.Prepare(12, rmw);
    transfer_after.Prepare(12, read);
    CHECK(UpdateBufferBarrier(transfer_after.access, transfer_after.stage,
                              Access::eTransferRead, Stage::eTransfer, buffer, 0, 4096, true));
    CHECK(UpdateBufferBarrier(transfer_after.access, transfer_after.stage,
                              general, Stage::eAllCommands, buffer, 0, 4096, true));
    CHECK(publishes_shader_write(transfer_after.Prepare(13, read)));

    // SlotVector moves Buffer objects; command state is a value, not a retained
    // pointer into the vector. Relocation keeps the union for a later alias.
    TrackedResource before_move{buffer, read};
    before_move.Prepare(14, rmw);
    auto after_move = std::move(before_move);
    CHECK(after_move.Prepare(14, read)->dstAccessMask == rmw);
    CHECK(publishes_shader_write(after_move.Prepare(15, read)));
    auto subrange = queued[0];
    subrange.offset = 128;
    subrange.size = 128;
    AppendShaderBufferBarrier(queued, subrange);
    CHECK(queued.size() == 3); // never coalesce nonidentical ranges
    std::puts("shader command alias regression: PASS (previous policy loses RAW dependency)");
    std::puts("buffer consumer barrier contract: PASS");
    return 0;
}
