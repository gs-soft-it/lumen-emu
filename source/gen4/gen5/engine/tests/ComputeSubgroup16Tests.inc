// SPDX-License-Identifier: GPL-2.0-only
// Numeric contracts for guest wave32/64 executed by native 16-lane subgroups.
TestCase ComputeSubgroupBoundaryReads(u32 wave) {
  using O = ShaderOpcode;
  TestCase test;
  test.name = wave == 32 ? "Subgroup16BoundaryReadsWave32" : "Subgroup16BoundaryReadsWave64";
  auto& code = test.code;
  AppendVMovU32(&code, 1, 100);
  code.push_back(EncodeVop2(0x25, 1, Vgpr(0), 1));
  test.initial.assign(wave, 0);
  for (u32 lane : {15u, 16u, 31u, 32u, 47u, 48u, 63u}) {
    if (lane < wave) test.initial[lane] = 1;
  }
  test.expected = test.initial;
  u32 row = 1;
  for (u32 lane : {15u, 16u, 31u, 32u, 47u, 48u, 63u}) {
    if (lane >= wave) continue;
    AppendVop3(&code, 0x360, 8, Vgpr(1), InlineU32(lane));
    AppendStoreSgprAtLaneDwordOffset(&code, 8, 0, row++ * wave);
    test.expected.insert(test.expected.end(), wave, 100u + lane);
  }
  code.push_back(EncodeVop2(0x1a, 31, InlineU32(2), 0));
  AppendBufferLoadDword(&code, 2, 31);
  code.push_back(EncodeSop1(0x04, 10, 126)); // save EXEC
  code.push_back(EncodeVopc(0xc2, InlineU32(1), 2));
  code.push_back(EncodeSop1(0x04, 12, 106)); // preserve the raw VCC word pair
  code.push_back(EncodeSop1(0x04, 126, 106));
  code.push_back(EncodeVop1(0x02, 8, Vgpr(1)));
  code.push_back(EncodeSop1(0x04, 126, 10));
  AppendStoreSgprAtLaneDwordOffset(&code, 8, 0, row++ * wave);
  test.expected.insert(test.expected.end(), wave, 115u);
  AppendStoreSgprAtLaneDwordOffset(&code, 12, 0, row++ * wave);
  test.expected.insert(test.expected.end(), wave, 0x80018000u);
  AppendStoreSgprAtLaneDwordOffset(&code, 13, 0, row++ * wave);
  test.expected.insert(test.expected.end(), wave, wave == 64 ? 0x80018001u : 0u);
  // The first active lane can be in a later slice while all earlier slices are empty.
  AppendSMovLiteral(&code, 126, wave == 32 ? 1u << 16u : 0u);
  AppendSMovLiteral(&code, 127, wave == 64 ? 1u << 16u : 0u);
  code.push_back(EncodeVop1(0x02, 8, Vgpr(1)));
  code.push_back(EncodeSop1(0x04, 126, 10));
  AppendStoreSgprAtLaneDwordOffset(&code, 8, 0, row++ * wave);
  test.expected.insert(test.expected.end(), wave, wave == 64 ? 148u : 116u);
  AppendEnd(&code);
  test.initial.resize(test.expected.size(), 0xdeadbeefu);
  test.opcodes = {O::V_MOV_B32, O::V_ADD_NC_U32, O::V_READLANE_B32,
                  O::V_READFIRSTLANE_B32, O::V_CMP_EQ_U32, O::S_MOV_B64,
                  O::S_MOV_B32, O::V_LSHLREV_B32, O::BUFFER_LOAD_DWORD,
                  O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.compute_info.threads_num[0] = wave;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = wave;
  test.has_compute_info = true;
  return test;
}

TestCase ComputeSubgroupDsSwizzle(u32 wave, bool masked) {
  using O = ShaderOpcode;
  TestCase test;
  test.name = wave == 32 ? (masked ? "Subgroup16SwizzleMaskedWave32" : "Subgroup16SwizzleWave32")
                         : (masked ? "Subgroup16SwizzleMaskedWave64" : "Subgroup16SwizzleWave64");
  auto& code = test.code;
  test.initial.assign(wave * 2u, 0xdeadbeefu);
  for (u32 lane = 0; lane < wave; ++lane) test.initial[lane] = 0;
  for (u32 lane : {15u, 16u, 31u, 32u, 47u, 48u, 63u}) {
    if (lane < wave) test.initial[lane] = 1;
  }
  test.expected = test.initial;
  for (u32 lane = 0; lane < wave; ++lane) {
    if (masked && !test.initial[lane]) continue;
    const auto source = lane ^ 16u;
    test.expected[wave + lane] = !masked || test.initial[source] ? 100u + source : 0u;
  }
  AppendVMovU32(&code, 1, 100);
  code.push_back(EncodeVop2(0x25, 1, Vgpr(0), 1));
  AppendVMovLiteral(&code, 3, 0xdeadbeefu);
  if (masked) {
    code.push_back(EncodeVop2(0x1a, 31, InlineU32(2), 0));
    AppendBufferLoadDword(&code, 2, 31);
    code.push_back(EncodeSop1(0x04, 10, 126));
    code.push_back(EncodeVopc(0xc2, InlineU32(1), 2));
    code.push_back(EncodeSop1(0x04, 126, 106));
  }
  code.push_back(EncodeDs0(0x35, 0x401f)); // XOR16 within each guest32 half
  code.push_back(EncodeDs1(3, 0, 1));
  if (masked) code.push_back(EncodeSop1(0x04, 126, 10));
  AppendStoreVgprAtLaneDwordOffset(&code, 3, 0, wave);
  AppendEnd(&code);
  test.opcodes = {O::V_MOV_B32, O::V_ADD_NC_U32, O::DS_SWIZZLE_B32,
                  O::V_LSHLREV_B32, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  if (masked) {
    test.opcodes.insert(test.opcodes.end(), {O::BUFFER_LOAD_DWORD, O::S_MOV_B64, O::V_CMP_EQ_U32});
  }
  test.compute_info.threads_num[0] = wave;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = wave;
  test.has_compute_info = true;
  return test;
}

TestCase ComputeSubgroupPartial3D(u32 wave) {
  using O = ShaderOpcode;
  TestCase test;
  test.name = wave == 32 ? "Subgroup16Partial3DWave32" : "Subgroup16Partial3DWave64";
  auto& code = test.code;
  // g = x + 7*(y + 3*z), dimensions 7x3x5: final slice has nine valid invocations.
  code.push_back(EncodeVop2(0x25, 3, Vgpr(2), 2));
  code.push_back(EncodeVop2(0x25, 3, Vgpr(2), 3));
  code.push_back(EncodeVop2(0x25, 3, Vgpr(1), 3));
  code.push_back(EncodeVop1(0x01, 4, Vgpr(3)));
  for (u32 i = 0; i < 6u; ++i) code.push_back(EncodeVop2(0x25, 3, Vgpr(4), 3));
  code.push_back(EncodeVop2(0x25, 3, Vgpr(0), 3));
  code.push_back(EncodeVop2(0x1a, 5, InlineU32(2), 3));
  AppendVMovU32(&code, 6, 1);
  AppendBufferStoreOpcode(&code, 0x32, 6, 5, false);
  AppendEnd(&code);
  test.initial.resize(128);
  test.expected.assign(105, 1);
  test.expected.resize(128);
  test.opcodes = {O::V_MOV_B32, O::V_ADD_NC_U32, O::V_LSHLREV_B32,
                  O::BUFFER_ATOMIC_ADD, O::S_ENDPGM};
  test.compute_info.threads_num[0] = 7;
  test.compute_info.threads_num[1] = 3;
  test.compute_info.threads_num[2] = 5;
  test.compute_info.thread_ids_num = 3;
  test.compute_info.wave_size = wave;
  test.has_compute_info = true;
  return test;
}

TestCase ComputeSubgroupPermuteCollision(u32 wave) {
  using O = ShaderOpcode;
  TestCase test;
  test.name = wave == 32 ? "Subgroup16PermuteCollisionWave32" : "Subgroup16PermuteCollisionWave64";
  auto& code = test.code;
  AppendVMovU32(&code, 1, 100);
  code.push_back(EncodeVop2(0x25, 1, Vgpr(0), 1));
  AppendVMovU32(&code, 2, 0);
  AppendVMovLiteral(&code, 3, 0xdeadbeefu);
  code.push_back(EncodeSop1(0x04, 10, 126));
  AppendSMovLiteral(&code, 126, 0x18001u); // destinations/sources 0,15,16
  AppendSMovLiteral(&code, 127, wave == 64 ? 0x18001u : 0u);
  code.push_back(EncodeDs0(0xb2));
  code.push_back(EncodeDs1(3, 1, 2));
  code.push_back(EncodeSop1(0x04, 126, 10));
  AppendStoreVgprAtLaneDwordOffset(&code, 3, 0, 0);
  AppendEnd(&code);
  test.initial.assign(wave, 0);
  test.expected.assign(wave, 0xdeadbeefu);
  for (u32 base = 0; base < wave; base += 32) {
    test.expected[base] = 116u + base; // highest enabled writer crosses the native16 boundary
    test.expected[base + 15u] = test.expected[base + 16u] = 0;
  }
  test.opcodes = {O::V_MOV_B32, O::V_ADD_NC_U32, O::S_MOV_B64, O::S_MOV_B32,
                  O::DS_PERMUTE_B32, O::V_LSHLREV_B32, O::BUFFER_STORE_DWORD, O::S_ENDPGM};
  test.compute_info.threads_num[0] = wave;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.wave_size = wave;
  test.has_compute_info = true;
  return test;
}

void CheckComputeSubgroup16(bool host_only, bool native_baseline = false) {
  std::vector<TestCase> cases;
  for (u32 wave : {32u, 64u}) {
    cases.push_back(ComputeSubgroupBoundaryReads(wave));
    cases.push_back(ComputeSubgroupDsSwizzle(wave, false));
    cases.push_back(ComputeSubgroupDsSwizzle(wave, true));
    cases.push_back(ComputeSubgroupPartial3D(wave));
    cases.push_back(ComputeSubgroupPermuteCollision(wave));
  }
  cases.push_back(DsPermuteCapturedExecOffsetAndWrap());
  cases.push_back(DsBpermuteCapturedExecOffsetAndWrap());
  cases.push_back(DsPermuteWave64UsesIndependentHalves());
  cases.push_back(DsBpermuteWave64UsesIndependentHalves());
  cases.push_back(Wave64CrossHalfLaneAndLds());
  cases.push_back(Wave64RawMasksAndScalarBranch());
  cases.push_back(Wave64PartialMultidimensionalWorkgroup());
  cases.push_back(Wave64AppendConsumeHighHalf());
  cases.push_back(ScratchIsPrivatePerInvocation());
  cases.push_back(DispatcherIrreducibleControlFlow());
  for (const auto& test : cases) {
    for (u32 host : {16u, 32u, 64u}) {
      (void)CompileCase(test, host);
    }
    std::printf("[host]    %-32s ok (subgroup16/32/64 SPIR-V)\n", test.name);
  }
  if (host_only) return;
  VulkanHarness vulkan(VulkanHarness::Profile::AluSubset);
  const auto width = vulkan.SubgroupSize();
  Require("ComputeSubgroup16", "physical subgroup",
          native_baseline ? (width == 32u || width == 64u) : width == 16u,
          native_baseline ? "the baseline regression requires actual native32/64"
                          : "this numerical fixture requires the actual native16 device");
  std::printf("[compute] numerical subgroup regression: native width=%u cases=%zu\n",
              width, cases.size());
  for (const auto& test : cases) RunCase(&vulkan, test);
}
