// SPDX-License-Identifier: GPL-2.0-only
// A synthetic guest fragment loads a draw-uniform scalar buffer and branches
// through VCC. Actual pixel readback tests both branch outcomes and partial
// raster quads without assuming where the GPU schedules helper invocations.
namespace UniformFragmentVoteGpu {

std::vector<u32> AfterVoteCode(bool branch_nonzero, bool multiword = false) {
  std::vector<u32> code;
  for (u32 component = 0; component < 2; ++component) {
    code.push_back(EncodeVintrp(0, 16 + component, 0, component, 0));
    code.push_back(EncodeVintrp(1, 16 + component, 0, component, 1));
  }
  AppendVMovLiteral(&code, 6, std::bit_cast<u32>(2.f));
  AppendVMovLiteral(&code, 7, std::bit_cast<u32>(4.f));
  if (multiword) {
    // The decoder scalarizes this real X4 load while retaining the original
    // component_count=4. Only its first and last words feed the uniform vote.
    code.insert(code.end(), {
        EncodeSmem0(0x0a, 12, 4), EncodeSmem1(16, 125), // s12..15 = CB words4..7
        EncodeVop1(0x01, 0, 12), EncodeVopc(0x0d, InlineU32(0), 0),
        EncodeSop1(0x04, 4, 106),                     // s[4:5] = VCC
        EncodeVop1(0x01, 1, 15), EncodeVopc(0x0d, InlineU32(0), 1),
        EncodeSop2(0x0f, 106, 4, 106)});             // VCC = first && last
  } else {
    code.insert(code.end(), {
        EncodeSmem0(0x08, 12, 4), EncodeSmem1(0, 125),
        EncodeVop1(0x01, 0, 12), EncodeVopc(0x0d, InlineU32(0), 0)});
  }
  const size_t conditional = code.size();
  code.push_back(0);
  // Fallthrough samples LOD0 and chooses UV*2 for the post-merge LOD1.
  code.push_back(EncodeMimg0(0x20, 0xf));
  code.push_back(EncodeMimg1(20, 16, 4, 6));
  code.push_back(EncodeVop2(0x08, 30, Vgpr(6), 16));
  code.push_back(EncodeVop2(0x08, 31, Vgpr(6), 17));
  code.push_back(EncodeVop1(0x01, 4, 242));
  const size_t skip_taken = code.size();
  code.push_back(0);
  const size_t taken = code.size();
  // Taken samples LOD1 and chooses UV*4 for the post-merge LOD2.
  code.push_back(EncodeVop2(0x08, 18, Vgpr(6), 16));
  code.push_back(EncodeVop2(0x08, 19, Vgpr(6), 17));
  code.push_back(EncodeMimg0(0x20, 0xf));
  code.push_back(EncodeMimg1(20, 18, 4, 6));
  code.push_back(EncodeVop2(0x08, 30, Vgpr(7), 16));
  code.push_back(EncodeVop2(0x08, 31, Vgpr(7), 17));
  code.push_back(EncodeVop1(0x01, 4, InlineU32(0)));
  const size_t merge = code.size();
  code[conditional] = EncodeSopp(branch_nonzero ? 0x07 : 0x06,
                                  static_cast<u32>(taken - conditional - 1));
  code[skip_taken] = EncodeSopp(0x02, static_cast<u32>(merge - skip_taken - 1));
  // v30/v31 become coordinate Phis selected by the uniform branch; the
  // coordinates themselves still vary between neighbouring fragment pixels.
  code.push_back(EncodeMimg0(0x20, 0xf));
  code.push_back(EncodeMimg1(24, 30, 4, 6));
  code.push_back(EncodeExp0(0, 0xf));
  code.push_back(EncodeExp1(20, 25, 22, 4)); // first sample R/B, merged sample G, branch A
  AppendEnd(&code);
  return code;
}

std::vector<u32> Code(bool branch_nonzero, bool sampled_prefix = false,
                      bool sampled_after_vote = false, bool multiword = false) {
  if (sampled_after_vote) return AfterVoteCode(branch_nonzero, multiword);
  std::vector<u32> code;
  if (sampled_prefix) {
    // Ordinary interpolated coordinates retain live implicit derivatives. The
    // sample precedes every scalar branch and its RGB is exported by both arms.
    for (u32 component = 0; component < 2; ++component) {
      code.push_back(EncodeVintrp(0, 16 + component, 0, component, 0));
      code.push_back(EncodeVintrp(1, 16 + component, 0, component, 1));
    }
    code.push_back(EncodeMimg0(0x20, 0xf));
    code.push_back(EncodeMimg1(20, 16, 4, 6)); // texture s[16:23], sampler s[24:27]
  }
  code.insert(code.end(), {
      EncodeSmem0(0x08, 12, 4), EncodeSmem1(0, 125), // s_buffer_load_dword s12,s[8:11],0
      EncodeVop1(0x01, 0, 12),                     // v0 = s12
      EncodeVopc(0x0d, InlineU32(0), 0),           // v_cmp_neq_f32 0,v0 (unordered true)
      EncodeSopp(branch_nonzero ? 0x07 : 0x06, 3),
      EncodeVop1(0x01, 1, 242),                   // fallthrough: red
      EncodeVop1(0x01, 2, InlineU32(0)),
      EncodeSopp(0x02, 2),
      EncodeVop1(0x01, 1, InlineU32(0)),           // taken: green
      EncodeVop1(0x01, 2, 242),
      EncodeVop1(0x01, 3, InlineU32(0)),
      EncodeVop1(0x01, 4, 242),
      EncodeExp0(0, 0xf),
      sampled_prefix ? EncodeExp1(20, 21, 22, 1) : EncodeExp1(1, 2, 3, 4)});
  AppendEnd(&code);
  return code;
}

CompiledShader Compile(u32 wave, bool branch_nonzero, bool sampled_prefix = false,
                       bool sampled_after_vote = false, bool multiword = false) {
  const auto name = multiword ? "UniformFragmentMultiwordVote"
      : sampled_after_vote ? "UniformFragmentAfterVote"
      : sampled_prefix ? "UniformFragmentTextureVote" : "UniformFragmentVote";
  Require(name, "multiword variant", !multiword || (sampled_after_vote && !sampled_prefix),
          "multiword fixture must retain the after-vote texture control flow");
  const bool sampled = sampled_prefix || sampled_after_vote;
  auto user_data = MakeNativeUserData(nullptr);
  // Raw scalar-buffer words at a fixed binding. Runtime values are uploaded
  // only after compilation, so the test cannot pass via constant folding.
  user_data[8] = 0;
  user_data[9] = 0;
  user_data[10] = (multiword ? 8u : 1u) * sizeof(u32);
  user_data[11] = 3u << 28u;
  if (sampled) {
    const auto texture = MakeSampledTextureData(Prospero::BufferFormat::k8_8_8_8UNorm);
    std::copy_n(texture.begin(), 8, user_data.begin() + 16);
    user_data[18] = 63u | (63u << 14u); // 64 by 64
    user_data[19] |= DstSel(4, 5, 6, 7) | (2u << 16u); // mip levels 0..2
    user_data[24] = 0;
    user_data[25] = 0;
    user_data[26] = 0;
    user_data[27] = 0;
  }
  ShaderPixelInputInfo pixel{};
  pixel.wave_size = wave;
  pixel.input_num = sampled ? 1 : 0;
  pixel.ps_no_perspective = sampled;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Pixel;
  options.wave_size = wave;
  options.input_info.pixel = &pixel;
  options.user_data = user_data;
  const auto code = Code(branch_nonzero, sampled_prefix, sampled_after_vote, multiword);
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
  ShaderRecompiler::IR::ResourceSnapshot resources;
  ShaderRecompiler::IR::ResourceSpecialization specialization;
  const ShaderRecompiler::IR::SrtRuntime runtime{
      .user_data = options.user_data,
      .shader_base = reinterpret_cast<uint64_t>(code.data())};
  Require(name, "resource materialization",
          ShaderRecompiler::IR::MaterializeResources(plan, runtime, resources, specialization),
          "synthetic scalar buffer could not be materialized");
  // Keep the first eight push words owned by the common vertex fixture.
  auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization, 8u);
  ValidateSpirv(name, compiled.spirv);
  const auto &info = compiled.program.info;
  Require(name, "lowered metadata", !info.subgroup_ballot && !info.subgroup_vote &&
              !info.subgroup_barrier && !info.subgroup_shuffle && !info.subgroup_arithmetic &&
              !info.subgroup_local_invocation_id && info.buffers.size() == 1 &&
              info.buffers.front().read && !info.buffers.front().written,
          "uniform scalar-buffer branch must keep its live read without a subgroup vote");
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string text;
  Require(name, "disassembly", tools.Disassemble(compiled.spirv, &text),
          "cannot inspect final fragment module");
  Require(name, "final module", text.find("GroupNonUniform") == std::string::npos &&
              text.find("SubgroupLocalInvocationId") == std::string::npos &&
              text.find("OpFUnordNotEqual") != std::string::npos &&
              text.find("StorageBuffer") != std::string::npos,
          "final module lost its runtime unordered comparison or retained subgroup operations");
  if (sampled) {
    Require(name, "implicit sample preserved", info.images.size() == 1 && info.samplers.size() == 1 &&
                CountText(text, "OpImageSampleImplicitLod") == (sampled_after_vote ? 3u : 1u) &&
                text.find("OpImageSampleExplicitLod") == std::string::npos,
            "texture samples were removed or changed to explicit LOD");
  }
  if (sampled_after_vote) {
    using namespace ShaderRecompiler::IR;
    bool coordinates_use_phi = false;
    for (const auto *block : compiled.program.blocks) {
      for (const auto &inst : *block) {
        if (inst.GetOpcode() != ValueOpcode::ImageSampleRaw) continue;
        std::vector<const Inst *> visited;
        const auto reaches_phi = [&](auto &&self, Value value, u32 depth) -> bool {
          const auto *source = value.Resolve().TryInstruction();
          if (!source || depth >= 128 || std::find(visited.begin(), visited.end(), source) != visited.end())
            return false;
          if (source->GetOpcode() == ValueOpcode::Phi) return true;
          visited.push_back(source);
          for (size_t arg = 0; arg < source->NumArgs(); ++arg)
            if (self(self, source->Arg(arg), depth + 1)) return true;
          return false;
        };
        coordinates_use_phi |= reaches_phi(reaches_phi, inst.Arg(2), 0);
      }
    }
    Require(name, "branch-selected coordinate Phi", coordinates_use_phi &&
                text.find("OpPhi") != std::string::npos &&
                text.find("OpSelectionMerge") != std::string::npos,
            "after-merge sample no longer exercises a real branch-selected coordinate Phi");
  }
  if (multiword) {
    using namespace ShaderRecompiler::IR;
    u32 reads = 0, comparisons = 0, logical_ands = 0;
    bool first = false, last = false;
    for (const auto *block : compiled.program.blocks) {
      for (const auto &inst : *block) {
        comparisons += inst.GetOpcode() == ValueOpcode::FPUnordNotEqual32;
        logical_ands += inst.GetOpcode() == ValueOpcode::LogicalAnd;
        if (inst.GetOpcode() != ValueOpcode::ReadConstBuffer) continue;
        ++reads;
        const auto index = inst.Flags<MemoryFlags>().index;
        Require(name, "X4 scalar memory index", index < compiled.program.memory_info.size(),
                "scalarized X4 read lost its memory metadata");
        const auto &memory = compiled.program.memory_info[index];
        Require(name, "X4 scalar memory shape", memory.kind == ResourceKind::ScalarBuffer &&
                    memory.component_count == 4 && memory.data_dwords == 1 && memory.data_bits == 32 &&
                    !memory.planning_only && !memory.coherent && !memory.typed && !memory.formatted &&
                    inst.Arg(1).Resolve() == Value(0u),
                "real guest X4 load did not retain scalarized four-component provenance");
        first |= memory.component_index == 0 && memory.offset == 16;
        last |= memory.component_index == 3 && memory.offset == 28;
      }
    }
    Require(name, "independent runtime words", reads == 2 && first && last &&
                comparisons == 2 && logical_ands == 1 &&
                CountText(text, "OpFUnordNotEqual") == 2,
            "both distinct runtime CB words and their logical AND must survive compilation");
  }
  std::vector<u32> packed;
  for (const auto reg : info.user_data_registers)
    packed.push_back(resources.user_data[reg - compiled.program.user_data_base]);
  packed.resize(compiled.program.bindings.ShaderDataDwords());
  std::printf("[host]    %s wave=%u branch=%s synthetic ISA/SPIR-V ok\n",
              name, wave, branch_nonzero ? "VCCNZ" : "VCCZ");
  return {std::move(compiled.spirv), std::move(compiled.program),
          std::move(resources), std::move(packed)};
}

