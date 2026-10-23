// SPDX-License-Identifier: GPL-2.0-only
#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>
#include "libs/controller.cpp"

namespace Common {
struct MutexPrivate { std::mutex value; };
Mutex::Mutex(): m_mutex(std::make_unique<MutexPrivate>()) {}
Mutex::~Mutex() = default;
void Mutex::Lock() { m_mutex->value.lock(); }
void Mutex::Unlock() { m_mutex->value.unlock(); }
int DbgExitIfHandler(const char*, const char*, int) { std::abort(); }
void DbgExit(int) { std::abort(); }
}
namespace Libs::LibKernel {
uint64_t KernelGetProcessTime() { static std::atomic<uint64_t> now {0}; return ++now; }
}

// The fixture supplies only host services; state, history and lifecycle use the
// production controller. No SDL device is opened and no guest CPU is started.
extern "C" SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID) { return nullptr; }
extern "C" bool SDL_SetGamepadLED(SDL_Gamepad*, Uint8, Uint8, Uint8) { return true; }
extern "C" bool SDL_GamepadHasSensor(SDL_Gamepad*, SDL_SensorType) { return false; }
extern "C" bool SDL_SetGamepadSensorEnabled(SDL_Gamepad*, SDL_SensorType, bool) { return false; }
extern "C" const char* SDL_GetError() { return "fixture"; }
extern "C" SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad*) { return SDL_GAMEPAD_TYPE_UNKNOWN; }
extern "C" bool SDL_SendGamepadEffect(SDL_Gamepad*, const void*, int) { return false; }
extern "C" bool SDL_RumbleGamepad(SDL_Gamepad*, Uint16, Uint16, Uint32) { return false; }
extern "C" void SDL_Delay(Uint32) {}
extern "C" void SDL_CloseGamepad(SDL_Gamepad*) {}
namespace Config {
const std::optional<ControllerColor>& GetControllerColor() {
 static const std::optional<ControllerColor> color;
 return color;
}
}
namespace Log { bool IsSilent() { return true; } }
namespace Libs::Controller::DualSenseHaptics { void Shutdown() {} }

int main() {
 using namespace Libs::Controller;
 SetVirtualButton(PAD_BUTTON_CROSS, true);
 SetVirtualAxis(Axis::LeftX, 0);
 ResetVirtualInputState(); // no controller initialized: all are harmless
 Initialize();
 auto& controller = *g_controller;
 controller.Connect(7);
 auto read = [&] { ControllerState state; bool flag; int count; controller.ReadState(&state, &flag, &count); return state; };
 controller.Button(7, PAD_BUTTON_CROSS, true);
 controller.Axis(7, Axis::LeftX, 205);
 controller.Axis(7, Axis::TriggerLeft, 71);
 SetVirtualButton(PAD_BUTTON_CROSS, true);
 SetVirtualButton(PAD_BUTTON_TRIANGLE, true);
 SetVirtualAxis(Axis::LeftX, 30);
 SetVirtualAxis(Axis::TriggerLeft, 255);
 assert(read().axes[0] == 30 && read().axes[4] == 255);
 SetVirtualButton(PAD_BUTTON_CROSS, false);
 assert((read().buttons & PAD_BUTTON_CROSS) != 0);
 ResetVirtualInputState();
 assert(read().buttons == (PAD_BUTTON_CROSS | PAD_BUTTON_L2));
 assert(read().axes[0] == 205 && read().axes[4] == 71);
 SetVirtualButton(PAD_BUTTON_SQUARE, true);
 controller.Button(7, PAD_BUTTON_SQUARE, true);
 controller.Button(7, PAD_BUTTON_SQUARE, false);
 assert((read().buttons & PAD_BUTTON_SQUARE) != 0);
 SetVirtualAxis(Axis::RightX, 255);
 SetVirtualAxis(Axis::RightX, 128);
 assert(read().axes[2] == 128);
 SetVirtualAxis(Axis::LeftX, -1);
 SetVirtualAxis(Axis::LeftX, 256);
 SetVirtualAxis(static_cast<Axis>(-1), 0);
 SetVirtualAxis(Axis::AxisMax, 0);
 assert(read().axes[0] == 205);
 std::thread ui([] { for (int i=0; i<10000; ++i) { SetVirtualButton(PAD_BUTTON_CIRCLE, true); SetVirtualAxis(Axis::RightY, i & 255); ResetVirtualInputState(); } });
 std::thread host([&] { for (int i=0; i<10000; ++i) { controller.Axis(7, Axis::LeftY, i & 255); (void)read(); } });
 ui.join(); host.join();
 assert((read().buttons & PAD_BUTTON_CROSS) != 0 && read().axes[0] == 205);
 ResetVirtualInputState();
 controller.ResetInputState();
 SetVirtualButton(PAD_BUTTON_OPTIONS, true);
 SetVirtualButton(PAD_BUTTON_OPTIONS, false);
 ControllerState history[64]; bool connected; int count;
 const int samples = controller.ReadStates(history, 64, &connected, &count);
 assert(connected && samples == 3);
 assert(history[0].buttons == 0);
 assert(history[1].buttons == PAD_BUTTON_OPTIONS && history[2].buttons == 0);
 // Hot-plug of a physical pad must not discard the separate touch source.
 SetVirtualButton(PAD_BUTTON_TRIANGLE, true);
 controller.Disconnect(7);
 controller.Connect(8);
 assert((read().buttons & PAD_BUTTON_TRIANGLE) != 0);
 Shutdown();
 SetVirtualButton(PAD_BUTTON_CROSS, false);
 ResetVirtualInputState();
 std::puts("PASS: virtual/physical button overlap, axis restoration, trigger merge, invalid axis rejection, absent core and concurrent UI/host state, ordered button history, hot-plug and lifecycle");
}
