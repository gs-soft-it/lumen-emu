#include "Emulator/Graphics/WindowIOS.h"

#import <GameController/GameController.h>
#import <UIKit/UIKit.h>

#include "lumen_config.h"
#include "SDL.h"
#include "SDL_syswm.h"

#include <atomic>
#include <cstdio>

#if LUMEN_APPLE_NATIVE_METAL
extern "C" void LumenMetalSetPresentCompletionCallback(void (*callback)(void*), void* context);
#endif

namespace {

// Only the main thread changes SDL window ownership. Keep an SDL identifier,
// not a retained UIKit window, so finalization can verify SDL destroyed it.
Uint32 gRuntimeWindowId = 0;
std::atomic_uint64_t gCompletedPresentCount {0};

#if LUMEN_APPLE_NATIVE_METAL
void NativeMetalPresentCompleted(void*)
{
	Lumen::Libs::Graphics::WindowIOSDidPresentFrame(
	    gCompletedPresentCount.fetch_add(1, std::memory_order_relaxed) + 1);
}
#endif

void EmitSDLWindowMarker(NSString* marker)
{
	NSLog(@"%@", marker);
	std::fprintf(stdout, "%s\n", marker.UTF8String);
	std::fflush(stdout);
}

UIWindow* ResolveRuntimeWindow(SDL_Window* window)
{
	if (window == nullptr)
	{
		return nil;
	}
	SDL_SysWMinfo info {};
	SDL_VERSION(&info.version);
	if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE || info.subsystem != SDL_SYSWM_UIKIT)
	{
		return nil;
	}
	return info.info.uikit.window;
}

bool HasOpenedPhysicalGameController()
{
	if ((SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0)
	{
		return false;
	}
	const int joystickCount = SDL_NumJoysticks();
	for (int deviceIndex = 0; deviceIndex < joystickCount; deviceIndex++)
	{
		if (SDL_IsGameController(deviceIndex) != SDL_TRUE)
		{
			continue;
		}
		const SDL_JoystickID instanceID = SDL_JoystickGetDeviceInstanceID(deviceIndex);
		SDL_GameController* controller = instanceID >= 0 ? SDL_GameControllerFromInstanceID(instanceID) : nullptr;
		if (controller != nullptr && SDL_GameControllerGetAttached(controller) == SDL_TRUE)
		{
			return true;
		}
	}
	return false;
}

} // namespace

namespace Lumen::Libs::Graphics {

void WindowIOSLogControllerState(const char* phase, SDL_Window* window)
{
	@autoreleasepool
	{
		const char* markerPhase = phase != nullptr ? phase : "unknown";
		NSArray<GCController*>* controllers = GCController.controllers;
		const BOOL sdlReady = (SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) != 0;
		const int sdlCount = sdlReady ? SDL_NumJoysticks() : -1;
		const BOOL hasFocus = window != nullptr && SDL_GetKeyboardFocus() == window;
		EmitSDLWindowMarker([NSString stringWithFormat:
		    @"LUMEN_IOS_CONTROLLER_SCAN phase=%s native=%lu sdl=%d focus=%d app_active=%d main=%d hidapi=%s",
		    markerPhase, static_cast<unsigned long>(controllers.count), sdlCount, hasFocus ? 1 : 0,
		    UIApplication.sharedApplication.applicationState == UIApplicationStateActive ? 1 : 0,
		    NSThread.isMainThread ? 1 : 0, SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI) ?: "default"]);

		NSUInteger index = 0;
		for (GCController* controller in controllers)
		{
			NSString* vendor = controller.vendorName.length > 0 ? controller.vendorName : @"unknown";
			NSString* category = controller.productCategory.length > 0 ? controller.productCategory : @"unknown";
			EmitSDLWindowMarker([NSString stringWithFormat:
			    @"LUMEN_IOS_CONTROLLER_NATIVE phase=%s index=%lu vendor=%@ category=%@ extended=%d attached=%d player=%ld",
			    markerPhase, static_cast<unsigned long>(index), vendor, category, controller.extendedGamepad != nil ? 1 : 0,
			    controller.isAttachedToDevice ? 1 : 0, static_cast<long>(controller.playerIndex)]);
			index++;
		}
	}
}

