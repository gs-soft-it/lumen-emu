# SPDX-License-Identifier: GPL-2.0-only
# The C13 pin keeps the PS5 exclusive-publication facade and its required
# backedge, decoded-block, JIT-retry and independent-denormal corrections.
# Its July core has no persistent disk cache and supports 4 KiB hosts.
# Interval instrumentation is deliberately unavailable and denied by the facade.
set(LUMEN_PS5_FEX_C13_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fex-f2b679f6-ps5-exclusive.patch")
set(LUMEN_PS5_FEX_C13_PATCH_SHA256
    "e74ffc5c5c2222d5da034af843d4e190465817a8e9a4a15e447f9a76c45b4ef9" CACHE INTERNAL "C13 PS5 exclusive source overlay" FORCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/prepare_source_overlay.py"
    "${LUMEN_PS5_FEX_C13_PATCH}")
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/prepare_source_overlay.py"
    --output-dir "${LUMEN_REPOSITORY_ROOT}/_Build/fex-sources"
    "${LUMEN_ANDROID_FEXCORE_SOURCE_REAL}" "${LUMEN_ANDROID_FEXCORE_PINNED_COMMIT}"
    "${LUMEN_PS5_FEX_C13_PATCH}=${LUMEN_PS5_FEX_C13_PATCH_SHA256}"
    OUTPUT_VARIABLE LUMEN_ANDROID_FEX_PATCHED_SOURCE
    OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE LUMEN_ANDROID_FEX_OVERLAY_RESULT)
if(NOT LUMEN_ANDROID_FEX_OVERLAY_RESULT EQUAL 0 OR
   NOT IS_DIRECTORY "${LUMEN_ANDROID_FEX_PATCHED_SOURCE}")
    message(FATAL_ERROR "Could not verify the C13 PS5 FEX source overlay")
endif()
set(LUMEN_ANDROID_FEXCORE_BUILD_SOURCE_REAL "${LUMEN_ANDROID_FEX_PATCHED_SOURCE}"
    CACHE INTERNAL "Verified Android FEX overlay" FORCE)
set(LUMEN_ANDROID_FEXCORE_SOURCE_REAL "${LUMEN_ANDROID_FEX_PATCHED_SOURCE}")
