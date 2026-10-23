#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/String8.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderSourceSnapshotCache.h"

#include <algorithm>
#include <memory>
#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

// GPU parser admission explicitly publishes native producers before reading
// shader bytes, including during an exclusive epoch's Core handoff bypass.
bool ShaderPrepareMemoryRead(uint64_t address, uint64_t bytes) noexcept;

// The bounded decoder's short tail is an explicit private copy, including
// padding that is checked against the original extent before accepting its AST.
class ScopedShaderPrivateRead
{
public:
	ScopedShaderPrivateRead(const uint32_t* words, uint64_t bytes);
	~ScopedShaderPrivateRead();
	LUMEN_CLASS_NO_COPY(ScopedShaderPrivateRead);
private:
	friend bool ShaderPrepareMemoryRead(uint64_t address, uint64_t bytes) noexcept;
	uint64_t m_address = 0, m_bytes = 0;
	const ScopedShaderPrivateRead* m_previous = nullptr;
};

// A single draw's checked, owned VS bytes, including usage slots and binary
// metadata. No mapping pin survives Capture(). The scope makes every VS
// consumer in that synchronous draw use the same bytes, including its ID and
// compiler; it never authorizes an unrelated address or resource descriptor.
class ShaderVertexSourceSnapshot
{
public:
	ShaderVertexSourceSnapshot() = default;
	LUMEN_CLASS_NO_COPY(ShaderVertexSourceSnapshot);
	bool Capture(uint64_t address, bool* retryable);
	uint64_t Address() const { return m_address; }
	const uint32_t* Data() const { return m_words.Data(); }
	const ShaderSourceWords& OwnedWords() const { return m_words; }
	size_t Bytes() const { return m_bytes; }
	uint32_t CodeWords() const { return m_code_words; }
private:
	friend class ShaderOwnedDrawInputs;
	uint64_t m_address = 0;
	uint32_t m_code_words = 0;
	size_t m_bytes = 0;
	ShaderSourceWords m_words;
};

class ScopedShaderVertexSource
{
public:
	explicit ScopedShaderVertexSource(const ShaderVertexSourceSnapshot* source);
	~ScopedShaderVertexSource();
	LUMEN_CLASS_NO_COPY(ScopedShaderVertexSource);
private:
	const ShaderVertexSourceSnapshot* m_source;
	const ShaderVertexSourceSnapshot* m_previous;
};

namespace HW {
struct VertexShaderInfo;
struct PixelShaderInfo;
struct ComputeShaderInfo;
struct ShaderRegisters;
} // namespace HW

enum class ShaderType
{
	Unknown,
	Vertex,
	Pixel,
	Fetch,
	Compute
};

enum class ShaderDx10ClampMode
{
	Enabled,
	Disabled
};

enum class ShaderFp32Mode
{
	Unvalidated,
	RneFlushInOut
};

enum class ShaderThreadTraceMode
{
	Unknown,
	Disabled,
	Enabled
};

enum class ShaderInstructionType : uint32_t
{
	Unknown,

	BufferLoadDword,
	BufferLoadFormatX,
	BufferLoadFormatXy,
	BufferLoadFormatXyz,
	BufferLoadFormatXyzw,
	BufferStoreDword,
	BufferStoreFormatX,
	BufferStoreFormatXy,
	DsAppend,
	DsConsume,
	Exp,
	ImageGetResinfo,
	ImageLoad,
	ImageLoadMip,
	ImageSample,
	ImageSampleLz,
	ImageSampleLzO,
	ImageStore,
	ImageStoreMip,
	SAddcU32,
	SAddI32,
	SAddU32,
	SAndB32,
	SAndB64,
	SAndn2B64,
	SAndSaveexecB64,
	SBfeU32,
	SBfeU64,
	SBfmB32,
	SBranch,
	SBufferLoadDword,
	SBufferLoadDwordx16,
	SBufferLoadDwordx2,
	SBufferLoadDwordx4,
	SBufferLoadDwordx8,
	SCbranchExecz,
	SCbranchScc0,
	SCbranchScc1,
	SCbranchVccz,
	SCbranchVccnz,
	SCmpEqI32,
	SCmpEqU32,
	SCmpGeI32,
	SCmpGeU32,
	SCmpGtI32,
	SCmpGtU32,
	SCmpLeI32,
	SCmpLeU32,
	SCmpLgI32,
	SCmpLgU32,
	SCmpLtI32,
	SCmpLtU32,
	SCselectB32,
	SCselectB64,
	SEndpgm,
	SInstPrefetch,
	SLoadDword,
	SLoadDwordx2,
	SLoadDwordx4,
	SLoadDwordx8,
	SLoadDwordx16,
	SLshlB32,
	SLshrB32,
	SLshlB64,
	SLshrB64,
	SMovB32,
	SMovB64,
	SMovkI32,
	SMulHiU32,
	SMulI32,
	SMulkI32,
	SNandB64,
	SNotB32,
	SNorB64,
	SOrB32,
	SOrB64,
	SOrn2B64,
	SSendmsg,
	SSetpcB64,
	SSwappcB64,
	SSubI32,
	SWaitcnt,
	SWqmB64,
	SXnorB64,
	SXorB64,
	TBufferLoadFormatX,
	TBufferLoadFormatXyzw,
	VAddF32,
	VAddI32,
	VAndB32,
	VAshrI32,
	VAshrrevI32,
	VBcntU32B32,
	VBfeU32,
	VBfmB32,
	VBfrevB32,
	VCeilF32,
	VCmpEqF32,
	VCmpEqI32,
	VCmpEqU32,
	VCmpFF32,
	VCmpFI32,
	VCmpFU32,
	VCmpGeF32,
	VCmpGeI32,
	VCmpGeU32,
	VCmpGtF32,
	VCmpGtI32,
	VCmpGtU32,
	VCmpLeF32,
	VCmpLeI32,
	VCmpLeU32,
	VCmpLgF32,
	VCmpLtF32,
	VCmpLtI32,
	VCmpLtU32,
	VCmpNeI32,
	VCmpNeqF32,
	VCmpNeU32,
	VCmpNgeF32,
	VCmpNgtF32,
	VCmpNleF32,
	VCmpNlgF32,
	VCmpNltF32,
	VCmpOF32,
	VCmpTI32,
	VCmpTruF32,
	VCmpTU32,
	VCmpUF32,
	VCmpxEqU32,
	VCmpxGeI32,
	VCmpxGeU32,
	VCmpxGtF32,
	VCmpxGtU32,
	VCmpxLtF32,
	VCmpxNeqF32,
	VCmpxNeU32,
	VCndmaskB32,
	VCosF32,
	VCvtF32F16,
	VCvtF32I32,
	VCvtOffF32I4,
	VCvtI32F32,
	VCvtF32U32,
	VCvtF32Ubyte0,
	VCvtF32Ubyte1,
	VCvtF32Ubyte2,
	VCvtF32Ubyte3,
	VCvtPkrtzF16F32,
	VCvtU32F32,
	VExpF32,
	VFloorF32,
	VFmaF32,
	VFractF32,
	VInterpMovF32,
	VInterpP1F32,
	VInterpP2F32,
	VLogF32,
	VLshlB32,
	VLshlrevB32,
	VLshrB32,
	VLshrrevB32,
	VMacF32,
	VMadakF32,
	VMadF32,
	VMadmkF32,
	VMadU32U24,
	VMax3F32,
	VMaxF32,
	VMbcntHiU32B32,
	VMbcntLoU32B32,
	VMed3F32,
	VMin3F32,
	VMinF32,
	VMovB32,
	VMulF32,
	VMulHiU32,
	VMulLoI32,
	VMulLoU32,
	VMulU32U24,
	VNotB32,
	VOrB32,
	VRcpF32,
	VRndneF32,
	VRsqF32,
	VSadU32,
	VSinF32,
	VSqrtF32,
	VSubF32,
	VSubI32,
	VSubrevF32,
	VSubrevI32,
	VTruncF32,
	VXorB32,

