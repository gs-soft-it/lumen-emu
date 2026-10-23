// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>
#include <unistd.h>
#include "common/object_pool.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/srt_gvn_table.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
namespace IR = Shader::IR;
using IR::Opcode;
using IR::Value;

struct Fixture {
    Common::ObjectPool<IR::Inst> pool{128};
    IR::Block block{pool};
    Shader::Info info{};
    IR::Program program{info};
    std::array<u32, 16> user_data{};
    std::array<u32, 512> data{};
    Value base;

    Fixture() {
        program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
        info.user_data = user_data;
        for (u32 i = 0; i < data.size(); ++i) data[i] = 0x83460000u ^ (i * 0x10203u);
        SetPointer(data.data());
        base = Add(Opcode::CompositeConstructU32x2, {User(0), User(1)});
    }
    Value Add(Opcode op, std::initializer_list<Value> args) {
        block.AppendNewInst(op, args);
        return Value{&block.back()};
    }
    Value User(u32 reg) { return Add(Opcode::GetUserData, {Value{static_cast<IR::ScalarReg>(reg)}}); }
    void SetPointer(const void* ptr) {
        const auto address = reinterpret_cast<uintptr_t>(ptr);
        std::memcpy(user_data.data(), &address, sizeof(address));
    }
    Value Read(Value offset, bool buffer = false, bool sharp = false) {
        auto value = Add(buffer ? Opcode::ReadConstBuffer : Opcode::ReadConst, {base, offset});
        if (buffer) {
            IR::BufferInstInfo flags{};
            flags.sharp_source.Assign(sharp);
            value.Inst()->SetFlags(flags);
        }
        return value;
    }
    u32 Index(Value value) {
        const auto* inst = value.Inst();
        const u32 index = inst->GetOpcode() == Opcode::ReadConstBuffer
                              ? inst->Flags<IR::BufferInstInfo>().flatbuf_off_dw.Value()
                              : inst->Flags<u16>();
        CHECK(index >= 16 && index < info.flattened_ud_buf.size());
        return index;
    }
    u32 Result(Value value) { return info.flattened_ud_buf[Index(value)]; }
    u64 SourceAddress(Value value) { return info.flattened_ud_source_addresses[Index(value)]; }
    void Compile(const Shader::Optimization::ResourceDiscoveryList& resources = {},
                 bool late_readconst = false) {
        Shader::Optimization::FlattenExtendedUserdataPass(program, resources, late_readconst);
        CHECK(info.srt_info.walker_func != nullptr);
    }
};

u32 Evaluate(Opcode op, u32 a, u32 b, u32 c) {
    switch (op) {
    case Opcode::IAdd32: return a + b;
    case Opcode::ISub32: return a - b;
    case Opcode::IMul32: return a * b;
    case Opcode::ShiftLeftLogical32: return a << (b & 31);
    case Opcode::ShiftRightLogical32: return a >> (b & 31);
    case Opcode::BitwiseAnd32: return a & b;
    case Opcode::BitwiseOr32: return a | b;
    case Opcode::BitwiseXor32: return a ^ b;
    case Opcode::BitwiseNot32: return ~a;
    case Opcode::UMin32: return std::min(a, b);
    case Opcode::UMax32: return std::max(a, b);
    case Opcode::BitFieldUExtract: return (a >> (b & 31)) & (c >= 32 ? ~0u : ((1u << c) - 1));
    default: std::abort();
    }
}

size_t CheckOperators() {
    constexpr std::array ops{Opcode::IAdd32, Opcode::ISub32, Opcode::IMul32,
        Opcode::ShiftLeftLogical32, Opcode::ShiftRightLogical32, Opcode::BitwiseAnd32,
        Opcode::BitwiseOr32, Opcode::BitwiseXor32, Opcode::BitwiseNot32,
        Opcode::UMin32, Opcode::UMax32, Opcode::BitFieldUExtract};
    constexpr std::array inputs{
        std::array<u32, 3>{11, 3, 5}, std::array<u32, 3>{0xfffffff1, 7, 4},
        std::array<u32, 3>{0x80000025, 0x80000002, 9}, std::array<u32, 3>{0, 0, 0},
        std::array<u32, 3>{0x7fffffff, 35, 31}, std::array<u32, 3>{0x76543210, 3, 32},
        std::array<u32, 3>{0x87654321, 36, 40}, std::array<u32, 3>{0xfedcba98, 0, ~0u}};
    size_t checked = 0;
    for (auto op : ops) {
        // Exercise both recursive operands and each immediate position. The final
        // mask keeps every computed address inside the real source allocation.
        for (unsigned form = 0; form < 3; ++form) {
            Fixture f;
            auto a = form == 1 ? Value{13u} : f.User(2);
            auto b = form == 2 ? Value{3u} : f.User(3);
            auto c = f.User(4);
            Value expression = op == Opcode::BitwiseNot32 ? f.Add(op, {a})
                : op == Opcode::BitFieldUExtract ? f.Add(op, {a, b, c}) : f.Add(op, {a, b});
            auto offset = f.Add(Opcode::BitwiseAnd32, {expression, Value{255u}});
            auto read = f.Read(offset);
            f.user_data[2] = inputs[0][0];
            f.user_data[3] = inputs[0][1];
            f.user_data[4] = inputs[0][2];
            f.Compile();
            for (const auto& input : inputs) {
                f.user_data[2] = input[0];
                f.user_data[3] = input[1];
                f.user_data[4] = input[2];
                f.info.RefreshFlatBuf();
                const u32 expected = Evaluate(op, form == 1 ? 13u : input[0],
                                              form == 2 ? 3u : input[1], input[2]) & 255;
                if (f.Result(read) != f.data[expected]) {
                    std::fprintf(stderr, "operator=%u form=%u a=%x b=%x c=%x offset=%u\n",
                                 static_cast<unsigned>(op), form, input[0], input[1], input[2], expected);
                    std::abort();
                }
                CHECK(f.SourceAddress(read) == reinterpret_cast<uintptr_t>(&f.data[expected]));
                ++checked;
            }
        }
    }
    return checked;
}

