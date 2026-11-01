// SPDX-License-Identifier: GPL-2.0-only
// Synthetic guest ISA exercises the production NGG entry/prologue lowering.
// Point-list readback observes every submitted vertex, including partial waves;
// triangle assembly would silently discard trailing vertices in these cases.
namespace NggVertexEligibilityGpu {

std::vector<u32> VertexCode() {
  std::vector<u32> code{
      EncodeSop2(0x0e, 0, 3, 255), 0xffu, // s0 = merged_wave_info.ESVertCount
      EncodeSop2(0x01, 1, 192, 0),        // s1 = 64 - s0
      EncodeSop2(0x21, 126, 193, 1)};     // EXEC = UINT64_MAX >> s1
  // The real translated vertex entry supplies v5=VertexIndex, v8=InstanceIndex.
  // Place each vertex at the centre of its own pixel in a 16 by 16 target.
  code.push_back(EncodeVop2(0x1b, 16, InlineU32(15), 5));
  code.push_back(EncodeVop2(0x16, 17, InlineU32(4), 5));
  code.push_back(EncodeVop1(0x06, 16, Vgpr(16)));
  code.push_back(EncodeVop1(0x06, 17, Vgpr(17)));
  AppendVMovLiteral(&code, 24, std::bit_cast<u32>(0.125f));
  AppendVMovLiteral(&code, 25, std::bit_cast<u32>(-0.9375f));
  for (u32 reg : {16u, 17u}) {
    code.push_back(EncodeVop2(0x08, reg, Vgpr(24), reg));
    code.push_back(EncodeVop2(0x03, reg, Vgpr(25), reg));
  }
  AppendVMovLiteral(&code, 18, std::bit_cast<u32>(0.f));
  AppendVMovLiteral(&code, 19, std::bit_cast<u32>(1.f));
  // Instance bits distinguish baseInstance 0/3/7/11 with exact UNORM 0/255
  // channels. No floating-point render target or rounding tolerance is needed.
  code.push_back(EncodeVop2(0x1b, 20, InlineU32(1), 8));
  code.push_back(EncodeVop2(0x16, 21, InlineU32(2), 8));
  code.push_back(EncodeVop2(0x16, 22, InlineU32(3), 8));
  code.push_back(EncodeVop2(0x1b, 21, InlineU32(1), 21));
  code.push_back(EncodeVop2(0x1b, 22, InlineU32(1), 22));
  for (u32 reg : {20u, 21u, 22u}) code.push_back(EncodeVop1(0x06, reg, Vgpr(reg)));
  code.push_back(EncodeExp0(0x0c, 0xf, false)); // Position
  code.push_back(EncodeExp1(16, 17, 18, 19));
  code.push_back(EncodeExp0(0x0d, 0x1, false)); // PointSize = 1
  code.push_back(EncodeExp1(19, 19, 19, 19));
  code.push_back(EncodeExp0(0x20, 0xf));        // parameter 0: RGBA
  code.push_back(EncodeExp1(20, 21, 22, 19));
  AppendEnd(&code);
  return code;
}

std::vector<u32> CompileVertex(u32 wave) {
  constexpr auto name = "NggVertexEligibility";
  ShaderVertexInputInfo vertex{};
  vertex.wave_size = wave;
  vertex.pa_cl_vs_out_cntl = (1u << 21u) | (1u << 16u); // auxiliary export PointSize
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Vertex;
  options.wave_size = wave;
  options.input_info.vertex = &vertex;
  const auto code = VertexCode();
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, {});
  ValidateSpirv(name, compiled.spirv);
  const auto &info = compiled.program.info;
  Require(name, "lowered metadata", !info.subgroup_ballot && !info.subgroup_vote &&
              !info.subgroup_barrier && !info.subgroup_shuffle && !info.subgroup_arithmetic &&
              !info.subgroup_local_invocation_id,
          "the actual NGG prologue still requires a physical vertex subgroup");
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string text;
  Require(name, "disassembly", tools.Disassemble(compiled.spirv, &text),
          "cannot inspect final vertex module");
  Require(name, "final module", text.find("GroupNonUniform") == std::string::npos &&
              text.find("SubgroupLocalInvocationId") == std::string::npos &&
              text.find("BuiltIn PointSize") != std::string::npos &&
              text.find("BuiltIn Position") != std::string::npos,
          "final shader must preserve real point exports without subgroup operations");
  std::printf("[host]    NggVertexEligibility wave=%u synthetic ISA/SPIR-V ok\n", wave);
  return std::move(compiled.spirv);
}

struct Case {
  std::string name;
  PrimitiveContractDraw draw;
};

