file(GLOB sys_src
    "src/*.cpp"
)

if(ANDROID)
	list(REMOVE_ITEM sys_src
		"${CMAKE_CURRENT_SOURCE_DIR}/src/SysMacOSDbg.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/SysWindowsDbg.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/SysWindowsFileIO.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/SysWindowsVirtual.cpp"
	)
endif()

add_library(sys STATIC ${sys_src})

target_link_libraries(sys core cpuinfo)

get_property(inc_headers TARGET sys PROPERTY INCLUDE_DIRECTORIES)

list(APPEND check_headers
	${PROJECT_SOURCE_DIR}/include
)

list(APPEND inc_headers
	${PROJECT_SOURCE_DIR}/3rdparty/sdl2/sdl2/include
	${PROJECT_SOURCE_DIR}/3rdparty/cpuinfo/include
)

clang_tidy_check(sys "" "${check_headers}" "${inc_headers}")
#clang_tidy_fix(sys_obj "{Checks: '-*,cppcoreguidelines-init-variables'}" "${check_headers}" "${inc_headers}")

include_what_you_use(sys "${inc_headers}")

