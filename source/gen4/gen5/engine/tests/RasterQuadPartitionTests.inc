// SPDX-License-Identifier: GPL-2.0-only
namespace RasterQuadPartitionTests {
using namespace ShaderRecompiler;
using namespace IR;

Block* NewBlock(Program& program) {
  program.block_storage.push_back(std::make_unique<Block>());
  auto* block = program.block_storage.back().get();
  block->id = static_cast<uint32_t>(program.blocks.size());
  program.blocks.push_back(block);
  return block;
}
void Jump(Block* from, Block* to) {
  from->terminator.kind = CFG::TerminatorKind::Branch;
  from->terminator.true_block = to;
  from->AddBranch(to);
}
void Branch(Block* from, Value condition, Block* yes, Block* no, Block* merge) {
  from->condition = condition;
  from->terminator.kind = CFG::TerminatorKind::ConditionalBranch;
  from->terminator.true_block = yes;
  from->terminator.false_block = no;
  from->terminator.merge_block = merge;
  from->AddBranch(yes); from->AddBranch(no);
}
Value Vote(IREmitter& ir, Value mask) {
  return ir.Emit(ValueOpcode::ConditionRef, {mask}, CFG::BranchCondition::ExecNonZero);
}
void Export(Program& program, Block* block, Value value) {
  program.export_info.push_back(ExportInfo{.kind = ExportTargetKind::Mrt, .en = 15, .done = true, .vm = true});
  IREmitter ir(block);
  auto color = ir.Emit(ValueOpcode::CompositeConstructU32x4, {value, value, value, value});
  ir.Emit(ValueOpcode::SetAttribute, {color, Value(true)}, ExportFlags{});
}
Program Base() {
  Program program;
  program.stage = ShaderType::Pixel;
  program.wave_size = 64;
  program.resource_tracking_complete = true;
  return program;
}

enum class RegionCase {
  Valid, UnmaskedValue, VoteEscape, ScalarVarying, StrayVote, HiddenCycle,
  MissingMerge, WrongStage, WrongWave, Untracked, DppValid, DppMasked,
  DppNonbroadcast, DppAfterVote, Ballot, ReadFirst, AtomicAlias, Barrier,
  DirectConditionEscape, MultipleReturn
};
Program Region(RegionCase mode) {
  auto program = Base();
  auto* entry = NewBlock(program);
  auto* body = NewBlock(program);
  auto* merge = NewBlock(program);
  merge->terminator.kind = CFG::TerminatorKind::Return;
  IREmitter ir(entry);
  auto pixel = ir.Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)});
  auto source = ir.Emit(ValueOpcode::BitCastU32F32, {pixel});
  if (mode == RegionCase::DppValid || mode == RegionCase::DppMasked || mode == RegionCase::DppNonbroadcast) {
    DppMoveFlags flags;
    flags.control = mode == RegionCase::DppNonbroadcast ? 0x01 : 0x55;
    auto moved = ir.Emit(ValueOpcode::DppMoveU32,
                         {source, Value(mode != RegionCase::DppMasked)}, flags);
    source = ir.Emit(ValueOpcode::DppUpdateU32,
                     {moved, source, Value(mode != RegionCase::DppMasked)}, flags);
  }
  auto mask = ir.Emit(ValueOpcode::FPOrdGreaterThan32, {pixel, Value::F32(0.0f)});
  auto vote = Vote(ir, mask);
  if (mode == RegionCase::ScalarVarying) vote.Instruction()->SetFlags(CFG::BranchCondition::ScalarInstruction);
  if (mode == RegionCase::StrayVote) ir.Emit(ValueOpcode::ConditionRef, {mask}, CFG::BranchCondition::ExecNonZero);
  if (mode == RegionCase::Ballot) ir.Emit(ValueOpcode::Ballot, {mask});
  if (mode == RegionCase::ReadFirst) ir.Emit(ValueOpcode::ReadFirstLane, {source, Value(true)});
  if (mode == RegionCase::Barrier) ir.Emit(ValueOpcode::Barrier, {});
  Branch(entry, vote, body, merge, mode == RegionCase::MissingMerge ? nullptr : merge);
  IREmitter inside(body);
  auto update = inside.Emit(ValueOpcode::SelectU32, {mask, Value(7u), source});
  if (mode == RegionCase::UnmaskedValue) update = Value(7u);
  if (mode == RegionCase::DppAfterVote) {
    DppMoveFlags flags;
    update = inside.Emit(ValueOpcode::DppMoveU32, {source, Value(true)}, flags);
  }
  Jump(body, merge);
  auto& phi = merge->AppendNewInst(ValueOpcode::Phi);
  phi.SetFlags(Type::U32);
  phi.AddPhiOperand(entry, source); phi.AddPhiOperand(body, update);
  Value output(&phi);
  IREmitter result(merge);
  if (mode == RegionCase::VoteEscape) output = result.Emit(ValueOpcode::SelectU32, {vote, Value(1u), Value(0u)});
  Export(program, merge, output);
  if (mode == RegionCase::HiddenCycle) {
    merge->terminator.kind = CFG::TerminatorKind::Branch;
    merge->terminator.true_block = body;
    merge->AddBranch(body);
  }
  if (mode == RegionCase::DirectConditionEscape) {
    auto* done = NewBlock(program);
    done->terminator.kind = CFG::TerminatorKind::Return;
    auto* alternate = NewBlock(program);
    Jump(alternate, done);
    // No Reference instruction: a raw CFG condition is still an observable use.
    Branch(merge, mask, done, alternate, done);
  }
  if (mode == RegionCase::MultipleReturn) {
    body->terminator.kind = CFG::TerminatorKind::Return;
  }
  if (mode == RegionCase::AtomicAlias) program.info.buffers.push_back(BufferResource{.atomic = true});
  if (mode == RegionCase::WrongStage) program.stage = ShaderType::Vertex;
  if (mode == RegionCase::WrongWave) program.wave_size = 16;
  if (mode == RegionCase::Untracked) program.resource_tracking_complete = false;
  return program;
}

