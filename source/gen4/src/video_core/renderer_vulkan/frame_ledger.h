// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Vulkan::FrameLedger {

inline constexpr std::uint32_t MaxFrames = 256;
inline constexpr std::size_t MaxSerializedBytes = 1024 * 1024;
inline constexpr std::size_t HeaderReserve = 16 * 1024;

constexpr bool SelectorEnabled(const char* value) {
    return value && value[0] == '1' && value[1] == '\0';
}

inline bool Enabled() {
#ifdef __ANDROID__
    static const bool enabled = SelectorEnabled(std::getenv("LUMEN_GEN4_FRAME_LEDGER"));
    return enabled;
#else
    return false;
#endif
}

enum class CommandKind : std::uint32_t {
    Draw, DrawIndexed, DrawIndirect, DrawIndexedIndirect, Dispatch, DispatchIndirect
};
enum class BindingKind : std::uint32_t {
    Vertex, Index, ShaderUniform, ShaderStorage, IndirectArguments, IndirectCount
};

// Values are metadata already used to record commands, never guest payload reads.
// writable classifies a shader descriptor; it does not prove a GPU write happened.
struct BufferBinding {
    BindingKind kind{};
    std::uint32_t stage{}, slot{}, writable{};
    std::uint64_t shader{}, guest_address{}, guest_size{}, buffer{}, offset{}, range{};
    std::uint32_t stride{}, format{};
    std::uint32_t descriptor_binding{};

    auto Values() const {
        return std::array<std::uint64_t, 13>{
            static_cast<std::uint64_t>(kind), stage, slot, writable, shader, guest_address,
            guest_size, buffer, offset, range, stride, format, descriptor_binding};
    }
    bool operator==(const BufferBinding&) const = default;
};

struct Command {
    CommandKind kind{};
    std::uint64_t ordinal{}, pipeline{}, vs{}, ps{}, cs{}, tick{};
    std::uint32_t primitive{}, count{}, instances{}, first_vertex{}, first_instance{}, first_index{};
    std::uint32_t max_count{}, indirect_stride{};
    std::uint64_t args_address{}, count_address{};
    std::uint32_t group_x{}, group_y{}, group_z{};
    std::uint32_t cull{}, front_face{}, depth_test{}, depth_write{}, depth_compare{}, color_write{};
    std::uint32_t target_mask{}, width{}, height{};
    std::array<std::uint64_t, 8> targets{};
    std::uint32_t viewport_count{}, scissor_count{}, rasterizer_discard{};
    std::array<std::uint32_t, 6> viewport0_bits{};
    std::array<std::uint32_t, 4> scissor0{};
    std::uint64_t depth_target{}, depth_write_target{};
    std::uint32_t binding_first{}, binding_count{};

    auto Values() const {
        return std::array<std::uint64_t, 54>{
            ordinal, static_cast<std::uint64_t>(kind), pipeline, vs, ps, cs, tick, primitive,
            count, instances, first_vertex, first_instance, first_index, max_count, indirect_stride,
            args_address, count_address, group_x, group_y, group_z, cull, front_face, depth_test,
            depth_write, depth_compare, color_write, target_mask, width, height,
            targets[0], targets[1], targets[2], targets[3], targets[4], targets[5], targets[6], targets[7],
            viewport_count, scissor_count, rasterizer_discard,
            viewport0_bits[0], viewport0_bits[1], viewport0_bits[2], viewport0_bits[3],
            viewport0_bits[4], viewport0_bits[5],
            scissor0[0], scissor0[1], scissor0[2], scissor0[3],
            depth_target, depth_write_target,
            binding_first, binding_count};
    }
};

struct Snapshot {
    std::uint64_t burst_id{}, scanout{}, ready_tick{}, command_count_seen{};
    std::uint32_t frame{}, command_overflow{}, binding_overflow{}, serialized_overflow{};
    bool complete{true};
    std::vector<Command> commands;
    std::vector<BufferBinding> bindings;
    std::vector<std::uint32_t> binding_refs;
};

