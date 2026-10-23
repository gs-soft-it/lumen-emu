# Lumen modifications, 2026-10-02: isolated native ARM64/FEX integration.
cmake_minimum_required(VERSION 3.18...4.3)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for bundled dependencies")
endif()

get_property(KYTY_GENERATOR_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT KYTY_GENERATOR_IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

project(Kyty)

find_program(SCCACHE_PROGRAM sccache)
find_program(CCACHE_PROGRAM ccache)

foreach(lang IN ITEMS C CXX)
    # An explicitly empty launcher disables caching for this language.
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER)
        if(SCCACHE_PROGRAM)
            set(CMAKE_${lang}_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
            message(STATUS "sccache enabled for ${lang}: ${SCCACHE_PROGRAM}")
        elseif(CCACHE_PROGRAM)
            set(CMAKE_${lang}_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
            message(STATUS "ccache enabled for ${lang}: ${CCACHE_PROGRAM}")
        endif()
    endif()
endforeach()

if(NOT LUMEN_PS5_DIRECT_FEX)
	include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/CompatibilitySources.cmake")
	lumen_gen5_prepare_compatibility_sources(LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT)
	set(KYTY_SOURCE_DIR "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/src")
	set(KYTY_TESTS_DIR "${LUMEN_GEN5_COMPATIBILITY_SOURCE_ROOT}/tests")
endif()
set(KYTY_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty")
set(KYTY_CPUINFO_TARGET kyty_cpuinfo)
set(KYTY_FMT_INCLUDE_DIR "${KYTY_THIRD_PARTY_DIR}/fmt/include")
if(LUMEN_PS5_FEX_C13 OR LUMEN_PS5_DIRECT_FEX)
	# FEX and the engine share fmt::fmt; its headers must match that archive.
	set(KYTY_FMT_INCLUDE_DIR "${LUMEN_ANDROID_FEXCORE_BUILD_SOURCE_REAL}/External/fmt/include")
endif()

option(LUMEN_PS5_UPSTREAM_20261018 "Build the adapted Gen5 October 18 candidate" OFF)
if(LUMEN_PS5_DIRECT_FEX)
	# The direct CPU uses the complete original engine and its host adapters.
	# The older selective graphics overlay belongs to the legacy engine only.
	set(KYTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../engine/src")
	set(KYTY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../engine/tests")
	add_compile_definitions(LUMEN_PS5_DIRECT_ENGINE=1 LUMEN_PS5_DIRECT_FEX=1)
elseif(LUMEN_PS5_UPSTREAM_20261018)
	include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/UpstreamCandidate.cmake")
endif()

if(CMAKE_SYSTEM_NAME MATCHES ".*Linux")
	set(LINUX TRUE)
endif()

if (NOT (WIN32 OR LINUX OR APPLE OR ANDROID))
    message(FATAL_ERROR "only Windows, Linux, macOS, and Android builds are supported")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include("${KYTY_SOURCE_DIR}/utils.cmake")
include(CTest)

option(KYTY_ENABLE_CLANG_TIDY "Run clang-tidy checks during builds" OFF)
option(LUMEN_PS5_FEXCORE "Use the audited Lumen FEX embedding for ARM64 guests" OFF)
set(LUMEN_PS5_FEX_TOOLCHAIN_FILE "" CACHE FILEPATH
  "Lumen module defining the isolated audited FEX link closure")
if(LUMEN_PS5_FEXCORE)
	if(NOT (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
		CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$"))
		message(FATAL_ERROR "LUMEN_PS5_FEXCORE requires a native ARM64 target")
	endif()
	if(NOT EXISTS "${LUMEN_PS5_FEX_TOOLCHAIN_FILE}")
		message(FATAL_ERROR "Provide LUMEN_PS5_FEX_TOOLCHAIN_FILE for the audited FEX closure")
	endif()
	add_compile_definitions(LUMEN_PS5_FEXCORE=1)
endif()

set(KYTY_BUILD_ORIGIN "Source" CACHE STRING "Build provenance shown in diagnostics")
set_property(CACHE KYTY_BUILD_ORIGIN PROPERTY STRINGS Source Official Fork)
if(NOT KYTY_BUILD_ORIGIN STREQUAL "Source" AND NOT KYTY_BUILD_ORIGIN STREQUAL "Official" AND
   NOT KYTY_BUILD_ORIGIN STREQUAL "Fork")
	message(FATAL_ERROR "KYTY_BUILD_ORIGIN must be Source, Official, or Fork")
endif()

set(KYTY_RELEASE_TAG "" CACHE STRING "Release tag embedded in official builds")
set(KYTY_BUILD_REPOSITORY "" CACHE STRING "GitHub repository embedded in fork builds")
set(KYTY_OFFICIAL_BUILD OFF)
set(KYTY_FORK_BUILD OFF)
if(KYTY_BUILD_ORIGIN STREQUAL "Official")
	if(KYTY_RELEASE_TAG STREQUAL "")
		message(FATAL_ERROR "Official builds require KYTY_RELEASE_TAG")
	endif()
	set(KYTY_OFFICIAL_BUILD ON)
elseif(KYTY_BUILD_ORIGIN STREQUAL "Fork")
	if(KYTY_BUILD_REPOSITORY STREQUAL "")
		message(FATAL_ERROR "Fork builds require KYTY_BUILD_REPOSITORY")
	endif()
	set(KYTY_FORK_BUILD ON)
endif()

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

include(TestBigEndian)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "only 64-bit builds are supported")
endif()

test_big_endian(BIG_ENDIAN)
if(BIG_ENDIAN)
	set(KYTY_ENDIAN KYTY_ENDIAN_BIG)
else()
	set(KYTY_ENDIAN KYTY_ENDIAN_LITTLE)
endif()

if(CMAKE_BUILD_TYPE MATCHES Debug)
	set(KYTY_BUILD KYTY_BUILD_DEBUG)
else()
	set(KYTY_BUILD KYTY_BUILD_RELEASE)
endif()

if(LINUX OR APPLE OR ANDROID)
	# macOS rides the POSIX/Linux code paths until it gets a dedicated platform
	set(KYTY_PLATFORM KYTY_PLATFORM_LINUX)
else()
	set(KYTY_PLATFORM KYTY_PLATFORM_WINDOWS)
endif()

set(CLANG FALSE)
set(GCC FALSE)
set(KYTY_CLANG_CL FALSE)
get_filename_component(KYTY_CXX_COMPILER_NAME "${CMAKE_CXX_COMPILER}" NAME_WE)
string(TOLOWER "${KYTY_CXX_COMPILER_NAME}" KYTY_CXX_COMPILER_NAME)

if(CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
	set(CLANG TRUE)
	set(KYTY_COMPILER CLANG)
	if(WIN32 AND KYTY_CXX_COMPILER_NAME STREQUAL "clang-cl")
		set(KYTY_CLANG_CL TRUE)
		set(KYTY_LINKER LLD_LINK)
	elseif(APPLE)
		set(KYTY_LINKER LD64) # Apple's default linker; lld flags don't apply
	else()
		set(KYTY_LINKER LLD)
	endif()
	if ((CMAKE_AR MATCHES "llvm") AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0.0) AND (CMAKE_CXX_COMPILER MATCHES "winlibs"))
		string(REPLACE "llvm-ar.exe" "ar.exe" CMAKE_AR ${CMAKE_AR}) # llvm-ar.exe 11.0.0 from winlibs is broken
	endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(GCC TRUE)
	set(KYTY_COMPILER GCC)
	set(KYTY_LINKER LD)
else()
	message(FATAL_ERROR "only Clang and GCC are supported; found ${CMAKE_CXX_COMPILER_ID}")
endif()

set(KYTY_PROJECT_NAME "Emulator" CACHE STRING "Project name suffix")

string(TOLOWER ${KYTY_COMPILER} KYTY_COMPILER_ID)
string(TOLOWER ${KYTY_LINKER} KYTY_LINKER_ID)

if (CLANG AND (KYTY_LINKER STREQUAL LLD))
	set(KYTY_LD_OPTIONS "-fuse-ld=lld")
endif()

if (KYTY_LINKER STREQUAL LD AND NOT LINUX)
	set(KYTY_LD_OPTIONS "-Wl,--image-base=0x100000000000")
endif()

project(Kyty${KYTY_PROJECT_NAME}${CMAKE_BUILD_TYPE}${KYTY_COMPILER} VERSION 0.3.0)

include_directories(
	${KYTY_THIRD_PARTY_DIR}/gtest/include
	${KYTY_THIRD_PARTY_DIR}/gtest
	${KYTY_FMT_INCLUDE_DIR}
	${KYTY_THIRD_PARTY_DIR}/magic_enum/include/magic_enum
	${PROJECT_BINARY_DIR}
	${KYTY_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/../../../../android/app/src/main/cpp
)

set(KYTY_VERSION "${PROJECT_VERSION}")

configure_file(
  ${KYTY_SOURCE_DIR}/cmake_config.h.in
  ${PROJECT_BINARY_DIR}/cmake_config.h
  )

find_package(Git)

add_custom_target(KytyGitVersion
	COMMAND ${CMAKE_COMMAND}
		-D INPUT_FILE=${KYTY_SOURCE_DIR}/kytyGitVersion.h.in
		-D OUTPUT_FILE=${PROJECT_BINARY_DIR}/kytyGitVersion.h
		-D GIT_EXECUTABLE=${GIT_EXECUTABLE}
		-D GIT_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
		-P ${KYTY_SOURCE_DIR}/generate_version.cmake
	BYPRODUCTS ${PROJECT_BINARY_DIR}/kytyGitVersion.h
	COMMENT "Generate kytyGitVersion.h"
)

add_library(kyty_git_version INTERFACE)
add_dependencies(kyty_git_version KytyGitVersion)

if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0.0)
	list(APPEND KYTY_IWYU
		kyty_emulator
		common
		#launcher
	)
	list(APPEND KYTY_CLANG_TIDY
		kyty_emulator
		#common
		launcher
	)
endif()

config_compiler_and_linker()

if(LUMEN_PS5_FEXCORE)
	include("${LUMEN_PS5_FEX_TOOLCHAIN_FILE}")
	if(NOT TARGET lumen_ps5_fex_embedding)
		message(FATAL_ERROR "FEX toolchain did not define lumen_ps5_fex_embedding")
	endif()
endif()
add_subdirectory("${KYTY_THIRD_PARTY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/3rdparty")
if(LUMEN_PS5_UPSTREAM_20261018 OR LUMEN_PS5_DIRECT_FEX)
	add_compile_definitions(XBYAK_NO_EXCEPTION)
endif()
add_subdirectory("${KYTY_SOURCE_DIR}/common" "${CMAKE_CURRENT_BINARY_DIR}/common")

file(GLOB kyty_emulator_src CONFIGURE_DEPENDS
	"${KYTY_SOURCE_DIR}/libs/*.cpp"
	"${KYTY_SOURCE_DIR}/libs/*.h"
	"${KYTY_SOURCE_DIR}/graphics/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/*.h"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/*.h"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/command_processor/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/command_processor/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/image/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/image/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/cfg/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/cfg/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/decode/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/decode/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/translate/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/translate/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/backend/spirv/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/backend/spirv/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.inc"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/*.h"
	"${KYTY_SOURCE_DIR}/graphics/presentation/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/*.h"
	"${KYTY_SOURCE_DIR}/graphics/presentation/window/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/window/*.h"
	"${KYTY_SOURCE_DIR}/kernel/*.cpp"
	"${KYTY_SOURCE_DIR}/kernel/*.h"
	"${KYTY_SOURCE_DIR}/loader/*.cpp"
	"${KYTY_SOURCE_DIR}/loader/*.h"
)

find_program(KYTY_GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED)
set(gpu_tiler_shader_dir "${KYTY_SOURCE_DIR}/graphics/host_gpu/shaders")
set(gpu_tiler_generated_dir "${PROJECT_BINARY_DIR}/gpu_tiler_shaders")
set(gpu_tiler_shader_names
	standard256
	standard4
	standard4_3d
	standard64
	standard64_3d
	prt
	prt_3d
	render_target
	depth
	promote_d16
	demote_d16
	color_transform
)
file(GLOB gpu_tiler_shader_includes CONFIGURE_DEPENDS "${gpu_tiler_shader_dir}/gpu_tiler_*.inc")
foreach(shader_name IN LISTS gpu_tiler_shader_names)
	set(shader_source "${gpu_tiler_shader_dir}/gpu_tiler_${shader_name}.comp")
	set(shader_spv "${gpu_tiler_generated_dir}/gpu_tiler_${shader_name}.spv")
	set(shader_header "${gpu_tiler_generated_dir}/gpu_tiler_${shader_name}_spv.h")
	string(TOUPPER "GPU_TILER_${shader_name}_SPV" shader_symbol)
	add_custom_command(
		OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_tiler_generated_dir}"
		COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
		        "-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
		        -DSYMBOL=${shader_symbol} -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}" ${gpu_tiler_shader_includes}
		VERBATIM
	)
	list(APPEND gpu_tiler_shader_headers "${shader_header}")
endforeach()

set(fault_buffer_shader_source "${gpu_tiler_shader_dir}/fault_buffer_process.comp")
set(fault_buffer_shader_spv "${gpu_tiler_generated_dir}/fault_buffer_process.spv")
set(fault_buffer_shader_header "${gpu_tiler_generated_dir}/fault_buffer_process_spv.h")
add_custom_command(
	OUTPUT "${fault_buffer_shader_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_tiler_generated_dir}"
	COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
	        -DCACHING_PAGEBITS=14 -DMAX_PAGE_FAULTS=1024
	        -o "${fault_buffer_shader_spv}" "${fault_buffer_shader_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${fault_buffer_shader_spv} -DOUTPUT=${fault_buffer_shader_header}
	        -DSYMBOL=FAULT_BUFFER_PROCESS_SPV -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${fault_buffer_shader_source}"
	VERBATIM
)
list(APPEND kyty_emulator_src "${fault_buffer_shader_header}")
list(APPEND kyty_emulator_src ${gpu_tiler_shader_headers})

set(gpu_blit_generated_dir "${PROJECT_BINARY_DIR}/gpu_blit_shaders")
set(gpu_blit_shader_sources
	"${gpu_tiler_shader_dir}/gpu_blit_fs_triangle.vert"
	"${gpu_tiler_shader_dir}/gpu_blit_color_to_ms_depth.frag"
	"${gpu_tiler_shader_dir}/gpu_video_out_overlay.frag"
)
foreach(shader_source IN LISTS gpu_blit_shader_sources)
	get_filename_component(shader_name "${shader_source}" NAME_WE)
	set(shader_spv "${gpu_blit_generated_dir}/${shader_name}.spv")
	set(shader_header "${gpu_blit_generated_dir}/${shader_name}_spv.h")
	string(TOUPPER "${shader_name}_SPV" shader_symbol)
	add_custom_command(
		OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_blit_generated_dir}"
		COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
		        "-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
		        -DSYMBOL=${shader_symbol} -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}"
		VERBATIM
	)
	list(APPEND gpu_blit_shader_headers "${shader_header}")
endforeach()
if(LUMEN_PS5_DIRECT_FEX)
	set(shader_source "${gpu_tiler_shader_dir}/gpu_video_out_overlay.frag")
	set(shader_spv "${gpu_blit_generated_dir}/gpu_imgui_hdr.spv")
	set(shader_header "${gpu_blit_generated_dir}/gpu_imgui_hdr_spv.h")
	add_custom_command(OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_blit_generated_dir}"
		COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os -DIMGUI_HDR=1
			"-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
			-DSYMBOL=GPU_IMGUI_HDR_SPV -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}" VERBATIM)
	list(APPEND gpu_blit_shader_headers "${shader_header}")
endif()
list(APPEND kyty_emulator_src ${gpu_blit_shader_headers})

list(APPEND kyty_emulator_src
	"${KYTY_SOURCE_DIR}/emulator.h"
	"${KYTY_SOURCE_DIR}/emulator.cpp"
)
list(REMOVE_DUPLICATES kyty_emulator_src)

if (KYTY_CLANG_CL)
	set_source_files_properties(${kyty_emulator_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
endif()

set(kyty_emulator_link_libraries common Vulkan::Headers kyty_spirv_tools_opt kyty_spirv_tools SDL3::SDL3-static imgui xxhash FFmpeg::ffmpeg fmt::fmt nlohmann_json::nlohmann_json LibAtrac9 kyty_git_version Zydis::Zydis xbyak::xbyak)

# Linux system libraries.
if(LINUX OR ANDROID)
	find_package(Threads REQUIRED)
	list(APPEND kyty_emulator_link_libraries m ${CMAKE_DL_LIBS} Threads::Threads)
endif()

set(inc_headers
	${KYTY_SOURCE_DIR}
	${KYTY_THIRD_PARTY_DIR}/VulkanMemoryAllocator/include
	${KYTY_THIRD_PARTY_DIR}/SPIRV-Tools/include
	${KYTY_THIRD_PARTY_DIR}/SPIRV-Headers/include
	${KYTY_FMT_INCLUDE_DIR}
	${KYTY_THIRD_PARTY_DIR}/xxhash
	${KYTY_THIRD_PARTY_DIR}/cpuinfo/include
	${KYTY_THIRD_PARTY_DIR}/renderdoc
	${KYTY_THIRD_PARTY_DIR}/stb
)

if(LUMEN_PS5_DIRECT_FEX)
	list(APPEND inc_headers "${CMAKE_CURRENT_SOURCE_DIR}/../direct_fex")
endif()

if (KYTY_CLANG_CL)
	list(APPEND kyty_emulator_link_libraries winpthread)
	list(APPEND inc_headers
		${KYTY_THIRD_PARTY_DIR}/winpthread/include
	)
endif()

list(APPEND check_headers
	${KYTY_SOURCE_DIR}
)

# Lumen, 2026-10-02: Android embeds the complete engine in a distinct process.
# The CLI entry point and desktop-only regression executables are not part of
# this DSO. VM, loader, typed HLE, graphics and audio are the production sources.
if(ANDROID)
	if(NOT LUMEN_PS5_FEXCORE)
		message(FATAL_ERROR "The Android Gen5 engine requires the audited FEX embedding")
	endif()
	list(REMOVE_ITEM kyty_emulator_src
		"${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
		"${KYTY_SOURCE_DIR}/loader/redZonePatcher.cpp")
	if(LUMEN_PS5_DIRECT_FEX)
		# Guest x86 instructions are translated by the direct CPU engine.
		list(REMOVE_ITEM kyty_emulator_src
			"${KYTY_SOURCE_DIR}/loader/guestInstructionPatcher.cpp")
		# These suites exercise the legacy facade's epoch/lease contracts.
		list(REMOVE_ITEM kyty_emulator_src
			"${KYTY_SOURCE_DIR}/loader/fexRuntimeTests.cpp"
			"${KYTY_SOURCE_DIR}/loader/fexGpuTests.cpp")
	endif()
	add_library(lumen_android_ps5 SHARED ${kyty_emulator_src})
	if(NOT LUMEN_PS5_DIRECT_FEX)
		target_sources(lumen_android_ps5 PRIVATE
			$<TARGET_OBJECTS:lumen_android_fexcore_lab_admission>)
	endif()
	target_compile_options(lumen_android_ps5 PRIVATE -fexceptions)
	target_compile_definitions(lumen_android_ps5 PRIVATE LUMEN_PS5_ANDROID_ENGINE=1)
	target_include_directories(lumen_android_ps5 PUBLIC
		"${KYTY_SOURCE_DIR}" "${PROJECT_BINARY_DIR}"
		PRIVATE ${inc_headers})
	find_library(KYTY_ANDROID_LOG log REQUIRED)
	find_library(KYTY_ANDROID_PLATFORM android REQUIRED)
	target_link_libraries(lumen_android_ps5 PRIVATE
		lumen_ps5_fex_embedding ${kyty_emulator_link_libraries}
		"${KYTY_ANDROID_LOG}" "${KYTY_ANDROID_PLATFORM}" "${CMAKE_DL_LIBS}")
	target_link_options(lumen_android_ps5 PRIVATE
		"-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384"
		# FFmpeg's static AArch64 assembly addresses private tables directly.
		# Localize this closure, preserving SDL3 JNI and audited FEX exports.
		"-Wl,--exclude-libs,libavformat.a:libavcodec.a:libswscale.a:libavutil.a:libavfilter.a:libswresample.a"
		"-Wl,-u,JNI_OnLoad")
	if(NOT LUMEN_PS5_DIRECT_FEX)
	add_executable(lumen_ps5_android_runtime_test EXCLUDE_FROM_ALL
		"${CMAKE_CURRENT_SOURCE_DIR}/../fex/AndroidRuntimeSmoke.cpp")
	set_target_properties(lumen_ps5_android_runtime_test PROPERTIES SKIP_BUILD_RPATH TRUE)
	target_link_libraries(lumen_ps5_android_runtime_test PRIVATE lumen_android_ps5)
	target_link_options(lumen_ps5_android_runtime_test PRIVATE
		"-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
	endif()
	if(LUMEN_PS5_DIRECT_FEX)
		# Its namespace function-pointer globals must not interpose Bionic's
		# real functions when the engine is loaded into a global ELF scope.
		target_link_options(lumen_android_ps5 PRIVATE
			"-Wl,--exclude-libs,liblinkernsbypass.a")
		include("${CMAKE_CURRENT_SOURCE_DIR}/../engine/Tests.cmake")
	endif()
	return()
endif()

function(add_kyty_full_emulator_test target source)
	add_executable(${target} EXCLUDE_FROM_ALL ${source} ${kyty_emulator_src})
	target_link_libraries(${target} ${kyty_emulator_link_libraries})
	target_include_directories(${target} PRIVATE ${inc_headers})
	if(LUMEN_PS5_FEXCORE)
		# Full-emulator tests compile the runtime's checked HLE marshalling and
		# require the same FEX embedding and native decoder exclusion.
		target_compile_options(${target} PRIVATE -fexceptions)
		target_link_libraries(${target} lumen_ps5_fex_embedding)
		get_target_property(full_emulator_test_sources ${target} SOURCES)
		list(REMOVE_ITEM full_emulator_test_sources
			"${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
			"${KYTY_SOURCE_DIR}/loader/redZonePatcher.cpp")
		set_property(TARGET ${target} PROPERTY SOURCES "${full_emulator_test_sources}")
	endif()
	if (WIN32)
		target_link_libraries(${target} iphlpapi)
	endif()
	if (KYTY_CLANG_CL)
		target_link_libraries(${target} onecore)
		add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:${target}>/libwinpthread-1.dll)
	endif()
	# The macOS x86_64 guest address space needs its .zerofill segments anchored
	# by linker flags, or the kernel kills the binary on load (posix_spawn EIO).
	configure_macos_guest_address_space(${target})
endfunction()

function(configure_macos_guest_address_space target)
	if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
			(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")))
		target_sources(${target} PRIVATE "${KYTY_SOURCE_DIR}/kernel/macosGuestAddressSpace.cpp")
		target_compile_definitions(${target} PRIVATE KYTY_LINKED_GUEST_ADDRESS_SPACE=1)
		target_link_options(${target} PRIVATE
			-Wl,-ld_classic,-no_pie,-no_fixup_chains,-no_huge,-pagezero_size,0x40000,-segaddr,SYSTEM_MANAGED,0x40000,-segaddr,SYSTEM_RESERVED,0x7ffffc000,-segaddr,USER_AREA,0x7000000000,-image_base,0x700000000000)
	endif()
endfunction()

add_kyty_full_emulator_test(shader_cfg_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp")

add_executable(scalar_provenance_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ScalarProvenanceTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ReadLaneElimination.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
)
target_link_libraries(scalar_provenance_tests fmt::fmt)
target_include_directories(scalar_provenance_tests PRIVATE ${inc_headers})

add_executable(page_manager_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PageManagerTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
)
target_link_libraries(page_manager_tests common)
target_include_directories(page_manager_tests PRIVATE ${inc_headers})

add_executable(archive_file_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ArchiveFileTests.cpp"
)
target_link_libraries(archive_file_tests common ZArchive::zarchive)
target_include_directories(archive_file_tests PRIVATE ${inc_headers})

add_executable(avplayer_file_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/AvPlayerFileTests.cpp")
target_link_libraries(avplayer_file_tests common FFmpeg::ffmpeg ZArchive::zarchive)
target_include_directories(avplayer_file_tests PRIVATE ${inc_headers})

add_executable(bit_array_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/BitArrayTests.cpp"
)
target_include_directories(bit_array_tests PRIVATE ${inc_headers})

add_executable(lru_cache_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/LruCacheTests.cpp"
)
target_include_directories(lru_cache_tests PRIVATE ${inc_headers})

add_executable(ime_dialog_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ImeDialogTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeCommon.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeDialog.cpp"
)
target_include_directories(ime_dialog_tests PRIVATE ${inc_headers})

# Host-only IME replacement contracts: callbacks are native fixture functions,
# so this isolated target must not inherit translated-guest callback routing.
find_package(Threads REQUIRED)
add_executable(ime_host_text_replacement_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ImeHostTextReplacementTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeCommon.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeDialog.cpp"
	"${KYTY_SOURCE_DIR}/libs/ime.cpp"
)
target_include_directories(ime_host_text_replacement_tests PRIVATE ${inc_headers})
target_compile_definitions(ime_host_text_replacement_tests PRIVATE KYTY_IME_TESTS=1)
if(MSVC)
	target_compile_options(ime_host_text_replacement_tests PRIVATE /ULUMEN_PS5_FEXCORE)
else()
	target_compile_options(ime_host_text_replacement_tests PRIVATE -ULUMEN_PS5_FEXCORE)
endif()
target_link_libraries(ime_host_text_replacement_tests PRIVATE fmt::fmt Threads::Threads)

add_executable(memory_tracker_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/MemoryTrackerTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/memoryTracker.cpp"
)
target_link_libraries(memory_tracker_tests fmt::fmt common)
target_include_directories(memory_tracker_tests PRIVATE ${inc_headers})

add_executable(shader_vertex_metadata_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ShaderVertexMetadataTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderVertexMetadata.cpp"
)
target_include_directories(shader_vertex_metadata_tests PRIVATE ${inc_headers})

add_executable(spirv_module_admission_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SpirvModuleAdmissionTests.cpp"
)
target_include_directories(spirv_module_admission_tests PRIVATE ${inc_headers})

add_executable(swapchain_present_policy_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SwapchainPresentPolicyTests.cpp"
)
target_include_directories(swapchain_present_policy_tests PRIVATE ${inc_headers})
target_link_libraries(swapchain_present_policy_tests Vulkan::Headers)

add_executable(frame_pacing_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/FramePacingTests.cpp")
target_include_directories(frame_pacing_tests PRIVATE "${KYTY_SOURCE_DIR}")

add_executable(resource_materialization_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ResourceMaterializationTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
)
target_link_libraries(resource_materialization_tests fmt::fmt)
target_include_directories(resource_materialization_tests PRIVATE ${inc_headers})

add_executable(resource_tracking_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ResourceTrackingTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderPixelParameter.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ShaderInfoCollection.cpp"
)
target_link_libraries(resource_tracking_tests fmt::fmt)
target_include_directories(resource_tracking_tests PRIVATE ${inc_headers})
if(LUMEN_PS5_UPSTREAM_20261018)
	foreach(candidate_test scalar_provenance_tests resource_materialization_tests resource_tracking_tests)
		target_link_libraries(${candidate_test} xbyak::xbyak)
	endforeach()
endif()

add_executable(audio_out2_port_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AudioOut2PortTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/libAudio2.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(audio_out2_port_tests common fmt::fmt)
target_include_directories(audio_out2_port_tests PRIVATE ${inc_headers})

add_executable(ngs2_sampler_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/Ngs2SamplerTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(ngs2_sampler_tests common fmt::fmt LibAtrac9)
target_include_directories(ngs2_sampler_tests PRIVATE ${inc_headers})

add_executable(pad_haptics_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PadHapticsTests.cpp"
)
target_link_libraries(pad_haptics_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(pad_haptics_tests PRIVATE ${inc_headers})

add_executable(save_data_memory_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SaveDataMemoryTests.cpp"
)
target_link_libraries(save_data_memory_tests common fmt::fmt)
target_include_directories(save_data_memory_tests PRIVATE ${inc_headers})

add_executable(png_enc_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/PngEncTests.cpp")
target_link_libraries(png_enc_tests common fmt::fmt)
target_include_directories(png_enc_tests PRIVATE ${inc_headers})

add_executable(ces_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/CesTests.cpp")
target_link_libraries(ces_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(ces_tests PRIVATE ${inc_headers})

add_executable(save_data_dialog_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SaveDataDialogTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/dialog.cpp"
)
target_link_libraries(save_data_dialog_tests common fmt::fmt)
target_include_directories(save_data_dialog_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(http_uri_parse_tests
	"${KYTY_TESTS_DIR}/HttpUriParseTests.cpp")

add_executable(event_queue_lifetime_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/EventQueueLifetimeTests.cpp"
	"${KYTY_SOURCE_DIR}/kernel/eventQueue.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(event_queue_lifetime_tests common fmt::fmt)
target_include_directories(event_queue_lifetime_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(kernel_file_system_tests
	"${KYTY_TESTS_DIR}/KernelFileSystemTests.cpp")
target_link_libraries(kernel_file_system_tests ZArchive::zarchive)

add_executable(sync_on_address_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SyncOnAddressTests.cpp"
	"${KYTY_SOURCE_DIR}/kernel/syncOnAddress.cpp"
)
target_link_libraries(sync_on_address_tests common)
target_include_directories(sync_on_address_tests PRIVATE ${inc_headers})

add_executable(image_page_table_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ImagePageTableTests.cpp"
)
target_link_libraries(image_page_table_tests common fmt::fmt)
target_include_directories(image_page_table_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(shader_recompiler_compute_tests "${KYTY_TESTS_DIR}/ShaderRecompilerComputeTests.cpp")

set(gpu_test_generated_dir "${PROJECT_BINARY_DIR}/gpu_test_shaders")
set(gpu_test_ms_depth_source
	"${gpu_tiler_shader_dir}/gpu_test_ms_depth.comp")
set(gpu_test_ms_depth_spv
	"${gpu_test_generated_dir}/gpu_test_ms_depth.spv")
set(gpu_test_ms_depth_header
	"${gpu_test_generated_dir}/gpu_test_ms_depth_spv.h")
add_custom_command(
	OUTPUT "${gpu_test_ms_depth_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_test_generated_dir}"
	COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
	        -o "${gpu_test_ms_depth_spv}" "${gpu_test_ms_depth_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${gpu_test_ms_depth_spv}
	        -DOUTPUT=${gpu_test_ms_depth_header} -DSYMBOL=GPU_TEST_MS_DEPTH_SPV
	        -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${gpu_test_ms_depth_source}"
	VERBATIM
)
target_sources(shader_recompiler_compute_tests PRIVATE
	"${gpu_test_ms_depth_header}")

add_kyty_full_emulator_test(virtual_memory_allocation_tests "${KYTY_TESTS_DIR}/VirtualMemoryAllocationTests.cpp")
target_compile_definitions(virtual_memory_allocation_tests PRIVATE
	KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS=1)

# These tests use exceptions.
if(NOT KYTY_CLANG_CL)
	foreach(kyty_exception_test scalar_provenance_tests resource_tracking_tests
			virtual_memory_allocation_tests)
		target_compile_options(${kyty_exception_test} PRIVATE -fexceptions)
	endforeach()
endif()

if(BUILD_TESTING)
	add_test(NAME emulator_user_name_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --user-name 1234567890123456)
	add_test(NAME emulator_user_name_too_long
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --user-name 12345678901234567)
	set_tests_properties(emulator_user_name_too_long PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_present_mode_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --present-mode Immediate)
	add_test(NAME emulator_present_mode_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --present-mode Invalid)
	set_tests_properties(emulator_present_mode_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME ime_dialog COMMAND $<TARGET_FILE:ime_dialog_tests>)
	add_test(NAME ime_host_text_replacement COMMAND $<TARGET_FILE:ime_host_text_replacement_tests>)
	set_tests_properties(ime_host_text_replacement PROPERTIES LABELS "ime;host;replacement" TIMEOUT 30)
	add_test(NAME shader_cfg COMMAND $<TARGET_FILE:shader_cfg_tests>)
	add_test(NAME scalar_provenance COMMAND $<TARGET_FILE:scalar_provenance_tests>)
	add_test(NAME image_page_table COMMAND $<TARGET_FILE:image_page_table_tests>)
	add_test(NAME memory_tracker COMMAND $<TARGET_FILE:memory_tracker_tests>)
	add_test(NAME page_manager COMMAND $<TARGET_FILE:page_manager_tests>)
	add_test(NAME archive_file COMMAND $<TARGET_FILE:archive_file_tests>)
	add_test(NAME avplayer_file COMMAND $<TARGET_FILE:avplayer_file_tests>)
	add_test(NAME bit_array COMMAND $<TARGET_FILE:bit_array_tests>)
	add_test(NAME lru_cache COMMAND $<TARGET_FILE:lru_cache_tests>)
	add_test(NAME shader_vertex_metadata COMMAND $<TARGET_FILE:shader_vertex_metadata_tests>)
	add_test(NAME spirv_module_admission COMMAND $<TARGET_FILE:spirv_module_admission_tests>)
	add_test(NAME swapchain_present_policy COMMAND $<TARGET_FILE:swapchain_present_policy_tests>)
	add_test(NAME frame_pacing COMMAND $<TARGET_FILE:frame_pacing_tests>)
	add_test(NAME resource_materialization COMMAND $<TARGET_FILE:resource_materialization_tests>)
	add_test(NAME resource_tracking COMMAND $<TARGET_FILE:resource_tracking_tests>)
	add_test(NAME event_queue_lifetime COMMAND $<TARGET_FILE:event_queue_lifetime_tests>)
	add_test(NAME kernel_file_system COMMAND $<TARGET_FILE:kernel_file_system_tests>)
	add_test(NAME sync_on_address COMMAND $<TARGET_FILE:sync_on_address_tests>)
	add_test(NAME audio_out2_port COMMAND $<TARGET_FILE:audio_out2_port_tests>)
	add_test(NAME ngs2_sampler COMMAND $<TARGET_FILE:ngs2_sampler_tests>)
	add_test(NAME pad_haptics COMMAND $<TARGET_FILE:pad_haptics_tests>)
	add_test(NAME save_data_memory COMMAND $<TARGET_FILE:save_data_memory_tests>)
	add_test(NAME png_enc COMMAND $<TARGET_FILE:png_enc_tests>)
	add_test(NAME ces COMMAND $<TARGET_FILE:ces_tests>)
	add_test(NAME save_data_dialog COMMAND $<TARGET_FILE:save_data_dialog_tests>)
	add_test(NAME http_uri_parse COMMAND $<TARGET_FILE:http_uri_parse_tests>)
	add_test(NAME shader_recompiler_compute COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	add_test(NAME graphics_rect_list_interfaces
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --rect-list-shaders-only)
	add_test(NAME shader_recompiler_alignbyte
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --alignbyte-only)
	add_test(NAME graphics_pipeline_rasterization
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --polygon-mode-only)
	add_test(NAME graphics_draw_offsets
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --draw-offset-only)
	add_test(NAME virtual_memory_allocation
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests>)
	add_test(NAME guest_red_zone_patcher
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --red-zone-patcher-only)
	add_test(NAME command_scheduler_timeline
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --scheduler-only)
	add_test(NAME stream_buffer_ring
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --stream-buffer-only)
	add_test(NAME gpu_command_lane
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-command-lane-only)
	add_test(NAME pm4_context_state
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --context-state-only)
	add_test(NAME gpu_tiler
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-tiler-only)
	add_test(NAME image_transfer_layout
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-layout-only)

	add_test(NAME texture_cache_layered_image
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --layered-image-only)
	add_test(NAME host_image_allocation
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --host-image-allocation-only)
	add_test(NAME texture_cache_image_views
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-view-cache-only)
	add_test(NAME texture_cache_storage_sampled
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --storage-sampled-only)
	add_test(NAME texture_cache_depth_readback
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --depth-readback-only)
	add_test(NAME compute_meta_clear_classification
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --compute-meta-clear-only)
	add_test(NAME buffer_cache_dirty_gc
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-gc-only)
	add_test(NAME color_render_target_1d
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --rt1d-only)

	if(WIN32)
		# These tests still depend on the Windows multisample-depth path.
		add_test(NAME texture_cache_image_overlap
			COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-overlap-only)
		add_test(NAME texture_cache_htile_clear
			COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --htile-clear-only)
		add_test(NAME buffer_cache_ranges
			COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-range-only)
	endif()

	add_custom_target(kyty_tests DEPENDS
		ime_dialog_tests
		ime_host_text_replacement_tests
		shader_cfg_tests
		scalar_provenance_tests
		image_page_table_tests
		memory_tracker_tests
		page_manager_tests
		archive_file_tests
		avplayer_file_tests
		bit_array_tests
		lru_cache_tests
		shader_vertex_metadata_tests
		spirv_module_admission_tests
		swapchain_present_policy_tests
		frame_pacing_tests
		resource_materialization_tests
		resource_tracking_tests
		event_queue_lifetime_tests
		kernel_file_system_tests
		sync_on_address_tests
		shader_recompiler_compute_tests
		virtual_memory_allocation_tests
		audio_out2_port_tests
		ngs2_sampler_tests
		pad_haptics_tests
		save_data_memory_tests
		png_enc_tests
		ces_tests
		save_data_dialog_tests
		http_uri_parse_tests
	)
endif()

# Test Darwin signal-context restoration for SSE4a instructions.
if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
		(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")))
	add_executable(macos_sse4a_tests EXCLUDE_FROM_ALL
		"${KYTY_TESTS_DIR}/macosSse4aTests.cpp"
		"${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
	)
	target_link_libraries(macos_sse4a_tests common fmt::fmt Zydis::Zydis)
	target_include_directories(macos_sse4a_tests PRIVATE ${inc_headers})
	if(BUILD_TESTING)
		add_test(NAME macos_sse4a COMMAND $<TARGET_FILE:macos_sse4a_tests>)
		set_tests_properties(macos_sse4a PROPERTIES TIMEOUT 15)
		add_dependencies(kyty_tests macos_sse4a_tests)
	endif()
endif()

if(LUMEN_PS5_FEXCORE)
	list(REMOVE_ITEM kyty_emulator_src
		"${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
		"${KYTY_SOURCE_DIR}/loader/redZonePatcher.cpp")
endif()
add_executable(kyty_emulator "${KYTY_SOURCE_DIR}/main.cpp" ${kyty_emulator_src})
if(LUMEN_PS5_FEXCORE)
	target_link_libraries(kyty_emulator lumen_ps5_fex_embedding)
	target_sources(kyty_emulator PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../fex/GameCache.cpp")
	target_include_directories(kyty_emulator PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/sqlite/include"
		"${CMAKE_CURRENT_SOURCE_DIR}/../../../3rdparty/sqlite/src")
	# Checked HLE marshalling reports argument-read failures to its caller.
	# Catch them on the native side before returning through FEX's noexcept ABI.
	target_compile_options(kyty_emulator PRIVATE -fexceptions)
	add_executable(lumen_ps5_host_call_test EXCLUDE_FROM_ALL
		"${CMAKE_CURRENT_SOURCE_DIR}/../fex/HostCallSmoke.cpp"
		"${KYTY_SOURCE_DIR}/loader/hostCall.cpp")
	target_compile_options(lumen_ps5_host_call_test PRIVATE -fexceptions)
	target_link_libraries(lumen_ps5_host_call_test PRIVATE lumen_ps5_fex_embedding)
	add_executable(lumen_ps5_memory_rights_test EXCLUDE_FROM_ALL
		"${KYTY_TESTS_DIR}/GuestMemoryRightsTests.cpp")
	add_executable(lumen_ps5_backing_policy_test EXCLUDE_FROM_ALL
		"${CMAKE_CURRENT_SOURCE_DIR}/../fex/GuestBackingPolicyTest.cpp")
	target_link_libraries(lumen_ps5_backing_policy_test PRIVATE xxhash)
	if(BUILD_TESTING)
		add_test(NAME lumen_ps5_typed_host_calls COMMAND lumen_ps5_host_call_test)
		# A separate cold process catches re-entrant effects queries from the
		# runtime's initialization bridge probes, before any loader preflight.
		add_test(NAME lumen_ps5_typed_host_calls_short_admission COMMAND lumen_ps5_host_call_test)
		set_tests_properties(lumen_ps5_typed_host_calls_short_admission PROPERTIES
			ENVIRONMENT "LUMEN_FEX_HLE_SHORT_ADMISSION=1" TIMEOUT 60)
		add_test(NAME lumen_ps5_guest_memory_rights COMMAND lumen_ps5_memory_rights_test)
		add_test(NAME lumen_ps5_runtime_contracts COMMAND kyty_emulator --fex-runtime-self-test)
		add_test(NAME lumen_ps5_backing_policy COMMAND lumen_ps5_backing_policy_test)
		add_test(NAME lumen_ps5_gpu_backing_publication COMMAND kyty_emulator --fex-gpu-backing-self-test)
		set_tests_properties(lumen_ps5_gpu_backing_publication PROPERTIES TIMEOUT 60)
		set_tests_properties(lumen_ps5_runtime_contracts lumen_ps5_gpu_backing_publication
			PROPERTIES ENVIRONMENT "LUMEN_CPU_TRANSLATOR=fex")
		set_tests_properties(lumen_ps5_typed_host_calls lumen_ps5_guest_memory_rights
			lumen_ps5_runtime_contracts
			lumen_ps5_backing_policy
			PROPERTIES TIMEOUT 60)
		if(TARGET lumen_qemu_tcg_core OR TARGET lumen_ps5_qemu_tcg_core)
			add_test(NAME lumen_ps5_qemu_runtime_contracts COMMAND "${CMAKE_COMMAND}" -E env
				LUMEN_CPU_TRANSLATOR=qemu-tcg $<TARGET_FILE:kyty_emulator> --fex-runtime-self-test)
			add_test(NAME lumen_ps5_qemu_gpu_backing_publication COMMAND "${CMAKE_COMMAND}" -E env
				LUMEN_CPU_TRANSLATOR=qemu-tcg $<TARGET_FILE:kyty_emulator> --fex-gpu-backing-self-test)
			set_tests_properties(lumen_ps5_qemu_runtime_contracts
				lumen_ps5_qemu_gpu_backing_publication PROPERTIES TIMEOUT 60)
		endif()
	endif()
endif()
configure_macos_guest_address_space(kyty_emulator)

target_link_libraries(kyty_emulator ${kyty_emulator_link_libraries})
if (WIN32)
	target_link_libraries(kyty_emulator opengl32)
	target_link_libraries(kyty_emulator iphlpapi)
	target_link_libraries(kyty_emulator setupapi)
	target_link_libraries(kyty_emulator ws2_32)
endif()
if (KYTY_CLANG_CL)
	target_link_libraries(kyty_emulator onecore)
endif()
if (CLANG AND NOT KYTY_CLANG_CL)
	target_link_libraries(kyty_emulator pthread)
endif()
# dlopen/dlsym/dladdr for RenderDoc.
target_link_libraries(kyty_emulator ${CMAKE_DL_LIBS})
target_include_directories(kyty_emulator PRIVATE ${inc_headers})

clang_tidy_check(kyty_emulator "" "${check_headers}" "${inc_headers}")

include_what_you_use(kyty_emulator "${inc_headers}")

set(KYTY_EMULATOR_MAP_NAME "kyty_emulator_${KYTY_COMPILER_ID}_${KYTY_LINKER_ID}.map")

set(KYTY_EMULATOR_MAP_LINK_PATH "${CMAKE_CURRENT_BINARY_DIR}/${KYTY_EMULATOR_MAP_NAME}")
set(KYTY_EMULATOR_PDB_LINK_PATH "${CMAKE_CURRENT_BINARY_DIR}/kyty_emulator.pdb")

if(KYTY_CLANG_CL)
	target_link_options(kyty_emulator PRIVATE
		"/DYNAMICBASE:NO"
		"/DEBUG:FULL"
		"/PDB:${KYTY_EMULATOR_PDB_LINK_PATH}"
		"/lldmap:${KYTY_EMULATOR_MAP_LINK_PATH}"
	)
	add_custom_command(TARGET kyty_emulator POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:kyty_emulator>/libwinpthread-1.dll)
elseif(WIN32 OR LINUX)
	set_target_properties(kyty_emulator PROPERTIES LINK_FLAGS "${KYTY_LD_OPTIONS}")
	# LINK_FLAGS is a space-delimited string: a -Map path containing spaces would
	# be split into bogus arguments, so pass it as a discrete link option instead.
	target_link_options(kyty_emulator PRIVATE "-Wl,-Map=${KYTY_EMULATOR_MAP_LINK_PATH}")
endif()

if(LINUX)
    target_link_options(kyty_emulator PRIVATE
        $<$<CONFIG:Release>:-Wl,--strip-all>
    )
endif()

if(APPLE)
	# The emulator writes x86-64 trampolines/PLT stubs into memory and executes them, and
	# runs guest code under Rosetta. That requires the JIT / unsigned-executable-memory
	# entitlements, so re-sign the binary after every link (an unsigned/relinked binary
	# reverts to the hardened defaults and aborts when it first executes written code).
	add_custom_command(TARGET kyty_emulator POST_BUILD
		COMMAND codesign -s - --force --options runtime
			--entitlements "${KYTY_SOURCE_DIR}/macos_jit.entitlements"
			$<TARGET_FILE:kyty_emulator>
		COMMENT "Codesign kyty_emulator with JIT entitlements (macOS)")
endif()

install(TARGETS kyty_emulator DESTINATION .)
# Lumen modification, 2026-10-02: preserve the canonical project and Kyty
# notices in installed runtime trees. This component is not a complete audit
# or source/notice bundle for the executable's third-party link closure.
get_filename_component(LUMEN_LEGAL_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
install(FILES
	"${LUMEN_LEGAL_SOURCE_ROOT}/LICENSE"
	"${LUMEN_LEGAL_SOURCE_ROOT}/NOTICE"
	"${LUMEN_LEGAL_SOURCE_ROOT}/source/licenses/THIRD_PARTY_NOTICES.txt"
	DESTINATION Legal/lumen COMPONENT LumenLegal)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/Kyty-MIT.txt"
	DESTINATION Legal/kyty COMPONENT LumenLegal)
if(KYTY_CLANG_CL)
	install(FILES "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" DESTINATION .)
endif()
