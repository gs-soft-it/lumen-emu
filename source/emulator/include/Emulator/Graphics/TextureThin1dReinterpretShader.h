#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURETHIN1DREINTERPRETSHADER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURETHIN1DREINTERPRETSHADER_H_

#include <cstddef>
#include <cstdint>

namespace Lumen::Libs::Graphics::TextureThin1dReinterpretShader {

enum class Direction
{
	Unsupported,
	R8ToRgba8,
	Rgba8ToR8,
	Rgba8ToRgba8
};

// Kept independent of emulator types so the NativeMetal probe can assemble
// and exercise exactly the same modules used by the runtime bridge.
struct PushConstants
{
	uint32_t source_tiles_per_row;
	uint32_t destination_tiles_per_row;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t destination_width;
	uint32_t destination_height;
};

static_assert(sizeof(PushConstants) == 24);

struct Assembly
{
	const char* data = nullptr;
	std::size_t size = 0;
};

// Each RGBA8 destination component addresses the byte at the same raw Thin1D
// offset in the R8 source allocation.
inline constexpr char R8_TO_RGBA8[] = R"spirv(
               OpCapability Shader
               OpCapability StorageImageExtendedFormats
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %global_id
               OpExecutionMode %main LocalSize 8 8 1
               OpDecorate %global_id BuiltIn GlobalInvocationId
               OpDecorate %source_image DescriptorSet 0
               OpDecorate %source_image Binding 0
               OpDecorate %source_image NonWritable
               OpDecorate %destination_image DescriptorSet 0
               OpDecorate %destination_image Binding 1
               OpDecorate %destination_image NonReadable
               OpMemberDecorate %push_block 0 Offset 0
               OpMemberDecorate %push_block 1 Offset 4
               OpMemberDecorate %push_block 2 Offset 8
               OpMemberDecorate %push_block 3 Offset 12
               OpMemberDecorate %push_block 4 Offset 16
               OpMemberDecorate %push_block 5 Offset 20
               OpDecorate %push_block Block

       %void = OpTypeVoid
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
      %float = OpTypeFloat 32
      %v2int = OpTypeVector %int 2
     %v3uint = OpTypeVector %uint 3
    %v4float = OpTypeVector %float 4
   %main_type = OpTypeFunction %void
   %byte_type = OpTypeFunction %float %uint
%source_image_type = OpTypeImage %float 2D 0 0 0 2 R8
%destination_image_type = OpTypeImage %float 2D 0 0 0 2 Rgba8
%ptr_uniform_source = OpTypePointer UniformConstant %source_image_type
%ptr_uniform_destination = OpTypePointer UniformConstant %destination_image_type
%ptr_input_v3uint = OpTypePointer Input %v3uint
%source_image = OpVariable %ptr_uniform_source UniformConstant
%destination_image = OpVariable %ptr_uniform_destination UniformConstant
%global_id = OpVariable %ptr_input_v3uint Input
 %push_block = OpTypeStruct %uint %uint %uint %uint %uint %uint
%ptr_push_block = OpTypePointer PushConstant %push_block
%ptr_push_uint = OpTypePointer PushConstant %uint
       %push = OpVariable %ptr_push_block PushConstant

     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
     %uint_4 = OpConstant %uint 4
     %uint_5 = OpConstant %uint 5
     %uint_6 = OpConstant %uint 6
     %uint_7 = OpConstant %uint 7
     %uint_8 = OpConstant %uint 8
    %uint_16 = OpConstant %uint 16
    %uint_32 = OpConstant %uint 32
    %uint_63 = OpConstant %uint 63
   %uint_256 = OpConstant %uint 256
    %float_0 = OpConstant %float 0

