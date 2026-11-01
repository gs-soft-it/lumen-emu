// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace UniformConditionRefDetail {

// Only operations whose result is uniform when every operand is uniform.
// In particular this excludes undef, lane/attribute/builtin reads, votes,
// read-first-lane, derivatives, and Phi values with control-flow provenance.
inline bool PureUniformOperation(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::BitCastU64F64:
		case ValueOpcode::BitCastF64U64:
		case ValueOpcode::ConvertU16U32:
		case ValueOpcode::ConvertU32U16:
		case ValueOpcode::ConvertU8U32:
		case ValueOpcode::ConvertU32U8:
		case ValueOpcode::ConvertF32S32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeConstructU32x3:
		case ValueOpcode::CompositeConstructU32x4:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::CompositeExtractU32x3:
		case ValueOpcode::CompositeExtractU32x4:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::SelectF64:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::SMulHi:
		case ValueOpcode::UMulHi:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::BitReverse32:
		case ValueOpcode::BitCount32:
		case ValueOpcode::BitCount64:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::SMin32:
		case ValueOpcode::UMin32:
		case ValueOpcode::SMax32:
		case ValueOpcode::UMax32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::IEqual64:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::INotEqual64:
		case ValueOpcode::SLessThan32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::SLessThanEqual32:
		case ValueOpcode::ULessThanEqual32:
		case ValueOpcode::SGreaterThan32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::SGreaterThanEqual32:
		case ValueOpcode::UGreaterThanEqual32:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::FPAbs32:
		case ValueOpcode::FPNeg32:
		case ValueOpcode::FPAdd32:
		case ValueOpcode::FPSub32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPMad32:
		case ValueOpcode::FPOrdEqual32:
		case ValueOpcode::FPOrdNotEqual32:
		case ValueOpcode::FPOrdLessThan32:
		case ValueOpcode::FPOrdGreaterThan32:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPUnordEqual32:
		case ValueOpcode::FPUnordNotEqual32:
		case ValueOpcode::FPUnordLessThan32:
		case ValueOpcode::FPUnordGreaterThan32:
		case ValueOpcode::FPUnordLessThanEqual32:
		case ValueOpcode::FPUnordGreaterThanEqual32: return true;
		default: return false;
	}
}

// A different descriptor or image view can alias a scalar buffer. Thus this
// first proof excludes ALL memory writes/atomics and barriers, not just writes
// to the resource being read. The usual data-race-free draw/dispatch contract
// supplies stability against external writers; no memory value is folded.
inline bool StableReadOnlyShader(const Program& program) {
	for (const auto& buffer: program.info.buffers)
		if (buffer.written || buffer.atomic) return false;
	for (const auto& image: program.info.images)
		if (image.written || image.atomic || image.atomic64) return false;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op = inst.GetOpcode();
			const auto buffer = BufferAccessOf(op);
			const auto address = AddressOpcodeInfoOf(op).access;
			const auto image = ImageOpcodeInfoOf(op).access;
			const auto shared = SharedAccessOf(op);
			if ((buffer != BufferAccess::None && buffer != BufferAccess::Read) ||
			    (address != AddressAccess::None && address != AddressAccess::Read) ||
			    (image != ImageAccess::None && image != ImageAccess::Read) ||
			    (shared != SharedAccess::None && shared != SharedAccess::Read)) return false;
			if (buffer != BufferAccess::None || address != AddressAccess::None ||
			    image != ImageAccess::None || shared != SharedAccess::None) {
				const auto index = inst.Flags<MemoryFlags>().index;
				if (index >= program.memory_info.size() || program.memory_info[index].coherent)
					return false;
			}
			// Sampling has a separate candidate-local ordering proof below. Other
			// helper-observing operations and unclassified side effects stay excluded.
			if (op == ValueOpcode::ImageGatherRaw || op == ValueOpcode::ImageQueryLod ||
			    op == ValueOpcode::SwizzleU32 || op == ValueOpcode::DppMoveU32 ||
			    op == ValueOpcode::DppUpdateU32) return false;
			if (op == ValueOpcode::SetAttribute && program.stage == ShaderType::Pixel) {
				const auto index = inst.Flags<ExportFlags>().index;
				if (index >= program.export_info.size() ||
				    (program.export_info[index].vm && inst.Arg(1).Resolve() != Value(true)))
					return false;
			}
			if (HasSideEffects(op) && buffer == BufferAccess::None &&
			    address == AddressAccess::None && image == ImageAccess::None &&
			    shared == SharedAccess::None && op != ValueOpcode::SetAttribute &&
			    op != ValueOpcode::Reference && op != ValueOpcode::ReferenceU32) return false;
		}
	}
	return true;
}

