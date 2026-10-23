#!/bin/bash

# Configure the current Mac FEX product in isolated caches. This checks backend
# selection and the generated emulator graph; it does not build or run Lumen.
# Every cache and log is retained, including failed cases. No existing build or
# prepared FEX checkout is modified or removed by this script.
set -euo pipefail

ScriptDirectory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RepositoryRoot=$(cd -- "$ScriptDirectory/.." && pwd)
PreparedSourceDirectory=${1:-}
OutputRoot=${2:-$RepositoryRoot/_Build/FexRuntimeConfigureMatrix}

fail() {
    echo "verify_MacOS_FEXRuntime_ConfigureMatrix: $*" >&2
    exit 1
}

[[ $# -ge 1 && $# -le 2 && -n "$PreparedSourceDirectory" ]] ||
    fail 'usage: verify_MacOS_FEXRuntime_ConfigureMatrix.sh <prepared-fex-source> [output-directory]'
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] ||
    fail 'this matrix requires native macOS arm64'
[[ $(sysctl -in sysctl.proc_translated 2>/dev/null || echo 0) != 1 ]] ||
    fail 'Rosetta-translated configuration is not allowed'
[[ $(getconf PAGESIZE) == 16384 ]] ||
    fail 'this matrix requires a 16384-byte host page'
[[ -d "$PreparedSourceDirectory/.git" ]] ||
    fail "prepared external FEX checkout not found: $PreparedSourceDirectory"
PreparedSourceDirectory=$(cd -- "$PreparedSourceDirectory" && pwd -P)
case "$PreparedSourceDirectory/" in
    "$RepositoryRoot/"*) fail 'the prepared FEX checkout must be outside the repository' ;;
esac
command -v cmake >/dev/null || fail 'cmake is required'
command -v ninja >/dev/null || fail 'ninja is required'
command -v python3 >/dev/null || fail 'python3 is required'

mkdir -p -- "$OutputRoot"
OutputRoot=$(cd -- "$OutputRoot" && pwd -P)
RunDirectory=$(mktemp -d "$OutputRoot/configure-matrix.XXXXXX")
printf 'Mac FEX configure-only matrix; retained evidence: %s\n' "$RunDirectory"

declare -a CommonConfigureArguments=(
    -G Ninja
    -DCMAKE_C_COMPILER=/usr/bin/clang
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_SYSROOT=macosx
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
    -DLUMEN_MACOS_X64_BACKEND=auto
    -DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$PreparedSourceDirectory"
    -DLUMEN_WITH_DESKTOP_FRONTEND=OFF
    -DLUMEN_WITH_CPU_X64_TEST=OFF
    -DLUMEN_WITH_CPU_A64_DYNAREC_TEST=OFF
    -DLUMEN_WITH_APPLE_VALIDATION=OFF
    -DLUMEN_A64_REJECTED_FASTBLOCK_PURGE=OFF
    -DLUMEN_A64_ARM_HOTPATH_COMPOSITE=OFF
    -DLUMEN_A64_ARM_LOADING_COMPOSITE=OFF
)

configure_case() {
    local CaseName=$1
    local TimeoutSeconds=$2
    shift 2
    local BuildDirectory="$RunDirectory/build-$CaseName"
    printf 'Configure %s; logs: %s/%s.{stdout,stderr}.log\n' \
        "$CaseName" "$RunDirectory" "$CaseName"
    python3 - "$TimeoutSeconds" "$RunDirectory/$CaseName" \
        cmake -S "$RepositoryRoot/source" -B "$BuildDirectory" \
        "${CommonConfigureArguments[@]}" "$@" <<'PY'
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

timeout_seconds = int(sys.argv[1])
prefix = Path(sys.argv[2])
command = sys.argv[3:]
Path(str(prefix) + ".command.json").write_text(json.dumps(command, indent=2) + "\n")
with Path(str(prefix) + ".stdout.log").open("wb") as stdout, \
        Path(str(prefix) + ".stderr.log").open("wb") as stderr:
    process = subprocess.Popen(command, stdout=stdout, stderr=stderr, start_new_session=True)
    try:
        result = process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
        result = 124
Path(str(prefix) + ".exit-code").write_text(str(result) + "\n")
raise SystemExit(result if result >= 0 else 128 - result)
PY
}

PositiveCaseCount=0
for BuildType in Debug Release RelWithDebInfo MinSizeRel; do
    CaseName="auto-$BuildType"
    BuildDirectory="$RunDirectory/build-$CaseName"
    mkdir -p "$BuildDirectory/.cmake/api/v1/query"
    : > "$BuildDirectory/.cmake/api/v1/query/codemodel-v2"
    if ! configure_case "$CaseName" 300 -DCMAKE_BUILD_TYPE="$BuildType"; then
        fail "positive case $CaseName failed; evidence: $RunDirectory"
    fi
    python3 - "$BuildDirectory" "$RunDirectory/$CaseName" "$BuildType" <<'PY'
import json
from pathlib import Path
import sys

build = Path(sys.argv[1])
prefix = Path(sys.argv[2])
build_type = sys.argv[3]
log = Path(str(prefix) + ".stdout.log").read_text()
if "macOS x64 backend: requested=auto effective=fex" not in log:
    raise SystemExit("auto did not select FEX")
