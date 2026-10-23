cmake_minimum_required(VERSION 3.22)

project(LumenMacOSFEXCoreLab LANGUAGES C CXX)

option(LUMEN_MACOS_FEXCORE_LAB
  "Build the isolated Debug-only macOS FEXCore bootstrap laboratory"
  OFF)
set(LUMEN_MACOS_FEXCORE_SOURCE_DIR "" CACHE PATH
  "External clean FEX checkout prepared for the macOS bootstrap laboratory")

if(NOT LUMEN_MACOS_FEXCORE_LAB)
  message(STATUS "Lumen macOS FEXCore laboratory is disabled")
  return()
endif()

set(LUMEN_FEX_UPSTREAM_COMMIT
  "e17fdf9e90a49614e16ac8414a687477fb9e4f7c")
set(LUMEN_FEX_PORT_COMMIT
  "2d729002c28b39c405636b6e69a0404ca3d97d05")
set(LUMEN_FEX_PORT_TREE
  "4847cd800a15a947ca0158d41abbdabf0b73e498")
set(LUMEN_FEX_UPSTREAM_ORIGIN
  "https://github.com/FEX-Emu/FEX.git")
set(LUMEN_FEX_LICENSE_SHA256
  "f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee")
set(LUMEN_FEX_PORT_PATCH_SHA256
  "6c9f5fb6b2485941a5ddef34d4becc522d5c12721cc7f99281f9ca215e03b700")

if(NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB is available only on macOS desktop")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB requires CMAKE_BUILD_TYPE=Debug")
endif()
if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB requires a thin CMAKE_OSX_ARCHITECTURES=arm64 build")
endif()
execute_process(
  COMMAND /usr/bin/xcrun --sdk macosx --show-sdk-path
  RESULT_VARIABLE LUMEN_FEX_SDK_RESULT
  OUTPUT_VARIABLE LUMEN_FEX_ACTIVE_SDK
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT LUMEN_FEX_SDK_RESULT EQUAL 0 OR
   NOT IS_DIRECTORY "${LUMEN_FEX_ACTIVE_SDK}")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB could not resolve the active macosx SDK")
endif()
if(NOT CMAKE_OSX_SYSROOT STREQUAL "macosx" AND
   NOT CMAKE_OSX_SYSROOT STREQUAL LUMEN_FEX_ACTIVE_SDK)
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB requires the explicit active macosx SDK; got ${CMAKE_OSX_SYSROOT}")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "12.0")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB requires CMAKE_OSX_DEPLOYMENT_TARGET=12.0")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$" OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB requires Apple Clang/Clang")
endif()
if(NOT CMAKE_C_COMPILER STREQUAL "/usr/bin/clang" OR
   NOT CMAKE_CXX_COMPILER STREQUAL "/usr/bin/clang++")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB requires /usr/bin/clang and /usr/bin/clang++")
