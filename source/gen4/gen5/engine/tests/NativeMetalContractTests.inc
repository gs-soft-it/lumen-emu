// Included inside ShaderRecompilerComputeTests.cpp's test namespace.
// These bounded contracts execute translated SPIR-V through the native backend;
// every success is conditional on CPU-observed GPU results.
std::vector<u32> AssembleNativeMetalContract(const char *source) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string diagnostic;
  tools.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &,
                               const char *message) { diagnostic += message; });
  std::vector<u32> words;
  Require("NativeMetalContract", "assemble", tools.Assemble(source, &words), diagnostic);
  ValidateSpirv("NativeMetalContract", words);
  return words;
}

std::vector<u32> NativeMetalBarycentricAdmissionFixture(
    const char *decoration = "", bool both_coordinates = false,
    const char *interpolation = "", bool existing_fragcoord = false,
    bool structured_fragcoord = false) {
  const bool explicit_interpolation = interpolation[0] != '\0';
  const bool uses_fragcoord = existing_fragcoord || structured_fragcoord;
  std::string source = "OpCapability Shader\nOpCapability FragmentBarycentricKHR\n";
  if (std::strcmp(decoration, "Sample") == 0)
    source += "OpCapability SampleRateShading\n";
  if (explicit_interpolation) source += "OpCapability InterpolationFunction\n";
  source += "OpExtension \"SPV_KHR_fragment_shader_barycentric\"\n";
  if (explicit_interpolation) source += "%glsl = OpExtInstImport \"GLSL.std.450\"\n";
  source += "OpMemoryModel Logical GLSL450\n"
            "OpEntryPoint Fragment %main \"main\" %bary %color";
  if (both_coordinates) source += " %affine";
  if (uses_fragcoord) source += " %frag_coord";
  source += "\nOpExecutionMode %main OriginUpperLeft\n"
            "OpDecorate %bary BuiltIn BaryCoordKHR\n"
            "OpDecorate %color Location 0\n";
  if (decoration[0] != '\0')
    source += std::string("OpDecorate %bary ") + decoration + "\n";
  if (both_coordinates) source += "OpDecorate %affine BuiltIn BaryCoordNoPerspKHR\n";
  if (structured_fragcoord)
    source += "OpDecorate %frag_coord_block Block\n"
              "OpMemberDecorate %frag_coord_block 0 BuiltIn FragCoord\n";
  else if (existing_fragcoord) source += "OpDecorate %frag_coord BuiltIn FragCoord\n";
  source += R"(
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%vec2 = OpTypeVector %float 2
%vec3 = OpTypeVector %float 3
%vec4 = OpTypeVector %float 4
%offset = OpConstantComposite %vec2 %fzero %fzero
%input3 = OpTypePointer Input %vec3
%output4 = OpTypePointer Output %vec4
)";
  if (uses_fragcoord) source += "%input4 = OpTypePointer Input %vec4\n";
  if (structured_fragcoord)
    source += "%frag_coord_block = OpTypeStruct %vec4\n"
              "%inputBlock = OpTypePointer Input %frag_coord_block\n";
  source += R"(
%bary = OpVariable %input3 Input
%color = OpVariable %output4 Output
)";
  if (both_coordinates) source += "%affine = OpVariable %input3 Input\n";
  if (structured_fragcoord) source += "%frag_coord = OpVariable %inputBlock Input\n";
  else if (existing_fragcoord) source += "%frag_coord = OpVariable %input4 Input\n";
  source += "%main = OpFunction %void None %fn\n%entry = OpLabel\n";
  if (explicit_interpolation) {
    source += std::string("%coordinates = OpExtInst %vec3 %glsl ") + interpolation + " %bary";
    if (std::strcmp(interpolation, "InterpolateAtSample") == 0) source += " %zero";
    if (std::strcmp(interpolation, "InterpolateAtOffset") == 0) source += " %offset";
    source += "\n";
  } else {
    source += "%coordinates = OpLoad %vec3 %bary\n";
  }
  if (both_coordinates) {
    source += "%other = OpLoad %vec3 %affine\n"
              "%combined = OpFAdd %vec3 %coordinates %other\n";
  }
  const char *result = both_coordinates ? "%combined" : "%coordinates";
  for (u32 component = 0; component < 3; ++component)
    source += "%c" + std::to_string(component) + " = OpCompositeExtract %float " +
              result + " " + std::to_string(component) + "\n";
  if (structured_fragcoord)
    source += "%coord_ptr = OpAccessChain %input4 %frag_coord %zero\n"
              "%coord = OpLoad %vec4 %coord_ptr\n";
  else if (existing_fragcoord) source += "%coord = OpLoad %vec4 %frag_coord\n";
  if (uses_fragcoord) source += "%reciprocal_w = OpCompositeExtract %float %coord 3\n";
  source += std::string("%pixel = OpCompositeConstruct %vec4 %c0 %c1 %c2 ") +
            (uses_fragcoord ? "%reciprocal_w\n" : "%fone\n") +
            "OpStore %color %pixel\nOpReturn\nOpFunctionEnd\n";
  return AssembleNativeMetalContract(source.c_str());
}

