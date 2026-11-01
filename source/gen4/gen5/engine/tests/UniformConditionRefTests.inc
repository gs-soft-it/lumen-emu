// SPDX-License-Identifier: GPL-2.0-only
namespace UniformConditionRefTests {
using namespace ShaderRecompiler;

std::vector<uint32_t> ShaderCode(bool nonzero) {
  return {EncodeSmem0(0x08, 12, 4), 125u << 25u,
          EncodeVop1(0x01, 0, 12), EncodeVopc(0x0d, 128, 0),
          EncodeSopp(nonzero ? 0x07 : 0x06, 2),
          EncodeVop1(0x01, 1, 242), EncodeSopp(0x02, 1),
          EncodeVop1(0x01, 1, 128),
          EncodeExp0(0, 0xf), EncodeExp1(1, 1, 1, 1), EncodeSopp(0x01)};
}

std::vector<uint32_t> ScalarizedShaderCode(uint32_t components, bool nonzero) {
  const auto opcode = 0x08u + std::countr_zero(components);
  return {EncodeSmem0(opcode, 12, 4), (125u << 25u) | 16u,
          EncodeVop1(0x01, 0, 12), EncodeVopc(0x0d, 128, 0),
          EncodeSop1(0x04, 4, 106), // Preserve the first comparison's VCC mask.
          EncodeVop1(0x01, 1, 12u + components - 1u), EncodeVopc(0x0d, 128, 1),
          EncodeSop2(0x0f, 106, 4, 106), // VCC = first != 0 AND last != 0.
          EncodeSopp(nonzero ? 0x07 : 0x06, 2),
          EncodeVop1(0x01, 2, 242), EncodeSopp(0x02, 1),
          EncodeVop1(0x01, 2, 128),
          EncodeExp0(0, 0xf), EncodeExp1(2, 2, 2, 2), EncodeSopp(0x01)};
}

void TestScalarizedUniformLoads() {
  using namespace IR;
  std::array<uint32_t, 12> userdata{};
  userdata[8] = 0x2000u;
  userdata[10] = 80u;
  for (uint32_t wave : {32u, 64u}) {
    auto options = MakeCompileOptions(ShaderType::Pixel);
    options.user_data = userdata;
    options.wave_size = wave;
    for (uint32_t components : {2u, 4u, 8u, 16u}) {
      for (bool nonzero : {false, true}) {
        const auto result = RecompileForTest(ScalarizedShaderCode(components, nonzero), options);
        CheckSpirvBinaryValidates(result.spirv);
        size_t reads = 0;
        bool first = false;
        bool last = false;
        for (const auto* block : result.program.blocks) {
          for (const auto& inst : *block) {
            if (inst.GetOpcode() != ValueOpcode::ReadConstBuffer) continue;
            const auto& memory = result.program.memory_info.at(inst.Flags<MemoryFlags>().index);
            Check(memory.kind == ResourceKind::ScalarBuffer && memory.data_bits == 32u &&
                      memory.data_dwords == 1u && memory.component_count == components,
                  "SMEM scalarized load lost its original width or single-dword operation");
            first |= memory.component_index == 0u && memory.offset == 16u;
            last |= memory.component_index == components - 1u &&
                    memory.offset == 16u + (components - 1u) * 4u;
            ++reads;
          }
        }
        Check(reads == 2u && first && last,
              "scalarized first/last-word fixture did not preserve both runtime reads");
        if (result.program.info.subgroup_vote)
          std::fprintf(stderr, "ScalarizedUniformLoad wave=%u components=%u branch=%s retained vote\n",
                       wave, components, nonzero ? "NZ" : "Z");
        Check(!result.program.info.subgroup_vote &&
                  !SpirvContainsCapability(result.spirv, spv::CapabilityGroupNonUniformVote) &&
                  SpirvInstructionOpcodeCount(result.spirv, spv::OpGroupNonUniformAll) == 0u &&
                  SpirvInstructionOpcodeCount(result.spirv, spv::OpGroupNonUniformAny) == 0u,
              "single-word reads from scalarized SMEM retained a redundant uniform vote");
        Check(SpirvInstructionOpcodeCount(result.spirv, spv::OpFUnordNotEqual) == 2u &&
                  SpirvInstructionOpcodeCount(result.spirv, spv::OpLogicalAnd) != 0u,
              "scalarized vote fixture lost an independent comparison or their conjunction");
      }
    }
  }
}

enum class Mutation {
  None, PushData, VaryingData, VaryingAddress, ReadFirstLane, Phi,
  Coherent, PlanningOnly, IndirectMemory, ImageAlias, IndirectRoot,
  IndirectArray, WrittenAlias, AtomicAlias, ImageWrittenAlias,
  WriteInstruction, AtomicInstruction, Barrier, Completion, WrongHandle,
  UnknownValue, WrongMemoryWidth, Untracked, PixelMask, DeepExpression
};

IR::Program Program(Mutation mutation, CFG::BranchCondition kind) {
  using namespace IR;
  IR::Program program;
  program.stage = ShaderType::Pixel;
  program.wave_size = 64;
  program.resource_tracking_complete = true;
  program.block_storage.push_back(std::make_unique<Block>());
  auto* block = program.block_storage.back().get();
  program.blocks.push_back(block);
  block->terminator.kind = CFG::TerminatorKind::Return;
  program.info.buffers.push_back(BufferResource{.read = true, .scalar = true});
  program.memory_info.push_back(MemoryInfo{.kind = ResourceKind::ScalarBuffer});
  IREmitter ir(block);
  const auto handle = ir.Emit(ValueOpcode::GetBufferResource,
                             {Value(0u), Value(0u), Value(0u), Value(0u)});
  const auto lane = ir.Emit(ValueOpcode::LaneId, {});
  const auto address = mutation == Mutation::VaryingAddress ? lane : Value(0u);
  auto source = ir.Emit(ValueOpcode::ReadConstBuffer, {handle, address}, MemoryFlags{});
  auto& memory = program.memory_info.front();
  auto& buffer = program.info.buffers.front();
  switch (mutation) {
    case Mutation::PushData:
      source = ir.Emit(ValueOpcode::GetUserData, {Value(ScalarReg{0})}); break;
    case Mutation::VaryingData:
      source = ir.Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)}); break;
    case Mutation::ReadFirstLane:
      source = ir.Emit(ValueOpcode::ReadFirstLane, {source, Value(true)}); break;
    case Mutation::Phi: {
      auto& phi = block->AppendNewInst(ValueOpcode::Phi);
      phi.SetFlags(Type::U32);
      phi.AddPhiOperand(block, source);
      phi.AddPhiOperand(block, Value(0u));
      source = Value(&phi);
      break;
    }
    case Mutation::Coherent: memory.coherent = true; break;
    case Mutation::PlanningOnly: memory.planning_only = true; break;
    case Mutation::IndirectMemory: memory.kind = ResourceKind::IndirectBuffer; break;
    case Mutation::ImageAlias: buffer.image_alias = 0; break;
    case Mutation::IndirectRoot: buffer.indirect_root = 0; break;
    case Mutation::IndirectArray: buffer.indirect_resources.push_back(0); break;
    case Mutation::WrittenAlias:
      program.info.buffers.push_back(BufferResource{.written = true}); break;
    case Mutation::AtomicAlias:
      program.info.buffers.push_back(BufferResource{.atomic = true}); break;
    case Mutation::ImageWrittenAlias:
      program.info.images.push_back(ImageResource{.written = true}); break;
    case Mutation::WriteInstruction:
    case Mutation::AtomicInstruction: {
      program.info.buffers.push_back(BufferResource{});
      program.memory_info.push_back(MemoryInfo{.kind = ResourceKind::Buffer, .resource = 1});
      const auto other = ir.Emit(ValueOpcode::GetBufferResource,
                                {Value(0u), Value(0u), Value(0u), Value(0u)}, uint32_t{1});
      ir.Emit(mutation == Mutation::WriteInstruction ? ValueOpcode::StoreBufferU32
                                                    : ValueOpcode::BufferAtomicIAdd32,
              {other, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)},
              MemoryFlags{.index = 1});
      break;
    }
    case Mutation::Barrier: ir.Emit(ValueOpcode::Barrier, {}); break;
    case Mutation::Completion: ir.Emit(ValueOpcode::StoreCompletion, {}); break;
    case Mutation::WrongHandle: handle.Instruction()->SetFlags(uint32_t{1}); break;
    case Mutation::UnknownValue: source = ir.Emit(ValueOpcode::UndefU32, {}); break;
    case Mutation::WrongMemoryWidth: memory.data_bits = 16; break;
    case Mutation::Untracked: program.resource_tracking_complete = false; break;
    case Mutation::PixelMask: {
      program.export_info.push_back(ExportInfo{.kind = ExportTargetKind::Null, .vm = true});
      const auto data = ir.Emit(ValueOpcode::CompositeConstructU32x4,
                               {Value(0u), Value(0u), Value(0u), Value(0u)});
      const auto enabled = ir.Emit(ValueOpcode::IEqual32, {lane, Value(0u)});
      ir.Emit(ValueOpcode::SetAttribute, {data, enabled}, ExportFlags{});
      break;
    }
    case Mutation::DeepExpression:
      for (uint32_t i = 0; i < 129u; ++i)
        source = ir.Emit(ValueOpcode::IAdd32, {source, Value(1u)});
      break;
    default: break;
  }
  const auto value = ir.Emit(ValueOpcode::BitCastF32U32, {source});
  auto predicate = ir.Emit(ValueOpcode::FPUnordNotEqual32, {Value::F32(0.f), value});
  if (kind == CFG::BranchCondition::VccZero || kind == CFG::BranchCondition::SccZero ||
      kind == CFG::BranchCondition::ExecZero)
    predicate = ir.Emit(ValueOpcode::LogicalNot, {predicate});
  const auto condition = ir.Emit(ValueOpcode::ConditionRef, {predicate}, kind);
  ir.Emit(ValueOpcode::Reference, {condition});
  return program;
}

