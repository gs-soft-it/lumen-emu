#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_CONTROLLER_HOTPLUG_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_CONTROLLER_HOTPLUG_H_

#include "SDL_gamecontroller.h"

namespace Lumen::Libs::Graphics {

enum class GameControllerOpenOrigin
{
	InitialScan,
	DeviceAddedEvent,
	RuntimeFallback,
};

enum class GameControllerOpenStatus
{
	Opened,
	AlreadyOpen,
	NotGameController,
	InvalidInstance,
	OpenFailed,
};

struct OpenedGameController
{
	SDL_GameController*      controller   = nullptr;
	SDL_JoystickID           instance_id  = -1;
	int                      device_index = -1;
	GameControllerOpenOrigin origin       = GameControllerOpenOrigin::InitialScan;
};

struct GameControllerOpenResult
{
	SDL_GameController*      controller   = nullptr;
	SDL_JoystickID           instance_id  = -1;
	int                      device_index = -1;
	GameControllerOpenOrigin origin       = GameControllerOpenOrigin::InitialScan;
	GameControllerOpenStatus status       = GameControllerOpenStatus::OpenFailed;
};

using GameControllerOpenedSink = void (*)(const OpenedGameController& opened, void* context);

// Opens one SDL game controller at most once. The sink is invoked synchronously
// only after a new SDL_GameControllerOpen succeeds; SDL remains the owner of
// discovery, mapping, instance IDs and device lifetime events.
GameControllerOpenResult EnsureGameControllerOpen(int device_index, GameControllerOpenOrigin origin, GameControllerOpenedSink sink,
                                                  void* context);

// Reconciles SDL's authoritative device snapshot with controllers that the
// application consumer has already opened. Returns the number of new opens.
int ReconcileUnopenedGameControllers(GameControllerOpenOrigin origin, GameControllerOpenedSink sink, void* context);

[[nodiscard]] bool        GameControllerOpenShouldNotify(GameControllerOpenOrigin origin);
[[nodiscard]] const char* GameControllerOpenOriginName(GameControllerOpenOrigin origin);

} // namespace Lumen::Libs::Graphics

#endif // EMULATOR_INCLUDE_EMULATOR_GRAPHICS_CONTROLLER_HOTPLUG_H_
