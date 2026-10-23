#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr uint64_t kCompletionTimeoutNanoseconds = 5'000'000'000;
constexpr uint32_t kUnsetOutput                  = 0xdeadbeefu;

struct ProbeCase
{
	uint32_t lhs;
	uint32_t rhs;
	uint32_t output;
};

static_assert(sizeof(ProbeCase) == 3 * sizeof(uint32_t));

// Inputs are uploaded as integer bits and are converted to F32 only inside the
// ordinary branch. In particular, the signaling-NaN cases cannot be consumed
// by the multiply on the legacy-zero path. The boundary cases exercise the
// exact 24-bit significand product immediately below, at, and above the RNE
// threshold, with operand order and result sign varied independently.
constexpr std::array<ProbeCase, 55> kInitialCases = {{
    {0x00000000u, 0x7f800000u, kUnsetOutput}, // +0, +inf
    {0xff800000u, 0x80000000u, kUnsetOutput}, // -inf, -0
    {0x80000000u, 0x7fc12345u, kUnsetOutput}, // -0, +qNaN
    {0xffc54321u, 0x00000000u, kUnsetOutput}, // -qNaN, +0
    {0x00000000u, 0xff812345u, kUnsetOutput}, // +0, -sNaN
    {0x7f854321u, 0x80000000u, kUnsetOutput}, // +sNaN, -0
    {0x00000001u, 0x7f800000u, kUnsetOutput}, // +minimum subnormal, +inf
    {0xff800000u, 0x807fffffu, kUnsetOutput}, // -inf, -maximum subnormal
    {0x80000001u, 0x7fc12345u, kUnsetOutput}, // -minimum subnormal, +qNaN
    {0xffc54321u, 0x007fffffu, kUnsetOutput}, // -qNaN, +maximum subnormal
    {0x007fffffu, 0xff812345u, kUnsetOutput}, // +maximum subnormal, -sNaN
    {0x7f854321u, 0x80000001u, kUnsetOutput}, // +sNaN, -minimum subnormal
    {0x80000000u, 0xc0400000u, kUnsetOutput}, // -0, -3
    {0x40800000u, 0x807fffffu, kUnsetOutput}, // +4, -maximum subnormal
    {0x3fc00000u, 0x40000000u, kUnsetOutput}, // +1.5, +2
    {0xbfc00000u, 0x40000000u, kUnsetOutput}, // -1.5, +2
    {0x3f800001u, 0x3fc00000u, kUnsetOutput}, // nearest-even tie rounds upward
    {0x3f800003u, 0x3fc00000u, kUnsetOutput}, // nearest-even tie rounds downward
    {0x00800000u, 0x3f000000u, kUnsetOutput}, // positive subnormal product
    {0x80800000u, 0x3f000000u, kUnsetOutput}, // negative subnormal product
    {0x00800000u, 0x3f7ffffeu, kUnsetOutput}, // boundary below, positive
    {0x3f7ffffeu, 0x00800000u, kUnsetOutput}, // boundary below, positive swapped
    {0x80800000u, 0x3f7ffffeu, kUnsetOutput}, // boundary below, negative
    {0x3f7ffffeu, 0x80800000u, kUnsetOutput}, // boundary below, negative swapped
    {0x80800000u, 0xbf7ffffeu, kUnsetOutput}, // boundary below, both negative
    {0xbf7ffffeu, 0x80800000u, kUnsetOutput}, // boundary below, both negative swapped
    {0x00800001u, 0x3f7ffffdu, kUnsetOutput}, // boundary oracle L-3
    {0x3f7ffffdu, 0x00800001u, kUnsetOutput}, // boundary oracle L-3 swapped
    {0x00800000u, 0x3f7fffffu, kUnsetOutput}, // boundary tie, positive
    {0x3f7fffffu, 0x00800000u, kUnsetOutput}, // boundary tie, positive swapped
    {0x80800000u, 0x3f7fffffu, kUnsetOutput}, // boundary tie, negative
    {0x3f7fffffu, 0x80800000u, kUnsetOutput}, // boundary tie, negative swapped
    {0x80800000u, 0xbf7fffffu, kUnsetOutput}, // boundary tie, both negative
    {0xbf7fffffu, 0x80800000u, kUnsetOutput}, // boundary tie, both negative swapped
    {0x00800001u, 0x3f7ffffeu, kUnsetOutput}, // boundary above, positive
    {0x3f7ffffeu, 0x00800001u, kUnsetOutput}, // boundary above, positive swapped
    {0x80800001u, 0x3f7ffffeu, kUnsetOutput}, // boundary above, negative
    {0x3f7ffffeu, 0x80800001u, kUnsetOutput}, // boundary above, negative swapped
    {0x80800001u, 0xbf7ffffeu, kUnsetOutput}, // boundary above, both negative
    {0xbf7ffffeu, 0x80800001u, kUnsetOutput}, // boundary above, both negative swapped
    {0x00801802u, 0x3f7fd004u, kUnsetOutput}, // boundary oracle L+8
    {0x3f7fd004u, 0x00801802u, kUnsetOutput}, // boundary oracle L+8 swapped
    {0x00ffffffu, 0x3effffffu, kUnsetOutput}, // maximum significands with exponent sum 126
    {0x3effffffu, 0x00ffffffu, kUnsetOutput}, // maximum exponent-sum-126 case swapped
    {0x80ffffffu, 0x3effffffu, kUnsetOutput}, // negative maximum exponent-sum-126 case
    {0x3effffffu, 0x80ffffffu, kUnsetOutput}, // negative exponent-sum-126 case swapped
    {0x00800000u, 0x3f800000u, kUnsetOutput}, // exact minimum normal
    {0x3f800000u, 0x00800000u, kUnsetOutput}, // exact minimum normal swapped
    {0x80800000u, 0x3f800000u, kUnsetOutput}, // exact negative minimum normal
    {0x3f800000u, 0x80800000u, kUnsetOutput}, // exact negative minimum normal swapped
    {0xc0200000u, 0xc0800000u, kUnsetOutput}, // -2.5, -4
    {0x00800000u, 0x33800000u, kUnsetOutput}, // underflow tie to +0
    {0x00800000u, 0xb3800000u, kUnsetOutput}, // underflow tie to -0
    {0x7f7fffffu, 0x40000000u, kUnsetOutput}, // positive overflow
    {0xff7fffffu, 0x40000000u, kUnsetOutput}, // negative overflow
}};

