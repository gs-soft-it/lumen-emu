# Native translations are tied to the audited FEX overlay, adapter and toolchain.
# FEX also buckets its cache by code-generation configuration and host features.
function(lumen_fex_translation_cache_build_identity target repository_root fex_source)
  file(GLOB adapter_headers CONFIGURE_DEPENDS
    "${repository_root}/android/app/src/main/cpp/AndroidFexCore*.h"
    "${repository_root}/android/app/src/main/cpp/Fex*.h"
    "${repository_root}/android/app/src/main/cpp/MacOSFexCore*.h")
  set(identity_sources ${adapter_headers}
    "${repository_root}/android/app/src/main/cpp/AndroidFexCoreRuntime.cpp"
    "${repository_root}/android/app/src/main/cpp/fexcore/CMakeLists.txt"
    "${repository_root}/macos/fexcore-runtime/CMakeLists.txt"
    "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
  list(SORT identity_sources)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${identity_sources})
  get_filename_component(overlay_name "${fex_source}" NAME)
  set(identity "lumen-fex-native-cache-v1;${overlay_name};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_BUILD_TYPE};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_RELEASE};${CMAKE_CXX_FLAGS_DEBUG};${CMAKE_CXX_FLAGS_RELWITHDEBINFO};${CMAKE_CXX_FLAGS_MINSIZEREL};${CMAKE_OSX_DEPLOYMENT_TARGET};${ANDROID_ABI};${ANDROID_PLATFORM}")
  foreach(source IN LISTS identity_sources)
    file(SHA256 "${source}" source_hash)
    get_filename_component(source_name "${source}" NAME)
    string(APPEND identity ";${source_name}:${source_hash}")
  endforeach()
  string(SHA256 build_id "${identity}")
  target_compile_definitions(${target} PRIVATE
    LUMEN_FEX_TRANSLATION_CACHE_BUILD_ID="${build_id}")
endfunction()
