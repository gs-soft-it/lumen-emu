#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HARDWARECOLORMETADATA_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HARDWARECOLORMETADATA_H_

#include "Lumen/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Graphics/HardwareContext.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics::HW {

// Local classification of color-render-target metadata state (CMASK/FMASK/DCC).
//
// The renderer keeps every color surface uncompressed and fully materialized,
// so the profile it can accept is the single-sample CMASK fast-clear state:
// CMASK present with FAST_CLEAR enabled, no DCC, no FMASK compression and no
// multisample encoding. Anything else stays fail-closed because the renderer
// has no representation for it.
enum class ColorMetadataProfile : uint32_t
{
	None = 0,
	FastClearSingleSample,
	Unsupported,
};

struct ColorMetadataSlotVerdict
{
	ColorMetadataProfile profile = ColorMetadataProfile::None;
	const char*          cause   = "none";
};

inline ColorMetadataSlotVerdict ClassifyColorMetadataSlot(const RenderTarget& rt)
{
	ColorMetadataSlotVerdict verdict {};

	const bool cmask_active = rt.cmask.addr != 0 || rt.cmask_slice.slice_minus1 != 0;
	// Block geometry, transform and independent-block controls configure the
	// DCC compressor; they do not request compression on an uncompressed
	// target. Keep every actual DCC request fail-closed, including a metadata
	// base or clear key even when the compression-enable bit is absent.
	const bool dcc_requested = rt.dcc_addr.addr != 0 || rt.info.dcc_compression_enable || rt.dcc.dcc_clear_key_enable;
	const bool any = cmask_active || rt.fmask.addr != 0 || rt.fmask_slice.slice_minus1 != 0 || dcc_requested ||
	                 rt.info.fmask_compression_enable || rt.info.fmask_data_compression_disable || rt.info.fmask_one_frag_mode ||
	                 rt.info.cmask_fast_clear_enable ||
	                 rt.attrib.num_samples != 0 || rt.attrib.num_fragments != 0 || rt.clear_word0.word0 != 0 ||
	                 rt.clear_word1.word1 != 0 || rt.info.cmask_tile_mode != 0 || rt.info.cmask_tile_mode_neo != 0 ||
	                 rt.attrib3.cmask_pipe_aligned || rt.attrib3.dcc_pipe_aligned;
	if (!any) return verdict;

	const auto unsupported = [&](const char* cause)
	{
		verdict.profile = ColorMetadataProfile::Unsupported;
		verdict.cause   = cause;
		return verdict;
	};

	if (dcc_requested)
	{
		return unsupported("dcc");
	}
	if (rt.attrib.num_samples != 0 || rt.attrib.num_fragments != 0) return unsupported("multisample-encoding");
	if (rt.info.fmask_compression_enable || rt.info.fmask_data_compression_disable || rt.info.fmask_one_frag_mode)
	{
		return unsupported("fmask-compression");
	}
	if (rt.info.cmask_tile_mode != 0 || rt.info.cmask_tile_mode_neo != 0) return unsupported("cmask-linear-or-addr-type");
	if (rt.attrib3.cmask_pipe_aligned || rt.attrib3.dcc_pipe_aligned) return unsupported("pipe-aligned-metadata");
	if (cmask_active && !rt.info.cmask_fast_clear_enable) return unsupported("cmask-without-fast-clear");
	if (rt.fmask_slice.slice_minus1 != 0 && rt.fmask_slice.slice_minus1 != rt.slice.slice_div64_minus1)
	{
		return unsupported("fmask-slice-mismatch");
	}
	if (rt.fmask.addr != 0 && rt.info.cmask_fast_clear_enable && rt.cmask.addr == 0) return unsupported("fmask-only-fast-clear");

	verdict.profile = ColorMetadataProfile::FastClearSingleSample;
	return verdict;
}

struct ColorMetadataVerdict
{
	ColorMetadataProfile profile    = ColorMetadataProfile::None;
	uint32_t             first_slot = 0;
	const char*          cause      = "none";
};

inline ColorMetadataVerdict ClassifyColorMetadata(const Context& ctx)
{
	ColorMetadataVerdict verdict {};
	for (uint32_t slot = 0; slot < 8; ++slot)
	{
		const auto slot_verdict = ClassifyColorMetadataSlot(ctx.GetRenderTarget(slot));
		if (slot_verdict.profile == ColorMetadataProfile::None) continue;
		if (verdict.profile == ColorMetadataProfile::None)
		{
			verdict.profile    = slot_verdict.profile;
			verdict.first_slot = slot;
			verdict.cause      = slot_verdict.cause;
		}
		if (slot_verdict.profile == ColorMetadataProfile::Unsupported)
		{
			verdict.profile    = ColorMetadataProfile::Unsupported;
			verdict.first_slot = slot;
			verdict.cause      = slot_verdict.cause;
			return verdict;
		}
	}
	return verdict;
}

} // namespace Lumen::Libs::Graphics::HW

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HARDWARECOLORMETADATA_H_ */
