# SPDX-License-Identifier: GPL-2.0-only
if(NOT LUMEN_PS5_NATIVE_METAL)
  message(FATAL_ERROR "The iOS FEX runtime requires native Metal")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/NativeMetal.cmake")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(spirv_grammar_dir "${KYTY_THIRD_PARTY_DIR}/SPIRV-Headers/include/spirv/unified1")
set(spirv_grammar_generator "${CMAKE_CURRENT_LIST_DIR}/../tools/GenerateSpirvOperandGrammar.py")
set(spirv_grammar_header "${PROJECT_BINARY_DIR}/lumenSpirvOperandGrammar.inc")
add_custom_command(OUTPUT "${spirv_grammar_header}"
  COMMAND "${Python3_EXECUTABLE}" "${spirv_grammar_generator}"
    "${spirv_grammar_dir}/spirv.core.grammar.json"
    "${spirv_grammar_dir}/extinst.glsl.std.450.grammar.json" "${spirv_grammar_header}"
  DEPENDS "${spirv_grammar_generator}" "${spirv_grammar_dir}/spirv.core.grammar.json"
    "${spirv_grammar_dir}/extinst.glsl.std.450.grammar.json" VERBATIM)
list(REMOVE_ITEM kyty_emulator_src
  "${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
  "${KYTY_SOURCE_DIR}/loader/redZonePatcher.cpp"
  "${KYTY_SOURCE_DIR}/loader/guestInstructionPatcher.cpp"
  "${KYTY_SOURCE_DIR}/loader/fexRuntimeTests.cpp"
  "${KYTY_SOURCE_DIR}/loader/fexGpuTests.cpp")
add_library(lumen_ios_ps5_fex_runtime SHARED
  ${kyty_emulator_src}
  "${spirv_grammar_header}"
  "${KYTY_SOURCE_DIR}/graphics/shader/spirvBinary/reader.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/lumen/ios_module.cpp")
target_include_directories(lumen_ios_ps5_fex_runtime PRIVATE
  ${inc_headers} "${PROJECT_BINARY_DIR}")
target_compile_options(lumen_ios_ps5_fex_runtime PRIVATE -fexceptions)
target_compile_definitions(lumen_ios_ps5_fex_runtime PRIVATE LUMEN_PS5_IOS_SPIRV_READER=1)
target_link_libraries(lumen_ios_ps5_fex_runtime PRIVATE
  lumen_ps5_fex_embedding ${kyty_emulator_link_libraries}
  lumen_ps5_native_metal Threads::Threads)
# SDL's CP932 conversion uses the SDK iconv API. A static try_compile cannot
# prove whether those functions are part of libc, so link the native stub.
find_library(LUMEN_PS5_IOS_ICONV_LIBRARY NAMES iconv REQUIRED)
target_link_libraries(lumen_ios_ps5_fex_runtime PUBLIC "${LUMEN_PS5_IOS_ICONV_LIBRARY}")
include("${CMAKE_CURRENT_LIST_DIR}/IOSRuntimeModule.cmake")
lumen_configure_ios_runtime_module(lumen_ios_ps5_fex_runtime LumenPS5Runtime)
install(TARGETS lumen_ios_ps5_fex_runtime FRAMEWORK DESTINATION .)
