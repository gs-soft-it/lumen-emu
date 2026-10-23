#include "Emulator/Graphics/ShaderParse.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"

#include "Emulator/Graphics/Shader.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#ifdef LUMEN_EMU_ENABLED

#define LUMEN_SHADER_PARSER_ARGS                                                                                                            \
	[[maybe_unused]] uint32_t pc, [[maybe_unused]] const uint32_t *src, [[maybe_unused]] const uint32_t *buffer,                           \
	    [[maybe_unused]] ShaderCode *dst
#define LUMEN_SHADER_PARSER(f) static uint32_t f(LUMEN_SHADER_PARSER_ARGS)
#define LUMEN_CP_OP_PARSER_ARGS                                                                                                             \
	[[maybe_unused]] CommandProcessor *cp, [[maybe_unused]] uint32_t cmd_id, [[maybe_unused]] const uint32_t *buffer,                      \
	    [[maybe_unused]] uint32_t dw, [[maybe_unused]] uint32_t num_dw
#define LUMEN_CP_OP_PARSER(f) static uint32_t f(LUMEN_CP_OP_PARSER_ARGS)

#define LUMEN_TYPE_STR(s) [[maybe_unused]] static const char* type_str = s;
#define LUMEN_NI(i)                                                                                                                         \
	printf("%s", dst->DbgDump().c_str());                                                                                                  \
	EXIT("unknown %s instruction %s, opcode = 0x%" PRIx32 ", word0 = 0x%08" PRIx32 " at addr 0x%08" PRIx32                            \
	     " (hash0 = 0x%08" PRIx32 ", crc32 = 0x%08" PRIx32 ")\n",                                                                         \
	     type_str, i, opcode, ShaderReadSourceWord(buffer, 0), pc, dst->GetHash0(), dst->GetCrc32());
#define LUMEN_UNKNOWN_OP()                                                                                                                  \
	printf("%s", dst->DbgDump().c_str());                                                                                                  \
	EXIT("unknown %s opcode: 0x%" PRIx32 ", word0 = 0x%08" PRIx32 " at addr 0x%08" PRIx32                                                \
	     " (hash0 = 0x%08" PRIx32 ", crc32 = 0x%08" PRIx32 ")\n",                                                                         \
	     type_str, opcode, ShaderReadSourceWord(buffer, 0), pc, dst->GetHash0(), dst->GetCrc32());

namespace Lumen::Libs::Graphics {

static uint32_t ShaderReadSourceWord(const uint32_t* source, uint64_t index)
{
	const auto address = reinterpret_cast<uint64_t>(source);
	EXIT_IF(index > UINT64_MAX / sizeof(uint32_t));
	const uint64_t offset = index * sizeof(uint32_t);
	EXIT_IF(address > UINT64_MAX - offset ||
	        !ShaderPrepareMemoryRead(address + offset, sizeof(uint32_t)));
	return source[index];
}


bool ShaderVMacLegacyF32ExperimentEnabled()
{
	#if (defined(LUMEN_V_MAC_LEGACY_F32_RELEASE_BUILD) && LUMEN_V_MAC_LEGACY_F32_RELEASE_BUILD == 1 && \
	     ((LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_APPLE_NATIVE_METAL && \
	       (defined(__aarch64__) || defined(__arm64__))) || \
	      (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64))) || \
	    (defined(LUMEN_V_MAC_LEGACY_F32_ANDROID_FEX_BUILD) && \
	     LUMEN_V_MAC_LEGACY_F32_ANDROID_FEX_BUILD == 1 && \
	     LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A)
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
	#else
	return false;
	#endif
}


bool ShaderVMacLegacyF32FormSupported(const ShaderCode& code, uint32_t src0, uint32_t src1, uint32_t dst, bool sdwa, bool dpp,
                                      uint32_t source_abs, uint32_t source_neg, uint32_t omod, bool clamp)
{
	const auto direct_vgpr = [](uint32_t operand) { return operand >= 256 && operand <= 511; };
	return ShaderVMacLegacyF32ExperimentEnabled() && code.GetType() == ShaderType::Vertex &&
	       code.GetFp32Mode() == ShaderFp32Mode::RneFlushInOut && code.GetDx10ClampMode() == ShaderDx10ClampMode::Disabled &&
	       direct_vgpr(src0) && direct_vgpr(src1) && direct_vgpr(dst) && !sdwa && !dpp && source_abs == 0 && source_neg == 0 &&
	       omod == 0 && !clamp;
}

bool ShaderVRsqClampF32ExperimentEnabled()
{
	#if (defined(LUMEN_V_RSQ_CLAMP_F32_RELEASE_BUILD) && LUMEN_V_RSQ_CLAMP_F32_RELEASE_BUILD == 1 && \
	     ((LUMEN_PLATFORM == LUMEN_PLATFORM_OSX && LUMEN_APPLE_NATIVE_METAL && \
	       (defined(__aarch64__) || defined(__arm64__))) || \
	      (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64))) || \
	    (defined(LUMEN_V_RSQ_CLAMP_F32_ANDROID_FEX_BUILD) && \
	     LUMEN_V_RSQ_CLAMP_F32_ANDROID_FEX_BUILD == 1 && \
	     LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID && LUMEN_ABI == LUMEN_ABI_ARM64_V8A)
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
	#else
	return false;
	#endif
}

bool ShaderVRsqClampF32FormSupported(const ShaderCode& code, uint32_t src0, uint32_t dst)
{
	const auto direct_vgpr = [](uint32_t operand) { return operand >= 256 && operand <= 511; };
	return ShaderVRsqClampF32ExperimentEnabled() && code.GetType() == ShaderType::Vertex && direct_vgpr(src0) && direct_vgpr(dst);
}

bool ShaderVMulLegacyF32FormSupported(const ShaderCode& code, bool sdwa, bool dpp, uint32_t source_abs, uint32_t source_neg, uint32_t omod,
                                      bool clamp, uint32_t unused_src2)
{
	const auto type           = code.GetType();
	const bool graphics_stage = type == ShaderType::Vertex || type == ShaderType::Pixel;
	return graphics_stage && code.GetFp32Mode() == ShaderFp32Mode::RneFlushInOut && !sdwa && !dpp && source_abs == 0 && source_neg == 0 &&
	       omod == 0 && !clamp && unused_src2 == 0;
}

static ShaderOperand operand_parse(uint32_t code)
{
	ShaderOperand ret;

	ret.size = 1;

	if (code >= 0 && code <= 103)
	{
		ret.type        = ShaderOperandType::Sgpr;
		ret.register_id = static_cast<int>(code);
	} else if (code >= 128 && code <= 192)
	{
		ret.type       = ShaderOperandType::IntegerInlineConstant;
		ret.constant.i = static_cast<int>(code) - 128;
		ret.size       = 0;
	} else if (code >= 193 && code <= 208)
	{
		ret.type       = ShaderOperandType::IntegerInlineConstant;
		ret.constant.i = 192 - static_cast<int>(code);
		ret.size       = 0;
	} else if (code >= 240 && code <= 247)
	{
		static const float fv[] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
		ret.type                = ShaderOperandType::FloatInlineConstant;
		ret.constant.f          = fv[static_cast<int>(code) - 240];
		ret.size                = 0;
	} else if (code >= 256)
	{
		ret.type        = ShaderOperandType::Vgpr;
		ret.register_id = static_cast<int>(code) - 256;
	} else
	{
		switch (code)
		{
			case 106: ret.type = ShaderOperandType::VccLo; break;
			case 107: ret.type = ShaderOperandType::VccHi; break;
			case 124: ret.type = ShaderOperandType::M0; break;
			case 125: ret.type = ShaderOperandType::Null; break;
			case 126: ret.type = ShaderOperandType::ExecLo; break;
			case 127: ret.type = ShaderOperandType::ExecHi; break;
			case 252: ret.type = ShaderOperandType::ExecZ; break;
			case 255:
				ret.type = ShaderOperandType::LiteralConstant;
				ret.size = 0;
				break;
			default: EXIT("unknown operand: %u\n", code);
		}
	}

	return ret;
}

