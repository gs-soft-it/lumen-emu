// SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_IME_DIALOG_INPUT_H
#define LUMEN_IME_DIALOG_INPUT_H

#include <string>
union SDL_Event;
struct SDL_Window;

namespace Lumen::Libs::Dialog::ImeDialog {

// SDL calls stay on the existing window event thread; guest calls only wake it.
void InitSdlInput();
void CloseSdlInput();
void SyncSdlInput(SDL_Window* window);
bool ProcessSdlEvent(const SDL_Event& event);
std::string HostCaption();
std::u16string HostPreedit();

} // namespace Lumen::Libs::Dialog::ImeDialog
#endif
