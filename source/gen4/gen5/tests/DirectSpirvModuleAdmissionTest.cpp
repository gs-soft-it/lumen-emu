// SPDX-License-Identifier: GPL-2.0-only
#include "graphics/host_gpu/spirvModuleAdmission.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using Libs::Graphics::AdmitSpirvModule;
using Libs::Graphics::AdmitShaderBufferInt64Atomics;
using Libs::Graphics::SpirvModuleAdmission;
using Words = std::vector<uint32_t>;
unsigned checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "DirectSpirvModuleAdmissionTest FAIL: %s\n", message);
        std::exit(1);
    }
}

Words Module() {
    // Complete fragment module storing a constant color; no optional feature.
    return {spv::MagicNumber, 0x00010000, 0, 11, 0,
        (2u << 16) | spv::OpCapability, spv::CapabilityShader,
        (3u << 16) | spv::OpMemoryModel, spv::AddressingModelLogical, spv::MemoryModelGLSL450,
        (6u << 16) | spv::OpEntryPoint, spv::ExecutionModelFragment, 9, 0x6e69616d, 0, 6,
        (3u << 16) | spv::OpExecutionMode, 9, spv::ExecutionModeOriginUpperLeft,
        (4u << 16) | spv::OpDecorate, 6, spv::DecorationLocation, 0,
        (2u << 16) | spv::OpTypeVoid, 1,
        (3u << 16) | spv::OpTypeFloat, 2, 32,
        (4u << 16) | spv::OpTypeVector, 3, 2, 4,
        (4u << 16) | spv::OpTypePointer, 4, spv::StorageClassOutput, 3,
        (3u << 16) | spv::OpTypeFunction, 5, 1,
        (4u << 16) | spv::OpVariable, 4, 6, spv::StorageClassOutput,
        (4u << 16) | spv::OpConstant, 2, 7, 0x3f800000,
        (7u << 16) | spv::OpConstantComposite, 3, 8, 7, 7, 7, 7,
        (5u << 16) | spv::OpFunction, 1, 9, spv::FunctionControlMaskNone, 5,
        (2u << 16) | spv::OpLabel, 10,
        (3u << 16) | spv::OpStore, 6, 8,
        (1u << 16) | spv::OpReturn,
        (1u << 16) | spv::OpFunctionEnd};
}

Words Prefix(std::initializer_list<uint32_t> instruction) {
    auto words = Module();
    words.insert(words.begin() + 5, instruction);
    return words;
}

Words Extension(const char* name) {
    const std::size_t count = (std::strlen(name) + 1 + 3) / 4;
    Words instruction(count + 1, 0);
    instruction[0] = (static_cast<uint32_t>(instruction.size()) << 16) | spv::OpExtension;
    std::memcpy(instruction.data() + 1, name, std::strlen(name));
    auto words = Module();
    words.insert(words.begin() + 7, instruction.begin(), instruction.end());
    return words;
}

void Required(const Words& words, const char* message) {
    const auto original = words;
    Check(AdmitSpirvModule(words, false) == SpirvModuleAdmission::FragmentBarycentricUnavailable,
          message);
    Check(AdmitSpirvModule(words, true) == SpirvModuleAdmission::Allowed,
          "actual enabled feature admits a required module");
    Check(words == original, "admission must not rewrite final words");
}

void Malformed(const Words& words, const char* message) {
    Check(AdmitSpirvModule(words, false) == SpirvModuleAdmission::Malformed, message);
    Check(AdmitSpirvModule(words, true) == SpirvModuleAdmission::Malformed,
          "enabled feature cannot admit malformed framing");
}
} // namespace

