// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdlib>
#include <utility>

namespace VideoCore {

constexpr bool BufferCopyResolveSelectorEnabled(const char* value) {
    return !value || value[0] != '0' || value[1] != '\0';
}

// General Gen4 correction; exact "0" retains an independent rollback.
inline bool BufferCopyResolveEnabled() {
    static const bool enabled = BufferCopyResolveSelectorEnabled(
        std::getenv("LUMEN_BUFFER_COPY_RESOLVE"));
    return enabled;
}

// FindBuffer can relocate SlotVector storage and merge/retire the source while
// resolving the destination. Keep IDs only, then look up the current source.
// A merge includes the entire old buffer, so the last lookup cannot allocate or
// retire the destination. Take references only after this preparation finishes.
template <typename Find>
auto ResolveBufferCopyIds(std::uint64_t src, std::uint64_t dst, std::uint32_t size,
                          Find&& find) {
    find(src, size);
    const auto dst_id = find(dst, size);
    return std::pair{find(src, size), dst_id};
}

constexpr bool BufferCopyRangesOverlap(std::uint64_t src, std::uint64_t dst,
                                       std::uint32_t size) {
    return size != 0 && (src <= dst ? dst - src < size : src - dst < size);
}

} // namespace VideoCore
