#include "Lumen/Core/Threads.h"
#include "Lumen/Scripts/Scripts.h"
#include "Lumen/VCvtOffF32I4SpirvCore.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/GraphicsRun.h"
#include "Emulator/Graphics/DepthSurfaceImport.h"
#include "Emulator/Graphics/GraphicsRender.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/HandoffTexelReadbackCache.h"
#include "Emulator/Graphics/MemoryHandoffIntervals.h"
#include "Emulator/Graphics/Objects/DepthStencilBuffer.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Objects/IndexBuffer.h"
#include "Emulator/Graphics/Objects/Label.h"
#include "Emulator/Graphics/Objects/RenderTexture.h"
#include "Emulator/Graphics/Objects/StorageBuffer.h"
#include "Emulator/Graphics/Objects/StorageTexture.h"
#include "Emulator/Graphics/Objects/Texture.h"
#include "Emulator/Graphics/Objects/VideoOutBuffer.h"
#include "Emulator/Graphics/Objects/VertexBuffer.h"
#include "Emulator/Graphics/Shader.h"
#include "Emulator/Graphics/ShaderParse.h"
#include "Emulator/Graphics/ShaderSpirv.h"
#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/Utils.h"
#include "Emulator/Graphics/VideoOut.h"
#include "Emulator/Graphics/Window.h"
#include "Emulator/Log.h"

#if defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#endif

#include "spirv-tools/libspirv.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Lumen::Core { bool NativeHeapConcurrencyContractTest(); }
namespace Lumen::Libs::Graphics { bool CompiledProgramCacheContractTest(); }
namespace Lumen::Core { bool CoreRefCounterContractTest(); }
namespace Lumen::Libs::Graphics { bool BackingQueryCacheContractTest(); }
namespace Lumen::GuestCpu { bool NativeMemoryClaimsContractTest(); }

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
#include "Emulator/Loader/RuntimeLinker.h"
#include <windows.h>
#endif

#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef LUMEN_GRAPHICS_PIXEL_DISCARD_DEPTH_PROBE
int GraphicsPixelDiscardDepthVulkanProbe();
int GraphicsStencilClearVulkanProbe();
int GraphicsColorReadbackRowsVulkanProbe();
int GraphicsCompletionPassReuseVulkanProbe();
#endif
#ifdef LUMEN_GRAPHICS_UPLOAD_STAGING_PROBE
int GraphicsUploadStagingVulkanProbe();
#endif
#ifdef LUMEN_OWNED_PREPARATION_FEX_TEST
int GraphicsOwnedPreparationFexTest();
#endif

namespace {

using Lumen::Libs::Graphics::ShaderCode;
using Lumen::Libs::Graphics::ShaderBindResources;
using Lumen::Libs::Graphics::ShaderComputeInputInfo;
using Lumen::Libs::Graphics::ShaderDx10ClampMode;
using Lumen::Libs::Graphics::ShaderFp32Mode;
using Lumen::Libs::Graphics::ShaderInstruction;
using Lumen::Libs::Graphics::ShaderInstructionType;
using Lumen::Libs::Graphics::ShaderOperandType;
using Lumen::Libs::Graphics::ShaderPixelInputInfo;
using Lumen::Libs::Graphics::ShaderType;
using Lumen::Libs::Graphics::ShaderVertexInputInfo;
namespace ShaderInstructionFormat = Lumen::Libs::Graphics::ShaderInstructionFormat;

size_t CountSubstring(std::string_view text, std::string_view needle)
{
	size_t count = 0;
	for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string_view::npos; pos += needle.size())
	{
		++count;
	}
	return count;
}

bool CheckHandoffReadbackComposition()
{
	using namespace Lumen::Libs::Graphics;
	constexpr uint32_t width = 13, height = 9, pitch = 128;
	constexpr size_t guard = 17;
	struct Case { ImageReadbackLayout layout; VkFormat format; bool neo; };
	std::vector<Case> cases;
	for (const auto format: {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
	                         VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB})
		cases.push_back({ImageReadbackLayout::Linear, format, false});
	for (const bool neo: {false, true})
	{
		for (const auto format: {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
		                         VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB})
			cases.push_back({ImageReadbackLayout::Display2dThin, format, neo});
		cases.push_back({ImageReadbackLayout::Color2dThin, VK_FORMAT_R8G8B8A8_UNORM, neo});
	}
	for (const auto& test: cases)
	{
		const auto fail = [&](const char* stage) {
			std::fprintf(stderr, "handoff readback composition failed: layout=%u format=%u neo=%u stage=%s\n",
			    static_cast<unsigned>(test.layout), static_cast<unsigned>(test.format), test.neo ? 1u : 0u, stage);
			return false;
		};
		uint64_t row = 0, span = 0;
		if (!UtilTryGetImageReadbackSpan(test.format, width, height, pitch, &row, &span)) return fail("footprint");
		const uint64_t stride = uint64_t(pitch) * (row / width);
		uint64_t footprint = stride * height;
		if (test.layout != ImageReadbackLayout::Linear)
		{
			TileSizeAlign tile {};
			const bool supported = test.layout == ImageReadbackLayout::Display2dThin ?
			    TileTryGetDisplay2dThinTextureSize(10, 0, width, height, pitch, 1, 10, test.neo, false, &tile, nullptr, nullptr) :
			    TileTryGetColor2dThinTextureSize(10, 0, width, height, pitch, 1, 14, test.neo, false, &tile, nullptr, nullptr);
			if (!supported) return fail("tile-footprint");
			footprint = tile.size;
		}
		const size_t bytes = static_cast<size_t>(footprint + 23); // Include an unused guest tail.
		const std::vector<HandoffTexelReadbackCache::Rows> rows {{0, row, stride, height}};
		std::vector<uint8_t> native(bytes, 0xdf), mask_source(bytes, 0), mask(bytes, 0);
		for (uint32_t y = 0; y < height; ++y)
			for (uint64_t x = 0; x < row; ++x)
			{
				native[y * stride + x] = static_cast<uint8_t>((y * 37 + x * 13) & 127u);
				mask_source[y * stride + x] = 1;
			}
		const auto convert = [&](std::vector<uint8_t>* dst, const std::vector<uint8_t>& src) {
			return UtilCopyReadbackImage(dst->data(), dst->size(), src.data(), src.size(), test.format,
			    width, height, pitch, test.layout, test.neo);
		};
		if (!convert(&mask, mask_source)) return fail("texel-mask");
		std::vector<size_t> texels;
		for (size_t i = 0; i < bytes; ++i) if (mask[i] != 0) texels.push_back(i);
		if (texels.size() != row * height || texels.size() < 2) return fail("logical-texels");
		HandoffTexelReadbackCache cache;
		HandoffTexelReadbackCache::Key key {101, 1, 201, 301};
		std::vector<uint8_t> backing(bytes + 2 * guard, 0xa5);
		MemoryHandoffIntervals dirty;
		dirty.Mark(0, bytes);
		unsigned captures = 0;
		bool hit = false;
		// Compare the composed publication with a fresh uncached readback. The
		// retained payload never contains guest padding or a final guest image.
		const auto publish = [&](const MemoryHandoffIntervals& selected, unsigned failure = 0) {
			std::vector<uint8_t> staging(bytes, 0), linear(bytes, 0);
			for (const auto& range: selected.Ranges())
				std::memcpy(staging.data() + range.begin, backing.data() + guard + range.begin, range.end - range.begin);
			if (!cache.Merge(key, rows, &linear, [&](std::vector<uint8_t>* output) {
				++captures;
				if (failure == 1) return false;
				*output = native;
				if (failure == 2) output->pop_back();
				return true;
			}, &hit) || !convert(&staging, linear)) return false;
			for (const auto& range: selected.Ranges())
				std::memcpy(backing.data() + guard + range.begin, staging.data() + range.begin, range.end - range.begin);
			return true;
		};
		for (const auto index: {texels[0], texels[1]})
		{
			MemoryHandoffIntervals selected;
			selected.Mark(index, 1);
			if (!publish(selected) || captures != 1 || hit != (index == texels[1])) return fail("partial-cache-reuse");
			dirty.Subtract(index, 1);
		}
		// A changed CPU store and a same-value CPU store both already own their
		// published bytes. Neither may be selected by the later dirty publication.
		backing[guard + texels[0]] = 0xe2;
		const uint8_t same_value = backing[guard + texels[1]];
		backing[guard + texels[1]] = same_value;
		if (!dirty.Intersections(texels[0], 1).empty() || !dirty.Intersections(texels[1], 1).empty()) return fail("cpu-owned-mask");
		for (size_t i = 0; i < bytes; ++i) if (mask[i] == 0) backing[guard + i] = 0xb6;
		auto expected = backing;
		std::vector<uint8_t> uncached(backing.begin() + guard, backing.end() - guard);
		if (!convert(&uncached, native)) return fail("uncached-oracle");
		for (const auto& range: dirty.Ranges())
			std::memcpy(expected.data() + guard + range.begin, uncached.data() + range.begin, range.end - range.begin);
		if (!publish(dirty) || !hit || captures != 1 || backing != expected ||
		    backing[guard + texels[0]] != 0xe2 || backing[guard + texels[1]] != same_value) return fail("fresh-padding-and-cpu-stores");
		// A new GPU producer owns the whole plane. The same handles with a new
		// epoch, then an ABA lifetime, must capture again instead of reusing bytes.
		dirty.Mark(0, bytes);
		for (unsigned change = 0; change != 2; ++change)
		{
			if (change == 0) ++key.native_epoch; else ++key.lifetime;
			for (uint32_t y = 0; y < height; ++y)
				for (uint64_t x = 0; x < row; ++x) ++native[y * stride + x];
			uncached.assign(backing.begin() + guard, backing.end() - guard);
			if (!convert(&uncached, native)) return fail("new-producer-oracle");
			expected = backing;
			std::memcpy(expected.data() + guard, uncached.data(), bytes);
			if (!publish(dirty) || hit || captures != change + 2 || backing != expected) return fail("epoch-or-lifetime");
		}
		const auto before_failure = backing;
		++key.native_epoch;
		if (publish(dirty, 1) || backing != before_failure || publish(dirty, 2) || backing != before_failure)
			return fail("failed-or-short-no-publication");
		std::vector<uint8_t> rejected(bytes, 0xa5);
		const auto unchanged = rejected;
		if (UtilCopyReadbackImage(rejected.data(), bytes, native.data(), bytes, VK_FORMAT_UNDEFINED,
		    width, height, pitch, test.layout, test.neo) || rejected != unchanged ||
		    UtilCopyReadbackImage(rejected.data(), test.layout == ImageReadbackLayout::Linear ? span - 1 : footprint - 1,
		        native.data(), bytes, test.format, width, height, pitch, test.layout, test.neo) || rejected != unchanged)
			return fail("conversion-rejection-no-write");
	}
	std::fprintf(stderr, "[GPU_HANDOFF_READBACK_COMPOSITION_TEST] cases=%zu linear/display/color=pass base/neo=pass"
	    " fresh-padding=preserved cpu-owned/same-value=preserved epoch/ABA=recapture failure=no-publication canaries=pass\n", cases.size());
	return true;
}

bool HasDisallowedVRsqClampF32Instruction(std::string_view text)
{
	for (size_t begin = 0; begin < text.size();)
	{
		while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' || text[begin] == '\n')) ++begin;
		size_t end = begin;
		while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\r' && text[end] != '\n') ++end;
		const auto token = text.substr(begin, end - begin);
		if (token == "OpExtInst" || token == "OpConvertFToS" || token == "OpConvertFToU" || token == "OpConvertSToF" ||
		    token == "OpConvertUToF") return true;
		if (token.size() >= 3 && token.substr(0, 3) == "OpF" && token != "OpFunction" && token != "OpFunctionParameter" &&
		    token != "OpFunctionCall" && token != "OpFunctionEnd") return true;
		begin = end;
	}
	return false;
}

constexpr uint32_t MimgWord(uint32_t opcode, bool globally_coherent, bool unnormalized, uint32_t dmask)
{
	return (0x3cu << 26u) | (opcode << 18u) | (static_cast<uint32_t>(globally_coherent) << 13u) |
	       (static_cast<uint32_t>(unnormalized) << 12u) | (dmask << 8u);
}

constexpr uint32_t MimgOperands(uint32_t sampler, uint32_t resource, uint32_t data, uint32_t address)
{
	return (sampler << 21u) | (resource << 16u) | (data << 8u) | address;
}

bool CheckShaderColorExportPipeline()
{
	constexpr uint32_t rg_fixture[]             = {0xf8001c03u, 0x00000000u, 0xbf810000u};
	constexpr uint32_t rg_program[]             = {0xbf8c0000u, 0xf8001c03u, 0x00000100u, 0xbf810000u};
	constexpr uint32_t rg_no_vm_program[]       = {0xbf8c0000u, 0xf8000c03u, 0x00000100u, 0xbf810000u};
	constexpr uint32_t rg_vm_not_done_program[] = {0xbf8c0000u, 0xf8001403u, 0x00000100u, 0xbf810000u};
	constexpr uint32_t rgba_program[]           = {0xbf8c0000u, 0xf8001c0fu, 0x00000100u, 0xbf810000u};
	constexpr uint32_t rgba32_program[]         = {0xbf8c0000u, 0xf800180fu, 0x03020100u, 0xbf810000u};
	constexpr uint32_t rgba_with_off_program[]  = {
	    0xbf840003u, 0xf8001c0fu, 0x00000100u, 0xbf810000u, 0xbefe0480u, 0xf8001c00u, 0x00000000u, 0xbf810000u,
	};
	constexpr uint32_t rgba32_with_off_program[] = {
	    0xbf840003u, 0xf800180fu, 0x03020100u, 0xbf810000u, 0xbefe0480u, 0xf8001c00u, 0x00000000u, 0xbf810000u,
	};
	constexpr uint32_t off_only_program[] = {0xf8001c00u, 0x00000000u, 0xbf810000u};
	constexpr uint32_t mixed_program[]    = {0xf8001c0fu, 0x00000100u, 0xf800180fu, 0x03020100u, 0xbf810000u};

	ShaderCode rg_code;
	rg_code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(rg_fixture, &rg_code);
	if (rg_code.GetInstructions().Size() != 2)
	{
		std::fprintf(stderr, "RG export fixture parsed %u instructions instead of 2\n", rg_code.GetInstructions().Size());
		return false;
	}
	const auto& rg_export = rg_code.GetInstructions().At(0);
	if (rg_export.type != ShaderInstructionType::Exp || rg_export.format != ShaderInstructionFormat::Mrt0Vsrc0OffComprVmDone ||
	    rg_export.src_num != 1 || rg_export.src[0].register_id != 0 || !rg_export.export_valid_mask || !rg_export.export_done)
	{
		std::fprintf(stderr, "compressed MRT0 EN=3 fixture did not parse as a one-source RG export\n");
		return false;
	}
	if (!Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rg_code, 4u, 0x3u) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rg_code, 4u, 0xfu) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rg_code, 9u, 0xfu))
	{
		std::fprintf(stderr, "RG export does not match only mode4/mask3\n");
		return false;
	}
	constexpr uint32_t rg_control_variants[] = {0xf8000403u, 0xf8000c03u, 0xf8001403u};
	for (const auto export_word: rg_control_variants)
	{
		const uint32_t fixture[] = {export_word, 0x00000100u, 0xbf810000u};
		ShaderCode     code;
		code.SetType(ShaderType::Pixel);
		Lumen::Libs::Graphics::ShaderParse(fixture, &code);
		const auto& color_export = code.GetInstructions().At(0);
		const bool expected_vm   = (export_word & (1u << 12u)) != 0;
		const bool expected_done = (export_word & (1u << 11u)) != 0;
		if (color_export.format != ShaderInstructionFormat::Mrt0Vsrc0OffComprVmDone || color_export.src_num != 1 ||
		    color_export.export_valid_mask != expected_vm || color_export.export_done != expected_done)
		{
			std::fprintf(stderr, "compressed MRT0 EN=3 did not preserve VM/DONE independently of operand shape\n");
			return false;
		}
	}

	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 4;
	input.shader_mask           = 0x3u;
	ShaderCode rg_program_code;
	rg_program_code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(rg_program, &rg_program_code);
	const auto             rg_source = Lumen::Libs::Graphics::SpirvGenerateSource(rg_program_code, nullptr, &input, nullptr);
	const std::string_view rg_text(rg_source.c_str(), rg_source.Size());
	if (CountSubstring(rg_text, "UnpackHalf2x16") != 1 ||
	    rg_text.find("OpCompositeConstruct %v4float %t4_1 %t5_1 %float_0_000000 %float_1_000000") == std::string_view::npos ||
	    CountSubstring(rg_text, "OpLoad %float %v0") != 1 || rg_text.find("%v1") != std::string_view::npos ||
	    CountSubstring(rg_text, "OpKill") != 1 || rg_text.find("%exp_vm_exec_1 = OpLoad %uint %exec_lo") == std::string_view::npos ||
	    rg_text.find("%exp_vm_export_1 = OpLabel") > rg_text.find("%t1_1 = OpLoad %float %v0"))
	{
		std::fprintf(stderr, "RG export SPIR-V source did not guard the exact R,G,0,1 write with final VM/EXEC\n");
		return false;
	}
	for (const auto* program: {rg_no_vm_program, rg_vm_not_done_program})
	{
		ShaderCode unguarded;
		unguarded.SetType(ShaderType::Pixel);
		Lumen::Libs::Graphics::ShaderParse(program, &unguarded);
		const auto source = Lumen::Libs::Graphics::SpirvGenerateSource(unguarded, nullptr, &input, nullptr);
		const std::string_view text(source.c_str(), source.Size());
		if (text.find("exp_vm_") != std::string_view::npos || text.find("OpKill") != std::string_view::npos)
		{
			std::fprintf(stderr, "non-final or absent EXP valid mask was terminated prematurely\n");
			return false;
		}
	}

	const auto           rg_module = Lumen::Libs::Graphics::ShaderRecompilePS(rg_program_code, &input);
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	if (rg_module.Size() < 5 || rg_module.At(0) != 0x07230203u || !validator.Validate(rg_module.GetDataConst(), rg_module.Size()))
	{
		std::fprintf(stderr, "RG export SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}
	ShaderCode rgba_code;
	rgba_code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(rgba_program, &rgba_code);
	if (rgba_code.GetInstructions().Size() != 3)
	{
		std::fprintf(stderr, "RGBA export fixture parsed %u instructions instead of 3\n", rgba_code.GetInstructions().Size());
		return false;
	}
	const auto& rgba_export = rgba_code.GetInstructions().At(1);
	if (rgba_export.type != ShaderInstructionType::Exp || rgba_export.format != ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone ||
	    rgba_export.src_num != 2 || rgba_export.src[1].register_id != 1 || !rgba_export.export_valid_mask || !rgba_export.export_done)
	{
		std::fprintf(stderr, "legacy compressed MRT0 EN=F export changed shape\n");
		return false;
	}
	if (!Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba_code, 4u, 0xfu) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba_code, 4u, 0x3u) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba_code, 9u, 0xfu))
	{
		std::fprintf(stderr, "compressed RGBA export does not match only mode4/maskf\n");
		return false;
	}
	ShaderCode rgba_with_off_code;
	rgba_with_off_code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(rgba_with_off_program, &rgba_with_off_code);
	bool saw_off_export = false;
	for (const auto& inst: rgba_with_off_code.GetInstructions())
	{
		saw_off_export = saw_off_export || inst.format == ShaderInstructionFormat::Mrt0OffOffComprVmDone;
	}
	if (!saw_off_export || !Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba_with_off_code, 4u, 0xfu) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba_with_off_code, 4u, 0x3u) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba_with_off_code, 9u, 0xfu))
	{
		std::fprintf(stderr, "MRT0 off export incorrectly invalidates a matching branch export\n");
		return false;
	}
	ShaderCode off_only_code;
	off_only_code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(off_only_program, &off_only_code);
	ShaderCode empty_code;
	empty_code.SetType(ShaderType::Pixel);
	ShaderCode mixed_code;
	mixed_code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(mixed_program, &mixed_code);
	if (!Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(off_only_code, 4u, 0xfu) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(empty_code, 4u, 0xfu) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(mixed_code, 4u, 0xfu) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(mixed_code, 9u, 0xfu))
	{
		std::fprintf(stderr, "pixel color-export sequence fail-closed contract mismatch\n");
		return false;
	}
	const auto             rgba_source = Lumen::Libs::Graphics::SpirvGenerateSource(rgba_code, nullptr, &input, nullptr);
	const std::string_view rgba_text(rgba_source.c_str(), rgba_source.Size());
	if (CountSubstring(rgba_text, "UnpackHalf2x16") != 2 || CountSubstring(rgba_text, "OpLoad %float %v0") != 1 ||
	    CountSubstring(rgba_text, "OpLoad %float %v1") != 1 || CountSubstring(rgba_text, "OpKill") != 1 ||
	    rgba_text.find("%exp_vm_export_1 = OpLabel") > rgba_text.find("%t1_1 = OpLoad %float %v0"))
	{
		std::fprintf(stderr, "compressed RGBA export no longer guards and reads both packed sources\n");
		return false;
	}
	validation_error.clear();
	input.shader_mask      = 0xfu;
	const auto rgba_module = Lumen::Libs::Graphics::ShaderRecompilePS(rgba_code, &input);
	if (rgba_module.Size() < 5 || rgba_module.At(0) != 0x07230203u || !validator.Validate(rgba_module.GetDataConst(), rgba_module.Size()))
	{
		std::fprintf(stderr, "legacy RGBA export SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}

	ShaderCode rgba32_code;
	rgba32_code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(rgba32_program, &rgba32_code);
	const auto& rgba32_export = rgba32_code.GetInstructions().At(1);
	if (!rgba32_export.export_valid_mask || !rgba32_export.export_done)
	{
		std::fprintf(stderr, "uncompressed RGBA export lost final VM/DONE control bits\n");
		return false;
	}
	if (!Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba32_code, 9u, 0xfu) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba32_code, 4u, 0xfu) ||
	    Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba32_code, 9u, 0x3u))
	{
		std::fprintf(stderr, "uncompressed RGBA export does not match only mode9/maskf\n");
		return false;
	}
	ShaderPixelInputInfo mode9_standalone_input {};
	mode9_standalone_input.target_output_mode[0] = 9;
	mode9_standalone_input.shader_mask           = 0xfu;
	const auto rgba32_source = Lumen::Libs::Graphics::SpirvGenerateSource(rgba32_code, nullptr, &mode9_standalone_input, nullptr);
	const std::string_view rgba32_text(rgba32_source.c_str(), rgba32_source.Size());
	if (CountSubstring(rgba32_text, "OpKill") != 1 ||
	    rgba32_text.find("%exp_vm_export_1 = OpLabel") > rgba32_text.find("%t0_1 = OpLoad %float %v0") ||
	    CountSubstring(rgba32_text, "OpStore %outColor") != 1)
	{
		std::fprintf(stderr, "uncompressed RGBA export did not guard its sole color write with final VM/EXEC\n");
		return false;
	}
	validation_error.clear();
	const auto rgba32_module = Lumen::Libs::Graphics::ShaderRecompilePS(rgba32_code, &mode9_standalone_input);
	if (rgba32_module.Size() < 5 || rgba32_module.At(0) != 0x07230203u ||
	    !validator.Validate(rgba32_module.GetDataConst(), rgba32_module.Size()))
	{
		std::fprintf(stderr, "uncompressed RGBA final-VM SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}
	ShaderCode rgba32_with_off_code;
	rgba32_with_off_code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(rgba32_with_off_program, &rgba32_with_off_code);
	ShaderPixelInputInfo mode9_input {};
	mode9_input.target_output_mode[0] = 9;
	mode9_input.shader_mask           = 0xfu;
	mode9_input.ps_pixel_kill_enable  = true;
	mode9_input.ps_early_z            = true;
	if (!Lumen::Libs::Graphics::ShaderIsPixelColorExportCompatible(rgba32_with_off_code, 9u, 0xfu))
	{
		std::fprintf(stderr, "mode9 RGBA export with a kill branch failed compatibility\n");
		return false;
	}
	const auto rgba32_with_off_source =
	    Lumen::Libs::Graphics::SpirvGenerateSource(rgba32_with_off_code, nullptr, &mode9_input, nullptr);
	const std::string_view rgba32_with_off_text(rgba32_with_off_source.c_str(), rgba32_with_off_source.Size());
	if (rgba32_with_off_text.find("EarlyFragmentTests") != std::string_view::npos ||
	    CountSubstring(rgba32_with_off_text, "OpKill") != 2)
	{
		std::fprintf(stderr, "mode9 RGBA kill branch forced depth/stencil effects before discard\n");
		return false;
	}
	validation_error.clear();
	const auto rgba32_with_off_module = Lumen::Libs::Graphics::ShaderRecompilePS(rgba32_with_off_code, &mode9_input);
	if (rgba32_with_off_module.Size() < 5 || rgba32_with_off_module.At(0) != 0x07230203u ||
	    !validator.Validate(rgba32_with_off_module.GetDataConst(), rgba32_with_off_module.Size()))
	{
		std::fprintf(stderr, "mode9 RGBA kill-branch SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}

	return true;
}

bool CheckShaderPixelDepthOrderingPipeline()
{
	struct ExportFixture
	{
		const char* name;
		uint32_t    instruction;
		uint32_t    operands;
		uint32_t    output_mode;
		uint32_t    shader_mask;
	};
	constexpr ExportFixture fixtures[] = {
	    {"RG16", 0xf8000403u, 0x00000100u, 4u, 0x3u},
	    {"RGBA16", 0xf800040fu, 0x00000100u, 4u, 0xfu},
	    {"RGBA32", 0xf800000fu, 0x03020100u, 9u, 0xfu},
	};
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	for (const auto& fixture: fixtures)
	{
		// VM and DONE are independent: only their final combination introduces
		// a discard. KILL_ENABLE must also prevent forced early side effects
		// when the final export has no EXEC guard.
		for (const uint32_t export_flags: {0u, 0x800u, 0x1000u, 0x1800u})
		{
			const uint32_t program[] = {0xbf8c0000u, fixture.instruction | export_flags, fixture.operands, 0xbf810000u};
			ShaderCode code;
			code.SetType(ShaderType::Pixel);
			Lumen::Libs::Graphics::ShaderParse(program, &code);
			for (const bool early_z: {false, true})
			{
				for (const bool kill_enable: {false, true})
				{
					ShaderPixelInputInfo input {};
					input.target_output_mode[0] = fixture.output_mode;
					input.shader_mask           = fixture.shader_mask;
					input.ps_early_z            = early_z;
					input.ps_pixel_kill_enable  = kill_enable;
					const auto source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, &input, nullptr);
					const std::string_view text(source.c_str(), source.Size());
					const bool final_valid_mask = export_flags == 0x1800u;
					const bool expect_early     = early_z && !kill_enable && !final_valid_mask;
					if ((text.find("EarlyFragmentTests") != std::string_view::npos) != expect_early ||
					    CountSubstring(text, "OpKill") != static_cast<size_t>(final_valid_mask))
					{
						std::fprintf(stderr, "%s export depth ordering mismatch: flags=0x%x early=%u kill=%u\n",
						             fixture.name, export_flags, early_z, kill_enable);
						return false;
					}
					validation_error.clear();
					const auto module = Lumen::Libs::Graphics::ShaderRecompilePS(code, &input);
					if (module.Size() < 5 || !validator.Validate(module.GetDataConst(), module.Size()))
					{
						std::fprintf(stderr, "%s export depth ordering SPIR-V validation failed: %s\n",
						             fixture.name, validation_error.c_str());
						return false;
					}
				}
			}
		}
	}
	return true;
}

bool CheckUnnormalizedImageLoadMipParsing()
{
	// Integer image addressing remains integer when the MIMG UNRM bit is set.
	constexpr uint32_t fixture[] = {MimgWord(0x01u, false, true, 0xfu), MimgOperands(0, 1, 2, 6), 0xbf810000u};
	ShaderCode         code;
	code.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);
	if (code.GetInstructions().Size() != 2)
	{
		std::fprintf(stderr, "UNRM image_load_mip fixture parsed %u instructions instead of 2\n", code.GetInstructions().Size());
		return false;
	}
	const auto& load = code.GetInstructions().At(0);
	if (load.type != ShaderInstructionType::ImageLoadMip || load.format != ShaderInstructionFormat::Vdata4Vaddr4StDmaskF ||
	    !load.unnormalized_coordinates || load.globally_coherent || load.dst.register_id != 2 || load.dst.size != 4 || load.src_num != 2 ||
	    load.src[0].register_id != 6 || load.src[0].size != 4 || load.src[1].register_id != 4 || load.src[1].size != 8)
	{
		std::fprintf(stderr, "UNRM image_load_mip operand shape changed\n");
		return false;
	}
	return true;
}

bool CheckCoherentImageStoreParsing()
{
	// Exercise the integer-coordinate, globally coherent store shape.
	constexpr uint32_t fixture[] = {MimgWord(0x08u, true, true, 0xfu), MimgOperands(0, 0, 2, 0), 0xbf810000u};
	ShaderCode         code;
	code.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);
	if (code.GetInstructions().Size() != 2)
	{
		std::fprintf(stderr, "GLC image_store fixture parsed %u instructions instead of 2\n", code.GetInstructions().Size());
		return false;
	}
	const auto& store = code.GetInstructions().At(0);
	if (store.type != ShaderInstructionType::ImageStore || store.format != ShaderInstructionFormat::Vdata4Vaddr3StDmaskF ||
	    !store.globally_coherent || !store.unnormalized_coordinates || store.dst.register_id != 2 || store.dst.size != 4 ||
	    store.src_num != 2 || store.src[0].register_id != 0 || store.src[0].size != 3 || store.src[1].register_id != 0 ||
	    store.src[1].size != 8)
	{
		std::fprintf(stderr, "GLC image_store operand or coherence shape changed\n");
		return false;
	}
	return true;
}

bool CheckImageSampleLzRgba()
{
	constexpr uint32_t fixture[] = {
	    MimgWord(0x27u, false, false, 0xfu),
	    MimgOperands(2, 0, 0, 4), // image_sample_lz v0:3, v4:6, s0:7, s8:11
	    0xf800180fu,
	    0x03020100u, // export MRT0 v0:v3
	    0xbf810000u,
	};
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);
	if (code.GetInstructions().Size() != 3)
	{
		std::fprintf(stderr, "RGBA image_sample_lz fixture parsed %u instructions instead of 3\n", code.GetInstructions().Size());
		return false;
	}
	const auto& sample = code.GetInstructions().At(0);
	if (sample.type != ShaderInstructionType::ImageSampleLz ||
	    sample.format != ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF || sample.globally_coherent ||
	    sample.unnormalized_coordinates || sample.dst.register_id != 0 || sample.dst.size != 4 || sample.src_num != 3 ||
	    sample.src[0].register_id != 4 || sample.src[0].size != 3 || sample.src[1].register_id != 0 ||
	    sample.src[1].size != 8 || sample.src[2].register_id != 8 || sample.src[2].size != 4)
	{
		std::fprintf(stderr, "RGBA image_sample_lz operand shape changed\n");
		return false;
	}

	ShaderPixelInputInfo input {};
	input.target_output_mode[0]                  = 9;
	input.shader_mask                            = 0xfu;
	input.bind.textures2D.textures_num           = 1;
	input.bind.textures2D.textures2d_sampled_num = 1;
	input.bind.textures2D.desc[0].usage          = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].start_register = 0;
	input.bind.textures2D.desc[0].texture.fields[3] =
	    static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u;
	input.bind.samplers.samplers_num      = 1;
	input.bind.samplers.start_register[0] = 8;
	if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &input.bind))
	{
		std::fprintf(stderr, "ordinary 2D RGBA image_sample_lz profile was rejected\n");
		return false;
	}
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);
	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, &input, nullptr);
	const std::string_view text(source.c_str(), source.Size());
	if (text.find("OpImageSampleExplicitLod %v4float") == std::string_view::npos ||
	    text.find("Lod %float_0_000000") == std::string_view::npos)
	{
		std::fprintf(stderr, "RGBA image_sample_lz no longer uses explicit zero LOD\n");
		return false;
	}
	for (uint32_t channel = 0; channel < 4; ++channel)
	{
		const auto extract = "OpAccessChain %_ptr_Function_float %temp_v4float %uint_" + std::to_string(channel);
		if (CountSubstring(text, extract) != 1)
		{
			std::fprintf(stderr, "RGBA image_sample_lz channel %u was not written exactly once\n", channel);
			return false;
		}
	}

	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	const auto module = Lumen::Libs::Graphics::ShaderRecompilePS(code, &input);
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "RGBA image_sample_lz SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}
	return true;
}

bool CheckImageSampleSingleComponent(uint32_t mask, uint32_t component)
{
	using namespace Lumen::Libs::Graphics;
	struct Registers { uint32_t texture; uint32_t sampler; uint32_t coordinates; uint32_t destination; };
	constexpr Registers layouts[] = {{0, 8, 0, 4}, {4, 12, 5, 5}, {4, 12, 253, 255}};
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
		std::fprintf(stderr, "IMAGE_SAMPLE single-component SPIR-V %zu: %s\n", position.index, message);
	});
	ShaderCode baseline;
	ShaderPixelInputInfo baseline_input;
	for (const auto& registers: layouts)
	{
		const uint32_t export_sources = registers.destination * 0x01010101u;
		// Owned MIMG/EXP ISA fixture; the second layout aliases destination
		// with coordinate X, while the last exercises the VGPR range boundary.
		const uint32_t words[9] = {MimgWord(0x20u, false, false, mask),
		                          MimgOperands(registers.sampler / 4, registers.texture / 4,
		                                       registers.destination, registers.coordinates),
		                          0xf800180fu, export_sources, 0xbf810000u};
		ShaderCode code;
		code.SetType(ShaderType::Pixel);
		if (ShaderTryParseBounded(words, std::size(words), &code) != 5 || code.GetInstructions().Size() != 3) return false;
		const auto& sample = code.GetInstructions().At(0);
		if (!ShaderImageSampleSingleComponentInstructionSupported(sample) || sample.pc != 0 || sample.dst.size != 1 ||
		    sample.dst.register_id != registers.destination ||
		    sample.src[0].register_id != registers.coordinates || sample.src[1].register_id != registers.texture ||
		    sample.src[2].register_id != registers.sampler || code.GetInstructions().At(1).pc != 8) return false;
		const auto dump = code.DbgDump();
		const auto format_name = "Vdata1Vaddr3StSsDmask" + std::to_string(mask);
		const auto mask_name = "dmask:0x" + std::to_string(mask);
		if (!dump.ContainsStr(format_name.c_str()) || !dump.ContainsStr(mask_name.c_str())) return false;
		ShaderPixelInputInfo input {};
		input.target_output_mode[0] = 9;
		input.shader_mask = 0xf;
		input.bind.textures2D.textures_num = input.bind.textures2D.textures2d_sampled_num = 1;
		input.bind.textures2D.desc[0].usage = ShaderTextureUsage::ReadOnly;
		input.bind.textures2D.desc[0].start_register = static_cast<int>(registers.texture);
		input.bind.textures2D.desc[0].texture.fields[3] = ShaderTextureResource::TYPE_2D << 28u;
		input.bind.samplers.samplers_num = 1;
		input.bind.samplers.start_register[0] = static_cast<int>(registers.sampler);
		if (!ShaderTextureOperationsSupported(code, &input.bind)) return false;
		ShaderCalcBindingIndices(&input.bind);
		const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
		const std::string_view text(source.c_str(), source.Size());
		const auto coordinate_x = text.find("%t39_0 = OpLoad %float %v" + std::to_string(registers.coordinates));
		const auto coordinate_y = text.find("%t40_0 = OpLoad %float %v" + std::to_string(registers.coordinates + 1));
		const auto write = text.find("OpStore %v" + std::to_string(registers.destination) + " %t47_0");
		const bool explicit_zero = ShaderImplicitSampleLodZeroExperimentEnabled();
		if (coordinate_x == std::string_view::npos || coordinate_y == std::string_view::npos || write == std::string_view::npos ||
		    coordinate_x >= write || coordinate_y >= write ||
		    text.find("OpCompositeConstruct %v2float %t39_0 %t40_0") == std::string_view::npos ||
		    CountSubstring(text, "OpAccessChain %_ptr_Function_float %temp_v4float %uint_" + std::to_string(component)) != 1 ||
		    CountSubstring(text, "OpStore %v" + std::to_string(registers.destination) + " %t47_0") != 1 ||
		    CountSubstring(text, "OpImageSampleImplicitLod %v4float") != (explicit_zero ? 0u : 1u) ||
		    CountSubstring(text, "OpImageSampleExplicitLod %v4float") != (explicit_zero ? 1u : 0u) ||
		    text.find("OpImageSampleDref") != std::string_view::npos) return false;
		for (const uint32_t other_component: {0u, 1u, 2u, 3u})
			if (other_component != component &&
			    text.find("OpAccessChain %_ptr_Function_float %temp_v4float %uint_" + std::to_string(other_component)) !=
			    std::string_view::npos) return false;
		const auto module = ShaderRecompilePS(code, &input);
		if (module.Size() < 5 || !validator.Validate(module.GetDataConst(), module.Size())) return false;
		baseline = code;
		baseline_input = input;
	}
	for (int mutation = 0; mutation < 22; ++mutation)
	{
		auto code = baseline;
		auto& sample = code.GetInstructions()[0];
		switch (mutation)
		{
			case 0: sample.dst.size = 2; break;
			case 1: sample.dst.register_id = 256; break;
			case 2: sample.dst.clamp = true; break;
			case 3: sample.dst.multiplier = 2.0f; break;
			case 4: sample.src[0].size = 2; break;
			case 5: sample.src[0].register_id = 254; break;
			case 6: sample.src[0].absolute = true; break;
			case 7: sample.src[0].negate = true; break;
			case 8: sample.src[1].size = 4; break;
			case 9: sample.src[1].register_id = 100; break;
			case 10: sample.src[1].register_id = 5; break;
			case 11: sample.src[1].type = ShaderOperandType::Vgpr; break;
			case 12: sample.src[2].size = 3; break;
			case 13: sample.src[2].register_id = 13; break;
			case 14: sample.src[2].constant.u = 1; break;
			case 15: sample.src_num = 4; break;
			case 16: sample.src[3].type = ShaderOperandType::Sgpr; break;
			case 17: sample.dst2.type = ShaderOperandType::VccLo; break;
			case 18: sample.globally_coherent = true; break;
			case 19: sample.unnormalized_coordinates = true; break;
			case 20: sample.type = ShaderInstructionType::ImageSampleLz; break;
			case 21: sample.export_done = true; break;
		}
		if (ShaderImageSampleSingleComponentInstructionSupported(sample) || ShaderTextureOperationsSupported(code, &baseline_input.bind)) return false;
	}
	if (ShaderTextureOperationsSupported(baseline, nullptr)) return false;
	for (const auto stage: {ShaderType::Unknown, ShaderType::Vertex, ShaderType::Compute})
	{
		auto code = baseline;
		code.SetType(stage);
		if (ShaderTextureOperationsSupported(code, &baseline_input.bind)) return false;
	}
	for (int mutation = 0; mutation < 6; ++mutation)
	{
		auto bind = baseline_input.bind;
		switch (mutation)
		{
			case 0: bind.samplers.samplers_num = 0; break;
			case 1: bind.textures2D.textures2d_sampled_num = 0; break;
			case 2: bind.textures2D.desc[0].texture.fields[3] = 11u << 28u; break;
			case 3: bind.textures2D.desc[0].texture.fields[3] = ShaderTextureResource::TYPE_8_COMPAT << 28u; break;
			case 4: bind.textures2D.textures_num = ShaderTextureResources::RES_MAX + 1; break;
			case 5: bind.samplers.samplers_num = ShaderSamplerResources::RES_MAX + 1; break;
		}
		if (ShaderTextureOperationsSupported(baseline, &bind)) return false;
	}
	std::fprintf(stderr, "[GFX_IMAGE_SAMPLE_DMASK%u_TEST] component=%u packed-destinations=1 pixel-modules=3 "
	                     "coordinate-alias=loaded-before-store invalid-ir=22 gates=unchanged gpu=not-tested status=clean\n",
	                     mask, component);
	return true;
}

