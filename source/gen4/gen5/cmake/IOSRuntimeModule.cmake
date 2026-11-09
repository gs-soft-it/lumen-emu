# SPDX-License-Identifier: GPL-2.0-only
# Each console framework has a private SDL/FEX/dependency graph. Only this
# small C boundary is visible to the UIKit host; no dependency may interpose
# the shell's SDL2 or the other console's C++ state.
function(lumen_configure_ios_runtime_module target name)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR
     NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "iOS runtime modules require a thin arm64 iOS target")
  endif()
  set(LUMEN_IOS_FRAMEWORK_NAME "${name}")
  set(LUMEN_IOS_FRAMEWORK_MINIMUM_OS "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  set(LUMEN_IOS_FRAMEWORK_SUPPORTED_PLATFORM iPhoneOS)
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(LUMEN_IOS_FRAMEWORK_SUPPORTED_PLATFORM iPhoneSimulator)
  endif()
  set(plist "${CMAKE_CURRENT_BINARY_DIR}/${name}-Info.plist")
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/IOSRuntimeModule-Info.plist.in" "${plist}" @ONLY)
  set_target_properties(${target} PROPERTIES
    FRAMEWORK TRUE FRAMEWORK_VERSION A OUTPUT_NAME "${name}"
    MACOSX_FRAMEWORK_IDENTIFIER "it.gssoft.lumenemu.${name}"
    MACOSX_FRAMEWORK_BUNDLE_VERSION 1 MACOSX_FRAMEWORK_SHORT_VERSION_STRING 1.0
    MACOSX_FRAMEWORK_INFO_PLIST "${plist}"
    INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE
    CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN TRUE)
  set(exports "${CMAKE_CURRENT_BINARY_DIR}/${name}.exports")
  file(WRITE "${exports}"
    "_LumenIOSFexModuleAbiV1\n_LumenIOSFexModuleConsoleV1\n_LumenIOSFexModuleSetHostV1\n_LumenIOSFexModuleProbeV1\n_LumenIOSFexModulePrepareJitV1\n_LumenIOSFexModuleLaunchV1\n_LumenIOSFexModuleRequestStopV1\n_LumenIOSFexModuleSetButtonV1\n_LumenIOSFexModuleSetAxisV1\n_LumenIOSFexModuleDiagnosticV1\n_LumenIOSJitDebuggerHandshake\n_LumenIOSJitPrepareRegion\n_LumenIOSJitDetach\n")
  if(name STREQUAL "LumenPS4Runtime")
    file(APPEND "${exports}" "_LumenIOSFexModuleImeActiveV1\n")
  endif()
  target_link_options(${target} PRIVATE "LINKER:-exported_symbols_list,${exports}")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${exports}")
  target_link_libraries(${target} PRIVATE "-framework CoreFoundation")
endfunction()
