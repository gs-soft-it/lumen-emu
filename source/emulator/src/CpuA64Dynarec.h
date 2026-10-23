#ifndef EMULATOR_SRC_CPUA64DYNAREC_H_
#define EMULATOR_SRC_CPUA64DYNAREC_H_

#include "CpuX64FastOps.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace Lumen::Cpu {

struct X64State;

namespace A64Dynarec {

// This is the contract for the eventual shared IR, not the set currently
// lowered by the phase-0 emitter. Keeping the complete semantic surface here
// prevents a register-only prototype from becoming the runtime interface.
enum class SemanticClass : uint32_t
{
	Integer           = 1u << 0u,
	Flags             = 1u << 1u,
	GuestMemory       = 1u << 2u,
	StackControl      = 1u << 3u,
	SimdFloatingPoint = 1u << 4u,
	Atomics           = 1u << 5u,
	StringOperations  = 1u << 6u,
	HostCalls         = 1u << 7u,
	PreciseExit       = 1u << 8u,
	SelfModifyingCode = 1u << 9u,
};

constexpr uint32_t SemanticMask(SemanticClass value)
{
	return static_cast<uint32_t>(value);
}

constexpr uint32_t GENERAL_BACKEND_SEMANTIC_CONTRACT =
    SemanticMask(SemanticClass::Integer) | SemanticMask(SemanticClass::Flags) | SemanticMask(SemanticClass::GuestMemory) |
    SemanticMask(SemanticClass::StackControl) | SemanticMask(SemanticClass::SimdFloatingPoint) | SemanticMask(SemanticClass::Atomics) |
    SemanticMask(SemanticClass::StringOperations) | SemanticMask(SemanticClass::HostCalls) | SemanticMask(SemanticClass::PreciseExit) |
    SemanticMask(SemanticClass::SelfModifyingCode);

constexpr size_t MAX_IR_INSTRUCTIONS = 16;
constexpr uint8_t IR_NO_REGISTER     = 0xff;
// IrInstruction::condition carrier for a canonicalized AH/CH/DH/BH source on
// TestImmediate. It is valid only for width 1 and source RAX..RBX.
constexpr uint8_t IR_TEST_IMMEDIATE_HIGH_BYTE = 1u << 0u;
// ShiftLeftImmediate carries ROL semantics in IrInstruction::condition.
constexpr uint8_t IR_SHIFT_IMMEDIATE_ROTATE_LEFT = 1u << 7u;
constexpr size_t SCALAR_MEMORY_CACHE_ENTRIES = 2;
constexpr uint64_t SCALAR_MEMORY_PAGE_SIZE = 4096;

// FastOp is a compact decoder record with overloaded fields. The dynarec
// consumes this normalized IR instead, so every operand and side effect has a
// stable meaning before executable memory is allocated.
enum class IrOpcode : uint8_t
{
	Nop,
	MoveRegister,
	MoveImmediate,
	Load,
	Store,
	AddImmediate,
	SubImmediate,
	CompareImmediate,
	PushRegister,
	PopRegister,
	BranchCondition,
	JumpImmediate,
	CallImmediate,
	Return,
	VectorMoveRegister,
	VectorLoad,
	VectorStore,
	VectorXorRegister,
	VectorXorMemory,
	ZeroExtendRegister,
	SignExtendRegister,
	EffectiveAddress,
	ByteSwapRegister,
	ZeroExtendMemory,
	SignExtendMemory,
	AddRegister,
	SubRegister,
	CompareRegister,
	ShiftRightLogicalImmediate,
	AndImmediate,
	BitTestRegister,
	Increment,
	SetCondition,
	BitwiseNot,
	Negate,
	TestImmediate,
	AndRegister,
	OrRegister,
	XorRegister,
	TestRegister,
	Decrement,
	VectorMoveGpr32ToXmm,
	ShiftLeftImmediate,
	ShiftLeftCl,
	ShiftRightLogicalCl,
	ShiftRightArithmeticImmediate,
	VectorPermuteDwordsImmediate,
	OrImmediate,
	XorImmediate,
	BitTestImmediate,
	ConditionalMove,
	ClearDirectionFlag,
	SubtractWithBorrowRegister,
	BitExtractRegister,
	AndNotRegister,
	SignedMultiplyRegister,
	SignedMultiplyImmediate,
	VectorMoveScalarRegister,
	AddRegisterMemory,
	SubRegisterMemory,
	OrRegisterMemory,
	AndRegisterMemory,
	XorRegisterMemory,
	CompareRegisterMemory,
	CompareMemoryRegister,
	CompareMemoryImmediate,
	TestMemoryImmediate,
	SignedMultiplyRegisterMemory,
	SignedMultiplyMemoryImmediate,
	AddMemoryRegister,
	SubtractMemoryRegister,
	AndMemoryRegister,
	AndMemoryImmediate,
	IncrementMemory,
	SetConditionMemory,
	VectorLoadScalar,
	VectorStoreScalar,
	// Helper-backed instructions remain typed all the way through verified IR.
	// This keeps operand/fault contracts reviewable while native AArch64
	// implementations are added family by family.
	VectorConvertFloatToIntegerRegister,
	VectorConvertFloatToIntegerMemory,
	VectorConvertIntegerToFloatRegister,
	VectorConvertIntegerToFloatMemory,
	VectorBinaryRegister,
	VectorBinaryMemory,
	VectorHorizontalAddRegister,
	VectorUnpackLowDwordsMemory,
	VectorMoveGpr64ToXmm,
	VectorCompareRegister,
	VectorCompareMemory,
	CallIndirectRegister,
	CallIndirectMemory,
	JumpIndirectRegister,
	JumpIndirectMemory,
	RepeatMove,
	RepeatStore,
	AtomicCompareExchangeMemory,
	Count,
};

enum class IrVectorBinaryOperation : uint8_t
{
	Add      = 0,
	Multiply = 1,
	Subtract = 2,
	Divide   = 3,
};

// Vector state is kept as a full 256-bit guest register even when an
// instruction operates on only the low 128 bits. These flags make the
// otherwise implicit VEX upper-lane and aligned-memory semantics explicit in
// verified IR.
enum class IrVectorFlag : uint8_t
{
	ZeroUpper        = 1u << 0u,
	RequireAlignment = 1u << 1u,
};

constexpr uint8_t IrVectorFlagMask(IrVectorFlag value)
{
	return static_cast<uint8_t>(value);
}

enum class IrEffect : uint8_t
{
	ReadGuestMemory  = 1u << 0u,
	WriteGuestMemory = 1u << 1u,
	ReadFlags        = 1u << 2u,
	WriteFlags       = 1u << 3u,
	Stack            = 1u << 4u,
	Terminator       = 1u << 5u,
	MayFault         = 1u << 6u,
	HostCall         = 1u << 7u,
};

constexpr uint8_t IrEffectMask(IrEffect value)
{
	return static_cast<uint8_t>(value);
}

struct IrAddress
{
	uint8_t base  = IR_NO_REGISTER;
	uint8_t index = IR_NO_REGISTER;
	uint8_t scale = 0;
	int32_t displacement = 0;
	uint64_t absolute = 0;
};

struct IrInstruction
{
	IrOpcode opcode {};
	uint8_t width      = 0;
	uint8_t destination = IR_NO_REGISTER;
	uint8_t source      = IR_NO_REGISTER;
	uint8_t source_width = 0;
	uint8_t source2     = IR_NO_REGISTER;
	uint8_t condition   = 0;
	uint8_t vector_flags = 0;
	uint8_t length      = 0;
	uint8_t step_cost   = 1;
	uint8_t effects     = 0;
	IrAddress address {};
	uint64_t rip       = 0;
	uint64_t immediate = 0;
	uint64_t target    = 0;
};

struct IrBlock
{
	uint64_t rip             = 0;
	uint64_t fallthrough_rip = 0;
	uint32_t semantic_mask   = 0;
	uint8_t instruction_count = 0;
	bool has_terminator      = false;
	std::array<IrInstruction, MAX_IR_INSTRUCTIONS> instructions {};
};

constexpr size_t MULTI_SEGMENT_MAX_SEGMENTS = 4;
constexpr size_t MULTI_SEGMENT_MAX_SIGNATURE_BYTES = 64;
constexpr size_t MULTI_SEGMENT_PAGE_SIZE = 4096;

struct MultiSegmentArtifactSegment
{
	IrBlock block {};
	uint16_t page_offset = 0;
	uint8_t byte_count = 0;
	std::array<uint8_t, MULTI_SEGMENT_MAX_SIGNATURE_BYTES> expected {};
};

struct MultiSegmentArtifactSpec
{
	uint8_t segment_count = 0;
	std::array<MultiSegmentArtifactSegment, MULTI_SEGMENT_MAX_SEGMENTS> segments {};
};

// Phase-6BX planner result. The diagnostic profiler and the exact-one
// production candidate share this fail-closed choice and its exact
// baseline/candidate arithmetic.
struct BlockGprCachePlan
{
	bool has_gpr_access = false;
	bool block_has_partial_hazard = false;
	bool block_has_shift_cl_hazard = false;
	bool eligible = false;
	uint8_t guest_register = IR_NO_REGISTER;
	bool selected_partial_width = false;
	bool selected_shift_cl = false;
	bool selected_read_only = false;
	bool selected_write_only = false;
	bool selected_readwrite = false;
	bool selected_lazy_init = false;
	bool selected_alias = false;
	bool selected_conditional_write = false;
	uint16_t baseline_loads = 0;
	uint16_t baseline_stores = 0;
	uint16_t baseline_accesses = 0;
	uint16_t candidate_initial_loads = 0;
	uint16_t candidate_final_stores = 0;
	uint16_t candidate_accesses = 0;
	uint16_t candidate_prepare_words = 0;
	uint16_t candidate_commit_words = 0;
	uint16_t candidate_cache_words = 0;
	uint16_t eliminated_access_floor = 0;
	uint16_t eliminated_word_floor = 0;
};

enum class ExitReason : uint32_t
{
	Completed   = 0,
	Bailout     = 1,
	MemoryFault = 2,
	Invalidated = 3,
	StepLimit   = 4,
	SignatureMismatch = 5,
};

enum class ScalarMemoryPermission : uint32_t
{
	Read  = 1u << 0u,
	Write = 1u << 1u,
};

constexpr uint32_t ScalarMemoryPermissionMask(ScalarMemoryPermission value)
{
	return static_cast<uint32_t>(value);
}

// A translation is data owned by one entry invocation, never an address baked
// into published code. The half-open guest interval maps linearly onto
// host_begin and carries the permissions verified by the authoritative slow
// translator. Entries are discarded before returning to the outer dispatcher.
struct ScalarMemoryRegion
{
	uint64_t guest_begin = 0;
	uint64_t guest_end   = 0;
	uint64_t host_begin  = 0;
	uint32_t permissions = 0;
	uint32_t reserved    = 0;
};

enum class ScalarMemoryResolveResult : uint32_t
{
	Resolved    = 0,
	MemoryFault = 1,
	UseGeneric  = 2,
};

// Native entries receive only this stable dispatch context. Host translations
// are bounded, invocation-local data; guest pointers are never embedded in
// immutable code and every miss still passes through the authoritative range
// translator before the entry dereferences host memory. CompiledBlock::Execute
// consumes any caller-preloaded records and clears them on every return.
struct EntryContext
{
	X64State*  state           = nullptr;
	void*      runtime_context = nullptr;
	uint64_t   cache_epoch     = 0;
	uint64_t   resume_rip      = 0;
	uint32_t   remaining_steps = 0;
	uint32_t   executed_steps  = 0;
	ExitReason exit_reason     = ExitReason::Bailout;
	// Updated only by instructions whose x86 semantics are emitted directly
	// into the AArch64 entry. Helper-backed IR steps leave this counter alone,
	// which makes accidental fallback visible to differential tests.
	uint32_t   direct_steps    = 0;
	std::array<ScalarMemoryRegion, SCALAR_MEMORY_CACHE_ENTRIES> scalar_memory_regions {};
	uint64_t   scalar_memory_host_address = 0;
	// A read/modify/write resolver may legally expose distinct readable and
	// writable host aliases for the same guest interval.  Keep the read side
	// separate until the single native commit; ordinary loads/stores leave it
	// zero and continue to use scalar_memory_host_address.
	uint64_t   scalar_memory_read_host_address = 0;
	uint32_t   scalar_memory_next          = 0;
	uint32_t   scalar_memory_post_action   = 0;
	uint32_t   scalar_memory_enabled       = 0;
	// Set only by a direct native CALL that observes an already-owned stack
	// transaction. The dispatcher maps this expected fail-closed exit without
	// poisoning the compiled cache or replaying the CALL in another tier.
	uint32_t   call_stack_transaction_conflict = 0;
	// Borrowed for one multi-segment invocation. The immutable artifact embeds
	// only bounded offsets and expected bytes, never this host pointer.
	const uint8_t* instruction_page_host_begin = nullptr;
	uint32_t multi_segment_mismatch_stage = 0;
	uint32_t multi_segment_reserved = 0;
};

using EntryPoint = uint32_t (*)(EntryContext* context);

// Phase-6CE direct-basic dense entries do not need the general EntryContext:
// x0 carries the guest state and x1 the already-validated sequential end.
using DenseContextlessEntryPoint = uint32_t (*)(X64State* state,
	                                             uint64_t block_end);

constexpr uint32_t DENSE_CONTEXTLESS_COMPLETION_COOKIE_BASE = 0xce00u;

constexpr uint32_t DenseContextlessCompletionCookie(
	uint8_t instruction_count)
{
	return DENSE_CONTEXTLESS_COMPLETION_COOKIE_BASE |
	       static_cast<uint32_t>(instruction_count);
}

enum class StepAction : uint32_t
{
	Continue = 0,
	Exit     = 1,
};

// A service may mutate the guest object referenced by context->state and may
// publish context->exit_reason. The dispatcher owns every other EntryContext
// field for the duration of an entry; in particular the callback must not
// change state/runtime pointers, epoch, resume RIP, budgets or step counters.
// A scalar resolver returning Resolved must publish a non-zero exact host
// address for the requested width. A resolver returning UseGeneric must leave
// the architectural guest state unchanged because the generic callback
// re-evaluates the instruction from that state. Region records are optional
// acceleration data and are sanitized by the entry wrapper. Generic semantic
// callbacks form a translation barrier; all cached records are discarded after
// they return.
// Store notification runs after the native commit; any Exit is normalized to
// the instruction's post-op RIP before the entry retires and side-exits.
using ExecuteIrStep = StepAction (*)(void* opaque, EntryContext* context, const IrInstruction* instruction) noexcept;
using ResolveScalarMemory = ScalarMemoryResolveResult (*)(void* opaque, EntryContext* context,
	                                                        const IrInstruction* instruction,
	                                                        uint64_t guest_address) noexcept;
using NotifyScalarStore = StepAction (*)(void* opaque, EntryContext* context,
	                                      const IrInstruction* instruction,
	                                      uint64_t guest_address, uint64_t host_address,
	                                      uint64_t value) noexcept;
// Completes the control-transfer half of a natively pushed CALL. The entry
// arms X64State::host_call_stack_transaction and publishes the stack write
// before invoking this service. The service must consume that transaction,
// publish the final RIP/exit reason (including HLE rollback faults), and return
// Exit. Until it consumes the transaction it must not unmap or remap the
// transaction's host slot: malformed-return recovery may restore that backing
// directly. CALL is a terminator even when the preceding store notification
// asked for an SMC side exit.
using CompleteDirectCall = StepAction (*)(void* opaque, EntryContext* context,
	                                       const IrInstruction* instruction) noexcept;

struct RuntimeServices
{
	void*         opaque       = nullptr;
	ExecuteIrStep execute_step = nullptr;
	ResolveScalarMemory resolve_scalar_memory = nullptr;
	NotifyScalarStore notify_scalar_store = nullptr;
	// Set only when the notifier cannot Protect/Free/remap guest memory, directly
	// or through re-entrant work. Other providers get a conservative barrier.
	bool notify_scalar_store_preserves_translations = false;
	CompleteDirectCall complete_direct_call = nullptr;
};

enum class Status : uint8_t
{
	Ok,
	UnsupportedHost,
	InvalidArgument,
	AllocationFailed,
	NotAllocated,
	CapacityExceeded,
	AlreadyAllocated,
	AlreadyPublished,
	UnsupportedOperation,
	InvalidIr,
	PlatformFailure,
};

// Existing preserves the original mixed-block admission contract. The direct
// policies additionally admit verified, helper-free blocks. ProductionBasicBlock
// preserves that contract while excluding typed-helper IR until those callbacks
// have a measured profitability win. ProductionScalarMemoryFallthrough extends
// that production contract only to the framed scalar-data-memory fallthrough
// class. VerifiedBasicBlock remains the structural coverage policy used to
// validate and encode the explicitly enumerated helpers.
enum class EligibilityPolicy : uint8_t
{
	Existing = 0,
	ExistingOrDirectFallthrough = 1,
	ExistingOrDirectBasicBlock = 2,
	ExistingOrVerifiedBasicBlock = 3,
	ExistingOrProductionBasicBlock = 4,
	ExistingOrProductionScalarMemoryFallthrough = 5,
	// Android's isolated semantic NODA path and W^X publication probe lower only
	// helper-free direct blocks. They deliberately exclude the historical
	// memory+stack class included by every ExistingOr* policy.
	DirectBasicBlockOnly = 6,
};

struct VerifiedIrResult;

// Capability proving that a normalized IR block has passed the complete
// lowering/verifier contract. The owned block is immutable and the type cannot
// be default-constructed or assigned, so a proof cannot become stale after it
// has been handed to eligibility, encoding or compilation.
class VerifiedIrBlock final
{
public:
	struct LoweringStorageTag final {};
	// Empty storage is deliberately explicit and supports no policy. Only the
	// stack-bounded lowering overload can turn it into a verifier capability;
	// the type therefore remains non-default-constructible.
	explicit VerifiedIrBlock(LoweringStorageTag);
	VerifiedIrBlock(const VerifiedIrBlock&) = default;
	VerifiedIrBlock(VerifiedIrBlock&&)      = default;
	VerifiedIrBlock& operator=(const VerifiedIrBlock&) = delete;
	VerifiedIrBlock& operator=(VerifiedIrBlock&&) = delete;

