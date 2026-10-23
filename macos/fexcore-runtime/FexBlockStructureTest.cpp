#include "AndroidFexCoreRuntime.h"
#include "FexStructuralDiagnostics.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "Interface/IR/Passes/LumenStructureTrace.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstdio>
#include <cstring>

namespace IR = FEXCore::IR;
namespace Trace = IR::LumenStructureTrace;
namespace Diagnostics = Lumen::FexCore::StructuralDiagnostics;

namespace {
unsigned deliveries = 0;
bool reportValid = true;
Diagnostics::StageSummary delivered;

class TestEmitter final : public IR::IREmitter {
public:
    using IR::IREmitter::IREmitter;
    auto Begin() {
        ReownOrClaimBuffer();
        auto header = _IRHeader(InvalidNode, 0, 0, 0, 0);
        auto block = CreateCodeNode();
        header.first->Blocks = block.Node->Wrapped(DualListData.ListBegin());
        SetCurrentCodeBlock(block);
        return block;
    }
    void Exit() {
        _ExitFunction(IR::OpSize::i64Bit, _InlineConstant(0), IR::BranchHint::None,
                      InvalidNode, InvalidNode, false);
    }
};
}

int main() {
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return 1;
    FEXCore::Utils::PooledAllocatorMalloc allocator;
    TestEmitter emit(allocator, false);
    unsigned failures = 0;
    auto check = [&failures](bool condition, const char* label) {
        if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
    };
    auto begin = [&]() { return emit.Begin(); };
    auto exit = [&]() { emit.Exit(); };

    begin();
    exit();
    check(Trace::Inspect(&emit) == Trace::Counts {1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, "valid block");
    LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* text) {
        ++deliveries;
        reportValid &= level == LogMan::ERROR && Diagnostics::ParseStage(text, delivered);
    });
    Trace::Report(0, Trace::Inspect(&emit));
    check(deliveries == 0, "valid stages emit nothing");

    emit._GuestOpcode(0);
    const auto afterExit = Trace::Inspect(&emit);
    check(afterExit == Trace::Counts {1, 1, 0, 1, 1, 0, 1, 0, 0, 0}, "metadata after exit");
    Trace::Report(4, afterExit);
    check(deliveries == 1 && reportValid && delivered.stage == 4 && delivered.counts == afterExit,
          "production walker reports closed schema");
    check(Trace::Inspect(&emit, Trace::BlockLimit, 1)[9] == 1, "node scan cap");

    begin();
    check(Trace::Inspect(&emit) == Trace::Counts {1, 1, 0, 0, 0, 1, 0, 0, 0, 0}, "empty block");
    exit();
    emit._StoreContextGPR(IR::OpSize::i32Bit, emit._Constant(0), offsetof(FEXCore::Core::CPUState, mxcsr));
    check(Trace::Inspect(&emit) == Trace::Counts {1, 1, 0, 1, 1, 0, 0, 1, 0, 0}, "state store after exit");

    auto first = begin();
    exit();
    auto second = emit.CreateNewCodeBlockAtEnd();
    emit.SetCurrentCodeBlock(second);
    check(Trace::Inspect(&emit) == Trace::Counts {2, 1, 0, 0, 0, 1, 0, 0, 0, 0}, "mixed valid and empty blocks");
    check(Trace::Inspect(&emit, 1) == Trace::Counts {1, 0, 0, 0, 0, 0, 0, 0, 0, 1}, "block scan cap");
    (void)first;

    auto malformed = begin();
    const auto view = emit.ViewIR();
    // Synthetic malformed boundary only; never execute or run validation over it.
    view.GetOp<IR::IROp_Header>(malformed.first->Last)->Op = IR::OP_DUMMY;
    check(Trace::Inspect(&emit) == Trace::Counts {1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, "missing EndBlock");
    LogMan::Msg::UnInstallHandler();
    std::printf("FEX block structure walker: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
