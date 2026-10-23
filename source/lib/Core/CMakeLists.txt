file(GLOB core_src
    "src/*.cpp"
)

if (APPLE_PLATFORM)
	list(REMOVE_ITEM core_src "${CMAKE_CURRENT_SOURCE_DIR}/src/VirtualMemory.cpp")
	# VirtualMemoryMacOS.cpp is already part of the glob.
else()
	list(REMOVE_ITEM core_src "${CMAKE_CURRENT_SOURCE_DIR}/src/VirtualMemoryMacOS.cpp")
endif()

if (MSVC AND CLANG)
	set_source_files_properties(${core_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack")
endif()

#add_library(core_obj OBJECT ${core_src})
add_library(core STATIC ${core_src})
#add_library(core STATIC $<TARGET_OBJECTS:core_obj>)

target_link_libraries(core sys math SDL2-static sqlite lzma zstd miniz)

if(MACOS AND LUMEN_MACOS_X64_BACKEND STREQUAL "fex")
	if(NOT TARGET lumen_macos_fexcore_runtime)
		message(FATAL_ERROR
			"The macOS FEX runtime target must exist before the core VM barrier")
	endif()
	target_compile_definitions(core PRIVATE LUMEN_FEXCORE_RUNTIME=1)
	target_include_directories(core PRIVATE
		"${PROJECT_SOURCE_DIR}/../android/app/src/main/cpp")
	target_link_libraries(core lumen_macos_fexcore_runtime)
endif()

get_property(inc_headers TARGET core PROPERTY INCLUDE_DIRECTORIES)
list(APPEND inc_headers 
	${PROJECT_SOURCE_DIR}/3rdparty/sdl2/sdl2/include
	${PROJECT_SOURCE_DIR}/3rdparty/sqlite/include
	${PROJECT_SOURCE_DIR}/3rdparty/lzma/include
	${PROJECT_SOURCE_DIR}/3rdparty/zstd/lib
	${PROJECT_SOURCE_DIR}/3rdparty/miniz/src
)

target_include_directories(core PRIVATE ${inc_headers})
#target_include_directories(core_obj PRIVATE ${inc_headers})

list(APPEND check_headers
	${PROJECT_SOURCE_DIR}/include
)

#clang_tidy_check(core_obj "" "${check_headers}" "${inc_headers}")
clang_tidy_check(core "" "${check_headers}" "${inc_headers}")

#include_what_you_use(core_obj "${inc_headers}")
include_what_you_use(core "${inc_headers}")
