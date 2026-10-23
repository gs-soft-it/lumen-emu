// SPDX-License-Identifier: MIT
#pragma once
#include "core/guest_cpu/callback_origin.h"
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
namespace Core::GuestCpu {
struct ThreadCallback {
    uintptr_t function{}, argument{};
    CallbackOrigin origin{CallbackOrigin::Guest};
};
// Pthread itself resides in guest-visible memory. Keep executable provenance
// and copied callback addresses outside that mapping, and never lock through
// callback execution. Entries are removed before dispatch for reentrancy.
class ThreadCallbackRegistry {
public:
    void SetStart(uintptr_t thread, ThreadCallback callback) {
        std::lock_guard lock{mutex}; states[thread].start = callback;
    }
    std::optional<ThreadCallback> TakeStart(uintptr_t thread) {
        std::lock_guard lock{mutex};
        const auto found = states.find(thread);
        if (found == states.end()) return std::nullopt;
        auto result = found->second.start; found->second.start.reset();
        Prune(found); return result;
    }
    void PushCleanup(uintptr_t thread, ThreadCallback callback) {
        std::lock_guard lock{mutex}; states[thread].cleanup.push_back(callback);
    }
    std::optional<ThreadCallback> PopCleanup(uintptr_t thread) {
        std::lock_guard lock{mutex};
        const auto found = states.find(thread);
        if (found == states.end() || found->second.cleanup.empty()) return std::nullopt;
        auto result = found->second.cleanup.back(); found->second.cleanup.pop_back();
        Prune(found); return result;
    }
    void Clear(uintptr_t thread) { std::lock_guard lock{mutex}; states.erase(thread); }
private:
    struct State { std::optional<ThreadCallback> start; std::vector<ThreadCallback> cleanup; };
    using States = std::unordered_map<uintptr_t, State>;
    void Prune(States::iterator found) {
        if (!found->second.start && found->second.cleanup.empty()) states.erase(found);
    }
    std::mutex mutex;
    States states;
};
} // namespace Core::GuestCpu