  %load_byte = OpFunction %float None %byte_type
        %raw = OpFunctionParameter %uint
 %byte_entry = OpLabel
       %tile = OpShiftRightLogical %uint %raw %uint_6
    %element = OpBitwiseAnd %uint %raw %uint_63
        %ex0 = OpBitwiseAnd %uint %element %uint_1
        %ex1a = OpBitwiseAnd %uint %element %uint_4
        %ex1 = OpShiftRightLogical %uint %ex1a %uint_1
        %ex2a = OpBitwiseAnd %uint %element %uint_16
        %ex2 = OpShiftRightLogical %uint %ex2a %uint_2
        %ex01 = OpBitwiseOr %uint %ex0 %ex1
          %lx = OpBitwiseOr %uint %ex01 %ex2
        %ey0a = OpBitwiseAnd %uint %element %uint_2
        %ey0 = OpShiftRightLogical %uint %ey0a %uint_1
        %ey1a = OpBitwiseAnd %uint %element %uint_8
        %ey1 = OpShiftRightLogical %uint %ey1a %uint_2
        %ey2a = OpBitwiseAnd %uint %element %uint_32
        %ey2 = OpShiftRightLogical %uint %ey2a %uint_3
        %ey01 = OpBitwiseOr %uint %ey0 %ey1
          %ly = OpBitwiseOr %uint %ey01 %ey2
%src_tiles_ptr = OpAccessChain %ptr_push_uint %push %uint_0
  %src_tiles = OpLoad %uint %src_tiles_ptr
      %tile_x = OpUMod %uint %tile %src_tiles
      %tile_y = OpUDiv %uint %tile %src_tiles
    %tile_x_8 = OpShiftLeftLogical %uint %tile_x %uint_3
    %tile_y_8 = OpShiftLeftLogical %uint %tile_y %uint_3
          %sx = OpIAdd %uint %tile_x_8 %lx
          %sy = OpIAdd %uint %tile_y_8 %ly
%src_width_ptr = OpAccessChain %ptr_push_uint %push %uint_2
   %src_width = OpLoad %uint %src_width_ptr
%src_height_ptr = OpAccessChain %ptr_push_uint %push %uint_3
  %src_height = OpLoad %uint %src_height_ptr
 %src_elements = OpIMul %uint %src_width %src_height
       %raw_ok = OpULessThan %bool %raw %src_elements
        %sx_ok = OpULessThan %bool %sx %src_width
        %sy_ok = OpULessThan %bool %sy %src_height
     %coord_ok = OpLogicalAnd %bool %sx_ok %sy_ok
      %src_ok = OpLogicalAnd %bool %raw_ok %coord_ok
               OpSelectionMerge %byte_merge None
               OpBranchConditional %src_ok %byte_read %byte_zero
  %byte_read = OpLabel
      %sx_int = OpBitcast %int %sx
      %sy_int = OpBitcast %int %sy
   %src_coord = OpCompositeConstruct %v2int %sx_int %sy_int
         %src = OpLoad %source_image_type %source_image
       %pixel = OpImageRead %v4float %src %src_coord
       %value = OpCompositeExtract %float %pixel 0
               OpBranch %byte_merge
  %byte_zero = OpLabel
               OpBranch %byte_merge
 %byte_merge = OpLabel
      %result = OpPhi %float %value %byte_read %float_0 %byte_zero
               OpReturnValue %result
               OpFunctionEnd

