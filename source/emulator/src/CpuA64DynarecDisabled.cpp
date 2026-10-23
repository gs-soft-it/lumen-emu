#include "CpuA64Dynarec.h"

// No-JIT provider shared by the Android bootstrap and full runtime targets.

#include <cstring>
#include <limits>
#include <utility>

namespace Lumen::Cpu::A64Dynarec {

namespace {

enum class SnapshotEffectClass : uint8_t
{
    Independent,
    DirectScalarGuestMemory,
    Unknown,
};

constexpr bool IsGuestRegister(uint8_t value) noexcept
{
    return value < 16;
}

bool HasValidGuestMemoryAddress(const X64Fast::FastOp& operation,
                                bool allow_absolute) noexcept
{
    if (operation.scale > 3) return false;
    if (operation.base == IR_NO_REGISTER)
    {
        return allow_absolute && operation.index == IR_NO_REGISTER &&
               operation.scale == 0 && operation.displacement == 0;
    }
    return IsGuestRegister(operation.base) &&
           (operation.index == IR_NO_REGISTER ||
            IsGuestRegister(operation.index)) &&
           (operation.index != IR_NO_REGISTER || operation.scale == 0) &&
           operation.target == 0;
}

bool HasValidSnapshotMetadata(const X64Fast::FastOp& operation) noexcept
{
    using X64Fast::FastOpKind;

    const auto has_valid_test_reg8_immediate_carrier = [&operation]()
    {
        return operation.condition == 0 && IsGuestRegister(operation.dst) &&
               (operation.base == 0 ||
                (operation.base ==
                     X64Fast::FAST_OP_TEST_REG8_IMM_HIGH_BYTE &&
                 operation.dst < 4));
    };

    const auto has_valid_shift_carrier = [&operation]()
    {
        const bool immediate_shift_left =
            operation.kind == FastOpKind::ShlReg32Imm ||
            operation.kind == FastOpKind::ShlReg64Imm;
        const bool shift_kind = immediate_shift_left ||
            operation.kind == FastOpKind::ShrReg8Imm ||
            operation.kind == FastOpKind::ShlReg32Cl ||
            operation.kind == FastOpKind::ShlReg64Cl ||
            operation.kind == FastOpKind::ShrReg32Cl ||
            operation.kind == FastOpKind::ShrReg64Cl ||
            operation.kind == FastOpKind::ShrReg32Imm ||
            operation.kind == FastOpKind::ShrReg64Imm ||
            operation.kind == FastOpKind::SarReg32Imm ||
            operation.kind == FastOpKind::SarReg64Imm;
        if (!shift_kind) return true;
        return IsGuestRegister(operation.dst) &&
               (operation.condition == 0 ||
                (immediate_shift_left &&
                 operation.condition ==
                     X64Fast::FAST_OP_SHIFT_IMMEDIATE_ROTATE_LEFT));
    };

    static_assert(static_cast<uint8_t>(FastOpKind::TestNoFlags) == 254,
                  "FastOpKind snapshot classification must be reviewed");

    if (static_cast<uint8_t>(operation.kind) >
            static_cast<uint8_t>(FastOpKind::TestNoFlags) ||
        operation.length == 0 || operation.length > 15 ||
        operation.rip > (std::numeric_limits<uint64_t>::max)() -
                            operation.length)
    {
        return false;
    }

    if (!has_valid_shift_carrier())
    {
        return false;
    } else if (operation.kind == FastOpKind::TestReg8Imm)
    {
        if (!has_valid_test_reg8_immediate_carrier()) return false;
    } else if (operation.kind == FastOpKind::CompareNoFlags)
    {
        const auto original = static_cast<FastOpKind>(operation.condition);
        if (original != FastOpKind::CmpReg32 &&
            original != FastOpKind::CmpReg64 &&
            original != FastOpKind::CmpReg16Imm &&
            original != FastOpKind::CmpReg32Imm &&
            original != FastOpKind::CmpReg64Imm)
        {
            return false;
        }
    } else if (operation.kind == FastOpKind::TestNoFlags)
    {
        const auto original = static_cast<FastOpKind>(operation.condition);
        if (original != FastOpKind::TestReg16 &&
            original != FastOpKind::TestReg8 &&
            original != FastOpKind::TestReg32 &&
            original != FastOpKind::TestReg64 &&
            original != FastOpKind::TestReg8Imm &&
            original != FastOpKind::TestReg16Imm &&
            original != FastOpKind::TestReg32Imm &&
            original != FastOpKind::TestReg64Imm)
        {
            return false;
        }
        if (original == FastOpKind::TestReg8Imm)
        {
            if (!IsGuestRegister(operation.dst) ||
                (operation.base != 0 &&
                 (operation.base !=
                     X64Fast::FAST_OP_TEST_REG8_IMM_HIGH_BYTE ||
                  operation.dst >= 4)))
            {
                return false;
            }
        }
    }

    return true;
}

bool IsProvablySnapshotIndependent(const X64Fast::FastOp& operation) noexcept
{
    using X64Fast::FastOpKind;

    if (!HasValidSnapshotMetadata(operation)) return false;

    // These operations can access guest memory or enter a host helper. A
    // nested guest Run can then recycle the TLS FastBlock that owns the live
    // operation array, so the ordinary executor must retain its snapshot.
    switch (operation.kind)
    {
        case FastOpKind::PushReg64:
        case FastOpKind::PopReg64:
        case FastOpKind::MovReg8Mem:
        case FastOpKind::MovReg16Mem:
        case FastOpKind::MovReg32Mem:
        case FastOpKind::MovReg64Mem:
        case FastOpKind::StoreReg8:
        case FastOpKind::StoreReg16:
        case FastOpKind::StoreReg32:
        case FastOpKind::StoreReg64:
        case FastOpKind::MovImmMem8:
        case FastOpKind::MovImmMem16:
        case FastOpKind::MovImmMem32:
        case FastOpKind::MovImmMem64:
        case FastOpKind::MovsxdReg64Mem32:
        case FastOpKind::MovzxReg32Mem8:
        case FastOpKind::MovzxReg64Mem8:
        case FastOpKind::MovzxReg32Mem16:
        case FastOpKind::MovzxReg64Mem16:
        case FastOpKind::MovsxReg32Mem8:
        case FastOpKind::MovsxReg32Mem16:
        case FastOpKind::MovsxReg64Mem8:
        case FastOpKind::MovsxReg64Mem16:
        case FastOpKind::OrReg16Mem:
        case FastOpKind::AndReg8Mem:
        case FastOpKind::AndReg32Mem:
        case FastOpKind::AndReg64Mem:
        case FastOpKind::XorReg32Mem:
        case FastOpKind::XorReg64Mem:
        case FastOpKind::ImulReg32MemImm:
        case FastOpKind::ImulReg64MemImm:
        case FastOpKind::ImulReg32Mem:
        case FastOpKind::ImulReg64Mem:
        case FastOpKind::AddReg32Mem:
        case FastOpKind::AddReg64Mem:
        case FastOpKind::SubReg32Mem:
        case FastOpKind::SubReg64Mem:
        case FastOpKind::CmpReg32Mem:
        case FastOpKind::CmpReg64Mem:
        case FastOpKind::CmpMem32Reg:
        case FastOpKind::CmpMem64Reg:
        case FastOpKind::CmpMem8Imm:
        case FastOpKind::CmpMem16Imm:
        case FastOpKind::CmpMem32Imm:
        case FastOpKind::CmpMem64Imm:
        case FastOpKind::TestMem8Imm:
        case FastOpKind::OrReg8Mem:
        case FastOpKind::AddMem32Reg:
        case FastOpKind::AddMem64Reg:
        case FastOpKind::SubMem32Reg:
        case FastOpKind::SubMem64Reg:
        case FastOpKind::AndMem32Reg:
        case FastOpKind::AndMem64Reg:
        case FastOpKind::AndMem8Imm:
        case FastOpKind::IncMem32:
        case FastOpKind::IncMem64:
        case FastOpKind::SetccMem8:
        case FastOpKind::VmovssMem:
        case FastOpKind::VmovssStoreMem:
        case FastOpKind::VmovsdMem:
        case FastOpKind::VmovsdStoreMem:
        case FastOpKind::VmovPackedMem:
        case FastOpKind::VmovPackedStoreMem:
        case FastOpKind::VxorPackedMem:
        case FastOpKind::Ret:
        case FastOpKind::Call:
        case FastOpKind::Vcvttss2siReg32Mem:
        case FastOpKind::Vcvttss2siReg32Reg:
        case FastOpKind::Vcvttss2siReg64Mem:
        case FastOpKind::Vcvttss2siReg64Reg:
        case FastOpKind::Vcvtsi2ssReg32Mem:
        case FastOpKind::Vcvtsi2ssReg32Reg:
        case FastOpKind::Vcvtsi2ssReg64Mem:
        case FastOpKind::Vcvtsi2ssReg64Reg:
        case FastOpKind::VaddssMem:
        case FastOpKind::VaddssReg:
        case FastOpKind::VmulssMem:
        case FastOpKind::VmulssReg:
        case FastOpKind::VsubssMem:
        case FastOpKind::VsubssReg:
        case FastOpKind::VdivssMem:
        case FastOpKind::VdivssReg:
        case FastOpKind::VaddsdMem:
        case FastOpKind::VaddsdReg:
        case FastOpKind::VmulsdMem:
        case FastOpKind::VmulsdReg:
        case FastOpKind::VsubsdMem:
        case FastOpKind::VsubsdReg:
        case FastOpKind::VdivsdMem:
        case FastOpKind::VdivsdReg:
        case FastOpKind::VaddpsMem:
        case FastOpKind::VaddpsReg:
        case FastOpKind::VmulpsMem:
        case FastOpKind::VmulpsReg:
        case FastOpKind::VsubpsMem:
        case FastOpKind::VsubpsReg:
        case FastOpKind::VsubpdMem128:
        case FastOpKind::VhaddpdReg128:
        case FastOpKind::VpunpckldqMem128:
        case FastOpKind::VmovqReg64ToXmm:
        case FastOpKind::VucomissMem:
        case FastOpKind::VucomissReg:
        case FastOpKind::VucomisdMem:
        case FastOpKind::VucomisdReg:
        case FastOpKind::LockCmpxchgMem64Reg:
        case FastOpKind::CallReg64:
        case FastOpKind::CallMem64:
        case FastOpKind::JmpReg64:
        case FastOpKind::JmpMem64:
        case FastOpKind::RepMovs8:
        case FastOpKind::RepMovs64:
        case FastOpKind::RepStos8:
        case FastOpKind::RepStos64:
            return false;
        default:
            return true;
    }
}

SnapshotEffectClass ClassifySnapshotEffects(
    const X64Fast::FastOp& operation) noexcept
{
    using X64Fast::FastOpKind;

    if (!HasValidSnapshotMetadata(operation))
        return SnapshotEffectClass::Unknown;
    if (IsProvablySnapshotIndependent(operation))
        return SnapshotEffectClass::Independent;

    // Conservative no-JIT proof for the direct scalar/stack/RMW operations
    // already covered by the ordinary FastBlock executor and phase-6CP
    // semantic tests. SIMD memory remains unknown in this first Android
    // tranche, as do all helper, indirect-control, atomic and REP paths. The
    // operand checks mirror the production lowering validation for each
    // admitted family; malformed decoder records remain fail-closed.
    switch (operation.kind)
    {
        case FastOpKind::PushReg64:
            return IsGuestRegister(operation.src)
                       ? SnapshotEffectClass::DirectScalarGuestMemory
                       : SnapshotEffectClass::Unknown;
        case FastOpKind::PopReg64:
            return IsGuestRegister(operation.dst)
                       ? SnapshotEffectClass::DirectScalarGuestMemory
                       : SnapshotEffectClass::Unknown;
        case FastOpKind::MovReg8Mem:
        case FastOpKind::MovReg16Mem:
        case FastOpKind::MovReg32Mem:
        case FastOpKind::MovReg64Mem:
        case FastOpKind::MovsxdReg64Mem32:
        case FastOpKind::MovzxReg32Mem8:
        case FastOpKind::MovzxReg64Mem8:
        case FastOpKind::MovzxReg32Mem16:
        case FastOpKind::MovzxReg64Mem16:
        case FastOpKind::MovsxReg32Mem8:
        case FastOpKind::MovsxReg32Mem16:
        case FastOpKind::MovsxReg64Mem8:
        case FastOpKind::MovsxReg64Mem16:
        case FastOpKind::OrReg8Mem:
        case FastOpKind::OrReg16Mem:
        case FastOpKind::AndReg8Mem:
        case FastOpKind::AndReg32Mem:
        case FastOpKind::AndReg64Mem:
        case FastOpKind::XorReg32Mem:
        case FastOpKind::XorReg64Mem:
        case FastOpKind::ImulReg32MemImm:
        case FastOpKind::ImulReg64MemImm:
        case FastOpKind::ImulReg32Mem:
        case FastOpKind::ImulReg64Mem:
        case FastOpKind::AddReg32Mem:
        case FastOpKind::AddReg64Mem:
        case FastOpKind::SubReg32Mem:
        case FastOpKind::SubReg64Mem:
        case FastOpKind::CmpReg32Mem:
        case FastOpKind::CmpReg64Mem:
            return IsGuestRegister(operation.dst) &&
                           HasValidGuestMemoryAddress(operation, true)
                       ? SnapshotEffectClass::DirectScalarGuestMemory
                       : SnapshotEffectClass::Unknown;
        case FastOpKind::StoreReg8:
        case FastOpKind::StoreReg16:
        case FastOpKind::StoreReg32:
        case FastOpKind::StoreReg64:
            return IsGuestRegister(operation.src) &&
                           HasValidGuestMemoryAddress(operation, false)
                       ? SnapshotEffectClass::DirectScalarGuestMemory
                       : SnapshotEffectClass::Unknown;
        case FastOpKind::MovImmMem8:
        case FastOpKind::MovImmMem16:
        case FastOpKind::MovImmMem32:
        case FastOpKind::MovImmMem64:
        case FastOpKind::CmpMem8Imm:
        case FastOpKind::CmpMem16Imm:
        case FastOpKind::CmpMem32Imm:
        case FastOpKind::CmpMem64Imm:
        case FastOpKind::TestMem8Imm:
        case FastOpKind::AndMem8Imm:
        case FastOpKind::IncMem32:
        case FastOpKind::IncMem64:
            return HasValidGuestMemoryAddress(operation, true)
                       ? SnapshotEffectClass::DirectScalarGuestMemory
                       : SnapshotEffectClass::Unknown;
        case FastOpKind::CmpMem32Reg:
        case FastOpKind::CmpMem64Reg:
        case FastOpKind::AddMem32Reg:
        case FastOpKind::AddMem64Reg:
        case FastOpKind::SubMem32Reg:
        case FastOpKind::SubMem64Reg:
        case FastOpKind::AndMem32Reg:
        case FastOpKind::AndMem64Reg:
            return IsGuestRegister(operation.src) &&
                           HasValidGuestMemoryAddress(operation, true)
                       ? SnapshotEffectClass::DirectScalarGuestMemory
                       : SnapshotEffectClass::Unknown;
        case FastOpKind::SetccMem8:
            return operation.condition <= 15 &&
                           HasValidGuestMemoryAddress(operation, true)
                       ? SnapshotEffectClass::DirectScalarGuestMemory
                       : SnapshotEffectClass::Unknown;
        case FastOpKind::Ret:
            return operation.immediate >= 0 &&
                           operation.immediate <=
                               static_cast<int32_t>(UINT16_MAX)
                       ? SnapshotEffectClass::DirectScalarGuestMemory
                       : SnapshotEffectClass::Unknown;
        default:
            return SnapshotEffectClass::Unknown;
    }
}

} // namespace

const char* StatusName(Status status)
{
    switch (status)
    {
        case Status::Ok: return "Ok";
        case Status::UnsupportedHost: return "UnsupportedHost";
        case Status::InvalidArgument: return "InvalidArgument";
        case Status::AllocationFailed: return "AllocationFailed";
        case Status::NotAllocated: return "NotAllocated";
        case Status::CapacityExceeded: return "CapacityExceeded";
        case Status::AlreadyAllocated: return "AlreadyAllocated";
        case Status::AlreadyPublished: return "AlreadyPublished";
        case Status::UnsupportedOperation: return "UnsupportedOperation";
        case Status::InvalidIr: return "InvalidIr";
        case Status::PlatformFailure: return "PlatformFailure";
    }
    return "Unknown";
}

FastOpExecutionSnapshotAnalysis AnalyzeFastOpExecutionSnapshot(
    const X64Fast::FastOp& operation) noexcept
{
    FastOpExecutionSnapshotAnalysis analysis {};
    switch (ClassifySnapshotEffects(operation))
    {
        case SnapshotEffectClass::Independent:
            analysis.known = true;
            analysis.unknown = false;
            break;
        case SnapshotEffectClass::DirectScalarGuestMemory:
            analysis.known = true;
            analysis.guest_memory = true;
            analysis.unknown = false;
            analysis.snapshot_avoidance_candidate = true;
            break;
        case SnapshotEffectClass::Unknown: break;
    }
    return analysis;
}

FastBlockOperationSnapshotAnalysis AnalyzeFastBlockOperationSnapshot(
    const X64Fast::FastOp* operations, size_t operation_count) noexcept
{
    FastBlockOperationSnapshotAnalysis analysis {};
    analysis.operation_count = operation_count;
    if (operation_count == 0) return analysis;
    if (operations == nullptr || operation_count > MAX_IR_INSTRUCTIONS)
    {
        analysis.unknown_operation_count = operation_count;
        return analysis;
    }
    for (size_t index = 0; index < operation_count; index++)
    {
        const auto operation_analysis =
            AnalyzeFastOpExecutionSnapshot(operations[index]);
        if (operation_analysis.known) analysis.known_operation_count++;
        if (operation_analysis.guest_memory)
            analysis.guest_memory_operation_count++;
        if (operation_analysis.host_call)
            analysis.host_call_operation_count++;
        if (operation_analysis.unknown) analysis.unknown_operation_count++;
    }
    analysis.snapshot_avoidance_candidate =
        analysis.known_operation_count == operation_count &&
        analysis.guest_memory_operation_count != 0 &&
        analysis.host_call_operation_count == 0 &&
        analysis.unknown_operation_count == 0;
    return analysis;
}

bool FastOpRequiresExecutionSnapshot(const X64Fast::FastOp& operation)
{
    return !IsProvablySnapshotIndependent(operation);
}

VerifiedIrBlock::VerifiedIrBlock(IrBlock&& block, uint8_t eligibility_mask)
    : m_block(std::move(block)), m_eligibility_mask(eligibility_mask)
{
}

VerifiedIrBlock::VerifiedIrBlock() = default;
VerifiedIrBlock::VerifiedIrBlock(LoweringStorageTag): VerifiedIrBlock() {}

const IrBlock& VerifiedIrBlock::Block() const noexcept { return m_block; }
bool VerifiedIrBlock::Supports(EligibilityPolicy) const noexcept
{
    // Keep the capability layout identical to the full provider while the
    // Android/no-JIT provider remains fail-closed.
    (void)m_eligibility_mask;
    return false;
}
bool VerifiedIrBlock::IsDirectFallthrough() const noexcept { return false; }
bool VerifiedIrBlock::IsDirectBasicBlock() const noexcept { return false; }
bool VerifiedIrBlock::IsProductionScalarMemoryFallthrough() const noexcept
{
    return false;
}

Status LowerFastOps(const X64Fast::FastOp*, size_t, IrBlock* output)
{
    if (output != nullptr) *output = {};
    return Status::UnsupportedHost;
}

VerifiedIrResult LowerFastOpsVerified(const X64Fast::FastOp*, size_t)
{
    return {Status::UnsupportedHost, std::nullopt};
}

Status LowerFastOpsVerified(const X64Fast::FastOp*, size_t,
                            VerifiedIrBlock* output)
{
    if (output != nullptr)
    {
        output->m_block = {};
        output->m_eligibility_mask = 0;
    }
    return Status::UnsupportedHost;
}

Status VerifyIrBlock(const IrBlock&)
{
    return Status::UnsupportedHost;
}

Status AnalyzeBlockGprCachePlan(const IrBlock&, BlockGprCachePlan* output)
{
    if (output == nullptr) return Status::InvalidArgument;
    *output = {};
    return Status::UnsupportedHost;
}

bool IsRuntimeEligible(const IrBlock&)
{
    return false;
}

bool IsRuntimeEligible(const IrBlock&, EligibilityPolicy)
{
    return false;
}

bool IsRuntimeEligible(const VerifiedIrBlock&, EligibilityPolicy)
{
    return false;
}

uint32_t ExecuteIrReference(const IrBlock&, EntryContext* context)
{
    if (context != nullptr) context->exit_reason = ExitReason::Bailout;
    return 0;
}

uint32_t ExecuteIrReference(const IrBlock&, EntryContext* context,
                            EligibilityPolicy)
{
    if (context != nullptr) context->exit_reason = ExitReason::Bailout;
    return 0;
}

Status EncodeIrBlock(const IrBlock&, void*, size_t, size_t* encoded_size,
                     uint8_t* direct_instruction_count)
{
    if (encoded_size != nullptr) *encoded_size = 0;
    if (direct_instruction_count != nullptr) *direct_instruction_count = 0;
    return Status::UnsupportedHost;
}

Status EncodeIrBlock(const IrBlock&, void*, size_t, size_t* encoded_size,
                     uint8_t* direct_instruction_count, EligibilityPolicy)
{
    if (encoded_size != nullptr) *encoded_size = 0;
    if (direct_instruction_count != nullptr) *direct_instruction_count = 0;
    return Status::UnsupportedHost;
}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST)
Status EncodeIrBlockWithBatchDirectRetirementForTest(
    const IrBlock&, void*, size_t, size_t* encoded_size,
    uint8_t* direct_instruction_count, EligibilityPolicy, bool)
{
    if (encoded_size != nullptr) *encoded_size = 0;
    if (direct_instruction_count != nullptr) *direct_instruction_count = 0;
    return Status::UnsupportedHost;
}

