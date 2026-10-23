# SPDX-License-Identifier: GPL-2.0-only
# Reconfiguration must verify an already-applied reviewed patch, not apply it
# a second time or discard local changes in the fetched dependency.
foreach(required IN ITEMS SOURCE_DIR PATCH GIT_EXECUTABLE)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing patch verifier argument: ${required}")
  endif()
endforeach()
file(SHA256 "${PATCH}" patch_sha)
if(NOT patch_sha STREQUAL "061a02bb73360ea7403f960caa8414728914bfc963372eadddb22c0a03141169")
  message(FATAL_ERROR "The reviewed upstream ZArchive patch changed")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse HEAD
  OUTPUT_VARIABLE source_head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR
   NOT source_head STREQUAL "d2c717730092c7bf8cbb033b12fd4001b7c4d932")
  message(FATAL_ERROR "ZArchive source pin mismatch")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" remote get-url origin
  OUTPUT_VARIABLE source_origin OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT source_origin STREQUAL "https://github.com/Exzap/ZArchive.git")
  message(FATAL_ERROR "ZArchive source origin mismatch")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
  apply --check --ignore-whitespace "${PATCH}" RESULT_VARIABLE patch_result
  OUTPUT_QUIET ERROR_QUIET)
if(patch_result EQUAL 0)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
    apply --ignore-whitespace "${PATCH}" RESULT_VARIABLE patch_result)
  if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "Could not apply the reviewed ZArchive patch")
  endif()
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
  apply --reverse --check --ignore-whitespace "${PATCH}"
  RESULT_VARIABLE reverse_result OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}"
  status --porcelain=v1 --untracked-files=all
  OUTPUT_VARIABLE source_status OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
if(NOT reverse_result EQUAL 0 OR NOT result EQUAL 0 OR
   NOT source_status STREQUAL " M include/zarchive/zarchivecommon.h\n M src/zarchivereader.cpp")
  message(FATAL_ERROR "ZArchive dependency has changes beyond its reviewed patch")
endif()
file(SHA256 "${SOURCE_DIR}/include/zarchive/zarchivecommon.h" common_sha)
file(SHA256 "${SOURCE_DIR}/src/zarchivereader.cpp" reader_sha)
if(NOT common_sha STREQUAL "b146d48503126f87e2bcbaab7a58b842616ea4d71992ddbbaee8dfe7476f034e" OR
   NOT reader_sha STREQUAL "8d563c24146827fa2df0d57d176b2bb05fa9de7e8790449cb73c16dcc62eb3f7")
  message(FATAL_ERROR "Patched ZArchive source content mismatch")
endif()
