// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "graphics/shader/recompiler/ir/passes/UniformConditionRef.h"
#include "graphics/shader/recompiler/ir/passes/RasterQuadLoopProgress.h"

#include <map>
#include <set>

namespace Libs::Graphics::ShaderRecompiler::IR {

// The certificate concerns the guest program, not the host subgroup width.
// A request to use quad control never constitutes this certificate.
struct RasterQuadProof {
	bool admitted = false;
	const char* reason = "not_analyzed";
	uint32_t block_id = UINT32_MAX;
	uint32_t instruction = UINT32_MAX;
};

namespace RasterQuadDetail {

using Blocks = std::unordered_set<const Block*>;

inline bool IsSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF64 || op == ValueOpcode::SelectF32;
}

inline bool PureOperation(ValueOpcode op) {
	if (UniformConditionRefDetail::PureUniformOperation(op)) return true;
	switch (op) {
		case ValueOpcode::ConvertS32F32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPTrunc32:
		case ValueOpcode::FPMin32:
		case ValueOpcode::FPMax32:
		case ValueOpcode::SMedTri32:
		case ValueOpcode::FPSaturate32:
		case ValueOpcode::FPRecip32:
		case ValueOpcode::FPRecipSqrt32:
		case ValueOpcode::FPSqrt:
		case ValueOpcode::FPFract32:
		case ValueOpcode::FPFloor32:
		case ValueOpcode::FPRoundEven32:
		case ValueOpcode::FPLog2:
		case ValueOpcode::FPExp2:
		case ValueOpcode::FPFma32:
		case ValueOpcode::FPMedTri32:
		case ValueOpcode::FPMinTri32:
		case ValueOpcode::FPMaxTri32:
		case ValueOpcode::PackFloat2x16Rtz: return true;
		default: return false;
	}
}

struct Loop {
	const Block* header = nullptr;
	const Block* merge = nullptr;
	Blocks body;
};

// Symbolic cofactoring does not read guest memory or mutate the IR. Unknown
// operations retain their identity; cycles are not treated as equal to an
// arbitrary entry value. Loop invariants require an explicit base and step.
class Cofactor {
public:
	struct Expression {
		Value atom;
		ValueOpcode op = ValueOpcode::Void;
		uint64_t flags = 0;
		std::vector<uint32_t> args;
		bool uniform = false;
	};

	explicit Cofactor(const Program& program): uniform_proof(program) {
		Atom(Value(false));
		Atom(Value(true));
	}

	void Assume(Value value, bool truth) {
		value = value.Resolve();
		if (const auto* inst = value.TryInstruction()) {
			if (const auto old = facts.find(inst); old != facts.end() && old->second == (truth ? 1u : 0u)) return;
			facts[inst] = truth ? 1u : 0u;
			if (truth) if (const auto implication = implications.find(inst); implication != implications.end())
				Assume(implication->second, true);
			if (inst->GetOpcode() == ValueOpcode::LogicalNot) Assume(inst->Arg(0), !truth);
			if (truth && inst->GetOpcode() == ValueOpcode::LogicalAnd) {
				Assume(inst->Arg(0), true);
				Assume(inst->Arg(1), true);
			}
		}
		Invalidate();
	}

	void OpaquePhiOutside(const Blocks& region) { phi_region = &region; Invalidate(); }

	void Freeze(const Inst* inst) { frozen.insert(inst); Invalidate(); }
	void MarkOutside(const Inst* inst) { stable_outside.insert(inst); }

	void RegisterImplication(const Inst* inst, Value necessary) { implications[inst] = necessary; }

	void MarkUniform(const Inst* inst, bool uniform) {
		if (uniform) uniform_atoms.insert(inst); else uniform_atoms.erase(inst);
		Invalidate();
	}

	void Substitute(const Inst* inst, Value value) { substitutions[inst] = value; Invalidate(); }
	void RemoveSubstitution(const Inst* inst) { substitutions.erase(inst); Invalidate(); }

