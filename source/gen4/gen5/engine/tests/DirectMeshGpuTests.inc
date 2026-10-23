// Synthetic VK_EXT_mesh_shader driver qualification. No ROM or native fragment
// barycentrics are required. This exercises the driver mesh-to-raster path; it
// does not substitute for the production guest wave64 translation contracts.

std::vector<u32> AssembleMeshGpuFixture(const std::string& source) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string diagnostic;
  tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&,
                               const char* message) { diagnostic += message; });
  std::vector<u32> words;
  Require("MeshDriver", "assemble synthetic module", tools.Assemble(source, &words), diagnostic);
  ValidateSpirv("MeshDriver synthetic module", words);
  return words;
}

std::vector<u32> MeshGpuProducer(u32 threads) {
  std::string source = R"(
OpCapability Shader
OpCapability MeshShadingEXT
OpExtension "SPV_EXT_mesh_shader"
OpMemoryModel Logical GLSL450
OpEntryPoint MeshEXT %main "main" %local %position %parameter %indices %culled %push %shared
OpExecutionMode %main OutputVertices 6
OpExecutionMode %main OutputPrimitivesEXT 2
OpExecutionMode %main OutputTrianglesEXT
)";
  source += "OpExecutionMode %main LocalSize " + std::to_string(threads) + " 1 1\n";
  source += R"(
OpDecorate %local BuiltIn LocalInvocationIndex
OpDecorate %position BuiltIn Position
OpDecorate %parameter Location 0
OpDecorate %indices BuiltIn PrimitiveTriangleIndicesEXT
OpDecorate %culled BuiltIn CullPrimitiveEXT
OpDecorate %culled PerPrimitiveEXT
OpDecorate %pushBlock Block
OpMemberDecorate %pushBlock 0 Offset 0
OpMemberDecorate %pushBlock 1 Offset 4
%void = OpTypeVoid
%function = OpTypeFunction %void
%bool = OpTypeBool
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%six = OpConstant %uint 6
%semantics = OpConstant %uint 264
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%fminus = OpConstant %float -1
%fthree = OpConstant %float 3
%fquarter = OpConstant %float 0.25
%vec3u = OpTypeVector %uint 3
%vec4 = OpTypeVector %float 4
%positions = OpTypeArray %vec4 %six
%triangles = OpTypeArray %vec3u %two
%culls = OpTypeArray %bool %two
%outputPositions = OpTypePointer Output %positions
%output4 = OpTypePointer Output %vec4
%outputTriangles = OpTypePointer Output %triangles
%output3u = OpTypePointer Output %vec3u
%outputCulls = OpTypePointer Output %culls
%outputBool = OpTypePointer Output %bool
%inputUint = OpTypePointer Input %uint
%pushBlock = OpTypeStruct %float %uint
%pushPointer = OpTypePointer PushConstant %pushBlock
%pushFloat = OpTypePointer PushConstant %float
%pushUint = OpTypePointer PushConstant %uint
)";
  source += "%threads = OpConstant %uint " + std::to_string(threads) + "\n";
  source += "%last = OpConstant %uint " + std::to_string(threads - 1) + "\n";
  source += "%halfLast = OpConstant %uint " + std::to_string(threads / 2 - 1) + "\n";
  source += "%fthreads = OpConstant %float " + std::to_string(threads) + "\n";
  source += R"(
%sharedArray = OpTypeArray %uint %threads
%sharedPointer = OpTypePointer Workgroup %sharedArray
%sharedUint = OpTypePointer Workgroup %uint
%p0 = OpConstantComposite %vec4 %fminus %fminus %fzero %fone
%p1 = OpConstantComposite %vec4 %fthree %fminus %fzero %fone
%p2 = OpConstantComposite %vec4 %fminus %fthree %fzero %fone
%local = OpVariable %inputUint Input
%position = OpVariable %outputPositions Output
%parameter = OpVariable %outputPositions Output
%indices = OpVariable %outputTriangles Output
%culled = OpVariable %outputCulls Output
%push = OpVariable %pushPointer PushConstant
%shared = OpVariable %sharedPointer Workgroup
%main = OpFunction %void None %function
%entry = OpLabel
%lane = OpLoad %uint %local
%modeAddress = OpAccessChain %pushUint %push %one
%mode = OpLoad %uint %modeAddress
%scaleAddress = OpAccessChain %pushFloat %push %zero
%scale = OpLoad %float %scaleAddress
%empty = OpIEqual %bool %mode %two
%single = OpIEqual %bool %mode %three
%primitiveCountNonzero = OpSelect %uint %single %one %two
%primitiveCount = OpSelect %uint %empty %zero %primitiveCountNonzero
%vertexCount = OpSelect %uint %empty %zero %six
OpSetMeshOutputsEXT %vertexCount %primitiveCount
%sharedAddress = OpAccessChain %sharedUint %shared %lane
%laneValue = OpIAdd %uint %lane %one
OpStore %sharedAddress %laneValue
OpControlBarrier %two %two %semantics
%nonempty = OpLogicalNot %bool %empty
%vertexLane = OpULessThan %bool %lane %six
%writeVertex = OpLogicalAnd %bool %nonempty %vertexLane
OpSelectionMerge %vertexMerge None
OpBranchConditional %writeVertex %vertexBody %vertexMerge
%vertexBody = OpLabel
%corner = OpUMod %uint %lane %three
%cornerZero = OpIEqual %bool %corner %zero
%cornerOne = OpIEqual %bool %corner %one
%p12 = OpSelect %vec4 %cornerOne %p1 %p2
%p = OpSelect %vec4 %cornerZero %p0 %p12
%positionAddress = OpAccessChain %output4 %position %lane
OpStore %positionAddress %p
%lastAddress = OpAccessChain %sharedUint %shared %last
%halfAddress = OpAccessChain %sharedUint %shared %halfLast
%lastValue = OpLoad %uint %lastAddress
%halfValue = OpLoad %uint %halfAddress
%lastFloat = OpConvertUToF %float %lastValue
%halfFloat = OpConvertUToF %float %halfValue
%high = OpFDiv %float %lastFloat %fthreads
%low = OpFDiv %float %halfFloat %fthreads
%triangle = OpUDiv %uint %lane %three
%trianglePlusOne = OpIAdd %uint %triangle %one
%triangleFloat = OpConvertUToF %float %trianglePlusOne
%alpha = OpFMul %float %triangleFloat %fquarter
%value = OpCompositeConstruct %vec4 %scale %high %low %alpha
%parameterAddress = OpAccessChain %output4 %parameter %lane
OpStore %parameterAddress %value
OpBranch %vertexMerge
%vertexMerge = OpLabel
%primitiveLane = OpULessThan %bool %lane %primitiveCount
OpSelectionMerge %primitiveMerge None
OpBranchConditional %primitiveLane %primitiveBody %primitiveMerge
%primitiveBody = OpLabel
%base = OpIMul %uint %lane %three
%index1 = OpIAdd %uint %base %one
%index2 = OpIAdd %uint %base %two
%triangleIndices = OpCompositeConstruct %vec3u %base %index1 %index2
%indexAddress = OpAccessChain %output3u %indices %lane
OpStore %indexAddress %triangleIndices
%cullSecond = OpIEqual %bool %mode %zero
%cullFirst = OpIEqual %bool %mode %one
%isSecond = OpIEqual %bool %lane %one
%isFirst = OpIEqual %bool %lane %zero
%killSecond = OpLogicalAnd %bool %cullSecond %isSecond
%killFirst = OpLogicalAnd %bool %cullFirst %isFirst
%kill = OpLogicalOr %bool %killSecond %killFirst
%cullAddress = OpAccessChain %outputBool %culled %lane
OpStore %cullAddress %kill
OpBranch %primitiveMerge
%primitiveMerge = OpLabel
OpReturn
OpFunctionEnd
)";
  return AssembleMeshGpuFixture(source);
}


