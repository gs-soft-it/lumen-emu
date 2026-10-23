// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include "common/object_pool.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestMixedSourceProducer(bool stride_patch) {
    Shader::Info info{};
    info.pgm_base = 0;
    info.flattened_ud_buf.assign(16, 0x3f800000);
    AmdGpu::Buffer expected{};
    expected.base_address = 0x8102030405;
    expected.stride = 16;
    expected.num_records = 512;
    expected.data_format = 4;
    expected.num_format = 7;
    const auto words = std::bit_cast<std::array<u32, 4>>(expected);
    info.flattened_ud_buf[2] = words[0];
    info.flattened_ud_buf[9] = words[2];
    info.flattened_ud_buf[13] = words[1];

    Common::ObjectPool<Shader::IR::Inst> pool{64};
    Shader::IR::Block block{pool};
    Shader::IR::IREmitter ir{block};
    const auto dw0 = ir.GetUserData(static_cast<Shader::IR::ScalarReg>(2));
    auto dw1 = ir.Imm32(words[1]);
    if (stride_patch) {
        const auto original = ir.GetUserData(static_cast<Shader::IR::ScalarReg>(13));
        dw1 = Shader::IR::U32{ir.BitwiseOr(original, ir.Imm32(0x00400000))};
        expected.stride |= 0x40;
    }
    const auto dw2 = ir.GetUserData(static_cast<Shader::IR::ScalarReg>(9));
    const auto handle = ir.CompositeConstruct(dw0, dw1, dw2, ir.Imm32(words[3]));
    const auto read = ir.ReadConstBuffer(handle, ir.Imm32(0), {});
    Shader::IR::Program program{info};
    program.blocks.push_back(&block);
    const Shader::Profile profile{};

    const auto resources = Shader::Optimization::ResourceDiscoverPass(program, profile);
    Require(resources.size() == 1, "resource discovery did not preserve the buffer use");
    Shader::Optimization::ResourcePatchingPass(info, resources, profile);
    Require(info.buffers.size() == 1, "resource patching did not produce one buffer");
    Require(info.buffers[0].GetSharp(info) == expected,
            "producer reconstructed adjacent float data as a buffer descriptor");
    Require(info.buffers[0].sharp_fetch.offsets[0] == 2 &&
                info.buffers[0].sharp_fetch.offsets[2] == 9,
            "producer collapsed independently sourced descriptor words");
    Require(info.buffers[0].used_as_readconst, "read-constant resource classification lost");
    Require(read.Inst()->Arg(0).IsImmediate() && read.Inst()->Arg(0).U32() == 0,
            "buffer instruction was not rewritten to the discovered binding");
    Require(info.buffers[0].post_op ==
                (stride_patch ? Shader::SharpFetchPostOp::BitwiseOrDw1WithImm
                              : Shader::SharpFetchPostOp::None),
            "producer failed to record the stride post operation");
}

namespace IR = Shader::IR;

struct GuardedDescriptorFixture {
    Common::ObjectPool<IR::Inst> pool{128};
    IR::Block entry{pool}, changed{pool}, merge{pool}, guard{pool}, consumer{pool},
        inner_exit{pool}, exit{pool};
    Shader::Info info{};
    IR::Program program{info};
    std::array<u32, 16> user_data{};
    std::array<u32, 64> table{};
    std::array<IR::Value, 4> original_sources{}, changed_sources{}, phis{};
    IR::Value read;
    AmdGpu::Buffer original{}, alternate{};
    bool select_original;
    bool stride_patch;
    bool invert_guard;
    static constexpr std::array<u32, 4> table_offsets{2, 9, 13, 19};

