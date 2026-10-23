// Included by ShaderRecompilerComputeTests.cpp inside its test namespace.
// Reuses the existing Vulkan allocation, draw, synchronization and readback
// harness. Modules and expected values are synthetic, independent of any ROM.

std::vector<u32> AssembleBarycentricGpuFixture(const std::string& source) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string diagnostic;
  tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&,
                               const char* message) { diagnostic += message; });
  std::vector<u32> words;
  const bool assembled = tools.Assemble(source, &words);
  Require("BarycentricGeometry", "assemble synthetic module", assembled, diagnostic);
  ValidateSpirv("BarycentricGeometry source", words);
  return words;
}

std::vector<u32> BarycentricGpuVertex() {
  return AssembleBarycentricGpuFixture(R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %xy %color %position %parameter
OpDecorate %xy Location 0
OpDecorate %color Location 1
OpDecorate %parameter Location 3
OpDecorate %parameter Flat
OpDecorate %perVertex Block
OpMemberDecorate %perVertex 0 BuiltIn Position
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%fzero = OpConstant %float 0
%vec2 = OpTypeVector %float 2
%vec4 = OpTypeVector %float 4
%perVertex = OpTypeStruct %vec4
%input2 = OpTypePointer Input %vec2
%input4 = OpTypePointer Input %vec4
%output4 = OpTypePointer Output %vec4
%outputBlock = OpTypePointer Output %perVertex
%xy = OpVariable %input2 Input
%color = OpVariable %input4 Input
%position = OpVariable %outputBlock Output
%parameter = OpVariable %output4 Output
%main = OpFunction %void None %function
%entry = OpLabel
%p = OpLoad %vec2 %xy
%c = OpLoad %vec4 %color
%x = OpCompositeExtract %float %p 0
%y = OpCompositeExtract %float %p 1
%w = OpCompositeExtract %float %c 3
%clip = OpCompositeConstruct %vec4 %x %y %fzero %w
%positionPointer = OpAccessChain %output4 %position %zero
OpStore %positionPointer %clip
OpStore %parameter %c
OpReturn
OpFunctionEnd
)");
}

std::vector<u32> BarycentricGpuFragment(bool no_perspective, bool per_vertex = false) {
  std::string source = R"(
OpCapability Shader
OpCapability FragmentBarycentricKHR
OpExtension "SPV_KHR_fragment_shader_barycentric"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %bary %ordinary %color
OpExecutionMode %main OriginUpperLeft
OpDecorate %ordinary Location 3
OpDecorate %color Location 0
)";
  source += per_vertex ? "OpDecorate %ordinary PerVertexKHR\n"
                       : "OpDecorate %ordinary Flat\n";
  source += std::string("OpDecorate %bary BuiltIn ") +
      (no_perspective ? "BaryCoordNoPerspKHR\n" : "BaryCoordKHR\n");
  source += R"(
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%vec3 = OpTypeVector %float 3
%vec4 = OpTypeVector %float 4
%array = OpTypeArray %vec4 %three
%input3 = OpTypePointer Input %vec3
%input4 = OpTypePointer Input %vec4
%inputArray = OpTypePointer Input %array
%output4 = OpTypePointer Output %vec4
%bary = OpVariable %input3 Input
)";
  source += per_vertex ? "%ordinary = OpVariable %inputArray Input\n"
                       : "%ordinary = OpVariable %input4 Input\n";
  source += R"(
%color = OpVariable %output4 Output
%main = OpFunction %void None %function
%entry = OpLabel
%coordinates = OpLoad %vec3 %bary
)";
  if (per_vertex) {
    // The raw corner values must survive even though the VS output is Flat.
    // Alpha exercises manual interpolation with the independently lowered basis.
    source += R"(
%pointer0 = OpAccessChain %input4 %ordinary %zero
%pointer1 = OpAccessChain %input4 %ordinary %one
%pointer2 = OpAccessChain %input4 %ordinary %two
%corner0 = OpLoad %vec4 %pointer0
%corner1 = OpLoad %vec4 %pointer1
%corner2 = OpLoad %vec4 %pointer2
%x = OpCompositeExtract %float %corner0 0
%y = OpCompositeExtract %float %corner1 0
%z = OpCompositeExtract %float %corner2 0
%raw = OpCompositeConstruct %vec3 %x %y %z
%canary = OpDot %float %raw %coordinates
)";
  } else {
    source += R"(
%parameter = OpLoad %vec4 %ordinary
%x = OpCompositeExtract %float %coordinates 0
%y = OpCompositeExtract %float %coordinates 1
%z = OpCompositeExtract %float %coordinates 2
%canary = OpCompositeExtract %float %parameter 0
)";
  }
  source += R"(
