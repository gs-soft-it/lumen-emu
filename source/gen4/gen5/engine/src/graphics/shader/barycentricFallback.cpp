// Optional geometry-assisted fragment barycentrics. SPDX-License-Identifier: GPL-2.0-only
#include "graphics/shader/barycentricFallback.h"

#include "graphics/host_gpu/spirvModuleAdmission.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvBuilder.h"

#include <spirv-tools/libspirv.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace Libs::Graphics::BarycentricFallback {
namespace {

constexpr uint32_t Absent = ~0u;
constexpr size_t MaxWords = 4u * 1024u * 1024u;

struct Decoration {
    uint32_t location = Absent;
    uint32_t builtin = Absent;
    uint32_t component = 0;
    bool flat = false;
    bool no_perspective = false;
    bool centroid = false;
    bool sample = false;
    bool invariant = false;
    bool block = false;
    bool per_vertex = false;
    bool unsupported = false;
};

struct Type {
    spv::Op opcode = spv::OpNop;
    std::vector<uint32_t> operands;
};

struct Variable {
    uint32_t type = 0;
    uint32_t storage = 0;
};

struct Module {
    std::span<const uint32_t> words;
    std::map<uint32_t, Type> types;
    std::map<uint32_t, uint32_t> constants;
    std::map<uint32_t, Variable> variables;
    std::map<uint32_t, Decoration> decorations;
    std::map<std::pair<uint32_t, uint32_t>, Decoration> members;
    std::set<uint32_t> interfaces;
    uint32_t entry_count = 0;
    uint32_t entry_id = 0;
    uint32_t model = Absent;
    bool grouped_decoration = false;
    bool transform_feedback = false;
    std::string reason;

    bool Fail(const char* message) { reason = message; return false; }

    const Decoration& Decor(uint32_t id) const {
        static const Decoration empty;
        const auto it = decorations.find(id);
        return it == decorations.end() ? empty : it->second;
    }

    const Decoration& Member(uint32_t type, uint32_t member) const {
        static const Decoration empty;
        const auto it = members.find({type, member});
        return it == members.end() ? empty : it->second;
    }

    const Type* GetType(uint32_t id) const {
        const auto it = types.find(id);
        return it == types.end() ? nullptr : &it->second;
    }

    uint32_t ValueType(const Variable& variable) const {
        const auto* type = GetType(variable.type);
        return type != nullptr && type->opcode == spv::OpTypePointer &&
                       type->operands.size() == 2 && type->operands[0] == variable.storage
                   ? type->operands[1] : 0;
    }

    bool ReadDecoration(Decoration& d, std::span<const uint32_t> operands) {
        if (operands.empty()) return Fail("truncated decoration");
        const auto value = [&]() { return operands.size() == 2 ? operands[1] : Absent; };
        const auto single = [&]() { return operands.size() == 1; };
        switch (operands[0]) {
        case spv::DecorationLocation:
            if (value() == Absent || d.location != Absent) return Fail("invalid location decoration");
            d.location = value(); break;
        case spv::DecorationBuiltIn:
            if (value() == Absent || d.builtin != Absent) return Fail("invalid builtin decoration");
            d.builtin = value(); break;
        case spv::DecorationComponent:
            if (value() == Absent) return Fail("invalid component decoration");
            d.component = value(); break;
        case spv::DecorationFlat: d.flat = single(); d.unsupported |= !single(); break;
        case spv::DecorationNoPerspective: d.no_perspective = single(); d.unsupported |= !single(); break;
        case spv::DecorationCentroid: d.centroid = single(); d.unsupported |= !single(); break;
        case spv::DecorationSample: d.sample = single(); d.unsupported |= !single(); break;
        case spv::DecorationInvariant: d.invariant = single(); d.unsupported |= !single(); break;
        case spv::DecorationBlock: d.block = single(); d.unsupported |= !single(); break;
        case spv::DecorationRelaxedPrecision: d.unsupported |= !single(); break;
        case spv::DecorationPerVertexKHR: d.per_vertex = single(); d.unsupported |= !single(); break;
        default: d.unsupported = true; break;
        }
        return true;
    }

    bool Parse(std::span<const uint32_t> input, spv::ExecutionModel expected) {
        words = input;
        if (input.size() > MaxWords ||
            AdmitSpirvModule(input, true) != SpirvModuleAdmission::Allowed)
            return Fail("malformed or oversized SPIR-V");
        bool in_function = false;
        bool entry_defined = false;
        uint32_t memory_models = 0;
        for (size_t cursor = 5; cursor < words.size();) {
            const auto count = words[cursor] >> spv::WordCountShift;
            const auto op = static_cast<spv::Op>(words[cursor] & spv::OpCodeMask);
            const auto w = words.subspan(cursor, count);
            switch (op) {
            case spv::OpEntryPoint: {
                if (count < 4) return Fail("truncated entry point");
                ++entry_count;
                model = w[1];
                entry_id = w[2];
                if (entry_id == 0 || entry_id >= words[3]) return Fail("invalid entry point ID");
                const auto* name = reinterpret_cast<const char*>(w.data() + 3);
                const auto capacity = (count - 3u) * 4u;
                size_t length = 0;
                while (length < capacity && name[length] != '\0') ++length;
                if (length == capacity || std::string_view(name, length) != "main")
                    return Fail("unsupported entry point name");
                const size_t first_interface = 3u + (length + 4u) / 4u;
                for (size_t i = first_interface; i < w.size(); ++i) {
                    if (w[i] == 0 || w[i] >= words[3] || !interfaces.insert(w[i]).second)
                        return Fail("invalid entry point interface");
                }
                break;
            }
            case spv::OpTypeFloat: case spv::OpTypeInt: case spv::OpTypeVector:
            case spv::OpTypeArray: case spv::OpTypeStruct: case spv::OpTypePointer:
                if (count < 2 || w[1] == 0 || w[1] >= words[3] ||
                    !types.emplace(w[1], Type{op, {w.begin() + 2, w.end()}}).second)
                    return Fail("invalid interface type declaration");
                break;
            case spv::OpConstant:
                if (count == 4) constants.emplace(w[2], w[3]);
                break;
            case spv::OpVariable:
                if (count < 4 || w[2] == 0 || w[2] >= words[3] ||
                    !variables.emplace(w[2], Variable{w[1], w[3]}).second)
                    return Fail("invalid variable declaration");
                if ((w[3] == spv::StorageClassInput || w[3] == spv::StorageClassOutput) && count != 4)
                    return Fail("interface variable has an initializer");
                break;
            case spv::OpMemoryModel:
                if (count != 3) return Fail("malformed memory model");
                ++memory_models;
                break;
            case spv::OpFunction:
                if (count != 5 || in_function) return Fail("malformed function declaration");
                in_function = true;
                entry_defined |= w[2] == entry_id;
                break;
            case spv::OpFunctionEnd:
                if (count != 1 || !in_function) return Fail("unmatched function end");
                in_function = false;
                break;
            case spv::OpDecorate:
                if (count < 3 || !ReadDecoration(decorations[w[1]], w.subspan(2))) return false;
                break;
            case spv::OpMemberDecorate:
                if (count < 4 || !ReadDecoration(members[{w[1], w[2]}], w.subspan(3))) return false;
                break;
            case spv::OpDecorateId: case spv::OpDecorationGroup:
            case spv::OpGroupDecorate: case spv::OpGroupMemberDecorate:
                grouped_decoration = true; break;
            case spv::OpCapability:
                transform_feedback |= w[1] == spv::CapabilityTransformFeedback;
                break;
            default: break;
            }
            cursor += count;
        }
        if (entry_count != 1 || model != expected) return Fail("expected one main entry point of the requested stage");
        if (in_function || !entry_defined || memory_models != 1) return Fail("incomplete module or entry point function");
        for (const auto& [id, d] : decorations) {
            if (d.per_vertex && (expected != spv::ExecutionModelFragment ||
                !variables.contains(id) || variables.at(id).storage != spv::StorageClassInput))
                return Fail("PerVertexKHR requires a standalone fragment input");
        }
        for (const auto& [key, d] : members) {
            if (d.per_vertex) return Fail("member-decorated PerVertexKHR is unsupported");
        }
        if (grouped_decoration) return Fail("grouped or ID-based decorations are unsupported");
        if (transform_feedback) return Fail("transform feedback is unsupported");
        for (const auto id : interfaces) {
            if (!variables.contains(id)) return Fail("entry point contains a non-variable interface");
        }
        return true;
    }