    GuardedDescriptorFixture(bool select_original_, bool reverse_incoming, bool nested,
                             bool stride_patch_, bool invert_guard_ = false)
        : select_original{select_original_}, stride_patch{stride_patch_},
          invert_guard{invert_guard_} {
        original.base_address = 0x8102030405;
        original.stride = 16;
        original.num_records = 512;
        original.data_format = 4;
        original.num_format = 7;
        alternate = original;
        alternate.base_address = 0x4405060708;
        alternate.stride = 32;
        alternate.num_records = 123;
        table.fill(0x3f800000);
        const auto pointer = reinterpret_cast<uintptr_t>(table.data());
        std::memcpy(user_data.data(), &pointer, sizeof(pointer));
        info.user_data = user_data;
        UpdateSources();

        IR::IREmitter first{entry};
        const auto base = first.CompositeConstruct(first.GetUserData(IR::ScalarReg::S0),
                                                  first.GetUserData(IR::ScalarReg::S1));
        auto condition = first.IEqual(first.GetUserData(IR::ScalarReg::S4), first.Imm32(0));
        if (invert_guard) condition = first.LogicalNot(condition);
        const auto inverse = first.LogicalNot(first.ConditionRef(condition));
        const auto overwrite_condition = first.ConditionRef(inverse);
        original_sources = {first.GetUserData(IR::ScalarReg::S8),
                            first.GetUserData(IR::ScalarReg::S9),
                            first.ReadConst(base, first.Imm32(40)),
                            first.Imm32(std::bit_cast<std::array<u32, 4>>(original)[3])};

        IR::IREmitter overwrite{changed};
        for (size_t i = 0; i < changed_sources.size(); ++i)
            changed_sources[i] = overwrite.ReadConst(base, overwrite.Imm32(table_offsets[i]));
        for (size_t i = 0; i < phis.size(); ++i) {
            merge.AppendNewInst(IR::Opcode::Phi, {});
            auto& phi = merge.back();
            phi.SetFlags(IR::Type::U32);
            if (reverse_incoming) {
                phi.AddPhiOperand(&entry, original_sources[i]);
                phi.AddPhiOperand(&changed, changed_sources[i]);
            } else {
                phi.AddPhiOperand(&changed, changed_sources[i]);
                phi.AddPhiOperand(&entry, original_sources[i]);
            }
            phis[i] = IR::Value{&phi};
        }

        IR::IREmitter branch{nested ? guard : merge};
        const auto selected_condition = branch.ConditionRef(
            select_original ? branch.LogicalNot(branch.LogicalNot(condition)) : inverse);
        IR::IREmitter use{consumer};
        auto dw1 = IR::U32{phis[1]};
        if (stride_patch) dw1 = IR::U32{use.BitwiseOr(dw1, use.Imm32(0x00400000))};
        const auto handle = use.CompositeConstruct(IR::U32{phis[0]}, dw1,
                                                  IR::U32{phis[2]}, IR::U32{phis[3]});
        read = use.ReadConstBuffer(handle, use.Imm32(0), {});

        entry.AddBranch(&changed);
        entry.AddBranch(&merge);
        changed.AddBranch(&merge);
        AddBlock(entry);
        AddIf(overwrite_condition, changed, merge);
        AddBlock(changed);
        AddEndIf(merge);
        AddBlock(merge);
        if (nested) {
            IR::IREmitter outer{merge};
            const auto unrelated = outer.IEqual(outer.GetUserData(IR::ScalarReg::S5),
                                                outer.Imm32(0));
            merge.AddBranch(&guard);
            merge.AddBranch(&exit);
            AddIf(outer.ConditionRef(unrelated), guard, exit);
            AddBlock(guard);
            guard.AddBranch(&consumer);
            guard.AddBranch(&inner_exit);
            consumer.AddBranch(&inner_exit);
            inner_exit.AddBranch(&exit);
            AddIf(selected_condition, consumer, inner_exit);
            AddBlock(consumer);
            AddEndIf(inner_exit);
            AddBlock(inner_exit);
            AddEndIf(exit);
        } else {
            merge.AddBranch(&consumer);
            merge.AddBranch(&exit);
            consumer.AddBranch(&exit);
            AddIf(selected_condition, consumer, exit);
            AddBlock(consumer);
            AddEndIf(exit);
        }
        AddBlock(exit);
        program.syntax_list.push_back({{}, IR::AbstractSyntaxNode::Type::Return});
        program.post_order_blocks.assign(program.blocks.rbegin(), program.blocks.rend());
    }

    void UpdateSources() {
        const auto words = std::bit_cast<std::array<u32, 4>>(original);
        std::copy(words.begin(), words.end(), user_data.begin() + 8);
        table[40] = words[2];
        const auto other = std::bit_cast<std::array<u32, 4>>(alternate);
        for (size_t i = 0; i < other.size(); ++i) table[table_offsets[i]] = other[i];
        user_data[4] = select_original != invert_guard ? 0 : 1;
    }

    void AddBlock(IR::Block& block) {
        program.blocks.push_back(&block);
        program.syntax_list.push_back({{.block = &block}, IR::AbstractSyntaxNode::Type::Block});
    }
    void AddIf(IR::U1 condition, IR::Block& body, IR::Block& end) {
        program.syntax_list.push_back(
            {{.if_node = {condition, &body, &end}}, IR::AbstractSyntaxNode::Type::If});
    }
    void AddEndIf(IR::Block& end) {
        program.syntax_list.push_back({{.end_if = {&end}}, IR::AbstractSyntaxNode::Type::EndIf});
    }
    AmdGpu::Buffer Expected() const {
        auto expected = select_original ? original : alternate;
        if (stride_patch) expected.stride |= 0x40;
        return expected;
    }
};

