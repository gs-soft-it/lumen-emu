// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR::RasterQuadDetail {

struct LoopProgress {
	bool admitted = false;
	const char* reason = "no_bounded_integer_counter";
	const Inst* counter = nullptr;
	Value bound;
	bool uniform_counter = false;
	bool inactive_hold = false;
	bool inactive_mask_stable = false;
};

namespace LoopProgressDetail {

template <class Proof, class LoopType>
void CurrentState(Proof& proof, const LoopType& loop) {
	proof.OpaquePhiOutside(loop.body);
	for (const auto& inst : *loop.header)
		if (inst.GetOpcode() == ValueOpcode::Phi) proof.Freeze(&inst);
}

// Early quad exit also requires that its mask cannot become true again while
// other quads keep the guest wave in the loop. This is separate from finite
// progress: (i < bound) && (per_lane_flag || i == 5) terminates but reactivates.
template <class Proof, class LoopType>
bool InactiveMaskStable(const Program& program, const LoopType& loop,
                        Value mask, const Inst* comparison) {
	Proof outside(program);
	CurrentState(outside, loop);
	std::vector<Value> pending {mask};
	std::vector<Value> factors;
	std::unordered_set<const Inst*> seen;
	while (!pending.empty() && seen.size() < 512u) {
		const auto value = pending.back().Resolve(); pending.pop_back();
		const auto* inst = value.TryInstruction();
		if (inst && !seen.insert(inst).second) continue;
		if (inst && inst->GetOpcode() == ValueOpcode::LogicalAnd) {
			pending.push_back(inst->Arg(0)); pending.push_back(inst->Arg(1));
		} else factors.push_back(value);
	}
	if (!pending.empty()) return false;
	bool invariant_factors = true;
	for (auto factor : factors) {
		const auto* inst = factor.Resolve().TryInstruction();
		if (inst == comparison) continue;
		invariant_factors &= outside.Outside(factor, loop.body);
		if (!inst || inst->GetOpcode() != ValueOpcode::Phi || inst->GetType() != Type::U1 ||
		    inst->Parent() != loop.header) continue;
		Proof inactive(program);
		CurrentState(inactive, loop);
		inactive.Assume(mask, false);
		bool entry = false, back = false, holds_false = true;
		std::unordered_set<const Block*> incoming;
		for (size_t i = 0; i < inst->NumArgs(); ++i) {
			const auto* predecessor = inst->PhiBlock(i);
			if (!predecessor || !incoming.insert(predecessor).second ||
			    std::ranges::find(loop.header->ImmPredecessors(), predecessor) ==
			        loop.header->ImmPredecessors().end()) { holds_false = false; break; }
			if (loop.body.contains(predecessor)) {
				back = true;
				holds_false &= inactive.Eval(inst->Arg(i)) == inactive.Eval(Value(false));
			} else entry = true;
		}
		if (entry && back && holds_false && incoming.size() == loop.header->ImmPredecessors().size())
			return true;
	}
	return invariant_factors;
}

using Choices = std::unordered_map<uint32_t, bool>;

template <class Proof>
std::optional<bool> Boolean(Proof& proof, uint32_t value, const Choices& choices, uint32_t depth = 0) {
	if (value <= 1u) return value != 0u;
	if (const auto found = choices.find(value); found != choices.end()) return found->second;
	if (depth >= 128u) return std::nullopt;
	const auto expression = proof.Expr(value);
	if (expression.op == ValueOpcode::LogicalNot && expression.args.size() == 1u) {
		const auto arg = Boolean(proof, expression.args[0], choices, depth + 1u);
		if (arg) return !*arg;
	}
	if ((expression.op == ValueOpcode::LogicalAnd || expression.op == ValueOpcode::LogicalOr) &&
	    expression.args.size() == 2u) {
		const auto a = Boolean(proof, expression.args[0], choices, depth + 1u);
		const auto b = Boolean(proof, expression.args[1], choices, depth + 1u);
		if (expression.op == ValueOpcode::LogicalAnd) {
			if ((a && !*a) || (b && !*b)) return false;
			if (a && b) return *a && *b;
		} else {
			if ((a && *a) || (b && *b)) return true;
			if (a && b) return *a || *b;
		}
	}
	return std::nullopt;
}

template <class Proof>
uint32_t ResolveChoice(Proof& proof, uint32_t value, const Choices& choices) {
	for (uint32_t depth = 0; depth < 128u; ++depth) {
		const auto expression = proof.Expr(value);
		if (expression.op != ValueOpcode::SelectU32 || expression.args.size() != 3u) break;
		const auto condition = Boolean(proof, expression.args[0], choices);
		if (!condition) break;
		value = expression.args[*condition ? 1u : 2u];
	}
	return value;
}

// Inspect the expression AFTER cofactoring. Unknown Select/Phi choices must
// all advance: choosing the convenient arm would not prove termination.
template <class Proof>
bool Advances(Proof& proof, uint32_t value, uint32_t counter, uint32_t bound,
              uint32_t one, bool allow_bound, std::unordered_set<uint32_t>& pending,
              Choices& choices, uint32_t& budget) {
	value = ResolveChoice(proof, value, choices);
	if (value == counter || budget == 0u) return false;
	--budget;
	if (allow_bound && value == bound) return true;
	if (!pending.insert(value).second) return false;
	// Eval may grow the expression arena: retain a copy across recursive calls.
	const auto expression = proof.Expr(value);
	bool result = false;
	if (expression.op == ValueOpcode::IAdd32 && expression.args.size() == 2u) {
		const auto a = ResolveChoice(proof, expression.args[0], choices);
		const auto b = ResolveChoice(proof, expression.args[1], choices);
		result = (a == counter && b == one) || (b == counter && a == one);
	} else if ((expression.op == ValueOpcode::SelectU32) && expression.args.size() == 3u) {
		const auto condition = expression.args[0];
		if (const auto known = Boolean(proof, condition, choices)) {
			result = Advances(proof, expression.args[*known ? 1u : 2u], counter, bound, one,
			                  allow_bound, pending, choices, budget);
		} else {
			choices.emplace(condition, true);
			result = Advances(proof, expression.args[1], counter, bound, one, allow_bound, pending, choices, budget);
			choices[condition] = false;
			result = Advances(proof, expression.args[2], counter, bound, one, allow_bound, pending, choices, budget) && result;
			choices.erase(condition);
		}
	} else if (const auto* inst = expression.atom.TryInstruction();
	           inst && inst->GetOpcode() == ValueOpcode::Phi && inst->NumArgs() != 0u) {
		result = true;
		for (size_t i = 0; i < inst->NumArgs(); ++i) {
			const auto incoming = proof.Eval(inst->Arg(i));
			if (!Advances(proof, incoming, counter, bound, one, allow_bound, pending, choices, budget)) {
				result = false;
				break;
			}
		}
	}
	pending.erase(value);
	return result;
}

template <class Proof>
bool Advances(Proof& proof, Value next, const Inst* counter, Value bound, bool allow_bound) {
	const auto counter_id = proof.Eval(Value(const_cast<Inst*>(counter)));
	const auto bound_id = proof.Eval(bound);
	const auto one = proof.Eval(Value(uint32_t{1}));
	const auto value = proof.Eval(next);
	std::unordered_set<uint32_t> pending;
	Choices choices;
	uint32_t budget = 4096u;
	return Advances(proof, value, counter_id, bound_id, one, allow_bound, pending, choices, budget);
}

} // namespace LoopProgressDetail

