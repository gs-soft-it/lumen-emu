// Synthetic module contracts for the optional geometry-based barycentric path.
// No ROM input, Vulkan device, or performance measurement is used here.
#include "graphics/shader/barycentricFallback.h"
#include "graphics/host_gpu/spirvModuleAdmission.h"

#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace {
namespace Fallback = Libs::Graphics::BarycentricFallback;
unsigned checks = 0;

void Check(bool value, const std::string& message) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "DirectBarycentricFallbackTests FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

std::vector<uint32_t> Assemble(const std::string& text, spv_target_env environment = SPV_ENV_VULKAN_1_2) {
    spvtools::SpirvTools tools(environment);
    std::string diagnostic;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&,
                                 const char* message) { diagnostic += message; });
    std::vector<uint32_t> words;
    Check(tools.Assemble(text, &words), "assemble fixture: " + diagnostic);
    return words;
}

void Validate(std::span<const uint32_t> words, const char* name) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string diagnostic;
    tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&,
                                 const char* message) { diagnostic += message; });
    const bool valid = tools.Validate(words.data(), words.size());
    Check(valid, std::string(name) + " validation: " + diagnostic);
}

std::string Disassemble(std::span<const uint32_t> words) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::string text;
    Check(tools.Disassemble(words.data(), words.size(), &text), "disassemble output");
    return text;
}

bool HasDecoration(std::span<const uint32_t> words, uint32_t variable,
                   spv::Decoration decoration, uint32_t literal = UINT32_MAX) {
    for (size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> 16u;
        if (count == 0 || count > words.size() - cursor) return false;
        if ((words[cursor] & 0xffffu) == spv::OpDecorate && count >= 3 &&
            words[cursor + 1] == variable && words[cursor + 2] == decoration &&
            (literal == UINT32_MAX || (count >= 4 && words[cursor + 3] == literal))) return true;
        cursor += count;
    }
    return false;
}

Fallback::Limits Limits() {
    Fallback::Limits limits;
    limits.max_geometry_input_components = 128;
    limits.max_geometry_output_components = 128;
    limits.max_geometry_total_output_components = 1024;
    limits.max_geometry_output_vertices = 256;
    limits.max_fragment_input_components = 128;
    return limits;
}

// A live ordinary float4 input at location 3 also exercises interface matching
// and ensures synthesized coordinates cannot take over an occupied location.
std::vector<uint32_t> Fragment(unsigned barycentrics = 1, const char* interpolation = "",
                               bool array_input = false, bool per_vertex = false,
                               bool component = false, bool second_entry = false,
                               bool primitive_id = false, bool signed_id = false,
                               unsigned sample_builtin = 0) {
    std::string text = "OpCapability Shader\n";
    if (primitive_id) text += "OpCapability Geometry\n";
    if (barycentrics || per_vertex) text += "OpCapability FragmentBarycentricKHR\n";
    if (std::string(interpolation) == "Sample" || sample_builtin) text += "OpCapability SampleRateShading\n";
    if (barycentrics || per_vertex)
        text += "OpExtension \"SPV_KHR_fragment_shader_barycentric\"\n";
    text += "OpMemoryModel Logical GLSL450\n";
    const auto interfaces = std::string(" %color %ordinary") +
        ((barycentrics & 1) ? " %perspective" : "") +
        ((barycentrics & 2) ? " %linear" : "") + (primitive_id ? " %primitive" : "") +
        (sample_builtin ? " %sample" : "");
    text += "OpEntryPoint Fragment %main \"main\"" + interfaces + "\n";
    if (second_entry) text += "OpEntryPoint Fragment %other \"other\"" + interfaces + "\n";
    text += "OpExecutionMode %main OriginUpperLeft\n";
    if (second_entry) text += "OpExecutionMode %other OriginUpperLeft\n";
    text += "OpDecorate %color Location 0\nOpDecorate %ordinary Location 3\n";
    if (component) text += "OpDecorate %ordinary Component 1\n";
    if (per_vertex) text += "OpDecorate %ordinary PerVertexKHR\n";
    if (primitive_id) text += "OpDecorate %primitive BuiltIn PrimitiveId\nOpDecorate %primitive Flat\n";
    if (sample_builtin == 1) text += "OpDecorate %sample BuiltIn SampleId\nOpDecorate %sample Flat\n";
    if (sample_builtin == 2) text += "OpDecorate %sample BuiltIn SamplePosition\n";
    if (barycentrics & 1) {
        text += "OpDecorate %perspective BuiltIn BaryCoordKHR\n";
        if (*interpolation) text += std::string("OpDecorate %perspective ") + interpolation + "\n";
    }
    if (barycentrics & 2) {
        text += "OpDecorate %linear BuiltIn BaryCoordNoPerspKHR\n";
        if (*interpolation) text += std::string("OpDecorate %linear ") + interpolation + "\n";
    }
    text += R"(
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%three = OpConstant %uint 3
%zero = OpConstant %uint 0
%one = OpConstant %float 1
%vec3 = OpTypeVector %float 3
%vec4 = OpTypeVector %float 4
%array = OpTypeArray %vec4 %three
%input3 = OpTypePointer Input %vec3
%input4 = OpTypePointer Input %vec4
%inputArray = OpTypePointer Input %array
%output4 = OpTypePointer Output %vec4
%color = OpVariable %output4 Output
)";
    text += (array_input || per_vertex) ? "%ordinary = OpVariable %inputArray Input\n"
                                      : "%ordinary = OpVariable %input4 Input\n";
    if (barycentrics & 1) text += "%perspective = OpVariable %input3 Input\n";
    if (barycentrics & 2) text += "%linear = OpVariable %input3 Input\n";
    if (primitive_id) {
        text += std::string("%inputId = OpTypePointer Input ") + (signed_id ? "%int" : "%uint") + "\n";
        text += "%primitive = OpVariable %inputId Input\n";
    }
    if (sample_builtin) {
        if (sample_builtin == 2) text += "%vec2 = OpTypeVector %float 2\n";
        text += std::string("%inputSample = OpTypePointer Input ") + (sample_builtin == 1 ? "%int" : "%vec2") + "\n";
        text += "%sample = OpVariable %inputSample Input\n";
    }
    text += "%main = OpFunction %void None %function\n%entry = OpLabel\n";
    if (primitive_id)
        text += std::string("%id = OpLoad ") + (signed_id ? "%int" : "%uint") + " %primitive\n";
    if (sample_builtin)
        text += std::string("%sampleValue = OpLoad ") + (sample_builtin == 1 ? "%int" : "%vec2") + " %sample\n";
    if (array_input || per_vertex)
        text += "%ordinaryElement = OpAccessChain %input4 %ordinary %zero\n"
                "%parameter = OpLoad %vec4 %ordinaryElement\n";
    else text += "%parameter = OpLoad %vec4 %ordinary\n";
    if (barycentrics) {
        if (barycentrics & 1) text += "%smooth = OpLoad %vec3 %perspective\n";
        if (barycentrics & 2) text += "%affine = OpLoad %vec3 %linear\n";
        std::string selected = (barycentrics & 1) ? "%smooth" : "%affine";
        if (barycentrics == 3) {
            text += "%both = OpFAdd %vec3 %smooth %affine\n";
            selected = "%both";
        }
        text += "%x = OpCompositeExtract %float " + selected + " 0\n";
        text += "%y = OpCompositeExtract %float " + selected + " 1\n";
        text += "%z = OpCompositeExtract %float " + selected + " 2\n";
        text += "%coordinates = OpCompositeConstruct %vec4 %x %y %z %one\n"
                "%result = OpFAdd %vec4 %coordinates %parameter\nOpStore %color %result\n";
    } else text += "OpStore %color %parameter\n";
    text += "OpReturn\nOpFunctionEnd\n";
    if (second_entry)
        text += "%other = OpFunction %void None %function\n%otherEntry = OpLabel\n"
                "OpReturn\nOpFunctionEnd\n";
    return Assemble(text);
}

