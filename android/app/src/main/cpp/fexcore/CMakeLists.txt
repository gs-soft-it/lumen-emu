cmake_minimum_required(VERSION 3.22.1)

# This directory is intentionally entered only by the explicitly admitted
# Android fexLab flavor.  Keep all upstream-compatible definitions scoped here
# so the baseline Lumen build is unaffected.
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "The Lumen FEXCore adapter supports Android arm64-v8a only")
endif()
if(NOT DEFINED LUMEN_ANDROID_FEXCORE_SOURCE_REAL OR
   NOT IS_DIRECTORY "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}")
    message(FATAL_ERROR "The validated external FEXCore checkout is unavailable")
endif()

# Build the reviewed Android page-size port in a content-verified external
# overlay. The official checkout and the macOS port are never modified.
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
set(LUMEN_ANDROID_FEX_OVERLAY_OUTPUT_ARGS)
if(LUMEN_ANDROID_FEXCORE_SOURCE_REAL STREQUAL LUMEN_BUNDLED_FEX_SOURCE_DIR)
    list(APPEND LUMEN_ANDROID_FEX_OVERLAY_OUTPUT_ARGS
        --output-dir "${LUMEN_REPOSITORY_ROOT}/_Build/fex-sources")
endif()
if(LUMEN_PS5_FEX_C13)
    include("${CMAKE_CURRENT_LIST_DIR}/C13Overlay.cmake")
