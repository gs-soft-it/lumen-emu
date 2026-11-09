// SPDX-License-Identifier: GPL-2.0-only
#include "../../../frontend/ios/FexRuntimeModule.h"
#include "../../../frontend/ios/RuntimeDefaults.h"
#include "../core/fex/darwin_jit.h"

#include <CoreFoundation/CoreFoundation.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <string_view>

extern "C" void LumenMetalSetPresentCompletionCallback(void (*callback)(void*), void* context);

#if defined(LUMEN_IOS_CONSOLE_PS4)
#include "imgui/renderer/imgui_core.h"
#include "lumen/session.h"
#include "lumen/succi_benchmark.h"
#else
int LumenIOSGen5Run(const char* root);
#endif

namespace {
std::atomic<bool> launched{};
std::atomic<uint64_t> frames{};
std::mutex jitProbeMutex;
LumenIOSFexModuleHostV1 host{};
SDL_Joystick* virtual_pad{};
SDL_JoystickID virtual_pad_id{};
char diagnostic[512] = "FEX runtime has not been probed.";
struct CurrentDirectory {
    std::filesystem::path previous = std::filesystem::current_path();
    ~CurrentDirectory() {
        std::error_code error;
        std::filesystem::current_path(previous, error);
    }
};
void Message(const char* value) {
    std::snprintf(diagnostic, sizeof(diagnostic), "%s", value);
}
std::filesystem::path Directory(const char* value) {
    if (!value || !*value) throw std::invalid_argument("A runtime directory is missing.");
    const auto path = std::filesystem::path(value);
    if (!path.is_absolute() || std::filesystem::is_symlink(std::filesystem::symlink_status(path)) ||
        !std::filesystem::is_directory(path))
        throw std::invalid_argument("Runtime directories must be absolute real directories.");
    return std::filesystem::canonical(path);
}
void PhysicalControllerState() {
    int count = 0;
    auto* ids = SDL_GetGamepads(&count);
    bool connected = false;
    for (int index = 0; ids && index < count; ++index) {
        if (ids[index] == virtual_pad_id) continue;
        if (auto* gamepad = SDL_OpenGamepad(ids[index])) {
            connected = true;
            SDL_CloseGamepad(gamepad);
        }
    }
    SDL_free(ids);
    if (host.physicalControllerChanged)
        host.physicalControllerChanged(host.context, connected);
}
bool ControllerEvent(void*, SDL_Event* event) {
    if (event->type == SDL_EVENT_GAMEPAD_ADDED || event->type == SDL_EVENT_GAMEPAD_REMOVED)
        PhysicalControllerState();
#if defined(LUMEN_IOS_CONSOLE_PS4)
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_WINDOW_FOCUS_LOST)
        Lumen::Gen4::SucciBenchmark::SetActive(false);
    else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND || event->type == SDL_EVENT_WINDOW_FOCUS_GAINED)
        Lumen::Gen4::SucciBenchmark::SetActive(true);
#endif
    return true;
}
void Presented(void*) {
    const auto number = frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (host.framePresented) host.framePresented(host.context, number);
}

int ProbeJit(bool allowPrepared = false) {
    const auto error = Core::Fex::ProbeDarwinJit(allowPrepared);
    std::printf("LUMEN_IOS_FEX_JIT_PROBE console=%u mode=%s error=%d\n",
        LumenIOSFexModuleConsoleV1(),
        FEXCore::Allocator::IsDebuggerJit() ? "debugger-rx-rw" : "map-jit", error);
    std::fflush(stdout);
    Message(error == 0 ? "FEX JIT is available; sessions require a fresh app process."
                       : Core::Fex::DarwinJitFailureDescription(error));
    return error;
}
}

#define LUMEN_IOS_EXPORT extern "C" __attribute__((visibility("default")))
LUMEN_IOS_EXPORT uint32_t LumenIOSFexModuleAbiV1() { return 1; }
LUMEN_IOS_EXPORT uint32_t LumenIOSFexModuleConsoleV1() {
#if defined(LUMEN_IOS_CONSOLE_PS4)
    return 4;
#else
    return 5;
#endif
}
#if defined(LUMEN_IOS_CONSOLE_PS4)
LUMEN_IOS_EXPORT uint32_t LumenIOSFexModuleImeActiveV1() {
    return ImGui::Core::IsGamepadInputCaptured() ? 1u : 0u;
}
#endif
LUMEN_IOS_EXPORT uint32_t LumenIOSFexModuleSetHostV1(const LumenIOSFexModuleHostV1* value) {
    if (!value || value->structSize != sizeof(*value) || value->abiVersion != 1 ||
        !value->windowReady || launched.load(std::memory_order_acquire)) return EINVAL;
    host = *value;
    return 0;
}
LUMEN_IOS_EXPORT uint32_t LumenIOSFexModuleProbeV1() {
    std::lock_guard lock(jitProbeMutex);
    return static_cast<uint32_t>(ProbeJit());
}