void TestScalarizedLoadMetadata() {
  using namespace IR;
  const auto check = [](uint32_t count, uint32_t component, uint32_t dwords,
                        uint32_t bits, Mutation mutation, bool accepted) {
    auto program = Program(mutation, CFG::BranchCondition::VccZero);
    auto& memory = program.memory_info.front();
    memory.component_count = count;
    memory.component_index = component;
    memory.data_dwords = dwords;
    memory.data_bits = bits;
    Check(FoldUniformConditionRefs(program) == (accepted ? 1u : 0u),
          "scalarized scalar-buffer provenance accepted an invalid shape or lost a valid lane");
  };
  for (uint32_t count : {1u, 2u, 4u, 8u, 16u}) {
    check(count, 0u, 1u, 32u, Mutation::None, true);
    if (count > 1u) check(count, count - 1u, 1u, 32u, Mutation::None, true);
    check(count, count, 1u, 32u, Mutation::None, false);
    check(count, UINT32_MAX, 1u, 32u, Mutation::None, false);
  }
  for (uint32_t count : {0u, 3u, 17u, 32u})
    check(count, 0u, 1u, 32u, Mutation::None, false);
  check(4u, 3u, 2u, 32u, Mutation::None, false);
  check(4u, 3u, 1u, 16u, Mutation::None, false);
  for (const auto mutation : {Mutation::Coherent, Mutation::PlanningOnly, Mutation::VaryingAddress,
                              Mutation::IndirectMemory, Mutation::ImageAlias, Mutation::WrittenAlias})
    check(4u, 3u, 1u, 32u, mutation, false);
}

