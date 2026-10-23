// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"

namespace VideoCore {

inline bool DetileSourceBarrierEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_DETILE_SOURCE_BARRIER");
        const bool result = !(value && value[0] == '0' && value[1] == '\0');
        LOG_INFO(Render_Vulkan, "[DETILE_SOURCE_BARRIER] enabled={} default=1", result ? 1 : 0);
        return result;
    }();
    return enabled;
}

// The next consumer of tiled bytes is the detiler's storage-buffer read.
// The following buffer-to-image copy reads a different, linear scratch buffer.
// Keep the source's tracked state at its actual consumer so a later writer also
// waits for the detiler; linear uploads retain their transfer-read dependency.
inline std::optional<vk::BufferMemoryBarrier2> GetImageUploadSourceBarrier(Buffer& source,
                                                                         bool tiled) {
    if (DetileSourceBarrierEnabled() && tiled) {
        return source.GetBarrier(vk::AccessFlagBits2::eShaderRead,
                                 vk::PipelineStageFlagBits2::eComputeShader);
    }
    return source.GetBarrier(vk::AccessFlagBits2::eTransferRead,
                             vk::PipelineStageFlagBits2::eTransfer);
}

} // namespace VideoCore
