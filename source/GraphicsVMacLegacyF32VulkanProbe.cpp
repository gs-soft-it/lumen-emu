#include "Lumen/VMacLegacyF32SpirvCore.h"
#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr uint32_t kAmdVendorId                   = 0x1002u;
constexpr uint32_t kRequiredDeviceId              = 0x13c0u;
constexpr uint32_t kAppleVendorId                 = 0x106bu;
constexpr char     kAppleM1DeviceName[]           = "Apple M1";
constexpr uint64_t kCompletionTimeoutNanoseconds  = 5'000'000'000ull;
constexpr uint32_t kPrefixWords                   = 4;
constexpr uint32_t kWordsPerCase                  = 7;
constexpr uint32_t kProductOutputOffset           = 3;
constexpr uint32_t kSumOutputOffset               = 4;
constexpr uint32_t kRecordCanary0Offset           = 5;
constexpr uint32_t kRecordCanary1Offset           = 6;
constexpr uint32_t kProductOutputUnset            = 0xbaadf00du;
constexpr uint32_t kSumOutputUnset                 = 0xc001d00du;
constexpr size_t   kDispatchCount                 = 3;
constexpr uint32_t kSignMask                      = 0x80000000u;
constexpr uint32_t kFractionMask                  = 0x007fffffu;
constexpr uint32_t kInfinityBits                  = 0x7f800000u;
constexpr uint32_t kCanonicalNanBits              = 0x7fc00000u;
constexpr uint32_t kMinimumNormalBits             = 0x00800000u;

enum class RuntimeMode
{
	Amd13c0,
	NativeMetalAppleM1,
};

struct ProbeCase
{
	const char* name;
	uint32_t    lhs;
	uint32_t    rhs;
	uint32_t    addend;
	uint32_t    expectedProduct;
	uint32_t    expectedSum;
};

constexpr uint32_t Exponent(uint32_t bits)
{
	return (bits >> 23u) & 0xffu;
}

constexpr uint32_t Fraction(uint32_t bits)
{
	return bits & kFractionMask;
}

constexpr uint64_t RoundRightToNearestEven(uint64_t value, unsigned distance)
{
	if (distance == 0)
	{
		return value;
	}
	if (distance >= 64)
	{
		return 0;
	}
	const uint64_t truncated = value >> distance;
	const uint64_t remainder = value & ((uint64_t {1} << distance) - 1u);
	const uint64_t midpoint  = uint64_t {1} << (distance - 1u);
	return truncated + (remainder > midpoint || (remainder == midpoint && (truncated & 1u) != 0) ? 1u : 0u);
}

constexpr int HighestSetBit(uint64_t value)
{
	int bit = -1;
	while (value != 0)
	{
		value >>= 1u;
		++bit;
	}
	return bit;
}

// Independent host oracle: magnitude * 2^scale is rounded to binary32 RNE,
// then every subnormal result is flushed to a signed zero.
constexpr uint32_t ReferenceRoundAndFlush(uint32_t sign, uint64_t magnitude, int scale)
{
	if (magnitude == 0)
	{
		return sign;
	}
	const int top              = HighestSetBit(magnitude);
	int       unbiasedExponent = top + scale;
	if (unbiasedExponent < -126)
	{
		const int subnormalScale = scale + 149;
		uint64_t  roundedUnits   = 0;
		if (subnormalScale >= 0)
		{
			if (subnormalScale >= 64 || magnitude > (UINT64_MAX >> static_cast<unsigned>(subnormalScale)))
			{
				return sign | kMinimumNormalBits;
			}
			roundedUnits = magnitude << static_cast<unsigned>(subnormalScale);
		} else
		{
			roundedUnits = RoundRightToNearestEven(magnitude, static_cast<unsigned>(-subnormalScale));
		}
		return sign | (roundedUnits >= kMinimumNormalBits ? kMinimumNormalBits : 0u);
	}

	const int significandShift = top - 23;
	uint64_t  significand      = significandShift > 0 ?
	    RoundRightToNearestEven(magnitude, static_cast<unsigned>(significandShift)) :
	    magnitude << static_cast<unsigned>(-significandShift);
	if (significand == 0x01000000u)
	{
		significand >>= 1u;
		++unbiasedExponent;
	}
	const int biasedExponent = unbiasedExponent + 127;
	if (biasedExponent >= 255)
	{
		return sign | kInfinityBits;
	}
	return sign | (static_cast<uint32_t>(biasedExponent) << 23u) |
	       (static_cast<uint32_t>(significand) & kFractionMask);
}

constexpr uint32_t ReferenceMulLegacyBits(uint32_t lhs, uint32_t rhs)
{
	const uint32_t lhsExponent = Exponent(lhs);
	const uint32_t rhsExponent = Exponent(rhs);
	const uint32_t lhsFraction = Fraction(lhs);
	const uint32_t rhsFraction = Fraction(rhs);
	if (lhsExponent == 0 || rhsExponent == 0)
	{
		return 0;
	}
	if ((lhsExponent == 255 && lhsFraction != 0) || (rhsExponent == 255 && rhsFraction != 0))
	{
		return kCanonicalNanBits;
	}
	const uint32_t sign = (lhs ^ rhs) & kSignMask;
	if (lhsExponent == 255 || rhsExponent == 255)
	{
		return sign | kInfinityBits;
	}
	const uint64_t lhsSignificand = 0x00800000u | lhsFraction;
	const uint64_t rhsSignificand = 0x00800000u | rhsFraction;
	return ReferenceRoundAndFlush(sign, lhsSignificand * rhsSignificand,
	                              static_cast<int>(lhsExponent + rhsExponent) - 300);
}

