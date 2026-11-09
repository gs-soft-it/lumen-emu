// SPDX-License-Identifier: GPL-2.0-only
#include "reader.h"

#include <algorithm>
#include <bit>
#include <string_view>
#include <unordered_map>
#include <spirv/unified1/spirv.hpp>

namespace Libs::Graphics::SpirvBinary {
namespace {

#include "lumenSpirvOperandGrammar.inc"

constexpr std::size_t MaxWords = 4u * 1024u * 1024u;
constexpr uint32_t MaxIdBound = 0x003fffffu;
constexpr unsigned MaxOperandNesting = 16;

const Instruction* FindOpcode(std::span<const Instruction> table, uint32_t opcode) {
    const auto it = std::ranges::lower_bound(table, opcode, {}, &Instruction::opcode);
    return it != table.end() && it->opcode == opcode ? &*it : nullptr;
}

struct IdInfo {
    uint32_t result_type = 0;
    uint32_t scalar_width = 0;
    bool defined = false;
    bool glsl_import = false;
};

struct Reader {
    uint32_t bound;
    IdUseVisitor visitor;
    void* context;
    std::unordered_map<uint32_t, IdInfo> ids;
    std::span<const uint32_t> instruction;
    std::size_t position = 1;
    uint32_t opcode = 0;
    uint32_t result_type = 0;
    uint32_t result_id = 0;

    bool ValidId(uint32_t id) const { return id != 0 && id < bound; }

    uint32_t ScalarWords(uint32_t type) const {
        const auto it = ids.find(type);
        if (it == ids.end() || it->second.scalar_width == 0) return 0;
        return (it->second.scalar_width - 1) / 32 + 1;
    }

    bool ReadOperands(std::span<const Operand> operands, unsigned depth) {
        if (depth > MaxOperandNesting) return false;
        for (const auto [kind, repeat] : operands) {
            if (repeat == Repeat::One) {
                if (!ReadOperand(kind, depth)) return false;
            } else if (repeat == Repeat::Optional) {
                if (position < instruction.size() && !ReadOperand(kind, depth)) return false;
            } else {
                while (position < instruction.size()) {
                    if (!ReadOperand(kind, depth)) return false;
                }
            }
        }
        return true;
    }

    bool ReadOperand(Kind kind, unsigned depth) {
        if (position >= instruction.size()) return false;
        const std::size_t offset = position++;
        const uint32_t value = instruction[offset];
        const auto& rule = KindRules[static_cast<unsigned>(kind)];
        switch (rule.category) {
        case Category::Id:
            if (!ValidId(value)) return false;
            if (kind == Kind::IdResult) {
                result_id = value;
            } else {
                ids.try_emplace(value);
                if (kind == Kind::IdResultType) {
                    result_type = value;
                } else if (visitor && !visitor(context, instruction, offset)) {
                    return false;
                }
            }
            return true;
        case Category::Literal:
            return ReadLiteral(kind, value, offset, depth);
        case Category::Composite:
            if (opcode == spv::OpSwitch && kind == Kind::PairLiteralIntegerIdRef) {
                // The selector's type determines the width of each case literal.
                const auto selector = ids.find(instruction[1]);
                const auto width = selector == ids.end() ? 0 : ScalarWords(selector->second.result_type);
                if (width == 0 || width > instruction.size() - offset) return false;
                position = offset + width;
                return ReadOperand(Kind::IdRef, depth + 1);
            }
            position = offset;
            return ReadOperands(std::span(Operands).subspan(rule.sequence.offset, rule.sequence.count), depth + 1);
        case Category::ValueEnum:
        case Category::BitEnum:
            return ReadEnumerants(rule, value, depth);
        }
        return false;
    }