void CheckReadLeavesAndRefresh() {
    for (bool buffer : {false, true}) {
        Fixture f;
        f.data[2] = 11;
        // An unmarked ReadConstBuffer must still be flattened when it supplies
        // the offset of a sharp. The marked case is independently covered below.
        auto offset = f.Read(Value{2u}, buffer, false);
        auto read = f.Read(offset);
        f.Compile();
        CHECK(f.Result(read) == f.data[11]);
        f.data[2] = 37;
        f.info.RefreshFlatBuf();
        CHECK(f.Result(read) == f.data[37]);
        CHECK(f.Result(offset) == 37);
    }
    Fixture f;
    f.user_data[2] = 9;
    auto sharp = f.Read(f.User(2), true, true);
    f.Compile();
    CHECK(f.Result(sharp) == f.data[9]);
    f.user_data[2] = 71;
    f.info.RefreshFlatBuf();
    CHECK(f.Result(sharp) == f.data[71]);
}

void CheckNestedPointerAndFaultRecovery() {
    Fixture f;
    std::array<u32, 64> child{};
    child[7] = 0x87654321;
    const auto address = reinterpret_cast<uintptr_t>(child.data());
    std::memcpy(f.data.data() + 8, &address, sizeof(address));
    f.user_data[2] = 8;
    f.user_data[3] = 7;
    auto pointer_offset = f.User(2);
    auto lo = f.Read(pointer_offset);
    auto hi = f.Read(f.Add(Opcode::IAdd32, {pointer_offset, Value{1u}}));
    auto child_base = f.Add(Opcode::CompositeConstructU32x2, {lo, hi});
    auto read = f.Add(Opcode::ReadConst, {child_base, f.User(3)});
    f.Compile();
    CHECK(f.Result(read) == child[7]);
    CHECK(f.SourceAddress(read) == reinterpret_cast<uintptr_t>(&child[7]));
    child[13] = 0x12345678;
    f.user_data[3] = 13;
    f.info.RefreshFlatBuf();
    CHECK(f.Result(read) == child[13]);
    CHECK(f.SourceAddress(read) == reinterpret_cast<uintptr_t>(&child[13]));

    // Production signal dispatch recognizes both pointer and dword loads. A
    // bad address returns zero and a later valid pointer still executes.
    f.SetPointer(nullptr);
    f.info.RefreshFlatBuf();
    CHECK(f.Result(read) == 0);
    f.SetPointer(f.data.data());
    f.info.RefreshFlatBuf();
    CHECK(f.Result(read) == child[13]);
}

void CheckCachedWalkerPublication() {
    Fixture f;
    f.user_data[2] = 29;
    const auto read = f.Read(f.User(2));
    f.Compile();
    const auto index = f.Index(read);
    const auto* code = reinterpret_cast<const u8*>(f.info.srt_info.walker_func);
    const std::vector<u8> cached(code, code + f.info.srt_info.walker_func_size);
    std::atomic<bool> finished{};
    std::thread publisher([&] {
        auto user_data = f.user_data;
        Shader::Info cached_info{};
        cached_info.user_data = user_data;
        cached_info.srt_info = f.info.srt_info;
        for (u32 i = 0; i < 16; ++i) {
            auto bytes = cached;
            cached_info.srt_info.walker_func =
                Shader::RegisterWalkerCode(bytes.data(), bytes.size());
            CHECK(cached_info.srt_info.walker_func != nullptr);
            // Registration must own the code, including on a shader-cache hit.
            std::fill(bytes.begin(), bytes.end(), 0);
            // Refresh initializes the flat buffer's first 16 SGPRs, which the
            // walker consumes for computed offsets as well as writing results.
            cached_info.RefreshFlatBuf();
            CHECK(cached_info.flattened_ud_buf[index] == f.data[29]);
            CHECK(cached_info.flattened_ud_source_addresses[index] ==
                  reinterpret_cast<uintptr_t>(&f.data[29]));
            // Fault recovery must also work on the native publishing thread.
            user_data[0] = user_data[1] = 0;
            cached_info.RefreshFlatBuf();
            CHECK(cached_info.flattened_ud_buf[index] == 0);
            user_data = f.user_data;
            cached_info.RefreshFlatBuf();
            CHECK(cached_info.flattened_ud_buf[index] == f.data[29]);
        }
        finished.store(true, std::memory_order_release);
    });
    // Publishing new code must not revoke execution of existing walkers on
    // other threads (Darwin JIT write permissions are thread-local).
    do {
        f.info.RefreshFlatBuf();
        CHECK(f.Result(read) == f.data[29]);
    } while (!finished.load(std::memory_order_acquire));
    publisher.join();
}