inline bool NoIndependentSubgroupObservers(const Program& program) {
	// An independent cross-lane consumer could expose a helper's changed state
	// to an observable invocation. Sample-bearing proofs exclude both
	// explicit lane operations and implicit subgroup-indexed buffer addressing.
	// Uniform ConditionRefs remain guest-local and are checked separately.
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto op = inst.GetOpcode();
			if (BufferAccessOf(op) != BufferAccess::None) {
				const auto index = inst.Flags<MemoryFlags>().index;
				if (index >= program.memory_info.size()) return false;
				const auto& memory = program.memory_info[index];
				if (!memory.planning_only &&
				    ((memory.kind == ResourceKind::IndirectBuffer && op != ValueOpcode::ReadConstBuffer) ||
				     (memory.kind == ResourceKind::Buffer &&
				      (memory.resource >= program.info.buffers.size() ||
				       (program.info.buffers[memory.resource].packed_stride & (1u << 20u)) != 0u))))
					return false;
			}
			switch (op) {
				case ValueOpcode::LaneId:
				case ValueOpcode::Ballot:
				case ValueOpcode::SubgroupUMinU32:
				case ValueOpcode::ReadFirstLane:
				case ValueOpcode::ReadLane:
				case ValueOpcode::WriteLane:
				case ValueOpcode::Permlane16U32:
				case ValueOpcode::PermuteU32:
				case ValueOpcode::BpermuteU32: return false;
				default: break;
			}
		}
	}
	return true;
}

// An already-executed sample cannot observe a later change to the helper's
// branch result. Require every sample to precede this vote in this same block,
// and prove that no later path can execute that sample block again. This is
// deliberately narrower than moving votes across arbitrary derivative regions.
inline bool SamplesPrecedeVote(const Program& program, const Block& owner, const Inst& vote,
                              bool uniform_conditions) {
	bool has_sample = false;
	for (const auto* block: program.blocks) {
		bool after_vote = false;
		for (const auto& inst: *block) {
			if (&inst == &vote) after_vote = true;
			if (inst.GetOpcode() != ValueOpcode::ImageSampleRaw) continue;
			if (program.stage != ShaderType::Pixel || block != &owner || after_vote ||
			    inst.Arg(3).Resolve() != Value(true)) return false;
			has_sample = true;
		}
	}
	if (!has_sample) return true;
	if (program.dispatcher_fallback || !uniform_conditions) return false;
	if (!NoIndependentSubgroupObservers(program)) return false;
	const std::unordered_set<const Block*> known(program.blocks.begin(), program.blocks.end());
	std::unordered_set<const Block*> visited;
	std::vector<const Block*> pending {&owner};
	while (!pending.empty()) {
		const auto* block = pending.back();
		pending.pop_back();
		if (!known.contains(block)) return false;
		if (!visited.insert(block).second) continue;
		const auto edge = [&](const Block* next) {
			if (!next || next == &owner || !known.contains(next)) return false;
			pending.push_back(next);
			return true;
		};
		switch (block->terminator.kind) {
			case CFG::TerminatorKind::Branch:
				if (!edge(block->terminator.true_block)) return false;
				break;
			case CFG::TerminatorKind::ConditionalBranch:
				if (!edge(block->terminator.true_block) || !edge(block->terminator.false_block))
					return false;
				break;
			case CFG::TerminatorKind::Return: break;
			default: return false; // Unknown/indirect targets cannot prove no re-entry.
		}
	}
	return true;
}

class Proof {
public:
	explicit Proof(const Program& program, bool transparent_conditions = false):
	    program(program), transparent_conditions(transparent_conditions) {}

