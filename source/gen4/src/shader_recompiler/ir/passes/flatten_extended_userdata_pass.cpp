// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <vector>
#include <boost/container/flat_map.hpp>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#include "common/arch.h"
#include "common/decoder.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/srt_gvn_table.h"
#include "shader_recompiler/ir/value.h"

#if defined(ARCH_ARM64) && (defined(__linux__) || defined(__APPLE__))
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif

namespace {

struct ReadConstAddressSources {
    Shader::Optimization::ReadConstSourceMap canonical;

    void Initialize(const Shader::IR::Program& program,
                    Shader::Optimization::SrtGvnTable& gvn_table) {
        canonical = Shader::Optimization::ResolveReadConstPhiSources(program);
        for (const auto& [inst, address] : canonical) {
            gvn_table.AssignUniqueValueNumber(inst);
        }
    }

    std::array<Shader::IR::Value, 2> GetAddress(Shader::IR::Inst* inst) const {
        if (const auto it = canonical.find(inst); it != canonical.end()) {
            return it->second;
        }
        auto* base = inst->Arg(0).Inst();
        if (base->GetOpcode() == Shader::IR::Opcode::CompositeConstructU32x2x2) {
            base = base->Arg(0).Inst();
        }
        return {base->Arg(0), base->Arg(1)};
    }
};

} // namespace

#ifdef ARCH_X86_64

using namespace Xbyak::util;

static Xbyak::CodeGenerator g_srt_codegen(32_MB);
static const u8* g_srt_codegen_start = nullptr;

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    const auto func_addr = (PFN_SrtWalker)g_srt_codegen.getCurr();
    g_srt_codegen.db(ptr, size);
    g_srt_codegen.ready();
    return func_addr;
}

} // namespace Shader

namespace {

static void DumpSrtProgram(const Shader::Info& info, const u8* code, size_t codesize) {
    using namespace Common::FS;

    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}.srtprogram.txt", info.stage, info.pgm_hash);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create, FileType::TextFile};

    u64 address = reinterpret_cast<u64>(code);
    u64 code_end = address + codesize;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = ZYAN_STATUS_SUCCESS;
    while (address < code_end && ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                                     instruction, operands, reinterpret_cast<void*>(address)))) {
        std::string s =
            Common::Decoder::Instance()->disassembleInst(instruction, operands, address);
        s += "\n";
        file.WriteString(s);
        address += instruction.length;
    }
}

static bool SrtWalkerSignalHandler(void* context, void* fault_address) {
    // Only handle if the fault address is within the SRT code range
    const u8* code_start = g_srt_codegen_start;
    const u8* code_end = code_start + g_srt_codegen.getSize();
    const void* code = Common::GetRip(context);
    if (code < code_start || code >= code_end) {
        return false; // Not in SRT code range
    }

    // Patch instruction to zero register
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(instruction, operands,
                                                                       const_cast<void*>(code), 15);

    ASSERT(ZYAN_SUCCESS(status) && instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY);

    size_t len = instruction.length;
    const size_t patch_size = 3;
    u8* code_patch = const_cast<u8*>(reinterpret_cast<const u8*>(code));

    // We can only encounter rdi or r10d as the first operand in a
    // fault memory access for SRT walker.
    switch (operands[0].reg.value) {
    case ZYDIS_REGISTER_RDI:
        // mov rdi, [rdi + (off_dw << 2)] -> xor rdi, rdi
        code_patch[0] = 0x48;
        code_patch[1] = 0x31;
        code_patch[2] = 0xFF;
        break;
    case ZYDIS_REGISTER_R10D:
        // mov r10d, [rdi + (off_dw << 2)] -> xor r10d, r10d
        code_patch[0] = 0x45;
        code_patch[1] = 0x31;
        code_patch[2] = 0xD2;
        break;
    default:
        UNREACHABLE_MSG("Unsupported register for SRT walker patch");
        return false;
    }

    // Fill nops
    memset(code_patch + patch_size, 0x90, len - patch_size);

    LOG_WARNING(Render_Recompiler, "Patched SRT walker at {}, fault address {}", code,
                fault_address);

    return true;
}

using namespace Shader;

struct PassInfo {
    struct PointerListCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            // Keep order with the following criteria:
            // 1. If immediate, sort by immediate value
            // 2. If not immediate, sort by instruction order
            // 3. Immediates come first, then instructions
            if (a.IsImmediate() && b.IsImmediate()) {
                return a.U32() < b.U32();
            } else if (!a.IsImmediate() && !b.IsImmediate()) {
                auto a_inst = a.Inst();
                auto b_inst = b.Inst();
                auto a_block = a_inst->GetParent();
                auto b_block = b_inst->GetParent();
                if (a_block != b_block) {
                    return a_block < b_block; // Sort by block address
                }

                auto it = a_block->Instructions().iterator_to(*a_inst);
                auto end = a_block->Instructions().end();
                for (; it != end; ++it) {
                    if (&*it == b_inst) {
                        return true;
                    }
                }
                return false;
            } else {
                return a.IsImmediate(); // Immediates come first
            }
        }
    };

    // map offset to inst
    using PtrUserList =
        boost::container::flat_map<IR::Value, boost::container::small_vector<IR::Inst*, 1>,
                                   PointerListCompare>;

    Optimization::SrtGvnTable gvn_table;
    ReadConstAddressSources readconst_sources;
    // keys are GetUserData, ReadConst or ReadConstBuffer instructions that are used as pointers
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    // GetUserData instructions corresponding to sgpr_base of SRT roots
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;

    // pick a single inst for a given value number
    std::unordered_map<u32, IR::Inst*> vn_to_inst;

    // Bumped during codegen to assign offsets to readconsts
    u16 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        if (it != pointer_uses.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Return a single instruction that this instruction is identical to, according
    // to value number
    // The "original" is arbitrary. Here it's the first instruction found for a given value number
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};
} // namespace

