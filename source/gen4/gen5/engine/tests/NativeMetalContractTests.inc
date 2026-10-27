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
