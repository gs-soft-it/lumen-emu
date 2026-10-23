#include "Lumen/VRsqClampF32SpirvCore.h"
#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace
{
constexpr uint32_t kAmdVendorId                  = 0x1002u;
constexpr uint32_t kRequiredDeviceId             = 0x13c0u;
constexpr uint32_t kAppleVendorId                = 0x106bu;
constexpr char     kAppleM1DeviceName[]          = "Apple M1";
constexpr uint64_t kCompletionTimeoutNanoseconds = 30'000'000'000ull;
constexpr uint32_t kPrefixWords                  = 4;
constexpr uint32_t kWordsPerCase                 = 4;
constexpr uint32_t kOutputOffset                 = 1;
constexpr uint32_t kOutputUnset                  = 0xbaadf00du;
constexpr uint32_t kSignMask                     = 0x80000000u;
constexpr uint32_t kFractionMask                 = 0x007fffffu;
constexpr uint32_t kInfinityBits                 = 0x7f800000u;
constexpr uint32_t kMaximumFiniteBits            = 0x7f7fffffu;
constexpr uint32_t kCanonicalNanBits             = 0x7fc00000u;
constexpr size_t   kDispatchCount                = 3;
constexpr uint32_t kExhaustiveChunkCases         = 1u << 15u;
constexpr uint32_t kExhaustiveCaseCount          = 1u << 25u;
constexpr size_t   kExhaustivePassCount          = 2;
static_assert(kExhaustiveCaseCount % kExhaustiveChunkCases == 0);

enum class RuntimeMode
{
	Amd13c0,
	NativeMetalAppleM1,
};

struct ProbeCase
{
	std::string name;
	uint32_t    input;
	uint32_t    expected;
};

int FloorHalf(int value)
{
	return value >= 0 ? value / 2 : -((-value + 1) / 2);
}

int CompareProductToPowerOfTwo(uint32_t significand, uint32_t candidate, unsigned power)
{
#if defined(__SIZEOF_INT128__)
	const unsigned __int128 product = static_cast<unsigned __int128>(candidate) * candidate * significand;
	const unsigned __int128 target = static_cast<unsigned __int128>(1) << power;
	return product < target ? -1 : product > target ? 1 : 0;
#elif defined(_MSC_VER) && defined(_M_X64)
	const uint64_t square = static_cast<uint64_t>(candidate) * candidate;
	uint64_t productHigh = 0;
	const uint64_t productLow = _umul128(square, significand, &productHigh);
	const uint64_t targetHigh = uint64_t {1} << (power - 64u);
	if (productHigh != targetHigh) return productHigh < targetHigh ? -1 : 1;
	return productLow == 0 ? 0 : 1;
#else
#error "The V_RSQ_CLAMP_F32 independent oracle requires unsigned __int128 or MSVC x64 _umul128"
#endif
}

// Independent host oracle. The positive finite path represents x exactly as
// M*2^(e-23), then rounds sqrt(C/M) by comparing integer squares in 128 bits.
// It does not share control flow or arithmetic helpers with the SPIR-V core.
uint32_t ReferencePositiveFinite(uint32_t bits)
{
	uint32_t significand = bits & kFractionMask;
	const uint32_t exponentBits = (bits >> 23u) & 0xffu;
	int exponent;
	if (exponentBits == 0)
	{
		exponent = -126;
		while (significand < 0x00800000u)
		{
			significand <<= 1u;
			--exponent;
		}
	} else
	{
		significand |= 0x00800000u;
		exponent = static_cast<int>(exponentBits) - 127;
	}

	const int parity = exponent - 2 * FloorHalf(exponent);
	const unsigned targetPower = parity == 0 ? 71u : 70u;
	uint32_t low = 0x00800000u;
	uint32_t high = 0x01000000u;
	while (low < high)
	{
		const uint32_t candidate = low + (high - low + 1u) / 2u;
		if (CompareProductToPowerOfTwo(significand, candidate, targetPower) <= 0)
		{
			low = candidate;
		} else
		{
			high = candidate - 1u;
		}
	}

	uint32_t rounded = low;
	const uint32_t doubledPlusOne = low * 2u + 1u;
	const int midpointRelation = CompareProductToPowerOfTwo(significand, doubledPlusOne, targetPower + 2u);
	if (midpointRelation < 0 || (midpointRelation == 0 && (low & 1u) != 0))
	{
		++rounded;
	}

	int outputExponent = 126 - FloorHalf(exponent);
	if (rounded == 0x01000000u)
	{
		rounded = 0x00800000u;
		++outputExponent;
	}
	return static_cast<uint32_t>(outputExponent) << 23u | (rounded & kFractionMask);
}

uint32_t ReferenceRsqClampBits(uint32_t bits)
{
	const uint32_t exponent = (bits >> 23u) & 0xffu;
	const uint32_t fraction = bits & kFractionMask;
	if (exponent == 0xffu)
	{
		if (fraction != 0 || (bits & kSignMask) != 0)
		{
			return kCanonicalNanBits;
		}
		return 0;
	}
	if ((bits & ~kSignMask) == 0)
	{
		return (bits & kSignMask) != 0 ? (kSignMask | kMaximumFiniteBits) : kMaximumFiniteBits;
	}
	if ((bits & kSignMask) != 0)
	{
		return kCanonicalNanBits;
	}
	return ReferencePositiveFinite(bits);
}

void AddCase(std::vector<ProbeCase>* cases, std::string name, uint32_t input)
{
	cases->push_back({std::move(name), input, ReferenceRsqClampBits(input)});
}

std::vector<ProbeCase> MakeCases()
{
	std::vector<ProbeCase> cases;
	cases.reserve(1800);
	AddCase(&cases, "plus-zero", 0x00000000u);
	AddCase(&cases, "minus-zero", 0x80000000u);
	AddCase(&cases, "plus-infinity", 0x7f800000u);
	AddCase(&cases, "minus-infinity", 0xff800000u);
	AddCase(&cases, "one", 0x3f800000u);
	AddCase(&cases, "four", 0x40800000u);
	AddCase(&cases, "quarter", 0x3e800000u);
	AddCase(&cases, "minus-one", 0xbf800000u);

	constexpr std::array<uint32_t, 10> nanSamples = {
	    0x7f800001u, 0x7fbfffffu, 0x7fc00000u, 0x7fc12345u, 0x7fffffffu,
	    0xff800001u, 0xffbfffffu, 0xffc00000u, 0xffc12345u, 0xffffffffu,
	};
	for (size_t index = 0; index < nanSamples.size(); ++index)
	{
		AddCase(&cases, "nan-" + std::to_string(index), nanSamples[index]);
	}

	constexpr std::array<uint32_t, 7> fractionEdges = {
	    0u, 1u, 2u, 0x003fffffu, 0x00400000u, 0x007ffffeu, 0x007fffffu,
	};
	for (uint32_t exponent = 1; exponent <= 254; ++exponent)
	{
		for (size_t edge = 0; edge < fractionEdges.size(); ++edge)
		{
			AddCase(&cases, "normal-e" + std::to_string(exponent) + "-f" + std::to_string(edge),
			        exponent << 23u | fractionEdges[edge]);
		}
	}

	std::vector<uint32_t> subnormals = {1u, 2u, 3u, 0x003fffffu, 0x00400000u,
	                                    0x00400001u, 0x007ffffdu, 0x007ffffeu, 0x007fffffu};
	for (uint32_t bit = 0; bit < 23; ++bit)
	{
		const uint32_t power = 1u << bit;
		subnormals.push_back(power);
		if (power > 1)
		{
			subnormals.push_back(power - 1u);
		}
		if (power < kFractionMask)
		{
			subnormals.push_back(power + 1u);
		}
	}
	for (uint32_t bucket = 1; bucket <= 257; ++bucket)
	{
		subnormals.push_back(static_cast<uint32_t>((static_cast<uint64_t>(kFractionMask) * bucket) / 257u));
	}
	std::sort(subnormals.begin(), subnormals.end());
	subnormals.erase(std::remove(subnormals.begin(), subnormals.end(), 0u), subnormals.end());
	subnormals.erase(std::unique(subnormals.begin(), subnormals.end()), subnormals.end());
	for (size_t index = 0; index < subnormals.size(); ++index)
	{
		AddCase(&cases, "subnormal-pos-" + std::to_string(index), subnormals[index]);
		AddCase(&cases, "subnormal-neg-" + std::to_string(index), subnormals[index] | kSignMask);
	}
	return cases;
}

bool CheckAnchorLiterals()
{
	constexpr std::array<std::array<uint32_t, 2>, 8> anchors = {{
	    {{0x00000000u, 0x7f7fffffu}}, {{0x80000000u, 0xff7fffffu}},
	    {{0x7f800000u, 0x00000000u}}, {{0xff800000u, 0x7fc00000u}},
	    {{0x3f800000u, 0x3f800000u}}, {{0x40800000u, 0x3f000000u}},
	    {{0x3e800000u, 0x40000000u}}, {{0xbf800000u, 0x7fc00000u}},
	}};
	for (const auto& anchor: anchors)
	{
		if (ReferenceRsqClampBits(anchor[0]) != anchor[1])
		{
			std::fprintf(stderr, "V_RSQ_CLAMP_F32 host oracle anchor mismatch: input=%08x\n", anchor[0]);
			return false;
		}
	}
	return true;
}

int RunExhaustiveHostOracle()
{
	if (!CheckAnchorLiterals())
	{
		return 1;
	}
	uint32_t checksum = 0;
	for (uint32_t fraction = 1; fraction <= kFractionMask; ++fraction)
	{
		const uint32_t positive = ReferenceRsqClampBits(fraction);
		const uint32_t negative = ReferenceRsqClampBits(fraction | kSignMask);
		if ((positive & kSignMask) != 0 || (positive & kInfinityBits) == 0 ||
		    negative != kCanonicalNanBits)
		{
			std::fprintf(stderr, "V_RSQ_CLAMP_F32 exhaustive subnormal failure: fraction=%08x\n", fraction);
			return 1;
		}
		checksum ^= positive + fraction;
	}
	for (uint32_t sign: {0u, kSignMask})
	{
		for (uint32_t fraction = 1; fraction <= kFractionMask; ++fraction)
		{
			const uint32_t input = sign | kInfinityBits | fraction;
			if (ReferenceRsqClampBits(input) != kCanonicalNanBits)
			{
				std::fprintf(stderr, "V_RSQ_CLAMP_F32 exhaustive NaN failure: input=%08x\n", input);
				return 1;
			}
			checksum ^= input;
		}
	}
	std::printf("[GFX_VRSQ_CLAMP_F32_HOST_EXHAUSTIVE] subnormals=all-signs nans=all-signs-payloads "
	            "oracle=independent-uint128 checksum=%08x status=clean\n", checksum);
	return std::fflush(stdout) == 0 ? 0 : 1;
}

uint32_t CanaryForIndex(size_t index)
{
	return 0x5a17c3e9u ^ static_cast<uint32_t>(index * 0x01010101u);
}

std::vector<uint32_t> MakeInitialWords(const std::vector<ProbeCase>& cases)
{
	std::vector<uint32_t> words(kPrefixWords + cases.size() * kWordsPerCase + 4u);
	for (size_t index = 0; index < words.size(); ++index)
	{
		words[index] = CanaryForIndex(index);
	}
	for (size_t index = 0; index < cases.size(); ++index)
	{
		const size_t base = kPrefixWords + index * kWordsPerCase;
		words[base] = cases[index].input;
		words[base + kOutputOffset] = kOutputUnset;
	}
	return words;
}

uint32_t ExhaustiveInputForIndex(uint32_t index)
{
	if (index < 4u)
	{
		constexpr std::array<uint32_t, 4> specials = {
		    0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u,
		};
		return specials[index];
	}
	index -= 4u;
	const uint32_t group = index / kFractionMask;
	const uint32_t fraction = index % kFractionMask + 1u;
	switch (group)
	{
	case 0: return fraction;
	case 1: return kSignMask | fraction;
	case 2: return kInfinityBits | fraction;
	default: return kSignMask | kInfinityBits | fraction;
	}
}

void FillExhaustiveChunkWords(std::vector<uint32_t>* words, uint32_t firstCase)
{
	words->resize(kPrefixWords + kExhaustiveChunkCases * kWordsPerCase + 4u);
	for (size_t index = 0; index < words->size(); ++index)
	{
		(*words)[index] = CanaryForIndex(index) ^ firstCase;
	}
	for (uint32_t index = 0; index < kExhaustiveChunkCases; ++index)
	{
		const size_t base = kPrefixWords + static_cast<size_t>(index) * kWordsPerCase;
		(*words)[base] = ExhaustiveInputForIndex(firstCase + index);
		(*words)[base + kOutputOffset] = kOutputUnset;
	}
}

constexpr char kComputeShaderPreamble[] = R"spirv(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %global_id
               OpExecutionMode %main LocalSize 1 1 1
               OpName %lumen_vrsq_clamp_f32_bits "lumen_vrsq_clamp_f32_bits"
               OpName %result_bits "result_bits"
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
        %int = OpTypeInt 32 1
   %ResTypeU = OpTypeStruct %uint %uint
%function_u1 = OpTypeFunction %uint %uint
%function_u_u_u = OpTypeFunction %uint %uint %uint %uint
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
    %uint_16 = OpConstant %uint 16
    %uint_22 = OpConstant %uint 22
    %uint_23 = OpConstant %uint 23
    %uint_24 = OpConstant %uint 24
    %uint_25 = OpConstant %uint 25
    %uint_30 = OpConstant %uint 30
    %uint_31 = OpConstant %uint 31
    %uint_32 = OpConstant %uint 32
    %uint_64 = OpConstant %uint 64
   %uint_126 = OpConstant %uint 126
   %uint_127 = OpConstant %uint 127
   %uint_128 = OpConstant %uint 128
   %uint_254 = OpConstant %uint 254
   %uint_255 = OpConstant %uint 255
%uint_0x00000040 = OpConstant %uint 0x00000040
%uint_0x00000080 = OpConstant %uint 0x00000080
%uint_0x00000100 = OpConstant %uint 0x00000100
%uint_0x00000200 = OpConstant %uint 0x00000200
%uint_0x007fffff = OpConstant %uint 0x007fffff
%uint_0x00800000 = OpConstant %uint 0x00800000
%uint_0x01000000 = OpConstant %uint 0x01000000
%uint_0x7f7fffff = OpConstant %uint 0x7f7fffff
%uint_0x7f800000 = OpConstant %uint 0x7f800000
%uint_0x7fc00000 = OpConstant %uint 0x7fc00000
%uint_0x80000000 = OpConstant %uint 0x80000000
%uint_0xff7fffff = OpConstant %uint 0xff7fffff
      %int_0 = OpConstant %int 0
      %int_1 = OpConstant %int 1
    %int_126 = OpConstant %int 126
    %int_127 = OpConstant %int 127
   %int_m126 = OpConstant %int -126
)spirv";