struct Geometry {
  const char *name;
  float x, y, extent;
};
constexpr std::array geometries{
    Geometry{"full", 0.f, 0.f, 32.f},
    Geometry{"diagonal-edge", 0.f, 0.f, 15.25f},
    Geometry{"single-covered-pixel", 0.f, 0.f, 1.25f},
    Geometry{"translated-edge", 3.25f, 4.25f, 6.75f}};

std::vector<u32> Vertices() {
  std::vector<std::array<float, 2>> positions;
  for (const auto &geometry : geometries) {
    const auto ndc = [](float value) { return value / 8.f - 1.f; };
    positions.push_back({ndc(geometry.x), ndc(geometry.y)});
    positions.push_back({ndc(geometry.x + geometry.extent), ndc(geometry.y)});
    positions.push_back({ndc(geometry.x), ndc(geometry.y + geometry.extent)});
  }
  return PrimitiveContract::Vertices(positions);
}

void Verify(const std::vector<u32> &pixels, const Geometry &geometry,
            u32 wave, bool branch_nonzero, u32 value) {
  constexpr auto name = "UniformFragmentVote";
  constexpr u32 width = PrimitiveContract::Width, height = PrimitiveContract::Height;
  Require(name, "readback size", pixels.size() == width * height,
          "uniform fragment readback did not cover the RGBA8 target");
  // IEEE unordered != zero is false only for either signed zero, including
  // true for quiet NaN. The reference does not perform a host FP comparison.
  const bool comparison = (value & 0x7fffffffu) != 0;
  const bool taken = branch_nonzero ? comparison : !comparison;
  const u32 color = taken ? 0xff00ff00u : 0xff0000ffu;
  u32 covered_count = 0;
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const float px = float(x) + 0.5f, py = float(y) + 0.5f;
      const bool covered = px >= geometry.x && py >= geometry.y &&
          px + py < geometry.x + geometry.y + geometry.extent;
      const u32 expected = covered ? color : 0u;
      covered_count += covered;
      if (pixels[y * width + x] != expected)
        Fail(name, "coverage/branch color",
             fmt::format("wave={} branch={} input=0x{:08x} geometry={} pixel=({}, {}) "
                         "actual=0x{:08x} expected=0x{:08x}", wave,
                         branch_nonzero ? "VCCNZ" : "VCCZ", value, geometry.name,
                         x, y, pixels[y * width + x], expected));
    }
  }
  std::printf("[gpu]     UniformFragmentVote wave=%u branch=%s input=0x%08x geometry=%s covered=%u ok\n",
              wave, branch_nonzero ? "VCCNZ" : "VCCZ", value, geometry.name, covered_count);
}
} // namespace UniformFragmentVoteGpu

