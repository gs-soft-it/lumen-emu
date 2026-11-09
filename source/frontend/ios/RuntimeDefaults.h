#ifndef LUMEN_FRONTEND_IOS_RUNTIME_DEFAULTS_H_
#define LUMEN_FRONTEND_IOS_RUNTIME_DEFAULTS_H_

// Standard logical launch size shared by the iOS bridge, HUD and Simulator
// previews. The physical drawable remains device-sized and independently
// scaled by the graphics backend.
enum
{
	LumenIOSStandardRuntimeWidth  = 1920,
	LumenIOSStandardRuntimeHeight = 1080,
};

#ifdef __OBJC__
#import <Foundation/Foundation.h>

// Shared by dashboard controls and the runtime overlay. Read the current
// preferences when each session starts, matching Android display defaults.
static NSString* const LumenIOSShowFPSPreferenceKey = @"lumen.display.showFps";
static NSString* const LumenIOSControllerOpacityPreferenceKey = @"lumen.display.controllerOpacity";

static inline BOOL LumenIOSShowFPS(void)
{
	NSUserDefaults* preferences = NSUserDefaults.standardUserDefaults;
	return [preferences objectForKey:LumenIOSShowFPSPreferenceKey] == nil ||
	       [preferences boolForKey:LumenIOSShowFPSPreferenceKey];
}

static inline NSInteger LumenIOSControllerOpacityPercent(void)
{
	NSUserDefaults* preferences = NSUserDefaults.standardUserDefaults;
	if ([preferences objectForKey:LumenIOSControllerOpacityPreferenceKey] == nil)
	{
		return 50;
	}
	return MAX(0, MIN(100, [preferences integerForKey:LumenIOSControllerOpacityPreferenceKey]));
}
#endif

#endif // LUMEN_FRONTEND_IOS_RUNTIME_DEFAULTS_H_