namespace Shader::Optimization {

namespace {

static inline u16 GetFlatbufOffset(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        return inst_info.flatbuf_off_dw;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        return inst->Flags<u16>();
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static inline void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        inst_info.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(inst_info);
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        inst->SetFlags(offset);
        return;
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static bool ComputeOffset(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                          const IR::Value& off_dw);

#define ABORT_ON_FAILURE(expr)                                                                     \
    if (!(expr)) {                                                                                 \
        return false;                                                                              \
    }

#define POP_ABORT_ON_FAILURE(expr)                                                                 \
    if (!(expr)) {                                                                                 \
        c.add(rsp, 8);                                                                             \
        return false;                                                                              \
    }

static bool EmitComputeOffsetIAdd32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.add(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.add(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.add(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetISub32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.neg(reg);
        c.add(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.sub(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.neg(reg);
        c.add(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetIMul32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.imul(reg, reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.imul(reg, reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.imul(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetShiftLeftLogical32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                                PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.shl(reg, inst->Arg(0).U32());
        c.mov(ecx, reg);
        c.mov(reg, inst->Arg(0).U32());
        c.shl(reg, cl);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.shl(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.pop(rcx);
        c.shl(reg, cl);
    }
    return true;
}

static bool EmitComputeOffsetShiftRightLogical32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                                 PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, reg);
        c.mov(reg, inst->Arg(0).U32());
        c.shr(reg, cl);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.shr(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.pop(rcx);
        c.shr(reg, cl);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseAnd32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.and_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.and_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.and_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseOr32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                         PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.or_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.or_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.or_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseXor32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.xor_(reg, inst->Arg(0).U32());
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.xor_(reg, inst->Arg(1).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.xor_(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitwiseNot32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                          PassInfo& pass_info, IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
    c.not_(reg);
    return true;
}

static bool EmitComputeOffsetUMin32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, inst->Arg(0).U32());
        c.cmp(reg, ecx);
        c.cmova(reg, ecx);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.mov(ecx, inst->Arg(1).U32());
        c.cmp(reg, ecx);
        c.cmova(reg, ecx);
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.cmp(reg, dword[rsp]);
        c.cmova(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetUMax32(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                                    IR::Inst* inst) {
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, inst->Arg(0).U32());
        c.cmp(reg, ecx);
        c.cmovb(reg, ecx);
    } else if (inst->Arg(1).IsImmediate()) {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.mov(ecx, inst->Arg(1).U32());
        c.cmp(reg, ecx);
        c.cmovb(reg, ecx);
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
        c.push(reg.cvt64());
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.cmp(reg, dword[rsp]);
        c.cmovb(reg, dword[rsp]);
        c.add(rsp, 8);
    }
    return true;
}

static bool EmitComputeOffsetBitFieldUExtract(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg,
                                              PassInfo& pass_info, IR::Inst* inst) {
    // We asume that the count is always less than 32, Is this correct?
    ASSERT(!inst->AreAllArgsImmediates());
    if (inst->Arg(0).IsImmediate()) {
        c.mov(reg, inst->Arg(0).U32());
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(0)));
    }
    bool in_stack = false;
    if (inst->Arg(1).IsImmediate()) {
        c.shr(reg, inst->Arg(1).U32());
    } else {
        c.push(reg.cvt64());
        in_stack = true;
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(1)));
        c.mov(ecx, reg);
        c.shr(dword[rsp], cl);
    }
    if (inst->Arg(2).IsImmediate()) {
        if (in_stack) {
            c.pop(reg.cvt64());
        }
        c.and_(reg, (1U << inst->Arg(2).U32()) - 1);
    } else {
        if (!in_stack) {
            c.push(reg.cvt64());
        }
        POP_ABORT_ON_FAILURE(ComputeOffset(c, reg, pass_info, inst->Arg(2)));
        c.mov(ecx, reg);
        c.mov(edx, 1);
        c.shl(edx, cl);
        c.dec(edx);
        c.and_(dword[rsp], edx);
        c.pop(reg.cvt64());
    }
    return true;
}

static bool IsAllowedOffsetInstruction(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitwiseNot32:
    case IR::Opcode::UMin32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitFieldUExtract:
        return true;
    default:
        return false;
    }
}

static bool ComputeOffset(Xbyak::CodeGenerator& c, Xbyak::Reg32 reg, PassInfo& pass_info,
                          const IR::Value& off_dw) {
    ASSERT(reg != ecx && reg != edx);
    auto inst = off_dw.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        c.mov(reg, ptr[rsi + (static_cast<u32>(inst->Arg(0).ScalarReg()) << 2)]);
        return true;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
        if (u16 offset = GetFlatbufOffset(pass_info.DeduplicateInstruction(inst)); offset != 0) {
            c.mov(reg, ptr[rsi + (offset << 2)]);
            return true;
        }
        return false;
    case IR::Opcode::IAdd32:
        ABORT_ON_FAILURE(EmitComputeOffsetIAdd32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ISub32:
        ABORT_ON_FAILURE(EmitComputeOffsetISub32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::IMul32:
        ABORT_ON_FAILURE(EmitComputeOffsetIMul32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ShiftLeftLogical32:
        ABORT_ON_FAILURE(EmitComputeOffsetShiftLeftLogical32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::ShiftRightLogical32:
        ABORT_ON_FAILURE(EmitComputeOffsetShiftRightLogical32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseAnd32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseAnd32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseOr32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseOr32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseXor32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseXor32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitwiseNot32:
        ABORT_ON_FAILURE(EmitComputeOffsetBitwiseNot32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::UMin32:
        ABORT_ON_FAILURE(EmitComputeOffsetUMin32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::UMax32:
        ABORT_ON_FAILURE(EmitComputeOffsetUMax32(c, reg, pass_info, inst));
        return true;
    case IR::Opcode::BitFieldUExtract:
        ABORT_ON_FAILURE(EmitComputeOffsetBitFieldUExtract(c, reg, pass_info, inst));
        return true;
    default:
        LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                  magic_enum::enum_name(inst->GetOpcode()));
        return false;
    }
}

static inline bool PushPtr(Xbyak::CodeGenerator& c, PassInfo& pass_info, const IR::Value& off_dw) {
    c.push(rdi);
    if (off_dw.IsImmediate()) {
        c.mov(rdi, ptr[rdi + (off_dw.U32() << 2)]);
    } else {
        ABORT_ON_FAILURE(ComputeOffset(c, r10d, pass_info, off_dw));
        c.shl(r10d, 2);
        c.mov(r10d, r10d);
        c.mov(rdi, ptr[rdi + r10]);
    }
    c.mov(r10, 0xFFFFFFFFFFFFULL);
    c.and_(rdi, r10);
    return true;
}

static inline void PopPtr(Xbyak::CodeGenerator& c) {
    c.pop(rdi);
}

static void VisitPointer(const IR::Value& off_dw, IR::Inst* subtree, PassInfo& pass_info,
                         Xbyak::CodeGenerator& c) {
    if (subtree->GetOpcode() == IR::Opcode::ReadConst && subtree->Flags<u16>() == 0 ||
        subtree->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw == 0) {
        return;
    }

    if (!PushPtr(c, pass_info, off_dw)) {
        LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
        return;
    }
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);

    // First copy all the src data from this tree level
    // That way, all data that was contiguous in the guest SRT is also contiguous in the
    // flattened buffer.
    // TODO src and dst are contiguous. Optimize with wider loads/stores
    // TODO if this subtree is dynamically indexed, don't compact it (keep it sparse)
    for (const auto& [src_off_dw, uses] : *use_list) {
        if (src_off_dw.IsImmediate()) {
            c.mov(r10d, ptr[rdi + (src_off_dw.U32() << 2)]);
        } else {
            if (!ComputeOffset(c, r10d, pass_info, src_off_dw)) {
                LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                continue;
            }
            c.shl(r10d, 2);
            c.mov(r10d, r10d);
            c.mov(r10d, dword[rdi + r10]);
        }
        c.mov(ptr[rsi + (pass_info.dst_off_dw << 2)], r10d);

        for (auto* use : uses) SetFlatbufOffset(use, pass_info.dst_off_dw);
        pass_info.dst_off_dw++;
    }

    // Then visit any children used as pointers
    for (const auto& [src_off_dw, uses] : *use_list) {
        for (auto* use : uses) {
            if (pass_info.GetUsesAsPointer(use)) {
                VisitPointer(src_off_dw, use, pass_info, c);
            }
        }
    }

    PopPtr(c);
}

static void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    Xbyak::CodeGenerator& c = g_srt_codegen;

    if (pass_info.srt_roots.empty()) {
        return;
    }

    // Register the signal handler for SRT walker, if not already registered
    if (g_srt_codegen_start == nullptr) {
        g_srt_codegen_start = c.getCurr();
        auto* signals = Core::Signals::Instance();
        // Call after the memory invalidation handler
        constexpr u32 priority = 1;
        signals->RegisterAccessViolationHandler(SrtWalkerSignalHandler, priority);
    }

    info.srt_info.walker_func = c.getCurr<PFN_SrtWalker>();
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);

    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, c);
    }

    c.ret();
    c.ready();

    info.srt_info.walker_func_size =
        c.getCurr() - reinterpret_cast<const u8*>(info.srt_info.walker_func);

    if (EmulatorSettings.IsDumpShaders()) {
        DumpSrtProgram(info, reinterpret_cast<const u8*>(info.srt_info.walker_func),
                       info.srt_info.walker_func_size);
    }

    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

static bool IsReadConstSource(const IR::Value base) {
    auto* inst = base.TryInst();
    if (!inst) {
        return false;
    }
    return inst->GetOpcode() == IR::Opcode::GetUserData ||
           inst->GetOpcode() == IR::Opcode::ReadConst ||
           inst->GetOpcode() == IR::Opcode::ReadConstBuffer;
}

void SimplifyReadConstAddressAdd(IR::Inst& inst) {
    // This handles the following pattern by combining the addition with the offset
    // %82 = IAdd32 %65, #28816
    // %83 = ULessThan32 %82, %65
    // %84 = SelectU32 %83, #1, #0
    // %85 = IAdd32 %66, %84
    // %86 = CompositeConstructU32x2 %82, %85
    // %87 = ReadConst (flags=0x0)  %86, #0
    if (inst.GetOpcode() != IR::Opcode::ReadConst) {
        return;
    }
    IR::Inst* addr = inst.Arg(0).Inst();
    IR::Inst* hi = addr->Arg(1).TryInst();
    IR::Inst* lo = addr->Arg(0).TryInst();
    if (!hi || !lo) {
        return;
    }

    if (lo->GetOpcode() != IR::Opcode::IAdd32 || hi->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }

    IR::Value add_offset;
    IR::Value base;
    if (base = lo->Arg(0); IsReadConstSource(base)) {
        add_offset = lo->Arg(1);
    } else if (base = lo->Arg(1); IsReadConstSource(base)) {
        add_offset = lo->Arg(0);
    } else {
        return;
    }

    IR::Inst* sel = hi->Arg(1).TryInst();
    if (!sel || sel->GetOpcode() != IR::Opcode::SelectU32) {
        return;
    }

    IR::Value sel_true = sel->Arg(1);
    IR::Value sel_false = sel->Arg(2);
    if (!sel_true.IsImmediate() || !sel_false.IsImmediate() || sel_true.U32() != 1 ||
        sel_false.U32() != 0) {
        return;
    }

    addr->SetArg(0, base);
    addr->SetArg(1, hi->Arg(0));
    for (auto [user, operand] : addr->Uses()) {
        ASSERT(user->GetOpcode() == IR::Opcode::ReadConst && operand == 0);
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        IR::U32 offset = IR::U32{user->Arg(1)};
        IR::U32 dw_add_offset =
            add_offset.IsImmediate()
                ? ir.Imm32(add_offset.U32() >> 2u)
                : IR::U32{ir.ShiftRightLogical(IR::U32{add_offset}, ir.Imm32(2u))};
        if (offset.IsImmediate() && dw_add_offset.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + dw_add_offset.U32()));
        } else {
            user->SetArg(1, ir.IAdd(offset, dw_add_offset));
        }
    }
}

} // Anonymous namespace

void FlattenExtendedUserdataPass(IR::Program& program,
                                 const ResourceDiscoveryList& sharp_usages, bool late_readconst) {
    auto& post_order = program.post_order_blocks;
    PassInfo pass_info;

    // traverse at end and assign offsets to duplicate readconsts, using
    // vn_to_inst as the source
    boost::container::small_vector<IR::Inst*, 32> all_readconsts, visited;
    std::queue<IR::Inst*> queue;

    for (auto it = post_order.rbegin(); it != post_order.rend(); it++) {
        IR::Block* block{*it};
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst &&
                inst.GetOpcode() != IR::Opcode::ReadConstBuffer) {
                continue;
            }

            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
                // Only flatten ReadConstBuffer if it was marked as a sharp source in the
                // resource discovery pass
                auto inst_info = inst.Flags<IR::BufferInstInfo>();
                if (!inst_info.sharp_source) {
                    continue;
                }
            }

            SimplifyReadConstAddressAdd(inst);

            all_readconsts.push_back(&inst);

            auto offset = inst.Arg(1);
            if (offset.IsImmediate()) {
                continue;
            }

            queue.push(offset.Inst());
            while (!queue.empty()) {
                IR::Inst* inst{queue.front()};
                queue.pop();

                if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                    auto buffer_inst_info = inst->Flags<IR::BufferInstInfo>();
                    if (!buffer_inst_info.sharp_source.Value()) {
                        all_readconsts.push_back(inst);
                        auto offset = inst->Arg(1);
                        if (!offset.IsImmediate()) {
                            auto arg_inst = offset.Inst();
                            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                                visited.push_back(arg_inst);
                                queue.push(arg_inst);
                            }
                        }
                    }
                    continue;
                }
                if (inst->GetOpcode() == IR::Opcode::GetUserData ||
                    inst->GetOpcode() == IR::Opcode::ReadConst ||
                    !IsAllowedOffsetInstruction(inst)) {
                    continue;
                }
                for (size_t arg = inst->NumArgs(); arg--;) {
                    const IR::Value arg_value = inst->Arg(arg);
                    if (arg_value.IsImmediate()) {
                        continue;
                    }
                    IR::Inst* arg_inst = arg_value.Inst();
                    if (std::ranges::find(visited, arg_inst) == visited.end()) {
                        visited.push_back(arg_inst);
                        queue.push(arg_inst);
                    }
                }
            }
        }
    }

    pass_info.readconst_sources.Initialize(program, pass_info.gvn_table);

    for (auto inst : all_readconsts) {
        if (pass_info.DeduplicateInstruction(inst) != inst) {
            // This is a duplicate of a readconst we've already visited
            continue;
        }

        const auto address = pass_info.readconst_sources.GetAddress(inst);
        if (auto* inst = address[0].TryInst();
            inst && inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            continue;
        }
        ASSERT_MSG(IsReadConstSource(address[0]), "ReadConst base low not from constant memory");
        ASSERT_MSG(IsReadConstSource(address[1]), "ReadConst base high not from constant memory");

        IR::Inst* ptr_lo = address[0].Inst();
        ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);

        auto ptr_uses_kv = pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
        PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;

        auto& users = user_list[inst->Arg(1)];
        if (std::ranges::find(users, inst) == users.end()) users.push_back(inst);

        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
            pass_info.srt_roots[ud_reg] = ptr_lo;
        }
    }

    GenerateSrtProgram(program.info, pass_info);

    // Assign offsets to duplicate readconsts
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        SetFlatbufOffset(readconst, GetFlatbufOffset(original));
    }

    program.info.RefreshFlatBuf();
}

} // namespace Shader::Optimization