constexpr uint32_t ReferenceAddRneFlushBits(uint32_t lhs, uint32_t rhs)
{
	const uint32_t lhsExponent = Exponent(lhs);
	const uint32_t rhsExponent = Exponent(rhs);
	const uint32_t lhsFraction = Fraction(lhs);
	const uint32_t rhsFraction = Fraction(rhs);
	const uint32_t lhsSign     = lhs & kSignMask;
	const uint32_t rhsSign     = rhs & kSignMask;
	const uint32_t lhsValue    = lhsExponent == 0 ? lhsSign : lhs;
	const uint32_t rhsValue    = rhsExponent == 0 ? rhsSign : rhs;
	const bool lhsNan          = lhsExponent == 255 && lhsFraction != 0;
	const bool rhsNan          = rhsExponent == 255 && rhsFraction != 0;
	const bool lhsInf          = lhsExponent == 255 && lhsFraction == 0;
	const bool rhsInf          = rhsExponent == 255 && rhsFraction == 0;
	if (lhsNan || rhsNan || (lhsInf && rhsInf && lhsSign != rhsSign))
	{
		return kCanonicalNanBits;
	}
	if (lhsInf)
	{
		return lhsSign | kInfinityBits;
	}
	if (rhsInf)
	{
		return rhsSign | kInfinityBits;
	}
	if (lhsExponent == 0 && rhsExponent == 0)
	{
		return lhsSign & rhsSign;
	}
	if (lhsExponent == 0)
	{
		return rhsValue;
	}
	if (rhsExponent == 0)
	{
		return lhsValue;
	}

	const uint32_t lhsSignificand = 0x00800000u | lhsFraction;
	const uint32_t rhsSignificand = 0x00800000u | rhsFraction;
	const bool lhsIsLarger = lhsExponent > rhsExponent ||
	                         (lhsExponent == rhsExponent && lhsSignificand >= rhsSignificand);
	const uint32_t largeExponent    = lhsIsLarger ? lhsExponent : rhsExponent;
	const uint32_t smallExponent    = lhsIsLarger ? rhsExponent : lhsExponent;
	const uint32_t largeSignificand = lhsIsLarger ? lhsSignificand : rhsSignificand;
	const uint32_t smallSignificand = lhsIsLarger ? rhsSignificand : lhsSignificand;
	const uint32_t largeSign        = lhsIsLarger ? lhsSign : rhsSign;
	const uint32_t largeValue       = lhsIsLarger ? lhsValue : rhsValue;
	const uint32_t distance         = largeExponent - smallExponent;
	if (distance >= 26)
	{
		return largeValue;
	}
	const uint64_t largeMagnitude = static_cast<uint64_t>(largeSignificand) << 25u;
	const uint64_t smallMagnitude = static_cast<uint64_t>(smallSignificand) << (25u - distance);
	const uint64_t magnitude      = lhsSign == rhsSign ? largeMagnitude + smallMagnitude :
	                                                    largeMagnitude - smallMagnitude;
	if (magnitude == 0)
	{
		return 0;
	}
	return ReferenceRoundAndFlush(lhsSign == rhsSign ? lhsSign : largeSign, magnitude,
	                              static_cast<int>(largeExponent) - 175);
}