std::vector<uint32_t> Vertex(bool position_block = false, bool ordinary_output = true,
                             bool vec3_output = false, bool unknown_builtin = false,
                             bool rich_builtins = false, bool point_size = false,
                             bool invariant_member = false, bool multiple_builtin_blocks = false,
                             bool layer_viewport = false) {
    std::string text = "OpCapability Shader\n";
    if (rich_builtins || multiple_builtin_blocks)
        text += "OpCapability ClipDistance\nOpCapability CullDistance\n";
    if (layer_viewport) text += "OpCapability ShaderLayer\nOpCapability ShaderViewportIndex\n";
    text += "OpMemoryModel Logical GLSL450\nOpEntryPoint Vertex %main \"main\" %position";
    if (ordinary_output) text += " %parameter";
    if (unknown_builtin) text += " %unknown";
    if (rich_builtins) text += " %clip %cull";
    if (layer_viewport) text += " %layer %viewport";
    if (point_size) text += " %pointSize";
    if (multiple_builtin_blocks) text += " %clipBlock %cullBlock";
    text += "\n";
    if (position_block)
        text += "OpDecorate %perVertex Block\nOpMemberDecorate %perVertex 0 BuiltIn Position\n";
    else text += "OpDecorate %position BuiltIn Position\n";
    if (invariant_member) text += "OpMemberDecorate %perVertex 0 Invariant\n";
    if (ordinary_output) text += "OpDecorate %parameter Location 3\n";
    if (unknown_builtin) text += "OpDecorate %unknown BuiltIn VertexIndex\n";
    if (rich_builtins)
        text += "OpDecorate %clip BuiltIn ClipDistance\nOpDecorate %cull BuiltIn CullDistance\n";
    if (layer_viewport)
        text += "OpDecorate %layer BuiltIn Layer\nOpDecorate %viewport BuiltIn ViewportIndex\n";
    if (point_size) text += "OpDecorate %pointSize BuiltIn PointSize\n";
    if (multiple_builtin_blocks)
        text += "OpDecorate %clipType Block\nOpMemberDecorate %clipType 0 BuiltIn ClipDistance\n"
                "OpDecorate %cullType Block\nOpMemberDecorate %cullType 0 BuiltIn CullDistance\n";
    text += R"(
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%two = OpConstant %uint 2
%fzero = OpConstant %float 0
%one = OpConstant %float 1
%vec3 = OpTypeVector %float 3
%vec4 = OpTypeVector %float 4
%origin = OpConstantComposite %vec4 %fzero %fzero %fzero %one
%values3 = OpConstantComposite %vec3 %one %one %one
%output4 = OpTypePointer Output %vec4
%output3 = OpTypePointer Output %vec3
%outputUint = OpTypePointer Output %uint
%outputFloat = OpTypePointer Output %float
%distances = OpTypeArray %float %two
%outputDistances = OpTypePointer Output %distances
%inside = OpConstantComposite %distances %one %one
)";
    if (position_block)
        text += "%perVertex = OpTypeStruct %vec4\n"
                "%outputBlock = OpTypePointer Output %perVertex\n"
                "%position = OpVariable %outputBlock Output\n";
    else text += "%position = OpVariable %output4 Output\n";
    if (ordinary_output)
        text += std::string("%parameter = OpVariable ") + (vec3_output ? "%output3" : "%output4") + " Output\n";
    if (unknown_builtin) text += "%unknown = OpVariable %outputUint Output\n";
    if (rich_builtins)
        text += "%clip = OpVariable %outputDistances Output\n%cull = OpVariable %outputDistances Output\n";
    if (layer_viewport)
        text += "%layer = OpVariable %outputUint Output\n%viewport = OpVariable %outputUint Output\n";
    if (point_size) text += "%pointSize = OpVariable %outputFloat Output\n";
    if (multiple_builtin_blocks)
        text += "%clipType = OpTypeStruct %distances\n%cullType = OpTypeStruct %distances\n"
                "%clipPointer = OpTypePointer Output %clipType\n%cullPointer = OpTypePointer Output %cullType\n"
                "%clipBlock = OpVariable %clipPointer Output\n%cullBlock = OpVariable %cullPointer Output\n";
    text += "%main = OpFunction %void None %function\n%entry = OpLabel\n";
    if (position_block)
        text += "%positionMember = OpAccessChain %output4 %position %zero\nOpStore %positionMember %origin\n";
    else text += "OpStore %position %origin\n";
    if (ordinary_output)
        text += std::string("OpStore %parameter ") + (vec3_output ? "%values3" : "%origin") + "\n";
    if (unknown_builtin) text += "OpStore %unknown %zero\n";
    if (rich_builtins)
        text += "OpStore %clip %inside\nOpStore %cull %inside\n";
    if (layer_viewport) text += "OpStore %layer %zero\nOpStore %viewport %zero\n";
    if (point_size) text += "OpStore %pointSize %one\n";
    if (multiple_builtin_blocks)
        text += "%clipMember = OpAccessChain %outputDistances %clipBlock %zero\n"
                "%cullMember = OpAccessChain %outputDistances %cullBlock %zero\n"
                "OpStore %clipMember %inside\nOpStore %cullMember %inside\n";
    text += "OpReturn\nOpFunctionEnd\n";
    return Assemble(text);
}

