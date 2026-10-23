#include "CpuA64HotRegion.h"

#include <algorithm>
#include <array>

namespace Lumen::Cpu::A64HotRegion {
namespace {

using A64Dynarec::IrEffect;
using A64Dynarec::IrEffectMask;
using A64Dynarec::IrInstruction;
using A64Dynarec::IrOpcode;

constexpr bool IsRegister(uint8_t value)
{
	return value < GPR_COUNT;
}

constexpr uint16_t RegisterBit(uint8_t value)
{
	return static_cast<uint16_t>(1u << value);
}

bool AddRead(InstructionGprAccess* access, uint8_t guest)
{
	if (access == nullptr || !IsRegister(guest)) return false;
	access->read_mask |= RegisterBit(guest);
	return true;
}

bool AddWrite(InstructionGprAccess* access, uint8_t guest, uint8_t width,
	          bool conditional = false)
{
	if (access == nullptr || !IsRegister(guest)) return false;
	const uint16_t bit = RegisterBit(guest);
	if (width == 1 || width == 2)
	{
		access->partial_write_mask |= bit;
		// An 8/16-bit x86 write preserves the remainder of the GPR.
		access->read_mask |= bit;
	}
	else if (width == 4 || width == 8)
	{
		// A 32-bit x86 write is a complete definition because it zeroes the
		// architectural upper half.
		access->full_write_mask |= bit;
	}
	else
	{
		return false;
	}
	if (conditional)
	{
		access->conditional_write_mask |= bit;
		// The not-taken path preserves the old complete value.
		access->read_mask |= bit;
	}
	return true;
}

bool AddReadWrite(InstructionGprAccess* access, uint8_t guest, uint8_t width,
	              bool conditional = false)
{
	return AddRead(access, guest) &&
	       AddWrite(access, guest, width, conditional);
}

bool IsTypedHelperOpcode(IrOpcode opcode)
{
	switch (opcode)
	{
		case IrOpcode::VectorConvertFloatToIntegerRegister:
		case IrOpcode::VectorConvertFloatToIntegerMemory:
		case IrOpcode::VectorConvertIntegerToFloatRegister:
		case IrOpcode::VectorConvertIntegerToFloatMemory:
		case IrOpcode::VectorBinaryRegister:
		case IrOpcode::VectorBinaryMemory:
		case IrOpcode::VectorHorizontalAddRegister:
		case IrOpcode::VectorUnpackLowDwordsMemory:
		case IrOpcode::VectorMoveGpr64ToXmm:
		case IrOpcode::VectorCompareRegister:
		case IrOpcode::VectorCompareMemory:
		case IrOpcode::CallIndirectRegister:
		case IrOpcode::CallIndirectMemory:
		case IrOpcode::JumpIndirectRegister:
		case IrOpcode::JumpIndirectMemory:
		case IrOpcode::RepeatMove:
		case IrOpcode::RepeatStore:
		case IrOpcode::AtomicCompareExchangeMemory: return true;
		default: return false;
	}
}

bool HasEffect(const IrInstruction& instruction, IrEffect effect)
{
	return (instruction.effects & IrEffectMask(effect)) != 0;
}

PlanResult Rejected(RejectReason reason)
{
	PlanResult result {};
	result.reject_reason = reason;
	return result;
}

bool ResolveSelectedSuccessor(const A64Dynarec::IrBlock& block,
	                          EdgeKind edge_kind, uint64_t* successor)
{
	if (successor == nullptr || block.instruction_count == 0) return false;
	if (!block.has_terminator)
	{
		if (edge_kind != EdgeKind::Fallthrough) return false;
		*successor = block.fallthrough_rip;
		return true;
	}

	const IrInstruction& tail =
	    block.instructions[block.instruction_count - 1u];
	if (tail.opcode == IrOpcode::JumpImmediate)
	{
		if (edge_kind != EdgeKind::JumpTarget) return false;
		*successor = tail.target;
		return true;
	}
	if (tail.opcode != IrOpcode::BranchCondition) return false;
	if (edge_kind == EdgeKind::BranchTaken)
	{
		*successor = tail.target;
		return true;
	}
	if (edge_kind == EdgeKind::BranchFallthrough)
	{
		*successor = block.fallthrough_rip;
		return true;
	}
	return false;
}

bool BetterCandidate(const ResidentCandidate& left,
	                 const ResidentCandidate& right)
{
	if (left.priority_score != right.priority_score)
		return left.priority_score > right.priority_score;
	if (left.boundary_crossings != right.boundary_crossings)
		return left.boundary_crossings > right.boundary_crossings;
	if (left.access_count != right.access_count)
		return left.access_count > right.access_count;
	if (left.read_count != right.read_count)
		return left.read_count > right.read_count;
	if (left.write_count != right.write_count)
		return left.write_count > right.write_count;
	return left.guest_register < right.guest_register;
}

} // namespace

const char* RejectReasonName(RejectReason reason) noexcept
{
	switch (reason)
	{
		case RejectReason::None: return "none";
		case RejectReason::InvalidArgument: return "invalid-argument";
		case RejectReason::TooFewSegments: return "too-few-segments";
		case RejectReason::TooManySegments: return "too-many-segments";
		case RejectReason::TooManyInstructions: return "too-many-instructions";
		case RejectReason::NotDirectBasicBlock: return "not-direct-basic-block";
		case RejectReason::HelperOpcode: return "helper-opcode";
		case RejectReason::GuestMemory: return "guest-memory";
		case RejectReason::MayFault: return "may-fault";
		case RejectReason::HostCall: return "host-call";
		case RejectReason::Stack: return "stack";
		case RejectReason::UnclassifiedOpcode: return "unclassified-opcode";
		case RejectReason::InvalidEdgeKind: return "invalid-edge-kind";
		case RejectReason::EdgeTargetMismatch: return "edge-target-mismatch";
		case RejectReason::Cycle: return "cycle";
	}
	return "unknown";
}

bool ClassifyInstruction(const IrInstruction& instruction,
	                     InstructionGprAccess* output) noexcept
{
	if (output == nullptr) return false;
	*output = {};
	switch (instruction.opcode)
	{
		case IrOpcode::Nop:
		case IrOpcode::ClearDirectionFlag:
		case IrOpcode::BranchCondition:
		case IrOpcode::JumpImmediate:
		case IrOpcode::VectorPermuteDwordsImmediate:
		case IrOpcode::VectorMoveRegister:
		case IrOpcode::VectorMoveScalarRegister:
		case IrOpcode::VectorXorRegister: return true;

		case IrOpcode::MoveRegister:
			return AddRead(output, instruction.source) &&
			       AddWrite(output, instruction.destination, instruction.width);
		case IrOpcode::MoveImmediate:
			return AddWrite(output, instruction.destination, instruction.width);
		case IrOpcode::ZeroExtendRegister:
		case IrOpcode::SignExtendRegister:
			return AddRead(output, instruction.source) &&
			       AddWrite(output, instruction.destination, instruction.width);
		case IrOpcode::EffectiveAddress:
			if (instruction.address.base != A64Dynarec::IR_NO_REGISTER &&
			    !AddRead(output, instruction.address.base))
			{
				return false;
			}
			if (instruction.address.index != A64Dynarec::IR_NO_REGISTER &&
			    !AddRead(output, instruction.address.index))
			{
				return false;
			}
			return AddWrite(output, instruction.destination, instruction.width);
		case IrOpcode::ByteSwapRegister:
		case IrOpcode::BitwiseNot:
		case IrOpcode::Negate:
			return AddReadWrite(output, instruction.destination,
			                    instruction.width);

		case IrOpcode::AddImmediate:
		case IrOpcode::SubImmediate:
			return AddReadWrite(output, instruction.destination,
			                    instruction.width);
		case IrOpcode::CompareImmediate:
			return AddRead(output, instruction.destination);
		case IrOpcode::AddRegister:
		case IrOpcode::SubRegister:
			return AddReadWrite(output, instruction.destination,
			                    instruction.width) &&
			       AddRead(output, instruction.source);
		case IrOpcode::CompareRegister:
			return AddRead(output, instruction.destination) &&
			       AddRead(output, instruction.source);

		case IrOpcode::ShiftLeftImmediate:
		case IrOpcode::ShiftRightLogicalImmediate:
		case IrOpcode::ShiftRightArithmeticImmediate:
			if (instruction.immediate == 0) return true;
			return AddReadWrite(output, instruction.destination,
			                    instruction.width);
		case IrOpcode::ShiftLeftCl:
		case IrOpcode::ShiftRightLogicalCl:
			return AddRead(output, 1) &&
			       AddReadWrite(output, instruction.destination,
			                    instruction.width, true);

		case IrOpcode::AndImmediate:
		case IrOpcode::OrImmediate:
		case IrOpcode::XorImmediate:
		case IrOpcode::Increment:
		case IrOpcode::Decrement:
			return AddReadWrite(output, instruction.destination,
			                    instruction.width);
		case IrOpcode::BitTestRegister:
			return AddRead(output, instruction.source) &&
			       AddRead(output, instruction.source2);
		case IrOpcode::BitTestImmediate:
		case IrOpcode::TestImmediate:
			return AddRead(output, instruction.source);
		case IrOpcode::SetCondition:
			return AddWrite(output, instruction.destination, 1);
		case IrOpcode::ConditionalMove:
			return AddRead(output, instruction.destination) &&
			       AddRead(output, instruction.source) &&
			       AddWrite(output, instruction.destination,
			                instruction.width, true);
		case IrOpcode::AndRegister:
		case IrOpcode::OrRegister:
		case IrOpcode::XorRegister:
		case IrOpcode::SubtractWithBorrowRegister:
			return AddReadWrite(output, instruction.destination,
			                    instruction.width) &&
			       AddRead(output, instruction.source);
		case IrOpcode::TestRegister:
			return AddRead(output, instruction.source) &&
			       AddRead(output, instruction.source2);
		case IrOpcode::BitExtractRegister:
		case IrOpcode::AndNotRegister:
		case IrOpcode::SignedMultiplyRegister:
			return AddRead(output, instruction.source) &&
			       AddRead(output, instruction.source2) &&
			       AddWrite(output, instruction.destination,
			                instruction.width);
		case IrOpcode::SignedMultiplyImmediate:
			return AddRead(output, instruction.source) &&
			       AddWrite(output, instruction.destination,
			                instruction.width);
		case IrOpcode::VectorMoveGpr32ToXmm:
			return AddRead(output, instruction.source);

		case IrOpcode::Load:
		case IrOpcode::Store:
		case IrOpcode::PushRegister:
		case IrOpcode::PopRegister:
		case IrOpcode::CallImmediate:
		case IrOpcode::Return:
		case IrOpcode::VectorLoad:
		case IrOpcode::VectorStore:
		case IrOpcode::VectorXorMemory:
		case IrOpcode::ZeroExtendMemory:
		case IrOpcode::SignExtendMemory:
		case IrOpcode::AddRegisterMemory:
		case IrOpcode::SubRegisterMemory:
		case IrOpcode::OrRegisterMemory:
		case IrOpcode::AndRegisterMemory:
		case IrOpcode::XorRegisterMemory:
		case IrOpcode::CompareRegisterMemory:
		case IrOpcode::CompareMemoryRegister:
		case IrOpcode::CompareMemoryImmediate:
		case IrOpcode::TestMemoryImmediate:
		case IrOpcode::SignedMultiplyRegisterMemory:
		case IrOpcode::SignedMultiplyMemoryImmediate:
		case IrOpcode::AddMemoryRegister:
		case IrOpcode::SubtractMemoryRegister:
		case IrOpcode::AndMemoryRegister:
		case IrOpcode::AndMemoryImmediate:
		case IrOpcode::IncrementMemory:
		case IrOpcode::SetConditionMemory:
		case IrOpcode::VectorLoadScalar:
		case IrOpcode::VectorStoreScalar:
		case IrOpcode::VectorConvertFloatToIntegerRegister:
		case IrOpcode::VectorConvertFloatToIntegerMemory:
		case IrOpcode::VectorConvertIntegerToFloatRegister:
		case IrOpcode::VectorConvertIntegerToFloatMemory:
		case IrOpcode::VectorBinaryRegister:
		case IrOpcode::VectorBinaryMemory:
		case IrOpcode::VectorHorizontalAddRegister:
		case IrOpcode::VectorUnpackLowDwordsMemory:
		case IrOpcode::VectorMoveGpr64ToXmm:
		case IrOpcode::VectorCompareRegister:
		case IrOpcode::VectorCompareMemory:
		case IrOpcode::CallIndirectRegister:
		case IrOpcode::CallIndirectMemory:
		case IrOpcode::JumpIndirectRegister:
		case IrOpcode::JumpIndirectMemory:
		case IrOpcode::RepeatMove:
		case IrOpcode::RepeatStore:
		case IrOpcode::AtomicCompareExchangeMemory:
		case IrOpcode::Count: return false;
	}
	return false;
}

PlanResult BuildPlan(const SegmentInput* inputs, size_t segment_count) noexcept
{
	if (inputs == nullptr) return Rejected(RejectReason::InvalidArgument);
	if (segment_count < MIN_SEGMENTS)
		return Rejected(RejectReason::TooFewSegments);
	if (segment_count > MAX_SEGMENTS)
		return Rejected(RejectReason::TooManySegments);

	Plan plan {};
	plan.segment_count = static_cast<uint8_t>(segment_count);
	uint16_t region_full_definitions = 0;
	uint16_t dirty_mask = 0;
	size_t flat_instruction = 0;

	for (size_t segment = 0; segment < segment_count; segment++)
	{
		if (inputs[segment].block == nullptr)
			return Rejected(RejectReason::InvalidArgument);
		const A64Dynarec::IrBlock& block = inputs[segment].block->Block();
		for (size_t previous = 0; previous < segment; previous++)
		{
			if (plan.segment_rips[previous] == block.rip)
				return Rejected(RejectReason::Cycle);
		}
		plan.segment_rips[segment] = block.rip;
		if (flat_instruction + block.instruction_count > MAX_INSTRUCTIONS)
			return Rejected(RejectReason::TooManyInstructions);

		SegmentSummary& summary = plan.segments[segment];
		summary.instruction_begin = static_cast<uint8_t>(flat_instruction);
		summary.instruction_count = block.instruction_count;
		uint16_t segment_full_definitions = 0;
		for (size_t instruction_index = 0;
		     instruction_index < block.instruction_count;
		     instruction_index++)
		{
			const IrInstruction& instruction =
			    block.instructions[instruction_index];
			if (IsTypedHelperOpcode(instruction.opcode))
				return Rejected(RejectReason::HelperOpcode);
			if (HasEffect(instruction, IrEffect::HostCall))
				return Rejected(RejectReason::HostCall);
			if (HasEffect(instruction, IrEffect::Stack))
				return Rejected(RejectReason::Stack);
			if (HasEffect(instruction, IrEffect::ReadGuestMemory) ||
			    HasEffect(instruction, IrEffect::WriteGuestMemory))
			{
				return Rejected(RejectReason::GuestMemory);
			}
			if (HasEffect(instruction, IrEffect::MayFault))
				return Rejected(RejectReason::MayFault);

			InstructionGprAccess access {};
			if (!ClassifyInstruction(instruction, &access))
				return Rejected(RejectReason::UnclassifiedOpcode);
			access.segment_index = static_cast<uint8_t>(segment);
			access.instruction_index =
			    static_cast<uint8_t>(instruction_index);
			plan.instruction_accesses[flat_instruction++] = access;

			summary.read_mask |= access.read_mask;
			summary.full_write_mask |= access.full_write_mask;
			summary.partial_write_mask |= access.partial_write_mask;
			summary.conditional_write_mask |=
			    access.conditional_write_mask;
			summary.read_before_full_definition_mask |=
			    access.read_mask &
			    static_cast<uint16_t>(~segment_full_definitions);
			plan.live_in_mask |=
			    access.read_mask &
			    static_cast<uint16_t>(~region_full_definitions);

			const uint16_t unconditional_full =
			    access.full_write_mask &
			    static_cast<uint16_t>(~access.conditional_write_mask);
			segment_full_definitions |= unconditional_full;
			region_full_definitions |= unconditional_full;
			dirty_mask |= access.full_write_mask |
			              access.partial_write_mask;
		}

		if (!inputs[segment].block->IsDirectBasicBlock())
			return Rejected(RejectReason::NotDirectBasicBlock);
		plan.boundaries[segment].dirty_mask = dirty_mask;
		// Every old basic-block boundary remains a legal side exit in this
		// tranche.  Resident state may cross on the hot path, but all dirty
		// values must be materializable there; no cross-boundary DSE is implied.
		plan.boundaries[segment].must_flush_mask = dirty_mask;
		plan.boundaries[segment].edge_kind =
		    inputs[segment].successor_kind;
		plan.boundaries[segment].successor_rip =
		    inputs[segment].successor_rip;
	}
	plan.instruction_count = static_cast<uint8_t>(flat_instruction);

	for (size_t segment = 0; segment < segment_count; segment++)
	{
		const A64Dynarec::IrBlock& block = inputs[segment].block->Block();
		uint64_t actual_successor = 0;
		if (!ResolveSelectedSuccessor(
		        block, inputs[segment].successor_kind, &actual_successor))
		{
			return Rejected(RejectReason::InvalidEdgeKind);
		}
		if (actual_successor != inputs[segment].successor_rip)
			return Rejected(RejectReason::EdgeTargetMismatch);

		for (size_t previous = 0; previous <= segment; previous++)
		{
			if (actual_successor == plan.segment_rips[previous])
				return Rejected(RejectReason::Cycle);
		}
		if (segment + 1u < segment_count &&
		    actual_successor != plan.segment_rips[segment + 1u])
		{
			return Rejected(RejectReason::EdgeTargetMismatch);
		}
		if (segment + 1u == segment_count)
		{
			for (size_t included = segment + 1u;
			     included < segment_count; included++)
			{
				if (actual_successor == plan.segment_rips[included])
					return Rejected(RejectReason::Cycle);
			}
		}
	}

	std::array<uint8_t, GPR_COUNT> first_segment {};
	std::array<uint8_t, GPR_COUNT> last_segment {};
	first_segment.fill(UINT8_MAX);
	std::array<ResidentCandidate, GPR_COUNT> candidates {};
	for (size_t instruction = 0; instruction < flat_instruction; instruction++)
	{
		const InstructionGprAccess& access =
		    plan.instruction_accesses[instruction];
		const uint16_t writes = access.full_write_mask |
		                        access.partial_write_mask;
		const uint16_t touched = access.read_mask | writes;
		for (uint8_t guest = 0; guest < GPR_COUNT; guest++)
		{
			const uint16_t bit = RegisterBit(guest);
			if ((touched & bit) == 0) continue;
			ResidentCandidate& candidate = candidates[guest];
			candidate.guest_register = guest;
			candidate.access_count++;
			if ((access.read_mask & bit) != 0) candidate.read_count++;
			if ((writes & bit) != 0) candidate.write_count++;
			candidate.has_partial_write |=
			    (access.partial_write_mask & bit) != 0;
			candidate.has_conditional_write |=
			    (access.conditional_write_mask & bit) != 0;
			candidate.segment_mask |= static_cast<uint8_t>(
			    1u << access.segment_index);
			if (first_segment[guest] == UINT8_MAX)
				first_segment[guest] = access.segment_index;
			last_segment[guest] = access.segment_index;
		}
	}

	for (uint8_t guest = 0; guest < GPR_COUNT; guest++)
	{
		ResidentCandidate candidate = candidates[guest];
		if (candidate.guest_register == A64Dynarec::IR_NO_REGISTER) continue;
		candidate.first_segment = first_segment[guest];
		candidate.last_segment = last_segment[guest];
		candidate.boundary_crossings = static_cast<uint8_t>(
		    candidate.last_segment - candidate.first_segment);
		candidate.live_in =
		    (plan.live_in_mask & RegisterBit(guest)) != 0;
		// A crossing dominates every possible within-segment access count.
		// Remaining low bits make ties transparent and deterministic; the
		// final comparator always falls back to the guest-register number.
		candidate.priority_score = static_cast<uint16_t>(
		    candidate.boundary_crossings * 1024u +
		    candidate.access_count * 8u +
		    (candidate.live_in ? 4u : 0u) +
		    (candidate.has_partial_write ? 0u : 2u) +
		    (candidate.has_conditional_write ? 0u : 1u));
		plan.resident_candidates[plan.resident_candidate_count++] = candidate;
	}
	std::sort(plan.resident_candidates.begin(),
	          plan.resident_candidates.begin() +
	              plan.resident_candidate_count,
	          BetterCandidate);

	PlanResult result {};
	result.reject_reason = RejectReason::None;
	result.plan = plan;
	return result;
}

} // namespace Lumen::Cpu::A64HotRegion