	const IrBlock& Block() const noexcept;
	bool Supports(EligibilityPolicy policy) const noexcept;
	bool IsDirectFallthrough() const noexcept;
	bool IsDirectBasicBlock() const noexcept;
	bool IsProductionScalarMemoryFallthrough() const noexcept;

private:
	friend VerifiedIrResult LowerFastOpsVerified(
	    const X64Fast::FastOp* operations, size_t operation_count);
	friend Status LowerFastOpsVerified(
	    const X64Fast::FastOp* operations, size_t operation_count,
	    VerifiedIrBlock* output);

	VerifiedIrBlock();
	VerifiedIrBlock(IrBlock&& block, uint8_t eligibility_mask);

	IrBlock m_block;
	uint8_t m_eligibility_mask = 0;
};

struct VerifiedIrResult
{
	Status status = Status::InvalidArgument;
	std::optional<VerifiedIrBlock> block;
};

// Phase-neutral, bounded diagnostics for an already verified instruction.
// These flags describe the production lowering shape; they do not grant
// runtime eligibility or change the immutable verifier capability.
enum class IrInstructionDiagnosticFlag : uint32_t
{
	ProductionDirect          = 1u << 0u,
	ReadsRflags               = 1u << 1u,
	WritesRflags              = 1u << 2u,
	ReadsGuestMemory          = 1u << 3u,
	WritesGuestMemory         = 1u << 4u,
	GuestMemory               = 1u << 5u,
	TypedHelper               = 1u << 6u,
	TypedHelperRegisterOnly   = 1u << 7u,
	TypedHelperMemory         = 1u << 8u,
	TypedHelperStack          = 1u << 9u,
	TypedHelperAtomic         = 1u << 10u,
	TypedHelperRepeat         = 1u << 11u,
	TypedHelperIndirectControl = 1u << 12u,
	Terminator                = 1u << 13u,
	DirectTerminator          = 1u << 14u,
	MayFault                  = 1u << 15u,
	HostCall                  = 1u << 16u,
	Stack                     = 1u << 17u,
};

constexpr uint32_t IrInstructionDiagnosticMask(
	IrInstructionDiagnosticFlag value)
{
	return static_cast<uint32_t>(value);
}

enum class SingleRegisterHelperMixedBlockRejection : uint32_t
{
	TypedHelperCountMismatch      = 1u << 0u,
	TypedHelperNotRegisterOnly    = 1u << 1u,
	InsufficientProductionDirect  = 1u << 2u,
	NonProductionInstruction      = 1u << 3u,
	NonDirectTerminator           = 1u << 4u,
	DirectTerminatorNotTail       = 1u << 5u,
};

constexpr uint32_t SingleRegisterHelperMixedBlockRejectionMask(
	SingleRegisterHelperMixedBlockRejection value)
{
	return static_cast<uint32_t>(value);
}

enum class SingleRegisterHelperMixedBlockReason : uint8_t
{
	Eligible = 0,
	TypedHelperCountMismatch,
	TypedHelperNotRegisterOnly,
	NonProductionInstruction,
	InvalidTerminator,
	InsufficientProductionDirect,
};

constexpr uint8_t SINGLE_REGISTER_HELPER_MIN_PRODUCTION_DIRECT = 6;
constexpr uint8_t NO_IR_INSTRUCTION_INDEX = 0xff;

// Diagnostic-only decomposition of the proposed single-register-helper mixed
// shape. instruction_flags is populated only through instruction_count; the
// unused tail stays zero. Category counts may overlap by design (for example,
// an indirect helper can also touch guest memory and stack).
struct SingleRegisterHelperMixedBlockAnalysis
{
	bool eligible = false;
	SingleRegisterHelperMixedBlockReason reason =
	    SingleRegisterHelperMixedBlockReason::TypedHelperCountMismatch;
	uint32_t rejection_flags = 0;
	uint8_t instruction_count = 0;
	uint8_t production_direct_count = 0;
	uint8_t production_direct_linear_count = 0;
	uint8_t typed_helper_count = 0;
	uint8_t typed_helper_register_only_count = 0;
	uint8_t typed_helper_memory_count = 0;
	uint8_t typed_helper_stack_count = 0;
	uint8_t typed_helper_atomic_count = 0;
	uint8_t typed_helper_repeat_count = 0;
	uint8_t typed_helper_indirect_control_count = 0;
	uint8_t non_production_instruction_count = 0;
	uint8_t rflags_instruction_count = 0;
	uint8_t guest_memory_instruction_count = 0;
	uint8_t host_call_instruction_count = 0;
	uint8_t may_fault_instruction_count = 0;
	uint8_t terminator_count = 0;
	uint8_t direct_terminator_count = 0;
	uint8_t helper_instruction_index = NO_IR_INSTRUCTION_INDEX;
	IrOpcode helper_opcode = IrOpcode::Count;
	std::array<uint32_t, MAX_IR_INSTRUCTIONS> instruction_flags {};
};

const char* StatusName(Status status);
const char* SingleRegisterHelperMixedBlockReasonName(
    SingleRegisterHelperMixedBlockReason reason) noexcept;

// Does not re-run lowering or verification: the VerifiedIrBlock capability is
// the proof. The function is allocation-free, side-effect-free and bounded by
// MAX_IR_INSTRUCTIONS, making it suitable for sampled diagnostics.
SingleRegisterHelperMixedBlockAnalysis AnalyzeSingleRegisterHelperMixedBlock(
    const VerifiedIrBlock& block) noexcept;

// Fail-closed execution-snapshot classification for the ordinary FastBlock
// executor. A known operation has a provider-level proof of its guest-memory
// and host-call effects. The full provider derives that proof from production
// lowering and validation; a disabled provider may use a conservative static
// proof. Unknown remains true whenever those effects cannot be proven.
struct FastOpExecutionSnapshotAnalysis
{
	bool known = false;
	bool guest_memory = false;
	bool host_call = false;
	bool unknown = true;
	bool snapshot_avoidance_candidate = false;
};

// Aggregate capability for one complete FastBlock operation prefix. The
// counts overlap where an operation has both guest-memory and HostCall
// effects. A candidate contains at least one known guest-memory operation and
// no HostCall or unknown operation; it does not itself authorize skipping the
// snapshot without a separate non-reentrant translator capability.
struct FastBlockOperationSnapshotAnalysis
{
	bool snapshot_avoidance_candidate = false;
	size_t operation_count = 0;
	size_t known_operation_count = 0;
	size_t guest_memory_operation_count = 0;
	size_t host_call_operation_count = 0;
	size_t unknown_operation_count = 0;
};

FastOpExecutionSnapshotAnalysis AnalyzeFastOpExecutionSnapshot(
    const X64Fast::FastOp& operation) noexcept;
FastBlockOperationSnapshotAnalysis AnalyzeFastBlockOperationSnapshot(
    const X64Fast::FastOp* operations, size_t operation_count) noexcept;

// Returns true when ordinary FastBlock execution may cross a guest-memory or
// host callback boundary. A true result requires a private operation snapshot
// because a nested Run can recycle the owning TLS cache entry.
bool FastOpRequiresExecutionSnapshot(const X64Fast::FastOp& operation);

Status LowerFastOps(const X64Fast::FastOp* operations, size_t operation_count, IrBlock* output);
VerifiedIrResult LowerFastOpsVerified(
    const X64Fast::FastOp* operations, size_t operation_count);
// Stack-bounded production overload. The caller supplies the sole IR owner;
// on success it becomes the same immutable-by-interface verifier capability.
Status LowerFastOpsVerified(
    const X64Fast::FastOp* operations, size_t operation_count,
    VerifiedIrBlock* output);
Status VerifyIrBlock(const IrBlock& block);
// Returns Ok for every fully verified direct basic block, including one with
// no profitable v1 choice (eligible=false). Other statuses are fail-closed and
// leave a zeroed plan with guest_register=IR_NO_REGISTER.
Status AnalyzeBlockGprCachePlan(const IrBlock& block, BlockGprCachePlan* output);
bool   IsRuntimeEligible(const IrBlock& block);
bool   IsRuntimeEligible(const IrBlock& block, EligibilityPolicy policy);
bool   IsRuntimeEligible(const VerifiedIrBlock& block, EligibilityPolicy policy);
uint32_t ExecuteIrReference(const IrBlock& block, EntryContext* context);
uint32_t ExecuteIrReference(const IrBlock& block, EntryContext* context, EligibilityPolicy policy);
// Host-independent AArch64 encoding used by CompiledBlock and golden tests.
// On failure encoded_size/direct_instruction_count are reset to zero and the
// caller must discard the staging bytes.
Status EncodeIrBlock(const IrBlock& block, void* storage, size_t capacity_bytes,
                     size_t* encoded_size, uint8_t* direct_instruction_count);
Status EncodeIrBlock(const IrBlock& block, void* storage, size_t capacity_bytes,
                     size_t* encoded_size, uint8_t* direct_instruction_count,
                     EligibilityPolicy policy);
#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
// Focused phase-6BL seam: select the emitted retirement policy without
// mutating the process-latched production selector. Both variants still pass
// through the same verifier and encoder.
Status EncodeIrBlockWithBatchDirectRetirementForTest(
    const IrBlock& block, void* storage, size_t capacity_bytes,
    size_t* encoded_size, uint8_t* direct_instruction_count,
    EligibilityPolicy policy, bool batch_direct_retirement);
// Focused phase-6BM seam: select both cumulative batch retirement and the
// direct-only entry ABI without changing either process latch.
Status EncodeIrBlockWithDirectLeafAbiForTest(
    const IrBlock& block, void* storage, size_t capacity_bytes,
    size_t* encoded_size, uint8_t* direct_instruction_count,
    EligibilityPolicy policy, bool batch_direct_retirement,
    bool direct_leaf_abi);
// Focused phase-6BU seams: compare the historical and compact wide-immediate
// policies in one process without mutating the production latch.
Status EncodeImmediate64ForTest(uint8_t destination, uint64_t value, bool compact_immediates, void* storage, size_t capacity_bytes,
                                size_t* encoded_size);
Status EncodeIrBlockWithCompactImmediatesForTest(const IrBlock& block, void* storage, size_t capacity_bytes, size_t* encoded_size,
                                                 uint8_t* direct_instruction_count, EligibilityPolicy policy, bool batch_direct_retirement,
                                                 bool direct_leaf_abi, bool compact_immediates);
bool BatchDirectRetirementEnvironmentValueEnabledForTest(const char* value);
bool DirectLeafAbiEnvironmentValueEnabledForTest(const char* value);
bool DirectContextReuseEnvironmentValueEnabledForTest(const char* value);
bool   CompactImmediateEnvironmentValueEnabledForTest(const char* value);
bool BlockRflagsCacheEnvironmentValueEnabledForTest(const char* value);
bool BlockGprCacheEnvironmentValueEnabledForTest(const char* value);
bool MultiSegmentArtifactEnvironmentValueEnabledForTest(const char* value);
bool DenseResidentCacheEnvironmentValueEnabledForTest(const char* value);
// Focused phase-6BV seam: compare the complete 128-bit rollback and the
// coherent 256-bit packed VMOV/VXOR family without mutating the process latch.
Status LowerFastOpsWithPacked256ForTest(
    const X64Fast::FastOp* operations, size_t operation_count,
    IrBlock* output, bool packed256_enabled);
Status EncodeIrBlockWithPacked256ForTest(
    const IrBlock& block, void* storage, size_t capacity_bytes,
    size_t* encoded_size, uint8_t* direct_instruction_count,
    EligibilityPolicy policy, bool packed256_enabled);
bool Packed256EnvironmentValueEnabledForTest(const char* value);
#endif
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_VERIFIED_IR_BENCHMARK) || \
    defined(LUMEN_JIT_PUBLICATION_BENCHMARK) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
