// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <vector>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_rect_list.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

const char* RectListReplayUnsupportedReason(const IR::Program& program) {
    const auto& info = program.info;
    if (info.stage != Stage::Vertex || info.l_stage != LogicalStage::Vertex) {
        return "vertex replay requires an ordinary vertex shader";
    }
    if (info.has_fetch_shader) {
        return "fixed vertex attributes require vertex-output capture";
    }
    if (info.uses_lane_id || info.uses_group_quad || info.uses_group_ballot ||
        info.uses_shader_clock || info.shared_types != IR::Type::Void) {
        return "subgroup, clock or shared-memory operations cannot be replayed";
    }
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            using enum IR::Opcode;
            switch (inst.GetOpcode()) {
            case LaneId:
            case WarpId:
            case QuadShuffle:
            case ReadFirstLane:
            case ReadLane:
            case WriteLane:
            case Ballot:
            case BallotFindLsb:
            case GroupAny:
            case Memtime:
            case LoadSharedU16:
            case LoadSharedU32:
            case LoadSharedU64:
                return "subgroup, clock or shared-memory operations cannot be replayed";
            case GetAttribute:
                return "fixed vertex attributes require vertex-output capture";
            case GetAttributeU32:
                if (inst.Arg(0).Attribute() != IR::Attribute::VertexId &&
                    inst.Arg(0).Attribute() != IR::Attribute::InstanceId) {
                    return "vertex replay input builtin is unsupported";
                }
                break;
            // These effects remain private to one guest invocation or its
            // output snapshot. All memory writes, atomics, barriers, debug
            // output and geometry emission are rejected below.
            case Prologue:
            case Epilogue:
            case SetAttribute:
            case ConditionRef:
            case Reference:
            case PhiMove:
                break;
            default:
                if (inst.MayHaveSideEffects()) {
                    return "vertex shader has observable side effects";
                }
                break;
            }
        }
    }
    return nullptr;
}

