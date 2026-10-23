# Core-only source export: application UI targets removed on 2026-10-23.
# SPDX-License-Identifier: GPL-2.0-only
# Local integration authored on 2026-10-02; upstream engines retain their notices.
cmake_minimum_required(VERSION 3.22)

if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/source/tools/macos-ar.sh"
    CACHE FILEPATH "Archiver wrapper for macOS" FORCE)
endif()
project(Lumen LANGUAGES C CXX)
include(CTest)

option(LUMEN_BUILD_PS4 "Build the PS4 runtime" ON)
set(LUMEN_GEN4_FULL_BUILD_DIR "${CMAKE_BINARY_DIR}/gen4" CACHE PATH "Independent PS4 build directory")
set(LUMEN_GEN4_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lumen-deps/gen4/sources" CACHE PATH "Pinned PS4 dependencies")
set(LUMEN_GEN4_FFMPEG_PREBUILT_DIR "" CACHE PATH "Native FFmpeg package for PS4")
set(LUMEN_GEN4_BUILD_JOBS "4" CACHE STRING "Parallel PS4 build jobs")
set(_lumen_ps5_fex_default OFF)
if((APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
              CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")) OR ANDROID)
  set(_lumen_ps5_fex_default ON)
endif()
option(LUMEN_BUILD_PS5 "Build the PS5 ARM64/FEX integration"
  ${_lumen_ps5_fex_default})

if(LUMEN_BUILD_PS4)
  # The inherited targets remain available for focused regression checks and
  # the common CPU/Metal libraries. Normal PS4 launches use the complete Gen4
  # graph, isolated from the PS5 engine's dependency target names.
  add_subdirectory(source ps4 EXCLUDE_FROM_ALL)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_target(lumen_gen4_runtime ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source/gen4/tools/build_runtime.py"
      --parent "${CMAKE_BINARY_DIR}" --build "${LUMEN_GEN4_FULL_BUILD_DIR}"
      --deps "${LUMEN_GEN4_DEPS_DIR}" --ffmpeg "${LUMEN_GEN4_FFMPEG_PREBUILT_DIR}"
      --jobs "${LUMEN_GEN4_BUILD_JOBS}"
    BYPRODUCTS "${LUMEN_GEN4_FULL_BUILD_DIR}/lumen_gen4_runtime"
    USES_TERMINAL VERBATIM)
  # Gen4 builds its own pinned direct FEX closure. Only the native Metal
  # adapter is imported from the parent; the legacy CPU remains PS5-only.
  if(TARGET vulkan-1)
    add_dependencies(lumen_gen4_runtime vulkan-1)
  endif()
endif()
if(LUMEN_BUILD_PS5)
  # Keep the existing binary layout while grouping both engines under source.
  add_subdirectory(source/gen4/gen5 ps5)
  if(APPLE AND TARGET lumen_macos_fexcore_runtime)
    # The complete imported engine has independent third-party target names.
    # Build it in a separate graph with the audited parent FEX closure, rather
    # than silently linking a second translator or incompatible fmt archives.
    set(LUMEN_PS5_FULL_BUILD_DIR "${CMAKE_BINARY_DIR}/ps5-full" CACHE PATH
      "Build directory for the complete PS5 engine")
    set(LUMEN_PS5_FFMPEG_PREBUILT_DIR "" CACHE PATH
      "Verified native ARM64 FFmpeg prebuilt directory for the PS5 engine")
    set(LUMEN_PS5_FULL_BUILD_JOBS "4" CACHE STRING
      "Parallel jobs for the independent PS5 engine build")
    if(NOT LUMEN_PS5_FULL_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR "LUMEN_PS5_FULL_BUILD_JOBS must be a positive integer")
    endif()
    add_custom_target(lumen_ps5_runtime
      COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/source/gen4/gen5/tools/build-full-kyty.sh"
        "${CMAKE_BINARY_DIR}" "${LUMEN_PS5_FULL_BUILD_DIR}"
        "${LUMEN_PS5_FFMPEG_PREBUILT_DIR}" "${LUMEN_PS5_FULL_BUILD_JOBS}"
      DEPENDS lumen_macos_fexcore_runtime
      USES_TERMINAL VERBATIM)
  endif()
endif()

if(NOT LUMEN_BUILD_PS4 AND NOT LUMEN_BUILD_PS5)
  message(FATAL_ERROR "Select at least one engine")
endif()
