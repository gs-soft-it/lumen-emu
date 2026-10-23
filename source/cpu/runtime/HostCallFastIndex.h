#ifndef LUMEN_GUEST_CPU_HOST_CALL_FAST_INDEX_H
#define LUMEN_GUEST_CPU_HOST_CALL_FAST_INDEX_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace Lumen::GuestCpu {

inline bool HostCallFastIndexRequested(const char* value) noexcept {
    return value != nullptr && std::strcmp(value, "1") == 0;
}

inline bool HostCallFastIndexEnabled() noexcept {
    static const bool enabled = HostCallFastIndexRequested(
        std::getenv("LUMEN_FEX_HOST_CALL_FAST_INDEX"));
    return enabled;
}

// An owned, immutable address-to-ordinal index. A hit only identifies a row in
// the caller's snapshot: it never grants native-call authority on its own.
// Keys retain all 64 bits, including zero/UINT64_MAX; occupied is represented
// separately so neither a hash collision nor a sentinel can admit a miss.
class HostCallFastIndex final {
public:
    static constexpr std::size_t NotFound = std::numeric_limits<std::size_t>::max();

    HostCallFastIndex() = default;

    explicit HostCallFastIndex(const std::vector<std::uint64_t>& addresses) {
        if (addresses.empty()) return;
        const std::size_t maximum = slots_.max_size();
        if (addresses.size() > maximum / 2) {
            usable_ = false;
            return;
        }
        const std::size_t required = addresses.size() * 2;
        std::size_t capacity = 2;
        while (capacity < required) {
            if (capacity > maximum / 2) {
                usable_ = false;
                return;
            }
            capacity *= 2;
        }
        slots_.resize(capacity);
        for (std::size_t ordinal = 0; ordinal != addresses.size(); ++ordinal) {
            const auto address = addresses[ordinal];
            std::size_t slot = Hash(address) & (capacity - 1);
            while (slots_[slot].ordinal != NotFound && slots_[slot].address != address)
                slot = (slot + 1) & (capacity - 1);
            // Preserve the first row if a registration snapshot has aliases.
            if (slots_[slot].ordinal == NotFound) slots_[slot] = {address, ordinal};
        }
    }

    // An unrepresentable table leaves the caller's original lookup in use.
    // Explicit exceptions are unavailable in the standalone FEX runtime.
    [[nodiscard]] bool IsUsable() const noexcept { return usable_; }

    [[nodiscard]] std::size_t Find(std::uint64_t address) const noexcept {
        if (slots_.empty()) return NotFound;
        const std::size_t mask = slots_.size() - 1;
        std::size_t slot = Hash(address) & mask;
        // The construction load is at most 1/2. The explicit bound also makes
        // a miss terminate without depending on an unbounded probe loop.
        for (std::size_t probes = 0; probes != slots_.size(); ++probes) {
            const auto& entry = slots_[slot];
            if (entry.ordinal == NotFound) return NotFound;
            if (entry.address == address) return entry.ordinal;
            slot = (slot + 1) & mask;
        }
        return NotFound;
    }

    // All pointer bits contribute; aligned functions must not crowd one bucket.
    [[nodiscard]] static std::size_t Hash(std::uint64_t address) noexcept {
        address ^= address >> 30;
        address *= UINT64_C(0xbf58476d1ce4e5b9);
        address ^= address >> 27;
        address *= UINT64_C(0x94d049bb133111eb);
        address ^= address >> 31;
        return static_cast<std::size_t>(address);
    }

private:
    struct Entry {
        std::uint64_t address = 0;
        std::size_t ordinal = NotFound;
    };
    std::vector<Entry> slots_;
    bool usable_ = true;
};

} // namespace Lumen::GuestCpu

#endif
