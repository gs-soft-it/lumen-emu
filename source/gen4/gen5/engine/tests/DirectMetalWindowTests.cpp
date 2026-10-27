// SPDX-License-Identifier: GPL-2.0-only
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"

#include <cstdio>
#include <cstdlib>

namespace {
void Require(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "DIRECT_METAL_WINDOW_TEST FAIL %s: %s\n", description, SDL_GetError());
        std::abort();
    }
}
}

int main() {
    // A stale external-driver preference must never redirect this build.
    ::setenv("SDL_VULKAN_LIBRARY", "/nonexistent/gen5-must-use-native-metal.dylib", 1);
    Common::InitializeThreads();
    Common::Subsystems subsystems;
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions options;
    options.screen_width = 96;
    options.screen_height = 64;
    options.printf_direction = Config::LogDirection::Silent;
    options.shader_log_direction = Config::LogDirection::Silent;
    Config::Load(options);
    subsystems.Initialize<Log::Lifecycle>();
    subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
    Libs::Graphics::ShaderInit();
    Require(SDL_Init(SDL_INIT_VIDEO), "SDL video initialization");
    {
        Libs::Graphics::WindowContext window;
        window.graphic_ctx.screen_width = 96;
        window.graphic_ctx.screen_height = 64;
        const auto flags = window.InitialWindowFlags(false);
        Require((flags & SDL_WINDOW_METAL) != 0 && (flags & SDL_WINDOW_VULKAN) == 0,
                "native window flags");
        window.window = SDL_CreateWindow("Gen5 Metal verification", 96, 64, flags);
        Require(window.window != nullptr, "create window");
        window.CreateVulkan();
        Require(window.metal_view != nullptr && window.surface != nullptr &&
                window.graphic_ctx.device != nullptr && window.presenter != nullptr,
                "production Metal renderer initialization");
        auto& frame = window.presenter->PrepareBlankFrame(96, 64, true);
        window.presenter->Present(frame);
        Require(window.loop.presented_frames.load() == 1, "production frame presentation");
        Require(window.graphic_ctx.device.waitIdle() == vk::Result::eSuccess,
                "finish native presentation");
        window.presenter.reset();
        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            Require(window.RecreateSurface(), "recreate surface available");
            Require(window.RefreshSurfaceCapabilities(), "query recreated surface");
            Require(window.surface != nullptr && window.metal_view != nullptr &&
                    !window.surface_capabilities.formats.empty(), "recreate Metal surface");
        }
    }
    std::puts("DIRECT_METAL_WINDOW_TEST PASS production-init=1 present=1 recreate=3 destroy=1 external-loader=0");
}
