// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Lumen::Gen4::VirtualInput {
struct State {
    uint32_t buttons{};
    std::array<int, 6> axes{128, 128, 128, 128, 0, 0};
    bool operator==(const State&) const = default;
};
// JNI owns no controller pointers. Preserve button transitions between SDL
// wakeups, while consecutive axis updates may share their final position.
class Queue {
public:
    void Button(uint32_t mask, bool pressed) {
        std::lock_guard lock(mutex);
        if (!enabled) return;
        const auto before = state.buttons;
        if (pressed) state.buttons |= mask; else state.buttons &= ~mask;
        if (before != state.buttons) pending.push_back(state);
    }
    void Axis(unsigned axis, int value) {
        std::lock_guard lock(mutex);
        if (enabled && axis < state.axes.size()) {
            state.axes[axis] = std::clamp(value, 0, 255);
            dirty = true;
        }
    }
    void Reset() {
        std::lock_guard lock(mutex);
        state = {};
        pending.clear();
        dirty = true;
    }
    void SetEnabled(bool value) {
        std::lock_guard lock(mutex);
        enabled = value;
        state = {};
        pending.clear();
        dirty = true;
    }
    std::vector<State> Drain() {
        std::lock_guard lock(mutex);
        if (dirty && (pending.empty() || pending.back() != state)) pending.push_back(state);
        dirty = false;
        std::vector<State> result;
        result.swap(pending);
        return result;
    }
private:
    std::mutex mutex;
    State state;
    std::vector<State> pending;
    bool dirty{};
    bool enabled{true};
};
}
