// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "graphics/host_gpu/spirvModuleAdmission.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
using Libs::Graphics::AdmitSpirvModule;
using Libs::Graphics::SpirvModuleAdmission;
using Words = std::vector<uint32_t>;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "SpirvModuleAdmissionTests: FAIL %s\n", label);
        std::exit(1);
    }
}

Words Module() {
    // Complete fragment module storing a constant color. No optional feature.
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

void AddExtension(Words& words, std::size_t offset, const char* name) {
    const std::size_t count = (std::strlen(name) + 1 + 3) / 4;
    Words instruction(count + 1, 0);
    instruction[0] = (static_cast<uint32_t>(instruction.size()) << 16) | spv::OpExtension;
    std::memcpy(instruction.data() + 1, name, std::strlen(name));
    words.insert(words.begin() + offset, instruction.begin(), instruction.end());
}

Words Extension(const char* name) {
    auto words = Module();
    AddExtension(words, 7, name);
    return words;
}

Words BarycentricModule() {
    // Complete fragment module declaring the extension's vec3 input built-in.
    Words words {spv::MagicNumber, 0x00010300, 0, 14, 0,
        (2u << 16) | spv::OpCapability, spv::CapabilityShader,
        (2u << 16) | spv::OpCapability, spv::CapabilityFragmentBarycentricKHR,
        (3u << 16) | spv::OpMemoryModel, spv::AddressingModelLogical, spv::MemoryModelGLSL450,
        (7u << 16) | spv::OpEntryPoint, spv::ExecutionModelFragment, 9, 0x6e69616d, 0, 6, 13,
        (3u << 16) | spv::OpExecutionMode, 9, spv::ExecutionModeOriginUpperLeft,
        (4u << 16) | spv::OpDecorate, 6, spv::DecorationLocation, 0,
        (4u << 16) | spv::OpDecorate, 13, spv::DecorationBuiltIn, spv::BuiltInBaryCoordKHR,
        (2u << 16) | spv::OpTypeVoid, 1,
        (3u << 16) | spv::OpTypeFloat, 2, 32,
        (4u << 16) | spv::OpTypeVector, 3, 2, 4,
        (4u << 16) | spv::OpTypeVector, 11, 2, 3,
        (4u << 16) | spv::OpTypePointer, 12, spv::StorageClassInput, 11,
        (4u << 16) | spv::OpTypePointer, 4, spv::StorageClassOutput, 3,
        (3u << 16) | spv::OpTypeFunction, 5, 1,
        (4u << 16) | spv::OpVariable, 4, 6, spv::StorageClassOutput,
        (4u << 16) | spv::OpVariable, 12, 13, spv::StorageClassInput,
        (4u << 16) | spv::OpConstant, 2, 7, 0x3f800000,
        (7u << 16) | spv::OpConstantComposite, 3, 8, 7, 7, 7, 7,
        (5u << 16) | spv::OpFunction, 1, 9, spv::FunctionControlMaskNone, 5,
        (2u << 16) | spv::OpLabel, 10,
        (3u << 16) | spv::OpStore, 6, 8,
        (1u << 16) | spv::OpReturn,
        (1u << 16) | spv::OpFunctionEnd};
    AddExtension(words, 9, "SPV_KHR_fragment_shader_barycentric");
    return words;
}

void Required(const Words& words, const char* label) {
    const auto original = words;
    Check(AdmitSpirvModule(words, false) == SpirvModuleAdmission::FragmentBarycentricUnavailable,
          label);
    Check(AdmitSpirvModule(words, true) == SpirvModuleAdmission::Allowed,
          "actual enabled feature admits a required module");
    Check(words == original, "rejected and admitted module words are unchanged");
}

void Malformed(const Words& words, const char* label) {
    Check(AdmitSpirvModule(words, false) == SpirvModuleAdmission::Malformed, label);
    Check(AdmitSpirvModule(words, true) == SpirvModuleAdmission::Malformed,
          "feature support cannot admit malformed framing");
}

void Fixtures() {
    auto words = Module();
    const auto original = words;
    Check(AdmitSpirvModule(words, false) == SpirvModuleAdmission::Allowed,
          "ordinary complete fragment module does not need barycentric");
    Check(AdmitSpirvModule(words, true) == SpirvModuleAdmission::Allowed,
          "ordinary module also works with feature enabled");
    Check(words == original, "admission never rewrites final words");
    Required(BarycentricModule(), "complete barycentric fragment module needs its feature");
    Required(Prefix({(2u << 16) | spv::OpCapability, spv::CapabilityFragmentBarycentricKHR}),
             "capability alone is denied before native creation");
    static_assert(spv::CapabilityFragmentBarycentricKHR == spv::CapabilityFragmentBarycentricNV);
    static_assert(spv::BuiltInBaryCoordKHR == spv::BuiltInBaryCoordNV);
    static_assert(spv::BuiltInBaryCoordNoPerspKHR == spv::BuiltInBaryCoordNoPerspNV);
    static_assert(spv::DecorationPerVertexKHR == spv::DecorationPerVertexNV);
    for (auto builtin : {spv::BuiltInBaryCoordKHR, spv::BuiltInBaryCoordNoPerspKHR}) {
        Required(Prefix({(4u << 16) | spv::OpDecorate, 6, spv::DecorationBuiltIn, builtin}),
                 "built-in alone cannot bypass capability admission");
        Required(Prefix({(5u << 16) | spv::OpMemberDecorate, 3, 0,
                         spv::DecorationBuiltIn, builtin}), "member built-in is covered");
        Required(Prefix({(4u << 16) | spv::OpDecorateId, 6, spv::DecorationBuiltIn, builtin}),
                 "ID annotation cannot bypass admission");
    }
    Required(Prefix({(3u << 16) | spv::OpDecorate, 6, spv::DecorationPerVertexKHR}),
             "raw per-vertex input requires the feature");
    Required(Prefix({(4u << 16) | spv::OpMemberDecorate, 3, 0, spv::DecorationPerVertexKHR}),
             "member per-vertex decoration is covered");
    Required(Extension("SPV_KHR_fragment_shader_barycentric"), "KHR extension is required");
    Required(Extension("SPV_NV_fragment_shader_barycentric"), "NV extension is required");
    Check(AdmitSpirvModule(Extension("SPV_KHR_shader_draw_parameters"), false) ==
          SpirvModuleAdmission::Allowed, "unrelated extension is not rejected");
    auto group = Prefix({(3u << 16) | spv::OpDecorate, 15, spv::DecorationPerVertexKHR,
                         (3u << 16) | spv::OpGroupDecorate, 15, 6});
    Required(group, "decoration group cannot hide per-vertex requirement");
    const auto sampled = Prefix({(2u << 16) | spv::OpCapability,
        spv::CapabilitySampledImageArrayNonUniformIndexing});
    const auto image_atomic = Prefix({(2u << 16) | spv::OpCapability, spv::CapabilityInt64ImageEXT});
    Check(AdmitSpirvModule(sampled, true, false, true) ==
        SpirvModuleAdmission::SampledImageArrayNonUniformIndexingUnavailable,
        "nonuniform sampled image capability needs the actually enabled device feature");
    Check(AdmitSpirvModule(sampled, false, true, false) == SpirvModuleAdmission::Allowed,
        "nonuniform sampled images do not require unrelated optional features");
    Check(AdmitSpirvModule(image_atomic, true, true, false) ==
        SpirvModuleAdmission::ImageInt64AtomicsUnavailable,
        "64-bit image atomics require the actually enabled device feature");
    Check(AdmitSpirvModule(image_atomic, false, false, true) == SpirvModuleAdmission::Allowed,
        "64-bit image atomics do not require unrelated optional features");
    auto malformed_sampled = sampled;
    malformed_sampled.push_back(0);
    Check(AdmitSpirvModule(malformed_sampled, true, false, true) == SpirvModuleAdmission::Malformed,
        "malformed framing takes precedence over optional capability rejection");
    words.push_back(0); Malformed(words, "zero-length trailing instruction");
    words = Module(); words.push_back((2u << 16) | spv::OpCapability);
    Malformed(words, "truncated trailing instruction");
    words = Module(); words[5] = (0xffffu << 16) | spv::OpCapability;
    Malformed(words, "large instruction cannot read out of bounds");
    Malformed(Prefix({(1u << 16) | spv::OpCapability}), "missing capability operand");
    Malformed(Prefix({(3u << 16) | spv::OpDecorate, 6, spv::DecorationBuiltIn}),
              "missing built-in operand");
    Malformed(Prefix({(3u << 16) | spv::OpMemberDecorate, 3, 0}), "missing member decoration");
    Malformed(Prefix({(2u << 16) | spv::OpExtension, 0x41414141}), "unterminated extension");
    Malformed(Prefix({(1u << 16) | spv::OpExtension}), "empty extension operand");
    words = Module(); words[0] = 0; Malformed(words, "bad magic");
    words = Module(); words[3] = 0; Malformed(words, "invalid ID bound");
    words = Module(); words[4] = 1; Malformed(words, "invalid schema");
    Malformed(Words(4, 0), "short module");
    words = Prefix({(2u << 16) | spv::OpCapability, spv::CapabilityFragmentBarycentricKHR});
    words.push_back(0); Malformed(words, "scan continues after a detected requirement");
}
} // namespace

int main(int argc, char** argv) {
    Fixtures();
    for (int argument = 1; argument < argc; ++argument) {
        std::ifstream stream(argv[argument], std::ios::binary);
        Check(stream.is_open(), "retained shader must exist");
        std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), {});
        Check(bytes.size() % sizeof(uint32_t) == 0, "retained shader word alignment");
        Words words(bytes.size() / sizeof(uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());
        Check(AdmitSpirvModule(words, false) == SpirvModuleAdmission::Allowed,
              argv[argument]);
    }
    std::printf("SpirvModuleAdmissionTests: fixtures PASS; retained non-barycentric modules=%d\n",
                argc - 1);
    return 0;
}