int main() {
    // An older GPU can run shaders with 32-bit atomics or ordinary 64-bit
    // arithmetic. Shared atomics need the buffer feature only when LDS spills.
    for (bool enabled : {false, true}) {
        Check(AdmitShaderBufferInt64Atomics(enabled, false, false, false) ==
                  SpirvModuleAdmission::Allowed, "ordinary shader needs no buffer64 atomics");
        Check(AdmitShaderBufferInt64Atomics(enabled, false, false, true) ==
                  SpirvModuleAdmission::Allowed, "LDS storage alone needs no buffer64 atomics");
        Check(AdmitShaderBufferInt64Atomics(enabled, false, true, false) ==
                  SpirvModuleAdmission::Allowed, "native Workgroup atomics use a separate feature");
        const auto expected = enabled ? SpirvModuleAdmission::Allowed
                                     : SpirvModuleAdmission::BufferInt64AtomicsUnavailable;
        for (bool shared : {false, true}) {
            for (bool lds_storage : {false, true}) {
                Check(AdmitShaderBufferInt64Atomics(enabled, true, shared, lds_storage) == expected,
                      "buffer64 atomic operations and coherent pair loads require enabled support");
            }
        }
        Check(AdmitShaderBufferInt64Atomics(enabled, false, true, true) == expected,
              "shared64 atomics lowered to SSBO require buffer64 support");
    }
    auto words = Module();
    const auto original = words;
    for (const bool supported : {false, true})
        Check(AdmitSpirvModule(words, supported) == SpirvModuleAdmission::Allowed,
              "ordinary fragment module needs no barycentric feature");
    Check(words == original, "ordinary module remains unchanged");
    Required(Prefix({(2u << 16) | spv::OpCapability, spv::CapabilityFragmentBarycentricKHR}),
             "capability alone requires enabled feature");
    static_assert(spv::CapabilityFragmentBarycentricKHR == spv::CapabilityFragmentBarycentricNV);
    static_assert(spv::BuiltInBaryCoordKHR == spv::BuiltInBaryCoordNV);
    static_assert(spv::BuiltInBaryCoordNoPerspKHR == spv::BuiltInBaryCoordNoPerspNV);
    static_assert(spv::DecorationPerVertexKHR == spv::DecorationPerVertexNV);
    for (const auto builtin : {spv::BuiltInBaryCoordKHR, spv::BuiltInBaryCoordNoPerspKHR}) {
        Required(Prefix({(4u << 16) | spv::OpDecorate, 6, spv::DecorationBuiltIn, builtin}),
                 "built-in alone cannot bypass admission");
        Required(Prefix({(5u << 16) | spv::OpMemberDecorate, 3, 0,
                         spv::DecorationBuiltIn, builtin}), "member built-in is covered");
        Required(Prefix({(4u << 16) | spv::OpDecorateId, 6, spv::DecorationBuiltIn, builtin}),
                 "ID decoration cannot bypass admission");
    }
    Required(Prefix({(3u << 16) | spv::OpDecorate, 6, spv::DecorationPerVertexKHR}),
             "raw per-vertex input requires feature");
    Required(Prefix({(4u << 16) | spv::OpMemberDecorate, 3, 0, spv::DecorationPerVertexKHR}),
             "per-vertex member is covered");
    Required(Prefix({(3u << 16) | spv::OpDecorate, 15, spv::DecorationPerVertexKHR,
                     (3u << 16) | spv::OpGroupDecorate, 15, 6}),
             "group cannot hide per-vertex requirement");
    Required(Extension("SPV_KHR_fragment_shader_barycentric"), "KHR extension is required");
    Required(Extension("SPV_NV_fragment_shader_barycentric"), "NV extension is required");
    Check(AdmitSpirvModule(Extension("SPV_KHR_shader_draw_parameters"), false) ==
              SpirvModuleAdmission::Allowed, "unrelated extension remains allowed");
    words.push_back(0); Malformed(words, "zero-length trailing instruction");
    words = Module(); words.push_back((2u << 16) | spv::OpCapability);
    Malformed(words, "truncated trailing instruction");
    words = Module(); words[5] = (0xffffu << 16) | spv::OpCapability;
    Malformed(words, "oversized instruction cannot read past module");
    Malformed(Prefix({(1u << 16) | spv::OpCapability}), "missing capability operand");
    Malformed(Prefix({(3u << 16) | spv::OpDecorate, 6, spv::DecorationBuiltIn}),
              "missing built-in operand");
    Malformed(Prefix({(3u << 16) | spv::OpMemberDecorate, 3, 0}), "missing member decoration");
    Malformed(Prefix({(2u << 16) | spv::OpExtension, 0x41414141}), "unterminated extension");
    Malformed(Prefix({(1u << 16) | spv::OpExtension}), "missing extension operand");
    words = Module(); words[0] = 0; Malformed(words, "bad magic");
    words = Module(); words[3] = 0; Malformed(words, "invalid ID bound");
    words = Module(); words[4] = 1; Malformed(words, "invalid schema");
    Malformed(Words(4, 0), "short module");
    words = Prefix({(2u << 16) | spv::OpCapability, spv::CapabilityFragmentBarycentricKHR});
    words.push_back(0); Malformed(words, "scan must continue after finding a requirement");
    // Every truncated instruction from a valid module must be rejected without an OOB read.
    const auto complete = Module();
    for (std::size_t cursor = 5; cursor < complete.size();) {
        const auto count = complete[cursor] >> spv::WordCountShift;
        for (std::size_t missing = 1; missing < count; ++missing)
            Malformed(Words(complete.begin(), complete.begin() + cursor + count - missing),
                      "truncated valid instruction");
        cursor += count;
    }
    std::printf("DirectSpirvModuleAdmissionTest PASS: %u checks\n", checks);
}
