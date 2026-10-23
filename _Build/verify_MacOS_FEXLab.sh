#!/bin/bash

# Build, execute and audit the bounded macOS FEXCore bootstrap milestone.
# Re-execute under an exact environment allowlist before inspecting sources,
# configuring, compiling, linking or executing the guest.  This excludes
# loader, Git, compiler and SDK environment injection from the formal receipt.
SafePath=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin
if [[ ${LUMEN_MACOS_FEXCORE_VERIFY_ENV_V0:-} != clean ]]; then
    exec /usr/bin/env -i \
        PATH="$SafePath" \
        HOME=/var/empty \
        TMPDIR=/tmp \
        LANG=C \
        LC_ALL=C \
        GIT_CONFIG_NOSYSTEM=1 \
        GIT_CONFIG_GLOBAL=/dev/null \
        ZERO_AR_DATE=1 \
        LUMEN_MACOS_FEXCORE_VERIFY_ENV_V0=clean \
        /bin/bash "$0" "$@"
fi

set -euo pipefail

[[ $PATH == "$SafePath" && $HOME == /var/empty && $TMPDIR == /tmp &&
   $LANG == C && $LC_ALL == C &&
   ${GIT_CONFIG_NOSYSTEM:-} == 1 &&
   ${GIT_CONFIG_GLOBAL:-} == /dev/null &&
   ${ZERO_AR_DATE:-} == 1 ]] || {
    echo 'verify_MacOS_FEXLab: sanitized environment contract was bypassed' >&2
    exit 1
}
ObservedEnvironmentNames=$(/usr/bin/env | /usr/bin/sed 's/=.*//' | /usr/bin/sort)
ExpectedEnvironmentNames=$(/usr/bin/printf '%s\n' \
    GIT_CONFIG_GLOBAL GIT_CONFIG_NOSYSTEM HOME LANG LC_ALL \
    LUMEN_MACOS_FEXCORE_VERIFY_ENV_V0 PATH PWD SHLVL TMPDIR ZERO_AR_DATE _ | \
    /usr/bin/sort)
[[ "$ObservedEnvironmentNames" == "$ExpectedEnvironmentNames" ]] || {
    echo 'verify_MacOS_FEXLab: unexpected variable entered sanitized environment' >&2
    /usr/bin/comm -3 \
        <(/usr/bin/printf '%s\n' "$ExpectedEnvironmentNames") \
        <(/usr/bin/printf '%s\n' "$ObservedEnvironmentNames") >&2 || true
    exit 1
}

ScriptDirectory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RepositoryRoot=$(cd -- "$ScriptDirectory/.." && pwd)
BuildDirectory=${1:-$RepositoryRoot/_Build/_DebugMacOSFexLab}
RequestedFEXSourceDirectory=${2:-}
ClosureManifest="$RepositoryRoot/macos/fexcore-lab/closure-tus-2d729002.txt"
LabPatchFile="$RepositoryRoot/macos/fexcore-lab/patches/fex-e17fdf9-darwin-arm64.patch"
ExpectedClosureSha256=9995f3f1eaafcd08c53a93a5c3475e6e7ac9c149973f8c9136f9757a39323e9a
ExpectedCommandGraphSha256=350557a39460ffe566eb0420ce7d6ca1f6cc6cc6a6ca98942d7d9e10bb7951f8
ExpectedLabCMakeSha256=2d4a0751a2fbd373dd713a800311dfa7ed02673e7dea355a5fe2e4887d2f7477
ExpectedLabHarnessSha256=3201e1c40fba97c6ab4381d2bbffdaf1397e5a47e9a0162ac60d7b82c62a349e
ExpectedLabManifestFileSha256=584e99e90d44add13d09f0aec574bbb67f8be7f67ab426d8d33d0b182ff62d1a
ExpectedConfigureMatrixScriptSha256=7fb978472d586c90fa99df1d5e514ba608fc8d4a532a43a5b91d1fae8bd83191
ExpectedCMakeSha256=70fe0b911ebfd1ba4e759b66a9124bbbe526399c1e26bd3e2c991ae36e02f7dd
ExpectedNinjaSha256=48761628046784f59bd789f5a72b88da99205c5849e5d19ae4d317854ae09e83
ExpectedCTestSha256=0ffc5b7938014521f1ff626d15a6f16bdf015e2372ee4ce9fe7ffe1871f24eed
ExpectedApplePythonSha256=271143990bc83af0fb2404a255038f5faafb96df1584ed7f085e5018c0f33ffb
ExpectedAppleClangSha256=7def90dd8829726686213a747fc5bff1583df933dae5edc55d755479e0bfe00a
ExpectedAppleGitSha256=10f9c1df894525ae4c7454258febab6d3d25071062b42cb48dbb1842cdffd2a9
ExpectedAppleArSha256=e49ffad64ad1cee722540fc5ecb00a230fd8071680682c60d9c851029d20e814
ExpectedUpstreamCommit=e17fdf9e90a49614e16ac8414a687477fb9e4f7c
ExpectedPortCommit=2d729002c28b39c405636b6e69a0404ca3d97d05
ExpectedPortTree=4847cd800a15a947ca0158d41abbdabf0b73e498
ExpectedOrigin=https://github.com/FEX-Emu/FEX.git
ExpectedPatchSha256=6c9f5fb6b2485941a5ddef34d4becc522d5c12721cc7f99281f9ca215e03b700
ReceiptPrefix=LUMEN_MACOS_FEXCORE_BOOTSTRAP_V0

fail() {
    echo "verify_MacOS_FEXLab: $*" >&2
    exit 1
}

check_hashed_tool() {
    local ToolPath=$1
    local ExpectedRealPath=$2
    local ExpectedSha256=$3
    local Phase=$4
    [[ -x "$ToolPath" ]] || fail "required tool is unavailable at $Phase: $ToolPath"
    [[ $(/bin/realpath "$ToolPath") == "$ExpectedRealPath" ]] ||
        fail "tool real path changed at $Phase: $ToolPath"
    [[ $(/usr/bin/shasum -a 256 "$ExpectedRealPath" | awk '{print $1}') == \
       "$ExpectedSha256" ]] || fail "tool bytes changed at $Phase: $ToolPath"
}

