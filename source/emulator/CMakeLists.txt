file(GLOB emulator_src
	src/*.cpp
	src/*.cpp
	src/Libs/*.cpp
	src/Graphics/*.cpp
	src/Graphics/Objects/*.cpp
	src/Kernel/*.cpp
	src/Loader/*.cpp
	include/*.h
)

# The Media NDK translation unit is Android-only. Keep it out of the broad
# source glob on desktop/Apple builds; the pure layout helper is selected with
# it in production and can also be built by its standalone host test.
list(REMOVE_ITEM emulator_src
	"${CMAKE_CURRENT_SOURCE_DIR}/src/AvPlayerAndroid.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/AvPlayerAndroidLayout.cpp"
	# The hot-region planner is a host-only analysis tranche.  Keep the broad
	# source glob from adding it to any shipping emulator until a separately
	# gated regional emitter and runtime contract exist.
	"${CMAKE_CURRENT_SOURCE_DIR}/src/CpuA64HotRegion.cpp"
)

if(ANDROID)
	list(APPEND emulator_src
		src/AvPlayerAndroid.cpp
		src/AvPlayerAndroidLayout.cpp
	)
	if(NOT DEFINED LUMEN_ANDROID_DYNAREC_PROVIDER OR
	   LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "disabled")
		list(REMOVE_ITEM emulator_src "${CMAKE_CURRENT_SOURCE_DIR}/src/CpuA64Dynarec.cpp")
	elseif(LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "aarch64-wx")
		list(REMOVE_ITEM emulator_src "${CMAKE_CURRENT_SOURCE_DIR}/src/CpuA64DynarecDisabled.cpp")
	else()
		message(FATAL_ERROR
			"Unsupported Android dynarec provider: ${LUMEN_ANDROID_DYNAREC_PROVIDER}")
	endif()
elseif(MACOS AND LUMEN_MACOS_X64_BACKEND STREQUAL "fex")
	# The FEX product is compile-time exclusive: retain the ABI-compatible
	# disabled provider but do not compile or link the NODA implementation.
	list(REMOVE_ITEM emulator_src "${CMAKE_CURRENT_SOURCE_DIR}/src/CpuA64Dynarec.cpp")
else()
	list(REMOVE_ITEM emulator_src "${CMAKE_CURRENT_SOURCE_DIR}/src/CpuA64DynarecDisabled.cpp")
endif()

if(LUMEN_WITH_AVPLAYER_ANDROID_LAYOUT_TEST)
	add_executable(lumen_avplayer_android_layout_test EXCLUDE_FROM_ALL
		"${CMAKE_SOURCE_DIR}/AvPlayerAndroidLayoutTest.cpp"
		src/AvPlayerAndroidLayout.cpp
	)
	target_include_directories(lumen_avplayer_android_layout_test PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/include"
	)
	add_test(NAME lumen_avplayer_android_layout
		COMMAND $<TARGET_FILE:lumen_avplayer_android_layout_test>
	)
endif()

if(IOS)
	list(APPEND emulator_src src/Graphics/WindowIOS.mm)
endif()

if(APPLE_PLATFORM)
	find_library(AVFOUNDATION_FRAMEWORK AVFoundation)
	find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
	find_library(COREMEDIA_FRAMEWORK CoreMedia)
	find_library(COREVIDEO_FRAMEWORK CoreVideo)
	find_library(FOUNDATION_FRAMEWORK Foundation)
	if(NOT AVFOUNDATION_FRAMEWORK OR NOT AUDIOTOOLBOX_FRAMEWORK OR NOT COREMEDIA_FRAMEWORK OR
	   NOT COREVIDEO_FRAMEWORK OR NOT FOUNDATION_FRAMEWORK)
		message(FATAL_ERROR "AVFoundation, AudioToolbox, CoreMedia, CoreVideo and Foundation are required for the Apple AvPlayer backend")
	endif()

	add_library(avplayer_mac_backend STATIC src/AvPlayerMac.mm)
	# Reuse the already configured C++ rule and ask Clang to parse this one
	# translation unit as Objective-C++. Enabling a third CMake language from
	# this subdirectory is too late for the project's custom archive rules.
	set_source_files_properties(src/AvPlayerMac.mm PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x objective-c++")
	target_compile_options(avplayer_mac_backend PRIVATE -Wno-deprecated-declarations)
	target_include_directories(avplayer_mac_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
	if(LUMEN_AV_SYNC_TELEMETRY)
		target_compile_definitions(avplayer_mac_backend PRIVATE LUMEN_AV_SYNC_TELEMETRY=1)
	endif()
	target_link_libraries(avplayer_mac_backend PRIVATE
		"${AVFOUNDATION_FRAMEWORK}"
		"${AUDIOTOOLBOX_FRAMEWORK}"
		"${COREMEDIA_FRAMEWORK}"
		"${COREVIDEO_FRAMEWORK}"
		"${FOUNDATION_FRAMEWORK}"
	)

	if(MACOS AND LUMEN_WITH_AVPLAYER_MAC_TEST)
		add_executable(lumen_avplayer_mac_test EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/AvPlayerMacTest.cpp")
		if(LUMEN_AV_SYNC_TELEMETRY)
			target_compile_definitions(lumen_avplayer_mac_test PRIVATE LUMEN_AV_SYNC_TELEMETRY=1)
		endif()
		target_link_libraries(lumen_avplayer_mac_test PRIVATE avplayer_mac_backend)
		add_test(NAME lumen_avplayer_mac
			COMMAND $<TARGET_FILE:lumen_avplayer_mac_test>
				"${CMAKE_SOURCE_DIR}/testdata/avplayer/avplayer_synthetic_av.mov.base64"
				"${CMAKE_SOURCE_DIR}/testdata/avplayer/avplayer_synthetic_video_only.mov.base64"
		)
		set_tests_properties(lumen_avplayer_mac PROPERTIES TIMEOUT 60)
	endif()
endif()

if (MSVC AND CLANG)
	set_source_files_properties(${emulator_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
endif()

# GCC/MinGW can hit an internal compiler error while optimizing the Media
# Foundation helper in Audio.cpp. Keep this translation unit unoptimized; the
# rest of the emulator retains the selected build configuration.
if (MINGW)
	set_source_files_properties(src/Audio.cpp PROPERTIES COMPILE_OPTIONS "-O0")
endif()

#add_library(emulator_obj OBJECT ${emulator_src})
#add_library(emulator STATIC $<TARGET_OBJECTS:emulator_obj>)
add_library(emulator STATIC ${emulator_src})

# This diagnostic shader profile is qualified only in the exact Release
# configuration. NDEBUG alone would also admit RelWithDebInfo/MinSizeRel.
target_compile_definitions(emulator PRIVATE
	"$<$<CONFIG:Release>:LUMEN_V_MAC_LEGACY_F32_RELEASE_BUILD=1>"
	"$<$<CONFIG:Release>:LUMEN_V_RSQ_CLAMP_F32_RELEASE_BUILD=1>")

if(MACOS AND LUMEN_MACOS_X64_BACKEND STREQUAL "fex")
	if(NOT TARGET lumen_macos_fexcore_runtime)
		message(FATAL_ERROR
			"The macOS FEX runtime target must be configured before emulator")
	endif()
	target_compile_definitions(emulator PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	target_link_libraries(emulator lumen_macos_fexcore_runtime)
endif()

if(ANDROID)
	if(LUMEN_ANDROID_DYNAREC_PROVIDER STREQUAL "aarch64-wx")
		target_compile_definitions(emulator PRIVATE
			LUMEN_ANDROID_DYNAREC_PROVIDER_AARCH64_WX=1
			LUMEN_ENABLE_X64_FAST_BLOCKS=1
		)
	else()
		target_compile_definitions(emulator PRIVATE
			LUMEN_ANDROID_DYNAREC_PROVIDER_DISABLED=1
		)
	endif()
	if(LUMEN_ANDROID_DYNAREC_POLICY STREQUAL "selective-noda")
		target_compile_definitions(emulator PRIVATE
			LUMEN_ANDROID_DYNAREC_POLICY_SELECTIVE_NODA=1
		)
	else()
		target_compile_definitions(emulator PRIVATE
			LUMEN_ANDROID_DYNAREC_POLICY_PROBE_ONLY=1
		)
	endif()
endif()

if(LUMEN_A64_REJECTED_FASTBLOCK_PURGE)
	target_compile_definitions(emulator PRIVATE
		LUMEN_A64_REJECTED_FASTBLOCK_PURGE=1)
endif()

if(LUMEN_A64_ARM_HOTPATH_COMPOSITE)
	target_compile_definitions(emulator PRIVATE
		LUMEN_A64_ARM_HOTPATH_COMPOSITE=1)
	set_source_files_properties(
		src/CpuX64.cpp
		src/CpuA64Dynarec.cpp
		PROPERTIES COMPILE_OPTIONS "-fomit-frame-pointer"
	)
endif()

if(LUMEN_A64_ARM_LOADING_COMPOSITE)
	target_compile_definitions(emulator PRIVATE
		LUMEN_A64_ARM_LOADING_COMPOSITE=1)
endif()

if(LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT)
	target_compile_definitions(emulator PRIVATE LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT=1)
else()
	target_compile_definitions(emulator PRIVATE LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT=0)
endif()

if(LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT)
	target_compile_definitions(emulator PRIVATE LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT=1)
else()
	target_compile_definitions(emulator PRIVATE LUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT=0)
endif()

if(LUMEN_APPLE_NATIVE_METAL)
	if(NOT TARGET vulkan-1)
		message(FATAL_ERROR "The native Metal adapter target must exist before the emulator")
	endif()
	get_target_property(LUMEN_APPLE_NATIVE_METAL_TARGET_TYPE vulkan-1 TYPE)
	if(NOT LUMEN_APPLE_NATIVE_METAL_TARGET_TYPE STREQUAL "STATIC_LIBRARY")
		message(FATAL_ERROR "The native Metal adapter must provide vulkan-1 as a static library")
	endif()
	# MetalVulkanCore.mm and MetalVulkanCommands.mm belong to vulkan-1, but
	# their language/ownership contract is part of the emulator graph. Keep ARC
	# explicit here as well as at target creation so future source-list moves do
	# not silently compile either translation unit with manual retain/release.
	set_target_properties(vulkan-1 PROPERTIES
		OBJCXX_STANDARD 17
		OBJCXX_STANDARD_REQUIRED ON
		OBJCXX_EXTENSIONS OFF
	)
	target_compile_options(vulkan-1 PRIVATE
		"$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
	)
endif()

if(LUMEN_FAST_BLOCK_TELEMETRY)
	target_compile_definitions(emulator PRIVATE LUMEN_FAST_BLOCK_TELEMETRY=1)
endif()

if(LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY)
	target_compile_definitions(emulator PRIVATE
		LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY=1)
endif()

if(LUMEN_A64_DYNAREC_STARTUP_TELEMETRY)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_STARTUP_TELEMETRY=1)
endif()

if(LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_PHASE6AF_TELEMETRY=1)
endif()

if(LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_PHASE6AL_TELEMETRY=1)
endif()

if(LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_CROSS_OWNER_TELEMETRY=1)
endif()

if(LUMEN_A64_DYNAREC_RFLAGS_PROFILE)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_RFLAGS_PROFILE=1)
endif()

if(LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_GPR_CACHE_PROFILE=1)
endif()

if(LUMEN_A64_DYNAREC_PHASE6CK_PROFILE)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_PHASE6CK_PROFILE=1)
endif()

if(LUMEN_A64_DYNAREC_PHASE6CM_PROFILE)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_PHASE6CM_PROFILE=1)
endif()

if(LUMEN_A64_DYNAREC_PHASE6CO_PROFILE)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_PHASE6CO_PROFILE=1)
endif()

if(LUMEN_A64_DYNAREC_HOST_PC_PROFILE)
	target_compile_definitions(emulator PRIVATE LUMEN_A64_DYNAREC_HOST_PC_PROFILE=1)
endif()

if(LUMEN_WITH_RUNTIME_LIFECYCLE_TEST)
	target_compile_definitions(emulator PRIVATE LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK=1)
endif()

if(APPLE_PLATFORM)
	# The guest ABI uses fixed-width PS4 types while Darwin maps size_t and
	# uintptr_t to unsigned long; legacy diagnostic printf calls are not
	# semantically portable yet.
	target_compile_options(emulator PRIVATE -Wno-format)
endif()

# Reuse the common GPL text editor/lifecycle strictly as a host model. The
# PS4 HLE adapter owns all guest access and never passes guest pointers or
# guest callbacks to these functions. This does not link the PS5 engine.
include("${CMAKE_CURRENT_SOURCE_DIR}/../gen4/gen5/cmake/CompatibilitySources.cmake")
lumen_gen5_prepare_compatibility_sources(lumen_ime_host_root)
set(lumen_ime_host_source "${lumen_ime_host_root}/src")
add_library(lumen_ime_dialog_host STATIC
	"${lumen_ime_host_source}/libs/imeCommon.cpp"
	"${lumen_ime_host_source}/libs/imeDialog.cpp"
)
target_include_directories(lumen_ime_dialog_host PUBLIC "${lumen_ime_host_source}")
target_compile_definitions(lumen_ime_dialog_host PUBLIC KYTY_IME_HOST_ADAPTER=1)
target_link_libraries(emulator lumen_ime_dialog_host)

if(NOT ANDROID AND NOT IOS)
	# The host text panel uses a separate software-rendered SDL2 window. Keep
	# its MIT renderer independent of the guest Vulkan/NativeMetal surface.
	set(lumen_ime_imgui_source "${CMAKE_CURRENT_SOURCE_DIR}/../gen4/gen5/compatibility/3rdparty/imgui")
	add_library(lumen_ime_dialog_ui STATIC
		"${CMAKE_CURRENT_SOURCE_DIR}/../shared/ime/KeyboardPanel.cpp"
		"${lumen_ime_imgui_source}/imgui.cpp"
		"${lumen_ime_imgui_source}/imgui_draw.cpp"
		"${lumen_ime_imgui_source}/imgui_tables.cpp"
		"${lumen_ime_imgui_source}/imgui_widgets.cpp"
		"${lumen_ime_imgui_source}/backends/imgui_impl_sdl2.cpp"
		"${lumen_ime_imgui_source}/backends/imgui_impl_sdlrenderer2.cpp"
	)
	target_include_directories(lumen_ime_dialog_ui PUBLIC "${lumen_ime_imgui_source}" "${lumen_ime_imgui_source}/backends"
		"${CMAKE_CURRENT_SOURCE_DIR}/../shared" "${lumen_ime_host_source}")
	target_compile_definitions(lumen_ime_dialog_ui PRIVATE KYTY_IME_HOST_ADAPTER=1)
	target_link_libraries(lumen_ime_dialog_ui PUBLIC SDL2-static)
	target_link_libraries(emulator lumen_ime_dialog_ui)
endif()

target_link_libraries(emulator core math scripts lua spirv-tools-opt spirv-tools easy_profiler SDL2-static Lumen::LibAtrac9)
if(ANDROID)
	# SDL and every PS4 consumer share one selected loader. Direct libvulkan
	# imports would mix stock dispatch with custom-driver instances/devices.
	target_compile_definitions(emulator PUBLIC LUMEN_VULKAN_DYNAMIC_DISPATCH=1 VK_NO_PROTOTYPES=1)
else()
	target_link_libraries(emulator vulkan-1)
endif()
if(NOT IOS AND NOT ANDROID)
	target_link_libraries(emulator unit_test)
endif()
if(APPLE_PLATFORM)
	target_link_libraries(emulator avplayer_mac_backend)
endif()

target_include_directories(emulator PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/../cpu/runtime")

get_property(inc_headers TARGET emulator PROPERTY INCLUDE_DIRECTORIES)

list(APPEND inc_headers
	${PROJECT_SOURCE_DIR}/3rdparty/sdl2/sdl2/include
	${PROJECT_SOURCE_DIR}/3rdparty/vulkan/include
	${PROJECT_SOURCE_DIR}/3rdparty/easy_profiler/include
	${PROJECT_SOURCE_DIR}/3rdparty/xxhash/include
	${PROJECT_SOURCE_DIR}/3rdparty/cpuinfo/include
	${PROJECT_SOURCE_DIR}/3rdparty/stb
)

if (MSVC AND CLANG)
	target_link_libraries(emulator winpthread)
	list(APPEND inc_headers 
		${PROJECT_SOURCE_DIR}/3rdparty/winpthread/include
	)
endif()

#target_include_directories(emulator_obj PRIVATE ${inc_headers})
target_include_directories(emulator PRIVATE ${inc_headers})

list(APPEND check_headers
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${PROJECT_SOURCE_DIR}/include
)

#clang_tidy_check(emulator_obj "" "${check_headers}" "${inc_headers}")
clang_tidy_check(emulator "" "${check_headers}" "${inc_headers}")

#include_what_you_use(emulator_obj "${inc_headers}")
include_what_you_use(emulator "${inc_headers}")