LUMEN_SHADER_PARSER(shader_parse_sopc)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("sopc");

	uint32_t ssrc1  = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0xffu;
	uint32_t ssrc0  = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xffu;
	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src[1]  = operand_parse(ssrc1);
	inst.src_num = 2;

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	if (inst.src[1].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[1].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	inst.format = ShaderInstructionFormat::Ssrc0Ssrc1;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::SCmpEqI32; break;
		case 0x01: inst.type = ShaderInstructionType::SCmpLgI32; break;
		case 0x02: inst.type = ShaderInstructionType::SCmpGtI32; break;
		case 0x03: inst.type = ShaderInstructionType::SCmpGeI32; break;
		case 0x04: inst.type = ShaderInstructionType::SCmpLtI32; break;
		case 0x05: inst.type = ShaderInstructionType::SCmpLeI32; break;
		case 0x06: inst.type = ShaderInstructionType::SCmpEqU32; break;
		case 0x07: inst.type = ShaderInstructionType::SCmpLgU32; break;
		case 0x08: inst.type = ShaderInstructionType::SCmpGtU32; break;
		case 0x09: inst.type = ShaderInstructionType::SCmpGeU32; break;
		case 0x0a: inst.type = ShaderInstructionType::SCmpLtU32; break;
		case 0x0b: inst.type = ShaderInstructionType::SCmpLeU32; break;
		case 0xC: LUMEN_NI("s_bitcmp0_b32"); break;
		case 0xD: LUMEN_NI("s_bitcmp1_b32"); break;
		case 0xE: LUMEN_NI("s_bitcmp0_b64"); break;
		case 0xF: LUMEN_NI("s_bitcmp1_b64"); break;
		case 0x10: LUMEN_NI("s_setvskip"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_sopk)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("sopk");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 23u) & 0x1fu;
	auto     imm    = static_cast<int16_t>(ShaderReadSourceWord(buffer, 0) >> 0u & 0xffffu);
	uint32_t sdst   = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc  = pc;
	inst.dst = operand_parse(sdst);

	inst.format            = ShaderInstructionFormat::SVdstSVsrc0;
	inst.src[0].type       = ShaderOperandType::IntegerInlineConstant;
	inst.src[0].constant.i = imm;
	inst.src_num           = 1;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::SMovkI32; break;

		case 0x02: LUMEN_NI("s_cmovk_i32"); break;
		case 0x03: LUMEN_NI("s_cmpk_eq_i32"); break;
		case 0x04: LUMEN_NI("s_cmpk_lg_i32"); break;
		case 0x05: LUMEN_NI("s_cmpk_gt_i32"); break;
		case 0x06: LUMEN_NI("s_cmpk_ge_i32"); break;
		case 0x07: LUMEN_NI("s_cmpk_lt_i32"); break;
		case 0x08: LUMEN_NI("s_cmpk_le_i32"); break;
		case 0x09: LUMEN_NI("s_cmpk_eq_u32"); break;
		case 0x0A: LUMEN_NI("s_cmpk_lg_u32"); break;
		case 0x0B: LUMEN_NI("s_cmpk_gt_u32"); break;
		case 0x0C: LUMEN_NI("s_cmpk_ge_u32"); break;
		case 0x0D: LUMEN_NI("s_cmpk_lt_u32"); break;
		case 0x0E: LUMEN_NI("s_cmpk_le_u32"); break;
		case 0x0F: LUMEN_NI("s_addk_i32"); break;
		case 0x10: inst.type = ShaderInstructionType::SMulkI32; break;
		case 0x11: LUMEN_NI("s_cbranch_i_fork"); break;
		case 0x12: LUMEN_NI("s_getreg_b32"); break;
		case 0x13: LUMEN_NI("s_setreg_b32"); break;
		case 0x14: LUMEN_NI("s_getreg_regrd_b32"); break;
		case 0x15: LUMEN_NI("s_setreg_imm32_b32"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return 1;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_sopp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("sopp");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x7fu;
	uint32_t simm   = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xffffu;

	ShaderInstruction inst;
	inst.pc = pc;

	inst.format            = ShaderInstructionFormat::Label;
	inst.src[0].type       = ShaderOperandType::LiteralConstant;
	inst.src[0].constant.i = static_cast<int16_t>(simm) * 4;
	inst.src_num           = 1;

	switch (opcode)
	{
		case 0x01:
			inst.type    = ShaderInstructionType::SEndpgm;
			inst.format  = ShaderInstructionFormat::Empty;
			inst.src_num = 0;
			break;
		case 0x02: inst.type = ShaderInstructionType::SBranch; break;
		case 0x04: inst.type = ShaderInstructionType::SCbranchScc0; break;
		case 0x05: inst.type = ShaderInstructionType::SCbranchScc1; break;
		case 0x06: inst.type = ShaderInstructionType::SCbranchVccz; break;
		case 0x07: inst.type = ShaderInstructionType::SCbranchVccnz; break;
		case 0x08: inst.type = ShaderInstructionType::SCbranchExecz; break;
		case 0x0c:
			inst.type              = ShaderInstructionType::SWaitcnt;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;
		case 0x10:
			inst.type              = ShaderInstructionType::SSendmsg;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;
		case 0x16:
			if (!ShaderThreadTraceDataFormSupported(*dst, simm))
				EXIT("unsupported S_TTRACEDATA requires explicitly disabled thread trace and SIMM16=0\n");
			inst.type              = ShaderInstructionType::STTraceData;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].constant.u = simm;
			break;
		case 0x20:
			inst.type              = ShaderInstructionType::SInstPrefetch;
			inst.format            = ShaderInstructionFormat::Imm;
			inst.src[0].type       = ShaderOperandType::LiteralConstant;
			inst.src[0].constant.u = simm;
			inst.src_num           = 1;
			break;

		case 0x0: LUMEN_NI("s_nop"); break;
		case 0x9: LUMEN_NI("s_cbranch_execnz"); break;
		case 0xA: LUMEN_NI("s_barrier"); break;
		case 0xB: LUMEN_NI("s_setkill"); break;
		case 0xD: LUMEN_NI("s_sethalt"); break;
		case 0xE: LUMEN_NI("s_sleep"); break;
		case 0xF: LUMEN_NI("s_setprio"); break;
		case 0x11: LUMEN_NI("s_sendmsghalt"); break;
		case 0x12: LUMEN_NI("s_trap"); break;
		case 0x13: LUMEN_NI("s_icache_inv"); break;
		case 0x14: LUMEN_NI("s_incperflevel"); break;
		case 0x15: LUMEN_NI("s_decperflevel"); break;
		case 0x17: LUMEN_NI("s_cbranch_cdbgsys"); break;
		case 0x18: LUMEN_NI("s_cbranch_cdbguser"); break;
		case 0x19: LUMEN_NI("s_cbranch_cdbgsys_or_user"); break;
		case 0x1A: LUMEN_NI("s_cbranch_cdbgsys_and_user"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	if (inst.type == ShaderInstructionType::SCbranchScc0 || inst.type == ShaderInstructionType::SCbranchScc1 ||
	    inst.type == ShaderInstructionType::SCbranchVccz || inst.type == ShaderInstructionType::SCbranchVccnz ||
	    inst.type == ShaderInstructionType::SCbranchExecz || inst.type == ShaderInstructionType::SBranch)
	{
		dst->GetLabels().Add(ShaderLabel(inst));

		if (inst.type != ShaderInstructionType::SBranch)
		{
			dst->GetIndirectLabels().Add(ShaderLabel(inst.pc + 4, inst.pc));
		}
	}

	return 1;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_sop1)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("sop1");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0xffu;
	uint32_t ssrc0  = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xffu;
	uint32_t sdst   = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src_num = 1;
	inst.dst     = operand_parse(sdst);

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	switch (opcode)
	{
		case 0x03:
			inst.type   = ShaderInstructionType::SMovB32;
			inst.format = ShaderInstructionFormat::SVdstSVsrc0;
			break;
		case 0x04:
			inst.type        = ShaderInstructionType::SMovB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x05: LUMEN_NI("s_cmov_b32"); break;
		case 0x06: LUMEN_NI("s_cmov_b64"); break;
		case 0x07:
			inst.type   = ShaderInstructionType::SNotB32;
			inst.format = ShaderInstructionFormat::SVdstSVsrc0;
			break;
		case 0x08: LUMEN_NI("s_not_b64"); break;
		case 0x09: LUMEN_NI("s_wqm_b32"); break;
		case 0x0a:
			inst.type        = ShaderInstructionType::SWqmB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x0B: LUMEN_NI("s_brev_b32"); break;
		case 0x0C: LUMEN_NI("s_brev_b64"); break;
		case 0x0D: LUMEN_NI("s_bcnt0_i32_b32"); break;
		case 0x0E: LUMEN_NI("s_bcnt0_i32_b64"); break;
		case 0x0F: LUMEN_NI("s_bcnt1_i32_b32"); break;
		case 0x10: LUMEN_NI("s_bcnt1_i32_b64"); break;
		case 0x11: LUMEN_NI("s_ff0_i32_b32"); break;
		case 0x12: LUMEN_NI("s_ff0_i32_b64"); break;
		case 0x13: LUMEN_NI("s_ff1_i32_b32"); break;
		case 0x14: LUMEN_NI("s_ff1_i32_b64"); break;
		case 0x15: LUMEN_NI("s_flbit_i32_b32"); break;
		case 0x16: LUMEN_NI("s_flbit_i32_b64"); break;
		case 0x17: LUMEN_NI("s_flbit_i32"); break;
		case 0x18: LUMEN_NI("s_flbit_i32_i64"); break;
		case 0x19: LUMEN_NI("s_sext_i32_i8"); break;
		case 0x1A: LUMEN_NI("s_sext_i32_i16"); break;
		case 0x1B: LUMEN_NI("s_bitset0_b32"); break;
		case 0x1C: LUMEN_NI("s_bitset0_b64"); break;
		case 0x1D: LUMEN_NI("s_bitset1_b32"); break;
		case 0x1E: LUMEN_NI("s_bitset1_b64"); break;
		case 0x1F: LUMEN_NI("s_getpc_b64"); break;
		case 0x20:
			inst.type        = ShaderInstructionType::SSetpcB64;
			inst.format      = ShaderInstructionFormat::Saddr;
			inst.src[0].size = 2;
			break;
		case 0x21:
			inst.type        = ShaderInstructionType::SSwappcB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.src[0].size = 2;
			inst.dst.size    = 2;
			break;
		case 0x22: LUMEN_NI("s_rfe_b64"); break;
		case 0x24:
			inst.type        = ShaderInstructionType::SAndSaveexecB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x25: LUMEN_NI("s_or_saveexec_b64"); break;
		case 0x26: LUMEN_NI("s_xor_saveexec_b64"); break;
		case 0x27: LUMEN_NI("s_andn2_saveexec_b64"); break;
		case 0x28: LUMEN_NI("s_orn2_saveexec_b64"); break;
		case 0x29: LUMEN_NI("s_nand_saveexec_b64"); break;
		case 0x2A: LUMEN_NI("s_nor_saveexec_b64"); break;
		case 0x2B: LUMEN_NI("s_xnor_saveexec_b64"); break;
		case 0x2C: LUMEN_NI("s_quadmask_b32"); break;
		case 0x2D: LUMEN_NI("s_quadmask_b64"); break;
		case 0x2E: LUMEN_NI("s_movrels_b32"); break;
		case 0x2F: LUMEN_NI("s_movrels_b64"); break;
		case 0x30: LUMEN_NI("s_movreld_b32"); break;
		case 0x31: LUMEN_NI("s_movreld_b64"); break;
		case 0x32: LUMEN_NI("s_cbranch_join"); break;
		case 0x33: LUMEN_NI("s_mov_regrd_b32"); break;
		case 0x34: LUMEN_NI("s_abs_i32"); break;
		case 0x35: LUMEN_NI("s_mov_fed_b32"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_sop2)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("sop2");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 23u) & 0x7fu;

	switch (opcode)
	{
		case 0x7d: return shader_parse_sop1(pc, src, buffer, dst); break;
		case 0x7e: return shader_parse_sopc(pc, src, buffer, dst); break;
		case 0x7f: return shader_parse_sopp(pc, src, buffer, dst); break;
		default: break;
	}

	if (opcode >= 0x60)
	{
		return shader_parse_sopk(pc, src, buffer, dst);
	}

	uint32_t ssrc1 = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0xffu;
	uint32_t ssrc0 = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xffu;
	uint32_t sdst  = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x7fu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(ssrc0);
	inst.src[1]  = operand_parse(ssrc1);
	inst.src_num = 2;
	inst.dst     = operand_parse(sdst);

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	if (inst.src[1].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[1].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	inst.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::SAddU32; break;
		case 0x01: LUMEN_NI("s_sub_u32"); break;
		case 0x02: inst.type = ShaderInstructionType::SAddI32; break;
		case 0x03: inst.type = ShaderInstructionType::SSubI32; break;
		case 0x04: inst.type = ShaderInstructionType::SAddcU32; break;
		case 0x05: LUMEN_NI("s_subb_u32"); break;
		case 0x06: LUMEN_NI("s_min_i32"); break;
		case 0x07: LUMEN_NI("s_min_u32"); break;
		case 0x08: LUMEN_NI("s_max_i32"); break;
		case 0x09: LUMEN_NI("s_max_u32"); break;
		case 0x0a: inst.type = ShaderInstructionType::SCselectB32; break;
		case 0x0b:
			inst.type        = ShaderInstructionType::SCselectB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x0e: inst.type = ShaderInstructionType::SAndB32; break;
		case 0x0f:
			inst.type        = ShaderInstructionType::SAndB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x10: inst.type = ShaderInstructionType::SOrB32; break;
		case 0x11:
			inst.type        = ShaderInstructionType::SOrB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x12: LUMEN_NI("s_xor_b32"); break;
		case 0x13:
			inst.type        = ShaderInstructionType::SXorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x14: LUMEN_NI("s_andn2_b32"); break;
		case 0x15:
			inst.type        = ShaderInstructionType::SAndn2B64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x16: LUMEN_NI("s_orn2_b32"); break;
		case 0x17:
			inst.type        = ShaderInstructionType::SOrn2B64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x18: LUMEN_NI("s_nand_b32"); break;
		case 0x19:
			inst.type        = ShaderInstructionType::SNandB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1A: LUMEN_NI("s_nor_b32"); break;
		case 0x1b:
			inst.type        = ShaderInstructionType::SNorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1C: LUMEN_NI("s_xnor_b32"); break;
		case 0x1d:
			inst.type        = ShaderInstructionType::SXnorB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc12;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			inst.src[1].size = 2;
			break;
		case 0x1e: inst.type = ShaderInstructionType::SLshlB32; break;
		case 0x1f:
			inst.type        = ShaderInstructionType::SLshlB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x20: inst.type = ShaderInstructionType::SLshrB32; break;
		case 0x21:
			inst.type        = ShaderInstructionType::SLshrB64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x22: LUMEN_NI("s_ashr_i32"); break;
		case 0x23: LUMEN_NI("s_ashr_i64"); break;
		case 0x24: inst.type = ShaderInstructionType::SBfmB32; break;
		case 0x25: LUMEN_NI("s_bfm_b64"); break;
		case 0x26: inst.type = ShaderInstructionType::SMulI32; break;
		case 0x27: inst.type = ShaderInstructionType::SBfeU32; break;
		case 0x28: LUMEN_NI("s_bfe_i32"); break;
		case 0x29:
			inst.type        = ShaderInstructionType::SBfeU64;
			inst.format      = ShaderInstructionFormat::Sdst2Ssrc02Ssrc1;
			inst.dst.size    = 2;
			inst.src[0].size = 2;
			break;
		case 0x2A: LUMEN_NI("s_bfe_i64"); break;
		case 0x2B: LUMEN_NI("s_cbranch_g_fork"); break;
		case 0x2C: LUMEN_NI("s_absdiff_i32"); break;
		case 0x32: LUMEN_NI("s_pack_ll_b32_b16"); break;
		case 0x33: LUMEN_NI("s_pack_lh_b32_b16"); break;
		case 0x34: LUMEN_NI("s_pack_hh_b32_b16"); break;
		case 0x35: inst.type = ShaderInstructionType::SMulHiU32; break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size,google-readability-function-size,hicpp-function-size)
LUMEN_SHADER_PARSER(shader_parse_vopc)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("vopc");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 17u) & 0xffu;
	uint32_t src0   = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0x1ffu;
	uint32_t vsrc1  = (ShaderReadSourceWord(buffer, 0) >> 9u) & 0xffu;

	bool sdwa = (src0 == 249);

	uint32_t size = (sdwa ? 2 : 1);

	src0               = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 0u) & 0xffu : src0);
	uint32_t sdst      = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 8u) & 0x7fu : 0);
	uint32_t sd        = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 15u) & 0x1u : 0);
	uint32_t src0_sel  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 16u) & 0x7u : 6);
	uint32_t src0_sext = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 19u) & 0x1u : 0);
	uint32_t src0_neg  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 20u) & 0x1u : 0);
	uint32_t src0_abs  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 21u) & 0x1u : 0);
	uint32_t s0        = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 23u) & 0x1u : 1);
	uint32_t src1_sel  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 24u) & 0x7u : 6);
	uint32_t src1_sext = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 27u) & 0x1u : 0);
	uint32_t src1_neg  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 28u) & 0x1u : 0);
	uint32_t src1_abs  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 29u) & 0x1u : 0);
	uint32_t s1        = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 31u) & 0x1u : 0);

	EXIT_NOT_IMPLEMENTED(src0_sel != 6);
	EXIT_NOT_IMPLEMENTED(src0_sext != 0);
	EXIT_NOT_IMPLEMENTED(src0_neg != 0);
	EXIT_NOT_IMPLEMENTED(src0_abs != 0);
	EXIT_NOT_IMPLEMENTED(src1_sel != 6);
	EXIT_NOT_IMPLEMENTED(src1_sext != 0);
	EXIT_NOT_IMPLEMENTED(src1_neg != 0);
	EXIT_NOT_IMPLEMENTED(src1_abs != 0);

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0 + (s0 == 0 ? 256 : 0));
	inst.src[1]  = operand_parse(vsrc1 + (s1 == 0 ? 256 : 0));
	inst.src_num = 2;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	inst.format = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
	if (sd == 0)
	{
		inst.dst.type = ShaderOperandType::VccLo;
	} else
	{
		inst.dst = operand_parse(sdst);
	}
	inst.dst.size = 2;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::VCmpFF32; break;
		case 0x01: inst.type = ShaderInstructionType::VCmpLtF32; break;
		case 0x02: inst.type = ShaderInstructionType::VCmpEqF32; break;
		case 0x03: inst.type = ShaderInstructionType::VCmpLeF32; break;
		case 0x04: inst.type = ShaderInstructionType::VCmpGtF32; break;
		case 0x05: inst.type = ShaderInstructionType::VCmpLgF32; break;
		case 0x06: inst.type = ShaderInstructionType::VCmpGeF32; break;
		case 0x07: inst.type = ShaderInstructionType::VCmpOF32; break;
		case 0x08: inst.type = ShaderInstructionType::VCmpUF32; break;
		case 0x09: inst.type = ShaderInstructionType::VCmpNgeF32; break;
		case 0x0a: inst.type = ShaderInstructionType::VCmpNlgF32; break;
		case 0x0b: inst.type = ShaderInstructionType::VCmpNgtF32; break;
		case 0x0c: inst.type = ShaderInstructionType::VCmpNleF32; break;
		case 0x0d: inst.type = ShaderInstructionType::VCmpNeqF32; break;
		case 0x0e: inst.type = ShaderInstructionType::VCmpNltF32; break;
		case 0x0f: inst.type = ShaderInstructionType::VCmpTruF32; break;
		case 0x10: LUMEN_NI("v_cmpx_f_f32"); break;
		case 0x11: inst.type = ShaderInstructionType::VCmpxLtF32; break;
		case 0x12: LUMEN_NI("v_cmpx_eq_f32"); break;
		case 0x13: LUMEN_NI("v_cmpx_le_f32"); break;
		case 0x14: inst.type = ShaderInstructionType::VCmpxGtF32; break;
		case 0x15: LUMEN_NI("v_cmpx_lg_f32"); break;
		case 0x16: LUMEN_NI("v_cmpx_ge_f32"); break;
		case 0x17: LUMEN_NI("v_cmpx_o_f32"); break;
		case 0x18: LUMEN_NI("v_cmpx_u_f32"); break;
		case 0x19: LUMEN_NI("v_cmpx_nge_f32"); break;
		case 0x1A: LUMEN_NI("v_cmpx_nlg_f32"); break;
		case 0x1B: LUMEN_NI("v_cmpx_ngt_f32"); break;
		case 0x1C: LUMEN_NI("v_cmpx_nle_f32"); break;
		case 0x1d: inst.type = ShaderInstructionType::VCmpxNeqF32; break;
		case 0x1E: LUMEN_NI("v_cmpx_nlt_f32"); break;
		case 0x1F: LUMEN_NI("v_cmpx_tru_f32"); break;
		case 0x20: LUMEN_NI("v_cmp_f_f64"); break;
		case 0x21: LUMEN_NI("v_cmp_lt_f64"); break;
		case 0x22: LUMEN_NI("v_cmp_eq_f64"); break;
		case 0x23: LUMEN_NI("v_cmp_le_f64"); break;
		case 0x24: LUMEN_NI("v_cmp_gt_f64"); break;
		case 0x25: LUMEN_NI("v_cmp_lg_f64"); break;
		case 0x26: LUMEN_NI("v_cmp_ge_f64"); break;
		case 0x27: LUMEN_NI("v_cmp_o_f64"); break;
		case 0x28: LUMEN_NI("v_cmp_u_f64"); break;
		case 0x29: LUMEN_NI("v_cmp_nge_f64"); break;
		case 0x2A: LUMEN_NI("v_cmp_nlg_f64"); break;
		case 0x2B: LUMEN_NI("v_cmp_ngt_f64"); break;
		case 0x2C: LUMEN_NI("v_cmp_nle_f64"); break;
		case 0x2D: LUMEN_NI("v_cmp_neq_f64"); break;
		case 0x2E: LUMEN_NI("v_cmp_nlt_f64"); break;
		case 0x2F: LUMEN_NI("v_cmp_tru_f64"); break;
		case 0x30: LUMEN_NI("v_cmpx_f_f64"); break;
		case 0x31: LUMEN_NI("v_cmpx_lt_f64"); break;
		case 0x32: LUMEN_NI("v_cmpx_eq_f64"); break;
		case 0x33: LUMEN_NI("v_cmpx_le_f64"); break;
		case 0x34: LUMEN_NI("v_cmpx_gt_f64"); break;
		case 0x35: LUMEN_NI("v_cmpx_lg_f64"); break;
		case 0x36: LUMEN_NI("v_cmpx_ge_f64"); break;
		case 0x37: LUMEN_NI("v_cmpx_o_f64"); break;
		case 0x38: LUMEN_NI("v_cmpx_u_f64"); break;
		case 0x39: LUMEN_NI("v_cmpx_nge_f64"); break;
		case 0x3A: LUMEN_NI("v_cmpx_nlg_f64"); break;
		case 0x3B: LUMEN_NI("v_cmpx_ngt_f64"); break;
		case 0x3C: LUMEN_NI("v_cmpx_nle_f64"); break;
		case 0x3D: LUMEN_NI("v_cmpx_neq_f64"); break;
		case 0x3E: LUMEN_NI("v_cmpx_nlt_f64"); break;
		case 0x3F: LUMEN_NI("v_cmpx_tru_f64"); break;
		case 0x40: LUMEN_NI("v_cmps_f_f32"); break;
		case 0x41: LUMEN_NI("v_cmps_lt_f32"); break;
		case 0x42: LUMEN_NI("v_cmps_eq_f32"); break;
		case 0x43: LUMEN_NI("v_cmps_le_f32"); break;
		case 0x44: LUMEN_NI("v_cmps_gt_f32"); break;
		case 0x45: LUMEN_NI("v_cmps_lg_f32"); break;
		case 0x46: LUMEN_NI("v_cmps_ge_f32"); break;
		case 0x47: LUMEN_NI("v_cmps_o_f32"); break;
		case 0x48: LUMEN_NI("v_cmps_u_f32"); break;
		case 0x49: LUMEN_NI("v_cmps_nge_f32"); break;
		case 0x4A: LUMEN_NI("v_cmps_nlg_f32"); break;
		case 0x4B: LUMEN_NI("v_cmps_ngt_f32"); break;
		case 0x4C: LUMEN_NI("v_cmps_nle_f32"); break;
		case 0x4D: LUMEN_NI("v_cmps_neq_f32"); break;
		case 0x4E: LUMEN_NI("v_cmps_nlt_f32"); break;
		case 0x4F: LUMEN_NI("v_cmps_tru_f32"); break;
		case 0x50: LUMEN_NI("v_cmpsx_f_f32"); break;
		case 0x51: LUMEN_NI("v_cmpsx_lt_f32"); break;
		case 0x52: LUMEN_NI("v_cmpsx_eq_f32"); break;
		case 0x53: LUMEN_NI("v_cmpsx_le_f32"); break;
		case 0x54: LUMEN_NI("v_cmpsx_gt_f32"); break;
		case 0x55: LUMEN_NI("v_cmpsx_lg_f32"); break;
		case 0x56: LUMEN_NI("v_cmpsx_ge_f32"); break;
		case 0x57: LUMEN_NI("v_cmpsx_o_f32"); break;
		case 0x58: LUMEN_NI("v_cmpsx_u_f32"); break;
		case 0x59: LUMEN_NI("v_cmpsx_nge_f32"); break;
		case 0x5A: LUMEN_NI("v_cmpsx_nlg_f32"); break;
		case 0x5B: LUMEN_NI("v_cmpsx_ngt_f32"); break;
		case 0x5C: LUMEN_NI("v_cmpsx_nle_f32"); break;
		case 0x5D: LUMEN_NI("v_cmpsx_neq_f32"); break;
		case 0x5E: LUMEN_NI("v_cmpsx_nlt_f32"); break;
		case 0x5F: LUMEN_NI("v_cmpsx_tru_f32"); break;
		case 0x60: LUMEN_NI("v_cmps_f_f64"); break;
		case 0x61: LUMEN_NI("v_cmps_lt_f64"); break;
		case 0x62: LUMEN_NI("v_cmps_eq_f64"); break;
		case 0x63: LUMEN_NI("v_cmps_le_f64"); break;
		case 0x64: LUMEN_NI("v_cmps_gt_f64"); break;
		case 0x65: LUMEN_NI("v_cmps_lg_f64"); break;
		case 0x66: LUMEN_NI("v_cmps_ge_f64"); break;
		case 0x67: LUMEN_NI("v_cmps_o_f64"); break;
		case 0x68: LUMEN_NI("v_cmps_u_f64"); break;
		case 0x69: LUMEN_NI("v_cmps_nge_f64"); break;
		case 0x6A: LUMEN_NI("v_cmps_nlg_f64"); break;
		case 0x6B: LUMEN_NI("v_cmps_ngt_f64"); break;
		case 0x6C: LUMEN_NI("v_cmps_nle_f64"); break;
		case 0x6D: LUMEN_NI("v_cmps_neq_f64"); break;
		case 0x6E: LUMEN_NI("v_cmps_nlt_f64"); break;
		case 0x6F: LUMEN_NI("v_cmps_tru_f64"); break;
		case 0x70: LUMEN_NI("v_cmpsx_f_f64"); break;
		case 0x71: LUMEN_NI("v_cmpsx_lt_f64"); break;
		case 0x72: LUMEN_NI("v_cmpsx_eq_f64"); break;
		case 0x73: LUMEN_NI("v_cmpsx_le_f64"); break;
		case 0x74: LUMEN_NI("v_cmpsx_gt_f64"); break;
		case 0x75: LUMEN_NI("v_cmpsx_lg_f64"); break;
		case 0x76: LUMEN_NI("v_cmpsx_ge_f64"); break;
		case 0x77: LUMEN_NI("v_cmpsx_o_f64"); break;
		case 0x78: LUMEN_NI("v_cmpsx_u_f64"); break;
		case 0x79: LUMEN_NI("v_cmpsx_nge_f64"); break;
		case 0x7A: LUMEN_NI("v_cmpsx_nlg_f64"); break;
		case 0x7B: LUMEN_NI("v_cmpsx_ngt_f64"); break;
		case 0x7C: LUMEN_NI("v_cmpsx_nle_f64"); break;
		case 0x7D: LUMEN_NI("v_cmpsx_neq_f64"); break;
		case 0x7E: LUMEN_NI("v_cmpsx_nlt_f64"); break;
		case 0x7F: LUMEN_NI("v_cmpsx_tru_f64"); break;
		case 0x80: inst.type = ShaderInstructionType::VCmpFI32; break;
		case 0x81: inst.type = ShaderInstructionType::VCmpLtI32; break;
		case 0x82: inst.type = ShaderInstructionType::VCmpEqI32; break;
		case 0x83: inst.type = ShaderInstructionType::VCmpLeI32; break;
		case 0x84: inst.type = ShaderInstructionType::VCmpGtI32; break;
		case 0x85: inst.type = ShaderInstructionType::VCmpNeI32; break;
		case 0x86: inst.type = ShaderInstructionType::VCmpGeI32; break;
		case 0x87: inst.type = ShaderInstructionType::VCmpTI32; break;
		case 0x88: LUMEN_NI("v_cmp_class_f32"); break;
		case 0x89: LUMEN_NI("v_cmp_lt_i16"); break;
		case 0x8A: LUMEN_NI("v_cmp_eq_i16"); break;
		case 0x8B: LUMEN_NI("v_cmp_le_i16"); break;
		case 0x8C: LUMEN_NI("v_cmp_gt_i16"); break;
		case 0x8D: LUMEN_NI("v_cmp_ne_i16"); break;
		case 0x8E: LUMEN_NI("v_cmp_ge_i16"); break;
		case 0x8F: LUMEN_NI("v_cmp_class_f16"); break;
		case 0x90: LUMEN_NI("v_cmpx_f_i32"); break;
		case 0x91: LUMEN_NI("v_cmpx_lt_i32"); break;
		case 0x92: LUMEN_NI("v_cmpx_eq_i32"); break;
		case 0x93: LUMEN_NI("v_cmpx_le_i32"); break;
		case 0x94: LUMEN_NI("v_cmpx_gt_i32"); break;
		case 0x95: LUMEN_NI("v_cmpx_ne_i32"); break;
		case 0x96: inst.type = ShaderInstructionType::VCmpxGeI32; break;
		case 0x97: LUMEN_NI("v_cmpx_t_i32"); break;
		case 0x98: LUMEN_NI("v_cmpx_class_f32"); break;
		case 0x99: LUMEN_NI("v_cmpx_lt_i16"); break;
		case 0x9A: LUMEN_NI("v_cmpx_eq_i16"); break;
		case 0x9B: LUMEN_NI("v_cmpx_le_i16"); break;
		case 0x9C: LUMEN_NI("v_cmpx_gt_i16"); break;
		case 0x9D: LUMEN_NI("v_cmpx_ne_i16"); break;
		case 0x9E: LUMEN_NI("v_cmpx_ge_i16"); break;
		case 0x9F: LUMEN_NI("v_cmpx_class_f16"); break;
		case 0xA0: LUMEN_NI("v_cmp_f_i64"); break;
		case 0xA1: LUMEN_NI("v_cmp_lt_i64"); break;
		case 0xA2: LUMEN_NI("v_cmp_eq_i64"); break;
		case 0xA3: LUMEN_NI("v_cmp_le_i64"); break;
		case 0xA4: LUMEN_NI("v_cmp_gt_i64"); break;
		case 0xA5: LUMEN_NI("v_cmp_ne_i64"); break;
		case 0xA6: LUMEN_NI("v_cmp_ge_i64"); break;
		case 0xA7: LUMEN_NI("v_cmp_t_i64"); break;
		case 0xA8: LUMEN_NI("v_cmp_class_f64"); break;
		case 0xA9: LUMEN_NI("v_cmp_lt_u16"); break;
		case 0xAA: LUMEN_NI("v_cmp_eq_u16"); break;
		case 0xAB: LUMEN_NI("v_cmp_le_u16"); break;
		case 0xAC: LUMEN_NI("v_cmp_gt_u16"); break;
		case 0xAD: LUMEN_NI("v_cmp_ne_u16"); break;
		case 0xAE: LUMEN_NI("v_cmp_ge_u16"); break;
		case 0xB0: LUMEN_NI("v_cmpx_f_i64"); break;
		case 0xB1: LUMEN_NI("v_cmpx_lt_i64"); break;
		case 0xB2: LUMEN_NI("v_cmpx_eq_i64"); break;
		case 0xB3: LUMEN_NI("v_cmpx_le_i64"); break;
		case 0xB4: LUMEN_NI("v_cmpx_gt_i64"); break;
		case 0xB5: LUMEN_NI("v_cmpx_ne_i64"); break;
		case 0xB6: LUMEN_NI("v_cmpx_ge_i64"); break;
		case 0xB7: LUMEN_NI("v_cmpx_t_i64"); break;
		case 0xB8: LUMEN_NI("v_cmpx_class_f64"); break;
		case 0xB9: LUMEN_NI("v_cmpx_lt_u16"); break;
		case 0xBA: LUMEN_NI("v_cmpx_eq_u16"); break;
		case 0xBB: LUMEN_NI("v_cmpx_le_u16"); break;
		case 0xBC: LUMEN_NI("v_cmpx_gt_u16"); break;
		case 0xBD: LUMEN_NI("v_cmpx_ne_u16"); break;
		case 0xBE: LUMEN_NI("v_cmpx_ge_u16"); break;
		case 0xc0: inst.type = ShaderInstructionType::VCmpFU32; break;
		case 0xc1: inst.type = ShaderInstructionType::VCmpLtU32; break;
		case 0xc2: inst.type = ShaderInstructionType::VCmpEqU32; break;
		case 0xc3: inst.type = ShaderInstructionType::VCmpLeU32; break;
		case 0xc4: inst.type = ShaderInstructionType::VCmpGtU32; break;
		case 0xc5: inst.type = ShaderInstructionType::VCmpNeU32; break;
		case 0xc6: inst.type = ShaderInstructionType::VCmpGeU32; break;
		case 0xc7: inst.type = ShaderInstructionType::VCmpTU32; break;
		case 0xC8: LUMEN_NI("v_cmp_f_f16"); break;
		case 0xC9: LUMEN_NI("v_cmp_lt_f16"); break;
		case 0xCA: LUMEN_NI("v_cmp_eq_f16"); break;
		case 0xCB: LUMEN_NI("v_cmp_le_f16"); break;
		case 0xCC: LUMEN_NI("v_cmp_gt_f16"); break;
		case 0xCD: LUMEN_NI("v_cmp_lg_f16"); break;
		case 0xCE: LUMEN_NI("v_cmp_ge_f16"); break;
		case 0xCF: LUMEN_NI("v_cmp_o_f16"); break;
		case 0xD0: LUMEN_NI("v_cmpx_f_u32"); break;
		case 0xD1: LUMEN_NI("v_cmpx_lt_u32"); break;
		case 0xd2: inst.type = ShaderInstructionType::VCmpxEqU32; break;
		case 0xD3: LUMEN_NI("v_cmpx_le_u32"); break;
		case 0xd4: inst.type = ShaderInstructionType::VCmpxGtU32; break;
		case 0xd5: inst.type = ShaderInstructionType::VCmpxNeU32; break;
		case 0xd6: inst.type = ShaderInstructionType::VCmpxGeU32; break;
		case 0xD7: LUMEN_NI("v_cmpx_t_u32"); break;
		case 0xD8: LUMEN_NI("v_cmpx_f_f16"); break;
		case 0xD9: LUMEN_NI("v_cmpx_lt_f16"); break;
		case 0xDA: LUMEN_NI("v_cmpx_eq_f16"); break;
		case 0xDB: LUMEN_NI("v_cmpx_le_f16"); break;
		case 0xDC: LUMEN_NI("v_cmpx_gt_f16"); break;
		case 0xDD: LUMEN_NI("v_cmpx_lg_f16"); break;
		case 0xDE: LUMEN_NI("v_cmpx_ge_f16"); break;
		case 0xDF: LUMEN_NI("v_cmpx_o_f16"); break;
		case 0xE0: LUMEN_NI("v_cmp_f_u64"); break;
		case 0xE1: LUMEN_NI("v_cmp_lt_u64"); break;
		case 0xE2: LUMEN_NI("v_cmp_eq_u64"); break;
		case 0xE3: LUMEN_NI("v_cmp_le_u64"); break;
		case 0xE4: LUMEN_NI("v_cmp_gt_u64"); break;
		case 0xE5: LUMEN_NI("v_cmp_ne_u64"); break;
		case 0xE6: LUMEN_NI("v_cmp_ge_u64"); break;
		case 0xE7: LUMEN_NI("v_cmp_t_u64"); break;
		case 0xE8: LUMEN_NI("v_cmp_u_f16"); break;
		case 0xE9: LUMEN_NI("v_cmp_nge_f16"); break;
		case 0xEA: LUMEN_NI("v_cmp_nlg_f16"); break;
		case 0xEB: LUMEN_NI("v_cmp_ngt_f16"); break;
		case 0xEC: LUMEN_NI("v_cmp_nle_f16"); break;
		case 0xED: LUMEN_NI("v_cmp_neq_f16"); break;
		case 0xEE: LUMEN_NI("v_cmp_nlt_f16"); break;
		case 0xEF: LUMEN_NI("v_cmp_tru_f16"); break;
		case 0xF0: LUMEN_NI("v_cmpx_f_u64"); break;
		case 0xF1: LUMEN_NI("v_cmpx_lt_u64"); break;
		case 0xF2: LUMEN_NI("v_cmpx_eq_u64"); break;
		case 0xF3: LUMEN_NI("v_cmpx_le_u64"); break;
		case 0xF4: LUMEN_NI("v_cmpx_gt_u64"); break;
		case 0xF5: LUMEN_NI("v_cmpx_ne_u64"); break;
		case 0xF6: LUMEN_NI("v_cmpx_ge_u64"); break;
		case 0xF7: LUMEN_NI("v_cmpx_t_u64"); break;
		case 0xF8: LUMEN_NI("v_cmpx_u_f16"); break;
		case 0xF9: LUMEN_NI("v_cmpx_nge_f16"); break;
		case 0xFA: LUMEN_NI("v_cmpx_nlg_f16"); break;
		case 0xFB: LUMEN_NI("v_cmpx_ngt_f16"); break;
		case 0xFC: LUMEN_NI("v_cmpx_nle_f16"); break;
		case 0xFD: LUMEN_NI("v_cmpx_neq_f16"); break;
		case 0xFE: LUMEN_NI("v_cmpx_nlt_f16"); break;
		case 0xFF: LUMEN_NI("v_cmpx_tru_f16"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_vop1)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("vop1");

	uint32_t vdst   = (ShaderReadSourceWord(buffer, 0) >> 17u) & 0xffu;
	uint32_t src0   = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0x1ffu;
	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 9u) & 0xffu;

	if (opcode == 0x0e && !((src0 >= 128u && src0 <= 135u) || (src0 >= 193u && src0 <= 200u) || src0 == 255u))
	{
		LUMEN_NI("v_cvt_off_f32_i4 (known integer constant profile)");
	}

	if (opcode == 0x2c && !ShaderVRsqClampF32FormSupported(*dst, src0, vdst + 256u))
	{
		if (!ShaderVRsqClampF32ExperimentEnabled())
		{
			LUMEN_NI("v_rsq_clamp_f32");
		}
		LUMEN_NI("v_rsq_clamp_f32 (unsupported form or state)");
	}

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0);
	inst.dst     = operand_parse(vdst + 256);
	inst.src_num = 1;

	uint32_t size = 1;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	inst.format = ShaderInstructionFormat::SVdstSVsrc0;

	switch (opcode)
	{
		case 0x00: LUMEN_NI("v_nop"); break;
		case 0x01: inst.type = ShaderInstructionType::VMovB32; break;
		case 0x02: LUMEN_NI("v_readfirstlane_b32"); break;
		case 0x03: LUMEN_NI("v_cvt_i32_f64"); break;
		case 0x04: LUMEN_NI("v_cvt_f64_i32"); break;
		case 0x05: inst.type = ShaderInstructionType::VCvtF32I32; break;
		case 0x06: inst.type = ShaderInstructionType::VCvtF32U32; break;
		case 0x07: inst.type = ShaderInstructionType::VCvtU32F32; break;
		case 0x08: inst.type = ShaderInstructionType::VCvtI32F32; break;
		case 0x09: LUMEN_NI("v_mov_fed_b32"); break;
		case 0x0A: LUMEN_NI("v_cvt_f16_f32"); break;
		case 0x0b: inst.type = ShaderInstructionType::VCvtF32F16; break;
		case 0x0C: LUMEN_NI("v_cvt_rpi_i32_f32"); break;
		case 0x0D: LUMEN_NI("v_cvt_flr_i32_f32"); break;
		case 0x0e:
			inst.type = ShaderInstructionType::VCvtOffF32I4;
			if (!ShaderCvtOffF32I4InstructionSupported(inst))
			{
				LUMEN_NI("v_cvt_off_f32_i4 (known integer constant profile)");
			}
			break;
		case 0x0F: LUMEN_NI("v_cvt_f32_f64"); break;
		case 0x10: LUMEN_NI("v_cvt_f64_f32"); break;
		case 0x11: inst.type = ShaderInstructionType::VCvtF32Ubyte0; break;
		case 0x12: inst.type = ShaderInstructionType::VCvtF32Ubyte1; break;
		case 0x13: inst.type = ShaderInstructionType::VCvtF32Ubyte2; break;
		case 0x14: inst.type = ShaderInstructionType::VCvtF32Ubyte3; break;
		case 0x15: LUMEN_NI("v_cvt_u32_f64"); break;
		case 0x16: LUMEN_NI("v_cvt_f64_u32"); break;
		case 0x17: LUMEN_NI("v_trunc_f64"); break;
		case 0x18: LUMEN_NI("v_ceil_f64"); break;
		case 0x19: LUMEN_NI("v_rndne_f64"); break;
		case 0x1A: LUMEN_NI("v_floor_f64"); break;
		case 0x20: inst.type = ShaderInstructionType::VFractF32; break;
		case 0x21: inst.type = ShaderInstructionType::VTruncF32; break;
		case 0x22: inst.type = ShaderInstructionType::VCeilF32; break;
		case 0x23: inst.type = ShaderInstructionType::VRndneF32; break;
		case 0x24: inst.type = ShaderInstructionType::VFloorF32; break;
		case 0x25: inst.type = ShaderInstructionType::VExpF32; break;
		case 0x26: LUMEN_NI("v_log_clamp_f32"); break;
		case 0x27: inst.type = ShaderInstructionType::VLogF32; break;
		case 0x28: LUMEN_NI("v_rcp_clamp_f32"); break;
		case 0x29: LUMEN_NI("v_rcp_legacy_f32"); break;
		case 0x2a: inst.type = ShaderInstructionType::VRcpF32; break;
		case 0x2B: LUMEN_NI("v_rcp_iflag_f32"); break;
		case 0x2C: inst.type = ShaderInstructionType::VRsqClampF32; break;
		case 0x2D: LUMEN_NI("v_rsq_legacy_f32"); break;
		case 0x2e: inst.type = ShaderInstructionType::VRsqF32; break;
		case 0x2F: LUMEN_NI("v_rcp_f64"); break;
		case 0x30: LUMEN_NI("v_rcp_clamp_f64"); break;
		case 0x31: LUMEN_NI("v_rsq_f64"); break;
		case 0x32: LUMEN_NI("v_rsq_clamp_f64"); break;
		case 0x33: inst.type = ShaderInstructionType::VSqrtF32; break;
		case 0x34: LUMEN_NI("v_sqrt_f64"); break;
		case 0x35: inst.type = ShaderInstructionType::VSinF32; break;
		case 0x36: inst.type = ShaderInstructionType::VCosF32; break;
		case 0x37: inst.type = ShaderInstructionType::VNotB32; break;
		case 0x38: inst.type = ShaderInstructionType::VBfrevB32; break;
		case 0x39: LUMEN_NI("v_ffbh_u32"); break;
		case 0x3A: LUMEN_NI("v_ffbl_b32"); break;
		case 0x3B: LUMEN_NI("v_ffbh_i32"); break;
		case 0x3C: LUMEN_NI("v_frexp_exp_i32_f64"); break;
		case 0x3D: LUMEN_NI("v_frexp_mant_f64"); break;
		case 0x3E: LUMEN_NI("v_fract_f64"); break;
		case 0x3F: LUMEN_NI("v_frexp_exp_i32_f32"); break;
		case 0x40: LUMEN_NI("v_frexp_mant_f32"); break;
		case 0x41: LUMEN_NI("v_clrexcp"); break;
		case 0x42: LUMEN_NI("v_movreld_b32"); break;
		case 0x43: LUMEN_NI("v_movrels_b32"); break;
		case 0x44: LUMEN_NI("v_movrelsd_b32"); break;
		case 0x45: LUMEN_NI("v_log_legacy_f32"); break;
		case 0x46: LUMEN_NI("v_exp_legacy_f32"); break;
		case 0x50: LUMEN_NI("v_cvt_f16_u16"); break;
		case 0x51: LUMEN_NI("v_cvt_f16_i16"); break;
		case 0x52: LUMEN_NI("v_cvt_u16_f16"); break;
		case 0x53: LUMEN_NI("v_cvt_i16_f16"); break;
		case 0x54: LUMEN_NI("v_rcp_f16"); break;
		case 0x55: LUMEN_NI("v_sqrt_f16"); break;
		case 0x56: LUMEN_NI("v_rsq_f16"); break;
		case 0x57: LUMEN_NI("v_log_f16"); break;
		case 0x58: LUMEN_NI("v_exp_f16"); break;
		case 0x59: LUMEN_NI("v_frexp_mant_f16"); break;
		case 0x5A: LUMEN_NI("v_frexp_exp_i16_f16"); break;
		case 0x5B: LUMEN_NI("v_floor_f16"); break;
		case 0x5C: LUMEN_NI("v_ceil_f16"); break;
		case 0x5D: LUMEN_NI("v_trunc_f16"); break;
		case 0x5E: LUMEN_NI("v_rndne_f16"); break;
		case 0x5F: LUMEN_NI("v_fract_f16"); break;
		case 0x60: LUMEN_NI("v_sin_f16"); break;
		case 0x61: LUMEN_NI("v_cos_f16"); break;
		case 0x62: LUMEN_NI("v_sat_pk_u8_i16"); break;
		case 0x63: LUMEN_NI("v_cvt_norm_i16_f16"); break;
		case 0x64: LUMEN_NI("v_cvt_norm_u16_f16"); break;
		case 0x65: LUMEN_NI("v_swap_b32"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_vop2)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("vop2");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 25u) & 0x3fu;

	switch (opcode)
	{
		case 0x3e: return shader_parse_vopc(pc, src, buffer, dst); break;
		case 0x3f: return shader_parse_vop1(pc, src, buffer, dst); break;
		default: break;
	}

	uint32_t vdst  = (ShaderReadSourceWord(buffer, 0) >> 17u) & 0xffu;
	uint32_t src0  = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0x1ffu;
	uint32_t vsrc1 = (ShaderReadSourceWord(buffer, 0) >> 9u) & 0xffu;

	bool sdwa = (src0 == 249);
	bool dpp  = (src0 == 250);

	if (opcode == 0x06 &&
	    !ShaderVMacLegacyF32FormSupported(*dst, src0, vsrc1 + 256u, vdst + 256u, sdwa, dpp, 0, 0, 0, false))
	{
		if (!ShaderVMacLegacyF32ExperimentEnabled())
		{
			LUMEN_NI("v_mac_legacy_f32");
		}
		LUMEN_NI("v_mac_legacy_f32 (unsupported form or state)");
	}
	if (opcode == 0x07 && !ShaderVMulLegacyF32FormSupported(*dst, sdwa, dpp, 0, 0, 0, false, 0))
	{
		LUMEN_NI("v_mul_legacy_f32 (unsupported form or FP mode)");
	}
	if (opcode == 0x13 && (sdwa || dpp))
	{
		LUMEN_NI("v_min_u32 (unsupported form)");
	}

	uint32_t size = (sdwa ? 2 : 1);

	src0               = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 0u) & 0xffu : src0);
	uint32_t dst_sel   = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 8u) & 0x7u : 6);
	uint32_t dst_u     = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 11u) & 0x3u : 2);
	uint32_t clmp      = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 13u) & 0x1u : 0);
	uint32_t omod      = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 14u) & 0x3u : 0);
	uint32_t src0_sel  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 16u) & 0x7u : 6);
	uint32_t src0_sext = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 19u) & 0x1u : 0);
	uint32_t src0_neg  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 20u) & 0x1u : 0);
	uint32_t src0_abs  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 21u) & 0x1u : 0);
	uint32_t s0        = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 23u) & 0x1u : 1);
	uint32_t src1_sel  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 24u) & 0x7u : 6);
	uint32_t src1_sext = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 27u) & 0x1u : 0);
	uint32_t src1_neg  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 28u) & 0x1u : 0);
	uint32_t src1_abs  = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 29u) & 0x1u : 0);
	uint32_t s1        = (sdwa ? (ShaderReadSourceWord(buffer, 1) >> 31u) & 0x1u : 0);

	EXIT_NOT_IMPLEMENTED(dst_sel != 6);
	EXIT_NOT_IMPLEMENTED(sdwa && dst_sel == 6 && dst_u != 0);
	EXIT_NOT_IMPLEMENTED(omod != 0);
	EXIT_NOT_IMPLEMENTED(src0_sel != 6);
	EXIT_NOT_IMPLEMENTED(src0_sext != 0);
	EXIT_NOT_IMPLEMENTED(src0_neg != 0);
	EXIT_NOT_IMPLEMENTED(src1_sel != 6);
	EXIT_NOT_IMPLEMENTED(src1_sext != 0);
	EXIT_NOT_IMPLEMENTED(src1_neg != 0);

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0 + (s0 == 0 ? 256 : 0));
	inst.src[1]  = operand_parse(vsrc1 + (s1 == 0 ? 256 : 0));
	inst.dst     = operand_parse(vdst + 256);
	inst.src_num = 2;

	switch (omod)
	{
		case 0: inst.dst.multiplier = 1.0f; break;
		case 1: inst.dst.multiplier = 2.0f; break;
		case 2: inst.dst.multiplier = 4.0f; break;
		case 3: inst.dst.multiplier = 0.5f; break;
		default: break;
	}

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	inst.src[0].absolute = (src0_abs != 0);
	inst.src[1].absolute = (src1_abs != 0);

	inst.dst.clamp = (clmp != 0);

	inst.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;

	switch (opcode)
	{
		case 0x00:
			inst.type        = ShaderInstructionType::VCndmaskB32;
			inst.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
			inst.src[2].type = ShaderOperandType::VccLo;
			inst.src[2].size = 2;
			inst.src_num     = 3;
			break;
		case 0x01: LUMEN_NI("v_readlane_b32"); break;
		case 0x02: LUMEN_NI("v_writelane_b32"); break;
		case 0x03: inst.type = ShaderInstructionType::VAddF32; break;
		case 0x04: inst.type = ShaderInstructionType::VSubF32; break;
		case 0x05: inst.type = ShaderInstructionType::VSubrevF32; break;
		case 0x06: inst.type = ShaderInstructionType::VMacLegacyF32; break;
		case 0x07: inst.type = ShaderInstructionType::VMulLegacyF32; break;
		case 0x08: inst.type = ShaderInstructionType::VMulF32; break;
		case 0x09: LUMEN_NI("v_mul_i32_i24"); break;
		case 0x0A: LUMEN_NI("v_mul_hi_i32_i24"); break;
		case 0x0C: LUMEN_NI("v_mul_hi_u32_u24"); break;
		case 0x0D: LUMEN_NI("v_min_legacy_f32"); break;
		case 0x0E: LUMEN_NI("v_max_legacy_f32"); break;
		case 0x0b: inst.type = ShaderInstructionType::VMulU32U24; break;
		case 0x0f: inst.type = ShaderInstructionType::VMinF32; break;
		case 0x10: inst.type = ShaderInstructionType::VMaxF32; break;
		case 0x11: LUMEN_NI("v_min_i32"); break;
		case 0x12: LUMEN_NI("v_max_i32"); break;
		case 0x13: inst.type = ShaderInstructionType::VMinU32; break;
		case 0x14: LUMEN_NI("v_max_u32"); break;
		case 0x15: inst.type = ShaderInstructionType::VLshrB32; break;
		case 0x16: inst.type = ShaderInstructionType::VLshrrevB32; break;
		case 0x17: inst.type = ShaderInstructionType::VAshrI32; break;
		case 0x18: inst.type = ShaderInstructionType::VAshrrevI32; break;
		case 0x19: inst.type = ShaderInstructionType::VLshlB32; break;
		case 0x1a: inst.type = ShaderInstructionType::VLshlrevB32; break;
		case 0x1b: inst.type = ShaderInstructionType::VAndB32; break;
		case 0x1c: inst.type = ShaderInstructionType::VOrB32; break;
		case 0x1d: inst.type = ShaderInstructionType::VXorB32; break;
		case 0x1E: inst.type = ShaderInstructionType::VBfmB32; break;
		case 0x1f: inst.type = ShaderInstructionType::VMacF32; break;
		case 0x20:
			inst.type              = ShaderInstructionType::VMadmkF32;
			inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
			inst.src_num           = 3;
			inst.src[2]            = inst.src[1];
			inst.src[1].type       = ShaderOperandType::LiteralConstant;
			inst.src[1].constant.u = ShaderReadSourceWord(buffer, size);
			inst.src[1].size       = 0;
			size++;
			break;
		case 0x21:
			inst.type              = ShaderInstructionType::VMadakF32;
			inst.format            = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;
			inst.src_num           = 3;
			inst.src[2].type       = ShaderOperandType::LiteralConstant;
			inst.src[2].constant.u = ShaderReadSourceWord(buffer, size);
			inst.src[2].size       = 0;
			size++;
			break;
		case 0x22: inst.type = ShaderInstructionType::VBcntU32B32; break;
		case 0x23: inst.type = ShaderInstructionType::VMbcntLoU32B32; break;
		case 0x24: inst.type = ShaderInstructionType::VMbcntHiU32B32; break;
		case 0x25:
			inst.type      = ShaderInstructionType::VAddI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2.type = ShaderOperandType::VccLo;
			inst.dst2.size = 2;
			break;
		case 0x26:
			inst.type      = ShaderInstructionType::VSubI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2.type = ShaderOperandType::VccLo;
			inst.dst2.size = 2;
			break;
		case 0x27:
			inst.type      = ShaderInstructionType::VSubrevI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2.type = ShaderOperandType::VccLo;
			inst.dst2.size = 2;
			break;
		case 0x28: LUMEN_NI("v_addc_u32"); break;
		case 0x29: LUMEN_NI("v_subb_u32"); break;
		case 0x2A: LUMEN_NI("v_subbrev_u32"); break;
		case 0x2B: LUMEN_NI("v_ldexp_f32"); break;
		case 0x2C: LUMEN_NI("v_cvt_pkaccum_u8_f32"); break;
		case 0x2D: LUMEN_NI("v_cvt_pknorm_i16_f32"); break;
		case 0x2E: LUMEN_NI("v_cvt_pknorm_u16_f32"); break;
		case 0x2f: inst.type = ShaderInstructionType::VCvtPkrtzF16F32; break;
		case 0x30: LUMEN_NI("v_cvt_pk_u16_u32"); break;
		case 0x31: LUMEN_NI("v_cvt_pk_i16_i32"); break;
		case 0x32: LUMEN_NI("v_add_f16"); break;
		case 0x33: LUMEN_NI("v_sub_f16"); break;
		case 0x34: LUMEN_NI("v_subrev_f16"); break;
		case 0x35: LUMEN_NI("v_mul_f16"); break;
		case 0x36: LUMEN_NI("v_mac_f16"); break;
		case 0x37: LUMEN_NI("v_madmk_f16"); break;
		case 0x38: LUMEN_NI("v_madak_f16"); break;
		case 0x39: LUMEN_NI("v_max_f16"); break;
		case 0x3A: LUMEN_NI("v_min_f16"); break;
		case 0x3B: LUMEN_NI("v_ldexp_f16"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity,readability-function-size,google-readability-function-size,hicpp-function-size)
LUMEN_SHADER_PARSER(shader_parse_vop3)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("vop3");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 17u) & 0x1ffu;
	uint32_t clamp  = (ShaderReadSourceWord(buffer, 0) >> 11u) & 0x1u;
	uint32_t op_sel = 0;
	uint32_t abs    = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0x7u;
	uint32_t vdst   = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xffu;
	uint32_t sdst   = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0x7fu;
	uint32_t neg    = (ShaderReadSourceWord(buffer, 1) >> 29u) & 0x7u;
	uint32_t omod   = (ShaderReadSourceWord(buffer, 1) >> 27u) & 0x3u;
	uint32_t src0   = (ShaderReadSourceWord(buffer, 1) >> 0u) & 0x1ffu;
	uint32_t src1   = (ShaderReadSourceWord(buffer, 1) >> 9u) & 0x1ffu;
	uint32_t src2   = (ShaderReadSourceWord(buffer, 1) >> 18u) & 0x1ffu;

	EXIT_NOT_IMPLEMENTED(op_sel != 0);

	if (opcode == 0x107)
	{
		const bool sdwa = src0 == 249 || src1 == 249 || src2 == 249;
		const bool dpp  = src0 == 250 || src1 == 250 || src2 == 250;
		if (!ShaderVMulLegacyF32FormSupported(*dst, sdwa, dpp, abs, neg, omod, clamp != 0, src2))
		{
			LUMEN_NI("v_mul_legacy_f32 (unsupported form or FP mode)");
		}
	}

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(src0);
	inst.src[1]  = operand_parse(src1);
	inst.src[2]  = operand_parse(src2);
	inst.src_num = 3;
	inst.dst     = operand_parse(vdst + 256);

	switch (omod)
	{
		case 0: inst.dst.multiplier = 1.0f; break;
		case 1: inst.dst.multiplier = 2.0f; break;
		case 2: inst.dst.multiplier = 4.0f; break;
		case 3: inst.dst.multiplier = 0.5f; break;
		default: break;
	}

	if ((neg & 0x1u) != 0)
	{
		inst.src[0].negate = true;
	}
	if ((neg & 0x2u) != 0)
	{
		inst.src[1].negate = true;
	}
	if ((neg & 0x4u) != 0)
	{
		inst.src[2].negate = true;
	}

	uint32_t size = 2;

	if (inst.src[0].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[0].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	if (inst.src[1].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[1].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	if (inst.src[2].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[2].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	inst.format = ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2;

	if (opcode >= 0 && opcode <= 0xff)
	{
		/* VOPC using VOP3 encoding */
		inst.format   = ShaderInstructionFormat::SmaskVsrc0Vsrc1;
		inst.src_num  = 2;
		inst.dst      = operand_parse(vdst);
		inst.dst.size = 2;
	}

	if (opcode >= 0x100 && opcode <= 0x13d)
	{
		/* VOP2 using VOP3 encoding */
		inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
		inst.src_num = 2;
	}

	if (opcode >= 0x180 && opcode <= 0x1e8)
	{
		/* VOP1 using VOP3 encoding */
		inst.format  = ShaderInstructionFormat::SVdstSVsrc0;
		inst.src_num = 1;
	}

	switch (opcode)
	{
		/* VOPC using VOP3 encoding */
		case 0x00: inst.type = ShaderInstructionType::VCmpFF32; break;
		case 0x01: inst.type = ShaderInstructionType::VCmpLtF32; break;
		case 0x02: inst.type = ShaderInstructionType::VCmpEqF32; break;
		case 0x03: inst.type = ShaderInstructionType::VCmpLeF32; break;
		case 0x04: inst.type = ShaderInstructionType::VCmpGtF32; break;
		case 0x05: inst.type = ShaderInstructionType::VCmpLgF32; break;
		case 0x06: inst.type = ShaderInstructionType::VCmpGeF32; break;
		case 0x07: inst.type = ShaderInstructionType::VCmpOF32; break;
		case 0x08: inst.type = ShaderInstructionType::VCmpUF32; break;
		case 0x09: inst.type = ShaderInstructionType::VCmpNgeF32; break;
		case 0x0a: inst.type = ShaderInstructionType::VCmpNlgF32; break;
		case 0x0b: inst.type = ShaderInstructionType::VCmpNgtF32; break;
		case 0x0c: inst.type = ShaderInstructionType::VCmpNleF32; break;
		case 0x0d: inst.type = ShaderInstructionType::VCmpNeqF32; break;
		case 0x0e: inst.type = ShaderInstructionType::VCmpNltF32; break;
		case 0x0f: inst.type = ShaderInstructionType::VCmpTruF32; break;
		case 0x10: LUMEN_NI("v_cmpx_f_f32"); break;
		case 0x11: LUMEN_NI("v_cmpx_lt_f32"); break;
		case 0x12: LUMEN_NI("v_cmpx_eq_f32"); break;
		case 0x13: LUMEN_NI("v_cmpx_le_f32"); break;
		case 0x14: LUMEN_NI("v_cmpx_gt_f32"); break;
		case 0x15: LUMEN_NI("v_cmpx_lg_f32"); break;
		case 0x16: LUMEN_NI("v_cmpx_ge_f32"); break;
		case 0x17: LUMEN_NI("v_cmpx_o_f32"); break;
		case 0x18: LUMEN_NI("v_cmpx_u_f32"); break;
		case 0x19: LUMEN_NI("v_cmpx_nge_f32"); break;
		case 0x1A: LUMEN_NI("v_cmpx_nlg_f32"); break;
		case 0x1B: LUMEN_NI("v_cmpx_ngt_f32"); break;
		case 0x1C: LUMEN_NI("v_cmpx_nle_f32"); break;
		case 0x1d: inst.type = ShaderInstructionType::VCmpxNeqF32; break;
		case 0x1E: LUMEN_NI("v_cmpx_nlt_f32"); break;
		case 0x1F: LUMEN_NI("v_cmpx_tru_f32"); break;
		case 0x20: LUMEN_NI("v_cmp_f_f64"); break;
		case 0x21: LUMEN_NI("v_cmp_lt_f64"); break;
		case 0x22: LUMEN_NI("v_cmp_eq_f64"); break;
		case 0x23: LUMEN_NI("v_cmp_le_f64"); break;
		case 0x24: LUMEN_NI("v_cmp_gt_f64"); break;
		case 0x25: LUMEN_NI("v_cmp_lg_f64"); break;
		case 0x26: LUMEN_NI("v_cmp_ge_f64"); break;
		case 0x27: LUMEN_NI("v_cmp_o_f64"); break;
		case 0x28: LUMEN_NI("v_cmp_u_f64"); break;
		case 0x29: LUMEN_NI("v_cmp_nge_f64"); break;
		case 0x2A: LUMEN_NI("v_cmp_nlg_f64"); break;
		case 0x2B: LUMEN_NI("v_cmp_ngt_f64"); break;
		case 0x2C: LUMEN_NI("v_cmp_nle_f64"); break;
		case 0x2D: LUMEN_NI("v_cmp_neq_f64"); break;
		case 0x2E: LUMEN_NI("v_cmp_nlt_f64"); break;
		case 0x2F: LUMEN_NI("v_cmp_tru_f64"); break;
		case 0x30: LUMEN_NI("v_cmpx_f_f64"); break;
		case 0x31: LUMEN_NI("v_cmpx_lt_f64"); break;
		case 0x32: LUMEN_NI("v_cmpx_eq_f64"); break;
		case 0x33: LUMEN_NI("v_cmpx_le_f64"); break;
		case 0x34: LUMEN_NI("v_cmpx_gt_f64"); break;
		case 0x35: LUMEN_NI("v_cmpx_lg_f64"); break;
		case 0x36: LUMEN_NI("v_cmpx_ge_f64"); break;
		case 0x37: LUMEN_NI("v_cmpx_o_f64"); break;
		case 0x38: LUMEN_NI("v_cmpx_u_f64"); break;
		case 0x39: LUMEN_NI("v_cmpx_nge_f64"); break;
		case 0x3A: LUMEN_NI("v_cmpx_nlg_f64"); break;
		case 0x3B: LUMEN_NI("v_cmpx_ngt_f64"); break;
		case 0x3C: LUMEN_NI("v_cmpx_nle_f64"); break;
		case 0x3D: LUMEN_NI("v_cmpx_neq_f64"); break;
		case 0x3E: LUMEN_NI("v_cmpx_nlt_f64"); break;
		case 0x3F: LUMEN_NI("v_cmpx_tru_f64"); break;
		case 0x40: LUMEN_NI("v_cmps_f_f32"); break;
		case 0x41: LUMEN_NI("v_cmps_lt_f32"); break;
		case 0x42: LUMEN_NI("v_cmps_eq_f32"); break;
		case 0x43: LUMEN_NI("v_cmps_le_f32"); break;
		case 0x44: LUMEN_NI("v_cmps_gt_f32"); break;
		case 0x45: LUMEN_NI("v_cmps_lg_f32"); break;
		case 0x46: LUMEN_NI("v_cmps_ge_f32"); break;
		case 0x47: LUMEN_NI("v_cmps_o_f32"); break;
		case 0x48: LUMEN_NI("v_cmps_u_f32"); break;
		case 0x49: LUMEN_NI("v_cmps_nge_f32"); break;
		case 0x4A: LUMEN_NI("v_cmps_nlg_f32"); break;
		case 0x4B: LUMEN_NI("v_cmps_ngt_f32"); break;
		case 0x4C: LUMEN_NI("v_cmps_nle_f32"); break;
		case 0x4D: LUMEN_NI("v_cmps_neq_f32"); break;
		case 0x4E: LUMEN_NI("v_cmps_nlt_f32"); break;
		case 0x4F: LUMEN_NI("v_cmps_tru_f32"); break;
		case 0x50: LUMEN_NI("v_cmpsx_f_f32"); break;
		case 0x51: LUMEN_NI("v_cmpsx_lt_f32"); break;
		case 0x52: LUMEN_NI("v_cmpsx_eq_f32"); break;
		case 0x53: LUMEN_NI("v_cmpsx_le_f32"); break;
		case 0x54: LUMEN_NI("v_cmpsx_gt_f32"); break;
		case 0x55: LUMEN_NI("v_cmpsx_lg_f32"); break;
		case 0x56: LUMEN_NI("v_cmpsx_ge_f32"); break;
		case 0x57: LUMEN_NI("v_cmpsx_o_f32"); break;
		case 0x58: LUMEN_NI("v_cmpsx_u_f32"); break;
		case 0x59: LUMEN_NI("v_cmpsx_nge_f32"); break;
		case 0x5A: LUMEN_NI("v_cmpsx_nlg_f32"); break;
		case 0x5B: LUMEN_NI("v_cmpsx_ngt_f32"); break;
		case 0x5C: LUMEN_NI("v_cmpsx_nle_f32"); break;
		case 0x5D: LUMEN_NI("v_cmpsx_neq_f32"); break;
		case 0x5E: LUMEN_NI("v_cmpsx_nlt_f32"); break;
		case 0x5F: LUMEN_NI("v_cmpsx_tru_f32"); break;
		case 0x60: LUMEN_NI("v_cmps_f_f64"); break;
		case 0x61: LUMEN_NI("v_cmps_lt_f64"); break;
		case 0x62: LUMEN_NI("v_cmps_eq_f64"); break;
		case 0x63: LUMEN_NI("v_cmps_le_f64"); break;
		case 0x64: LUMEN_NI("v_cmps_gt_f64"); break;
		case 0x65: LUMEN_NI("v_cmps_lg_f64"); break;
		case 0x66: LUMEN_NI("v_cmps_ge_f64"); break;
		case 0x67: LUMEN_NI("v_cmps_o_f64"); break;
		case 0x68: LUMEN_NI("v_cmps_u_f64"); break;
		case 0x69: LUMEN_NI("v_cmps_nge_f64"); break;
		case 0x6A: LUMEN_NI("v_cmps_nlg_f64"); break;
		case 0x6B: LUMEN_NI("v_cmps_ngt_f64"); break;
		case 0x6C: LUMEN_NI("v_cmps_nle_f64"); break;
		case 0x6D: LUMEN_NI("v_cmps_neq_f64"); break;
		case 0x6E: LUMEN_NI("v_cmps_nlt_f64"); break;
		case 0x6F: LUMEN_NI("v_cmps_tru_f64"); break;
		case 0x70: LUMEN_NI("v_cmpsx_f_f64"); break;
		case 0x71: LUMEN_NI("v_cmpsx_lt_f64"); break;
		case 0x72: LUMEN_NI("v_cmpsx_eq_f64"); break;
		case 0x73: LUMEN_NI("v_cmpsx_le_f64"); break;
		case 0x74: LUMEN_NI("v_cmpsx_gt_f64"); break;
		case 0x75: LUMEN_NI("v_cmpsx_lg_f64"); break;
		case 0x76: LUMEN_NI("v_cmpsx_ge_f64"); break;
		case 0x77: LUMEN_NI("v_cmpsx_o_f64"); break;
		case 0x78: LUMEN_NI("v_cmpsx_u_f64"); break;
		case 0x79: LUMEN_NI("v_cmpsx_nge_f64"); break;
		case 0x7A: LUMEN_NI("v_cmpsx_nlg_f64"); break;
		case 0x7B: LUMEN_NI("v_cmpsx_ngt_f64"); break;
		case 0x7C: LUMEN_NI("v_cmpsx_nle_f64"); break;
		case 0x7D: LUMEN_NI("v_cmpsx_neq_f64"); break;
		case 0x7E: LUMEN_NI("v_cmpsx_nlt_f64"); break;
		case 0x7F: LUMEN_NI("v_cmpsx_tru_f64"); break;
		case 0x80: inst.type = ShaderInstructionType::VCmpFI32; break;
		case 0x81: inst.type = ShaderInstructionType::VCmpLtI32; break;
		case 0x82: inst.type = ShaderInstructionType::VCmpEqI32; break;
		case 0x83: inst.type = ShaderInstructionType::VCmpLeI32; break;
		case 0x84: inst.type = ShaderInstructionType::VCmpGtI32; break;
		case 0x85: inst.type = ShaderInstructionType::VCmpNeI32; break;
		case 0x86: inst.type = ShaderInstructionType::VCmpGeI32; break;
		case 0x87: inst.type = ShaderInstructionType::VCmpTI32; break;
		case 0x88: LUMEN_NI("v_cmp_class_f32"); break;
		case 0x89: LUMEN_NI("v_cmp_lt_i16"); break;
		case 0x8A: LUMEN_NI("v_cmp_eq_i16"); break;
		case 0x8B: LUMEN_NI("v_cmp_le_i16"); break;
		case 0x8C: LUMEN_NI("v_cmp_gt_i16"); break;
		case 0x8D: LUMEN_NI("v_cmp_ne_i16"); break;
		case 0x8E: LUMEN_NI("v_cmp_ge_i16"); break;
		case 0x8F: LUMEN_NI("v_cmp_class_f16"); break;
		case 0x90: LUMEN_NI("v_cmpx_f_i32"); break;
		case 0x91: LUMEN_NI("v_cmpx_lt_i32"); break;
		case 0x92: LUMEN_NI("v_cmpx_eq_i32"); break;
		case 0x93: LUMEN_NI("v_cmpx_le_i32"); break;
		case 0x94: LUMEN_NI("v_cmpx_gt_i32"); break;
		case 0x95: LUMEN_NI("v_cmpx_ne_i32"); break;
		case 0x96: inst.type = ShaderInstructionType::VCmpxGeI32; break;
		case 0x97: LUMEN_NI("v_cmpx_t_i32"); break;
		case 0x98: LUMEN_NI("v_cmpx_class_f32"); break;
		case 0x99: LUMEN_NI("v_cmpx_lt_i16"); break;
		case 0x9A: LUMEN_NI("v_cmpx_eq_i16"); break;
		case 0x9B: LUMEN_NI("v_cmpx_le_i16"); break;
		case 0x9C: LUMEN_NI("v_cmpx_gt_i16"); break;
		case 0x9D: LUMEN_NI("v_cmpx_ne_i16"); break;
		case 0x9E: LUMEN_NI("v_cmpx_ge_i16"); break;
		case 0x9F: LUMEN_NI("v_cmpx_class_f16"); break;
		case 0xA0: LUMEN_NI("v_cmp_f_i64"); break;
		case 0xA1: LUMEN_NI("v_cmp_lt_i64"); break;
		case 0xA2: LUMEN_NI("v_cmp_eq_i64"); break;
		case 0xA3: LUMEN_NI("v_cmp_le_i64"); break;
		case 0xA4: LUMEN_NI("v_cmp_gt_i64"); break;
		case 0xA5: LUMEN_NI("v_cmp_ne_i64"); break;
		case 0xA6: LUMEN_NI("v_cmp_ge_i64"); break;
		case 0xA7: LUMEN_NI("v_cmp_t_i64"); break;
		case 0xA8: LUMEN_NI("v_cmp_class_f64"); break;
		case 0xA9: LUMEN_NI("v_cmp_lt_u16"); break;
		case 0xAA: LUMEN_NI("v_cmp_eq_u16"); break;
		case 0xAB: LUMEN_NI("v_cmp_le_u16"); break;
		case 0xAC: LUMEN_NI("v_cmp_gt_u16"); break;
		case 0xAD: LUMEN_NI("v_cmp_ne_u16"); break;
		case 0xAE: LUMEN_NI("v_cmp_ge_u16"); break;
		case 0xB0: LUMEN_NI("v_cmpx_f_i64"); break;
		case 0xB1: LUMEN_NI("v_cmpx_lt_i64"); break;
		case 0xB2: LUMEN_NI("v_cmpx_eq_i64"); break;
		case 0xB3: LUMEN_NI("v_cmpx_le_i64"); break;
		case 0xB4: LUMEN_NI("v_cmpx_gt_i64"); break;
		case 0xB5: LUMEN_NI("v_cmpx_ne_i64"); break;
		case 0xB6: LUMEN_NI("v_cmpx_ge_i64"); break;
		case 0xB7: LUMEN_NI("v_cmpx_t_i64"); break;
		case 0xB8: LUMEN_NI("v_cmpx_class_f64"); break;
		case 0xB9: LUMEN_NI("v_cmpx_lt_u16"); break;
		case 0xBA: LUMEN_NI("v_cmpx_eq_u16"); break;
		case 0xBB: LUMEN_NI("v_cmpx_le_u16"); break;
		case 0xBC: LUMEN_NI("v_cmpx_gt_u16"); break;
		case 0xBD: LUMEN_NI("v_cmpx_ne_u16"); break;
		case 0xBE: LUMEN_NI("v_cmpx_ge_u16"); break;
		case 0xc0: inst.type = ShaderInstructionType::VCmpFU32; break;
		case 0xc1: inst.type = ShaderInstructionType::VCmpLtU32; break;
		case 0xc2: inst.type = ShaderInstructionType::VCmpEqU32; break;
		case 0xc3: inst.type = ShaderInstructionType::VCmpLeU32; break;
		case 0xc4: inst.type = ShaderInstructionType::VCmpGtU32; break;
		case 0xc5: inst.type = ShaderInstructionType::VCmpNeU32; break;
		case 0xc6: inst.type = ShaderInstructionType::VCmpGeU32; break;
		case 0xc7: inst.type = ShaderInstructionType::VCmpTU32; break;
		case 0xC8: LUMEN_NI("v_cmp_f_f16"); break;
		case 0xC9: LUMEN_NI("v_cmp_lt_f16"); break;
		case 0xCA: LUMEN_NI("v_cmp_eq_f16"); break;
		case 0xCB: LUMEN_NI("v_cmp_le_f16"); break;
		case 0xCC: LUMEN_NI("v_cmp_gt_f16"); break;
		case 0xCD: LUMEN_NI("v_cmp_lg_f16"); break;
		case 0xCE: LUMEN_NI("v_cmp_ge_f16"); break;
		case 0xCF: LUMEN_NI("v_cmp_o_f16"); break;
		case 0xD0: LUMEN_NI("v_cmpx_f_u32"); break;
		case 0xD1: LUMEN_NI("v_cmpx_lt_u32"); break;
		case 0xd2: inst.type = ShaderInstructionType::VCmpxEqU32; break;
		case 0xD3: LUMEN_NI("v_cmpx_le_u32"); break;
		case 0xd4: inst.type = ShaderInstructionType::VCmpxGtU32; break;
		case 0xd5: inst.type = ShaderInstructionType::VCmpxNeU32; break;
		case 0xd6: inst.type = ShaderInstructionType::VCmpxGeU32; break;
		case 0xD7: LUMEN_NI("v_cmpx_t_u32"); break;
		case 0xD8: LUMEN_NI("v_cmpx_f_f16"); break;
		case 0xD9: LUMEN_NI("v_cmpx_lt_f16"); break;
		case 0xDA: LUMEN_NI("v_cmpx_eq_f16"); break;
		case 0xDB: LUMEN_NI("v_cmpx_le_f16"); break;
		case 0xDC: LUMEN_NI("v_cmpx_gt_f16"); break;
		case 0xDD: LUMEN_NI("v_cmpx_lg_f16"); break;
		case 0xDE: LUMEN_NI("v_cmpx_ge_f16"); break;
		case 0xDF: LUMEN_NI("v_cmpx_o_f16"); break;
		case 0xE0: LUMEN_NI("v_cmp_f_u64"); break;
		case 0xE1: LUMEN_NI("v_cmp_lt_u64"); break;
		case 0xE2: LUMEN_NI("v_cmp_eq_u64"); break;
		case 0xE3: LUMEN_NI("v_cmp_le_u64"); break;
		case 0xE4: LUMEN_NI("v_cmp_gt_u64"); break;
		case 0xE5: LUMEN_NI("v_cmp_ne_u64"); break;
		case 0xE6: LUMEN_NI("v_cmp_ge_u64"); break;
		case 0xE7: LUMEN_NI("v_cmp_t_u64"); break;
		case 0xE8: LUMEN_NI("v_cmp_u_f16"); break;
		case 0xE9: LUMEN_NI("v_cmp_nge_f16"); break;
		case 0xEA: LUMEN_NI("v_cmp_nlg_f16"); break;
		case 0xEB: LUMEN_NI("v_cmp_ngt_f16"); break;
		case 0xEC: LUMEN_NI("v_cmp_nle_f16"); break;
		case 0xED: LUMEN_NI("v_cmp_neq_f16"); break;
		case 0xEE: LUMEN_NI("v_cmp_nlt_f16"); break;
		case 0xEF: LUMEN_NI("v_cmp_tru_f16"); break;
		case 0xF0: LUMEN_NI("v_cmpx_f_u64"); break;
		case 0xF1: LUMEN_NI("v_cmpx_lt_u64"); break;
		case 0xF2: LUMEN_NI("v_cmpx_eq_u64"); break;
		case 0xF3: LUMEN_NI("v_cmpx_le_u64"); break;
		case 0xF4: LUMEN_NI("v_cmpx_gt_u64"); break;
		case 0xF5: LUMEN_NI("v_cmpx_ne_u64"); break;
		case 0xF6: LUMEN_NI("v_cmpx_ge_u64"); break;
		case 0xF7: LUMEN_NI("v_cmpx_t_u64"); break;
		case 0xF8: LUMEN_NI("v_cmpx_u_f16"); break;
		case 0xF9: LUMEN_NI("v_cmpx_nge_f16"); break;
		case 0xFA: LUMEN_NI("v_cmpx_nlg_f16"); break;
		case 0xFB: LUMEN_NI("v_cmpx_ngt_f16"); break;
		case 0xFC: LUMEN_NI("v_cmpx_nle_f16"); break;
		case 0xFD: LUMEN_NI("v_cmpx_neq_f16"); break;
		case 0xFE: LUMEN_NI("v_cmpx_nlt_f16"); break;
		case 0xFF: LUMEN_NI("v_cmpx_tru_f16"); break;

		/* VOP2 using VOP3 encoding */
		case 0x100:
			inst.type        = ShaderInstructionType::VCndmaskB32;
			inst.format      = ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2;
			inst.src_num     = 3;
			inst.src[2].size = 2;
			break;
		case 0x101: LUMEN_NI("v_readlane_b32"); break;
		case 0x102: LUMEN_NI("v_writelane_b32"); break;
		case 0x103: inst.type = ShaderInstructionType::VAddF32; break;
		case 0x104: inst.type = ShaderInstructionType::VSubF32; break;
		case 0x105: inst.type = ShaderInstructionType::VSubrevF32; break;
		case 0x106: LUMEN_NI("v_mac_legacy_f32"); break;
		case 0x107: inst.type = ShaderInstructionType::VMulLegacyF32; break;
		case 0x108: inst.type = ShaderInstructionType::VMulF32; break;
		case 0x109: LUMEN_NI("v_mul_i32_i24"); break;
		case 0x10A: LUMEN_NI("v_mul_hi_i32_i24"); break;
		case 0x10b: inst.type = ShaderInstructionType::VMulU32U24; break;
		case 0x10C: LUMEN_NI("v_mul_hi_u32_u24"); break;
		case 0x10D: LUMEN_NI("v_min_legacy_f32"); break;
		case 0x10E: LUMEN_NI("v_max_legacy_f32"); break;
		case 0x10f: inst.type = ShaderInstructionType::VMinF32; break;
		case 0x110: inst.type = ShaderInstructionType::VMaxF32; break;
		case 0x111: LUMEN_NI("v_min_i32"); break;
		case 0x112: LUMEN_NI("v_max_i32"); break;
		case 0x113: LUMEN_NI("v_min_u32"); break;
		case 0x114: LUMEN_NI("v_max_u32"); break;
		case 0x115: inst.type = ShaderInstructionType::VLshrB32; break;
		case 0x116: inst.type = ShaderInstructionType::VLshrrevB32; break;
		case 0x117: inst.type = ShaderInstructionType::VAshrI32; break;
		case 0x118: inst.type = ShaderInstructionType::VAshrrevI32; break;
		case 0x119: inst.type = ShaderInstructionType::VLshlB32; break;
		case 0x11a: inst.type = ShaderInstructionType::VLshlrevB32; break;
		case 0x11b: inst.type = ShaderInstructionType::VAndB32; break;
		case 0x11c: inst.type = ShaderInstructionType::VOrB32; break;
		case 0x11d: inst.type = ShaderInstructionType::VXorB32; break;
		case 0x11E: inst.type = ShaderInstructionType::VBfmB32; break;
		case 0x11f: inst.type = ShaderInstructionType::VMacF32; break;
		case 0x120: LUMEN_NI("v_madmk_f32"); break;
		case 0x121: LUMEN_NI("v_madak_f32"); break;
		case 0x122: inst.type = ShaderInstructionType::VBcntU32B32; break;
		case 0x123: inst.type = ShaderInstructionType::VMbcntLoU32B32; break;
		case 0x124: inst.type = ShaderInstructionType::VMbcntHiU32B32; break;
		case 0x125:
			inst.type      = ShaderInstructionType::VAddI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2      = operand_parse(sdst);
			inst.dst2.size = 2;
			break;
		case 0x126:
			inst.type      = ShaderInstructionType::VSubI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2      = operand_parse(sdst);
			inst.dst2.size = 2;
			break;
		case 0x127:
			inst.type      = ShaderInstructionType::VSubrevI32;
			inst.format    = ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1;
			inst.dst2      = operand_parse(sdst);
			inst.dst2.size = 2;
			break;
		case 0x128: LUMEN_NI("v_addc_u32"); break;
		case 0x129: LUMEN_NI("v_subb_u32"); break;
		case 0x12A: LUMEN_NI("v_subbrev_u32"); break;
		case 0x12B: LUMEN_NI("v_ldexp_f32"); break;
		case 0x12C: LUMEN_NI("v_cvt_pkaccum_u8_f32"); break;
		case 0x12D: LUMEN_NI("v_cvt_pknorm_i16_f32"); break;
		case 0x12E: LUMEN_NI("v_cvt_pknorm_u16_f32"); break;
		case 0x12f: inst.type = ShaderInstructionType::VCvtPkrtzF16F32; break;
		case 0x130: LUMEN_NI("v_cvt_pk_u16_u32"); break;
		case 0x131: LUMEN_NI("v_cvt_pk_i16_i32"); break;
		case 0x132: LUMEN_NI("v_add_f16"); break;
		case 0x133: LUMEN_NI("v_sub_f16"); break;
		case 0x134: LUMEN_NI("v_subrev_f16"); break;
		case 0x135: LUMEN_NI("v_mul_f16"); break;
		case 0x136: LUMEN_NI("v_mac_f16"); break;
		case 0x137: LUMEN_NI("v_madmk_f16"); break;
		case 0x138: LUMEN_NI("v_madak_f16"); break;
		case 0x139: LUMEN_NI("v_max_f16"); break;
		case 0x13A: LUMEN_NI("v_min_f16"); break;
		case 0x13B: LUMEN_NI("v_ldexp_f16"); break;

		/* VOP3 instructions */
		case 0x140: LUMEN_NI("v_mad_legacy_f32"); break;
		case 0x141: inst.type = ShaderInstructionType::VMadF32; break;
		case 0x142: LUMEN_NI("v_mad_i32_i24"); break;
		case 0x143: inst.type = ShaderInstructionType::VMadU32U24; break;
		case 0x144: LUMEN_NI("v_cubeid_f32"); break;
		case 0x145: LUMEN_NI("v_cubesc_f32"); break;
		case 0x146: LUMEN_NI("v_cubetc_f32"); break;
		case 0x147: LUMEN_NI("v_cubema_f32"); break;
		case 0x148: inst.type = ShaderInstructionType::VBfeU32; break;
		case 0x149: LUMEN_NI("v_bfe_i32"); break;
		case 0x14A: LUMEN_NI("v_bfi_b32"); break;
		case 0x14b: inst.type = ShaderInstructionType::VFmaF32; break;
		case 0x14C: LUMEN_NI("v_fma_f64"); break;
		case 0x14D: LUMEN_NI("v_lerp_u8"); break;
		case 0x14E: LUMEN_NI("v_alignbit_b32"); break;
		case 0x14F: LUMEN_NI("v_alignbyte_b32"); break;
		case 0x150: LUMEN_NI("v_mullit_f32"); break;
		case 0x151: inst.type = ShaderInstructionType::VMin3F32; break;
		case 0x152: LUMEN_NI("v_min3_i32"); break;
		case 0x153: LUMEN_NI("v_min3_u32"); break;
		case 0x154: inst.type = ShaderInstructionType::VMax3F32; break;
		case 0x155: LUMEN_NI("v_max3_i32"); break;
		case 0x156: LUMEN_NI("v_max3_u32"); break;
		case 0x157: inst.type = ShaderInstructionType::VMed3F32; break;
		case 0x158: LUMEN_NI("v_med3_i32"); break;
		case 0x159: LUMEN_NI("v_med3_u32"); break;
		case 0x15A: LUMEN_NI("v_sad_u8"); break;
		case 0x15B: LUMEN_NI("v_sad_hi_u8"); break;
		case 0x15C: LUMEN_NI("v_sad_u16"); break;
		case 0x15d: inst.type = ShaderInstructionType::VSadU32; break;
		case 0x15E: LUMEN_NI("v_cvt_pk_u8_f32"); break;
		case 0x15F: LUMEN_NI("v_div_fixup_f32"); break;
		case 0x160: LUMEN_NI("v_div_fixup_f64"); break;
		case 0x161: LUMEN_NI("v_lshl_b64"); break;
		case 0x162: LUMEN_NI("v_lshr_b64"); break;
		case 0x163: LUMEN_NI("v_ashr_i64"); break;
		case 0x164: LUMEN_NI("v_add_f64"); break;
		case 0x165: LUMEN_NI("v_mul_f64"); break;
		case 0x166: LUMEN_NI("v_min_f64"); break;
		case 0x167: LUMEN_NI("v_max_f64"); break;
		case 0x168: LUMEN_NI("v_ldexp_f64"); break;
		case 0x169:
			inst.type    = ShaderInstructionType::VMulLoU32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16a:
			inst.type    = ShaderInstructionType::VMulHiU32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16b:
			inst.type    = ShaderInstructionType::VMulLoI32;
			inst.format  = ShaderInstructionFormat::SVdstSVsrc0SVsrc1;
			inst.src_num = 2;
			break;
		case 0x16C: LUMEN_NI("v_mul_hi_i32"); break;
		case 0x16D: LUMEN_NI("v_div_scale_f32"); break;
		case 0x16E: LUMEN_NI("v_div_scale_f64"); break;
		case 0x16F: LUMEN_NI("v_div_fmas_f32"); break;
		case 0x170: LUMEN_NI("v_div_fmas_f64"); break;
		case 0x171: LUMEN_NI("v_msad_u8"); break;
		case 0x174: LUMEN_NI("v_trig_preop_f64"); break;
		case 0x175: LUMEN_NI("v_mqsad_u32_u8"); break;
		case 0x176: LUMEN_NI("v_mad_u64_u32"); break;
		case 0x177: LUMEN_NI("v_mad_i64_i32"); break;
		/* VOP1 using VOP3 encoding */
		case 0x180: LUMEN_NI("v_nop"); break;
		case 0x181: LUMEN_NI("v_mov_b32"); break;
		case 0x182: LUMEN_NI("v_readfirstlane_b32"); break;
		case 0x183: LUMEN_NI("v_cvt_i32_f64"); break;
		case 0x184: LUMEN_NI("v_cvt_f64_i32"); break;
		case 0x185: LUMEN_NI("v_cvt_f32_i32"); break;
		case 0x186: LUMEN_NI("v_cvt_f32_u32"); break;
		case 0x187: LUMEN_NI("v_cvt_u32_f32"); break;
		case 0x188: LUMEN_NI("v_cvt_i32_f32"); break;
		case 0x189: LUMEN_NI("v_mov_fed_b32"); break;
		case 0x18A: LUMEN_NI("v_cvt_f16_f32"); break;
		case 0x18B: LUMEN_NI("v_cvt_f32_f16"); break;
		case 0x18C: LUMEN_NI("v_cvt_rpi_i32_f32"); break;
		case 0x18D: LUMEN_NI("v_cvt_flr_i32_f32"); break;
		case 0x18E: LUMEN_NI("v_cvt_off_f32_i4"); break;
		case 0x18F: LUMEN_NI("v_cvt_f32_f64"); break;
		case 0x190: LUMEN_NI("v_cvt_f64_f32"); break;
		case 0x191: LUMEN_NI("v_cvt_f32_ubyte0"); break;
		case 0x192: LUMEN_NI("v_cvt_f32_ubyte1"); break;
		case 0x193: LUMEN_NI("v_cvt_f32_ubyte2"); break;
		case 0x194: LUMEN_NI("v_cvt_f32_ubyte3"); break;
		case 0x195: LUMEN_NI("v_cvt_u32_f64"); break;
		case 0x196: LUMEN_NI("v_cvt_f64_u32"); break;
		case 0x197: LUMEN_NI("v_trunc_f64"); break;
		case 0x198: LUMEN_NI("v_ceil_f64"); break;
		case 0x199: LUMEN_NI("v_rndne_f64"); break;
		case 0x19A: LUMEN_NI("v_floor_f64"); break;
		case 0x1a0: inst.type = ShaderInstructionType::VFractF32; break;
		case 0x1a1: inst.type = ShaderInstructionType::VTruncF32; break;
		case 0x1a2: inst.type = ShaderInstructionType::VCeilF32; break;
		case 0x1a3: inst.type = ShaderInstructionType::VRndneF32; break;
		case 0x1a4: inst.type = ShaderInstructionType::VFloorF32; break;
		case 0x1a5: inst.type = ShaderInstructionType::VExpF32; break;
		case 0x1A6: LUMEN_NI("v_log_clamp_f32"); break;
		case 0x1a7: inst.type = ShaderInstructionType::VLogF32; break;
		case 0x1A8: LUMEN_NI("v_rcp_clamp_f32"); break;
		case 0x1A9: LUMEN_NI("v_rcp_legacy_f32"); break;
		case 0x1aa: inst.type = ShaderInstructionType::VRcpF32; break;
		case 0x1AB: LUMEN_NI("v_rcp_iflag_f32"); break;
		case 0x1AC: LUMEN_NI("v_rsq_clamp_f32"); break;
		case 0x1AD: LUMEN_NI("v_rsq_legacy_f32"); break;
		case 0x1ae: inst.type = ShaderInstructionType::VRsqF32; break;
		case 0x1AF: LUMEN_NI("v_rcp_f64"); break;
		case 0x1B0: LUMEN_NI("v_rcp_clamp_f64"); break;
		case 0x1B1: LUMEN_NI("v_rsq_f64"); break;
		case 0x1B2: LUMEN_NI("v_rsq_clamp_f64"); break;
		case 0x1b3: inst.type = ShaderInstructionType::VSqrtF32; break;
		case 0x1B4: LUMEN_NI("v_sqrt_f64"); break;
		case 0x1b5: inst.type = ShaderInstructionType::VSinF32; break;
		case 0x1b6: inst.type = ShaderInstructionType::VCosF32; break;
		case 0x1B7: LUMEN_NI("v_not_b32"); break;
		case 0x1B8: LUMEN_NI("v_bfrev_b32"); break;
		case 0x1B9: LUMEN_NI("v_ffbh_u32"); break;
		case 0x1BA: LUMEN_NI("v_ffbl_b32"); break;
		case 0x1BB: LUMEN_NI("v_ffbh_i32"); break;
		case 0x1BC: LUMEN_NI("v_frexp_exp_i32_f64"); break;
		case 0x1BD: LUMEN_NI("v_frexp_mant_f64"); break;
		case 0x1BE: LUMEN_NI("v_fract_f64"); break;
		case 0x1BF: LUMEN_NI("v_frexp_exp_i32_f32"); break;
		case 0x1C0: LUMEN_NI("v_frexp_mant_f32"); break;
		case 0x1C1: LUMEN_NI("v_clrexcp"); break;
		case 0x1C2: LUMEN_NI("v_movreld_b32"); break;
		case 0x1C3: LUMEN_NI("v_movrels_b32"); break;
		case 0x1C4: LUMEN_NI("v_movrelsd_b32"); break;
		case 0x1C5: LUMEN_NI("v_log_legacy_f32"); break;
		case 0x1C6: LUMEN_NI("v_exp_legacy_f32"); break;
		case 0x1D0: LUMEN_NI("v_cvt_f16_u16"); break;
		case 0x1D1: LUMEN_NI("v_cvt_f16_i16"); break;
		case 0x1D2: LUMEN_NI("v_cvt_u16_f16"); break;
		case 0x1D3: LUMEN_NI("v_cvt_i16_f16"); break;
		case 0x1D4: LUMEN_NI("v_rcp_f16"); break;
		case 0x1D5: LUMEN_NI("v_sqrt_f16"); break;
		case 0x1D6: LUMEN_NI("v_rsq_f16"); break;
		case 0x1D7: LUMEN_NI("v_log_f16"); break;
		case 0x1D8: LUMEN_NI("v_exp_f16"); break;
		case 0x1D9: LUMEN_NI("v_frexp_mant_f16"); break;
		case 0x1DA: LUMEN_NI("v_frexp_exp_i16_f16"); break;
		case 0x1DB: LUMEN_NI("v_floor_f16"); break;
		case 0x1DC: LUMEN_NI("v_ceil_f16"); break;
		case 0x1DD: LUMEN_NI("v_trunc_f16"); break;
		case 0x1DE: LUMEN_NI("v_rndne_f16"); break;
		case 0x1DF: LUMEN_NI("v_fract_f16"); break;
		case 0x1E0: LUMEN_NI("v_sin_f16"); break;
		case 0x1E1: LUMEN_NI("v_cos_f16"); break;
		case 0x1E2: LUMEN_NI("v_sat_pk_u8_i16"); break;
		case 0x1E3: LUMEN_NI("v_cvt_norm_i16_f16"); break;
		case 0x1E4: LUMEN_NI("v_cvt_norm_u16_f16"); break;
		case 0x1E5: LUMEN_NI("v_swap_b32"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	if (inst.dst2.type == ShaderOperandType::Unknown)
	{
		if ((abs & 0x1u) != 0)
		{
			inst.src[0].absolute = true;
		}
		if ((abs & 0x2u) != 0)
		{
			inst.src[1].absolute = true;
		}
		if ((abs & 0x4u) != 0)
		{
			inst.src[2].absolute = true;
		}

		inst.dst.clamp = (clamp != 0);
	}

	dst->GetInstructions().Add(inst);

	return size;
}

LUMEN_SHADER_PARSER(shader_parse_exp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("exp");

	uint32_t vm     = (ShaderReadSourceWord(buffer, 0) >> 12u) & 0x1u;
	uint32_t done   = (ShaderReadSourceWord(buffer, 0) >> 11u) & 0x1u;
	uint32_t compr  = (ShaderReadSourceWord(buffer, 0) >> 10u) & 0x1u;
	uint32_t target = (ShaderReadSourceWord(buffer, 0) >> 4u) & 0x3fu;
	uint32_t en     = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xfu;

	uint32_t vsrc0 = (ShaderReadSourceWord(buffer, 1) >> 0u) & 0xffu;
	uint32_t vsrc1 = (ShaderReadSourceWord(buffer, 1) >> 8u) & 0xffu;
	uint32_t vsrc2 = (ShaderReadSourceWord(buffer, 1) >> 16u) & 0xffu;
	uint32_t vsrc3 = (ShaderReadSourceWord(buffer, 1) >> 24u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.src[0]  = operand_parse(vsrc0 + 256);
	inst.src[1]  = operand_parse(vsrc1 + 256);
	inst.src[2]  = operand_parse(vsrc2 + 256);
	inst.src[3]  = operand_parse(vsrc3 + 256);
	inst.src_num = 4;

	inst.type = ShaderInstructionType::Exp;
	inst.export_valid_mask = vm != 0;
	inst.export_done       = done != 0;
	inst.export_target     = static_cast<uint8_t>(target);

	switch (target)
	{
		case 0x00:
			if (done != 0 && compr != 0 && vm != 0 && en == 0x0)
			{
				inst.format  = ShaderInstructionFormat::Mrt0OffOffComprVmDone;
				inst.src_num = 0;
			} else if (compr != 0 && en == 0x3)
			{
				inst.format  = ShaderInstructionFormat::Mrt0Vsrc0OffComprVmDone;
				inst.src_num = 1;
			} else if (compr != 0 && en == 0xf)
			{
				inst.format  = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone;
				inst.src_num = 2;
			} else if (compr == 0 && en == 0xf)
			{
				inst.format = ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone;
			};
			break;
		case 0x0c:
			if (done != 0 && en == 0xf)
			{
				inst.format = ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done;
			};
			break;
		default: break;
	}

	if (inst.format == ShaderInstructionFormat::Unknown && done == 0 && compr == 0 && vm == 0 && en == 0xf)
	{
		switch (target)
		{
			case 0x20: inst.format = ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x21: inst.format = ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x22: inst.format = ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x23: inst.format = ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 0x24: inst.format = ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3; break;
			default:
				if (target >= 0x25 && target <= 0x3f && dst->GetType() == ShaderType::Vertex && (ShaderReadSourceWord(buffer, 0) & 0x03ffe000u) == 0)
					inst.format = ShaderInstructionFormat::ParamVsrc0Vsrc1Vsrc2Vsrc3;
				break;
		}
	}

	if (inst.format == ShaderInstructionFormat::Unknown)
	{
		printf("%s", dst->DbgDump().c_str());
		EXIT("%s\n"
		     "unknown exp target: 0x%02" PRIx32 " at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ", crc32 = 0x%08" PRIx32
		     ") done=%u compr=%u vm=%u en=0x%x\n",
		     dst->DbgDump().c_str(), target, pc, dst->GetHash0(), dst->GetCrc32(), done, compr, vm, en);
	}

	dst->GetInstructions().Add(inst);

	return 2;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_smrd)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("smrd");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 22u) & 0x1fu;
	uint32_t sdst   = (ShaderReadSourceWord(buffer, 0) >> 15u) & 0x7fu;
	uint32_t sbase  = (ShaderReadSourceWord(buffer, 0) >> 9u) & 0x3fu;
	uint32_t imm    = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0x1u;
	uint32_t offset = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xffu;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(sdst);
	inst.src_num = 2;
	inst.src[0]  = operand_parse(sbase * 2);

	uint32_t size = 1;

	if (imm == 1)
	{
		inst.src[1].type       = ShaderOperandType::LiteralConstant;
		inst.src[1].constant.u = offset << 2u;
	} else
	{
		inst.src[1] = operand_parse(offset);

		if (inst.src[1].type == ShaderOperandType::LiteralConstant)
		{
			inst.src[1].constant.u = ShaderReadSourceWord(buffer, size);
			size++;
		}
	}

	switch (opcode)
	{
		case 0x00: LUMEN_NI("s_load_dword"); break;
		case 0x01: LUMEN_NI("s_load_dwordx2"); break;
		case 0x02:
			inst.type        = ShaderInstructionType::SLoadDwordx4;
			inst.format      = ShaderInstructionFormat::Sdst4SbaseSoffset;
			inst.src[0].size = 2;
			inst.dst.size    = 4;
			break;
		case 0x03:
			inst.type        = ShaderInstructionType::SLoadDwordx8;
			inst.format      = ShaderInstructionFormat::Sdst8SbaseSoffset;
			inst.src[0].size = 2;
			inst.dst.size    = 8;
			break;
		case 0x04: LUMEN_NI("s_load_dwordx16"); break;
		case 0x08:
			inst.type        = ShaderInstructionType::SBufferLoadDword;
			inst.format      = ShaderInstructionFormat::SdstSvSoffset;
			inst.src[0].size = 4;
			break;
		case 0x09:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx2;
			inst.format      = ShaderInstructionFormat::Sdst2SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x0a:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx4;
			inst.format      = ShaderInstructionFormat::Sdst4SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x0b:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx8;
			inst.format      = ShaderInstructionFormat::Sdst8SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 8;
			break;
		case 0x0c:
			inst.type        = ShaderInstructionType::SBufferLoadDwordx16;
			inst.format      = ShaderInstructionFormat::Sdst16SvSoffset;
			inst.src[0].size = 4;
			inst.dst.size    = 16;
			break;
		case 0x1C: LUMEN_NI("s_memrealtime"); break;
		case 0x1D: LUMEN_NI("s_dcache_inv_vol"); break;
		case 0x1E: LUMEN_NI("s_memtime"); break;
		case 0x1F: LUMEN_NI("s_dcache_inv") break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_mubuf)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("mubuf");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 18u) & 0x1fu;
	uint32_t lds    = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x1u;
	uint32_t glc    = (ShaderReadSourceWord(buffer, 0) >> 14u) & 0x1u;
	uint32_t idxen  = (ShaderReadSourceWord(buffer, 0) >> 13u) & 0x1u;
	uint32_t offen  = (ShaderReadSourceWord(buffer, 0) >> 12u) & 0x1u;
	uint32_t offset = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xfffu;

	uint32_t soffset = (ShaderReadSourceWord(buffer, 1) >> 24u) & 0xffu;
	uint32_t tfe     = (ShaderReadSourceWord(buffer, 1) >> 23u) & 0x1u;
	uint32_t slc     = (ShaderReadSourceWord(buffer, 1) >> 22u) & 0x1u;
	uint32_t srsrc   = (ShaderReadSourceWord(buffer, 1) >> 16u) & 0x1fu;
	uint32_t vdata   = (ShaderReadSourceWord(buffer, 1) >> 8u) & 0xffu;
	uint32_t vaddr   = (ShaderReadSourceWord(buffer, 1) >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(idxen == 0);
	EXIT_NOT_IMPLEMENTED(offen == 1);
	EXIT_NOT_IMPLEMENTED(offset != 0);
	EXIT_NOT_IMPLEMENTED(glc == 1);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(lds == 1);
	EXIT_NOT_IMPLEMENTED(tfe == 1);

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(vdata + 256);
	inst.src_num = 3;
	inst.src[0]  = operand_parse(vaddr + 256);
	inst.src[1]  = operand_parse(srsrc * 4);
	inst.src[2]  = operand_parse(soffset);

	if (inst.src[2].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[2].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	switch (opcode)
	{
		case 0x00:
			inst.type        = ShaderInstructionType::BufferLoadFormatX;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x01:
			inst.type        = ShaderInstructionType::BufferLoadFormatXy;
			inst.format      = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x02:
			inst.type        = ShaderInstructionType::BufferLoadFormatXyz;
			inst.format      = ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 3;
			break;
		case 0x03:
			inst.type        = ShaderInstructionType::BufferLoadFormatXyzw;
			inst.format      = ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 4;
			break;
		case 0x04:
			inst.type        = ShaderInstructionType::BufferStoreFormatX;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x05:
			inst.type        = ShaderInstructionType::BufferStoreFormatXy;
			inst.format      = ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			inst.dst.size    = 2;
			break;
		case 0x06: LUMEN_NI("buffer_store_format_xyz"); break;
		case 0x07: LUMEN_NI("buffer_store_format_xyzw"); break;
		case 0x08: LUMEN_NI("buffer_load_ubyte"); break;
		case 0x09: LUMEN_NI("buffer_load_sbyte"); break;
		case 0x0A: LUMEN_NI("buffer_load_ushort"); break;
		case 0x0B: LUMEN_NI("buffer_load_sshort"); break;
		case 0x0c:
			inst.type        = ShaderInstructionType::BufferLoadDword;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x0D: LUMEN_NI("buffer_load_dwordx2"); break;
		case 0x0E: LUMEN_NI("buffer_load_dwordx4"); break;
		case 0x0F: LUMEN_NI("buffer_load_dwordx3"); break;
		case 0x18: LUMEN_NI("buffer_store_byte"); break;
		case 0x1A: LUMEN_NI("buffer_store_short"); break;
		case 0x1c:
			inst.type        = ShaderInstructionType::BufferStoreDword;
			inst.format      = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen;
			inst.src[1].size = 4;
			break;
		case 0x1D: LUMEN_NI("buffer_store_dwordx2"); break;
		case 0x1E: LUMEN_NI("buffer_store_dwordx4"); break;
		case 0x1F: LUMEN_NI("buffer_store_dwordx3"); break;
		case 0x30: LUMEN_NI("buffer_atomic_swap"); break;
		case 0x31: LUMEN_NI("buffer_atomic_cmpswap"); break;
		case 0x32: LUMEN_NI("buffer_atomic_add"); break;
		case 0x33: LUMEN_NI("buffer_atomic_sub"); break;
		case 0x34: LUMEN_NI("buffer_atomic_rsub"); break;
		case 0x35: LUMEN_NI("buffer_atomic_smin"); break;
		case 0x36: LUMEN_NI("buffer_atomic_umin"); break;
		case 0x37: LUMEN_NI("buffer_atomic_smax"); break;
		case 0x38: LUMEN_NI("buffer_atomic_umax"); break;
		case 0x39: LUMEN_NI("buffer_atomic_and"); break;
		case 0x3A: LUMEN_NI("buffer_atomic_or"); break;
		case 0x3B: LUMEN_NI("buffer_atomic_xor"); break;
		case 0x3C: LUMEN_NI("buffer_atomic_inc"); break;
		case 0x3D: LUMEN_NI("buffer_atomic_dec"); break;
		case 0x3E: LUMEN_NI("buffer_atomic_fcmpswap"); break;
		case 0x3F: LUMEN_NI("buffer_atomic_fmin"); break;
		case 0x40: LUMEN_NI("buffer_atomic_fmax"); break;
		case 0x50: LUMEN_NI("buffer_atomic_swap_x2"); break;
		case 0x51: LUMEN_NI("buffer_atomic_cmpswap_x2"); break;
		case 0x52: LUMEN_NI("buffer_atomic_add_x2"); break;
		case 0x53: LUMEN_NI("buffer_atomic_sub_x2"); break;
		case 0x54: LUMEN_NI("buffer_atomic_rsub_x2"); break;
		case 0x55: LUMEN_NI("buffer_atomic_smin_x2"); break;
		case 0x56: LUMEN_NI("buffer_atomic_umin_x2"); break;
		case 0x57: LUMEN_NI("buffer_atomic_smax_x2"); break;
		case 0x58: LUMEN_NI("buffer_atomic_umax_x2"); break;
		case 0x59: LUMEN_NI("buffer_atomic_and_x2"); break;
		case 0x5A: LUMEN_NI("buffer_atomic_or_x2"); break;
		case 0x5B: LUMEN_NI("buffer_atomic_xor_x2"); break;
		case 0x5C: LUMEN_NI("buffer_atomic_inc_x2"); break;
		case 0x5D: LUMEN_NI("buffer_atomic_dec_x2"); break;
		case 0x5E: LUMEN_NI("buffer_atomic_fcmpswap_x2"); break;
		case 0x5F: LUMEN_NI("buffer_atomic_fmin_x2"); break;
		case 0x60: LUMEN_NI("buffer_atomic_fmax_x2"); break;
		case 0x71: LUMEN_NI("buffer_wbinvl1"); break;
		case 0x80: LUMEN_NI("buffer_load_format_d16_x"); break;
		case 0x81: LUMEN_NI("buffer_load_format_d16_xy"); break;
		case 0x82: LUMEN_NI("buffer_load_format_d16_xyz"); break;
		case 0x83: LUMEN_NI("buffer_load_format_d16_xyzw"); break;
		case 0x84: LUMEN_NI("buffer_store_format_d16_x"); break;
		case 0x85: LUMEN_NI("buffer_store_format_d16_xy"); break;
		case 0x86: LUMEN_NI("buffer_store_format_d16_xyz"); break;
		case 0x87: LUMEN_NI("buffer_store_format_d16_xyzw"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_ds)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("ds");

	uint32_t opcode  = (ShaderReadSourceWord(buffer, 0) >> 18u) & 0xffu;
	uint32_t gds     = (ShaderReadSourceWord(buffer, 0) >> 17u) & 0x1u;
	uint32_t offset0 = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xffu;
	uint32_t offset1 = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0xffu;

	uint32_t vdst  = (ShaderReadSourceWord(buffer, 1) >> 24u) & 0xffu;
	uint32_t data1 = (ShaderReadSourceWord(buffer, 1) >> 16u) & 0xffu;
	uint32_t data0 = (ShaderReadSourceWord(buffer, 1) >> 8u) & 0xffu;
	uint32_t addr  = (ShaderReadSourceWord(buffer, 1) >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(addr != 0);
	EXIT_NOT_IMPLEMENTED(data0 != 0);
	EXIT_NOT_IMPLEMENTED(data1 != 0);
	EXIT_NOT_IMPLEMENTED(offset0 != 0);
	EXIT_NOT_IMPLEMENTED(offset1 != 0);
	EXIT_NOT_IMPLEMENTED(gds == 0);

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(vdst + 256);
	inst.src_num = 0;

	switch (opcode) // NOLINT
	{
		case 0x00: LUMEN_NI("ds_add_u32"); break;
		case 0x01: LUMEN_NI("ds_sub_u32"); break;
		case 0x02: LUMEN_NI("ds_rsub_u32"); break;
		case 0x03: LUMEN_NI("ds_inc_u32"); break;
		case 0x04: LUMEN_NI("ds_dec_u32"); break;
		case 0x05: LUMEN_NI("ds_min_i32"); break;
		case 0x06: LUMEN_NI("ds_max_i32"); break;
		case 0x07: LUMEN_NI("ds_min_u32"); break;
		case 0x08: LUMEN_NI("ds_max_u32"); break;
		case 0x09: LUMEN_NI("ds_and_b32"); break;
		case 0x0A: LUMEN_NI("ds_or_b32"); break;
		case 0x0B: LUMEN_NI("ds_xor_b32"); break;
		case 0x0C: LUMEN_NI("ds_mskor_b32"); break;
		case 0x0D: LUMEN_NI("ds_write_b32"); break;
		case 0x0E: LUMEN_NI("ds_write2_b32"); break;
		case 0x0F: LUMEN_NI("ds_write2st64_b32"); break;
		case 0x10: LUMEN_NI("ds_cmpst_b32"); break;
		case 0x11: LUMEN_NI("ds_cmpst_f32"); break;
		case 0x12: LUMEN_NI("ds_min_f32"); break;
		case 0x13: LUMEN_NI("ds_max_f32"); break;
		case 0x14: LUMEN_NI("ds_nop"); break;
		case 0x18: LUMEN_NI("ds_gws_sema_release_all"); break;
		case 0x19: LUMEN_NI("ds_gws_init"); break;
		case 0x1A: LUMEN_NI("ds_gws_sema_v"); break;
		case 0x1B: LUMEN_NI("ds_gws_sema_br"); break;
		case 0x1C: LUMEN_NI("ds_gws_sema_p"); break;
		case 0x1D: LUMEN_NI("ds_gws_barrier"); break;
		case 0x1E: LUMEN_NI("ds_write_b8"); break;
		case 0x1F: LUMEN_NI("ds_write_b16"); break;
		case 0x20: LUMEN_NI("ds_add_rtn_u32"); break;
		case 0x21: LUMEN_NI("ds_sub_rtn_u32"); break;
		case 0x22: LUMEN_NI("ds_rsub_rtn_u32"); break;
		case 0x23: LUMEN_NI("ds_inc_rtn_u32"); break;
		case 0x24: LUMEN_NI("ds_dec_rtn_u32"); break;
		case 0x25: LUMEN_NI("ds_min_rtn_i32"); break;
		case 0x26: LUMEN_NI("ds_max_rtn_i32"); break;
		case 0x27: LUMEN_NI("ds_min_rtn_u32"); break;
		case 0x28: LUMEN_NI("ds_max_rtn_u32"); break;
		case 0x29: LUMEN_NI("ds_and_rtn_b32"); break;
		case 0x2A: LUMEN_NI("ds_or_rtn_b32"); break;
		case 0x2B: LUMEN_NI("ds_xor_rtn_b32"); break;
		case 0x2C: LUMEN_NI("ds_mskor_rtn_b32"); break;
		case 0x2D: LUMEN_NI("ds_wrxchg_rtn_b32"); break;
		case 0x2E: LUMEN_NI("ds_wrxchg2_rtn_b32"); break;
		case 0x2F: LUMEN_NI("ds_wrxchg2st64_rtn_b32"); break;
		case 0x30: LUMEN_NI("ds_cmpst_rtn_b32"); break;
		case 0x31: LUMEN_NI("ds_cmpst_rtn_f32"); break;
		case 0x32: LUMEN_NI("ds_min_rtn_f32"); break;
		case 0x33: LUMEN_NI("ds_max_rtn_f32"); break;
		case 0x34: LUMEN_NI("ds_wrap_rtn_b32"); break;
		case 0x35: LUMEN_NI("ds_swizzle_b32"); break;
		case 0x36: LUMEN_NI("ds_read_b32"); break;
		case 0x37: LUMEN_NI("ds_read2_b32"); break;
		case 0x38: LUMEN_NI("ds_read2st64_b32"); break;
		case 0x39: LUMEN_NI("ds_read_i8"); break;
		case 0x3A: LUMEN_NI("ds_read_u8"); break;
		case 0x3B: LUMEN_NI("ds_read_i16"); break;
		case 0x3C: LUMEN_NI("ds_read_u16"); break;
		case 0x3d:
			inst.type   = ShaderInstructionType::DsConsume;
			inst.format = ShaderInstructionFormat::VdstGds;
			break;
		case 0x3e:
			inst.type   = ShaderInstructionType::DsAppend;
			inst.format = ShaderInstructionFormat::VdstGds;
			break;
		case 0x3F: LUMEN_NI("ds_ordered_count"); break;
		case 0x40: LUMEN_NI("ds_add_u64"); break;
		case 0x41: LUMEN_NI("ds_sub_u64"); break;
		case 0x42: LUMEN_NI("ds_rsub_u64"); break;
		case 0x43: LUMEN_NI("ds_inc_u64"); break;
		case 0x44: LUMEN_NI("ds_dec_u64"); break;
		case 0x45: LUMEN_NI("ds_min_i64"); break;
		case 0x46: LUMEN_NI("ds_max_i64"); break;
		case 0x47: LUMEN_NI("ds_min_u64"); break;
		case 0x48: LUMEN_NI("ds_max_u64"); break;
		case 0x49: LUMEN_NI("ds_and_b64"); break;
		case 0x4A: LUMEN_NI("ds_or_b64"); break;
		case 0x4B: LUMEN_NI("ds_xor_b64"); break;
		case 0x4C: LUMEN_NI("ds_mskor_b64"); break;
		case 0x4D: LUMEN_NI("ds_write_b64"); break;
		case 0x4E: LUMEN_NI("ds_write2_b64"); break;
		case 0x4F: LUMEN_NI("ds_write2st64_b64"); break;
		case 0x50: LUMEN_NI("ds_cmpst_b64"); break;
		case 0x51: LUMEN_NI("ds_cmpst_f64"); break;
		case 0x52: LUMEN_NI("ds_min_f64"); break;
		case 0x53: LUMEN_NI("ds_max_f64"); break;
		case 0x60: LUMEN_NI("ds_add_rtn_u64"); break;
		case 0x61: LUMEN_NI("ds_sub_rtn_u64"); break;
		case 0x62: LUMEN_NI("ds_rsub_rtn_u64"); break;
		case 0x63: LUMEN_NI("ds_inc_rtn_u64"); break;
		case 0x64: LUMEN_NI("ds_dec_rtn_u64"); break;
		case 0x65: LUMEN_NI("ds_min_rtn_i64"); break;
		case 0x66: LUMEN_NI("ds_max_rtn_i64"); break;
		case 0x67: LUMEN_NI("ds_min_rtn_u64"); break;
		case 0x68: LUMEN_NI("ds_max_rtn_u64"); break;
		case 0x69: LUMEN_NI("ds_and_rtn_b64"); break;
		case 0x6A: LUMEN_NI("ds_or_rtn_b64"); break;
		case 0x6B: LUMEN_NI("ds_xor_rtn_b64"); break;
		case 0x6C: LUMEN_NI("ds_mskor_rtn_b64"); break;
		case 0x6D: LUMEN_NI("ds_wrxchg_rtn_b64"); break;
		case 0x6E: LUMEN_NI("ds_wrxchg2_rtn_b64"); break;
		case 0x6F: LUMEN_NI("ds_wrxchg2st64_rtn_b64"); break;
		case 0x70: LUMEN_NI("ds_cmpst_rtn_b64"); break;
		case 0x71: LUMEN_NI("ds_cmpst_rtn_f64"); break;
		case 0x72: LUMEN_NI("ds_min_rtn_f64"); break;
		case 0x73: LUMEN_NI("ds_max_rtn_f64"); break;
		case 0x76: LUMEN_NI("ds_read_b64"); break;
		case 0x77: LUMEN_NI("ds_read2_b64"); break;
		case 0x78: LUMEN_NI("ds_read2st64_b64"); break;
		case 0x7E: LUMEN_NI("ds_condxchg32_rtn_b64"); break;
		case 0x80: LUMEN_NI("ds_add_src2_u32"); break;
		case 0x81: LUMEN_NI("ds_sub_src2_u32"); break;
		case 0x82: LUMEN_NI("ds_rsub_src2_u32"); break;
		case 0x83: LUMEN_NI("ds_inc_src2_u32"); break;
		case 0x84: LUMEN_NI("ds_dec_src2_u32"); break;
		case 0x85: LUMEN_NI("ds_min_src2_i32"); break;
		case 0x86: LUMEN_NI("ds_max_src2_i32"); break;
		case 0x87: LUMEN_NI("ds_min_src2_u32"); break;
		case 0x88: LUMEN_NI("ds_max_src2_u32"); break;
		case 0x89: LUMEN_NI("ds_and_src2_b32"); break;
		case 0x8A: LUMEN_NI("ds_or_src2_b32"); break;
		case 0x8B: LUMEN_NI("ds_xor_src2_b32"); break;
		case 0x8D: LUMEN_NI("ds_write_src2_b32"); break;
		case 0x92: LUMEN_NI("ds_min_src2_f32"); break;
		case 0x93: LUMEN_NI("ds_max_src2_f32"); break;
		case 0xC0: LUMEN_NI("ds_add_src2_u64"); break;
		case 0xC1: LUMEN_NI("ds_sub_src2_u64"); break;
		case 0xC2: LUMEN_NI("ds_rsub_src2_u64"); break;
		case 0xC3: LUMEN_NI("ds_inc_src2_u64"); break;
		case 0xC4: LUMEN_NI("ds_dec_src2_u64"); break;
		case 0xC5: LUMEN_NI("ds_min_src2_i64"); break;
		case 0xC6: LUMEN_NI("ds_max_src2_i64"); break;
		case 0xC7: LUMEN_NI("ds_min_src2_u64"); break;
		case 0xC8: LUMEN_NI("ds_max_src2_u64"); break;
		case 0xC9: LUMEN_NI("ds_and_src2_b64"); break;
		case 0xCA: LUMEN_NI("ds_or_src2_b64"); break;
		case 0xCB: LUMEN_NI("ds_xor_src2_b64"); break;
		case 0xCD: LUMEN_NI("ds_write_src2_b64"); break;
		case 0xD2: LUMEN_NI("ds_min_src2_f64"); break;
		case 0xD3: LUMEN_NI("ds_max_src2_f64"); break;
		case 0xDE: LUMEN_NI("ds_write_b96"); break;
		case 0xDF: LUMEN_NI("ds_write_b128"); break;
		case 0xFD: LUMEN_NI("ds_condxchg32_rtn_b128"); break;
		case 0xFE: LUMEN_NI("ds_read_b96"); break;
		case 0xFF: LUMEN_NI("ds_read_b128"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_mimg)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("mimg");

	uint32_t slc    = (ShaderReadSourceWord(buffer, 0) >> 25u) & 0x1u;
	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 18u) & 0x7fu;
	uint32_t lwe    = (ShaderReadSourceWord(buffer, 0) >> 17u) & 0x1u;
	uint32_t tff    = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x1u;
	uint32_t r128   = (ShaderReadSourceWord(buffer, 0) >> 15u) & 0x1u;
	uint32_t da     = (ShaderReadSourceWord(buffer, 0) >> 14u) & 0x1u;
	uint32_t glc    = (ShaderReadSourceWord(buffer, 0) >> 13u) & 0x1u;
	uint32_t unrm   = (ShaderReadSourceWord(buffer, 0) >> 12u) & 0x1u;
	uint32_t dmask  = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0xfu;

	uint32_t ssamp = (ShaderReadSourceWord(buffer, 1) >> 21u) & 0x1fu; // S#
	uint32_t srsrc = (ShaderReadSourceWord(buffer, 1) >> 16u) & 0x1fu; // T#
	uint32_t vdata = (ShaderReadSourceWord(buffer, 1) >> 8u) & 0xffu;
	uint32_t vaddr = (ShaderReadSourceWord(buffer, 1) >> 0u) & 0xffu;
	const bool integer_image_op = opcode == 0x00 || opcode == 0x01 || opcode == 0x08 || opcode == 0x09;
	const bool coherent_store   = opcode == 0x08 || opcode == 0x09;

	EXIT_NOT_IMPLEMENTED(da == 1);
	EXIT_NOT_IMPLEMENTED(r128 == 1);
	EXIT_NOT_IMPLEMENTED(tff == 1);
	EXIT_NOT_IMPLEMENTED(lwe == 1);
	EXIT_NOT_IMPLEMENTED(glc == 1 && !coherent_store);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(unrm == 1 && !integer_image_op);
	// EXIT_NOT_IMPLEMENTED(dmask != 0xf && dmask != 0x7);

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc                       = pc;
	inst.dst                      = operand_parse(vdata + 256);
	inst.src_num                  = 3;
	inst.src[0]                   = operand_parse(vaddr + 256);
	inst.src[1]                   = operand_parse(srsrc * 4);
	inst.src[2]                   = operand_parse(ssamp * 4);
	inst.globally_coherent        = glc == 1;
	inst.unnormalized_coordinates = unrm == 1;

	switch (opcode)
	{
		case 0x00:
			inst.type        = ShaderInstructionType::ImageLoad;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			if (dmask == 0xf)
			{
				inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StDmaskF;
				inst.dst.size = 4;
			}
			break;
		case 0x01:
		{
			inst.type        = ShaderInstructionType::ImageLoadMip;
			inst.src[0].size = 4;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			int lm_popcount  = 0;
			for (int b = 0; b < 4; b++)
			{
				if ((dmask >> b) & 1)
				{
					lm_popcount++;
				}
			}
			inst.dst.size = lm_popcount;
			switch (dmask)
			{
				case 0x1: inst.format = ShaderInstructionFormat::Vdata1Vaddr4StDmask1; break;
				case 0x2: inst.format = ShaderInstructionFormat::Vdata1Vaddr4StDmask2; break;
				case 0x3: inst.format = ShaderInstructionFormat::Vdata2Vaddr4StDmask3; break;
				case 0x5: inst.format = ShaderInstructionFormat::Vdata2Vaddr4StDmask5; break;
				case 0x7: inst.format = ShaderInstructionFormat::Vdata3Vaddr4StDmask7; break;
				case 0x8: inst.format = ShaderInstructionFormat::Vdata1Vaddr4StDmask8; break;
				case 0x9: inst.format = ShaderInstructionFormat::Vdata2Vaddr4StDmask9; break;
				case 0xf: inst.format = ShaderInstructionFormat::Vdata4Vaddr4StDmaskF; break;
				default: break;
			}
			break;
		}
		case 0x02: LUMEN_NI("image_load_pck"); break;
		case 0x03: LUMEN_NI("image_load_pck_sgn"); break;
		case 0x04: LUMEN_NI("image_load_mip_pck"); break;
		case 0x05: LUMEN_NI("image_load_mip_pck_sgn"); break;
		case 0x08:
			inst.type        = ShaderInstructionType::ImageStore;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			if (dmask == 0xf)
			{
				inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StDmaskF;
				inst.dst.size = 4;
			}
			break;
		case 0x09:
			inst.type        = ShaderInstructionType::ImageStoreMip;
			inst.src[0].size = 4;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			if (dmask == 0xf)
			{
				inst.format   = ShaderInstructionFormat::Vdata4Vaddr4StDmaskF;
				inst.dst.size = 4;
			}
			break;
		case 0x0A: LUMEN_NI("image_store_pck"); break;
		case 0x0B: LUMEN_NI("image_store_mip_pck"); break;
		case 0x0E:
		{
			inst.type        = ShaderInstructionType::ImageGetResinfo;
			inst.src[0].size = 1;
			inst.src[1].size = 8;
			inst.src_num     = 2;
			int popcount     = 0;
			for (int b = 0; b < 4; b++)
			{
				if ((dmask >> b) & 1)
				{
					popcount++;
				}
			}
			inst.dst.size = popcount;
			switch (dmask)
			{
				case 0x1: inst.format = ShaderInstructionFormat::Vdata1Vaddr1StDmask1; break;
				case 0x2: inst.format = ShaderInstructionFormat::Vdata1Vaddr1StDmask2; break;
				case 0x3: inst.format = ShaderInstructionFormat::Vdata2Vaddr1StDmask3; break;
				case 0x7: inst.format = ShaderInstructionFormat::Vdata3Vaddr1StDmask7; break;
				case 0xf: inst.format = ShaderInstructionFormat::Vdata4Vaddr1StDmaskF; break;
				default: break;
			}
			break;
		}
		case 0x0F: LUMEN_NI("image_atomic_swap"); break;
		case 0x10: LUMEN_NI("image_atomic_cmpswap"); break;
		case 0x11: LUMEN_NI("image_atomic_add"); break;
		case 0x12: LUMEN_NI("image_atomic_sub"); break;
		case 0x13: LUMEN_NI("image_atomic_rsub"); break;
		case 0x14: LUMEN_NI("image_atomic_smin"); break;
		case 0x15: LUMEN_NI("image_atomic_umin"); break;
		case 0x16: LUMEN_NI("image_atomic_smax"); break;
		case 0x17: LUMEN_NI("image_atomic_umax"); break;
		case 0x18: LUMEN_NI("image_atomic_and"); break;
		case 0x19: LUMEN_NI("image_atomic_or"); break;
		case 0x1A: LUMEN_NI("image_atomic_xor"); break;
		case 0x1B: LUMEN_NI("image_atomic_inc"); break;
		case 0x1C: LUMEN_NI("image_atomic_dec"); break;
		case 0x1D: LUMEN_NI("image_atomic_fcmpswap"); break;
		case 0x1E: LUMEN_NI("image_atomic_fmin"); break;
		case 0x1F: LUMEN_NI("image_atomic_fmax"); break;
		case 0x20:
			inst.type        = ShaderInstructionType::ImageSample;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask)
			{
				case 0x1:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1;
					inst.dst.size = 1;
					break;
				}
				case 0x2:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2;
					inst.dst.size = 1;
					break;
				}
				case 0x3:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3;
					inst.dst.size = 2;
					break;
				}
				case 0x4:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4;
					inst.dst.size = 1;
					break;
				}
				case 0x5:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5;
					inst.dst.size = 2;
					break;
				}
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				case 0x8:
				{
					inst.format   = ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8;
					inst.dst.size = 1;
					break;
				}
				case 0x9:
				{
					inst.format   = ShaderInstructionFormat::Vdata2Vaddr3StSsDmask9;
					inst.dst.size = 2;
					break;
				}
				case 0xf:
				{
					inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF;
					inst.dst.size = 4;
					break;
				}
				default:;
			}
			break;
		case 0x21: LUMEN_NI("image_sample_cl"); break;
		case 0x22: LUMEN_NI("image_sample_d"); break;
		case 0x23: LUMEN_NI("image_sample_d_cl"); break;
		case 0x24: LUMEN_NI("image_sample_l"); break;
		case 0x25: LUMEN_NI("image_sample_b"); break;
		case 0x26: LUMEN_NI("image_sample_b_cl"); break;
		case 0x27:
			inst.type        = ShaderInstructionType::ImageSampleLz;
			inst.src[0].size = 3;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask) // NOLINT
			{
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				case 0xf:
				{
					inst.format   = ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF;
					inst.dst.size = 4;
					break;
				}
				default:;
			}
			break;
		case 0x28: LUMEN_NI("image_sample_c"); break;
		case 0x29: LUMEN_NI("image_sample_c_cl"); break;
		case 0x2A: LUMEN_NI("image_sample_c_d"); break;
		case 0x2B: LUMEN_NI("image_sample_c_d_cl"); break;
		case 0x2C: LUMEN_NI("image_sample_c_l"); break;
		case 0x2D: LUMEN_NI("image_sample_c_b"); break;
		case 0x2E: LUMEN_NI("image_sample_c_b_cl"); break;
		case 0x2F: LUMEN_NI("image_sample_c_lz"); break;
		case 0x30: LUMEN_NI("image_sample_o"); break;
		case 0x31: LUMEN_NI("image_sample_cl_o"); break;
		case 0x32: LUMEN_NI("image_sample_d_o"); break;
		case 0x33: LUMEN_NI("image_sample_d_cl_o"); break;
		case 0x34: LUMEN_NI("image_sample_l_o"); break;
		case 0x35: LUMEN_NI("image_sample_b_o"); break;
		case 0x36: LUMEN_NI("image_sample_b_cl_o"); break;
		case 0x37:
			inst.type        = ShaderInstructionType::ImageSampleLzO;
			inst.src[0].size = 4;
			inst.src[1].size = 8;
			inst.src[2].size = 4;
			switch (dmask) // NOLINT
			{
				case 0x7:
				{
					inst.format   = ShaderInstructionFormat::Vdata3Vaddr4StSsDmask7;
					inst.dst.size = 3;
					break;
				}
				default:;
			}
			break;
		case 0x38: LUMEN_NI("image_sample_c_o"); break;
		case 0x39: LUMEN_NI("image_sample_c_cl_o"); break;
		case 0x3A: LUMEN_NI("image_sample_c_d_o"); break;
		case 0x3B: LUMEN_NI("image_sample_c_d_cl_o"); break;
		case 0x3C: LUMEN_NI("image_sample_c_l_o"); break;
		case 0x3D: LUMEN_NI("image_sample_c_b_o"); break;
		case 0x3E: LUMEN_NI("image_sample_c_b_cl_o"); break;
		case 0x3F: LUMEN_NI("image_sample_c_lz_o"); break;
		case 0x40: LUMEN_NI("image_gather4"); break;
		case 0x41: LUMEN_NI("image_gather4_cl"); break;
		case 0x44: LUMEN_NI("image_gather4_l"); break;
		case 0x45: LUMEN_NI("image_gather4_b"); break;
		case 0x46: LUMEN_NI("image_gather4_b_cl"); break;
		case 0x47: LUMEN_NI("image_gather4_lz"); break;
		case 0x48: LUMEN_NI("image_gather4_c"); break;
		case 0x49: LUMEN_NI("image_gather4_c_cl"); break;
		case 0x4C: LUMEN_NI("image_gather4_c_l"); break;
		case 0x4D: LUMEN_NI("image_gather4_c_b"); break;
		case 0x4E: LUMEN_NI("image_gather4_c_b_cl"); break;
		case 0x4F: LUMEN_NI("image_gather4_c_lz"); break;
		case 0x50: LUMEN_NI("image_gather4_o"); break;
		case 0x51: LUMEN_NI("image_gather4_cl_o"); break;
		case 0x54: LUMEN_NI("image_gather4_l_o"); break;
		case 0x55: LUMEN_NI("image_gather4_b_o"); break;
		case 0x56: LUMEN_NI("image_gather4_b_cl_o"); break;
		case 0x57: LUMEN_NI("image_gather4_lz_o"); break;
		case 0x58: LUMEN_NI("image_gather4_c_o"); break;
		case 0x59: LUMEN_NI("image_gather4_c_cl_o"); break;
		case 0x5C: LUMEN_NI("image_gather4_c_l_o"); break;
		case 0x5D: LUMEN_NI("image_gather4_c_b_o"); break;
		case 0x5E: LUMEN_NI("image_gather4_c_b_cl_o"); break;
		case 0x5F: LUMEN_NI("image_gather4_c_lz_o"); break;
		case 0x60: LUMEN_NI("image_get_lod"); break;
		case 0x68: LUMEN_NI("image_sample_cd"); break;
		case 0x69: LUMEN_NI("image_sample_cd_cl"); break;
		case 0x6A: LUMEN_NI("image_sample_c_cd"); break;
		case 0x6B: LUMEN_NI("image_sample_c_cd_cl"); break;
		case 0x6C: LUMEN_NI("image_sample_cd_o"); break;
		case 0x6D: LUMEN_NI("image_sample_cd_cl_o"); break;
		case 0x6E: LUMEN_NI("image_sample_c_cd_o"); break;
		case 0x6F: LUMEN_NI("image_sample_c_cd_cl_o"); break;
		case 0x7E: LUMEN_NI("image_rsrc256"); break;
		case 0x7F: LUMEN_NI("image_sampler"); break;
		case 0xA0: LUMEN_NI("image_sample_a"); break;
		case 0xA1: LUMEN_NI("image_sample_cl_a"); break;
		case 0xA5: LUMEN_NI("image_sample_b_a"); break;
		case 0xA6: LUMEN_NI("image_sample_b_cl_a"); break;
		case 0xA8: LUMEN_NI("image_sample_c_a"); break;
		case 0xA9: LUMEN_NI("image_sample_c_cl_a"); break;
		case 0xAD: LUMEN_NI("image_sample_c_b_a"); break;
		case 0xAE: LUMEN_NI("image_sample_c_b_cl_a"); break;
		case 0xB0: LUMEN_NI("image_sample_o_a"); break;
		case 0xB1: LUMEN_NI("image_sample_cl_o_a"); break;
		case 0xB5: LUMEN_NI("image_sample_b_o_a"); break;
		case 0xB6: LUMEN_NI("image_sample_b_cl_o_a"); break;
		case 0xB8: LUMEN_NI("image_sample_c_o_a"); break;
		case 0xB9: LUMEN_NI("image_sample_c_cl_o_a"); break;
		case 0xBD: LUMEN_NI("image_sample_c_b_o_a"); break;
		case 0xBE: LUMEN_NI("image_sample_c_b_cl_o_a"); break;
		case 0xC0: LUMEN_NI("image_gather4_a"); break;
		case 0xC1: LUMEN_NI("image_gather4_cl_a"); break;
		case 0xC5: LUMEN_NI("image_gather4_b_a"); break;
		case 0xC6: LUMEN_NI("image_gather4_b_cl_a"); break;
		case 0xC8: LUMEN_NI("image_gather4_c_a"); break;
		case 0xC9: LUMEN_NI("image_gather4_c_cl_a"); break;
		case 0xCD: LUMEN_NI("image_gather4_c_b_a"); break;
		case 0xCE: LUMEN_NI("image_gather4_c_b_cl_a"); break;
		case 0xD0: LUMEN_NI("image_gather4_o_a"); break;
		case 0xD1: LUMEN_NI("image_gather4_cl_o_a"); break;
		case 0xD5: LUMEN_NI("image_gather4_b_o_a"); break;
		case 0xD6: LUMEN_NI("image_gather4_b_cl_o_a"); break;
		case 0xD8: LUMEN_NI("image_gather4_c_o_a"); break;
		case 0xD9: LUMEN_NI("image_gather4_c_cl_o_a"); break;
		case 0xDD: LUMEN_NI("image_gather4_c_b_o_a"); break;
		case 0xDE: LUMEN_NI("image_gather4_c_b_cl_o_a"); break;

		default: LUMEN_UNKNOWN_OP();
	}

	if (inst.format == ShaderInstructionFormat::Unknown)
	{
		printf("%s", dst->DbgDump().c_str());
		EXIT("unknown mimg format for opcode: 0x%02" PRIx32 " at addr 0x%08" PRIx32 ", dmask: 0x%" PRIx32 "\n", opcode, pc, dmask);
	}
	if (inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2 ||
	    inst.format == ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4)
		EXIT_NOT_IMPLEMENTED(!ShaderImageSampleSingleComponentInstructionSupported(inst));

	dst->GetInstructions().Add(inst);

	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
