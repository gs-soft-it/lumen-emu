// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Vulkan::SwapchainPolicy {
enum class Outcome { Success, Suboptimal, OutOfDate, SurfaceLost, DeviceLost };
constexpr bool MustRecreate(bool android, Outcome result) {
    switch(result) {
    case Outcome::Success:return false;
    // Android may return this persistently with the same extent/image count.
    // The image is still acquired/presented. Explicit resize and out-of-date
    // handling remain responsible for a real swapchain replacement.
    case Outcome::Suboptimal:return !android;
    case Outcome::OutOfDate:
    case Outcome::SurfaceLost:
    case Outcome::DeviceLost:return true;
    }
    return true;
}
}