// Standalone PerVertex-only interfaces exercise scalar and integer payloads,
// multiple arrays, and a dynamically computed corner index without bary builtins.
std::vector<uint32_t> PerVertexPayload(bool vertex, unsigned kind, bool second = false,
                                      uint32_t length = 3, const char* qualifier = "") {
    const char* type = kind == 0 ? "%float" : kind == 1 ? "%uint" : kind == 2 ? "%int" : "%uvec2";
    std::string text = "OpCapability Shader\n";
    if (!vertex)
        text += "OpCapability FragmentBarycentricKHR\nOpExtension \"SPV_KHR_fragment_shader_barycentric\"\n";
    text += "OpMemoryModel Logical GLSL450\nOpEntryPoint ";
    text += vertex ? "Vertex %main \"main\" %position %parameter" : "Fragment %main \"main\" %color %parameter";
    if (second) text += " %second";
    text += "\n";
    if (vertex) text += "OpDecorate %position BuiltIn Position\n";
    else text += "OpExecutionMode %main OriginUpperLeft\nOpDecorate %color Location 0\n";
    text += "OpDecorate %parameter Location 3\n";
    if (second) text += "OpDecorate %second Location 4\n";
    if (!vertex) {
        text += "OpDecorate %parameter PerVertexKHR\n";
        if (second) text += "OpDecorate %second PerVertexKHR\n";
        if (*qualifier) text += std::string("OpDecorate %parameter ") + qualifier + "\n";
    }
    if (kind != 0) {
        text += "OpDecorate %parameter Flat\n";
        if (second) text += "OpDecorate %second Flat\n";
    }
    text += R"(
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%vec4 = OpTypeVector %float 4
%uvec2 = OpTypeVector %uint 2
%one = OpConstant %uint 1
%u17 = OpConstant %uint 17
%u19 = OpConstant %uint 19
%i17 = OpConstant %int -17
%f17 = OpConstant %float 17
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%pair = OpConstantComposite %uvec2 %u17 %u19
%origin = OpConstantComposite %vec4 %fzero %fzero %fzero %fone
%output4 = OpTypePointer Output %vec4
)";
    if (vertex) {
        text += std::string("%outputPayload = OpTypePointer Output ") + type + "\n";
        text += "%position = OpVariable %output4 Output\n%parameter = OpVariable %outputPayload Output\n";
        if (second) text += "%second = OpVariable %outputPayload Output\n";
    } else {
        text += "%length = OpConstant %uint " + std::to_string(length) + "\n";
        text += std::string("%array = OpTypeArray ") + type + " %length\n";
        text += std::string("%inputPayload = OpTypePointer Input ") + type + "\n";
        text += "%inputArray = OpTypePointer Input %array\n%parameter = OpVariable %inputArray Input\n";
        if (second) text += "%second = OpVariable %inputArray Input\n";
        text += "%color = OpVariable %output4 Output\n";
    }
    text += "%main = OpFunction %void None %function\n%entry = OpLabel\n";
    if (vertex) {
        const char* value = kind == 0 ? "%f17" : kind == 1 ? "%u17" : kind == 2 ? "%i17" : "%pair";
        text += std::string("OpStore %position %origin\nOpStore %parameter ") + value + "\n";
        if (second) text += std::string("OpStore %second ") + value + "\n";
    } else {
        text += "%index = OpIAdd %uint %one %one\n%pointer = OpAccessChain %inputPayload %parameter %index\n";
        text += std::string("%value = OpLoad ") + type + " %pointer\n";
        if (second) {
            text += "%otherPointer = OpAccessChain %inputPayload %second %one\n";
            text += std::string("%other = OpLoad ") + type + " %otherPointer\n";
        }
        if (kind == 3) text += "%scalar = OpCompositeExtract %uint %value 1\n";
        if (kind != 0)
            text += std::string("%raw = ") + (kind == 2 ? "OpConvertSToF" : "OpConvertUToF") +
                    " %float " + (kind == 3 ? "%scalar" : "%value") + "\n";
        const char* scalar = kind == 0 ? "%value" : "%raw";
        text += std::string("%pixel = OpCompositeConstruct %vec4 ") + scalar + " " + scalar + " " + scalar + " %fone\n";
        text += "OpStore %color %pixel\n";
    }
    text += "OpReturn\nOpFunctionEnd\n";
    return Assemble(text);
}

std::vector<uint32_t> FunctionInstructions(std::span<const uint32_t> words) {
    for (size_t cursor = 5; cursor < words.size(); cursor += words[cursor] >> 16u) {
        if ((words[cursor] & 0xffffu) == spv::OpFunction)
            return {words.begin() + cursor, words.end()};
    }
    return {};
}

