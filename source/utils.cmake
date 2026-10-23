
function(remove_generator_macros t m)
	while(1)
		list(FIND CMAKE_EXTRA_GENERATOR_${t}_SYSTEM_DEFINED_MACROS ${m} GEN_MACRO_INDEX)
		if( ${GEN_MACRO_INDEX} GREATER -1 )
			message("remove ${t} ${m} ${GEN_MACRO_INDEX}")
			list(REMOVE_AT CMAKE_EXTRA_GENERATOR_${t}_SYSTEM_DEFINED_MACROS ${GEN_MACRO_INDEX})
			list(REMOVE_AT CMAKE_EXTRA_GENERATOR_${t}_SYSTEM_DEFINED_MACROS ${GEN_MACRO_INDEX})
			SET(CMAKE_EXTRA_GENERATOR_${t}_SYSTEM_DEFINED_MACROS ${CMAKE_EXTRA_GENERATOR_${t}_SYSTEM_DEFINED_MACROS} CACHE INTERNAL "")
		else()
			break()
		endif()				
	endwhile()
endfunction()	

function(add_generator_macros t m v)
	list(APPEND CMAKE_EXTRA_GENERATOR_${t}_SYSTEM_DEFINED_MACROS ${m} ${v})
	SET(CMAKE_EXTRA_GENERATOR_${t}_SYSTEM_DEFINED_MACROS ${CMAKE_EXTRA_GENERATOR_${t}_SYSTEM_DEFINED_MACROS} CACHE INTERNAL "")
endfunction()	

function(include_what_you_use target dirs)
  if (NOT LINUX AND CLANG AND ("${target}" IN_LIST LUMEN_IWYU))
    find_program (CLANG_IWYU_EXE NAMES "include-what-you-use")
    if (CLANG_IWYU_EXE)
		set_target_properties(${target} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "${CLANG_IWYU_EXE};-Xiwyu;--cxx17ns;-Qunused-arguments;-Werror")
    endif()
  endif()
endfunction()

function(include_what_you_use_with_mappings target dirs mappings)
  if (NOT LINUX AND CLANG AND ("${target}" IN_LIST LUMEN_IWYU))
    find_program (CLANG_IWYU_EXE NAMES "include-what-you-use")
    if (CLANG_IWYU_EXE)
		foreach(map ${mappings})
			list(APPEND mapdirs ";-Xiwyu;--mapping_file=${map}")
		endforeach()
		set_target_properties(${target} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "${CLANG_IWYU_EXE};${mapdirs};-Xiwyu;--cxx17ns;-Qunused-arguments;-Werror")
    endif()
  endif()
endfunction()

# Xcode does not reliably run the archive phase for a STATIC library whose
# source list contains only object-library generator expressions. Give those
# targets one real, target-specific translation unit on iOS/Xcode; other
# generators keep the original object-only archive layout.
function(add_static_library_from_objects target object_target)
	if(IOS AND CMAKE_GENERATOR STREQUAL "Xcode")
		string(MAKE_C_IDENTIFIER "${target}" target_identifier)
		set(anchor "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target_identifier}_xcode_archive_anchor.c")
		file(WRITE "${anchor}"
			"void lumen_xcode_archive_anchor_${target_identifier}(void) {}\n")
		add_library(${target} STATIC "${anchor}" $<TARGET_OBJECTS:${object_target}>)
	else()
		add_library(${target} STATIC $<TARGET_OBJECTS:${object_target}>)
	endif()
endfunction()

function(clang_tidy_check target config headers dirs)
  if (CLANG AND ("${target}" IN_LIST LUMEN_CLANG_TYDY) AND NOT MSVC)
    find_program (CLANG_TIDY_EXE NAMES "clang-tidy")
    if (CLANG_TIDY_EXE)
		foreach(dir ${dirs})
			list(APPEND incdirs "-extra-arg=-I${dir}")
		endforeach()
		foreach(header ${headers})
			list(APPEND filter "(${header}.*)")
		endforeach()
		string(REPLACE ";" "|" filter "${filter}")
		if ("${config}" STREQUAL "")
			set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-warnings-as-errors=*;-header-filter=${filter};-extra-arg=-std=c++17;${incdirs}")	
		else()		
			set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-config=${config};-warnings-as-errors=*;-header-filter=${filter};-extra-arg=-std=c++17;${incdirs}")	
		endif()
    endif()
  endif()
