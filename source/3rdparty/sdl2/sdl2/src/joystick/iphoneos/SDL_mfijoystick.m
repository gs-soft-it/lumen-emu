/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

/* This is the iOS implementation of the SDL joystick API */
#include "SDL_events.h"
#include "SDL_joystick.h"
#include "SDL_hints.h"
#include "SDL_stdinc.h"
#include "SDL_timer.h"
#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"
#include "../hidapi/SDL_hidapijoystick_c.h"
#include "../usb_ids.h"

#include "SDL_mfijoystick_c.h"

#if (TARGET_OS_IOS || defined(__MACOSX__)) && defined(SDL_JOYSTICK_MFI)
#include "SDL_mfidigitalqueue.h"
#import <dispatch/dispatch.h>
#endif

#if !SDL_EVENTS_DISABLED
#include "../../events/SDL_events_c.h"
#endif

#if TARGET_OS_IOS
#define SDL_JOYSTICK_iOS_ACCELEROMETER
#import <CoreMotion/CoreMotion.h>
#endif

#if defined(__MACOSX__)
#include <IOKit/hid/IOHIDManager.h>
#include <AppKit/NSApplication.h>
#ifndef NSAppKitVersionNumber10_15
#define NSAppKitVersionNumber10_15 1894
#endif
#endif /* __MACOSX__ */

#ifdef SDL_JOYSTICK_MFI
#import <GameController/GameController.h>

static id connectObserver = nil;
static id disconnectObserver = nil;
static NSString *GCInputXboxShareButton = @"Button Share";
#if TARGET_OS_IOS
#define SDL_MFI_RECONCILE_INTERVAL_MS 250
static Uint32 s_mfiLastReconcileTicks;
static SDL_bool s_mfiHasReconciled;
static SDL_atomic_t s_mfiReconcileRequested;
#endif

#include <Availability.h>
#include <objc/message.h>

/* remove compilation warnings for strict builds by defining these selectors, even though
 * they are only ever used indirectly through objc_msgSend
 */
@interface GCController (SDL)
#if defined(__MACOSX__) && (__MAC_OS_X_VERSION_MAX_ALLOWED <= 101600)
+ (BOOL)supportsHIDDevice:(IOHIDDeviceRef)device;
#endif
#if !((__IPHONE_OS_VERSION_MAX_ALLOWED >= 130000) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 130000) || (__MAC_OS_VERSION_MAX_ALLOWED >= 1500000))
@property(nonatomic, readonly) NSString *productCategory;
#endif
#if !((__IPHONE_OS_VERSION_MAX_ALLOWED >= 140500) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 140500) || (__MAC_OS_X_VERSION_MAX_ALLOWED >= 110300))
@property(class, nonatomic, readwrite) BOOL shouldMonitorBackgroundEvents;
#endif
@end
@interface GCExtendedGamepad (SDL)
#if !((__IPHONE_OS_VERSION_MAX_ALLOWED >= 121000) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 121000) || (__MAC_OS_VERSION_MAX_ALLOWED >= 1401000))
@property (nonatomic, readonly, nullable) GCControllerButtonInput *leftThumbstickButton;
@property (nonatomic, readonly, nullable) GCControllerButtonInput *rightThumbstickButton;
#endif
#if !((__IPHONE_OS_VERSION_MAX_ALLOWED >= 130000) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 130000) || (__MAC_OS_VERSION_MAX_ALLOWED >= 1500000))
@property (nonatomic, readonly) GCControllerButtonInput *buttonMenu;
@property (nonatomic, readonly, nullable) GCControllerButtonInput *buttonOptions;
#endif
#if !((__IPHONE_OS_VERSION_MAX_ALLOWED >= 140000) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 140000) || (__MAC_OS_VERSION_MAX_ALLOWED > 1500000))
@property (nonatomic, readonly, nullable) GCControllerButtonInput *buttonHome;
#endif
@end
@interface GCMicroGamepad (SDL)
#if !((__IPHONE_OS_VERSION_MAX_ALLOWED >= 130000) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 130000) || (__MAC_OS_VERSION_MAX_ALLOWED >= 1500000))
@property (nonatomic, readonly) GCControllerButtonInput *buttonMenu;
#endif
@end

#if (__IPHONE_OS_VERSION_MAX_ALLOWED >= 140000) || (__APPLETV_OS_VERSION_MAX_ALLOWED >= 140000) || (__MAC_OS_VERSION_MAX_ALLOWED > 1500000) || (__MAC_OS_X_VERSION_MAX_ALLOWED > 101600)
#define ENABLE_MFI_BATTERY
#define ENABLE_MFI_RUMBLE
#define ENABLE_MFI_LIGHT
#define ENABLE_MFI_SENSORS
#define ENABLE_MFI_SYSTEM_GESTURE_STATE
#define ENABLE_PHYSICAL_INPUT_PROFILE
#endif

#ifdef ENABLE_MFI_RUMBLE
#import <CoreHaptics/CoreHaptics.h>
#endif

#endif /* SDL_JOYSTICK_MFI */

#ifdef SDL_JOYSTICK_iOS_ACCELEROMETER
static const char *accelerometerName = "iOS Accelerometer";
static CMMotionManager *motionManager = nil;
#endif /* SDL_JOYSTICK_iOS_ACCELEROMETER */

static SDL_JoystickDeviceItem *deviceList = NULL;

static int numjoysticks = 0;
int SDL_AppleTVRemoteOpenedAsJoystick = 0;

static SDL_JoystickDeviceItem *
GetDeviceForIndex(int device_index)
{
    SDL_JoystickDeviceItem *device = deviceList;
    int i = 0;

    while (i < device_index) {
        if (device == NULL) {
            return NULL;
        }
        device = device->next;
        i++;
    }

    return device;
}

#ifdef SDL_JOYSTICK_MFI
static BOOL
IsControllerPS4(GCController *controller)
{
    if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *)) {
        if ([controller.productCategory isEqualToString:@"DualShock 4"]) {
            return TRUE;
        }
    } else {
        if ([controller.vendorName containsString: @"DUALSHOCK"]) {
            return TRUE;
        }
    }
    return FALSE;
}
static BOOL
IsControllerPS5(GCController *controller)
{
    if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *)) {
        if ([controller.productCategory isEqualToString:@"DualSense"]) {
            return TRUE;
        }
    } else {
        if ([controller.vendorName containsString: @"DualSense"]) {
            return TRUE;
        }
    }
    return FALSE;
}
static BOOL
IsControllerXbox(GCController *controller)
{
    if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *)) {
        if ([controller.productCategory isEqualToString:@"Xbox One"]) {
            return TRUE;
        }
    } else {
        if ([controller.vendorName containsString: @"Xbox"]) {
            return TRUE;
        }
    }
    return FALSE;
}
static BOOL
IsControllerSwitchPro(GCController *controller)
{
    if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *)) {
        if ([controller.productCategory isEqualToString:@"Switch Pro Controller"]) {
            return TRUE;
        }
    }
    return FALSE;
}
static BOOL
IsControllerSwitchJoyConL(GCController *controller)
{
    if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *)) {
        if ([controller.productCategory isEqualToString:@"Nintendo Switch Joy-Con (L)"]) {
            return TRUE;
        }
    }
    return FALSE;
}
static BOOL
IsControllerSwitchJoyConR(GCController *controller)
{
    if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *)) {
        if ([controller.productCategory isEqualToString:@"Nintendo Switch Joy-Con (R)"]) {
            return TRUE;
        }
    }
    return FALSE;
}
static BOOL
IsControllerSwitchJoyConPair(GCController *controller)
{
    if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *)) {
        if ([controller.productCategory isEqualToString:@"Nintendo Switch Joy-Con (L/R)"]) {
            return TRUE;
        }
    }
    return FALSE;
}
static BOOL
IOS_AddMFIJoystickDevice(SDL_JoystickDeviceItem *device, GCController *controller)
{
    Uint16 *guid16 = (Uint16 *)device->guid.data;
    Uint16 vendor = 0;
    Uint16 product = 0;
    Uint8 subtype = 0;
    const char *name = NULL;
    BOOL is_xbox = FALSE;
    BOOL is_ps4 = FALSE;
    BOOL is_ps5 = FALSE;
    BOOL is_switch_pro = FALSE;
    BOOL is_switch_joycon_pair = FALSE;

    if (@available(macOS 11.3, iOS 14.5, tvOS 14.5, *)) {
        if (!GCController.shouldMonitorBackgroundEvents) {
            GCController.shouldMonitorBackgroundEvents = YES;
        }
    }

    if (controller.vendorName) {
        name = controller.vendorName.UTF8String;
    }

    if (!name) {
        name = "MFi Gamepad";
    }

    if (controller.extendedGamepad) {
        is_xbox = IsControllerXbox(controller);
        is_ps4 = IsControllerPS4(controller);
        is_ps5 = IsControllerPS5(controller);
        is_switch_pro = IsControllerSwitchPro(controller);
        is_switch_joycon_pair = IsControllerSwitchJoyConPair(controller);
#ifdef SDL_JOYSTICK_HIDAPI
        if ((is_xbox && HIDAPI_IsDeviceTypePresent(SDL_CONTROLLER_TYPE_XBOXONE)) ||
            (is_ps4 && HIDAPI_IsDeviceTypePresent(SDL_CONTROLLER_TYPE_PS4)) ||
            (is_ps5 && HIDAPI_IsDeviceTypePresent(SDL_CONTROLLER_TYPE_PS5)) ||
            (is_switch_pro && HIDAPI_IsDeviceTypePresent(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO))) {
            /* The HIDAPI driver is taking care of this device. */
            return FALSE;
        }
#endif
    }

    /* Do not retain or allocate per-device state until the HIDAPI ownership
     * check above accepts this controller. Detect() retries rejected devices,
     * so doing either before that check would turn a one-time leak into an
     * unbounded one. */
    device->controller = (__bridge GCController *) CFBridgingRetain(controller);
    device->name = SDL_CreateJoystickName(0, 0, NULL, name);

#ifdef DEBUG_CONTROLLER_PROFILE
    if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
        if (controller.physicalInputProfile) {
            for (id key in controller.physicalInputProfile.buttons) {
                NSLog(@"Button %@ available\n", key);
            }
            for (id key in controller.physicalInputProfile.axes) {
                NSLog(@"Axis %@ available\n", key);
            }
        }
    }
