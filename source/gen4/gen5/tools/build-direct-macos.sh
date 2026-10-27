#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the native C13 runtime without involving the desktop dashboard.
set -eu
if [ "$#" -lt 2 ]; then
  printf '%s\n' 'usage: build-direct-macos.sh <build> <FFmpeg-prebuilt> [jobs] [cmake options...]' >&2
  exit 2
fi
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository=$(CDPATH= cd -- "$script_dir/../../../.." && pwd)
build_dir=$1
ffmpeg_prebuilt=$2
build_jobs=${3:-4}
case "$build_jobs" in ''|*[!0-9]*|0) printf '%s\n' 'jobs must be a positive integer' >&2; exit 2 ;; esac
if [ "$#" -ge 3 ]; then shift 3; else shift 2; fi
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
  printf '%s\n' 'A native macOS arm64 shell is required.' >&2
  exit 1
fi
if [ ! -d "$ffmpeg_prebuilt" ]; then
  printf '%s\n' 'FFmpeg prebuilt directory does not exist.' >&2
  exit 2
fi
cmake -S "$repository/source/gen4/gen5/compatibility" -B "$build_dir" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_AR="$repository/source/tools/macos-ar.sh" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER= \
  -DKYTY_BUILD_LAUNCHER=OFF -DKYTY_ENABLE_CLANG_TIDY=OFF \
  -DLUMEN_PS5_DIRECT_FEX=ON -DLUMEN_PS5_FEXCORE=ON \
  -DFFMPEG_PREBUILT_DIR="$ffmpeg_prebuilt" "$@"
cmake --build "$build_dir" --target kyty_emulator --parallel "$build_jobs"
python3 "$script_dir/VerifyDirectMac.py" --build "$build_dir" \
  --output "$build_dir/direct-audit.json"
