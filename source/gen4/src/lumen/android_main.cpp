// SPDX-License-Identifier: GPL-2.0-only
#include "lumen/session.h"
#include "lumen/succi_benchmark.h"
#include "video_core/renderer_vulkan/bounded_draw_diagnostics.h"
#include "AndroidVulkanDriver.h"
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_error.h>
#include <android/log.h>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>

extern "C" __attribute__((visibility("default"))) int LumenAndroidGen4MainV1(int argc, char** argv) {
    try {
        if ((argc != 10 && argc != 11) || !argv) throw std::runtime_error("Invalid Android PS4 launch arguments");
        for (int i = 1; i < argc; ++i) if (!argv[i]) throw std::runtime_error("Missing Android PS4 launch argument");
        Lumen::Gen4::SessionOptions options;
        options.root = argv[1]; options.data = argv[2];
        const auto dimension = [](const char* text, unsigned& number, unsigned minimum, unsigned maximum) {
            const auto length = std::strlen(text);
            const auto [end, error] = std::from_chars(text, text + length, number);
            return error == std::errc{} && end == text + length && number >= minimum && number <= maximum;
        };
        if (!dimension(argv[3], options.width, 320, 7680) || !dimension(argv[4], options.height, 240, 4320))
            throw std::runtime_error("Invalid Android PS4 dimensions");
        if ((std::strcmp(argv[5], "0") && std::strcmp(argv[5], "1")) || std::strcmp(argv[6], "0"))
            throw std::runtime_error("The new PS4 runtime does not support inherited experimental launch profiles");
        if (std::strcmp(argv[7], "eboot-only") && std::strcmp(argv[7], "known-system"))
            throw std::runtime_error("Invalid PS4 module policy");
        options.known_system = std::strcmp(argv[7], "known-system") == 0;
        const bool diagnostics = std::strcmp(argv[5], "1") == 0;
        Vulkan::DrawDiagnostic::Configure(diagnostics);
        options.shader_dump = diagnostics;
        if (argc == 11) {
            if (std::strcmp(argv[10], "succi-demo-v2") != 0)
                throw std::runtime_error("Invalid Android PS4 benchmark mode");
            const char* storage = SDL_GetAndroidInternalStoragePath();
            Lumen::Gen4::SucciBenchmark::ValidateLaunch(options, storage ? storage : "");
        }
        std::string error;
        if (!Lumen::Gen4::ValidateContent(options, error)) throw std::runtime_error(error);
        if (const auto* enabled = std::getenv("LUMEN_HOST_CRASH_REGISTERS");
            enabled && std::strcmp(enabled, "1") == 0) {
            const auto path = options.data / "android-runtime-crash.log";
            const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) throw std::runtime_error("Cannot open runtime crash diagnostics");
            std::fflush(stdout);
            std::fflush(stderr);
            const bool redirected = dup2(fd, STDOUT_FILENO) >= 0 && dup2(fd, STDERR_FILENO) >= 0;
            close(fd);
            if (!redirected) throw std::runtime_error("Cannot redirect runtime crash diagnostics");
            std::setvbuf(stdout, nullptr, _IOLBF, 0);
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            __android_log_print(ANDROID_LOG_INFO, "LumenGen4", "Crash diagnostics: %s", path.c_str());
        }
        const LumenAndroidVulkanHost host{SDL_GetAndroidInternalStoragePath, SDL_GetAndroidJNIEnv, SDL_GetAndroidActivity};
        if (!LumenAndroidConfigureVulkanDriver("PS4", argv[8], argv[9], host))
            throw std::runtime_error(LumenAndroidVulkanDriverError());
        return Lumen::Gen4::RunSession(options);
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_ERROR, "LumenGen4", "PS4 launch failed: %s", error.what());
        return 1;
    }
}
extern "C" void* LumenGen4SDL_LoadObject(const char* path) {
    return LumenAndroidVulkanLoadObject(path);
}
extern "C" void* LumenGen4SDL_LoadFunction(void* object, const char* name) {
    return LumenAndroidVulkanLoadFunction(object, name);
}
extern "C" void LumenGen4SDL_UnloadObject(void* object) { LumenAndroidVulkanUnloadObject(object); }