#endif

    if (controller.extendedGamepad) {
        GCExtendedGamepad *gamepad = controller.extendedGamepad;
#if TARGET_OS_TV
        BOOL is_MFi = (!is_xbox && !is_ps4 && !is_ps5 && !is_switch_pro && !is_switch_joycon_pair);
#endif
        int nbuttons = 0;
        BOOL has_direct_menu;

        /* These buttons are part of the original MFi spec */
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_A);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_B);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_X);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_Y);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        nbuttons += 6;

        /* These buttons are available on some newer controllers */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
        if ([gamepad respondsToSelector:@selector(leftThumbstickButton)] && gamepad.leftThumbstickButton) {
            device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_LEFTSTICK);
            ++nbuttons;
        }
        if ([gamepad respondsToSelector:@selector(rightThumbstickButton)] && gamepad.rightThumbstickButton) {
            device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_RIGHTSTICK);
            ++nbuttons;
        }
        if ([gamepad respondsToSelector:@selector(buttonOptions)] && gamepad.buttonOptions) {
            device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_BACK);
            ++nbuttons;
        }
        /* The Nintendo Switch JoyCon home button doesn't ever show as being held down */
        if ([gamepad respondsToSelector:@selector(buttonHome)] && gamepad.buttonHome && !is_switch_joycon_pair) {
            device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_GUIDE);
            ++nbuttons;
        }
        has_direct_menu = [gamepad respondsToSelector:@selector(buttonMenu)] && gamepad.buttonMenu;
#if TARGET_OS_TV
        /* On tvOS MFi controller menu button brings you to the home screen */
        if (is_MFi) {
            has_direct_menu = FALSE;
        }
#endif
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_START);
        ++nbuttons;
        if (!has_direct_menu) {
            device->uses_pause_handler = SDL_TRUE;
        }

#ifdef ENABLE_PHYSICAL_INPUT_PROFILE
        if ([controller respondsToSelector:@selector(physicalInputProfile)]) {
            if (controller.physicalInputProfile.buttons[GCInputDualShockTouchpadButton] != nil) {
                device->has_dualshock_touchpad = SDL_TRUE;
                device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_MISC1);
                ++nbuttons;
            }
            if (controller.physicalInputProfile.buttons[GCInputXboxPaddleOne] != nil) {
                device->has_xbox_paddles = SDL_TRUE;
                device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_PADDLE1);
                ++nbuttons;
            }
            if (controller.physicalInputProfile.buttons[GCInputXboxPaddleTwo] != nil) {
                device->has_xbox_paddles = SDL_TRUE;
                device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_PADDLE2);
                ++nbuttons;
            }
            if (controller.physicalInputProfile.buttons[GCInputXboxPaddleThree] != nil) {
                device->has_xbox_paddles = SDL_TRUE;
                device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_PADDLE3);
                ++nbuttons;
            }
            if (controller.physicalInputProfile.buttons[GCInputXboxPaddleFour] != nil) {
                device->has_xbox_paddles = SDL_TRUE;
                device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_PADDLE4);
                ++nbuttons;
            }
            if (controller.physicalInputProfile.buttons[GCInputXboxShareButton] != nil) {
                device->has_xbox_share_button = SDL_TRUE;
                device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_MISC1);
                ++nbuttons;
            }
        }
#endif
#pragma clang diagnostic pop

        if (is_xbox) {
            vendor = USB_VENDOR_MICROSOFT;
            if (device->has_xbox_paddles) {
                /* Assume Xbox One Elite Series 2 Controller unless/until GCController flows VID/PID */
                product = USB_PRODUCT_XBOX_ONE_ELITE_SERIES_2_BLUETOOTH;
                subtype = 1;
            } else if (device->has_xbox_share_button) {
                /* Assume Xbox Series X Controller unless/until GCController flows VID/PID */
                product = USB_PRODUCT_XBOX_SERIES_X_BLE;
                subtype = 1;
            } else {
                /* Assume Xbox One S Bluetooth Controller unless/until GCController flows VID/PID */
                product = USB_PRODUCT_XBOX_ONE_S_REV1_BLUETOOTH;
                subtype = 0;
            }
        } else if (is_ps4) {
            /* Assume DS4 Slim unless/until GCController flows VID/PID */
            vendor = USB_VENDOR_SONY;
            product = USB_PRODUCT_SONY_DS4_SLIM;
            if (device->has_dualshock_touchpad) {
                subtype = 1;
            } else {
                subtype = 0;
            }
        } else if (is_ps5) {
            vendor = USB_VENDOR_SONY;
            product = USB_PRODUCT_SONY_DS5;
            subtype = 0;
        } else if (is_switch_pro) {
            vendor = USB_VENDOR_NINTENDO;
            product = USB_PRODUCT_NINTENDO_SWITCH_PRO;
            subtype = 0;
        } else if (is_switch_joycon_pair) {
            vendor = USB_VENDOR_NINTENDO;
            product = USB_PRODUCT_NINTENDO_SWITCH_JOYCON_PAIR;
            subtype = 0;
        } else {
            vendor = USB_VENDOR_APPLE;
            product = 1;
            subtype = 1;
        }

        if (SDL_strcmp(name, "Backbone One") == 0) {
            /* The Backbone app uses share button */
            if ((device->button_mask & (1 << SDL_CONTROLLER_BUTTON_MISC1)) != 0) {
                device->button_mask &= ~(1 << SDL_CONTROLLER_BUTTON_MISC1);
                --nbuttons;
                device->has_xbox_share_button = SDL_FALSE;
            }
        }

        device->naxes = 6; /* 2 thumbsticks and 2 triggers */
        device->nhats = 1; /* d-pad */
        device->nbuttons = nbuttons;

    } else if (controller.gamepad) {
        BOOL is_switch_joyconL = IsControllerSwitchJoyConL(controller);
        BOOL is_switch_joyconR = IsControllerSwitchJoyConR(controller);
        int nbuttons = 0;

        if (is_switch_joyconL) {
            vendor = USB_VENDOR_NINTENDO;
            product = USB_PRODUCT_NINTENDO_SWITCH_JOYCON_LEFT;
            subtype = 0;
        } else if (is_switch_joyconR) {
            vendor = USB_VENDOR_NINTENDO;
            product = USB_PRODUCT_NINTENDO_SWITCH_JOYCON_RIGHT;
            subtype = 0;
        } else {
            vendor = USB_VENDOR_APPLE;
            product = 2;
            subtype = 2;
        }

        /* These buttons are part of the original MFi spec */
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_A);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_B);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_X);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_Y);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_START);
        nbuttons += 7;
        device->uses_pause_handler = SDL_TRUE;

        device->naxes = 0; /* no traditional analog inputs */
        device->nhats = 1; /* d-pad */
        device->nbuttons = nbuttons;
    }
#if TARGET_OS_TV
    else if (controller.microGamepad) {
        int nbuttons = 0;

        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_A);
        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_B); /* Button X on microGamepad */
        nbuttons += 2;

        device->button_mask |= (1 << SDL_CONTROLLER_BUTTON_START);
        ++nbuttons;
        device->uses_pause_handler = SDL_TRUE;

        vendor = USB_VENDOR_APPLE;
        product = 3;
        subtype = 3;
        device->naxes = 2; /* treat the touch surface as two axes */
        device->nhats = 0; /* apparently the touch surface-as-dpad is buggy */
        device->nbuttons = nbuttons;

        controller.microGamepad.allowsRotation = SDL_GetHintBoolean(SDL_HINT_APPLE_TV_REMOTE_ALLOW_ROTATION, SDL_FALSE);
    }
#endif /* TARGET_OS_TV */

    /* We only need 16 bits for each of these; space them out to fill 128. */
    /* Byteswap so devices get same GUID on little/big endian platforms. */
    *guid16++ = SDL_SwapLE16(SDL_HARDWARE_BUS_BLUETOOTH);
    *guid16++ = 0;
    *guid16++ = SDL_SwapLE16(vendor);
    *guid16++ = 0;
    *guid16++ = SDL_SwapLE16(product);
    *guid16++ = 0;

    *guid16++ = SDL_SwapLE16(device->button_mask);

    if (vendor == USB_VENDOR_APPLE) {
        /* Note that this is an MFI controller and what subtype it is */
        device->guid.data[14] = 'm';
        device->guid.data[15] = subtype;
    } else {
        device->guid.data[15] = subtype;
    }

    /* This will be set when the first button press of the controller is
     * detected. */
    controller.playerIndex = -1;
    return TRUE;
}

#if TARGET_OS_IOS || defined(__MACOSX__)
/* Lumen: preserve complete digital taps between event pumps on macOS too.
 * Device discovery, mappings and analog polling keep their existing paths. */
@interface SDL_MFIDigitalInputContext : NSObject
{
@public
    SDL_MFIDigitalQueue queue;
    Uint8 dpad_state;
    SDL_bool logged_overflow;
}
@property(nonatomic, strong) dispatch_queue_t handlerQueue;
@property(nonatomic, strong) dispatch_queue_t previousHandlerQueue;
@end

@implementation SDL_MFIDigitalInputContext
@end

static char s_mfiInputQueueKey;

static Uint8
IOS_MFIDigitalHatState(Uint8 state)
{
    Uint8 hat = SDL_HAT_CENTERED;

    if (state & SDL_HAT_UP) {
        hat |= SDL_HAT_UP;
    } else if (state & SDL_HAT_DOWN) {
        hat |= SDL_HAT_DOWN;
    }
    if (state & SDL_HAT_LEFT) {
        hat |= SDL_HAT_LEFT;
    } else if (state & SDL_HAT_RIGHT) {
        hat |= SDL_HAT_RIGHT;
    }
    return hat;
}

static Uint8
IOS_MFIDigitalCurrentDPadState(GCControllerDirectionPad *dpad)
{
    Uint8 state = 0;

    if (dpad.up.isPressed) {
        state |= SDL_HAT_UP;
    }
    if (dpad.down.isPressed) {
        state |= SDL_HAT_DOWN;
    }
    if (dpad.left.isPressed) {
        state |= SDL_HAT_LEFT;
    }
    if (dpad.right.isPressed) {
        state |= SDL_HAT_RIGHT;
    }
    return state;
}

static void
IOS_MFISetButtonHandler(GCControllerButtonInput *button, SDL_MFIDigitalInputContext *context, int button_index)
{
    if (button == nil) {
        return;
    }

    button.pressedChangedHandler = ^(GCControllerButtonInput *input, float value, BOOL pressed) {
        SDL_MFIDigitalQueueEnqueue(&context->queue, SDL_MFI_DIGITAL_EVENT_BUTTON,
                                   (Uint8)button_index, pressed ? SDL_PRESSED : SDL_RELEASED);
    };
}

static void
IOS_MFISetDPadButtonHandler(GCControllerButtonInput *button, SDL_MFIDigitalInputContext *context, Uint8 direction)
{
    if (button == nil) {
        return;
    }

    button.pressedChangedHandler = ^(GCControllerButtonInput *input, float value, BOOL pressed) {
        if (pressed) {
            context->dpad_state |= direction;
        } else {
            context->dpad_state &= ~direction;
        }
        SDL_MFIDigitalQueueEnqueue(&context->queue, SDL_MFI_DIGITAL_EVENT_HAT, 0,
                                   IOS_MFIDigitalHatState(context->dpad_state));
    };
}

