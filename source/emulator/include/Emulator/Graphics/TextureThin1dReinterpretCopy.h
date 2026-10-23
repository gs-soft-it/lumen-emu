#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURETHIN1DREINTERPRETCOPY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURETHIN1DREINTERPRETCOPY_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"

#include <cstdint>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

class CommandBuffer;
struct GraphicContext;
struct VulkanImage;

struct TextureThin1dReinterpretCopyInfo
{
	VulkanImage* source      = nullptr;
	VulkanImage* destination = nullptr;
	uint32_t     source_pitch      = 0;
	uint32_t     destination_pitch = 0;
	uint64_t     source_size       = 0;
	uint64_t     destination_size  = 0;
	bool         destination_pow2_pad = false;
};

// Exact guest allocation contract for the only pitched/padded destination
// accepted by the reinterpret bridge. The host image remains width x height;
// bytes in the complete Thin1D micro-tiles outside that logical rectangle do
// not have a host-image representation and are deliberately not copied.
[[nodiscard]] constexpr bool TextureThin1dSingleLevelPaddedRgba8Size(
	uint32_t width, uint32_t height, uint32_t pitch, bool pow2_pad, uint64_t* size)
{
	if (size == nullptr || !pow2_pad || width == 0 || height == 0 || width > 0x80000000u ||
	    (width <= 1u && height <= 1u))
	{
		return false;
	}

	uint32_t canonical_pitch = width - 1u;
	canonical_pitch |= canonical_pitch >> 1u;
	canonical_pitch |= canonical_pitch >> 2u;
	canonical_pitch |= canonical_pitch >> 4u;
	canonical_pitch |= canonical_pitch >> 8u;
	canonical_pitch |= canonical_pitch >> 16u;
	canonical_pitch++;
	if (canonical_pitch < 8u)
	{
		canonical_pitch = 8u;
	}
	if (pitch != canonical_pitch || height > UINT32_MAX - 7u)
	{
		return false;
	}

	const uint64_t aligned_height = (static_cast<uint64_t>(height) + 7u) & ~uint64_t {7};
	const uint64_t physical_height = aligned_height < 8u ? 8u : aligned_height;
	if (pitch == 0 || physical_height > UINT32_MAX / pitch ||
	    static_cast<uint64_t>(pitch) * physical_height > UINT32_MAX / 4u)
	{
		return false;
	}
	const uint64_t allocation_size = static_cast<uint64_t>(pitch) * physical_height * 4u;
	if (allocation_size == 0)
	{
		return false;
	}
	*size = allocation_size;
	return true;
}

// Destination-only extension of the existing single-level RGBA8 Thin1D
// footprint: complete 8x8 micro-tiles, with a canonical aligned pitch when
// POW2_PAD is clear. The source profile remains the strict full-image shape.
[[nodiscard]] constexpr bool TextureThin1dSingleLevelRgba8DestinationSize(
	uint32_t width, uint32_t height, uint32_t pitch, bool pow2_pad, uint64_t* size)
{
	if (pow2_pad)
	{
		return TextureThin1dSingleLevelPaddedRgba8Size(width, height, pitch, true, size);
	}
	if (size == nullptr || width == 0 || height == 0 || width > UINT32_MAX - 7u || height > UINT32_MAX - 7u)
	{
		return false;
	}
	const uint64_t physical_width  = (static_cast<uint64_t>(width) + 7u) & ~uint64_t {7};
	const uint64_t physical_height = (static_cast<uint64_t>(height) + 7u) & ~uint64_t {7};
	if (pitch != physical_width || physical_height > UINT32_MAX / physical_width ||
	    physical_width * physical_height > UINT32_MAX / 4u)
	{
		return false;
	}
	*size = physical_width * physical_height * 4u;
	return true;
}

// Records a typed storage-image compute copy which projects the destination's
// logical texels from the matching raw Thin1D byte offsets for R8<->RGBA8 or
// RGBA8->RGBA8 with an independently pitched layout. This is the complete
// smaller prefix for strict shapes; a partial/padded destination intentionally has no
// host representation for bytes outside its logical rectangle. The helper never submits, waits or
// maps memory; all temporary Vulkan objects retire on the supplied command
// buffer's fence.
[[nodiscard]] bool TextureThin1dReinterpretCopyPreflight(GraphicContext* ctx, CommandBuffer* buffer,
	                                                      const TextureThin1dReinterpretCopyInfo& info);
[[nodiscard]] bool TextureThin1dReinterpretCopyRecord(GraphicContext* ctx, CommandBuffer* buffer,
	                                                   const TextureThin1dReinterpretCopyInfo& info);

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_TEXTURETHIN1DREINTERPRETCOPY_H_ */
