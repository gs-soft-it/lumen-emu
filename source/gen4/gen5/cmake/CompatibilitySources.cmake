# SPDX-License-Identifier: GPL-2.0-only
include_guard(GLOBAL)

# The source repository holds common files only once. A coherent build view
# preserves local includes and the desktop/facade source inventory exactly.
function(lumen_gen5_prepare_compatibility_sources output_variable)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(gen5_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(prepare_tool "${gen5_root}/tools/PrepareCompatibilitySources.py")
    file(GLOB_RECURSE compatibility_inputs CONFIGURE_DEPENDS
        "${gen5_root}/compatibility/src/*" "${gen5_root}/compatibility/tests/*"
        "${gen5_root}/engine/src/*" "${gen5_root}/engine/tests/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${compatibility_inputs} "${gen5_root}/compatibility/shared-sources.json" "${prepare_tool}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${prepare_tool}"
            --source-root "${gen5_root}"
            --output-root "${CMAKE_BINARY_DIR}/gen5-compatibility-sources"
        RESULT_VARIABLE prepare_result OUTPUT_VARIABLE prepared_source
        ERROR_VARIABLE prepare_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT prepare_result EQUAL 0)
        message(FATAL_ERROR "Gen5 compatibility source preparation failed: ${prepare_error}")
    endif()
    set(${output_variable} "${prepared_source}" PARENT_SCOPE)
endfunction()
