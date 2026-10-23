if(LUMEN_APPLE_NATIVE_METAL)
	# LUMEN_APPLE_NATIVE_METAL_CANDIDATE_BEGIN
	set(LUMEN_METAL_VULKAN_SOURCE_DIR
		"${PROJECT_SOURCE_DIR}/emulator/src/Graphics/Metal")
	set(LUMEN_METAL_VULKAN_SOURCES
		"${LUMEN_METAL_VULKAN_SOURCE_DIR}/MetalVulkanCore.mm"
		"${LUMEN_METAL_VULKAN_SOURCE_DIR}/MetalVulkanCommands.mm"
	)
	foreach(LUMEN_METAL_VULKAN_SOURCE IN LISTS LUMEN_METAL_VULKAN_SOURCES)
		if(NOT EXISTS "${LUMEN_METAL_VULKAN_SOURCE}")
			message(FATAL_ERROR
				"The native Metal adapter source is missing: ${LUMEN_METAL_VULKAN_SOURCE}")
		endif()
	endforeach()

	include("${PROJECT_SOURCE_DIR}/3rdparty/spirv_cross_msl.cmake")
	if(NOT TARGET Lumen::SpirvCrossMsl)
		message(FATAL_ERROR "The native Metal adapter requires Lumen::SpirvCrossMsl")
	endif()

	add_library(vulkan-1 STATIC ${LUMEN_METAL_VULKAN_SOURCES})
	set_target_properties(vulkan-1 PROPERTIES
		POSITION_INDEPENDENT_CODE ON
		OBJCXX_STANDARD 17
		OBJCXX_STANDARD_REQUIRED ON
		OBJCXX_EXTENSIONS OFF
	)
	# The local CompilerMSL subclass must use the same no-RTTI ABI as the
	# SPIRV-Cross C++ targets; ObjC++ does not inherit CMAKE_CXX_FLAGS.
	set_property(SOURCE "${LUMEN_METAL_VULKAN_SOURCE_DIR}/MetalVulkanCommands.mm"
		APPEND PROPERTY COMPILE_OPTIONS -fno-rtti)
	target_compile_definitions(vulkan-1 PRIVATE LUMEN_APPLE_NATIVE_METAL=1)
	target_compile_options(vulkan-1 PRIVATE
		"$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
		# The pinned permissive upstream headers contain pre-C++20 unqualified
		# move() calls. Keep this compatibility warning scoped to the adapter TU
		# that includes spirv_msl.hpp; project warnings remain errors elsewhere.
		"$<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-unqualified-std-cast-call>"
	)
	target_include_directories(vulkan-1
		PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
		PRIVATE "${LUMEN_METAL_VULKAN_SOURCE_DIR}"
	)
	get_filename_component(_lumen_modern_vk_headers
		"${CMAKE_CURRENT_SOURCE_DIR}/../../../../lumen-deps/gen4/sources/vulkan-headers/include" ABSOLUTE)
	set(LUMEN_METAL_VULKAN_HEADERS_DIR "${_lumen_modern_vk_headers}" CACHE PATH
		"Pinned Vulkan headers for the native Metal contract")
	if(NOT EXISTS "${LUMEN_METAL_VULKAN_HEADERS_DIR}/vulkan/vulkan_core.h")
		message(FATAL_ERROR "Prepare the pinned headers with source/gen4/tools/prepare_dependencies.py, or set LUMEN_METAL_VULKAN_HEADERS_DIR")
	endif()
	target_include_directories(vulkan-1 BEFORE PUBLIC "${LUMEN_METAL_VULKAN_HEADERS_DIR}")

	set(LUMEN_APPLE_NATIVE_METAL_FRAMEWORKS)
	foreach(LUMEN_APPLE_NATIVE_METAL_FRAMEWORK IN ITEMS Metal QuartzCore Foundation)
		string(TOUPPER "${LUMEN_APPLE_NATIVE_METAL_FRAMEWORK}"
			LUMEN_APPLE_NATIVE_METAL_FRAMEWORK_ID)
		set(LUMEN_APPLE_NATIVE_METAL_FRAMEWORK_VAR
			"LUMEN_APPLE_NATIVE_METAL_${LUMEN_APPLE_NATIVE_METAL_FRAMEWORK_ID}_FRAMEWORK")
		find_library(${LUMEN_APPLE_NATIVE_METAL_FRAMEWORK_VAR}
			NAMES ${LUMEN_APPLE_NATIVE_METAL_FRAMEWORK})
		if(NOT ${LUMEN_APPLE_NATIVE_METAL_FRAMEWORK_VAR})
			message(FATAL_ERROR
				"${LUMEN_APPLE_NATIVE_METAL_FRAMEWORK} is required by the native Metal adapter")
		endif()
		list(APPEND LUMEN_APPLE_NATIVE_METAL_FRAMEWORKS
			"${${LUMEN_APPLE_NATIVE_METAL_FRAMEWORK_VAR}}")
		mark_as_advanced(${LUMEN_APPLE_NATIVE_METAL_FRAMEWORK_VAR})
	endforeach()

	if(MACOS)
		set(LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK AppKit)
	else()
		set(LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK UIKit)
	endif()
	find_library(LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK_PATH
		NAMES ${LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK})
	if(NOT LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK_PATH)
		message(FATAL_ERROR
			"${LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK} is required by the native Metal adapter")
	endif()
	list(APPEND LUMEN_APPLE_NATIVE_METAL_FRAMEWORKS
		"${LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK_PATH}")
	mark_as_advanced(LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK_PATH)

	target_link_libraries(vulkan-1 PRIVATE
		Lumen::SpirvCrossMsl
	)
	target_link_libraries(vulkan-1 PUBLIC
		${LUMEN_APPLE_NATIVE_METAL_FRAMEWORKS}
	)
	# SDL's Apple Vulkan window glue resolves the statically linked adapter with
	# dlsym(RTLD_DEFAULT). Force the defining object out of the archive and keep
	# exactly this internal compatibility entry point in the export trie.
	target_link_options(vulkan-1 INTERFACE
		"-Wl,-u,_vkGetInstanceProcAddr"
		"-Wl,-exported_symbol,_vkGetInstanceProcAddr"
	)
	message(STATUS
		"Vulkan contract on Apple: native Metal static adapter (no portability loader)")
	# LUMEN_APPLE_NATIVE_METAL_CANDIDATE_END
