#include "CpuA64Dynarec.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

using Lumen::Cpu::A64Dynarec::AnalyzeFastBlockOperationSnapshot;
using Lumen::Cpu::A64Dynarec::AnalyzeFastOpExecutionSnapshot;
using Lumen::Cpu::A64Dynarec::FastOpRequiresExecutionSnapshot;
using Lumen::Cpu::X64Fast::FastOp;
using Lumen::Cpu::X64Fast::FastOpKind;

int g_failures = 0;

constexpr std::array<FastOpKind, 58> PROVEN_SCALAR_GUEST_MEMORY_KINDS = {
    FastOpKind::PushReg64, FastOpKind::PopReg64,
    FastOpKind::MovReg8Mem, FastOpKind::MovReg16Mem,
    FastOpKind::MovReg32Mem, FastOpKind::MovReg64Mem,
    FastOpKind::StoreReg8, FastOpKind::StoreReg16,
    FastOpKind::StoreReg32, FastOpKind::StoreReg64,
    FastOpKind::MovImmMem8, FastOpKind::MovImmMem16,
    FastOpKind::MovImmMem32, FastOpKind::MovImmMem64,
    FastOpKind::MovsxdReg64Mem32,
    FastOpKind::MovzxReg32Mem8, FastOpKind::MovzxReg64Mem8,
    FastOpKind::MovzxReg32Mem16, FastOpKind::MovzxReg64Mem16,
    FastOpKind::MovsxReg32Mem8, FastOpKind::MovsxReg32Mem16,
    FastOpKind::MovsxReg64Mem8, FastOpKind::MovsxReg64Mem16,
    FastOpKind::OrReg8Mem, FastOpKind::OrReg16Mem,
    FastOpKind::AndReg8Mem, FastOpKind::AndReg32Mem,
    FastOpKind::AndReg64Mem, FastOpKind::XorReg32Mem,
    FastOpKind::XorReg64Mem, FastOpKind::ImulReg32MemImm,
    FastOpKind::ImulReg64MemImm, FastOpKind::ImulReg32Mem,
    FastOpKind::ImulReg64Mem, FastOpKind::AddReg32Mem,
    FastOpKind::AddReg64Mem, FastOpKind::SubReg32Mem,
    FastOpKind::SubReg64Mem, FastOpKind::CmpReg32Mem,
    FastOpKind::CmpReg64Mem, FastOpKind::CmpMem32Reg,
    FastOpKind::CmpMem64Reg, FastOpKind::CmpMem8Imm,
    FastOpKind::CmpMem16Imm, FastOpKind::CmpMem32Imm,
    FastOpKind::CmpMem64Imm, FastOpKind::TestMem8Imm,
    FastOpKind::AddMem32Reg, FastOpKind::AddMem64Reg,
    FastOpKind::SubMem32Reg, FastOpKind::SubMem64Reg,
    FastOpKind::AndMem32Reg, FastOpKind::AndMem64Reg,
    FastOpKind::AndMem8Imm, FastOpKind::IncMem32,
    FastOpKind::IncMem64, FastOpKind::SetccMem8,
    FastOpKind::Ret,
};

void Expect(bool condition, const char* expression, int line)
{
    if (condition) return;
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, line,
                 expression);
    g_failures++;
}

#define EXPECT_TRUE(expression) Expect((expression), #expression, __LINE__)

FastOp Operation(FastOpKind kind, uint8_t length = 3)
{
    FastOp operation {};
    operation.kind = kind;
    operation.length = length;
    operation.rip = UINT64_C(0x100000000);
    return operation;
}

bool IsExpectedProvenGuestMemoryKind(FastOpKind kind)
{
    for (const FastOpKind expected : PROVEN_SCALAR_GUEST_MEMORY_KINDS)
    {
        if (kind == expected) return true;
    }
    return false;
}

} // namespace

