#include "Emulator/Graphics/DepthSurfaceLayout.h"
#include "Emulator/Graphics/DepthSurfaceImport.h"
#include "Emulator/Graphics/HtileCpuObservation.h"
#include "Emulator/Graphics/TextureBc7Contract.h"
#include "Emulator/Graphics/Tile.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using Lumen::Libs::Graphics::TileConvertTiledToLinear;
using Lumen::Libs::Graphics::TileConvertLinearToTiled;
using Lumen::Libs::Graphics::TileGetTextureSize;
using Lumen::Libs::Graphics::TileTryGetDisplay2dThinTextureSize;
using Lumen::Libs::Graphics::TileTryConvertDisplay2dThinToLinear;
using Lumen::Libs::Graphics::TileTryConvertLinearToDisplay2dThin;
using Lumen::Libs::Graphics::TileTryGetColor2dThinTextureSize;
using Lumen::Libs::Graphics::TileTryConvertColor2dThinToLinear;
using Lumen::Libs::Graphics::TileTryConvertLinearToColor2dThin;
using Lumen::Libs::Graphics::TileMode;
using Lumen::Libs::Graphics::TilePaddedSize;
using Lumen::Libs::Graphics::TileSizeAlign;
using Lumen::Libs::Graphics::TileSizeOffset;
using Lumen::Libs::Graphics::TileTryGetDisplayLinearAlignedTextureSize;
using Lumen::Libs::Graphics::TileTryGetThin1dPow2PaddedTextureSize;
using Lumen::Libs::Graphics::DepthSurfaceLayout;
using Lumen::Libs::Graphics::DepthSurfaceLayoutRequest;
using Lumen::Libs::Graphics::ResolveDepthSurfaceLayout;

namespace {

struct TextureInfoPadded
{
	uint32_t width  = 0;
	uint32_t height = 0;
};

struct TextureInfoSize
{
	uint32_t total_size    = 0;
	uint32_t total_align   = 0;
	uint32_t mipmap_offset = 0;
	uint32_t mipmap_size   = 0;
};

struct TextureInfo
{
	uint32_t          dfmt   = 0;
	uint32_t          nfmt   = 0;
	uint32_t          width  = 0;
	uint32_t          height = 0;
	uint32_t          pitch  = 0;
	uint32_t          levels = 0;
	uint32_t          tile   = 0;
	bool              neo    = false;
	TextureInfoSize   size[16];
	TextureInfoPadded padded[16];
};

#include "emulator/src/Graphics/Tables/TileTextureInfo_10_10_0.inc"

} // namespace

struct TextureSizeCase
{
	const char* name;
	uint32_t    dfmt;
	uint32_t    nfmt;
	uint32_t    width;
	uint32_t    height;
	uint32_t    pitch;
	uint32_t    tile;
	uint32_t    expected_size;
	uint32_t    expected_padded_width;
	uint32_t    expected_padded_height;
};

static bool CheckTextureSize(const TextureSizeCase& test)
{
	for (int neo = 0; neo <= 1; neo++)
	{
		TileSizeAlign  total {};
		TileSizeOffset level {};
		TilePaddedSize padded {};

		TileGetTextureSize(test.dfmt, test.nfmt, test.width, test.height, test.pitch, 1, test.tile, neo != 0, &total, &level, &padded);

		if (total.size != test.expected_size || total.align != 256 || level.size != test.expected_size || level.offset != 0 ||
		    padded.width != test.expected_padded_width || padded.height != test.expected_padded_height)
		{
			std::fprintf(stderr,
			             "%s (neo=%d) failed: size=0x%x align=%u level={0x%x,0x%x} padded=%ux%u; expected size=0x%x "
			             "padded=%ux%u\n",
			             test.name, neo, total.size, total.align, level.size, level.offset, padded.width, padded.height,
			             test.expected_size, test.expected_padded_width, test.expected_padded_height);
			return false;
		}
	}

	return true;
}

static bool CheckDisplay2dThinFallback()
{
	auto check = [](const char* name, uint32_t width, uint32_t height, uint32_t pitch, bool neo, uint32_t expected_size,
	                uint32_t expected_align)
	{
		TileSizeAlign  total {};
		TileSizeOffset level {};
		TilePaddedSize padded {};
		if (!TileTryGetDisplay2dThinTextureSize(10, 0, width, height, pitch, 1, 10, neo, false, &total, &level, &padded) ||
		    total.size != expected_size || total.align != expected_align || level.size != expected_size || level.offset != 0 ||
		    padded.width != 0 || padded.height != 0)
		{
			std::fprintf(stderr, "%s Display_2dThin fallback changed: size=0x%x align=0x%x level={0x%x,0x%x} padded=%ux%u\n",
			             name, total.size, total.align, level.size, level.offset, padded.width, padded.height);
			return false;
		}
		return true;
	};

	if (!check("Minecraft non-Neo", 480, 270, 512, false, 0xa0000u, 0x8000u) ||
	    !check("Minecraft Neo", 480, 270, 512, true, 0xc0000u, 0x10000u) ||
	    !check("non-Neo height 63", 96, 63, 128, false, 0x8000u, 0x8000u) ||
	    !check("non-Neo height 64", 96, 64, 128, false, 0x8000u, 0x8000u) ||
	    !check("non-Neo height 65", 96, 65, 128, false, 0x10000u, 0x8000u) ||
	    !check("Neo height 127", 96, 127, 128, true, 0x10000u, 0x10000u) ||
	    !check("Neo height 128", 96, 128, 128, true, 0x10000u, 0x10000u) ||
	    !check("Neo height 129", 96, 129, 128, true, 0x20000u, 0x10000u))
	{
		return false;
	}

	auto check_table = [](const auto& table, const char* name)
	{
		size_t index = 0;
		for (const auto& info: table)
		{
			TileSizeAlign  total {};
			TileSizeOffset level {};
			TilePaddedSize padded {};
			if (!TileTryGetDisplay2dThinTextureSize(info.dfmt, info.nfmt, info.width, info.height, info.pitch, info.levels,
			                                          info.tile, info.neo, false, &total, &level, &padded) ||
			    total.size != info.size[0].total_size || total.align != info.size[0].total_align ||
			    level.size != info.size[0].mipmap_size || level.offset != info.size[0].mipmap_offset ||
			    padded.width != info.padded[0].width || padded.height != info.padded[0].height)
			{
				std::fprintf(stderr, "%s fixture %zu no longer matches calculated Display_2dThin layout\n", name, index);
				return false;
			}
			++index;
		}
		return index != 0;
	};
	if (!check_table(infos_10_10_0, "non-power-of-two") || !check_table(infos_10_10_0_pow2, "power-of-two"))
	{
		return false;
	}

	auto expect_rejected = [](uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels,
	                          uint32_t tile, bool pow2_pad)
	{
		TileSizeAlign  total {0x11111111u, 0x22222222u};
		TileSizeOffset level {0x33333333u, 0x44444444u};
		TilePaddedSize padded {0x55555555u, 0x66666666u};
		const bool accepted = TileTryGetDisplay2dThinTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, false, pow2_pad,
		                                                            &total, &level, &padded);
		return !accepted && total.size == 0x11111111u && total.align == 0x22222222u && level.size == 0x33333333u &&
		       level.offset == 0x44444444u && padded.width == 0x55555555u && padded.height == 0x66666666u;
	};
	if (!expect_rejected(1, 0, 96, 64, 128, 1, 10, false) ||
	    !expect_rejected(10, 9, 96, 64, 128, 1, 10, false) ||
	    !expect_rejected(10, 0, 96, 64, 128, 1, 13, false) ||
	    !expect_rejected(10, 0, 96, 64, 128, 0, 10, false) ||
	    !expect_rejected(10, 0, 96, 64, 128, 2, 10, false) ||
	    !expect_rejected(10, 0, 96, 64, 128, 1, 10, true) ||
	    !expect_rejected(10, 0, 0, 64, 128, 1, 10, false) ||
	    !expect_rejected(10, 0, 96, 0, 128, 1, 10, false) ||
	    !expect_rejected(10, 0, 96, 64, 0, 1, 10, false) ||
	    !expect_rejected(10, 0, 129, 64, 128, 1, 10, false) ||
	    !expect_rejected(10, 0, 96, 64, 192, 1, 10, false) ||
	    !expect_rejected(10, 0, 1, UINT32_MAX, 128, 1, 10, false) ||
	    !expect_rejected(10, 0, 1, 1, 0x01000000u, 1, 10, false) ||
	    !expect_rejected(10, 0, 1, UINT32_MAX, 0xffffff80u, 1, 10, false))
	{
		std::fprintf(stderr, "unsupported Display_2dThin shape was admitted or partially wrote outputs\n");
		return false;
	}

	TileSizeAlign  total {};
	TileSizeOffset level {};
	TilePaddedSize padded {};
	TileGetTextureSize(10, 0, 480, 270, 512, 1, 10, false, &total, &level, &padded, false);
	if (total.size != 0xa0000u || total.align != 0x8000u || level.size != 0xa0000u || level.offset != 0 ||
	    padded.width != 0 || padded.height != 0)
	{
		std::fprintf(stderr, "TileGetTextureSize did not select the Minecraft Display_2dThin fallback\n");
		return false;
	}

	total = {};
	level = {};
	padded = {};
	TileGetTextureSize(10, 0, 96, 96, 128, 1, 10, false, &total, &level, &padded, true);
	if (total.size != 0x10000u || total.align != 0x8000u || level.size != 0x10000u || level.offset != 0 ||
	    padded.width != 0 || padded.height != 0)
	{
		std::fprintf(stderr, "exact Display_2dThin fixture no longer takes precedence over calculated fallback\n");
		return false;
	}

	total = {};
	TileGetTextureSize(10, 0, 65536, 64, 65536, 1, 10, false, &total, nullptr, nullptr, false);
	if (total.size != 0x1000000u || total.align != 0x8000u)
	{
		std::fprintf(stderr, "large power-of-two Display_2dThin lookup was not bounded safely\n");
		return false;
	}
	return true;
}

static bool CheckThin1dPow2PaddedMipLayout()
{
	TileSizeAlign  total {};
	TileSizeOffset levels[3] {};
	TilePaddedSize padded[3] {};
	if (!TileTryGetThin1dPow2PaddedTextureSize(10, 0, 1230, 216, 2048, 2, 13, true, &total, levels, padded) ||
	    total.size != 0x280000u || total.align != 256u || levels[0].offset != 0 || levels[0].size != 0x200000u ||
	    levels[1].offset != 0x200000u || levels[1].size != 0x80000u || padded[0].width != 2048u ||
	    padded[0].height != 216u || padded[1].width != 1024u || padded[1].height != 128u)
	{
		std::fprintf(stderr, "Thin1D two-level POW2_PAD layout changed\n");
		return false;
	}

	total = {};
	for (auto& level: levels)
	{
		level = {};
	}
	for (auto& size: padded)
	{
		size = {};
	}
	if (!TileTryGetThin1dPow2PaddedTextureSize(10, 0, 1000, 300, 1024, 3, 13, true, &total, levels, padded) ||
	    total.size != 0x2a0000u || levels[0].size != 0x200000u || levels[1].offset != 0x200000u ||
	    levels[1].size != 0x80000u || levels[2].offset != 0x280000u || levels[2].size != 0x20000u)
	{
		std::fprintf(stderr, "Thin1D general three-level POW2_PAD layout changed\n");
		return false;
	}

	TileSizeAlign via_public_api {};
	TileSizeOffset public_levels[2] {};
	TilePaddedSize public_padded[2] {};
	TileGetTextureSize(10, 0, 1230, 216, 2048, 2, 13, false, &via_public_api, public_levels, public_padded, true);
	if (via_public_api.size != 0x280000u || public_levels[1].offset != 0x200000u || public_levels[1].size != 0x80000u)
	{
		std::fprintf(stderr, "TileGetTextureSize did not select the proven POW2_PAD fallback\n");
		return false;
	}

	if (TileTryGetThin1dPow2PaddedTextureSize(10, 0, 1230, 216, 1230, 2, 13, true, nullptr, nullptr, nullptr) ||
	    TileTryGetThin1dPow2PaddedTextureSize(10, 0, 1230, 216, 1024, 2, 13, true, nullptr, nullptr, nullptr) ||
	    TileTryGetThin1dPow2PaddedTextureSize(10, 0, 1230, 216, 2048, 2, 13, false, nullptr, nullptr, nullptr) ||
	    TileTryGetThin1dPow2PaddedTextureSize(10, 0, 1230, 216, 2048, 2, 14, true, nullptr, nullptr, nullptr) ||
	    TileTryGetThin1dPow2PaddedTextureSize(1, 0, 1230, 216, 2048, 2, 13, true, nullptr, nullptr, nullptr) ||
	    TileTryGetThin1dPow2PaddedTextureSize(10, 0, 1, 1, 8, 2, 13, true, nullptr, nullptr, nullptr))
	{
		std::fprintf(stderr, "unsupported Thin1D POW2_PAD shape was admitted\n");
		return false;
	}
	return true;
}

