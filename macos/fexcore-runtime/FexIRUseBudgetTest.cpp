// Original synthetic IR fixtures; no guest program or upstream test is used.
#include "AndroidFexCoreRuntime.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "Interface/IR/IREmitter.h"
#include "Interface/IR/PassManager.h"
#include "Interface/IR/Passes/IRValidation.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace IR = FEXCore::IR;

namespace {
unsigned failures = 0;
unsigned cases = 0;
unsigned validationErrors = 0;
const char* currentCase = "initialization";

void Check(bool condition, const char* invariant) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s: %s\n", currentCase, invariant);
        ++failures;
    }
}

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
        // The emitter reserves gregs for LoadRegister/StoreRegister. A real
        // 64-bit non-register context field keeps these unexecuted graphs valid.
        return _LoadContextGPR(IR::OpSize::i64Bit, offsetof(FEXCore::Core::CPUState, rip));
    }

    IR::Ref Store(IR::Ref value) {
        return _StoreContextGPR(IR::OpSize::i64Bit, value, offsetof(FEXCore::Core::CPUState, rip));
    }

    void Exit() {
        _ExitFunction(IR::OpSize::i64Bit, _InlineConstant(0), IR::BranchHint::None,
                      InvalidNode, InvalidNode, false);
    }

    IR::AllNodesIterator At(IR::Ref node) {
        return IR::AllNodesIterator(ViewIR().at(node));
    }

    IR::AllNodesIterator End() {
        return ViewIR().GetAllCode().end();
    }
};

IR::Ref Argument(TestEmitter& emit, IR::Ref node, unsigned index = 0) {
    const auto view = emit.ViewIR();
    return view.GetNode(view.GetOp<IR::IROp_Header>(node)->Args[index]);
}

// Node IDs are local allocation offsets. Each comparison either uses one
// emitter or independently builds the same graph in the same allocation order.
// Shape excludes only arguments and use counts, the fields replacement owns.
std::vector<uint64_t> Snapshot(TestEmitter& emit, bool shapeOnly = false) {
    const auto view = emit.ViewIR();
    std::vector<uint64_t> result {view.GetSSACount(), view.GetHeader()->BlockCount,
        view.GetID(emit.GetWriteCursor()).Value, view.GetID(emit.GetCurrentBlock()).Value};
    for (uint32_t id = 0; id < view.GetSSACount(); ++id) {
        auto [node, op] = view.at(IR::NodeID {id})();
        result.insert(result.end(), {id, static_cast<uint64_t>(op->Op),
            static_cast<uint64_t>(op->Size), static_cast<uint64_t>(op->ElementSize),
            node->Header.Previous.ID().Value, node->Header.Next.ID().Value});
        if (!shapeOnly) {
            result.push_back(node->GetUses());
            for (unsigned arg = 0; arg < IR::GetArgs(op->Op); ++arg) {
                result.push_back(op->Args[arg].NodeOffset);
            }
        }
        if (op->Op == IR::OP_CODEBLOCK) {
            const auto* block = op->C<IR::IROp_CodeBlock>();
            result.insert(result.end(), {block->Begin.ID().Value, block->Last.ID().Value, block->ID});
        }
    }
    // Traversal order independently covers the linked block and code lists.
    for (auto [node, op] : view.GetAllCode()) {
        (void)op;
        result.push_back(view.GetID(node).Value);
    }
    return result;
}

bool Acyclic(TestEmitter& emit) {
    const auto view = emit.ViewIR();
    std::vector<std::vector<uint32_t>> inputs(view.GetSSACount());
    for (auto [node, op] : view.GetAllCode()) {
        // Control-flow targets are not data dependencies.
        if (!IR::GetHasDest(op->Op)) continue;
        const auto id = view.GetID(node).Value;
        for (unsigned arg = 0; arg < IR::GetRAArgs(op->Op); ++arg) {
            const auto input = op->Args[arg];
            if (input.IsImmediate() || !input.ID().IsValid()) continue;
            if (input.ID().Value >= inputs.size()) return false;
            inputs[id].push_back(input.ID().Value);
        }
    }
    std::vector<uint8_t> state(inputs.size());
    const auto visit = [&](const auto& self, uint32_t node) -> bool {
        if (state[node] == 1) return false;
        if (state[node] == 2) return true;
        state[node] = 1;
        for (const auto input : inputs[node]) {
            if (!self(self, input)) return false;
        }
        state[node] = 2;
        return true;
    };
    for (uint32_t node = 0; node < inputs.size(); ++node) {
        if (!visit(visit, node)) return false;
    }
    return true;
}