struct Limits {
    std::uint32_t frames{MaxFrames}, commands{4096}, bindings{8192}, binding_refs{65536};
    std::uint32_t pending_bindings{128};
    std::size_t data_bytes{MaxSerializedBytes - HeaderReserve};
};

inline std::size_t Digits(std::uint64_t value) {
    std::size_t count = 1;
    while (value >= 10) { value /= 10; ++count; }
    return count;
}
template <typename Values>
inline std::size_t RowBytes(const Values& values) {
    std::size_t result = 2; // brackets
    for (const auto value : values) result += Digits(value) + 1;
    return result; // includes the separator after this row (conservative for last row)
}

struct BindingHash {
    std::size_t operator()(const BufferBinding& binding) const {
        std::uint64_t hash = 0xcbf29ce484222325ULL;
        for (const auto value : binding.Values()) {
            hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        }
        return static_cast<std::size_t>(hash);
    }
};

// Arm is called by the input thread. All other mutations belong to the GPU
// command thread. Arm only changes Idle -> Align and never touches recorded data.
class Collector {
public:
    explicit Collector(bool enabled_ = true, Limits limits_ = {})
        : enabled{enabled_}, limits{limits_} {}

    bool Arm() {
        if (!enabled) return false;
        State expected = State::Idle;
        return state.compare_exchange_strong(expected, State::Align,
                                              std::memory_order_acq_rel);
    }
    bool Active() const { return state.load(std::memory_order_acquire) == State::Capturing; }

    void BeginCommand() {
        if (!Active()) return;
        pending.clear();
        pending_overflow = 0;
        pending_pipeline = {};
        command_open = true;
    }
    void SetPipeline(std::uint64_t pipeline, std::uint64_t vs, std::uint64_t ps, std::uint64_t cs) {
        if (!Active() || !command_open) return;
        pending_pipeline = {pipeline, vs, ps, cs};
    }
    void AddBufferBinding(const BufferBinding& binding) {
        if (!Active() || !command_open) return;
        if (pending.size() == limits.pending_bindings) { ++pending_overflow; return; }
        pending.push_back(binding);
    }
    void Emit(Command command) {
        if (!Active() || !command_open) return;
        command_open = false;
        command.ordinal = ++current.command_count_seen;
        if (pending_overflow || current.commands.size() == limits.commands) {
            current.binding_overflow += pending_overflow;
            DropCommand();
            return;
        }
        command.pipeline = pending_pipeline[0];
        command.vs = pending_pipeline[1];
        command.ps = pending_pipeline[2];
        command.cs = pending_pipeline[3];
        command.binding_first = static_cast<std::uint32_t>(current.binding_refs.size());
        command.binding_count = static_cast<std::uint32_t>(pending.size());
        new_bindings.clear();
        new_refs.clear();
        std::size_t additional = RowBytes(command.Values());
        for (const auto& binding : pending) {
            auto found = binding_ids.find(binding);
            std::uint32_t id{};
            if (found != binding_ids.end()) {
                id = found->second;
            } else {
                std::size_t i = 0;
                while (i < new_bindings.size() && new_bindings[i] != binding) ++i;
                id = static_cast<std::uint32_t>(current.bindings.size() + i);
                if (i == new_bindings.size()) {
                    new_bindings.push_back(binding);
                    additional += RowBytes(binding.Values());
                }
            }
            new_refs.push_back(id);
            additional += Digits(id) + 1;
        }
        if (current.bindings.size() + new_bindings.size() > limits.bindings ||
            current.binding_refs.size() + new_refs.size() > limits.binding_refs) {
            ++current.binding_overflow;
            DropCommand();
            return;
        }
        if (additional > limits.data_bytes || data_bytes > limits.data_bytes - additional) {
            ++current.serialized_overflow;
            DropCommand();
            return;
        }
        for (const auto& binding : new_bindings) {
            const auto id = static_cast<std::uint32_t>(current.bindings.size());
            binding_ids.emplace(binding, id);
            current.bindings.push_back(binding);
        }
        current.binding_refs.insert(current.binding_refs.end(), new_refs.begin(), new_refs.end());
        current.commands.push_back(command);
        data_bytes += additional;
    }