bool CheckOrdinarySamplerComparisonLowering()
{
	constexpr uint32_t fixture[] = {
	    MimgWord(0x20u, false, false, 0xfu), MimgOperands(2, 0, 0, 4),
	    MimgWord(0x27u, false, false, 0xfu), MimgOperands(2, 0, 0, 4),
	    MimgWord(0x37u, false, false, 0x7u), MimgOperands(2, 0, 0, 4),
	    0xf800180fu, 0x03020100u, 0xbf810000u,
	};
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);
	if (code.GetInstructions().Size() != 5 || code.GetInstructions().At(0).type != ShaderInstructionType::ImageSample ||
	    code.GetInstructions().At(1).type != ShaderInstructionType::ImageSampleLz ||
	    code.GetInstructions().At(2).type != ShaderInstructionType::ImageSampleLzO) return false;
	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 9;
	input.shader_mask = 0xfu;
	input.bind.textures2D.textures_num = 1;
	input.bind.textures2D.textures2d_sampled_num = 1;
	input.bind.textures2D.desc[0].usage = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].texture.fields[3] =
	    static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u;
	input.bind.samplers.samplers_num = 1;
	input.bind.samplers.start_register[0] = 8;
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);
	std::string ordinary_source;
	for (uint32_t index = 0; index < 24; ++index)
	{
		const uint32_t function = index & 7u, z_filter = index >> 3u;
		input.bind.samplers.samplers[0].fields[0] = function << 12u;
		input.bind.samplers.samplers[0].fields[2] = z_filter << 24u;
		if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &input.bind)) return false;
		const auto source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, &input, nullptr);
		const std::string_view text(source.c_str(), source.Size());
		if (text.find("OpImageSampleDref") != std::string_view::npos || text.find("OpImageDrefGather") != std::string_view::npos ||
		    text.find("OpImageSampleExplicitLod %v4float") == std::string_view::npos ||
		    text.find("%ImageS = OpTypeImage %float 2D 0 0 0 1 Unknown") == std::string_view::npos) return false;
		if (index == 0) ordinary_source.assign(text);
		else if (text != ordinary_source) return false;
	}
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	const auto module = Lumen::Libs::Graphics::ShaderRecompilePS(code, &input);
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size())) return false;
	std::fprintf(stderr, "[GFX_ORDINARY_SAMPLER_LOWERING_TEST] functions=8 z_filter=0/1/2 image=2d-nonarray"
	                     " sample=ordinary lz=ordinary lz_offset=ordinary dref=absent spirv=valid\n");
	return true;
}

bool CheckVertexParameterExports()
{
	using namespace Lumen::Libs::Graphics;
	auto fail = [](const char* phase, uint32_t index = UINT32_MAX)
	{
		std::fprintf(stderr, "[GFX_PARAMETER_EXPORT_TEST_FAIL] phase=%s index=%u\n", phase, index);
		return false;
	};
	// Templates contain commented example declarations; count only assembly lines.
	auto count_assembly_line = [](std::string_view text, std::string_view needle, bool prefix = false)
	{
		size_t count = 0;
		for (size_t begin = 0; begin < text.size();)
		{
			const auto end = text.find('\n', begin);
			auto line = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
			const auto first = line.find_first_not_of(" \t\r");
			if (first != std::string_view::npos)
			{
				line.remove_prefix(first);
				const auto comment = line.find(';');
				if (comment != std::string_view::npos) line = line.substr(0, comment);
				const auto last = line.find_last_not_of(" \t\r");
				line = last == std::string_view::npos ? std::string_view{} : line.substr(0, last + 1);
				if (prefix ? line.substr(0, needle.size()) == needle : line == needle) ++count;
			}
			if (end == std::string_view::npos) break;
			begin = end + 1;
		}
		return count;
	};
	std::vector<uint32_t> fixture;
	for (uint32_t parameter = 0; parameter < 32; ++parameter)
	{
		fixture.push_back(0xf800000fu | ((0x20u + parameter) << 4u));
		fixture.push_back(0x03020100u);
	}
	fixture.push_back(0xf80008cfu); fixture.push_back(0x03020100u); // POS0, DONE
	fixture.push_back(0xbf810000u);
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	ShaderParse(fixture.data(), &code);
	if (code.GetInstructions().Size() != 34) return fail("parse-count");
	ShaderVertexInputInfo vertex;
	vertex.export_count = 32;
	uint32_t produced = 0;
	if (!ShaderVertexParameterExportsSupported(code, vertex, nullptr, nullptr, &produced) || produced != UINT32_MAX)
		return fail("producer-proof");
	vertex.parameter_export_mask = produced;
	vertex.parameter_exports_valid = true;
	// Synthetic limits qualify the CPU module fixture, not a real device draw.
	vertex.host_vertex_output_components = 135;
	vertex.host_parameter_interface_valid = true;
	for (int index = 0; index < 32; ++index)
	{
		const auto& inst = code.GetInstructions().At(index);
		if (inst.pc != static_cast<uint32_t>(index * 8) || inst.export_target != 0x20 + index ||
		    ShaderGetParameterExportIndex(inst) != index || inst.export_done || inst.export_valid_mask || inst.src_num != 4 ||
		    (index >= 5 && inst.format != ShaderInstructionFormat::ParamVsrc0Vsrc1Vsrc2Vsrc3))
			return fail("decoded-parameter", static_cast<uint32_t>(index));
	}
	const auto source = SpirvGenerateSource(code, &vertex, nullptr, nullptr);
	const std::string_view text(source.c_str(), source.Size());
	for (uint32_t parameter = 0; parameter < 32; ++parameter)
	{
		const auto name = "%param" + std::to_string(parameter);
		const auto decorations = count_assembly_line(text, "OpDecorate " + name + " Location " + std::to_string(parameter));
		const auto declarations = count_assembly_line(text, name + " = OpVariable %_ptr_Output_v4float Output");
		const auto stores = count_assembly_line(text, "OpStore " + name + " ", true);
		if (decorations != 1 || declarations != 1 || stores != 1)
		{
			std::fprintf(stderr, "[GFX_PARAMETER_EXPORT_TEST_FAIL] phase=source-lines index=%u decorations=%zu declarations=%zu stores=%zu\n",
			             parameter, decorations, declarations, stores);
			return false;
		}
	}
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = (std::to_string(position.index) + ": " + message).substr(0, 1024); });
	const auto module = ShaderRecompileVS(code, &vertex);
	if (module.Size() < 5 || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "[GFX_PARAMETER_EXPORT_TEST_FAIL] phase=vertex-module words=%u error=%.1024s\n",
		             module.Size(), validation_error.c_str());
		return false;
	}
	for (uint32_t mutation = 0; mutation < 12; ++mutation)
	{
		auto bad = code;
		auto input = vertex;
		auto& generic = bad.GetInstructions()[31];
		if (mutation == 0) input.export_count = 31;
		if (mutation == 1) input.export_count = 33;
		if (mutation == 2) bad.SetType(ShaderType::Pixel);
		if (mutation == 3) generic.export_target = 0xff;
		if (mutation == 4) generic.export_target = 0x24;
		if (mutation == 5) generic.export_target = 0x40;
		if (mutation == 6) generic.export_done = true;
		if (mutation == 7) generic.export_valid_mask = true;
		if (mutation == 8) generic.src_num = 3;
		if (mutation == 9) generic.src[0].negate = true;
		if (mutation == 10) bad.GetInstructions()[0].export_target = 0x21; // Legacy format/raw-target mismatch.
		if (mutation == 11) input.rect_list = true;
		if (ShaderVertexParameterExportsSupported(bad, input)) return fail("negative-parameter-proof", mutation);
	}
	for (const auto destination: {ShaderOperandType::ExecLo, ShaderOperandType::ExecHi})
	{
		auto bad = code;
		bad.GetInstructions()[32].dst.type = destination;
		if (ShaderVertexParameterExportsSupported(bad, vertex)) return fail("negative-exec-destination", static_cast<uint32_t>(destination));
	}
	for (const auto type: {ShaderInstructionType::SAndSaveexecB64, ShaderInstructionType::VCmpxEqU32,
	                      ShaderInstructionType::VCmpxGeI32, ShaderInstructionType::VCmpxGeU32, ShaderInstructionType::VCmpxGtF32,
	                      ShaderInstructionType::VCmpxGtU32, ShaderInstructionType::VCmpxLtF32, ShaderInstructionType::VCmpxNeqF32,
	                      ShaderInstructionType::VCmpxNeU32})
	{
		auto bad = code;
		bad.GetInstructions()[32].type = type;
		if (ShaderVertexParameterExportsSupported(bad, vertex)) return fail("negative-exec-opcode", static_cast<uint32_t>(type));
	}
	// Keep constructed legacy IR (unset target) and its old lowerer intact.
	auto legacy_target = code;
	legacy_target.GetInstructions()[0].export_target = 0xff;
	if (!ShaderVertexParameterExportsSupported(legacy_target, vertex) ||
	    SpirvGenerateSource(legacy_target, &vertex, nullptr, nullptr) != source) return fail("legacy-target-identity");
	ShaderPixelInputInfo pixel;
	pixel.input_num = 1;
	pixel.interpolator_settings[0] = 31;
	if (!ShaderVertexPixelInterfaceSupported(vertex, pixel, 135, 128) ||
	    ShaderVertexPixelInterfaceSupported(vertex, pixel, 134, 128) ||
	    ShaderVertexPixelInterfaceSupported(vertex, pixel, 135, 124) ||
	    ShaderVertexPixelInterfaceSupported(vertex, pixel, 0, 128) ||
	    ShaderVertexPixelInterfaceSupported(vertex, pixel, 135, 0)) return fail("host-interface-limits");
	for (uint32_t mutation = 0; mutation < 7; ++mutation)
	{
		auto bad = pixel;
		auto input = vertex;
		if (mutation == 0) bad.input_num = 33;
		if (mutation == 1) bad.interpolator_settings[0] |= 0x20u;
		if (mutation == 2) bad.interpolator_settings[0] |= 0x800u;
		if (mutation == 3) { bad.input_num = 2; bad.interpolator_settings[1] = 31; }
		if (mutation == 4) input.export_count = 31;
		if (mutation == 5) input.parameter_exports_valid = false;
		if (mutation == 6) input.parameter_export_mask &= ~(uint32_t{1} << 31);
		if (ShaderVertexPixelInterfaceSupported(input, bad, 135, 128)) return fail("negative-pixel-interface", mutation);
	}
	pixel.ps_pos_xy = true;
	pixel.input_num = 32;
	for (uint32_t location = 0; location < 32; ++location) pixel.interpolator_settings[location] = location;
	if (ShaderVertexPixelInterfaceSupported(vertex, pixel, 135, 131) ||
	    !ShaderVertexPixelInterfaceSupported(vertex, pixel, 135, 132)) return fail("fragment-position-capacity");
	// Unconsumed declarations need no invented exports; only PARAM31 is linked.
	const uint32_t sparse_fixture[] = {0xf80003ffu, 0x03020100u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
	ShaderCode sparse_code;
	sparse_code.SetType(ShaderType::Vertex);
	ShaderParse(sparse_fixture, &sparse_code);
	vertex.parameter_exports_valid = ShaderVertexParameterExportsSupported(sparse_code, vertex, nullptr, nullptr,
	                                                                      &vertex.parameter_export_mask);
	if (!vertex.parameter_exports_valid || vertex.parameter_export_mask != (uint32_t{1} << 31)) return fail("sparse-producer");
	pixel = {};
	pixel.input_num = 1; pixel.interpolator_settings[0] = 31;
	if (!ShaderVertexPixelInterfaceSupported(vertex, pixel, 135, 128)) return fail("sparse-interface");
	const uint32_t pixel_fixture[] = {0xc8010000u, 0xf800000fu, 0x03020100u, 0xbf810000u}; // P2 attr0.x -> v0
	ShaderCode pixel_code;
	pixel_code.SetType(ShaderType::Pixel);
	ShaderParse(pixel_fixture, &pixel_code);
	pixel.target_output_mode[0] = 9;
	pixel.shader_mask = 0xf;
	const auto pixel_source = SpirvGenerateSource(pixel_code, nullptr, &pixel, nullptr);
	const std::string_view pixel_text(pixel_source.c_str(), pixel_source.Size());
	if (pixel_text.find("OpDecorate %attr0 Location 31") == std::string_view::npos ||
	    pixel_text.find("OpAccessChain %_ptr_Input_float %attr0 %uint_0") == std::string_view::npos) return fail("pixel-source");
	const auto pixel_module = ShaderRecompilePS(pixel_code, &pixel);
	validation_error.clear();
	if (pixel_module.Size() < 5 || !validator.Validate(pixel_module.GetDataConst(), pixel_module.Size()))
	{
		std::fprintf(stderr, "[GFX_PARAMETER_EXPORT_TEST_FAIL] phase=pixel-module words=%u error=%.1024s\n",
		             pixel_module.Size(), validation_error.c_str());
		return false;
	}
	vertex.parameter_export_mask = 1;
	vertex.export_count = 29;
	pixel.interpolator_settings[0] = 0;
	if (!ShaderVertexPixelInterfaceSupported(vertex, pixel, 124, 124)) return fail("device-capacity-29");
	vertex.export_count = 30;
	if (ShaderVertexPixelInterfaceSupported(vertex, pixel, 124, 124)) return fail("device-capacity-30");
	vertex.export_count = 21;
	vertex.parameter_export_mask = uint32_t{1} << 20;
	pixel.interpolator_settings[0] = 20;
	if (ShaderVertexPixelInterfaceSupported(vertex, pixel, 135, 64)) return fail("sparse-location-capacity");
	std::fprintf(stderr, "[GFX_PARAMETER_EXPORT_TEST] targets=0..31 flags=canonical outputs=float4 locations=preserved"
	                     " current-producers=checked exec-mutation=reject caps=explicit sparse-location=checked spirv=valid gpu=not-tested\n");
	return true;
}

bool CheckImplicitSampleLodZeroPipeline()
{
	constexpr uint32_t fixture[] = {
	    MimgWord(0x20u, false, false, 0xfu),
	    MimgOperands(2, 0, 0, 4), // image_sample v0:3, v4:6, s0:7, s8:11
	    MimgWord(0x27u, false, false, 0xfu),
	    MimgOperands(2, 0, 0, 4), // image_sample_lz v0:3, v4:6, s0:7, s8:11
	    0xf800180fu,
	    0x03020100u, // export MRT0 v0:v3
	    0xbf810000u,
	};
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);
	if (code.GetInstructions().Size() != 4 || code.GetInstructions().At(0).type != ShaderInstructionType::ImageSample ||
	    code.GetInstructions().At(1).type != ShaderInstructionType::ImageSampleLz)
	{
		std::fprintf(stderr, "implicit/explicit image-sample fixture did not parse as expected\n");
		return false;
	}

	ShaderPixelInputInfo input {};
	input.target_output_mode[0]                  = 9;
	input.shader_mask                            = 0xfu;
	input.bind.textures2D.textures_num           = 1;
	input.bind.textures2D.textures2d_sampled_num = 1;
	input.bind.textures2D.desc[0].usage          = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].start_register = 0;
	input.bind.textures2D.desc[0].texture.fields[3] =
	    static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u;
	input.bind.samplers.samplers_num      = 1;
	input.bind.samplers.start_register[0] = 8;
	if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &input.bind))
	{
		std::fprintf(stderr, "implicit/explicit image-sample fixture was rejected\n");
		return false;
	}
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);

	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, &input, nullptr);
	const std::string_view text(source.c_str(), source.Size());
	const bool forced_lod_zero = Lumen::Libs::Graphics::ShaderImplicitSampleLodZeroExperimentEnabled();
	const size_t expected_implicit = forced_lod_zero ? 0u : 1u;
	const size_t expected_explicit = forced_lod_zero ? 2u : 1u;
	if (CountSubstring(text, "OpImageSampleImplicitLod %v4float") != expected_implicit ||
	    CountSubstring(text, "OpImageSampleExplicitLod %v4float") != expected_explicit ||
	    CountSubstring(text, "Lod %float_0_000000") != expected_explicit ||
	    text.find("<implicit_sample>") != std::string_view::npos)
	{
		std::fprintf(stderr,
		             "implicit ImageSample LOD-zero lowering changed: selected=%u implicit=%zu explicit=%zu lod0=%zu\n",
		             forced_lod_zero ? 1u : 0u, CountSubstring(text, "OpImageSampleImplicitLod %v4float"),
		             CountSubstring(text, "OpImageSampleExplicitLod %v4float"), CountSubstring(text, "Lod %float_0_000000"));
		return false;
	}

	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	const auto module = Lumen::Libs::Graphics::ShaderRecompilePS(code, &input);
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "implicit ImageSample LOD-zero SPIR-V validation failed: %s\n", validation_error.c_str());
		return false;
	}
	return true;
}

bool CheckType8SingleLevelImageBridge()
{
	constexpr uint32_t fixture[] = {
	    MimgWord(0x01u, false, true, 0xfu),
	    MimgOperands(0, 1, 2, 6),                               // image_load_mip v2:5, v6:9, s4:11
	    0xc0000000u | (0x03u << 22u) | (1u << 9u) | (1u << 8u), // s_load_dwordx8 s0:7, s2:3, 0
	    MimgWord(0x08u, true, true, 0xfu),
	    MimgOperands(0, 0, 2, 0), // image_store v2:5, v0:2, s0:7
	    0xbf810000u,
	};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);

	ShaderComputeInputInfo input {};
	input.threads_num[0]                            = 8;
	input.threads_num[1]                            = 8;
	input.threads_num[2]                            = 1;
	input.thread_ids_num                            = 2;
	input.workgroup_register                        = 16;
	input.bind.textures2D.textures_num              = 2;
	input.bind.textures2D.textures2d_sampled_num    = 1;
	input.bind.textures2D.textures2d_storage_num    = 1;
	input.bind.textures2D.desc[0].usage             = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].start_register    = 4;
	input.bind.textures2D.desc[0].texture.fields[1] = (3u << 30u) | (10u << 20u) | (1u << 6u);
	input.bind.textures2D.desc[0].texture.fields[2] = 1u;
	input.bind.textures2D.desc[0].texture.fields[3] =
	    (static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_8_COMPAT) << 28u) | (8u << 20u) | 0xfacu;
	// Exercise the padded pitch used by this compatibility shape: logical width 2,
	// physical pitch 64, and a single logical row.
	input.bind.textures2D.desc[0].texture.fields[4]          = 63u << 13u;
	input.bind.textures2D.desc[1].usage                      = Lumen::Libs::Graphics::ShaderTextureUsage::ReadWrite;
	input.bind.textures2D.desc[1].textures2d_without_sampler = true;
	input.bind.textures2D.desc[1].start_register             = 16;
	input.bind.textures2D.desc[1].extended                   = true;
	input.bind.textures2D.desc[1].texture.fields[1]          = input.bind.textures2D.desc[0].texture.fields[1];
	input.bind.textures2D.desc[1].texture.fields[2]          = input.bind.textures2D.desc[0].texture.fields[2];
	input.bind.textures2D.desc[1].texture.fields[3]          = input.bind.textures2D.desc[0].texture.fields[3];
	input.bind.textures2D.desc[1].texture.fields[4]          = input.bind.textures2D.desc[0].texture.fields[4];
	input.bind.extended.used                                 = true;
	input.bind.extended.start_register                       = 2;
	if (!Lumen::Libs::Graphics::ShaderTextureType8SingleLevelCompatible(input.bind.textures2D.desc[0].texture))
	{
		std::fprintf(stderr, "canonical guest Type-8 descriptor was rejected\n");
		return false;
	}
	if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &input.bind))
	{
		std::fprintf(stderr, "canonical guest Type-8 operations were rejected\n");
		return false;
	}
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);

	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &input);
	const std::string_view text(source.c_str(), source.Size());
	if (text.find("OpImageFetch %v4float %t27_0 %t73_0 Lod %uint_0") == std::string_view::npos ||
	    text.find("OpCompositeConstruct %v2uint %t69_0 %uint_0") == std::string_view::npos ||
	    text.find("%vsharp_1_s0 = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_2 %int_0") == std::string_view::npos ||
	    text.find("%vsharp_1_s7 = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_3 %int_3") == std::string_view::npos ||
	    text.find("OpCompositeConstruct %v2uint %t69_2 %uint_0") == std::string_view::npos ||
	    text.find("OpImageWrite %t27_2 %t73_2 %t88_2") == std::string_view::npos ||
	    text.find("OpDecorate %textures2D_L Coherent") == std::string_view::npos)
	{
		std::fprintf(stderr, "guest Type-8 single-level coordinate, LOD or coherence lowering changed\n");
		return false;
	}

	const auto           module = Lumen::Libs::Graphics::ShaderRecompileCS(code, &input);
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "guest Type-8 single-level SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}

	input.bind.textures2D.desc[0].texture.fields[3] =
	    (static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u) | (8u << 20u) | 0xfacu;
	input.bind.textures2D.desc[1].texture.fields[3] = input.bind.textures2D.desc[0].texture.fields[3];
	constexpr uint32_t two_dimensional_fixture[]    = {
        MimgWord(0x01u, false, true, 0xfu), MimgOperands(0, 1, 2, 6), 0xc0000000u | (0x03u << 22u) | (1u << 9u) | (1u << 8u),
        MimgWord(0x08u, false, true, 0xfu), MimgOperands(0, 0, 2, 0), 0xbf810000u,
    };
	ShaderCode two_dimensional_code;
	two_dimensional_code.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(two_dimensional_fixture, &two_dimensional_code);
	if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(two_dimensional_code, &input.bind) ||
	    !Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &input.bind) ||
	    Lumen::Libs::Graphics::ShaderComputeRequiresPostDispatchImageStoreBarrier(two_dimensional_code) ||
	    !Lumen::Libs::Graphics::ShaderComputeRequiresPostDispatchImageStoreBarrier(code))
	{
		std::fprintf(stderr, "guest-2D coherent-store admission or barrier classification changed\n");
		return false;
	}
	const auto two_dimensional_source = Lumen::Libs::Graphics::SpirvGenerateSource(two_dimensional_code, nullptr, nullptr, &input);
	const std::string_view two_dimensional_text(two_dimensional_source.c_str(), two_dimensional_source.Size());
	if (two_dimensional_text.find("OpImageFetch %v4float %t27_0 %t73_0 Lod %t75_0") == std::string_view::npos ||
	    two_dimensional_text.find("OpCompositeConstruct %v2uint %t69_0 %t71_0") == std::string_view::npos ||
	    two_dimensional_text.find("OpCompositeConstruct %v2uint %t69_2 %t71_2") == std::string_view::npos ||
	    two_dimensional_text.find("OpDecorate %textures2D_L Coherent") != std::string_view::npos)
	{
		std::fprintf(stderr, "existing guest-2D coordinate, LOD or coherence lowering changed\n");
		return false;
	}
	const auto             coherent_two_dimensional_source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &input);
	const std::string_view coherent_two_dimensional_text(coherent_two_dimensional_source.c_str(),
	                                                     coherent_two_dimensional_source.Size());
	if (coherent_two_dimensional_text.find("OpImageWrite %t27_2 %t73_2 %t88_2") == std::string_view::npos ||
	    coherent_two_dimensional_text.find("OpDecorate %textures2D_L Coherent") == std::string_view::npos)
	{
		std::fprintf(stderr, "guest-2D coherent ImageStore lowering changed\n");
		return false;
	}
	ShaderBindResources alternate_dimensions = input.bind;
	alternate_dimensions.textures2D.desc[1].texture.fields[2] = 63u | (31u << 14u);
	alternate_dimensions.textures2D.desc[1].texture.fields[3] =
	    (static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u) | (13u << 20u) | 0xfacu;
	alternate_dimensions.textures2D.desc[1].texture.fields[4] = 127u << 13u;
	if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &alternate_dimensions))
	{
		std::fprintf(stderr, "guest-2D coherent-store policy unexpectedly depends on dimensions or pitch\n");
		return false;
	}
	ShaderBindResources srgb_storage = input.bind;
	ShaderBindResources bc3_storage = input.bind;
	ShaderBindResources swizzled_storage = input.bind;
	srgb_storage.textures2D.desc[1].texture.fields[1] |= 9u << 26u;
	bc3_storage.textures2D.desc[1].texture.fields[1] =
	    (bc3_storage.textures2D.desc[1].texture.fields[1] & ~(0x3fu << 20u)) | (37u << 20u);
	bc3_storage.textures2D.desc[1].texture.fields[1] |= 9u << 26u;
	swizzled_storage.textures2D.desc[1].texture.fields[3] &= ~0xfffu;
	for (const auto* rejected_bind: {&srgb_storage, &bc3_storage, &swizzled_storage})
	{
		if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, rejected_bind))
		{
			std::fprintf(stderr, "unsupported guest-2D coherent storage format was admitted\n");
			return false;
		}
	}
	validation_error.clear();
	const auto two_dimensional_module = Lumen::Libs::Graphics::ShaderRecompileCS(two_dimensional_code, &input);
	if (two_dimensional_module.Size() < 5 || two_dimensional_module.At(0) != 0x07230203u ||
	    !validator.Validate(two_dimensional_module.GetDataConst(), two_dimensional_module.Size()))
	{
		std::fprintf(stderr, "existing guest-2D SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}
	validation_error.clear();
	const auto coherent_two_dimensional_module = Lumen::Libs::Graphics::ShaderRecompileCS(code, &input);
	if (coherent_two_dimensional_module.Size() < 5 || coherent_two_dimensional_module.At(0) != 0x07230203u ||
	    !validator.Validate(coherent_two_dimensional_module.GetDataConst(), coherent_two_dimensional_module.Size()))
	{
		std::fprintf(stderr, "guest-2D coherent SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}

	constexpr uint32_t normalized_store_fixture[] = {
	    MimgWord(0x08u, true, false, 0xfu), MimgOperands(0, 0, 2, 0), 0xbf810000u,
	};
	constexpr uint32_t mip_store_fixture[] = {
	    MimgWord(0x09u, true, true, 0xfu), MimgOperands(0, 0, 2, 0), 0xbf810000u,
	};
	constexpr uint32_t post_store_read_fixture[] = {
	    MimgWord(0x08u, true, true, 0xfu), MimgOperands(0, 0, 2, 0),
	    MimgWord(0x01u, false, true, 0xfu), MimgOperands(0, 1, 2, 6), 0xbf810000u,
	};
	constexpr uint32_t post_store_backedge_fixture[] = {
	    MimgWord(0x01u, false, true, 0xfu), MimgOperands(0, 1, 2, 6),
	    MimgWord(0x08u, true, true, 0xfu), MimgOperands(0, 0, 2, 0),
	    0xbf82fffbu,
	    0xbf810000u,
	};
	ShaderCode normalized_store_code;
	ShaderCode mip_store_code;
	ShaderCode post_store_read_code;
	ShaderCode post_store_backedge_code;
	for (auto* candidate: {&normalized_store_code, &mip_store_code, &post_store_read_code, &post_store_backedge_code})
	{
		candidate->SetType(ShaderType::Compute);
	}
	Lumen::Libs::Graphics::ShaderParse(normalized_store_fixture, &normalized_store_code);
	Lumen::Libs::Graphics::ShaderParse(mip_store_fixture, &mip_store_code);
	Lumen::Libs::Graphics::ShaderParse(post_store_read_fixture, &post_store_read_code);
	Lumen::Libs::Graphics::ShaderParse(post_store_backedge_fixture, &post_store_backedge_code);
	ShaderCode partial_mask_store_code = code;
	for (auto& inst: partial_mask_store_code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::ImageStore && inst.globally_coherent)
		{
			inst.dst.size = 3;
		}
	}
	ShaderCode graphics_stage_store_code = code;
	graphics_stage_store_code.SetType(ShaderType::Pixel);
	for (const auto* rejected: {&normalized_store_code, &partial_mask_store_code, &mip_store_code, &post_store_read_code,
	                            &post_store_backedge_code, &graphics_stage_store_code})
	{
		if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(*rejected, &input.bind) ||
		    Lumen::Libs::Graphics::ShaderComputeRequiresPostDispatchImageStoreBarrier(*rejected))
		{
			std::fprintf(stderr, "unsupported guest-2D coherent-store profile was admitted\n");
			return false;
		}
	}
	return true;
}

bool CheckType8PixelSampleBridge()
{
	constexpr uint32_t load_type8 =
	    0xc0000000u | (0x03u << 22u) | (16u << 15u) | (1u << 9u) | (1u << 8u); // s_load_dwordx8 s16:23, s2:3, 0
	constexpr uint32_t load_sampler = 0xc0000000u | (0x02u << 22u) | (24u << 15u) | (1u << 9u) | (1u << 8u) |
	                                  8u; // s_load_dwordx4 s24:27, s2:3, byte offset 32
	constexpr uint32_t fixture[] = {
	    load_type8,
	    load_sampler,
	    MimgWord(0x20u, false, false, 0xfu),
	    MimgOperands(3, 1, 4, 0), // image_sample v4:7, v0:2, s4:11, s12:15
	    MimgWord(0x20u, false, false, 0xfu),
	    MimgOperands(6, 4, 0, 3), // image_sample v0:3, v3:5, s16:23, s24:27
	    0xf800180fu,
	    0x03020100u, // export MRT0 v0:v3
	    0xbf810000u,
	};
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);
	if (code.GetInstructions().Size() != 6 || code.GetInstructions().At(2).type != ShaderInstructionType::ImageSample ||
	    code.GetInstructions().At(3).type != ShaderInstructionType::ImageSample)
	{
		std::fprintf(stderr, "mixed 2D/Type-8 pixel fixture did not parse as expected\n");
		return false;
	}

	ShaderPixelInputInfo input {};
	input.target_output_mode[0]                  = 9;
	input.shader_mask                            = 0xfu;
	input.bind.extended.used                     = true;
	input.bind.extended.start_register           = 2;
	input.bind.textures2D.textures_num           = 2;
	input.bind.textures2D.textures2d_sampled_num = 2;
	input.bind.textures2D.desc[0].usage          = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].start_register = 4;
	input.bind.textures2D.desc[0].texture.fields[3] =
	    static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u;
	input.bind.textures2D.desc[1].usage             = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[1].start_register    = 16;
	input.bind.textures2D.desc[1].extended          = true;
	input.bind.textures2D.desc[1].texture.fields[1] = 10u << 20u;
	input.bind.textures2D.desc[1].texture.fields[2] = 1u;
	input.bind.textures2D.desc[1].texture.fields[3] =
	    (static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_8_COMPAT) << 28u) | (1u << 26u) | (8u << 20u) |
	    0xfacu;
	input.bind.textures2D.desc[1].texture.fields[4] = 63u << 13u;
	input.bind.samplers.samplers_num                = 2;
	input.bind.samplers.start_register[0]           = 12;
	input.bind.samplers.start_register[1]           = 24;
	input.bind.samplers.extended[1]                 = true;

	if (!Lumen::Libs::Graphics::ShaderTextureType8PixelSampleCompatible(input.bind.textures2D.desc[1].texture) ||
	    Lumen::Libs::Graphics::ShaderTextureType8SingleLevelCompatible(input.bind.textures2D.desc[1].texture) ||
	    !Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &input.bind))
	{
		std::fprintf(stderr, "mixed 2D/Type-8 pixel sample profile was rejected\n");
		return false;
	}
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);
	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, &input, nullptr);
	const std::string_view text(source.c_str(), source.Size());
	if (text.find("OpCompositeConstruct %v2float %t39_2 %t40_2") == std::string_view::npos ||
	    text.find("OpCompositeConstruct %v2float %t39_3 %float_0_500000") == std::string_view::npos ||
	    text.find("%vsharp_0_s16 = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_2 %int_0") ==
	        std::string_view::npos ||
	    text.find("%vsharp_1_s24 = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_5 %int_0") ==
	        std::string_view::npos)
	{
		std::fprintf(stderr, "mixed 2D/Type-8 pixel coordinate or extended mapping changed\n");
		return false;
	}

	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	const auto module = Lumen::Libs::Graphics::ShaderRecompilePS(code, &input);
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "mixed 2D/Type-8 pixel SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}
	for (const uint32_t mask: {0x1u, 0x2u, 0x3u, 0x4u, 0x5u, 0x7u, 0x8u, 0x9u, 0xfu})
	{
		// The 1D operation retains RGBA while the independent 2D operation
		// selects its own packed channels, including alpha-only R8 views.
		const uint32_t masked_fixture[] = {
		    load_type8, load_sampler,
		    MimgWord(0x20u, false, false, 0xfu), MimgOperands(6, 4, 0, 3),
		    MimgWord(0x20u, false, false, mask), MimgOperands(3, 1, 4, 0),
		    0xf800180fu, 0x03020100u, 0xbf810000u,
		};
		ShaderCode masked_code;
		masked_code.SetType(ShaderType::Pixel);
		Lumen::Libs::Graphics::ShaderParse(masked_fixture, &masked_code);
		ShaderPixelInputInfo masked_input = input;
		auto& texture2d = masked_input.bind.textures2D.desc[0].texture;
		texture2d.fields[1] = 1u << 20u;
		texture2d.fields[2] = (511u << 14u) | 511u;
		texture2d.fields[3] = (Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D << 28u) | (1u << 26u) |
		                       (13u << 20u) | Lumen::Libs::Graphics::DstSel(4, 4, 4, 4);
		texture2d.fields[4] = 511u << 13u;
		for (uint32_t reordered = 0; reordered < 2; ++reordered)
		{
			if (reordered != 0)
			{
				std::swap(masked_input.bind.textures2D.desc[0], masked_input.bind.textures2D.desc[1]);
			}
			if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(masked_code, &masked_input.bind))
			{
				std::fprintf(stderr, "mixed Type-8/F and 2D dmask %x rejected, reordered=%u\n", mask, reordered);
				return false;
			}
			Lumen::Libs::Graphics::ShaderCalcBindingIndices(&masked_input.bind);
			const auto masked_source = Lumen::Libs::Graphics::SpirvGenerateSource(masked_code, nullptr, &masked_input, nullptr);
			const std::string_view masked_text(masked_source.c_str(), masked_source.Size());
			if (masked_text.find("OpCompositeConstruct %v2float %t39_2 %float_0_500000") == std::string_view::npos ||
			    masked_text.find("OpCompositeConstruct %v2float %t39_3 %t40_3") == std::string_view::npos)
			{
				std::fprintf(stderr, "mixed masked sample changed per-descriptor coordinates\n");
				return false;
			}
			for (uint32_t channel = 0; channel < 4; ++channel)
			{
				const auto extract = "OpAccessChain %_ptr_Function_float %temp_v4float %uint_" + std::to_string(channel);
				const size_t expected_count = 1u + ((mask >> channel) & 1u);
				if (CountSubstring(masked_text, extract) != expected_count)
				{
					std::fprintf(stderr, "mixed 2D dmask %x changed packed channel %u\n", mask, channel);
					return false;
				}
			}
			validation_error.clear();
			const auto masked_module = Lumen::Libs::Graphics::ShaderRecompilePS(masked_code, &masked_input);
			if (masked_module.Size() < 5 || !validator.Validate(masked_module.GetDataConst(), masked_module.Size()))
			{
				std::fprintf(stderr, "mixed masked sample SPIR-V failed: %s\n", validation_error.c_str());
				return false;
			}
		}
		if (mask == 0x2u || mask == 0x4u || mask == 0x8u)
		{
			const auto reject_changed_sample = [&](const ShaderInstruction& sample)
			{
				ShaderCode changed = masked_code;
				changed.GetInstructions()[3] = sample;
				return !Lumen::Libs::Graphics::ShaderTextureOperationsSupported(changed, &masked_input.bind);
			};
			const auto original = masked_code.GetInstructions().At(3);
			auto malformed = original;
			malformed.dst.size = 2;
			if (!reject_changed_sample(malformed)) return false;
			malformed = original;
			malformed.src[0].size = 2;
			if (!reject_changed_sample(malformed)) return false;
			malformed = original;
			malformed.src[1].size = 4;
			if (!reject_changed_sample(malformed)) return false;
			malformed = original;
			malformed.src[2].size = 3;
			if (!reject_changed_sample(malformed)) return false;
			malformed = original;
			malformed.unnormalized_coordinates = true;
			if (!reject_changed_sample(malformed)) return false;
			malformed = original;
			malformed.globally_coherent = true;
			if (!reject_changed_sample(malformed)) return false;
			malformed = original;
			malformed.type = ShaderInstructionType::ImageSampleLz;
			if (!reject_changed_sample(malformed)) return false;
			malformed = original;
			malformed.src[1].register_id = 28;
			if (!reject_changed_sample(malformed)) return false;
			ShaderCode type8_alpha = masked_code;
			type8_alpha.GetInstructions()[2].format = original.format;
			type8_alpha.GetInstructions()[2].dst.size = 1;
			if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(type8_alpha, &masked_input.bind))
			{
				std::fprintf(stderr, "Type-8 single-channel sample was incorrectly admitted by the 2D extension\n");
				return false;
			}
		}
	}

	ShaderCode wrong_format = code;
	ShaderCode unnormalized = code;
	ShaderCode coherent     = code;
	ShaderCode unresolved   = code;
	ShaderCode wrong_stage  = code;
	ShaderCode extended_clobber = code;
	ShaderCode direct_clobber   = code;
	ShaderCode crossed_branch   = code;
	ShaderCode pointer_clobber   = code;
	ShaderCode skipped_load      = code;
	wrong_format.GetInstructions()[3].format = ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7;
	unnormalized.GetInstructions()[3].unnormalized_coordinates = true;
	coherent.GetInstructions()[3].globally_coherent            = true;
	unresolved.GetInstructions()[3].src[1].register_id          = 20;
	wrong_stage.SetType(ShaderType::Compute);
	ShaderInstruction clobber {};
	clobber.type            = ShaderInstructionType::SMovB32;
	clobber.dst.type        = ShaderOperandType::Sgpr;
	clobber.dst.register_id = 16;
	clobber.dst.size        = 1;
	extended_clobber.GetInstructions().InsertAt(3, clobber);
	clobber.dst.register_id = 4;
	direct_clobber.GetInstructions().InsertAt(2, clobber);
	ShaderInstruction branch {};
	branch.type = ShaderInstructionType::SBranch;
	crossed_branch.GetInstructions().InsertAt(3, branch);
	clobber.dst.register_id = 2;
	pointer_clobber.GetInstructions().InsertAt(0, clobber);
	skipped_load.GetLabels().Add(
	    Lumen::Libs::Graphics::ShaderLabel(skipped_load.GetInstructions().At(3).pc, skipped_load.GetInstructions().At(0).pc));
	for (const auto* rejected: {&wrong_format, &unnormalized, &coherent, &unresolved, &wrong_stage})
	{
		if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(*rejected, &input.bind))
		{
			std::fprintf(stderr, "unsupported Type-8 pixel sample profile was admitted\n");
			return false;
		}
	}
	for (const auto* rejected: {&extended_clobber, &direct_clobber, &crossed_branch, &pointer_clobber, &skipped_load})
	{
		if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(*rejected, &input.bind))
		{
			std::fprintf(stderr, "ambiguous Type-8 pixel descriptor provenance was admitted\n");
			return false;
		}
	}

	ShaderCode nonzero_offset = code;
	nonzero_offset.GetInstructions()[0].src[1].constant.u = 4;
	ShaderBindResources nonzero_offset_bind = input.bind;
	nonzero_offset_bind.textures2D.desc[1].start_register = 17;
	if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(nonzero_offset, &nonzero_offset_bind))
	{
		std::fprintf(stderr, "aligned nonzero extended Type-8 descriptor offset was rejected\n");
		return false;
	}

	ShaderBindResources homogeneous = input.bind;
	homogeneous.textures2D.desc[0].texture = homogeneous.textures2D.desc[1].texture;
	if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &homogeneous))
	{
		std::fprintf(stderr, "homogeneous Type-8 pixel sample profile was rejected\n");
		return false;
	}
	ShaderBindResources wrong_memory = input.bind;
	wrong_memory.textures2D.desc[1].texture.fields[1] |= (3u << 30u) | (1u << 6u);
	wrong_memory.textures2D.desc[1].texture.fields[3] &= ~(1u << 26u);
	if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &wrong_memory))
	{
		std::fprintf(stderr, "compute/storage TYPE-8 memory profile was admitted as a pixel sampled profile\n");
		return false;
	}
	ShaderCode homogeneous_unresolved = code;
	homogeneous_unresolved.GetInstructions()[3].src[1].register_id = 20;
	if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(homogeneous_unresolved, &homogeneous))
	{
		std::fprintf(stderr, "homogeneous unresolved Type-8 pixel descriptor mapping was admitted\n");
		return false;
	}
	constexpr uint32_t type2d_branch_fixture[] = {
	    0xbf820000u, // s_branch to the following image_sample basic block
	    MimgWord(0x20u, false, false, 0xfu),
	    MimgOperands(3, 1, 0, 3),
	    0xf800180fu,
	    0x03020100u,
	    0xbf810000u,
	};
	ShaderCode type2d_branch;
	type2d_branch.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(type2d_branch_fixture, &type2d_branch);
	ShaderPixelInputInfo type2d_input {};
	type2d_input.target_output_mode[0]                  = 9;
	type2d_input.shader_mask                            = 0xfu;
	type2d_input.bind.textures2D.textures_num           = 1;
	type2d_input.bind.textures2D.textures2d_sampled_num = 1;
	type2d_input.bind.textures2D.desc[0]                = input.bind.textures2D.desc[0];
	type2d_input.bind.samplers.samplers_num              = 1;
	type2d_input.bind.samplers.start_register[0]         = 12;
	if (!Lumen::Libs::Graphics::ShaderTextureOperationsSupported(type2d_branch, &type2d_input.bind))
	{
		std::fprintf(stderr, "established homogeneous 2D pixel profile was changed by Type-8 provenance checks\n");
		return false;
	}
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&type2d_input.bind);
	const auto             type2d_source = Lumen::Libs::Graphics::SpirvGenerateSource(type2d_branch, nullptr, &type2d_input, nullptr);
	const std::string_view type2d_text(type2d_source.c_str(), type2d_source.Size());
	if (type2d_text.find("OpCompositeConstruct %v2float %t39_1 %t40_1") == std::string_view::npos ||
	    type2d_text.find("OpCompositeConstruct %v2float %t39_1 %float_0_500000") != std::string_view::npos)
	{
		std::fprintf(stderr, "established homogeneous 2D pixel coordinate lowering changed\n");
		return false;
	}
	const auto type2d_module = Lumen::Libs::Graphics::ShaderRecompilePS(type2d_branch, &type2d_input);
	validation_error.clear();
	if (type2d_module.Size() < 5 || type2d_module.At(0) != 0x07230203u ||
	    !validator.Validate(type2d_module.GetDataConst(), type2d_module.Size()))
	{
		std::fprintf(stderr, "established homogeneous 2D branch SPIR-V validation failed: %s\n", validation_error.c_str());
		return false;
	}
	return true;
}