constexpr char kComputeShaderMain[] = R"spirv(
       %main = OpFunction %void None %function
      %entry = OpLabel
         %gid = OpLoad %v3uint %global_id
  %case_index = OpCompositeExtract %uint %gid 0
 %case_offset = OpIMul %uint %case_index %uint_4
        %base = OpIAdd %uint %case_offset %uint_4
%output_index = OpIAdd %uint %base %uint_1
   %input_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %base
  %input_bits = OpLoad %uint %input_ptr
 %result_bits = OpFunctionCall %uint %lumen_vrsq_clamp_f32_bits %input_bits
  %output_ptr = OpAccessChain %ptr_uniform_uint %storage %uint_0 %output_index
               OpStore %output_ptr %result_bits
               OpReturn
               OpFunctionEnd
)spirv";

struct ShaderProgram
{
	std::vector<uint32_t> binary;
	std::string           disassembly;
};

size_t CountToken(std::string_view text, std::string_view token)
{
	size_t count = 0;
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
	const size_t call = disassembly.find("%result_bits = OpFunctionCall %uint %lumen_vrsq_clamp_f32_bits");
	const size_t store = disassembly.find("OpStore", call);
	return call != std::string_view::npos && store != std::string_view::npos && call < store &&
	       CountToken(disassembly, "%lumen_vrsq_clamp_f32_bits = OpFunction") == 1 &&
	       CountToken(disassembly, "OpFunctionCall %uint %lumen_vrsq_clamp_f32_bits") == 1 &&
	       CountToken(disassembly, "OpUMulExtended") >= 1 &&
	       CountToken(disassembly, "OpTypeRuntimeArray %uint") == 1 &&
	       CountToken(disassembly, "OpTypeFloat") == 0 && CountToken(disassembly, "OpConstant %float") == 0 &&
	       CountToken(disassembly, "OpFAdd") == 0 && CountToken(disassembly, "OpFSub") == 0 &&
	       CountToken(disassembly, "OpFMul") == 0 && CountToken(disassembly, "OpFDiv") == 0 &&
	       CountToken(disassembly, "OpExtInst") == 0 && CountToken(disassembly, "OpTypeInt 64") == 0 &&
	       CountToken(disassembly, "OpCapability Int64") == 0 && CountToken(disassembly, "FindUMsb") == 0 &&
	       CountToken(disassembly, "SPV_KHR_float_controls") == 0 &&
	       CountToken(disassembly, "RoundingModeRTE") == 0 &&
	       CountToken(disassembly, "DenormPreserve") == 0 &&
	       CountToken(disassembly, "DenormFlushToZero") == 0 && CountToken(disassembly, "NoContraction") == 0;
}

