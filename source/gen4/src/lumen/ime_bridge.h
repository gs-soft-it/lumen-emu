// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace Lumen::Gen4::Ime {
constexpr uint32_t IME_DIALOG_MAX_TEXT_LENGTH = 2048;
struct HostSnapshot {
    uint64_t generation{};
    std::u16string text, title, placeholder;
    uint32_t cursor{}, max_text_length{}, type{}, option{}, enter_label{};
};
struct Command { std::u16string text; uint32_t cursor{}; int finish{-1}; };
struct Model {
    std::mutex mutex;
    HostSnapshot snapshot;
    uint64_t next_generation{};
    std::optional<Command> pending;
    bool finish_queued{};
};
inline Model& State() { static Model value; return value; }
#ifdef __ANDROID__
void SyncAndroidImeDialog();
#endif
inline void Publish(HostSnapshot snapshot) {
    {
        auto& state = State(); std::lock_guard lock(state.mutex);
        snapshot.generation = ++state.next_generation;
        state.snapshot = snapshot;
        state.pending.reset();
        state.finish_queued = false;
    }
#ifdef __ANDROID__
    SyncAndroidImeDialog();
#endif
}
inline void Republish() {
#ifdef __ANDROID__
    SyncAndroidImeDialog();
#endif
}
inline void Close() {
    { auto& state = State(); std::lock_guard lock(state.mutex); state.snapshot = {}; state.pending.reset(); state.finish_queued = false; }
#ifdef __ANDROID__
    SyncAndroidImeDialog();
#endif
}
inline bool HostReplaceText(uint64_t generation, const std::u16string& text, uint32_t cursor) {
    auto& state = State(); std::lock_guard lock(state.mutex);
    if (!generation || generation != state.snapshot.generation || cursor > text.size() ||
        text.size() > state.snapshot.max_text_length || state.finish_queued) return false;
    state.pending = Command{text, cursor};
    state.snapshot.text = text; state.snapshot.cursor = cursor;
    return true;
}
inline bool Finish(uint64_t generation, int result) {
    auto& state = State(); std::lock_guard lock(state.mutex);
    if (!generation || generation != state.snapshot.generation || state.finish_queued) return false;
    if (!state.pending) state.pending = Command{state.snapshot.text, state.snapshot.cursor};
    state.pending->finish = result;
    state.finish_queued = true;
    return true;
}
inline bool HostAccept(uint64_t generation) { return Finish(generation, 0); }
inline bool HostCancel(uint64_t generation) { return Finish(generation, 1); }
inline std::optional<Command> TakeCommand() {
    auto& state = State(); std::lock_guard lock(state.mutex);
    auto command = std::move(state.pending); state.pending.reset(); return command;
}
}