	VMulLegacyF32,
	VMinU32,
	VMacLegacyF32,
	VRsqClampF32,
	STTraceData,
	ZMax
};

namespace ShaderInstructionFormat {

enum FormatByte : uint64_t
{
	U = 0,
	N,
	D,      // operand_to_str(inst.dst)
	D2,     // operand_to_str(inst.dst2)
	S0,     // operand_to_str(inst.src[0])
	S1,     // operand_to_str(inst.src[1])
	S2,     // operand_to_str(inst.src[2])
	S3,     // operand_to_str(inst.src[3])
	DA2,    // operand_array_to_str(inst.dst, 2)
	DA3,    // operand_array_to_str(inst.dst, 3)
	DA4,    // operand_array_to_str(inst.dst, 4)
	DA8,    // operand_array_to_str(inst.dst, 8)
	DA16,   // operand_array_to_str(inst.dst, 16)
	D2A2,   // operand_array_to_str(inst.dst2, 2)
	D2A3,   // operand_array_to_str(inst.dst2, 3)
	D2A4,   // operand_array_to_str(inst.dst2, 4)
	S0A2,   // operand_array_to_str(inst.src[0], 2)
	S0A3,   // operand_array_to_str(inst.src[0], 3)
	S0A4,   // operand_array_to_str(inst.src[0], 4)
	S1A2,   // operand_array_to_str(inst.src[1], 2)
	S1A3,   // operand_array_to_str(inst.src[1], 3)
	S1A4,   // operand_array_to_str(inst.src[1], 4)
	S1A8,   // operand_array_to_str(inst.src[1], 8)
	S2A2,   // operand_array_to_str(inst.src[2], 2)
	S2A3,   // operand_array_to_str(inst.src[2], 3)
	S2A4,   // operand_array_to_str(inst.src[2], 4)
	Attr,   // attr%u.%u <- inst.src[1].constant.u, inst.src[2].constant.u
	Idxen,  // idxen
	Offen,  // offen
	Float1, // format:float1
	Float4, // format:float4
	Pos0,   // pos0
	Done,   // done
	Param0, // param0
	Param1, // param1
	Param2, // param2
	Param3, // param3
	Param4, // param4
	Mrt0,   // mrt_color0
	Off,    // off
	Compr,  // compr
	Vm,     // vm
	L,      // label_%u
	DmaskF, // dmask:0xf
	Dmask7, // dmask:0x7
	Dmask1, // dmask:0x1
	Dmask2, // dmask:0x2
	Dmask8, // dmask:0x8
	Dmask3, // dmask:0x3
	Dmask5, // dmask:0x5
	Dmask9, // dmask:0x9
	Gds,    // gds
	Param,  // param%u <- inst.export_target - 0x20; appended to preserve legacy atoms
	Dmask4, // dmask:0x4; appended to preserve legacy atoms
};

constexpr uint64_t FormatDefine(std::initializer_list<uint64_t> f)
{
	uint64_t r = 0;
	for (auto n: f)
	{
		r = (r << 8u) | n;
	}
	return r;
}

enum Format : uint64_t
{
	Unknown                             = FormatDefine({U}),
	Empty                               = FormatDefine({N}),
	Imm                                 = FormatDefine({S0}),
	Label                               = FormatDefine({L}),
	Mrt0OffOffComprVmDone               = FormatDefine({Mrt0, Off, Off, Compr, Vm, Done}),
	Mrt0Vsrc0OffComprVmDone             = FormatDefine({Mrt0, S0, Off, Compr, Vm, Done}),
	Mrt0Vsrc0Vsrc1ComprVmDone           = FormatDefine({Mrt0, S0, S1, Compr, Vm, Done}),
	Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone      = FormatDefine({Mrt0, S0, S1, S2, S3, Vm, Done}),
	Param0Vsrc0Vsrc1Vsrc2Vsrc3          = FormatDefine({Param0, S0, S1, S2, S3}),
	Param1Vsrc0Vsrc1Vsrc2Vsrc3          = FormatDefine({Param1, S0, S1, S2, S3}),
	Param2Vsrc0Vsrc1Vsrc2Vsrc3          = FormatDefine({Param2, S0, S1, S2, S3}),
	Param3Vsrc0Vsrc1Vsrc2Vsrc3          = FormatDefine({Param3, S0, S1, S2, S3}),
	Param4Vsrc0Vsrc1Vsrc2Vsrc3          = FormatDefine({Param4, S0, S1, S2, S3}),
	Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done        = FormatDefine({Pos0, S0, S1, S2, S3, Done}),
	Saddr                               = FormatDefine({S0A2}),
	SdstSbaseSoffset                    = FormatDefine({D, S0A2, S1}),
	Sdst16SvSoffset                     = FormatDefine({DA16, S0A4, S1}),
	Sdst2Ssrc02                         = FormatDefine({DA2, S0A2}),
	Sdst2Ssrc02Ssrc1                    = FormatDefine({DA2, S0A2, S1}),
	Sdst2Ssrc02Ssrc12                   = FormatDefine({DA2, S0A2, S1A2}),
	Sdst2SvSoffset                      = FormatDefine({DA2, S0A4, S1}),
	Sdst4SbaseSoffset                   = FormatDefine({DA4, S0A2, S1}),
	Sdst4SvSoffset                      = FormatDefine({DA4, S0A4, S1}),
	Sdst8SbaseSoffset                   = FormatDefine({DA8, S0A2, S1}),
	Sdst8SvSoffset                      = FormatDefine({DA8, S0A4, S1}),
	SdstSvSoffset                       = FormatDefine({D, S0A4, S1}),
	SmaskVsrc0Vsrc1                     = FormatDefine({DA2, S0, S1}),
	Ssrc0Ssrc1                          = FormatDefine({S0, S1}),
	SVdstSVsrc0                         = FormatDefine({D, S0}),
	SVdstSVsrc0SVsrc1                   = FormatDefine({D, S0, S1}),
	Vdata1Vaddr3StSsDmask1              = FormatDefine({D, S0A3, S1A8, S2A4, Dmask1}),
	Vdata1Vaddr3StSsDmask2              = FormatDefine({D, S0A3, S1A8, S2A4, Dmask2}),
	Vdata1Vaddr3StSsDmask4              = FormatDefine({D, S0A3, S1A8, S2A4, Dmask4}),
	Vdata1Vaddr3StSsDmask8              = FormatDefine({D, S0A3, S1A8, S2A4, Dmask8}),
	Vdata1VaddrSvSoffsIdxen             = FormatDefine({D, S0, S1A4, S2, Idxen}),
	Vdata1VaddrSvSoffsIdxenFloat1       = FormatDefine({D, S0, S1A4, S2, Idxen, Float1}),
	Vdata2Vaddr3StSsDmask3              = FormatDefine({DA2, S0A3, S1A8, S2A4, Dmask3}),
	Vdata2Vaddr3StSsDmask5              = FormatDefine({DA2, S0A3, S1A8, S2A4, Dmask5}),
	Vdata2Vaddr3StSsDmask9              = FormatDefine({DA2, S0A3, S1A8, S2A4, Dmask9}),
	Vdata2VaddrSvSoffsIdxen             = FormatDefine({DA2, S0, S1A4, S2, Idxen}),
	Vdata3Vaddr3StSsDmask7              = FormatDefine({DA3, S0A3, S1A8, S2A4, Dmask7}),
	Vdata3Vaddr4StSsDmask7              = FormatDefine({DA3, S0A4, S1A8, S2A4, Dmask7}),
	Vdata3VaddrSvSoffsIdxen             = FormatDefine({DA3, S0, S1A4, S2, Idxen}),
	Vdata4Vaddr2SvSoffsOffenIdxenFloat4 = FormatDefine({DA4, S0A2, S1A4, S2, Offen, Idxen, Float4}),
	Vdata1Vaddr1StDmask1                = FormatDefine({D, S0, S1A8, Dmask1}),
	Vdata1Vaddr1StDmask2                = FormatDefine({D, S0, S1A8, Dmask2}),
	Vdata2Vaddr1StDmask3                = FormatDefine({DA2, S0, S1A8, Dmask3}),
	Vdata3Vaddr1StDmask7                = FormatDefine({DA3, S0, S1A8, Dmask7}),
	Vdata1Vaddr4StDmask1                = FormatDefine({D, S0A4, S1A8, Dmask1}),
	Vdata1Vaddr4StDmask2                = FormatDefine({D, S0A4, S1A8, Dmask2}),
	Vdata1Vaddr4StDmask8                = FormatDefine({D, S0A4, S1A8, Dmask8}),
	Vdata2Vaddr4StDmask3                = FormatDefine({DA2, S0A4, S1A8, Dmask3}),
	Vdata2Vaddr4StDmask5                = FormatDefine({DA2, S0A4, S1A8, Dmask5}),
	Vdata2Vaddr4StDmask9                = FormatDefine({DA2, S0A4, S1A8, Dmask9}),
	Vdata3Vaddr4StDmask7                = FormatDefine({DA3, S0A4, S1A8, Dmask7}),
	Vdata4Vaddr1StDmaskF                = FormatDefine({DA4, S0, S1A8, DmaskF}),
	Vdata4Vaddr3StDmaskF                = FormatDefine({DA4, S0A3, S1A8, DmaskF}),
	Vdata4Vaddr3StSsDmaskF              = FormatDefine({DA4, S0A3, S1A8, S2A4, DmaskF}),
	Vdata4Vaddr4StDmaskF                = FormatDefine({DA4, S0A4, S1A8, DmaskF}),
	Vdata4VaddrSvSoffsIdxen             = FormatDefine({DA4, S0, S1A4, S2, Idxen}),
	Vdata4VaddrSvSoffsIdxenFloat4       = FormatDefine({DA4, S0, S1A4, S2, Idxen, Float4}),
	VdstGds                             = FormatDefine({D, Gds}),
	VdstSdst2Vsrc0Vsrc1                 = FormatDefine({D, D2A2, S0, S1}),
	VdstVsrc0Vsrc1Smask2                = FormatDefine({D, S0, S1, S2A2}),
	VdstVsrc0Vsrc1Vsrc2                 = FormatDefine({D, S0, S1, S2}),
	VdstVsrcAttrChan                    = FormatDefine({D, S0, Attr}),
	ParamVsrc0Vsrc1Vsrc2Vsrc3           = FormatDefine({Param, S0, S1, S2, S3}),
};

} // namespace ShaderInstructionFormat