void VulkanHarness::CheckNativeMetalContract() {
  constexpr const char *name = "NativeMetalContract";
  constexpr u32 count = 64;
  constexpr u32 guard = 0xbad0cafe;
  std::vector<u32> initial(count + 2, guard);
  for (u32 i = 0; i < count; ++i) initial[i + 1] = 0x1000 + i;
  auto output = CreateStorageBuffer(name, initial, initial.size());
  const auto compute_spirv = AssembleNativeMetalContract(R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %global %output
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %global BuiltIn GlobalInvocationId
OpDecorate %words ArrayStride 4
OpMemberDecorate %block 0 Offset 0
OpDecorate %block Block
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%vec3 = OpTypeVector %uint 3
%in = OpTypePointer Input %vec3
%global = OpVariable %in Input
%words = OpTypeRuntimeArray %uint
%block = OpTypeStruct %words
%storage = OpTypePointer StorageBuffer %block
%element = OpTypePointer StorageBuffer %uint
%output = OpVariable %storage StorageBuffer
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%factor = OpConstant %uint 17
%mask = OpConstant %uint 2779096485
%main = OpFunction %void None %fn
%entry = OpLabel
%xyz = OpLoad %vec3 %global
%id = OpCompositeExtract %uint %xyz 0
%index = OpIAdd %uint %id %one
%ptr = OpAccessChain %element %output %zero %index
%input = OpLoad %uint %ptr
%scaled = OpIMul %uint %id %factor
%sum = OpIAdd %uint %input %scaled
%result = OpBitwiseXor %uint %sum %mask
OpStore %ptr %result
OpReturn
OpFunctionEnd
)");
  auto module = CreateShaderModule(name, compute_spirv);
  vk::DescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = vk::DescriptorType::eStorageBuffer;
  binding.descriptorCount = 1;
  binding.stageFlags = vk::ShaderStageFlagBits::eCompute;
  vk::DescriptorSetLayoutCreateInfo descriptor_info{};
  descriptor_info.bindingCount = 1;
  descriptor_info.pBindings = &binding;
  vk::DescriptorSetLayout descriptor_layout{};
  RequireVk(name, "compute", m_device.createDescriptorSetLayout(
      &descriptor_info, nullptr, &descriptor_layout), "vkCreateDescriptorSetLayout");
  vk::PipelineLayoutCreateInfo layout_info{};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &descriptor_layout;
  vk::PipelineLayout layout{};
  RequireVk(name, "compute", m_device.createPipelineLayout(&layout_info, nullptr, &layout),
            "vkCreatePipelineLayout");
  vk::ComputePipelineCreateInfo pipeline_info{};
  pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
  pipeline_info.stage.module = module;
  pipeline_info.stage.pName = "main";
  pipeline_info.layout = layout;
  vk::Pipeline pipeline{};
  RequireVk(name, "compute", m_device.createComputePipelines(nullptr, 1, &pipeline_info,
      nullptr, &pipeline), "vkCreateComputePipelines");
  vk::DescriptorPoolSize pool_size{};
  pool_size.type = vk::DescriptorType::eStorageBuffer;
  pool_size.descriptorCount = 1;
  vk::DescriptorPoolCreateInfo pool_info{};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  vk::DescriptorPool pool{};
  RequireVk(name, "compute", m_device.createDescriptorPool(&pool_info, nullptr, &pool),
            "vkCreateDescriptorPool");
  vk::DescriptorSetAllocateInfo set_info{};
  set_info.descriptorPool = pool;
  set_info.descriptorSetCount = 1;
  set_info.pSetLayouts = &descriptor_layout;
  vk::DescriptorSet set{};
  RequireVk(name, "compute", m_device.allocateDescriptorSets(&set_info, &set),
            "vkAllocateDescriptorSets");
  vk::DescriptorBufferInfo buffer_info{};
  buffer_info.buffer = output.buffer;
  buffer_info.range = output.size;
  vk::WriteDescriptorSet write{};
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = vk::DescriptorType::eStorageBuffer;
  write.pBufferInfo = &buffer_info;
  m_device.updateDescriptorSets(1, &write, 0, nullptr);
  auto cmd = BeginCommands(name, "compute");
  cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, 1, &set, 0, nullptr);
  cmd.dispatch(count, 1, 1);
  vk::MemoryBarrier visible{};
  visible.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
  visible.dstAccessMask = vk::AccessFlagBits::eHostRead;
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
      vk::PipelineStageFlagBits::eHost, {}, 1, &visible, 0, nullptr, 0, nullptr);
  EndSubmitAndFree(name, "compute", cmd);
  auto expected = initial;
  for (u32 i = 0; i < count; ++i) expected[i + 1] = (initial[i + 1] + i * 17) ^ 0xa5a5a5a5;
  Require(name, "compute readback", ReadBuffer(name, output, expected.size()) == expected,
          "compute arithmetic, uploaded inputs, dispatch indexing or buffer guards differed");
  m_device.destroyDescriptorPool(pool);
  m_device.destroyPipeline(pipeline);
  m_device.destroyPipelineLayout(layout);
  m_device.destroyDescriptorSetLayout(descriptor_layout);
  m_device.destroyShaderModule(module);
  DestroyBuffer(&output);

  CompiledShader fragment{};
  fragment.spirv = AssembleNativeMetalContract(R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %input %output
OpExecutionMode %main OriginUpperLeft
OpDecorate %input Location 0
OpDecorate %output Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%vec4 = OpTypeVector %float 4
%in = OpTypePointer Input %vec4
%out = OpTypePointer Output %vec4
%input = OpVariable %in Input
%output = OpVariable %out Output
%main = OpFunction %void None %fn
%entry = OpLabel
%color = OpLoad %vec4 %input
OpStore %output %color
OpReturn
OpFunctionEnd
)");
  GraphicsCase draw{};
  draw.name = name;
  draw.width = 8;
  const auto pixels = RenderFragment(draw, fragment);
  Require(name, "render readback size", pixels.size() == draw.width * 4,
          "draw readback size differs from target extent");
  const std::array<float, 4> color{0.25f, 0.5f, 0.75f, 1.f};
  for (size_t i = 0; i < pixels.size(); ++i) {
    Require(name, "render readback", std::abs(std::bit_cast<float>(pixels[i]) - color[i % 4]) < 0.00001f,
            "vertex fetch, interpolation, fragment output or render target readback differed");
  }
  // Exercise native vertex fetch conversion, not only pipeline acceptance.
  // Keep the 24-byte position/color stride, with NaN padding after each narrow
  // attribute. A wrong offset, width or stride cannot silently read valid color.
  struct NarrowVertexCase {
    const char *name;
    vk::Format format;
    std::array<u32, 4> words;
    std::array<float, 4> expected;
  };
  constexpr u32 poison = 0x7fc00000;
  const std::array<NarrowVertexCase, 6> narrow_vertices{{
      {"NativeMetalVertexHalf4SignedFraction", vk::Format::eR16G16B16A16Sfloat,
       {0xbc003c00, 0x40003800, poison, poison}, {1.f, -1.f, 0.5f, 2.f}},
      {"NativeMetalVertexHalf4FractionZero", vk::Format::eR16G16B16A16Sfloat,
       {0xb8003400, 0x00003a00, poison, poison}, {0.25f, -0.5f, 0.75f, 0.f}},
      {"NativeMetalVertexHalf4Negative", vk::Format::eR16G16B16A16Sfloat,
       {0x4000bc00, 0x3c00b400, poison, poison}, {-1.f, 2.f, -0.25f, 1.f}},
      {"NativeMetalVertexUnorm2Endpoints", vk::Format::eR16G16Unorm,
       {0xffff0000, poison, poison, poison}, {0.f, 1.f, 0.f, 1.f}},
      {"NativeMetalVertexUnorm2SmallMidpoint", vk::Format::eR16G16Unorm,
       {0x80000001, poison, poison, poison}, {1.f / 65535.f, 32768.f / 65535.f, 0.f, 1.f}},
      {"NativeMetalVertexUnorm2Fractions", vk::Format::eR16G16Unorm,
       {0xc0004000, poison, poison, poison}, {16384.f / 65535.f, 49152.f / 65535.f, 0.f, 1.f}},
  }};
  for (const auto &test : narrow_vertices) {
    const auto properties = m_physical_device.getFormatProperties(test.format);
    Require(test.name, "vertex capability",
            static_cast<bool>(properties.bufferFeatures & vk::FormatFeatureFlagBits::eVertexBuffer),
            "native vertex fetch format was not advertised");
    GraphicsCase narrow{};
    narrow.name = test.name;
    narrow.width = 8;
    narrow.vertex_color_format = test.format;
    constexpr std::array<std::array<float, 2>, 3> positions{{
        {-1.f, -1.f}, {3.f, -1.f}, {-1.f, 3.f}}};
    for (const auto &position : positions) {
      narrow.vertices.push_back(std::bit_cast<u32>(position[0]));
      narrow.vertices.push_back(std::bit_cast<u32>(position[1]));
      narrow.vertices.insert(narrow.vertices.end(), test.words.begin(), test.words.end());
    }
    const auto rendered = RenderFragment(narrow, fragment);
    Require(test.name, "readback size", rendered.size() == narrow.width * 4,
            "narrow vertex fetch render target readback has wrong extent");
    for (size_t i = 0; i < rendered.size(); ++i) {
      Require(test.name, "vertex conversion readback",
              std::abs(std::bit_cast<float>(rendered[i]) - test.expected[i % 4]) < 0.000001f,
              "half/UNORM conversion, missing components, vertex stride or padding differed");
    }
  }
  for (const auto format : {vk::Format::eR16G16Snorm, vk::Format::eR16G16Sscaled,
                             vk::Format::eR16G16B16A16Snorm, vk::Format::eR16G16B16A16Sscaled}) {
    Require(name, "unsupported vertex capability",
            !static_cast<bool>(m_physical_device.getFormatProperties(format).bufferFeatures &
                               vk::FormatFeatureFlagBits::eVertexBuffer),
            "unqualified narrow vertex format was advertised");
  }
  // Actual guest EXP streams distinguish transport encoding from attachment
  // numeric class. The same compressed shader must work as float/uint/sint.
  using NumericClass = Prospero::TextureNumericClass;
  const auto typed_code = [](u32 variant) {
    std::vector<u32> code;
    const auto move = [&](u32 reg, u32 bits) { AppendVMovLiteral(&code, reg, bits); };
    const auto exp = [&](u32 slot, bool compressed, u32 first) {
      code.push_back(EncodeExp0(slot, 0xf, slot == 2, compressed, slot == 0));
      code.push_back(EncodeExp1(first, first + 1, compressed ? first : first + 2,
                               compressed ? first + 1 : first + 3));
    };
    if (variant == 0) {
      move(0, 0x80003c00u); move(1, 0x35557bffu);
      move(2, 0x38003400u); move(3, 0x3c003a00u);
      move(4, 0xc0003c00u); move(5, 0x42003800u);
      exp(0, true, 0); exp(1, true, 2); exp(2, true, 4);
    } else if (variant == 1) {
      move(0, 0xff808000u); move(1, 0xffff7fffu);
      move(2, 0x80000000u); move(3, 0xdeadbeefu);
      move(4, 0x12345678u); move(5, 0x7fffffffu);
      move(6, 0x3e800000u); move(7, 0xbf000000u);
      move(8, 0x40000000u); move(9, 0x3f800000u);
      exp(0, true, 0); exp(1, false, 2); exp(2, false, 6);
    } else {
      move(0, 0x80000000u); move(1, 0xffffff80u);
      move(2, 0x00007fffu); move(3, 0xffffffffu);
      move(4, 0x80000000u); move(5, 0x4000ffffu);
      move(6, 0x80018000u); move(7, 0x7fff0000u);
      exp(0, false, 0); exp(1, true, 4); exp(2, true, 6);
    }
    AppendEnd(&code);
    return code;
  };
  std::array<std::vector<u32>, 3> typed_keys;
  std::vector<u32> first_float_spirv;
  for (u32 phase = 0; phase < 6; ++phase) {
    const u32 variant = phase < 4 ? 0u : phase - 3;
    const auto first_class = phase == 1 ? NumericClass::Uint :
        phase == 2 || phase >= 4 ? NumericClass::Sint : NumericClass::Float;
    GraphicsCase typed{};
    typed.name = "NativeMetalTypedFragmentExports";
    typed.width = 4;
    typed.height = 2;
    typed.fragment_code = typed_code(variant);
    typed.pixel_numeric_classes[0] = first_class;
    typed.pixel_numeric_classes[1] = variant == 0 ? NumericClass::Float : NumericClass::Uint;
    typed.pixel_export_mappings[0] = Prospero::ColorMappingAbgr;
    typed.pixel_output_modes[0] = variant == 2 ? 9 : 4;
    typed.pixel_output_modes[1] = variant == 1 ? 9 : 4;
    typed.pixel_output_modes[2] = variant == 0 ? 4 : variant == 1 ? 9 : 6;
    typed.color_target_formats = {
        variant == 0 ? (first_class == NumericClass::Float ? vk::Format::eR16G16B16A16Sfloat :
                        first_class == NumericClass::Uint ? vk::Format::eR16G16B16A16Uint :
                                                           vk::Format::eR16G16B16A16Sint)
                     : vk::Format::eR32G32B32A32Sint,
        variant == 0 ? vk::Format::eR32G32B32A32Sfloat : vk::Format::eR32G32B32A32Uint,
        vk::Format::eR32G32B32A32Sfloat};
    ShaderPixelInputInfo key_input{};
    key_input.target_numeric_class = typed.pixel_numeric_classes;
    key_input.target_export_mapping = typed.pixel_export_mappings;
    std::copy(typed.pixel_output_modes.begin(), typed.pixel_output_modes.end(),
              std::begin(key_input.target_output_mode));
    std::vector<u32> key;
    BuildStageStaticKey(key_input, key);
    const auto compiled = CompileFragmentCase(typed);
    if (phase < 3) {
      typed_keys[phase] = key;
      if (phase == 0) first_float_spirv = compiled.spirv;
    } else if (phase == 3) {
      Require(typed.name, "numeric class cache keys",
              key == typed_keys[0] && key != typed_keys[1] && key != typed_keys[2] &&
                  typed_keys[1] != typed_keys[2] && compiled.spirv == first_float_spirv,
              "float/uint/sint share a static key or restoring float changed its shader");
    }
    std::array<std::vector<u32>, 3> texels;
    if (variant == 0) {
      texels[0] = {0x7bff3555u, 0x3c008000u};
      texels[1] = {0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u};
      texels[2] = {0x3f800000u, 0xc0000000u, 0x3f000000u, 0x40400000u};
    } else if (variant == 1) {
      texels[0] = {0xffffffffu, 0x7fffu, 0xffffff80u, 0xffff8000u};
      texels[1] = {0x80000000u, 0xdeadbeefu, 0x12345678u, 0x7fffffffu};
      texels[2] = {0x3e800000u, 0xbf000000u, 0x40000000u, 0x3f800000u};
    } else {
      texels[0] = {0xffffffffu, 0x7fffu, 0xffffff80u, 0x80000000u};
      texels[1] = {0u, 0x8000u, 0xffffu, 0x4000u};
      texels[2] = {0xbf800000u, 0xbf800000u, 0u, 0x3f800000u};
    }
    std::vector<u32> expected;
    for (const auto &texel : texels) {
      for (u32 i = 0; i < typed.width * typed.height; ++i) {
        expected.insert(expected.end(), texel.begin(), texel.end());
      }
    }
    const auto actual = RenderFragment(typed, compiled);
    Require(typed.name, "typed MRT readback", actual == expected,
            "compressed/raw EXP payload, sign extension, float decoding or typed swizzle differed");
    std::printf("[gpu]     %-32s phase=%u ok (three MRT, raw16/raw32)\n", typed.name, phase);
  }
  // Use the original SPIR-V directly: no geometry fallback or rewritten basis
  // may make native barycentric support appear to pass.
  const auto barycentric_vertex = BarycentricGpuVertex();
  if (m_barycentric_enabled) {
    struct BarycentricGeometry {
      const char *name;
      bool clipped;
      std::array<float, 3> w;
    };
    const std::array<BarycentricGeometry, 5> geometries{{
        {"ClippedW124", true, {1.f, 2.f, 4.f}},
        {"InsideW124", false, {1.f, 2.f, 4.f}},
        {"ClippedW111", true, {1.f, 1.f, 1.f}},
        {"InsideW235", false, {2.f, 3.f, 5.f}},
        {"InsideWFractional", false, {0.5f, 1.25f, 3.5f}},
    }};
    for (bool no_perspective : {false, true}) {
    for (const auto &geometry : geometries) {
      GraphicsCase barycentric{};
      const std::string case_name = std::string("NativeMetalBarycentric") +
          (no_perspective ? "Affine" : "Perspective") + geometry.name;
      barycentric.name = case_name.c_str();
      // The oversized triangle exercises clipping; the interior triangle
      // isolates interpolation. The 1x1 pixel center is NDC (0,0) for both.
      const std::array<std::array<float, 2>, 3> ndc = geometry.clipped ?
          std::array<std::array<float, 2>, 3>{{{-1.f, -1.f}, {3.f, -1.f}, {-1.f, 3.f}}} :
          std::array<std::array<float, 2>, 3>{{{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.f, 0.8f}}};
      const std::array<float, 3> affine = geometry.clipped ?
          std::array<float, 3>{0.5f, 0.25f, 0.25f} :
          std::array<float, 3>{0.25f, 0.25f, 0.5f};
      for (u32 vertex = 0; vertex < 3; ++vertex) {
        const std::array<float, 6> attributes{
            ndc[vertex][0] * geometry.w[vertex], ndc[vertex][1] * geometry.w[vertex],
            static_cast<float>(vertex + 1), 0.f, 0.f, geometry.w[vertex]};
        for (float value : attributes) barycentric.vertices.push_back(std::bit_cast<u32>(value));
      }
      CompiledShader native{};
      native.spirv = BarycentricGpuFragment(no_perspective);
      const auto actual = RenderFragment(barycentric, native, &barycentric_vertex);
      std::array<float, 4> expected{affine[0], affine[1], affine[2], 1.f};
      if (!no_perspective) {
        float denominator = 0.f;
        for (u32 vertex = 0; vertex < 3; ++vertex) {
          expected[vertex] /= geometry.w[vertex];
          denominator += expected[vertex];
        }
        for (u32 vertex = 0; vertex < 3; ++vertex) expected[vertex] /= denominator;
      }
      Require(barycentric.name, "readback extent", actual.size() == expected.size(),
              "native barycentric pixel has wrong extent");
      for (u32 component = 0; component < expected.size(); ++component)
        Require(barycentric.name, "native barycentric readback",
                std::abs(std::bit_cast<float>(actual[component]) - expected[component]) < 0.0001f,
                "component=" + std::to_string(component) +
                " expected=" + std::to_string(expected[component]) +
                " actual=" + std::to_string(std::bit_cast<float>(actual[component])));
      std::printf("[gpu]     %-32s ok (native builtin, clip W=%g/%g/%g)\n",
                  barycentric.name, geometry.w[0], geometry.w[1], geometry.w[2]);
    }
    }
    // A shader that already consumes FragCoord must reuse its standalone
    // input. Alpha observes reciprocal W independently of the barycentric RGB.
    GraphicsCase reuse{};
    reuse.name = "NativeMetalBarycentricFragCoordReuse";
    const std::array<float, 18> reuse_vertices{
        -1.f, -1.f, 1.f, 0.f, 0.f, 1.f,
         6.f, -2.f, 2.f, 0.f, 0.f, 2.f,
        -4.f, 12.f, 3.f, 0.f, 0.f, 4.f};
    for (float value : reuse_vertices) reuse.vertices.push_back(std::bit_cast<u32>(value));
    CompiledShader reuse_fragment{};
    reuse_fragment.spirv = NativeMetalBarycentricAdmissionFixture("", false, "", true);
    const auto reuse_actual = RenderFragment(reuse, reuse_fragment, &barycentric_vertex);
    const std::array<float, 4> reuse_expected{8.f / 11.f, 2.f / 11.f, 1.f / 11.f, 11.f / 16.f};
    Require(reuse.name, "readback extent", reuse_actual.size() == reuse_expected.size(),
            "FragCoord reuse pixel has wrong extent");
    for (u32 component = 0; component < reuse_expected.size(); ++component)
      Require(reuse.name, "FragCoord reuse readback",
              std::abs(std::bit_cast<float>(reuse_actual[component]) - reuse_expected[component]) < 0.0001f,
              "component=" + std::to_string(component) +
              " expected=" + std::to_string(reuse_expected[component]) +
              " actual=" + std::to_string(std::bit_cast<float>(reuse_actual[component])));
    std::printf("[gpu]     %-32s ok (existing FragCoord, reciprocal W alpha)\n", reuse.name);
  } else {
    std::printf("[host]    NativeMetalBarycentric          unsupported; positive draws skipped\n");
  }
  const auto require_rejected = [&](vk::Device device, const char *label,
                                    const std::vector<u32> &words) {
    vk::ShaderModuleCreateInfo info{};
    info.codeSize = words.size() * sizeof(u32);
    info.pCode = words.data();
    vk::ShaderModule rejected{};
    const auto result = device.createShaderModule(&info, nullptr, &rejected);
    const bool null_module = rejected == nullptr;
    if (!null_module) device.destroyShaderModule(rejected);
    Require(name, label, result == vk::Result::eErrorFeatureNotPresent && null_module,
            "unsupported barycentric interface must fail before Metal compilation with a null module");
  };
  if (m_barycentric_enabled) {
    require_rejected(m_device, "PerVertexKHR admission", BarycentricGpuFragment(false, true));
    require_rejected(m_device, "both barycentric builtins admission",
                     NativeMetalBarycentricAdmissionFixture("", true));
    require_rejected(m_device, "structured FragCoord admission",
                     NativeMetalBarycentricAdmissionFixture("", false, "", false, true));
    for (const char *decoration : {"Centroid", "Sample"})
      require_rejected(m_device, decoration, NativeMetalBarycentricAdmissionFixture(decoration));
    for (const char *interpolation : {"InterpolateAtCentroid", "InterpolateAtSample", "InterpolateAtOffset"})
      require_rejected(m_device, interpolation,
                       NativeMetalBarycentricAdmissionFixture("", false, interpolation));
    std::printf("[host]    NativeMetalBarycentricAdmission ok (eight unsupported interfaces)\n");
  }
  // Capability availability alone is insufficient: a second device deliberately
  // omits both the extension and its feature, while the main device stays enabled.
  const float priority = 1.f;
  vk::DeviceQueueCreateInfo queue_info{};
  queue_info.queueFamilyIndex = m_queue_family;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;
  vk::DeviceCreateInfo disabled_info{};
  disabled_info.queueCreateInfoCount = 1;
  disabled_info.pQueueCreateInfos = &queue_info;
  vk::Device disabled_device{};
  RequireVk(name, "barycentric disabled device",
            m_physical_device.createDevice(&disabled_info, nullptr, &disabled_device), "vkCreateDevice");
  for (bool no_perspective : {false, true})
    require_rejected(disabled_device, "barycentric feature not enabled",
                     BarycentricGpuFragment(no_perspective));
  disabled_device.destroy();
  std::printf("[host]    NativeMetalBarycentricDisabled  ok (both builtins rejected)\n");

  for (u32 selected_viewport : {0u, 1u}) {
    std::string vertex_source = R"(
OpCapability Shader
OpCapability MultiViewport
OpCapability ShaderViewportIndex
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %xy %color %position %parameter %viewport
OpDecorate %xy Location 0
OpDecorate %color Location 1
OpDecorate %parameter Location 0
OpDecorate %viewport BuiltIn ViewportIndex
OpDecorate %perVertex Block
OpMemberDecorate %perVertex 0 BuiltIn Position
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 1
%zero = OpConstant %int 0
%viewport_number = OpConstant %int 0
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%vec2 = OpTypeVector %float 2
%vec4 = OpTypeVector %float 4
%perVertex = OpTypeStruct %vec4
%input2 = OpTypePointer Input %vec2
%input4 = OpTypePointer Input %vec4
%output4 = OpTypePointer Output %vec4
%outputInt = OpTypePointer Output %int
%outputBlock = OpTypePointer Output %perVertex
%xy = OpVariable %input2 Input
%color = OpVariable %input4 Input
%position = OpVariable %outputBlock Output
%parameter = OpVariable %output4 Output
%viewport = OpVariable %outputInt Output
%main = OpFunction %void None %fn
%entry = OpLabel
%p = OpLoad %vec2 %xy
%c = OpLoad %vec4 %color
%x = OpCompositeExtract %float %p 0
%y = OpCompositeExtract %float %p 1
%pos = OpCompositeConstruct %vec4 %x %y %fzero %fone
%pos_ptr = OpAccessChain %output4 %position %zero
OpStore %pos_ptr %pos
OpStore %parameter %c
OpStore %viewport %viewport_number
OpReturn
OpFunctionEnd
)";
    const std::string marker = "%viewport_number = OpConstant %int 0";
    vertex_source.replace(vertex_source.find(marker), marker.size(),
        "%viewport_number = OpConstant %int " + std::to_string(selected_viewport));
    const auto vertex = AssembleNativeMetalContract(vertex_source.c_str());
    GraphicsCase viewports{};
    viewports.name = "NativeMetalViewportRanges";
    viewports.width = 8;
    viewports.height = 4;
    viewports.viewports = {{0, 0, 4, 4, 0, 1}, {4, 4, 4, -4, 0, 1}};
    viewports.scissors = {{{1, 0}, {2, 4}}, {{5, 0}, {2, 4}}};
    // Two triangles fill only NDC y[-1,0]. A negative-height viewport must
    // move this half to the opposite two target rows without moving its peer.
    const std::array<std::array<float, 2>, 6> positions{{
        {-1, -1}, {1, -1}, {-1, 0}, {1, -1}, {1, 0}, {-1, 0}}};
    for (const auto &position : positions) {
      viewports.vertices.push_back(std::bit_cast<u32>(position[0]));
      viewports.vertices.push_back(std::bit_cast<u32>(position[1]));
      for (float component : color) viewports.vertices.push_back(std::bit_cast<u32>(component));
    }
    const auto rendered = RenderFragment(viewports, fragment, &vertex);
    Require(name, "viewport readback size", rendered.size() == 8 * 4 * 4,
            "multi-viewport render target readback has wrong extent");
    for (u32 y = 0; y < 4; ++y) {
      for (u32 x = 0; x < 8; ++x) {
        const bool covered = selected_viewport == 0 ?
            (x >= 1 && x < 3 && y < 2) : (x >= 5 && x < 7 && y >= 2);
        for (u32 channel = 0; channel < 4; ++channel) {
          const float wanted = covered ? color[channel] : 0.f;
          Require(name, "viewport/scissor partial update",
                  std::abs(std::bit_cast<float>(rendered[(y * 8 + x) * 4 + channel]) - wanted) < 0.00001f,
                  "shader viewport selection, first=1 state, scissor or negative height was lost");
        }
      }
    }
  }
  std::printf("[gpu]     %-32s ok (compute=%u guarded lanes, draw=%u RGBA pixels, two viewport orientations)\n",
              name, count, draw.width);
}

