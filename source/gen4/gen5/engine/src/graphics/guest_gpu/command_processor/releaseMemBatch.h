// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

namespace Libs::Graphics {

// After the first deferred release, at most 16 PM4 packets are fetched, including the
// release itself. The budget is not restarted by later releases. A native
// readback may already have submitted its tick; never flush that stale request.
class ReleaseMemBatch {
public:
    static constexpr uint32_t PacketBudget = 16;

    void Request(uint64_t tick) {
        Rebase(tick);
        if (!m_pending) {
            m_tick = tick;
            m_packets = 0;
            m_pending = true;
        }
    }
    [[nodiscard]] bool ConsumePacket(uint64_t tick) {
        Rebase(tick);
        return m_pending && ++m_packets >= PacketBudget;
    }
    [[nodiscard]] bool TakePending(uint64_t tick) {
        Rebase(tick);
        const bool pending = m_pending;
        Reset();
        return pending;
    }
    [[nodiscard]] bool IsPending() const { return m_pending; }
    void Reset() { m_pending = false; m_packets = 0; }

private:
    void Rebase(uint64_t tick) {
        if (m_pending && m_tick != tick) Reset();
    }
    uint64_t m_tick = 0;
    uint32_t m_packets = 0;
    bool m_pending = false;
};

} // namespace Libs::Graphics