#elif defined(ARCH_ARM64) && (defined(__linux__) || defined(__APPLE__))

namespace {

constexpr u32 Arm64MovX7X2 = 0xaa0203e7;
constexpr u32 Arm64MovX2X0 = 0xaa0003e2;
constexpr u32 Arm64PushX2 = 0xf81f0fe2;
constexpr u32 Arm64PopX2 = 0xf84107e2;
constexpr u32 Arm64LoadPointer = 0xf8646842;
constexpr u32 Arm64MaskPointer = 0xd340bc42;
constexpr u32 Arm64LoadDataRegisterOffset = 0xb8646843;
constexpr u32 Arm64StoreDataRegisterOffset = 0xb8256823;
constexpr u32 Arm64Ret = 0xd65f03c0;
constexpr size_t MaxSrtCodeRanges = 32768;

struct SrtCodeRange {
    uintptr_t begin{};
    uintptr_t end{};
};

struct SrtCodeMapping {
    u8* data{};
    size_t size{};

    ~SrtCodeMapping() {
        if (data != nullptr) {
            munmap(data, size);
        }
    }
};

std::array<SrtCodeRange, MaxSrtCodeRanges> g_srt_code_ranges{};
std::atomic_size_t g_srt_code_range_count{};
std::mutex g_srt_code_mutex;
std::vector<std::unique_ptr<SrtCodeMapping>> g_srt_code_mappings;
std::once_flag g_srt_signal_once;

bool IsSrtCodeAddress(uintptr_t pc) {
    const size_t count = g_srt_code_range_count.load(std::memory_order_acquire);
    for (size_t index = 0; index < count; ++index) {
        const auto& range = g_srt_code_ranges[index];
        if (pc >= range.begin && pc < range.end) {
            return true;
        }
    }
    return false;
}

bool SrtWalkerSignalHandler(void* context, void* fault_address) {
    const auto pc = reinterpret_cast<uintptr_t>(Common::GetRip(context));
    if (!IsSrtCodeAddress(pc)) {
        return false;
    }

    u32 instruction{};
    std::memcpy(&instruction, reinterpret_cast<const void*>(pc), sizeof(instruction));
    auto* signal_context = static_cast<ucontext_t*>(context);
#ifdef __APPLE__
    auto* registers = signal_context->uc_mcontext->__ss.__x;
#else
    auto* registers = signal_context->uc_mcontext.regs;
#endif
    if (instruction == Arm64LoadPointer) {
        registers[2] = 0;
    } else if ((instruction & 0xffc003ffu) == 0xb9400043u ||
               instruction == Arm64LoadDataRegisterOffset) {
        registers[3] = 0;
    } else if (instruction == 0xb94000a4u) { // ldr w4, [x5]
        registers[4] = 0;
    } else {
        return false;
    }
    Common::IncrementRip(context, sizeof(u32));
    return true;
}

bool IsArm64SrtWalker(const u8* ptr, size_t size) {
    if (ptr == nullptr || size < sizeof(u32) * 3 || size % sizeof(u32) != 0) {
        return false;
    }
    u32 first{};
    u32 second{};
    u32 last{};
    std::memcpy(&first, ptr, sizeof(first));
    std::memcpy(&second, ptr + sizeof(u32), sizeof(second));
    std::memcpy(&last, ptr + size - sizeof(last), sizeof(last));
    return first == Arm64MovX7X2 && second == Arm64MovX2X0 && last == Arm64Ret;
}

class Arm64SrtEmitter {
public:
    void Begin() {
        Emit(Arm64MovX7X2);
        Emit(Arm64MovX2X0);
    }