check_apple_tool() {
    local ToolName=$1
    local ExpectedPath=$2
    local ExpectedSha256=$3
    local Phase=$4
    local ObservedPath
    ObservedPath=$(/usr/bin/xcrun --find "$ToolName")
    [[ "$ObservedPath" == "$ExpectedPath" ]] ||
        fail "selected Apple tool path changed at $Phase: $ToolName"
    [[ $(/usr/bin/shasum -a 256 "$ObservedPath" | awk '{print $1}') == \
       "$ExpectedSha256" ]] || fail "selected Apple tool bytes changed at $Phase: $ToolName"
    /usr/bin/codesign --verify --strict "$ObservedPath" 2>/dev/null ||
        fail "selected Apple tool signature is invalid at $Phase: $ToolName"
}

check_toolchain_identity() {
    local Phase=$1
    check_hashed_tool /opt/homebrew/bin/cmake \
        /opt/homebrew/Cellar/cmake/4.4.1/bin/cmake \
        "$ExpectedCMakeSha256" "$Phase"
    check_hashed_tool /opt/homebrew/bin/ninja \
        /opt/homebrew/Cellar/ninja/1.13.2/bin/ninja \
        "$ExpectedNinjaSha256" "$Phase"
    check_hashed_tool /opt/homebrew/bin/ctest \
        /opt/homebrew/Cellar/cmake/4.4.1/bin/ctest \
        "$ExpectedCTestSha256" "$Phase"
    check_apple_tool python3 \
        /Applications/Xcode.app/Contents/Developer/usr/bin/python3 \
        "$ExpectedApplePythonSha256" "$Phase"
    check_apple_tool clang \
        /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang \
        "$ExpectedAppleClangSha256" "$Phase"
    check_apple_tool git \
        /Applications/Xcode.app/Contents/Developer/usr/bin/git \
        "$ExpectedAppleGitSha256" "$Phase"
    check_apple_tool ar \
        /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/ar \
        "$ExpectedAppleArSha256" "$Phase"
}

TemporaryDirectory=$(mktemp -d /tmp/lumen-fex-macos-verify.XXXXXX)
cleanup() {
    if [[ -n "${TemporaryDirectory:-}" &&
          "$TemporaryDirectory" == /tmp/lumen-fex-macos-verify.* &&
          -d "$TemporaryDirectory" ]]; then
        chmod -R u+w "$TemporaryDirectory" 2>/dev/null || true
        rm -rf -- "$TemporaryDirectory"
    fi
}
trap cleanup EXIT

[[ $(uname -s) == Darwin ]] || fail 'this gate requires macOS'
[[ $(uname -m) == arm64 ]] || fail 'this gate requires native arm64'
check_toolchain_identity initial-audit
SDKRoot=$(xcrun --sdk macosx --show-sdk-path)
SDKVersion=$(xcrun --sdk macosx --show-sdk-version)
[[ -d "$SDKRoot" && -n "$SDKVersion" ]] || fail 'active macOS SDK is unavailable'
CMakeCommand=/opt/homebrew/bin/cmake
CMakeVersion=$("$CMakeCommand" --version | sed -n '1s/^cmake version //p')
ClangVersion=$(/usr/bin/clang --version | \
    sed -n '1s/^Apple clang version \([^ ]*\).*/\1/p')
MacOSVersion=$(sw_vers -productVersion)
[[ -n "$CMakeVersion" && -n "$ClangVersion" && -n "$MacOSVersion" ]] ||
    fail 'toolchain identity could not be recorded'
[[ -f "$BuildDirectory/CMakeCache.txt" ]] ||
    fail "lab is not configured: $BuildDirectory"

FEXSourceDirectory=$(sed -n \
    's/^LUMEN_MACOS_FEXCORE_SOURCE_DIR:PATH=//p' \
    "$BuildDirectory/CMakeCache.txt")
[[ -n "$FEXSourceDirectory" && -d "$FEXSourceDirectory/.git" ]] ||
    fail 'CMakeCache does not identify a valid FEX source checkout'
FEXSourceDirectory=$(cd -- "$FEXSourceDirectory" && pwd -P)
if [[ -n "$RequestedFEXSourceDirectory" ]]; then
    [[ -d "$RequestedFEXSourceDirectory/.git" ]] ||
        fail "requested FEX source is not a checkout: $RequestedFEXSourceDirectory"
    RequestedFEXSourceDirectory=$(cd -- "$RequestedFEXSourceDirectory" && pwd -P)
    [[ "$RequestedFEXSourceDirectory" == "$FEXSourceDirectory" ]] ||
        fail 'requested FEX source differs from the checkout recorded in CMakeCache'
fi
OriginalFEXSourceDirectory=$FEXSourceDirectory
grep -Fqx 'LUMEN_MACOS_FEXCORE_LAB:BOOL=ON' \
    "$BuildDirectory/CMakeCache.txt" || fail 'lab opt-in is not ON in CMakeCache'
grep -Fqx 'CMAKE_BUILD_TYPE:STRING=Debug' \
    "$BuildDirectory/CMakeCache.txt" || fail 'lab build type is not Debug'
grep -Fqx 'CMAKE_OSX_ARCHITECTURES:STRING=arm64' \
    "$BuildDirectory/CMakeCache.txt" || fail 'lab architecture is not thin arm64'
grep -Eq '^CMAKE_C_COMPILER:(FILEPATH|STRING|UNINITIALIZED)=/usr/bin/clang$' \
    "$BuildDirectory/CMakeCache.txt" || fail 'lab C compiler is not /usr/bin/clang'
grep -Eq '^CMAKE_CXX_COMPILER:(FILEPATH|STRING|UNINITIALIZED)=/usr/bin/clang\+\+$' \
    "$BuildDirectory/CMakeCache.txt" || fail 'lab C++ compiler is not /usr/bin/clang++'
grep -Eq '^CMAKE_OSX_SYSROOT:(PATH|STRING|UNINITIALIZED)=macosx$' \
    "$BuildDirectory/CMakeCache.txt" || fail 'lab SDK selector is not macosx'
grep -Eq '^CMAKE_OSX_DEPLOYMENT_TARGET:(STRING|UNINITIALIZED)=12\.0$' \
    "$BuildDirectory/CMakeCache.txt" || fail 'lab deployment target is not 12.0'