static bool CheckDisplayLinearAlignedMipLayout()
{
	constexpr uint32_t pow2_offsets[]     = {0x00000u, 0x40000u, 0x50000u, 0x54000u, 0x55000u};
	constexpr uint32_t pow2_sizes[]       = {0x40000u, 0x10000u, 0x04000u, 0x01000u, 0x00400u};
	constexpr uint32_t pow2_total         = 0x55400u;

	for (int neo = 0; neo <= 1; ++neo)
	{
		TileSizeAlign  total {};
		TileSizeOffset levels[5] {};
		TilePaddedSize padded[5] {};
		if (!TileTryGetDisplayLinearAlignedTextureSize(10, 0, 184, 160, 256, 5, 8, neo != 0, true, &total, levels, padded) ||
		    total.size != pow2_total || total.align != 256u)
		{
			std::fprintf(stderr, "DisplayLinearAligned POW2_PAD live layout was rejected (neo=%d)\n", neo);
			return false;
		}
		for (uint32_t level = 0; level < 5; ++level)
		{
			const uint32_t logical_width  = std::max(184u >> level, 1u);
			const uint32_t logical_height = std::max(160u >> level, 1u);
			const uint32_t row_bytes      = std::max(256u >> level, 1u) * 4u;
			const uint64_t last_read = static_cast<uint64_t>(pow2_offsets[level]) +
			                           static_cast<uint64_t>(logical_height - 1u) * row_bytes + logical_width * 4u;
			if (levels[level].offset != pow2_offsets[level] || levels[level].size != pow2_sizes[level] ||
			    padded[level].width != 0 || padded[level].height != 0 ||
			    last_read > static_cast<uint64_t>(pow2_offsets[level]) + pow2_sizes[level])
			{
				std::fprintf(stderr, "DisplayLinearAligned POW2_PAD live mip %u changed (neo=%d)\n", level, neo);
				return false;
			}
		}

		total = {};
		for (auto& level: levels)
		{
			level = {};
		}
		TileGetTextureSize(10, 0, 184, 160, 256, 5, 8, neo != 0, &total, levels, nullptr, true);
		if (total.size != pow2_total || total.align != 256u || levels[4].offset != pow2_offsets[4] ||
		    levels[4].size != pow2_sizes[4])
		{
			std::fprintf(stderr, "TileGetTextureSize did not select the DisplayLinearAligned POW2_PAD oracle (neo=%d)\n", neo);
			return false;
		}
	}

	auto expect_rejected = [](uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels,
	                          uint32_t tile, bool pow2_pad)
	{
		TileSizeAlign  total {0x11111111u, 0x22222222u};
		TileSizeOffset level_sizes[16];
		TilePaddedSize padded[16];
		for (uint32_t index = 0; index < 16; ++index)
		{
			level_sizes[index] = {0x33333333u, 0x44444444u};
			padded[index]      = {0x77777777u, 0x88888888u};
		}
		const bool accepted = TileTryGetDisplayLinearAlignedTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, false, pow2_pad,
		                                                                  &total, level_sizes, padded);
		if (accepted || total.size != 0x11111111u || total.align != 0x22222222u)
		{
			return false;
		}
		for (uint32_t index = 0; index < 16; ++index)
		{
			if (level_sizes[index].size != 0x33333333u || level_sizes[index].offset != 0x44444444u ||
			    padded[index].width != 0x77777777u || padded[index].height != 0x88888888u)
			{
				return false;
			}
		}
		return true;
	};

	if (!expect_rejected(10, 0, 184, 160, 256, 5, 8, false) || !expect_rejected(10, 0, 184, 160, 256, 5, 13, true) ||
	    !expect_rejected(3, 0, 184, 160, 256, 5, 8, true) || !expect_rejected(10, 1, 184, 160, 256, 5, 8, true) ||
	    !expect_rejected(10, 0, 184, 160, 128, 5, 8, true) || !expect_rejected(10, 0, 0, 160, 256, 5, 8, true) ||
	    !expect_rejected(10, 0, 184, 0, 256, 5, 8, true) || !expect_rejected(10, 0, 184, 160, 256, 1, 8, true) ||
	    !expect_rejected(10, 0, 184, 160, 256, 16, 8, true) || !expect_rejected(10, 0, 184, 160, 256, 9, 8, true) ||
	    !expect_rejected(10, 9, 184, 160, 256, 5, 8, true) || !expect_rejected(10, 0, 64, 5, 64, 3, 8, true) ||
	    !expect_rejected(10, 0, 184, 160, 256, 6, 8, true) ||
	    !expect_rejected(10, 0, 184, 160, 512, 5, 8, true) ||
	    !expect_rejected(10, 0, 0x80000000u, 2, 0x80000000u, 2, 8, true))
	{
		std::fprintf(stderr, "unsupported DisplayLinearAligned shape was admitted or partially wrote outputs\n");
		return false;
	}

	auto check_conversion = [](uint32_t total, bool pow2_pad)
	{
		std::vector<uint8_t> source(total + 32u);
		std::vector<uint8_t> destination(total + 32u, 0xa5u);
		std::vector<uint8_t> retiled(total + 32u, 0x6du);
		for (uint32_t i = 0; i < total; ++i)
		{
			source[i] = static_cast<uint8_t>((i * 131u + 17u) & 0xffu);
		}
		TileConvertTiledToLinear(destination.data(), source.data(), TileMode::TextureTiled, 10, 0, 184, 160, 256, 5, false, 8,
		                        total, pow2_pad);
		TileConvertLinearToTiled(retiled.data(), destination.data(), TileMode::TextureTiled, 10, 0, 184, 160, 256, 5, false, 8,
		                         total, pow2_pad);
		if (std::memcmp(destination.data(), source.data(), total) != 0 || std::memcmp(retiled.data(), source.data(), total) != 0)
		{
			return false;
		}
		for (uint32_t i = total; i < destination.size(); ++i)
		{
			if (destination[i] != 0xa5u || retiled[i] != 0x6du)
			{
				return false;
			}
		}
		return true;
	};
	if (!check_conversion(pow2_total, true))
	{
		std::fprintf(stderr, "DisplayLinearAligned conversion changed bytes or crossed its computed allocation\n");
		return false;
	}
	return true;
}

static bool CheckGuard(const std::vector<uint8_t>& bytes, uint32_t payload_size, uint8_t canary)
{
	constexpr uint32_t guard_size = 32u;
	if (bytes.size() != static_cast<size_t>(payload_size) + guard_size * 2u)
	{
		return false;
	}
	return std::all_of(bytes.begin(), bytes.begin() + guard_size, [canary](uint8_t value) { return value == canary; }) &&
	       std::all_of(bytes.end() - guard_size, bytes.end(), [canary](uint8_t value) { return value == canary; });
}

static bool CheckThin1dRoundTrip(const char* name, uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
                                 uint32_t expected_size)
{
	constexpr uint32_t guard_size = 32u;
	constexpr uint8_t  canary     = 0xa5u;
	TileSizeAlign      total {};
	TileGetTextureSize(dfmt, nfmt, width, height, pitch, 1, 13, false, &total, nullptr, nullptr);
	if (total.size != expected_size || total.align != 256u)
	{
		std::fprintf(stderr, "%s layout changed: size=0x%x align=%u\n", name, total.size, total.align);
		return false;
	}

	std::vector<uint8_t> linear(total.size + guard_size * 2u, canary);
	std::vector<uint8_t> tiled(total.size + guard_size * 2u, canary);
	std::vector<uint8_t> recovered(total.size + guard_size * 2u, canary);
	for (uint32_t index = 0; index < total.size; ++index)
	{
		linear[guard_size + index] = static_cast<uint8_t>((index * 131u + (index >> 8u) * 17u + 29u) & 0xffu);
	}

	TileConvertLinearToTiled(tiled.data() + guard_size, linear.data() + guard_size, TileMode::TextureTiled, dfmt, nfmt, width, height,
	                         pitch, 1, false, 13, total.size, false);
	TileConvertTiledToLinear(recovered.data() + guard_size, tiled.data() + guard_size, TileMode::TextureTiled, dfmt, nfmt, width,
	                         height, pitch, 1, false, 13, total.size, false);
	if (std::memcmp(linear.data() + guard_size, recovered.data() + guard_size, total.size) != 0 ||
	    !CheckGuard(linear, total.size, canary) || !CheckGuard(tiled, total.size, canary) ||
	    !CheckGuard(recovered, total.size, canary))
	{
		std::fprintf(stderr, "%s Thin1D round-trip or guard check failed\n", name);
		return false;
	}
	return true;
}

static uint32_t Thin1dMortonIndex(uint32_t x, uint32_t y)
{
	return ((x >> 0u) & 1u) << 0u | ((y >> 0u) & 1u) << 1u | ((x >> 1u) & 1u) << 2u |
	       ((y >> 1u) & 1u) << 3u | ((x >> 2u) & 1u) << 4u | ((y >> 2u) & 1u) << 5u;
}

static uint32_t Thin1dMortonX(uint32_t index)
{
	return ((index >> 0u) & 1u) << 0u | ((index >> 2u) & 1u) << 1u | ((index >> 4u) & 1u) << 2u;
}

static uint32_t Thin1dMortonY(uint32_t index)
{
	return ((index >> 1u) & 1u) << 0u | ((index >> 3u) & 1u) << 1u | ((index >> 5u) & 1u) << 2u;
}

static bool CheckBc7Thin1dShapeContract()
{
	using Lumen::Libs::Graphics::Bc7Thin1dLayout;
	using Lumen::Libs::Graphics::ResolveBc7Thin1dLayout;
	const uint64_t original[] = {41, 0, 900, 736, 928, 0, 1, 13, 0};
	const auto resolve = [](const uint64_t* values, Bc7Thin1dLayout* output) {
		return ResolveBc7Thin1dLayout(values[0], values[1], values[2], values[3], values[4],
		    values[5], values[6], values[7], values[8] != 0, output);
	};
	Bc7Thin1dLayout layout {};
	if (!resolve(original, &layout) || layout.bytes != 0xa6c00u || layout.alignment != 256 ||
	    layout.padded_block_width != 232 || layout.padded_block_height != 184) return false;
	constexpr uint64_t negatives[][2] = {
	    {0, 37}, {1, 9}, {1, 7}, {2, 0}, {2, 899}, {3, 0}, {3, 735}, {4, 0}, {4, 900 - 4}, {4, 927},
	    {5, 1}, {6, 0}, {6, 2}, {7, 8}, {7, 10}, {7, 14}, {7, 31}, {8, 1},
	    {2, uint64_t {UINT32_MAX} + 1}, {3, uint64_t {UINT32_MAX} + 1}, {4, uint64_t {UINT32_MAX} + 1},
	};
	for (const auto& negative: negatives)
	{
		uint64_t changed[9];
		std::copy_n(original, 9, changed);
		changed[negative[0]] = negative[1];
		const Bc7Thin1dLayout sentinel {1, 2, 3, 4};
		layout = sentinel;
		if (resolve(changed, &layout) || std::memcmp(&layout, &sentinel, sizeof(layout)) != 0) return false;
	}
	uint64_t huge[] = {41, 0, UINT32_MAX - 3u, UINT32_MAX - 3u, UINT32_MAX - 3u, 0, 1, 13, 0};
	layout = {1, 2, 3, 4};
	if (resolve(huge, &layout) || layout.bytes != 1 || layout.alignment != 2 ||
	    layout.padded_block_width != 3 || layout.padded_block_height != 4) return false;
	return true;
}

// The CPU never decodes these blocks. Independent Morton indexing checks
// exact byte locations, active blocks and untouched pitch/micro-tile padding.
static bool CheckBc7OpaqueBlockConversion(uint32_t width, uint32_t height, uint32_t pitch, bool neo)
{
	using namespace Lumen::Libs::Graphics;
	Bc7Thin1dLayout layout {};
	if (!ResolveBc7Thin1dLayout(41, 0, width, height, pitch, 0, 1, 13, false, &layout)) return false;
	constexpr uint32_t guard_size = 32;
	constexpr uint8_t guard = 0xcdu, tiled_padding = 0xa5u, linear_padding = 0x6du, recovered_padding = 0xe7u;
	std::vector<uint8_t> linear(layout.bytes + 2u * guard_size, guard);
	std::vector<uint8_t> tiled(layout.bytes + 2u * guard_size, guard);
	std::vector<uint8_t> recovered(layout.bytes + 2u * guard_size, guard);
	std::vector<uint8_t> expected_tiled(layout.bytes, tiled_padding);
	std::vector<uint8_t> expected_recovered(layout.bytes, recovered_padding);
	std::memset(linear.data() + guard_size, linear_padding, layout.bytes);
	std::memset(tiled.data() + guard_size, tiled_padding, layout.bytes);
	std::memset(recovered.data() + guard_size, recovered_padding, layout.bytes);
	for (uint32_t y = 0; y < height / 4u; ++y)
		for (uint32_t x = 0; x < width / 4u; ++x)
		{
			const uint64_t linear_offset = (uint64_t {y} * (pitch / 4u) + x) * 16u;
			const uint64_t tile_index = uint64_t {y / 8u} * (layout.padded_block_width / 8u) + x / 8u;
			const uint64_t tiled_offset = tile_index * 1024u + Thin1dMortonIndex(x % 8u, y % 8u) * 16u;
			if (linear_offset + 16u > layout.bytes || tiled_offset + 16u > layout.bytes) return false;
			for (uint32_t byte = 0; byte < 16; ++byte)
			{
				const uint8_t value = static_cast<uint8_t>((x * 137u + y * 43u + byte * 19u + (x >> 3u) * 29u) & 0xffu);
				linear[guard_size + linear_offset + byte] = value;
				expected_tiled[tiled_offset + byte] = value;
				expected_recovered[linear_offset + byte] = value;
			}
		}
	const auto original = linear;
	TileConvertLinearToTiled(tiled.data() + guard_size, linear.data() + guard_size, TileMode::TextureTiled,
	    41, 0, width, height, pitch, 1, neo, 13, layout.bytes, false);
	if (linear != original || std::memcmp(tiled.data() + guard_size, expected_tiled.data(), layout.bytes) != 0 ||
	    !CheckGuard(linear, layout.bytes, guard) || !CheckGuard(tiled, layout.bytes, guard)) return false;
	TileConvertTiledToLinear(recovered.data() + guard_size, tiled.data() + guard_size, TileMode::TextureTiled,
	    41, 0, width, height, pitch, 1, neo, 13, layout.bytes, false);
	if (std::memcmp(recovered.data() + guard_size, expected_recovered.data(), layout.bytes) != 0 ||
	    !CheckGuard(recovered, layout.bytes, guard) || !CheckGuard(tiled, layout.bytes, guard)) return false;
	return true;
}