static void
IOS_MFISetDPadHandlers(GCControllerDirectionPad *dpad, SDL_MFIDigitalInputContext *context)
{
    IOS_MFISetDPadButtonHandler(dpad.up, context, SDL_HAT_UP);
    IOS_MFISetDPadButtonHandler(dpad.down, context, SDL_HAT_DOWN);
    IOS_MFISetDPadButtonHandler(dpad.left, context, SDL_HAT_LEFT);
    IOS_MFISetDPadButtonHandler(dpad.right, context, SDL_HAT_RIGHT);
}

static void
IOS_MFIActivateDigitalInput(SDL_MFIDigitalInputContext *context, GCControllerDirectionPad *dpad)
{
    void (^activate)(void) = ^{
        /* Handler installation is intentionally fail-closed. Any callback
         * submitted before this barrier observes accepting == SDL_FALSE.
         * Re-snapshot the d-pad on its serial delivery queue, then start with
         * an empty FIFO so the first normal poll is the authoritative initial
         * state and later callbacks preserve complete transitions. */
        context->dpad_state = IOS_MFIDigitalCurrentDPadState(dpad);
        SDL_MFIDigitalQueueReset(&context->queue);
    };

    if (dispatch_get_specific(&s_mfiInputQueueKey) == (__bridge void *)context) {
        activate();
    } else {
        dispatch_sync(context.handlerQueue, activate);
    }
}

static void
IOS_MFIClearDPadHandlers(GCControllerDirectionPad *dpad)
{
    dpad.up.pressedChangedHandler = nil;
    dpad.down.pressedChangedHandler = nil;
    dpad.left.pressedChangedHandler = nil;
    dpad.right.pressedChangedHandler = nil;
}

static void
IOS_MFIEnqueuePause(SDL_MFIDigitalInputContext *context, int button_index)
{
    SDL_MFIDigitalQueueEnqueue(&context->queue, SDL_MFI_DIGITAL_EVENT_BUTTON,
                               (Uint8)button_index, SDL_PRESSED);
    SDL_MFIDigitalQueueEnqueue(&context->queue, SDL_MFI_DIGITAL_EVENT_BUTTON,
                               (Uint8)button_index, SDL_RELEASED);
}

static void
IOS_MFISetPauseHandler(GCController *controller, SDL_MFIDigitalInputContext *context, int button_index)
{
    controller.controllerPausedHandler = ^(GCController *c) {
        if (dispatch_get_specific(&s_mfiInputQueueKey) == (__bridge void *)context) {
            IOS_MFIEnqueuePause(context, button_index);
        } else {
            dispatch_async(context.handlerQueue, ^{
                IOS_MFIEnqueuePause(context, button_index);
            });
        }
    };
}

static SDL_bool
IOS_MFIInstallDigitalInput(SDL_JoystickDeviceItem *device, GCController *controller)
{
    SDL_MFIDigitalInputContext *context;
    int button_index = 0;

    context = [[SDL_MFIDigitalInputContext alloc] init];
    if (context == nil) {
        return SDL_FALSE;
    }

    SDL_MFIDigitalQueueReset(&context->queue);
    SDL_MFIDigitalQueueStop(&context->queue);
    context.handlerQueue = dispatch_queue_create("org.libsdl.mfi-digital-input", DISPATCH_QUEUE_SERIAL);
    if (context.handlerQueue == nil) {
        return SDL_FALSE;
    }
    context.previousHandlerQueue = controller.handlerQueue;
    dispatch_queue_set_specific(context.handlerQueue, &s_mfiInputQueueKey, (__bridge void *)context, NULL);
    controller.handlerQueue = context.handlerQueue;

    if (controller.extendedGamepad) {
        GCExtendedGamepad *gamepad = controller.extendedGamepad;

        IOS_MFISetButtonHandler(gamepad.buttonA, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.buttonB, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.buttonX, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.buttonY, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.leftShoulder, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.rightShoulder, context, button_index++);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
        if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_LEFTSTICK)) {
            IOS_MFISetButtonHandler(gamepad.leftThumbstickButton, context, button_index++);
        }
        if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
            IOS_MFISetButtonHandler(gamepad.rightThumbstickButton, context, button_index++);
        }
        if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_BACK)) {
            IOS_MFISetButtonHandler(gamepad.buttonOptions, context, button_index++);
        }
        if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_GUIDE)) {
            IOS_MFISetButtonHandler(gamepad.buttonHome, context, button_index++);
        }
        if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_START)) {
            if (device->uses_pause_handler) {
                IOS_MFISetPauseHandler(controller, context, button_index++);
            } else {
                IOS_MFISetButtonHandler(gamepad.buttonMenu, context, button_index++);
            }
        }
#pragma clang diagnostic pop

#ifdef ENABLE_PHYSICAL_INPUT_PROFILE
        if (device->has_dualshock_touchpad) {
            IOS_MFISetButtonHandler(controller.physicalInputProfile.buttons[GCInputDualShockTouchpadButton],
                                    context, button_index++);
        }
        if (device->has_xbox_paddles) {
            if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_PADDLE1)) {
                IOS_MFISetButtonHandler(controller.physicalInputProfile.buttons[GCInputXboxPaddleOne],
                                        context, button_index++);
            }
            if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_PADDLE2)) {
                IOS_MFISetButtonHandler(controller.physicalInputProfile.buttons[GCInputXboxPaddleTwo],
                                        context, button_index++);
            }
            if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_PADDLE3)) {
                IOS_MFISetButtonHandler(controller.physicalInputProfile.buttons[GCInputXboxPaddleThree],
                                        context, button_index++);
            }
            if (device->button_mask & (1 << SDL_CONTROLLER_BUTTON_PADDLE4)) {
                IOS_MFISetButtonHandler(controller.physicalInputProfile.buttons[GCInputXboxPaddleFour],
                                        context, button_index++);
            }
        }
        if (device->has_xbox_share_button) {
            IOS_MFISetButtonHandler(controller.physicalInputProfile.buttons[GCInputXboxShareButton],
                                    context, button_index++);
        }
#endif

        IOS_MFISetDPadHandlers(gamepad.dpad, context);
        IOS_MFIActivateDigitalInput(context, gamepad.dpad);
    } else if (controller.gamepad) {
        GCGamepad *gamepad = controller.gamepad;

        IOS_MFISetButtonHandler(gamepad.buttonA, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.buttonB, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.buttonX, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.buttonY, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.leftShoulder, context, button_index++);
        IOS_MFISetButtonHandler(gamepad.rightShoulder, context, button_index++);

        IOS_MFISetPauseHandler(controller, context, button_index++);
        IOS_MFISetDPadHandlers(gamepad.dpad, context);
        IOS_MFIActivateDigitalInput(context, gamepad.dpad);
    } else {
        controller.handlerQueue = context.previousHandlerQueue;
        dispatch_queue_set_specific(context.handlerQueue, &s_mfiInputQueueKey, NULL, NULL);
        SDL_MFIDigitalQueueStop(&context->queue);
        return SDL_FALSE;
    }

    device->mfi_input_context = (__bridge_retained void *)context;
    {
        static unsigned int trace_records;
        const char *trace = SDL_getenv("LUMEN_PAD_OPTIONS_TRACE");
        if (trace_records < 4 && trace && trace[0] == '1' && trace[1] == '\0') {
            ++trace_records;
            fprintf(stderr, "[MFI_DIGITAL_INPUT] phase=installed instance=%d callback_queue=1 name=\"%s\"\n",
                    (int)device->instance_id, device->name ? device->name : "unknown");
        }
    }
    return SDL_TRUE;
}

static void
IOS_MFIDisarmDigitalInput(SDL_JoystickDeviceItem *device)
{
    SDL_MFIDigitalInputContext *context;
    GCController *controller;
    void *retained_context;

    if (device == NULL || device->mfi_input_context == NULL) {
        return;
    }

    context = (__bridge SDL_MFIDigitalInputContext *)device->mfi_input_context;
    controller = device->controller;
    SDL_MFIDigitalQueueStop(&context->queue);

    if (controller.extendedGamepad) {
        GCExtendedGamepad *gamepad = controller.extendedGamepad;

        gamepad.buttonA.pressedChangedHandler = nil;
        gamepad.buttonB.pressedChangedHandler = nil;
        gamepad.buttonX.pressedChangedHandler = nil;
        gamepad.buttonY.pressedChangedHandler = nil;
        gamepad.leftShoulder.pressedChangedHandler = nil;
        gamepad.rightShoulder.pressedChangedHandler = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
        gamepad.leftThumbstickButton.pressedChangedHandler = nil;
        gamepad.rightThumbstickButton.pressedChangedHandler = nil;
        gamepad.buttonOptions.pressedChangedHandler = nil;
        gamepad.buttonHome.pressedChangedHandler = nil;
        gamepad.buttonMenu.pressedChangedHandler = nil;
#pragma clang diagnostic pop
#ifdef ENABLE_PHYSICAL_INPUT_PROFILE
        if (device->has_dualshock_touchpad) {
            controller.physicalInputProfile.buttons[GCInputDualShockTouchpadButton].pressedChangedHandler = nil;
        }
        if (device->has_xbox_paddles) {
            controller.physicalInputProfile.buttons[GCInputXboxPaddleOne].pressedChangedHandler = nil;
            controller.physicalInputProfile.buttons[GCInputXboxPaddleTwo].pressedChangedHandler = nil;
            controller.physicalInputProfile.buttons[GCInputXboxPaddleThree].pressedChangedHandler = nil;
            controller.physicalInputProfile.buttons[GCInputXboxPaddleFour].pressedChangedHandler = nil;
        }
        if (device->has_xbox_share_button) {
            controller.physicalInputProfile.buttons[GCInputXboxShareButton].pressedChangedHandler = nil;
        }
#endif
        IOS_MFIClearDPadHandlers(gamepad.dpad);
    } else if (controller.gamepad) {
        GCGamepad *gamepad = controller.gamepad;

        gamepad.buttonA.pressedChangedHandler = nil;
        gamepad.buttonB.pressedChangedHandler = nil;
        gamepad.buttonX.pressedChangedHandler = nil;
        gamepad.buttonY.pressedChangedHandler = nil;
        gamepad.leftShoulder.pressedChangedHandler = nil;
        gamepad.rightShoulder.pressedChangedHandler = nil;
        IOS_MFIClearDPadHandlers(gamepad.dpad);
    }
    controller.controllerPausedHandler = nil;

    if (dispatch_get_specific(&s_mfiInputQueueKey) != (__bridge void *)context) {
        dispatch_sync(context.handlerQueue, ^{});
    }
    controller.handlerQueue = context.previousHandlerQueue;
    dispatch_queue_set_specific(context.handlerQueue, &s_mfiInputQueueKey, NULL, NULL);

    retained_context = device->mfi_input_context;
    device->mfi_input_context = NULL;
    __unused SDL_MFIDigitalInputContext *released_context =
        (__bridge_transfer SDL_MFIDigitalInputContext *)retained_context;
}
#endif /* TARGET_OS_IOS || __MACOSX__ */
#endif /* SDL_JOYSTICK_MFI */