    // Only scalar/vector 32-bit user varyings are accepted. Built-in clip/cull
    // arrays are checked separately and never consume an arbitrary location span.
    uint32_t Components(uint32_t id, bool* integer = nullptr) const {
        const auto* type = GetType(id);
        if (type == nullptr) return 0;
        if (type->opcode == spv::OpTypeFloat && type->operands == std::vector<uint32_t>{32}) {
            if (integer != nullptr) *integer = false;
            return 1;
        }
        if (type->opcode == spv::OpTypeInt && type->operands.size() == 2 &&
            type->operands[0] == 32 && type->operands[1] <= 1) {
            if (integer != nullptr) *integer = true;
            return 1;
        }
        if (type->opcode == spv::OpTypeVector && type->operands.size() == 2 &&
            type->operands[1] >= 2 && type->operands[1] <= 4) {
            const auto* scalar = GetType(type->operands[0]);
            if (scalar != nullptr && (scalar->opcode == spv::OpTypeFloat || scalar->opcode == spv::OpTypeInt) &&
                Components(type->operands[0], integer) == 1) return type->operands[1];
        }
        return 0;
    }
};

bool Barycentric(uint32_t builtin) {
    return builtin == spv::BuiltInBaryCoordKHR || builtin == spv::BuiltInBaryCoordNoPerspKHR;
}

bool BarycentricExtension(std::span<const uint32_t> instruction) {
    const auto* text = reinterpret_cast<const char*>(instruction.data() + 1);
    // Framing and terminated extension strings were checked by admission.
    return std::string_view(text) == "SPV_KHR_fragment_shader_barycentric" ||
           std::string_view(text) == "SPV_NV_fragment_shader_barycentric";
}

bool Compatible(const Module& a, uint32_t a_id, const Module& b, uint32_t b_id) {
    const auto* x = a.GetType(a_id);
    const auto* y = b.GetType(b_id);
    if (x == nullptr || y == nullptr || x->opcode != y->opcode) return false;
    if (x->opcode == spv::OpTypeFloat || x->opcode == spv::OpTypeInt)
        return x->operands == y->operands;
    if (x->opcode == spv::OpTypeVector && x->operands.size() == 2 && y->operands.size() == 2)
        return x->operands[1] == y->operands[1] &&
               Compatible(a, x->operands[0], b, y->operands[0]);
    return false;
}

bool FitsLocation(uint32_t location, uint32_t components, uint32_t limit) {
    return uint64_t(location) * 4u + components <= limit;
}

uint32_t Array3Element(const Module& module, uint32_t id) {
    const auto* type = module.GetType(id);
    if (type == nullptr || type->opcode != spv::OpTypeArray || type->operands.size() != 2)
        return 0;
    const auto length = module.constants.find(type->operands[1]);
    return length != module.constants.end() && length->second == 3u &&
                   module.Components(type->operands[0]) != 0
               ? type->operands[0] : 0;
}

bool FitsArray3Location(uint32_t location, uint32_t components, uint32_t limit) {
    return uint64_t(location) * 4u + 8u + components <= limit;
}

bool ValidUser(const Module& module, uint32_t id, uint32_t limit, bool fragment) {
    const auto& variable = module.variables.at(id);
    const auto& d = module.Decor(id);
    bool integer = false;
    const auto components = module.Components(module.ValueType(variable), &integer);
    return components != 0 && d.location != Absent && d.builtin == Absent &&
           d.component == 0 && !d.block && !d.per_vertex && !d.unsupported && !(d.flat && d.no_perspective) &&
           !(fragment && integer && !d.flat) && FitsLocation(d.location, components, limit);
}

FragmentResult FragmentFailure(std::string reason) {
    return {Status::Unsupported, std::move(reason), {}};
}

GeometryResult GeometryFailure(std::string reason) {
    return {false, std::move(reason), {}};
}

struct Forward {
    uint32_t source = 0;
    uint32_t type = 0;
    Decoration decoration;
    bool flat = false;
    uint32_t input_location = Absent;
    bool per_vertex = false;
};

// Validate a vertex built-in without relying on implementation-defined struct
// layout. Blocks retain their member order and decorations at the VS/GS link.
bool ValidateBuiltin(const Module& vertex, uint32_t type, uint32_t builtin,
                     std::set<uint32_t>& builtins, uint32_t& components) {
    if (!builtins.insert(builtin).second) return false;
    bool integer = false;
    const auto width = vertex.Components(type, &integer);
    switch (builtin) {
    case spv::BuiltInPosition: components += 4; return width == 4 && !integer;
    // The runtime does not enable shaderTessellationAndGeometryPointSize.
    case spv::BuiltInPointSize: return false;
    // Vulkan permits these built-ins as geometry outputs, but not geometry
    // inputs. Forwarding them would require rewriting the vertex interface.
    case spv::BuiltInLayer: case spv::BuiltInViewportIndex:
        return false;
    case spv::BuiltInClipDistance: case spv::BuiltInCullDistance: {
        const auto* array = vertex.GetType(type);
        if (array == nullptr || array->opcode != spv::OpTypeArray || array->operands.size() != 2)
            return false;
        const auto length = vertex.constants.find(array->operands[1]);
        if (length == vertex.constants.end() || length->second == 0 || length->second > 8 ||
            vertex.Components(array->operands[0], &integer) != 1 || integer) return false;
        components += length->second;
        return true;
    }
    default: return false;
    }
}

class GeometryEmitter {
public:
    explicit GeometryEmitter(const Module& vertex) : source(vertex), builder(0x00010500u) {
        builder.RequireCapability(spv::CapabilityShader);
        builder.RequireCapability(spv::CapabilityGeometry);
        builder.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
        void_type = builder.Type(spv::OpTypeVoid);
        uint_type = builder.Type(spv::OpTypeInt, 32u, 0u);
        float_type = builder.Type(spv::OpTypeFloat, 32u);
    }