void CheckMeshSubgroupPolicy() {
  GraphicContext context;
  context.mesh_subgroup_size_control_enabled = true;
  context.min_subgroup_size = 32;
  context.max_subgroup_size = 128;
  context.max_compute_workgroup_subgroups = 32;
  context.subgroup_size = 64;
  context.mesh_shader_properties.maxMeshWorkGroupInvocations = 256;
  context.mesh_shader_properties.maxMeshWorkGroupSize[0] = 256;
  unsigned checks = 0;
  const auto check = [&](u32 wave, uint64_t threads, u32 expected, const char* detail) {
    Require("MeshSubgroupPolicy", detail,
            context.SelectMeshSubgroupSize(wave, threads) == expected,
            "selected an unsupported subgroup or changed the logical workgroup coverage");
    ++checks;
  };
  check(64, 256, 64, "preserve fitting native wave64");
  context.subgroup_size = 128;
  check(64, 128, 64, "request matching guest wave instead of native wave128");
  context.mesh_shader_properties.maxMeshWorkGroupInvocations = 128;
  context.mesh_shader_properties.maxMeshWorkGroupSize[0] = 128;
  check(64, 256, 32, "compact 256 logical lanes to 128 physical lanes");
  check(32, 96, 32, "wave32 requires an actual wave32 subgroup");
  check(32, 256, 0, "wave32 cannot compact logical invocations");
  context.subgroup_size = 64;
  check(64, 256, 32, "native wave64 exceeding invocation limit selects wave32");
  context.mesh_subgroup_size_control_enabled = false;
  check(64, 256, 0, "deny unsupported required subgroup");
  context.subgroup_size = 128;
  check(64, 64, 0, "deny unpartitioned wave128");
  context.subgroup_size = 32;
  check(64, 256, 32, "existing native wave32 dual-lane path remains valid");
  context.subgroup_size = 128;
  context.mesh_subgroup_size_control_enabled = true;
  context.min_subgroup_size = 64;
  check(64, 256, 0, "honor minimum subgroup size");
  context.min_subgroup_size = 32;
  context.max_compute_workgroup_subgroups = 3;
  check(64, 256, 0, "honor maximum subgroups with required subgroup size");
  context.max_compute_workgroup_subgroups = 32;
  context.mesh_shader_properties.maxMeshWorkGroupSize[0] = 96;
  check(64, 256, 0, "honor workgroup X limit independently");
  check(64, 0, 0, "deny empty workgroup");
  check(128, 64, 0, "deny unsupported guest wave size");
  check(64, UINT64_MAX, 0, "large workgroup cannot overflow into admission");
  ShaderVertexInputInfo vertex;
  vertex.mesh.threads_num[0] = 256;
  std::vector<u32> native_key, compact_key;
  vertex.mesh.host_subgroup_size = 64;
  BuildStageStaticKey(vertex, native_key);
  vertex.mesh.host_subgroup_size = 32;
  BuildStageStaticKey(vertex, compact_key);
  Require("MeshSubgroupPolicy", "shader cache key", native_key != compact_key,
          "mesh subgroup size must distinguish shader permutations");

  BarycentricFallback::MeshPlan plan;
  plan.coordinate_inputs = {{17, 4, false}};
  plan.coefficient_locations = {5, 6};
  plan.raw_inputs = {{27, 3, 7}};
  plan.ordinary_locations = {0};
  vertex.mesh.barycentric_limits = {28672, 32768, 128, 1, 1};
  vertex.mesh.barycentric_fallback = std::make_shared<const BarycentricFallback::MeshPlan>(plan);
  std::vector<u32> fallback_key;
  BuildStageStaticKey(vertex, fallback_key);
  unsigned key_checks = 0;
  const auto check_key = [&](const ShaderVertexInputInfo& changed, bool equal, const char* detail) {
    std::vector<u32> key;
    BuildStageStaticKey(changed, key);
    Require("MeshSubgroupPolicy", detail, (key == fallback_key) == equal,
            "mesh cache key did not preserve the compiled interface or resource admission contract");
    ++key_checks;
  };
  auto without_fallback = vertex;
  without_fallback.mesh.barycentric_fallback.reset();
  check_key(without_fallback, false, "mesh fallback distinguishes native permutation");
  auto equivalent = vertex;
  equivalent.mesh.barycentric_fallback =
      std::make_shared<const BarycentricFallback::MeshPlan>(plan);
  check_key(equivalent, true, "equivalent plans share a key despite different owners");
  const auto check_plan = [&](auto modify, const char* detail) {
    auto changed_plan = plan;
    modify(changed_plan);
    auto changed = vertex;
    changed.mesh.barycentric_fallback =
        std::make_shared<const BarycentricFallback::MeshPlan>(std::move(changed_plan));
    check_key(changed, false, detail);
  };
  check_plan([](auto& value) { value.coordinate_inputs[0].location = 10; }, "coordinate location key");
  check_plan([](auto& value) { value.coordinate_inputs[0].no_perspective = true; }, "interpolation mode key");
  check_plan([](auto& value) { value.coefficient_locations[0] = 10; }, "first coefficient location key");
  check_plan([](auto& value) { value.coefficient_locations[1] = 10; }, "second coefficient location key");
  check_plan([](auto& value) { value.raw_inputs[0].source_location = 2; }, "raw corner source key");
  check_plan([](auto& value) { value.raw_inputs[0].location = 10; }, "raw corner destination key");
  check_plan([](auto& value) { value.ordinary_locations = {1}; }, "ordinary mesh interface key");
  check_plan([](auto& value) { value.compact = !value.compact; }, "compact mesh interface key");
  auto changed_shared_cap = vertex;
  changed_shared_cap.mesh.barycentric_limits.max_shared_memory_bytes /= 2;
  check_key(changed_shared_cap, false, "shared memory admission cap key");
  auto changed_output_cap = vertex;
  changed_output_cap.mesh.barycentric_limits.max_output_memory_bytes /= 2;
  check_key(changed_output_cap, false, "output memory admission cap key");
  ShaderPixelInputInfo pixel;
  std::vector<u32> native_pixel_key, mesh_pixel_key;
  BuildStageStaticKey(pixel, native_pixel_key);
  pixel.mesh_barycentric_fallback = true;
  BuildStageStaticKey(pixel, mesh_pixel_key);
  Require("MeshSubgroupPolicy", "mesh fragment cache key", native_pixel_key != mesh_pixel_key,
          "mesh and geometry fragment interfaces must not reuse the same cached program");
  ++key_checks;
  pixel.ps_sample_shading = true;
  std::vector<u32> sample_pixel_key;
  BuildStageStaticKey(pixel, sample_pixel_key);
  Require("MeshSubgroupPolicy", "sample shading fragment cache key", sample_pixel_key != mesh_pixel_key,
          "sample shading must distinguish the fragment interpolation transport");
  ++key_checks;
  std::printf("[host]    MeshSubgroupPolicy ok (%u selections, %u mesh/fragment key contracts)\n",
              checks, key_checks);
}

