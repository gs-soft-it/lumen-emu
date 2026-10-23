#include "CpuA64HotRegion.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

using Lumen::Cpu::A64Dynarec::IrInstruction;
using Lumen::Cpu::A64Dynarec::IrOpcode;
using Lumen::Cpu::A64Dynarec::LowerFastOpsVerified;
using Lumen::Cpu::A64Dynarec::Status;
using Lumen::Cpu::A64Dynarec::VerifiedIrResult;
using Lumen::Cpu::A64HotRegion::BuildPlan;
using Lumen::Cpu::A64HotRegion::ClassifyInstruction;
using Lumen::Cpu::A64HotRegion::EdgeKind;
using Lumen::Cpu::A64HotRegion::InstructionGprAccess;
using Lumen::Cpu::A64HotRegion::PlanResult;
using Lumen::Cpu::A64HotRegion::RejectReason;
using Lumen::Cpu::A64HotRegion::SegmentInput;
using Lumen::Cpu::X64Fast::FastOp;
using Lumen::Cpu::X64Fast::FastOpKind;

int g_failures = 0;

void Expect(bool condition, const char* expression, int line)
{
	if (condition) return;
	std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, line,
	             expression);
	g_failures++;
}

#define EXPECT_TRUE(expression) Expect((expression), #expression, __LINE__)

constexpr uint16_t Bit(uint8_t guest)
{
	return static_cast<uint16_t>(1u << guest);
}

FastOp Operation(FastOpKind kind, uint64_t rip, uint8_t length = 1)
{
	FastOp operation {};
	operation.kind = kind;
	operation.index = Lumen::Cpu::A64Dynarec::IR_NO_REGISTER;
	operation.length = length;
	operation.rip = rip;
	return operation;
}

VerifiedIrResult Verify(const std::vector<FastOp>& operations)
{
	const auto result = LowerFastOpsVerified(operations.data(), operations.size());
	EXPECT_TRUE(result.status == Status::Ok);
	EXPECT_TRUE(result.block.has_value());
	return result;
}

const Lumen::Cpu::A64Dynarec::VerifiedIrBlock* Block(
	const VerifiedIrResult& result)
{
	return result.block.has_value() ? &*result.block : nullptr;
}

std::vector<FastOp> Nops(uint64_t rip, size_t count)
{
	std::vector<FastOp> operations;
	operations.reserve(count);
	for (size_t index = 0; index < count; index++)
		operations.push_back(Operation(FastOpKind::Nop, rip + index));
	return operations;
}