       %main = OpFunction %void None %main_type
      %entry = OpLabel
        %gid = OpLoad %v3uint %global_id
          %x = OpCompositeExtract %uint %gid 0
          %y = OpCompositeExtract %uint %gid 1
%dst_width_ptr = OpAccessChain %ptr_push_uint %push %uint_4
   %dst_width = OpLoad %uint %dst_width_ptr
%dst_height_ptr = OpAccessChain %ptr_push_uint %push %uint_5
  %dst_height = OpLoad %uint %dst_height_ptr
        %x_ok = OpULessThan %bool %x %dst_width
        %y_ok = OpULessThan %bool %y %dst_height
       %dst_ok = OpLogicalAnd %bool %x_ok %y_ok
               OpSelectionMerge %main_merge None
               OpBranchConditional %dst_ok %main_write %main_merge
 %main_write = OpLabel
          %xx = OpBitwiseAnd %uint %x %uint_7
          %yy = OpBitwiseAnd %uint %y %uint_7
         %mx0 = OpBitwiseAnd %uint %xx %uint_1
        %my0a = OpBitwiseAnd %uint %yy %uint_1
         %my0 = OpShiftLeftLogical %uint %my0a %uint_1
        %mx1a = OpBitwiseAnd %uint %xx %uint_2
         %mx1 = OpShiftLeftLogical %uint %mx1a %uint_1
        %my1a = OpBitwiseAnd %uint %yy %uint_2
         %my1 = OpShiftLeftLogical %uint %my1a %uint_2
        %mx2a = OpBitwiseAnd %uint %xx %uint_4
         %mx2 = OpShiftLeftLogical %uint %mx2a %uint_2
        %my2a = OpBitwiseAnd %uint %yy %uint_4
         %my2 = OpShiftLeftLogical %uint %my2a %uint_3
        %m01a = OpBitwiseOr %uint %mx0 %my0
        %m01b = OpBitwiseOr %uint %mx1 %my1
        %m01c = OpBitwiseOr %uint %mx2 %my2
         %m01 = OpBitwiseOr %uint %m01a %m01b
      %morton = OpBitwiseOr %uint %m01 %m01c
%dst_tiles_ptr = OpAccessChain %ptr_push_uint %push %uint_1
  %dst_tiles = OpLoad %uint %dst_tiles_ptr
      %tile_x_d = OpShiftRightLogical %uint %x %uint_3
      %tile_y_d = OpShiftRightLogical %uint %y %uint_3
      %tile_row = OpIMul %uint %tile_y_d %dst_tiles
     %dst_tile = OpIAdd %uint %tile_row %tile_x_d
    %tile_base = OpIMul %uint %dst_tile %uint_256
 %element_base = OpShiftLeftLogical %uint %morton %uint_2
     %raw_base = OpIAdd %uint %tile_base %element_base
         %raw1 = OpIAdd %uint %raw_base %uint_1
         %raw2 = OpIAdd %uint %raw_base %uint_2
         %raw3 = OpIAdd %uint %raw_base %uint_3
           %r = OpFunctionCall %float %load_byte %raw_base
           %g = OpFunctionCall %float %load_byte %raw1
           %b = OpFunctionCall %float %load_byte %raw2
           %a = OpFunctionCall %float %load_byte %raw3
       %color = OpCompositeConstruct %v4float %r %g %b %a
       %x_int = OpBitcast %int %x
       %y_int = OpBitcast %int %y
   %dst_coord = OpCompositeConstruct %v2int %x_int %y_int
         %dst = OpLoad %destination_image_type %destination_image
               OpImageWrite %dst %dst_coord %color
               OpBranch %main_merge
  %main_merge = OpLabel
               OpReturn
               OpFunctionEnd
)spirv";

// Each R8 destination texel addresses one raw byte. The byte's low two
// in-tile bits select the RGBA8 source component; the remaining bits select
// the source texel in that source tile.
inline constexpr char RGBA8_TO_R8[] = R"spirv(
               OpCapability Shader
               OpCapability StorageImageExtendedFormats
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %global_id
               OpExecutionMode %main LocalSize 8 8 1
               OpDecorate %global_id BuiltIn GlobalInvocationId
               OpDecorate %source_image DescriptorSet 0
               OpDecorate %source_image Binding 0
               OpDecorate %source_image NonWritable
               OpDecorate %destination_image DescriptorSet 0
               OpDecorate %destination_image Binding 1
               OpDecorate %destination_image NonReadable
               OpMemberDecorate %push_block 0 Offset 0
               OpMemberDecorate %push_block 1 Offset 4
               OpMemberDecorate %push_block 2 Offset 8
               OpMemberDecorate %push_block 3 Offset 12
               OpMemberDecorate %push_block 4 Offset 16
               OpMemberDecorate %push_block 5 Offset 20
               OpDecorate %push_block Block

       %void = OpTypeVoid
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
      %float = OpTypeFloat 32
      %v2int = OpTypeVector %int 2
     %v3uint = OpTypeVector %uint 3
    %v4float = OpTypeVector %float 4
   %main_type = OpTypeFunction %void