%pixel = OpCompositeConstruct %vec4 %x %y %z %canary
OpStore %color %pixel
OpReturn
OpFunctionEnd
)";
  return AssembleBarycentricGpuFixture(source);
}

void CheckBarycentricFallbackGpu(VulkanHarness& vulkan) {
  const auto limits = vulkan.BarycentricLimits();
  std::printf("DIRECT_BARYCENTRIC_LIMITS gsInput=%u gsOutput=%u gsTotalOutput=%u gsVertices=%u fsInput=%u\n",
              limits.max_geometry_input_components, limits.max_geometry_output_components,
              limits.max_geometry_total_output_components, limits.max_geometry_output_vertices,
              limits.max_fragment_input_components);
  const auto vertex = BarycentricGpuVertex();
  unsigned cases = 0;
  bool strips_matched = true;
  for (const bool per_vertex : {false, true}) {
  for (const bool no_perspective : {false, true}) {
    const auto source = BarycentricGpuFragment(no_perspective, per_vertex);
    const auto fragment = BarycentricFallback::PrepareFragment(source, limits);
    Require("BarycentricGeometry", "fragment lowering",
            fragment.status == BarycentricFallback::Status::Ready, fragment.reason);
    ValidateSpirv("BarycentricGeometry lowered fragment", fragment.plan.spirv);
    for (const bool last : {false, true}) {
      const auto geometry = BarycentricFallback::BuildGeometry(
          vertex, fragment.plan, BarycentricFallback::Topology::TriangleList, last, limits);
      Require("BarycentricGeometry", "geometry generation", geometry.success, geometry.reason);
      ValidateSpirv("BarycentricGeometry generated GS", geometry.spirv);
      GraphicsCase test;
      const std::string list_name = std::string(per_vertex ? "BarycentricPerVertex" : "Barycentric") +
          (no_perspective ? "Affine" : "Smooth") + (last ? "Last" : "First");
      test.name = list_name.c_str();
      test.provoking_vtx_last = last;
      // NDC vertices (-1,-1), (3,-1), (-1,3), clip W 1,2,4.
      // At NDC (0,0), affine weights are (1/2,1/4,1/4), and smooth
      // weights are (lambda_i/W_i) / sum(lambda/W) = (8/11,2/11,1/11).
      // The distinct flat x values identify the actual provoking vertex.
      const std::array<float, 18> vertices{
          -1.0f, -1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
           6.0f, -2.0f, 2.0f, 0.0f, 0.0f, 2.0f,
          -4.0f, 12.0f, 3.0f, 0.0f, 0.0f, 4.0f};
      for (const auto value : vertices) test.vertices.push_back(std::bit_cast<u32>(value));
      CompiledShader compiled;
      compiled.spirv = fragment.plan.spirv;
      const auto actual = vulkan.RenderFragment(test, compiled, &vertex, &geometry.spirv);
      Require(test.name, "readback pixel count", actual.size() == 4,
              "one RGBA32F pixel must be returned");
      std::array<float, 4> expected = no_perspective
          ? std::array<float, 4>{0.5f, 0.25f, 0.25f, last ? 3.0f : 1.0f}
          : std::array<float, 4>{8.0f / 11.0f, 2.0f / 11.0f, 1.0f / 11.0f, last ? 3.0f : 1.0f};
      if (per_vertex) expected = {1.0f, 2.0f, 3.0f, no_perspective ? 1.75f : 15.0f / 11.0f};
      for (size_t component = 0; component < expected.size(); ++component) {
        const float observed = std::bit_cast<float>(actual[component]);
        std::printf("DIRECT_BARYCENTRIC_READBACK case=%s component=%zu expected=%.9g actual=%.9g\n",
                    test.name, component, expected[component], observed);
        Require(test.name, "numeric barycentric/vertex readback",
                std::isfinite(observed) && std::abs(observed - expected[component]) <= 0.0001f,
                "coordinate basis, perspective correction or provoking vertex changed");
      }
      ++cases;

      for (const bool restart : {false, true}) {
        const std::string name = list_name + (restart ? "StripRestart" : "Strip");
        GraphicsCase strip;
        strip.name = name.c_str();
        strip.provoking_vtx_last = last;
        strip.width = 4;
        if (restart) {
          // One clipped primitive precedes restart: the visible strip must
          // restart its even/odd order despite a nonzero global primitive ID.
          const std::array<float, 18> offscreen{
              3.0f, -1.0f, 5.0f, 0.0f, 0.0f, 1.0f,
              3.0f,  1.0f, 6.0f, 0.0f, 0.0f, 1.0f,
              5.0f, -1.0f, 7.0f, 0.0f, 0.0f, 1.0f};
          for (const auto value : offscreen) strip.vertices.push_back(std::bit_cast<u32>(value));
          strip.indices = {0, 1, 2, UINT32_MAX, 3, 4, 5, 6};
        }
        // Four NDC corners (-1,-1),(-1,1),(1,-1),(1,1), with W=1,2,4,8.
        // Pixel centers x=-3/4,-1/4,+1/4,+3/4 sample both triangles away
        // from their shared edge. Distinct flat values identify all vertices.
        const std::array<float, 24> quad{
            -1.0f, -1.0f, 11.0f, 0.0f, 0.0f, 1.0f,
            -2.0f,  2.0f, 22.0f, 0.0f, 0.0f, 2.0f,
             4.0f, -4.0f, 33.0f, 0.0f, 0.0f, 4.0f,
             8.0f,  8.0f, 44.0f, 0.0f, 0.0f, 8.0f};
        for (const auto value : quad) strip.vertices.push_back(std::bit_cast<u32>(value));
        // Use the production assembly helper. Expected pixels below remain
        // analytic and independent of the generated list and GS input order.
        auto triangles = restart
            ? Libs::Graphics::PrimitiveAssembly::TriangleStripToList(
                  {reinterpret_cast<const uint8_t*>(strip.indices.data()),
                   strip.indices.size() * sizeof(u32)},
                  static_cast<u32>(strip.indices.size()), sizeof(u32), last, true)
            : Libs::Graphics::PrimitiveAssembly::TriangleStripToListAuto(4, last);
        Require(strip.name, "canonical triangle-strip assembly", triangles.success, triangles.reason);
        strip.indices = std::move(triangles.indices);
        const auto pixels = vulkan.RenderFragment(strip, compiled, &vertex, &geometry.spirv);
        Require(strip.name, "strip readback pixel count", pixels.size() == 16,
                "four RGBA32F pixels must be returned");
        constexpr std::array<std::array<float, 3>, 4> affine{{
            {0.375f, 0.5f, 0.125f}, {0.125f, 0.5f, 0.375f},
            {0.375f, 0.125f, 0.5f}, {0.125f, 0.375f, 0.5f}}};
        constexpr std::array<std::array<unsigned, 3>, 2> order{{{0, 1, 2}, {1, 3, 2}}};
        constexpr std::array<float, 4> clip_w{1.0f, 2.0f, 4.0f, 8.0f};
        bool matched = true;
        for (size_t pixel = 0; pixel < affine.size(); ++pixel) {
          const auto triangle = pixel / 2;
          auto weights = affine[pixel];
          auto vertices = order[triangle];
          // Vulkan's provoking-dependent barycentric convention:
          // odd strips use [i,i+2,i+1] for FIRST and [i+1,i,i+2] for LAST.
          // Both preserve winding, with the provoking vertex in slot 0 or 2.
          if (last && triangle == 1) {
            weights = {affine[pixel][2], affine[pixel][0], affine[pixel][1]};
            vertices = {2, 1, 3};
          }
          std::array<float, 4> expected_pixel{
              weights[0], weights[1], weights[2],
              triangle == 0 ? (last ? 33.0f : 11.0f) : (last ? 44.0f : 22.0f)};
          if (!no_perspective) {
            float denominator = 0;
            for (size_t component = 0; component < 3; ++component) {
              expected_pixel[component] /= clip_w[vertices[component]];
              denominator += expected_pixel[component];
            }
            for (size_t component = 0; component < 3; ++component)
              expected_pixel[component] /= denominator;
          }
          if (per_vertex) {
            const std::array<float, 3> raw{
                11.0f * (vertices[0] + 1), 11.0f * (vertices[1] + 1),
                11.0f * (vertices[2] + 1)};
            const float interpolated = raw[0] * expected_pixel[0] +
                raw[1] * expected_pixel[1] + raw[2] * expected_pixel[2];
            expected_pixel = {raw[0], raw[1], raw[2], interpolated};
          }
          for (size_t component = 0; component < expected_pixel.size(); ++component) {
            const float observed = std::bit_cast<float>(pixels[pixel * 4 + component]);
            std::printf("DIRECT_BARYCENTRIC_READBACK case=%s pixel=%zu triangle=%zu component=%zu expected=%.9g actual=%.9g\n",
                        strip.name, pixel, triangle, component, expected_pixel[component], observed);
            matched &= std::isfinite(observed) &&
                std::abs(observed - expected_pixel[component]) <= 0.0001f;
          }
        }
        strips_matched &= matched;
        ++cases;
      }
    }
  }
  }
  Require("BarycentricGeometry", "numeric strip barycentric/flat readback", strips_matched,
          "strip order, perspective correction, provoking vertex or restart changed");
  std::printf("[gpu]     DirectBarycentricFallback ok (%u cases, list/canonical-strip/restart, different clip W, first/last flat and raw PerVertex corners)\n", cases);
}