// This certifies FINITE PROGRESS, not partition equivalence. The caller must
// separately prove every observable live-out and side effect stutters when a
// quad leaves earlier. Proof is the cofactor evaluator; LoopType supplies the
// header, merge and complete body, including nested loops.
//
// A strict signed/unsigned i < bound test makes i+1 nonoverflowing while i is
// active. Jumping to the invariant bound also exits that lane. Inactive lanes
// must hold i, unless i is one common guest-wave counter advanced unconditionally
// by one from uniform entry values. In that latter case all lanes stop before
// its signed/unsigned maximum, and its unused inactive values need not stutter.
template <class Proof, class LoopType>
LoopProgress AnalyzeLoopProgress(const Program& program, const LoopType& loop,
                                 const Block& exit_block, Value mask) {
	if (!loop.header || !loop.merge || !loop.body.contains(&exit_block) ||
	    (exit_block.terminator.true_block != loop.merge &&
	     exit_block.terminator.false_block != loop.merge))
		return {.reason = "not_a_structured_loop_exit"};
	// Every iteration must evaluate the strict bound before reaching a body or
	// backedge. A bounded-looking exit elsewhere does not constrain a bypassing
	// continue path. Keep the accepted shape explicit rather than infer dominance.
	if (loop.header != &exit_block &&
	    (loop.header->terminator.kind != CFG::TerminatorKind::Branch ||
	     loop.header->terminator.true_block != &exit_block))
		return {.reason = "loop_exit_does_not_guard_iteration"};
	for (const auto* member : loop.body) {
		if (!member) return {.reason = "null_loop_member"};
		if (member == loop.header) continue;
		for (const auto* predecessor : member->ImmPredecessors())
			if (!loop.body.contains(predecessor)) return {.reason = "loop_has_interior_entry"};
	}
	// The rank decreases in the BODY, which must execute exactly when some
	// lane has the normalized mask set. Reversed exit polarity is not progress.
	auto condition = exit_block.condition.Resolve();
	bool inverted = false;
	uint32_t condition_depth = 0;
	const Inst* vote = condition.TryInstruction();
	while (vote && vote->GetOpcode() == ValueOpcode::LogicalNot && condition_depth++ < 64u) {
		inverted = !inverted;
		condition = vote->Arg(0).Resolve();
		vote = condition.TryInstruction();
	}
	if (!vote || vote->GetOpcode() != ValueOpcode::ConditionRef)
		return {.reason = "loop_exit_not_a_vote"};
	const auto kind = vote->Flags<CFG::BranchCondition>();
	const bool zero = kind == CFG::BranchCondition::ExecZero ||
	                  kind == CFG::BranchCondition::VccZero || kind == CFG::BranchCondition::SccZero;
	if (!zero && kind != CFG::BranchCondition::ExecNonZero &&
	    kind != CFG::BranchCondition::VccNonZero && kind != CFG::BranchCondition::SccNonZero)
		return {.reason = "loop_exit_vote_kind"};
	auto vote_mask = vote->Arg(0).Resolve();
	if (zero) {
		const auto* negation = vote_mask.TryInstruction();
		if (!negation || negation->GetOpcode() != ValueOpcode::LogicalNot)
			return {.reason = "loop_exit_mask_form"};
		vote_mask = negation->Arg(0).Resolve();
	}
	if (vote_mask != mask.Resolve() ||
	    ((zero != inverted) ? exit_block.terminator.true_block : exit_block.terminator.false_block) != loop.merge ||
	    ((zero != inverted) ? exit_block.terminator.false_block : exit_block.terminator.true_block) == loop.merge)
		return {.reason = "loop_exit_polarity"};
	std::vector<Value> factors {mask};
	std::unordered_set<const Inst*> visited;
	LoopProgress failure;
	while (!factors.empty() && visited.size() < 512u) {
		const auto value = factors.back().Resolve();
		factors.pop_back();
		const auto* comparison = value.TryInstruction();
		if (!comparison || !visited.insert(comparison).second) continue;
		const auto op = comparison->GetOpcode();
		if (op == ValueOpcode::LogicalAnd) {
			factors.push_back(comparison->Arg(0));
			factors.push_back(comparison->Arg(1));
			continue;
		}
		if (op != ValueOpcode::SLessThan32 && op != ValueOpcode::ULessThan32) continue;
		const auto* counter = comparison->Arg(0).Resolve().TryInstruction();
		if (!counter || counter->GetOpcode() != ValueOpcode::Phi ||
		    counter->Parent() != loop.header || counter->GetType() != Type::U32) continue;
		const auto bound = comparison->Arg(1).Resolve();
		failure = {.reason = "counter_metadata", .counter = counter, .bound = bound};
		std::vector<Value> entries, backs;
		std::unordered_set<const Block*> incoming;
		bool valid = true;
		for (size_t i = 0; i < counter->NumArgs(); ++i) {
			const auto* from = counter->PhiBlock(i);
			if (!from || !incoming.insert(from).second ||
			    std::ranges::find(loop.header->ImmPredecessors(), from) ==
			        loop.header->ImmPredecessors().end()) { valid = false; break; }
			(loop.body.contains(from) ? backs : entries).push_back(counter->Arg(i));
		}
		if (!valid || incoming.size() != loop.header->ImmPredecessors().size() ||
		    entries.empty() || backs.empty()) continue;
		Proof invariant(program);
		LoopProgressDetail::CurrentState(invariant, loop);
		if (!invariant.Outside(bound, loop.body)) {
			failure.reason = "loop_variant_bound";
			continue;
		}
		Proof active(program);
		LoopProgressDetail::CurrentState(active, loop);
		active.Assume(mask, true);
		bool active_progress = true;
		for (auto back : backs)
			active_progress &= LoopProgressDetail::Advances(active, back, counter, bound, true);
		if (!active_progress) {
			failure.reason = "active_counter_not_advancing";
			continue;
		}
		Proof inactive(program);
		LoopProgressDetail::CurrentState(inactive, loop);
		inactive.Assume(mask, false);
		bool inactive_hold = true;
		for (auto back : backs)
			inactive_hold &= inactive.Equal(back, Value(const_cast<Inst*>(counter)));
		Proof unconditional(program);
		LoopProgressDetail::CurrentState(unconditional, loop);
		bool uniform_counter = true;
		for (auto entry : entries) uniform_counter &= unconditional.Uniform(entry);
		for (auto back : backs)
			uniform_counter &= LoopProgressDetail::Advances(unconditional, back, counter, bound, false);
		if (!inactive_hold && !uniform_counter) {
			failure.reason = "inactive_counter_reactivation";
			continue;
		}
		if (!LoopProgressDetail::InactiveMaskStable<Proof>(program, loop, mask, comparison)) {
			failure.reason = "loop_mask_can_reactivate";
			continue;
		}
		return {.admitted = true, .reason = "bounded_integer_progress", .counter = counter,
		        .bound = bound, .uniform_counter = uniform_counter, .inactive_hold = inactive_hold,
		        .inactive_mask_stable = true};
	}
	return failure;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR::RasterQuadDetail