void TestExhaustiveDirectOpcodeClassification()
{
	constexpr std::array<IrOpcode, 47> direct_opcodes = {
	    IrOpcode::Nop,
	    IrOpcode::MoveRegister,
	    IrOpcode::MoveImmediate,
	    IrOpcode::AddImmediate,
	    IrOpcode::SubImmediate,
	    IrOpcode::CompareImmediate,
	    IrOpcode::AddRegister,
	    IrOpcode::SubRegister,
	    IrOpcode::CompareRegister,
	    IrOpcode::ShiftLeftImmediate,
	    IrOpcode::ShiftLeftCl,
	    IrOpcode::ShiftRightLogicalImmediate,
	    IrOpcode::ShiftRightLogicalCl,
	    IrOpcode::ShiftRightArithmeticImmediate,
	    IrOpcode::AndImmediate,
	    IrOpcode::OrImmediate,
	    IrOpcode::XorImmediate,
	    IrOpcode::BitTestRegister,
	    IrOpcode::BitTestImmediate,
	    IrOpcode::Increment,
	    IrOpcode::Decrement,
	    IrOpcode::SetCondition,
	    IrOpcode::ConditionalMove,
	    IrOpcode::ClearDirectionFlag,
	    IrOpcode::BitwiseNot,
	    IrOpcode::Negate,
	    IrOpcode::TestImmediate,
	    IrOpcode::AndRegister,
	    IrOpcode::OrRegister,
	    IrOpcode::XorRegister,
	    IrOpcode::TestRegister,
	    IrOpcode::SubtractWithBorrowRegister,
	    IrOpcode::BitExtractRegister,
	    IrOpcode::AndNotRegister,
	    IrOpcode::SignedMultiplyRegister,
	    IrOpcode::SignedMultiplyImmediate,
	    IrOpcode::VectorMoveGpr32ToXmm,
	    IrOpcode::VectorPermuteDwordsImmediate,
	    IrOpcode::VectorMoveRegister,
	    IrOpcode::VectorMoveScalarRegister,
	    IrOpcode::VectorXorRegister,
	    IrOpcode::ZeroExtendRegister,
	    IrOpcode::SignExtendRegister,
	    IrOpcode::EffectiveAddress,
	    IrOpcode::ByteSwapRegister,
	    IrOpcode::BranchCondition,
	    IrOpcode::JumpImmediate,
	};

	for (uint16_t ordinal = 0;
	     ordinal < static_cast<uint16_t>(IrOpcode::Count); ordinal++)
	{
		const IrOpcode opcode = static_cast<IrOpcode>(ordinal);
		bool expected = false;
		for (const IrOpcode direct : direct_opcodes)
			expected |= direct == opcode;

		IrInstruction instruction {};
		instruction.opcode = opcode;
		instruction.width = 8;
		instruction.destination = 2;
		instruction.source = 3;
		instruction.source2 = 4;
		instruction.address.base = 5;
		instruction.address.index = 6;
		instruction.immediate = 1;
		InstructionGprAccess access {};
		EXPECT_TRUE(ClassifyInstruction(instruction, &access) == expected);
	}

	EXPECT_TRUE(!ClassifyInstruction(IrInstruction {}, nullptr));
}

void TestInstructionMasks()
{
	InstructionGprAccess access {};
	IrInstruction instruction {};
	instruction.opcode = IrOpcode::MoveImmediate;
	instruction.destination = 3;
	instruction.width = 1;
	EXPECT_TRUE(ClassifyInstruction(instruction, &access));
	EXPECT_TRUE(access.read_mask == Bit(3));
	EXPECT_TRUE(access.partial_write_mask == Bit(3));
	EXPECT_TRUE(access.full_write_mask == 0 &&
	            access.conditional_write_mask == 0);

	instruction.width = 4;
	EXPECT_TRUE(ClassifyInstruction(instruction, &access));
	EXPECT_TRUE(access.read_mask == 0 &&
	            access.full_write_mask == Bit(3) &&
	            access.partial_write_mask == 0);

	instruction = {};
	instruction.opcode = IrOpcode::MoveRegister;
	instruction.width = 8;
	instruction.destination = 2;
	instruction.source = 2;
	EXPECT_TRUE(ClassifyInstruction(instruction, &access));
	EXPECT_TRUE(access.read_mask == Bit(2) &&
	            access.full_write_mask == Bit(2));

	instruction = {};
	instruction.opcode = IrOpcode::ConditionalMove;
	instruction.width = 8;
	instruction.destination = 4;
	instruction.source = 5;
	EXPECT_TRUE(ClassifyInstruction(instruction, &access));
	EXPECT_TRUE(access.read_mask == (Bit(4) | Bit(5)) &&
	            access.full_write_mask == Bit(4) &&
	            access.conditional_write_mask == Bit(4));

	instruction = {};
	instruction.opcode = IrOpcode::ShiftLeftCl;
	instruction.width = 8;
	instruction.destination = 1;
	EXPECT_TRUE(ClassifyInstruction(instruction, &access));
	EXPECT_TRUE(access.read_mask == Bit(1) &&
	            access.full_write_mask == Bit(1) &&
	            access.conditional_write_mask == Bit(1));

	instruction = {};
	instruction.opcode = IrOpcode::ShiftLeftImmediate;
	instruction.width = 8;
	instruction.destination = 7;
	instruction.immediate = 0;
	EXPECT_TRUE(ClassifyInstruction(instruction, &access));
	EXPECT_TRUE(access.read_mask == 0 && access.full_write_mask == 0 &&
	            access.partial_write_mask == 0 &&
	            access.conditional_write_mask == 0);

	instruction = {};
	instruction.opcode = IrOpcode::EffectiveAddress;
	instruction.width = 8;
	instruction.destination = 7;
	instruction.address.base = 8;
	instruction.address.index = 9;
	EXPECT_TRUE(ClassifyInstruction(instruction, &access));
	EXPECT_TRUE(access.read_mask == (Bit(8) | Bit(9)) &&
	            access.full_write_mask == Bit(7));
}

