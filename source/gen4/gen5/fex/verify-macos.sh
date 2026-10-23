#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ScriptDirectory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RepositoryRoot=$(cd -- "$ScriptDirectory/../../../.." && pwd)
FexSourceDirectory=${1:-${LUMEN_MACOS_FEXCORE_SOURCE_DIR:-}}
BuildDirectory=${2:-$RepositoryRoot/_Build/ps5-arm64}
GameExecutable=${3:-}

if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
  echo 'PS5 FEX verification requires native macOS ARM64.' >&2
  exit 1
fi
if [[ $(sysctl -in sysctl.proc_translated 2>/dev/null || echo 0) == 1 ]]; then
  echo 'Run PS5 FEX verification from a native ARM64 process.' >&2
  exit 1
fi
if [[ -z "$FexSourceDirectory" ]]; then
  echo 'Pass the reviewed external Darwin FEX checkout as argument 1.' >&2
  exit 2
fi

cmake -S "$RepositoryRoot/source/gen4/gen5" -B "$BuildDirectory" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DCMAKE_BUILD_TYPE=Release \
  -DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$FexSourceDirectory"
cmake --build "$BuildDirectory" --target lumen_ps5_fex_smoke \
  lumen_ps5_elf_preflight lumen_ps5_host_exception_test
ctest --test-dir "$BuildDirectory" -N -R '^lumen_ps5_'
ctest --test-dir "$BuildDirectory" --output-on-failure --no-tests=error \
  -R '^lumen_ps5_'
if [[ -n "$GameExecutable" ]]; then
  mkdir -p -- "$BuildDirectory/receipts"
  "$BuildDirectory/lumen_ps5_elf_preflight" "$GameExecutable" \
    --output "$BuildDirectory/receipts/elf-preflight.json"
fi