constexpr std::array<uint32_t, kInitialCases.size()> kExpectedOutputs = {
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x40400000u, 0xc0400000u, 0x3fc00002u, 0x3fc00004u, 0x00000000u, 0x80000000u,
    0x00000000u, 0x00000000u, 0x80000000u, 0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00800000u, 0x00800000u,
    0x80800000u, 0x80800000u, 0x00800000u, 0x00800000u, 0x00800000u, 0x00800000u, 0x80800000u, 0x80800000u, 0x00800000u, 0x00800000u,
    0x00800000u, 0x00800000u, 0x00000000u, 0x00000000u, 0x80000000u, 0x80000000u, 0x00800000u, 0x00800000u, 0x80800000u, 0x80800000u,
    0x41200000u, 0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u,
};

constexpr std::array<const char*, kInitialCases.size()> kCaseNames = {
    "zero-inf-left",
    "zero-inf-right",
    "zero-qnan-left",
    "zero-qnan-right",
    "zero-snan-left",
    "zero-snan-right",
    "subnormal-inf-left",
    "subnormal-inf-right",
    "subnormal-qnan-left",
    "subnormal-qnan-right",
    "subnormal-snan-left",
    "subnormal-snan-right",
    "zero-normal",
    "subnormal-normal",
    "normal-positive",
    "normal-negative",
    "nearest-even-up",
    "nearest-even-down",
    "output-subnormal-positive",
    "output-subnormal-negative",
    "boundary-below-positive",
    "boundary-below-positive-swapped",
    "boundary-below-negative",
    "boundary-below-negative-swapped",
    "boundary-below-both-negative",
    "boundary-below-both-negative-swapped",
    "boundary-oracle-l-minus-3",
    "boundary-oracle-l-minus-3-swapped",
    "boundary-tie-positive",
    "boundary-tie-positive-swapped",
    "boundary-tie-negative",
    "boundary-tie-negative-swapped",
    "boundary-tie-both-negative",
    "boundary-tie-both-negative-swapped",
    "boundary-above-positive",
    "boundary-above-positive-swapped",
    "boundary-above-negative",
    "boundary-above-negative-swapped",
    "boundary-above-both-negative",
    "boundary-above-both-negative-swapped",
    "boundary-oracle-l-plus-8",
    "boundary-oracle-l-plus-8-swapped",
    "maximum-s126-positive",
    "maximum-s126-positive-swapped",
    "maximum-s126-negative",
    "maximum-s126-negative-swapped",
    "minimum-normal-positive",
    "minimum-normal-positive-swapped",
    "minimum-normal-negative",
    "minimum-normal-negative-swapped",
    "normal-signs",
    "underflow-zero-positive",
    "underflow-zero-negative",
    "overflow-positive",
    "overflow-negative",
};