    void PushPointer(u32 offset_dw) {
        Emit(Arm64PushX2);
        MoveImmediate(4, static_cast<u64>(offset_dw) * sizeof(u32));
        Emit(Arm64LoadPointer);
        Emit(Arm64MaskPointer);
    }

    void PopPointer() {
        Emit(Arm64PopX2);
    }

    void CopyDword(u32 source_offset_dw, u32 destination_offset_dw) {
        if (source_offset_dw <= 0xfff) {
            Emit(0xb9400043u | (source_offset_dw << 10));
        } else {
            MoveImmediate(4, static_cast<u64>(source_offset_dw) * sizeof(u32));
            Emit(Arm64LoadDataRegisterOffset);
        }

        if (destination_offset_dw <= 0xfff) {
            Emit(0xb9000023u | (destination_offset_dw << 10));
        } else {
            MoveImmediate(5, static_cast<u64>(destination_offset_dw) * sizeof(u32));
            Emit(Arm64StoreDataRegisterOffset);
        }
        MoveImmediate(4, static_cast<u64>(source_offset_dw) * sizeof(u32));
        Emit(0x8b040046u); // add x6, x2, x4
        StoreSourceAddress(destination_offset_dw);
    }

    void MoveOffsetImmediate(u32 value) { MoveImmediate(4, value); }