void TestGuardedDescriptor(bool invert_guard, bool reverse_incoming, bool nested,
                           bool stride_patch) {
    GuardedDescriptorFixture f{true, reverse_incoming, nested, stride_patch, invert_guard};
    const Shader::Profile profile{};
    const auto resources = Shader::Optimization::ResourceDiscoverPass(f.program, profile);
    Require(resources.size() == 1, "guarded descriptor lost its buffer use");
    const auto& expected_sources = f.original_sources;
    for (size_t i = 0; i < expected_sources.size(); ++i) {
        Require(resources[0].sharps[0].dwords[i] == expected_sources[i],
                "descriptor selected an incoming value from an incompatible branch");
        Require(f.phis[i].Inst()->GetOpcode() == IR::Opcode::Phi &&
                    f.phis[i].Inst()->NumArgs() == 2,
                "resource discovery changed the shared SSA phi");
    }
    Shader::Optimization::FlattenExtendedUserdataPass(f.program, resources, false);
    Shader::Optimization::ResourcePatchingPass(f.info, resources, profile);
    const auto binding = f.read.Inst()->Arg(0).U32();
    const auto& resource = f.info.buffers.at(binding);
    Require(resource.GetSharp(f.info) == f.Expected(),
            "guarded resource did not reconstruct the complete selected descriptor");
    Require(resource.post_op ==
                (stride_patch ? Shader::SharpFetchPostOp::BitwiseOrDw1WithImm
                              : Shader::SharpFetchPostOp::None),
            "guarded resource lost the stride post operation");
    f.original.base_address += 0x200000000;
    f.original.stride = 24;
    f.original.num_records = 999;
    f.alternate.base_address += 0x100000000;
    f.alternate.stride = 8;
    f.alternate.num_records = 777;
    f.UpdateSources();
    f.info.RefreshFlatBuf();
    Require(resource.GetSharp(f.info) == f.Expected(),
            "guarded descriptor retained stale userdata or SRT words after refresh");
}

void TestAmbiguousDescriptorRemainsUnresolved() {
    for (bool reverse : {false, true}) {
        GuardedDescriptorFixture f{true, reverse, false, false};
        Shader::Optimization::SharpReference sharp{};
        sharp.num_dwords = 4;
        std::copy(f.phis.begin(), f.phis.end(), sharp.dwords.begin());
        Shader::Optimization::ResolveSharpPhiSources(f.program, f.merge, sharp);
        Require(std::equal(f.phis.begin(), f.phis.end(), sharp.dwords.begin()),
                "unguarded descriptor arbitrarily selected one phi incoming");

        IR::IREmitter branch{f.merge};
        const auto unrelated = branch.ConditionRef(
            branch.IEqual(branch.GetUserData(IR::ScalarReg::S5), branch.Imm32(0)));
        for (auto& node : f.program.syntax_list) {
            if (node.type == IR::AbstractSyntaxNode::Type::If &&
                node.data.if_node.body == &f.consumer)
                node.data.if_node.cond = unrelated;
        }
        Shader::Optimization::ResolveSharpPhiSources(f.program, f.consumer, sharp);
        Require(std::equal(f.phis.begin(), f.phis.end(), sharp.dwords.begin()),
                "unrelated consumer guard arbitrarily selected one phi incoming");

        // The entry predecessor has no lexical guard. Proving that its direct
        // edge cannot reach this use would require stronger edge analysis.
        GuardedDescriptorFixture compatible{false, reverse, false, false};
        std::copy(compatible.phis.begin(), compatible.phis.end(), sharp.dwords.begin());
        Shader::Optimization::ResolveSharpPhiSources(compatible.program, compatible.consumer,
                                                    sharp);
        Require(std::equal(compatible.phis.begin(), compatible.phis.end(), sharp.dwords.begin()),
                "two compatible predecessors were resolved without sufficient path facts");
    }
}

