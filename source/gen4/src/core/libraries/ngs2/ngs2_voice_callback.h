// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ngs2.h"
#include "ngs2_error.h"

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include <array>
#include "core/guest_cpu/guest_callback.h"
#include "core/guest_cpu/guest_callback_scratch.h"
#include "core/guest_cpu/hle_call_adapter.h"
#endif

namespace Libraries::Ngs2::Detail {

// The caller owns callback ordering and must release any lock that a guest
// callback can reenter. This helper does not access the NGS2 registry.
inline s32 InvokeVoiceCallback(OrbisNgs2VoiceCallbackHandler handler,
                               const OrbisNgs2VoiceCallbackInfo& info,
                               bool native_registration = false) {
    if (!handler) return 0;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    // Native callers can register a host callback for the public native API.
    // Preserve that explicit provenance: an unknown/failed guest address is
    // never retried as native code, and guest HLE cannot invoke a host callback.
    if (native_registration) {
        if (Core::GuestCpu::IsGuestHleCallActive()) return ORBIS_NGS2_ERROR_UNABLE_CALLBACK;
        handler(&info);
        return 0;
    }
    Core::GuestCpu::GuestCallbackScratch<OrbisNgs2VoiceCallbackInfo> scratch{info};
    if (!scratch.Data()) return ORBIS_NGS2_ERROR_UNABLE_CALLBACK;
    const std::array<u64, 1> arguments{reinterpret_cast<u64>(scratch.Data())};
    const auto result = Common::Singleton<Core::Linker>::Instance()->RunGuestFunction(
        reinterpret_cast<VAddr>(handler), arguments);
    if (std::holds_alternative<Core::GuestExecutionFailure>(result))
        return ORBIS_NGS2_ERROR_UNABLE_CALLBACK;
#else
    (void)native_registration;
    handler(&info);
#endif
    return 0;
}

} // namespace Libraries::Ngs2::Detail