void CheckReady(const Fallback::FragmentResult& fragment, unsigned expected_inputs) {
    Check(fragment.status == Fallback::Status::Ready, "fragment lowering: " + fragment.reason);
    Validate(fragment.plan.spirv, "lowered fragment");
    Check(fragment.plan.barycentric_inputs.size() == expected_inputs, "coordinate input count");
    Check(Libs::Graphics::AdmitSpirvModule(fragment.plan.spirv, false) ==
          Libs::Graphics::SpirvModuleAdmission::Allowed, "lowered fragment requires no native barycentric feature");
    std::vector<uint32_t> locations;
    for (const auto& input : fragment.plan.barycentric_inputs) {
        Check(input.location != 3, "coordinate location does not replace live ordinary varying");
        Check(std::ranges::find(locations, input.location) == locations.end(), "coordinate locations are distinct");
        locations.push_back(input.location);
        Check(HasDecoration(fragment.plan.spirv, input.variable, spv::DecorationLocation, input.location),
              "fragment plan agrees with emitted location");
        Check(HasDecoration(fragment.plan.spirv, input.variable, spv::DecorationNoPerspective) == input.no_perspective,
              "perspective interpolation mode retained");
    }
}

void PrimitiveAssemblyContracts() {
    namespace Assembly = Libs::Graphics::PrimitiveAssembly;
    const auto expand = [](const std::vector<uint32_t>& input, uint32_t element_size,
                           bool last, bool restart, uint64_t capacity = Assembly::MaxTriangleListBytes) {
        std::vector<uint8_t> bytes(input.size() * element_size);
        for (size_t i = 0; i < input.size(); ++i)
            std::memcpy(bytes.data() + i * element_size, &input[i], element_size);
        return Assembly::TriangleStripToList(bytes, static_cast<uint32_t>(input.size()),
                                             element_size, last, restart, capacity);
    };
    for (const bool last : {false, true}) {
        // Expected lists are explicit primitive-assembly contracts, independent
        // of the helper's parity implementation and the GPU readback fixture.
        const std::vector<uint32_t> expected = last
            ? std::vector<uint32_t>{0, 1, 2, 2, 1, 3, 2, 3, 4}
            : std::vector<uint32_t>{0, 1, 2, 1, 3, 2, 2, 3, 4};
        const auto automatic = Assembly::TriangleStripToListAuto(5, last);
        Check(automatic.success && automatic.indices == expected,
              "auto strip preserves alternating winding and provoking slot");
        for (const auto element_size : {1u, 2u, 4u}) {
            const auto marker = UINT32_MAX >> ((4 - element_size) * 8);
            const auto ordinary = expand({0, 1, 2, 3, 4}, element_size, last, false);
            Check(ordinary.success && ordinary.indices == expected,
                  "8/16/32-bit indexed strip uses the same primitive order");
            const std::vector<uint32_t> restart_input{
                marker, 4, marker, 5, 6, marker,
                10, 11, 12, 13, marker, marker, 20, 21, 22, 23, 24, marker};
            const std::vector<uint32_t> restart_expected = last
                ? std::vector<uint32_t>{10, 11, 12, 12, 11, 13, 20, 21, 22, 22, 21, 23, 22, 23, 24}
                : std::vector<uint32_t>{10, 11, 12, 11, 13, 12, 20, 21, 22, 21, 23, 22, 22, 23, 24};
            const auto restarted = expand(restart_input, element_size, last, true);
            Check(restarted.success && restarted.indices == restart_expected,
                  "restart resets parity, omits incomplete segments, and preserves primitive sequence");
            const auto high = expand({0, marker, 2, 3}, element_size, last, false);
            const std::vector<uint32_t> high_expected = last
                ? std::vector<uint32_t>{0, marker, 2, 2, marker, 3}
                : std::vector<uint32_t>{0, marker, 2, marker, 3, 2};
            Check(high.success && high.indices == high_expected,
                  "all-one index remains a real vertex when restart is disabled");
            const auto degenerate = expand({0, 1, 1, 2, 3}, element_size, last, false);
            const std::vector<uint32_t> degenerate_expected = last
                ? std::vector<uint32_t>{0, 1, 1, 1, 1, 2, 1, 2, 3}
                : std::vector<uint32_t>{0, 1, 1, 1, 2, 1, 1, 2, 3};
            Check(degenerate.success && degenerate.indices == degenerate_expected,
                  "degenerates retain parity and primitive IDs");
            const auto exact = expand({0, 1, 2, 3}, element_size, last, false, 24);
            Check(exact.success && exact.indices.size() == 6, "exact byte capacity is admitted");
            const auto overflow = expand({0, 1, 2, 3}, element_size, last, false, 23);
            Check(!overflow.success && overflow.indices.empty(), "byte capacity failure publishes no partial list");
        }
        for (const auto count : {0u, 1u, 2u}) {
            const auto incomplete = Assembly::TriangleStripToListAuto(count, last);
            Check(incomplete.success && incomplete.indices.empty(), "incomplete auto strip has no triangles");
        }
        Check(!Assembly::TriangleStripToListAuto(UINT32_MAX, last, UINT64_MAX).success,
              "expanded Vulkan index count cannot overflow uint32");
        Check(!Assembly::TriangleStripToListAuto(
                  static_cast<uint32_t>(Assembly::MaxTriangleListBytes / 12 + 3), last).success,
              "default upload capacity is checked before auto allocation");
    }
    const std::array<uint8_t, 3> bytes {0, 1, 2};
    Check(!Assembly::TriangleStripToList(bytes, 3, 0, false, false).success,
          "zero index element width rejected");
    Check(!Assembly::TriangleStripToList(bytes, 1, 3, false, false).success,
          "unsupported index element width rejected");
    Check(!Assembly::TriangleStripToList(bytes, UINT32_MAX, 4, false, false).success,
          "index byte size multiplication is checked in uint64");
    Check(!Assembly::TriangleStripToList(bytes, 2, 2, false, false).success,
          "truncated input byte range rejected");
}