void TestPlanLivenessDirtyAndRanking()
{
	constexpr uint64_t rip = UINT64_C(0x100100000);
	std::vector<FastOp> first;
	FastOp move64 = Operation(FastOpKind::MovImmReg64, rip);
	move64.dst = 0;
	move64.target = 7;
	first.push_back(move64);
	FastOp move8 = Operation(FastOpKind::MovImmReg8, rip + 1);
	move8.dst = 1;
	move8.immediate = 3;
	first.push_back(move8);
	FastOp alias = Operation(FastOpKind::MovReg64, rip + 2);
	alias.dst = 0;
	alias.src = 2;
	first.push_back(alias);
	auto first_block = Verify(first);

	std::vector<FastOp> second;
	FastOp cmov = Operation(FastOpKind::CmovccReg64, rip + 3);
	cmov.dst = 2;
	cmov.src = 3;
	cmov.condition = 4;
	second.push_back(cmov);
	FastOp shift = Operation(FastOpKind::ShlReg64Cl, rip + 4);
	shift.dst = 4;
	second.push_back(shift);
	auto second_block = Verify(second);
	if (!Block(first_block) || !Block(second_block)) return;

	const std::array<SegmentInput, 2> inputs = {{
	    {Block(first_block), EdgeKind::Fallthrough, rip + 3},
	    {Block(second_block), EdgeKind::Fallthrough, rip + 5},
	}};
	const PlanResult result = BuildPlan(inputs.data(), inputs.size());
	EXPECT_TRUE(result.Accepted());
	if (!result.Accepted()) return;
	const auto& plan = result.plan;
	EXPECT_TRUE(plan.segment_count == 2 && plan.instruction_count == 5);
	EXPECT_TRUE(plan.live_in_mask == (Bit(1) | Bit(3) | Bit(4)));
	EXPECT_TRUE(plan.segments[0].read_before_full_definition_mask == Bit(1));
	EXPECT_TRUE(plan.segments[1].read_before_full_definition_mask ==
	            (Bit(1) | Bit(2) | Bit(3) | Bit(4)));
	const uint16_t first_dirty = Bit(0) | Bit(1) | Bit(2);
	const uint16_t second_dirty = first_dirty | Bit(4);
	EXPECT_TRUE(plan.boundaries[0].dirty_mask == first_dirty &&
	            plan.boundaries[0].must_flush_mask == first_dirty);
	EXPECT_TRUE(plan.boundaries[1].dirty_mask == second_dirty &&
	            plan.boundaries[1].must_flush_mask == second_dirty);
	EXPECT_TRUE(plan.instruction_accesses[1].partial_write_mask == Bit(1));
	EXPECT_TRUE(plan.instruction_accesses[3].conditional_write_mask == Bit(2));
	EXPECT_TRUE(plan.instruction_accesses[4].read_mask == (Bit(1) | Bit(4)));

	bool found_crossing_two = false;
	for (size_t index = 0; index < plan.resident_candidate_count; index++)
	{
		const auto& candidate = plan.resident_candidates[index];
		if (candidate.guest_register == 2)
		{
			found_crossing_two = candidate.boundary_crossings == 1 &&
			                     candidate.access_count == 2 &&
			                     candidate.has_conditional_write;
		}
	}
	EXPECT_TRUE(found_crossing_two);
	for (size_t index = 1; index < plan.resident_candidate_count; index++)
	{
		const auto& previous = plan.resident_candidates[index - 1u];
		const auto& current = plan.resident_candidates[index];
		EXPECT_TRUE(previous.priority_score > current.priority_score ||
		            (previous.priority_score == current.priority_score &&
		             previous.guest_register < current.guest_register));
	}
}