std::vector<u32> MeshGpuProductionEmitter(u32 host_subgroup, u32 base_vertex) {
  namespace IR = ShaderRecompiler::IR;
  using IR::Value;
  using IR::ValueOpcode;
  ShaderVertexInputInfo input;
  input.logical_stage = ShaderType::Mesh;
  input.mesh.threads_num[0] = 256;
  input.mesh.threads_num[1] = input.mesh.threads_num[2] = 1;
  input.mesh.host_subgroup_size = host_subgroup;
  input.mesh.wave_size = 64;
  input.mesh.max_vertices = base_vertex + 3;
  input.mesh.max_primitives = 1;
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Mesh;
  options.wave_size = 64;
  options.input_info.vertex = &input;
  ShaderRecompiler::TranslateResult translated;
  auto& program = translated.program;
  program.stage = ShaderType::Mesh;
  program.wave_size = 64;
  program.block_storage.push_back(std::make_unique<IR::Block>());
  auto* block = program.block_storage.back().get();
  program.blocks.push_back(block);
  const auto emit = [&](ValueOpcode op, std::initializer_list<Value> args, uint64_t flags = 0) {
    return Value(&block->AppendNewInst(op, args, flags));
  };
  const auto logical = emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<u32>(IR::StageInputKind::LocalInvocationIndex)), Value(0u)});
  const auto lane = emit(ValueOpcode::LaneId, {});
  emit(ValueOpcode::MeshAllocate, {Value(0x1000u | (base_vertex + 3u))});
  const auto first = emit(ValueOpcode::IEqual32, {logical, Value(base_vertex)});
  const auto second = emit(ValueOpcode::IEqual32, {logical, Value(base_vertex + 1u)});
  const auto coord_x = emit(ValueOpcode::SelectU32,
      {second, Value(std::bit_cast<u32>(3.0f)), Value(std::bit_cast<u32>(-1.0f))});
  const auto first_two = emit(ValueOpcode::LogicalOr, {first, second});
  const auto coord_y = emit(ValueOpcode::SelectU32,
      {first_two, Value(std::bit_cast<u32>(-1.0f)), Value(std::bit_cast<u32>(3.0f))});
  const auto position = emit(ValueOpcode::CompositeConstructU32x4,
      {coord_x, coord_y, Value(0u), Value(std::bit_cast<u32>(1.0f))});
  const auto normalized = [&](Value integer, float reciprocal) {
    const auto value = emit(ValueOpcode::ConvertF32U32, {integer});
    const auto scaled = emit(ValueOpcode::FPMul32, {value, Value::F32(reciprocal)});
    return emit(ValueOpcode::BitCastU32F32, {scaled});
  };
  const auto parameter = emit(ValueOpcode::CompositeConstructU32x4,
      {normalized(lane, 1.0f / 64.0f), normalized(logical, 1.0f / 256.0f),
       Value(std::bit_cast<u32>(0.75f)), Value(std::bit_cast<u32>(1.0f))});
  const auto packed = base_vertex | ((base_vertex + 1u) << 10u) | ((base_vertex + 2u) << 20u);
  const auto primitive = emit(ValueOpcode::CompositeConstructU32x4,
      {Value(packed), Value(0u), Value(0u), Value(0u)});
  program.export_info = {
      {.kind = IR::ExportTargetKind::Position, .target = 0x0c, .en = 0xf},
      {.kind = IR::ExportTargetKind::Parameter, .target = 0x20, .en = 0xf},
      {.kind = IR::ExportTargetKind::Primitive, .target = 0x14, .en = 1},
  };
  for (const auto [index, data] : std::array<std::pair<u32, Value>, 3>{{
           {0, position}, {1, parameter}, {2, primitive}}}) {
    IR::ExportFlags flags{index, 0};
    uint64_t bits = 0;
    std::memcpy(&bits, &flags, sizeof(flags));
    emit(ValueOpcode::SetAttribute, {data, Value(true)}, bits);
  }
  block->SsaSeal();
  IR::TrackResources(program, {}, {}, options.input_info);
  auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, {});
  ValidateSpirv("MeshProductionWave64", compiled.spirv);
  Require("MeshProductionWave64", "descriptor-free fixture",
          std::ranges::all_of(compiled.program.bindings.descriptor_counts,
                              [](u32 count) { return count == 0; }),
          "synthetic mesh unexpectedly needs descriptor resources");
  return compiled.spirv;
}