	uint32_t Eval(Value value, uint32_t depth = 0) {
		value = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (!inst || depth >= 512u) return Atom(value);
		if (const auto found = facts.find(inst); found != facts.end()) return found->second;
		if (const auto found = cache.find(inst); found != cache.end()) return found->second;
		const auto original = Atom(value);
		if (expressions.size() >= 262144u) return original;
		if (frozen.contains(inst)) return original;
		cache.emplace(inst, original); // A recursive Phi remains unknown.
		if (const auto found = substitutions.find(inst); found != substitutions.end())
			return cache[inst] = Eval(found->second, depth + 1u);
		const auto op = inst->GetOpcode();
		if (IsSelect(op)) {
			const auto condition = Eval(inst->Arg(0), depth + 1u);
			if (condition <= 1u) return cache[inst] = Eval(inst->Arg(condition ? 1u : 2u), depth + 1u);
			const auto yes = Eval(inst->Arg(1), depth + 1u);
			const auto no = Eval(inst->Arg(2), depth + 1u);
			if (yes == no) return cache[inst] = yes;
		}
		if (op == ValueOpcode::LogicalNot) {
			const auto arg = Eval(inst->Arg(0), depth + 1u);
			if (arg <= 1u) return cache[inst] = 1u - arg;
			if (expressions[arg].op == ValueOpcode::LogicalNot)
				return cache[inst] = expressions[arg].args.front();
		}
		if (op == ValueOpcode::LogicalAnd || op == ValueOpcode::LogicalOr) {
			const auto left = Eval(inst->Arg(0), depth + 1u);
			const auto right = Eval(inst->Arg(1), depth + 1u);
			if (left == right) return cache[inst] = left;
			if (op == ValueOpcode::LogicalAnd) {
				if (left == 0u || right == 0u) return cache[inst] = 0u;
				if (left == 1u) return cache[inst] = right;
				if (right == 1u) return cache[inst] = left;
			} else {
				if (left == 1u || right == 1u) return cache[inst] = 1u;
				if (left == 0u) return cache[inst] = right;
				if (right == 0u) return cache[inst] = left;
			}
		}
		if (op == ValueOpcode::Phi) {
			if (phi_region && !phi_region->contains(inst->Parent())) return original;
			if (inst->NumArgs() == 0u) return original;
			const auto first = Eval(inst->Arg(0), depth + 1u);
			bool equal = true, uniform = expressions[first].uniform;
			for (size_t i = 1; i < inst->NumArgs(); ++i) {
				const auto next = Eval(inst->Arg(i), depth + 1u);
				equal &= next == first;
				uniform &= expressions[next].uniform;
			}
			// Every admitted guest edge is a scalar wave decision. Different
			// uniform incoming values remain wave-uniform, not command-constant.
			if (uniform) expressions[original].uniform = true;
			return cache[inst] = equal ? first : original;
		}
		if (!PureOperation(op)) return original;
		Expression result;
		result.op = op;
		result.flags = inst->Flags<uint64_t>();
		result.uniform = true;
		for (size_t i = 0; i < inst->NumArgs(); ++i) {
			const auto arg = Eval(inst->Arg(i), depth + 1u);
			result.args.push_back(arg);
			result.uniform &= expressions[arg].uniform;
		}
		return cache[inst] = Intern(std::move(result));
	}

	const Expression& Expr(uint32_t index) const { return expressions.at(index); }

	bool Uniform(Value value) { return expressions[Eval(value)].uniform; }
	bool Equal(Value left, Value right) { return Eval(left) == Eval(right); }
	bool Outside(Value value, const Blocks& blocks) {
		std::unordered_set<uint32_t> visited;
		std::vector<uint32_t> pending {Eval(value)};
		while (!pending.empty()) {
			const auto index = pending.back(); pending.pop_back();
			if (!visited.insert(index).second) continue;
			const auto& expression = expressions[index];
			if (const auto* inst = expression.atom.TryInstruction())
				if (blocks.contains(inst->Parent()) && !stable_outside.contains(inst)) return false;
			pending.insert(pending.end(), expression.args.begin(), expression.args.end());
		}
		return true;
	}

private:
	void Invalidate() {
		cache.clear();
		nodes.clear();
		for (const auto& [inst, index] : atoms)
			expressions[index].uniform = uniform_atoms.contains(inst) ||
			                             uniform_proof.Uniform(Value(const_cast<Inst*>(inst)));
	}