void TestDeterministicRankingTie()
{
	constexpr uint64_t rip = UINT64_C(0x100110000);
	std::vector<FastOp> first;
	for (uint8_t guest : {uint8_t {6}, uint8_t {5}})
	{
		FastOp compare = Operation(FastOpKind::CmpReg64Imm,
		                           rip + first.size());
		compare.dst = guest;
		first.push_back(compare);
	}
	auto first_block = Verify(first);
	std::vector<FastOp> second;
	for (uint8_t guest : {uint8_t {6}, uint8_t {5}})
	{
		FastOp compare = Operation(FastOpKind::CmpReg64Imm,
		                           rip + 2 + second.size());
		compare.dst = guest;
		second.push_back(compare);
	}
	auto second_block = Verify(second);
	if (!Block(first_block) || !Block(second_block)) return;
	const std::array<SegmentInput, 2> inputs = {{
	    {Block(first_block), EdgeKind::Fallthrough, rip + 2},
	    {Block(second_block), EdgeKind::Fallthrough, rip + 4},
	}};
	const auto result = BuildPlan(inputs.data(), inputs.size());
	EXPECT_TRUE(result.Accepted());
	if (!result.Accepted()) return;
	EXPECT_TRUE(result.plan.resident_candidate_count == 2);
	EXPECT_TRUE(result.plan.resident_candidates[0].guest_register == 5 &&
	            result.plan.resident_candidates[1].guest_register == 6 &&
	            result.plan.resident_candidates[0].priority_score ==
	                result.plan.resident_candidates[1].priority_score);
}

void TestEdgesAndCycles()
{
	constexpr uint64_t rip = UINT64_C(0x100120000);

	std::vector<FastOp> taken_ops;
	FastOp compare = Operation(FastOpKind::CmpReg64Imm, rip);
	compare.dst = 0;
	taken_ops.push_back(compare);
	FastOp branch = Operation(FastOpKind::Jcc, rip + 1, 2);
	branch.condition = 5;
	branch.target = rip + 0x100;
	taken_ops.push_back(branch);
	auto taken = Verify(taken_ops);
	auto taken_successor = Verify(Nops(rip + 0x100, 1));
	if (!Block(taken) || !Block(taken_successor)) return;
	std::array<SegmentInput, 2> inputs = {{
	    {Block(taken), EdgeKind::BranchTaken, rip + 0x100},
	    {Block(taken_successor), EdgeKind::Fallthrough, rip + 0x101},
	}};
	EXPECT_TRUE(BuildPlan(inputs.data(), inputs.size()).Accepted());
	inputs[0].successor_kind = EdgeKind::BranchFallthrough;
	EXPECT_TRUE(BuildPlan(inputs.data(), inputs.size()).reject_reason ==
	            RejectReason::EdgeTargetMismatch);
	inputs[0].successor_kind = EdgeKind::JumpTarget;
	EXPECT_TRUE(BuildPlan(inputs.data(), inputs.size()).reject_reason ==
	            RejectReason::InvalidEdgeKind);

	const uint64_t fallthrough = rip + 3;
	auto fallthrough_successor = Verify(Nops(fallthrough, 1));
	if (!Block(fallthrough_successor)) return;
	inputs = {{
	    {Block(taken), EdgeKind::BranchFallthrough, fallthrough},
	    {Block(fallthrough_successor), EdgeKind::Fallthrough,
	     fallthrough + 1},
	}};
	EXPECT_TRUE(BuildPlan(inputs.data(), inputs.size()).Accepted());

	FastOp jump = Operation(FastOpKind::Jmp, rip + 0x200, 2);
	jump.target = rip + 0x300;
	auto jump_block = Verify({jump});
	auto jump_successor = Verify(Nops(rip + 0x300, 1));
	if (!Block(jump_block) || !Block(jump_successor)) return;
	inputs = {{
	    {Block(jump_block), EdgeKind::JumpTarget, rip + 0x300},
	    {Block(jump_successor), EdgeKind::Fallthrough, rip + 0x301},
	}};
	EXPECT_TRUE(BuildPlan(inputs.data(), inputs.size()).Accepted());

	auto cycle_first = Verify(Nops(rip + 0x400, 1));
	FastOp back = Operation(FastOpKind::Jmp, rip + 0x401, 2);
	back.target = rip + 0x400;
	auto cycle_second = Verify({back});
	if (!Block(cycle_first) || !Block(cycle_second)) return;
	inputs = {{
	    {Block(cycle_first), EdgeKind::Fallthrough, rip + 0x401},
	    {Block(cycle_second), EdgeKind::JumpTarget, rip + 0x400},
	}};
	EXPECT_TRUE(BuildPlan(inputs.data(), inputs.size()).reject_reason ==
	            RejectReason::Cycle);
}

