// SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_SHARED_IME_KEYBOARD_PANEL_H_
#define LUMEN_SHARED_IME_KEYBOARD_PANEL_H_

#include "libs/imeCommon.h"

#include <cstdint>
#include <string_view>

namespace Lumen::ImeUi {

// Presentation only: the caller supplies an owned snapshot and delegates each
// action to its engine's existing IME owner. This panel never owns an editor,
// accesses guest memory, initializes SDL, or opens input devices.
struct KeyboardPanelActions {
    bool (*insert_text)(uint64_t, std::u16string_view);
    bool (*replace_text)(uint64_t, std::u16string_view, uint32_t);
    bool (*backspace)(uint64_t);
    bool (*move_cursor)(uint64_t, int);
    bool (*accept)(uint64_t);
    bool (*cancel)(uint64_t);
};

struct KeyboardPanelState {
    uint64_t generation = 0;
    bool shift = false;
    bool symbols = false;
    bool focus_pending = false;

    void Reset(uint64_t token, bool initial_shift = false, bool initial_focus = false)
    {
        generation = token;
        shift = initial_shift;
        symbols = false;
        focus_pending = initial_focus;
    }
};

// Draws inside the caller's current ImGui window/context. Acceptance and the
// readonly field remain outside the scrollable keys when space is limited.
void DrawKeyboardPanel(const ::Libs::ImeCommon::HostSnapshot& snapshot,
                       std::u16string_view preedit, const KeyboardPanelActions& actions,
                       KeyboardPanelState& state, float scale = 1.0f);

} // namespace Lumen::ImeUi
#endif