std::vector<Case> Cases(std::vector<u32> &indices) {
  std::vector<Case> cases;
  constexpr std::array counts{1u, 15u, 16u, 17u, 31u, 32u, 33u, 63u, 64u};
  for (u32 count : counts) {
    for (u32 first : {0u, 7u}) {
      auto draw = PrimitiveContract::Draw();
      draw.vertex_count = count;
      draw.first_vertex = first;
      draw.first_instance = first == 0 ? 0 : 3;
      cases.push_back({fmt::format("direct-{}-first{}", count, first), draw});
    }
    for (u32 variant = 0; variant < 3; ++variant) {
      auto draw = PrimitiveContract::Draw();
      draw.indexed = true;
      draw.vertex_count = count;
      // Guard before each index range also makes firstIndex observably nonzero.
      indices.push_back(255);
      draw.first_index = static_cast<u32>(indices.size());
      draw.vertex_offset = variant == 0 ? 0 : variant == 1 ? 7 : -3;
      draw.first_instance = variant == 0 ? 3 : variant == 1 ? 7 : 11;
      for (u32 i = 0; i < count; ++i) {
        const u32 ordinal = variant == 2 && i % 5 == 0 ? 0 : i;
        const u32 sparse_id = (ordinal * 37 + 73) % 128;
        indices.push_back(sparse_id + (variant == 2 ? 10 : 0));
      }
      cases.push_back({fmt::format("indexed-{}-variant{}", count, variant), draw});
    }
  }
  // Replay the first draw after all other ranges to expose leaked state.
  cases.push_back({"direct-replay", cases.front().draw});
  return cases;
}

void Verify(const Case &test, const std::vector<u32> &indices,
            const std::vector<u32> &pixels, u32 wave) {
  constexpr u32 pixels_count = PrimitiveContract::Width * PrimitiveContract::Height;
  std::array<std::array<u32, 4>, pixels_count> expected{};
  const auto &draw = test.draw;
  const std::array color{(draw.first_instance & 1u) * 255u,
                         ((draw.first_instance >> 2u) & 1u) * 255u,
                         ((draw.first_instance >> 3u) & 1u) * 255u, 255u};
  for (u32 i = 0; i < draw.vertex_count; ++i) {
    const int64_t id = draw.indexed
        ? int64_t(indices[draw.first_index + i]) + draw.vertex_offset
        : int64_t(draw.first_vertex) + i;
    Require(test.name.c_str(), "reference bounds", id >= 0 && id < pixels_count,
            "invalid synthetic point reference");
    expected[static_cast<size_t>(id)] = color;
  }
  Require(test.name.c_str(), "readback size", pixels.size() == pixels_count,
          "point readback did not cover the complete render target");
  u32 covered = 0;
  for (u32 pixel = 0; pixel < pixels_count; ++pixel) {
    covered += expected[pixel][3] != 0;
    for (u32 channel = 0; channel < 4; ++channel) {
      const u32 actual = (pixels[pixel] >> (channel * 8u)) & 0xffu;
      if (actual != expected[pixel][channel])
        Fail(test.name.c_str(), "point coverage/color",
             fmt::format("wave={} pixel={} channel={} actual={} expected={}",
                         wave, pixel, channel, actual, expected[pixel][channel]));
    }
  }
  std::printf("[gpu]     NggVertexEligibility wave=%u case=%s vertices=%u covered=%u ok\n",
              wave, test.name.c_str(), draw.vertex_count, covered);
}
} // namespace NggVertexEligibilityGpu

void CheckNggVertexEligibilityGpu(bool host_only) {
  using namespace NggVertexEligibilityGpu;
  const std::array vertices_spv{CompileVertex(32), CompileVertex(64)};
  const auto fragment = AssembleNativeMetalContract(PrimitiveContractSpv::Fragment);
  ValidateSpirv("NggVertexEligibilityFragment", fragment);
  if (host_only) {
    std::printf("NggVertexEligibility: 2 synthetic shaders compiled and SPIR-V validated; GPU not run\n");
    return;
  }
  VulkanHarness vulkan(VulkanHarness::Profile::VertexEligibility);
  std::vector<u32> indices;
  const auto cases = Cases(indices);
  std::vector<PrimitiveContractDraw> draws;
  for (const auto &test : cases) draws.push_back(test.draw);
  // Keep the common harness's guarded descriptor/input lifetime contract even
  // though these synthetic shaders derive positions directly from VertexIndex.
  const std::vector<u32> dummy_vertices(256 * 8, 0);
  for (u32 shader = 0; shader < vertices_spv.size(); ++shader) {
    const auto images = vulkan.RenderPrimitiveContract(
        "NggVertexEligibility", vertices_spv[shader], {}, {}, fragment, 0,
        dummy_vertices, indices, draws, vk::PrimitiveTopology::ePointList,
        vk::Format::eR8G8B8A8Unorm);
    Require("NggVertexEligibility", "draw completion", images.size() == cases.size(),
            "not every submitted draw produced a readback");
    for (size_t i = 0; i < cases.size(); ++i) Verify(cases[i], indices, images[i], 32u << shader);
  }
  std::printf("NggVertexEligibility: %zu GPU draw/readbacks passed for guest wave32/64\n", cases.size() * 2);
}
