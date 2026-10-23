// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <initializer_list>
#include "video_core/renderer_vulkan/vk_swapchain_policy.h"
using namespace Vulkan::SwapchainPolicy;
int main() {
    // Persistent suboptimal on either acquire or present must not churn the
    // Android swapchain, while an actual invalidation still requests recovery.
    for(unsigned frame=0;frame<128;++frame) {
        if(MustRecreate(true,Outcome::Suboptimal)||MustRecreate(true,Outcome::Success))return 1;
    }
    for(bool android:{false,true}) {
        if(MustRecreate(android,Outcome::Success)||!MustRecreate(android,Outcome::OutOfDate)||
            !MustRecreate(android,Outcome::SurfaceLost)||!MustRecreate(android,Outcome::DeviceLost))return 2;
    }
    if(!MustRecreate(false,Outcome::Suboptimal))return 3;
    std::puts("Android persistent-suboptimal and invalidation status policy passed");
    return 0;
}