void CheckSharedSlots() {
    for (bool dynamic : {false, true}) {
        Fixture f;
        f.user_data[2] = 19;
        auto offset = dynamic ? f.User(2) : Value{19u};
        auto plain = f.Read(offset);
        auto buffer = f.Read(offset, true, true);
        f.Compile();
        CHECK(f.Result(plain) == f.data[19]);
        CHECK(f.Result(buffer) == f.data[19]);
        CHECK(plain.Inst()->Flags<u16>() ==
              buffer.Inst()->Flags<IR::BufferInstInfo>().flatbuf_off_dw.Value());
        CHECK(f.info.srt_info.flattened_bufsize_dw == 17);
    }
}

void CheckGuardedPointerPhi(bool nested, u32 source_kind = 0, bool contiguous = true,
                            bool shared_late = false) {
    for (bool buffer : {false, true}) {
        if (shared_late && buffer) continue;
        for (bool reverse : {false, true}) {
            Fixture f;
            IR::Block changed{f.pool}, merge{f.pool}, consumer{f.pool}, exit{f.pool};
            std::array<u32, 512> alternate{};
            std::array<u32, 32> nested_data{}, alternate_nested{};
            alternate[7] = 0x11223344;
            nested_data[7] = 0x55667788;
            alternate_nested[7] = 0x99887766;
            const auto other_pointer = reinterpret_cast<uintptr_t>(alternate.data());
            std::memcpy(&f.data[32], &other_pointer, sizeof(other_pointer));
            const auto original_pointer = reinterpret_cast<uintptr_t>(f.data.data());
            std::memcpy(&f.data[16], &original_pointer, sizeof(original_pointer));
            std::memcpy(&alternate[16], &other_pointer, sizeof(other_pointer));
            const auto nested_pointer = reinterpret_cast<uintptr_t>(nested_data.data());
            const auto alternate_nested_pointer = reinterpret_cast<uintptr_t>(alternate_nested.data());
            std::memcpy(&f.data[20], &nested_pointer, sizeof(nested_pointer));
            std::memcpy(&alternate[20], &alternate_nested_pointer, sizeof(alternate_nested_pointer));
            IR::IREmitter entry{f.block};
            const auto condition = entry.IEqual(entry.GetUserData(IR::ScalarReg::S4),
                                                entry.Imm32(0));
            const auto inverse = entry.ConditionRef(entry.LogicalNot(condition));
            std::array<Value, 2> original_sources;
            if (source_kind == 0) {
                original_sources = {f.base.Inst()->Arg(0),
                    contiguous ? f.base.Inst()->Arg(1)
                               : Value{entry.GetUserData(IR::ScalarReg::S12)}};
            } else {
                const auto source_base = source_kind == 1 ? f.base
                    : entry.CompositeConstruct(IR::U32{f.base.Inst()->Arg(0)},
                                               IR::U32{f.base.Inst()->Arg(1)},
                                               entry.Imm32(512), entry.Imm32(0));
                for (size_t i = 0; i < original_sources.size(); ++i) {
                    const auto offset = entry.Imm32(i == 0 ? 16 : contiguous ? 17 : 19);
                    if (source_kind == 1) {
                        original_sources[i] = entry.ReadConst(source_base, offset);
                    } else {
                        IR::BufferInstInfo flags{};
                        flags.sharp_source.Assign(1u);
                        original_sources[i] = entry.ReadConstBuffer(source_base, offset, flags);
                    }
                }
            }
            IR::IREmitter overwrite{changed};
            const std::array<Value, 2> incoming{
                overwrite.ReadConst(f.base, overwrite.Imm32(32)),
                overwrite.ReadConst(f.base, overwrite.Imm32(33))};
            std::array<Value, 2> phis;
            for (size_t i = 0; i < phis.size(); ++i) {
                merge.AppendNewInst(Opcode::Phi, {});
                auto& phi = merge.back();
                phi.SetFlags(IR::Type::U32);
                const auto original = original_sources[i];
                if (reverse) {
                    phi.AddPhiOperand(&f.block, original);
                    phi.AddPhiOperand(&changed, incoming[i]);
                } else {
                    phi.AddPhiOperand(&changed, incoming[i]);
                    phi.AddPhiOperand(&f.block, original);
                }
                phis[i] = Value{&phi};
            }
            IR::IREmitter branch{merge};
            const auto guard = branch.ConditionRef(condition);
            IR::IREmitter use{consumer};
            const auto raw_base = buffer
                ? use.CompositeConstruct(IR::U32{phis[0]}, IR::U32{phis[1]},
                                         use.Imm32(512), use.Imm32(0))
                : use.CompositeConstruct(IR::U32{phis[0]}, IR::U32{phis[1]});
            const auto emit_read = [&](Value base, u32 offset) -> Value {
                if (buffer) {
                    IR::BufferInstInfo flags{};
                    flags.sharp_source.Assign(1u);
                    return use.ReadConstBuffer(base, use.Imm32(offset), flags);
                }
                return use.ReadConst(base, use.Imm32(offset));
            };
            Value final_base = raw_base;
            if (nested) {
                const IR::U32 low{emit_read(raw_base, 20)};
                const IR::U32 high{emit_read(raw_base, 21)};
                final_base = buffer
                    ? use.CompositeConstruct(low, high, use.Imm32(32), use.Imm32(0))
                    : use.CompositeConstruct(low, high);
            }
            const Value read = emit_read(final_base, 7);
            f.block.AddBranch(&changed);
            f.block.AddBranch(&merge);
            changed.AddBranch(&merge);
            merge.AddBranch(&consumer);
            merge.AddBranch(&exit);
            consumer.AddBranch(&exit);
            f.program.blocks = {&f.block, &changed, &merge, &consumer, &exit};
            f.program.post_order_blocks.assign(f.program.blocks.rbegin(), f.program.blocks.rend());
            using Node = IR::AbstractSyntaxNode;
            f.program.syntax_list = {
                {{.block = &f.block}, Node::Type::Block},
                {{.if_node = {inverse, &changed, &merge}}, Node::Type::If},
                {{.block = &changed}, Node::Type::Block},
                {{.end_if = {&merge}}, Node::Type::EndIf},
                {{.block = &merge}, Node::Type::Block},
                {{.if_node = {guard, &consumer, &exit}}, Node::Type::If},
                {{.block = &consumer}, Node::Type::Block},
                {{.end_if = {&exit}}, Node::Type::EndIf},
                {{.block = &exit}, Node::Type::Block},
                {{}, Node::Type::Return}};
            // The canonical-address walker currently accepts consecutive direct
            // userdata words. Loaded pointer pairs require a separate dependency proof.
            if (!contiguous || source_kind != 0) {
                const auto canonical = Shader::Optimization::ResolveReadConstPhiSources(f.program);
                CHECK(!canonical.contains(read.Inst()));
                CHECK(read.Inst()->Arg(0) == final_base);
                continue;
            }
            if (!nested) {
                const auto offset = read.Inst()->Arg(1);
                for (const Value dynamic_offset : {f.User(4), phis[0]}) {
                    read.Inst()->SetArg(1, dynamic_offset);
                    const auto canonical = Shader::Optimization::ResolveReadConstPhiSources(f.program);
                    CHECK(!canonical.contains(read.Inst()));
                    CHECK(read.Inst()->Arg(0) == final_base);
                }
                read.Inst()->SetArg(1, offset);
            }
            Value duplicate, descriptor_read;
            AmdGpu::Buffer descriptor{};
            Shader::Optimization::ResourceDiscoveryList resources;
            const Shader::Profile profile{};
            if (shared_late) {
                // These reads have identical unresolved addresses, but separate
                // canonical value numbers. Only one supplies a buffer descriptor.
                duplicate = emit_read(final_base, 7);
                descriptor.base_address = 0x8102030405;
                descriptor.stride = 16;
                descriptor.num_records = 512;
                descriptor.data_format = 4;
                descriptor.num_format = 7;
                const auto words = std::bit_cast<std::array<u32, 4>>(descriptor);
                f.data[7] = words[0];
                const auto handle = use.CompositeConstruct(IR::U32{read}, use.Imm32(words[1]),
                    use.Imm32(words[2]), use.Imm32(words[3]));
                descriptor_read = use.ReadConstBuffer(handle, use.Imm32(0), {});
                resources = Shader::Optimization::ResourceDiscoverPass(f.program, profile);
            }
            f.Compile(resources, shared_late);
            if (shared_late)
                Shader::Optimization::ResourcePatchingPass(f.info, resources, profile);
            const u32* expected_address = nested ? &nested_data[7] : &f.data[7];
            const auto check_shared_source = [&] {
                if (!shared_late) return;
                CHECK(f.Index(read) == f.Index(duplicate));
                CHECK(f.SourceAddress(duplicate) == reinterpret_cast<uintptr_t>(expected_address));
                CHECK(std::ranges::find(f.info.flattened_ud_ignored_source_indices, f.Index(read)) ==
                      f.info.flattened_ud_ignored_source_indices.end());
                CHECK((duplicate.Inst()->Flags<u32>() & 0x80000000u) == 0);
                auto words = std::bit_cast<std::array<u32, 4>>(descriptor);
                words[0] = *expected_address;
                const auto expected_descriptor = std::bit_cast<AmdGpu::Buffer>(words);
                const auto binding = descriptor_read.Inst()->Arg(0).U32();
                CHECK(f.info.buffers.at(binding).GetSharp(f.info) == expected_descriptor);
            };
            CHECK(f.Result(read) == *expected_address);
            CHECK(f.SourceAddress(read) == reinterpret_cast<uintptr_t>(expected_address));
            check_shared_source();
            CHECK(read.Inst()->Arg(0) == final_base);
            CHECK(raw_base.Inst()->Arg(0) == phis[0] && raw_base.Inst()->Arg(1) == phis[1]);

            // Refresh both pointed-to storage and the direct userdata pointer.
            f.data[7] = 0x89abcdef;
            nested_data[7] = 0xfedcba98;
            f.info.RefreshFlatBuf();
            CHECK(f.Result(read) == *expected_address);
            check_shared_source();
            f.SetPointer(alternate.data());
            f.info.RefreshFlatBuf();
            expected_address = nested ? &alternate_nested[7] : &alternate[7];
            CHECK(f.Result(read) == *expected_address);
            CHECK(f.SourceAddress(read) == reinterpret_cast<uintptr_t>(expected_address));
            CHECK(read.Inst()->Arg(0) == final_base);
            check_shared_source();
        }
    }
}

