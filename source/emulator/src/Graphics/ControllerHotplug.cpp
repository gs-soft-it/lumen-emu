#include "Emulator/Graphics/ControllerHotplug.h"

#include "SDL_error.h"
#include "SDL_joystick.h"

namespace Lumen::Libs::Graphics {

GameControllerOpenResult EnsureGameControllerOpen(int device_index, GameControllerOpenOrigin origin, GameControllerOpenedSink sink,
                                                  void* context)
{
	GameControllerOpenResult result;
	result.device_index = device_index;
	result.origin       = origin;

	if (SDL_IsGameController(device_index) != SDL_TRUE)
	{
		result.status = GameControllerOpenStatus::NotGameController;
		return result;
	}

	result.instance_id = SDL_JoystickGetDeviceInstanceID(device_index);
	if (result.instance_id < 0)
	{
		result.status = GameControllerOpenStatus::InvalidInstance;
		return result;
	}

	result.controller = SDL_GameControllerFromInstanceID(result.instance_id);
	if (result.controller != nullptr)
	{
		result.status = GameControllerOpenStatus::AlreadyOpen;
		return result;
	}

	SDL_ClearError();
	result.controller = SDL_GameControllerOpen(device_index);
	if (result.controller == nullptr)
	{
		result.status = GameControllerOpenStatus::OpenFailed;
		return result;
	}

	result.status = GameControllerOpenStatus::Opened;
	if (sink != nullptr)
	{
		const OpenedGameController opened = {
		    result.controller,
		    result.instance_id,
		    result.device_index,
		    result.origin,
		};
		sink(opened, context);
	}
	return result;
}

int ReconcileUnopenedGameControllers(GameControllerOpenOrigin origin, GameControllerOpenedSink sink, void* context)
{
	int       opened_count   = 0;
	const int joystick_count = SDL_NumJoysticks();
	for (int device_index = 0; device_index < joystick_count; ++device_index)
	{
		const auto result = EnsureGameControllerOpen(device_index, origin, sink, context);
		if (result.status == GameControllerOpenStatus::Opened)
		{
			++opened_count;
		}
	}
	return opened_count;
}

bool GameControllerOpenShouldNotify(GameControllerOpenOrigin origin)
{
	return origin != GameControllerOpenOrigin::InitialScan;
}

const char* GameControllerOpenOriginName(GameControllerOpenOrigin origin)
{
	switch (origin)
	{
		case GameControllerOpenOrigin::InitialScan: return "initial";
		case GameControllerOpenOrigin::DeviceAddedEvent: return "device-added-event";
		case GameControllerOpenOrigin::RuntimeFallback: return "runtime-fallback";
	}
	return "unknown";
}

} // namespace Lumen::Libs::Graphics
