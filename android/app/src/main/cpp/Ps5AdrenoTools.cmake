# SPDX-License-Identifier: GPL-2.0-only
# Shared PS4/PS5 Android host driver; configure before source/gen4/gen5.
# The existing cache/property name remains compatible. No downloads at configure/build.
include_guard(GLOBAL)
set(LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR "" CACHE PATH "Hash-bound official adrenotools source closure")
set(LUMEN_PS5_SDL3_VULKAN_ADAPTER_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/Ps5Sdl3VulkanAdapter.cmake")
set(LUMEN_PS5_ADRENOTOOLS_ENABLED OFF)
if(LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR)
    file(READ "${LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR}/SOURCE.json" _adreno_receipt)
    string(JSON _schema GET "${_adreno_receipt}" schema)
    string(JSON _modified GET "${_adreno_receipt}" upstream_modified)
    string(JSON _public GET "${_adreno_receipt}" public_binary_distribution_qualified)
    if(NOT _schema STREQUAL "LUMEN_PS5_ADRENOTOOLS_SOURCE_V1" OR _modified OR _public)
        message(FATAL_ERROR "Adrenotools source receipt is not qualified for this local build")
    endif()
    set(_pins "8fae8ce254dfc1344527e05301e43f37dea2df80" "aa3975893d83ef1bc84c321ec60c65fbf1287887")
    set(_hashes "ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb" "da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638")
    set(_names libadrenotools liblinkernsbypass)
    foreach(_index RANGE 0 1)
        string(JSON _pin GET "${_adreno_receipt}" sources ${_index} commit)
        string(JSON _hash GET "${_adreno_receipt}" sources ${_index} archive_sha256)
        string(JSON _name GET "${_adreno_receipt}" sources ${_index} name)
        string(JSON _license GET "${_adreno_receipt}" sources ${_index} license)
        list(GET _pins ${_index} _expected_pin)
        list(GET _hashes ${_index} _expected_hash)
        list(GET _names ${_index} _expected_name)
        if(NOT _pin STREQUAL _expected_pin OR NOT _hash STREQUAL _expected_hash
                OR NOT _name STREQUAL _expected_name OR NOT _license STREQUAL "BSD-2-Clause")
            message(FATAL_ERROR "Adrenotools reviewed source pin mismatch")
        endif()
    endforeach()
    string(JSON _file_count LENGTH "${_adreno_receipt}" files)
    if(NOT _file_count EQUAL 50)
        message(FATAL_ERROR "Adrenotools source closure member count mismatch")
    endif()
    math(EXPR _file_last "${_file_count}-1")
    set(_tree_text "")
    foreach(_index RANGE 0 ${_file_last})
        string(JSON _relative MEMBER "${_adreno_receipt}" files ${_index})
        if(_relative MATCHES "(^/|(^|/)\\.\\.(/|$))")
            message(FATAL_ERROR "Adrenotools source path escapes closure")
        endif()
        string(JSON _expected GET "${_adreno_receipt}" files "${_relative}")
        if(IS_SYMLINK "${LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR}/${_relative}")
            message(FATAL_ERROR "Adrenotools source symlink is forbidden: ${_relative}")
        endif()
        file(SHA256 "${LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR}/${_relative}" _actual)
        if(NOT _actual STREQUAL _expected)
            message(FATAL_ERROR "Adrenotools source hash mismatch: ${_relative}")
        endif()
        string(APPEND _tree_text "${_relative}:${_actual}\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR}/${_relative}")
    endforeach()
    string(SHA256 _tree_hash "${_tree_text}")
    if(NOT _tree_hash STREQUAL "d73b46ba72829dc3771a6017713d4d5561dd05789a9dfc06b8d57a1ff6e626e7")
        message(FATAL_ERROR "Adrenotools reviewed source tree seal mismatch")
    endif()
    file(GLOB_RECURSE _source_members LIST_DIRECTORIES FALSE
        "${LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR}/*")
    list(LENGTH _source_members _source_member_count)
    if(NOT _source_member_count EQUAL 52)
        message(FATAL_ERROR "Adrenotools source closure contains unexpected files")
    endif()
    # Scope upstream's untyped add_library to static: only its two mandatory
    # namespace hooks are DSOs. Optional redirection/GPU mapping is not enabled.
    function(_lumen_add_adrenotools)
        set(BUILD_SHARED_LIBS OFF)
        add_subdirectory("${LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/ps5-adrenotools")
    endfunction()
    _lumen_add_adrenotools()
    foreach(_target adrenotools linkernsbypass hook_impl main_hook)
        set_target_properties(${_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_compile_options(${_target} PRIVATE -O2)
    endforeach()
    foreach(_target hook_impl main_hook)
        target_link_options(${_target} PRIVATE "-Wl,-z,max-page-size=16384"
            "-Wl,-z,common-page-size=16384" "-Wl,--build-id=sha1")
        set_target_properties(${_target} PROPERTIES SKIP_BUILD_RPATH TRUE)
    endforeach()
    set_target_properties(file_redirect_hook gsl_alloc_hook PROPERTIES EXCLUDE_FROM_ALL TRUE)
    set(LUMEN_PS5_ADRENOTOOLS_ENABLED ON)
endif()

# Compile this neutral unit separately: PS4's legacy Vulkan headers must not
# shadow the NDK headers needed by the custom-driver identity probe.
add_library(lumen_android_vulkan_driver_host STATIC
    "${CMAKE_CURRENT_LIST_DIR}/AndroidVulkanDriver.cpp")
set_target_properties(lumen_android_vulkan_driver_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(lumen_android_vulkan_driver_host PRIVATE cxx_std_17)
target_compile_options(lumen_android_vulkan_driver_host PRIVATE -O2 -Wall -Wextra -Werror -fno-exceptions)
target_link_libraries(lumen_android_vulkan_driver_host PRIVATE
    "${CMAKE_DL_LIBS}" "${LUMEN_ANDROID_LOG_LIBRARY}")
if(LUMEN_PS5_ADRENOTOOLS_ENABLED)
    target_compile_definitions(lumen_android_vulkan_driver_host PRIVATE LUMEN_ANDROID_ADRENOTOOLS=1)
    target_link_libraries(lumen_android_vulkan_driver_host PRIVATE adrenotools)
    add_dependencies(lumen_android_vulkan_driver_host main_hook hook_impl)
endif()

function(_lumen_attach_vulkan_driver_host target)
    target_link_libraries(${target} PRIVATE lumen_android_vulkan_driver_host)
endfunction()

function(lumen_ps5_attach_vulkan_driver target)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidPs5VulkanDriver.cpp")
    _lumen_attach_vulkan_driver_host(${target})
endfunction()

function(lumen_ps4_attach_vulkan_driver target)
    if(NOT TARGET SDL2-static)
        message(FATAL_ERROR "PS4 private Vulkan loader requires SDL2-static")
    endif()
    # SDL2's Android Vulkan backend calls these hooks, so the adapter and its
    # host closure belong to SDL2 itself, including non-runtime/test consumers.
    # A separate host archive keeps SDL2/legacy Vulkan includes out of the core.
    target_sources(SDL2-static PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidPs4VulkanDriver.cpp")
    target_compile_features(SDL2-static PRIVATE cxx_std_17)
    target_compile_definitions(SDL2-static PRIVATE LUMEN_ANDROID_PRIVATE_VULKAN_LOADER=1)
    _lumen_attach_vulkan_driver_host(SDL2-static)
    target_link_libraries(${target} PRIVATE SDL2-static)
endfunction()

# Device gate for fail-closed selection and process-lifetime loader handles.
# Separate process per case; this does not qualify renderer capabilities.
add_executable(lumen_android_vulkan_driver_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/../../test/cpp/AndroidVulkanDriverTest.cpp")
target_include_directories(lumen_android_vulkan_driver_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_features(lumen_android_vulkan_driver_test PRIVATE cxx_std_17)
target_compile_options(lumen_android_vulkan_driver_test PRIVATE -Wall -Wextra -Werror)
target_link_options(lumen_android_vulkan_driver_test PRIVATE
    "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
target_link_libraries(lumen_android_vulkan_driver_test PRIVATE "${LUMEN_ANDROID_LOG_LIBRARY}")
_lumen_attach_vulkan_driver_host(lumen_android_vulkan_driver_test)
