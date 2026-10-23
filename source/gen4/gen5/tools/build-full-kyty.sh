#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build only: this command does not launch guest code or start a game.
set -eu
if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
  printf '%s\n' 'usage: build-full-kyty.sh <parent-FEX-build> <Kyty-build> <FFmpeg-prebuilt> [jobs]' >&2
  exit 2
fi
if [ -z "$3" ] || [ ! -d "$3" ]; then
  printf '%s\n' 'Set LUMEN_PS5_FFMPEG_PREBUILT_DIR to an existing verified FFmpeg prebuilt directory.' >&2
  exit 2
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository=$(CDPATH= cd -- "$script_dir/../../../.." && pwd)
parent_build=$(CDPATH= cd -- "$1" && pwd)
mkdir -p -- "$2"
kyty_build=$(CDPATH= cd -- "$2" && pwd)
ffmpeg_prebuilt=$(CDPATH= cd -- "$3" && pwd)
build_jobs=${4:-4}
case "$build_jobs" in ''|*[!0-9]*|0) printf '%s\n' 'jobs must be a positive integer' >&2; exit 2 ;; esac
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
  printf '%s\n' 'A native macOS arm64 shell is required.' >&2
  exit 1
fi
# CMake normally rejects a source-directory move even within this checkout.
# Retain all build options and objects, clearing only its source identity for
# the one legacy location that was moved into the unified source tree.
set --
if [ -f "$kyty_build/CMakeCache.txt" ]; then
  cached_source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$kyty_build/CMakeCache.txt")
  case "$cached_source" in
    "$repository/source/ps5/kyty")
      set -- -U CMAKE_HOME_DIRECTORY
      ;;
    "$repository/source/gen4/gen5/compatibility") ;;
    *)
      printf '%s\n' "Refusing to reuse a Gen5 build cache from an unrelated source: $cached_source" >&2
      exit 1
      ;;
  esac
fi
# Reuse the parent graph and bring its archives up to date with the current
# wrapper and reviewed overlay. This is a no-op for a fresh parent build and
# never creates a second translator or another copy of fmt/xxhash.
cmake --build "$parent_build" --target lumen_macos_fexcore_runtime --parallel "$build_jobs"
cmake "$@" -S "$repository/source/gen4/gen5/compatibility" -B "$kyty_build" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_AR="$repository/source/tools/macos-ar.sh" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER= \
  -DKYTY_BUILD_LAUNCHER=OFF -DKYTY_ENABLE_CLANG_TIDY=OFF \
  -DLUMEN_PS5_FEXCORE=ON \
  -DLUMEN_PS5_FEX_TOOLCHAIN_FILE="$repository/source/gen4/gen5/cmake/LumenFexPrebuilt.cmake" \
  -DLUMEN_PS5_FEX_PREBUILT_ROOT="$parent_build" \
  -DFFMPEG_PREBUILT_DIR="$ffmpeg_prebuilt"
cmake --build "$kyty_build" --target kyty_emulator --parallel "$build_jobs"
python3 "$script_dir/VerifyFexPrebuilt.py" --repository "$repository" \
  --parent "$parent_build" --binary "$kyty_build/kyty_emulator" \
  --output "$kyty_build/lumen-full-kyty-fex-link.json"
