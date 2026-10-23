cmake_minimum_required(VERSION 3.22)

if(NOT EXISTS "${BINARY}" OR NOT EXISTS "${CODESIGN}" OR
   NOT CASE MATCHES "^(jit|runtime|jit_denied|root_state|signal)$")
  message(FATAL_ERROR "A built FEX gate, codesign and an explicit test case are required")
endif()
# Work only on a disposable copy, preserving the linked build artifact.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef RUN_ID)
set(WORK "${CMAKE_CURRENT_BINARY_DIR}/fex-hardened-${CASE}-${RUN_ID}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY_FILE "${BINARY}" "${WORK}/probe")
set(ENTITLEMENTS "")
if(NOT CASE STREQUAL "jit_denied")
  file(WRITE "${WORK}/entitlements.plist"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict><key>com.apple.security.cs.allow-jit</key><true/></dict></plist>\n")
  set(ENTITLEMENTS --entitlements "${WORK}/entitlements.plist")
endif()
execute_process(COMMAND "${CODESIGN}" --force --sign - --options runtime
  ${ENTITLEMENTS} "${WORK}/probe" RESULT_VARIABLE SIGN_RESULT)
if(NOT SIGN_RESULT EQUAL 0)
  message(FATAL_ERROR "Hardened ad-hoc signing failed: ${WORK}")
endif()
execute_process(COMMAND "${CODESIGN}" --verify --strict --verbose=2
  "${WORK}/probe" RESULT_VARIABLE VERIFY_RESULT)
if(NOT VERIFY_RESULT EQUAL 0)
  message(FATAL_ERROR "Hardened signature verification failed: ${WORK}")
endif()
set(ARGUMENTS "")
if(CASE STREQUAL "jit_denied")
  set(ARGUMENTS --expect-jit-denied)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LUMEN_MACOS_FEXCORE_RUN
  "${WORK}/probe" ${ARGUMENTS} RESULT_VARIABLE TEST_RESULT TIMEOUT 100)
if(NOT TEST_RESULT EQUAL 0)
  message(FATAL_ERROR "Hardened ${CASE} gate failed (${TEST_RESULT}); copy: ${WORK}")
endif()
file(REMOVE_RECURSE "${WORK}")
message(STATUS "Hardened ${CASE}: PASS (local ad-hoc; no distribution qualification)")
