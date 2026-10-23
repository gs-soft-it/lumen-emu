#!/bin/bash

# Configure the thin-arm64 product that routes x64 guests through FEXCore.
# The ordinary script selects FEX in every configuration. Argument 3 selects
# this entry point's build configuration; Release remains its default.
set -euo pipefail

ScriptDirectory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RepositoryRoot=$(cd -- "$ScriptDirectory/.." && pwd)
BuildDirectory=${1:-$RepositoryRoot/_Build/_ReleaseMacOSFEXRuntimeNinjaClang}
FEXSourceDirectory=${2:-${LUMEN_MACOS_FEXCORE_SOURCE_DIR:-}}
BuildType=${3:-Release}
DeploymentTarget=${LUMEN_MACOS_FEXCORE_DEPLOYMENT_TARGET:-12.0}

fail() {
	echo "cmake_MacOS_FEXRuntime_Ninja_Clang: $*" >&2
	exit 1
}

[[ $(uname -s) == Darwin ]] || fail 'this product requires macOS'
[[ $(uname -m) == arm64 ]] || fail 'this product requires a native arm64 process'
[[ $(sysctl -in sysctl.proc_translated 2>/dev/null || echo 0) != 1 ]] ||
	fail 'Rosetta-translated configuration is not allowed'
[[ $(getconf PAGESIZE) == 16384 ]] || fail 'this product requires a 16384-byte host page'
if [[ -z "$FEXSourceDirectory" ]]; then
	FEXSourceDirectory=$("$ScriptDirectory/prepare_Bundled_FEXCore.sh" macos)
fi
[[ -d "$FEXSourceDirectory/.git" ]] ||
	fail "prepared external FEX checkout not found: $FEXSourceDirectory"
[[ "$DeploymentTarget" == 12.0 ]] ||
	fail 'LUMEN_MACOS_FEXCORE_DEPLOYMENT_TARGET must be 12.0'
case "$BuildType" in
	Debug|Release|RelWithDebInfo|MinSizeRel) ;;
	*) fail "unsupported FEX build configuration: $BuildType" ;;
esac

mkdir -p -- "$BuildDirectory"
cmake -S "$RepositoryRoot/source" \
	-B "$BuildDirectory" \
	-G Ninja \
	-DCMAKE_C_COMPILER=/usr/bin/clang \
	-DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_SYSROOT=macosx \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$DeploymentTarget" \
	-DCMAKE_BUILD_TYPE="$BuildType" \
	-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
	-DCMAKE_INSTALL_PREFIX=_bin \
	-DLUMEN_MACOS_X64_BACKEND=fex \
	-DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$FEXSourceDirectory" \
	-DLUMEN_GEN4_FFMPEG_PREBUILT_DIR="${LUMEN_GEN4_FFMPEG_PREBUILT_DIR:-${LUMEN_PS5_FFMPEG_PREBUILT_DIR:-}}" \
	-DLUMEN_VULKAN_DYNAMIC_VERTEX_HOST_VISIBLE_DEFAULT=ON \
	-DLUMEN_A64_REJECTED_FASTBLOCK_PURGE=OFF \
	-DLUMEN_A64_ARM_HOTPATH_COMPOSITE=OFF \
	-DLUMEN_A64_ARM_LOADING_COMPOSITE=OFF \
	-DLUMEN_WITH_DESKTOP_FRONTEND=OFF \
	-DLUMEN_WITH_CPU_X64_TEST=OFF \
	-DLUMEN_WITH_CPU_A64_DYNAREC_TEST=OFF

printf '%s\n' \
	"Configured FEX $BuildType PS4 runtime in $BuildDirectory" \
	"FEX source: $FEXSourceDirectory"