endif()
if(CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB cannot be cross-compiled")
endif()

execute_process(
  COMMAND uname -m
  RESULT_VARIABLE LUMEN_FEX_UNAME_RESULT
  OUTPUT_VARIABLE LUMEN_FEX_NATIVE_ARCH
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT LUMEN_FEX_UNAME_RESULT EQUAL 0 OR
   NOT LUMEN_FEX_NATIVE_ARCH STREQUAL "arm64")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB requires a native arm64 configure process")
endif()
execute_process(
  COMMAND sysctl -in sysctl.proc_translated
  RESULT_VARIABLE LUMEN_FEX_TRANSLATED_RESULT
  OUTPUT_VARIABLE LUMEN_FEX_TRANSLATED
  ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(LUMEN_FEX_TRANSLATED_RESULT EQUAL 0 AND LUMEN_FEX_TRANSLATED STREQUAL "1")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB refuses a Rosetta-translated configure process")
endif()
execute_process(
  COMMAND getconf PAGESIZE
  RESULT_VARIABLE LUMEN_FEX_PAGE_SIZE_RESULT
  OUTPUT_VARIABLE LUMEN_FEX_CONFIGURE_PAGE_SIZE
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT LUMEN_FEX_PAGE_SIZE_RESULT EQUAL 0 OR
   NOT LUMEN_FEX_CONFIGURE_PAGE_SIZE STREQUAL "16384")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB currently requires a 16384-byte host page")
endif()

include(CheckCXXSourceRuns)
check_cxx_source_runs(
  "#include <pthread.h>
   #include <sys/mman.h>
   int main() {
   #ifndef MAP_JIT
     return 2;
   #else
     return pthread_jit_write_protect_supported_np() ? 0 : 3;
   #endif
   }"
  LUMEN_FEX_HAS_PUBLIC_MAP_JIT_WX)
if(NOT LUMEN_FEX_HAS_PUBLIC_MAP_JIT_WX)
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_LAB requires public MAP_JIT and per-thread JIT W^X support")
endif()
if(LUMEN_MACOS_FEXCORE_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR
    "Set LUMEN_MACOS_FEXCORE_SOURCE_DIR to the prepared external checkout")
endif()

file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." LUMEN_REPOSITORY_ROOT)
file(REAL_PATH "${LUMEN_MACOS_FEXCORE_SOURCE_DIR}"
  LUMEN_MACOS_FEXCORE_SOURCE_REAL)
if(NOT IS_DIRECTORY "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}")
  message(FATAL_ERROR "The configured FEX source directory does not exist")
endif()
file(RELATIVE_PATH LUMEN_FEX_SOURCE_FROM_REPOSITORY
  "${LUMEN_REPOSITORY_ROOT}" "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}")
if(LUMEN_FEX_SOURCE_FROM_REPOSITORY STREQUAL "" OR
   (NOT IS_ABSOLUTE "${LUMEN_FEX_SOURCE_FROM_REPOSITORY}" AND
    NOT LUMEN_FEX_SOURCE_FROM_REPOSITORY MATCHES "^\\.\\./"))
  message(FATAL_ERROR
    "The FEX checkout must live outside the Lumen repository")
endif()

find_package(Git REQUIRED)

function(lumen_fex_git_capture OUTPUT_NAME)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}" ${ARGN}
    RESULT_VARIABLE LUMEN_FEX_GIT_RESULT
    OUTPUT_VARIABLE LUMEN_FEX_GIT_OUTPUT
    ERROR_VARIABLE LUMEN_FEX_GIT_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT LUMEN_FEX_GIT_RESULT EQUAL 0)
    message(FATAL_ERROR
      "FEX provenance command failed: git ${ARGN}: ${LUMEN_FEX_GIT_ERROR}")
  endif()
  set(${OUTPUT_NAME} "${LUMEN_FEX_GIT_OUTPUT}" PARENT_SCOPE)
endfunction()

lumen_fex_git_capture(LUMEN_FEX_HEAD rev-parse --verify HEAD)
lumen_fex_git_capture(LUMEN_FEX_PARENT rev-parse --verify HEAD^)
lumen_fex_git_capture(LUMEN_FEX_TREE rev-parse --verify "HEAD^{tree}")
lumen_fex_git_capture(LUMEN_FEX_ORIGIN remote get-url origin)
lumen_fex_git_capture(LUMEN_FEX_STATUS
  status --porcelain=v1 --untracked-files=all --ignore-submodules=none)

if(NOT LUMEN_FEX_HEAD STREQUAL LUMEN_FEX_PORT_COMMIT)
  message(FATAL_ERROR
    "FEX port commit mismatch: expected ${LUMEN_FEX_PORT_COMMIT}, got ${LUMEN_FEX_HEAD}")
endif()
if(NOT LUMEN_FEX_PARENT STREQUAL LUMEN_FEX_UPSTREAM_COMMIT)
  message(FATAL_ERROR
    "FEX port parent mismatch: expected ${LUMEN_FEX_UPSTREAM_COMMIT}, got ${LUMEN_FEX_PARENT}")
endif()
if(NOT LUMEN_FEX_TREE STREQUAL LUMEN_FEX_PORT_TREE)
  message(FATAL_ERROR
    "FEX port tree mismatch: expected ${LUMEN_FEX_PORT_TREE}, got ${LUMEN_FEX_TREE}")
endif()
if(NOT LUMEN_FEX_ORIGIN STREQUAL LUMEN_FEX_UPSTREAM_ORIGIN)
  message(FATAL_ERROR
    "FEX origin mismatch: expected ${LUMEN_FEX_UPSTREAM_ORIGIN}, got ${LUMEN_FEX_ORIGIN}")
endif()
set(LUMEN_FEX_REQUIRED_SUBMODULES
  "External/fmt|1be298e1bd68957e4cd352e1f676f00e07dcfb57"
  "External/xxhash|e626a72bc2321cd320e953a0ccf1584cad60f363"
  "External/unordered_dense|3234af2c03549bc85656bfd3a86993bf1cd8aef1"
  "External/rpmalloc|1d85c246cd827ead6865f4f880d4fef53f2b1864")