struct ShaderInstructionTypeFormat
{
	ShaderInstructionType           type   = ShaderInstructionType::Unknown;
	ShaderInstructionFormat::Format format = ShaderInstructionFormat::Unknown;
};

enum class ShaderOperandType
{
	Unknown,
	LiteralConstant,
	IntegerInlineConstant,
	FloatInlineConstant,
	VccLo,
	VccHi,
	ExecLo,
	ExecHi,
	ExecZ,
	Scc,
	Vgpr,
	Sgpr,
	M0,
	Null
};

union ShaderConstant
{
	int32_t  i;
	uint32_t u;
	float    f;
};

struct ShaderOperand
{
	ShaderOperandType type        = ShaderOperandType::Unknown;
	ShaderConstant    constant    = {0};
	int               register_id = 0;
	int               size        = 0;
	float             multiplier  = 1.0f;
	bool              absolute    = false;
	bool              negate      = false;
	bool              clamp       = false;

	bool operator==(const ShaderOperand& other) const
	{
		return type == other.type && constant.u == other.constant.u && register_id == other.register_id && size == other.size;
	}
};

struct ShaderInstruction
{
	uint32_t                        pc     = 0;
	ShaderInstructionType           type   = ShaderInstructionType::Unknown;
	ShaderInstructionFormat::Format format = ShaderInstructionFormat::Unknown;
	ShaderOperand                   src[4];
	int                             src_num = 0;
	ShaderOperand                   dst;
	ShaderOperand                   dst2;
	bool                            globally_coherent        = false;
	bool                            unnormalized_coordinates = false;
	bool                            export_valid_mask        = false;
	bool                            export_done              = false;
	uint8_t                         export_target            = 0xff; // EXP target; unset for constructed legacy IR
};

// The public signed-I4 mapping proves the numeric domain, but not physical
// source-bit placement. Admit only known whole integer constants in [-8, 7];
// register operands and wider raw patterns require a separately proved contract.
inline bool ShaderCvtOffF32I4InstructionSupported(const ShaderInstruction& inst)
{
	const auto plain = [](const ShaderOperand& operand) {
		return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp;
	};
	const auto unused = [&](const ShaderOperand& operand) {
		return plain(operand) && operand.type == ShaderOperandType::Unknown && operand.constant.u == 0 &&
		       operand.register_id == 0 && operand.size == 0;
	};
	const auto& source = inst.src[0];
	if (inst.type != ShaderInstructionType::VCvtOffF32I4 || inst.format != ShaderInstructionFormat::SVdstSVsrc0 ||
	    inst.src_num != 1 || !plain(source) || source.size != 0 || source.register_id != 0 ||
	    (source.type != ShaderOperandType::IntegerInlineConstant && source.type != ShaderOperandType::LiteralConstant) ||
	    (source.constant.u > 7u && source.constant.u < 0xfffffff8u) ||
	    !plain(inst.dst) || inst.dst.type != ShaderOperandType::Vgpr || inst.dst.register_id < 0 || inst.dst.register_id > 255 ||
	    inst.dst.size != 1 || inst.dst.constant.u != 0 || !unused(inst.dst2) ||
	    inst.globally_coherent || inst.unnormalized_coordinates || inst.export_valid_mask || inst.export_done ||
	    inst.export_target != 0xff) return false;
	for (int i = 1; i < 4; ++i)
		if (!unused(inst.src[i])) return false;
	return true;
}