// The source flush is materialized as integer selects before the control-flow
// split. Only the ordinary block converts the flushed words to float and owns
// the single OpFMul. If the native product has exponent zero, a single extended
// integer significand multiply recovers the one RNE boundary that rounds to a
// normal result before output flush. The merge phis prevent an eager FMul from
// being part of the special +0 value path.
constexpr char kComputeShaderAssembly[] = R"spirv(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %global_id
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %global_id BuiltIn GlobalInvocationId
               OpDecorate %words ArrayStride 4
               OpMemberDecorate %buffer_block 0 Offset 0
               OpDecorate %buffer_block BufferBlock
               OpDecorate %storage DescriptorSet 0
               OpDecorate %storage Binding 0
               OpDecorate %product NoContraction

       %void = OpTypeVoid
    %function = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
      %float = OpTypeFloat 32
     %v3uint = OpTypeVector %uint 3
  %uint_pair = OpTypeStruct %uint %uint
      %words = OpTypeRuntimeArray %uint
%buffer_block = OpTypeStruct %words
%ptr_uniform_buffer = OpTypePointer Uniform %buffer_block
%ptr_uniform_uint = OpTypePointer Uniform %uint
%ptr_input_v3uint = OpTypePointer Input %v3uint
    %storage = OpVariable %ptr_uniform_buffer Uniform
  %global_id = OpVariable %ptr_input_v3uint Input

     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
     %uint_8 = OpConstant %uint 8
    %uint_23 = OpConstant %uint 23
   %uint_127 = OpConstant %uint 127
   %abs_mask = OpConstant %uint 2147483647
  %sign_mask = OpConstant %uint 2147483648
 %min_normal = OpConstant %uint 8388608
 %boundary_hi = OpConstant %uint 32767
 %boundary_lo = OpConstant %uint 4286578688

       %main = OpFunction %void None %function
      %entry = OpLabel
         %gid = OpLoad %v3uint %global_id
  %case_index = OpCompositeExtract %uint %gid 0
        %base = OpIMul %uint %case_index %uint_3
   %rhs_index = OpIAdd %uint %base %uint_1
%output_index = OpIAdd %uint %base %uint_2
     %lhs_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %base
     %rhs_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %rhs_index
    %lhs_bits = OpLoad %uint %lhs_ptr
    %rhs_bits = OpLoad %uint %rhs_ptr
     %lhs_abs = OpBitwiseAnd %uint %lhs_bits %abs_mask
     %rhs_abs = OpBitwiseAnd %uint %rhs_bits %abs_mask
%lhs_zero_or_subnormal = OpULessThan %bool %lhs_abs %min_normal
%rhs_zero_or_subnormal = OpULessThan %bool %rhs_abs %min_normal
    %lhs_sign = OpBitwiseAnd %uint %lhs_bits %sign_mask
    %rhs_sign = OpBitwiseAnd %uint %rhs_bits %sign_mask