bool BuildAndAuditShader(ShaderProgram* program)
{
	std::string assembly = kComputeShaderPreamble;
	assembly += Lumen::Libs::Graphics::Detail::kVRsqClampF32SpirvCore;
	assembly += kComputeShaderMain;
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
	tools.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                         { std::fprintf(stderr, "SPIR-V line %zu column %zu: %s\n", position.line, position.column, message); });
	if (!tools.Assemble(assembly.data(), assembly.size(), &program->binary) || !tools.Validate(program->binary))
	{
		std::fprintf(stderr, "V_RSQ_CLAMP_F32 probe SPIR-V assembly or validation failed\n");
		return false;
	}
	if (!tools.Disassemble(program->binary, &program->disassembly) || !HasRequiredShaderShape(program->disassembly))
	{
		std::fprintf(stderr, "V_RSQ_CLAMP_F32 probe SPIR-V shape audit failed\n");
		return false;
	}
	return true;
}

struct ProbeState
{
	VkInstance instance = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	VkShaderModule shaderModule = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* mapped = nullptr;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	~ProbeState()
	{
		if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
		if (mapped != nullptr) vkUnmapMemory(device, memory);
		if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPool, nullptr);
		if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
		if (shaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, shaderModule, nullptr);
		if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		if (descriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(device, buffer, nullptr);
		if (memory != VK_NULL_HANDLE) vkFreeMemory(device, memory, nullptr);
		if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
		if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
	}
};