void CheckSpecializedReadValueNumbers() {
    for (bool buffer : {false, true}) {
        Fixture f;
        auto phi = f.Add(Opcode::Phi, {});
        phi.Inst()->SetFlags(IR::Type::U32);
        phi.Inst()->AddPhiOperand(&f.block, f.User(2));
        phi.Inst()->AddPhiOperand(&f.block, f.User(3));
        const auto base = f.Add(Opcode::CompositeConstructU32x2, {phi, f.User(1)});
        const auto opcode = buffer ? Opcode::ReadConstBuffer : Opcode::ReadConst;
        const auto first = f.Add(opcode, {base, Value{7u}});
        const auto second = f.Add(opcode, {base, Value{7u}});
        const auto add_first = f.Add(Opcode::IAdd32, {first, Value{3u}});
        const auto add_second = f.Add(Opcode::IAdd32, {second, Value{3u}});
        const auto indirect_first = f.Read(add_first);
        const auto indirect_second = f.Read(add_second);

        // Ordinary identical reads and their dependent addresses share value numbers.
        Shader::Optimization::SrtGvnTable ordinary;
        CHECK(ordinary.GetValueNumber(first) == ordinary.GetValueNumber(second));
        CHECK(ordinary.GetValueNumber(indirect_first) == ordinary.GetValueNumber(indirect_second));
        for (bool reverse : {false, true}) {
            Shader::Optimization::SrtGvnTable specialized;
            specialized.AssignUniqueValueNumber((reverse ? second : first).Inst());
            specialized.AssignUniqueValueNumber((reverse ? first : second).Inst());
            // Hash dependencies first, as the flatten pass can encounter reads in any order.
            CHECK(specialized.GetValueNumber(indirect_first) !=
                  specialized.GetValueNumber(indirect_second));
            CHECK(specialized.GetValueNumber(add_first) != specialized.GetValueNumber(add_second));
            CHECK(specialized.GetValueNumber(first) != specialized.GetValueNumber(second));
            CHECK(first.Inst()->Arg(0) == second.Inst()->Arg(0));
        }
    }
}

