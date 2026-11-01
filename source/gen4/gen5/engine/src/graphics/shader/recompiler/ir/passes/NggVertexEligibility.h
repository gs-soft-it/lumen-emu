// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Only the translator's initial vertex EXEC ballot may establish NGG ABI counts.
// An otherwise identical ballot produced by guest instructions has no such meaning.
inline constexpr uint64_t NggVertexEntryBallotFlag = 1u;

namespace NggVertexEligibilityDetail {

// Keep the ABI remapping independent of any genuine lane or cross-vertex
// observation. Include the implicit subgroup consumers tracked by shader-info
// collection, not just explicit ballot/shuffle instructions.
inline bool ObservesSubgroup(const Program& program, const Inst& inst) {
	const auto op = inst.GetOpcode();
	const auto shared = SharedAccessOf(op);
	if (shared == SharedAccess::Append || shared == SharedAccess::Consume) return true;
	if (BufferAccessOf(op) != BufferAccess::None) {
		const auto index = inst.Flags<MemoryFlags>().index;
		if (index >= program.memory_info.size()) return true;
		const auto& memory = program.memory_info[index];
		if (!memory.planning_only) {
			if (memory.kind == ResourceKind::IndirectBuffer && op != ValueOpcode::ReadConstBuffer)
				return true;
			if (memory.kind == ResourceKind::Buffer &&
			    (memory.resource >= program.info.buffers.size() ||
			     (program.info.buffers[memory.resource].packed_stride & (1u << 20u)) != 0u))
				return true;
		}
	}
	switch (op) {
		case ValueOpcode::ConditionRef:
			return inst.Flags<CFG::BranchCondition>() != CFG::BranchCondition::ScalarInstruction;
		case ValueOpcode::StoreCompletion:
		case ValueOpcode::Ballot:
		case ValueOpcode::LaneId:
		case ValueOpcode::SubgroupUMinU32:
		case ValueOpcode::DppMoveU32:
		case ValueOpcode::DppUpdateU32:
		case ValueOpcode::ReadFirstLane:
		case ValueOpcode::ReadLane:
		case ValueOpcode::WriteLane:
		case ValueOpcode::SwizzleU32:
		case ValueOpcode::Permlane16U32:
		case ValueOpcode::PermuteU32:
		case ValueOpcode::BpermuteU32: return true;
		default: return false;
	}
}

struct Match {
	std::unordered_set<Inst*> nodes;
	Inst* predicate = nullptr;
	Inst* ballot = nullptr;
	uint32_t wave;

	Inst* Node(Value value, ValueOpcode opcode) {
		auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != opcode) return nullptr;
		nodes.insert(inst);
		return inst;
	}

	static bool U32(Value value, uint32_t expected) {
		value = value.Resolve();
		return value.IsImmediate() && value.GetType() == Type::U32 && value.U32() == expected;
	}

	static bool U64(Value value, uint64_t expected) {
		value = value.Resolve();
		return value.IsImmediate() && value.GetType() == Type::U64 && value.U64() == expected;
	}

	Value WithConstant(Value value, ValueOpcode opcode, uint32_t constant) {
		const auto* inst = Node(value, opcode);
		if (!inst) return {};
		if (U32(inst->Arg(1), constant)) return inst->Arg(0);
		if (U32(inst->Arg(0), constant)) return inst->Arg(1);
		return {};
	}

	bool CountWord(Value value, uint32_t word) {
		const auto* count = Node(value, ValueOpcode::BitCount32);
		const auto* extract = count ? Node(count->Arg(0), ValueOpcode::CompositeExtractU32x4) : nullptr;
		if (!extract || !U32(extract->Arg(1), word)) return false;
		auto* source = Node(extract->Arg(0), ValueOpcode::Ballot);
		if (!source || source->Flags<uint64_t>() != NggVertexEntryBallotFlag ||
		    source->Arg(0).Resolve() != Value(true) || (ballot && ballot != source)) return false;
		ballot = source;
		return true;
	}

	bool Count(Value value) {
		// ESVertCount is the low byte of compiler-seeded merged_wave_info (s3).
		auto packed = WithConstant(value, ValueOpcode::BitwiseAnd32, 0xffu);
		if (packed.IsEmpty()) return false;
		auto count = WithConstant(packed, ValueOpcode::BitwiseOr32, 1u << 28u);
		if (count.IsEmpty()) return false;
		if (wave == 32u) return CountWord(count, 0u);
		const auto* sum = Node(count, ValueOpcode::IAdd32);
		if (!sum) return false;
		// The translator emits low then high. Do not infer provenance from an
		// arbitrary popcount or assume all active physical lane IDs are dense.
		return CountWord(sum->Arg(0), 0u) && CountWord(sum->Arg(1), 1u);
	}