void CheckUniformFragmentVoteGpu(bool host_only) {
  using namespace UniformFragmentVoteGpu;
  std::vector<CompiledShader> fragments;
  for (u32 wave : {32u, 64u})
    for (bool nonzero : {false, true}) fragments.push_back(Compile(wave, nonzero));
  if (host_only) {
    std::printf("UniformFragmentVote: 4 synthetic fragment shaders compiled and SPIR-V validated; GPU not run\n");
    return;
  }
  VulkanHarness vulkan(VulkanHarness::Profile::UniformFragmentVote);
  const auto vertex = AssembleNativeMetalContract(PrimitiveContractSpv::Vertex);
  const auto vertices = Vertices();
  std::vector<PrimitiveContractDraw> draws;
  for (u32 geometry = 0; geometry < geometries.size(); ++geometry) {
    auto draw = PrimitiveContract::Draw();
    draw.first_vertex = geometry * 3;
    draws.push_back(draw);
  }
  constexpr std::array values{0x00000000u, 0x80000000u, 0x3f800000u,
                              0xbf800000u, 0x7f800000u, 0x7fc12345u};
  for (u32 shader = 0; shader < fragments.size(); ++shader) {
    const u32 wave = shader < 2 ? 32 : 64;
    const bool nonzero = shader % 2 != 0;
    for (u32 value : values) {
      const std::array constants{value};
      const auto pixels = vulkan.RenderPrimitiveContract(
          "UniformFragmentVote", vertex, {}, {}, fragments[shader].spirv, 0,
          vertices, {}, draws, vk::PrimitiveTopology::eTriangleList,
          vk::Format::eR8G8B8A8Unorm, &fragments[shader], constants);
      Require("UniformFragmentVote", "draw completion", pixels.size() == geometries.size(),
              "not every geometry produced a readback");
      for (u32 geometry = 0; geometry < geometries.size(); ++geometry)
        Verify(pixels[geometry], geometries[geometry], wave, nonzero, value);
    }
  }
  std::printf("UniformFragmentVote: %zu GPU draw/readbacks passed for guest wave32/64\n",
              fragments.size() * values.size() * geometries.size());
}

