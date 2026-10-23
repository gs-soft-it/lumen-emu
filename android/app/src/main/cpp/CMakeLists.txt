# Core-only source export: application UI targets removed on 2026-10-23.
cmake_minimum_required(VERSION 3.22.1)

project(LumenAndroidSpike LANGUAGES CXX)

if(NOT ANDROID)
    message(FATAL_ERROR "LumenAndroidSpike must be cross-compiled with the Android NDK")
endif()

if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "LumenAndroidSpike supports only arm64-v8a, not ${ANDROID_ABI}")
endif()

if(NOT DEFINED ANDROID_PLATFORM_LEVEL OR ANDROID_PLATFORM_LEVEL LESS 35)
    message(FATAL_ERROR
        "The Android runtime requires API 35+ for both FEX and QEMU, including native TLS")
endif()

option(LUMEN_ANDROID_FULL_RUNTIME
    "Link the complete FEX-only Lumen runtime into the Android APK" ON)

option(LUMEN_ANDROID_FEXCORE_LAB
    "Build the FEX-only Android runtime" ON)
if(NOT LUMEN_ANDROID_FULL_RUNTIME OR NOT LUMEN_ANDROID_FEXCORE_LAB)
    message(FATAL_ERROR
        "Android ARM64 is FEX-only; FULL_RUNTIME and FEXCORE_LAB must both be ON")
endif()
option(LUMEN_PS5_DIRECT_FEX "Use the complete Gen4 direct FEX integration for Android PS5" OFF)
option(LUMEN_PS5_FEX_C13 "Opt in to the C13 FEX pin for the isolated Android PS5 provider" OFF)
if(LUMEN_PS5_DIRECT_FEX AND LUMEN_PS5_FEX_C13)
    message(FATAL_ERROR "The direct PS5 FEX integration and facade candidate are mutually exclusive")
