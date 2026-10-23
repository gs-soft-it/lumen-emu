#!/bin/bash

# Prepare the exact external FEX checkout consumed by the macOS bootstrap lab
# or, with the explicit `runtime` profile, its deterministic runtime extension.
# Existing unexpected state is never reset or overwritten.
set -euo pipefail

ScriptDirectory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
RepositoryRoot=$(cd -- "$ScriptDirectory/.." && pwd)
SourceDirectory=${1:-${LUMEN_MACOS_FEXCORE_SOURCE_DIR:-/tmp/lumen-fex-macos-e17fdf9-darwin-v1}}
TargetProfile=${2:-bootstrap}
PatchPath="$RepositoryRoot/macos/fexcore-lab/patches/fex-e17fdf9-darwin-arm64.patch"
RuntimePatch1Path="$RepositoryRoot/macos/fexcore-runtime/patches/fex-2d729002-to-bf70bef0.patch"
RuntimePatch2Path="$RepositoryRoot/macos/fexcore-runtime/patches/fex-bf70bef0-to-02a37ec9.patch"
RuntimePatch3Path="$RepositoryRoot/macos/fexcore-runtime/patches/fex-02a37ec9-to-c9c5a132.patch"
UpstreamOrigin=https://github.com/FEX-Emu/FEX.git
UpstreamCommit=e17fdf9e90a49614e16ac8414a687477fb9e4f7c
PortCommit=2d729002c28b39c405636b6e69a0404ca3d97d05
PortTree=4847cd800a15a947ca0158d41abbdabf0b73e498
PatchSha256=6c9f5fb6b2485941a5ddef34d4becc522d5c12721cc7f99281f9ca215e03b700
RuntimeCommit1=bf70bef0ba5c92b36d732d667ab7f2dd9f28a335
RuntimeTree1=673b38305d95fedcff485ad69121128c6a9f239d
RuntimePatch1Sha256=d27c571ebd1edd7689e847073ece24243c6729fd8bab61970120e63f974c3afa
RuntimeCommit2=02a37ec9bb7873ba71fe619df858b487e2fea7ea
RuntimeTree2=d7b227ac0029101ac1676e30a0ff3d78121fc4f4
RuntimePatch2Sha256=042ded7f10b677537712892cc1b01496684cb81403b647198f7db99de8d89cb3
RuntimeCommit3=c9c5a132c18175656936b8c25c1d453fe6e262a0
RuntimeTree3=6d7c5e14fb2e80e117bc160a3460e14e889a678d
RuntimePatch3Sha256=cb155e1d5d34f041a2677b3cb7e19d827f642385638e6dd6318fd73727b9abb9
RuntimePatchChainSha256=c3f5ff7abb790ad72d54ba2e3883e16568fc9628c9aad3f6432141ce1c611c5e

fail() {
    echo "prepare_MacOS_FEXCore: $*" >&2
    exit 1
}

verify_exact_patch_commit() {
    local ParentCommit=$1
    local ExpectedCommit=$2
    local ExpectedTree=$3
    local TrackedPatch=$4
    local ObservedCommit
    local ObservedParent
    local ObservedTree

    ObservedCommit=$(git -C "$SourceDirectory" rev-parse --verify \
        "$ExpectedCommit^{commit}") ||
        fail "required deterministic commit is missing: $ExpectedCommit"
    ObservedParent=$(git -C "$SourceDirectory" rev-parse --verify \
        "$ExpectedCommit^")
    ObservedTree=$(git -C "$SourceDirectory" rev-parse --verify \
        "$ExpectedCommit^{tree}")
    [[ "$ObservedCommit" == "$ExpectedCommit" ]] ||
        fail "non-deterministic commit identity: expected $ExpectedCommit, got $ObservedCommit"
    [[ "$ObservedParent" == "$ParentCommit" ]] ||
        fail "unexpected parent for $ExpectedCommit: $ObservedParent"
    [[ "$ObservedTree" == "$ExpectedTree" ]] ||
        fail "unexpected tree for $ExpectedCommit: $ObservedTree"
    cmp -s "$TrackedPatch" \
        <(git -C "$SourceDirectory" diff --binary --no-ext-diff \
            "$ParentCommit" "$ExpectedCommit") ||
        fail "tracked patch is not the exact diff for $ExpectedCommit"
}