namespace UniformFragmentTextureVoteGpu {
constexpr std::array<u32, 3> mip_colors{0xff0000ffu, 0xff00ff00u, 0xffff0000u};

std::vector<u32> Vertices(u32 mip) {
  std::vector<u32> words;
  const float uv_per_pixel = std::ldexp(1.f, static_cast<int>(mip)) / 64.f;
  for (const auto &geometry : UniformFragmentVoteGpu::geometries) {
    const std::array points{
        std::array{geometry.x, geometry.y},
        std::array{geometry.x + geometry.extent, geometry.y},
        std::array{geometry.x, geometry.y + geometry.extent}};
    for (const auto &point : points) {
      for (float value : std::array{point[0] / 8.f - 1.f, point[1] / 8.f - 1.f, 0.f, 1.f,
                                    point[0] * uv_per_pixel, point[1] * uv_per_pixel, 0.f, 1.f})
        words.push_back(std::bit_cast<u32>(value));
    }
  }
  return words;
}

void Verify(const std::vector<u32> &pixels, const UniformFragmentVoteGpu::Geometry &geometry,
            u32 wave, bool branch_nonzero, u32 value, u32 mip) {
  constexpr auto name = "UniformFragmentTextureVote";
  constexpr u32 width = PrimitiveContract::Width, height = PrimitiveContract::Height;
  Require(name, "readback size", pixels.size() == width * height,
          "sampled fragment readback did not cover the RGBA8 target");
  const bool comparison = (value & 0x7fffffffu) != 0;
  const bool taken = branch_nonzero ? comparison : !comparison;
  // RGB must retain the sampled mip color in either branch. Alpha records the
  // branch, independently of texture data; uncovered pixels remain clear zero.
  const u32 color = (mip_colors[mip] & 0x00ffffffu) | (taken ? 0u : 0xff000000u);
  u32 covered_count = 0;
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const float px = float(x) + 0.5f, py = float(y) + 0.5f;
      const bool covered = px >= geometry.x && py >= geometry.y &&
          px + py < geometry.x + geometry.y + geometry.extent;
      const u32 expected = covered ? color : 0;
      covered_count += covered;
      if (pixels[y * width + x] != expected)
        Fail(name, "implicit LOD/branch/coverage",
             fmt::format("wave={} branch={} input=0x{:08x} mip={} geometry={} pixel=({}, {}) "
                         "actual=0x{:08x} expected=0x{:08x}", wave,
                         branch_nonzero ? "VCCNZ" : "VCCZ", value, mip, geometry.name,
                         x, y, pixels[y * width + x], expected));
    }
  }
  std::printf("[gpu]     UniformFragmentTextureVote wave=%u branch=%s input=0x%08x mip=%u "
              "geometry=%s covered=%u ok\n", wave, branch_nonzero ? "VCCNZ" : "VCCZ",
              value, mip, geometry.name, covered_count);
}
} // namespace UniformFragmentTextureVoteGpu

