execute_process(COMMAND "${WORKLOAD_TEST}" "${WORKLOAD_CASE}"
	RESULT_VARIABLE workload_result OUTPUT_VARIABLE workload_output ERROR_VARIABLE workload_error TIMEOUT 10)
if(NOT workload_result MATCHES "^(65|321)$" OR
   NOT "${workload_output}${workload_error}" MATCHES "Gnm workload output is not writable")
	message(FATAL_ERROR "Expected checked Gnm output rejection; result=${workload_result}\n${workload_output}${workload_error}")
endif()
