# SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.25)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 12.0 CACHE STRING "Minimum macOS target")
endif()
project(lumen_gen4 LANGUAGES C CXX ASM)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()
# This is a separate configure from the Android parent, so its optimized
# Debug profile does not propagate here. Keep native code and dependencies
# optimized while retaining Debug symbols, assertions and dependency ABI.
if(ANDROID)
    add_compile_options($<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C,CXX>>:-O2>)
endif()
file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../.." LUMEN_REPOSITORY_ROOT)
set(LUMEN_GEN4_DEPS_DIR "${LUMEN_REPOSITORY_ROOT}/../lumen-deps/gen4/sources" CACHE PATH "Pinned Gen4 dependency source directory")
set(LUMEN_GEN4_FFMPEG_PREBUILT_DIR "" CACHE PATH "Native FFmpeg static package")
set(LUMEN_GEN4_PARENT_BUILD "" CACHE PATH "Parent Lumen CPU and renderer build")
set(LUMEN_GEN4_ENTRY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp" CACHE STRING "Runtime entry point sources")
set(ENABLE_DISCORD_RPC OFF)
set(ENABLE_UPDATER OFF)
set(ENABLE_TESTS OFF)
set(ENABLE_LUMEN_HOST_RUNTIME OFF)
set(ENABLE_SYSTEM_VULKAN ON)
enable_testing()
if(CMAKE_OSX_ARCHITECTURES)
    set(BASE_ARCHITECTURE "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(BASE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(BASE_ARCHITECTURE MATCHES "^(arm64|aarch64)$")
    set(ARCHITECTURE arm64)
    set(ENABLE_FEX_GUEST_CPU ON)
elseif(BASE_ARCHITECTURE MATCHES "^(x86_64|AMD64|amd64)$")
    set(ARCHITECTURE x86_64)
    set(ENABLE_FEX_GUEST_CPU OFF)
else()
    message(FATAL_ERROR "Unsupported Gen4 architecture: ${BASE_ARCHITECTURE}")
endif()
include(cmake/Gen4ParentRuntime.cmake)
include(cmake/Gen4Dependencies.cmake)
add_subdirectory(externals)
include_directories(src)
if(APPLE)
    set(LUMEN_GEN4_MACOS_GPU_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos")
    include_directories(BEFORE "${LUMEN_GEN4_MACOS_GPU_ROOT}")
endif()
set(APP_VERSION "Lumen Gen4")
set(APP_IS_RELEASE false)
set(GIT_REV "C13")
set(GIT_BRANCH "main")
set(GIT_DESC "Lumen Gen4 runtime")
set(GIT_REMOTE_NAME "origin")
set(GIT_REMOTE_URL "")
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")
configure_file(src/common/scm_rev.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/src/common/scm_rev.cpp" @ONLY)

set(AJM_LIB src/core/libraries/ajm/ajm.cpp
            src/core/libraries/ajm/ajm.h
            src/core/libraries/ajm/ajm_aac.cpp
            src/core/libraries/ajm/ajm_aac.h
            src/core/libraries/ajm/ajm_at9.cpp
            src/core/libraries/ajm/ajm_at9.h
            src/core/libraries/ajm/ajm_batch.cpp
            src/core/libraries/ajm/ajm_batch.h
            src/core/libraries/ajm/ajm_context.cpp
            src/core/libraries/ajm/ajm_context.h
            src/core/libraries/ajm/ajm_error.h
            src/core/libraries/ajm/ajm_instance_statistics.cpp
            src/core/libraries/ajm/ajm_instance_statistics.h
            src/core/libraries/ajm/ajm_instance.cpp
            src/core/libraries/ajm/ajm_instance.h
            src/core/libraries/ajm/ajm_mp3.cpp
            src/core/libraries/ajm/ajm_mp3.h
)

set(AUDIO_LIB src/core/libraries/audio/audioin.cpp
              src/core/libraries/audio/audioin.h
              src/core/libraries/audio/audioin_backend.h
              src/core/libraries/audio/audioin_error.h
              src/core/libraries/audio/sdl_audio_in.cpp
              src/core/libraries/voice/voice.cpp
              src/core/libraries/voice/voice.h
              src/core/libraries/audio/audioout.cpp
              src/core/libraries/audio/audioout.h
              src/core/libraries/audio/audioout_backend.h
              src/core/libraries/audio/audioout_error.h
              src/core/libraries/audio/sdl_audio_out.cpp
              src/core/libraries/audio/openal_audio_out.cpp
              src/core/libraries/audio/openal_manager.h
              src/core/libraries/ngs2/ngs2.cpp
              src/core/libraries/ngs2/ngs2.h
              src/core/libraries/audio3d/audio3d.cpp
              src/core/libraries/audio3d/audio3d_openal.cpp
              src/core/libraries/audio3d/audio3d_openal.h
              src/core/libraries/audio3d/audio3d.h
              src/core/libraries/audio3d/audio3d_error.h
)

set(GNM_LIB src/core/libraries/gnmdriver/gnmdriver.cpp
            src/core/libraries/gnmdriver/gnmdriver.h
            src/core/libraries/gnmdriver/gnmdriver_init.h
            src/core/libraries/gnmdriver/gnm_error.h
)

set(KERNEL_LIB src/core/libraries/kernel/coredump/coredump.cpp
               src/core/libraries/kernel/umtx.cpp
               src/core/libraries/kernel/coredump/coredump.h
               src/core/libraries/kernel/coredump/coredump_error.h
               src/core/libraries/kernel/sync/mutex.cpp
               src/core/libraries/kernel/sync/mutex.h
               src/core/libraries/kernel/sync/semaphore.h
               src/core/libraries/kernel/threads/condvar.cpp
               src/core/libraries/kernel/threads/event_flag.cpp
               src/core/libraries/kernel/threads/exception.cpp
               src/core/libraries/kernel/threads/exception.h
               src/core/libraries/kernel/threads/mutex.cpp
               src/core/libraries/kernel/threads/pthread_attr.cpp
               src/core/libraries/kernel/threads/pthread_clean.cpp
               src/core/libraries/kernel/threads/pthread.cpp
               src/core/libraries/kernel/threads/pthread_spec.cpp
               src/core/libraries/kernel/threads/rwlock.cpp
               src/core/libraries/kernel/threads/semaphore.cpp
               src/core/libraries/kernel/threads/sleepq.cpp
               src/core/libraries/kernel/threads/sleepq.h
               src/core/libraries/kernel/threads/stack.cpp
               src/core/libraries/kernel/threads/tcb.cpp
               src/core/libraries/kernel/threads/pthread.h
               src/core/libraries/kernel/threads/thread_state.cpp
               src/core/libraries/kernel/threads/thread_state.h
               src/core/libraries/kernel/process.cpp
               src/core/libraries/kernel/process.h
               src/core/libraries/kernel/debug.cpp
               src/core/libraries/kernel/debug.h
               src/core/libraries/kernel/equeue.cpp
               src/core/libraries/kernel/equeue.h
               src/core/libraries/kernel/file_system.cpp
               src/core/libraries/kernel/file_system.h
               src/core/libraries/kernel/kernel.cpp
               src/core/libraries/kernel/kernel.h
               src/core/libraries/kernel/memory.cpp
               src/core/libraries/kernel/memory.h
               src/core/libraries/kernel/threads.cpp
               src/core/libraries/kernel/threads.h
               src/core/libraries/kernel/time.cpp
               src/core/libraries/kernel/time.h
               src/core/libraries/kernel/orbis_error.h
               src/core/libraries/kernel/posix_error.h
               src/core/libraries/kernel/aio.cpp
               src/core/libraries/kernel/aio.h
)

if (ARCHITECTURE STREQUAL "x86_64")
    list(APPEND KERNEL_LIB src/core/libraries/kernel/threads/stack.S)
    set_source_files_properties(src/core/libraries/kernel/threads/stack.S PROPERTIES COMPILE_OPTIONS -Wno-unused-command-line-argument)
endif()

set(NETWORK_LIBS src/core/libraries/network/http.cpp
                 src/core/libraries/np/np_lookup.cpp
                 src/core/libraries/np/np_in_game_message.cpp
                 src/core/libraries/np/np_utility.cpp
                 src/core/libraries/network/http.h
                 src/core/libraries/network/http_error.h
                 src/core/libraries/network/http2.cpp
                 src/core/libraries/network/http2.h
                 src/core/libraries/network/net.cpp
                 src/core/libraries/network/netctl.cpp
                 src/core/libraries/network/netctl.h
                 src/core/libraries/network/net_ctl_obj.cpp
                 src/core/libraries/network/net_ctl_obj.h
                 src/core/libraries/network/net_ctl_codes.h
                 src/core/libraries/network/net_util.cpp
                 src/core/libraries/network/net_util.h
                 src/core/libraries/network/net_epoll.cpp
                 src/core/libraries/network/net_epoll.h
                 src/core/libraries/network/net_resolver.cpp
                 src/core/libraries/network/net_resolver.h
                 src/core/libraries/network/net_upnp.cpp
                 src/core/libraries/network/net_upnp.h
                 src/core/libraries/network/net_error.h
                 src/core/libraries/network/net.h
                 src/core/libraries/network/ssl.cpp
                 src/core/libraries/network/ssl.h
                 src/core/libraries/network/ssl2.cpp
                 src/core/libraries/network/ssl2.h
                 src/core/libraries/network/sys_net.cpp
                 src/core/libraries/network/sys_net.h
                 src/core/libraries/network/posix_sockets.cpp
                 src/core/libraries/network/p2p_sockets.cpp
                 src/core/libraries/network/unix_sockets.cpp
                 src/core/libraries/network/sockets.h
)

set(AVPLAYER_LIB src/core/libraries/avplayer/avplayer_common.cpp
                 src/core/libraries/avplayer/avplayer_common.h
                 src/core/libraries/avplayer/avplayer_file_streamer.cpp
                 src/core/libraries/avplayer/avplayer_file_streamer.h
                 src/core/libraries/avplayer/avplayer_impl.cpp
                 src/core/libraries/avplayer/avplayer_impl.h
                 src/core/libraries/avplayer/avplayer_source.cpp
                 src/core/libraries/avplayer/avplayer_source.h
                 src/core/libraries/avplayer/avplayer_state.cpp
                 src/core/libraries/avplayer/avplayer_state.h
                 src/core/libraries/avplayer/avplayer.cpp
                 src/core/libraries/avplayer/avplayer.h
                 src/core/libraries/avplayer/avplayer_error.h
)

set(SYSTEM_LIBS src/core/libraries/system/commondialog.cpp
                src/core/libraries/system/commondialog.h
                src/core/libraries/system/msgdialog.cpp
                src/core/libraries/system/msgdialog.h
                src/core/libraries/system/msgdialog_ui.cpp
                src/core/libraries/system/posix.cpp
                src/core/libraries/system/posix.h
                src/core/libraries/save_data/save_backup.cpp
                src/core/libraries/save_data/save_backup.h
                src/core/libraries/save_data/save_instance.cpp
                src/core/libraries/save_data/save_instance.h
                src/core/libraries/save_data/save_memory.cpp
                src/core/libraries/save_data/save_memory.h
                src/core/libraries/save_data/savedata.cpp
                src/core/libraries/save_data/savedata.h
                src/core/libraries/save_data/savedata_error.h
                src/core/libraries/save_data/dialog/savedatadialog.cpp
                src/core/libraries/save_data/dialog/savedatadialog.h
                src/core/libraries/save_data/dialog/savedatadialog_ui.cpp
                src/core/libraries/save_data/dialog/savedatadialog_ui.h
                src/core/libraries/sysmodule/sysmodule.cpp
                src/core/libraries/sysmodule/sysmodule.h
                src/core/libraries/sysmodule/sysmodule_internal.cpp
                src/core/libraries/sysmodule/sysmodule_internal.h
                src/core/libraries/sysmodule/sysmodule_error.h
                src/core/libraries/sysmodule/sysmodule_table.h
                src/core/libraries/system/systemservice.cpp
                src/core/libraries/system/systemservice.h
                src/core/libraries/system/systemservice_error.h
                src/core/libraries/system/userservice.cpp
                src/core/libraries/system/userservice.h
                src/core/libraries/system/userservice_error.h
                src/core/libraries/app_content/app_content.cpp
                src/core/libraries/app_content/app_content.h
                src/core/libraries/app_content/app_content_error.h
                src/core/libraries/rtc/rtc.cpp
                src/core/libraries/rtc/rtc.h
                src/core/libraries/rtc/rtc_error.h
                src/core/libraries/rudp/rudp.cpp
                src/core/libraries/rudp/rudp.h
                src/core/libraries/disc_map/disc_map.cpp
                src/core/libraries/disc_map/disc_map.h
                src/core/libraries/disc_map/disc_map_codes.h
                src/core/libraries/ngs2/ngs2.cpp
                src/core/libraries/ngs2/ngs2.h
                src/core/libraries/ngs2/ngs2_error.h
                src/core/libraries/ngs2/ngs2_impl.cpp
                src/core/libraries/ngs2/ngs2_impl.h
                src/core/libraries/ngs2/ngs2_custom.cpp
                src/core/libraries/ngs2/ngs2_custom.h
                src/core/libraries/ngs2/ngs2_reverb.cpp
                src/core/libraries/ngs2/ngs2_reverb.h
                src/core/libraries/ngs2/ngs2_geom.cpp
                src/core/libraries/ngs2/ngs2_geom.h
                src/core/libraries/ngs2/ngs2_pan.cpp
                src/core/libraries/ngs2/ngs2_pan.h
                src/core/libraries/ngs2/ngs2_report.cpp
                src/core/libraries/ngs2/ngs2_report.h
                src/core/libraries/ngs2/ngs2_eq.cpp
                src/core/libraries/ngs2/ngs2_eq.h
                src/core/libraries/ngs2/ngs2_mastering.cpp
                src/core/libraries/ngs2/ngs2_mastering.h
                src/core/libraries/ngs2/ngs2_sampler.cpp
                src/core/libraries/ngs2/ngs2_sampler.h
                src/core/libraries/ngs2/ngs2_submixer.cpp
                src/core/libraries/ngs2/ngs2_submixer.h
                src/core/libraries/ajm/ajm_error.h
                src/core/libraries/audio3d/audio3d.cpp
                src/core/libraries/audio3d/audio3d.h
                src/core/libraries/audio3d/audio3d_error.h
                src/core/libraries/game_live_streaming/gamelivestreaming.cpp
                src/core/libraries/game_live_streaming/gamelivestreaming.h
                src/core/libraries/remote_play/remoteplay.cpp
                src/core/libraries/remote_play/remoteplay.h
                src/core/libraries/share_play/shareplay.cpp
                src/core/libraries/share_play/shareplay.h
                src/core/libraries/razor_cpu/razor_cpu.cpp
                src/core/libraries/razor_cpu/razor_cpu.h
                src/core/libraries/mouse/mouse.cpp
                src/core/libraries/mouse/mouse.h
                src/core/libraries/mouse/mouse_error.h
                src/core/libraries/mouse/sdl_mouse.cpp
                src/core/libraries/mouse/sdl_mouse.h
                src/core/libraries/web_browser_dialog/webbrowserdialog.cpp
                src/core/libraries/web_browser_dialog/webbrowserdialog.h
                src/core/libraries/font/font.cpp
                src/core/libraries/font/font.h
                src/core/libraries/fios2/fios2.cpp
                src/core/libraries/fios2/fios2.h
                src/core/libraries/font/font_internal.cpp
                src/core/libraries/font/font_internal.h
                src/core/libraries/font/fontft.cpp
                src/core/libraries/font/fontft.h
                src/core/libraries/font/fontft_internal.cpp
                src/core/libraries/font/fontft_internal.h
                src/core/libraries/font/font_error.h
                src/core/libraries/content_export/content_export.cpp
                src/core/libraries/content_export/content_export.h
                src/core/libraries/content_export/content_export_error.h
                src/core/libraries/video_recording/video_recording.cpp
                src/core/libraries/video_recording/video_recording.h
                src/core/libraries/video_recording/video_recording_error.h
)

set(VIDEOOUT_LIB src/core/libraries/videoout/buffer.h
                 src/core/libraries/videoout/driver.cpp
                 src/core/libraries/videoout/driver.h
                 src/core/libraries/videoout/video_out.cpp
                 src/core/libraries/videoout/video_out.h
                 src/core/libraries/videoout/videoout_error.h
)

set(HLE_LIBC_INTERNAL_LIB src/core/libraries/libc_internal/libc_internal.cpp
                 src/core/libraries/libc_internal/libc_internal.h
                 src/core/libraries/libc_internal/libc_internal_io.cpp
                 src/core/libraries/libc_internal/libc_internal_io.h
                 src/core/libraries/libc_internal/libc_internal_cxa.cpp
                 src/core/libraries/libc_internal/libc_internal_cxa.h
                 src/core/libraries/libc_internal/libc_internal_memory.cpp
                 src/core/libraries/libc_internal/libc_internal_memory.h
                 src/core/libraries/libc_internal/mspace.cpp
                 src/core/libraries/libc_internal/mspace.h
                 src/core/libraries/libc_internal/libc_internal_str.cpp
                 src/core/libraries/libc_internal/libc_internal_str.h
                 src/core/libraries/libc_internal/libc_internal_math.cpp
                 src/core/libraries/libc_internal/libc_internal_math.h
                 src/core/libraries/libc_internal/libc_internal_threads.cpp
                 src/core/libraries/libc_internal/libc_internal_threads.h
                 src/core/libraries/libc_internal/printf.h
)

set(IME_LIB src/core/libraries/ime/error_dialog.cpp
            src/core/libraries/ime/error_dialog.h
            src/core/libraries/ime/ime_common.h
            src/core/libraries/ime/ime_kb_layout.cpp
            src/core/libraries/ime/ime_kb_layout.h
            src/core/libraries/ime/ime_dialog_ui.cpp
            src/core/libraries/ime/ime_dialog_ui.h
            src/core/libraries/ime/ime_dialog.cpp
            src/core/libraries/ime/ime_dialog.h
            src/core/libraries/ime/ime_ui.cpp
            src/core/libraries/ime/ime_ui.h
            src/core/libraries/ime/ime_ui_shared.cpp
            src/core/libraries/ime/ime_ui_shared.h
            src/core/libraries/ime/ime.cpp
            src/core/libraries/ime/ime.h
            src/core/libraries/ime/ime_error.h
)

set(PAD_LIB src/core/libraries/pad/pad.cpp
            src/core/libraries/pad/pad.h
            src/core/libraries/pad/pad_errors.h
)

set(SYSTEM_GESTURE_LIB
            src/core/libraries/system_gesture/system_gesture.cpp
            src/core/libraries/system_gesture/system_gesture.h
)

set(PNG_LIB src/core/libraries/libpng/pngdec.cpp
            src/core/libraries/libpng/pngdec.h
            src/core/libraries/libpng/pngdec_error.h
            src/core/libraries/libpng/pngenc.cpp
            src/core/libraries/libpng/pngenc.h
            src/core/libraries/libpng/pngenc_error.h
)

set(JPEG_LIB src/core/libraries/jpeg/jpeg_error.h
             src/core/libraries/jpeg/jpegdec.cpp
             src/core/libraries/jpeg/jpegdec.h
             src/core/libraries/jpeg/jpegenc.cpp
             src/core/libraries/jpeg/jpegenc.h
)

set(PLAYGO_LIB src/core/libraries/playgo/playgo.cpp
               src/core/libraries/playgo/playgo.h
               src/core/libraries/playgo/playgo_dialog.cpp
               src/core/libraries/playgo/playgo_dialog.h
               src/core/libraries/playgo/playgo_types.h
)

set(RANDOM_LIB src/core/libraries/random/random.cpp
               src/core/libraries/random/random.h
               src/core/libraries/random/random_error.h
)

set(USBD_LIB src/core/libraries/usbd/usbd.cpp
             src/core/libraries/usbd/usbd.h
             src/core/libraries/usbd/usb_backend.h
             src/core/libraries/usbd/usbd_event_loop.h
             src/core/libraries/usbd/emulated/dimensions.cpp
             src/core/libraries/usbd/emulated/dimensions.h
             src/core/libraries/usbd/emulated/infinity.cpp
             src/core/libraries/usbd/emulated/infinity.h
             src/core/libraries/usbd/emulated/skylander.cpp
             src/core/libraries/usbd/emulated/skylander.h
)

set(FIBER_LIB src/core/libraries/fiber/fiber_context.s
              src/core/libraries/fiber/fiber.cpp
              src/core/libraries/fiber/fiber.h
              src/core/libraries/fiber/fiber_error.h
)

set_source_files_properties(src/core/libraries/fiber/fiber_context.s PROPERTIES COMPILE_OPTIONS -Wno-unused-command-line-argument)

set(VDEC_LIB src/core/libraries/videodec/videodec2_impl.cpp
             src/core/libraries/videodec/videodec2_impl.h
             src/core/libraries/videodec/videodec2.cpp
             src/core/libraries/videodec/videodec2.h
             src/core/libraries/videodec/videodec2_avc.h
             src/core/libraries/videodec/videodec.cpp
             src/core/libraries/videodec/videodec.h
             src/core/libraries/videodec/videodec_error.h
             src/core/libraries/videodec/videodec_impl.cpp
             src/core/libraries/videodec/videodec_impl.h
)

set(NP_LIBS src/core/libraries/np/np_error.h
            src/core/libraries/np/np_common.cpp
            src/core/libraries/np/np_common.h
            src/core/libraries/np/np_commerce.cpp
            src/core/libraries/np/np_commerce.h
            src/core/libraries/np/np_manager.cpp
            src/core/libraries/np/np_manager.h
            src/core/libraries/np/np_matching2.cpp
            src/core/libraries/np/np_matching2_compat.cpp
            src/core/libraries/np/np_matching2.h
            src/core/libraries/np/np_signaling/np_signaling.cpp
            src/core/libraries/np/np_signaling/np_signaling.h
            src/core/libraries/np/np_signaling/np_signaling_state.cpp
            src/core/libraries/np/np_signaling/np_signaling_state.h
            src/core/libraries/np/np_signaling/np_signaling_helpers.cpp
            src/core/libraries/np/np_signaling/np_signaling_helpers.h
            src/core/libraries/np/np_signaling/np_signaling_stubs.cpp
            src/core/libraries/np/np_signaling/np_signaling_stubs.h
            src/core/libraries/np/np_trophy.cpp
            src/core/libraries/np/np_trophy.h
            src/core/libraries/np/np_tus.cpp
            src/core/libraries/np/np_tus.h
            src/core/libraries/np/trophy_ui.cpp
            src/core/libraries/np/trophy_ui.h
            src/core/libraries/np/np_web_api2.cpp
            src/core/libraries/np/np_web_api2.h
            src/core/libraries/np/np_party.cpp
            src/core/libraries/np/np_party.h
            src/core/libraries/np/np_auth.cpp
            src/core/libraries/np/np_auth.h
            src/core/libraries/np/np_profile_dialog/np_profile_dialog.cpp
            src/core/libraries/np/np_profile_dialog/np_profile_dialog.h
            src/core/libraries/np/np_profile_dialog/np_profile_dialog_ui.cpp
            src/core/libraries/np/np_profile_dialog/np_profile_dialog_ui.h
            src/core/libraries/np/np_sns_facebook_dialog.cpp
            src/core/libraries/np/np_sns_facebook_dialog.h
            src/core/libraries/np/np_partner.cpp
            src/core/libraries/np/np_partner.h
            src/core/libraries/np/object_manager.h
            src/core/libraries/np/np_handler.cpp
            src/core/libraries/np/np_handler.h
            src/core/libraries/np/np_score/np_score.cpp
            src/core/libraries/np/np_score/np_score.h
            src/core/libraries/np/np_score/np_score_ctx.h
            src/core/libraries/np/np_web_api/np_web_api.cpp
            src/core/libraries/np/np_web_api/np_web_api.h
            src/core/libraries/np/np_web_api/np_web_api_internal.cpp
            src/core/libraries/np/np_web_api/np_web_api_internal.h
)

set(ZLIB_LIB src/core/libraries/zlib/zlib.cpp
             src/core/libraries/zlib/zlib_sce.h
             src/core/libraries/zlib/zlib_error.h
)

set(VR_LIBS  src/core/libraries/hmd/hmd.cpp
             src/core/libraries/hmd/hmd_reprojection.cpp
             src/core/libraries/hmd/hmd_distortion.cpp
             src/core/libraries/hmd/hmd.h
             src/core/libraries/hmd/hmd_setup_dialog.cpp
             src/core/libraries/hmd/hmd_setup_dialog.h
             src/core/libraries/vr_tracker/vr_tracker.cpp
             src/core/libraries/vr_tracker/vr_tracker.h
)

set(MISC_LIBS  src/core/libraries/screenshot/screenshot.cpp
               src/core/libraries/screenshot/screenshot.h
               src/core/libraries/move/move.cpp
               src/core/libraries/move/move.h
               src/core/libraries/move/move_error.h
               src/core/libraries/ulobjmgr/ulobjmgr.cpp
               src/core/libraries/ulobjmgr/ulobjmgr.h
               src/core/libraries/signin_dialog/signindialog.cpp
               src/core/libraries/signin_dialog/signindialog.h
)

set(CAMERA_LIBS  src/core/libraries/camera/camera.cpp
                 src/core/libraries/camera/camera.h
                 src/core/libraries/camera/camera_error.h
)

set(COMPANION_LIBS  src/core/libraries/companion/companion_httpd.cpp
                    src/core/libraries/companion/companion_httpd.h
                    src/core/libraries/companion/companion_util.cpp
                    src/core/libraries/companion/companion_util.h
                    src/core/libraries/companion/companion_error.h
)
set(DEV_TOOLS src/core/devtools/layer.cpp
              src/core/devtools/layer.h
              src/core/devtools/layer_extra.cpp
              src/core/devtools/options.cpp
              src/core/devtools/options.h
              src/core/devtools/gcn/gcn_context_regs.cpp
              src/core/devtools/gcn/gcn_op_names.cpp
              src/core/devtools/gcn/gcn_shader_regs.cpp
              src/core/devtools/widget/cmd_list.cpp
              src/core/devtools/widget/cmd_list.h
              src/core/devtools/widget/common.h
              src/core/devtools/widget/frame_dump.cpp
              src/core/devtools/widget/frame_dump.h
              src/core/devtools/widget/frame_graph.cpp
              src/core/devtools/widget/frame_graph.h
              src/core/devtools/widget/imgui_memory_editor.h
              src/core/devtools/widget/memory_map.cpp
              src/core/devtools/widget/memory_map.h
              src/core/devtools/widget/module_list.cpp
              src/core/devtools/widget/module_list.h
              src/core/devtools/widget/reg_popup.cpp
              src/core/devtools/widget/reg_popup.h
              src/core/devtools/widget/reg_view.cpp
              src/core/devtools/widget/reg_view.h
              src/core/devtools/widget/shader_list.cpp
              src/core/devtools/widget/shader_list.h
              src/core/devtools/widget/text_editor.cpp
              src/core/devtools/widget/text_editor.h
)

set(COMMON src/common/logging/classes.h
           src/common/logging/formatter.h
           src/common/logging/log.cpp
           src/common/logging/log.h
           src/common/logging/log_file_sink.h
           src/common/aes.h
           src/common/alignment.h
           src/common/arch.h
           src/common/assert.cpp
           src/common/assert.h
           src/common/bit_array.h
           src/common/bit_field.h
           src/common/bounded_threadsafe_queue.h
           src/common/concepts.h
           src/common/cstring.h
           src/common/debug.h
           src/common/decoder.cpp
           src/common/decoder.h
           src/common/elf_info.h
           src/common/endian.h
           src/common/enum.h
           src/common/incremental_id.h
           src/common/io_file.cpp
           src/common/io_file.h
           src/common/lru_cache.h
           src/common/error.cpp
           src/common/error.h
           src/common/fixed_value.h
           src/common/func_traits.h
           src/common/native_clock.cpp
           src/common/native_clock.h
           src/common/path_util.cpp
           src/common/path_util.h
           src/common/object_pool.h
           src/common/polyfill_thread.h
           src/common/range_lock.h
           src/common/rdtsc.cpp
           src/common/rdtsc.h
           src/common/recursive_lock.cpp
           src/common/recursive_lock.h
           src/common/scope_exit.h
           src/common/serdes.h
           src/common/sha1.h
           src/common/shared_first_mutex.h
           src/common/signal_context.h
           src/common/signal_context.cpp
           src/common/crash_reporter.h
           src/common/crash_reporter.cpp
           src/common/singleton.h
           src/common/slab_heap.h
           src/common/slot_vector.h
           src/common/spin_lock.cpp
           src/common/spin_lock.h
           src/common/stb.cpp
           src/common/stb.h
           src/common/string_literal.h
           src/common/string_util.cpp
           src/common/string_util.h
           src/common/thread.cpp
           src/common/thread.h
           src/common/types.h
           src/common/uint128.h
           src/common/unique_function.h
           src/common/va_ctx.h
           src/common/ntapi.h
           src/common/ntapi.cpp
           src/common/number_utils.h
           src/common/number_utils.cpp
           src/common/memory_patcher.h
           src/common/memory_patcher.cpp
           src/common/patch_repository.h
           src/common/patch_repository.cpp
           src/common/patch_session.h
           src/common/patch_session.cpp
           src/common/patch_state.h
           src/common/patch_state.cpp
           src/common/patch_preset.h
           src/common/patch_preset.cpp
           src/common/sha256.h
           ${CMAKE_CURRENT_BINARY_DIR}/src/common/scm_rev.cpp
           src/common/scm_rev.h
           src/common/key_manager.cpp
           src/common/key_manager.h
)

if (ENABLE_DISCORD_RPC)
    list(APPEND COMMON src/common/discord_rpc_handler.cpp src/common/discord_rpc_handler.h)
endif()

set(CORE src/core/aerolib/stubs.cpp
         src/core/aerolib/stubs.h
         src/core/aerolib/aerolib.cpp
         src/core/aerolib/aerolib.h
         src/core/address_space.cpp
         src/core/address_space.h
         src/core/file_sys/devices/base_device.cpp
         src/core/file_sys/devices/base_device.h
         src/core/file_sys/devices/ioccom.h
         src/core/file_sys/devices/logger.cpp
         src/core/file_sys/devices/logger.h
         src/core/file_sys/devices/nop_device.h
         src/core/file_sys/devices/console_device.cpp
         src/core/file_sys/devices/console_device.h
         src/core/file_sys/devices/deci_tty6_device.cpp
         src/core/file_sys/devices/deci_tty6_device.h
         src/core/file_sys/devices/random_device.cpp
         src/core/file_sys/devices/random_device.h
         src/core/file_sys/devices/rng_device.cpp
         src/core/file_sys/devices/rng_device.h
         src/core/file_sys/devices/urandom_device.cpp
         src/core/file_sys/devices/urandom_device.h
         src/core/file_sys/devices/srandom_device.cpp
         src/core/file_sys/devices/srandom_device.h
         src/core/file_sys/directories/base_directory.cpp
         src/core/file_sys/directories/base_directory.h
         src/core/file_sys/directories/normal_directory.cpp
         src/core/file_sys/directories/normal_directory.h
         src/core/file_sys/directories/pfs_directory.cpp
         src/core/file_sys/directories/pfs_directory.h
         src/core/file_format/pfs.h
         src/core/file_format/psf.cpp
         src/core/file_format/psf.h
         src/core/file_format/playgo_chunk.cpp
         src/core/file_format/playgo_chunk.h
         src/core/file_format/trp.cpp
         src/core/file_format/trp.h
         src/core/file_format/npbind.cpp
         src/core/file_format/npbind.h
         src/core/file_sys/fs.cpp
         src/core/file_sys/fs.h
         src/core/ipc/ipc.cpp
         src/core/ipc/ipc.h
         src/core/loader/dwarf.cpp
         src/core/loader/dwarf.h
         src/core/loader/compiled_sdk.cpp
         src/core/loader/compiled_sdk.h
         src/core/loader/elf.cpp
         src/core/loader/elf.h
         src/core/loader/symbols_resolver.h
         src/core/loader/symbols_resolver.cpp
         src/core/libraries/libs.h
         src/core/libraries/libs.cpp
         ${AJM_LIB}
         ${AVPLAYER_LIB}
         ${AUDIO_LIB}
         ${GNM_LIB}
         ${KERNEL_LIB}
         ${NETWORK_LIBS}
         ${SYSTEM_LIBS}
         ${HLE_LIBC_INTERNAL_LIB}
         ${PAD_LIB}
         ${SYSTEM_GESTURE_LIB}
         ${VIDEOOUT_LIB}
         ${NP_LIBS}
         ${PNG_LIB}
         ${JPEG_LIB}
         ${PLAYGO_LIB}
         ${RANDOM_LIB}
         ${USBD_LIB}
         ${ZLIB_LIB}
         ${MISC_LIBS}
         ${IME_LIB}
         ${VDEC_LIB}
         ${VR_LIBS}
         ${CAMERA_LIBS}
          ${COMPANION_LIBS}
          ${DEV_TOOLS}
          src/core/debug_state.cpp
          src/core/debug_state.h
          src/core/debugger.cpp
          src/core/debugger.h
          src/core/guest_cpu/guest_cpu.cpp
          src/core/guest_cpu/guest_cpu.h
          src/core/linker.cpp
         src/core/linker.h
         src/core/memory.cpp
         src/core/memory.h
         src/core/module.cpp
         src/core/module.h
         src/core/platform.h
         src/core/signals.cpp
         src/core/signals.h
         src/core/thread.cpp
         src/core/thread.h
         src/core/tls.cpp
         src/core/tls.h
         src/core/emulator_state.cpp
         src/core/emulator_state.h
         src/core/emulator_settings.cpp
         src/core/emulator_settings.h
         src/core/user_manager.cpp
         src/core/user_manager.h
         src/core/user_settings.cpp
         src/core/user_settings.h
)

if (ARCHITECTURE STREQUAL "x86_64")
    set(CORE ${CORE}
             src/core/cpu_patches.cpp
             src/core/cpu_patches.h
             ${FIBER_LIB})
endif()

if(ENABLE_FEX_GUEST_CPU)
    list(REMOVE_ITEM CORE src/core/file_sys/fs.cpp src/core/libraries/kernel/file_system.cpp)
    list(APPEND CORE
        src/core/file_sys/fs_direct.cpp
        src/core/file_sys/ifile.cpp
        src/core/file_sys/backends/host_fs.cpp
        src/core/file_sys/backends/zarchive_fs.cpp
        src/core/file_sys/devices/deci_tty_device.cpp
        src/core/file_sys/devices/zero_device.cpp
        src/core/libraries/kernel/file_system_direct.cpp
        src/core/libraries/ngs2/ngs2_waveform_direct.cpp)
    list(REMOVE_ITEM CORE src/core/libraries/kernel/equeue.cpp)
    list(APPEND CORE src/core/libraries/kernel/equeue_direct.cpp)
    list(REMOVE_ITEM CORE src/core/libraries/kernel/threads/event_flag.cpp)
    list(APPEND CORE src/core/libraries/kernel/threads/event_flag_direct.cpp)
    list(REMOVE_ITEM CORE src/core/libraries/kernel/threads/semaphore.cpp)
    list(APPEND CORE src/core/libraries/kernel/threads/semaphore_direct.cpp)
    foreach(sync_unit condvar mutex sleepq)
        list(REMOVE_ITEM CORE src/core/libraries/kernel/threads/${sync_unit}.cpp)
        list(APPEND CORE src/core/libraries/kernel/threads/${sync_unit}_direct.cpp)
    endforeach()
    foreach(ajm_unit ajm ajm_context ajm_batch ajm_at9)
        list(REMOVE_ITEM CORE src/core/libraries/ajm/${ajm_unit}.cpp)
        list(APPEND CORE src/core/libraries/ajm/${ajm_unit}_direct.cpp)
    endforeach()
    list(REMOVE_ITEM CORE src/core/libraries/fios2/fios2.cpp)
    list(APPEND CORE src/core/libraries/fios2/fios2_direct.cpp)
    list(REMOVE_ITEM CORE src/core/libraries/kernel/threads/exception.cpp)
    list(APPEND CORE
        src/core/libraries/kernel/threads/exception_direct.cpp
        src/core/libraries/kernel/threads/exception_direct.h
        src/core/libraries/kernel/threads/exception_codes.h)
    list(APPEND CORE
        src/core/libraries/fiber/fiber_fex.cpp
        src/core/libraries/fiber/fiber.h
        src/core/libraries/fiber/fiber_error.h)
endif()

set(LUMEN_GEN4_FEX_ENGINE_SOURCE src/core/fex/fex_guest_engine_direct.cpp)

if (ENABLE_FEX_GUEST_CPU)
    list(APPEND CORE
        ${LUMEN_GEN4_FEX_ENGINE_SOURCE}
        src/core/fex/fex_guest_engine.h
        src/core/guest_cpu/fex_guest_cpu.cpp
        src/core/guest_cpu/fex_guest_cpu.h
        src/core/guest_cpu/fex_hle_bridge.cpp
        src/core/guest_cpu/fex_hle_bridge.h
        src/core/guest_cpu/guest_callback.h
        src/core/guest_cpu/hle_call_adapter.cpp
        src/core/guest_cpu/hle_call_adapter.h
    )
endif()

set(SHADER_RECOMPILER src/shader_recompiler/profile.h
                      src/shader_recompiler/recompiler.cpp
                      src/shader_recompiler/recompiler.h
                      src/shader_recompiler/resource.h
                      src/shader_recompiler/info.h
                      src/shader_recompiler/params.h
                      src/shader_recompiler/runtime_info.h
                      src/shader_recompiler/specialization.h
                      src/shader_recompiler/backend/bindings.h
                      src/shader_recompiler/backend/spirv/emit_spirv.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv.h
                      src/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_barriers.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_bitwise_conversion.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_composite.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_convert.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_floating_point.cpp
                      src/shader_recompiler/backend/spirv/fma_split_diag.h
                      src/shader_recompiler/backend/spirv/emit_spirv_image.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_instructions.h
                      src/shader_recompiler/backend/spirv/emit_spirv_integer.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_logical.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_quad_rect.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_rect_list.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_quad_rect.h
                      src/shader_recompiler/backend/spirv/emit_spirv_select.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_shared_memory.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_special.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_undefined.cpp
                      src/shader_recompiler/backend/spirv/emit_spirv_warp.cpp
                      src/shader_recompiler/backend/spirv/spirv_emit_context.cpp
                      src/shader_recompiler/backend/spirv/spirv_emit_context.h
                      src/shader_recompiler/frontend/translate/data_share.cpp
                      src/shader_recompiler/frontend/translate/export.cpp
                      src/shader_recompiler/frontend/translate/scalar_alu.cpp
                      src/shader_recompiler/frontend/translate/scalar_flow.cpp
                      src/shader_recompiler/frontend/translate/scalar_memory.cpp
                      src/shader_recompiler/frontend/translate/translate.cpp
                      src/shader_recompiler/frontend/translate/translate.h
                      src/shader_recompiler/frontend/translate/vector_alu.cpp
                      src/shader_recompiler/frontend/translate/vector_interpolation.cpp
                      src/shader_recompiler/frontend/translate/vector_memory.cpp
                      src/shader_recompiler/frontend/control_flow_graph.cpp
                      src/shader_recompiler/frontend/control_flow_graph.h
                      src/shader_recompiler/frontend/copy_shader.cpp
                      src/shader_recompiler/frontend/copy_shader.h
                      src/shader_recompiler/frontend/decode.cpp
                      src/shader_recompiler/frontend/decode.h
                      src/shader_recompiler/frontend/fetch_shader.cpp
                      src/shader_recompiler/frontend/fetch_shader.h
                      src/shader_recompiler/frontend/format.cpp
                      src/shader_recompiler/frontend/instruction.cpp
                      src/shader_recompiler/frontend/instruction.h
                      src/shader_recompiler/frontend/opcodes.h
                      src/shader_recompiler/frontend/structured_control_flow.cpp
                      src/shader_recompiler/frontend/structured_control_flow.h
                      src/shader_recompiler/ir/passes/constant_propagation_pass.cpp
                      src/shader_recompiler/ir/passes/dead_code_elimination_pass.cpp
                      src/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp
                      src/shader_recompiler/ir/passes/hull_shader_transform.cpp
                      src/shader_recompiler/ir/passes/identity_removal_pass.cpp
                      src/shader_recompiler/ir/passes/inject_clip_distance_attributes.cpp
                      src/shader_recompiler/ir/passes/ir_passes.h
                      src/shader_recompiler/ir/passes/lower_buffer_format_to_raw.cpp
                      src/shader_recompiler/ir/passes/lower_fp64_to_fp32.cpp
                      src/shader_recompiler/ir/passes/readlane_elimination_pass.cpp
                      src/shader_recompiler/ir/passes/resource_discover_pass.cpp
                        src/shader_recompiler/ir/passes/resource_patching_pass.cpp
                        src/shader_recompiler/ir/passes/resource_pass.h
                      src/shader_recompiler/ir/passes/ring_access_elimination.cpp
                      src/shader_recompiler/ir/passes/shader_info_collection_pass.cpp
                      src/shader_recompiler/ir/passes/shared_memory_barrier_pass.cpp
                      src/shader_recompiler/ir/passes/shared_memory_simplify_pass.cpp
                      src/shader_recompiler/ir/passes/shared_memory_to_storage_pass.cpp
                      src/shader_recompiler/ir/passes/ssa_rewrite_pass.cpp
                      src/shader_recompiler/ir/abstract_syntax_list.cpp
                      src/shader_recompiler/ir/abstract_syntax_list.h
                      src/shader_recompiler/ir/attribute.cpp
                      src/shader_recompiler/ir/attribute.h
                      src/shader_recompiler/ir/basic_block.cpp
                      src/shader_recompiler/ir/basic_block.h
                      src/shader_recompiler/ir/breadth_first_search.h
                      src/shader_recompiler/ir/condition.h
                      src/shader_recompiler/ir/ir_emitter.cpp
                      src/shader_recompiler/ir/ir_emitter.h
                      src/shader_recompiler/ir/microinstruction.cpp
                      src/shader_recompiler/ir/opcodes.cpp
                      src/shader_recompiler/ir/opcodes.h
                      src/shader_recompiler/ir/opcodes.inc
                      src/shader_recompiler/ir/operand_helper.h
                      src/shader_recompiler/ir/patch.cpp
                      src/shader_recompiler/ir/patch.h
                      src/shader_recompiler/ir/position.h
                      src/shader_recompiler/ir/post_order.cpp
                      src/shader_recompiler/ir/post_order.h
                      src/shader_recompiler/ir/program.cpp
                      src/shader_recompiler/ir/program.h
                      src/shader_recompiler/ir/reinterpret.h
                      src/shader_recompiler/ir/reg.h
                      src/shader_recompiler/ir/type.cpp
                      src/shader_recompiler/ir/type.h
                      src/shader_recompiler/ir/value.cpp
                      src/shader_recompiler/ir/value.h
)

set(VIDEO_CORE src/video_core/amdgpu/cb_db_extent.h
               src/video_core/amdgpu/command_buffer_copy.h
               src/video_core/amdgpu/liverpool.cpp
               src/video_core/amdgpu/liverpool.h
               src/video_core/amdgpu/pixel_format.cpp
               src/video_core/amdgpu/pixel_format.h
               src/video_core/amdgpu/pm4_cmds.h
               src/video_core/amdgpu/pm4_opcodes.h
               src/video_core/amdgpu/regs_color.h
               src/video_core/amdgpu/regs_depth.h
               src/video_core/amdgpu/regs.cpp
               src/video_core/amdgpu/regs.h
               src/video_core/amdgpu/regs_primitive.h
               src/video_core/amdgpu/regs_shader.h
               src/video_core/amdgpu/regs_texture.h
               src/video_core/amdgpu/regs_vertex.h
               src/video_core/amdgpu/resource.h
               src/video_core/amdgpu/tiling.cpp
               src/video_core/amdgpu/tiling.h
               src/video_core/buffer_cache/buffer.cpp
               src/video_core/buffer_cache/buffer.h
               src/video_core/buffer_cache/buffer_cache.cpp
               src/video_core/buffer_cache/buffer_cache.h
               src/video_core/buffer_cache/vk_dispatch_reload.cpp
               src/video_core/buffer_cache/fault_manager.cpp
               src/video_core/buffer_cache/fault_manager.h
               src/video_core/buffer_cache/memory_tracker.h
               src/video_core/buffer_cache/range_set.h
               src/video_core/buffer_cache/region_definitions.h
               src/video_core/buffer_cache/region_manager.h
               src/video_core/renderer_vulkan/liverpool_to_vk.cpp
               src/video_core/renderer_vulkan/liverpool_to_vk.h
               src/video_core/renderer_vulkan/vk_common.cpp
               src/video_core/renderer_vulkan/vk_common.h
               src/video_core/renderer_vulkan/vk_compute_pipeline.cpp
               src/video_core/renderer_vulkan/vk_compute_pipeline.h
               src/video_core/renderer_vulkan/vk_driver_pipeline_cache.cpp
               src/video_core/renderer_vulkan/vk_driver_pipeline_cache.h
               src/video_core/renderer_vulkan/vk_driver_pipeline_cache_storage.h
               src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp
               src/video_core/renderer_vulkan/vk_graphics_pipeline.h
               src/video_core/renderer_vulkan/vk_instance.cpp
               src/video_core/renderer_vulkan/vk_instance.h
               src/video_core/renderer_vulkan/vk_master_semaphore.cpp
               src/video_core/renderer_vulkan/vk_master_semaphore.h
               src/video_core/renderer_vulkan/vk_pipeline_cache.cpp
               src/video_core/renderer_vulkan/vk_pipeline_cache.h
               src/video_core/renderer_vulkan/vk_pipeline_common.cpp
               src/video_core/renderer_vulkan/vk_pipeline_common.h
               src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp
               src/video_core/renderer_vulkan/vk_pipeline_serialization.h
               src/video_core/renderer_vulkan/vk_platform.cpp
               src/video_core/renderer_vulkan/vk_platform.h
               src/video_core/renderer_vulkan/vk_presenter.cpp
               src/video_core/renderer_vulkan/vk_presenter.h
               src/video_core/renderer_vulkan/vk_rasterizer.cpp
               src/video_core/renderer_vulkan/vk_rasterizer.h
               src/video_core/renderer_vulkan/readconst_snapshot_diag.h
               src/video_core/renderer_vulkan/vk_resource_pool.cpp
               src/video_core/renderer_vulkan/vk_resource_pool.h
               src/video_core/renderer_vulkan/vk_scheduler.cpp
               src/video_core/renderer_vulkan/vk_scheduler.h
               src/video_core/renderer_vulkan/vk_shader_hle.cpp
               src/video_core/renderer_vulkan/vk_shader_hle.h
               src/video_core/renderer_vulkan/vk_shader_util.cpp
               src/video_core/renderer_vulkan/vk_shader_util.h
               src/video_core/renderer_vulkan/vk_swapchain.cpp
               src/video_core/renderer_vulkan/vk_swapchain.h
               src/video_core/renderer_vulkan/host_passes/fsr_pass.cpp
               src/video_core/renderer_vulkan/host_passes/fsr_pass.h
               src/video_core/renderer_vulkan/host_passes/pp_pass.cpp
               src/video_core/renderer_vulkan/host_passes/pp_pass.h
               src/video_core/texture_cache/blit_helper.cpp
               src/video_core/texture_cache/blit_helper.h
               src/video_core/texture_cache/host_compatibility.cpp
               src/video_core/texture_cache/host_compatibility.h
               src/video_core/texture_cache/image.cpp
               src/video_core/texture_cache/image.h
               src/video_core/texture_cache/image_info.cpp
               src/video_core/texture_cache/image_info.h
               src/video_core/texture_cache/image_view.cpp
               src/video_core/texture_cache/image_view.h
               src/video_core/texture_cache/sampler.cpp
               src/video_core/texture_cache/sampler.h
               src/video_core/texture_cache/texture_cache.cpp
               src/video_core/texture_cache/texture_cache.h
               src/video_core/texture_cache/tile_manager.cpp
               src/video_core/texture_cache/tile_manager.h
               src/video_core/texture_cache/types.h
               src/video_core/cache_storage.cpp
               src/video_core/cache_storage.h
               src/video_core/page_manager.cpp
               src/video_core/page_manager.h
               src/video_core/multi_level_page_table.h
               src/video_core/renderdoc.cpp
               src/video_core/renderdoc.h
)

if(APPLE)
    set(gen4_host_video_sources)
    foreach(gen4_video_source IN LISTS VIDEO_CORE)
        string(REGEX REPLACE "^src/" "" gen4_video_relative "${gen4_video_source}")
        if(EXISTS "${LUMEN_GEN4_MACOS_GPU_ROOT}/${gen4_video_relative}")
            list(APPEND gen4_host_video_sources "${LUMEN_GEN4_MACOS_GPU_ROOT}/${gen4_video_relative}")
        else()
            list(APPEND gen4_host_video_sources "${gen4_video_source}")
        endif()
    endforeach()
    set(VIDEO_CORE ${gen4_host_video_sources})
endif()

set(IMGUI src/imgui/imgui_config.h
          src/imgui/imgui_layer.h
          src/imgui/imgui_std.h
          src/imgui/imgui_texture.h
          src/imgui/imgui_translations.cpp
          src/imgui/imgui_translations.h
          src/imgui/notifications_layer.cpp
          src/imgui/notifications_layer.h
          src/imgui/renderer/imgui_core.cpp
          src/imgui/renderer/imgui_core.h
          src/imgui/renderer/imgui_impl_sdl3.cpp
          src/imgui/renderer/imgui_impl_sdl3.h
          src/imgui/renderer/imgui_impl_vulkan.cpp
          src/imgui/renderer/imgui_impl_vulkan.h
          src/imgui/renderer/font_data.cpp
          src/imgui/renderer/font_data.h
          src/imgui/renderer/font_stack.cpp
          src/imgui/renderer/font_stack.h
          src/imgui/renderer/texture_manager.cpp
          src/imgui/renderer/texture_manager.h
          src/imgui/big_picture/settings_dialog_imgui.cpp
          src/imgui/big_picture/settings_dialog_imgui.h
          src/imgui/big_picture/settings_dialog_layer.cpp
          src/imgui/big_picture/settings_dialog_layer.h
)

if(APPLE)
    foreach(gen4_font_source IN ITEMS font.cpp font_internal.cpp fontft_internal.cpp)
        list(TRANSFORM CORE REPLACE
            "^src/core/libraries/font/${gen4_font_source}$"
            "${LUMEN_GEN4_MACOS_GPU_ROOT}/core/libraries/font/${gen4_font_source}")
    endforeach()
endif()

set(INPUT src/input/controller.cpp
          src/input/controller.h
          src/input/input_handler.cpp
          src/input/input_handler.h
          src/input/input_mouse.cpp
          src/input/input_mouse.h
)

set(EMULATOR src/emulator.cpp
             src/emulator.h
             src/sdl_window.h
             src/sdl_window.cpp
)

if (APPLE)
    list(APPEND EMULATOR src/sdl_window_apple.mm)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CMakeRC.cmake")

if(ANDROID)
    set(LUMEN_GEN4_LIBRARY_TYPE SHARED)
    add_library(lumen_gen4_runtime ${LUMEN_GEN4_LIBRARY_TYPE})
else()
    add_executable(lumen_gen4_runtime)
endif()

target_sources(lumen_gen4_runtime PRIVATE
    src/lumen/admission.cpp
    src/lumen/session.cpp
    src/lumen/succi_benchmark.cpp
    ${AUDIO_CORE}
    ${IMGUI}
    ${INPUT}
    ${COMMON}
    ${CORE}
    ${SHADER_RECOMPILER}
    ${VIDEO_CORE}
    ${EMULATOR}
    ${LUMEN_GEN4_ENTRY_SOURCES}
    src/emulator.cpp
    src/emulator.h
    src/sdl_window.h
    src/sdl_window.cpp
)

target_compile_definitions(lumen_gen4_runtime PRIVATE LUMEN_GEN4_HOST_RUNTIME=1 VK_ENABLE_BETA_EXTENSIONS=)
if(ANDROID)
    find_library(LUMEN_ANDROID_LOG_LIBRARY log REQUIRED)
    include("${LUMEN_REPOSITORY_ROOT}/android/app/src/main/cpp/Ps5AdrenoTools.cmake")
    set_target_properties(lumen_gen4_runtime PROPERTIES OUTPUT_NAME lumen_android_gen4)
    target_link_libraries(lumen_gen4_runtime PRIVATE lumen_android_vulkan_driver_host android log)
    target_link_options(lumen_gen4_runtime PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384" "-Wl,--no-undefined")
    # The AArch64 FFmpeg assembly addresses its internal transform tables
    # directly. Keep those archive symbols local without hiding SDL's JNI API.
    target_link_options(lumen_gen4_runtime PRIVATE
        "-Wl,--exclude-libs,libavformat.a:libavcodec.a:libswscale.a:libavutil.a:libavfilter.a:libswresample.a")
endif()
if(APPLE)
    target_compile_definitions(lumen_gen4_runtime PRIVATE LUMEN_APPLE_NATIVE_METAL=1 LUMEN_GEN4_NATIVE_METAL=1)
    target_link_libraries(lumen_gen4_runtime PRIVATE lumen_gen4_native_metal)
endif()

set_source_files_properties(
    src/video_core/buffer_cache/vk_dispatch_reload.cpp
    PROPERTIES COMPILE_FLAGS "-fno-lto"
)

target_link_libraries(lumen_gen4_runtime PRIVATE spdlog::spdlog)
target_link_libraries(lumen_gen4_runtime PRIVATE magic_enum::magic_enum fmt::fmt toml11::toml11 tsl::robin_map xbyak::xbyak Tracy::TracyClient RenderDoc::API FFmpeg::ffmpeg Dear_ImGui ImGuiFileDialog gcn half::half ZLIB::ZLIB PNG::PNG minimp3)
target_link_libraries(lumen_gen4_runtime PRIVATE Boost::headers GPUOpen::VulkanMemoryAllocator LibAtrac9 sirit Vulkan::Headers xxHash::xxhash Zydis::Zydis glslang::glslang SDL3::SDL3 pugixml::pugixml)
target_link_libraries(lumen_gen4_runtime PRIVATE stb::headers lfreist-hwinfo::hwinfo nlohmann_json::nlohmann_json miniz::miniz fdk-aac CLI11::CLI11 OpenAL::OpenAL Cpp_Httplib miniupnpc::miniupnpc)
target_link_libraries(lumen_gen4_runtime PRIVATE Freetype::Freetype)

if(ENABLE_FEX_GUEST_CPU)
    target_compile_definitions(lumen_gen4_runtime PRIVATE LUMEN_GEN4_ENABLE_FEX_GUEST_CPU=1)
    target_link_libraries(lumen_gen4_runtime PRIVATE lumen_gen4_guest_cpu)
    add_executable(lumen_gen4_guest_runtime_test EXCLUDE_FROM_ALL
        tests/guest_runtime_test.cpp
        ${LUMEN_GEN4_FEX_ENGINE_SOURCE}
        src/core/guest_cpu/fex_guest_cpu.cpp
        src/core/guest_cpu/hle_call_adapter.cpp
        src/core/guest_cpu/fex_hle_bridge.cpp)
    target_compile_definitions(lumen_gen4_guest_runtime_test PRIVATE LUMEN_GEN4_ENABLE_FEX_GUEST_CPU=1)
    target_link_libraries(lumen_gen4_guest_runtime_test PRIVATE lumen_gen4_guest_cpu)
    add_test(NAME lumen_gen4_guest_runtime COMMAND lumen_gen4_guest_runtime_test)
    set_tests_properties(lumen_gen4_guest_runtime PROPERTIES TIMEOUT 30)
endif()

add_executable(lumen_gen4_bounded_draw_diagnostics_test EXCLUDE_FROM_ALL
    tests/bounded_draw_diagnostics_test.cpp)
add_test(NAME lumen_gen4_bounded_draw_diagnostics COMMAND lumen_gen4_bounded_draw_diagnostics_test)

add_executable(lumen_gen4_frame_ledger_test EXCLUDE_FROM_ALL
    tests/frame_ledger_test.cpp)
target_include_directories(lumen_gen4_frame_ledger_test PRIVATE src)
target_compile_features(lumen_gen4_frame_ledger_test PRIVATE cxx_std_20)
add_test(NAME lumen_gen4_frame_ledger COMMAND lumen_gen4_frame_ledger_test)
set_tests_properties(lumen_gen4_frame_ledger PROPERTIES TIMEOUT 20)

add_executable(lumen_gen4_admission_test EXCLUDE_FROM_ALL
    tests/admission_test.cpp src/lumen/admission.cpp)
target_include_directories(lumen_gen4_admission_test PRIVATE
    "${LUMEN_REPOSITORY_ROOT}/source/emulator/include")
add_test(NAME lumen_gen4_admission COMMAND lumen_gen4_admission_test)

add_executable(lumen_gen4_succi_benchmark_test EXCLUDE_FROM_ALL
    tests/succi_benchmark_test.cpp src/lumen/succi_benchmark.cpp)
target_link_libraries(lumen_gen4_succi_benchmark_test PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME lumen_gen4_succi_benchmark COMMAND lumen_gen4_succi_benchmark_test)

if(APPLE)
    add_executable(lumen_gen4_tls_layout_test EXCLUDE_FROM_ALL tests/tls_layout_test.cpp)
    add_test(NAME lumen_gen4_tls_layout COMMAND lumen_gen4_tls_layout_test)
endif()


add_executable(lumen_gen4_elf_validation_test EXCLUDE_FROM_ALL tests/elf_validation_test.cpp)
add_test(NAME lumen_gen4_elf_validation COMMAND lumen_gen4_elf_validation_test)

find_package(Threads REQUIRED)
add_executable(lumen_gen4_driver_pipeline_cache_test EXCLUDE_FROM_ALL
    tests/driver_pipeline_cache_test.cpp)
target_include_directories(lumen_gen4_driver_pipeline_cache_test PRIVATE src)
target_compile_features(lumen_gen4_driver_pipeline_cache_test PRIVATE cxx_std_20)
target_link_libraries(lumen_gen4_driver_pipeline_cache_test PRIVATE Threads::Threads)
add_test(NAME lumen_gen4_driver_pipeline_cache COMMAND lumen_gen4_driver_pipeline_cache_test)
set_tests_properties(lumen_gen4_driver_pipeline_cache PROPERTIES TIMEOUT 20)
add_executable(lumen_gen4_command_buffer_copy_test EXCLUDE_FROM_ALL
    tests/command_buffer_copy_test.cpp)
target_include_directories(lumen_gen4_command_buffer_copy_test PRIVATE src)
target_compile_features(lumen_gen4_command_buffer_copy_test PRIVATE cxx_std_20)
add_test(NAME lumen_gen4_command_buffer_copy COMMAND lumen_gen4_command_buffer_copy_test)
set_tests_properties(lumen_gen4_command_buffer_copy PROPERTIES TIMEOUT 20)
add_executable(lumen_gen4_source_page_tracker_test EXCLUDE_FROM_ALL
    tests/source_page_tracker_test.cpp)
target_include_directories(lumen_gen4_source_page_tracker_test PRIVATE src)
target_compile_features(lumen_gen4_source_page_tracker_test PRIVATE cxx_std_20)
add_test(NAME lumen_gen4_source_page_tracker COMMAND lumen_gen4_source_page_tracker_test)
set_tests_properties(lumen_gen4_source_page_tracker PROPERTIES TIMEOUT 20)
add_executable(lumen_gen4_buffer_consumer_barrier_test EXCLUDE_FROM_ALL
    tests/buffer_consumer_barrier_test.cpp)
target_include_directories(lumen_gen4_buffer_consumer_barrier_test PRIVATE src)
target_compile_features(lumen_gen4_buffer_consumer_barrier_test PRIVATE cxx_std_20)
target_link_libraries(lumen_gen4_buffer_consumer_barrier_test PRIVATE Vulkan::Headers)
add_test(NAME lumen_gen4_buffer_consumer_barrier COMMAND lumen_gen4_buffer_consumer_barrier_test)
set_tests_properties(lumen_gen4_buffer_consumer_barrier PROPERTIES TIMEOUT 20)
add_test(NAME lumen_gen4_buffer_consumer_barrier_default
    COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_BUFFER_CONSUMER_BARRIER
        --unset=LUMEN_BUFFER_SHADER_ACCESS_BARRIER
        $<TARGET_FILE:lumen_gen4_buffer_consumer_barrier_test> 1 0)
set_tests_properties(lumen_gen4_buffer_consumer_barrier_default PROPERTIES TIMEOUT 20)
foreach(selector IN ITEMS 0 1 invalid)
    if(selector STREQUAL "0")
        set(expected 0)
    else()
        set(expected 1)
    endif()
    add_test(NAME lumen_gen4_buffer_consumer_barrier_selector_${selector}
        COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_BUFFER_SHADER_ACCESS_BARRIER
            "LUMEN_BUFFER_CONSUMER_BARRIER=${selector}"
            $<TARGET_FILE:lumen_gen4_buffer_consumer_barrier_test> ${expected} 0)
    set_tests_properties(lumen_gen4_buffer_consumer_barrier_selector_${selector} PROPERTIES TIMEOUT 20)
endforeach()
add_test(NAME lumen_gen4_buffer_consumer_barrier_shader_invalid
    COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_BUFFER_CONSUMER_BARRIER
        LUMEN_BUFFER_SHADER_ACCESS_BARRIER=invalid
        $<TARGET_FILE:lumen_gen4_buffer_consumer_barrier_test> 1 0)
set_tests_properties(lumen_gen4_buffer_consumer_barrier_shader_invalid PROPERTIES TIMEOUT 20)
add_test(NAME lumen_gen4_buffer_consumer_barrier_fixed_only
    COMMAND lumen_gen4_buffer_consumer_barrier_test 1 0)
set_tests_properties(lumen_gen4_buffer_consumer_barrier_fixed_only PROPERTIES
    TIMEOUT 20 ENVIRONMENT "LUMEN_BUFFER_CONSUMER_BARRIER=1;LUMEN_BUFFER_SHADER_ACCESS_BARRIER=0")
add_test(NAME lumen_gen4_buffer_consumer_barrier_shader_only
    COMMAND lumen_gen4_buffer_consumer_barrier_test 0 1)
set_tests_properties(lumen_gen4_buffer_consumer_barrier_shader_only PROPERTIES
    TIMEOUT 20 ENVIRONMENT "LUMEN_BUFFER_CONSUMER_BARRIER=0;LUMEN_BUFFER_SHADER_ACCESS_BARRIER=1")
add_executable(lumen_gen4_buffer_copy_resolve_test EXCLUDE_FROM_ALL
    tests/buffer_copy_resolve_test.cpp)
target_include_directories(lumen_gen4_buffer_copy_resolve_test PRIVATE src)
target_compile_features(lumen_gen4_buffer_copy_resolve_test PRIVATE cxx_std_20)
target_link_libraries(lumen_gen4_buffer_copy_resolve_test PRIVATE spdlog::spdlog fmt::fmt)
add_test(NAME lumen_gen4_buffer_copy_resolve COMMAND lumen_gen4_buffer_copy_resolve_test)
set_tests_properties(lumen_gen4_buffer_copy_resolve PROPERTIES TIMEOUT 20)
add_test(NAME lumen_gen4_buffer_copy_resolve_default
    COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_BUFFER_COPY_RESOLVE
        $<TARGET_FILE:lumen_gen4_buffer_copy_resolve_test> 1)
set_tests_properties(lumen_gen4_buffer_copy_resolve_default PROPERTIES TIMEOUT 20)
foreach(selector IN ITEMS 0 1 invalid)
    if(selector STREQUAL "0")
        set(expected 0)
    else()
        set(expected 1)
    endif()
    add_test(NAME lumen_gen4_buffer_copy_resolve_selector_${selector}
        COMMAND lumen_gen4_buffer_copy_resolve_test ${expected})
    set_tests_properties(lumen_gen4_buffer_copy_resolve_selector_${selector} PROPERTIES
        TIMEOUT 20 ENVIRONMENT "LUMEN_BUFFER_COPY_RESOLVE=${selector}")
endforeach()
add_executable(lumen_gen4_cpu_upload_policy_test EXCLUDE_FROM_ALL
    tests/cpu_upload_policy_test.cpp src/common/spin_lock.cpp)
target_include_directories(lumen_gen4_cpu_upload_policy_test PRIVATE src)
target_compile_features(lumen_gen4_cpu_upload_policy_test PRIVATE cxx_std_20)
target_link_libraries(lumen_gen4_cpu_upload_policy_test PRIVATE
    Boost::headers nlohmann_json::nlohmann_json spdlog::spdlog fmt::fmt
    tsl::robin_map xxHash::xxhash Tracy::TracyClient Threads::Threads)
add_test(NAME lumen_gen4_cpu_upload_policy COMMAND lumen_gen4_cpu_upload_policy_test)
set_tests_properties(lumen_gen4_cpu_upload_policy PROPERTIES TIMEOUT 20)
add_test(NAME lumen_gen4_buffer_pending_write_resolve_default
    COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_BUFFER_PENDING_WRITE_RESOLVE
        $<TARGET_FILE:lumen_gen4_cpu_upload_policy_test> 1)
set_tests_properties(lumen_gen4_buffer_pending_write_resolve_default PROPERTIES TIMEOUT 20)
foreach(selector IN ITEMS 0 1 invalid)
    if(selector STREQUAL "0")
        set(expected 0)
    else()
        set(expected 1)
    endif()
    add_test(NAME lumen_gen4_buffer_pending_write_resolve_selector_${selector}
        COMMAND ${CMAKE_COMMAND} -E env "LUMEN_BUFFER_PENDING_WRITE_RESOLVE=${selector}"
            $<TARGET_FILE:lumen_gen4_cpu_upload_policy_test> ${expected})
    set_tests_properties(lumen_gen4_buffer_pending_write_resolve_selector_${selector} PROPERTIES TIMEOUT 20)
endforeach()
add_executable(lumen_gen4_counter_clock_contract_test EXCLUDE_FROM_ALL
    tests/counter_clock_contract_test.cpp src/common/native_clock.cpp src/common/rdtsc.cpp)
target_include_directories(lumen_gen4_counter_clock_contract_test PRIVATE src)
target_compile_features(lumen_gen4_counter_clock_contract_test PRIVATE cxx_std_20)
add_test(NAME lumen_gen4_counter_clock_contract COMMAND lumen_gen4_counter_clock_contract_test)
set_tests_properties(lumen_gen4_counter_clock_contract PROPERTIES TIMEOUT 15)
add_executable(lumen_gen4_sharp_fetch_contract_test EXCLUDE_FROM_ALL
    tests/sharp_fetch_contract_test.cpp)
target_include_directories(lumen_gen4_sharp_fetch_contract_test PRIVATE src)
target_compile_features(lumen_gen4_sharp_fetch_contract_test PRIVATE cxx_std_20)
target_link_libraries(lumen_gen4_sharp_fetch_contract_test PRIVATE Boost::headers spdlog::spdlog fmt::fmt)
add_test(NAME lumen_gen4_sharp_fetch_contract COMMAND lumen_gen4_sharp_fetch_contract_test)
set_tests_properties(lumen_gen4_sharp_fetch_contract PROPERTIES TIMEOUT 20)

add_executable(lumen_gen4_primitive_expansion_contract_test EXCLUDE_FROM_ALL
    tests/primitive_expansion_contract_test.cpp)
target_include_directories(lumen_gen4_primitive_expansion_contract_test PRIVATE src)
target_compile_features(lumen_gen4_primitive_expansion_contract_test PRIVATE cxx_std_20)
add_test(NAME lumen_gen4_primitive_expansion_contract COMMAND lumen_gen4_primitive_expansion_contract_test)
set_tests_properties(lumen_gen4_primitive_expansion_contract PROPERTIES TIMEOUT 20)
add_executable(lumen_gen4_guest_printf_test EXCLUDE_FROM_ALL tests/guest_printf_test.cpp)
add_test(NAME lumen_gen4_guest_printf COMMAND lumen_gen4_guest_printf_test)
add_executable(lumen_gen4_swapchain_policy_test EXCLUDE_FROM_ALL tests/swapchain_policy_test.cpp)
add_test(NAME lumen_gen4_swapchain_policy COMMAND lumen_gen4_swapchain_policy_test)
add_executable(lumen_gen4_address_wait_queue_test EXCLUDE_FROM_ALL tests/address_wait_queue_test.cpp)
target_link_libraries(lumen_gen4_address_wait_queue_test PRIVATE Threads::Threads)
add_test(NAME lumen_gen4_address_wait_queue COMMAND lumen_gen4_address_wait_queue_test)
add_executable(lumen_gen4_umtx_test EXCLUDE_FROM_ALL
    tests/umtx_test.cpp src/core/libraries/kernel/umtx.cpp)
target_link_libraries(lumen_gen4_umtx_test PRIVATE Threads::Threads)
add_test(NAME lumen_gen4_umtx COMMAND lumen_gen4_umtx_test)
add_executable(lumen_gen4_thread_callback_registry_test EXCLUDE_FROM_ALL tests/thread_callback_registry_test.cpp)
target_link_libraries(lumen_gen4_thread_callback_registry_test PRIVATE Threads::Threads)
add_test(NAME lumen_gen4_thread_callback_registry COMMAND lumen_gen4_thread_callback_registry_test)
add_executable(lumen_gen4_avplayer_file_io_test EXCLUDE_FROM_ALL tests/avplayer_file_io_test.cpp)
target_link_libraries(lumen_gen4_avplayer_file_io_test PRIVATE Threads::Threads)
add_test(NAME lumen_gen4_avplayer_file_io COMMAND lumen_gen4_avplayer_file_io_test)
set_tests_properties(lumen_gen4_avplayer_file_io PROPERTIES TIMEOUT 15)
set_tests_properties(lumen_gen4_guest_printf lumen_gen4_address_wait_queue lumen_gen4_umtx PROPERTIES TIMEOUT 15)
add_executable(lumen_gen4_exit_registry_test EXCLUDE_FROM_ALL tests/exit_registry_test.cpp)
target_link_libraries(lumen_gen4_exit_registry_test PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(lumen_gen4_exit_registry_test PRIVATE /UNDEBUG)
else()
    target_compile_options(lumen_gen4_exit_registry_test PRIVATE -UNDEBUG)
endif()
add_test(NAME lumen_gen4_exit_registry COMMAND lumen_gen4_exit_registry_test)
set_tests_properties(lumen_gen4_exit_registry PROPERTIES TIMEOUT 10)

add_executable(lumen_gen4_fios_filter_test EXCLUDE_FROM_ALL tests/fios_filter_test.cpp)
target_link_libraries(lumen_gen4_fios_filter_test PRIVATE Threads::Threads)
add_test(NAME lumen_gen4_fios_filter COMMAND lumen_gen4_fios_filter_test)
set_tests_properties(lumen_gen4_fios_filter PROPERTIES TIMEOUT 10)

add_executable(lumen_gen4_ngs2_waveform_direct_contract_test EXCLUDE_FROM_ALL
    tests/ngs2_waveform_direct_contract_test.cpp
    src/core/libraries/ngs2/ngs2_waveform_direct.cpp)
target_include_directories(lumen_gen4_ngs2_waveform_direct_contract_test PRIVATE src)
add_test(NAME lumen_gen4_ngs2_waveform_direct_contract
    COMMAND lumen_gen4_ngs2_waveform_direct_contract_test)
set_tests_properties(lumen_gen4_ngs2_waveform_direct_contract PROPERTIES TIMEOUT 10)


if(NOT WIN32)
    add_executable(lumen_gen4_elf_loader_test EXCLUDE_FROM_ALL
        tests/elf_loader_test.cpp src/core/loader/elf.cpp
        src/core/loader/compiled_sdk.cpp
        src/common/io_file.cpp src/common/path_util.cpp)
    target_link_libraries(lumen_gen4_elf_loader_test PRIVATE
        Boost::headers nlohmann_json::nlohmann_json spdlog::spdlog fmt::fmt)
    if(APPLE)
        target_link_libraries(lumen_gen4_elf_loader_test PRIVATE "-framework CoreFoundation")
    endif()
    add_test(NAME lumen_gen4_elf_loader COMMAND lumen_gen4_elf_loader_test)
endif()

add_executable(lumen_gen4_ime_bridge_test EXCLUDE_FROM_ALL tests/ime_bridge_test.cpp)
add_test(NAME lumen_gen4_ime_bridge COMMAND lumen_gen4_ime_bridge_test)
add_executable(lumen_gen4_virtual_input_test EXCLUDE_FROM_ALL tests/virtual_input_test.cpp)
add_test(NAME lumen_gen4_virtual_input COMMAND lumen_gen4_virtual_input_test)

if(ENABLE_FEX_GUEST_CPU)
    add_executable(lumen_gen4_exception_boundary_test EXCLUDE_FROM_ALL
        tests/exception_boundary_test.cpp)
    target_link_libraries(lumen_gen4_exception_boundary_test PRIVATE lumen_gen4_guest_cpu)
    add_test(NAME lumen_gen4_exception_boundary COMMAND lumen_gen4_exception_boundary_test)
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD")
    target_link_libraries(lumen_gen4_runtime PRIVATE "/usr/lib/libusb.so")
    target_link_libraries(lumen_gen4_runtime PRIVATE "/usr/local/lib/libuuid.so")
else()
    target_link_libraries(lumen_gen4_runtime PRIVATE libusb::usb)
endif()

target_compile_definitions(lumen_gen4_runtime PRIVATE IMGUI_USER_CONFIG="imgui/imgui_config.h")
if(ENABLE_FEX_GUEST_CPU)
    target_link_libraries(lumen_gen4_runtime PRIVATE ZArchive::zarchive)
endif()
target_include_directories(Dear_ImGui PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(Dear_ImGui PRIVATE IMGUI_USER_CONFIG="imgui/imgui_config.h")

if (ENABLE_DISCORD_RPC)
    target_compile_definitions(lumen_gen4_runtime PRIVATE ENABLE_DISCORD_RPC)
endif()

if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    # Optional due to https://github.com/shadps4-emu/shadPS4/issues/1704
    if (ENABLE_USERFAULTFD)
        target_compile_definitions(lumen_gen4_runtime PRIVATE ENABLE_USERFAULTFD)
    endif()

    target_link_libraries(lumen_gen4_runtime PRIVATE uuid)
endif()

if (APPLE)
    if (ARCHITECTURE STREQUAL "x86_64")
        # Reserve system-managed memory space.
        target_link_options(lumen_gen4_runtime PRIVATE -Wl,-ld_classic,-no_pie,-no_fixup_chains,-no_huge,-pagezero_size,0x4000,-segaddr,TCB_SPACE,0x4000,-segaddr,SYSTEM_MANAGED,0x400000,-segaddr,SYSTEM_RESERVED,0x7FFFFC000,-segaddr,USER_AREA,0x7000000000,-image_base,0x700000000000)
    endif()

    # Replacement for std::chrono::time_zone
    target_link_libraries(lumen_gen4_runtime PRIVATE date::date-tz epoll-shim)

elseif (${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD")
    target_link_libraries(lumen_gen4_runtime PRIVATE date::date-tz epoll-shim)
endif()

if (WIN32)
    target_link_libraries(lumen_gen4_runtime PRIVATE mincore wepoll wbemuuid)

    if (MSVC)
        # MSVC likes putting opinions on what people can use, disable:
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE _SCL_SECURE_NO_WARNINGS)
    endif()

    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

    if (MSVC)
        # Needed for conflicts with time.h of windows.h
        add_compile_definitions(_TIMESPEC_DEFINED)
    endif()

    # Target Windows 10 RS5
    add_compile_definitions(NTDDI_VERSION=0x0A000006 _WIN32_WINNT=0x0A00 WINVER=0x0A00)

    if (MSVC)
        target_link_libraries(lumen_gen4_runtime PRIVATE clang_rt.builtins-x86_64.lib)
    endif()

    # Disable ASLR so we can reserve the user area
    if (MSVC)
        target_link_options(lumen_gen4_runtime PRIVATE /DYNAMICBASE:NO)
    else()
        target_link_options(lumen_gen4_runtime PRIVATE -Wl,--disable-dynamicbase)
    endif()

    # Increase stack commit area (Needed, otherwise there are crashes)
    if (MSVC)
        target_link_options(lumen_gen4_runtime PRIVATE /STACK:0x200000,0x200000)
    else()
        target_link_options(lumen_gen4_runtime PRIVATE -Wl,--stack,2097152)
    endif()

    # Change base image address
    if (MSVC)
        target_link_options(lumen_gen4_runtime PRIVATE /BASE:0x700000000000)
    else()
        target_link_options(lumen_gen4_runtime PRIVATE -Wl,--image-base=0x700000000000)
    endif()
endif()

add_compile_definitions(BOOST_ASIO_STANDALONE)

target_include_directories(lumen_gen4_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    "${LUMEN_REPOSITORY_ROOT}/source/emulator/include")

# Shaders sources
if(APPLE)
    set(HOST_SHADERS_INCLUDE "${LUMEN_GEN4_MACOS_GPU_ROOT}/video_core/host_shaders")
else()
    set(HOST_SHADERS_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/src/video_core/host_shaders")
endif()

add_subdirectory(${HOST_SHADERS_INCLUDE} host-shaders)
add_dependencies(lumen_gen4_runtime host_shaders)
target_include_directories(lumen_gen4_runtime PRIVATE ${HOST_SHADERS_INCLUDE})

# embed resources

cmrc_add_resource_library(embedded-resources
        ALIAS res::embedded
        NAMESPACE res
        src/resources/big_picture/controller.png
        src/resources/big_picture/experimental.png
        src/resources/big_picture/graphics.png
        src/resources/big_picture/log.png
        src/resources/big_picture/profiles.png
        src/resources/big_picture/settings.png
        src/resources/big_picture/trophy.png
        src/resources/bronze.png
        src/resources/gold.png
        src/resources/platinum.png
        src/resources/lumen.png
        src/resources/silver.png
        src/resources/trophy.wav)
target_link_libraries(lumen_gen4_runtime PRIVATE res::embedded)

# ImGui resources
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/imgui/renderer)
add_dependencies(lumen_gen4_runtime ImGui_Resources)
target_include_directories(lumen_gen4_runtime PRIVATE ${IMGUI_RESOURCES_INCLUDE})


# Discord RPC
if (ENABLE_DISCORD_RPC)
    target_link_libraries(lumen_gen4_runtime PRIVATE discord-rpc)
endif()

# Install rules
install(TARGETS lumen_gen4_runtime BUNDLE DESTINATION .)

# Build the complete engine once. The runtime entry and the graphics contract
# executable both link these exact objects and the same dependency closure.
get_target_property(gen4_engine_sources lumen_gen4_runtime SOURCES)
foreach(entry IN LISTS LUMEN_GEN4_ENTRY_SOURCES)
    list(REMOVE_ITEM gen4_engine_sources "${entry}")
endforeach()
list(REMOVE_DUPLICATES gen4_engine_sources)
get_target_property(gen4_engine_libraries lumen_gen4_runtime LINK_LIBRARIES)
get_target_property(gen4_engine_dependencies lumen_gen4_runtime MANUALLY_ADDED_DEPENDENCIES)
add_library(lumen_gen4_core OBJECT ${gen4_engine_sources})
target_link_libraries(lumen_gen4_core PRIVATE ${gen4_engine_libraries})
foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES)
    get_target_property(gen4_property lumen_gen4_runtime ${property})
    if(gen4_property)
        set_property(TARGET lumen_gen4_core PROPERTY ${property} "${gen4_property}")
    endif()
endforeach()
if(gen4_engine_dependencies)
    add_dependencies(lumen_gen4_core ${gen4_engine_dependencies})
endif()
set_property(TARGET lumen_gen4_runtime PROPERTY SOURCES
    "${LUMEN_GEN4_ENTRY_SOURCES};$<TARGET_OBJECTS:lumen_gen4_core>")

if(APPLE OR ANDROID)
    set(gen4_native_contracts page_fault_watch resource_tracking_contract
         image_binding_contract srt_flatten_contract msgdialog_network_notice_contract
         png_codec_contract ngs2_mixer avplayer_frame_lifetime_contract
         avplayer_decoder_completion_contract detile_source_contract)
    if(APPLE)
        list(APPEND gen4_native_contracts rect_list_contract desktop_input_contract)
        if(NOT ENABLE_FEX_GUEST_CPU)
            list(APPEND gen4_native_contracts pthread_wait_contract)
        endif()
    endif()
    if(ANDROID)
        list(APPEND gen4_native_contracts thread_exit_publication memory_backing_alias_contract
             thread_exit_owner_contract)
    endif()
    if(ENABLE_FEX_GUEST_CPU)
        list(APPEND gen4_native_contracts fiber_runtime_contract equeue_direct_contract
             thread_sync_direct_contract vfs_direct_contract ajm_direct_contract
             ajm_at9_direct_contract userservice_direct_contract)
    endif()
    list(APPEND gen4_native_contracts render_end_attachment_contract)
    foreach(contract IN LISTS gen4_native_contracts)
        add_executable(lumen_gen4_${contract}_test EXCLUDE_FROM_ALL
            tests/${contract}_test.cpp $<TARGET_OBJECTS:lumen_gen4_core>)
        if(ANDROID)
            target_sources(lumen_gen4_${contract}_test PRIVATE ${LUMEN_GEN4_ENTRY_SOURCES})
        endif()
        target_link_libraries(lumen_gen4_${contract}_test PRIVATE ${gen4_engine_libraries})
        foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES LINK_OPTIONS)
            get_target_property(gen4_property lumen_gen4_runtime ${property})
            if(gen4_property)
                set_property(TARGET lumen_gen4_${contract}_test PROPERTY ${property} "${gen4_property}")
            endif()
        endforeach()
        add_dependencies(lumen_gen4_${contract}_test host_shaders ImGui_Resources)
        add_test(NAME lumen_gen4_${contract} COMMAND lumen_gen4_${contract}_test)
        set_tests_properties(lumen_gen4_${contract} PROPERTIES TIMEOUT 15)
    endforeach()
    add_test(NAME lumen_gen4_ngs2_mixer_disabled
        COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER
            $<TARGET_FILE:lumen_gen4_ngs2_mixer_test> --disabled)
    add_test(NAME lumen_gen4_ngs2_mixer_nonexact_selector
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_EXPERIMENTAL_NGS2_AUDIO_RENDER=01
            $<TARGET_FILE:lumen_gen4_ngs2_mixer_test> --disabled)
    set_tests_properties(lumen_gen4_ngs2_mixer_disabled lumen_gen4_ngs2_mixer_nonexact_selector
        PROPERTIES TIMEOUT 15)
    set_tests_properties(lumen_gen4_avplayer_decoder_completion_contract PROPERTIES
        ENVIRONMENT "LUMEN_AVPLAYER_DECODER_COMPLETION=1")
    add_test(NAME lumen_gen4_avplayer_decoder_completion_default
        COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_AVPLAYER_DECODER_COMPLETION
            $<TARGET_FILE:lumen_gen4_avplayer_decoder_completion_contract_test>)
    add_test(NAME lumen_gen4_avplayer_decoder_completion_zero
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_AVPLAYER_DECODER_COMPLETION=0
            $<TARGET_FILE:lumen_gen4_avplayer_decoder_completion_contract_test> --selector-off)
    add_test(NAME lumen_gen4_avplayer_decoder_completion_nonexact_selector
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_AVPLAYER_DECODER_COMPLETION=01
            $<TARGET_FILE:lumen_gen4_avplayer_decoder_completion_contract_test>)
    add_test(NAME lumen_gen4_avplayer_decoder_completion_nonexact_zero
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_AVPLAYER_DECODER_COMPLETION=00
            $<TARGET_FILE:lumen_gen4_avplayer_decoder_completion_contract_test>)
    set_tests_properties(lumen_gen4_avplayer_decoder_completion_default
        lumen_gen4_avplayer_decoder_completion_zero
        lumen_gen4_avplayer_decoder_completion_nonexact_selector
        lumen_gen4_avplayer_decoder_completion_nonexact_zero PROPERTIES TIMEOUT 15)
    set_tests_properties(lumen_gen4_detile_source_contract PROPERTIES
        ENVIRONMENT "LUMEN_DETILE_SOURCE_BARRIER=1" TIMEOUT 30 LABELS graphics)
    add_test(NAME lumen_gen4_detile_source_default
        COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_DETILE_SOURCE_BARRIER
            $<TARGET_FILE:lumen_gen4_detile_source_contract_test> --selector-on)
    add_test(NAME lumen_gen4_detile_source_zero
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_DETILE_SOURCE_BARRIER=0
            $<TARGET_FILE:lumen_gen4_detile_source_contract_test> --selector-off)
    add_test(NAME lumen_gen4_detile_source_nonexact_selector
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_DETILE_SOURCE_BARRIER=01
            $<TARGET_FILE:lumen_gen4_detile_source_contract_test> --selector-on)
    add_test(NAME lumen_gen4_detile_source_nonexact_zero
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_DETILE_SOURCE_BARRIER=00
            $<TARGET_FILE:lumen_gen4_detile_source_contract_test> --selector-on)
    set_tests_properties(lumen_gen4_detile_source_default lumen_gen4_detile_source_zero
        lumen_gen4_detile_source_nonexact_selector lumen_gen4_detile_source_nonexact_zero
        PROPERTIES TIMEOUT 15)
    set_tests_properties(lumen_gen4_render_end_attachment_contract PROPERTIES
        ENVIRONMENT "LUMEN_RENDER_END_ATTACHMENT_BARRIER=1" TIMEOUT 30 LABELS graphics)
    add_test(NAME lumen_gen4_render_end_attachment_masks
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_RENDER_END_ATTACHMENT_BARRIER=1
            $<TARGET_FILE:lumen_gen4_render_end_attachment_contract_test> --contract-only)
    add_test(NAME lumen_gen4_render_end_attachment_default
        COMMAND ${CMAKE_COMMAND} -E env --unset=LUMEN_RENDER_END_ATTACHMENT_BARRIER
            $<TARGET_FILE:lumen_gen4_render_end_attachment_contract_test> --contract-only)
    add_test(NAME lumen_gen4_render_end_attachment_zero
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_RENDER_END_ATTACHMENT_BARRIER=0
            $<TARGET_FILE:lumen_gen4_render_end_attachment_contract_test> --selector-off)
    add_test(NAME lumen_gen4_render_end_attachment_nonexact_selector
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_RENDER_END_ATTACHMENT_BARRIER=01
            $<TARGET_FILE:lumen_gen4_render_end_attachment_contract_test> --contract-only)
    add_test(NAME lumen_gen4_render_end_attachment_nonexact_zero
        COMMAND ${CMAKE_COMMAND} -E env LUMEN_RENDER_END_ATTACHMENT_BARRIER=00
            $<TARGET_FILE:lumen_gen4_render_end_attachment_contract_test> --contract-only)
    set_tests_properties(lumen_gen4_render_end_attachment_masks
        lumen_gen4_render_end_attachment_default lumen_gen4_render_end_attachment_zero
        lumen_gen4_render_end_attachment_nonexact_selector
        lumen_gen4_render_end_attachment_nonexact_zero PROPERTIES TIMEOUT 15)
    if(APPLE)
        set_tests_properties(lumen_gen4_rect_list_contract PROPERTIES
            ENVIRONMENT "MTL_DEBUG_LAYER=1" LABELS "graphics;native-metal;rect-list")
    endif()
endif()

if(APPLE)
    set(gen4_macos_contracts np_lookup_contract np_in_game_message_contract
        np_utility_contract np_plus_feature_contract np_matching_compat_contract
        rwlock_publication thread_exit_publication signal_chain memory_backing_alias_contract)
    if(NOT ENABLE_FEX_GUEST_CPU)
        list(APPEND gen4_macos_contracts audio_services ajm_at9_contract file_metadata)
    endif()
    foreach(contract IN LISTS gen4_macos_contracts)
        add_executable(lumen_gen4_${contract}_test EXCLUDE_FROM_ALL
            tests/${contract}_test.cpp $<TARGET_OBJECTS:lumen_gen4_core>)
        target_link_libraries(lumen_gen4_${contract}_test PRIVATE ${gen4_engine_libraries})
        foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES LINK_OPTIONS)
            get_target_property(gen4_property lumen_gen4_runtime ${property})
            if(gen4_property)
                set_property(TARGET lumen_gen4_${contract}_test PROPERTY ${property} "${gen4_property}")
            endif()
        endforeach()
        add_dependencies(lumen_gen4_${contract}_test host_shaders ImGui_Resources)
        add_test(NAME lumen_gen4_${contract} COMMAND lumen_gen4_${contract}_test)
        set_tests_properties(lumen_gen4_${contract} PROPERTIES TIMEOUT 30)
    endforeach()

    add_executable(lumen_gen4_np_callback_contract_test EXCLUDE_FROM_ALL
        tests/np_callback_contract_test.cpp $<TARGET_OBJECTS:lumen_gen4_core>)
    target_link_libraries(lumen_gen4_np_callback_contract_test PRIVATE ${gen4_engine_libraries})
    foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES LINK_OPTIONS)
        get_target_property(gen4_property lumen_gen4_runtime ${property})
        if(gen4_property)
            set_property(TARGET lumen_gen4_np_callback_contract_test PROPERTY ${property} "${gen4_property}")
        endif()
    endforeach()
    add_dependencies(lumen_gen4_np_callback_contract_test host_shaders ImGui_Resources)
    add_test(NAME lumen_gen4_np_callback_contract COMMAND lumen_gen4_np_callback_contract_test)
    set_tests_properties(lumen_gen4_np_callback_contract PROPERTIES TIMEOUT 30)

    add_executable(lumen_gen4_graphics_contract_test EXCLUDE_FROM_ALL
        tests/graphics_contract_test.cpp $<TARGET_OBJECTS:lumen_gen4_core>)
    target_link_libraries(lumen_gen4_graphics_contract_test PRIVATE ${gen4_engine_libraries})
    foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES LINK_OPTIONS)
        get_target_property(gen4_property lumen_gen4_runtime ${property})
        if(gen4_property)
            set_property(TARGET lumen_gen4_graphics_contract_test PROPERTY ${property} "${gen4_property}")
        endif()
    endforeach()
    add_dependencies(lumen_gen4_graphics_contract_test host_shaders ImGui_Resources)
    add_test(NAME lumen_gen4_graphics_contract COMMAND lumen_gen4_graphics_contract_test)
    set_tests_properties(lumen_gen4_graphics_contract PROPERTIES TIMEOUT 30 LABELS graphics)
    add_executable(lumen_gen4_guest_publication_inspection_test EXCLUDE_FROM_ALL
        tests/guest_publication_inspection_test.cpp)
    target_include_directories(lumen_gen4_guest_publication_inspection_test PRIVATE src)
    target_compile_features(lumen_gen4_guest_publication_inspection_test PRIVATE cxx_std_20)
    add_test(NAME lumen_gen4_guest_publication_inspection
        COMMAND lumen_gen4_guest_publication_inspection_test)
    set_tests_properties(lumen_gen4_guest_publication_inspection PROPERTIES TIMEOUT 30 LABELS graphics)
    add_executable(lumen_gen4_guest_publication_snapshots_test EXCLUDE_FROM_ALL
        tests/guest_publication_snapshots_test.cpp)
    target_include_directories(lumen_gen4_guest_publication_snapshots_test PRIVATE src)
    target_compile_features(lumen_gen4_guest_publication_snapshots_test PRIVATE cxx_std_20)
    add_test(NAME lumen_gen4_guest_publication_snapshots
        COMMAND lumen_gen4_guest_publication_snapshots_test)
    set_tests_properties(lumen_gen4_guest_publication_snapshots PROPERTIES TIMEOUT 30 LABELS graphics)
endif()