Status EncodeIrBlockWithDirectLeafAbiForTest(
    const IrBlock&, void*, size_t, size_t* encoded_size,
    uint8_t* direct_instruction_count, EligibilityPolicy, bool, bool)
{
    if (encoded_size != nullptr) *encoded_size = 0;
    if (direct_instruction_count != nullptr) *direct_instruction_count = 0;
    return Status::UnsupportedHost;
}

Status EncodeImmediate64ForTest(uint8_t, uint64_t, bool, void*, size_t, size_t* encoded_size)
{
    if (encoded_size != nullptr) *encoded_size = 0;
    return Status::UnsupportedHost;
}

Status EncodeIrBlockWithCompactImmediatesForTest(const IrBlock&, void*, size_t, size_t* encoded_size, uint8_t* direct_instruction_count,
                                                 EligibilityPolicy, bool, bool, bool)
{
    if (encoded_size != nullptr) *encoded_size = 0;
    if (direct_instruction_count != nullptr) *direct_instruction_count = 0;
    return Status::UnsupportedHost;
}

bool BatchDirectRetirementEnvironmentValueEnabledForTest(const char* value)
{
    return value == nullptr || std::strcmp(value, "1") == 0;
}

bool DirectLeafAbiEnvironmentValueEnabledForTest(const char* value)
{
    return value == nullptr || std::strcmp(value, "1") == 0;
}