%lhs_flushed_bits = OpSelect %uint %lhs_zero_or_subnormal %lhs_sign %lhs_bits
%rhs_flushed_bits = OpSelect %uint %rhs_zero_or_subnormal %rhs_sign %rhs_bits
%lhs_effective_abs = OpBitwiseAnd %uint %lhs_flushed_bits %abs_mask
%rhs_effective_abs = OpBitwiseAnd %uint %rhs_flushed_bits %abs_mask
%lhs_effective_zero = OpIEqual %bool %lhs_effective_abs %uint_0
%rhs_effective_zero = OpIEqual %bool %rhs_effective_abs %uint_0
%has_effective_zero = OpLogicalOr %bool %lhs_effective_zero %rhs_effective_zero
               OpSelectionMerge %merge None
               OpBranchConditional %has_effective_zero %special_zero %ordinary

%special_zero = OpLabel
               OpBranch %merge

   %ordinary = OpLabel
   %lhs_float = OpBitcast %float %lhs_flushed_bits
   %rhs_float = OpBitcast %float %rhs_flushed_bits
     %product = OpFMul %float %lhs_float %rhs_float
%product_bits = OpBitcast %uint %product
 %product_exp = OpBitFieldUExtract %uint %product_bits %uint_23 %uint_8
%product_exp_zero = OpIEqual %bool %product_exp %uint_0
               OpSelectionMerge %output_merge None
               OpBranchConditional %product_exp_zero %underflow %keep_product

  %underflow = OpLabel
%signed_zero_bits = OpBitwiseXor %uint %lhs_sign %rhs_sign
     %lhs_exp = OpBitFieldUExtract %uint %lhs_flushed_bits %uint_23 %uint_8
     %rhs_exp = OpBitFieldUExtract %uint %rhs_flushed_bits %uint_23 %uint_8
    %lhs_frac = OpBitFieldUExtract %uint %lhs_flushed_bits %uint_0 %uint_23
    %rhs_frac = OpBitFieldUExtract %uint %rhs_flushed_bits %uint_0 %uint_23
     %lhs_sig = OpBitwiseOr %uint %lhs_frac %min_normal
     %rhs_sig = OpBitwiseOr %uint %rhs_frac %min_normal
     %exp_sum = OpIAdd %uint %lhs_exp %rhs_exp
%boundary_exp = OpIEqual %bool %exp_sum %uint_127
 %sig_product = OpUMulExtended %uint_pair %lhs_sig %rhs_sig
      %sig_lo = OpCompositeExtract %uint %sig_product 0
      %sig_hi = OpCompositeExtract %uint %sig_product 1
%boundary_hi_match = OpIEqual %bool %sig_hi %boundary_hi
%boundary_lo_match = OpUGreaterThanEqual %bool %sig_lo %boundary_lo
%boundary_product = OpLogicalAnd %bool %boundary_hi_match %boundary_lo_match
%rounds_normal = OpLogicalAnd %bool %boundary_exp %boundary_product
%signed_min_normal_bits = OpBitwiseOr %uint %signed_zero_bits %min_normal
%underflow_result = OpSelect %uint %rounds_normal %signed_min_normal_bits %signed_zero_bits
               OpBranch %output_merge

%keep_product = OpLabel
               OpBranch %output_merge

%output_merge = OpLabel
%ordinary_result = OpPhi %uint %underflow_result %underflow %product_bits %keep_product
               OpBranch %merge

      %merge = OpLabel
%result_bits = OpPhi %uint %uint_0 %special_zero %ordinary_result %output_merge
  %output_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %output_index
               OpStore %output_ptr %result_bits
               OpReturn
               OpFunctionEnd
)spirv";