void TestLimitsAndRejectReasons()
{
	constexpr uint64_t rip = UINT64_C(0x100130000);
	auto single = Verify(Nops(rip, 1));
	SegmentInput one {Block(single), EdgeKind::Fallthrough, rip + 1};
	EXPECT_TRUE(BuildPlan(&one, 1).reject_reason ==
	            RejectReason::TooFewSegments);
	EXPECT_TRUE(BuildPlan(nullptr, 2).reject_reason ==
	            RejectReason::InvalidArgument);
	std::array<SegmentInput, 9> nine {};
	EXPECT_TRUE(BuildPlan(nine.data(), nine.size()).reject_reason ==
	            RejectReason::TooManySegments);

	std::vector<VerifiedIrResult> blocks;
	blocks.reserve(8);
	uint64_t next_rip = rip + 0x100;
	for (size_t segment = 0; segment < 8; segment++)
	{
		blocks.push_back(Verify(Nops(next_rip, 8)));
		next_rip += 8;
	}
	std::array<SegmentInput, 8> inputs {};
	for (size_t segment = 0; segment < inputs.size(); segment++)
	{
		inputs[segment] = {
		    Block(blocks[segment]), EdgeKind::Fallthrough,
		    blocks[segment].block->Block().fallthrough_rip};
	}
	EXPECT_TRUE(BuildPlan(inputs.data(), inputs.size()).Accepted());

	blocks.clear();
	blocks.reserve(8);
	next_rip = rip + 0x200;
	for (size_t segment = 0; segment < 8; segment++)
	{
		const size_t count = segment == 0 ? 9 : 8;
		blocks.push_back(Verify(Nops(next_rip, count)));
		next_rip += count;
	}
	for (size_t segment = 0; segment < inputs.size(); segment++)
	{
		inputs[segment] = {
		    Block(blocks[segment]), EdgeKind::Fallthrough,
		    blocks[segment].block->Block().fallthrough_rip};
	}
	const auto oversized = BuildPlan(inputs.data(), inputs.size());
	EXPECT_TRUE(oversized.reject_reason == RejectReason::TooManyInstructions);
	EXPECT_TRUE(oversized.plan.segment_count == 0 &&
	            oversized.plan.instruction_count == 0);

	FastOp load = Operation(FastOpKind::MovReg64Mem, rip + 0x300, 3);
	load.dst = 0;
	load.base = 7;
	load.index = Lumen::Cpu::A64Dynarec::IR_NO_REGISTER;
	auto memory = Verify({load});
	auto memory_next = Verify(Nops(rip + 0x303, 1));
	std::array<SegmentInput, 2> rejected = {{
	    {Block(memory), EdgeKind::Fallthrough, rip + 0x303},
	    {Block(memory_next), EdgeKind::Fallthrough, rip + 0x304},
	}};
	EXPECT_TRUE(BuildPlan(rejected.data(), rejected.size()).reject_reason ==
	            RejectReason::GuestMemory);

	FastOp helper = Operation(FastOpKind::VaddssReg, rip + 0x400, 5);
	helper.dst = 15;
	helper.src = 14;
	helper.base = 12;
	auto helper_block = Verify({helper});
	auto helper_next = Verify(Nops(rip + 0x405, 1));
	rejected = {{
	    {Block(helper_block), EdgeKind::Fallthrough, rip + 0x405},
	    {Block(helper_next), EdgeKind::Fallthrough, rip + 0x406},
	}};
	EXPECT_TRUE(BuildPlan(rejected.data(), rejected.size()).reject_reason ==
	            RejectReason::HelperOpcode);

	FastOp push = Operation(FastOpKind::PushReg64, rip + 0x500);
	push.src = 2;
	auto stack = Verify({push});
	auto stack_next = Verify(Nops(rip + 0x501, 1));
	rejected = {{
	    {Block(stack), EdgeKind::Fallthrough, rip + 0x501},
	    {Block(stack_next), EdgeKind::Fallthrough, rip + 0x502},
	}};
	EXPECT_TRUE(BuildPlan(rejected.data(), rejected.size()).reject_reason ==
	            RejectReason::Stack);

	FastOp call = Operation(FastOpKind::Call, rip + 0x600, 5);
	call.target = rip + 0x900;
	auto host_call = Verify({call});
	auto call_next = Verify(Nops(rip + 0x605, 1));
	rejected = {{
	    {Block(host_call), EdgeKind::Fallthrough, rip + 0x605},
	    {Block(call_next), EdgeKind::Fallthrough, rip + 0x606},
	}};
	EXPECT_TRUE(BuildPlan(rejected.data(), rejected.size()).reject_reason ==
	            RejectReason::HostCall);

	EXPECT_TRUE(Lumen::Cpu::A64HotRegion::RejectReasonName(
	                RejectReason::MayFault) != nullptr &&
	            std::strcmp(Lumen::Cpu::A64HotRegion::RejectReasonName(
	                            RejectReason::MayFault),
	                        "may-fault") == 0);
}