	bool Uniform(Value value, uint32_t depth = 0) {
		value = value.Resolve();
		if (value.IsImmediate()) return true;
		const auto* inst = value.TryInstruction();
		if (!inst || depth >= 128u) return false;
		if (const auto found = known.find(inst); found != known.end()) return found->second;
		known.emplace(inst, false); // Cycles and unknowns are never assumed uniform.
		const auto op = inst->GetOpcode();
		bool uniform = false;
		if (op == ValueOpcode::GetUserData) {
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			uniform = reg >= program.user_data_base &&
			          reg - program.user_data_base < program.user_data_count;
		} else if (op == ValueOpcode::GetShaderBase) {
			uniform = true;
		} else if (op == ValueOpcode::ReadConstBuffer) {
			const auto index = inst->Flags<MemoryFlags>().index;
			if (index < program.memory_info.size()) {
				const auto& memory = program.memory_info[index];
				const auto* handle = inst->Arg(0).Resolve().TryInstruction();
				// S_LOAD splits SMEM xN into independent U32 loads, retaining
				// the original span and component index as provenance metadata.
				const auto count = memory.component_count;
				const bool scalarized = (count == 1u || count == 2u || count == 4u ||
				                         count == 8u || count == 16u) &&
				                        memory.component_index < count;
				if (memory.kind == ResourceKind::ScalarBuffer && !memory.planning_only &&
				    !memory.coherent && !memory.typed && !memory.formatted &&
				    memory.data_bits == 32u && memory.data_dwords == 1u &&
				    scalarized && !memory.idxen && !memory.offen &&
				    memory.resource < program.info.buffers.size() && handle &&
				    handle->GetOpcode() == ValueOpcode::GetBufferResource &&
				    handle->Flags<uint32_t>() == memory.resource) {
					const auto& buffer = program.info.buffers[memory.resource];
					// Descriptor operands are scrubbed after materialization. The
					// validated direct resource binding, not those zeros, proves that
					// base, bounds and offset push data are shared by the command.
					uniform = buffer.read && buffer.scalar && !buffer.written &&
					          !buffer.atomic && !buffer.formatted &&
					          buffer.image_alias == BufferResource::NoImageAlias &&
					          buffer.indirect_root == BufferResource::NoIndirectBuffer &&
					          buffer.indirect_resources.empty() &&
					          Uniform(inst->Arg(1), depth + 1u);
				}
			}
		} else if (op == ValueOpcode::ConditionRef && transparent_conditions) {
			const auto kind = inst->Flags<CFG::BranchCondition>();
			if (kind == CFG::BranchCondition::SccZero || kind == CFG::BranchCondition::SccNonZero ||
			    kind == CFG::BranchCondition::VccZero || kind == CFG::BranchCondition::VccNonZero ||
			    kind == CFG::BranchCondition::ExecZero || kind == CFG::BranchCondition::ExecNonZero ||
			    kind == CFG::BranchCondition::ScalarInstruction)
				uniform = Uniform(inst->Arg(0), depth + 1u);
		} else if (PureUniformOperation(op)) {
			uniform = true;
			for (size_t i = 0; i < inst->NumArgs(); ++i)
				uniform &= Uniform(inst->Arg(i), depth + 1u);
		}
		known[inst] = uniform;
		return uniform;
	}

private:
	const Program& program;
	const bool transparent_conditions;
	std::unordered_map<const Inst*, bool> known;
};

