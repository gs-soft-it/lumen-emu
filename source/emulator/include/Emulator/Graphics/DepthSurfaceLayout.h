#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSURFACELAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSURFACELAYOUT_H_

#include "Emulator/Graphics/Tile.h"
#include "Emulator/Graphics/HardwareContext.h"

#include <cstdint>
#include <limits>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

struct DepthSurfaceLayoutRequest
{
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t pitch_div8_minus1 = 0;
	uint32_t slice_div64_minus1 = 0;
	uint32_t depth_format = 0;
	uint32_t stencil_format = 0;
	bool htile = false;
	bool neo = false;
	uint64_t depth_address = 0;
	uint64_t stencil_address = 0;
	uint64_t htile_address = 0;
};

struct DepthSurfaceRange
{
	TileSizeAlign allocation;
	uint64_t address = 0;
	uint64_t swizzle = 0;
};

struct DepthSurfaceLayout
{
	DepthSurfaceRange depth;
	DepthSurfaceRange stencil;
	DepthSurfaceRange htile;
};

[[nodiscard]] inline bool ResolveDepthSurfaceAllocationAddresses(const DepthSurfaceLayoutRequest& request,
	                                                            DepthSurfaceLayout* layout)
{
	const auto resolve_range = [](DepthSurfaceRange* range, uint64_t address, bool required)
	{
		const auto& allocation = range->allocation;
		if (!required) return allocation.size == 0 && allocation.align == 0;
		if (allocation.size == 0 || allocation.align == 0 || (allocation.align & (allocation.align - 1u)) != 0)
		{
			return false;
		}
		const uint64_t mask = static_cast<uint64_t>(allocation.align) - 1u;
		range->address = address & ~mask;
		range->swizzle = address & mask;
		return range->address != 0 && range->address <= std::numeric_limits<uint64_t>::max() - allocation.size;
	};
	return resolve_range(&layout->depth, request.depth_address, request.depth_format != 0) &&
	       resolve_range(&layout->stencil, request.stencil_address, request.stencil_format != 0) &&
	       resolve_range(&layout->htile, request.htile_address, request.htile);
}

// Resolves guest allocation ranges only; this does not import or decode depth,
// stencil, or HTile content. Failure leaves the caller's output untouched.
[[nodiscard]] inline bool ResolveDepthSurfaceLayout(const DepthSurfaceLayoutRequest& request, DepthSurfaceLayout* output)
{
	if (output == nullptr || request.width == 0 || request.height == 0 ||
	    (request.depth_format != 0 && request.depth_format != 1 && request.depth_format != 3) ||
	    request.stencil_format > 1 || (request.depth_format == 0 && request.stencil_format == 0))
	{
		return false;
	}

	const uint64_t pitch = (static_cast<uint64_t>(request.pitch_div8_minus1) + 1u) * 8u;
	const uint64_t slice_pixels = (static_cast<uint64_t>(request.slice_div64_minus1) + 1u) * 64u;
	const uint64_t depth_bytes = slice_pixels * (request.depth_format == 3 ? 4u : request.depth_format == 1 ? 2u : 0u);
	if (pitch > std::numeric_limits<uint32_t>::max() || request.width > pitch ||
	    static_cast<uint64_t>(request.height) * pitch > slice_pixels ||
	    depth_bytes > std::numeric_limits<uint32_t>::max() ||
	    (request.stencil_format != 0 && slice_pixels > std::numeric_limits<uint32_t>::max()))
	{
		return false;
	}

	DepthSurfaceLayout layout {};
	if (!TileGetDepthSize(request.width, request.height, static_cast<uint32_t>(pitch), request.depth_format,
	                      request.stencil_format, request.htile, request.neo, &layout.stencil.allocation,
	                      &layout.htile.allocation, &layout.depth.allocation))
	{
		// The existing slice-based fallback describes only a depth plane. It
		// cannot supply missing stencil or metadata allocations from a failed
		// table lookup, even when the depth byte count itself is representable.
		if (request.depth_format == 0 || request.stencil_format != 0 || request.htile) return false;
		layout.depth.allocation = {static_cast<uint32_t>(depth_bytes), request.neo ? 65536u : 32768u};
	}

	if (layout.depth.allocation.size != depth_bytes ||
	    layout.stencil.allocation.size != (request.stencil_format != 0 ? slice_pixels : 0u))
	{
		return false;
	}

	if (!ResolveDepthSurfaceAllocationAddresses(request, &layout))
	{
		return false;
	}
	*output = layout;
	return true;
}

