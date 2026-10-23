// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/session.h"
#include "lumen/succi_benchmark.h"
#include "lumen/fex_profile.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "Emulator/Runtime.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#if defined(__APPLE__)
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
extern "C" void LumenGen4OnRuntimeReady(bool ready);
#endif

namespace Lumen::Gen4 {
namespace { std::atomic<bool> known_system_modules{}; }
bool UseKnownSystemModules() { return known_system_modules.load(std::memory_order_acquire); }
int RunSession(const SessionOptions& options, const std::function<void(bool, int)>& lifecycle) {
#if defined(__APPLE__) && defined(LUMEN_GEN4_DIRECT_FEX)
    ConfigureDesktopFexProfile();
#endif
    known_system_modules.store(options.known_system, std::memory_order_release);
    if (options.succi_benchmark) SucciBenchmark::Initialize(options.root, options.data);
    Common::FS::InitializeUserPaths(options.data);
    auto state = std::make_shared<EmulatorState>();
    state->SetAutoPatchesLoadEnabled(false);
    EmulatorState::SetInstance(state);
    auto settings = std::make_shared<EmulatorSettingsImpl>();
    EmulatorSettingsImpl::SetInstance(settings);
    settings->SetConfigMode(ConfigMode::Global);
#ifdef LUMEN_GEN4_DIRECT_FEX
    settings->SetLogSync(false);
    settings->SetCopyGpuBuffers(true);
    settings->SetPipelineCacheEnabled(true);
#endif
    settings->SetDumpShaders(options.shader_dump);
#ifndef __ANDROID__
    settings->SetShowFpsCounter(true);
#endif
    settings->SetWindowWidth(options.width);
    settings->SetWindowHeight(options.height);
    settings->SetSysModulesDir(options.root / "sce_module");
    Common::Log::Setup("lumen-startup.log");
    UserSettings.Load();
#ifdef __ANDROID__
    UserManagement.LoginUser(UserManagement.GetUserByPlayerIndex(1), 1);
    if (auto* user = UserManagement.GetUserByPlayerIndex(1)) {
        auto* controllers = Common::Singleton<Input::GameControllers>::Instance();
        (*controllers)[0]->user_id = user->user_id;
        (*controllers)[0]->ConnectController(nullptr);
    }
#endif
    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
#if defined(__APPLE__)
    if (!options.executable.empty()) {
        emulator->onRuntimeRestart = [options](const std::filesystem::path& entry,
                                               const std::vector<std::string>& arguments) {
            auto next = options;
            next.entry = entry.lexically_relative(options.root);
            next.guest_args = arguments;
            std::string error;
            if (arguments.size() > 33 || !ValidateEntry(next, next.entry, error)) return false;
            for (const auto& argument : arguments) if (argument.size() > 4096) return false;
            auto values = ManagedArguments(next);
            std::vector<char*> argv;
            for (auto& value : values) argv.push_back(value.data());
            argv.push_back(nullptr);
            Common::Log::Flush();
            // exec keeps the supervised PID and its stdout protocol. A failed
            // exec leaves this session alive and reports failure to the guest.
            execv(values.front().c_str(), argv.data());
            return false;
        };
    }
#endif
    emulator->onRuntimeError = [&](std::string_view error) {
        EmitEvent(options, "SCRIPT_ERROR");
        std::fprintf(stderr, "PS4 runtime: %.*s\n", static_cast<int>(error.size()), error.data());
    };
    emulator->onRuntimeRunning = [&] {
        EmitEvent(options, "RUNNING");
        if (lifecycle) lifecycle(true, 0);
#ifdef __ANDROID__
        LumenGen4OnRuntimeReady(true);
#endif
    };
    emulator->onRuntimeStopped = [&](int status) {
        EmitEvent(options, status == 0 ? "FINISHED" : "SCRIPT_ERROR");
        if (lifecycle) lifecycle(false, status);
#ifdef __ANDROID__
        LumenGen4OnRuntimeReady(false);
#endif
    };
    EmitEvent(options, "STARTING");
    emulator->Run(options.root / options.entry, options.guest_args, options.root);
    return 0;
}
}