std::vector<u32> MeshGpuBarycentricEmitter(
    u32 host_subgroup, u32 variant, bool last,
    const std::shared_ptr<const BarycentricFallback::MeshPlan>& fallback,
    const vk::PhysicalDeviceMeshShaderPropertiesEXT& properties) {
  namespace IR = ShaderRecompiler::IR;
  using IR::Value;
  using IR::ValueOpcode;
  // Shared vertices span logical wave boundaries and both halves of wave64.
  // Keep all 162 declared vertices: a fallback which duplicates every primitive
  // corner cannot hide behind a six-vertex synthetic producer.
  constexpr std::array<u32, 4> slots{0, 33, 96, 161};
  const bool budget = variant == 6;
  const bool empty = variant == 5;
  const u32 max_vertices = budget ? 190 : 162;
  const u32 max_primitives = budget ? 152 : 2;
  ShaderVertexInputInfo input;
  input.logical_stage = ShaderType::Mesh;
  input.mesh.threads_num[0] = 256;
  input.mesh.threads_num[1] = input.mesh.threads_num[2] = 1;
  input.mesh.host_subgroup_size = host_subgroup;
  input.mesh.wave_size = 64;
  input.mesh.max_vertices = max_vertices;
  input.mesh.max_primitives = max_primitives;
  input.mesh.provoking_vertex = last ? 2 : 0;
  input.mesh.barycentric_fallback = fallback;
  input.mesh.barycentric_limits = {
      .max_shared_memory_bytes = std::min(properties.maxMeshSharedMemorySize,
                                           properties.maxMeshPayloadAndSharedMemorySize),
      .max_output_memory_bytes = std::min(properties.maxMeshOutputMemorySize,
                                           properties.maxMeshPayloadAndOutputMemorySize),
      .max_output_components = properties.maxMeshOutputComponents,
      .output_vertex_granularity = properties.meshOutputPerVertexGranularity,
      .output_primitive_granularity = properties.meshOutputPerPrimitiveGranularity};
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Mesh;
  options.wave_size = 64;
  options.input_info.vertex = &input;
  ShaderRecompiler::TranslateResult translated;
  auto& program = translated.program;
  program.stage = ShaderType::Mesh;
  program.wave_size = 64;
  program.block_storage.push_back(std::make_unique<IR::Block>());
  auto* block = program.block_storage.back().get();
  program.blocks.push_back(block);
  const auto emit = [&](ValueOpcode op, std::initializer_list<Value> args, uint64_t flags = 0) {
    return Value(&block->AppendNewInst(op, args, flags));
  };
  const auto logical = emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<u32>(IR::StageInputKind::LocalInvocationIndex)), Value(0u)});
  emit(ValueOpcode::MeshAllocate, {Value((max_primitives << 12u) | (empty ? 0u : max_vertices))});
  std::array<Value, 4> selected;
  for (size_t i = 0; i < slots.size(); ++i)
    selected[i] = emit(ValueOpcode::IEqual32, {logical, Value(slots[i])});
  // A 1/8-pixel strip remains nondegenerate even at Vulkan's minimum
  // four-bit subpixel precision; every sampled center stays strictly inside.
  const float y_scale = variant == 3 ? 1.0f / 8.0f : 1.0f;
  std::array<std::array<float, 4>, 4> positions{{
      {-1.0f, -y_scale, variant == 4 ? -0.5f : 0.5f, 1.0f},
      {-2.0f, 2.0f * y_scale, 1.0f, 2.0f},
      {4.0f, -4.0f * y_scale, 2.0f, 4.0f},
      {8.0f, 8.0f * y_scale, 4.0f, 8.0f}}};
  const float homogeneous_scale = variant == 7 ? 1.0e-12f : variant == 8 ? 1.0e12f : 1.0f;
  for (auto& position : positions)
    for (auto& component : position) component *= homogeneous_scale;
  const auto select_word = [&](const std::array<float, 4>& values) {
    Value word(std::bit_cast<u32>(values[0]));
    for (size_t i = 1; i < values.size(); ++i)
      word = emit(ValueOpcode::SelectU32,
                  {selected[i], Value(std::bit_cast<u32>(values[i])), word});
    return word;
  };
  std::array<Value, 4> position_components;
  for (size_t component = 0; component < 4; ++component)
    position_components[component] = select_word({positions[0][component], positions[1][component],
                                                 positions[2][component], positions[3][component]});
  const auto position = emit(ValueOpcode::CompositeConstructU32x4,
      {position_components[0], position_components[1], position_components[2], position_components[3]});
  const auto raw = select_word({11.0f, 22.0f, 33.0f, 44.0f});
  const auto parameter = emit(ValueOpcode::CompositeConstructU32x4,
      {raw, Value(0u), Value(0u), position_components[3]});
  // Rotated primitive orders deliberately differ from both vertex IDs and
  // strip assembly. Vertices 33 and 96 belong to both primitives.
  const u32 packed0 = slots[2] | (slots[0] << 10u) | (slots[1] << 20u) |
      (variant == 1 ? 0x80000000u : 0u);
  const u32 packed1 = slots[3] | (slots[2] << 10u) | (slots[1] << 20u) |
      (variant == 2 ? 0x80000000u : 0u);
  const auto first_primitive = emit(ValueOpcode::IEqual32, {logical, Value(0u)});
  auto packed = emit(ValueOpcode::SelectU32,
      {first_primitive, Value(packed0), Value(packed1)});
  if (budget) {
    const auto visible = emit(ValueOpcode::ULessThan32, {logical, Value(2u)});
    packed = emit(ValueOpcode::SelectU32, {visible, packed, Value(UINT32_MAX)});
  } else if (empty) {
    packed = Value(UINT32_MAX);
  }
  const auto primitive = emit(ValueOpcode::CompositeConstructU32x4,
      {packed, Value(0u), Value(0u), Value(0u)});
  program.export_info = {
      {.kind = IR::ExportTargetKind::Position, .target = 0x0c, .en = 0xf},
      {.kind = IR::ExportTargetKind::Parameter, .target = 0x20, .en = 0xf},
      {.kind = IR::ExportTargetKind::Primitive, .target = 0x14, .en = 1},
  };
  for (const auto [index, data] : std::array<std::pair<u32, Value>, 3>{{
           {0, position}, {1, parameter}, {2, primitive}}}) {
    IR::ExportFlags flags{index, 0};
    uint64_t bits = 0;
    std::memcpy(&bits, &flags, sizeof(flags));
    emit(ValueOpcode::SetAttribute, {data, Value(true)}, bits);
  }
  if (budget) {
    for (u32 location = 1; location < 8; ++location) {
      const auto index = static_cast<u32>(program.export_info.size());
      program.export_info.push_back({.kind = IR::ExportTargetKind::Parameter,
                                    .target = 0x20 + location, .index = location, .en = 0xf});
      IR::ExportFlags flags{index, 0};
      uint64_t bits = 0;
      std::memcpy(&bits, &flags, sizeof(flags));
      emit(ValueOpcode::SetAttribute, {parameter, Value(true)}, bits);
    }
  }
  block->SsaSeal();
  IR::TrackResources(program, {}, {}, options.input_info);
  auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, {});
  ValidateSpirv("MeshBarycentricProduction", compiled.spirv);
  Require("MeshBarycentricProduction", "descriptor-free fixture",
          std::ranges::all_of(compiled.program.bindings.descriptor_counts,
                              [](u32 count) { return count == 0; }),
          "production fallback fixture unexpectedly needs descriptors");
  bool original_vertex_count = false;
  bool declares_cull = false;
  for (size_t cursor = 5; cursor < compiled.spirv.size(); cursor += compiled.spirv[cursor] >> 16u) {
    const auto* instruction = compiled.spirv.data() + cursor;
    if ((instruction[0] & 0xffffu) == spv::OpExecutionMode && (instruction[0] >> 16u) == 4 &&
        instruction[2] == spv::ExecutionModeOutputVertices)
      original_vertex_count = instruction[3] == max_vertices;
    if ((instruction[0] & 0xffffu) == spv::OpDecorate && (instruction[0] >> 16u) == 4 &&
        instruction[2] == spv::DecorationBuiltIn && instruction[3] == spv::BuiltInCullPrimitiveEXT)
      declares_cull = true;
  }
  Require("MeshBarycentricProduction", "preserve shared mesh vertices", original_vertex_count,
          "fallback changed the declared output vertex count");
  Require("MeshBarycentricProduction", "compact primitive storage", declares_cull != fallback->compact,
          "compact culling must use indices without allocating a CullPrimitiveEXT output");
  return compiled.spirv;
}