void CheckPhiAndBitcastOffsets() {
    Fixture f;
    f.user_data[2] = 23;
    auto first = f.Add(Opcode::Phi, {});
    auto second = f.Add(Opcode::Phi, {});
    first.Inst()->SetFlags(IR::Type::U32);
    second.Inst()->SetFlags(IR::Type::U32);
    first.Inst()->AddPhiOperand(&f.block, first);
    first.Inst()->AddPhiOperand(&f.block, second);
    second.Inst()->AddPhiOperand(&f.block, first);
    // The first noncyclic alternative fails after emitting part of an add.
    // The next alternative must execute without the abandoned stack push.
    auto unsupported = f.Add(Opcode::UDiv32, {f.User(2), Value{2u}});
    auto partial = f.Add(Opcode::IAdd32, {Value{1u}, unsupported});
    second.Inst()->AddPhiOperand(&f.block, partial);
    auto identity = f.Add(Opcode::Identity, {f.User(2)});
    auto as_float = f.Add(Opcode::BitCastF32U32, {identity});
    auto as_integer = f.Add(Opcode::BitCastU32F32, {as_float});
    second.Inst()->AddPhiOperand(&f.block, as_integer);
    auto read = f.Read(first);
    f.Compile();
    CHECK(f.Result(read) == f.data[23]);
    f.user_data[2] = 45;
    f.info.RefreshFlatBuf();
    CHECK(f.Result(read) == f.data[45]);

    Fixture rejected;
    auto cycle = rejected.Add(Opcode::Phi, {});
    cycle.Inst()->SetFlags(IR::Type::U32);
    cycle.Inst()->AddPhiOperand(&rejected.block, cycle);
    auto invalid = rejected.Read(cycle);
    auto valid = rejected.Read(Value{3u});
    rejected.Compile();
    CHECK(invalid.Inst()->Flags<u16>() == 0);
    CHECK(rejected.Result(valid) == rejected.data[3]);
}

