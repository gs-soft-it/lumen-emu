# Lumen modification, 2026-10-02: verify the reviewed ZArchive overlay, reuse
# audited FEX dependencies, and include pinned cpuinfo Darwin ARM sources.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(cpuinfo)

# The pinned cpuinfo build selects ARM Linux but omits its existing Darwin ARM
# implementation. Keep the pinned source inventory unchanged and compile the
# native implementation here; no x86 CPU topology is substituted on ARM.
if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
	(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")))
	target_sources(${KYTY_CPUINFO_TARGET} PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/cpuinfo/src/arm/mach/init.c"
		"${CMAKE_CURRENT_SOURCE_DIR}/cpuinfo/src/mach/topology.c")
endif()

# The Windows guest red-zone patcher needs a decoder/encoder and a small
# trampoline assembler. Pin the same revisions used by shadPS4's implementation.
include(FetchContent)

set(XBYAK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	xbyak
	GIT_REPOSITORY https://github.com/herumi/xbyak.git
	GIT_TAG 44a72f369268f7d552650891b296693e91db86bb
	GIT_SHALLOW FALSE
)

set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	zydis
	GIT_REPOSITORY https://github.com/zyantific/zydis.git
	GIT_TAG 120e0e705f8e3b507dc49377ac2879979f0d545c
	GIT_SUBMODULES_RECURSE TRUE
	GIT_SHALLOW FALSE
)

FetchContent_MakeAvailable(xbyak zydis)

# ZArchive provides random-access reads for compressed game dumps. Build only
# the reader/writer library and its zstd dependency; the command-line tool is
# not part of the Lumen runtime distribution.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	zstd
	GIT_REPOSITORY https://github.com/facebook/zstd.git
	GIT_TAG f8745da6ff1ad1e7bab384bd1f9d742439278e99
	GIT_SHALLOW FALSE
	SOURCE_SUBDIR build/cmake
)
FetchContent_Declare(
	zarchive_source
	GIT_REPOSITORY https://github.com/Exzap/ZArchive.git
	GIT_TAG d2c717730092c7bf8cbb033b12fd4001b7c4d932
	GIT_SHALLOW FALSE
	PATCH_COMMAND "${CMAKE_COMMAND}"
		"-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
		"-DSOURCE_DIR=<SOURCE_DIR>"
		"-DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/patches/zarchive-reader.patch"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/../../fex/ApplyZarchivePatch.cmake"
	SOURCE_SUBDIR kyty-library-only
)
FetchContent_MakeAvailable(zstd zarchive_source)

add_library(zarchive STATIC
	"${zarchive_source_SOURCE_DIR}/src/zarchivereader.cpp"
	"${zarchive_source_SOURCE_DIR}/src/zarchivewriter.cpp"
	"${zarchive_source_SOURCE_DIR}/src/sha_256.c"
)
add_library(ZArchive::zarchive ALIAS zarchive)
target_include_directories(zarchive SYSTEM PUBLIC "${zarchive_source_SOURCE_DIR}/include")
target_include_directories(zarchive PRIVATE "${zstd_SOURCE_DIR}/lib")
target_link_libraries(zarchive PRIVATE libzstd_static)
target_compile_features(zarchive PRIVATE cxx_std_20)
if(MSVC)
	target_compile_options(zarchive PRIVATE /W0)
else()
	target_compile_options(zarchive PRIVATE -w)
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
if(APPLE)
	# SDL's built-in iconv fallback does not support CP932.
	set(SDL_SYSTEM_ICONV ON CACHE BOOL "Use system iconv for CP932 conversion" FORCE)
