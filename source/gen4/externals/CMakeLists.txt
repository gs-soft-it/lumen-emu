# SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set_directory_properties(PROPERTIES
    EXCLUDE_FROM_ALL ON
    SYSTEM ON
)

# Set CMP0069 policy to "NEW" in order to ensure consistent behavior when building external targets with LTO enabled
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

if (MSVC)
    # Silence "deprecation" warnings
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE -D_SCL_SECURE_NO_WARNINGS)
endif()

# Boost
if (NOT TARGET Boost::headers)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/ext-boost" "${CMAKE_CURRENT_BINARY_DIR}/ext-boost")
endif()

# spdlog
set(SPDLOG_NO_EXCEPTIONS ON)
set(SPDLOG_DISABLE_GLOBAL_LOGGER ON)
if (TARGET fmt::fmt)
    set(SPDLOG_FMT_EXTERNAL ON)
endif()
add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/spdlog" "${CMAKE_CURRENT_BINARY_DIR}/spdlog" EXCLUDE_FROM_ALL SYSTEM)
target_compile_definitions(spdlog PUBLIC SPDLOG_FUNCTION=__func__)

# fmtlib
if (NOT TARGET fmt::fmt)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/fmt" "${CMAKE_CURRENT_BINARY_DIR}/fmt")
endif()

# FFmpeg
if (NOT TARGET FFmpeg::ffmpeg)
    add_subdirectory(ffmpeg-core)
    add_library(FFmpeg::ffmpeg ALIAS ffmpeg)
endif()