void PositiveContracts() {
    const auto limits = Limits();
    const auto ordinary = Fragment(0);
    Validate(ordinary, "ordinary fragment");
    const auto unchanged = Fallback::PrepareFragment(ordinary, limits);
    Check(unchanged.status == Fallback::Status::NotNeeded, "ordinary fragment needs no geometry fallback");
    Check(unchanged.plan.spirv == ordinary, "ordinary fragment preserved byte for byte");
    for (const auto barycentrics : {1u, 2u, 3u}) {
        for (const auto* interpolation : {"", "Centroid", "Sample"}) {
            const auto source = Fragment(barycentrics, interpolation);
            Validate(source, "source barycentric fragment");
            const auto source_copy = source;
            const auto fragment = Fallback::PrepareFragment(source, limits);
            Check(source == source_copy, "fragment source remains immutable");
            CheckReady(fragment, barycentrics == 3 ? 2 : 1);
            if (*interpolation) {
                const auto decoration = std::string(interpolation) == "Sample" ? spv::DecorationSample : spv::DecorationCentroid;
                for (const auto& input : fragment.plan.barycentric_inputs)
                    Check(HasDecoration(fragment.plan.spirv, input.variable, decoration), "sample/centroid qualifier preserved");
            }
            for (const bool block : {false, true}) {
                const auto vertex = Vertex(block);
                Validate(vertex, "vertex producer");
                for (const bool last : {false, true}) {
                    const auto geometry = Fallback::BuildGeometry(vertex, fragment.plan,
                                                                  Fallback::Topology::TriangleList, last, limits);
                    Check(geometry.success, "geometry generation: " + geometry.reason);
                    Validate(geometry.spirv, "generated geometry");
                    const auto text = Disassemble(geometry.spirv);
                    Check(text.find("Triangles") != std::string::npos && text.find("OutputVertices 3") != std::string::npos,
                          "geometry stage consumes and emits one triangle");
                    Check(text.find(" Location 3") != std::string::npos, "geometry preserves ordinary export");
                    Check(Libs::Graphics::AdmitSpirvModule(geometry.spirv, false) ==
                          Libs::Graphics::SpirvModuleAdmission::Allowed, "geometry does not require barycentric feature");
                }
            }
        }
    }
}

void BuiltinContracts() {
    const auto limits = Limits();
    const auto vertex_with_builtins = Vertex(true, true, false, false, true);
    Validate(vertex_with_builtins, "vertex with clip/cull");
    for (const bool signed_id : {false, true}) {
        const auto source = Fragment(1, "", false, false, false, false, true, signed_id);
        Validate(source, "fragment primitive ID");
        const auto fragment = Fallback::PrepareFragment(source, limits);
        CheckReady(fragment, 1);
        const auto geometry = Fallback::BuildGeometry(vertex_with_builtins, fragment.plan,
                                                       Fallback::Topology::TriangleList, false, limits);
        Check(geometry.success, "builtin geometry generation: " + geometry.reason);
        Validate(geometry.spirv, "geometry with clip/cull/primitive ID");
        const auto text = Disassemble(geometry.spirv);
        for (const auto* builtin : {"ClipDistance", "CullDistance", "PrimitiveId"})
            Check(text.find(builtin) != std::string::npos, std::string("forward builtin ") + builtin);
    }
    const auto invariant_vertex = Vertex(true, true, false, false, false, false, true);
    Validate(invariant_vertex, "vertex invariant Position producer");
    const auto fragment = Fallback::PrepareFragment(Fragment(), limits);
    CheckReady(fragment, 1);
    const auto invariant_geometry = Fallback::BuildGeometry(invariant_vertex, fragment.plan,
                                                          Fallback::Topology::TriangleList, false, limits);
    Check(invariant_geometry.success, "invariant Position geometry generation: " + invariant_geometry.reason);
    Validate(invariant_geometry.spirv, "geometry with invariant Position");
    Check(Disassemble(invariant_geometry.spirv).find("Invariant") != std::string::npos,
          "invariant Position qualifier preserved");
}

