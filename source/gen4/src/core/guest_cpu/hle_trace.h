// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef LUMEN_GEN4_DIRECT_FEX
#include <string_view>
#endif

namespace Core::GuestCpu {

// LUMEN_HOST_FEX_HLE_TRACE stderr dump.
inline bool LumenHostHleTraceRequested(const char* value) {
#ifdef LUMEN_GEN4_DIRECT_FEX
    if (value == nullptr) return false;
    const std::string_view text{value};
    const auto equals = [&](std::string_view expected) {
        if (text.size() != expected.size()) return false;
        for (std::size_t index = 0; index < text.size(); ++index) {
            const auto ch = static_cast<unsigned char>(text[index]);
            const auto lower = ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
            if (lower != expected[index]) return false;
        }
        return true;
    };
    return equals("1") || equals("true") || equals("yes") || equals("on");
#else
    return value != nullptr && value[0] != '\0' && value[0] != '0';
#endif
}

} // namespace Core::GuestCpu