endif()
get_filename_component(LUMEN_BUNDLED_FEX_SOURCE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../source/3rdparty/fex" ABSOLUTE)
set(LUMEN_ANDROID_FEXCORE_SOURCE_DIR "${LUMEN_BUNDLED_FEX_SOURCE_DIR}" CACHE PATH
    "Checkout of the exact pinned FEX revision")
# Gen4 owns a separate process/build; this switch only changes the shared legacy
# facade consumed by PS5. Preserve the configured baseline checkout for rollback.
if(LUMEN_PS5_FEX_C13 OR LUMEN_PS5_DIRECT_FEX)
    get_filename_component(LUMEN_ANDROID_FEXCORE_SOURCE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../../../../source/3rdparty/fex-gen4" ABSOLUTE)
endif()
set(LUMEN_ANDROID_FEXCORE_EFFECTIVE_SOURCE_DIR "${LUMEN_ANDROID_FEXCORE_SOURCE_DIR}"
    CACHE INTERNAL "FEX source selected by the explicit PS5 build option" FORCE)
option(LUMEN_ANDROID_VULKAN_VALIDATION
    "Enable app-packaged synchronization validation in a private FEX diagnostic" OFF)
if(LUMEN_ANDROID_VULKAN_VALIDATION AND
   (NOT LUMEN_ANDROID_FEXCORE_LAB OR NOT CMAKE_BUILD_TYPE STREQUAL "Debug"))
    message(FATAL_ERROR "Vulkan validation diagnostics require the Debug FEX lab")
endif()

option(LUMEN_ANDROID_ARM_FASTBLOCK_SNAPSHOT_ELISION
    "Elide proven-safe FastBlock operation snapshots on Android ARM64" OFF)
if(LUMEN_ANDROID_ARM_FASTBLOCK_SNAPSHOT_ELISION AND
   NOT LUMEN_ANDROID_FULL_RUNTIME)
    message(FATAL_ERROR
        "Android FastBlock snapshot elision requires LUMEN_ANDROID_FULL_RUNTIME")
endif()

option(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY
    "Collect bounded FastBlock kind/pair/reject telemetry on Android ARM64" OFF)
if(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY AND
   NOT LUMEN_ANDROID_FULL_RUNTIME)
    message(FATAL_ERROR
        "Android FastBlock telemetry requires LUMEN_ANDROID_FULL_RUNTIME")
endif()
if(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY AND
   LUMEN_ANDROID_ARM_FASTBLOCK_SNAPSHOT_ELISION)
    message(FATAL_ERROR
        "Android FastBlock telemetry and snapshot elision are mutually exclusive")
endif()

set(LUMEN_ANDROID_DYNAREC_PROVIDER "disabled" CACHE STRING
    "Legacy Lumen dynarec provider; must remain disabled beside FEX")
set_property(CACHE LUMEN_ANDROID_DYNAREC_PROVIDER PROPERTY STRINGS
    disabled)
if(NOT LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "disabled")
    message(FATAL_ERROR
        "Android is FEX-only; the legacy Lumen dynarec provider must be disabled")
endif()

set(LUMEN_ANDROID_DYNAREC_POLICY "probe-only" CACHE STRING
    "Guest-native execution policy for the Android dynarec provider")
set_property(CACHE LUMEN_ANDROID_DYNAREC_POLICY PROPERTY STRINGS
    probe-only)
if(NOT LUMEN_ANDROID_DYNAREC_POLICY STREQUAL "probe-only")
    message(FATAL_ERROR
        "NODA is forbidden on Android; the legacy policy must remain probe-only")
endif()
if(LUMEN_ANDROID_ARM_FASTBLOCK_SNAPSHOT_ELISION AND
   (NOT LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "disabled" OR
    NOT LUMEN_ANDROID_DYNAREC_POLICY STREQUAL "probe-only"))
    message(FATAL_ERROR
        "Android FastBlock snapshot elision requires the disabled/probe-only provider")
endif()
if(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY AND
   (NOT LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "disabled" OR
    NOT LUMEN_ANDROID_DYNAREC_POLICY STREQUAL "probe-only"))
    message(FATAL_ERROR
        "Android FastBlock telemetry requires the disabled/probe-only provider")
endif()

get_filename_component(LUMEN_REPOSITORY_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(LUMEN_SOURCE_ROOT "${LUMEN_REPOSITORY_ROOT}/source")
set(LUMEN_CPU_SOURCE_ROOT "${LUMEN_REPOSITORY_ROOT}/source/emulator")
set(LUMEN_ANDROID_DYNAREC_SOURCE
    "${LUMEN_CPU_SOURCE_ROOT}/src/CpuA64DynarecDisabled.cpp")

if(LUMEN_ANDROID_FEXCORE_LAB)
    set(LUMEN_ANDROID_FEXCORE_PINNED_COMMIT
        "e17fdf9e90a49614e16ac8414a687477fb9e4f7c")
    if(LUMEN_PS5_FEX_C13 OR LUMEN_PS5_DIRECT_FEX)
        set(LUMEN_ANDROID_FEXCORE_PINNED_COMMIT "f2b679f6028ce1c38875233aecfcf5d3f8ebecec")
    endif()
    set(LUMEN_ANDROID_FEXCORE_LICENSE_SHA256
        "f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee")

    # AGP maps its Release variant to CMake RelWithDebInfo. Keep its symbols
    # for crash attribution; both accepted configurations are optimized.
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$" AND
       NOT (LUMEN_PS5_DIRECT_FEX AND CMAKE_BUILD_TYPE STREQUAL "Release"))
        message(FATAL_ERROR
            "Android FEX requires Debug or RelWithDebInfo; direct PS5 also supports native Release")
    endif()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        message(FATAL_ERROR
            "LUMEN_ANDROID_FEXCORE_LAB requires an AArch64 compiler target")
    endif()
    if(NOT LUMEN_ANDROID_FULL_RUNTIME)
        message(FATAL_ERROR
            "LUMEN_ANDROID_FEXCORE_LAB requires LUMEN_ANDROID_FULL_RUNTIME")
    endif()
    if(NOT LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "disabled" OR
       NOT LUMEN_ANDROID_DYNAREC_POLICY STREQUAL "probe-only")
        message(FATAL_ERROR
            "LUMEN_ANDROID_FEXCORE_LAB requires the disabled/probe-only Lumen provider")
    endif()
    if(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY OR
       LUMEN_ANDROID_ARM_FASTBLOCK_SNAPSHOT_ELISION)
        message(FATAL_ERROR
            "LUMEN_ANDROID_FEXCORE_LAB requires FastBlock telemetry and snapshot elision OFF")
    endif()
    if(LUMEN_ANDROID_FEXCORE_SOURCE_DIR STREQUAL "")
        message(FATAL_ERROR
            "LUMEN_ANDROID_FEXCORE_LAB requires LUMEN_ANDROID_FEXCORE_SOURCE_DIR")
    endif()
    if(LUMEN_ANDROID_FEXCORE_SOURCE_DIR STREQUAL LUMEN_BUNDLED_FEX_SOURCE_DIR)
        execute_process(
            COMMAND bash "${LUMEN_REPOSITORY_ROOT}/_Build/prepare_Bundled_FEXCore.sh" android
                "${LUMEN_BUNDLED_FEX_SOURCE_DIR}"
            RESULT_VARIABLE LUMEN_ANDROID_FEX_BUNDLE_RESULT
            OUTPUT_QUIET)
        if(NOT LUMEN_ANDROID_FEX_BUNDLE_RESULT EQUAL 0)
            message(FATAL_ERROR "Could not prepare the bundled FEX source for Android")
        endif()
    endif()
    if(NOT IS_DIRECTORY "${LUMEN_ANDROID_FEXCORE_SOURCE_DIR}")
        message(FATAL_ERROR
            "LUMEN_ANDROID_FEXCORE_SOURCE_DIR is not a directory")
    endif()

    file(REAL_PATH "${LUMEN_ANDROID_FEXCORE_SOURCE_DIR}"
        LUMEN_ANDROID_FEXCORE_SOURCE_REAL)
    file(RELATIVE_PATH LUMEN_ANDROID_FEXCORE_SOURCE_FROM_REPOSITORY
        "${LUMEN_REPOSITORY_ROOT}"
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}")
    if(NOT LUMEN_PS5_FEX_C13 AND NOT LUMEN_PS5_DIRECT_FEX AND
       NOT LUMEN_ANDROID_FEXCORE_SOURCE_REAL STREQUAL LUMEN_BUNDLED_FEX_SOURCE_DIR AND
       (LUMEN_ANDROID_FEXCORE_SOURCE_FROM_REPOSITORY STREQUAL "" OR
       (NOT IS_ABSOLUTE "${LUMEN_ANDROID_FEXCORE_SOURCE_FROM_REPOSITORY}" AND
        NOT LUMEN_ANDROID_FEXCORE_SOURCE_FROM_REPOSITORY MATCHES "^\\.\\./")))
        message(FATAL_ERROR
            "Use the bundled source/3rdparty/fex checkout or an external FEX checkout")
    endif()

    set(LUMEN_ANDROID_FEXCORE_X86_ENUMS_HEADER
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore/include/FEXCore/Core/X86Enums.h")
    set(LUMEN_ANDROID_FEXCORE_IR_HEADER
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore/include/FEXCore/IR/IR.h")
    set(LUMEN_ANDROID_FEXCORE_TYPES_HEADER
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/FEXCore/include/FEXCore/Utils/TypeDefines.h")
    set(LUMEN_ANDROID_FEXCORE_LICENSE_FILE
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/LICENSE")
    if(NOT EXISTS "${LUMEN_ANDROID_FEXCORE_X86_ENUMS_HEADER}" OR
       NOT EXISTS "${LUMEN_ANDROID_FEXCORE_IR_HEADER}" OR
       NOT EXISTS "${LUMEN_ANDROID_FEXCORE_TYPES_HEADER}" OR
       NOT EXISTS "${LUMEN_ANDROID_FEXCORE_LICENSE_FILE}")
        message(FATAL_ERROR
            "The external FEX checkout is missing the probed public headers or LICENSE")
    endif()

    find_package(Git REQUIRED)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C
            "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}" rev-parse --verify HEAD
        RESULT_VARIABLE LUMEN_ANDROID_FEXCORE_GIT_RESULT
        OUTPUT_VARIABLE LUMEN_ANDROID_FEXCORE_ACTUAL_COMMIT
        ERROR_VARIABLE LUMEN_ANDROID_FEXCORE_GIT_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT LUMEN_ANDROID_FEXCORE_GIT_RESULT EQUAL 0 OR
       NOT LUMEN_ANDROID_FEXCORE_ACTUAL_COMMIT STREQUAL
           LUMEN_ANDROID_FEXCORE_PINNED_COMMIT)
        message(FATAL_ERROR
            "The external FEX checkout must be at exact commit ${LUMEN_ANDROID_FEXCORE_PINNED_COMMIT}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C
            "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}" status --porcelain
            --untracked-files=all
        RESULT_VARIABLE LUMEN_ANDROID_FEXCORE_STATUS_RESULT
        OUTPUT_VARIABLE LUMEN_ANDROID_FEXCORE_STATUS
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT LUMEN_ANDROID_FEXCORE_STATUS_RESULT EQUAL 0 OR
       NOT LUMEN_ANDROID_FEXCORE_STATUS STREQUAL "")
        message(FATAL_ERROR
            "The external FEX checkout must be completely clean")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C
            "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}" remote get-url origin
        RESULT_VARIABLE LUMEN_ANDROID_FEXCORE_ORIGIN_RESULT
        OUTPUT_VARIABLE LUMEN_ANDROID_FEXCORE_ORIGIN
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT LUMEN_ANDROID_FEXCORE_ORIGIN_RESULT EQUAL 0 OR
       (NOT LUMEN_ANDROID_FEXCORE_ORIGIN STREQUAL
            "https://github.com/FEX-Emu/FEX.git" AND
        NOT LUMEN_ANDROID_FEXCORE_ORIGIN STREQUAL
            "https://github.com/FEX-Emu/FEX" AND
        NOT LUMEN_ANDROID_FEXCORE_ORIGIN STREQUAL
            "git@github.com:FEX-Emu/FEX.git"))
        message(FATAL_ERROR
            "The external checkout origin must be the official FEX-Emu/FEX repository")
    endif()

    file(SHA256 "${LUMEN_ANDROID_FEXCORE_LICENSE_FILE}"
        LUMEN_ANDROID_FEXCORE_ACTUAL_LICENSE_SHA256)
    if(NOT LUMEN_ANDROID_FEXCORE_ACTUAL_LICENSE_SHA256 STREQUAL
       LUMEN_ANDROID_FEXCORE_LICENSE_SHA256)
        message(FATAL_ERROR
            "The external FEX top-level MIT license does not match the pinned revision")
    endif()

    set(LUMEN_ANDROID_FEXCORE_REQUIRED_SUBMODULES
        "External/fmt|1be298e1bd68957e4cd352e1f676f00e07dcfb57|LICENSE|a65fcc5a1095fc33cc37e4fff69366e49d7e92f613558a2b95ed1b70e33758b4"
        "External/xxhash|e626a72bc2321cd320e953a0ccf1584cad60f363|LICENSE|6ffedbc0f7878612d2b23589f1ff2ab15633e1df7963a5d9fc750ec5500c7e7a"
        "External/unordered_dense|3234af2c03549bc85656bfd3a86993bf1cd8aef1|LICENSE|4f5a62734899ec0ba85d3e8264a7ed1fcb4bcb8f0b4359c0afd627b6314ae599"
        "External/rpmalloc|1d85c246cd827ead6865f4f880d4fef53f2b1864|LICENSE|9fdc653aacc736bef0cd12854f4a8d94f703ec5603fb5dca1be945b7f679918c"
    )
    if(LUMEN_PS5_FEX_C13 OR LUMEN_PS5_DIRECT_FEX)
        list(REMOVE_AT LUMEN_ANDROID_FEXCORE_REQUIRED_SUBMODULES 0)
        list(PREPEND LUMEN_ANDROID_FEXCORE_REQUIRED_SUBMODULES
            "External/fmt|407c905e45ad75fc29bf0f9bb7c5c2fd3475976f|LICENSE|07580f2a3b35709ce703d523f447b242f6dfec7582a8c0df102c7fa2849375f8")
    endif()
    foreach(LUMEN_ANDROID_FEXCORE_SUBMODULE IN LISTS
            LUMEN_ANDROID_FEXCORE_REQUIRED_SUBMODULES)
        string(REPLACE "|" ";" LUMEN_ANDROID_FEXCORE_SUBMODULE_FIELDS
            "${LUMEN_ANDROID_FEXCORE_SUBMODULE}")
        list(GET LUMEN_ANDROID_FEXCORE_SUBMODULE_FIELDS 0
            LUMEN_ANDROID_FEXCORE_SUBMODULE_PATH)
        list(GET LUMEN_ANDROID_FEXCORE_SUBMODULE_FIELDS 1
            LUMEN_ANDROID_FEXCORE_SUBMODULE_PIN)
        list(GET LUMEN_ANDROID_FEXCORE_SUBMODULE_FIELDS 2
            LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE)
        list(GET LUMEN_ANDROID_FEXCORE_SUBMODULE_FIELDS 3
            LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE_SHA256)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C
                "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/${LUMEN_ANDROID_FEXCORE_SUBMODULE_PATH}"
                rev-parse --verify HEAD
            RESULT_VARIABLE LUMEN_ANDROID_FEXCORE_SUBMODULE_RESULT
            OUTPUT_VARIABLE LUMEN_ANDROID_FEXCORE_SUBMODULE_ACTUAL
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT LUMEN_ANDROID_FEXCORE_SUBMODULE_RESULT EQUAL 0 OR
           NOT LUMEN_ANDROID_FEXCORE_SUBMODULE_ACTUAL STREQUAL
               LUMEN_ANDROID_FEXCORE_SUBMODULE_PIN)
            message(FATAL_ERROR
                "FEX dependency ${LUMEN_ANDROID_FEXCORE_SUBMODULE_PATH} must be initialized at ${LUMEN_ANDROID_FEXCORE_SUBMODULE_PIN}")
        endif()
        set(LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE_PATH
            "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/${LUMEN_ANDROID_FEXCORE_SUBMODULE_PATH}/${LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE}")
        if(NOT EXISTS "${LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE_PATH}")
            message(FATAL_ERROR
                "FEX dependency ${LUMEN_ANDROID_FEXCORE_SUBMODULE_PATH} is missing its license")
        endif()
        file(SHA256 "${LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE_PATH}"
            LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE_ACTUAL)
        if(NOT LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE_ACTUAL STREQUAL
           LUMEN_ANDROID_FEXCORE_SUBMODULE_LICENSE_SHA256)
            message(FATAL_ERROR
                "FEX dependency ${LUMEN_ANDROID_FEXCORE_SUBMODULE_PATH} license bytes changed")
        endif()
    endforeach()

    # The two remaining third-party libraries are stored directly in the
    # pinned FEX tree rather than as submodules. Keep their applicable license
    # evidence under the same fail-closed gate.
    set(LUMEN_ANDROID_FEXCORE_SOFTFLOAT_LICENSE_HEADER
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/External/SoftFloat-3e/include/SoftFloat-3e/softfloat.h")
    set(LUMEN_ANDROID_FEXCORE_CEPHES_LICENSE
        "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}/External/cephes/LICENSE")
    if(NOT EXISTS "${LUMEN_ANDROID_FEXCORE_SOFTFLOAT_LICENSE_HEADER}" OR
       NOT EXISTS "${LUMEN_ANDROID_FEXCORE_CEPHES_LICENSE}")
        message(FATAL_ERROR
            "The pinned FEX SoftFloat/cephes license evidence is missing")
    endif()
    file(SHA256 "${LUMEN_ANDROID_FEXCORE_SOFTFLOAT_LICENSE_HEADER}"
        LUMEN_ANDROID_FEXCORE_SOFTFLOAT_LICENSE_HEADER_SHA256)
    if(NOT LUMEN_ANDROID_FEXCORE_SOFTFLOAT_LICENSE_HEADER_SHA256 STREQUAL
       "907a309d402530a9647cfe8292228250539d4cafb6f2c1cbb9eaa6e7bd9d639c")
        message(FATAL_ERROR
            "The pinned FEX SoftFloat license-bearing header changed")
    endif()
    file(SHA256 "${LUMEN_ANDROID_FEXCORE_CEPHES_LICENSE}"
        LUMEN_ANDROID_FEXCORE_CEPHES_LICENSE_SHA256)
    if(NOT LUMEN_ANDROID_FEXCORE_CEPHES_LICENSE_SHA256 STREQUAL
       "edf1440b625e6d434a986cdb535c702001afab8959d94ce6275441a58cfd0ebb")
        message(FATAL_ERROR
            "The pinned FEX cephes license bytes changed")
    endif()

    # AGP packages this flavor-specific directory as APK assets. Validate the
    # repository copies here so a linked laboratory cannot build with missing
    # or stale legal resources.
    set(LUMEN_ANDROID_FEXCORE_LEGAL_DIR
        "${LUMEN_REPOSITORY_ROOT}/android/app/src/fexLab/assets/Legal")
    set(LUMEN_ANDROID_FEXCORE_LEGAL_RESOURCES
        "FEX/LICENSE|f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee"
        "fmt/LICENSE|a65fcc5a1095fc33cc37e4fff69366e49d7e92f613558a2b95ed1b70e33758b4"
        "xxhash/LICENSE|6ffedbc0f7878612d2b23589f1ff2ab15633e1df7963a5d9fc750ec5500c7e7a"
        "unordered_dense/LICENSE|4f5a62734899ec0ba85d3e8264a7ed1fcb4bcb8f0b4359c0afd627b6314ae599"
        "rpmalloc/LICENSE|2b886bfac9bf7b24d5eaca6437abccfd1d31068ed6d1c06abda74ebf1e55d51a"
        "SoftFloat-3e/LICENSE|ddcb981bd77da11fc970b30d319748a288767f8b7511bd0365696d3fc1e2085a"
        "cephes/LICENSE|edf1440b625e6d434a986cdb535c702001afab8959d94ce6275441a58cfd0ebb"
        "FEXCORE-NOTICE.md|ecb0ab603cd0548fe637565602bb2ee168c869c2f66c6b4b5e2bb6699f3dbee9"
    )
    foreach(LUMEN_ANDROID_FEXCORE_LEGAL_RESOURCE IN LISTS
            LUMEN_ANDROID_FEXCORE_LEGAL_RESOURCES)
        string(REPLACE "|" ";" LUMEN_ANDROID_FEXCORE_LEGAL_FIELDS
            "${LUMEN_ANDROID_FEXCORE_LEGAL_RESOURCE}")
        list(GET LUMEN_ANDROID_FEXCORE_LEGAL_FIELDS 0
            LUMEN_ANDROID_FEXCORE_LEGAL_RELATIVE_PATH)
        list(GET LUMEN_ANDROID_FEXCORE_LEGAL_FIELDS 1
            LUMEN_ANDROID_FEXCORE_LEGAL_EXPECTED_SHA256)
        set(LUMEN_ANDROID_FEXCORE_LEGAL_PATH
            "${LUMEN_ANDROID_FEXCORE_LEGAL_DIR}/${LUMEN_ANDROID_FEXCORE_LEGAL_RELATIVE_PATH}")
        if(NOT EXISTS "${LUMEN_ANDROID_FEXCORE_LEGAL_PATH}")
            message(FATAL_ERROR
                "Required FEX laboratory legal resource is missing: ${LUMEN_ANDROID_FEXCORE_LEGAL_RELATIVE_PATH}")
        endif()
        file(SHA256 "${LUMEN_ANDROID_FEXCORE_LEGAL_PATH}"
            LUMEN_ANDROID_FEXCORE_LEGAL_ACTUAL_SHA256)
        if(NOT LUMEN_ANDROID_FEXCORE_LEGAL_ACTUAL_SHA256 STREQUAL
           LUMEN_ANDROID_FEXCORE_LEGAL_EXPECTED_SHA256)
            message(FATAL_ERROR
                "FEX laboratory legal resource changed: ${LUMEN_ANDROID_FEXCORE_LEGAL_RELATIVE_PATH}")
        endif()
    endforeach()

    add_library(lumen_android_fexcore_header_api_probe OBJECT
        AndroidFexCoreApiProbe.cpp
    )
    set_target_properties(lumen_android_fexcore_header_api_probe PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    target_compile_definitions(lumen_android_fexcore_header_api_probe PRIVATE
        "$<$<OR:$<BOOL:${LUMEN_PS5_FEX_C13}>,$<BOOL:${LUMEN_PS5_DIRECT_FEX}>>:LUMEN_PS5_FEX_C13=1>")
    target_compile_features(lumen_android_fexcore_header_api_probe PRIVATE
        cxx_std_20
    )
    target_compile_options(lumen_android_fexcore_header_api_probe PRIVATE
        -fno-exceptions
        -fno-rtti
        -fno-strict-aliasing
        -Wall
        -Wextra
        -Werror
    )

    add_library(lumen_android_fexcore_lab_admission OBJECT
        AndroidFexCoreLabAdmission.cpp
    )
    set_target_properties(lumen_android_fexcore_lab_admission PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(lumen_android_fexcore_lab_admission PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}"
    )
    target_compile_features(lumen_android_fexcore_lab_admission PRIVATE
        cxx_std_17
    )
    target_compile_definitions(lumen_android_fexcore_lab_admission PRIVATE
        LUMEN_ANDROID_FEXCORE_LAB_BUILD=1
        LUMEN_ANDROID_FULL_RUNTIME_BUILD=1
        $<IF:$<CONFIG:Debug>,LUMEN_ANDROID_FEXCORE_BUILD_DEBUG=1,LUMEN_ANDROID_FEXCORE_BUILD_RELEASE=1>
    )
    target_compile_options(lumen_android_fexcore_lab_admission PRIVATE
        -fno-exceptions
        -fno-rtti
        -Wall
        -Wextra
        -Werror
    )

    if(LUMEN_PS5_DIRECT_FEX)
        set(LUMEN_ANDROID_FEXCORE_BUILD_SOURCE_REAL "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}")
        add_subdirectory("${LUMEN_SOURCE_ROOT}/gen4/gen5/direct_fex"
            "${CMAKE_CURRENT_BINARY_DIR}/fexcore-direct")
    else()
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/fexcore"
        "${CMAKE_CURRENT_BINARY_DIR}/fexcore"
    )
    endif()
    target_include_directories(lumen_android_fexcore_header_api_probe PRIVATE
        "${LUMEN_ANDROID_FEXCORE_BUILD_SOURCE_REAL}/FEXCore/include"
    )

    message(STATUS
        "LUMEN_ANDROID_FEXCORE_LAB_V3 pin=${LUMEN_ANDROID_FEXCORE_PINNED_COMMIT} backend=FEXCore linked=1 selfTest=device-gated guestExecution=RunGuestX64")
endif()

if(LUMEN_ANDROID_FULL_RUNTIME)
    set(LUMEN_WITH_DESKTOP_FRONTEND OFF CACHE BOOL "" FORCE)
    set(LUMEN_ENABLE_MEMORY_TRACKER OFF CACHE BOOL "" FORCE)
    set(LUMEN_ENABLE_EASY_PROFILER OFF CACHE BOOL "" FORCE)
    set(LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT OFF CACHE BOOL "" FORCE)
    set(LUMEN_FAST_BLOCK_TELEMETRY
        ${LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY} CACHE BOOL "" FORCE)
    if(LUMEN_ANDROID_DYNAREC_POLICY STREQUAL "selective-noda")
        set(LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK OFF CACHE BOOL "" FORCE)
        set(LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK OFF CACHE BOOL "" FORCE)
    endif()

    # The Debug APK keeps a useful native profile without an unoptimized
    # dependency graph. AGP Release uses optimized RelWithDebInfo flags;
    # FEX itself retains its pinned -O2 profile in the FEX subtree.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O2)
    endif()

    add_subdirectory(
        "${LUMEN_SOURCE_ROOT}"
        "${CMAKE_CURRENT_BINARY_DIR}/lumen-runtime"
        EXCLUDE_FROM_ALL
    )
    if(LUMEN_ANDROID_ARM_FASTBLOCK_SNAPSHOT_ELISION)
        target_compile_definitions(emulator PRIVATE
            LUMEN_ANDROID_ARM_FASTBLOCK_SNAPSHOT_ELISION=1
        )
    endif()
    if(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY)
        target_compile_definitions(emulator PRIVATE
            LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY=1
        )
    endif()
    if(LUMEN_ANDROID_FEXCORE_LAB)
        target_compile_definitions(emulator PRIVATE
            LUMEN_ANDROID_FEXCORE_RUNTIME=1
            LUMEN_V_MAC_LEGACY_F32_ANDROID_FEX_BUILD=1
            LUMEN_V_RSQ_CLAMP_F32_ANDROID_FEX_BUILD=1
        )
        target_include_directories(emulator PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}"
        )
        target_link_libraries(emulator
            lumen_android_fexcore_runtime
        )
    endif()
    if(LUMEN_ANDROID_VULKAN_VALIDATION)
        target_compile_definitions(emulator PRIVATE LUMEN_ANDROID_VULKAN_VALIDATION=1)
    endif()
endif()

add_library(lumen_android_spike SHARED
    AndroidBootstrap.cpp
    AndroidGamePreflight.cpp
    JniBridge.cpp
)

if(LUMEN_ANDROID_FEXCORE_LAB)
    target_sources(lumen_android_spike PRIVATE
        $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>
    )
    add_dependencies(lumen_android_spike
        lumen_android_fexcore_header_api_probe
    )
    target_compile_definitions(lumen_android_spike PRIVATE
        LUMEN_ANDROID_FEXCORE_LAB_BUILD=1
    )
endif()

if(LUMEN_ANDROID_FULL_RUNTIME)
    target_sources(lumen_android_spike PRIVATE
        AndroidDynarecProbe.cpp
        AndroidRuntimeMain.cpp
        AndroidRuntimeUi.cpp
    )
    target_compile_definitions(lumen_android_spike PRIVATE
        LUMEN_ANDROID_FULL_RUNTIME=1
    )
    if(LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY)
        target_compile_definitions(lumen_android_spike PRIVATE
            LUMEN_ANDROID_ARM_FASTBLOCK_TELEMETRY=1
        )
    endif()
    target_include_directories(lumen_android_spike PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/lumen-runtime"
    )
else()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(LUMEN_ANDROID_CORE_BUILD LUMEN_BUILD_DEBUG)
    else()
        set(LUMEN_ANDROID_CORE_BUILD LUMEN_BUILD_RELEASE)
    endif()
    set(LUMEN_ANDROID_GENERATED_INCLUDE_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${LUMEN_ANDROID_GENERATED_INCLUDE_DIR}")
    configure_file(
        AndroidCoreConfig.h.in
        "${LUMEN_ANDROID_GENERATED_INCLUDE_DIR}/cmake_config.h"
        @ONLY
    )

    target_sources(lumen_android_spike PRIVATE
        "${LUMEN_ANDROID_DYNAREC_SOURCE}"
        "${LUMEN_CPU_SOURCE_ROOT}/src/CpuX64.cpp"
    )

    # Compile the runtime linker's complete Android/API 26 branch without
    # linking it into the fast bootstrap APK.
    add_library(lumen_android_runtime_linker_compile_probe OBJECT
        "${LUMEN_CPU_SOURCE_ROOT}/src/Loader/RuntimeLinker.cpp"
    )
    set_target_properties(lumen_android_runtime_linker_compile_probe PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(lumen_android_runtime_linker_compile_probe PRIVATE
        "${LUMEN_ANDROID_GENERATED_INCLUDE_DIR}"
        "${LUMEN_SOURCE_ROOT}/include"
        "${LUMEN_CPU_SOURCE_ROOT}/include"
        "${LUMEN_CPU_SOURCE_ROOT}/src"
        "${LUMEN_SOURCE_ROOT}/3rdparty/magic_enum/include"
        "${LUMEN_SOURCE_ROOT}/3rdparty/sdl2/sdl2/include"
        "${LUMEN_SOURCE_ROOT}/3rdparty/easy_profiler/include"
    )
    target_compile_features(lumen_android_runtime_linker_compile_probe PRIVATE
        cxx_std_17
    )
    target_compile_options(lumen_android_runtime_linker_compile_probe PRIVATE
        -fno-exceptions
        -fno-rtti
        -fno-strict-aliasing
        -fno-omit-frame-pointer
        -ffunction-sections
        -fdata-sections
        -Wall
        -Wextra
        -Werror
    )
    add_dependencies(lumen_android_spike
        lumen_android_runtime_linker_compile_probe
    )
endif()

target_include_directories(lumen_android_spike PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${LUMEN_SOURCE_ROOT}/include"
    "${LUMEN_CPU_SOURCE_ROOT}/include"
    "${LUMEN_CPU_SOURCE_ROOT}/src"
)

target_compile_features(lumen_android_spike PRIVATE cxx_std_17)
target_compile_options(lumen_android_spike PRIVATE
    -fno-exceptions
    -fno-rtti
    -fno-strict-aliasing
    -fno-omit-frame-pointer
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -Werror
)
if(LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "aarch64-wx")
    target_compile_definitions(lumen_android_spike PRIVATE
        LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX=1
    )
else()
    target_compile_definitions(lumen_android_spike PRIVATE
        LUMEN_ANDROID_DYNAREC_PROVIDER_DISABLED=1
    )
endif()

if(LUMEN_ANDROID_DYNAREC_POLICY STREQUAL "selective-noda")
    target_compile_definitions(lumen_android_spike PRIVATE
        LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA=1
    )
else()
    target_compile_definitions(lumen_android_spike PRIVATE
        LUMEN_ANDROID_DYNAREC_POLICY_PROBE_ONLY=1
    )
endif()

target_compile_definitions(lumen_android_spike PRIVATE
    LUMEN_ENABLE_X64_FAST_BLOCKS=0
    LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK=0
    LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK=0
)

target_link_options(lumen_android_spike PRIVATE
    "-Wl,--gc-sections"
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384"
)

find_library(LUMEN_ANDROID_LOG_LIBRARY log REQUIRED)
find_library(LUMEN_ANDROID_MATH_LIBRARY m REQUIRED)
find_library(LUMEN_ANDROID_MEDIA_NDK_LIBRARY mediandk REQUIRED)
if(LUMEN_ANDROID_FEXCORE_LAB)
    find_library(LUMEN_ANDROID_PLATFORM_LIBRARY android REQUIRED)
endif()
target_link_libraries(lumen_android_spike PRIVATE
    "${LUMEN_ANDROID_LOG_LIBRARY}"
    "${LUMEN_ANDROID_MATH_LIBRARY}"
)
if(LUMEN_ANDROID_FEXCORE_LAB)
    target_link_libraries(lumen_android_spike PRIVATE
        lumen_android_fexcore_runtime
        "${LUMEN_ANDROID_PLATFORM_LIBRARY}"
    )
    # Lumen integration, 2026-10-02: the PS5 embedding shares this exact FEX
    # provider; its complete-game admission remains separate and fail-closed.
    include("${CMAKE_CURRENT_LIST_DIR}/Ps5AdrenoTools.cmake")
    lumen_ps4_attach_vulkan_driver(lumen_android_spike)
    add_subdirectory("${LUMEN_SOURCE_ROOT}/gen4/gen5"
        "${CMAKE_CURRENT_BINARY_DIR}/lumen-ps5")
    target_link_libraries(lumen_android_spike PRIVATE lumen_ps5_guest_execution)
    # Gen5 has its own process/SDL3 DSO. Its JNI frontend never links the PS4
    # engine or its SDL2 instance into that process.
    target_sources(lumen_android_ps5 PRIVATE AndroidPs5Main.cpp AndroidPs5RuntimeUi.cpp)
    target_include_directories(lumen_android_ps5 PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    lumen_ps5_attach_vulkan_driver(lumen_android_ps5)
endif()
if(LUMEN_ANDROID_FULL_RUNTIME)
    target_link_options(lumen_android_spike PRIVATE
        "-Wl,-u,JNI_OnLoad"
    )
    target_link_libraries(lumen_android_spike PRIVATE
        emulator
        "${LUMEN_ANDROID_MEDIA_NDK_LIBRARY}"
        "${CMAKE_DL_LIBS}"
    )
endif()

# Native page-size gates are explicit developer targets, excluded from the APK.
# Run the same binaries on both 4 KiB and 16 KiB Android kernels.
if(LUMEN_ANDROID_FEXCORE_LAB)
    add_executable(lumen_android_fex_host_call_return_test EXCLUDE_FROM_ALL
        "${LUMEN_SOURCE_ROOT}/FexHostCallReturnTest.cpp"
        $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>)
    target_include_directories(lumen_android_fex_host_call_return_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}" "${LUMEN_SOURCE_ROOT}/cpu/runtime")
    target_compile_features(lumen_android_fex_host_call_return_test PRIVATE cxx_std_20)
    target_compile_options(lumen_android_fex_host_call_return_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(lumen_android_fex_host_call_return_test PRIVATE
        lumen_android_fexcore_runtime "${LUMEN_ANDROID_LOG_LIBRARY}"
        "${LUMEN_ANDROID_PLATFORM_LIBRARY}" "${CMAKE_DL_LIBS}")

    add_executable(lumen_android_fex_translation_cache_test EXCLUDE_FROM_ALL
        "${LUMEN_REPOSITORY_ROOT}/macos/fexcore-runtime/FexTranslationCacheTest.cpp"
        $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>)
    target_include_directories(lumen_android_fex_translation_cache_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_features(lumen_android_fex_translation_cache_test PRIVATE cxx_std_20)
    target_compile_options(lumen_android_fex_translation_cache_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(lumen_android_fex_translation_cache_test PRIVATE
        lumen_android_fexcore_runtime "${LUMEN_ANDROID_LOG_LIBRARY}"
        "${LUMEN_ANDROID_PLATFORM_LIBRARY}" "${CMAKE_DL_LIBS}")

    add_executable(lumen_android_fexcore_fp_boundary_probe EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/../../test/cpp/AndroidFexCoreFpBoundaryProbe.cpp"
        $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>)
    target_include_directories(lumen_android_fexcore_fp_boundary_probe PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(lumen_android_fexcore_fp_boundary_probe PRIVATE
        LUMEN_FEX_FP_PROBE_DIRECT_LINK=1)
    target_compile_features(lumen_android_fexcore_fp_boundary_probe PRIVATE cxx_std_20)
    target_compile_options(lumen_android_fexcore_fp_boundary_probe PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(lumen_android_fexcore_fp_boundary_probe PRIVATE
        lumen_android_fexcore_runtime "${LUMEN_ANDROID_LOG_LIBRARY}"
        "${LUMEN_ANDROID_PLATFORM_LIBRARY}" "${CMAKE_DL_LIBS}")

    add_executable(lumen_android_fexcore_numerical_probe EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/../../test/cpp/AndroidFexCoreNumericalProbe.cpp"
        $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>)
    target_include_directories(lumen_android_fexcore_numerical_probe PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_definitions(lumen_android_fexcore_numerical_probe PRIVATE
        LUMEN_FEX_NUMERICAL_PROBE_DIRECT_LINK=1)
    target_compile_features(lumen_android_fexcore_numerical_probe PRIVATE cxx_std_20)
    target_compile_options(lumen_android_fexcore_numerical_probe PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(lumen_android_fexcore_numerical_probe PRIVATE
        lumen_android_fexcore_runtime "${LUMEN_ANDROID_LOG_LIBRARY}"
        "${LUMEN_ANDROID_PLATFORM_LIBRARY}" "${CMAKE_DL_LIBS}")

    add_executable(lumen_android_fexcore_page_size_test EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/../../test/cpp/AndroidFexCorePageSizeTest.cpp"
        $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>
    )
    target_include_directories(lumen_android_fexcore_page_size_test PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}")
    target_compile_features(lumen_android_fexcore_page_size_test PRIVATE cxx_std_20)
    target_compile_options(lumen_android_fexcore_page_size_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(lumen_android_fexcore_page_size_test PRIVATE
        lumen_android_fexcore_runtime "${LUMEN_ANDROID_LOG_LIBRARY}"
        "${LUMEN_ANDROID_PLATFORM_LIBRARY}")

    add_executable(lumen_android_virtual_memory_test EXCLUDE_FROM_ALL
        "${LUMEN_SOURCE_ROOT}/AndroidVirtualMemoryTest.cpp")
    target_include_directories(lumen_android_virtual_memory_test PRIVATE
        "${LUMEN_SOURCE_ROOT}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/lumen-runtime")
    target_compile_features(lumen_android_virtual_memory_test PRIVATE cxx_std_17)
    target_compile_options(lumen_android_virtual_memory_test PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(lumen_android_virtual_memory_test PRIVATE core)

    add_executable(lumen_android_proc_maps_reader_test EXCLUDE_FROM_ALL
        "${LUMEN_SOURCE_ROOT}/ProcMapsReaderTest.cpp")
    target_include_directories(lumen_android_proc_maps_reader_test PRIVATE
        "${LUMEN_SOURCE_ROOT}/include")
    target_compile_features(lumen_android_proc_maps_reader_test PRIVATE cxx_std_17)
    target_compile_options(lumen_android_proc_maps_reader_test PRIVATE -Wall -Wextra -Werror)

    find_package(Threads REQUIRED)
    add_executable(lumen_android_fexcore_guest_tsc_probe EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/../../test/cpp/FexGuestTscProbe.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../test/cpp/AndroidPthreadMutexTestStubs.cpp"
        $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>)
    target_include_directories(lumen_android_fexcore_guest_tsc_probe PRIVATE
        "${LUMEN_SOURCE_ROOT}/include"
        "${LUMEN_CPU_SOURCE_ROOT}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/lumen-runtime")
    target_compile_definitions(lumen_android_fexcore_guest_tsc_probe PRIVATE
        LUMEN_FEX_TSC_PROBE_DIRECT_LINK=1)
    target_compile_features(lumen_android_fexcore_guest_tsc_probe PRIVATE cxx_std_20)
    target_compile_options(lumen_android_fexcore_guest_tsc_probe PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(lumen_android_fexcore_guest_tsc_probe PRIVATE
        lumen_android_fexcore_runtime emulator Threads::Threads
        "${LUMEN_ANDROID_LOG_LIBRARY}"
        "${LUMEN_ANDROID_MEDIA_NDK_LIBRARY}" "${CMAKE_DL_LIBS}")
    add_executable(lumen_android_pthread_mutex_test EXCLUDE_FROM_ALL
        "${LUMEN_SOURCE_ROOT}/PthreadMutexTest.cpp"
        "${LUMEN_SOURCE_ROOT}/emulator/src/Kernel/Pthread.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../test/cpp/AndroidPthreadMutexTestStubs.cpp"
        $<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>)
    target_include_directories(lumen_android_pthread_mutex_test PRIVATE
        "${LUMEN_SOURCE_ROOT}/include"
        "${LUMEN_CPU_SOURCE_ROOT}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/lumen-runtime")
    target_compile_features(lumen_android_pthread_mutex_test PRIVATE cxx_std_17)
    # Compile the instrumented pthread unit only into this explicit gate. Its
    # definitions satisfy the archive references without extracting emulator's
    # ordinary pthread unit; the APK keeps the production unit without hooks.
    target_compile_definitions(lumen_android_pthread_mutex_test PRIVATE
        LUMEN_PTHREAD_EXIT_TEST_HOOK
        "$<TARGET_PROPERTY:emulator,COMPILE_DEFINITIONS>")
    target_compile_options(lumen_android_pthread_mutex_test PRIVATE
        -Wall -Wextra -Werror -fno-rtti)
    # Match the APK's archive order: FEX supplies an ABI-compatible xxHash
    # superset before emulator's zstd closure, so ld need not extract zstd's
    # older standalone xxhash object and no duplicate-definition waiver is needed.
    target_link_libraries(lumen_android_pthread_mutex_test PRIVATE
        lumen_android_fexcore_runtime emulator Threads::Threads
        "${LUMEN_ANDROID_LOG_LIBRARY}"
        "${LUMEN_ANDROID_MEDIA_NDK_LIBRARY}" "${CMAKE_DL_LIBS}")
    foreach(page_test IN ITEMS
            lumen_android_fexcore_fp_boundary_probe
            lumen_android_fexcore_numerical_probe
            lumen_android_fexcore_guest_tsc_probe
            lumen_android_fexcore_page_size_test
            lumen_android_virtual_memory_test
            lumen_android_proc_maps_reader_test
            lumen_android_pthread_mutex_test)
        target_link_options(${page_test} PRIVATE
            "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
    endforeach()
endif()

set(LUMEN_GEN4_DEPS_DIR "${LUMEN_REPOSITORY_ROOT}/../lumen-deps/gen4/sources" CACHE PATH
    "Pinned PS4 dependencies")
set(LUMEN_GEN4_ANDROID_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen4-stage" CACHE PATH
    "Variant-specific Android PS4 library staging directory")
set(LUMEN_GEN4_BUILD_JOBS 4 CACHE STRING "Parallel jobs for the independent PS4 build")
set(LUMEN_GEN4_FULL_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen4" CACHE PATH
    "Independent PS4 build directory for this Android variant")
set(LUMEN_GEN4_BUILD_TYPE "" CACHE STRING
    "Optional independent PS4 build configuration; empty inherits this Android variant")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_target(lumen_android_gen4_build
    COMMAND "${Python3_EXECUTABLE}" "${LUMEN_SOURCE_ROOT}/gen4/tools/build_runtime.py"
        --parent "${CMAKE_CURRENT_BINARY_DIR}" --build "${LUMEN_GEN4_FULL_BUILD_DIR}"
        --deps "${LUMEN_GEN4_DEPS_DIR}" --ffmpeg "${FFMPEG_PREBUILT_DIR}"
        --stage "${LUMEN_GEN4_ANDROID_STAGE_DIR}" --jobs "${LUMEN_GEN4_BUILD_JOBS}"
        --configuration "${LUMEN_GEN4_BUILD_TYPE}"
    BYPRODUCTS "${LUMEN_GEN4_ANDROID_STAGE_DIR}/arm64-v8a/liblumen_android_gen4.so"
    DEPENDS lumen_android_fexcore_runtime lumen_android_fexcore_lab_admission
    USES_TERMINAL VERBATIM)