void MeshBarycentricViewportPush(GraphicsCase& test) {
  const auto viewport = test.viewport_override.value_or(
      vk::Viewport{0.0f, 0.0f, static_cast<float>(test.width), 1.0f, 0.0f, 1.0f});
  test.push_constants.resize(ShaderRecompiler::IR::NativePushConstantSize / sizeof(u32));
  const std::array<float, 4> inverse{
      2.0f / viewport.width, 2.0f / viewport.height,
      -1.0f - 2.0f * viewport.x / viewport.width,
      -1.0f - 2.0f * viewport.y / viewport.height};
  static_assert(ShaderRecompiler::IR::PushData::MeshDrawDwordCount >= 10);
  for (size_t i = 0; i < inverse.size(); ++i)
    test.push_constants[6 + i] = std::bit_cast<u32>(inverse[i]);
}

std::vector<u32> MeshBarycentricBudgetFragment() {
  std::string source = R"(
OpCapability Shader
OpCapability FragmentBarycentricKHR
OpExtension "SPV_KHR_fragment_shader_barycentric"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %bary %color %p0 %p1 %p2 %p3 %p4 %p5 %p6 %p7
OpExecutionMode %main OriginUpperLeft
OpDecorate %bary BuiltIn BaryCoordKHR
OpDecorate %color Location 0
)";
  for (u32 location = 0; location < 8; ++location)
    source += "OpDecorate %p" + std::to_string(location) + " Location " + std::to_string(location) +
              "\nOpDecorate %p" + std::to_string(location) + " Flat\n";
  source += R"(
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%vec3 = OpTypeVector %float 3
%vec4 = OpTypeVector %float 4
%input3 = OpTypePointer Input %vec3
%input4 = OpTypePointer Input %vec4
%output4 = OpTypePointer Output %vec4
%bary = OpVariable %input3 Input
%color = OpVariable %output4 Output
)";
  for (u32 location = 0; location < 8; ++location)
    source += "%p" + std::to_string(location) + " = OpVariable %input4 Input\n";
  source += "%main = OpFunction %void None %function\n%entry = OpLabel\n";
  for (u32 location = 0; location < 8; ++location)
    source += "%v" + std::to_string(location) + " = OpLoad %vec4 %p" + std::to_string(location) + "\n";
  source += "%sum0 = OpFAdd %vec4 %v0 %v1\n";
  for (u32 location = 2; location < 8; ++location)
    source += "%sum" + std::to_string(location - 1) + " = OpFAdd %vec4 %sum" +
              std::to_string(location - 2) + " %v" + std::to_string(location) + "\n";
  source += R"(
