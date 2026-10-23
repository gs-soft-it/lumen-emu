// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#endif

namespace Libraries::Np {

template <typename Fn, typename... Args>
void InvokeNpCallback(Fn callback, std::string_view label, Args... args) {
    if (callback == nullptr) {
        return;
    }
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::GuestCpu::RunGuestFunctionOrAbort(reinterpret_cast<const void*>(callback), label, args...);
#else
    callback(args...);
#endif
}

} // namespace Libraries::Np