    void ReadIndirectDword(bool buffer) {
        Emit(0xf84107e5u); // ldr x5, [sp], #16 (high)
        Emit(0xf84107e6u); // ldr x6, [sp], #16 (low)
        Emit(buffer ? 0x92402ca5u : 0x92403ca5u); // and x5, x5, #0xfff / #0xffff
        Emit(0xd3607ca5u); // lsl x5, x5, #32
        Emit(0xaa0600a5u); // orr x5, x5, x6
        Emit(0x8b2448a5u); // add x5, x5, w4, uxtw #2
        Emit(0xb94000a4u); // ldr w4, [x5]
    }

    void LoadFlatDword(u32 offset_dw) {
        if (offset_dw <= 0xfff) {
            Emit(0xb9400024u | (offset_dw << 10)); // ldr w4, [x1, #offset]
        } else {
            MoveImmediate(5, static_cast<u64>(offset_dw) * sizeof(u32));
            Emit(0xb8656824u); // ldr w4, [x1, x5]
        }
    }

    void PushOffset() { Emit(0xf81f0fe4u); } // str x4, [sp, #-16]!
    void PopOffset() { Emit(0xf84107e6u); } // ldr x6, [sp], #16
    void OffsetInstruction(u32 instruction) { Emit(instruction); }
    size_t Checkpoint() const { return code.size(); }
    void Rewind(size_t checkpoint) { code.resize(checkpoint); }

    void PushComputedPointer() {
        Emit(Arm64PushX2);
        Emit(0x531e7484u); // lsl w4, w4, #2
        Emit(Arm64LoadPointer);
        Emit(Arm64MaskPointer);
    }

    void CopyComputedDword(u32 destination_offset_dw) {
        Emit(0x531e7484u); // lsl w4, w4, #2
        Emit(Arm64LoadDataRegisterOffset);
        if (destination_offset_dw <= 0xfff) {
            Emit(0xb9000023u | (destination_offset_dw << 10));
        } else {
            MoveImmediate(5, static_cast<u64>(destination_offset_dw) * sizeof(u32));
            Emit(Arm64StoreDataRegisterOffset);
        }
        Emit(0x8b040046u); // add x6, x2, x4 (wrapped byte offset)
        StoreSourceAddress(destination_offset_dw);
    }

    void StoreSourceAddress(u32 destination_offset_dw) {
        if (destination_offset_dw <= 0xfff) {
            Emit(0xf90000e6u | (destination_offset_dw << 10)); // str x6,[x7,#index*8]
        } else {
            MoveImmediate(5, static_cast<u64>(destination_offset_dw) * sizeof(u64));
            Emit(0xf82568e6u); // str x6,[x7,x5]
        }
    }

    void End() {
        Emit(Arm64Ret);
    }

    const u8* Data() const {
        return reinterpret_cast<const u8*>(code.data());
    }

    size_t Size() const {
        return code.size() * sizeof(u32);
    }

private:
    void Emit(u32 instruction) {
        code.push_back(instruction);
    }

    void MoveImmediate(u32 reg, u64 value) {
        Emit(0xd2800000u | reg | (static_cast<u32>(value & 0xffff) << 5));
        for (u32 halfword = 1; halfword < 4; ++halfword) {
            const u32 part = static_cast<u32>((value >> (halfword * 16)) & 0xffff);
            if (part != 0) {
                Emit(0xf2800000u | reg | (halfword << 21) | (part << 5));
            }
        }
    }

    std::vector<u32> code;
};

using namespace Shader;

struct PassInfo {
    struct PointerListCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            // Keep order with the following criteria:
            // 1. If immediate, sort by immediate value
            // 2. If not immediate, sort by instruction order
            // 3. Immediates come first, then instructions
            if (a.IsImmediate() && b.IsImmediate()) {
                return a.U32() < b.U32();
            } else if (!a.IsImmediate() && !b.IsImmediate()) {
                auto a_inst = a.Inst();
                auto b_inst = b.Inst();
                if (a_inst == b_inst) return false;
                auto a_block = a_inst->GetParent();
                auto b_block = b_inst->GetParent();
                if (a_block != b_block) {
                    return a_block < b_block; // Sort by block address
                }

                auto it = a_block->Instructions().iterator_to(*a_inst);
                auto end = a_block->Instructions().end();
                for (; it != end; ++it) {
                    if (&*it == b_inst) {
                        return true;
                    }
                }
                return false;
            } else {
                return a.IsImmediate(); // Immediates come first
            }
        }
    };

    // map offset to inst
    using PtrUserList =
        boost::container::flat_map<IR::Value, boost::container::small_vector<IR::Inst*, 1>,
                                   PointerListCompare>;

    Optimization::SrtGvnTable gvn_table;
    ReadConstAddressSources readconst_sources;
    // keys are GetUserData, ReadConst or ReadConstBuffer instructions that are used as pointers
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    // GetUserData instructions corresponding to sgpr_base of SRT roots
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;

    // pick a single inst for a given value number
    std::unordered_map<u32, IR::Inst*> vn_to_inst;

    // Bumped during codegen to assign offsets to readconsts
    u16 dst_off_dw;
    std::vector<IR::Inst*> active_phis;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        if (it != pointer_uses.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Return a single instruction that this instruction is identical to, according
    // to value number
    // The "original" is arbitrary. Here it's the first instruction found for a given value number
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};
} // namespace

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    if (!IsArm64SrtWalker(ptr, size)) {
        LOG_WARNING(Render_Recompiler,
                    "Ignoring incompatible cached SRT walker; shader will be recompiled");
        return nullptr;
    }

    std::lock_guard lock{g_srt_code_mutex};
    const size_t range_index = g_srt_code_range_count.load(std::memory_order_relaxed);
    if (range_index >= g_srt_code_ranges.size()) {
        LOG_CRITICAL(Render_Recompiler, "ARM64 SRT walker range table is full");
        std::abort();
    }

    const long page_size_result = sysconf(_SC_PAGESIZE);
    if (page_size_result <= 0) {
        LOG_CRITICAL(Render_Recompiler, "Unable to query host page size for ARM64 SRT walker");
        std::abort();
    }
    const size_t page_size = static_cast<size_t>(page_size_result);
    const size_t mapping_size = (size + page_size - 1) & ~(page_size - 1);
    auto mapping = std::make_unique<SrtCodeMapping>();
    int protection = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
    protection |= PROT_EXEC;
    flags |= MAP_JIT;
#endif
    mapping->data = static_cast<u8*>(mmap(nullptr, mapping_size, protection, flags, -1, 0));
    mapping->size = mapping_size;
    if (mapping->data == MAP_FAILED) {
        mapping->data = nullptr;
        LOG_CRITICAL(Render_Recompiler, "Unable to allocate ARM64 SRT walker: errno {}", errno);
        std::abort();
    }