#if defined(SDL_JOYSTICK_iOS_ACCELEROMETER) || defined(SDL_JOYSTICK_MFI)
static void
IOS_AddJoystickDevice(GCController *controller, SDL_bool accelerometer)
{
    SDL_JoystickDeviceItem *device = deviceList;

#if TARGET_OS_TV
    if (!SDL_GetHintBoolean(SDL_HINT_TV_REMOTE_AS_JOYSTICK, SDL_TRUE)) {
        /* Ignore devices that aren't actually controllers (e.g. remotes), they'll be handled as keyboard input */
        if (controller && !controller.extendedGamepad && !controller.gamepad && controller.microGamepad) {
            return;
        }
    }
#endif

    while (device != NULL) {
        if (device->controller == controller) {
            return;
        }
        device = device->next;
    }

    device = (SDL_JoystickDeviceItem *) SDL_calloc(1, sizeof(SDL_JoystickDeviceItem));
    if (device == NULL) {
        return;
    }

    device->accelerometer = accelerometer;
    device->instance_id = SDL_GetNextJoystickInstanceID();

    if (accelerometer) {
#ifdef SDL_JOYSTICK_iOS_ACCELEROMETER
        device->name = SDL_strdup(accelerometerName);
        device->naxes = 3; /* Device acceleration in the x, y, and z axes. */
        device->nhats = 0;
        device->nbuttons = 0;

        /* Use the accelerometer name as a GUID. */
        SDL_memcpy(&device->guid.data, device->name, SDL_min(sizeof(SDL_JoystickGUID), SDL_strlen(device->name)));
#else
        SDL_free(device);
        return;
#endif /* SDL_JOYSTICK_iOS_ACCELEROMETER */
    } else if (controller) {
#ifdef SDL_JOYSTICK_MFI
        if (!IOS_AddMFIJoystickDevice(device, controller)) {
            SDL_free(device);
            return;
        }
#else
        SDL_free(device);
        return;
#endif /* SDL_JOYSTICK_MFI */
    }

    if (deviceList == NULL) {
        deviceList = device;
    } else {
        SDL_JoystickDeviceItem *lastdevice = deviceList;
        while (lastdevice->next != NULL) {
            lastdevice = lastdevice->next;
        }
        lastdevice->next = device;
    }

    ++numjoysticks;

    SDL_PrivateJoystickAdded(device->instance_id);
}
#endif /* SDL_JOYSTICK_iOS_ACCELEROMETER || SDL_JOYSTICK_MFI */

static SDL_JoystickDeviceItem *
IOS_RemoveJoystickDevice(SDL_JoystickDeviceItem *device)
{
    SDL_JoystickDeviceItem *prev = NULL;
    SDL_JoystickDeviceItem *next = NULL;
    SDL_JoystickDeviceItem *item = deviceList;

    if (device == NULL) {
        return NULL;
    }

    next = device->next;

    while (item != NULL) {
        if (item == device) {
            break;
        }
        prev = item;
        item = item->next;
    }

    /* Unlink the device item from the device list. */
    if (prev) {
        prev->next = device->next;
    } else if (device == deviceList) {
        deviceList = device->next;
    }

#if (TARGET_OS_IOS || defined(__MACOSX__)) && defined(SDL_JOYSTICK_MFI)
    IOS_MFIDisarmDigitalInput(device);
#endif

    if (device->joystick) {
        device->joystick->hwdata = NULL;
    }

#ifdef SDL_JOYSTICK_MFI
    @autoreleasepool {
        if (device->controller) {
            /* The controller was explicitly retained in the struct, so it
             * should be explicitly released before freeing the struct. */
            GCController *controller = CFBridgingRelease((__bridge CFTypeRef)(device->controller));
            controller.controllerPausedHandler = nil;
            device->controller = nil;
        }
    }
#endif /* SDL_JOYSTICK_MFI */

    --numjoysticks;

    SDL_PrivateJoystickRemoved(device->instance_id);

    SDL_free(device->name);
    SDL_free(device);

    return next;
}

#if TARGET_OS_IOS && defined(SDL_JOYSTICK_MFI)
static void
IOS_MFIReconcileControllerSnapshot(void)
{
    @autoreleasepool {
        NSArray<GCController *> *controllers;
        SDL_JoystickDeviceItem *device;

        /* Detect() is the only caller and SDL_JoystickUpdate already owns this
         * recursive mutex. Keep the GameController snapshot and its complete
         * remove/add transaction under that ownership so snapshots cannot be
         * applied out of order. Notification callbacks only set an atomic
         * request flag and never touch the SDL device list. */
        SDL_LockJoysticks();
        controllers = [GCController controllers];
        device = deviceList;
        while (device != NULL) {
            SDL_JoystickDeviceItem *next = device->next;
            if (device->controller != nil &&
                [controllers indexOfObjectIdenticalTo:device->controller] == NSNotFound) {
                IOS_RemoveJoystickDevice(device);
            }
            device = next;
        }
        /* Publish REMOVED(old) before ADDED(new) when a controller is replaced
         * within one reconciliation pass. This keeps device-index changes and
         * instance-ID event ordering deterministic across reconnects. */
        for (GCController *controller in controllers) {
            IOS_AddJoystickDevice(controller, SDL_FALSE);
        }
        SDL_UnlockJoysticks();
    }
}
#endif

#if TARGET_OS_TV
static void SDLCALL
SDL_AppleTVRemoteRotationHintChanged(void *udata, const char *name, const char *oldValue, const char *newValue)
{
    BOOL allowRotation = newValue != NULL && *newValue != '0';

    @autoreleasepool {
        for (GCController *controller in [GCController controllers]) {
            if (controller.microGamepad) {
                controller.microGamepad.allowsRotation = allowRotation;
            }
        }
    }
}
#endif /* TARGET_OS_TV */

#if defined(__MACOSX__)
static int is_macos11(void)
{
    return (floor(NSAppKitVersionNumber) > NSAppKitVersionNumber10_15);
}
#endif

static int
IOS_JoystickInit(void)
{
#if defined(__MACOSX__)
    if (!is_macos11()) {
        return 0;
    }
#endif

    @autoreleasepool {
#if TARGET_OS_IOS && defined(SDL_JOYSTICK_MFI)
        s_mfiLastReconcileTicks = 0;
        s_mfiHasReconciled = SDL_FALSE;
        SDL_AtomicSet(&s_mfiReconcileRequested, 1);
#endif
#ifdef SDL_JOYSTICK_MFI
        NSNotificationCenter *center;
#endif
#ifdef SDL_JOYSTICK_iOS_ACCELEROMETER
        if (SDL_GetHintBoolean(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, SDL_TRUE)) {
            /* Default behavior, accelerometer as joystick */
            IOS_AddJoystickDevice(nil, SDL_TRUE);
        }
#endif

#ifdef SDL_JOYSTICK_MFI
        /* GameController.framework was added in iOS 7. */
        if (![GCController class]) {
            return 0;
        }

        /* For whatever reason, this always returns an empty array on
         macOS 11.0.1 */
        for (GCController *controller in [GCController controllers]) {
            IOS_AddJoystickDevice(controller, SDL_FALSE);
        }

#if TARGET_OS_TV
        SDL_AddHintCallback(SDL_HINT_APPLE_TV_REMOTE_ALLOW_ROTATION,
                            SDL_AppleTVRemoteRotationHintChanged, NULL);
#endif /* TARGET_OS_TV */

        center = [NSNotificationCenter defaultCenter];

        connectObserver = [center addObserverForName:GCControllerDidConnectNotification
                                              object:nil
                                               queue:nil
                                          usingBlock:^(NSNotification *note) {
#if TARGET_OS_IOS
                                              (void)note;
                                              SDL_AtomicSet(&s_mfiReconcileRequested, 1);
#else
                                              GCController *controller = note.object;
                                              IOS_AddJoystickDevice(controller, SDL_FALSE);
#endif
                                          }];

        disconnectObserver = [center addObserverForName:GCControllerDidDisconnectNotification
                                                 object:nil
                                                  queue:nil
                                             usingBlock:^(NSNotification *note) {
#if TARGET_OS_IOS
                                                 (void)note;
                                                 SDL_AtomicSet(&s_mfiReconcileRequested, 1);
#else
                                                 GCController *controller = note.object;
                                                 SDL_JoystickDeviceItem *device = deviceList;
                                                 while (device != NULL) {
                                                     if (device->controller == controller) {
                                                         IOS_RemoveJoystickDevice(device);
                                                         break;
                                                     }
                                                     device = device->next;
                                                 }
#endif
                                             }];
#endif /* SDL_JOYSTICK_MFI */
    }

    return 0;
}

static int
IOS_JoystickGetCount(void)
{
    return numjoysticks;
}

static void
IOS_JoystickDetect(void)
{
#if TARGET_OS_IOS && defined(SDL_JOYSTICK_MFI)
    const Uint32 now = SDL_GetTicks();
    const SDL_bool requested = SDL_AtomicGet(&s_mfiReconcileRequested) ? SDL_TRUE : SDL_FALSE;

    /* GCController notifications need the UIKit main thread, which the guest
     * runtime owns. They only request an early pass; all list mutations remain
     * on SDL's runtime-thread Detect() path. The periodic pass is authoritative
     * when UIKit cannot deliver a notification while the guest is running. */
    if (requested || !s_mfiHasReconciled ||
        SDL_TICKS_PASSED(now, s_mfiLastReconcileTicks + SDL_MFI_RECONCILE_INTERVAL_MS)) {
        SDL_AtomicSet(&s_mfiReconcileRequested, 0);
        s_mfiLastReconcileTicks = now;
        s_mfiHasReconciled = SDL_TRUE;
        IOS_MFIReconcileControllerSnapshot();
    }
#endif
}

static const char *
IOS_JoystickGetDeviceName(int device_index)
{
    SDL_JoystickDeviceItem *device = GetDeviceForIndex(device_index);
    return device ? device->name : "Unknown";
}

static const char *
IOS_JoystickGetDevicePath(int device_index)
{
    return NULL;
}

static int
IOS_JoystickGetDevicePlayerIndex(int device_index)
{
#ifdef SDL_JOYSTICK_MFI
    SDL_JoystickDeviceItem *device = GetDeviceForIndex(device_index);
    if (device && device->controller) {
        return (int)device->controller.playerIndex;
    }
#endif
    return -1;
}

