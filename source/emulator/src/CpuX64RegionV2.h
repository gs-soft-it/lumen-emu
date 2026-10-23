#ifndef EMULATOR_SRC_CPUX64REGIONV2_H_
#define EMULATOR_SRC_CPUX64REGIONV2_H_

#include "CpuX64FastOps.h"

#include <cstdint>

namespace Lumen::Cpu::X64RegionV2 {

#if defined(_MSC_VER)
#define LUMEN_REGION_V2_ALWAYS_INLINE __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define LUMEN_REGION_V2_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define LUMEN_REGION_V2_ALWAYS_INLINE inline
#endif

// This first proof admits only effect-free 32-bit operations on the four
// scalar registers that the earlier resident-frame experiment could keep in
// AArch64 registers. Unlike that per-block experiment, admission is learned
// once and execution crosses multiple blocks. The frame remains private until
// the complete code envelope has been revalidated.
struct Frame
{
	uint64_t rax = 0;
	uint64_t rcx = 0;
	uint64_t rdx = 0;
	uint64_t rbx = 0;
	uint64_t rip = 0;
	uint64_t rflags = 0;
	uint16_t dirty_gprs = 0;
	bool rflags_dirty = false;
};

enum class OpResult : uint8_t
{
	Continue,
	ControlTransfer,
	Unsupported,
};

constexpr uint64_t FLAG_CF = UINT64_C(1) << 0u;
constexpr uint64_t FLAG_PF = UINT64_C(1) << 2u;
constexpr uint64_t FLAG_AF = UINT64_C(1) << 4u;
constexpr uint64_t FLAG_ZF = UINT64_C(1) << 6u;
constexpr uint64_t FLAG_SF = UINT64_C(1) << 7u;
constexpr uint64_t FLAG_OF = UINT64_C(1) << 11u;

LUMEN_REGION_V2_ALWAYS_INLINE uint64_t ReadRegister(
	const Frame& frame, uint8_t reg)
{
	switch (reg)
	{
		case 0: return frame.rax;
		case 1: return frame.rcx;
		case 2: return frame.rdx;
		case 3: return frame.rbx;
		default: return 0;
	}
}

LUMEN_REGION_V2_ALWAYS_INLINE bool WriteRegister32(
	Frame& frame, uint8_t reg, uint32_t value)
{
	switch (reg)
	{
		case 0: frame.rax = value; break;
		case 1: frame.rcx = value; break;
		case 2: frame.rdx = value; break;
		case 3: frame.rbx = value; break;
		default: return false;
	}
	frame.dirty_gprs |= static_cast<uint16_t>(1u << reg);
	return true;
}

LUMEN_REGION_V2_ALWAYS_INLINE constexpr uint64_t ResultFlags32(
	uint32_t value)
{
	const uint64_t parity =
	    (UINT64_C(0x9669) >> (((value >> 4u) ^ value) & 0x0fu)) & 1u;
	return (value == 0 ? FLAG_ZF : 0) |
	       ((value & UINT32_C(0x80000000)) != 0 ? FLAG_SF : 0) |
	       (parity != 0 ? FLAG_PF : 0);
}

LUMEN_REGION_V2_ALWAYS_INLINE uint32_t AddWithFlags32(
	Frame& frame, uint32_t lhs, uint32_t rhs)
{
	const uint64_t wide = static_cast<uint64_t>(lhs) + rhs;
	const uint32_t result = static_cast<uint32_t>(wide);
	const uint64_t changed =
	    FLAG_ZF | FLAG_SF | FLAG_PF | FLAG_CF | FLAG_AF | FLAG_OF;
	const uint64_t flags =
	    ResultFlags32(result) |
	    (wide > UINT32_MAX ? FLAG_CF : 0) |
	    (((lhs ^ rhs ^ result) & UINT32_C(0x10)) != 0 ? FLAG_AF : 0) |
	    (((~(lhs ^ rhs)) & (lhs ^ result) & UINT32_C(0x80000000)) != 0
	         ? FLAG_OF
	         : 0);
	frame.rflags = (frame.rflags & ~changed) | flags;
	frame.rflags_dirty = true;
	return result;
}

LUMEN_REGION_V2_ALWAYS_INLINE void SetCompareFlags32(
	Frame& frame, uint32_t lhs, uint32_t rhs)
{
	const uint32_t result = lhs - rhs;
	const uint64_t changed =
	    FLAG_ZF | FLAG_SF | FLAG_PF | FLAG_CF | FLAG_AF | FLAG_OF;
	const uint64_t flags =
	    ResultFlags32(result) | (lhs < rhs ? FLAG_CF : 0) |
	    (((lhs ^ rhs ^ result) & UINT32_C(0x10)) != 0 ? FLAG_AF : 0) |
	    (((lhs ^ rhs) & (lhs ^ result) & UINT32_C(0x80000000)) != 0
	         ? FLAG_OF
	         : 0);
	frame.rflags = (frame.rflags & ~changed) | flags;
	frame.rflags_dirty = true;
}

LUMEN_REGION_V2_ALWAYS_INLINE constexpr bool Condition(
	uint8_t condition, uint64_t flags)
{
	if (condition > 0x0f) return false;
	const uint32_t cf = (flags & FLAG_CF) != 0;
	const uint32_t pf = (flags & FLAG_PF) != 0;
	const uint32_t zf = (flags & FLAG_ZF) != 0;
	const uint32_t sf = (flags & FLAG_SF) != 0;
	const uint32_t of = (flags & FLAG_OF) != 0;
	const uint32_t sl = sf ^ of;
	const uint32_t predicates =
	    of | (cf << 1u) | (zf << 2u) | ((cf | zf) << 3u) |
	    (sf << 4u) | (pf << 5u) | (sl << 6u) | ((zf | sl) << 7u);
	return ((((predicates >> (condition >> 1u)) ^ condition) & 1u) != 0);
}

// Keep the proof deliberately smaller than the previous 37-kind resident
// scalar experiment. Every admitted kind is covered by the differential gate;
// expanding this list must expand that gate first.
constexpr bool IsSupportedKind(X64Fast::FastOpKind kind)
{
	using X64Fast::FastOpKind;
	switch (kind)
	{
		case FastOpKind::Nop:
		case FastOpKind::MovImmReg32:
		case FastOpKind::AddReg32Imm:
		case FastOpKind::CmpReg32Imm:
		case FastOpKind::Jcc:
		case FastOpKind::Jmp: return true;
		default: return false;
	}
}

LUMEN_REGION_V2_ALWAYS_INLINE OpResult Execute(
	Frame& frame, const X64Fast::FastOp& op)
{
	using X64Fast::FastOpKind;
	switch (op.kind)
	{
		case FastOpKind::Nop: break;
		case FastOpKind::MovImmReg32:
			if (!WriteRegister32(
			        frame, op.dst, static_cast<uint32_t>(op.immediate)))
				return OpResult::Unsupported;
			break;
		case FastOpKind::AddReg32Imm:
			if (op.dst >= 4 ||
			    !WriteRegister32(
			        frame, op.dst,
			        AddWithFlags32(
			            frame, static_cast<uint32_t>(ReadRegister(frame, op.dst)),
			            static_cast<uint32_t>(op.immediate))))
			{
				return OpResult::Unsupported;
			}
			break;
		case FastOpKind::CmpReg32Imm:
			if (op.dst >= 4) return OpResult::Unsupported;
			SetCompareFlags32(
			    frame, static_cast<uint32_t>(ReadRegister(frame, op.dst)),
			    static_cast<uint32_t>(op.immediate));
			break;
		case FastOpKind::Jcc:
			frame.rip = Condition(op.condition, frame.rflags)
			                ? op.target
			                : op.rip + op.length;
			return OpResult::ControlTransfer;
		case FastOpKind::Jmp:
			frame.rip = op.target;
			return OpResult::ControlTransfer;
		default: return OpResult::Unsupported;
	}
	return OpResult::Continue;
}

} // namespace Lumen::Cpu::X64RegionV2

#undef LUMEN_REGION_V2_ALWAYS_INLINE

#endif