// Staging-only proof seam for the focused equivalence test and mechanical
// benchmark. Production consumes a VerifiedIrBlock only through
// CompiledBlock::CompileVerified and cannot request unchecked staging bytes.
Status EncodeVerifiedIrBlockForTest(
    const VerifiedIrBlock& block, void* storage, size_t capacity_bytes,
    size_t* encoded_size, uint8_t* direct_instruction_count,
    EligibilityPolicy policy);
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || defined(LUMEN_CPU_X64_TEST)
struct IrVerificationTestCounters
{
	uint64_t instruction_validations = 0;
	uint64_t full_block_verifications = 0;
	uint64_t structural_block_verifications = 0;
};

void ResetIrVerificationTestCounters();
IrVerificationTestCounters GetIrVerificationTestCounters();
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
struct IrInstructionValidationPairForTest
{
	bool eager = false;
	bool branch_local = false;
};

IrInstructionValidationPairForTest CompareIrInstructionValidatorsForTest(
    const IrInstruction& instruction);
bool BranchLocalIrValidationEnvironmentValueEnabledForTest(const char* value);
bool IsBranchLocalIrValidationEnabledForTest();

// Phase-6BY is deliberately a test-only proof, not a production bundle ABI.
// Expected bytes are artifact-owned values; the host page mapping is borrowed
// by one invocation and is never retained by the encoded entry.
constexpr size_t PHASE6BY_BOUNDARY_MAX_SEGMENTS = 4;
constexpr size_t PHASE6BY_BOUNDARY_MAX_SIGNATURE_BYTES = 64;
constexpr size_t PHASE6BY_BOUNDARY_PAGE_SIZE = 4096;

