// GPU stage contracts use software vertex fetch, as Gen5 shaders do. The same
// pipeline, descriptors, images and buffers serve several draws in one submission.
#include "NativeMetalPrimitiveShaders.inc"

namespace PrimitiveContract {
constexpr u32 Width = 16;
constexpr u32 Height = 16;
constexpr u32 Guard = 0xd15ca11u;
constexpr std::array<float, 4> Color{0.25f, 0.5f, 0.75f, 1.f};

PrimitiveContractDraw Draw(float level = 1.f, bool curved = false, u32 cull_mode = 1) {
  PrimitiveContractDraw draw;
  draw.push = {std::bit_cast<u32>(level), std::bit_cast<u32>(1.f), 0,
               std::bit_cast<u32>(curved ? 1.f : 0.f), cull_mode, 0, 0, 0};
  return draw;
}

std::vector<u32> Vertices(std::span<const std::array<float, 2>> positions,
                          bool uv = false) {
  std::vector<u32> words;
  for (const auto &position : positions) {
    for (const auto value : std::array{position[0], position[1], 0.f, 1.f})
      words.push_back(std::bit_cast<u32>(value));
    auto color = Color;
    if (uv) color = {(position[0] + 0.75f) / 1.5f, (position[1] + 0.75f) / 1.5f, 0.f, 1.f};
    for (const auto value : color) words.push_back(std::bit_cast<u32>(value));
  }
  return words;
}

std::vector<u32> Triangle() {
  constexpr std::array<std::array<float, 2>, 3> positions{{{-1, -1}, {3, -1}, {-1, 3}}};
  return Vertices(positions);
}

void Solid(const char *name, const std::vector<u32> &pixels,
           const std::array<float, 4> &expected) {
  Require(name, "readback size", pixels.size() == Width * Height * 4,
          "primitive readback did not cover the complete target");
  for (size_t i = 0; i < pixels.size(); ++i) {
    if (std::abs(std::bit_cast<float>(pixels[i]) - expected[i % 4]) > 0.0001f ||
        !std::isfinite(std::bit_cast<float>(pixels[i]))) {
      Fail(name, "complete primitive coverage",
           fmt::format("pixel={} component={} actual={} expected={}", i / 4, i % 4,
                       std::bit_cast<float>(pixels[i]), expected[i % 4]));
    }
  }
}
} // namespace PrimitiveContract

