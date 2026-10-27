# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.22)

# This subtree can be configured independently or entered by Lumen's root.
# Apple's ar needs the existing response-file wrapper before project().
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/../../tools/macos-ar.sh"
      CACHE FILEPATH "Archiver wrapper for macOS" FORCE)
  endif()
  project(LumenPS5FexBridge LANGUAGES C CXX)
  include(CTest)
endif()

get_filename_component(LUMEN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
option(LUMEN_PS5_FEXCORE "Build the PS5 guest execution embedding with native ARM64 FEX" ON)
if(NOT LUMEN_PS5_FEXCORE)
  message(STATUS "PS5 ARM64 guest execution disabled")
  return()
endif()

if(APPLE AND LUMEN_PS5_DIRECT_FEX)
  # Standalone callers select the same source-owned direct engine as the
  # root's isolated build. Never silently substitute the facade provider.
  add_subdirectory(compatibility full-engine)
  return()
endif()

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(MACOS TRUE)
  set(LUMEN_MACOS_FEXCORE_RUNTIME ON)
  set(LUMEN_MACOS_X64_BACKEND_ARCHITECTURE "${CMAKE_OSX_ARCHITECTURES}")
  if(LUMEN_MACOS_X64_BACKEND_ARCHITECTURE STREQUAL "")
    set(LUMEN_MACOS_X64_BACKEND_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  set(LUMEN_MACOS_FEXCORE_SOURCE_DIR "" CACHE PATH
    "External reviewed FEX Darwin checkout (c9c5a132)")
  if(NOT TARGET lumen_macos_fexcore_runtime)
    add_subdirectory("${LUMEN_ROOT}/macos/fexcore-runtime" fexcore-runtime)
  endif()
  set(LUMEN_PS5_FEX_RUNTIME_TARGET lumen_macos_fexcore_runtime)
elseif(ANDROID)
  # Android's complete FEX audit/admission remains in the existing Android
  # application build. It may enter this subtree after producing this target.
  if(NOT ANDROID_ABI STREQUAL "arm64-v8a" OR
     NOT TARGET lumen_android_fexcore_runtime OR
     (NOT LUMEN_PS5_DIRECT_FEX AND NOT TARGET lumen_android_fexcore_lab_admission))
    message(FATAL_ERROR
      "PS5 Android embedding requires the audited arm64-v8a FEX runtime target")
  endif()
  set(LUMEN_PS5_FEX_RUNTIME_TARGET lumen_android_fexcore_runtime)
else()
  message(FATAL_ERROR "The PS5 FEX embedding supports native macOS/Android ARM64 only")
endif()

if(ANDROID AND LUMEN_PS5_DIRECT_FEX)
  set(LUMEN_PS5_ENGINE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/engine/src")
else()
  include(cmake/CompatibilitySources.cmake)
  lumen_gen5_prepare_compatibility_sources(LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT)
  set(LUMEN_PS5_ENGINE_SOURCE_DIR "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src")
endif()
add_library(lumen_ps5_guest_execution STATIC
  "${LUMEN_PS5_ENGINE_SOURCE_DIR}/loader/guestExecution.cpp")
target_include_directories(lumen_ps5_guest_execution PUBLIC
  "${LUMEN_PS5_ENGINE_SOURCE_DIR}")
target_compile_definitions(lumen_ps5_guest_execution PUBLIC LUMEN_PS5_FEXCORE=1)
if(ANDROID AND LUMEN_PS5_DIRECT_FEX)
  target_compile_definitions(lumen_ps5_guest_execution PUBLIC LUMEN_PS5_DIRECT_FEX=1)
endif()
target_compile_definitions(lumen_ps5_guest_execution PRIVATE LUMEN_PS5_STANDALONE_BRIDGE=1)
target_compile_features(lumen_ps5_guest_execution PUBLIC cxx_std_20)
target_compile_options(lumen_ps5_guest_execution PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lumen_ps5_guest_execution PUBLIC ${LUMEN_PS5_FEX_RUNTIME_TARGET})

if(ANDROID AND LUMEN_PS5_DIRECT_FEX)
  # The direct bridge has its own smoke test; facade contracts do not apply.
elseif(ANDROID)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  add_executable(lumen_ps5_fex_smoke EXCLUDE_FROM_ALL
    fex/GuestExecutionSmoke.cpp
    $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>)
  find_library(LUMEN_PS5_ANDROID_LOG log REQUIRED)
  find_library(LUMEN_PS5_ANDROID_PLATFORM android REQUIRED)
  target_link_libraries(lumen_ps5_fex_smoke PRIVATE
    "${LUMEN_PS5_ANDROID_LOG}" "${LUMEN_PS5_ANDROID_PLATFORM}")
  target_link_options(lumen_ps5_fex_smoke PRIVATE
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
else()
  add_executable(lumen_ps5_fex_smoke fex/GuestExecutionSmoke.cpp)
endif()
if(TARGET lumen_ps5_fex_smoke)
  target_link_libraries(lumen_ps5_fex_smoke PRIVATE lumen_ps5_guest_execution)
  target_compile_options(lumen_ps5_fex_smoke PRIVATE -Wall -Wextra -Werror)
endif()
if(NOT ANDROID)
  add_test(NAME lumen_ps5_fex_guest_execution COMMAND lumen_ps5_fex_smoke)
  set_tests_properties(lumen_ps5_fex_guest_execution PROPERTIES
    TIMEOUT 60 LABELS "ps5;fex;synthetic" ENVIRONMENT "LUMEN_CPU_TRANSLATOR=fex")
  if(NOT LUMEN_QEMU_TCG_LIBRARY STREQUAL "")
    add_test(NAME lumen_ps5_qemu_guest_execution COMMAND "${CMAKE_COMMAND}" -E env
      LUMEN_CPU_TRANSLATOR=qemu-tcg $<TARGET_FILE:lumen_ps5_fex_smoke>)
    set_tests_properties(lumen_ps5_qemu_guest_execution PROPERTIES
      TIMEOUT 60 LABELS "ps5;qemu;synthetic")
  endif()
endif()

option(LUMEN_PS5_ELF_PREFLIGHT "Build the Gen5 read-only ELF/SELF loader" ON)
if(LUMEN_PS5_ELF_PREFLIGHT AND NOT ANDROID)
  include(fex/ElfPreflightBuild.cmake)
endif()

# The Android app owns the audited FEX provider. The full Gen5 engine runs in
# its own process and links that same provider, with a separate SDL3 namespace.
if(ANDROID)
  set(LUMEN_PS5_FEX_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/cmake/LumenFexAndroid.cmake")
  add_subdirectory(compatibility full-engine)
endif()