endif()
if(ANDROID)
	# SDL2 and SDL3 use the same Java class names upstream. Keep the pinned
	# source unchanged and namespace only its generated Android JNI bindings.
	set(KYTY_SDL3_ANDROID_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/sdl3-android")
	file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/SDL3/" DESTINATION "${KYTY_SDL3_ANDROID_SOURCE}")
	foreach(binding IN ITEMS
		src/core/android/SDL_android.c
		src/hidapi/android/hid.cpp
		src/video/android/SDL_androidevents.c)
		file(READ "${CMAKE_CURRENT_SOURCE_DIR}/SDL3/${binding}" binding_text)
		string(REPLACE "org_libsdl_app" "org_libsdl3_app" binding_text "${binding_text}")
		string(REPLACE "org/libsdl/app" "org/libsdl3/app" binding_text "${binding_text}")
		file(WRITE "${KYTY_SDL3_ANDROID_SOURCE}/${binding}" "${binding_text}")
		set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/SDL3/${binding}")
	endforeach()
	include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ApplyAndroidSdl3WaitPatch.cmake")
	lumen_ps5_patch_android_sdl_wait(
		"${CMAKE_CURRENT_SOURCE_DIR}/SDL3" "${KYTY_SDL3_ANDROID_SOURCE}"
		"${CMAKE_CURRENT_SOURCE_DIR}/../../UPSTREAM.json")
	if(LUMEN_PS5_SDL3_VULKAN_ADAPTER_SCRIPT)
		include("${LUMEN_PS5_SDL3_VULKAN_ADAPTER_SCRIPT}")
	endif()
	add_subdirectory("${KYTY_SDL3_ANDROID_SOURCE}" SDL3)
else()
	add_subdirectory(SDL3)
endif()

add_library(Vulkan-Headers INTERFACE)
add_library(Vulkan::Headers ALIAS Vulkan-Headers)
target_include_directories(Vulkan-Headers INTERFACE
	"${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers/include"
)

add_library(imgui STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/../../../../shared/ime/KeyboardPanel.cpp"
	imgui/imgui.cpp
	imgui/imgui_draw.cpp
	imgui/imgui_tables.cpp
	imgui/imgui_widgets.cpp
	imgui/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/imgui"
	"${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../../../shared"
)
target_compile_definitions(imgui PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_link_libraries(imgui PUBLIC Vulkan::Headers)
if(MSVC)
	target_compile_options(imgui PRIVATE /W0)
else()
	target_compile_options(imgui PRIVATE -w)
endif()

set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SPIRV-Headers_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SPIRV-Headers" CACHE PATH "" FORCE)
add_subdirectory(SPIRV-Tools)

add_library(kyty_spirv_tools ALIAS SPIRV-Tools-static)
add_library(kyty_spirv_tools_opt ALIAS SPIRV-Tools-opt)

if(NOT TARGET xxhash)
	add_subdirectory(xxHash/build/cmake)
endif()

set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_MODULE OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
if(NOT TARGET fmt::fmt)
	add_subdirectory(fmt EXCLUDE_FROM_ALL)
endif()

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
add_subdirectory(nlohmann_json EXCLUDE_FROM_ALL)

set(SPDLOG_BUILD_ALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_WARNINGS OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
if(WIN32)
	set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(spdlog EXCLUDE_FROM_ALL)

if (NOT TARGET FFmpeg::ffmpeg)
	# On macOS the target arch is driven by CMAKE_OSX_ARCHITECTURES, not the host
	# CMAKE_SYSTEM_PROCESSOR (which reports arm64 even for an x86_64/Rosetta build).
	if(APPLE AND CMAKE_OSX_ARCHITECTURES)
		if(CMAKE_OSX_ARCHITECTURES MATCHES "^(x86_64)$")
			set(ARCHITECTURE x86_64)
		elseif(CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64)$")
			set(ARCHITECTURE arm64)
		else()
			set(ARCHITECTURE x86_64 arm64) # universal
		endif()
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
		set(ARCHITECTURE arm64)
	else()
		set(ARCHITECTURE x86_64)
	endif()
	add_subdirectory(ffmpeg-core)
	add_library(FFmpeg::ffmpeg ALIAS ffmpeg)
endif()

file(GLOB LIBATRAC9_SOURCES LibAtrac9/C/src/*.c)
add_library(LibAtrac9 STATIC ${LIBATRAC9_SOURCES})
target_include_directories(LibAtrac9 PUBLIC LibAtrac9/C/src)
if(MSVC)
	target_compile_options(LibAtrac9 PRIVATE /W0)
endif()

set(TRACY_STATIC ON CACHE BOOL "" FORCE)
# Android product builds omit profiler instrumentation, including when a
# previously configured cache enabled it. Keep laboratory and desktop defaults.
if (ANDROID AND LUMEN_PS5_DIRECT_FEX AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)
else()
	set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
endif()
set(TRACY_NO_CRASH_HANDLER ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_DELAYED_INIT ON CACHE BOOL "" FORCE)
set(TRACY_MANUAL_LIFETIME ON CACHE BOOL "" FORCE)
add_subdirectory(tracy EXCLUDE_FROM_ALL)
if (TARGET TracyClient)
	if (MSVC)
		target_compile_options(TracyClient PRIVATE /WX-)
	endif()
	if (CLANG)
		target_compile_options(TracyClient PRIVATE
			-Wno-error
			-Wno-braced-scalar-init
			-Wno-format
			-Wno-unused-variable
		)
	endif()
endif()

if (CLANG AND MSVC)
add_subdirectory(winpthread)
endif()