void CheckLiveReadFallback() {
    // A late root has no flat index while the earlier root is emitted.
    {
        Fixture f;
        std::array<u32, 32> indices{};
        indices[4] = 17;
        const auto address = reinterpret_cast<uintptr_t>(indices.data());
        std::memcpy(f.user_data.data() + 8, &address, sizeof(address));
        f.user_data[9] |= 0xabcd0000u; // ReadConst uses 48 address bits.
        auto source = f.Add(Opcode::CompositeConstructU32x2, {f.User(8), f.User(9)});
        auto index = f.Add(Opcode::ReadConst, {source, Value{4u}});
        auto read = f.Read(index);
        f.Compile();
        CHECK(f.Result(read) == f.data[17]);
        indices[4] = 29;
        f.info.RefreshFlatBuf();
        CHECK(f.Result(read) == f.data[29]);
    }
    for (bool pairs : {false, true}) {
        Fixture f;
        std::array<u32, 32> indices{};
        indices[4] = 21;
        const auto address = reinterpret_cast<uintptr_t>(indices.data());
        std::memcpy(f.user_data.data() + 8, &address, sizeof(address));
        f.user_data[9] |= 0xfffff000u; // Buffer descriptors use 44 address bits.
        Value handle;
        if (pairs) {
            auto address_pair = f.Add(Opcode::CompositeConstructU32x2, {f.User(8), f.User(9)});
            auto other_pair = f.Add(Opcode::CompositeConstructU32x2, {Value{0u}, Value{0u}});
            handle = f.Add(Opcode::CompositeConstructU32x2x2, {address_pair, other_pair});
        } else {
            handle = f.Add(Opcode::CompositeConstructU32x4,
                           {f.User(8), f.User(9), Value{0u}, Value{0u}});
        }
        auto index = f.Add(Opcode::ReadConstBuffer, {handle, Value{4u}});
        // The reference queue stops at Phi. This unmarked buffer read is not
        // preflattened, so ComputeOffset must perform the live descriptor read.
        auto phi = f.Add(Opcode::Phi, {});
        phi.Inst()->SetFlags(IR::Type::U32);
        phi.Inst()->AddPhiOperand(&f.block, index);
        auto read = f.Read(phi);
        f.Compile();
        CHECK(index.Inst()->Flags<IR::BufferInstInfo>().flatbuf_off_dw.Value() == 0);
        CHECK(f.Result(read) == f.data[21]);
        indices[4] = 31;
        f.info.RefreshFlatBuf();
        CHECK(f.Result(read) == f.data[31]);
        f.user_data[8] = 0;
        f.user_data[9] = 0;
        f.info.RefreshFlatBuf();
        CHECK(f.Result(read) == f.data[0]); // Native LDR W4 fault returns zero.
        std::memcpy(f.user_data.data() + 8, &address, sizeof(address));
        f.info.RefreshFlatBuf();
        CHECK(f.Result(read) == f.data[31]);
    }
}

void CheckLaneOffsetFallbacks() {
    for (bool indexed : {false, true}) {
        for (bool unsupported : {false, true}) {
            Fixture f;
            f.user_data[2] = 41;
            auto input = unsupported ? f.Add(Opcode::UDiv32, {f.User(2), Value{2u}}) : f.User(2);
            auto lane = indexed ? f.Add(Opcode::ReadLane, {input, Value{17u}})
                                : f.Add(Opcode::ReadFirstLane, {input});
            auto read = f.Read(lane);
            f.Compile();
            CHECK(f.Result(read) == f.data[unsupported ? 0 : 41]);
        }
    }
    Fixture f;
    // A two-dword handle is valid for ReadConst, but not for raw buffer fallback.
    auto buffer = f.Add(Opcode::ReadConstBuffer, {f.base, Value{3u}});
    auto phi = f.Add(Opcode::Phi, {});
    phi.Inst()->SetFlags(IR::Type::U32);
    phi.Inst()->AddPhiOperand(&f.block, buffer);
    phi.Inst()->AddPhiOperand(&f.block, Value{19u});
    auto read = f.Read(phi);
    f.Compile();
    CHECK(f.Result(read) == f.data[19]);
}

void CheckDynamicOffsetWrap() {
    Fixture f;
    f.user_data[2] = 0x40000007u;
    auto read = f.Read(f.User(2));
    f.Compile();
    // The source emitter shifts the 32-bit index before zero-extending it.
    CHECK(f.Result(read) == f.data[7]);
    CHECK(f.SourceAddress(read) == reinterpret_cast<uintptr_t>(&f.data[7]));
    f.user_data[2] = 0xc000000bu;
    f.info.RefreshFlatBuf();
    CHECK(f.Result(read) == f.data[11]);
    CHECK(f.SourceAddress(read) == reinterpret_cast<uintptr_t>(&f.data[11]));
}