void VulkanHarness::CheckNullDescriptorFallback() {
  constexpr const char *name = "NullDescriptorFallback";
  EnsureRuntimeContext();
  Require(name, "capability", !m_runtime_context.null_descriptor_enabled &&
              m_runtime_context.null_descriptor_fallback_enabled,
          "fixture requires the production shader fallback policy");
  // Validate the software bounds branch even on hosts whose native image
  // robustness was negotiated. These are explicitly CPU compiler checks.
  for (auto bounds : {ImageLoadR32UintUsesIntegerSampledImage(),
                      ImageStoreR32FloatUsesFormatlessStorageImage(),
                      ImageAtomicSwapReturnsPreviousTexel()}) {
    bounds.explicit_image_bounds = true;
    if (!bounds.has_user_data) {
      bounds.user_data = MakeStorageTextureData(Prospero::BufferFormat::k32Float);
      bounds.has_user_data = true;
    }
    bounds.required_spirv.push_back("OpImageQuerySize");
    (void)CompileCase(bounds, SubgroupSize());
  }
  std::printf("[host]    %-32s ok (load/store/atomic SPIR-V validated)\n",
              "ExplicitImageBoundsCompilation");
  auto &scheduler = Renderer().GetCommandScheduler();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  const std::vector<u32> real_contents{0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000};
  for (bool storage : {false, true}) {
    TestCase test{};
    test.name = storage ? "NullStorageReadWriteIsolation" : "NullSampledReadIsolation";
    test.null_descriptor_fallback = true;
    test.explicit_image_bounds = true;
    test.has_user_data = true;
    test.has_compute_info = true;
    test.compute_info.wave_size = 32;
    test.compute_info.threads_num[0] = 1;
    test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
    test.compute_info.lds_size_dwords = 0;
    test.initial.assign(11, 0xfeedface);
    test.require_storage_read_write_source = storage;
    // s[0:7] is null; s[8:15] is an independent real descriptor. The
    // output buffer descriptor lives at s[48:51], as in production fixtures.
    const auto real = MakeSampledTextureData(Prospero::BufferFormat::k32_32_32_32Float);
    std::copy_n(real.begin(), 8, test.user_data.begin() + 8);
    test.user_data[11] |= DstSel(4, 5, 6, 7);
    const auto buffer = MakeStructuredStorageBufferData(0, test.initial.size() * sizeof(u32));
    std::copy_n(buffer.begin(), 4, test.user_data.begin() + 48);
    AppendVMovU32(&test.code, 20, 0);
    AppendVMovU32(&test.code, 21, 0);
    if (storage) {
      for (u32 i = 0; i < 4; ++i) AppendVMovLiteral(&test.code, i, 0x41000000 + i);
      test.code.push_back(EncodeMimg0(0x08, 0xf));
      test.code.push_back(EncodeMimg1(0, 20));
      // IMAGE_LOAD is a sampled resource in the guest IR, even for the same
      // descriptor. An atomic swap provides an actual storage read/modify/write
      // and makes its returned old value observable independently of that load.
      AppendVMovLiteral(&test.code, 8, 0x12345678);
      test.code.push_back(EncodeMimg0(0x0f, 0x1, 0, true));
      test.code.push_back(EncodeMimg1(8, 20));
    }
    test.code.push_back(EncodeMimg0(0x00, 0xf));
    test.code.push_back(EncodeMimg1(0, 20));
    test.code.push_back(EncodeMimg0(0x00, 0xf));
    test.code.push_back(EncodeMimg1(4, 20, 2));
    for (u32 i = 0; i < 8; ++i) AppendStoreVgpr(&test.code, i, i + 1);
    if (storage) AppendStoreVgpr(&test.code, 8, 9);
    AppendEnd(&test.code);
    test.required_spirv = {"OpImageFetch"};
    if (storage) test.forbidden_spirv = {"OpImageWrite", "OpAtomicExchange", "OpImageTexelPointer"};
    auto compiled = CompileCase(test, SubgroupSize());
    Require(name, "materialized null specialization",
            std::ranges::any_of(compiled.program.info.images,
                [](const auto &image) { return image.null_descriptor; }) &&
            std::ranges::any_of(compiled.program.info.images,
                [](const auto &image) { return !image.null_descriptor; }),
            "materialization failed to distinguish null and real image resources");
    if (storage) {
      Require(name, "storage read/write specialization",
              std::ranges::any_of(compiled.program.info.images, [](const auto &image) {
                return image.null_descriptor && image.read && image.written &&
                    image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Storage;
              }), "fixture did not exercise a null storage image with both reads and writes");
    }
    auto native_test = test;
    native_test.null_descriptor_fallback = false;
    native_test.forbidden_spirv.clear();
    const auto native = CompileCase(native_test, SubgroupSize());
    Require(name, "specialization separation", compiled.spirv != native.spirv,
            "native null descriptors and shader fallback produced the same shader");
    auto real_image = CreateImage2D(name, 1, 1, vk::Format::eR32G32B32A32Sfloat,
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
        real_contents, 4, vk::ImageLayout::eGeneral);
    std::vector<Image> images(compiled.program.info.images.size(), real_image);
    auto output = CreateStorageBuffer(name, test.initial, test.initial.size());
    Dispatch(test, compiled, output, nullptr, nullptr, nullptr, nullptr, nullptr, images);
    auto expected = test.initial;
    std::fill_n(expected.begin() + 1, 4, 0);
    std::copy(real_contents.begin(), real_contents.end(), expected.begin() + 5);
    if (storage) expected[9] = 0;
    CompareWords(test, "null zero reads and real descriptor isolation", expected,
                 ReadBuffer(name, output, expected.size()));
    Require(name, "real image contents", ReadImage(name, &real_image) == real_contents,
            "null storage write modified a real resource");
    DestroyBuffer(&output);
    DestroyImage(&real_image);
    std::printf("[gpu]     %-32s ok\n", test.name);
  }
  scheduler.Finish();
}