void CheckUniformFragmentTextureVoteGpu(bool host_only) {
  using namespace UniformFragmentTextureVoteGpu;
  std::vector<CompiledShader> fragments;
  for (u32 wave : {32u, 64u})
    for (bool nonzero : {false, true})
      fragments.push_back(UniformFragmentVoteGpu::Compile(wave, nonzero, true));
  if (host_only) {
    std::printf("UniformFragmentTextureVote: 4 synthetic sampled fragment shaders compiled "
                "and SPIR-V validated; GPU not run\n");
    return;
  }
  VulkanHarness vulkan(VulkanHarness::Profile::UniformFragmentVote);
  const auto vertex = AssembleNativeMetalContract(PrimitiveContractSpv::Vertex);
  std::vector<std::vector<u32>> mips;
  for (u32 mip = 0; mip < mip_colors.size(); ++mip)
    mips.emplace_back((64u >> mip) * (64u >> mip), mip_colors[mip]);
  auto texture = vulkan.CreateImageMips("UniformFragmentTextureVote", 64, 64,
      vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled, mips, 1,
      vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageType::e2D, vk::ImageViewType::e2D, 1);
  const auto sampler = vulkan.CreateSampler("UniformFragmentTextureVote", 3, vk::Filter::eNearest);
  std::vector<PrimitiveContractDraw> draws;
  for (u32 geometry = 0; geometry < UniformFragmentVoteGpu::geometries.size(); ++geometry) {
    auto draw = PrimitiveContract::Draw();
    draw.first_vertex = geometry * 3;
    draws.push_back(draw);
  }
  constexpr std::array values{0x00000000u, 0x3f800000u, 0x7fc12345u};
  for (u32 shader = 0; shader < fragments.size(); ++shader) {
    const u32 wave = shader < 2 ? 32 : 64;
    const bool nonzero = shader % 2 != 0;
    for (u32 mip = 0; mip < mip_colors.size(); ++mip) {
      // At a 64-wide texture, these exact binary gradients give rho=1,2,4
      // and implicit LOD=0,1,2, far from nearest-mip selection boundaries.
      const auto vertices = Vertices(mip);
      for (u32 value : values) {
        const std::array constants{value};
        const auto pixels = vulkan.RenderPrimitiveContract(
            "UniformFragmentTextureVote", vertex, {}, {}, fragments[shader].spirv, 0,
            vertices, {}, draws, vk::PrimitiveTopology::eTriangleList,
            vk::Format::eR8G8B8A8Unorm, &fragments[shader], constants, &texture, sampler);
        Require("UniformFragmentTextureVote", "draw completion",
                pixels.size() == UniformFragmentVoteGpu::geometries.size(),
                "not every sampled geometry produced a readback");
        for (u32 geometry = 0; geometry < UniformFragmentVoteGpu::geometries.size(); ++geometry)
          Verify(pixels[geometry], UniformFragmentVoteGpu::geometries[geometry],
                 wave, nonzero, value, mip);
      }
    }
  }
  vulkan.Device().destroySampler(sampler);
  vulkan.DestroyImage(&texture);
  std::printf("UniformFragmentTextureVote: %zu GPU draw/readbacks passed for guest wave32/64\n",
              fragments.size() * values.size() * mip_colors.size() * draws.size());
}