IR::Block* AddBlock(IR::Program& program) {
  program.block_storage.push_back(std::make_unique<IR::Block>());
  auto* block = program.block_storage.back().get();
  program.blocks.push_back(block);
  block->id = static_cast<uint32_t>(program.blocks.size() - 1u);
  return block;
}

void Branch(IR::Block& from, IR::Block* to) {
  from.terminator.kind = CFG::TerminatorKind::Branch;
  from.terminator.true_block = to;
  if (to) from.AddBranch(to);
}

// Keep the sampled data observable independently of the uniform predicate.
// Splice the whole dependency chain to preserve SSA instruction order.
IR::Inst* Sample(IR::Program& program, IR::Block& block,
                 IR::Block::iterator before, bool varying_exec = false) {
  using namespace IR;
  const auto previous = block.empty() ? block.end() : std::prev(block.end());
  IREmitter ir(&block);
  const auto image = ir.Emit(ValueOpcode::GetImageResource,
      {Value(0u), Value(0u), Value(0u), Value(0u),
       Value(0u), Value(0u), Value(0u), Value(0u)});
  const auto sampler = ir.Emit(ValueOpcode::GetSamplerResource,
      {Value(0u), Value(0u), Value(0u), Value(0u)});
  const auto address = ir.Emit(ValueOpcode::MakeImageAddress,
      {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
       Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u)});
  auto exec = Value(true);
  if (varying_exec) {
    const auto lane = ir.Emit(ValueOpcode::LaneId, {});
    exec = ir.Emit(ValueOpcode::IEqual32, {lane, Value(0u)});
  }
  const auto index = static_cast<uint32_t>(program.memory_info.size());
  program.memory_info.push_back(MemoryInfo{.kind = ResourceKind::Image});
  if (program.info.images.empty()) program.info.images.push_back(ImageResource{.read = true});
  const auto sampled = ir.Emit(ValueOpcode::ImageSampleRaw,
      {image, sampler, address, exec}, MemoryFlags{.index = index});
  const auto component = ir.Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)});
  ir.Emit(ValueOpcode::ReferenceU32, {component});
  const auto first = previous == block.end() ? block.begin() : std::next(previous);
  if (before != block.end()) block.Instructions().splice(before, block.Instructions(), first, block.end());
  return sampled.Instruction();
}

