if(NOT DEFINED LUMEN_SOURCE_DIR)
	message(FATAL_ERROR "LUMEN_SOURCE_DIR is required")
endif()

include("${LUMEN_SOURCE_DIR}/apple_native_metal.cmake")

function(lumen_expect_metal_support test_name apple_platform osx_arch host_arch expected)
	set(APPLE_PLATFORM "${apple_platform}")
	set(CMAKE_OSX_ARCHITECTURES "${osx_arch}")
	set(CMAKE_SYSTEM_PROCESSOR "${host_arch}")
	lumen_detect_apple_native_metal_target(LUMEN_ACTUAL_SUPPORTED LUMEN_ACTUAL_ARCHITECTURES)
	if(NOT "${LUMEN_ACTUAL_SUPPORTED}" STREQUAL "${expected}")
		message(FATAL_ERROR
			"${test_name}: expected support=${expected}, got ${LUMEN_ACTUAL_SUPPORTED} "
			"for architectures '${LUMEN_ACTUAL_ARCHITECTURES}'")
	endif()
endfunction()

lumen_expect_metal_support(macos_arm64 TRUE "arm64" "x86_64" TRUE)
lumen_expect_metal_support(macos_aarch64 TRUE "aarch64" "x86_64" TRUE)
lumen_expect_metal_support(macos_x86_64 TRUE "x86_64" "arm64" TRUE)
lumen_expect_metal_support(macos_universal TRUE "arm64;x86_64" "arm64" FALSE)
lumen_expect_metal_support(apple_unsupported_arch TRUE "ppc64" "arm64" FALSE)
lumen_expect_metal_support(non_apple_arm64 FALSE "arm64" "arm64" FALSE)

set(LUMEN_VULKAN_CMAKE "${LUMEN_SOURCE_DIR}/3rdparty/vulkan/CMakeLists.txt")
file(READ "${LUMEN_VULKAN_CMAKE}" LUMEN_VULKAN_CONTENTS)
set(LUMEN_CANDIDATE_BEGIN_MARKER "# LUMEN_APPLE_NATIVE_METAL_CANDIDATE_BEGIN")
set(LUMEN_CANDIDATE_END_MARKER "# LUMEN_APPLE_NATIVE_METAL_CANDIDATE_END")
string(FIND "${LUMEN_VULKAN_CONTENTS}" "${LUMEN_CANDIDATE_BEGIN_MARKER}"
	LUMEN_CANDIDATE_BEGIN)
string(FIND "${LUMEN_VULKAN_CONTENTS}" "${LUMEN_CANDIDATE_END_MARKER}"
	LUMEN_CANDIDATE_END)
if(LUMEN_CANDIDATE_BEGIN EQUAL -1 OR LUMEN_CANDIDATE_END EQUAL -1 OR
	LUMEN_CANDIDATE_END LESS LUMEN_CANDIDATE_BEGIN)
	message(FATAL_ERROR "Native Metal candidate block was not found in ${LUMEN_VULKAN_CMAKE}")
endif()
string(LENGTH "${LUMEN_CANDIDATE_END_MARKER}" LUMEN_CANDIDATE_END_MARKER_LENGTH)
math(EXPR LUMEN_CANDIDATE_LENGTH
	"${LUMEN_CANDIDATE_END} - ${LUMEN_CANDIDATE_BEGIN} + ${LUMEN_CANDIDATE_END_MARKER_LENGTH}")
string(SUBSTRING "${LUMEN_VULKAN_CONTENTS}" "${LUMEN_CANDIDATE_BEGIN}"
	"${LUMEN_CANDIDATE_LENGTH}" LUMEN_METAL_CANDIDATE)

foreach(LUMEN_REQUIRED_TOKEN IN ITEMS
		"add_library(vulkan-1 STATIC"
		"MetalVulkanCore.mm"
		"MetalVulkanCommands.mm"
		"Lumen::SpirvCrossMsl"
		"Metal QuartzCore Foundation"
		"AppKit"
		"UIKit"
		"_vkGetInstanceProcAddr"
)
	string(FIND "${LUMEN_METAL_CANDIDATE}" "${LUMEN_REQUIRED_TOKEN}" LUMEN_TOKEN_OFFSET)
	if(LUMEN_TOKEN_OFFSET EQUAL -1)
		message(FATAL_ERROR
			"Native Metal candidate is missing required token '${LUMEN_REQUIRED_TOKEN}'")
	endif()
endforeach()

# The rollback branch may still contain this dependency. It must never occur
# in the candidate selected for supported Apple architectures.
string(FIND "${LUMEN_METAL_CANDIDATE}" "MoltenVK" LUMEN_MOLTENVK_OFFSET)
if(NOT LUMEN_MOLTENVK_OFFSET EQUAL -1)
	message(FATAL_ERROR "Native Metal candidate still references MoltenVK")
endif()