std::vector<std::vector<u32>> VulkanHarness::RenderPrimitiveContract(
    const char *name, const std::vector<u32> &vertex, const std::vector<u32> &control,
    const std::vector<u32> &evaluation, const std::vector<u32> &fragment,
    u32 patch_control_points, const std::vector<u32> &vertices,
    const std::vector<u32> &indices, std::span<const PrimitiveContractDraw> draws) {
  using namespace PrimitiveContract;
  const bool tessellated = !control.empty();
  Require(name, "stage contract", !draws.empty() && vertices.size() % 8 == 0 &&
              (tessellated == !evaluation.empty()) &&
              (!tessellated || (m_enabled_base_features.tessellationShader && patch_control_points != 0)),
          "incomplete stages, malformed vertices or tessellation feature not enabled");
  std::vector<vk::ShaderModule> modules;
  std::vector<vk::PipelineShaderStageCreateInfo> stages;
  const auto AddStage = [&](vk::ShaderStageFlagBits stage, const std::vector<u32> &code) {
    ValidateSpirv(name, code);
    modules.push_back(CreateShaderModule(name, code));
    vk::PipelineShaderStageCreateInfo info{};
    info.stage = stage;
    info.module = modules.back();
    info.pName = "main";
    stages.push_back(info);
  };
  AddStage(vk::ShaderStageFlagBits::eVertex, vertex);
  if (tessellated) {
    AddStage(vk::ShaderStageFlagBits::eTessellationControl, control);
    AddStage(vk::ShaderStageFlagBits::eTessellationEvaluation, evaluation);
  }
  AddStage(vk::ShaderStageFlagBits::eFragment, fragment);

  // Keep a real descriptor offset and canaries around vertex/index input and
  // each image readback, so internal tessellation work cannot borrow their bytes.
  std::vector<u32> guarded_vertices(4, Guard);
  guarded_vertices.insert(guarded_vertices.end(), vertices.begin(), vertices.end());
  guarded_vertices.insert(guarded_vertices.end(), 4, Guard);
  auto vertex_buffer = CreateStorageBuffer(name, guarded_vertices, guarded_vertices.size());
  std::vector<u32> guarded_indices(4, Guard);
  guarded_indices.insert(guarded_indices.end(), indices.begin(), indices.end());
  guarded_indices.insert(guarded_indices.end(), 4, Guard);
  auto index_buffer = CreateHostBuffer(name, guarded_indices.size() * sizeof(u32),
      vk::BufferUsageFlagBits::eIndexBuffer, guarded_indices);
  vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eStorageBuffer, 1,
                                         vk::ShaderStageFlagBits::eVertex};
  vk::DescriptorSetLayoutCreateInfo set_layout_info{};
  set_layout_info.bindingCount = 1;
  set_layout_info.pBindings = &binding;
  vk::DescriptorSetLayout set_layout{};
  RequireVk(name, "descriptor layout", m_device.createDescriptorSetLayout(
      &set_layout_info, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
  vk::ShaderStageFlags push_stages = vk::ShaderStageFlagBits::eVertex;
  if (tessellated) push_stages |= vk::ShaderStageFlagBits::eTessellationControl |
                                vk::ShaderStageFlagBits::eTessellationEvaluation;
  const vk::PushConstantRange push_range{push_stages, 0, 8 * sizeof(u32)};
  vk::PipelineLayoutCreateInfo layout_info{};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &push_range;
  vk::PipelineLayout layout{};
  RequireVk(name, "pipeline layout", m_device.createPipelineLayout(&layout_info, nullptr, &layout),
            "vkCreatePipelineLayout");
  const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eStorageBuffer, 1};
  vk::DescriptorPoolCreateInfo pool_info{};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  vk::DescriptorPool pool{};
  RequireVk(name, "descriptor pool", m_device.createDescriptorPool(&pool_info, nullptr, &pool),
            "vkCreateDescriptorPool");
  vk::DescriptorSetAllocateInfo allocate_info{};
  allocate_info.descriptorPool = pool;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &set_layout;
  vk::DescriptorSet set{};
  RequireVk(name, "descriptor set", m_device.allocateDescriptorSets(&allocate_info, &set),
            "vkAllocateDescriptorSets");
  const vk::DescriptorBufferInfo source_info{vertex_buffer.buffer, 4 * sizeof(u32),
                                            vertices.size() * sizeof(u32)};
  vk::WriteDescriptorSet write{};
  write.dstSet = set;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = vk::DescriptorType::eStorageBuffer;
  write.pBufferInfo = &source_info;
  m_device.updateDescriptorSets(1, &write, 0, nullptr);

  auto target = CreateImageMips(name, Width, Height, vk::Format::eR32G32B32A32Sfloat,
      vk::ImageUsageFlagBits::eColorAttachment, {}, 4, vk::ImageLayout::eGeneral,
      vk::ImageType::e2D, vk::ImageViewType::e2D, 1);
  vk::PipelineVertexInputStateCreateInfo vertex_input{};
  vk::PipelineInputAssemblyStateCreateInfo assembly{};
  assembly.topology = tessellated ? vk::PrimitiveTopology::ePatchList : vk::PrimitiveTopology::eTriangleList;
  vk::PipelineTessellationStateCreateInfo tessellation{};
  tessellation.patchControlPoints = patch_control_points;
  const vk::Viewport viewport{0, 0, float(Width), float(Height), 0, 1};
  const vk::Rect2D scissor{{0, 0}, {Width, Height}};
  vk::PipelineViewportStateCreateInfo viewports{};
  viewports.viewportCount = viewports.scissorCount = 1;
  viewports.pViewports = &viewport;
  viewports.pScissors = &scissor;
  vk::PipelineRasterizationStateCreateInfo raster{};
  raster.polygonMode = vk::PolygonMode::eFill;
  raster.cullMode = vk::CullModeFlagBits::eNone;
  raster.frontFace = vk::FrontFace::eCounterClockwise;
  raster.lineWidth = 1;
  vk::PipelineMultisampleStateCreateInfo samples{};
  samples.rasterizationSamples = vk::SampleCountFlagBits::e1;
  vk::PipelineColorBlendAttachmentState color{};
  color.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
  vk::PipelineColorBlendStateCreateInfo blend{};
  blend.attachmentCount = 1;
  blend.pAttachments = &color;
  vk::PipelineRenderingCreateInfo rendering_pipeline{};
  rendering_pipeline.colorAttachmentCount = 1;
  rendering_pipeline.pColorAttachmentFormats = &target.format;
  vk::GraphicsPipelineCreateInfo pipeline_info{};
  pipeline_info.pNext = &rendering_pipeline;
  pipeline_info.stageCount = static_cast<u32>(stages.size());
  pipeline_info.pStages = stages.data();
  pipeline_info.pVertexInputState = &vertex_input;
  pipeline_info.pInputAssemblyState = &assembly;
  pipeline_info.pTessellationState = tessellated ? &tessellation : nullptr;
  pipeline_info.pViewportState = &viewports;
  pipeline_info.pRasterizationState = &raster;
  pipeline_info.pMultisampleState = &samples;
  pipeline_info.pColorBlendState = &blend;
  pipeline_info.layout = layout;
  vk::Pipeline pipeline{};
  RequireVk(name, "graphics pipeline", m_device.createGraphicsPipelines(nullptr, 1,
      &pipeline_info, nullptr, &pipeline), "vkCreateGraphicsPipelines");

  std::vector<Buffer> readbacks;
  const std::vector<u32> initial_readback(Width * Height * 4 + 8, Guard);
  for (size_t i = 0; i < draws.size(); ++i)
    readbacks.push_back(CreateHostBuffer(name, initial_readback.size() * sizeof(u32),
        vk::BufferUsageFlagBits::eTransferDst, initial_readback));
  auto cmd = BeginCommands(name, "primitive stages");
  vk::RenderingAttachmentInfo attachment{};
  attachment.imageView = target.view;
  attachment.imageLayout = vk::ImageLayout::eGeneral;
  attachment.loadOp = vk::AttachmentLoadOp::eClear;
  attachment.storeOp = vk::AttachmentStoreOp::eStore;
  vk::RenderingInfo rendering{};
  rendering.renderArea.extent = {Width, Height};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &attachment;
  for (size_t i = 0; i < draws.size(); ++i) {
    const auto &draw = draws[i];
    if (i != 0) AddImageBarrier(cmd, target.image, vk::ImageLayout::eTransferSrcOptimal,
        vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::AccessFlagBits::eTransferRead,
        vk::AccessFlagBits::eColorAttachmentWrite, 1, 1);
    cmd.beginRendering(&rendering);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, 1, &set, 0, nullptr);
    cmd.pushConstants(layout, push_stages, 0, sizeof(draw.push), draw.push.data());
    if (draw.indexed) {
      Require(name, "index range", draw.first_index + draw.vertex_count <= indices.size(),
              "indexed fixture references unavailable indices");
      cmd.bindIndexBuffer(index_buffer.buffer, 4 * sizeof(u32), vk::IndexType::eUint32);
      cmd.drawIndexed(draw.vertex_count, draw.instance_count, draw.first_index,
                      draw.vertex_offset, draw.first_instance);
    } else {
      Require(name, "vertex range", (draw.first_vertex + draw.vertex_count) * 8 <= vertices.size(),
              "direct fixture references unavailable vertices");
      cmd.draw(draw.vertex_count, draw.instance_count, draw.first_vertex, draw.first_instance);
    }
    cmd.endRendering();
    AddImageBarrier(cmd, target.image, vk::ImageLayout::eGeneral,
        vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eTransfer, vk::AccessFlagBits::eColorAttachmentWrite,
        vk::AccessFlagBits::eTransferRead, 1, 1);
    vk::BufferImageCopy copy{};
    copy.bufferOffset = 4 * sizeof(u32);
    copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.imageExtent = {Width, Height, 1};
    cmd.copyImageToBuffer(target.image, vk::ImageLayout::eTransferSrcOptimal,
                          readbacks[i].buffer, 1, &copy);
  }
  vk::MemoryBarrier host_visible{};
  host_visible.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  host_visible.dstAccessMask = vk::AccessFlagBits::eHostRead;
  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                       {}, 1, &host_visible, 0, nullptr, 0, nullptr);
  EndSubmitAndFree(name, "primitive stages", cmd);
  std::vector<std::vector<u32>> result;
  for (auto &buffer : readbacks) {
    auto words = ReadBuffer(name, buffer, initial_readback.size());
    Require(name, "readback guards", std::all_of(words.begin(), words.begin() + 4,
                [](u32 word) { return word == Guard; }) &&
                std::all_of(words.end() - 4, words.end(), [](u32 word) { return word == Guard; }),
            "primitive rendering or transfer changed a readback canary");
    result.emplace_back(words.begin() + 4, words.end() - 4);
    DestroyBuffer(&buffer);
  }
  Require(name, "input buffer guards and contents",
          ReadBuffer(name, vertex_buffer, guarded_vertices.size()) == guarded_vertices &&
              ReadBuffer(name, index_buffer, guarded_indices.size()) == guarded_indices,
          "stage expansion changed guarded vertex/index input buffers");
  m_device.destroyPipeline(pipeline);
  m_device.destroyDescriptorPool(pool);
  m_device.destroyPipelineLayout(layout);
  m_device.destroyDescriptorSetLayout(set_layout);
  for (auto module : modules) m_device.destroyShaderModule(module);
  DestroyBuffer(&index_buffer);
  DestroyBuffer(&vertex_buffer);
  DestroyImage(&target);
  return result;
}

