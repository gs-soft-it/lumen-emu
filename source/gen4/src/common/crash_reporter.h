// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Common {

/// Call once at startup (from SignalDispatch ctor). Checks LUMEN_HOST_CRASH_REGISTERS.
void InitCrashReporter();

bool IsCrashReporterEnabled() noexcept;

/// Best-effort fatal-signal diagnostic: dumps registers and bounded memory when enabled.
/// Call only for an unhandled fault with a valid signal context.
void ReportCrash(void* raw_context, int signum, void* siginfo);

/// Records the last spilled guest state, which may be stale during JIT execution.
/// Async-signal-safe and controlled by the same diagnostic opt-in as ReportCrash.
void ReportGuestCrashSnapshot(std::uint64_t rip, std::uint64_t rax);

/// Records a guest location inferred from JIT metadata; metadata may fall back
/// to the last spilled instruction when a precise mapping is unavailable.
void ReportGuestCrashJitRip(std::uint64_t host_pc, std::uint64_t guest_rip);

} // namespace Common