struct ProbeState
{
	VkInstance            instance            = VK_NULL_HANDLE;
	VkDevice              device              = VK_NULL_HANDLE;
	VkQueue               queue               = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool      descriptorPool      = VK_NULL_HANDLE;
	VkDescriptorSet       descriptorSet       = VK_NULL_HANDLE;
	VkPipelineLayout      pipelineLayout      = VK_NULL_HANDLE;
	VkShaderModule        shaderModule        = VK_NULL_HANDLE;
	VkPipeline            pipeline            = VK_NULL_HANDLE;
	VkBuffer              buffer              = VK_NULL_HANDLE;
	VkDeviceMemory        memory              = VK_NULL_HANDLE;
	void*                 mapped              = nullptr;
	VkCommandPool         commandPool         = VK_NULL_HANDLE;
	VkCommandBuffer       commandBuffer       = VK_NULL_HANDLE;
	VkFence               fence               = VK_NULL_HANDLE;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE)
		{
			vkDeviceWaitIdle(device);
		}
		if (mapped != nullptr)
		{
			vkUnmapMemory(device, memory);
		}
		if (fence != VK_NULL_HANDLE)
		{
			vkDestroyFence(device, fence, nullptr);
		}
		if (commandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(device, commandPool, nullptr);
		}
		if (pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, pipeline, nullptr);
		}
		if (shaderModule != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
		}
		if (pipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		}
		if (descriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		}
		if (descriptorSetLayout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		}
		if (buffer != VK_NULL_HANDLE)
		{
			vkDestroyBuffer(device, buffer, nullptr);
		}
		if (memory != VK_NULL_HANDLE)
		{
			vkFreeMemory(device, memory, nullptr);
		}
		if (device != VK_NULL_HANDLE)
		{
			vkDestroyDevice(device, nullptr);
		}
		if (instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(instance, nullptr);
		}
	}
};

int Fail(const char* operation, VkResult result = VK_SUCCESS)
{
	if (result == VK_SUCCESS)
	{
		std::fprintf(stderr, "%s\n", operation);
	} else
	{
		std::fprintf(stderr, "%s: %d\n", operation, result);
	}
	return 1;
}

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((typeBits & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & required) == required)
		{
			return index;
		}
	}
	return UINT32_MAX;
}

size_t CountToken(std::string_view text, std::string_view token)
{
	size_t count    = 0;
	size_t position = 0;
	while ((position = text.find(token, position)) != std::string_view::npos)
	{
		++count;
		position += token.size();
	}
	return count;
}

bool HasLegacyMultiplyShape(std::string_view disassembly)
{
	const size_t valueSelection  = disassembly.find("OpSelectionMerge");
	const size_t valueBranch     = disassembly.find("OpBranchConditional", valueSelection);
	const size_t multiply        = disassembly.find("OpFMul", valueBranch);
	const size_t productExponent = disassembly.find("OpBitFieldUExtract", multiply);
	const size_t outputSelection = disassembly.find("OpSelectionMerge", productExponent);
	const size_t extendedProduct = disassembly.find("OpUMulExtended", outputSelection);
	const size_t lowThreshold    = disassembly.find("OpUGreaterThanEqual", extendedProduct);
	const size_t ordinaryPhi     = disassembly.find("OpPhi %uint", lowThreshold);
	const size_t valuePhi        = disassembly.find("OpPhi %uint", ordinaryPhi + 1);

	const bool ordered = valueSelection != std::string_view::npos && valueBranch != std::string_view::npos &&
	                     multiply != std::string_view::npos && productExponent != std::string_view::npos &&
	                     outputSelection != std::string_view::npos && extendedProduct != std::string_view::npos &&
	                     lowThreshold != std::string_view::npos && ordinaryPhi != std::string_view::npos &&
	                     valuePhi != std::string_view::npos && valueSelection < valueBranch && valueBranch < multiply &&
	                     multiply < productExponent && productExponent < outputSelection && outputSelection < extendedProduct &&
	                     extendedProduct < lowThreshold && lowThreshold < ordinaryPhi && ordinaryPhi < valuePhi;
	const bool exactIntegerCorrection =
	    CountToken(disassembly, "OpFMul") == 1 && CountToken(disassembly, "OpUMulExtended") == 1 &&
	    CountToken(disassembly, "NoContraction") == 1 && CountToken(disassembly, "OpIAdd") == 3 &&
	    CountToken(disassembly, "OpIEqual") == 5 && CountToken(disassembly, "OpBitFieldUExtract") == 5 &&
	    CountToken(disassembly, "OpCompositeExtract") == 3 && CountToken(disassembly, "OpUGreaterThanEqual") == 1 &&
	    CountToken(disassembly, "OpLogicalAnd") == 2 && CountToken(disassembly, "OpBitwiseXor") == 1 &&
	    CountToken(disassembly, "OpBitwiseOr") == 3 && CountToken(disassembly, "OpPhi %uint") == 2 &&
	    CountToken(disassembly, "OpSelect %uint") == 3 && disassembly.find("OpConstant %uint 127") != std::string_view::npos &&
	    disassembly.find("OpConstant %uint 8388608") != std::string_view::npos &&
	    disassembly.find("OpConstant %uint 32767") != std::string_view::npos &&
	    disassembly.find("OpConstant %uint 4286578688") != std::string_view::npos;
	const bool noFloatingComparison = disassembly.find("OpFOrd") == std::string_view::npos &&
	                                  disassembly.find("OpFUnord") == std::string_view::npos &&
	                                  disassembly.find("OpSelect %float") == std::string_view::npos;
	return ordered && exactIntegerCorrection && noFloatingComparison;
}
} // namespace

