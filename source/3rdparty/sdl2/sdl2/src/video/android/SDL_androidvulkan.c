/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
 * @author Mark Callow, www.edgewise-consulting.com. Based on Jacob Lifshay's
 * SDL_x11vulkan.c.
 */

#include "../../SDL_internal.h"

#if SDL_VIDEO_VULKAN && SDL_VIDEO_DRIVER_ANDROID

#include "SDL_androidvideo.h"
#include "SDL_androidwindow.h"

#include "SDL_loadso.h"
#include "SDL_androidvulkan.h"
#include "SDL_syswm.h"

/* Lumen altered Android host loader, 2026-10-11. The SDL2 surface and engine
 * dispatcher use one selected process-local Vulkan library. */
#if defined(LUMEN_ANDROID_PRIVATE_VULKAN_LOADER)
extern void *LumenPs4SDL_LoadObject(const char *path);
extern void *LumenPs4SDL_LoadFunction(void *object, const char *name);
extern void LumenPs4SDL_UnloadObject(void *object);
#define LUMEN_VULKAN_LOAD_OBJECT LumenPs4SDL_LoadObject
#define LUMEN_VULKAN_LOAD_FUNCTION LumenPs4SDL_LoadFunction
#define LUMEN_VULKAN_UNLOAD_OBJECT LumenPs4SDL_UnloadObject
#else
#define LUMEN_VULKAN_LOAD_OBJECT SDL_LoadObject
#define LUMEN_VULKAN_LOAD_FUNCTION SDL_LoadFunction
#define LUMEN_VULKAN_UNLOAD_OBJECT SDL_UnloadObject
#endif

int Android_Vulkan_LoadLibrary(_THIS, const char *path)
{
    VkExtensionProperties *extensions = NULL;
    Uint32 i, extensionCount = 0;
    SDL_bool hasSurfaceExtension = SDL_FALSE;
    SDL_bool hasAndroidSurfaceExtension = SDL_FALSE;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = NULL;
    if(_this->vulkan_config.loader_handle)
        return SDL_SetError("Vulkan already loaded");

    /* Load the Vulkan loader library */
    if(!path)
        path = SDL_getenv("SDL_VULKAN_LIBRARY");
    if(!path)
        path = "libvulkan.so";
    _this->vulkan_config.loader_handle = LUMEN_VULKAN_LOAD_OBJECT(path);
    if(!_this->vulkan_config.loader_handle)
        return -1;
    SDL_strlcpy(_this->vulkan_config.loader_path, path,
                SDL_arraysize(_this->vulkan_config.loader_path));
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)LUMEN_VULKAN_LOAD_FUNCTION(
        _this->vulkan_config.loader_handle, "vkGetInstanceProcAddr");
    if(!vkGetInstanceProcAddr)
        goto fail;
    _this->vulkan_config.vkGetInstanceProcAddr = (void *)vkGetInstanceProcAddr;
    _this->vulkan_config.vkEnumerateInstanceExtensionProperties =
        (void *)((PFN_vkGetInstanceProcAddr)_this->vulkan_config.vkGetInstanceProcAddr)(
            VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if(!_this->vulkan_config.vkEnumerateInstanceExtensionProperties)
        goto fail;
    extensions = SDL_Vulkan_CreateInstanceExtensionsList(
        (PFN_vkEnumerateInstanceExtensionProperties)
            _this->vulkan_config.vkEnumerateInstanceExtensionProperties,
        &extensionCount);
    if(!extensions)
        goto fail;
    for(i = 0; i < extensionCount; i++)
    {
        if(SDL_strcmp(VK_KHR_SURFACE_EXTENSION_NAME, extensions[i].extensionName) == 0)
            hasSurfaceExtension = SDL_TRUE;
        else if(SDL_strcmp(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, extensions[i].extensionName) == 0)
            hasAndroidSurfaceExtension = SDL_TRUE;
    }
    SDL_free(extensions);
    if(!hasSurfaceExtension)
    {
        SDL_SetError("Installed Vulkan doesn't implement the "
                     VK_KHR_SURFACE_EXTENSION_NAME " extension");
        goto fail;
    }
    else if(!hasAndroidSurfaceExtension)
    {
        SDL_SetError("Installed Vulkan doesn't implement the "
                     VK_KHR_ANDROID_SURFACE_EXTENSION_NAME "extension");
        goto fail;
    }
    return 0;

fail:
    LUMEN_VULKAN_UNLOAD_OBJECT(_this->vulkan_config.loader_handle);
    _this->vulkan_config.loader_handle = NULL;
    return -1;
}

void Android_Vulkan_UnloadLibrary(_THIS)
{
    if(_this->vulkan_config.loader_handle)
    {
        LUMEN_VULKAN_UNLOAD_OBJECT(_this->vulkan_config.loader_handle);
        _this->vulkan_config.loader_handle = NULL;
    }
}

SDL_bool Android_Vulkan_GetInstanceExtensions(_THIS,
                                          SDL_Window *window,
                                          unsigned *count,
                                          const char **names)
{
    static const char *const extensionsForAndroid[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    if(!_this->vulkan_config.loader_handle)
    {
        SDL_SetError("Vulkan is not loaded");
        return SDL_FALSE;
    }
    return SDL_Vulkan_GetInstanceExtensions_Helper(
            count, names, SDL_arraysize(extensionsForAndroid),
            extensionsForAndroid);
}

SDL_bool Android_Vulkan_CreateSurface(_THIS,
                                  SDL_Window *window,
                                  VkInstance instance,
                                  VkSurfaceKHR *surface)
{
    SDL_WindowData *windowData;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)_this->vulkan_config.vkGetInstanceProcAddr;
    PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR =
        (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(
                                            instance,
                                            "vkCreateAndroidSurfaceKHR");
    VkAndroidSurfaceCreateInfoKHR createInfo;
    VkResult result;
    SDL_bool created = SDL_FALSE;

    if(!_this->vulkan_config.loader_handle)
    {
        SDL_SetError("Vulkan is not loaded");
        return SDL_FALSE;
    }

    if(!vkCreateAndroidSurfaceKHR)
    {
        SDL_SetError(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
                     " extension is not enabled in the Vulkan instance.");
        return SDL_FALSE;
    }

    /* Lumen modification: surfaceCreated/surfaceDestroyed replace and release
       native_window on the Android UI thread. Hold the same activity mutex
       until vkCreateAndroidSurfaceKHR has acquired its reference, otherwise a
       lifecycle transition can race this read into a NULL or freed window. */
    Android_ActivityMutex_Lock();
    windowData = window ? (SDL_WindowData *)window->driverdata : NULL;
    if(!windowData || !windowData->native_window)
    {
        SDL_SetError("Android native window is not ready for Vulkan surface creation");
        goto done;
    }
    SDL_zero(createInfo);
    createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    createInfo.pNext = NULL;
    createInfo.flags = 0;
    createInfo.window = windowData->native_window;
    result = vkCreateAndroidSurfaceKHR(instance, &createInfo,
                                       NULL, surface);
    if(result != VK_SUCCESS)
    {
        SDL_SetError("vkCreateAndroidSurfaceKHR failed: %s",
                     SDL_Vulkan_GetResultString(result));
        goto done;
    }
    created = SDL_TRUE;

done:
    Android_ActivityMutex_Unlock();
    return created;
}

#endif

/* vi: set ts=4 sw=4 expandtab: */