static bool CheckThin1dCrossFormatSubrange()
{
	constexpr uint32_t guard_size  = 32u;
	constexpr uint32_t r8_width    = 512u;
	constexpr uint32_t r8_height   = 512u;
	constexpr uint32_t r8_size     = 0x40000u;
	constexpr uint32_t rgba_width  = 48u;
	constexpr uint32_t rgba_height = 48u;
	constexpr uint32_t rgba_size   = 0x2400u;
	constexpr uint8_t  canary      = 0x6du;

	std::vector<uint8_t> r8_linear(r8_size + guard_size * 2u, canary);
	std::vector<uint8_t> r8_tiled(r8_size + guard_size * 2u, canary);
	std::vector<uint8_t> rgba_tiled(rgba_size + guard_size * 2u, canary);
	std::vector<uint8_t> rgba_linear(rgba_size + guard_size * 2u, canary);
	for (uint32_t y = 0; y < r8_height; ++y)
	{
		for (uint32_t x = 0; x < r8_width; ++x)
		{
			r8_linear[guard_size + y * r8_width + x] =
			    static_cast<uint8_t>((x * 37u + y * 11u + (x >> 3u) * 19u + (y >> 3u) * 23u) & 0xffu);
		}
	}

	TileConvertLinearToTiled(r8_tiled.data() + guard_size, r8_linear.data() + guard_size, TileMode::TextureTiled, 1, 0, r8_width,
	                         r8_height, r8_width, 1, false, 13, r8_size, false);
	for (uint32_t y = 0; y < r8_height; ++y)
	{
		for (uint32_t x = 0; x < r8_width; ++x)
		{
			const uint32_t r8_tile = (y / 8u) * (r8_width / 8u) + x / 8u;
			const uint32_t raw_offset = r8_tile * 64u + Thin1dMortonIndex(x & 7u, y & 7u);
			if (raw_offset < rgba_size && r8_tiled[guard_size + raw_offset] != r8_linear[guard_size + y * r8_width + x])
			{
				std::fprintf(stderr, "Thin1D R8 reference re-tile mismatch at (%u,%u): raw=0x%x\n", x, y, raw_offset);
				return false;
			}
		}
	}
	std::memcpy(rgba_tiled.data() + guard_size, r8_tiled.data() + guard_size, rgba_size);
	TileConvertTiledToLinear(rgba_linear.data() + guard_size, rgba_tiled.data() + guard_size, TileMode::TextureTiled, 10, 0,
	                         rgba_width, rgba_height, rgba_width, 1, false, 13, rgba_size, false);

	constexpr uint32_t r8_tile_bytes       = 8u * 8u;
	constexpr uint32_t r8_tiles_per_row    = r8_width / 8u;
	constexpr uint32_t rgba_tile_bytes     = 8u * 8u * 4u;
	constexpr uint32_t rgba_tiles_per_row  = rgba_width / 8u;
	bool               differs_from_naive  = false;
	for (uint32_t y = 0; y < rgba_height; ++y)
	{
		for (uint32_t x = 0; x < rgba_width; ++x)
		{
			const uint32_t rgba_tile = (y / 8u) * rgba_tiles_per_row + x / 8u;
			const uint32_t rgba_element = Thin1dMortonIndex(x & 7u, y & 7u);
			for (uint32_t component = 0; component < 4u; ++component)
			{
				const uint32_t raw_offset = rgba_tile * rgba_tile_bytes + rgba_element * 4u + component;
				const uint32_t r8_tile    = raw_offset / r8_tile_bytes;
				const uint32_t r8_element = raw_offset % r8_tile_bytes;
				const uint32_t source_x = (r8_tile % r8_tiles_per_row) * 8u + Thin1dMortonX(r8_element);
				const uint32_t source_y = (r8_tile / r8_tiles_per_row) * 8u + Thin1dMortonY(r8_element);
				const uint8_t expected = r8_linear[guard_size + source_y * r8_width + source_x];
				const uint8_t actual = rgba_linear[guard_size + (y * rgba_width + x) * 4u + component];
				differs_from_naive = differs_from_naive ||
				                     actual != r8_linear[guard_size + (y * rgba_width + x) * 4u + component];
				if (actual != expected)
				{
					std::fprintf(stderr,
					             "Thin1D R8/RGBA8 subrange mismatch at (%u,%u).%u: raw=0x%x source=(%u,%u) expected=%u actual=%u\n",
					             x, y, component, raw_offset, source_x, source_y, expected, actual);
					return false;
				}
			}
		}
	}
	if (!differs_from_naive)
	{
		std::fprintf(stderr, "Thin1D R8/RGBA8 subrange unexpectedly matched naive linear reinterpretation\n");
		return false;
	}

	if (!CheckGuard(r8_linear, r8_size, canary) || !CheckGuard(r8_tiled, r8_size, canary) ||
	    !CheckGuard(rgba_tiled, rgba_size, canary) || !CheckGuard(rgba_linear, rgba_size, canary))
	{
		std::fprintf(stderr, "Thin1D R8/RGBA8 subrange guard check failed\n");
		return false;
	}
	return true;
}

// Scalar address reference for the pre-existing 32-bit display layout. Keep
// this independent of the production micro-tile block traversal: it checks
// every logical texel, including incomplete 8x8 tails and pipe/bank edges.
static uint64_t ReferenceDisplay2dThinOffset(uint32_t x, uint32_t y, uint32_t pitch, bool neo)
{
	const uint32_t element = (x & 1u) | (x & 2u) | ((y & 1u) << 2u) | ((x & 4u) << 1u) |
	                         ((y & 2u) << 3u) | ((y & 4u) << 3u);
	const uint32_t pipe = ((((x >> 3u) ^ (y >> 3u) ^ (x >> 4u)) & 1u) << 0u) |
	                      ((((x >> 4u) ^ (y >> 4u)) & 1u) << 1u) |
	                      ((((x >> 5u) ^ (y >> 5u)) & 1u) << 2u) |
	                      (neo ? ((((x >> 6u) ^ (y >> 5u)) & 1u) << 3u) : 0u);
	const uint32_t xs = x >> (neo ? 4u : 3u);
	const uint32_t ys = y >> (neo ? 1u : 0u);
	const uint32_t bank = neo
	    ? (((((xs >> 3u) ^ (ys >> 5u)) & 1u) << 0u) |
	       ((((xs >> 4u) ^ (ys >> 4u) ^ (ys >> 5u)) & 1u) << 1u) |
	       ((((xs >> 5u) ^ (ys >> 3u)) & 1u) << 2u))
	    : (((((xs >> 3u) ^ (ys >> 6u)) & 1u) << 0u) |
	       ((((xs >> 4u) ^ (ys >> 5u) ^ (ys >> 6u)) & 1u) << 1u) |
	       ((((xs >> 5u) ^ (ys >> 4u)) & 1u) << 2u) |
	       ((((xs >> 6u) ^ (ys >> 3u)) & 1u) << 3u));
	const uint32_t macro_height = neo ? 128u : 64u;
	const uint64_t macro_index = static_cast<uint64_t>(y / macro_height) * (pitch / 128u) + x / 128u;
	const uint64_t byte_index = macro_index * (neo ? 512u : 256u) + ((y / 8u) % (neo ? 2u : 1u)) * 256u + element * 4u;
	return (byte_index & 255u) | (static_cast<uint64_t>(pipe) << 8u) |
	       (static_cast<uint64_t>(bank) << (neo ? 12u : 11u)) | ((byte_index >> 8u) << 15u);
}

static bool CheckDisplay2dThinConversion(uint32_t width, uint32_t height, uint32_t pitch, bool neo)
{
	constexpr size_t guard = 19;
	constexpr size_t tail = 31;
	constexpr uint8_t canary = 0xa5;
	constexpr uint32_t untouched = 0xa5a5a5a5u;
	TileSizeAlign layout;
	if (!TileTryGetDisplay2dThinTextureSize(10, 0, width, height, pitch, 1, 10, neo, false, &layout, nullptr, nullptr)) return false;
	const uint32_t padded_height = layout.size / (pitch * 4u);
	const size_t capacity = layout.size + tail;
	std::vector<uint8_t> linear(guard + capacity + guard, canary);
	std::vector<uint8_t> full_tiled(linear.size(), canary);
	std::vector<uint8_t> tiled(linear.size(), canary);
	std::vector<uint8_t> recovered(linear.size(), canary);
	for (uint32_t y = 0; y < padded_height; ++y)
	{
		for (uint32_t x = 0; x < pitch; ++x)
		{
			const uint32_t identity = y * pitch + x + 1u;
			std::memcpy(linear.data() + guard + (y * pitch + x) * 4u, &identity, 4);
		}
	}
	const auto original_linear = linear;
	if (!TileTryConvertLinearToDisplay2dThin(full_tiled.data() + guard, capacity, linear.data() + guard, capacity,
	                                       pitch, padded_height, pitch, neo)) return false;
	// Full physical geometry must be a permutation: no colliding addresses,
	// unwritten texels or lost identities. This does not use a second tiler.
	std::vector<bool> seen(layout.size / 4u, false);
	for (uint32_t offset = 0; offset < layout.size; offset += 4u)
	{
		uint32_t identity = 0;
		std::memcpy(&identity, full_tiled.data() + guard + offset, 4);
		if (identity == 0 || identity > seen.size() || seen[identity - 1u]) return false;
		seen[identity - 1u] = true;
	}
	if (!TileTryConvertLinearToDisplay2dThin(tiled.data() + guard, capacity, linear.data() + guard, capacity,
	                                       width, height, pitch, neo) ||
	    !TileTryConvertDisplay2dThinToLinear(recovered.data() + guard, capacity, tiled.data() + guard, capacity,
	                                       width, height, pitch, neo)) return false;
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const uint64_t offset = ReferenceDisplay2dThinOffset(x, y, pitch, neo);
			if (offset > layout.size || 4u > layout.size - offset) return false;
			uint32_t actual = 0;
			std::memcpy(&actual, tiled.data() + guard + offset, 4u);
			if (actual != y * pitch + x + 1u)
			{
				std::fprintf(stderr, "Display_2dThin scalar address mismatch (%u,%u) pitch=%u neo=%u offset=%llu\n",
				             x, y, pitch, neo ? 1u : 0u, static_cast<unsigned long long>(offset));
				return false;
			}
		}
	}
	const auto original_tiled = tiled;
	for (uint32_t offset = 0; offset < layout.size; offset += 4u)
	{
		uint32_t full_identity = 0;
		uint32_t actual = 0;
		std::memcpy(&full_identity, full_tiled.data() + guard + offset, 4);
		std::memcpy(&actual, tiled.data() + guard + offset, 4);
		const uint32_t x = (full_identity - 1u) % pitch;
		const uint32_t y = (full_identity - 1u) / pitch;
		if (actual != (x < width && y < height ? full_identity : untouched)) return false;
		std::memcpy(&actual, recovered.data() + guard + offset, 4);
		const uint32_t linear_index = offset / 4u;
		if (actual != (linear_index % pitch < width && linear_index / pitch < height ? linear_index + 1u : untouched)) return false;
	}
	// The checked helpers accept the exact active linear span and a complete
	// tiled slice. Neither needs access to the trailing linear pitch padding.
	const uint64_t linear_span = (uint64_t(height - 1u) * pitch + width) * 4u;
	if (!TileTryConvertLinearToDisplay2dThin(tiled.data() + guard, layout.size, linear.data() + guard, linear_span,
	                                       width, height, pitch, neo) ||
	    !TileTryConvertDisplay2dThinToLinear(recovered.data() + guard, linear_span, tiled.data() + guard, layout.size,
	                                       width, height, pitch, neo) || tiled != original_tiled || linear != original_linear) return false;
	const auto original_recovered = recovered;
	for (const auto* bytes: {&linear, &full_tiled, &tiled, &recovered})
	{
		if (!std::all_of(bytes->begin(), bytes->begin() + guard, [](uint8_t value) { return value == canary; }) ||
		    !std::all_of(bytes->begin() + guard + layout.size, bytes->end(), [](uint8_t value) { return value == canary; })) return false;
	}
	// Exercise the production generic entry points too; tile 8/13 remain
	// covered by the existing independent linear and Thin1D cases below.
	std::fill(tiled.begin(), tiled.end(), canary);
	std::fill(recovered.begin(), recovered.end(), canary);
	TileConvertLinearToTiled(tiled.data() + guard, linear.data() + guard, TileMode::TextureTiled, 10, 0, width, height,
	                         pitch, 1, neo, 10, layout.size, false);
	TileConvertTiledToLinear(recovered.data() + guard, tiled.data() + guard, TileMode::TextureTiled, 10, 0, width, height,
	                         pitch, 1, neo, 10, layout.size, false);
	return tiled == original_tiled && recovered == original_recovered && linear == original_linear;
}