InstructionGprAccess ExpectedRandomAccess(const FastOp& operation)
{
	InstructionGprAccess access {};
	const auto read = [&access](uint8_t guest) { access.read_mask |= Bit(guest); };
	const auto full = [&access](uint8_t guest) { access.full_write_mask |= Bit(guest); };
	const auto partial = [&access, &read](uint8_t guest) {
		read(guest);
		access.partial_write_mask |= Bit(guest);
	};
	switch (operation.kind)
	{
		case FastOpKind::Nop: break;
		case FastOpKind::MovImmReg64: full(operation.dst); break;
		case FastOpKind::MovImmReg8: partial(operation.dst); break;
		case FastOpKind::MovReg64:
			read(operation.dst);
			full(operation.src);
			break;
		case FastOpKind::AddReg64Imm:
			read(operation.dst);
			full(operation.dst);
			break;
		case FastOpKind::AddReg8Imm:
			partial(operation.dst);
			break;
		case FastOpKind::CmovccReg64:
			read(operation.dst);
			read(operation.src);
			full(operation.dst);
			access.conditional_write_mask |= Bit(operation.dst);
			break;
		case FastOpKind::ShlReg64Cl:
			read(1);
			read(operation.dst);
			full(operation.dst);
			access.conditional_write_mask |= Bit(operation.dst);
			break;
		case FastOpKind::CmpReg64Imm: read(operation.dst); break;
		default: EXPECT_TRUE(false); break;
	}
	return access;
}

uint32_t NextRandom(uint32_t* state)
{
	*state = *state * 1664525u + 1013904223u;
	return *state;
}