void VulkanHarness::CheckNativeTessellation() {
  using namespace PrimitiveContract;
  constexpr const char *name = "NativeTessellation";
  Require(name, "capability", m_enabled_base_features.tessellationShader,
          "tessellationShader was not supported and enabled");
  const auto vertex = AssembleNativeMetalContract(PrimitiveContractSpv::Vertex);
  const auto control3 = AssembleNativeMetalContract(PrimitiveContractSpv::Control3);
  const auto control4 = AssembleNativeMetalContract(PrimitiveContractSpv::Control4);
  const auto triangle = AssembleNativeMetalContract(PrimitiveContractSpv::TriangleEvaluation);
  const auto odd = AssembleNativeMetalContract(PrimitiveContractSpv::OddEvaluation);
  const auto even = AssembleNativeMetalContract(PrimitiveContractSpv::EvenEvaluation);
  const auto quad = AssembleNativeMetalContract(PrimitiveContractSpv::QuadEvaluation);
  const auto fragment = AssembleNativeMetalContract(PrimitiveContractSpv::Fragment);
  const std::array level_draws{Draw(1, true), Draw(2, true), Draw(0), Draw(1)};
  for (const auto *evaluation : {&triangle, &odd}) {
    const auto pixels = RenderPrimitiveContract(name, vertex, control3, *evaluation,
        fragment, 3, Triangle(), {}, level_draws);
    Solid("TessellationLevelOne", pixels[0], {0, Color[1], Color[2], 1});
    bool curved = false;
    for (size_t p = 0; p < pixels[1].size(); p += 4) {
      curved |= std::bit_cast<float>(pixels[1][p]) > 0.1f;
      for (u32 channel = 1; channel < 4; ++channel)
        Require(name, "higher tessellation level coverage",
                std::abs(std::bit_cast<float>(pixels[1][p + channel]) - Color[channel]) < 0.0001f,
                "higher tessellation level lost coverage or a control-point parameter");
    }
    Require(name, "higher tessellation level evaluation", curved,
            "level two did not evaluate interior/edge vertices beyond the original corners");
    Solid("TessellationZeroLevel", pixels[2], {});
    Solid("TessellationPipelineReuse", pixels[3], Color);
  }

  auto asymmetric = Draw(2.5f, true);
  asymmetric.push[6] = 1;
  const std::array fractional_draws{Draw(2.5f, true), asymmetric, Draw(2.5f, true)};
  for (const auto *evaluation : {&odd, &even}) {
    const auto pixels = RenderPrimitiveContract("TessellationFractionalAsymmetric", vertex,
        control3, *evaluation, fragment, 3, Triangle(), {}, fractional_draws);
    u32 different = 0;
    for (size_t pixel = 0; pixel < Width * Height; ++pixel) {
      different += std::abs(std::bit_cast<float>(pixels[0][pixel * 4]) -
                            std::bit_cast<float>(pixels[1][pixel * 4])) > 0.001f;
      for (const auto &frame : pixels) for (u32 channel = 1; channel < 4; ++channel)
        Require(name, "fractional/asymmetric patch coverage",
                std::abs(std::bit_cast<float>(frame[pixel * 4 + channel]) - Color[channel]) < 0.0001f,
                "fractional factors lost patch coverage or control-point parameters");
    }
    Require(name, "independent fractional tessellation factors", different >= 16 && pixels[0] == pixels[2],
            "asymmetric factors did not change evaluation or factor state leaked into a reused draw");
  }

  constexpr std::array<std::array<float, 2>, 4> square{{{-1, -1}, {1, -1}, {-1, 1}, {1, 1}}};
  auto quad_draw = Draw();
  quad_draw.vertex_count = 4;
  auto quad_zero = quad_draw;
  quad_zero.push[0] = 0;
  const std::array quad_draws{quad_draw, quad_zero, quad_draw};
  const auto quad_pixels = RenderPrimitiveContract(name, vertex, control4, quad, fragment,
      4, Vertices(square), {}, quad_draws);
  Solid("TessellationFourControlPoints", quad_pixels[0], Color);
  Solid("TessellationQuadZeroLevel", quad_pixels[1], {});
  Solid("TessellationQuadReuse", quad_pixels[2], Color);

  auto offset_vertices = Triangle();
  offset_vertices.insert(offset_vertices.begin(), {std::bit_cast<u32>(8.f), std::bit_cast<u32>(8.f),
      0, std::bit_cast<u32>(1.f), 0, 0, 0, 0});
  auto direct = Draw();
  direct.first_vertex = 1;
  auto indexed = Draw();
  indexed.indexed = true;
  indexed.first_index = 1;
  indexed.vertex_offset = 1;
  auto instanced = direct;
  instanced.instance_count = 2;
  instanced.first_instance = 7;
  instanced.push[5] = 7;
  const std::array offset_draws{direct, indexed, instanced, direct};
  const auto offset_pixels = RenderPrimitiveContract(name, vertex, control3, triangle,
      fragment, 3, offset_vertices, {1, 0, 1, 2}, offset_draws);
  Solid("TessellationFirstVertex", offset_pixels[0], Color);
  Solid("TessellationIndexedBaseVertex", offset_pixels[1], Color);
  Solid("TessellationInstances", offset_pixels[2], {Color[0], Color[1], Color[2] + 0.125f, 1});
  Solid("TessellationDrawStateReuse", offset_pixels[3], Color);

  constexpr std::array<std::array<float, 2>, 6> patch_positions{{
      {-1, -1}, {0, -1}, {-1, 1}, {0, -1}, {1, -1}, {1, 1}}};
  auto patch_vertices = Vertices(patch_positions);
  for (size_t v = 3; v < 6; ++v) patch_vertices[v * 8 + 4] = std::bit_cast<u32>(0.875f);
  auto two_patches = Draw(2);
  two_patches.vertex_count = 6;
  const std::array patch_draws{two_patches, two_patches};
  const auto patch_frames = RenderPrimitiveContract("TessellationMultiplePatches", vertex,
      control3, triangle, fragment, 3, patch_vertices, {}, patch_draws);
  for (const auto &frame : patch_frames) for (u32 y = 0; y < Height; ++y) for (u32 x = 0; x < Width; ++x) {
    const bool left = 2 * x + y <= 14;
    const bool right = x >= 8 && int32_t(y) <= 2 * int32_t(x) - 16;
    auto expected = left || right ? Color : std::array<float, 4>{};
    if (right) expected[0] = 0.875f;
    for (u32 channel = 0; channel < 4; ++channel)
      Require(name, "independent patch control points",
              std::abs(std::bit_cast<float>(frame[(y * Width + x) * 4 + channel]) - expected[channel]) < 0.0001f,
              "patch expansion reused another patch's control points or lost its coverage");
  }

  // Execute the production Gen5 rectangle expansion: exactly three software
  // vertex fetches become four control points and a complete quad with UVs.
  ShaderRecompiler::IR::CompiledShaderInfo vertex_info{.param_export_mask = 1};
  ShaderRecompiler::IR::CompiledShaderInfo pixel_info{};
  pixel_info.info.inputs.push_back({ShaderRecompiler::IR::StageInputKind::Parameter, 0, 4, "uv"});
  ShaderRecompiler::IR::ResourceSnapshot empty;
  ShaderVertexInputInfo vertex_input{};
  vertex_input.stage.program = &vertex_info;
  vertex_input.stage.resources = &empty;
  ShaderPixelInputInfo pixel_input{};
  pixel_input.parameter_mode = ShaderPixelParameterMode::Rectangle;
  pixel_input.input_num = 1;
  pixel_input.stage.program = &pixel_info;
  pixel_input.stage.resources = &empty;
  const auto rectangle = BuildRectListShaders(vertex_input, &pixel_input);
  constexpr std::array<std::array<float, 2>, 3> corners{{{-0.75f, -0.75f},
      {0.75f, -0.75f}, {-0.75f, 0.75f}}};
  const std::array rectangle_draws{Draw(), Draw()};
  const auto rectangle_pixels = RenderPrimitiveContract("Gen5RectList", vertex,
      rectangle.control, rectangle.evaluation, fragment, 3, Vertices(corners, true),
      {}, rectangle_draws);
  for (const auto &frame : rectangle_pixels) {
    for (u32 y = 0; y < Height; ++y) for (u32 x = 0; x < Width; ++x) {
      const bool covered = x >= 2 && x < 14 && y >= 2 && y < 14;
      const std::array<float, 4> expected{covered ? (float(x) + 0.5f - 2.f) / 12.f : 0.f,
          covered ? (float(y) + 0.5f - 2.f) / 12.f : 0.f, 0.f, covered ? 1.f : 0.f};
      for (u32 channel = 0; channel < 4; ++channel)
        Require(name, "Gen5 rectangle reconstruction and interpolation",
                std::abs(std::bit_cast<float>(frame[(y * Width + x) * 4 + channel]) - expected[channel]) < 0.0001f,
                "production rectangle tessellation lost the fourth corner, UVs, or untouched border");
    }
  }
  std::printf("[gpu]     %-32s ok (triangle equal/odd/even, levels=0/1/2/2.5 asymmetric, quad4, multipatch, indexed/instanced, Gen5 rect3-to-4, guards/reuse)\n", name);
}