static void
IOS_JoystickSetDevicePlayerIndex(int device_index, int player_index)
{
#ifdef SDL_JOYSTICK_MFI
    SDL_JoystickDeviceItem *device = GetDeviceForIndex(device_index);
    if (device && device->controller) {
        device->controller.playerIndex = player_index;
    }
#endif
}

static SDL_JoystickGUID
IOS_JoystickGetDeviceGUID(int device_index)
{
    SDL_JoystickDeviceItem *device = GetDeviceForIndex(device_index);
    SDL_JoystickGUID guid;
    if (device) {
        guid = device->guid;
    } else {
        SDL_zero(guid);
    }
    return guid;
}

static SDL_JoystickID
IOS_JoystickGetDeviceInstanceID(int device_index)
{
    SDL_JoystickDeviceItem *device = GetDeviceForIndex(device_index);
    return device ? device->instance_id : -1;
}

static int
IOS_JoystickOpen(SDL_Joystick *joystick, int device_index)
{
    SDL_JoystickDeviceItem *device = GetDeviceForIndex(device_index);
    if (device == NULL) {
        return SDL_SetError("Could not open Joystick: no hardware device for the specified index");
    }

    joystick->hwdata = device;
    joystick->instance_id = device->instance_id;

    joystick->naxes = device->naxes;
    joystick->nhats = device->nhats;
    joystick->nbuttons = device->nbuttons;
    joystick->nballs = 0;

    if (device->has_dualshock_touchpad) {
        SDL_PrivateJoystickAddTouchpad(joystick, 2);
    }

    device->joystick = joystick;

    @autoreleasepool {
        if (device->accelerometer) {
#ifdef SDL_JOYSTICK_iOS_ACCELEROMETER
            if (motionManager == nil) {
                motionManager = [[CMMotionManager alloc] init];
            }

            /* Shorter times between updates can significantly increase CPU usage. */
            motionManager.accelerometerUpdateInterval = 0.1;
            [motionManager startAccelerometerUpdates];
#endif
        } else {
#ifdef SDL_JOYSTICK_MFI
#if TARGET_OS_IOS || defined(__MACOSX__)
            {
                const SDL_bool installed = IOS_MFIInstallDigitalInput(device, device->controller);
                if (!installed && device->uses_pause_handler) {
#else
            if (device->uses_pause_handler) {
#endif
                GCController *controller = device->controller;
                controller.controllerPausedHandler = ^(GCController *c) {
                    if (joystick->hwdata) {
                        ++joystick->hwdata->num_pause_presses;
                    }
                };
#if TARGET_OS_IOS || defined(__MACOSX__)
                }
            }
#else
            }
#endif

#ifdef ENABLE_MFI_SENSORS
            if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
                GCController *controller = joystick->hwdata->controller;
                GCMotion *motion = controller.motion;
                if (motion && motion.hasRotationRate) {
                    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 0.0f);
                }
                if (motion && motion.hasGravityAndUserAcceleration) {
                    SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 0.0f);
                }
            }
#endif /* ENABLE_MFI_SENSORS */

#ifdef ENABLE_MFI_SYSTEM_GESTURE_STATE
            if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
                GCController *controller = joystick->hwdata->controller;
                for (id key in controller.physicalInputProfile.buttons) {
                    GCControllerButtonInput *button = controller.physicalInputProfile.buttons[key];
                    if ([button isBoundToSystemGesture]) {
                        button.preferredSystemGestureState = GCSystemGestureStateDisabled;
                    }
                }
            }
#endif /* ENABLE_MFI_SYSTEM_GESTURE_STATE */

#endif /* SDL_JOYSTICK_MFI */
        }
    }
    if (device->remote) {
        ++SDL_AppleTVRemoteOpenedAsJoystick;
    }

    return 0;
}

static void
IOS_AccelerometerUpdate(SDL_Joystick *joystick)
{
#ifdef SDL_JOYSTICK_iOS_ACCELEROMETER
    const float maxgforce = SDL_IPHONE_MAX_GFORCE;
    const SInt16 maxsint16 = 0x7FFF;
    CMAcceleration accel;

    @autoreleasepool {
        if (!motionManager.isAccelerometerActive) {
            return;
        }

        accel = motionManager.accelerometerData.acceleration;
    }

    /*
     Convert accelerometer data from floating point to Sint16, which is what
     the joystick system expects.

     To do the conversion, the data is first clamped onto the interval
     [-SDL_IPHONE_MAX_G_FORCE, SDL_IPHONE_MAX_G_FORCE], then the data is multiplied
     by MAX_SINT16 so that it is mapped to the full range of an Sint16.

     You can customize the clamped range of this function by modifying the
     SDL_IPHONE_MAX_GFORCE macro in SDL_config_iphoneos.h.

     Once converted to Sint16, the accelerometer data no longer has coherent
     units. You can convert the data back to units of g-force by multiplying
     it in your application's code by SDL_IPHONE_MAX_GFORCE / 0x7FFF.
     */

    /* clamp the data */
    accel.x = SDL_clamp(accel.x, -maxgforce, maxgforce);
    accel.y = SDL_clamp(accel.y, -maxgforce, maxgforce);
    accel.z = SDL_clamp(accel.z, -maxgforce, maxgforce);

    /* pass in data mapped to range of SInt16 */
    SDL_PrivateJoystickAxis(joystick, 0,  (accel.x / maxgforce) * maxsint16);
    SDL_PrivateJoystickAxis(joystick, 1, -(accel.y / maxgforce) * maxsint16);
    SDL_PrivateJoystickAxis(joystick, 2,  (accel.z / maxgforce) * maxsint16);
#endif /* SDL_JOYSTICK_iOS_ACCELEROMETER */
}

#ifdef SDL_JOYSTICK_MFI
static Uint8
IOS_MFIJoystickHatStateForDPad(GCControllerDirectionPad *dpad)
{
    Uint8 hat = 0;

    if (dpad.up.isPressed) {
        hat |= SDL_HAT_UP;
    } else if (dpad.down.isPressed) {
        hat |= SDL_HAT_DOWN;
    }

    if (dpad.left.isPressed) {
        hat |= SDL_HAT_LEFT;
    } else if (dpad.right.isPressed) {
        hat |= SDL_HAT_RIGHT;
    }

    if (hat == 0) {
        return SDL_HAT_CENTERED;
    }

    return hat;
}
#endif

static void
IOS_MFIJoystickUpdate(SDL_Joystick *joystick)
{
#if SDL_JOYSTICK_MFI
    @autoreleasepool {
        GCController *controller = joystick->hwdata->controller;
        Uint8 hatstate = SDL_HAT_CENTERED;
        int i;
        int pause_button_index = 0;

#if TARGET_OS_IOS || defined(__MACOSX__)
        if (joystick->hwdata->mfi_input_context != NULL) {
            SDL_MFIDigitalInputContext *context =
                (__bridge SDL_MFIDigitalInputContext *)joystick->hwdata->mfi_input_context;
            SDL_MFIDigitalEvent events[SDL_MFI_DIGITAL_QUEUE_CAPACITY];
            const int event_count = SDL_MFIDigitalQueueDrain(&context->queue, events, SDL_arraysize(events));

            for (i = 0; i < event_count; ++i) {
                const SDL_MFIDigitalEvent *event = &events[i];
                if (event->type == SDL_MFI_DIGITAL_EVENT_BUTTON) {
                    SDL_PrivateJoystickButton(joystick, event->index, event->value);
                } else if (event->type == SDL_MFI_DIGITAL_EVENT_HAT && joystick->nhats > 0) {
                    SDL_PrivateJoystickHat(joystick, event->index, event->value);
                }
            }
            if (SDL_MFIDigitalQueueTakeOverflow(&context->queue) && !context->logged_overflow) {
                context->logged_overflow = SDL_TRUE;
                SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                            "MFi digital input queue overflowed; polling current controller state");
            }
        }
#endif

#ifdef DEBUG_CONTROLLER_STATE
        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            if (controller.physicalInputProfile) {
                for (id key in controller.physicalInputProfile.buttons) {
                    GCControllerButtonInput *button = controller.physicalInputProfile.buttons[key];
                    if (button.isPressed)
                        NSLog(@"Button %@ = %s\n", key, button.isPressed ? "pressed" : "released");
                }
                for (id key in controller.physicalInputProfile.axes) {
                    GCControllerAxisInput *axis = controller.physicalInputProfile.axes[key];
                    if (axis.value != 0.0f)
                        NSLog(@"Axis %@ = %.2f\n", key, axis.value);
                }
            }
        }
#endif

        if (controller.extendedGamepad) {
            GCExtendedGamepad *gamepad = controller.extendedGamepad;

            /* Axis order matches the XInput Windows mappings. */
            Sint16 axes[] = {
                (Sint16) (gamepad.leftThumbstick.xAxis.value * 32767),
                (Sint16) (gamepad.leftThumbstick.yAxis.value * -32767),
                (Sint16) ((gamepad.leftTrigger.value * 65535) - 32768),
                (Sint16) (gamepad.rightThumbstick.xAxis.value * 32767),
                (Sint16) (gamepad.rightThumbstick.yAxis.value * -32767),
                (Sint16) ((gamepad.rightTrigger.value * 65535) - 32768),
            };

            /* Button order matches the XInput Windows mappings. */
            Uint8 buttons[joystick->nbuttons];
            int button_count = 0;

            /* These buttons are part of the original MFi spec */
            buttons[button_count++] = gamepad.buttonA.isPressed;
            buttons[button_count++] = gamepad.buttonB.isPressed;
            buttons[button_count++] = gamepad.buttonX.isPressed;
            buttons[button_count++] = gamepad.buttonY.isPressed;
            buttons[button_count++] = gamepad.leftShoulder.isPressed;
            buttons[button_count++] = gamepad.rightShoulder.isPressed;

            /* These buttons are available on some newer controllers */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
            if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_LEFTSTICK)) {
                buttons[button_count++] = gamepad.leftThumbstickButton.isPressed;
            }
            if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_RIGHTSTICK)) {
                buttons[button_count++] = gamepad.rightThumbstickButton.isPressed;
            }
            if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_BACK)) {
                buttons[button_count++] = gamepad.buttonOptions.isPressed;
            }
            if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_GUIDE)) {
                buttons[button_count++] = gamepad.buttonHome.isPressed;
            }
            /* This must be the last button, so we can optionally handle it with pause_button_index below */
            if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_START)) {
                if (joystick->hwdata->uses_pause_handler) {
                    pause_button_index = button_count;
                    buttons[button_count++] = joystick->delayed_guide_button;
                } else {
                    buttons[button_count++] = gamepad.buttonMenu.isPressed;
                }
            }

