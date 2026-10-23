// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <filesystem>

namespace Lumen::Gen4 {
// Resolve existing parents as well as the final component. This also confines
// new save files whose final component does not exist yet.
inline bool PathWithinRoot(const std::filesystem::path& root,
                           const std::filesystem::path& candidate) {
    std::error_code error;
    const auto base = std::filesystem::weakly_canonical(root, error);
    if (error || base.empty()) return false;
    const auto resolved = std::filesystem::weakly_canonical(candidate, error);
    if (error || resolved.empty()) return false;
    auto item = resolved.begin();
    for (const auto& part : base) {
        if (item == resolved.end() || *item++ != part) return false;
    }
    return true;
}
}