LUMEN_SHADER_PARSER(shader_parse_mtbuf)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("mtbuf");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x7u;
	uint32_t dfmt   = (ShaderReadSourceWord(buffer, 0) >> 19u) & 0xfu;
	uint32_t nfmt   = (ShaderReadSourceWord(buffer, 0) >> 23u) & 0x7u;
	uint32_t glc    = (ShaderReadSourceWord(buffer, 0) >> 14u) & 0x1u;
	uint32_t idxen  = (ShaderReadSourceWord(buffer, 0) >> 13u) & 0x1u;
	uint32_t offen  = (ShaderReadSourceWord(buffer, 0) >> 12u) & 0x1u;
	uint32_t offset = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xfffu;

	uint32_t soffset = (ShaderReadSourceWord(buffer, 1) >> 24u) & 0xffu;
	uint32_t tfe     = (ShaderReadSourceWord(buffer, 1) >> 23u) & 0x1u;
	uint32_t slc     = (ShaderReadSourceWord(buffer, 1) >> 22u) & 0x1u;
	uint32_t srsrc   = (ShaderReadSourceWord(buffer, 1) >> 16u) & 0x1fu;
	uint32_t vdata   = (ShaderReadSourceWord(buffer, 1) >> 8u) & 0xffu;
	uint32_t vaddr   = (ShaderReadSourceWord(buffer, 1) >> 0u) & 0xffu;

	EXIT_NOT_IMPLEMENTED(idxen == 0);
	// EXIT_NOT_IMPLEMENTED(offen == 1);
	EXIT_NOT_IMPLEMENTED(offset != 0);
	EXIT_NOT_IMPLEMENTED(glc == 1);
	EXIT_NOT_IMPLEMENTED(slc == 1);
	EXIT_NOT_IMPLEMENTED(tfe == 1);
	// EXIT_NOT_IMPLEMENTED(dfmt != 14);
	// EXIT_NOT_IMPLEMENTED(nfmt != 7);

	if ((dfmt != 14 && dfmt != 4) || nfmt != 7)
	{
		EXIT("unknown format: dfmt = %d, nfmt = %d at addr 0x%08" PRIx32 " (hash0 = 0x%08" PRIx32 ", crc32 = 0x%08" PRIx32 ")\n", dfmt,
		     nfmt, pc, dst->GetHash0(), dst->GetCrc32());
	}

	uint32_t size = 2;

	ShaderInstruction inst;
	inst.pc      = pc;
	inst.dst     = operand_parse(vdata + 256);
	inst.src_num = 3;
	inst.src[0]  = operand_parse(vaddr + 256);
	inst.src[1]  = operand_parse(srsrc * 4);
	inst.src[2]  = operand_parse(soffset);

	if (inst.src[2].type == ShaderOperandType::LiteralConstant)
	{
		inst.src[2].constant.u = ShaderReadSourceWord(buffer, size);
		size++;
	}

	inst.src[1].size = 4;

	switch (opcode)
	{
		case 0x00:
			inst.type   = ShaderInstructionType::TBufferLoadFormatX;
			inst.format = ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1;
			EXIT_NOT_IMPLEMENTED(offen == 1);
			EXIT_NOT_IMPLEMENTED(!(dfmt == 4 && nfmt == 7));
			break;
		case 0x01: LUMEN_NI("tbuffer_load_format_xy"); break;
		case 0x02: LUMEN_NI("tbuffer_load_format_xyz"); break;
		case 0x03:
			inst.type   = ShaderInstructionType::TBufferLoadFormatXyzw;
			inst.format = (offen == 1 ? ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4
			                          : ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4);
			inst.src[0].size += static_cast<int>(offen);
			inst.dst.size = 4;
			EXIT_NOT_IMPLEMENTED(!(dfmt == 14 && nfmt == 7));
			break;
		case 0x04: LUMEN_NI("tbuffer_store_format_x"); break;
		case 0x05: LUMEN_NI("tbuffer_store_format_xy"); break;
		case 0x06: LUMEN_NI("tbuffer_store_format_xyz"); break;
		case 0x07: LUMEN_NI("tbuffer_store_format_xyzw"); break;
		case 0x08: LUMEN_NI("tbuffer_load_format_d16_x"); break;
		case 0x09: LUMEN_NI("tbuffer_load_format_d16_xy"); break;
		case 0x0A: LUMEN_NI("tbuffer_load_format_d16_xyz"); break;
		case 0x0B: LUMEN_NI("tbuffer_load_format_d16_xyzw"); break;
		case 0x0C: LUMEN_NI("tbuffer_store_format_d16_x"); break;
		case 0x0D: LUMEN_NI("tbuffer_store_format_d16_xy"); break;
		case 0x0E: LUMEN_NI("tbuffer_store_format_d16_xyz"); break;
		case 0x0F: LUMEN_NI("tbuffer_store_format_d16_xyzw"); break;
		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return size;
}