enum class LoopCase { Valid, ScalarEscape, MergeEscape, CurrentState, FutureActivation, NoProgress,
                      ReversedExit, ContinueBypass, MissingMetadata, DppInHeader, NoExit, ScalarExit };
Program LoopProgram(LoopCase mode) {
  auto program = Base();
  auto* entry = NewBlock(program);
  auto* header = NewBlock(program);
  auto* test = NewBlock(program);
  auto* body = NewBlock(program);
  auto* merge = NewBlock(program);
  merge->terminator.kind = CFG::TerminatorKind::Return;
  IREmitter initial(entry);
  auto pixel = initial.Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)});
  auto bound = initial.Emit(ValueOpcode::ConvertS32F32, {pixel});
  Jump(entry, header);
  Jump(header, test);
  header->terminator.loop_header = mode != LoopCase::MissingMetadata;
  header->terminator.merge_block = merge;
  header->terminator.continue_block = body;
  auto& counter = header->AppendNewInst(ValueOpcode::Phi); counter.SetFlags(Type::U32);
  counter.AddPhiOperand(entry, Value(0u));
  auto& data = header->AppendNewInst(ValueOpcode::Phi); data.SetFlags(Type::U32);
  data.AddPhiOperand(entry, Value(0u));
  Value i(&counter), a(&data);
  IREmitter compare(test);
  auto mask = compare.Emit(ValueOpcode::SLessThan32, {i, bound});
  if (mode == LoopCase::FutureActivation) {
    auto delayed = compare.Emit(ValueOpcode::IEqual32, {i, Value(5u)});
    mask = compare.Emit(ValueOpcode::LogicalAnd, {mask, delayed});
  }
  auto vote = Vote(compare, mask);
  Branch(test, vote, mode == LoopCase::ReversedExit ? merge : body,
         mode == LoopCase::ReversedExit ? body : merge, nullptr);
  IREmitter update(body);
  auto next = update.Emit(ValueOpcode::IAdd32, {i, Value(1u)});
  const bool scalar = mode == LoopCase::ScalarEscape || mode == LoopCase::MergeEscape ||
                      mode == LoopCase::FutureActivation;
  if (!scalar) next = update.Emit(ValueOpcode::SelectU32, {mask, next, i});
  if (mode == LoopCase::NoProgress) next = i;
  counter.AddPhiOperand(body, next);
  auto next_data = update.Emit(ValueOpcode::IAdd32, {a, Value(1u)});
  next_data = update.Emit(ValueOpcode::SelectU32, {mask, next_data, a});
  data.AddPhiOperand(body, next_data);
  Value output = (mode == LoopCase::ScalarEscape || mode == LoopCase::MergeEscape) ? i : a;
  if (mode == LoopCase::CurrentState) {
    auto& previous = header->AppendNewInst(ValueOpcode::Phi); previous.SetFlags(Type::U32);
    previous.AddPhiOperand(entry, Value(0u));
    // Initially a==b, but after an active iteration a changes. Once inactive,
    // an extra guest iteration copies CURRENT a to b; skipping changes output.
    auto next_previous = update.Emit(ValueOpcode::SelectU32, {mask, Value(&previous), a});
    previous.AddPhiOperand(body, next_previous);
    output = Value(&previous);
  }
  Jump(body, header);
  if (mode == LoopCase::MergeEscape) {
    auto& escaped = merge->AppendNewInst(ValueOpcode::Phi); escaped.SetFlags(Type::U32);
    escaped.AddPhiOperand(test, i); output = Value(&escaped);
  }
  if (mode == LoopCase::ContinueBypass) {
    header->terminator.true_block = body;
    header->AddBranch(body);
  }
  if (mode == LoopCase::DppInHeader) {
    IREmitter ir(header);
    auto dpp = ir.Emit(ValueOpcode::DppMoveU32, {i, Value(true)}, DppMoveFlags{});
    ir.Emit(ValueOpcode::ReferenceU32, {dpp});
  }
  if (mode == LoopCase::NoExit) {
    test->terminator.kind = CFG::TerminatorKind::Branch;
    test->terminator.true_block = body;
    test->condition = {};
  }
  if (mode == LoopCase::ScalarExit) {
    vote.Instruction()->SetFlags(CFG::BranchCondition::ScalarInstruction);
    vote.Instruction()->SetArg(0, Value(true));
  }
  Export(program, merge, output);
  return program;
}
// A tentative outer invariant may initially make two nested entry values
// equal. Removing that prerequisite must also invalidate the nested invariant.
void TestInvariantEntries() {
  auto program = Base();
  auto* entry = NewBlock(program);
  auto* outer = NewBlock(program);
  auto* choose = NewBlock(program);
  auto* left = NewBlock(program);
  auto* right = NewBlock(program);
  auto* inner = NewBlock(program);
  auto* inner_test = NewBlock(program);
  auto* inner_back = NewBlock(program);
  auto* inner_done = NewBlock(program);
  auto* outer_back = NewBlock(program);
  auto* done = NewBlock(program);
  done->terminator.kind = CFG::TerminatorKind::Return;
  Jump(entry, outer);
  Branch(outer, Value(true), choose, done, done);
  outer->terminator.loop_header = true;
  outer->terminator.continue_block = outer_back;
  Branch(choose, Value(true), left, right, inner);
  Jump(left, inner); Jump(right, inner);
  Jump(inner, inner_test);
  inner->terminator.loop_header = true;
  inner->terminator.merge_block = inner_done;
  inner->terminator.continue_block = inner_back;
  Branch(inner_test, Value(true), inner_back, inner_done, nullptr);
  Jump(inner_back, inner); Jump(inner_done, outer_back); Jump(outer_back, outer);
  auto& a = outer->AppendNewInst(ValueOpcode::Phi); a.SetFlags(Type::U32);
  a.AddPhiOperand(entry, Value(0u));
  IREmitter out(outer_back);
  a.AddPhiOperand(outer_back, out.Emit(ValueOpcode::IAdd32, {Value(&a), Value(1u)}));
  auto& b = inner->AppendNewInst(ValueOpcode::Phi); b.SetFlags(Type::U32);
  b.AddPhiOperand(left, Value(0u)); b.AddPhiOperand(right, Value(&a));
  b.AddPhiOperand(inner_back, Value(&b));
  RasterQuadDetail::Analysis analysis(program);
  Check(!analysis.Control().admitted, "unbounded inference fixture unexpectedly became executable");
  RasterQuadDetail::Blocks region(program.blocks.begin(), program.blocks.end());
  RasterQuadDetail::Cofactor proof(program);
  proof.OpaquePhiOutside(region);
  analysis.Invariants(proof, region);
  Check(!proof.Equal(Value(&a), Value(0u)), "incrementing outer state became an invariant");
  Check(!proof.Equal(Value(&b), Value(0u)),
        "nested invariant kept a stale entry equality after its prerequisite was removed");
}
} // namespace RasterQuadPartitionTests

