#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSURFACEIMPORT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_DEPTHSURFACEIMPORT_H_

#include <cstdlib>
#include <cstring>

#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Graphics/DepthSurfaceLayout.h"
#include "Emulator/Graphics/HardwareContext.h"
#include "Emulator/Graphics/Objects/GpuMemory.h"
#include "Emulator/Graphics/Pm4.h"

namespace Lumen::Libs::Graphics {

inline bool DepthSurfaceUniformZeroImportEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_UNIFORM_DEPTH_ZERO_IMPORT");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

inline bool DepthSurfaceUniformZeroImportParams(const GpuObject& object)
{
	return object.type == GpuMemoryObjectType::DepthStencilBuffer && object.params[6] == 1 && object.params[7] == 0;
}

// A single sample and slice, with the in-tree base/Neo tiling profile. The
// allocation resolver is a separate prerequisite; uniform values avoid a
// permutation decoder, not the requirement for a known complete footprint.
inline bool DepthSurfaceUniformZeroLayoutProfile(const HW::DepthRenderTarget& z, bool neo,
	                                             const DepthSurfaceLayout& layout)
{
	const auto& d = z.z_info;
	const auto& s = z.stencil_info;
	const auto& t = z.depth_info;
	const auto& h = z.htile_surface;
	const auto& v = z.depth_view;
	return d.format == 3 && d.tile_mode_index == (neo ? 2u : 0u) && d.num_samples == 0 &&
	       d.zrange_precision == 1 && d.tile_surface_enable && !d.expclear_enabled &&
	       !d.embedded_sample_locations && !d.partially_resident && d.num_mip_levels == 0 && d.plane_compression == 0 &&
	       s.format == 1 && s.tile_mode_index == (neo ? 2u : 0u) && s.tile_split == (neo ? 2u : 0u) &&
	       !s.expclear_enabled && s.tile_stencil_disable && !s.texture_compatible_stencil && !s.partially_resident &&
	       t.addr5_swizzle_mask <= 1 && t.array_mode == 4 && t.pipe_config == (neo ? 0x12u : 0xcu) &&
	       t.bank_width == 0 && t.bank_height == (neo ? 1u : 2u) && t.macro_tile_aspect == (neo ? 0u : 2u) &&
	       t.num_banks == (neo ? 2u : 3u) && (h.raw == 0 || h.raw == 8) && h.linear == 0 && h.full_cache == 0 &&
	       h.htile_uses_preload_win == 0 && h.preload == (h.raw >> 3u) && h.prefetch_width == 0 &&
	       h.prefetch_height == 0 && h.dst_outside_zero_to_one == 0 && v.slice_start == 0 && v.slice_max == 0 &&
	       v.current_mip_level == 0 && !v.depth_write_disable && !v.stencil_write_disable &&
	       d.unsupported_bits == 0 && s.unsupported_bits == 0 && t.unsupported_bits == 0 &&
	       z.depth_size_unsupported_bits == 0 && z.depth_slice_unsupported_bits == 0 &&
	       z.z_read_base_addr == z.z_write_base_addr && z.stencil_read_base_addr == z.stencil_write_base_addr &&
	       layout.depth.address != 0 && layout.stencil.address != 0 && layout.htile.address != 0 &&
	       layout.depth.allocation.size != 0 && layout.stencil.allocation.size != 0 && layout.htile.allocation.size != 0 &&
	       layout.depth.swizzle == 0 && layout.stencil.swizzle == 0 && layout.htile.swizzle == 0;
}

// Check the raw shader interface on every bind, including pipeline-cache hits
// and embedded shaders. A host image containing zero cannot prove equivalence
// when a requested guest depth/stencil export was never translated.
inline bool DepthSurfaceUniformZeroShaderProfile(const HW::ShaderRegisters& shader)
{
	const auto& control = shader.db_shader_control;
	return shader.shader_z_format == 0 && !control.shader_z_export_enable &&
	       control.conservative_z_export_value == 0 && control.other_bits == 0 &&
	       (control.shader_z_behavior == 0 || control.shader_z_behavior == 1);
}

inline bool DepthSurfaceUniformZeroProfile(const HW::DepthRenderTarget& z, bool neo,
	                                       const DepthSurfaceLayout& layout, const HW::RenderControl& rc,
	                                       const HW::DepthControl& dc, const HW::StencilControl& sc,
	                                       const HW::StencilMask& sm, uint32_t depth_clear_raw, uint8_t stencil_clear)
{
	return DepthSurfaceUniformZeroLayoutProfile(z, neo, layout) && depth_clear_raw == 0 && stencil_clear == 0 &&
	       rc.raw == (Pm4::DB_RENDER_CONTROL_STENCIL_CLEAR_ENABLE_MASK << Pm4::DB_RENDER_CONTROL_STENCIL_CLEAR_ENABLE_SHIFT) &&
	       !rc.depth_clear_enable && rc.stencil_clear_enable &&
	       !rc.resummarize_enable && !rc.stencil_compress_disable && !rc.depth_compress_disable &&
	       !rc.copy_centroid && rc.copy_sample == 0 && !dc.z_enable && !dc.z_write_enable && !dc.depth_bounds_enable &&
	       dc.stencil_enable && !dc.backface_enable && dc.stencilfunc == 7 &&
	       !dc.color_writes_on_depth_fail_enable && !dc.color_writes_on_depth_pass_disable &&
	       sc.stencil_fail == 3 && sc.stencil_zpass == 3 && sc.stencil_zfail == 3 &&
	       sm.stencil_testval == 0 && sm.stencil_mask == 0xff && sm.stencil_writemask == 0xff;
}

// Permission to bind an already imported, exact attachment, not a proof of
// uniform content. Every later pass that can write must be completed and read
// back in full before guest publication can preserve the zero representation.
inline bool DepthSurfaceUniformZeroReuseProfile(const HW::DepthRenderTarget& z, bool neo,
	                                            const DepthSurfaceLayout& layout, const HW::RenderControl& rc,
	                                            const HW::DepthControl& dc, const HW::StencilControl& sc,
	                                            const HW::StencilMask& sm, uint32_t depth_clear_raw, uint8_t stencil_clear)
{
	// Retain the metadata interpretation of the original zero import even when
	// clearing is disabled: no general HTile decoder proves another clear value.
	if (!DepthSurfaceUniformZeroLayoutProfile(z, neo, layout) || depth_clear_raw != 0 || stencil_clear != 0 || rc.raw != 0 ||
	    rc.depth_clear_enable || rc.stencil_clear_enable || rc.resummarize_enable ||
	    rc.stencil_compress_disable || rc.depth_compress_disable || rc.copy_centroid || rc.copy_sample != 0 ||
	    dc.depth_bounds_enable || dc.color_writes_on_depth_fail_enable || dc.color_writes_on_depth_pass_disable) return false;
	// AMD defines ADDR5 as a choice of 32-byte half within a 64-byte word.
	// Mask zero can alias those halves; admit it only while every sample stays
	// zero, with the original full-footprint and native/guest content proofs.
	// This is checked at every attachment request before any pipeline cache hit.
	if (z.depth_info.addr5_swizzle_mask == 0)
	{
		if (dc.z_write_enable) return false;
		if (!dc.stencil_enable) return true;
		if (dc.stencilfunc != 7) return false;
		const auto preserves_zero = [](uint8_t op, uint8_t testval, uint8_t opval) {
			return op == 0 || op == 1 || (op == 3 && testval == 0) || (op == 4 && opval == 0);
		};
		if (!preserves_zero(sc.stencil_fail, sm.stencil_testval, sm.stencil_opval) ||
		    !preserves_zero(sc.stencil_zpass, sm.stencil_testval, sm.stencil_opval) ||
		    !preserves_zero(sc.stencil_zfail, sm.stencil_testval, sm.stencil_opval)) return false;
		// Keep the existing renderer's exact front/back representation. Equal
		// operations alone cannot conceal different replacement operands/masks.
		return !dc.backface_enable ||
		       (dc.stencilfunc_bf == dc.stencilfunc && sc.stencil_fail_bf == sc.stencil_fail &&
		        sc.stencil_zpass_bf == sc.stencil_zpass && sc.stencil_zfail_bf == sc.stencil_zfail &&
		        sm.stencil_testval_bf == sm.stencil_testval && sm.stencil_mask_bf == sm.stencil_mask &&
		        sm.stencil_writemask_bf == sm.stencil_writemask && sm.stencil_opval_bf == sm.stencil_opval);
	}
	if (!dc.stencil_enable) return true;
	// The legacy front decoder derives its host reference from write operations.
	// Only Always makes that reference independent of guest comparison semantics.
	// The renderer separately validates front/back equivalence and shared refs.
	if (dc.stencilfunc != 7 || (dc.backface_enable && dc.stencilfunc_bf != 7)) return false;
	const auto operations_supported = [](uint8_t fail, uint8_t pass, uint8_t depth_fail, uint8_t value) {
		const uint8_t operations[] {fail, pass, depth_fail};
		for (const auto op: operations)
			if (op > 9 || ((op == 5 || op == 6 || op == 8 || op == 9) && value != 1)) return false;
		return true;
	};
	return operations_supported(sc.stencil_fail, sc.stencil_zpass, sc.stencil_zfail, sm.stencil_opval) &&
	       (!dc.backface_enable || operations_supported(sc.stencil_fail_bf, sc.stencil_zpass_bf,
	                                                 sc.stencil_zfail_bf, sm.stencil_opval_bf));
}

inline bool DepthSurfaceStencilFaceReadOnly(uint8_t fail, uint8_t pass, uint8_t depth_fail, uint8_t write_mask)
{
	return write_mask == 0 || (fail == 0 && pass == 0 && depth_fail == 0);
}

// Literal attachment reads may retain an already sealed zero proof. This
// predicate never creates that proof and deliberately excludes stencil writes,
// including operations that would happen to preserve zero. The actual bound
// native pipeline and its immutable pixel-shader export proof are checked by
// the renderer separately on every draw, including pipeline-cache hits.
inline bool DepthSurfaceUniformZeroReadOnlyProfile(const HW::DepthRenderTarget& z, bool neo,
    const DepthSurfaceLayout& layout, const HW::RenderControl& rc, const HW::DepthControl& dc,
    const HW::StencilControl& sc, const HW::StencilMask& sm, const HW::ShaderRegisters& shader,
    uint32_t depth_clear_raw, uint8_t stencil_clear)
{
	if (!DepthSurfaceUniformZeroShaderProfile(shader) ||
	    !DepthSurfaceUniformZeroReuseProfile(z, neo, layout, rc, dc, sc, sm, depth_clear_raw, stencil_clear) ||
	    dc.z_write_enable || dc.unsupported_bits != 0 ||
	    dc.zfunc > 7 || dc.stencilfunc > 7 || dc.stencilfunc_bf > 7) return false;
	if (dc.stencil_enable &&
	    (!DepthSurfaceStencilFaceReadOnly(sc.stencil_fail, sc.stencil_zpass, sc.stencil_zfail, sm.stencil_writemask) ||
	     (dc.backface_enable && !DepthSurfaceStencilFaceReadOnly(sc.stencil_fail_bf, sc.stencil_zpass_bf,
	         sc.stencil_zfail_bf, sm.stencil_writemask_bf)))) return false;
	const uint32_t read_only_control =
	    (static_cast<uint32_t>(dc.stencil_enable) << Pm4::DB_DEPTH_CONTROL_STENCIL_ENABLE_SHIFT) |
	    (static_cast<uint32_t>(dc.z_enable) << Pm4::DB_DEPTH_CONTROL_Z_ENABLE_SHIFT) |
	    (static_cast<uint32_t>(dc.zfunc) << Pm4::DB_DEPTH_CONTROL_ZFUNC_SHIFT) |
	    (static_cast<uint32_t>(dc.backface_enable) << Pm4::DB_DEPTH_CONTROL_BACKFACE_ENABLE_SHIFT) |
	    (static_cast<uint32_t>(dc.stencilfunc) << Pm4::DB_DEPTH_CONTROL_STENCILFUNC_SHIFT) |
	    (static_cast<uint32_t>(dc.stencilfunc_bf) << Pm4::DB_DEPTH_CONTROL_STENCILFUNC_BF_SHIFT);
	return dc.raw == read_only_control;
}

// Called only after the tracker has proved ownership and completed writeback.
// Reads every byte of the current S/H planes; no diagnostic history or hash is
// an authority. Unsupported mapping-pinning backends fail closed.
// The original generation must precede any GPU wait/readback being validated.
// Only full local-set acquisition may retry; content scans are outside retry.
struct DepthSurfaceUniformZeroLeaseSet
{
	Core::VirtualMemory::GuestDataLease planes[3];
};

// The optional caller-local set must enter empty. Success keeps the exact three
// leases used by this scan until that set leaves scope; failure releases every
// newly enrolled lease. An already-active incoming set is rejected intact.
// With no output set, all leases are released before returning. Diagnostics,
// waits and caller-lock release must occur only after the retained set ends.
bool DepthSurfaceUniformZeroBytes(const uint64_t* addresses, const uint64_t* sizes, int count,
                                 uint64_t expected_generation, const char** rejection = nullptr,
                                 int* rejected_plane = nullptr, DepthSurfaceUniformZeroLeaseSet* retained_set = nullptr);
// An exact managed publication pin extends the gate, never the original
// continuity/content authority. Every child proves its own range and rights.
bool DepthSurfaceUniformZeroBytesUnderParent(const uint64_t* addresses, const uint64_t* sizes, int count,
    uint64_t original_generation, const Core::VirtualMemory::GuestDataLease& parent,
    const char** rejection, DepthSurfaceUniformZeroLeaseSet* retained_set, int* rejected_plane = nullptr);
#ifdef LUMEN_GRAPHICS_STATE_TEST_HOOK
bool DepthSurfaceUniformZeroLeaseSetContractTest();
#endif

} // namespace Lumen::Libs::Graphics

#endif
