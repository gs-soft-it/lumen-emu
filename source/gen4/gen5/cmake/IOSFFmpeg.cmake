# SPDX-License-Identifier: GPL-2.0-only
# Predefine the native iOS closure before the desktop dependency graph. An
# arm64 macOS archive is not a valid substitute for an arm64 device/Simulator.
if(NOT EXISTS "${FFMPEG_PREBUILT_DIR}/include/libavcodec/avcodec.h")
  message(FATAL_ERROR "iOS Gen5 requires an explicit SDK-native FFMPEG_PREBUILT_DIR")
endif()
add_library(lumen_ios_gen5_ffmpeg INTERFACE)
add_library(FFmpeg::ffmpeg ALIAS lumen_ios_gen5_ffmpeg)
target_include_directories(lumen_ios_gen5_ffmpeg INTERFACE "${FFMPEG_PREBUILT_DIR}/include")
foreach(part avformat avcodec swscale avutil avfilter swresample)
  set(archive "${FFMPEG_PREBUILT_DIR}/lib/lib${part}.a")
  if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "Missing native iOS FFmpeg archive: ${archive}")
  endif()
  target_link_libraries(lumen_ios_gen5_ffmpeg INTERFACE "${archive}")
endforeach()
target_link_libraries(lumen_ios_gen5_ffmpeg INTERFACE
  "-framework CoreFoundation" "-framework AudioToolbox"
  "-framework CoreAudio" "-framework CoreMedia" "-framework VideoToolbox"
  "-framework CoreVideo" "-framework Security")