else()
set(LUMEN_ANDROID_FEX_HOST_PAGES_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-android-host-pages.patch")
set(LUMEN_ANDROID_FEX_HOST_PAGES_PATCH_SHA256
    "e38b3c2a2d9f8c799ff50b9bc9d1333c64709c27f88e7422f5ec811013f8a953" CACHE INTERNAL "Reviewed Android FEX host-page patch SHA256" FORCE)
# Same decoded-block exit-flag lifecycle correction as the macOS overlay:
# the upstream declaration and GenerateIR block boundary are identical.
set(LUMEN_ANDROID_FEX_BLOCK_EXIT_RESET_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-block-set-rip-reset.patch")
set(LUMEN_ANDROID_FEX_BLOCK_EXIT_RESET_PATCH_SHA256
    "190ca0c02351d837f31c60a11d1227331213c389f1c20757288fc1486badbd26" CACHE INTERNAL "Reviewed Android FEX block exit reset patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_BLOCK_RIP_METADATA_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-decoded-block-rip-metadata.patch")
set(LUMEN_ANDROID_FEX_BLOCK_RIP_METADATA_PATCH_SHA256
    "b64055b5de69a91cee6ba46ff9273118ec0abdf1ff4a38fba9f855155c91c96c" CACHE INTERNAL "Reviewed Android FEX decoded block RIP metadata patch SHA256" FORCE)
# Both admitted pins expose the same numerical IR interfaces. Reuse the
# reviewed shared legalizer and bounded-use repair without copying a second
# implementation; the preparer verifies that they apply to this exact pin.
set(LUMEN_ANDROID_FEX_DENORMAL_PATCH
    "${LUMEN_SOURCE_ROOT}/../macos/fexcore-runtime/patches/fex-c9c5a132-independent-denormals.patch")
set(LUMEN_ANDROID_FEX_DENORMAL_PATCH_SHA256
    "7dd066b03c747c5a5a0d5be6c5bb97b8e9aa9bfcb59c5fe6287b42c3d63d87ee" CACHE INTERNAL "Reviewed shared FEX independent-denormal patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_DENORMAL_USE_BUDGET_PATCH
    "${LUMEN_SOURCE_ROOT}/../macos/fexcore-runtime/patches/fex-c9c5a132-denormal-use-budget.patch")
set(LUMEN_ANDROID_FEX_DENORMAL_USE_BUDGET_PATCH_SHA256
    "59ae4ddeac57c4b656872d52c56da61c8a9f8438e6d5af2bb89c62f9aa543681" CACHE INTERNAL "Reviewed shared FEX denormal-use-budget patch SHA256" FORCE)
# Both pins suppress IR warnings before logging. Remove only their discarded
# formatting; all hard validation checks and diagnostics remain enabled.
set(LUMEN_ANDROID_FEX_IR_VALIDATION_WARNING_PATCH
    "${LUMEN_SOURCE_ROOT}/../macos/fexcore-runtime/patches/fex-c9c5a132-ir-validation-warning-format.patch")
set(LUMEN_ANDROID_FEX_IR_VALIDATION_WARNING_PATCH_SHA256
    "6d33f17ac268bc00cf330cbb03a15d3f8ca2b8b7cc7167ed3fde8318732512dd" CACHE INTERNAL "Reviewed shared FEX IR validation warning patch SHA256" FORCE)
# Opt-in symbol output for an embedding Android app; default naming is off.
set(LUMEN_ANDROID_FEX_APP_JIT_SYMBOLS_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-android-app-jit-symbols.patch")
set(LUMEN_ANDROID_FEX_APP_JIT_SYMBOLS_PATCH_SHA256
    "f99eaa54a0dfa1dd2f52186d7d6fcc61dc24f659112b5ad159ba93559b02674e" CACHE INTERNAL "Reviewed Android app JIT symbols patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_MEMORY_HANDOFF_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-memory-handoff.patch")
set(LUMEN_ANDROID_FEX_MEMORY_HANDOFF_PATCH_SHA256
    "d52cb46fd267cec0139db23701e3555de29122acac119fc37ec76fe4b44f06ed" CACHE INTERNAL "Android FEX memory handoff patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_JIT_RESTART_METADATA_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-jit-restart-metadata.patch")
set(LUMEN_ANDROID_FEX_JIT_RESTART_METADATA_PATCH_SHA256
    "340bbf85c58a15e8ec0484a14bed4edbaed0b502b3c355fabdd1a0e5385361cd" CACHE INTERNAL "Android FEX JIT restart metadata patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_UNWATCHED_REP_BULK_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-unwatched-rep-bulk.patch")
set(LUMEN_ANDROID_FEX_UNWATCHED_REP_BULK_PATCH_SHA256
    "08d828b978a104418788b3afbba803a158fc12d6267614a5c1d3db984845e25f" CACHE INTERNAL "Android FEX unwatched REP bulk patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_JIT_CACHE_DIAGNOSTICS_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-jit-cache-diagnostics.patch")
set(LUMEN_ANDROID_FEX_JIT_CACHE_DIAGNOSTICS_PATCH_SHA256
    "740e2e2ab601036e95a3a603d8e52399c326afc2b53e3497f735c0ca2b23c2d9" CACHE INTERNAL "Android FEX cold JIT cache diagnostics patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_MEMORY_PRESERVE_ALL_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-memory-handoff-preserve-all.patch")
set(LUMEN_ANDROID_FEX_MEMORY_PRESERVE_ALL_PATCH_SHA256
    "99716ee9cad2e93d8d2ed3a38b2b6f22bd5fd60c27b93aa3bb75f4a3c7811d88" CACHE INTERNAL "Android FEX preserving memory handoff ABI patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_MEMORY_READ_DIRECTORY_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-memory-handoff-read-directory.patch")
set(LUMEN_ANDROID_FEX_MEMORY_READ_DIRECTORY_PATCH_SHA256
    "6aa2aad58c4411cbb44cd10c67e88bdeee0ed3d8aae65b6f6f8d0d9c1ebb885f" CACHE INTERNAL "Android FEX clean-read handoff directory patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_GUEST_PROGRESS_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-guest-progress-witness.patch")
set(LUMEN_ANDROID_FEX_GUEST_PROGRESS_PATCH_SHA256
    "1acc6d245512d6506318aa822ff3164c87fed79980c76d148319a457f39b18ab" CACHE INTERNAL "Android FEX guest progress witness patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_MEMORY_SCRATCH_LIVENESS_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-memory-handoff-scratch-liveness.patch")
set(LUMEN_ANDROID_FEX_MEMORY_SCRATCH_LIVENESS_PATCH_SHA256
    "e005bf7a11d9b58a7e98e62770f6d93ed16a529c86ac741c78ea88c12846de67" CACHE INTERNAL "Android FEX memory scratch liveness patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_JIT_SYMBOL_RANGES_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-jit-symbol-ranges.patch")
set(LUMEN_ANDROID_FEX_JIT_SYMBOL_RANGES_PATCH_SHA256
    "9814733642a8f13d6565c6e91618c8d221fdb8eaadbf79c1b6a3ad73db456105" CACHE INTERNAL "Android FEX diagnostic symbol ranges patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_MEMORY_FASTPATH_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-memory-handoff-fastpath.patch")
set(LUMEN_ANDROID_FEX_MEMORY_FASTPATH_PATCH_SHA256
    "3b4fb269b15d51cfe59bcd9c8af9a95316b50a360ca354947a62742562fe1a8e" CACHE INTERNAL "Android FEX retained-address memory guard patch SHA256" FORCE)
set(LUMEN_ANDROID_FEX_MEMORY_SHARED_THUNK_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-e17fdf9-memory-handoff-shared-thunk.patch")
set(LUMEN_ANDROID_FEX_MEMORY_SHARED_THUNK_PATCH_SHA256
    "1f3ff242cfca75aec251c172ebfcf83792f0ce6f5de329ca14fa4df58d0acf88" CACHE INTERNAL "Android FEX per-block memory handoff thunk patch SHA256" FORCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/prepare_source_overlay.py"
    "${LUMEN_ANDROID_FEX_HOST_PAGES_PATCH}"
    "${LUMEN_ANDROID_FEX_BLOCK_EXIT_RESET_PATCH}"
    "${LUMEN_ANDROID_FEX_BLOCK_RIP_METADATA_PATCH}"
    "${LUMEN_ANDROID_FEX_DENORMAL_PATCH}"
    "${LUMEN_ANDROID_FEX_DENORMAL_USE_BUDGET_PATCH}"
    "${LUMEN_ANDROID_FEX_APP_JIT_SYMBOLS_PATCH}"
    "${LUMEN_ANDROID_FEX_MEMORY_HANDOFF_PATCH}"
    "${LUMEN_ANDROID_FEX_JIT_RESTART_METADATA_PATCH}"
    "${LUMEN_ANDROID_FEX_UNWATCHED_REP_BULK_PATCH}"
    "${LUMEN_ANDROID_FEX_IR_VALIDATION_WARNING_PATCH}"
    "${LUMEN_ANDROID_FEX_JIT_CACHE_DIAGNOSTICS_PATCH}"
    "${LUMEN_ANDROID_FEX_MEMORY_PRESERVE_ALL_PATCH}"
    "${LUMEN_ANDROID_FEX_MEMORY_READ_DIRECTORY_PATCH}"
    "${LUMEN_ANDROID_FEX_GUEST_PROGRESS_PATCH}"
    "${LUMEN_ANDROID_FEX_MEMORY_SCRATCH_LIVENESS_PATCH}"
    "${LUMEN_ANDROID_FEX_JIT_SYMBOL_RANGES_PATCH}"
    "${LUMEN_ANDROID_FEX_MEMORY_FASTPATH_PATCH}"
    "${LUMEN_ANDROID_FEX_MEMORY_SHARED_THUNK_PATCH}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/prepare_source_overlay.py"
        ${LUMEN_ANDROID_FEX_OVERLAY_OUTPUT_ARGS}
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}"
        "${LUMEN_ANDROID_FEXCORE_PINNED_COMMIT}"
        "${LUMEN_ANDROID_FEX_HOST_PAGES_PATCH}=${LUMEN_ANDROID_FEX_HOST_PAGES_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_BLOCK_EXIT_RESET_PATCH}=${LUMEN_ANDROID_FEX_BLOCK_EXIT_RESET_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_BLOCK_RIP_METADATA_PATCH}=${LUMEN_ANDROID_FEX_BLOCK_RIP_METADATA_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_DENORMAL_PATCH}=${LUMEN_ANDROID_FEX_DENORMAL_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_DENORMAL_USE_BUDGET_PATCH}=${LUMEN_ANDROID_FEX_DENORMAL_USE_BUDGET_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_APP_JIT_SYMBOLS_PATCH}=${LUMEN_ANDROID_FEX_APP_JIT_SYMBOLS_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_MEMORY_HANDOFF_PATCH}=${LUMEN_ANDROID_FEX_MEMORY_HANDOFF_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_JIT_RESTART_METADATA_PATCH}=${LUMEN_ANDROID_FEX_JIT_RESTART_METADATA_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_UNWATCHED_REP_BULK_PATCH}=${LUMEN_ANDROID_FEX_UNWATCHED_REP_BULK_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_IR_VALIDATION_WARNING_PATCH}=${LUMEN_ANDROID_FEX_IR_VALIDATION_WARNING_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_JIT_CACHE_DIAGNOSTICS_PATCH}=${LUMEN_ANDROID_FEX_JIT_CACHE_DIAGNOSTICS_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_MEMORY_PRESERVE_ALL_PATCH}=${LUMEN_ANDROID_FEX_MEMORY_PRESERVE_ALL_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_MEMORY_READ_DIRECTORY_PATCH}=${LUMEN_ANDROID_FEX_MEMORY_READ_DIRECTORY_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_GUEST_PROGRESS_PATCH}=${LUMEN_ANDROID_FEX_GUEST_PROGRESS_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_MEMORY_SCRATCH_LIVENESS_PATCH}=${LUMEN_ANDROID_FEX_MEMORY_SCRATCH_LIVENESS_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_JIT_SYMBOL_RANGES_PATCH}=${LUMEN_ANDROID_FEX_JIT_SYMBOL_RANGES_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_MEMORY_FASTPATH_PATCH}=${LUMEN_ANDROID_FEX_MEMORY_FASTPATH_PATCH_SHA256}"
        "${LUMEN_ANDROID_FEX_MEMORY_SHARED_THUNK_PATCH}=${LUMEN_ANDROID_FEX_MEMORY_SHARED_THUNK_PATCH_SHA256}"
    OUTPUT_VARIABLE LUMEN_ANDROID_FEX_PATCHED_SOURCE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LUMEN_ANDROID_FEX_OVERLAY_RESULT)
if(NOT LUMEN_ANDROID_FEX_OVERLAY_RESULT EQUAL 0 OR
   NOT IS_DIRECTORY "${LUMEN_ANDROID_FEX_PATCHED_SOURCE}")
    message(FATAL_ERROR "Could not verify the reviewed Android FEX source overlay")
endif()
# Cache the effective build root for the parent public-header probe and APK
# artifact audit. SOURCE_DIR still identifies the unmodified upstream checkout.
set(LUMEN_ANDROID_FEXCORE_BUILD_SOURCE_REAL "${LUMEN_ANDROID_FEX_PATCHED_SOURCE}"
    CACHE INTERNAL "Verified Android FEX host-page overlay" FORCE)
set(LUMEN_ANDROID_FEXCORE_SOURCE_REAL "${LUMEN_ANDROID_FEX_PATCHED_SOURCE}")

endif()

enable_language(C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
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
set(GIT_DESCRIBE_STRING "FEX-e17fdf9")
set(GIT_HASH_ARRAY
    "0xe1, 0x7f, 0xdf, 0x9e, 0x90, 0xa4, 0x96, 0x14, 0xe1, 0x6a, 0xc8, 0x41, 0x4a, 0x68, 0x74, 0x77, 0xfb, 0x9e, 0x4f, 0x7c")

if(LUMEN_PS5_FEX_C13)
    set(GIT_DESCRIBE_STRING "FEX-f2b679f6-ps5-c13")
    set(GIT_HASH_ARRAY "0xf2, 0xb6, 0x79, 0xf6, 0x02, 0x8c, 0xe1, 0xc3, 0x88, 0x75, 0x23, 0x3a, 0xec, 0xfc, 0xf5, 0xd3, 0xf8, 0xeb, 0xec, 0xec")
endif()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-Wattributes -Werror=attributes")
check_cxx_source_compiles(
    "__attribute__((preserve_all)) int f(int x) { return x; }
     int main() { return f(1); }"
    HAS_CLANG_PRESERVE_ALL)
unset(CMAKE_REQUIRED_FLAGS)

add_compile_definitions(
    ARCHITECTURE_arm64=1
    ASSERTIONS_ENABLED=1
    _LARGEFILE64_SOURCE
    FEX_DISABLE_TELEMETRY=1
    GLOBAL_DATA_DIRECTORY=""
)
if(HAS_CLANG_PRESERVE_ALL)
    add_compile_definitions(
        "FEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))"
        FEX_HAS_PRESERVE_ALL_ATTR=1
    )
else()
    add_compile_definitions(
        FEX_PRESERVE_ALL_ATTR=
        FEX_HAS_PRESERVE_ALL_ATTR=0
    )
endif()

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(XXHASH_BUNDLED_MODE TRUE CACHE BOOL "" FORCE)
set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)

# The parent Android graph adds its Debug-runtime optimization flags only after
# this opt-in subtree has already been configured. Pin the complete FEX closure
# (including the local adapter) to the laboratory's explicit -O2 profile while
# retaining Debug symbols and the FEX assertions enabled above.
add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-O2>)

add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/External/fmt"
    fex-fmt EXCLUDE_FROM_ALL)
add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/External/xxhash/cmake_unofficial"
    fex-xxhash EXCLUDE_FROM_ALL)