LUMEN_IOS_EXPORT uint32_t LumenIOSFexModulePrepareJitV1(uint32_t provider) {
    std::lock_guard lock(jitProbeMutex);
    if (provider != LumenIOSFexJitProviderUniversalV1) {
        Message("The JIT provider is unsupported.");
        return EINVAL;
    }
    if (launched.load(std::memory_order_acquire)) {
        Message("Restart Lumen before preparing JIT for another session.");
        return EBUSY;
    }
    if (pthread_main_np()) {
        Message("JIT preparation requires the host's background queue.");
        return EWOULDBLOCK;
    }
    if (FEXCore::Allocator::IsDebuggerJitReady()) return static_cast<uint32_t>(ProbeJit());

    auto error = FEXCore::Allocator::InitializeDebuggerJit(
        FEXCore::Allocator::DebuggerJitProvider::Universal);
    const char* stage = "prepare";
    if (error == 0) {
        stage = "execute-before-detach";
        error = ProbeJit(true);
    }
    // Close only a universal session owned by this module. This is also safe
    // after an early failure: no breakpoint is emitted without that ownership
    // and a freshly observed debugger attachment.
    const auto detachError = FEXCore::Allocator::DetachUniversalDebuggerJit();
    if (error == 0 && detachError != 0) {
        stage = "detach";
        error = detachError;
    }
    if (error == 0) {
        stage = "execute-after-detach";
        error = ProbeJit(true);
    }
    if (error == 0) {
        stage = "publish-ready";
        error = FEXCore::Allocator::CompleteUniversalDebuggerJit();
    }
    std::printf("LUMEN_IOS_FEX_JIT_PREPARE console=%u provider=universal stage=%s error=%d\n",
        LumenIOSFexModuleConsoleV1(), stage, error);
    std::fflush(stdout);
    if (error != 0) {
        std::snprintf(diagnostic, sizeof(diagnostic), "Universal JIT %s failed (%d): %s",
            stage, error, Core::Fex::DarwinJitFailureDescription(error));
    } else {
        Message("FEX JIT is ready after universal preparation and detachment.");
    }
    return static_cast<uint32_t>(error);
}

LUMEN_IOS_EXPORT int LumenIOSFexModuleLaunchV1(const char* root, const char* data,
                                            const char* profile) {
    try {
        if (!pthread_main_np()) throw std::invalid_argument("FEX launch requires the main thread.");
        if (!profile) throw std::invalid_argument("The runtime profile is unsupported.");
        const std::string_view requested_profile{profile};
#if defined(LUMEN_IOS_CONSOLE_PS4)
        const bool bundled_demo = requested_profile == "succi-demo-v2";
#else
        constexpr bool bundled_demo = false;
#endif
        if (requested_profile != "eboot-only" && requested_profile != "known-system" && !bundled_demo)
            throw std::invalid_argument("The runtime profile is unsupported.");
        if (!host.windowReady) throw std::invalid_argument("The UIKit host services are missing.");
        const auto content = Directory(root);
        const auto writable = Directory(data);
        if (content == writable) throw std::invalid_argument("Runtime data must be separate from ROM content.");
        if (const auto status = LumenIOSFexModuleProbeV1(); status != 0) return static_cast<int>(status);
        if (launched.exchange(true)) {
            Message("Restart Lumen before starting another game.");
            return EALREADY;
        }
        // Both engines use relative paths for their private runtime data.
        const CurrentDirectory restore_directory;
        std::filesystem::current_path(writable);
        SDL_SetMainReady();
        // SDL's resizable-window default admits portrait even for a landscape
        // scene, and its keyboard layout can then restore portrait bounds.
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        LumenMetalSetPresentCompletionCallback(Presented, nullptr);
        if (host.progress) host.progress(host.context, 200, "Starting FEX runtime");
#if defined(LUMEN_IOS_CONSOLE_PS4)
        Lumen::Gen4::SessionOptions options;
        options.root = content;
        options.data = writable;
        options.known_system = requested_profile == "known-system" || bundled_demo;
        options.width = LumenIOSStandardRuntimeWidth;
        options.height = LumenIOSStandardRuntimeHeight;
        // The bridge selects the bundled route explicitly. A title identifier,
        // ROM name or an ordinary known-system launch never enables this mode.
        // Reuse Android's admission of the exact isolated ROM and data paths;
        // ValidateContent also verifies the executable digest from its marker.
        if (bundled_demo)
            Lumen::Gen4::SucciBenchmark::ValidateLaunch(options, writable.parent_path().parent_path());
        std::string error;
        if (!Lumen::Gen4::ValidateContent(options, error)) throw std::runtime_error(error);
        // Enable the shared PS4 mixer before the guest can latch its selector.
        if (setenv("LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER", "1", 0) != 0)
            throw std::runtime_error("Could not configure the PS4 NGS2 mixer.");
        return Lumen::Gen4::RunSession(options);
#else
        const auto result = LumenIOSGen5Run(content.c_str());
        if (result != 0) Message("The PS5 FEX runtime could not initialize. Restart Lumen before retrying.");
        return result;
#endif
    } catch (const std::exception& error) {
        Message(error.what());
        return EINVAL;
    }
}