// Every row has literal expected product and sum bits. Inputs are uploaded at
// runtime, so the shader cannot constant-fold the matrix.
constexpr std::array<ProbeCase, 46> kCases = {{
	{"finite-exact", 0x3f800000u, 0x40000000u, 0x40400000u, 0x40000000u, 0x40a00000u},
	{"zero-times-normal", 0x00000000u, 0x40400000u, 0x80000000u, 0x00000000u, 0x00000000u},
	{"negative-zero-times-negative", 0x80000000u, 0xc0400000u, 0x80000000u, 0x00000000u, 0x00000000u},
	{"zero-precedes-nan", 0x00000000u, 0x7fa12345u, 0x3f800000u, 0x00000000u, 0x3f800000u},
	{"negative-zero-precedes-inf", 0x80000000u, 0xff800000u, 0x00000000u, 0x00000000u, 0x00000000u},
	{"subnormal-precedes-inf", 0x00000001u, 0x7f800000u, 0x80000000u, 0x00000000u, 0x00000000u},
	{"negative-subnormal-precedes-nan", 0x807fffffu, 0xffa00001u, 0x80000000u, 0x00000000u, 0x00000000u},
	{"lhs-snan-canonical", 0x7f800001u, 0x3f800000u, 0x3f800000u, 0x7fc00000u, 0x7fc00000u},
	{"rhs-qnan-canonical", 0x3f800000u, 0xffc12345u, 0x3f800000u, 0x7fc00000u, 0x7fc00000u},
	{"acc-snan-canonical", 0x3f800000u, 0x3f800000u, 0x7f812345u, 0x3f800000u, 0x7fc00000u},
	{"positive-inf-product", 0x7f800000u, 0x40000000u, 0x3f800000u, 0x7f800000u, 0x7f800000u},
	{"negative-inf-product", 0x7f800000u, 0xc0000000u, 0xbf800000u, 0xff800000u, 0xff800000u},
	{"opposite-infinities", 0x7f800000u, 0x3f800000u, 0xff800000u, 0x7f800000u, 0x7fc00000u},
	{"same-negative-infinities", 0xff800000u, 0x3f800000u, 0xff800000u, 0xff800000u, 0xff800000u},
	{"product-tie-to-even-upper", 0x3fc00000u, 0x3f800001u, 0x00000000u, 0x3fc00002u, 0x3fc00002u},
	{"product-tie-to-even-lower", 0x3fc00000u, 0x3f800003u, 0x00000000u, 0x3fc00004u, 0x3fc00004u},
	{"product-sticky-up", 0x3f800001u, 0x3f800001u, 0x00000000u, 0x3f800002u, 0x3f800002u},
	{"underflow-below-midpoint", 0x00fffffeu, 0x3f000000u, 0x00000000u, 0x00000000u, 0x00000000u},
	{"underflow-at-midpoint", 0x00ffffffu, 0x3f000000u, 0x00000000u, 0x00800000u, 0x00800000u},
	{"underflow-negative-midpoint", 0x80ffffffu, 0x3f000000u, 0x80000000u, 0x80800000u, 0x80800000u},
	{"underflow-above-midpoint", 0x00ffffffu, 0x3f000001u, 0x00000000u, 0x00800000u, 0x00800000u},
	{"overflow-positive", 0x7f7fffffu, 0x3f800001u, 0x00000000u, 0x7f800000u, 0x7f800000u},
	{"overflow-negative", 0xff7fffffu, 0x3f800001u, 0x00000000u, 0xff800000u, 0xff800000u},
	{"max-finite-exact", 0x7f7fffffu, 0x3f800000u, 0x00000000u, 0x7f7fffffu, 0x7f7fffffu},
	{"zero-plus-negative-normal", 0x00000000u, 0x3f800000u, 0xc0200000u, 0x00000000u, 0xc0200000u},
	{"opposite-zero-sum", 0x00000000u, 0x3f800000u, 0x80000000u, 0x00000000u, 0x00000000u},
	{"both-negative-zero-sum", 0x80fffffeu, 0x3f000000u, 0x80000001u, 0x80000000u, 0x80000000u},
	{"acc-positive-subnormal-flush", 0x3f800000u, 0x3f800000u, 0x00000001u, 0x3f800000u, 0x3f800000u},
	{"acc-negative-subnormal-flush", 0xbf800000u, 0x3f800000u, 0x807fffffu, 0xbf800000u, 0xbf800000u},
	{"add-exact-cancel", 0x3f800000u, 0x40000000u, 0xc0000000u, 0x40000000u, 0x00000000u},
	{"add-tie-even", 0x3f800000u, 0x3f800000u, 0x33800000u, 0x3f800000u, 0x3f800000u},
	{"add-tie-up", 0x3f800001u, 0x3f800000u, 0x33800000u, 0x3f800001u, 0x3f800002u},
	{"add-above-tie", 0x3f800000u, 0x3f800000u, 0x33800001u, 0x3f800000u, 0x3f800001u},
	{"add-d25-max", 0x3f800000u, 0x3f800000u, 0x337fffffu, 0x3f800000u, 0x3f800000u},
	{"add-d26-max", 0x3f800000u, 0x3f800000u, 0x32ffffffu, 0x3f800000u, 0x3f800000u},
	{"add-d26-negative", 0x3f800000u, 0x3f800000u, 0xb2ffffffu, 0x3f800000u, 0x3f800000u},
	{"subtract-near-one", 0x3f800000u, 0x3f800000u, 0xbf7fffffu, 0x3f800000u, 0x33800000u},
	{"subtract-near-two", 0x40000000u, 0x3f800000u, 0xbfffffffu, 0x40000000u, 0x34000000u},
	{"subtract-underflow-positive", 0x01000000u, 0x3f800000u, 0x80ffffffu, 0x01000000u, 0x00000000u},
	{"subtract-underflow-negative", 0x81000000u, 0x3f800000u, 0x00ffffffu, 0x81000000u, 0x80000000u},
	{"add-overflow-positive", 0x7f7fffffu, 0x3f800000u, 0x7f7fffffu, 0x7f7fffffu, 0x7f800000u},
	{"add-overflow-negative", 0xff7fffffu, 0x3f800000u, 0xff7fffffu, 0xff7fffffu, 0xff800000u},
	{"round-carry-add", 0x3fffffffu, 0x3f800000u, 0x33800000u, 0x3fffffffu, 0x40000000u},
	{"legacy-tiny-negative", 0x3f800001u, 0x3f7ffffeu, 0xbf800000u, 0x3f800000u, 0x00000000u},
	{"legacy-square-below", 0x3f7fffffu, 0x3f7fffffu, 0x3f800002u, 0x3f7ffffeu, 0x40000000u},
	{"fractional-chain", 0x3eaaaaabu, 0x3f4ccccdu, 0xbe4ccccdu, 0x3e888889u, 0x3d88888au},
}};

constexpr bool ExpectedLiteralsMatchReferenceOracle()
{
	for (const ProbeCase& test: kCases)
	{
		const uint32_t product = ReferenceMulLegacyBits(test.lhs, test.rhs);
		if (product != test.expectedProduct || ReferenceAddRneFlushBits(product, test.addend) != test.expectedSum)
		{
			return false;
		}
	}
	return true;
}

static_assert(ExpectedLiteralsMatchReferenceOracle(), "V_MAC_LEGACY_F32 literal matrix disagrees with the host oracle");
static_assert(kCases[14].expectedProduct == 0x3fc00002u && kCases[15].expectedProduct == 0x3fc00004u,
              "product RNE tie literals changed");
static_assert(kCases[18].expectedProduct == 0x00800000u && kCases[19].expectedProduct == 0x80800000u,
              "minimum-normal threshold literals changed");
