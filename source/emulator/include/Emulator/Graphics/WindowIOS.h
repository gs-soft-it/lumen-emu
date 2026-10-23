#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_IOS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_IOS_H_

#include <stdint.h>

struct SDL_Window;

namespace Lumen::Libs::Graphics {

// Logs Apple GameController discovery together with SDL's controller view.
// The marker is intentionally available before SDL initialization so device
// capability and SDL mapping failures can be distinguished on a real phone.
void WindowIOSLogControllerState(const char* phase, SDL_Window* window);

// Shows SDL's public UIKit window on the main thread and verifies that its
// view is attached and visible. No application shell or overlay is supplied.
// The public iOS build currently exposes only the CPU/VM bootstrap; an
// embedding application must integrate the full runtime separately.
bool WindowIOSShowRuntime(SDL_Window* window, uint32_t logical_width, uint32_t logical_height);

// Logs the first completed presentation without an application HUD.
void WindowIOSDidPresentFrame(uint64_t frame_number);

// Logs discovery of a newly opened physical controller.
void WindowIOSControllerDidConnect();

// Legacy notification hook: logs successfully opened physical controllers.
// Virtual controls, if wanted, belong to the embedding application.
void WindowIOSRefreshVirtualControllerAvailability();

// Legacy lifecycle name: hides the SDL runtime window on the main thread.
// The embedding application owns any transition to another host view.
bool WindowIOSReturnToDashboard(SDL_Window* window);

// Legacy lifecycle name: verifies SDL no longer owns the runtime window after
// SDL_DestroyWindow. It does not create or restore an application window.
bool WindowIOSFinalizeDashboardReturn();

} // namespace Lumen::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_IOS_H_