static bool CheckDisplay2dThinAddressFixtures()
{
	// Fixed local address vectors for the existing display micro-tile bit
	// ordering and pipe/bank transitions. They are deliberately literal: a
	// matching inverse alone cannot detect a shared wrong address permutation.
	// The generated Addrlib tables in this test supply size, not address oracles.
	struct Address { uint32_t x; uint32_t y; uint32_t base; uint32_t neo; };
	constexpr Address addresses[] = {
	    {0, 0, 0, 0}, {1, 0, 4, 4}, {2, 0, 8, 8}, {4, 0, 32, 32},
	    {0, 1, 16, 16}, {0, 2, 64, 64}, {0, 4, 128, 128}, {7, 7, 252, 252},
	    {8, 0, 256, 256}, {16, 0, 768, 768}, {32, 0, 1024, 1024}, {64, 0, 2048, 2048},
	    {0, 8, 16640, 33024}, {0, 16, 8704, 16896}, {0, 32, 5120, 11264},
	    {0, 64, 71680, 12288}, {128, 0, 36864, 69632}, {0, 128, 131072, 131072},
	    {24, 8, 17152, 33536},
	};
	for (const bool neo: {false, true})
	{
		std::vector<uint32_t> linear(256u * 256u);
		std::vector<uint32_t> tiled(linear.size(), 0xa5a5a5a5u);
		std::vector<uint32_t> fixture(linear.size(), 0);
		std::vector<uint32_t> decoded(linear.size(), 0);
		for (uint32_t index = 0; index < linear.size(); ++index) linear[index] = index + 1u;
		if (!TileTryConvertLinearToDisplay2dThin(tiled.data(), tiled.size() * 4u, linear.data(), linear.size() * 4u,
		                                       256, 256, 256, neo)) return false;
		for (const auto& point: addresses)
		{
			const uint32_t offset = neo ? point.neo : point.base;
			const uint32_t identity = point.y * 256u + point.x + 1u;
			if (tiled[offset / 4u] != identity)
			{
				std::fprintf(stderr, "Display_2dThin address mismatch (%u,%u) neo=%u byte_offset=%u expected=%u actual=%u\n",
				             point.x, point.y, neo ? 1u : 0u, offset, identity, tiled[offset / 4u]);
				return false;
			}
			fixture[offset / 4u] = identity;
		}
		if (!TileTryConvertDisplay2dThinToLinear(decoded.data(), decoded.size() * 4u, fixture.data(), fixture.size() * 4u,
		                                       256, 256, 256, neo)) return false;
		for (const auto& point: addresses)
			if (decoded[point.y * 256u + point.x] != point.y * 256u + point.x + 1u) return false;
	}
	return true;
}

static bool CheckDisplay2dThinRejectedConversions()
{
	for (const bool neo: {false, true})
	{
		const uint64_t size = neo ? 0x10000u : 0x8000u;
		const uint32_t height = neo ? 128u : 64u;
		std::vector<uint8_t> source(size, 0x3c);
		std::vector<uint8_t> destination(size, 0xa5);
		const auto original_source = source;
		const auto original_destination = destination;
		for (const auto convert: {TileTryConvertDisplay2dThinToLinear, TileTryConvertLinearToDisplay2dThin})
		{
			if (convert(destination.data(), size, source.data(), size - 1u, 128, height, 128, neo) ||
			    convert(destination.data(), size - 1u, source.data(), size, 128, height, 128, neo) ||
			    convert(nullptr, size, source.data(), size, 128, height, 128, neo) ||
			    convert(destination.data(), size, nullptr, size, 128, height, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 0, height, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 128, 0, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 129, height, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 64, height, 192, neo) ||
			    convert(destination.data(), size, source.data(), size, 1, UINT32_MAX, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 1, 1, UINT32_MAX, neo) ||
			    convert(destination.data(), UINT64_MAX, source.data(), size, 128, height, 128, neo) ||
			    convert(destination.data(), size, source.data(), UINT64_MAX, 128, height, 128, neo) ||
			    convert(destination.data(), size, destination.data(), size, 128, height, 128, neo) ||
			    convert(destination.data() + 4, size, destination.data(), size, 128, height, 128, neo) ||
			    convert(destination.data(), size, destination.data() + 4, size, 128, height, 128, neo) ||
			    source != original_source || destination != original_destination) return false;
		}
	}
	return true;
}

static bool CheckColor2dThinLayout()
{
	for (const bool neo: {false, true})
	{
		for (const auto& shape: {std::array<uint32_t, 3> {1, 1, 128}, {960, 540, 1024}, {129, 65, 256}})
		{
			const uint32_t physical_height = ((shape[1] + (neo ? 127u : 63u)) / (neo ? 128u : 64u)) * (neo ? 128u : 64u);
			TileSizeAlign total {};
			TileSizeOffset level {};
			TilePaddedSize padded {};
			if (!TileTryGetColor2dThinTextureSize(10, 0, shape[0], shape[1], shape[2], 1, 14, neo, false,
			                                      &total, &level, &padded) ||
			    total.size != uint64_t {shape[2]} * physical_height * 4u || total.align != (neo ? 65536u : 32768u) ||
			    level.size != total.size || level.offset != 0 || padded.width != shape[2] || padded.height != physical_height) return false;
		}
		// Rejections leave all output records intact, including overflow during
		// height alignment or multiplication of otherwise aligned dimensions.
		constexpr uint32_t rejected[][8] = {
		    {1,0,64,64,128,1,14,0}, {10,9,64,64,128,1,14,0}, {10,0,0,64,128,1,14,0},
		    {10,0,64,0,128,1,14,0}, {10,0,129,64,128,1,14,0}, {10,0,64,64,0,1,14,0},
		    {10,0,64,64,192,1,14,0}, {10,0,64,64,128,0,14,0}, {10,0,64,64,128,2,14,0},
		    {10,0,64,64,128,1,13,0}, {10,0,64,64,128,1,14,1},
		    {10,0,1,UINT32_MAX,128,1,14,0}, {10,0,1,1,0xffffff80u,1,14,0},
		    {10,0,1,1,0x1000000u,1,14,0},
		};
		for (const auto& values: rejected)
		{
			TileSizeAlign total {0x515u, 0x616u};
			TileSizeOffset level {0x717u, 0x818u};
			TilePaddedSize padded {0x919u, 0xa1au};
			if (TileTryGetColor2dThinTextureSize(values[0], values[1], values[2], values[3], values[4], values[5], values[6],
			                                   neo, values[7] != 0, &total, &level, &padded) ||
			    total.size != 0x515u || total.align != 0x616u || level.size != 0x717u || level.offset != 0x818u ||
			    padded.width != 0x919u || padded.height != 0xa1au) return false;
		}
	}
	return true;
}

static uint64_t ReferenceColor2dThinOffset(uint32_t x, uint32_t y, uint32_t pitch, bool neo)
{
	// Scalar reference shares the already tested macro allocation with display
	// tiling, then replaces only its low eight address bits. Literal vectors
	// below anchor the color element order independently of either converter.
	const uint32_t element = (x & 7u) | ((y & 2u) << 2u) | ((y & 1u) << 4u) | ((y & 4u) << 3u);
	return (ReferenceDisplay2dThinOffset(x, y, pitch, neo) & ~uint64_t {255}) | (element * 4u);
}

static bool CheckColor2dThinAddressFixtures()
{
	struct Address { uint32_t x, y, base, neo; };
	constexpr Address addresses[] = {
	    {0,0,0,0}, {1,0,4,4}, {4,0,16,16}, {0,1,64,64}, {0,2,32,32}, {7,7,252,252},
	    {8,0,256,256}, {0,8,16640,33024}, {0,64,71680,12288}, {128,0,36864,69632},
	    {0,128,131072,131072}, {255,255,258556,246268},
	};
	for (const bool neo: {false, true})
	{
		std::vector<uint32_t> linear(256u * 256u), tiled(linear.size(), 0xa5a5a5a5u);
		std::vector<uint32_t> fixture(linear.size(), 0), decoded(linear.size(), 0);
		for (uint32_t index = 0; index != linear.size(); ++index) linear[index] = index + 1u;
		if (!TileTryConvertLinearToColor2dThin(tiled.data(), tiled.size() * 4u, linear.data(), linear.size() * 4u,
		                                     256, 256, 256, neo)) return false;
		for (const auto& point: addresses)
		{
			const uint32_t offset = neo ? point.neo : point.base;
			const uint32_t identity = point.y * 256u + point.x + 1u;
			if (tiled[offset / 4u] != identity || ReferenceColor2dThinOffset(point.x, point.y, 256, neo) != offset) return false;
			fixture[offset / 4u] = identity;
		}
		if (!TileTryConvertColor2dThinToLinear(decoded.data(), decoded.size() * 4u, fixture.data(), fixture.size() * 4u,
		                                     256, 256, 256, neo)) return false;
		for (const auto& point: addresses)
			if (decoded[point.y * 256u + point.x] != point.y * 256u + point.x + 1u) return false;
	}
	return true;
}

static bool CheckColor2dThinConversion(uint32_t width, uint32_t height, uint32_t pitch, bool neo)
{
	constexpr size_t guard = 19, tail = 31;
	constexpr uint8_t canary = 0xa5;
	TileSizeAlign layout {};
	TilePaddedSize padded {};
	if (!TileTryGetColor2dThinTextureSize(10, 0, width, height, pitch, 1, 14, neo, false, &layout, nullptr, &padded)) return false;
	const uint64_t capacity = layout.size + tail;
	std::vector<uint8_t> linear(guard + capacity + guard, canary), full(linear.size(), canary);
	std::vector<uint8_t> tiled(linear.size(), canary), recovered(linear.size(), canary);
	for (uint32_t y = 0; y != padded.height; ++y)
		for (uint32_t x = 0; x != pitch; ++x)
		{
			const uint32_t identity = y * pitch + x + 1u;
			std::memcpy(linear.data() + guard + (uint64_t {y} * pitch + x) * 4u, &identity, 4u);
		}
	const auto original_linear = linear;
	if (!TileTryConvertLinearToColor2dThin(full.data() + guard, capacity, linear.data() + guard, capacity,
	                                     pitch, padded.height, pitch, neo)) return false;
	// Every full-plane identity appears exactly once: neither a matching inverse
	// nor a duplicated address formula can hide collisions or omitted texels.
	std::vector<bool> seen(layout.size / 4u, false);
	for (uint32_t offset = 0; offset < layout.size; offset += 4u)
	{
		uint32_t identity {};
		std::memcpy(&identity, full.data() + guard + offset, 4u);
		if (identity == 0 || identity > seen.size() || seen[identity - 1u]) return false;
		seen[identity - 1u] = true;
	}
	std::vector<uint8_t> expected_tiled(tiled), expected_linear(recovered);
	for (uint32_t y = 0; y != height; ++y)
		for (uint32_t x = 0; x != width; ++x)
		{
			const uint64_t linear_offset = (uint64_t {y} * pitch + x) * 4u;
			const uint64_t tiled_offset = ReferenceColor2dThinOffset(x, y, pitch, neo);
			if (tiled_offset > layout.size || 4u > layout.size - tiled_offset) return false;
			std::memcpy(expected_tiled.data() + guard + tiled_offset, linear.data() + guard + linear_offset, 4u);
			std::memcpy(expected_linear.data() + guard + linear_offset, linear.data() + guard + linear_offset, 4u);
		}
	const uint64_t linear_span = (uint64_t {height - 1u} * pitch + width) * 4u;
	if (!TileTryConvertLinearToColor2dThin(tiled.data() + guard, layout.size, linear.data() + guard, linear_span,
	                                     width, height, pitch, neo) || tiled != expected_tiled || linear != original_linear ||
	    !TileTryConvertColor2dThinToLinear(recovered.data() + guard, linear_span, tiled.data() + guard, layout.size,
	                                     width, height, pitch, neo) || recovered != expected_linear || tiled != expected_tiled) return false;
	return std::all_of(full.begin(), full.begin() + guard, [](uint8_t value) { return value == canary; }) &&
	       std::all_of(full.begin() + guard + layout.size, full.end(), [](uint8_t value) { return value == canary; });
}

static bool CheckColor2dThinRejectedConversions()
{
	for (const bool neo: {false, true})
	{
		const uint32_t height = neo ? 128u : 64u;
		const uint64_t size = uint64_t {128} * height * 4u;
		std::vector<uint8_t> source(size, 0x3c), destination(size, 0xa5);
		const auto original_source = source, original_destination = destination;
		for (const auto convert: {TileTryConvertLinearToColor2dThin, TileTryConvertColor2dThinToLinear})
		{
			if (convert(destination.data(), size, source.data(), size - 1u, 128, height, 128, neo) ||
			    convert(destination.data(), size - 1u, source.data(), size, 128, height, 128, neo) ||
			    convert(nullptr, size, source.data(), size, 128, height, 128, neo) ||
			    convert(destination.data(), size, nullptr, size, 128, height, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 0, height, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 128, 0, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 129, height, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 64, height, 192, neo) ||
			    convert(destination.data(), size, source.data(), size, 1, UINT32_MAX, 128, neo) ||
			    convert(destination.data(), size, source.data(), size, 1, 1, 0xffffff80u, neo) ||
			    convert(destination.data(), UINT64_MAX, source.data(), size, 128, height, 128, neo) ||
			    convert(destination.data(), size, source.data(), UINT64_MAX, 128, height, 128, neo) ||
			    convert(destination.data(), size, destination.data(), size, 128, height, 128, neo) ||
			    convert(destination.data() + 4u, size, destination.data(), size, 128, height, 128, neo) ||
			    convert(destination.data(), size, destination.data() + 4u, size, 128, height, 128, neo) ||
			    source != original_source || destination != original_destination) return false;
		}
	}
	return true;
}

