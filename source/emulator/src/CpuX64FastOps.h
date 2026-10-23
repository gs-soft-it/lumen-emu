#ifndef EMULATOR_SRC_CPUX64FASTOPS_H_
#define EMULATOR_SRC_CPUX64FASTOPS_H_

#include <cstdint>
#include <type_traits>

#ifndef LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK
#define LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK 0
#endif

#ifndef LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK
#define LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK 0
#endif

static_assert(LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK == 0 ||
                  LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK == 1,
              "LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK must be 0 or 1");
static_assert(LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK == 0 ||
                  LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK == 1,
              "LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK must be 0 or 1");

// These two FastOp families can be withheld from the ordinary C++ tier and
// routed through the native-only admission path. Android enables that routing
// only in the compile-time selective-NODA prototype; probe-only APKs retain
// the historical ordinary FastBlock behavior even if their environment is
// modified at runtime.
#if (defined(__APPLE__) && defined(__aarch64__)) || \
    (defined(__ANDROID__) && defined(__aarch64__) && \
     defined(LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA))
#define LUMEN_X64_A64_SELECTIVE_NATIVE_FASTOPS 1
#else
#define LUMEN_X64_A64_SELECTIVE_NATIVE_FASTOPS 0
#endif

namespace Lumen::Cpu::X64Fast {

#if LUMEN_X64_A64_SELECTIVE_NATIVE_FASTOPS
constexpr bool VMOVD_ORDINARY_FAST_BLOCK_ENABLED =
    LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK != 0;
constexpr bool VECTOR_PERMUTE_ORDINARY_FAST_BLOCK_ENABLED =
    LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK != 0;
#else
constexpr bool VMOVD_ORDINARY_FAST_BLOCK_ENABLED = true;
constexpr bool VECTOR_PERMUTE_ORDINARY_FAST_BLOCK_ENABLED = true;
#endif

// This is the private decoder/executor hand-off. Keep the ordinal values and
// layout stable: FastBlock stores these inline in a large TLS cache.
enum class FastOpKind: uint8_t
{
	Nop,
	PushReg64,
	PopReg64,
	MovReg16,
	MovReg32,
	MovReg64,
	MovReg8,
	MovImmReg8,
	MovImmReg16,
	MovReg8Mem,
	StoreReg8,
	LeaReg32,
	LeaReg64,
	MovImmReg32,
	MovImmReg64,
	MovReg16Mem,
	MovReg32Mem,
	MovReg64Mem,
	MovsxdReg64Reg32,
	MovsxdReg64Mem32,
	BswapReg32,
	BswapReg64,
	MovImmMem8,
	MovImmMem16,
	MovImmMem32,
	MovImmMem64,
	AddReg8Imm,
	AddReg32Imm,
	AddReg64Imm,
	SubReg32Imm,
	SubReg64Imm,
	ShrReg8Imm,
	ShlReg32Imm,
	ShlReg64Imm,
	ShlReg32Cl,
	ShlReg64Cl,
	ShrReg32Cl,
	ShrReg64Cl,
	ShrReg32Imm,
	ShrReg64Imm,
	SarReg32Imm,
	SarReg64Imm,
	AndReg8Imm,
	OrReg8Imm,
	AndMem8Imm,
	AndReg16Imm,
	AndReg32Imm,
	AndReg64Imm,
	OrReg32Imm,
	OrReg64Imm,
	XorReg32Imm,
	XorReg64Imm,
	AndReg16,
	AndReg32,
	AndReg64,
	OrReg16Mem,
	AndReg8Mem,
	AndReg32Mem,
	AndReg64Mem,
	AndMem32Reg,
	AndMem64Reg,
	XorReg32Mem,
	XorReg64Mem,
	BextrReg32,
	AndnReg32,
	AndnReg64,
	ImulReg32Imm,
	ImulReg64Imm,
	ImulReg32MemImm,
	ImulReg64MemImm,
	ImulReg32,
	ImulReg64,
	ImulReg32Mem,
	ImulReg64Mem,
	MovzxReg32Mem8,
	MovzxReg64Mem8,
	MovzxReg32Mem16,
	MovzxReg64Mem16,
	MovzxReg32Reg8,
	MovzxReg32Reg16,
	MovsxReg32Reg16,
	MovsxReg32Reg8,
	MovsxReg32Mem8,
	MovsxReg32Mem16,
	MovsxReg64Reg16,
	MovsxReg64Reg8,
	MovsxReg64Mem8,
	MovsxReg64Mem16,
	BtReg32,
	BtReg64,
	BtReg32Imm,
	BtReg64Imm,
	SetbReg8,
	IncReg32,
	IncReg64,
	IncMem32,
	IncMem64,
	DecReg32,
	DecReg64,
	NegReg16,
	NotReg32,
	NotReg64,
	NegReg32,
	NegReg64,
	AddReg32,
	AddReg64,
	AddMem32Reg,
	AddMem64Reg,
	AddReg32Mem,
	AddReg64Mem,
	SubMem32Reg,
	SubMem64Reg,
	SubReg32,
	SubReg64,
	SubReg32Mem,
	SubReg64Mem,
	SbbReg8,
	SbbReg16,
	Vcvttss2siReg32Mem,
	Vcvttss2siReg32Reg,
	Vcvttss2siReg64Mem,
	Vcvttss2siReg64Reg,
	Vcvtsi2ssReg32Mem,
	Vcvtsi2ssReg32Reg,
	Vcvtsi2ssReg64Mem,
	Vcvtsi2ssReg64Reg,
	VmovssMem,
	VmovssReg,
	VmovssStoreMem,
	VmovsdMem,
	VmovsdReg,
	VmovsdStoreMem,
	VmovPackedMem,
	VmovPackedReg,
	VmovPackedStoreMem,
	VxorPackedReg,
	VaddssMem,
	VaddssReg,
	VmulssMem,
	VmulssReg,
	VsubssMem,
	VsubssReg,
	VdivssMem,
	VdivssReg,
	VaddsdMem,
	VaddsdReg,
	VmulsdMem,
	VmulsdReg,
	VsubsdMem,
	VsubsdReg,
	VdivsdMem,
	VdivsdReg,
	VaddpsMem,
	VaddpsReg,
	VmulpsMem,
	VmulpsReg,
	VsubpsMem,
	VsubpsReg,
	VsubpdMem128,
	VhaddpdReg128,
	VpunpckldqMem128,
	VmovqReg64ToXmm,
	VucomissMem,
	VucomissReg,
	VucomisdMem,
	VucomisdReg,
	CmpReg32,
	CmpReg64,
	CmpReg32Mem,
	CmpReg64Mem,
	CmpMem32Reg,
	CmpMem64Reg,
	LockCmpxchgMem64Reg,
	CmpReg16Imm,
	CmpReg32Imm,
	CmpReg64Imm,
	CmpMem8Imm,
	CmpMem16Imm,
	CmpMem32Imm,
	CmpMem64Imm,
	TestReg16,
	TestReg8,
	TestReg32,
	TestReg64,
	TestReg8Imm,
	TestReg16Imm,
	TestReg32Imm,
	TestReg64Imm,
	TestMem8Imm,
	OrReg32,
	OrReg64,
	OrReg8,
	OrReg8Mem,
	XorReg32,
	XorReg64,
	SetccReg8,
	SetccMem8,
	CmovccReg32,
	CmovccReg64,
	Jcc,
	Jmp,
	Call,
	CallReg64,
	CallMem64,
	JmpReg64,
	JmpMem64,
	StoreReg16,
	StoreReg32,
	StoreReg64,
	Cld,
	RepMovs8,
	RepMovs64,
	RepStos8,
	RepStos64,
	Ret,
	VxorPackedMem,
	VmovdReg32ToXmm,
	VectorPermuteDwordsImmediate,
	AddReg8ImmNoFlags,
	AddReg32ImmNoFlags,
	AddReg64ImmNoFlags,
	SubReg32ImmNoFlags,
	SubReg64ImmNoFlags,
	AndReg8ImmNoFlags,
	OrReg8ImmNoFlags,
	AndReg16ImmNoFlags,
	AndReg32ImmNoFlags,
	AndReg64ImmNoFlags,
	OrReg32ImmNoFlags,
	OrReg64ImmNoFlags,
	XorReg32ImmNoFlags,
	XorReg64ImmNoFlags,
	AndReg16NoFlags,
	AndReg32NoFlags,
	AndReg64NoFlags,
	NegReg16NoFlags,
	NegReg32NoFlags,
	NegReg64NoFlags,
	AddReg32NoFlags,
	AddReg64NoFlags,
	SubReg32NoFlags,
	SubReg64NoFlags,
	OrReg32NoFlags,
	OrReg64NoFlags,
	OrReg8NoFlags,
	XorReg32NoFlags,
	XorReg64NoFlags,
	SbbReg8NoFlags,
	SbbReg16NoFlags,
	IncReg32NoFlags,
	IncReg64NoFlags,
	DecReg32NoFlags,
	DecReg64NoFlags,
	CompareNoFlags,
	TestNoFlags,
};

constexpr FastOpKind NormalizeDeadRflagsFastOpKind(FastOpKind kind)
{
	switch (kind)
	{
		case FastOpKind::AddReg8ImmNoFlags: return FastOpKind::AddReg8Imm;
		case FastOpKind::AddReg32ImmNoFlags: return FastOpKind::AddReg32Imm;
		case FastOpKind::AddReg64ImmNoFlags: return FastOpKind::AddReg64Imm;
		case FastOpKind::SubReg32ImmNoFlags: return FastOpKind::SubReg32Imm;
		case FastOpKind::SubReg64ImmNoFlags: return FastOpKind::SubReg64Imm;
		case FastOpKind::AndReg8ImmNoFlags: return FastOpKind::AndReg8Imm;
		case FastOpKind::OrReg8ImmNoFlags: return FastOpKind::OrReg8Imm;
		case FastOpKind::AndReg16ImmNoFlags: return FastOpKind::AndReg16Imm;
		case FastOpKind::AndReg32ImmNoFlags: return FastOpKind::AndReg32Imm;
		case FastOpKind::AndReg64ImmNoFlags: return FastOpKind::AndReg64Imm;
		case FastOpKind::OrReg32ImmNoFlags: return FastOpKind::OrReg32Imm;
		case FastOpKind::OrReg64ImmNoFlags: return FastOpKind::OrReg64Imm;
		case FastOpKind::XorReg32ImmNoFlags: return FastOpKind::XorReg32Imm;
		case FastOpKind::XorReg64ImmNoFlags: return FastOpKind::XorReg64Imm;
		case FastOpKind::AndReg16NoFlags: return FastOpKind::AndReg16;
		case FastOpKind::AndReg32NoFlags: return FastOpKind::AndReg32;
		case FastOpKind::AndReg64NoFlags: return FastOpKind::AndReg64;
		case FastOpKind::NegReg16NoFlags: return FastOpKind::NegReg16;
		case FastOpKind::NegReg32NoFlags: return FastOpKind::NegReg32;
		case FastOpKind::NegReg64NoFlags: return FastOpKind::NegReg64;
		case FastOpKind::AddReg32NoFlags: return FastOpKind::AddReg32;
		case FastOpKind::AddReg64NoFlags: return FastOpKind::AddReg64;
		case FastOpKind::SubReg32NoFlags: return FastOpKind::SubReg32;
		case FastOpKind::SubReg64NoFlags: return FastOpKind::SubReg64;
		case FastOpKind::OrReg32NoFlags: return FastOpKind::OrReg32;
		case FastOpKind::OrReg64NoFlags: return FastOpKind::OrReg64;
		case FastOpKind::OrReg8NoFlags: return FastOpKind::OrReg8;
		case FastOpKind::XorReg32NoFlags: return FastOpKind::XorReg32;
		case FastOpKind::XorReg64NoFlags: return FastOpKind::XorReg64;
		case FastOpKind::SbbReg8NoFlags: return FastOpKind::SbbReg8;
		case FastOpKind::SbbReg16NoFlags: return FastOpKind::SbbReg16;
		case FastOpKind::IncReg32NoFlags: return FastOpKind::IncReg32;
		case FastOpKind::IncReg64NoFlags: return FastOpKind::IncReg64;
		case FastOpKind::DecReg32NoFlags: return FastOpKind::DecReg32;
		case FastOpKind::DecReg64NoFlags: return FastOpKind::DecReg64;
		default: return kind;
	}
}

constexpr bool IsDeadRflagsElidedFastOpKind(FastOpKind kind)
{
	return NormalizeDeadRflagsFastOpKind(kind) != kind ||
	       kind == FastOpKind::CompareNoFlags ||
	       kind == FastOpKind::TestNoFlags;
}

// SIMD-specific flags carried in FastOp::condition. These bits are shared by
// the precise FastBlock executor and the AArch64 IR lowering contract.
constexpr uint8_t FAST_OP_VECTOR_ZERO_UPPER       = 1u << 0u;
constexpr uint8_t FAST_OP_VECTOR_REQUIRE_ALIGNMENT = 1u << 1u;

// TEST r8, imm8 canonicalizes AH/CH/DH/BH to their underlying GPR so register
// analyses see RAX/RCX/RDX/RBX. FastOp::base is otherwise unused by this kind
// and carries whether the selected byte is bits 8..15 instead of bits 0..7.
constexpr uint8_t FAST_OP_TEST_REG8_IMM_HIGH_BYTE = 1u << 0u;

// ShlReg32Imm/ShlReg64Imm carry ROL semantics in FastOp::condition. No other
// shift kind accepts this marker.
constexpr uint8_t FAST_OP_SHIFT_IMMEDIATE_ROTATE_LEFT = 1u << 7u;

struct FastOp
{
	FastOpKind kind;
	uint8_t dst;
	uint8_t src;
	uint8_t base;
	uint8_t index;
	uint8_t scale;
	uint8_t length;
	uint8_t condition;
	int32_t immediate;
	int32_t displacement;
	uint64_t rip;
	uint64_t target;
};

static_assert(sizeof(FastOp) == 32, "FastOp is embedded in the TLS FastBlock cache");
static_assert(static_cast<uint8_t>(FastOpKind::Nop) == 0, "FastOp ordinal contract changed");
static_assert(static_cast<uint8_t>(FastOpKind::Ret) == 214, "FastOp ordinal contract changed");
static_assert(static_cast<uint8_t>(FastOpKind::VxorPackedMem) == 215, "FastOp append-only ordinal contract changed");
static_assert(static_cast<uint8_t>(FastOpKind::VmovdReg32ToXmm) == 216,
              "FastOp append-only ordinal contract changed");
static_assert(static_cast<uint8_t>(FastOpKind::VectorPermuteDwordsImmediate) == 217,
              "FastOp append-only ordinal contract changed");
static_assert(static_cast<uint8_t>(FastOpKind::AddReg8ImmNoFlags) == 218,
              "FastOp dead-RFLAGS append-only ordinal contract changed");
static_assert(static_cast<uint8_t>(FastOpKind::DecReg64NoFlags) == 252,
              "FastOp dead-RFLAGS append-only ordinal contract changed");
static_assert(static_cast<uint8_t>(FastOpKind::TestNoFlags) == 254,
              "FastOp dead-RFLAGS terminal ordinal contract changed");
static_assert(std::is_standard_layout<FastOp>::value, "FastOp must remain a plain decoder record");
static_assert(std::is_trivially_default_constructible<FastOp>::value,
              "FastOp snapshots rely on uninitialized trivial storage");
static_assert(std::is_trivially_copyable<FastOp>::value, "FastOp must remain safely copyable into IR input snapshots");

} // namespace Lumen::Cpu::X64Fast

#endif
