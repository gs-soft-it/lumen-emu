# SPDX-License-Identifier: GPL-2.0-only
# Called only on SDL3's generated Android source copy, before add_subdirectory.
set(_vulkan_backend "${KYTY_SDL3_ANDROID_SOURCE}/src/video/android/SDL_androidvulkan.c")
file(READ "${_vulkan_backend}" _vulkan_text)
set(_loader_functions SDL_LoadObject SDL_LoadFunction SDL_UnloadObject)
foreach(_function IN LISTS _loader_functions)
    string(FIND "${_vulkan_text}" "${_function}(" _position)
    if(_position LESS 0)
        message(FATAL_ERROR "SDL3 Vulkan host adapter source contract changed: ${_function}")
    endif()
    string(REPLACE "${_function}(" "LumenPs5${_function}(" _vulkan_text "${_vulkan_text}")
endforeach()
set(_declarations "\n/* Lumen altered Android host loader, 2026-10-02: one process-local Vulkan handle. */\nextern SDL_SharedObject *LumenPs5SDL_LoadObject(const char *path);\nextern SDL_FunctionPointer LumenPs5SDL_LoadFunction(SDL_SharedObject *object, const char *name);\nextern void LumenPs5SDL_UnloadObject(SDL_SharedObject *object);\n")
string(REPLACE "#include \"SDL_androidvulkan.h\""
    "#include \"SDL_androidvulkan.h\"\n${_declarations}" _vulkan_text "${_vulkan_text}")
file(WRITE "${_vulkan_backend}" "${_vulkan_text}")