bool CheckR8UnormStorageImageBridge()
{
	constexpr uint32_t fixture[] = {
	    MimgWord(0x01u, false, true, 0xfu),
	    MimgOperands(0, 1, 2, 6),
	    0xc0000000u | (0x03u << 22u) | (1u << 9u) | (1u << 8u),
	    MimgWord(0x08u, true, true, 0xfu),
	    MimgOperands(0, 0, 2, 0),
	    0xbf810000u,
	};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);

	ShaderComputeInputInfo input {};
	input.threads_num[0]                            = 8;
	input.threads_num[1]                            = 8;
	input.threads_num[2]                            = 1;
	input.thread_ids_num                            = 2;
	input.workgroup_register                        = 16;
	input.bind.extended.used                        = true;
	input.bind.extended.start_register              = 2;
	input.bind.textures2D.textures_num              = 2;
	input.bind.textures2D.textures2d_sampled_num    = 1;
	input.bind.textures2D.textures2d_storage_num    = 1;
	input.bind.textures2D.desc[0].usage             = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].start_register    = 4;
	input.bind.textures2D.desc[0].texture.fields[0] = 0x01000000u;
	input.bind.textures2D.desc[0].texture.fields[1] = 1u << 20u;
	input.bind.textures2D.desc[0].texture.fields[2] = 511u | (511u << 14u);
	input.bind.textures2D.desc[0].texture.fields[3] =
	    (static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u) | (1u << 26u) |
	    (8u << 20u) | Lumen::Libs::Graphics::DstSel(4, 0, 0, 1);
	input.bind.textures2D.desc[0].texture.fields[4]          = 511u << 13u;
	input.bind.textures2D.desc[1].usage                      = Lumen::Libs::Graphics::ShaderTextureUsage::ReadWrite;
	input.bind.textures2D.desc[1].textures2d_without_sampler = true;
	input.bind.textures2D.desc[1].start_register             = 16;
	input.bind.textures2D.desc[1].extended                   = true;
	input.bind.textures2D.desc[1].texture.fields[0]          = 0x02000000u;
	input.bind.textures2D.desc[1].texture.fields[1]          = (3u << 30u) | (1u << 20u) | (1u << 6u);
	input.bind.textures2D.desc[1].texture.fields[2]          = input.bind.textures2D.desc[0].texture.fields[2];
	input.bind.textures2D.desc[1].texture.fields[3] =
	    (static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u) | (13u << 20u) |
	    Lumen::Libs::Graphics::DstSel(4, 4, 4, 4);
	input.bind.textures2D.desc[1].texture.fields[4] = 511u << 13u;

	const auto& sampled = input.bind.textures2D.desc[0].texture;
	const auto& storage = input.bind.textures2D.desc[1].texture;
	if (!Lumen::Libs::Graphics::ShaderTextureR8UnormSampledCompatible(sampled) ||
	    !Lumen::Libs::Graphics::ShaderTextureR8UnormStorageCompatible(storage) ||
	    Lumen::Libs::Graphics::ShaderTextureStorageViewSwizzle(storage) != Lumen::Libs::Graphics::DstSel(4, 5, 6, 7) ||
	    Lumen::Libs::Graphics::ShaderGetUniformStorageImageFormat(input.bind.textures2D) !=
	        Lumen::Libs::Graphics::ShaderStorageImageFormat::R8Unorm ||
	    !Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &input.bind))
	{
		std::fprintf(stderr, "canonical R8_UNORM storage-image profile was rejected\n");
		return false;
	}

	for (int mutation = 0; mutation < 8; ++mutation)
	{
		auto rejected = storage;
		switch (mutation)
		{
			case 0: rejected.fields[1] ^= 1u << 26u; break;
			case 1: rejected.fields[2] |= 1u << 28u; break;
			case 2: rejected.fields[3] ^= 1u << 20u; break;
			case 3: rejected.fields[3] ^= 1u; break;
			case 4: rejected.fields[4] = 510u << 13u; break;
			case 5: rejected.fields[5] = 1u; break;
			case 6: rejected.fields[6] = 1u; break;
			case 7: rejected.fields[7] = 1u; break;
		}
		if (Lumen::Libs::Graphics::ShaderTextureR8UnormStorageCompatible(rejected))
		{
			std::fprintf(stderr, "malformed R8_UNORM storage descriptor was admitted (%d)\n", mutation);
			return false;
		}
	}

	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);
	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &input);
	const std::string_view text(source.c_str(), source.Size());
	if (text.find("OpCapability StorageImageExtendedFormats") == std::string_view::npos ||
	    text.find("%ImageL = OpTypeImage %float 2D 0 0 0 2 R8") == std::string_view::npos ||
	    text.find("OpImageWrite") == std::string_view::npos)
	{
		std::fprintf(stderr, "R8_UNORM SPIR-V image type, capability or store is missing\n");
		return false;
	}

	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	const auto module = Lumen::Libs::Graphics::ShaderRecompileCS(code, &input);
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "R8_UNORM storage-image SPIR-V validation failed: %s\n", validation_error.c_str());
		return false;
	}

	ShaderComputeInputInfo rgba_input = input;
	auto& rgba = rgba_input.bind.textures2D.desc[1].texture;
	rgba.fields[1] = (rgba.fields[1] & ~((0x3fu << 20u) | (0xfu << 26u))) | (10u << 20u);
	rgba.fields[3] = (rgba.fields[3] & ~0xfffu) | Lumen::Libs::Graphics::DstSel(4, 5, 6, 7);
	if (Lumen::Libs::Graphics::ShaderGetUniformStorageImageFormat(rgba_input.bind.textures2D) !=
	        Lumen::Libs::Graphics::ShaderStorageImageFormat::Rgba8 ||
	    !Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &rgba_input.bind))
	{
		std::fprintf(stderr, "established RGBA8 storage-image profile changed\n");
		return false;
	}
	const auto             rgba_source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &rgba_input);
	const std::string_view rgba_text(rgba_source.c_str(), rgba_source.Size());
	if (rgba_text.find("%ImageL = OpTypeImage %float 2D 0 0 0 2 Rgba8") == std::string_view::npos ||
	    rgba_text.find("OpCapability StorageImageExtendedFormats") != std::string_view::npos)
	{
		std::fprintf(stderr, "RGBA8 storage-image SPIR-V type or capability changed\n");
		return false;
	}

	ShaderBindResources mixed = input.bind;
	mixed.textures2D.textures_num           = 3;
	mixed.textures2D.textures2d_storage_num = 2;
	mixed.textures2D.desc[2]                = rgba_input.bind.textures2D.desc[1];
	if (Lumen::Libs::Graphics::ShaderGetUniformStorageImageFormat(mixed.textures2D) !=
	        Lumen::Libs::Graphics::ShaderStorageImageFormat::Invalid ||
	    Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &mixed))
	{
		std::fprintf(stderr, "mixed typed storage-image array was admitted\n");
		return false;
	}

	ShaderCode noncoherent = code;
	ShaderCode normalized  = code;
	for (auto& inst: noncoherent.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::ImageStore)
		{
			inst.globally_coherent = false;
		}
	}
	for (auto& inst: normalized.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::ImageStore)
		{
			inst.unnormalized_coordinates = false;
		}
	}
	if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(noncoherent, &input.bind) ||
	    Lumen::Libs::Graphics::ShaderTextureOperationsSupported(normalized, &input.bind))
	{
		std::fprintf(stderr, "unsupported R8_UNORM storage operation was admitted\n");
		return false;
	}

	constexpr uint32_t shader_fixture[] = {
	    0xBEEB03FFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000100u,
	    0x00000000u, 0xa4537592u, 0xc33cbd00u, 0x4c554d45u,
	};
	Lumen::Libs::Graphics::HW::ComputeShaderInfo regs {};
	regs.cs_regs.data_addr = reinterpret_cast<uint64_t>(shader_fixture);
	const auto r8_id_a     = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &input);
	input.bind.textures2D.desc[1].texture.fields[0] ^= 0x100u;
	const auto r8_id_b = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &input);
	const auto rgba_id = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &rgba_input);
	input.bind.textures2D.desc[1].texture.fields[0] ^= 0x100u;
	const auto r8_id_c = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &input);
	if (r8_id_a != r8_id_b || r8_id_a != r8_id_c || r8_id_a == rgba_id)
	{
		std::fprintf(stderr, "storage-image format cache key does not preserve R8-RGBA8-R8 isolation\n");
		return false;
	}
	return true;
}

bool CheckRgba8StorageAdjacentSingleWindowBridge()
{
	if (!Lumen::Libs::Graphics::StorageTextureWindowSupported(0, 1) ||
	    !Lumen::Libs::Graphics::StorageTextureWindowSupported(0, 15) ||
	    !Lumen::Libs::Graphics::StorageTextureWindowSupported(1, 2) ||
	    !Lumen::Libs::Graphics::StorageTextureWindowSupported(2, 3) ||
	    !Lumen::Libs::Graphics::StorageTextureWindowSupported(14, 15) ||
	    !Lumen::Libs::Graphics::StorageTextureUploadLevelIncluded(0, 3, 0) ||
	    !Lumen::Libs::Graphics::StorageTextureUploadLevelIncluded(0, 3, 2) ||
	    !Lumen::Libs::Graphics::StorageTextureUploadLevelIncluded(2, 3, 2) ||
	    Lumen::Libs::Graphics::StorageTextureUploadLevelIncluded(2, 3, 0) ||
	    Lumen::Libs::Graphics::StorageTextureUploadLevelIncluded(2, 3, 1) ||
	    Lumen::Libs::Graphics::StorageTextureWindowSupported(0, 0) ||
	    Lumen::Libs::Graphics::StorageTextureWindowSupported(1, 1) ||
	    Lumen::Libs::Graphics::StorageTextureWindowSupported(1, 3) ||
	    Lumen::Libs::Graphics::StorageTextureWindowSupported(15, 16) ||
	    Lumen::Libs::Graphics::StorageTextureWindowSupported(0, 16))
	{
		std::fprintf(stderr, "storage texture single-window contract changed\n");
		return false;
	}

	constexpr uint32_t fixture[] = {
	    MimgWord(0x01u, false, true, 0xfu), MimgOperands(0, 1, 2, 6),
	    MimgWord(0x01u, false, true, 0xfu), MimgOperands(0, 1, 2, 6),
	    MimgWord(0x01u, false, true, 0xfu), MimgOperands(0, 1, 2, 6),
	    MimgWord(0x01u, false, true, 0xfu), MimgOperands(0, 1, 2, 6),
	    0xc0c00300u, // s_load_dwordx8 s0:7, s2:3, byte offset 0
	    0xbf8c0000u, // s_waitcnt
	    MimgWord(0x08u, true, true, 0xfu), MimgOperands(0, 0, 2, 0),
	    0xbf810000u,
	};
	ShaderCode code;
	code.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);
	if (code.GetInstructions().Size() != 8 || code.GetInstructions().At(4).type != ShaderInstructionType::SLoadDwordx8 ||
	    code.GetInstructions().At(5).type != ShaderInstructionType::SWaitcnt ||
	    code.GetInstructions().At(6).type != ShaderInstructionType::ImageStore)
	{
		std::fprintf(stderr, "RGBA8 adjacent-mip storage fixture instruction layout changed\n");
		return false;
	}

	ShaderComputeInputInfo input {};
	input.threads_num[0]                            = 8;
	input.threads_num[1]                            = 8;
	input.threads_num[2]                            = 1;
	input.thread_ids_num                            = 2;
	input.workgroup_register                        = 16;
	input.bind.extended.used                        = true;
	input.bind.extended.start_register              = 2;
	input.bind.textures2D.textures_num              = 2;
	input.bind.textures2D.textures2d_sampled_num    = 1;
	input.bind.textures2D.textures2d_storage_num    = 1;
	input.bind.textures2D.desc[0].usage             = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].start_register    = 4;
	input.bind.textures2D.desc[0].texture.fields[0] = 0x04b171b4u;
	input.bind.textures2D.desc[0].texture.fields[1] = 0xc0a00040u;
	input.bind.textures2D.desc[0].texture.fields[2] = 0x0035c4cdu;
	input.bind.textures2D.desc[0].texture.fields[3] = 0x92d00facu;
	input.bind.textures2D.desc[0].texture.fields[4] = 0x00ffe000u;
	input.bind.textures2D.desc[1]                   = input.bind.textures2D.desc[0];
	input.bind.textures2D.desc[1].usage             = Lumen::Libs::Graphics::ShaderTextureUsage::ReadWrite;
	input.bind.textures2D.desc[1].textures2d_without_sampler = true;
	input.bind.textures2D.desc[1].start_register             = 16;
	input.bind.textures2D.desc[1].extended                   = true;
	input.bind.textures2D.desc[1].texture.fields[3]          = 0x92d11facu;

	const auto set_window = [](auto& texture, uint32_t base_level, uint32_t last_level)
	{
		constexpr uint32_t mip_window_mask = 0x000ff000u;
		texture.fields[3] = (texture.fields[3] & ~mip_window_mask) | ((base_level & 0xfu) << 12u) |
		                    ((last_level & 0xfu) << 16u);
	};
	const auto with_windows = [&input, &set_window](uint32_t sampled_base, uint32_t sampled_last, uint32_t storage_base,
	                                              uint32_t storage_last)
	{
		ShaderComputeInputInfo candidate = input;
		set_window(candidate.bind.textures2D.desc[0].texture, sampled_base, sampled_last);
		set_window(candidate.bind.textures2D.desc[1].texture, storage_base, storage_last);
		return candidate;
	};

	constexpr uint32_t adjacent_profile =
	    Lumen::Libs::Graphics::SHADER_TEXTURE_STORAGE_MIP_PROFILE_ADJACENT_SINGLE_WINDOW;
	constexpr uint32_t legacy_atlas_profile = Lumen::Libs::Graphics::SHADER_TEXTURE_STORAGE_MIP_PROFILE_LEGACY_ATLAS;
	if (adjacent_profile != 2u || legacy_atlas_profile != 1u)
	{
		std::fprintf(stderr, "storage mip profile IDs changed\n");
		return false;
	}

	struct MipPair
	{
		uint32_t sampled;
		uint32_t storage;
	};
	constexpr MipPair accepted_pairs[] = {{0u, 1u}, {1u, 2u}, {2u, 3u}, {9u, 10u}};
	for (const auto pair: accepted_pairs)
	{
		const auto  accepted = with_windows(pair.sampled, pair.sampled, pair.storage, pair.storage);
		const auto& textures = accepted.bind.textures2D;
		if (textures.desc[0].texture.Width4() + 1u != 1230u || textures.desc[0].texture.Height4() + 1u != 216u ||
		    textures.desc[0].texture.Pitch() + 1u != 2048u ||
		    Lumen::Libs::Graphics::ShaderTextureStorageMipProfileId(textures) != adjacent_profile ||
		    Lumen::Libs::Graphics::ShaderTextureStorageMipProfileId(textures) == legacy_atlas_profile ||
		    !Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(textures, ShaderType::Compute) ||
		    Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(textures, ShaderType::Pixel) ||
		    !Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &accepted.bind) ||
		    Lumen::Libs::Graphics::ShaderClassifyComputeImageMutation(code, accepted.bind) !=
		        Lumen::Libs::Graphics::ShaderComputeImageMutationClass::AdjacentSampledToStorage ||
		    !Lumen::Libs::Graphics::ShaderComputeRequiresPostDispatchImageStoreBarrier(code))
		{
			std::fprintf(stderr, "canonical RGBA8 adjacent singleton profile %u->%u was rejected\n", pair.sampled,
			             pair.storage);
			return false;
		}
	}

	ShaderComputeInputInfo reordered = input;
	const auto             sampled_descriptor = reordered.bind.textures2D.desc[0];
	reordered.bind.textures2D.desc[0]          = reordered.bind.textures2D.desc[1];
	reordered.bind.textures2D.desc[1]          = sampled_descriptor;
	const auto& reordered_textures             = reordered.bind.textures2D;
	if (reordered_textures.textures_num != 2 || reordered_textures.textures2d_sampled_num != 1 ||
	    reordered_textures.textures2d_storage_num != 1 ||
	    Lumen::Libs::Graphics::ShaderTextureStorageMipProfileId(reordered_textures) != adjacent_profile ||
	    !Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(reordered_textures, ShaderType::Compute) ||
	    Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(reordered_textures, ShaderType::Pixel) ||
	    !Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &reordered.bind) ||
	    Lumen::Libs::Graphics::ShaderClassifyComputeImageMutation(code, reordered.bind) !=
	        Lumen::Libs::Graphics::ShaderComputeImageMutationClass::AdjacentSampledToStorage)
	{
		std::fprintf(stderr, "RGBA8 adjacent singleton profile depended on sampled/storage descriptor record order\n");
		return false;
	}

	ShaderBindResources index_only_reordered = input.bind;
	const int           sampled_start        = index_only_reordered.textures2D.desc[0].start_register;
	const bool          sampled_extended     = index_only_reordered.textures2D.desc[0].extended;
	index_only_reordered.textures2D.desc[0].start_register = index_only_reordered.textures2D.desc[1].start_register;
	index_only_reordered.textures2D.desc[0].extended       = index_only_reordered.textures2D.desc[1].extended;
	index_only_reordered.textures2D.desc[1].start_register = sampled_start;
	index_only_reordered.textures2D.desc[1].extended       = sampled_extended;
	if (Lumen::Libs::Graphics::ShaderTextureStorageMipProfileId(index_only_reordered.textures2D) != adjacent_profile ||
	    !Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(index_only_reordered.textures2D,
	                                                                            ShaderType::Compute) ||
	    Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &index_only_reordered))
	{
		std::fprintf(stderr, "RGBA8 adjacent singleton partial descriptor-index reorder bypassed provenance validation\n");
		return false;
	}

	for (int invalid_window = 0; invalid_window < 7; ++invalid_window)
	{
		ShaderComputeInputInfo rejected {};
		switch (invalid_window)
		{
			case 0: rejected = with_windows(1u, 1u, 1u, 1u); break;  // Same level.
			case 1: rejected = with_windows(1u, 1u, 3u, 3u); break;  // Gap.
			case 2: rejected = with_windows(2u, 2u, 1u, 1u); break;  // Reverse.
			case 3: rejected = with_windows(1u, 2u, 2u, 2u); break;  // Sampled is not a singleton.
			case 4: rejected = with_windows(1u, 1u, 2u, 3u); break;  // Storage is not a singleton.
			case 5: rejected = with_windows(10u, 10u, 11u, 11u); break; // Exceeds the 1230x216 physical chain.
			case 6: rejected = with_windows(14u, 14u, 15u, 15u); break; // levels == 16.
			default: return false;
		}
		if (Lumen::Libs::Graphics::ShaderTextureStorageMipProfileId(rejected.bind.textures2D) != 0u ||
		    Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(rejected.bind.textures2D,
		                                                                            ShaderType::Compute) ||
		    Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &rejected.bind))
		{
			std::fprintf(stderr, "invalid RGBA8 adjacent singleton window was admitted (%d)\n", invalid_window);
			return false;
		}
	}

	for (int mutation = 0; mutation < 8; ++mutation)
	{
		ShaderBindResources rejected = input.bind;
		switch (mutation)
		{
			case 0: rejected.textures2D.desc[1].texture.fields[0] ^= 1u; break;
			case 1: rejected.textures2D.desc[1].texture.fields[1] ^= 1u << 20u; break;
			case 2: rejected.textures2D.desc[1].texture.fields[2] ^= 1u; break;
			case 3: rejected.textures2D.desc[1].texture.fields[3] ^= 1u << 16u; break;
			case 4: rejected.textures2D.desc[1].texture.fields[3] ^= 1u; break;
			case 5: rejected.textures2D.desc[1].texture.fields[3] ^= 1u << 20u; break;
			case 6: rejected.textures2D.desc[1].texture.fields[4] ^= 1u << 13u; break;
			case 7: rejected.textures2D.desc[1].texture.fields[5] = 1u; break;
			default: return false;
		}
		if (Lumen::Libs::Graphics::ShaderTextureStorageMipProfileId(rejected.textures2D) != 0u ||
		    Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(rejected.textures2D, ShaderType::Compute) ||
		    Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &rejected))
		{
			std::fprintf(stderr, "mutated RGBA8 adjacent singleton descriptor profile was admitted (%d)\n", mutation);
			return false;
		}
	}

	ShaderBindResources wrong_count = input.bind;
	ShaderBindResources wrong_role  = input.bind;
	wrong_count.textures2D.textures2d_sampled_num            = 2;
	wrong_role.textures2D.desc[0].textures2d_without_sampler = true;
	for (const auto* rejected: {&wrong_count, &wrong_role})
	{
		if (Lumen::Libs::Graphics::ShaderTextureStorageMipProfileId(rejected->textures2D) != 0u ||
		    Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(rejected->textures2D, ShaderType::Compute) ||
		    Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, rejected))
		{
			std::fprintf(stderr, "malformed RGBA8 adjacent singleton resource roles were admitted\n");
			return false;
		}
	}

	ShaderBindResources base_zero = input.bind;
	base_zero.textures2D.desc[1].texture.fields[3] &= ~0x000ff000u;
	if (Lumen::Libs::Graphics::ShaderTextureStorageMipProfileId(base_zero.textures2D) != 0u ||
	    !Lumen::Libs::Graphics::ShaderTextureDescriptorProfileSupportedPreCache(base_zero.textures2D, ShaderType::Compute) ||
	    !Lumen::Libs::Graphics::ShaderTextureOperationsSupported(code, &base_zero) ||
	    Lumen::Libs::Graphics::ShaderClassifyComputeImageMutation(code, base_zero) !=
	        Lumen::Libs::Graphics::ShaderComputeImageMutationClass::UniqueStorageTarget)
	{
		std::fprintf(stderr, "historical base-zero RGBA8 storage profile changed\n");
		return false;
	}

	ShaderCode noncoherent_store = code;
	ShaderCode nonterminal_store = code;
	ShaderCode unresolved_sample = code;
	ShaderCode unresolved_store  = code;
	noncoherent_store.GetInstructions()[6].globally_coherent = false;
	nonterminal_store.GetInstructions().InsertAt(7, code.GetInstructions().At(0));
	unresolved_sample.GetInstructions()[0].src[1].register_id = 20;
	unresolved_store.GetInstructions()[4].src[1].constant.u   = 4;
	for (const auto* rejected: {&noncoherent_store, &nonterminal_store, &unresolved_sample, &unresolved_store})
	{
		if (Lumen::Libs::Graphics::ShaderTextureOperationsSupported(*rejected, &input.bind))
		{
			std::fprintf(stderr, "unsupported RGBA8 adjacent singleton operation or descriptor provenance was admitted\n");
			return false;
		}
	}
	if (Lumen::Libs::Graphics::ShaderComputeRequiresPostDispatchImageStoreBarrier(noncoherent_store) ||
	    Lumen::Libs::Graphics::ShaderComputeRequiresPostDispatchImageStoreBarrier(nonterminal_store) ||
	    Lumen::Libs::Graphics::ShaderClassifyComputeImageMutation(noncoherent_store, input.bind) !=
	        Lumen::Libs::Graphics::ShaderComputeImageMutationClass::None ||
	    Lumen::Libs::Graphics::ShaderClassifyComputeImageMutation(nonterminal_store, input.bind) !=
	        Lumen::Libs::Graphics::ShaderComputeImageMutationClass::None ||
	    Lumen::Libs::Graphics::ShaderClassifyComputeImageMutation(unresolved_store, input.bind) !=
	        Lumen::Libs::Graphics::ShaderComputeImageMutationClass::None ||
	    Lumen::Libs::Graphics::ShaderClassifyComputeImageMutation(unresolved_sample, input.bind) ==
	        Lumen::Libs::Graphics::ShaderComputeImageMutationClass::AdjacentSampledToStorage)
	{
		std::fprintf(stderr, "invalid RGBA8 adjacent singleton store retained the post-dispatch barrier classification\n");
		return false;
	}

	constexpr uint32_t shader_fixture[] = {
	    0xBEEB03FFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000100u,
	    0x00000000u, 0xa4537592u, 0xc33cbd00u, 0x4c554d45u,
	};
	Lumen::Libs::Graphics::HW::ComputeShaderInfo regs {};
	regs.cs_regs.data_addr = reinterpret_cast<uint64_t>(shader_fixture);
	const auto adjacent_id    = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &input);
	const auto mip_two_input  = with_windows(1u, 1u, 2u, 2u);
	const auto mip_three_input = with_windows(2u, 2u, 3u, 3u);
	const auto last_input      = with_windows(9u, 9u, 10u, 10u);
	const auto mip_two_id      = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &mip_two_input);
	const auto mip_three_id = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &mip_three_input);
	const auto last_id      = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &last_input);
	ShaderComputeInputInfo moved_address = input;
	moved_address.bind.textures2D.desc[0].texture.fields[0] ^= 0x100u;
	moved_address.bind.textures2D.desc[1].texture.fields[0] ^= 0x100u;
	const auto moved_address_id = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &moved_address);
	ShaderComputeInputInfo base_zero_input = input;
	base_zero_input.bind = base_zero;
	const auto base_zero_id = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &base_zero_input);
	if (adjacent_id != mip_two_id || adjacent_id != mip_three_id || adjacent_id != last_id ||
	    adjacent_id != moved_address_id || adjacent_id == base_zero_id)
	{
		std::fprintf(stderr, "adjacent singleton cache key did not preserve generic-level/address reuse or profile isolation\n");
		return false;
	}
	const auto reordered_id = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &reordered);
	ShaderComputeInputInfo reordered_moved_address = reordered;
	reordered_moved_address.bind.textures2D.desc[0].texture.fields[0] ^= 0x100u;
	reordered_moved_address.bind.textures2D.desc[1].texture.fields[0] ^= 0x100u;
	const auto reordered_moved_address_id = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &reordered_moved_address);
	if (reordered_id == adjacent_id || reordered_id != reordered_moved_address_id)
	{
		std::fprintf(stderr, "adjacent singleton cache key lost descriptor-order isolation or reordered address reuse\n");
		return false;
	}

	auto lowering_input = mip_two_input;
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&lowering_input.bind);
	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &lowering_input);
	const std::string_view text(source.c_str(), source.Size());
	if (CountSubstring(text, "OpImageFetch") != 4 || CountSubstring(text, "OpImageWrite") != 1 ||
	    CountSubstring(text, "Lod %uint_0") != 4 || text.find("Lod %t75_") != std::string_view::npos ||
	    text.find("%storage_mip_") != std::string_view::npos ||
	    text.find("OpCompositeConstruct %v2uint %t69_0 %t71_0") == std::string_view::npos ||
	    text.find("OpImageFetch %v4float %t27_0 %t73_0 Lod %uint_0") == std::string_view::npos ||
	    text.find("OpCompositeConstruct %v2uint %t69_6 %t71_6") == std::string_view::npos ||
	    text.find("OpImageWrite %t27_6 %t73_6 %t88_6") == std::string_view::npos ||
	    text.find("OpDecorate %textures2D_L Coherent") == std::string_view::npos ||
	    text.find("OpCapability StorageImageExtendedFormats") != std::string_view::npos)
	{
		std::fprintf(stderr, "RGBA8 adjacent singleton direct-coordinate/LOD-zero lowering changed\n");
		return false;
	}

	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	const auto module = Lumen::Libs::Graphics::ShaderRecompileCS(code, &lowering_input);
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "RGBA8 adjacent singleton SPIR-V validation failed: %s\n", validation_error.c_str());
		return false;
	}
	return true;
}