run_bounded() {
    local TimeoutSeconds=$1
    local StandardOutput=$2
    local StandardError=$3
    shift 3
    /usr/bin/python3 - "$TimeoutSeconds" "$StandardOutput" "$StandardError" "$@" <<'PY'
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

check_lab_input_identity() {
    local LabRoot=$1
    local Phase=$2
    local Record RelativePath ExpectedSha256 ObservedSha256
    local -a RequiredLabInputs=(
        "CMakeLists.txt|$ExpectedLabCMakeSha256"
        "BootstrapSmoke.cpp|$ExpectedLabHarnessSha256"
        "closure-tus-2d729002.txt|$ExpectedLabManifestFileSha256"
        "patches/fex-e17fdf9-darwin-arm64.patch|$ExpectedPatchSha256"
    )
    for Record in "${RequiredLabInputs[@]}"; do
        RelativePath=${Record%%|*}
        ExpectedSha256=${Record#*|}
        [[ -f "$LabRoot/$RelativePath" &&
           ! -L "$LabRoot/$RelativePath" ]] ||
            fail "lab input is missing or not a regular file at $Phase: $RelativePath"
        ObservedSha256=$(shasum -a 256 "$LabRoot/$RelativePath" | awk '{print $1}')
        [[ "$ObservedSha256" == "$ExpectedSha256" ]] ||
            fail "lab input changed at $Phase: $RelativePath"
    done
}

check_configure_matrix_identity() {
    local MatrixScript=$1
    local Phase=$2
    [[ -f "$MatrixScript" && ! -L "$MatrixScript" ]] ||
        fail "configure matrix script is missing or not regular at $Phase"
    [[ $(shasum -a 256 "$MatrixScript" | awk '{print $1}') == \
       "$ExpectedConfigureMatrixScriptSha256" ]] ||
        fail "configure matrix script changed at $Phase"
}

check_source_identity() {
    local Phase=$1
    local DirtyState
    DirtyState=$(git -C "$FEXSourceDirectory" status --porcelain=v1 \
        --untracked-files=all --ignore-submodules=none)
    [[ -z "$DirtyState" ]] ||
        fail "external FEX checkout is dirty at $Phase: $DirtyState"
    [[ $(git -C "$FEXSourceDirectory" rev-parse HEAD) == \
       "$ExpectedPortCommit" ]] ||
        fail "external FEX checkout has the wrong commit at $Phase"
    [[ $(git -C "$FEXSourceDirectory" rev-parse HEAD^) == \
       "$ExpectedUpstreamCommit" ]] ||
        fail "external FEX checkout has the wrong parent at $Phase"
    [[ $(git -C "$FEXSourceDirectory" rev-parse 'HEAD^{tree}') == \
       "$ExpectedPortTree" ]] ||
        fail "external FEX checkout has the wrong tree at $Phase"
    [[ $(git -C "$FEXSourceDirectory" remote get-url origin) == \
       "$ExpectedOrigin" ]] ||
        fail "external FEX checkout has the wrong origin at $Phase"
    git -C "$FEXSourceDirectory" diff --no-ext-diff --exit-code HEAD -- \
        >/dev/null || fail "external FEX tracked bytes differ at $Phase"
    git -C "$FEXSourceDirectory" diff --no-ext-diff --cached --exit-code HEAD -- \
        >/dev/null || fail "external FEX index differs at $Phase"
    [[ $(shasum -a 256 \
        "$LabPatchFile" | \
        awk '{print $1}') == "$ExpectedPatchSha256" ]] ||
        fail "tracked Darwin port patch changed at $Phase"
    cmp -s \
        "$LabPatchFile" \
        <(git -C "$FEXSourceDirectory" diff --binary --no-ext-diff \
            "$ExpectedUpstreamCommit" "$ExpectedPortCommit") ||
        fail "tracked Darwin patch is not the exact port diff at $Phase"

    local Record SubmodulePath ExpectedSubmoduleCommit
    local ComponentPath ExpectedComponentTree LicensePath ExpectedLicenseSha256
    local -a RequiredSubmodules=(
        'External/fmt|1be298e1bd68957e4cd352e1f676f00e07dcfb57'
        'External/xxhash|e626a72bc2321cd320e953a0ccf1584cad60f363'
        'External/unordered_dense|3234af2c03549bc85656bfd3a86993bf1cd8aef1'
        'External/rpmalloc|1d85c246cd827ead6865f4f880d4fef53f2b1864'
    )
    for Record in "${RequiredSubmodules[@]}"; do
        SubmodulePath=${Record%%|*}
        ExpectedSubmoduleCommit=${Record#*|}
        [[ -e "$FEXSourceDirectory/$SubmodulePath/.git" ]] ||
            fail "submodule is unavailable at $Phase: $SubmodulePath"
        [[ $(git -C "$FEXSourceDirectory/$SubmodulePath" rev-parse HEAD) == \
           "$ExpectedSubmoduleCommit" ]] ||
            fail "submodule has the wrong commit at $Phase: $SubmodulePath"
        git -C "$FEXSourceDirectory/$SubmodulePath" diff --no-ext-diff \
            --exit-code HEAD -- >/dev/null ||
            fail "submodule tracked bytes differ at $Phase: $SubmodulePath"
        git -C "$FEXSourceDirectory/$SubmodulePath" diff --no-ext-diff \
            --cached --exit-code HEAD -- >/dev/null ||
            fail "submodule index differs at $Phase: $SubmodulePath"
    done

    local -a RequiredInTreeComponents=(
        'External/SoftFloat-3e|9b2659373bcc8bbfa28eda054b22214a0482ebd2'
        'External/cephes|f60034c28a4d816fb972ccb78d14ae560a973fa2'
    )
    for Record in "${RequiredInTreeComponents[@]}"; do
        ComponentPath=${Record%%|*}
        ExpectedComponentTree=${Record#*|}
        [[ $(git -C "$FEXSourceDirectory" rev-parse "HEAD:$ComponentPath") == \
           "$ExpectedComponentTree" ]] ||
            fail "in-tree component changed at $Phase: $ComponentPath"
    done

    local -a RequiredLicenses=(
        'LICENSE|f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee'
        'FEXCore/LICENSE|f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee'
        'External/fmt/LICENSE|a65fcc5a1095fc33cc37e4fff69366e49d7e92f613558a2b95ed1b70e33758b4'
        'External/xxhash/LICENSE|6ffedbc0f7878612d2b23589f1ff2ab15633e1df7963a5d9fc750ec5500c7e7a'
        'External/unordered_dense/LICENSE|4f5a62734899ec0ba85d3e8264a7ed1fcb4bcb8f0b4359c0afd627b6314ae599'
        'External/rpmalloc/LICENSE|9fdc653aacc736bef0cd12854f4a8d94f703ec5603fb5dca1be945b7f679918c'
        'External/SoftFloat-3e/include/SoftFloat-3e/softfloat.h|907a309d402530a9647cfe8292228250539d4cafb6f2c1cbb9eaa6e7bd9d639c'
        'External/cephes/LICENSE|edf1440b625e6d434a986cdb535c702001afab8959d94ce6275441a58cfd0ebb'
    )
    for Record in "${RequiredLicenses[@]}"; do
        LicensePath=${Record%%|*}
        ExpectedLicenseSha256=${Record#*|}
        [[ -f "$FEXSourceDirectory/$LicensePath" ]] ||
            fail "license/notice is unavailable at $Phase: $LicensePath"
        [[ $(shasum -a 256 "$FEXSourceDirectory/$LicensePath" | \
            awk '{print $1}') == "$ExpectedLicenseSha256" ]] ||
            fail "license/notice changed at $Phase: $LicensePath"
    done
}

# Validate the user-prepared checkout and the exact Lumen lab inputs, then
# materialize the only sources admitted to compilation from immutable Git
# objects.  The build never reads FEX bytes from the mutable supplied worktree.
check_lab_input_identity "$RepositoryRoot/macos/fexcore-lab" original-pre-snapshot
check_source_identity original-pre-snapshot

AuditRepositoryRoot="$TemporaryDirectory/lumen"
AuditLabSourceDirectory="$AuditRepositoryRoot/macos/fexcore-lab"
AuditFEXSourceDirectory="$TemporaryDirectory/fex"
AuditBuildDirectory="$TemporaryDirectory/build"
SnapshotConfigureMatrixScript="$AuditRepositoryRoot/_Build/verify_MacOS_FEXLab_ConfigureMatrix.sh"
mkdir -p -- "$AuditLabSourceDirectory/patches" "$AuditRepositoryRoot/_Build"
install -m 0444 "$RepositoryRoot/macos/fexcore-lab/CMakeLists.txt" \
    "$AuditLabSourceDirectory/CMakeLists.txt"
install -m 0444 "$RepositoryRoot/macos/fexcore-lab/BootstrapSmoke.cpp" \
    "$AuditLabSourceDirectory/BootstrapSmoke.cpp"
install -m 0444 "$RepositoryRoot/macos/fexcore-lab/closure-tus-2d729002.txt" \
    "$AuditLabSourceDirectory/closure-tus-2d729002.txt"
install -m 0444 "$RepositoryRoot/macos/fexcore-lab/patches/fex-e17fdf9-darwin-arm64.patch" \
    "$AuditLabSourceDirectory/patches/fex-e17fdf9-darwin-arm64.patch"
install -m 0555 "$ScriptDirectory/verify_MacOS_FEXLab_ConfigureMatrix.sh" \
    "$SnapshotConfigureMatrixScript"
check_lab_input_identity "$AuditLabSourceDirectory" snapshot-materialized
check_configure_matrix_identity \
    "$ScriptDirectory/verify_MacOS_FEXLab_ConfigureMatrix.sh" original-pre-matrix
check_configure_matrix_identity "$SnapshotConfigureMatrixScript" snapshot-pre-matrix
chmod -R a-w "$AuditRepositoryRoot"

ExpectedConfigureMatrixReceipt='LUMEN_MACOS_FEXCORE_CONFIGURE_MATRIX_V0 pass=1 optOut=1 targetAbsent=1 negativeCases=12 release=deny ios=deny x86_64=deny arm64e=deny universal=deny missingSource=deny inTreeSource=deny dirtySource=deny originMismatch=deny pinMismatch=deny missingSubmodule=deny licenseMismatch=deny'
run_bounded 600 "$TemporaryDirectory/configure-matrix.out" \
    "$TemporaryDirectory/configure-matrix.err" \
    "$SnapshotConfigureMatrixScript" "$OriginalFEXSourceDirectory"
[[ ! -s "$TemporaryDirectory/configure-matrix.err" ]] ||
    fail 'configure matrix emitted unexpected stderr'
[[ $(cat "$TemporaryDirectory/configure-matrix.out") == \
   "$ExpectedConfigureMatrixReceipt" ]] ||
    fail 'configure matrix did not emit the exact success receipt'
printf '%s\n' "$ExpectedConfigureMatrixReceipt"
check_configure_matrix_identity "$SnapshotConfigureMatrixScript" snapshot-post-matrix
check_configure_matrix_identity \
    "$ScriptDirectory/verify_MacOS_FEXLab_ConfigureMatrix.sh" original-post-matrix

git clone --quiet --no-hardlinks --no-checkout -- \
    "$OriginalFEXSourceDirectory" "$AuditFEXSourceDirectory"
git -C "$AuditFEXSourceDirectory" remote set-url origin "$ExpectedOrigin"
git -C "$AuditFEXSourceDirectory" checkout --quiet --detach "$ExpectedPortCommit"
while IFS='|' read -r SubmodulePath ExpectedSubmoduleCommit; do
    mkdir -p -- "$(dirname -- "$AuditFEXSourceDirectory/$SubmodulePath")"
    git clone --quiet --no-hardlinks --no-checkout -- \
        "$OriginalFEXSourceDirectory/$SubmodulePath" \
        "$AuditFEXSourceDirectory/$SubmodulePath"
    git -C "$AuditFEXSourceDirectory/$SubmodulePath" checkout --quiet \
        --detach "$ExpectedSubmoduleCommit"
done <<'SUBMODULES'
External/fmt|1be298e1bd68957e4cd352e1f676f00e07dcfb57
External/xxhash|e626a72bc2321cd320e953a0ccf1584cad60f363
External/unordered_dense|3234af2c03549bc85656bfd3a86993bf1cd8aef1
External/rpmalloc|1d85c246cd827ead6865f4f880d4fef53f2b1864
SUBMODULES

FEXSourceDirectory=$(cd -- "$AuditFEXSourceDirectory" && pwd -P)
LabPatchFile="$AuditLabSourceDirectory/patches/fex-e17fdf9-darwin-arm64.patch"
ClosureManifest="$AuditLabSourceDirectory/closure-tus-2d729002.txt"
check_source_identity snapshot-pre-read-only
chmod -R a-w "$AuditRepositoryRoot" "$AuditFEXSourceDirectory"
check_lab_input_identity "$AuditLabSourceDirectory" snapshot-read-only
check_source_identity snapshot-read-only

# Configure a fresh graph whose source roots are the read-only snapshots.
check_toolchain_identity pre-configure
"$CMakeCommand" -S "$AuditLabSourceDirectory" -B "$AuditBuildDirectory" \
    -G Ninja \
    -DCMAKE_C_COMPILER=/usr/bin/clang \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_SYSROOT=macosx \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DLUMEN_MACOS_FEXCORE_LAB=ON \
    -DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$FEXSourceDirectory"

BuildDirectory=$AuditBuildDirectory
Binary="$BuildDirectory/lumen_macos_fexcore_bootstrap_smoke"
CompileCommands="$BuildDirectory/compile_commands.json"
grep -Fqx 'LUMEN_MACOS_FEXCORE_LAB:BOOL=ON' \
    "$BuildDirectory/CMakeCache.txt" || fail 'snapshot lab opt-in is not ON'
grep -Fqx 'CMAKE_BUILD_TYPE:STRING=Debug' \
    "$BuildDirectory/CMakeCache.txt" || fail 'snapshot build type is not Debug'
grep -Fqx 'CMAKE_OSX_ARCHITECTURES:STRING=arm64' \
    "$BuildDirectory/CMakeCache.txt" || fail 'snapshot architecture is not thin arm64'
grep -Eq '^CMAKE_C_COMPILER:(FILEPATH|STRING|UNINITIALIZED)=/usr/bin/clang$' \
    "$BuildDirectory/CMakeCache.txt" || fail 'snapshot C compiler changed'
grep -Eq '^CMAKE_CXX_COMPILER:(FILEPATH|STRING|UNINITIALIZED)=/usr/bin/clang\+\+$' \
    "$BuildDirectory/CMakeCache.txt" || fail 'snapshot C++ compiler changed'
grep -Eq '^CMAKE_OSX_SYSROOT:(PATH|STRING|UNINITIALIZED)=macosx$' \
    "$BuildDirectory/CMakeCache.txt" || fail 'snapshot SDK selector changed'
grep -Eq '^CMAKE_OSX_DEPLOYMENT_TARGET:(STRING|UNINITIALIZED)=12\.0$' \
    "$BuildDirectory/CMakeCache.txt" || fail 'snapshot deployment target changed'
[[ -f "$CompileCommands" ]] || fail 'snapshot compile_commands.json is missing'
[[ -f "$ClosureManifest" ]] || fail 'snapshot closure manifest is missing'

# Freeze and audit the exact target closure before allowing either compilation,
# linking or guest-code execution.  This keeps the allowlist a true admission
# gate instead of a post-build observation.
check_source_identity pre-closure-audit
check_lab_input_identity "$AuditLabSourceDirectory" pre-closure-audit
Ninja=$(sed -n 's/^CMAKE_MAKE_PROGRAM:FILEPATH=//p' \
    "$BuildDirectory/CMakeCache.txt")
[[ "$Ninja" == /opt/homebrew/bin/ninja && -x "$Ninja" ]] ||
    fail 'CMakeCache does not identify the pinned Ninja executable'
NinjaVersion=$("$Ninja" --version)
[[ -n "$NinjaVersion" ]] || fail 'Ninja version could not be recorded'
"$Ninja" -C "$BuildDirectory" -t commands \
    lumen_macos_fexcore_bootstrap_smoke \
    >"$TemporaryDirectory/target-commands.txt"

/usr/bin/python3 - "$AuditRepositoryRoot" "$FEXSourceDirectory" "$BuildDirectory" \
    "$TemporaryDirectory/target-commands.txt" \
    "$TemporaryDirectory/actual-closure.txt" \
    "$TemporaryDirectory/compile-audit-counts.txt" \
    "$SDKRoot" "$CMakeCommand" \
    "$TemporaryDirectory/normalized-command-graph.txt" <<'PY'
import os
import shlex
import sys

repository_root_argument = os.path.normpath(sys.argv[1])
fex_root_argument = os.path.normpath(sys.argv[2])
build_root_argument = os.path.normpath(sys.argv[3])
repository_root = os.path.realpath(repository_root_argument)
fex_root = os.path.realpath(fex_root_argument)
build_root = os.path.realpath(build_root_argument)
commands_path = sys.argv[4]
output_path = sys.argv[5]
counts_path = sys.argv[6]
sdk_root = sys.argv[7]
cmake_command = sys.argv[8]
command_graph_path = sys.argv[9]
records = []
compile_count = 0
preserve_all_count = 0
generator_actions = []
archive_outputs = []
link_outputs = []
normalized_commands = []

def root_aliases(argument, resolved):
    aliases = {argument, resolved}
    for path in tuple(aliases):
        if path.startswith("/private/tmp/"):
            aliases.add(path.removeprefix("/private"))
        elif path.startswith("/tmp/"):
            aliases.add("/private" + path)
    return sorted(aliases, key=len, reverse=True)

normalization_roots = (
    (root_aliases(build_root_argument, build_root), "BUILDROOT"),
    (root_aliases(fex_root_argument, fex_root), "FEXROOT"),
    (root_aliases(repository_root_argument, repository_root), "LUMENROOT"),
)
with open(commands_path, encoding="utf-8") as commands_file:
    for line_number, command in enumerate(commands_file, start=1):
        normalized_command = command.rstrip("\n")
        for aliases, replacement in normalization_roots:
            for alias in aliases:
                normalized_command = normalized_command.replace(alias, replacement)
        normalized_commands.append(
            normalized_command
            .replace(sdk_root, "MACOS_SDKROOT")
            .replace(cmake_command, "CMAKE_COMMAND"))
        try:
            arguments = shlex.split(command)
        except ValueError as error:
            raise SystemExit(f"malformed Ninja command: {error}")
        if "-c" not in arguments or "-o" not in arguments:
            basenames = [os.path.basename(argument) for argument in arguments]
            known_generators = [
                name for name in (
                    "config_generator.py",
                    "json_ir_generator.py",
                    "json_ir_doc_generator.py",
                )
                if name in basenames
            ]
            if known_generators:
                if len(known_generators) != 1 or \
                        arguments.count("python3") != 1:
                    raise SystemExit(
                        f"invalid generator command {line_number}: {arguments!r}")
                generator_actions.extend(known_generators)
                continue
            if "gzip" in basenames:
                if basenames.count("gzip") != 1 or \
                        sum(arguments.count(name) for name in (
                            "gzip", "/usr/bin/gzip")) != 1:
                    raise SystemExit(
                        f"invalid gzip command {line_number}: {arguments!r}")
                generator_actions.append("gzip")
                continue
            if "/usr/bin/ar" in arguments:
                archive_index = arguments.index("/usr/bin/ar")
                if arguments[archive_index + 1] != "qc":
                    raise SystemExit(
                        f"invalid archive command {line_number}")
                archive_outputs.append(arguments[archive_index + 2])
                continue
            if "/usr/bin/clang++" in arguments and "-o" in arguments:
                if arguments.count("/usr/bin/clang++") != 1 or \
                        arguments.count("-isysroot") != 1 or \
                        arguments[arguments.index("-isysroot") + 1] != sdk_root or \
                        arguments.count("-arch") != 1 or \
                        arguments[arguments.index("-arch") + 1] != "arm64" or \
                        arguments.count("-mmacosx-version-min=12.0") != 1:
                    raise SystemExit(
                        f"link command {line_number} violates the toolchain contract")
                link_outputs.append(arguments[arguments.index("-o") + 1])
                continue
            raise SystemExit(
                f"unclassified non-compile target command {line_number}")
        else:
            pass
        if "-c" not in arguments or "-o" not in arguments:
            continue
        compile_count += 1
        if arguments[0] not in ("/usr/bin/clang", "/usr/bin/clang++"):
            raise SystemExit(
                f"compile command {line_number} uses an unqualified compiler")
        if arguments.count("-isysroot") != 1 or \
                arguments[arguments.index("-isysroot") + 1] != sdk_root:
            raise SystemExit(
                f"compile command {line_number} does not use the active SDK")
        if arguments.count("-arch") != 1 or \
                arguments[arguments.index("-arch") + 1] != "arm64":
            raise SystemExit(
                f"compile command {line_number} is not thin arm64")
        if arguments.count("-mmacosx-version-min=12.0") != 1:
            raise SystemExit(
                f"compile command {line_number} has the wrong deployment target")
        if arguments.count("-O2") != 1:
            raise SystemExit(
                f"compile command {line_number} must contain exactly one -O2")
        conflicting_optimization = [
            argument for argument in arguments
            if argument.startswith("-O") and argument != "-O2"
        ]
        if conflicting_optimization:
            raise SystemExit(
                f"compile command {line_number} has conflicting optimization: "
                + ", ".join(conflicting_optimization))
        for required_flag in (
                "-ffixed-x18", "-UNDEBUG", "-DASSERTIONS_ENABLED=1"):
            if arguments.count(required_flag) != 1:
                raise SystemExit(
                    f"compile command {line_number} must contain exactly one "
                    f"{required_flag}")
        for index, argument in enumerate(arguments):
            if argument == "-DNDEBUG" or argument.startswith("-DNDEBUG="):
                raise SystemExit(
                    f"compile command {line_number} defines NDEBUG")
            if argument == "-D" and index + 1 < len(arguments) and \
                    (arguments[index + 1] == "NDEBUG" or
                     arguments[index + 1].startswith("NDEBUG=")):
                raise SystemExit(
                    f"compile command {line_number} defines split NDEBUG")
            if argument == "-UASSERTIONS_ENABLED" or \
                    (argument.startswith("-DASSERTIONS_ENABLED") and
                     argument != "-DASSERTIONS_ENABLED=1"):
                raise SystemExit(
                    f"compile command {line_number} conflicts with assertions")
        preserve_definitions = [
            argument for argument in arguments
            if argument.startswith("-DFEXCORE_HAS_PRESERVE_ALL_ATTR=")
        ]
        if preserve_definitions:
            if preserve_definitions != ["-DFEXCORE_HAS_PRESERVE_ALL_ATTR=1"]:
                raise SystemExit(
                    f"compile command {line_number} has invalid preserve_all ABI")
            preserve_all_count += 1
        source = os.path.realpath(arguments[arguments.index("-c") + 1])
        object_path = arguments[arguments.index("-o") + 1]
        if os.path.isabs(object_path):
            if not object_path.startswith(build_root + os.sep):
                raise SystemExit(f"object outside build directory: {object_path}")
            object_path = os.path.relpath(object_path, build_root)
        else:
            object_path = os.path.normpath(object_path)
        object_path = object_path.replace(
            fex_root.lstrip(os.sep) + os.sep, "FEXROOT/")
        if source.startswith(fex_root + os.sep):
            source = "FEX/" + os.path.relpath(source, fex_root)
        elif source.startswith(repository_root + os.sep):
            source = "LUMEN/" + os.path.relpath(source, repository_root)
        else:
            raise SystemExit(f"source outside admitted roots: {source}")
        records.append(f"{object_path}|{source}")
with open(output_path, "w", encoding="utf-8", newline="\n") as output_file:
    for record in sorted(records):
        output_file.write(record + "\n")
with open(command_graph_path, "w", encoding="utf-8", newline="\n") as graph_file:
    for normalized_command in normalized_commands:
        graph_file.write(normalized_command + "\n")
with open(counts_path, "w", encoding="ascii", newline="\n") as counts_file:
    expected_generators = sorted([
        "config_generator.py",
        "gzip",
        "json_ir_doc_generator.py",
        "json_ir_generator.py",
    ])
    expected_archives = sorted([
        "fex-cephes/libcephes_128bit.a",
        "fex-core/Source/libFEXCore.a",
        "fex-core/Source/libFEXCore_Base.a",
        "fex-core/Source/libJemallocLibs.a",
        "fex-fmt/libfmtd.a",
        "fex-rpmalloc/librpmalloc.a",
        "fex-softfloat/libsoftfloat_3e.a",
        "fex-xxhash/libxxhash.a",
    ])
    if sorted(generator_actions) != expected_generators:
        raise SystemExit(
            "generator action closure mismatch: " + repr(generator_actions))
    if sorted(archive_outputs) != expected_archives:
        raise SystemExit(
            "archive action closure mismatch: " + repr(archive_outputs))
    if link_outputs != ["lumen_macos_fexcore_bootstrap_smoke"]:
        raise SystemExit("link action closure mismatch: " + repr(link_outputs))
    counts_file.write(
        f"{compile_count} {preserve_all_count} "
        f"{len(generator_actions)} {len(archive_outputs)} {len(link_outputs)}\n")
PY

ObservedManifestSha256=$(sed '/^#/d' "$ClosureManifest" | \
    shasum -a 256 | awk '{print $1}')
[[ "$ObservedManifestSha256" == "$ExpectedClosureSha256" ]] ||
    fail 'tracked target closure manifest hash mismatch'
ObservedCommandGraphSha256=$(shasum -a 256 \
    "$TemporaryDirectory/normalized-command-graph.txt" | awk '{print $1}')
[[ "$ObservedCommandGraphSha256" == "$ExpectedCommandGraphSha256" ]] ||
    fail "normalized target command graph hash mismatch: expected $ExpectedCommandGraphSha256, got $ObservedCommandGraphSha256"
cmp -s <(sed '/^#/d' "$ClosureManifest") \
    "$TemporaryDirectory/actual-closure.txt" ||
    fail 'actual target compile closure differs from the exact allowlist'
! grep -Fq 'DummyHandlers' "$TemporaryDirectory/actual-closure.txt" ||
    fail 'unsafe upstream dummy handlers entered the target closure'

read -r CompileLineCount PreserveAllLineCount GeneratorActionCount \
    ArchiveActionCount LinkActionCount \
    <"$TemporaryDirectory/compile-audit-counts.txt"
[[ $CompileLineCount -eq 159 ]] ||
    fail "expected 159 compile commands, audited $CompileLineCount"
[[ $PreserveAllLineCount -eq 154 ]] ||
    fail "expected 154 preserve_all-aware commands, audited $PreserveAllLineCount"
[[ $GeneratorActionCount -eq 4 && $ArchiveActionCount -eq 8 &&
   $LinkActionCount -eq 1 ]] ||
    fail 'non-compile target action closure has an unexpected size'

check_source_identity pre-build
check_lab_input_identity "$AuditLabSourceDirectory" pre-build
check_toolchain_identity pre-build
"$CMakeCommand" --build "$BuildDirectory" \
    --target lumen_macos_fexcore_bootstrap_smoke \
    --clean-first
[[ -x "$Binary" ]] || fail "bootstrap executable is missing: $Binary"
check_source_identity post-build
check_lab_input_identity "$AuditLabSourceDirectory" post-build
check_toolchain_identity post-build
"$Ninja" -C "$BuildDirectory" -t commands \
    lumen_macos_fexcore_bootstrap_smoke \
    >"$TemporaryDirectory/post-build-target-commands.txt"
cmp -s "$TemporaryDirectory/target-commands.txt" \
    "$TemporaryDirectory/post-build-target-commands.txt" ||
    fail 'target command graph changed between admission audit and link'
chmod a-w "$Binary"
PreExecutionBinarySha256=$(shasum -a 256 "$Binary" | awk '{print $1}')
PreExecutionBinaryIdentity=$(/usr/bin/stat -f '%d:%i:%z' "$Binary")

check_binary_identity() {
    local Phase=$1
    [[ $(/usr/bin/stat -f '%d:%i:%z' "$Binary") == \
       "$PreExecutionBinaryIdentity" ]] ||
        fail "bootstrap artifact inode/size changed at $Phase"
    [[ $(shasum -a 256 "$Binary" | awk '{print $1}') == \
       "$PreExecutionBinarySha256" ]] ||
        fail "bootstrap artifact bytes changed at $Phase"
}

run_denied_case() {
    local Name=$1
    local ExpectedDeniedReceipt
    shift
    check_binary_identity "selector-$Name-pre-run"
    set +e
    run_bounded 30 "$TemporaryDirectory/$Name.out" \
        "$TemporaryDirectory/$Name.err" "$@"
    local ExitCode=$?
    set -e
    check_binary_identity "selector-$Name-post-run"
    [[ $ExitCode -eq 64 ]] ||
        fail "selector case $Name returned $ExitCode instead of 64"
    [[ $(grep -c "^$ReceiptPrefix " "$TemporaryDirectory/$Name.out") -eq 1 ]] ||
        fail "selector case $Name did not emit exactly one receipt"
    ExpectedDeniedReceipt="$ReceiptPrefix pass=0 code=64 reason=selector-denied bootstrapSmoke=0 runtimeReady=0 romRoute=0 fallback=0 nodaRoute=0 precompiledOnly=1 lazyJit=0 hltStop=exit-on-any-hlt-debug-only hostType=unknown hostPageSize=0 guestPageSize=4096 mapJit=0 rawRwxAttempted=0 threadWx=0 jitPublisher=thread-toggle-debug-only icache=sys_icache_invalidate x18AllocatorExcluded=1 x18RuntimeCanary=0 pageGuardQualified=0 context=0 guestExecuted=0 opt=O2 assertions=1 maxInst=5000 smcConfig=mtrack guestCodeProtection=ro syscallPolicy=abort-deny precompileCalls=0 executePrecompileHookDelta=0 executeCompileMissDelta=0 syscallCalls=0 queryAllowed=0 queryDenied=0 markAccepted=0 markRejected=0 invalidates=0 lookupMisses=0 overcommitMarks=0 overcommitUnmarks=0 sleepCalls=0 monoHacks=0 logicalCores=0 guestResult=0x0000000000000000 upstream=e17fdf9 port=2d729002"
    [[ $(cat "$TemporaryDirectory/$Name.out") == "$ExpectedDeniedReceipt" ]] ||
        fail "selector case $Name did not fail before context creation"
}

run_denied_case absent env -u LUMEN_MACOS_FEXCORE_RUN "$Binary"
run_denied_case zero env LUMEN_MACOS_FEXCORE_RUN=0 "$Binary"
run_denied_case empty env LUMEN_MACOS_FEXCORE_RUN= "$Binary"
run_denied_case malformed env LUMEN_MACOS_FEXCORE_RUN=true "$Binary"

for Cycle in 1 2; do
    check_binary_identity "positive-$Cycle-pre-run"
    run_bounded 30 "$TemporaryDirectory/positive-$Cycle.out" \
        "$TemporaryDirectory/positive-$Cycle.err" \
        env LUMEN_MACOS_FEXCORE_RUN=1 "$Binary"
    check_binary_identity "positive-$Cycle-post-run"
    [[ $(grep -c "^$ReceiptPrefix " \
        "$TemporaryDirectory/positive-$Cycle.out") -eq 1 ]] ||
        fail "positive cycle $Cycle did not emit exactly one receipt"
    grep -Eq \
        "^$ReceiptPrefix pass=1 code=0 reason=ok bootstrapSmoke=1 runtimeReady=0 romRoute=0 fallback=0 nodaRoute=0 precompiledOnly=1 lazyJit=0 hltStop=exit-on-any-hlt-debug-only hostType=unknown hostPageSize=16384 guestPageSize=4096 mapJit=1 rawRwxAttempted=0 threadWx=1 jitPublisher=thread-toggle-debug-only icache=sys_icache_invalidate x18AllocatorExcluded=1 x18RuntimeCanary=0 pageGuardQualified=0 context=1 guestExecuted=1 opt=O2 assertions=1 maxInst=5000 smcConfig=mtrack guestCodeProtection=ro syscallPolicy=abort-deny precompileCalls=2 executePrecompileHookDelta=1 executeCompileMissDelta=0 syscallCalls=0 queryAllowed=([1-9][0-9]*) queryDenied=1 markAccepted=([1-9][0-9]*) markRejected=0 invalidates=0 lookupMisses=([1-9][0-9]*) overcommitMarks=([1-9][0-9]*) overcommitUnmarks=([1-9][0-9]*) sleepCalls=0 monoHacks=0 logicalCores=([1-9]|[1-9][0-9]|1[0-9]{2}|2[0-4][0-9]|25[0-5]) guestResult=0x0000000012345678 upstream=e17fdf9 port=2d729002$" \
        "$TemporaryDirectory/positive-$Cycle.out" ||
        fail "positive cycle $Cycle receipt is incomplete or dishonest"
done
cmp -s "$TemporaryDirectory/positive-1.out" \
    "$TemporaryDirectory/positive-2.out" ||
    fail 'the two cold-process receipts differ'

check_binary_identity pre-ctest
/opt/homebrew/bin/ctest --test-dir "$BuildDirectory" --output-on-failure \
    -R '^lumen_macos_fexcore_bootstrap_smoke$'
check_binary_identity post-ctest

check_binary_identity pre-artifact-audit
file "$Binary" | grep -Fq 'Mach-O 64-bit executable arm64' ||
    fail 'bootstrap artifact is not a Mach-O arm64 executable'
[[ $(lipo -archs "$Binary") == arm64 ]] || fail 'bootstrap artifact is not thin arm64'
otool -l "$Binary" >"$TemporaryDirectory/otool-load-commands.txt"
! grep -q 'cmd LC_RPATH' "$TemporaryDirectory/otool-load-commands.txt" ||
    fail 'unexpected LC_RPATH'
! awk '
    $1 == "initprot" && ($2 == "0x00000006" || $2 == "0x00000007") { found = 1 }
    END { exit found ? 0 : 1 }
' "$TemporaryDirectory/otool-load-commands.txt" ||
    fail 'Mach-O contains a static writable-executable segment'

LinkedLibraries=$(otool -L "$Binary" | tail -n +2 | awk '{print $1}')
while IFS= read -r Library; do
    case "$Library" in
        /usr/lib/libSystem.B.dylib|/usr/lib/libc++.1.dylib) ;;
        *) fail "unexpected linked library: $Library" ;;
    esac
done <<<"$LinkedLibraries"

nm -nm "$Binary" >"$TemporaryDirectory/nm.txt"
strings -a "$Binary" >"$TemporaryDirectory/strings.txt"
grep -Fq 'CreateNewContext' "$TemporaryDirectory/nm.txt" ||
    fail 'linked artifact does not contain the expected FEXCore context symbol'
! grep -Eq 'CpuA64Dynarec|NODA' "$TemporaryDirectory/nm.txt" ||
    fail 'standalone FEX lab unexpectedly contains a NODA symbol'
! grep -Eq \
    'LUMEN_X64_A64_DYNAREC|LUMEN_ANDROID_FEXCORE_RUNTIME_V1' \
    "$TemporaryDirectory/strings.txt" ||
    fail 'standalone FEX lab unexpectedly contains a product/Android marker'
! grep -Eq \
    '/proc/self/maps|SYS_futex|__ulock_|MAP_FIXED_NOREPLACE' \
    "$TemporaryDirectory/strings.txt" ||
    fail 'standalone FEX lab contains a forbidden Linux/private Darwin path'
! grep -Fq '__pth_jit_func' "$TemporaryDirectory/otool-load-commands.txt" ||
    fail 'bootstrap artifact unexpectedly contains a JIT callback allowlist'

codesign -d --entitlements - "$Binary" \
    >"$TemporaryDirectory/codesign-entitlements.txt" 2>&1
! grep -Fq 'com.apple.security.cs.' \
    "$TemporaryDirectory/codesign-entitlements.txt" ||
    fail 'bootstrap artifact has an unexpected code-signing entitlement'

check_source_identity final-audit
check_lab_input_identity "$AuditLabSourceDirectory" final-audit
check_lab_input_identity "$RepositoryRoot/macos/fexcore-lab" original-final-audit
check_configure_matrix_identity "$SnapshotConfigureMatrixScript" snapshot-final-audit
check_configure_matrix_identity \
    "$ScriptDirectory/verify_MacOS_FEXLab_ConfigureMatrix.sh" original-final-audit
check_binary_identity post-artifact-audit
check_toolchain_identity final-audit

BinarySha256=$(shasum -a 256 "$Binary" | awk '{print $1}')
[[ "$BinarySha256" == "$PreExecutionBinarySha256" ]] ||
    fail 'bootstrap artifact changed after the clean snapshot build'
BinaryUuid=$(dwarfdump --uuid "$Binary" | awk '{print $2}')
Receipt=$(cat "$TemporaryDirectory/positive-1.out")
printf '%s\n' "$Receipt"
printf '%s\n' \
    "LUMEN_MACOS_FEXCORE_AUDIT_V0 pass=1 bootstrapSmoke=1 runtimeReady=0 romRoute=0 nodaRoute=0 configureMatrix=1 coldProcesses=2 selectorNegative=4 compileCommands=$CompileLineCount preserveAllCommands=$PreserveAllLineCount generatorActions=$GeneratorActionCount archiveActions=$ArchiveActionCount linkActions=$LinkActionCount closureSha256=$ExpectedClosureSha256 commandGraphSha256=$ExpectedCommandGraphSha256 commandGraphStable=1 cleanFirst=1 sourceSnapshot=git-objects-read-only environmentAllowlist=1 toolchainPinned=1 binaryIdentityStable=1 labCMakeSha256=$ExpectedLabCMakeSha256 labHarnessSha256=$ExpectedLabHarnessSha256 labManifestFileSha256=$ExpectedLabManifestFileSha256 configureMatrixScriptSha256=$ExpectedConfigureMatrixScriptSha256 cmakeSha256=$ExpectedCMakeSha256 ninjaSha256=$ExpectedNinjaSha256 applePythonSha256=$ExpectedApplePythonSha256 appleClangSha256=$ExpectedAppleClangSha256 patchDiffExact=1 thinArm64=1 staticWx=0 unexpectedDylibs=0 sourceClean=1 clang=$ClangVersion cmake=$CMakeVersion ninja=$NinjaVersion sdk=$SDKVersion deployment=12.0 macOS=$MacOSVersion binarySha256=$BinarySha256 uuid=$BinaryUuid"
