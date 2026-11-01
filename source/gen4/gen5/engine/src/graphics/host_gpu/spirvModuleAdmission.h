// Lumen shader admission. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics {

enum class SpirvModuleAdmission {
    Allowed, Malformed, FragmentBarycentricUnavailable, BufferInt64AtomicsUnavailable,
    CullDistanceUnavailable, MultiViewportUnavailable, OutputLayerUnavailable,
    FragmentQuadControlUnavailable
};

struct SpirvOptionalFeatures {
    bool cull_distance = true;
    bool multi_viewport = true;
    bool output_layer = true;
    bool fragment_quad_control = false;
};

namespace SpirvAdmissionDetail {
inline bool LayerOutputStage(uint32_t model) noexcept {
    return model == spv::ExecutionModelVertex || model == spv::ExecutionModelTessellationEvaluation ||
        model == spv::ExecutionModelMeshEXT || model == spv::ExecutionModelMeshNV;
}

// Called only after instruction framing and operand lengths have been checked.
// Follow interface variables and decoration groups without treating fragment
// Layer inputs or geometry Layer outputs as vertex-layer feature consumers.
inline bool HasLayerOutput(std::span<const uint32_t> words) noexcept {
    const auto direct_layer = [&](uint32_t id, bool member) {
        for (std::size_t at = 5; at < words.size(); at += words[at] >> spv::WordCountShift) {
            const auto op = words[at] & spv::OpCodeMask;
            if (op != spv::OpDecorate && op != spv::OpDecorateId && op != spv::OpMemberDecorate)
                continue;
            if (words[at + 1] != id) continue;
            if (member && op == spv::OpMemberDecorate &&
                words[at + 3] == spv::DecorationBuiltIn && words[at + 4] == spv::BuiltInLayer)
                return true;
            if (!member && (op == spv::OpDecorate || op == spv::OpDecorateId) &&
                words[at + 2] == spv::DecorationBuiltIn && words[at + 3] == spv::BuiltInLayer)
                return true;
        }
        return false;
    };
    const auto decorated_layer = [&](uint32_t id, bool member) {
        if (direct_layer(id, member)) return true;
        for (std::size_t at = 5; at < words.size(); at += words[at] >> spv::WordCountShift) {
            const auto count = words[at] >> spv::WordCountShift;
            const auto op = words[at] & spv::OpCodeMask;
            if (op != (member ? spv::OpGroupMemberDecorate : spv::OpGroupDecorate)) continue;
            if (!direct_layer(words[at + 1], false)) continue;
            for (std::size_t operand = 2; operand < count; operand += member ? 2 : 1)
                if (words[at + operand] == id) return true;
        }
        return false;
    };
    const auto output_layer = [&](uint32_t id) {
        uint32_t type = 0;
        for (std::size_t at = 5; at < words.size(); at += words[at] >> spv::WordCountShift) {
            if ((words[at] & spv::OpCodeMask) == spv::OpVariable && words[at + 2] == id &&
                words[at + 3] == spv::StorageClassOutput) type = words[at + 1];
        }
        if (type == 0) return false;
        if (decorated_layer(id, false)) return true;
        // A Layer member can be inside an output block or an array of blocks.
        for (std::size_t remaining = words.size(); remaining != 0; --remaining) {
            uint32_t next = 0;
            for (std::size_t at = 5; at < words.size(); at += words[at] >> spv::WordCountShift) {
                const auto op = words[at] & spv::OpCodeMask;
                if ((op == spv::OpTypePointer || op == spv::OpTypeArray ||
                     op == spv::OpTypeRuntimeArray || op == spv::OpTypeStruct) &&
                    words[at + 1] == type) {
                    if (op == spv::OpTypeStruct) return decorated_layer(type, true);
                    next = words[at + (op == spv::OpTypePointer ? 3 : 2)];
                    break;
                }
            }
            if (next == 0 || next == type) return false;
            type = next;
        }
        return false;
    };
    for (std::size_t at = 5; at < words.size(); at += words[at] >> spv::WordCountShift) {
        const auto count = words[at] >> spv::WordCountShift;
        if ((words[at] & spv::OpCodeMask) != spv::OpEntryPoint ||
            !LayerOutputStage(words[at + 1])) continue;
        const auto* name = reinterpret_cast<const char*>(words.data() + at + 3);
        std::size_t length = 0;
        while (name[length] != '\0') ++length;
        const auto first_interface = 3 + (length + 1 + 3) / 4;
        for (std::size_t operand = first_interface; operand < count; ++operand)
            if (output_layer(words[at + operand])) return true;
    }
    return false;
}
} // namespace SpirvAdmissionDetail