    std::vector<uint32_t> Emit(const std::vector<Forward>& forwards,
                               const FragmentPlan& fragment, bool last, uint32_t primitive_id_kind) {
        struct Pair { uint32_t input, output, type; bool flat; bool per_vertex; };
        std::vector<Pair> pairs;
        for (const auto& forward : forwards) {
            const auto type = CloneType(forward.type);
            const auto array_type = builder.Type(spv::OpTypeArray, type, Uint(3));
            const auto input = Interface(spv::StorageClassInput, array_type);
            const auto output_type = forward.per_vertex ? array_type : type;
            const auto output = Interface(spv::StorageClassOutput, output_type);
            auto input_decoration = forward.decoration;
            if (forward.input_location != Absent) input_decoration.location = forward.input_location;
            Decorate(input, input_decoration, false);
            Decorate(output, forward.decoration, true);
            pairs.push_back({input, output, output_type, forward.flat, forward.per_vertex});
        }
        const auto vec3 = builder.Type(spv::OpTypeVector, float_type, 3u);
        std::vector<uint32_t> bary_outputs;
        for (const auto& input : fragment.barycentric_inputs) {
            const auto variable = Interface(spv::StorageClassOutput, vec3);
            builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationLocation, input.location);
            if (input.no_perspective)
                builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationNoPerspective);
            bary_outputs.push_back(variable);
        }
        uint32_t primitive_input = 0;
        uint32_t primitive_output = 0;
        uint32_t primitive_type = 0;
        if (primitive_id_kind != 0) {
            primitive_type = builder.Type(spv::OpTypeInt, 32u, primitive_id_kind - 1u);
            primitive_input = Interface(spv::StorageClassInput, primitive_type);
            primitive_output = Interface(spv::StorageClassOutput, primitive_type);
            builder.AddAnnotation(spv::OpDecorate, primitive_input, spv::DecorationBuiltIn, spv::BuiltInPrimitiveId);
            builder.AddAnnotation(spv::OpDecorate, primitive_output, spv::DecorationBuiltIn, spv::BuiltInPrimitiveId);
        }
        const auto entry = builder.AllocateId();
        builder.AddEntryPoint(spv::ExecutionModelGeometry, entry, "main", interfaces);
        builder.AddExecutionMode(entry, spv::ExecutionModeTriangles);
        builder.AddExecutionMode(entry, spv::ExecutionModeOutputTriangleStrip);
        builder.AddExecutionMode(entry, spv::ExecutionModeOutputVertices, 3u);
        builder.AddExecutionMode(entry, spv::ExecutionModeInvocations, 1u);
        const auto function = builder.Type(spv::OpTypeFunction, void_type);
        builder.AddFunction(spv::OpFunction, void_type, entry, spv::FunctionControlMaskNone, function);
        builder.AddFunction(spv::OpLabel, builder.AllocateId());
        const auto zero = builder.Constant(spv::OpConstant, float_type, 0u);
        const auto one = builder.Constant(spv::OpConstant, float_type, std::bit_cast<uint32_t>(1.0f));
        for (uint32_t vertex = 0; vertex < 3; ++vertex) {
            for (const auto& pair : pairs) {
                if (pair.per_vertex) {
                    // PerVertexKHR exposes every raw corner independently of the
                    // producer's Flat decoration and the rasterization provoking vertex.
                    const auto value = Result(spv::OpLoad, pair.type, pair.input);
                    builder.AddFunction(spv::OpStore, pair.output, value);
                    continue;
                }
                const auto pointer = builder.Type(spv::OpTypePointer, spv::StorageClassInput, pair.type);
                const auto index = Uint(pair.flat ? (last ? 2u : 0u) : vertex);
                const auto address = Result(spv::OpAccessChain, pointer, pair.input, index);
                const auto value = Result(spv::OpLoad, pair.type, address);
                builder.AddFunction(spv::OpStore, pair.output, value);
            }
            const auto basis = builder.Constant(spv::OpConstantComposite, vec3,
                                                vertex == 0 ? one : zero,
                                                vertex == 1 ? one : zero,
                                                vertex == 2 ? one : zero);
            for (const auto output : bary_outputs) builder.AddFunction(spv::OpStore, output, basis);
            if (primitive_id_kind != 0) {
                const auto value = Result(spv::OpLoad, primitive_type, primitive_input);
                builder.AddFunction(spv::OpStore, primitive_output, value);
            }
            // EmitVertex makes output variables undefined; all are written again
            // for the next vertex, including flat values and clip/cull distances.
            builder.AddFunction(spv::OpEmitVertex);
        }
        builder.AddFunction(spv::OpEndPrimitive);
        builder.AddFunction(spv::OpReturn);
        builder.AddFunction(spv::OpFunctionEnd);
        return builder.Build();
    }

private:
    uint32_t Uint(uint32_t value) { return builder.Constant(spv::OpConstant, uint_type, value); }

    uint32_t Interface(spv::StorageClass storage, uint32_t type) {
        const auto pointer = builder.Type(spv::OpTypePointer, storage, type);
        const auto variable = builder.DefineGlobalVariable(pointer, storage);
        interfaces.push_back(variable);
        return variable;
    }

    template <typename... Args>
    uint32_t Result(spv::Op opcode, uint32_t type, Args... args) {
        const auto id = builder.AllocateId();
        builder.AddFunction(opcode, type, id, args...);
        return id;
    }

    void BuiltinCapability(uint32_t builtin) {
        if (builtin == spv::BuiltInClipDistance) builder.RequireCapability(spv::CapabilityClipDistance);
        if (builtin == spv::BuiltInCullDistance) builder.RequireCapability(spv::CapabilityCullDistance);
        if (builtin == spv::BuiltInPointSize) builder.RequireCapability(spv::CapabilityGeometryPointSize);
        if (builtin == spv::BuiltInViewportIndex) builder.RequireCapability(spv::CapabilityMultiViewport);
    }

    void Decorate(uint32_t variable, const Decoration& d, bool output) {
        if (d.location != Absent)
            builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationLocation, d.location);
        if (d.builtin != Absent) {
            BuiltinCapability(d.builtin);
            builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, d.builtin);
        }
        if (output && d.flat) builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationFlat);
        if (output && d.no_perspective) builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationNoPerspective);
        if (output && d.invariant) builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationInvariant);
    }

    uint32_t CloneType(uint32_t id) {
        if (const auto found = cloned.find(id); found != cloned.end()) return found->second;
        const auto& t = source.types.at(id);
        uint32_t result = 0;
        switch (t.opcode) {
        case spv::OpTypeFloat: case spv::OpTypeInt:
            result = builder.Type(t.opcode, std::span<const uint32_t>(t.operands)); break;
        case spv::OpTypeVector:
            result = builder.Type(t.opcode, CloneType(t.operands[0]), t.operands[1]); break;
        case spv::OpTypeArray:
            result = builder.Type(t.opcode, CloneType(t.operands[0]), Uint(source.constants.at(t.operands[1]))); break;
        case spv::OpTypeStruct: {
            std::vector<uint32_t> types;
            for (const auto member : t.operands) types.push_back(CloneType(member));
            result = builder.Type(t.opcode, std::span<const uint32_t>(types));
            builder.AddAnnotation(spv::OpDecorate, result, spv::DecorationBlock);
            for (uint32_t member = 0; member < t.operands.size(); ++member) {
                const auto& d = source.Member(id, member);
                BuiltinCapability(d.builtin);
                builder.AddAnnotation(spv::OpMemberDecorate, result, member, spv::DecorationBuiltIn, d.builtin);
                if (d.invariant)
                    builder.AddAnnotation(spv::OpMemberDecorate, result, member, spv::DecorationInvariant);
            }
            break;
        }
        default: break; // All reachable types were checked before emitting.
        }
        cloned.emplace(id, result);
        return result;
    }

    const Module& source;
    ShaderRecompiler::Spirv::Builder builder;
    std::map<uint32_t, uint32_t> cloned;
    std::vector<uint32_t> interfaces;
    uint32_t void_type = 0;
    uint32_t uint_type = 0;
    uint32_t float_type = 0;
};

} // namespace