void PerVertexContracts() {
    const auto limits = Limits();
    for (const auto barycentrics : {0u, 1u, 2u, 3u}) {
        for (const auto* interpolation : {"", "Centroid", "Sample"}) {
            if (barycentrics == 0 && *interpolation) continue;
            const auto source = Fragment(barycentrics, interpolation, false, true);
            Validate(source, "source PerVertex array with barycentric interpolation");
            const auto fragment = Fallback::PrepareFragment(source, limits);
            Check(fragment.status == Fallback::Status::Ready, "lower PerVertex input: " + fragment.reason);
            Validate(fragment.plan.spirv, "lowered PerVertex fragment");
            Check(fragment.plan.per_vertex_inputs.size() == 1, "retain one PerVertex input plan");
            Check(FunctionInstructions(source) == FunctionInstructions(fragment.plan.spirv),
                  "PerVertex access chains and payload loads remain unchanged");
            Check(Libs::Graphics::AdmitSpirvModule(fragment.plan.spirv, false) ==
                  Libs::Graphics::SpirvModuleAdmission::Allowed, "PerVertex fragment needs no native barycentric capability");
            const auto& per_vertex = fragment.plan.per_vertex_inputs.front();
            Check(HasDecoration(fragment.plan.spirv, per_vertex.variable, spv::DecorationFlat),
                  "raw three-corner array is transported without interpolation");
            Check(!HasDecoration(fragment.plan.spirv, per_vertex.variable, spv::DecorationPerVertexKHR),
                  "PerVertexKHR requirement removed after preserving array semantics");
            for (const auto& bary : fragment.plan.barycentric_inputs) {
                Check(bary.location < per_vertex.location || bary.location >= per_vertex.location + 3,
                      "barycentric location avoids all three PerVertex array locations");
                if (*interpolation)
                    Check(HasDecoration(fragment.plan.spirv, bary.variable,
                                        std::string(interpolation) == "Sample" ? spv::DecorationSample : spv::DecorationCentroid),
                          "barycentric centroid/sample qualifier survives beside PerVertex input");
            }
            for (const bool last : {false, true}) {
                const auto geometry = Fallback::BuildGeometry(Vertex(), fragment.plan,
                                                               Fallback::Topology::TriangleList, last, limits);
                Check(geometry.success, "emit PerVertex array geometry: " + geometry.reason);
                Validate(geometry.spirv, "PerVertex forwarding geometry");
            }
        }
    }
    for (const auto kind : {0u, 1u, 2u, 3u}) {
        const auto source = PerVertexPayload(false, kind, true);
        const auto vertex = PerVertexPayload(true, kind, true);
        Validate(source, "PerVertex scalar/integer source with dynamic indexing");
        Validate(vertex, "PerVertex scalar/integer producer");
        const auto fragment = Fallback::PrepareFragment(source, limits);
        Check(fragment.status == Fallback::Status::Ready, "PerVertex-only scalar/integer lowering: " + fragment.reason);
        Validate(fragment.plan.spirv, "PerVertex-only lowered scalar/integer fragment");
        Check(fragment.plan.barycentric_inputs.empty() && fragment.plan.per_vertex_inputs.size() == 2,
              "PerVertex-only shader does not require barycentric builtins");
        const auto a = fragment.plan.per_vertex_inputs[0].location;
        const auto b = fragment.plan.per_vertex_inputs[1].location;
        Check(a + 3 <= b || b + 3 <= a, "two PerVertex arrays reserve disjoint three-location spans");
        Check(FunctionInstructions(source) == FunctionInstructions(fragment.plan.spirv),
              "dynamic PerVertex indexing and integer conversion remain byte-for-byte intact");
        for (const bool last : {false, true}) {
            const auto geometry = Fallback::BuildGeometry(vertex, fragment.plan,
                                                           Fallback::Topology::TriangleList, last, limits);
            Check(geometry.success, "scalar/integer PerVertex geometry: " + geometry.reason);
            Validate(geometry.spirv, "scalar/integer PerVertex geometry");
        }
    }
    for (const auto length : {2u, 4u})
        Check(Fallback::PrepareFragment(PerVertexPayload(false, 0, false, length), limits).status ==
              Fallback::Status::Unsupported, "PerVertex array must contain exactly three corners");
    for (const auto* qualifier : {"NoPerspective", "Centroid", "Sample"})
        Check(Fallback::PrepareFragment(PerVertexPayload(false, 0, false, 3, qualifier), limits).status ==
              Fallback::Status::Unsupported, "unsupported PerVertex interpolation qualifier rejected");
    const auto scalar = Fallback::PrepareFragment(PerVertexPayload(false, 0), limits);
    Check(!Fallback::BuildGeometry(Vertex(), scalar.plan, Fallback::Topology::TriangleList, false, limits).success,
          "PerVertex element type must match the producer type");
    const auto source = Fragment(1, "", false, true);
    const auto fragment = Fallback::PrepareFragment(source, limits);
    auto constrained = limits;
    constrained.max_geometry_output_components = 18; // Position4 + array12 + bary3 = 19.
    Check(!Fallback::BuildGeometry(Vertex(), fragment.plan, Fallback::Topology::TriangleList, false, constrained).success,
          "GS per-vertex component budget counts all three raw corners");
    constrained = limits;
    constrained.max_geometry_total_output_components = 56; // Three emitted vertices require 57.
    Check(!Fallback::BuildGeometry(Vertex(), fragment.plan, Fallback::Topology::TriangleList, false, constrained).success,
          "GS total component budget counts the replicated three-corner payload");
    constrained = limits;
    constrained.max_fragment_input_components = 14; // array12 + bary3.
    Check(Fallback::PrepareFragment(source, constrained).status == Fallback::Status::Unsupported,
          "fragment component budget counts the expanded raw array");
}