// The hardware overload can resolve complete allocations outside the size
// fixtures, but only for the explicit base memory model already used by the
// local tilers: eight pipes, 256-byte pipe interleave, bank interleave one.
// This is allocation authority, never depth/stencil/HTile content authority.
[[nodiscard]] inline bool ResolveDepthSurfaceLayout(const HW::DepthRenderTarget& z, bool neo, DepthSurfaceLayout* output)
{
	const DepthSurfaceLayoutRequest request {z.width, z.height, z.pitch_div8_minus1, z.slice_div64_minus1,
	    z.z_info.format, z.stencil_info.format, z.z_info.tile_surface_enable, neo,
	    z.z_read_base_addr, z.stencil_read_base_addr, z.htile_data_base_addr};
	if (ResolveDepthSurfaceLayout(request, output)) return true;
	if (output == nullptr || neo || z.width == 0 || z.height == 0) return false;

	const auto& d = z.z_info;
	const auto& s = z.stencil_info;
	const auto& t = z.depth_info;
	const auto& h = z.htile_surface;
	const auto& v = z.depth_view;
	if (d.format != 3 || d.tile_mode_index != 0 || d.num_samples != 0 || d.zrange_precision != 1 ||
	    !d.tile_surface_enable || d.expclear_enabled || d.embedded_sample_locations || d.partially_resident ||
	    d.num_mip_levels != 0 || d.plane_compression != 0 || s.format != 1 || s.tile_mode_index != 0 ||
	    s.tile_split != 0 || s.expclear_enabled || !s.tile_stencil_disable || s.texture_compatible_stencil ||
	    s.partially_resident || t.addr5_swizzle_mask > 1 || t.array_mode != 4 || t.pipe_config != 0xc ||
	    t.bank_width != 0 || t.bank_height != 2 || t.macro_tile_aspect != 2 || t.num_banks != 3 ||
	    (h.raw != 0 && h.raw != 8) || h.linear != 0 || h.full_cache != 0 || h.htile_uses_preload_win != 0 ||
	    h.preload != (h.raw >> 3u) || h.prefetch_width != 0 || h.prefetch_height != 0 ||
	    h.dst_outside_zero_to_one != 0 || v.slice_start != 0 || v.slice_max != 0 || v.current_mip_level != 0 ||
	    v.depth_write_disable || v.stencil_write_disable || d.unsupported_bits != 0 || s.unsupported_bits != 0 ||
	    t.unsupported_bits != 0 || z.depth_size_unsupported_bits != 0 || z.depth_slice_unsupported_bits != 0 ||
	    z.z_read_base_addr != z.z_write_base_addr ||
	    z.stencil_read_base_addr != z.stencil_write_base_addr)
	{
		return false;
	}

	const uint64_t pitch = (static_cast<uint64_t>(z.pitch_div8_minus1) + 1u) * 8u;
	const uint64_t pixels = (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u;
	if (pitch > std::numeric_limits<uint32_t>::max() || pitch % 256u != 0 || pixels % pitch != 0 ||
	    z.width > pitch || pixels > std::numeric_limits<uint32_t>::max() / 4u)
	{
		return false;
	}
	const uint64_t height = pixels / pitch;
	const uint64_t declared_height = (static_cast<uint64_t>(z.height_div8_minus1) + 1u) * 8u;
	if (height % 128u != 0 || z.height > height || height != declared_height) return false;
	DepthSurfaceLayout fixture {};
	if (TileGetDepthSize(z.width, z.height, static_cast<uint32_t>(pitch), d.format, s.format, true, false,
	                     &fixture.stencil.allocation, &fixture.htile.allocation, &fixture.depth.allocation))
	{
		// A known fixture rejected above has a mismatched slice or invalid
		// address. The generic path must not replace that rejection.
		return false;
	}

	// The declared slice already consists of whole 256x128 macro regions.
	// Non-TC HTile covers whole 512x512 cache regions, with four metadata
	// bytes per 8x8 depth tile. Its padded extent is independent of the
	// visible viewport and of the depth plane's 64-byte tile split.
	const uint64_t metadata_pitch = ((pitch + 511u) / 512u) * 512u;
	const uint64_t metadata_height = ((height + 511u) / 512u) * 512u;
	if (metadata_pitch > std::numeric_limits<uint32_t>::max() ||
	    metadata_height > std::numeric_limits<uint32_t>::max() ||
	    metadata_height == 0 || metadata_pitch > std::numeric_limits<uint64_t>::max() / metadata_height)
	{
		return false;
	}
	const uint64_t metadata_bytes = metadata_pitch * metadata_height / 16u;
	if (metadata_bytes == 0 || metadata_bytes > std::numeric_limits<uint32_t>::max()) return false;
	DepthSurfaceLayout layout {};
	layout.depth.allocation = {static_cast<uint32_t>(pixels * 4u), 32768u};
	layout.stencil.allocation = {static_cast<uint32_t>(pixels), 32768u};
	layout.htile.allocation = {static_cast<uint32_t>(metadata_bytes), 2048u};
	if (!ResolveDepthSurfaceAllocationAddresses(request, &layout)) return false;
	*output = layout;
	return true;
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSURFACELAYOUT_H_