void TestDeterministicRandomCorpus()
{
	constexpr std::array<FastOpKind, 9> kinds = {
	    FastOpKind::Nop,
	    FastOpKind::MovImmReg64,
	    FastOpKind::MovImmReg8,
	    FastOpKind::MovReg64,
	    FastOpKind::AddReg64Imm,
	    FastOpKind::AddReg8Imm,
	    FastOpKind::CmovccReg64,
	    FastOpKind::ShlReg64Cl,
	    FastOpKind::CmpReg64Imm,
	};
	uint32_t random = 0x4c554d45u;
	uint64_t region_rip = UINT64_C(0x200000000);
	for (size_t iteration = 0; iteration < 128; iteration++)
	{
		const size_t segment_count = 2 + NextRandom(&random) % 7;
		std::vector<VerifiedIrResult> blocks;
		blocks.reserve(segment_count);
		std::vector<InstructionGprAccess> expected;
		std::array<size_t, 8> segment_ends {};
		uint64_t next_rip = region_rip;
		for (size_t segment = 0; segment < segment_count; segment++)
		{
			const size_t instruction_count = 1 + NextRandom(&random) % 8;
			std::vector<FastOp> operations;
			operations.reserve(instruction_count);
			for (size_t index = 0; index < instruction_count; index++)
			{
				FastOp operation = Operation(
				    kinds[NextRandom(&random) % kinds.size()], next_rip++);
				operation.dst = static_cast<uint8_t>(NextRandom(&random) % 16);
				operation.src = static_cast<uint8_t>(NextRandom(&random) % 16);
				if (operation.kind == FastOpKind::CmovccReg64)
				{
					operation.condition =
					    static_cast<uint8_t>(NextRandom(&random) % 16);
				}
				else
				{
					(void)NextRandom(&random);
				}
				operation.immediate = static_cast<int32_t>(NextRandom(&random) | 1u);
				operation.target = NextRandom(&random);
				operations.push_back(operation);
				expected.push_back(ExpectedRandomAccess(operation));
			}
			blocks.push_back(Verify(operations));
			segment_ends[segment] = expected.size();
		}

		std::array<SegmentInput, 8> inputs {};
		for (size_t segment = 0; segment < segment_count; segment++)
		{
			inputs[segment] = {
			    Block(blocks[segment]), EdgeKind::Fallthrough,
			    blocks[segment].block->Block().fallthrough_rip};
		}
		const auto result = BuildPlan(inputs.data(), segment_count);
		EXPECT_TRUE(result.Accepted());
		if (!result.Accepted()) continue;
		EXPECT_TRUE(result.plan.instruction_count == expected.size());

		uint16_t defined = 0;
		uint16_t live_in = 0;
		uint16_t dirty = 0;
		size_t segment = 0;
		for (size_t index = 0; index < expected.size(); index++)
		{
			const auto& actual = result.plan.instruction_accesses[index];
			const auto& wanted = expected[index];
			EXPECT_TRUE(actual.read_mask == wanted.read_mask &&
			            actual.full_write_mask == wanted.full_write_mask &&
			            actual.partial_write_mask == wanted.partial_write_mask &&
			            actual.conditional_write_mask ==
			                wanted.conditional_write_mask);
			live_in |= wanted.read_mask & static_cast<uint16_t>(~defined);
			defined |= wanted.full_write_mask &
			           static_cast<uint16_t>(~wanted.conditional_write_mask);
			dirty |= wanted.full_write_mask | wanted.partial_write_mask;
			if (index + 1u == segment_ends[segment])
			{
				EXPECT_TRUE(result.plan.boundaries[segment].dirty_mask == dirty &&
				            result.plan.boundaries[segment].must_flush_mask == dirty);
				segment++;
			}
		}
		EXPECT_TRUE(result.plan.live_in_mask == live_in);
		region_rip += 0x1000;
	}
}

} // namespace

int main()
{
	TestExhaustiveDirectOpcodeClassification();
	TestInstructionMasks();
	TestPlanLivenessDirtyAndRanking();
	TestDeterministicRankingTie();
	TestEdgesAndCycles();
	TestLimitsAndRejectReasons();
	TestDeterministicRandomCorpus();
	if (g_failures != 0)
	{
		std::fprintf(stderr, "CpuA64HotRegionTest: %d failure(s)\n", g_failures);
		return 1;
	}
	std::puts("CpuA64HotRegionTest: ok");
	return 0;
}
