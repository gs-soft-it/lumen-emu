#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTUREBC7CONTRACT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTUREBC7CONTRACT_H_

#include <cstdint>

namespace Lumen::Libs::Graphics {

struct Bc7Thin1dLayout
{
	uint32_t bytes = 0;
	uint32_t alignment = 0;
	uint32_t padded_block_width = 0;
	uint32_t padded_block_height = 0;
};

// BC7 blocks remain opaque: 4x4 texels and 16 bytes per block. This layout is
// the existing Thin1D 8x8 micro-tile rule applied to those blocks. It admits
// only one unpadded mip, with complete blocks and a validated custom pitch.
// No output is changed on rejection, and no unknown shape enters a fallback.
inline bool ResolveBc7Thin1dLayout(uint64_t dfmt, uint64_t nfmt, uint64_t width, uint64_t height, uint64_t pitch,
                                  uint64_t base_level, uint64_t levels, uint64_t tile, bool pow2_pad,
                                  Bc7Thin1dLayout* output = nullptr)
{
	if (dfmt != 41 || nfmt != 0 || tile != 13 || base_level != 0 || levels != 1 || pow2_pad ||
	    width == 0 || height == 0 || pitch < width || width > UINT32_MAX || height > UINT32_MAX || pitch > UINT32_MAX ||
	    width % 4u != 0 || height % 4u != 0 || pitch % 4u != 0) return false;
	const uint64_t padded_width = (pitch / 4u + 7u) & ~uint64_t {7};
	const uint64_t padded_height = (height / 4u + 7u) & ~uint64_t {7};
	if (padded_width == 0 || padded_height == 0 || padded_width > UINT32_MAX / 16u ||
	    padded_height > UINT32_MAX / (padded_width * 16u)) return false;
	if (output != nullptr)
		*output = {static_cast<uint32_t>(padded_width * padded_height * 16u), 256u,
		           static_cast<uint32_t>(padded_width), static_cast<uint32_t>(padded_height)};
	return true;
}

} // namespace Lumen::Libs::Graphics

#endif