inline bool ShaderImageSampleSingleComponentInstructionSupported(const ShaderInstruction& inst)
{
	const auto plain = [](const ShaderOperand& operand) {
		return operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp;
	};
	const auto reg = [&](const ShaderOperand& operand, ShaderOperandType type, int width, int limit) {
		return plain(operand) && operand.type == type && operand.constant.u == 0 && operand.size == width &&
		       operand.register_id >= 0 && operand.register_id <= limit - width;
	};
	const auto unused = [&](const ShaderOperand& operand) {
		return plain(operand) && operand.type == ShaderOperandType::Unknown && operand.constant.u == 0 &&
		       operand.register_id == 0 && operand.size == 0;
	};
	return inst.type == ShaderInstructionType::ImageSample &&
	       (inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2 ||
	        inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4) &&
	       inst.src_num == 3 && reg(inst.dst, ShaderOperandType::Vgpr, 1, 256) &&
	       reg(inst.src[0], ShaderOperandType::Vgpr, 3, 256) && reg(inst.src[1], ShaderOperandType::Sgpr, 8, 104) &&
	       reg(inst.src[2], ShaderOperandType::Sgpr, 4, 104) && (inst.src[1].register_id & 3) == 0 &&
	       (inst.src[2].register_id & 3) == 0 && unused(inst.src[3]) && unused(inst.dst2) &&
	       !inst.globally_coherent && !inst.unnormalized_coordinates && !inst.export_valid_mask && !inst.export_done &&
	       inst.export_target == 0xff;
}

int ShaderGetParameterExportIndex(const ShaderInstruction& instruction);

class ShaderLabel
{
public:
	ShaderLabel(uint32_t dst, uint32_t src): m_dst(dst), m_src(src) {}
	explicit ShaderLabel(const ShaderInstruction& inst): m_dst(inst.pc + 4 + inst.src[0].constant.i), m_src(inst.pc) {}
	~ShaderLabel() = default;
	LUMEN_CLASS_DEFAULT_COPY(ShaderLabel);

	[[nodiscard]] uint32_t GetDst() const { return m_dst; }
	[[nodiscard]] uint32_t GetSrc() const { return m_src; }

	[[nodiscard]] String8 ToString() const { return String8::FromPrintf("label_%04" PRIx32 "_%04" PRIx32, m_dst, m_src); }

	void Disable()
	{
		m_dst = 0;
		m_src = 0;
	}

	[[nodiscard]] bool IsDisabled() const { return m_dst == 0 && m_src == 0; }

private:
	uint32_t m_dst;
	uint32_t m_src;
};

struct ShaderDebugPrintf
{
	enum class Type
	{
		Uint,
		Int,
		Float
	};
	uint32_t              pc = 0;
	String                format;
	Vector<Type>          types;
	Vector<ShaderOperand> args;
};

struct ShaderControlFlowBlock
{
	uint32_t          pc         = 0;
	bool              is_discard = false;
	bool              is_valid   = false;
	ShaderInstruction last;
};

class ShaderCode
{
public:
	ShaderCode() { m_instructions.Expand(128); };
	virtual ~ShaderCode() = default;
	LUMEN_CLASS_DEFAULT_COPY(ShaderCode);

	[[nodiscard]] const Vector<ShaderInstruction>& GetInstructions() const { return m_instructions; }
	Vector<ShaderInstruction>&                     GetInstructions() { return m_instructions; }
	[[nodiscard]] const Vector<ShaderLabel>&       GetLabels() const { return m_labels; }
	Vector<ShaderLabel>&                           GetLabels() { return m_labels; }
	[[nodiscard]] const Vector<ShaderLabel>&       GetIndirectLabels() const { return m_indirect_labels; }
	Vector<ShaderLabel>&                           GetIndirectLabels() { return m_indirect_labels; }
	[[nodiscard]] const Vector<ShaderDebugPrintf>& GetDebugPrintfs() const { return m_debug_printfs; }
	Vector<ShaderDebugPrintf>&                     GetDebugPrintfs() { return m_debug_printfs; }

	[[nodiscard]] String8 DbgDump() const;

	static String8 DbgInstructionToStr(const ShaderInstruction& inst);

	[[nodiscard]] ShaderType GetType() const { return m_type; }
	void                     SetType(ShaderType type) { this->m_type = type; }
	[[nodiscard]] ShaderDx10ClampMode GetDx10ClampMode() const { return m_dx10_clamp_mode; }
	void                              SetDx10ClampMode(ShaderDx10ClampMode mode) { m_dx10_clamp_mode = mode; }
	[[nodiscard]] ShaderFp32Mode      GetFp32Mode() const { return m_fp32_mode; }
	void                              SetFp32Mode(ShaderFp32Mode mode) { m_fp32_mode = mode; }
	[[nodiscard]] ShaderThreadTraceMode GetThreadTraceMode() const { return m_thread_trace_mode; }
	void                                SetThreadTraceMode(ShaderThreadTraceMode mode) { m_thread_trace_mode = mode; }

	[[nodiscard]] bool HasAnyOf(std::initializer_list<ShaderInstructionType> types) const
	{
		return std::any_of(types.begin(), types.end(),
		                   [this](auto type)
		                   { return m_instructions.Contains(type, [](auto inst, auto type) { return inst.type == type; }); });
	}

	[[nodiscard]] bool     IsVsEmbedded() const { return m_vs_embedded; }
	void                   SetVsEmbedded(bool embedded) { this->m_vs_embedded = embedded; }
	[[nodiscard]] uint32_t GetVsEmbeddedId() const { return m_vs_embedded_id; }
	void                   SetVsEmbeddedId(uint32_t embedded_id) { m_vs_embedded_id = embedded_id; }
	[[nodiscard]] bool     IsPsEmbedded() const { return m_ps_embedded; }
	void                   SetPsEmbedded(bool embedded) { this->m_ps_embedded = embedded; }
	[[nodiscard]] uint32_t GetPsEmbeddedId() const { return m_ps_embedded_id; }
	void                   SetPsEmbeddedId(uint32_t embedded_id) { m_ps_embedded_id = embedded_id; }

	//[[nodiscard]] bool                      IsDiscardBlock(uint32_t pc) const;
	//[[nodiscard]] bool                      IsDiscardInstruction(uint32_t index) const;
	//[[nodiscard]] Vector<ShaderInstruction> GetDiscardBlock(uint32_t pc) const;
	[[nodiscard]] ShaderControlFlowBlock    ReadBlock(uint32_t pc) const;
	[[nodiscard]] Vector<ShaderInstruction> ReadIntructions(const ShaderControlFlowBlock& block) const;

