// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <sys/stat.h>

namespace Lumen::Documents::Detail {

// A snapshot of successful metadata reads within one synchronous filesystem
// operation. No result survives the outermost scope, an error or a mount change.
// The provider still admits every opened descriptor independently.
class ReadOperationCache {
public:
    static constexpr std::size_t MaxEntries = 128;
    static constexpr std::size_t MaxPathBytes = 4096;
    static constexpr std::size_t MaxKeyBytes = 64 * 1024;

    void Begin() {
        if (depth++ == 0) Reset();
    }

    void End() {
        if (depth != 0 && --depth == 0) Reset();
    }

    bool Get(const std::string& path, std::uint64_t mount_generation, struct stat& result) {
        Refresh(mount_generation);
        if (depth == 0 || disabled) return false;
        const auto found = entries.find(path);
        if (found == entries.end()) return false;
        result = found->second;
        return true;
    }

    void Put(const std::string& path, std::uint64_t mount_generation, const struct stat& result) {
        Refresh(mount_generation);
        if (depth == 0 || disabled) return;
        if (const auto found = entries.find(path); found != entries.end()) {
            found->second = result;
            return;
        }
        if (entries.size() >= MaxEntries || path.size() > MaxPathBytes ||
            path.size() > MaxKeyBytes - key_bytes) {
            Fail();
            return;
        }
        entries.emplace(path, result);
        key_bytes += path.size();
    }

    // A later request in the same scope must consult the provider after any
    // failed access, never recover using metadata observed before that error.
    void Fail() {
        entries.clear();
        key_bytes = 0;
        disabled = true;
    }

private:
    void Reset() {
        entries.clear();
        key_bytes = 0;
        has_generation = false;
        disabled = false;
    }

    void Refresh(std::uint64_t mount_generation) {
        if (!has_generation || generation != mount_generation) {
            entries.clear();
            key_bytes = 0;
            generation = mount_generation;
            has_generation = true;
        }
    }

    std::unordered_map<std::string, struct stat> entries;
    std::size_t depth{};
    std::size_t key_bytes{};
    std::uint64_t generation{};
    bool has_generation{};
    bool disabled{};
};

inline ReadOperationCache& CurrentReadOperation() {
    static thread_local ReadOperationCache cache;
    return cache;
}

} // namespace Lumen::Documents::Detail