bool DirectContextReuseEnvironmentValueEnabledForTest(const char* value)
{
    return value == nullptr || std::strcmp(value, "1") == 0;
}

bool CompactImmediateEnvironmentValueEnabledForTest(const char* value)
{
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool BlockRflagsCacheEnvironmentValueEnabledForTest(const char* value)
{
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool BlockGprCacheEnvironmentValueEnabledForTest(const char* value)
{
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool MultiSegmentArtifactEnvironmentValueEnabledForTest(const char* value)
{
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool DenseResidentCacheEnvironmentValueEnabledForTest(const char* value)
{
    return value == nullptr || std::strcmp(value, "1") == 0;
}

Status LowerFastOpsWithPacked256ForTest(
    const X64Fast::FastOp*, size_t, IrBlock* output, bool)
{
    if (output != nullptr) *output = {};
    return Status::UnsupportedHost;
}

Status EncodeIrBlockWithPacked256ForTest(
    const IrBlock&, void*, size_t, size_t* encoded_size,
    uint8_t* direct_instruction_count, EligibilityPolicy, bool)
{
    if (encoded_size != nullptr) *encoded_size = 0;
    if (direct_instruction_count != nullptr) *direct_instruction_count = 0;
    return Status::UnsupportedHost;
}

bool Packed256EnvironmentValueEnabledForTest(const char* value)
{
    return value != nullptr && std::strcmp(value, "1") == 0;
}
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_VERIFIED_IR_BENCHMARK) || \
    defined(LUMEN_JIT_PUBLICATION_BENCHMARK) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
Status EncodeVerifiedIrBlockForTest(
    const VerifiedIrBlock&, void*, size_t, size_t* encoded_size,
    uint8_t* direct_instruction_count, EligibilityPolicy)
{
    if (encoded_size != nullptr) *encoded_size = 0;
    if (direct_instruction_count != nullptr) *direct_instruction_count = 0;
    return Status::UnsupportedHost;
}
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || defined(LUMEN_CPU_X64_TEST)
void ResetIrVerificationTestCounters() {}
IrVerificationTestCounters GetIrVerificationTestCounters() { return {}; }
#endif

A64CodeBuffer::A64CodeBuffer(void* storage, size_t capacity_bytes)
    : m_storage(static_cast<uint8_t*>(storage)),
      m_capacity_bytes(capacity_bytes)
{
}

Status A64CodeBuffer::Reserve(size_t) const { return Status::UnsupportedHost; }
void A64CodeBuffer::EmitUnchecked(uint32_t) {}
Status A64CodeBuffer::EmitBtiCall() { return Status::UnsupportedHost; }
Status A64CodeBuffer::EmitMoveZero64(uint8_t, uint16_t, uint8_t) { return Status::UnsupportedHost; }
Status A64CodeBuffer::EmitMoveKeep64(uint8_t, uint16_t, uint8_t) { return Status::UnsupportedHost; }
Status A64CodeBuffer::EmitMoveImmediate64(uint8_t, uint64_t) { return Status::UnsupportedHost; }
Status A64CodeBuffer::EmitReturn(uint8_t) { return Status::UnsupportedHost; }
Status A64CodeBuffer::EmitReturnConstant(uint64_t) { return Status::UnsupportedHost; }
Status A64CodeBuffer::EmitEntryContextSmoke() { return Status::UnsupportedHost; }
const void* A64CodeBuffer::Data() const { return m_storage; }
uint32_t A64CodeBuffer::WordAt(size_t) const { return 0; }
size_t A64CodeBuffer::WordCount() const { return 0; }
size_t A64CodeBuffer::SizeBytes() const { return m_size_bytes; }
size_t A64CodeBuffer::CapacityBytes() const { return m_capacity_bytes; }

ExecutableCodeMemory::~ExecutableCodeMemory() = default;
Status ExecutableCodeMemory::Allocate(size_t) { return Status::UnsupportedHost; }
Status ExecutableCodeMemory::AllocateDenseResident(size_t)
{
    return Status::UnsupportedHost;
}
Status ExecutableCodeMemory::Publish(const void*, size_t) { return Status::UnsupportedHost; }
void ExecutableCodeMemory::Reset() { ResetUnlocked(); }
void ExecutableCodeMemory::RetireForReuseAfterQuiescence() { ResetUnlocked(); }
#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
void ExecutableCodeMemory::RetireForReuseAfterQuiescenceForTest()
{
    RetireForReuseAfterQuiescence();
}
#endif
bool ExecutableCodeMemory::ResetUnlocked()
{
    m_address = nullptr;
    m_capacity = 0;
    m_published_size.store(0, std::memory_order_relaxed);
    m_publish_started = false;
    m_recycled_lease = false;
    m_dense_resident_slice = false;
    m_dense_resident_page = UINT32_MAX;
    m_dense_resident_slot = UINT8_MAX;
    m_epoch = 0;
    return true;
}
const void* ExecutableCodeMemory::EntryAddress() const { return m_address; }
size_t ExecutableCodeMemory::Capacity() const { return m_capacity; }
size_t ExecutableCodeMemory::PublishedSize() const
{
    return m_published_size.load(std::memory_order_relaxed);
}
uint64_t ExecutableCodeMemory::Epoch() const { return m_epoch; }
bool ExecutableCodeMemory::IsPublished() const { return m_publish_started; }

Status CompiledBlock::Compile(const IrBlock&) { return Status::UnsupportedHost; }
Status CompiledBlock::Compile(const IrBlock&, EligibilityPolicy) { return Status::UnsupportedHost; }
Status CompiledBlock::CompileVerified(const VerifiedIrBlock&, EligibilityPolicy)
{
    return Status::UnsupportedHost;
}
Status CompiledBlock::CompileMultiSegment(const MultiSegmentArtifactSpec&)
{
    return Status::UnsupportedHost;
}
Status CompiledBlock::CompileWithMinimumCapacity(const IrBlock&, EligibilityPolicy, size_t)
{
    return Status::UnsupportedHost;
}
uint32_t CompiledBlock::Execute(EntryContext* context) const
{
    if (context != nullptr) context->exit_reason = ExitReason::Bailout;
    return 0;
}
uint32_t CompiledBlock::ExecutePreparedDirect(EntryContext* context) const
{
    if (context != nullptr) context->exit_reason = ExitReason::Bailout;
    return 0;
}
const IrBlock& CompiledBlock::Ir() const { return m_ir; }
uint64_t CompiledBlock::Epoch() const { return m_code.Epoch(); }
const void* CompiledBlock::CodeAddress() const { return m_code.EntryAddress(); }
size_t CompiledBlock::CodeSize() const { return m_code.PublishedSize(); }
size_t CompiledBlock::CodeCapacity() const { return m_code.Capacity(); }
uint8_t CompiledBlock::DirectInstructionCount() const { return m_direct_instruction_count; }
bool CompiledBlock::IsDirectFallthrough() const { return m_direct_fallthrough; }
bool CompiledBlock::IsDirectBasicBlock() const { return m_direct_basic_block; }
bool CompiledBlock::IsProductionScalarMemoryFallthrough() const
{
    return m_production_scalar_memory_fallthrough;
}
bool CompiledBlock::IsCompiled() const { return m_code.IsPublished(); }
void CompiledBlock::RetireCodeForReuseAfterQuiescence()
{
    m_code.RetireForReuseAfterQuiescence();
    m_production_scalar_memory_fallthrough = false;
}

Status DenseResidentBlock::CompileVerified(const VerifiedIrBlock&,
                                           EligibilityPolicy)
{
    return Status::UnsupportedHost;
}
#if defined(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
Status DenseResidentBlock::CompileVerified(const VerifiedIrBlock&,
                                           EligibilityPolicy,
                                           bool,
                                           DenseResidentCapacityReclaimer,
                                           void*,
                                           DenseResidentCompileTiming*)
#else
Status DenseResidentBlock::CompileVerified(const VerifiedIrBlock&,
                                           EligibilityPolicy,
                                           bool,
                                           DenseResidentCapacityReclaimer,
                                           void*)
#endif
{
    return Status::UnsupportedHost;
}
uint32_t DenseResidentBlock::ExecutePreparedDirect(EntryContext* context) const
{
    (void)m_rip;
    (void)m_instruction_count;
    if (context != nullptr) context->exit_reason = ExitReason::Bailout;
    return 0;
}
uint32_t DenseResidentBlock::ExecutePreparedDirectKnownValid(
    EntryContext* context) const
{
    if (context != nullptr) context->exit_reason = ExitReason::Bailout;
    return 0;
}
uint32_t DenseResidentBlock::ExecuteContextlessDirect(X64State*, uint64_t) const
{
    return 0;
}
uint64_t DenseResidentBlock::Epoch() const { return m_code.Epoch(); }
const void* DenseResidentBlock::CodeAddress() const
{
    return m_code.EntryAddress();
}
size_t DenseResidentBlock::CodeSize() const { return m_code.PublishedSize(); }
size_t DenseResidentBlock::CodeCapacity() const { return m_code.Capacity(); }
bool DenseResidentBlock::IsDirectFallthrough() const
{
    return m_direct_fallthrough;
}
bool DenseResidentBlock::IsContextlessDirect() const
{
    return m_contextless_direct;
}
bool DenseResidentBlock::IsCompiled() const { return m_code.IsPublished(); }
bool DenseResidentBlock::MatchesMetadata(uint64_t rip,
                                         uint64_t fallthrough_rip,
                                         uint8_t instruction_count,
                                         bool direct_fallthrough,
                                         bool contextless_direct) const
{
    return IsCompiled() && m_rip == rip &&
           m_fallthrough_rip == fallthrough_rip &&
           m_instruction_count == instruction_count &&
           m_direct_fallthrough == direct_fallthrough &&
           m_contextless_direct == contextless_direct;
}

bool IsHostSupported()
{
    return false;
}

bool IsJitPageRecycleEnabled()
{
    return false;
}

bool IsBatchDirectRetirementEnabled()
{
    return false;
}

bool IsDirectLeafAbiEnabled()
{
    return false;
}

bool IsDirectContextReuseEnabled()
{
    return false;
}

bool IsCompactImmediateEncodingEnabled()
{
	return false;
}

bool IsPacked256Enabled()
{
    return false;
}

bool IsBlockRflagsCacheEnabled()
{
    return false;
}

bool IsBlockGprCacheEnabled()
{
    return false;
}

bool IsMultiSegmentArtifactEnabled()
{
    return false;
}

bool IsDenseResidentCacheEnabled()
{
    return false;
}

uint64_t DenseResidentReleaseGeneration()
{
    return 0;
}

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST)
DenseResidentArenaTestSnapshot GetDenseResidentArenaTestSnapshot()
{
    DenseResidentArenaTestSnapshot snapshot {};
    snapshot.executable_budget = DENSE_RESIDENT_EXECUTABLE_BUDGET;
    return snapshot;
}
#endif

#if defined(LUMEN_CPU_A64_DYNAREC_TEST) || \
    defined(LUMEN_CPU_X64_TEST) || \
    defined(LUMEN_JIT_PAGE_RECYCLE_BENCHMARK)
bool JitPageRecycleEnvironmentValueEnabledForTest(const char* value)
{
    return value == nullptr || std::strcmp(value, "0") != 0;
}

JitPageRecycleTestSnapshot GetJitPageRecycleTestSnapshot()
{
    return {};
}

void ResetJitPageRecyclePoolForTest() {}
#endif

} // namespace Lumen::Cpu::A64Dynarec
