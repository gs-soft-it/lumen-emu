# SPDX-License-Identifier: GPL-2.0-only
# Native CLI and correctness fixtures share exactly the production engine.
if(LUMEN_PS5_NATIVE_METAL)
  include("${CMAKE_CURRENT_LIST_DIR}/NativeMetal.cmake")
  list(APPEND kyty_emulator_link_libraries lumen_ps5_native_metal)
endif()
list(REMOVE_ITEM kyty_emulator_src
  "${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
  "${KYTY_SOURCE_DIR}/loader/redZonePatcher.cpp"
  "${KYTY_SOURCE_DIR}/loader/guestInstructionPatcher.cpp"
  "${KYTY_SOURCE_DIR}/loader/fexRuntimeTests.cpp"
  "${KYTY_SOURCE_DIR}/loader/fexGpuTests.cpp")
add_library(lumen_ps5_direct_engine STATIC ${kyty_emulator_src})
target_include_directories(lumen_ps5_direct_engine PUBLIC ${inc_headers} "${PROJECT_BINARY_DIR}")
target_compile_options(lumen_ps5_direct_engine PRIVATE -fexceptions)
target_link_libraries(lumen_ps5_direct_engine PUBLIC
  lumen_ps5_fex_embedding ${kyty_emulator_link_libraries} Threads::Threads)
