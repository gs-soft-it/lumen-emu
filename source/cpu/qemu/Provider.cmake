# SPDX-License-Identifier: GPL-2.0-only
include_guard(GLOBAL)
set(LUMEN_QEMU_TCG_LIBRARY "" CACHE FILEPATH
  "Pinned native QEMU TCG C-boundary library; empty builds FEX only")

function(lumen_add_qemu_provider target repository)
  if(LUMEN_QEMU_TCG_LIBRARY STREQUAL "")
    return()
  endif()
  if(NOT EXISTS "${LUMEN_QEMU_TCG_LIBRARY}" OR
     NOT EXISTS "${LUMEN_QEMU_TCG_LIBRARY}.json")
    message(FATAL_ERROR "QEMU TCG requires a built library and its source receipt")
  endif()
  find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
  set(qemu_verification_arguments)
  if(ANDROID)
    set(qemu_host android)
  elseif(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(qemu_host macos)
    if(CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
      message(FATAL_ERROR "QEMU TCG macOS verification requires an explicit deployment target")
    endif()
    set(qemu_verification_arguments --deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  else()
    message(FATAL_ERROR "QEMU TCG provider currently supports macOS/Android ARM64")
  endif()
  execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${repository}/source/cpu/qemu/tools/VerifyLibrary.py"
    --library "${LUMEN_QEMU_TCG_LIBRARY}" --host "${qemu_host}" ${qemu_verification_arguments}
    RESULT_VARIABLE verified OUTPUT_VARIABLE receipt ERROR_VARIABLE error)
  if(NOT verified EQUAL 0)
    message(FATAL_ERROR "QEMU TCG library verification failed: ${receipt}${error}")
  endif()
  if(NOT TARGET lumen_qemu_tcg_core)
    add_library(lumen_qemu_tcg_core SHARED IMPORTED GLOBAL)
    set_target_properties(lumen_qemu_tcg_core PROPERTIES
      IMPORTED_LOCATION "${LUMEN_QEMU_TCG_LIBRARY}")
  endif()
  target_sources(${target} PRIVATE "${repository}/source/cpu/qemu/QemuProvider.cpp")
  target_include_directories(${target} PUBLIC "${repository}/source/cpu/qemu")
  target_compile_definitions(${target} PUBLIC LUMEN_WITH_QEMU_TCG=1)
  target_link_libraries(${target} PUBLIC lumen_qemu_tcg_core)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${LUMEN_QEMU_TCG_LIBRARY}.json"
    "${LUMEN_QEMU_TCG_LIBRARY}"
    "${repository}/source/cpu/qemu/tools/VerifyLibrary.py"
    "${repository}/source/cpu/qemu/tools/BuildLibrary.py"
    "${repository}/source/cpu/qemu/source-lock.json"
    "${repository}/source/cpu/qemu/dependency-lock.json"
    "${repository}/source/cpu/qemu/patches/qemu-persistent-ir-cache.patch"
    "${repository}/source/cpu/qemu/LumenQemuTcgCache.c"
    "${repository}/source/cpu/qemu/LumenQemuTcgCache.h"
    "${repository}/source/cpu/qemu/LumenQemuTcg.h"
    "${repository}/source/cpu/qemu/LumenQemuTcg.c")
endfunction()
