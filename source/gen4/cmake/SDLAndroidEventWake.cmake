# SPDX-License-Identifier: GPL-2.0-or-later
# Apply only to the build-owned Android overlay of the pinned SDL source.
function(lumen_gen4_patch_sdl_android_event_wake upstream overlay)
    set(events "${upstream}/src/events/SDL_events.c")
    file(SHA256 "${events}" events_sha256)
    if(NOT events_sha256 STREQUAL "51205b8beac66c4e0e289c16ae5943d4bca262aa211431bc9fa44651456e6063")
        message(FATAL_ERROR "SDL Android event wake patch needs review for this source revision")
    endif()
    file(READ "${events}" events_text)
    set(before [=[#ifdef SDL_PLATFORM_ANDROID
    Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_WAKE);
#else]=])
    set(after [=[#ifdef SDL_PLATFORM_ANDROID
    // Local Android adaptation: pumping adds a poll sentinel on this thread.
    // Waking ourselves for it makes WaitEvent spin instead of blocking.
    // Producers on other threads must still wake the lifecycle semaphore.
    if (!SDL_IsMainThread()) {
        Android_SendLifecycleEvent(SDL_ANDROID_LIFECYCLE_WAKE);
    }
#else]=])
    string(FIND "${events_text}" "${before}" patch_position)
    if(patch_position LESS 0)
        message(FATAL_ERROR "SDL Android event wake patch context is missing")
    endif()
    string(REPLACE "${before}" "${after}" events_text "${events_text}")
    file(WRITE "${overlay}/src/events/SDL_events.c" "${events_text}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${events}")
endfunction()
