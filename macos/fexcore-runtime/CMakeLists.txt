cmake_minimum_required(VERSION 3.22)

if(NOT LUMEN_MACOS_FEXCORE_RUNTIME)
  return()
endif()

if(NOT MACOS OR IOS OR NOT APPLE)
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME is available only on macOS desktop")
endif()
if(NOT LUMEN_MACOS_X64_BACKEND_ARCHITECTURE STREQUAL "arm64")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME requires a thin arm64 build")
endif()
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release|RelWithDebInfo|MinSizeRel)$" OR
   CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME requires a single Debug, Release, RelWithDebInfo or MinSizeRel build")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "12.0")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME requires deployment target 12.0")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$" OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME requires Apple Clang/Clang")
endif()
if(CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME cannot be cross-compiled")
endif()
if(LUMEN_MACOS_FEXCORE_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR
    "Set LUMEN_MACOS_FEXCORE_SOURCE_DIR to the prepared Darwin checkout")
endif()

execute_process(
  COMMAND uname -m
  OUTPUT_VARIABLE LUMEN_MACOS_FEX_NATIVE_ARCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE LUMEN_MACOS_FEX_UNAME_RESULT)
if(NOT LUMEN_MACOS_FEX_UNAME_RESULT EQUAL 0 OR
   NOT LUMEN_MACOS_FEX_NATIVE_ARCH STREQUAL "arm64")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME requires a native arm64 configure process")
endif()
execute_process(
  COMMAND getconf PAGESIZE
  OUTPUT_VARIABLE LUMEN_MACOS_FEX_HOST_PAGE_SIZE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE LUMEN_MACOS_FEX_PAGE_RESULT)
if(NOT LUMEN_MACOS_FEX_PAGE_RESULT EQUAL 0 OR
   NOT LUMEN_MACOS_FEX_HOST_PAGE_SIZE STREQUAL "16384")
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME requires a 16384-byte host page")
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
  LUMEN_MACOS_FEX_HAS_PUBLIC_MAP_JIT_WX)
if(NOT LUMEN_MACOS_FEX_HAS_PUBLIC_MAP_JIT_WX)
  message(FATAL_ERROR
    "LUMEN_MACOS_FEXCORE_RUNTIME requires MAP_JIT and per-thread JIT W^X")
endif()

file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../.." LUMEN_REPOSITORY_ROOT)
file(REAL_PATH "${LUMEN_MACOS_FEXCORE_SOURCE_DIR}"
  LUMEN_MACOS_FEXCORE_SOURCE_REAL)
if(NOT IS_DIRECTORY "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/.git")
  message(FATAL_ERROR "The configured FEX source is not a Git checkout")
endif()
file(RELATIVE_PATH LUMEN_FEX_SOURCE_FROM_REPOSITORY
  "${LUMEN_REPOSITORY_ROOT}" "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}")
if(LUMEN_FEX_SOURCE_FROM_REPOSITORY STREQUAL "" OR
   (NOT IS_ABSOLUTE "${LUMEN_FEX_SOURCE_FROM_REPOSITORY}" AND
    NOT LUMEN_FEX_SOURCE_FROM_REPOSITORY MATCHES "^\\.\\./"))
  message(FATAL_ERROR "The FEX checkout must live outside the Lumen repository")
endif()

find_package(Git REQUIRED)
function(lumen_macos_fex_git_capture OUTPUT_NAME)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}" ${ARGN}
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "FEX provenance command failed: ${ERROR}")
  endif()
  set(${OUTPUT_NAME} "${OUTPUT}" PARENT_SCOPE)
endfunction()

set(LUMEN_MACOS_FEX_RUNTIME_PORT_COMMIT
  "c9c5a132c18175656936b8c25c1d453fe6e262a0")
lumen_macos_fex_git_capture(LUMEN_MACOS_FEX_HEAD rev-parse --verify HEAD)
lumen_macos_fex_git_capture(LUMEN_MACOS_FEX_ORIGIN remote get-url origin)
lumen_macos_fex_git_capture(LUMEN_MACOS_FEX_STATUS
  status --porcelain=v1 --untracked-files=all --ignore-submodules=none)
if(NOT LUMEN_MACOS_FEX_HEAD STREQUAL LUMEN_MACOS_FEX_RUNTIME_PORT_COMMIT)
  message(FATAL_ERROR
    "FEX runtime port commit mismatch: ${LUMEN_MACOS_FEX_HEAD}")
endif()
if(NOT LUMEN_MACOS_FEX_ORIGIN STREQUAL
   "https://github.com/FEX-Emu/FEX.git")
  message(FATAL_ERROR "FEX runtime origin mismatch")
endif()
if(NOT LUMEN_MACOS_FEX_STATUS STREQUAL "")
  message(FATAL_ERROR "The FEX runtime checkout must be clean")
endif()