LUMEN_SHADER_PARSER(shader_parse_vintrp)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer == nullptr || buffer < src);

	LUMEN_TYPE_STR("vintrp");

	uint32_t opcode = (ShaderReadSourceWord(buffer, 0) >> 16u) & 0x3u;
	uint32_t vdst   = (ShaderReadSourceWord(buffer, 0) >> 18u) & 0xffu;
	uint32_t attr   = (ShaderReadSourceWord(buffer, 0) >> 10u) & 0x3fu;
	uint32_t chan   = (ShaderReadSourceWord(buffer, 0) >> 8u) & 0x3u;
	uint32_t vsrc   = (ShaderReadSourceWord(buffer, 0) >> 0u) & 0xffu;

	ShaderInstruction inst;
	inst.pc                = pc;
	inst.src[0]            = operand_parse(vsrc + 256);
	inst.dst               = operand_parse(vdst + 256);
	inst.src[1].type       = ShaderOperandType::IntegerInlineConstant;
	inst.src[1].constant.u = attr;
	inst.src[2].type       = ShaderOperandType::IntegerInlineConstant;
	inst.src[2].constant.u = chan;
	inst.src_num           = 3;

	inst.format = ShaderInstructionFormat::VdstVsrcAttrChan;

	switch (opcode)
	{
		case 0x00: inst.type = ShaderInstructionType::VInterpP1F32; break;
		case 0x01: inst.type = ShaderInstructionType::VInterpP2F32; break;
		case 0x02:
			inst.type              = ShaderInstructionType::VInterpMovF32;
			inst.src[0].type       = ShaderOperandType::IntegerInlineConstant;
			inst.src[0].constant.u = vsrc & 0x3u;
			inst.src[0].size       = 0;
			break;
		default: LUMEN_UNKNOWN_OP();
	}

	dst->GetInstructions().Add(inst);

	return 1;
}