	uint32_t Atom(Value value) {
		if (const auto* inst = value.TryInstruction()) {
			if (const auto found = atoms.find(inst); found != atoms.end()) return found->second;
			const auto index = static_cast<uint32_t>(expressions.size());
			atoms.emplace(inst, index);
			expressions.push_back({.atom = value, .uniform = uniform_atoms.contains(inst) || uniform_proof.Uniform(value)});
			return index;
		}
		for (uint32_t i : immediates) if (expressions[i].atom == value) return i;
		const auto index = static_cast<uint32_t>(expressions.size());
		immediates.push_back(index);
		expressions.push_back({.atom = value, .uniform = value.IsImmediate()});
		return index;
	}
	uint32_t Intern(Expression expression) {
		std::vector<uint64_t> key {static_cast<uint64_t>(expression.op), expression.flags};
		for (auto arg : expression.args) key.push_back(arg);
		if (const auto found = nodes.find(key); found != nodes.end()) return found->second;
		const auto index = static_cast<uint32_t>(expressions.size());
		nodes.emplace(std::move(key), index);
		expressions.push_back(std::move(expression));
		return index;
	}
	UniformConditionRefDetail::Proof uniform_proof;
	std::vector<Expression> expressions;
	std::vector<uint32_t> immediates;
	std::unordered_map<const Inst*, uint32_t> atoms, facts, cache;
	std::unordered_map<const Inst*, Value> substitutions, implications;
	std::unordered_set<const Inst*> uniform_atoms, frozen, stable_outside;
	const Blocks* phi_region = nullptr;
	std::map<std::vector<uint64_t>, uint32_t> nodes;
};

class Analysis {
public:
	explicit Analysis(const Program& program): program(program), known(program.blocks.begin(), program.blocks.end()) {}

	RasterQuadProof Operations() const {
		if (!program.resource_tracking_complete) return Deny("resources_untracked");
		for (const auto& buffer : program.info.buffers)
			if (buffer.written || buffer.atomic || buffer.formatted ||
			    buffer.image_alias != BufferResource::NoImageAlias ||
			    buffer.indirect_root != BufferResource::NoIndirectBuffer ||
			    !buffer.indirect_resources.empty() || (buffer.packed_stride & (1u << 20u)))
				return Deny("buffer_contract");
		for (const auto& image : program.info.images)
			if (image.written || image.atomic || image.atomic64 || image.null_descriptor ||
			    image.cube || image.explicit_image_bounds || image.mip_mode != ImageMipMode::None ||
			    image.conversion_format != Prospero::BufferFormat::kInvalid ||
			    image.indirect_root != ImageResource::NoIndirectImage || !image.indirect_resources.empty() ||
			    (image.dimension != Decoder::ImageDimension::Dim2D &&
			     image.dimension != Decoder::ImageDimension::Dim2DArray)) return Deny("image_contract");
		for (const auto& sampler : program.info.samplers)
			if (sampler.indirect_root != SamplerResource::NoIndirectSampler ||
			    !sampler.indirect_resources.empty()) return Deny("sampler_contract");
		Blocks prefix;
		std::unordered_set<const Inst*> prefix_dpp;
		bool prefix_vote = false;
		const Block* current = program.blocks.empty() ? nullptr : program.blocks.front();
		while (current && known.contains(current) && prefix.insert(current).second) {
			if (current->terminator.loop_header) break;
			for (const auto& inst : *current) {
				if (inst.GetOpcode() == ValueOpcode::ConditionRef) prefix_vote = true;
				if (!prefix_vote) prefix_dpp.insert(&inst);
			}
			if (current->terminator.kind != CFG::TerminatorKind::Branch) break;
			current = current->terminator.true_block;
		}
		for (const auto* block : program.blocks) {
			bool seen_vote = false;
			for (const auto& inst : *block) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::ConditionRef) {
					seen_vote = true;
					if (!VoteControlsOnly(inst)) return At("vote_data_escape", *block, inst);
					continue;
				}
				if (op == ValueOpcode::DppMoveU32 || op == ValueOpcode::DppUpdateU32) {
					const auto flags = inst.Flags<DppMoveFlags>();
					if (!prefix_dpp.contains(&inst) || seen_vote || flags.dpp8 || flags.row_mask != 15u ||
					    flags.bank_mask != 15u || (flags.control != 0x00u && flags.control != 0x55u &&
					    flags.control != 0xaau && flags.control != 0xffu) ||
					    inst.Arg(op == ValueOpcode::DppMoveU32 ? 1u : 2u).Resolve() != Value(true))
						return At("dpp_prefix_contract", *block, inst);
					continue;
				}
				if (op == ValueOpcode::SetAttribute) {
					const auto index = inst.Flags<ExportFlags>().index;
					if (block->terminator.kind != CFG::TerminatorKind::Return ||
					    index >= program.export_info.size() || inst.Arg(1).Resolve() != Value(true))
						return At("terminal_export_contract", *block, inst);
					continue;
				}
				if (op == ValueOpcode::GetBufferResource || op == ValueOpcode::GetImageResource ||
				    op == ValueOpcode::GetSamplerResource) {
					const auto index = inst.Flags<uint32_t>();
					const auto count = op == ValueOpcode::GetBufferResource ? program.info.buffers.size() :
					                   op == ValueOpcode::GetImageResource ? program.info.images.size() : program.info.samplers.size();
					if (index >= count) return At("resource_handle", *block, inst);
					continue;
				}
				if (BufferAccessOf(op) == BufferAccess::Read || ImageOpcodeInfoOf(op).access == ImageAccess::Read) {
					if (!MemoryRead(inst)) return At("memory_read_contract", *block, inst);
					continue;
				}
				if (PureOperation(op) || op == ValueOpcode::Phi || op == ValueOpcode::GetAttribute ||
				    op == ValueOpcode::GetUserData || op == ValueOpcode::GetShaderBase ||
				    op == ValueOpcode::MakeImageAddress || op == ValueOpcode::Reference ||
				    op == ValueOpcode::ReferenceU32) continue;
				return At("unsupported_operation", *block, inst);
			}
		}
		return {.admitted = true, .reason = "operations_proved"};
	}

	RasterQuadProof Control() {
		if (program.stage != ShaderType::Pixel || program.dispatcher_fallback ||
		    program.blocks.empty() || program.blocks.size() > 512u ||
		    (program.wave_size != 32u && program.wave_size != 64u)) return Deny("stage_or_cfg");
		uint32_t instructions = 0;
		for (const auto* block : program.blocks) {
			if (!block) return Deny("null_block");
			for (const auto& inst : *block) {
				(void)inst;
				if (++instructions > 32768u) return Deny("instruction_budget", block);
			}
			const auto& t = block->terminator;
			if (t.indexed || !t.cases.empty() ||
			    (t.kind != CFG::TerminatorKind::Branch && t.kind != CFG::TerminatorKind::ConditionalBranch &&
			     t.kind != CFG::TerminatorKind::Return)) return Deny("indirect_control", block);
			if (t.kind != CFG::TerminatorKind::Return && !known.contains(t.true_block)) return Deny("missing_target", block);
			if (t.kind == CFG::TerminatorKind::ConditionalBranch &&
			    (!known.contains(t.false_block) || t.true_block == t.false_block)) return Deny("conditional_target", block);
			if (t.merge_block && !known.contains(t.merge_block)) return Deny("foreign_merge", block);
			if (t.loop_header) {
				if (!known.contains(t.merge_block) || !known.contains(t.continue_block)) return Deny("loop_metadata", block);
				Loop loop {.header = block, .merge = t.merge_block};
				if (!Region(block, t.merge_block, loop.body)) return Deny("loop_region", block);
				loops.push_back(std::move(loop));
			} else if (t.continue_block) return Deny("unexpected_continue", block);
		}
		if (const auto cfg = Cycles(); !cfg.admitted) return cfg;
		for (const auto* block : program.blocks) {
			if (block->terminator.kind != CFG::TerminatorKind::ConditionalBranch) continue;
			const auto result = Branch(*block);
			if (!result.admitted) return result;
		}
		for (const auto& loop : loops)
			if (!certified_loops.contains(loop.header)) return Deny("loop_without_progress_certificate", loop.header);
		return {.admitted = true, .reason = "control_proved"};
	}

