// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>

namespace VideoCore {
// Compatibility opt-in. No image read watches or downloads are introduced
// into the default renderer. Readbacks happen only when CPU ownership returns.
inline bool ImageCpuReadbackEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_IMAGE_CPU_READBACK");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
} // namespace VideoCore
