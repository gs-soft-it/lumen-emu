#!/bin/bash

# Resolve the pinned runtime source without changing upstream files. Android
# uses the bundled upstream; macOS keeps its deterministic Darwin port separate.
set -euo pipefail

ScriptDirectory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RepositoryRoot=$(cd -- "$ScriptDirectory/.." && pwd)
BundledSource="$RepositoryRoot/source/3rdparty/fex"
UpstreamOrigin=https://github.com/FEX-Emu/FEX.git
UpstreamCommit=e17fdf9e90a49614e16ac8414a687477fb9e4f7c

fail() {
    echo "prepare_Bundled_FEXCore: $*" >&2
    exit 1
}

usage() {
    echo 'usage: prepare_Bundled_FEXCore.sh <android|macos> [source-directory]'
    echo 'An explicit directory takes precedence over LUMEN_*_FEXCORE_SOURCE_DIR.'
    echo 'Without an override, use the bundled upstream or its external Darwin port.'
}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    usage
    exit 0
fi
[[ $# -ge 1 && $# -le 2 ]] || { usage >&2; exit 1; }
TargetProfile=$1
[[ "$TargetProfile" == android || "$TargetProfile" == macos ]] ||
    fail "unsupported target '$TargetProfile'"
command -v git >/dev/null 2>&1 || fail 'git is required'
command -v python3 >/dev/null 2>&1 || fail 'python3 is required'

resolve_path() {
    python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"
}

require_clean_checkout() {
    local SourceDirectory=$1
    local ExpectedCommit=$2
    local ObservedRoot ObservedHead ObservedStatus
    ObservedRoot=$(git -C "$SourceDirectory" rev-parse --show-toplevel) ||
        fail "source is not a Git checkout: $SourceDirectory"
    [[ $(resolve_path "$ObservedRoot") == "$SourceDirectory" ]] ||
        fail "source is not the checkout root: $SourceDirectory"
    ObservedHead=$(git -C "$SourceDirectory" rev-parse --verify HEAD)
    [[ "$ObservedHead" == "$ExpectedCommit" ]] ||
        fail "source pin mismatch at $SourceDirectory: $ObservedHead"
    ObservedStatus=$(git -C "$SourceDirectory" status --porcelain=v1 \
        --untracked-files=all --ignore-submodules=none)
    [[ -z "$ObservedStatus" ]] ||
        fail "source has local changes and was preserved: $SourceDirectory"
}

prepare_upstream() {
    local SourceDirectory=$1
    local ObservedOrigin Record SubmodulePath ExpectedCommit RecordedCommit
    local MissingSubmodules=()
    if [[ "$SourceDirectory" == "$BundledSource" &&
          ! -e "$SourceDirectory/.git" ]]; then
        git -c submodule.recurse=false -C "$RepositoryRoot" \
            submodule update --init -- source/3rdparty/fex >&2
    fi
    require_clean_checkout "$SourceDirectory" "$UpstreamCommit"
    ObservedOrigin=$(git -C "$SourceDirectory" remote get-url origin)
    case "$ObservedOrigin" in
        https://github.com/FEX-Emu/FEX.git|https://github.com/FEX-Emu/FEX|git@github.com:FEX-Emu/FEX.git) ;;
        *) fail "unexpected upstream origin: $ObservedOrigin" ;;
    esac

    local RequiredSubmodules=(
        'External/fmt|1be298e1bd68957e4cd352e1f676f00e07dcfb57'
        'External/xxhash|e626a72bc2321cd320e953a0ccf1584cad60f363'
        'External/unordered_dense|3234af2c03549bc85656bfd3a86993bf1cd8aef1'
        'External/rpmalloc|1d85c246cd827ead6865f4f880d4fef53f2b1864'
    )
    for Record in "${RequiredSubmodules[@]}"; do
        SubmodulePath=${Record%%|*}
        ExpectedCommit=${Record#*|}
        RecordedCommit=$(git -C "$SourceDirectory" rev-parse "HEAD:$SubmodulePath")
        [[ "$RecordedCommit" == "$ExpectedCommit" ]] ||
            fail "recorded dependency pin mismatch: $SubmodulePath"
        if [[ -e "$SourceDirectory/$SubmodulePath/.git" ]]; then
            require_clean_checkout "$SourceDirectory/$SubmodulePath" "$ExpectedCommit"
        else
            MissingSubmodules+=("$SubmodulePath")
        fi
    done
    if [[ ${#MissingSubmodules[@]} -gt 0 ]]; then
        git -c submodule.recurse=false -C "$SourceDirectory" \
            submodule update --init -- "${MissingSubmodules[@]}" >&2
    fi
    for Record in "${RequiredSubmodules[@]}"; do
        require_clean_checkout "$SourceDirectory/${Record%%|*}" "${Record#*|}"
    done
    require_clean_checkout "$SourceDirectory" "$UpstreamCommit"
    python3 - "$SourceDirectory" <<'PY'
import hashlib
from pathlib import Path
import sys

root = Path(sys.argv[1])
licenses = {
    'LICENSE': 'f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee',
    'FEXCore/LICENSE': 'f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee',
    'External/fmt/LICENSE': 'a65fcc5a1095fc33cc37e4fff69366e49d7e92f613558a2b95ed1b70e33758b4',
    'External/xxhash/LICENSE': '6ffedbc0f7878612d2b23589f1ff2ab15633e1df7963a5d9fc750ec5500c7e7a',
    'External/unordered_dense/LICENSE': '4f5a62734899ec0ba85d3e8264a7ed1fcb4bcb8f0b4359c0afd627b6314ae599',
    'External/rpmalloc/LICENSE': '9fdc653aacc736bef0cd12854f4a8d94f703ec5603fb5dca1be945b7f679918c',
    'External/SoftFloat-3e/include/SoftFloat-3e/softfloat.h': '907a309d402530a9647cfe8292228250539d4cafb6f2c1cbb9eaa6e7bd9d639c',
    'External/cephes/LICENSE': 'edf1440b625e6d434a986cdb535c702001afab8959d94ce6275441a58cfd0ebb',
}
for name, expected in licenses.items():
    if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
        raise SystemExit('prepare_Bundled_FEXCore: license mismatch: ' + name)
PY
}

if [[ "$TargetProfile" == android ]]; then
    SourceDirectory=$(resolve_path "${2:-${LUMEN_ANDROID_FEXCORE_SOURCE_DIR:-$BundledSource}}")
    prepare_upstream "$SourceDirectory"
else
    SourceDirectory=$(resolve_path "${2:-${LUMEN_MACOS_FEXCORE_SOURCE_DIR:-$RepositoryRoot/../lumen-deps/FEX-macos-bundled-e17fdf9}}")
    if [[ -e "$SourceDirectory" ]]; then
        # Preserve explicit prepared checkouts and let the existing verifier
        # enforce its complete Darwin provenance and dependency contract.
        bash "$ScriptDirectory/prepare_MacOS_FEXCore.sh" "$SourceDirectory" runtime >&2
    else
        prepare_upstream "$BundledSource"
        (
            # Apply local transport only to acquisition operations. Applying
            # insteadOf to identity queries would change remote get-url output.
            export LumenBundledFexSource="$BundledSource"
            export LumenBundledFexGit
            LumenBundledFexGit=$(command -v git)
            git() {
                local Argument Acquire=0 CloneDestination ConfigKey ConfigValue
                for Argument in "$@"; do
                    case "$Argument" in clone|submodule) Acquire=1; break ;; esac
                done
                if [[ $Acquire -eq 1 ]]; then
                    command "$LumenBundledFexGit" \
                        -c protocol.file.allow=always \
                        -c "url.$LumenBundledFexSource.insteadOf=https://github.com/FEX-Emu/FEX.git" \
                        -c "url.$LumenBundledFexSource/External/fmt.insteadOf=https://github.com/fmtlib/fmt.git" \
                        -c "url.$LumenBundledFexSource/External/xxhash.insteadOf=https://github.com/Cyan4973/xxHash.git" \
                        -c "url.$LumenBundledFexSource/External/unordered_dense.insteadOf=https://github.com/martinus/unordered_dense.git" \
                        -c "url.$LumenBundledFexSource/External/rpmalloc.insteadOf=https://github.com/FEX-Emu/rpmalloc.git" \
                        "$@" || return $?
                    if [[ ${1:-} == clone ]]; then
                        # A local clone copies existing objects, including
                        # partial-clone packs; preserve their promise metadata.
                        CloneDestination=${!#}
                        for ConfigKey in remote.origin.promisor remote.origin.partialclonefilter; do
                            if ConfigValue=$(command "$LumenBundledFexGit" \
                                -C "$LumenBundledFexSource" config --get "$ConfigKey"); then
                                command "$LumenBundledFexGit" -C "$CloneDestination" \
                                    config "$ConfigKey" "$ConfigValue"
                            fi
                        done
                    fi
                else
                    command "$LumenBundledFexGit" "$@"
                fi
            }
            export -f git
            bash "$ScriptDirectory/prepare_MacOS_FEXCore.sh" "$SourceDirectory" runtime >&2
        )
    fi
fi

printf '%s\n' "$SourceDirectory"