# Keep the audited base checkout immutable. Each reviewed patch set produces
# a separate, content-verified external source snapshot for this Mac target.
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
set(LUMEN_FEX_DENORMAL_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-independent-denormals.patch")
set(LUMEN_FEX_DENORMAL_USE_BUDGET_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-denormal-use-budget.patch")
set(LUMEN_FEX_IR_VALIDATION_WARNING_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-ir-validation-warning-format.patch")
set(LUMEN_FEX_MEMORY_CHECK_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-guest-memory-check-experiment.patch")
set(LUMEN_FEX_MEMORY_FIXED_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-guest-memory-fixed-accesses.patch")
set(LUMEN_FEX_MEMORY_VARIABLE_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-guest-memory-variable-accesses.patch")
set(LUMEN_FEX_MEMORY_X87_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-guest-memory-x87-accesses.patch")
set(LUMEN_FEX_MEMORY_PREFLIGHT_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-guest-memory-instruction-preflight.patch")
set(LUMEN_FEX_STRUCTURE_TRACE_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-structural-stage-diagnostics.patch")
set(LUMEN_FEX_BLOCK_EXIT_RESET_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-block-set-rip-reset.patch")
set(LUMEN_FEX_TAKEN_BACKEDGE_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-taken-backedge-safepoint.patch")
set(LUMEN_FEX_BLOCK_RIP_METADATA_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-decoded-block-rip-metadata.patch")
set(LUMEN_FEX_DIRECT_UNALIGNED_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-direct-unaligned-tso.patch")
set(LUMEN_FEX_MEMORY_HANDOFF_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-memory-handoff.patch")
set(LUMEN_FEX_RELOCATABLE_HEADER_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-relocatable-jit-header.patch")
set(LUMEN_FEX_GUEST_PROGRESS_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-guest-progress-witness.patch")
set(LUMEN_FEX_JIT_CACHE_DIAGNOSTICS_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-jit-cache-diagnostics.patch")
set(LUMEN_FEX_MEMORY_SCRATCH_LIVENESS_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-memory-handoff-scratch-liveness.patch")
set(LUMEN_FEX_MEMORY_FASTPATH_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-memory-handoff-fastpath.patch")
set(LUMEN_FEX_MEMORY_SHARED_THUNK_PATCH
  "${CMAKE_CURRENT_LIST_DIR}/patches/fex-c9c5a132-memory-handoff-shared-thunk.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/prepare_source_overlay.py"
  "${LUMEN_FEX_DENORMAL_PATCH}" "${LUMEN_FEX_MEMORY_CHECK_PATCH}"
  "${LUMEN_FEX_MEMORY_FIXED_PATCH}" "${LUMEN_FEX_MEMORY_VARIABLE_PATCH}"
  "${LUMEN_FEX_MEMORY_X87_PATCH}" "${LUMEN_FEX_MEMORY_PREFLIGHT_PATCH}"
  "${LUMEN_FEX_STRUCTURE_TRACE_PATCH}" "${LUMEN_FEX_BLOCK_EXIT_RESET_PATCH}"
  "${LUMEN_FEX_TAKEN_BACKEDGE_PATCH}" "${LUMEN_FEX_DENORMAL_USE_BUDGET_PATCH}"
  "${LUMEN_FEX_BLOCK_RIP_METADATA_PATCH}" "${LUMEN_FEX_DIRECT_UNALIGNED_PATCH}"
  "${LUMEN_FEX_IR_VALIDATION_WARNING_PATCH}" "${LUMEN_FEX_MEMORY_HANDOFF_PATCH}"
  "${LUMEN_FEX_MEMORY_SCRATCH_LIVENESS_PATCH}"
  "${LUMEN_FEX_MEMORY_FASTPATH_PATCH}"
  "${LUMEN_FEX_MEMORY_SHARED_THUNK_PATCH}"
  "${LUMEN_FEX_RELOCATABLE_HEADER_PATCH}" "${LUMEN_FEX_GUEST_PROGRESS_PATCH}"
  "${LUMEN_FEX_JIT_CACHE_DIAGNOSTICS_PATCH}")
execute_process(
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/prepare_source_overlay.py"
    "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}" "${LUMEN_MACOS_FEX_RUNTIME_PORT_COMMIT}"
    "${LUMEN_FEX_DENORMAL_PATCH}=7dd066b03c747c5a5a0d5be6c5bb97b8e9aa9bfcb59c5fe6287b42c3d63d87ee"
    "${LUMEN_FEX_MEMORY_CHECK_PATCH}=4eec431b4e478029b82c160b824bde9d635fe31b4ab95acefc85aaf7a3943fd2"
    "${LUMEN_FEX_MEMORY_FIXED_PATCH}=067474aec681758e2b8849670b7898a9eff97517e6f7cff69d1c78832bbfcbfc"
    "${LUMEN_FEX_MEMORY_VARIABLE_PATCH}=117d112736e6d1db79ef8d810243e0b151151c8bcfc29793a6a4e9c78ef7f06f"
    "${LUMEN_FEX_MEMORY_X87_PATCH}=8309b187b33f88533ec74c4069e21e8488ff0b80ff14c9c5c6d53bbd05b2989d"
    "${LUMEN_FEX_MEMORY_PREFLIGHT_PATCH}=c3361c4f5cff8fd32b9975bedd33a069f98e051053fe1ed976808d2c1c999f3b"
    "${LUMEN_FEX_STRUCTURE_TRACE_PATCH}=93de2e5776789dd5638880feed564022717dc4927cddadea84a2d066ad437c78"
    "${LUMEN_FEX_BLOCK_EXIT_RESET_PATCH}=190ca0c02351d837f31c60a11d1227331213c389f1c20757288fc1486badbd26"
    "${LUMEN_FEX_TAKEN_BACKEDGE_PATCH}=dcb6fe7b385d3d13d242b537dc43a7236179e0ad02f38d59c84666cf8814e3cf"
    "${LUMEN_FEX_DENORMAL_USE_BUDGET_PATCH}=59ae4ddeac57c4b656872d52c56da61c8a9f8438e6d5af2bb89c62f9aa543681"
    "${LUMEN_FEX_BLOCK_RIP_METADATA_PATCH}=b64055b5de69a91cee6ba46ff9273118ec0abdf1ff4a38fba9f855155c91c96c"
    "${LUMEN_FEX_DIRECT_UNALIGNED_PATCH}=938efe068b6ed66cacf338966c9e4542f619a182f15be7db1de55e9aaccb3d2e"
    "${LUMEN_FEX_IR_VALIDATION_WARNING_PATCH}=6d33f17ac268bc00cf330cbb03a15d3f8ca2b8b7cc7167ed3fde8318732512dd"
    "${LUMEN_FEX_MEMORY_HANDOFF_PATCH}=16b55271c5b8292898d0c928adb5a9fcf59fd5b079c6cfb71dd356171f962d80"
    "${LUMEN_FEX_RELOCATABLE_HEADER_PATCH}=f60bb1d81d2e4a84b51536077a9558a22680990478fb3ceeef3bd8cf467ad342"
    "${LUMEN_FEX_GUEST_PROGRESS_PATCH}=931ddd555f12e2fe2a7afe550fb71673a3f54cead07ac4e54dca287be45d61ac"
    "${LUMEN_FEX_JIT_CACHE_DIAGNOSTICS_PATCH}=5155bd2ded0b377fa1d2f0e33371ce79f05b9331a74ec624d8ba33b586f9582e"
    "${LUMEN_FEX_MEMORY_SCRATCH_LIVENESS_PATCH}=689130576cbb6484a16f734364d4920a6c5b791a1bf708ac3dca22129ad834e2"
    "${LUMEN_FEX_MEMORY_FASTPATH_PATCH}=27f5ad70a4298d61c53f8e4a4e0612bb91e18dd791b7e58b4669770dbe3b3bc0"
    "${LUMEN_FEX_MEMORY_SHARED_THUNK_PATCH}=39eb9df1ba1ff6e1e6b79115ecbba912af6b750d299dfb8dc8bd6870994c13cc"
  OUTPUT_VARIABLE LUMEN_FEX_PATCHED_SOURCE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE LUMEN_FEX_OVERLAY_RESULT)
if(NOT LUMEN_FEX_OVERLAY_RESULT EQUAL 0 OR NOT IS_DIRECTORY "${LUMEN_FEX_PATCHED_SOURCE}")
  message(FATAL_ERROR "Could not verify the reviewed FEX source overlay")
endif()
set(LUMEN_MACOS_FEXCORE_SOURCE_REAL "${LUMEN_FEX_PATCHED_SOURCE}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)
set(lumen_runtime_build_testing "${BUILD_TESTING}")
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
set(GIT_DESCRIBE_STRING "FEX-e17fdf9-macos-runtime")
set(GIT_HASH_ARRAY
  "0xe1, 0x7f, 0xdf, 0x9e, 0x90, 0xa4, 0x96, 0x14, 0xe1, 0x6a, 0xc8, 0x41, 0x4a, 0x68, 0x74, 0x77, 0xfb, 0x9e, 0x4f, 0x7c")

add_compile_options(
  $<$<COMPILE_LANGUAGE:C,CXX>:-O2>
  $<$<COMPILE_LANGUAGE:C,CXX>:-ffixed-x18>
  $<$<COMPILE_LANGUAGE:C,CXX>:-UNDEBUG>)
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
  add_compile_definitions(FEX_PRESERVE_ALL_ATTR= FEX_HAS_PRESERVE_ALL_ATTR=0)
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
list(APPEND CMAKE_MODULE_PATH "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/Data/CMake")
include(LinkerGC)
add_subdirectory(
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore"
  fex-core EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${lumen_runtime_build_testing}")

add_library(lumen_macos_fexcore_runtime STATIC
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp/AndroidFexCoreRuntime.cpp"
  "${LUMEN_REPOSITORY_ROOT}/source/cpu/runtime/GuestCpuRuntime.cpp"
  "${LUMEN_REPOSITORY_ROOT}/source/cpu/runtime/GuestCpuFexProvider.cpp")
target_include_directories(lumen_macos_fexcore_runtime
  PUBLIC "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp"
         "${LUMEN_REPOSITORY_ROOT}/source/cpu/runtime"
  PRIVATE "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_compile_features(lumen_macos_fexcore_runtime PRIVATE cxx_std_20)
target_compile_definitions(lumen_macos_fexcore_runtime PRIVATE
  LUMEN_ANDROID_FEXCORE_OPTIMIZATION_LEVEL=2
  LUMEN_MACOS_FEXCORE_RUNTIME_BUILD=1
  LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED=1)
include("${CMAKE_CURRENT_LIST_DIR}/FexTranslationCacheBuildIdentity.cmake")
lumen_fex_translation_cache_build_identity(lumen_macos_fexcore_runtime
  "${LUMEN_REPOSITORY_ROOT}" "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}")
target_compile_options(lumen_macos_fexcore_runtime PRIVATE
  -fno-exceptions
  -fno-rtti
  -fno-strict-aliasing
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Werror)
# Optional cache filesystem/allocation failures are caught at the adapter
# boundary; the FEX core and remaining translation units keep their policy.
set_source_files_properties(
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp/AndroidFexCoreRuntime.cpp"
  # libc++ exception matching needs the canonical public RTTI. Compiling this
  # exception boundary with -fno-rtti emits private std::exception typeinfo.
  PROPERTIES COMPILE_OPTIONS "-fexceptions;-frtti")
target_link_libraries(lumen_macos_fexcore_runtime PUBLIC
  FEXCore
  FEXCore_Base
  JemallocLibs)

add_executable(lumen_fex_performance_trace_test EXCLUDE_FROM_ALL
  FexPerformanceTraceTest.cpp)
add_executable(lumen_fex_short_host_call_test EXCLUDE_FROM_ALL
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/test/cpp/AndroidFexCoreShortHostCallTest.cpp")
find_package(Threads REQUIRED)
target_include_directories(lumen_fex_short_host_call_test PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp")
target_compile_features(lumen_fex_short_host_call_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_short_host_call_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(lumen_fex_short_host_call_test PRIVATE Threads::Threads)
add_test(NAME lumen_fex_short_host_call COMMAND lumen_fex_short_host_call_test)
set_tests_properties(lumen_fex_short_host_call PROPERTIES TIMEOUT 30 LABELS "cpu;hle;vm;concurrency")
target_include_directories(lumen_fex_performance_trace_test PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp")
target_compile_features(lumen_fex_performance_trace_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_performance_trace_test PRIVATE -Wall -Wextra -Werror)
foreach(lumen_trace_value IN ITEMS 0 1 malformed)
  add_test(NAME lumen_fex_performance_trace_${lumen_trace_value}
    COMMAND "${CMAKE_COMMAND}" -E env "LUMEN_FEX_PERFORMANCE_TRACE=${lumen_trace_value}"
      $<TARGET_FILE:lumen_fex_performance_trace_test>)
endforeach()

add_executable(lumen_fex_translation_cache_test EXCLUDE_FROM_ALL
  FexTranslationCacheTest.cpp)
target_link_libraries(lumen_fex_translation_cache_test PRIVATE lumen_macos_fexcore_runtime)
target_compile_features(lumen_fex_translation_cache_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_translation_cache_test PRIVATE -Wall -Wextra -Werror)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/translation-cache-tests")
add_test(NAME lumen_fex_translation_cache
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/TestTranslationCache.py"
    $<TARGET_FILE:lumen_fex_translation_cache_test>
    --evidence-root "${CMAKE_CURRENT_BINARY_DIR}/translation-cache-tests")
set_tests_properties(lumen_fex_translation_cache PROPERTIES
  TIMEOUT 240 LABELS "macos;fexcore;cpu;cache")
add_test(NAME lumen_fex_translation_cache_guest_return
  COMMAND "${CMAKE_COMMAND}" -E env LUMEN_FEX_HLE_RETURN_GUEST=1
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/TestTranslationCache.py"
    $<TARGET_FILE:lumen_fex_translation_cache_test>
    --evidence-root "${CMAKE_CURRENT_BINARY_DIR}/translation-cache-tests")
set_tests_properties(lumen_fex_translation_cache_guest_return PROPERTIES
  TIMEOUT 240 LABELS "macos;fexcore;cpu;cache;host-call")

add_executable(lumen_fex_translation_cache_storage_test EXCLUDE_FROM_ALL
  FexTranslationCacheStorageTest.cpp)
target_include_directories(lumen_fex_translation_cache_storage_test PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp"
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/External/xxhash")
target_link_libraries(lumen_fex_translation_cache_storage_test PRIVATE xxhash)
target_compile_features(lumen_fex_translation_cache_storage_test PRIVATE cxx_std_20)
target_compile_definitions(lumen_fex_translation_cache_storage_test PRIVATE
  LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED=1)
target_compile_options(lumen_fex_translation_cache_storage_test PRIVATE -Wall -Wextra -Werror -fexceptions)
add_test(NAME lumen_fex_translation_cache_storage
  COMMAND lumen_fex_translation_cache_storage_test)
set_tests_properties(lumen_fex_translation_cache_storage PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;cpu;cache")

include("${LUMEN_REPOSITORY_ROOT}/source/cpu/runtime/SelectorTests.cmake")
include("${LUMEN_REPOSITORY_ROOT}/source/cpu/qemu/Provider.cmake")
lumen_add_qemu_provider(lumen_macos_fexcore_runtime "${LUMEN_REPOSITORY_ROOT}")

if(NOT LUMEN_QEMU_TCG_LIBRARY STREQUAL "")
  add_executable(lumen_guest_cpu_qemu_provider_test
    "${LUMEN_REPOSITORY_ROOT}/source/cpu/qemu/QemuProviderTest.cpp")
  target_link_libraries(lumen_guest_cpu_qemu_provider_test PRIVATE lumen_macos_fexcore_runtime)
  add_test(NAME lumen_guest_cpu_qemu_provider
    COMMAND $<TARGET_FILE:lumen_guest_cpu_qemu_provider_test>)
  set_tests_properties(lumen_guest_cpu_qemu_provider PROPERTIES
    TIMEOUT 20 LABELS "macos;qemu-tcg;host-call;boundary;fp")
endif()

add_executable(lumen_macos_fexcore_runtime_probe EXCLUDE_FROM_ALL
  MacOSFexCoreRuntimeProbe.cpp)
target_link_libraries(lumen_macos_fexcore_runtime_probe PRIVATE
  lumen_macos_fexcore_runtime)
target_compile_features(lumen_macos_fexcore_runtime_probe PRIVATE cxx_std_20)

add_executable(lumen_fex_host_feature_policy_test EXCLUDE_FROM_ALL
  FexHostFeaturePolicyTest.cpp)
target_link_libraries(lumen_fex_host_feature_policy_test PRIVATE
  lumen_macos_fexcore_runtime)
target_compile_features(lumen_fex_host_feature_policy_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_host_feature_policy_test PRIVATE
  -Wall -Wextra -Werror)
foreach(native_feature_case IN ITEMS default zero one empty malformed)
  if(native_feature_case STREQUAL "default")
    set(native_feature_environment --unset=LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES)
  elseif(native_feature_case STREQUAL "zero")
    set(native_feature_environment LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES=0)
  elseif(native_feature_case STREQUAL "one")
    set(native_feature_environment LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES=1)
  elseif(native_feature_case STREQUAL "empty")
    set(native_feature_environment LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES=)
  else()
    set(native_feature_environment LUMEN_EXPERIMENTAL_FEX_NATIVE_HOST_FEATURES=01)
  endif()
  add_test(NAME lumen_fex_native_host_features_${native_feature_case}
    COMMAND "${CMAKE_COMMAND}" -E env ${native_feature_environment}
      $<TARGET_FILE:lumen_fex_host_feature_policy_test>)
  set_tests_properties(lumen_fex_native_host_features_${native_feature_case} PROPERTIES
    TIMEOUT 30 LABELS "macos;fexcore;cpu")
endforeach()

add_executable(lumen_macos_fexcore_signal_policy_test EXCLUDE_FROM_ALL
  MacOSFexCoreSignalPolicyTest.cpp)
target_link_libraries(lumen_macos_fexcore_signal_policy_test PRIVATE
  lumen_macos_fexcore_runtime)
target_compile_features(lumen_macos_fexcore_signal_policy_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_signal_policy_test PRIVATE
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Werror)

add_executable(lumen_macos_fexcore_jit_test EXCLUDE_FROM_ALL
  MacOSFexCoreJitTest.cpp)
target_link_libraries(lumen_macos_fexcore_jit_test PRIVATE
  lumen_macos_fexcore_runtime)
target_compile_features(lumen_macos_fexcore_jit_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_jit_test PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Werror)

add_test(NAME lumen_macos_fexcore_jit
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_jit_test>)
add_test(NAME lumen_macos_fexcore_jit_blocked_signal
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_jit_test> --blocked-usr1)
add_test(NAME lumen_macos_fexcore_mutation_close
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_jit_test> --mutation-close)
set_tests_properties(lumen_macos_fexcore_mutation_close PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;virtual-memory")

add_test(NAME lumen_macos_fexcore_boundary_interrupt
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_jit_test> --boundary-interrupt)
set_tests_properties(lumen_macos_fexcore_boundary_interrupt PROPERTIES
  TIMEOUT 60 LABELS "macos;fexcore;virtual-memory")

add_test(NAME lumen_macos_fexcore_host_return_boundary
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_jit_test> --host-return-boundary)
set_tests_properties(lumen_macos_fexcore_host_return_boundary PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;jit;virtual-memory")

add_test(NAME lumen_macos_fexcore_large_unit_header
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_jit_test> --large-unit-header)
set_tests_properties(lumen_macos_fexcore_large_unit_header PROPERTIES
  TIMEOUT 45 LABELS "macos;fexcore;jit;virtual-memory")

# Console-semantics experiment: the writable executable range is denied for
# every environment except an exact "1". The product default never admits one.
add_test(NAME lumen_fex_writable_execution_default
  COMMAND "${CMAKE_COMMAND}" -E env
    --unset=LUMEN_EXPERIMENTAL_FEX_WRITABLE_EXECUTION
    $<TARGET_FILE:lumen_macos_fexcore_jit_test> --writable-execution)
add_test(NAME lumen_fex_writable_execution_zero
  COMMAND "${CMAKE_COMMAND}" -E env
    LUMEN_EXPERIMENTAL_FEX_WRITABLE_EXECUTION=0
    $<TARGET_FILE:lumen_macos_fexcore_jit_test> --writable-execution)
add_test(NAME lumen_fex_writable_execution_malformed
  COMMAND "${CMAKE_COMMAND}" -E env
    LUMEN_EXPERIMENTAL_FEX_WRITABLE_EXECUTION=01
    $<TARGET_FILE:lumen_macos_fexcore_jit_test> --writable-execution)
add_test(NAME lumen_fex_writable_execution_one
  COMMAND "${CMAKE_COMMAND}" -E env
    LUMEN_EXPERIMENTAL_FEX_WRITABLE_EXECUTION=1
    $<TARGET_FILE:lumen_macos_fexcore_jit_test> --writable-execution)
set_tests_properties(
  lumen_fex_writable_execution_default
  lumen_fex_writable_execution_zero
  lumen_fex_writable_execution_malformed
  lumen_fex_writable_execution_one
  PROPERTIES TIMEOUT 60 LABELS "macos;fexcore;virtual-memory;experimental")

add_executable(lumen_macos_fexcore_root_state_test EXCLUDE_FROM_ALL
  "${LUMEN_REPOSITORY_ROOT}/source/FexRootStateTest.cpp")
target_link_libraries(lumen_macos_fexcore_root_state_test PRIVATE
  lumen_macos_fexcore_runtime)
target_compile_features(lumen_macos_fexcore_root_state_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_root_state_test PRIVATE
  -Wall -Wextra -Werror)
add_test(NAME lumen_macos_fexcore_root_state
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_root_state_test>)

add_executable(lumen_fex_host_call_return_test EXCLUDE_FROM_ALL
  "${LUMEN_REPOSITORY_ROOT}/source/FexHostCallReturnTest.cpp")
target_link_libraries(lumen_fex_host_call_return_test PRIVATE lumen_macos_fexcore_runtime)
target_compile_features(lumen_fex_host_call_return_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_host_call_return_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lumen_fex_host_call_return COMMAND $<TARGET_FILE:lumen_fex_host_call_return_test>)
set_tests_properties(lumen_fex_host_call_return PROPERTIES
  TIMEOUT 60 LABELS "fexcore;host-call;memory;handoff")

add_executable(lumen_macos_fexcore_fp_boundary_probe EXCLUDE_FROM_ALL
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/test/cpp/AndroidFexCoreFpBoundaryProbe.cpp")
target_link_libraries(lumen_macos_fexcore_fp_boundary_probe PRIVATE
  lumen_macos_fexcore_runtime)
target_compile_definitions(lumen_macos_fexcore_fp_boundary_probe PRIVATE
  LUMEN_FEX_FP_PROBE_DIRECT_LINK=1)
target_compile_features(lumen_macos_fexcore_fp_boundary_probe PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_fp_boundary_probe PRIVATE
  -Wall -Wextra -Werror)
add_test(NAME lumen_macos_fexcore_fp_boundary
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_fp_boundary_probe>)
set_tests_properties(lumen_macos_fexcore_fp_boundary PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;floating-point")

add_executable(lumen_macos_fexcore_guest_tsc_probe EXCLUDE_FROM_ALL
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/test/cpp/FexGuestTscProbe.cpp")
target_include_directories(lumen_macos_fexcore_guest_tsc_probe PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/source/include"
  "${LUMEN_REPOSITORY_ROOT}/source/emulator/include")
target_link_libraries(lumen_macos_fexcore_guest_tsc_probe PRIVATE
  lumen_macos_fexcore_runtime emulator)
target_compile_definitions(lumen_macos_fexcore_guest_tsc_probe PRIVATE
  LUMEN_FEX_TSC_PROBE_DIRECT_LINK=1)
target_compile_features(lumen_macos_fexcore_guest_tsc_probe PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_guest_tsc_probe PRIVATE
  # Existing emulator DbgAssert headers use a volatile assignment expression.
  # Keep the gate strict while admitting that pre-existing C++20 diagnostic.
  -Wall -Wextra -Werror -Wno-deprecated-volatile)
add_test(NAME lumen_macos_fexcore_guest_tsc
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_guest_tsc_probe>)
set_tests_properties(lumen_macos_fexcore_guest_tsc PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;clock")

enable_language(ASM)
add_executable(lumen_macos_fexcore_rcpc_memory_access_test EXCLUDE_FROM_ALL
  MacOSFexCoreRcpcMemoryAccessTest.cpp MacOSFexCoreRcpcFixtures.S)
target_include_directories(lumen_macos_fexcore_rcpc_memory_access_test PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp")
target_compile_features(lumen_macos_fexcore_rcpc_memory_access_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_rcpc_memory_access_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lumen_macos_fexcore_rcpc_memory_access
  COMMAND lumen_macos_fexcore_rcpc_memory_access_test)
set_tests_properties(lumen_macos_fexcore_rcpc_memory_access PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)

add_executable(lumen_macos_fexcore_rcpc_guest_test EXCLUDE_FROM_ALL
  MacOSFexCoreRcpcGuestTest.cpp)
target_link_libraries(lumen_macos_fexcore_rcpc_guest_test PRIVATE lumen_macos_fexcore_runtime)
target_compile_features(lumen_macos_fexcore_rcpc_guest_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_rcpc_guest_test PRIVATE -Wall -Wextra -Werror)
foreach(rcpc IN ITEMS 0 1)
  add_test(NAME lumen_macos_fexcore_rcpc_guest_${rcpc}
    COMMAND "${CMAKE_COMMAND}" -E env "LUMEN_FEX_MACOS_RCPC=${rcpc}"
      LUMEN_FEX_MACOS_DIRECT_UNALIGNED=0
      $<TARGET_FILE:lumen_macos_fexcore_rcpc_guest_test>)
  set_tests_properties(lumen_macos_fexcore_rcpc_guest_${rcpc} PROPERTIES
    TIMEOUT 180 SKIP_RETURN_CODE 77)
  add_test(NAME lumen_macos_fexcore_direct_unaligned_guest_${rcpc}
    COMMAND "${CMAKE_COMMAND}" -E env "LUMEN_FEX_MACOS_RCPC=${rcpc}"
      LUMEN_FEX_MACOS_DIRECT_UNALIGNED=1
      $<TARGET_FILE:lumen_macos_fexcore_rcpc_guest_test>)
  set_tests_properties(lumen_macos_fexcore_direct_unaligned_guest_${rcpc} PROPERTIES
    TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "macos;fexcore;cpu;memory")
endforeach()
add_test(NAME lumen_macos_fexcore_direct_unaligned_guest_default
  COMMAND "${CMAKE_COMMAND}" -E env --unset=LUMEN_FEX_MACOS_DIRECT_UNALIGNED
    LUMEN_FEX_MACOS_RCPC=0
    $<TARGET_FILE:lumen_macos_fexcore_rcpc_guest_test>)
set_tests_properties(lumen_macos_fexcore_direct_unaligned_guest_default PROPERTIES
  TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "macos;fexcore;cpu;memory;selector")
foreach(direct_invalid IN ITEMS "" "01")
  string(LENGTH "${direct_invalid}" direct_length)
  add_test(NAME lumen_macos_fexcore_direct_unaligned_invalid_${direct_length}
    COMMAND "${CMAKE_COMMAND}" -E env "LUMEN_FEX_MACOS_DIRECT_UNALIGNED=${direct_invalid}"
      $<TARGET_FILE:lumen_macos_fexcore_runtime_probe> --expect-configuration-denied)
endforeach()

add_executable(lumen_macos_fexcore_direct_unaligned_cache_test EXCLUDE_FROM_ALL
  MacOSFexCoreDirectUnalignedCacheTest.cpp)
target_link_libraries(lumen_macos_fexcore_direct_unaligned_cache_test PRIVATE lumen_macos_fexcore_runtime)
target_compile_features(lumen_macos_fexcore_direct_unaligned_cache_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_direct_unaligned_cache_test PRIVATE -Wall -Wextra -Werror)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/direct-unaligned-cache-tests")
add_test(NAME lumen_macos_fexcore_direct_unaligned_cache
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/TestDirectUnalignedCache.py"
    $<TARGET_FILE:lumen_macos_fexcore_direct_unaligned_cache_test>
    --evidence-root "${CMAKE_CURRENT_BINARY_DIR}/direct-unaligned-cache-tests")
set_tests_properties(lumen_macos_fexcore_direct_unaligned_cache PROPERTIES
  TIMEOUT 180 LABELS "macos;fexcore;cpu;memory;cache")

# Execute original guest instructions through the product Darwin runtime.
# These opt-in targets are never packaged into the application.
add_executable(lumen_macos_fexcore_memory_handoff_test EXCLUDE_FROM_ALL
  MacOSFexCoreMemoryHandoffTest.cpp)
add_executable(lumen_fex_memory_handoff_policy_test EXCLUDE_FROM_ALL FexMemoryHandoffPolicyTest.cpp)
target_link_libraries(lumen_fex_memory_handoff_policy_test PRIVATE lumen_macos_fexcore_runtime)
target_compile_features(lumen_fex_memory_handoff_policy_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_memory_handoff_policy_test PRIVATE -Wall -Wextra -Werror)
foreach(policy IN ITEMS default exclusive interval query-first provider-first race)
  add_test(NAME lumen_fex_memory_handoff_policy_${policy}
    COMMAND $<TARGET_FILE:lumen_fex_memory_handoff_policy_test> ${policy})
  set_tests_properties(lumen_fex_memory_handoff_policy_${policy} PROPERTIES TIMEOUT 30 LABELS "fexcore;memory;handoff;cache")
endforeach()
target_link_libraries(lumen_macos_fexcore_memory_handoff_test PRIVATE lumen_macos_fexcore_runtime)
target_compile_features(lumen_macos_fexcore_memory_handoff_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_memory_handoff_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lumen_macos_fexcore_memory_handoff
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_memory_handoff_test>)
set_tests_properties(lumen_macos_fexcore_memory_handoff PROPERTIES
  TIMEOUT 60 LABELS "macos;fexcore;memory;handoff;jit")

# Own the synthetic providers, so a missing provider can be tested without
# weakening or overriding the production runtime's fail-closed providers.
add_executable(lumen_macos_fexcore_handoff_cache_test EXCLUDE_FROM_ALL
  MacOSFexCoreHandoffCacheTest.cpp)
target_include_directories(lumen_macos_fexcore_handoff_cache_test PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp"
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_compile_definitions(lumen_macos_fexcore_handoff_cache_test PRIVATE
  LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED=1)
target_link_libraries(lumen_macos_fexcore_handoff_cache_test PRIVATE FEXCore FEXCore_Base JemallocLibs)
target_compile_features(lumen_macos_fexcore_handoff_cache_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_handoff_cache_test PRIVATE -Wall -Wextra -Werror -fexceptions)
add_test(NAME lumen_macos_fexcore_handoff_cache
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_handoff_cache_test>)
set_tests_properties(lumen_macos_fexcore_handoff_cache PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;memory;handoff;cache")

add_executable(lumen_fex_guest_progress_emission_test EXCLUDE_FROM_ALL
  FexGuestProgressEmissionTest.cpp)
target_include_directories(lumen_fex_guest_progress_emission_test PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp"
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_compile_definitions(lumen_fex_guest_progress_emission_test PRIVATE
  LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED=1)
target_link_libraries(lumen_fex_guest_progress_emission_test PRIVATE FEXCore FEXCore_Base JemallocLibs)
target_compile_features(lumen_fex_guest_progress_emission_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_guest_progress_emission_test PRIVATE -Wall -Wextra -Werror -fexceptions)
add_test(NAME lumen_fex_guest_progress_emission
  COMMAND $<TARGET_FILE:lumen_fex_guest_progress_emission_test>)
set_tests_properties(lumen_fex_guest_progress_emission PROPERTIES TIMEOUT 30 LABELS "macos;fexcore;progress")

add_executable(lumen_fex_memory_scratch_emission_test EXCLUDE_FROM_ALL
  FexMemoryScratchEmissionTest.cpp)
target_include_directories(lumen_fex_memory_scratch_emission_test PRIVATE
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_link_libraries(lumen_fex_memory_scratch_emission_test PRIVATE FEXCore FEXCore_Base JemallocLibs)
target_compile_features(lumen_fex_memory_scratch_emission_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_memory_scratch_emission_test PRIVATE -Wall -Wextra -Werror -fexceptions)
add_test(NAME lumen_fex_memory_scratch_emission
  COMMAND $<TARGET_FILE:lumen_fex_memory_scratch_emission_test>)
set_tests_properties(lumen_fex_memory_scratch_emission PROPERTIES TIMEOUT 30 LABELS "macos;fexcore;memory;handoff;jit")

add_executable(lumen_macos_fexcore_memory_access_test EXCLUDE_FROM_ALL
  MacOSFexCoreMemoryAccessTest.cpp)
target_include_directories(lumen_macos_fexcore_memory_access_test PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp")
target_compile_features(lumen_macos_fexcore_memory_access_test PRIVATE cxx_std_20)
target_compile_options(lumen_macos_fexcore_memory_access_test PRIVATE
  -Wall -Wextra -Werror)
add_test(NAME lumen_macos_fexcore_memory_access
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_memory_access_test>)
set_tests_properties(lumen_macos_fexcore_memory_access PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;virtual-memory")

add_executable(lumen_fex_structural_diagnostics_test EXCLUDE_FROM_ALL
  FexStructuralDiagnosticsTest.cpp)
target_include_directories(lumen_fex_structural_diagnostics_test PRIVATE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp")
target_compile_features(lumen_fex_structural_diagnostics_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_structural_diagnostics_test PRIVATE
  -Wall -Wextra -Werror)
add_test(NAME lumen_fex_structural_diagnostics
  COMMAND $<TARGET_FILE:lumen_fex_structural_diagnostics_test>)
set_tests_properties(lumen_fex_structural_diagnostics PROPERTIES
  TIMEOUT 30 LABELS "fexcore;diagnostics")

add_executable(lumen_fex_block_structure_test EXCLUDE_FROM_ALL
  FexBlockStructureTest.cpp)
target_link_libraries(lumen_fex_block_structure_test PRIVATE lumen_macos_fexcore_runtime)
target_include_directories(lumen_fex_block_structure_test PRIVATE
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_compile_features(lumen_fex_block_structure_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_block_structure_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lumen_fex_block_structure
  COMMAND $<TARGET_FILE:lumen_fex_block_structure_test>)
set_tests_properties(lumen_fex_block_structure PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;diagnostics")

add_executable(lumen_fex_ir_use_budget_test EXCLUDE_FROM_ALL
  FexIRUseBudgetTest.cpp)
target_link_libraries(lumen_fex_ir_use_budget_test PRIVATE lumen_macos_fexcore_runtime)
target_include_directories(lumen_fex_ir_use_budget_test PRIVATE
  "${LUMEN_MACOS_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_compile_features(lumen_fex_ir_use_budget_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_ir_use_budget_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME lumen_fex_ir_use_budget
  COMMAND $<TARGET_FILE:lumen_fex_ir_use_budget_test>)
set_tests_properties(lumen_fex_ir_use_budget PROPERTIES
  TIMEOUT 30 LABELS "macos;fexcore;ir")

# Keep the original explicit qualification command. Ordinary and hardened
# regression gates now require the same independent DAZ/FTZ numerical cases.
add_custom_target(lumen_macos_fexcore_general_denorm_qualification
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_root_state_test>
    --require-general-denormals
  DEPENDS lumen_macos_fexcore_root_state_test
  USES_TERMINAL
  VERBATIM)

add_executable(lumen_fex_memory_permission_test EXCLUDE_FROM_ALL
  "${LUMEN_REPOSITORY_ROOT}/source/FexMemoryPermissionTest.cpp")
target_link_libraries(lumen_fex_memory_permission_test PRIVATE
  core lumen_macos_fexcore_runtime)
target_compile_features(lumen_fex_memory_permission_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_memory_permission_test PRIVATE
  -Wall -Wextra -Werror -Wno-error=deprecated-volatile)
add_test(NAME lumen_fex_memory_permission
  COMMAND $<TARGET_FILE:lumen_fex_memory_permission_test>)
add_test(NAME lumen_fex_memory_permission_snapshot_disabled
  COMMAND "${CMAKE_COMMAND}" -E env LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=0
    $<TARGET_FILE:lumen_fex_memory_permission_test>)
set_tests_properties(lumen_fex_memory_permission
  lumen_fex_memory_permission_snapshot_disabled PROPERTIES
  TIMEOUT 120 LABELS "macos;fexcore;virtual-memory")
foreach(owned_stack_read IN ITEMS disabled enabled)
  if(owned_stack_read STREQUAL "enabled")
    set(owned_stack_read_value 1)
  else()
    set(owned_stack_read_value 0)
  endif()
  add_test(NAME lumen_fex_memory_permission_owned_stack_read_${owned_stack_read}
    COMMAND "${CMAKE_COMMAND}" -E env
      "LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ=${owned_stack_read_value}"
      $<TARGET_FILE:lumen_fex_memory_permission_test> --owned-stack-read-${owned_stack_read})
  set_tests_properties(lumen_fex_memory_permission_owned_stack_read_${owned_stack_read} PROPERTIES
    TIMEOUT 120 LABELS "macos;fexcore;virtual-memory;owned-stack")
endforeach()
foreach(owned_stack_read IN ITEMS default empty malformed)
  if(owned_stack_read STREQUAL "default")
    set(owned_stack_selector --unset=LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ)
    set(owned_stack_expected enabled)
  elseif(owned_stack_read STREQUAL "empty")
    set(owned_stack_selector LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ=)
    set(owned_stack_expected disabled)
  else()
    set(owned_stack_selector LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ=01)
    set(owned_stack_expected disabled)
  endif()
  add_test(NAME lumen_fex_memory_permission_owned_stack_read_${owned_stack_read}
    COMMAND "${CMAKE_COMMAND}" -E env ${owned_stack_selector}
      $<TARGET_FILE:lumen_fex_memory_permission_test> --owned-stack-read-${owned_stack_expected})
  set_tests_properties(lumen_fex_memory_permission_owned_stack_read_${owned_stack_read} PROPERTIES
    TIMEOUT 120 LABELS "macos;fexcore;virtual-memory;owned-stack;selector")
endforeach()

# This executable registers a synthetic permission oracle through the product
# handoff providers. Its subpage table cannot enable mixed product VM mappings.
add_executable(lumen_fex_guest_memory_exact_test EXCLUDE_FROM_ALL
  "${LUMEN_REPOSITORY_ROOT}/source/FexGuestMemoryExactTest.cpp")
target_link_libraries(lumen_fex_guest_memory_exact_test PRIVATE
  core lumen_macos_fexcore_runtime)
target_compile_features(lumen_fex_guest_memory_exact_test PRIVATE cxx_std_20)
target_compile_options(lumen_fex_guest_memory_exact_test PRIVATE
  -Wall -Wextra -Werror -Wno-error=deprecated-volatile)
add_test(NAME lumen_fex_guest_memory_exact_experiment
  COMMAND $<TARGET_FILE:lumen_fex_guest_memory_exact_test>)
add_test(NAME lumen_fex_guest_memory_exact_snapshot_disabled
  COMMAND "${CMAKE_COMMAND}" -E env LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=0
    $<TARGET_FILE:lumen_fex_guest_memory_exact_test>)
set_tests_properties(lumen_fex_guest_memory_exact_experiment
  lumen_fex_guest_memory_exact_snapshot_disabled PROPERTIES
  TIMEOUT 45 LABELS "macos;fexcore;virtual-memory;experimental")

find_program(LUMEN_FEX_CODESIGN codesign REQUIRED)
foreach(LUMEN_FEX_HARDENED_CASE IN ITEMS jit runtime jit_denied root_state signal)
  if(LUMEN_FEX_HARDENED_CASE STREQUAL "jit")
    set(LUMEN_FEX_HARDENED_TARGET lumen_macos_fexcore_jit_test)
  elseif(LUMEN_FEX_HARDENED_CASE STREQUAL "root_state")
    set(LUMEN_FEX_HARDENED_TARGET lumen_macos_fexcore_root_state_test)
  elseif(LUMEN_FEX_HARDENED_CASE STREQUAL "signal")
    set(LUMEN_FEX_HARDENED_TARGET lumen_macos_fexcore_signal_policy_test)
  else()
    set(LUMEN_FEX_HARDENED_TARGET lumen_macos_fexcore_runtime_probe)
  endif()
  add_test(NAME lumen_macos_fexcore_hardened_${LUMEN_FEX_HARDENED_CASE}
    COMMAND "${CMAKE_COMMAND}"
      "-DBINARY=$<TARGET_FILE:${LUMEN_FEX_HARDENED_TARGET}>"
      "-DCODESIGN=${LUMEN_FEX_CODESIGN}"
      "-DCASE=${LUMEN_FEX_HARDENED_CASE}"
      -P "${CMAKE_CURRENT_LIST_DIR}/RunHardenedTest.cmake")
endforeach()
set_tests_properties(lumen_macos_fexcore_jit
  lumen_macos_fexcore_jit_blocked_signal
  lumen_macos_fexcore_root_state
  lumen_macos_fexcore_hardened_root_state
  lumen_macos_fexcore_hardened_jit
  lumen_macos_fexcore_hardened_runtime
  lumen_macos_fexcore_hardened_jit_denied
  lumen_macos_fexcore_hardened_signal
  PROPERTIES TIMEOUT 120 LABELS "macos;fexcore;jit;hardened")

add_test(NAME lumen_macos_fexcore_signal_policy
  COMMAND $<TARGET_FILE:lumen_macos_fexcore_signal_policy_test>)

add_test(NAME lumen_macos_fexcore_runtime_default
  COMMAND "${CMAKE_COMMAND}" -E env
    --unset=LUMEN_MACOS_FEXCORE_RUN
    $<TARGET_FILE:lumen_macos_fexcore_runtime_probe>)
add_test(NAME lumen_macos_fexcore_runtime_explicit_one
  COMMAND "${CMAKE_COMMAND}" -E env
    LUMEN_MACOS_FEXCORE_RUN=1
    $<TARGET_FILE:lumen_macos_fexcore_runtime_probe>)
foreach(LUMEN_MACOS_FEXCORE_DISABLED_VALUE IN ITEMS zero empty malformed)
  if(LUMEN_MACOS_FEXCORE_DISABLED_VALUE STREQUAL "zero")
    set(LUMEN_MACOS_FEXCORE_SELECTOR_VALUE 0)
  elseif(LUMEN_MACOS_FEXCORE_DISABLED_VALUE STREQUAL "empty")
    set(LUMEN_MACOS_FEXCORE_SELECTOR_VALUE "")
  else()
    set(LUMEN_MACOS_FEXCORE_SELECTOR_VALUE 01)
  endif()
  add_test(NAME lumen_macos_fexcore_runtime_${LUMEN_MACOS_FEXCORE_DISABLED_VALUE}
    COMMAND "${CMAKE_COMMAND}" -E env
      "LUMEN_MACOS_FEXCORE_RUN=${LUMEN_MACOS_FEXCORE_SELECTOR_VALUE}"
      $<TARGET_FILE:lumen_macos_fexcore_runtime_probe>
      --expect-admission-denied)
endforeach()
set_tests_properties(
  lumen_macos_fexcore_signal_policy
  lumen_macos_fexcore_runtime_default
  lumen_macos_fexcore_runtime_explicit_one
  lumen_macos_fexcore_runtime_zero
  lumen_macos_fexcore_runtime_empty
  lumen_macos_fexcore_runtime_malformed
  PROPERTIES TIMEOUT 120 LABELS "macos;fexcore;runtime")

# These historical probes inspect FEX-specific admission and checkpoint
# contracts. Pin their provider independently of explicit runtime overrides.
get_property(lumen_fex_registered_tests DIRECTORY PROPERTY TESTS)
foreach(lumen_fex_test IN LISTS lumen_fex_registered_tests)
  if(lumen_fex_test MATCHES "^lumen_(macos_fexcore_|fex_)")
    set_property(TEST "${lumen_fex_test}" APPEND PROPERTY
      ENVIRONMENT "LUMEN_CPU_TRANSLATOR=fex")
  endif()
endforeach()
