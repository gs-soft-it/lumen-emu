// Lumen shader admission, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics {
enum class SpirvModuleAdmission {
    Allowed, Malformed, FragmentBarycentricUnavailable,
    SampledImageArrayNonUniformIndexingUnavailable, ImageInt64AtomicsUnavailable
};

// This is a bounded requirements scan, not a replacement for SPIR-V validation.
// Inspect the final words, including built-in, typed, and cached shader paths.
inline SpirvModuleAdmission AdmitSpirvModule(std::span<const uint32_t> words,
                                            bool fragment_barycentric_enabled,
                                            bool sampled_image_array_nonuniform_enabled = false,
                                            bool image_int64_atomics_enabled = false) noexcept {
    if (words.size() < 5 || words[0] != spv::MagicNumber || words[3] == 0 || words[4] != 0)
        return SpirvModuleAdmission::Malformed;
    bool requires_barycentric = false;
    bool requires_sampled_image_array_nonuniform = false;
    bool requires_image_int64_atomics = false;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        const auto opcode = words[cursor] & spv::OpCodeMask;
        if (count == 0 || count > words.size() - cursor)
            return SpirvModuleAdmission::Malformed;
        const auto instruction = words.subspan(cursor, count);
        if (opcode == spv::OpCapability) {
            if (count != 2) return SpirvModuleAdmission::Malformed;
            requires_barycentric |= instruction[1] == spv::CapabilityFragmentBarycentricKHR;
            requires_sampled_image_array_nonuniform |=
                instruction[1] == spv::CapabilitySampledImageArrayNonUniformIndexing;
            requires_image_int64_atomics |= instruction[1] == spv::CapabilityInt64ImageEXT;
        } else if (opcode == spv::OpDecorate || opcode == spv::OpMemberDecorate
                   || opcode == spv::OpDecorateId) {
            const std::size_t decoration = opcode == spv::OpMemberDecorate ? 3 : 2;
            if (count <= decoration) return SpirvModuleAdmission::Malformed;
            requires_barycentric |= instruction[decoration] == spv::DecorationPerVertexKHR;
            if (instruction[decoration] == spv::DecorationBuiltIn) {
                if (count != decoration + 2) return SpirvModuleAdmission::Malformed;
                requires_barycentric |= instruction[decoration + 1] == spv::BuiltInBaryCoordKHR
                    || instruction[decoration + 1] == spv::BuiltInBaryCoordNoPerspKHR;
            }
        } else if (opcode == spv::OpExtension) {
            if (count < 2) return SpirvModuleAdmission::Malformed;
            const auto* text = reinterpret_cast<const char*>(instruction.data() + 1);
            const auto capacity = (count - 1) * sizeof(uint32_t);
            std::size_t length = 0;
            while (length < capacity && text[length] != '\0') ++length;
            if (length == capacity) return SpirvModuleAdmission::Malformed;
            const std::string_view extension(text, length);
            requires_barycentric |= extension == "SPV_KHR_fragment_shader_barycentric"
                || extension == "SPV_NV_fragment_shader_barycentric";
        }
        cursor += count;
    }
    if (requires_barycentric && !fragment_barycentric_enabled)
        return SpirvModuleAdmission::FragmentBarycentricUnavailable;
    if (requires_sampled_image_array_nonuniform && !sampled_image_array_nonuniform_enabled)
        return SpirvModuleAdmission::SampledImageArrayNonUniformIndexingUnavailable;
    if (requires_image_int64_atomics && !image_int64_atomics_enabled)
        return SpirvModuleAdmission::ImageInt64AtomicsUnavailable;
    return SpirvModuleAdmission::Allowed;
}
} // namespace Libs::Graphics