int Fail(const char* operation, VkResult result = VK_SUCCESS)
{
	if (result == VK_SUCCESS) std::fprintf(stderr, "%s\n", operation);
	else std::fprintf(stderr, "%s: VkResult=%d\n", operation, static_cast<int>(result));
	return 1;
}

uint32_t FindMemoryType(VkPhysicalDevice device, uint32_t typeBits, VkMemoryPropertyFlags required)
{
	VkPhysicalDeviceMemoryProperties properties {};
	vkGetPhysicalDeviceMemoryProperties(device, &properties);
	for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((typeBits & (1u << index)) != 0 &&
		    (properties.memoryTypes[index].propertyFlags & required) == required) return index;
	}
	return UINT32_MAX;
}

bool InputsAndCanariesUnchanged(const std::vector<uint32_t>& initial,
	                            const std::vector<uint32_t>& observed,
	                            size_t caseCount, size_t dispatch)
{
	for (size_t index = 0; index < observed.size(); ++index)
	{
		bool output = false;
		if (index >= kPrefixWords && index < kPrefixWords + caseCount * kWordsPerCase)
		{
			output = (index - kPrefixWords) % kWordsPerCase == kOutputOffset;
		}
		if (!output && observed[index] != initial[index])
		{
			std::fprintf(stderr, "V_RSQ_CLAMP_F32 modified protected word: dispatch=%zu word=%zu\n", dispatch, index);
			return false;
		}
	}
	return true;
}
} // namespace