private:
	bool VoteControlsOnly(const Inst& vote) const {
		std::vector<const Inst*> pending {&vote};
		std::unordered_set<const Inst*> visited;
		bool actual_condition = false;
		while (!pending.empty()) {
			const auto* inst = pending.back(); pending.pop_back();
			if (!visited.insert(inst).second) continue;
			for (const auto* block : program.blocks)
				actual_condition |= block->condition.Resolve().TryInstruction() == inst;
			for (const auto& use : inst->Uses()) {
				if (use.user->GetOpcode() == ValueOpcode::LogicalNot) pending.push_back(use.user);
				else if (use.user->GetOpcode() != ValueOpcode::Reference) return false;
			}
		}
		return actual_condition;
	}
	bool MemoryRead(const Inst& inst) const {
		const auto op = inst.GetOpcode();
		const auto index = inst.Flags<MemoryFlags>().index;
		if (index >= program.memory_info.size()) return false;
		const auto& memory = program.memory_info[index];
		if (memory.coherent || memory.planning_only || memory.formatted || memory.typed ||
		    memory.data_bits != 32u || memory.data_dwords == 0u || memory.data_dwords > 4u) return false;
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		if (BufferAccessOf(op) == BufferAccess::Read) {
			if (!handle || handle->GetOpcode() != ValueOpcode::GetBufferResource ||
			    handle->Flags<uint32_t>() != memory.resource || memory.resource >= program.info.buffers.size()) return false;
			if (op == ValueOpcode::ReadConstBuffer) {
				UniformConditionRefDetail::Proof uniform(program);
				return uniform.Uniform(Value(const_cast<Inst*>(&inst)));
			}
			return memory.kind == ResourceKind::Buffer &&
			       (op == ValueOpcode::LoadBufferU32 || op == ValueOpcode::LoadBufferU32x2 ||
			        op == ValueOpcode::LoadBufferU32x3 || op == ValueOpcode::LoadBufferU32x4);
		}
		if (memory.kind != ResourceKind::Image || !handle || handle->GetOpcode() != ValueOpcode::GetImageResource ||
		    handle->Flags<uint32_t>() != memory.resource || memory.resource >= program.info.images.size() ||
		    (memory.image_dimension != Decoder::ImageDimension::Dim2D &&
		     memory.image_dimension != Decoder::ImageDimension::Dim2DArray)) return false;
		if (op == ValueOpcode::ImageSampleRaw || op == ValueOpcode::ImageGatherRaw) {
			const auto* sampler = inst.Arg(1).Resolve().TryInstruction();
			return sampler && sampler->GetOpcode() == ValueOpcode::GetSamplerResource &&
			       sampler->Flags<uint32_t>() == memory.sampler && memory.sampler < program.info.samplers.size();
		}
		return op == ValueOpcode::ImageRead || op == ValueOpcode::ImageQueryDimensions;
	}

	RasterQuadProof Deny(const char* reason, const Block* block = nullptr) const {
		return {.reason = reason, .block_id = block ? block->id : UINT32_MAX};
	}
	RasterQuadProof At(const char* reason, const Block& block, const Inst& instruction) const {
		auto result = Deny(reason, &block);
		uint32_t index = 0;
		for (const auto* owner : program.blocks) for (const auto& inst : *owner) {
			++index;
			if (&inst == &instruction) { result.instruction = index; return result; }
		}
		return result;
	}
	bool Region(const Block* start, const Block* stop, Blocks& region) const {
		std::vector<const Block*> pending {start};
		while (!pending.empty()) {
			const auto* block = pending.back(); pending.pop_back();
			if (block == stop) continue;
			if (!known.contains(block)) return false;
			if (!region.insert(block).second) continue;
			if (block->terminator.kind == CFG::TerminatorKind::Return) return false;
			pending.push_back(block->terminator.true_block);
			if (block->terminator.kind == CFG::TerminatorKind::ConditionalBranch)
				pending.push_back(block->terminator.false_block);
		}
		return true;
	}
	RasterQuadProof Cycles() const {
		std::unordered_map<const Block*, std::vector<const Block*>> predecessors;
		for (auto* block : program.blocks) predecessors.emplace(block, std::vector<const Block*>{});
		for (auto* block : program.blocks) {
			if (block->terminator.kind == CFG::TerminatorKind::Return) continue;
			predecessors.at(block->terminator.true_block).push_back(block);
			if (block->terminator.kind == CFG::TerminatorKind::ConditionalBranch)
				predecessors.at(block->terminator.false_block).push_back(block);
		}
		if (!predecessors.at(program.blocks.front()).empty()) return Deny("entry_has_predecessor");
		uint32_t returns = 0;
		for (const auto* block : program.blocks) {
			returns += block->terminator.kind == CFG::TerminatorKind::Return;
			for (const auto& phi : *block) {
				if (phi.GetOpcode() != ValueOpcode::Phi) continue;
				Blocks incoming;
				for (size_t i = 0; i < phi.NumArgs(); ++i)
					if (!incoming.insert(phi.PhiBlock(i)).second ||
					    std::ranges::find(predecessors.at(block), phi.PhiBlock(i)) == predecessors.at(block).end())
						return At("invalid_phi_predecessor", *block, phi);
				if (incoming.size() != predecessors.at(block).size()) return At("missing_phi_predecessor", *block, phi);
			}
		}
		if (returns != 1u) return Deny("terminal_return_contract");
		Blocks reachable;
		std::vector<const Block*> pending {program.blocks.front()};
		while (!pending.empty()) {
			const auto* block = pending.back(); pending.pop_back();
			if (!reachable.insert(block).second) continue;
			if (block->terminator.kind == CFG::TerminatorKind::Return) continue;
			pending.push_back(block->terminator.true_block);
			if (block->terminator.kind == CFG::TerminatorKind::ConditionalBranch)
				pending.push_back(block->terminator.false_block);
		}
		if (reachable.size() != program.blocks.size()) return Deny("unreachable_block");
		for (const auto& loop : loops) {
			bool back = false, entry = false;
			if (!loop.body.contains(loop.header->terminator.continue_block)) return Deny("continue_outside_loop", loop.header);
			for (const auto* member : loop.body) for (const auto* from : predecessors.at(member)) {
				if (member != loop.header && !loop.body.contains(from)) return Deny("loop_interior_entry", member);
				if (member == loop.header) { back |= loop.body.contains(from); entry |= !loop.body.contains(from); }
			}
			if (!back || !entry) return Deny("loop_missing_base_or_backedge", loop.header);
		}
		const auto backedge = [&](const Block* from, const Block* to) {
			return std::ranges::any_of(loops, [&](const auto& loop) { return loop.header == to && loop.body.contains(from); });
		};
		std::unordered_map<const Block*, uint32_t> incoming;
		for (const auto& [block, froms] : predecessors) {
			uint32_t count = 0;
			for (const auto* from : froms) count += !backedge(from, block);
			incoming.emplace(block, count);
		}
		pending.clear();
		for (const auto& [block, count] : incoming) if (count == 0u) pending.push_back(block);
		uint32_t visited = 0;
		while (!pending.empty()) {
			const auto* block = pending.back(); pending.pop_back(); ++visited;
			const auto edge = [&](const Block* to) {
				if (!backedge(block, to) && --incoming.at(to) == 0u) pending.push_back(to);
			};
			if (block->terminator.kind == CFG::TerminatorKind::Return) continue;
			edge(block->terminator.true_block);
			if (block->terminator.kind == CFG::TerminatorKind::ConditionalBranch) edge(block->terminator.false_block);
		}
		return visited == program.blocks.size() ? RasterQuadProof{.admitted = true} : Deny("unmodelled_cycle");
	}

	const Loop* ExitLoop(const Block& block) const {
		const Loop* found = nullptr;
		for (const auto& loop : loops) {
			if (!loop.body.contains(&block) ||
			    (block.terminator.true_block != loop.merge && block.terminator.false_block != loop.merge)) continue;
			if (!found || loop.body.size() < found->body.size()) found = &loop;
		}
		return found;
	}