FragmentResult PrepareFragment(std::span<const uint32_t> words, const Limits& limits) {
    const auto admission = AdmitSpirvModule(words, false);
    if (admission == SpirvModuleAdmission::Malformed) return FragmentFailure("malformed SPIR-V");
    if (admission == SpirvModuleAdmission::Allowed)
        return {Status::NotNeeded, {}, {{words.begin(), words.end()}, {}}};
    Module fragment;
    if (!fragment.Parse(words, spv::ExecutionModelFragment)) return FragmentFailure(fragment.reason);
    std::set<uint32_t> locations;
    std::set<uint32_t> source_locations;
    FragmentPlan plan;
    for (const auto& [id, variable] : fragment.variables) {
        if (variable.storage != spv::StorageClassInput) continue;
        if (!fragment.interfaces.contains(id)) return FragmentFailure("input is absent from the fragment entry point");
        const auto& d = fragment.Decor(id);
        if (Barycentric(d.builtin)) {
            bool integer = false;
            if (fragment.Components(fragment.ValueType(variable), &integer) != 3 || integer ||
                d.location != Absent || d.component != 0 || d.flat || d.block || d.per_vertex || d.unsupported ||
                (d.builtin == spv::BuiltInBaryCoordKHR && d.no_perspective))
                return FragmentFailure("unsupported barycentric input shape or decoration");
            plan.barycentric_inputs.push_back({id, 0, d.builtin == spv::BuiltInBaryCoordNoPerspKHR});
        } else if (d.per_vertex) {
            const auto element = Array3Element(fragment, fragment.ValueType(variable));
            if (element == 0 || d.builtin != Absent || d.location == Absent ||
                d.component != 0 || d.block || d.unsupported || d.no_perspective ||
                d.centroid || d.sample ||
                !FitsLocation(d.location, fragment.Components(element), limits.max_geometry_input_components) ||
                !source_locations.insert(d.location).second)
                return FragmentFailure("unsupported or overlapping PerVertexKHR input");
            plan.per_vertex_inputs.push_back({id, d.location, 0});
        } else if (d.builtin == Absent) {
            if (!ValidUser(fragment, id, limits.max_fragment_input_components, true) ||
                !locations.insert(d.location).second || !source_locations.insert(d.location).second)
                return FragmentFailure("unsupported or overlapping fragment input interface");
        }
    }
    // Member-decorated barycentrics and capability-only modules are deliberately
    // refused: stripping their requirement would not be a complete rewrite.
    if ((plan.barycentric_inputs.empty() && plan.per_vertex_inputs.empty()) ||
        plan.barycentric_inputs.size() > 2)
        return FragmentFailure("expected standalone barycentric or PerVertexKHR inputs");
    const auto capacity = std::min(limits.max_fragment_input_components,
                                   limits.max_geometry_output_components);
    for (auto& input : plan.per_vertex_inputs) {
        const auto type = fragment.ValueType(fragment.variables.at(input.variable));
        const auto width = fragment.Components(Array3Element(fragment, type));
        uint32_t location = 0;
        while (FitsArray3Location(location, width, capacity) &&
               (locations.contains(location) || locations.contains(location + 1u) ||
                locations.contains(location + 2u))) ++location;
        if (!FitsArray3Location(location, width, capacity))
            return FragmentFailure("no free three-location PerVertexKHR varying span");
        input.location = location;
        for (uint32_t corner = 0; corner < 3; ++corner) locations.insert(location + corner);
    }
    std::set<bool> interpolation_modes;
    for (auto& bary : plan.barycentric_inputs) {
        if (!interpolation_modes.insert(bary.no_perspective).second)
            return FragmentFailure("duplicate barycentric interpolation mode");
        uint32_t location = 0;
        while (FitsLocation(location, 3, capacity) && locations.contains(location)) ++location;
        if (!FitsLocation(location, 3, capacity)) return FragmentFailure("no free barycentric varying location");
        bary.location = location;
        locations.insert(location);
    }
    plan.spirv.assign(words.begin(), words.begin() + 5);
    for (size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        const auto opcode = words[cursor] & spv::OpCodeMask;
        const auto instruction = words.subspan(cursor, count);
        bool omit = opcode == spv::OpCapability && instruction[1] == spv::CapabilityFragmentBarycentricKHR;
        omit |= opcode == spv::OpExtension && BarycentricExtension(instruction);
        const auto bary = opcode == spv::OpDecorate && count == 4 && instruction[2] == spv::DecorationBuiltIn
            ? std::find_if(plan.barycentric_inputs.begin(), plan.barycentric_inputs.end(),
                           [&](const BarycentricInput& input) { return input.variable == instruction[1]; })
            : plan.barycentric_inputs.end();
        const auto per_vertex = opcode == spv::OpDecorate && count >= 3
            ? std::find_if(plan.per_vertex_inputs.begin(), plan.per_vertex_inputs.end(),
                           [&](const PerVertexInput& input) { return input.variable == instruction[1]; })
            : plan.per_vertex_inputs.end();
        if (per_vertex != plan.per_vertex_inputs.end() && instruction[2] == spv::DecorationPerVertexKHR) {
            if (!fragment.Decor(per_vertex->variable).flat)
                plan.spirv.insert(plan.spirv.end(), {(3u << spv::WordCountShift) | spv::OpDecorate,
                                                   per_vertex->variable, spv::DecorationFlat});
        } else if (per_vertex != plan.per_vertex_inputs.end() && instruction[2] == spv::DecorationLocation) {
            plan.spirv.insert(plan.spirv.end(), {(4u << spv::WordCountShift) | spv::OpDecorate,
                                               per_vertex->variable, spv::DecorationLocation, per_vertex->location});
        } else if (bary != plan.barycentric_inputs.end()) {
            plan.spirv.insert(plan.spirv.end(), {instruction[0], bary->variable,
                                               spv::DecorationLocation, bary->location});
            if (bary->no_perspective && !fragment.Decor(bary->variable).no_perspective)
                plan.spirv.insert(plan.spirv.end(), {(3u << spv::WordCountShift) | spv::OpDecorate,
                                                   bary->variable, spv::DecorationNoPerspective});
        } else if (!omit) {
            plan.spirv.insert(plan.spirv.end(), instruction.begin(), instruction.end());
        }
        cursor += count;
    }
    if (AdmitSpirvModule(plan.spirv, false) != SpirvModuleAdmission::Allowed)
        return FragmentFailure("barycentric requirements remain after rewriting");
    return {Status::Ready, {}, std::move(plan)};
}