bool CheckComputeDirectSgprPipeline()
{
	constexpr uint32_t fixture[] = {0xbf8c0000u, 0xbf8c0000u, 0xbf810000u};
	ShaderCode         code;
	code.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);

	ShaderComputeInputInfo input {};
	input.threads_num[0]                      = 1;
	input.threads_num[1]                      = 1;
	input.threads_num[2]                      = 1;
	input.thread_ids_num                      = 1;
	input.workgroup_register                  = 16;
	input.bind.direct_sgprs.sgprs_num         = 2;
	input.bind.direct_sgprs.start_register[0] = 0;
	input.bind.direct_sgprs.start_register[1] = 1;
	input.bind.direct_sgprs.sgprs[0].field    = 0x13579bdfu;
	input.bind.direct_sgprs.sgprs[1].field    = 0x2468ace0u;
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);
	if (input.bind.push_constant_size != 16)
	{
		std::fprintf(stderr, "two direct compute SGPRs did not reserve one zero-padded push-constant block\n");
		return false;
	}

	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &input);
	const std::string_view text(source.c_str(), source.Size());
	if (text.find("%s0 = OpVariable %_ptr_Function_uint Function") == std::string_view::npos ||
	    text.find("%s1 = OpVariable %_ptr_Function_uint Function") == std::string_view::npos ||
	    text.find("%vsharp_s0 = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_0 %int_0") == std::string_view::npos ||
	    text.find("%vsharp_s1 = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_0 %int_1") == std::string_view::npos)
	{
		std::fprintf(stderr, "direct compute SGPR declarations or push-constant mapping changed\n");
		return false;
	}

	const auto           module = Lumen::Libs::Graphics::ShaderRecompileCS(code, &input);
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "direct compute SGPR SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}
	return true;
}

bool CheckPixelDirectSgprPipeline()
{
	constexpr uint32_t fixture[] = {0xbf8c0000u, 0xf8001c0fu, 0x00000100u, 0xbf810000u};
	ShaderCode         code;
	code.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(fixture, &code);

	ShaderPixelInputInfo input {};
	input.target_output_mode[0]             = 4;
	input.shader_mask                       = 0xfu;
	input.bind.push_constant_offset         = 32;
	input.bind.descriptor_set_slot          = 1;
	input.bind.direct_sgprs.sgprs_num       = 4;
	for (int i = 0; i < 4; ++i)
	{
		input.bind.direct_sgprs.start_register[i] = i;
		input.bind.direct_sgprs.sgprs[i].field    = 0x11111111u * static_cast<uint32_t>(i + 1);
	}
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);
	if (input.bind.push_constant_size != 16 || input.bind.push_constant_offset != 32)
	{
		std::fprintf(stderr, "four direct pixel SGPRs did not reserve one offset push-constant block\n");
		return false;
	}

	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, &input, nullptr);
	const std::string_view text(source.c_str(), source.Size());
	if (text.find("OpMemberDecorate %BufferResource 0 Offset 32") == std::string_view::npos ||
	    text.find("%s0 = OpVariable %_ptr_Function_uint Function") == std::string_view::npos ||
	    text.find("%s3 = OpVariable %_ptr_Function_uint Function") == std::string_view::npos ||
	    text.find("%vsharp_s0 = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_0 %int_0") == std::string_view::npos ||
	    text.find("%vsharp_s3 = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_0 %int_3") == std::string_view::npos)
	{
		std::fprintf(stderr, "direct pixel SGPR declarations, offset or push-constant mapping changed\n");
		return false;
	}

	const auto           module = Lumen::Libs::Graphics::ShaderRecompilePS(code, &input);
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "direct pixel SGPR SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}
	return true;
}

bool CheckVMulLegacyF32Pipeline()
{
	constexpr uint32_t waitcnt                     = 0xbf8c0000u;
	constexpr uint32_t endpgm                      = 0xbf810000u;
	constexpr uint32_t vop2_legacy_program[]       = {waitcnt, 0x0e040300u, endpgm};
	constexpr uint32_t vop3_legacy_pixel_program[] = {
	    waitcnt, 0xd20e0002u, 0x00020300u, 0xf8001c0fu, 0x00000100u, endpgm,
	};
	constexpr uint32_t vop2_literal_program[]  = {waitcnt, 0x0e0402ffu, 0x3f800000u, endpgm};
	constexpr uint32_t vop3_literal_program[]  = {waitcnt, 0xd20e0002u, 0x000202ffu, 0x3f800000u, endpgm};
	constexpr uint32_t vop2_ordinary_program[] = {waitcnt, 0x10040300u, endpgm};
	constexpr uint32_t vop3_ordinary_program[] = {waitcnt, 0xd2100002u, 0x00020300u, endpgm};

	auto make_admitted = [](ShaderType type)
	{
		ShaderCode code;
		code.SetType(type);
		code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
		return code;
	};

	ShaderCode vertex = make_admitted(ShaderType::Vertex);
	Lumen::Libs::Graphics::ShaderParse(vop2_legacy_program, &vertex);
	if (vertex.GetInstructions().Size() != 3)
	{
		std::fprintf(stderr, "legacy VOP2 fixture parsed %u instructions instead of 3\n", vertex.GetInstructions().Size());
		return false;
	}
	const auto& vop2 = vertex.GetInstructions().At(1);
	if (vop2.type != ShaderInstructionType::VMulLegacyF32 || vop2.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || vop2.pc != 4 ||
	    vop2.src_num != 2 || vop2.src[0].register_id != 0 || vop2.src[1].register_id != 1 || vop2.dst.register_id != 2 ||
	    vop2.src[0].absolute || vop2.src[1].absolute || vop2.src[0].negate || vop2.src[1].negate || vop2.dst.clamp ||
	    vop2.dst.multiplier != 1.0f)
	{
		std::fprintf(stderr, "legacy VOP2 operand or modifier shape changed\n");
		return false;
	}

	ShaderCode pixel = make_admitted(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(vop3_legacy_pixel_program, &pixel);
	if (pixel.GetInstructions().Size() != 4)
	{
		std::fprintf(stderr, "legacy VOP3 pixel fixture parsed %u instructions instead of 4\n", pixel.GetInstructions().Size());
		return false;
	}
	const auto& vop3 = pixel.GetInstructions().At(1);
	if (vop3.type != ShaderInstructionType::VMulLegacyF32 || vop3.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || vop3.pc != 4 ||
	    vop3.src_num != 2 || vop3.src[0].register_id != 0 || vop3.src[1].register_id != 1 || vop3.dst.register_id != 2 ||
	    vop3.src[0].absolute || vop3.src[1].absolute || vop3.src[0].negate || vop3.src[1].negate || vop3.dst.clamp ||
	    vop3.dst.multiplier != 1.0f)
	{
		std::fprintf(stderr, "legacy VOP3 operand or modifier shape changed\n");
		return false;
	}

	ShaderCode vop2_literal = make_admitted(ShaderType::Vertex);
	Lumen::Libs::Graphics::ShaderParse(vop2_literal_program, &vop2_literal);
	ShaderCode vop3_literal = make_admitted(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(vop3_literal_program, &vop3_literal);
	if (vop2_literal.GetInstructions().Size() != 3 || vop2_literal.GetInstructions().At(1).src[0].constant.u != 0x3f800000u ||
	    vop2_literal.GetInstructions().At(2).pc != 12 || vop3_literal.GetInstructions().Size() != 3 ||
	    vop3_literal.GetInstructions().At(1).src[0].constant.u != 0x3f800000u || vop3_literal.GetInstructions().At(2).pc != 16)
	{
		std::fprintf(stderr, "legacy literal consumption changed instruction boundaries\n");
		return false;
	}

	ShaderCode ordinary_vop2;
	ordinary_vop2.SetType(ShaderType::Vertex);
	Lumen::Libs::Graphics::ShaderParse(vop2_ordinary_program, &ordinary_vop2);
	ShaderCode ordinary_vop3;
	ordinary_vop3.SetType(ShaderType::Pixel);
	Lumen::Libs::Graphics::ShaderParse(vop3_ordinary_program, &ordinary_vop3);
	if (ordinary_vop2.GetInstructions().At(1).type != ShaderInstructionType::VMulF32 ||
	    ordinary_vop3.GetInstructions().At(1).type != ShaderInstructionType::VMulF32)
	{
		std::fprintf(stderr, "ordinary VMulF32 opcode mapping changed\n");
		return false;
	}

	ShaderCode admitted_pixel = make_admitted(ShaderType::Pixel);
	ShaderCode unvalidated;
	unvalidated.SetType(ShaderType::Vertex);
	ShaderCode compute = make_admitted(ShaderType::Compute);
	if (!Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(vertex, false, false, 0, 0, 0, false, 0) ||
	    !Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(admitted_pixel, false, false, 0, 0, 0, false, 0) ||
	    Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(unvalidated, false, false, 0, 0, 0, false, 0) ||
	    Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(compute, false, false, 0, 0, 0, false, 0))
	{
		std::fprintf(stderr, "legacy VMul graphics-stage/FP-mode admission changed\n");
		return false;
	}
	for (const uint32_t mask: {1u, 2u, 4u})
	{
		if (Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(vertex, false, false, mask, 0, 0, false, 0) ||
		    Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(vertex, false, false, 0, mask, 0, false, 0))
		{
			std::fprintf(stderr, "legacy VMul accepted a source modifier mask 0x%x\n", mask);
			return false;
		}
	}
	for (const uint32_t omod: {1u, 2u, 3u})
	{
		if (Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(vertex, false, false, 0, 0, omod, false, 0))
		{
			std::fprintf(stderr, "legacy VMul accepted OMOD %u\n", omod);
			return false;
		}
	}
	if (Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(vertex, true, false, 0, 0, 0, false, 0) ||
	    Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(vertex, false, true, 0, 0, 0, false, 0) ||
	    Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(vertex, false, false, 0, 0, 0, true, 0) ||
	    Lumen::Libs::Graphics::ShaderVMulLegacyF32FormSupported(vertex, false, false, 0, 0, 0, false, 1))
	{
		std::fprintf(stderr, "legacy VMul accepted an excluded lane, clamp or unused-source form\n");
		return false;
	}

	const auto             vertex_source = Lumen::Libs::Graphics::SpirvGenerateSource(vertex, nullptr, nullptr, nullptr);
	const std::string_view vertex_text(vertex_source.c_str(), vertex_source.Size());
	const auto             multiply_label  = vertex_text.find("%vmul_legacy_multiply_1 = OpLabel");
	const auto             product         = vertex_text.find("%vmul_legacy_product_1 = OpFMul %float %t0_1 %t1_1");
	const auto             product_zero    = vertex_text.find("%vmul_legacy_product_zero_1 = OpIEqual %bool");
	const auto             flush_label     = vertex_text.find("%vmul_legacy_flush_1 = OpLabel");
	const auto             wide_product    = vertex_text.find("%vmul_legacy_sig_product_1 = OpUMulExtended %ResTypeU");
	const auto             boundary_select = vertex_text.find("%vmul_legacy_underflow_bits_1 = OpSelect %uint");
	const auto             output_merge    = vertex_text.find("%vmul_legacy_output_merge_1 = OpLabel");
	const auto             value_merge     = vertex_text.find("%vmul_legacy_value_merge_1 = OpLabel");
	if (multiply_label == std::string_view::npos || product == std::string_view::npos || product_zero == std::string_view::npos ||
	    flush_label == std::string_view::npos || wide_product == std::string_view::npos || boundary_select == std::string_view::npos ||
	    output_merge == std::string_view::npos || value_merge == std::string_view::npos ||
	    !(multiply_label < product && product < product_zero && product_zero < flush_label && flush_label < wide_product &&
	      wide_product < boundary_select && boundary_select < output_merge && output_merge < value_merge) ||
	    CountSubstring(vertex_text, "%vmul_legacy_product_1 = OpFMul") != 1 || CountSubstring(vertex_text, "OpUMulExtended") != 1 ||
	    vertex_text.find("%vmul_legacy_boundary_exp_1 = OpIEqual %bool %vmul_legacy_exp_sum_1 %uint_127") == std::string_view::npos ||
	    vertex_text.find("%vmul_legacy_boundary_hi_1 = OpIEqual %bool %vmul_legacy_sig_product_hi_1 %uint_0x00007fff") ==
	        std::string_view::npos ||
	    vertex_text.find("%vmul_legacy_boundary_lo_1 = OpUGreaterThanEqual %bool %vmul_legacy_sig_product_lo_1 %uint_0xff800000") ==
	        std::string_view::npos ||
	    vertex_text.find("%vmul_legacy_signed_min_normal_bits_1 = OpBitwiseOr %uint %vmul_legacy_signed_zero_bits_1 %uint_0x00800000") ==
	        std::string_view::npos ||
	    CountSubstring(vertex_text, "OpPhi %float") < 2 || vertex_text.find("OpFOrd") != std::string_view::npos ||
	    vertex_text.find("OpFUnord") != std::string_view::npos || vertex_text.find("OpSelect %float") != std::string_view::npos ||
	    vertex_text.find("OpStore %v2 %t_1") == std::string_view::npos)
	{
		std::fprintf(stderr, "legacy VMul SPIR-V changed its lazy multiply, boundary correction, CFG or store shape\n");
		return false;
	}

	ShaderVertexInputInfo vertex_input {};
	const auto            vertex_module = Lumen::Libs::Graphics::ShaderRecompileVS(vertex, &vertex_input);
	ShaderPixelInputInfo  pixel_input {};
	pixel_input.target_output_mode[0]   = 4;
	pixel_input.shader_mask             = 0xfu;
	const auto             pixel_source = Lumen::Libs::Graphics::SpirvGenerateSource(pixel, nullptr, &pixel_input, nullptr);
	const std::string_view pixel_text(pixel_source.c_str(), pixel_source.Size());
	if (CountSubstring(pixel_text, "%vmul_legacy_product_1 = OpFMul") != 1 || CountSubstring(pixel_text, "OpUMulExtended") != 1 ||
	    pixel_text.find("%vmul_legacy_underflow_bits_1 = OpSelect %uint") == std::string_view::npos ||
	    pixel_text.find("%vmul_legacy_value_merge_1 = OpLabel") == std::string_view::npos ||
	    pixel_text.find("OpFOrd") != std::string_view::npos || pixel_text.find("OpFUnord") != std::string_view::npos ||
	    pixel_text.find("OpSelect %float") != std::string_view::npos)
	{
		std::fprintf(stderr, "legacy VMul pixel lowering diverged from the admitted CFG\n");
		return false;
	}
	const auto pixel_module = Lumen::Libs::Graphics::ShaderRecompilePS(pixel, &pixel_input);

	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	if (vertex_module.Size() < 5 || vertex_module.At(0) != 0x07230203u ||
	    !validator.Validate(vertex_module.GetDataConst(), vertex_module.Size()))
	{
		std::fprintf(stderr, "legacy VMul vertex SPIR-V validation failed: %s\n", validation_error.c_str());
		return false;
	}
	validation_error.clear();
	if (pixel_module.Size() < 5 || pixel_module.At(0) != 0x07230203u ||
	    !validator.Validate(pixel_module.GetDataConst(), pixel_module.Size()))
	{
		std::fprintf(stderr, "legacy VMul pixel SPIR-V validation failed: %s\n", validation_error.c_str());
		return false;
	}

	return true;
}

bool VMacLegacyF32CandidateAdmitted()
{
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
	code.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
	return Lumen::Libs::Graphics::ShaderVMacLegacyF32FormSupported(code, 267u, 267u, 258u, false, false, 0, 0, 0, false);
}

bool CheckVMacLegacyF32Pipeline()
{
	if (!VMacLegacyF32CandidateAdmitted())
	{
		return true;
	}

	constexpr uint32_t waitcnt                  = 0xbf8c0000u;
	constexpr uint32_t endpgm                   = 0xbf810000u;
	constexpr uint32_t observed_program[]       = {waitcnt, 0x0c04170bu, endpgm};
	constexpr uint32_t dst_src0_alias_program[] = {waitcnt, 0x0c16030bu, endpgm};
	constexpr uint32_t dst_src1_alias_program[] = {waitcnt, 0x0c161702u, endpgm};

	auto make_admitted = []()
	{
		ShaderCode code;
		code.SetType(ShaderType::Vertex);
		code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
		code.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
		return code;
	};

	ShaderCode observed = make_admitted();
	Lumen::Libs::Graphics::ShaderParse(observed_program, &observed);
	if (observed.GetInstructions().Size() != 3)
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 observed fixture parsed %u instructions instead of 3\n",
		             observed.GetInstructions().Size());
		return false;
	}
	const auto& mac = observed.GetInstructions().At(1);
	if (mac.type != ShaderInstructionType::VMacLegacyF32 || mac.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || mac.pc != 4 ||
	    mac.src_num != 2 || mac.src[0].type != ShaderOperandType::Vgpr || mac.src[0].register_id != 11 ||
	    mac.src[1].type != ShaderOperandType::Vgpr || mac.src[1].register_id != 11 || mac.dst.type != ShaderOperandType::Vgpr ||
	    mac.dst.register_id != 2 || mac.dst2.type != ShaderOperandType::Unknown || mac.src[0].absolute || mac.src[1].absolute ||
	    mac.src[0].negate || mac.src[1].negate || mac.dst.clamp || mac.dst.multiplier != 1.0f ||
	    observed.GetInstructions().At(2).pc != 8)
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 observed VOP2 operand, accumulator or single-word boundary changed\n");
		return false;
	}

	struct AliasFixture
	{
		const uint32_t* program;
		uint32_t        dst;
		uint32_t        src0;
		uint32_t        src1;
	};
	const AliasFixture aliases[] = {
	    {dst_src0_alias_program, 11u, 11u, 1u},
	    {dst_src1_alias_program, 11u, 2u, 11u},
	};
	for (const auto& fixture: aliases)
	{
		ShaderCode alias = make_admitted();
		Lumen::Libs::Graphics::ShaderParse(fixture.program, &alias);
		if (alias.GetInstructions().Size() != 3 || alias.GetInstructions().At(1).type != ShaderInstructionType::VMacLegacyF32 ||
		    alias.GetInstructions().At(1).dst.register_id != fixture.dst || alias.GetInstructions().At(1).src[0].register_id != fixture.src0 ||
		    alias.GetInstructions().At(1).src[1].register_id != fixture.src1 || alias.GetInstructions().At(2).pc != 8)
		{
			std::fprintf(stderr, "V_MAC_LEGACY_F32 dst/source alias fixture changed parser shape\n");
			return false;
		}

		const auto             alias_source = Lumen::Libs::Graphics::SpirvGenerateSource(alias, nullptr, nullptr, nullptr);
		const std::string_view alias_text(alias_source.c_str(), alias_source.Size());
		const auto load0    = alias_text.find("%tt0_1 = OpLoad %float %v" + std::to_string(fixture.src0));
		const auto load1    = alias_text.find("%tt1_1 = OpLoad %float %v" + std::to_string(fixture.src1));
		const auto load_acc = alias_text.find("%ttacc_1 = OpLoad %float %v" + std::to_string(fixture.dst));
		const auto store    = alias_text.find("OpStore %v" + std::to_string(fixture.dst) + " %t_1");
		if (load0 == std::string_view::npos || load1 == std::string_view::npos || load_acc == std::string_view::npos ||
		    store == std::string_view::npos || !(load0 < load1 && load1 < load_acc && load_acc < store))
		{
			std::fprintf(stderr, "V_MAC_LEGACY_F32 alias lowering did not load both inputs and old dst before its store\n");
			return false;
		}
	}

	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(observed, nullptr, nullptr, nullptr);
	const std::string_view text(source.c_str(), source.Size());
	const auto load0       = text.find("%tt0_1 = OpLoad %float %v11");
	const auto load1       = text.find("%tt1_1 = OpLoad %float %v11");
	const auto load_acc    = text.find("%ttacc_1 = OpLoad %float %v2");
	const auto product = text.find("%vmac_legacy_product_bits_1 = OpFunctionCall %uint %lumen_vmac_mul_legacy_bits %t0_1 %t1_1");
	const auto sum = text.find("%vmac_legacy_result_bits_1 = OpFunctionCall %uint %lumen_vmac_add_rne_flush_bits "
	                           "%vmac_legacy_product_bits_1 %tacc_1");
	const auto final_bitcast = text.find("%t_1 = OpBitcast %float %vmac_legacy_result_bits_1");
	const auto store         = text.find("OpStore %v2 %t_1");
	if (load0 == std::string_view::npos || load1 == std::string_view::npos || load_acc == std::string_view::npos ||
	    product == std::string_view::npos || sum == std::string_view::npos || final_bitcast == std::string_view::npos ||
	    store == std::string_view::npos ||
	    !(load0 < load1 && load1 < load_acc && load_acc < product && product < sum && sum < final_bitcast && final_bitcast < store) ||
	    CountSubstring(text, "%lumen_vmac_mul_legacy_bits = OpFunction") != 1 ||
	    CountSubstring(text, "%lumen_vmac_add_rne_flush_bits = OpFunction") != 1 ||
	    CountSubstring(text, "%lumen_vmac_shr_jam_u32 = OpFunction") != 1 ||
	    CountSubstring(text, "OpFunctionCall %uint %lumen_vmac_mul_legacy_bits") != 1 ||
	    CountSubstring(text, "OpFunctionCall %uint %lumen_vmac_add_rne_flush_bits") != 1 ||
	    text.find("%lumen_vmac_mul_wide = OpUMulExtended %ResTypeU") == std::string_view::npos ||
	    text.find("%lumen_vmac_add_norm16_hi_candidate = OpBitwiseOr %uint") == std::string_view::npos ||
	    text.find("%lumen_vmac_add_exact_cancel = OpLogicalAnd %bool") == std::string_view::npos ||
	    text.find("%lumen_vmac_add_result = OpSelect %uint %lumen_vmac_add_canonical_required %uint_0x7fc00000") ==
	        std::string_view::npos ||
	    text.find("OpFMul") != std::string_view::npos || text.find("OpFAdd") != std::string_view::npos ||
	    text.find("Fma") != std::string_view::npos || text.find("NoContraction") != std::string_view::npos ||
	    text.find("SPV_KHR_float_controls") != std::string_view::npos || text.find("RoundingModeRTE") != std::string_view::npos ||
	    text.find("SignedZeroInfNanPreserve") != std::string_view::npos || text.find("FindUMsb") != std::string_view::npos ||
	    text.find("OpTypeInt 64") != std::string_view::npos)
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 shared integer product/add/flush/canonical-NaN structure changed\n");
		return false;
	}

	ShaderVertexInputInfo input {};
	const auto            module = Lumen::Libs::Graphics::ShaderRecompileVS(observed, &input);
	spvtools::SpirvTools  validator(SPV_ENV_VULKAN_1_1);
	std::string           validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 vertex SPIR-V validation failed: %s\n", validation_error.c_str());
		return false;
	}

	return true;
}

bool VRsqClampF32CandidateAdmitted()
{
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	return Lumen::Libs::Graphics::ShaderVRsqClampF32FormSupported(code, 258u, 258u);
}

bool CheckVRsqClampF32Pipeline()
{
	if (!VRsqClampF32CandidateAdmitted())
	{
		return true;
	}

	constexpr uint32_t waitcnt = 0xbf8c0000u;
	constexpr uint32_t endpgm  = 0xbf810000u;
	struct Fixture
	{
		uint32_t word;
		uint32_t dst;
		uint32_t src;
	};
	constexpr Fixture fixtures[] = {
	    {0x7e045902u, 2u, 2u},     // observed source/destination alias
	    {0x7e005900u, 0u, 0u},     // minimum direct VGPR indices
	    {0x7ffe59ffu, 255u, 255u}, // maximum direct VGPR indices
	    {0x7e065905u, 3u, 5u},     // form admission is not tied to the observed registers
	};

	struct StateFixture
	{
		bool rne_flush;
		bool dx10_disabled;
	};
	constexpr StateFixture states[] = {
	    {false, false},
	    {false, true},
	    {true, false},
	    {true, true},
	};

	for (const auto& state: states)
	{
		for (const auto& fixture: fixtures)
		{
			const uint32_t program[] = {waitcnt, fixture.word, endpgm};
			ShaderCode     code;
			code.SetType(ShaderType::Vertex);
			if (state.rne_flush) code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
			if (state.dx10_disabled) code.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
			Lumen::Libs::Graphics::ShaderParse(program, &code);
			if (code.GetInstructions().Size() != 3)
			{
				std::fprintf(stderr, "V_RSQ_CLAMP_F32 fixture parsed %u instructions instead of 3\n",
				             code.GetInstructions().Size());
				return false;
			}
			const auto& rsq = code.GetInstructions().At(1);
			if (rsq.type != ShaderInstructionType::VRsqClampF32 || rsq.format != ShaderInstructionFormat::SVdstSVsrc0 ||
			    rsq.pc != 4 || rsq.src_num != 1 || rsq.src[0].type != ShaderOperandType::Vgpr ||
			    rsq.src[0].register_id != static_cast<int>(fixture.src) || rsq.src[0].size != 1 ||
			    rsq.dst.type != ShaderOperandType::Vgpr || rsq.dst.register_id != static_cast<int>(fixture.dst) || rsq.dst.size != 1 ||
			    rsq.dst2.type != ShaderOperandType::Unknown || rsq.src[0].absolute || rsq.src[0].negate || rsq.dst.clamp ||
			    rsq.dst.multiplier != 1.0f || code.GetInstructions().At(2).pc != 8)
			{
				std::fprintf(stderr, "V_RSQ_CLAMP_F32 compact VOP1 operand or single-word shape changed\n");
				return false;
			}
		}
	}

	ShaderCode admitted;
	admitted.SetType(ShaderType::Vertex);
	if (!Lumen::Libs::Graphics::ShaderVRsqClampF32FormSupported(admitted, 256u, 256u) ||
	    !Lumen::Libs::Graphics::ShaderVRsqClampF32FormSupported(admitted, 511u, 511u) ||
	    Lumen::Libs::Graphics::ShaderVRsqClampF32FormSupported(admitted, 255u, 256u) ||
	    Lumen::Libs::Graphics::ShaderVRsqClampF32FormSupported(admitted, 256u, 512u))
	{
		std::fprintf(stderr, "V_RSQ_CLAMP_F32 direct-VGPR form boundary changed\n");
		return false;
	}

	std::string canonical_source;
	for (const auto& state: states)
	{
		constexpr uint32_t observed_program[] = {waitcnt, 0x7e045902u, endpgm};
		ShaderCode observed;
		observed.SetType(ShaderType::Vertex);
		if (state.rne_flush) observed.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
		if (state.dx10_disabled) observed.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
		Lumen::Libs::Graphics::ShaderParse(observed_program, &observed);

		const auto source = Lumen::Libs::Graphics::SpirvGenerateSource(observed, nullptr, nullptr, nullptr);
		const std::string current_source(source.c_str(), source.Size());
		const std::string_view text(current_source);
		if (canonical_source.empty()) canonical_source = current_source;
		else if (current_source != canonical_source)
		{
			std::fprintf(stderr, "V_RSQ_CLAMP_F32 source changed across FP32/DX10 mode states\n");
			return false;
		}

		const auto load = text.find("%tt0_1 = OpLoad %float %v2");
		const auto source_bits = text.find("%t0_1 = OpBitcast %uint %tt0_1");
		const auto call = text.find("%vrsq_clamp_result_bits_1 = OpFunctionCall %uint %lumen_vrsq_clamp_f32_bits %t0_1");
		const auto result = text.find("%t_1 = OpBitcast %float %vrsq_clamp_result_bits_1");
		const auto selection = text.find("OpSelectionMerge %vrsq_clamp_exec_merge_1 None");
		const auto branch = text.find("OpBranchConditional %exec_lo_b_1 %vrsq_clamp_exec_1 %vrsq_clamp_exec_merge_1");
		const auto active = text.find("%vrsq_clamp_exec_1 = OpLabel");
		const auto store = text.find("OpStore %v2 %t_1");
		const auto merge = text.find("%vrsq_clamp_exec_merge_1 = OpLabel");
		if (load == std::string_view::npos || source_bits == std::string_view::npos || call == std::string_view::npos ||
		    result == std::string_view::npos || selection == std::string_view::npos || branch == std::string_view::npos ||
		    active == std::string_view::npos || store == std::string_view::npos || merge == std::string_view::npos ||
		    !(load < source_bits && source_bits < call && call < result && result < selection && selection < branch &&
		      branch < active && active < store && store < merge) ||
		    CountSubstring(text, "OpStore %v2 %t_1") != 1 ||
		    CountSubstring(text, "%lumen_vrsq_clamp_f32_bits = OpFunction") != 1 ||
		    CountSubstring(text, "OpFunctionCall %uint %lumen_vrsq_clamp_f32_bits") != 1 ||
		    CountSubstring(text, "%lumen_vrsq_compare_product = OpFunction") != 1 ||
		    CountSubstring(text, "OpFunctionCall %uint %lumen_vrsq_compare_product") != 2 ||
		    text.find("OpUMulExtended") == std::string_view::npos ||
		    text.find("OpLoopMerge %lumen_vrsq_subnormal_done") == std::string_view::npos ||
		    text.find("OpLoopMerge %lumen_vrsq_search_done") == std::string_view::npos ||
		    HasDisallowedVRsqClampF32Instruction(text) || text.find("InverseSqrt") != std::string_view::npos ||
		    text.find("FindUMsb") != std::string_view::npos || text.find("OpTypeInt 64") != std::string_view::npos ||
		    text.find("OpCapability Int64") != std::string_view::npos || text.find("SPV_KHR_float_controls") != std::string_view::npos ||
		    text.find("RoundingModeRTE") != std::string_view::npos || text.find("DenormPreserve") != std::string_view::npos ||
		    text.find("DenormFlushToZero") != std::string_view::npos ||
		    text.find("SignedZeroInfNanPreserve") != std::string_view::npos || text.find("NoContraction") != std::string_view::npos)
		{
			std::fprintf(stderr, "V_RSQ_CLAMP_F32 integer helper, alias ordering or EXEC masking changed\n");
			return false;
		}

		ShaderVertexInputInfo input {};
		const auto module = Lumen::Libs::Graphics::ShaderRecompileVS(observed, &input);
		spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
		std::string validation_error;
		validator.SetMessageConsumer(
		    [&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
		    { validation_error = std::to_string(position.index) + ": " + message; });
		if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
		{
			std::fprintf(stderr, "V_RSQ_CLAMP_F32 vertex SPIR-V validation failed: %s\n", validation_error.c_str());
			return false;
		}
	}

	return true;
}

bool CheckVMinU32Pipeline()
{
	constexpr uint32_t waitcnt         = 0xbf8c0000u;
	constexpr uint32_t endpgm          = 0xbf810000u;
	constexpr uint32_t register_program[] = {waitcnt, 0x26040300u, endpgm};
	constexpr uint32_t literal_program[]  = {waitcnt, 0x260402ffu, 0xffffffffu, endpgm};

	ShaderCode code;
	code.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(register_program, &code);
	if (code.GetInstructions().Size() != 3)
	{
		std::fprintf(stderr, "VMinU32 fixture parsed %u instructions instead of 3\n", code.GetInstructions().Size());
		return false;
	}
	const auto& minimum = code.GetInstructions().At(1);
	if (minimum.type != ShaderInstructionType::VMinU32 ||
	    minimum.format != ShaderInstructionFormat::SVdstSVsrc0SVsrc1 || minimum.pc != 4 || minimum.src_num != 2 ||
	    minimum.src[0].type != ShaderOperandType::Vgpr || minimum.src[0].register_id != 0 ||
	    minimum.src[1].type != ShaderOperandType::Vgpr || minimum.src[1].register_id != 1 ||
	    minimum.dst.type != ShaderOperandType::Vgpr || minimum.dst.register_id != 2 ||
	    minimum.dst2.type != ShaderOperandType::Unknown || minimum.src[0].absolute || minimum.src[1].absolute ||
	    minimum.src[0].negate || minimum.src[1].negate || minimum.dst.clamp || minimum.dst.multiplier != 1.0f)
	{
		std::fprintf(stderr, "VMinU32 base VOP2 operand or modifier shape changed\n");
		return false;
	}

	ShaderCode literal;
	literal.SetType(ShaderType::Compute);
	Lumen::Libs::Graphics::ShaderParse(literal_program, &literal);
	if (literal.GetInstructions().Size() != 3 || literal.GetInstructions().At(1).type != ShaderInstructionType::VMinU32 ||
	    literal.GetInstructions().At(1).src[0].type != ShaderOperandType::LiteralConstant ||
	    literal.GetInstructions().At(1).src[0].constant.u != 0xffffffffu || literal.GetInstructions().At(2).pc != 12)
	{
		std::fprintf(stderr, "VMinU32 literal consumption changed instruction boundaries\n");
		return false;
	}

	ShaderComputeInputInfo input {};
	input.threads_num[0]     = 1;
	input.threads_num[1]     = 1;
	input.threads_num[2]     = 1;
	input.thread_ids_num     = 1;
	input.workgroup_register = 16;
	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &input);
	const std::string_view text(source.c_str(), source.Size());
	constexpr std::string_view unsigned_min = "%t_1 = OpExtInst %uint %GLSL_std_450 UMin %t0_1 %t1_1";
	if (CountSubstring(text, unsigned_min) != 1 ||
	    text.find("%t0_1 = OpBitcast %uint %tt0_1") == std::string_view::npos ||
	    text.find("%t1_1 = OpBitcast %uint %tt1_1") == std::string_view::npos ||
	    text.find("%tf_1 = OpBitcast %float %t_1") == std::string_view::npos ||
	    text.find("%tval_1 = OpSelect %float %exec_lo_b_1 %tf_1 %tdst_1") == std::string_view::npos ||
	    text.find("%t_1 = OpExtInst %int %GLSL_std_450 SMin") != std::string_view::npos ||
	    text.find("%t_1 = OpExtInst %float %GLSL_std_450 FMin") != std::string_view::npos)
	{
		std::fprintf(stderr, "VMinU32 SPIR-V lost unsigned lowering, bit preservation or EXEC masking\n");
		return false;
	}

	const auto module = Lumen::Libs::Graphics::ShaderRecompileCS(code, &input);
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "VMinU32 compute SPIR-V validation failed: %s\n", validation_error.c_str());
		return false;
	}

	constexpr auto umin = [](uint32_t lhs, uint32_t rhs) { return lhs < rhs ? lhs : rhs; };
	if (umin(0xffffffffu, 0u) != 0u || umin(0u, 0xffffffffu) != 0u ||
	    umin(0x80000000u, 0x7fffffffu) != 0x7fffffffu ||
	    umin(0x7fffffffu, 0x80000000u) != 0x7fffffffu ||
	    umin(0xffffffffu, 0x80000000u) != 0x80000000u || umin(0xffffffffu, 0xffffffffu) != 0xffffffffu)
	{
		std::fprintf(stderr, "VMinU32 unsigned boundary oracle changed\n");
		return false;
	}

	return true;
}