%alpha = OpCompositeExtract %float %sum6 0
%coordinates = OpLoad %vec3 %bary
%x = OpCompositeExtract %float %coordinates 0
%y = OpCompositeExtract %float %coordinates 1
%z = OpCompositeExtract %float %coordinates 2
%result = OpCompositeConstruct %vec4 %x %y %z %alpha
OpStore %color %result
OpReturn
OpFunctionEnd
)";
  return AssembleMeshGpuFixture(source);
}

void CheckMeshBarycentricGpu(VulkanHarness& vulkan) {
  const auto& properties = vulkan.MeshProperties();
  std::printf("DIRECT_MESH_BARYCENTRIC_LIMITS vertices=%u primitives=%u components=%u outputBytes=%u payloadOutputBytes=%u vertexGranularity=%u primitiveGranularity=%u\n",
              properties.maxMeshOutputVertices, properties.maxMeshOutputPrimitives,
              properties.maxMeshOutputComponents, properties.maxMeshOutputMemorySize,
              properties.maxMeshPayloadAndOutputMemorySize, properties.meshOutputPerVertexGranularity,
              properties.meshOutputPerPrimitiveGranularity);
  Require("MeshBarycentricProduction", "mesh output limits",
          properties.maxMeshOutputVertices >= 190 && properties.maxMeshOutputPrimitives >= 152,
          "fixture needs up to 190 shared output slots and 152 primitives");
  GraphicContext selection;
  vulkan.MeshSelectionContext(selection);
  const auto selected = selection.SelectMeshSubgroupSize(64, 256);
  Require("MeshBarycentricProduction", "production subgroup", selected != 0,
          "device cannot execute the wave64 production emitter");
  const auto required = selection.mesh_subgroup_size_control_enabled ? selected : 0u;
  const auto limits = vulkan.BarycentricLimits();
  // Screen-space weights are derived from the quad geometry, independently of
  // primitive indices and of the fallback's matrix/basis construction.
  constexpr std::array<std::array<float, 4>, 4> weights_by_vertex{{
      {0.375f, 0.5f, 0.125f, 0.0f}, {0.125f, 0.5f, 0.375f, 0.0f},
      {0.0f, 0.375f, 0.5f, 0.125f}, {0.0f, 0.125f, 0.5f, 0.375f}}};
  constexpr std::array<std::array<unsigned, 3>, 2> primitive_order{{{2, 0, 1}, {3, 2, 1}}};
  constexpr std::array<float, 4> clip_w{1.0f, 2.0f, 4.0f, 8.0f};
  constexpr std::array<std::array<float, 4>, 4> offset_weights{{
      {0.3125f, 0.5625f, 0.125f, 0.0f}, {0.0625f, 0.5625f, 0.375f, 0.0f},
      {0.0f, 0.375f, 0.4375f, 0.1875f}, {0.0f, 0.125f, 0.4375f, 0.4375f}}};
  struct Variant { const char* name; u32 emitter; bool offset = false; bool negative = false; };
  constexpr std::array<Variant, 10> variants{{
      {"Shared", 0}, {"CullFirst", 1}, {"CullSecond", 2}, {"Thin", 3}, {"NearClip", 4},
      {"ZeroVertices", 5}, {"SmallHomogeneousScale", 7}, {"LargeHomogeneousScale", 8},
      {"OffsetViewport", 0, true}, {"NegativeViewport", 0, true, true}}};
  bool matched = true;
  unsigned cases = 0;
  for (const bool per_vertex : {false, true}) {
    for (const bool affine : {false, true}) {
      auto source = BarycentricGpuFragment(affine, per_vertex);
      // The production mesh export parameter0 occupies Location0; the common
      // barycentric fixture uses Location3 for its synthetic VS producer.
      for (size_t cursor = 5; cursor < source.size(); cursor += source[cursor] >> 16u) {
        auto* instruction = source.data() + cursor;
        if ((instruction[0] & 0xffffu) == spv::OpDecorate && (instruction[0] >> 16u) == 4 &&
            instruction[2] == spv::DecorationLocation && instruction[3] == 3)
          instruction[3] = 0;
      }
      ValidateSpirv("MeshBarycentricProduction source fragment", source);
      const auto fragment = BarycentricFallback::PrepareFragment(source, limits);
      Require("MeshBarycentricProduction", "fragment lowering",
              fragment.status == BarycentricFallback::Status::Ready, fragment.reason);
      const auto mesh_fragment = BarycentricFallback::PrepareMeshFragment(fragment.plan, limits);
      Require("MeshBarycentricProduction", "mesh fragment lowering",
              mesh_fragment.status == BarycentricFallback::Status::Ready,
              mesh_fragment.reason);
      const auto plan = std::make_shared<const BarycentricFallback::MeshPlan>(mesh_fragment.plan);
      Require("MeshBarycentricProduction", "compact center interpolation", plan->compact,
              "center-qualified fragment did not select the compact mesh interface");
      CompiledShader compiled_fragment;
      compiled_fragment.spirv = plan->spirv;
      ValidateSpirv("MeshBarycentricProduction lowered fragment", compiled_fragment.spirv);
      for (const bool last : {false, true}) {
        for (const auto& variant : variants) {
          const std::string name = std::string("MeshBarycentric") + (per_vertex ? "Raw" : "Basis") +
              (affine ? "Affine" : "Smooth") + (last ? "Last" : "First") + variant.name;
          const auto mesh = MeshGpuBarycentricEmitter(selected, variant.emitter, last, plan, properties);
          GraphicsCase test;
          test.name = name.c_str();
          test.width = variant.offset ? 8 : 4;
          if (variant.offset)
            test.viewport_override = vk::Viewport{2.0f, variant.negative ? 1.0625f : -0.0625f,
                                                  4.0f, variant.negative ? -1.0f : 1.0f, 0.0f, 1.0f};
          test.provoking_vtx_last = last;
          MeshBarycentricViewportPush(test);
          const auto pixels = vulkan.RenderFragment(test, compiled_fragment, &mesh, nullptr, 1, required);
          Require(test.name, "pixel count", pixels.size() == test.width * 4, "RGBA32F row expected");
          for (size_t pixel = 0; pixel < test.width; ++pixel) {
            const bool outside = variant.offset && (pixel < 2 || pixel >= 6);
            const size_t local_pixel = outside ? 0 : pixel - (variant.offset ? 2 : 0);
            const auto triangle = local_pixel / 2;
            const auto& order = primitive_order[triangle];
            std::array<float, 4> expected{};
            if (!outside && variant.emitter != 5 &&
                !((variant.emitter == 1 && triangle == 0) || (variant.emitter == 2 && triangle == 1))) {
              float sum = 0;
              for (size_t corner = 0; corner < 3; ++corner) {
                expected[corner] = (variant.offset ? offset_weights : weights_by_vertex)[local_pixel][order[corner]] /
                    (affine ? 1.0f : clip_w[order[corner]]);
                sum += expected[corner];
              }
              for (size_t corner = 0; corner < 3; ++corner) expected[corner] /= sum;
              expected[3] = 11.0f * (order[last ? 2 : 0] + 1);
              if (per_vertex) {
                std::array<float, 3> raw{};
                float interpolated = 0;
                for (size_t corner = 0; corner < 3; ++corner) {
                  raw[corner] = 11.0f * (order[corner] + 1);
                  interpolated += raw[corner] * expected[corner];
                }
                expected = {raw[0], raw[1], raw[2], interpolated};
              }
            }
            for (size_t component = 0; component < 4; ++component) {
              const float observed = std::bit_cast<float>(pixels[pixel * 4 + component]);
              std::printf("DIRECT_MESH_BARYCENTRIC_READBACK case=%s pixel=%zu primitive=%zu component=%zu expected=%.9g actual=%.9g\n",
                          test.name, pixel, triangle, component, expected[component], observed);
              matched &= std::isfinite(observed) && std::abs(observed - expected[component]) <= 0.0002f;
            }
          }
          ++cases;
        }
      }
    }
  }
  // This ordinary interface fills the actual failing shape: 190 vertices and
  // 152 primitives with eight live vec4 parameters. Only two primitives are
  // visible; the other 150 carry culled, invalid corner indices.
  const auto budget_source = MeshBarycentricBudgetFragment();
  ValidateSpirv("MeshBarycentricBudget source", budget_source);
  const auto budget_fragment = BarycentricFallback::PrepareFragment(budget_source, limits);
  Require("MeshBarycentricBudget", "fragment lowering",
          budget_fragment.status == BarycentricFallback::Status::Ready, budget_fragment.reason);
  const auto budget_result = BarycentricFallback::PrepareMeshFragment(budget_fragment.plan, limits);
  Require("MeshBarycentricBudget", "compact budget interface",
          budget_result.status == BarycentricFallback::Status::Ready && budget_result.plan.compact &&
              budget_result.plan.ordinary_locations.size() == 8,
          budget_result.reason);
  const auto budget_plan = std::make_shared<const BarycentricFallback::MeshPlan>(budget_result.plan);
  const auto round_count = [](u32 count, u32 granularity) {
    return (count + granularity - 1) / granularity * granularity;
  };
  const auto budget_bytes = 4u * (36u * round_count(190, properties.meshOutputPerVertexGranularity) +
                                   8u * round_count(152, properties.meshOutputPerPrimitiveGranularity));
  static_assert(4u * (36u * 190u + 8u * 152u) == 32224);
  std::printf("DIRECT_MESH_BARYCENTRIC_BUDGET vertices=190 primitives=152 ordinary=8 outputBytes=%u limit=%u\n",
              budget_bytes, std::min(properties.maxMeshOutputMemorySize,
                                     properties.maxMeshPayloadAndOutputMemorySize));
  CompiledShader budget_pixel;
  budget_pixel.spirv = budget_plan->spirv;
  const bool budget_supported = budget_bytes <= std::min(properties.maxMeshOutputMemorySize,
                                                        properties.maxMeshPayloadAndOutputMemorySize);
  if (!budget_supported)
    std::printf("DIRECT_MESH_BARYCENTRIC_BUDGET SKIP: device allocation granularity exceeds output memory limit\n");
  for (const bool last : {false, true}) {
    if (!budget_supported) continue;
    const auto mesh = MeshGpuBarycentricEmitter(selected, 6, last, budget_plan, properties);
    GraphicsCase test;
    test.name = last ? "MeshBarycentricBudgetLast" : "MeshBarycentricBudgetFirst";
    test.width = 4;
    test.provoking_vtx_last = last;
    MeshBarycentricViewportPush(test);
    const auto pixels = vulkan.RenderFragment(test, budget_pixel, &mesh, nullptr, 1, required);
    Require(test.name, "pixel count", pixels.size() == 16, "four budget-fixture pixels expected");
    for (size_t pixel = 0; pixel < 4; ++pixel) {
      const auto& order = primitive_order[pixel / 2];
      std::array<float, 4> expected{};
      float sum = 0;
      for (size_t corner = 0; corner < 3; ++corner) {
        expected[corner] = weights_by_vertex[pixel][order[corner]] / clip_w[order[corner]];
        sum += expected[corner];
      }
      for (size_t corner = 0; corner < 3; ++corner) expected[corner] /= sum;
      expected[3] = 8.0f * 11.0f * (order[last ? 2 : 0] + 1);
      for (size_t component = 0; component < 4; ++component) {
        const float observed = std::bit_cast<float>(pixels[pixel * 4 + component]);
        std::printf("DIRECT_MESH_BARYCENTRIC_READBACK case=%s pixel=%zu primitive=%zu component=%zu expected=%.9g actual=%.9g\n",
                    test.name, pixel, pixel / 2, component, expected[component], observed);
        matched &= std::isfinite(observed) && std::abs(observed - expected[component]) <= 0.0002f;
      }
    }
    ++cases;
  }
  Require("MeshBarycentricProduction", "numeric shared-vertex barycentric/raw readback", matched,
          "primitive basis, raw corners, perspective, culling, clipping or provoking mode changed");
  std::printf("[gpu]     DirectMeshBarycentricFallback ok (%u cases; production wave64/shared vertices/cull/zero/clip/scales/viewports/32224-byte budget)\n", cases);
}