    // Call exactly once per successful PrepareFrame, including dropped images.
    // The first boundary after Arm discards the partial interval and starts one
    // complete interval. A failed/retried PrepareFrame must not call this method.
    std::shared_ptr<const Snapshot> TakeFrame(std::uint64_t scanout, std::uint64_t ready_tick) {
        const auto status = state.load(std::memory_order_acquire);
        if (status == State::Idle) return {};
        if (status == State::Align) {
            ++burst_id;
            completed = 0;
            ResetInterval();
            state.store(State::Capturing, std::memory_order_release);
            return {};
        }
        current.burst_id = burst_id;
        current.frame = ++completed;
        current.scanout = scanout;
        current.ready_tick = ready_tick;
        auto result = std::make_shared<const Snapshot>(std::move(current));
        ResetInterval();
        if (completed == limits.frames) state.store(State::Idle, std::memory_order_release);
        return result;
    }

private:
    enum class State : unsigned { Idle, Align, Capturing };
    void DropCommand() { ++current.command_overflow; current.complete = false; }
    void ResetInterval() {
        current = {};
        data_bytes = 0;
        pending.clear();
        pending_overflow = 0;
        command_open = false;
        binding_ids.clear();
    }
    const bool enabled;
    const Limits limits;
    std::atomic<State> state{State::Idle};
    std::uint64_t burst_id{};
    std::uint32_t completed{}, pending_overflow{};
    bool command_open{};
    std::size_t data_bytes{};
    Snapshot current;
    std::array<std::uint64_t, 4> pending_pipeline{};
    std::vector<BufferBinding> pending, new_bindings;
    std::vector<std::uint32_t> new_refs;
    std::unordered_map<BufferBinding, std::uint32_t, BindingHash> binding_ids;
};

inline void AppendNumber(std::string& out, std::uint64_t number) {
    char text[24];
    const auto result = std::to_chars(text, text + sizeof(text), number);
    out.append(text, result.ptr);
}
template <typename Values>
inline void AppendRow(std::string& out, const Values& values) {
    out += '[';
    bool first = true;
    for (const auto value : values) {
        if (!first) out += ',';
        first = false;
        AppendNumber(out, value);
    }
    out += ']';
}