MeshResult PrepareMeshFragment(const FragmentPlan& fragment, const Limits& limits,
                               bool allow_compact) {
    const auto fail = [](std::string reason) {
        return MeshResult{Status::Unsupported, std::move(reason), {}};
    };
    Module pixel;
    if (!pixel.Parse(fragment.spirv, spv::ExecutionModelFragment)) return fail(pixel.reason);
    if (AdmitSpirvModule(fragment.spirv, false) != SpirvModuleAdmission::Allowed)
        return fail("mesh fragment plan still requires native barycentrics");
    if ((fragment.barycentric_inputs.empty() && fragment.per_vertex_inputs.empty()) ||
        fragment.barycentric_inputs.size() > 2)
        return fail("mesh fragment requires a nonempty fallback plan");

    MeshPlan plan;
    plan.coordinate_inputs = fragment.barycentric_inputs;
    plan.raw_inputs = fragment.per_vertex_inputs;
    plan.compact = allow_compact && fragment.barycentric_inputs.size() <= 1 &&
        std::ranges::all_of(fragment.barycentric_inputs, [&](const auto& input) {
            const auto& decor = pixel.Decor(input.variable);
            return !decor.centroid && !decor.sample;
        });
    std::map<uint32_t, const BarycentricInput*> coordinates;
    std::map<uint32_t, const PerVertexInput*> raw_inputs;
    std::set<bool> modes;
    std::set<uint32_t> raw_sources;
    for (const auto& input : fragment.barycentric_inputs) {
        if (!coordinates.emplace(input.variable, &input).second ||
            !modes.insert(input.no_perspective).second)
            return fail("duplicate mesh coordinate input");
    }
    for (const auto& input : fragment.per_vertex_inputs) {
        if (coordinates.contains(input.variable) ||
            !raw_inputs.emplace(input.variable, &input).second ||
            !raw_sources.insert(input.source_location).second)
            return fail("duplicate mesh raw-corner input");
    }
    std::set<uint32_t> locations;
    uint32_t vector_type = 0;
    uint32_t scalar_type = 0;
    uint32_t pointer_type = 0;
    uint32_t frag_coord = 0;
    std::set<uint32_t> found_coordinates, found_raw;
    for (const auto& [id, variable] : pixel.variables) {
        if (variable.storage != spv::StorageClassInput) continue;
        if (!pixel.interfaces.contains(id)) return fail("mesh fragment input is absent from entry point");
        const auto& d = pixel.Decor(id);
        const auto type = pixel.ValueType(variable);
        if (const auto input = coordinates.find(id); input != coordinates.end()) {
            bool integer = false;
            if (!ValidUser(pixel, id, limits.max_fragment_input_components, true) ||
                pixel.Components(type, &integer) != 3 || integer || d.flat ||
                d.location != input->second->location ||
                d.no_perspective != input->second->no_perspective)
                return fail("incompatible mesh coordinate input");
            if (vector_type != 0 && type != vector_type)
                return fail("mesh coordinates require a common float32 vector type");
            vector_type = type;
            scalar_type = pixel.GetType(type)->operands[0];
            pointer_type = variable.type;
            found_coordinates.insert(id);
        } else if (const auto input = raw_inputs.find(id); input != raw_inputs.end()) {
            const auto element = Array3Element(pixel, type);
            bool integer = false;
            if (element == 0 || d.builtin != Absent || d.location != input->second->location ||
                d.component != 0 || d.block || d.per_vertex || d.unsupported || !d.flat ||
                d.no_perspective || d.centroid || d.sample ||
                !FitsArray3Location(d.location, pixel.Components(element), limits.max_fragment_input_components))
                return fail("incompatible mesh raw-corner input");
            if (pixel.Components(element, &integer) != 4 || integer)
                return fail("mesh raw-corner input requires an array of three float32 vec4 values");
            for (uint32_t corner = 0; corner < 3; ++corner) {
                if (!locations.insert(d.location + corner).second)
                    return fail("overlapping mesh raw-corner input span");
            }
            found_raw.insert(id);
            continue;
        } else if (d.builtin != Absent) {
            if (d.builtin == spv::BuiltInPrimitiveId)
                return fail("mesh barycentric fallback does not publish PrimitiveId");
            if (d.builtin == spv::BuiltInSampleId || d.builtin == spv::BuiltInSamplePosition)
                plan.compact = false;
            if (d.builtin == spv::BuiltInFragCoord) {
                if (frag_coord != 0) return fail("duplicate mesh fragment FragCoord input");
                frag_coord = id;
            }
            continue;
        } else {
            if (!ValidUser(pixel, id, limits.max_fragment_input_components, true))
                return fail("unsupported ordinary mesh fragment input");
            bool integer = false;
            if (pixel.Components(type, &integer) != 4 || integer)
                return fail("ordinary mesh fragment input requires float32 vec4");
            plan.ordinary_locations.push_back(d.location);
        }
        if (!locations.insert(d.location).second) return fail("overlapping mesh fragment input locations");
    }
    if (found_coordinates.size() != coordinates.size() || found_raw.size() != raw_inputs.size())
        return fail("mesh fragment plan references missing inputs");
    if (plan.compact) {
        for (const auto& input : plan.coordinate_inputs) locations.erase(input.location);
    }
    if (!coordinates.empty()) {
        const auto width = plan.compact ? 4u : 3u;
        for (auto& location : plan.coefficient_locations) {
            location = 0;
            while (FitsLocation(location, width, limits.max_fragment_input_components) &&
                   locations.contains(location)) ++location;
            if (!FitsLocation(location, width, limits.max_fragment_input_components))
                return fail("no free mesh inverse-coefficient input location");
            locations.insert(location);
        }
    }
    std::sort(plan.ordinary_locations.begin(), plan.ordinary_locations.end());

    // Every supported load is replaced at its original execution point;
    // pointer escapes are denied. Compact plans can then remove the original
    // coordinate variables and derived pointers without changing other uses.
    // BinaryParse distinguishes ID operands from equal-valued literal words.
    struct Pointer { uint32_t variable; uint32_t component; };
    std::map<uint32_t, Pointer> pointers;
    for (const auto& [id, input] : coordinates) pointers.emplace(id, Pointer{id, Absent});
    const auto words = std::span<const uint32_t>(fragment.spirv);
    bool has_mesh_capability = false;
    bool has_mesh_extension = false;
    for (size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        const auto op = static_cast<spv::Op>(words[cursor] & spv::OpCodeMask);
        const auto w = words.subspan(cursor, count);
        if (op == spv::OpCapability && w[1] == spv::CapabilityMeshShadingEXT)
            has_mesh_capability = true;
        if (op == spv::OpExtension &&
            std::string_view(reinterpret_cast<const char*>(w.data() + 1)) == "SPV_EXT_mesh_shader")
            has_mesh_extension = true;
        if ((op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain || op == spv::OpCopyObject) &&
            count >= 4 && pointers.contains(w[3])) {
            auto pointer = pointers.at(w[3]);
            if (op == spv::OpCopyObject) {
                if (count != 4) return fail("unsupported mesh coordinate pointer copy");
            } else {
                if (count != 5 || pointer.component != Absent)
                    return fail("unsupported mesh coordinate access chain");
                pointer.component = w[4];
            }
            const auto* type = pixel.GetType(w[1]);
            const auto expected = pointer.component == Absent ? vector_type : scalar_type;
            if (type == nullptr || type->opcode != spv::OpTypePointer ||
                type->operands != std::vector<uint32_t>{spv::StorageClassInput, expected})
                return fail("incompatible mesh coordinate pointer type");
            pointers.emplace(w[2], pointer);
        }
        if (op == spv::OpLoad && count >= 4 && pointers.contains(w[3])) {
            const auto pointer = pointers.at(w[3]);
            if (count != 4 || w[1] != (pointer.component == Absent ? vector_type : scalar_type))
                return fail("unsupported mesh coordinate load");
        }
        cursor += count;
    }
    struct Uses { const std::map<uint32_t, Pointer>& pointers; bool valid = true; } uses{pointers};
    const auto context = spvContextCreate(SPV_ENV_VULKAN_1_3);
    if (context == nullptr) return fail("cannot parse mesh fragment pointer uses");
    spv_diagnostic diagnostic = nullptr;
    const auto parsed = spvBinaryParse(context, &uses, words.data(), words.size(), nullptr,
        [](void* opaque, const spv_parsed_instruction_t* instruction) -> spv_result_t {
            auto& state = *static_cast<Uses*>(opaque);
            const auto op = static_cast<spv::Op>(instruction->opcode);
            for (uint32_t i = 0; i < instruction->num_operands; ++i) {
                const auto& operand = instruction->operands[i];
                if (operand.type != SPV_OPERAND_TYPE_ID || operand.num_words != 1 ||
                    !state.pointers.contains(instruction->words[operand.offset])) continue;
                const bool metadata = op == spv::OpName || op == spv::OpDecorate || op == spv::OpEntryPoint;
                const bool read = op == spv::OpLoad && operand.offset == 3;
                const bool derived = (op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain ||
                                      op == spv::OpCopyObject) && operand.offset == 3 &&
                                     state.pointers.contains(instruction->result_id);
                if (!metadata && !read && !derived) {
                    state.valid = false;
                    return SPV_ERROR_INVALID_ID;
                }
            }
            return SPV_SUCCESS;
        }, &diagnostic);
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    if (parsed != SPV_SUCCESS || !uses.valid)
        return fail("mesh coordinate pointer escapes supported loads and access chains");

    uint32_t next_id = words[3];
    // Bound growth before reserving IDs, including per-load arithmetic.
    if (uint64_t(next_id) + uint64_t(words.size()) * 12u + 8u >= UINT32_MAX)
        return fail("mesh fragment ID space is exhausted");
    const auto fresh = [&]() { return next_id++; };
    std::array<uint32_t, 2> coefficient_variables {};
    uint32_t one = 0;
    uint32_t coefficient_type = vector_type;
    uint32_t coefficient_pointer_type = pointer_type;
    bool create_coefficient_type = false;
    bool create_coefficient_pointer_type = false;
    bool create_frag_coord = false;
    if (!coordinates.empty()) {
        for (auto& id : coefficient_variables) id = fresh();
        one = fresh();
        if (plan.compact) {
            coefficient_type = 0;
            if (frag_coord != 0) {
                coefficient_type = pixel.ValueType(pixel.variables.at(frag_coord));
                const auto* type = pixel.GetType(coefficient_type);
                if (type == nullptr || type->opcode != spv::OpTypeVector ||
                    type->operands != std::vector<uint32_t>{scalar_type, 4u})
                    return fail("compact mesh coordinates require compatible float32 vec4 FragCoord");
                coefficient_pointer_type = pixel.variables.at(frag_coord).type;
            } else {
                for (const auto& [id, type] : pixel.types) {
                    if (type.opcode == spv::OpTypeVector &&
                        type.operands == std::vector<uint32_t>{scalar_type, 4u}) {
                        coefficient_type = id;
                        break;
                    }
                }
                if (coefficient_type == 0) {
                    coefficient_type = fresh();
                    create_coefficient_type = true;
                }
                coefficient_pointer_type = 0;
                for (const auto& [id, type] : pixel.types) {
                    if (type.opcode == spv::OpTypePointer &&
                        type.operands == std::vector<uint32_t>{spv::StorageClassInput, coefficient_type}) {
                        coefficient_pointer_type = id;
                        break;
                    }
                }
                if (coefficient_pointer_type == 0) {
                    coefficient_pointer_type = fresh();
                    create_coefficient_pointer_type = true;
                }
                frag_coord = fresh();
                create_frag_coord = true;
            }
        }
    }
    // MeshShadingEXT requires SPIR-V 1.4. Its entry-point interface must also
    // list referenced globals outside Input/Output; this single-entry module
    // can conservatively publish every non-Function variable, as Builder does.
    std::vector<uint32_t> added_interfaces;
    for (const auto& [id, variable] : pixel.variables) {
        if (variable.storage != spv::StorageClassFunction && !pixel.interfaces.contains(id) &&
            !(plan.compact && coordinates.contains(id)))
            added_interfaces.push_back(id);
    }
    if (!coordinates.empty())
        added_interfaces.insert(added_interfaces.end(), coefficient_variables.begin(), coefficient_variables.end());
    if (create_frag_coord) added_interfaces.push_back(frag_coord);
    auto& output = plan.spirv;
    output.assign(words.begin(), words.begin() + 5);
    output[1] = std::max(output[1], 0x00010400u);
    const auto emit = [&](spv::Op op, std::initializer_list<uint32_t> operands) {
        output.push_back((static_cast<uint32_t>(operands.size() + 1u) << spv::WordCountShift) | op);
        output.insert(output.end(), operands.begin(), operands.end());
    };
    if (!has_mesh_capability) emit(spv::OpCapability, {spv::CapabilityMeshShadingEXT});
    bool extension_inserted = has_mesh_extension;
    bool annotations_inserted = false;
    bool globals_inserted = false;
    for (size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        const auto op = static_cast<spv::Op>(words[cursor] & spv::OpCodeMask);
        const auto w = words.subspan(cursor, count);
        if (!extension_inserted && op != spv::OpCapability) {
            constexpr char extension[] = "SPV_EXT_mesh_shader";
            std::array<uint32_t, (sizeof(extension) + 3u) / 4u> encoded {};
            std::memcpy(encoded.data(), extension, sizeof(extension));
            output.push_back((static_cast<uint32_t>(encoded.size() + 1u) << spv::WordCountShift) | spv::OpExtension);
            output.insert(output.end(), encoded.begin(), encoded.end());
            extension_inserted = true;
        }
        if (!annotations_inserted && op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer) {
            if (create_frag_coord)
                emit(spv::OpDecorate, {frag_coord, spv::DecorationBuiltIn, spv::BuiltInFragCoord});
            for (uint32_t i = 0; i < coefficient_variables.size() && !coordinates.empty(); ++i) {
                emit(spv::OpDecorate, {coefficient_variables[i], spv::DecorationLocation, plan.coefficient_locations[i]});
                emit(spv::OpDecorate, {coefficient_variables[i], spv::DecorationFlat});
                emit(spv::OpDecorate, {coefficient_variables[i], spv::DecorationPerPrimitiveEXT});
            }
            for (const auto& input : plan.raw_inputs)
                emit(spv::OpDecorate, {input.variable, spv::DecorationPerPrimitiveEXT});
            annotations_inserted = true;
        }
        if (!globals_inserted && op == spv::OpFunction) {
            if (!coordinates.empty()) {
                if (create_coefficient_type)
                    emit(spv::OpTypeVector, {coefficient_type, scalar_type, 4u});
                if (create_coefficient_pointer_type)
                    emit(spv::OpTypePointer, {coefficient_pointer_type, spv::StorageClassInput, coefficient_type});
                emit(spv::OpConstant, {scalar_type, one, std::bit_cast<uint32_t>(1.0f)});
                if (create_frag_coord)
                    emit(spv::OpVariable, {coefficient_pointer_type, frag_coord, spv::StorageClassInput});
                for (const auto id : coefficient_variables)
                    emit(spv::OpVariable, {coefficient_pointer_type, id, spv::StorageClassInput});
            }
            globals_inserted = true;
        }
        if (op == spv::OpEntryPoint) {
            // Parse accepts exactly one entry named "main", whose name takes
            // two words. Keep its header and all surviving interface IDs.
            std::vector<uint32_t> entry(w.begin() + 1, w.begin() + 5);
            for (size_t i = 5; i < w.size(); ++i) {
                if (!(plan.compact && coordinates.contains(w[i]))) entry.push_back(w[i]);
            }
            entry.insert(entry.end(), added_interfaces.begin(), added_interfaces.end());
            if (entry.size() + 1u > 0xffffu)
                return fail("mesh fragment entry-point interface is oversized");
            output.push_back((static_cast<uint32_t>(entry.size() + 1u) << spv::WordCountShift) |
                             spv::OpEntryPoint);
            output.insert(output.end(), entry.begin(), entry.end());
        } else if (plan.compact &&
                   (((op == spv::OpName || op == spv::OpDecorate) && pointers.contains(w[1])) ||
                    (op == spv::OpVariable && coordinates.contains(w[2])) ||
                    ((op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain ||
                      op == spv::OpCopyObject) && pointers.contains(w[2])))) {
            // All uses have been proved to be rewritten loads, pointer
            // derivations or metadata. No input survives for the compact base.
        } else if (op == spv::OpLoad && pointers.contains(w[3])) {
            const auto pointer = pointers.at(w[3]);
            const auto coordinate = fresh();
            uint32_t reciprocal_w = 0;
            if (plan.compact) {
                const auto position = fresh();
                const auto x = fresh();
                const auto y = fresh();
                emit(spv::OpLoad, {coefficient_type, position, frag_coord});
                emit(spv::OpCompositeExtract, {scalar_type, x, position, 0u});
                emit(spv::OpCompositeExtract, {scalar_type, y, position, 1u});
                emit(spv::OpCompositeConstruct, {vector_type, coordinate, x, y, one});
                if (!coordinates.at(pointer.variable)->no_perspective) {
                    reciprocal_w = fresh();
                    emit(spv::OpCompositeExtract, {scalar_type, reciprocal_w, position, 3u});
                }
            } else {
                emit(spv::OpLoad, {vector_type, coordinate, pointer.variable});
            }
            std::array<uint32_t, 2> bary {};
            for (uint32_t i = 0; i < 2; ++i) {
                const auto row = fresh();
                bary[i] = fresh();
                emit(spv::OpLoad, {coefficient_type, row, coefficient_variables[i]});
                if (plan.compact) {
                    const auto xyz = fresh();
                    const auto scale = fresh();
                    const auto weight = fresh();
                    emit(spv::OpVectorShuffle, {vector_type, xyz, row, row, 0u, 1u, 2u});
                    emit(spv::OpCompositeExtract, {scalar_type, scale, row, 3u});
                    emit(spv::OpDot, {scalar_type, weight, xyz, coordinate});
                    if (coordinates.at(pointer.variable)->no_perspective) {
                        emit(spv::OpFMul, {scalar_type, bary[i], weight, scale});
                    } else {
                        const auto denominator = fresh();
                        emit(spv::OpFMul, {scalar_type, denominator, scale, reciprocal_w});
                        emit(spv::OpFDiv, {scalar_type, bary[i], weight, denominator});
                    }
                } else {
                    emit(spv::OpDot, {scalar_type, bary[i], row, coordinate});
                }
            }
            const auto remainder = fresh();
            const auto last = fresh();
            emit(spv::OpFSub, {scalar_type, remainder, one, bary[0]});
            emit(spv::OpFSub, {scalar_type, last, remainder, bary[1]});
            const auto value = pointer.component == Absent ? w[2] : fresh();
            emit(spv::OpCompositeConstruct, {vector_type, value, bary[0], bary[1], last});
            if (pointer.component != Absent)
                emit(spv::OpVectorExtractDynamic, {scalar_type, w[2], value, pointer.component});
        } else {
            output.insert(output.end(), w.begin(), w.end());
        }
        cursor += count;
    }
    if (!annotations_inserted || !globals_inserted || output.size() > MaxWords)
        return fail("mesh fragment rewrite is incomplete or oversized");
    output[3] = next_id;
    if (AdmitSpirvModule(output, false) != SpirvModuleAdmission::Allowed)
        return fail("mesh fragment requirements remain after rewriting");
    return {Status::Ready, {}, std::move(plan)};
}