namespace UniformFragmentAfterVoteGpu {
// Direct UNORM8 sampling/export round trips these byte values. Each mip has a
// unique R, G and B, so the branch and merged samples are independently visible.
constexpr std::array<u32, 3> mip_colors{0xff604020u, 0xffc0a080u, 0xfffff0e0u};

void Verify(const std::vector<u32> &pixels, const UniformFragmentVoteGpu::Geometry &geometry,
            u32 wave, bool branch_nonzero, u32 value) {
  constexpr auto name = "UniformFragmentAfterVote";
  constexpr u32 width = PrimitiveContract::Width, height = PrimitiveContract::Height;
  Require(name, "readback size", pixels.size() == width * height,
          "after-vote fragment readback did not cover the RGBA8 target");
  const bool comparison = (value & 0x7fffffffu) != 0;
  const bool taken = branch_nonzero ? comparison : !comparison;
  const u32 first_mip = taken ? 1 : 0;
  const u32 merged_mip = taken ? 2 : 1;
  const u32 color = (mip_colors[first_mip] & 0x00ff00ffu) |
                    (mip_colors[merged_mip] & 0x0000ff00u) |
                    (taken ? 0u : 0xff000000u);
  u32 covered_count = 0;
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const float px = float(x) + 0.5f, py = float(y) + 0.5f;
      const bool covered = px >= geometry.x && py >= geometry.y &&
          px + py < geometry.x + geometry.y + geometry.extent;
      const u32 expected = covered ? color : 0;
      covered_count += covered;
      if (pixels[y * width + x] != expected)
        Fail(name, "branch/coordinate-Phi/implicit LOD/coverage",
             fmt::format("wave={} branch={} input=0x{:08x} firstMip={} mergedMip={} geometry={} "
                         "pixel=({}, {}) actual=0x{:08x} expected=0x{:08x}", wave,
                         branch_nonzero ? "VCCNZ" : "VCCZ", value, first_mip, merged_mip,
                         geometry.name, x, y, pixels[y * width + x], expected));
    }
  }
  std::printf("[gpu]     UniformFragmentAfterVote wave=%u branch=%s input=0x%08x "
              "firstMip=%u mergedMip=%u geometry=%s covered=%u ok\n", wave,
              branch_nonzero ? "VCCNZ" : "VCCZ", value, first_mip, merged_mip,
              geometry.name, covered_count);
}
} // namespace UniformFragmentAfterVoteGpu

