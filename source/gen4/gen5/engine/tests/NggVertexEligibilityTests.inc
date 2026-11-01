// SPDX-License-Identifier: GPL-2.0-only
// Execute the translator/compiler pipeline on NGG ABI prologues, including escapes.
std::vector<uint32_t> NggEligibilityShader(uint32_t escape = 0, bool primitive = false) {
  std::vector<uint32_t> code;
  if (primitive) {
    code = {EncodeSop2(0x27, 2, 3, 255), 0x00080008u, // s_bfe_u32 s2,s3,8:8
            EncodeSop2(0x01, 4, 192, 2),             // s_sub_u32 s4,64,s2
            EncodeSop2(0x21, 126, 193, 4),           // s_lshr_b64 exec,-1,s4
            EncodeExp0(0x14, 1), EncodeExp1(0, 0, 0, 0)};
  }
  const std::array prefix{EncodeSop2(0x0e, 0, 3, 255), 0xffu,
                         EncodeSop2(0x01, 1, 192, 0)};
  code.insert(code.end(), prefix.begin(), prefix.end());
  if (escape == 3) code.push_back(EncodeSop2(0x00, 1, 1, 129)); // off-by-one prefix
  code.push_back(EncodeSop2(0x21, 126, 193, 1));
  if (escape == 1) code.push_back(EncodeVop1(0x01, 0, 0));     // count escapes to Position
  else if (escape == 2) code.push_back(EncodeVop1(0x01, 0, 126)); // raw EXEC word escapes
  else if (escape == 4) {
    code.push_back(EncodeVop1(0x01, 0, 128));
    code.push_back(EncodeVop2(0x23, 0, 193, 0)); // MBCNT exposes lane ordinal
  } else if (escape == 5) {
    code.push_back(EncodeVop1(0x02, 2, 5 + 256)); // READFIRSTLANE has a real subgroup consumer
    code.push_back(EncodeVop1(0x01, 0, 2));
  } else code.push_back(EncodeVop1(0x01, 0, 5 + 256));
  code.insert(code.end(), {EncodeExp0(0x0c, 0xf), EncodeExp1(0, 0, 0, 0), EncodeSopp(0x01)});
  return code;
}

void TestNggVertexEligibility() {
  using namespace ShaderRecompiler;
  const auto needs_subgroup = [](const auto& info) {
    return info.subgroup_ballot || info.subgroup_vote || info.subgroup_barrier ||
           info.subgroup_shuffle || info.subgroup_arithmetic || info.subgroup_local_invocation_id;
  };
  for (uint32_t wave : {32u, 64u}) {
    auto options = MakeCompileOptions(ShaderType::Vertex);
    options.wave_size = wave;
    for (bool primitive : {false, true}) {
      const auto result = RecompileForTest(NggEligibilityShader(0, primitive), options);
      CheckSpirvBinaryValidates(result.spirv);
      Check(!needs_subgroup(result.program.info) &&
                !SpirvContainsCapability(result.spirv, spv::CapabilityGroupNonUniform) &&
                SpirvInstructionOpcodeCount(result.spirv, spv::OpGroupNonUniformBallot) == 0,
            "closed NGG eligibility wrapper retained a physical subgroup dependency");
      Check(std::ranges::any_of(result.program.info.outputs, [](const auto& output) {
              return output.kind == IR::StageOutputKind::Position;
            }), "NGG eligibility lowering removed the observable vertex export");
    }
    for (uint32_t escape : {1u, 2u, 3u, 4u, 5u}) {
      const auto result = RecompileForTest(NggEligibilityShader(escape), options);
      CheckSpirvBinaryValidates(result.spirv);
      Check(needs_subgroup(result.program.info) &&
                SubgroupPolicy::AdmitRaster(true, 16u, false) ==
                    SubgroupPolicy::RasterAdmission::StageUnavailable,
            "observable NGG count/raw-mask/lane or cross-vertex operation lost its admission gate");
    }

    // Mixed shaders must keep the original eligibility predicate on native32/64
    // too, rather than merely retaining a feature that denies Mali vertex use.
    for (uint32_t escape : {0u, 4u, 5u}) {
      auto mixed = TranslateProgram(NggEligibilityShader(escape), options);
      IR::ConstantPropagationPass(mixed.program.blocks, wave);
      IR::RemoveIdentities(mixed.program);
      IR::EliminateDeadCode(mixed.program.blocks);
      const auto changed = IR::LowerNggVertexEligibility(mixed.program);
      Check(escape == 0u ? changed > 0u : changed == 0u,
            "NGG eligibility remapped a shader with independent lane/cross-vertex observations");
    }

    const auto code = NggEligibilityShader();
    auto unmarked = TranslateProgram(code, options);
    for (auto* block : unmarked.program.blocks)
      for (auto& inst : *block)
        if (inst.GetOpcode() == IR::ValueOpcode::Ballot) inst.SetFlags(uint64_t{0});
    const auto unmarked_result = CompileProgram(std::move(unmarked), options, {});
    CheckSpirvBinaryValidates(unmarked_result.spirv);
    Check(needs_subgroup(unmarked_result.program.info),
          "guest-created Ballot(true) was mistaken for the compiler's NGG entry state");

    // Identical operands do not make an ordinary guest ballot the ABI seed.
    auto duplicate = TranslateProgram(code, options);
    auto* entry = duplicate.program.blocks.front();
    auto seed = std::find_if(entry->begin(), entry->end(), [](const auto& inst) {
      return inst.GetOpcode() == IR::ValueOpcode::Ballot &&
             inst.template Flags<uint64_t>() == IR::NggVertexEntryBallotFlag;
    });
    Check(seed != entry->end(), "translator did not mark the NGG entry ballot");
    auto ordinary = entry->PrependNewInst(seed, IR::ValueOpcode::Ballot, {IR::Value(true)});
    Check(!IR::EquivalentValue(duplicate.program, IR::Value(&*seed), IR::Value(&*ordinary)),
          "IR equivalence merged guest ballot with compiler-only provenance");
    // Move the count's consumers to the unmarked ballot; retain the genuine seed
    // through an observable reference to exercise both in the simplification pass.
    const auto consumers = seed->Uses();
    for (const auto& use : consumers) use.user->SetArg(use.operand, IR::Value(&*ordinary));
    auto& seed_word = entry->AppendNewInst(IR::ValueOpcode::CompositeExtractU32x4,
                                          {IR::Value(&*seed), IR::Value(uint32_t{0})});
    entry->AppendNewInst(IR::ValueOpcode::ReferenceU32, {IR::Value(&seed_word)});
    IR::ConstantPropagationPass(duplicate.program.blocks, wave);
    IR::RemoveIdentities(duplicate.program);
    IR::EliminateDeadCode(duplicate.program.blocks);
    Check(IR::LowerNggVertexEligibility(duplicate.program) == 0,
          "simplification promoted the unmarked ballot into NGG provenance");

    auto other_stage = TranslateProgram(code, options);
    other_stage.program.stage = ShaderType::Compute;
    Check(IR::LowerNggVertexEligibility(other_stage.program) == 0,
          "vertex eligibility lowering changed another shader stage");
  }
}
