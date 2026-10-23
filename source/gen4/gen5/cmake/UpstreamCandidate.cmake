# SPDX-License-Identifier: GPL-2.0-only
# A generated source overlay keeps the accepted engine available unchanged.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(lumen_ps5_update_dir "${CMAKE_CURRENT_LIST_DIR}/../updates/20261018")
set(lumen_ps5_update_tool "${CMAKE_CURRENT_LIST_DIR}/../tools/PrepareUpstreamCandidate.py")
file(GLOB_RECURSE lumen_ps5_candidate_inputs CONFIGURE_DEPENDS
    "${KYTY_SOURCE_DIR}/*" "${KYTY_TESTS_DIR}/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${lumen_ps5_candidate_inputs}
    "${lumen_ps5_update_dir}/manifest.json"
    "${lumen_ps5_update_dir}/kyty.patch"
    "${lumen_ps5_update_tool}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${lumen_ps5_update_tool}"
        --source "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}"
        --manifest "${lumen_ps5_update_dir}/manifest.json"
        --output-root "${CMAKE_CURRENT_BINARY_DIR}/upstream-candidate"
    RESULT_VARIABLE lumen_ps5_update_result
    OUTPUT_VARIABLE lumen_ps5_update_source
    ERROR_VARIABLE lumen_ps5_update_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT lumen_ps5_update_result EQUAL 0)
    message(FATAL_ERROR "PS5 candidate preparation failed: ${lumen_ps5_update_error}")
endif()
set(KYTY_SOURCE_DIR "${lumen_ps5_update_source}/src")
set(KYTY_TESTS_DIR "${lumen_ps5_update_source}/tests")
add_compile_definitions(LUMEN_PS5_UPSTREAM_20261018=1)
message(STATUS "PS5 upstream candidate: ${lumen_ps5_update_source}")