else()
	set(LUMEN_VULKAN_IMPORTED_TYPE SHARED)

if(MACOS)
	# Respect an explicit/cache Vulkan_Lib value.  For a fresh configuration,
	# prefer MoltenVK's static archive so fc_script has no package-manager
	# runtime dependency; fall back to the existing loader/dylib search when a
	# static archive is unavailable.
	if(NOT Vulkan_Lib)
		set(LUMEN_SAVED_FIND_LIBRARY_SUFFIXES "${CMAKE_FIND_LIBRARY_SUFFIXES}")
		set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
		find_library(LUMEN_MOLTENVK_STATIC_LIBRARY NAMES MoltenVK)
		set(CMAKE_FIND_LIBRARY_SUFFIXES "${LUMEN_SAVED_FIND_LIBRARY_SUFFIXES}")

		if(LUMEN_MOLTENVK_STATIC_LIBRARY)
			set(Vulkan_Lib "${LUMEN_MOLTENVK_STATIC_LIBRARY}" CACHE FILEPATH "Vulkan/MoltenVK library" FORCE)
		else()
			find_library(Vulkan_Lib NAMES vulkan MoltenVK)
		endif()
	endif()

	if(NOT Vulkan_Lib)
		message(FATAL_ERROR "Vulkan loader not found on macOS. Install the Vulkan SDK/MoltenVK and set Vulkan_Lib if needed.")
	endif()

	get_filename_component(LUMEN_VULKAN_LIBRARY_EXTENSION "${Vulkan_Lib}" EXT)
	if(LUMEN_VULKAN_LIBRARY_EXTENSION STREQUAL ".a")
		set(LUMEN_VULKAN_IMPORTED_TYPE STATIC)
	endif()
