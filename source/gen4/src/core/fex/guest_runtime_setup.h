// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/guest_cpu/fex_guest_cpu.h"
#include "core/signals.h"

namespace Core::Fex {

// The translator chains the core's native fault handlers. Install that
// predecessor before its runtime probe, so a renderer's first SRT walker only
// registers a callback and cannot replace the active translator signal handler.
inline FexGuestCpuBackend::CreateResult CreateGuestBackend(GuestBridge& bridge) {
    (void)Core::Signals::Instance();
    return FexGuestCpuBackend::Create(bridge);
}

} // namespace Core::Fex