void TestRasterQuadPartition() {
  using namespace RasterQuadPartitionTests;
  TestInvariantEntries();
  uint32_t cases = 1;
  for (uint32_t wave : {32u, 64u}) {
    for (const auto mode : {RegionCase::Valid, RegionCase::DppValid}) {
      auto program = Region(mode); program.wave_size = wave;
      const auto result = IR::AnalyzeRasterQuadPartition(program);
      if (!result.admitted) std::fprintf(stderr, "RasterQuad positive region %u: %s block%u inst%u\n",
                                         unsigned(mode), result.reason, result.block_id, result.instruction);
      Check(result.admitted, "closed masked raster region was not certified"); ++cases;
    }
    auto loop = LoopProgram(LoopCase::Valid); loop.wave_size = wave;
    const auto result = IR::AnalyzeRasterQuadPartition(loop);
    if (!result.admitted) std::fprintf(stderr, "RasterQuad positive loop: %s block%u inst%u\n",
                                       result.reason, result.block_id, result.instruction);
    Check(result.admitted, "bounded masked raster loop was not certified"); ++cases;
  }
  for (const auto mode : {RegionCase::UnmaskedValue, RegionCase::VoteEscape, RegionCase::ScalarVarying,
       RegionCase::StrayVote, RegionCase::HiddenCycle, RegionCase::MissingMerge, RegionCase::WrongStage,
       RegionCase::WrongWave, RegionCase::Untracked, RegionCase::DppMasked, RegionCase::DppNonbroadcast,
       RegionCase::DppAfterVote, RegionCase::Ballot, RegionCase::ReadFirst, RegionCase::AtomicAlias,
       RegionCase::Barrier, RegionCase::DirectConditionEscape, RegionCase::MultipleReturn}) {
    auto program = Region(mode);
    Check(!IR::AnalyzeRasterQuadPartition(program).admitted, "unsafe raster region was certified"); ++cases;
  }
  for (const auto mode : {LoopCase::ScalarEscape, LoopCase::MergeEscape, LoopCase::CurrentState,
       LoopCase::FutureActivation, LoopCase::NoProgress, LoopCase::ReversedExit, LoopCase::ContinueBypass,
       LoopCase::MissingMetadata, LoopCase::DppInHeader, LoopCase::NoExit, LoopCase::ScalarExit}) {
    auto program = LoopProgram(mode);
    const auto result = IR::AnalyzeRasterQuadPartition(program);
    if (result.admitted) std::fprintf(stderr, "RasterQuad unexpected loop admission %u\n", unsigned(mode));
    Check(!result.admitted, "unsafe raster loop was certified");
    if (mode == LoopCase::FutureActivation)
      Check(std::strcmp(result.reason, "loop_mask_can_reactivate") == 0,
            "future activation fixture failed for an unrelated data escape");
    if (mode == LoopCase::ScalarExit)
      Check(std::strcmp(result.reason, "loop_without_progress_certificate") == 0,
            "unproved uniform loop fixture did not exercise certificate coverage");
    ++cases;
  }
  std::printf("RasterQuadPartition CPU: %u cases passed\n", cases);
}
