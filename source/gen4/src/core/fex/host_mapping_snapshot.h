// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <istream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Core::Fex::detail {

struct HostMapping {
    std::uintptr_t Begin{};
    std::uintptr_t End{};
    bool Readable{};
    bool Writable{};
    bool Executable{};
};

// A snapshot belongs to one thread request. It is never reused after the
// request has been validated or treated as ownership of the host mappings.
inline std::optional<std::vector<HostMapping>> ParseHostMappings(std::istream& input) {
    std::vector<HostMapping> mappings;
    std::string line;
    while (std::getline(input, line)) {
        HostMapping mapping;
        const auto* begin = line.data();
        const auto* end = begin + line.size();
        const auto first = std::from_chars(begin, end, mapping.Begin, 16);
        if (first.ec != std::errc{} || first.ptr == end || *first.ptr != '-') {
            return std::nullopt;
        }
        const auto last = std::from_chars(first.ptr + 1, end, mapping.End, 16);
        if (last.ec != std::errc{} || last.ptr == end || *last.ptr != ' ' ||
            mapping.Begin > mapping.End ||
            (!mappings.empty() && mappings.back().End > mapping.Begin)) {
            return std::nullopt;
        }
        const auto* permissions = last.ptr + 1;
        if (end - permissions < 4 ||
            (permissions[0] != 'r' && permissions[0] != '-') ||
            (permissions[1] != 'w' && permissions[1] != '-') ||
            (permissions[2] != 'x' && permissions[2] != '-') ||
            (permissions[3] != 'p' && permissions[3] != 's')) {
            return std::nullopt;
        }
        mapping.Readable = permissions[0] == 'r';
        mapping.Writable = permissions[1] == 'w';
        mapping.Executable = permissions[2] == 'x';
        if (mapping.Begin != mapping.End) mappings.push_back(mapping);
    }
    if (input.bad() || mappings.empty()) return std::nullopt;
    return mappings;
}

enum class HostMappingResult {
    Valid,
    Gap,
    CodePermission,
    WritePermission,
};

inline HostMappingResult CheckHostMapping(std::span<const HostMapping> mappings,
                                         std::uintptr_t begin, std::size_t size,
                                         bool executable, bool writable) {
    if (size == 0 || begin > UINTPTR_MAX - size) return HostMappingResult::Gap;
    const auto end = begin + size;
    auto cursor = begin;
    auto mapping = std::lower_bound(mappings.begin(), mappings.end(), begin,
        [](const HostMapping& entry, std::uintptr_t address) { return entry.End <= address; });
    for (; mapping != mappings.end(); ++mapping) {
        if (mapping->Begin > cursor) return HostMappingResult::Gap;
        // Guest instructions are translated, so host execute permission is
        // unnecessary. The source must be readable and sealed against writes.
        if (executable && (!mapping->Readable || mapping->Writable)) {
            return HostMappingResult::CodePermission;
        }
        if (writable && (!mapping->Writable || mapping->Executable)) {
            return HostMappingResult::WritePermission;
        }
        if (end <= mapping->End) return HostMappingResult::Valid;
        cursor = mapping->End;
    }
    return HostMappingResult::Gap;
}

} // namespace Core::Fex::detail