enum class SampleMutation {
  Prefix, AfterVote, OtherBlock, SelfLoop, BackEdge, ConditionalBackEdge,
  IndirectTarget, UnsupportedTarget, MissingTarget, ExternalTarget,
  Dispatcher, VaryingExec, NonPixel, SamplePredicate, CoherentImage,
  TwoVotes, OtherCycle, ReadFirstLane, Ballot, LaneId, VaryingVote,
  ImplicitSubgroupBuffer
};

void TestSampleOrdering() {
  using namespace IR;
  for (const auto mutation : {
           SampleMutation::Prefix, SampleMutation::AfterVote, SampleMutation::OtherBlock,
           SampleMutation::SelfLoop, SampleMutation::BackEdge, SampleMutation::ConditionalBackEdge,
           SampleMutation::IndirectTarget, SampleMutation::UnsupportedTarget,
           SampleMutation::MissingTarget, SampleMutation::ExternalTarget,
           SampleMutation::Dispatcher, SampleMutation::VaryingExec, SampleMutation::NonPixel,
           SampleMutation::SamplePredicate, SampleMutation::CoherentImage,
           SampleMutation::TwoVotes, SampleMutation::OtherCycle,
           SampleMutation::ReadFirstLane, SampleMutation::Ballot,
           SampleMutation::LaneId, SampleMutation::VaryingVote,
           SampleMutation::ImplicitSubgroupBuffer}) {
    auto program = Program(Mutation::None, CFG::BranchCondition::VccZero);
    // The production pass follows DCE; remove the builder's unused LaneId.
    EliminateDeadCode(program.blocks);
    auto* owner = program.blocks.front();
    auto vote = std::find_if(owner->begin(), owner->end(), [](const auto& inst) {
      return inst.GetOpcode() == ValueOpcode::ConditionRef;
    });
    Check(vote != owner->end(), "sample-order fixture lacks its vote");
    auto* sample_block = mutation == SampleMutation::OtherBlock ? AddBlock(program) : owner;
    if (sample_block != owner) Branch(*owner, sample_block);
    const auto before = sample_block != owner || mutation == SampleMutation::AfterVote ||
                                mutation == SampleMutation::TwoVotes
                            ? sample_block->end() : vote;
    auto* sample = Sample(program, *sample_block, before,
                          mutation == SampleMutation::VaryingExec);
    switch (mutation) {
      case SampleMutation::SelfLoop: Branch(*owner, owner); break;
      case SampleMutation::BackEdge: {
        auto* later = AddBlock(program);
        Branch(*owner, later);
        Branch(*later, owner);
        break;
      }
      case SampleMutation::ConditionalBackEdge: {
        auto* later = AddBlock(program);
        auto* done = AddBlock(program);
        Branch(*owner, later);
        later->terminator.kind = CFG::TerminatorKind::ConditionalBranch;
        later->terminator.true_block = done;
        later->terminator.false_block = owner;
        later->condition = Value(true);
        later->AddBranch(done);
        later->AddBranch(owner);
        break;
      }
      case SampleMutation::IndirectTarget:
        owner->terminator.kind = CFG::TerminatorKind::IndirectBranch; break;
      case SampleMutation::UnsupportedTarget:
        owner->terminator.kind = CFG::TerminatorKind::Unsupported; break;
      case SampleMutation::MissingTarget: Branch(*owner, nullptr); break;
      case SampleMutation::ExternalTarget: {
        auto* external = AddBlock(program);
        program.blocks.pop_back();
        Branch(*owner, external);
        break;
      }
      case SampleMutation::Dispatcher: program.dispatcher_fallback = true; break;
      case SampleMutation::NonPixel: program.stage = ShaderType::Vertex; break;
      case SampleMutation::SamplePredicate: {
        const auto component = owner->PrependNewInst(vote, ValueOpcode::CompositeExtractU32x4,
                                                   {Value(sample), Value(0u)});
        const auto predicate = owner->PrependNewInst(vote, ValueOpcode::IEqual32,
                                                    {Value(&*component), Value(0u)});
        vote->SetArg(0, Value(&*predicate));
        break;
      }
      case SampleMutation::CoherentImage: program.memory_info.back().coherent = true; break;
      case SampleMutation::TwoVotes: {
        IREmitter ir(owner);
        const auto later = ir.Emit(ValueOpcode::ConditionRef, {vote->Arg(0)},
                                   CFG::BranchCondition::VccZero);
        ir.Emit(ValueOpcode::Reference, {later});
        break;
      }
      case SampleMutation::OtherCycle: {
        auto* later = AddBlock(program);
        Branch(*owner, later);
        Branch(*later, later);
        break;
      }
      case SampleMutation::ReadFirstLane:
      case SampleMutation::Ballot:
      case SampleMutation::LaneId:
      case SampleMutation::VaryingVote: {
        IREmitter ir(owner);
        if (mutation == SampleMutation::Ballot) {
          const auto ballot = ir.Emit(ValueOpcode::Ballot, {Value(true)});
          const auto bits = ir.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(0u)});
          ir.Emit(ValueOpcode::ReferenceU32, {bits});
        } else {
          const auto input = mutation == SampleMutation::LaneId
                                 ? ir.Emit(ValueOpcode::LaneId, {})
                                 : ir.Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)});
          if (mutation == SampleMutation::VaryingVote) {
            const auto pred = ir.Emit(ValueOpcode::IEqual32, {input, Value(0u)});
            const auto other = ir.Emit(ValueOpcode::ConditionRef, {pred},
                                       CFG::BranchCondition::VccNonZero);
            ir.Emit(ValueOpcode::Reference, {other});
          } else {
            const auto observed = mutation == SampleMutation::LaneId ? input :
                ir.Emit(ValueOpcode::ReadFirstLane, {input, Value(true)});
            ir.Emit(ValueOpcode::ReferenceU32, {observed});
          }
        }
        break;
      }
      case SampleMutation::ImplicitSubgroupBuffer: {
        IREmitter ir(owner);
        const auto index = static_cast<uint32_t>(program.memory_info.size());
        program.info.buffers.push_back(BufferResource{.packed_stride = 1u << 20u, .read = true});
        program.memory_info.push_back(MemoryInfo{.kind = ResourceKind::Buffer, .resource = 1});
        const auto handle = ir.Emit(ValueOpcode::GetBufferResource,
                                   {Value(0u), Value(0u), Value(0u), Value(0u)}, uint32_t{1});
        const auto loaded = ir.Emit(ValueOpcode::LoadBufferU32,
            {handle, Value(0u), Value(0u), Value(0u), Value(true)}, MemoryFlags{.index = index});
        ir.Emit(ValueOpcode::ReferenceU32, {loaded});
        break;
      }
      default: break;
    }
    const bool accepted = mutation == SampleMutation::Prefix ||
                          mutation == SampleMutation::TwoVotes ||
                          mutation == SampleMutation::OtherCycle;
    Check(FoldUniformConditionRefs(program) == (accepted ? 1u : 0u),
          "sample/vote order or CFG re-entry proof admitted an unsafe fold or rejected a safe prefix");
    if (accepted) {
      size_t samples = 0;
      size_t votes = 0;
      for (const auto* block : program.blocks) {
        for (const auto& inst : *block) {
          samples += inst.GetOpcode() == ValueOpcode::ImageSampleRaw;
          votes += inst.GetOpcode() == ValueOpcode::ConditionRef;
        }
      }
      Check(samples == 1u && votes == (mutation == SampleMutation::TwoVotes ? 1u : 0u),
            "sample-prefix fold lost the sample or folded the vote preceding it");
    }
  }
}