endfunction()

function(clang_tidy_fix target config headers dirs)
  if (CLANG AND ("${target}" IN_LIST LUMEN_CLANG_TYDY))
    find_program (CLANG_TIDY_EXE NAMES "clang-tidy")
    if (CLANG_TIDY_EXE)
		foreach(dir ${dirs})
			list(APPEND incdirs "-extra-arg=-I${dir}")
		endforeach()
		foreach(header ${headers})
			list(APPEND filter "(${header}.*)")
		endforeach()
		string(REPLACE ";" "|" filter "${filter}")
		if ("${config}" STREQUAL "")
			set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-fix;-format-style=file;-header-filter=${filter};-extra-arg=-std=c++17;${incdirs}")	
		else()	
			set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE};-fix;-format-style=file;-config=${config};-header-filter=${filter};-extra-arg=-std=c++17;${incdirs}")	
		endif()
    endif()
  endif()
endfunction()

# MSVC enables C++ exceptions by default. Close that platform-specific gap in
# Lumen-owned targets without changing the exception model of dependencies.
function(lumen_disable_msvc_cxx_exceptions target)
	if(NOT TARGET "${target}")
		message(FATAL_ERROR "Cannot apply the C++ exception policy to missing target '${target}'")
	endif()

	target_compile_options(${target} PRIVATE
		"$<$<COMPILE_LANGUAGE:CXX>:/EHs-c->"
	)
endfunction()