elseif(IOS)
	set(LUMEN_IOS_MOLTENVK_ROOT "" CACHE PATH
		"Root of an extracted official MoltenVK iOS package, or its static MoltenVK.xcframework")
	if(NOT LUMEN_IOS_MOLTENVK_ROOT)
		message(FATAL_ERROR
			"An official static MoltenVK package is required for iOS. "
			"Run _Build/fetch_MoltenVK_iOS.sh device, then configure with "
			"-DLUMEN_IOS_MOLTENVK_ROOT=<extracted-root>.")
	endif()

	get_filename_component(LUMEN_IOS_MOLTENVK_SEARCH_ROOT
		"${LUMEN_IOS_MOLTENVK_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_BINARY_DIR}")
	if(NOT IS_DIRECTORY "${LUMEN_IOS_MOLTENVK_SEARCH_ROOT}")
		message(FATAL_ERROR
			"LUMEN_IOS_MOLTENVK_ROOT is not a directory: ${LUMEN_IOS_MOLTENVK_SEARCH_ROOT}")
	endif()
	get_filename_component(LUMEN_IOS_MOLTENVK_SEARCH_ROOT
		"${LUMEN_IOS_MOLTENVK_SEARCH_ROOT}" REALPATH)

	string(TOLOWER "${CMAKE_OSX_SYSROOT}" LUMEN_IOS_MOLTENVK_SYSROOT_LOWER)
	if(LUMEN_IOS_MOLTENVK_SYSROOT_LOWER MATCHES "iphonesimulator")
		set(LUMEN_IOS_MOLTENVK_PLATFORM "iOS Simulator")
		set(LUMEN_IOS_MOLTENVK_SLICE_REGEX
			"/MoltenVK\\.xcframework/ios-[^/]*arm64[^/]*-simulator/libMoltenVK\\.a$")
	elseif(LUMEN_IOS_MOLTENVK_SYSROOT_LOWER MATCHES "iphoneos")
		set(LUMEN_IOS_MOLTENVK_PLATFORM "iOS device")
		set(LUMEN_IOS_MOLTENVK_SLICE_REGEX
			"/MoltenVK\\.xcframework/ios-arm64/libMoltenVK\\.a$")
	else()
		message(FATAL_ERROR
			"Unsupported iOS sysroot for MoltenVK: ${CMAKE_OSX_SYSROOT}. "
			"Use iphoneos or iphonesimulator.")
	endif()

	# The official archives may be passed either at their extraction root or as
	# the static XCFramework itself. Search only for the exact platform archive;
	# this deliberately ignores the dynamic XCFramework shipped beside it.
	file(GLOB_RECURSE LUMEN_IOS_MOLTENVK_ARCHIVE_CANDIDATES
		LIST_DIRECTORIES false
		"${LUMEN_IOS_MOLTENVK_SEARCH_ROOT}/*.a")
	set(LUMEN_IOS_MOLTENVK_MATCHES)
	foreach(LUMEN_IOS_MOLTENVK_ARCHIVE IN LISTS LUMEN_IOS_MOLTENVK_ARCHIVE_CANDIDATES)
		if(LUMEN_IOS_MOLTENVK_ARCHIVE MATCHES "${LUMEN_IOS_MOLTENVK_SLICE_REGEX}")
			list(APPEND LUMEN_IOS_MOLTENVK_MATCHES "${LUMEN_IOS_MOLTENVK_ARCHIVE}")
		endif()
	endforeach()
	list(REMOVE_DUPLICATES LUMEN_IOS_MOLTENVK_MATCHES)
	list(LENGTH LUMEN_IOS_MOLTENVK_MATCHES LUMEN_IOS_MOLTENVK_MATCH_COUNT)
	if(LUMEN_IOS_MOLTENVK_MATCH_COUNT EQUAL 0)
		message(FATAL_ERROR
			"No static ${LUMEN_IOS_MOLTENVK_PLATFORM} arm64 slice was found below "
			"${LUMEN_IOS_MOLTENVK_SEARCH_ROOT}. Pass the official package root or its "
			"static MoltenVK.xcframework. Simulator builds require a package that "
			"contains an ios-*-simulator slice (for example MoltenVK-all.tar).")
	elseif(LUMEN_IOS_MOLTENVK_MATCH_COUNT GREATER 1)
		string(JOIN "\n  " LUMEN_IOS_MOLTENVK_MATCH_LIST ${LUMEN_IOS_MOLTENVK_MATCHES})
		message(FATAL_ERROR
			"LUMEN_IOS_MOLTENVK_ROOT contains multiple matching static slices. "
			"Pass the intended MoltenVK.xcframework directly:\n  ${LUMEN_IOS_MOLTENVK_MATCH_LIST}")
	endif()
	list(GET LUMEN_IOS_MOLTENVK_MATCHES 0 LUMEN_IOS_MOLTENVK_STATIC_LIBRARY)
	set(LUMEN_VULKAN_IMPORTED_TYPE STATIC)
