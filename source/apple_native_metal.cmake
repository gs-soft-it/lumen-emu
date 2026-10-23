include_guard(GLOBAL)

# Resolve the target architecture rather than the build host. An explicit
# CMAKE_OSX_ARCHITECTURES wins for cross-builds; native Apple builds fall back
# to the processor detected by CMake.
function(lumen_detect_apple_native_metal_target output_supported output_architectures)
	set(LUMEN_APPLE_METAL_ARCHITECTURES "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT LUMEN_APPLE_METAL_ARCHITECTURES)
		set(LUMEN_APPLE_METAL_ARCHITECTURES "${CMAKE_SYSTEM_PROCESSOR}")
	endif()

	set(LUMEN_APPLE_METAL_SUPPORTED FALSE)
	if(APPLE_PLATFORM AND LUMEN_APPLE_METAL_ARCHITECTURES)
		# The complete runtime contains architecture-specific CPU probes. Build
		# each supported Apple slice natively instead of advertising a universal
		# graph that cannot select both probe implementations in one target.
		list(LENGTH LUMEN_APPLE_METAL_ARCHITECTURES
			LUMEN_APPLE_METAL_ARCHITECTURE_COUNT)
		if(LUMEN_APPLE_METAL_ARCHITECTURE_COUNT EQUAL 1)
			list(GET LUMEN_APPLE_METAL_ARCHITECTURES 0
				LUMEN_APPLE_METAL_ARCHITECTURE)
			string(STRIP "${LUMEN_APPLE_METAL_ARCHITECTURE}"
				LUMEN_APPLE_METAL_ARCHITECTURE)
			string(TOLOWER "${LUMEN_APPLE_METAL_ARCHITECTURE}"
				LUMEN_APPLE_METAL_ARCHITECTURE_LOWER)
			if(LUMEN_APPLE_METAL_ARCHITECTURE_LOWER MATCHES
			   "^(arm64|aarch64|x86_64)$")
				set(LUMEN_APPLE_METAL_SUPPORTED TRUE)
			endif()
		endif()
	endif()

	set(${output_supported} "${LUMEN_APPLE_METAL_SUPPORTED}" PARENT_SCOPE)
	set(${output_architectures} "${LUMEN_APPLE_METAL_ARCHITECTURES}" PARENT_SCOPE)
endfunction()