static uint32_t shader_parse_instruction(LUMEN_SHADER_PARSER_ARGS)
{
	const uint32_t instruction = ShaderReadSourceWord(buffer, 0);
	if ((instruction & 0x80000000u) == 0x00000000)
	{
		return shader_parse_vop2(pc, src, buffer, dst);
	} else if ((instruction & 0xF8000000u) == 0xC0000000)
	{
		return shader_parse_smrd(pc, src, buffer, dst);
	} else if ((instruction & 0xC0000000u) == 0x80000000)
	{
		return shader_parse_sop2(pc, src, buffer, dst);
	} else
	{
		switch (instruction >> 26u)
		{
			case 0x32: return shader_parse_vintrp(pc, src, buffer, dst);
			case 0x34: return shader_parse_vop3(pc, src, buffer, dst);
			case 0x36: return shader_parse_ds(pc, src, buffer, dst);
			case 0x38: return shader_parse_mubuf(pc, src, buffer, dst);
			case 0x3a: return shader_parse_mtbuf(pc, src, buffer, dst);
			case 0x3c: return shader_parse_mimg(pc, src, buffer, dst);
			case 0x3e: return shader_parse_exp(pc, src, buffer, dst);
			default:
			{
				printf("%s", dst->DbgDump().c_str());
				EXIT("unknown code 0x%08" PRIx32 " at addr 0x%08" PRIx32 "\n", ShaderReadSourceWord(buffer, 0), pc);
				return 0;
			}
		}
	}
}