struct Phase6ByBoundarySegmentForTest
{
	uint16_t page_offset = 0;
	uint8_t byte_count = 0;
	std::array<uint8_t, PHASE6BY_BOUNDARY_MAX_SIGNATURE_BYTES> expected {};
};

struct alignas(8) Phase6ByBoundaryInvocationForTest
{
	const uint8_t* page_host_begin = nullptr;
	// The entry publishes one synthetic effect before waiting at that segment's
	// old boundary. Tests release checkpoints monotonically after any mutation.
	std::atomic<uint32_t> completed_segments {0};
	std::atomic<uint32_t> checkpoint_release {UINT32_MAX};
	std::atomic<uint32_t> mismatch_stage {0};
};

using Phase6ByBoundaryEntryForTest =
    uint32_t (*)(Phase6ByBoundaryInvocationForTest* invocation);

constexpr uint32_t Phase6ByInitialSourceStageForTest()
{
	return 1;
}

constexpr uint32_t Phase6BySourcePostStageForTest(size_t segment_index)
{
	return static_cast<uint32_t>(2u + segment_index * 2u);
}

constexpr uint32_t Phase6BySuccessorPreStageForTest(size_t source_index)
{
	return static_cast<uint32_t>(3u + source_index * 2u);
}

constexpr uint32_t PHASE6BY_BOUNDARY_INVALID_MAPPING_STAGE = UINT32_MAX;

