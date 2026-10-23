#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif

#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <TargetConditionals.h>
#import <UIKit/UIKit.h>

#include "SDL.h"

#include "Emulator/Graphics/ControllerHotplug.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kMarker = "LUMEN_MFI_CONTROLLER_DELIVERY";

using Clock = std::chrono::steady_clock;

void LogPhase(const char* phase)
{
	std::printf("%s phase=%s\n", kMarker, phase);
	std::fflush(stdout);
}

void PerformOnMainSync(dispatch_block_t operation)
{
	if (NSThread.isMainThread)
	{
		operation();
	}
	else
	{
		dispatch_sync(dispatch_get_main_queue(), operation);
	}
}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout)
{
	const auto deadline = Clock::now() + timeout;
	while (Clock::now() < deadline)
	{
		if (predicate())
		{
			return true;
		}

		@autoreleasepool
		{
			NSDate* wake = [NSDate dateWithTimeIntervalSinceNow:0.01];
			[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:wake];
		}
	}
	return predicate();
}

bool ContainsInstance(const std::vector<SDL_JoystickID>& instances, SDL_JoystickID instance)
{
	return std::find(instances.begin(), instances.end(), instance) != instances.end();
}

struct ControllerConsumerProbe
{
	int                 open_count         = 0;
	int                 notification_count = 0;
	SDL_GameController* last_controller    = nullptr;
	SDL_JoystickID      last_instance      = -1;
};

void RecordConsumerOpen(const Lumen::Libs::Graphics::OpenedGameController& opened, void* context)
{
	auto* probe = static_cast<ControllerConsumerProbe*>(context);
	if (probe == nullptr)
	{
		return;
	}
	probe->open_count++;
	if (Lumen::Libs::Graphics::GameControllerOpenShouldNotify(opened.origin))
	{
		probe->notification_count++;
	}
	probe->last_controller = opened.controller;
	probe->last_instance   = opened.instance_id;
	std::printf("%s phase=consumer-open origin=%s index=%d instance=%d opens=%d notifications=%d\n", kMarker,
	            Lumen::Libs::Graphics::GameControllerOpenOriginName(opened.origin), opened.device_index,
	            opened.instance_id, probe->open_count, probe->notification_count);
	std::fflush(stdout);
}

bool TakeControllerAddedEvent(SDL_JoystickID expected_instance, int* device_index, int* matching_events,
                              std::string& error)
{
	if (device_index == nullptr || matching_events == nullptr)
	{
		error = "invalid-added-event-output";
		return false;
	}
	*device_index    = -1;
	*matching_events = 0;
	for (;;)
	{
		SDL_Event events[16];
		const int count = SDL_PeepEvents(events, SDL_arraysize(events), SDL_GETEVENT,
		                                 SDL_CONTROLLERDEVICEADDED, SDL_CONTROLLERDEVICEADDED);
		if (count < 0)
		{
			error = SDL_GetError();
			return false;
		}
		if (count == 0)
		{
			return true;
		}
		for (int index = 0; index < count; ++index)
		{
			const int candidate_index = events[index].cdevice.which;
			if (SDL_JoystickGetDeviceInstanceID(candidate_index) == expected_instance)
			{
				*device_index = candidate_index;
				++*matching_events;
			}
		}
	}
}

struct DeliveryTiming
{
	Clock::time_point down;
	Clock::time_point up;
	std::string       error;
};

struct ExpectedButtonEvent
{
	Uint32 type;
	Uint8  button;
};

bool SetVirtualButton(GCVirtualController* controller, NSString* element, CGFloat value,
                      std::string& error)
{
	if (@available(iOS 17.0, *))
	{
		__block NSString* input_exception = nil;
		PerformOnMainSync(^{
			@try
			{
				[controller setValue:value forButtonElement:element];
			}
			@catch (NSException* exception)
			{
				input_exception = exception.reason ?: exception.name ?: @"unknown";
			}
		});
		if (input_exception != nil)
		{
			error = input_exception.UTF8String ?: "unknown";
			return false;
		}
		return true;
	}
	error = "requires-ios-17";
	return false;
}

bool TapVirtualButton(GCVirtualController* controller, NSString* element,
                      std::chrono::milliseconds hold, std::string& error)
{
	if (!SetVirtualButton(controller, element, 1.0, error))
	{
		return false;
	}
	std::this_thread::sleep_for(hold);
	return SetVirtualButton(controller, element, 0.0, error);
}