public:
	// Infer only invariants with actual entry operands. Candidate invariants are
	// removed until EVERY retained recurrence has a base and inductive step.
	void Invariants(Cofactor& proof, const Blocks& region, const Block* current_header = nullptr) const {
		struct Candidate { const Inst* phi; Value base; std::vector<Value> entries, back; };
		std::vector<Candidate> candidates;
		for (const auto& loop : loops) {
			if (!region.contains(loop.header) || loop.header == current_header) continue;
			for (const auto& inst : *loop.header) {
				if (inst.GetOpcode() != ValueOpcode::Phi) continue;
				Candidate candidate {.phi = &inst};
				bool valid = true;
				for (size_t i = 0; i < inst.NumArgs(); ++i) {
					if (loop.body.contains(inst.PhiBlock(i))) candidate.back.push_back(inst.Arg(i));
					else {
						candidate.entries.push_back(inst.Arg(i));
						if (candidate.base.IsEmpty()) candidate.base = inst.Arg(i);
						else valid &= proof.Equal(candidate.base, inst.Arg(i));
					}
				}
				if (valid && !candidate.base.IsEmpty() && !candidate.back.empty()) {
					proof.Substitute(candidate.phi, candidate.base);
					candidates.push_back(std::move(candidate));
				}
			}
		}
		for (;;) {
			std::vector<const Inst*> remove;
			for (const auto& candidate : candidates) {
				bool valid = true;
				// Entry equalities can depend on another tentative invariant too.
				// Recheck them after EVERY removal, just like the inductive step.
				for (auto entry : candidate.entries) valid &= proof.Equal(candidate.base, entry);
				for (auto back : candidate.back) valid &= proof.Equal(candidate.base, back);
				if (!valid) remove.push_back(candidate.phi);
			}
			if (remove.empty()) break;
			for (auto* phi : remove) proof.RemoveSubstitution(phi);
			std::erase_if(candidates, [&](const auto& c) { return std::ranges::find(remove, c.phi) != remove.end(); });
		}
	}