enum class ControlMutation {
  UniformDiamond, ArrayImage, DataPhi, VaryingCondition, ControlPhi,
  LoopFlag, ContinueMetadata, MissingMerge, ExternalMerge, BackEdge,
  Dispatcher, IndirectTarget, DetachedBlock, VaryingSampleExec,
  IndirectImage, IndirectSampler, MissingSampler, WrongImageHandle,
  WrongSamplerHandle, DynamicMip, ConvertedImage, CubeImage, ExplicitBounds,
  VaryingScalarAddress, OtherMemoryRead, VaryingExport, IndependentLane,
  Bvh, SrtRead, BufferResourceIndex, UnknownConditionKind
};

void TestUniformSampleControl() {
  using namespace IR;
  for (const auto mutation : {
           ControlMutation::UniformDiamond, ControlMutation::ArrayImage, ControlMutation::DataPhi,
           ControlMutation::VaryingCondition, ControlMutation::ControlPhi,
           ControlMutation::LoopFlag, ControlMutation::ContinueMetadata,
           ControlMutation::MissingMerge, ControlMutation::ExternalMerge, ControlMutation::BackEdge,
           ControlMutation::Dispatcher, ControlMutation::IndirectTarget, ControlMutation::DetachedBlock,
           ControlMutation::VaryingSampleExec, ControlMutation::IndirectImage,
           ControlMutation::IndirectSampler, ControlMutation::MissingSampler,
           ControlMutation::WrongImageHandle, ControlMutation::WrongSamplerHandle,
           ControlMutation::DynamicMip, ControlMutation::ConvertedImage,
           ControlMutation::CubeImage, ControlMutation::ExplicitBounds,
           ControlMutation::VaryingScalarAddress, ControlMutation::OtherMemoryRead,
           ControlMutation::VaryingExport, ControlMutation::IndependentLane,
           ControlMutation::Bvh, ControlMutation::SrtRead, ControlMutation::BufferResourceIndex,
           ControlMutation::UnknownConditionKind}) {
    auto program = Program(Mutation::None, CFG::BranchCondition::VccZero);
    EliminateDeadCode(program.blocks);
    auto* entry = program.blocks.front();
    auto* left = AddBlock(program);
    auto* right = AddBlock(program);
    auto* merge = AddBlock(program);
    auto vote = std::find_if(entry->begin(), entry->end(), [](const auto& inst) {
      return inst.GetOpcode() == ValueOpcode::ConditionRef;
    });
    IREmitter start(entry);
    entry->condition = start.Emit(ValueOpcode::LogicalNot, {Value(&*vote)});
    start.Emit(ValueOpcode::Reference, {entry->condition});
    entry->terminator.kind = CFG::TerminatorKind::ConditionalBranch;
    entry->terminator.true_block = left;
    entry->terminator.false_block = right;
    entry->terminator.merge_block = merge;
    entry->AddBranch(left);
    entry->AddBranch(right);
    Branch(*left, merge);
    Branch(*right, merge);
    auto* sample = Sample(program, *left, left->end());
    Sample(program, *right, right->end());
    const auto dimension = mutation == ControlMutation::ArrayImage
                               ? Decoder::ImageDimension::Dim2DArray : Decoder::ImageDimension::Dim2D;
    program.info.images.front().dimension = dimension;
    program.info.samplers.push_back(SamplerResource{});
    for (auto& memory : program.memory_info)
      if (memory.kind == ResourceKind::Image) memory.image_dimension = dimension;
    IREmitter tail(merge);
    const auto attribute = [&]() {
      return tail.Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)});
    };
    switch (mutation) {
      case ControlMutation::ArrayImage: {
        auto* address = sample->Arg(2).Instruction();
        auto at = std::find_if(left->begin(), left->end(), [&](const auto& inst) {
          return &inst == address;
        });
        const auto layer = left->PrependNewInst(at, ValueOpcode::GetAttribute,
                                               {Value(0u), Value(0u)});
        address->SetArg(2, Value(&*layer)); // Array layer is data, not a branch.
        break;
      }
      case ControlMutation::DataPhi:
      case ControlMutation::ControlPhi: {
        auto& phi = merge->AppendNewInst(ValueOpcode::Phi);
        phi.SetFlags(Type::U32);
        phi.AddPhiOperand(left, Value(0x3f800000u));
        phi.AddPhiOperand(right, Value(0x40000000u));
        if (mutation == ControlMutation::DataPhi) {
          auto* after = Sample(program, *merge, merge->end());
          after->Arg(2).Instruction()->SetArg(0, Value(&phi));
          program.memory_info.back().image_dimension = dimension;
        } else {
          auto* yes = AddBlock(program);
          auto* no = AddBlock(program);
          auto* done = AddBlock(program);
          merge->condition = tail.Emit(ValueOpcode::IEqual32, {Value(&phi), Value(0u)});
          tail.Emit(ValueOpcode::Reference, {merge->condition});
          merge->terminator.kind = CFG::TerminatorKind::ConditionalBranch;
          merge->terminator.true_block = yes;
          merge->terminator.false_block = no;
          merge->terminator.merge_block = done;
          merge->AddBranch(yes);
          merge->AddBranch(no);
          Branch(*yes, done);
          Branch(*no, done);
        }
        break;
      }
      case ControlMutation::VaryingCondition: {
        const auto input = start.Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)});
        entry->condition = start.Emit(ValueOpcode::IEqual32, {input, Value(0u)});
        start.Emit(ValueOpcode::Reference, {entry->condition});
        break;
      }
      case ControlMutation::LoopFlag: entry->terminator.loop_header = true; break;
      case ControlMutation::ContinueMetadata: entry->terminator.continue_block = merge; break;
      case ControlMutation::MissingMerge: entry->terminator.merge_block = nullptr; break;
      case ControlMutation::ExternalMerge: {
        auto* external = AddBlock(program);
        program.blocks.pop_back();
        entry->terminator.merge_block = external;
        break;
      }
      case ControlMutation::BackEdge: Branch(*merge, entry); break;
      case ControlMutation::Dispatcher: program.dispatcher_fallback = true; break;
      case ControlMutation::IndirectTarget:
        merge->terminator.kind = CFG::TerminatorKind::IndirectBranch; break;
      case ControlMutation::DetachedBlock: AddBlock(program); break;
      case ControlMutation::VaryingSampleExec: {
        auto at = std::find_if(left->begin(), left->end(), [&](const auto& inst) {
          return &inst == sample;
        });
        const auto value = left->PrependNewInst(at, ValueOpcode::GetAttribute,
                                               {Value(0u), Value(0u)});
        const auto enabled = left->PrependNewInst(at, ValueOpcode::IEqual32,
                                                 {Value(&*value), Value(0u)});
        sample->SetArg(3, Value(&*enabled));
        break;
      }
      case ControlMutation::IndirectImage: program.info.images.front().indirect_root = 0; break;
      case ControlMutation::IndirectSampler: program.info.samplers.front().indirect_root = 0; break;
      case ControlMutation::MissingSampler: program.info.samplers.clear(); break;
      case ControlMutation::WrongImageHandle: sample->Arg(0).Instruction()->SetFlags(uint32_t{1}); break;
      case ControlMutation::WrongSamplerHandle: sample->Arg(1).Instruction()->SetFlags(uint32_t{1}); break;
      case ControlMutation::DynamicMip: program.info.images.front().mip_mode = ImageMipMode::Dynamic; break;
      case ControlMutation::ConvertedImage:
        program.info.images.front().conversion_format = Prospero::BufferFormat::k11_11_10UNorm; break;
      case ControlMutation::CubeImage: program.info.images.front().cube = true; break;
      case ControlMutation::ExplicitBounds: program.info.images.front().explicit_image_bounds = true; break;
      case ControlMutation::VaryingScalarAddress:
      case ControlMutation::OtherMemoryRead: {
        const auto handle = tail.Emit(ValueOpcode::GetBufferResource,
                                     {Value(0u), Value(0u), Value(0u), Value(0u)});
        Value read;
        if (mutation == ControlMutation::VaryingScalarAddress) {
          read = tail.Emit(ValueOpcode::ReadConstBuffer, {handle, attribute()}, MemoryFlags{});
        } else {
          const auto index = static_cast<uint32_t>(program.memory_info.size());
          program.memory_info.push_back(MemoryInfo{.kind = ResourceKind::Buffer});
          read = tail.Emit(ValueOpcode::LoadBufferU32,
              {handle, Value(0u), Value(0u), Value(0u), Value(true)}, MemoryFlags{.index = index});
        }
        tail.Emit(ValueOpcode::ReferenceU32, {read});
        break;
      }
      case ControlMutation::VaryingExport: {
        const auto enabled = tail.Emit(ValueOpcode::IEqual32, {attribute(), Value(0u)});
        const auto data = tail.Emit(ValueOpcode::CompositeConstructU32x4,
                                    {Value(0u), Value(0u), Value(0u), Value(0u)});
        program.export_info.push_back(ExportInfo{.kind = ExportTargetKind::Mrt, .en = 0xf});
        tail.Emit(ValueOpcode::SetAttribute, {data, enabled}, ExportFlags{});
        break;
      }
      case ControlMutation::IndependentLane:
        tail.Emit(ValueOpcode::ReferenceU32, {tail.Emit(ValueOpcode::LaneId, {})}); break;
      case ControlMutation::Bvh: {
        IREmitter body(left);
        const auto data = body.Emit(ValueOpcode::CompositeConstructU32x4,
                                    {Value(0u), Value(0u), Value(0u), Value(0u)});
        const auto hit = body.Emit(ValueOpcode::BvhIntersect, {data, sample->Arg(2), Value(true)});
        body.Emit(ValueOpcode::ReferenceU32,
                  {body.Emit(ValueOpcode::CompositeExtractU32x4, {hit, Value(0u)})});
        break;
      }
      case ControlMutation::SrtRead: {
        const auto resource = tail.Emit(ValueOpcode::GetSrtResource, {});
        tail.Emit(ValueOpcode::ReferenceU32,
                  {tail.Emit(ValueOpcode::ReadConst, {resource, Value(0u)})});
        break;
      }
      case ControlMutation::BufferResourceIndex:
        tail.Emit(ValueOpcode::ReferenceU32,
                  {tail.Emit(ValueOpcode::GetBufferResourceIndex, {attribute()})}); break;
      case ControlMutation::UnknownConditionKind:
        vote->SetFlags(CFG::BranchCondition::Unknown); break;
      default: break;
    }
    const bool accepted = mutation == ControlMutation::UniformDiamond ||
                          mutation == ControlMutation::ArrayImage ||
                          mutation == ControlMutation::DataPhi;
    const auto count = FoldUniformConditionRefs(program);
    if (count != (accepted ? 1u : 0u))
      std::fprintf(stderr, "UniformSampleControl mutation=%u changed=%u\n",
                   static_cast<unsigned>(mutation), count);
    Check(count == (accepted ? 1u : 0u), "uniform derivative control proof accepted an unproved path");
    if (accepted) {
      size_t samples = 0;
      size_t votes = 0;
      for (const auto* block : program.blocks)
        for (const auto& inst : *block) {
          samples += inst.GetOpcode() == ValueOpcode::ImageSampleRaw;
          votes += inst.GetOpcode() == ValueOpcode::ConditionRef;
        }
      Check(votes == 0u && samples == (mutation == ControlMutation::DataPhi ? 3u : 2u),
            "uniform control fold removed an observable sample or retained its vote");
    }
  }
}
} // namespace UniformConditionRefTests