apply_runtime_patch_commit() {
    local TrackedPatch=$1
    local ParentCommit=$2
    local ExpectedCommit=$3
    local ExpectedTree=$4
    local AuthorName=$5
    local AuthorEmail=$6
    local CommitDate=$7
    local CommitSubject=$8
    shift 8

    local ObservedHead
    ObservedHead=$(git -C "$SourceDirectory" rev-parse --verify HEAD)
    [[ "$ObservedHead" == "$ParentCommit" ]] ||
        fail "runtime patch parent mismatch: expected $ParentCommit, got $ObservedHead"

    git -C "$SourceDirectory" apply --check "$TrackedPatch"
    git -C "$SourceDirectory" apply "$TrackedPatch"
    git -C "$SourceDirectory" add -- "$@"
    cmp -s "$TrackedPatch" \
        <(git -C "$SourceDirectory" diff --cached --binary --no-ext-diff) ||
        fail "staged runtime diff does not match $TrackedPatch"

    GIT_AUTHOR_NAME="$AuthorName" \
    GIT_AUTHOR_EMAIL="$AuthorEmail" \
    GIT_AUTHOR_DATE="$CommitDate" \
    GIT_COMMITTER_NAME="$AuthorName" \
    GIT_COMMITTER_EMAIL="$AuthorEmail" \
    GIT_COMMITTER_DATE="$CommitDate" \
        git -c commit.gpgSign=false -C "$SourceDirectory" commit \
        --no-verify -m "$CommitSubject"

    verify_exact_patch_commit \
        "$ParentCommit" "$ExpectedCommit" "$ExpectedTree" "$TrackedPatch"
}

[[ $(uname -s) == Darwin ]] || fail 'this preparation is supported only on macOS'
[[ "$TargetProfile" == bootstrap || "$TargetProfile" == runtime ]] ||
    fail "unsupported target profile '$TargetProfile'; use bootstrap or runtime"
[[ -f "$PatchPath" ]] || fail "tracked Darwin patch is missing: $PatchPath"
ObservedPatchSha256=$(shasum -a 256 "$PatchPath" | awk '{print $1}')
[[ "$ObservedPatchSha256" == "$PatchSha256" ]] ||
    fail "Darwin patch hash mismatch: $ObservedPatchSha256"