	[[nodiscard]] uint32_t GetCrc32() const { return m_crc32; }
	void                   SetCrc32(uint32_t c) { this->m_crc32 = c; }
	[[nodiscard]] uint32_t GetHash0() const { return m_hash0; }
	void                   SetHash0(uint32_t h) { this->m_hash0 = h; }

private:
	uint32_t                  m_hash0 = 0;
	uint32_t                  m_crc32 = 0;
	Vector<ShaderInstruction> m_instructions;
	Vector<ShaderLabel>       m_labels;
	Vector<ShaderLabel>       m_indirect_labels;
	ShaderType                m_type = ShaderType::Unknown;
	Vector<ShaderDebugPrintf> m_debug_printfs;
	uint32_t                  m_vs_embedded_id = 0;
	uint32_t                  m_ps_embedded_id = 0;
	ShaderDx10ClampMode       m_dx10_clamp_mode = ShaderDx10ClampMode::Enabled;
	ShaderFp32Mode            m_fp32_mode       = ShaderFp32Mode::Unvalidated;
	ShaderThreadTraceMode     m_thread_trace_mode = ShaderThreadTraceMode::Unknown;
	bool                      m_vs_embedded    = false;
	bool                      m_ps_embedded    = false;
};

// SIMM16 semantics beyond zero are unqualified. Disabled is an explicit
// execution contract, never inferred from the absence of a host trace sink.
inline bool ShaderThreadTraceDataFormSupported(const ShaderCode& code, uint32_t simm)
{
	return code.GetThreadTraceMode() == ShaderThreadTraceMode::Disabled && simm == 0;
}

inline bool ShaderThreadTraceDataInstructionSupported(const ShaderCode& code, const ShaderInstruction& inst)
{
	const auto canonical_operand = [](const ShaderOperand& operand, ShaderOperandType type) {
		return operand.type == type && operand.constant.u == 0 && operand.register_id == 0 && operand.size == 0 &&
		       operand.multiplier == 1.0f && !operand.absolute && !operand.negate && !operand.clamp;
	};
	if (inst.type != ShaderInstructionType::STTraceData || inst.format != ShaderInstructionFormat::Imm || inst.src_num != 1 ||
	    !ShaderThreadTraceDataFormSupported(code, inst.src[0].constant.u) ||
	    !canonical_operand(inst.src[0], ShaderOperandType::LiteralConstant) ||
	    !canonical_operand(inst.dst, ShaderOperandType::Unknown) || !canonical_operand(inst.dst2, ShaderOperandType::Unknown) ||
	    inst.globally_coherent || inst.unnormalized_coordinates || inst.export_valid_mask || inst.export_done) return false;
	for (int i = 1; i < 4; ++i)
		if (!canonical_operand(inst.src[i], ShaderOperandType::Unknown)) return false;
	return true;
}

struct ShaderId
{
	uint32_t         hash0 = 0;
	uint32_t         crc32 = 0;
	Vector<uint32_t> ids;
	bool             dx10_clamp_disabled = false;

	bool operator==(const ShaderId& other) const
	{
		return hash0 == other.hash0 && crc32 == other.crc32 && ids == other.ids && dx10_clamp_disabled == other.dx10_clamp_disabled;
	}
	bool operator!=(const ShaderId& other) const { return !(*this == other); }
};

constexpr uint32_t DstSel(uint32_t x, uint32_t y = 0, uint32_t z = 0, uint32_t w = 0)
{
	return x | (y << 3u) | (z << 6u) | (w << 9u);
}

inline uint8_t GetDstSel(uint32_t swizzle, uint32_t channel)
{
	return (swizzle >> (channel * 3u)) & 0x7u;
}

struct ShaderBufferResource
{
	uint32_t fields[4] = {0};

	void UpdateAddress44(uint64_t gpu_addr)
	{
		auto lo   = static_cast<uint32_t>(gpu_addr & 0xffffffffu);
		auto hi   = static_cast<uint32_t>(gpu_addr >> 32u);
		fields[0] = lo;
		fields[1] = (fields[1] & 0xfffff000u) | (hi & 0x00000fffu);
	}

