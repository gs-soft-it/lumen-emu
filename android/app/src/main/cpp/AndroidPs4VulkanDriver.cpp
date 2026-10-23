// Lumen SDL2 Android host integration, 2026-10-11. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidPs4VulkanDriver.h"
#include "AndroidVulkanDriver.h"
#include <SDL_error.h>
#include <SDL_system.h>

bool LumenAndroidPs4ConfigureVulkanDriver(const char* directory, const char* name) {
    const LumenAndroidVulkanHost host {SDL_AndroidGetInternalStoragePath,
        SDL_AndroidGetJNIEnv, SDL_AndroidGetActivity};
    if (LumenAndroidConfigureVulkanDriver("PS4", directory, name, host)) return true;
    // SDL2 returns -1 on error; SDL3 returns false. Never coerce SDL2's result.
    SDL_SetError("PS4 private Vulkan driver: %s", LumenAndroidVulkanDriverError());
    return false;
}

extern "C" void* LumenPs4SDL_LoadObject(const char* path) {
    auto* object = LumenAndroidVulkanLoadObject(path);
    if (!object) SDL_SetError("PS4 Vulkan loader: %s", LumenAndroidVulkanDriverError());
    return object;
}

extern "C" void* LumenPs4SDL_LoadFunction(void* object, const char* name) {
    auto* function = LumenAndroidVulkanLoadFunction(object, name);
    if (!function) SDL_SetError("PS4 Vulkan symbol %s: %s", name ? name : "(null)", LumenAndroidVulkanDriverError());
    return function;
}

extern "C" void LumenPs4SDL_UnloadObject(void* object) {
    LumenAndroidVulkanUnloadObject(object);
}
