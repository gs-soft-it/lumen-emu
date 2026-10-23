#ifndef EMULATOR_SRC_CPUA64HOTREGION_H_
#define EMULATOR_SRC_CPUA64HOTREGION_H_

#include "CpuA64Dynarec.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Lumen::Cpu::A64HotRegion {

constexpr size_t MIN_SEGMENTS = 2;
constexpr size_t MAX_SEGMENTS = 8;
constexpr size_t MAX_INSTRUCTIONS = 64;
constexpr size_t GPR_COUNT = 16;

// The selected successor is explicit.  In particular, BranchTaken and
// BranchFallthrough keep trace selection out of this pure analysis pass.
enum class EdgeKind : uint8_t
{
	Fallthrough,
	BranchTaken,
	BranchFallthrough,
	JumpTarget,
};

enum class RejectReason : uint8_t
{
	None,
	InvalidArgument,
	TooFewSegments,
	TooManySegments,
	TooManyInstructions,
	NotDirectBasicBlock,
	HelperOpcode,
	GuestMemory,
	MayFault,
	HostCall,
	Stack,
	UnclassifiedOpcode,
	InvalidEdgeKind,
	EdgeTargetMismatch,
	Cycle,
};

const char* RejectReasonName(RejectReason reason) noexcept;

struct SegmentInput
{
	const A64Dynarec::VerifiedIrBlock* block = nullptr;
	EdgeKind successor_kind = EdgeKind::Fallthrough;
	uint64_t successor_rip = 0;
};

// Width and conditionality are intentionally orthogonal.  For example, a
// CMOV r64 appears in both full_write_mask and conditional_write_mask, while
// SETcc appears in partial_write_mask only.  read_mask includes the old
// destination whenever a partial or conditional write must preserve it.
struct InstructionGprAccess
{
	uint16_t read_mask = 0;
	uint16_t full_write_mask = 0;
	uint16_t partial_write_mask = 0;
	uint16_t conditional_write_mask = 0;
	uint8_t segment_index = 0;
	uint8_t instruction_index = 0;
};

struct SegmentSummary
{
	uint16_t read_mask = 0;
	uint16_t full_write_mask = 0;
	uint16_t partial_write_mask = 0;
	uint16_t conditional_write_mask = 0;
	uint16_t read_before_full_definition_mask = 0;
	uint8_t instruction_begin = 0;
	uint8_t instruction_count = 0;
};

struct BoundaryPlan
{
	uint16_t dirty_mask = 0;
	uint16_t must_flush_mask = 0;
	EdgeKind edge_kind = EdgeKind::Fallthrough;
	uint64_t successor_rip = 0;
};

struct ResidentCandidate
{
	uint8_t guest_register = A64Dynarec::IR_NO_REGISTER;
	uint8_t first_segment = 0;
	uint8_t last_segment = 0;
	uint8_t boundary_crossings = 0;
	uint8_t access_count = 0;
	uint8_t read_count = 0;
	uint8_t write_count = 0;
	uint8_t segment_mask = 0;
	bool live_in = false;
	bool has_partial_write = false;
	bool has_conditional_write = false;
	uint16_t priority_score = 0;
};

struct Plan
{
	uint8_t segment_count = 0;
	uint8_t instruction_count = 0;
	uint8_t resident_candidate_count = 0;
	uint16_t live_in_mask = 0;
	std::array<uint64_t, MAX_SEGMENTS> segment_rips {};
	std::array<InstructionGprAccess, MAX_INSTRUCTIONS> instruction_accesses {};
	std::array<SegmentSummary, MAX_SEGMENTS> segments {};
	// There is one boundary after every segment, including the region exit.
	std::array<BoundaryPlan, MAX_SEGMENTS> boundaries {};
	std::array<ResidentCandidate, GPR_COUNT> resident_candidates {};
};

struct PlanResult
{
	RejectReason reject_reason = RejectReason::InvalidArgument;
	Plan plan {};

	bool Accepted() const noexcept { return reject_reason == RejectReason::None; }
};

// This classifier is deliberately independent of AArch64 physical-register
// allocation.  It covers exactly the helper-free direct-basic opcode surface
// and is also used by the focused exhaustive test.
bool ClassifyInstruction(const A64Dynarec::IrInstruction& instruction,
	                     InstructionGprAccess* output) noexcept;

// Pure, bounded analysis.  It allocates no executable memory and is not linked
// into the emulator runtime in this tranche.
PlanResult BuildPlan(const SegmentInput* segments, size_t segment_count) noexcept;

} // namespace Lumen::Cpu::A64HotRegion

#endif // EMULATOR_SRC_CPUA64HOTREGION_H_