void NegativeContracts() {
    const auto limits = Limits();
    const auto vertex = Vertex();
    const auto source = Fragment();
    const auto fragment = Fallback::PrepareFragment(source, limits);
    CheckReady(fragment, 1);
    const auto reject_fragment = [&](const std::vector<uint32_t>& words, const char* reason) {
        const auto result = Fallback::PrepareFragment(words, limits);
        Check(result.status == Fallback::Status::Unsupported, std::string("reject ") + reason);
        Check(!result.reason.empty(), std::string("explain ") + reason);
    };
    reject_fragment(Fragment(1, "", true), "user array input");
    reject_fragment(Fragment(1, "", false, false, true), "nonzero component");
    reject_fragment(Fragment(1, "", false, false, false, true), "multiple entry points");
    for (size_t prefix = 0; prefix < source.size(); ++prefix) {
        const std::vector<uint32_t> truncated(source.begin(), source.begin() + prefix);
        const auto result = Fallback::PrepareFragment(truncated, limits);
        Check(result.status != Fallback::Status::Ready,
              "truncated module cannot become executable fallback at word " + std::to_string(prefix));
    }
    auto malformed = source;
    malformed.push_back(0);
    reject_fragment(malformed, "zero instruction length");
    malformed = source;
    malformed.push_back((0xffffu << 16u) | spv::OpNop);
    reject_fragment(malformed, "oversized instruction length");
    auto too_small = limits;
    too_small.max_fragment_input_components = 2;
    Check(Fallback::PrepareFragment(source, too_small).status == Fallback::Status::Unsupported,
          "fragment input component limit enforced");
    Check(Fallback::PrepareFragment(source, {}).status == Fallback::Status::Unsupported,
          "unreported limits fail closed");

    const auto reject_geometry = [&](const std::vector<uint32_t>& producer, Fallback::Topology topology,
                                     const Fallback::Limits& selected_limits, const char* reason) {
        const auto result = Fallback::BuildGeometry(producer, fragment.plan, topology, false, selected_limits);
        Check(!result.success, std::string("reject geometry ") + reason);
        Check(!result.reason.empty(), std::string("explain geometry ") + reason);
    };
    reject_geometry(vertex, Fallback::Topology::Unsupported, limits, "unsupported topology");
    reject_geometry(vertex, Fallback::Topology::TriangleStrip, limits, "strip must be canonicalized before GS input");
    reject_geometry(Vertex(false, false), Fallback::Topology::TriangleList, limits, "missing ordinary export");
    reject_geometry(Vertex(false, true, true), Fallback::Topology::TriangleList, limits, "ordinary export type mismatch");
    reject_geometry(Vertex(false, true, false, true), Fallback::Topology::TriangleList, limits, "unknown vertex output builtin");
    const auto point_size = Vertex(true, true, false, false, false, true);
    Validate(point_size, "vertex PointSize producer");
    reject_geometry(point_size, Fallback::Topology::TriangleList, limits, "GS PointSize feature is not enabled");
    const auto multiple_blocks = Vertex(false, true, false, false, false, false, false, true);
    reject_geometry(multiple_blocks, Fallback::Topology::TriangleList, limits, "multiple identically shaped builtin blocks");
    const auto layer_viewport = Vertex(true, true, false, false, false, false, false, false, true);
    Validate(layer_viewport, "vertex Layer/ViewportIndex producer");
    reject_geometry(layer_viewport, Fallback::Topology::TriangleList, limits, "Layer/ViewportIndex cannot be read as GS builtins");
    reject_geometry({}, Fallback::Topology::TriangleList, limits, "empty vertex shader");
    for (auto field : {&Fallback::Limits::max_geometry_input_components,
                       &Fallback::Limits::max_geometry_output_components,
                       &Fallback::Limits::max_geometry_total_output_components,
                       &Fallback::Limits::max_geometry_output_vertices}) {
        auto constrained = limits;
        constrained.*field = 2;
        reject_geometry(vertex, Fallback::Topology::TriangleList, constrained, "geometry device limit");
    }
}
void MeshLegacyVersionContracts() {
    // SPIR-V 1.3 lists only Input/Output variables in the entry interface.
    // Native fragment modules can still use descriptors, push data and private
    // globals; upgrading for SPV_EXT_mesh_shader must expose all of them.
    for (const bool raw_only : {false, true}) {
        std::string text = R"(
OpCapability Shader
OpCapability FragmentBarycentricKHR
OpExtension "SPV_KHR_fragment_shader_barycentric"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %input %color
OpExecutionMode %main OriginUpperLeft
OpDecorate %color Location 0
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %uniform DescriptorSet 0
OpDecorate %uniform Binding 0
)";
        text += raw_only ? "OpDecorate %input Location 3\nOpDecorate %input PerVertexKHR\n"
                         : "OpDecorate %input BuiltIn BaryCoordKHR\n";
        text += R"(
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%three = OpConstant %uint 3
%one = OpConstant %float 1
%vec3 = OpTypeVector %float 3
%vec4 = OpTypeVector %float 4
%zero4 = OpConstantNull %vec4
%array = OpTypeArray %vec4 %three
%block = OpTypeStruct %vec4
%uniformBlock = OpTypePointer Uniform %block
%uniform4 = OpTypePointer Uniform %vec4
%pushBlock = OpTypePointer PushConstant %block
%push4 = OpTypePointer PushConstant %vec4
%private4 = OpTypePointer Private %vec4
%input3 = OpTypePointer Input %vec3
%input4 = OpTypePointer Input %vec4
%inputArray = OpTypePointer Input %array
%output4 = OpTypePointer Output %vec4
%uniform = OpVariable %uniformBlock Uniform
%push = OpVariable %pushBlock PushConstant
%private = OpVariable %private4 Private %zero4
%color = OpVariable %output4 Output
)";
        text += raw_only ? "%input = OpVariable %inputArray Input\n"
                         : "%input = OpVariable %input3 Input\n";
        text += R"(
%main = OpFunction %void None %function
%entry = OpLabel
%uniformAddress = OpAccessChain %uniform4 %uniform %zero
%uniformValue = OpLoad %vec4 %uniformAddress
%pushAddress = OpAccessChain %push4 %push %zero
%pushValue = OpLoad %vec4 %pushAddress
%privateValue = OpLoad %vec4 %private
%sum = OpFAdd %vec4 %uniformValue %pushValue
%globals = OpFAdd %vec4 %sum %privateValue
)";
        text += raw_only
            ? "%rawAddress = OpAccessChain %input4 %input %zero\n%value = OpLoad %vec4 %rawAddress\n"
            : "%coordinate = OpLoad %vec3 %input\n"
              "%x = OpCompositeExtract %float %coordinate 0\n"
              "%y = OpCompositeExtract %float %coordinate 1\n"
              "%z = OpCompositeExtract %float %coordinate 2\n"
              "%value = OpCompositeConstruct %vec4 %x %y %z %one\n";
        text += "%result = OpFAdd %vec4 %value %globals\nOpStore %color %result\n"
                "OpReturn\nOpFunctionEnd\n";
        const auto source = Assemble(text, SPV_ENV_VULKAN_1_1);
        Check(source[1] == 0x00010300, "legacy fixture must actually be SPIR-V 1.3");
        Validate(source, "legacy fragment with global resources");
        std::vector<uint32_t> globals;
        for (size_t cursor = 5; cursor < source.size(); cursor += source[cursor] >> 16u) {
            const auto* instruction = source.data() + cursor;
            if ((instruction[0] & 0xffffu) == spv::OpVariable &&
                (instruction[3] == spv::StorageClassUniform ||
                 instruction[3] == spv::StorageClassPushConstant ||
                 instruction[3] == spv::StorageClassPrivate)) globals.push_back(instruction[2]);
        }
        Check(globals.size() == 3, "fixture must use three non-I/O globals");
        const auto fragment = Fallback::PrepareFragment(source, Limits());
        Check(fragment.status == Fallback::Status::Ready, "legacy fragment lowering: " + fragment.reason);
        const auto mesh = Fallback::PrepareMeshFragment(fragment.plan, Limits());
        Check(mesh.status == Fallback::Status::Ready, "legacy mesh lowering: " + mesh.reason);
        Check(mesh.plan.compact, "legacy center/raw-only fragment must use compact mesh transport");
        Check(mesh.plan.spirv[1] >= 0x00010400, "mesh fragment requires SPIR-V 1.4 or newer");
        Validate(mesh.plan.spirv, raw_only ? "legacy raw-only mesh fragment" : "legacy coordinate mesh fragment");
        bool found_entry = false;
        for (size_t cursor = 5; cursor < mesh.plan.spirv.size(); cursor += mesh.plan.spirv[cursor] >> 16u) {
            const auto* instruction = mesh.plan.spirv.data() + cursor;
            if ((instruction[0] & 0xffffu) != spv::OpEntryPoint) continue;
            found_entry = true;
            // The unchanged entry name "main" occupies exactly two words.
            const auto interfaces = std::span(instruction + 5, (instruction[0] >> 16u) - 5);
            for (const auto global : globals)
                Check(std::count(interfaces.begin(), interfaces.end(), global) == 1,
                      "upgraded entry must contain each existing global exactly once");
        }
        Check(found_entry, "upgraded mesh fragment lost its entry point");
    }
}

