if(NOT DEFINED PROBE OR NOT EXISTS "${PROBE}")
	message(FATAL_ERROR "V_RSQ_CLAMP_F32 Vulkan probe executable is missing")
endif()
if(NOT DEFINED MODE)
	message(FATAL_ERROR "V_RSQ_CLAMP_F32 Vulkan probe test mode is required")
endif()

if(MODE STREQUAL "shader-only")
	set(PROBE_ARGUMENTS --shader-only)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VRSQ_CLAMP_F32_VULKAN_PROBE\\].*mode=shader-only"
		".*scope=diagnostic-production-core.*core=shared-integer-binary32"
		".*spirv=assembled\\+validated\\+shape-audited.*helpers=audited"
		".*float-ops=none.*int64=none.*find-umsb=none.*float-controls=none"
		".*cases=[0-9]+.*vulkan-created=no.*status=clean")
elseif(MODE STREQUAL "exhaustive-host")
	set(PROBE_ARGUMENTS --exhaustive-host)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VRSQ_CLAMP_F32_HOST_EXHAUSTIVE\\]"
		".*subnormals=all-signs.*nans=all-signs-payloads"
		".*oracle=independent-uint128.*checksum=[0-9a-f]+.*status=clean")
elseif(MODE STREQUAL "amd-13c0")
	set(PROBE_ARGUMENTS)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VRSQ_CLAMP_F32_VULKAN_PROBE\\].*mode=runtime"
		".*scope=diagnostic-production-core.*core=shared-integer-binary32.*backend=vulkan"
		".*vendor=0x1002.*device=0x13c0.*float-ops=none.*int64=none"
		".*find-umsb=none.*float-controls=none.*result=bit-exact.*expected=all"
		".*dispatches=3.*inputs=unchanged.*canaries=unchanged.*deterministic=yes"
		".*cases=[0-9]+.*status=clean")
elseif(MODE STREQUAL "native-metal-apple-m1")
	set(PROBE_ARGUMENTS --native-metal-apple-m1)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VRSQ_CLAMP_F32_VULKAN_PROBE\\].*mode=runtime"
		".*scope=diagnostic-production-core.*core=shared-integer-binary32"
		".*backend=native-metal.*adapter=static.*host=apple-m1"
		".*vendor=0x106b.*reported-type=(integrated|discrete).*translation=black-box"
		".*float-ops=none.*int64=none.*find-umsb=none.*float-controls=none"
		".*result=bit-exact.*expected=all.*dispatches=3"
		".*inputs=unchanged.*canaries=unchanged.*deterministic=yes"
		".*cases=[0-9]+.*status=clean")
elseif(MODE STREQUAL "exhaustive-native-metal-apple-m1")
	set(PROBE_ARGUMENTS --exhaustive-native-metal-apple-m1)
	string(CONCAT EXPECTED_MARKER
		"\\[GFX_VRSQ_CLAMP_F32_VULKAN_EXHAUSTIVE\\].*mode=exhaustive-runtime"
		".*scope=diagnostic-production-core.*core=shared-integer-binary32"
		".*backend=native-metal.*adapter=static.*host=apple-m1"
		".*vendor=0x106b.*reported-type=(integrated|discrete).*translation=black-box"
		".*classes=zeros\\+infinities\\+subnormals\\+nan"
		".*subnormals=all-signs.*nans=all-signs-payloads"
		".*cases=33554432.*chunk-cases=32768.*chunks=1024.*passes=2"
		".*result=bit-exact.*expected=all.*inputs=unchanged.*canaries=unchanged"
		".*deterministic=yes.*checksum=[0-9a-f]+.*status=clean")
else()
	message(FATAL_ERROR "Unknown V_RSQ_CLAMP_F32 Vulkan probe test mode: ${MODE}")
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
	message(FATAL_ERROR "V_RSQ_CLAMP_F32 Vulkan probe mode ${MODE} returned ${PROBE_RESULT}")
endif()
if(NOT PROBE_STDOUT MATCHES "${EXPECTED_MARKER}")
	message(FATAL_ERROR "V_RSQ_CLAMP_F32 Vulkan probe mode ${MODE} omitted its clean scope marker")
endif()
if((MODE STREQUAL "native-metal-apple-m1" OR
    MODE STREQUAL "exhaustive-native-metal-apple-m1") AND
   PROBE_STDOUT MATCHES "device=0x|[Uu][Uu][Ii][Dd]=")
	message(FATAL_ERROR "V_RSQ_CLAMP_F32 Native Metal probe leaked unstable device identity")
endif()