void CheckAddressCarrySimplification() {
    Fixture f;
    f.user_data[2] = 16;
    auto low = f.User(0);
    auto high = f.User(1);
    auto added_low = f.Add(Opcode::IAdd32, {low, f.User(2)});
    auto carry = f.Add(Opcode::ULessThan32, {added_low, low});
    auto selected = f.Add(Opcode::SelectU32, {carry, Value{1u}, Value{0u}});
    auto added_high = f.Add(Opcode::IAdd32, {high, selected});
    auto added_base = f.Add(Opcode::CompositeConstructU32x2, {added_low, added_high});
    auto read = f.Add(Opcode::ReadConst, {added_base, Value{3u}});
    f.Compile();
    CHECK(f.Result(read) == f.data[7]);
    f.user_data[2] = 32;
    f.info.RefreshFlatBuf();
    CHECK(f.Result(read) == f.data[11]);
}
void CheckSourceAddresses() {
    Fixture f;
    auto immediate = f.Read(Value{4u});
    auto dynamic = f.Read(f.User(2));
    f.user_data[2] = 14;
    f.Compile();
    CHECK(f.SourceAddress(immediate) == reinterpret_cast<uintptr_t>(&f.data[4]));
    CHECK(f.SourceAddress(dynamic) == reinterpret_cast<uintptr_t>(&f.data[14]));
    std::fill(f.info.flattened_ud_source_addresses.begin(),
              f.info.flattened_ud_source_addresses.end(), ~u64{});
    f.user_data[2] = 28;
    f.info.RefreshFlatBuf();
    CHECK(f.SourceAddress(dynamic) == reinterpret_cast<uintptr_t>(&f.data[28]));
    CHECK(std::all_of(f.info.flattened_ud_source_addresses.begin(),
                      f.info.flattened_ud_source_addresses.begin() + 16,
                      [](u64 address) { return address == 0; }));
    f.info.srt_info.walker_func = nullptr;
    f.info.RefreshFlatBuf();
    CHECK(std::all_of(f.info.flattened_ud_source_addresses.begin(),
                      f.info.flattened_ud_source_addresses.end(),
                      [](u64 address) { return address == 0; }));

    // Cached two-argument walkers must not execute against the new ABI.
    const std::array<u32, 2> old_walker{0xaa0003e2u, 0xd65f03c0u};
    CHECK(Shader::RegisterWalkerCode(reinterpret_cast<const u8*>(old_walker.data()),
                                     sizeof(old_walker)) == nullptr);

    Fixture large;
    std::vector<u32> source(4200);
    for (u32 i = 0; i < source.size(); ++i) source[i] = i ^ 0x81000000u;
    large.SetPointer(source.data());
    Value last;
    for (u32 i = 0; i < 4085; ++i) last = large.Read(Value{i + 19});
    large.Compile();
    CHECK(large.Index(last) > 4095);
    CHECK(large.Result(last) == source[4103]);
    const auto tagged_address = reinterpret_cast<uintptr_t>(&source[4103]);
    const auto source_address = tagged_address & 0x0000ffffffffffffULL;
    // Heap pointers can carry a host top-byte tag. Pointer traversal masks guest
    // addresses to 48 bits before loading, and records that canonical address.
    std::printf("large source address actual=%#llx tagged=%#llx canonical=%#llx\n",
                static_cast<unsigned long long>(large.SourceAddress(last)),
                static_cast<unsigned long long>(tagged_address),
                static_cast<unsigned long long>(source_address));
    CHECK(large.SourceAddress(last) == source_address);
}

void CheckLateReadConsts() {
    using Shader::Optimization::ResourceDiscoveryList;
    Fixture f;
    f.data[2] = 17;
    auto dependency = f.Read(Value{2u});
    auto dependent = f.Read(dependency);
    auto resource = f.Read(Value{5u});
    auto resource_duplicate = f.Read(Value{5u});
    auto ordinary = f.Read(Value{7u});
    auto ordinary_duplicate = f.Read(Value{7u});
    auto buffer = f.Read(Value{8u}, true, true);
    ResourceDiscoveryList usages(1);
    usages[0].sharps[0].num_dwords = 1;
    usages[0].sharps[0].dwords[0] = resource_duplicate;
    f.Compile(usages, true);
    CHECK(f.SourceAddress(dependency) == reinterpret_cast<uintptr_t>(&f.data[2]));
    CHECK(f.SourceAddress(resource) == reinterpret_cast<uintptr_t>(&f.data[5]));
    CHECK(f.SourceAddress(resource_duplicate) == f.SourceAddress(resource));
    CHECK(f.SourceAddress(buffer) == reinterpret_cast<uintptr_t>(&f.data[8]));
    for (auto value : {dependent, ordinary, ordinary_duplicate}) {
        CHECK(f.SourceAddress(value) == 0);
        CHECK(value.Inst()->Flags<u32>() == (0x80000000u | f.Index(value)));
    }
    CHECK(f.Result(dependent) == f.data[17]);
    CHECK(f.Result(ordinary) == f.data[7]);
    CHECK(f.info.flattened_ud_ignored_source_indices.size() == 2);
    CHECK(std::ranges::is_sorted(f.info.flattened_ud_ignored_source_indices));
    f.data[2] = 29;
    f.info.RefreshFlatBuf();
    CHECK(f.Result(dependent) == f.data[29]);
    CHECK(f.SourceAddress(dependent) == 0);
    CHECK(f.SourceAddress(dependency) == reinterpret_cast<uintptr_t>(&f.data[2]));

    for (u32 depth : {64u, 65u}) {
        Fixture chain;
        auto read = chain.Read(Value{3u});
        auto value = read;
        for (u32 i = 0; i < depth; ++i) value = chain.Add(Opcode::BitwiseNot32, {value});
        ResourceDiscoveryList resources(1);
        resources[0].sharps[1].num_dwords = 1;
        resources[0].sharps[1].dwords[0] = value;
        chain.Compile(resources, true);
        CHECK((chain.SourceAddress(read) != 0) == (depth == 64));
    }
    Fixture cycle;
    auto read = cycle.Read(Value{6u});
    auto phi = cycle.Add(Opcode::Phi, {});
    phi.Inst()->SetFlags(IR::Type::U32);
    phi.Inst()->AddPhiOperand(&cycle.block, read);
    phi.Inst()->AddPhiOperand(&cycle.block, phi);
    ResourceDiscoveryList resources(1);
    resources[0].sharps[0].num_dwords = 1;
    resources[0].sharps[0].dwords[0] = phi;
    cycle.Compile(resources, true);
    CHECK(cycle.SourceAddress(read) == reinterpret_cast<uintptr_t>(&cycle.data[6]));
}