void MeshCompactContracts() {
    for (const unsigned modes : {1u, 2u, 3u}) {
        for (const char* qualifier : {"", "Centroid", "Sample"}) {
            const auto source = Fragment(modes, qualifier);
            Validate(source, "qualified mesh fragment source");
            const auto fragment = Fallback::PrepareFragment(source, Limits());
            Check(fragment.status == Fallback::Status::Ready, "qualified fragment lowering: " + fragment.reason);
            const auto mesh = Fallback::PrepareMeshFragment(fragment.plan, Limits());
            Check(mesh.status == Fallback::Status::Ready, "qualified mesh lowering: " + mesh.reason);
            const bool compact = modes != 3 && *qualifier == '\0';
            Check(mesh.plan.compact == compact,
                  "dual-mode, centroid and sample coordinates must retain homogeneous interpolation");
            Validate(mesh.plan.spirv, "qualified mesh fragment result");
            const auto text = Disassemble(mesh.plan.spirv);
            Check((text.find("BuiltIn FragCoord") != std::string::npos) == compact,
                  "only compact coordinates should introduce FragCoord");
            for (const auto& coordinate : fragment.plan.barycentric_inputs) {
                bool declared = false;
                for (size_t cursor = 5; cursor < mesh.plan.spirv.size(); cursor += mesh.plan.spirv[cursor] >> 16u) {
                    const auto* instruction = mesh.plan.spirv.data() + cursor;
                    if ((instruction[0] & 0xffffu) == spv::OpVariable && instruction[2] == coordinate.variable)
                        declared = true;
                }
                Check(declared != compact, "compact path must remove the original coordinate varying");
            }
            for (const auto location : mesh.plan.coefficient_locations) {
                uint32_t variable = 0, pointer = 0, vector = 0, components = 0;
                for (size_t cursor = 5; cursor < mesh.plan.spirv.size(); cursor += mesh.plan.spirv[cursor] >> 16u) {
                    const auto* instruction = mesh.plan.spirv.data() + cursor;
                    if ((instruction[0] & 0xffffu) == spv::OpDecorate && (instruction[0] >> 16u) == 4 &&
                        instruction[2] == spv::DecorationLocation && instruction[3] == location &&
                        HasDecoration(mesh.plan.spirv, instruction[1], spv::DecorationPerPrimitiveEXT))
                        variable = instruction[1];
                }
                for (size_t cursor = 5; cursor < mesh.plan.spirv.size(); cursor += mesh.plan.spirv[cursor] >> 16u) {
                    const auto* instruction = mesh.plan.spirv.data() + cursor;
                    if ((instruction[0] & 0xffffu) == spv::OpVariable && instruction[2] == variable)
                        pointer = instruction[1];
                }
                for (size_t cursor = 5; cursor < mesh.plan.spirv.size(); cursor += mesh.plan.spirv[cursor] >> 16u) {
                    const auto* instruction = mesh.plan.spirv.data() + cursor;
                    if ((instruction[0] & 0xffffu) == spv::OpTypePointer && instruction[1] == pointer)
                        vector = instruction[3];
                }
                for (size_t cursor = 5; cursor < mesh.plan.spirv.size(); cursor += mesh.plan.spirv[cursor] >> 16u) {
                    const auto* instruction = mesh.plan.spirv.data() + cursor;
                    if ((instruction[0] & 0xffffu) == spv::OpTypeVector && instruction[1] == vector)
                        components = instruction[3];
                }
                Check(variable != 0 && components == (compact ? 4u : 3u),
                      "inverse row interface must carry the compact scale or preserve its old shape");
            }
        }
    }
    const auto primitive_id = Fallback::PrepareFragment(
        Fragment(1, "", false, false, false, false, true), Limits());
    Check(primitive_id.status == Fallback::Status::Ready, "primitive ID remains supported by GS transport");
    Check(Fallback::PrepareMeshFragment(primitive_id.plan, Limits()).status == Fallback::Status::Unsupported,
          "mesh path must reject PrimitiveId until the producer supplies it");
    const auto center = Fallback::PrepareFragment(Fragment(), Limits());
    Check(center.status == Fallback::Status::Ready, "center fragment lowering");
    const auto sample_shading = Fallback::PrepareMeshFragment(center.plan, Limits(), false);
    Check(sample_shading.status == Fallback::Status::Ready && !sample_shading.plan.compact,
          "pipeline sample shading must retain the homogeneous coordinate path");
    Validate(sample_shading.plan.spirv, "sample-shading mesh fragment");
    for (const unsigned builtin : {1u, 2u}) {
        const auto source = Fragment(1, "", false, false, false, false, false, false, builtin);
        Validate(source, "sample builtin mesh fragment source");
        const auto fragment = Fallback::PrepareFragment(source, Limits());
        Check(fragment.status == Fallback::Status::Ready, "sample builtin fragment lowering");
        const auto mesh = Fallback::PrepareMeshFragment(fragment.plan, Limits());
        Check(mesh.status == Fallback::Status::Ready && !mesh.plan.compact,
              "sample builtin must preserve the homogeneous interpolation path");
        Validate(mesh.plan.spirv, "sample builtin mesh fragment");
    }
}
} // namespace

int main() {
    PrimitiveAssemblyContracts();
    PositiveContracts();
    BuiltinContracts();
    PerVertexContracts();
    MeshLegacyVersionContracts();
    MeshCompactContracts();
    NegativeContracts();
    std::printf("DirectBarycentricFallbackTests PASS: %u checks; primitive assembly and synthetic SPIR-V only\n", checks);
    return 0;
}