bool CheckVCvtOffF32I4Pipeline()
{
	using namespace Lumen::Libs::Graphics;
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
		std::fprintf(stderr, "V_CVT_OFF_F32_I4 SPIR-V %zu: %s\n", position.index, message);
	});
	uint32_t modules = 0;
	ShaderInstruction baseline;
	for (const auto stage: {ShaderType::Vertex, ShaderType::Pixel, ShaderType::Compute})
		for (const bool literal: {false, true})
			for (int32_t value = -8; value <= 7; ++value)
			{
				// Original VOP1 fixture: initialize the destination, set EXEC,
				// convert a known whole integer, terminate, then owned padding.
				// Reusing v9 verifies the lowerer's read-before-masked-store path;
				// register source/destination self-alias remains outside the profile.
				uint32_t words[12] {};
				uint32_t count = 0;
				words[count++] = 0x7e1202ffu;
				words[count++] = 0x3fc00000u;
				words[count++] = literal ? 0xbefe0480u : 0xbefe0481u;
				const uint32_t selector = literal ? 255u : value >= 0 ? 128u + static_cast<uint32_t>(value) :
				                                                                      192u + static_cast<uint32_t>(-value);
				words[count++] = 0x7e000000u | (9u << 17u) | (0x0eu << 9u) | selector;
				if (literal) words[count++] = static_cast<uint32_t>(value);
				if (stage == ShaderType::Pixel)
				{
					// Uncompressed RGBA32 export matches target mode 9 below.
					words[count++] = 0xf800180fu;
					words[count++] = 0x09090909u;
				}
				words[count++] = 0xbf810000u;
				ShaderCode code;
				code.SetType(stage);
				code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
				if (ShaderTryParseBounded(words, count + 4, &code) != count ||
				    code.GetInstructions().Size() != (stage == ShaderType::Pixel ? 5u : 4u)) return false;
				const auto& instruction = code.GetInstructions().At(2);
				if (!ShaderCvtOffF32I4InstructionSupported(instruction) || instruction.pc != 12 ||
				    instruction.dst.register_id != 9 || instruction.src[0].constant.u != static_cast<uint32_t>(value) ||
				    instruction.src[0].type != (literal ? ShaderOperandType::LiteralConstant : ShaderOperandType::IntegerInlineConstant) ||
				    code.GetInstructions().At(code.GetInstructions().Size() - 1).pc !=
				        (literal ? 20u : 16u) + (stage == ShaderType::Pixel ? 8u : 0u)) return false;
				if (!literal && value == 2 && words[3] != 0x7e121c82u) return false;
				baseline = instruction;
				ShaderVertexInputInfo vertex {};
				ShaderPixelInputInfo pixel {};
				pixel.target_output_mode[0] = 9;
				pixel.shader_mask = 0xf;
				ShaderComputeInputInfo compute {};
				compute.threads_num[0] = compute.threads_num[1] = compute.threads_num[2] = 1;
				compute.thread_ids_num = 1;
				compute.workgroup_register = 16;
				const auto source = SpirvGenerateSource(code, stage == ShaderType::Vertex ? &vertex : nullptr,
				                                       stage == ShaderType::Pixel ? &pixel : nullptr,
				                                       stage == ShaderType::Compute ? &compute : nullptr);
				const std::string_view text(source.c_str(), source.Size());
				const auto arithmetic = Lumen::Core::String8(Detail::kVCvtOffF32I4SpirvArithmetic)
				                            .ReplaceStr("<input>", "t0_2").ReplaceStr("<result>", "t_2").ReplaceStr("<index>", "2");
				const auto old_destination = text.find("%tdst_2 = OpLoad %float %v9");
				const auto masked_result = text.find("%tval_2 = OpSelect %float %exec_lo_b_2 %t_2 %tdst_2");
				const auto write = text.find("OpStore %v9 %tval_2");
				if (text.find(arithmetic.c_str()) == std::string_view::npos ||
				    text.find("%float_0_062500 = OpConstant %float 0.062500") == std::string_view::npos ||
				    text.find("%exec_lo_b_2 = OpINotEqual %bool %exec_lo_u_2 %uint_0") == std::string_view::npos ||
				    old_destination == std::string_view::npos || masked_result == std::string_view::npos ||
				    write == std::string_view::npos || old_destination >= masked_result || masked_result >= write ||
				    text.find("%cvt_off_i32_2 = OpBitField") != std::string_view::npos) return false;
				const auto module = stage == ShaderType::Vertex ? ShaderRecompileVS(code, &vertex) :
				                    stage == ShaderType::Pixel ? ShaderRecompilePS(code, &pixel) : ShaderRecompileCS(code, &compute);
				if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size())) return false;
				++modules;
			}
	for (int mutation = 0; mutation < 24; ++mutation)
	{
		auto instruction = baseline;
		switch (mutation)
		{
			case 0: instruction.src[0].constant.u = 8; break;
			case 1: instruction.src[0].constant.u = 15; break;
			case 2: instruction.src[0].constant.u = 0xfffffff7u; break;
			case 3: instruction.src[0].constant.u = 0x80000000u; break;
			case 4: instruction.src[0].type = ShaderOperandType::Sgpr; break;
			case 5: instruction.src[0].type = ShaderOperandType::Vgpr; instruction.src[0].register_id = 9; break;
			case 6: instruction.src[0].type = ShaderOperandType::FloatInlineConstant; break;
			case 7: instruction.src[0].negate = true; break;
			case 8: instruction.src[0].absolute = true; break;
			case 9: instruction.src[0].multiplier = 2.0f; break;
			case 10: instruction.src[0].clamp = true; break;
			case 11: instruction.src[0].size = 1; break;
			case 12: instruction.dst.clamp = true; break;
			case 13: instruction.dst.multiplier = 2.0f; break;
			case 14: instruction.dst.negate = true; break;
			case 15: instruction.dst.register_id = 256; break;
			case 16: instruction.dst.type = ShaderOperandType::Sgpr; break;
			case 17: instruction.src_num = 2; break;
			case 18: instruction.src[1].type = ShaderOperandType::IntegerInlineConstant; break;
			case 19: instruction.dst2.type = ShaderOperandType::VccLo; break;
			case 20: instruction.globally_coherent = true; break;
			case 21: instruction.export_target = 0; break;
			case 22: instruction.format = ShaderInstructionFormat::SVdstSVsrc0SVsrc1; break;
			case 23: instruction.type = ShaderInstructionType::VCvtF32I32; break;
		}
		if (ShaderCvtOffF32I4InstructionSupported(instruction)) return false;
	}
	// The bounded decoder must not consume a missing literal or read padding
	// as an operand. Its existing conservative lookahead rejects both tails.
	for (const uint32_t available: {1u, 2u})
	{
		constexpr uint32_t truncated[] = {0x7e121cffu, 0xffffffffu};
		ShaderCode code;
		code.SetType(ShaderType::Compute);
		if (ShaderTryParseBounded(truncated, available, &code) != 0) return false;
	}
	std::fprintf(stderr, "[GFX_VCVT_OFF_F32_I4_TEST] profile=known-whole-integer constants=16 source-forms=2 stages=3 modules=%u "
	                     "exec=preserve-existing-destination raw-placement=unclaimed invalid-ir=24 status=clean\n", modules);
	return true;
}

bool CheckBufferLoadFormatXyzwPipeline()
{
	constexpr uint32_t program[] = {
	    0xbf8c0000u,
	    (0x38u << 26u) | (0x03u << 18u) | (1u << 13u),
	    (128u << 24u) | (1u << 16u) | (1u << 8u),
	    0xbf810000u,
	};

	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	Lumen::Libs::Graphics::ShaderParse(program, &code);
	if (code.GetInstructions().Size() != 3)
	{
		std::fprintf(stderr, "MUBUF format XYZW fixture parsed %u instructions instead of 3\n", code.GetInstructions().Size());
		return false;
	}
	const auto& load = code.GetInstructions().At(1);
	if (load.type != ShaderInstructionType::BufferLoadFormatXyzw ||
	    load.format != ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen || load.pc != 4 || load.dst.register_id != 1 ||
	    load.dst.size != 4 || load.src_num != 3 || load.src[0].register_id != 0 || load.src[1].register_id != 4 ||
	    load.src[1].size != 4 || load.src[2].constant.i != 0)
	{
		std::fprintf(stderr, "MUBUF format XYZW operand shape changed\n");
		return false;
	}

	auto make_resource = [](uint32_t stride, uint32_t dst_sel, uint32_t dfmt, uint32_t nfmt,
	                        uint32_t memory_type = 0u, uint32_t num_records = 3u)
	{
		Lumen::Libs::Graphics::ShaderBufferResource resource;
		resource.fields[1] = (stride << 16u) | ((memory_type & 0x60u) << 7u) | ((memory_type & 0x3u) << 14u);
		resource.fields[2] = num_records;
		resource.fields[3] = dst_sel | (nfmt << 12u) | (dfmt << 15u) | ((memory_type & 0x1cu) << 25u);
		return resource;
	};
	const auto x_uint = make_resource(4, Lumen::Libs::Graphics::DstSel(4, 0, 0, 0), 4, 4);
	const auto x_float = make_resource(4, Lumen::Libs::Graphics::DstSel(4, 0, 0, 1), 4, 7);
	const auto xy_uint = make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 0, 0), 11, 4);
	const auto xy_float = make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 7);
	const auto xy_float_stride8 =
	    make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 7, 0x10, 4);
	const auto unorm8_constant0011 =
	    make_resource(8, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 10, 0, 0x10, 4);
	const uint32_t unorm8_constant0011_key = (unorm8_constant0011.fields[3] >> 12u) & 0x7fu;
	const auto xyzw_float = make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 6, 7), 14, 7);
	const auto xyz_sscaled16 = make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 6, 0), 12, 3, 0x10);
	if (unorm8_constant0011_key != 80u ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferUnorm8x4Constant0011Compatible(unorm8_constant0011) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferUnorm8x4Constant0011Compatible(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 10, 0, 0x10, 1)) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(unorm8_constant0011) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        unorm8_constant0011, Lumen::Libs::Graphics::ShaderStorageUsage::ReadOnly) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        unorm8_constant0011, Lumen::Libs::Graphics::ShaderStorageUsage::Unknown) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        unorm8_constant0011, Lumen::Libs::Graphics::ShaderStorageUsage::Constant) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        unorm8_constant0011, Lumen::Libs::Graphics::ShaderStorageUsage::ReadWrite) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(4, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 10, 0, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(12, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 10, 0, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(16, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 10, 0, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(4, 0, 1, 1), 10, 0, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(0, 0, 1, 0), 10, 0, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 11, 0, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 10, 1, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 10, 0, 0x00)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(0, 0, 1, 1), 10, 0, 0x6d)))
	{
		std::fprintf(stderr, "UNORM8x4 constant-0011 storage profile no longer has an exact read-only boundary\n");
		return false;
	}
	if (!Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(x_uint) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(x_float) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(xy_uint) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xy_uint, Lumen::Libs::Graphics::ShaderStorageUsage::Constant) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xy_uint, Lumen::Libs::Graphics::ShaderStorageUsage::ReadWrite) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(xy_float) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xy_float, Lumen::Libs::Graphics::ShaderStorageUsage::Constant) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xy_float, Lumen::Libs::Graphics::ShaderStorageUsage::ReadWrite) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferXyFloatStride8Compatible(xy_float_stride8) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferXyFloatStride8Compatible(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 7, 0x10, 1)) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(xy_float_stride8) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xy_float_stride8, Lumen::Libs::Graphics::ShaderStorageUsage::ReadOnly) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xy_float_stride8, Lumen::Libs::Graphics::ShaderStorageUsage::Unknown) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xy_float_stride8, Lumen::Libs::Graphics::ShaderStorageUsage::Constant) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xy_float_stride8, Lumen::Libs::Graphics::ShaderStorageUsage::ReadWrite) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(xyzw_float) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferSscaled16XyzCompatible(xyz_sscaled16) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(xyz_sscaled16) ||
	    !Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xyz_sscaled16, Lumen::Libs::Graphics::ShaderStorageUsage::ReadOnly) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xyz_sscaled16, Lumen::Libs::Graphics::ShaderStorageUsage::Constant) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatUsageSupported(
	        xyz_sscaled16, Lumen::Libs::Graphics::ShaderStorageUsage::ReadWrite) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 7)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(4, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 7, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(12, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 7, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 0, 0), 11, 7, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 14, 7, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 4, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 7, 0x6d)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 0, 0), 11, 7)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 14, 7)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 0, 1), 11, 4)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(8, Lumen::Libs::Graphics::DstSel(4, 5, 6, 0), 12, 3, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 6, 1), 12, 3, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 6, 0), 12, 2, 0x10)) ||
	    Lumen::Libs::Graphics::ShaderStorageBufferFormatSupported(
	        make_resource(16, Lumen::Libs::Graphics::DstSel(4, 5, 6, 0), 12, 3, 0x00)))
	{
		std::fprintf(stderr, "storage-buffer format whitelist no longer isolates its exact admitted profiles\n");
		return false;
	}

	const auto stride8_word_index = [&xy_float_stride8](uint32_t record, uint32_t component)
	{
		return (record * xy_float_stride8.Stride() + component * 4u) / 4u;
	};
	for (uint32_t record = 0; record < xy_float_stride8.NumRecords(); ++record)
	{
		for (uint32_t component = 0; component < 2; ++component)
		{
			if (stride8_word_index(record, component) != record * 2u + component)
			{
				std::fprintf(stderr, "stride-8 XY float addressing no longer maps two dwords per record\n");
				return false;
			}
		}
	}
	const uint64_t stride8_size =
	    static_cast<uint64_t>(xy_float_stride8.Stride()) * xy_float_stride8.NumRecords();
	const uint64_t stride8_last_end =
	    static_cast<uint64_t>(stride8_word_index(xy_float_stride8.NumRecords() - 1u, 1u) + 1u) * 4u;
	if (stride8_size != 32u || stride8_last_end != stride8_size)
	{
		std::fprintf(stderr, "stride-8 XY float final dword no longer ends at the 32-byte allocation boundary\n");
		return false;
	}

	ShaderVertexInputInfo input {};
	input.bind.storage_buffers.buffers_num       = 1;
	input.bind.storage_buffers.buffers[0]         = unorm8_constant0011;
	input.bind.storage_buffers.usages[0]          = Lumen::Libs::Graphics::ShaderStorageUsage::ReadOnly;
	input.bind.storage_buffers.slots[0]           = 0;
	input.bind.storage_buffers.start_register[0]  = 4;
	input.bind.storage_buffers.extended[0]        = false;
	input.bind.descriptor_set_slot                = 0;
	Lumen::Libs::Graphics::ShaderCalcBindingIndices(&input.bind);

	const auto             source = Lumen::Libs::Graphics::SpirvGenerateSource(code, &input, nullptr, nullptr);
	const std::string_view text(source.c_str(), source.Size());
	if (CountSubstring(text, "%buffer_load_float2 = OpFunction") != 1 ||
	    CountSubstring(text, "%buffer_load_sscaled16_xyzw = OpFunction") != 1 ||
	    text.find("OpSwitch %mubuf_fxyzw_format_1 %mubuf_fxyzw_merge_1 36") == std::string_view::npos ||
	    CountSubstring(text, "80 %mubuf_fxyzw_unorm8_constant_0011_1") != 1 ||
	    text.find("%mubuf_fxyzw_unorm8_constant_0011_1 = OpLabel\n"
	              "               OpStore %v1 %float_0_000000\n"
	              "               OpStore %v2 %float_0_000000\n"
	              "               OpStore %v3 %float_1_000000\n"
	              "               OpStore %v4 %float_1_000000\n"
	              "               OpBranch %mubuf_fxyzw_merge_1") == std::string_view::npos ||
	    text.find("95 %mubuf_fxyzw_xy_float_1") == std::string_view::npos ||
	    text.find("99 %mubuf_fxyzw_xyz_sscaled16_1") == std::string_view::npos ||
	    text.find("119 %mubuf_fxyzw_xyzw_float_1") == std::string_view::npos ||
	    text.find("%mubuf_fxyzw_load_xy_float_1 = OpFunctionCall %void %buffer_load_float2 %v1 %v2") ==
	        std::string_view::npos ||
	    text.find("%mubuf_fxyzw_load_xy_float_1 = OpFunctionCall %void %buffer_load_float2 %v1 %v2 %temp_int_1 "
	              "%temp_int_2 %temp_int_3 %temp_int_4\n"
	              "               OpStore %v3 %float_0_000000\n"
	              "               OpStore %v4 %float_1_000000") == std::string_view::npos ||
	    text.find("%mubuf_fxyzw_stride_word_1 = OpLoad %uint %s5") == std::string_view::npos ||
	    text.find("%mubuf_fxyzw_stride_shifted_1 = OpShiftRightLogical %uint %mubuf_fxyzw_stride_word_1 %int_16") ==
	        std::string_view::npos ||
	    text.find("%mubuf_fxyzw_stride_bits_1 = OpBitwiseAnd %uint %mubuf_fxyzw_stride_shifted_1 %uint_0x00003fff") ==
	        std::string_view::npos ||
	    text.find("OpStore %temp_int_3 %mubuf_fxyzw_stride_1") == std::string_view::npos ||
	    text.find("%buf_l_f2_32 = OpIMul %int %buf_l_f2_30 %buf_l_f2_31") == std::string_view::npos ||
	    text.find("%buf_l_f2_33 = OpIAdd %int %buf_l_f2_29 %buf_l_f2_32") == std::string_view::npos ||
	    text.find("%buf_l_f2_34 = OpSDiv %int %buf_l_f2_33 %int_4") == std::string_view::npos ||
	    text.find("%buf_l_f2_36 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f2_35 %int_0 %buf_l_f2_34") ==
	        std::string_view::npos ||
	    text.find("%buf_l_f2_38 = OpIAdd %int %buf_l_f2_34 %int_1") == std::string_view::npos ||
	    text.find("%buf_l_f2_39 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f2_35 %int_0 %buf_l_f2_38") ==
	        std::string_view::npos ||
	    text.find("%mubuf_fxyzw_load_xyz_sscaled16_1 = OpFunctionCall %void %buffer_load_sscaled16_xyzw %v1 %v2 %v3 %v4") ==
	        std::string_view::npos ||
	    text.find("%buf_l_s16_xyzw_x = OpBitFieldSExtract %int %buf_l_s16_xyzw_word_0 %int_0 %int_16") ==
	        std::string_view::npos ||
	    text.find("%buf_l_s16_xyzw_z_float = OpConvertSToF %float %buf_l_s16_xyzw_z") ==
	        std::string_view::npos ||
	    text.find("OpStore %v3 %float_0_000000") == std::string_view::npos ||
	    text.find("OpStore %v4 %float_1_000000") == std::string_view::npos ||
	    text.find("%v4 %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4\n               OpStore %v4 %float_0_000000") ==
	        std::string_view::npos ||
	    text.find("%mubuf_fxyzw_format_word_1 = OpLoad %uint %s7") == std::string_view::npos)
	{
		std::fprintf(stderr, "MUBUF format XYZW SPIR-V lost dynamic selection, SSCALED conversion, or DstSel materialization\n");
		return false;
	}
	const auto unorm8_constant_start = text.find("%mubuf_fxyzw_unorm8_constant_0011_1 = OpLabel");
	const auto unorm8_constant_end =
	    unorm8_constant_start == std::string_view::npos
	        ? std::string_view::npos
	        : text.find("OpBranch %mubuf_fxyzw_merge_1", unorm8_constant_start);
	if (unorm8_constant_end == std::string_view::npos ||
	    text.substr(unorm8_constant_start, unorm8_constant_end - unorm8_constant_start).find("OpFunctionCall") !=
	        std::string_view::npos ||
	    text.substr(unorm8_constant_start, unorm8_constant_end - unorm8_constant_start).find("OpAccessChain") !=
	        std::string_view::npos ||
	    text.substr(unorm8_constant_start, unorm8_constant_end - unorm8_constant_start).find("OpLoad") !=
	        std::string_view::npos)
	{
		std::fprintf(stderr, "UNORM8x4 constant-0011 lowering unexpectedly reads storage data\n");
		return false;
	}

	const auto module = Lumen::Libs::Graphics::ShaderRecompileVS(code, &input);
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	std::string          validation_error;
	validator.SetMessageConsumer([&validation_error](spv_message_level_t, const char*, const spv_position_t& position, const char* message)
	                             { validation_error = std::to_string(position.index) + ": " + message; });
	if (module.Size() < 5 || module.At(0) != 0x07230203u || !validator.Validate(module.GetDataConst(), module.Size()))
	{
		std::fprintf(stderr, "MUBUF format XYZW SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}

	constexpr uint32_t x_program[] = {
	    0xbf8c0000u,
	    (0x38u << 26u) | (1u << 13u),
	    (128u << 24u) | (1u << 16u) | (1u << 8u),
	    0xbf810000u,
	};
	ShaderCode x_code;
	x_code.SetType(ShaderType::Vertex);
	Lumen::Libs::Graphics::ShaderParse(x_program, &x_code);
	if (x_code.GetInstructions().Size() != 3 || x_code.GetInstructions().At(1).type != ShaderInstructionType::BufferLoadFormatX)
	{
		std::fprintf(stderr, "MUBUF format X fixture no longer parses as the admitted single-component load\n");
		return false;
	}
	const auto             x_source = Lumen::Libs::Graphics::SpirvGenerateSource(x_code, &input, nullptr, nullptr);
	const std::string_view x_text(x_source.c_str(), x_source.Size());
	const auto tbuffer_x_switch_start =
	    x_text.find("OpSwitch %tbuf_l_f_x_76 %tbuf_l_f_x_81");
	const auto tbuffer_x_switch_end =
	    tbuffer_x_switch_start == std::string_view::npos
	        ? std::string_view::npos
	        : x_text.find('\n', tbuffer_x_switch_start);
	if (x_text.find("OpSwitch %tbuf_l_f_x_76 %tbuf_l_f_x_81 36 %tbuf_l_f_x_raw") == std::string_view::npos ||
	    x_text.find("119 %tbuf_l_f_x_raw 99 %tbuf_l_f_x_sscaled16") == std::string_view::npos ||
	    tbuffer_x_switch_end == std::string_view::npos ||
	    x_text.substr(tbuffer_x_switch_start, tbuffer_x_switch_end - tbuffer_x_switch_start).find(" 80 ") !=
	        std::string_view::npos ||
	    CountSubstring(x_text, "80 %mubuf_f_x_constant_0_1") != 1 ||
	    x_text.find("OpSwitch %t211_1 %mubuf_f_x_load_1 80 %mubuf_f_x_constant_0_1\n"
	                "        %mubuf_f_x_constant_0_1 = OpLabel\n"
	                "               OpStore %v1 %float_0_000000\n"
	                "               OpBranch %mubuf_f_x_merge_1\n"
	                "        %mubuf_f_x_load_1 = OpLabel\n"
	                "        %t110_1 = OpFunctionCall %void %tbuffer_load_format_x") == std::string_view::npos ||
	    x_text.find("%tbuf_l_f_x_93 = OpFunctionCall %void %buffer_load_sscaled16_x") == std::string_view::npos ||
	    x_text.find("%buf_l_s16_x_value = OpBitFieldSExtract %int %buf_l_s16_x_word %int_0 %int_16") ==
	        std::string_view::npos ||
	    x_text.find("%buf_l_s16_x_float = OpConvertSToF %float %buf_l_s16_x_value") == std::string_view::npos ||
	    x_text.find("%t211_1 = OpBitcast %int %t210_1") == std::string_view::npos)
	{
		std::fprintf(stderr, "MUBUF format X lowering can silently skip an admitted multi-component descriptor\n");
		return false;
	}
	validation_error.clear();
	const auto x_module = Lumen::Libs::Graphics::ShaderRecompileVS(x_code, &input);
	if (x_module.Size() < 5 || x_module.At(0) != 0x07230203u || !validator.Validate(x_module.GetDataConst(), x_module.Size()))
	{
		std::fprintf(stderr, "MUBUF format X SPIR-V module validation failed: %s\n", validation_error.c_str());
		return false;
	}

	return true;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
template <typename Action>
bool ExpectGraphicsRejection(Action action, std::string_view expected_diagnostic)
{
	int output_pipe[2] = {-1, -1};
	if (pipe(output_pipe) != 0)
	{
		std::perror("graphics rejection pipe");
		return false;
	}

	const pid_t child = fork();
	if (child == -1)
	{
		std::perror("graphics rejection fork");
		close(output_pipe[0]);
		close(output_pipe[1]);
		return false;
	}
	if (child == 0)
	{
		close(output_pipe[0]);
		if (dup2(output_pipe[1], STDOUT_FILENO) == -1 || dup2(output_pipe[1], STDERR_FILENO) == -1)
		{
			std::_Exit(111);
		}
		close(output_pipe[1]);

		action();
		std::_Exit(0);
	}

	close(output_pipe[1]);
	std::string diagnostic;
	char        buffer[4096];
	for (;;)
	{
		const ssize_t length = read(output_pipe[0], buffer, sizeof(buffer));
		if (length > 0)
		{
			diagnostic.append(buffer, static_cast<size_t>(length));
			continue;
		}
		if (length == -1 && errno == EINTR)
		{
			continue;
		}
		break;
	}
	close(output_pipe[0]);

	int status = 0;
	while (waitpid(child, &status, 0) == -1)
	{
		if (errno != EINTR)
		{
			std::perror("graphics rejection waitpid");
			return false;
		}
	}
	constexpr int expected_exit = 321 & 0xff;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != expected_exit || diagnostic.find(expected_diagnostic) == std::string::npos)
	{
		std::fprintf(stderr, "graphics did not fail closed as expected (status=0x%x):\n%s\n", status, diagnostic.c_str());
		return false;
	}
	return true;
}

bool ExpectShaderParserRejection(const uint32_t* program, ShaderType type, bool attested, std::string_view expected_diagnostic,
                                 bool dx10_disabled = false)
{
	return ExpectGraphicsRejection([&]()
	{
		ShaderCode code;
		code.SetType(type);
		if (attested) code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
		if (dx10_disabled) code.SetDx10ClampMode(ShaderDx10ClampMode::Disabled);
		Lumen::Libs::Graphics::ShaderParse(program, &code);
	}, expected_diagnostic);
}

bool CheckComparisonImageParserRejections()
{
	// Every comparison sample/gather opcode remains terminal, including derivative and offset forms.
	for (const uint32_t opcode: {0x28u, 0x29u, 0x2au, 0x2bu, 0x2cu, 0x2du, 0x2eu, 0x2fu,
	                            0x38u, 0x39u, 0x3au, 0x3bu, 0x3cu, 0x3du, 0x3eu, 0x3fu,
	                            0x48u, 0x49u, 0x4cu, 0x4du, 0x4eu, 0x4fu,
	                            0x58u, 0x59u, 0x5cu, 0x5du, 0x5eu, 0x5fu,
	                            0x6au, 0x6bu, 0x6eu, 0x6fu})
	{
		const uint32_t fixture[] = {MimgWord(opcode, false, false, 0xfu), MimgOperands(2, 0, 0, 4), 0xbf810000u};
		const auto diagnostic = opcode >= 0x48u && opcode <= 0x5fu ? "unknown mimg instruction image_gather4_c" :
		                                                                     "unknown mimg instruction image_sample_c";
		if (!ExpectShaderParserRejection(fixture, ShaderType::Pixel, false, diagnostic)) return false;
	}
	std::fprintf(stderr, "[GFX_COMPARISON_IMAGE_PARSER_TEST] comparison-opcodes=32 admission=reject\n");
	return true;
}

bool CheckParameterExportParserRejections()
{
	using namespace Lumen::Libs::Graphics;
	for (const uint32_t changed: {0x800u, 0x400u, 0x1000u, 0x2000u})
	{
		const uint32_t fixture[] = {0xf800026fu | changed, 0x03020100u, 0xbf810000u};
		if (!ExpectShaderParserRejection(fixture, ShaderType::Vertex, false, "unknown exp target: 0x26")) return false;
	}
	const uint32_t partial[] = {0xf8000267u, 0x03020100u, 0xbf810000u};
	const uint32_t wrong_target[] = {0xf80001ffu, 0x03020100u, 0xbf810000u};
	const uint32_t parameter[] = {0xf800026fu, 0x03020100u, 0xbf810000u};
	if (!ExpectShaderParserRejection(partial, ShaderType::Vertex, false, "unknown exp target: 0x26") ||
	    !ExpectShaderParserRejection(wrong_target, ShaderType::Vertex, false, "unknown exp target: 0x1f") ||
	    !ExpectShaderParserRejection(parameter, ShaderType::Pixel, false, "unknown exp target: 0x26") ||
	    !ExpectShaderParserRejection(parameter, ShaderType::Compute, false, "unknown exp target: 0x26")) return false;
	for (const uint32_t count: {33u, UINT32_MAX})
		if (!ExpectGraphicsRejection([&]() {
			HW::PixelShaderInfo regs;
			HW::ShaderRegisters registers;
			registers.ps_in_control = count;
			ShaderVertexInputInfo vertex;
			ShaderPixelInputInfo pixel;
			ShaderGetInputInfoPS(&regs, &registers, &vertex, &pixel);
		}, "ps_info->input_num > 32")) return false;
	const uint32_t wrong_attribute[] = {0xc801fc00u, 0xf800000fu, 0x03020100u, 0xbf810000u};
	if (!ExpectGraphicsRejection([&]() {
		ShaderCode code;
		code.SetType(ShaderType::Pixel);
		ShaderParse(wrong_attribute, &code);
		ShaderPixelInputInfo pixel;
		pixel.input_num = 1; pixel.target_output_mode[0] = 9;
		(void)SpirvGenerateSource(code, nullptr, &pixel, nullptr);
	}, "unsupported pixel interpolator input index or channel")) return false;
	for (uint32_t mutation = 0; mutation < 3; ++mutation)
	{
		const char* diagnostic = mutation == 0 ? "produced != vs_input_info->parameter_export_mask" :
		                         mutation == 1 ? "host_parameter_interface_valid" : "host_vertex_output_components";
		if (!ExpectGraphicsRejection([&]() {
			const uint32_t fixture[] = {0xf800026fu, 0x03020100u, 0xf80008cfu, 0x03020100u, 0xbf810000u};
			ShaderCode code;
			code.SetType(ShaderType::Vertex);
			ShaderParse(fixture, &code);
			ShaderVertexInputInfo vertex;
			vertex.export_count = 7;
			vertex.parameter_exports_valid = ShaderVertexParameterExportsSupported(code, vertex, nullptr, nullptr,
			                                                                      &vertex.parameter_export_mask);
			vertex.host_parameter_interface_valid = true;
			vertex.host_vertex_output_components = 35;
			if (mutation == 0) code.GetInstructions()[0].export_target = 0x25; // New code remains valid; captured mask is stale.
			if (mutation == 1) vertex.host_parameter_interface_valid = false;
			if (mutation == 2) vertex.host_vertex_output_components = 34;
			(void)SpirvGenerateSource(code, &vertex, nullptr, nullptr);
		}, diagnostic)) return false;
	}
	std::fprintf(stderr, "[GFX_PARAMETER_EXPORT_REJECTION_TEST] flags-stage-range=reject ps-count-overflow=reject interpolation-index=reject\n");
	return true;
}

bool CheckScanModeDrawRejections()
{
	using namespace Lumen::Libs::Graphics;
	// Exercise both real entry points without a GPU. A missing or late guard
	// would fail on a null resource instead of the requested register diagnostic.
	for (const uint32_t unsupported: {0x00010000u, 0x08000000u, 0x80000000u})
	{
		for (const bool indexed: {false, true})
		{
			if (!ExpectGraphicsRejection([&]()
			{
				HW::Context ctx;
				HW::UserConfig ucfg;
				ctx.SetScanModeControl1(0x06020000u | unsupported);
				if (indexed)
					GraphicsRenderDrawIndex(0, nullptr, &ctx, &ucfg, nullptr, 0, 3, nullptr, 0, 0);
				else
					GraphicsRenderDrawIndexAuto(0, nullptr, &ctx, &ucfg, nullptr, 3, 0);
			}, "unsupported PA_SC_MODE_CNTL_1 for draw:")) return false;
		}
	}
	return true;
}

bool CheckAaMaskDrawRejections()
{
	using namespace Lumen::Libs::Graphics;
	for (uint32_t index = 0; index < 2; ++index)
	{
		for (const uint32_t mask: {0u, 0xfffffffeu, 0xfffeffffu, 0x00010001u})
		{
			for (const bool indexed: {false, true})
			{
				if (!ExpectGraphicsRejection([&]()
				{
					HW::Context ctx;
					HW::UserConfig ucfg;
					ctx.SetAaMask(index, mask);
					if (indexed)
						GraphicsRenderDrawIndex(0, nullptr, &ctx, &ucfg, nullptr, 0, 3, nullptr, 0, 0);
					else
						GraphicsRenderDrawIndexAuto(0, nullptr, &ctx, &ucfg, nullptr, 3, 0);
				}, "unsupported PA_SC_AA_MASK for draw:")) return false;
			}
		}
	}
	return true;
}

bool CheckVMulLegacyF32ParserRejections()
{
	constexpr uint32_t         waitcnt            = 0xbf8c0000u;
	constexpr uint32_t         endpgm             = 0xbf810000u;
	constexpr uint32_t         return_from_fetch  = 0xbe802000u;
	constexpr uint32_t         vop2_neutral[]     = {waitcnt, 0x0e040300u, endpgm};
	constexpr uint32_t         fetch_vop2_neutral[] = {waitcnt, 0x0e040300u, return_from_fetch};
	constexpr uint32_t         vop2_sdwa[]        = {waitcnt, 0x0e0402f9u, 0u, endpgm};
	constexpr uint32_t         vop2_dpp[]         = {waitcnt, 0x0e0402fau, endpgm};
	constexpr uint32_t         vop3_neutral[]     = {waitcnt, 0xd20e0002u, 0x00020300u, endpgm};
	constexpr uint32_t         vop3_absolute[]    = {waitcnt, 0xd20e0102u, 0x00020300u, endpgm};
	constexpr uint32_t         vop3_unused_src2[] = {waitcnt, 0xd20e0002u, 0x00060300u, endpgm};
	constexpr std::string_view vop2_diagnostic    = "unknown vop2 instruction v_mul_legacy_f32 (unsupported form or FP mode)";
	constexpr std::string_view vop3_diagnostic    = "unknown vop3 instruction v_mul_legacy_f32 (unsupported form or FP mode)";

	return ExpectShaderParserRejection(vop2_neutral, ShaderType::Vertex, false, vop2_diagnostic) &&
	       ExpectShaderParserRejection(vop2_neutral, ShaderType::Compute, true, vop2_diagnostic) &&
	       ExpectShaderParserRejection(fetch_vop2_neutral, ShaderType::Fetch, true, vop2_diagnostic) &&
	       ExpectShaderParserRejection(vop2_sdwa, ShaderType::Vertex, true, vop2_diagnostic) &&
	       ExpectShaderParserRejection(vop2_dpp, ShaderType::Vertex, true, vop2_diagnostic) &&
	       ExpectShaderParserRejection(vop3_neutral, ShaderType::Pixel, false, vop3_diagnostic) &&
	       ExpectShaderParserRejection(vop3_neutral, ShaderType::Compute, true, vop3_diagnostic) &&
	       ExpectShaderParserRejection(vop3_absolute, ShaderType::Pixel, true, vop3_diagnostic) &&
	       ExpectShaderParserRejection(vop3_unused_src2, ShaderType::Pixel, true, vop3_diagnostic);
}

bool CheckVMacLegacyF32ParserRejections()
{
	if (!VMacLegacyF32CandidateAdmitted())
	{
		return true;
	}

	constexpr uint32_t         waitcnt              = 0xbf8c0000u;
	constexpr uint32_t         endpgm               = 0xbf810000u;
	constexpr uint32_t         return_from_fetch    = 0xbe802000u;
	constexpr uint32_t         vop2_neutral[]       = {waitcnt, 0x0c04170bu, endpgm};
	constexpr uint32_t         fetch_vop2_neutral[] = {waitcnt, 0x0c04170bu, return_from_fetch};
	constexpr uint32_t         vop2_scalar[]        = {waitcnt, 0x0c041600u, endpgm};
	constexpr uint32_t         vop2_literal[]       = {waitcnt, 0x0c0416ffu, 0x3f800000u, endpgm};
	constexpr uint32_t         vop2_sdwa[]          = {waitcnt, 0x0c0416f9u, 0u, endpgm};
	constexpr uint32_t         vop2_dpp[]           = {waitcnt, 0x0c0416fau, endpgm};
	constexpr uint32_t         vop3_neutral[]       = {waitcnt, 0xd20c0002u, 0x00020300u, endpgm};
	constexpr std::string_view vop2_diagnostic      = "unknown vop2 instruction v_mac_legacy_f32 (unsupported form or state)";
	constexpr std::string_view vop3_diagnostic      = "unknown vop3 instruction v_mac_legacy_f32";

	return ExpectShaderParserRejection(vop3_neutral, ShaderType::Vertex, true, vop3_diagnostic, true) &&
	       ExpectShaderParserRejection(vop2_neutral, ShaderType::Pixel, true, vop2_diagnostic, true) &&
	       ExpectShaderParserRejection(vop2_neutral, ShaderType::Compute, true, vop2_diagnostic, true) &&
	       ExpectShaderParserRejection(fetch_vop2_neutral, ShaderType::Fetch, true, vop2_diagnostic, true) &&
	       ExpectShaderParserRejection(vop2_scalar, ShaderType::Vertex, true, vop2_diagnostic, true) &&
	       ExpectShaderParserRejection(vop2_literal, ShaderType::Vertex, true, vop2_diagnostic, true) &&
	       ExpectShaderParserRejection(vop2_sdwa, ShaderType::Vertex, true, vop2_diagnostic, true) &&
	       ExpectShaderParserRejection(vop2_dpp, ShaderType::Vertex, true, vop2_diagnostic, true) &&
	       ExpectShaderParserRejection(vop2_neutral, ShaderType::Vertex, false, vop2_diagnostic, true) &&
	       ExpectShaderParserRejection(vop2_neutral, ShaderType::Vertex, true, vop2_diagnostic);
}

bool CheckVRsqClampF32ParserRejections()
{
	if (!VRsqClampF32CandidateAdmitted())
	{
		return true;
	}

	constexpr uint32_t         waitcnt              = 0xbf8c0000u;
	constexpr uint32_t         endpgm               = 0xbf810000u;
	constexpr uint32_t         return_from_fetch    = 0xbe802000u;
	constexpr uint32_t         vop1_neutral[]       = {waitcnt, 0x7e045902u, endpgm};
	constexpr uint32_t         fetch_vop1_neutral[] = {waitcnt, 0x7e045902u, return_from_fetch};
	constexpr uint32_t         vop1_scalar[]        = {waitcnt, 0x7e045800u, endpgm};
	constexpr uint32_t         vop1_inline[]        = {waitcnt, 0x7e045880u, endpgm};
	constexpr uint32_t         vop1_literal[]       = {waitcnt, 0x7e0458ffu, 0x3f800000u, endpgm};
	constexpr uint32_t         vop1_special[]       = {waitcnt, 0x7e04587eu, endpgm};
	constexpr uint32_t         vop1_sdwa[]          = {waitcnt, 0x7e0458f9u, 0u, endpgm};
	constexpr uint32_t         vop1_dpp[]           = {waitcnt, 0x7e0458fau, endpgm};
	constexpr uint32_t         vop3_neutral[]       = {waitcnt, 0xd3580002u, 0x00000102u, endpgm};
	constexpr uint32_t         vop3_absolute[]      = {waitcnt, 0xd3580102u, 0x00000102u, endpgm};
	constexpr uint32_t         vop3_clamp[]         = {waitcnt, 0xd3580802u, 0x00000102u, endpgm};
	constexpr uint32_t         vop3_negate[]        = {waitcnt, 0xd3580002u, 0x20000102u, endpgm};
	constexpr uint32_t         vop3_omod[]          = {waitcnt, 0xd3580002u, 0x08000102u, endpgm};
	constexpr std::string_view vop1_diagnostic = "unknown vop1 instruction v_rsq_clamp_f32 (unsupported form or state)";
	constexpr std::string_view vop3_diagnostic = "unknown vop3 instruction v_rsq_clamp_f32";

	return ExpectShaderParserRejection(vop3_neutral, ShaderType::Vertex, true, vop3_diagnostic, true) &&
	       ExpectShaderParserRejection(vop3_absolute, ShaderType::Vertex, true, vop3_diagnostic, true) &&
	       ExpectShaderParserRejection(vop3_clamp, ShaderType::Vertex, true, vop3_diagnostic, true) &&
	       ExpectShaderParserRejection(vop3_negate, ShaderType::Vertex, true, vop3_diagnostic, true) &&
	       ExpectShaderParserRejection(vop3_omod, ShaderType::Vertex, true, vop3_diagnostic, true) &&
	       ExpectShaderParserRejection(vop1_neutral, ShaderType::Pixel, true, vop1_diagnostic, true) &&
	       ExpectShaderParserRejection(vop1_neutral, ShaderType::Compute, true, vop1_diagnostic, true) &&
	       ExpectShaderParserRejection(fetch_vop1_neutral, ShaderType::Fetch, true, vop1_diagnostic, true) &&
	       ExpectShaderParserRejection(vop1_scalar, ShaderType::Vertex, true, vop1_diagnostic, true) &&
	       ExpectShaderParserRejection(vop1_inline, ShaderType::Vertex, true, vop1_diagnostic, true) &&
	       ExpectShaderParserRejection(vop1_literal, ShaderType::Vertex, true, vop1_diagnostic, true) &&
	       ExpectShaderParserRejection(vop1_special, ShaderType::Vertex, true, vop1_diagnostic, true) &&
	       ExpectShaderParserRejection(vop1_sdwa, ShaderType::Vertex, true, vop1_diagnostic, true) &&
	       ExpectShaderParserRejection(vop1_dpp, ShaderType::Vertex, true, vop1_diagnostic, true);
}

bool CheckUnknownShaderInstructionDiagnostic()
{
	constexpr uint32_t v_mac_legacy_program[] = {0xbf8c0000u, 0x0c040300u, 0xbf810000u};
	constexpr uint32_t v_rsq_clamp_program[]  = {0xbf8c0000u, 0x7e045902u, 0xbf810000u};
	constexpr uint32_t unknown_vop2_program[]  = {0xbf8c0000u, 0x7a040300u, 0xbf810000u};
	if (!VMacLegacyF32CandidateAdmitted() &&
	    !ExpectShaderParserRejection(v_mac_legacy_program, ShaderType::Vertex, true,
	                                 "unknown vop2 instruction v_mac_legacy_f32, opcode = 0x6, word0 = 0x0c040300 at addr 0x00000004"))
	{
		return false;
	}
	if (!VRsqClampF32CandidateAdmitted() &&
	    !ExpectShaderParserRejection(v_rsq_clamp_program, ShaderType::Vertex, true,
	                                 "unknown vop1 instruction v_rsq_clamp_f32, opcode = 0x2c, word0 = 0x7e045902 at addr 0x00000004",
	                                 true))
	{
		return false;
	}
	return ExpectShaderParserRejection(unknown_vop2_program, ShaderType::Vertex, true,
	                                   "unknown vop2 opcode: 0x3d, word0 = 0x7a040300 at addr 0x00000004");
}

bool CheckVMinU32ParserRejections()
{
	constexpr uint32_t waitcnt        = 0xbf8c0000u;
	constexpr uint32_t endpgm         = 0xbf810000u;
	constexpr uint32_t vop2_sdwa[]    = {waitcnt, 0x260402f9u, 0u, endpgm};
	constexpr uint32_t vop2_dpp[]     = {waitcnt, 0x260402fau, endpgm};
	constexpr uint32_t vop3[]         = {waitcnt, 0xd2260002u, 0x00020300u, endpgm};
	constexpr uint32_t vop2_min_i32[] = {waitcnt, 0x22040300u, endpgm};
	constexpr uint32_t vop2_max_i32[] = {waitcnt, 0x24040300u, endpgm};
	constexpr uint32_t vop2_max_u32[] = {waitcnt, 0x28040300u, endpgm};

	return ExpectShaderParserRejection(vop2_sdwa, ShaderType::Compute, false,
	                                   "unknown vop2 instruction v_min_u32 (unsupported form)") &&
	       ExpectShaderParserRejection(vop2_dpp, ShaderType::Compute, false,
	                                   "unknown vop2 instruction v_min_u32 (unsupported form)") &&
	       ExpectShaderParserRejection(vop3, ShaderType::Compute, false, "unknown vop3 instruction v_min_u32") &&
	       ExpectShaderParserRejection(vop2_min_i32, ShaderType::Compute, false, "unknown vop2 instruction v_min_i32") &&
	       ExpectShaderParserRejection(vop2_max_i32, ShaderType::Compute, false, "unknown vop2 instruction v_max_i32") &&
	       ExpectShaderParserRejection(vop2_max_u32, ShaderType::Compute, false, "unknown vop2 instruction v_max_u32");
}

bool CheckVCvtOffF32I4ParserRejections()
{
	constexpr std::string_view diagnostic = "unknown vop1 instruction v_cvt_off_f32_i4 (known integer constant profile)";
	uint32_t rejected = 0;
	for (const uint32_t selector: {0u, 103u, 126u, 136u, 143u, 192u, 201u, 208u, 240u, 247u, 249u, 250u, 256u, 265u, 511u})
	{
		// Includes VGPR9 -> VGPR9 self-alias; a register's possible runtime
		// value cannot establish this known-constant profile.
		const uint32_t words[] = {0x7e000000u | (9u << 17u) | (0x0eu << 9u) | selector, 0u, 0xbf810000u};
		if (!ExpectShaderParserRejection(words, ShaderType::Compute, false, diagnostic)) return false;
		++rejected;
	}
	for (const uint32_t value: {8u, 15u, 0xfffffff7u, 0x80000000u, 0x7fffffffu, 0x3f800000u, 0x7fc00000u})
	{
		const uint32_t words[] = {0x7e121cffu, value, 0xbf810000u};
		if (!ExpectShaderParserRejection(words, ShaderType::Pixel, false, diagnostic)) return false;
		++rejected;
	}
	for (const uint32_t modifier: {0u, 0x100u, 0x800u})
	{
		const uint32_t words[] = {0xd0000000u | (0x18eu << 17u) | 9u | modifier, 130u, 0xbf810000u};
		if (!ExpectShaderParserRejection(words, ShaderType::Vertex, false, "unknown vop3 instruction v_cvt_off_f32_i4")) return false;
		++rejected;
	}
	// The lowerer independently rejects poisoned/cached IR, even when no
	// vertex resource-profile validator is required by a particular input.
	if (!ExpectGraphicsRejection([]() {
		const uint32_t words[] = {0x7e121c82u, 0xbf810000u};
		ShaderCode code;
		code.SetType(ShaderType::Compute);
		Lumen::Libs::Graphics::ShaderParse(words, &code);
		code.GetInstructions()[0].src[0].negate = true;
		ShaderComputeInputInfo input {};
		input.threads_num[0] = input.threads_num[1] = input.threads_num[2] = 1;
		input.thread_ids_num = 1;
		(void)Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, nullptr, &input);
	}, "!ShaderCvtOffF32I4InstructionSupported")) return false;
	std::fprintf(stderr, "[GFX_VCVT_OFF_F32_I4_REJECTION_TEST] parser-cases=%u registers=float-inline=wide-literal=reject "
	                     "vop3=reject cached-ir=reject status=clean\n", rejected);
	return true;
}

