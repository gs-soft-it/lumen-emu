# SPDX-License-Identifier: MIT
# Isolated Gen4 translator closure, shared by Android and native macOS.
# The PS5 providers retain their independent graphs.
set(LUMEN_GEN4_DIRECT_FEX_SOURCE_DIR
    "${LUMEN_REPOSITORY_ROOT}/source/3rdparty/fex-gen4"
    CACHE PATH "Pinned direct Gen4 CPU source")
set(gen4_fex_source "${LUMEN_GEN4_DIRECT_FEX_SOURCE_DIR}")
execute_process(COMMAND git -C "${gen4_fex_source}" rev-parse HEAD
    OUTPUT_VARIABLE gen4_fex_revision OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE gen4_fex_git_result)
if(NOT gen4_fex_git_result EQUAL 0 OR
   NOT gen4_fex_revision STREQUAL "f2b679f6028ce1c38875233aecfcf5d3f8ebecec")
    message(FATAL_ERROR "Direct Gen4 CPU source does not match its pinned revision")
endif()
if(APPLE)
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/prepare_darwin.py"
        "${CMAKE_CURRENT_LIST_DIR}/fex-f2b679f-darwin.patch")
    file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/prepare_darwin.py" preparer_identity)
    file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/fex-f2b679f-darwin.patch" darwin_patch_identity)
    string(SHA256 overlay_identity "${preparer_identity};${darwin_patch_identity}")
    string(SUBSTRING "${overlay_identity}" 0 16 overlay_key)
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/prepare_darwin.py" "${gen4_fex_source}"
        "${CMAKE_CURRENT_BINARY_DIR}/upstream-source-${overlay_key}"
        RESULT_VARIABLE preparation_status OUTPUT_VARIABLE gen4_fex_source
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT preparation_status EQUAL 0)
        message(FATAL_ERROR "Could not prepare pinned Gen4 FEX Darwin host overlay")
    endif()
    include(CheckCXXSourceRuns)
    check_cxx_source_runs("#include <pthread.h>
        #include <unistd.h>
        int main() { return sysconf(_SC_PAGESIZE) == 16384 &&
            pthread_jit_write_protect_supported_np() ? 0 : 1; }"
        LUMEN_GEN4_NATIVE_DARWIN_JIT)
    if(NOT LUMEN_GEN4_NATIVE_DARWIN_JIT)
        message(FATAL_ERROR "Gen4 FEX requires native ARM64, 16 KiB host pages and JIT W^X")
    endif()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-ffixed-x18>)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(BUILD_STEAM_SUPPORT OFF)