reply = build / ".cmake/api/v1/reply"
indices = list(reply.glob("index-*.json"))
if len(indices) != 1:
    raise SystemExit("expected exactly one CMake File API index in the fresh cache")
index = json.loads(indices[0].read_text())
model_ref = index["reply"]["codemodel-v2"]
model = json.loads((reply / model_ref["jsonFile"]).read_text())
configurations = model["configurations"]
if len(configurations) != 1 or configurations[0]["name"] != build_type:
    raise SystemExit("unexpected CMake configuration")
targets = {target["name"]: target for target in configurations[0]["targets"]}
for required in ("emulator", "lumen_macos_fexcore_runtime", "lumen_macos_fexcore_runtime_probe"):
    if required not in targets:
        raise SystemExit(f"FEX product target missing: {required}")
emulator = json.loads((reply / targets["emulator"]["jsonFile"]).read_text())
sources = {Path(source["path"]).name for source in emulator["sources"]}
if "CpuA64Dynarec.cpp" in sources:
    raise SystemExit("the emulator target still compiles the NODA implementation")
if "CpuA64DynarecDisabled.cpp" not in sources:
    raise SystemExit("the emulator target is missing the disabled NODA provider")
definitions = {
    define["define"]
    for group in emulator["compileGroups"]
    for define in group.get("defines", [])
}
if "LUMEN_FEXCORE_RUNTIME=1" not in definitions:
    raise SystemExit("the emulator target is missing its FEX runtime definition")
result = {
    "configuration": build_type,
    "requestedBackend": "auto",
    "effectiveBackend": "fex",
    "configurePass": True,
    "fexRuntimeInEmulator": True,
    "nodaImplementationInEmulator": False,
    "disabledProviderInEmulator": True,
    "productBuilt": False,
    "runtimeExecuted": False,
}
Path(str(prefix) + ".graph.json").write_text(json.dumps(result, indent=2) + "\n")
print(f"PASS {build_type}: FEX emulator graph, NODA implementation absent")
PY
    PositiveCaseCount=$((PositiveCaseCount + 1))
done

NegativeCaseCount=0
expect_configure_failure() {
    local CaseName=$1
    local ExpectedDiagnostic=$2
    shift 2
    local ExitCode=0
    configure_case "$CaseName" 90 "$@" || ExitCode=$?
    [[ $ExitCode -ne 0 && $ExitCode -ne 124 ]] ||
        fail "negative case $CaseName returned $ExitCode; evidence: $RunDirectory"
    python3 - "$RunDirectory/$CaseName" "$ExpectedDiagnostic" <<'PY'
from pathlib import Path
import sys

prefix = Path(sys.argv[1])
expected = sys.argv[2]
log = "\n".join(Path(str(prefix) + suffix).read_text()
                for suffix in (".stdout.log", ".stderr.log"))
if " ".join(expected.split()) not in " ".join(log.split()):
    raise SystemExit(f"negative case missed diagnostic {expected!r}; evidence: {prefix.parent}")
print(f"PASS {prefix.name}: configure refused for the expected reason")
PY
    NegativeCaseCount=$((NegativeCaseCount + 1))
}

expect_configure_failure noda-arm64 \
    'macOS Apple Silicon requires FEX in every build configuration; NODA is not a supported backend' \
    -DLUMEN_MACOS_X64_BACKEND=noda
expect_configure_failure fex-x86_64 \
    'requires a thin arm64 build' \
    -DLUMEN_MACOS_X64_BACKEND=fex -DCMAKE_OSX_ARCHITECTURES=x86_64
expect_configure_failure fex-universal \
    'requires a thin arm64 build' \
    -DLUMEN_MACOS_X64_BACKEND=fex '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
expect_configure_failure auto-x86_64 \
    'requires a thin arm64 build' \
    -DCMAKE_OSX_ARCHITECTURES=x86_64
expect_configure_failure auto-universal \
    'requires a thin arm64 build' \
    '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
expect_configure_failure invalid-configuration \
    'requires a single Debug, Release, RelWithDebInfo or MinSizeRel build' \
    -DCMAKE_BUILD_TYPE=LumenInvalidConfiguration
expect_configure_failure missing-source \
    'The configured FEX source is not a Git checkout' \
    -DLUMEN_MACOS_FEXCORE_SOURCE_DIR="$RunDirectory/missing-fex-source"

Receipt="LUMEN_MACOS_FEXCORE_RUNTIME_CONFIGURE_MATRIX_V1 pass=1 positiveCases=$PositiveCaseCount negativeCases=$NegativeCaseCount autoDebug=fex autoRelease=fex autoRelWithDebInfo=fex autoMinSizeRel=fex noda=deny x86_64=deny universal=deny autoX86_64=deny autoUniversal=deny invalidConfiguration=deny missingSource=deny emulatorNodaImplementation=0 configureOnly=1 productBuilt=0 runtimeExecuted=0"
printf '%s\n' "$Receipt" > "$RunDirectory/receipt.txt"
printf '%s\nevidence=%s\n' "$Receipt" "$RunDirectory"
