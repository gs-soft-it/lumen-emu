#!/bin/bash

# Prove the macOS FEXCore lab's opt-in and configure-time fail-closed matrix.
set -euo pipefail

ScriptDirectory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RepositoryRoot=$(cd -- "$ScriptDirectory/.." && pwd)
LabSourceDirectory="$RepositoryRoot/macos/fexcore-lab"
PreparedSourceDirectory=${1:-${LUMEN_MACOS_FEXCORE_SOURCE_DIR:-/tmp/lumen-fex-macos-e17fdf9-darwin-v1}}
ExpectedPortCommit=2d729002c28b39c405636b6e69a0404ca3d97d05
ExpectedUpstreamCommit=e17fdf9e90a49614e16ac8414a687477fb9e4f7c
ExpectedOrigin=https://github.com/FEX-Emu/FEX.git

fail() {
    echo "verify_MacOS_FEXLab_ConfigureMatrix: $*" >&2
    exit 1
}

[[ $(uname -s) == Darwin ]] || fail 'this matrix requires macOS'
[[ $(uname -m) == arm64 ]] || fail 'this matrix requires native arm64'
[[ -d "$PreparedSourceDirectory/.git" ]] ||
    fail "prepared external FEX checkout not found: $PreparedSourceDirectory"
PreparedSourceDirectory=$(cd -- "$PreparedSourceDirectory" && pwd -P)
[[ $(git -C "$PreparedSourceDirectory" rev-parse HEAD) == \
   "$ExpectedPortCommit" ]] || fail 'prepared FEX checkout has the wrong port commit'

TemporaryDirectory=$(mktemp -d /tmp/lumen-fex-macos-configure-matrix.XXXXXX)
FixtureSourceDirectory="$TemporaryDirectory/fex-fixture"
cleanup() {
    [[ -n "${TemporaryDirectory:-}" && -d "$TemporaryDirectory" ]] &&
        rm -rf -- "$TemporaryDirectory"
}
trap cleanup EXIT

run_bounded() {
    local TimeoutSeconds=$1
    local StandardOutput=$2
    local StandardError=$3
    shift 3
    python3 - "$TimeoutSeconds" "$StandardOutput" "$StandardError" "$@" <<'PY'
import subprocess
import sys

timeout_seconds = float(sys.argv[1])
stdout_path = sys.argv[2]
stderr_path = sys.argv[3]
command = sys.argv[4:]
try:
    with open(stdout_path, "wb") as stdout_file, open(stderr_path, "wb") as stderr_file:
        completed = subprocess.run(
            command,
            stdout=stdout_file,
            stderr=stderr_file,
            timeout=timeout_seconds,
            check=False,
        )
except subprocess.TimeoutExpired:
    raise SystemExit(124)
raise SystemExit(completed.returncode if completed.returncode >= 0 else 128 - completed.returncode)
PY
}

declare -a CommonConfigureArguments=(
    -G Ninja
    -DCMAKE_C_COMPILER=/usr/bin/clang
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++
    -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_SYSROOT=macosx
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
    -DLUMEN_MACOS_FEXCORE_LAB=ON
)

NegativeCaseCount=0
expect_configure_failure() {
    local Name=$1
    local ExpectedDiagnostic=$2
    shift 2
    local BuildDirectory="$TemporaryDirectory/build-$Name"
    set +e
    run_bounded 45 "$TemporaryDirectory/$Name.out" \
        "$TemporaryDirectory/$Name.err" \
        cmake -S "$LabSourceDirectory" -B "$BuildDirectory" \
        "${CommonConfigureArguments[@]}" \
        -DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$FixtureSourceDirectory" \
        "$@"
    local ExitCode=$?
    set -e
    [[ $ExitCode -ne 0 && $ExitCode -ne 124 ]] ||
        fail "negative configure case $Name returned $ExitCode"
    grep -Fq "$ExpectedDiagnostic" \
        "$TemporaryDirectory/$Name.out" "$TemporaryDirectory/$Name.err" ||
        fail "negative configure case $Name missed its expected diagnostic"
    NegativeCaseCount=$((NegativeCaseCount + 1))
}

# Default OFF must configure without a FEX checkout and must expose no lab target.
run_bounded 45 "$TemporaryDirectory/default-off.out" \
    "$TemporaryDirectory/default-off.err" \
    cmake -S "$LabSourceDirectory" \
    -B "$TemporaryDirectory/build-default-off" \
    -G Ninja \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++
run_bounded 15 "$TemporaryDirectory/default-off-targets.out" \
    "$TemporaryDirectory/default-off-targets.err" \
    ninja -C "$TemporaryDirectory/build-default-off" -t targets
! grep -Fq 'lumen_macos_fexcore_bootstrap_smoke' \
    "$TemporaryDirectory/default-off-targets.out" ||
    fail 'default-OFF configuration exposed the lab target'