void CheckReadConstConsumer() {
    namespace SPV = Shader::Backend::SPIRV;
    const bool previous_dma = EmulatorSettings.IsDirectMemoryAccessEnabled();
    for (bool dma : {false, true}) {
        EmulatorSettings.SetDirectMemoryAccessEnabled(dma);
        for (u32 flags : {0u, 17u, 0x80000011u}) {
            Fixture f;
            f.info.stage = Shader::Stage::Compute;
            f.info.l_stage = Shader::LogicalStage::Compute;
            f.info.flattened_ud_buf.resize(64);
            auto read = f.Read(Value{3u});
            read.Inst()->SetFlags(flags);
            Shader::Profile profile{};
            profile.supported_spirv = 0x00010500;
            profile.support_int64 = true;
            profile.max_ubo_size = 16384;
            Shader::Optimization::CollectShaderInfoPass(f.program, profile);
            const bool late = (flags & 0x80000000u) != 0;
            const auto expected_types = !dma || flags == 17u ? Shader::Info::ReadConstType::None
                : flags == 0 ? Shader::Info::ReadConstType::Immediate | Shader::Info::ReadConstType::Dynamic
                : Shader::Info::ReadConstType::Immediate;
            CHECK(f.info.readconst_types == expected_types);
            CHECK(f.info.uses_dma == (dma && (flags == 0 || late)));
            Shader::RuntimeInfo runtime{};
            runtime.Initialize(Shader::Stage::Compute);
            Shader::Backend::Bindings bindings{};
            SPV::EmitContext ctx{profile, runtime, f.info, bindings};
            const auto type = ctx.TypeFunction(ctx.U32[1]);
            ctx.OpFunction(ctx.U32[1], spv::FunctionControlMask::MaskNone, type);
            ctx.AddLabel();
            const auto address = ctx.ConstU32(0x2000u, 0u);
            const auto offset = ctx.ConstU32(3u);
            const auto result = SPV::EmitReadConst(ctx, read.Inst(), address, offset);
            ctx.OpReturnValue(result);
            ctx.OpFunctionEnd();
            const auto words = ctx.Assemble();
            bool found = false;
            for (size_t i = 5; i < words.size();) {
                const u32 count = words[i] >> 16;
                const auto opcode = static_cast<spv::Op>(words[i] & 0xffff);
                CHECK(count > 0 && i + count <= words.size());
                if ((opcode == spv::Op::OpLoad || opcode == spv::Op::OpFunctionCall) &&
                    count >= 3 && words[i + 2] == result.value) {
                    found = true;
                    if (dma && (flags == 0 || late)) {
                        CHECK(opcode == spv::Op::OpFunctionCall);
                        CHECK(count == (late ? 7u : 6u));
                        CHECK(words[i + 3] == (late ? ctx.read_const.value : ctx.read_const_dynamic.value));
                        CHECK(words[i + 4] == address.value && words[i + 5] == offset.value);
                        if (late) CHECK(words[i + 6] == ctx.ConstU32(17u).value);
                    } else {
                        CHECK(opcode == spv::Op::OpLoad);
                    }
                }
                if (opcode == spv::Op::OpConstant && count == 4) {
                    CHECK(words[i + 3] != 0x80000011u);
                }
                i += count;
            }
            CHECK(found);
        }
    }
    EmulatorSettings.SetDirectMemoryAccessEnabled(previous_dma);
}

} // namespace

int main() {
    alarm(12);
    const auto cases = CheckOperators();
    CheckReadLeavesAndRefresh();
    CheckNestedPointerAndFaultRecovery();
    CheckCachedWalkerPublication();
    CheckAddressCarrySimplification();
    CheckDynamicOffsetWrap();
    CheckSharedSlots();
    CheckGuardedPointerPhi(false);
    CheckGuardedPointerPhi(true);
    CheckGuardedPointerPhi(false, 0, true, true);
    for (u32 source_kind = 0; source_kind < 3; ++source_kind)
        CheckGuardedPointerPhi(false, source_kind, false);
    CheckGuardedPointerPhi(false, 1);
    CheckGuardedPointerPhi(false, 2);
    CheckSpecializedReadValueNumbers();
    CheckPhiAndBitcastOffsets();
    CheckLiveReadFallback();
    CheckLaneOffsetFallbacks();
    CheckSourceAddresses();
    CheckLateReadConsts();
    CheckReadConstConsumer();
    alarm(0);
    std::printf("SrtFlattenContractTest: PASS arithmetic_operators=12 arithmetic_cases=%zu "
                "leaf_kinds=3 dynamic_refresh=PASS nested_pointer=PASS fault_recovery=PASS "
                "cached_code_publication=PASS concurrent_walker_execution=PASS "
                "address_add_carry_pattern=PASS dynamic_offset_wrap32=PASS shared_slots=PASS phi_cycles=PASS bitcasts=PASS "
                "live_read_fallback=PASS fallback_fault_recovery=PASS lane_fallback=PASS "
                "buffer_shape_rejection=PASS source_addresses=PASS large_source_destination=PASS "
                "late_readconst_dependencies=PASS blacklist_refresh=PASS dependency_cycles_and_limit=PASS "
                "readconst_spirv_consumer=PASS production_walker=executed\n", cases);
}