// Guest IR records both explicit buffer atomics and coherent paired buffer
// loads, which become 64-bit OpAtomicLoad. Compute LDS can also be lowered to
// a storage buffer; native Workgroup and image atomics have separate features.
inline SpirvModuleAdmission AdmitShaderBufferInt64Atomics(bool buffer_atomics_enabled,
                                                         bool uses_buffer_atomics,
                                                         bool uses_shared_atomics,
                                                         bool lds_uses_storage_buffer) noexcept {
    const bool required = uses_buffer_atomics ||
        (uses_shared_atomics && lds_uses_storage_buffer);
    return required && !buffer_atomics_enabled
        ? SpirvModuleAdmission::BufferInt64AtomicsUnavailable : SpirvModuleAdmission::Allowed;
}

// Inspect the final words for every native shader-module path. This bounded
// requirements scan does not replace SPIR-V validation or rewrite guest semantics.
inline SpirvModuleAdmission AdmitSpirvModule(std::span<const uint32_t> words,
                                            bool fragment_barycentric_enabled,
                                            SpirvOptionalFeatures features = {}) noexcept {
    if (words.size() < 5 || words[0] != spv::MagicNumber || words[3] == 0 || words[4] != 0)
        return SpirvModuleAdmission::Malformed;
    bool requires_barycentric = false;
    bool requires_cull_distance = false;
    bool requires_multi_viewport = false;
    bool requires_output_layer = false;
    bool requires_quad_control = false;
    bool has_layer_builtin = false;
    bool has_layer_output_stage = false;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        const auto opcode = words[cursor] & spv::OpCodeMask;
        if (count == 0 || count > words.size() - cursor)
            return SpirvModuleAdmission::Malformed;
        const auto instruction = words.subspan(cursor, count);
        if (opcode == spv::OpCapability) {
            if (count != 2) return SpirvModuleAdmission::Malformed;
            requires_quad_control |= instruction[1] == spv::CapabilityQuadControlKHR;
            requires_barycentric |= instruction[1] == spv::CapabilityFragmentBarycentricKHR;
            requires_cull_distance |= instruction[1] == spv::CapabilityCullDistance;
            requires_multi_viewport |= instruction[1] == spv::CapabilityMultiViewport ||
                instruction[1] == spv::CapabilityShaderViewportIndex;
            requires_output_layer |= instruction[1] == spv::CapabilityShaderLayer ||
                instruction[1] == spv::CapabilityShaderViewportIndexLayerEXT;
        } else if (opcode == spv::OpExecutionMode || opcode == spv::OpExecutionModeId) {
            if (count < 3) return SpirvModuleAdmission::Malformed;
            requires_quad_control |= instruction[2] == spv::ExecutionModeQuadDerivativesKHR ||
                instruction[2] == spv::ExecutionModeRequireFullQuadsKHR ||
                instruction[2] == spv::ExecutionModeMaximallyReconvergesKHR;
        } else if (opcode == spv::OpGroupNonUniformQuadAnyKHR ||
                   opcode == spv::OpGroupNonUniformQuadAllKHR) {
            if (count != 4) return SpirvModuleAdmission::Malformed;
            requires_quad_control = true;
        } else if (opcode == spv::OpDecorate || opcode == spv::OpMemberDecorate ||
                   opcode == spv::OpDecorateId) {
            const std::size_t decoration = opcode == spv::OpMemberDecorate ? 3 : 2;
            if (count <= decoration) return SpirvModuleAdmission::Malformed;
            // Decorations of a group are scanned here too, before its applications.
            requires_barycentric |= instruction[decoration] == spv::DecorationPerVertexKHR;
            if (instruction[decoration] == spv::DecorationBuiltIn) {
                if (count != decoration + 2) return SpirvModuleAdmission::Malformed;
                requires_barycentric |= instruction[decoration + 1] == spv::BuiltInBaryCoordKHR ||
                    instruction[decoration + 1] == spv::BuiltInBaryCoordNoPerspKHR;
                has_layer_builtin |= instruction[decoration + 1] == spv::BuiltInLayer;
                // Glslang's default gl_PerVertex block contains unused cull
                // members even in the full-screen triangle. Actual use needs
                // the corresponding OpCapability, checked above; rejecting an
                // inactive member would also reject ordinary blit shaders.
            }
        } else if (opcode == spv::OpEntryPoint) {
            if (count < 4) return SpirvModuleAdmission::Malformed;
            const auto* name = reinterpret_cast<const char*>(instruction.data() + 3);
            std::size_t length = 0;
            while (length < (count - 3) * sizeof(uint32_t) && name[length] != '\0') ++length;
            if (length == (count - 3) * sizeof(uint32_t)) return SpirvModuleAdmission::Malformed;
            has_layer_output_stage |= SpirvAdmissionDetail::LayerOutputStage(instruction[1]);
        } else if (opcode == spv::OpVariable || opcode == spv::OpTypePointer ||
                   opcode == spv::OpTypeArray) {
            if (count < 4) return SpirvModuleAdmission::Malformed;
        } else if (opcode == spv::OpTypeRuntimeArray || opcode == spv::OpGroupDecorate) {
            if (count < 3) return SpirvModuleAdmission::Malformed;
        } else if (opcode == spv::OpTypeStruct) {
            if (count < 2) return SpirvModuleAdmission::Malformed;
        } else if (opcode == spv::OpGroupMemberDecorate) {
            if (count < 4 || count % 2 != 0) return SpirvModuleAdmission::Malformed;
        } else if (opcode == spv::OpExtension) {
            if (count < 2) return SpirvModuleAdmission::Malformed;
            const auto* text = reinterpret_cast<const char*>(instruction.data() + 1);
            const auto capacity = (count - 1) * sizeof(uint32_t);
            std::size_t length = 0;
            while (length < capacity && text[length] != '\0') ++length;
            if (length == capacity) return SpirvModuleAdmission::Malformed;
            const std::string_view extension(text, length);
            requires_quad_control |= extension == "SPV_KHR_quad_control" ||
                extension == "SPV_KHR_maximal_reconvergence";
            requires_barycentric |= extension == "SPV_KHR_fragment_shader_barycentric" ||
                extension == "SPV_NV_fragment_shader_barycentric";
        }
        cursor += count;
    }
    if (requires_barycentric && !fragment_barycentric_enabled)
        return SpirvModuleAdmission::FragmentBarycentricUnavailable;
    if (requires_cull_distance && !features.cull_distance)
        return SpirvModuleAdmission::CullDistanceUnavailable;
    if (requires_multi_viewport && !features.multi_viewport)
        return SpirvModuleAdmission::MultiViewportUnavailable;
    if (!features.output_layer && (requires_output_layer ||
        (has_layer_builtin && has_layer_output_stage && SpirvAdmissionDetail::HasLayerOutput(words))))
        return SpirvModuleAdmission::OutputLayerUnavailable;
    if (requires_quad_control && !features.fragment_quad_control)
        return SpirvModuleAdmission::FragmentQuadControlUnavailable;
    return SpirvModuleAdmission::Allowed;
}

} // namespace Libs::Graphics