# LibAtrac9
set(LIBATRAC9_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../3rdparty/libatrac9/C/src")
file(GLOB LIBATRAC9_SOURCES CONFIGURE_DEPENDS "${LIBATRAC9_SOURCE_DIR}/*.c")
add_library(LibAtrac9 STATIC ${LIBATRAC9_SOURCES})
target_include_directories(LibAtrac9 INTERFACE "${LIBATRAC9_SOURCE_DIR}")
set_target_properties(LibAtrac9 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
if(UNIX AND NOT APPLE)
    target_link_libraries(LibAtrac9 PUBLIC m)
endif()

# Direct VFS archive backend
if(ENABLE_FEX_GUEST_CPU)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
    set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/zstd/build/cmake"
                     "${CMAKE_CURRENT_BINARY_DIR}/zstd")
    add_library(zarchive STATIC
        "${LUMEN_GEN4_DEPS_DIR}/zarchive/src/zarchivereader.cpp"
        "${LUMEN_GEN4_DEPS_DIR}/zarchive/src/zarchivewriter.cpp"
        "${LUMEN_GEN4_DEPS_DIR}/zarchive/src/sha_256.c")
    target_include_directories(zarchive PUBLIC "${LUMEN_GEN4_DEPS_DIR}/zarchive/include")
    target_include_directories(zarchive PRIVATE "${LUMEN_GEN4_DEPS_DIR}/zstd/lib")
    target_link_libraries(zarchive PRIVATE libzstd_static)
    target_compile_features(zarchive PRIVATE cxx_std_20)
    add_library(ZArchive::zarchive ALIAS zarchive)
endif()

# zlib
if (NOT TARGET ZLIB::ZLIB)
    set(ZLIB_ENABLE_TESTS OFF)
    set(WITH_GTEST OFF)
    set(WITH_NEW_STRATEGIES ON)
    set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
    include(FetchContent)
    FetchContent_Declare(
        ZLIB
        SOURCE_DIR "${LUMEN_GEN4_DEPS_DIR}/zlib-ng"
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib)
    # libpng expects this variable to exist after its find_package(ZLIB)
    set(ZLIB_INCLUDE_DIRS "${FETCHCONTENT_BASE_DIR}/zlib-build")
endif()

# FreeType
if (NOT TARGET Freetype::Freetype)
    # Keep optional deps off to reduce build surface; zlib stays enabled for compressed fonts.
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/freetype" "${CMAKE_CURRENT_BINARY_DIR}/freetype")
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# SDL3
if (NOT TARGET SDL3::SDL3)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_PIPEWIRE OFF)
    set(SDL_AVX512F OFF)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    if(ANDROID)
        # Keep upstream immutable; this JNI namespace coexists with the other runtime.
        set(gen4_sdl_source "${CMAKE_CURRENT_BINARY_DIR}/sdl3-android-source")
        file(REAL_PATH "${LUMEN_GEN4_DEPS_DIR}/sdl3" gen4_sdl_upstream)
        if(IS_SYMLINK "${gen4_sdl_source}")
            # A source-directory link must never become the writable overlay.
            file(REMOVE "${gen4_sdl_source}")
        endif()
        file(COPY "${gen4_sdl_upstream}/" DESTINATION "${gen4_sdl_source}" PATTERN ".git" EXCLUDE)
        include("${CMAKE_CURRENT_LIST_DIR}/../cmake/SDLAndroidEventWake.cmake")
        lumen_gen4_patch_sdl_android_event_wake("${gen4_sdl_upstream}" "${gen4_sdl_source}")
        foreach(binding IN ITEMS src/core/android/SDL_android.c src/hidapi/android/hid.cpp src/video/android/SDL_androidevents.c)
            file(READ "${LUMEN_GEN4_DEPS_DIR}/sdl3/${binding}" binding_text)
            string(REPLACE "org_libsdl_app" "org_libsdlgen4_app" binding_text "${binding_text}")
            string(REPLACE "org/libsdl/app" "org/libsdlgen4/app" binding_text "${binding_text}")
            file(WRITE "${gen4_sdl_source}/${binding}" "${binding_text}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LUMEN_GEN4_DEPS_DIR}/sdl3/${binding}")
        endforeach()
        set(gen4_vulkan_backend "${gen4_sdl_source}/src/video/android/SDL_androidvulkan.c")
        file(READ "${gen4_sdl_upstream}/src/video/android/SDL_androidvulkan.c" gen4_vulkan_text)
        foreach(function IN ITEMS SDL_LoadObject SDL_LoadFunction SDL_UnloadObject)
            string(FIND "${gen4_vulkan_text}" "${function}(" position)
            if(position LESS 0)
                message(FATAL_ERROR "Gen4 SDL3 Vulkan host adapter contract changed: ${function}")
            endif()
            string(REPLACE "${function}(" "LumenGen4${function}(" gen4_vulkan_text "${gen4_vulkan_text}")
        endforeach()
        set(gen4_declarations "\nextern SDL_SharedObject *LumenGen4SDL_LoadObject(const char *path);\nextern SDL_FunctionPointer LumenGen4SDL_LoadFunction(SDL_SharedObject *object, const char *name);\nextern void LumenGen4SDL_UnloadObject(SDL_SharedObject *object);\n")
        string(REPLACE "#include \"SDL_androidvulkan.h\"" "#include \"SDL_androidvulkan.h\"\n${gen4_declarations}" gen4_vulkan_text "${gen4_vulkan_text}")
        file(WRITE "${gen4_vulkan_backend}" "${gen4_vulkan_text}")
        add_subdirectory("${gen4_sdl_source}" "${CMAKE_CURRENT_BINARY_DIR}/sdl3")
    else()
        add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/sdl3" "${CMAKE_CURRENT_BINARY_DIR}/sdl3")
    endif()
endif()

# vulkan-headers
if (NOT TARGET Vulkan::Headers)
    set(VULKAN_HEADERS_ENABLE_MODULE OFF)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/vulkan-headers" "${CMAKE_CURRENT_BINARY_DIR}/vulkan-headers")
    set(VulkanHeaders_VERSION "1.4.330")
endif()

# VMA
if (NOT TARGET GPUOpen::VulkanMemoryAllocator)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/vma" "${CMAKE_CURRENT_BINARY_DIR}/vma")
endif()

# RenderDoc
if (NOT TARGET RenderDoc::API)
    add_library(renderdoc INTERFACE)
    target_include_directories(renderdoc INTERFACE ./renderdoc)
    add_library(RenderDoc::API ALIAS renderdoc)
endif()