Status EncodePhase6ByBoundaryProofForTest(
    const Phase6ByBoundarySegmentForTest* segments, size_t segment_count,
    void* storage, size_t capacity_bytes, size_t* encoded_size);
#endif

#if defined(LUMEN_IR_VALIDATOR_BENCHMARK)
bool ValidateIrInstructionForBenchmark(
    const IrInstruction& instruction, bool branch_local);
#endif

// A bounds-checked staging writer for the phase-0 ABI smoke. It is purposely
// not a general assembler; the guest backend will adopt a separately reviewed
// macro-assembler before growing instruction-by-instruction lowering here.
class A64CodeBuffer final
{
public:
	A64CodeBuffer(void* storage, size_t capacity_bytes);

	Status EmitBtiCall();
	Status EmitMoveZero64(uint8_t destination, uint16_t immediate, uint8_t shift);
	Status EmitMoveKeep64(uint8_t destination, uint16_t immediate, uint8_t shift);
	Status EmitMoveImmediate64(uint8_t destination, uint64_t value);
	Status EmitReturn(uint8_t register_index = 30);
	Status EmitReturnConstant(uint64_t value);
	Status EmitEntryContextSmoke();

	const void* Data() const;
	uint32_t    WordAt(size_t index) const;
	size_t      WordCount() const;
	size_t      SizeBytes() const;
	size_t      CapacityBytes() const;

private:
	Status Reserve(size_t word_count) const;
	void   EmitUnchecked(uint32_t word);

