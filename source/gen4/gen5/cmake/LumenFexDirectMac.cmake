# SPDX-License-Identifier: GPL-2.0-only
# The complete direct Gen5 graph owns one C13 translator, including the same
# verified Darwin overlay and configuration closure used by native Gen4.
include_guard(GLOBAL)
if(NOT APPLE OR CMAKE_CROSSCOMPILING OR
   NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR NOT LUMEN_PS5_DIRECT_FEX)
  message(FATAL_ERROR "Gen5 direct FEX requires native macOS arm64")
endif()
if(TARGET fmt::fmt OR TARGET FEXCore OR TARGET lumen_macos_fexcore_runtime)
  message(FATAL_ERROR "The Gen5 direct runtime must own an isolated CPU/dependency graph")
endif()
get_filename_component(LUMEN_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(LUMEN_SOURCE_ROOT "${LUMEN_REPOSITORY_ROOT}/source")
find_package(Threads REQUIRED)
# FEX's configuration layer uses dynamic_cast. The engine's inherited
# -fno-rtti/-fno-exceptions policy must not leak into the shared CPU sources.
set(direct_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
get_directory_property(direct_saved_includes INCLUDE_DIRECTORIES)
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")
string(APPEND CMAKE_CXX_FLAGS " -frtti -fexceptions")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../direct_fex" direct-fex)
set(CMAKE_CXX_FLAGS "${direct_saved_cxx_flags}")
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${direct_saved_includes}")
add_library(lumen_ps5_fex_embedding INTERFACE)
target_compile_definitions(lumen_ps5_fex_embedding INTERFACE
  LUMEN_PS5_FEXCORE=1 LUMEN_PS5_DIRECT_FEX=1)
target_link_libraries(lumen_ps5_fex_embedding INTERFACE lumen_ps5_direct_fex)
target_include_directories(lumen_ps5_fex_embedding INTERFACE
  "${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp"
  "${LUMEN_SOURCE_ROOT}/cpu/runtime")
include_directories(BEFORE
  "${LUMEN_PS5_DIRECT_FEX_SOURCE_REAL}/External/fmt/include"
  "${LUMEN_PS5_DIRECT_FEX_SOURCE_REAL}/External/xxhash")
