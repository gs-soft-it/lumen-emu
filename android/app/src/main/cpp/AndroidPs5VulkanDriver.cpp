// Lumen SDL3 Android host integration, 2026-10-02/11. SPDX-License-Identifier: GPL-2.0-only
#include "AndroidPs5VulkanDriver.h"
#include "AndroidVulkanDriver.h"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_loadso.h>
#include <SDL3/SDL_system.h>

bool LumenAndroidPs5ConfigureVulkanDriver(const char* directory, const char* name) {
    const LumenAndroidVulkanHost host {SDL_GetAndroidInternalStoragePath,
        SDL_GetAndroidJNIEnv, SDL_GetAndroidActivity};
    if (LumenAndroidConfigureVulkanDriver("PS5", directory, name, host)) return true;
    return SDL_SetError("PS5 private Vulkan driver: %s", LumenAndroidVulkanDriverError());
}

extern "C" SDL_SharedObject* LumenPs5SDL_LoadObject(const char* path) {
    auto* object = LumenAndroidVulkanLoadObject(path);
    if (!object) SDL_SetError("PS5 Vulkan loader: %s", LumenAndroidVulkanDriverError());
    return reinterpret_cast<SDL_SharedObject*>(object);
}

extern "C" SDL_FunctionPointer LumenPs5SDL_LoadFunction(SDL_SharedObject* object, const char* name) {
    auto* function = LumenAndroidVulkanLoadFunction(object, name);
    if (!function) SDL_SetError("PS5 Vulkan symbol %s: %s", name ? name : "(null)", LumenAndroidVulkanDriverError());
    return reinterpret_cast<SDL_FunctionPointer>(function);
}

extern "C" void LumenPs5SDL_UnloadObject(SDL_SharedObject* object) {
    LumenAndroidVulkanUnloadObject(object);
}