static_assert(kCases[29].expectedSum == 0x00000000u && kCases[39].expectedSum == 0x80000000u,
              "cancellation or signed-underflow literals changed");
static_assert(kCases[40].expectedSum == 0x7f800000u && kCases[41].expectedSum == 0xff800000u,
              "addition overflow literals changed");

constexpr size_t kSuffixWords = 4;
constexpr size_t kWordCount   = kPrefixWords + kCases.size() * kWordsPerCase + kSuffixWords;
using ProbeWords              = std::array<uint32_t, kWordCount>;

uint32_t CanaryForIndex(size_t index)
{
	return 0x5a17c3e9u ^ static_cast<uint32_t>(index * 0x01010101u);
}

ProbeWords MakeInitialWords()
{
	ProbeWords words {};
	for (size_t index = 0; index < words.size(); ++index)
	{
		words[index] = CanaryForIndex(index);
	}
	for (size_t index = 0; index < kCases.size(); ++index)
	{
		const size_t base = kPrefixWords + index * kWordsPerCase;
		words[base + 0]    = kCases[index].lhs;
		words[base + 1]    = kCases[index].rhs;
		words[base + 2]    = kCases[index].addend;
		words[base + kProductOutputOffset] = kProductOutputUnset;
		words[base + kSumOutputOffset]     = kSumOutputUnset;
		words[base + kRecordCanary0Offset] = CanaryForIndex(base + kRecordCanary0Offset);
		words[base + kRecordCanary1Offset] = CanaryForIndex(base + kRecordCanary1Offset);
	}
	return words;
}

// Only the harness is local to the probe. The three semantic functions are
// appended byte-for-byte from VMacLegacyF32SpirvCore.h below.
constexpr char kComputeShaderPreamble[] = R"spirv(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %global_id
               OpExecutionMode %main LocalSize 1 1 1
               OpName %lumen_vmac_shr_jam_u32 "lumen_vmac_shr_jam_u32"
               OpName %lumen_vmac_mul_legacy_bits "lumen_vmac_mul_legacy_bits"
               OpName %lumen_vmac_add_rne_flush_bits "lumen_vmac_add_rne_flush_bits"
               OpName %product_bits "product_bits"
               OpName %sum_bits "sum_bits"
               OpDecorate %global_id BuiltIn GlobalInvocationId
               OpDecorate %words ArrayStride 4
               OpMemberDecorate %buffer_block 0 Offset 0
               OpDecorate %buffer_block BufferBlock
               OpDecorate %storage DescriptorSet 0
               OpDecorate %storage Binding 0

       %void = OpTypeVoid
   %function = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
   %ResTypeU = OpTypeStruct %uint %uint
%function_u_u = OpTypeFunction %uint %uint %uint
     %v3uint = OpTypeVector %uint 3
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
     %uint_4 = OpConstant %uint 4
     %uint_7 = OpConstant %uint 7
     %uint_8 = OpConstant %uint 8
     %uint_9 = OpConstant %uint 9
    %uint_16 = OpConstant %uint 16
    %uint_22 = OpConstant %uint 22
    %uint_23 = OpConstant %uint 23
    %uint_24 = OpConstant %uint 24
    %uint_25 = OpConstant %uint 25
    %uint_26 = OpConstant %uint 26
    %uint_28 = OpConstant %uint 28
    %uint_30 = OpConstant %uint 30
    %uint_31 = OpConstant %uint 31
    %uint_32 = OpConstant %uint 32
   %uint_127 = OpConstant %uint 127
   %uint_128 = OpConstant %uint 128
   %uint_255 = OpConstant %uint 255
%uint_0x00000100 = OpConstant %uint 0x00000100
%uint_0x00007fff = OpConstant %uint 0x00007fff
%uint_0x00008000 = OpConstant %uint 0x00008000
%uint_0x00010000 = OpConstant %uint 0x00010000
%uint_0x00100000 = OpConstant %uint 0x00100000
%uint_0x003fffff = OpConstant %uint 0x003fffff
%uint_0x00400000 = OpConstant %uint 0x00400000
%uint_0x007fffff = OpConstant %uint 0x007fffff
%uint_0x00800000 = OpConstant %uint 0x00800000
%uint_0x01000000 = OpConstant %uint 0x01000000
%uint_0x7f800000 = OpConstant %uint 0x7f800000
%uint_0x7fc00000 = OpConstant %uint 0x7fc00000
%uint_0xff800000 = OpConstant %uint 0xff800000
)spirv";

constexpr char kComputeShaderMain[] = R"spirv(
       %main = OpFunction %void None %function
      %entry = OpLabel
         %gid = OpLoad %v3uint %global_id
  %case_index = OpCompositeExtract %uint %gid 0
 %case_offset = OpIMul %uint %case_index %uint_7
        %base = OpIAdd %uint %case_offset %uint_4
   %rhs_index = OpIAdd %uint %base %uint_1
%addend_index = OpIAdd %uint %base %uint_2
%product_index = OpIAdd %uint %base %uint_3
    %sum_index = OpIAdd %uint %base %uint_4
     %lhs_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %base
     %rhs_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %rhs_index
  %addend_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %addend_index
    %lhs_bits = OpLoad %uint %lhs_ptr
    %rhs_bits = OpLoad %uint %rhs_ptr
 %addend_bits = OpLoad %uint %addend_ptr