function(lumen_apply_first_party_msvc_exception_policy directory)
	get_property(LUMEN_DIRECTORY_TARGETS DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
	foreach(LUMEN_TARGET IN LISTS LUMEN_DIRECTORY_TARGETS)
		get_target_property(LUMEN_TARGET_IMPORTED "${LUMEN_TARGET}" IMPORTED)
		get_target_property(LUMEN_TARGET_TYPE "${LUMEN_TARGET}" TYPE)
		if(LUMEN_TARGET_IMPORTED OR
		   LUMEN_TARGET_TYPE STREQUAL "INTERFACE_LIBRARY" OR
		   LUMEN_TARGET_TYPE STREQUAL "UTILITY")
			continue()
		endif()

		get_target_property(LUMEN_TARGET_SOURCE_DIR "${LUMEN_TARGET}" SOURCE_DIR)
		string(FIND "${LUMEN_TARGET_SOURCE_DIR}/" "${PROJECT_SOURCE_DIR}/" LUMEN_PROJECT_PREFIX)
		if(NOT LUMEN_PROJECT_PREFIX EQUAL 0)
			continue()
		endif()

		string(FIND "${LUMEN_TARGET_SOURCE_DIR}/" "${PROJECT_SOURCE_DIR}/3rdparty/" LUMEN_THIRDPARTY_PREFIX)
		if(LUMEN_THIRDPARTY_PREFIX EQUAL 0)
			continue()
		endif()

		lumen_disable_msvc_cxx_exceptions("${LUMEN_TARGET}")
	endforeach()

	get_property(LUMEN_DIRECTORY_SUBDIRECTORIES DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
	foreach(LUMEN_SUBDIRECTORY IN LISTS LUMEN_DIRECTORY_SUBDIRECTORIES)
		lumen_apply_first_party_msvc_exception_policy("${LUMEN_SUBDIRECTORY}")
	endforeach()
endfunction()

macro(config_compiler_and_linker)

set(LUMEN_WARNINGS_ARE_ERRORS ON)

set(LUMEN_C_FLAGS "")
set(LUMEN_CPP_FLAGS "")


if (APPLE_PLATFORM)
	# CMake's forced response files are emitted relative to the top-level
	# Ninja directory, while Apple's ar is invoked for nested targets.  A
	# cached value from another configuration therefore makes ar look for
	# CMakeFiles/*.rsp in the wrong directory.
	SET(CMAKE_NINJA_FORCE_RESPONSE_FILE 0 CACHE INTERNAL "" FORCE)
	set(CMAKE_C_ARCHIVE_CREATE "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-ar.sh qc <TARGET> <OBJECTS>")
	set(CMAKE_CXX_ARCHIVE_CREATE "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos-ar.sh qc <TARGET> <OBJECTS>")
else()
	SET(CMAKE_NINJA_FORCE_RESPONSE_FILE 1 CACHE INTERNAL "" FORCE)
endif()

if(MSVC)
	if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
		string(REGEX REPLACE "/W[0-4]" "/W3" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	else()
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3")
	endif()
	
	string(REGEX REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
	string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
			
	if (CLANG)
		set(LUMEN_CPP_FLAGS "${LUMEN_CPP_FLAGS} /utf-8 /Oy- /wd4244 /wd4305 /wd4800 /wd4345")
	else()
		# MSVC without Clang: add extra warning suppressions for VS2026 compatibility
		set(LUMEN_CPP_FLAGS "${LUMEN_CPP_FLAGS} /Zc:__cplusplus /utf-8 /Oy- /wd4244 /wd4305 /wd4800 /wd4345 /wd4267 /wd4996 /wd4229")
		set(LUMEN_WARNINGS_ARE_ERRORS OFF)  # Pure MSVC has more strict warnings
	endif()
	
	add_link_options("$<$<CONFIG:RELEASE>:/OPT:NOREF>")
  
	if(LUMEN_WARNINGS_ARE_ERRORS)
		#set(LUMEN_CPP_FLAGS "${LUMEN_CPP_FLAGS} /WX")
	    add_compile_options(/WX)
	endif()

	set(LUMEN_C_FLAGS "${LUMEN_CPP_FLAGS}")
	
elseif(MINGW OR ANDROID OR LINUX OR APPLE_PLATFORM)

	if (CLANG)
	    set(LUMEN_CPP_FLAGS "${LUMEN_CPP_FLAGS} -fno-rtti -fno-exceptions -fcolor-diagnostics -finput-charset=UTF-8 -fexec-charset=UTF-8 -g -fno-strict-aliasing -fno-omit-frame-pointer -Wall -fmessage-length=0")
	    if (MINGW)
		set(LUMEN_CPP_FLAGS "${LUMEN_CPP_FLAGS} -static")
	    endif()
	else()
		set(LUMEN_CPP_FLAGS "${LUMEN_CPP_FLAGS} -fno-exceptions -fdiagnostics-color=always -finput-charset=UTF-8 -fexec-charset=UTF-8 -static-libgcc -static-libstdc++ -g -fno-strict-aliasing -fno-omit-frame-pointer -Wall -Wno-unused-value -fmessage-length=0")
	endif()
	
    if(LUMEN_WARNINGS_ARE_ERRORS)
        #set(LUMEN_CPP_FLAGS "${LUMEN_CPP_FLAGS} -Werror")
	add_compile_options(-Werror)
    endif()

	# Xcode enables -Wshorten-64-to-32 outside Clang's regular -Wall set.
	# Lumen's guest ABI intentionally transports many uint32_t values through
	# uint64_t parameter blocks, so keep the iOS warning policy aligned with
	# the Ninja/Clang build while retaining -Werror for all enabled warnings.
	if(IOS)
		add_compile_options(-Wno-shorten-64-to-32)
	endif()

	# Objective-C++ needs a language-specific policy: disable only C++
	# throw/catch while retaining Objective-C @try/@catch/@finally cleanup.
	if(APPLE_PLATFORM)
		add_compile_options(
			"$<$<COMPILE_LANGUAGE:OBJCXX>:-fno-cxx-exceptions>"
			"$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-exceptions>"
		)
	endif()

	add_link_options("-g")
	
	unset(CMAKE_CXX_STANDARD_LIBRARIES CACHE)
	unset(CMAKE_C_STANDARD_LIBRARIES CACHE)
	
	set(LUMEN_C_FLAGS "${LUMEN_CPP_FLAGS}")
	
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LUMEN_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LUMEN_CPP_FLAGS}")

endmacro()
