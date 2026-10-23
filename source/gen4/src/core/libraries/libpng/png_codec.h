// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "common/types.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/hle_call_adapter.h"
#endif

namespace Libraries::Png {
constexpr u64 MaxStagingBytes = 256_MB;
constexpr u32 WorkspaceSize = 16;

inline bool RangeValid(const void* address, u64 size, bool writable = false) {
    if (!address || size > std::numeric_limits<uintptr_t>::max() -
                           reinterpret_cast<uintptr_t>(address)) return false;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    return Core::GuestCpu::ValidateHlePointer(address, size, writable);
#else
    return true;
#endif
}

// All operands have already passed RangeValid, so interval ends cannot wrap.
inline bool Overlaps(const void* left, u64 left_size, const void* right, u64 right_size) {
    if (!left || !right) return false;
    const auto a = reinterpret_cast<uintptr_t>(left);
    const auto b = reinterpret_cast<uintptr_t>(right);
    return a < b + right_size && b < a + left_size;
}

class StagingReservation {
public:
    explicit StagingReservation(u64 bytes) : bytes{bytes} {
        auto current = used.load(std::memory_order_relaxed);
        do {
            if (bytes > MaxStagingBytes - current) throw std::bad_alloc{};
        } while (!used.compare_exchange_weak(current, current + bytes,
                                             std::memory_order_relaxed));
    }
    ~StagingReservation() { used.fetch_sub(bytes, std::memory_order_relaxed); }
    StagingReservation(const StagingReservation&) = delete;
    StagingReservation& operator=(const StagingReservation&) = delete;
private:
    inline static std::atomic<u64> used{};
    u64 bytes;
};

struct State {
    std::mutex mutex;
    u32 max_width{};
    bool alive{true};
};

// Guest workspaces identify handles; they never contain host libpng pointers.
// Each call owns its own codec so an error or a completed image cannot poison
// the next image. The per-handle lock also serializes Delete against that call.
class Registry {
public:
    bool Create(void* workspace, u32 max_width) {
        auto state = std::make_shared<State>();
        state->max_width = max_width;
        std::scoped_lock lock{mutex};
        if (handles.size() >= 256) throw std::bad_alloc{};
        return handles.emplace(workspace, std::move(state)).second;
    }
    std::shared_ptr<State> Find(void* handle) {
        std::scoped_lock lock{mutex};
        const auto it = handles.find(handle);
        return it == handles.end() ? nullptr : it->second;
    }
    bool Delete(void* handle) {
        auto state = Find(handle);
        if (!state) return false;
        std::scoped_lock state_lock{state->mutex};
        std::scoped_lock registry_lock{mutex};
        const auto it = handles.find(handle);
        if (!state->alive || it == handles.end() || it->second != state) return false;
        state->alive = false;
        handles.erase(it);
        return true;
    }
private:
    std::mutex mutex;
    std::unordered_map<void*, std::shared_ptr<State>> handles;
};
} // namespace Libraries::Png