bool ReadButtonEvents(std::vector<SDL_ControllerButtonEvent>& button_events, std::string& error)
{
	for (;;)
	{
		SDL_Event events[32];
		const int count = SDL_PeepEvents(events, SDL_arraysize(events), SDL_GETEVENT,
		                                 SDL_FIRSTEVENT, SDL_LASTEVENT);
		if (count < 0)
		{
			error = SDL_GetError();
			return false;
		}
		if (count == 0)
		{
			return true;
		}
		for (int index = 0; index < count; ++index)
		{
			if (events[index].type == SDL_CONTROLLERBUTTONDOWN ||
			    events[index].type == SDL_CONTROLLERBUTTONUP)
			{
				button_events.push_back(events[index].cbutton);
			}
		}
	}
}

void LogButtonEvents(const char* phase, const std::vector<SDL_ControllerButtonEvent>& button_events)
{
	for (size_t index = 0; index < button_events.size(); ++index)
	{
		const SDL_ControllerButtonEvent& event = button_events[index];
		std::printf("%s phase=%s ordinal=%zu type=%s which=%d button=%u state=%u\n", kMarker,
		            phase, index, event.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up",
		            event.which, static_cast<unsigned>(event.button),
		            static_cast<unsigned>(event.state));
	}
	std::fflush(stdout);
}

bool HasButtonEventForInstance(const std::vector<SDL_ControllerButtonEvent>& events,
	                           SDL_JoystickID instance)
{
	return std::any_of(events.begin(), events.end(), [instance](const SDL_ControllerButtonEvent& event)
	{
		return event.which == instance;
	});
}

bool HasExactButtonSequence(const std::vector<SDL_ControllerButtonEvent>& all_events,
                            SDL_JoystickID instance,
                            const std::vector<ExpectedButtonEvent>& expected)
{
	std::vector<SDL_ControllerButtonEvent> instance_events;
	for (const SDL_ControllerButtonEvent& event : all_events)
	{
		if (event.which == instance)
		{
			instance_events.push_back(event);
		}
	}
	if (instance_events.size() != expected.size())
	{
		return false;
	}
	for (size_t index = 0; index < expected.size(); ++index)
	{
		const bool expected_down = expected[index].type == SDL_CONTROLLERBUTTONDOWN;
		if (instance_events[index].type != expected[index].type ||
		    instance_events[index].button != expected[index].button ||
		    instance_events[index].state != (expected_down ? SDL_PRESSED : SDL_RELEASED))
		{
			return false;
		}
	}
	return true;
}

int FindDeviceIndex(SDL_JoystickID instance)
{
	const int joystick_count = SDL_NumJoysticks();
	for (int index = 0; index < joystick_count; ++index)
	{
		if (SDL_JoystickGetDeviceInstanceID(index) == instance)
		{
			return index;
		}
	}
	return -1;
}

