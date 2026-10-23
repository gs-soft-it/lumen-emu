#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

enum class TileMode
{
	VideoOutLinear,
	VideoOutTiled,
	TextureLinear,
	TextureTiled,
	// RenderTextureLinear,
	// RenderTextureTiled,
};

struct TileSizeAlign
{
	uint32_t size  = 0;
	uint32_t align = 0;
};

struct TileSizeOffset
{
	uint32_t size   = 0;
	uint32_t offset = 0;
};

struct TilePaddedSize
{
	uint32_t width  = 0;
	uint32_t height = 0;
};

void TileInit();
void TileConvertTiledToLinear(void* dst, const void* src, TileMode mode, uint32_t width, uint32_t height, bool neo);
void TileConvertTiledToLinear(void* dst, const void* src, TileMode mode, uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t levels, bool neo, uint32_t tile = 13, uint64_t buffer_size = 0,
                              bool pow2_pad = false);
// Converts the logical linear texels back to the guest texture layout. Source
// and destination must be disjoint and each contain buffer_size bytes. Tiled
// padding has no linear texel counterpart and is deliberately left unchanged
// in dst; callers that need a bit-exact raw snapshot must initialize it first.
void TileConvertLinearToTiled(void* dst, const void* src, TileMode mode, uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t levels, bool neo, uint32_t tile, uint64_t buffer_size,
                              bool pow2_pad = false);

// Single-mip tile-10 Display_2dThin raw 32-bit texels, using the profile of
// TileTryGetDisplay2dThinTextureSize. Both linear and tiled layouts use pitch
// texels per physical row. Only logical width*height texels are copied; all
// padding and destination tail bytes stay unchanged. Buffers must be disjoint.
// Unsupported layouts, insufficient capacities, overflow or overlap return
// false before any destination write. No channel or colorspace conversion.
bool TileTryConvertDisplay2dThinToLinear(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                       uint32_t width, uint32_t height, uint32_t pitch, bool neo);
bool TileTryConvertLinearToDisplay2dThin(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                       uint32_t width, uint32_t height, uint32_t pitch, bool neo);

// Dedicated one-mip tile-14 raw RGBA8 readback profile, using the existing
// Tiler2d macro/pipe/bank and non-display element model. It does not change
// TileGetTextureSize's sampled descriptor footprint. Physical GB_ADDR_CONFIG
// and the hardware pixel permutation are not qualified by this local model.
bool TileTryGetColor2dThinTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
                                     uint32_t levels, uint32_t tile, bool neo, bool pow2_pad, TileSizeAlign* total_size,
                                     TileSizeOffset* level_sizes, TilePaddedSize* padded_size);

// Both directions copy only logical raw 32-bit texels at the requested pitch.
// Tiled padding, linear row padding and destination tail bytes stay untouched.
// Invalid geometry, capacity, overflow or overlap returns false before writes.
bool TileTryConvertColor2dThinToLinear(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                     uint32_t width, uint32_t height, uint32_t pitch, bool neo);
bool TileTryConvertLinearToColor2dThin(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                     uint32_t width, uint32_t height, uint32_t pitch, bool neo);

bool TileGetDepthSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t z_format, uint32_t stencil_format, bool htile, bool neo,
                      TileSizeAlign* stencil_size, TileSizeAlign* htile_size, TileSizeAlign* depth_size);
void TileGetVideoOutSize(uint32_t width, uint32_t height, uint32_t pitch, bool tile, bool neo, TileSizeAlign* size);
void TileGetTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels, uint32_t tile,
                        bool neo, TileSizeAlign* total_size, TileSizeOffset* level_sizes, TilePaddedSize* padded_size,
                        bool pow2_pad = false);

// Resolves a POW2_PAD DisplayLinearAligned mip layout through an existing
// physical power-of-two fixture. Returns false outside the proven profile and
// never partially writes the outputs.
bool TileTryGetDisplayLinearAlignedTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
                                               uint32_t levels, uint32_t tile, bool neo, bool pow2_pad, TileSizeAlign* total_size,
                                               TileSizeOffset* level_sizes, TilePaddedSize* padded_size);

// Computes the single-level Display_2dThin RGBA8 slice layout used by tile
// mode 10 when a generated Addrlib fixture is not available. The accepted
// profile and macro-height alignment are shared by every known tile-10
// fixture. Returns false outside that profile and never partially writes the
// outputs.
bool TileTryGetDisplay2dThinTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
	                                    uint32_t levels, uint32_t tile, bool neo, bool pow2_pad, TileSizeAlign* total_size,
	                                    TileSizeOffset* level_sizes, TilePaddedSize* padded_size);

// Computes the documented Thin1D power-of-two-padded mip layout without
// consulting the generated lookup tables. Returns false for every shape that
// is outside the proven profile and never partially writes the outputs.
bool TileTryGetThin1dPow2PaddedTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
                                          uint32_t levels, uint32_t tile, bool pow2_pad, TileSizeAlign* total_size,
                                          TileSizeOffset* level_sizes, TilePaddedSize* padded_size);

} // namespace Lumen::Libs::Graphics

#endif

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TILE_H_ */