%product_bits = OpFunctionCall %uint %lumen_vmac_mul_legacy_bits %lhs_bits %rhs_bits
    %sum_bits = OpFunctionCall %uint %lumen_vmac_add_rne_flush_bits %product_bits %addend_bits
 %product_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %product_index
     %sum_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %sum_index
               OpStore %product_ptr %product_bits
               OpStore %sum_ptr %sum_bits
               OpReturn
               OpFunctionEnd
)spirv";

struct ShaderProgram
{
	std::vector<uint32_t> binary;
	std::string           disassembly;
};

struct ProbeResult
{
	uint32_t product;
	uint32_t sum;
};

using ProbeOutputs = std::array<ProbeResult, kCases.size()>;

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
		std::fprintf(stderr, "%s: VkResult=%d\n", operation, static_cast<int>(result));
	}
	return 1;
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

bool HasRequiredShaderShape(std::string_view disassembly)
{
	const size_t multiply = disassembly.find(
	    "%product_bits = OpFunctionCall %uint %lumen_vmac_mul_legacy_bits");
	const size_t add = disassembly.find(
	    "%sum_bits = OpFunctionCall %uint %lumen_vmac_add_rne_flush_bits", multiply);
	const size_t productStore = disassembly.find("OpStore", add);
	const size_t sumStore     = disassembly.find("OpStore", productStore + 1);

	return multiply != std::string_view::npos && add != std::string_view::npos &&
	       productStore != std::string_view::npos && sumStore != std::string_view::npos &&
	       multiply < add && add < productStore && productStore < sumStore &&
	       CountToken(disassembly, "%lumen_vmac_shr_jam_u32 = OpFunction") == 1 &&
	       CountToken(disassembly, "%lumen_vmac_mul_legacy_bits = OpFunction") == 1 &&
	       CountToken(disassembly, "%lumen_vmac_add_rne_flush_bits = OpFunction") == 1 &&
	       CountToken(disassembly, "OpFunctionCall %uint %lumen_vmac_mul_legacy_bits") == 1 &&
	       CountToken(disassembly, "OpFunctionCall %uint %lumen_vmac_add_rne_flush_bits") == 1 &&
	       CountToken(disassembly, "OpUMulExtended") == 1 &&
	       CountToken(disassembly, "OpLoad %uint") == 3 && CountToken(disassembly, "OpStore") == 2 &&
	       CountToken(disassembly, "OpTypeRuntimeArray %uint") == 1 &&
	       CountToken(disassembly, "OpTypeFloat") == 0 && CountToken(disassembly, "OpConstant %float") == 0 &&
	       CountToken(disassembly, "OpFAdd") == 0 && CountToken(disassembly, "OpFSub") == 0 &&
	       CountToken(disassembly, "OpFMul") == 0 && CountToken(disassembly, "OpFDiv") == 0 &&
	       CountToken(disassembly, "OpFRem") == 0 && CountToken(disassembly, "OpFMod") == 0 &&
	       CountToken(disassembly, "OpFConvert") == 0 && CountToken(disassembly, "OpConvertFTo") == 0 &&
	       CountToken(disassembly, "OpConvertSToF") == 0 && CountToken(disassembly, "OpConvertUToF") == 0 &&
	       CountToken(disassembly, " Fma ") == 0 && CountToken(disassembly, "OpTypeInt 64") == 0 &&
	       CountToken(disassembly, "OpCapability Int64") == 0 && CountToken(disassembly, "FindUMsb") == 0 &&
	       CountToken(disassembly, "SPV_KHR_float_controls") == 0 &&
	       CountToken(disassembly, "RoundingModeRTE") == 0 &&
	       CountToken(disassembly, "SignedZeroInfNanPreserve") == 0 &&
	       CountToken(disassembly, "DenormPreserve") == 0 &&
	       CountToken(disassembly, "DenormFlushToZero") == 0 && CountToken(disassembly, "NoContraction") == 0;
}

bool BuildAndAuditShader(ShaderProgram* program)
{
	std::string shaderAssembly = kComputeShaderPreamble;
	shaderAssembly += Lumen::Libs::Graphics::Detail::kVMacLegacyF32SpirvShrJamU32;
	shaderAssembly += Lumen::Libs::Graphics::Detail::kVMacLegacyF32SpirvMulLegacyBits;
	shaderAssembly += Lumen::Libs::Graphics::Detail::kVMacLegacyF32SpirvAddRneFlushBits;
	shaderAssembly += kComputeShaderMain;

	// Vulkan 1.1 targets SPIR-V 1.3, where the established BufferBlock/Uniform
	// SSBO form remains valid. All binary32 semantics are implemented by u32 ops.
	spvtools::SpirvTools spirvTools(SPV_ENV_VULKAN_1_1);
	spirvTools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                              { std::fprintf(stderr, "SPIR-V line %zu column %zu: %s\n", position.line, position.column, message); });
	if (!spirvTools.Assemble(shaderAssembly.data(), shaderAssembly.size(), &program->binary))
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 backend probe SPIR-V assembly failed\n");
		return false;
	}
	if (!spirvTools.Validate(program->binary))
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 backend probe SPIR-V validation failed\n");
		return false;
	}
	if (!spirvTools.Disassemble(program->binary, &program->disassembly) ||
	    !HasRequiredShaderShape(program->disassembly))
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 backend probe SPIR-V shape audit failed\n");
		return false;
	}
	return true;
}

uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((typeBits & (1u << index)) != 0 &&
		    (properties.memoryTypes[index].propertyFlags & required) == required)
		{
			return index;
		}
	}
	return UINT32_MAX;
}

