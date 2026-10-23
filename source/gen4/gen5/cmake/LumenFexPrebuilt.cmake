# SPDX-License-Identifier: GPL-2.0-only
# Lumen, 2026-10-02: import the reviewed native FEX closure from its parent graph.
# build-full-kyty.sh ensures that graph is current before configuring the child.
# Include before Kyty's third-party subdirectory so fmt and xxhash have one owner.
include_guard(GLOBAL)

if(NOT APPLE OR CMAKE_CROSSCOMPILING OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
  message(FATAL_ERROR "The prebuilt PS5 FEX embedding requires native macOS arm64")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "12.0")
  message(FATAL_ERROR "The audited FEX embedding requires macOS deployment target 12.0")
endif()
if(CMAKE_CONFIGURATION_TYPES)
  message(FATAL_ERROR "The prebuilt FEX embedding requires a single-config build")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  message(FATAL_ERROR "The prebuilt PS5 FEX embedding currently requires matching Release builds")
endif()
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$" OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
  message(FATAL_ERROR "The audited FEX embedding requires Apple Clang/Clang")
endif()
if(TARGET fmt::fmt OR TARGET xxhash)
  message(FATAL_ERROR "Import the audited FEX dependencies before creating fmt/xxhash")
endif()

set(LUMEN_PS5_FEX_PREBUILT_ROOT "" CACHE PATH "Parent Lumen build containing the audited FEX runtime")
if(NOT EXISTS "${LUMEN_PS5_FEX_PREBUILT_ROOT}/CMakeCache.txt")
  message(FATAL_ERROR "Provide LUMEN_PS5_FEX_PREBUILT_ROOT with completed parent FEX archives")
endif()
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../.." LUMEN_PS5_EMBEDDING_ROOT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${LUMEN_PS5_EMBEDDING_ROOT}/source/gen4/gen5/tools/VerifyFexPrebuilt.py"
  "${LUMEN_PS5_EMBEDDING_ROOT}/macos/fexcore-runtime/CMakeLists.txt"
  "${LUMEN_PS5_EMBEDDING_ROOT}/macos/fexcore-runtime/prepare_source_overlay.py"
  "${LUMEN_PS5_FEX_PREBUILT_ROOT}/CMakeCache.txt")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${LUMEN_PS5_EMBEDDING_ROOT}/source/gen4/gen5/tools/VerifyFexPrebuilt.py"
    --parent "${LUMEN_PS5_FEX_PREBUILT_ROOT}" --repository "${LUMEN_PS5_EMBEDDING_ROOT}"
    --output "${CMAKE_BINARY_DIR}/lumen-fex-prebuilt.json"
    --cmake-output "${CMAKE_BINARY_DIR}/lumen-fex-prebuilt-paths.cmake"
  RESULT_VARIABLE LUMEN_PS5_VERIFY_RESULT
  OUTPUT_VARIABLE LUMEN_PS5_VERIFY_OUTPUT
  ERROR_VARIABLE LUMEN_PS5_VERIFY_ERROR)
if(NOT LUMEN_PS5_VERIFY_RESULT EQUAL 0)
  message(FATAL_ERROR "FEX prebuilt verification failed: ${LUMEN_PS5_VERIFY_OUTPUT}${LUMEN_PS5_VERIFY_ERROR}")
endif()
include("${CMAKE_BINARY_DIR}/lumen-fex-prebuilt-paths.cmake")

function(lumen_ps5_import_archive target path)
  add_library(${target} STATIC IMPORTED GLOBAL)
  set_target_properties(${target} PROPERTIES IMPORTED_LOCATION "${path}")
endfunction()
lumen_ps5_import_archive(fmt "${LUMEN_PS5_IMPORTED_fmt}")
add_library(fmt::fmt ALIAS fmt)
set_target_properties(fmt PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${LUMEN_PS5_IMPORTED_FMT_INCLUDE}"
  IMPORTED_NO_SYSTEM TRUE SYSTEM FALSE)
lumen_ps5_import_archive(xxhash "${LUMEN_PS5_IMPORTED_xxhash}")
set_target_properties(xxhash PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${LUMEN_PS5_IMPORTED_XXHASH_INCLUDE}"
  IMPORTED_NO_SYSTEM TRUE SYSTEM FALSE)