// Fragment derivative groups belong to one primitive/command, including
// helpers. Uniform guest control throughout this acyclic function therefore
// keeps every derivative group on the same path even when a sample follows a
// vote. This is not a claim that a native subgroup contains only one command.
// The older prefix proof remains available for programs outside this subset.
inline bool UniformSampleControl(const Program& program, bool uniform_conditions) {
	if (program.stage != ShaderType::Pixel || program.dispatcher_fallback ||
	    program.blocks.empty() || !uniform_conditions ||
	    !NoIndependentSubgroupObservers(program)) return false;
	Proof control(program, true);
	Proof data(program);
	std::unordered_map<const Block*, uint32_t> incoming;
	for (const auto* block: program.blocks)
		if (!block || !incoming.emplace(block, 0u).second) return false;
	bool has_sample = false;
	for (auto* block: program.blocks) {
		const auto& term = block->terminator;
		if (term.loop_header || term.continue_block || term.indexed || !term.cases.empty() ||
		    (term.merge_block && (term.kind != CFG::TerminatorKind::ConditionalBranch ||
		                          !incoming.contains(term.merge_block)))) return false;
		const auto edge = [&](const Block* next) {
			const auto found = incoming.find(next);
			if (found == incoming.end()) return false;
			++found->second;
			return true;
		};
		switch (term.kind) {
			case CFG::TerminatorKind::Branch:
				if (!edge(term.true_block)) return false;
				break;
			case CFG::TerminatorKind::ConditionalBranch:
				if (!term.merge_block || block->condition.IsEmpty() ||
				    !control.Uniform(block->condition) ||
				    !edge(term.true_block) || !edge(term.false_block)) return false;
				break;
			case CFG::TerminatorKind::Return: break;
			default: return false;
		}
		for (auto& inst: *block) {
			const auto op = inst.GetOpcode();
			if (op == ValueOpcode::BvhIntersect || op == ValueOpcode::GetBufferResourceIndex ||
			    op == ValueOpcode::ReadConst) return false; // Hidden memory/control flow.
			if (op == ValueOpcode::ConditionRef && !control.Uniform(Value(&inst))) return false;
			if (op == ValueOpcode::SetAttribute && inst.Arg(1).Resolve() != Value(true)) return false;
			if (op == ValueOpcode::ImageSampleRaw) {
				has_sample = true;
				if (inst.Arg(3).Resolve() != Value(true)) return false;
				const auto index = inst.Flags<MemoryFlags>().index;
				if (index >= program.memory_info.size()) return false;
				const auto& memory = program.memory_info[index];
				if (memory.kind != ResourceKind::Image || memory.planning_only || memory.coherent ||
				    memory.resource >= program.info.images.size() ||
				    memory.sampler >= program.info.samplers.size()) return false;
				const auto& image = program.info.images[memory.resource];
				const auto& sampler = program.info.samplers[memory.sampler];
				const auto* image_handle = inst.Arg(0).Resolve().TryInstruction();
				const auto* sampler_handle = inst.Arg(1).Resolve().TryInstruction();
				if (!image_handle || image_handle->GetOpcode() != ValueOpcode::GetImageResource ||
				    image_handle->Flags<uint32_t>() != memory.resource ||
				    !sampler_handle || sampler_handle->GetOpcode() != ValueOpcode::GetSamplerResource ||
				    sampler_handle->Flags<uint32_t>() != memory.sampler ||
				    !image.read || image.indirect_root != ImageResource::NoIndirectImage ||
				    !image.indirect_resources.empty() || image.null_descriptor || image.cube ||
				    image.explicit_image_bounds || image.mip_mode != ImageMipMode::None ||
				    image.conversion_format != Prospero::BufferFormat::kInvalid ||
				    (image.dimension != Decoder::ImageDimension::Dim2D &&
				     image.dimension != Decoder::ImageDimension::Dim2DArray) ||
				    memory.image_dimension != image.dimension ||
				    sampler.indirect_root != SamplerResource::NoIndirectSampler ||
				    !sampler.indirect_resources.empty()) return false;
			} else if (BufferAccessOf(op) != BufferAccess::None ||
			           AddressOpcodeInfoOf(op).access != AddressAccess::None ||
			           ImageOpcodeInfoOf(op).access != ImageAccess::None ||
			           SharedAccessOf(op) != SharedAccess::None) {
				// Direct scalar reads have command-uniform bounds branches too.
				// Other memory emitters may introduce unproved internal branches.
				if (op != ValueOpcode::ReadConstBuffer || !data.Uniform(Value(&inst))) return false;
			}
		}
	}
	if (!has_sample || incoming.at(program.blocks.front()) != 0u) return false;
	// Kahn traversal rejects every cycle, including one hidden from loop flags.
	std::vector<const Block*> pending;
	for (const auto& [block, count]: incoming)
		if (count == 0u) pending.push_back(block);
	if (pending.size() != 1u) return false; // One reachable entry, no detached roots.
	size_t visited = 0;
	while (!pending.empty()) {
		const auto* block = pending.back();
		pending.pop_back();
		++visited;
		const auto edge = [&](const Block* next) {
			if (--incoming.at(next) == 0u) pending.push_back(next);
		};
		if (block->terminator.kind != CFG::TerminatorKind::Return) {
			edge(block->terminator.true_block);
			if (block->terminator.kind == CFG::TerminatorKind::ConditionalBranch)
				edge(block->terminator.false_block);
		}
	}
	return visited == program.blocks.size();
}
} // namespace UniformConditionRefDetail

inline uint32_t FoldUniformConditionRefs(Program& program) {
	if (!program.resource_tracking_complete ||
	    !UniformConditionRefDetail::StableReadOnlyShader(program)) return 0;
	UniformConditionRefDetail::Proof proof(program);
	bool uniform_conditions = true;
	for (const auto* block: program.blocks)
		for (const auto& inst: *block)
			if (inst.GetOpcode() == ValueOpcode::ConditionRef)
				uniform_conditions &= proof.Uniform(inst.Arg(0));
	const bool uniform_sample_control =
	    UniformConditionRefDetail::UniformSampleControl(program, uniform_conditions);
	uint32_t changed = 0;
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::ConditionRef) continue;
			const auto kind = inst.Flags<CFG::BranchCondition>();
			if (kind != CFG::BranchCondition::SccZero && kind != CFG::BranchCondition::SccNonZero &&
			    kind != CFG::BranchCondition::VccZero && kind != CFG::BranchCondition::VccNonZero &&
			    kind != CFG::BranchCondition::ExecZero && kind != CFG::BranchCondition::ExecNonZero)
				continue;
			if (!proof.Uniform(inst.Arg(0)) ||
			    (!uniform_sample_control &&
			     !UniformConditionRefDetail::SamplesPrecedeVote(program, *block, inst, uniform_conditions))) continue;
			// Translation already inverted Zero predicates. A predicate uniform in
			// the guest command/wave is its own guest Any/All result. This does not
			// assume a physical Vulkan subgroup contains only one command.
			inst.ReplaceUsesWith(inst.Arg(0));
			++changed;
		}
	}
	if (changed != 0u) {
		ConstantPropagationPass(program.blocks, program.wave_size);
		RemoveIdentities(program);
		EliminateDeadCode(program.blocks);
		program.shader_info_complete = false;
	}
	return changed;
}
} // namespace Libs::Graphics::ShaderRecompiler::IR
