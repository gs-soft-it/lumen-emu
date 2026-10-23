// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Common::UniformMetadata {

inline bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("LUMEN_PS5_UNIFORM_METADATA");
        if (value == nullptr || std::strcmp(value, "1") == 0) return true;
        if (std::strcmp(value, "0") == 0) return false;
        std::fprintf(stderr, "LUMEN_PS5_UNIFORM_METADATA must be 0 or 1\n");
        std::abort();
    }();
    return enabled;
}

// Renderer-owner-only provenance of actual ordered GPU writes. This is not
// guest backing and never makes bytes CPU-visible or changes dirty ownership.
class KnownRanges final {
public:
    static constexpr std::size_t Capacity = 64;

    void Clear() noexcept {
        m_entries = {};
        m_next = 0;
    }

    void Invalidate(uint64_t address, uint64_t size) noexcept {
        if (!Valid(address, size)) {
            Clear();
            return;
        }
        for (auto& entry : m_entries) {
            if (entry.size != 0 && address < entry.address + entry.size &&
                entry.address < address + size) entry = {};
        }
    }

    void Remember(uint64_t address, uint64_t size, uint32_t value, uint64_t generation) noexcept {
        if (!Valid(address, size) || ((address | size) & 3u) != 0 || !Stable(generation)) return;
        Invalidate(address, size);
        for (auto& entry : m_entries) {
            if (entry.generation != generation) entry = {};
        }
        for (auto& entry : m_entries) {
            if (entry.size == 0) {
                entry = {address, size, generation, value};
                return;
            }
        }
        m_entries[m_next] = {address, size, generation, value};
        m_next = (m_next + 1) % Capacity;
    }

    [[nodiscard]] bool Lookup(uint64_t address, uint64_t size, uint64_t generation,
                              uint32_t* value) const noexcept {
        if (!Valid(address, size) || ((address | size) & 3u) != 0 ||
            !Stable(generation) || value == nullptr) return false;
        for (const auto& entry : m_entries) {
            if (entry.generation == generation && address >= entry.address &&
                address - entry.address <= entry.size &&
                size <= entry.size - (address - entry.address)) {
                *value = entry.value;
                return true;
            }
        }
        return false;
    }

private:
    static bool Valid(uint64_t address, uint64_t size) noexcept {
        return address != 0 && size != 0 && size <= UINT64_MAX - address;
    }
    static bool Stable(uint64_t generation) noexcept { return generation != 0 && (generation & 1) == 0; }
    struct Entry {
        uint64_t address = 0;
        uint64_t size = 0;
        uint64_t generation = 0;
        uint32_t value = 0;
    };
    std::array<Entry, Capacity> m_entries {};
    std::size_t m_next = 0;
};

} // namespace Common::UniformMetadata
