// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <thread>

#include "common/singleton.h"
#include "core/linker.h"
#include "input/controller.h"
#include "sdl_window.h"

namespace Core {

using HLEInitDef = void (*)(Core::Loader::SymbolsResolver* sym);

struct SysModules {
    std::string_view module_name;
    HLEInitDef callback;
};

class Emulator {
public:
    Emulator();
    ~Emulator();

    void Run(std::filesystem::path file, std::vector<std::string> args = {},
             std::optional<std::filesystem::path> game_folder = {});
    void UpdatePlayTime(const std::string& serial);
    void Shutdown();

    bool Restart(const std::filesystem::path& eboot_path, const std::vector<std::string>& guest_args = {});
    std::function<bool(const std::filesystem::path&, const std::vector<std::string>&)> onRuntimeRestart;

    bool waitForDebuggerBeforeRun{false};
    std::function<void()> onRuntimeRunning;
    std::function<void(std::string_view)> onRuntimeError;
    std::function<void(int)> onRuntimeStopped;

private:
    void LoadSystemModules(const std::string& game_serial);

    Core::MemoryManager* memory;
    Input::GameControllers* controllers;
    Core::Linker* linker;
    std::unique_ptr<Frontend::WindowSDL> window;
    std::chrono::steady_clock::time_point start_time;
    std::jthread play_time_thread;
};

} // namespace Core
