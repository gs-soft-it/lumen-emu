// SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_IME_DIALOG_DESKTOP_UI_H
#define LUMEN_IME_DIALOG_DESKTOP_UI_H

#include "Emulator/Libs/ImeDialog.h"

struct SDL_Window;
union SDL_Event;

#ifdef LUMEN_EMU_ENABLED
namespace Lumen::Libs::Dialog::ImeDialog {

// All calls belong to the existing SDL window thread. The independent software
// window renders owned snapshots; its buttons never access guest memory.
void SyncDesktopUi(const HostSnapshot* snapshot, SDL_Window* parent);
bool ProcessDesktopUi(const SDL_Event& event);
void CloseDesktopUi();

} // namespace Lumen::Libs::Dialog::ImeDialog
#endif
#endif