namespace {
std::atomic<bool> launched{};
LumenRuntimeResultCodeV1 Result(LumenRuntimeResultV1* output, LumenRuntimeResultCodeV1 code, const char* text) {
    if (output) {
        *output = {};
        output->structSize = sizeof(*output);
        output->abiVersion = LUMEN_RUNTIME_ABI_VERSION_V1;
        output->code = code;
        std::snprintf(output->message, sizeof(output->message), "%s", text);
    }
    return code;
}
bool Options(const LumenRuntimeLaunchRequestV1* request, Lumen::Gen4::SessionOptions& options) {
    if (!request || request->structSize != sizeof(*request) || request->abiVersion != LUMEN_RUNTIME_ABI_VERSION_V1 ||
        !request->installPath || !request->dataPath || request->profile > LUMEN_RUNTIME_PROFILE_KNOWN_SYSTEM_V1 ||
        request->width < 320 || request->width > 7680 || request->height < 240 || request->height > 4320) return false;
    options.root = std::filesystem::u8path(request->installPath);
    options.data = std::filesystem::u8path(request->dataPath);
    options.known_system = request->profile == LUMEN_RUNTIME_PROFILE_KNOWN_SYSTEM_V1;
    options.width = request->width;
    options.height = request->height;
    return true;
}
void Notify(const LumenRuntimeLaunchRequestV1* request, LumenRuntimeEventTypeV1 type,
            unsigned progress, LumenRuntimeResultCodeV1 code, const char* text) {
    if (!request->callback) return;
    LumenRuntimeEventV1 event{};
    event.structSize = sizeof(event);
    event.abiVersion = LUMEN_RUNTIME_ABI_VERSION_V1;
    event.type = type;
    event.progressPermille = progress;
    event.code = code;
    std::snprintf(event.message, sizeof(event.message), "%s", text);
    request->callback(&event, request->callbackUserData);
}
}
extern "C" LumenRuntimeCapabilitiesV1 LumenRuntimeGetCapabilitiesV1() {
    LumenRuntimeCapabilitiesV1 capabilities{};
    capabilities.structSize = sizeof(capabilities);
    capabilities.abiVersion = LUMEN_RUNTIME_ABI_VERSION_V1;
    capabilities.cpuVirtualMemoryAvailable = capabilities.loaderAvailable = capabilities.hleAvailable = 1;
    capabilities.graphicsAvailable = capabilities.audioAvailable = capabilities.inputAvailable = 1;
    capabilities.romLaunchAvailable = capabilities.launchIsOneShot = 1;
#ifndef __ANDROID__
    capabilities.launchMustRunOnMainThread = 1;
#endif
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    capabilities.jitEnabled = 1;
#endif
    return capabilities;
}
extern "C" LumenRuntimeResultCodeV1 LumenRuntimePreflightV1(const LumenRuntimeLaunchRequestV1* request,
                                                            LumenRuntimeResultV1* output) {
    try {
        Lumen::Gen4::SessionOptions options;
        if (!Options(request, options)) return Result(output, LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1, "Invalid launch request");
        Notify(request, LUMEN_RUNTIME_EVENT_PREFLIGHT_STARTED_V1, 0, 0, "Validating PS4 content");
        std::string error;
        if (!Lumen::Gen4::ValidateContent(options, error)) {
            Notify(request, LUMEN_RUNTIME_EVENT_FAILED_V1, 0, LUMEN_RUNTIME_RESULT_PREFLIGHT_FAILED_V1, error.c_str());
            return Result(output, LUMEN_RUNTIME_RESULT_PREFLIGHT_FAILED_V1, error.c_str());
        }
        Notify(request, LUMEN_RUNTIME_EVENT_PREFLIGHT_SUCCEEDED_V1, 150, 0, "PS4 content validated");
        return Result(output, 0, "PS4 content validated");
    } catch (const std::exception& error) { return Result(output, LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1, error.what()); }
}
extern "C" LumenRuntimeResultCodeV1 LumenRuntimeLaunchV1(const LumenRuntimeLaunchRequestV1* request,
                                                         LumenRuntimeResultV1* output) {
    try {
        Lumen::Gen4::SessionOptions options;
        if (!Options(request, options)) return Result(output, LUMEN_RUNTIME_RESULT_INVALID_ARGUMENT_V1, "Invalid launch request");
#if defined(__APPLE__)
        if (!pthread_main_np()) return Result(output, LUMEN_RUNTIME_RESULT_NOT_MAIN_THREAD_V1, "Launch requires the main thread");
#endif
        std::string error;
        if (!Lumen::Gen4::ValidateContent(options, error)) return Result(output, LUMEN_RUNTIME_RESULT_PREFLIGHT_FAILED_V1, error.c_str());
        if (launched.exchange(true)) return Result(output, LUMEN_RUNTIME_RESULT_ALREADY_LAUNCHED_V1, "Use a new process for each PS4 session");
        Notify(request, LUMEN_RUNTIME_EVENT_INITIALIZATION_STARTED_V1, 200, 0, "Starting PS4 runtime");
        Lumen::Gen4::RunSession(options, [request](bool running, int status) {
            Notify(request, running ? LUMEN_RUNTIME_EVENT_EXECUTION_STARTED_V1 :
                   (status == 0 ? LUMEN_RUNTIME_EVENT_FINISHED_V1 : LUMEN_RUNTIME_EVENT_FAILED_V1),
                   running ? 850 : 1000, status == 0 ? LUMEN_RUNTIME_RESULT_OK_V1 : LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1,
                   running ? "PS4 runtime running" : "PS4 session finished");
        });
        return Result(output, 0, "PS4 session finished");
    } catch (const std::exception& error) { return Result(output, LUMEN_RUNTIME_RESULT_INTERNAL_ERROR_V1, error.what()); }
}