static bool CheckDisplay2dThinRandomizedConversions()
{
	// Compare each direction against the independent scalar address oracle,
	// including every padding/guard byte. Random source data avoids relying
	// on a matching inverse or only on the sequential identities above.
	uint32_t state = 0x7d4698a3u;
	auto random = [&state]() {
		state ^= state << 13u;
		state ^= state >> 17u;
		state ^= state << 5u;
		return state;
	};
	for (const bool neo: {false, true})
	{
		for (uint32_t sample = 0; sample < 64u; ++sample)
		{
			const uint32_t pitch = (1u + random() % 12u) * 128u;
			const uint32_t width = 1u + random() % pitch;
			const uint32_t height = 1u + random() % 385u;
			const size_t guard = 1u + random() % 31u;
			TileSizeAlign layout;
			if (!TileTryGetDisplay2dThinTextureSize(10, 0, width, height, pitch, 1, 10, neo, false,
			                                       &layout, nullptr, nullptr)) return false;
			std::vector<uint8_t> source(guard + layout.size + 37u);
			for (auto& byte: source) byte = static_cast<uint8_t>(random());
			const auto original_source = source;
			std::vector<uint8_t> expected_tiled(source.size(), 0xa5);
			std::vector<uint8_t> expected_linear(source.size(), 0xa5);
			std::vector<uint8_t> tiled(source.size(), 0xa5);
			std::vector<uint8_t> linear(source.size(), 0xa5);
			for (uint32_t y = 0; y < height; ++y)
			{
				for (uint32_t x = 0; x < width; ++x)
				{
					const uint64_t tiled_offset = ReferenceDisplay2dThinOffset(x, y, pitch, neo);
					const uint64_t linear_offset = (static_cast<uint64_t>(y) * pitch + x) * 4u;
					if (tiled_offset > layout.size || 4u > layout.size - tiled_offset) return false;
					std::memcpy(expected_tiled.data() + guard + tiled_offset, source.data() + guard + linear_offset, 4u);
					std::memcpy(expected_linear.data() + guard + linear_offset, source.data() + guard + tiled_offset, 4u);
				}
			}
			const uint64_t linear_span = (static_cast<uint64_t>(height - 1u) * pitch + width) * 4u;
			if (!TileTryConvertLinearToDisplay2dThin(tiled.data() + guard, layout.size, source.data() + guard, linear_span,
			                                       width, height, pitch, neo) ||
			    !TileTryConvertDisplay2dThinToLinear(linear.data() + guard, linear_span, source.data() + guard, layout.size,
			                                       width, height, pitch, neo) ||
			    tiled != expected_tiled || linear != expected_linear || source != original_source)
			{
				std::fprintf(stderr, "Display_2dThin random byte comparison failed: sample=%u %ux%u pitch=%u neo=%u\n",
				             sample, width, height, pitch, neo ? 1u : 0u);
				return false;
			}
		}
	}
	return true;
}

static bool CheckDisplay2dThinInitializedPool()
{
	// The earlier suite exercises g_tiler == nullptr. Initialize the actual
	// production Tiler/AsyncJob pool here so selector=1 must also exercise
	// worker joins, not only the serial fallback used by standalone callers.
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Libs::Graphics::TileInit();
	for (const bool neo: {false, true})
	{
		// Keep 16 MiB serial, then exercise the exact 64 MiB threshold and
		// ragged right/bottom edges above it with the real production workers.
		for (const auto& shape: {std::array<uint32_t, 3>{2048, 2048, 2048},
		                         std::array<uint32_t, 3>{4096, 4096, 4096},
		                         std::array<uint32_t, 3>{4097, 4097, 4224}})
		{
			if (!CheckDisplay2dThinConversion(shape[0], shape[1], shape[2], neo)) return false;
		}
	}
	// Concurrent callers contend for the same production pool. Its workers
	// may serve only one owner; busy callers must still finish correctly via
	// serial conversion, with no cross-call stack argument or buffer reuse.
	std::atomic_uint ready {0};
	std::atomic_bool start {false};
	std::array<bool, 3> results {};
	std::array<std::thread, 3> callers;
	for (size_t index = 0; index < callers.size(); ++index)
	{
		callers[index] = std::thread([&, index] {
			ready.fetch_add(1, std::memory_order_release);
			while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
			results[index] = CheckDisplay2dThinConversion(4096, 4096, 4096, (index & 1u) != 0);
		});
	}
	while (ready.load(std::memory_order_acquire) != callers.size()) std::this_thread::yield();
	start.store(true, std::memory_order_release);
	for (auto& caller: callers) caller.join();
	return std::all_of(results.begin(), results.end(), [](bool result) { return result; }) &&
	       CheckDisplay2dThinRejectedConversions();
}

static bool CheckDepthSurfaceLayout()
{
	struct Shape
	{
		uint32_t width;
		uint32_t height;
		uint32_t pitch;
		uint32_t slice_pixels;
		uint32_t htile_base;
		uint32_t htile_neo;
	};
	constexpr Shape shapes[] = {
	    {1280, 720, 1280, 983040, 98304, 131072},
	    {1920, 1080, 2048, 2359296, 196608, 196608},
	    {3840, 2160, 3840, 8355840, 655360, 655360},
	};
	for (const auto& shape: shapes)
	{
		for (const bool neo: {false, true})
		{
			for (const uint32_t depth_format: {0u, 1u, 3u})
			{
				for (const uint32_t stencil_format: {0u, 1u})
				{
					if (depth_format == 0 && stencil_format == 0) continue;
					for (const bool htile: {false, true})
					{
						const DepthSurfaceLayoutRequest request {
						    shape.width, shape.height, shape.pitch / 8u - 1u, shape.slice_pixels / 64u - 1u,
						    depth_format, stencil_format, htile, neo, 0x10001200u, 0x20000200u, 0x30000100u};
						DepthSurfaceLayout layout {};
						const uint32_t depth_bytes = shape.slice_pixels * (depth_format == 3 ? 4u : depth_format == 1 ? 2u : 0u);
						const uint32_t stencil_bytes = stencil_format != 0 ? shape.slice_pixels : 0u;
						const uint32_t htile_bytes = htile ? (neo ? shape.htile_neo : shape.htile_base) : 0u;
						if (!ResolveDepthSurfaceLayout(request, &layout) ||
						    layout.depth.allocation.size != depth_bytes ||
						    layout.depth.allocation.align != (depth_format != 0 ? (neo ? 65536u : 32768u) : 0u) ||
						    layout.depth.address != (depth_format != 0 ? 0x10000000u : 0u) ||
						    layout.depth.swizzle != (depth_format != 0 ? 0x1200u : 0u) ||
						    layout.stencil.allocation.size != stencil_bytes ||
						    layout.stencil.allocation.align != (stencil_format != 0 ? 32768u : 0u) ||
						    layout.stencil.address != (stencil_format != 0 ? 0x20000000u : 0u) ||
						    layout.stencil.swizzle != (stencil_format != 0 ? 0x200u : 0u) ||
						    layout.htile.allocation.size != htile_bytes ||
						    layout.htile.allocation.align != (htile ? (neo ? 4096u : 2048u) : 0u) ||
						    layout.htile.address != (htile ? 0x30000000u : 0u) ||
						    layout.htile.swizzle != (htile ? 0x100u : 0u))
						{
							std::fprintf(stderr, "depth layout fixture failed: %ux%u neo=%u depth=%u stencil=%u htile=%u\n",
							             shape.width, shape.height, neo, depth_format, stencil_format, htile);
							return false;
						}
					}
				}
			}
		}
	}

	// Preserve the known Neo 1920-pitch layout and the existing depth-only
	// slice fallback. Inactive planes must ignore their stale raw addresses.
	DepthSurfaceLayout layout {};
	DepthSurfaceLayoutRequest request {1920, 1080, 239, 34559, 3, 0, true, true,
	                                   0x10000000u, UINT64_MAX, 0x30000000u};
	if (!ResolveDepthSurfaceLayout(request, &layout) || layout.depth.allocation.size != 8847360u ||
	    layout.htile.allocation.size != 196608u || layout.stencil.address != 0 || layout.stencil.swizzle != 0) return false;
	for (const bool neo: {false, true})
	{
		for (const uint32_t depth_format: {1u, 3u})
		{
			request = {96, 64, 15, 127, depth_format, 0, false, neo, 0x10000000u, UINT64_MAX, UINT64_MAX};
			if (!ResolveDepthSurfaceLayout(request, &layout) ||
			    layout.depth.allocation.size != (depth_format == 3 ? 32768u : 16384u) ||
			    layout.depth.allocation.align != (neo ? 65536u : 32768u) ||
			    layout.stencil.address != 0 || layout.stencil.swizzle != 0 ||
			    layout.htile.address != 0 || layout.htile.swizzle != 0) return false;
			request.width = request.height = 8;
			request.pitch_div8_minus1 = 0;
			request.slice_div64_minus1 = depth_format == 3 ? 0xfffffeu : 0x1fffffeu;
			if (!ResolveDepthSurfaceLayout(request, &layout) ||
			    layout.depth.allocation.size != (depth_format == 3 ? 0xffffff00u : 0xffffff80u)) return false;
		}
	}

	const DepthSurfaceLayoutRequest valid {1920, 1080, 255, 36863, 3, 1, true, false,
	                                        0x10000000u, 0x20000000u, 0x30000000u};
	const auto expect_rejected = [&](const char* name, auto change)
	{
		auto candidate = valid;
		change(candidate);
		DepthSurfaceLayout output {{{11, 16}, 17, 18}, {{21, 32}, 33, 34}, {{41, 64}, 65, 66}};
		const auto before = output;
		const auto same_range = [](const auto& a, const auto& b)
		{
			return a.allocation.size == b.allocation.size && a.allocation.align == b.allocation.align &&
			       a.address == b.address && a.swizzle == b.swizzle;
		};
		if (ResolveDepthSurfaceLayout(candidate, &output) || !same_range(output.depth, before.depth) ||
		    !same_range(output.stencil, before.stencil) || !same_range(output.htile, before.htile))
		{
			std::fprintf(stderr, "invalid depth layout accepted or output mutated: %s\n", name);
			return false;
		}
		return true;
	};
	if (!expect_rejected("missing stencil layout", [](auto& q) { q.width = 1919; q.htile = false; }) ||
	    !expect_rejected("missing metadata layout", [](auto& q) { q.width = 1919; q.stencil_format = 0; }) ||
	    !expect_rejected("zero width", [](auto& q) { q.width = 0; }) ||
	    !expect_rejected("zero height", [](auto& q) { q.height = 0; }) ||
	    !expect_rejected("width exceeds pitch", [](auto& q) { q.width = 2049; }) ||
	    !expect_rejected("pitch multiplication wraps", [](auto& q) { q.pitch_div8_minus1 = 0x1fffffffu; }) ||
	    !expect_rejected("pitch addition wraps", [](auto& q) { q.pitch_div8_minus1 = UINT32_MAX; }) ||
	    !expect_rejected("slice addition wraps", [](auto& q) { q.slice_div64_minus1 = UINT32_MAX; }) ||
	    !expect_rejected("depth size wraps", [](auto& q) { q.slice_div64_minus1 = 0xffffffu; }) ||
	    !expect_rejected("D16 size wraps", [](auto& q) { q.depth_format = 1; q.slice_div64_minus1 = 0x1ffffffu; }) ||
	    !expect_rejected("stencil size wraps", [](auto& q) { q.depth_format = 0; q.slice_div64_minus1 = 0x3ffffffu; }) ||
	    !expect_rejected("slice cannot cover image", [](auto& q) { q.slice_div64_minus1 = 0; }) ||
	    !expect_rejected("table and slice mismatch", [](auto& q) { ++q.slice_div64_minus1; }) ||
	    !expect_rejected("unknown depth format", [](auto& q) { q.depth_format = 2; }) ||
	    !expect_rejected("unknown stencil format", [](auto& q) { q.stencil_format = 2; }) ||
	    !expect_rejected("no planes", [](auto& q) { q.depth_format = q.stencil_format = 0; }) ||
	    !expect_rejected("null depth", [](auto& q) { q.depth_address = 0; }) ||
	    !expect_rejected("masked null depth", [](auto& q) { q.depth_address = 0x100; }) ||
	    !expect_rejected("null stencil", [](auto& q) { q.stencil_address = 0; }) ||
	    !expect_rejected("null metadata", [](auto& q) { q.htile_address = 0; }) ||
	    !expect_rejected("depth end overflow", [](auto& q) { q.depth_address = UINT64_MAX; }) ||
	    !expect_rejected("stencil end overflow", [](auto& q) { q.stencil_address = UINT64_MAX; }) ||
	    !expect_rejected("metadata end overflow", [](auto& q) { q.htile_address = UINT64_MAX; }) ||
	    ResolveDepthSurfaceLayout(valid, nullptr)) return false;
	return true;
}

