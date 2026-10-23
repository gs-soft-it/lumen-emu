// Original synthetic IR; compares the real validator at the admitted FEX pins.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"
#include "Interface/IR/Passes/IRValidation.h"
#include "Interface/IR/RegisterAllocationData.h"
#include "Utils/Allocator.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/Allocator.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unistd.h>

namespace IR = FEXCore::IR;

namespace {
class TestEmitter final : public IR::IREmitter {
public:
    using IR::IREmitter::IREmitter;
    IR::Ref Begin() {
        ReownOrClaimBuffer();
        auto header = _IRHeader(InvalidNode, 0, 0, 0, 0);
        auto block = CreateCodeNode();
        header.first->Blocks = WrapNode(block);
        SetCurrentCodeBlock(block);
        return block;
    }
    IR::Ref Raw() {
        return _LoadContextGPR(IR::OpSize::i64Bit, offsetof(FEXCore::Core::CPUState, rip));
    }
    void Store(IR::Ref value) {
        _StoreContextGPR(IR::OpSize::i64Bit, value, offsetof(FEXCore::Core::CPUState, rip));
    }
    void Exit() {
        _ExitFunction(IR::OpSize::i64Bit, _InlineConstant(0), IR::BranchHint::None,
                      InvalidNode, InvalidNode, false);
    }
};

// Ignore addresses and allocator padding; cover all graph fields the pass reads.
uint64_t Snapshot(TestEmitter& emit) {
    uint64_t hash = 14695981039346656037ULL;
    const auto add = [&hash](uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
    const auto view = emit.ViewIR();
    add(view.GetSSACount());
    add(view.PostRA());
    for (uint32_t id = 0; id < view.GetSSACount(); ++id) {
        auto [node, op] = view.at(IR::NodeID {id})();
        add(id); add(op->Op); add(static_cast<uint64_t>(op->Size));
        add(node->NumUses); add(node->Reg);
        add(node->Header.Previous.ID().Value); add(node->Header.Next.ID().Value);
        for (unsigned arg = 0; arg < IR::GetArgs(op->Op); ++arg) add(op->Args[arg].NodeOffset);
        if (op->Op == IR::OP_CODEBLOCK) {
            auto block = op->C<IR::IROp_CodeBlock>();
            add(block->Begin.ID().Value); add(block->Last.ID().Value);
        }
    }
    return hash;
}

void Build(TestEmitter& emit, std::string_view mode, unsigned nodes) {
    const auto block = emit.Begin();
    IR::Ref value = nullptr;
    for (unsigned i = 0; i < nodes; ++i) {
        if (mode == "mismatch-post" || mode == "invalid-class" || mode == "invalid-register") {
            value = emit._Add(IR::OpSize::i64Bit, emit._InlineConstant(i), emit._InlineConstant(i + 1));
        } else {
            value = emit.Raw();
        }
        if (mode == "clean" || mode == "bad-uses" || mode == "unsized") emit.Store(value);
    }
    if (mode == "cross-block-argument") {
        auto second = emit.CreateNewCodeBlockAtEnd();
        emit._Jump(second);
        emit.SetCurrentCodeBlock(second);
        emit.Store(value);
    }
    emit.Exit();
    auto view = emit.ViewIR();
    if (mode == "unsized") view.GetOp<IR::IROp_Header>(value)->Size = IR::OpSize::iInvalid;
    if (mode == "bad-uses") ++value->NumUses;
    if (mode == "mismatch-post" || mode == "invalid-class" || mode == "invalid-register") {
        view.GetHeader()->PostRA = true;
        for (auto [node, op] : view.GetAllCode()) {
            if (IR::GetHasDest(op->Op)) node->Reg = IR::PhysicalRegister(IR::RegClass::GPR, 1).Raw;
            if (op->Op == IR::OP_ADD && mode == "mismatch-post") {
                node->Reg = IR::PhysicalRegister(IR::RegClass::FPR, 1).Raw;
            }
        }
        if (mode == "invalid-class") value->Reg = IR::PhysicalRegister(IR::RegClass::Invalid, 1).Raw;
        if (mode == "invalid-register") value->Reg = IR::PhysicalRegister::Invalid().Raw;
    }
    if (mode == "missing-end" || mode == "missing-exit") {
        auto codeBlock = view.GetOp<IR::IROp_CodeBlock>(block);
        auto last = view.at(codeBlock->Last);
        if (mode == "missing-exit") --last;
        auto [node, op] = last();
        (void)node;
        op->Op = IR::OP_YIELD;
    }
}

void Log(LogMan::DebugLevels level, const char* message) {
    std::printf("log[%u]:%s\n", static_cast<unsigned>(level), message);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) return 2;
    const std::string_view mode = argv[1];
    const unsigned nodes = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 1;
    const unsigned iterations = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 0;
    if (!nodes || nodes > 8192 || iterations > 1000000) return 2;
    // Reject malformed fixtures without creating an OS crash report.
    std::signal(SIGTRAP, [](int) { std::_Exit(86); });
    std::signal(SIGILL, [](int) { std::_Exit(86); });
    LogMan::Msg::InstallHandler(Log);
    LogMan::Throw::InstallHandler([](const char* message) {
        std::printf("throw:%s\n", message);
        std::fflush(stdout);
        std::_Exit(86);
    });
    FEXCore::Allocator::InitializeAllocator(static_cast<size_t>(getpagesize()));
    FEXCore::Allocator::InitializeThread();
    FEXCore::Utils::PooledAllocatorMalloc allocator;
    TestEmitter emit(allocator, false);
    Build(emit, mode, nodes);
    const auto before = Snapshot(emit);
    if (mode == "invalid-header") emit.ViewIR().GetHeader()->Header.Op = IR::OP_YIELD;
    IR::Validation::IRValidation validator;
    validator.Run(&emit);
    if (Snapshot(emit) != before) return 3;
    if (!iterations) {
        std::printf("PASS:%s nodes=%u graph=%llu\n", argv[1], nodes,
                    static_cast<unsigned long long>(before));
        return 0;
    }
    for (unsigned i = 0; i < 100; ++i) validator.Run(&emit);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) validator.Run(&emit);
    const auto elapsed = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count();
    if (Snapshot(emit) != before) return 3;
    std::printf("%.9f\n", elapsed / iterations);
}
