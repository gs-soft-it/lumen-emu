// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <string>
#include <string_view>

namespace Lumen::Dialog {

// Guest user messages carry free text, not a network-status discriminator.
// Recognize only explicit offline connectivity notices. Callers must separately
// require an acknowledgement-only dialog; questions and other UI remain visible.
inline bool IsOfflineConnectivityNotice(std::string_view message) {
    std::string normalized;
    normalized.reserve(message.size());
    for (std::size_t index = 0; index < message.size(); ++index) {
        unsigned char ch = static_cast<unsigned char>(message[index]);
        if (message.substr(index, 3) == "\xe2\x80\x99") {
            ch = '\'';
            index += 2;
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') {
            if (!normalized.empty() && normalized.back() != ' ') normalized.push_back(' ');
        } else {
            normalized.push_back(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
        }
    }
    constexpr std::array phrases{
        "not connected to internet", "not connected to the internet",
        "not currently connected to internet", "not currently connected to the internet",
        "isn't connected to internet", "isn't connected to the internet",
        "aren't connected to internet", "aren't connected to the internet",
        "non sei connesso a internet", "non sei connesso ad internet",
        "non siete connessi a internet", "non connesso a internet",
        "non connessi a internet", "non è connesso a internet",
    };
    for (const std::string_view phrase : phrases) {
        const auto at = normalized.find(phrase);
        if (at == std::string::npos) continue;
        const auto end = at + phrase.size();
        const auto is_word = [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                   ch == '-' || ch == '_' || ch >= 0x80;
        };
        if ((at == 0 || !is_word(normalized[at - 1])) &&
            (end == normalized.size() || !is_word(normalized[end]))) {
            return true;
        }
    }
    return false;
}

} // namespace Lumen::Dialog