add_executable(kyty_emulator "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_compile_options(kyty_emulator PRIVATE -fexceptions)
target_link_libraries(kyty_emulator PRIVATE lumen_ps5_direct_engine)
target_link_options(kyty_emulator PRIVATE "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/kyty_emulator.map")
add_executable(lumen_ps5_direct_cli_forwarding_test EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_SOURCE_DIR}/../tests/DirectCliForwardingTest.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../tests/DirectCliForwardingBoundary.cpp")
target_compile_options(lumen_ps5_direct_cli_forwarding_test PRIVATE -fexceptions)
target_link_libraries(lumen_ps5_direct_cli_forwarding_test PRIVATE lumen_ps5_direct_engine)
set(LUMEN_PS5_DIRECT_ENGINE_TARGET lumen_ps5_direct_engine)
include("${CMAKE_CURRENT_SOURCE_DIR}/../engine/Tests.cmake")
add_executable(lumen_ps5_direct_guest_io_test EXCLUDE_FROM_ALL
  "${KYTY_TESTS_DIR}/DirectGuestIoTests.cpp")
target_compile_options(lumen_ps5_direct_guest_io_test PRIVATE -fexceptions)
target_link_libraries(lumen_ps5_direct_guest_io_test PRIVATE lumen_ps5_direct_engine)
if(LUMEN_PS5_NATIVE_METAL)
  add_executable(lumen_ps5_direct_arm64_memory_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/Arm64MemoryAccessTests.cpp"
    "${KYTY_SOURCE_DIR}/common/arm64MemoryAccess.cpp")
  target_include_directories(lumen_ps5_direct_arm64_memory_test PRIVATE "${KYTY_SOURCE_DIR}")
  target_compile_features(lumen_ps5_direct_arm64_memory_test PRIVATE cxx_std_20)
  target_link_libraries(lumen_ps5_direct_arm64_memory_test PRIVATE Threads::Threads)
  add_executable(lumen_ps5_direct_metal_window_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/DirectMetalWindowTests.cpp")
  target_include_directories(lumen_ps5_direct_metal_window_test PRIVATE ${inc_headers})
  target_compile_options(lumen_ps5_direct_metal_window_test PRIVATE -fexceptions)
  target_link_libraries(lumen_ps5_direct_metal_window_test PRIVATE lumen_ps5_direct_engine)
  add_executable(lumen_ps5_direct_metal_tessellator_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/../../../tests/MetalSoftwareTessellatorTest.mm")
  set_target_properties(lumen_ps5_direct_metal_tessellator_test PROPERTIES
    OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
  target_compile_options(lumen_ps5_direct_metal_tessellator_test PRIVATE -fobjc-arc)
  target_link_libraries(lumen_ps5_direct_metal_tessellator_test PRIVATE
    "-framework Foundation" "-framework Metal")
  add_executable(lumen_ps5_direct_metal_cull_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/../../../tests/MetalCullDistanceTest.mm")
  set_target_properties(lumen_ps5_direct_metal_cull_test PROPERTIES
    OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
  target_compile_options(lumen_ps5_direct_metal_cull_test PRIVATE -fobjc-arc)
  target_include_directories(lumen_ps5_direct_metal_cull_test PRIVATE
    "${KYTY_THIRD_PARTY_DIR}/Vulkan-Headers/include")
  target_link_libraries(lumen_ps5_direct_metal_cull_test PRIVATE lumen_ps5_native_metal)
endif()

function(lumen_ps5_sign_direct target)
  set(profile "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/direct-macos.entitlements")
  if(NOT LUMEN_PS5_NATIVE_METAL AND
     (target STREQUAL "kyty_emulator" OR target STREQUAL "lumen_ps5_direct_renderer_test"))
    set(profile "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/direct-macos-vulkan.entitlements")
  endif()
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${profile}")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND codesign --force --sign - --options runtime
      --entitlements "${profile}"
      "$<TARGET_FILE:${target}>" VERBATIM)
endfunction()
lumen_ps5_sign_direct(kyty_emulator)
add_test(NAME lumen_ps5_direct_cli_help COMMAND kyty_emulator --help)
set_tests_properties(lumen_ps5_direct_cli_help PROPERTIES PASS_REGULAR_EXPRESSION "fex")
get_property(direct_targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS direct_targets)
  if(target MATCHES "^lumen_ps5_.*_test$")
    lumen_ps5_sign_direct(${target})
    if(NOT target STREQUAL "lumen_ps5_direct_renderer_test")
      add_test(NAME ${target} COMMAND ${target})
      set_tests_properties(${target} PROPERTIES TIMEOUT 90 ENVIRONMENT "LUMEN_CPU_TRANSLATOR=fex")
    endif()
  endif()
endforeach()
if(LUMEN_PS5_NATIVE_METAL)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/metal-window-fixture")
  set_tests_properties(lumen_ps5_direct_metal_window_test PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/metal-window-fixture")
endif()
# JIT smoke lives in a child directory and is signed there.
add_test(NAME lumen_ps5_direct_cpu COMMAND lumen_ps5_direct_fex_smoke)
set_tests_properties(lumen_ps5_direct_cpu PROPERTIES TIMEOUT 60 ENVIRONMENT "LUMEN_CPU_TRANSLATOR=fex")
if(NOT LUMEN_PS5_NATIVE_METAL)
  find_library(LUMEN_PS5_TEST_VULKAN_LIBRARY NAMES MoltenVK HINTS /opt/homebrew/lib
    DOC "Native Vulkan implementation for the direct renderer correctness tests")
endif()
set(direct_renderer_modes bda-page-table buffer-cache-range)
if(LUMEN_PS5_NATIVE_METAL)
  list(APPEND direct_renderer_modes native-metal-contract native-metal-vertex-subgroup null-descriptor-fallback
    texture-head-cpu-write texture-tail-cpu-write native-metal-tessellation
    native-metal-cull-distance tessellation agc-address-admission)
endif()
foreach(mode IN LISTS direct_renderer_modes)
  add_test(NAME lumen_ps5_direct_renderer_${mode}
    COMMAND lumen_ps5_direct_renderer_test --${mode}-only)
  set_tests_properties(lumen_ps5_direct_renderer_${mode} PROPERTIES TIMEOUT 120)
  if(NOT LUMEN_PS5_NATIVE_METAL AND LUMEN_PS5_TEST_VULKAN_LIBRARY)
    set_tests_properties(lumen_ps5_direct_renderer_${mode} PROPERTIES
      ENVIRONMENT "SDL_VULKAN_LIBRARY=${LUMEN_PS5_TEST_VULKAN_LIBRARY}")
  endif()
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(direct_cli_test_args)
if(LUMEN_PS5_NATIVE_METAL)
  list(APPEND direct_cli_test_args --native-metal)
endif()
add_test(NAME lumen_ps5_direct_cli_admission
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tests/DirectCliTests.py"
    "$<TARGET_FILE:kyty_emulator>" ${direct_cli_test_args})
install(TARGETS kyty_emulator DESTINATION .)