foreach(LUMEN_FEX_SUBMODULE_RECORD IN LISTS LUMEN_FEX_REQUIRED_SUBMODULES)
  string(REPLACE "|" ";" LUMEN_FEX_SUBMODULE_FIELDS
    "${LUMEN_FEX_SUBMODULE_RECORD}")
  list(GET LUMEN_FEX_SUBMODULE_FIELDS 0 LUMEN_FEX_SUBMODULE_PATH)
  list(GET LUMEN_FEX_SUBMODULE_FIELDS 1 LUMEN_FEX_SUBMODULE_COMMIT)
  if(NOT EXISTS
     "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/${LUMEN_FEX_SUBMODULE_PATH}/.git")
    message(FATAL_ERROR
      "Required FEX submodule is not initialized: ${LUMEN_FEX_SUBMODULE_PATH}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C
      "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/${LUMEN_FEX_SUBMODULE_PATH}"
      rev-parse --verify HEAD
    RESULT_VARIABLE LUMEN_FEX_SUBMODULE_RESULT
    OUTPUT_VARIABLE LUMEN_FEX_SUBMODULE_HEAD
    ERROR_VARIABLE LUMEN_FEX_SUBMODULE_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT LUMEN_FEX_SUBMODULE_RESULT EQUAL 0 OR
     NOT LUMEN_FEX_SUBMODULE_HEAD STREQUAL LUMEN_FEX_SUBMODULE_COMMIT)
    message(FATAL_ERROR
      "FEX submodule mismatch for ${LUMEN_FEX_SUBMODULE_PATH}: expected ${LUMEN_FEX_SUBMODULE_COMMIT}, got ${LUMEN_FEX_SUBMODULE_HEAD}")
  endif()
endforeach()

set(LUMEN_FEX_REQUIRED_IN_TREE_COMPONENTS
  "External/SoftFloat-3e|9b2659373bcc8bbfa28eda054b22214a0482ebd2"
  "External/cephes|f60034c28a4d816fb972ccb78d14ae560a973fa2")
foreach(LUMEN_FEX_IN_TREE_RECORD IN LISTS LUMEN_FEX_REQUIRED_IN_TREE_COMPONENTS)
  string(REPLACE "|" ";" LUMEN_FEX_IN_TREE_FIELDS
    "${LUMEN_FEX_IN_TREE_RECORD}")
  list(GET LUMEN_FEX_IN_TREE_FIELDS 0 LUMEN_FEX_IN_TREE_PATH)
  list(GET LUMEN_FEX_IN_TREE_FIELDS 1 LUMEN_FEX_IN_TREE_TREE)
  lumen_fex_git_capture(LUMEN_FEX_OBSERVED_IN_TREE_TREE
    rev-parse --verify "HEAD:${LUMEN_FEX_IN_TREE_PATH}")
  if(NOT LUMEN_FEX_OBSERVED_IN_TREE_TREE STREQUAL LUMEN_FEX_IN_TREE_TREE)
    message(FATAL_ERROR
      "FEX in-tree component mismatch for ${LUMEN_FEX_IN_TREE_PATH}: expected ${LUMEN_FEX_IN_TREE_TREE}, got ${LUMEN_FEX_OBSERVED_IN_TREE_TREE}")
  endif()
endforeach()

# The immutable component pins above make these representative upstream
# license/notice hashes sufficient to bind the complete linked legal closure.
set(LUMEN_FEX_REQUIRED_LICENSES
  "LICENSE|f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee"
  "FEXCore/LICENSE|f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee"
  "External/fmt/LICENSE|a65fcc5a1095fc33cc37e4fff69366e49d7e92f613558a2b95ed1b70e33758b4"
  "External/xxhash/LICENSE|6ffedbc0f7878612d2b23589f1ff2ab15633e1df7963a5d9fc750ec5500c7e7a"
  "External/unordered_dense/LICENSE|4f5a62734899ec0ba85d3e8264a7ed1fcb4bcb8f0b4359c0afd627b6314ae599"
  "External/rpmalloc/LICENSE|9fdc653aacc736bef0cd12854f4a8d94f703ec5603fb5dca1be945b7f679918c"
  "External/SoftFloat-3e/include/SoftFloat-3e/softfloat.h|907a309d402530a9647cfe8292228250539d4cafb6f2c1cbb9eaa6e7bd9d639c"
  "External/cephes/LICENSE|edf1440b625e6d434a986cdb535c702001afab8959d94ce6275441a58cfd0ebb")
