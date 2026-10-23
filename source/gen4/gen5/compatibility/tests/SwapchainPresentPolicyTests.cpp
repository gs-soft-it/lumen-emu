// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#include "graphics/presentation/window/swapchainPresentPolicy.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

namespace {
using namespace Libs::Graphics;

void Check(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "SwapchainPresentPolicyTests: FAIL %s\n", label);
        std::exit(1);
    }
}

struct Wsi {
    std::vector<VkResult> acquisitions;
    std::vector<VkResult> presentations;
    unsigned acquired = 0, submitted = 0, presented = 0, recovered = 0;
    bool acquire_wait_pending = false;
    bool render_wait_pending = false;
    std::vector<SwapchainStatus> recoveries;

    Wsi() = default;
    Wsi(std::initializer_list<VkResult> acquire, std::initializer_list<VkResult> present)
        : acquisitions(acquire), presentations(present) {}

    PresentationCycle Run() {
        return RunPresentationCycle([&] {
            Check(acquired < acquisitions.size(), "no unplanned reacquire/replay");
            Check(!acquire_wait_pending && !render_wait_pending,
                  "new acquisition cannot overwrite semaphore ownership");
            const auto status = AcquireStatus(acquisitions[acquired++]);
            acquire_wait_pending = status == SwapchainStatus::Success;
            return status;
        }, [&] {
            Check(acquire_wait_pending, "submit must consume a successful acquisition");
            ++submitted;
            acquire_wait_pending = false;
            render_wait_pending = true;
            Check(presented < presentations.size(), "no duplicate presentation effects");
            const auto result = presentations[presented++];
            // Queue-present consumes the submitted wait for success, suboptimal,
            // out-of-date and surface-lost. Fatal stops the process instead.
            if (PresentStatus(result) != SwapchainStatus::Fatal) render_wait_pending = false;
            return PresentStatus(result);
        }, [&](SwapchainStatus status) {
            Check(!acquire_wait_pending && !render_wait_pending,
                  "recovery cannot abandon an acquired semaphore or replay submitted work");
            ++recovered;
            recoveries.push_back(status);
        });
    }
};

void EffectsOnce(std::initializer_list<VkResult> acquire,
                 std::initializer_list<VkResult> present, PresentationCycle expected,
                 unsigned recoveries) {
    Wsi wsi {acquire, present};
    Check(wsi.Run() == expected, "expected cycle outcome");
    Check(wsi.submitted == 1 && wsi.presented == 1, "GPU/present effects occur once");
    Check(wsi.recovered == recoveries, "expected recovery count");
    Check(!wsi.acquire_wait_pending && !wsi.render_wait_pending,
          "all successful and recoverable paths consume owned semaphore waits");
}
} // namespace

int main() {
    EffectsOnce({VK_SUCCESS}, {VK_SUCCESS}, PresentationCycle::Presented, 0);
    EffectsOnce({VK_SUBOPTIMAL_KHR}, {VK_SUCCESS}, PresentationCycle::Presented, 0);
    EffectsOnce({VK_SUCCESS}, {VK_SUBOPTIMAL_KHR}, PresentationCycle::Presented, 0);
    EffectsOnce({VK_SUBOPTIMAL_KHR}, {VK_SUBOPTIMAL_KHR}, PresentationCycle::Presented, 0);
    EffectsOnce({VK_ERROR_OUT_OF_DATE_KHR, VK_SUCCESS}, {VK_SUCCESS},
                PresentationCycle::Presented, 1);
    EffectsOnce({VK_ERROR_SURFACE_LOST_KHR, VK_SUBOPTIMAL_KHR}, {VK_SUBOPTIMAL_KHR},
                PresentationCycle::Presented, 1);
    EffectsOnce({VK_SUCCESS}, {VK_ERROR_OUT_OF_DATE_KHR},
                PresentationCycle::SubmittedNotPresented, 1);
    EffectsOnce({VK_SUBOPTIMAL_KHR}, {VK_ERROR_SURFACE_LOST_KHR},
                PresentationCycle::SubmittedNotPresented, 1);

    Wsi persistent;
    persistent.acquisitions.assign(100, VK_SUBOPTIMAL_KHR);
    persistent.presentations.assign(100, VK_SUBOPTIMAL_KHR);
    for (unsigned frame = 0; frame < 100; ++frame)
        Check(persistent.Run() == PresentationCycle::Presented,
              "unchanged suboptimal surface keeps presenting successfully");
    Check(persistent.acquired == 100 && persistent.submitted == 100 &&
          persistent.presented == 100 && persistent.recovered == 0,
          "persistent advisory cannot force identical recreate or doubled effects");

    Wsi exhausted {{VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR}, {}};
    Check(exhausted.Run() == PresentationCycle::AcquisitionExhausted,
          "acquisition recovery is bounded");
    Check(exhausted.acquired == 2 && exhausted.submitted == 0 && exhausted.presented == 0 &&
          exhausted.recovered == 2, "failed acquisition performs no GPU effects");
    Check(exhausted.recoveries[1] == SwapchainStatus::SurfaceLost,
          "surface-lost retains distinct recovery authority");

    for (auto fatal : {VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY, VK_TIMEOUT}) {
        Wsi failed {{fatal}, {}};
        Check(failed.Run() == PresentationCycle::Fatal && failed.submitted == 0 &&
              failed.recovered == 0, "fatal acquisition cannot guess recovery or success");
    }
    Wsi failed_present {{VK_SUCCESS}, {VK_ERROR_DEVICE_LOST}};
    Check(failed_present.Run() == PresentationCycle::Fatal && failed_present.submitted == 1 &&
          failed_present.presented == 1 && failed_present.recovered == 0,
          "fatal present never replays submitted effects");
    Check(PresentStatus(VK_ERROR_UNKNOWN) == SwapchainStatus::Fatal,
          "unknown present remains fatal");
    std::puts("SwapchainPresentPolicyTests: PASS; persistent advisory=100 single-effect cycles");
    return 0;
}