void CheckUniformFragmentAfterVoteGpu(bool host_only) {
  using namespace UniformFragmentAfterVoteGpu;
  std::vector<CompiledShader> fragments;
  for (u32 wave : {32u, 64u})
    for (bool nonzero : {false, true})
      fragments.push_back(UniformFragmentVoteGpu::Compile(wave, nonzero, false, true));
  if (host_only) {
    std::printf("UniformFragmentAfterVote: 4 synthetic shaders with branch samples and "
                "after-merge coordinate Phi compiled and SPIR-V validated; GPU not run\n");
    return;
  }
  VulkanHarness vulkan(VulkanHarness::Profile::UniformFragmentVote);
  const auto vertex = AssembleNativeMetalContract(PrimitiveContractSpv::Vertex);
  const auto vertices = UniformFragmentTextureVoteGpu::Vertices(0);
  std::vector<std::vector<u32>> mips;
  for (u32 mip = 0; mip < mip_colors.size(); ++mip)
    mips.emplace_back((64u >> mip) * (64u >> mip), mip_colors[mip]);
  auto texture = vulkan.CreateImageMips("UniformFragmentAfterVote", 64, 64,
      vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled, mips, 1,
      vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageType::e2D, vk::ImageViewType::e2D, 1);
  const auto sampler = vulkan.CreateSampler("UniformFragmentAfterVote", 3, vk::Filter::eNearest);
  std::vector<PrimitiveContractDraw> draws;
  for (u32 geometry = 0; geometry < UniformFragmentVoteGpu::geometries.size(); ++geometry) {
    auto draw = PrimitiveContract::Draw();
    draw.first_vertex = geometry * 3;
    draws.push_back(draw);
  }
  constexpr std::array values{0x00000000u, 0x80000000u, 0x3f800000u,
                              0xbf800000u, 0x7f800000u, 0x7fc12345u};
  for (u32 shader = 0; shader < fragments.size(); ++shader) {
    const u32 wave = shader < 2 ? 32 : 64;
    const bool nonzero = shader % 2 != 0;
    for (u32 value : values) {
      const std::array constants{value};
      const auto pixels = vulkan.RenderPrimitiveContract(
          "UniformFragmentAfterVote", vertex, {}, {}, fragments[shader].spirv, 0,
          vertices, {}, draws, vk::PrimitiveTopology::eTriangleList,
          vk::Format::eR8G8B8A8Unorm, &fragments[shader], constants, &texture, sampler);
      Require("UniformFragmentAfterVote", "draw completion",
              pixels.size() == UniformFragmentVoteGpu::geometries.size(),
              "not every after-vote geometry produced a readback");
      for (u32 geometry = 0; geometry < UniformFragmentVoteGpu::geometries.size(); ++geometry)
        UniformFragmentAfterVoteGpu::Verify(
            pixels[geometry], UniformFragmentVoteGpu::geometries[geometry], wave, nonzero, value);
    }
  }
  vulkan.Device().destroySampler(sampler);
  vulkan.DestroyImage(&texture);
  std::printf("UniformFragmentAfterVote: %zu GPU draw/readbacks passed for guest wave32/64\n",
              fragments.size() * values.size() * draws.size());
}

namespace UniformFragmentMultiwordVoteGpu {
struct Inputs {
  u32 first, last;
};
constexpr std::array inputs{
    Inputs{0x00000000u, 0x00000000u}, Inputs{0x3f800000u, 0x00000000u},
    Inputs{0x00000000u, 0x3f800000u}, Inputs{0x3f800000u, 0x3f800000u},
    Inputs{0x80000000u, 0x3f800000u}, Inputs{0x3f800000u, 0x80000000u},
    Inputs{0x7fc12345u, 0x00000000u}, Inputs{0x00000000u, 0x7fc12345u},
    Inputs{0x7fc12345u, 0x3f800000u}, Inputs{0x3f800000u, 0x7fc12345u},
    Inputs{0x7f800000u, 0xbf800000u}, Inputs{0xbf800000u, 0x7f800000u}};

void Verify(const std::vector<u32> &pixels, const UniformFragmentVoteGpu::Geometry &geometry,
            u32 wave, bool branch_nonzero, Inputs values) {
  constexpr auto name = "UniformFragmentMultiwordVote";
  constexpr u32 width = PrimitiveContract::Width, height = PrimitiveContract::Height;
  Require(name, "readback size", pixels.size() == width * height,
          "multiword fragment readback did not cover the RGBA8 target");
  const bool comparison = (values.first & 0x7fffffffu) != 0 &&
                          (values.last & 0x7fffffffu) != 0;
  const bool taken = branch_nonzero ? comparison : !comparison;
  const u32 first_mip = taken ? 1 : 0;
  const u32 merged_mip = taken ? 2 : 1;
  const auto &colors = UniformFragmentAfterVoteGpu::mip_colors;
  const u32 color = (colors[first_mip] & 0x00ff00ffu) |
                    (colors[merged_mip] & 0x0000ff00u) |
                    (taken ? 0u : 0xff000000u);
  u32 covered_count = 0;
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const float px = float(x) + 0.5f, py = float(y) + 0.5f;
      const bool covered = px >= geometry.x && py >= geometry.y &&
          px + py < geometry.x + geometry.y + geometry.extent;
      const u32 expected = covered ? color : 0;
      covered_count += covered;
      if (pixels[y * width + x] != expected)
        Fail(name, "X4 words/AND/branch/coordinate-Phi/implicit LOD/coverage",
             fmt::format("wave={} branch={} word4=0x{:08x} word7=0x{:08x} "
                         "firstMip={} mergedMip={} geometry={} pixel=({}, {}) "
                         "actual=0x{:08x} expected=0x{:08x}", wave,
                         branch_nonzero ? "VCCNZ" : "VCCZ", values.first, values.last,
                         first_mip, merged_mip, geometry.name, x, y, pixels[y * width + x], expected));
    }
  }
  std::printf("[gpu]     UniformFragmentMultiwordVote wave=%u branch=%s "
              "word4=0x%08x word7=0x%08x firstMip=%u mergedMip=%u geometry=%s covered=%u ok\n",
              wave, branch_nonzero ? "VCCNZ" : "VCCZ", values.first, values.last,
              first_mip, merged_mip, geometry.name, covered_count);
}
} // namespace UniformFragmentMultiwordVoteGpu