Sirit::Id EmitRectListVertexWrapper(EmitContext& ctx, Sirit::Id guest) {
    const auto& runtime = ctx.runtime_info.vs_info;
    const Id function_type = ctx.TypeFunction(ctx.void_id);
    const Id main = ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone, function_type);
    ctx.AddLabel();
    const Id host_index = ctx.OpLoad(ctx.U32[1], ctx.vertex_index);
    const Id base_vertex = ctx.OpLoad(ctx.U32[1], ctx.base_vertex);
    const Id local_index = ctx.OpISub(ctx.U32[1], host_index, base_vertex);
    const Id rect_index = ctx.OpUDiv(ctx.U32[1], local_index, ctx.ConstU32(6u));
    const Id source_base = ctx.OpIAdd(ctx.U32[1], base_vertex,
                                     ctx.OpIMul(ctx.U32[1], rect_index, ctx.ConstU32(3u)));
    const Id triangle_vertex = ctx.OpUMod(ctx.U32[1], local_index, ctx.ConstU32(6u));

    struct Output {
        Id pointer;
        u32 components;
        bool flat;
        bool integer;
        std::array<Id, 3> values;
    };
    std::vector<Output> outputs;
    outputs.push_back({ctx.output_position, 4, false, false, {}});
    for (u32 i = 0; i < ctx.output_params.size(); ++i) {
        const auto& attribute = ctx.output_params[i];
        if (attribute.id.value != 0) {
            ASSERT(!attribute.is_array && !attribute.is_integer);
            outputs.push_back({attribute.id, attribute.num_components,
                               (runtime.rect_list_flat_mask & (1u << i)) != 0, false, {}});
        }
    }
    const auto add_distances = [&](Id variable, u32 count) {
        for (u32 i = 0; i < count; ++i) {
            outputs.push_back({ctx.OpAccessChain(ctx.output_f32, variable, ctx.ConstU32(i)),
                               1, false, false, {}});
        }
    };
    add_distances(ctx.clip_distances, ctx.clip_distance_count);
    add_distances(ctx.cull_distances, ctx.cull_distance_count);
    if (ctx.output_point_size.value != 0) outputs.push_back({ctx.output_point_size, 1, true, false, {}});
    if (ctx.output_layer.value != 0) outputs.push_back({ctx.output_layer, 1, true, true, {}});
    if (ctx.output_viewport_index.value != 0) {
        outputs.push_back({ctx.output_viewport_index, 1, true, true, {}});
    }

    for (u32 i = 0; i < 3; ++i) {
        ctx.OpStore(ctx.rect_vertex_index,
                    ctx.OpIAdd(ctx.U32[1], source_base, ctx.ConstU32(i)));
        // Outputs are per invocation. Partially written/undefined components
        // must not inherit another replay's outputs through Private storage.
        for (const auto& output : outputs) {
            const Id type = output.integer ? ctx.U32[output.components] : ctx.F32[output.components];
            ctx.OpStore(output.pointer, ctx.ConstantNull(type));
        }
        ctx.OpFunctionCall(ctx.void_id, guest);
        for (auto& output : outputs) {
            const Id type = output.integer ? ctx.U32[output.components] : ctx.F32[output.components];
            output.values[i] = ctx.OpLoad(type, output.pointer);
        }
    }

    const auto& positions = outputs.front().values;
    std::array<Id, 3> equal_xy;
    for (u32 i = 0; i < 3; ++i) {
        const Id a = ctx.OpVectorShuffle(ctx.F32[2], positions[i], positions[i], 0, 1);
        const Id b = ctx.OpVectorShuffle(ctx.F32[2], positions[(i + 1) % 3],
                                         positions[(i + 1) % 3], 0, 1);
        equal_xy[i] = ctx.OpFOrdEqual(ctx.U1[2], a, b);
    }
    std::array<Id, 3> is_corner;
    std::array<Id, 3> weight;
    for (u32 i = 0; i < 3; ++i) {
        const Id xy = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpCompositeExtract(ctx.U1[1], equal_xy[i], 0),
                                       ctx.OpCompositeExtract(ctx.U1[1], equal_xy[(i + 2) % 3], 1));
        const Id yx = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpCompositeExtract(ctx.U1[1], equal_xy[i], 1),
                                       ctx.OpCompositeExtract(ctx.U1[1], equal_xy[(i + 2) % 3], 0));
        is_corner[i] = ctx.OpLogicalOr(ctx.U1[1], xy, yx);
        weight[i] = ctx.OpSelect(ctx.F32[1], is_corner[i], ctx.Constant(ctx.F32[1], -1.f),
                                 ctx.Constant(ctx.F32[1], 1.f));
    }
    Id corner = ctx.OpSelect(ctx.U32[1], is_corner[2], ctx.ConstU32(2u), ctx.ConstU32(0u));
    corner = ctx.OpSelect(ctx.U32[1], is_corner[1], ctx.ConstU32(1u), corner);
    // The same four control points as the tessellation path, triangulated as
    // the two triangles of its 2x2 grid: [0,1,2], [1,3,2].
    Id grid_index = triangle_vertex;
    grid_index = ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1[1], triangle_vertex, ctx.ConstU32(3u)),
                              ctx.ConstU32(1u), grid_index);
    grid_index = ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1[1], triangle_vertex, ctx.ConstU32(4u)),
                              ctx.ConstU32(3u), grid_index);
    grid_index = ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1[1], triangle_vertex, ctx.ConstU32(5u)),
                              ctx.ConstU32(2u), grid_index);
    const Id fourth = ctx.OpIEqual(ctx.U1[1], grid_index, ctx.ConstU32(3u));
    const Id original = ctx.OpUMod(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], corner, grid_index),
                                   ctx.ConstU32(3u));
    for (const auto& output : outputs) {
        const auto& values = output.values;
        const Id type = output.integer ? ctx.U32[output.components] : ctx.F32[output.components];
        if (output.flat) {
            ctx.OpStore(output.pointer, values[runtime.rect_list_provoking_last ? 2 : 0]);
            continue;
        }
        std::array<Id, 3> weighted;
        for (u32 i = 0; i < 3; ++i) {
            weighted[i] = output.components == 1 ? ctx.OpFMul(type, values[i], weight[i])
                                                 : ctx.OpVectorTimesScalar(type, values[i], weight[i]);
        }
        Id interpolated = ctx.OpFAdd(type, weighted[0], ctx.OpFAdd(type, weighted[1], weighted[2]));
        const Id equal = ctx.OpLogicalAnd(ctx.U1[output.components],
                                          ctx.OpFOrdEqual(ctx.U1[output.components], values[0], values[1]),
                                          ctx.OpFOrdEqual(ctx.U1[output.components], values[0], values[2]));
        // Preserve identical components (especially Z/W) exactly, including
        // large values whose otherwise cancelling sum could overflow.
        interpolated = ctx.OpSelect(type, equal, values[0], interpolated);
        Id selected = ctx.OpSelect(type, ctx.OpIEqual(ctx.U1[1], original, ctx.ConstU32(1u)),
                                    values[1], values[0]);
        selected = ctx.OpSelect(type, ctx.OpIEqual(ctx.U1[1], original, ctx.ConstU32(2u)),
                                values[2], selected);
        ctx.OpStore(output.pointer, ctx.OpSelect(type, fourth, interpolated, selected));
    }
    EmitVertexEpilogue(ctx);
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    return main;
}
} // namespace Shader::Backend::SPIRV