void CheckMeshDriverGpu(VulkanHarness& vulkan) {
  const auto& limits = vulkan.MeshProperties();
  Require("MeshDriver", "minimum mesh limits",
          limits.maxMeshOutputVertices >= 6 && limits.maxMeshOutputPrimitives >= 2 &&
              limits.maxMeshWorkGroupInvocations >= 96 && limits.maxMeshWorkGroupSize[0] >= 96 &&
              limits.maxMeshSharedMemorySize >= 96 * sizeof(u32),
          "fixture requires six vertices, two primitives, 96 invocations and 384 shared bytes");
  CompiledShader fragment;
  fragment.spirv = AssembleMeshGpuFixture(R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %parameter %color
OpExecutionMode %main OriginUpperLeft
OpDecorate %parameter Location 0
OpDecorate %color Location 0
%void = OpTypeVoid
%function = OpTypeFunction %void
%float = OpTypeFloat 32
%vec4 = OpTypeVector %float 4
%input4 = OpTypePointer Input %vec4
%output4 = OpTypePointer Output %vec4
%parameter = OpVariable %input4 Input
%color = OpVariable %output4 Output
%main = OpFunction %void None %function
%entry = OpLabel
%value = OpLoad %vec4 %parameter
OpStore %color %value
OpReturn
OpFunctionEnd
)");
  std::vector<u32> workgroups{32, 96};
  if (limits.maxMeshWorkGroupInvocations >= 192 && limits.maxMeshWorkGroupSize[0] >= 192 &&
      limits.maxMeshSharedMemorySize >= 192 * sizeof(u32)) workgroups.push_back(192);
  unsigned cases = 0;
  for (const u32 threads : workgroups) {
    const auto mesh = MeshGpuProducer(threads);
    for (const u32 mode : {0u, 1u, 2u, 3u}) {
      for (const float input : {0.25f, 0.75f}) {
        const std::string name = "MeshDriverThreads" + std::to_string(threads) +
                                 "Mode" + std::to_string(mode);
        GraphicsCase test;
        test.name = name.c_str();
        test.push_constants = {std::bit_cast<u32>(input), mode};
        const auto pixel = vulkan.RenderFragment(test, fragment, &mesh, nullptr, 1);
        Require(test.name, "readback pixel count", pixel.size() == 4,
                "one mesh-rendered RGBA32F pixel must be returned");
        const std::array<float, 4> expected = mode == 2
            ? std::array<float, 4>{0, 0, 0, 0}
            : std::array<float, 4>{input, 1.0f, 0.5f, mode == 1 ? 0.5f : 0.25f};
        for (size_t component = 0; component < expected.size(); ++component) {
          const float observed = std::bit_cast<float>(pixel[component]);
          std::printf("DIRECT_MESH_READBACK case=%s input=%.2f component=%zu expected=%.9g actual=%.9g\n",
                      test.name, input, component, expected[component], observed);
          Require(test.name, "numeric mesh readback",
                  std::isfinite(observed) && std::abs(observed - expected[component]) <= 0.0001f,
                  "mesh output count, primitive indices/cull, shared visibility or push data changed");
        }
        ++cases;
      }
    }
  }
  GraphicContext selection;
  vulkan.MeshSelectionContext(selection);
  const auto selected = selection.SelectMeshSubgroupSize(64, 256);
  Require("MeshProductionWave64", "select production mesh subgroup", selected != 0,
          "device cannot execute the 256-logical-lane mesh fixture");
  const auto required = selection.mesh_subgroup_size_control_enabled ? selected : 0u;
  for (const u32 base : {0u, 32u, 160u}) {
    const auto mesh = MeshGpuProductionEmitter(selected, base);
    GraphicsCase test;
    test.name = "MeshProductionWave64";
    test.push_constants.resize(ShaderRecompiler::IR::NativePushConstantSize / sizeof(u32));
    const auto pixel = vulkan.RenderFragment(test, fragment, &mesh, nullptr, 1, required);
    Require(test.name, "readback pixel count", pixel.size() == 4,
            "one production-emitter RGBA32F pixel must be returned");
    const std::array<float, 4> expected{
        (float(base % 64) + 0.75f) / 64.0f, (float(base) + 0.75f) / 256.0f, 0.75f, 1.0f};
    for (size_t component = 0; component < expected.size(); ++component) {
      const float observed = std::bit_cast<float>(pixel[component]);
      std::printf("DIRECT_MESH_PRODUCTION_READBACK guest_wave=64 logical_threads=256 host_subgroup=%u required=%u vertex_base=%u component=%zu expected=%.9g actual=%.9g\n",
                  selected, required, base, component, expected[component], observed);
      Require(test.name, "production wave64 numeric readback",
              std::isfinite(observed) && std::abs(observed - expected[component]) <= 0.0001f,
              "logical upper-half lane, later wave or mesh vertex/primitive export changed");
    }
    ++cases;
  }
  std::printf("[gpu]     DirectMeshDriver ok (%u cases, push data/shared memory/counts/indices/culling; native subgroup %u)\n",
              cases, vulkan.SubgroupSize());
  CheckMeshBarycentricGpu(vulkan);
}