set(ARCHITECTURE_arm64 1)
set(ENABLE_FEX_ALLOCATOR ON)
set(ENABLE_JEMALLOC_GLIBC_ALLOC OFF)
set(ENABLE_FEXCORE_PROFILER OFF)
set(ENABLE_GLIBC_ALLOCATOR_HOOK_FAULT OFF)
set(ENABLE_VIXL_DISASSEMBLER OFF)
set(ENABLE_VIXL_SIMULATOR OFF)
set(ENABLE_ZYDIS OFF)
set(FEX_TUNE_COMPILE_FLAGS "")
set(GIT_DESCRIBE_STRING "FEX-2607-179-gf2b679f")
set(GIT_HASH_ARRAY "0xf2, 0xb6, 0x79, 0xf6, 0x02, 0x8c, 0xe1, 0xc3, 0x88, 0x75, 0x23, 0x3a, 0xec, 0xfc, 0xf5, 0xd3, 0xf8, 0xeb, 0xec, 0xec")
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-Wattributes -Werror=attributes")
check_cxx_source_compiles("__attribute__((preserve_all)) int f(int x) { return x; }
int main() { return f(1); }" HAS_CLANG_PRESERVE_ALL)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT HAS_CLANG_PRESERVE_ALL)
    message(FATAL_ERROR "Direct ARM64 CPU requires the preserving call ABI")
endif()
add_compile_definitions(ARCHITECTURE_arm64=1 $<$<CONFIG:Debug>:ASSERTIONS_ENABLED=1> _LARGEFILE64_SOURCE
    FEX_DISABLE_TELEMETRY=1 GLOBAL_DATA_DIRECTORY="/usr/share/fex-emu/"
    FEX_HAS_PRESERVE_ALL_ATTR=1 "FEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))")
# Preserve the upstream Release C++ frame-pointer policy; only the Android
# Debug laboratory configuration supplies an optimization override.
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -fomit-frame-pointer")
add_compile_options($<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C,CXX>>:-O2>)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(XXHASH_BUNDLED_MODE TRUE CACHE BOOL "" FORCE)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
add_subdirectory("${gen4_fex_source}/External/fmt" fmt EXCLUDE_FROM_ALL)
add_subdirectory("${gen4_fex_source}/External/xxhash/cmake_unofficial" xxhash EXCLUDE_FROM_ALL)
add_subdirectory("${gen4_fex_source}/External/unordered_dense" unordered-dense EXCLUDE_FROM_ALL)
add_subdirectory("${gen4_fex_source}/External/rpmalloc" rpmalloc EXCLUDE_FROM_ALL)
add_subdirectory("${gen4_fex_source}/External/SoftFloat-3e" softfloat EXCLUDE_FROM_ALL)
add_subdirectory("${gen4_fex_source}/External/cephes" cephes EXCLUDE_FROM_ALL)
add_subdirectory("${gen4_fex_source}/External/tiny-json" tiny-json EXCLUDE_FROM_ALL)
add_subdirectory("${gen4_fex_source}/FEXHeaderUtils" header-utils EXCLUDE_FROM_ALL)
add_subdirectory("${gen4_fex_source}/CodeEmitter" code-emitter EXCLUDE_FROM_ALL)
list(APPEND CMAKE_MODULE_PATH "${gen4_fex_source}/Data/CMake")
include(LinkerGC)
add_subdirectory("${gen4_fex_source}/FEXCore" core EXCLUDE_FROM_ALL)
add_library(lumen_gen4_fex_host_features STATIC
    "${gen4_fex_source}/Source/Common/HostFeatures.cpp"
    "${gen4_fex_source}/Source/Common/CPUInfo.cpp")
target_include_directories(lumen_gen4_fex_host_features PRIVATE
    "${gen4_fex_source}/Source" "${gen4_fex_source}/External/range-v3/include")
target_link_libraries(lumen_gen4_fex_host_features PUBLIC FEXCore_Base)
add_library(lumen_gen4_fex_config STATIC
    "${gen4_fex_source}/Source/Common/Config.cpp"
    "${gen4_fex_source}/Source/Common/JSONPool.cpp")
target_include_directories(lumen_gen4_fex_config PRIVATE
    "${gen4_fex_source}/Source" "${gen4_fex_source}/External/range-v3/include"
    "${CMAKE_BINARY_DIR}/generated")
if(BUILD_STEAM_SUPPORT)
    target_compile_definitions(lumen_gen4_fex_config PRIVATE FEX_STEAM_SUPPORT=1)
endif()
target_link_libraries(lumen_gen4_fex_config PUBLIC
    FEXCore_Base tiny-json::tiny-json FEXHeaderUtils)
add_executable(lumen_gen4_fex_environment_contract_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/../../tests/fex_environment_contract_test.cpp")
target_include_directories(lumen_gen4_fex_environment_contract_test PRIVATE
    "${gen4_fex_source}/Source")
target_link_libraries(lumen_gen4_fex_environment_contract_test PRIVATE
    lumen_gen4_fex_config FEXCore_Base JemallocLibs ${CMAKE_DL_LIBS})
add_test(NAME lumen_gen4_fex_environment_contract COMMAND lumen_gen4_fex_environment_contract_test)
add_library(lumen_gen4_guest_cpu INTERFACE)
target_compile_definitions(lumen_gen4_guest_cpu INTERFACE
    LUMEN_GEN4_DIRECT_FEX=1 ARCHITECTURE_arm64=1 FEX_DISABLE_TELEMETRY=1
    FEXCORE_HAS_PRESERVE_ALL_ATTR=1 FEX_HAS_PRESERVE_ALL_ATTR=1)
target_compile_options(lumen_gen4_guest_cpu INTERFACE
    "-DFEXCORE_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
    "-DFEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))")
target_include_directories(lumen_gen4_guest_cpu INTERFACE
    "${gen4_fex_source}/Source" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(lumen_gen4_guest_cpu INTERFACE
    FEXCore lumen_gen4_fex_host_features lumen_gen4_fex_config FEXCore_Base JemallocLibs ${CMAKE_DL_LIBS})
if(ANDROID)
    target_link_libraries(lumen_gen4_guest_cpu INTERFACE log)
    target_link_libraries(lumen_gen4_fex_environment_contract_test PRIVATE log)
endif()