template <bool Bounded>
static uint32_t shader_parse(LUMEN_SHADER_PARSER_ARGS, uint32_t source_dwords = UINT32_MAX)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_IF(buffer != nullptr);

	auto type = dst->GetType();

	dst->GetInstructions().Clear();
	dst->GetLabels().Clear();
	dst->GetIndirectLabels().Clear();

	const auto* ptr = src + pc / 4;
	for (;;)
	{
		if constexpr (Bounded)
		{
			const auto offset = static_cast<uint32_t>(ptr - src);
			if (offset >= source_dwords) return 0;
		}
		auto instruction = ShaderReadSourceWord(ptr, 0);
		auto pc          = 4 * static_cast<uint32_t>(ptr - src);
		if constexpr (Bounded)
		{
			if ((instruction & 0xffff0000u) == 0xbf960000u &&
			    !ShaderThreadTraceDataFormSupported(*dst, instruction & 0xffffu)) return 0;
		}

		uint32_t consumed = 0;
		if constexpr (Bounded)
		{
			const uint32_t remaining = source_dwords - static_cast<uint32_t>(ptr - src);
			if (remaining < 5 && instruction != 0xbf810000u)
			{
				// Missing encoding words must be refused before semantic checks:
				// e.g. a fabricated SDWA control word would trigger an unrelated
				// unsupported-form error before the decoder returns its length.
				const uint32_t family = instruction >> 26u;
				const bool two_word_family = family == 0x34 || family == 0x36 || family == 0x38 ||
				    family == 0x3a || family == 0x3c || family == 0x3e;
				const uint32_t vector_source = instruction & 0x1ffu;
				const bool vector_extension = (instruction & 0x80000000u) == 0 &&
				    (vector_source == 249 || vector_source == 250);
				if (remaining < 2 && (two_word_family || vector_extension)) return 0;
				// Decoders inspect at most two encoding words plus three literals,
				// and count every inspected word in their returned extent. Isolate
				// the short tail so no decoder can read outside the owned source.
				// Padding is never accepted as source: publish the candidate AST
				// only after its complete encoding fits in the actual remaining span.
				uint32_t tail[5] {};
				EXIT_IF(!ShaderPrepareMemoryRead(reinterpret_cast<uint64_t>(ptr), remaining * sizeof(uint32_t)));
				std::memcpy(tail, ptr, remaining * sizeof(uint32_t));
				ShaderCode candidate = *dst;
				ScopedShaderPrivateRead private_tail(tail, sizeof(tail));
				consumed = shader_parse_instruction(pc, tail, tail, &candidate);
				if (consumed == 0 || consumed > remaining) return 0;
				*dst = std::move(candidate);
			} else
			{
				consumed = shader_parse_instruction(pc, src, ptr, dst);
			}
		} else
		{
			consumed = shader_parse_instruction(pc, src, ptr, dst);
		}
		ptr += consumed;

		if ((instruction == 0xBF810000 && (type == ShaderType::Vertex || type == ShaderType::Pixel || type == ShaderType::Compute) &&
		     !dst->GetLabels().Contains(4 * static_cast<uint32_t>(ptr - src), [](auto label, auto pc) { return label.GetDst() == pc; })) ||
		    (instruction == 0xBE802000 && type == ShaderType::Fetch))
		{
			break;
		}
	}

	return ptr - src;
}

void ShaderParse(const uint32_t* src, ShaderCode* dst)
{
	shader_parse<false>(0, src, nullptr, dst);
}

uint32_t ShaderTryParseBounded(const uint32_t* src, uint32_t source_dwords, ShaderCode* dst)
{
	return shader_parse<true>(0, src, nullptr, dst, source_dwords);
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