#ifdef __APPLE__
    // Darwin MAP_JIT selects write or execute permission per thread. Keep this
    // non-reentrant publication window limited to copying already-emitted code;
    // no guest execution or compiler callbacks can enter it. Publish the walker
    // only after restoring execution permission and invalidating the I-cache.
    pthread_jit_write_protect_np(0);
    std::memcpy(mapping->data, ptr, size);
    sys_icache_invalidate(mapping->data, size);
    pthread_jit_write_protect_np(1);
#else
    std::memcpy(mapping->data, ptr, size);
    __builtin___clear_cache(reinterpret_cast<char*>(mapping->data),
                            reinterpret_cast<char*>(mapping->data + size));
    if (mprotect(mapping->data, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        LOG_CRITICAL(Render_Recompiler, "Unable to protect ARM64 SRT walker: errno {}", errno);
        std::abort();
    }
#endif

    std::call_once(g_srt_signal_once, [] {
        constexpr u32 priority = 1;
        Core::Signals::Instance()->RegisterAccessViolationHandler(SrtWalkerSignalHandler, priority);
    });

    const auto begin = reinterpret_cast<uintptr_t>(mapping->data);
    auto* function = reinterpret_cast<PFN_SrtWalker>(mapping->data);
    g_srt_code_mappings.push_back(std::move(mapping));
    g_srt_code_ranges[range_index] = {begin, begin + size};
    g_srt_code_range_count.store(range_index + 1, std::memory_order_release);
    return function;
}

} // namespace Shader

namespace Shader::Optimization {

namespace {

static inline u16 GetFlatbufOffset(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        return inst_info.flatbuf_off_dw;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        return inst->Flags<u16>();
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static inline void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        inst_info.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(inst_info);
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        inst->SetFlags(offset);
        return;
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static bool IsAllowedOffsetInstruction(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitwiseNot32:
    case IR::Opcode::UMin32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitFieldUExtract:
        return true;
    default:
        return false;
    }
}

// The offset expression is the same IR accepted by the platform-independent
// flattening pass. W4 holds its result; X6 and aligned stack slots are scratch.
static bool ComputeOffset(Arm64SrtEmitter& emitter, PassInfo& pass_info, const IR::Value& value) {
    if (value.IsImmediate()) {
        emitter.MoveOffsetImmediate(value.Type() == IR::Type::F32
                                        ? std::bit_cast<u32>(value.F32()) : value.U32());
        return true;
    }
    auto* inst = value.InstRecursive();
    const auto opcode = inst->GetOpcode();
    const size_t checkpoint = emitter.Checkpoint();
    const auto fail = [&] { emitter.Rewind(checkpoint); return false; };
    if (opcode == IR::Opcode::Phi) {
        if (std::ranges::find(pass_info.active_phis, inst) != pass_info.active_phis.end()) {
            return false;
        }
        pass_info.active_phis.push_back(inst);
        bool success = false;
        for (size_t i = 0; i < inst->NumArgs(); ++i) {
            auto argument = inst->Arg(i);
            if (argument.TryInstRecursive() != inst && ComputeOffset(emitter, pass_info, argument)) {
                success = true;
                break;
            }
        }
        pass_info.active_phis.pop_back();
        return success || fail();
    }
    if (opcode == IR::Opcode::ReadFirstLane || opcode == IR::Opcode::ReadLane) {
        if (!ComputeOffset(emitter, pass_info, inst->Arg(0))) emitter.MoveOffsetImmediate(0);
        return true;
    }
    if (opcode == IR::Opcode::BitCastU32F32 || opcode == IR::Opcode::BitCastF32U32) {
        return ComputeOffset(emitter, pass_info, inst->Arg(0)) || fail();
    }
    if (opcode == IR::Opcode::GetUserData) {
        emitter.LoadFlatDword(static_cast<u32>(inst->Arg(0).ScalarReg()));
        return true;
    }
    if (opcode == IR::Opcode::ReadConst || opcode == IR::Opcode::ReadConstBuffer) {
        const u16 offset = GetFlatbufOffset(pass_info.DeduplicateInstruction(inst));
        if (offset) {
            emitter.LoadFlatDword(offset);
            return true;
        }
        auto* base = inst->Arg(0).TryInstRecursive();
        if (!base) return fail();
        const bool buffer = opcode == IR::Opcode::ReadConstBuffer;
        if (buffer) {
            if (base->GetOpcode() == IR::Opcode::CompositeConstructU32x2x2) {
                base = base->Arg(0).TryInstRecursive();
                if (!base || base->GetOpcode() != IR::Opcode::CompositeConstructU32x2) return fail();
            } else if (base->GetOpcode() != IR::Opcode::CompositeConstructU32x4) {
                return fail();
            }
        } else if (base->GetOpcode() != IR::Opcode::CompositeConstructU32x2) {
            return fail();
        }
        const auto address = pass_info.readconst_sources.GetAddress(inst);
        if (!ComputeOffset(emitter, pass_info, address[0])) return fail();
        emitter.PushOffset();
        if (!ComputeOffset(emitter, pass_info, address[1])) return fail();
        emitter.PushOffset();
        if (!ComputeOffset(emitter, pass_info, inst->Arg(1))) return fail();
        emitter.ReadIndirectDword(buffer);
        return true;
    }
    if (!IsAllowedOffsetInstruction(inst)) return false;
    if (!ComputeOffset(emitter, pass_info, inst->Arg(0))) return fail();
    if (opcode == IR::Opcode::BitwiseNot32) {
        emitter.OffsetInstruction(0x2a2403e4u); // mvn w4, w4
        return true;
    }
    emitter.PushOffset();
    if (!ComputeOffset(emitter, pass_info, inst->Arg(1))) return fail();
    emitter.PopOffset();
    switch (opcode) {
    case IR::Opcode::IAdd32: emitter.OffsetInstruction(0x0b0400c4u); break;
    case IR::Opcode::ISub32: emitter.OffsetInstruction(0x4b0400c4u); break;
    case IR::Opcode::IMul32: emitter.OffsetInstruction(0x1b047cc4u); break;
    case IR::Opcode::ShiftLeftLogical32: emitter.OffsetInstruction(0x1ac420c4u); break;
    case IR::Opcode::ShiftRightLogical32: emitter.OffsetInstruction(0x1ac424c4u); break;
    case IR::Opcode::BitwiseAnd32: emitter.OffsetInstruction(0x0a0400c4u); break;
    case IR::Opcode::BitwiseOr32: emitter.OffsetInstruction(0x2a0400c4u); break;
    case IR::Opcode::BitwiseXor32: emitter.OffsetInstruction(0x4a0400c4u); break;
    case IR::Opcode::UMin32:
        emitter.OffsetInstruction(0x6b0400dfu); // cmp w6, w4
        emitter.OffsetInstruction(0x1a8430c4u); // csel w4, w6, w4, lo
        break;
    case IR::Opcode::UMax32:
        emitter.OffsetInstruction(0x6b0400dfu);
        emitter.OffsetInstruction(0x1a8480c4u); // csel w4, w6, w4, hi
        break;
    case IR::Opcode::BitFieldUExtract:
        emitter.OffsetInstruction(0x1ac424c4u); // lsr w4, w6, w4
        emitter.PushOffset();
        if (!ComputeOffset(emitter, pass_info, inst->Arg(2))) return fail();
        emitter.OffsetInstruction(0x52800025u); // mov w5, #1
        emitter.OffsetInstruction(0x1ac420a5u); // lsl w5, w5, w4
        emitter.OffsetInstruction(0x510004a5u); // sub w5, w5, #1
        emitter.OffsetInstruction(0x7100809fu); // cmp w4, #32
        emitter.OffsetInstruction(0x12800006u); // mov w6, #-1
        emitter.OffsetInstruction(0x1a8520c4u); // csel w4, w6, w5, hs
        emitter.PopOffset();
        emitter.OffsetInstruction(0x0a0400c4u); // and w4, w6, w4
        break;
    default: return fail();
    }
    return true;
}

void VisitPointer(const IR::Value& offset_dw, IR::Inst* subtree, PassInfo& pass_info,
                  Arm64SrtEmitter& emitter) {
    if ((subtree->GetOpcode() == IR::Opcode::ReadConst && GetFlatbufOffset(subtree) == 0) ||
        (subtree->GetOpcode() == IR::Opcode::ReadConstBuffer && GetFlatbufOffset(subtree) == 0)) {
        return;
    }
    if (offset_dw.IsImmediate()) {
        emitter.PushPointer(offset_dw.U32());
    } else {
        if (!ComputeOffset(emitter, pass_info, offset_dw)) {
            LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
            return;
        }
        emitter.PushComputedPointer();
    }
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);
    for (const auto& [source_offset_dw, uses] : *use_list) {
        if (source_offset_dw.IsImmediate()) {
            emitter.CopyDword(source_offset_dw.U32(), pass_info.dst_off_dw);
        } else {
            if (!ComputeOffset(emitter, pass_info, source_offset_dw)) {
                LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
                continue;
            }
            emitter.CopyComputedDword(pass_info.dst_off_dw);
        }
        for (auto* use : uses) SetFlatbufOffset(use, pass_info.dst_off_dw);
        ++pass_info.dst_off_dw;
    }
    for (const auto& [source_offset_dw, uses] : *use_list) {
        for (auto* use : uses) {
            if (pass_info.GetUsesAsPointer(use)) {
                VisitPointer(source_offset_dw, use, pass_info, emitter);
            }
        }
    }
    emitter.PopPointer();
}

void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    if (pass_info.srt_roots.empty()) {
        return;
    }

