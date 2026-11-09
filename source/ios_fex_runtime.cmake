# SPDX-License-Identifier: GPL-2.0-only
# Core export adaptation. Each native engine owns its dependency graph.
include_guard(GLOBAL)
if(CMAKE_VERSION VERSION_LESS "3.25")
  message(FATAL_ERROR "The iOS DIRECT FEX runtime graphs require CMake 3.25 or later")
endif()
include(ExternalProject)

set(LUMEN_IOS_FFMPEG_PREBUILT_DIR "" CACHE PATH "FFmpeg static package for this iOS SDK/ARM64 slice")
set(LUMEN_IOS_FEX_BUILD_JOBS "4" CACHE STRING "Parallel jobs for each isolated iOS runtime")
if(NOT LUMEN_IOS_FEX_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "LUMEN_IOS_FEX_BUILD_JOBS must be a positive integer")
endif()
if(NOT EXISTS "${LUMEN_IOS_FFMPEG_PREBUILT_DIR}/include/libavcodec/avcodec.h")
  message(FATAL_ERROR "FEX iOS requires LUMEN_IOS_FFMPEG_PREBUILT_DIR for the selected SDK; build it with source/gen4/gen5/tools/BuildIOSFfmpeg.py")
endif()
set(ios_ffmpeg_receipt "${LUMEN_IOS_FFMPEG_PREBUILT_DIR}/share/ffmpeg/SOURCE.json")
if(NOT EXISTS "${ios_ffmpeg_receipt}")
  message(FATAL_ERROR "The native iOS FFmpeg package is missing its source/build receipt")
endif()
file(READ "${ios_ffmpeg_receipt}" ios_ffmpeg_manifest)
string(JSON ios_ffmpeg_sdk GET "${ios_ffmpeg_manifest}" sdk)
string(JSON ios_ffmpeg_source_sha GET "${ios_ffmpeg_manifest}" source_sha256)
string(JSON ios_ffmpeg_target GET "${ios_ffmpeg_manifest}" target)
if(NOT ios_ffmpeg_target MATCHES "^arm64-apple-ios([0-9.]+)(-simulator)?$")
  message(FATAL_ERROR "The FFmpeg package is not a native iOS ARM64 target")
endif()
if(CMAKE_MATCH_1 VERSION_GREATER CMAKE_OSX_DEPLOYMENT_TARGET)
  message(FATAL_ERROR "The FFmpeg package requires a newer iOS version than the runtime")
endif()
set(ios_expected_sdk iphoneos)
if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
  set(ios_expected_sdk iphonesimulator)
endif()
if(NOT ios_ffmpeg_sdk STREQUAL ios_expected_sdk OR NOT ios_ffmpeg_source_sha STREQUAL
   "3222a17b900456acca57d46cf495867a353a37ae8ee2a5d3c8317f8c266d3699")
  message(FATAL_ERROR "The FFmpeg package does not match the pinned source and selected iOS SDK")
endif()
foreach(ios_ffmpeg_part avformat avcodec swscale avutil avfilter swresample)
  string(JSON ios_ffmpeg_expected_sha GET "${ios_ffmpeg_manifest}" archives "${ios_ffmpeg_part}")
  file(SHA256 "${LUMEN_IOS_FFMPEG_PREBUILT_DIR}/lib/lib${ios_ffmpeg_part}.a" ios_ffmpeg_actual_sha)
  if(NOT ios_ffmpeg_actual_sha STREQUAL ios_ffmpeg_expected_sha)
    message(FATAL_ERROR "The FFmpeg archive differs from its build receipt: ${ios_ffmpeg_part}")
  endif()
endforeach()
if(NOT LUMEN_APPLE_NATIVE_METAL)
  message(FATAL_ERROR "The iOS DIRECT FEX product requires native Metal")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "17.4")
  message(FATAL_ERROR "The public iOS JIT callback API requires deployment target 17.4 or later")
endif()
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
set(ios_font_embed_source "${LUMEN_GEN4_DEPS_DIR}/dear_imgui/misc/fonts/binary_to_compressed_c.cpp")
set(ios_font_embed "${CMAKE_BINARY_DIR}/ios-host-tools/imgui-font-embed")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ios-host-tools")
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
  OUTPUT_VARIABLE ios_host_sdk OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND xcrun --sdk macosx clang++ -std=c++11 -O2
  -isysroot "${ios_host_sdk}" "${ios_font_embed_source}" -o "${ios_font_embed}"
  COMMAND_ERROR_IS_FATAL ANY)
