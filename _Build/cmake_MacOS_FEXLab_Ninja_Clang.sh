#!/bin/bash

# Configure the isolated, Debug-only, thin-arm64 macOS FEXCore bootstrap lab.
set -euo pipefail

ScriptDirectory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RepositoryRoot=$(cd -- "$ScriptDirectory/.." && pwd)
BuildDirectory=${1:-$RepositoryRoot/_Build/_DebugMacOSFexLab}
FEXSourceDirectory=${2:-${LUMEN_MACOS_FEXCORE_SOURCE_DIR:-/tmp/lumen-fex-macos-e17fdf9-darwin-v1}}
DeploymentTarget=${LUMEN_MACOS_FEXCORE_DEPLOYMENT_TARGET:-12.0}

fail() {
    echo "cmake_MacOS_FEXLab_Ninja_Clang: $*" >&2
    exit 1
}

[[ $(uname -s) == Darwin ]] || fail 'this laboratory requires macOS'
[[ $(uname -m) == arm64 ]] || fail 'this laboratory requires a native arm64 process'
[[ $(sysctl -in sysctl.proc_translated 2>/dev/null || echo 0) != 1 ]] ||
    fail 'Rosetta-translated configuration is not allowed'
[[ -d "$FEXSourceDirectory/.git" ]] ||
    fail "prepared external FEX checkout not found: $FEXSourceDirectory"

mkdir -p -- "$BuildDirectory"
cmake -S "$RepositoryRoot/macos/fexcore-lab" \
    -B "$BuildDirectory" \
    -G Ninja \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT=macosx \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DeploymentTarget" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DLUMEN_MACOS_FEXCORE_LAB=ON \
    -DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$FEXSourceDirectory"

printf '%s\n' \
    "Configured target lumen_macos_fexcore_bootstrap_smoke in $BuildDirectory"
