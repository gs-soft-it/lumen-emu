// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string_view>

namespace Lumen::FexCore {

// Read the initialized runtime's effective limit for the Android dashboard,
// never a new environment selection. Unknown, incomplete or repeated fields
// cannot report readiness.
constexpr std::uint32_t MaximumBlockInstructionsFromReceipt(std::string_view receipt) noexcept {
    constexpr std::string_view marker = " maxInst=";
    const auto start = receipt.find(marker);
    if (start == std::string_view::npos ||
        receipt.find(marker, start + marker.size()) != std::string_view::npos) return 0;
    const auto value = receipt.substr(start + marker.size());
    const auto end = value.find(' ');
    if (end == std::string_view::npos) return 0;
    const auto field = value.substr(0, end);
    if (field == "128") return 128;
    if (field == "5000") return 5000;
    return 0;
}

} // namespace Lumen::FexCore
