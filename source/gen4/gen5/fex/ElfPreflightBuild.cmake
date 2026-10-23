# SPDX-License-Identifier: GPL-2.0-only
# Native read-only closure of the Gen5 ELF/SELF loader. This deliberately
# does not link its x86 host signal handlers, guest callbacks or runtime linker.
include(FetchContent)
find_package(Git REQUIRED)

set(SPDLOG_BUILD_ALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
if(NOT TARGET spdlog::spdlog)
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../compatibility/3rdparty/spdlog"
    preflight-spdlog EXCLUDE_FROM_ALL)
endif()

# The exact upstream pins and patch are preserved. Both dependency snapshots
# live under this build's _deps, never inside another repository's checkout.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lumen_ps5_zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG f8745da6ff1ad1e7bab384bd1f9d742439278e99
  GIT_SHALLOW FALSE
  SOURCE_SUBDIR build/cmake)
FetchContent_Declare(lumen_ps5_zarchive_source
  GIT_REPOSITORY https://github.com/Exzap/ZArchive.git
  GIT_TAG d2c717730092c7bf8cbb033b12fd4001b7c4d932
  GIT_SHALLOW FALSE
  PATCH_COMMAND "${CMAKE_COMMAND}"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    "-DSOURCE_DIR=<SOURCE_DIR>"
    "-DPATCH=${CMAKE_CURRENT_LIST_DIR}/../compatibility/3rdparty/patches/zarchive-reader.patch"
    -P "${CMAKE_CURRENT_LIST_DIR}/ApplyZarchivePatch.cmake"
  SOURCE_SUBDIR kyty-library-only)
FetchContent_MakeAvailable(lumen_ps5_zstd lumen_ps5_zarchive_source)

add_library(lumen_ps5_zarchive STATIC
  "${lumen_ps5_zarchive_source_SOURCE_DIR}/src/zarchivereader.cpp"
  "${lumen_ps5_zarchive_source_SOURCE_DIR}/src/zarchivewriter.cpp"
  "${lumen_ps5_zarchive_source_SOURCE_DIR}/src/sha_256.c")
target_include_directories(lumen_ps5_zarchive SYSTEM PUBLIC
  "${lumen_ps5_zarchive_source_SOURCE_DIR}/include")
target_include_directories(lumen_ps5_zarchive PRIVATE
  "${lumen_ps5_zstd_SOURCE_DIR}/lib")
target_link_libraries(lumen_ps5_zarchive PRIVATE libzstd_static)
target_compile_features(lumen_ps5_zarchive PRIVATE cxx_std_20)

set(KYTY_VERSION "0.3.0")
set(KYTY_ENDIAN KYTY_ENDIAN_LITTLE)
set(KYTY_PLATFORM KYTY_PLATFORM_LINUX)
set(KYTY_COMPILER CLANG)
set(KYTY_LINKER LD)
if(CMAKE_BUILD_TYPE MATCHES "Debug")
  set(KYTY_BUILD KYTY_BUILD_DEBUG)
else()
  set(KYTY_BUILD KYTY_BUILD_RELEASE)
endif()
set(KYTY_BUILD_REPOSITORY "")
set(KYTY_RELEASE_TAG "")
set(KYTY_OFFICIAL_BUILD OFF)
set(KYTY_FORK_BUILD OFF)
set(KYTY_FINAL OFF)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../UPSTREAM.json" LUMEN_PS5_UPSTREAM_METADATA)
string(JSON KYTY_GIT_REVISION GET "${LUMEN_PS5_UPSTREAM_METADATA}" commit)
string(SUBSTRING "${KYTY_GIT_REVISION}" 0 7 KYTY_GIT_HASH)
string(APPEND KYTY_GIT_HASH "-lumen-local")
set(KYTY_GIT_VERSION "${KYTY_GIT_REVISION} with Lumen local integration")
configure_file("${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/cmake_config.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/preflight-config/cmake_config.h")
configure_file("${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/kytyGitVersion.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/preflight-config/kytyGitVersion.h")

add_library(lumen_ps5_elf_loader STATIC
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/loader/elf.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/file.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/archive.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/dateTime.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/assert.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/subsystems.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/emulatorConfig.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/timer.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/logging/log.cpp"
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/platform/sysLinuxFileIO.cpp")
target_include_directories(lumen_ps5_elf_loader PUBLIC
  "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/preflight-config")
target_compile_features(lumen_ps5_elf_loader PUBLIC cxx_std_20)
target_link_libraries(lumen_ps5_elf_loader PUBLIC
  fmt::fmt spdlog::spdlog lumen_ps5_zarchive)
target_compile_options(lumen_ps5_elf_loader PRIVATE -Wall -Wextra)

add_executable(lumen_ps5_elf_preflight fex/ElfPreflight.cpp)
target_include_directories(lumen_ps5_elf_preflight SYSTEM PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../compatibility/3rdparty/nlohmann_json/include")
target_link_libraries(lumen_ps5_elf_preflight PRIVATE lumen_ps5_elf_loader)
target_compile_options(lumen_ps5_elf_preflight PRIVATE
  -Wall -Wextra -Werror -Wno-error=deprecated-declarations)

if(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  add_executable(lumen_ps5_host_exception_test
    fex/HostExceptionSmoke.cpp "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src/common/hostException.cpp")
  target_include_directories(lumen_ps5_host_exception_test PRIVATE
    "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/preflight-config")
  target_compile_features(lumen_ps5_host_exception_test PRIVATE cxx_std_20)
  target_compile_options(lumen_ps5_host_exception_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME lumen_ps5_native_arm_fault COMMAND lumen_ps5_host_exception_test)
  set_tests_properties(lumen_ps5_native_arm_fault PROPERTIES
    TIMEOUT 15 LABELS "ps5;native-arm;host-signal")
endif()