endif()

add_library(vulkan-1 ${LUMEN_VULKAN_IMPORTED_TYPE} IMPORTED GLOBAL)

if (MINGW AND NOT CLANG)
	set_target_properties(vulkan-1 PROPERTIES IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/lib/vulkan-1.lib")
elseif(MINGW AND CLANG)
	set_target_properties(vulkan-1 PROPERTIES IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/lib/vulkan-1.lib")
elseif(MSVC)
	set_target_properties(vulkan-1 PROPERTIES IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/lib/vulkan-1.lib")
elseif(LINUX)
	find_library(Vulkan_Lib vulkan)
    set_target_properties(vulkan-1 PROPERTIES IMPORTED_LOCATION ${Vulkan_Lib})
elseif(ANDROID)
	find_library(Vulkan_Lib NAMES vulkan)
	if(NOT Vulkan_Lib)
		message(FATAL_ERROR "Android NDK Vulkan loader not found")
	endif()
	set_target_properties(vulkan-1 PROPERTIES IMPORTED_LOCATION ${Vulkan_Lib})
elseif(MACOS)
	set_target_properties(vulkan-1 PROPERTIES IMPORTED_LOCATION ${Vulkan_Lib})

	get_filename_component(LUMEN_VULKAN_LIBRARY_NAME "${Vulkan_Lib}" NAME)
	if(LUMEN_VULKAN_LIBRARY_NAME STREQUAL "libMoltenVK.a")
		set(LUMEN_MOLTENVK_FRAMEWORKS)
		foreach(LUMEN_MOLTENVK_FRAMEWORK IN ITEMS Metal Foundation QuartzCore CoreGraphics IOSurface IOKit AppKit)
			string(TOUPPER "${LUMEN_MOLTENVK_FRAMEWORK}" LUMEN_MOLTENVK_FRAMEWORK_ID)
			set(LUMEN_MOLTENVK_FRAMEWORK_VAR "LUMEN_MOLTENVK_${LUMEN_MOLTENVK_FRAMEWORK_ID}_FRAMEWORK")
			find_library(${LUMEN_MOLTENVK_FRAMEWORK_VAR} NAMES ${LUMEN_MOLTENVK_FRAMEWORK})
			set(LUMEN_MOLTENVK_FRAMEWORK_PATH "${${LUMEN_MOLTENVK_FRAMEWORK_VAR}}")
			if(NOT LUMEN_MOLTENVK_FRAMEWORK_PATH)
				message(FATAL_ERROR "${LUMEN_MOLTENVK_FRAMEWORK} is required by static MoltenVK on macOS")
			endif()
			list(APPEND LUMEN_MOLTENVK_FRAMEWORKS "${LUMEN_MOLTENVK_FRAMEWORK_PATH}")
			mark_as_advanced(${LUMEN_MOLTENVK_FRAMEWORK_VAR})
		endforeach()
		target_link_libraries(vulkan-1 INTERFACE ${LUMEN_MOLTENVK_FRAMEWORKS})
		message(STATUS "Vulkan on macOS: static MoltenVK (${Vulkan_Lib})")
	else()
		message(STATUS "Vulkan on macOS: dynamic loader override (${Vulkan_Lib})")
	endif()
elseif(IOS)
	set_target_properties(vulkan-1 PROPERTIES
		IMPORTED_LOCATION "${LUMEN_IOS_MOLTENVK_STATIC_LIBRARY}")

	set(LUMEN_IOS_MOLTENVK_FRAMEWORKS)
	foreach(LUMEN_IOS_MOLTENVK_FRAMEWORK IN ITEMS Metal Foundation QuartzCore CoreGraphics IOSurface UIKit)
		string(TOUPPER "${LUMEN_IOS_MOLTENVK_FRAMEWORK}" LUMEN_IOS_MOLTENVK_FRAMEWORK_ID)
		set(LUMEN_IOS_MOLTENVK_FRAMEWORK_VAR
			"LUMEN_IOS_MOLTENVK_${LUMEN_IOS_MOLTENVK_FRAMEWORK_ID}_FRAMEWORK")
		find_library(${LUMEN_IOS_MOLTENVK_FRAMEWORK_VAR}
			NAMES ${LUMEN_IOS_MOLTENVK_FRAMEWORK})
		set(LUMEN_IOS_MOLTENVK_FRAMEWORK_PATH
			"${${LUMEN_IOS_MOLTENVK_FRAMEWORK_VAR}}")
		if(NOT LUMEN_IOS_MOLTENVK_FRAMEWORK_PATH)
			message(FATAL_ERROR
				"${LUMEN_IOS_MOLTENVK_FRAMEWORK} is required by static MoltenVK on iOS")
		endif()
		list(APPEND LUMEN_IOS_MOLTENVK_FRAMEWORKS
			"${LUMEN_IOS_MOLTENVK_FRAMEWORK_PATH}")
		mark_as_advanced(${LUMEN_IOS_MOLTENVK_FRAMEWORK_VAR})
	endforeach()
	target_link_libraries(vulkan-1 INTERFACE ${LUMEN_IOS_MOLTENVK_FRAMEWORKS})
	target_link_options(vulkan-1 INTERFACE "-ObjC")
	message(STATUS
		"Vulkan on ${LUMEN_IOS_MOLTENVK_PLATFORM}: static MoltenVK (${LUMEN_IOS_MOLTENVK_STATIC_LIBRARY})")
endif()
endif() # LUMEN_APPLE_NATIVE_METAL

set(SPIRV_SOURCES

  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/bitutils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/bit_vector.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/hex_float.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/make_unique.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/parse_number.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/small_vector.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/string_utils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/timer.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/assembly_grammar.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/binary.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/cfa.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/common_debug_info.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/diagnostic.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/disassemble.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/enum_set.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/enum_string_mapping.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/ext_inst.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/extensions.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/instruction.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/latest_version_glsl_std_450_header.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/latest_version_opencl_std_header.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/latest_version_spirv_header.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/macro.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/name_mapper.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opcode.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/operand.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/parsed_operand.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/print.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_constant.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_definition.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_endian.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_fuzzer_options.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_optimizer_options.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_reducer_options.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_target_env.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_validator_options.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/table.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/text.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/text_handler.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate.h
							  
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/bit_vector.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/parse_number.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/util/string_utils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/assembly_grammar.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/binary.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/diagnostic.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/disassemble.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/enum_string_mapping.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/ext_inst.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/extensions.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/libspirv.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/name_mapper.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opcode.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/operand.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/parsed_operand.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/print.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/software_version.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_endian.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_fuzzer_options.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_optimizer_options.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_reducer_options.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_target_env.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/spirv_validator_options.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/table.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/text.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/text_handler.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_adjacency.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_annotation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_arithmetics.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_atomics.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_barriers.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_bitwise.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_builtins.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_capability.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_cfg.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_composites.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_constants.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_conversion.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_debug.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_decorations.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_derivatives.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_extensions.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_execution_limitations.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_function.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_id.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_image.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_interfaces.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_instruction.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_layout.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_literals.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_logicals.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_memory.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_memory_semantics.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_misc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_mode_setting.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_non_uniform.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_primitives.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_scopes.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_small_type_uses.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validate_type.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/decoration.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/basic_block.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/construct.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/function.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/instruction.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/val/validation_state.cpp
)
  
set(SPIRV_TOOLS_OPT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/aggressive_dead_code_elim_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/amd_ext_to_khr.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/basic_block.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/block_merge_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/block_merge_util.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/build_module.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ccp_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/cfg_cleanup_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/cfg.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/code_sink.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/combine_access_chains.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/compact_ids_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/composite.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/const_folding_rules.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/constants.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/control_dependence.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/convert_to_half_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/copy_prop_arrays.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dead_branch_elim_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dead_insert_elim_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dead_variable_elimination.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/decoration_manager.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/debug_info_manager.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/def_use_manager.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/desc_sroa.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dominator_analysis.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dominator_tree.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/eliminate_dead_constant_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/eliminate_dead_functions_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/eliminate_dead_functions_util.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/eliminate_dead_members_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/empty_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/feature_manager.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/fix_storage_class.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/flatten_decoration_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/fold.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/folding_rules.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/fold_spec_constant_op_and_composite_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/freeze_spec_constant_value_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/function.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/graphics_robust_access_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/if_conversion.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inline_exhaustive_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inline_opaque_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inline_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inst_bindless_check_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inst_buff_addr_check_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inst_debug_printf_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/instruction.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/instruction_list.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/instrument_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/interp_fixup_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ir_builder.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ir_context.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ir_loader.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/licm_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/local_access_chain_convert_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/local_redundancy_elimination.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/local_single_block_elim_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/local_single_store_elim_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/log.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_dependence.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_descriptor.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_fission.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_fusion.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_fusion_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_peeling.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_unroller.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_utils.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_unswitch_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/mem_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/merge_return_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/module.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/null_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/passes.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/pass_manager.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/private_to_local_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/propagator.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/reduce_load_size.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/redundancy_elimination.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/reflect.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/register_pressure.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/relax_float_ops_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/remove_duplicates_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/remove_unused_interface_variables_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/replace_invalid_opc.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/scalar_analysis.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/scalar_analysis_nodes.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/scalar_replacement_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/set_spec_constant_default_value_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/simplification_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ssa_rewrite_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/strength_reduction_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/strip_debug_info_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/strip_reflect_info_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/struct_cfg_analysis.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/tree_iterator.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/type_manager.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/types.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/unify_const_pass.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/upgrade_memory_model.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/value_number_table.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/vector_dce.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/workaround1209.h
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/wrap_opkill.h

  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/aggressive_dead_code_elim_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/amd_ext_to_khr.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/basic_block.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/block_merge_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/block_merge_util.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/build_module.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ccp_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/cfg_cleanup_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/cfg.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/code_sink.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/combine_access_chains.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/compact_ids_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/composite.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/const_folding_rules.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/constants.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/control_dependence.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/convert_to_half_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/copy_prop_arrays.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dead_branch_elim_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dead_insert_elim_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dead_variable_elimination.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/decoration_manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/debug_info_manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/def_use_manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/desc_sroa.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dominator_analysis.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/dominator_tree.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/eliminate_dead_constant_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/eliminate_dead_functions_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/eliminate_dead_functions_util.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/eliminate_dead_members_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/feature_manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/fix_storage_class.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/flatten_decoration_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/fold.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/folding_rules.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/fold_spec_constant_op_and_composite_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/freeze_spec_constant_value_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/function.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/graphics_robust_access_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/if_conversion.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inline_exhaustive_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inline_opaque_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inline_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inst_bindless_check_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inst_buff_addr_check_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/inst_debug_printf_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/instruction.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/instruction_list.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/instrument_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/interp_fixup_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ir_context.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ir_loader.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/licm_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/local_access_chain_convert_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/local_redundancy_elimination.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/local_single_block_elim_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/local_single_store_elim_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_dependence.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_dependence_helpers.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_descriptor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_fission.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_fusion.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_fusion_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_peeling.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_utils.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_unroller.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/loop_unswitch_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/mem_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/merge_return_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/module.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/optimizer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/pass_manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/private_to_local_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/propagator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/reduce_load_size.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/redundancy_elimination.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/register_pressure.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/relax_float_ops_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/remove_duplicates_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/remove_unused_interface_variables_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/replace_invalid_opc.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/scalar_analysis.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/scalar_analysis_simplification.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/scalar_replacement_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/set_spec_constant_default_value_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/simplification_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/ssa_rewrite_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/strength_reduction_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/strip_debug_info_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/strip_reflect_info_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/struct_cfg_analysis.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/type_manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/types.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/unify_const_pass.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/upgrade_memory_model.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/value_number_table.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/vector_dce.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/workaround1209.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/source/opt/wrap_opkill.cpp
)

if (IOS)
	set_source_files_properties(${SPIRV_SOURCES} PROPERTIES COMPILE_FLAGS "-Os -DSPIRV_IOS -Wno-unqualified-std-cast-call -Wno-deprecated-declarations")
	set_source_files_properties(${SPIRV_TOOLS_OPT_SOURCES} PROPERTIES COMPILE_FLAGS "-Os -DSPIRV_IOS -Wno-unqualified-std-cast-call -Wno-deprecated-declarations")
elseif (ANDROID)
	set_source_files_properties(${SPIRV_SOURCES} PROPERTIES COMPILE_FLAGS "-Os -DSPIRV_ANDROID -Wno-unqualified-std-cast-call -Wno-deprecated-declarations")
	set_source_files_properties(${SPIRV_TOOLS_OPT_SOURCES} PROPERTIES COMPILE_FLAGS "-Os -DSPIRV_ANDROID -Wno-unqualified-std-cast-call -Wno-deprecated-declarations")
elseif (LINUX OR MACOS)
	set_source_files_properties(${SPIRV_SOURCES} PROPERTIES COMPILE_FLAGS "-Os -DSPIRV_LINUX -Wno-unqualified-std-cast-call -Wno-deprecated-declarations")
	set_source_files_properties(${SPIRV_TOOLS_OPT_SOURCES} PROPERTIES COMPILE_FLAGS "-Os -DSPIRV_LINUX -Wno-unqualified-std-cast-call -Wno-deprecated-declarations")
elseif (MSVC)
    if (CLANG)
        set_source_files_properties(${SPIRV_SOURCES} PROPERTIES COMPILE_FLAGS "/Os -D_CRT_SECURE_NO_WARNINGS -DSPIRV_WINDOWS -Wno-unqualified-std-cast-call")
        set_source_files_properties(${SPIRV_TOOLS_OPT_SOURCES} PROPERTIES COMPILE_FLAGS "/Os -D_CRT_SECURE_NO_WARNINGS -DSPIRV_WINDOWS -Wno-unqualified-std-cast-call")
    else()
        set_source_files_properties(${SPIRV_SOURCES} PROPERTIES COMPILE_FLAGS "/Os -D_CRT_SECURE_NO_WARNINGS -DSPIRV_WINDOWS")
        set_source_files_properties(${SPIRV_TOOLS_OPT_SOURCES} PROPERTIES COMPILE_FLAGS "/Os -D_CRT_SECURE_NO_WARNINGS -DSPIRV_WINDOWS")
    endif()
else()
    set_source_files_properties(${SPIRV_SOURCES} PROPERTIES COMPILE_FLAGS "-Os -DSPIRV_WINDOWS")
    set_source_files_properties(${SPIRV_TOOLS_OPT_SOURCES} PROPERTIES COMPILE_FLAGS "-Os -DSPIRV_WINDOWS")
endif()


add_library(spirv-tools_obj OBJECT ${SPIRV_SOURCES})
add_static_library_from_objects(spirv-tools spirv-tools_obj)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/source)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/source/gen)

target_include_directories(spirv-tools PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

get_property(dirs TARGET spirv-tools PROPERTY INCLUDE_DIRECTORIES)
target_include_directories(spirv-tools_obj PRIVATE ${dirs})

add_library(spirv-tools-opt_obj OBJECT ${SPIRV_TOOLS_OPT_SOURCES})
add_static_library_from_objects(spirv-tools-opt spirv-tools-opt_obj)

target_include_directories(spirv-tools-opt PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

get_property(dirs-opt TARGET spirv-tools-opt PROPERTY INCLUDE_DIRECTORIES)
target_include_directories(spirv-tools-opt_obj PRIVATE ${dirs-opt})