#ifdef ENABLE_PHYSICAL_INPUT_PROFILE
            if (joystick->hwdata->has_dualshock_touchpad) {
                GCControllerDirectionPad *dpad;
                buttons[button_count++] = controller.physicalInputProfile.buttons[GCInputDualShockTouchpadButton].isPressed;

                dpad = controller.physicalInputProfile.dpads[GCInputDualShockTouchpadOne];
                if (dpad.xAxis.value || dpad.yAxis.value) {
                    SDL_PrivateJoystickTouchpad(joystick, 0, 0, SDL_PRESSED, (1.0f + dpad.xAxis.value) * 0.5f, 1.0f - (1.0f + dpad.yAxis.value) * 0.5f, 1.0f);
                } else {
                    SDL_PrivateJoystickTouchpad(joystick, 0, 0, SDL_RELEASED, 0.0f, 0.0f, 1.0f);
                }

                dpad = controller.physicalInputProfile.dpads[GCInputDualShockTouchpadTwo];
                if (dpad.xAxis.value || dpad.yAxis.value) {
                    SDL_PrivateJoystickTouchpad(joystick, 0, 1, SDL_PRESSED, (1.0f + dpad.xAxis.value) * 0.5f, 1.0f - (1.0f + dpad.yAxis.value) * 0.5f, 1.0f);
                } else {
                    SDL_PrivateJoystickTouchpad(joystick, 0, 1, SDL_RELEASED, 0.0f, 0.0f, 1.0f);
                }
            }

            if (joystick->hwdata->has_xbox_paddles) {
                if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_PADDLE1)) {
                    buttons[button_count++] = controller.physicalInputProfile.buttons[GCInputXboxPaddleOne].isPressed;
                }
                if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_PADDLE2)) {
                    buttons[button_count++] = controller.physicalInputProfile.buttons[GCInputXboxPaddleTwo].isPressed;
                }
                if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_PADDLE3)) {
                    buttons[button_count++] = controller.physicalInputProfile.buttons[GCInputXboxPaddleThree].isPressed;
                }
                if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_PADDLE4)) {
                    buttons[button_count++] = controller.physicalInputProfile.buttons[GCInputXboxPaddleFour].isPressed;
                }

                /*
                SDL_Log("Paddles: [%d,%d,%d,%d]",
                    controller.physicalInputProfile.buttons[GCInputXboxPaddleOne].isPressed,
                    controller.physicalInputProfile.buttons[GCInputXboxPaddleTwo].isPressed,
                    controller.physicalInputProfile.buttons[GCInputXboxPaddleThree].isPressed,
                    controller.physicalInputProfile.buttons[GCInputXboxPaddleFour].isPressed);
                */
            }

            if (joystick->hwdata->has_xbox_share_button) {
                buttons[button_count++] = controller.physicalInputProfile.buttons[GCInputXboxShareButton].isPressed;
            }
#endif
#pragma clang diagnostic pop

            hatstate = IOS_MFIJoystickHatStateForDPad(gamepad.dpad);

            for (i = 0; i < SDL_arraysize(axes); i++) {
                SDL_PrivateJoystickAxis(joystick, i, axes[i]);
            }

            for (i = 0; i < button_count; i++) {
                SDL_PrivateJoystickButton(joystick, i, buttons[i]);
            }

#ifdef ENABLE_MFI_SENSORS
            if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
                GCMotion *motion = controller.motion;
                if (motion && motion.sensorsActive) {
                    float data[3];

                    if (motion.hasRotationRate) {
                        GCRotationRate rate = motion.rotationRate;
                        data[0] = rate.x;
                        data[1] = rate.z;
                        data[2] = -rate.y;
                        SDL_PrivateJoystickSensor(joystick, SDL_SENSOR_GYRO, data, 3);
                    }
                    if (motion.hasGravityAndUserAcceleration) {
                        GCAcceleration accel = motion.acceleration;
                        data[0] = -accel.x * SDL_STANDARD_GRAVITY;
                        data[1] = -accel.y * SDL_STANDARD_GRAVITY;
                        data[2] = -accel.z * SDL_STANDARD_GRAVITY;
                        SDL_PrivateJoystickSensor(joystick, SDL_SENSOR_ACCEL, data, 3);
                    }
                }
            }
#endif /* ENABLE_MFI_SENSORS */

        } else if (controller.gamepad) {
            GCGamepad *gamepad = controller.gamepad;

            /* Button order matches the XInput Windows mappings. */
            Uint8 buttons[joystick->nbuttons];
            int button_count = 0;
            buttons[button_count++] = gamepad.buttonA.isPressed;
            buttons[button_count++] = gamepad.buttonB.isPressed;
            buttons[button_count++] = gamepad.buttonX.isPressed;
            buttons[button_count++] = gamepad.buttonY.isPressed;
            buttons[button_count++] = gamepad.leftShoulder.isPressed;
            buttons[button_count++] = gamepad.rightShoulder.isPressed;
            pause_button_index = button_count;
            buttons[button_count++] = joystick->delayed_guide_button;

            hatstate = IOS_MFIJoystickHatStateForDPad(gamepad.dpad);

            for (i = 0; i < button_count; i++) {
                SDL_PrivateJoystickButton(joystick, i, buttons[i]);
            }
        }
#if TARGET_OS_TV
        else if (controller.microGamepad) {
            GCMicroGamepad *gamepad = controller.microGamepad;

            Sint16 axes[] = {
                (Sint16) (gamepad.dpad.xAxis.value * 32767),
                (Sint16) (gamepad.dpad.yAxis.value * -32767),
            };

            for (i = 0; i < SDL_arraysize(axes); i++) {
                SDL_PrivateJoystickAxis(joystick, i, axes[i]);
            }

            Uint8 buttons[joystick->nbuttons];
            int button_count = 0;
            buttons[button_count++] = gamepad.buttonA.isPressed;
            buttons[button_count++] = gamepad.buttonX.isPressed;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
            /* This must be the last button, so we can optionally handle it with pause_button_index below */
            if (joystick->hwdata->button_mask & (1 << SDL_CONTROLLER_BUTTON_START)) {
                if (joystick->hwdata->uses_pause_handler) {
                    pause_button_index = button_count;
                    buttons[button_count++] = joystick->delayed_guide_button;
                } else {
                    buttons[button_count++] = gamepad.buttonMenu.isPressed;
                }
            }
#pragma clang diagnostic pop

            for (i = 0; i < button_count; i++) {
                SDL_PrivateJoystickButton(joystick, i, buttons[i]);
            }
        }
#endif /* TARGET_OS_TV */

        if (joystick->nhats > 0) {
            SDL_PrivateJoystickHat(joystick, 0, hatstate);
        }

        if (joystick->hwdata->uses_pause_handler
#if TARGET_OS_IOS || defined(__MACOSX__)
            && joystick->hwdata->mfi_input_context == NULL
#endif
        ) {
            for (i = 0; i < joystick->hwdata->num_pause_presses; i++) {
                SDL_PrivateJoystickButton(joystick, pause_button_index, SDL_PRESSED);
                SDL_PrivateJoystickButton(joystick, pause_button_index, SDL_RELEASED);
            }
            joystick->hwdata->num_pause_presses = 0;
        }

#ifdef ENABLE_MFI_BATTERY
        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            GCDeviceBattery *battery = controller.battery;
            if (battery) {
                SDL_JoystickPowerLevel ePowerLevel = SDL_JOYSTICK_POWER_UNKNOWN;

                switch (battery.batteryState) {
                case GCDeviceBatteryStateDischarging:
                    {
                        float power_level = battery.batteryLevel;
                        if (power_level <= 0.05f) {
                            ePowerLevel = SDL_JOYSTICK_POWER_EMPTY;
                        } else if (power_level <= 0.20f) {
                            ePowerLevel = SDL_JOYSTICK_POWER_LOW;
                        } else if (power_level <= 0.70f) {
                            ePowerLevel = SDL_JOYSTICK_POWER_MEDIUM;
                        } else {
                            ePowerLevel = SDL_JOYSTICK_POWER_FULL;
                        }
                    }
                    break;
                case GCDeviceBatteryStateCharging:
                    ePowerLevel = SDL_JOYSTICK_POWER_WIRED;
                    break;
                case GCDeviceBatteryStateFull:
                    ePowerLevel = SDL_JOYSTICK_POWER_FULL;
                    break;
                default:
                    break;
                }

                SDL_PrivateJoystickBatteryLevel(joystick, ePowerLevel);
            }
        }
#endif /* ENABLE_MFI_BATTERY */
    }
#endif /* SDL_JOYSTICK_MFI */
}

#ifdef ENABLE_MFI_RUMBLE

@interface SDL_RumbleMotor : NSObject
    @property(nonatomic,strong) CHHapticEngine *engine API_AVAILABLE(macos(10.16), ios(13.0), tvos(14.0));
    @property(nonatomic,strong) id<CHHapticPatternPlayer> player API_AVAILABLE(macos(10.16), ios(13.0), tvos(14.0));
    @property bool active;
@end

@implementation SDL_RumbleMotor {
}

-(void)cleanup
{
    @autoreleasepool {
        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            if (self.player != nil) {
                [self.player cancelAndReturnError:nil];
                self.player = nil;
            }
            if (self.engine != nil) {
                [self.engine stopWithCompletionHandler:nil];
                self.engine = nil;
            }
        }
    }
}

-(int)setIntensity:(float)intensity
{
    @autoreleasepool {
        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            NSError *error = nil;
            CHHapticDynamicParameter *param;

            if (self.engine == nil) {
                return SDL_SetError("Haptics engine was stopped");
            }

            if (intensity == 0.0f) {
                if (self.player && self.active) {
                    [self.player stopAtTime:0 error:&error];
                }
                self.active = false;
                return 0;
            }

            if (self.player == nil) {
                CHHapticEventParameter *event_param = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
                CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:[NSArray arrayWithObjects:event_param, nil] relativeTime:0 duration:GCHapticDurationInfinite];
                CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:[NSArray arrayWithObject:event] parameters:[[NSArray alloc] init] error:&error];
                if (error != nil) {
                    return SDL_SetError("Couldn't create haptic pattern: %s", [error.localizedDescription UTF8String]);
                }

                self.player = [self.engine createPlayerWithPattern:pattern error:&error];
                if (error != nil) {
                    return SDL_SetError("Couldn't create haptic player: %s", [error.localizedDescription UTF8String]);
                }
                self.active = false;
            }

            param = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0];
            [self.player sendParameters:[NSArray arrayWithObject:param] atTime:0 error:&error];
            if (error != nil) {
                return SDL_SetError("Couldn't update haptic player: %s", [error.localizedDescription UTF8String]);
            }

            if (!self.active) {
                [self.player startAtTime:0 error:&error];
                self.active = true;
            }
        }

        return 0;
    }
}

