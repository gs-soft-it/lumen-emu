include_guard(GLOBAL)

include(FetchContent)

# Vulkan SDK 1.4.321.0 supplies physical-storage-buffer lowering required
# by the Gen4 renderer. Compiler headers and implementation share this pin.
set(LUMEN_SPIRV_CROSS_MSL_COMMIT
	"d8e3e2b141b8c8a167b2e3984736a6baacff316c"
)

# Build only the dependency chain required by CompilerMSL. The C API, CLI,
# tests and the other source-language backends are deliberately excluded.
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)

FetchContent_Declare(lumen_spirv_cross_msl_upstream
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
	GIT_TAG "${LUMEN_SPIRV_CROSS_MSL_COMMIT}"
	GIT_SUBMODULES ""
	GIT_PROGRESS TRUE
)

# Keep a policy floor compatible with existing supported CMake versions.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
	set(_lumen_spirv_cross_had_policy_minimum FALSE)
	if(DEFINED CMAKE_POLICY_VERSION_MINIMUM)
		set(_lumen_spirv_cross_had_policy_minimum TRUE)
		set(_lumen_spirv_cross_saved_policy_minimum
			"${CMAKE_POLICY_VERSION_MINIMUM}"
		)
	endif()
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

FetchContent_MakeAvailable(lumen_spirv_cross_msl_upstream)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
	if(_lumen_spirv_cross_had_policy_minimum)
		set(CMAKE_POLICY_VERSION_MINIMUM
			"${_lumen_spirv_cross_saved_policy_minimum}"
		)
	else()
		unset(CMAKE_POLICY_VERSION_MINIMUM)
	endif()
	unset(_lumen_spirv_cross_had_policy_minimum)
	unset(_lumen_spirv_cross_saved_policy_minimum)
endif()

if(NOT TARGET spirv-cross-core OR
   NOT TARGET spirv-cross-glsl OR
   NOT TARGET spirv-cross-msl)
	message(FATAL_ERROR "The pinned SPIRV-Cross core/GLSL/MSL targets are unavailable")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	target_compile_options(spirv-cross-core PRIVATE
		-Wno-unqualified-std-cast-call
	)
	target_compile_options(spirv-cross-glsl PRIVATE
		-Wno-unqualified-std-cast-call
	)
	target_compile_options(spirv-cross-msl PRIVATE
		-Wno-unqualified-std-cast-call
	)
endif()

add_library(lumen_spirv_cross_msl INTERFACE)
# CompilerMSL exposes inline error paths in its public headers. Propagate the
# same fail-fast policy used to compile the pinned implementation so consumers
# never instantiate C++ throw expressions with exceptions disabled.
target_compile_definitions(lumen_spirv_cross_msl INTERFACE
	SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
)
target_link_libraries(lumen_spirv_cross_msl INTERFACE spirv-cross-msl)
add_library(Lumen::SpirvCrossMsl ALIAS lumen_spirv_cross_msl)