static bool CheckBaseDepthMacroLayout()
{
	using namespace Lumen::Libs::Graphics;
	HW::DepthRenderTarget target {};
	target.width = 960; target.height = 540;
	target.pitch_div8_minus1 = 127; target.height_div8_minus1 = 79; target.slice_div64_minus1 = 10239;
	target.z_info.format = 3; target.z_info.zrange_precision = 1; target.z_info.tile_surface_enable = true;
	target.stencil_info.format = 1; target.stencil_info.tile_stencil_disable = true;
	target.depth_info = {0, 4, 0xc, 0, 2, 2, 3};
	target.z_read_base_addr = target.z_write_base_addr = 0x10000000;
	target.stencil_read_base_addr = target.stencil_write_base_addr = 0x20000000;
	target.htile_data_base_addr = 0x30000000;

	// Independently calculated complete macro/cache extents. Visible dimensions
	// deliberately vary within each allocation; they cannot select the size.
	struct Extent { uint32_t width, height, pitch, allocated_height, depth, stencil, metadata; };
	constexpr Extent extents[] {
	    {960, 540, 1024, 640, 2621440, 655360, 65536},
	    {13, 7, 256, 128, 131072, 32768, 16384},
	    {500, 511, 512, 512, 1048576, 262144, 16384},
	    {500, 513, 512, 640, 1310720, 327680, 32768},
	    {767, 127, 768, 128, 393216, 98304, 32768},
	    {1279, 719, 1280, 768, 3932160, 983040, 98304},
	    {1919, 1079, 2048, 1152, 9437184, 2359296, 196608},
	    {3839, 2159, 3840, 2176, 33423360, 8355840, 655360},
	};
	for (const auto& extent: extents)
	{
		auto z = target;
		z.width = extent.width; z.height = extent.height;
		z.pitch_div8_minus1 = extent.pitch / 8u - 1u;
		z.height_div8_minus1 = extent.allocated_height / 8u - 1u;
		z.slice_div64_minus1 = extent.pitch * extent.allocated_height / 64u - 1u;
		for (const uint32_t addr5: {0u, 1u})
		{
			z.depth_info.addr5_swizzle_mask = addr5;
			for (const bool preload: {false, true})
			{
				z.htile_surface.raw = preload ? 8 : 0; z.htile_surface.preload = preload ? 1 : 0;
				DepthSurfaceLayout result {};
				if (!ResolveDepthSurfaceLayout(z, false, &result) || result.depth.allocation.size != extent.depth ||
				    result.stencil.allocation.size != extent.stencil || result.htile.allocation.size != extent.metadata ||
				    result.depth.allocation.align != 32768 || result.stencil.allocation.align != 32768 ||
				    result.htile.allocation.align != 2048 || result.depth.address != z.z_read_base_addr ||
				    result.stencil.address != z.stencil_read_base_addr || result.htile.address != z.htile_data_base_addr)
				{
					std::fprintf(stderr, "base depth macro allocation rejected or incomplete: %ux%u\n", z.width, z.height);
					return false;
				}
			}
		}
	}
	const auto rejected = [&](auto change)
	{
		auto z = target; change(z);
		DepthSurfaceLayout result {{{11, 16}, 17, 18}, {{21, 32}, 33, 34}, {{41, 64}, 65, 66}};
		const auto before = result;
		return !ResolveDepthSurfaceLayout(z, false, &result) &&
		       std::memcmp(&result, &before, sizeof(result)) == 0;
	};
	for (uint32_t field = 0; field < 49; ++field)
	{
		if (!rejected([&](auto& z) {
			switch (field)
			{
				case 0: z.width = 0; break;
				case 1: z.height = 0; break;
				case 2: z.width = 1025; break;
				case 3: z.height = 641; break;
				case 4: z.pitch_div8_minus1 = UINT32_MAX; break;
				case 5: z.slice_div64_minus1 = UINT32_MAX; break;
				case 6: z.pitch_div8_minus1 = 128; break;
				case 7: ++z.slice_div64_minus1; break;
				case 8: z.slice_div64_minus1 = 10223; break; // Integral height639, incomplete macro.
				case 9: z.z_info.format = 1; break;
				case 10: z.stencil_info.format = 0; break;
				case 11: z.z_info.tile_mode_index = 2; break;
				case 12: z.stencil_info.tile_mode_index = 2; break;
				case 13: z.z_info.num_samples = 1; break;
				case 14: z.z_info.zrange_precision = 0; break;
				case 15: z.z_info.tile_surface_enable = false; break;
				case 16: z.z_info.expclear_enabled = true; break;
				case 17: z.z_info.embedded_sample_locations = true; break;
				case 18: z.z_info.partially_resident = true; break;
				case 19: z.z_info.num_mip_levels = 1; break;
				case 20: z.z_info.plane_compression = 1; break;
				case 21: z.stencil_info.tile_split = 1; break;
				case 22: z.stencil_info.expclear_enabled = true; break;
				case 23: z.stencil_info.tile_stencil_disable = false; break;
				case 24: z.stencil_info.texture_compatible_stencil = true; break;
				case 25: z.stencil_info.partially_resident = true; break;
				case 26: z.depth_info.addr5_swizzle_mask = 2; break;
				case 27: z.depth_info.array_mode = 3; break;
				case 28: z.depth_info.pipe_config = 0x12; break;
				case 29: z.depth_info.bank_width = 1; break;
				case 30: z.depth_info.bank_height = 1; break;
				case 31: z.depth_info.macro_tile_aspect = 1; break;
				case 32: z.depth_info.num_banks = 2; break;
				case 33: z.htile_surface.raw = 9; break;
				case 34: z.htile_surface.preload = 1; break;
				case 35: z.htile_surface.full_cache = 1; break;
				case 36: z.htile_surface.prefetch_width = 1; break;
				case 37: z.depth_view.slice_max = 1; break;
				case 38: z.depth_view.current_mip_level = 1; break;
				case 39: z.z_write_base_addr += 256; break;
				case 40: z.stencil_read_base_addr = z.stencil_write_base_addr = UINT64_MAX; break;
				case 41: z.htile_data_base_addr = 0; break;
				case 42: z.htile_data_base_addr = UINT64_MAX; break;
				case 43: z.z_info.unsupported_bits = 0x80000000u; break;
				case 44: z.stencil_info.unsupported_bits = 0x80000000u; break;
				case 45: z.depth_info.unsupported_bits = 0x80000000u; break;
				case 46: ++z.height_div8_minus1; break;
				case 47: z.depth_size_unsupported_bits = 0x80000000u; break;
				case 48: z.depth_slice_unsupported_bits = 0x80000000u; break;
			}
		})) { std::fprintf(stderr, "base macro negative accepted or mutated: %u\n", field); return false; }
	}
	DepthSurfaceLayout result {};
	if (ResolveDepthSurfaceLayout(target, true, &result) || ResolveDepthSurfaceLayout(target, false, nullptr)) return false;
	if (!rejected([](auto& z) {
		z.width = 1920; z.height = 1080; z.pitch_div8_minus1 = 255;
		z.height_div8_minus1 = 159;
		z.slice_div64_minus1 = 40959; // Known fixture with a larger whole-macro slice.
	})) return false;
	std::printf("base depth macro allocation: 32 positive and 52 negative cases passed\n");
	return true;
}

static bool CheckUniformDepthZeroBytes()
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
	using namespace Lumen::Libs::Graphics;
	namespace VM = Lumen::Core::VirtualMemory;
	VM::Init();
	uint64_t addresses[3] {};
	uint64_t sizes[3] {4096, 4096, 4096};
	bool ok = true;
	for (int plane = 0; plane < 3; ++plane)
	{
		addresses[plane] = VM::Alloc(0, sizes[plane], VM::Mode::ReadWrite);
		if (addresses[plane] == 0) { ok = false; break; }
		std::memset(reinterpret_cast<void*>(addresses[plane]), plane == 0 ? 0xa5 : 0, sizes[plane]);
	}
	if (ok)
	{
		uint64_t generation = VM::GetMutationGeneration();
		ok = DepthSurfaceUniformZeroBytes(addresses, sizes, 3, generation);
		for (int plane: {1, 2})
		{
			auto* data = reinterpret_cast<uint8_t*>(addresses[plane]);
			data[sizes[plane] - 1] = 1;
			ok = !DepthSurfaceUniformZeroBytes(addresses, sizes, 3, generation) && ok;
			data[sizes[plane] - 1] = 0;
		}
		const auto original = addresses[2]; addresses[2] = addresses[1];
		ok = !DepthSurfaceUniformZeroBytes(addresses, sizes, 3, generation) && ok; addresses[2] = original;
		ok = !DepthSurfaceUniformZeroBytes(addresses, sizes, 2, generation) && ok;
		ok = VM::Protect(addresses[1], sizes[1], VM::Mode::NoAccess) && ok;
		generation = VM::GetMutationGeneration();
		ok = !DepthSurfaceUniformZeroBytes(addresses, sizes, 3, generation) && ok;
		ok = VM::Protect(addresses[1], sizes[1], VM::Mode::ReadWrite) && ok;
		generation = VM::GetMutationGeneration();
		ok = DepthSurfaceUniformZeroBytes(addresses, sizes, 3, generation) && ok;
		std::array<uint8_t, 4096> native {};
		addresses[2] = reinterpret_cast<uint64_t>(native.data());
		ok = !DepthSurfaceUniformZeroBytes(addresses, sizes, 3, generation) && ok; addresses[2] = original;
		const auto old_size = sizes[2]; sizes[2] = HTILE_CPU_OBSERVATION_MAX_BYTES + 4u;
		ok = !DepthSurfaceUniformZeroBytes(addresses, sizes, 3, generation) && ok; sizes[2] = old_size;
		ok = !DepthSurfaceUniformZeroBytes(nullptr, sizes, 3, generation) && ok;
	}
	for (const auto address: addresses) if (address != 0) ok = VM::Free(address) && ok;
	return ok;
#else
	return true;
#endif
}

