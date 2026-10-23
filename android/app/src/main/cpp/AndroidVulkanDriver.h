// SPDX-License-Identifier: GPL-2.0-only
#pragma once

// SDL-independent host functions; SDL2 and SDL3 adapters keep their own ABI.
struct LumenAndroidVulkanHost {
    const char* (*getInternalStoragePath)();
    void* (*getJNIEnv)();
    void* (*getActivity)();
};

// Empty directory/name chooses stock. Custom selection is app-private and
// immutable for this process; every invalid request fails without fallback.
bool LumenAndroidConfigureVulkanDriver(const char* generation, const char* driverDirectory,
    const char* driverName, const LumenAndroidVulkanHost& host);
void* LumenAndroidVulkanLoadObject(const char* path);
void* LumenAndroidVulkanLoadFunction(void* object, const char* name);
void LumenAndroidVulkanUnloadObject(void* object);
const char* LumenAndroidVulkanDriverError();
