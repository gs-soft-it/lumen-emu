// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <string_view>

namespace Lumen::FexCore {

inline constexpr std::uint32_t kDefaultMaximumBlockInstructions = 128;

// Region-size policy shared by all FEX hosts, with an explicit 5000 rollback. Zero denotes
// a malformed selector and must fail admission; it is not a FEX block limit.
constexpr std::uint32_t ParseMaximumBlockInstructions(const char* value) noexcept {
    if (value == nullptr) return kDefaultMaximumBlockInstructions;
    const std::string_view selection(value);
    if (selection == "5000") return 5000;
    if (selection == "128") return 128;
    return 0;
}

} // namespace Lumen::FexCore
