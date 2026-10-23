if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}")
	message(FATAL_ERROR "V_CVT_OFF_F32_I4 arithmetic probe executable is missing")
endif()
if(NOT DEFINED MODE)
	message(FATAL_ERROR "V_CVT_OFF_F32_I4 arithmetic probe test mode is required")
endif()

if(MODE STREQUAL "shader-only")
	set(PROBE_ARGUMENTS --shader-only)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VCVT_OFF_F32_I4_VULKAN_PROBE\\].*mode=shader-only"
		".*scope=diagnostic-production-core.*core=shared-signed-convert-scale"
		".*spirv=assembled\\+validated\\+shape-audited"
		".*domain=all-known-signed4.*nibble-extraction=none"
		".*cases=16.*vulkan-created=no.*status=clean")
elseif(MODE STREQUAL "amd-13c0")
	set(PROBE_ARGUMENTS)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VCVT_OFF_F32_I4_VULKAN_PROBE\\].*mode=runtime"
		".*scope=diagnostic-production-core.*core=shared-signed-convert-scale"
		".*backend=vulkan.*vendor=0x1002.*device=0x13c0"
		".*domain=all-known-signed4.*nibble-extraction=none"
		".*result=bit-exact.*expected=all.*dispatches=3"
		".*inputs=unchanged.*canaries=unchanged.*deterministic=yes.*cases=16.*status=clean")
elseif(MODE STREQUAL "native-metal-apple-m1")
	set(PROBE_ARGUMENTS --native-metal-apple-m1)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VCVT_OFF_F32_I4_VULKAN_PROBE\\].*mode=runtime"
		".*scope=diagnostic-production-core.*core=shared-signed-convert-scale"
		".*backend=native-metal.*adapter=static.*host=apple-m1"
		".*vendor=0x106b.*reported-type=(integrated|discrete).*translation=black-box"
		".*domain=all-known-signed4.*nibble-extraction=none"
		".*result=bit-exact.*expected=all.*dispatches=3"
		".*inputs=unchanged.*canaries=unchanged.*deterministic=yes.*cases=16.*status=clean")
else()
	message(FATAL_ERROR "Unknown V_CVT_OFF_F32_I4 arithmetic probe test mode: ${MODE}")
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
	message(FATAL_ERROR "V_CVT_OFF_F32_I4 arithmetic probe mode ${MODE} returned ${PROBE_RESULT}")
endif()
if(NOT PROBE_STDOUT MATCHES "${EXPECTED_MARKER}")
	message(FATAL_ERROR "V_CVT_OFF_F32_I4 arithmetic probe mode ${MODE} omitted its clean scope marker")
endif()
if(MODE STREQUAL "native-metal-apple-m1" AND
   PROBE_STDOUT MATCHES "device=0x|[Uu][Uu][Ii][Dd]=")
	message(FATAL_ERROR "V_CVT_OFF_F32_I4 Native Metal probe leaked unstable device identity")
endif()
