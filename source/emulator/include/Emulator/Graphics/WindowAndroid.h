#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_ANDROID_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_ANDROID_H_

#include <cstdint>

namespace Lumen::Libs::Graphics {

// Android owns the runtime HUD in the Java activity layered above SDL's
// SurfaceView. These hooks keep SDL as the authority for controller presence
// and Vulkan presents as the authority for the FPS sample.
void WindowAndroidRuntimeInputReady(bool ready);
void WindowAndroidControllerStateChanged(bool connected, bool notify);
void WindowAndroidDidPresentFrame(uint64_t frame_number);
void WindowAndroidRuntimeProgress(uint32_t progress_permille, const char* message);

} // namespace Lumen::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_ANDROID_H_