set(ios_runtime_common_args
  "-DCMAKE_SYSTEM_NAME=iOS"
  "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
  "-DCMAKE_OSX_ARCHITECTURES=arm64"
  "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"
  "-DCMAKE_AR=${PROJECT_SOURCE_DIR}/tools/macos-ar.sh"
  "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
  "-DCMAKE_C_COMPILER_LAUNCHER=" "-DCMAKE_CXX_COMPILER_LAUNCHER="
  "-DPython3_EXECUTABLE=${Python3_EXECUTABLE}"
  "-DLUMEN_IOS_FEX_RUNTIME=ON"
  "-DBUILD_TESTING=OFF")
# A Ninja child is deliberately independent of the application's Xcode graph.
find_program(ios_runtime_ninja NAMES ninja REQUIRED)
list(REMOVE_ITEM ios_runtime_common_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
list(APPEND ios_runtime_common_args "-DCMAKE_MAKE_PROGRAM=${ios_runtime_ninja}")

# Parent Metal must exist before Gen4 configures its imported adapter closure.
ExternalProject_Add(lumen_ios_ps4_fex_build
  SOURCE_DIR "${PROJECT_SOURCE_DIR}/gen4"
  BINARY_DIR "${CMAKE_BINARY_DIR}/ios-ps4-fex"
  DOWNLOAD_COMMAND "" UPDATE_COMMAND "" INSTALL_COMMAND ""
  CMAKE_GENERATOR Ninja
  LIST_SEPARATOR "|"
  CMAKE_ARGS ${ios_runtime_common_args}
    "-DLUMEN_GEN4_PARENT_BUILD=${CMAKE_BINARY_DIR}"
    "-DLUMEN_GEN4_NATIVE_METAL_ARCHIVE=$<TARGET_FILE:vulkan-1>"
    "-DLUMEN_GEN4_NATIVE_METAL_DEPENDENCIES=$<TARGET_FILE:spirv-cross-msl>|$<TARGET_FILE:spirv-cross-glsl>|$<TARGET_FILE:spirv-cross-core>"
    "-DLUMEN_GEN4_DEPS_DIR=${LUMEN_GEN4_DEPS_DIR}"
    "-DLUMEN_GEN4_FFMPEG_PREBUILT_DIR=${LUMEN_IOS_FFMPEG_PREBUILT_DIR}"
    "-DALLOWS_ONESHOT_TIMERS_WITH_TIMEOUT_ZERO=OFF"
    "-DIMGUI_FONT_EMBED_EXECUTABLE=${ios_font_embed}"
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target lumen_ios_ps4_fex_runtime
    --parallel "${LUMEN_IOS_FEX_BUILD_JOBS}"
  BUILD_ALWAYS TRUE
  BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/ios-ps4-fex/LumenPS4Runtime.framework/LumenPS4Runtime"
  DEPENDS vulkan-1)
ExternalProject_Add(lumen_ios_ps5_fex_build
  SOURCE_DIR "${PROJECT_SOURCE_DIR}/gen4/gen5/compatibility"
  BINARY_DIR "${CMAKE_BINARY_DIR}/ios-ps5-fex"
  DOWNLOAD_COMMAND "" UPDATE_COMMAND "" INSTALL_COMMAND ""
  CMAKE_GENERATOR Ninja
  CMAKE_ARGS ${ios_runtime_common_args}
    "-DLUMEN_PS5_DIRECT_FEX=ON" "-DLUMEN_PS5_FEXCORE=ON" "-DLUMEN_PS5_NATIVE_METAL=ON"
    "-DKYTY_BUILD_LAUNCHER=OFF" "-DKYTY_ENABLE_CLANG_TIDY=OFF"
    "-DFFMPEG_PREBUILT_DIR=${LUMEN_IOS_FFMPEG_PREBUILT_DIR}"
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target lumen_ios_ps5_fex_runtime
    --parallel "${LUMEN_IOS_FEX_BUILD_JOBS}"
  BUILD_ALWAYS TRUE
  BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/ios-ps5-fex/LumenPS5Runtime.framework/LumenPS5Runtime")

add_custom_target(lumen_ios_fex_frameworks
  DEPENDS lumen_ios_ps4_fex_build lumen_ios_ps5_fex_build)