int main()
{
    const FastOp register_only = Operation(FastOpKind::AddReg64);
    const FastOp direct_branch = Operation(FastOpKind::Jmp, 2);
    const FastOp guest_memory = Operation(FastOpKind::MovReg64Mem);
    const FastOp guest_store = Operation(FastOpKind::StoreReg64);
    const FastOp guest_push = Operation(FastOpKind::PushReg64);
    const FastOp guest_pop = Operation(FastOpKind::PopReg64);
    const FastOp guest_return = Operation(FastOpKind::Ret);
    const FastOp conservative_simd = Operation(FastOpKind::VmovssMem);
    const FastOp host_helper = Operation(FastOpKind::VaddssReg);

    EXPECT_TRUE(!FastOpRequiresExecutionSnapshot(register_only));
    EXPECT_TRUE(!FastOpRequiresExecutionSnapshot(direct_branch));
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(guest_memory));
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(guest_store));
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(guest_push));
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(guest_pop));
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(guest_return));
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(conservative_simd));
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(host_helper));

    const auto register_analysis =
        AnalyzeFastOpExecutionSnapshot(register_only);
    EXPECT_TRUE(register_analysis.known && !register_analysis.unknown);
    EXPECT_TRUE(!register_analysis.guest_memory &&
                !register_analysis.host_call);

    const std::array<FastOp, 5> proven_guest_memory = {
        guest_memory, guest_store, guest_push, guest_pop, guest_return,
    };
    for (const FastOp& operation : proven_guest_memory)
    {
        const auto analysis = AnalyzeFastOpExecutionSnapshot(operation);
        EXPECT_TRUE(analysis.known && !analysis.unknown);
        EXPECT_TRUE(analysis.guest_memory && !analysis.host_call);
        EXPECT_TRUE(analysis.snapshot_avoidance_candidate);
    }

    const auto simd_analysis =
        AnalyzeFastOpExecutionSnapshot(conservative_simd);
    EXPECT_TRUE(!simd_analysis.known && simd_analysis.unknown);
    EXPECT_TRUE(!simd_analysis.snapshot_avoidance_candidate);

    const auto helper_analysis = AnalyzeFastOpExecutionSnapshot(host_helper);
    EXPECT_TRUE(!helper_analysis.known && helper_analysis.unknown);
    EXPECT_TRUE(!helper_analysis.snapshot_avoidance_candidate);

    FastOp malformed = register_only;
    malformed.length = 0;
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(malformed));
    const auto malformed_analysis = AnalyzeFastOpExecutionSnapshot(malformed);
    EXPECT_TRUE(!malformed_analysis.known && malformed_analysis.unknown);

    FastOp malformed_guest = guest_memory;
    malformed_guest.length = 0;
    const auto malformed_guest_analysis =
        AnalyzeFastOpExecutionSnapshot(malformed_guest);
    EXPECT_TRUE(!malformed_guest_analysis.known &&
                malformed_guest_analysis.unknown);
    EXPECT_TRUE(!malformed_guest_analysis.snapshot_avoidance_candidate);

    malformed_guest = guest_memory;
    malformed_guest.length = 16;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_guest).unknown);
    malformed_guest = guest_memory;
    malformed_guest.rip = UINT64_MAX - malformed_guest.length + 1;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_guest).unknown);

    FastOp malformed_address = guest_memory;
    malformed_address.scale = 4;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_address).unknown);
    malformed_address = guest_memory;
    malformed_address.base =
        Lumen::Cpu::A64Dynarec::IR_NO_REGISTER;
    malformed_address.index = 0;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_address).unknown);

    FastOp malformed_register = guest_memory;
    malformed_register.dst = 16;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_register).unknown);
    malformed_register = guest_store;
    malformed_register.src = 16;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_register).unknown);
    malformed_register = guest_push;
    malformed_register.src = 16;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_register).unknown);
    malformed_register = guest_pop;
    malformed_register.dst = 16;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_register).unknown);

    FastOp malformed_return = guest_return;
    malformed_return.immediate = -1;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_return).unknown);
    malformed_return.immediate = UINT16_MAX + 1;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_return).unknown);

    FastOp malformed_condition = Operation(FastOpKind::SetccMem8);
    malformed_condition.condition = 16;
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_condition).unknown);

    FastOp unknown = register_only;
    unknown.kind = static_cast<FastOpKind>(UINT8_MAX);
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(unknown));

    FastOp overflowing = register_only;
    overflowing.rip = UINT64_MAX - overflowing.length + 1;
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(overflowing));

    FastOp compare_no_flags = Operation(FastOpKind::CompareNoFlags);
    compare_no_flags.condition =
        static_cast<uint8_t>(FastOpKind::CmpReg64);
    EXPECT_TRUE(!FastOpRequiresExecutionSnapshot(compare_no_flags));
    compare_no_flags.condition =
        static_cast<uint8_t>(FastOpKind::CmpReg32Mem);
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(compare_no_flags));

    FastOp test_no_flags = Operation(FastOpKind::TestNoFlags);
    test_no_flags.condition = static_cast<uint8_t>(FastOpKind::TestReg64);
    EXPECT_TRUE(!FastOpRequiresExecutionSnapshot(test_no_flags));
    test_no_flags.condition =
        static_cast<uint8_t>(FastOpKind::TestMem8Imm);
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(test_no_flags));

    FastOp high_byte_test = Operation(FastOpKind::TestReg8Imm);
    high_byte_test.dst = 0;
    high_byte_test.base =
        Lumen::Cpu::X64Fast::FAST_OP_TEST_REG8_IMM_HIGH_BYTE;
    EXPECT_TRUE(!FastOpRequiresExecutionSnapshot(high_byte_test));
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(high_byte_test).known);

    FastOp high_byte_test_no_flags = high_byte_test;
    high_byte_test_no_flags.kind = FastOpKind::TestNoFlags;
    high_byte_test_no_flags.condition =
        static_cast<uint8_t>(FastOpKind::TestReg8Imm);
    EXPECT_TRUE(!FastOpRequiresExecutionSnapshot(high_byte_test_no_flags));
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(high_byte_test_no_flags).known);

    FastOp malformed_high_byte = high_byte_test;
    malformed_high_byte.dst = 4;
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(malformed_high_byte));
    EXPECT_TRUE(AnalyzeFastOpExecutionSnapshot(malformed_high_byte).unknown);
    malformed_high_byte = high_byte_test;
    malformed_high_byte.base = 2;
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(malformed_high_byte));
    malformed_high_byte = high_byte_test;
    malformed_high_byte.condition = 1;
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(malformed_high_byte));
    malformed_high_byte = high_byte_test_no_flags;
    malformed_high_byte.dst = 4;
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(malformed_high_byte));
    malformed_high_byte = high_byte_test_no_flags;
    malformed_high_byte.base = 2;
    EXPECT_TRUE(FastOpRequiresExecutionSnapshot(malformed_high_byte));

    FastOp rol32 = Operation(FastOpKind::ShlReg32Imm);
    rol32.dst = 15;
    rol32.immediate = 7;
    rol32.condition =
        Lumen::Cpu::X64Fast::FAST_OP_SHIFT_IMMEDIATE_ROTATE_LEFT;
    const auto rol32_analysis = AnalyzeFastOpExecutionSnapshot(rol32);
    EXPECT_TRUE(!FastOpRequiresExecutionSnapshot(rol32));
    EXPECT_TRUE(rol32_analysis.known && !rol32_analysis.unknown &&
                !rol32_analysis.guest_memory && !rol32_analysis.host_call);

    FastOp rol64 = rol32;
    rol64.kind = FastOpKind::ShlReg64Imm;
    rol64.immediate = 33;
    const auto rol64_analysis = AnalyzeFastOpExecutionSnapshot(rol64);
    EXPECT_TRUE(!FastOpRequiresExecutionSnapshot(rol64));
    EXPECT_TRUE(rol64_analysis.known && !rol64_analysis.unknown &&
                !rol64_analysis.guest_memory && !rol64_analysis.host_call);

    const auto expect_malformed_rotate = [](FastOp operation)
    {
        EXPECT_TRUE(FastOpRequiresExecutionSnapshot(operation));
        const auto analysis = AnalyzeFastOpExecutionSnapshot(operation);
        EXPECT_TRUE(!analysis.known && analysis.unknown &&
                    !analysis.guest_memory && !analysis.host_call);
    };
    FastOp malformed_rotate = rol32;
    malformed_rotate.condition = 0x40;
    expect_malformed_rotate(malformed_rotate);
    malformed_rotate = rol32;
    malformed_rotate.condition |= 1u;
    expect_malformed_rotate(malformed_rotate);
    malformed_rotate = rol32;
    malformed_rotate.dst = 16;
    expect_malformed_rotate(malformed_rotate);
    malformed_rotate = Operation(FastOpKind::ShrReg32Imm);
    malformed_rotate.condition =
        Lumen::Cpu::X64Fast::FAST_OP_SHIFT_IMMEDIATE_ROTATE_LEFT;
    expect_malformed_rotate(malformed_rotate);

    size_t snapshot_kind_count = 0;
    size_t independent_kind_count = 0;
    size_t proven_guest_memory_kind_count = 0;
    size_t unknown_kind_count = 0;
    constexpr auto last_kind = static_cast<uint8_t>(FastOpKind::TestNoFlags);
    EXPECT_TRUE(last_kind + 1 == 255);
    for (uint16_t raw_kind = 0; raw_kind <= last_kind; raw_kind++)
    {
        FastOp operation =
            Operation(static_cast<FastOpKind>(raw_kind));
        if (operation.kind == FastOpKind::CompareNoFlags)
        {
            operation.condition =
                static_cast<uint8_t>(FastOpKind::CmpReg64);
        } else if (operation.kind == FastOpKind::TestNoFlags)
        {
            operation.condition =
                static_cast<uint8_t>(FastOpKind::TestReg64);
        }
        if (FastOpRequiresExecutionSnapshot(operation))
            snapshot_kind_count++;
        else
            independent_kind_count++;
        const auto analysis = AnalyzeFastOpExecutionSnapshot(operation);
        EXPECT_TRUE(analysis.guest_memory ==
                    IsExpectedProvenGuestMemoryKind(operation.kind));
        if (analysis.guest_memory)
            proven_guest_memory_kind_count++;
        else if (analysis.unknown)
            unknown_kind_count++;
    }
    EXPECT_TRUE(snapshot_kind_count == 113);
    EXPECT_TRUE(independent_kind_count == 142);
    EXPECT_TRUE(proven_guest_memory_kind_count == 58);
    EXPECT_TRUE(unknown_kind_count == 55);

    const std::array<FastOp, 2> pure_block = {
        register_only, direct_branch,
    };
    const auto pure_analysis = AnalyzeFastBlockOperationSnapshot(
        pure_block.data(), pure_block.size());
    EXPECT_TRUE(pure_analysis.operation_count == pure_block.size());
    EXPECT_TRUE(!pure_analysis.snapshot_avoidance_candidate);
    EXPECT_TRUE(pure_analysis.known_operation_count == pure_block.size());
    EXPECT_TRUE(pure_analysis.unknown_operation_count == 0);

    const std::array<FastOp, 2> mixed_block = {
        register_only, guest_memory,
    };
    const auto mixed_analysis = AnalyzeFastBlockOperationSnapshot(
        mixed_block.data(), mixed_block.size());
    EXPECT_TRUE(mixed_analysis.snapshot_avoidance_candidate);
    EXPECT_TRUE(mixed_analysis.known_operation_count == 2);
    EXPECT_TRUE(mixed_analysis.guest_memory_operation_count == 1);
    EXPECT_TRUE(mixed_analysis.host_call_operation_count == 0);
    EXPECT_TRUE(mixed_analysis.unknown_operation_count == 0);

    const std::array<FastOp, 2> helper_mixed_block = {
        guest_memory, host_helper,
    };
    const auto helper_mixed_analysis = AnalyzeFastBlockOperationSnapshot(
        helper_mixed_block.data(), helper_mixed_block.size());
    EXPECT_TRUE(!helper_mixed_analysis.snapshot_avoidance_candidate);
    EXPECT_TRUE(helper_mixed_analysis.known_operation_count == 1);
    EXPECT_TRUE(helper_mixed_analysis.guest_memory_operation_count == 1);
    EXPECT_TRUE(helper_mixed_analysis.host_call_operation_count == 0);
    EXPECT_TRUE(helper_mixed_analysis.unknown_operation_count == 1);

    const auto null_analysis = AnalyzeFastBlockOperationSnapshot(nullptr, 1);
    EXPECT_TRUE(!null_analysis.snapshot_avoidance_candidate);
    EXPECT_TRUE(null_analysis.operation_count == 1);
    EXPECT_TRUE(null_analysis.known_operation_count == 0);
    EXPECT_TRUE(null_analysis.unknown_operation_count == 1);

    const auto oversized_analysis = AnalyzeFastBlockOperationSnapshot(
        &register_only, Lumen::Cpu::A64Dynarec::MAX_IR_INSTRUCTIONS + 1);
    EXPECT_TRUE(!oversized_analysis.snapshot_avoidance_candidate);
    EXPECT_TRUE(oversized_analysis.known_operation_count == 0);
    EXPECT_TRUE(oversized_analysis.unknown_operation_count ==
                Lumen::Cpu::A64Dynarec::MAX_IR_INSTRUCTIONS + 1);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d disabled-provider checks failed\n", g_failures);
        return 1;
    }
    std::puts("disabled AArch64 provider snapshot classification: PASS");
    return 0;
}