%source_image_type = OpTypeImage %float 2D 0 0 0 2 Rgba8
%destination_image_type = OpTypeImage %float 2D 0 0 0 2 R8
%ptr_uniform_source = OpTypePointer UniformConstant %source_image_type
%ptr_uniform_destination = OpTypePointer UniformConstant %destination_image_type
%ptr_input_v3uint = OpTypePointer Input %v3uint
%source_image = OpVariable %ptr_uniform_source UniformConstant
%destination_image = OpVariable %ptr_uniform_destination UniformConstant
%global_id = OpVariable %ptr_input_v3uint Input
 %push_block = OpTypeStruct %uint %uint %uint %uint %uint %uint
%ptr_push_block = OpTypePointer PushConstant %push_block
%ptr_push_uint = OpTypePointer PushConstant %uint
       %push = OpVariable %ptr_push_block PushConstant

     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
     %uint_4 = OpConstant %uint 4
     %uint_5 = OpConstant %uint 5
     %uint_6 = OpConstant %uint 6
     %uint_7 = OpConstant %uint 7
     %uint_8 = OpConstant %uint 8
    %uint_16 = OpConstant %uint 16
    %uint_32 = OpConstant %uint 32
    %uint_63 = OpConstant %uint 63
   %uint_255 = OpConstant %uint 255
    %float_0 = OpConstant %float 0

       %main = OpFunction %void None %main_type
      %entry = OpLabel
        %gid = OpLoad %v3uint %global_id
          %x = OpCompositeExtract %uint %gid 0
          %y = OpCompositeExtract %uint %gid 1
%dst_width_ptr = OpAccessChain %ptr_push_uint %push %uint_4
   %dst_width = OpLoad %uint %dst_width_ptr
%dst_height_ptr = OpAccessChain %ptr_push_uint %push %uint_5
  %dst_height = OpLoad %uint %dst_height_ptr
        %x_ok = OpULessThan %bool %x %dst_width
        %y_ok = OpULessThan %bool %y %dst_height
       %dst_ok = OpLogicalAnd %bool %x_ok %y_ok
               OpSelectionMerge %main_merge None
               OpBranchConditional %dst_ok %map_byte %main_merge
   %map_byte = OpLabel
          %xx = OpBitwiseAnd %uint %x %uint_7
          %yy = OpBitwiseAnd %uint %y %uint_7
         %mx0 = OpBitwiseAnd %uint %xx %uint_1
        %my0a = OpBitwiseAnd %uint %yy %uint_1
         %my0 = OpShiftLeftLogical %uint %my0a %uint_1
        %mx1a = OpBitwiseAnd %uint %xx %uint_2
         %mx1 = OpShiftLeftLogical %uint %mx1a %uint_1
        %my1a = OpBitwiseAnd %uint %yy %uint_2
         %my1 = OpShiftLeftLogical %uint %my1a %uint_2
        %mx2a = OpBitwiseAnd %uint %xx %uint_4
         %mx2 = OpShiftLeftLogical %uint %mx2a %uint_2
        %my2a = OpBitwiseAnd %uint %yy %uint_4
         %my2 = OpShiftLeftLogical %uint %my2a %uint_3
        %m01a = OpBitwiseOr %uint %mx0 %my0
        %m01b = OpBitwiseOr %uint %mx1 %my1
        %m01c = OpBitwiseOr %uint %mx2 %my2
         %m01 = OpBitwiseOr %uint %m01a %m01b
  %dst_morton = OpBitwiseOr %uint %m01 %m01c