if [[ "$TargetProfile" == runtime ]]; then
    declare -a RuntimePatchRecords=(
        "$RuntimePatch1Path|$RuntimePatch1Sha256"
        "$RuntimePatch2Path|$RuntimePatch2Sha256"
        "$RuntimePatch3Path|$RuntimePatch3Sha256"
    )
    for Record in "${RuntimePatchRecords[@]}"; do
        RuntimePatchPath=${Record%%|*}
        ExpectedRuntimePatchSha256=${Record#*|}
        [[ -f "$RuntimePatchPath" ]] ||
            fail "tracked runtime patch is missing: $RuntimePatchPath"
        ObservedRuntimePatchSha256=$(shasum -a 256 "$RuntimePatchPath" | awk '{print $1}')
        [[ "$ObservedRuntimePatchSha256" == "$ExpectedRuntimePatchSha256" ]] ||
            fail "runtime patch hash mismatch for $RuntimePatchPath: $ObservedRuntimePatchSha256"
    done
    ObservedRuntimePatchChainSha256=$(
        awk '{ printf "%s", $0 ORS }' \
            "$RuntimePatch1Path" "$RuntimePatch2Path" "$RuntimePatch3Path" |
            shasum -a 256 | awk '{print $1}')
    [[ "$ObservedRuntimePatchChainSha256" == "$RuntimePatchChainSha256" ]] ||
        fail "runtime patch-chain hash mismatch: $ObservedRuntimePatchChainSha256"
fi

# Resolve dot segments and symlinked parents before any destination mutation.
command -v python3 >/dev/null 2>&1 || fail 'python3 is required to resolve the source path safely'
SourceDirectory=$(python3 -c \
    'import os, sys; print(os.path.realpath(sys.argv[1]))' \
    "$SourceDirectory")
case "$SourceDirectory" in
    "$RepositoryRoot"|"$RepositoryRoot"/*)
        fail 'the FEX checkout must be outside the Lumen repository'
        ;;
esac

CreatedDedicatedOverlay=0
if [[ ! -e "$SourceDirectory" ]]; then
    mkdir -p -- "$(dirname -- "$SourceDirectory")"
    git clone --no-checkout "$UpstreamOrigin" "$SourceDirectory"
    git -C "$SourceDirectory" checkout --detach "$UpstreamCommit"
    CreatedDedicatedOverlay=1
elif [[ ! -d "$SourceDirectory/.git" ]]; then
    fail "existing destination is not a Git checkout: $SourceDirectory"
fi

ObservedOrigin=$(git -C "$SourceDirectory" remote get-url origin)
[[ "$ObservedOrigin" == "$UpstreamOrigin" ]] ||
    fail "unexpected origin: $ObservedOrigin"

ObservedHead=$(git -C "$SourceDirectory" rev-parse --verify HEAD)
DirtyState=$(git -C "$SourceDirectory" status --porcelain=v1 \
    --untracked-files=all --ignore-submodules=none)
[[ -z "$DirtyState" ]] ||
    fail 'refusing to modify a dirty FEX checkout'
if [[ "$ObservedHead" == "$UpstreamCommit" ]]; then
    [[ $CreatedDedicatedOverlay -eq 1 ]] ||
        fail 'refusing to mutate an existing byte-clean upstream checkout; choose an absent dedicated overlay destination'
    git -C "$SourceDirectory" apply --check "$PatchPath"
    git -C "$SourceDirectory" apply "$PatchPath"
    git -C "$SourceDirectory" add \
        CodeEmitter/CodeEmitter/Buffer.h \
        FEXCore FEXHeaderUtils
    GIT_AUTHOR_NAME='Lumen FEXCore Lab' \
    GIT_AUTHOR_EMAIL='lumen-fexcore-lab@invalid' \
    GIT_AUTHOR_DATE='2026-09-01T00:00:00Z' \
    GIT_COMMITTER_NAME='Lumen FEXCore Lab' \
    GIT_COMMITTER_EMAIL='lumen-fexcore-lab@invalid' \
    GIT_COMMITTER_DATE='2026-09-01T00:00:00Z' \
        git -c commit.gpgSign=false -C "$SourceDirectory" commit \
        --no-verify -m 'FEXCore: add Darwin arm64 bootstrap lab port'
elif [[ "$TargetProfile" == bootstrap && "$ObservedHead" != "$PortCommit" ]]; then
    fail "unexpected bootstrap source HEAD: $ObservedHead"
elif [[ "$TargetProfile" == runtime ]]; then
    case "$ObservedHead" in
        "$PortCommit"|"$RuntimeCommit1"|"$RuntimeCommit2"|"$RuntimeCommit3")
            ;;
        *)
            fail "unexpected runtime source HEAD: $ObservedHead"
            ;;
    esac
fi

verify_exact_patch_commit "$UpstreamCommit" "$PortCommit" "$PortTree" "$PatchPath"

if [[ "$TargetProfile" == runtime ]]; then
    ObservedHead=$(git -C "$SourceDirectory" rev-parse --verify HEAD)
    case "$ObservedHead" in
        "$PortCommit")
            RuntimeStage=0
            ;;
        "$RuntimeCommit1")
            RuntimeStage=1
            ;;
        "$RuntimeCommit2")
            RuntimeStage=2
            ;;
        "$RuntimeCommit3")
            RuntimeStage=3
            ;;
        *)
            fail "unexpected runtime preparation stage: $ObservedHead"
            ;;
    esac

    if [[ $RuntimeStage -lt 1 ]]; then
        apply_runtime_patch_commit \
            "$RuntimePatch1Path" "$PortCommit" "$RuntimeCommit1" "$RuntimeTree1" \
            'Lumen FEXCore Runtime' 'lumen-fexcore-runtime@invalid' \
            '2026-09-01T01:00:00Z' \
            'FEXCore: publish Darwin lazy JIT blocks safely' \
            FEXCore/Source/Interface/Core/Core.cpp \
            FEXCore/include/FEXCore/Utils/AllocatorHooks.h
    fi
    if [[ $RuntimeStage -lt 2 ]]; then
        apply_runtime_patch_commit \
            "$RuntimePatch2Path" "$RuntimeCommit1" "$RuntimeCommit2" "$RuntimeTree2" \
            'gs-soft-it' 'info@gs-soft.it' '2026-09-01T16:52:11+02:00' \
            'FEXCore: guard Darwin runtime JIT mutations' \
            FEXCore/Source/Interface/Core/Core.cpp \
            FEXCore/Source/Interface/Core/JIT/JIT.cpp
    fi
    if [[ $RuntimeStage -lt 3 ]]; then
        apply_runtime_patch_commit \
            "$RuntimePatch3Path" "$RuntimeCommit2" "$RuntimeCommit3" "$RuntimeTree3" \
            'gs-soft-it' 'info@gs-soft.it' '2026-09-01T17:04:08+02:00' \
            'FEXCore: clear Darwin lookup caches deterministically' \
            FEXCore/include/FEXCore/Utils/AllocatorHooks.h
    fi

    verify_exact_patch_commit \
        "$PortCommit" "$RuntimeCommit1" "$RuntimeTree1" "$RuntimePatch1Path"
    verify_exact_patch_commit \
        "$RuntimeCommit1" "$RuntimeCommit2" "$RuntimeTree2" "$RuntimePatch2Path"
    verify_exact_patch_commit \
        "$RuntimeCommit2" "$RuntimeCommit3" "$RuntimeTree3" "$RuntimePatch3Path"
fi

ObservedHead=$(git -C "$SourceDirectory" rev-parse --verify HEAD)
if [[ "$TargetProfile" == runtime ]]; then
    [[ "$ObservedHead" == "$RuntimeCommit3" ]] ||
        fail "non-deterministic runtime commit: expected $RuntimeCommit3, got $ObservedHead"
else
    [[ "$ObservedHead" == "$PortCommit" ]] ||
        fail "non-deterministic bootstrap commit: expected $PortCommit, got $ObservedHead"
fi

# Only initialize the four submodules in the linked closure.  Test binaries,
# VIXL, Zydis, Tracy, range-v3 and jemalloc-glibc remain outside this lab.
git -C "$SourceDirectory" submodule update --init -- \
    External/fmt \
    External/xxhash \
    External/unordered_dense \
    External/rpmalloc

declare -a RequiredSubmodules=(
    'External/fmt|1be298e1bd68957e4cd352e1f676f00e07dcfb57'
    'External/xxhash|e626a72bc2321cd320e953a0ccf1584cad60f363'
    'External/unordered_dense|3234af2c03549bc85656bfd3a86993bf1cd8aef1'
    'External/rpmalloc|1d85c246cd827ead6865f4f880d4fef53f2b1864'
)
for Record in "${RequiredSubmodules[@]}"; do
    SubmodulePath=${Record%%|*}
    ExpectedCommit=${Record#*|}
    [[ -e "$SourceDirectory/$SubmodulePath/.git" ]] ||
        fail "submodule was not initialized: $SubmodulePath"
    ActualCommit=$(git -C "$SourceDirectory/$SubmodulePath" rev-parse HEAD)
    [[ "$ActualCommit" == "$ExpectedCommit" ]] ||
        fail "submodule mismatch for $SubmodulePath: $ActualCommit"
done

DirtyState=$(git -C "$SourceDirectory" status --porcelain=v1 \
    --untracked-files=all --ignore-submodules=none)
[[ -z "$DirtyState" ]] || fail "prepared checkout is dirty: $DirtyState"

declare -a RequiredInTreeComponents=(
    'External/SoftFloat-3e|9b2659373bcc8bbfa28eda054b22214a0482ebd2'
    'External/cephes|f60034c28a4d816fb972ccb78d14ae560a973fa2'
)
for Record in "${RequiredInTreeComponents[@]}"; do
    ComponentPath=${Record%%|*}
    ExpectedTree=${Record#*|}
    ActualTree=$(git -C "$SourceDirectory" rev-parse "HEAD:$ComponentPath")
    [[ "$ActualTree" == "$ExpectedTree" ]] ||
        fail "in-tree component mismatch for $ComponentPath: $ActualTree"
done

declare -a RequiredLicenses=(
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
    [[ -f "$SourceDirectory/$LicensePath" ]] ||
        fail "required license/notice is missing: $LicensePath"
    ActualLicenseSha256=$(shasum -a 256 "$SourceDirectory/$LicensePath" | awk '{print $1}')
    [[ "$ActualLicenseSha256" == "$ExpectedLicenseSha256" ]] ||
        fail "license/notice hash mismatch for $LicensePath: $ActualLicenseSha256"
done

if [[ "$TargetProfile" == runtime ]]; then
    printf '%s\n' \
        "LUMEN_MACOS_FEXCORE_SOURCE_V2 ready=1 clean=1 profile=runtime upstream=$UpstreamCommit bootstrapPort=$PortCommit bootstrapTree=$PortTree bootstrapPatchSha256=$PatchSha256 port=$RuntimeCommit3 tree=$RuntimeTree3 runtimePatchChainSha256=$RuntimePatchChainSha256 runtimePatch1Sha256=$RuntimePatch1Sha256 runtimePatch2Sha256=$RuntimePatch2Sha256 runtimePatch3Sha256=$RuntimePatch3Sha256 source=$SourceDirectory"
else
    printf '%s\n' \
        "LUMEN_MACOS_FEXCORE_SOURCE_V1 ready=1 clean=1 upstream=$UpstreamCommit port=$PortCommit tree=$PortTree patchSha256=$PatchSha256 source=$SourceDirectory"
fi
