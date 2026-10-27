# Original correctness fixtures for the direct engine. Run without benchmark
# options on native ARM64 Android/macOS; these targets are not part of the APK.
if(NOT LUMEN_PS5_DIRECT_ENGINE_TARGET)
    set(LUMEN_PS5_DIRECT_ENGINE_TARGET lumen_android_ps5)
endif()
set(direct_test_link_options)
set(direct_test_hide_options)
if(ANDROID)
    set(direct_test_link_options "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
    set(direct_test_hide_options "-Wl,--exclude-libs,liblinkernsbypass.a")
endif()
function(ps5_direct_test name)
    add_executable(${name} EXCLUDE_FROM_ALL ${ARGN})
    target_include_directories(${name} PRIVATE ${inc_headers})
    target_link_libraries(${name} PRIVATE common lumen_ps5_fex_embedding)
    target_compile_options(${name} PRIVATE -fexceptions)
    target_link_options(${name} PRIVATE
        ${direct_test_link_options})
endfunction()

ps5_direct_test(lumen_ps5_direct_audio_timing_test
    "${KYTY_TESTS_DIR}/AudioOutTimingTests.cpp")
target_link_libraries(lumen_ps5_direct_audio_timing_test PRIVATE SDL3::SDL3-static)

add_executable(lumen_ps5_direct_performance_options_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/DirectPerformanceOptionsTest.cpp")
target_compile_features(lumen_ps5_direct_performance_options_test PRIVATE cxx_std_20)

add_executable(lumen_ps5_release_mem_batch_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ReleaseMemBatchTest.cpp")
target_compile_features(lumen_ps5_release_mem_batch_test PRIVATE cxx_std_20)

add_executable(lumen_ps5_memory_budget_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/MemoryBudgetTests.cpp")
target_compile_features(lumen_ps5_memory_budget_test PRIVATE cxx_std_20)
target_link_options(lumen_ps5_memory_budget_test PRIVATE ${direct_test_link_options})

add_executable(lumen_ps5_render_timing_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/RenderTimingTests.cpp")
target_include_directories(lumen_ps5_render_timing_test PRIVATE ${inc_headers})
target_compile_features(lumen_ps5_render_timing_test PRIVATE cxx_std_20)
target_link_libraries(lumen_ps5_render_timing_test PRIVATE Threads::Threads)

add_executable(lumen_ps5_uniform_metadata_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/UniformMetadataTests.cpp")
target_include_directories(lumen_ps5_uniform_metadata_test PRIVATE ${inc_headers})
target_compile_features(lumen_ps5_uniform_metadata_test PRIVATE cxx_std_20)

add_executable(lumen_ps5_video_upload_probe_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/VideoUploadProbeTests.cpp")
target_include_directories(lumen_ps5_video_upload_probe_test PRIVATE ${inc_headers})
target_compile_features(lumen_ps5_video_upload_probe_test PRIVATE cxx_std_20)
target_link_libraries(lumen_ps5_video_upload_probe_test PRIVATE Threads::Threads)

# Decoder and output-buffer lifecycle with native callbacks; the production
# renderer publication helper is covered by the separate renderer fixture.
add_executable(lumen_ps5_direct_avplayer_file_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/AvPlayerFileTests.cpp")
target_include_directories(lumen_ps5_direct_avplayer_file_test PRIVATE ${inc_headers})
target_compile_features(lumen_ps5_direct_avplayer_file_test PRIVATE cxx_std_20)
# Allocator/file callbacks in this fixture are host functions, never guest code.
# Disable only its translated-callback branch; production definitions stay intact.
target_compile_options(lumen_ps5_direct_avplayer_file_test PRIVATE
    -fexceptions -ULUMEN_PS5_FEXCORE)
target_link_libraries(lumen_ps5_direct_avplayer_file_test PRIVATE
    common FFmpeg::ffmpeg ZArchive::zarchive Threads::Threads)
target_link_options(lumen_ps5_direct_avplayer_file_test PRIVATE
    ${direct_test_link_options})

add_executable(lumen_ps5_path_case_cache_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/PathCaseCacheTests.cpp")
target_include_directories(lumen_ps5_path_case_cache_test PRIVATE ${inc_headers})
target_compile_features(lumen_ps5_path_case_cache_test PRIVATE cxx_std_20)
target_compile_options(lumen_ps5_path_case_cache_test PRIVATE -fexceptions)
target_link_libraries(lumen_ps5_path_case_cache_test PRIVATE Threads::Threads)

add_executable(lumen_ps5_direct_spirv_admission_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/DirectSpirvModuleAdmissionTest.cpp")
target_include_directories(lumen_ps5_direct_spirv_admission_test PRIVATE ${inc_headers})
target_compile_features(lumen_ps5_direct_spirv_admission_test PRIVATE cxx_std_20)

add_executable(lumen_ps5_direct_driver_cache_checkpoint_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/DriverCacheCheckpointTests.cpp")
target_include_directories(lumen_ps5_direct_driver_cache_checkpoint_test PRIVATE ${inc_headers})
target_compile_features(lumen_ps5_direct_driver_cache_checkpoint_test PRIVATE cxx_std_20)
target_compile_options(lumen_ps5_direct_driver_cache_checkpoint_test PRIVATE -fexceptions)
target_link_libraries(lumen_ps5_direct_driver_cache_checkpoint_test PRIVATE Threads::Threads)

add_executable(lumen_ps5_direct_hle_dispatch_test EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/DirectHleDispatchTest.cpp"
    "${KYTY_SOURCE_DIR}/loader/hostCall.cpp")
target_include_directories(lumen_ps5_direct_hle_dispatch_test PRIVATE ${inc_headers}
    "${PROJECT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../android/app/src/main/cpp")
target_compile_options(lumen_ps5_direct_hle_dispatch_test PRIVATE -fexceptions)
target_link_libraries(lumen_ps5_direct_hle_dispatch_test PRIVATE Threads::Threads)

add_executable(lumen_ps5_direct_srt_arm64_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/DirectSrtArm64Tests.cpp")
target_include_directories(lumen_ps5_direct_srt_arm64_test PRIVATE ${inc_headers}
    "${PROJECT_BINARY_DIR}")
target_compile_options(lumen_ps5_direct_srt_arm64_test PRIVATE -fexceptions)
target_link_libraries(lumen_ps5_direct_srt_arm64_test PRIVATE
    ${LUMEN_PS5_DIRECT_ENGINE_TARGET} ${kyty_emulator_link_libraries})
target_link_options(lumen_ps5_direct_srt_arm64_test PRIVATE
    ${direct_test_link_options}
    ${direct_test_hide_options})

add_executable(lumen_ps5_direct_resource_tracking_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/ResourceTrackingTests.cpp")
add_executable(lumen_ps5_direct_resource_materialization_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/ResourceMaterializationTests.cpp")
foreach(resource_test IN ITEMS lumen_ps5_direct_resource_tracking_test
                               lumen_ps5_direct_resource_materialization_test)
    target_include_directories(${resource_test} PRIVATE ${inc_headers}
        "${PROJECT_BINARY_DIR}")
    target_compile_options(${resource_test} PRIVATE -fexceptions)
    target_link_libraries(${resource_test} PRIVATE
        ${LUMEN_PS5_DIRECT_ENGINE_TARGET} ${kyty_emulator_link_libraries})
    target_link_options(${resource_test} PRIVATE
        ${direct_test_link_options}
        ${direct_test_hide_options})
endforeach()

# Focused upstream regression fixtures use the same production engine as the APK.
foreach(fixture IN ITEMS "shader_cfg;shaderCfgTests.cpp"
                         "scalar_provenance;ScalarProvenanceTests.cpp"
                         "filesystem;DirectFilesystemTests.cpp")
    list(GET fixture 0 fixture_name)
    list(GET fixture 1 fixture_source)
    set(fixture_target "lumen_ps5_direct_${fixture_name}_test")
    add_executable(${fixture_target} EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/${fixture_source}")
    target_include_directories(${fixture_target} PRIVATE ${inc_headers}
        "${PROJECT_BINARY_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../direct_fex")
    target_compile_options(${fixture_target} PRIVATE -fexceptions)
    target_link_libraries(${fixture_target} PRIVATE
        ${LUMEN_PS5_DIRECT_ENGINE_TARGET} ${kyty_emulator_link_libraries})
    target_link_options(${fixture_target} PRIVATE
        ${direct_test_link_options} ${direct_test_hide_options})
endforeach()

add_executable(lumen_ps5_direct_barycentric_fallback_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/DirectBarycentricFallbackTests.cpp")
target_include_directories(lumen_ps5_direct_barycentric_fallback_test PRIVATE
    ${inc_headers} "${PROJECT_BINARY_DIR}")
target_compile_options(lumen_ps5_direct_barycentric_fallback_test PRIVATE -fexceptions)
target_link_libraries(lumen_ps5_direct_barycentric_fallback_test PRIVATE
    ${LUMEN_PS5_DIRECT_ENGINE_TARGET} ${kyty_emulator_link_libraries})
target_link_options(lumen_ps5_direct_barycentric_fallback_test PRIVATE
    ${direct_test_link_options}
    ${direct_test_hide_options})

ps5_direct_test(lumen_ps5_direct_page_manager_test
    "${KYTY_TESTS_DIR}/PageManagerTests.cpp"
    "${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp")
ps5_direct_test(lumen_ps5_direct_memory_tracker_test
    "${KYTY_TESTS_DIR}/MemoryTrackerTests.cpp"
    "${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
    "${KYTY_SOURCE_DIR}/graphics/host_gpu/memoryTracker.cpp")
ps5_direct_test(lumen_ps5_direct_sync_address_test
    "${KYTY_TESTS_DIR}/SyncOnAddressTests.cpp"
    "${KYTY_SOURCE_DIR}/kernel/syncOnAddress.cpp")
ps5_direct_test(lumen_ps5_direct_event_queue_test
    "${KYTY_TESTS_DIR}/EventQueueLifetimeTests.cpp"
    "${KYTY_SOURCE_DIR}/kernel/eventQueue.cpp"
    "${KYTY_SOURCE_DIR}/loader/timer.cpp")
ps5_direct_test(lumen_ps5_direct_host_fault_test
    "${KYTY_TESTS_DIR}/DirectHostFaultTests.cpp")

ps5_direct_test(lumen_ps5_direct_fiber_test
    "${KYTY_TESTS_DIR}/DirectFiberTests.cpp"
    "${KYTY_SOURCE_DIR}/loader/hostCall.cpp"
    "${KYTY_SOURCE_DIR}/loader/timer.cpp")

# Real shared backing/protection state and translator invalidation. No ROM.
ps5_direct_test(lumen_ps5_direct_guest_address_space_test
    "${KYTY_TESTS_DIR}/DirectGuestAddressSpaceTests.cpp")

# Exercise the production renderer through the same shared engine as the APK.
set(direct_gpu_test_dir "${PROJECT_BINARY_DIR}/gpu_test_shaders")
set(direct_gpu_test_header "${direct_gpu_test_dir}/gpu_test_ms_depth_spv.h")
add_custom_command(
    OUTPUT "${direct_gpu_test_header}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${direct_gpu_test_dir}"
    COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
        -o "${direct_gpu_test_dir}/gpu_test_ms_depth.spv"
        "${gpu_tiler_shader_dir}/gpu_test_ms_depth.comp"
    COMMAND ${CMAKE_COMMAND}
        -DINPUT=${direct_gpu_test_dir}/gpu_test_ms_depth.spv
        -DOUTPUT=${direct_gpu_test_header} -DSYMBOL=GPU_TEST_MS_DEPTH_SPV
        -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
    DEPENDS "${gpu_tiler_shader_dir}/gpu_test_ms_depth.comp"
    VERBATIM)
add_executable(lumen_ps5_direct_renderer_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/ShaderRecompilerComputeTests.cpp"
    "${direct_gpu_test_header}")
target_include_directories(lumen_ps5_direct_renderer_test PRIVATE
    ${inc_headers} "${PROJECT_BINARY_DIR}")
target_link_libraries(lumen_ps5_direct_renderer_test PRIVATE
    ${LUMEN_PS5_DIRECT_ENGINE_TARGET} ${kyty_emulator_link_libraries})
if(TARGET adrenotools)
    target_link_libraries(lumen_ps5_direct_renderer_test PRIVATE adrenotools)
endif()
target_compile_options(lumen_ps5_direct_renderer_test PRIVATE -fexceptions)
target_link_options(lumen_ps5_direct_renderer_test PRIVATE
    ${direct_test_link_options}
    ${direct_test_hide_options})

if(ANDROID)
    add_executable(lumen_ps5_direct_surface_lifecycle_test EXCLUDE_FROM_ALL
        "${KYTY_TESTS_DIR}/AndroidSurfaceLifecycleTests.cpp")
    target_include_directories(lumen_ps5_direct_surface_lifecycle_test PRIVATE ${inc_headers})
    target_link_libraries(lumen_ps5_direct_surface_lifecycle_test PRIVATE
        ${LUMEN_PS5_DIRECT_ENGINE_TARGET} ${kyty_emulator_link_libraries})
    target_compile_options(lumen_ps5_direct_surface_lifecycle_test PRIVATE -fexceptions)
    target_link_options(lumen_ps5_direct_surface_lifecycle_test PRIVATE
        ${direct_test_link_options} ${direct_test_hide_options})
endif()

add_executable(lumen_ps5_direct_services_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/DirectServicesTests.cpp")
target_include_directories(lumen_ps5_direct_services_test PRIVATE ${inc_headers})
target_include_directories(lumen_ps5_direct_services_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../direct_fex"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../android/app/src/main/cpp")
target_link_libraries(lumen_ps5_direct_services_test PRIVATE
    ${LUMEN_PS5_DIRECT_ENGINE_TARGET} ${kyty_emulator_link_libraries})
target_compile_options(lumen_ps5_direct_services_test PRIVATE -fexceptions)
target_link_options(lumen_ps5_direct_services_test PRIVATE
    ${direct_test_link_options})

add_executable(lumen_ps5_direct_ngs2_test EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/DirectNgs2Tests.cpp")
target_include_directories(lumen_ps5_direct_ngs2_test PRIVATE ${inc_headers})
target_link_libraries(lumen_ps5_direct_ngs2_test PRIVATE
    ${LUMEN_PS5_DIRECT_ENGINE_TARGET} ${kyty_emulator_link_libraries})
target_compile_options(lumen_ps5_direct_ngs2_test PRIVATE -fexceptions)
target_link_options(lumen_ps5_direct_ngs2_test PRIVATE
    ${direct_test_link_options})
