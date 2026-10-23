// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>

namespace VideoCore {

constexpr bool BufferPendingWriteResolveSelectorEnabled(const char* value) {
    return !value || value[0] != '0' || value[1] != '\0';
}

// Resolve deferred CPU writes before reusing an upload on every Gen4 host.
// Only the exact value "0" selects the previous behavior for rollback.
inline bool BufferPendingWriteResolveEnabled() {
    static const bool enabled = BufferPendingWriteResolveSelectorEnabled(
        std::getenv("LUMEN_BUFFER_PENDING_WRITE_RESOLVE"));
    return enabled;
}

} // namespace VideoCore
