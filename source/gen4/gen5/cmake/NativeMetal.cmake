# SPDX-License-Identifier: GPL-2.0-only
# Reuse the source-owned Gen4 Metal implementation in the isolated Gen5 graph.
# Vulkan names remain an internal command interface, never an external driver.
get_filename_component(metal_repository "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(metal_source "${metal_repository}/source/emulator/src/Graphics/Metal")
include("${metal_repository}/source/3rdparty/spirv_cross_msl.cmake")
add_library(lumen_ps5_native_metal STATIC
  "${metal_source}/MetalVulkanCore.mm"
  "${metal_source}/MetalCullDistance.mm"
  "${metal_source}/MetalVulkanCommands.mm")
set_target_properties(lumen_ps5_native_metal PROPERTIES
  POSITION_INDEPENDENT_CODE ON OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON
  OBJCXX_EXTENSIONS OFF)
target_compile_definitions(lumen_ps5_native_metal PRIVATE
  LUMEN_APPLE_NATIVE_METAL=1 LUMEN_PS5_NATIVE_METAL=1)
target_compile_options(lumen_ps5_native_metal PRIVATE
  -fobjc-arc -fno-rtti -fno-exceptions -Wno-unqualified-std-cast-call)
target_include_directories(lumen_ps5_native_metal PRIVATE "${metal_source}")
target_include_directories(lumen_ps5_native_metal BEFORE PRIVATE
  "${KYTY_THIRD_PARTY_DIR}/Vulkan-Headers/include")
# The shared adapter uses the local generated enum-name helper. Keep the
# engine's pinned core headers first; this directory supplies only that helper.
target_include_directories(lumen_ps5_native_metal PRIVATE
  "${metal_repository}/source/3rdparty/vulkan/include")
target_link_libraries(lumen_ps5_native_metal PRIVATE
  Lumen::SpirvCrossMsl Vulkan::Headers)
foreach(framework IN ITEMS Metal QuartzCore Foundation AppKit)
  find_library(LUMEN_PS5_${framework}_FRAMEWORK NAMES "${framework}" REQUIRED)
  target_link_libraries(lumen_ps5_native_metal PUBLIC "${LUMEN_PS5_${framework}_FRAMEWORK}")
endforeach()
# Keep the engine's static dispatch entry point in the executable.
target_link_options(lumen_ps5_native_metal INTERFACE
  "LINKER:-u,_vkGetInstanceProcAddr" "LINKER:-exported_symbol,_vkGetInstanceProcAddr")
message(STATUS "Gen5 graphics: native Metal (source-owned static backend)")
