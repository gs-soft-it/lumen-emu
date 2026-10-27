// SPDX-License-Identifier: GPL-2.0-only
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

#include "common/threads.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/window/windowInternal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
void Require(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "ANDROID_SURFACE_LIFECYCLE_TEST FAIL %s: %s\n", description,
                     SDL_GetError());
        std::abort();
    }
}
}

int main() {
    Common::InitializeThreads();
    // This is a standalone adb executable, without SDLActivity's Java bootstrap.
    SDL_SetMainReady();
    // SDL's Android executable-name fallback asks Java for the package name.
    // Supply metadata explicitly because this fixture has no JVM.
    Require(SDL_SetAppMetadata("Surface lifecycle fixture", "1", "lumen.surface.fixture"),
            "set standalone application metadata");
    Require(SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy"), "select dummy video driver");
    Require(SDL_Init(SDL_INIT_VIDEO), "initialize SDL video");
    {
        Libs::Graphics::WindowContext window;
        window.window = SDL_CreateWindow("Surface lifecycle fixture", 64, 64, 0);
        Require(window.window != nullptr, "create host window");
        const auto properties = SDL_GetWindowProperties(window.window);

        // A non-null sentinel makes the production precondition pass, but must
        // never reach Vulkan: Android has withdrawn the native-window property.
        // Before the fix the real method called SDL_Vulkan_CreateSurface and
        // terminated the process instead of reporting deferred recovery.
        window.graphic_ctx.instance = reinterpret_cast<VkInstance>(uintptr_t{1});
        Require(!window.IsPresentationSurfaceAvailable(), "missing native window is unavailable");
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            Require(!window.RecreateSurface(), "defer recreation while native window is missing");
            Require(window.surface == nullptr, "deferred recovery has no Vulkan surface");
        }

        // Exercise publication from another thread, as in Java surfaceCreated.
        // This also detects a leaked properties lock on the deferred path.
        std::atomic_bool published = false;
        int native_window_token = 0;
        std::thread publisher([&] {
            Require(SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
                                           &native_window_token), "publish replacement window");
            published.store(true, std::memory_order_release);
        });
        for (unsigned wait = 0; wait < 1000 && !published.load(std::memory_order_acquire); ++wait) {
            SDL_Delay(1);
        }
        Require(published.load(std::memory_order_acquire), "deferred recovery releases SDL properties");
        publisher.join();
        Require(window.IsPresentationSurfaceAvailable(), "replacement native window becomes available");
        Require(SDL_ClearProperty(properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER),
                "withdraw replacement window");
        Require(!window.RecreateSurface(), "defer again after another withdrawal");
        window.graphic_ctx.instance = nullptr;
    }
    std::puts("ANDROID_SURFACE_LIFECYCLE_TEST PASS missing-surface=65 publication=1 lock-release=1");
}
