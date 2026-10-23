// Lumen, 2026-10-09. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "MemoryHandoff.h"

namespace Lumen::GuestCpu {

// A caller-owned cache for one thread's counted execution admission. The
// epoch must change whenever that reader is released, including an HLE,
// safepoint or writer transition. A host-memory reader alone is insufficient:
// additive VM mutations may coexist with it. The page proof must cover every
// physical peer, including dirty bytes outside the requested access, and a
// write proof must find every peer's native upload already revoked.
class MemoryHandoffPageCache final {
public:
    struct Counters {
        std::uint64_t hits {}, page_proofs {}, deferred_proofs {};
    };

    template<typename PageProof>
    bool Try(std::uint64_t epoch, std::uint64_t address, std::uint64_t bytes,
             MemoryAccessIntent intent, PageProof&& proof) noexcept {
        constexpr std::uint64_t page_size = UINT64_C(1) << MemoryHandoffPageShift;
        if (epoch == 0 || bytes == 0 || address > UINT64_MAX - bytes ||
            !ValidMemoryAccessIntent(intent) || bytes > page_size - (address & (page_size - 1))) return false;
        const auto page = address & ~(page_size - 1);
        // The last page has no representable exclusive end for a full proof.
        if (page > UINT64_MAX - page_size) return false;
        auto& entry = entries_[(page >> MemoryHandoffPageShift) & (entries_.size() - 1)];
        if (entry.epoch != epoch || entry.page != page) entry = {epoch, page, 0, {}};
        const bool write = WritesMemory(intent);
        const auto permission = static_cast<std::uint8_t>(write ? 2 : 1);
        if ((entry.permissions & permission) != 0) {
            ++counters_.hits;
            return true;
        }
        auto& deferred = entry.deferred[write ? 1 : 0];
        if (deferred != 0) {
            --deferred;
            ++counters_.deferred_proofs;
            return false;
        }
        ++counters_.page_proofs;
        if (!proof(page, page_size, write ? MemoryAccessIntent::ReadWrite : MemoryAccessIntent::Read)) {
            // This never grants permission: the caller still performs its
            // fresh exact-range preflight/renderer transaction on every miss.
            // Bound overhead for partially pending or dirty pages.
            deferred = 63;
            return false;
        }
        entry.permissions |= write ? 3 : 1;
        return true;
    }

    const Counters& GetCounters() const noexcept { return counters_; }

private:
    struct Entry {
        std::uint64_t epoch {}, page {};
        std::uint8_t permissions {};
        std::array<std::uint8_t, 2> deferred {};
    };
    std::array<Entry, 64> entries_ {};
    Counters counters_ {};
};

} // namespace Lumen::GuestCpu
