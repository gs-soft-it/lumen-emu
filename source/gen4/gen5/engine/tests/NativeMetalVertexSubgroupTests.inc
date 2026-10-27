// Vertex fixture generated from native_metal_shaders/vertex_subgroup.vert with
// glslangValidator -V --target-env vulkan1.2 -g0, spirv-val and spirv-dis --no-header.
// Each visible triangle samples all three vertices with nonzero barycentric
// weights, so a failed lane check cannot hide behind a later overlapping draw.
void VulkanHarness::CheckNativeVertexSubgroup() {
  using namespace PrimitiveContract;
  constexpr const char *name = "NativeVertexSubgroup";
  const u32 subgroup_size = SubgroupSize();
  Require(name, "subgroup size", subgroup_size > 1 && subgroup_size <= 64,
          "fixture requires a nontrivial subgroup of at most 64 lanes");
  const auto vertex = AssembleNativeMetalContract(R"SPV(
               OpCapability Shader
               OpCapability GroupNonUniform
               OpCapability GroupNonUniformBallot
               OpCapability GroupNonUniformShuffle
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %4 "main" %SubgroupLocalInvocationId %gl_VertexIndex %gl_InstanceIndex %105 %114 %119 %128
               OpDecorate %SubgroupLocalInvocationId RelaxedPrecision
               OpDecorate %SubgroupLocalInvocationId BuiltIn SubgroupLocalInvocationId
               OpDecorate %12 RelaxedPrecision
               OpDecorate %gl_VertexIndex BuiltIn VertexIndex
               OpDecorate %gl_InstanceIndex BuiltIn InstanceIndex
               OpDecorate %_struct_103 Block
               OpMemberDecorate %_struct_103 0 Offset 0
               OpMemberDecorate %_struct_103 1 Offset 16
               OpDecorate %_struct_112 Block
               OpMemberDecorate %_struct_112 0 BuiltIn Position
               OpDecorate %_runtimearr_v4float ArrayStride 16
               OpDecorate %_struct_117 Block
               OpMemberDecorate %_struct_117 0 NonWritable
               OpMemberDecorate %_struct_117 0 Offset 0
               OpDecorate %119 NonWritable
               OpDecorate %119 Binding 0
               OpDecorate %119 DescriptorSet 0
               OpDecorate %128 Location 0
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
          %7 = OpTypeFunction %uint
%_ptr_Input_uint = OpTypePointer Input %uint
%SubgroupLocalInvocationId = OpVariable %_ptr_Input_uint Input
%_ptr_Function_uint = OpTypePointer Function %uint
        %int = OpTypeInt 32 1
%_ptr_Input_int = OpTypePointer Input %int
%gl_VertexIndex = OpVariable %_ptr_Input_int Input
     %v4uint = OpTypeVector %uint 4
%_ptr_Function_v4uint = OpTypePointer Function %v4uint
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
     %uint_3 = OpConstant %uint 3
%_ptr_Function_bool = OpTypePointer Function %bool
    %uint_64 = OpConstant %uint 64
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
%gl_InstanceIndex = OpVariable %_ptr_Input_int Input
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_struct_103 = OpTypeStruct %v4float %v4uint
%_ptr_PushConstant__struct_103 = OpTypePointer PushConstant %_struct_103
        %105 = OpVariable %_ptr_PushConstant__struct_103 PushConstant
      %int_1 = OpConstant %int 1
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
%_struct_112 = OpTypeStruct %v4float
%_ptr_Output__struct_112 = OpTypePointer Output %_struct_112
        %114 = OpVariable %_ptr_Output__struct_112 Output
      %int_0 = OpConstant %int 0
%_runtimearr_v4float = OpTypeRuntimeArray %v4float
%_struct_117 = OpTypeStruct %_runtimearr_v4float
%_ptr_StorageBuffer__struct_117 = OpTypePointer StorageBuffer %_struct_117
        %119 = OpVariable %_ptr_StorageBuffer__struct_117 StorageBuffer
     %uint_2 = OpConstant %uint 2
%_ptr_StorageBuffer_v4float = OpTypePointer StorageBuffer %v4float
%_ptr_Output_v4float = OpTypePointer Output %v4float
        %128 = OpVariable %_ptr_Output_v4float Output
    %float_1 = OpConstant %float 1
    %float_0 = OpConstant %float 0
  %float_0_5 = OpConstant %float 0.5
 %float_0_75 = OpConstant %float 0.75
          %4 = OpFunction %void None %3
          %5 = OpLabel
         %18 = OpVariable %_ptr_Function_uint Function
         %24 = OpVariable %_ptr_Function_uint Function
         %28 = OpVariable %_ptr_Function_v4uint Function
         %34 = OpVariable %_ptr_Function_bool Function
         %36 = OpVariable %_ptr_Function_bool Function
         %71 = OpVariable %_ptr_Function_uint Function
         %73 = OpVariable %_ptr_Function_v4uint Function
         %22 = OpLoad %int %gl_VertexIndex
         %23 = OpBitcast %uint %22
               OpStore %18 %23
         %25 = OpFunctionCall %uint %8
               OpStore %24 %25
         %32 = OpGroupNonUniformBallot %v4uint %uint_3 %true
               OpStore %28 %32
         %35 = OpGroupNonUniformElect %bool %uint_3
               OpStore %34 %35
         %37 = OpLoad %uint %24
         %39 = OpULessThan %bool %37 %uint_64
               OpSelectionMerge %41 None
               OpBranchConditional %39 %40 %41
         %40 = OpLabel
         %42 = OpLoad %v4uint %28
         %43 = OpLoad %uint %24
         %44 = OpGroupNonUniformBallotBitExtract %bool %uint_3 %42 %43
               OpBranch %41
         %41 = OpLabel
         %45 = OpPhi %bool %39 %5 %44 %40
               OpStore %36 %45
         %46 = OpLoad %bool %36
               OpSelectionMerge %48 None
               OpBranchConditional %46 %47 %48
         %47 = OpLabel
         %49 = OpLoad %uint %18
         %50 = OpLoad %uint %24
         %51 = OpGroupNonUniformShuffle %uint %uint_3 %49 %50
         %52 = OpLoad %uint %18
         %53 = OpIEqual %bool %51 %52
               OpBranch %48
         %48 = OpLabel
         %54 = OpPhi %bool %46 %41 %53 %47
               OpStore %36 %54
         %55 = OpLoad %bool %36
               OpSelectionMerge %57 None
               OpBranchConditional %55 %56 %57
         %56 = OpLabel
         %58 = OpLoad %bool %34
         %59 = OpLoad %v4uint %28
         %60 = OpGroupNonUniformBallotBitCount %uint %uint_3 ExclusiveScan %59
         %62 = OpIEqual %bool %60 %uint_0
         %63 = OpLogicalEqual %bool %58 %62
               OpBranch %57
         %57 = OpLabel
         %64 = OpPhi %bool %55 %48 %63 %56
               OpStore %36 %64
         %65 = OpLoad %uint %18
         %67 = OpBitwiseAnd %uint %65 %uint_1
         %68 = OpINotEqual %bool %67 %uint_0
               OpSelectionMerge %70 None
               OpBranchConditional %68 %69 %70
         %69 = OpLabel
         %72 = OpFunctionCall %uint %8
               OpStore %71 %72
         %74 = OpGroupNonUniformBallot %v4uint %uint_3 %true
               OpStore %73 %74
         %75 = OpLoad %bool %36
         %76 = OpLoad %uint %71
         %77 = OpLoad %uint %24
         %78 = OpIEqual %bool %76 %77
         %79 = OpLogicalAnd %bool %75 %78
               OpSelectionMerge %81 None
               OpBranchConditional %79 %80 %81
         %80 = OpLabel
         %82 = OpLoad %v4uint %73
         %83 = OpLoad %uint %71
         %84 = OpGroupNonUniformBallotBitExtract %bool %uint_3 %82 %83
               OpBranch %81
         %81 = OpLabel
         %85 = OpPhi %bool %79 %69 %84 %80
               OpStore %36 %85
         %86 = OpLoad %bool %36
               OpSelectionMerge %88 None
               OpBranchConditional %86 %87 %88
         %87 = OpLabel
         %89 = OpLoad %uint %18
         %90 = OpLoad %uint %71
         %91 = OpGroupNonUniformShuffle %uint %uint_3 %89 %90
         %92 = OpLoad %uint %18
         %93 = OpIEqual %bool %91 %92
               OpBranch %88
         %88 = OpLabel
         %94 = OpPhi %bool %86 %81 %93 %87
               OpStore %36 %94
               OpBranch %70
         %70 = OpLabel
         %95 = OpLoad %bool %36
               OpSelectionMerge %97 None
               OpBranchConditional %95 %96 %97
         %96 = OpLabel
         %99 = OpLoad %int %gl_InstanceIndex
        %100 = OpBitcast %uint %99
        %108 = OpAccessChain %_ptr_PushConstant_uint %105 %int_1 %uint_1
        %109 = OpLoad %uint %108
        %110 = OpIEqual %bool %100 %109
               OpBranch %97
         %97 = OpLabel
        %111 = OpPhi %bool %95 %70 %110 %96
               OpStore %36 %111
        %120 = OpLoad %uint %18
        %122 = OpIMul %uint %120 %uint_2
        %124 = OpAccessChain %_ptr_StorageBuffer_v4float %119 %int_0 %122
        %125 = OpLoad %v4float %124
        %127 = OpAccessChain %_ptr_Output_v4float %114 %int_0
               OpStore %127 %125
        %129 = OpLoad %bool %36
        %132 = OpSelect %float %129 %float_1 %float_0
        %135 = OpCompositeConstruct %v4float %132 %float_0_5 %float_0_75 %float_1
               OpStore %128 %135
               OpReturn
               OpFunctionEnd
          %8 = OpFunction %uint None %7
          %9 = OpLabel
         %14 = OpVariable %_ptr_Function_uint Function
         %12 = OpLoad %uint %SubgroupLocalInvocationId
               OpStore %14 %12
         %15 = OpLoad %uint %14
               OpReturnValue %15
               OpFunctionEnd
)SPV");
  const auto fragment = AssembleNativeMetalContract(PrimitiveContractSpv::Fragment);
  const u32 triangle_count = (2 * subgroup_size + 2) / 3;
  const u32 vertex_count = 3 * triangle_count;
  Require(name, "partial subgroup", vertex_count % subgroup_size != 0 && triangle_count <= 64,
          "fixture must cover full and partial subgroups within the target");
  constexpr u32 first_vertex = 7;
  std::vector<std::array<float, 2>> positions(first_vertex, {8.f, 8.f});
  for (u32 triangle = 0; triangle < triangle_count; ++triangle) {
    const float x = -1.f + float((triangle % 8) * 2) / 8.f;
    const float y = -1.f + float((triangle / 8) * 2) / 8.f;
    positions.push_back({x, y});
    positions.push_back({x + 0.25f, y});
    positions.push_back({x, y + 0.25f});
  }
  const auto vertices = Vertices(positions);
  std::vector<u32> indices{0}; // Nonzero firstIndex and reversed primitive order.
  for (u32 triangle = triangle_count; triangle-- > 0;)
    for (u32 corner = 0; corner < 3; ++corner) indices.push_back(3 * triangle + corner);
  indices.insert(indices.end(), {0, 1, 2}); // Repeated vertices may hit native vertex reuse.
  const u32 negative_first = indices.size();
  for (u32 vertex_index = 0; vertex_index < vertex_count; ++vertex_index)
    indices.push_back(vertex_index + 10);

  auto direct = Draw();
  direct.vertex_count = vertex_count;
  direct.first_vertex = first_vertex;
  direct.first_instance = direct.push[5] = 3;
  auto indexed = direct;
  indexed.indexed = true;
  indexed.first_index = 1;
  indexed.vertex_offset = first_vertex;
  indexed.first_instance = indexed.push[5] = 7;
  auto repeated = indexed;
  repeated.vertex_count += 3;
  auto negative = indexed;
  negative.first_index = negative_first;
  negative.vertex_offset = -3;
  negative.first_instance = negative.push[5] = 11;
  const std::array draws{direct, indexed, repeated, negative, direct};
  const auto frames = RenderPrimitiveContract(name, vertex, {}, {}, fragment, 0,
                                              vertices, indices, draws);
  for (size_t draw = 0; draw < frames.size(); ++draw) {
    for (u32 triangle = 0; triangle < triangle_count; ++triangle) {
      const u32 x = (triangle % 8) * 2;
      const u32 y = (triangle / 8) * 2;
      const std::array<float, 4> expected{1.f, 0.5f, 0.75f, 1.f};
      for (u32 channel = 0; channel < 4; ++channel) {
        const float actual = std::bit_cast<float>(frames[draw][(y * Width + x) * 4 + channel]);
        Require(name, "lane identity through rasterization",
                std::isfinite(actual) && std::abs(actual - expected[channel]) < 0.0001f,
                fmt::format("draw={} triangle={} component={} actual={} expected={}; "
                            "lane/self-shuffle, ballot/election, divergent helper or vertex/instance addressing failed",
                            draw, triangle, channel, actual, expected[channel]));
      }
    }
  }
  std::printf("[gpu]     %-32s ok (subgroup=%u, vertices=%u, draws=%zu, "
              "partial/indexed/repeated/base-instance/divergent-helper)\n",
              name, subgroup_size, vertex_count, draws.size());
}