LUMEN_IOS_EXPORT void LumenIOSFexModuleRequestStopV1() {
#if defined(LUMEN_IOS_CONSOLE_PS4)
    Lumen::Gen4::SucciBenchmark::Cancel();
#endif
    Message("This FEX session cannot reset safely. Close and restart Lumen to leave the game.");
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

LUMEN_IOS_EXPORT void LumenIOSFexModuleSetButtonV1(uint32_t button, uint32_t pressed) {
    constexpr SDL_GamepadButton mapping[] {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_TOUCHPAD};
    if (virtual_pad && button < std::size(mapping))
        SDL_SetJoystickVirtualButton(virtual_pad, mapping[button], pressed != 0);
}

LUMEN_IOS_EXPORT void LumenIOSFexModuleSetAxisV1(uint32_t axis, uint32_t value) {
    if (!virtual_pad || axis >= SDL_GAMEPAD_AXIS_COUNT || value > 255) return;
    const auto signed_value = axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER
        ? static_cast<int>(value * 257) - 32768
        : value < 128 ? (static_cast<int>(value) - 128) * 256
                      : (static_cast<int>(value) - 128) * 32767 / 127;
    SDL_SetJoystickVirtualAxis(virtual_pad, axis, static_cast<Sint16>(signed_value));
}

extern "C" void LumenIOSFexModuleWindowReadyV1(SDL_Window* window) {
    if (!pthread_main_np() || !host.windowReady)
        throw std::runtime_error("The FEX runtime surface requires a UIKit host on the main thread.");
    auto* native = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (!native || host.windowReady(host.context, native) != 0)
        throw std::runtime_error("The UIKit host rejected the FEX runtime window.");
    // The host must see the hidden window while preparing its scene and opaque
    // surface. Synchronize SDL's visibility only after UIKit accepts the handoff.
    SDL_ShowWindow(window);
    SDL_PumpEvents();
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    SDL_VirtualJoystickDesc description{};
    description.version = sizeof(description);
    description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    description.naxes = SDL_GAMEPAD_AXIS_COUNT;
    description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    description.name = "Lumen virtual DualShock";
    virtual_pad_id = SDL_AttachVirtualJoystick(&description);
    virtual_pad = SDL_OpenJoystick(virtual_pad_id);
    if (!virtual_pad) throw std::runtime_error(SDL_GetError());
    for (uint32_t axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
        LumenIOSFexModuleSetAxisV1(axis, axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : 128);
    SDL_AddEventWatch(ControllerEvent, nullptr);
    PhysicalControllerState();
    if (host.progress) host.progress(host.context, 500, "FEX runtime surface ready");
}

extern "C" void LumenIOSFexModuleExecutionStartedV1() {
    if (host.progress) host.progress(host.context, 850, "FEX runtime running");
}

LUMEN_IOS_EXPORT const char* LumenIOSFexModuleDiagnosticV1() { return diagnostic; }

// Direct runtime workers have process lifetime. Never unwind engine owners,
// release the surface, or return to the dashboard while those workers live.
extern "C" [[noreturn]] void LumenIOSFexModuleParkV1() {
    Message("Session finished. Close and restart Lumen before starting another game.");
    for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
}
