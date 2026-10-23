// Lumen shader admission. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics {

enum class SpirvModuleAdmission {
    Allowed, Malformed, FragmentBarycentricUnavailable, BufferInt64AtomicsUnavailable
};

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
                                            bool fragment_barycentric_enabled) noexcept {
    if (words.size() < 5 || words[0] != spv::MagicNumber || words[3] == 0 || words[4] != 0)
        return SpirvModuleAdmission::Malformed;
    bool requires_barycentric = false;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        const auto opcode = words[cursor] & spv::OpCodeMask;
        if (count == 0 || count > words.size() - cursor)
            return SpirvModuleAdmission::Malformed;
        const auto instruction = words.subspan(cursor, count);
        if (opcode == spv::OpCapability) {
            if (count != 2) return SpirvModuleAdmission::Malformed;
            requires_barycentric |= instruction[1] == spv::CapabilityFragmentBarycentricKHR;
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
            }
        } else if (opcode == spv::OpExtension) {
            if (count < 2) return SpirvModuleAdmission::Malformed;
            const auto* text = reinterpret_cast<const char*>(instruction.data() + 1);
            const auto capacity = (count - 1) * sizeof(uint32_t);
            std::size_t length = 0;
            while (length < capacity && text[length] != '\0') ++length;
            if (length == capacity) return SpirvModuleAdmission::Malformed;
            const std::string_view extension(text, length);
            requires_barycentric |= extension == "SPV_KHR_fragment_shader_barycentric" ||
                extension == "SPV_NV_fragment_shader_barycentric";
        }
        cursor += count;
    }
    return requires_barycentric && !fragment_barycentric_enabled
        ? SpirvModuleAdmission::FragmentBarycentricUnavailable : SpirvModuleAdmission::Allowed;
}

} // namespace Libs::Graphics
