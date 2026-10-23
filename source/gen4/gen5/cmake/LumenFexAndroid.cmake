# SPDX-License-Identifier: GPL-2.0-only
# Reuse the app's reviewed Android FEX provider rather than a second checkout.
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a" OR
   NOT TARGET lumen_android_fexcore_runtime OR
   NOT TARGET lumen_android_fexcore_lab_admission)
  message(FATAL_ERROR "The Android PS5 engine requires the app's audited FEX targets")
endif()
add_library(lumen_ps5_fex_embedding INTERFACE)
target_compile_definitions(lumen_ps5_fex_embedding INTERFACE LUMEN_PS5_FEXCORE=1)
target_include_directories(lumen_ps5_fex_embedding INTERFACE
  "${CMAKE_CURRENT_LIST_DIR}/../../../../android/app/src/main/cpp")
if(LUMEN_PS5_DIRECT_FEX)
  target_compile_definitions(lumen_ps5_fex_embedding INTERFACE LUMEN_PS5_DIRECT_FEX=1)
  target_link_libraries(lumen_ps5_fex_embedding INTERFACE lumen_ps5_direct_fex)
else()
  target_link_libraries(lumen_ps5_fex_embedding INTERFACE lumen_android_fexcore_runtime)
endif()
