// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {
enum class SwapchainStatus : uint8_t { Success, Recreate, SurfaceLost, Fatal };
enum class PresentationCycle : uint8_t {
    Presented, AcquisitionExhausted, SubmittedNotPresented, Fatal
};

// SUBOPTIMAL has successfully acquired an image or queued its presentation.
// It is advisory; an unchanged surface may keep returning it indefinitely.
constexpr SwapchainStatus AcquireStatus(VkResult result) noexcept {
    switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR: return SwapchainStatus::Success;
        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_ERROR_UNKNOWN: return SwapchainStatus::Recreate;
        case VK_ERROR_SURFACE_LOST_KHR: return SwapchainStatus::SurfaceLost;
        default: return SwapchainStatus::Fatal;
    }
}

constexpr SwapchainStatus PresentStatus(VkResult result) noexcept {
    switch (result) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR: return SwapchainStatus::Success;
        case VK_ERROR_OUT_OF_DATE_KHR: return SwapchainStatus::Recreate;
        case VK_ERROR_SURFACE_LOST_KHR: return SwapchainStatus::SurfaceLost;
        default: return SwapchainStatus::Fatal;
    }
}

// Retry only acquisition, before any command effects. Once submitted, even a
// rejected present queues its semaphore wait, so recover without replaying it.
template <typename Acquire, typename SubmitAndPresent, typename Recover>
PresentationCycle RunPresentationCycle(Acquire&& acquire, SubmitAndPresent&& submit_and_present,
                                       Recover&& recover) {
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        const auto acquired = acquire();
        if (acquired == SwapchainStatus::Fatal) return PresentationCycle::Fatal;
        if (acquired != SwapchainStatus::Success) {
            recover(acquired);
            continue;
        }
        const auto presented = submit_and_present();
        if (presented == SwapchainStatus::Fatal) return PresentationCycle::Fatal;
        if (presented == SwapchainStatus::Success) return PresentationCycle::Presented;
        recover(presented);
        return PresentationCycle::SubmittedNotPresented;
    }
    return PresentationCycle::AcquisitionExhausted;
}
} // namespace Libs::Graphics
