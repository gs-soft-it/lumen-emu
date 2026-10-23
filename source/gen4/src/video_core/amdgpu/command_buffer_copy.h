// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace AmdGpu {

// A submission owns its command bytes until its coroutine is destroyed.
struct CommandBufferCopy {
    CommandBufferCopy(std::span<const std::uint32_t> dcb_,
                      std::span<const std::uint32_t> ccb_) {
        if (!dcb_.empty()) {
            dcb.assign(dcb_.begin(), dcb_.end());
        }
        if (!ccb_.empty()) {
            ccb.assign(ccb_.begin(), ccb_.end());
        }
    }

    std::vector<std::uint32_t> dcb;
    std::vector<std::uint32_t> ccb;
};

} // namespace AmdGpu