# glslang
if (NOT TARGET glslang::glslang)
    set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "")
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "")
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "")
    set(ENABLE_CTEST OFF CACHE BOOL "")
    set(ENABLE_HLSL OFF CACHE BOOL "")
    set(BUILD_EXTERNAL OFF CACHE BOOL "")
    set(ENABLE_OPT OFF CACHE BOOL "")
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/glslang" "${CMAKE_CURRENT_BINARY_DIR}/glslang")
    file(COPY "${LUMEN_GEN4_DEPS_DIR}/glslang/SPIRV" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/glslang/glslang" FILES_MATCHING PATTERN "*.h")
    target_include_directories(glslang INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/glslang")
endif()

# Robin-map
if (NOT TARGET tsl::robin_map)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/robin-map" "${CMAKE_CURRENT_BINARY_DIR}/robin-map")
endif()

# Xbyak
if (NOT TARGET xbyak::xbyak)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/xbyak" "${CMAKE_CURRENT_BINARY_DIR}/xbyak")
endif()

# MagicEnum
if (NOT TARGET magic_enum::magic_enum)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/magic_enum" "${CMAKE_CURRENT_BINARY_DIR}/magic_enum")
endif()

# Toml11
if (NOT TARGET toml11::toml11)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/toml11" "${CMAKE_CURRENT_BINARY_DIR}/toml11")

    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        get_target_property(_toml11_compile_options toml11 INTERFACE_COMPILE_OPTIONS)
        list(REMOVE_ITEM _toml11_compile_options "/Zc:preprocessor")
        set_target_properties(toml11 PROPERTIES INTERFACE_COMPILE_OPTIONS ${_toml11_compile_options})
      endif()
    endif()
endif()

# xxHash
if (NOT TARGET xxHash::xxhash)
    add_library(xxhash ${LUMEN_GEN4_DEPS_DIR}/xxhash/xxhash.h ${LUMEN_GEN4_DEPS_DIR}/xxhash/xxhash.c)
    target_include_directories(xxhash PUBLIC "${LUMEN_GEN4_DEPS_DIR}/xxhash")
    add_library(xxHash::xxhash ALIAS xxhash)
endif()

# Zydis
if (NOT TARGET Zydis::Zydis)
    option(ZYDIS_BUILD_TOOLS "" OFF)
    option(ZYDIS_BUILD_EXAMPLES "" OFF)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/zydis" "${CMAKE_CURRENT_BINARY_DIR}/zydis")
endif()

# sirit
add_subdirectory("${LUMEN_GEN4_SIRIT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sirit")
if (WIN32)
    target_compile_options(sirit PRIVATE $<$<CXX_COMPILER_ID:Clang>:-Wno-error=unused-command-line-argument>)
endif()

# half
if (NOT TARGET half::half)
    add_library(half INTERFACE)
    target_include_directories(half INTERFACE ${LUMEN_GEN4_DEPS_DIR}/half/include)
    add_library(half::half ALIAS half)
endif()

# libpng
if (NOT TARGET PNG::PNG)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_STATIC ON CACHE BOOL "" FORCE)
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/libpng" "${CMAKE_CURRENT_BINARY_DIR}/libpng")
    add_library(PNG::PNG ALIAS png_static)
endif()

# Dear ImGui
add_library(Dear_ImGui
        ${LUMEN_GEN4_DEPS_DIR}/dear_imgui/imgui.cpp
        ${LUMEN_GEN4_DEPS_DIR}/dear_imgui/imgui_demo.cpp
        ${LUMEN_GEN4_DEPS_DIR}/dear_imgui/imgui_draw.cpp
        ${LUMEN_GEN4_DEPS_DIR}/dear_imgui/imgui_internal.h
        ${LUMEN_GEN4_DEPS_DIR}/dear_imgui/imgui_tables.cpp
        ${LUMEN_GEN4_DEPS_DIR}/dear_imgui/imgui_widgets.cpp
)
target_include_directories(Dear_ImGui INTERFACE ${LUMEN_GEN4_DEPS_DIR}/dear_imgui/)

# Tracy
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    option(TRACY_ENABLE "" OFF)
else()
    option(TRACY_ENABLE "" ON)
endif()
option(TRACY_NO_CRASH_HANDLER "" ON) # Otherwise texture cache exceptions will be treaten as a crash
option(TRACY_ON_DEMAND "" ON)
option(TRACY_NO_FRAME_IMAGE "" ON)
option(TRACY_FIBERS "" OFF) # For AmdGpu frontend profiling, disabled due to instability
option(TRACY_NO_SYSTEM_TRACING "" ON)
option(TRACY_NO_CALLSTACK "" ON)
option(TRACY_NO_CODE_TRANSFER "" ON)
option(TRACY_NO_SAMPLING "" ON)
option(TRACY_ONLY_LOCALHOST "" ON)
option(TRACY_NO_CONTEXT_SWITCH "" ON)
add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/tracy" "${CMAKE_CURRENT_BINARY_DIR}/tracy")

# pugixml
if (NOT TARGET pugixml::pugixml)
    option(PUGIXML_NO_EXCEPTIONS "" ON)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/pugixml" "${CMAKE_CURRENT_BINARY_DIR}/pugixml")
endif()

# libusb
if (NOT TARGET libusb::usb)
    if (${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD")
        # YOU MUST USE NATIVE LIBUSB
        # using anything else will crash instantly, also freebsd will NOT like it
        # no you cant vendor this libusb, its builtin on freebsd
        find_package(libusb)
    else()
        add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/libusb" "${CMAKE_CURRENT_BINARY_DIR}/libusb")
        add_library(libusb::usb ALIAS usb-1.0)
    endif()
endif()

# Discord RPC
if (ENABLE_DISCORD_RPC)
    add_subdirectory(discord-rpc)
endif()

# GCN Headers
add_subdirectory(gcn)

# stb
if (NOT TARGET stb::headers)
    add_library(stb INTERFACE)
    target_include_directories(stb INTERFACE stb)
    add_library(stb::headers ALIAS stb)
endif()

# hwinfo
set(HWINFO_STATIC ON)
add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/hwinfo" "${CMAKE_CURRENT_BINARY_DIR}/hwinfo")

if (APPLE OR ${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD")
    # date
    if (NOT TARGET date::date-tz)
        option(BUILD_TZ_LIB "" ON)
        option(USE_SYSTEM_TZ_DB "" ON)
        add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/date" "${CMAKE_CURRENT_BINARY_DIR}/date")
    endif()
    if (NOT TARGET epoll-shim)
        add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/epoll-shim" "${CMAKE_CURRENT_BINARY_DIR}/epoll-shim")
    endif()
endif()

#windows only
if (WIN32)
add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/ext-wepoll" "${CMAKE_CURRENT_BINARY_DIR}/ext-wepoll")
endif()

add_subdirectory(aacdec)

#nlohmann json 
if (NOT TARGET nlohmann_json::nlohmann_json)
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/json" "${CMAKE_CURRENT_BINARY_DIR}/json")
endif()

# miniz
if (NOT TARGET miniz::miniz)
add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/miniz" "${CMAKE_CURRENT_BINARY_DIR}/miniz")
add_library(miniz::miniz ALIAS miniz)
endif()

# cli11
if (NOT TARGET CLI11::CLI11)
    set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/CLI11" "${CMAKE_CURRENT_BINARY_DIR}/CLI11")
endif()

# miniupnpc
if (NOT TARGET miniupnpc::miniupnpc)
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/miniupnp/miniupnpc" "${CMAKE_CURRENT_BINARY_DIR}/miniupnpc")
endif()

# minimp3
add_library(minimp3 INTERFACE)
target_include_directories(minimp3 INTERFACE "${LUMEN_GEN4_DEPS_DIR}/minimp3")

# ImGuiFileDialog
add_library(ImGuiFileDialog ${LUMEN_GEN4_DEPS_DIR}/ImGuiFileDialog/ImGuiFileDialog.cpp)
target_compile_definitions(ImGuiFileDialog PRIVATE USE_STD_FILESYSTEM)
target_include_directories(ImGuiFileDialog INTERFACE ${LUMEN_GEN4_DEPS_DIR}/ImGuiFileDialog/)
target_link_libraries(ImGuiFileDialog PRIVATE Dear_ImGui)

#openal
if (NOT TARGET OpenAL::OpenAL)
set(ALSOFT_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
set(LIBTYPE "STATIC" CACHE STRING "" FORCE)
# Disable everything we don't need
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_CONFIG OFF CACHE BOOL "" FORCE)

# Backends (platform-specific)
if (WIN32)
    set(ALSOFT_BACKEND_WASAPI ON CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_DSOUND OFF CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_WINMM OFF CACHE BOOL "" FORCE)
elseif (APPLE)
    set(ALSOFT_BACKEND_COREAUDIO ON CACHE BOOL "" FORCE)
elseif(ANDROID)
    set(ALSOFT_BACKEND_OBOE OFF CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_OPENSL ON CACHE BOOL "" FORCE)
else()
    set(ALSOFT_BACKEND_ALSA ON CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_PULSEAUDIO ON CACHE BOOL "" FORCE)
    set(ALSOFT_BACKEND_PIPEWIRE OFF CACHE BOOL "" FORCE)
endif()
# Headless-safe
set(ALSOFT_BACKEND_NULL ON CACHE BOOL "" FORCE)

# Static build
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/openal-soft" "${CMAKE_CURRENT_BINARY_DIR}/openal-soft" EXCLUDE_FROM_ALL)
endif()

# LibreSSL
if (TARGET LibreSSL::SSL OR TARGET LibreSSL::Crypto)
    set(SSL_DEPENDENCIES LibreSSL::SSL LibreSSL::Crypto)
elseif (TARGET OpenSSL::SSL OR TARGET OpenSSL::Crypto)
    set(SSL_DEPENDENCIES OpenSSL::SSL OpenSSL::Crypto)
else()
    set(LIBRESSL_SKIP_INSTALL ON CACHE BOOL "" FORCE)
    set(LIBRESSL_APPS OFF CACHE BOOL "" FORCE)
    set(LIBRESSL_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${LUMEN_GEN4_DEPS_DIR}/libressl" "${CMAKE_CURRENT_BINARY_DIR}/libressl" EXCLUDE_FROM_ALL)
    get_directory_property(SSL_DEPENDENCIES DIRECTORY "${LUMEN_GEN4_DEPS_DIR}/libressl" DEFINITION OPENSSL_LIBS)
endif()

# cpp-httplib
add_library(Cpp_Httplib INTERFACE)
target_include_directories(Cpp_Httplib INTERFACE ${LUMEN_GEN4_DEPS_DIR}/cpp-httplib/)

if (TARGET ZLIB::ZLIB)
    target_link_libraries(Cpp_Httplib INTERFACE ZLIB::ZLIB)
    target_compile_definitions(Cpp_Httplib INTERFACE CPPHTTPLIB_ZLIB_SUPPORT)
    message(STATUS "cpp-httplib: zlib support enabled (gzip response decompression)")
else()
    message(STATUS "cpp-httplib: zlib not available — gzip responses will arrive uncompressed")
endif()

if (DEFINED SSL_DEPENDENCIES)
    target_link_libraries(Cpp_Httplib INTERFACE ${SSL_DEPENDENCIES})
    target_compile_definitions(Cpp_Httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
    message(STATUS "cpp-httplib: SSL support enabled (TLS connections)")
else()
    message(STATUS "cpp-httplib: OpenSSL/LibreSSL not available — libSceHttp HTTPS will fall back to mock at runtime.")
endif()

# macOS Vulkan driver
if (APPLE AND NOT ENABLE_SYSTEM_VULKAN AND NOT ENABLE_TESTS)
    add_subdirectory(vulkan-loader)
    set(VULKAN_LOADER_PATH "${CMAKE_CURRENT_BINARY_DIR}/vulkan-loader/loader/libvulkan.dylib" PARENT_SCOPE)

    add_subdirectory(mesa-kosmickrisp)
    set(VULKAN_DRIVER_PATH "${CMAKE_CURRENT_BINARY_DIR}/mesa-kosmickrisp/outputs/libvulkan_kosmickrisp.dylib" PARENT_SCOPE)
    set(VULKAN_ICD_PATH "${CMAKE_CURRENT_BINARY_DIR}/mesa-kosmickrisp/outputs/kosmickrisp_mesa_icd.json" PARENT_SCOPE)
endif()
