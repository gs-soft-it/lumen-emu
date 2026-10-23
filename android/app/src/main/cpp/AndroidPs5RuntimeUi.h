// SPDX-License-Identifier: GPL-2.0-only
#ifndef LUMEN_ANDROID_PS5_RUNTIME_UI_H_
#define LUMEN_ANDROID_PS5_RUNTIME_UI_H_

#include "libs/imeCommon.h"

namespace Libs::Graphics {

// Called by the SDL event thread with an owned broker snapshot. A null snapshot
// closes the native editor without cancelling an already completed session.
void SyncAndroidImeDialogUi(const ImeCommon::HostSnapshot* snapshot = nullptr);

} // namespace Libs::Graphics

#endif // LUMEN_ANDROID_PS5_RUNTIME_UI_H_