int main(int argc, char** argv)
{
	bool shaderOnly = false;
	bool exhaustiveHost = false;
	bool exhaustiveNativeMetal = false;
	RuntimeMode runtimeMode = RuntimeMode::Amd13c0;
	if (argc == 2 && std::strcmp(argv[1], "--shader-only") == 0) shaderOnly = true;
	else if (argc == 2 && std::strcmp(argv[1], "--exhaustive-host") == 0) exhaustiveHost = true;
	else if (argc == 2 && std::strcmp(argv[1], "--native-metal-apple-m1") == 0)
	{
#if defined(LUMEN_VRSQ_CLAMP_F32_PROBE_NATIVE_METAL_APPLE_M1)
		runtimeMode = RuntimeMode::NativeMetalAppleM1;
#else
		return Fail("--native-metal-apple-m1 is unavailable in this build");
#endif
	}
	else if (argc == 2 && std::strcmp(argv[1], "--exhaustive-native-metal-apple-m1") == 0)
	{
#if defined(LUMEN_VRSQ_CLAMP_F32_PROBE_NATIVE_METAL_APPLE_M1)
		runtimeMode = RuntimeMode::NativeMetalAppleM1;
		exhaustiveNativeMetal = true;
#else
		return Fail("--exhaustive-native-metal-apple-m1 is unavailable in this build");
#endif
	}
	else if (argc != 1)
	{
		return Fail("usage: lumen_graphics_vrsq_clamp_f32_vulkan_probe "
		            "[--shader-only|--exhaustive-host|--native-metal-apple-m1|--exhaustive-native-metal-apple-m1]");
	}

	if (!CheckAnchorLiterals()) return 1;
	ShaderProgram shader;
	if (!BuildAndAuditShader(&shader)) return 1;
	if (exhaustiveHost) return RunExhaustiveHostOracle();
	const std::vector<ProbeCase> cases = MakeCases();
	if (shaderOnly)
	{
		std::printf("[GFX_VRSQ_CLAMP_F32_VULKAN_PROBE] mode=shader-only scope=diagnostic-production-core "
		            "core=shared-integer-binary32 spirv=assembled+validated+shape-audited helpers=audited "
		            "float-ops=none int64=none find-umsb=none float-controls=none "
		            "cases=%zu vulkan-created=no status=clean\n", cases.size());
		return std::fflush(stdout) == 0 ? 0 : 1;
	}

	ProbeState state;
	VkApplicationInfo appInfo {};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Lumen V_RSQ_CLAMP_F32 Vulkan diagnostic probe";
	appInfo.applicationVersion = 1;
	appInfo.pEngineName = "Lumen";
	appInfo.engineVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo instanceInfo {};
	instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instanceInfo.pApplicationInfo = &appInfo;
	VkResult result = vkCreateInstance(&instanceInfo, nullptr, &state.instance);
	if (result != VK_SUCCESS) return Fail("vkCreateInstance failed", result);

	uint32_t physicalDeviceCount = 0;
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, nullptr);
	if (result != VK_SUCCESS || physicalDeviceCount == 0) return Fail("No Vulkan physical device found", result);
	std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
	result = vkEnumeratePhysicalDevices(state.instance, &physicalDeviceCount, physicalDevices.data());
	if (result != VK_SUCCESS) return Fail("vkEnumeratePhysicalDevices(list) failed", result);
	physicalDevices.resize(physicalDeviceCount);

	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkPhysicalDeviceProperties selectedProperties {};
	const bool nativeMetal = runtimeMode == RuntimeMode::NativeMetalAppleM1;
	const char* reportedType = nullptr;
	if (nativeMetal)
	{
		if (physicalDevices.size() != 1) return Fail("Native Metal Apple M1 probe requires exactly one Vulkan device");
		physicalDevice = physicalDevices.front();
		vkGetPhysicalDeviceProperties(physicalDevice, &selectedProperties);
		if (selectedProperties.vendorID != kAppleVendorId ||
		    std::strcmp(selectedProperties.deviceName, kAppleM1DeviceName) != 0)
			return Fail("Native Metal probe device is not the unique Apple M1 adapter");
		if (selectedProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) reportedType = "integrated";
		else if (selectedProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) reportedType = "discrete";
		else return Fail("Native Metal Apple M1 device type is unsupported");
	}
	else
	{
		uint32_t exactCount = 0;
		for (VkPhysicalDevice candidate: physicalDevices)
		{
			VkPhysicalDeviceProperties properties {};
			vkGetPhysicalDeviceProperties(candidate, &properties);
			if (properties.vendorID == kAmdVendorId && properties.deviceID == kRequiredDeviceId)
			{
				++exactCount;
				physicalDevice = candidate;
				selectedProperties = properties;
			}
		}
		if (exactCount != 1) return Fail(exactCount == 0 ? "Required AMD device was not found" : "Required AMD device is ambiguous");
		if (selectedProperties.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
			return Fail("Required AMD device is not reported as an integrated GPU");
	}
	if (VK_API_VERSION_MAJOR(selectedProperties.apiVersion) < 1 ||
	    (VK_API_VERSION_MAJOR(selectedProperties.apiVersion) == 1 && VK_API_VERSION_MINOR(selectedProperties.apiVersion) < 2))
		return Fail("Selected device does not expose Vulkan 1.2");

	uint32_t familyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(familyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());
	uint32_t queueFamily = UINT32_MAX;
	for (uint32_t index = 0; index < familyCount; ++index)
	{
		if (families[index].queueCount != 0 && (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
		{
			queueFamily = index;
			break;
		}
	}
	if (queueFamily == UINT32_MAX) return Fail("Selected device has no compute queue");

	constexpr float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {};
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &queuePriority;
	VkDeviceCreateInfo deviceInfo {};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	result = vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &state.device);
	if (result != VK_SUCCESS) return Fail("vkCreateDevice failed", result);
	vkGetDeviceQueue(state.device, queueFamily, 0, &state.queue);
	if (state.queue == VK_NULL_HANDLE) return Fail("vkGetDeviceQueue returned null");

	std::vector<uint32_t> initial = exhaustiveNativeMetal ? std::vector<uint32_t> {} : MakeInitialWords(cases);
	if (exhaustiveNativeMetal) FillExhaustiveChunkWords(&initial, 0);
	const uint32_t runtimeCaseCount = exhaustiveNativeMetal ? kExhaustiveChunkCases : static_cast<uint32_t>(cases.size());
	const VkDeviceSize bufferSize = initial.size() * sizeof(uint32_t);
	VkBufferCreateInfo bufferInfo {};
	bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferInfo.size = bufferSize;
	bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	result = vkCreateBuffer(state.device, &bufferInfo, nullptr, &state.buffer);
	if (result != VK_SUCCESS) return Fail("vkCreateBuffer failed", result);
	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(state.device, state.buffer, &requirements);
	const uint32_t memoryType = FindMemoryType(physicalDevice, requirements.memoryTypeBits,
	                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memoryType == UINT32_MAX) return Fail("No coherent host-visible memory type");
	VkMemoryAllocateInfo allocationInfo {};
	allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocationInfo.allocationSize = requirements.size;
	allocationInfo.memoryTypeIndex = memoryType;
	result = vkAllocateMemory(state.device, &allocationInfo, nullptr, &state.memory);
	if (result != VK_SUCCESS) return Fail("vkAllocateMemory failed", result);
	result = vkBindBufferMemory(state.device, state.buffer, state.memory, 0);
	if (result != VK_SUCCESS) return Fail("vkBindBufferMemory failed", result);
	result = vkMapMemory(state.device, state.memory, 0, VK_WHOLE_SIZE, 0, &state.mapped);
	if (result != VK_SUCCESS || state.mapped == nullptr) return Fail("vkMapMemory failed", result);

	VkDescriptorSetLayoutBinding binding {};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo layoutInfo {};
	layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layoutInfo.bindingCount = 1;
	layoutInfo.pBindings = &binding;
	result = vkCreateDescriptorSetLayout(state.device, &layoutInfo, nullptr, &state.descriptorSetLayout);
	if (result != VK_SUCCESS) return Fail("vkCreateDescriptorSetLayout failed", result);
	VkPipelineLayoutCreateInfo pipelineLayoutInfo {};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &state.descriptorSetLayout;
	result = vkCreatePipelineLayout(state.device, &pipelineLayoutInfo, nullptr, &state.pipelineLayout);
	if (result != VK_SUCCESS) return Fail("vkCreatePipelineLayout failed", result);

	VkDescriptorPoolSize poolSize {};
	poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	poolSize.descriptorCount = 1;
	VkDescriptorPoolCreateInfo poolInfo {};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	result = vkCreateDescriptorPool(state.device, &poolInfo, nullptr, &state.descriptorPool);
	if (result != VK_SUCCESS) return Fail("vkCreateDescriptorPool failed", result);
	VkDescriptorSetAllocateInfo setInfo {};
	setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	setInfo.descriptorPool = state.descriptorPool;
	setInfo.descriptorSetCount = 1;
	setInfo.pSetLayouts = &state.descriptorSetLayout;
	result = vkAllocateDescriptorSets(state.device, &setInfo, &state.descriptorSet);
	if (result != VK_SUCCESS) return Fail("vkAllocateDescriptorSets failed", result);
	VkDescriptorBufferInfo descriptorBuffer {state.buffer, 0, bufferSize};
	VkWriteDescriptorSet descriptorWrite {};
	descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.dstSet = state.descriptorSet;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	descriptorWrite.pBufferInfo = &descriptorBuffer;
	vkUpdateDescriptorSets(state.device, 1, &descriptorWrite, 0, nullptr);

	VkShaderModuleCreateInfo shaderInfo {};
	shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shaderInfo.codeSize = shader.binary.size() * sizeof(uint32_t);
	shaderInfo.pCode = shader.binary.data();
	result = vkCreateShaderModule(state.device, &shaderInfo, nullptr, &state.shaderModule);
	if (result != VK_SUCCESS) return Fail("vkCreateShaderModule failed", result);
	VkPipelineShaderStageCreateInfo stageInfo {};
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageInfo.module = state.shaderModule;
	stageInfo.pName = "main";
	VkComputePipelineCreateInfo pipelineInfo {};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipelineInfo.stage = stageInfo;
	pipelineInfo.layout = state.pipelineLayout;
	result = vkCreateComputePipelines(state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &state.pipeline);
	if (result != VK_SUCCESS) return Fail("vkCreateComputePipelines failed", result);

	VkCommandPoolCreateInfo commandPoolInfo {};
	commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	commandPoolInfo.queueFamilyIndex = queueFamily;
	result = vkCreateCommandPool(state.device, &commandPoolInfo, nullptr, &state.commandPool);
	if (result != VK_SUCCESS) return Fail("vkCreateCommandPool failed", result);
	VkCommandBufferAllocateInfo commandBufferInfo {};
	commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferInfo.commandPool = state.commandPool;
	commandBufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferInfo.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(state.device, &commandBufferInfo, &state.commandBuffer);
	if (result != VK_SUCCESS) return Fail("vkAllocateCommandBuffers failed", result);
	VkFenceCreateInfo fenceInfo {};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(state.device, &fenceInfo, nullptr, &state.fence);
	if (result != VK_SUCCESS) return Fail("vkCreateFence failed", result);

	VkCommandBufferBeginInfo beginInfo {};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	result = vkBeginCommandBuffer(state.commandBuffer, &beginInfo);
	if (result != VK_SUCCESS) return Fail("vkBeginCommandBuffer failed", result);
	VkBufferMemoryBarrier hostToShader {};
	hostToShader.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	hostToShader.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	hostToShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	hostToShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostToShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostToShader.buffer = state.buffer;
	hostToShader.size = bufferSize;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     0, 0, nullptr, 1, &hostToShader, 0, nullptr);
	vkCmdBindPipeline(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipeline);
	vkCmdBindDescriptorSets(state.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, state.pipelineLayout,
	                        0, 1, &state.descriptorSet, 0, nullptr);
	vkCmdDispatch(state.commandBuffer, runtimeCaseCount, 1, 1);
	VkBufferMemoryBarrier shaderToHost {};
	shaderToHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	shaderToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	shaderToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	shaderToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	shaderToHost.buffer = state.buffer;
	shaderToHost.size = bufferSize;
	vkCmdPipelineBarrier(state.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	                     0, 0, nullptr, 1, &shaderToHost, 0, nullptr);
	result = vkEndCommandBuffer(state.commandBuffer);
	if (result != VK_SUCCESS) return Fail("vkEndCommandBuffer failed", result);

	VkMappedMemoryRange mappedRange {};
	mappedRange.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	mappedRange.memory = state.memory;
	mappedRange.size = VK_WHOLE_SIZE;
	size_t submission = 0;
	auto submitAndRead = [&](const std::vector<uint32_t>& inputWords, std::vector<uint32_t>* observed)
	{
		std::memcpy(state.mapped, inputWords.data(), static_cast<size_t>(bufferSize));
		result = vkFlushMappedMemoryRanges(state.device, 1, &mappedRange);
		if (result != VK_SUCCESS) return false;
		if (submission != 0)
		{
			result = vkResetFences(state.device, 1, &state.fence);
			if (result != VK_SUCCESS) return false;
		}
		VkSubmitInfo submitInfo {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &state.commandBuffer;
		result = vkQueueSubmit(state.queue, 1, &submitInfo, state.fence);
		if (result != VK_SUCCESS) return false;
		result = vkWaitForFences(state.device, 1, &state.fence, VK_TRUE, kCompletionTimeoutNanoseconds);
		if (result != VK_SUCCESS) return false;
		result = vkInvalidateMappedMemoryRanges(state.device, 1, &mappedRange);
		if (result != VK_SUCCESS) return false;
		observed->resize(inputWords.size());
		std::memcpy(observed->data(), state.mapped, static_cast<size_t>(bufferSize));
		++submission;
		return true;
	};

	if (exhaustiveNativeMetal)
	{
		std::array<uint32_t, kExhaustivePassCount> checksums {};
		std::vector<uint32_t> observed(initial.size());
		constexpr uint32_t chunkCount = kExhaustiveCaseCount / kExhaustiveChunkCases;
		for (size_t pass = 0; pass < kExhaustivePassCount; ++pass)
		{
			uint32_t checksum = 0x811c9dc5u;
			for (uint32_t chunk = 0; chunk < chunkCount; ++chunk)
			{
				const uint32_t firstCase = chunk * kExhaustiveChunkCases;
				FillExhaustiveChunkWords(&initial, firstCase);
				if (!submitAndRead(initial, &observed))
					return Fail("V_RSQ_CLAMP_F32 exhaustive GPU submission failed", result);
				if (!InputsAndCanariesUnchanged(initial, observed, kExhaustiveChunkCases, submission - 1u))
					return 1;
				for (uint32_t index = 0; index < kExhaustiveChunkCases; ++index)
				{
					const uint32_t input = ExhaustiveInputForIndex(firstCase + index);
					const uint32_t expected = ReferenceRsqClampBits(input);
					const uint32_t value = observed[kPrefixWords + static_cast<size_t>(index) * kWordsPerCase + kOutputOffset];
					if (value == kOutputUnset || value != expected)
					{
						std::fprintf(stderr,
						             "V_RSQ_CLAMP_F32 exhaustive mismatch: pass=%zu index=%u input=%08x output=%08x expected=%08x\n",
						             pass, firstCase + index, input, value, expected);
						return 1;
					}
					checksum = (checksum << 5u | checksum >> 27u) ^ value ^ input;
				}
			}
			checksums[pass] = checksum;
		}
		if (checksums[0] != checksums[1]) return Fail("V_RSQ_CLAMP_F32 exhaustive passes are nondeterministic");
		std::printf("[GFX_VRSQ_CLAMP_F32_VULKAN_EXHAUSTIVE] mode=exhaustive-runtime "
		            "scope=diagnostic-production-core core=shared-integer-binary32 backend=native-metal "
		            "adapter=static host=apple-m1 vendor=0x%04x reported-type=%s translation=black-box "
		            "classes=zeros+infinities+subnormals+nan subnormals=all-signs nans=all-signs-payloads "
		            "cases=%u chunk-cases=%u chunks=%u passes=%zu result=bit-exact expected=all "
		            "inputs=unchanged canaries=unchanged deterministic=yes checksum=%08x status=clean\n",
		            selectedProperties.vendorID, reportedType, kExhaustiveCaseCount, kExhaustiveChunkCases,
		            chunkCount, kExhaustivePassCount, checksums[0]);
		return std::fflush(stdout) == 0 ? 0 : 1;
	}

	std::vector<std::vector<uint32_t>> outputs(kDispatchCount, std::vector<uint32_t>(cases.size()));
	std::vector<uint32_t> observed(initial.size());
	for (size_t dispatch = 0; dispatch < kDispatchCount; ++dispatch)
	{
		if (!submitAndRead(initial, &observed)) return Fail("V_RSQ_CLAMP_F32 GPU submission failed", result);
		if (!InputsAndCanariesUnchanged(initial, observed, cases.size(), dispatch)) return 1;
		for (size_t index = 0; index < cases.size(); ++index)
		{
			const uint32_t value = observed[kPrefixWords + index * kWordsPerCase + kOutputOffset];
			if (value == kOutputUnset || value != cases[index].expected)
			{
				std::fprintf(stderr, "V_RSQ_CLAMP_F32 mismatch: dispatch=%zu case=%s input=%08x output=%08x expected=%08x\n",
				             dispatch, cases[index].name.c_str(), cases[index].input, value, cases[index].expected);
				return 1;
			}
			outputs[dispatch][index] = value;
		}
	}
	for (size_t dispatch = 1; dispatch < kDispatchCount; ++dispatch)
		if (outputs[dispatch] != outputs[0]) return Fail("V_RSQ_CLAMP_F32 probe is nondeterministic");

	if (nativeMetal)
	{
		std::printf("[GFX_VRSQ_CLAMP_F32_VULKAN_PROBE] mode=runtime scope=diagnostic-production-core "
		            "core=shared-integer-binary32 backend=native-metal adapter=static host=apple-m1 "
		            "vendor=0x%04x reported-type=%s translation=black-box float-ops=none int64=none "
		            "find-umsb=none float-controls=none result=bit-exact expected=all dispatches=%zu "
		            "inputs=unchanged canaries=unchanged deterministic=yes cases=%zu status=clean\n",
		            selectedProperties.vendorID, reportedType, kDispatchCount, cases.size());
	}
	else
	{
		std::printf("[GFX_VRSQ_CLAMP_F32_VULKAN_PROBE] mode=runtime scope=diagnostic-production-core "
		            "core=shared-integer-binary32 backend=vulkan vendor=0x%04x device=0x%04x "
		            "float-ops=none int64=none find-umsb=none float-controls=none result=bit-exact "
		            "expected=all dispatches=%zu inputs=unchanged canaries=unchanged deterministic=yes "
		            "cases=%zu status=clean\n", selectedProperties.vendorID, selectedProperties.deviceID,
		            kDispatchCount, cases.size());
	}
	return std::fflush(stdout) == 0 ? 0 : 1;
}