void TestUniformConditionRefs() {
  using namespace UniformConditionRefTests;
  using namespace ShaderRecompiler;
  TestScalarizedUniformLoads();
  TestScalarizedLoadMetadata();
  TestSampleOrdering();
  TestUniformSampleControl();
  std::array<uint32_t, 12> userdata{};
  userdata[8] = 0x2000u;
  userdata[10] = 4u;
  for (uint32_t wave : {32u, 64u}) {
    auto options = MakeCompileOptions(ShaderType::Pixel);
    options.user_data = userdata;
    options.wave_size = wave;
    for (bool nonzero : {false, true}) {
      const auto result = RecompileForTest(ShaderCode(nonzero), options);
      CheckSpirvBinaryValidates(result.spirv);
      Check(!result.program.info.subgroup_vote &&
                !SpirvContainsCapability(result.spirv, spv::CapabilityGroupNonUniformVote) &&
                SpirvInstructionOpcodeCount(result.spirv, spv::OpGroupNonUniformAll) == 0 &&
                SpirvInstructionOpcodeCount(result.spirv, spv::OpGroupNonUniformAny) == 0,
            "uniform scalar-buffer VCC branch retained a physical subgroup vote");
      Check(result.program.info.buffers.size() == 1u && result.program.info.buffers[0].live &&
                SpirvInstructionOpcodeCount(result.spirv, spv::OpFUnordNotEqual) == 1u,
            "uniform-vote fold removed the runtime buffer read or float comparison");
    }
  }
  for (const auto kind : {CFG::BranchCondition::VccZero, CFG::BranchCondition::VccNonZero,
                          CFG::BranchCondition::SccZero, CFG::BranchCondition::SccNonZero,
                          CFG::BranchCondition::ExecZero, CFG::BranchCondition::ExecNonZero}) {
    for (const auto mutation : {Mutation::None, Mutation::PushData}) {
      auto program = Program(mutation, kind);
      const auto count = IR::FoldUniformConditionRefs(program);
      Check(count == 1u, "known uniform vote was not folded");
      const auto& reference = program.blocks.front()->Instructions().back();
      const auto expected = kind == CFG::BranchCondition::VccZero ||
                                    kind == CFG::BranchCondition::SccZero ||
                                    kind == CFG::BranchCondition::ExecZero
                                ? IR::ValueOpcode::LogicalNot : IR::ValueOpcode::FPUnordNotEqual32;
      Check(reference.GetOpcode() == IR::ValueOpcode::Reference &&
                reference.Arg(0).Resolve().Instruction()->GetOpcode() == expected,
            "uniform zero-kind vote changed its pre-inverted predicate");
    }
    for (const auto mutation : {
             Mutation::VaryingData, Mutation::VaryingAddress, Mutation::ReadFirstLane,
             Mutation::Phi, Mutation::Coherent, Mutation::PlanningOnly, Mutation::IndirectMemory,
             Mutation::ImageAlias, Mutation::IndirectRoot, Mutation::IndirectArray,
             Mutation::WrittenAlias, Mutation::AtomicAlias, Mutation::ImageWrittenAlias,
             Mutation::WriteInstruction, Mutation::AtomicInstruction, Mutation::Barrier,
             Mutation::Completion, Mutation::WrongHandle, Mutation::UnknownValue,
             Mutation::WrongMemoryWidth, Mutation::Untracked, Mutation::PixelMask,
             Mutation::DeepExpression}) {
      auto program = Program(mutation, kind);
      Check(IR::FoldUniformConditionRefs(program) == 0u,
            "unproved varying/mutable/aliased uniform-vote input was folded");
    }
  }
}