void Validate(TestEmitter& emit) {
    Check(Acyclic(emit), "data dependencies are acyclic");
    const auto errorsBefore = validationErrors;
    IR::Validation::IRValidation validator;
    validator.Run(&emit);
    Check(validationErrors == errorsBefore, "real FEX IR validator accepts graph and all refcounts");
}

struct Graph {
    IR::Ref raw {};
    IR::Ref result {};
    IR::Ref earlier {};
    IR::Ref first {};
    IR::Ref last {};
    IR::Ref end {};
    uint32_t originalUses {};
};

Graph ProtectedGraph(TestEmitter& emit, bool earlier = false, bool duplicate = false) {
    emit.Begin();
    Graph graph;
    graph.raw = emit.Raw();
    if (earlier) graph.earlier = emit.Store(graph.raw);
    graph.first = duplicate ? static_cast<IR::Ref>(emit._Add(IR::OpSize::i64Bit, graph.raw, graph.raw)) : emit.Store(graph.raw);
    graph.last = emit.Store(graph.raw);
    graph.originalUses = graph.raw->GetUses();
    emit.Exit();
    // This is the legalizer's ordering: snapshot original edges, then insert
    // a wrapper whose two raw inputs must remain outside the replacement range.
    emit.SetWriteCursor(earlier ? graph.earlier : graph.raw);
    graph.result = emit._Add(IR::OpSize::i64Bit, graph.raw, graph.raw);
    return graph;
}

Graph CrossBlockGraph(TestEmitter& emit) {
    emit.Begin();
    Graph graph;
    // Inline constants are permitted across blocks by the actual validator;
    // arbitrary cross-block local SSA values would form invalid fixtures.
    graph.raw = emit._InlineConstant(21);
    graph.result = emit._InlineConstant(23);
    graph.first = emit.Store(graph.raw);
    const IR::Ref second = emit.CreateNewCodeBlockAtEnd();
    emit._Jump(second);
    emit.SetCurrentCodeBlock(second);
    graph.last = emit.Store(graph.raw);
    graph.originalUses = graph.raw->GetUses();
    emit.Exit();
    return graph;
}

void CheckProtected(TestEmitter& emit, const Graph& graph) {
    Check(Argument(emit, graph.result, 0) == graph.raw && Argument(emit, graph.result, 1) == graph.raw,
          "wrapper retains both protected raw inputs");
}

void StartCase(const char* name) {
    currentCase = name;
    ++cases;
}