bool InputsAndCanariesAreUnchanged(const ProbeWords& initial, const ProbeWords& observed, size_t dispatch)
{
	for (size_t index = 0; index < observed.size(); ++index)
	{
		bool isOutput = false;
		if (index >= kPrefixWords && index < kPrefixWords + kCases.size() * kWordsPerCase)
		{
			const size_t recordOffset = (index - kPrefixWords) % kWordsPerCase;
			isOutput = recordOffset == kProductOutputOffset || recordOffset == kSumOutputOffset;
		}
		if (!isOutput && observed[index] != initial[index])
		{
			std::fprintf(stderr,
			             "V_MAC_LEGACY_F32 backend probe modified protected word: dispatch=%zu word=%zu "
			             "observed=%08x expected=%08x\n",
			             dispatch, index, observed[index], initial[index]);
			return false;
		}
	}
	return true;
}

bool CaptureOutputs(const ProbeWords& observed, size_t dispatch, ProbeOutputs* outputs)
{
	for (size_t index = 0; index < kCases.size(); ++index)
	{
		const size_t base = kPrefixWords + index * kWordsPerCase;
		const uint32_t product = observed[base + kProductOutputOffset];
		const uint32_t sum     = observed[base + kSumOutputOffset];
		if (product == kProductOutputUnset || sum == kSumOutputUnset)
		{
			std::fprintf(stderr,
			             "V_MAC_LEGACY_F32 backend probe left output unset: dispatch=%zu case=%s "
			             "product=%08x sum=%08x\n",
			             dispatch, kCases[index].name, product, sum);
			return false;
		}
		if (product != kCases[index].expectedProduct || sum != kCases[index].expectedSum)
		{
			std::fprintf(stderr,
			             "V_MAC_LEGACY_F32 bit-exact mismatch: dispatch=%zu case=%s "
			             "product=%08x/%08x sum=%08x/%08x\n",
			             dispatch, kCases[index].name, product, kCases[index].expectedProduct,
			             sum, kCases[index].expectedSum);
			return false;
		}
		(*outputs)[index] = {product, sum};
	}
	return true;
}
} // namespace

