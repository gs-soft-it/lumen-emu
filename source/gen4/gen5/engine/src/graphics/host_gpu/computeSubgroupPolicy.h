// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>

namespace Libs::Graphics::SubgroupPolicy {

constexpr uint32_t ComputeHostWidth(uint32_t native_width, bool supports_wave64) {
    if (supports_wave64) return 64u;
    return native_width == 16u || native_width == 32u ? native_width : 0u;
}

enum class RasterAdmission { Supported, StageUnavailable, WidthUnavailable };

constexpr RasterAdmission AdmitRaster(bool consumes_subgroup, uint32_t native_width,
                                      bool stage_supported) {
    // Preserve the established native32/64 raster paths. The new compute
    // virtualization does not provide missing raster invocations on native16.
    if (!consumes_subgroup || native_width >= 32u) return RasterAdmission::Supported;
    return stage_supported ? RasterAdmission::WidthUnavailable
                           : RasterAdmission::StageUnavailable;
}

} // namespace Libs::Graphics::SubgroupPolicy