int RunTest()
{
	constexpr auto kConnectTimeout = std::chrono::seconds(5);
	constexpr auto kEnumerationTimeout = std::chrono::seconds(5);
	constexpr auto kDisconnectTimeout = std::chrono::seconds(3);
	constexpr auto kBaselineSettle = std::chrono::milliseconds(750);
	constexpr auto kButtonHold = std::chrono::milliseconds(75);
	constexpr auto kPostInputRunLoop = std::chrono::milliseconds(150);
	constexpr auto kNoPollSleep = std::chrono::milliseconds(1250);

	bool                         sdl_initialized    = false;
	bool                         virtual_connected  = false;
	int                          baseline_joysticks = 0;
	SDL_GameController*          game_controller    = nullptr;
	GCVirtualController*         virtual_controller = nil;
	ControllerConsumerProbe      consumer_probe;
	std::vector<SDL_JoystickID> baseline_instances;

	auto finish = [&](int requested_code, std::string reason) -> int
	{
		bool clean_disconnect = true;

		if (virtual_controller != nil)
		{
			LogPhase("disconnect");
			GCVirtualController* controller_to_disconnect = virtual_controller;
			__block BOOL disconnect_threw = NO;
			PerformOnMainSync(^{
				@try
				{
					[controller_to_disconnect disconnect];
				}
				@catch (NSException* exception)
				{
					disconnect_threw = YES;
					std::printf("%s phase=disconnect-exception name=%s reason=%s\n", kMarker,
					            exception.name.UTF8String ?: "unknown",
					            exception.reason.UTF8String ?: "unknown");
				}
			});
			clean_disconnect = clean_disconnect && !disconnect_threw;

			if (virtual_connected && sdl_initialized)
			{
				const bool removed = WaitFor([&]
				{
					SDL_GameControllerUpdate();
					return virtual_controller.controller == nil && SDL_NumJoysticks() == baseline_joysticks;
				}, kDisconnectTimeout);
				clean_disconnect = clean_disconnect && removed;
			}
		}

		if (game_controller != nullptr)
		{
			LogPhase("close");
			SDL_GameControllerClose(game_controller);
			game_controller = nullptr;
		}
		for (const SDL_JoystickID instance : baseline_instances)
		{
			SDL_GameController* baseline_controller = SDL_GameControllerFromInstanceID(instance);
			if (baseline_controller != nullptr)
			{
				SDL_GameControllerClose(baseline_controller);
			}
		}
		if (sdl_initialized)
		{
			LogPhase("quit");
			SDL_Quit();
			sdl_initialized = false;
		}

		int code = requested_code;
		if (!clean_disconnect && code == 0)
		{
			code = 1;
			reason = "disconnect-timeout";
		}

		const char* result = code == 0 ? "pass" : (code == 2 ? "unsupported" : "fail");
		std::printf("%s result=%s reason=%s cleanup=%s\n", kMarker, result, reason.c_str(),
		            clean_disconnect ? "clean" : "incomplete");
		std::fflush(stdout);
		return code;
	};

	if (@available(iOS 17.0, *))
	{
		SDL_SetMainReady();
		LogPhase("sdl-init");
		if (SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) < 0)
		{
			std::printf("%s phase=sdl-init-failed error=%s\n", kMarker, SDL_GetError());
			return finish(1, "sdl-init-failed");
		}
		sdl_initialized = true;
		SDL_GameControllerEventState(SDL_ENABLE);
		SDL_JoystickEventState(SDL_ENABLE);

		/* GameController session activation is asynchronous in Simulator. Let
		 * controllers that predate this test enter SDL before freezing the
		 * baseline, otherwise the system MFi gamepad can be mistaken for the
		 * newly-created touch controller. */
		LogPhase("baseline-settle");
		const auto baseline_deadline = Clock::now() + kBaselineSettle;
		while (Clock::now() < baseline_deadline)
		{
			SDL_GameControllerUpdate();
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		SDL_GameControllerUpdate();
		SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

		baseline_joysticks = SDL_NumJoysticks();
		if (baseline_joysticks < 0)
		{
			return finish(1, "invalid-baseline-count");
		}

		baseline_instances.reserve(static_cast<size_t>(baseline_joysticks));
		for (int index = 0; index < baseline_joysticks; ++index)
		{
			baseline_instances.push_back(SDL_JoystickGetDeviceInstanceID(index));
		}
		const int initial_opened = Lumen::Libs::Graphics::ReconcileUnopenedGameControllers(
		    Lumen::Libs::Graphics::GameControllerOpenOrigin::InitialScan, RecordConsumerOpen, &consumer_probe);
		if (consumer_probe.notification_count != 0)
		{
			return finish(1, "initial-scan-produced-notification");
		}
		std::printf("%s phase=consumer-initial-scan opened=%d notifications=%d\n", kMarker, initial_opened,
		            consumer_probe.notification_count);
		std::fflush(stdout);

		LogPhase("virtual-create");
		__block GCVirtualController* created_controller = nil;
		__block std::string create_error;
		PerformOnMainSync(^{
			@try
			{
				GCVirtualControllerConfiguration* configuration = [[GCVirtualControllerConfiguration alloc] init];
				configuration.elements = [NSSet setWithObjects:GCInputButtonA, GCInputButtonB,
				                          GCInputButtonX, GCInputButtonY, nil];
				configuration.hidden = YES;
				created_controller = [GCVirtualController virtualControllerWithConfiguration:configuration];
			}
			@catch (NSException* exception)
			{
				create_error = exception.reason.UTF8String ?: exception.name.UTF8String ?: "unknown";
			}
		});
		virtual_controller = created_controller;
		if (!create_error.empty())
		{
			std::printf("%s phase=virtual-create-exception reason=%s\n", kMarker, create_error.c_str());
			return finish(2, "standalone-virtual-create-unsupported");
		}

		if (virtual_controller == nil)
		{
			return finish(2, "standalone-virtual-create-returned-nil");
		}

		LogPhase("virtual-connect");
		dispatch_semaphore_t connected = dispatch_semaphore_create(0);
		__block NSError* connection_error = nil;
		__block std::string connect_exception;
		GCVirtualController* controller_to_connect = virtual_controller;
		PerformOnMainSync(^{
			@try
			{
				[controller_to_connect connectWithReplyHandler:^(NSError* error)
				{
					connection_error = error;
					dispatch_semaphore_signal(connected);
				}];
			}
			@catch (NSException* exception)
			{
				connect_exception = exception.reason.UTF8String ?: exception.name.UTF8String ?: "unknown";
			}
		});
		if (!connect_exception.empty())
		{
			std::printf("%s phase=virtual-connect-exception reason=%s\n", kMarker,
			            connect_exception.c_str());
			return finish(2, "standalone-virtual-connect-unsupported");
		}

		const bool connect_replied = WaitFor([&]
		{
			return dispatch_semaphore_wait(connected, DISPATCH_TIME_NOW) == 0;
		}, kConnectTimeout);
		if (!connect_replied)
		{
			return finish(2, "standalone-virtual-connect-timeout");
		}
		if (connection_error != nil)
		{
			std::printf("%s phase=virtual-connect-error domain=%s code=%ld description=%s\n", kMarker,
			            connection_error.domain.UTF8String ?: "unknown", (long)connection_error.code,
			            connection_error.localizedDescription.UTF8String ?: "unknown");
			return finish(2, "standalone-virtual-connect-error");
		}
		if (virtual_controller.controller == nil)
		{
			return finish(2, "standalone-virtual-controller-unavailable");
		}
		virtual_connected = true;

		const bool enumerated = WaitFor([&]
		{
			/* Exercise the production Detect() reconciliation path. UIKit's
			 * notification only requests this pass; it never mutates SDL's list. */
			SDL_GameControllerUpdate();
			return SDL_NumJoysticks() > baseline_joysticks;
		}, kEnumerationTimeout);
		if (!enumerated)
		{
			return finish(1, "sdl-connect-timeout");
		}

		int new_device_index = -1;
		int new_device_count = 0;
		const int joystick_count = SDL_NumJoysticks();
		for (int index = 0; index < joystick_count; ++index)
		{
			const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
			if (!ContainsInstance(baseline_instances, instance))
			{
				new_device_index = index;
				++new_device_count;
			}
		}
		if (new_device_count != 1 || new_device_index < 0)
		{
			std::printf("%s phase=scan-failed baseline=%d current=%d candidates=%d\n", kMarker,
			            baseline_joysticks, joystick_count, new_device_count);
			return finish(1, "ambiguous-sdl-controller");
		}

		const SDL_JoystickID expected_instance = SDL_JoystickGetDeviceInstanceID(new_device_index);
		std::printf("%s phase=scan index=%d instance=%d name=%s game_controller=%d\n", kMarker,
		            new_device_index, expected_instance,
		            SDL_JoystickNameForIndex(new_device_index) ?: "unknown",
		            SDL_IsGameController(new_device_index) == SDL_TRUE ? 1 : 0);
		std::fflush(stdout);
		if (SDL_IsGameController(new_device_index) != SDL_TRUE)
		{
			return finish(1, "missing-sdl-controller-mapping");
		}

		int         added_device_index   = -1;
		int         matching_added_events = 0;
		std::string added_event_error;
		if (!TakeControllerAddedEvent(expected_instance, &added_device_index, &matching_added_events,
		                              added_event_error))
		{
			std::printf("%s phase=consumer-added-read-failed error=%s\n", kMarker, added_event_error.c_str());
			return finish(1, "consumer-added-read-failed");
		}
		if (matching_added_events != 1 || added_device_index != new_device_index)
		{
			return finish(1, "consumer-added-event-missing-or-duplicate");
		}
		if (SDL_GameControllerFromInstanceID(expected_instance) != nullptr)
		{
			return finish(1, "consumer-controller-open-before-event");
		}

		const int opens_before_event = consumer_probe.open_count;
		const auto event_open = Lumen::Libs::Graphics::EnsureGameControllerOpen(
		    added_device_index, Lumen::Libs::Graphics::GameControllerOpenOrigin::DeviceAddedEvent,
		    RecordConsumerOpen, &consumer_probe);
		game_controller = event_open.controller;
		if (event_open.status != Lumen::Libs::Graphics::GameControllerOpenStatus::Opened ||
		    game_controller == nullptr || consumer_probe.open_count != opens_before_event + 1 ||
		    consumer_probe.notification_count != 1 ||
		    SDL_GameControllerFromInstanceID(expected_instance) != game_controller ||
		    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(game_controller)) != expected_instance)
		{
			return finish(1, "consumer-event-open-failed");
		}
		LogPhase("consumer-event-open");

		const auto duplicate_event = Lumen::Libs::Graphics::EnsureGameControllerOpen(
		    added_device_index, Lumen::Libs::Graphics::GameControllerOpenOrigin::DeviceAddedEvent,
		    RecordConsumerOpen, &consumer_probe);
		const int duplicate_reconcile = Lumen::Libs::Graphics::ReconcileUnopenedGameControllers(
		    Lumen::Libs::Graphics::GameControllerOpenOrigin::RuntimeFallback, RecordConsumerOpen,
		    &consumer_probe);
		if (duplicate_event.status != Lumen::Libs::Graphics::GameControllerOpenStatus::AlreadyOpen ||
		    duplicate_reconcile != 0 || consumer_probe.open_count != opens_before_event + 1 ||
		    consumer_probe.notification_count != 1)
		{
			return finish(1, "consumer-event-not-idempotent");
		}
		LogPhase("consumer-idempotent");

		LogPhase("drain-baseline");
		SDL_GameControllerUpdate();
		SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

		DeliveryTiming timing;
		std::string input_error;
		const auto no_poll_start = Clock::now();
		LogPhase("abxy-no-poll-window");
		timing.down = Clock::now();
		if (!TapVirtualButton(virtual_controller, GCInputButtonA, kButtonHold, input_error))
		{
			std::printf("%s phase=virtual-input-exception button=A reason=%s\n", kMarker,
			            input_error.c_str());
			return finish(2, "standalone-virtual-input-unsupported");
		}
		timing.up = Clock::now();
		if (!TapVirtualButton(virtual_controller, GCInputButtonB, kButtonHold, input_error) ||
		    !TapVirtualButton(virtual_controller, GCInputButtonX, kButtonHold, input_error) ||
		    !TapVirtualButton(virtual_controller, GCInputButtonY, kButtonHold, input_error))
		{
			std::printf("%s phase=virtual-input-exception button=ABXY reason=%s\n", kMarker,
			            input_error.c_str());
			return finish(2, "standalone-virtual-input-unsupported");
		}

		/* All eight mutations finish before the first SDL update. The wait is
		 * measured from A-down so this gate covers a main-loop stall longer than
		 * one second, not merely four adjacent short taps. */
		std::this_thread::sleep_until(no_poll_start + kNoPollSleep);
		std::this_thread::sleep_for(kPostInputRunLoop);
		const auto no_poll_end = Clock::now();
		const auto hold_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(timing.up - timing.down).count();
		const auto no_poll_ms =
		    std::chrono::duration_cast<std::chrono::milliseconds>(no_poll_end - no_poll_start).count();
		std::printf("%s phase=abxy-input-complete a_hold_ms=%lld no_poll_ms=%lld\n", kMarker,
		            static_cast<long long>(hold_ms), static_cast<long long>(no_poll_ms));
		std::fflush(stdout);
		if (hold_ms < 50 || hold_ms > 100 || no_poll_ms <= 1000)
		{
			return finish(1, "invalid-delivery-timing");
		}

		LogPhase("abxy-single-update");
		SDL_GameControllerUpdate();
		std::vector<SDL_ControllerButtonEvent> button_events;
		std::string event_error;
		if (!ReadButtonEvents(button_events, event_error))
		{
			std::printf("%s phase=abxy-event-read-failed error=%s\n", kMarker, event_error.c_str());
			return finish(1, "abxy-event-read-failed");
		}
		LogButtonEvents("abxy-event", button_events);
		const std::vector<ExpectedButtonEvent> expected_abxy = {
		    {SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A},
		    {SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A},
		    {SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_B},
		    {SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_B},
		    {SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_X},
		    {SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_X},
		    {SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_Y},
		    {SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_Y},
		};
		if (!HasExactButtonSequence(button_events, expected_instance, expected_abxy))
		{
			return finish(1, "unexpected-abxy-button-sequence");
		}
		for (int button = SDL_CONTROLLER_BUTTON_A; button <= SDL_CONTROLLER_BUTTON_Y; ++button)
		{
			if (SDL_GameControllerGetButton(game_controller,
			                                static_cast<SDL_GameControllerButton>(button)) != SDL_RELEASED)
			{
				return finish(1, "abxy-final-state-not-released");
			}
		}

		LogPhase("abxy-duplicate-check");
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error))
		{
			return finish(1, "abxy-duplicate-check-read-failed");
		}
		if (HasButtonEventForInstance(button_events, expected_instance))
		{
			LogButtonEvents("abxy-duplicate", button_events);
			return finish(1, "duplicate-abxy-button-event");
		}

		LogPhase("hold-down");
		if (!SetVirtualButton(virtual_controller, GCInputButtonA, 1.0, input_error))
		{
			return finish(1, "hold-down-input-failed");
		}
		std::this_thread::sleep_for(kPostInputRunLoop);
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error) ||
		    !HasExactButtonSequence(button_events, expected_instance,
		                            {{SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A}}) ||
		    SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_A) != SDL_PRESSED)
		{
			LogButtonEvents("hold-down-event", button_events);
			return finish(1, "unexpected-hold-down-state");
		}

		LogPhase("hold-stable");
		for (int update = 0; update < 3; ++update)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
			SDL_GameControllerUpdate();
			button_events.clear();
			if (!ReadButtonEvents(button_events, event_error))
			{
				return finish(1, "hold-stable-read-failed");
			}
			if (HasButtonEventForInstance(button_events, expected_instance) ||
			    SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_A) != SDL_PRESSED)
			{
				LogButtonEvents("hold-duplicate", button_events);
				return finish(1, "duplicate-hold-button-event");
			}
		}

		LogPhase("hold-up");
		if (!SetVirtualButton(virtual_controller, GCInputButtonA, 0.0, input_error))
		{
			return finish(1, "hold-up-input-failed");
		}
		std::this_thread::sleep_for(kPostInputRunLoop);
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error) ||
		    !HasExactButtonSequence(button_events, expected_instance,
		                            {{SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A}}) ||
		    SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_A) != SDL_RELEASED)
		{
			LogButtonEvents("hold-up-event", button_events);
			return finish(1, "unexpected-hold-up-state");
		}
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error) ||
		    HasButtonEventForInstance(button_events, expected_instance))
		{
			LogButtonEvents("hold-release-duplicate", button_events);
			return finish(1, "duplicate-hold-release-event");
		}

		LogPhase("close-before-input");
		SDL_GameControllerClose(game_controller);
		game_controller = nullptr;
		SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
		if (!TapVirtualButton(virtual_controller, GCInputButtonB, kButtonHold, input_error))
		{
			return finish(1, "closed-input-failed");
		}
		std::this_thread::sleep_for(kPostInputRunLoop);
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error))
		{
			return finish(1, "closed-input-read-failed");
		}
		if (HasButtonEventForInstance(button_events, expected_instance))
		{
			LogButtonEvents("closed-input-event", button_events);
			return finish(1, "closed-input-produced-event");
		}

		LogPhase("reopen");
		const int reopen_index = FindDeviceIndex(expected_instance);
		if (reopen_index < 0 || SDL_IsGameController(reopen_index) != SDL_TRUE)
		{
			return finish(1, "reopen-device-missing");
		}
		const int notifications_before_initial_reopen = consumer_probe.notification_count;
		const auto initial_reopen = Lumen::Libs::Graphics::EnsureGameControllerOpen(
		    reopen_index, Lumen::Libs::Graphics::GameControllerOpenOrigin::InitialScan,
		    RecordConsumerOpen, &consumer_probe);
		game_controller = initial_reopen.controller;
		if (initial_reopen.status != Lumen::Libs::Graphics::GameControllerOpenStatus::Opened ||
		    game_controller == nullptr ||
		    consumer_probe.notification_count != notifications_before_initial_reopen ||
		    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(game_controller)) != expected_instance)
		{
			return finish(1, "reopen-instance-mismatch");
		}
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error) ||
		    HasButtonEventForInstance(button_events, expected_instance) ||
		    SDL_GameControllerGetButton(game_controller, SDL_CONTROLLER_BUTTON_B) != SDL_RELEASED)
		{
			LogButtonEvents("reopen-latent-event", button_events);
			return finish(1, "closed-input-replayed-after-reopen");
		}

		LogPhase("reopen-input");
		if (!TapVirtualButton(virtual_controller, GCInputButtonB, kButtonHold, input_error))
		{
			return finish(1, "reopen-input-failed");
		}
		std::this_thread::sleep_for(kPostInputRunLoop);
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error) ||
		    !HasExactButtonSequence(button_events, expected_instance,
		                            {{SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_B},
		                             {SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_B}}))
		{
			LogButtonEvents("reopen-input-event", button_events);
			return finish(1, "reopen-input-sequence-failed");
		}

		LogPhase("hotplug-disconnect");
		GCVirtualController* controller_to_disconnect = virtual_controller;
		__block NSString* hotplug_disconnect_exception = nil;
		PerformOnMainSync(^{
			@try
			{
				[controller_to_disconnect disconnect];
			}
			@catch (NSException* exception)
			{
				hotplug_disconnect_exception = exception.reason ?: exception.name ?: @"unknown";
			}
		});
		if (hotplug_disconnect_exception != nil)
		{
			std::printf("%s phase=hotplug-disconnect-exception reason=%s\n", kMarker,
			            hotplug_disconnect_exception.UTF8String ?: "unknown");
			return finish(1, "hotplug-disconnect-exception");
		}
		const bool hotplug_removed = WaitFor([&]
		{
			SDL_GameControllerUpdate();
			return virtual_controller.controller == nil && SDL_NumJoysticks() == baseline_joysticks;
		}, kDisconnectTimeout);
		if (!hotplug_removed)
		{
			return finish(1, "hotplug-remove-timeout");
		}
		virtual_connected = false;
		if (SDL_GameControllerGetAttached(game_controller) != SDL_FALSE)
		{
			return finish(1, "hotplug-controller-still-attached");
		}
		SDL_GameControllerClose(game_controller);
		game_controller = nullptr;
		if (FindDeviceIndex(expected_instance) >= 0)
		{
			return finish(1, "hotplug-old-instance-still-enumerated");
		}
		SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

		LogPhase("hotplug-reconnect");
		dispatch_semaphore_t reconnected = dispatch_semaphore_create(0);
		__block NSError* reconnection_error = nil;
		__block NSString* reconnect_exception = nil;
		GCVirtualController* controller_to_reconnect = virtual_controller;
		PerformOnMainSync(^{
			@try
			{
				[controller_to_reconnect connectWithReplyHandler:^(NSError* error)
				{
					reconnection_error = error;
					dispatch_semaphore_signal(reconnected);
				}];
			}
			@catch (NSException* exception)
			{
				reconnect_exception = exception.reason ?: exception.name ?: @"unknown";
			}
		});
		if (reconnect_exception != nil)
		{
			std::printf("%s phase=hotplug-reconnect-exception reason=%s\n", kMarker,
			            reconnect_exception.UTF8String ?: "unknown");
			return finish(1, "hotplug-reconnect-exception");
		}
		const bool reconnect_replied = WaitFor([&]
		{
			return dispatch_semaphore_wait(reconnected, DISPATCH_TIME_NOW) == 0;
		}, kConnectTimeout);
		if (!reconnect_replied || reconnection_error != nil || virtual_controller.controller == nil)
		{
			if (reconnection_error != nil)
			{
				std::printf("%s phase=hotplug-reconnect-error domain=%s code=%ld description=%s\n",
				            kMarker, reconnection_error.domain.UTF8String ?: "unknown",
				            (long)reconnection_error.code,
				            reconnection_error.localizedDescription.UTF8String ?: "unknown");
			}
			return finish(1, reconnect_replied ? "hotplug-reconnect-error" :
			                                      "hotplug-reconnect-timeout");
		}
		virtual_connected = true;

		const bool hotplug_enumerated = WaitFor([&]
		{
			SDL_GameControllerUpdate();
			return SDL_NumJoysticks() > baseline_joysticks;
		}, kEnumerationTimeout);
		if (!hotplug_enumerated)
		{
			return finish(1, "hotplug-reenumeration-timeout");
		}
		int reconnected_index = -1;
		int reconnected_count = 0;
		const int reconnected_joysticks = SDL_NumJoysticks();
		for (int index = 0; index < reconnected_joysticks; ++index)
		{
			const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
			if (!ContainsInstance(baseline_instances, instance))
			{
				reconnected_index = index;
				++reconnected_count;
			}
		}
		if (reconnected_count != 1 || reconnected_index < 0 ||
		    SDL_IsGameController(reconnected_index) != SDL_TRUE)
		{
			return finish(1, "hotplug-reenumeration-ambiguous");
		}
		const SDL_JoystickID reconnected_instance =
		    SDL_JoystickGetDeviceInstanceID(reconnected_index);
		if (reconnected_instance == expected_instance)
		{
			return finish(1, "hotplug-instance-not-renewed");
		}
		std::printf("%s phase=hotplug-scan old_instance=%d new_instance=%d index=%d\n", kMarker,
		            expected_instance, reconnected_instance, reconnected_index);
		std::fflush(stdout);

		int discarded_added_index  = -1;
		int discarded_added_events = 0;
		added_event_error.clear();
		if (!TakeControllerAddedEvent(reconnected_instance, &discarded_added_index, &discarded_added_events,
		                              added_event_error))
		{
			return finish(1, "consumer-fallback-added-read-failed");
		}
		if (discarded_added_events != 1 || discarded_added_index != reconnected_index ||
		    SDL_GameControllerFromInstanceID(reconnected_instance) != nullptr)
		{
			return finish(1, "consumer-fallback-precondition-failed");
		}

		const int opens_before_fallback = consumer_probe.open_count;
		const int notifications_before_fallback = consumer_probe.notification_count;
		const int fallback_opened = Lumen::Libs::Graphics::ReconcileUnopenedGameControllers(
		    Lumen::Libs::Graphics::GameControllerOpenOrigin::RuntimeFallback, RecordConsumerOpen,
		    &consumer_probe);
		game_controller = consumer_probe.last_controller;
		if (fallback_opened != 1 || consumer_probe.open_count != opens_before_fallback + 1 ||
		    consumer_probe.notification_count != notifications_before_fallback + 1 ||
		    consumer_probe.last_instance != reconnected_instance || game_controller == nullptr ||
		    SDL_GameControllerFromInstanceID(reconnected_instance) != game_controller ||
		    SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(game_controller)) != reconnected_instance)
		{
			return finish(1, "consumer-fallback-open-failed");
		}
		LogPhase("consumer-fallback-open");

		const auto stale_added = Lumen::Libs::Graphics::EnsureGameControllerOpen(
		    discarded_added_index, Lumen::Libs::Graphics::GameControllerOpenOrigin::DeviceAddedEvent,
		    RecordConsumerOpen, &consumer_probe);
		const int fallback_duplicate = Lumen::Libs::Graphics::ReconcileUnopenedGameControllers(
		    Lumen::Libs::Graphics::GameControllerOpenOrigin::RuntimeFallback, RecordConsumerOpen,
		    &consumer_probe);
		if (stale_added.status != Lumen::Libs::Graphics::GameControllerOpenStatus::AlreadyOpen ||
		    fallback_duplicate != 0 || consumer_probe.open_count != opens_before_fallback + 1 ||
		    consumer_probe.notification_count != notifications_before_fallback + 1)
		{
			return finish(1, "consumer-fallback-not-idempotent");
		}
		SDL_GameControllerUpdate();
		SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

		LogPhase("hotplug-reconnected-input");
		if (!TapVirtualButton(virtual_controller, GCInputButtonY, kButtonHold, input_error))
		{
			return finish(1, "hotplug-reconnected-input-failed");
		}
		std::this_thread::sleep_for(kPostInputRunLoop);
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error) ||
		    !HasExactButtonSequence(button_events, reconnected_instance,
		                            {{SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_Y},
		                             {SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_Y}}) ||
		    HasButtonEventForInstance(button_events, expected_instance))
		{
			LogButtonEvents("hotplug-reconnected-event", button_events);
			return finish(1, "hotplug-reconnected-input-sequence-failed");
		}

		LogPhase("hotplug-duplicate-check");
		SDL_GameControllerUpdate();
		button_events.clear();
		if (!ReadButtonEvents(button_events, event_error) ||
		    HasButtonEventForInstance(button_events, reconnected_instance))
		{
			LogButtonEvents("hotplug-duplicate", button_events);
			return finish(1, "hotplug-duplicate-button-event");
		}

		/* Keep the stable result token consumed by the standalone runner; the
		 * preceding phase markers carry the expanded contract. */
		return finish(0, "ordered-a-down-up");
	}

	return finish(2, "requires-ios-17");
}

} // namespace

#if TARGET_OS_IOS && TARGET_OS_SIMULATOR
@interface LumenMFIControllerDeliveryAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@property(nonatomic) BOOL testStarted;
@end

@implementation LumenMFIControllerDeliveryAppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
	(void)application;
	(void)launchOptions;
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	self.window.rootViewController = [[UIViewController alloc] init];
	self.window.backgroundColor = UIColor.blackColor;
	[self.window makeKeyAndVisible];
	return YES;
}

- (void)applicationDidBecomeActive:(UIApplication*)application
{
	(void)application;
	if (self.testStarted)
	{
		return;
	}
	self.testStarted = YES;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		const int result = RunTest();
		dispatch_async(dispatch_get_main_queue(), ^{
			std::fflush(stdout);
			std::fflush(stderr);
			std::exit(result);
		});
	});
}

@end

int main(int argc, char** argv)
{
	@autoreleasepool
	{
		return UIApplicationMain(argc, argv, nil,
		                         NSStringFromClass([LumenMFIControllerDeliveryAppDelegate class]));
	}
}
#else
int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	std::printf("%s result=unsupported reason=requires-ios-simulator cleanup=clean\n", kMarker);
	std::fflush(stdout);
	return 2;
}
#endif