%dst_tiles_ptr = OpAccessChain %ptr_push_uint %push %uint_1
  %dst_tiles = OpLoad %uint %dst_tiles_ptr
      %tile_x_d = OpShiftRightLogical %uint %x %uint_3
      %tile_y_d = OpShiftRightLogical %uint %y %uint_3
  %dst_tile_row = OpIMul %uint %tile_y_d %dst_tiles
     %dst_tile = OpIAdd %uint %dst_tile_row %tile_x_d
 %dst_tile_base = OpShiftLeftLogical %uint %dst_tile %uint_6
          %raw = OpIAdd %uint %dst_tile_base %dst_morton
     %src_tile = OpShiftRightLogical %uint %raw %uint_8
%src_byte_in_tile = OpBitwiseAnd %uint %raw %uint_255
   %src_morton = OpShiftRightLogical %uint %src_byte_in_tile %uint_2
      %channel = OpBitwiseAnd %uint %src_byte_in_tile %uint_3
         %sx0 = OpBitwiseAnd %uint %src_morton %uint_1
        %sx1a = OpBitwiseAnd %uint %src_morton %uint_4
         %sx1 = OpShiftRightLogical %uint %sx1a %uint_1
        %sx2a = OpBitwiseAnd %uint %src_morton %uint_16
         %sx2 = OpShiftRightLogical %uint %sx2a %uint_2
        %sx01 = OpBitwiseOr %uint %sx0 %sx1
          %slx = OpBitwiseOr %uint %sx01 %sx2
        %sy0a = OpBitwiseAnd %uint %src_morton %uint_2
         %sy0 = OpShiftRightLogical %uint %sy0a %uint_1
        %sy1a = OpBitwiseAnd %uint %src_morton %uint_8
         %sy1 = OpShiftRightLogical %uint %sy1a %uint_2
        %sy2a = OpBitwiseAnd %uint %src_morton %uint_32
         %sy2 = OpShiftRightLogical %uint %sy2a %uint_3
        %sy01 = OpBitwiseOr %uint %sy0 %sy1
          %sly = OpBitwiseOr %uint %sy01 %sy2
%src_tiles_ptr = OpAccessChain %ptr_push_uint %push %uint_0
  %src_tiles = OpLoad %uint %src_tiles_ptr
      %src_tile_x = OpUMod %uint %src_tile %src_tiles
      %src_tile_y = OpUDiv %uint %src_tile %src_tiles
 %src_tile_x_8 = OpShiftLeftLogical %uint %src_tile_x %uint_3
 %src_tile_y_8 = OpShiftLeftLogical %uint %src_tile_y %uint_3
           %sx = OpIAdd %uint %src_tile_x_8 %slx
           %sy = OpIAdd %uint %src_tile_y_8 %sly
%src_width_ptr = OpAccessChain %ptr_push_uint %push %uint_2
   %src_width = OpLoad %uint %src_width_ptr
%src_height_ptr = OpAccessChain %ptr_push_uint %push %uint_3
  %src_height = OpLoad %uint %src_height_ptr
 %src_elements = OpIMul %uint %src_width %src_height
    %src_bytes = OpShiftLeftLogical %uint %src_elements %uint_2
       %raw_ok = OpULessThan %bool %raw %src_bytes
        %sx_ok = OpULessThan %bool %sx %src_width
        %sy_ok = OpULessThan %bool %sy %src_height
     %coord_ok = OpLogicalAnd %bool %sx_ok %sy_ok
      %src_ok = OpLogicalAnd %bool %raw_ok %coord_ok
               OpSelectionMerge %value_merge None
               OpBranchConditional %src_ok %read_source %zero_value