void RunTests(FEXCore::Utils::PooledAllocatorMalloc& allocator) {
    {
        StartCase("zero original uses and zero budget");
        TestEmitter emit(allocator, false);
        emit.Begin();
        Graph graph;
        graph.raw = emit.Raw();
        graph.originalUses = graph.raw->GetUses();
        graph.result = emit._Add(IR::OpSize::i64Bit, graph.raw, graph.raw);
        for (unsigned index = 0; index < 128; ++index) emit.Store(graph.result);
        emit.Exit();
        Validate(emit);
        const auto before = Snapshot(emit);
        Check(emit.ReplaceUsesWithAfterLimit(graph.raw, graph.result, graph.result, graph.originalUses) == 0,
              "zero budget returns zero");
        Check(Snapshot(emit) == before, "zero budget preserves every argument, cursor, link and refcount");
        CheckProtected(emit, graph);
        Validate(emit);
    }
    for (const bool duplicate : {false, true}) {
        StartCase(duplicate ? "duplicated argument edges" : "two protected wrapper edges");
        TestEmitter emit(allocator, false);
        const auto graph = ProtectedGraph(emit, false, duplicate);
        Validate(emit);
        const auto shape = Snapshot(emit, true);
        const uint32_t expected = duplicate ? 3 : 2;
        Check(graph.originalUses == expected, "snapshot counts original argument occurrences");
        Check(emit.ReplaceUsesWithAfterLimit(graph.raw, graph.result, graph.result, graph.originalUses) == expected,
              "each changed argument consumes one unit");
        Check(Argument(emit, graph.first) == graph.result && Argument(emit, graph.last) == graph.result,
              "all original consumers reference result");
        if (duplicate) Check(Argument(emit, graph.first, 1) == graph.result, "second edge in same consumer is replaced");
        Check(graph.raw->GetUses() == 2 && graph.result->GetUses() == expected, "exact protected raw and result refcounts");
        CheckProtected(emit, graph);
        Check(Snapshot(emit, true) == shape, "replacement preserves size, opcode, block lists and cursor");
        Validate(emit);
    }
    {
        StartCase("original use before range");
        TestEmitter emit(allocator, false);
        const auto graph = ProtectedGraph(emit, true);
        Validate(emit);
        Check(graph.originalUses == 3, "snapshot includes earlier original use");
        Check(emit.ReplaceUsesWithAfterLimit(graph.raw, graph.result, graph.result, graph.originalUses) == 2,
              "range may finish with budget remaining");
        Check(Argument(emit, graph.earlier) == graph.raw, "earlier use remains excluded");
        Check(Argument(emit, graph.first) == graph.result && Argument(emit, graph.last) == graph.result,
              "later uses are replaced");
        Check(graph.raw->GetUses() == 3 && graph.result->GetUses() == 2, "excluded and protected refcounts remain exact");
        CheckProtected(emit, graph);
        Validate(emit);
    }
    {
        StartCase("exclusive End and empty range");
        TestEmitter emit(allocator, false);
        auto graph = ProtectedGraph(emit);
        emit.SetWriteCursor(graph.last);
        graph.end = emit.Store(graph.raw);
        const auto emptyBefore = Snapshot(emit);
        Check(emit.ReplaceUsesWithRangeLimit(graph.raw, graph.result, emit.At(graph.last), emit.At(graph.last), 7) == 0,
              "empty range returns zero");
        Check(Snapshot(emit) == emptyBefore, "empty range preserves graph");
        Check(emit.ReplaceUsesWithRangeLimit(graph.raw, graph.result, emit.At(graph.first), emit.At(graph.last), 7) == 1,
              "Begin included and End excluded");
        Check(Argument(emit, graph.first) == graph.result && Argument(emit, graph.last) == graph.raw &&
              Argument(emit, graph.end) == graph.raw, "End and its following tail stay raw");
        Check(graph.raw->GetUses() == 4 && graph.result->GetUses() == 1, "exclusive range refcounts");
        CheckProtected(emit, graph);
        Validate(emit);
    }
    for (const uint32_t limit : {1U, 2U}) {
        StartCase(limit == 1 ? "intentional limit one" : "intentional limit two");
        TestEmitter emit(allocator, false);
        const auto graph = ProtectedGraph(emit, false, true);
        Check(emit.ReplaceUsesWithAfterLimit(graph.raw, graph.result, graph.result, limit) == limit,
              "intentional limit returns exactly consumed edges");
        Check(Argument(emit, graph.first, 0) == graph.result &&
              Argument(emit, graph.first, 1) == (limit == 2 ? graph.result : graph.raw) &&
              Argument(emit, graph.last) == graph.raw, "only first requested argument occurrences change");
        Check(graph.raw->GetUses() == 5 - limit && graph.result->GetUses() == limit, "partial replacement refcounts");
        CheckProtected(emit, graph);
        Validate(emit);
    }
    {
        StartCase("absent user with excluded use");
        TestEmitter emit(allocator, false);
        emit.Begin();
        const auto raw = emit.Raw();
        emit.Store(raw);
        const auto other = emit.Raw();
        const IR::Ref result = emit._Add(IR::OpSize::i64Bit, other, other);
        emit.Store(result);
        emit.Exit();
        const auto before = Snapshot(emit);
        Check(emit.ReplaceUsesWithAfterLimit(raw, result, result, std::numeric_limits<uint32_t>::max()) == 0,
              "absent match reaches End with maximum budget");
        Check(Snapshot(emit) == before && raw->GetUses() == 1, "absent match preserves excluded raw use and entire graph");
        Validate(emit);
    }
    {
        StartCase("maximum bound with protected and excluded uses");
        TestEmitter emit(allocator, false);
        const auto graph = ProtectedGraph(emit, true, true);
        Check(emit.ReplaceUsesWithAfterLimit(graph.raw, graph.result, graph.result, std::numeric_limits<uint32_t>::max()) == 3,
              "UINT32_MAX returns actual count without arithmetic wrap");
        Check(graph.raw->GetUses() == 3 && graph.result->GetUses() == 3, "maximum bound preserves excluded edges");
        CheckProtected(emit, graph);
        Validate(emit);
    }
    for (const uint32_t limit : {1U, std::numeric_limits<uint32_t>::max()}) {
        StartCase("identity replacement");
        TestEmitter emit(allocator, false);
        const auto graph = ProtectedGraph(emit, false, true);
        const auto before = Snapshot(emit);
        Check(emit.ReplaceUsesWithAfterLimit(graph.raw, graph.raw, graph.result, limit) == std::min(limit, 3U),
              "identity replacement terminates and counts matching edges");
        Check(Snapshot(emit) == before, "identity replacement has no net refcount or argument change");
        Validate(emit);
    }
    {
        StartCase("multiple code blocks");
        TestEmitter emit(allocator, false);
        const auto graph = CrossBlockGraph(emit);
        Validate(emit);
        const auto shape = Snapshot(emit, true);
        Check(emit.ReplaceUsesWithAfterLimit(graph.raw, graph.result, graph.result, graph.originalUses) == 2,
              "AllNodesIterator crosses first EndBlock");
        Check(Argument(emit, graph.first) == graph.result && Argument(emit, graph.last) == graph.result,
              "consumers in both blocks change");
        Check(graph.raw->GetUses() == 0 && graph.result->GetUses() == 2, "cross-block refcounts");
        Check(Snapshot(emit, true) == shape, "cross-block links, terminators and cursor preserved");
        Validate(emit);
    }
    for (unsigned variant = 0; variant < 5; ++variant) {
        StartCase("legacy API equivalence");
        TestEmitter bounded(allocator, false);
        TestEmitter legacy(allocator, false);
        const auto build = [variant](TestEmitter& emit) {
            return variant == 4 ? CrossBlockGraph(emit) : ProtectedGraph(emit, variant == 1, variant >= 2);
        };
        const auto candidate = build(bounded);
        const auto original = build(legacy);
        Check(Snapshot(bounded) == Snapshot(legacy), "independent original graphs match");
        uint32_t changed;
        if (variant == 3) {
            changed = bounded.ReplaceUsesWithRangeLimit(candidate.raw, candidate.result, bounded.At(candidate.first),
                bounded.At(candidate.last), std::numeric_limits<uint32_t>::max());
            legacy.ReplaceAllUsesWithRange(original.raw, original.result, legacy.At(original.first), legacy.At(original.last));
            Check(changed == 2, "legacy exclusive range duplicate edge count");
        } else {
            changed = bounded.ReplaceUsesWithAfterLimit(candidate.raw, candidate.result, candidate.result,
                std::numeric_limits<uint32_t>::max());
            legacy.ReplaceUsesWithAfter(original.raw, original.result, original.result);
            Check(changed == (variant == 2 ? 3U : 2U), "legacy full-range edge count");
        }
        Check(Snapshot(bounded) == Snapshot(legacy), "legacy and maximum-bound args, refcounts, sizes, blocks and cursor match");
        Validate(bounded);
        Validate(legacy);
    }
}
} // namespace

int main() {
    LumenAndroidFexCoreRuntimeStatusV1 status {};
    if (LumenAndroidFexCoreRuntimeProbeV1(&status) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return 1;
    LogMan::Throw::InstallHandler([](const char* message) {
        std::fprintf(stderr, "FAIL: %s: FEX assertion: %s\n", currentCase, message);
    });
    LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char* message) {
        if (level == LogMan::ERROR || level == LogMan::ASSERT) {
            ++validationErrors;
            std::fprintf(stderr, "FAIL: %s: FEX validator: %s\n", currentCase, message);
        }
    });
    FEXCore::Utils::PooledAllocatorMalloc allocator;
    RunTests(allocator);
    LogMan::Msg::UnInstallHandler();
    LogMan::Throw::UnInstallHandler();
    std::printf("FEX bounded IR replacement: %s cases=%u failures=%u validation_errors=%u\n",
        failures == 0 && validationErrors == 0 ? "PASS" : "FAIL", cases, failures, validationErrors);
    return failures == 0 && validationErrors == 0 ? 0 : 1;
}