    Arm64SrtEmitter emitter;
    emitter.Begin();
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);
    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, emitter);
    }
    emitter.End();

    info.srt_info.walker_func = RegisterWalkerCode(emitter.Data(), emitter.Size());
    ASSERT(info.srt_info.walker_func != nullptr);
    info.srt_info.walker_func_size = emitter.Size();
    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

static bool IsReadConstSource(const IR::Value base) {
    auto* inst = base.TryInst();
    if (!inst) {
        return false;
    }
    return inst->GetOpcode() == IR::Opcode::GetUserData ||
           inst->GetOpcode() == IR::Opcode::ReadConst ||
           inst->GetOpcode() == IR::Opcode::ReadConstBuffer;
}

void SimplifyReadConstAddressAdd(IR::Inst& inst) {
    // This handles the following pattern by combining the addition with the offset
    // %82 = IAdd32 %65, #28816
    // %83 = ULessThan32 %82, %65
    // %84 = SelectU32 %83, #1, #0
    // %85 = IAdd32 %66, %84
    // %86 = CompositeConstructU32x2 %82, %85
    // %87 = ReadConst (flags=0x0)  %86, #0
    if (inst.GetOpcode() != IR::Opcode::ReadConst) {
        return;
    }
    IR::Inst* addr = inst.Arg(0).Inst();
    IR::Inst* hi = addr->Arg(1).TryInst();
    IR::Inst* lo = addr->Arg(0).TryInst();
    if (!hi || !lo) {
        return;
    }

    if (lo->GetOpcode() != IR::Opcode::IAdd32 || hi->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }

    IR::Value add_offset;
    IR::Value base;
    if (base = lo->Arg(0); IsReadConstSource(base)) {
        add_offset = lo->Arg(1);
    } else if (base = lo->Arg(1); IsReadConstSource(base)) {
        add_offset = lo->Arg(0);
    } else {
        return;
    }

    IR::Inst* sel = hi->Arg(1).TryInst();
    if (!sel || sel->GetOpcode() != IR::Opcode::SelectU32) {
        return;
    }

    IR::Value sel_true = sel->Arg(1);
    IR::Value sel_false = sel->Arg(2);
    if (!sel_true.IsImmediate() || !sel_false.IsImmediate() || sel_true.U32() != 1 ||
        sel_false.U32() != 0) {
        return;
    }

    addr->SetArg(0, base);
    addr->SetArg(1, hi->Arg(0));
    for (auto [user, operand] : addr->Uses()) {
        ASSERT(user->GetOpcode() == IR::Opcode::ReadConst && operand == 0);
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        IR::U32 offset = IR::U32{user->Arg(1)};
        IR::U32 dw_add_offset =
            add_offset.IsImmediate()
                ? ir.Imm32(add_offset.U32() >> 2u)
                : IR::U32{ir.ShiftRightLogical(IR::U32{add_offset}, ir.Imm32(2u))};
        if (offset.IsImmediate() && dw_add_offset.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + dw_add_offset.U32()));
        } else {
            user->SetArg(1, ir.IAdd(offset, dw_add_offset));
        }
    }
}

// Collect the first constant-memory reads on each dependency path. The traversal
// limit bounds exploration, while already queued reads still contribute.
void CollectReadConstDependencies(IR::Value value,
                                  boost::container::small_vector<IR::Inst*, 32>& reads) {
    auto* first = value.TryInst();
    if (!first) return;
    boost::container::small_vector<IR::Inst*, 16> pending{first};
    boost::container::small_vector<IR::Inst*, 32> visited;
    while (!pending.empty()) {
        auto* inst = pending.back();
        pending.pop_back();
        if (std::ranges::find(visited, inst) != visited.end()) continue;
        visited.push_back(inst);
        if (inst->GetOpcode() == IR::Opcode::ReadConst ||
            inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
            reads.push_back(inst);
            continue;
        }
        if (inst->GetOpcode() == IR::Opcode::GetUserData || visited.size() > 64) continue;
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
            if (auto* dependency = inst->Arg(arg).TryInst()) pending.push_back(dependency);
        }
    }
}