	uint8_t* m_storage        = nullptr;
	size_t   m_capacity_bytes = 0;
	size_t   m_size_bytes     = 0;
};

// Executable mappings are never shared with guest virtual memory. Publication
// is single-shot, single-writer and immutable for one artifact generation.
// Published entries may be called by concurrent readers, but Reset/destruction
// requires those readers to be quiescent and releases the mapping. A separate
// macOS-only path may detach one page from an archive-retired artifact after
// the same quiescence proof; the next owner always gets a new epoch and
// republishes through the ordinary protected callback.
class ExecutableCodeMemory final
{
public:
	ExecutableCodeMemory() = default;
	~ExecutableCodeMemory();

	ExecutableCodeMemory(const ExecutableCodeMemory&)            = delete;
	ExecutableCodeMemory& operator=(const ExecutableCodeMemory&) = delete;
	ExecutableCodeMemory(ExecutableCodeMemory&&)                 = delete;
	ExecutableCodeMemory& operator=(ExecutableCodeMemory&&)      = delete;

	Status Allocate(size_t minimum_capacity);
	// Phase-6CC direct-basic artifacts use a bounded MAP_JIT arena. The
	// ordinary whole-mapping allocator remains unchanged for every existing
	// CompiledBlock and for all non-macOS hosts.
	Status AllocateDenseResident(size_t minimum_capacity);
	Status Publish(const void* staging_code, size_t size);
	void   Reset();
#if defined(__ANDROID__) && \
    defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)
	// The Android startup probe is the only caller that needs the platform
	// munmap result. Production cache retirement retains its existing API.
	bool   ResetForAndroidProbe();
#endif
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
	void   RetireForReuseAfterQuiescenceForTest();
#endif

	const void* EntryAddress() const;
	size_t      Capacity() const;
	size_t      PublishedSize() const;
	uint64_t    Epoch() const;
	bool        IsPublished() const;

private:
	friend class CompiledBlock;
	void RetireForReuseAfterQuiescence();
	bool ResetUnlocked();

	void*               m_address  = nullptr;
	size_t              m_capacity = 0;
	std::atomic<size_t> m_published_size {0};
	bool                 m_publish_started = false;
	bool                 m_recycled_lease = false;
	bool                 m_dense_resident_slice = false;
	uint32_t             m_dense_resident_page = UINT32_MAX;
	uint8_t              m_dense_resident_slot = UINT8_MAX;
	uint64_t            m_epoch = 0;
	std::mutex           m_publish_mutex;
#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
	uint64_t            m_host_pc_profile_token = 0;
#endif
};

// A direct-basic entry contains no helper call and therefore embeds no
// IrInstruction pointer. Phase-6CC can discard the large IrBlock after
// emission and retain only this compact immutable execution artifact.
using DenseResidentCapacityReclaimer = bool (*)(void*, size_t);

#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
// Diagnostic-only breakdown for one dense artifact. Keep the type and the
// extended CompileVerified ABI absent from ordinary production builds.
struct DenseResidentCompileTiming
{
	uint64_t staging_ns = 0;
	uint64_t emit_ns = 0;
	uint64_t allocate_ns = 0;
	uint64_t publish_ns = 0;
	size_t code_size = 0;
	size_t code_capacity = 0;
};
#endif

class DenseResidentBlock final
{
public:
	DenseResidentBlock() = default;
	~DenseResidentBlock() = default;

	DenseResidentBlock(const DenseResidentBlock&) = delete;
	DenseResidentBlock& operator=(const DenseResidentBlock&) = delete;
	DenseResidentBlock(DenseResidentBlock&&) = delete;
	DenseResidentBlock& operator=(DenseResidentBlock&&) = delete;

	Status CompileVerified(const VerifiedIrBlock& block,
	                       EligibilityPolicy policy);
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	Status CompileVerified(const VerifiedIrBlock& block,
	                       EligibilityPolicy policy,
	                       bool contextless_direct,
	                       DenseResidentCapacityReclaimer reclaimer = nullptr,
	                       void* reclaimer_context = nullptr,
	                       DenseResidentCompileTiming* timing = nullptr);
#else
	Status CompileVerified(const VerifiedIrBlock& block,
	                       EligibilityPolicy policy,
	                       bool contextless_direct,
	                       DenseResidentCapacityReclaimer reclaimer = nullptr,
	                       void* reclaimer_context = nullptr);
#endif
	uint32_t ExecutePreparedDirect(EntryContext* context) const;
	// The dispatcher constructs the same direct-entry contract and retains all
	// post-entry validation; this variant removes only duplicated prechecks.
	uint32_t ExecutePreparedDirectKnownValid(EntryContext* context) const;
	// Valid only for the phase-6CE direct-basic ABI. The immutable artifact
	// verifies its RIP/end identity before entering code that receives no
	// EntryContext and returns a tagged completion cookie instead of accounting.
	uint32_t ExecuteContextlessDirect(X64State* state,
	                                  uint64_t block_end) const;