add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/External/unordered_dense"
    fex-unordered-dense EXCLUDE_FROM_ALL)
add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/External/rpmalloc"
    fex-rpmalloc EXCLUDE_FROM_ALL)
add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/External/SoftFloat-3e"
    fex-softfloat EXCLUDE_FROM_ALL)
add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/External/cephes"
    fex-cephes EXCLUDE_FROM_ALL)
add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXHeaderUtils"
    fex-header-utils EXCLUDE_FROM_ALL)
add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/CodeEmitter"
    fex-code-emitter EXCLUDE_FROM_ALL)

list(APPEND CMAKE_MODULE_PATH
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/Data/CMake")
include(LinkerGC)
add_subdirectory(
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore"
    fex-core EXCLUDE_FROM_ALL)

add_library(lumen_android_fexcore_runtime STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../AndroidFexCoreRuntime.cpp"
    "${LUMEN_SOURCE_ROOT}/cpu/runtime/GuestCpuRuntime.cpp"
    "${LUMEN_SOURCE_ROOT}/cpu/runtime/GuestCpuFexProvider.cpp"
)
get_filename_component(LUMEN_CPU_REPOSITORY_ROOT "${LUMEN_SOURCE_ROOT}/.." ABSOLUTE)
include("${LUMEN_CPU_REPOSITORY_ROOT}/source/cpu/qemu/Provider.cmake")
lumen_add_qemu_provider(lumen_android_fexcore_runtime "${LUMEN_CPU_REPOSITORY_ROOT}")
target_include_directories(lumen_android_fexcore_runtime
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/.."
        "${LUMEN_SOURCE_ROOT}/cpu/runtime"
    PRIVATE
        # Pin-private interfaces used by the adapter: repair Custom-IR decoder
        # metadata on every cache generation, exercise deterministic cache
        # replacement, and enable FEX's existing block-boundary interrupt
        # checkpoints for cooperative guest-memory mutation safepoints.
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore/Source"
)
target_compile_features(lumen_android_fexcore_runtime PRIVATE cxx_std_20)
target_compile_definitions(lumen_android_fexcore_runtime PRIVATE
    LUMEN_ANDROID_FEXCORE_OPTIMIZATION_LEVEL=2
    LUMEN_ANDROID_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED=$<IF:$<BOOL:${LUMEN_PS5_FEX_C13}>,0,1>
)
target_compile_definitions(lumen_android_fexcore_runtime PUBLIC
    "$<$<BOOL:${LUMEN_PS5_FEX_C13}>:LUMEN_PS5_FEX_C13=1>")
include("${LUMEN_CPU_REPOSITORY_ROOT}/macos/fexcore-runtime/FexTranslationCacheBuildIdentity.cmake")
lumen_fex_translation_cache_build_identity(lumen_android_fexcore_runtime
    "${LUMEN_CPU_REPOSITORY_ROOT}" "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}")
target_compile_options(lumen_android_fexcore_runtime PRIVATE
    -fno-exceptions
    -fno-rtti
    -fno-strict-aliasing
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Werror
)
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/../AndroidFexCoreRuntime.cpp"
    PROPERTIES COMPILE_OPTIONS "-fexceptions;-frtti")