static bool CheckUniformDepthZeroProfile()
{
	using namespace Lumen::Libs::Graphics;
	HW::ShaderRegisters shader {};
	for (const uint8_t behavior: {0, 1})
	{
		shader.db_shader_control.shader_z_behavior = behavior;
		if (!DepthSurfaceUniformZeroShaderProfile(shader)) return false;
	}
	// These interfaces are checked at every attachment request, independently
	// of shader parsing, embedded mode or a later pipeline-cache hit.
	const auto valid_shader = shader;
	for (unsigned field = 0; field < 6; ++field)
	{
		shader = valid_shader;
		switch (field)
		{
			case 0: shader.shader_z_format = 1; break;
			case 1: shader.db_shader_control.shader_z_export_enable = true; break;
			case 2: shader.db_shader_control.conservative_z_export_value = 1; break;
			case 3: shader.db_shader_control.other_bits = 1; break;
			case 4: shader.db_shader_control.shader_z_behavior = 2; break;
			case 5: shader.db_shader_control.shader_z_behavior = 3; break;
		}
		if (DepthSurfaceUniformZeroShaderProfile(shader)) return false;
	}
	shader = valid_shader;
	shader.db_shader_control.shader_kill_enable = true;
	shader.db_shader_control.shader_execute_on_noop = true;
	if (!DepthSurfaceUniformZeroShaderProfile(shader)) return false;
	for (const bool neo: {false, true})
	{
		HW::DepthRenderTarget z {};
		z.width = 1280; z.height = 720;
		z.pitch_div8_minus1 = 159; z.slice_div64_minus1 = 15359;
		z.z_read_base_addr = z.z_write_base_addr = 0x10000000;
		z.stencil_read_base_addr = z.stencil_write_base_addr = 0x20000000;
		z.htile_data_base_addr = 0x30000000;
		z.z_info.format = 3; z.z_info.tile_mode_index = neo ? 2 : 0;
		z.z_info.zrange_precision = 1; z.z_info.tile_surface_enable = true;
		z.stencil_info.format = 1; z.stencil_info.tile_mode_index = neo ? 2 : 0;
		z.stencil_info.tile_split = neo ? 2 : 0; z.stencil_info.tile_stencil_disable = true;
		z.depth_info = {1, 4, neo ? 0x12u : 0xcu, 0, neo ? 1u : 2u, neo ? 0u : 2u, neo ? 2u : 3u};
		DepthSurfaceLayout layout {};
		if (!ResolveDepthSurfaceLayout({z.width, z.height, z.pitch_div8_minus1, z.slice_div64_minus1, 3, 1, true, neo,
		                                z.z_read_base_addr, z.stencil_read_base_addr, z.htile_data_base_addr}, &layout)) return false;
		HW::RenderControl rc {}; rc.stencil_clear_enable = true;
		rc.raw = Pm4::DB_RENDER_CONTROL_STENCIL_CLEAR_ENABLE_MASK << Pm4::DB_RENDER_CONTROL_STENCIL_CLEAR_ENABLE_SHIFT;
		HW::DepthControl dc {}; dc.stencil_enable = true; dc.stencilfunc = 7;
		HW::StencilControl sc {}; sc.stencil_fail = sc.stencil_zpass = sc.stencil_zfail = 3;
		HW::StencilMask sm {}; sm.stencil_mask = sm.stencil_writemask = 255;
		auto accepts = [&] { return DepthSurfaceUniformZeroProfile(z, neo, layout, rc, dc, sc, sm, 0, 0); };
		HW::RenderControl reuse_rc {};
		HW::DepthControl reuse_dc {};
		reuse_dc.z_enable = reuse_dc.z_write_enable = reuse_dc.stencil_enable = reuse_dc.backface_enable = true;
		reuse_dc.zfunc = reuse_dc.stencilfunc = reuse_dc.stencilfunc_bf = 7;
		HW::StencilControl reuse_sc {};
		reuse_sc.stencil_zpass = reuse_sc.stencil_zpass_bf = 1;
		HW::StencilMask reuse_sm {};
		reuse_sm.stencil_mask = reuse_sm.stencil_mask_bf = reuse_sm.stencil_writemask = reuse_sm.stencil_writemask_bf = 255;
		reuse_sm.stencil_opval = reuse_sm.stencil_opval_bf = 1;
		auto accepts_layout = [&] { return DepthSurfaceUniformZeroLayoutProfile(z, neo, layout); };
		auto accepts_reuse = [&] {
			return DepthSurfaceUniformZeroReuseProfile(z, neo, layout, reuse_rc, reuse_dc, reuse_sc, reuse_sm, 0, 0);
		};
		// Reuse grants an ordinary attachment binding, never import or proof that
		// later depth/stencil writes left the image zero. The clear import remains
		// exact and cannot be selected for this Always/write state.
		if (!accepts() || !accepts_layout() || !accepts_reuse() ||
		    DepthSurfaceUniformZeroReuseProfile(z, neo, layout, rc, dc, sc, sm, 0, 0) ||
		    DepthSurfaceUniformZeroProfile(z, neo, layout, reuse_rc, reuse_dc, sc, sm, 0, 0)) return false;
		{
			// A known allocation size does not qualify unmodeled register bits.
			// Keep its real table lookup available while revoking the uniform
			// import and reuse authority for either ADDR5 interpretation.
			const auto known_z = z;
			auto narrow_dc = reuse_dc;
			narrow_dc.z_write_enable = false;
			auto narrow_sm = reuse_sm;
			narrow_sm.stencil_opval = narrow_sm.stencil_opval_bf = 0;
			for (const uint32_t addr5: {0u, 1u})
			{
				z = known_z; z.depth_info.addr5_swizzle_mask = addr5;
				if (!accepts() || !accepts_layout() ||
				    !DepthSurfaceUniformZeroReuseProfile(z, neo, layout, reuse_rc, narrow_dc, reuse_sc, narrow_sm, 0, 0)) return false;
				for (unsigned field = 0; field < 5; ++field)
				{
					z = known_z; z.depth_info.addr5_swizzle_mask = addr5;
					switch (field)
					{
						case 0: z.z_info.unsupported_bits = 0x10u; break;
						case 1: z.stencil_info.unsupported_bits = 0x2u; break;
						case 2: z.depth_info.unsupported_bits = 0x00200000u; break;
						case 3: z.depth_size_unsupported_bits = 0x00400000u; break;
						case 4: z.depth_slice_unsupported_bits = 0x00400000u; break;
					}
					DepthSurfaceLayout known_layout {};
					if (!ResolveDepthSurfaceLayout(z, neo, &known_layout) ||
					    known_layout.depth.allocation.size != layout.depth.allocation.size ||
					    known_layout.stencil.allocation.size != layout.stencil.allocation.size ||
					    known_layout.htile.allocation.size != layout.htile.allocation.size ||
					    accepts() || accepts_layout() ||
					    DepthSurfaceUniformZeroReuseProfile(z, neo, known_layout, reuse_rc, narrow_dc, reuse_sc, narrow_sm, 0, 0)) return false;
				}
			}
			z = known_z;
		}
		{
			const auto mask_one_z = z;
			z.depth_info.addr5_swizzle_mask = 0;
			// The same known complete footprint can represent uniform zero with
			// ADDR5=0. It cannot authorize ordinary nonzero depth/stencil writes.
			if (!accepts_layout() || !accepts() || accepts_reuse()) return false;
			auto zero_dc = reuse_dc;
			zero_dc.z_write_enable = false;
			auto zero_sc = reuse_sc;
			auto zero_sm = reuse_sm;
			zero_sm.stencil_opval = zero_sm.stencil_opval_bf = 0;
			const auto zero_accepts = [&] {
				return DepthSurfaceUniformZeroReuseProfile(z, neo, layout, reuse_rc, zero_dc, zero_sc, zero_sm, 0, 0);
			};
			if (!zero_accepts()) return false;
			const auto valid_zero_dc = zero_dc;
			const auto valid_zero_sc = zero_sc;
			const auto valid_zero_sm = zero_sm;
			for (const bool backface: {false, true})
			{
				for (unsigned outcome = 0; outcome < 3; ++outcome)
				{
					for (uint8_t op = 0; op < 16; ++op)
					{
						zero_dc = valid_zero_dc; zero_dc.backface_enable = backface;
						zero_sc = valid_zero_sc; zero_sm = valid_zero_sm;
						(outcome == 0 ? zero_sc.stencil_fail : outcome == 1 ? zero_sc.stencil_zpass : zero_sc.stencil_zfail) = op;
						(outcome == 0 ? zero_sc.stencil_fail_bf : outcome == 1 ? zero_sc.stencil_zpass_bf : zero_sc.stencil_zfail_bf) = op;
						if (zero_accepts() != (op == 0 || op == 1 || op == 3 || op == 4)) return false;
						if (op == 3 || op == 4)
						{
							for (const uint8_t value: {1, 255})
							{
								if (op == 3) zero_sm.stencil_testval = zero_sm.stencil_testval_bf = value;
								else zero_sm.stencil_opval = zero_sm.stencil_opval_bf = value;
								if (zero_accepts()) return false;
							}
						}
					}
				}
			}
			zero_dc = valid_zero_dc; zero_sc = valid_zero_sc; zero_sm = valid_zero_sm;
			for (const bool stencil_enabled: {false, true})
			{
				zero_dc.stencil_enable = stencil_enabled;
				zero_dc.z_write_enable = true;
				if (zero_accepts()) return false;
				zero_dc.z_write_enable = false;
				if (!zero_accepts()) return false;
			}
			zero_dc = valid_zero_dc;
			for (uint8_t compare = 0; compare < 7; ++compare)
			{
				zero_dc.stencilfunc = zero_dc.stencilfunc_bf = compare;
				if (zero_accepts()) return false;
			}
			zero_dc = valid_zero_dc;
			for (unsigned mismatch = 0; mismatch < 8; ++mismatch)
			{
				zero_dc = valid_zero_dc; zero_sc = valid_zero_sc; zero_sm = valid_zero_sm;
				switch (mismatch)
				{
					case 0: zero_dc.stencilfunc_bf = 6; break;
					case 1: zero_sc.stencil_fail_bf = 1; break;
					case 2: zero_sc.stencil_zpass_bf = 0; break;
					case 3: zero_sc.stencil_zfail_bf = 1; break;
					case 4: zero_sm.stencil_testval_bf = 1; break;
					case 5: zero_sm.stencil_mask_bf = 0; break;
					case 6: zero_sm.stencil_writemask_bf = 0; break;
					case 7: zero_sm.stencil_opval_bf = 1; break;
				}
				if (zero_accepts()) return false;
			}
			zero_dc = valid_zero_dc; zero_sc = valid_zero_sc; zero_sm = valid_zero_sm;
			z.z_info.format = 1;
			if (accepts_layout() || accepts() || zero_accepts()) return false;
			z = mask_one_z;
			for (uint32_t mask = 2; mask < 16; ++mask)
			{
				z.depth_info.addr5_swizzle_mask = mask;
				if (accepts_layout() || accepts() || zero_accepts()) return false;
			}
			DepthSurfaceLayout unknown_layout {};
			if (ResolveDepthSurfaceLayout({960, 540, 127, 0x27ffu, 3, 1, true, neo,
			    z.z_read_base_addr, z.stencil_read_base_addr, z.htile_data_base_addr}, &unknown_layout)) return false;
			z = mask_one_z;
			// The original mask-one path still admits ordinary writes for a later
			// full native-content proof; the mask-zero preservation gate is local.
			if (!accepts_reuse()) return false;
		}
		// A clear-register change without clear-enable still needs a metadata
		// interpretation that the zero-only import does not provide.
		for (const uint32_t clear: {1u, 0x80000000u, 0x3f800000u, 0x7fc00000u})
			if (DepthSurfaceUniformZeroReuseProfile(z, neo, layout, reuse_rc, reuse_dc, reuse_sc, reuse_sm, clear, 0)) return false;
		for (const uint8_t clear: {1, 255})
			if (DepthSurfaceUniformZeroReuseProfile(z, neo, layout, reuse_rc, reuse_dc, reuse_sc, reuse_sm, 0, clear)) return false;
		const auto valid_reuse_dc = reuse_dc;
		reuse_dc.z_enable = reuse_dc.z_write_enable = reuse_dc.stencil_enable = reuse_dc.backface_enable = false;
		if (!accepts_reuse()) return false;
		reuse_dc = valid_reuse_dc;
		for (unsigned bit = 0; bit < 32; ++bit)
		{
			reuse_rc.raw = uint32_t {1} << bit;
			if (accepts_reuse()) return false;
		}
		reuse_rc.raw = 0;
		const auto valid_reuse_rc = reuse_rc;
		for (unsigned field = 0; field < 10; ++field)
		{
			reuse_rc = valid_reuse_rc;
			reuse_dc = valid_reuse_dc;
			switch (field)
			{
				case 0: reuse_rc.depth_clear_enable = true; break;
				case 1: reuse_rc.stencil_clear_enable = true; break;
				case 2: reuse_rc.resummarize_enable = true; break;
				case 3: reuse_rc.stencil_compress_disable = true; break;
				case 4: reuse_rc.depth_compress_disable = true; break;
				case 5: reuse_rc.copy_centroid = true; break;
				case 6: reuse_rc.copy_sample = 1; break;
				case 7: reuse_dc.depth_bounds_enable = true; break;
				case 8: reuse_dc.color_writes_on_depth_fail_enable = true; break;
				case 9: reuse_dc.color_writes_on_depth_pass_disable = true; break;
			}
			if (accepts_reuse()) return false;
		}
		reuse_rc = valid_reuse_rc;
		reuse_dc = valid_reuse_dc;
		for (const bool backface: {false, true})
		{
			reuse_dc.backface_enable = backface;
			for (uint8_t compare = 0; compare < 7; ++compare)
			{
				reuse_dc.stencilfunc = compare;
				if (accepts_reuse()) return false;
				reuse_dc.stencilfunc = 7;
				reuse_dc.stencilfunc_bf = compare;
				if (accepts_reuse() != !backface) return false;
				reuse_dc.stencilfunc_bf = 7;
			}
			for (uint8_t compare = 0; compare < 8; ++compare)
			{
				reuse_dc.zfunc = compare;
				if (!accepts_reuse()) return false;
			}
			reuse_dc.zfunc = 7;
		}
		reuse_dc = valid_reuse_dc;
		const auto valid_reuse_sc = reuse_sc;
		const auto valid_reuse_sm = reuse_sm;
		for (const bool backface: {false, true})
		{
			for (unsigned outcome = 0; outcome < 3; ++outcome)
			{
				for (uint8_t op = 0; op < 16; ++op)
				{
					reuse_sc = valid_reuse_sc;
					reuse_sm = valid_reuse_sm;
					auto& operation = backface
					    ? (outcome == 0 ? reuse_sc.stencil_fail_bf : outcome == 1 ? reuse_sc.stencil_zpass_bf : reuse_sc.stencil_zfail_bf)
					    : (outcome == 0 ? reuse_sc.stencil_fail : outcome == 1 ? reuse_sc.stencil_zpass : reuse_sc.stencil_zfail);
					operation = op;
					if (accepts_reuse() != (op <= 9)) return false;
					if (op == 5 || op == 6 || op == 8 || op == 9)
					{
						for (const uint8_t value: {0, 2, 255})
						{
							(backface ? reuse_sm.stencil_opval_bf : reuse_sm.stencil_opval) = value;
							if (accepts_reuse()) return false;
						}
					}
				}
			}
		}
		reuse_sc = valid_reuse_sc;
		reuse_sm = valid_reuse_sm;
		const auto valid_rc_raw = rc.raw;
		// All other raw modes (including fields not decoded by the renderer)
		// must remain outside this narrow import profile.
		for (unsigned bit = 0; bit < 32; ++bit)
		{
			rc.raw = valid_rc_raw ^ (uint32_t {1} << bit);
			if (accepts()) return false;
		}
		rc.raw = valid_rc_raw;
		// ReplaceTest uses testval, so opval is intentionally independent.
		sm.stencil_opval = 255;
		z.htile_surface.raw = 8; z.htile_surface.preload = 1;
		if (!accepts() || !accepts_layout() || !accepts_reuse() ||
		    DepthSurfaceUniformZeroProfile(z, neo, layout, rc, dc, sc, sm, 0x80000000u, 0) ||
		    DepthSurfaceUniformZeroProfile(z, neo, layout, rc, dc, sc, sm, 0, 1)) return false;
		const auto valid_z = z;
		for (unsigned field = 0; field < 35; ++field)
		{
			z = valid_z;
			switch (field)
			{
				case 0: z.z_info.num_samples = 1; break;
				case 1: z.z_info.tile_mode_index ^= 1; break;
				case 2: z.stencil_info.tile_split ^= 1; break;
				case 3: z.stencil_info.tile_mode_index ^= 1; break;
				case 4: z.depth_info.bank_height ^= 1; break;
				case 5: z.depth_info.macro_tile_aspect ^= 1; break;
				case 6: z.depth_info.num_banks ^= 1; break;
				case 7: z.depth_info.pipe_config ^= 1; break;
				case 8: z.depth_info.array_mode ^= 1; break;
				case 9: z.z_info.expclear_enabled = true; break;
				case 10: z.stencil_info.texture_compatible_stencil = true; break;
				case 11: z.stencil_info.tile_stencil_disable = false; break;
				case 12: z.htile_surface.raw |= 0x10; break;
				case 13: z.htile_surface.preload = 0; break;
				case 14: z.depth_view.slice_max = 1; break;
				case 15: z.depth_view.current_mip_level = 1; break;
				case 16: z.z_info.partially_resident = true; break;
				case 17: z.z_write_base_addr += 256; break;
				case 18: z.stencil_write_base_addr += 256; break;
				case 19: z.z_info.format = 1; break;
				case 20: z.z_info.zrange_precision = 0; break;
				case 21: z.z_info.tile_surface_enable = false; break;
				case 22: z.z_info.embedded_sample_locations = true; break;
				case 23: z.z_info.num_mip_levels = 1; break;
				case 24: z.z_info.plane_compression = 1; break;
				case 25: z.stencil_info.format = 0; break;
				case 26: z.stencil_info.expclear_enabled = true; break;
				case 27: z.stencil_info.partially_resident = true; break;
				case 28: z.depth_info.addr5_swizzle_mask = 2; break;
				case 29: z.depth_info.bank_width = 1; break;
				case 30: z.htile_surface.full_cache = 1; break;
				case 31: z.htile_surface.prefetch_height = 1; break;
				case 32: z.depth_view.slice_start = 1; break;
				case 33: z.depth_view.depth_write_disable = true; break;
				case 34: z.depth_view.stencil_write_disable = true; break;
			}
			if (accepts() || accepts_layout() || accepts_reuse()) return false;
		}
		z = valid_z;
		const auto valid_layout = layout;
		for (unsigned plane = 0; plane < 3; ++plane)
		{
			for (unsigned field = 0; field < 3; ++field)
			{
				layout = valid_layout;
				auto& range = plane == 0 ? layout.depth : plane == 1 ? layout.stencil : layout.htile;
				if (field == 0) range.address = 0;
				if (field == 1) range.allocation.size = 0;
				if (field == 2) range.swizzle = 1;
				if (accepts() || accepts_layout() || accepts_reuse()) return false;
			}
		}
		layout = valid_layout;
		dc.z_write_enable = true; if (accepts()) return false; dc.z_write_enable = false;
		rc.resummarize_enable = true; if (accepts()) return false; rc.resummarize_enable = false;
		dc.backface_enable = true; if (accepts()) return false; dc.backface_enable = false;
		sc.stencil_zpass = 4; if (accepts()) return false; sc.stencil_zpass = 3;
		sm.stencil_testval = 1; if (accepts()) return false; sm.stencil_testval = 0;
		sm.stencil_writemask = 127; if (accepts()) return false; sm.stencil_writemask = 255;
		if (!accepts() || !accepts_layout() || !accepts_reuse()) return false;
	}
	return true;
}

