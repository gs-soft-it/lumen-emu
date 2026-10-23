// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/frame_ledger.h"

#include <cstdio>
#include <limits>
#include <string_view>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #condition); return 1; \
} } while (false)

using namespace Vulkan::FrameLedger;

static void EmitDraw(Collector& collector, std::uint32_t count, const BufferBinding& binding) {
    collector.BeginCommand();
    collector.SetPipeline(0x1234, 0x2345, 0x3456, 0);
    collector.AddBufferBinding(binding);
    Command command{.kind = CommandKind::DrawIndexed};
    command.tick = 77;
    command.count = count;
    command.instances = 2;
    command.first_vertex = 0xfffffffcu; // signed base vertex -4, retained as raw bits
    collector.Emit(command);
}

int main(int argc, char** argv) {
    CHECK(!SelectorEnabled(nullptr));
    CHECK(SelectorEnabled("1"));
    for (const char* value : {"", "0", "01", "10", "true", " 1", "1 "}) {
        CHECK(!SelectorEnabled(value));
    }
    Collector disabled{false};
    CHECK(!disabled.Arm());
    CHECK(!disabled.TakeFrame(1, 2));

    const BufferBinding vertex{.kind = BindingKind::Vertex, .slot = 2,
        .guest_address = 0x12345000, .guest_size = 4096,
        .buffer = 0xb400006e701d3f00ULL, .offset = 512, .range = 4096, .stride = 32};
    Limits short_burst{};
    short_burst.frames = 2;
    Collector collector{true, short_burst};
    EmitDraw(collector, 99, vertex); // inactive commands must never leak into a capture
    CHECK(collector.Arm());
    CHECK(!collector.Active());
    CHECK(!collector.Arm());
    EmitDraw(collector, 98, vertex); // partial interval between Arm and first boundary
    CHECK(!collector.TakeFrame(0x1000, 10));
    CHECK(collector.Active());
    CHECK(!collector.Arm());
    EmitDraw(collector, 123, vertex);
    EmitDraw(collector, 456, vertex);
    auto first = collector.TakeFrame(0x2000, 20);
    CHECK(first && first->burst_id == 1 && first->frame == 1);
    CHECK(first->scanout == 0x2000 && first->ready_tick == 20 && first->complete);
    CHECK(first->command_count_seen == 2 && first->commands.size() == 2);
    CHECK(first->commands[0].count == 123 && first->commands[1].count == 456);
    CHECK(first->commands[0].instances == 2 && first->commands[0].first_vertex == 0xfffffffcu);
    CHECK(first->commands[0].pipeline == 0x1234 && first->commands[0].vs == 0x2345);
    CHECK(first->bindings.size() == 1 && first->binding_refs.size() == 2);
    CHECK(first->binding_refs[0] == 0 && first->binding_refs[1] == 0);
    CHECK(first->bindings[0].buffer == vertex.buffer);

    // Presenter must drain a successful frame even when its image slot is full.
    // Discarding this snapshot does not merge its draws into the next interval.
    EmitDraw(collector, 789, vertex);
    auto dropped_image = collector.TakeFrame(0x3000, 30);
    CHECK(dropped_image && dropped_image->frame == 2 && dropped_image->commands.size() == 1);
    CHECK(!collector.Active() && !collector.TakeFrame(0x4000, 40));
    CHECK(first->commands.size() == 2 && first->commands[0].count == 123);
    CHECK(collector.Arm());
    CHECK(!collector.Active() && !collector.TakeFrame(0x5000, 50));
    EmitDraw(collector, 7, vertex);
    auto rearmed = collector.TakeFrame(0x6000, 60);
    CHECK(rearmed && rearmed->burst_id == 2 && rearmed->frame == 1);
    CHECK(rearmed->commands.size() == 1 && rearmed->commands[0].count == 7);
    CHECK(first->burst_id == 1 && first->scanout == 0x2000); // delayed callback identity immutable

    const std::string json = Serialize(*first);
    CHECK(json.size() < MaxSerializedBytes);
    CHECK(json.find("\"complete\":true") != std::string::npos);
    CHECK(json.find("\"indirect_counts_known\":false") != std::string::npos);
    CHECK(json.find(std::to_string(vertex.buffer)) != std::string::npos);
    if (argc == 2 && std::string_view{argv[1]} == "--json") {
        std::fwrite(json.data(), 1, json.size(), stdout);
        return 0;
    }

    Limits command_limit{};
    command_limit.commands = 1;
    Collector bounded{true, command_limit};
    CHECK(bounded.Arm() && !bounded.TakeFrame(1, 1));
    EmitDraw(bounded, 1, vertex);
    EmitDraw(bounded, 2, vertex);
    auto limited = bounded.TakeFrame(2, 2);
    CHECK(!limited->complete && limited->commands.size() == 1);
    CHECK(limited->command_count_seen == 2 && limited->command_overflow == 1);
    CHECK(Serialize(*limited).find("\"complete\":false") != std::string::npos);

    Limits pending_limit{};
    pending_limit.pending_bindings = 1;
    Collector pending{true, pending_limit};
    CHECK(pending.Arm() && !pending.TakeFrame(1, 1));
    pending.BeginCommand();
    pending.AddBufferBinding(vertex);
    pending.AddBufferBinding(vertex);
    pending.Emit({});
    auto pending_drop = pending.TakeFrame(2, 2);
    CHECK(!pending_drop->complete && pending_drop->binding_overflow == 1);
    CHECK(pending_drop->commands.empty() && pending_drop->bindings.empty());

    Limits binding_limit{};
    binding_limit.bindings = 1;
    Collector bindings{true, binding_limit};
    CHECK(bindings.Arm() && !bindings.TakeFrame(1, 1));
    EmitDraw(bindings, 1, vertex);
    auto another = vertex;
    another.buffer += 16;
    EmitDraw(bindings, 2, another);
    auto binding_drop = bindings.TakeFrame(2, 2);
    CHECK(!binding_drop->complete && binding_drop->binding_overflow == 1);
    CHECK(binding_drop->bindings.size() == 1 && binding_drop->commands.size() == 1);

    Limits byte_limit{};
    byte_limit.data_bytes = 1;
    Collector bytes{true, byte_limit};
    CHECK(bytes.Arm() && !bytes.TakeFrame(1, 1));
    EmitDraw(bytes, 1, vertex);
    auto bytes_drop = bytes.TakeFrame(2, 2);
    CHECK(!bytes_drop->complete && bytes_drop->serialized_overflow == 1);
    CHECK(bytes_drop->commands.empty() && bytes_drop->bindings.empty());

    Collector full;
    CHECK(full.Arm() && !full.TakeFrame(1, 1));
    for (std::uint32_t i = 0; i < 5000; ++i) {
        auto unique = vertex;
        unique.guest_address += i * 4096ULL;
        unique.buffer = std::numeric_limits<std::uint64_t>::max() - i;
        EmitDraw(full, i + 1, unique);
    }
    auto full_snapshot = full.TakeFrame(2, 2);
    CHECK(!full_snapshot->complete && full_snapshot->command_count_seen == 5000);
    CHECK(full_snapshot->command_overflow != 0);
    CHECK(Serialize(*full_snapshot).size() <= MaxSerializedBytes);
    for (std::uint32_t frame = 2; frame <= MaxFrames; ++frame) {
        auto snapshot = full.TakeFrame(frame, frame);
        CHECK(snapshot && snapshot->frame == frame && snapshot->commands.empty());
    }
    CHECK(!full.Active() && full.Arm());
    std::puts("frame ledger boundary, immutable snapshots, dedup, caps and rearm: PASS");
    return 0;
}
