// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#if defined(__APPLE__)
#include <crt_externs.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Lumen::Gen4 {

// Keep the desktop process entry consistent with Android's
// RuntimeActivity.configureFexDefaultProfile. In particular, preserve the
// original environment loader's interpretation of the SMC literal.
inline void ConfigureDesktopFexProfile() {
    const char* translator = std::getenv("LUMEN_CPU_TRANSLATOR");
    if (translator && *translator && std::string_view{translator} != "fex") {
        throw std::system_error(ENOTSUP, std::generic_category(),
                                "The direct Gen4 runtime requires the FEX translator");
    }
    std::vector<std::string> inherited;
    for (auto** entry = *_NSGetEnviron(); entry && *entry; ++entry) {
        const std::string_view value{*entry};
        if (value.starts_with("FEX_")) {
            inherited.emplace_back(value.substr(0, value.find('=')));
        }
    }
    for (const auto& name : inherited) {
        if (unsetenv(name.c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(), "Reset FEX profile");
        }
    }
    constexpr std::array<std::pair<const char*, const char*>, 8> profile{{
        {"FEX_TSOENABLED", "1"}, {"FEX_HALFBARRIERTSOENABLED", "1"},
        {"FEX_VECTORTSOENABLED", "0"}, {"FEX_STRICTINPROCESSSPLITLOCKS", "0"},
        {"FEX_SMCCHECKS", "mtrack"}, {"FEX_MULTIBLOCK", "1"},
        {"FEX_MAXINST", "5000"}, {"FEX_DYNAMICL1CACHE", "1"},
    }};
    for (const auto& [name, value] : profile) {
        if (setenv(name, value, 1) != 0) {
            throw std::system_error(errno, std::generic_category(), "Configure FEX profile");
        }
    }
}

} // namespace Lumen::Gen4
#endif