bool CheckImageSampleSingleComponentParserRejections(uint32_t mask)
{
	for (const uint32_t opcode: {0x20u, 0x27u})
	{
		const uint32_t words[] = {MimgWord(opcode, false, false, opcode == 0x20u ? 0u : mask),
		                          MimgOperands(2, 0, 4, 0), 0xbf810000u};
		if (!ExpectShaderParserRejection(words, ShaderType::Pixel, false, "unknown mimg format for opcode")) return false;
	}
	struct Flag { uint32_t bit; const char* diagnostic; };
	constexpr Flag flags[] = {{25, "slc == 1"}, {17, "lwe == 1"}, {16, "tff == 1"}, {15, "r128 == 1"},
	                          {14, "da == 1"}, {13, "glc == 1 && !coherent_store"}, {12, "unrm == 1 && !integer_image_op"}};
	for (const auto& flag: flags)
	{
		const uint32_t words[] = {MimgWord(0x20u, false, false, mask) | (1u << flag.bit), MimgOperands(2, 0, 4, 0), 0xbf810000u};
		if (!ExpectShaderParserRejection(words, ShaderType::Pixel, false, flag.diagnostic)) return false;
	}
	for (const uint32_t operands: {MimgOperands(2, 0, 4, 254), MimgOperands(2, 25, 4, 0)})
	{
		const uint32_t words[] = {MimgWord(0x20u, false, false, mask), operands, 0xbf810000u};
		if (!ExpectShaderParserRejection(words, ShaderType::Pixel, false, "!ShaderImageSampleSingleComponentInstructionSupported")) return false;
	}
	if (!ExpectGraphicsRejection([mask]() {
		const uint32_t words[] = {MimgWord(0x20u, false, false, mask), MimgOperands(2, 0, 4, 0), 0xbf810000u};
		ShaderCode code;
		code.SetType(ShaderType::Pixel);
		Lumen::Libs::Graphics::ShaderParse(words, &code);
		code.GetInstructions()[0].src[0].absolute = true;
		ShaderPixelInputInfo input {};
		(void)Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, &input, nullptr);
	}, "!ShaderImageSampleSingleComponentInstructionSupported")) return false;
	for (const int texture_count: {-1, Lumen::Libs::Graphics::ShaderTextureResources::RES_MAX + 1})
	{
		const auto expected = "invalid-count-metadata textures=" + std::to_string(texture_count);
		if (!ExpectGraphicsRejection([&]() {
			const uint32_t words[] = {MimgWord(0x20u, false, false, mask), MimgOperands(2, 0, 4, 0), 0xbf810000u};
			ShaderCode code;
			code.SetType(ShaderType::Pixel);
			Lumen::Libs::Graphics::ShaderParse(words, &code);
			ShaderPixelInputInfo input {};
			input.bind.textures2D.textures_num = texture_count;
			input.bind.textures2D.textures2d_sampled_num = 1;
			input.bind.samplers.samplers_num = 1;
			(void)Lumen::Libs::Graphics::SpirvGenerateSource(code, nullptr, &input, nullptr);
		}, expected)) return false;
	}
	std::fprintf(stderr, "[GFX_IMAGE_SAMPLE_DMASK%u_REJECTION_TEST] parser-cases=11 zero-mask=lz=unsupported-flags=tuple-overflow=reject "
	                     "cached-ir=reject public-count-rejections=2 status=clean\n", mask);
	return true;
}
#else
bool CheckComparisonImageParserRejections()
{
	// Process rejection coverage is available on the POSIX test hosts only.
	return true;
}

bool CheckParameterExportParserRejections()
{
	return true;
}

bool CheckScanModeDrawRejections()
{
	// Process rejection coverage is available on the POSIX test hosts only.
	return true;
}

bool CheckAaMaskDrawRejections()
{
	return true;
}

bool CheckVMulLegacyF32ParserRejections()
{
	return true;
}

bool CheckVMacLegacyF32ParserRejections()
{
	return true;
}

bool CheckVRsqClampF32ParserRejections()
{
	return true;
}

bool CheckUnknownShaderInstructionDiagnostic()
{
	return true;
}

bool CheckVMinU32ParserRejections()
{
	return true;
}

bool CheckVCvtOffF32I4ParserRejections()
{
	// Fatal parser/lowerer rejection cases run only on the POSIX test hosts.
	return true;
}

bool CheckImageSampleSingleComponentParserRejections(uint32_t)
{
	return true; // Fatal parser/lowerer rejection cases run on POSIX test hosts.
}
#endif

bool CheckThreadTraceDataPipeline()
{
	using namespace Lumen::Libs::Graphics;
	// Entirely synthetic SOP1/SOPP/VOP1/export program. Owned padding makes
	// the bounded decoder's conservative lookahead available after endpgm.
	constexpr uint32_t program[] = {0xbe800381u, 0xbf960000u, 0x7e000200u,
	    0xf800180fu, 0x00000000u, 0xbf810000u, 0, 0, 0, 0};
	constexpr uint32_t branched[] = {0xbe800381u, 0xbf820001u, 0xbf960000u, 0x7e000200u,
	    0xf800180fu, 0x00000000u, 0xbf810000u, 0, 0, 0, 0};
	constexpr uint32_t kill_join_program[] = {0xbf8c0000u, 0xbf840004u, 0xbefe0480u,
	    0xf8001c00u, 0x00000000u, 0xbf810000u, 0xbf8c0000u, 0xf800180fu,
	    0x00000000u, 0xbf810000u};
	ShaderCode code;
	code.SetType(ShaderType::Pixel);
	if (code.GetThreadTraceMode() != ShaderThreadTraceMode::Unknown) return false;
	code.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
	if (ShaderTryParseBounded(program, std::size(program), &code) != 6 || code.GetInstructions().Size() != 5) return false;
	const auto& instructions = code.GetInstructions();
	const auto& trace = instructions.At(1);
	if (instructions.At(0).type != ShaderInstructionType::SMovB32 || instructions.At(0).pc != 0 ||
	    instructions.At(0).dst.register_id != 0 || instructions.At(0).src[0].constant.i != 1 ||
	    trace.pc != 4 || !ShaderThreadTraceDataInstructionSupported(code, trace) ||
	    instructions.At(2).type != ShaderInstructionType::VMovB32 || instructions.At(2).pc != 8 ||
	    instructions.At(2).src[0].type != ShaderOperandType::Sgpr || instructions.At(2).src[0].register_id != 0 ||
	    instructions.At(3).type != ShaderInstructionType::Exp || instructions.At(3).pc != 12 ||
	    !instructions.At(3).export_valid_mask || !instructions.At(3).export_done || instructions.At(4).pc != 20)
	{
		std::fprintf(stderr, "S_TTRACEDATA did not preserve the following synthetic instructions/PCs\n");
		return false;
	}
	ShaderCode branch;
	branch.SetType(ShaderType::Pixel);
	branch.SetThreadTraceMode(ShaderThreadTraceMode::Disabled);
	if (ShaderTryParseBounded(branched, std::size(branched), &branch) != 7 || branch.GetInstructions().Size() != 6 ||
	    branch.GetInstructions().At(2).pc != 8 || branch.GetInstructions().At(3).pc != 12 ||
	    branch.GetLabels().Size() != 1 || branch.GetLabels().At(0).GetSrc() != 4 || branch.GetLabels().At(0).GetDst() != 12)
		return false;

	for (const auto mode: {ShaderThreadTraceMode::Unknown, ShaderThreadTraceMode::Enabled})
	{
		ShaderCode unsupported;
		unsupported.SetType(ShaderType::Pixel);
		unsupported.SetThreadTraceMode(mode);
		if (ShaderTryParseBounded(program, std::size(program), &unsupported) != 0) return false;
	}
	for (uint32_t bit = 0; bit < 16; ++bit)
	{
		uint32_t mutated[std::size(program)];
		std::memcpy(mutated, program, sizeof(program));
		mutated[1] |= uint32_t{1} << bit;
		ShaderCode unsupported = code;
		if (ShaderTryParseBounded(mutated, std::size(mutated), &unsupported) != 0) return false;
	}
	// A neighboring admitted opcode retains its own IR, without being
	// reclassified merely because the trace execution contract is disabled.
	uint32_t wait_variant[std::size(program)];
	std::memcpy(wait_variant, program, sizeof(program));
	wait_variant[1] = 0xbf8c0000u;
	ShaderCode wait = code;
	if (ShaderTryParseBounded(wait_variant, std::size(wait_variant), &wait) != 6 ||
	    wait.GetInstructions().At(1).type != ShaderInstructionType::SWaitcnt) return false;

	ShaderPixelInputInfo input {};
	input.target_output_mode[0] = 9;
	input.shader_mask = 0xf;
	const auto source = SpirvGenerateSource(code, nullptr, &input, nullptr);
	const std::string_view text(source.c_str(), source.Size());
	if (CountSubstring(text, "OpStore %s0 ") != 1 || CountSubstring(text, "OpStore %v0 ") != 1 ||
	    CountSubstring(text, "OpStore %outColor ") != 1) return false;
	const auto branch_source = SpirvGenerateSource(branch, nullptr, &input, nullptr);
	if (CountSubstring(std::string_view(branch_source.c_str(), branch_source.Size()), "OpBranch %label_000c_0004") != 1)
		return false;

	ShaderCode baseline = code;
	baseline.GetInstructions().Clear();
	for (const auto& instruction: code.GetInstructions())
		if (instruction.type != ShaderInstructionType::STTraceData) baseline.GetInstructions().Add(instruction);
	const auto module = ShaderRecompilePS(code, &input);
	const auto baseline_module = ShaderRecompilePS(baseline, &input);
	const auto branch_module = ShaderRecompilePS(branch, &input);
	ShaderCode branch_baseline = branch;
	branch_baseline.GetInstructions().Clear();
	for (const auto& instruction: branch.GetInstructions())
		if (instruction.type != ShaderInstructionType::STTraceData) branch_baseline.GetInstructions().Add(instruction);
	const auto branch_baseline_module = ShaderRecompilePS(branch_baseline, &input);
	ShaderCode branch_wait = branch;
	branch_wait.GetInstructions()[2].type = ShaderInstructionType::SWaitcnt;
	const auto branch_wait_module = ShaderRecompilePS(branch_wait, &input);
	// The existing kill-only export terminates the block too. Its following
	// endpgm emits nothing; the joined ordinary export opens an active block.
	ShaderCode kill_join;
	kill_join.SetType(ShaderType::Pixel);
	ShaderParse(kill_join_program, &kill_join);
	if (kill_join.GetInstructions().Size() != 8 || kill_join.GetLabels().Size() != 1 ||
	    kill_join.GetLabels().At(0).GetDst() != 24) return false;
	auto kill_input = input;
	kill_input.ps_pixel_kill_enable = true;
	const auto kill_source = SpirvGenerateSource(kill_join, nullptr, &kill_input, nullptr);
	const std::string_view kill_text(kill_source.c_str(), kill_source.Size());
	if (CountSubstring(kill_text, "OpKill") != 2 || CountSubstring(kill_text, "OpStore %outColor ") != 1) return false;
	const auto kill_module = ShaderRecompilePS(kill_join, &kill_input);
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
		std::fprintf(stderr, "thread-trace SPIR-V line %zu: %s\n", position.line, message);
	});
	const bool branch_baseline_valid = validator.Validate(branch_baseline_module.GetDataConst(), branch_baseline_module.Size());
	const bool branch_wait_valid = validator.Validate(branch_wait_module.GetDataConst(), branch_wait_module.Size());
	const bool branch_trace_valid = validator.Validate(branch_module.GetDataConst(), branch_module.Size());
	if (!branch_baseline_valid || !branch_wait_valid || !branch_trace_valid)
	{
		std::fprintf(stderr, "branch after nonemitting IR: baseline=%u existing_wait=%u trace=%u\n",
		             branch_baseline_valid, branch_wait_valid, branch_trace_valid);
		return false;
	}
	if (module.Size() < 5 || module.At(0) != 0x07230203u ||
	    !validator.Validate(module.GetDataConst(), module.Size()) ||
	    !validator.Validate(baseline_module.GetDataConst(), baseline_module.Size()) ||
	    !validator.Validate(kill_module.GetDataConst(), kill_module.Size())) return false;
	const auto operation_sequence = [](const auto& words) {
		std::vector<uint32_t> operations;
		for (uint32_t index = 5; index < words.Size();)
		{
			const auto count = words.At(index) >> 16u;
			if (count == 0 || count > words.Size() - index) return std::vector<uint32_t>{};
			operations.push_back(words.At(index) & 0xffffu);
			index += count;
		}
		return operations;
	};
	// Trace contributes no SPIR-V operations at all, including any additional
	// stores to M0/SCC/EXEC, waits or barriers. Arithmetic/exports stay present.
	const auto operations = operation_sequence(module);
	if (operations.empty() || operations != operation_sequence(baseline_module) ||
	    operation_sequence(branch_module) != operation_sequence(branch_baseline_module) ||
	    operation_sequence(branch_wait_module) != operation_sequence(branch_baseline_module)) return false;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	constexpr std::string_view parser_diagnostic = "unsupported S_TTRACEDATA requires explicitly disabled thread trace and SIMM16=0";
	constexpr std::string_view ir_diagnostic = "unsupported S_TTRACEDATA IR requires explicitly disabled thread trace and canonical SIMM16=0";
	for (const auto mode: {ShaderThreadTraceMode::Unknown, ShaderThreadTraceMode::Enabled})
	{
		if (!ExpectGraphicsRejection([&]() {
			ShaderCode unsupported;
			unsupported.SetType(ShaderType::Pixel);
			unsupported.SetThreadTraceMode(mode);
			ShaderParse(program, &unsupported);
		}, parser_diagnostic)) return false;
		if (!ExpectGraphicsRejection([&]() {
			auto revoked = code;
			revoked.SetThreadTraceMode(mode);
			SpirvGenerateSource(revoked, nullptr, &input, nullptr);
		}, ir_diagnostic)) return false;
	}
	if (!ExpectGraphicsRejection([&]() {
		uint32_t unsupported[std::size(program)];
		std::memcpy(unsupported, program, sizeof(program));
		unsupported[1] |= 0xffffu;
		auto enabled = code;
		ShaderParse(unsupported, &enabled);
	}, parser_diagnostic)) return false;
	if (!ExpectGraphicsRejection([&]() {
		Lumen::Scripts::ScriptsSubsystem::Instance()->Init(nullptr);
		if (Lumen::Scripts::RunString(U"thread_trace_debug_test = { SpirvDebugPrintfEnabled = true }") !=
		    Lumen::Scripts::ScriptError::Ok) std::_Exit(111);
		Lumen::Config::Load(Lumen::Scripts::GlobalGetVar(U"thread_trace_debug_test"));
		ShaderDebugPrintf command;
		command.pc = 4;
		command.format = U"synthetic trace debug point";
		auto active = code;
		active.GetDebugPrintfs().Add(command);
		const auto active_source = SpirvGenerateSource(active, nullptr, &input, nullptr);
		if (std::string_view(active_source.c_str(), active_source.Size()).find("OpExtInst %void %NonSemantic_DebugPrintf") ==
		    std::string_view::npos) std::_Exit(111);
		auto injected = branch;
		command.pc = 8;
		injected.GetDebugPrintfs().Add(command);
		SpirvGenerateSource(injected, nullptr, &input, nullptr);
	}, "cannot emit shader debug printf after a terminated block")) return false;
#endif
	for (uint32_t mutation = 0; mutation < 16; ++mutation)
	{
		auto malformed = code;
		auto& instruction = malformed.GetInstructions()[1];
		switch (mutation)
		{
			case 0: instruction.src[0].constant.u = 1; break;
			case 1: instruction.format = ShaderInstructionFormat::Empty; break;
			case 2: instruction.src_num = 0; break;
			case 3: instruction.src[0].type = ShaderOperandType::IntegerInlineConstant; break;
			case 4: instruction.src[0].size = 1; break;
			case 5: instruction.src[0].register_id = 1; break;
			case 6: instruction.src[0].absolute = true; break;
			case 7: instruction.src[0].negate = true; break;
			case 8: instruction.src[0].clamp = true; break;
			case 9: instruction.src[0].multiplier = 2.0f; break;
			case 10: instruction.dst.type = ShaderOperandType::M0; break;
			case 11: instruction.dst2.type = ShaderOperandType::Scc; break;
			case 12: instruction.src[1].type = ShaderOperandType::ExecLo; break;
			case 13: instruction.globally_coherent = true; break;
			case 14: instruction.export_done = true; break;
			case 15: instruction.unnormalized_coordinates = true; break;
		}
		if (ShaderThreadTraceDataInstructionSupported(malformed, instruction)) return false;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_LINUX || LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		if (!ExpectGraphicsRejection([&]() { SpirvGenerateSource(malformed, nullptr, &input, nullptr); }, ir_diagnostic)) return false;
#endif
	}
	return true;
}

bool CheckEopLeaseRetryPolicy()
{
	using namespace Lumen::Libs::Graphics;
	for (const char* reason: {"materialization-rejected", "depth-equivalence-unproven", "readonly-storage-generation-changed",
	                         "readonly-storage-content-diverged", "readonly-storage-permission-unavailable",
	                         "readonly-storage-range-unavailable", "target-publication-rejected"})
		if (GpuMemoryEopCheckpointRetryAllowed({GpuMemoryEopCheckpointStatus::Rejected, reason}, 1, 7, 7)) return false;
	const GpuMemoryEopCheckpointResult unavailable {GpuMemoryEopCheckpointStatus::Rejected, "readonly-storage-lease-unavailable", true};
	for (uint32_t attempts = 0; attempts <= GpuMemoryEopCheckpointMaxAttempts + 1; ++attempts)
		if (GpuMemoryEopCheckpointRetryAllowed(unavailable, attempts, 7, 7) !=
		    (attempts > 0 && attempts < GpuMemoryEopCheckpointMaxAttempts)) return false;
	if (GpuMemoryEopCheckpointRetryAllowed(unavailable, 1, 7, 8) ||
	    GpuMemoryEopCheckpointRetryAllowed(unavailable, 1, UINT64_MAX, UINT64_MAX) ||
	    GpuMemoryEopCheckpointRetryAllowed(unavailable, UINT32_MAX, 7, 7)) return false;
	for (const auto status: {GpuMemoryEopCheckpointStatus::Materialized, GpuMemoryEopCheckpointStatus::Published})
		if (GpuMemoryEopCheckpointRetryAllowed({status, "lease-unavailable", true}, 1, 7, 7)) return false;
	return true;
}

bool CheckDx10ClampPreCacheGate()
{
	using Lumen::Libs::Graphics::HW::PixelShaderInfo;
	using Lumen::Libs::Graphics::HW::VertexShaderInfo;
	const bool zero_experiment = Lumen::Libs::Graphics::ShaderDx10ClampZeroExperimentEnabled();

	VertexShaderInfo vertex {};
	PixelShaderInfo  pixel {};
	vertex.vs_embedded = true;
	pixel.ps_embedded  = true;
	if (!Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel))
	{
		return false;
	}

	vertex.vs_embedded              = false;
	vertex.vs_regs.data_addr        = 1;
	vertex.vs_regs.rsrc1.dx10_clamp = false;
	if (Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel) != zero_experiment)
	{
		return false;
	}
	vertex.vs_regs.rsrc1.dx10_clamp = true;
	if (!Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel))
	{
		return false;
	}

	vertex.vs_regs.data_addr        = 0;
	vertex.vs_regs.rsrc1.dx10_clamp = false;
	vertex.es_regs.data_addr        = 1;
	vertex.gs_regs.rsrc1.dx10_clamp = false;
	if (Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel))
	{
		return false;
	}
	vertex.gs_regs.rsrc1.dx10_clamp = true;
	if (!Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel))
	{
		return false;
	}

	vertex.vs_embedded              = true;
	vertex.gs_regs.rsrc1.dx10_clamp = false;
	pixel.ps_embedded               = false;
	pixel.ps_regs.rsrc1.dx10_clamp  = false;
	if (Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel) != zero_experiment)
	{
		return false;
	}
	pixel.ps_regs.rsrc1.dx10_clamp = true;
	if (!Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel))
	{
		return false;
	}

	vertex.vs_embedded              = false;
	vertex.vs_regs.data_addr        = 1;
	vertex.vs_regs.rsrc1.dx10_clamp = false;
	if (Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel))
	{
		return false;
	}

	vertex.es_regs.data_addr = 0;
	vertex.gs_regs.data_addr = 0;
	if (Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel) != zero_experiment)
	{
		return false;
	}
	vertex.vs_embedded = true;
	pixel.ps_embedded  = true;
	return Lumen::Libs::Graphics::ShaderGraphicsDx10ClampStateSupported(vertex, pixel);
}

int InspectRectListCapture(const char* shader_path, const char* input_path)
{
	using namespace Lumen::Libs::Graphics;
	ShaderVertexInputInfo input {};
	FILE* input_file = std::fopen(input_path, "rb");
	if (input_file == nullptr) return 1;
	const bool input_read = std::fread(&input, 1, sizeof(input), input_file) == sizeof(input) && std::fgetc(input_file) == EOF;
	std::fclose(input_file);
	if (!input_read) { std::fprintf(stderr, "capture input must match host ABI size %zu\n", sizeof(input)); return 1; }
	FILE* shader_file = std::fopen(shader_path, "rb");
	if (shader_file == nullptr) return 1;
	std::vector<uint32_t> binary(16384);
	const size_t bytes = std::fread(binary.data(), 1, binary.size() * sizeof(uint32_t), shader_file);
	const bool shader_read = bytes >= 4 && bytes % sizeof(uint32_t) == 0 && std::fgetc(shader_file) == EOF;
	std::fclose(shader_file);
	if (!shader_read) return 1;
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	code.SetFp32Mode(ShaderFp32Mode::RneFlushInOut);
	ShaderParse(binary.data(), &code);
	input.rect_list = true;
	ShaderCalcBindingIndices(&input.bind);
	const char* reason = nullptr;
	uint32_t pc = UINT32_MAX;
	const bool supported = ShaderRectListProfileSupported(code, input, &reason, &pc);
	std::printf("RectList capture profile=%d reason=%s pc=%08x instructions=%u exports=%d buffers=%d\n%s\n",
	            supported, reason == nullptr ? "none" : reason, pc, code.GetInstructions().Size(), input.export_count,
	            input.bind.storage_buffers.buffers_num, code.DbgDump().c_str());
	if (!supported) return 2;
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
		std::fprintf(stderr, "capture SPIR-V line %zu: %s\n", position.line, message);
	});
	std::vector<uint32_t> words;
	const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);
	return validator.Assemble(source.c_str(), &words) && validator.Validate(words) ? 0 : 3;
}

