# Core-only source export: application UI targets removed on 2026-10-23.
cmake_minimum_required(VERSION 3.17)

# This internal Ninja switch is consumed while languages are enabled by the
# first project() call. Apple's ar does not expand @response-file arguments,
# so setting it later in config_compiler_and_linker() is too late. The same
# wrapper is required by both macOS and iOS toolchains.
if((APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android" AND
    NOT CMAKE_TOOLCHAIN_FILE MATCHES "android\\.toolchain\\.cmake$") OR
   CMAKE_SYSTEM_NAME STREQUAL "iOS")
	# CMake/Ninja may use @response files for large static libraries, while
	# Apple's /usr/bin/ar treats that syntax as a literal filename.
	set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/tools/macos-ar.sh" CACHE FILEPATH "Archiver wrapper for macOS" FORCE)
endif()

project(LumenEmu)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(LUMEN_GEN4_FULL_BUILD_DIR "${CMAKE_BINARY_DIR}/gen4" CACHE PATH "Independent PS4 build directory")
    set(LUMEN_GEN4_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../lumen-deps/gen4/sources" CACHE PATH "Pinned PS4 dependencies")
    set(LUMEN_GEN4_FFMPEG_PREBUILT_DIR "" CACHE PATH "Native FFmpeg package for PS4")
    set(LUMEN_GEN4_BUILD_JOBS "4" CACHE STRING "Parallel PS4 build jobs")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
	set(ANDROID TRUE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
	set(IOS TRUE)
elseif(CMAKE_SYSTEM_NAME MATCHES ".*Linux")
	set(LINUX TRUE)

# CMake defines APPLE for every Darwin target. Keep macOS and iOS distinct so
# desktop-only dependencies are never selected for an iOS build.
elseif(APPLE)
	set(MACOS TRUE)
endif()

if(MACOS OR IOS)
	set(APPLE_PLATFORM TRUE)
endif()

if (NOT ((WIN32 AND (MINGW OR MSVC)) OR ANDROID OR LINUX OR MACOS OR IOS))
	message(FATAL_ERROR "only mingw, msvc, Android, Linux, macOS and iOS are supported")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(utils.cmake)
include(legal_notices.cmake)

	remove_generator_macros(CXX __cplusplus)
	remove_generator_macros(CXX __clang_version__)
	remove_generator_macros(C __clang_version__)
	remove_generator_macros(CXX __VERSION__)
	remove_generator_macros(C __VERSION__)
	
	add_generator_macros(CXX __cplusplus "201703L")

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

include(TestBigEndian)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(LUMEN_BITNESS 64)
else()
	set(LUMEN_BITNESS 32)
endif()

test_big_endian(BIG_ENDIAN)
if(BIG_ENDIAN)
	set(LUMEN_ENDIAN LUMEN_ENDIAN_BIG)
else()
	set(LUMEN_ENDIAN LUMEN_ENDIAN_LITTLE)
endif()

if(CMAKE_BUILD_TYPE MATCHES Debug)
	set(LUMEN_BUILD LUMEN_BUILD_DEBUG)
else()
	set(LUMEN_BUILD LUMEN_BUILD_RELEASE)
endif()

if(ANDROID)
	set(LUMEN_PLATFORM LUMEN_PLATFORM_ANDROID)
elseif(LINUX)
	set(LUMEN_PLATFORM LUMEN_PLATFORM_LINUX)
elseif(IOS)
	set(LUMEN_PLATFORM LUMEN_PLATFORM_IOS)
elseif(MACOS)
	set(LUMEN_PLATFORM LUMEN_PLATFORM_OSX)
else()
	set(LUMEN_PLATFORM LUMEN_PLATFORM_WINDOWS)
endif()

if(ANDROID OR LINUX)
	if(CMAKE_CXX_COMPILER_ID MATCHES "(C|c?)lang")
		set(CLANG 1)
		set(LUMEN_COMPILER CLANG)
		set(LUMEN_LINKER LLD)
	else()
		set(LUMEN_COMPILER GCC)
		set(LUMEN_LINKER LD)
	endif()
elseif(MINGW)
	if(CMAKE_CXX_COMPILER_ID MATCHES "(C|c?)lang")
		set(CLANG 1)
		set(LUMEN_COMPILER CLANG)
		set(LUMEN_LINKER LLD)
		if ((CMAKE_AR MATCHES "llvm") AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0.0) AND (CMAKE_CXX_COMPILER MATCHES "winlibs"))
			string(REPLACE "llvm-ar.exe" "ar.exe" CMAKE_AR ${CMAKE_AR}) # llvm-ar.exe 11.0.0 from winlibs is broken
		endif()
	else()
		set(LUMEN_COMPILER MINGW)
		set(LUMEN_LINKER LD)
	endif()
elseif(APPLE_PLATFORM)
	set(CLANG 1)
	set(LUMEN_COMPILER CLANG)
	set(LUMEN_LINKER LD)
else()
	if(CMAKE_CXX_COMPILER_ID MATCHES "(C|c?)lang")
		set(CLANG 1)
		set(LUMEN_COMPILER CLANG)
		set(LUMEN_LINKER LLD_LINK)
	else()
		set(LUMEN_COMPILER MSVC)
		set(LUMEN_LINKER LINK)
	endif()
endif()

set(LUMEN_PROJECT_NAME "Emulator" CACHE STRING "Project name")
set(LUMEN_MACOS_X64_BACKEND "auto" CACHE STRING
	"macOS x64 guest backend (auto or fex on Apple Silicon; noda is legacy-only)")
set_property(CACHE LUMEN_MACOS_X64_BACKEND PROPERTY STRINGS auto fex)
set(LUMEN_MACOS_FEXCORE_SOURCE_DIR "" CACHE PATH
	"Prepared external FEX checkout used by the macOS FEX backend")
if(NOT LUMEN_MACOS_X64_BACKEND MATCHES "^(auto|noda|fex)$")
	message(FATAL_ERROR
		"Unsupported LUMEN_MACOS_X64_BACKEND='${LUMEN_MACOS_X64_BACKEND}'; use auto, noda or fex")
endif()
set(LUMEN_MACOS_X64_BACKEND_REQUESTED "${LUMEN_MACOS_X64_BACKEND}")
set(LUMEN_MACOS_X64_BACKEND_ARCHITECTURE "${CMAKE_OSX_ARCHITECTURES}")
if(MACOS AND LUMEN_MACOS_X64_BACKEND_ARCHITECTURE STREQUAL "")
	set(LUMEN_MACOS_X64_BACKEND_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(LUMEN_MACOS_X64_BACKEND_REQUESTED STREQUAL "auto")
	set(LUMEN_MACOS_X64_BACKEND "noda")
	if(MACOS AND NOT IOS AND APPLE)
		set(LUMEN_MACOS_X64_BACKEND "fex")
	endif()
endif()
if(MACOS AND NOT LUMEN_MACOS_X64_BACKEND STREQUAL "fex")
	message(FATAL_ERROR
		"macOS Apple Silicon requires FEX in every build configuration; NODA is not a supported backend")
endif()
message(STATUS
	"macOS x64 backend: requested=${LUMEN_MACOS_X64_BACKEND_REQUESTED} effective=${LUMEN_MACOS_X64_BACKEND}")
set(LUMEN_MACOS_FEXCORE_RUNTIME OFF)
if(LUMEN_MACOS_X64_BACKEND STREQUAL "fex")
	if(NOT MACOS OR IOS OR NOT APPLE)
		message(FATAL_ERROR
			"LUMEN_MACOS_X64_BACKEND=fex is available only on macOS desktop")
	endif()
	if(NOT LUMEN_MACOS_X64_BACKEND_ARCHITECTURE STREQUAL "arm64")
		message(FATAL_ERROR
			"LUMEN_MACOS_X64_BACKEND=fex requires a thin arm64 build")
	endif()
	if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release|RelWithDebInfo|MinSizeRel)$" OR
	   CMAKE_CONFIGURATION_TYPES)
		message(FATAL_ERROR
			"LUMEN_MACOS_X64_BACKEND=fex requires a single Debug, Release, RelWithDebInfo or MinSizeRel build")
	endif()
	if(LUMEN_MACOS_FEXCORE_SOURCE_DIR STREQUAL "")
		execute_process(
			COMMAND bash "${CMAKE_CURRENT_LIST_DIR}/../_Build/prepare_Bundled_FEXCore.sh" macos
			RESULT_VARIABLE LUMEN_BUNDLED_FEX_PREPARE_RESULT
			OUTPUT_VARIABLE LUMEN_BUNDLED_FEX_PREPARED_SOURCE
			OUTPUT_STRIP_TRAILING_WHITESPACE)
		if(NOT LUMEN_BUNDLED_FEX_PREPARE_RESULT EQUAL 0 OR
		   NOT IS_DIRECTORY "${LUMEN_BUNDLED_FEX_PREPARED_SOURCE}")
			message(FATAL_ERROR "Could not prepare the bundled FEX source for macOS")
		endif()
		set(LUMEN_MACOS_FEXCORE_SOURCE_DIR "${LUMEN_BUNDLED_FEX_PREPARED_SOURCE}"
			CACHE PATH "Prepared FEX checkout used by the macOS FEX backend" FORCE)
	endif()
	set(LUMEN_MACOS_FEXCORE_RUNTIME ON)
endif()
# This source distribution contains emulator cores and native runtime adapters.
# The separate application shells and their model tests are not included.
option(LUMEN_WITH_DESKTOP_FRONTEND "Unavailable in the core source distribution" OFF)
option(LUMEN_WITH_FRONTEND_MODEL_TEST "Unavailable in the core source distribution" OFF)
if(LUMEN_WITH_DESKTOP_FRONTEND OR LUMEN_WITH_FRONTEND_MODEL_TEST)
	message(FATAL_ERROR "Desktop frontends and frontend model tests are not included in this core source distribution")
endif()
option(LUMEN_WITH_GRAPHICS_SMOKE_TEST "Build the SDL/Vulkan graphics smoke test" OFF)
option(LUMEN_WITH_CPU_X64_TEST "Build the standalone x64 interpreter test" OFF)
option(LUMEN_WITH_CPU_A64_DYNAREC_TEST "Build the standalone AArch64 dynarec foundation and lowering test" OFF)
if(LUMEN_MACOS_FEXCORE_RUNTIME AND
   (LUMEN_WITH_CPU_X64_TEST OR LUMEN_WITH_CPU_A64_DYNAREC_TEST))
	message(FATAL_ERROR
		"The macOS FEX product cannot build standalone interpreter/NODA execution tests; use the FEX runtime tests")
endif()
option(LUMEN_FAST_BLOCK_TELEMETRY "Compile bounded no-JIT FastBlock diagnostic telemetry" OFF)
option(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2
	"Compile the default-off transactional scalar Region Engine v2 proof"
	OFF)
option(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT
	"Compile bounded Region Engine v2 functional-smoke receipts"
	OFF)
if(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT AND
   NOT LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2)
	message(FATAL_ERROR
		"Region Engine v2 receipts require LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2=ON")
endif()
if(ANDROID AND LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2)
	message(FATAL_ERROR
		"Region Engine v2 Android requires a VM code-read lease before it can be compiled")
endif()
option(LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY
	"Compile and default-enable the existing FastBlock dense directory on Apple ARM64"
	OFF)
if(LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY AND NOT APPLE_PLATFORM)
	message(FATAL_ERROR
		"LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY is available only for Apple platform builds")
endif()
option(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY
	"Compile bounded AArch64 dynarec startup diagnostic telemetry"
	OFF)
option(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY
	"Compile sampled AArch64 dynarec phase-6AF dispatch telemetry"
	OFF)
option(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY
	"Compile bounded AArch64 dynarec phase-6AL archive diagnostic telemetry"
	OFF)
option(LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY
	"Compile bounded AArch64 dynarec cross-owner compile diagnostic telemetry"
	OFF)
option(LUMEN_A64_DYNAREC_RFLAGS_PROFILE
	"Compile sampled AArch64 dynarec direct-block RFLAGS opportunity telemetry"
	OFF)
option(LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE
	"Compile sampled AArch64 dynarec direct-block GPR-cache opportunity telemetry"
	OFF)
option(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE
	"Compile scene-anchored production-shape AArch64 dense-resident telemetry"
	OFF)
option(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE
	"Compile scene-anchored sampled AArch64 ordinary-body diagnostic telemetry"
	OFF)
option(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE
	"Compile scene-anchored AArch64 operation-snapshot opportunity telemetry"
	OFF)
set(LUMEN_A64_PROMOTED_LOADING_DEFAULT OFF)
set(LUMEN_A64_PROMOTED_LOADING_ARCH "${CMAKE_OSX_ARCHITECTURES}")
if(NOT LUMEN_A64_PROMOTED_LOADING_ARCH)
	set(LUMEN_A64_PROMOTED_LOADING_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(MACOS AND NOT LUMEN_MACOS_X64_BACKEND STREQUAL "fex" AND
   CMAKE_BUILD_TYPE STREQUAL "Release" AND
   LUMEN_A64_PROMOTED_LOADING_ARCH STREQUAL "arm64")
	# ALC g3 is the promoted Apple Silicon production baseline.  Keep a clean
	# native Release configure aligned with the promoted cached build instead of
	# silently falling back to the slower pre-ALC layout.
	set(LUMEN_A64_PROMOTED_LOADING_DEFAULT ON)
endif()
option(LUMEN_A64_REJECTED_FASTBLOCK_PURGE
	"Remove rejected UFSB/IMSE/FCFOS paths from the macOS ARM64 Release binary"
	${LUMEN_A64_PROMOTED_LOADING_DEFAULT})
option(LUMEN_A64_ARM_HOTPATH_COMPOSITE
	"Add compact FastBlock snapshots and omit CPU frame pointers on top of the purge"
	${LUMEN_A64_PROMOTED_LOADING_DEFAULT})
option(LUMEN_A64_ARM_LOADING_COMPOSITE
	"Use the retained 6DA matcher inside the ARM hot-path composite"
	${LUMEN_A64_PROMOTED_LOADING_DEFAULT})
if(LUMEN_A64_ARM_HOTPATH_COMPOSITE AND
   NOT LUMEN_A64_REJECTED_FASTBLOCK_PURGE)
	message(FATAL_ERROR
		"LUMEN_A64_ARM_HOTPATH_COMPOSITE requires LUMEN_A64_REJECTED_FASTBLOCK_PURGE")
endif()
if(LUMEN_A64_ARM_LOADING_COMPOSITE AND
   NOT LUMEN_A64_ARM_HOTPATH_COMPOSITE)
	message(FATAL_ERROR
		"LUMEN_A64_ARM_LOADING_COMPOSITE requires LUMEN_A64_ARM_HOTPATH_COMPOSITE")
endif()
if(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
	if(NOT MACOS)
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY is available only for macOS desktop builds")
	endif()
	set(LUMEN_PHASE6AL_TELEMETRY_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_PHASE6AL_TELEMETRY_ARCH)
		set(LUMEN_PHASE6AL_TELEMETRY_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_PHASE6AL_TELEMETRY_ARCH STREQUAL "arm64")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY requires a thin arm64 macOS build")
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY OR
	   LUMEN_A64_DYNAREC_STARTUP_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY OR
	   LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY cannot be combined with FastBlock, startup, phase-6AF or cross-owner telemetry")
	endif()
endif()
if(LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY)
	if(NOT MACOS)
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY is available only for macOS desktop builds")
	endif()
	set(LUMEN_CROSS_OWNER_TELEMETRY_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_CROSS_OWNER_TELEMETRY_ARCH)
		set(LUMEN_CROSS_OWNER_TELEMETRY_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_CROSS_OWNER_TELEMETRY_ARCH STREQUAL "arm64")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY requires a thin arm64 macOS build")
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY OR
	   LUMEN_A64_DYNAREC_STARTUP_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY OR
	   LUMEN_A64_DYNAREC_TELEMETRY OR
	   LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY cannot be combined with FastBlock, generic dynarec, startup, phase-6AF, phase-6AL or host-PC telemetry")
	endif()
endif()
if(LUMEN_A64_DYNAREC_RFLAGS_PROFILE)
	if(NOT MACOS)
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_RFLAGS_PROFILE is available only for macOS desktop builds")
	endif()
	set(LUMEN_RFLAGS_PROFILE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_RFLAGS_PROFILE_ARCH)
		set(LUMEN_RFLAGS_PROFILE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_RFLAGS_PROFILE_ARCH STREQUAL "arm64")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_RFLAGS_PROFILE requires a thin arm64 macOS build")
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY OR
	   LUMEN_A64_DYNAREC_STARTUP_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY OR
	   LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY OR
	   LUMEN_A64_DYNAREC_TELEMETRY OR
	   LUMEN_A64_DYNAREC_HOST_PC_PROFILE OR
	   LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_RFLAGS_PROFILE cannot be combined with other dynarec, FastBlock or host-PC telemetry")
	endif()
endif()
if(LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
	if(NOT MACOS)
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE is available only for macOS desktop builds")
	endif()
	set(LUMEN_GPR_CACHE_PROFILE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_GPR_CACHE_PROFILE_ARCH)
		set(LUMEN_GPR_CACHE_PROFILE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_GPR_CACHE_PROFILE_ARCH STREQUAL "arm64")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE requires a thin arm64 macOS build")
	endif()
	if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE requires a Release build")
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY OR
	   LUMEN_A64_DYNAREC_STARTUP_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY OR
	   LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY OR
	   LUMEN_A64_DYNAREC_TELEMETRY OR
	   LUMEN_A64_DYNAREC_HOST_PC_PROFILE OR
	   LUMEN_A64_DYNAREC_RFLAGS_PROFILE)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE cannot be combined with other dynarec, FastBlock or host-PC telemetry")
	endif()
endif()
if(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	if(NOT MACOS)
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CK_PROFILE is available only for macOS desktop builds")
	endif()
	set(LUMEN_DENSE_SCENE_PROFILE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_DENSE_SCENE_PROFILE_ARCH)
		set(LUMEN_DENSE_SCENE_PROFILE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_DENSE_SCENE_PROFILE_ARCH STREQUAL "arm64")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CK_PROFILE requires a thin arm64 macOS build")
	endif()
	if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CK_PROFILE requires a Release build")
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY OR
	   LUMEN_A64_DYNAREC_STARTUP_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY OR
	   LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY OR
	   LUMEN_A64_DYNAREC_TELEMETRY OR
	   LUMEN_A64_DYNAREC_HOST_PC_PROFILE OR
	   LUMEN_A64_DYNAREC_RFLAGS_PROFILE OR
	   LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CM_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_PHASE6CK_PROFILE cannot be combined with other dynarec, FastBlock or host-PC telemetry")
	endif()
endif()
option(LUMEN_A64_DYNAREC_HOST_PC_PROFILE
	"Compile macOS ARM64 AArch64 dynarec host-PC profiling boundaries"
	OFF)
if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
	if(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY cannot be combined with the host-PC profile")
	endif()
	if(LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY cannot be combined with the host-PC profile")
	endif()
	if(NOT MACOS)
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_HOST_PC_PROFILE is available only for macOS desktop builds")
	endif()
	set(LUMEN_HOST_PC_PROFILE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_HOST_PC_PROFILE_ARCH)
		set(LUMEN_HOST_PC_PROFILE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_HOST_PC_PROFILE_ARCH STREQUAL "arm64")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_HOST_PC_PROFILE requires a thin arm64 macOS build")
	endif()
endif()
if(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
	if(NOT MACOS)
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CM_PROFILE is available only for macOS desktop builds")
	endif()
	set(LUMEN_PHASE6CM_PROFILE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_PHASE6CM_PROFILE_ARCH)
		set(LUMEN_PHASE6CM_PROFILE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_PHASE6CM_PROFILE_ARCH STREQUAL "arm64")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CM_PROFILE requires a thin arm64 macOS build")
	endif()
	if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CM_PROFILE requires a Release build")
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY OR
	   LUMEN_A64_DYNAREC_STARTUP_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY OR
	   LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY OR
	   LUMEN_A64_DYNAREC_TELEMETRY OR
	   LUMEN_A64_DYNAREC_HOST_PC_PROFILE OR
	   LUMEN_A64_DYNAREC_RFLAGS_PROFILE OR
	   LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CK_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CO_PROFILE OR
	   LUMEN_ENABLE_EASY_PROFILER)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_PHASE6CM_PROFILE cannot be combined with other telemetry or profiling instrumentation")
	endif()
endif()
if(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
	if(NOT MACOS)
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CO_PROFILE is available only for macOS desktop builds")
	endif()
	set(LUMEN_PHASE6CO_PROFILE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_PHASE6CO_PROFILE_ARCH)
		set(LUMEN_PHASE6CO_PROFILE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_PHASE6CO_PROFILE_ARCH STREQUAL "arm64")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CO_PROFILE requires a thin arm64 macOS build")
	endif()
	if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
		message(FATAL_ERROR "LUMEN_A64_DYNAREC_PHASE6CO_PROFILE requires a Release build")
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY OR
	   LUMEN_A64_DYNAREC_STARTUP_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY OR
	   LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY OR
	   LUMEN_A64_DYNAREC_TELEMETRY OR
	   LUMEN_A64_DYNAREC_HOST_PC_PROFILE OR
	   LUMEN_A64_DYNAREC_RFLAGS_PROFILE OR
	   LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CK_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CM_PROFILE OR
	   LUMEN_ENABLE_EASY_PROFILER)
		message(FATAL_ERROR
			"LUMEN_A64_DYNAREC_PHASE6CO_PROFILE cannot be combined with other telemetry or profiling instrumentation")
	endif()
endif()
option(LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK
	"Execute VEX VMOVD xmm,r32 in the ordinary Apple ARM64 FastBlock tier"
	OFF)
option(LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK
	"Execute register-only VEX.128 dword-immediate permutes in the ordinary Apple ARM64 FastBlock tier"
	OFF)
option(LUMEN_WITH_AUDIO_OUT_TEST "Build the standalone silent AudioOut backend test" OFF)
option(LUMEN_WITH_AVPLAYER_MAC_TEST "Build the standalone macOS AvPlayer audio/video test" OFF)
option(LUMEN_WITH_AVPLAYER_ANDROID_LAYOUT_TEST "Build the standalone Android AvPlayer NV12 layout test" OFF)
option(LUMEN_WITH_AVPLAYER_LIFETIME_TEST "Build the standalone AvPlayer concurrent lifetime test" OFF)
option(LUMEN_AV_SYNC_TELEMETRY
	"Compile bounded Apple AvPlayer PTS/read telemetry (without AudioOut token linkage)"
	OFF)
option(LUMEN_WITH_GRAPHICS_STATE_TEST "Build the standalone graphics command-state synchronization test" OFF)
option(LUMEN_WITH_GRAPHICS_HOST_POLLING_TEST "Build the graphics host polling sleep and handle lifecycle test" OFF)
option(LUMEN_WITH_RENDER_TARGET_FORMAT_TEST "Build the standalone render-target format and capability test" OFF)
option(LUMEN_WITH_VMAC_LEGACY_F32_VULKAN_PROBE
	"Build the opt-in desktop Vulkan backend probe for V_MAC_LEGACY_F32"
	OFF)
option(LUMEN_WITH_VRSQ_CLAMP_F32_VULKAN_PROBE
	"Build the opt-in desktop Vulkan backend probe for V_RSQ_CLAMP_F32"
	OFF)
option(LUMEN_WITH_VCVT_OFF_F32_I4_VULKAN_PROBE
	"Build the opt-in desktop arithmetic backend probe for the V_CVT_OFF_F32_I4 known-constant profile"
	OFF)
option(LUMEN_WITH_RUNTIME_LINKER_ADDRESS_TEST "Build the standalone Apple ARM64 RuntimeLinker address test" OFF)
option(LUMEN_WITH_MODULE_LOAD_DEDUPE_TEST "Build the standalone module-load dedupe contract test" OFF)
option(LUMEN_WITH_RUNTIME_LIFECYCLE_TEST "Build the repeatable runtime lifecycle foundation test" OFF)
option(LUMEN_WITH_PTHREAD_MUTEX_TEST "Build the standalone desktop pthread mutex contract test" OFF)
option(LUMEN_WITH_PTHREAD_CONDITION_TEST "Build the standalone desktop pthread condition lifecycle test" OFF)
option(LUMEN_WITH_PTHREAD_ONCE_TEST "Build the standalone desktop pthread once contract test" OFF)
option(LUMEN_WITH_PTHREAD_EXIT_TEST "Build the macOS FEX guest pthread exit test" OFF)
option(LUMEN_WITH_PTHREAD_EXCEPTION_TEST "Build the macOS FEX guest exception delivery test" OFF)
option(LUMEN_WITH_VIDEOOUT_GAMMA_TEST "Build the macOS VideoOut gamma settings test" OFF)
option(LUMEN_WITH_VIDEOOUT_EVENT_DATA_TEST "Build the VideoOut event-data model contract test" OFF)
option(LUMEN_WITH_HOST_CALL_SCALAR_TEST "Build the macOS FEX scalar host-call test" OFF)
option(LUMEN_WITH_PTHREAD_AFFINITY_TEST "Build the standalone desktop pthread logical-affinity contract test" OFF)
option(LUMEN_WITH_LOG_FILE_SHUTDOWN_TEST "Build the standalone log file shutdown concurrency test" OFF)
option(LUMEN_WITH_KERNEL_USER_EVENT_TEST "Build the standalone kernel user-event queue test" OFF)
option(LUMEN_WITH_KERNEL_EVENT_FLAG_TEST "Build the standalone kernel event-flag export, attribute, clear and deletion test" OFF)
option(LUMEN_WITH_POSIX_SEMAPHORE_TEST "Build the macOS ARM64 POSIX semaphore test" OFF)
option(LUMEN_WITH_PNG_DEC_TEST "Build the standalone PNG decoder API test" OFF)
option(LUMEN_WITH_PNG_ENC_TEST "Build the standalone PNG encoder API test" OFF)
option(LUMEN_WITH_REMOTEPLAY_TEST "Build the opt-in macOS Remoteplay offline model test" OFF)
option(LUMEN_WITH_NP_PLUS_TEST "Build the opt-in macOS NP Plus offline model test" OFF)
option(LUMEN_WITH_NP_PARENTAL_TEST "Build the opt-in macOS NP parental offline model test" OFF)
option(LUMEN_WITH_NP_IN_GAME_MESSAGE_TEST "Build the opt-in macOS NP in-game messaging model test" OFF)
option(LUMEN_WITH_NP_CMP_ABORT_TEST "Build the opt-in shared NP comparison and abort test" OFF)
option(LUMEN_WITH_NETCTL_STATE_TEST "Build the opt-in shared NetCtl offline state test" OFF)
option(LUMEN_WITH_NET_OFFLINE_SURFACE_TEST "Build the shared libSceNet offline surface test" OFF)
option(LUMEN_WITH_APP_CONTENT_TEMPORARY_DATA_TEST
	"Build the macOS FEX AppContent temporary-data mount test" OFF)
option(LUMEN_WITH_NP_TROPHY_BINDING_TEST "Build the opt-in desktop NP Trophy binding test" OFF)
option(LUMEN_WITH_RTC_TEST "Build the shared RTC local clock contract test" OFF)
option(LUMEN_WITH_ERROR_DIALOG_TEST "Build the shared inactive ErrorDialog polling test" OFF)
option(LUMEN_WITH_NP_COMMERCE_DIALOG_TEST
	"Build the opt-in macOS Apple Silicon NpCommerce local-absence dialog test"
	OFF)
option(LUMEN_WITH_SAVE_DATA_DIR_NAME_SEARCH_TEST
	"Build the opt-in isolated SaveData directory search and SetParam validation test"
	OFF)
option(LUMEN_WITH_GNM_SET_CS_SHADER_TEST "Build the opt-in shared Gnm compute-shader binding test" OFF)
option(LUMEN_WITH_GNM_EMBEDDED_PS_SHADER_TEST "Build the opt-in shared Gnm embedded pixel-shader binding test" OFF)
option(LUMEN_WITH_GNM_MARKER_TEST "Build the shared Gnm InsertSetMarker packet contract test" OFF)
option(LUMEN_WITH_GNM_VGT_CONTROL_TEST "Build the shared Gnm SetVgtControl packet contract test" OFF)
option(LUMEN_WITH_GNM_WORKLOAD_STREAM_TEST "Build the shared Gnm workload API contract test" OFF)
option(LUMEN_WITH_GNM_EQ_EVENT_TEST "Build the graphics event registration and producer lifetime test" OFF)
option(LUMEN_WITH_HLE_IMPLEMENTED_EXPORTS_TEST "Build the shared implemented-export registration test" OFF)
option(LUMEN_WITH_RUDP_TEST "Build the opt-in macOS RUDP administrative model test" OFF)
option(LUMEN_WITH_VOICE_TEST "Build the opt-in macOS Voice administrative model test" OFF)
option(LUMEN_WITH_ULT_INITIALIZE_TEST "Build the opt-in macOS ULT Initialize local-profile test" OFF)
option(LUMEN_WITH_MOVE_INITIALIZE_TEST
	"Build the opt-in macOS FEX Move Initialize no-device profile test"
	OFF)
option(LUMEN_WITH_MOUSE_INITIALIZE_TEST
	"Build the opt-in macOS FEX Mouse Initialize no-device profile test"
	OFF)
option(LUMEN_WITH_IME_KEYBOARD_TEST
	"Build the opt-in macOS FEX Ime keyboard no-device profile test"
	OFF)
option(LUMEN_WITH_IME_DIALOG_TEST "Build the checked PS4 text dialog contract test" OFF)
option(LUMEN_WITH_SIMPLE_ARRAY_SORT_TEST "Build the core array sorting contract test" OFF)
option(LUMEN_WITH_TILE_TEST "Build the standalone texture tiling size test" OFF)
option(LUMEN_WITH_GUEST_FILE_TEST "Build the standalone guest file read test" OFF)
option(LUMEN_WITH_FILESYSTEM_PATH_TEST "Build the standalone mounted-path translation test" OFF)
option(LUMEN_WITH_AJM_TEST "Build the standalone AJM batch/runtime/ATRAC9 test" OFF)
option(LUMEN_WITH_VIRTUAL_MEMORY_TEST "Build the standalone platform virtual-memory test" OFF)
option(LUMEN_WITH_MFI_DIGITAL_QUEUE_TEST "Build the standalone SDL MFi digital-input queue test" OFF)
option(LUMEN_WITH_CONTROLLER_INPUT_TEST "Build the standalone guest digital-input history test" OFF)
option(LUMEN_WITH_PAD_HANDLE_TEST "Build the opt-in shared Pad handle-validation test" OFF)
option(LUMEN_WITH_PAD_ANALOG_INPUT_TEST "Build the opt-in shared Pad analog-input test" OFF)
option(LUMEN_WITH_APPLE_VALIDATION "Require and expose the complete native Apple validation gate" OFF)
option(LUMEN_WITH_IOS_RUNTIME "Link the complete in-process emulator runtime into the UIKit application" OFF)

if(LUMEN_AV_SYNC_TELEMETRY)
	if(NOT APPLE_PLATFORM)
		message(FATAL_ERROR "LUMEN_AV_SYNC_TELEMETRY is available only for Apple platform builds")
	endif()
	if(IOS AND NOT LUMEN_WITH_IOS_RUNTIME)
		message(FATAL_ERROR
			"LUMEN_AV_SYNC_TELEMETRY requires LUMEN_WITH_IOS_RUNTIME=ON for iOS builds")
	endif()
endif()

if(LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY)
	set(LUMEN_DENSE_DIRECTORY_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_DENSE_DIRECTORY_ARCH)
		set(LUMEN_DENSE_DIRECTORY_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if(NOT LUMEN_DENSE_DIRECTORY_ARCH MATCHES "^(arm64|aarch64)$")
		message(FATAL_ERROR
			"LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY requires a thin Apple ARM64 build")
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY OR
	   LUMEN_A64_DYNAREC_TELEMETRY OR
	   LUMEN_A64_DYNAREC_STARTUP_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY OR
	   LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY OR
	   LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY OR
	   LUMEN_A64_DYNAREC_HOST_PC_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CK_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CM_PROFILE OR
	   LUMEN_A64_DYNAREC_PHASE6CO_PROFILE OR
	   LUMEN_A64_DYNAREC_RFLAGS_PROFILE OR
	   LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
		message(FATAL_ERROR
			"LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY cannot be combined with CPU telemetry or profiling selectors")
	endif()
endif()

include(apple_native_metal.cmake)
lumen_detect_apple_native_metal_target(
	LUMEN_APPLE_NATIVE_METAL_TARGET_SUPPORTED
	LUMEN_APPLE_NATIVE_METAL_ARCHITECTURES
)
set(LUMEN_APPLE_NATIVE_METAL_DEFAULT OFF)
if(LUMEN_APPLE_NATIVE_METAL_TARGET_SUPPORTED)
	set(LUMEN_APPLE_NATIVE_METAL_DEFAULT ON)
endif()
option(LUMEN_APPLE_NATIVE_METAL
	"Use Lumen's native Metal Vulkan-contract adapter on supported Apple targets"
	${LUMEN_APPLE_NATIVE_METAL_DEFAULT}
)
if(LUMEN_APPLE_NATIVE_METAL AND NOT LUMEN_APPLE_NATIVE_METAL_TARGET_SUPPORTED)
	message(FATAL_ERROR
		"LUMEN_APPLE_NATIVE_METAL=ON requires exactly one Apple target architecture: arm64, aarch64 or x86_64; "
		"target platform='${CMAKE_SYSTEM_NAME}', architectures='${LUMEN_APPLE_NATIVE_METAL_ARCHITECTURES}'")
endif()

if(LUMEN_APPLE_NATIVE_METAL)
	message(STATUS
		"Apple graphics backend: native Metal adapter (${LUMEN_APPLE_NATIVE_METAL_ARCHITECTURES})")
elseif(APPLE_PLATFORM)
	message(STATUS
		"Apple graphics backend: Vulkan/MoltenVK rollback (${LUMEN_APPLE_NATIVE_METAL_ARCHITECTURES})")
else()
	message(STATUS "Graphics backend: platform Vulkan loader (native Metal adapter unavailable)")
endif()

set(LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT_VALUE OFF)
if(APPLE_PLATFORM AND LUMEN_APPLE_NATIVE_METAL)
	set(LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT_VALUE ON)
endif()
option(LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT
	"Enable exact-zero Gen4 PS sampled-image handling by default"
	${LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT_VALUE}
)

set(LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT_VALUE OFF)
if(WIN32 OR LUMEN_MACOS_FEXCORE_RUNTIME OR
   (ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a" AND LUMEN_ANDROID_FEXCORE_LAB AND LUMEN_ANDROID_FULL_RUNTIME))
	set(LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT_VALUE ON)
endif()
option(LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT
	"Upload changed vertex buffers through host-visible coherent memory by default"
	${LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT_VALUE}
)

# The retired GPU page-fault watcher missed GPU/video/readback writes and
# corrupted Windows rendering. Keep its old entry points out of production so
# it cannot be revived accidentally behind a flag that defaults to false.
set(LUMEN_GPU_PAGE_WATCHER_GUARD_FILES
	"${PROJECT_SOURCE_DIR}/emulator/include/Emulator/Graphics/Objects/GpuMemory.h"
	"${PROJECT_SOURCE_DIR}/emulator/src/Graphics/GraphicsRun.cpp"
	"${PROJECT_SOURCE_DIR}/emulator/src/Graphics/Objects/GpuMemory.cpp"
	"${PROJECT_SOURCE_DIR}/emulator/src/Loader/RuntimeLinker.cpp"
	"${PROJECT_SOURCE_DIR}/include/Lumen/Core/VirtualMemory.h"
	"${PROJECT_SOURCE_DIR}/lib/Core/src/VirtualMemory.cpp"
	"${PROJECT_SOURCE_DIR}/lib/Core/src/VirtualMemoryMacOS.cpp"
)
set(LUMEN_GPU_PAGE_WATCHER_RETIRED_TOKENS
	GpuMemoryWatcherEnabled
	GpuMemoryCheckAccessViolation
	GpuMemoryMarkCpuDirty
	GpuMemoryIsCpuPageWatched
	RegisterWatchedPages
	UnregisterWatchedPages
	ArmWatchedPages
	HandleCpuWriteFault
	m_watched_pages
	pages_registered
	InstallVectored
)
foreach(GUARD_FILE IN LISTS LUMEN_GPU_PAGE_WATCHER_GUARD_FILES)
	file(READ "${GUARD_FILE}" GUARD_CONTENTS)
	foreach(RETIRED_TOKEN IN LISTS LUMEN_GPU_PAGE_WATCHER_RETIRED_TOKENS)
		string(FIND "${GUARD_CONTENTS}" "${RETIRED_TOKEN}" RETIRED_TOKEN_OFFSET)
		if(NOT RETIRED_TOKEN_OFFSET EQUAL -1)
			message(FATAL_ERROR
				"Retired GPU page-fault watcher token '${RETIRED_TOKEN}' found in ${GUARD_FILE}")
		endif()
	endforeach()
endforeach()

# Keep expensive debug instrumentation independent from LUMEN_FINAL so a
# standard optimized build can retain ASSERT/EXIT_IF without also paying for
# instrumentation. ReleaseFinal may still disable those checks. The project
# build scripts use a single configuration per build directory, making
# CMAKE_BUILD_TYPE the appropriate default selector while still allowing an
# explicit cache override.
set(LUMEN_DEBUG_INSTRUMENTATION_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	set(LUMEN_DEBUG_INSTRUMENTATION_DEFAULT ON)
endif()

set(LUMEN_MEMORY_TRACKER_DEFAULT OFF)
if(LUMEN_DEBUG_INSTRUMENTATION_DEFAULT AND (WIN32 OR LINUX))
	set(LUMEN_MEMORY_TRACKER_DEFAULT ON)
endif()

option(LUMEN_ENABLE_MEMORY_TRACKER
	"Track host allocations with stack traces and guard patterns (Windows/Linux only)"
	${LUMEN_MEMORY_TRACKER_DEFAULT})
option(LUMEN_ENABLE_EASY_PROFILER
	"Compile EasyProfiler instrumentation into emulator call sites"
	${LUMEN_DEBUG_INSTRUMENTATION_DEFAULT})

if(LUMEN_ENABLE_MEMORY_TRACKER AND APPLE_PLATFORM)
	message(FATAL_ERROR "LUMEN_ENABLE_MEMORY_TRACKER is not supported on Apple platforms")
endif()

message(STATUS "Lumen instrumentation for ${CMAKE_BUILD_TYPE}:")
message(STATUS "  Host memory tracker: ${LUMEN_ENABLE_MEMORY_TRACKER}")
message(STATUS "  EasyProfiler call sites: ${LUMEN_ENABLE_EASY_PROFILER}")

string(TOUPPER ${LUMEN_PROJECT_NAME} LUMEN_PROJECT)
string(TOLOWER ${LUMEN_COMPILER} LUMEN_COMPILER_ID)
string(TOLOWER ${LUMEN_LINKER} LUMEN_LINKER_ID)

if (CLANG AND (LUMEN_LINKER STREQUAL LLD))
	set(LUMEN_LD_OPTIONS "-fuse-ld=lld")
endif()

if (LUMEN_LINKER STREQUAL LD AND NOT LINUX AND NOT APPLE_PLATFORM)
	set(LUMEN_LD_OPTIONS "-Wl,--image-base=0x100000000000")
endif()

project(LumenEmu${LUMEN_PROJECT_NAME}${CMAKE_BUILD_TYPE}${LUMEN_COMPILER} VERSION 0.2.2)

# CpuX64.cpp is also compiled directly by focused tests and the iOS CPU
# archive, rather than only through the emulator target. Propagate the
# configured profiler-call-site state to every target in this directory so
# the runtime configuration receipt reports the actual build. Standalone
# bootstrap consumers that do not enter this CMake project retain the
# conservative fallback in CpuX64.cpp.
if(LUMEN_ENABLE_EASY_PROFILER)
	add_compile_definitions(LUMEN_ENABLE_EASY_PROFILER=1)
else()
	add_compile_definitions(LUMEN_ENABLE_EASY_PROFILER=0)
endif()

if(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2)
	# CpuX64.cpp is compiled by the emulator, iOS archives and focused tests.
	# Leave the macro absent when OFF so the Region dispatcher, TLS and executor
	# are compiled out; the shared VM lease substrate and configuration receipt
	# remain part of the product. An ON build still requires the exact-one runtime
	# selector.
	add_compile_definitions(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2=1)
endif()
if(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT)
	add_compile_definitions(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2_REPORT=1)
endif()

if(LUMEN_APPLE_NATIVE_METAL)
	# The adapter is implemented as Objective-C++ and is compiled in the same
	# static graph on macOS and iOS. Enable the language before adding any
	# third-party or emulator targets that consume it.
	enable_language(OBJCXX)
	set(CMAKE_OBJCXX_STANDARD 17)
	set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
	set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

if(LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK)
	add_compile_definitions(LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK=1)
else()
	add_compile_definitions(LUMEN_ENABLE_APPLE_ARM64_VMOVD_FAST_BLOCK=0)
endif()

if(LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK)
	add_compile_definitions(LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK=1)
else()
	add_compile_definitions(LUMEN_ENABLE_APPLE_ARM64_VECTOR_PERMUTE_FAST_BLOCK=0)
endif()

include(src_script.cmake)

include_directories(	
	3rdparty/gtest/include
	3rdparty/gtest
	3rdparty/lua/include
	3rdparty/rijndael/source
	3rdparty/miniz
	3rdparty/magic_enum/include
	${PROJECT_BINARY_DIR}
	include
    )

set(LUMEN_VERSION "${PROJECT_VERSION}")

configure_file(
  ${PROJECT_SOURCE_DIR}/cmake_config.h.in
  ${PROJECT_BINARY_DIR}/cmake_config.h
  )

find_package(Git)
  
add_custom_target( LumenGitVersion
    COMMAND ${CMAKE_COMMAND} 
	-D INPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/LumenGitVersion.h.in
    -D OUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/LumenGitVersion.h
	-D GIT_EXECUTABLE=${GIT_EXECUTABLE}
	-D GIT_WORK_TREE=${PROJECT_SOURCE_DIR}/..
	-P ${CMAKE_CURRENT_SOURCE_DIR}/generate_version.cmake
	COMMENT "Generate LumenGitVersion.h"
)

if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0.0 AND NOT ANDROID)
	list(APPEND LUMEN_IWYU
		emulator
		core
		math_obj
		scripts
		sys
		unit_test
	)
	list(APPEND LUMEN_CLANG_TYDY
		emulator
		#core
		#math_obj
		#scripts
		#sys
		#unit_test
	)
endif()

set(LUMEN_LIB_HEADERS ${PROJECT_SOURCE_DIR}/include)

config_compiler_and_linker()  

# This pure conversion gate is also available before Android's runtime return.
if(LUMEN_WITH_IME_DIALOG_TEST)
	enable_testing()
	add_executable(lumen_sdl_android_text_test EXCLUDE_FROM_ALL SdlAndroidTextTest.cpp)
	add_test(NAME lumen_sdl_android_text COMMAND $<TARGET_FILE:lumen_sdl_android_text_test>)
	set_tests_properties(lumen_sdl_android_text PROPERTIES LABELS "ime;android;unicode;local-contract" TIMEOUT 30)
endif()

if(NOT ANDROID)
	enable_testing()
	if(NOT CMAKE_CROSSCOMPILING)
		add_test(NAME lumen_apple_native_metal_build_graph
			COMMAND "${CMAKE_COMMAND}"
			        "-DLUMEN_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
			        -P "${PROJECT_SOURCE_DIR}/tests/AppleNativeMetalBuildGraphTest.cmake"
		)
		set_tests_properties(lumen_apple_native_metal_build_graph PROPERTIES
			LABELS "build-graph;graphics"
			TIMEOUT 30
		)
	endif()
endif()

# Android builds only the in-process runtime graph. Packaging, Activity/SDL
# glue and ROM staging remain owned by the Gradle application.
if(ANDROID)
	if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
		message(FATAL_ERROR "the Android runtime supports only arm64-v8a, not ${ANDROID_ABI}")
	endif()
	if(NOT LUMEN_ANDROID_FEXCORE_LAB OR NOT LUMEN_ANDROID_FULL_RUNTIME OR
	   NOT TARGET lumen_android_fexcore_runtime)
		message(FATAL_ERROR
			"The Android runtime requires the FEX application build; interpreter-only builds are no longer supported")
	endif()
	if(NOT LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "disabled" OR
	   NOT LUMEN_ANDROID_DYNAREC_POLICY STREQUAL "probe-only")
		message(FATAL_ERROR "The Android FEX runtime does not allow NODA or alternate dynarec providers")
	endif()
	add_subdirectory(3rdparty)
	add_subdirectory(emulator)
	add_subdirectory(lib)
	message(STATUS "Android runtime: static loader/HLE/SDL/Vulkan graph (FEX only, no fallback)")
	return()
endif()

# The core distribution retains the standalone iOS CPU/VM bootstrap. The
# full runtime needs an application-supplied window and input integration.
if(IOS)
	if(LUMEN_WITH_IOS_RUNTIME)
		message(FATAL_ERROR "The full iOS runtime requires the application shell omitted from this core source distribution")
	endif()
	if(LUMEN_WITH_RUNTIME_LIFECYCLE_TEST AND NOT LUMEN_WITH_IOS_RUNTIME)
		message(FATAL_ERROR "LUMEN_WITH_RUNTIME_LIFECYCLE_TEST requires LUMEN_WITH_IOS_RUNTIME on iOS")
	endif()
	if(LUMEN_WITH_PAD_HANDLE_TEST AND NOT LUMEN_WITH_IOS_RUNTIME)
		message(FATAL_ERROR "LUMEN_WITH_PAD_HANDLE_TEST requires LUMEN_WITH_IOS_RUNTIME on iOS")
	endif()
	if(NOT CMAKE_OSX_ARCHITECTURES)
		message(FATAL_ERROR "iOS requires -DCMAKE_OSX_ARCHITECTURES=arm64")
	endif()
	foreach(IOS_ARCH IN LISTS CMAKE_OSX_ARCHITECTURES)
		if(NOT IOS_ARCH STREQUAL "arm64")
			message(FATAL_ERROR "the Apple Silicon iOS core supports only arm64, not ${IOS_ARCH}")
		endif()
	endforeach()

	enable_language(OBJCXX)

	add_library(lumen_x64_cpu_backend STATIC emulator/src/CpuX64.cpp emulator/src/CpuA64Dynarec.cpp)
	target_include_directories(lumen_x64_cpu_backend PUBLIC "${PROJECT_SOURCE_DIR}/emulator/include")
	if(LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY)
		target_compile_definitions(lumen_x64_cpu_backend PRIVATE
			LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1)
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY)
		target_compile_definitions(lumen_x64_cpu_backend PRIVATE LUMEN_FAST_BLOCK_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
		target_compile_definitions(lumen_x64_cpu_backend PRIVATE LUMEN_A64_DYNAREC_STARTUP_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
		target_compile_definitions(lumen_x64_cpu_backend PRIVATE LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY=1)
	endif()
	set_target_properties(lumen_x64_cpu_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)
	add_library(lumen_apple_virtual_memory STATIC lib/Core/src/VirtualMemoryMacOS.cpp)
	target_include_directories(lumen_apple_virtual_memory PUBLIC "${PROJECT_SOURCE_DIR}/include")
	# The bootstrap archive is intentionally self-contained and exercises only
	# the mapping registry. Full Core builds keep the Sys-backed system-info hook.
	target_compile_definitions(lumen_apple_virtual_memory PRIVATE LUMEN_APPLE_VM_STANDALONE=1)
	set_target_properties(lumen_apple_virtual_memory PROPERTIES POSITION_INDEPENDENT_CODE ON)
	if(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2)
		target_link_libraries(lumen_x64_cpu_backend PRIVATE lumen_apple_virtual_memory)
	endif()


	add_executable(lumen_ios_production_smoke AppleBootstrapSmoke.cpp)
	target_link_libraries(lumen_ios_production_smoke PRIVATE
		lumen_x64_cpu_backend lumen_apple_virtual_memory)
	add_custom_target(lumen_ios_bootstrap DEPENDS lumen_ios_production_smoke)
	message(STATUS "iOS core: standalone CPU/VM bootstrap; application shell is not included")
	return()
endif()

add_subdirectory(3rdparty)
if(LUMEN_MACOS_X64_BACKEND STREQUAL "fex")
	# Keep FEX in a separate product graph and make its usage requirements
	# available before the emulator target selects its execution provider.
	add_subdirectory(
		"${PROJECT_SOURCE_DIR}/../macos/fexcore-runtime"
		"${CMAKE_BINARY_DIR}/macos-fexcore-runtime")
	# Lumen globally disables RTTI, while FEX's configuration and pass layers
	# use dynamic_cast. Restore RTTI only on the two external implementation
	# targets; the adapter and every Lumen target keep the normal policy.
	foreach(LUMEN_MACOS_FEXCORE_RTTI_TARGET FEXCore_Base FEXCore_object)
		if(TARGET ${LUMEN_MACOS_FEXCORE_RTTI_TARGET})
			target_compile_options(${LUMEN_MACOS_FEXCORE_RTTI_TARGET} PRIVATE -frtti)
		endif()
	endforeach()
endif()
add_subdirectory(emulator)
add_subdirectory(unit_test)
add_subdirectory(lib)

if(LUMEN_WITH_RUNTIME_LIFECYCLE_TEST)
	add_executable(lumen_runtime_lifecycle_test EXCLUDE_FROM_ALL RuntimeLifecycleTest.cpp)
	# Runtime initialization links the unit_test registry, whose GoogleTest
	# implementation is normally supplied by the desktop executable.
	target_sources(lumen_runtime_lifecycle_test PRIVATE 3rdparty/gtest/src/gtest-all.cc)
	target_compile_definitions(lumen_runtime_lifecycle_test PRIVATE LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK=1)
	target_link_libraries(lumen_runtime_lifecycle_test PRIVATE emulator)
	add_test(NAME lumen_runtime_lifecycle COMMAND $<TARGET_FILE:lumen_runtime_lifecycle_test>)
	set_tests_properties(lumen_runtime_lifecycle PROPERTIES TIMEOUT 180)
endif()

if(LUMEN_WITH_MODULE_LOAD_DEDUPE_TEST)
	target_compile_definitions(emulator PRIVATE LUMEN_MODULE_LOAD_DEDUPE_TEST_HOOK)
	add_executable(lumen_module_load_dedupe_test EXCLUDE_FROM_ALL ModuleLoadDedupeTest.cpp)
	target_compile_definitions(lumen_module_load_dedupe_test PRIVATE LUMEN_MODULE_LOAD_DEDUPE_TEST_HOOK=1)
	target_link_libraries(lumen_module_load_dedupe_test PRIVATE emulator)
	add_test(NAME lumen_module_load_dedupe COMMAND $<TARGET_FILE:lumen_module_load_dedupe_test>)
	set_tests_properties(lumen_module_load_dedupe PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "ModuleLoadDedupeTest: PASS")
endif()

if(LUMEN_WITH_PAD_HANDLE_TEST)
	add_executable(lumen_pad_handle_test EXCLUDE_FROM_ALL PadHandleTest.cpp)
	target_link_libraries(lumen_pad_handle_test PRIVATE emulator)
	add_test(NAME lumen_pad_handle COMMAND $<TARGET_FILE:lumen_pad_handle_test>)
	set_tests_properties(lumen_pad_handle PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "PadHandleTest: PASS")
endif()

if(LUMEN_WITH_PAD_ANALOG_INPUT_TEST)
	add_executable(lumen_pad_analog_input_test EXCLUDE_FROM_ALL PadAnalogInputTest.cpp)
	target_link_libraries(lumen_pad_analog_input_test PRIVATE emulator)
	add_test(NAME lumen_pad_analog_input COMMAND $<TARGET_FILE:lumen_pad_analog_input_test>)
	set_tests_properties(lumen_pad_analog_input PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "PadAnalogInputTest: PASS")
endif()

if(LUMEN_WITH_NP_CMP_ABORT_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_np_cmp_abort_test EXCLUDE_FROM_ALL NpCmpAbortTest.cpp)
	target_link_libraries(lumen_np_cmp_abort_test PRIVATE emulator Threads::Threads)
	set(np_cmp_abort_tests lumen_np_cmp_abort)
	add_test(NAME lumen_np_cmp_abort COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		$<TARGET_FILE:lumen_np_cmp_abort_test> 0 0)
	if(MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
		foreach(plus IN ITEMS 0 1)
			foreach(parental IN ITEMS 0 1)
				if(plus STREQUAL "0" AND parental STREQUAL "0")
					continue()
				endif()
				set(matrix_test "lumen_np_cmp_abort_${plus}_${parental}")
				list(APPEND np_cmp_abort_tests ${matrix_test})
				add_test(NAME ${matrix_test} COMMAND ${CMAKE_COMMAND} -E env
					"LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=${plus}" "LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=${parental}"
					$<TARGET_FILE:lumen_np_cmp_abort_test> ${plus} ${parental})
			endforeach()
		endforeach()
	endif()
	set_tests_properties(${np_cmp_abort_tests} PROPERTIES TIMEOUT 20
		PASS_REGULAR_EXPRESSION "NpCmpAbortTest: PASS")
endif()

if(LUMEN_WITH_NETCTL_STATE_TEST)
	add_executable(lumen_netctl_state_test EXCLUDE_FROM_ALL NetCtlStateTest.cpp)
	target_link_libraries(lumen_netctl_state_test PRIVATE emulator)
	add_test(NAME lumen_netctl_state_selector_unset COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_NETCTL_OFFLINE
		-- $<TARGET_FILE:lumen_netctl_state_test> unset)
	add_test(NAME lumen_netctl_state_selector_zero COMMAND ${CMAKE_COMMAND} -E env
		LUMEN_EXPERIMENTAL_NETCTL_OFFLINE=0
		-- $<TARGET_FILE:lumen_netctl_state_test> zero)
	add_test(NAME lumen_netctl_state_selector_one COMMAND ${CMAKE_COMMAND} -E env
		LUMEN_EXPERIMENTAL_NETCTL_OFFLINE=1
		-- $<TARGET_FILE:lumen_netctl_state_test> one)
	add_test(NAME lumen_netctl_state_selector_malformed COMMAND ${CMAKE_COMMAND} -E env
		LUMEN_EXPERIMENTAL_NETCTL_OFFLINE=01
		-- $<TARGET_FILE:lumen_netctl_state_test> malformed)
	set_tests_properties(lumen_netctl_state_selector_unset lumen_netctl_state_selector_zero
		lumen_netctl_state_selector_one lumen_netctl_state_selector_malformed
		PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "NetCtlStateTest: PASS")
endif()

if(LUMEN_WITH_NET_OFFLINE_SURFACE_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_net_offline_surface_test EXCLUDE_FROM_ALL NetOfflineSurfaceTest.cpp)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_net_offline_surface_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	endif()
	target_link_libraries(lumen_net_offline_surface_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_net_offline_surface COMMAND $<TARGET_FILE:lumen_net_offline_surface_test>)
	set_tests_properties(lumen_net_offline_surface PROPERTIES
		TIMEOUT 15
		PASS_REGULAR_EXPRESSION "NetOfflineSurfaceTest: PASS"
		LABELS "network;offline;abi")
endif()

if(LUMEN_WITH_APP_CONTENT_TEMPORARY_DATA_TEST AND LUMEN_MACOS_FEXCORE_RUNTIME)
	add_executable(lumen_appcontent_temporary_data_test EXCLUDE_FROM_ALL
		AppContentTemporaryDataTest.cpp)
	target_compile_definitions(lumen_appcontent_temporary_data_test PRIVATE
		LUMEN_FEXCORE_RUNTIME=1)
	target_link_libraries(lumen_appcontent_temporary_data_test PRIVATE emulator)
	add_test(NAME lumen_appcontent_temporary_data
		COMMAND $<TARGET_FILE:lumen_appcontent_temporary_data_test>)
	set_tests_properties(lumen_appcontent_temporary_data PROPERTIES
		TIMEOUT 15
		PASS_REGULAR_EXPRESSION "AppContentTemporaryDataTest: PASS"
		LABELS "macos;fexcore;appcontent;filesystem;guest-memory")
endif()

if(LUMEN_WITH_NP_TROPHY_BINDING_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_np_trophy_binding_test EXCLUDE_FROM_ALL NpTrophyBindingTest.cpp)
	target_link_libraries(lumen_np_trophy_binding_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_np_trophy_binding COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE --unset=LUMEN_EXPERIMENTAL_VOICE_OFFLINE
		$<TARGET_FILE:lumen_np_trophy_binding_test>)
	set_tests_properties(lumen_np_trophy_binding PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "NpTrophyBindingTest: PASS")
endif()

if(LUMEN_WITH_RTC_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_rtc_test EXCLUDE_FROM_ALL RtcTest.cpp)
	target_link_libraries(lumen_rtc_test PRIVATE emulator Threads::Threads)
	if(MSVC AND CLANG AND
	   CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
		execute_process(
			COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
			OUTPUT_VARIABLE RTC_CLANG_RESOURCE_DIR
			OUTPUT_STRIP_TRAILING_WHITESPACE)
		find_library(RTC_CLANG_BUILTINS_LIBRARY
			NAMES clang_rt.builtins-x86_64
			PATHS "${RTC_CLANG_RESOURCE_DIR}/lib/windows"
			NO_DEFAULT_PATH)
		if(NOT RTC_CLANG_BUILTINS_LIBRARY)
			message(FATAL_ERROR "clang x86_64 builtins were not found for lumen_rtc_test")
		endif()
		target_link_libraries(lumen_rtc_test PRIVATE
			"${RTC_CLANG_BUILTINS_LIBRARY}")
	endif()
	add_test(NAME lumen_rtc COMMAND $<TARGET_FILE:lumen_rtc_test>)
	set_tests_properties(lumen_rtc PROPERTIES TIMEOUT 20
		PASS_REGULAR_EXPRESSION "RtcTest: PASS")
endif()

if(LUMEN_WITH_ERROR_DIALOG_TEST)
	add_executable(lumen_error_dialog_test EXCLUDE_FROM_ALL ErrorDialogTest.cpp)
	target_link_libraries(lumen_error_dialog_test PRIVATE emulator)
	add_test(NAME lumen_error_dialog COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE
		-- $<TARGET_FILE:lumen_error_dialog_test>)
	set_tests_properties(lumen_error_dialog PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "ErrorDialogTest: PASS")

	add_executable(lumen_save_data_dialog_test EXCLUDE_FROM_ALL SaveDataDialogTest.cpp)
	target_link_libraries(lumen_save_data_dialog_test PRIVATE emulator)
	add_test(NAME lumen_save_data_dialog COMMAND $<TARGET_FILE:lumen_save_data_dialog_test>)
	set_tests_properties(lumen_save_data_dialog PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "SaveDataDialogTest: PASS")
endif()

if(LUMEN_WITH_NP_COMMERCE_DIALOG_TEST)
	set(LUMEN_NP_COMMERCE_DIALOG_TEST_QUALIFIED OFF)
	if((MACOS AND NOT IOS AND APPLE AND
	    (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
	     (NOT CMAKE_OSX_ARCHITECTURES AND
	      CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"))) OR
	   (WIN32 AND LUMEN_BITNESS EQUAL 64 AND
	    CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$"))
		set(LUMEN_NP_COMMERCE_DIALOG_TEST_QUALIFIED ON)
	endif()

	if(LUMEN_NP_COMMERCE_DIALOG_TEST_QUALIFIED)
		add_executable(lumen_np_commerce_dialog_test EXCLUDE_FROM_ALL NpCommerceDialogTest.cpp)
		target_link_libraries(lumen_np_commerce_dialog_test PRIVATE emulator)
		if(WIN32)
			target_compile_definitions(lumen_np_commerce_dialog_test PRIVATE
				_CRT_SECURE_NO_WARNINGS)
		endif()
		if(MSVC AND CLANG)
			execute_process(
				COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
				OUTPUT_VARIABLE NP_COMMERCE_CLANG_RESOURCE_DIR
				OUTPUT_STRIP_TRAILING_WHITESPACE)
			find_library(NP_COMMERCE_CLANG_BUILTINS_LIBRARY
				NAMES clang_rt.builtins-x86_64
				PATHS "${NP_COMMERCE_CLANG_RESOURCE_DIR}/lib/windows"
				NO_DEFAULT_PATH)
			if(NOT NP_COMMERCE_CLANG_BUILTINS_LIBRARY)
				message(FATAL_ERROR "clang x86_64 builtins were not found for lumen_np_commerce_dialog_test")
			endif()
			target_link_libraries(lumen_np_commerce_dialog_test PRIVATE
				"${NP_COMMERCE_CLANG_BUILTINS_LIBRARY}")
		endif()
		set(LUMEN_NP_COMMERCE_DIALOG_TESTS
			lumen_np_commerce_dialog_signature
			lumen_np_commerce_dialog_selector_absent
			lumen_np_commerce_dialog_selector_zero
			lumen_np_commerce_dialog_selector_empty
			lumen_np_commerce_dialog_selector_whitespace
			lumen_np_commerce_dialog_selector_malformed
			lumen_np_commerce_dialog_selector_word
			lumen_np_commerce_dialog_selector_one
			lumen_np_commerce_dialog_latch_disabled
			lumen_np_commerce_dialog_latch_enabled
			lumen_np_commerce_dialog_exact_binding
			lumen_np_commerce_dialog_negative_identity
			lumen_np_commerce_dialog_siblings_absent
			lumen_np_commerce_dialog_baseline_disabled
			lumen_np_commerce_dialog_baseline_enabled
			lumen_np_commerce_dialog_polling
			lumen_np_commerce_dialog_isolation)

		add_test(NAME lumen_np_commerce_dialog_signature COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> signature)
		add_test(NAME lumen_np_commerce_dialog_selector_absent COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> selector-absent)
		add_test(NAME lumen_np_commerce_dialog_selector_zero COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=0
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> selector-zero)
		add_test(NAME lumen_np_commerce_dialog_selector_empty COMMAND ${CMAKE_COMMAND} -E env
			"LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE="
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> selector-empty)
		add_test(NAME lumen_np_commerce_dialog_selector_whitespace COMMAND ${CMAKE_COMMAND} -E env
			"LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE= "
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> selector-whitespace)
		add_test(NAME lumen_np_commerce_dialog_selector_malformed COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=01
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> selector-malformed)
		add_test(NAME lumen_np_commerce_dialog_selector_word COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=true
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> selector-word)
		add_test(NAME lumen_np_commerce_dialog_selector_one COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=1
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> selector-one)
		add_test(NAME lumen_np_commerce_dialog_latch_disabled COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=0
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> latch-disabled)
		add_test(NAME lumen_np_commerce_dialog_latch_enabled COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=1
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> latch-enabled)
		add_test(NAME lumen_np_commerce_dialog_exact_binding COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=1
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> exact-binding)
		add_test(NAME lumen_np_commerce_dialog_negative_identity COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=1
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> negative-identity)
		add_test(NAME lumen_np_commerce_dialog_siblings_absent COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=1
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> siblings-absent)
		add_test(NAME lumen_np_commerce_dialog_baseline_disabled COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=0
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> baseline-disabled)
		add_test(NAME lumen_np_commerce_dialog_baseline_enabled COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=1
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> baseline-enabled)
		add_test(NAME lumen_np_commerce_dialog_polling COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=1
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> polling)
		add_test(NAME lumen_np_commerce_dialog_isolation COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_NP_COMMERCE_DIALOG_OFFLINE=1
			-- $<TARGET_FILE:lumen_np_commerce_dialog_test> isolation)
		set_tests_properties(${LUMEN_NP_COMMERCE_DIALOG_TESTS} PROPERTIES
			ENVIRONMENT "LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=1"
			LABELS "dialog;np-commerce;offline-contract"
			TIMEOUT 15
			PASS_REGULAR_EXPRESSION "NpCommerceDialogTest: PASS")
		unset(LUMEN_NP_COMMERCE_DIALOG_TESTS)
	else()
		message(STATUS
			"NpCommerceDialog runtime tests: NON VERIFICATO (requires a thin macOS arm64 build)")
	endif()
	unset(LUMEN_NP_COMMERCE_DIALOG_TEST_QUALIFIED)
endif()

if(LUMEN_WITH_SAVE_DATA_DIR_NAME_SEARCH_TEST)
	find_package(Threads REQUIRED)
	set(LUMEN_SAVE_DATA_TEST_WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/SaveDataTestWork")
	file(MAKE_DIRECTORY "${LUMEN_SAVE_DATA_TEST_WORK_DIR}")
	add_executable(lumen_save_data_dir_name_search_test EXCLUDE_FROM_ALL SaveDataTest.cpp)
	set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/emulator/src/Libs/LibSaveData.cpp"
		DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/emulator"
		APPEND PROPERTY COMPILE_DEFINITIONS LUMEN_SAVE_DATA_TEST_HOOK=1)
	target_compile_definitions(lumen_save_data_dir_name_search_test PRIVATE LUMEN_SAVE_DATA_TEST_HOOK=1)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_save_data_dir_name_search_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	endif()
	target_link_libraries(lumen_save_data_dir_name_search_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_save_data_dir_name_search
		COMMAND $<TARGET_FILE:lumen_save_data_dir_name_search_test> "${LUMEN_SAVE_DATA_TEST_WORK_DIR}")
	set_tests_properties(lumen_save_data_dir_name_search PROPERTIES
		WORKING_DIRECTORY "${LUMEN_SAVE_DATA_TEST_WORK_DIR}"
		TIMEOUT 30
		PASS_REGULAR_EXPRESSION "SaveDataTest: PASS")
endif()

if(LUMEN_WITH_GNM_SET_CS_SHADER_TEST)
	add_executable(lumen_gnm_set_cs_shader_test EXCLUDE_FROM_ALL GnmSetCsShaderTest.cpp)
	target_link_libraries(lumen_gnm_set_cs_shader_test PRIVATE emulator)
	add_test(NAME lumen_gnm_set_cs_shader COMMAND $<TARGET_FILE:lumen_gnm_set_cs_shader_test>)
	set_tests_properties(lumen_gnm_set_cs_shader PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "GnmSetCsShaderTest: PASS")
endif()

if(LUMEN_WITH_GNM_EMBEDDED_PS_SHADER_TEST)
	add_executable(lumen_gnm_embedded_ps_shader_test EXCLUDE_FROM_ALL GnmEmbeddedPsShaderTest.cpp)
	target_link_libraries(lumen_gnm_embedded_ps_shader_test PRIVATE emulator)
	add_test(NAME lumen_gnm_embedded_ps_shader COMMAND $<TARGET_FILE:lumen_gnm_embedded_ps_shader_test>)
	set_tests_properties(lumen_gnm_embedded_ps_shader PROPERTIES TIMEOUT 30 LABELS "graphics;gnm;embedded-shader"
		PASS_REGULAR_EXPRESSION "GnmEmbeddedPsShaderTest: PASS")
endif()

if(LUMEN_WITH_GNM_MARKER_TEST)
	add_executable(lumen_gnm_marker_test EXCLUDE_FROM_ALL GnmMarkerTest.cpp)
	target_link_libraries(lumen_gnm_marker_test PRIVATE emulator)
	add_test(NAME lumen_gnm_marker COMMAND $<TARGET_FILE:lumen_gnm_marker_test>)
	set_tests_properties(lumen_gnm_marker PROPERTIES TIMEOUT 30 LABELS "graphics;gnm;marker"
		PASS_REGULAR_EXPRESSION "GnmMarkerTest: PASS")
endif()

if(LUMEN_WITH_GNM_VGT_CONTROL_TEST)
	add_executable(lumen_gnm_vgt_control_test EXCLUDE_FROM_ALL GnmVgtControlTest.cpp)
	target_link_libraries(lumen_gnm_vgt_control_test PRIVATE emulator)
	add_test(NAME lumen_gnm_vgt_control COMMAND $<TARGET_FILE:lumen_gnm_vgt_control_test>)
	set_tests_properties(lumen_gnm_vgt_control PROPERTIES TIMEOUT 30 LABELS "graphics;gnm;vgt-control"
		PASS_REGULAR_EXPRESSION "GnmVgtControlTest: PASS")
endif()

if(LUMEN_WITH_HLE_IMPLEMENTED_EXPORTS_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_hle_implemented_exports_test EXCLUDE_FROM_ALL HleImplementedExportsTest.cpp)
	target_link_libraries(lumen_hle_implemented_exports_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_hle_implemented_exports COMMAND $<TARGET_FILE:lumen_hle_implemented_exports_test>)
	set_tests_properties(lumen_hle_implemented_exports PROPERTIES TIMEOUT 30
		ENVIRONMENT "LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=1"
		LABELS "hle;imports;registry" PASS_REGULAR_EXPRESSION "HleImplementedExportsTest: PASS")
endif()

if(LUMEN_WITH_GNM_EQ_EVENT_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_gnm_eq_event_test EXCLUDE_FROM_ALL GnmEqEventTest.cpp)
	target_link_libraries(lumen_gnm_eq_event_test PRIVATE emulator Threads::Threads)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_gnm_eq_event_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	endif()
	foreach(graphics_event_case IN ITEMS contract lifetime completion)
		add_test(NAME lumen_gnm_eq_event_${graphics_event_case}
			COMMAND $<TARGET_FILE:lumen_gnm_eq_event_test> ${graphics_event_case})
		set_tests_properties(lumen_gnm_eq_event_${graphics_event_case} PROPERTIES TIMEOUT 30
			LABELS "graphics;gnm;event-queue" PASS_REGULAR_EXPRESSION "GnmEqEventTest: PASS")
	endforeach()
endif()

if(LUMEN_WITH_GNM_WORKLOAD_STREAM_TEST)
	add_executable(lumen_gnm_workload_stream_test EXCLUDE_FROM_ALL GnmWorkloadStreamTest.cpp)
	target_link_libraries(lumen_gnm_workload_stream_test PRIVATE emulator)
	add_test(NAME lumen_gnm_workload_stream COMMAND $<TARGET_FILE:lumen_gnm_workload_stream_test>)
	set_tests_properties(lumen_gnm_workload_stream PROPERTIES TIMEOUT 15 LABELS "graphics;gnm;workload"
		PASS_REGULAR_EXPRESSION "GnmWorkloadStreamTest: PASS")
	if(MACOS OR WIN32)
		foreach(workload_api IN ITEMS create begin)
			foreach(workload_guard IN ITEMS invalid readonly noaccess crosspage overflow)
				add_test(NAME lumen_gnm_workload_stream_${workload_api}_${workload_guard}
					COMMAND ${CMAKE_COMMAND}
						-DWORKLOAD_TEST=$<TARGET_FILE:lumen_gnm_workload_stream_test>
						-DWORKLOAD_CASE=${workload_api}-${workload_guard}
						-P ${CMAKE_CURRENT_SOURCE_DIR}/GnmWorkloadStreamGuardTest.cmake)
				set_tests_properties(lumen_gnm_workload_stream_${workload_api}_${workload_guard}
					PROPERTIES TIMEOUT 15 LABELS "graphics;gnm;workload;guest-memory")
			endforeach()
		endforeach()
	endif()
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		# Compile the production wrapper with only its ordinary submit callees
		# replaced by original recorders. The real registry and FEX ABI remain.
		target_compile_definitions(emulator PRIVATE LUMEN_PTHREAD_EXIT_TEST_HOOK)
		add_library(lumen_gnm_workload_forwarding_fixture OBJECT EXCLUDE_FROM_ALL
			emulator/src/Graphics/GraphicsWorkload.cpp)
		target_link_libraries(lumen_gnm_workload_forwarding_fixture PRIVATE emulator)
		target_compile_definitions(lumen_gnm_workload_forwarding_fixture PRIVATE
			GraphicsSubmitCommandBuffers=GnmTestSubmitCommandBuffers
			GraphicsSubmitAndFlipCommandBuffers=GnmTestSubmitAndFlipCommandBuffers)
		add_executable(lumen_gnm_workload_forwarding_test EXCLUDE_FROM_ALL
			GnmWorkloadForwardingTest.cpp $<TARGET_OBJECTS:lumen_gnm_workload_forwarding_fixture>)
		target_compile_definitions(lumen_gnm_workload_forwarding_test PRIVATE LUMEN_FEXCORE_RUNTIME=1 LUMEN_PTHREAD_EXIT_TEST_HOOK)
		target_link_libraries(lumen_gnm_workload_forwarding_test PRIVATE emulator)
		add_test(NAME lumen_gnm_workload_forwarding COMMAND $<TARGET_FILE:lumen_gnm_workload_forwarding_test>)
		set_tests_properties(lumen_gnm_workload_forwarding PROPERTIES TIMEOUT 30 LABELS "graphics;gnm;workload;fex;abi"
			PASS_REGULAR_EXPRESSION "GnmWorkloadForwardingTest: PASS")
	endif()
endif()

if(LUMEN_WITH_PTHREAD_MUTEX_TEST AND NOT ANDROID)
	find_package(Threads REQUIRED)
	add_executable(lumen_native_object_directory_test EXCLUDE_FROM_ALL NativeObjectDirectoryTest.cpp)
	target_include_directories(lumen_native_object_directory_test PRIVATE emulator/include)
	target_compile_features(lumen_native_object_directory_test PRIVATE cxx_std_20)
	target_link_libraries(lumen_native_object_directory_test PRIVATE Threads::Threads)
	add_test(NAME lumen_native_object_directory COMMAND lumen_native_object_directory_test)
	set_tests_properties(lumen_native_object_directory PROPERTIES TIMEOUT 20 LABELS "structural-candidate;correctness")
	target_compile_definitions(emulator PRIVATE LUMEN_PTHREAD_EXIT_TEST_HOOK)
	add_executable(lumen_pthread_mutex_test EXCLUDE_FROM_ALL PthreadMutexTest.cpp)
	target_compile_definitions(lumen_pthread_mutex_test PRIVATE LUMEN_PTHREAD_EXIT_TEST_HOOK)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_pthread_mutex_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	endif()
	target_link_libraries(lumen_pthread_mutex_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_pthread_mutex COMMAND $<TARGET_FILE:lumen_pthread_mutex_test>)
	set_tests_properties(lumen_pthread_mutex PROPERTIES
		TIMEOUT 15
		PASS_REGULAR_EXPRESSION "PthreadMutexTest: PASS")
	foreach(directory 0 1)
		add_test(NAME lumen_pthread_mutex_directory_${directory}
			COMMAND ${CMAKE_COMMAND} -E env LUMEN_PTHREAD_MUTEX_DIRECTORY=${directory}
				$<TARGET_FILE:lumen_pthread_mutex_test>)
		set_tests_properties(lumen_pthread_mutex_directory_${directory} PROPERTIES TIMEOUT 20
			PASS_REGULAR_EXPRESSION "PthreadMutexTest: PASS" LABELS "structural-candidate;correctness")
	endforeach()
	foreach(mutex_head_wake 0 1)
		add_test(NAME lumen_pthread_mutex_head_wake_${mutex_head_wake}
			COMMAND ${CMAKE_COMMAND} -E env LUMEN_PTHREAD_MUTEX_HEAD_WAKE=${mutex_head_wake}
				$<TARGET_FILE:lumen_pthread_mutex_test>)
		set_tests_properties(lumen_pthread_mutex_head_wake_${mutex_head_wake} PROPERTIES
			TIMEOUT 15 PASS_REGULAR_EXPRESSION "PthreadMutexTest: PASS")
	endforeach()
endif()

if(LUMEN_WITH_POSIX_SEMAPHORE_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	find_package(Threads REQUIRED)
	target_compile_definitions(emulator PRIVATE LUMEN_POSIX_SEMAPHORE_TEST_HOOK)
	add_executable(lumen_posix_semaphore_test EXCLUDE_FROM_ALL PosixSemaphoreTest.cpp)
	target_compile_definitions(lumen_posix_semaphore_test PRIVATE LUMEN_POSIX_SEMAPHORE_TEST_HOOK)
	target_link_libraries(lumen_posix_semaphore_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_posix_semaphore COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_POSIX_SEM_TIMEDWAIT_CARRY
		-- $<TARGET_FILE:lumen_posix_semaphore_test>)
	add_test(NAME lumen_posix_semaphore_carry COMMAND ${CMAKE_COMMAND} -E env
		LUMEN_EXPERIMENTAL_POSIX_SEM_TIMEDWAIT_CARRY=1
		-- $<TARGET_FILE:lumen_posix_semaphore_test>)
	set_tests_properties(lumen_posix_semaphore lumen_posix_semaphore_carry PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "PosixSemaphoreTest: PASS")
endif()

if(LUMEN_WITH_PTHREAD_CONDITION_TEST AND NOT ANDROID)
	find_package(Threads REQUIRED)
	add_executable(lumen_pthread_condition_test EXCLUDE_FROM_ALL PthreadConditionTest.cpp)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(emulator PRIVATE LUMEN_PTHREAD_CONDITION_TEST_HOOK)
		target_compile_definitions(lumen_pthread_condition_test PRIVATE LUMEN_PTHREAD_CONDITION_TEST_HOOK)
	endif()
	target_link_libraries(lumen_pthread_condition_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_pthread_condition COMMAND $<TARGET_FILE:lumen_pthread_condition_test>)
	set_tests_properties(lumen_pthread_condition PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "PthreadConditionTest: PASS")
endif()

if(LUMEN_WITH_PTHREAD_ONCE_TEST AND NOT ANDROID)
	find_package(Threads REQUIRED)
	add_executable(lumen_pthread_once_test EXCLUDE_FROM_ALL PthreadOnceTest.cpp)
	target_link_libraries(lumen_pthread_once_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_pthread_once COMMAND $<TARGET_FILE:lumen_pthread_once_test>)
	set_tests_properties(lumen_pthread_once PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "PthreadOnceTest: PASS")
endif()

if(LUMEN_WITH_VIDEOOUT_EVENT_DATA_TEST)
	add_executable(lumen_videoout_event_data_test EXCLUDE_FROM_ALL VideoOutEventDataTest.cpp)
	target_link_libraries(lumen_videoout_event_data_test PRIVATE emulator)
	add_test(NAME lumen_videoout_event_data_unset COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_EVENT_DATA -- $<TARGET_FILE:lumen_videoout_event_data_test> --disabled)
	set_tests_properties(lumen_videoout_event_data_unset PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "VideoOutEventDataTest: PASS")
	foreach(event_data_value IN ITEMS "0" "1" "true" "01")
		set(event_data_expected "--disabled")
		if(event_data_value STREQUAL "1")
			set(event_data_expected "--enabled")
		endif()
		add_test(NAME lumen_videoout_event_data_${event_data_value} COMMAND ${CMAKE_COMMAND} -E env
			"LUMEN_EXPERIMENTAL_VIDEOOUT_EVENT_DATA=${event_data_value}" -- $<TARGET_FILE:lumen_videoout_event_data_test> ${event_data_expected})
		set_tests_properties(lumen_videoout_event_data_${event_data_value} PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "VideoOutEventDataTest: PASS")
	endforeach()
endif()

if(LUMEN_WITH_VIDEOOUT_GAMMA_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	add_executable(lumen_videoout_gamma_test EXCLUDE_FROM_ALL VideoOutGammaTest.cpp)
	target_link_libraries(lumen_videoout_gamma_test PRIVATE emulator)
	add_test(NAME lumen_videoout_gamma COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_OPEN_PARAM_IGNORE
		--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_MODE_INIT
		--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_CONFIGURE_ANY_SDR
		-- $<TARGET_FILE:lumen_videoout_gamma_test>)
	set_tests_properties(lumen_videoout_gamma PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "VideoOutGammaTest: PASS")
	add_test(NAME lumen_videoout_open_param_unset COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_OPEN_PARAM_IGNORE
		--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_MODE_INIT
		--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_CONFIGURE_ANY_SDR
		-- $<TARGET_FILE:lumen_videoout_gamma_test> --open-param-ignore=disabled)
	set_tests_properties(lumen_videoout_open_param_unset PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "VideoOutOpenParamTest: PASS")
	foreach(open_param_case IN ITEMS enabled zero empty malformed leading_zero)
		set(open_param_expected disabled)
		if(open_param_case STREQUAL "enabled")
			set(open_param_value "1")
			set(open_param_expected enabled)
		elseif(open_param_case STREQUAL "zero")
			set(open_param_value "0")
		elseif(open_param_case STREQUAL "empty")
			set(open_param_value "")
		elseif(open_param_case STREQUAL "malformed")
			set(open_param_value "true")
		else()
			set(open_param_value "01")
		endif()
		add_test(NAME lumen_videoout_open_param_${open_param_case} COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_MODE_INIT
			--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_CONFIGURE_ANY_SDR
			"LUMEN_EXPERIMENTAL_VIDEOOUT_OPEN_PARAM_IGNORE=${open_param_value}"
			-- $<TARGET_FILE:lumen_videoout_gamma_test> --open-param-ignore=${open_param_expected})
		set_tests_properties(lumen_videoout_open_param_${open_param_case} PROPERTIES TIMEOUT 15
			PASS_REGULAR_EXPRESSION "VideoOutOpenParamTest: PASS")
	endforeach()
endif()

if(LUMEN_WITH_VIDEOOUT_GAMMA_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	add_executable(lumen_videoout_mode_init_test EXCLUDE_FROM_ALL VideoOutModeInitTest.cpp)
	target_link_libraries(lumen_videoout_mode_init_test PRIVATE emulator)
	foreach(mode_init_case IN ITEMS memory unset enabled zero empty malformed leading_zero)
		set(mode_init_argument --disabled)
		if(mode_init_case STREQUAL "memory")
			set(mode_init_argument --memory)
		elseif(mode_init_case STREQUAL "enabled")
			set(mode_init_argument --enabled)
		endif()
		if(mode_init_case STREQUAL "memory" OR mode_init_case STREQUAL "unset")
			set(mode_init_environment --unset=LUMEN_EXPERIMENTAL_VIDEOOUT_MODE_INIT)
		else()
			if(mode_init_case STREQUAL "enabled")
				set(mode_init_value "1")
			elseif(mode_init_case STREQUAL "zero")
				set(mode_init_value "0")
			elseif(mode_init_case STREQUAL "empty")
				set(mode_init_value "")
			elseif(mode_init_case STREQUAL "malformed")
				set(mode_init_value "true")
			else()
				set(mode_init_value "01")
			endif()
			set(mode_init_environment "LUMEN_EXPERIMENTAL_VIDEOOUT_MODE_INIT=${mode_init_value}")
		endif()
		add_test(NAME lumen_videoout_mode_init_${mode_init_case} COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_CONFIGURE_ANY_SDR
			--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_OPEN_PARAM_IGNORE "${mode_init_environment}"
			-- $<TARGET_FILE:lumen_videoout_mode_init_test> ${mode_init_argument})
		set_tests_properties(lumen_videoout_mode_init_${mode_init_case} PROPERTIES TIMEOUT 15
			PASS_REGULAR_EXPRESSION "VideoOutModeInitTest: PASS")
	endforeach()
endif()

if(LUMEN_WITH_VIDEOOUT_GAMMA_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	add_executable(lumen_videoout_configure_any_sdr_test EXCLUDE_FROM_ALL VideoOutConfigureAnySdrTest.cpp)
	target_link_libraries(lumen_videoout_configure_any_sdr_test PRIVATE emulator)
	add_executable(lumen_window_sdr_policy_test EXCLUDE_FROM_ALL WindowSdrPolicyTest.cpp)
	target_link_libraries(lumen_window_sdr_policy_test PRIVATE emulator)
	add_test(NAME lumen_window_sdr_policy COMMAND $<TARGET_FILE:lumen_window_sdr_policy_test>)
	set_tests_properties(lumen_window_sdr_policy PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "WindowSdrPolicyTest: PASS")
	foreach(configure_sdr_case IN ITEMS record unset enabled zero empty malformed leading_zero)
		set(configure_sdr_argument --disabled)
		if(configure_sdr_case STREQUAL "record")
			set(configure_sdr_argument --record)
		elseif(configure_sdr_case STREQUAL "enabled")
			set(configure_sdr_argument --enabled)
		endif()
		if(configure_sdr_case STREQUAL "record" OR configure_sdr_case STREQUAL "unset")
			set(configure_sdr_environment --unset=LUMEN_EXPERIMENTAL_VIDEOOUT_CONFIGURE_ANY_SDR)
		else()
			if(configure_sdr_case STREQUAL "enabled")
				set(configure_sdr_value "1")
			elseif(configure_sdr_case STREQUAL "zero")
				set(configure_sdr_value "0")
			elseif(configure_sdr_case STREQUAL "empty")
				set(configure_sdr_value "")
			elseif(configure_sdr_case STREQUAL "malformed")
				set(configure_sdr_value "true")
			else()
				set(configure_sdr_value "01")
			endif()
			set(configure_sdr_environment "LUMEN_EXPERIMENTAL_VIDEOOUT_CONFIGURE_ANY_SDR=${configure_sdr_value}")
		endif()
		add_test(NAME lumen_videoout_configure_any_sdr_${configure_sdr_case} COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_VIDEOOUT_OPEN_PARAM_IGNORE --unset=LUMEN_EXPERIMENTAL_VIDEOOUT_MODE_INIT
			"${configure_sdr_environment}" -- $<TARGET_FILE:lumen_videoout_configure_any_sdr_test> ${configure_sdr_argument})
		set_tests_properties(lumen_videoout_configure_any_sdr_${configure_sdr_case} PROPERTIES TIMEOUT 15
			PASS_REGULAR_EXPRESSION "VideoOutConfigureAnySdrTest: PASS")
	endforeach()
endif()

if(LUMEN_WITH_HOST_CALL_SCALAR_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_host_call_symbol_cache_test EXCLUDE_FROM_ALL HostCallSymbolCacheTest.cpp)
	target_include_directories(lumen_host_call_symbol_cache_test PRIVATE ${PROJECT_SOURCE_DIR}/emulator/include)
	target_link_libraries(lumen_host_call_symbol_cache_test PRIVATE Threads::Threads)
	add_test(NAME lumen_host_call_symbol_cache COMMAND $<TARGET_FILE:lumen_host_call_symbol_cache_test>)
	set_tests_properties(lumen_host_call_symbol_cache PROPERTIES TIMEOUT 15 LABELS "hle;host-call;diagnostics"
		PASS_REGULAR_EXPRESSION "HostCallSymbolCacheTest: PASS")
endif()

if(LUMEN_WITH_HOST_CALL_SCALAR_TEST AND NOT ANDROID)
	add_executable(lumen_host_call_scalar_metadata_test EXCLUDE_FROM_ALL HostCallScalarMetadataTest.cpp)
	target_link_libraries(lumen_host_call_scalar_metadata_test PRIVATE emulator)
	add_test(NAME lumen_host_call_scalar_metadata COMMAND $<TARGET_FILE:lumen_host_call_scalar_metadata_test>)
	set_tests_properties(lumen_host_call_scalar_metadata PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "HostCallScalarMetadataTest: PASS")
	foreach(index legacy fast)
		add_test(NAME lumen_host_call_metadata_${index}_index COMMAND $<TARGET_FILE:lumen_host_call_scalar_metadata_test>
			--${index}-index)
		set_tests_properties(lumen_host_call_metadata_${index}_index PROPERTIES TIMEOUT 15
			PASS_REGULAR_EXPRESSION "HostCallScalarMetadataTest: PASS" LABELS "structural-candidate;correctness")
	endforeach()
endif()

if(LUMEN_WITH_HOST_CALL_SCALAR_TEST AND LUMEN_MACOS_FEXCORE_RUNTIME)
	target_compile_definitions(emulator PRIVATE LUMEN_PTHREAD_EXIT_TEST_HOOK)
	add_executable(lumen_host_call_scalar_test EXCLUDE_FROM_ALL HostCallScalarTest.cpp)
	target_compile_definitions(lumen_host_call_scalar_test PRIVATE
		LUMEN_PTHREAD_EXIT_TEST_HOOK LUMEN_FEXCORE_RUNTIME=1)
	target_link_libraries(lumen_host_call_scalar_test PRIVATE emulator)
	add_test(NAME lumen_host_call_scalar COMMAND $<TARGET_FILE:lumen_host_call_scalar_test>)
	set_tests_properties(lumen_host_call_scalar PROPERTIES TIMEOUT 45
		PASS_REGULAR_EXPRESSION "HostCallScalarTest: PASS" ENVIRONMENT "LUMEN_CPU_TRANSLATOR=fex")
	foreach(index 0 1)
		add_test(NAME lumen_host_call_fast_index_${index} COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_CPU_TRANSLATOR=fex LUMEN_FEX_HOST_CALL_FAST_INDEX=${index}
			$<TARGET_FILE:lumen_host_call_scalar_test>)
		set_tests_properties(lumen_host_call_fast_index_${index} PROPERTIES TIMEOUT 45
			PASS_REGULAR_EXPRESSION "HostCallScalarTest: PASS" LABELS "structural-candidate;correctness")
	endforeach()
	add_test(NAME lumen_host_call_scalar_short_admission COMMAND "${CMAKE_COMMAND}" -E env
		LUMEN_CPU_TRANSLATOR=fex LUMEN_FEX_HLE_SHORT_ADMISSION=1
		$<TARGET_FILE:lumen_host_call_scalar_test>)
	set_tests_properties(lumen_host_call_scalar_short_admission PROPERTIES TIMEOUT 45
		PASS_REGULAR_EXPRESSION "HostCallScalarTest: PASS" LABELS "cpu;hle;vm")
	if(TARGET lumen_qemu_tcg_core)
		add_test(NAME lumen_qemu_host_call_scalar COMMAND "${CMAKE_COMMAND}" -E env
			LUMEN_CPU_TRANSLATOR=qemu-tcg $<TARGET_FILE:lumen_host_call_scalar_test>)
		set_tests_properties(lumen_qemu_host_call_scalar PROPERTIES TIMEOUT 45
			PASS_REGULAR_EXPRESSION "HostCallScalarTest: PASS" LABELS "macos;qemu;host-call;ps4")
	endif()
	foreach(owned_stack_read IN ITEMS disabled enabled)
		if(owned_stack_read STREQUAL "enabled")
			set(owned_stack_read_value 1)
		else()
			set(owned_stack_read_value 0)
		endif()
		add_test(NAME lumen_host_call_scalar_owned_stack_read_${owned_stack_read}
			COMMAND "${CMAKE_COMMAND}" -E env
				LUMEN_CPU_TRANSLATOR=fex
				"LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ=${owned_stack_read_value}"
				$<TARGET_FILE:lumen_host_call_scalar_test> --owned-stack-read-${owned_stack_read})
		set_tests_properties(lumen_host_call_scalar_owned_stack_read_${owned_stack_read} PROPERTIES
			TIMEOUT 45 PASS_REGULAR_EXPRESSION "HostCallScalarTest: PASS"
			LABELS "macos;fexcore;host-call;owned-stack")
	endforeach()
	foreach(owned_stack_read IN ITEMS default empty malformed)
		if(owned_stack_read STREQUAL "default")
			set(owned_stack_selector --unset=LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ)
			set(owned_stack_expected enabled)
		elseif(owned_stack_read STREQUAL "empty")
			set(owned_stack_selector LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ=)
			set(owned_stack_expected disabled)
		else()
			set(owned_stack_selector LUMEN_EXPERIMENTAL_FEX_HLE_OWNED_STACK_READ=01)
			set(owned_stack_expected disabled)
		endif()
		add_test(NAME lumen_host_call_scalar_owned_stack_read_${owned_stack_read}
			COMMAND "${CMAKE_COMMAND}" -E env ${owned_stack_selector}
				LUMEN_CPU_TRANSLATOR=fex
				$<TARGET_FILE:lumen_host_call_scalar_test> --owned-stack-read-${owned_stack_expected})
		set_tests_properties(lumen_host_call_scalar_owned_stack_read_${owned_stack_read} PROPERTIES
			TIMEOUT 45 PASS_REGULAR_EXPRESSION "HostCallScalarTest: PASS"
			LABELS "macos;fexcore;host-call;owned-stack;selector")
	endforeach()
	add_executable(lumen_libc_guest_memory_test EXCLUDE_FROM_ALL LibCGuestMemoryTest.cpp)
	target_compile_definitions(lumen_libc_guest_memory_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	target_link_libraries(lumen_libc_guest_memory_test PRIVATE emulator)
	add_test(NAME lumen_libc_guest_memory COMMAND $<TARGET_FILE:lumen_libc_guest_memory_test>)
	set_tests_properties(lumen_libc_guest_memory PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "LibCGuestMemoryTest: PASS")
	foreach(comparison_selectors default strings memory both zero empty malformed)
		set(string_compare_expected 0)
		set(memcmp_expected 0)
		if(comparison_selectors STREQUAL "default")
			set(string_compare_expected 1)
			set(memcmp_expected 1)
			set(comparison_environment
				--unset=LUMEN_EXPERIMENTAL_FEX_LIBC_STRING_COMPARE_GUEST
				--unset=LUMEN_EXPERIMENTAL_FEX_LIBC_MEMCMP_GUEST)
		elseif(comparison_selectors STREQUAL "empty")
			set(comparison_environment LUMEN_EXPERIMENTAL_FEX_LIBC_STRING_COMPARE_GUEST=
				LUMEN_EXPERIMENTAL_FEX_LIBC_MEMCMP_GUEST=)
		elseif(comparison_selectors STREQUAL "malformed")
			set(comparison_environment LUMEN_EXPERIMENTAL_FEX_LIBC_STRING_COMPARE_GUEST=01
				LUMEN_EXPERIMENTAL_FEX_LIBC_MEMCMP_GUEST=true)
		else()
			if(comparison_selectors STREQUAL "strings" OR comparison_selectors STREQUAL "both")
				set(string_compare_expected 1)
			endif()
			if(comparison_selectors STREQUAL "memory" OR comparison_selectors STREQUAL "both")
				set(memcmp_expected 1)
			endif()
			set(comparison_environment
				LUMEN_EXPERIMENTAL_FEX_LIBC_STRING_COMPARE_GUEST=${string_compare_expected}
				LUMEN_EXPERIMENTAL_FEX_LIBC_MEMCMP_GUEST=${memcmp_expected})
		endif()
		add_test(NAME lumen_libc_guest_memory_compare_${comparison_selectors}
			COMMAND "${CMAKE_COMMAND}" -E env ${comparison_environment}
				LUMEN_EXPECT_FEX_LIBC_STRING_COMPARE_GUEST=${string_compare_expected}
				LUMEN_EXPECT_FEX_LIBC_MEMCMP_GUEST=${memcmp_expected}
				$<TARGET_FILE:lumen_libc_guest_memory_test>)
		set_tests_properties(lumen_libc_guest_memory_compare_${comparison_selectors} PROPERTIES
			TIMEOUT 30 PASS_REGULAR_EXPRESSION "LibCGuestMemoryTest: PASS"
			LABELS "macos;fexcore;libc;selector")
	endforeach()
	add_executable(lumen_filesystem_guest_memory_test EXCLUDE_FROM_ALL FileSystemGuestMemoryTest.cpp)
	target_compile_definitions(lumen_filesystem_guest_memory_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	target_link_libraries(lumen_filesystem_guest_memory_test PRIVATE emulator)
	add_test(NAME lumen_filesystem_guest_memory COMMAND $<TARGET_FILE:lumen_filesystem_guest_memory_test>)
	set_tests_properties(lumen_filesystem_guest_memory PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "FileSystemGuestMemoryTest: PASS")
endif()

if(LUMEN_WITH_PTHREAD_EXIT_TEST AND LUMEN_MACOS_FEXCORE_RUNTIME)
	find_package(Threads REQUIRED)
	target_compile_definitions(emulator PRIVATE LUMEN_PTHREAD_EXIT_TEST_HOOK)
	add_executable(lumen_pthread_exit_test EXCLUDE_FROM_ALL PthreadExitTest.cpp)
	target_compile_definitions(lumen_pthread_exit_test PRIVATE LUMEN_PTHREAD_EXIT_TEST_HOOK LUMEN_FEXCORE_RUNTIME=1)
	target_link_libraries(lumen_pthread_exit_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_pthread_exit COMMAND $<TARGET_FILE:lumen_pthread_exit_test>)
	set_tests_properties(lumen_pthread_exit PROPERTIES TIMEOUT 45
		PASS_REGULAR_EXPRESSION "PthreadExitTest: PASS")
endif()

if(LUMEN_WITH_PTHREAD_EXCEPTION_TEST AND LUMEN_MACOS_FEXCORE_RUNTIME)
	find_package(Threads REQUIRED)
	# Test-only synthetic TLS owner and native-sleep slice rendezvous.
	target_compile_definitions(emulator PRIVATE LUMEN_PTHREAD_EXIT_TEST_HOOK)
	add_executable(lumen_pthread_exception_test EXCLUDE_FROM_ALL PthreadExceptionDeliveryTest.cpp)
	target_compile_definitions(lumen_pthread_exception_test PRIVATE
		LUMEN_FEXCORE_RUNTIME=1 LUMEN_PTHREAD_EXIT_TEST_HOOK)
	target_link_libraries(lumen_pthread_exception_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_pthread_exception COMMAND $<TARGET_FILE:lumen_pthread_exception_test>)
	set_tests_properties(lumen_pthread_exception PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "PthreadExceptionDeliveryTest: PASS")
	add_test(NAME lumen_pthread_exception_short_admission COMMAND "${CMAKE_COMMAND}" -E env
		LUMEN_FEX_HLE_SHORT_ADMISSION=1 $<TARGET_FILE:lumen_pthread_exception_test>)
	set_tests_properties(lumen_pthread_exception_short_admission PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "PthreadExceptionDeliveryTest: PASS")
	add_test(NAME lumen_pthread_exception_sleep_retain COMMAND "${CMAKE_COMMAND}" -E env
		LUMEN_FEX_SLEEP_RETAIN_HOST_WAIT=1 $<TARGET_FILE:lumen_pthread_exception_test>)
	set_tests_properties(lumen_pthread_exception_sleep_retain PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "PthreadExceptionDeliveryTest: PASS")
endif()

if(LUMEN_WITH_PTHREAD_AFFINITY_TEST AND NOT ANDROID)
	find_package(Threads REQUIRED)
	target_compile_definitions(emulator PRIVATE LUMEN_PTHREAD_ATTRIBUTE_TEST_HOOK)
	add_executable(lumen_pthread_affinity_test EXCLUDE_FROM_ALL PthreadAffinityTest.cpp)
	target_compile_definitions(lumen_pthread_affinity_test PRIVATE LUMEN_PTHREAD_ATTRIBUTE_TEST_HOOK)
	target_link_libraries(lumen_pthread_affinity_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_pthread_affinity COMMAND $<TARGET_FILE:lumen_pthread_affinity_test>)
	set_tests_properties(lumen_pthread_affinity PROPERTIES
		TIMEOUT 15
		PASS_REGULAR_EXPRESSION "PthreadAffinityTest: PASS")
endif()

if(LUMEN_WITH_LOG_FILE_SHUTDOWN_TEST AND NOT ANDROID)
	find_package(Threads REQUIRED)
	add_executable(lumen_log_file_shutdown_test EXCLUDE_FROM_ALL LogFileShutdownTest.cpp)
	target_link_libraries(lumen_log_file_shutdown_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_log_file_shutdown COMMAND $<TARGET_FILE:lumen_log_file_shutdown_test>)
	set_tests_properties(lumen_log_file_shutdown PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "LogFileShutdownTest: PASS")
endif()

if(LUMEN_WITH_KERNEL_USER_EVENT_TEST AND NOT ANDROID)
	find_package(Threads REQUIRED)
	target_compile_definitions(emulator PRIVATE LUMEN_KERNEL_USER_EVENT_TEST_HOOK)
	add_executable(lumen_kernel_user_event_test EXCLUDE_FROM_ALL KernelUserEventTest.cpp)
	target_compile_definitions(lumen_kernel_user_event_test PRIVATE LUMEN_KERNEL_USER_EVENT_TEST_HOOK)
	target_link_libraries(lumen_kernel_user_event_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_kernel_user_event COMMAND $<TARGET_FILE:lumen_kernel_user_event_test>)
	set_tests_properties(lumen_kernel_user_event PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "KernelUserEventTest: PASS")
endif()

if(LUMEN_WITH_KERNEL_EVENT_FLAG_TEST AND NOT ANDROID)
	find_package(Threads REQUIRED)
	target_compile_definitions(emulator PRIVATE LUMEN_KERNEL_EVENT_FLAG_TEST_HOOK)
	add_executable(lumen_kernel_event_flag_test EXCLUDE_FROM_ALL KernelEventFlagTest.cpp)
	target_compile_definitions(lumen_kernel_event_flag_test PRIVATE LUMEN_KERNEL_EVENT_FLAG_TEST_HOOK)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_kernel_event_flag_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	endif()
	target_link_libraries(lumen_kernel_event_flag_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_kernel_event_flag COMMAND $<TARGET_FILE:lumen_kernel_event_flag_test>)
	set_tests_properties(lumen_kernel_event_flag PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "KernelEventFlagTest: PASS")
endif()

if(LUMEN_WITH_KERNEL_EVENT_FLAG_TEST AND LUMEN_MACOS_FEXCORE_RUNTIME)
	find_package(Threads REQUIRED)
	add_executable(lumen_host_wait_sleep_test EXCLUDE_FROM_ALL HostWaitSleepTest.cpp)
	target_link_libraries(lumen_host_wait_sleep_test PRIVATE Threads::Threads)
	add_test(NAME lumen_host_wait_sleep COMMAND $<TARGET_FILE:lumen_host_wait_sleep_test>)
	set_tests_properties(lumen_host_wait_sleep PROPERTIES TIMEOUT 10
		PASS_REGULAR_EXPRESSION "HostWaitSleepTest: PASS")
	add_executable(lumen_kernel_sleep_fex_test EXCLUDE_FROM_ALL KernelSleepFexTest.cpp)
	target_compile_definitions(lumen_kernel_sleep_fex_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	target_link_libraries(lumen_kernel_sleep_fex_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_kernel_sleep_fex COMMAND $<TARGET_FILE:lumen_kernel_sleep_fex_test>)
	set_tests_properties(lumen_kernel_sleep_fex PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "KernelSleepFexTest: PASS")
	add_test(NAME lumen_kernel_sleep_fex_retain COMMAND "${CMAKE_COMMAND}" -E env
		LUMEN_FEX_SLEEP_RETAIN_HOST_WAIT=1 $<TARGET_FILE:lumen_kernel_sleep_fex_test>)
	set_tests_properties(lumen_kernel_sleep_fex_retain PROPERTIES TIMEOUT 15
		PASS_REGULAR_EXPRESSION "KernelSleepFexTest: PASS")
endif()

if(LUMEN_WITH_PNG_DEC_TEST)
	add_executable(lumen_png_dec_test EXCLUDE_FROM_ALL PngDecTest.cpp)
	target_link_libraries(lumen_png_dec_test PRIVATE emulator)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_png_dec_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	endif()
	add_test(NAME lumen_png_dec COMMAND $<TARGET_FILE:lumen_png_dec_test>)
	set_tests_properties(lumen_png_dec PROPERTIES TIMEOUT 15)
endif()

if(LUMEN_WITH_PNG_ENC_TEST)
	find_package(Threads REQUIRED)
	target_compile_definitions(core PRIVATE LUMEN_ZLIB_TEST_HOOK=1)
	target_compile_definitions(emulator PRIVATE LUMEN_PNG_ENC_TEST_HOOK=1)
	add_executable(lumen_png_enc_test EXCLUDE_FROM_ALL PngEncTest.cpp)
	target_compile_definitions(lumen_png_enc_test PRIVATE
		LUMEN_ZLIB_TEST_HOOK=1 LUMEN_PNG_ENC_TEST_HOOK=1)
	target_link_libraries(lumen_png_enc_test PRIVATE emulator Threads::Threads)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_png_enc_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	endif()
	add_test(NAME lumen_png_enc COMMAND $<TARGET_FILE:lumen_png_enc_test>)
	set_tests_properties(lumen_png_enc PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "PngEncTest: PASS")
endif()

if(LUMEN_WITH_REMOTEPLAY_TEST AND MACOS)
	find_package(Threads REQUIRED)
	add_executable(lumen_remoteplay_test EXCLUDE_FROM_ALL RemoteplayTest.cpp)
	target_link_libraries(lumen_remoteplay_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_remoteplay_default COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE $<TARGET_FILE:lumen_remoteplay_test> disabled)
	foreach(case IN ITEMS zero empty malformed whitespace)
		if(case STREQUAL "zero")
			set(remoteplay_value "0")
		elseif(case STREQUAL "empty")
			set(remoteplay_value "")
		elseif(case STREQUAL "malformed")
			set(remoteplay_value "01")
		else()
			set(remoteplay_value "1 ")
		endif()
		add_test(NAME lumen_remoteplay_${case} COMMAND ${CMAKE_COMMAND} -E env
			"LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE=${remoteplay_value}"
			$<TARGET_FILE:lumen_remoteplay_test> disabled)
	endforeach()
	add_test(NAME lumen_remoteplay_enabled COMMAND ${CMAKE_COMMAND} -E env
		LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE=1 $<TARGET_FILE:lumen_remoteplay_test> enabled)
	foreach(value IN ITEMS 0 1)
		add_test(NAME lumen_remoteplay_latch_${value} COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE=${value} $<TARGET_FILE:lumen_remoteplay_test> latch ${value})
	endforeach()
	set_tests_properties(lumen_remoteplay_default lumen_remoteplay_zero lumen_remoteplay_empty
		lumen_remoteplay_malformed lumen_remoteplay_whitespace lumen_remoteplay_enabled
		lumen_remoteplay_latch_0 lumen_remoteplay_latch_1 PROPERTIES TIMEOUT 15)
endif()

if(LUMEN_WITH_NP_PLUS_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	find_package(Threads REQUIRED)
	add_executable(lumen_np_plus_test EXCLUDE_FROM_ALL NpPlusTest.cpp)
	target_link_libraries(lumen_np_plus_test PRIVATE emulator Threads::Threads)
	add_test(NAME lumen_np_plus_default COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		$<TARGET_FILE:lumen_np_plus_test> disabled)
	foreach(case IN ITEMS zero empty malformed whitespace)
		if(case STREQUAL "zero")
			set(np_plus_value "0")
		elseif(case STREQUAL "empty")
			set(np_plus_value "")
		elseif(case STREQUAL "malformed")
			set(np_plus_value "01")
		else()
			set(np_plus_value "1 ")
		endif()
		add_test(NAME lumen_np_plus_${case} COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
			--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
			"LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=${np_plus_value}"
			$<TARGET_FILE:lumen_np_plus_test> disabled)
	endforeach()
	add_test(NAME lumen_np_plus_enabled COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=1
		$<TARGET_FILE:lumen_np_plus_test> enabled)
	foreach(value IN ITEMS 0 1)
		add_test(NAME lumen_np_plus_latch_${value} COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
			--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
			"LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=${value}"
			$<TARGET_FILE:lumen_np_plus_test> latch ${value})
	endforeach()
	add_test(NAME lumen_np_plus_fatal_paths COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=1
		$<TARGET_FILE:lumen_np_plus_test> fatal-paths)
	set_tests_properties(lumen_np_plus_default lumen_np_plus_zero lumen_np_plus_empty
		lumen_np_plus_malformed lumen_np_plus_whitespace lumen_np_plus_enabled
		lumen_np_plus_latch_0 lumen_np_plus_latch_1 lumen_np_plus_fatal_paths PROPERTIES
		TIMEOUT 20 PASS_REGULAR_EXPRESSION "NpPlusTest: PASS")
endif()

if(LUMEN_WITH_NP_PARENTAL_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	find_package(Threads REQUIRED)
	add_executable(lumen_np_parental_test EXCLUDE_FROM_ALL NpParentalTest.cpp)
	target_link_libraries(lumen_np_parental_test PRIVATE emulator Threads::Threads)
	set(np_parental_tests lumen_np_parental_default lumen_np_parental_memory lumen_np_parental_fatal_paths)
	add_test(NAME lumen_np_parental_default COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE --unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE $<TARGET_FILE:lumen_np_parental_test> matrix 0 0)
	foreach(plus IN ITEMS 0 1)
		foreach(parental IN ITEMS 0 1)
			set(matrix_test "lumen_np_parental_matrix_${plus}_${parental}")
			list(APPEND np_parental_tests ${matrix_test})
			add_test(NAME ${matrix_test} COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
				--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE "LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=${plus}"
				"LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=${parental}" $<TARGET_FILE:lumen_np_parental_test> matrix ${plus} ${parental})
			foreach(first IN ITEMS plus parental create delete)
				set(latch_test "lumen_np_parental_latch_${plus}_${parental}_${first}")
				list(APPEND np_parental_tests ${latch_test})
				add_test(NAME ${latch_test} COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
					--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE "LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=${plus}"
					"LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=${parental}"
					$<TARGET_FILE:lumen_np_parental_test> latch ${plus} ${parental} ${first})
			endforeach()
		endforeach()
		foreach(case IN ITEMS empty malformed whitespace)
			if(case STREQUAL "empty")
				set(parental_value "")
			elseif(case STREQUAL "malformed")
				set(parental_value "01")
			else()
				set(parental_value "1 ")
			endif()
			set(malformed_test "lumen_np_parental_${case}_plus_${plus}")
			list(APPEND np_parental_tests ${malformed_test})
			add_test(NAME ${malformed_test} COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
				--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE "LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=${plus}"
				"LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=${parental_value}" $<TARGET_FILE:lumen_np_parental_test> matrix ${plus} 0)
		endforeach()
	endforeach()
	add_test(NAME lumen_np_parental_memory COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=0
		LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=1 $<TARGET_FILE:lumen_np_parental_test> memory)
	add_test(NAME lumen_np_parental_fatal_paths COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE --unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=0
		LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=1 $<TARGET_FILE:lumen_np_parental_test> fatal-paths)
	set_tests_properties(${np_parental_tests} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "NpParentalTest: PASS")
endif()

if(LUMEN_WITH_NP_IN_GAME_MESSAGE_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	find_package(Threads REQUIRED)
	add_executable(lumen_np_in_game_message_test EXCLUDE_FROM_ALL NpInGameMessageTest.cpp)
	target_link_libraries(lumen_np_in_game_message_test PRIVATE emulator Threads::Threads)
	set(np_message_clean_env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE)
	set(np_message_tests lumen_np_in_game_message_default lumen_np_in_game_message_enabled
		lumen_np_in_game_message_coexist lumen_np_in_game_message_fatal_paths)
	add_test(NAME lumen_np_in_game_message_default COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE ${np_message_clean_env}
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE $<TARGET_FILE:lumen_np_in_game_message_test> disabled)
	foreach(case IN ITEMS zero empty malformed whitespace)
		if(case STREQUAL "zero")
			set(np_message_value "0")
		elseif(case STREQUAL "empty")
			set(np_message_value "")
		elseif(case STREQUAL "malformed")
			set(np_message_value "01")
		else()
			set(np_message_value "1 ")
		endif()
		list(APPEND np_message_tests lumen_np_in_game_message_${case})
		add_test(NAME lumen_np_in_game_message_${case} COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE ${np_message_clean_env}
			"LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE=${np_message_value}" $<TARGET_FILE:lumen_np_in_game_message_test> disabled)
	endforeach()
	add_test(NAME lumen_np_in_game_message_enabled COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE ${np_message_clean_env}
		LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE=1 $<TARGET_FILE:lumen_np_in_game_message_test> enabled)
	foreach(value IN ITEMS 0 1)
		list(APPEND np_message_tests lumen_np_in_game_message_latch_${value})
		add_test(NAME lumen_np_in_game_message_latch_${value} COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE ${np_message_clean_env}
			"LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE=${value}" $<TARGET_FILE:lumen_np_in_game_message_test> latch ${value})
	endforeach()
	add_test(NAME lumen_np_in_game_message_coexist COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE=1 LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=1
		LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=1 $<TARGET_FILE:lumen_np_in_game_message_test> coexist)
	add_test(NAME lumen_np_in_game_message_fatal_paths COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE ${np_message_clean_env}
		LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE=1 $<TARGET_FILE:lumen_np_in_game_message_test> fatal-paths)
	set_tests_properties(${np_message_tests} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "NpInGameMessageTest: PASS")
endif()

if(LUMEN_WITH_RUDP_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	find_package(Threads REQUIRED)
	add_executable(lumen_rudp_test EXCLUDE_FROM_ALL RudpTest.cpp)
	target_link_libraries(lumen_rudp_test PRIVATE emulator Threads::Threads)
	set(rudp_clean_env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE)
	set(rudp_tests lumen_rudp_default lumen_rudp_enabled lumen_rudp_coexist lumen_rudp_fatal_paths)
	add_test(NAME lumen_rudp_default COMMAND ${CMAKE_COMMAND} -E env ${rudp_clean_env}
		--unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE $<TARGET_FILE:lumen_rudp_test> disabled)
	foreach(case IN ITEMS zero empty malformed whitespace)
		if(case STREQUAL "zero")
			set(rudp_value "0")
		elseif(case STREQUAL "empty")
			set(rudp_value "")
		elseif(case STREQUAL "malformed")
			set(rudp_value "01")
		else()
			set(rudp_value "1 ")
		endif()
		list(APPEND rudp_tests lumen_rudp_${case})
		add_test(NAME lumen_rudp_${case} COMMAND ${CMAKE_COMMAND} -E env ${rudp_clean_env}
			"LUMEN_EXPERIMENTAL_RUDP_OFFLINE=${rudp_value}" $<TARGET_FILE:lumen_rudp_test> disabled)
	endforeach()
	add_test(NAME lumen_rudp_enabled COMMAND ${CMAKE_COMMAND} -E env ${rudp_clean_env}
		LUMEN_EXPERIMENTAL_RUDP_OFFLINE=1 $<TARGET_FILE:lumen_rudp_test> enabled)
	foreach(value IN ITEMS 0 1)
		list(APPEND rudp_tests lumen_rudp_latch_${value})
		add_test(NAME lumen_rudp_latch_${value} COMMAND ${CMAKE_COMMAND} -E env ${rudp_clean_env}
			"LUMEN_EXPERIMENTAL_RUDP_OFFLINE=${value}" $<TARGET_FILE:lumen_rudp_test> latch ${value})
	endforeach()
	add_test(NAME lumen_rudp_coexist COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		LUMEN_EXPERIMENTAL_RUDP_OFFLINE=1 LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=1
		LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=1 LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE=1
		$<TARGET_FILE:lumen_rudp_test> coexist)
	add_test(NAME lumen_rudp_fatal_paths COMMAND ${CMAKE_COMMAND} -E env ${rudp_clean_env}
		LUMEN_EXPERIMENTAL_RUDP_OFFLINE=1 $<TARGET_FILE:lumen_rudp_test> fatal-paths)
	set_tests_properties(${rudp_tests} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "RudpTest: PASS")
endif()

if(LUMEN_WITH_VOICE_TEST AND MACOS AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
	find_package(Threads REQUIRED)
	add_executable(lumen_voice_test EXCLUDE_FROM_ALL VoiceTest.cpp)
	target_link_libraries(lumen_voice_test PRIVATE emulator Threads::Threads)
	set(voice_clean_env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO
		--unset=LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS
		--unset=LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER)
	set(voice_tests lumen_voice_default lumen_voice_enabled lumen_voice_coexist lumen_voice_fatal_paths)
	add_test(NAME lumen_voice_default COMMAND ${CMAKE_COMMAND} -E env ${voice_clean_env}
		--unset=LUMEN_EXPERIMENTAL_VOICE_OFFLINE $<TARGET_FILE:lumen_voice_test> disabled)
	foreach(case IN ITEMS zero empty malformed whitespace)
		if(case STREQUAL "zero")
			set(voice_value "0")
		elseif(case STREQUAL "empty")
			set(voice_value "")
		elseif(case STREQUAL "malformed")
			set(voice_value "01")
		else()
			set(voice_value "1 ")
		endif()
		list(APPEND voice_tests lumen_voice_${case})
		add_test(NAME lumen_voice_${case} COMMAND ${CMAKE_COMMAND} -E env ${voice_clean_env}
			"LUMEN_EXPERIMENTAL_VOICE_OFFLINE=${voice_value}" $<TARGET_FILE:lumen_voice_test> disabled)
	endforeach()
	add_test(NAME lumen_voice_enabled COMMAND ${CMAKE_COMMAND} -E env ${voice_clean_env}
		LUMEN_EXPERIMENTAL_VOICE_OFFLINE=1 $<TARGET_FILE:lumen_voice_test> enabled)
	foreach(value IN ITEMS 0 1)
		list(APPEND voice_tests lumen_voice_latch_${value})
		add_test(NAME lumen_voice_latch_${value} COMMAND ${CMAKE_COMMAND} -E env ${voice_clean_env}
			"LUMEN_EXPERIMENTAL_VOICE_OFFLINE=${value}" $<TARGET_FILE:lumen_voice_test> latch ${value})
	endforeach()
	add_test(NAME lumen_voice_coexist COMMAND ${CMAKE_COMMAND} -E env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO
		--unset=LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS
		--unset=LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER
		LUMEN_EXPERIMENTAL_VOICE_OFFLINE=1 LUMEN_EXPERIMENTAL_RUDP_OFFLINE=1
		LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE=1 LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE=1
		LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE=1 $<TARGET_FILE:lumen_voice_test> coexist)
	add_test(NAME lumen_voice_fatal_paths COMMAND ${CMAKE_COMMAND} -E env ${voice_clean_env}
		LUMEN_EXPERIMENTAL_VOICE_OFFLINE=1 $<TARGET_FILE:lumen_voice_test> fatal-paths)
	set_tests_properties(${voice_tests} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "VoiceTest: PASS")

	set(ngs2_waveform_clean_env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_VOICE_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS
		--unset=LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER)
	set(ngs2_waveform_tests
		lumen_ngs2_waveform_default lumen_ngs2_waveform_enabled lumen_ngs2_waveform_fatal_paths)
	add_test(NAME lumen_ngs2_waveform_default COMMAND ${CMAKE_COMMAND} -E env ${ngs2_waveform_clean_env}
		--unset=LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO $<TARGET_FILE:lumen_voice_test> ngs2-disabled)
	foreach(case IN ITEMS zero empty malformed whitespace two negative text leading_whitespace)
		if(case STREQUAL "zero")
			set(ngs2_waveform_value "0")
		elseif(case STREQUAL "empty")
			set(ngs2_waveform_value "")
		elseif(case STREQUAL "malformed")
			set(ngs2_waveform_value "01")
		elseif(case STREQUAL "whitespace")
			set(ngs2_waveform_value "1 ")
		elseif(case STREQUAL "two")
			set(ngs2_waveform_value "2")
		elseif(case STREQUAL "negative")
			set(ngs2_waveform_value "-1")
		elseif(case STREQUAL "text")
			set(ngs2_waveform_value "enabled")
		else()
			set(ngs2_waveform_value " 1")
		endif()
		list(APPEND ngs2_waveform_tests lumen_ngs2_waveform_${case})
		add_test(NAME lumen_ngs2_waveform_${case} COMMAND ${CMAKE_COMMAND} -E env ${ngs2_waveform_clean_env}
			"LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO=${ngs2_waveform_value}"
			$<TARGET_FILE:lumen_voice_test> ngs2-disabled)
	endforeach()
	add_test(NAME lumen_ngs2_waveform_enabled COMMAND ${CMAKE_COMMAND} -E env ${ngs2_waveform_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO=1 $<TARGET_FILE:lumen_voice_test> ngs2-enabled)
	foreach(value IN ITEMS 0 1)
		list(APPEND ngs2_waveform_tests lumen_ngs2_waveform_latch_${value})
		add_test(NAME lumen_ngs2_waveform_latch_${value} COMMAND ${CMAKE_COMMAND} -E env ${ngs2_waveform_clean_env}
			"LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO=${value}"
			$<TARGET_FILE:lumen_voice_test> ngs2-latch ${value})
	endforeach()
	add_test(NAME lumen_ngs2_waveform_fatal_paths COMMAND ${CMAKE_COMMAND} -E env ${ngs2_waveform_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO=1 $<TARGET_FILE:lumen_voice_test> ngs2-fatal-paths)
	set_tests_properties(${ngs2_waveform_tests} PROPERTIES TIMEOUT 20 PASS_REGULAR_EXPRESSION "VoiceTest: PASS")

	set(ngs2_state_flags_clean_env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_VOICE_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO
		--unset=LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER)
	set(ngs2_state_flags_tests
		lumen_ngs2_voice_state_flags_default
		lumen_ngs2_voice_state_flags_enabled
		lumen_ngs2_voice_state_flags_independence_waveform
		lumen_ngs2_voice_state_flags_independence_state
		lumen_ngs2_voice_state_flags_fatal_paths)
	add_test(NAME lumen_ngs2_voice_state_flags_default COMMAND ${CMAKE_COMMAND} -E env ${ngs2_state_flags_clean_env}
		--unset=LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS
		$<TARGET_FILE:lumen_voice_test> ngs2-state-disabled)
	foreach(case IN ITEMS zero empty malformed whitespace two negative text leading_whitespace)
		if(case STREQUAL "zero")
			set(ngs2_state_flags_value "0")
		elseif(case STREQUAL "empty")
			set(ngs2_state_flags_value "")
		elseif(case STREQUAL "malformed")
			set(ngs2_state_flags_value "01")
		elseif(case STREQUAL "whitespace")
			set(ngs2_state_flags_value "1 ")
		elseif(case STREQUAL "two")
			set(ngs2_state_flags_value "2")
		elseif(case STREQUAL "negative")
			set(ngs2_state_flags_value "-1")
		elseif(case STREQUAL "text")
			set(ngs2_state_flags_value "enabled")
		else()
			set(ngs2_state_flags_value " 1")
		endif()
		list(APPEND ngs2_state_flags_tests lumen_ngs2_voice_state_flags_${case})
		add_test(NAME lumen_ngs2_voice_state_flags_${case} COMMAND ${CMAKE_COMMAND} -E env ${ngs2_state_flags_clean_env}
			"LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS=${ngs2_state_flags_value}"
			$<TARGET_FILE:lumen_voice_test> ngs2-state-disabled)
	endforeach()
	add_test(NAME lumen_ngs2_voice_state_flags_enabled COMMAND ${CMAKE_COMMAND} -E env ${ngs2_state_flags_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS=1
		$<TARGET_FILE:lumen_voice_test> ngs2-state-enabled)
	foreach(value IN ITEMS 0 1)
		list(APPEND ngs2_state_flags_tests lumen_ngs2_voice_state_flags_latch_${value})
		add_test(NAME lumen_ngs2_voice_state_flags_latch_${value} COMMAND ${CMAKE_COMMAND} -E env ${ngs2_state_flags_clean_env}
			"LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS=${value}"
			$<TARGET_FILE:lumen_voice_test> ngs2-state-latch ${value})
	endforeach()
	add_test(NAME lumen_ngs2_voice_state_flags_independence_waveform COMMAND ${CMAKE_COMMAND} -E env
		${ngs2_state_flags_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO=1
		LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS=0
		$<TARGET_FILE:lumen_voice_test> ngs2-state-independence-waveform)
	add_test(NAME lumen_ngs2_voice_state_flags_independence_state COMMAND ${CMAKE_COMMAND} -E env
		${ngs2_state_flags_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO=0
		LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS=1
		$<TARGET_FILE:lumen_voice_test> ngs2-state-independence-state)
	add_test(NAME lumen_ngs2_voice_state_flags_fatal_paths COMMAND ${CMAKE_COMMAND} -E env
		${ngs2_state_flags_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS=1
		$<TARGET_FILE:lumen_voice_test> ngs2-state-fatal-paths)
	set_tests_properties(${ngs2_state_flags_tests} PROPERTIES
		TIMEOUT 20 PASS_REGULAR_EXPRESSION "VoiceTest: PASS")

	set(ngs2_audio_render_clean_env
		--unset=LUMEN_EXPERIMENTAL_REMOTEPLAY_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PLUS_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_PARENTAL_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NP_IN_GAME_MESSAGE_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_RUDP_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_VOICE_OFFLINE
		--unset=LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO
		--unset=LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS)
	set(ngs2_audio_render_tests
		lumen_ngs2_audio_render_default
		lumen_ngs2_audio_render_enabled
		lumen_ngs2_audio_render_independence_waveform
		lumen_ngs2_audio_render_independence_state
		lumen_ngs2_audio_render_independence_render)
	add_test(NAME lumen_ngs2_audio_render_default COMMAND ${CMAKE_COMMAND} -E env
		${ngs2_audio_render_clean_env}
		--unset=LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER
		$<TARGET_FILE:lumen_voice_test> ngs2-render-disabled)
	foreach(case IN ITEMS zero empty malformed whitespace two negative text leading_whitespace)
		if(case STREQUAL "zero")
			set(ngs2_audio_render_value "0")
		elseif(case STREQUAL "empty")
			set(ngs2_audio_render_value "")
		elseif(case STREQUAL "malformed")
			set(ngs2_audio_render_value "01")
		elseif(case STREQUAL "whitespace")
			set(ngs2_audio_render_value "1 ")
		elseif(case STREQUAL "two")
			set(ngs2_audio_render_value "2")
		elseif(case STREQUAL "negative")
			set(ngs2_audio_render_value "-1")
		elseif(case STREQUAL "text")
			set(ngs2_audio_render_value "enabled")
		else()
			set(ngs2_audio_render_value " 1")
		endif()
		list(APPEND ngs2_audio_render_tests lumen_ngs2_audio_render_${case})
		add_test(NAME lumen_ngs2_audio_render_${case} COMMAND ${CMAKE_COMMAND} -E env
			${ngs2_audio_render_clean_env}
			"LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=${ngs2_audio_render_value}"
			$<TARGET_FILE:lumen_voice_test> ngs2-render-disabled)
	endforeach()
	add_test(NAME lumen_ngs2_audio_render_enabled COMMAND ${CMAKE_COMMAND} -E env
		${ngs2_audio_render_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=1
		$<TARGET_FILE:lumen_voice_test> ngs2-render-enabled)
	foreach(value IN ITEMS 0 1)
		list(APPEND ngs2_audio_render_tests lumen_ngs2_audio_render_latch_${value})
		add_test(NAME lumen_ngs2_audio_render_latch_${value} COMMAND ${CMAKE_COMMAND} -E env
			${ngs2_audio_render_clean_env}
			"LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=${value}"
			$<TARGET_FILE:lumen_voice_test> ngs2-render-latch ${value})
	endforeach()
	add_test(NAME lumen_ngs2_audio_render_independence_waveform COMMAND ${CMAKE_COMMAND} -E env
		${ngs2_audio_render_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_WAVEFORM_FRAME_INFO=1
		LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=0
		$<TARGET_FILE:lumen_voice_test> ngs2-render-independence-waveform)
	add_test(NAME lumen_ngs2_audio_render_independence_state COMMAND ${CMAKE_COMMAND} -E env
		${ngs2_audio_render_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS=1
		LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=0
		$<TARGET_FILE:lumen_voice_test> ngs2-render-independence-state)
	add_test(NAME lumen_ngs2_audio_render_independence_render COMMAND ${CMAKE_COMMAND} -E env
		${ngs2_audio_render_clean_env}
		LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=1
		$<TARGET_FILE:lumen_voice_test> ngs2-render-independence-render)
	set_tests_properties(${ngs2_audio_render_tests} PROPERTIES
		TIMEOUT 20 PASS_REGULAR_EXPRESSION "VoiceTest: PASS")
	if(LUMEN_WITH_RUNTIME_LIFECYCLE_TEST)
		target_compile_definitions(lumen_voice_test PRIVATE LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK=1)
		add_test(NAME lumen_ngs2_voice_get_state_memory COMMAND ${CMAKE_COMMAND} -E env
			${voice_clean_env} --unset=LUMEN_EXPERIMENTAL_VOICE_OFFLINE
			$<TARGET_FILE:lumen_voice_test> ngs2-get-state-memory)
		set_tests_properties(lumen_ngs2_voice_get_state_memory PROPERTIES
			TIMEOUT 20 PASS_REGULAR_EXPRESSION "VoiceTest: PASS")
		add_test(NAME lumen_ngs2_audio_render_queue COMMAND ${CMAKE_COMMAND} -E env
			${ngs2_audio_render_clean_env} LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=1
			$<TARGET_FILE:lumen_voice_test> ngs2-render-queue)
		add_test(NAME lumen_ngs2_audio_render_pending_play_state COMMAND ${CMAKE_COMMAND} -E env
			${ngs2_audio_render_clean_env} LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=1
			LUMEN_EXPERIMENTAL_NGS2_VOICE_STATE_FLAGS=1
			$<TARGET_FILE:lumen_voice_test> ngs2-render-queue)
		set_tests_properties(lumen_ngs2_audio_render_queue lumen_ngs2_audio_render_pending_play_state PROPERTIES
			TIMEOUT 20 PASS_REGULAR_EXPRESSION "VoiceTest: PASS")
	endif()
endif()

if(LUMEN_WITH_ULT_INITIALIZE_TEST)
	set(LUMEN_ULT_INITIALIZE_TEST_QUALIFIED OFF)
	if(MACOS AND NOT IOS AND APPLE AND
	   (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
	    (NOT CMAKE_OSX_ARCHITECTURES AND
	     CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")))
		set(LUMEN_ULT_INITIALIZE_TEST_QUALIFIED ON)
	endif()

	if(LUMEN_ULT_INITIALIZE_TEST_QUALIFIED)
		find_package(Threads REQUIRED)
		add_executable(lumen_ult_initialize_test EXCLUDE_FROM_ALL UltInitializeTest.cpp)
		target_link_libraries(lumen_ult_initialize_test PRIVATE emulator Threads::Threads)
		set(ult_initialize_tests
			lumen_ult_initialize_selector_absent
			lumen_ult_initialize_selector_zero
			lumen_ult_initialize_selector_empty
			lumen_ult_initialize_selector_malformed
			lumen_ult_initialize_selector_whitespace
			lumen_ult_initialize_selector_word
			lumen_ult_initialize_selector_two
			lumen_ult_initialize_selector_one
			lumen_ult_initialize_latch_disabled
			lumen_ult_initialize_latch_enabled
			lumen_ult_initialize_exact_identity
			lumen_ult_initialize_scalar_abi
			lumen_ult_initialize_first_repeat
			lumen_ult_initialize_concurrency
			lumen_ult_initialize_reset_lifecycle
			lumen_ult_initialize_init_all_disabled
			lumen_ult_initialize_init_all_enabled
			lumen_ult_initialize_fatal_absent
			lumen_ult_initialize_fatal_zero)
		set(ult_initialize_env LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=1)

		add_test(NAME lumen_ult_initialize_selector_absent COMMAND ${CMAKE_COMMAND} -E env
			${ult_initialize_env} --unset=LUMEN_EXPERIMENTAL_ULT_INITIALIZE
			-- $<TARGET_FILE:lumen_ult_initialize_test> selector-absent)
		foreach(case IN ITEMS zero empty malformed whitespace word two)
			if(case STREQUAL "zero")
				set(ult_initialize_value "0")
			elseif(case STREQUAL "empty")
				set(ult_initialize_value "")
			elseif(case STREQUAL "malformed")
				set(ult_initialize_value "01")
			elseif(case STREQUAL "whitespace")
				set(ult_initialize_value "1 ")
			elseif(case STREQUAL "word")
				set(ult_initialize_value "true")
			else()
				set(ult_initialize_value "2")
			endif()
			add_test(NAME lumen_ult_initialize_selector_${case} COMMAND ${CMAKE_COMMAND} -E env
				${ult_initialize_env} "LUMEN_EXPERIMENTAL_ULT_INITIALIZE=${ult_initialize_value}"
				-- $<TARGET_FILE:lumen_ult_initialize_test> selector-${case})
		endforeach()
		add_test(NAME lumen_ult_initialize_selector_one COMMAND ${CMAKE_COMMAND} -E env
			${ult_initialize_env} LUMEN_EXPERIMENTAL_ULT_INITIALIZE=1
			-- $<TARGET_FILE:lumen_ult_initialize_test> selector-one)
		add_test(NAME lumen_ult_initialize_latch_disabled COMMAND ${CMAKE_COMMAND} -E env
			${ult_initialize_env} LUMEN_EXPERIMENTAL_ULT_INITIALIZE=0
			-- $<TARGET_FILE:lumen_ult_initialize_test> latch-disabled)
		add_test(NAME lumen_ult_initialize_latch_enabled COMMAND ${CMAKE_COMMAND} -E env
			${ult_initialize_env} LUMEN_EXPERIMENTAL_ULT_INITIALIZE=1
			-- $<TARGET_FILE:lumen_ult_initialize_test> latch-enabled)
		foreach(case IN ITEMS exact-identity scalar-abi first-repeat concurrency reset-lifecycle init-all-enabled)
			string(REPLACE "-" "_" test_suffix "${case}")
			add_test(NAME lumen_ult_initialize_${test_suffix} COMMAND ${CMAKE_COMMAND} -E env
				${ult_initialize_env} LUMEN_EXPERIMENTAL_ULT_INITIALIZE=1
				-- $<TARGET_FILE:lumen_ult_initialize_test> ${case})
		endforeach()
		add_test(NAME lumen_ult_initialize_init_all_disabled COMMAND ${CMAKE_COMMAND} -E env
			${ult_initialize_env} --unset=LUMEN_EXPERIMENTAL_ULT_INITIALIZE
			-- $<TARGET_FILE:lumen_ult_initialize_test> init-all-disabled)
		add_test(NAME lumen_ult_initialize_fatal_absent COMMAND ${CMAKE_COMMAND} -E env
			${ult_initialize_env} --unset=LUMEN_EXPERIMENTAL_ULT_INITIALIZE
			-- $<TARGET_FILE:lumen_ult_initialize_test> fatal-disabled)
		add_test(NAME lumen_ult_initialize_fatal_zero COMMAND ${CMAKE_COMMAND} -E env
			${ult_initialize_env} LUMEN_EXPERIMENTAL_ULT_INITIALIZE=0
			-- $<TARGET_FILE:lumen_ult_initialize_test> fatal-disabled)
		set_tests_properties(${ult_initialize_tests} PROPERTIES
			LABELS "macos;arm64;ult;local-contract"
			TIMEOUT 30
			PASS_REGULAR_EXPRESSION "UltInitializeTest: PASS")
	else()
		message(STATUS "ULT Initialize tests: NON VERIFICATO (requires a thin macOS arm64 build)")
	endif()
	unset(LUMEN_ULT_INITIALIZE_TEST_QUALIFIED)
endif()

if(LUMEN_WITH_MOVE_INITIALIZE_TEST)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		find_package(Threads REQUIRED)
		add_executable(lumen_move_initialize_test EXCLUDE_FROM_ALL MoveInitializeTest.cpp)
		target_link_libraries(lumen_move_initialize_test PRIVATE emulator Threads::Threads)
		set(move_initialize_tests
			lumen_move_initialize_selector_absent
			lumen_move_initialize_selector_zero
			lumen_move_initialize_selector_empty
			lumen_move_initialize_selector_malformed
			lumen_move_initialize_selector_whitespace
			lumen_move_initialize_selector_word
			lumen_move_initialize_selector_two
			lumen_move_initialize_selector_one
			lumen_move_initialize_latch_disabled
			lumen_move_initialize_latch_enabled
			lumen_move_initialize_exact_identity
			lumen_move_initialize_scalar_abi
			lumen_move_initialize_first_reset
			lumen_move_initialize_init_profile
			lumen_move_initialize_concurrent_init
			lumen_move_initialize_open_profile
			lumen_move_initialize_device_info_profile
			lumen_move_initialize_read_state_recent_profile
			lumen_move_initialize_init_all_disabled
			lumen_move_initialize_init_all_enabled
			lumen_move_initialize_fatal_absent
			lumen_move_initialize_fatal_zero
			lumen_move_initialize_fatal_open_disabled
			lumen_move_initialize_fatal_info_disabled
			lumen_move_initialize_fatal_read_disabled)
		set(move_initialize_env LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=1)

		add_test(NAME lumen_move_initialize_selector_absent COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_move_initialize_test> selector-absent)
		foreach(case IN ITEMS zero empty malformed whitespace word two)
			if(case STREQUAL "zero")
				set(move_initialize_value "0")
			elseif(case STREQUAL "empty")
				set(move_initialize_value "")
			elseif(case STREQUAL "malformed")
				set(move_initialize_value "01")
			elseif(case STREQUAL "whitespace")
				set(move_initialize_value "1 ")
			elseif(case STREQUAL "word")
				set(move_initialize_value "true")
			else()
				set(move_initialize_value "2")
			endif()
			add_test(NAME lumen_move_initialize_selector_${case} COMMAND ${CMAKE_COMMAND} -E env
				${move_initialize_env}
				"LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE=${move_initialize_value}"
				-- $<TARGET_FILE:lumen_move_initialize_test> selector-${case})
		endforeach()
		add_test(NAME lumen_move_initialize_selector_one COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE=1
			-- $<TARGET_FILE:lumen_move_initialize_test> selector-one)
		add_test(NAME lumen_move_initialize_latch_disabled COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_move_initialize_test> latch-disabled)
		add_test(NAME lumen_move_initialize_latch_enabled COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE=1
			-- $<TARGET_FILE:lumen_move_initialize_test> latch-enabled)
		foreach(case IN ITEMS exact-identity scalar-abi first-reset init-profile concurrent-init open-profile device-info-profile read-state-recent-profile init-all-enabled)
			string(REPLACE "-" "_" test_suffix "${case}")
			add_test(NAME lumen_move_initialize_${test_suffix} COMMAND ${CMAKE_COMMAND} -E env
				${move_initialize_env} LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE=1
				-- $<TARGET_FILE:lumen_move_initialize_test> ${case})
		endforeach()
		add_test(NAME lumen_move_initialize_init_all_disabled COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_move_initialize_test> init-all-disabled)
		add_test(NAME lumen_move_initialize_fatal_absent COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_move_initialize_test> fatal-disabled)
		add_test(NAME lumen_move_initialize_fatal_zero COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_move_initialize_test> fatal-disabled)
		add_test(NAME lumen_move_initialize_fatal_open_disabled COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_move_initialize_test> fatal-open-disabled)
		add_test(NAME lumen_move_initialize_fatal_info_disabled COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_move_initialize_test> fatal-info-disabled)
		add_test(NAME lumen_move_initialize_fatal_read_disabled COMMAND ${CMAKE_COMMAND} -E env
			${move_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_move_initialize_test> fatal-read-disabled)
		set_tests_properties(${move_initialize_tests} PROPERTIES
			LABELS "macos;arm64;fex;move;no-device;local-contract"
			TIMEOUT 20
			PASS_REGULAR_EXPRESSION "MoveInitializeTest: PASS")
	else()
		message(STATUS
			"Move Initialize tests: NON VERIFICATO (requires the macOS arm64 FEX runtime)")
	endif()
endif()

if(LUMEN_WITH_MOUSE_INITIALIZE_TEST)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		find_package(Threads REQUIRED)
		add_executable(lumen_mouse_initialize_test EXCLUDE_FROM_ALL MouseInitializeTest.cpp)
		target_link_libraries(lumen_mouse_initialize_test PRIVATE emulator Threads::Threads)
		set(mouse_initialize_tests
			lumen_mouse_initialize_selector_absent
			lumen_mouse_initialize_selector_zero
			lumen_mouse_initialize_selector_empty
			lumen_mouse_initialize_selector_malformed
			lumen_mouse_initialize_selector_whitespace
			lumen_mouse_initialize_selector_word
			lumen_mouse_initialize_selector_two
			lumen_mouse_initialize_selector_one
			lumen_mouse_initialize_latch_disabled
			lumen_mouse_initialize_latch_enabled
			lumen_mouse_initialize_exact_identity
			lumen_mouse_initialize_scalar_abi
			lumen_mouse_initialize_calls_reset
			lumen_mouse_initialize_concurrent
			lumen_mouse_initialize_open_profile
			lumen_mouse_initialize_read_profile
			lumen_mouse_initialize_init_all_disabled
			lumen_mouse_initialize_init_all_enabled
			lumen_mouse_initialize_fatal_absent
			lumen_mouse_initialize_fatal_zero
			lumen_mouse_initialize_fatal_open_disabled
			lumen_mouse_initialize_fatal_read_disabled)
		set(mouse_initialize_env
			LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=1
			--unset=LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE)

		add_test(NAME lumen_mouse_initialize_selector_absent COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_mouse_initialize_test> selector-absent)
		foreach(case IN ITEMS zero empty malformed whitespace word two)
			if(case STREQUAL "zero")
				set(mouse_initialize_value "0")
			elseif(case STREQUAL "empty")
				set(mouse_initialize_value "")
			elseif(case STREQUAL "malformed")
				set(mouse_initialize_value "01")
			elseif(case STREQUAL "whitespace")
				set(mouse_initialize_value "1 ")
			elseif(case STREQUAL "word")
				set(mouse_initialize_value "true")
			else()
				set(mouse_initialize_value "2")
			endif()
			add_test(NAME lumen_mouse_initialize_selector_${case} COMMAND ${CMAKE_COMMAND} -E env
				${mouse_initialize_env}
				"LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE=${mouse_initialize_value}"
				-- $<TARGET_FILE:lumen_mouse_initialize_test> selector-${case})
		endforeach()
		add_test(NAME lumen_mouse_initialize_selector_one COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE=1
			-- $<TARGET_FILE:lumen_mouse_initialize_test> selector-one)
		add_test(NAME lumen_mouse_initialize_latch_disabled COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_mouse_initialize_test> latch-disabled)
		add_test(NAME lumen_mouse_initialize_latch_enabled COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE=1
			-- $<TARGET_FILE:lumen_mouse_initialize_test> latch-enabled)
		foreach(case IN ITEMS exact-identity scalar-abi calls-reset concurrent open-profile read-profile init-all-enabled)
			string(REPLACE "-" "_" test_suffix "${case}")
			add_test(NAME lumen_mouse_initialize_${test_suffix} COMMAND ${CMAKE_COMMAND} -E env
				${mouse_initialize_env} LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE=1
				-- $<TARGET_FILE:lumen_mouse_initialize_test> ${case})
		endforeach()
		add_test(NAME lumen_mouse_initialize_init_all_disabled COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_mouse_initialize_test> init-all-disabled)
		add_test(NAME lumen_mouse_initialize_fatal_absent COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_mouse_initialize_test> fatal-disabled)
		add_test(NAME lumen_mouse_initialize_fatal_zero COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_mouse_initialize_test> fatal-disabled)
		add_test(NAME lumen_mouse_initialize_fatal_open_disabled COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_mouse_initialize_test> fatal-open-disabled)
		add_test(NAME lumen_mouse_initialize_fatal_read_disabled COMMAND ${CMAKE_COMMAND} -E env
			${mouse_initialize_env} --unset=LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE
			-- $<TARGET_FILE:lumen_mouse_initialize_test> fatal-read-disabled)
		set_tests_properties(${mouse_initialize_tests} PROPERTIES
			LABELS "macos;arm64;fex;mouse;no-device;local-contract"
			TIMEOUT 20
			PASS_REGULAR_EXPRESSION "MouseInitializeTest: PASS")
	else()
		message(STATUS
			"Mouse Initialize tests: NON VERIFICATO (requires the macOS arm64 FEX runtime)")
	endif()
endif()

if(LUMEN_WITH_IME_DIALOG_TEST)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		find_package(Threads REQUIRED)
		add_executable(lumen_ime_dialog_test EXCLUDE_FROM_ALL ImeDialogTest.cpp)
		target_link_libraries(lumen_ime_dialog_test PRIVATE emulator Threads::Threads)
		foreach(snapshot IN ITEMS 0 1)
			add_test(NAME lumen_ime_dialog_snapshot_${snapshot}
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_MACOS_FEXCORE_RUN=1
					LUMEN_MACOS_VM_REGION_SNAPSHOT=${snapshot}
					LUMEN_MACOS_VM_ROOT_SNAPSHOT=${snapshot}
					LUMEN_EXPERIMENTAL_GUEST_DATA_SNAPSHOT_PREFLIGHT=${snapshot}
					$<TARGET_FILE:lumen_ime_dialog_test>)
			set_tests_properties(lumen_ime_dialog_snapshot_${snapshot} PROPERTIES
				LABELS "ime;dialog;guest-memory;local-contract" TIMEOUT 30)
		endforeach()
	else()
		message(STATUS "PS4 IME dialog guest-memory gate: NON VERIFICATO (requires macOS arm64/FEX); Android uses its device VM/seal gate")
	endif()
endif()

if(LUMEN_WITH_IME_KEYBOARD_TEST)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		find_package(Threads REQUIRED)
		add_executable(lumen_ime_keyboard_test EXCLUDE_FROM_ALL ImeKeyboardTest.cpp)
		target_link_libraries(lumen_ime_keyboard_test PRIVATE emulator Threads::Threads)
		set(ime_keyboard_tests
			lumen_ime_keyboard_selector_absent
			lumen_ime_keyboard_selector_zero
			lumen_ime_keyboard_selector_empty
			lumen_ime_keyboard_selector_malformed
			lumen_ime_keyboard_selector_whitespace
			lumen_ime_keyboard_selector_word
			lumen_ime_keyboard_selector_two
			lumen_ime_keyboard_selector_one
			lumen_ime_keyboard_latch_disabled
			lumen_ime_keyboard_latch_enabled
			lumen_ime_keyboard_exact_identity
			lumen_ime_keyboard_scalar_abi
			lumen_ime_keyboard_open_profile
			lumen_ime_keyboard_session_reset
			lumen_ime_keyboard_init_all_disabled
			lumen_ime_keyboard_init_all_enabled
			lumen_ime_keyboard_fatal_open_absent
			lumen_ime_keyboard_fatal_open_zero
			lumen_ime_keyboard_resource_id_profile
			lumen_ime_keyboard_fatal_resource_id_absent
			lumen_ime_keyboard_fatal_resource_id_zero
			lumen_ime_keyboard_fatal_resource_id_unwritable
			lumen_ime_keyboard_get_info_profile
			lumen_ime_keyboard_fatal_get_info_absent
			lumen_ime_keyboard_fatal_get_info_zero
			lumen_ime_keyboard_fatal_get_info_unwritable
			lumen_ime_keyboard_update_profile
			lumen_ime_keyboard_fatal_update_absent
			lumen_ime_keyboard_fatal_update_zero)
		set(ime_keyboard_env
			LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=1
			--unset=LUMEN_EXPERIMENTAL_MOVE_INITIALIZE_NO_DEVICE
			--unset=LUMEN_EXPERIMENTAL_MOUSE_INITIALIZE_NO_DEVICE)

		add_test(NAME lumen_ime_keyboard_selector_absent COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} --unset=LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE
			-- $<TARGET_FILE:lumen_ime_keyboard_test> selector-absent)
		foreach(case IN ITEMS zero empty malformed whitespace word two)
			if(case STREQUAL "zero")
				set(ime_keyboard_value "0")
			elseif(case STREQUAL "empty")
				set(ime_keyboard_value "")
			elseif(case STREQUAL "malformed")
				set(ime_keyboard_value "01")
			elseif(case STREQUAL "whitespace")
				set(ime_keyboard_value "1 ")
			elseif(case STREQUAL "word")
				set(ime_keyboard_value "true")
			else()
				set(ime_keyboard_value "2")
			endif()
			add_test(NAME lumen_ime_keyboard_selector_${case} COMMAND ${CMAKE_COMMAND} -E env
				${ime_keyboard_env}
				"LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=${ime_keyboard_value}"
				-- $<TARGET_FILE:lumen_ime_keyboard_test> selector-${case})
		endforeach()
		add_test(NAME lumen_ime_keyboard_selector_one COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=1
			-- $<TARGET_FILE:lumen_ime_keyboard_test> selector-one)
		add_test(NAME lumen_ime_keyboard_latch_disabled COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_ime_keyboard_test> latch-disabled)
		add_test(NAME lumen_ime_keyboard_latch_enabled COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=1
			-- $<TARGET_FILE:lumen_ime_keyboard_test> latch-enabled)
		foreach(case IN ITEMS exact-identity scalar-abi open-profile session-reset resource-id-profile get-info-profile update-profile init-all-enabled)
			string(REPLACE "-" "_" test_suffix "${case}")
			add_test(NAME lumen_ime_keyboard_${test_suffix} COMMAND ${CMAKE_COMMAND} -E env
				${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=1
				-- $<TARGET_FILE:lumen_ime_keyboard_test> ${case})
		endforeach()
		add_test(NAME lumen_ime_keyboard_init_all_disabled COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} --unset=LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE
			-- $<TARGET_FILE:lumen_ime_keyboard_test> init-all-disabled)
		add_test(NAME lumen_ime_keyboard_fatal_open_absent COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} --unset=LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-open-absent)
		add_test(NAME lumen_ime_keyboard_fatal_open_zero COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-open-zero)
		add_test(NAME lumen_ime_keyboard_fatal_resource_id_absent COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} --unset=LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-resource-id-absent)
		add_test(NAME lumen_ime_keyboard_fatal_resource_id_zero COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-resource-id-zero)
		add_test(NAME lumen_ime_keyboard_fatal_resource_id_unwritable COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=1
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-resource-id-unwritable)
		add_test(NAME lumen_ime_keyboard_fatal_get_info_absent COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} --unset=LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-get-info-absent)
		add_test(NAME lumen_ime_keyboard_fatal_get_info_zero COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-get-info-zero)
		add_test(NAME lumen_ime_keyboard_fatal_get_info_unwritable COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=1
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-get-info-unwritable)
		add_test(NAME lumen_ime_keyboard_fatal_update_absent COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} --unset=LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-update-absent)
		add_test(NAME lumen_ime_keyboard_fatal_update_zero COMMAND ${CMAKE_COMMAND} -E env
			${ime_keyboard_env} LUMEN_EXPERIMENTAL_IME_KEYBOARD_NO_DEVICE=0
			-- $<TARGET_FILE:lumen_ime_keyboard_test> fatal-update-zero)
		set_tests_properties(${ime_keyboard_tests} PROPERTIES
			LABELS "macos;arm64;fex;ime;keyboard;no-device;local-contract"
			TIMEOUT 20
			PASS_REGULAR_EXPRESSION "ImeKeyboardTest: PASS")
	else()
		message(STATUS
			"Ime keyboard tests: NON VERIFICATO (requires the macOS arm64 FEX runtime)")
	endif()
endif()

add_executable(fc_script ${LUMEN_SCRIPT_SRC})
lumen_attach_legal_notices(fc_script)
target_include_directories(fc_script PRIVATE "${PROJECT_SOURCE_DIR}/3rdparty/sqlite/src")
if(LUMEN_MACOS_FEXCORE_RUNTIME)
	target_compile_definitions(fc_script PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	add_executable(lumen_game_translation_cache_test
		"${PROJECT_SOURCE_DIR}/cpu/runtime/GameTranslationCacheTest.cpp"
		"${PROJECT_SOURCE_DIR}/cpu/runtime/GameTranslationCacheSha.c")
	target_include_directories(lumen_game_translation_cache_test PRIVATE
		"${PROJECT_SOURCE_DIR}/3rdparty/sqlite/include"
		"${PROJECT_SOURCE_DIR}/3rdparty/sqlite/src")
	add_test(NAME lumen_game_translation_cache COMMAND $<TARGET_FILE:lumen_game_translation_cache_test>)
	set_tests_properties(lumen_game_translation_cache PROPERTIES TIMEOUT 30 LABELS "macos;fex;cache;synthetic")
endif()
if(WIN32)
	target_compile_definitions(fc_script PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()


if(LUMEN_WITH_GRAPHICS_SMOKE_TEST)
	add_executable(lumen_graphics_smoke GraphicsSmoke.cpp)
	target_include_directories(lumen_graphics_smoke PRIVATE
		${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include
		${PROJECT_SOURCE_DIR}/emulator/include)
	target_link_libraries(lumen_graphics_smoke SDL2-static vulkan-1)
	if(MACOS AND LUMEN_APPLE_NATIVE_METAL)
		target_compile_definitions(lumen_graphics_smoke PRIVATE LUMEN_GRAPHICS_SMOKE_NATIVE_METAL=1)
		add_test(NAME lumen_graphics_smoke_null_sampled_image_native_metal
			COMMAND $<TARGET_FILE:lumen_graphics_smoke> --null-sampled-image-zero)
		set_tests_properties(lumen_graphics_smoke_null_sampled_image_native_metal PROPERTIES
			LABELS "graphics;native-metal;phase6cj"
			RUN_SERIAL TRUE
			TIMEOUT 60)
		add_test(NAME lumen_graphics_smoke_auto_index_line_strip_native_metal
			COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1
				$<TARGET_FILE:lumen_graphics_smoke> --line-strip-readback)
		set_tests_properties(lumen_graphics_smoke_auto_index_line_strip_native_metal PROPERTIES
			LABELS "graphics;native-metal;auto-index;line-strip"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_AUTO_INDEX_LINE_STRIP_BACKEND_TEST\\].*topology=line-strip.*draw=non-indexed.*padding=clean status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)
		add_test(NAME lumen_graphics_smoke_auto_index_line_list_native_metal
			COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1
				$<TARGET_FILE:lumen_graphics_smoke> --line-list-readback)
		set_tests_properties(lumen_graphics_smoke_auto_index_line_list_native_metal PROPERTIES
			LABELS "graphics;native-metal;auto-index;line-list"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_AUTO_INDEX_LINE_LIST_BACKEND_TEST\\].*topology=line-list.*draw=non-indexed.*vertices=3.*first_vertex=1.*padding=clean status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_dx10_clamp_nan_compute
			GraphicsDx10ClampNanCompute.cpp)
		target_include_directories(lumen_graphics_dx10_clamp_nan_compute PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
		target_link_libraries(lumen_graphics_dx10_clamp_nan_compute PRIVATE
			vulkan-1 spirv-tools)
		add_test(NAME lumen_graphics_dx10_clamp_nan_compute_native_metal
			COMMAND $<TARGET_FILE:lumen_graphics_dx10_clamp_nan_compute>)
		set_tests_properties(lumen_graphics_dx10_clamp_nan_compute_native_metal PROPERTIES
			LABELS "graphics;native-metal;dx10-clamp;backend-arithmetic"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_DX10_CLAMP_NAN_COMPUTE\\].*scope=backend-arithmetic-only.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_vmul_legacy_f32_compute
			GraphicsVMulLegacyF32Compute.cpp)
		target_include_directories(lumen_graphics_vmul_legacy_f32_compute PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
		target_link_libraries(lumen_graphics_vmul_legacy_f32_compute PRIVATE
			vulkan-1 spirv-tools)
		add_test(NAME lumen_graphics_vmul_legacy_f32_compute_native_metal
			COMMAND $<TARGET_FILE:lumen_graphics_vmul_legacy_f32_compute>)
		set_tests_properties(lumen_graphics_vmul_legacy_f32_compute_native_metal PROPERTIES
			LABELS "graphics;native-metal;vmul-legacy;backend-arithmetic"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_VMUL_LEGACY_F32_COMPUTE\\].*control-flow=branch-merge-phi.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_vmin_u32_compute
			GraphicsVMinU32Compute.cpp)
		target_include_directories(lumen_graphics_vmin_u32_compute PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
		target_link_libraries(lumen_graphics_vmin_u32_compute PRIVATE
			vulkan-1 spirv-tools)
		add_test(NAME lumen_graphics_vmin_u32_compute_native_metal
			COMMAND $<TARGET_FILE:lumen_graphics_vmin_u32_compute>)
		set_tests_properties(lumen_graphics_vmin_u32_compute_native_metal PROPERTIES
			LABELS "graphics;native-metal;vmin-u32;backend-arithmetic"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_VMIN_U32_COMPUTE\\].*operation=umin.*signed-boundary=verified.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_storage_base_mip_one_compute
			GraphicsStorageBaseMipOneCompute.cpp)
		target_include_directories(lumen_graphics_storage_base_mip_one_compute PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
		target_link_libraries(lumen_graphics_storage_base_mip_one_compute PRIVATE
			vulkan-1 spirv-tools)
		add_test(NAME lumen_graphics_storage_base_mip_one_compute
			COMMAND $<TARGET_FILE:lumen_graphics_storage_base_mip_one_compute>)
		set_tests_properties(lumen_graphics_storage_base_mip_one_compute PROPERTIES
			LABELS "graphics;native-metal;storage-image;mip-window"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_STORAGE_SINGLE_MIP_WINDOW_COMPUTE\\].*chain=s1-write->general-barrier->sample-s1->write-s2.*layouts=general.*s1=verified.*s2=verified.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_thin1d_reinterpret_compute
			GraphicsThin1dReinterpretCompute.cpp)
		target_include_directories(lumen_graphics_thin1d_reinterpret_compute PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include
			${PROJECT_SOURCE_DIR}/emulator/include)
		target_link_libraries(lumen_graphics_thin1d_reinterpret_compute PRIVATE
			vulkan-1 spirv-tools)
		add_test(NAME lumen_graphics_thin1d_reinterpret_compute_native_metal
			COMMAND $<TARGET_FILE:lumen_graphics_thin1d_reinterpret_compute>)
		set_tests_properties(lumen_graphics_thin1d_reinterpret_compute_native_metal PROPERTIES
			LABELS "graphics;native-metal;storage-image;thin1d;reinterpret"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_THIN1D_REINTERPRET_COMPUTE\\].*bindings=storage\\+storage.*layout=thin1d-prefix.*padded-destination=logical-texels-only.*canaries=clean.*source=unchanged.*destination=byte-exact.*lifetime=through-fence.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_vertex_snorm8x4_vec3_vulkan_probe
			GraphicsVertexSnorm8x4Vec3VulkanProbe.cpp)
		target_include_directories(lumen_graphics_vertex_snorm8x4_vec3_vulkan_probe PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
		target_link_libraries(lumen_graphics_vertex_snorm8x4_vec3_vulkan_probe PRIVATE
			vulkan-1 spirv-tools)
		add_test(NAME lumen_graphics_vertex_snorm8x4_vec3_vulkan_probe_native_metal
			COMMAND $<TARGET_FILE:lumen_graphics_vertex_snorm8x4_vec3_vulkan_probe>)
		set_tests_properties(lumen_graphics_vertex_snorm8x4_vec3_vulkan_probe_native_metal PROPERTIES
			LABELS "graphics;native-metal;vertex-input;snorm8x4;vec3"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_VERTEX_SNORM8X4_VEC3_VULKAN_PROBE\\].*format=R8G8B8A8_SNORM.*shader-input=v3float.*w-independence=verified.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_bc7_texture_vulkan_probe
			GraphicsBc7TextureVulkanProbe.cpp)
		target_include_directories(lumen_graphics_bc7_texture_vulkan_probe PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
		target_link_libraries(lumen_graphics_bc7_texture_vulkan_probe PRIVATE
			vulkan-1 spirv-tools)
		add_test(NAME lumen_graphics_bc7_texture_vulkan_probe_native_metal
			COMMAND $<TARGET_FILE:lumen_graphics_bc7_texture_vulkan_probe>)
		set_tests_properties(lumen_graphics_bc7_texture_vulkan_probe_native_metal PROPERTIES
			LABELS "graphics;native-metal;sampled-image;bc7;buffer-image-copy"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_BC7_TEXTURE_VULKAN_PROBE\\].*format=BC7_UNORM.*pixels=9216.*colors=6.*compressed-row-bytes=256,192,256.*feature-negatives=clean.*guards=clean.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_depth_stencil_copy_vulkan_probe
			GraphicsDepthStencilCopyVulkanProbe.cpp)
		target_include_directories(lumen_graphics_depth_stencil_copy_vulkan_probe PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
		target_link_libraries(lumen_graphics_depth_stencil_copy_vulkan_probe PRIVATE
			vulkan-1)
		add_test(NAME lumen_graphics_depth_stencil_copy_vulkan_probe_native_metal
			COMMAND $<TARGET_FILE:lumen_graphics_depth_stencil_copy_vulkan_probe>)
		set_tests_properties(lumen_graphics_depth_stencil_copy_vulkan_probe_native_metal PROPERTIES
			LABELS "graphics;native-metal;depth-stencil;buffer-image-copy;d32s8"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_DEPTH_STENCIL_COPY_VULKAN_PROBE\\].*format=D32_SFLOAT_S8_UINT.*depth_bpt=4.*stencil_bpt=1.*directions=2.*invalid_masks=4.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)

		add_executable(lumen_graphics_absolute_depth_bias_vulkan_probe
			GraphicsAbsoluteDepthBiasVulkanProbe.cpp)
		target_include_directories(lumen_graphics_absolute_depth_bias_vulkan_probe PRIVATE
			${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
		target_link_libraries(lumen_graphics_absolute_depth_bias_vulkan_probe PRIVATE
			vulkan-1 spirv-tools)
		add_test(NAME lumen_graphics_absolute_depth_bias_vulkan_probe_native_metal
			COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1
				$<TARGET_FILE:lumen_graphics_absolute_depth_bias_vulkan_probe>)
		set_tests_properties(lumen_graphics_absolute_depth_bias_vulkan_probe_native_metal PROPERTIES
			LABELS "graphics;native-metal;depth-stencil;depth-bias;absolute;d32s8"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_ABSOLUTE_DEPTH_BIAS_VULKAN_PROBE\\].*format=D32_SFLOAT_S8_UINT.*viewport_original=0.5:1.*viewport_translated=0.2:0.7.*absolute_offset=-0.3.*native_constant=0.*native_slope=-0.3.*format_relative_constant=-1.*bases=3.*modes=on\\+off\\+format-relative.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)
	endif()
endif()

if(LUMEN_WITH_VMAC_LEGACY_F32_VULKAN_PROBE)
	if(CMAKE_CROSSCOMPILING OR ANDROID OR IOS)
		message(FATAL_ERROR
			"LUMEN_WITH_VMAC_LEGACY_F32_VULKAN_PROBE is limited to native desktop builds")
	endif()
	if(WIN32)
		if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR MSVC_VERSION LESS 1930 OR
		   NOT LUMEN_BITNESS EQUAL 64 OR
		   NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022" OR
		   NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
			message(FATAL_ERROR
				"The Windows V_MAC_LEGACY_F32 Vulkan probe requires the Visual Studio 17 2022 generator, MSVC, and -A x64")
		endif()
	endif()
	add_executable(lumen_graphics_vmac_legacy_f32_vulkan_probe
		GraphicsVMacLegacyF32VulkanProbe.cpp)
	target_include_directories(lumen_graphics_vmac_legacy_f32_vulkan_probe PRIVATE
		${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
	target_link_libraries(lumen_graphics_vmac_legacy_f32_vulkan_probe PRIVATE
		vulkan-1 spirv-tools)
	set(LUMEN_VMAC_LEGACY_F32_PROBE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_VMAC_LEGACY_F32_PROBE_ARCH)
		set(LUMEN_VMAC_LEGACY_F32_PROBE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	add_test(NAME lumen_graphics_vmac_legacy_f32_vulkan_probe_shader_only
		COMMAND ${CMAKE_COMMAND}
			"-DPROBE=$<TARGET_FILE:lumen_graphics_vmac_legacy_f32_vulkan_probe>"
			-DMODE=shader-only
			-P "${PROJECT_SOURCE_DIR}/tests/VMacLegacyF32VulkanProbeTest.cmake")
	set_tests_properties(lumen_graphics_vmac_legacy_f32_vulkan_probe_shader_only PROPERTIES
		LABELS "graphics;vulkan;vmac-legacy;backend-only;shader-only"
		TIMEOUT 10)
	if(WIN32)
		add_test(NAME lumen_graphics_vmac_legacy_f32_vulkan_probe_amd_13c0
			COMMAND ${CMAKE_COMMAND}
				"-DPROBE=$<TARGET_FILE:lumen_graphics_vmac_legacy_f32_vulkan_probe>"
				-DMODE=amd-13c0
				-P "${PROJECT_SOURCE_DIR}/tests/VMacLegacyF32VulkanProbeTest.cmake")
		set_tests_properties(lumen_graphics_vmac_legacy_f32_vulkan_probe_amd_13c0 PROPERTIES
			LABELS "graphics;vulkan;vmac-legacy;backend-only;amd-13c0"
			RUN_SERIAL TRUE
			TIMEOUT 20)
	endif()
	if(MACOS AND LUMEN_APPLE_NATIVE_METAL AND
	   LUMEN_VMAC_LEGACY_F32_PROBE_ARCH MATCHES "^(arm64|aarch64)$")
		target_compile_definitions(lumen_graphics_vmac_legacy_f32_vulkan_probe PRIVATE
			LUMEN_VMAC_LEGACY_F32_PROBE_NATIVE_METAL_APPLE_M1=1)
		add_test(NAME lumen_graphics_vmac_legacy_f32_vulkan_probe_native_metal_apple_m1
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_A64_METAL_SHADER_FUNCTION_CACHE
				--unset=LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION
				--unset=LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE
				${CMAKE_COMMAND}
				"-DPROBE=$<TARGET_FILE:lumen_graphics_vmac_legacy_f32_vulkan_probe>"
				-DMODE=native-metal-apple-m1
				-P "${PROJECT_SOURCE_DIR}/tests/VMacLegacyF32VulkanProbeTest.cmake")
		set_tests_properties(lumen_graphics_vmac_legacy_f32_vulkan_probe_native_metal_apple_m1 PROPERTIES
			LABELS "graphics;vulkan;vmac-legacy;backend-only;native-metal;apple-m1"
			RUN_SERIAL TRUE
			TIMEOUT 60)
	endif()
endif()

if(LUMEN_WITH_VRSQ_CLAMP_F32_VULKAN_PROBE)
	if(CMAKE_CROSSCOMPILING OR ANDROID OR IOS)
		message(FATAL_ERROR
			"LUMEN_WITH_VRSQ_CLAMP_F32_VULKAN_PROBE is limited to native desktop builds")
	endif()
	if(WIN32)
		if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR MSVC_VERSION LESS 1930 OR
		   NOT LUMEN_BITNESS EQUAL 64 OR
		   NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022" OR
		   NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
			message(FATAL_ERROR
				"The Windows V_RSQ_CLAMP_F32 Vulkan probe requires the Visual Studio 17 2022 generator, MSVC, and -A x64")
		endif()
	endif()
	add_executable(lumen_graphics_vrsq_clamp_f32_vulkan_probe
		GraphicsVRsqClampF32VulkanProbe.cpp)
	target_include_directories(lumen_graphics_vrsq_clamp_f32_vulkan_probe PRIVATE
		${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
	target_link_libraries(lumen_graphics_vrsq_clamp_f32_vulkan_probe PRIVATE
		vulkan-1 spirv-tools)
	set(LUMEN_VRSQ_CLAMP_F32_PROBE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_VRSQ_CLAMP_F32_PROBE_ARCH)
		set(LUMEN_VRSQ_CLAMP_F32_PROBE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	add_test(NAME lumen_graphics_vrsq_clamp_f32_vulkan_probe_shader_only
		COMMAND ${CMAKE_COMMAND}
			"-DPROBE=$<TARGET_FILE:lumen_graphics_vrsq_clamp_f32_vulkan_probe>"
			-DMODE=shader-only
			-P "${PROJECT_SOURCE_DIR}/tests/VRsqClampF32VulkanProbeTest.cmake")
	set_tests_properties(lumen_graphics_vrsq_clamp_f32_vulkan_probe_shader_only PROPERTIES
		LABELS "graphics;vulkan;vrsq-clamp;backend-only;shader-only"
		TIMEOUT 15)
	add_test(NAME lumen_graphics_vrsq_clamp_f32_vulkan_probe_exhaustive_host
		COMMAND ${CMAKE_COMMAND}
			"-DPROBE=$<TARGET_FILE:lumen_graphics_vrsq_clamp_f32_vulkan_probe>"
			-DMODE=exhaustive-host
			-P "${PROJECT_SOURCE_DIR}/tests/VRsqClampF32VulkanProbeTest.cmake")
	set_tests_properties(lumen_graphics_vrsq_clamp_f32_vulkan_probe_exhaustive_host PROPERTIES
		LABELS "graphics;vrsq-clamp;host-oracle;exhaustive"
		RUN_SERIAL TRUE
		TIMEOUT 180)
	if(WIN32)
		add_test(NAME lumen_graphics_vrsq_clamp_f32_vulkan_probe_amd_13c0
			COMMAND ${CMAKE_COMMAND}
				"-DPROBE=$<TARGET_FILE:lumen_graphics_vrsq_clamp_f32_vulkan_probe>"
				-DMODE=amd-13c0
				-P "${PROJECT_SOURCE_DIR}/tests/VRsqClampF32VulkanProbeTest.cmake")
		set_tests_properties(lumen_graphics_vrsq_clamp_f32_vulkan_probe_amd_13c0 PROPERTIES
			LABELS "graphics;vulkan;vrsq-clamp;backend-only;amd-13c0"
			RUN_SERIAL TRUE
			TIMEOUT 45)
	endif()
	if(MACOS AND LUMEN_APPLE_NATIVE_METAL AND
	   LUMEN_VRSQ_CLAMP_F32_PROBE_ARCH MATCHES "^(arm64|aarch64)$")
		target_compile_definitions(lumen_graphics_vrsq_clamp_f32_vulkan_probe PRIVATE
			LUMEN_VRSQ_CLAMP_F32_PROBE_NATIVE_METAL_APPLE_M1=1)
		add_test(NAME lumen_graphics_vrsq_clamp_f32_vulkan_probe_native_metal_apple_m1
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_A64_METAL_SHADER_FUNCTION_CACHE
				--unset=LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION
				--unset=LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE
				${CMAKE_COMMAND}
				"-DPROBE=$<TARGET_FILE:lumen_graphics_vrsq_clamp_f32_vulkan_probe>"
				-DMODE=native-metal-apple-m1
				-P "${PROJECT_SOURCE_DIR}/tests/VRsqClampF32VulkanProbeTest.cmake")
		set_tests_properties(lumen_graphics_vrsq_clamp_f32_vulkan_probe_native_metal_apple_m1 PROPERTIES
			LABELS "graphics;vulkan;vrsq-clamp;backend-only;native-metal;apple-m1"
			RUN_SERIAL TRUE
			TIMEOUT 90)
		add_test(NAME lumen_graphics_vrsq_clamp_f32_vulkan_probe_exhaustive_native_metal_apple_m1
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_A64_METAL_SHADER_FUNCTION_CACHE
				--unset=LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION
				--unset=LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE
				${CMAKE_COMMAND}
				"-DPROBE=$<TARGET_FILE:lumen_graphics_vrsq_clamp_f32_vulkan_probe>"
				-DMODE=exhaustive-native-metal-apple-m1
				-P "${PROJECT_SOURCE_DIR}/tests/VRsqClampF32VulkanProbeTest.cmake")
		set_tests_properties(lumen_graphics_vrsq_clamp_f32_vulkan_probe_exhaustive_native_metal_apple_m1 PROPERTIES
			LABELS "graphics;vulkan;vrsq-clamp;backend-only;native-metal;apple-m1;exhaustive"
			RUN_SERIAL TRUE
			TIMEOUT 180)
	endif()
	unset(LUMEN_VRSQ_CLAMP_F32_PROBE_ARCH)
endif()

if(LUMEN_WITH_VCVT_OFF_F32_I4_VULKAN_PROBE)
	if(CMAKE_CROSSCOMPILING OR ANDROID OR IOS)
		message(FATAL_ERROR
			"LUMEN_WITH_VCVT_OFF_F32_I4_VULKAN_PROBE is limited to native desktop builds")
	endif()
	if(WIN32)
		if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR MSVC_VERSION LESS 1930 OR
		   NOT LUMEN_BITNESS EQUAL 64 OR
		   NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022" OR
		   NOT CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
			message(FATAL_ERROR
				"The Windows V_CVT_OFF_F32_I4 Vulkan probe requires the Visual Studio 17 2022 generator, MSVC, and -A x64")
		endif()
	endif()
	add_executable(lumen_graphics_vcvt_off_f32_i4_vulkan_probe
		GraphicsVCvtOffF32I4VulkanProbe.cpp)
	target_include_directories(lumen_graphics_vcvt_off_f32_i4_vulkan_probe PRIVATE
		${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include)
	target_link_libraries(lumen_graphics_vcvt_off_f32_i4_vulkan_probe PRIVATE
		vulkan-1 spirv-tools)
	set(LUMEN_VCVT_OFF_F32_I4_PROBE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_VCVT_OFF_F32_I4_PROBE_ARCH)
		set(LUMEN_VCVT_OFF_F32_I4_PROBE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	add_test(NAME lumen_graphics_vcvt_off_f32_i4_vulkan_probe_shader_only
		COMMAND ${CMAKE_COMMAND}
			"-DPROBE=$<TARGET_FILE:lumen_graphics_vcvt_off_f32_i4_vulkan_probe>"
			-DMODE=shader-only
			-P "${PROJECT_SOURCE_DIR}/tests/VCvtOffF32I4VulkanProbeTest.cmake")
	set_tests_properties(lumen_graphics_vcvt_off_f32_i4_vulkan_probe_shader_only PROPERTIES
		LABELS "graphics;vulkan;vcvt-off-i4;backend-only;shader-only"
		TIMEOUT 10)
	if(WIN32)
		add_test(NAME lumen_graphics_vcvt_off_f32_i4_vulkan_probe_amd_13c0
			COMMAND ${CMAKE_COMMAND}
				"-DPROBE=$<TARGET_FILE:lumen_graphics_vcvt_off_f32_i4_vulkan_probe>"
				-DMODE=amd-13c0
				-P "${PROJECT_SOURCE_DIR}/tests/VCvtOffF32I4VulkanProbeTest.cmake")
		set_tests_properties(lumen_graphics_vcvt_off_f32_i4_vulkan_probe_amd_13c0 PROPERTIES
			LABELS "graphics;vulkan;vcvt-off-i4;backend-only;amd-13c0"
			RUN_SERIAL TRUE
			TIMEOUT 20)
	endif()
	if(MACOS AND LUMEN_APPLE_NATIVE_METAL AND
	   LUMEN_VCVT_OFF_F32_I4_PROBE_ARCH MATCHES "^(arm64|aarch64)$")
		target_compile_definitions(lumen_graphics_vcvt_off_f32_i4_vulkan_probe PRIVATE
			LUMEN_VCVT_OFF_F32_I4_PROBE_NATIVE_METAL_APPLE_M1=1)
		add_test(NAME lumen_graphics_vcvt_off_f32_i4_vulkan_probe_native_metal_apple_m1
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_A64_METAL_SHADER_FUNCTION_CACHE
				--unset=LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION
				--unset=LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE
				${CMAKE_COMMAND}
				"-DPROBE=$<TARGET_FILE:lumen_graphics_vcvt_off_f32_i4_vulkan_probe>"
				-DMODE=native-metal-apple-m1
				-P "${PROJECT_SOURCE_DIR}/tests/VCvtOffF32I4VulkanProbeTest.cmake")
		set_tests_properties(lumen_graphics_vcvt_off_f32_i4_vulkan_probe_native_metal_apple_m1 PROPERTIES
			LABELS "graphics;vulkan;vcvt-off-i4;backend-only;native-metal;apple-m1"
			RUN_SERIAL TRUE
			TIMEOUT 60)
	endif()
	unset(LUMEN_VCVT_OFF_F32_I4_PROBE_ARCH)
endif()

if(LUMEN_WITH_CPU_X64_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_cpu_x64_test CpuX64Test.cpp emulator/src/CpuX64.cpp emulator/src/CpuA64Dynarec.cpp)
	target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_CPU_X64_TEST)
	if(LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE
			LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1)
	endif()
	if(LUMEN_FAST_BLOCK_TELEMETRY)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_FAST_BLOCK_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_STARTUP_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
		find_package(Python3 COMPONENTS Interpreter REQUIRED)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_HOST_PC_PROFILE=1)
	endif()
	if(LUMEN_A64_DYNAREC_RFLAGS_PROFILE)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_RFLAGS_PROFILE=1)
	endif()
	if(LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_PHASE6CK_PROFILE=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_PHASE6CM_PROFILE=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
		target_compile_definitions(lumen_cpu_x64_test PRIVATE LUMEN_A64_DYNAREC_PHASE6CO_PROFILE=1)
	endif()
	target_include_directories(lumen_cpu_x64_test PRIVATE ${PROJECT_SOURCE_DIR}/emulator/include)
	target_link_libraries(lumen_cpu_x64_test PRIVATE Threads::Threads)
	if(LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2)
		target_link_libraries(lumen_cpu_x64_test PRIVATE core)
	endif()
	if(MACOS)
		# The integration corpus exercises the Apple immutable-RX code-read lease.
		# Other backends deliberately fail that lease closed and retain the canonical
		# CPU test without registering an integration test that cannot succeed.
		add_executable(lumen_cpu_x64_region_v2_test
			CpuX64RegionV2Test.cpp
			emulator/src/CpuX64.cpp
			emulator/src/CpuA64Dynarec.cpp)
		target_compile_definitions(lumen_cpu_x64_region_v2_test PRIVATE
			LUMEN_CPU_X64_TEST=1
			LUMEN_ENABLE_X64_A64_REGION_ENGINE_V2=1)
		target_include_directories(lumen_cpu_x64_region_v2_test PRIVATE
			${PROJECT_SOURCE_DIR}/emulator/include)
		target_link_libraries(lumen_cpu_x64_region_v2_test PRIVATE
			core Threads::Threads)
	endif()
	if(MSVC AND CLANG)
		target_compile_options(lumen_cpu_x64_test PRIVATE -Wno-deprecated-declarations)
		execute_process(
			COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
			OUTPUT_VARIABLE CLANG_RESOURCE_DIR
			OUTPUT_STRIP_TRAILING_WHITESPACE
		)
		set(CLANG_BUILTINS_ARCH x86_64)
		if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(ARM64|ARM64EC)$" OR
		   CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
			set(CLANG_BUILTINS_ARCH aarch64)
		endif()
		find_library(CLANG_BUILTINS_LIBRARY
			NAMES "clang_rt.builtins-${CLANG_BUILTINS_ARCH}"
			PATHS "${CLANG_RESOURCE_DIR}/lib/windows"
			NO_DEFAULT_PATH
			REQUIRED
		)
		target_link_libraries(lumen_cpu_x64_test PRIVATE "${CLANG_BUILTINS_LIBRARY}")
	endif()
	if(MACOS)
		add_test(NAME lumen_cpu_x64_region_v2
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_REGION_ENGINE_V2=1
			LUMEN_X64_A64_DYNAREC=0
			LUMEN_X64_FAST_BLOCKS=1
			LUMEN_X64_FAST_BLOCK_CHAIN=1
			LUMEN_X64_FAST_BLOCK_EXTENSIONS=1
			LUMEN_X64_FAST_BLOCK_HOT_THRESHOLD=2
			LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION=0
			$<TARGET_FILE:lumen_cpu_x64_region_v2_test>)
		set_tests_properties(lumen_cpu_x64_region_v2 PROPERTIES
			LABELS "cpu;region-v2;semantic"
			TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_region_v2_selector_off
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_REGION_ENGINE_V2=0
			LUMEN_X64_A64_DYNAREC=0
			LUMEN_X64_FAST_BLOCKS=1
			LUMEN_X64_FAST_BLOCK_CHAIN=1
			LUMEN_X64_FAST_BLOCK_EXTENSIONS=1
			LUMEN_X64_FAST_BLOCK_HOT_THRESHOLD=2
			LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION=0
			$<TARGET_FILE:lumen_cpu_x64_region_v2_test>
			--selector-off-only)
		set_tests_properties(lumen_cpu_x64_region_v2_selector_off PROPERTIES
			LABELS "cpu;region-v2;selector"
			TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_region_v2_selector_non_exact
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_REGION_ENGINE_V2=01
			LUMEN_X64_A64_DYNAREC=0
			LUMEN_X64_FAST_BLOCKS=1
			LUMEN_X64_FAST_BLOCK_CHAIN=1
			LUMEN_X64_FAST_BLOCK_EXTENSIONS=1
			LUMEN_X64_FAST_BLOCK_HOT_THRESHOLD=2
			LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION=0
			$<TARGET_FILE:lumen_cpu_x64_region_v2_test>
			--selector-off-only)
		set_tests_properties(lumen_cpu_x64_region_v2_selector_non_exact PROPERTIES
			LABELS "cpu;region-v2;selector"
			TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_region_v2_selector_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_REGION_ENGINE_V2
			LUMEN_X64_A64_DYNAREC=0
			LUMEN_X64_FAST_BLOCKS=1
			LUMEN_X64_FAST_BLOCK_CHAIN=1
			LUMEN_X64_FAST_BLOCK_EXTENSIONS=1
			LUMEN_X64_FAST_BLOCK_HOT_THRESHOLD=2
			LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION=0
			-- $<TARGET_FILE:lumen_cpu_x64_region_v2_test>
			--selector-off-only)
		set_tests_properties(lumen_cpu_x64_region_v2_selector_absent PROPERTIES
			LABELS "cpu;region-v2;selector"
			TIMEOUT 120)
	endif()
	add_test(NAME lumen_cpu_x64
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES
			--unset=LUMEN_X64_A64_DYNAREC_PACKED_256
			--unset=LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY
			--unset=LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY
			--unset=LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE
			--unset=LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION
			--unset=LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION
			--unset=LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE
			--unset=LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION
			--unset=LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION
			--unset=LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION
			--unset=LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION
			--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
			--unset=LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY
			--unset=LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			--unset=LUMEN_X64_A64_COMPACT_FASTOP_TIER
			-- $<TARGET_FILE:lumen_cpu_x64_test>)
	set_tests_properties(lumen_cpu_x64 PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_a64_compact_fastop_tier_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_COMPACT_FASTOP_TIER
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-compact-fastop-tier-selection-only)
	add_test(NAME lumen_cpu_x64_a64_compact_fastop_tier_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_COMPACT_FASTOP_TIER=
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-compact-fastop-tier-selection-only)
	add_test(NAME lumen_cpu_x64_a64_compact_fastop_tier_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_COMPACT_FASTOP_TIER=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-compact-fastop-tier-selection-only)
	add_test(NAME lumen_cpu_x64_a64_compact_fastop_tier_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_COMPACT_FASTOP_TIER=1
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-compact-fastop-tier-selection-only)
	add_test(NAME lumen_cpu_x64_a64_compact_fastop_tier_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_COMPACT_FASTOP_TIER=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-compact-fastop-tier-selection-only)
	add_test(NAME lumen_cpu_x64_a64_compact_fastop_tier_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_COMPACT_FASTOP_TIER=1
			--unset=LUMEN_X64_A64_DYNAREC
			--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
			--unset=LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY
			--unset=LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			--unset=LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-compact-fastop-tier-only)
	add_test(NAME lumen_cpu_x64_a64_compact_fastop_tier_full
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_COMPACT_FASTOP_TIER=1
			--unset=LUMEN_X64_A64_DYNAREC
			--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
			--unset=LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY
			--unset=LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			--unset=LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION
			-- $<TARGET_FILE:lumen_cpu_x64_test>)
	set_tests_properties(
		lumen_cpu_x64_a64_compact_fastop_tier_absent
		lumen_cpu_x64_a64_compact_fastop_tier_empty
		lumen_cpu_x64_a64_compact_fastop_tier_exact_zero
		lumen_cpu_x64_a64_compact_fastop_tier_exact_one
		lumen_cpu_x64_a64_compact_fastop_tier_malformed
		PROPERTIES LABELS "cpu;fast-block;a64-compact-tier;selector" TIMEOUT 120)
	set_tests_properties(
		lumen_cpu_x64_a64_compact_fastop_tier_candidate
		lumen_cpu_x64_a64_compact_fastop_tier_full
		PROPERTIES LABELS "cpu;fast-block;a64-compact-tier;semantic" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_hybrid_direct_entry_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-hybrid-direct-entry-selection-only)
	set_tests_properties(lumen_cpu_x64_hybrid_direct_entry_rollback
		PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_contextless_dense_entry_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-contextless-dense-entry-selection-only)
	set_tests_properties(lumen_cpu_x64_contextless_dense_entry_rollback
		PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_dense_victim_archive_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-victim-archive-selection-only)
	set_tests_properties(lumen_cpu_x64_dense_victim_archive_rollback
		PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_dense_victim_archive_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-victim-archive-selection-only)
	add_test(NAME lumen_cpu_x64_dense_victim_archive_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-victim-archive-selection-only)
	set_tests_properties(
		lumen_cpu_x64_dense_victim_archive_exact_zero
		lumen_cpu_x64_dense_victim_archive_malformed
		PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_dense_async_compilation_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-async-compilation-selection-only)
	add_test(NAME lumen_cpu_x64_dense_async_compilation_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-async-compilation-selection-only)
	add_test(NAME lumen_cpu_x64_dense_async_compilation_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-async-compilation-selection-only)
	set_tests_properties(
		lumen_cpu_x64_dense_async_compilation_rollback
		lumen_cpu_x64_dense_async_compilation_exact_zero
		lumen_cpu_x64_dense_async_compilation_malformed
		PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_dense_async_stability_admission_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-async-stability-admission-selection-only)
	add_test(NAME lumen_cpu_x64_dense_async_stability_admission_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-async-stability-admission-selection-only)
	add_test(NAME lumen_cpu_x64_dense_async_stability_admission_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-dense-async-stability-admission-selection-only)
	set_tests_properties(
		lumen_cpu_x64_dense_async_stability_admission_rollback
		lumen_cpu_x64_dense_async_stability_admission_exact_zero
		lumen_cpu_x64_dense_async_stability_admission_malformed
		PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_stable_negative_route_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-stable-negative-route-selection-only)
	add_test(NAME lumen_cpu_x64_stable_negative_route_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-stable-negative-route-selection-only)
	add_test(NAME lumen_cpu_x64_stable_negative_route_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-stable-negative-route-selection-only)
	set_tests_properties(
		lumen_cpu_x64_stable_negative_route_rollback
		lumen_cpu_x64_stable_negative_route_exact_zero
		lumen_cpu_x64_stable_negative_route_malformed
		PROPERTIES LABELS "cpu;dynarec;phase6cl;selector" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_profitable_block_admission_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-profitable-block-admission-selection-only)
	add_test(NAME lumen_cpu_x64_profitable_block_admission_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-profitable-block-admission-selection-only)
	add_test(NAME lumen_cpu_x64_profitable_block_admission_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-profitable-block-admission-selection-only)
	set_tests_properties(
		lumen_cpu_x64_profitable_block_admission_rollback
		lumen_cpu_x64_profitable_block_admission_exact_zero
		lumen_cpu_x64_profitable_block_admission_malformed
		PROPERTIES LABELS "cpu;dynarec;profitable-admission;selector" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_native_only_admission_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-native-only-admission-selection-only)
	add_test(NAME lumen_cpu_x64_native_only_admission_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-native-only-admission-selection-only)
	add_test(NAME lumen_cpu_x64_native_only_admission_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-native-only-admission-selection-only)
	set_tests_properties(
		lumen_cpu_x64_native_only_admission_rollback
		lumen_cpu_x64_native_only_admission_exact_zero
		lumen_cpu_x64_native_only_admission_malformed
		PROPERTIES LABELS "cpu;dynarec;native-only-admission;selector" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_guest_memory_operation_snapshot_elision_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-guest-memory-operation-snapshot-elision-selection-only)
	add_test(NAME lumen_cpu_x64_guest_memory_operation_snapshot_elision_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-guest-memory-operation-snapshot-elision-selection-only)
	add_test(NAME lumen_cpu_x64_guest_memory_operation_snapshot_elision_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-guest-memory-operation-snapshot-elision-selection-only)
	set_tests_properties(
		lumen_cpu_x64_guest_memory_operation_snapshot_elision_rollback
		lumen_cpu_x64_guest_memory_operation_snapshot_elision_exact_zero
		lumen_cpu_x64_guest_memory_operation_snapshot_elision_malformed
		PROPERTIES LABELS "cpu;dynarec;phase6cp;selector" TIMEOUT 120)
	if(APPLE_PLATFORM AND
	   (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
	    (NOT CMAKE_OSX_ARCHITECTURES AND
	     CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")) AND
	   NOT LUMEN_A64_DYNAREC_PHASE6CK_PROFILE AND
	   NOT LUMEN_A64_DYNAREC_PHASE6CM_PROFILE AND
	   NOT LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
		foreach(selection IN ITEMS absent 0 1 true empty)
			if(selection STREQUAL "absent")
				set(snapshot_selector --unset=LUMEN_X64_FAST_BLOCK_NOJIT_SNAPSHOT_ELISION)
			elseif(selection STREQUAL "empty")
				set(snapshot_selector LUMEN_X64_FAST_BLOCK_NOJIT_SNAPSHOT_ELISION=)
			else()
				set(snapshot_selector LUMEN_X64_FAST_BLOCK_NOJIT_SNAPSHOT_ELISION=${selection})
			endif()
			add_test(NAME lumen_cpu_x64_nojit_snapshot_elision_${selection}
				COMMAND ${CMAKE_COMMAND} -E env
					${snapshot_selector}
					LUMEN_X64_A64_DYNAREC=0
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=0
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-nojit-snapshot-elision-only)
			set_tests_properties(lumen_cpu_x64_nojit_snapshot_elision_${selection}
				PROPERTIES LABELS "cpu;fast-block;no-jit;snapshot;semantic;selector" TIMEOUT 120)
		endforeach()
		# A legacy dynarec selector cannot silently opt the no-JIT route in.
		add_test(NAME lumen_cpu_x64_nojit_snapshot_elision_legacy_isolation
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_FAST_BLOCK_NOJIT_SNAPSHOT_ELISION=0
				LUMEN_X64_A64_DYNAREC=0
				LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--fast-block-nojit-snapshot-elision-only)
		set_tests_properties(lumen_cpu_x64_nojit_snapshot_elision_legacy_isolation
			PROPERTIES LABELS "cpu;fast-block;no-jit;snapshot;semantic;selector" TIMEOUT 120)
		if(MACOS)
			foreach(route_order IN ITEMS native-then-nojit nojit-then-native)
				add_test(NAME lumen_cpu_x64_nojit_snapshot_elision_${route_order}
					COMMAND ${CMAKE_COMMAND} -E env
						LUMEN_X64_FAST_BLOCK_NOJIT_SNAPSHOT_ELISION=1
						LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
						LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
						-- $<TARGET_FILE:lumen_cpu_x64_test>
						--snapshot-coverage-${route_order}-only)
				set_tests_properties(lumen_cpu_x64_nojit_snapshot_elision_${route_order}
					PROPERTIES LABELS "cpu;fast-block;no-jit;snapshot;semantic;coverage" TIMEOUT 120)
			endforeach()
			add_test(NAME lumen_cpu_x64_nojit_snapshot_elision_native_isolation
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_FAST_BLOCK_NOJIT_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=0
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-nojit-snapshot-elision-native-isolation-only)
			set_tests_properties(lumen_cpu_x64_nojit_snapshot_elision_native_isolation
				PROPERTIES LABELS "cpu;fast-block;no-jit;snapshot;semantic;selector" TIMEOUT 120)
		endif()
		if(IOS)
			add_test(NAME lumen_cpu_x64_nojit_snapshot_elision_ios_default
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_FAST_BLOCK_NOJIT_SNAPSHOT_ELISION=1
					--unset=LUMEN_X64_A64_DYNAREC
					--unset=LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-nojit-snapshot-elision-default-only)
			set_tests_properties(lumen_cpu_x64_nojit_snapshot_elision_ios_default
				PROPERTIES LABELS "cpu;fast-block;no-jit;snapshot;semantic;ios" TIMEOUT 120)
		endif()
	endif()
	add_test(NAME lumen_cpu_x64_scalar_memory_fallthrough_admission_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-scalar-memory-fallthrough-admission-selection-only)
	add_test(NAME lumen_cpu_x64_scalar_memory_fallthrough_admission_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-scalar-memory-fallthrough-admission-selection-only)
	add_test(NAME lumen_cpu_x64_scalar_memory_fallthrough_admission_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-scalar-memory-fallthrough-admission-selection-only)
	set_tests_properties(
		lumen_cpu_x64_scalar_memory_fallthrough_admission_rollback
		lumen_cpu_x64_scalar_memory_fallthrough_admission_exact_zero
		lumen_cpu_x64_scalar_memory_fallthrough_admission_malformed
		PROPERTIES LABELS "cpu;dynarec;phase6cq;selector" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_dead_rflags_elision_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-dead-rflags-elision-selection-only)
	add_test(NAME lumen_cpu_x64_dead_rflags_elision_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-dead-rflags-elision-selection-only)
	add_test(NAME lumen_cpu_x64_dead_rflags_elision_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-dead-rflags-elision-selection-only)
	set_tests_properties(
		lumen_cpu_x64_dead_rflags_elision_rollback
		lumen_cpu_x64_dead_rflags_elision_exact_zero
		lumen_cpu_x64_dead_rflags_elision_malformed
		PROPERTIES LABELS "cpu;fast-block;phase6cs;selector" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_fast_block_dense_directory_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-dense-directory-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_dense_directory_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-dense-directory-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_dense_directory_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-dense-directory-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_dense_directory_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-dense-directory-selection-only)
	set_tests_properties(
		lumen_cpu_x64_fast_block_dense_directory_rollback
		lumen_cpu_x64_fast_block_dense_directory_empty
		lumen_cpu_x64_fast_block_dense_directory_exact_zero
		lumen_cpu_x64_fast_block_dense_directory_malformed
		PROPERTIES LABELS "cpu;fast-block;phase6da;selector" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_fast_block_inline_fixed_signature_match_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-inline-fixed-signature-match-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_inline_fixed_signature_match_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-inline-fixed-signature-match-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_inline_fixed_signature_match_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-inline-fixed-signature-match-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_inline_fixed_signature_match_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-inline-fixed-signature-match-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_inline_fixed_signature_match_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-inline-fixed-signature-match-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_inline_fixed_signature_match_comparator
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-inline-fixed-signature-match-comparator-only)
	set_tests_properties(
		lumen_cpu_x64_fast_block_inline_fixed_signature_match_absent
		lumen_cpu_x64_fast_block_inline_fixed_signature_match_empty
		lumen_cpu_x64_fast_block_inline_fixed_signature_match_exact_zero
		lumen_cpu_x64_fast_block_inline_fixed_signature_match_exact_one
		lumen_cpu_x64_fast_block_inline_fixed_signature_match_malformed
		PROPERTIES LABELS "cpu;fast-block;selector" TIMEOUT 120)
	set_tests_properties(
		lumen_cpu_x64_fast_block_inline_fixed_signature_match_comparator
		PROPERTIES LABELS "cpu;fast-block;semantic" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-unrolled-fixed-signature-batching-rollback-only)
	add_test(NAME lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
			LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING=
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-unrolled-fixed-signature-batching-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
			LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING=0
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-unrolled-fixed-signature-batching-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
			LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING=1
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-unrolled-fixed-signature-batching-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
			LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING=true
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-unrolled-fixed-signature-batching-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_dependency_off
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH
			LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING=1
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-unrolled-fixed-signature-batching-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_comparator
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-unrolled-fixed-signature-batching-comparator-only)
	add_test(NAME lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
			LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING=1
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-unrolled-fixed-signature-batching-only)
	set_tests_properties(
		lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_rollback
		lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_empty
		lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_exact_zero
		lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_exact_one
		lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_malformed
		lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_dependency_off
		PROPERTIES LABELS "cpu;fast-block;selector" TIMEOUT 120)
	set_tests_properties(
		lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_comparator
		lumen_cpu_x64_fast_block_unrolled_fixed_signature_batching_candidate
		PROPERTIES LABELS "cpu;fast-block;semantic" TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_fast_block_idempotent_mru_store_elision_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-idempotent-mru-store-elision-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_idempotent_mru_store_elision_empty
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION=
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-idempotent-mru-store-elision-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_idempotent_mru_store_elision_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-idempotent-mru-store-elision-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_idempotent_mru_store_elision_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION=1
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-idempotent-mru-store-elision-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_idempotent_mru_store_elision_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION=true
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-idempotent-mru-store-elision-selection-only)
	add_test(NAME lumen_cpu_x64_fast_block_idempotent_mru_store_elision_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
			--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--fast-block-idempotent-mru-store-elision-rollback-only)
	set_tests_properties(
		lumen_cpu_x64_fast_block_idempotent_mru_store_elision_absent
		lumen_cpu_x64_fast_block_idempotent_mru_store_elision_empty
		lumen_cpu_x64_fast_block_idempotent_mru_store_elision_exact_zero
		lumen_cpu_x64_fast_block_idempotent_mru_store_elision_exact_one
		lumen_cpu_x64_fast_block_idempotent_mru_store_elision_malformed
		lumen_cpu_x64_fast_block_idempotent_mru_store_elision_rollback
		PROPERTIES LABELS "cpu;fast-block;selector;imse" TIMEOUT 120)
	if(MACOS AND
	   (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
	    (NOT CMAKE_OSX_ARCHITECTURES AND
	     CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")))
		if(NOT LUMEN_FAST_BLOCK_TELEMETRY AND
		   NOT LUMEN_A64_DYNAREC_TELEMETRY AND
		   NOT LUMEN_A64_DYNAREC_STARTUP_TELEMETRY AND
		   NOT LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY AND
		   NOT LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY AND
		   NOT LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY AND
		   NOT LUMEN_A64_DYNAREC_HOST_PC_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_PHASE6CK_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_PHASE6CM_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_PHASE6CO_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_RFLAGS_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
			add_test(NAME lumen_cpu_x64_native_only_admission_default_finalizer
				COMMAND ${CMAKE_COMMAND} -E env
					--unset=LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--a64-dynarec-native-only-admission-default-finalizer-only)
			set_tests_properties(
				lumen_cpu_x64_native_only_admission_default_finalizer
				PROPERTIES LABELS "cpu;dynarec;native-only-admission;default;finalizer" TIMEOUT 120)
			add_test(NAME lumen_cpu_x64_dead_rflags_elision_candidate
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-dead-rflags-elision-only)
			set_tests_properties(
				lumen_cpu_x64_dead_rflags_elision_candidate
				PROPERTIES LABELS "cpu;fast-block;phase6cs;semantic" TIMEOUT 120)
			add_test(NAME lumen_cpu_x64_fast_block_dense_directory_candidate
				COMMAND ${CMAKE_COMMAND} -E env
					--unset=LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION
					--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-dense-directory-only)
			set_tests_properties(
				lumen_cpu_x64_fast_block_dense_directory_candidate
				PROPERTIES LABELS "cpu;fast-block;phase6da;semantic" TIMEOUT 120)
			add_test(NAME lumen_cpu_x64_fast_block_dense_directory_mismatch
				COMMAND ${CMAKE_COMMAND} -E env
					--unset=LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION
					--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-dense-directory-mismatch-only)
			set_tests_properties(
				lumen_cpu_x64_fast_block_dense_directory_mismatch
				PROPERTIES LABELS "cpu;fast-block;phase6da;semantic" TIMEOUT 120)
			# Run the complete CPU matrix with 6DA isolated. The focused candidate
			# test above executes and finalizes the retained 6CL+6CP runtimes too;
			# the legacy cache-policy oracle intentionally assumes those routes OFF.
			add_test(NAME lumen_cpu_x64_fast_block_dense_directory_full_candidate
				COMMAND ${CMAKE_COMMAND} -E env
					--unset=LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT
					--unset=LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE
					--unset=LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE
					--unset=LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES
					--unset=LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY
					--unset=LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION
					--unset=LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION
					--unset=LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE
					--unset=LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE
					--unset=LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING
					--unset=LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE
					--unset=LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI
					--unset=LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY
					--unset=LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT
					--unset=LUMEN_X64_A64_DYNAREC_PACKED_256
					--unset=LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE
					--unset=LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION
					--unset=LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION
					--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
					LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>)
			set_tests_properties(
				lumen_cpu_x64_fast_block_dense_directory_full_candidate
				PROPERTIES LABELS "cpu;fast-block;phase6da;semantic;full" TIMEOUT 120)
			add_test(NAME lumen_cpu_x64_fast_block_idempotent_mru_store_elision_historical
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=0
					LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
					--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
					LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-idempotent-mru-store-elision-historical-only)
			add_test(NAME lumen_cpu_x64_fast_block_idempotent_mru_store_elision_full_candidate
				COMMAND ${CMAKE_COMMAND} -E env
					--unset=LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION
					--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1
					LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
					--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
					LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-idempotent-mru-store-elision-only)
			set_tests_properties(
				lumen_cpu_x64_fast_block_idempotent_mru_store_elision_historical
				lumen_cpu_x64_fast_block_idempotent_mru_store_elision_full_candidate
				PROPERTIES LABELS "cpu;fast-block;imse;semantic;full" TIMEOUT 120)
			add_test(NAME lumen_cpu_x64_fast_block_ufsb_imse_both_on
				COMMAND ${CMAKE_COMMAND} -E env
					--unset=LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION
					--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1
					LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
					LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING=1
					LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-ufsb-imse-both-on-only)
			set_tests_properties(lumen_cpu_x64_fast_block_ufsb_imse_both_on
				PROPERTIES LABELS "cpu;fast-block;ufsb;imse;semantic;full" TIMEOUT 120)
			add_test(NAME lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_absent
				COMMAND ${CMAKE_COMMAND} -E env
					--unset=LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-callback-free-ordinary-specialization-selection-only)
			add_test(NAME lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_empty
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION=
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-callback-free-ordinary-specialization-selection-only)
			add_test(NAME lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_exact_zero
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION=0
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-callback-free-ordinary-specialization-selection-only)
			add_test(NAME lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_exact_one
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-callback-free-ordinary-specialization-selection-only)
			add_test(NAME lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_malformed
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION=true
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-callback-free-ordinary-specialization-selection-only)
			set_tests_properties(
				lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_absent
				lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_empty
				lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_exact_zero
				lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_exact_one
				lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_malformed
				PROPERTIES LABELS "cpu;fast-block;fcfos;selector" TIMEOUT 120)
			add_test(NAME lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_candidate
				COMMAND ${CMAKE_COMMAND} -E env
					--unset=LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION
					--unset=LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1
					LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH=1
					--unset=LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING
					--unset=LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION
					LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--fast-block-callback-free-ordinary-specialization-only)
			set_tests_properties(
				lumen_cpu_x64_fast_block_callback_free_ordinary_specialization_candidate
				PROPERTIES LABELS "cpu;fast-block;fcfos;semantic;full" TIMEOUT 120)
		endif()
		add_test(NAME lumen_cpu_x64_guest_memory_operation_snapshot_elision_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
				LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-guest-memory-operation-snapshot-elision-only)
		set_tests_properties(
			lumen_cpu_x64_guest_memory_operation_snapshot_elision_candidate
			PROPERTIES LABELS "cpu;dynarec;phase6cp;semantic" TIMEOUT 120)
		if(NOT LUMEN_A64_DYNAREC_PHASE6CK_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_PHASE6CM_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_PHASE6CO_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_RFLAGS_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
			add_test(NAME lumen_cpu_x64_scalar_memory_fallthrough_admission_candidate
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC=1
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION=1
					LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
					LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--a64-dynarec-scalar-memory-fallthrough-admission-only)
			add_test(NAME lumen_cpu_x64_scalar_memory_fallthrough_admission_mismatch_after_success
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC=1
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION=1
					LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
					LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--a64-dynarec-scalar-memory-fallthrough-admission-mismatch-only)
			set_tests_properties(
				lumen_cpu_x64_scalar_memory_fallthrough_admission_candidate
				lumen_cpu_x64_scalar_memory_fallthrough_admission_mismatch_after_success
				PROPERTIES LABELS "cpu;dynarec;phase6cq;semantic" TIMEOUT 120)
		endif()
		add_test(NAME lumen_cpu_x64_stable_negative_route_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=0
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=0
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-stable-negative-route-only)
		add_test(NAME lumen_cpu_x64_stable_negative_route_dense_off
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=0
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=0
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=0
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-stable-negative-route-only)
		set_tests_properties(
			lumen_cpu_x64_stable_negative_route_candidate
			lumen_cpu_x64_stable_negative_route_dense_off
			PROPERTIES LABELS "cpu;dynarec;phase6cl;semantic" TIMEOUT 120)
		if(NOT LUMEN_A64_DYNAREC_PHASE6CK_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_PHASE6CM_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_PHASE6CO_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_RFLAGS_PROFILE AND
		   NOT LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
			add_test(NAME lumen_cpu_x64_profitable_block_admission_candidate
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC=1
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
					LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
					LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
					LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
					LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION=0
					LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--a64-dynarec-profitable-block-admission-only)
			set_tests_properties(
				lumen_cpu_x64_profitable_block_admission_candidate
				PROPERTIES LABELS "cpu;dynarec;profitable-admission;semantic" TIMEOUT 120)
			add_test(NAME lumen_cpu_x64_native_only_admission_candidate
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC=1
					LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE=1
					LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION=1
					LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
					LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
					LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
					LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
					LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION=0
					LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION=0
					LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION=1
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--a64-dynarec-native-only-admission-only)
			set_tests_properties(
				lumen_cpu_x64_native_only_admission_candidate
				PROPERTIES LABELS "cpu;dynarec;native-only-admission;semantic" TIMEOUT 180)
		endif()
		add_test(NAME lumen_cpu_x64_hybrid_direct_entry_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=1
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
				LUMEN_X64_A64_DYNAREC_PACKED_256=0
				LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE=0
				LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE=0
				LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-hybrid-direct-entry-only)
		set_tests_properties(lumen_cpu_x64_hybrid_direct_entry_candidate
			PROPERTIES TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_dense_victim_archive_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
				LUMEN_X64_A64_DYNAREC_PACKED_256=0
				LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE=0
				LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE=0
				LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-victim-archive-only)
		set_tests_properties(lumen_cpu_x64_dense_victim_archive_candidate
			PROPERTIES TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_dense_victim_archive_dependency_off
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=0
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-victim-archive-selection-only)
		add_test(NAME lumen_cpu_x64_dense_victim_archive_hybrid_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-victim-archive-selection-only)
		add_test(NAME lumen_cpu_x64_dense_victim_archive_contextless_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=1
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-victim-archive-selection-only)
		set_tests_properties(
			lumen_cpu_x64_dense_victim_archive_dependency_off
			lumen_cpu_x64_dense_victim_archive_hybrid_conflict
			lumen_cpu_x64_dense_victim_archive_contextless_conflict
			PROPERTIES TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_dense_async_compilation_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=0
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
				LUMEN_X64_A64_DYNAREC_PACKED_256=0
				LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE=0
				LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE=0
				LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-compilation-only)
		add_test(NAME lumen_cpu_x64_dense_async_compilation_dependency_off
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=0
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-compilation-selection-only)
		add_test(NAME lumen_cpu_x64_dense_async_compilation_archive_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-compilation-selection-only)
		add_test(NAME lumen_cpu_x64_dense_async_compilation_hybrid_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=1
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-compilation-selection-only)
		add_test(NAME lumen_cpu_x64_dense_async_compilation_contextless_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=1
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-compilation-selection-only)
		set_tests_properties(
			lumen_cpu_x64_dense_async_compilation_candidate
			lumen_cpu_x64_dense_async_compilation_dependency_off
			lumen_cpu_x64_dense_async_compilation_archive_conflict
			lumen_cpu_x64_dense_async_compilation_hybrid_conflict
			lumen_cpu_x64_dense_async_compilation_contextless_conflict
			PROPERTIES TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_dense_async_stability_admission_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
				LUMEN_X64_A64_DYNAREC_PACKED_256=0
				LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE=0
				LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE=0
				LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-stability-admission-only)
		add_test(NAME lumen_cpu_x64_dense_async_stability_admission_async_off
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=0
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-stability-admission-selection-only)
		add_test(NAME lumen_cpu_x64_dense_async_stability_admission_dense_off
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=0
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-stability-admission-selection-only)
		add_test(NAME lumen_cpu_x64_dense_async_stability_admission_archive_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-stability-admission-selection-only)
		add_test(NAME lumen_cpu_x64_dense_async_stability_admission_hybrid_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=1
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-stability-admission-selection-only)
		add_test(NAME lumen_cpu_x64_dense_async_stability_admission_contextless_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=1
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=1
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-dense-async-stability-admission-selection-only)
		set_tests_properties(
			lumen_cpu_x64_dense_async_stability_admission_candidate
			lumen_cpu_x64_dense_async_stability_admission_async_off
			lumen_cpu_x64_dense_async_stability_admission_dense_off
			lumen_cpu_x64_dense_async_stability_admission_archive_conflict
			lumen_cpu_x64_dense_async_stability_admission_hybrid_conflict
			lumen_cpu_x64_dense_async_stability_admission_contextless_conflict
			PROPERTIES TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_contextless_dense_entry_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
				LUMEN_X64_A64_DYNAREC_PACKED_256=0
				LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE=0
				LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE=0
				LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-contextless-dense-entry-only)
		set_tests_properties(lumen_cpu_x64_contextless_dense_entry_candidate
			PROPERTIES TIMEOUT 120)
		add_test(NAME lumen_cpu_x64_contextless_dense_entry_dependency_off
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-contextless-dense-entry-selection-only)
		add_test(NAME lumen_cpu_x64_contextless_dense_entry_hybrid_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=1
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=1
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-contextless-dense-entry-selection-only)
		set_tests_properties(
			lumen_cpu_x64_contextless_dense_entry_dependency_off
			lumen_cpu_x64_contextless_dense_entry_hybrid_conflict
			PROPERTIES TIMEOUT 120)
	endif()
	if(LUMEN_A64_DYNAREC_RFLAGS_PROFILE)
		add_test(NAME lumen_cpu_x64_a64_dynarec_rflags_profile
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_SEPARATE_NEGATIVE_CACHE=1
				LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION=1
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE=1
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=1024
				LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=1
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
				LUMEN_X64_A64_DYNAREC_PACKED_256=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-rflags-profile-only)
		set_tests_properties(lumen_cpu_x64_a64_dynarec_rflags_profile
			PROPERTIES TIMEOUT 120)
	endif()
	if(LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
		add_test(NAME lumen_cpu_x64_a64_dynarec_gpr_cache_profile
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_FAST_BLOCKS=1
				LUMEN_X64_FAST_BLOCK_CHAIN=1
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_SEPARATE_NEGATIVE_CACHE=1
				LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION=1
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE=1
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=1024
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=1
				LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=1
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
				LUMEN_X64_A64_DYNAREC_PACKED_256=0
				LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE=0
				-- $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-gpr-cache-profile-only)
		set_tests_properties(lumen_cpu_x64_a64_dynarec_gpr_cache_profile
			PROPERTIES TIMEOUT 120)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
		find_package(Python3 COMPONENTS Interpreter REQUIRED)
		add_test(NAME lumen_cpu_x64_a64_dynarec_phase6ck_profile
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC=1
				LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION=0
				LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION=0
				LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY=0
				LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY=0
				LUMEN_X64_A64_DYNAREC_DENSE_PROFILE=1 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6ck-profile-only)
		set_tests_properties(lumen_cpu_x64_a64_dynarec_phase6ck_profile
			PROPERTIES LABELS "cpu;dynarec;phase6ck;profile" TIMEOUT 120)
		add_test(NAME lumen_phase6ck_profile_report_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6ck_profile_report.py"
				--self-test)
		add_test(NAME lumen_phase6ck_profile_live_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6ck_profile_live.py"
				--self-test)
		add_test(NAME lumen_persistent_performance_harness_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/persistent_performance_harness.py"
				--self-test)
		add_test(NAME lumen_persistent_performance_registry_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/persistent_performance_registry.py"
				--self-test)
		add_test(NAME lumen_persistent_performance_report_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/persistent_performance_report.py"
				--self-test)
		set_tests_properties(
			lumen_phase6ck_profile_report_self_test
			lumen_phase6ck_profile_live_self_test
			lumen_persistent_performance_harness_self_test
			lumen_persistent_performance_registry_self_test
			lumen_persistent_performance_report_self_test
			PROPERTIES LABELS "phase6ck;performance;tools" TIMEOUT 120)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
		find_package(Python3 COMPONENTS Interpreter REQUIRED)
		add_test(NAME lumen_phase6cm_profile_report_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6cm_profile_report.py"
				--self-test)
		add_test(NAME lumen_phase6cm_profile_live_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6cm_profile_live.py"
				--self-test)
		set_tests_properties(
			lumen_phase6cm_profile_report_self_test
			lumen_phase6cm_profile_live_self_test
			PROPERTIES LABELS "phase6cm;tools" TIMEOUT 120)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
		find_package(Python3 COMPONENTS Interpreter REQUIRED)
		add_test(NAME lumen_phase6co_profile_report_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6co_profile_report.py"
				--self-test)
		add_test(NAME lumen_phase6co_profile_live_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6co_profile_live.py"
				--self-test)
		set_tests_properties(
			lumen_phase6co_profile_report_self_test
			lumen_phase6co_profile_live_self_test
			PROPERTIES LABELS "phase6co;tools" TIMEOUT 120)
	endif()
	set(LUMEN_MACOS_ARM64_PACKED_256_TEST OFF)
	if(MACOS)
		if("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "arm64" OR
		   ("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "" AND
		    CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"))
			set(LUMEN_MACOS_ARM64_PACKED_256_TEST ON)
		endif()
	endif()
	if(LUMEN_MACOS_ARM64_PACKED_256_TEST)
		set(LUMEN_CPU_X64_PACKED_256_MATRIX_TESTS)
		foreach(PACKED_256_VALIDATOR IN ITEMS eager branch_local)
			if(PACKED_256_VALIDATOR STREQUAL "branch_local")
				set(PACKED_256_BRANCH_LOCAL 1)
			else()
				set(PACKED_256_BRANCH_LOCAL 0)
			endif()
			foreach(PACKED_256_BATCH IN ITEMS 0 1)
				foreach(PACKED_256_LEAF IN ITEMS 0 1)
					foreach(PACKED_256_CONTEXT IN ITEMS 0 1)
						set(PACKED_256_MATRIX_TEST
							lumen_cpu_x64_packed_256_candidate_${PACKED_256_VALIDATOR}_batch${PACKED_256_BATCH}_leaf${PACKED_256_LEAF}_context${PACKED_256_CONTEXT})
						add_test(NAME ${PACKED_256_MATRIX_TEST}
							COMMAND ${CMAKE_COMMAND} -E env
								LUMEN_X64_A64_DYNAREC=1
								LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=${PACKED_256_BRANCH_LOCAL}
								LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=${PACKED_256_BATCH}
								LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=${PACKED_256_LEAF}
								LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=${PACKED_256_CONTEXT}
								LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
								LUMEN_X64_A64_DYNAREC_PACKED_256=1
								LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
								-- $<TARGET_FILE:lumen_cpu_x64_test>
								--a64-dynarec-packed-256-only)
						list(APPEND LUMEN_CPU_X64_PACKED_256_MATRIX_TESTS
							${PACKED_256_MATRIX_TEST})
					endforeach()
				endforeach()
			endforeach()
		endforeach()
		set_tests_properties(${LUMEN_CPU_X64_PACKED_256_MATRIX_TESTS}
			PROPERTIES TIMEOUT 120)
		unset(PACKED_256_MATRIX_TEST)
		unset(PACKED_256_CONTEXT)
		unset(PACKED_256_LEAF)
		unset(PACKED_256_BATCH)
		unset(PACKED_256_BRANCH_LOCAL)
		unset(PACKED_256_VALIDATOR)
		unset(LUMEN_CPU_X64_PACKED_256_MATRIX_TESTS)
	endif()
	unset(LUMEN_MACOS_ARM64_PACKED_256_TEST)
	add_test(NAME lumen_cpu_x64_direct_context_reuse_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-direct-context-reuse-only)
	add_test(NAME lumen_cpu_x64_direct_context_reuse_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-direct-context-reuse-only)
	set(LUMEN_DIRECT_CONTEXT_REUSE_MATRIX_TESTS)
	foreach(DIRECT_CONTEXT_BATCH IN ITEMS 0 1)
		foreach(DIRECT_CONTEXT_LEAF IN ITEMS 0 1)
			set(DIRECT_CONTEXT_MATRIX_TEST
				lumen_cpu_x64_direct_context_reuse_candidate_batch${DIRECT_CONTEXT_BATCH}_leaf${DIRECT_CONTEXT_LEAF})
			add_test(NAME ${DIRECT_CONTEXT_MATRIX_TEST}
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
					LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=${DIRECT_CONTEXT_BATCH}
					LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=${DIRECT_CONTEXT_LEAF}
					-- $<TARGET_FILE:lumen_cpu_x64_test>
					--a64-dynarec-direct-context-reuse-only)
			list(APPEND LUMEN_DIRECT_CONTEXT_REUSE_MATRIX_TESTS
				${DIRECT_CONTEXT_MATRIX_TEST})
		endforeach()
	endforeach()
	set_tests_properties(
		lumen_cpu_x64_direct_context_reuse_rollback
		lumen_cpu_x64_direct_context_reuse_candidate
		${LUMEN_DIRECT_CONTEXT_REUSE_MATRIX_TESTS}
		PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_x64_direct_block_binding_rollback
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-direct-block-binding-only)
	add_test(NAME lumen_cpu_x64_direct_block_binding_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=1
			LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=0
			LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
			-- $<TARGET_FILE:lumen_cpu_x64_test>
			--a64-dynarec-direct-block-binding-only)
	set(LUMEN_DIRECT_BLOCK_BINDING_MATRIX_TESTS)
	foreach(DIRECT_BINDING_CONTEXT IN ITEMS 0 1)
		foreach(DIRECT_BINDING_BATCH IN ITEMS 0 1)
			foreach(DIRECT_BINDING_LEAF IN ITEMS 0 1)
				set(DIRECT_BINDING_MATRIX_TEST
					lumen_cpu_x64_direct_block_binding_candidate_context${DIRECT_BINDING_CONTEXT}_batch${DIRECT_BINDING_BATCH}_leaf${DIRECT_BINDING_LEAF})
				add_test(NAME ${DIRECT_BINDING_MATRIX_TEST}
					COMMAND ${CMAKE_COMMAND} -E env
						LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING=1
						LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=0
						LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE=0
						LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=${DIRECT_BINDING_CONTEXT}
						LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=${DIRECT_BINDING_BATCH}
						LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=${DIRECT_BINDING_LEAF}
						-- $<TARGET_FILE:lumen_cpu_x64_test>
						--a64-dynarec-direct-block-binding-only)
				list(APPEND LUMEN_DIRECT_BLOCK_BINDING_MATRIX_TESTS
					${DIRECT_BINDING_MATRIX_TEST})
			endforeach()
		endforeach()
	endforeach()
	set_tests_properties(
		lumen_cpu_x64_direct_block_binding_rollback
		lumen_cpu_x64_direct_block_binding_candidate
		${LUMEN_DIRECT_BLOCK_BINDING_MATRIX_TESTS}
		PROPERTIES TIMEOUT 120)
	if(LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY)
		add_test(NAME lumen_cpu_x64_cross_owner_core
			COMMAND $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-cross-owner-telemetry-core-only)
		foreach(CROSS_OWNER_VALUE IN ITEMS empty zero leading_one malformed)
			if(CROSS_OWNER_VALUE STREQUAL "empty")
				set(CROSS_OWNER_ENV_VALUE "")
			elseif(CROSS_OWNER_VALUE STREQUAL "zero")
				set(CROSS_OWNER_ENV_VALUE "0")
			elseif(CROSS_OWNER_VALUE STREQUAL "leading_one")
				set(CROSS_OWNER_ENV_VALUE "01")
			else()
				set(CROSS_OWNER_ENV_VALUE "garbage")
			endif()
			add_test(NAME lumen_cpu_x64_cross_owner_env_${CROSS_OWNER_VALUE}
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC_CROSS_OWNER_TELEMETRY=${CROSS_OWNER_ENV_VALUE}
					--
					$<TARGET_FILE:lumen_cpu_x64_test>
					--a64-dynarec-cross-owner-env-disabled-only)
		endforeach()
		add_test(NAME lumen_cpu_x64_cross_owner_env_absent
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC_CROSS_OWNER_TELEMETRY
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-cross-owner-env-disabled-only)
		add_test(NAME lumen_cpu_x64_cross_owner_env_exact_one
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_CROSS_OWNER_TELEMETRY=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-cross-owner-env-enabled-only)
		set_tests_properties(
			lumen_cpu_x64_cross_owner_core
			lumen_cpu_x64_cross_owner_env_empty
			lumen_cpu_x64_cross_owner_env_zero
			lumen_cpu_x64_cross_owner_env_leading_one
			lumen_cpu_x64_cross_owner_env_malformed
			lumen_cpu_x64_cross_owner_env_absent
			lumen_cpu_x64_cross_owner_env_exact_one
			PROPERTIES TIMEOUT 120)
		unset(CROSS_OWNER_ENV_VALUE)
	endif()
	if(MACOS AND NOT LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY AND
	   (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
	    (NOT CMAKE_OSX_ARCHITECTURES AND
	     CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")))
		find_package(Python3 COMPONENTS Interpreter REQUIRED)
		add_test(NAME lumen_cpu_x64_compiled_archive_geometry_absent
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-geometry-only)
		add_test(NAME lumen_cpu_x64_compiled_archive_geometry_empty
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-geometry-only)
		add_test(NAME lumen_cpu_x64_compiled_archive_geometry_exact_512
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=512
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-geometry-only)
		add_test(NAME lumen_cpu_x64_compiled_archive_geometry_exact_1024
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=1024
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-geometry-only)
		add_test(NAME lumen_cpu_x64_compiled_archive_geometry_malformed
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=1024x
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-geometry-only)
		add_test(NAME lumen_cpu_x64_compiled_archive_k512
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=0
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=512
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-only)
		add_test(NAME lumen_cpu_x64_compiled_archive_k1024
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=0
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=1024
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-only)
		add_test(NAME lumen_phase6am_ab_runner_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6am_ab.py" --self-test)
		add_test(NAME lumen_phase6am_bench_runner_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6am_bench.py" --self-test)
		add_test(NAME lumen_cpu_x64_verified_ir_absent
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-selection-only)
		add_test(NAME lumen_cpu_x64_verified_ir_empty
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-selection-only)
		add_test(NAME lumen_cpu_x64_verified_ir_exact_zero
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=0
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-selection-only)
		add_test(NAME lumen_cpu_x64_verified_ir_exact_one
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-selection-only)
		add_test(NAME lumen_cpu_x64_verified_ir_malformed
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=01
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-selection-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_absent
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-lazy-ir-storage-selection-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_empty
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-lazy-ir-storage-selection-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_exact_zero
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=0
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-lazy-ir-storage-selection-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_exact_one
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-lazy-ir-storage-selection-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_malformed
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=yes
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-lazy-ir-storage-selection-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_runtime_baseline
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=0
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-runtime-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_runtime_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-runtime-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_raw_rollback
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=0
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-runtime-only)
		add_test(NAME lumen_cpu_x64_lazy_ir_storage_full_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=1
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_cpu_x64_branch_local_ir_validation_full_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=1
				LUMEN_X64_A64_DYNAREC_LAZY_IR_STORAGE=1
				LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_cpu_x64_verified_ir_runtime_baseline
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=0
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-runtime-only)
		add_test(NAME lumen_cpu_x64_verified_ir_runtime_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-verified-ir-runtime-only)
		add_test(NAME lumen_cpu_x64_verified_ir_archive_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=512
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-only)
		add_test(NAME lumen_cpu_x64_verified_ir_full_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_cpu_x64_jit_page_recycle_full_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_cpu_x64_jit_page_recycle_archive_retired_artifacts
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=1
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-only)
		add_test(NAME lumen_phase6ao_ab_runner_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6ao_ab.py" --self-test)
		add_test(NAME lumen_phase6ao_bench_runner_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6ao_bench.py" --self-test)
		set(PHASE6AM_CTESTS
			lumen_cpu_x64_compiled_archive_geometry_absent
			lumen_cpu_x64_compiled_archive_geometry_empty
			lumen_cpu_x64_compiled_archive_geometry_exact_512
			lumen_cpu_x64_compiled_archive_geometry_exact_1024
			lumen_cpu_x64_compiled_archive_geometry_malformed
			lumen_cpu_x64_compiled_archive_k512
			lumen_cpu_x64_compiled_archive_k1024
			lumen_phase6am_ab_runner_self_test
			lumen_phase6am_bench_runner_self_test
			lumen_cpu_x64_verified_ir_absent
			lumen_cpu_x64_verified_ir_empty
			lumen_cpu_x64_verified_ir_exact_zero
			lumen_cpu_x64_verified_ir_exact_one
			lumen_cpu_x64_verified_ir_malformed
			lumen_cpu_x64_lazy_ir_storage_absent
			lumen_cpu_x64_lazy_ir_storage_empty
			lumen_cpu_x64_lazy_ir_storage_exact_zero
			lumen_cpu_x64_lazy_ir_storage_exact_one
			lumen_cpu_x64_lazy_ir_storage_malformed
			lumen_cpu_x64_lazy_ir_storage_runtime_baseline
			lumen_cpu_x64_lazy_ir_storage_runtime_candidate
			lumen_cpu_x64_lazy_ir_storage_raw_rollback
			lumen_cpu_x64_lazy_ir_storage_full_candidate
			lumen_cpu_x64_branch_local_ir_validation_full_candidate
			lumen_cpu_x64_verified_ir_runtime_baseline
			lumen_cpu_x64_verified_ir_runtime_candidate
			lumen_cpu_x64_verified_ir_archive_candidate
			lumen_cpu_x64_verified_ir_full_candidate
			lumen_cpu_x64_jit_page_recycle_full_candidate
			lumen_cpu_x64_jit_page_recycle_archive_retired_artifacts
			lumen_phase6ao_ab_runner_self_test
			lumen_phase6ao_bench_runner_self_test)
		set_tests_properties(${PHASE6AM_CTESTS} PROPERTIES TIMEOUT 120)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
		find_package(Python3 COMPONENTS Interpreter REQUIRED)
		add_test(NAME lumen_cpu_x64_phase6af_core
			COMMAND $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-core-only)
		add_test(NAME lumen_cpu_x64_phase6af_env_absent
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=1
				--unset=LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_env_empty
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=1
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_env_zero
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=0
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=1
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_env_malformed
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=yes
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=1
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_seed_absent
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=1
				--unset=LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_seed_zero
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=1
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=0
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_seed_empty
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=1
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_seed_overflow
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=1
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=18446744073709551616
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_seed_malformed
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=1
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=1x
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_startup_conflict
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=1
				LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=1
				LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=1 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6af-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6af_report_parser
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6af_report.py"
				--run-report-binary $<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_phase6af_report_parser_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6af_report.py" --self-test)
		set(PHASE6AF_CTESTS
			lumen_cpu_x64_phase6af_core
			lumen_cpu_x64_phase6af_env_absent
			lumen_cpu_x64_phase6af_env_empty
			lumen_cpu_x64_phase6af_env_zero
			lumen_cpu_x64_phase6af_env_malformed
			lumen_cpu_x64_phase6af_seed_absent
			lumen_cpu_x64_phase6af_seed_zero
			lumen_cpu_x64_phase6af_seed_empty
			lumen_cpu_x64_phase6af_seed_overflow
			lumen_cpu_x64_phase6af_seed_malformed
			lumen_cpu_x64_phase6af_startup_conflict
			lumen_cpu_x64_phase6af_report_parser
			lumen_phase6af_report_parser_self_test)
		if(MACOS AND (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" OR
		              CMAKE_OSX_ARCHITECTURES MATCHES "(^|;)arm64(;|$)"))
			add_test(NAME lumen_cpu_x64_phase6af_env_exact_one
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY=1
					LUMEN_X64_A64_DYNAREC_PHASE6AF_SEED=1
					LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY=0 --
					$<TARGET_FILE:lumen_cpu_x64_test>
					--a64-dynarec-phase6af-telemetry-env-enabled-only)
			list(APPEND PHASE6AF_CTESTS
				lumen_cpu_x64_phase6af_env_exact_one)
		endif()
		set_tests_properties(${PHASE6AF_CTESTS} PROPERTIES TIMEOUT 120)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
		add_test(NAME lumen_cpu_x64_phase6al_core
			COMMAND $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6al-telemetry-core-only)
		add_test(NAME lumen_cpu_x64_phase6al_env_absent
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6al-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6al_env_empty
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY= --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6al-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6al_env_zero
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY=0 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6al-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6al_env_malformed
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY=01 --
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6al-telemetry-env-disabled-only)
		add_test(NAME lumen_cpu_x64_phase6al_env_exact_one
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY=1
				LUMEN_X64_A64_DYNAREC_PHASE6AL_INTERVAL=100000
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=512
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_DEFAULT_REQUESTED
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				--unset=LUMEN_X64_A64_DYNAREC_SEPARATE_NEGATIVE_CACHE
				--unset=LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY
				--unset=LUMEN_X64_A64_DYNAREC_TELEMETRY
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6al-telemetry-env-enabled-only)
		add_test(NAME lumen_cpu_x64_phase6al_geometry_forced_k512
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC
				--unset=LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE
				--unset=LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION
				LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=1024
				--
				$<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-compiled-archive-geometry-only)
		add_test(NAME lumen_cpu_x64_phase6al_report_parser
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6al_report.py"
				--run-report-binary $<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_cpu_x64_phase6al_primary_report_parser
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6al_report.py"
				--run-primary-report-binary $<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_cpu_x64_phase6al_hidden_worker_invalid
			COMMAND $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6al-hidden-worker-invalid-only)
		add_test(NAME lumen_cpu_x64_phase6al_primary_after_worker_parser
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6al_report.py"
				--run-primary-after-worker-binary
				$<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_cpu_x64_phase6al_post_compile_boundary
			COMMAND $<TARGET_FILE:lumen_cpu_x64_test>
				--a64-dynarec-phase6al-post-compile-boundary-only)
		add_test(NAME lumen_cpu_x64_phase6al_archive_zero_rollback
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6al_report.py"
				--run-rollback-binary $<TARGET_FILE:lumen_cpu_x64_test>)
		add_test(NAME lumen_phase6al_report_parser_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6al_report.py" --self-test)
		add_test(NAME lumen_phase6al_live_runner_self_test
			COMMAND ${Python3_EXECUTABLE}
				"${PROJECT_SOURCE_DIR}/tools/phase6al_live.py" --self-test)
		set_tests_properties(
			lumen_cpu_x64_phase6al_core
			lumen_cpu_x64_phase6al_env_absent
			lumen_cpu_x64_phase6al_env_empty
			lumen_cpu_x64_phase6al_env_zero
			lumen_cpu_x64_phase6al_env_malformed
			lumen_cpu_x64_phase6al_env_exact_one
			lumen_cpu_x64_phase6al_geometry_forced_k512
			lumen_cpu_x64_phase6al_report_parser
			lumen_cpu_x64_phase6al_primary_report_parser
			lumen_cpu_x64_phase6al_hidden_worker_invalid
			lumen_cpu_x64_phase6al_primary_after_worker_parser
			lumen_cpu_x64_phase6al_post_compile_boundary
			lumen_cpu_x64_phase6al_archive_zero_rollback
			lumen_phase6al_report_parser_self_test
			lumen_phase6al_live_runner_self_test
			PROPERTIES
				TIMEOUT 120
				ENVIRONMENT
					"LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES=512")
	endif()
	# Keep every pre-existing CPU CTest isolated from a selector inherited from
	# the invoking shell. The compact-tier tests set their requested value in
	# their own `cmake -E env` command after this empty parent environment.
	get_property(LUMEN_CPU_X64_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
	if(LUMEN_CPU_X64_REGISTERED_TESTS)
		set_property(TEST ${LUMEN_CPU_X64_REGISTERED_TESTS} APPEND PROPERTY
			ENVIRONMENT "LUMEN_X64_A64_COMPACT_FASTOP_TIER=")
	endif()
	unset(LUMEN_CPU_X64_REGISTERED_TESTS)
endif()

if(LUMEN_WITH_CPU_A64_DYNAREC_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_cpu_a64_hot_region_test
		CpuA64HotRegionTest.cpp
		emulator/src/CpuA64HotRegion.cpp
		emulator/src/CpuA64Dynarec.cpp)
	target_compile_definitions(lumen_cpu_a64_hot_region_test PRIVATE
		LUMEN_CPU_A64_DYNAREC_TEST)
	target_include_directories(lumen_cpu_a64_hot_region_test PRIVATE
		${PROJECT_SOURCE_DIR}/emulator/src
		${PROJECT_SOURCE_DIR}/emulator/include)
	target_link_libraries(lumen_cpu_a64_hot_region_test PRIVATE Threads::Threads)
	add_test(NAME lumen_cpu_a64_hot_region
		COMMAND $<TARGET_FILE:lumen_cpu_a64_hot_region_test>)
	add_executable(lumen_cpu_a64_dynarec_disabled_test
		CpuA64DynarecDisabledTest.cpp
		emulator/src/CpuA64DynarecDisabled.cpp)
	target_include_directories(lumen_cpu_a64_dynarec_disabled_test PRIVATE
		${PROJECT_SOURCE_DIR}/emulator/src
		${PROJECT_SOURCE_DIR}/emulator/include)
	add_test(NAME lumen_cpu_a64_dynarec_disabled
		COMMAND $<TARGET_FILE:lumen_cpu_a64_dynarec_disabled_test>)
	add_executable(lumen_cpu_a64_dynarec_test CpuA64DynarecTest.cpp emulator/src/CpuA64Dynarec.cpp)
	target_compile_definitions(lumen_cpu_a64_dynarec_test PRIVATE LUMEN_CPU_A64_DYNAREC_TEST)
	if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		target_compile_definitions(lumen_cpu_a64_dynarec_test PRIVATE LUMEN_A64_DYNAREC_HOST_PC_PROFILE=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
		target_compile_definitions(lumen_cpu_a64_dynarec_test PRIVATE LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY=1)
	endif()
	target_include_directories(lumen_cpu_a64_dynarec_test PRIVATE
		${PROJECT_SOURCE_DIR}/emulator/src
		${PROJECT_SOURCE_DIR}/emulator/include
	)
	target_link_libraries(lumen_cpu_a64_dynarec_test PRIVATE Threads::Threads)
	set(LUMEN_MACOS_ARM64_BRANCH_LOCAL_IR_VALIDATION OFF)
	if(MACOS)
		if("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "arm64" OR
		   ("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "" AND
		    CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"))
			set(LUMEN_MACOS_ARM64_BRANCH_LOCAL_IR_VALIDATION ON)
		endif()
	endif()
	if(LUMEN_MACOS_ARM64_BRANCH_LOCAL_IR_VALIDATION)
		set(LUMEN_BRANCH_LOCAL_DEFAULT_EXPECTATION
			--branch-local-ir-validation-enabled-only)
		set(LUMEN_BATCH_DIRECT_RETIREMENT_DEFAULT_EXPECTATION
			--batch-direct-retirement-enabled-only)
		set(LUMEN_DIRECT_LEAF_ABI_DEFAULT_EXPECTATION
			--direct-leaf-abi-enabled-only)
		set(LUMEN_DIRECT_CONTEXT_REUSE_DEFAULT_EXPECTATION
			--direct-context-reuse-enabled-only)
		set(LUMEN_COMPACT_IMMEDIATE_CANDIDATE_EXPECTATION
			--compact-immediates-enabled-only)
		set(LUMEN_PACKED_256_CANDIDATE_EXPECTATION
			--packed-256-enabled-only)
	else()
		set(LUMEN_BRANCH_LOCAL_DEFAULT_EXPECTATION
			--branch-local-ir-validation-disabled-only)
		set(LUMEN_BATCH_DIRECT_RETIREMENT_DEFAULT_EXPECTATION
			--batch-direct-retirement-disabled-only)
		set(LUMEN_DIRECT_LEAF_ABI_DEFAULT_EXPECTATION
			--direct-leaf-abi-disabled-only)
		set(LUMEN_DIRECT_CONTEXT_REUSE_DEFAULT_EXPECTATION
			--direct-context-reuse-disabled-only)
		set(LUMEN_COMPACT_IMMEDIATE_CANDIDATE_EXPECTATION
			--compact-immediates-disabled-only)
		set(LUMEN_PACKED_256_CANDIDATE_EXPECTATION
			--packed-256-disabled-only)
	endif()
	add_test(NAME lumen_cpu_a64_dynarec
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE
			--unset=LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION
			--unset=LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT
			--unset=LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI
			--unset=LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE
			--unset=LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES
			--unset=LUMEN_X64_A64_DYNAREC_PACKED_256
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>)
	if(LUMEN_MACOS_ARM64_BRANCH_LOCAL_IR_VALIDATION)
		add_test(NAME lumen_cpu_a64_dynarec_phase6ce_contextless_dense_entry
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
				LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
				LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE=1
				LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
				LUMEN_X64_A64_DYNAREC_PACKED_256=0
				LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE=0
				LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE=0
				LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT=0
				-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
				--phase6ce-contextless-dense-entry-only)
		set_tests_properties(
			lumen_cpu_a64_dynarec_phase6ce_contextless_dense_entry
			PROPERTIES TIMEOUT 120)
	endif()
	add_test(NAME lumen_cpu_a64_dynarec_phase6by_boundary
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE
			--unset=LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION
			--unset=LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT
			--unset=LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI
			--unset=LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE
			--unset=LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES
			--unset=LUMEN_X64_A64_DYNAREC_PACKED_256
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--phase6by-boundary-proof-only)
	set_tests_properties(lumen_cpu_a64_dynarec_phase6by_boundary
		PROPERTIES TIMEOUT 120)
	add_test(NAME lumen_cpu_a64_dynarec_branch_local_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_BRANCH_LOCAL_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_branch_local_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_BRANCH_LOCAL_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_branch_local_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=0
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--branch-local-ir-validation-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_branch_local_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=1
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_BRANCH_LOCAL_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_branch_local_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=01
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_BRANCH_LOCAL_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_batch_retirement_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_BATCH_DIRECT_RETIREMENT_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_batch_retirement_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--batch-direct-retirement-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_batch_retirement_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=0
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--batch-direct-retirement-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_batch_retirement_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=1
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_BATCH_DIRECT_RETIREMENT_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_batch_retirement_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT=01
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--batch-direct-retirement-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_direct_leaf_abi_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_DIRECT_LEAF_ABI_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_direct_leaf_abi_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--direct-leaf-abi-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_direct_leaf_abi_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=0
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--direct-leaf-abi-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_direct_leaf_abi_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=1
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_DIRECT_LEAF_ABI_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_direct_leaf_abi_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI=01
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--direct-leaf-abi-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_direct_context_reuse_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_DIRECT_CONTEXT_REUSE_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_direct_context_reuse_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--direct-context-reuse-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_direct_context_reuse_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=0
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--direct-context-reuse-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_direct_context_reuse_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=1
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_DIRECT_CONTEXT_REUSE_DEFAULT_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_direct_context_reuse_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE=01
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--direct-context-reuse-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_compact_immediates_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--compact-immediates-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_compact_immediates_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--compact-immediates-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_compact_immediates_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=0
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--compact-immediates-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_compact_immediates_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=1
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_COMPACT_IMMEDIATE_CANDIDATE_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_compact_immediates_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES=01
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--compact-immediates-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_packed_256_absent
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_X64_A64_DYNAREC_PACKED_256
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--packed-256-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_packed_256_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_PACKED_256=
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--packed-256-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_packed_256_exact_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_PACKED_256=0
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--packed-256-disabled-only)
	add_test(NAME lumen_cpu_a64_dynarec_packed_256_exact_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_PACKED_256=1
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			${LUMEN_PACKED_256_CANDIDATE_EXPECTATION})
	add_test(NAME lumen_cpu_a64_dynarec_packed_256_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_X64_A64_DYNAREC_PACKED_256=01
			-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>
			--packed-256-disabled-only)
	if(LUMEN_MACOS_ARM64_BRANCH_LOCAL_IR_VALIDATION)
		add_test(NAME lumen_cpu_a64_dynarec_branch_local_candidate
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=0
				LUMEN_X64_A64_DYNAREC_BRANCH_LOCAL_IR_VALIDATION=1
				-- $<TARGET_FILE:lumen_cpu_a64_dynarec_test>)
	endif()
	set_tests_properties(
		lumen_cpu_a64_dynarec
		lumen_cpu_a64_dynarec_branch_local_absent
		lumen_cpu_a64_dynarec_branch_local_empty
		lumen_cpu_a64_dynarec_branch_local_exact_zero
		lumen_cpu_a64_dynarec_branch_local_exact_one
		lumen_cpu_a64_dynarec_branch_local_malformed
		lumen_cpu_a64_dynarec_batch_retirement_absent
		lumen_cpu_a64_dynarec_batch_retirement_empty
		lumen_cpu_a64_dynarec_batch_retirement_exact_zero
		lumen_cpu_a64_dynarec_batch_retirement_exact_one
		lumen_cpu_a64_dynarec_batch_retirement_malformed
		lumen_cpu_a64_dynarec_direct_leaf_abi_absent
		lumen_cpu_a64_dynarec_direct_leaf_abi_empty
		lumen_cpu_a64_dynarec_direct_leaf_abi_exact_zero
		lumen_cpu_a64_dynarec_direct_leaf_abi_exact_one
		lumen_cpu_a64_dynarec_direct_leaf_abi_malformed
		lumen_cpu_a64_dynarec_direct_context_reuse_absent
		lumen_cpu_a64_dynarec_direct_context_reuse_empty
		lumen_cpu_a64_dynarec_direct_context_reuse_exact_zero
		lumen_cpu_a64_dynarec_direct_context_reuse_exact_one
		lumen_cpu_a64_dynarec_direct_context_reuse_malformed
		lumen_cpu_a64_dynarec_compact_immediates_absent
		lumen_cpu_a64_dynarec_compact_immediates_empty
		lumen_cpu_a64_dynarec_compact_immediates_exact_zero
		lumen_cpu_a64_dynarec_compact_immediates_exact_one
		lumen_cpu_a64_dynarec_compact_immediates_malformed
		lumen_cpu_a64_dynarec_packed_256_absent
		lumen_cpu_a64_dynarec_packed_256_empty
		lumen_cpu_a64_dynarec_packed_256_exact_zero
		lumen_cpu_a64_dynarec_packed_256_exact_one
		lumen_cpu_a64_dynarec_packed_256_malformed
		PROPERTIES TIMEOUT 120)
	if(LUMEN_MACOS_ARM64_BRANCH_LOCAL_IR_VALIDATION)
		set_tests_properties(
			lumen_cpu_a64_dynarec_branch_local_candidate
			PROPERTIES TIMEOUT 120)
	endif()

	if(LUMEN_MACOS_ARM64_BRANCH_LOCAL_IR_VALIDATION)
		# The mechanical timing binary intentionally omits
		# LUMEN_CPU_A64_DYNAREC_TEST: its verification-counter TLS writes must
		# not bias baseline (four scans) against candidate (one scan).
		add_executable(lumen_verified_ir_benchmark EXCLUDE_FROM_ALL
			CpuA64DynarecTest.cpp
			emulator/src/CpuA64Dynarec.cpp
		)
		target_compile_definitions(lumen_verified_ir_benchmark PRIVATE
			LUMEN_VERIFIED_IR_BENCHMARK
		)
		if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
			target_compile_definitions(lumen_verified_ir_benchmark PRIVATE
				LUMEN_A64_DYNAREC_HOST_PC_PROFILE=1)
		endif()
		target_include_directories(lumen_verified_ir_benchmark PRIVATE
			${PROJECT_SOURCE_DIR}/emulator/src
			${PROJECT_SOURCE_DIR}/emulator/include
		)
		target_link_libraries(lumen_verified_ir_benchmark PRIVATE Threads::Threads)

		# Single thin-arm64 Release binary for phase-6AS. Runtime exact 0/1
		# selects the eager or branch-local validator while source, compiler and
		# link inputs stay identical. Unit-test instrumentation is intentionally
		# absent from both the direct-validator and LowerFastOpsVerified timings.
		add_executable(lumen_ir_validator_benchmark EXCLUDE_FROM_ALL
			CpuA64DynarecTest.cpp
			emulator/src/CpuA64Dynarec.cpp
		)
		target_compile_definitions(lumen_ir_validator_benchmark PRIVATE
			LUMEN_IR_VALIDATOR_BENCHMARK
		)
		target_include_directories(lumen_ir_validator_benchmark PRIVATE
			${PROJECT_SOURCE_DIR}/emulator/src
			${PROJECT_SOURCE_DIR}/emulator/include
		)
		target_link_libraries(lumen_ir_validator_benchmark PRIVATE Threads::Threads)
		set_target_properties(lumen_ir_validator_benchmark PROPERTIES
			OSX_ARCHITECTURES "arm64")
	endif()
	unset(LUMEN_BRANCH_LOCAL_DEFAULT_EXPECTATION)
	unset(LUMEN_COMPACT_IMMEDIATE_CANDIDATE_EXPECTATION)
	unset(LUMEN_PACKED_256_CANDIDATE_EXPECTATION)
	unset(LUMEN_MACOS_ARM64_BRANCH_LOCAL_IR_VALIDATION)

	set(LUMEN_MACOS_ARM64_JIT_PAGE_RECYCLE OFF)
	if(MACOS)
		if("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "arm64" OR
		   ("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "" AND
		    CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"))
			set(LUMEN_MACOS_ARM64_JIT_PAGE_RECYCLE ON)
		endif()
	endif()
	if(LUMEN_MACOS_ARM64_JIT_PAGE_RECYCLE)
		# This single Release binary is selected at runtime with exact 0/1 so
		# baseline and candidate share source, compiler and link inputs. It omits
		# unit-test and host-PC instrumentation from the timed lifecycle.
		add_executable(lumen_jit_page_recycle_benchmark EXCLUDE_FROM_ALL
			CpuA64DynarecTest.cpp
			emulator/src/CpuA64Dynarec.cpp
		)
		target_compile_definitions(lumen_jit_page_recycle_benchmark PRIVATE
			LUMEN_JIT_PAGE_RECYCLE_BENCHMARK
		)
		target_include_directories(lumen_jit_page_recycle_benchmark PRIVATE
			${PROJECT_SOURCE_DIR}/emulator/src
			${PROJECT_SOURCE_DIR}/emulator/include
		)
		target_link_libraries(lumen_jit_page_recycle_benchmark PRIVATE
			Threads::Threads)

		if(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
			set(LUMEN_JIT_PAGE_RECYCLE_DEFAULT_EXPECTATION
				--jit-page-recycle-disabled-only)
		else()
			set(LUMEN_JIT_PAGE_RECYCLE_DEFAULT_EXPECTATION
				--jit-page-recycle-enabled-only)
		endif()

		add_test(NAME lumen_cpu_a64_jit_page_recycle_absent
			COMMAND ${CMAKE_COMMAND} -E env
				--unset=LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE --
				$<TARGET_FILE:lumen_cpu_a64_dynarec_test>
				${LUMEN_JIT_PAGE_RECYCLE_DEFAULT_EXPECTATION})
		add_test(NAME lumen_cpu_a64_jit_page_recycle_empty
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE= --
				$<TARGET_FILE:lumen_cpu_a64_dynarec_test>
				${LUMEN_JIT_PAGE_RECYCLE_DEFAULT_EXPECTATION})
		add_test(NAME lumen_cpu_a64_jit_page_recycle_zero
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=0 --
				$<TARGET_FILE:lumen_cpu_a64_dynarec_test>
				--jit-page-recycle-disabled-only)
		add_test(NAME lumen_cpu_a64_jit_page_recycle_malformed
			COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=01 --
				$<TARGET_FILE:lumen_cpu_a64_dynarec_test>
				${LUMEN_JIT_PAGE_RECYCLE_DEFAULT_EXPECTATION})
		if(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
			add_test(NAME lumen_cpu_a64_jit_page_recycle_phase6al_hard_off
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=1 --
					$<TARGET_FILE:lumen_cpu_a64_dynarec_test>
					--jit-page-recycle-disabled-only)
			set(LUMEN_JIT_PAGE_RECYCLE_CTESTS
				lumen_cpu_a64_jit_page_recycle_absent
				lumen_cpu_a64_jit_page_recycle_empty
				lumen_cpu_a64_jit_page_recycle_zero
				lumen_cpu_a64_jit_page_recycle_malformed
				lumen_cpu_a64_jit_page_recycle_phase6al_hard_off)
		else()
			add_test(NAME lumen_cpu_a64_jit_page_recycle_exact_one
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=1 --
					$<TARGET_FILE:lumen_cpu_a64_dynarec_test>
					--jit-page-recycle-enabled-only)
			add_test(NAME lumen_cpu_a64_jit_page_recycle_lifecycle
				COMMAND ${CMAKE_COMMAND} -E env
					LUMEN_X64_A64_DYNAREC_JIT_PAGE_RECYCLE=1 --
					$<TARGET_FILE:lumen_cpu_a64_dynarec_test>
					--jit-page-recycle-only)
			set(LUMEN_JIT_PAGE_RECYCLE_CTESTS
				lumen_cpu_a64_jit_page_recycle_absent
				lumen_cpu_a64_jit_page_recycle_empty
				lumen_cpu_a64_jit_page_recycle_zero
				lumen_cpu_a64_jit_page_recycle_malformed
				lumen_cpu_a64_jit_page_recycle_exact_one
				lumen_cpu_a64_jit_page_recycle_lifecycle)
		endif()
		set_tests_properties(${LUMEN_JIT_PAGE_RECYCLE_CTESTS}
			PROPERTIES TIMEOUT 120)
		unset(LUMEN_JIT_PAGE_RECYCLE_CTESTS)
		unset(LUMEN_JIT_PAGE_RECYCLE_DEFAULT_EXPECTATION)
	endif()
	unset(LUMEN_MACOS_ARM64_JIT_PAGE_RECYCLE)

	if(MACOS)
		foreach(JIT_PUBLICATION_VARIANT IN ITEMS baseline candidate)
			set(JIT_PUBLICATION_TARGET lumen_jit_publication_benchmark_${JIT_PUBLICATION_VARIANT})
			add_executable(${JIT_PUBLICATION_TARGET} EXCLUDE_FROM_ALL
				CpuA64DynarecTest.cpp
				emulator/src/CpuA64Dynarec.cpp
			)
			target_include_directories(${JIT_PUBLICATION_TARGET} PRIVATE
				${PROJECT_SOURCE_DIR}/emulator/src
				${PROJECT_SOURCE_DIR}/emulator/include
			)
			target_compile_definitions(${JIT_PUBLICATION_TARGET} PRIVATE
				LUMEN_JIT_PUBLICATION_BENCHMARK
			)
			target_link_libraries(${JIT_PUBLICATION_TARGET} PRIVATE Threads::Threads)
			if(JIT_PUBLICATION_VARIANT STREQUAL "baseline")
				target_compile_definitions(${JIT_PUBLICATION_TARGET} PRIVATE
					LUMEN_JIT_COPY_LEGACY_REGISTRY_BENCHMARK
				)
			endif()
		endforeach()
	endif()
endif()

if(LUMEN_WITH_AUDIO_OUT_TEST)
	if(NOT SDL_AUDIO OR NOT SDL_DUMMYAUDIO)
		message(FATAL_ERROR "LUMEN_WITH_AUDIO_OUT_TEST requires SDL audio and the SDL dummy audio driver")
	endif()
	add_executable(lumen_audio_out_test EXCLUDE_FROM_ALL
		AudioOutTest.cpp
		emulator/src/AudioOutputBackendSDL.cpp
	)
	target_include_directories(lumen_audio_out_test PRIVATE ${PROJECT_SOURCE_DIR}/emulator/include)
	target_compile_definitions(lumen_audio_out_test PRIVATE LUMEN_AUDIO_OUT_TEST_HOOK=1)
	target_link_libraries(lumen_audio_out_test PRIVATE SDL2-static)
	add_test(NAME lumen_audio_out COMMAND $<TARGET_FILE:lumen_audio_out_test>)
	set_tests_properties(lumen_audio_out PROPERTIES
		ENVIRONMENT "SDL_AUDIODRIVER=dummy"
		TIMEOUT 120
	)
endif()

if(LUMEN_WITH_AVPLAYER_LIFETIME_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_avplayer_lifetime_test EXCLUDE_FROM_ALL
		AvPlayerLifetimeTest.cpp
		emulator/src/Audio.cpp
	)
	target_compile_definitions(lumen_avplayer_lifetime_test PRIVATE LUMEN_AVPLAYER_LIFETIME_TEST_HOOK)
	target_link_libraries(lumen_avplayer_lifetime_test PRIVATE emulator core sys math scripts lua SDL2-static rijndael lzma sqlite zstd
	                                                       easy_profiler cpuinfo Threads::Threads)
	add_test(NAME lumen_avplayer_lifetime COMMAND $<TARGET_FILE:lumen_avplayer_lifetime_test>)
	set_tests_properties(lumen_avplayer_lifetime PROPERTIES TIMEOUT 30)
endif()

if(LUMEN_WITH_GRAPHICS_HOST_POLLING_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_graphics_host_polling_test EXCLUDE_FROM_ALL GraphicsHostPollingTest.cpp)
	target_include_directories(lumen_graphics_host_polling_test PRIVATE ${PROJECT_SOURCE_DIR}/emulator/include)
	target_link_libraries(lumen_graphics_host_polling_test PRIVATE core Threads::Threads)
	foreach(HOST_POLLING_SELECTOR IN ITEMS unset 0 1 01 true)
		add_test(NAME lumen_graphics_host_polling_${HOST_POLLING_SELECTOR}
			COMMAND $<TARGET_FILE:lumen_graphics_host_polling_test> "${HOST_POLLING_SELECTOR}")
		set_tests_properties(lumen_graphics_host_polling_${HOST_POLLING_SELECTOR} PROPERTIES
			TIMEOUT 30 RUN_SERIAL TRUE LABELS "graphics;host-polling"
			PASS_REGULAR_EXPRESSION "GraphicsHostPollingTest: PASS")
	endforeach()
endif()

if(LUMEN_WITH_GRAPHICS_STATE_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_vulkan_dispatch_test EXCLUDE_FROM_ALL VulkanDispatchTest.cpp
		emulator/src/Graphics/VulkanDispatch.cpp)
	target_include_directories(lumen_vulkan_dispatch_test PRIVATE emulator/include 3rdparty/vulkan/include)
	target_compile_definitions(lumen_vulkan_dispatch_test PRIVATE LUMEN_VULKAN_DYNAMIC_DISPATCH=1 VK_NO_PROTOTYPES=1)
	target_compile_features(lumen_vulkan_dispatch_test PRIVATE cxx_std_17)
	add_test(NAME lumen_vulkan_dispatch COMMAND lumen_vulkan_dispatch_test)
	set_tests_properties(lumen_vulkan_dispatch PROPERTIES TIMEOUT 10 LABELS "graphics;vulkan")
	add_executable(lumen_upload_staging_arena_test EXCLUDE_FROM_ALL UploadStagingArenaTest.cpp)
	target_include_directories(lumen_upload_staging_arena_test PRIVATE emulator/include)
	target_compile_features(lumen_upload_staging_arena_test PRIVATE cxx_std_17)
	add_test(NAME lumen_upload_staging_arena COMMAND lumen_upload_staging_arena_test)
	set_tests_properties(lumen_upload_staging_arena PROPERTIES TIMEOUT 10 LABELS "graphics;upload")
	add_executable(lumen_label_event_pool_test EXCLUDE_FROM_ALL LabelEventPoolTest.cpp)
	target_include_directories(lumen_label_event_pool_test PRIVATE emulator/include)
	target_compile_features(lumen_label_event_pool_test PRIVATE cxx_std_17)
	add_test(NAME lumen_label_event_pool COMMAND lumen_label_event_pool_test)
	set_tests_properties(lumen_label_event_pool PROPERTIES TIMEOUT 10 LABELS "graphics;label")
	add_executable(lumen_memory_handoff_physical_index_test EXCLUDE_FROM_ALL MemoryHandoffPhysicalIndexTest.cpp)
	target_include_directories(lumen_memory_handoff_physical_index_test PRIVATE emulator/include)
	target_compile_features(lumen_memory_handoff_physical_index_test PRIVATE cxx_std_17)
	add_test(NAME lumen_memory_handoff_physical_index COMMAND lumen_memory_handoff_physical_index_test)
	add_executable(lumen_native_fence_wait_pins_test EXCLUDE_FROM_ALL NativeFenceWaitPinsTest.cpp)
	add_executable(lumen_draw_access_reservation_test EXCLUDE_FROM_ALL DrawAccessReservationTest.cpp)
	target_include_directories(lumen_draw_access_reservation_test PRIVATE emulator/include)
	target_compile_features(lumen_draw_access_reservation_test PRIVATE cxx_std_17)
	add_test(NAME lumen_draw_access_reservation COMMAND lumen_draw_access_reservation_test)
	set_tests_properties(lumen_draw_access_reservation PROPERTIES TIMEOUT 10 LABELS "graphics;draw;correctness")
	target_include_directories(lumen_native_fence_wait_pins_test PRIVATE emulator/include)
	target_compile_features(lumen_native_fence_wait_pins_test PRIVATE cxx_std_17)
	target_link_libraries(lumen_native_fence_wait_pins_test PRIVATE Threads::Threads)
	add_test(NAME lumen_native_fence_wait_pins COMMAND lumen_native_fence_wait_pins_test)
	add_executable(lumen_presentation_pacer_test EXCLUDE_FROM_ALL PresentationPacerTest.cpp)
	target_include_directories(lumen_presentation_pacer_test PRIVATE emulator/include)
	target_compile_features(lumen_presentation_pacer_test PRIVATE cxx_std_17)
	add_test(NAME lumen_presentation_pacer COMMAND lumen_presentation_pacer_test)
	set_tests_properties(lumen_presentation_pacer PROPERTIES TIMEOUT 10 LABELS "graphics;pacing")
	set_tests_properties(lumen_memory_handoff_physical_index lumen_native_fence_wait_pins PROPERTIES TIMEOUT 30)
	add_executable(lumen_shader_source_snapshot_cache_test EXCLUDE_FROM_ALL ShaderSourceSnapshotCacheTest.cpp)
	target_include_directories(lumen_shader_source_snapshot_cache_test PRIVATE emulator/include)
	target_compile_features(lumen_shader_source_snapshot_cache_test PRIVATE cxx_std_17)
	target_link_libraries(lumen_shader_source_snapshot_cache_test PRIVATE Threads::Threads)
	add_test(NAME lumen_shader_source_snapshot_cache COMMAND lumen_shader_source_snapshot_cache_test)
	set_tests_properties(lumen_shader_source_snapshot_cache PROPERTIES TIMEOUT 10 LABELS "graphics;shader")
	add_executable(lumen_graphics_state_test EXCLUDE_FROM_ALL
		GraphicsCommandStateTest.cpp
		NativeHeapConcurrencyTest.cpp
		CompiledProgramCacheTest.cpp
		CoreRefCounterTest.cpp
		BackingQueryCacheTest.cpp
		NativeMemoryClaimsTest.cpp
		emulator/src/Graphics/Objects/StorageBuffer.cpp
		emulator/src/Graphics/Objects/GpuMemory.cpp
		emulator/src/Graphics/Objects/Label.cpp
		emulator/src/Graphics/Utils.cpp
		emulator/src/Graphics/GraphicsRender.cpp
		emulator/src/Graphics/GraphicsRun.cpp
		emulator/src/Graphics/Shader.cpp
		emulator/src/Graphics/ShaderSpirv.cpp
		emulator/src/Graphics/VideoOut.cpp
		emulator/src/Graphics/Window.cpp
	)
	target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_GRAPHICS_STATE_TEST_HOOK)
	add_test(NAME lumen_backing_query_cache COMMAND $<TARGET_FILE:lumen_graphics_state_test>
		--backing-query-cache-probe)
	add_test(NAME lumen_core_ref_counter COMMAND $<TARGET_FILE:lumen_graphics_state_test>
		--core-ref-counter-probe)
	add_test(NAME lumen_native_memory_claims COMMAND $<TARGET_FILE:lumen_graphics_state_test>
		--native-memory-claims-probe)
	set_tests_properties(lumen_native_memory_claims PROPERTIES TIMEOUT 30 LABELS "frame-work;range-execution;correctness")
	set_tests_properties(lumen_backing_query_cache lumen_core_ref_counter PROPERTIES TIMEOUT 60
		LABELS "frame-work;correctness")
	add_test(NAME lumen_native_heap_concurrency COMMAND $<TARGET_FILE:lumen_graphics_state_test>
		--native-heap-concurrency-probe)
	add_test(NAME lumen_compiled_programs COMMAND $<TARGET_FILE:lumen_graphics_state_test>
		--compiled-programs-probe)
	set_tests_properties(lumen_native_heap_concurrency lumen_compiled_programs PROPERTIES TIMEOUT 60
		LABELS "structural-candidate;correctness")
	foreach(cache 0 1)
		foreach(parallel 0 1)
			add_test(NAME lumen_compiled_programs_${cache}_${parallel} COMMAND ${CMAKE_COMMAND} -E env
				LUMEN_GPU_COMPILED_PROGRAMS=${cache} LUMEN_GPU_PARALLEL_COMPILATION=${parallel}
				$<TARGET_FILE:lumen_graphics_state_test> --compiled-programs-probe)
			set_tests_properties(lumen_compiled_programs_${cache}_${parallel} PROPERTIES TIMEOUT 60
				PASS_REGULAR_EXPRESSION "CompiledProgramsTest: PASS" LABELS "structural-candidate;correctness")
		endforeach()
	endforeach()
	if(NOT ANDROID)
		target_sources(lumen_graphics_state_test PRIVATE GraphicsUploadStagingVulkanProbe.cpp)
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_GRAPHICS_UPLOAD_STAGING_PROBE=1)
		add_test(NAME lumen_graphics_upload_staging_vulkan_probe
			COMMAND $<TARGET_FILE:lumen_graphics_state_test> --upload-staging-probe)
		set_tests_properties(lumen_graphics_upload_staging_vulkan_probe PROPERTIES
			TIMEOUT 30 LABELS "graphics;upload;gpu")
		if(MACOS AND LUMEN_APPLE_NATIVE_METAL)
			set_tests_properties(lumen_graphics_upload_staging_vulkan_probe PROPERTIES ENVIRONMENT "MTL_DEBUG_LAYER=1")
		endif()
	endif()
	if(MACOS AND LUMEN_MACOS_X64_BACKEND STREQUAL "fex")
		# Match the core VM capability so retained-resource tests exercise the
		# real managed mappings instead of compiling out their FEX branch.
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
		target_sources(lumen_graphics_state_test PRIVATE OwnedDrawPreparationFexTest.cpp)
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_OWNED_PREPARATION_FEX_TEST=1)
		add_test(NAME lumen_owned_preparation_fex COMMAND $<TARGET_FILE:lumen_graphics_state_test>
			--owned-preparation-fex-test)
		set_tests_properties(lumen_owned_preparation_fex PROPERTIES TIMEOUT 20
			PASS_REGULAR_EXPRESSION "OwnedDrawPreparationFexTest: PASS" LABELS "graphics;fex;correctness")
		add_test(NAME lumen_backing_query_cache_fex COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_GPU_BACKING_QUERY_CACHE=1 LUMEN_CORE_ATOMIC_REFS=1
			$<TARGET_FILE:lumen_graphics_state_test> --owned-preparation-fex-test)
		set_tests_properties(lumen_backing_query_cache_fex PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "OwnedMutationTokenTest: PASS" LABELS "frame-work;fex;correctness")
		add_test(NAME lumen_range_execution_fex COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_GPU_RANGE_EXECUTION=1 LUMEN_GPU_BACKING_QUERY_CACHE=1 LUMEN_CORE_ATOMIC_REFS=1
			$<TARGET_FILE:lumen_graphics_state_test> --owned-preparation-fex-test)
		set_tests_properties(lumen_range_execution_fex PROPERTIES TIMEOUT 60
			PASS_REGULAR_EXPRESSION "NativeRangeExecutionSuite: PASS" LABELS "frame-work;range-execution;fex;correctness")
		add_test(NAME lumen_owned_draw_inputs COMMAND $<TARGET_FILE:lumen_graphics_state_test>
			--owned-draw-inputs-probe)
		set_tests_properties(lumen_owned_draw_inputs PROPERTIES TIMEOUT 20
			PASS_REGULAR_EXPRESSION "OwnedDrawInputsTest: PASS" LABELS "graphics;shader;correctness")
	endif()
	target_include_directories(lumen_graphics_state_test PRIVATE ${PROJECT_SOURCE_DIR}/3rdparty/xxhash/include)
	if(WIN32)
		target_compile_definitions(lumen_graphics_state_test PRIVATE _CRT_SECURE_NO_WARNINGS)
	endif()
	if(MSVC AND CLANG)
		execute_process(
			COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
			OUTPUT_VARIABLE GRAPHICS_STATE_CLANG_RESOURCE_DIR
			OUTPUT_STRIP_TRAILING_WHITESPACE)
		set(GRAPHICS_STATE_CLANG_BUILTINS_ARCH x86_64)
		if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(ARM64|ARM64EC)$" OR
		   CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
			set(GRAPHICS_STATE_CLANG_BUILTINS_ARCH aarch64)
		endif()
		find_library(GRAPHICS_STATE_CLANG_BUILTINS_LIBRARY
			NAMES "clang_rt.builtins-${GRAPHICS_STATE_CLANG_BUILTINS_ARCH}"
			PATHS "${GRAPHICS_STATE_CLANG_RESOURCE_DIR}/lib/windows"
			NO_DEFAULT_PATH)
		if(NOT GRAPHICS_STATE_CLANG_BUILTINS_LIBRARY)
			message(FATAL_ERROR "clang ${GRAPHICS_STATE_CLANG_BUILTINS_ARCH} builtins were not found for lumen_graphics_state_test")
		endif()
		target_link_libraries(lumen_graphics_state_test PRIVATE
			"${GRAPHICS_STATE_CLANG_BUILTINS_LIBRARY}")
	endif()
	if(MACOS AND LUMEN_APPLE_NATIVE_METAL)
		add_executable(lumen_native_metal_gen4_lifetime_test EXCLUDE_FROM_ALL MetalGen4LifetimeContractTest.mm)
		set_target_properties(lumen_native_metal_gen4_lifetime_test PROPERTIES
			OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
		target_compile_options(lumen_native_metal_gen4_lifetime_test PRIVATE -fobjc-arc)
		target_include_directories(lumen_native_metal_gen4_lifetime_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
		target_link_libraries(lumen_native_metal_gen4_lifetime_test PRIVATE vulkan-1 Threads::Threads)
		add_test(NAME lumen_native_metal_gen4_lifetime COMMAND $<TARGET_FILE:lumen_native_metal_gen4_lifetime_test>)
		set_tests_properties(lumen_native_metal_gen4_lifetime PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "native Metal Gen4.*PASS")
		add_executable(lumen_native_metal_modern_contract_test EXCLUDE_FROM_ALL MetalModernContractTest.mm)
		set_target_properties(lumen_native_metal_modern_contract_test PROPERTIES
			OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
		target_compile_options(lumen_native_metal_modern_contract_test PRIVATE -fobjc-arc)
		target_include_directories(lumen_native_metal_modern_contract_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
		target_link_libraries(lumen_native_metal_modern_contract_test PRIVATE vulkan-1 Threads::Threads)
		add_test(NAME lumen_native_metal_modern_contract COMMAND $<TARGET_FILE:lumen_native_metal_modern_contract_test>)
		set_tests_properties(lumen_native_metal_modern_contract PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "native Metal.*PASS")
		add_executable(lumen_native_metal_sample_readback_test EXCLUDE_FROM_ALL
			MetalSampleReadbackTest.mm
			"${LUMEN_GEN4_DEPS_DIR}/glslang/glslang/ResourceLimits/ResourceLimits.cpp")
		set_target_properties(lumen_native_metal_sample_readback_test PROPERTIES
			OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
		target_compile_options(lumen_native_metal_sample_readback_test PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
		target_include_directories(lumen_native_metal_sample_readback_test PRIVATE
			"${CMAKE_CURRENT_SOURCE_DIR}" "${LUMEN_GEN4_DEPS_DIR}/glslang"
			"${LUMEN_GEN4_FULL_BUILD_DIR}/externals/glslang")
		target_compile_definitions(lumen_native_metal_sample_readback_test PRIVATE
			"LUMEN_SAMPLE_READBACK_SHADER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/gen4/src/video_core/host_shaders/sample_readback.comp\"")
		target_link_libraries(lumen_native_metal_sample_readback_test PRIVATE vulkan-1 Threads::Threads
			"${LUMEN_GEN4_FULL_BUILD_DIR}/externals/glslang/glslang/libglslang.a")
		add_dependencies(lumen_native_metal_sample_readback_test lumen_gen4_runtime)
		add_test(NAME lumen_native_metal_sample_readback COMMAND $<TARGET_FILE:lumen_native_metal_sample_readback_test>)
		set_tests_properties(lumen_native_metal_sample_readback PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "PASS")
		add_executable(lumen_native_metal_tiling_layout_test EXCLUDE_FROM_ALL
			MetalTilingLayoutTest.mm
			"${LUMEN_GEN4_DEPS_DIR}/glslang/glslang/ResourceLimits/ResourceLimits.cpp")
		set_target_properties(lumen_native_metal_tiling_layout_test PROPERTIES
			OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
		target_compile_options(lumen_native_metal_tiling_layout_test PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
		target_include_directories(lumen_native_metal_tiling_layout_test PRIVATE
			"${CMAKE_CURRENT_SOURCE_DIR}" "${LUMEN_GEN4_DEPS_DIR}/glslang"
			"${LUMEN_GEN4_FULL_BUILD_DIR}/externals/glslang")
		target_compile_definitions(lumen_native_metal_tiling_layout_test PRIVATE
			"LUMEN_TILING_SHADER_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/gen4/src/video_core/host_shaders/tiling.comp\"")
		target_link_libraries(lumen_native_metal_tiling_layout_test PRIVATE vulkan-1 Threads::Threads
			"${LUMEN_GEN4_FULL_BUILD_DIR}/externals/glslang/glslang/libglslang.a")
		add_dependencies(lumen_native_metal_tiling_layout_test lumen_gen4_runtime)
		add_test(NAME lumen_native_metal_tiling_layout COMMAND $<TARGET_FILE:lumen_native_metal_tiling_layout_test>)
		set_tests_properties(lumen_native_metal_tiling_layout PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "PASS")
		add_executable(lumen_native_metal_fence_wait_test EXCLUDE_FROM_ALL NativeMetalFenceWaitTest.mm)
		set_target_properties(lumen_native_metal_fence_wait_test PROPERTIES
			OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON OBJCXX_EXTENSIONS OFF)
		target_compile_options(lumen_native_metal_fence_wait_test PRIVATE -fobjc-arc)
		target_link_libraries(lumen_native_metal_fence_wait_test PRIVATE vulkan-1 Threads::Threads)
		add_test(NAME lumen_native_metal_fence_wait COMMAND $<TARGET_FILE:lumen_native_metal_fence_wait_test>)
		set_tests_properties(lumen_native_metal_fence_wait PROPERTIES TIMEOUT 15
			PASS_REGULAR_EXPRESSION "NativeMetalFenceWaitTest: PASS")
		target_sources(lumen_graphics_state_test PRIVATE GraphicsPixelDiscardDepthVulkanProbe.cpp)
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_GRAPHICS_PIXEL_DISCARD_DEPTH_PROBE=1)
		add_test(NAME lumen_graphics_pixel_discard_depth_vulkan_probe_native_metal
			COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1
				$<TARGET_FILE:lumen_graphics_state_test> --pixel-discard-depth-probe)
		add_test(NAME lumen_graphics_stencil_clear_vulkan_probe_native_metal
			COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1
				$<TARGET_FILE:lumen_graphics_state_test> --stencil-clear-probe)
		add_test(NAME lumen_graphics_color_readback_rows_vulkan_probe_native_metal
			COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1
				$<TARGET_FILE:lumen_graphics_state_test> --color-readback-rows-probe)
		foreach(COMPLETION_REUSE_CASE IN ITEMS default zero empty malformed one state_elision)
			set(COMPLETION_REUSE_EXPECTED 0)
			set(COMPLETION_REUSE_STATE_ELISION 0)
			if(COMPLETION_REUSE_CASE STREQUAL "default")
				set(COMPLETION_REUSE_ENV --unset=LUMEN_METAL_COMPLETION_PASS_REUSE)
				set(COMPLETION_REUSE_EXPECTED 1)
			elseif(COMPLETION_REUSE_CASE STREQUAL "zero")
				set(COMPLETION_REUSE_ENV LUMEN_METAL_COMPLETION_PASS_REUSE=0)
			elseif(COMPLETION_REUSE_CASE STREQUAL "empty")
				set(COMPLETION_REUSE_ENV LUMEN_METAL_COMPLETION_PASS_REUSE=)
			elseif(COMPLETION_REUSE_CASE STREQUAL "malformed")
				set(COMPLETION_REUSE_ENV LUMEN_METAL_COMPLETION_PASS_REUSE=01)
			else()
				set(COMPLETION_REUSE_ENV LUMEN_METAL_COMPLETION_PASS_REUSE=1)
				set(COMPLETION_REUSE_EXPECTED 1)
				if(COMPLETION_REUSE_CASE STREQUAL "state_elision")
					set(COMPLETION_REUSE_STATE_ELISION 1)
				endif()
			endif()
			add_test(NAME lumen_graphics_completion_pass_reuse_${COMPLETION_REUSE_CASE}_native_metal
				COMMAND ${CMAKE_COMMAND} -E env MTL_DEBUG_LAYER=1 "${COMPLETION_REUSE_ENV}"
					LUMEN_EXPECT_METAL_COMPLETION_PASS_REUSE=${COMPLETION_REUSE_EXPECTED}
					LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION=${COMPLETION_REUSE_STATE_ELISION}
					LUMEN_METAL_COMPLETION_PASS_TRACE=0
					-- $<TARGET_FILE:lumen_graphics_state_test> --completion-pass-reuse-probe)
			set_tests_properties(lumen_graphics_completion_pass_reuse_${COMPLETION_REUSE_CASE}_native_metal PROPERTIES
				LABELS "graphics;native-metal;render-pass;completion-reuse" RUN_SERIAL TRUE TIMEOUT 60
				PASS_REGULAR_EXPRESSION "\\[GFX_COMPLETION_PASS_REUSE_PROBE\\].*color-depth-stencil=verified.*physical-encoder-counters=verified.*logical-pass-negatives=verified.*status=clean")
		endforeach()
		set_tests_properties(lumen_graphics_color_readback_rows_vulkan_probe_native_metal PROPERTIES
			LABELS "graphics;native-metal;readback;row-padding"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_COLOR_READBACK_ROWS_VULKAN_PROBE\\].*formats=RGBA,BGRA.*frames-per-format=2.*row-copy=runtime-helper.*guards=clean.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)
		set_tests_properties(lumen_graphics_stencil_clear_vulkan_probe_native_metal PROPERTIES
			LABELS "graphics;native-metal;stencil-clear"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_STENCIL_CLEAR_VULKAN_PROBE\\].*clear-mask=runtime-helper.*stencil-and-color=verified.*legacy-overflow=reproduced.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)
		set_tests_properties(lumen_graphics_pixel_discard_depth_vulkan_probe_native_metal PROPERTIES
			LABELS "graphics;native-metal;pixel-export;discard;depth-stencil"
			PASS_REGULAR_EXPRESSION
				"\\[GFX_PIXEL_DISCARD_DEPTH_VULKAN_PROBE\\].*fragment=runtime-gen4.*orders=2.*discard-color-depth=verified.*forced-early-corruption=reproduced.*status=clean"
			RUN_SERIAL TRUE
			TIMEOUT 60)
	endif()
	if(LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT)
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT=1)
	else()
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT=0)
	endif()
	if(LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT)
		target_compile_definitions(lumen_graphics_state_test PRIVATE
			LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT=1)
	else()
		target_compile_definitions(lumen_graphics_state_test PRIVATE
			LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT=0)
	endif()
	if(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_A64_DYNAREC_STARTUP_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		target_compile_definitions(lumen_graphics_state_test PRIVATE
			LUMEN_A64_DYNAREC_HOST_PC_PROFILE=1
			LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST=1)
	endif()
	target_link_libraries(lumen_graphics_state_test PRIVATE emulator core sys math scripts lua SDL2-static rijndael lzma sqlite zstd
	                                                    easy_profiler cpuinfo Threads::Threads)
	if(MACOS AND NOT LUMEN_APPLE_NATIVE_METAL)
		# Window.cpp intentionally weak-imports the native-Metal finalizers. A
		# MoltenVK-only test binary has no defining object, so authorize those
		# test-only unresolved weak references explicitly for ld64.
		target_link_options(lumen_graphics_state_test PRIVATE
			"-Wl,-U,_LumenMetalFinalizeShaderFunctionCache"
			"-Wl,-U,_LumenMetalFinalizeDepthStencilStateCache"
			"-Wl,-U,_LumenMetalFinalizeStateOnlyRecordElision"
			"-Wl,-U,_LumenMetalFinalizeCompletionPassReuse"
		)
	endif()
	add_test(NAME lumen_graphics_state COMMAND $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_command_access_reducer
		COMMAND ${CMAKE_COMMAND} -E env LUMEN_FRAME_PIPELINE_TRACE=1
			$<TARGET_FILE:lumen_graphics_state_test> --gpu-command-access-reducer-probe)
	add_test(NAME lumen_graphics_draw_access_plan
		COMMAND ${CMAKE_COMMAND} -E env LUMEN_FRAME_PIPELINE_TRACE=1
			$<TARGET_FILE:lumen_graphics_state_test> --gpu-draw-access-plan-probe)
	set_tests_properties(lumen_graphics_draw_access_plan PROPERTIES TIMEOUT 30 LABELS "graphics;draw;correctness"
		PASS_REGULAR_EXPRESSION "\\[GFX_DRAW_ACCESS_PLAN_TEST\\] status=PASS" FAIL_REGULAR_EXPRESSION "status=FAIL")
	add_test(NAME lumen_graphics_draw_access_plan_untracked
		COMMAND ${CMAKE_COMMAND} -E env LUMEN_FRAME_PIPELINE_TRACE=0
			$<TARGET_FILE:lumen_graphics_state_test> --gpu-draw-access-plan-untracked-probe)
	set_tests_properties(lumen_graphics_draw_access_plan_untracked PROPERTIES TIMEOUT 30 LABELS "graphics;draw;correctness"
		PASS_REGULAR_EXPRESSION "\\[GFX_DRAW_ACCESS_PLAN_TEST\\] status=PASS" FAIL_REGULAR_EXPRESSION "status=FAIL")
	set_tests_properties(lumen_graphics_command_access_reducer PROPERTIES
		TIMEOUT 30 LABELS "graphics;access-ledger;correctness"
		PASS_REGULAR_EXPRESSION "\\[GFX_COMMAND_ACCESS_REDUCER_TEST\\] status=PASS"
		FAIL_REGULAR_EXPRESSION "status=FAIL")
	foreach(ready_counter_wait 0 1)
		add_test(NAME lumen_graphics_ready_counter_wait_${ready_counter_wait}
			COMMAND ${CMAKE_COMMAND} -E env LUMEN_GPU_READY_COUNTER_WAIT=${ready_counter_wait}
				$<TARGET_FILE:lumen_graphics_state_test>)
		set_tests_properties(lumen_graphics_ready_counter_wait_${ready_counter_wait} PROPERTIES
			TIMEOUT 30 LABELS "graphics;counter-wait;handoff"
			ENVIRONMENT "LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000=;LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_DB_ONLY=;LUMEN_EXPERIMENTAL_GFX_CB_META_EVENT="
			PASS_REGULAR_EXPRESSION "\\[GFX_READY_COUNTER_WAIT_TEST\\] selector=${ready_counter_wait}"
			FAIL_REGULAR_EXPRESSION "test failed")
	endforeach()
	foreach(ready_memory_wait 0 1)
		add_test(NAME lumen_graphics_ready_memory_wait_${ready_memory_wait}
			COMMAND ${CMAKE_COMMAND} -E env LUMEN_GPU_READY_MEMORY_WAIT=${ready_memory_wait}
				$<TARGET_FILE:lumen_graphics_state_test>)
		set_tests_properties(lumen_graphics_ready_memory_wait_${ready_memory_wait} PROPERTIES
			TIMEOUT 30 LABELS "graphics;memory-wait;handoff"
			ENVIRONMENT "LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000=;LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_DB_ONLY=;LUMEN_EXPERIMENTAL_GFX_CB_META_EVENT="
			PASS_REGULAR_EXPRESSION "\\[GFX_READY_MEMORY_WAIT_TEST\\] selector=${ready_memory_wait}"
			FAIL_REGULAR_EXPRESSION "test failed")
	endforeach()
	foreach(SPLIT_READ_GUARDS_CASE IN ITEMS default zero empty malformed trailing_space one)
		set(SPLIT_READ_GUARDS_EXPECTED 0)
		if(SPLIT_READ_GUARDS_CASE STREQUAL "default")
			set(SPLIT_READ_GUARDS_ENV --unset=LUMEN_GPU_SPLIT_READ_GUARDS)
		elseif(SPLIT_READ_GUARDS_CASE STREQUAL "zero")
			set(SPLIT_READ_GUARDS_ENV LUMEN_GPU_SPLIT_READ_GUARDS=0)
		elseif(SPLIT_READ_GUARDS_CASE STREQUAL "empty")
			set(SPLIT_READ_GUARDS_ENV LUMEN_GPU_SPLIT_READ_GUARDS=)
		elseif(SPLIT_READ_GUARDS_CASE STREQUAL "malformed")
			set(SPLIT_READ_GUARDS_ENV LUMEN_GPU_SPLIT_READ_GUARDS=01)
		elseif(SPLIT_READ_GUARDS_CASE STREQUAL "trailing_space")
			set(SPLIT_READ_GUARDS_ENV "LUMEN_GPU_SPLIT_READ_GUARDS=1 ")
		else()
			set(SPLIT_READ_GUARDS_ENV LUMEN_GPU_SPLIT_READ_GUARDS=1)
			set(SPLIT_READ_GUARDS_EXPECTED 1)
		endif()
		add_test(NAME lumen_graphics_split_read_guards_${SPLIT_READ_GUARDS_CASE}
			COMMAND ${CMAKE_COMMAND} -E env "${SPLIT_READ_GUARDS_ENV}"
				LUMEN_EXPECT_GPU_SPLIT_READ_GUARDS=${SPLIT_READ_GUARDS_EXPECTED}
				$<TARGET_FILE:lumen_graphics_state_test>)
		set_tests_properties(lumen_graphics_split_read_guards_${SPLIT_READ_GUARDS_CASE} PROPERTIES
			TIMEOUT 30 LABELS "graphics;guest-memory;handoff"
			PASS_REGULAR_EXPRESSION "\\[GPU_MEMORY_HANDOFF_SPLIT_READ_GUARDS_TEST\\] selected=${SPLIT_READ_GUARDS_EXPECTED}"
			FAIL_REGULAR_EXPRESSION "test failed")
	endforeach()
	if(MACOS)
		foreach(label_snapshot 0 1)
			add_test(NAME lumen_graphics_metadata_writer_snapshot_${label_snapshot}
				COMMAND ${CMAKE_COMMAND} -E env LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${label_snapshot}
					LUMEN_FRAME_PIPELINE_TRACE=1 $<TARGET_FILE:lumen_graphics_state_test>)
			set_tests_properties(lumen_graphics_metadata_writer_snapshot_${label_snapshot} PROPERTIES
				TIMEOUT 30 LABELS "graphics;guest-data;metadata-writer"
				ENVIRONMENT "LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000=;LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_DB_ONLY=;LUMEN_EXPERIMENTAL_GFX_CB_META_EVENT="
				PASS_REGULAR_EXPRESSION "\\[GFX_UNIFORM_DEPTH_ZERO_METADATA_WRITER_COMMAND_TEST\\].*negative_cases=12"
				FAIL_REGULAR_EXPRESSION "test failed")
			add_test(NAME lumen_graphics_label_destination_snapshot_${label_snapshot}
				COMMAND ${CMAKE_COMMAND} -E env LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${label_snapshot}
					$<TARGET_FILE:lumen_graphics_state_test> --label-destination-probe)
			set_tests_properties(lumen_graphics_label_destination_snapshot_${label_snapshot} PROPERTIES
				TIMEOUT 30 LABELS "graphics;guest-data;deferred-publication")
			add_test(NAME lumen_graphics_gds_destination_snapshot_${label_snapshot}
				COMMAND ${CMAKE_COMMAND} -E env LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${label_snapshot}
					$<TARGET_FILE:lumen_graphics_state_test> --gds-destination-probe)
			set_tests_properties(lumen_graphics_gds_destination_snapshot_${label_snapshot} PROPERTIES
				TIMEOUT 30 LABELS "graphics;guest-data;dma")
			add_test(NAME lumen_graphics_eop_virtual_clock_snapshot_${label_snapshot}
				COMMAND ${CMAKE_COMMAND} -E env LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${label_snapshot}
					$<TARGET_FILE:lumen_graphics_state_test> --eop-virtual-clock-probe)
			set_tests_properties(lumen_graphics_eop_virtual_clock_snapshot_${label_snapshot} PROPERTIES
				TIMEOUT 30 LABELS "graphics;guest-data;eop")
		endforeach()
	endif()

	set_tests_properties(lumen_graphics_state PROPERTIES
		TIMEOUT 30
		ENVIRONMENT "LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000=;LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_DB_ONLY=;LUMEN_EXPERIMENTAL_GFX_CB_META_EVENT=")
	add_test(NAME lumen_graphics_state_acquire_mem_08c40000_candidate
		COMMAND ${CMAKE_COMMAND} -E env LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000=1
			$<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(lumen_graphics_state_acquire_mem_08c40000_candidate PROPERTIES TIMEOUT 30)
	add_test(NAME lumen_graphics_state_acquire_mem_db_only_candidate
		COMMAND ${CMAKE_COMMAND} -E env LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_DB_ONLY=1
			LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000=1 $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(lumen_graphics_state_acquire_mem_db_only_candidate PROPERTIES TIMEOUT 30)
	add_test(NAME lumen_graphics_state_cb_meta_event_candidate
		COMMAND ${CMAKE_COMMAND} -E env LUMEN_EXPERIMENTAL_GFX_CB_META_EVENT=1
			LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_DB_ONLY=1 LUMEN_EXPERIMENTAL_GFX_ACQUIRE_MEM_08C40000=1
			$<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(lumen_graphics_state_cb_meta_event_candidate PROPERTIES TIMEOUT 30)
	add_test(NAME lumen_graphics_state_hle_d32_default_candidate
		COMMAND ${CMAKE_COMMAND} -E env LUMEN_EXPERIMENTAL_GFX_HLE_D32_DEFAULT=1
			$<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(lumen_graphics_state_hle_d32_default_candidate PROPERTIES TIMEOUT 30)
	if(LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT)
		set(LUMEN_EXPECT_GFX_NULL_SAMPLED_IMAGE_DEFAULT 1)
	else()
		set(LUMEN_EXPECT_GFX_NULL_SAMPLED_IMAGE_DEFAULT 0)
	endif()
	add_test(NAME lumen_graphics_state_null_sampled_image_production_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_GFX_NULL_SAMPLED_IMAGE
			LUMEN_EXPECT_GFX_NULL_SAMPLED_IMAGE=${LUMEN_EXPECT_GFX_NULL_SAMPLED_IMAGE_DEFAULT}
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_null_sampled_image_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_GFX_NULL_SAMPLED_IMAGE=0
			LUMEN_EXPECT_GFX_NULL_SAMPLED_IMAGE=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_null_sampled_image_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_GFX_NULL_SAMPLED_IMAGE=01
			LUMEN_EXPECT_GFX_NULL_SAMPLED_IMAGE=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_null_sampled_image_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_GFX_NULL_SAMPLED_IMAGE=1
			LUMEN_EXPECT_GFX_NULL_SAMPLED_IMAGE=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_null_sampled_image_production_default
		lumen_graphics_state_null_sampled_image_zero
		lumen_graphics_state_null_sampled_image_malformed
		lumen_graphics_state_null_sampled_image_candidate
		PROPERTIES TIMEOUT 30)
	add_test(NAME lumen_graphics_state_dx10_clamp_zero_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO
			LUMEN_EXPECT_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dx10_clamp_zero_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=0
			LUMEN_EXPECT_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dx10_clamp_zero_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=
			LUMEN_EXPECT_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dx10_clamp_zero_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=01
			LUMEN_EXPECT_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dx10_clamp_zero_word
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=true
			LUMEN_EXPECT_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dx10_clamp_zero_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			"LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=1 "
			LUMEN_EXPECT_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dx10_clamp_zero_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=1
			LUMEN_EXPECT_EXPERIMENTAL_GFX_DX10_CLAMP_ZERO=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_dx10_clamp_zero_default
		lumen_graphics_state_dx10_clamp_zero_zero
		lumen_graphics_state_dx10_clamp_zero_empty
		lumen_graphics_state_dx10_clamp_zero_malformed
		lumen_graphics_state_dx10_clamp_zero_word
		lumen_graphics_state_dx10_clamp_zero_trailing_space
		lumen_graphics_state_dx10_clamp_zero_candidate
		PROPERTIES TIMEOUT 30)
	add_test(NAME lumen_graphics_state_implicit_sample_lod_zero_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO
			LUMEN_EXPECT_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_implicit_sample_lod_zero_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=0
			LUMEN_EXPECT_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_implicit_sample_lod_zero_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=
			LUMEN_EXPECT_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_implicit_sample_lod_zero_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=01
			LUMEN_EXPECT_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_implicit_sample_lod_zero_word
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=true
			LUMEN_EXPECT_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_implicit_sample_lod_zero_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			"LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=1 "
			LUMEN_EXPECT_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_implicit_sample_lod_zero_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=1
			LUMEN_EXPECT_EXPERIMENTAL_GFX_IMPLICIT_SAMPLE_LOD_ZERO=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_implicit_sample_lod_zero_default
		lumen_graphics_state_implicit_sample_lod_zero_zero
		lumen_graphics_state_implicit_sample_lod_zero_empty
		lumen_graphics_state_implicit_sample_lod_zero_malformed
		lumen_graphics_state_implicit_sample_lod_zero_word
		lumen_graphics_state_implicit_sample_lod_zero_trailing_space
		lumen_graphics_state_implicit_sample_lod_zero_candidate
		PROPERTIES TIMEOUT 30)
	set(LUMEN_V_MAC_LEGACY_F32_CANDIDATE_EXPECTATION 0)
	set(LUMEN_V_MAC_LEGACY_F32_CANDIDATE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_V_MAC_LEGACY_F32_CANDIDATE_ARCH)
		set(LUMEN_V_MAC_LEGACY_F32_CANDIDATE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if((MACOS AND LUMEN_APPLE_NATIVE_METAL AND
	    LUMEN_V_MAC_LEGACY_F32_CANDIDATE_ARCH MATCHES "^(arm64|aarch64)$") OR
	   (WIN32 AND LUMEN_BITNESS EQUAL 64 AND
	    LUMEN_V_MAC_LEGACY_F32_CANDIDATE_ARCH MATCHES "^(AMD64|amd64|x86_64|X86_64)$"))
		# Keep every unqualified configuration fail-closed even if it defines
		# NDEBUG; only exact Release receives the compile-time gate.
		set(LUMEN_V_MAC_LEGACY_F32_CANDIDATE_EXPECTATION
			"$<IF:$<CONFIG:Release>,1,0>")
	endif()
	add_test(NAME lumen_graphics_state_v_mac_legacy_f32_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_mac_legacy_f32_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=0
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_mac_legacy_f32_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_mac_legacy_f32_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=01
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_mac_legacy_f32_word
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=true
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_mac_legacy_f32_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			"LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=1 "
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_mac_legacy_f32_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=1
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_MAC_LEGACY_F32=${LUMEN_V_MAC_LEGACY_F32_CANDIDATE_EXPECTATION}
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_v_mac_legacy_f32_default
		lumen_graphics_state_v_mac_legacy_f32_zero
		lumen_graphics_state_v_mac_legacy_f32_empty
		lumen_graphics_state_v_mac_legacy_f32_malformed
		lumen_graphics_state_v_mac_legacy_f32_word
		lumen_graphics_state_v_mac_legacy_f32_trailing_space
		lumen_graphics_state_v_mac_legacy_f32_candidate
		PROPERTIES TIMEOUT 30)
	unset(LUMEN_V_MAC_LEGACY_F32_CANDIDATE_EXPECTATION)
	unset(LUMEN_V_MAC_LEGACY_F32_CANDIDATE_ARCH)
	set(LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_EXPECTATION 0)
	set(LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_ARCH "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_ARCH)
		set(LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	endif()
	if((MACOS AND LUMEN_APPLE_NATIVE_METAL AND
	    LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_ARCH MATCHES "^(arm64|aarch64)$") OR
	   (WIN32 AND LUMEN_BITNESS EQUAL 64 AND
	    LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_ARCH MATCHES "^(AMD64|amd64|x86_64|X86_64)$"))
		set(LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_EXPECTATION
			"$<IF:$<CONFIG:Release>,1,0>")
	endif()
	add_test(NAME lumen_graphics_state_v_rsq_clamp_f32_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_rsq_clamp_f32_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=0
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_rsq_clamp_f32_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_rsq_clamp_f32_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=01
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_rsq_clamp_f32_word
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=true
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_rsq_clamp_f32_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			"LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=1 "
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_v_rsq_clamp_f32_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=1
			LUMEN_EXPECT_EXPERIMENTAL_GFX_V_RSQ_CLAMP_F32=${LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_EXPECTATION}
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_v_rsq_clamp_f32_default
		lumen_graphics_state_v_rsq_clamp_f32_zero
		lumen_graphics_state_v_rsq_clamp_f32_empty
		lumen_graphics_state_v_rsq_clamp_f32_malformed
		lumen_graphics_state_v_rsq_clamp_f32_word
		lumen_graphics_state_v_rsq_clamp_f32_trailing_space
		lumen_graphics_state_v_rsq_clamp_f32_candidate
		PROPERTIES LABELS "graphics;selector;v-rsq-clamp-f32" TIMEOUT 30)
	unset(LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_EXPECTATION)
	unset(LUMEN_V_RSQ_CLAMP_F32_CANDIDATE_ARCH)
	add_test(NAME lumen_graphics_state_texture_mip_assembly_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			--unset=LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_texture_mip_assembly_zero
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=0
			LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_texture_mip_assembly_empty
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=
			LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_texture_mip_assembly_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=01
			LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_texture_mip_assembly_word
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=true
			LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_texture_mip_assembly_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			"LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=1 "
			LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_texture_mip_assembly_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=1
			LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_texture_mip_assembly_default
		lumen_graphics_state_texture_mip_assembly_zero
		lumen_graphics_state_texture_mip_assembly_empty
		lumen_graphics_state_texture_mip_assembly_malformed
		lumen_graphics_state_texture_mip_assembly_word
		lumen_graphics_state_texture_mip_assembly_trailing_space
		lumen_graphics_state_texture_mip_assembly_candidate
		PROPERTIES LABELS "graphics;selector;texture-mip-assembly" TIMEOUT 30)
	add_test(NAME lumen_graphics_state_render_texture_snapshot_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			--unset=LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_render_texture_snapshot_zero
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			"LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=0"
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_render_texture_snapshot_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			"LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=01"
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_render_texture_snapshot_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			"LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=1 "
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_render_texture_snapshot_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			--unset=LUMEN_EXPECT_EXPERIMENTAL_GFX_TEXTURE_MIP_ASSEMBLY
			"LUMEN_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=1"
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RENDER_TEXTURE_SNAPSHOT=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_render_texture_snapshot_default
		lumen_graphics_state_render_texture_snapshot_zero
		lumen_graphics_state_render_texture_snapshot_malformed
		lumen_graphics_state_render_texture_snapshot_trailing_space
		lumen_graphics_state_render_texture_snapshot_candidate
		PROPERTIES LABELS "graphics;selector;render-texture-snapshot" TIMEOUT 30)
	add_test(NAME lumen_graphics_state_rect_list_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_EXPERIMENTAL_GFX_RECT_LIST
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RECT_LIST=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_rect_list_zero
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_EXPERIMENTAL_GFX_RECT_LIST=0
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RECT_LIST=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_rect_list_empty
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_EXPERIMENTAL_GFX_RECT_LIST=
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RECT_LIST=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_rect_list_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_EXPERIMENTAL_GFX_RECT_LIST=01
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RECT_LIST=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_rect_list_word
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_EXPERIMENTAL_GFX_RECT_LIST=true
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RECT_LIST=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_rect_list_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			"LUMEN_EXPERIMENTAL_GFX_RECT_LIST=1 "
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RECT_LIST=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_rect_list_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_EXPERIMENTAL_GFX_RECT_LIST=1
			LUMEN_EXPECT_EXPERIMENTAL_GFX_RECT_LIST=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_rect_list_default
		lumen_graphics_state_rect_list_zero
		lumen_graphics_state_rect_list_empty
		lumen_graphics_state_rect_list_malformed
		lumen_graphics_state_rect_list_word
		lumen_graphics_state_rect_list_trailing_space
		lumen_graphics_state_rect_list_candidate
		PROPERTIES LABELS "graphics;selector;rect-list" TIMEOUT 30)
	add_test(NAME lumen_graphics_state_gpu_r8_capture_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_R8=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_r8_capture_zero
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8=0
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_R8=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_r8_capture_empty
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8=
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_R8=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_r8_capture_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8=01
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_R8=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_r8_capture_word
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8=true
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_R8=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_r8_capture_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			"LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8=1 "
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_R8=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_r8_capture_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_R8=1
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_R8=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_gpu_r8_capture_default
		lumen_graphics_state_gpu_r8_capture_zero
		lumen_graphics_state_gpu_r8_capture_empty
		lumen_graphics_state_gpu_r8_capture_malformed
		lumen_graphics_state_gpu_r8_capture_word
		lumen_graphics_state_gpu_r8_capture_trailing_space
		lumen_graphics_state_gpu_r8_capture_candidate
		PROPERTIES LABELS "graphics;selector;gpu-r8-capture" TIMEOUT 30)
	add_test(NAME lumen_graphics_state_gpu_rgba8_mips_capture_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			--unset=LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_rgba8_mips_capture_zero
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=0
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_rgba8_mips_capture_empty
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_rgba8_mips_capture_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=01
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_rgba8_mips_capture_word
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=true
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_rgba8_mips_capture_trailing_space
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			"LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=1 "
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_gpu_rgba8_mips_capture_candidate
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_FRAME_PIPELINE_TRACE
			LUMEN_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=1
			LUMEN_EXPECT_GFX_RESOURCE_CAPTURE_GPU_RGBA8_MIPS=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_gpu_rgba8_mips_capture_default
		lumen_graphics_state_gpu_rgba8_mips_capture_zero
		lumen_graphics_state_gpu_rgba8_mips_capture_empty
		lumen_graphics_state_gpu_rgba8_mips_capture_malformed
		lumen_graphics_state_gpu_rgba8_mips_capture_word
		lumen_graphics_state_gpu_rgba8_mips_capture_trailing_space
		lumen_graphics_state_gpu_rgba8_mips_capture_candidate
		PROPERTIES LABELS "graphics;selector;gpu-rgba8-mips-capture" TIMEOUT 30)
	if(LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT)
		set(LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT 1)
	else()
		set(LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT 0)
	endif()
	add_test(NAME lumen_graphics_state_dynamic_vertex_host_visible_default
		COMMAND ${CMAKE_COMMAND} -E env
			--unset=LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE
			LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=${LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT}
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dynamic_vertex_host_visible_zero
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=0
			LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dynamic_vertex_host_visible_empty
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=
			LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dynamic_vertex_host_visible_malformed
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=01
			LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=0
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	add_test(NAME lumen_graphics_state_dynamic_vertex_host_visible_one
		COMMAND ${CMAKE_COMMAND} -E env
			LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=1
			LUMEN_EXPECT_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE=1
			-- $<TARGET_FILE:lumen_graphics_state_test>)
	set_tests_properties(
		lumen_graphics_state_dynamic_vertex_host_visible_default
		lumen_graphics_state_dynamic_vertex_host_visible_zero
		lumen_graphics_state_dynamic_vertex_host_visible_empty
		lumen_graphics_state_dynamic_vertex_host_visible_malformed
		lumen_graphics_state_dynamic_vertex_host_visible_one
		PROPERTIES TIMEOUT 30)
	if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		add_test(NAME lumen_graphics_state_host_pc_rejected
		         COMMAND $<TARGET_FILE:lumen_graphics_state_test>)
		set_tests_properties(lumen_graphics_state_host_pc_rejected PROPERTIES
			TIMEOUT 30
			ENVIRONMENT "LUMEN_X64_A64_DYNAREC_HOST_PC_PROFILE=1;LUMEN_A64_DYNAREC_HOST_PC_PROFILE_ORACLE=rejected")
		add_test(NAME lumen_graphics_state_host_pc_accepted
		         COMMAND $<TARGET_FILE:lumen_graphics_state_test>)
		set_tests_properties(lumen_graphics_state_host_pc_accepted PROPERTIES
			TIMEOUT 30
			ENVIRONMENT "LUMEN_X64_A64_DYNAREC_HOST_PC_PROFILE=1;LUMEN_A64_DYNAREC_HOST_PC_PROFILE_ORACLE=accepted")
	endif()
endif()

if(LUMEN_WITH_RENDER_TARGET_FORMAT_TEST AND NOT IOS)
	add_executable(lumen_render_target_format_test RenderTargetFormatTest.cpp)
	target_include_directories(lumen_render_target_format_test PRIVATE
		${PROJECT_SOURCE_DIR}/emulator/include
		${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include
	)
	if(MACOS)
		target_compile_definitions(lumen_render_target_format_test PRIVATE LUMEN_RENDER_TARGET_VULKAN_PROBE=1)
		target_link_libraries(lumen_render_target_format_test PRIVATE vulkan-1)
	endif()
	add_test(NAME lumen_render_target_format COMMAND $<TARGET_FILE:lumen_render_target_format_test>)
	set_tests_properties(lumen_render_target_format PROPERTIES TIMEOUT 30)
endif()

if(LUMEN_WITH_SIMPLE_ARRAY_SORT_TEST)
	add_executable(lumen_simple_array_sort_test EXCLUDE_FROM_ALL SimpleArraySortTest.cpp)
	target_link_libraries(lumen_simple_array_sort_test PRIVATE core)
	add_test(NAME lumen_simple_array_sort COMMAND $<TARGET_FILE:lumen_simple_array_sort_test>)
	set_tests_properties(lumen_simple_array_sort PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "SimpleArraySortTest: PASS")
endif()

if(LUMEN_WITH_TILE_TEST)
	add_executable(lumen_tile_size_test EXCLUDE_FROM_ALL TileSizeTest.cpp)
	target_link_libraries(lumen_tile_size_test PRIVATE emulator)
	add_test(NAME lumen_tile_size COMMAND $<TARGET_FILE:lumen_tile_size_test>)
	set_tests_properties(lumen_tile_size PROPERTIES TIMEOUT 30)
endif()

if(LUMEN_WITH_GUEST_FILE_TEST)
	add_executable(lumen_core_file_exclusive_create_test EXCLUDE_FROM_ALL CoreFileExclusiveCreateTest.cpp)
	target_link_libraries(lumen_core_file_exclusive_create_test PRIVATE core)
	add_test(NAME lumen_core_file_exclusive_create COMMAND $<TARGET_FILE:lumen_core_file_exclusive_create_test>)
	set_tests_properties(lumen_core_file_exclusive_create PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "CoreFileExclusiveCreateTest: PASS"
		FAIL_REGULAR_EXPRESSION "CoreFileExclusiveCreateTest: FAIL|test failed|Fatal Error")
	add_executable(lumen_guest_file_test EXCLUDE_FROM_ALL GuestFileTest.cpp emulator/src/Kernel/GuestFile.cpp)
	target_include_directories(lumen_guest_file_test PRIVATE ${PROJECT_SOURCE_DIR}/emulator/include)
	target_link_libraries(lumen_guest_file_test PRIVATE core)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_guest_file_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	endif()
	add_test(NAME lumen_guest_file COMMAND $<TARGET_FILE:lumen_guest_file_test>)
	set_tests_properties(lumen_guest_file PROPERTIES TIMEOUT 120)
endif()

if(LUMEN_WITH_FILESYSTEM_PATH_TEST)
	add_executable(lumen_filesystem_path_test EXCLUDE_FROM_ALL
		FileSystemPathTest.cpp
		emulator/src/Kernel/FileSystemPath.cpp
	)
	target_include_directories(lumen_filesystem_path_test PRIVATE ${PROJECT_SOURCE_DIR}/emulator/src)
	target_link_libraries(lumen_filesystem_path_test PRIVATE core)
	add_test(NAME lumen_filesystem_path COMMAND $<TARGET_FILE:lumen_filesystem_path_test>)
	set_tests_properties(lumen_filesystem_path PROPERTIES TIMEOUT 120)
endif()

if(LUMEN_WITH_AJM_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_ajm_test EXCLUDE_FROM_ALL
		AjmTest.cpp
		emulator/src/Libs/AjmCore.cpp
	)
	target_include_directories(lumen_ajm_test PRIVATE ${PROJECT_SOURCE_DIR}/emulator/src)
	target_link_libraries(lumen_ajm_test PRIVATE Lumen::LibAtrac9 Threads::Threads)
	add_test(NAME lumen_ajm COMMAND $<TARGET_FILE:lumen_ajm_test>)
	set_tests_properties(lumen_ajm PROPERTIES TIMEOUT 120)
endif()

if(LUMEN_WITH_RUNTIME_LINKER_ADDRESS_TEST)
	add_executable(lumen_prelinked_plt_test EXCLUDE_FROM_ALL PrelinkedPltTest.cpp)
	target_include_directories(lumen_prelinked_plt_test PRIVATE ${PROJECT_SOURCE_DIR}/emulator/include)
	add_test(NAME lumen_prelinked_plt COMMAND $<TARGET_FILE:lumen_prelinked_plt_test>)
	set_tests_properties(lumen_prelinked_plt PROPERTIES TIMEOUT 30)
endif()

if(MACOS AND LUMEN_WITH_RUNTIME_LINKER_ADDRESS_TEST)
	find_package(Threads REQUIRED)
	add_executable(lumen_runtime_linker_address_test EXCLUDE_FROM_ALL
		RuntimeLinkerAddressTest.cpp
		emulator/src/CpuX64.cpp
		emulator/src/CpuA64Dynarec.cpp
		emulator/src/Loader/RuntimeLinker.cpp
	)
	target_compile_definitions(lumen_runtime_linker_address_test PRIVATE LUMEN_CPU_X64_TEST
	                                                                      LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK)
	if(LUMEN_FAST_BLOCK_TELEMETRY)
		target_compile_definitions(lumen_runtime_linker_address_test PRIVATE LUMEN_FAST_BLOCK_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
		target_compile_definitions(lumen_runtime_linker_address_test PRIVATE LUMEN_A64_DYNAREC_STARTUP_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
		target_compile_definitions(lumen_runtime_linker_address_test PRIVATE LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY=1)
	endif()
	if(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
		# Keep the directly compiled CpuX64/RuntimeLinker objects ABI-identical
		# to emulator. Otherwise VideoOut/Window's phase-6CO references pull a
		# second CpuX64 object from libemulator and duplicate every public symbol.
		target_compile_definitions(lumen_runtime_linker_address_test PRIVATE LUMEN_A64_DYNAREC_PHASE6CO_PROFILE=1)
	endif()
	if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		target_compile_definitions(lumen_runtime_linker_address_test PRIVATE
			LUMEN_A64_DYNAREC_HOST_PC_PROFILE=1
			LUMEN_A64_DYNAREC_HOST_PC_PROFILE_TEST=1)
	endif()
	target_link_libraries(lumen_runtime_linker_address_test PRIVATE emulator core sys math scripts lua SDL2-static rijndael lzma sqlite zstd
	                                                          easy_profiler cpuinfo Threads::Threads)
	add_test(NAME lumen_fex_unresolved_argument_regions
		COMMAND $<TARGET_FILE:lumen_runtime_linker_address_test> --fex-unresolved-arguments)
	set_tests_properties(lumen_fex_unresolved_argument_regions PROPERTIES TIMEOUT 30)
	add_test(NAME lumen_host_call_dispatch
		COMMAND $<TARGET_FILE:lumen_runtime_linker_address_test> --host-call-dispatch)
	set_tests_properties(lumen_host_call_dispatch PROPERTIES TIMEOUT 30
		PASS_REGULAR_EXPRESSION "host-call-dispatch metadata=ok classification=ok registry=ok macros=excluded")
	add_test(NAME lumen_runtime_linker_address COMMAND $<TARGET_FILE:lumen_runtime_linker_address_test>)
	add_test(NAME lumen_runtime_linker_address_registry_absent
		COMMAND "${CMAKE_COMMAND}" -E env
			--unset=LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY
			$<TARGET_FILE:lumen_runtime_linker_address_test>)
	add_test(NAME lumen_runtime_linker_address_registry_enabled
		COMMAND "${CMAKE_COMMAND}" -E env
			"LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=1"
			$<TARGET_FILE:lumen_runtime_linker_address_test>)
	add_test(NAME lumen_runtime_linker_address_registry_zero
		COMMAND "${CMAKE_COMMAND}" -E env
			"LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=0"
			$<TARGET_FILE:lumen_runtime_linker_address_test>)
	add_test(NAME lumen_runtime_linker_address_registry_empty
		COMMAND "${CMAKE_COMMAND}" -E env
			"LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY="
			$<TARGET_FILE:lumen_runtime_linker_address_test>)
	add_test(NAME lumen_runtime_linker_address_registry_malformed
		COMMAND "${CMAKE_COMMAND}" -E env
			"LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY=01"
			$<TARGET_FILE:lumen_runtime_linker_address_test>)
	set_tests_properties(
		lumen_runtime_linker_address
		lumen_runtime_linker_address_registry_absent
		lumen_runtime_linker_address_registry_enabled
		lumen_runtime_linker_address_registry_zero
		lumen_runtime_linker_address_registry_empty
		lumen_runtime_linker_address_registry_malformed
		PROPERTIES TIMEOUT 120)
	if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
		add_test(NAME lumen_runtime_linker_address_host_pc_boundary
		         COMMAND $<TARGET_FILE:lumen_runtime_linker_address_test>)
		set_tests_properties(lumen_runtime_linker_address_host_pc_boundary PROPERTIES
			TIMEOUT 30
			ENVIRONMENT "LUMEN_X64_A64_DYNAREC_HOST_PC_PROFILE=1;LUMEN_A64_DYNAREC_HOST_PC_PROFILE_ORACLE=runtime-linker")
		add_test(NAME lumen_runtime_linker_address_ir_storage_close_race
		         COMMAND $<TARGET_FILE:lumen_runtime_linker_address_test>)
		set_tests_properties(lumen_runtime_linker_address_ir_storage_close_race PROPERTIES
			TIMEOUT 30
			ENVIRONMENT "LUMEN_X64_A64_DYNAREC_HOST_PC_PROFILE=1;LUMEN_A64_DYNAREC_HOST_PC_PROFILE_ORACLE=ir-storage-close-race")
	endif()
endif()

if(WIN32 AND LUMEN_BITNESS EQUAL 64 AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$" AND
   LUMEN_WITH_VIRTUAL_MEMORY_TEST)
	add_executable(lumen_windows_virtual_range_test EXCLUDE_FROM_ALL
		WindowsVirtualRangeTest.cpp)
	target_link_libraries(lumen_windows_virtual_range_test PRIVATE core)
	if(MSVC AND CLANG)
		execute_process(
			COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
			OUTPUT_VARIABLE WINDOWS_VIRTUAL_RANGE_CLANG_RESOURCE_DIR
			OUTPUT_STRIP_TRAILING_WHITESPACE)
		find_library(WINDOWS_VIRTUAL_RANGE_CLANG_BUILTINS_LIBRARY
			NAMES clang_rt.builtins-x86_64
			PATHS "${WINDOWS_VIRTUAL_RANGE_CLANG_RESOURCE_DIR}/lib/windows"
			NO_DEFAULT_PATH)
		if(NOT WINDOWS_VIRTUAL_RANGE_CLANG_BUILTINS_LIBRARY)
			message(FATAL_ERROR "clang x86_64 builtins were not found for lumen_windows_virtual_range_test")
		endif()
		target_link_libraries(lumen_windows_virtual_range_test PRIVATE
			"${WINDOWS_VIRTUAL_RANGE_CLANG_BUILTINS_LIBRARY}")
	endif()
	add_test(NAME lumen_windows_virtual_range
		COMMAND $<TARGET_FILE:lumen_windows_virtual_range_test>)
	set_tests_properties(lumen_windows_virtual_range PROPERTIES
		TIMEOUT 30
		LABELS "virtual-memory;windows;sparse-reservation")
endif()

if(MACOS AND LUMEN_WITH_VIRTUAL_MEMORY_TEST)
	find_package(Threads REQUIRED)
	target_compile_definitions(core PRIVATE LUMEN_VIRTUAL_RANGE_TEST_HOOK=1)
	if(TARGET lumen_graphics_state_test)
		target_compile_definitions(lumen_graphics_state_test PRIVATE LUMEN_VIRTUAL_RANGE_TEST_HOOK=1)
	endif()
	if(TARGET emulator)
		target_compile_definitions(emulator PRIVATE LUMEN_VIRTUAL_RANGE_TEST_HOOK=1)
	endif()
	add_executable(lumen_virtual_memory_test EXCLUDE_FROM_ALL VirtualMemoryTest.cpp)
	target_compile_definitions(lumen_virtual_memory_test PRIVATE
		LUMEN_VIRTUAL_RANGE_TEST_HOOK=1)
	target_link_libraries(lumen_virtual_memory_test PRIVATE core Threads::Threads)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(lumen_virtual_memory_test PRIVATE LUMEN_FEXCORE_RUNTIME=1)
		target_link_libraries(lumen_virtual_memory_test PRIVATE lumen_macos_fexcore_runtime)
		foreach(snapshot_value IN ITEMS 0 1)
			add_test(NAME lumen_virtual_memory_handoff_snapshot_${snapshot_value}
				COMMAND "${CMAKE_COMMAND}" -E env "LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${snapshot_value}"
					$<TARGET_FILE:lumen_virtual_memory_test> --memory-handoff)
			set_tests_properties(lumen_virtual_memory_handoff_snapshot_${snapshot_value} PROPERTIES TIMEOUT 30
				PASS_REGULAR_EXPRESSION "virtual-memory memory-handoff=ok"
				LABELS "virtual-memory;handoff;backing;ownership")
		endforeach()
	endif()
	add_test(NAME lumen_virtual_memory COMMAND $<TARGET_FILE:lumen_virtual_memory_test>)
	set_tests_properties(lumen_virtual_memory PROPERTIES TIMEOUT 120)
	foreach(snapshot_value IN ITEMS 0 1)
		add_test(NAME lumen_virtual_memory_guest_data_continuity_snapshot_${snapshot_value}
			COMMAND "${CMAKE_COMMAND}" -E env
				"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${snapshot_value}"
				$<TARGET_FILE:lumen_virtual_memory_test>
				--guest-data-continuity)
		set_tests_properties(lumen_virtual_memory_guest_data_continuity_snapshot_${snapshot_value}
			PROPERTIES TIMEOUT 15
			PASS_REGULAR_EXPRESSION "virtual-memory guest-data-continuity=ok"
			LABELS "virtual-memory;continuity;guest-data")
		add_test(NAME lumen_virtual_memory_transaction_guest_data_snapshot_${snapshot_value}
			COMMAND "${CMAKE_COMMAND}" -E env
				"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${snapshot_value}"
				$<TARGET_FILE:lumen_virtual_memory_test>
				--transaction-guest-data)
		set_tests_properties(lumen_virtual_memory_transaction_guest_data_snapshot_${snapshot_value}
			PROPERTIES TIMEOUT 15
			PASS_REGULAR_EXPRESSION "virtual-memory transaction-guest-data=ok"
			LABELS "virtual-memory;transaction;guest-data")
		add_test(NAME lumen_virtual_memory_guest_data_concurrency_snapshot_${snapshot_value}
			COMMAND "${CMAKE_COMMAND}" -E env
				"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${snapshot_value}"
				$<TARGET_FILE:lumen_virtual_memory_test>
				--guest-data-concurrency)
		set_tests_properties(lumen_virtual_memory_guest_data_concurrency_snapshot_${snapshot_value}
			PROPERTIES TIMEOUT 15
			PASS_REGULAR_EXPRESSION "virtual-memory guest-data-concurrency=ok"
			LABELS "virtual-memory;concurrency;guest-data")
	endforeach()
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		foreach(snapshot_value IN ITEMS 0 1)
			foreach(preflight_selection IN ITEMS absent zero empty malformed enabled)
				if(preflight_selection STREQUAL "absent")
					set(preflight_selector --unset=LUMEN_EXPERIMENTAL_GUEST_DATA_SNAPSHOT_PREFLIGHT)
				elseif(preflight_selection STREQUAL "zero")
					set(preflight_selector LUMEN_EXPERIMENTAL_GUEST_DATA_SNAPSHOT_PREFLIGHT=0)
				elseif(preflight_selection STREQUAL "empty")
					set(preflight_selector LUMEN_EXPERIMENTAL_GUEST_DATA_SNAPSHOT_PREFLIGHT=)
				elseif(preflight_selection STREQUAL "malformed")
					set(preflight_selector LUMEN_EXPERIMENTAL_GUEST_DATA_SNAPSHOT_PREFLIGHT=01)
				else()
					set(preflight_selector LUMEN_EXPERIMENTAL_GUEST_DATA_SNAPSHOT_PREFLIGHT=1)
				endif()
				if(preflight_selection STREQUAL "absent" OR preflight_selection STREQUAL "enabled")
					set(preflight_expected 1)
				else()
					set(preflight_expected 0)
				endif()
				set(preflight_test lumen_virtual_memory_guest_data_snapshot_preflight_${preflight_selection}_snapshot_${snapshot_value})
				add_test(NAME ${preflight_test}
					COMMAND "${CMAKE_COMMAND}" -E env
						${preflight_selector}
						"LUMEN_EXPECT_GUEST_DATA_SNAPSHOT_PREFLIGHT=${preflight_expected}"
						"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=${snapshot_value}"
						$<TARGET_FILE:lumen_virtual_memory_test>
						--guest-data-snapshot-preflight)
				set_tests_properties(${preflight_test} PROPERTIES TIMEOUT 30
					PASS_REGULAR_EXPRESSION "virtual-memory guest-data-snapshot-preflight=ok"
					LABELS "virtual-memory;guest-data;snapshot;semantic")
			endforeach()
		endforeach()
	endif()
	add_test(NAME lumen_virtual_memory_snapshot_selector_enabled
		COMMAND "${CMAKE_COMMAND}" -E env
			"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=1"
			$<TARGET_FILE:lumen_virtual_memory_test>
			--snapshot-selector-enabled)
	add_test(NAME lumen_virtual_memory_snapshot_selector_absent
		COMMAND "${CMAKE_COMMAND}" -E env
			--unset=LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT
			$<TARGET_FILE:lumen_virtual_memory_test>
			--snapshot-selector-enabled)
	add_test(NAME lumen_virtual_memory_snapshot_selector_zero
		COMMAND "${CMAKE_COMMAND}" -E env
			"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=0"
			$<TARGET_FILE:lumen_virtual_memory_test>
			--snapshot-selector-disabled)
	add_test(NAME lumen_virtual_memory_snapshot_selector_empty
		COMMAND "${CMAKE_COMMAND}" -E env
			"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT="
			$<TARGET_FILE:lumen_virtual_memory_test>
			--snapshot-selector-disabled)
	add_test(NAME lumen_virtual_memory_snapshot_selector_malformed
		COMMAND "${CMAKE_COMMAND}" -E env
			"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=01"
			$<TARGET_FILE:lumen_virtual_memory_test>
			--snapshot-selector-disabled)
	set_tests_properties(
		lumen_virtual_memory_snapshot_selector_enabled
		lumen_virtual_memory_snapshot_selector_absent
		lumen_virtual_memory_snapshot_selector_zero
		lumen_virtual_memory_snapshot_selector_empty
		lumen_virtual_memory_snapshot_selector_malformed
		PROPERTIES TIMEOUT 120)
	if(LUMEN_MACOS_FEXCORE_RUNTIME)
		target_compile_definitions(emulator PRIVATE
			LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK=1)
		add_executable(lumen_kernel_virtual_range_test EXCLUDE_FROM_ALL
			KernelVirtualRangeTest.cpp)
		target_compile_definitions(lumen_kernel_virtual_range_test PRIVATE
			LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK=1
			LUMEN_VIRTUAL_RANGE_TEST_HOOK=1)
		target_link_libraries(lumen_kernel_virtual_range_test PRIVATE
			emulator Threads::Threads)
		add_test(NAME lumen_kernel_virtual_range_snapshot_enabled
			COMMAND "${CMAKE_COMMAND}" -E env
				"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=1"
				$<TARGET_FILE:lumen_kernel_virtual_range_test>
				--snapshot-enabled)
		set_tests_properties(lumen_kernel_virtual_range_snapshot_enabled
			PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "KernelVirtualRangeTest: PASS")
		add_test(NAME lumen_kernel_virtual_range_snapshot_disabled
			COMMAND "${CMAKE_COMMAND}" -E env
				"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=0"
				$<TARGET_FILE:lumen_kernel_virtual_range_test>
				--snapshot-disabled)
		set_tests_properties(lumen_kernel_virtual_range_snapshot_disabled
			PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "KernelVirtualRangeTest: PASS")
		add_executable(lumen_kernel_virtual_query_test EXCLUDE_FROM_ALL
			KernelVirtualQueryTest.cpp)
		target_compile_definitions(lumen_kernel_virtual_query_test PRIVATE
			LUMEN_KERNEL_VIRTUAL_RANGE_TEST_HOOK=1
			LUMEN_VIRTUAL_RANGE_TEST_HOOK=1)
		target_link_libraries(lumen_kernel_virtual_query_test PRIVATE
			emulator Threads::Threads)
		add_test(NAME lumen_kernel_virtual_query_snapshot_enabled
			COMMAND "${CMAKE_COMMAND}" -E env
				"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=1"
				$<TARGET_FILE:lumen_kernel_virtual_query_test>
				--snapshot-enabled)
		set_tests_properties(lumen_kernel_virtual_query_snapshot_enabled
			PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "KernelVirtualQueryTest: PASS"
			LABELS "kernel;virtual-memory;query")
		add_test(NAME lumen_kernel_virtual_query_snapshot_disabled
			COMMAND "${CMAKE_COMMAND}" -E env
				"LUMEN_VM_IMMUTABLE_REGION_SNAPSHOT=0"
				$<TARGET_FILE:lumen_kernel_virtual_query_test>
				--snapshot-disabled)
		set_tests_properties(lumen_kernel_virtual_query_snapshot_disabled
			PROPERTIES TIMEOUT 30
			PASS_REGULAR_EXPRESSION "KernelVirtualQueryTest: PASS"
			LABELS "kernel;virtual-memory;query")
	endif()
endif()

if(MACOS AND LUMEN_WITH_MFI_DIGITAL_QUEUE_TEST)
	add_executable(lumen_mfi_digital_queue_test EXCLUDE_FROM_ALL MFIDigitalQueueTest.cpp)
	target_include_directories(lumen_mfi_digital_queue_test PRIVATE
		"${PROJECT_SOURCE_DIR}/3rdparty/sdl2/sdl2/include"
		"${PROJECT_SOURCE_DIR}/3rdparty/sdl2/sdl2/src/joystick/iphoneos"
	)
	target_link_libraries(lumen_mfi_digital_queue_test PRIVATE SDL2-static)
	add_test(NAME lumen_mfi_digital_queue COMMAND $<TARGET_FILE:lumen_mfi_digital_queue_test>)
	set_tests_properties(lumen_mfi_digital_queue PROPERTIES TIMEOUT 60)
endif()

if(LUMEN_WITH_CONTROLLER_INPUT_TEST)
	add_executable(lumen_controller_digital_history_test EXCLUDE_FROM_ALL ControllerDigitalHistoryTest.cpp)
	target_include_directories(lumen_controller_digital_history_test PRIVATE "${PROJECT_SOURCE_DIR}")
	add_test(NAME lumen_controller_digital_history COMMAND $<TARGET_FILE:lumen_controller_digital_history_test>)
	set_tests_properties(lumen_controller_digital_history PROPERTIES TIMEOUT 60)
endif()

if(MACOS AND LUMEN_WITH_APPLE_VALIDATION)
	set(LUMEN_APPLE_REQUIRED_VALIDATION_TARGETS
		lumen_cpu_x64_test
		lumen_cpu_x64_region_v2_test
		lumen_cpu_a64_dynarec_test
		lumen_cpu_a64_hot_region_test
		lumen_audio_out_test
		lumen_avplayer_mac_test
		lumen_avplayer_lifetime_test
		lumen_graphics_state_test
		lumen_render_target_format_test
		lumen_tile_size_test
		lumen_guest_file_test
		lumen_filesystem_path_test
		lumen_ajm_test
		lumen_runtime_linker_address_test
		lumen_virtual_memory_test
		lumen_mfi_digital_queue_test
		lumen_controller_digital_history_test
	)
	if(TARGET lumen_kernel_virtual_range_test)
		list(APPEND LUMEN_APPLE_REQUIRED_VALIDATION_TARGETS
			lumen_kernel_virtual_range_test)
	endif()
	foreach(VALIDATION_TARGET IN LISTS LUMEN_APPLE_REQUIRED_VALIDATION_TARGETS)
		if(NOT TARGET ${VALIDATION_TARGET})
			message(FATAL_ERROR
				"lumen_apple_validation requires ${VALIDATION_TARGET}; configure macOS with the corresponding validation option enabled"
			)
		endif()
	endforeach()
	add_custom_target(lumen_apple_validation
		COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure --no-tests=error
		DEPENDS fc_script ${LUMEN_APPLE_REQUIRED_VALIDATION_TARGETS}
		WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
		USES_TERMINAL
		COMMENT "Build and run the native Apple validation gate"
	)
endif()

if(MINGW)
	SET(UNIT_TEST_LIB -Wl,--whole-archive unit_test -Wl,--no-whole-archive)
endif()	

if(MSVC)
	SET(UNIT_TEST_LIB unit_test)
endif()	

target_link_libraries(fc_script ${UNIT_TEST_LIB})
target_link_libraries(fc_script core)
target_link_libraries(fc_script sys)
target_link_libraries(fc_script math)
target_link_libraries(fc_script scripts)
target_link_libraries(fc_script lua)
target_link_libraries(fc_script SDL2-static)
target_link_libraries(fc_script emulator)
target_link_libraries(fc_script rijndael)
target_link_libraries(fc_script lzma)
target_link_libraries(fc_script sqlite)
target_link_libraries(fc_script zstd)
target_link_libraries(fc_script easy_profiler)
target_link_libraries(fc_script cpuinfo)

if (MSVC OR MINGW)
target_link_libraries(fc_script opengl32)
target_link_libraries(fc_script iphlpapi)
target_link_libraries(fc_script setupapi)
target_link_libraries(fc_script ws2_32)
target_link_libraries(fc_script psapi)
target_link_libraries(fc_script mfplat)
target_link_libraries(fc_script mfreadwrite)
target_link_libraries(fc_script mfuuid)
target_link_libraries(fc_script ole32)
target_link_libraries(fc_script shell32)
endif()
if (CLANG AND NOT MSVC AND NOT MACOS)
target_link_libraries(fc_script pthread)
endif()
if (CLANG AND MSVC)
target_link_libraries(fc_script winpthread)
	execute_process(
		COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
		OUTPUT_VARIABLE FC_SCRIPT_CLANG_RESOURCE_DIR
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	set(FC_SCRIPT_CLANG_BUILTINS_ARCH x86_64)
	if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(ARM64|ARM64EC)$" OR
	   CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
		set(FC_SCRIPT_CLANG_BUILTINS_ARCH aarch64)
	endif()
	find_library(FC_SCRIPT_CLANG_BUILTINS_LIBRARY
		NAMES "clang_rt.builtins-${FC_SCRIPT_CLANG_BUILTINS_ARCH}"
		PATHS "${FC_SCRIPT_CLANG_RESOURCE_DIR}/lib/windows"
		NO_DEFAULT_PATH
	)
	if(NOT FC_SCRIPT_CLANG_BUILTINS_LIBRARY)
		message(FATAL_ERROR "clang ${FC_SCRIPT_CLANG_BUILTINS_ARCH} builtins were not found for fc_script")
	endif()
	target_link_libraries(fc_script "${FC_SCRIPT_CLANG_BUILTINS_LIBRARY}")
endif()

if(LINUX)
	set_target_properties(fc_script PROPERTIES LINK_FLAGS "${LUMEN_LD_OPTIONS} -Wl,-Map=fc_script_${FYTY_COMPILER_ID}_${LUMEN_LINKER_ID}_${LUMEN_BITNESS}.map")
elseif(MSVC)
	set_target_properties(fc_script PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "$(SolutionDir)$(Configuration)/")
	set_target_properties(fc_script	PROPERTIES LINK_FLAGS "/DYNAMICBASE:NO /MAP:fc_script_msvc_${LUMEN_BITNESS}.map")
	add_custom_command(TARGET fc_script POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different fc_script_msvc_${LUMEN_BITNESS}.map $<TARGET_FILE_DIR:fc_script>/fc_script_${LUMEN_COMPILER_ID}_${LUMEN_LINKER_ID}_${LUMEN_BITNESS}.map)
	add_custom_command(TARGET fc_script POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/3rdparty/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:fc_script>/libwinpthread-1.dll)
elseif(MINGW)
	set_target_properties(fc_script PROPERTIES LINK_FLAGS "${LUMEN_LD_OPTIONS} -Wl,-Map=fc_script_${LUMEN_COMPILER_ID}_${LUMEN_LINKER_ID}_${LUMEN_BITNESS}.map")
	# Converting the map requires running fc_script and therefore the Vulkan
	# loader used by the emulator. Keep that runtime operation out of the normal
	# compile/link path; builders with Vulkan installed can request it explicitly.
	# A relink invalidates any CSV left by an older executable, while preserving
	# the authoritative GNU LD map produced by the linker.
	add_custom_command(TARGET fc_script POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E rm -f
		        "${CMAKE_CURRENT_BINARY_DIR}/fc_script_${LUMEN_COMPILER_ID}_${LUMEN_LINKER_ID}_${LUMEN_BITNESS}.csv"
		VERBATIM
	)
	add_custom_target(lumen_mingw_map_csv
		COMMAND "$<TARGET_FILE:fc_script>" "${PROJECT_SOURCE_DIR}/map_to_csv.lua"
		        fc_script ${LUMEN_COMPILER_ID} ${LUMEN_BITNESS} ${LUMEN_LINKER_ID}
		DEPENDS fc_script
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:fc_script>"
		VERBATIM
	)
endif()

add_dependencies(fc_script LumenGitVersion)

install(TARGETS fc_script DESTINATION .)
if(NOT ${LUMEN_PROJECT_NAME} MATCHES "Build_Tools")
	# CSV file is only generated by map_to_csv.lua for MinGW builds
	if(MINGW)
		install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fc_script_${LUMEN_COMPILER_ID}_${LUMEN_LINKER_ID}_${LUMEN_BITNESS}.csv DESTINATION . OPTIONAL)
	endif()
endif()

if(MSVC)
	lumen_apply_first_party_msvc_exception_policy("${PROJECT_SOURCE_DIR}")
endif()
		
if(MSVC)
    set(CMAKE_USE_RELATIVE_PATHS ON)
endif()

# Tests explicitly labeled FEX inspect that provider's private contracts.
# These existing probes always exercise FEX, independently of explicit overrides.
get_property(lumen_provider_tests DIRECTORY PROPERTY TESTS)
foreach(lumen_provider_test IN LISTS lumen_provider_tests)
	get_test_property("${lumen_provider_test}" LABELS lumen_provider_labels)
	if("fexcore" IN_LIST lumen_provider_labels OR "fex" IN_LIST lumen_provider_labels)
		set_property(TEST "${lumen_provider_test}" APPEND PROPERTY
			ENVIRONMENT "LUMEN_CPU_TRANSLATOR=fex")
	endif()
endforeach()

# Standalone source builds use the same PS4 child as the repository build.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT ANDROID AND NOT IOS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(lumen_gen4_runtime ALL
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gen4/tools/build_runtime.py"
            --parent "${CMAKE_BINARY_DIR}" --build "${LUMEN_GEN4_FULL_BUILD_DIR}"
            --deps "${LUMEN_GEN4_DEPS_DIR}" --ffmpeg "${LUMEN_GEN4_FFMPEG_PREBUILT_DIR}"
            --jobs "${LUMEN_GEN4_BUILD_JOBS}"
        BYPRODUCTS "${LUMEN_GEN4_FULL_BUILD_DIR}/lumen_gen4_runtime"
        USES_TERMINAL VERBATIM)
    # The independent Gen4 graph builds its pinned direct CPU provider.
    if(TARGET vulkan-1)
        add_dependencies(lumen_gen4_runtime vulkan-1)
    endif()
    if(TARGET fc_script)
        set_target_properties(fc_script PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()