-(id) initWithController:(GCController*)controller locality:(GCHapticsLocality)locality API_AVAILABLE(macos(10.16), ios(14.0), tvos(14.0))
{
    @autoreleasepool {
        NSError *error;
        __weak __typeof(self) weakSelf;
        self = [super init];
        weakSelf = self;

        self.engine = [controller.haptics createEngineWithLocality:locality];
        if (self.engine == nil) {
            SDL_SetError("Couldn't create haptics engine");
            return nil;
        }

        [self.engine startAndReturnError:&error];
        if (error != nil) {
            SDL_SetError("Couldn't start haptics engine");
            return nil;
        }

        self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason stoppedReason) {
            SDL_RumbleMotor *_this = weakSelf;
            if (_this == nil) {
                return;
            }

            _this.player = nil;
            _this.engine = nil;
        };
        self.engine.resetHandler = ^{
            SDL_RumbleMotor *_this = weakSelf;
            if (_this == nil) {
                return;
            }

            _this.player = nil;
            [_this.engine startAndReturnError:nil];
        };

        return self;
    }
}

@end

@interface SDL_RumbleContext : NSObject
    @property(nonatomic,strong) SDL_RumbleMotor *m_low_frequency_motor;
    @property(nonatomic,strong) SDL_RumbleMotor *m_high_frequency_motor;
    @property(nonatomic,strong) SDL_RumbleMotor *m_left_trigger_motor;
    @property(nonatomic,strong) SDL_RumbleMotor *m_right_trigger_motor;
@end

@implementation SDL_RumbleContext {
}

-(id) initWithLowFrequencyMotor:(SDL_RumbleMotor*)low_frequency_motor
             HighFrequencyMotor:(SDL_RumbleMotor*)high_frequency_motor
               LeftTriggerMotor:(SDL_RumbleMotor*)left_trigger_motor
              RightTriggerMotor:(SDL_RumbleMotor*)right_trigger_motor
{
    self = [super init];
    self.m_low_frequency_motor = low_frequency_motor;
    self.m_high_frequency_motor = high_frequency_motor;
    self.m_left_trigger_motor = left_trigger_motor;
    self.m_right_trigger_motor = right_trigger_motor;
    return self;
}

-(int) rumbleWithLowFrequency:(Uint16)low_frequency_rumble andHighFrequency:(Uint16)high_frequency_rumble
{
    int result = 0;

    result += [self.m_low_frequency_motor setIntensity:((float)low_frequency_rumble / 65535.0f)];
    result += [self.m_high_frequency_motor setIntensity:((float)high_frequency_rumble / 65535.0f)];
    return ((result < 0) ? -1 : 0);
}

-(int) rumbleLeftTrigger:(Uint16)left_rumble andRightTrigger:(Uint16)right_rumble
{
    int result = 0;

    if (self.m_left_trigger_motor && self.m_right_trigger_motor) {
        result += [self.m_left_trigger_motor setIntensity:((float)left_rumble / 65535.0f)];
        result += [self.m_right_trigger_motor setIntensity:((float)right_rumble / 65535.0f)];
    } else {
        result = SDL_Unsupported();
    }
    return ((result < 0) ? -1 : 0);
}

-(void)cleanup
{
    [self.m_low_frequency_motor cleanup];
    [self.m_high_frequency_motor cleanup];
}

@end

static SDL_RumbleContext *IOS_JoystickInitRumble(GCController *controller)
{
    @autoreleasepool {
        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            SDL_RumbleMotor *low_frequency_motor = [[SDL_RumbleMotor alloc] initWithController:controller locality:GCHapticsLocalityLeftHandle];
            SDL_RumbleMotor *high_frequency_motor = [[SDL_RumbleMotor alloc] initWithController:controller locality:GCHapticsLocalityRightHandle];
            SDL_RumbleMotor *left_trigger_motor = [[SDL_RumbleMotor alloc] initWithController:controller locality:GCHapticsLocalityLeftTrigger];
            SDL_RumbleMotor *right_trigger_motor = [[SDL_RumbleMotor alloc] initWithController:controller locality:GCHapticsLocalityRightTrigger];
            if (low_frequency_motor && high_frequency_motor) {
                return [[SDL_RumbleContext alloc] initWithLowFrequencyMotor:low_frequency_motor
                                                         HighFrequencyMotor:high_frequency_motor
                                                           LeftTriggerMotor:left_trigger_motor
                                                          RightTriggerMotor:right_trigger_motor];
            }
        }
    }
    return nil;
}

#endif /* ENABLE_MFI_RUMBLE */

static int
IOS_JoystickRumble(SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{
#ifdef ENABLE_MFI_RUMBLE
    SDL_JoystickDeviceItem *device = joystick->hwdata;

    if (device == NULL) {
        return SDL_SetError("Controller is no longer connected");
    }

    if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
        if (!device->rumble && device->controller && device->controller.haptics) {
            SDL_RumbleContext *rumble = IOS_JoystickInitRumble(device->controller);
            if (rumble) {
                device->rumble = (void *)CFBridgingRetain(rumble);
            }
        }
    }

    if (device->rumble) {
        SDL_RumbleContext *rumble = (__bridge SDL_RumbleContext *)device->rumble;
        return [rumble rumbleWithLowFrequency:low_frequency_rumble andHighFrequency:high_frequency_rumble];
    } else {
        return SDL_Unsupported();
    }
#else
    return SDL_Unsupported();
#endif
}

static int
IOS_JoystickRumbleTriggers(SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
#ifdef ENABLE_MFI_RUMBLE
    SDL_JoystickDeviceItem *device = joystick->hwdata;

    if (device == NULL) {
        return SDL_SetError("Controller is no longer connected");
    }

    if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
        if (!device->rumble && device->controller && device->controller.haptics) {
            SDL_RumbleContext *rumble = IOS_JoystickInitRumble(device->controller);
            if (rumble) {
                device->rumble = (void *)CFBridgingRetain(rumble);
            }
        }
    }

    if (device->rumble) {
        SDL_RumbleContext *rumble = (__bridge SDL_RumbleContext *)device->rumble;
        return [rumble rumbleLeftTrigger:left_rumble andRightTrigger:right_rumble];
    } else {
        return SDL_Unsupported();
    }
#else
    return SDL_Unsupported();
#endif
}

static Uint32
IOS_JoystickGetCapabilities(SDL_Joystick *joystick)
{
    Uint32 result = 0;

#if defined(ENABLE_MFI_LIGHT) || defined(ENABLE_MFI_RUMBLE)
    @autoreleasepool {
        SDL_JoystickDeviceItem *device = joystick->hwdata;

        if (device == NULL) {
            return 0;
        }

        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            GCController *controller = device->controller;
            #ifdef ENABLE_MFI_LIGHT
            if (controller.light) {
                result |= SDL_JOYCAP_LED;
            }
            #endif

            #ifdef ENABLE_MFI_RUMBLE
            if (controller.haptics) {
                for (GCHapticsLocality locality in controller.haptics.supportedLocalities) {
                    if ([locality isEqualToString:GCHapticsLocalityHandles]) {
                        result |= SDL_JOYCAP_RUMBLE;
                    } else if ([locality isEqualToString:GCHapticsLocalityTriggers]) {
                        result |= SDL_JOYCAP_RUMBLE_TRIGGERS;
                    }
                }
            }
            #endif
        }
    }
#endif /* ENABLE_MFI_LIGHT || ENABLE_MFI_RUMBLE */

    return result;
}

static int
IOS_JoystickSetLED(SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
#ifdef ENABLE_MFI_LIGHT
    @autoreleasepool {
        SDL_JoystickDeviceItem *device = joystick->hwdata;

        if (device == NULL) {
            return SDL_SetError("Controller is no longer connected");
        }

        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            GCController *controller = device->controller;
            GCDeviceLight *light = controller.light;
            if (light) {
                light.color = [[GCColor alloc] initWithRed:(float)red / 255.0f
                                                     green:(float)green / 255.0f
                                                      blue:(float)blue / 255.0f];
                return 0;
            }
        }
    }
#endif /* ENABLE_MFI_LIGHT */

    return SDL_Unsupported();
}

static int
IOS_JoystickSendEffect(SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static int
IOS_JoystickSetSensorsEnabled(SDL_Joystick *joystick, SDL_bool enabled)
{
#ifdef ENABLE_MFI_SENSORS
    @autoreleasepool {
        SDL_JoystickDeviceItem *device = joystick->hwdata;

        if (device == NULL) {
            return SDL_SetError("Controller is no longer connected");
        }

        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            GCController *controller = device->controller;
            GCMotion *motion = controller.motion;
            if (motion) {
                motion.sensorsActive = enabled ? YES : NO;
                return 0;
            }
        }
    }
#endif /* ENABLE_MFI_SENSORS */

    return SDL_Unsupported();
}

static void
IOS_JoystickUpdate(SDL_Joystick *joystick)
{
    SDL_JoystickDeviceItem *device = joystick->hwdata;

    if (device == NULL) {
        return;
    }

    if (device->accelerometer) {
        IOS_AccelerometerUpdate(joystick);
    } else if (device->controller) {
        IOS_MFIJoystickUpdate(joystick);
    }
}

static void
IOS_JoystickClose(SDL_Joystick *joystick)
{
    SDL_JoystickDeviceItem *device = joystick->hwdata;

    if (device == NULL) {
        return;
    }

#if (TARGET_OS_IOS || defined(__MACOSX__)) && defined(SDL_JOYSTICK_MFI)
    IOS_MFIDisarmDigitalInput(device);
#endif

    device->joystick = NULL;

    @autoreleasepool {
#ifdef ENABLE_MFI_RUMBLE
        if (device->rumble) {
            SDL_RumbleContext *rumble = (__bridge SDL_RumbleContext *)device->rumble;

            [rumble cleanup];
            CFRelease(device->rumble);
            device->rumble = NULL;
        }
#endif /* ENABLE_MFI_RUMBLE */

        if (device->accelerometer) {
#ifdef SDL_JOYSTICK_iOS_ACCELEROMETER
            [motionManager stopAccelerometerUpdates];
#endif
        } else if (device->controller) {
#ifdef SDL_JOYSTICK_MFI
            GCController *controller = device->controller;
            controller.controllerPausedHandler = nil;
            controller.playerIndex = -1;

#ifdef ENABLE_MFI_SYSTEM_GESTURE_STATE
            if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
                for (id key in controller.physicalInputProfile.buttons) {
                    GCControllerButtonInput *button = controller.physicalInputProfile.buttons[key];
                    if ([button isBoundToSystemGesture]) {
                        button.preferredSystemGestureState = GCSystemGestureStateEnabled;
                    }
                }
            }
#endif /* ENABLE_MFI_SYSTEM_GESTURE_STATE */

#endif /* SDL_JOYSTICK_MFI */
        }
    }
    if (device->remote) {
        --SDL_AppleTVRemoteOpenedAsJoystick;
    }
}