static bool CheckHtileCpuObservation()
{
	using namespace Lumen::Libs::Graphics;
	for (const char* value: std::array<const char*, 7>{nullptr, "", "0", "01", "true", "1x", "1 "})
	{
		if (HtileCpuObservationSelectorEnabled(value)) return false;
	}
	if (!HtileCpuObservationSelectorEnabled("1")) return false;

	const uint8_t bytes[] = {0, 1, 2, 3, 4, 5, 6, 7};
	HtileCpuSummary summary {};
	if (!SummarizeHtileCpuBytes(bytes, sizeof(bytes), &summary) || summary.byte_count != 8 || summary.word_count != 2 ||
	    summary.bin_count != 2 || summary.other_words != 0 || summary.IsUniform() ||
	    summary.byte_uniform || summary.first_byte != 0 ||
	    summary.bins[0].value != 0x03020100u || summary.bins[0].count != 1 ||
	    summary.bins[1].value != 0x07060504u || summary.bins[1].count != 1 ||
	    summary.fnv1a64 != 0xa4dc49e2b28ecb7dull) return false;
	const uint8_t repeated_word[] = {1, 2, 3, 4, 1, 2, 3, 4};
	if (!SummarizeHtileCpuBytes(repeated_word, sizeof(repeated_word), &summary) || !summary.IsUniform() ||
	    summary.byte_uniform || summary.first_byte != 1) return false;
	for (const uint8_t value: {0x00u, 0x7au, 0xffu})
	{
		std::array<uint8_t, 12> constant;
		constant.fill(value);
		if (!SummarizeHtileCpuBytes(constant.data(), constant.size(), &summary) || !summary.IsUniform() ||
		    !summary.byte_uniform || summary.first_byte != value || summary.bins[0].value != value * 0x01010101u) return false;
		constant.back() ^= 1u;
		if (!SummarizeHtileCpuBytes(constant.data(), constant.size(), &summary) || summary.byte_uniform) return false;
	}

	std::vector<uint8_t> words;
	for (uint32_t word = 0; word < 20; ++word)
	{
		words.insert(words.end(), {static_cast<uint8_t>(word), 0, 0, 0});
	}
	words.insert(words.end(), {0, 0, 0, 0, 19, 0, 0, 0});
	if (!SummarizeHtileCpuBytes(words.data(), words.size(), &summary) || summary.word_count != 22 ||
	    summary.bin_count != 16 || summary.other_words != 5 || summary.bins[0].count != 2 || summary.IsUniform()) return false;
	uint32_t accounted = summary.other_words;
	for (uint32_t bin = 0; bin < summary.bin_count; ++bin) accounted += summary.bins[bin].count;
	if (accounted != summary.word_count) return false;

	std::vector<uint8_t> maximum(HTILE_CPU_OBSERVATION_MAX_BYTES, 0xffu);
	if (!SummarizeHtileCpuBytes(maximum.data(), maximum.size(), &summary) || !summary.IsUniform() ||
	    !summary.byte_uniform || summary.first_byte != 0xffu ||
	    summary.bins[0].value != UINT32_MAX || summary.bins[0].count != maximum.size() / 4u ||
	    summary.other_words != 0) return false;
	maximum.resize(CPU_OBSERVATION_HARD_MAX_BYTES, 0xffu);
	if (SummarizeHtileCpuBytes(maximum.data(), maximum.size(), &summary) ||
	    !SummarizeHtileCpuBytes(maximum.data(), maximum.size(), &summary, CPU_OBSERVATION_HARD_MAX_BYTES) ||
	    summary.byte_count != CPU_OBSERVATION_HARD_MAX_BYTES || !summary.byte_uniform || summary.first_byte != 0xffu ||
	    summary.bins[0].count != CPU_OBSERVATION_HARD_MAX_BYTES / 4u) return false;
	const auto before = summary;
	for (const uint64_t size: std::array<uint64_t, 6>{0, 1, 3, 5, HTILE_CPU_OBSERVATION_MAX_BYTES + 4u, UINT64_MAX})
	{
		if (SummarizeHtileCpuBytes(bytes, size, &summary) || summary.byte_count != before.byte_count ||
		    summary.word_count != before.word_count || summary.bin_count != before.bin_count ||
		    summary.other_words != before.other_words || summary.fnv1a64 != before.fnv1a64 ||
		    summary.first_byte != before.first_byte || summary.byte_uniform != before.byte_uniform ||
		    summary.bins[0].value != before.bins[0].value || summary.bins[0].count != before.bins[0].count) return false;
	}
	if (SummarizeHtileCpuBytes(nullptr, 4, &summary) || SummarizeHtileCpuBytes(bytes, 4, nullptr)) return false;
	for (const uint64_t cap: std::array<uint64_t, 4>{0, 3, CPU_OBSERVATION_HARD_MAX_BYTES + 1u, UINT64_MAX})
	{
		if (SummarizeHtileCpuBytes(bytes, sizeof(bytes), &summary, cap) || summary.byte_count != before.byte_count ||
		    summary.fnv1a64 != before.fnv1a64 || summary.byte_uniform != before.byte_uniform) return false;
	}
	if (SummarizeHtileCpuBytes(bytes, CPU_OBSERVATION_HARD_MAX_BYTES + 4u, &summary, CPU_OBSERVATION_HARD_MAX_BYTES)) return false;

	HtileStorageWritebackHistory history;
	HtileStorageWritebackSnapshot found {};
	if (history.Record(0, 8, 1, bytes) || history.Record(UINT64_MAX - 3u, 8, 1, bytes) ||
	    history.Record(0x10000, 8, 0, bytes) || history.Record(0x10000, 8, 1, nullptr) ||
	    history.Record(0x10000, HTILE_CPU_OBSERVATION_MAX_BYTES + 4u, 1, bytes)) return false;
	for (uint32_t index = 0; index < HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS; ++index)
	{
		if (!history.Record(0x10000, 8, index + 1u, bytes)) return false;
	}
	if (history.Record(0x10000, 8, 9, bytes) || !history.Find(0x10000, 8, &found) ||
	    found.sequence != HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS || found.source_memory_id != HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS ||
	    found.summary.fnv1a64 != 0xa4dc49e2b28ecb7dull) return false;
	if (history.Find(0x10004, 4, &found) || history.Find(0x10000, 4, &found) || history.Find(0x10000, 8, nullptr) ||
	    found.sequence != HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS) return false;

	// Concurrent callback writers share one bounded history. No slot can be
	// published twice, exceed the cap, or expose another range's summary.
	HtileStorageWritebackHistory concurrent;
	std::array<std::thread, 16> writers;
	std::array<bool, 16> recorded {};
	for (size_t index = 0; index < writers.size(); ++index)
	{
		writers[index] = std::thread([&, index] {
			const uint8_t source[] = {static_cast<uint8_t>(index), 0, 0, 0};
			recorded[index] = concurrent.Record(0x20000u + index * 4096u, 4, index + 1u, source);
		});
	}
	for (auto& writer: writers) writer.join();
	uint32_t count = 0;
	std::array<bool, HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS + 1> sequences {};
	for (size_t index = 0; index < writers.size(); ++index)
	{
		if (concurrent.Find(0x20000u + index * 4096u, 4, &found) != recorded[index]) return false;
		if (!recorded[index]) continue;
		if (found.sequence == 0 || found.sequence > HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS || sequences[found.sequence] ||
		    found.source_memory_id != index + 1u || !found.summary.IsUniform() || found.summary.bins[0].value != index) return false;
		sequences[found.sequence] = true;
		++count;
	}
	return count == HTILE_CPU_OBSERVATION_MAX_SNAPSHOTS;
}

int main()
{
	if (!CheckUniformDepthZeroBytes() || !CheckUniformDepthZeroProfile() || !CheckHtileCpuObservation())
	{
		std::fprintf(stderr, "HTile CPU diagnostic summary/history checks failed\n");
		return 1;
	}
	if (!CheckDepthSurfaceLayout() || !CheckBaseDepthMacroLayout()) return 1;
	if (!CheckDisplay2dThinFallback())
	{
		return 1;
	}
	if (!CheckDisplay2dThinAddressFixtures() || !CheckDisplay2dThinRejectedConversions() ||
	    !CheckDisplay2dThinRandomizedConversions()) return 1;
	if (!CheckColor2dThinLayout() || !CheckColor2dThinAddressFixtures() || !CheckColor2dThinRejectedConversions()) return 1;
	constexpr uint32_t display_shapes[][3] = {
	    {1, 1, 128}, {64, 64, 128}, {228, 128, 256}, {480, 270, 512}, {960, 540, 1024},
	    {127, 63, 128}, {128, 64, 128}, {128, 65, 128}, {129, 127, 256}, {256, 128, 256}, {383, 129, 384},
	};
	for (const bool neo: {false, true})
	{
		for (const auto& shape: display_shapes)
		{
			if (!CheckColor2dThinConversion(shape[0], shape[1], shape[2], neo))
			{
				std::fprintf(stderr, "Color_2dThin model conversion/bijection/padding failed: %ux%u pitch=%u neo=%u\n",
				             shape[0], shape[1], shape[2], neo ? 1u : 0u);
				return 1;
			}
			if (!CheckDisplay2dThinConversion(shape[0], shape[1], shape[2], neo))
			{
				std::fprintf(stderr, "Display_2dThin conversion/bijection/padding failed: %ux%u pitch=%u neo=%u\n",
				             shape[0], shape[1], shape[2], neo ? 1u : 0u);
				return 1;
			}
		}
	}
	if (!CheckDisplayLinearAlignedMipLayout())
	{
		return 1;
	}
	if (!CheckThin1dPow2PaddedMipLayout())
	{
		return 1;
	}
	if (!CheckThin1dRoundTrip("R8 512x512", 1, 0, 512, 512, 512, 0x40000u) ||
	    !CheckThin1dRoundTrip("R8G8 64x64", 3, 0, 64, 64, 64, 0x2000u) ||
	    !CheckThin1dRoundTrip("RGBA8 48x48", 10, 0, 48, 48, 48, 0x2400u) ||
	    !CheckThin1dRoundTrip("BC1 32x32", 35, 0, 32, 32, 32, 0x200u) ||
	    !CheckThin1dRoundTrip("BC2 32x32", 36, 0, 32, 32, 32, 0x400u) || !CheckThin1dCrossFormatSubrange())
	{
		return 1;
	}
	const TextureSizeCase tests[] = {
	    {"RGBA8 tile-13 non-power-of-two", 10, 0, 1024, 1020, 1024, 13, 0x400000, 1024, 1024},
	    {"BC2 tile-13 control atlas", 36, 0, 2048, 2044, 2048, 13, 0x400000, 512, 512},
	    {"BC2 tile-13 non-power-of-two", 36, 0, 359, 236, 384, 13, 0x18000, 96, 64},
	    {"RGBA8 linear non-power-of-two", 10, 0, 1024, 1020, 1024, 8, 0x3fc000, 1024, 1020},
	    {"RGBA8 tile-13 micro-tile padding", 10, 0, 17, 9, 17, 13, 0x600, 24, 16},
	    {"R8 tile-13 base alignment", 1, 0, 1, 1, 1, 13, 0x100, 32, 8},
	    {"R8G8 tile-13 base alignment", 3, 0, 1, 1, 1, 13, 0x100, 16, 8},
	    {"R8G8 linear Restart fixture", 3, 0, 1024, 4096, 1024, 31, 0x800000, 1024, 4096},
	    {"BC1 tile-13 block padding", 35, 0, 17, 9, 17, 13, 0x200, 8, 8},
	    {"BC3 sRGB tile-13 block padding", 37, 9, 33, 17, 33, 13, 0x800, 16, 8},
	    {"BC3 tile-13 known layout", 37, 0, 1920, 1080, 1920, 13, 0x1fe000, 480, 272},
	    {"BC7 tile-13 atlas", 41, 0, 900, 736, 928, 13, 0xa6c00, 232, 184},
	    {"BC7 tile-13 padded blocks", 41, 0, 48, 20, 64, 13, 0x800, 16, 8},
	};

	for (const auto& test: tests)
	{
		if (!CheckTextureSize(test))
		{
			return 1;
		}
	}
	if (!CheckBc7Thin1dShapeContract() || !CheckThin1dRoundTrip("BC7 32x32", 41, 0, 32, 32, 32, 0x400u)) return 1;
	for (bool neo: {false, true})
		for (const auto shape: {std::array<uint32_t, 3> {4, 4, 4}, {48, 20, 64}, {900, 736, 928}})
			if (!CheckBc7OpaqueBlockConversion(shape[0], shape[1], shape[2], neo)) return 1;
	std::printf("BC7-Thin1D-profile=ok opaque-block-Morton=ok active-blocks/padding/guards=ok neo/base=ok\n");
	if (!CheckDisplay2dThinInitializedPool()) return 1;

	std::printf("HTile-CPU-observation=ok Depth-surface-layout=ok Tile-Display2dThin=ok Tile-Display2dThin-conversion=ok Tile-Display2dThin-random=ok Tile-Color2dThin-model=ok Tile-Color2dThin-vectors/bijection/padding/reject=ok Tile-size-padding=ok Tile-size-BC=ok Tile-size-linear=ok Tile-size-linear-mips=ok "
	            "Tile-Thin1D-round-trip=ok Tile-cross-format-subrange=ok\n");
	return 0;
}