foreach(LUMEN_FEX_LICENSE_RECORD IN LISTS LUMEN_FEX_REQUIRED_LICENSES)
  string(REPLACE "|" ";" LUMEN_FEX_LICENSE_FIELDS
    "${LUMEN_FEX_LICENSE_RECORD}")
  list(GET LUMEN_FEX_LICENSE_FIELDS 0 LUMEN_FEX_LICENSE_PATH)
  list(GET LUMEN_FEX_LICENSE_FIELDS 1 LUMEN_FEX_LICENSE_HASH)
  set(LUMEN_FEX_LICENSE_FILE
    "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/${LUMEN_FEX_LICENSE_PATH}")
  if(NOT EXISTS "${LUMEN_FEX_LICENSE_FILE}")
    message(FATAL_ERROR
      "Required FEX closure license/notice is missing: ${LUMEN_FEX_LICENSE_PATH}")
  endif()
  file(SHA256 "${LUMEN_FEX_LICENSE_FILE}"
    LUMEN_FEX_OBSERVED_LICENSE_SHA256)
  if(NOT LUMEN_FEX_OBSERVED_LICENSE_SHA256 STREQUAL LUMEN_FEX_LICENSE_HASH)
    message(FATAL_ERROR
      "FEX closure license/notice hash mismatch: ${LUMEN_FEX_LICENSE_PATH}")
  endif()
endforeach()
set(LUMEN_FEX_PORT_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-darwin-arm64.patch")
file(SHA256 "${LUMEN_FEX_PORT_PATCH}" LUMEN_FEX_OBSERVED_PATCH_SHA256)
if(NOT LUMEN_FEX_OBSERVED_PATCH_SHA256 STREQUAL LUMEN_FEX_PORT_PATCH_SHA256)
  message(FATAL_ERROR "Tracked FEX Darwin patch hash mismatch")
endif()
set(LUMEN_FEX_OBSERVED_PORT_PATCH
  "${CMAKE_CURRENT_BINARY_DIR}/lumen-fex-observed-port.patch")
execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}"
    diff --binary --no-ext-diff
    "${LUMEN_FEX_UPSTREAM_COMMIT}" "${LUMEN_FEX_PORT_COMMIT}"
  RESULT_VARIABLE LUMEN_FEX_PORT_DIFF_RESULT
  OUTPUT_FILE "${LUMEN_FEX_OBSERVED_PORT_PATCH}"
  ERROR_VARIABLE LUMEN_FEX_PORT_DIFF_ERROR)
if(NOT LUMEN_FEX_PORT_DIFF_RESULT EQUAL 0)
  message(FATAL_ERROR
    "Could not reconstruct the deterministic FEX port diff: ${LUMEN_FEX_PORT_DIFF_ERROR}")
endif()
file(SHA256 "${LUMEN_FEX_OBSERVED_PORT_PATCH}"
  LUMEN_FEX_OBSERVED_PORT_DIFF_SHA256)
file(REMOVE "${LUMEN_FEX_OBSERVED_PORT_PATCH}")
if(NOT LUMEN_FEX_OBSERVED_PORT_DIFF_SHA256 STREQUAL
   LUMEN_FEX_PORT_PATCH_SHA256)
  message(FATAL_ERROR
    "Tracked FEX Darwin patch is not the exact deterministic port diff")