static void
IOS_JoystickQuit(void)
{
    @autoreleasepool {
#ifdef SDL_JOYSTICK_MFI
        NSNotificationCenter *center = [NSNotificationCenter defaultCenter];

#if TARGET_OS_IOS
        /* An observer already executing may still set this static flag, but it
         * can no longer enter the joystick lock or mutate deviceList. */
        SDL_AtomicSet(&s_mfiReconcileRequested, 0);
#endif

        if (connectObserver) {
            [center removeObserver:connectObserver name:GCControllerDidConnectNotification object:nil];
            connectObserver = nil;
        }

        if (disconnectObserver) {
            [center removeObserver:disconnectObserver name:GCControllerDidDisconnectNotification object:nil];
            disconnectObserver = nil;
        }

#if TARGET_OS_TV
        SDL_DelHintCallback(SDL_HINT_APPLE_TV_REMOTE_ALLOW_ROTATION,
                            SDL_AppleTVRemoteRotationHintChanged, NULL);
#endif /* TARGET_OS_TV */
#endif /* SDL_JOYSTICK_MFI */

        while (deviceList != NULL) {
            IOS_RemoveJoystickDevice(deviceList);
        }

#if TARGET_OS_IOS && defined(SDL_JOYSTICK_MFI)
        s_mfiLastReconcileTicks = 0;
        s_mfiHasReconciled = SDL_FALSE;
        SDL_AtomicSet(&s_mfiReconcileRequested, 0);
#endif

#ifdef SDL_JOYSTICK_iOS_ACCELEROMETER
        motionManager = nil;
#endif
    }

    numjoysticks = 0;
}

static SDL_bool
IOS_JoystickGetGamepadMapping(int device_index, SDL_GamepadMapping *out)
{
    return SDL_FALSE;
}

#if defined(SDL_JOYSTICK_MFI) && defined(__MACOSX__)
SDL_bool IOS_SupportedHIDDevice(IOHIDDeviceRef device)
{
    if (is_macos11()) {
        if ([GCController supportsHIDDevice:device]) {
            return SDL_TRUE;
        }

        /* GCController supportsHIDDevice may return false if the device hasn't been
         * seen by the framework yet, so check a few controllers we know are supported.
         */
        {
            Sint32 vendor = 0;
            Sint32 product = 0;
            CFTypeRef refCF = NULL;

            refCF = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDVendorIDKey));
            if (refCF) {
                CFNumberGetValue(refCF, kCFNumberSInt32Type, &vendor);
            }

            refCF = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
            if (refCF) {
                CFNumberGetValue(refCF, kCFNumberSInt32Type, &product);
            }

            if (vendor == USB_VENDOR_MICROSOFT && SDL_IsJoystickXboxSeriesX(vendor, product)) {
                return SDL_TRUE;
            }
        }
    }
    return SDL_FALSE;
}
#endif

#if defined(SDL_JOYSTICK_MFI) && defined(ENABLE_PHYSICAL_INPUT_PROFILE)
static void
GetAppleSFSymbolsNameForElement(GCControllerElement *element, char *name)
{
    if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
        if (element) {
            [element.sfSymbolsName getCString: name maxLength: 255 encoding: NSASCIIStringEncoding];
        }
    }
}

static GCControllerDirectionPad *
GetDirectionalPadForController(GCController *controller)
{
    if (controller.extendedGamepad) {
        return controller.extendedGamepad.dpad;
    }

    if (controller.gamepad) {
        return controller.gamepad.dpad;
    }

    if (controller.microGamepad) {
        return controller.microGamepad.dpad;
    }

    return nil;
}
#endif /* SDL_JOYSTICK_MFI && ENABLE_PHYSICAL_INPUT_PROFILE */

static char elementName[256];

const char *
IOS_GameControllerGetAppleSFSymbolsNameForButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button)
{
    elementName[0] = '\0';
#if defined(SDL_JOYSTICK_MFI) && defined(ENABLE_PHYSICAL_INPUT_PROFILE)
    if (gamecontroller && SDL_GameControllerGetJoystick(gamecontroller)->driver == &SDL_IOS_JoystickDriver) {
        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            GCController *controller = SDL_GameControllerGetJoystick(gamecontroller)->hwdata->controller;
            if ([controller respondsToSelector:@selector(physicalInputProfile)]) {
                NSDictionary<NSString *,GCControllerElement *> *elements = controller.physicalInputProfile.elements;
                switch (button)
                {
                case SDL_CONTROLLER_BUTTON_A:
                    GetAppleSFSymbolsNameForElement(elements[GCInputButtonA], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    GetAppleSFSymbolsNameForElement(elements[GCInputButtonB], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_X:
                    GetAppleSFSymbolsNameForElement(elements[GCInputButtonX], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_Y:
                    GetAppleSFSymbolsNameForElement(elements[GCInputButtonY], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_BACK:
                    GetAppleSFSymbolsNameForElement(elements[GCInputButtonOptions], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_GUIDE:
                    GetAppleSFSymbolsNameForElement(elements[GCInputButtonHome], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_START:
                    GetAppleSFSymbolsNameForElement(elements[GCInputButtonMenu], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_LEFTSTICK:
                    GetAppleSFSymbolsNameForElement(elements[GCInputLeftThumbstickButton], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
                    GetAppleSFSymbolsNameForElement(elements[GCInputRightThumbstickButton], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
                    GetAppleSFSymbolsNameForElement(elements[GCInputLeftShoulder], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
                    GetAppleSFSymbolsNameForElement(elements[GCInputRightShoulder], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP: {
                    GCControllerDirectionPad * dpad = GetDirectionalPadForController(controller);
                    if (dpad) {
                        GetAppleSFSymbolsNameForElement(dpad.up, elementName);
                        if (SDL_strlen(elementName) == 0) {
                            SDL_strlcpy(elementName, "dpad.up.fill", sizeof(elementName));
                        }
                    }
                    break;
                }
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: {
                    GCControllerDirectionPad * dpad = GetDirectionalPadForController(controller);
                    if (dpad) {
                        GetAppleSFSymbolsNameForElement(dpad.down, elementName);
                        if (SDL_strlen(elementName) == 0) {
                            SDL_strlcpy(elementName, "dpad.down.fill", sizeof(elementName));
                        }
                    }
                    break;
                }
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: {
                    GCControllerDirectionPad * dpad = GetDirectionalPadForController(controller);
                    if (dpad) {
                        GetAppleSFSymbolsNameForElement(dpad.left, elementName);
                        if (SDL_strlen(elementName) == 0) {
                            SDL_strlcpy(elementName, "dpad.left.fill", sizeof(elementName));
                        }
                    }
                    break;
                }
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: {
                    GCControllerDirectionPad * dpad = GetDirectionalPadForController(controller);
                    if (dpad) {
                        GetAppleSFSymbolsNameForElement(dpad.right, elementName);
                        if (SDL_strlen(elementName) == 0) {
                            SDL_strlcpy(elementName, "dpad.right.fill", sizeof(elementName));
                        }
                    }
                    break;
                }
                case SDL_CONTROLLER_BUTTON_MISC1:
                    GetAppleSFSymbolsNameForElement(elements[GCInputDualShockTouchpadButton], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_PADDLE1:
                    GetAppleSFSymbolsNameForElement(elements[GCInputXboxPaddleOne], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_PADDLE2:
                    GetAppleSFSymbolsNameForElement(elements[GCInputXboxPaddleTwo], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_PADDLE3:
                    GetAppleSFSymbolsNameForElement(elements[GCInputXboxPaddleThree], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_PADDLE4:
                    GetAppleSFSymbolsNameForElement(elements[GCInputXboxPaddleFour], elementName);
                    break;
                case SDL_CONTROLLER_BUTTON_TOUCHPAD:
                    GetAppleSFSymbolsNameForElement(elements[GCInputDualShockTouchpadButton], elementName);
                    break;
                default:
                    break;
                }
            }
        }
    }
#endif
    return elementName;
}

const char *
IOS_GameControllerGetAppleSFSymbolsNameForAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis)
{
    elementName[0] = '\0';
#if defined(SDL_JOYSTICK_MFI) && defined(ENABLE_PHYSICAL_INPUT_PROFILE)
    if (gamecontroller && SDL_GameControllerGetJoystick(gamecontroller)->driver == &SDL_IOS_JoystickDriver) {
        if (@available(macOS 10.16, iOS 14.0, tvOS 14.0, *)) {
            GCController *controller = SDL_GameControllerGetJoystick(gamecontroller)->hwdata->controller;
            if ([controller respondsToSelector:@selector(physicalInputProfile)]) {
                NSDictionary<NSString *,GCControllerElement *> *elements = controller.physicalInputProfile.elements;
                switch (axis)
                {
                case SDL_CONTROLLER_AXIS_LEFTX:
                    GetAppleSFSymbolsNameForElement(elements[GCInputLeftThumbstick], elementName);
                    break;
                case SDL_CONTROLLER_AXIS_LEFTY:
                    GetAppleSFSymbolsNameForElement(elements[GCInputLeftThumbstick], elementName);
                    break;
                case SDL_CONTROLLER_AXIS_RIGHTX:
                    GetAppleSFSymbolsNameForElement(elements[GCInputRightThumbstick], elementName);
                    break;
                case SDL_CONTROLLER_AXIS_RIGHTY:
                    GetAppleSFSymbolsNameForElement(elements[GCInputRightThumbstick], elementName);
                    break;
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                    GetAppleSFSymbolsNameForElement(elements[GCInputLeftTrigger], elementName);
                    break;
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                    GetAppleSFSymbolsNameForElement(elements[GCInputRightTrigger], elementName);
                    break;
                default:
                    break;
                }
            }
        }
    }
#endif
    return *elementName ? elementName : NULL;
}


SDL_JoystickDriver SDL_IOS_JoystickDriver =
{
    IOS_JoystickInit,
    IOS_JoystickGetCount,
    IOS_JoystickDetect,
    IOS_JoystickGetDeviceName,
    IOS_JoystickGetDevicePath,
    IOS_JoystickGetDevicePlayerIndex,
    IOS_JoystickSetDevicePlayerIndex,
    IOS_JoystickGetDeviceGUID,
    IOS_JoystickGetDeviceInstanceID,
    IOS_JoystickOpen,
    IOS_JoystickRumble,
    IOS_JoystickRumbleTriggers,
    IOS_JoystickGetCapabilities,
    IOS_JoystickSetLED,
    IOS_JoystickSendEffect,
    IOS_JoystickSetSensorsEnabled,
    IOS_JoystickUpdate,
    IOS_JoystickClose,
    IOS_JoystickQuit,
    IOS_JoystickGetGamepadMapping
};

/* vi: set ts=4 sw=4 expandtab: */
