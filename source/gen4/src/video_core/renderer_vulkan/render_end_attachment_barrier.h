// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include <optional>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

inline bool RenderEndAttachmentBarrierEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_RENDER_END_ATTACHMENT_BARRIER");
        return !(value && value[0] == '0' && value[1] == '\0');
    }();
    return enabled;
}

// Dynamic rendering has no implicit external dependency. An attachment can be
// reused with the same layout/access after a render-pass break, which does not
// necessarily produce an Image::Transit barrier. Publish attachment stores at
// the actual render end, without breaking an otherwise reusable pass per draw.
inline std::optional<vk::MemoryBarrier2> GetRenderEndAttachmentBarrier(bool has_color,
                                                                     bool has_depth_stencil) {
    if (!RenderEndAttachmentBarrierEnabled() || (!has_color && !has_depth_stencil)) {
        return {};
    }
    vk::MemoryBarrier2 barrier{
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    if (has_color) {
        barrier.srcStageMask |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        barrier.srcAccessMask |= vk::AccessFlagBits2::eColorAttachmentWrite;
    }
    if (has_depth_stencil) {
        barrier.srcStageMask |= vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                vk::PipelineStageFlagBits2::eLateFragmentTests;
        barrier.srcAccessMask |= vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    }
    return barrier;
}

} // namespace Vulkan