void CheckUniformFragmentMultiwordVoteGpu(bool host_only) {
  constexpr auto name = "UniformFragmentMultiwordVote";
  std::vector<CompiledShader> fragments;
  for (u32 wave : {32u, 64u})
    for (bool nonzero : {false, true})
      fragments.push_back(UniformFragmentVoteGpu::Compile(wave, nonzero, false, true, true));
  if (host_only) {
    std::printf("UniformFragmentMultiwordVote: 4 synthetic X4 scalar-load/AND shaders with "
                "after-vote samples and coordinate Phi compiled and SPIR-V validated; GPU not run\n");
    return;
  }
  VulkanHarness vulkan(VulkanHarness::Profile::UniformFragmentVote);
  const auto vertex = AssembleNativeMetalContract(PrimitiveContractSpv::Vertex);
  const auto vertices = UniformFragmentTextureVoteGpu::Vertices(0);
  std::vector<std::vector<u32>> mips;
  for (u32 mip = 0; mip < UniformFragmentAfterVoteGpu::mip_colors.size(); ++mip)
    mips.emplace_back((64u >> mip) * (64u >> mip), UniformFragmentAfterVoteGpu::mip_colors[mip]);
  auto texture = vulkan.CreateImageMips(name, 64, 64,
      vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled, mips, 1,
      vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageType::e2D, vk::ImageViewType::e2D, 1);
  const auto sampler = vulkan.CreateSampler(name, 3, vk::Filter::eNearest);
  std::vector<PrimitiveContractDraw> draws;
  for (u32 geometry = 0; geometry < UniformFragmentVoteGpu::geometries.size(); ++geometry) {
    auto draw = PrimitiveContract::Draw();
    draw.first_vertex = geometry * 3;
    draws.push_back(draw);
  }
  for (u32 shader = 0; shader < fragments.size(); ++shader) {
    const u32 wave = shader < 2 ? 32 : 64;
    const bool nonzero = shader % 2 != 0;
    for (const auto values : UniformFragmentMultiwordVoteGpu::inputs) {
      // Keep both selected words independent and the unused words distinct.
      // Upload happens after all shaders were compiled, preventing CPU folding.
      const std::array constants{0x7fc0abcdu, 0u, 0x3f800000u, 0xbf800000u,
                                 values.first, 0x3f000000u, 0u, values.last};
      const auto pixels = vulkan.RenderPrimitiveContract(
          name, vertex, {}, {}, fragments[shader].spirv, 0,
          vertices, {}, draws, vk::PrimitiveTopology::eTriangleList,
          vk::Format::eR8G8B8A8Unorm, &fragments[shader], constants, &texture, sampler);
      Require(name, "draw completion", pixels.size() == UniformFragmentVoteGpu::geometries.size(),
              "not every multiword geometry produced a readback");
      for (u32 geometry = 0; geometry < UniformFragmentVoteGpu::geometries.size(); ++geometry)
        UniformFragmentMultiwordVoteGpu::Verify(
            pixels[geometry], UniformFragmentVoteGpu::geometries[geometry], wave, nonzero, values);
    }
  }
  vulkan.Device().destroySampler(sampler);
  vulkan.DestroyImage(&texture);
  std::printf("UniformFragmentMultiwordVote: %zu GPU draw/readbacks passed for guest wave32/64\n",
              fragments.size() * UniformFragmentMultiwordVoteGpu::inputs.size() * draws.size());
}