	uint64_t Epoch() const;
	const void* CodeAddress() const;
	size_t CodeSize() const;
	size_t CodeCapacity() const;
	bool IsDirectFallthrough() const;
	bool IsContextlessDirect() const;
	bool IsCompiled() const;
	bool MatchesMetadata(uint64_t rip, uint64_t fallthrough_rip,
	                     uint8_t instruction_count,
	                     bool direct_fallthrough,
	                     bool contextless_direct) const;

private:
	ExecutableCodeMemory m_code;
	uint64_t m_rip = 0;
	uint64_t m_fallthrough_rip = 0;
	uint8_t m_instruction_count = 0;
	bool m_direct_fallthrough = false;
	bool m_contextless_direct = false;
};

#if defined(__ANDROID__) && defined(__aarch64__) && \
    defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
struct AndroidNodaExecutionResult;
#endif

// A compiled block owns both normalized IR and its immutable native entry, so
// embedded instruction pointers cannot outlive or alias an evicted FastBlock.
// Destruction/replacement is permitted only while the calling thread is not
// executing this block.
class CompiledBlock final
{
public:
	CompiledBlock() = default;
	~CompiledBlock() = default;

	CompiledBlock(const CompiledBlock&)            = delete;
	CompiledBlock& operator=(const CompiledBlock&) = delete;
	CompiledBlock(CompiledBlock&&)                 = delete;
	CompiledBlock& operator=(CompiledBlock&&)      = delete;

	Status   Compile(const IrBlock& block);
	Status   Compile(const IrBlock& block, EligibilityPolicy policy);
	Status   CompileVerified(
	    const VerifiedIrBlock& block, EligibilityPolicy policy);
	Status   CompileMultiSegment(const MultiSegmentArtifactSpec& spec);
#if defined(LUMEN_CPU_X64_TEST)
	Status   CompileWithMinimumCapacityForTest(
	    const IrBlock& block, EligibilityPolicy policy,
	    size_t minimum_capacity);
#endif
	uint32_t Execute(EntryContext* context) const;
	// The caller has already initialized the per-segment direct-entry fields.
	// This path is valid only for proven direct basic blocks and deliberately
	// leaves every scalar-memory field untouched.
	uint32_t ExecutePreparedDirect(EntryContext* context) const;
#if defined(__ANDROID__) && defined(__aarch64__) && \
    defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
	AndroidNodaExecutionResult ExecuteAndroidNodaIsolated(
	    EntryContext* context) const;
#endif

	const IrBlock& Ir() const;
	uint64_t Epoch() const;
	const void* CodeAddress() const;
	size_t   CodeSize() const;
	size_t   CodeCapacity() const;
	uint8_t  DirectInstructionCount() const;
	bool     IsDirectFallthrough() const;
	bool     IsDirectBasicBlock() const;
	bool     IsProductionScalarMemoryFallthrough() const;
	uint8_t  SegmentCount() const { return m_segment_count; }
	uint8_t  SegmentInstructionCount(size_t index) const
	{
		return index < m_segment_count ? m_segment_instruction_counts[index] : 0;
	}
	uint8_t  SegmentByteCount(size_t index) const
	{
		return index < m_segment_count ? m_segment_byte_counts[index] : 0;
	}
	bool     IsCompiled() const;
#if defined(__ANDROID__) && \
    defined(LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX)
	bool     ResetCodeForAndroidProbe();
#endif
	// The caller must exclusively own a quiescent artifact after active_depth
	// reached zero. This empties the old owner before the page can be acquired
	// by another generation.
	void     RetireCodeForReuseAfterQuiescence();

private:
	Status CompileWithMinimumCapacity(
	    const IrBlock& block, EligibilityPolicy policy,
	    size_t minimum_capacity);

	IrBlock              m_ir {};
	ExecutableCodeMemory m_code;
	uint8_t              m_direct_instruction_count = 0;
	bool                 m_direct_fallthrough = false;
	bool                 m_direct_basic_block = false;
	bool                 m_production_scalar_memory_fallthrough = false;
	uint8_t              m_segment_count = 1;
	std::array<uint8_t, MULTI_SEGMENT_MAX_SEGMENTS> m_segment_instruction_counts {};
	std::array<uint8_t, MULTI_SEGMENT_MAX_SEGMENTS> m_segment_byte_counts {};
};

#if defined(__ANDROID__) && defined(__aarch64__) && \
    defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA)
enum class AndroidNodaExecutionStatus : uint8_t
{
	Completed,
	Faulted,
	TimedOut,
	Unavailable,
};

struct AndroidNodaExecutionResult
{
	AndroidNodaExecutionStatus status =
	    AndroidNodaExecutionStatus::Unavailable;
	uint32_t returned_steps = 0;
	int32_t signal = 0;
};

// Installs fail-closed process signal ownership, then proves success, injected
// fatal signals, timeout, shadow no-commit and concurrent TLS isolation. Both
// calls are required before the Android runtime may expose jit=1.
bool InitializeAndroidNodaIsolation();
bool ValidateAndroidNodaIsolation();
bool IsAndroidNodaIsolationReady();
// Re-reads every process signal disposition immediately before native use.
// A later owner is never overwritten: a mismatch permanently closes the tier.
bool VerifyAndroidNodaIsolationOwnership();
void InvalidateAndroidNodaIsolation();
void CopyAndroidNodaArchitectedState(X64State* destination,
                                     const X64State& source);
bool EqualAndroidNodaArchitectedState(const X64State& left,
                                      const X64State& right);
// The only helper allowed to copy a contained shadow back to live state.
// False validation is an explicit no-commit operation.
bool CommitAndroidNodaArchitectedState(X64State* live,
                                       const X64State& shadow,
                                       bool validation_passed);
#endif

// iOS is intentionally false even on an ARM64 Simulator. This reports host/OS
// support only; Allocate remains authoritative because a hardened macOS
// process may still lack the required allow-jit entitlement.
bool IsHostSupported();

// Desktop macOS arm64 only. Recycling is the promoted default; the exact
// runtime selector value "0" restores mmap/munmap ownership. iOS and other
// hosts remain disabled.
bool IsJitPageRecycleEnabled();

