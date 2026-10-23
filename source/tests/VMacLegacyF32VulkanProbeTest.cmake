if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}")
	message(FATAL_ERROR "V_MAC_LEGACY_F32 Vulkan probe executable is missing")
endif()
if(NOT DEFINED MODE)
	message(FATAL_ERROR "V_MAC_LEGACY_F32 Vulkan probe test mode is required")
endif()

if(MODE STREQUAL "shader-only")
	set(PROBE_ARGUMENTS --shader-only)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VMAC_LEGACY_F32_VULKAN_PROBE\\].*mode=shader-only"
		".*scope=production-core.*core=shared-integer-binary32"
		".*spirv=assembled\\+validated\\+shape-audited.*helpers=once"
		".*float-ops=none.*int64=none.*find-umsb=none.*float-controls=none"
		".*cases=46.*vulkan-created=no.*status=clean")
elseif(MODE STREQUAL "amd-13c0")
	set(PROBE_ARGUMENTS)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VMAC_LEGACY_F32_VULKAN_PROBE\\].*mode=runtime"
		".*scope=production-core.*core=shared-integer-binary32.*backend=vulkan"
		".*vendor=0x1002.*device=0x13c0.*float-ops=none.*float-controls=none"
		".*product=bit-exact.*sum=bit-exact.*expected=all.*dispatches=3"
		".*inputs=unchanged.*canaries=unchanged.*deterministic=yes.*cases=46.*status=clean")
elseif(MODE STREQUAL "native-metal-apple-m1")
	set(PROBE_ARGUMENTS --native-metal-apple-m1)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VMAC_LEGACY_F32_VULKAN_PROBE\\].*mode=runtime"
		".*scope=production-core.*core=shared-integer-binary32"
		".*backend=native-metal.*adapter=static.*host=apple-m1"
		".*vendor=0x106b.*reported-type=(integrated|discrete)"
		".*translation=black-box.*float-ops=none.*float-controls=none"
		".*product=bit-exact.*sum=bit-exact.*expected=all.*dispatches=3"
		".*inputs=unchanged.*canaries=unchanged.*deterministic=yes.*cases=46.*status=clean")
else()
	message(FATAL_ERROR "Unknown V_MAC_LEGACY_F32 Vulkan probe test mode: ${MODE}")
endif()

execute_process(
	COMMAND "${PROBE}" ${PROBE_ARGUMENTS}
	RESULT_VARIABLE PROBE_RESULT
	OUTPUT_VARIABLE PROBE_STDOUT
	ERROR_VARIABLE PROBE_STDERR
)

if(NOT PROBE_STDOUT STREQUAL "")
	message("${PROBE_STDOUT}")
endif()
if(NOT PROBE_STDERR STREQUAL "")
	message("${PROBE_STDERR}")
endif()
if(NOT PROBE_RESULT STREQUAL "0")
	message(FATAL_ERROR
		"V_MAC_LEGACY_F32 Vulkan probe mode ${MODE} returned ${PROBE_RESULT}")
endif()
if(NOT PROBE_STDOUT MATCHES "${EXPECTED_MARKER}")
	message(FATAL_ERROR
		"V_MAC_LEGACY_F32 Vulkan probe mode ${MODE} omitted its clean scope marker")
endif()
if(MODE STREQUAL "native-metal-apple-m1" AND
   PROBE_STDOUT MATCHES "device=0x|[Uu][Uu][Ii][Dd]=")
	message(FATAL_ERROR
		"V_MAC_LEGACY_F32 Native Metal probe leaked unstable device identity")
endif()