%read_source = OpLabel
       %sx_int = OpBitcast %int %sx
       %sy_int = OpBitcast %int %sy
    %src_coord = OpCompositeConstruct %v2int %sx_int %sy_int
          %src = OpLoad %source_image_type %source_image
        %pixel = OpImageRead %v4float %src %src_coord
        %value = OpVectorExtractDynamic %float %pixel %channel
                OpBranch %value_merge
 %zero_value = OpLabel
                OpBranch %value_merge
 %value_merge = OpLabel
       %result = OpPhi %float %value %read_source %float_0 %zero_value
        %color = OpCompositeConstruct %v4float %result %float_0 %float_0 %float_0
        %x_int = OpBitcast %int %x
        %y_int = OpBitcast %int %y
    %dst_coord = OpCompositeConstruct %v2int %x_int %y_int
          %dst = OpLoad %destination_image_type %destination_image
                OpImageWrite %dst %dst_coord %color
                OpBranch %main_merge
  %main_merge = OpLabel
                OpReturn
                OpFunctionEnd
)spirv";

// Equal RGBA8 formats share their Morton order inside each 8x8 micro-tile.
// Preserve that local coordinate and the raw tile index while decoding the
// tile's row/column using the source pitch. A rectangle copy would address
// different guest bytes whenever the destination pitch changes.
inline constexpr char RGBA8_TO_RGBA8[] = R"spirv(
               OpCapability Shader
               OpCapability StorageImageExtendedFormats
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %global_id
               OpExecutionMode %main LocalSize 8 8 1
               OpDecorate %global_id BuiltIn GlobalInvocationId
               OpDecorate %source_image DescriptorSet 0
               OpDecorate %source_image Binding 0
               OpDecorate %source_image NonWritable
               OpDecorate %destination_image DescriptorSet 0
               OpDecorate %destination_image Binding 1
               OpDecorate %destination_image NonReadable
               OpMemberDecorate %push_block 0 Offset 0
               OpMemberDecorate %push_block 1 Offset 4
               OpMemberDecorate %push_block 2 Offset 8
               OpMemberDecorate %push_block 3 Offset 12
               OpMemberDecorate %push_block 4 Offset 16
               OpMemberDecorate %push_block 5 Offset 20
               OpDecorate %push_block Block

       %void = OpTypeVoid
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
        %int = OpTypeInt 32 1
      %float = OpTypeFloat 32
      %v2int = OpTypeVector %int 2
     %v3uint = OpTypeVector %uint 3
    %v4float = OpTypeVector %float 4
   %main_type = OpTypeFunction %void
 %image_type = OpTypeImage %float 2D 0 0 0 2 Rgba8
%ptr_uniform_image = OpTypePointer UniformConstant %image_type
%ptr_input_v3uint = OpTypePointer Input %v3uint
%source_image = OpVariable %ptr_uniform_image UniformConstant
%destination_image = OpVariable %ptr_uniform_image UniformConstant
%global_id = OpVariable %ptr_input_v3uint Input
 %push_block = OpTypeStruct %uint %uint %uint %uint %uint %uint
%ptr_push_block = OpTypePointer PushConstant %push_block
%ptr_push_uint = OpTypePointer PushConstant %uint
       %push = OpVariable %ptr_push_block PushConstant

     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
     %uint_4 = OpConstant %uint 4
     %uint_5 = OpConstant %uint 5
     %uint_7 = OpConstant %uint 7
    %float_0 = OpConstant %float 0
      %zero = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_0

       %main = OpFunction %void None %main_type
      %entry = OpLabel
        %gid = OpLoad %v3uint %global_id
          %x = OpCompositeExtract %uint %gid 0
          %y = OpCompositeExtract %uint %gid 1
%dst_width_ptr = OpAccessChain %ptr_push_uint %push %uint_4
   %dst_width = OpLoad %uint %dst_width_ptr