// Desktop macOS arm64 only. Direct basic blocks aggregate retirement by
// default; exact "0" is the process-latched rollback. Other hosts remain on
// the precise per-instruction path.
bool IsBatchDirectRetirementEnabled();

// Desktop macOS arm64 only. Proven direct basic blocks use the caller-saved
// leaf ABI by default; exact "0" is the process-latched framed rollback.
bool IsDirectLeafAbiEnabled();

// Desktop macOS arm64 production only. A direct chain may reuse one prepared
// EntryContext; exact "0" and malformed values retain the historical wrapper.
bool IsDirectContextReuseEnabled();

// Desktop macOS arm64 only. Phase-6BU remains opt-in after its frozen
// same-binary live campaign; exact "1" selects compact wide-immediate codegen.
bool IsCompactImmediateEncodingEnabled();

// Desktop macOS arm64 only. Phase-6BV is default-off; exact "1" enables the
// process-latched 256-bit packed VMOV/VXOR lowering family.
bool IsPacked256Enabled();

// Phase-6CB production candidates are independently reversible and remain
// default-off. Only exact "1" enables each process-latched mechanism.
bool IsBlockRflagsCacheEnabled();
bool IsBlockGprCacheEnabled();
bool IsMultiSegmentArtifactEnabled();

// Desktop macOS arm64 production only. Phase-6CC is enabled by default;
// exact "0" or a malformed value selects the complete legacy rollback.
bool IsDenseResidentCacheEnabled();
uint64_t DenseResidentReleaseGeneration();

constexpr size_t DENSE_RESIDENT_LOGICAL_ENTRIES = 4096u * 2u;
constexpr size_t DENSE_RESIDENT_EXECUTABLE_BUDGET = 16u * 1024u * 1024u;

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST)
struct DenseResidentArenaTestSnapshot
{
	size_t executable_budget = 0;
	size_t page_size = 0;
	size_t mapped_bytes = 0;
	size_t page_count = 0;
	uint64_t acquire_hits = 0;
	uint64_t acquire_misses = 0;
	uint64_t releases = 0;
	uint64_t live_slices = 0;
};

DenseResidentArenaTestSnapshot GetDenseResidentArenaTestSnapshot();
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
struct JitPageRecycleTestSnapshot
{
	size_t   slot_capacity = 0;
	size_t   page_size = 0;
	size_t   resident = 0;
	uint64_t acquire_hits = 0;
	uint64_t acquire_misses = 0;
	uint64_t retire_attempts = 0;
	uint64_t retire_stored = 0;
	uint64_t retire_overflows = 0;
	uint64_t retire_rejected = 0;
};

bool JitPageRecycleEnvironmentValueEnabledForTest(const char* value);
JitPageRecycleTestSnapshot GetJitPageRecycleTestSnapshot();
void ResetJitPageRecyclePoolForTest();
#endif

#if defined(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
// The host-PC sidecar is diagnostic-only and exists solely in explicitly
// instrumented builds. Records are process-lifetime and become immutable when
// FinishHostPcProfileSnapshot succeeds, so a caller can serialize them after
// the first-submit mutex has been released without copying the fixed registry.
constexpr uint64_t HOST_PC_PROFILE_RECORD_CAPACITY = 1000000;

struct HostPcProfileRecord
{
	uint64_t base           = 0;
	uint64_t published_size = 0;
	uint64_t capacity       = 0;
	uint64_t epoch          = 0;
	uint64_t publish_ns     = 0;
	// Zero means that the mapping was still live when the profile closed.
	uint64_t retire_ns      = 0;
};

struct HostPcProfileSnapshot
{
	uint32_t schema_version = 0;
	uint32_t complete       = 0;
	uint64_t generation     = 0;
	uint64_t pid            = 0;
	uint64_t begin_ns       = 0;
	uint64_t cutoff_ns      = 0;
	uint64_t capacity       = 0;
	uint64_t record_count   = 0;
	uint64_t retired_count  = 0;
	uint64_t overflow_count = 0;
	uint64_t invalid_count  = 0;
	uint64_t checksum       = 0;
};

bool HostPcProfileEnvironmentRequested();
// Prepare starts recording mapping lifetimes before entry.begin. It is
// idempotent while the one process-lifetime generation remains open.
bool PrepareHostPcProfile();
bool BeginHostPcProfile(uint64_t begin_ns);
bool ClaimCloseHostPcProfile(uint64_t cutoff_ns);
bool FinishHostPcProfileSnapshot(HostPcProfileSnapshot* snapshot);
bool ReadHostPcProfileRecord(const HostPcProfileSnapshot& snapshot,
	                          uint64_t index, HostPcProfileRecord* record);

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
// Deterministic registry seams. They are absent from production and let the
// focused A64 test cover address reuse, overflow, and close/writer races
// without allocating hundreds of thousands of executable mappings.
bool ResetHostPcProfileForTest(uint64_t effective_capacity,
	                           bool environment_requested);
void SetHostPcProfileClockForTest(uint64_t now_ns);
void ConfigureHostPcProfilePreAdmissionPauseForTest(bool pause);
bool HostPcProfilePreAdmissionPausedForTest();
void ReleaseHostPcProfilePreAdmissionForTest();
void ConfigureHostPcProfileWriterPauseForTest(bool pause);
bool HostPcProfileWriterPausedForTest();
bool HostPcProfileFinishWaitingForTest();
void ReleaseHostPcProfileWriterForTest();
uint64_t RegisterHostPcProfileMappingForTest(
	uint64_t base, uint64_t published_size, uint64_t capacity, uint64_t epoch);
bool RetireHostPcProfileMappingForTest(uint64_t token);
bool HostPcProfileEnvironmentValueEnabledForTest(const char* value);
#endif
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
struct JitCopyTestSnapshot
{
	uint32_t  arrivals = 0;
	uint32_t  max_concurrent = 0;
	uintptr_t last_token = 0;
};

struct JitCopyTestLease;

// Test-only observability for the macOS MAP_JIT callback. Production targets
// do not compile these hooks or their atomics into the publication path.
void ConfigureJitCopyConcurrencyTest(uint32_t expected_callbacks);
void ReleaseJitCopyConcurrencyTest();
JitCopyTestSnapshot GetJitCopyTestSnapshot();
int InvokeJitCopyCallbackForTest(uintptr_t token);
JitCopyTestLease* CreateJitCopyLeaseForTest(void* destination, size_t capacity,
                                            const void* source, size_t size);
uintptr_t JitCopyLeaseTokenForTest(const JitCopyTestLease* lease);
void DestroyJitCopyLeaseForTest(JitCopyTestLease* lease);
#endif

} // namespace A64Dynarec
} // namespace Lumen::Cpu

#endif
