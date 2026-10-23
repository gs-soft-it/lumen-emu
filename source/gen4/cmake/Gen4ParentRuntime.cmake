# SPDX-License-Identifier: GPL-2.0-or-later
# Build the isolated Gen4 CPU and reuse only the parent native Metal closure.
include_guard(GLOBAL)
if(ENABLE_FEX_GUEST_CPU)
    if(NOT EXISTS "${LUMEN_GEN4_PARENT_BUILD}/CMakeCache.txt")
        message(FATAL_ERROR "Set LUMEN_GEN4_PARENT_BUILD to a completed Lumen ARM64 build")
    endif()
    if(APPLE OR ANDROID)
        add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/direct_fex" "direct-fex")
    else()
        message(FATAL_ERROR "The active Gen4 ARM64 hosts are macOS and Android")
    endif()
    target_include_directories(lumen_gen4_guest_cpu INTERFACE
        "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp"
        "${LUMEN_REPOSITORY_ROOT}/source/cpu/runtime")
    # spdlog requires a config package even when the parent target already exists.
    set(gen4_fmt_include "${LUMEN_GEN4_DIRECT_FEX_SOURCE_DIR}/External/fmt/include")
    file(STRINGS "${gen4_fmt_include}/fmt/base.h" gen4_fmt_version_line REGEX "^#define FMT_VERSION ")
    string(REGEX REPLACE "^#define FMT_VERSION " "" gen4_fmt_version_number "${gen4_fmt_version_line}")
    math(EXPR gen4_fmt_major "${gen4_fmt_version_number} / 10000")
    math(EXPR gen4_fmt_minor "${gen4_fmt_version_number} / 100 % 100")
    math(EXPR gen4_fmt_patch "${gen4_fmt_version_number} % 100")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/parent-packages/fmt")
    file(WRITE "${CMAKE_BINARY_DIR}/parent-packages/fmt/fmtConfig.cmake"
        "if(NOT TARGET fmt::fmt)\n  message(FATAL_ERROR \"Parent fmt target is missing\")\nendif()\nset(fmt_FOUND TRUE)\nset(fmt_VERSION ${gen4_fmt_major}.${gen4_fmt_minor}.${gen4_fmt_patch})\n")
    set(fmt_DIR "${CMAKE_BINARY_DIR}/parent-packages/fmt" CACHE PATH "Exact parent fmt package" FORCE)
endif()

if(APPLE)
    if(EXISTS "${LUMEN_GEN4_PARENT_BUILD}/3rdparty/vulkan/libvulkan-1.a")
        set(gen4_native_metal_default "${LUMEN_GEN4_PARENT_BUILD}/3rdparty/vulkan/libvulkan-1.a")
    else()
        set(gen4_native_metal_default "${LUMEN_GEN4_PARENT_BUILD}/ps4/3rdparty/vulkan/libvulkan-1.a")
    endif()
    set(LUMEN_GEN4_NATIVE_METAL_ARCHIVE "${gen4_native_metal_default}" CACHE FILEPATH "Parent native Metal adapter archive")
    set(LUMEN_GEN4_NATIVE_METAL_DEPENDENCIES
        "${LUMEN_GEN4_PARENT_BUILD}/_deps/lumen_spirv_cross_msl_upstream-build/libspirv-cross-msl.a;${LUMEN_GEN4_PARENT_BUILD}/_deps/lumen_spirv_cross_msl_upstream-build/libspirv-cross-glsl.a;${LUMEN_GEN4_PARENT_BUILD}/_deps/lumen_spirv_cross_msl_upstream-build/libspirv-cross-core.a"
        CACHE STRING "Static dependency closure of the parent native Metal adapter")
    foreach(archive IN ITEMS "${LUMEN_GEN4_NATIVE_METAL_ARCHIVE}" ${LUMEN_GEN4_NATIVE_METAL_DEPENDENCIES})
        if(NOT EXISTS "${archive}")
            message(FATAL_ERROR "Missing native Metal archive: ${archive}")
        endif()
    endforeach()
    add_library(lumen_gen4_native_metal INTERFACE)
    target_link_libraries(lumen_gen4_native_metal INTERFACE
        "${LUMEN_GEN4_NATIVE_METAL_ARCHIVE}" ${LUMEN_GEN4_NATIVE_METAL_DEPENDENCIES}
        "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit")
    target_link_options(lumen_gen4_native_metal INTERFACE "LINKER:-u,_vkGetInstanceProcAddr")
endif()