void VulkanHarness::CheckNativeCullDistance() {
  using namespace PrimitiveContract;
  constexpr const char *name = "NativeCullDistance";
  Require(name, "capability", m_enabled_base_features.shaderCullDistance,
          "shaderCullDistance was not supported and enabled");
  const auto vertex = AssembleNativeMetalContract(PrimitiveContractSpv::Vertex);
  const auto cull_vertex = AssembleNativeMetalContract(PrimitiveContractSpv::CullVertex);
  const auto fragment = AssembleNativeMetalContract(PrimitiveContractSpv::Fragment);
  std::array<PrimitiveContractDraw, 6> draws;
  for (u32 mode = 0; mode < draws.size(); ++mode) draws[mode] = Draw(1, false, mode);
  const auto Check = [&](const std::vector<std::vector<u32>> &frames,
                         std::array<float, 4> visible = Color) {
    Require(name, "case count", frames.size() == draws.size(), "cull fixture lost a draw");
    for (u32 mode = 0; mode < frames.size(); ++mode)
      Solid(mode == 0 ? "CullAllNegative" : mode == 1 ? "CullMixedSignsKeepWholePrimitive" :
            mode == 2 ? "CullSecondComponent" : mode == 3 ? "CullZeroBoundary" :
            mode == 4 ? "CullIndependentMixedComponents" : "CullAllZero",
            frames[mode], mode == 0 || mode == 2 ? std::array<float, 4>{} : visible);
  };
  Check(RenderPrimitiveContract(name, cull_vertex, {}, {}, fragment, 0, Triangle(), {}, draws));
  Require(name, "TES combination capability", m_enabled_base_features.tessellationShader,
          "cull-distance contract also requires real tessellation evaluation coverage");
  const auto control = AssembleNativeMetalContract(PrimitiveContractSpv::Control3);
  const auto evaluation = AssembleNativeMetalContract(PrimitiveContractSpv::CullEvaluation);
  Check(RenderPrimitiveContract(name, vertex, control, evaluation, fragment, 3, Triangle(), {}, draws));

  auto offset_vertices = Triangle();
  offset_vertices.insert(offset_vertices.begin(), {std::bit_cast<u32>(8.f), std::bit_cast<u32>(8.f),
      0, std::bit_cast<u32>(1.f), 0, 0, 0, 0});
  auto indexed_draws = draws;
  for (auto &draw : indexed_draws) {
    draw.indexed = true;
    draw.first_index = 1;
    draw.vertex_offset = 1;
    draw.instance_count = 2;
    draw.first_instance = 7;
    draw.push[5] = 7;
  }
  const std::array<float, 4> instanced_color{Color[0], Color[1], Color[2] + 0.125f, 1};
  Check(RenderPrimitiveContract(name, cull_vertex, {}, {}, fragment, 0, offset_vertices,
      {1, 0, 1, 2}, indexed_draws), instanced_color);
  Check(RenderPrimitiveContract(name, vertex, control, evaluation, fragment, 3, offset_vertices,
      {1, 0, 1, 2}, indexed_draws), instanced_color);

  // At level two, two lower triangles have all-negative d0 while their
  // neighbors touch d0==0 and survive. A skew prevents pixel centers lying on
  // shared edges, making the complete per-primitive mask unambiguous.
  constexpr std::array<std::array<float, 2>, 3> skew{{{-1, -1}, {3, -1}, {-1, 2.875f}}};
  const std::array partial_draws{Draw(2, false, 1), Draw(1, false, 1), Draw(2, false, 1)};
  const auto partial_frames = RenderPrimitiveContract("TessellationCullPerPrimitive", vertex,
      control, evaluation, fragment, 3, Vertices(skew), {}, partial_draws);
  for (size_t frame_index : {size_t(0), size_t(2)}) {
    u32 visible_pixels = 0;
    for (u32 y = 0; y < Height; ++y) for (u32 x = 0; x < Width; ++x) {
      const float px = float(x) + 0.5f, py = float(y) + 0.5f;
      const bool culled = py < px * (31.f / 32.f) && py < (32.f - px) * (31.f / 64.f);
      visible_pixels += !culled;
      for (u32 channel = 0; channel < 4; ++channel)
        Require(name, "TES cull applies per generated primitive",
                std::abs(std::bit_cast<float>(partial_frames[frame_index][(y * Width + x) * 4 + channel]) -
                         (culled ? 0.f : Color[channel])) < 0.0001f,
                "TES cull discarded a whole patch, clipped mixed signs, or retained an all-negative generated triangle");
    }
    Require(name, "partial primitive culling", visible_pixels > 0 && visible_pixels < Width * Height,
            "partial-cull fixture did not distinguish patch and primitive culling");
  }
  Solid("TessellationMixedSignsWholePrimitive", partial_frames[1], Color);
  std::printf("[gpu]     %-32s ok (VS/TES, all-negative/mixed/zero, two components, indexed/instanced, level2 per-primitive mask, guards/reuse)\n", name);
}