    bool ReadLiteral(Kind kind, uint32_t value, std::size_t offset, unsigned depth) {
        switch (kind) {
        case Kind::LiteralString: {
            const char* data = reinterpret_cast<const char*>(instruction.data() + offset);
            const std::size_t size = (instruction.size() - offset) * sizeof(uint32_t);
            const auto length = std::string_view(data, size).find('\0');
            if (length == std::string_view::npos) return false;
            position = offset + length / sizeof(uint32_t) + 1;
            if (opcode == spv::OpExtInstImport && result_id != 0)
                ids[result_id].glsl_import = std::string_view(data, length) == "GLSL.std.450";
            return true;
        }
        case Kind::LiteralContextDependentNumber: {
            const auto width = ScalarWords(result_type);
            if (width == 0 || width > instruction.size() - offset) return false;
            position = offset + width;
            return true;
        }
        case Kind::LiteralSpecConstantOpInteger: {
            const auto* operation = FindOpcode(Core, value);
            if (!operation || operation->operands.count < 2 || value == spv::OpSpecConstantOp)
                return false;
            const auto operands = std::span(Operands).subspan(operation->operands.offset,
                                                             operation->operands.count);
            if (operands[0].kind != Kind::IdResultType || operands[1].kind != Kind::IdResult)
                return false;
            return ReadOperands(operands.subspan(2), depth + 1);
        }
        case Kind::LiteralExtInstInteger: {
            // GLSL.std.450 is the emitter's only extended set. An unknown set
            // could assign a different meaning to the same operand words.
            if (opcode != spv::OpExtInst || offset != 4) return false;
            const auto set = ids.find(instruction[3]);
            if (set == ids.end() || !set->second.glsl_import) return false;
            const auto* operation = FindOpcode(Glsl, value);
            return operation && ReadOperands(std::span(Operands).subspan(operation->operands.offset,
                                            operation->operands.count), depth + 1);
        }
        default:
            return true;
        }
    }

    bool ReadEnumerants(const KindRule& rule, uint32_t value, unsigned depth) {
        const auto table = std::span(Enumerants).subspan(rule.sequence.offset, rule.sequence.count);
        const auto read = [&](uint32_t enumerant) {
            const auto it = std::ranges::lower_bound(table, enumerant, {}, &Enumerant::value);
            return it != table.end() && it->value == enumerant &&
                ReadOperands(std::span(Operands).subspan(it->parameters.offset, it->parameters.count), depth + 1);
        };
        if (rule.category == Category::ValueEnum) return read(value);
        while (value != 0) {
            const uint32_t bit = 1u << std::countr_zero(value);
            if (!read(bit)) return false;
            value &= ~bit;
        }
        return true;
    }

    bool Read(std::span<const uint32_t> words, const Instruction& rule) {
        instruction = words;
        opcode = rule.opcode;
        position = 1;
        result_type = 0;
        result_id = 0;
        auto operands = std::span(Operands).subspan(rule.operands.offset, rule.operands.count);
        // The extended-set grammar consumes the argument words at its opcode.
        if (opcode == spv::OpExtInst) operands = operands.first(4);
        if (!ReadOperands(operands, 0) || position != words.size()) return false;
        if (result_id != 0) {
            auto& id = ids[result_id];
            if (id.defined) return false;
            id.defined = true;
            id.result_type = result_type;
            if (opcode == spv::OpTypeInt || opcode == spv::OpTypeFloat) {
                id.scalar_width = words[2];
                if (id.scalar_width == 0) return false;
            }
        }
        return true;
    }
};

} // namespace

Issue Inspect(std::span<const uint32_t> words, IdUseVisitor visit, void* context) {
    if (words.size() < 5 || words.size() > MaxWords || words[0] != spv::MagicNumber ||
        words[1] < 0x00010000u || words[1] > spv::Version || (words[1] & 0xff0000ffu) != 0 ||
        words[3] == 0 || words[3] > MaxIdBound || words[4] != 0)
        return {0, "invalid SPIR-V header"};
    Reader reader{.bound = words[3], .visitor = visit, .context = context};
    bool memory_model = false;
    bool entry_point = false;
    bool in_function = false;
    for (std::size_t cursor = 5; cursor < words.size();) {
        const auto count = words[cursor] >> spv::WordCountShift;
        if (count == 0 || count > words.size() - cursor)
            return {cursor, "invalid instruction length"};
        const auto opcode = words[cursor] & spv::OpCodeMask;
        const auto* rule = FindOpcode(Core, opcode);
        if (!rule || !reader.Read(words.subspan(cursor, count), *rule))
            return {cursor, "unsupported or invalid instruction operands"};
        if (opcode == spv::OpMemoryModel) {
            if (memory_model) return {cursor, "multiple memory models"};
            memory_model = true;
        } else if (opcode == spv::OpEntryPoint) {
            entry_point = true;
        } else if (opcode == spv::OpFunction || opcode == spv::OpFunctionEnd) {
            if (in_function == (opcode == spv::OpFunction))
                return {cursor, "unmatched function boundary"};
            in_function = !in_function;
        }
        cursor += count;
    }
    if (!memory_model || !entry_point || in_function)
        return {words.size(), "incomplete shader module"};
    if (std::ranges::any_of(reader.ids, [](const auto& entry) { return !entry.second.defined; }))
        return {words.size(), "undefined SPIR-V ID"};
    return {};
}

} // namespace Libs::Graphics::SpirvBinary
