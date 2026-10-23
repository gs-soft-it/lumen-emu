#include "Emulator/Graphics/WindowAndroid.h"
#include "Emulator/Libs/ImeDialogAndroidUi.h"

namespace Lumen::Libs::Graphics {

// This native gate links the emulator archive without AndroidRuntimeUi.cpp.
// Keep the UI boundary inert and type-checked against its production header.
void WindowAndroidRuntimeInputReady(bool) {}

void WindowAndroidControllerStateChanged(bool, bool) {}

void WindowAndroidDidPresentFrame(uint64_t) {}

void WindowAndroidRuntimeProgress(uint32_t, const char*) {}

} // namespace Lumen::Libs::Graphics

namespace Lumen::Libs::Dialog::ImeDialog {

// The standalone native gate has no Java UI or JNI runtime attached.
void SyncAndroidImeDialog(const HostSnapshot*) {}

} // namespace Lumen::Libs::Dialog::ImeDialog