int main(int argc, char** argv)
{
	bool        shaderOnly = false;
	RuntimeMode runtimeMode = RuntimeMode::Amd13c0;
	if (argc == 2 && std::strcmp(argv[1], "--shader-only") == 0)
	{
		shaderOnly = true;
	} else if (argc == 2 && std::strcmp(argv[1], "--native-metal-apple-m1") == 0)
	{
#if defined(LUMEN_VMAC_LEGACY_F32_PROBE_NATIVE_METAL_APPLE_M1)
		runtimeMode = RuntimeMode::NativeMetalAppleM1;
#else
		return Fail("--native-metal-apple-m1 is unavailable in this build");
#endif
	} else if (argc != 1)
	{
		return Fail(
		    "usage: lumen_graphics_vmac_legacy_f32_vulkan_probe [--shader-only|--native-metal-apple-m1]");
	}

	ShaderProgram shader;
	if (!BuildAndAuditShader(&shader))
	{
		return 1;
	}
	if (shaderOnly)
	{
		const int markerResult = std::printf(
		    "[GFX_VMAC_LEGACY_F32_VULKAN_PROBE] mode=shader-only scope=production-core "
		    "core=shared-integer-binary32 spirv=assembled+validated+shape-audited helpers=once "
		    "float-ops=none int64=none find-umsb=none float-controls=none "
		    "cases=%zu vulkan-created=no status=clean\n",
		    kCases.size());
		if (markerResult <= 0 || std::fflush(stdout) != 0)
		{
			return Fail("Failed to emit V_MAC_LEGACY_F32 shader-only marker");
		}
		return 0;
	}

	ProbeState state;
	VkApplicationInfo applicationInfo {};
	applicationInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	applicationInfo.pApplicationName   = "Lumen V_MAC_LEGACY_F32 Vulkan backend probe";
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
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0)
	{
		return Fail("No Vulkan physical device found", result);
	}
	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS)
	{
		return Fail("vkEnumeratePhysicalDevices(list) failed", result);
	}
	physicalDevices.resize(physicalDeviceCount);

	VkPhysicalDevice           physicalDevice = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties selectedProperties {};
	const bool                 nativeMetalAppleM1 = runtimeMode == RuntimeMode::NativeMetalAppleM1;
	const char*                nativeMetalReportedType = nullptr;
	if (nativeMetalAppleM1)
	{
		if (physicalDevices.size() != 1)
		{
			return Fail("Native Metal Apple M1 probe requires exactly one Vulkan physical device");
		}
		physicalDevice = physicalDevices.front();
		vkGetPhysicalDeviceProperties(physicalDevice, &selectedProperties);
		if (selectedProperties.vendorID != kAppleVendorId ||
		    std::strcmp(selectedProperties.deviceName, kAppleM1DeviceName) != 0)
		{
			return Fail("Native Metal probe physical device is not the unique Apple M1 adapter");
		}
		if (selectedProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
		{
			nativeMetalReportedType = "integrated";
		} else if (selectedProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			nativeMetalReportedType = "discrete";
		} else
		{
			return Fail("Native Metal Apple M1 device type is neither integrated nor discrete");
		}
		if (VK_API_VERSION_MAJOR(selectedProperties.apiVersion) < 1 ||
		    (VK_API_VERSION_MAJOR(selectedProperties.apiVersion) == 1 &&
		     VK_API_VERSION_MINOR(selectedProperties.apiVersion) < 2))
		{
			return Fail("Native Metal Apple M1 device does not expose Vulkan 1.2");
		}
	} else
	{
		uint32_t exactDeviceCount = 0;
		for (VkPhysicalDevice candidate: physicalDevices)
		{
			VkPhysicalDeviceProperties properties {};
			vkGetPhysicalDeviceProperties(candidate, &properties);
			if (properties.vendorID != kAmdVendorId || properties.deviceID != kRequiredDeviceId)
			{
				continue;
			}
			++exactDeviceCount;
			physicalDevice     = candidate;
			selectedProperties = properties;
		}
		if (exactDeviceCount == 0)
		{
			return Fail("Required AMD Vulkan device vendor=0x1002 device=0x13c0 was not found");
		}
		if (exactDeviceCount != 1)
		{
			return Fail("Required AMD Vulkan device vendor=0x1002 device=0x13c0 is ambiguous");
		}
		if (selectedProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
		{
			return Fail("AMD device 0x13c0 is not reported as an integrated GPU");
		}
		if (VK_API_VERSION_MAJOR(selectedProperties.apiVersion) < 1 ||
		    (VK_API_VERSION_MAJOR(selectedProperties.apiVersion) == 1 &&
		     VK_API_VERSION_MINOR(selectedProperties.apiVersion) < 2))
		{
			return Fail("AMD device 0x13c0 does not expose Vulkan 1.2");
		}
	}

	uint32_t queueFamily = UINT32_MAX;
	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
	for (uint32_t family = 0; family < familyCount; ++family)
	{
		if (families[family].queueCount != 0 && (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
		{
			queueFamily = family;
			break;
		}
	}
	if (queueFamily == UINT32_MAX)
	{
		return Fail(nativeMetalAppleM1 ? "Native Metal Apple M1 device does not expose a compute queue" :
		                                "AMD device 0x13c0 does not expose a compute queue");
	}
	if (nativeMetalAppleM1)
	{
		std::printf("[GFX_VMAC_LEGACY_F32_VULKAN_DEVICE] name=\"%s\" api=%u.%u.%u "
		            "reported-type=%s vendor=0x%04x\n",
		            selectedProperties.deviceName, VK_API_VERSION_MAJOR(selectedProperties.apiVersion),
		            VK_API_VERSION_MINOR(selectedProperties.apiVersion), VK_API_VERSION_PATCH(selectedProperties.apiVersion),
		            nativeMetalReportedType, selectedProperties.vendorID);
	} else
	{
		std::printf("[GFX_VMAC_LEGACY_F32_VULKAN_DEVICE] name=\"%s\" api=%u.%u.%u driver=0x%08x "
		            "type=integrated vendor=0x%04x device=0x%04x\n",
		            selectedProperties.deviceName, VK_API_VERSION_MAJOR(selectedProperties.apiVersion),
		            VK_API_VERSION_MINOR(selectedProperties.apiVersion), VK_API_VERSION_PATCH(selectedProperties.apiVersion),
		            selectedProperties.driverVersion, selectedProperties.vendorID, selectedProperties.deviceID);
	}

	constexpr float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount       = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos    = &queueInfo;
	result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &state.device);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDevice failed", result);
	}
	vkGetDeviceQueue(state.device, queueFamily, 0, &state.queue);
	if (state.queue == VK_NULL_HANDLE)
	{
		return Fail("vkGetDeviceQueue returned a null compute queue");
	}

	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size        = sizeof(ProbeWords);
	bufferInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(state.device, &bufferInfo, nullptr, &state.buffer);
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
		return Fail("No coherent host-visible memory type for probe storage buffer");
	}
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize  = memoryRequirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(state.device, &allocationInfo, nullptr, &state.memory);
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
	VkMappedMemoryRange mappedRange {};
	mappedRange.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	mappedRange.memory = state.memory;
	mappedRange.size   = VK_WHOLE_SIZE;

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
	result = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
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
	result = vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateDescriptorPool(storage) failed", result);
	}
	VkDescriptorSetAllocateInfo descriptorSetInfo {};
	descriptorSetInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	descriptorSetInfo.descriptorPool     = state.descriptorPool;
	descriptorSetInfo.descriptorSetCount = 1;
	descriptorSetInfo.pSetLayouts        = &state.descriptorSetLayout;
	result = vkAllocateDescriptorSets(state.device, &descriptorSetInfo, &state.descriptorSet);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateDescriptorSets(storage) failed", result);
	}
	VkDescriptorBufferInfo descriptorBuffer {};
	descriptorBuffer.buffer = state.buffer;
	descriptorBuffer.range  = sizeof(ProbeWords);
	VkWriteDescriptorSet descriptorWrite {};
	descriptorWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet          = state.descriptorSet;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.pBufferInfo     = &descriptorBuffer;
	vkUpdateDescriptorSets(state.device, 1, &descriptorWrite, 0, nullptr);

	VkShaderModuleCreateInfo shaderInfo {};
	shaderInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = shader.binary.size() * sizeof(uint32_t);
	shaderInfo.pCode    = shader.binary.data();
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.shaderModule);
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
	result = vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.pipeline);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateComputePipelines failed", result);
	}

	VkCommandPoolCreateInfo commandPoolInfo {};
	commandPoolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	commandPoolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(state.device, &commandPoolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateCommandPool failed", result);
	}
	VkCommandBufferAllocateInfo commandBufferInfo {};
	commandBufferInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferInfo.commandPool        = state.commandPool;
	commandBufferInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferInfo.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(state.device, &commandBufferInfo, &state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkAllocateCommandBuffers failed", result);
	}
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(state.device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS)
	{
		return Fail("vkCreateFence failed", result);
	}

	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = 0;
	result = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS)
	{
		return Fail("vkBeginCommandBuffer failed", result);
	}
	VkBufferMemoryBarrier hostToShader {};
	hostToShader.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	hostToShader.srcAccessMask       = VK_ACCESS_HOST_WRITE_BIT;
	hostToShader.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
	hostToShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostToShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostToShader.buffer              = state.buffer;
	hostToShader.size                = sizeof(ProbeWords);
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
	                     nullptr, 1, &hostToShader, 0, nullptr);
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout, 0, 1,
	                        &state.descriptorSet, 0, nullptr);
	vkCmdDispatch(state.commandBuffer, static_cast<uint32_t>(kCases.size()), 1, 1);
	VkBufferMemoryBarrier shaderToHost {};
	shaderToHost.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	shaderToHost.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
	shaderToHost.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
	shaderToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.buffer              = state.buffer;
	shaderToHost.size                = sizeof(ProbeWords);
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
	                     nullptr, 1, &shaderToHost, 0, nullptr);
	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS)
	{
		return Fail("vkEndCommandBuffer failed", result);
	}

	std::array<ProbeOutputs, kDispatchCount> observations {};
	for (size_t dispatch = 0; dispatch < kDispatchCount; ++dispatch)
	{
		const ProbeWords initial = MakeInitialWords();
		std::memcpy(state.mapped, initial.data(), sizeof(initial));
		result = vkFlushMappedMemoryRanges(state.device, 1, &mappedRange);
		if (result != VK_SUCCESS)
		{
			return Fail("vkFlushMappedMemoryRanges(storage) failed", result);
		}
		if (dispatch != 0)
		{
			result = vkResetFences(state.device, 1, &state.fence);
			if (result != VK_SUCCESS)
			{
				return Fail("vkResetFences failed", result);
			}
		}
		VkSubmitInfo submitInfo {};
		submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers    = &state.commandBuffer;
		result = vkQueueSubmit(state.queue, 1, &submitInfo, state.fence);
		if (result != VK_SUCCESS)
		{
			return Fail("vkQueueSubmit failed", result);
		}
		result = vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, kCompletionTimeoutNanoseconds);
		if (result != VK_SUCCESS)
		{
			return Fail("vkWaitForFences failed (5 second timeout)", result);
		}
		result = vkInvalidateMappedMemoryRanges(state.device, 1, &mappedRange);
		if (result != VK_SUCCESS)
		{
			return Fail("vkInvalidateMappedMemoryRanges(storage) failed", result);
		}
		ProbeWords observed {};
		std::memcpy(observed.data(), state.mapped, sizeof(observed));
		if (!InputsAndCanariesAreUnchanged(initial, observed, dispatch) ||
		    !CaptureOutputs(observed, dispatch, &observations[dispatch]))
		{
			return 1;
		}
	}

	for (size_t dispatch = 1; dispatch < observations.size(); ++dispatch)
	{
		for (size_t index = 0; index < kCases.size(); ++index)
		{
			if (observations[dispatch][index].product != observations[0][index].product ||
			    observations[dispatch][index].sum != observations[0][index].sum)
			{
				std::fprintf(stderr,
				             "V_MAC_LEGACY_F32 backend probe is nondeterministic: dispatch=%zu case=%s "
				             "product=%08x/%08x sum=%08x/%08x\n",
				             dispatch, kCases[index].name, observations[dispatch][index].product,
				             observations[0][index].product, observations[dispatch][index].sum,
				             observations[0][index].sum);
				return 1;
			}
		}
	}

	for (size_t index = 0; index < kCases.size(); ++index)
	{
		std::printf("[GFX_VMAC_LEGACY_F32_VULKAN_CASE] index=%zu name=%s lhs=%08x rhs=%08x addend=%08x "
		            "product=%08x sum=%08x expected=verified\n",
		            index, kCases[index].name, kCases[index].lhs, kCases[index].rhs, kCases[index].addend,
		            observations[0][index].product, observations[0][index].sum);
	}
	const int markerResult = nativeMetalAppleM1 ?
	    std::printf(
	        "[GFX_VMAC_LEGACY_F32_VULKAN_PROBE] mode=runtime scope=production-core "
	        "core=shared-integer-binary32 "
	        "backend=native-metal adapter=static host=apple-m1 vendor=0x%04x "
	        "reported-type=%s translation=black-box float-ops=none int64=none find-umsb=none float-controls=none "
	        "product=bit-exact sum=bit-exact expected=all "
	        "dispatches=%zu inputs=unchanged canaries=unchanged "
	        "deterministic=yes cases=%zu status=clean\n",
	        selectedProperties.vendorID, nativeMetalReportedType, kDispatchCount, kCases.size()) :
	    std::printf(
	        "[GFX_VMAC_LEGACY_F32_VULKAN_PROBE] mode=runtime scope=production-core "
	        "core=shared-integer-binary32 backend=vulkan vendor=0x%04x device=0x%04x "
	        "float-ops=none int64=none find-umsb=none float-controls=none "
	        "product=bit-exact sum=bit-exact expected=all "
	        "dispatches=%zu inputs=unchanged canaries=unchanged "
	        "deterministic=yes cases=%zu status=clean\n",
	        selectedProperties.vendorID, selectedProperties.deviceID, kDispatchCount, kCases.size());
	if (markerResult <= 0 || std::fflush(stdout) != 0)
	{
		return Fail("Failed to emit V_MAC_LEGACY_F32 Vulkan backend marker");
	}
	return 0;
}