bool WindowIOSShowRuntime(SDL_Window* window, uint32_t logical_width, uint32_t logical_height)
{
	if (window == nullptr || !NSThread.isMainThread)
	{
		EmitSDLWindowMarker(@"LUMEN_IOS_SDL_WINDOW phase=show reason=invalid-window-or-thread ok=0");
		return false;
	}
	UIWindow* runtimeWindow = ResolveRuntimeWindow(window);
	if (runtimeWindow == nil)
	{
		EmitSDLWindowMarker(@"LUMEN_IOS_SDL_WINDOW phase=show reason=no-uikit-window ok=0");
		return false;
	}

	// SDL owns the view controller, drawable geometry and key-window transition.
	// The core adds no application window, game library, HUD or virtual pad.
	runtimeWindow.opaque = YES;
	runtimeWindow.backgroundColor = UIColor.blackColor;
	runtimeWindow.rootViewController.view.backgroundColor = UIColor.blackColor;
	gRuntimeWindowId = SDL_GetWindowID(window);
	gCompletedPresentCount.store(0, std::memory_order_relaxed);
	SDL_ShowWindow(window);
	SDL_PumpEvents();
	[runtimeWindow layoutIfNeeded];
	const BOOL shown = !runtimeWindow.hidden && runtimeWindow.isKeyWindow &&
	                   runtimeWindow.rootViewController != nil &&
	                   runtimeWindow.rootViewController.view.window == runtimeWindow &&
	                   (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) != 0;
	EmitSDLWindowMarker([NSString stringWithFormat:
	    @"LUMEN_IOS_SDL_WINDOW phase=show width=%u height=%u key=%d hidden=%d ok=%d",
	    logical_width, logical_height, runtimeWindow.isKeyWindow ? 1 : 0,
	    runtimeWindow.hidden ? 1 : 0, shown ? 1 : 0]);
	if (!shown)
	{
		SDL_HideWindow(window);
		return false;
	}
	WindowIOSLogControllerState("runtime-window-ready", window);
#if LUMEN_APPLE_NATIVE_METAL
	LumenMetalSetPresentCompletionCallback(NativeMetalPresentCompleted, nullptr);
#endif
	return true;
}

void WindowIOSDidPresentFrame(uint64_t frame_number)
{
	// Presentation diagnostics remain available without an application HUD.
	if (frame_number == 1)
	{
		@autoreleasepool
		{
			EmitSDLWindowMarker(@"LUMEN_IOS_SDL_WINDOW phase=first-present ok=1");
		}
	}
}

void WindowIOSControllerDidConnect()
{
	WindowIOSLogControllerState("controller-connected", SDL_GetKeyboardFocus());
}

void WindowIOSRefreshVirtualControllerAvailability()
{
	// Legacy call site: report real SDL input availability. An embedding app
	// supplies any virtual controls through the public controller interface.
	EmitSDLWindowMarker([NSString stringWithFormat:
	    @"LUMEN_IOS_CONTROLLER_INPUT physical_open=%d virtual_ui=0",
	    HasOpenedPhysicalGameController() ? 1 : 0]);
}

bool WindowIOSReturnToDashboard(SDL_Window* window)
{
	// Legacy symbol name retained for Window.cpp. This core-only adapter hides
	// its SDL window; selecting or restoring an application view is host policy.
#if LUMEN_APPLE_NATIVE_METAL
	LumenMetalSetPresentCompletionCallback(nullptr, nullptr);
#endif
	if (window == nullptr || !NSThread.isMainThread)
	{
		EmitSDLWindowMarker(@"LUMEN_IOS_SDL_WINDOW phase=hide reason=invalid-window-or-thread ok=0");
		return false;
	}
	UIWindow* runtimeWindow = ResolveRuntimeWindow(window);
	if (runtimeWindow == nil)
	{
		EmitSDLWindowMarker(@"LUMEN_IOS_SDL_WINDOW phase=hide reason=no-uikit-window ok=0");
		return false;
	}
	gRuntimeWindowId = SDL_GetWindowID(window);
	SDL_HideWindow(window);
	const BOOL hidden = runtimeWindow.hidden && (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0;
	EmitSDLWindowMarker([NSString stringWithFormat:@"LUMEN_IOS_SDL_WINDOW phase=hide ok=%d", hidden ? 1 : 0]);
	return hidden;
}

bool WindowIOSFinalizeDashboardReturn()
{
	if (!NSThread.isMainThread)
	{
		EmitSDLWindowMarker(@"LUMEN_IOS_SDL_WINDOW phase=destroyed main=0 ok=0");
		return false;
	}
	const bool destroyed = gRuntimeWindowId != 0 && SDL_GetWindowFromID(gRuntimeWindowId) == nullptr;
	if (destroyed)
	{
		gRuntimeWindowId = 0;
	}
	EmitSDLWindowMarker([NSString stringWithFormat:@"LUMEN_IOS_SDL_WINDOW phase=destroyed main=1 ok=%d", destroyed ? 1 : 0]);
	return destroyed;
}

} // namespace Lumen::Libs::Graphics