bool CheckRectListPipeline(const char* output_directory = nullptr)
{
	using namespace Lumen::Libs::Graphics;
	auto reg = [](int index, int size = 1, ShaderOperandType type = ShaderOperandType::Vgpr) {
		ShaderOperand result; result.type = type; result.register_id = index; result.size = size; return result;
	};
	auto scalar = [&](int index, int size = 1) { return reg(index, size, ShaderOperandType::Sgpr); };
	auto constant = [](float value) {
		ShaderOperand result; result.type = ShaderOperandType::FloatInlineConstant; result.constant.f = value; return result;
	};
	ShaderCode code;
	code.SetType(ShaderType::Vertex);
	auto emit = [&](ShaderInstructionType type, ShaderInstructionFormat::Format format, ShaderOperand destination,
	                std::initializer_list<ShaderOperand> sources) {
		ShaderInstruction instruction;
		instruction.type = type; instruction.format = format; instruction.dst = destination;
		instruction.pc = code.GetInstructions().Size() * 4u;
		instruction.src_num = static_cast<int>(sources.size());
		int index = 0; for (const auto& operand: sources) instruction.src[index++] = operand;
		code.GetInstructions().Add(instruction);
	};
	// Three input records only. A fourth physical canary lets the GPU probe
	// distinguish post-VS completion from another fetch or input extrapolation.
	emit(ShaderInstructionType::VCvtF32I32, ShaderInstructionFormat::SVdstSVsrc0, reg(30), {reg(3)});
	emit(ShaderInstructionType::BufferLoadFormatXyzw, ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen,
	     reg(1, 4), {reg(0), scalar(4, 4), [] { ShaderOperand offset; offset.type = ShaderOperandType::IntegerInlineConstant; return offset; }()});
	emit(ShaderInstructionType::VMulF32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, reg(10), {reg(1), reg(2)});
	emit(ShaderInstructionType::VMulF32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, reg(10), {reg(10), constant(0.125f)});
	emit(ShaderInstructionType::VAddF32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, reg(10), {reg(10), constant(0.5f)});
	emit(ShaderInstructionType::VMulF32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, reg(11), {reg(1), constant(0.25f)});
	emit(ShaderInstructionType::VAddF32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, reg(11), {reg(11), constant(0.5f)});
	emit(ShaderInstructionType::VMulF32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, reg(30), {reg(30), constant(0.125f)});
	emit(ShaderInstructionType::VAddF32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1, reg(30), {scalar(0), reg(30)});
	emit(ShaderInstructionType::VMovB32, ShaderInstructionFormat::SVdstSVsrc0, reg(8), {constant(0)});
	emit(ShaderInstructionType::VMovB32, ShaderInstructionFormat::SVdstSVsrc0, reg(9), {constant(1)});
	emit(ShaderInstructionType::Exp, ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done, {}, {reg(1), reg(2), reg(8), reg(9)});
	emit(ShaderInstructionType::Exp, ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3, {}, {reg(10), reg(11), reg(30), reg(9)});
	emit(ShaderInstructionType::SEndpgm, ShaderInstructionFormat::Empty, {}, {});
	ShaderVertexInputInfo input {};
	input.export_count = 1;
	input.bind.storage_buffers.buffers_num = 1;
	input.bind.storage_buffers.start_register[0] = 4;
	input.bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadOnly;
	input.bind.storage_buffers.buffers[0].fields[1] = 16u << 16u;
	input.bind.storage_buffers.buffers[0].fields[2] = 3;
	input.bind.storage_buffers.buffers[0].fields[3] = DstSel(4, 5, 6, 7) | (7u << 12u) | (14u << 15u);
	input.bind.direct_sgprs.sgprs_num = 1;
	input.bind.direct_sgprs.start_register[0] = 0;
	input.bind.direct_sgprs.sgprs[0].field = 0x3e800000u;
	ShaderCalcBindingIndices(&input.bind);
	const char* rejection_reason = "previous failure";
	uint32_t rejection_pc = 0;
	if (!ShaderRectListProfileSupported(code, input, &rejection_reason, &rejection_pc) || rejection_reason != nullptr ||
	    rejection_pc != UINT32_MAX || input.bind.push_constant_size != 32) return false;
	if (VRsqClampF32CandidateAdmitted())
	{
		auto rsq_pure = code;
		ShaderInstruction rsq;
		rsq.type = ShaderInstructionType::VRsqClampF32;
		rsq.format = ShaderInstructionFormat::SVdstSVsrc0;
		rsq.dst = reg(2);
		rsq.src[0] = reg(2);
		rsq.src_num = 1;
		rsq_pure.GetInstructions().InsertAt(11, rsq);
		for (uint32_t index = 0; index < rsq_pure.GetInstructions().Size(); ++index)
			rsq_pure.GetInstructions()[index].pc = index * 4u;
		if (!ShaderRectListProfileSupported(rsq_pure, input))
		{
			std::fprintf(stderr, "V_RSQ_CLAMP_F32 lost RectList pure-instruction admission\n");
			return false;
		}
	}
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
	validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& position, const char* message) {
		std::fprintf(stderr, "rect-list SPIR-V line %zu: %s\n", position.line, message);
	});
	for (const bool wrapped: {false, true})
	{
		input.rect_list = wrapped;
		const auto source = SpirvGenerateSource(code, &input, nullptr, nullptr);
		std::vector<uint32_t> words;
		if (!validator.Assemble(source.c_str(), &words) || !validator.Validate(words)) return false;
		const std::string_view text(source.c_str(), source.Size());
		if ((text.find("%rect_guest = OpFunction") != std::string_view::npos) != wrapped) return false;
		if (wrapped && (CountSubstring(text, "OpFunctionCall %void %rect_guest") != 4 ||
		                text.find("OpLoad %int %gl_InstanceIndex") == std::string_view::npos ||
		                text.find("%rect_param_0 = OpFSub") == std::string_view::npos)) return false;
		if (output_directory != nullptr)
		{
			const std::string path = std::string(output_directory) + (wrapped ? "/rect_list_wrapped.spv" : "/rect_list_original.spv");
			FILE* file = std::fopen(path.c_str(), "wb");
			if (file == nullptr) return false;
			const bool written = std::fwrite(words.data(), sizeof(uint32_t), words.size(), file) == words.size();
			const bool closed = std::fclose(file) == 0;
			if (!written || !closed) return false;
		}
	}
	for (int mutation = 0; mutation < 9; ++mutation)
	{
		auto bad = input;
		switch (mutation)
		{
			case 0: bad.fetch_external = true; break;
			case 1: bad.fetch_inline = true; break;
			case 2: bad.resources_num = 1; break;
			case 3: bad.buffers_num = 1; break;
			case 4: bad.export_count = 2; break;
			case 5: bad.bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadWrite; break;
			case 6: bad.bind.textures2D.textures_num = 1; break;
			case 7: bad.bind.gds_pointers.pointers_num = 1; break;
			case 8: bad.bind.extended.used = true; break;
		}
		if (ShaderRectListProfileSupported(code, bad)) return false;
	}
	for (int mutation = 0; mutation < 10; ++mutation)
	{
		auto bad = code;
		auto& instructions = bad.GetInstructions();
		switch (mutation)
		{
			case 0: instructions[10].src[0] = constant(2); break;
			case 1: instructions[10].src[0].negate = true; break;
			case 2: instructions[9].src[0] = reg(0); break;
			case 3: instructions[12].format = ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3; break;
			case 4: instructions[0].type = ShaderInstructionType::BufferStoreDword; break;
			case 5: instructions[0].type = ShaderInstructionType::DsAppend; break;
			case 6: instructions[0].type = ShaderInstructionType::ImageStore; break;
			case 7: instructions[0].type = ShaderInstructionType::SEndpgm; break;
			case 8: bad.GetDebugPrintfs().Add(ShaderDebugPrintf {}); break;
			case 9:
				instructions[0].type = ShaderInstructionType::SCbranchExecz;
				instructions[0].format = ShaderInstructionFormat::Label;
				instructions[0].src[0].type = ShaderOperandType::IntegerInlineConstant;
				instructions[0].src[0].constant.i = instructions[11].pc - 4;
				break;
		}
		if (ShaderRectListProfileSupported(bad, input, &rejection_reason, &rejection_pc) || rejection_reason == nullptr) return false;
	}
	auto renumber = [](ShaderCode& value) {
		for (uint32_t index = 0; index < value.GetInstructions().Size(); ++index) value.GetInstructions()[index].pc = index * 4;
	};
	auto validate = [&](const ShaderCode& value, const ShaderVertexInputInfo& state) {
		if (!ShaderRectListProfileSupported(value, state)) return false;
		const auto source = SpirvGenerateSource(value, &state, nullptr, nullptr);
		std::vector<uint32_t> words;
		return validator.Assemble(source.c_str(), &words) && validator.Validate(words);
	};
	// Parser-backed export scheduling: position is already stored when its
	// source VGPR is reused to build a later parameter. No game binary is used.
	constexpr uint32_t scheduled_exports_program[] = {
	    0x7e0002f3u, 0x7e0202f2u, 0x7e0402f3u, 0x7e0602f2u, // v0=-1, v1=1, Z=-1, W=1
	    0xf80008cfu, 0x03020100u,                            // export Pos0
	    0xbf8c0f0fu, 0x7e000280u,                           // wait; v0=0
	    0xf800020fu, 0x00000000u, 0xbf810000u,               // export Param0; end
	};
	ShaderCode scheduled;
	scheduled.SetType(ShaderType::Vertex);
	ShaderParse(scheduled_exports_program, &scheduled);
	ShaderVertexInputInfo scheduled_input {};
	scheduled_input.rect_list = true;
	scheduled_input.export_count = 1;
	if (scheduled.GetInstructions().Size() != 9 || !validate(scheduled, scheduled_input)) return false;
	// Clobbering a position source after Pos0 cannot change that stored output.
	auto after_position = scheduled;
	after_position.GetInstructions()[6].dst = reg(3);
	after_position.GetInstructions()[6].src[0] = constant(2);
	if (!validate(after_position, scheduled_input)) return false;
	// The parser may schedule ordinary arithmetic after Pos0 as well. The
	// literal operands make instruction PCs nonuniform without any CFG edges.
	constexpr uint32_t arithmetic_exports_program[] = {
	    0x7e0002f3u, 0x7e0202f2u, 0x7e0402f3u, 0x7e0602f2u, // v0=-1, v1=1, Z=-1, W=1
	    0xf80008cfu, 0x03020100u,                            // export Pos0
	    0x060802ffu, 0x3e800000u,                            // v4=0.25+v1
	    0x060a00ffu, 0x3f000000u,                            // v5=0.5+v0
	    0xf800020fu, 0x04050100u, 0xbf810000u,               // export Param0(v0,v1,v5,v4); end
	};
	ShaderCode arithmetic_exports;
	arithmetic_exports.SetType(ShaderType::Vertex);
	ShaderParse(arithmetic_exports_program, &arithmetic_exports);
	if (arithmetic_exports.GetInstructions().Size() != 9 ||
	    arithmetic_exports.GetInstructions().At(5).type != ShaderInstructionType::VAddF32 ||
	    arithmetic_exports.GetInstructions().At(6).type != ShaderInstructionType::VAddF32 ||
	    !validate(arithmetic_exports, scheduled_input)) return false;
	auto arithmetic_before_position = arithmetic_exports;
	std::swap(arithmetic_before_position.GetInstructions()[4], arithmetic_before_position.GetInstructions()[7]);
	for (int lane = 0; lane < 4; ++lane) arithmetic_before_position.GetInstructions()[4].src[lane] = reg(lane);
	renumber(arithmetic_before_position);
	if (!validate(arithmetic_before_position, scheduled_input)) return false;
	for (const int position_lane: {2, 3})
	{
		auto bad = arithmetic_before_position;
		bad.GetInstructions()[5].dst = reg(position_lane);
		if (ShaderRectListProfileSupported(bad, scheduled_input)) return false;
	}
	// Reuse the nonlinear fixture's complete arithmetic after Pos0, then read
	// the vertex record again and compute a scalar value before its parameter.
	// These operations have the same replay contract on either side of Pos0.
	ShaderCode pure_tail;
	pure_tail.SetType(ShaderType::Vertex);
	for (const int index: {0, 1, 9, 10, 11, 2, 3, 4, 5, 6, 7, 8})
		pure_tail.GetInstructions().Add(code.GetInstructions().At(index));
	auto tail_read = code.GetInstructions().At(1);
	tail_read.dst = reg(20, 4);
	pure_tail.GetInstructions().Add(tail_read);
	auto tail_scalar = code.GetInstructions().At(4);
	tail_scalar.type = ShaderInstructionType::SAddU32;
	tail_scalar.dst = scalar(20);
	tail_scalar.src[0] = scalar(0);
	tail_scalar.src[1] = {};
	tail_scalar.src[1].type = ShaderOperandType::IntegerInlineConstant;
	tail_scalar.src[1].constant.u = 1;
	pure_tail.GetInstructions().Add(tail_scalar);
	auto tail_add = code.GetInstructions().At(4);
	tail_add.dst = reg(8);
	tail_add.src[0] = reg(20);
	tail_add.src[1] = scalar(20);
	pure_tail.GetInstructions().Add(tail_add);
	auto tail_parameter = code.GetInstructions().At(12);
	tail_parameter.src[2] = reg(8);
	pure_tail.GetInstructions().Add(tail_parameter);
	pure_tail.GetInstructions().Add(code.GetInstructions().At(13));
	renumber(pure_tail);
	if (!validate(pure_tail, input)) return false;
	auto tail_constant_input = input;
	tail_constant_input.bind.storage_buffers.usages[0] = ShaderStorageUsage::Constant;
	if (!validate(pure_tail, tail_constant_input)) return false;
	auto tail_writable_input = input;
	tail_writable_input.bind.storage_buffers.usages[0] = ShaderStorageUsage::ReadWrite;
	if (ShaderRectListProfileSupported(pure_tail, tail_writable_input)) return false;
	auto parameter_first = scheduled;
	std::swap(parameter_first.GetInstructions()[4], parameter_first.GetInstructions()[7]);
	renumber(parameter_first);
	if (!validate(parameter_first, scheduled_input)) return false;
	for (int mutation = 0; mutation < 11; ++mutation)
	{
		auto bad = parameter_first;
		auto& between = bad.GetInstructions()[6];
		switch (mutation)
		{
			case 0: between.dst = reg(3); between.src[0] = constant(2); break; // W changed before Pos0
			case 1: between.dst = reg(2); between.src[0] = reg(0); break;     // Z no longer uniform
			case 2: between.dst.type = ShaderOperandType::ExecLo; break;
			case 3: between.dst2.type = ShaderOperandType::ExecHi; break;   // implicit-size destination still matters
			case 4: between.type = ShaderInstructionType::VCmpxEqU32; break;
			case 5: between.type = ShaderInstructionType::BufferStoreDword; break;
			case 6: between.dst = reg(3); between.src[0] = constant(1); between.src[0].negate = true; break;
			case 7: between.type = ShaderInstructionType::DsAppend; break;
			case 8:
				between.type = ShaderInstructionType::SCbranchExecz;
				between.format = ShaderInstructionFormat::Label;
				between.src[0].type = ShaderOperandType::IntegerInlineConstant;
				between.src[0].constant.i = 4;
				break;
			case 9: bad.GetInstructions()[5].type = ShaderInstructionType::SEndpgm; break;
			case 10: between.type = ShaderInstructionType::ImageStore; break;
		}
		if (ShaderRectListProfileSupported(bad, scheduled_input)) return false;
	}
	auto duplicate_output = scheduled;
	duplicate_output.GetInstructions().InsertAt(8, scheduled.GetInstructions().At(7));
	renumber(duplicate_output);
	if (ShaderRectListProfileSupported(duplicate_output, scheduled_input)) return false;
	auto missing_output = scheduled;
	missing_output.GetInstructions()[7] = scheduled.GetInstructions().At(5);
	renumber(missing_output);
	if (ShaderRectListProfileSupported(missing_output, scheduled_input)) return false;
	// Additional complete attributes exercise all local export slots.
	auto five_parameters = code;
	auto five_input = input;
	five_input.export_count = 5;
	for (const auto format: {ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3,
	                        ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3,
	                        ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3,
	                        ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3})
	{
		auto output = code.GetInstructions()[12];
		output.format = format;
		five_parameters.GetInstructions().InsertAt(five_parameters.GetInstructions().Size() - 1, output);
	}
	renumber(five_parameters);
	if (!validate(five_parameters, five_input)) return false;
	// A forward branch joins before the common output-defining tail.
	auto branched = code;
	ShaderInstruction branch;
	branch.type = ShaderInstructionType::SCbranchExecz; branch.format = ShaderInstructionFormat::Label;
	branch.src_num = 1; branch.src[0].type = ShaderOperandType::IntegerInlineConstant; branch.src[0].constant.i = 4;
	ShaderInstruction wait;
	wait.type = ShaderInstructionType::SWaitcnt; wait.format = ShaderInstructionFormat::Imm;
	wait.src_num = 1; wait.src[0].type = ShaderOperandType::IntegerInlineConstant;
	branched.GetInstructions().InsertAt(0, wait);
	branched.GetInstructions().InsertAt(0, branch);
	renumber(branched);
	branched.GetLabels().Add(ShaderLabel(branched.GetInstructions()[0]));
	if (!validate(branched, input)) return false;
	// Actual parser metadata includes a static fallthrough in IndirectLabels.
	// All supported conditional opcodes must keep the same bounded CFG proof.
	for (const uint32_t opcode: {4u, 5u, 6u, 7u, 8u})
	{
		// s_mov_b64 vcc,0 also defines the condition used by VCC branches.
		const uint32_t program[] = {0xbeea0480u, 0xbf800001u | (opcode << 16u), 0xbf8c0000u, 0xbf810000u};
		ShaderCode parsed;
		parsed.SetType(ShaderType::Vertex);
		ShaderParse(program, &parsed);
		if (parsed.GetInstructions().Size() != 4 || parsed.GetIndirectLabels().Size() != 1) return false;
		auto with_metadata = code;
		for (int index = 2; index >= 0; --index) with_metadata.GetInstructions().InsertAt(0, parsed.GetInstructions().At(index));
		renumber(with_metadata);
		with_metadata.GetLabels() = parsed.GetLabels();
		with_metadata.GetIndirectLabels() = parsed.GetIndirectLabels();
		if (!validate(with_metadata, input)) return false;
		for (const ShaderLabel foreign: {ShaderLabel(12, 4), ShaderLabel(12, 8), ShaderLabel(8, 100)})
		{
			auto bad = with_metadata;
			bad.GetIndirectLabels().Add(foreign);
			if (ShaderRectListProfileSupported(bad, input, &rejection_reason, &rejection_pc) || rejection_reason == nullptr) return false;
		}
	}
	for (const int target_index: {12, 13, 14, 15})
	{
		auto bad = branched;
		bad.GetInstructions()[0].src[0].constant.i = target_index * 4 - 4;
		if (ShaderRectListProfileSupported(bad, input)) return false;
	}
	// A saved entry EXEC value is restored at the common join before outputs.
	auto restored = code;
	auto special = [&](ShaderOperandType type, int size) { return reg(0, size, type); };
	ShaderOperand zero_mask; zero_mask.type = ShaderOperandType::IntegerInlineConstant; zero_mask.size = 2;
	ShaderInstruction set_mask;
	set_mask.type = ShaderInstructionType::SMovB64; set_mask.format = ShaderInstructionFormat::Sdst2Ssrc02;
	set_mask.dst = special(ShaderOperandType::VccLo, 2); set_mask.src_num = 1; set_mask.src[0] = zero_mask;
	ShaderInstruction save;
	save.type = ShaderInstructionType::SAndSaveexecB64; save.format = ShaderInstructionFormat::Sdst2Ssrc02;
	save.dst = scalar(40, 2); save.src_num = 1; save.src[0] = special(ShaderOperandType::VccLo, 2);
	ShaderInstruction restore;
	restore.type = ShaderInstructionType::SMovB64; restore.format = ShaderInstructionFormat::Sdst2Ssrc02;
	restore.dst = special(ShaderOperandType::ExecLo, 2); restore.src_num = 1; restore.src[0] = scalar(40, 2);
	restored.GetInstructions().InsertAt(0, restore);
	restored.GetInstructions().InsertAt(0, wait);
	restored.GetInstructions().InsertAt(0, branch);
	restored.GetInstructions().InsertAt(0, save);
	restored.GetInstructions().InsertAt(0, set_mask);
	renumber(restored);
	restored.GetLabels().Add(ShaderLabel(restored.GetInstructions()[2]));
	if (!validate(restored, input)) return false;
	auto literal_restore = restored;
	literal_restore.GetInstructions()[4].src[0] = zero_mask;
	literal_restore.GetInstructions()[4].src[0].constant.u = 1;
	if (!validate(literal_restore, input)) return false;
	literal_restore.GetInstructions()[4].src[0].size = 0;
	if (ShaderRectListProfileSupported(literal_restore, input)) return false;
	for (int mutation = 0; mutation < 5; ++mutation)
	{
		auto bad = restored;
		if (mutation == 0) { bad.GetInstructions()[0] = branch; renumber(bad); }
		if (mutation == 1) { bad.GetInstructions()[3] = set_mask; bad.GetInstructions()[3].dst = scalar(41, 1); renumber(bad); }
		if (mutation == 2) bad.GetInstructions()[5].type = ShaderInstructionType::VCmpxEqU32;
		if (mutation == 3) bad.GetInstructions()[5].dst2.type = ShaderOperandType::ExecHi;
		if (mutation == 4)
		{
			auto masked = restore;
			masked.src[0] = zero_mask;
			bad.GetInstructions().InsertAt(16, masked);
			renumber(bad);
		}
		if (ShaderRectListProfileSupported(bad, input)) return false;
	}
	// Z is one lane of a scalar constant load, with a distinct descriptor.
	auto uniform = code;
	auto uniform_input = input;
	uniform_input.bind.storage_buffers.buffers_num = 2;
	uniform_input.bind.storage_buffers.start_register[1] = 12;
	uniform_input.bind.storage_buffers.usages[1] = ShaderStorageUsage::Constant;
	ShaderCalcBindingIndices(&uniform_input.bind);
	uniform.GetInstructions()[9].src[0] = scalar(20);
	ShaderInstruction load;
	load.type = ShaderInstructionType::SBufferLoadDwordx4; load.format = ShaderInstructionFormat::Sdst4SvSoffset;
	load.dst = scalar(18, 4); load.src_num = 2; load.src[0] = scalar(12, 4);
	load.src[1].type = ShaderOperandType::IntegerInlineConstant;
	uniform.GetInstructions().InsertAt(9, load);
	renumber(uniform);
	if (!validate(uniform, uniform_input)) return false;
	for (int mutation = 0; mutation < 4; ++mutation)
	{
		auto bad = uniform;
		auto bad_input = uniform_input;
		if (mutation == 0) bad_input.bind.storage_buffers.usages[1] = ShaderStorageUsage::ReadOnly;
		if (mutation == 1) bad.GetInstructions()[9].src[1] = scalar(3);
		if (mutation == 2) bad.GetInstructions()[9].dst.size = 2;
		if (mutation == 3) bad.GetInstructions()[0].dst2 = scalar(11, 4);
		if (ShaderRectListProfileSupported(bad, bad_input)) return false;
	}
	constexpr uint32_t shader_fixture[] = {0xBEEB03FFu, 0, 0, 0, 0x100u, 0, 0xa4537592u, 0xc33cbd00u, 0x4c554d45u};
	HW::VertexShaderInfo regs {};
	regs.vs_regs.data_addr = reinterpret_cast<uint64_t>(shader_fixture);
	input.rect_list = false;
	const auto ordinary_a = ShaderGetIdVS(&regs, &input);
	input.rect_list = true;
	const auto rectangle = ShaderGetIdVS(&regs, &input);
	input.rect_list = false;
	const auto ordinary_b = ShaderGetIdVS(&regs, &input);
	if (ordinary_a == rectangle || ordinary_a != ordinary_b) return false;
	return true;
}

bool CheckRectListSelector()
{
	const char* expected = std::getenv("LUMEN_EXPECT_EXPERIMENTAL_GFX_RECT_LIST");
	if (expected == nullptr) return true;
	const std::string_view oracle(expected);
	if (oracle != "0" && oracle != "1") return false;
	const bool enabled = Lumen::Libs::Graphics::ShaderRectListExperimentEnabled();
	if (enabled != (oracle == "1")) return false;
#if defined(_WIN32)
	const int result = _putenv_s("LUMEN_EXPERIMENTAL_GFX_RECT_LIST", enabled ? "0" : "1");
#else
	const int result = setenv("LUMEN_EXPERIMENTAL_GFX_RECT_LIST", enabled ? "0" : "1", 1);
#endif
	return result == 0 && Lumen::Libs::Graphics::ShaderRectListExperimentEnabled() == enabled;
}

bool CheckFp32PreCacheGate()
{
	using Lumen::Libs::Graphics::HW::PixelShaderInfo;
	using Lumen::Libs::Graphics::HW::VertexShaderInfo;

	VertexShaderInfo vertex {};
	PixelShaderInfo  pixel {};
	vertex.vs_embedded = true;
	pixel.ps_embedded  = true;
	if (!Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}

	vertex.vs_embedded              = false;
	vertex.vs_regs.data_addr        = 1;
	vertex.vs_regs.rsrc1.float_mode = 192;
	vertex.vs_regs.rsrc1.ieee_mode  = false;
	if (!Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}
	vertex.vs_regs.rsrc1.float_mode = 0;
	if (Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}
	vertex.vs_regs.rsrc1.float_mode = 192;
	vertex.vs_regs.rsrc1.ieee_mode  = true;
	if (Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}
	vertex.vs_regs.rsrc1.ieee_mode = false;
	if (!Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}

	vertex.vs_regs.data_addr        = 0;
	vertex.gs_regs.data_addr        = 1;
	vertex.gs_regs.rsrc1.float_mode = 192;
	vertex.gs_regs.rsrc1.ieee_mode  = false;
	if (!Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}
	vertex.gs_regs.rsrc1.float_mode = 0;
	if (Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}

	vertex.vs_embedded             = true;
	pixel.ps_embedded              = false;
	pixel.ps_regs.rsrc1.float_mode = 192;
	pixel.ps_regs.rsrc1.ieee_mode  = false;
	if (!Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}
	pixel.ps_regs.rsrc1.ieee_mode = true;
	if (Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}
	pixel.ps_regs.rsrc1.ieee_mode  = false;
	pixel.ps_regs.rsrc1.float_mode = 0;
	if (Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel))
	{
		return false;
	}
	pixel.ps_regs.rsrc1.float_mode = 192;
	return Lumen::Libs::Graphics::ShaderGraphicsFp32StateSupported(vertex, pixel);
}

bool CheckDx10ClampCacheKey()
{
	constexpr uint32_t shader_fixture[] = {
	    0xBEEB03FFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000100u, 0x00000000u, 0xa4537592u, 0xc33cbd00u, 0x4c554d45u,
	};

	Lumen::Libs::Graphics::HW::VertexShaderInfo vertex {};
	vertex.vs_regs.data_addr        = reinterpret_cast<uint64_t>(shader_fixture);
	vertex.vs_regs.rsrc1.dx10_clamp = true;
	Lumen::Libs::Graphics::ShaderVertexInputInfo vertex_input {};
	const auto                                   vertex_enabled_a = Lumen::Libs::Graphics::ShaderGetIdVS(&vertex, &vertex_input);
	vertex.vs_regs.rsrc1.dx10_clamp                               = false;
	const auto vertex_disabled                                    = Lumen::Libs::Graphics::ShaderGetIdVS(&vertex, &vertex_input);
	vertex.vs_regs.rsrc1.dx10_clamp                               = true;
	const auto vertex_enabled_b                                   = Lumen::Libs::Graphics::ShaderGetIdVS(&vertex, &vertex_input);

	Lumen::Libs::Graphics::HW::PixelShaderInfo pixel {};
	pixel.ps_regs.data_addr        = reinterpret_cast<uint64_t>(shader_fixture);
	pixel.ps_regs.rsrc1.dx10_clamp = true;
	ShaderPixelInputInfo pixel_input {};
	const auto           pixel_enabled_a = Lumen::Libs::Graphics::ShaderGetIdPS(&pixel, &pixel_input);
	pixel.ps_regs.rsrc1.dx10_clamp       = false;
	const auto pixel_disabled            = Lumen::Libs::Graphics::ShaderGetIdPS(&pixel, &pixel_input);
	pixel.ps_regs.rsrc1.dx10_clamp       = true;
	const auto pixel_enabled_b           = Lumen::Libs::Graphics::ShaderGetIdPS(&pixel, &pixel_input);

	const bool vertex_legacy_fields_unchanged = vertex_enabled_a.hash0 == vertex_disabled.hash0 &&
	                                            vertex_enabled_a.crc32 == vertex_disabled.crc32 &&
	                                            vertex_enabled_a.ids == vertex_disabled.ids;
	const bool pixel_legacy_fields_unchanged  = pixel_enabled_a.hash0 == pixel_disabled.hash0 &&
	                                            pixel_enabled_a.crc32 == pixel_disabled.crc32 && pixel_enabled_a.ids == pixel_disabled.ids;
	if (vertex_enabled_a == vertex_disabled || vertex_enabled_a != vertex_enabled_b || pixel_enabled_a == pixel_disabled ||
	    pixel_enabled_a != pixel_enabled_b || !vertex_legacy_fields_unchanged || !pixel_legacy_fields_unchanged)
	{
		std::fprintf(stderr, "DX10 clamp shader-cache key does not preserve A-B-A isolation\n");
		return false;
	}
	return true;
}

bool CheckVRsqClampF32CacheIdentity()
{
	constexpr uint32_t shader_fixture[] = {
	    0xBEEB03FFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000100u,
	    0x00000000u, 0xa4537592u, 0xc33cbd00u, 0x4c554d45u,
	};
	Lumen::Libs::Graphics::HW::VertexShaderInfo regs {};
	regs.vs_regs.data_addr = reinterpret_cast<uint64_t>(shader_fixture);
	ShaderVertexInputInfo input {};
	const auto            id = Lumen::Libs::Graphics::ShaderGetIdVS(&regs, &input);
	constexpr uint32_t    profile_revision = 0x52535131u;
	const bool            keyed = id.ids.Size() > 1 && id.ids.At(1) == profile_revision;
	const bool            enabled = Lumen::Libs::Graphics::ShaderVRsqClampF32ExperimentEnabled();
	if (keyed != enabled)
	{
		std::fprintf(stderr, "V_RSQ_CLAMP_F32 diagnostic profile is missing from the shader-cache identity\n");
		return false;
	}
	return true;
}

bool CheckImplicitSampleLodZeroCacheIdentity()
{
	constexpr uint32_t shader_fixture[] = {
	    0xBEEB03FFu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000100u,
	    0x00000000u, 0xa4537592u, 0xc33cbd00u, 0x4c554d45u,
	};
	Lumen::Libs::Graphics::HW::PixelShaderInfo regs {};
	regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(shader_fixture);
	ShaderPixelInputInfo sampled_input {};
	sampled_input.bind.textures2D.textures_num           = 1;
	sampled_input.bind.textures2D.textures2d_sampled_num = 1;
	sampled_input.bind.textures2D.desc[0].usage          = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	sampled_input.bind.textures2D.desc[0].texture.fields[3] =
	    static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u;
	sampled_input.bind.samplers.samplers_num = 1;
	ShaderPixelInputInfo ordinary_input {};
	const auto sampled_id  = Lumen::Libs::Graphics::ShaderGetIdPS(&regs, &sampled_input);
	const auto ordinary_id = Lumen::Libs::Graphics::ShaderGetIdPS(&regs, &ordinary_input);
	constexpr uint32_t profile_revision = 0x494c5a31u;
	const auto has_profile = [=](const auto& id)
	{
		for (const auto value: id.ids)
		{
			if (value == profile_revision) return true;
		}
		return false;
	};
	const bool enabled = Lumen::Libs::Graphics::ShaderImplicitSampleLodZeroExperimentEnabled();
	if (has_profile(sampled_id) != enabled || has_profile(ordinary_id) || (enabled && sampled_id == ordinary_id))
	{
		std::fprintf(stderr, "implicit ImageSample LOD-zero shader-cache identity is not isolated\n");
		return false;
	}
	return true;
}

bool CheckTextureTypeCacheKey()
{
	constexpr uint32_t shader_fixture[] = {
	    0xBEEB03FFu, 0x00000000u,              // binary marker and zero usage slots
	    0x00000000u, 0x00000000u,              // signature and version
	    0x00000100u, 0x00000000u,              // one-dword length and usage metadata
	    0xa4537592u, 0xc33cbd00u, 0x4c554d45u, // hash0, hash1 and crc32
	};
	Lumen::Libs::Graphics::HW::ComputeShaderInfo regs {};
	regs.cs_regs.data_addr = reinterpret_cast<uint64_t>(shader_fixture);
	ShaderComputeInputInfo input {};
	input.bind.textures2D.textures_num              = 1;
	input.bind.textures2D.textures2d_sampled_num    = 1;
	input.bind.textures2D.desc[0].usage             = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	input.bind.textures2D.desc[0].texture.fields[0] = 0x100u;
	input.bind.textures2D.desc[0].texture.fields[3] = static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_8_COMPAT)
	                                                  << 28u;
	const auto type8_id                             = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &input);
	input.bind.textures2D.desc[0].texture.fields[0] = 0x200u;
	const auto different_base_id                    = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &input);
	input.bind.textures2D.desc[0].texture.fields[3] = static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D) << 28u;
	const auto                                 type2d_id = Lumen::Libs::Graphics::ShaderGetIdCS(&regs, &input);
	Lumen::Libs::Graphics::HW::PixelShaderInfo pixel_regs {};
	pixel_regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(shader_fixture);
	ShaderPixelInputInfo pixel_input {};
	pixel_input.bind.textures2D.textures_num              = 1;
	pixel_input.bind.textures2D.textures2d_sampled_num    = 1;
	pixel_input.bind.textures2D.desc[0].usage             = Lumen::Libs::Graphics::ShaderTextureUsage::ReadOnly;
	pixel_input.bind.textures2D.desc[0].texture.fields[3] = static_cast<uint32_t>(Lumen::Libs::Graphics::ShaderTextureResource::TYPE_2D)
	                                                        << 28u;
	const auto pixel_type2d_id                  = Lumen::Libs::Graphics::ShaderGetIdPS(&pixel_regs, &pixel_input);
	pixel_input.bind.textures2D.desc[0].texture = {};
	const auto pixel_null_sampled_id            = Lumen::Libs::Graphics::ShaderGetIdPS(&pixel_regs, &pixel_input);
	if (type8_id != different_base_id || type8_id == type2d_id || pixel_null_sampled_id != pixel_type2d_id)
	{
		std::fprintf(stderr, "shader cache key does not separate generated texture types or canonicalize null sampled images\n");
		return false;
	}
	return true;
}

bool CheckPixelShaderOutputModeCacheKey()
{
	// Minimal Gen4 shader-binary envelope. ShaderGetIdPS reads the embedded
	// metadata to build the cache key; the payload itself is not parsed here.
	constexpr uint32_t ps4_shader_fixture[] = {
	    0xBEEB03FFu, 0x00000000u, // binary marker and zero usage slots
	    0x00000000u, 0x00000000u, // signature and version
	    0x00000100u, 0x00000000u, // one-dword length and usage metadata
	    0xa4537592u, 0xc33cbd00u, 0x4c554d45u, // hash0, hash1 and crc32
	};
	Lumen::Libs::Graphics::HW::PixelShaderInfo regs {};
	regs.ps_regs.data_addr = reinterpret_cast<uint64_t>(ps4_shader_fixture);
	ShaderPixelInputInfo mode4_mask3 {};
	ShaderPixelInputInfo mode4_maskf {};
	ShaderPixelInputInfo mode9_maskf {};
	mode4_mask3.target_output_mode[0] = 4;
	mode4_mask3.shader_mask           = 0x3u;
	mode4_maskf.target_output_mode[0] = 4;
	mode4_maskf.shader_mask           = 0xfu;
	mode9_maskf.target_output_mode[0] = 9;
	mode9_maskf.shader_mask           = 0xfu;
	const auto id4_mask3              = Lumen::Libs::Graphics::ShaderGetIdPS(&regs, &mode4_mask3);
	const auto id4_maskf              = Lumen::Libs::Graphics::ShaderGetIdPS(&regs, &mode4_maskf);
	const auto id9_maskf              = Lumen::Libs::Graphics::ShaderGetIdPS(&regs, &mode9_maskf);
	if (id4_mask3 == id4_maskf || id4_maskf == id9_maskf || id4_mask3 == id9_maskf)
	{
		std::fprintf(stderr, "pixel shader cache key aliases distinct output mode/mask states\n");
		return false;
	}
	return true;
}

bool CheckNullSampledImageSelector()
{
	const char* expected_value = std::getenv("LUMEN_EXPECT_GFX_NULL_SAMPLED_IMAGE");
	if (expected_value == nullptr)
	{
		return true;
	}
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1")
	{
		std::fprintf(stderr, "invalid null sampled-image selector oracle: %s\n", expected_value);
		return false;
	}
	const bool requested = Lumen::Libs::Graphics::ShaderNullSampledImageEnabled();
	if (requested != (expected == "1"))
	{
		std::fprintf(stderr, "null sampled-image selector requested=%u expected=%s\n", requested ? 1u : 0u, expected_value);
		return false;
	}
	return true;
}

bool CheckDx10ClampZeroSelector()
{
	constexpr const char* selector       = "LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO";
	const char*           expected_value = std::getenv("LUMEN_EXPECT_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO");
	if (expected_value == nullptr)
	{
		return true;
	}
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1")
	{
		std::fprintf(stderr, "invalid DX10 clamp selector oracle: %s\n", expected_value);
		return false;
	}

	const bool requested = Lumen::Libs::Graphics::ShaderDx10ClampZeroExperimentEnabled();
	if (requested != (expected == "1"))
	{
		std::fprintf(stderr, "DX10 clamp selector requested=%u expected=%s\n", requested ? 1u : 0u, expected_value);
		return false;
	}

#if defined(_WIN32)
	const int mutation_result = _putenv_s(selector, requested ? "0" : "1");
#else
	const int mutation_result = setenv(selector, requested ? "0" : "1", 1);
#endif
	if (mutation_result != 0 || Lumen::Libs::Graphics::ShaderDx10ClampZeroExperimentEnabled() != requested)
	{
		std::fprintf(stderr, "DX10 clamp selector is not process-latched\n");
		return false;
	}
	return true;
}

bool CheckImplicitSampleLodZeroSelector()
{
	constexpr const char* selector = "LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO";
	const char* expected_value = std::getenv("LUMEN_EXPECT_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO");
	if (expected_value == nullptr)
	{
		return true;
	}
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1")
	{
		std::fprintf(stderr, "invalid implicit ImageSample LOD-zero selector oracle: %s\n", expected_value);
		return false;
	}

	const bool requested = Lumen::Libs::Graphics::ShaderImplicitSampleLodZeroExperimentEnabled();
	if (requested != (expected == "1"))
	{
		std::fprintf(stderr, "implicit ImageSample LOD-zero selector requested=%u expected=%s\n", requested ? 1u : 0u,
		             expected_value);
		return false;
	}

#if defined(_WIN32)
	const int mutation_result = _putenv_s(selector, requested ? "0" : "1");
#else
	const int mutation_result = setenv(selector, requested ? "0" : "1", 1);
#endif
	if (mutation_result != 0 || Lumen::Libs::Graphics::ShaderImplicitSampleLodZeroExperimentEnabled() != requested)
	{
		std::fprintf(stderr, "implicit ImageSample LOD-zero selector is not process-latched\n");
		return false;
	}
	return true;
}

bool CheckVMacLegacyF32Selector()
{
	constexpr const char* selector       = "LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32";
	const char*           expected_value = std::getenv("LUMEN_EXPECT_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32");
	if (expected_value == nullptr)
	{
		return true;
	}
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1")
	{
		std::fprintf(stderr, "invalid V_MAC_LEGACY_F32 selector oracle: %s\n", expected_value);
		return false;
	}

	const bool requested = Lumen::Libs::Graphics::ShaderVMacLegacyF32ExperimentEnabled();
	if (requested != (expected == "1") || VMacLegacyF32CandidateAdmitted() != requested)
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 selector requested=%u admitted=%u expected=%s\n", requested ? 1u : 0u,
		             VMacLegacyF32CandidateAdmitted() ? 1u : 0u, expected_value);
		return false;
	}

#if defined(_WIN32)
	const int mutation_result = _putenv_s(selector, requested ? "0" : "1");
#else
	const int mutation_result = setenv(selector, requested ? "0" : "1", 1);
#endif
	if (mutation_result != 0 || Lumen::Libs::Graphics::ShaderVMacLegacyF32ExperimentEnabled() != requested ||
	    VMacLegacyF32CandidateAdmitted() != requested)
	{
		std::fprintf(stderr, "V_MAC_LEGACY_F32 selector is not process-latched\n");
		return false;
	}
	return true;
}