inline std::string Serialize(const Snapshot& snapshot) {
    std::string out;
    out.reserve(4096);
    out = "{\"schema\":\"lumen.frame_ledger.v1\",\"burst_id\":";
    AppendNumber(out, snapshot.burst_id);
    out += ",\"frame\":"; AppendNumber(out, snapshot.frame);
    out += ",\"scanout\":"; AppendNumber(out, snapshot.scanout);
    out += ",\"ready_tick\":"; AppendNumber(out, snapshot.ready_tick);
    out += snapshot.complete ? ",\"complete\":true" : ",\"complete\":false";
    out += ",\"command_count_seen\":"; AppendNumber(out, snapshot.command_count_seen);
    out += ",\"command_overflow\":"; AppendNumber(out, snapshot.command_overflow);
    out += ",\"binding_overflow\":"; AppendNumber(out, snapshot.binding_overflow);
    out += ",\"serialized_overflow\":"; AppendNumber(out, snapshot.serialized_overflow);
    out += ",\"indirect_counts_known\":false,\"pixel_causality_proven\":false";
    out += ",\"scope\":\"emitted_draw_dispatch_only_no_transfer_hle_or_filtered_outcomes\",\"dynamic_viewport_scissor_scope\":\"first_entry_and_total_count\"";
    out += ",\"buffer_range_scope\":\"shader=descriptor_range;vertex=guest_declared_range;index=cache_requested_range;indirect=argument_acquisition_range;not_proof_of_consumed_bytes\"";
    out += ",\"binding_slot_scope\":\"shader=stage_local_resource_index;vertex=native_binding_index;descriptor_binding=shader_Vulkan_dstBinding\"";
    out += ",\"command_kinds\":[\"draw\",\"draw_indexed\",\"draw_indirect\",\"draw_indexed_indirect\",\"dispatch\",\"dispatch_indirect\"]";
    out += ",\"binding_kinds\":[\"vertex\",\"index\",\"shader_uniform\",\"shader_storage\",\"indirect_arguments\",\"indirect_count\"]";
    out += ",\"command_columns\":[\"ordinal\",\"kind\",\"pipeline\",\"vs\",\"ps\",\"cs\",\"tick\",\"primitive\",\"count\",\"instances\",\"first_vertex_u32_or_base_vertex_i32_bits\",\"first_instance\",\"first_index\",\"max_count\",\"indirect_stride\",\"args_address\",\"count_address\",\"group_x\",\"group_y\",\"group_z\",\"cull\",\"front_face\",\"depth_test\",\"depth_write\",\"depth_compare\",\"color_write_packed\",\"target_mask\",\"width\",\"height\",\"target0\",\"target1\",\"target2\",\"target3\",\"target4\",\"target5\",\"target6\",\"target7\",\"viewport_count\",\"scissor_count\",\"rasterizer_discard\",\"viewport0_x_f32_bits\",\"viewport0_y_f32_bits\",\"viewport0_width_f32_bits\",\"viewport0_height_f32_bits\",\"viewport0_min_depth_f32_bits\",\"viewport0_max_depth_f32_bits\",\"scissor0_x_i32_bits\",\"scissor0_y_i32_bits\",\"scissor0_width\",\"scissor0_height\",\"depth_target\",\"depth_write_target\",\"binding_first\",\"binding_count\"]";
    out += ",\"binding_columns\":[\"kind\",\"stage\",\"slot\",\"writable\",\"shader\",\"guest_address\",\"guest_size\",\"buffer\",\"offset\",\"range\",\"stride\",\"format\",\"descriptor_binding\"]";
    out += ",\"commands\":[";
    bool first = true;
    for (const auto& command : snapshot.commands) {
        if (!first) out += ',';
        first = false;
        AppendRow(out, command.Values());
    }
    out += "],\"bindings\":[";
    first = true;
    for (const auto& binding : snapshot.bindings) {
        if (!first) out += ',';
        first = false;
        AppendRow(out, binding.Values());
    }
    out += "],\"binding_refs\":";
    AppendRow(out, snapshot.binding_refs);
    out += "}\n";
    if (out.size() > MaxSerializedBytes) {
        // Defensive bound for externally constructed snapshots. Collector's
        // conservative row accounting reserves 16 KiB for the fixed schema.
        out = "{\"schema\":\"lumen.frame_ledger.v1\",\"burst_id\":";
        AppendNumber(out, snapshot.burst_id);
        out += ",\"frame\":"; AppendNumber(out, snapshot.frame);
        out += ",\"complete\":false,\"serialized_overflow\":1,\"reason\":\"serialization_limit\"}\n";
    }
    return out;
}

inline Collector& Session() { static Collector collector{Enabled()}; return collector; }
inline bool Active() { return Enabled() && Session().Active(); }
inline bool Arm() { return Enabled() && Session().Arm(); }
inline void BeginCommand() { if (Active()) Session().BeginCommand(); }
inline void SetPipeline(std::uint64_t pipeline, std::uint64_t vs, std::uint64_t ps,
                        std::uint64_t cs = 0) {
    if (Active()) Session().SetPipeline(pipeline, vs, ps, cs);
}
inline void AddBufferBinding(const BufferBinding& binding) {
    if (Active()) Session().AddBufferBinding(binding);
}
inline void Emit(Command command) { if (Active()) Session().Emit(command); }
inline std::shared_ptr<const Snapshot> TakeFrame(std::uint64_t scanout, std::uint64_t ready_tick) {
    return Enabled() ? Session().TakeFrame(scanout, ready_tick) : nullptr;
}

} // namespace Vulkan::FrameLedger
