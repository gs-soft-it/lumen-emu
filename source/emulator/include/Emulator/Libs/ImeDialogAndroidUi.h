// SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_IME_DIALOG_ANDROID_UI_H
#define LUMEN_IME_DIALOG_ANDROID_UI_H

#include "Emulator/Libs/ImeDialog.h"

namespace Lumen::Libs::Dialog::ImeDialog {

// Called on the SDL event thread. Java receives owned UTF-16 copies and posts
// presentation to its UI thread; nullptr dismisses without canceling the model.
void SyncAndroidImeDialog(const HostSnapshot* snapshot = nullptr);

} // namespace Lumen::Libs::Dialog::ImeDialog
#endif