	[[nodiscard]] uint16_t Stride() const { return (fields[1] >> 16u) & 0x3FFFu; }
	[[nodiscard]] bool     SwizzleEnabled() const { return ((fields[1] >> 31u) & 0x1u) == 1; }
	[[nodiscard]] uint32_t NumRecords() const { return fields[2]; }
	[[nodiscard]] uint8_t  DstSelX() const { return (fields[3] >> 0u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelY() const { return (fields[3] >> 3u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelZ() const { return (fields[3] >> 6u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelW() const { return (fields[3] >> 9u) & 0x7u; }
	[[nodiscard]] uint32_t DstSelXY() const { return (fields[3] >> 0u) & 0x3Fu; }
	[[nodiscard]] uint32_t DstSelXYZ() const { return (fields[3] >> 0u) & 0x1FFu; }
	[[nodiscard]] uint32_t DstSelXYZW() const { return (fields[3] >> 0u) & 0xFFFu; }
	[[nodiscard]] bool     AddTid() const { return ((fields[3] >> 23u) & 0x1u) == 1; }

	[[nodiscard]] uint64_t Base44() const { return (fields[0] | (static_cast<uint64_t>(fields[1]) << 32u)) & 0x0FFFFFFFFFFFu; }
	[[nodiscard]] uint8_t  Nfmt() const { return (fields[3] >> 12u) & 0x7u; }
	[[nodiscard]] uint8_t  Dfmt() const { return (fields[3] >> 15u) & 0xFu; }
	[[nodiscard]] uint8_t  MemoryType() const
	{
		return ((fields[1] >> 7u) & 0x60u) | ((fields[3] >> 25u) & 0x1cu) | ((fields[1] >> 14u) & 0x3u);
	}
};

struct ShaderTextureResource
{
	static constexpr uint8_t TYPE_8_COMPAT = 8;
	static constexpr uint8_t TYPE_2D       = 9;

	uint32_t fields[8] = {0};

	[[nodiscard]] bool IsAllZero() const
	{
		uint32_t bits = 0;
		for (const auto field: fields)
		{
			bits |= field;
		}
		return bits == 0;
	}

	void UpdateAddress38(uint64_t gpu_addr)
	{
		auto lo   = static_cast<uint32_t>(gpu_addr & 0xffffffffu);
		auto hi   = static_cast<uint32_t>(gpu_addr >> 32u);
		fields[0] = lo;
		fields[1] = (fields[1] & 0xffffffc0u) | (hi & 0x0000003fu);
	}

	[[nodiscard]] uint16_t MinLod() const { return (fields[1] >> 8u) & 0xFFFu; }
	[[nodiscard]] uint8_t  DstSelX() const { return (fields[3] >> 0u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelY() const { return (fields[3] >> 3u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelZ() const { return (fields[3] >> 6u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelW() const { return (fields[3] >> 9u) & 0x7u; }
	[[nodiscard]] uint32_t DstSelXY() const { return (fields[3] >> 0u) & 0x3Fu; }
	[[nodiscard]] uint32_t DstSelXYZ() const { return (fields[3] >> 0u) & 0x1FFu; }
	[[nodiscard]] uint32_t DstSelXYZW() const { return (fields[3] >> 0u) & 0xFFFu; }
	[[nodiscard]] uint8_t  BaseLevel() const { return (fields[3] >> 12u) & 0xFu; }
	[[nodiscard]] uint8_t  LastLevel() const { return (fields[3] >> 16u) & 0xFu; }
	[[nodiscard]] uint8_t  TileMode() const { return (fields[3] >> 20u) & 0x1Fu; }
	[[nodiscard]] uint8_t  Type() const { return (fields[3] >> 28u) & 0xFu; }
	[[nodiscard]] uint16_t Depth() const { return (fields[4] >> 0u) & 0x1FFFu; }

	[[nodiscard]] uint64_t Base38() const { return ((fields[0] | (static_cast<uint64_t>(fields[1]) << 32u)) & 0x3FFFFFFFFFu) << 8u; }
	[[nodiscard]] uint8_t  Dfmt() const { return (fields[1] >> 20u) & 0x3Fu; }
	[[nodiscard]] uint8_t  Nfmt() const { return (fields[1] >> 26u) & 0xFu; }
	[[nodiscard]] uint16_t Width4() const { return (fields[2] >> 0u) & 0x3FFFu; }
	[[nodiscard]] uint16_t Height4() const { return (fields[2] >> 14u) & 0x3FFFu; }
	[[nodiscard]] uint8_t  PerfMod() const { return (fields[2] >> 28u) & 0x7u; }
	[[nodiscard]] bool     Interlaced() const { return ((fields[2] >> 31u) & 0x1u) == 1; }
	[[nodiscard]] bool     Pow2Pad() const { return ((fields[3] >> 25u) & 0x1u) == 1; }
	[[nodiscard]] uint16_t Pitch() const { return (fields[4] >> 13u) & 0x3FFFu; }
	[[nodiscard]] uint16_t BaseArray() const { return (fields[5] >> 0u) & 0x1FFFu; }
	[[nodiscard]] uint16_t LastArray() const { return (fields[5] >> 13u) & 0x1FFFu; }
	[[nodiscard]] uint16_t MinLodWarn() const { return (fields[6] >> 0u) & 0xFFFu; }
	[[nodiscard]] bool     LodHdwCntEn() const { return ((fields[6] >> 20u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  CounterBankId() const { return (fields[6] >> 12u) & 0xFFu; }
	[[nodiscard]] uint8_t  MemoryType() const
	{
		return ((fields[1] >> 6u) & 0x3u) | ((fields[1] >> 30u) << 2u) | ((fields[3] & 0x04000000u) == 0 ? 0x60u : 0x10u);
	}
};

struct ShaderSamplerResource
{
	uint32_t fields[4] = {0};

	void UpdateIndex(uint32_t index) { fields[0] = index; }

	[[nodiscard]] uint8_t  ClampX() const { return (fields[0] >> 0u) & 0x7u; }
	[[nodiscard]] uint8_t  ClampY() const { return (fields[0] >> 3u) & 0x7u; }
	[[nodiscard]] uint8_t  ClampZ() const { return (fields[0] >> 6u) & 0x7u; }
	[[nodiscard]] uint8_t  MaxAnisoRatio() const { return (fields[0] >> 9u) & 0x7u; }
	[[nodiscard]] uint8_t  DepthCompareFunc() const { return (fields[0] >> 12u) & 0x7u; }
	[[nodiscard]] bool     ForceUnormCoords() const { return ((fields[0] >> 15u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  AnisoThreshold() const { return (fields[0] >> 16u) & 0x7u; }
	[[nodiscard]] bool     ForceDegamma() const { return ((fields[0] >> 20u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  AnisoBias() const { return (fields[0] >> 21u) & 0x3Fu; }
	[[nodiscard]] bool     TruncCoord() const { return ((fields[0] >> 27u) & 0x1u) == 1; }
	[[nodiscard]] bool     DisableCubeWrap() const { return ((fields[0] >> 28u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  FilterMode() const { return (fields[0] >> 29u) & 0x3u; }
	[[nodiscard]] uint16_t MinLod() const { return (fields[1] >> 0u) & 0xFFFu; }
	[[nodiscard]] uint16_t MaxLod() const { return (fields[1] >> 12u) & 0xFFFu; }
	[[nodiscard]] uint8_t  PerfMip() const { return (fields[1] >> 24u) & 0xFu; }
	[[nodiscard]] uint8_t  PerfZ() const { return (fields[1] >> 28u) & 0xFu; }
	[[nodiscard]] uint16_t LodBias() const { return (fields[2] >> 0u) & 0x3FFFu; }
	[[nodiscard]] uint8_t  LodBiasSec() const { return (fields[2] >> 14u) & 0x3Fu; }
	[[nodiscard]] uint8_t  XyMagFilter() const { return (fields[2] >> 20u) & 0x3u; }
	[[nodiscard]] uint8_t  XyMinFilter() const { return (fields[2] >> 22u) & 0x3u; }
	[[nodiscard]] uint8_t  ZFilter() const { return (fields[2] >> 24u) & 0x3u; }
	[[nodiscard]] uint8_t  MipFilter() const { return (fields[2] >> 26u) & 0x3u; }
	[[nodiscard]] uint16_t BorderColorPtr() const { return (fields[3] >> 0u) & 0xFFFu; }
	[[nodiscard]] uint8_t  BorderColorType() const { return (fields[3] >> 30u) & 0x3u; }

	[[nodiscard]] bool McCoordTrunc() const { return ((fields[0] >> 19u) & 0x1u) == 1; }
};

struct ShaderGdsResource
{
	uint32_t field = 0;

	[[nodiscard]] uint16_t Base() const { return (field >> 16u) & 0xFFFFu; }
	[[nodiscard]] uint16_t Size() const { return field & 0xFFFFu; }
};

struct ShaderDirectSgprResource
{
	uint32_t field = 0;
};

struct ShaderExtendedResource
{
	uint32_t fields[2] = {0};

	void UpdateAddress(uint64_t gpu_addr)
	{
		auto lo   = static_cast<uint32_t>(gpu_addr & 0xffffffffu);
		auto hi   = static_cast<uint32_t>(gpu_addr >> 32u);
		fields[0] = lo;
		fields[1] = hi;
	}

	[[nodiscard]] uint64_t Base() const { return (fields[0] | (static_cast<uint64_t>(fields[1]) << 32u)); }
};

struct ShaderVertexInputBuffer
{
	static constexpr int ATTR_MAX = 16;

	uint64_t addr                   = 0;
	uint32_t stride                 = 0;
	uint32_t num_records            = 0;
	int      attr_num               = 0;
	int      attr_indices[ATTR_MAX] = {0};
	uint32_t attr_offsets[ATTR_MAX] = {0};
};

struct ShaderVertexDestination
{
	int register_start = 0;
	int registers_num  = 0;
};

enum class ShaderStorageUsage
{
	Unknown,
	Constant,
	ReadOnly,
	ReadWrite,
};

enum class ShaderTextureUsage
{
	Unknown,
	ReadOnly,
	ReadWrite,
};

enum class ShaderStorageImageFormat: uint8_t
{
	Invalid,
	Rgba8,
	R8Unorm,
};

struct ShaderStorageResources
{
	static constexpr int BUFFERS_MAX = 16;

	ShaderBufferResource buffers[BUFFERS_MAX];
	ShaderStorageUsage   usages[BUFFERS_MAX]         = {};
	int                  slots[BUFFERS_MAX]          = {0};
	int                  start_register[BUFFERS_MAX] = {0};
	bool                 extended[BUFFERS_MAX]       = {};
	int                  buffers_num                 = 0;
	int                  binding_index               = 0;
};

struct ShaderTextureDescriptor
{
	ShaderTextureResource texture;
	ShaderTextureUsage    usage                      = ShaderTextureUsage::Unknown;
	int                   slot                       = 0;
	int                   start_register             = 0;
	bool                  extended                   = false;
	bool                  textures2d_without_sampler = false;

	[[nodiscard]] bool IsNullSampledImage() const
	{
		return usage == ShaderTextureUsage::ReadOnly && !textures2d_without_sampler && texture.IsAllZero();
	}
};

struct ShaderTextureResources
{
	static constexpr int RES_MAX = 16;

	[[nodiscard]] bool HasNullSampledImage() const
	{
		for (int index = 0; index < textures_num; index++)
		{
			if (desc[index].IsNullSampledImage())
			{
				return true;
			}
		}
		return false;
	}

	ShaderTextureDescriptor desc[RES_MAX];
	int                     textures_num           = 0;
	int                     textures2d_sampled_num = 0;
	int                     textures2d_storage_num = 0;
	int                     binding_sampled_index  = 0;
	int                     binding_storage_index  = 0;
};

struct ShaderSamplerResources
{
	static constexpr int RES_MAX = 16;

	ShaderSamplerResource samplers[RES_MAX];
	int                   slots[RES_MAX]          = {0};
	int                   start_register[RES_MAX] = {0};
	bool                  extended[RES_MAX]       = {};
	int                   samplers_num            = 0;
	int                   binding_index           = 0;
};

struct ShaderGdsResources
{
	static constexpr int POINTERS_MAX = 1;

	ShaderGdsResource pointers[POINTERS_MAX];
	int               slots[POINTERS_MAX]          = {0};
	int               start_register[POINTERS_MAX] = {0};
	bool              extended[POINTERS_MAX]       = {};
	int               pointers_num                 = 0;
	int               binding_index                = 0;
};

struct ShaderDirectSgprsResources
{
	static constexpr int SGPRS_MAX = 4;

	ShaderDirectSgprResource sgprs[SGPRS_MAX];
	int                      start_register[SGPRS_MAX] = {0};
	int                      sgprs_num                 = 0;
};

struct ShaderExtendedResources
{
	static constexpr int DATA_DWORDS_MAX = 64;

	bool                   used           = false;
	int                    slot           = 0;
	int                    start_register = 0;
	ShaderExtendedResource data;
};

enum class ShaderConstantStorage: uint8_t
{
	PushConstants,
	UniformBuffer,
};

struct ShaderBindResources
{
	static constexpr uint32_t UNIFORM_DESCRIPTOR_SET = 2;
	static constexpr uint32_t UNIFORM_BINDING = 0;
	static constexpr uint32_t CONSTANT_DATA_MAX = ShaderStorageResources::BUFFERS_MAX * 16 +
	    ShaderTextureResources::RES_MAX * 32 + ShaderSamplerResources::RES_MAX * 16 +
	    ((ShaderGdsResources::POINTERS_MAX + 3) / 4) * 16 +
	    ((ShaderDirectSgprsResources::SGPRS_MAX + 3) / 4) * 16;
	ShaderConstantStorage      constant_storage     = ShaderConstantStorage::PushConstants;
	uint32_t                   push_constant_offset = 0;
	uint32_t                   push_constant_size   = 0;
	uint32_t                   descriptor_set_slot  = 0;
	ShaderStorageResources     storage_buffers;
	ShaderTextureResources     textures2D;
	ShaderSamplerResources     samplers;
	ShaderGdsResources         gds_pointers;
	ShaderDirectSgprsResources direct_sgprs;
	ShaderExtendedResources    extended;
};

// Choose storage before shader/pipeline cache lookup; retain the byte packing
// in both paths. The second binding is null for compute.
bool ShaderSelectConstantStorage(ShaderBindResources* first, ShaderBindResources* second, uint32_t max_push_constants,
                                 uint32_t max_uniform_range, uint32_t max_descriptor_sets);

struct ShaderVertexInputInfo
{
	static constexpr int RES_MAX = 16;

	ShaderBufferResource    resources[RES_MAX];
	ShaderVertexDestination resources_dst[RES_MAX];
	ShaderVertexInputBuffer buffers[RES_MAX];
	ShaderBindResources     bind;
	int                     resources_num    = 0;
	int                     fetch_shader_reg = 0;
	int                     buffers_num      = 0;
	int                     export_count     = 0;
	bool                    fetch_external   = false;
	bool                    fetch_inline     = false;
	bool                    rect_list        = false;
	// A decoded external fetch is replaced by vertex attributes, not executed.
	// These SGPR effects must be dead before any subsequent main-shader read.
	uint64_t                fetch_written_sgprs[2] = {};
	bool                    fetch_effects_valid   = false;
	// Current-draw admission metadata; not part of the persistent shader ID.
	uint32_t                parameter_export_mask = 0;
	bool                    parameter_exports_valid = false;
	uint32_t                host_vertex_output_components = 0;
	bool                    host_parameter_interface_valid = false;
};

struct ShaderComputeInputInfo
{
	uint32_t            threads_num[3]     = {0, 0, 0};
	bool                group_id[3]        = {false, false, false};
	int                 thread_ids_num     = 0;
	int                 workgroup_register = 0;
	ShaderBindResources bind;
};

struct ShaderPixelInputInfo
{
	uint32_t            interpolator_settings[32] = {0};
	uint32_t            input_num                 = 0;
	uint32_t            shader_mask               = 0;
	uint8_t             target_output_mode[8]     = {};
	bool                ps_pos_xy                 = false;
	bool                ps_pixel_kill_enable      = false;
	bool                ps_early_z                = false;
	bool                ps_execute_on_noop        = false;
	ShaderBindResources bind;
};

void ShaderInit();
bool ShaderResetForNextSession();
bool ShaderNullSampledImageEnabled();
bool    ShaderDx10ClampZeroExperimentEnabled();
bool ShaderImplicitSampleLodZeroExperimentEnabled();
bool ShaderCodeHasImageLoad(const ShaderCode& code);
bool ShaderCodeHasStorageBufferWrites(const ShaderCode& code);
struct ShaderStorageWriteAuthority
{
	uint32_t no_store_mask = 0;
	uint32_t write_mask    = 0;
	bool     complete      = false;
};
// Bits identify the storage descriptor array entries of the compiled module.
// A failed classification grants no resource exclusion.
ShaderStorageWriteAuthority ShaderClassifyStorageBufferWrites(const ShaderCode& code, ShaderType expected_stage,
                                                               const ShaderBindResources& bind);
bool ShaderStorageBufferXyFloatStride8Compatible(const ShaderBufferResource& resource);
bool ShaderStorageBufferUnorm8x4Constant0011Compatible(const ShaderBufferResource& resource);
bool ShaderStorageBufferSscaled16XyzCompatible(const ShaderBufferResource& resource);
bool ShaderVertexInputSnorm8x4XyzCompatible(const ShaderBufferResource& resource, int registers_num);
bool ShaderVertexInputSint16x2Compatible(const ShaderBufferResource& resource, int registers_num);
bool ShaderVertexInputSint16x4Compatible(const ShaderBufferResource& resource, int registers_num);
bool ShaderVertexInputFloat4SwizzleCompatible(const ShaderBufferResource& resource, int registers_num);
bool ShaderStorageBufferFormatSupported(const ShaderBufferResource& resource);
bool ShaderStorageBufferFormatUsageSupported(const ShaderBufferResource& resource, ShaderStorageUsage usage);
bool    ShaderGraphicsFp32StateSupported(const HW::VertexShaderInfo& vs, const HW::PixelShaderInfo& ps);
bool ShaderGraphicsDx10ClampStateSupported(const HW::VertexShaderInfo& vs, const HW::PixelShaderInfo& ps);
bool ShaderNullSampledImageLoadUnsupported(const ShaderTextureResources& textures, bool pixel_stage, bool shader_has_image_load);
bool    ShaderTextureType8SingleLevelCompatible(const ShaderTextureResource& texture);
bool    ShaderTextureType8PixelSampleCompatible(const ShaderTextureResource& texture);
bool    ShaderTextureDescriptorDimensionSupported(const ShaderTextureDescriptor& desc, ShaderType shader_type);
// Capture and Validate require the caller's exclusive guest-memory admission.
// Analyze reads only private, owned bytes and copied registers. It acquires no
// guest lease and neither publishes nor authorizes native GPU resource access.
// A failed Capture is a conservative fallback before releasing that admission.
class ShaderOwnedDrawInputs
{
public:
	ShaderOwnedDrawInputs();
	~ShaderOwnedDrawInputs();
	LUMEN_CLASS_NO_COPY(ShaderOwnedDrawInputs);
	bool Capture(const HW::VertexShaderInfo* vertex, const HW::PixelShaderInfo* pixel,
	             const HW::ShaderRegisters* registers, const ShaderVertexSourceSnapshot* retained_vertex = nullptr);
	bool Analyze(ShaderVertexInputInfo* vertex, ShaderPixelInputInfo* pixel) const;
	bool Validate() const;
	const ShaderVertexSourceSnapshot* VertexSource() const;
	// Enumerates the exact captured guest intervals, including binary headers,
	// FETCH and descriptor tables. The callback receives no guest pointer.
	bool VisitRanges(bool (*visitor)(void*, uint64_t, uint64_t), void* context) const;
private:
	#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
	friend bool ShaderOwnedDrawInputsContractTest();
	#endif
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

bool    ShaderTextureR8UnormSampledCompatible(const ShaderTextureResource& texture);
bool    ShaderTextureR8UnormStorageCompatible(const ShaderTextureResource& texture);
uint32_t ShaderTextureStorageViewSwizzle(const ShaderTextureResource& texture);
ShaderStorageImageFormat ShaderGetUniformStorageImageFormat(const ShaderTextureResources& textures);
uint8_t ShaderGetUniformTextureType(const ShaderTextureResources& textures, ShaderTextureUsage usage);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderSessionResetContractTest();
bool ShaderOwnedDrawInputsContractTest();
bool ShaderNullSampledImageContractTest();
bool ShaderUsageLayoutContractTest();
bool ShaderResourceRoutingContractTest();
bool ShaderUsageOrderContractTest();
bool ShaderIdBulkAppendContractTest();
bool ShaderConstantStorageContractTest();
bool ShaderVertexResourcePreCacheFp32ContractTest();
bool ShaderVertexParseCacheContractTest();
bool ShaderVertexUserSgprDeclarationContractTest();
bool ShaderVertexReadOnlyStorageContractTest();
bool ShaderStorageWriteAuthorityContractTest();
bool ShaderPixelDirectSgprContractTest();
bool ShaderDx10ClampOptimizationContractTest();
bool ShaderDx10ClampSpirvPassThroughContractTest();
bool ShaderVMulLegacyF32OptimizationContractTest();
bool ShaderVMacLegacyF32OptimizationContractTest();
bool ShaderVRsqClampF32OptimizationContractTest();
bool ShaderAndroidFexStructuralProfileSelectorContractTest();
#endif
void             ShaderCalcBindingIndices(ShaderBindResources* bind);
void             ShaderGetInputInfoVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh, ShaderVertexInputInfo* info);
void             ShaderGetInputInfoPS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh, const ShaderVertexInputInfo* vs_info,
                                      ShaderPixelInputInfo* ps_info);
void             ShaderGetInputInfoCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* sh, ShaderComputeInputInfo* info);
void             ShaderDbgDumpInputInfo(const ShaderVertexInputInfo* info);
void             ShaderDbgDumpInputInfo(const ShaderPixelInputInfo* info);
void             ShaderDbgDumpInputInfo(const ShaderComputeInputInfo* info);
ShaderId         ShaderGetIdVS(const HW::VertexShaderInfo* regs, const ShaderVertexInputInfo* input_info);
ShaderId         ShaderGetIdPS(const HW::PixelShaderInfo* regs, const ShaderPixelInputInfo* input_info);
bool             ShaderIsPixelColorExportCompatible(const ShaderCode& code, uint32_t target_output_mode, uint32_t shader_mask);
bool             ShaderVertexParameterExportsSupported(const ShaderCode& code, const ShaderVertexInputInfo& input,
                                                       const char** rejection_reason = nullptr, uint32_t* rejection_pc = nullptr,
                                                       uint32_t* parameter_export_mask = nullptr);
bool             ShaderVertexPixelInterfaceSupported(const ShaderVertexInputInfo& vertex, const ShaderPixelInputInfo& pixel,
                                                     uint32_t max_vertex_output_components, uint32_t max_fragment_input_components,
                                                     const char** rejection_reason = nullptr);
ShaderId         ShaderGetIdCS(const HW::ComputeShaderInfo* regs, const ShaderComputeInputInfo* input_info);
ShaderCode       ShaderParseVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh);
ShaderCode       ShaderParsePS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh);
ShaderCode       ShaderParseCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* sh);
Vector<uint32_t> ShaderRecompileVS(const ShaderCode& code, const ShaderVertexInputInfo* input_info);
Vector<uint32_t> ShaderRecompilePS(const ShaderCode& code, const ShaderPixelInputInfo* input_info);
Vector<uint32_t> ShaderRecompileCS(const ShaderCode& code, const ShaderComputeInputInfo* input_info);
void ShaderRecompileGraphicsPair(const ShaderCode& vertex_code, const ShaderVertexInputInfo* vertex_input,
                                const ShaderCode& pixel_code, const ShaderPixelInputInfo* pixel_input,
                                Vector<uint32_t>* vertex_words, Vector<uint32_t>* pixel_words);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool ShaderCompiledProgramsContractTest();
#endif
bool             ShaderIsDisabled(uint64_t addr, bool vertex_source = false);
void             ShaderDisable(uint64_t id);
void             ShaderInjectDebugPrintf(uint64_t id, const ShaderDebugPrintf& cmd);

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_H_ */
