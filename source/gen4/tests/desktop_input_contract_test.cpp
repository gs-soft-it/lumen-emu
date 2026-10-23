// SPDX-License-Identifier: GPL-2.0-or-later
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "input/controller.h"
#include "input/input_handler.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
using Button = Libraries::Pad::OrbisPadButtonDataOffset;

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path);
    CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}

void Write(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::trunc);
    output << contents;
    CHECK(output.good());
}

Button Buttons() {
    Input::State state;
    bool connected;
    int count;
    (*Common::Singleton<Input::GameControllers>::Instance())[0]->ReadState(
        &state, &connected, &count);
    return state.buttonsState;
}

void Key(SDL_Keycode key, bool down) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = key;
    event.key.down = down;
    if (Input::UpdatePressedKeys(Input::InputBinding::GetInputEventFromSDLEvent(event))) {
        Input::ActivateOutputsFromInputs();
    }
}

void ExpectKey(SDL_Keycode key, Button expected) {
    Key(key, true);
    CHECK(Buttons() == expected);
    Key(key, false);
    CHECK(Buttons() == Button::None);
}
} // namespace

int main() {
    alarm(10);
    char temporary[] = "/tmp/lumen-desktop-input-XXXXXX";
    const auto* directory = mkdtemp(temporary);
    CHECK(directory != nullptr);
    const std::filesystem::path root{directory};
    Common::FS::InitializeUserPaths(root);
    EmulatorSettingsImpl::SetInstance(std::make_shared<EmulatorSettingsImpl>());
    EmulatorSettings.SetUseUnifiedInputConfig(false);
    Common::Log::Setup("desktop-input-contract.log");
    CHECK(SDL_Init(SDL_INIT_EVENTS));

    Input::ParseInputConfig("fresh");
    const auto config = root / "input_config";
    const auto defaults = Read(config / "default.ini");
    CHECK(Read(config / "fresh.ini") == defaults);
    ExpectKey(SDLK_RETURN, Button::Cross);
    ExpectKey(SDLK_Z, Button::Cross);
    ExpectKey(SDLK_N, Button::Cross);
    ExpectKey(SDLK_KP_2, Button::Cross);
    ExpectKey(SDLK_X, Button::Circle);
    ExpectKey(SDLK_TAB, Button::Options);
    ExpectKey(SDLK_BACKSPACE, Button::TouchPad);
    ExpectKey(SDLK_UP, Button::Up);
    ExpectKey(SDLK_R, Button::L3);

    // The same output remains held while another bound keyboard key is down.
    Key(SDLK_RETURN, true);
    Key(SDLK_Z, true);
    Key(SDLK_RETURN, false);
    CHECK(Buttons() == Button::Cross);
    Key(SDLK_Z, false);
    CHECK(Buttons() == Button::None);

    // Focus loss clears keyboard state without releasing a physical input.
    Input::InputEvent pad{Input::InputType::Controller, SDL_GAMEPAD_BUTTON_NORTH, true};
    CHECK(Input::UpdatePressedKeys(pad));
    Input::ActivateOutputsFromInputs();
    Key(SDLK_Z, true);
    CHECK(Buttons() == (Button::Triangle | Button::Cross));
    Input::ReleaseKeyboardMouseInputs();
    CHECK(Buttons() == Button::Triangle);
    pad.active = false;
    CHECK(Input::UpdatePressedKeys(pad));
    Input::ActivateOutputsFromInputs();
    CHECK(Buttons() == Button::None);

    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    Key(SDLK_F10, true);
    Key(SDLK_F10, false);
    SDL_Event hotkey{};
    CHECK(SDL_PeepEvents(&hotkey, 1, SDL_GETEVENT, SDL_EVENT_TOGGLE_SIMPLE_FPS,
                        SDL_EVENT_TOGGLE_SIMPLE_FPS) == 1);
    CHECK(hotkey.type == SDL_EVENT_TOGGLE_SIMPLE_FPS);

    // Existing generated files are upgraded, while even a comment makes a
    // customized profile ineligible for replacement.
    const auto original = Read(std::filesystem::path(__FILE__).parent_path() /
                               "fixtures/input_default_v1.ini");
    Write(config / "default.ini", original);
    Write(config / "generated.ini", original);
    Input::ParseInputConfig("generated");
    CHECK(Read(config / "default.ini") == defaults);
    CHECK(Read(config / "generated.ini") == defaults);
    ExpectKey(SDLK_RETURN, Button::Cross);
    ExpectKey(SDLK_TAB, Button::Options);

    const auto custom = original + "\n# My custom layout\n";
    Write(config / "custom.ini", custom);
    Input::ParseInputConfig("custom");
    CHECK(Read(config / "custom.ini") == custom);
    ExpectKey(SDLK_RETURN, Button::Options);

    SDL_Quit();
    Common::Log::Flush();
    std::filesystem::remove_all(root);
    alarm(0);
    std::puts("desktop-input=PASS bindings/releases/focus-loss/F10/generated-migration/custom-preservation");
}