bool CheckVRsqClampF32Selector()
{
	constexpr const char* selector       = "LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32";
	const char*           expected_value = std::getenv("LUMEN_EXPECT_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32");
	if (expected_value == nullptr)
	{
		return true;
	}
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1")
	{
		std::fprintf(stderr, "invalid V_RSQ_CLAMP_F32 selector oracle: %s\n", expected_value);
		return false;
	}

	const bool requested = Lumen::Libs::Graphics::ShaderVRsqClampF32ExperimentEnabled();
	if (requested != (expected == "1") || VRsqClampF32CandidateAdmitted() != requested)
	{
		std::fprintf(stderr, "V_RSQ_CLAMP_F32 selector requested=%u admitted=%u expected=%s\n", requested ? 1u : 0u,
		             VRsqClampF32CandidateAdmitted() ? 1u : 0u, expected_value);
		return false;
	}

#if defined(_WIN32)
	const int mutation_result = _putenv_s(selector, requested ? "0" : "1");
#else
	const int mutation_result = setenv(selector, requested ? "0" : "1", 1);
#endif
	if (mutation_result != 0 || Lumen::Libs::Graphics::ShaderVRsqClampF32ExperimentEnabled() != requested ||
	    VRsqClampF32CandidateAdmitted() != requested)
	{
		std::fprintf(stderr, "V_RSQ_CLAMP_F32 selector is not process-latched\n");
		return false;
	}
	return true;
}

bool CheckTextureMipAssemblySelector()
{
	constexpr const char* selector       = "LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY";
	const char*           expected_value = std::getenv("LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY");
	if (expected_value == nullptr)
	{
		return true;
	}
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1")
	{
		std::fprintf(stderr, "invalid texture mip assembly selector oracle: %s\n", expected_value);
		return false;
	}
	if (std::getenv("LUMEN_FRAME_PIPELINE_TRACE") != nullptr ||
	    std::getenv("LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT") != nullptr)
	{
		std::fprintf(stderr, "texture mip assembly selector test requires frame trace and render snapshot selector to be unset\n");
		return false;
	}

	const bool requested = Lumen::Libs::Graphics::GpuMemoryTextureMipSnapshotEnabled();
	const bool tracking  = Lumen::Libs::Graphics::GpuMemoryAccessTrackingEnabled();
	if (requested != (expected == "1") || tracking != requested)
	{
		std::fprintf(stderr, "texture mip assembly selector requested=%u tracking=%u expected=%s trace=unset\n",
		             requested ? 1u : 0u, tracking ? 1u : 0u, expected_value);
		return false;
	}

#if defined(_WIN32)
	const int mutation_result = _putenv_s(selector, requested ? "0" : "1");
#else
	const int mutation_result = setenv(selector, requested ? "0" : "1", 1);
#endif
	if (mutation_result != 0 || Lumen::Libs::Graphics::GpuMemoryTextureMipSnapshotEnabled() != requested ||
	    Lumen::Libs::Graphics::GpuMemoryAccessTrackingEnabled() != tracking)
	{
		std::fprintf(stderr, "texture mip assembly selector or access tracking is not process-latched\n");
		return false;
	}
	std::fprintf(stderr, "[GFX_TEXTURE_MIP_ASSEMBLY_SELECTOR] requested=%u tracking=%u trace=unset latched=1 status=clean\n",
	             requested ? 1u : 0u, tracking ? 1u : 0u);
	return true;
}

bool CheckRenderTextureSnapshotSelector()
{
	constexpr const char* selector = "LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT";
	const char* expected_value = std::getenv("LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT");
	if (expected_value == nullptr) return true;
	const std::string_view expected(expected_value);
	if ((expected != "0" && expected != "1") || std::getenv("LUMEN_FRAME_PIPELINE_TRACE") != nullptr ||
	    std::getenv("LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY") != nullptr)
	{
		std::fprintf(stderr, "render snapshot selector test requires a binary oracle with trace and mip selector unset\n");
		return false;
	}
	const bool requested = Lumen::Libs::Graphics::GpuMemoryRenderTextureSnapshotEnabled();
	const bool tracking = Lumen::Libs::Graphics::GpuMemoryAccessTrackingEnabled();
	if (requested != (expected == "1") || tracking != requested) return false;
#if defined(_WIN32)
	const int mutation_result = _putenv_s(selector, requested ? "0" : "1");
#else
	const int mutation_result = setenv(selector, requested ? "0" : "1", 1);
#endif
	if (mutation_result != 0 || Lumen::Libs::Graphics::GpuMemoryRenderTextureSnapshotEnabled() != requested ||
	    Lumen::Libs::Graphics::GpuMemoryAccessTrackingEnabled() != tracking) return false;
	std::fprintf(stderr, "[GFX_RENDER_TEXTURE_SNAPSHOT_SELECTOR] requested=%u tracking=%u trace=unset mip=unset latched=1 status=clean\n",
	             requested ? 1u : 0u, tracking ? 1u : 0u);
	return true;
}

bool CheckGpuR8CaptureSelector()
{
	const char* expected_value = std::getenv("LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_R8");
	if (expected_value == nullptr) { return true; }
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1") { return false; }
	const bool enabled = Lumen::Libs::Graphics::GraphicsRenderGpuR8CaptureEnabled();
	if (enabled != (expected == "1"))
	{
		std::fprintf(stderr, "GPU R8 capture selector requested=%u expected=%s\n", enabled ? 1u : 0u, expected_value);
		return false;
	}
#if defined(_WIN32)
	const int changed = _putenv_s("LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8", enabled ? "0" : "1");
#else
	const int changed = setenv("LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8", enabled ? "0" : "1", 1);
#endif
	if (changed != 0 || Lumen::Libs::Graphics::GraphicsRenderGpuR8CaptureEnabled() != enabled) { return false; }
	std::fprintf(stderr, "[GFX_GPU_R8_CAPTURE_SELECTOR] requested=%u latched=1 status=clean\n", enabled ? 1u : 0u);
	return true;
}

bool CheckGpuRgba8MipsCaptureSelector()
{
	const char* expected_value = std::getenv("LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS");
	if (expected_value == nullptr) { return true; }
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1") { return false; }
	const bool enabled = Lumen::Libs::Graphics::GraphicsRenderGpuRgba8MipsCaptureEnabled();
	if (enabled != (expected == "1"))
	{
		std::fprintf(stderr, "GPU RGBA8 mip capture selector requested=%u expected=%s\n", enabled ? 1u : 0u,
		             expected_value);
		return false;
	}
#if defined(_WIN32)
	const int changed = _putenv_s("LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS", enabled ? "0" : "1");
#else
	const int changed = setenv("LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS", enabled ? "0" : "1", 1);
#endif
	if (changed != 0 || Lumen::Libs::Graphics::GraphicsRenderGpuRgba8MipsCaptureEnabled() != enabled) { return false; }
	std::fprintf(stderr, "[GFX_GPU_RGBA8_MIPS_CAPTURE_SELECTOR] requested=%u latched=1 status=clean\n", enabled ? 1u : 0u);
	return true;
}

bool CheckDynamicVertexHostVisibleSelector()
{
	const char* expected_value = std::getenv("LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE");
	if (expected_value == nullptr)
	{
		return true;
	}
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1")
	{
		std::fprintf(stderr, "invalid dynamic vertex host-visible selector oracle: %s\n", expected_value);
		return false;
	}
	const bool requested = Lumen::Libs::Graphics::VertexBufferDynamicHostVisibleEnabled();
	if (requested != (expected == "1"))
	{
		std::fprintf(stderr, "dynamic vertex host-visible selector requested=%u expected=%s\n", requested ? 1u : 0u,
		             expected_value);
		return false;
	}
	return true;
}

bool CheckExactReuseFastPathSelector()
{
	const char* expected_value = std::getenv("LUMEN_EXPECT_GFX_EXACT_REUSE_FAST_PATH");
	if (expected_value == nullptr)
	{
		if (std::getenv("LUMEN_GFX_EXACT_REUSE_FAST_PATH") != nullptr) return true;
#if ((LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID) && \
     LUMEN_ABI == LUMEN_ABI_ARM64_V8A) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
		expected_value = "1";
#else
		expected_value = "0";
#endif
	}
	const std::string_view expected(expected_value);
	if (expected != "0" && expected != "1") return false;
	const bool enabled = Lumen::Libs::Graphics::GpuMemoryExactReuseFastPathEnabled();
	if (enabled != (expected == "1")) return false;
#if defined(_WIN32)
	const int changed = _putenv_s("LUMEN_GFX_EXACT_REUSE_FAST_PATH", enabled ? "0" : "1");
#else
	const int changed = setenv("LUMEN_GFX_EXACT_REUSE_FAST_PATH", enabled ? "0" : "1", 1);
#endif
	return changed == 0 && Lumen::Libs::Graphics::GpuMemoryExactReuseFastPathEnabled() == enabled;
}

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
bool CheckWindowsGuestMemoryRead()
{
	struct Allocation
	{
		explicit Allocation(size_t size): data(static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {}
		Allocation(const Allocation&) = delete;
		Allocation& operator=(const Allocation&) = delete;
		~Allocation() { Release(); }
		bool Release()
		{
			if (data != nullptr && VirtualFree(data, 0, MEM_RELEASE) == 0) return false;
			data = nullptr;
			return true;
		}
		uint8_t* data;
	};
	const auto fail = [](const char* test) {
		std::fprintf(stderr, "Windows guest-memory read contract failed: %s\n", test);
		return false;
	};
	SYSTEM_INFO system_info {};
	GetSystemInfo(&system_info);
	const size_t page_size = system_info.dwPageSize;
	if (page_size < 64 || page_size > SIZE_MAX / 130) return fail("page size");
	const size_t size = page_size * 130;
	Allocation source(size);
	if (source.data == nullptr) return fail("allocation");
	std::vector<uint8_t> expected(size);
	for (size_t i = 0; i < size; ++i) expected[i] = static_cast<uint8_t>((i * 17 + i / page_size * 31) & 0xffu);
	std::memcpy(source.data, expected.data(), size);
	const auto address = reinterpret_cast<uint64_t>(source.data);
	constexpr size_t canary_size = 32;
	constexpr uint8_t canary = 0xa5;
	// Allocate once before any source is released; the freed-address case must
	// not accidentally read a new allocation at the same virtual address.
	std::vector<uint8_t> destination(size + 2 * canary_size, canary);
	const auto read = [&](const char* test, uint64_t from, size_t count, bool success, const uint8_t* bytes = nullptr) {
		std::memset(destination.data(), canary, destination.size());
		const bool result = Lumen::Loader::ReadGuestMemory(from, destination.data() + canary_size, count);
		if (result != success) return fail(test);
		for (size_t i = 0; i < canary_size; ++i)
		{
			if (destination[i] != canary || destination[canary_size + count + i] != canary) return fail(test);
		}
		// Failure permits a partial snapshot, but never writes outside the span.
		if (success && std::memcmp(destination.data() + canary_size, bytes, count) != 0) return fail(test);
		return true;
	};
	const auto protect = [&](size_t page, DWORD protection) {
		DWORD previous = 0;
		return VirtualProtect(source.data + page * page_size, page_size, protection, &previous) != 0;
	};
	if (!read("130 readable pages", address, size, true, expected.data()) ||
	    !read("unaligned 130-page span", address + 17, size - 34, true, expected.data() + 17)) return false;

	for (DWORD protection: {PAGE_READWRITE, PAGE_READONLY, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE})
	{
		if (!protect(64, protection)) return fail("readable protection");
		if (!read("readable protection bytes", address + 64 * page_size + 17, page_size - 34, true,
		          expected.data() + 64 * page_size + 17) ||
		    !read("mixed readable pages", address + 17, size - 34, true, expected.data() + 17)) return false;
	}
	if (!protect(64, PAGE_READWRITE)) return fail("restore readable protection");

	// Invalid pages at and beyond the 64-page boundary must reject the whole
	// request, including when only the final byte lies on the invalid page.
	for (size_t page: {size_t(63), size_t(64), size_t(129)})
	{
		for (DWORD protection: {PAGE_EXECUTE, PAGE_NOACCESS, PAGE_READWRITE | PAGE_GUARD})
		{
			if (!protect(page, protection)) return fail("unreadable protection");
			const auto read_invalid = [&](const char* test, uint64_t from, size_t count) {
				if (!read(test, from, count, false)) return false;
				if ((protection & PAGE_GUARD) != 0)
				{
					MEMORY_BASIC_INFORMATION region {};
					if (VirtualQuery(source.data + page * page_size, &region, sizeof(region)) != sizeof(region) ||
					    region.Protect != protection) return fail("guard must remain armed");
				}
				return true;
			};
			if (!read_invalid("unreadable page", address + page * page_size + 17, 32) ||
			    !read_invalid("unreadable partial tail", address + page * page_size - 17, 18) ||
			    !read_invalid("unreadable page in long span", address + 17, size - 34)) return false;
			if (!protect(page, PAGE_READWRITE)) return fail("restore unreadable protection");
		}
	}
	if (!read("readable after protection changes", address, size, true, expected.data())) return false;

	if (VirtualFree(source.data + 64 * page_size, page_size, MEM_DECOMMIT) == 0) return fail("decommit");
	if (!read("decommitted page", address + 64 * page_size, 32, false) ||
	    !read("decommitted partial tail", address + 64 * page_size - 17, 18, false) ||
	    !read("decommitted page in long span", address, size, false)) return false;
	if (VirtualAlloc(source.data + 64 * page_size, page_size, MEM_COMMIT, PAGE_READWRITE) != source.data + 64 * page_size)
		return fail("recommit");
	std::memcpy(source.data + 64 * page_size, expected.data() + 64 * page_size, page_size);
	if (!read("readable after recommit", address, size, true, expected.data())) return false;

	// Untouched committed pages are readable even before entering the working set.
	Allocation cold(page_size * 3);
	if (cold.data == nullptr) return fail("cold allocation");
	const std::vector<uint8_t> zeroes(page_size * 3, 0);
	if (!read("untouched committed pages", reinterpret_cast<uint64_t>(cold.data), zeroes.size(), true, zeroes.data())) return false;
	if (Lumen::Loader::ReadGuestMemory(address, nullptr, 32)) return fail("null destination");
	if (!read("null source", 0, 32, false) || !read("empty span", address, 0, false) ||
	    !read("address overflow", UINT64_MAX - 7, 16, false) || !read("invalid address", UINT64_MAX, 1, false)) return false;
	if (!source.Release()) return fail("release");
	if (!read("freed page", address, 32, false)) return false;
	std::fprintf(stderr, "[WINDOWS_GUEST_MEMORY_READ] pages=130 protections=checked guard=preserved bounds=checked status=clean\n");
	return true;
}
#endif

} // namespace

int main(int argc, char** argv)
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (argc == 2 && std::string_view(argv[1]) == "--core-ref-counter-probe")
		return Lumen::Core::CoreRefCounterContractTest() ? 0 : 1;
	if (argc == 2 && std::string_view(argv[1]) == "--backing-query-cache-probe")
		return Lumen::Libs::Graphics::BackingQueryCacheContractTest() ? 0 : 1;
	if (argc == 2 && std::string_view(argv[1]) == "--native-memory-claims-probe")
		return Lumen::GuestCpu::NativeMemoryClaimsContractTest() ? 0 : 1;
	if (argc == 2 && std::string_view(argv[1]) == "--native-heap-concurrency-probe")
		return Lumen::Core::NativeHeapConcurrencyContractTest() ? 0 : 1;
	if (argc == 2 && std::string_view(argv[1]) == "--compiled-programs-probe")
	{
		const bool passed = Lumen::Libs::Graphics::CompiledProgramCacheContractTest() &&
		    Lumen::Libs::Graphics::ShaderCompiledProgramsContractTest();
		std::fprintf(stderr, "CompiledProgramsTest: %s\n", passed ? "PASS" : "FAIL");
		return passed ? 0 : 1;
	}
#if defined(LUMEN_FEXCORE_RUNTIME)
	if (LumenAndroidFexCoreRuntimeProbeV1(nullptr) != LUMEN_ANDROID_FEXCORE_RUNTIME_READY_V1) return 1;
#endif
#ifdef LUMEN_OWNED_PREPARATION_FEX_TEST
	if (argc == 2 && std::string_view(argv[1]) == "--owned-preparation-fex-test") return GraphicsOwnedPreparationFexTest();
	if (argc == 2 && std::string_view(argv[1]) == "--owned-draw-inputs-probe")
	{
		const bool passed = Lumen::Libs::Graphics::ShaderOwnedDrawInputsContractTest() &&
		    Lumen::Libs::Graphics::GraphicsRunParserPauseProofContractTest();
		std::fprintf(stderr, "OwnedDrawInputsTest: %s\n", passed ? "PASS" : "FAIL");
		return passed ? 0 : 1;
	}
#endif
#ifdef LUMEN_GRAPHICS_UPLOAD_STAGING_PROBE
	if (argc == 2 && std::string_view(argv[1]) == "--upload-staging-probe") return GraphicsUploadStagingVulkanProbe();
#endif
#ifdef LUMEN_GRAPHICS_PIXEL_DISCARD_DEPTH_PROBE
	if (argc == 2 && std::string_view(argv[1]) == "--pixel-discard-depth-probe") return GraphicsPixelDiscardDepthVulkanProbe();
	if (argc == 2 && std::string_view(argv[1]) == "--stencil-clear-probe") return GraphicsStencilClearVulkanProbe();
	if (argc == 2 && std::string_view(argv[1]) == "--color-readback-rows-probe") return GraphicsColorReadbackRowsVulkanProbe();
	if (argc == 2 && std::string_view(argv[1]) == "--completion-pass-reuse-probe") return GraphicsCompletionPassReuseVulkanProbe();
#endif
	if (argc == 3 && std::string_view(argv[1]) == "--rect-list-spv-out") return CheckRectListPipeline(argv[2]) ? 0 : 1;
	if (argc == 4 && std::string_view(argv[1]) == "--rect-list-inspect") return InspectRectListCapture(argv[2], argv[3]);
	if (argc == 2 && std::string_view(argv[1]) == "--label-destination-probe")
		return Lumen::Libs::Graphics::GraphicsRunDeferredLabelCaptureScopeContractTest() &&
		       Lumen::Libs::Graphics::LabelDestinationContractTest() ? 0 : 1;
	if (argc == 2 && std::string_view(argv[1]) == "--eop-virtual-clock-probe")
		return Lumen::Libs::Graphics::GpuMemoryEopVirtualClockContractTest() ? 0 : 1;
	if (argc == 2 && std::string_view(argv[1]) == "--gds-destination-probe")
		return Lumen::Libs::Graphics::GraphicsRunDmaDataGdsReadContractTest() &&
		       Lumen::Libs::Graphics::GraphicsRenderGdsReadDestinationContractTest() ? 0 : 1;
	if (argc == 2 && (std::string_view(argv[1]) == "--gpu-draw-access-plan-probe" ||
	                  std::string_view(argv[1]) == "--gpu-draw-access-plan-untracked-probe"))
	{
		const bool expected_tracking = std::string_view(argv[1]) == "--gpu-draw-access-plan-probe";
		const bool passed = Lumen::Libs::Graphics::GpuMemoryAccessTrackingEnabled() == expected_tracking &&
		                    Lumen::Libs::Graphics::GraphicsRenderDrawAccessPlanContractTest() &&
		                    Lumen::Libs::Graphics::GpuMemoryDrawAccessPlanContractTest();
		std::fprintf(stderr, "[GFX_DRAW_ACCESS_PLAN_TEST] status=%s\n", passed ? "PASS" : "FAIL");
		return passed ? 0 : 1;
	}
	if (argc == 2 && std::string_view(argv[1]) == "--gpu-command-access-reducer-probe")
	{
		const bool passed = Lumen::Libs::Graphics::GpuMemoryAccessTrackingEnabled() &&
		                    Lumen::Libs::Graphics::GpuMemoryStorageCommandAccessContractTest();
		std::fprintf(stderr, "[GFX_COMMAND_ACCESS_REDUCER_TEST] status=%s\n", passed ? "PASS" : "FAIL");
		return passed ? 0 : 1;
	}
	if (argc != 1) return 1;
	if (!CheckScanModeDrawRejections() || !CheckAaMaskDrawRejections()) return 1;

#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
	if (!CheckWindowsGuestMemoryRead()) return 1;
#endif

	if (!CheckRectListSelector() || !CheckRectListPipeline())
	{
		std::fprintf(stderr, "rect-list selector/profile/SPIR-V/cache contract failed\n");
		return 1;
	}
	if (!CheckEopLeaseRetryPolicy() || !CheckThreadTraceDataPipeline() || !CheckShaderColorExportPipeline() || !CheckShaderPixelDepthOrderingPipeline() || !CheckUnnormalizedImageLoadMipParsing() ||
	    !CheckCoherentImageStoreParsing() || !CheckImageSampleLzRgba() || !CheckImageSampleSingleComponent(2, 1) ||
	    !CheckImageSampleSingleComponent(4, 2) || !CheckImageSampleSingleComponentParserRejections(2) ||
	    !CheckImageSampleSingleComponentParserRejections(4) || !CheckImplicitSampleLodZeroPipeline() ||
	    !CheckOrdinarySamplerComparisonLowering() || !CheckComparisonImageParserRejections() ||
	    !CheckVertexParameterExports() || !CheckParameterExportParserRejections() ||
	    !CheckType8SingleLevelImageBridge() || !CheckType8PixelSampleBridge() || !CheckR8UnormStorageImageBridge() ||
	    !CheckRgba8StorageAdjacentSingleWindowBridge() ||
	    !CheckComputeDirectSgprPipeline() ||
	    !CheckPixelDirectSgprPipeline() ||
	    !CheckVMulLegacyF32Pipeline() || !CheckVMacLegacyF32Pipeline() || !CheckVRsqClampF32Pipeline() || !CheckVMinU32Pipeline() ||
	    !CheckVCvtOffF32I4Pipeline() || !CheckVCvtOffF32I4ParserRejections() ||
	    !CheckBufferLoadFormatXyzwPipeline() || !CheckVMulLegacyF32ParserRejections() || !CheckVMacLegacyF32ParserRejections() ||
	    !CheckVRsqClampF32ParserRejections() || !CheckUnknownShaderInstructionDiagnostic() || !CheckVMinU32ParserRejections() ||
	    !CheckFp32PreCacheGate() ||
	    !CheckDx10ClampPreCacheGate() || !CheckDx10ClampCacheKey() || !CheckVRsqClampF32CacheIdentity() ||
	    !CheckImplicitSampleLodZeroCacheIdentity() || !CheckTextureTypeCacheKey() ||
	    !CheckPixelShaderOutputModeCacheKey() || !CheckNullSampledImageSelector() || !CheckDx10ClampZeroSelector() ||
	    !CheckImplicitSampleLodZeroSelector() ||
	    !CheckVMacLegacyF32Selector() || !CheckVRsqClampF32Selector() || !CheckTextureMipAssemblySelector() || !CheckRenderTextureSnapshotSelector() ||
	    !CheckGpuR8CaptureSelector() || !CheckGpuRgba8MipsCaptureSelector() || !CheckDynamicVertexHostVisibleSelector())
	{
		return 1;
	}
	if (!Lumen::Libs::Graphics::VertexBufferDynamicHostVisibleContractTest())
	{
		std::fprintf(stderr, "dynamic vertex host-visible allocation policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::VulkanReadbackMemoryContractTest() || !CheckHandoffReadbackComposition())
	{
		std::fprintf(stderr, "readback staging memory policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryReadOnlyStorageCrossAliasContractTest())
	{
		std::fprintf(stderr, "read-only storage-buffer cross-alias policy test failed\n");
		return 1;
	}
	if (!CheckExactReuseFastPathSelector() || !Lumen::Libs::Graphics::GpuMemoryExactReuseFastPathContractTest())
	{
		std::fprintf(stderr, "exact-reuse fast-path selector/alias policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryPageLookupContractTest())
	{
		std::fprintf(stderr, "GPU page lookup ownership/order contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryLabelSlotReuseContractTest())
	{
		std::fprintf(stderr, "GPU label slot replacement/lifetime contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemorySampledTextureRetirementContractTest())
	{
		std::fprintf(stderr, "sampled-texture retirement quarantine contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryPinnedLifetimeContractTest())
	{
		std::fprintf(stderr, "command-pinned GPU lifetime contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryStorageCommandAccessContractTest())
	{
		std::fprintf(stderr, "storage-buffer grouped command access contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryStorageWriteBackOwnerContractTest() ||
	    !Lumen::Libs::Graphics::GpuMemoryEopRecheckContractTest() ||
	    !Lumen::Libs::Graphics::GpuMemoryEopVirtualClockContractTest())
	{
		std::fprintf(stderr, "storage-buffer writeback owner contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryCpuHandoffContractTest())
	{
		std::fprintf(stderr, "CPU/GPU backing interval and physical alias handoff contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowHandoffQueueFamilySelectionTest())
	{
		std::fprintf(stderr, "CPU/GPU handoff queue family selection contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::StorageBufferGuestWriteContractTest())
	{
		std::fprintf(stderr, "storage-buffer guest publication contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::DepthSurfaceUniformZeroLeaseSetContractTest() ||
	    !Lumen::Libs::Graphics::GpuMemoryUniformDepthZeroSnapshotContractTest())
	{
		std::fprintf(stderr, "uniform depth zero snapshot policy contract test failed\n");
		return 1;
	}
	{
		using Lumen::Libs::Graphics::DepthStencilBufferObject;
		DepthStencilBufferObject legacy(VK_FORMAT_D32_SFLOAT_S8_UINT, 64, 64, true, false, false);
		DepthStencilBufferObject explicit_legacy(VK_FORMAT_D32_SFLOAT_S8_UINT, 64, 64, true, false, false, false);
		DepthStencilBufferObject snapshot(VK_FORMAT_D32_SFLOAT_S8_UINT, 64, 64, true, false, false, true);
		auto reserved = snapshot;
		reserved.params[DepthStencilBufferObject::PARAM_RESERVED] = 1;
		if (!legacy.Equal(explicit_legacy.params) || !explicit_legacy.Equal(legacy.params) ||
		    legacy.Equal(snapshot.params) || snapshot.Equal(legacy.params) || snapshot.Equal(reserved.params) ||
		    reserved.Equal(snapshot.params))
		{
			std::fprintf(stderr, "depth stencil uniform import parameter identity test failed\n");
			return 1;
		}
	}
	if (!Lumen::Libs::Graphics::GpuMemoryRenderTextureWriteTrackingContractTest())
	{
		std::fprintf(stderr, "render-texture write tracking contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::RenderTextureObjectParameterContractTest())
	{
		std::fprintf(stderr, "render-texture parameter identity contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryTextureMipWindowAliasContractTest())
	{
		std::fprintf(stderr, "texture mip-window alias contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryTextureMipChainObservationContractTest())
	{
		std::fprintf(stderr, "texture mip-chain observation contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::TextureMipSnapshotPreflightContractTest())
	{
		std::fprintf(stderr, "texture mip snapshot preflight contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::IndexBufferUpdateContractTest())
	{
		std::fprintf(stderr, "index-buffer replacement update contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryAllocatedRangeContainmentContractTest())
	{
		std::fprintf(stderr, "allocated-range containment policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GpuMemoryCpuCopyWriteBackPolicyContractTest())
	{
		std::fprintf(stderr, "CPU-copy writeback safety policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderComputeImageStoreBarrierContractTest())
	{
		std::fprintf(stderr, "compute image-store barrier policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::LabelDestroyQueueContractTest())
	{
		std::fprintf(stderr, "label destruction queue/reentrancy contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::LabelDestinationContractTest())
	{
		std::fprintf(stderr, "label deferred destination protection contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::LabelPublicationContractTest())
	{
		std::fprintf(stderr, "label deferred publication/lifetime contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderVideoOutSampledAliasBarrierContractTest())
	{
		std::fprintf(stderr, "VideoOut sampled-alias barrier policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderAutoIndexPrimitiveContractTest())
	{
		std::fprintf(stderr, "auto-index primitive routing contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderPipelineCacheGrowthContractTest())
	{
		std::fprintf(stderr, "graphics pipeline-cache growth/tombstone contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderFramebufferCacheReleaseAllContractTest())
	{
		std::fprintf(stderr, "framebuffer-cache release-all contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderBc7TextureProfileContractTest() ||
	    !Lumen::Libs::Graphics::GraphicsRenderTexturePreparationOrderContractTest())
	{
		std::fprintf(stderr, "texture preparation strict-prefix ordering contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderDynamicScissorContractTest())
	{
		std::fprintf(stderr, "dynamic scissor geometry/cache contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderInactiveD16DescriptorContractTest())
	{
		std::fprintf(stderr, "inactive D16 descriptor admission contract failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderStencilClearContractTest())
	{
		std::fprintf(stderr, "stencil attachment clear/write-mask/cache contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderPolygonOffsetContractTest())
	{
		std::fprintf(stderr, "polygon offset face/format/cache contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderResourceCaptureLimitsContractTest())
	{
		std::fprintf(stderr, "Graphics resource capture limits contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderDescriptorRecordingContractTest())
	{
		std::fprintf(stderr, "descriptor recording lifetime/cache reuse contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderConstantUploadContractTest())
	{
		std::fprintf(stderr, "constant UBO packing/arena/recording retirement contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderPreparedInputsContractTest())
	{
		std::fprintf(stderr, "prepared shader-input transaction contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::CommandBufferPendingSubmissionWorkContractTest())
	{
		std::fprintf(stderr, "pending command-buffer submission work contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderPassReuseContractTest())
	{
		std::fprintf(stderr, "render-pass reuse sequence/resource hazard contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderGpuR8CaptureContractTest())
	{
		std::fprintf(stderr, "GPU R8 capture/completion contract failed\n");
		return EXIT_FAILURE;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderBlendConstantContractTest())
	{
		std::fprintf(stderr, "blend constant range/factor/cache contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderVertexInputFormatContractTest())
	{
		std::fprintf(stderr, "vertex-input host format contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderOrdinarySamplerComparisonContractTest())
	{
		std::fprintf(stderr, "ordinary sampler comparison/cache contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderSamplerZFilterDimensionContractTest())
	{
		std::fprintf(stderr, "sampler Z-filter current-image dimension contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderSessionResetContractTest())
	{
		std::fprintf(stderr, "Shader session policy reset test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderNullSampledImageContractTest())
	{
		std::fprintf(stderr, "Shader null sampled-image policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderUsageLayoutContractTest())
	{
		std::fprintf(stderr, "Shader usage-layout contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderResourceRoutingContractTest())
	{
		std::fprintf(stderr, "Shader resource-routing contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderUsageOrderContractTest())
	{
		std::fprintf(stderr, "Shader usage-order contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderIdBulkAppendContractTest())
	{
		std::fprintf(stderr, "Shader key bulk append byte-order, bounds and fresh-input contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderConstantStorageContractTest())
	{
		std::fprintf(stderr, "constant storage capability/packing/SPIR-V contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderVertexResourcePreCacheFp32ContractTest())
	{
		std::fprintf(stderr, "Shader vertex resource pre-cache FP32 contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderVertexParseCacheContractTest())
	{
		std::fprintf(stderr, "vertex parse-cache identity, bounds and current-input proof contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderVertexUserSgprDeclarationContractTest())
	{
		std::fprintf(stderr, "Shader vertex user-SGPR declaration contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderVertexReadOnlyStorageContractTest())
	{
		std::fprintf(stderr, "Shader vertex read-only storage contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderPixelDirectSgprContractTest())
	{
		std::fprintf(stderr, "Shader pixel direct-SGPR contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderDx10ClampOptimizationContractTest())
	{
		std::fprintf(stderr, "DX10 clamp optimizer-isolation contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderDx10ClampSpirvPassThroughContractTest())
	{
		std::fprintf(stderr, "DX10 clamp SPIR-V pass-through contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderVMulLegacyF32OptimizationContractTest())
	{
		std::fprintf(stderr, "legacy VMul optimizer-isolation contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderVMacLegacyF32OptimizationContractTest())
	{
		std::fprintf(stderr, "experimental legacy VMac optimizer-isolation contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderVRsqClampF32OptimizationContractTest())
	{
		std::fprintf(stderr, "experimental V_RSQ_CLAMP_F32 optimizer-isolation contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderAndroidFexStructuralProfileSelectorContractTest())
	{
		std::fprintf(stderr, "Android FEX structural shader-profile selector contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::ShaderExtendedMappingContractTest())
	{
		std::fprintf(stderr, "Shader extended-mapping contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRunStateSynchronizationTest())
	{
		std::fprintf(stderr, "Graphics command-state synchronization test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRunDx10ClampBitExtractionContractTest())
	{
		std::fprintf(stderr, "DX10 clamp PM4 bit-extraction contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRunConstRamBoundsContractTest())
	{
		std::fprintf(stderr, "Const-RAM bounds contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRunDmaDataMemoryCopyContractTest())
	{
		std::fprintf(stderr, "DMA_DATA memory-copy contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRunDmaDataGdsReadContractTest() ||
	    !Lumen::Libs::Graphics::GraphicsRenderGdsReadDestinationContractTest())
	{
		std::fprintf(stderr, "DMA_DATA GDS destination contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::VideoOut::VideoOutWaitServiceContractTest() ||
	    !Lumen::Libs::Graphics::WindowFlipWaitEventServiceTest())
	{
		std::fprintf(stderr, "VideoOut/Window wait event-service contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::GraphicsRenderFlipArgumentsContractTest())
	{
		std::fprintf(stderr, "Deferred flip full-width argument contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::VideoOut::VideoOutIOSFlipCancelContractTest())
	{
		std::fprintf(stderr, "iOS VideoOut flip-cancel policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::VideoOut::VideoOutFramePresentLimitContractTest())
	{
		std::fprintf(stderr, "VideoOut frame-present limit policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::VideoOut::VideoOutFramePresentCheckpointContractTest())
	{
		std::fprintf(stderr, "VideoOut frame-present checkpoint policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::VideoOut::VideoOutRegistrationHandoffContractTest() ||
	    !Lumen::Libs::VideoOut::VideoOutBufferPublicationContractTest())
	{
		std::fprintf(stderr, "VideoOut buffer publication contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::VideoOutBufferSampledAliasContractTest())
	{
		std::fprintf(stderr, "VideoOut sampled-alias Vulkan contract test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowIOSRuntimeLifecycleStateTest())
	{
		std::fprintf(stderr, "iOS window lifecycle/exit-latch policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowUncappedPresentModeSelectionTest())
	{
		std::fprintf(stderr, "Uncapped present-mode selection policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowSwapchainImageCountSelectionTest())
	{
		std::fprintf(stderr, "Swapchain image-count selection policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowSwapchainRecreationPolicyTest())
	{
		std::fprintf(stderr, "Swapchain recreation policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowSwapchainPreTransformSelectionTest())
	{
		std::fprintf(stderr, "Swapchain pre-transform selection policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowSurfaceReplacementResultPolicyTest())
	{
		std::fprintf(stderr, "Surface replacement result policy test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowGuestPresentationLayoutTest())
	{
		std::fprintf(stderr, "Guest presentation aspect-fit layout test failed\n");
		return 1;
	}
	if (!Lumen::Libs::Graphics::WindowPresentationConfigurationTest())
	{
		std::fprintf(stderr, "Window presentation configuration test failed\n");
		return 1;
	}

	std::printf("Graphics packet atomicity=ok provenance-shadow=ok marker isolation/reset=ok CE progress=ok shader-export-parser=ok "
	            "shader-export-spirv=ok shader-export-state=ok shader-export-cache-key=ok pixel-depth-ordering=ok resource-capture-limits=ok "
	            "implicit-sample-lod-zero=ok image-type8-bridge=ok "
	            "image-type8-pixel-sample=ok image-rgba8-mip1-storage=ok texture-type-cache-key=ok compute-direct-sgpr=ok pixel-direct-sgpr=ok "
	            "vmul-legacy-parser-rejection=ok vmin-u32-parser-spirv=ok vmin-u32-rejections=ok "
	            "fp32-pre-cache=ok dx10-pre-cache=ok rect-list-profile-spirv-cache=ok pipeline-cache-growth=ok framebuffer-cache-release-all=ok "
	            "shader-session-reset=ok shader-null-sampled-image=ok shader-usage-layout=ok shader-usage-order=ok "
	            "shader-resource-routing=ok sampler-unknown-provenance=ok shader-extended-mapping=ok vertex-readonly-storage=ok storage-cross-alias=ok sampled-texture-retirement=ok texture-mip-window-alias=ok texture-mip-chain-observation=ok index-update=ok "
	            "allocated-range-containment=ok cpu-copy-writeback-safety=ok dma-data-memory-copy=ok "
	            "image-store-glc=ok compute-image-store-barrier=ok texture-prepare-order=ok "
	            "ios-videoout-cancel=ok "
	            "frame-present-limit=ok ios-window-lifecycle=ok "
	            "uncapped-present-mode=ok swapchain-image-count=ok swapchain-pre-transform=ok surface-replacement-result=ok "
	            "guest-presentation-aspect-fit=ok window-presentation-config=ok\n");
	return 0;
}