GeometryResult BuildGeometry(std::span<const uint32_t> vertex_spirv,
                             const FragmentPlan& fragment, Topology topology,
                             bool provoking_vertex_last, const Limits& limits) {
    // Strip input order differs between drivers; the renderer must expand it
    // before the GS. PrimitiveId parity cannot recover order after restart.
    if (topology != Topology::TriangleList)
        return GeometryFailure("only triangle lists are qualified; expand strips before the GS");
    if (limits.max_geometry_output_vertices < 3) return GeometryFailure("geometry output vertex limit is insufficient");
    if ((fragment.barycentric_inputs.empty() && fragment.per_vertex_inputs.empty()) ||
        fragment.barycentric_inputs.size() > 2)
        return GeometryFailure("invalid fragment fallback plan");
    Module vertex;
    Module pixel;
    if (!vertex.Parse(vertex_spirv, spv::ExecutionModelVertex)) return GeometryFailure(vertex.reason);
    if (!pixel.Parse(fragment.spirv, spv::ExecutionModelFragment)) return GeometryFailure(pixel.reason);
    if (AdmitSpirvModule(fragment.spirv, false) != SpirvModuleAdmission::Allowed)
        return GeometryFailure("fragment plan still requires native barycentrics");
    std::map<uint32_t, uint32_t> bary_ids;
    std::set<uint32_t> bary_locations;
    std::set<bool> bary_modes;
    for (const auto& bary : fragment.barycentric_inputs) {
        const auto found = pixel.variables.find(bary.variable);
        if (found == pixel.variables.end() || found->second.storage != spv::StorageClassInput ||
            !pixel.interfaces.contains(bary.variable) || !bary_ids.emplace(bary.variable, bary.location).second ||
            !bary_locations.insert(bary.location).second || !bary_modes.insert(bary.no_perspective).second)
            return GeometryFailure("fragment plan does not match its input variables");
        bool integer = false;
        const auto& d = pixel.Decor(bary.variable);
        if (!ValidUser(pixel, bary.variable, limits.max_fragment_input_components, true) ||
            pixel.Components(pixel.ValueType(found->second), &integer) != 3 || integer || d.flat ||
            d.location != bary.location || d.no_perspective != bary.no_perspective ||
            !FitsLocation(d.location, 3, limits.max_geometry_output_components))
            return GeometryFailure("fragment plan has incompatible barycentric varyings");
    }
    std::map<uint32_t, const PerVertexInput*> per_vertex_ids;
    std::set<uint32_t> per_vertex_sources;
    for (const auto& input : fragment.per_vertex_inputs) {
        const auto found = pixel.variables.find(input.variable);
        if (found == pixel.variables.end() || found->second.storage != spv::StorageClassInput ||
            !pixel.interfaces.contains(input.variable) || bary_ids.contains(input.variable) ||
            !per_vertex_ids.emplace(input.variable, &input).second ||
            !per_vertex_sources.insert(input.source_location).second)
            return GeometryFailure("fragment plan does not match its PerVertexKHR variables");
        const auto element = Array3Element(pixel, pixel.ValueType(found->second));
        const auto& d = pixel.Decor(input.variable);
        if (element == 0 || d.location != input.location || d.builtin != Absent ||
            d.component != 0 || d.block || d.per_vertex || d.unsupported || !d.flat ||
            d.no_perspective || d.centroid || d.sample ||
            !FitsArray3Location(d.location, pixel.Components(element), limits.max_fragment_input_components) ||
            !FitsArray3Location(d.location, pixel.Components(element), limits.max_geometry_output_components))
            return GeometryFailure("fragment plan has incompatible flat PerVertexKHR arrays");
    }
    std::map<uint32_t, uint32_t> exports;
    std::vector<Forward> forwards;
    std::set<uint32_t> builtins;
    uint32_t builtin_blocks = 0;
    uint32_t input_components = 0;
    uint32_t output_components = 0;
    for (const auto& [id, variable] : vertex.variables) {
        if (variable.storage != spv::StorageClassOutput) continue;
        if (!vertex.interfaces.contains(id)) return GeometryFailure("vertex output is absent from entry point");
        const auto& d = vertex.Decor(id);
        const auto type_id = vertex.ValueType(variable);
        const auto* type = vertex.GetType(type_id);
        if (d.location != Absent) {
            if (d.builtin != Absent || !exports.emplace(d.location, id).second)
                return GeometryFailure("overlapping vertex output locations");
            continue; // Unconsumed vertex varyings do not enter the geometry interface.
        }
        if (type == nullptr || d.component != 0 || d.unsupported)
            return GeometryFailure("unsupported vertex builtin interface");
        uint32_t components = 0;
        if (d.builtin != Absent) {
            if (!ValidateBuiltin(vertex, type_id, d.builtin, builtins, components))
                return GeometryFailure("unsupported vertex builtin type");
        } else if (type->opcode == spv::OpTypeStruct && vertex.Decor(type_id).block &&
                   !vertex.Decor(type_id).unsupported && !type->operands.empty()) {
            // Builder interns structurally identical types. Keep one built-in
            // block so distinct blocks with identical shapes cannot acquire
            // conflicting member BuiltIn decorations during type cloning.
            if (++builtin_blocks > 1)
                return GeometryFailure("multiple vertex builtin blocks are unsupported");
            for (uint32_t member = 0; member < type->operands.size(); ++member) {
                const auto& md = vertex.Member(type_id, member);
                if (md.unsupported || md.location != Absent || md.component != 0 || md.flat ||
                    md.no_perspective || md.centroid || md.sample || md.block ||
                    !ValidateBuiltin(vertex, type->operands[member], md.builtin, builtins, components))
                    return GeometryFailure("unsupported builtin block member");
            }
        } else {
            return GeometryFailure("vertex output has neither a supported builtin nor location");
        }
        input_components += components;
        output_components += components;
        forwards.push_back({id, type_id, d, false});
    }
    if (!builtins.contains(spv::BuiltInPosition)) return GeometryFailure("vertex position output is missing");
    uint32_t primitive_id_kind = 0;
    std::set<uint32_t> pixel_locations;
    for (const auto& [id, variable] : pixel.variables) {
        if (variable.storage != spv::StorageClassInput) continue;
        if (!pixel.interfaces.contains(id)) return GeometryFailure("fragment input is absent from entry point");
        const auto& d = pixel.Decor(id);
        if (d.builtin != Absent) {
            if (d.builtin == spv::BuiltInPrimitiveId) {
                bool integer = false;
                if (pixel.Components(pixel.ValueType(variable), &integer) != 1 || !integer)
                    return GeometryFailure("unsupported fragment primitive ID type");
                primitive_id_kind = pixel.GetType(pixel.ValueType(variable))->operands[1] + 1u;
            }
            continue;
        }
        if (const auto planned = per_vertex_ids.find(id); planned != per_vertex_ids.end()) {
            for (uint32_t corner = 0; corner < 3; ++corner) {
                if (!pixel_locations.insert(d.location + corner).second)
                    return GeometryFailure("overlapping PerVertexKHR varying span");
            }
            const auto& input = *planned->second;
            const auto exported = exports.find(input.source_location);
            if (exported == exports.end() ||
                !ValidUser(vertex, exported->second, limits.max_geometry_input_components, false))
                return GeometryFailure("PerVertexKHR varying is missing from the vertex interface");
            const auto type = vertex.ValueType(vertex.variables.at(exported->second));
            if (!Compatible(vertex, type, pixel, Array3Element(pixel, pixel.ValueType(variable))))
                return GeometryFailure("vertex and PerVertexKHR element types do not match");
            auto output_decoration = d;
            output_decoration.invariant |= vertex.Decor(exported->second).invariant;
            forwards.push_back({exported->second, type, output_decoration, true,
                                input.source_location, true});
            input_components += vertex.Components(type);
            output_components += vertex.Components(type) * 3u;
            continue;
        }
        if (!ValidUser(pixel, id, limits.max_fragment_input_components, true) ||
            !pixel_locations.insert(d.location).second)
            return GeometryFailure("unsupported fragment input interface");
        if (bary_ids.contains(id)) continue;
        const auto exported = exports.find(d.location);
        if (exported == exports.end() || !ValidUser(vertex, exported->second, limits.max_geometry_input_components, false))
            return GeometryFailure("fragment varying is missing from the vertex interface");
        const auto& vs_variable = vertex.variables.at(exported->second);
        const auto type = vertex.ValueType(vs_variable);
        if (!Compatible(vertex, type, pixel, pixel.ValueType(variable)))
            return GeometryFailure("vertex and fragment varying types do not match");
        const auto width = vertex.Components(type);
        if (!FitsLocation(d.location, width, limits.max_geometry_output_components))
            return GeometryFailure("geometry output location exceeds the device limit");
        auto output_decoration = d;
        output_decoration.invariant |= vertex.Decor(exported->second).invariant;
        forwards.push_back({exported->second, type, output_decoration, d.flat});
        input_components += width;
        output_components += width;
    }
    output_components += static_cast<uint32_t>(fragment.barycentric_inputs.size()) * 3u;
    if (primitive_id_kind != 0) { ++input_components; ++output_components; }
    if (input_components > limits.max_geometry_input_components ||
        output_components > limits.max_geometry_output_components ||
        uint64_t(output_components) * 3u > limits.max_geometry_total_output_components)
        return GeometryFailure("geometry component limits are insufficient");
    GeometryEmitter emitter(vertex);
    return {true, {}, emitter.Emit(forwards, fragment, provoking_vertex_last, primitive_id_kind)};
}

} // namespace Libs::Graphics::BarycentricFallback