void MarkLateReadConsts(Shader::Info& info, PassInfo& pass_info,
                       const boost::container::small_vector<IR::Inst*, 32>& all_readconsts,
                       const ResourceDiscoveryList& sharp_usages) {
    boost::container::small_vector<IR::Inst*, 32> dependencies;
    for (auto* inst : all_readconsts) {
        CollectReadConstDependencies(inst->Arg(0), dependencies);
        CollectReadConstDependencies(inst->Arg(1), dependencies);
    }
    for (const auto& [inst, address] : pass_info.readconst_sources.canonical) {
        CollectReadConstDependencies(address[0], dependencies);
        CollectReadConstDependencies(address[1], dependencies);
    }
    for (const auto& usage : sharp_usages) {
        for (const auto& sharp : usage.sharps) {
            for (u32 index = 0; index < sharp.num_dwords; ++index) {
                CollectReadConstDependencies(sharp.dwords[index], dependencies);
            }
        }
    }
    std::unordered_set<IR::Inst*> required_reads;
    for (auto* inst : dependencies) required_reads.insert(pass_info.DeduplicateInstruction(inst));
    // Distinct path-specialized reads can still share a flattened address. Keep
    // source tracking for the slot if any of its users supplies a descriptor.
    std::unordered_set<u16> required_indices;
    for (auto* inst : required_reads) {
        const u16 index = GetFlatbufOffset(inst);
        if (index != 0) required_indices.insert(index);
    }
    std::vector<u16> ignored_indices;
    for (auto* inst : all_readconsts) {
        if (inst->GetOpcode() != IR::Opcode::ReadConst ||
            required_reads.contains(pass_info.DeduplicateInstruction(inst))) continue;
        const u16 index = GetFlatbufOffset(inst);
        if (index == 0 || required_indices.contains(index)) continue;
        inst->SetFlags<u32>(0x80000000u | index);
        ignored_indices.push_back(index);
    }
    std::ranges::sort(ignored_indices);
    ignored_indices.erase(std::unique(ignored_indices.begin(), ignored_indices.end()),
                          ignored_indices.end());
    info.flattened_ud_ignored_source_indices = std::move(ignored_indices);
}

} // namespace

void FlattenExtendedUserdataPass(IR::Program& program,
                                 const ResourceDiscoveryList& sharp_usages, bool late_readconst) {
    auto& post_order = program.post_order_blocks;
    PassInfo pass_info;

    // traverse at end and assign offsets to duplicate readconsts, using
    // vn_to_inst as the source
    boost::container::small_vector<IR::Inst*, 32> all_readconsts, visited;
    std::queue<IR::Inst*> queue;

    for (auto it = post_order.rbegin(); it != post_order.rend(); it++) {
        IR::Block* block{*it};
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst &&
                inst.GetOpcode() != IR::Opcode::ReadConstBuffer) {
                continue;
            }

            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
                // Only flatten ReadConstBuffer if it was marked as a sharp source in the
                // resource discovery pass
                auto inst_info = inst.Flags<IR::BufferInstInfo>();
                if (!inst_info.sharp_source) {
                    continue;
                }
            }

            SimplifyReadConstAddressAdd(inst);

            all_readconsts.push_back(&inst);

            auto offset = inst.Arg(1);
            if (offset.IsImmediate()) {
                continue;
            }

            queue.push(offset.Inst());
            while (!queue.empty()) {
                IR::Inst* inst{queue.front()};
                queue.pop();

                if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                    auto buffer_inst_info = inst->Flags<IR::BufferInstInfo>();
                    if (!buffer_inst_info.sharp_source.Value()) {
                        all_readconsts.push_back(inst);
                        auto offset = inst->Arg(1);
                        if (!offset.IsImmediate()) {
                            auto arg_inst = offset.Inst();
                            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                                visited.push_back(arg_inst);
                                queue.push(arg_inst);
                            }
                        }
                    }
                    continue;
                }
                if (inst->GetOpcode() == IR::Opcode::GetUserData ||
                    inst->GetOpcode() == IR::Opcode::ReadConst ||
                    !IsAllowedOffsetInstruction(inst)) {
                    continue;
                }
                for (size_t arg = inst->NumArgs(); arg--;) {
                    const IR::Value arg_value = inst->Arg(arg);
                    if (arg_value.IsImmediate()) {
                        continue;
                    }
                    IR::Inst* arg_inst = arg_value.Inst();
                    if (std::ranges::find(visited, arg_inst) == visited.end()) {
                        visited.push_back(arg_inst);
                        queue.push(arg_inst);
                    }
                }
            }
        }
    }

    pass_info.readconst_sources.Initialize(program, pass_info.gvn_table);

    for (auto inst : all_readconsts) {
        if (pass_info.DeduplicateInstruction(inst) != inst) {
            // This is a duplicate of a readconst we've already visited
            continue;
        }

        const auto address = pass_info.readconst_sources.GetAddress(inst);
        if (auto* inst = address[0].TryInst();
            inst && inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            continue;
        }
        ASSERT_MSG(IsReadConstSource(address[0]), "ReadConst base low not from constant memory");
        ASSERT_MSG(IsReadConstSource(address[1]), "ReadConst base high not from constant memory");

        IR::Inst* ptr_lo = address[0].Inst();
        ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);

        auto ptr_uses_kv = pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
        PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;

        auto& users = user_list[inst->Arg(1)];
        if (std::ranges::find(users, inst) == users.end()) users.push_back(inst);

        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
            pass_info.srt_roots[ud_reg] = ptr_lo;
        }
    }

    GenerateSrtProgram(program.info, pass_info);

    // Assign offsets to duplicate readconsts
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        SetFlatbufOffset(readconst, GetFlatbufOffset(original));
    }

    if (late_readconst) {
        MarkLateReadConsts(program.info, pass_info, all_readconsts, sharp_usages);
    }
    program.info.RefreshFlatBuf();
}

} // namespace Shader::Optimization

#else

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    UNREACHABLE_MSG("RegisterWalkerCode unimplemented for target architecture.");
}

namespace Optimization {

void FlattenExtendedUserdataPass(IR::Program& program,
                                 const ResourceDiscoveryList& sharp_usages, bool late_readconst) {
    UNREACHABLE_MSG("FlattenExtendedUserdataPass unimplemented for target architecture.");
}

} // namespace Optimization

} // namespace Shader

#endif
