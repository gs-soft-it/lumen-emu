// Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
#pragma once

// The standalone CPU bridge has no dependency on the complete engine's
// generated configuration, formatter or logging subsystem. A failed epoch
// close/readmission must still terminate execution on that build graph.
#if defined(LUMEN_PS5_STANDALONE_BRIDGE)
#include <cstdio>
#include <cstdlib>

namespace Loader::GuestExecution {
[[noreturn]] inline void TerminateGuestCoordination(const char* message) noexcept {
    std::fputs(message, stderr);
    std::fflush(stderr);
    std::_Exit(321);
}
} // namespace Loader::GuestExecution

#define LUMEN_PS5_GUEST_COORDINATION_FAILURE(message) \
    ::Loader::GuestExecution::TerminateGuestCoordination(message)
#else
#include "common/assert.h"
#define LUMEN_PS5_GUEST_COORDINATION_FAILURE(message) EXIT(message)
#endif