	bool PrefixMask(Value value) {
		const auto* shift = Node(value, ValueOpcode::ShiftRightLogical64);
		if (!shift || !U64(shift->Arg(0), UINT64_MAX)) return false;
		const auto* subtract = Node(shift->Arg(1), ValueOpcode::ISub32);
		return subtract && U32(subtract->Arg(0), 64u) && Count(subtract->Arg(1));
	}

	bool Predicate(Inst& candidate) {
		predicate = Node(Value(&candidate), ValueOpcode::INotEqual32);
		if (!predicate || !U32(predicate->Arg(1), 0u)) return false;
		auto shifted = WithConstant(predicate->Arg(0), ValueOpcode::BitwiseAnd32, 1u);
		const auto* shift = Node(shifted, ValueOpcode::ShiftRightLogical32);
		if (!shift) return false;
		auto lane_value = WithConstant(shift->Arg(1), ValueOpcode::BitwiseAnd32, 31u);
		const auto* lane = Node(lane_value, ValueOpcode::LaneId);
		if (!lane) return false;
		Value low_word = shift->Arg(0);
		Value high_word;
		if (wave == 64u) {
			const auto* select = Node(low_word, ValueOpcode::SelectU32);
			const auto* half = select ? Node(select->Arg(0), ValueOpcode::ULessThan32) : nullptr;
			if (!half || half->Arg(0).Resolve() != lane_value.Resolve() || !U32(half->Arg(1), 32u))
				return false;
			low_word = select->Arg(1);
			high_word = select->Arg(2);
		}
		const auto* low = Node(low_word, ValueOpcode::CompositeExtractU64);
		if (!low || !U32(low->Arg(1), 0u)) return false;
		if (wave == 64u) {
			const auto* high = Node(high_word, ValueOpcode::CompositeExtractU64);
			if (!high || !U32(high->Arg(1), 1u) || high->Arg(0).Resolve() != low->Arg(0).Resolve())
				return false;
		}
		return PrefixMask(low->Arg(0));
	}
};

} // namespace NggVertexEligibilityDetail

// Vulkan already invokes the ordinary vertex shader for admitted input vertices;
// primitive assembly belongs to the API. The NGG has_input_vertex prefix is an
// ABI eligibility wrapper for those same invocations. This is NOT the algebraic
// claim physical_lane < popcount(ballot(true)): physical subgroups may be sparse.
// Only a closed, compiler-provenanced eligibility graph may lose its mask. Any
// observable count/raw-mask/lane use keeps the original subgroup requirements.
inline uint32_t LowerNggVertexEligibility(Program& program) {
	if (program.stage != ShaderType::Vertex || (program.wave_size != 32u && program.wave_size != 64u))
		return 0;

	// EmitSetAttribute already ignores Primitive exports outside Mesh. Eliminate
	// that no-op here, retaining independently observable operand side effects.
	bool removed_primitive = false;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::SetAttribute) continue;
			const auto index = inst.Flags<ExportFlags>().index;
			if (index < program.export_info.size() &&
			    program.export_info[index].kind == ExportTargetKind::Primitive) {
				inst.Invalidate();
				removed_primitive = true;
			}
		}
	}
	if (removed_primitive) {
		EliminateDeadCode(program.blocks);
		program.shader_info_complete = false;
	}

	std::unordered_set<Inst*> nodes;
	std::vector<Inst*> predicates;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::INotEqual32) continue;
			NggVertexEligibilityDetail::Match match {.wave = program.wave_size};
			if (!match.Predicate(inst)) continue;
			predicates.push_back(match.predicate);
			nodes.insert(match.nodes.begin(), match.nodes.end());
		}
	}
	if (predicates.empty()) return 0;
	const auto root = [&](const Inst* inst) {
		return std::find(predicates.begin(), predicates.end(), inst) != predicates.end();
	};
	for (const auto* inst: nodes) {
		if (root(inst)) continue;
		for (const auto& use: inst->Uses()) {
			if (!nodes.contains(use.user)) return 0;
		}
	}
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (!nodes.contains(const_cast<Inst*>(&inst)) &&
			    NggVertexEligibilityDetail::ObservesSubgroup(program, inst)) return 0;
		}
		for (const auto control: {block->condition, block->indirect_target}) {
			const auto* inst = control.Resolve().TryInstruction();
			if (inst && nodes.contains(const_cast<Inst*>(inst)) && !root(inst)) return 0;
		}
	}
	for (auto* predicate: predicates) predicate->ReplaceUsesWith(Value(true));
	ConstantPropagationPass(program.blocks, program.wave_size);
	RemoveIdentities(program);
	EliminateDeadCode(program.blocks);
	program.shader_info_complete = false;
	return static_cast<uint32_t>(predicates.size());
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