%dst_height_ptr = OpAccessChain %ptr_push_uint %push %uint_5
  %dst_height = OpLoad %uint %dst_height_ptr
        %x_ok = OpULessThan %bool %x %dst_width
        %y_ok = OpULessThan %bool %y %dst_height
      %dst_ok = OpLogicalAnd %bool %x_ok %y_ok
               OpSelectionMerge %main_merge None
               OpBranchConditional %dst_ok %map_tile %main_merge
   %map_tile = OpLabel
%dst_tiles_ptr = OpAccessChain %ptr_push_uint %push %uint_1
  %dst_tiles = OpLoad %uint %dst_tiles_ptr
%src_tiles_ptr = OpAccessChain %ptr_push_uint %push %uint_0
  %src_tiles = OpLoad %uint %src_tiles_ptr
 %dst_tile_x = OpShiftRightLogical %uint %x %uint_3
 %dst_tile_y = OpShiftRightLogical %uint %y %uint_3
%dst_tile_row = OpIMul %uint %dst_tile_y %dst_tiles
   %raw_tile = OpIAdd %uint %dst_tile_row %dst_tile_x
 %src_tile_x = OpUMod %uint %raw_tile %src_tiles
 %src_tile_y = OpUDiv %uint %raw_tile %src_tiles
%src_tile_x_8 = OpShiftLeftLogical %uint %src_tile_x %uint_3
%src_tile_y_8 = OpShiftLeftLogical %uint %src_tile_y %uint_3
         %lx = OpBitwiseAnd %uint %x %uint_7
         %ly = OpBitwiseAnd %uint %y %uint_7
         %sx = OpIAdd %uint %src_tile_x_8 %lx
         %sy = OpIAdd %uint %src_tile_y_8 %ly
%src_width_ptr = OpAccessChain %ptr_push_uint %push %uint_2
   %src_width = OpLoad %uint %src_width_ptr
%src_height_ptr = OpAccessChain %ptr_push_uint %push %uint_3
  %src_height = OpLoad %uint %src_height_ptr
       %sx_ok = OpULessThan %bool %sx %src_width
       %sy_ok = OpULessThan %bool %sy %src_height
      %src_ok = OpLogicalAnd %bool %sx_ok %sy_ok
               OpSelectionMerge %value_merge None
               OpBranchConditional %src_ok %read_source %zero_value
%read_source = OpLabel
      %sx_int = OpBitcast %int %sx
      %sy_int = OpBitcast %int %sy
   %src_coord = OpCompositeConstruct %v2int %sx_int %sy_int
         %src = OpLoad %image_type %source_image
       %pixel = OpImageRead %v4float %src %src_coord
               OpBranch %value_merge
 %zero_value = OpLabel
               OpBranch %value_merge
%value_merge = OpLabel
       %color = OpPhi %v4float %pixel %read_source %zero %zero_value
       %x_int = OpBitcast %int %x
       %y_int = OpBitcast %int %y
   %dst_coord = OpCompositeConstruct %v2int %x_int %y_int
         %dst = OpLoad %image_type %destination_image
               OpImageWrite %dst %dst_coord %color
               OpBranch %main_merge
 %main_merge = OpLabel
               OpReturn
               OpFunctionEnd
)spirv";

[[nodiscard]] constexpr Assembly GetAssembly(Direction direction)
{
	switch (direction)
	{
		case Direction::R8ToRgba8: return {R8_TO_RGBA8, sizeof(R8_TO_RGBA8) - 1u};
		case Direction::Rgba8ToR8: return {RGBA8_TO_R8, sizeof(RGBA8_TO_R8) - 1u};
		case Direction::Rgba8ToRgba8: return {RGBA8_TO_RGBA8, sizeof(RGBA8_TO_RGBA8) - 1u};
		case Direction::Unsupported: break;
	}
	return {};
}

} // namespace Lumen::Libs::Graphics::TextureThin1dReinterpretShader

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURETHIN1DREINTERPRETSHADER_H_ */