foreach(part runtime core base cephes softfloat jemalloc rpmalloc)
  lumen_ps5_import_archive(lumen_ps5_fex_${part} "${LUMEN_PS5_IMPORTED_${part}}")
endforeach()

add_library(lumen_ps5_fex_embedding INTERFACE)
target_include_directories(lumen_ps5_fex_embedding INTERFACE
  "${LUMEN_PS5_EMBEDDING_ROOT}/android/app/src/main/cpp"
  "${LUMEN_PS5_EMBEDDING_ROOT}/source/cpu/runtime")
target_compile_definitions(lumen_ps5_fex_embedding INTERFACE LUMEN_PS5_FEXCORE=1)
target_link_libraries(lumen_ps5_fex_embedding INTERFACE
  lumen_ps5_fex_runtime lumen_ps5_fex_core lumen_ps5_fex_base
  fmt::fmt xxhash lumen_ps5_fex_cephes lumen_ps5_fex_softfloat
  ${CMAKE_DL_LIBS} lumen_ps5_fex_jemalloc lumen_ps5_fex_rpmalloc)
# Keep the real embedding entry point in the full executable even while the
# production loader correctly denies admission for missing runtime contracts.
target_link_options(lumen_ps5_fex_embedding INTERFACE
  "LINKER:-u,_LumenAndroidFexCoreRunGuestV3")

# The parent archive contains the common facade and the optional TCG provider.
# Reuse its exact native library rather than initializing a second runtime.
file(STRINGS "${LUMEN_PS5_FEX_PREBUILT_ROOT}/CMakeCache.txt" qemu_cache_entry
  REGEX "^LUMEN_QEMU_TCG_LIBRARY:FILEPATH=")
if(qemu_cache_entry)
  string(REGEX REPLACE "^LUMEN_QEMU_TCG_LIBRARY:FILEPATH=" "" qemu_library "${qemu_cache_entry}")
  if(NOT qemu_library STREQUAL "")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
      "${LUMEN_PS5_EMBEDDING_ROOT}/source/cpu/qemu/tools/VerifyLibrary.py"
      --library "${qemu_library}" --host macos
      --deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
      RESULT_VARIABLE qemu_verified OUTPUT_VARIABLE qemu_receipt ERROR_VARIABLE qemu_error)
    if(NOT qemu_verified EQUAL 0)
      message(FATAL_ERROR "Parent QEMU TCG verification failed: ${qemu_receipt}${qemu_error}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${qemu_library}" "${qemu_library}.json"
      "${LUMEN_PS5_EMBEDDING_ROOT}/source/cpu/qemu/tools/VerifyLibrary.py"
      "${LUMEN_PS5_EMBEDDING_ROOT}/source/cpu/qemu/LumenQemuTcg.c"
      "${LUMEN_PS5_EMBEDDING_ROOT}/source/cpu/qemu/LumenQemuTcg.h"
      "${LUMEN_PS5_EMBEDDING_ROOT}/source/cpu/qemu/source-lock.json")
    add_library(lumen_ps5_qemu_tcg_core SHARED IMPORTED GLOBAL)
    set_target_properties(lumen_ps5_qemu_tcg_core PROPERTIES IMPORTED_LOCATION "${qemu_library}")
    target_link_libraries(lumen_ps5_fex_embedding INTERFACE lumen_ps5_qemu_tcg_core)
    target_include_directories(lumen_ps5_fex_embedding INTERFACE
      "${LUMEN_PS5_EMBEDDING_ROOT}/source/cpu/qemu")
    target_compile_definitions(lumen_ps5_fex_embedding INTERFACE LUMEN_WITH_QEMU_TCG=1)
  endif()
endif()

# Some inherited include directories name another fmt/xxhash checkout. Use the
# exact headers corresponding to the imported archives throughout this graph.
include_directories(BEFORE "${LUMEN_PS5_IMPORTED_FMT_INCLUDE}" "${LUMEN_PS5_IMPORTED_XXHASH_INCLUDE}")
message(STATUS "Reused audited native FEX closure from parent: ${LUMEN_PS5_FEX_PREBUILT_ROOT}")
