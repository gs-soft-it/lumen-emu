// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Common {

/// Call once at startup (from SignalDispatch ctor). Checks LUMEN_HOST_CRASH_REGISTERS.
void InitCrashReporter();

/// Async-signal-safe: dumps registers + fault info to stdout when enabled.
/// Call only for an unhandled fault with a valid signal context.
void ReportCrash(void* raw_context, int signum, void* siginfo);

/// Records the last spilled guest state, which may be stale during JIT execution.
/// Async-signal-safe and controlled by the same diagnostic opt-in as ReportCrash.
void ReportGuestCrashSnapshot(std::uint64_t rip, std::uint64_t rax);

} // namespace Common