endif()
if(NOT LUMEN_FEX_STATUS STREQUAL "")
  message(FATAL_ERROR
    "The external FEX checkout must be clean; status was: ${LUMEN_FEX_STATUS}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(BUILD_STEAM_SUPPORT ON)
set(ARCHITECTURE_arm64 1)
set(ENABLE_FEX_ALLOCATOR ON)
set(ENABLE_JEMALLOC_GLIBC_ALLOC OFF)
set(ENABLE_FEXCORE_PROFILER OFF)
set(ENABLE_GLIBC_ALLOCATOR_HOOK_FAULT OFF)
set(ENABLE_VIXL_DISASSEMBLER OFF)
set(ENABLE_VIXL_SIMULATOR OFF)
set(ENABLE_ZYDIS OFF)
set(FEX_TUNE_COMPILE_FLAGS "")
set(GIT_DESCRIBE_STRING "FEX-e17fdf9-macos-bootstrap")
set(GIT_HASH_ARRAY
  "0xe1, 0x7f, 0xdf, 0x9e, 0x90, 0xa4, 0x96, 0x14, 0xe1, 0x6a, 0xc8, 0x41, 0x4a, 0x68, 0x74, 0x77, 0xfb, 0x9e, 0x4f, 0x7c")

# Keep the complete linked closure at the Android lab's intentional profile:
# optimized code with assertions still enabled.  Apple x18 is reserved both
# here and inside the patched dynamic register allocator.
add_compile_options(
  $<$<COMPILE_LANGUAGE:C,CXX>:-O2>
  $<$<COMPILE_LANGUAGE:C,CXX>:-ffixed-x18>
  $<$<COMPILE_LANGUAGE:C,CXX>:-UNDEBUG>
  $<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=LUMENROOT/macos/fexcore-lab>
  $<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${LUMEN_REPOSITORY_ROOT}=LUMENROOT>
  $<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${LUMEN_MACOS_FEXCORE_SOURCE_REAL}=FEXROOT>
  $<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=BUILDROOT>
  $<$<COMPILE_LANGUAGE:C,CXX>:-fdebug-compilation-dir=BUILDROOT>)
add_compile_definitions(
  ARCHITECTURE_arm64=1
  ASSERTIONS_ENABLED=1
  _LARGEFILE64_SOURCE
  FEX_DISABLE_TELEMETRY=1
  GLOBAL_DATA_DIRECTORY="")

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-Wattributes -Werror=attributes")
check_cxx_source_compiles(
  "__attribute__((preserve_all)) int f(int x) { return x; }
   int main() { return f(1); }"
  HAS_CLANG_PRESERVE_ALL)
unset(CMAKE_REQUIRED_FLAGS)
if(HAS_CLANG_PRESERVE_ALL)
  add_compile_definitions(
    "FEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
    FEX_HAS_PRESERVE_ALL_ATTR=1)
else()
  add_compile_definitions(
    FEX_PRESERVE_ALL_ATTR=
    FEX_HAS_PRESERVE_ALL_ATTR=0)
endif()

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(XXHASH_BUNDLED_MODE TRUE CACHE BOOL "" FORCE)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)

add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/External/fmt"
  fex-fmt EXCLUDE_FROM_ALL)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/External/xxhash/cmake_unofficial"
  fex-xxhash EXCLUDE_FROM_ALL)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/External/unordered_dense"
  fex-unordered-dense EXCLUDE_FROM_ALL)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/External/rpmalloc"
  fex-rpmalloc EXCLUDE_FROM_ALL)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/External/SoftFloat-3e"
  fex-softfloat EXCLUDE_FROM_ALL)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/External/cephes"
  fex-cephes EXCLUDE_FROM_ALL)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXHeaderUtils"
  fex-header-utils EXCLUDE_FROM_ALL)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/CodeEmitter"
  fex-code-emitter EXCLUDE_FROM_ALL)

list(APPEND CMAKE_MODULE_PATH
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/Data/CMake")
include(LinkerGC)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore"
  fex-core EXCLUDE_FROM_ALL)

add_executable(lumen_macos_fexcore_bootstrap_smoke
  BootstrapSmoke.cpp)
target_include_directories(lumen_macos_fexcore_bootstrap_smoke PRIVATE
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_link_libraries(lumen_macos_fexcore_bootstrap_smoke PRIVATE
  FEXCore
  FEXCore_Base
  JemallocLibs)
set_target_properties(lumen_macos_fexcore_bootstrap_smoke PROPERTIES
  OUTPUT_NAME lumen_macos_fexcore_bootstrap_smoke)

enable_testing()
add_test(NAME lumen_macos_fexcore_bootstrap_smoke
  COMMAND "${CMAKE_COMMAND}" -E env
    LUMEN_MACOS_FEXCORE_RUN=1
    $<TARGET_FILE:lumen_macos_fexcore_bootstrap_smoke>)
set_tests_properties(lumen_macos_fexcore_bootstrap_smoke PROPERTIES
  TIMEOUT 30
  PASS_REGULAR_EXPRESSION
    "LUMEN_MACOS_FEXCORE_BOOTSTRAP_V0 pass=1 code=0 reason=ok bootstrapSmoke=1 runtimeReady=0 romRoute=0")

message(STATUS
  "Lumen macOS FEXCore bootstrap lab: upstream=${LUMEN_FEX_UPSTREAM_COMMIT} port=${LUMEN_FEX_PORT_COMMIT} tree=${LUMEN_FEX_PORT_TREE}")
