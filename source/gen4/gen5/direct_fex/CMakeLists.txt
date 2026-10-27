# SPDX-License-Identifier: GPL-2.0-only
# Build the same pinned translator/configuration closure as Gen4. Only the
# PS5-facing bridge differs; the Gen4 engine source is included unchanged by it.
enable_language(C ASM)
add_subdirectory("${LUMEN_SOURCE_ROOT}/gen4/cmake/direct_fex"
    "${CMAKE_CURRENT_BINARY_DIR}/gen4")
get_target_property(direct_fex_core_directory FEXCore SOURCE_DIR)
get_filename_component(direct_fex_source "${direct_fex_core_directory}/../.." ABSOLUTE)
set(LUMEN_PS5_DIRECT_FEX_SOURCE_REAL "${direct_fex_source}" CACHE INTERNAL
    "Verified source of the linked direct translator" FORCE)

add_library(lumen_ps5_direct_fex STATIC
    DirectFexRuntime.cpp DirectFexProviderSelection.cpp)
target_compile_features(lumen_ps5_direct_fex PUBLIC cxx_std_23)
target_compile_options(lumen_ps5_direct_fex PRIVATE
    $<$<CONFIG:Debug>:-O2> -fexceptions)
target_compile_definitions(lumen_ps5_direct_fex PUBLIC LUMEN_PS5_DIRECT_FEX=1)
target_include_directories(lumen_ps5_direct_fex
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
        "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp"
        "${LUMEN_SOURCE_ROOT}/cpu/runtime"
  PRIVATE "${LUMEN_SOURCE_ROOT}/gen4/src"
    "${direct_fex_source}/FEXCore/Source")
target_link_libraries(lumen_ps5_direct_fex PRIVATE lumen_gen4_guest_cpu)

add_executable(lumen_ps5_direct_fex_smoke EXCLUDE_FROM_ALL DirectFexSmoke.cpp)
target_link_libraries(lumen_ps5_direct_fex_smoke PRIVATE lumen_ps5_direct_fex)
if(ANDROID)
    target_link_options(lumen_ps5_direct_fex_smoke PRIVATE
        "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
elseif(APPLE)
    target_compile_options(lumen_ps5_direct_fex PRIVATE -ffixed-x18)
    set_property(TARGET lumen_ps5_direct_fex_smoke APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/../cmake/direct-macos.entitlements")
    add_custom_command(TARGET lumen_ps5_direct_fex_smoke POST_BUILD
        COMMAND codesign --force --sign - --options runtime
            --entitlements "${CMAKE_CURRENT_LIST_DIR}/../cmake/direct-macos.entitlements"
            "$<TARGET_FILE:lumen_ps5_direct_fex_smoke>" VERBATIM)
endif()

# Legacy Android targets are still configured, but are not part of this APK.
# Their dependency name must not pull the legacy facade into the PS5 DSO.
if(ANDROID)
    add_library(lumen_android_fexcore_runtime INTERFACE)
    target_link_libraries(lumen_android_fexcore_runtime INTERFACE lumen_ps5_direct_fex)
endif()