int main()
{
	spvtools::SpirvTools spirvTools(SPV_ENV_VULKAN_1_1);
	spirvTools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                              { std::fprintf(stderr, "SPIR-V line %zu column %zu: %s\n", position.line, position.column, message); });

	std::vector<uint32_t> shaderBinary;
	if (!spirvTools.Assemble(kComputeShaderAssembly, sizeof(kComputeShaderAssembly) - 1, &shaderBinary))
	{
		return Fail("V_MUL_LEGACY_F32 backend SPIR-V assembly failed");
	}
	if (!spirvTools.Validate(shaderBinary))
	{
		return Fail("V_MUL_LEGACY_F32 backend SPIR-V validation failed");
	}
	std::string disassembly;
	if (!spirvTools.Disassemble(shaderBinary, &disassembly) || !HasLegacyMultiplyShape(disassembly))
	{
		return Fail("V_MUL_LEGACY_F32 backend SPIR-V lost its non-eager integer-boundary shape");
	}

	ProbeState        state;
	VkApplicationInfo applicationInfo {};
	applicationInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName   = "Lumen V_MUL_LEGACY_F32 compute probe";
	applicationInfo.applicationVersion = 1;
	applicationInfo.pEngineName        = "Lumen";
	applicationInfo.engineVersion      = 1;
	applicationInfo.apiVersion         = VK_API_VERSION_1_2;
	VkInstanceCreateInfo instanceInfo {};
	instanceInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &applicationInfo;
	VkResult result               = vkCreateInstance(&instanceInfo, nullptr, &state.instance);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateInstance failed", result);
	}

	uint32_t physicalDeviceCount = 0;
	result                       = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0)
	{
		return Fail("No NativeMetal physical device found", result);
	}
	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS)
	{
		return Fail("vkEnumeratePhysicalDevices(list) failed", result);
	}
	physicalDevices.resize(physicalDeviceCount);

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	uint32_t         queueFamily    = UINT32_MAX;
	for (VkPhysicalDevice candidate: physicalDevices)
	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
		for (uint32_t family = 0; family < familyCount; ++family)
		{
			if (families[family].queueCount != 0 && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
			{
				physicalDevice = candidate;
				queueFamily    = family;
				break;
			}
		}
		if (physicalDevice != VK_NULL_HANDLE)
		{
			break;
		}
	}
	if (physicalDevice == VK_NULL_HANDLE)
	{
		return Fail("No compute-capable NativeMetal queue found");
	}

	constexpr float         queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos    = &queueInfo;
	result                          = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &state.device);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDevice failed", result);
	}
	vkGetDeviceQueue(state.device, queueFamily, 0, &state.queue);
	if (state.queue == VK_NULL_HANDLE)
	{
		return Fail("vkGetDeviceQueue returned a null queue");
	}

	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size        = sizeof(kInitialCases);
	bufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result                 = vkCreateBuffer(state.device, &bufferInfo, nullptr, &state.buffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateBuffer(storage) failed", result);
	}
	VkMemoryRequirements memoryRequirements {};
	vkGetBufferMemoryRequirements(state.device, state.buffer, &memoryRequirements);
	const uint32_t memoryType = FindMemoryType(physicalDevice, memoryRequirements.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX)
	{
		return Fail("No coherent host-visible memory type for storage buffer");
	}
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = memoryRequirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result                         = vkAllocateMemory(state.device, &allocationInfo, nullptr, &state.memory);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateMemory(storage) failed", result);
	}
	result = vkBindBufferMemory(state.device, state.buffer, state.memory, 0);
	if (result != VK_SUCCESS)
	{
		return Fail("vkBindBufferMemory(storage) failed", result);
	}
	result = vkMapMemory(state.device, state.memory, 0, VK_WHOLE_SIZE, 0, &state.mapped);
	if (result != VK_SUCCESS || state.mapped == nullptr)
	{
		return Fail("vkMapMemory(storage) failed", result);
	}
	std::memcpy(state.mapped, kInitialCases.data(), sizeof(kInitialCases));
	VkMappedMemoryRange mappedRange {};
	mappedRange.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	mappedRange.memory = state.memory;
	mappedRange.size   = VK_WHOLE_SIZE;
	result             = vkFlushMappedMemoryRanges(state.device, 1, &mappedRange);
	if (result != VK_SUCCESS)
	{
		return Fail("vkFlushMappedMemoryRanges(storage) failed", result);
	}

	VkDescriptorSetLayoutBinding descriptorBinding {};
	descriptorBinding.binding         = 0;
	descriptorBinding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorBinding.descriptorCount = 1;
	descriptorBinding.stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo {};
	descriptorLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptorLayoutInfo.bindingCount = 1;
	descriptorLayoutInfo.pBindings    = &descriptorBinding;
	result = vkCreateDescriptorSetLayout(state.device, &descriptorLayoutInfo, nullptr, &state.descriptorSetLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorSetLayout(storage) failed", result);
	}

	VkPipelineLayoutCreateInfo pipelineLayoutInfo {};
	pipelineLayoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts    = &state.descriptorSetLayout;
	result                            = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreatePipelineLayout(compute) failed", result);
	}

	VkDescriptorPoolSize poolSize {};
	poolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = 1;
	VkDescriptorPoolCreateInfo poolInfo {};
	poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets       = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes    = &poolSize;
	result                 = vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorPool(storage) failed", result);
	}
	VkDescriptorSetAllocateInfo descriptorSetInfo {};
	descriptorSetInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetInfo.descriptorPool     = state.descriptorPool;
	descriptorSetInfo.descriptorSetCount = 1;
	descriptorSetInfo.pSetLayouts        = &state.descriptorSetLayout;
	result                               = vkAllocateDescriptorSets(state.device, &descriptorSetInfo, &state.descriptorSet);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateDescriptorSets(storage) failed", result);
	}
	VkDescriptorBufferInfo descriptorBuffer {};
	descriptorBuffer.buffer = state.buffer;
	descriptorBuffer.range  = sizeof(kInitialCases);
	VkWriteDescriptorSet descriptorWrite {};
	descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet          = state.descriptorSet;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.pBufferInfo     = &descriptorBuffer;
	vkUpdateDescriptorSets(state.device, 1, &descriptorWrite, 0, nullptr);

	VkShaderModuleCreateInfo shaderInfo {};
	shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = shaderBinary.size() * sizeof(uint32_t);
	shaderInfo.pCode    = shaderBinary.data();
	result              = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.shaderModule);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateShaderModule(compute) failed", result);
	}
	VkPipelineShaderStageCreateInfo stageInfo {};
	stageInfo.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = state.shaderModule;
	stageInfo.pName  = "main";
	VkComputePipelineCreateInfo pipelineInfo {};
	pipelineInfo.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage  = stageInfo;
	pipelineInfo.layout = state.pipelineLayout;
	result              = vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.pipeline);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateComputePipelines failed", result);
	}

	VkCommandPoolCreateInfo commandPoolInfo {};
	commandPoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	commandPoolInfo.queueFamilyIndex = queueFamily;
	result                           = vkCreateCommandPool(state.device, &commandPoolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateCommandPool failed", result);
	}
	VkCommandBufferAllocateInfo commandBufferInfo {};
	commandBufferInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferInfo.commandPool        = state.commandPool;
	commandBufferInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferInfo.commandBufferCount = 1;
	result                               = vkAllocateCommandBuffers(state.device, &commandBufferInfo, &state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateCommandBuffers failed", result);
	}
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result          = vkCreateFence(state.device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateFence failed", result);
	}

	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result          = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
	{
		return Fail("vkBeginCommandBuffer failed", result);
	}
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout, 0, 1, &state.descriptorSet, 0,
	                        nullptr);
	vkCmdDispatch(state.commandBuffer, static_cast<uint32_t>(kInitialCases.size()), 1, 1);
	VkBufferMemoryBarrier shaderToHost {};
	shaderToHost.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	shaderToHost.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
	shaderToHost.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	shaderToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.buffer              = state.buffer;
	shaderToHost.size                = sizeof(kInitialCases);
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
	                     &shaderToHost, 0, nullptr);
	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkEndCommandBuffer failed", result);
	}

	VkSubmitInfo submitInfo {};
	submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers    = &state.commandBuffer;
	result                        = vkQueueSubmit(state.queue, 1, &submitInfo, state.fence);
	if (result != VK_SUCCESS)
	{
		return Fail("vkQueueSubmit failed", result);
	}
	result = vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, kCompletionTimeoutNanoseconds);
	if (result != VK_SUCCESS)
	{
		return Fail("vkWaitForFences failed", result);
	}
	result = vkInvalidateMappedMemoryRanges(state.device, 1, &mappedRange);
	if (result != VK_SUCCESS)
	{
		return Fail("vkInvalidateMappedMemoryRanges(storage) failed", result);
	}

	std::array<ProbeCase, kInitialCases.size()> observed {};
	std::memcpy(observed.data(), state.mapped, sizeof(observed));
	bool allCasesPassed = true;
	for (size_t index = 0; index < observed.size(); ++index)
	{
		if (observed[index].lhs != kInitialCases[index].lhs || observed[index].rhs != kInitialCases[index].rhs ||
		    observed[index].output != kExpectedOutputs[index])
		{
			std::fprintf(stderr,
			             "V_MUL_LEGACY_F32 backend mismatch: case=%s index=%zu lhs=%08x/%08x rhs=%08x/%08x "
			             "output=%08x/%08x\n",
			             kCaseNames[index], index, observed[index].lhs, kInitialCases[index].lhs, observed[index].rhs,
			             kInitialCases[index].rhs, observed[index].output, kExpectedOutputs[index]);
			allCasesPassed = false;
		}
	}
	if (!allCasesPassed)
	{
		return 1;
	}

	const int markerResult = std::printf("[GFX_VMUL_LEGACY_F32_COMPUTE] backend=native-metal scope=backend-arithmetic-only "
	                                     "classification=bitwise input-flush=explicit control-flow=branch-merge-phi fmul=nonzero-only "
	                                     "output-flush=signed underflow-boundary=integer-umul-extended cases=%zu status=clean\n",
	                                     kInitialCases.size());
	if (markerResult <= 0 || std::fflush(stdout) != 0)
	{
		return Fail("Failed to emit V_MUL_LEGACY_F32 backend marker");
	}
	return 0;
}