private:
	void MaskImplications(Cofactor& proof) const {
		// A monotone loop mask can never re-enable an invocation absent at
		// entry. Prove the base and every recurrence; never infer it merely
		// from a Phi's register name or from a recursive walk closing a cycle.
		for (const auto& loop : loops) for (const auto& phi : *loop.header) {
			if (phi.GetOpcode() != ValueOpcode::Phi || phi.GetType() != Type::U1) continue;
			Value base;
			bool valid = true, back = false;
			for (size_t i = 0; i < phi.NumArgs(); ++i) {
				if (!loop.body.contains(phi.PhiBlock(i))) {
					if (base.IsEmpty()) base = phi.Arg(i).Resolve();
					else valid &= base == phi.Arg(i).Resolve();
					continue;
				}
				back = true;
				std::unordered_set<const Inst*> visited;
				const auto contains = [&](auto&& self, Value value) -> bool {
					value = value.Resolve();
					if (value == Value(const_cast<Inst*>(&phi)) || value == Value(false)) return true;
					const auto* inst = value.TryInstruction();
					if (!inst || !visited.insert(inst).second) return false;
					if (inst->GetOpcode() == ValueOpcode::LogicalAnd)
						return self(self, inst->Arg(0)) || self(self, inst->Arg(1));
					return false;
				};
				valid &= contains(contains, phi.Arg(i));
			}
			if (valid && back && !base.IsEmpty()) proof.RegisterImplication(&phi, base);
		}
	}

	// In the guest, every admitted branch is a scalar wave decision. A loop
	// Phi is wave-uniform only with uniform values on EVERY external entry
	// and on EVERY backedge under the retained simultaneous invariants.
	void UniformInvariants(Cofactor& proof) const {
		struct Candidate { const Inst* phi; std::vector<Value> back; };
		std::vector<Candidate> candidates;
		for (const auto& loop : loops) {
			for (const auto& inst : *loop.header) {
				if (inst.GetOpcode() != ValueOpcode::Phi) continue;
				Candidate candidate {.phi = &inst};
				bool valid = true, entry = false;
				for (size_t i = 0; i < inst.NumArgs(); ++i) {
					if (loop.body.contains(inst.PhiBlock(i))) candidate.back.push_back(inst.Arg(i));
					else { entry = true; valid &= proof.Uniform(inst.Arg(i)); }
				}
				if (valid && entry && !candidate.back.empty()) candidates.push_back(std::move(candidate));
			}
		}
		for (const auto& candidate : candidates) proof.MarkUniform(candidate.phi, true);
		for (;;) {
			std::vector<const Inst*> remove;
			for (const auto& candidate : candidates)
				for (const auto back : candidate.back)
					if (!proof.Uniform(back)) { remove.push_back(candidate.phi); break; }
			if (remove.empty()) break;
			for (auto* phi : remove) proof.MarkUniform(phi, false);
			std::erase_if(candidates, [&](const auto& c) { return std::ranges::find(remove, c.phi) != remove.end(); });
		}
	}

	RasterQuadProof Branch(const Block& block) {
		Value condition = block.condition.Resolve();
		const Inst* vote = condition.TryInstruction();
		while (vote && vote->GetOpcode() == ValueOpcode::LogicalNot) vote = vote->Arg(0).Resolve().TryInstruction();
		if (!vote || vote->GetOpcode() != ValueOpcode::ConditionRef) {
			Cofactor proof(program);
			return proof.Uniform(condition) ? RasterQuadProof{.admitted = true} : Deny("non_vote_control", &block);
		}
		const auto kind = vote->Flags<CFG::BranchCondition>();
		if (kind == CFG::BranchCondition::ScalarInstruction) {
			Cofactor proof(program);
			return proof.Uniform(vote->Arg(0)) ? RasterQuadProof{.admitted = true} : Deny("varying_scalar_control", &block);
		}
		const bool zero = kind == CFG::BranchCondition::ExecZero || kind == CFG::BranchCondition::VccZero || kind == CFG::BranchCondition::SccZero;
		if (!zero && kind != CFG::BranchCondition::ExecNonZero && kind != CFG::BranchCondition::VccNonZero && kind != CFG::BranchCondition::SccNonZero)
			return Deny("unknown_vote", &block);
		Value mask = vote->Arg(0).Resolve();
		if (zero) {
			const auto* negation = mask.TryInstruction();
			if (!negation || negation->GetOpcode() != ValueOpcode::LogicalNot) return Deny("noncanonical_zero_vote", &block);
			mask = negation->Arg(0).Resolve();
		}
		const auto* loop = ExitLoop(block);
		const auto* merge = loop ? loop->merge : block.terminator.merge_block;
		if (!merge) return Deny("missing_merge", &block);
		Blocks region;
		if (loop) region = loop->body;
		else if (!Region(block.terminator.true_block, merge, region) || !Region(block.terminator.false_block, merge, region)) return Deny("unclosed_region", &block);
		std::vector<Value> assumptions {mask};
		std::vector<Value> factors {mask};
		std::unordered_set<const Inst*> visited;
		while (!factors.empty()) {
			const auto factor = factors.back().Resolve(); factors.pop_back();
			const auto* inst = factor.TryInstruction();
			if (!inst || !visited.insert(inst).second) continue;
			if (factor != mask) {
				Cofactor active(program);
				MaskImplications(active);
				active.Assume(factor, true);
				UniformInvariants(active);
				// If the predicate is command-uniform wherever this execution
				// mask is true, only an entirely inactive partition can disagree.
				if (active.Uniform(mask)) assumptions.push_back(factor);
			}
			if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
				factors.push_back(inst->Arg(0)); factors.push_back(inst->Arg(1));
			}
		}
		RasterQuadProof failure = Deny("stutter_not_proved", &block);
		for (const auto assumption : assumptions) {
			const auto candidate = Stutter(block, *merge, region, assumption, loop);
			if (!candidate.admitted) { failure = candidate; continue; }
			if (loop) {
				const auto progress = AnalyzeLoopProgress<Cofactor>(program, *loop, block, mask);
				if (!progress.admitted) return Deny(progress.reason, &block);
				certified_loops.insert(loop->header);
			}
			return {.admitted = true};
		}
		return failure;
	}

	RasterQuadProof Stutter(const Block& block, const Block& merge, const Blocks& region, Value assumption, const Loop* loop = nullptr) const {
		Cofactor proof(program);
		proof.OpaquePhiOutside(region);
		proof.Assume(assumption, false);
		if (loop) for (const auto& phi : *loop->header)
			if (phi.GetOpcode() == ValueOpcode::Phi) proof.Freeze(&phi);
		Invariants(proof, region, loop ? loop->header : nullptr);
		if (loop) {
			// A quad may leave after any iteration. Preserve its CURRENT Phi
			// state, not a hypothetical entry state with the exit mask false.
			// Non-stuttering counters may exist, but can never escape this region
			// or influence an escaping value under the inactive cofactor.
			for (const auto& phi : *loop->header) {
				if (phi.GetOpcode() != ValueOpcode::Phi) continue;
				bool stable = true, back = false;
				for (size_t i = 0; i < phi.NumArgs(); ++i) if (region.contains(phi.PhiBlock(i))) {
					back = true;
					stable &= proof.Equal(phi.Arg(i), Value(const_cast<Inst*>(&phi)));
				}
				if (stable && back) proof.MarkOutside(&phi);
			}
		}
		for (const auto* member : region) {
			for (const auto& inst : *member) {
				for (const auto& use : inst.Uses()) {
					if (region.contains(use.user->Parent())) continue;
					if (use.user->Parent() == &merge && use.user->GetOpcode() == ValueOpcode::Phi) continue;
					if (!proof.Outside(Value(const_cast<Inst*>(&inst)), region)) return At("liveout_not_stutter", block, inst);
				}
			}
		}
		for (const auto* owner : program.blocks) {
			if (region.contains(owner)) continue;
			const auto condition = owner->condition.Resolve();
			const auto* source = condition.TryInstruction();
			if (source && region.contains(source->Parent()) && !proof.Outside(condition, region))
				return At("control_liveout_not_stutter", block, *source);
		}
		for (const auto& phi : merge) {
			if (phi.GetOpcode() != ValueOpcode::Phi) continue;
			Value first;
			for (size_t i = 0; i < phi.NumArgs(); ++i) {
				if (!region.contains(phi.PhiBlock(i)) && phi.PhiBlock(i) != &block) continue;
				if (loop && !proof.Outside(phi.Arg(i), region)) return At("loop_merge_liveout_not_stutter", block, phi);
				if (first.IsEmpty()) first = phi.Arg(i);
				else if (!proof.Equal(first, phi.Arg(i))) return At("merge_not_stutter", block, phi);
			}
		}
		return {.admitted = true};
	}
	const Program& program;
	Blocks known;
	std::vector<Loop> loops;
	Blocks certified_loops;
};

} // namespace RasterQuadDetail

inline RasterQuadProof AnalyzeRasterQuadPartition(const Program& program) {
	RasterQuadDetail::Analysis analysis(program);
	const auto control = analysis.Control();
	if (!control.admitted) return control;
	const auto operations = analysis.Operations();
	if (!operations.admitted) return operations;
	return {.admitted = true, .reason = "quad_partition_proved"};
}
} // namespace Libs::Graphics::ShaderRecompiler::IR