target_link_libraries(lumen_android_fexcore_runtime PUBLIC
    FEXCore
    FEXCore_Base
    JemallocLibs
)

# Explicit synthetic device gate. It is never packaged in the APK and does not
# load game inputs; EXCLUDE_FROM_ALL keeps ordinary builds unchanged.
add_executable(lumen_android_fex_memory_handoff_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/../../../test/cpp/AndroidFexCoreMemoryHandoffTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../AndroidFexCoreLabAdmission.cpp")
target_compile_features(lumen_android_fex_memory_handoff_test PRIVATE cxx_std_20)
target_compile_options(lumen_android_fex_memory_handoff_test PRIVATE -O2 -Wall -Wextra -Werror)
target_include_directories(lumen_android_fex_memory_handoff_test PRIVATE
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_compile_definitions(lumen_android_fex_memory_handoff_test PRIVATE
    LUMEN_ANDROID_FEXCORE_LAB_BUILD=1 LUMEN_ANDROID_FULL_RUNTIME_BUILD=1
    "$<$<CONFIG:Debug>:LUMEN_ANDROID_FEXCORE_BUILD_DEBUG=1>"
    "$<$<NOT:$<CONFIG:Debug>>:LUMEN_ANDROID_FEXCORE_BUILD_RELEASE=1>")
target_link_libraries(lumen_android_fex_memory_handoff_test PRIVATE
    lumen_android_fexcore_runtime log dl)

add_executable(lumen_android_fex_memory_handoff_policy_test EXCLUDE_FROM_ALL
    "${LUMEN_CPU_REPOSITORY_ROOT}/macos/fexcore-runtime/FexMemoryHandoffPolicyTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../AndroidFexCoreLabAdmission.cpp")
target_compile_features(lumen_android_fex_memory_handoff_policy_test PRIVATE cxx_std_20)
target_compile_options(lumen_android_fex_memory_handoff_policy_test PRIVATE -O2 -Wall -Wextra -Werror)
target_include_directories(lumen_android_fex_memory_handoff_policy_test PRIVATE
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_compile_definitions(lumen_android_fex_memory_handoff_policy_test PRIVATE
    LUMEN_ANDROID_FEXCORE_LAB_BUILD=1 LUMEN_ANDROID_FULL_RUNTIME_BUILD=1
    "$<$<CONFIG:Debug>:LUMEN_ANDROID_FEXCORE_BUILD_DEBUG=1>"
    "$<$<NOT:$<CONFIG:Debug>>:LUMEN_ANDROID_FEXCORE_BUILD_RELEASE=1>")
target_link_libraries(lumen_android_fex_memory_handoff_policy_test PRIVATE lumen_android_fexcore_runtime log dl)

# Bind cached native code to the exact preserving ABI, including embeddings
# without that provider. The fixture owns its providers and links no runtime.
add_executable(lumen_android_fexcore_preserve_all_cache_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/../../../test/cpp/AndroidFexCorePreserveAllCacheTest.cpp")
target_compile_features(lumen_android_fexcore_preserve_all_cache_test PRIVATE cxx_std_20)
target_compile_definitions(lumen_android_fexcore_preserve_all_cache_test PRIVATE
    LUMEN_ANDROID_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED=1)
target_compile_options(lumen_android_fexcore_preserve_all_cache_test PRIVATE -O2 -Wall -Wextra -Werror)
target_include_directories(lumen_android_fexcore_preserve_all_cache_test PRIVATE
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_link_libraries(lumen_android_fexcore_preserve_all_cache_test PRIVATE
    FEXCore FEXCore_Base JemallocLibs log dl)
target_link_options(lumen_android_fexcore_preserve_all_cache_test PRIVATE
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")

add_executable(lumen_android_fex_memory_scratch_emission_test EXCLUDE_FROM_ALL
    "${LUMEN_CPU_REPOSITORY_ROOT}/macos/fexcore-runtime/FexMemoryScratchEmissionTest.cpp")
target_compile_features(lumen_android_fex_memory_scratch_emission_test PRIVATE cxx_std_20)
target_compile_options(lumen_android_fex_memory_scratch_emission_test PRIVATE -O2 -Wall -Wextra -Werror)
target_include_directories(lumen_android_fex_memory_scratch_emission_test PRIVATE
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore/Source")
target_link_libraries(lumen_android_fex_memory_scratch_emission_test PRIVATE
    FEXCore FEXCore_Base JemallocLibs log dl)
target_link_options(lumen_android_fex_memory_scratch_emission_test PRIVATE
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