void TestLoopDescriptorRemainsUnresolved() {
    GuardedDescriptorFixture f{true, false, false, false};
    IR::Block loop_entry{f.pool}, loop_exit{f.pool};
    loop_entry.AddBranch(&f.entry);
    f.exit.AddBranch(&f.entry);
    f.exit.AddBranch(&loop_exit);

    // The same condition instruction observes a different counter value on
    // successive iterations. Its lexical identity is not a stable path fact.
    const auto counter_it = f.entry.PrependNewInst(f.entry.begin(), IR::Opcode::Phi);
    counter_it->SetFlags(IR::Type::U32);
    const IR::U32 counter{IR::Value{&*counter_it}};
    counter_it->AddPhiOperand(&loop_entry, IR::Value{0u});
    IR::IREmitter end{f.exit};
    const auto next = end.IAdd(counter, end.Imm32(1));
    counter_it->AddPhiOperand(&f.exit, next);
    for (auto& inst : f.entry)
        if (inst.GetOpcode() == IR::Opcode::IEqual32) inst.SetArg(0, counter);

    f.program.syntax_list.pop_back(); // Replace the original return with a loop backedge.
    f.program.syntax_list.insert(f.program.syntax_list.begin(),
        {{.loop = {&f.entry, &f.exit, &loop_exit}}, IR::AbstractSyntaxNode::Type::Loop});
    f.program.syntax_list.insert(f.program.syntax_list.begin(),
        {{.block = &loop_entry}, IR::AbstractSyntaxNode::Type::Block});
    const auto repeat = end.LogicalNot(end.IEqual(next, end.Imm32(4)));
    f.program.syntax_list.push_back(
        {{.repeat = {repeat, &f.entry, &loop_exit}}, IR::AbstractSyntaxNode::Type::Repeat});
    f.program.syntax_list.push_back(
        {{.block = &loop_exit}, IR::AbstractSyntaxNode::Type::Block});
    f.program.syntax_list.push_back({{}, IR::AbstractSyntaxNode::Type::Return});
    f.program.blocks.insert(f.program.blocks.begin(), &loop_entry);
    f.program.blocks.push_back(&loop_exit);

    Shader::Optimization::SharpReference sharp{};
    sharp.num_dwords = 4;
    std::copy(f.phis.begin(), f.phis.end(), sharp.dwords.begin());
    Shader::Optimization::ResolveSharpPhiSources(f.program, f.consumer, sharp);
    Require(std::equal(f.phis.begin(), f.phis.end(), sharp.dwords.begin()),
            "loop-dependent conditions incorrectly selected a descriptor incoming");
}

void TestCyclicDescriptorRemainsUnresolved() {
    for (bool has_leaf : {false, true}) {
        for (bool leaf_first : {false, true}) {
            Common::ObjectPool<IR::Inst> pool{16};
            IR::Block entry{pool}, block{pool};
            Shader::Info info{};
            IR::Program program{info};
            program.blocks = {&entry, &block};
            program.syntax_list = {
                {{.block = &entry}, IR::AbstractSyntaxNode::Type::Block},
                {{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                {{}, IR::AbstractSyntaxNode::Type::Return}};
            entry.AddBranch(&block);
            block.AddBranch(&block);
            IR::IREmitter ir{entry};
            const auto leaf = ir.GetUserData(IR::ScalarReg::S8);
            block.AppendNewInst(IR::Opcode::Phi, {});
            const IR::Value first{&block.back()};
            first.Inst()->SetFlags(IR::Type::U32);
            block.AppendNewInst(IR::Opcode::Phi, {});
            const IR::Value second{&block.back()};
            second.Inst()->SetFlags(IR::Type::U32);
            first.Inst()->AddPhiOperand(&block, second);
            if (has_leaf && leaf_first) second.Inst()->AddPhiOperand(&entry, leaf);
            second.Inst()->AddPhiOperand(&block, first);
            if (has_leaf && !leaf_first) second.Inst()->AddPhiOperand(&entry, leaf);

            // Exercise dependency cycles independently of the ASL loop check.
            // Neither an available leaf nor a different phi may replace the root.
            Shader::Optimization::SharpReference sharp{};
            sharp.num_dwords = 2;
            sharp.dwords[0] = first;
            sharp.dwords[1] = second;
            Shader::Optimization::ResolveSharpPhiSources(program, block, sharp);
            Require(sharp.dwords[0] == first && sharp.dwords[1] == second,
                    "cyclic descriptor selected a leaf or aliased another phi");
        }
    }
}

} // namespace

int main() {
    try {
        TestMixedSourceProducer(false);
        TestMixedSourceProducer(true);
        TestAmbiguousDescriptorRemainsUnresolved();
        TestLoopDescriptorRemainsUnresolved();
        TestCyclicDescriptorRemainsUnresolved();
        for (bool invert_guard : {false, true})
            for (bool reverse : {false, true})
                for (bool nested : {false, true})
                    for (bool stride : {false, true})
                        TestGuardedDescriptor(invert_guard, reverse, nested, stride);
        std::cout << "resource discovery and patching descriptor contracts PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
