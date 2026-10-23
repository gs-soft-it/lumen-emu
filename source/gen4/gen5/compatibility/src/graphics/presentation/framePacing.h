// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// A stalled presentation cannot be repaid with synthetic vblank bursts. Keep
// the fractional scheduling correction, but discard debt beyond this cycle.
constexpr int64_t NextVblankWait(int64_t carried_wait, uint64_t period,
                                uint64_t frame_ticks) noexcept {
    if (frame_ticks >= period) return 0;
    const auto remaining = static_cast<int64_t>(period - frame_ticks);
    return std::clamp(carried_wait, -remaining, static_cast<int64_t>(frame_ticks)) + remaining;
}

} // namespace Libs::Graphics