# Use only local repositories to construct a disposable, complete fixture.
run_bounded 45 "$TemporaryDirectory/clone.out" "$TemporaryDirectory/clone.err" \
    git clone --no-checkout --no-hardlinks \
    "$PreparedSourceDirectory" "$FixtureSourceDirectory"
git -C "$FixtureSourceDirectory" checkout -q --detach "$ExpectedPortCommit"
git -C "$FixtureSourceDirectory" remote set-url origin "$ExpectedOrigin"
declare -a RequiredSubmodulePaths=(
    External/fmt
    External/xxhash
    External/unordered_dense
    External/rpmalloc
)
for SubmodulePath in "${RequiredSubmodulePaths[@]}"; do
    git -C "$FixtureSourceDirectory" config \
        "submodule.$SubmodulePath.url" \
        "$PreparedSourceDirectory/$SubmodulePath"
done
run_bounded 45 "$TemporaryDirectory/submodules.out" \
    "$TemporaryDirectory/submodules.err" \
    git -c protocol.file.allow=always -C "$FixtureSourceDirectory" \
    submodule update --init -- "${RequiredSubmodulePaths[@]}"
[[ -z $(git -C "$FixtureSourceDirectory" status --porcelain=v1 \
    --untracked-files=all --ignore-submodules=none) ]] ||
    fail 'the disposable configure fixture is unexpectedly dirty'

expect_configure_failure release \
    'requires CMAKE_BUILD_TYPE=Debug' \
    -DCMAKE_BUILD_TYPE=Release
expect_configure_failure ios \
    'available only on macOS desktop' \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos
expect_configure_failure x86_64 \
    'requires a thin CMAKE_OSX_ARCHITECTURES=arm64 build' \
    -DCMAKE_OSX_ARCHITECTURES=x86_64
expect_configure_failure arm64e \
    'requires a thin CMAKE_OSX_ARCHITECTURES=arm64 build' \
    -DCMAKE_OSX_ARCHITECTURES=arm64e
expect_configure_failure universal \
    'requires a thin CMAKE_OSX_ARCHITECTURES=arm64 build' \
    '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
expect_configure_failure missing-source \
    'configured FEX source directory does not exist' \
    -DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$TemporaryDirectory/absent-fex-source"
expect_configure_failure in-tree-source \
    'FEX checkout must live outside the Lumen repository' \
    -DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$RepositoryRoot"

touch "$FixtureSourceDirectory/lumen-matrix-dirty-probe"
expect_configure_failure dirty-source \
    'external FEX checkout must be clean'
mv "$FixtureSourceDirectory/lumen-matrix-dirty-probe" \
    "$TemporaryDirectory/dirty-probe-consumed"

git -C "$FixtureSourceDirectory" remote set-url origin \
    https://invalid.example/FEX.git
expect_configure_failure origin-mismatch 'FEX origin mismatch'
git -C "$FixtureSourceDirectory" remote set-url origin "$ExpectedOrigin"

git -C "$FixtureSourceDirectory" checkout -q --detach "$ExpectedUpstreamCommit"
expect_configure_failure pin-mismatch 'FEX port commit mismatch'
git -C "$FixtureSourceDirectory" checkout -q --detach "$ExpectedPortCommit"

mv "$FixtureSourceDirectory/External/fmt/.git" \
    "$TemporaryDirectory/fmt-git-marker"
expect_configure_failure missing-submodule \
    'Required FEX submodule is not initialized: External/fmt'
mv "$TemporaryDirectory/fmt-git-marker" \
    "$FixtureSourceDirectory/External/fmt/.git"

perl -0pi -e 's/\A/LUMEN-MATRIX-TAMPER\n/' \
    "$FixtureSourceDirectory/LICENSE"
expect_configure_failure license-mismatch \
    'FEX closure license/notice hash mismatch: LICENSE'
git -C "$FixtureSourceDirectory" checkout -- LICENSE

[[ $NegativeCaseCount -eq 12 ]] ||
    fail "expected 12 negative configure cases, ran $NegativeCaseCount"
[[ -z $(git -C "$FixtureSourceDirectory" status --porcelain=v1 \
    --untracked-files=all --ignore-submodules=none) ]] ||
    fail 'the configure fixture was not restored after negative tests'

printf '%s\n' \
    "LUMEN_MACOS_FEXCORE_CONFIGURE_MATRIX_V0 pass=1 optOut=1 targetAbsent=1 negativeCases=$NegativeCaseCount release=deny ios=deny x86_64=deny arm64e=deny universal=deny missingSource=deny inTreeSource=deny dirtySource=deny originMismatch=deny pinMismatch=deny missingSubmodule=deny licenseMismatch=deny"
