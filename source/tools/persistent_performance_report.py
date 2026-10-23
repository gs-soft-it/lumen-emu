#!/usr/bin/env python3
"""Validate one persistent baseline and one candidate manifest offline.

The tool never launches the emulator, changes a baseline pointer, or promotes a
candidate.  It accepts two immutable JSON manifests, publishes the authoritative
report exactly once with ``--output``, mirrors it to stdout, and writes a compact
comparison table to stderr.

The authoritative input contract is ``lumen.persistent-performance-run``
version 1.  ``--print-schema`` emits the corresponding JSON Schema document;
validation itself is implemented with the Python standard library so the gate
does not depend on an optional schema package.
"""

from __future__ import annotations

import argparse
import decimal
import errno
import hashlib
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
import threading
from dataclasses import dataclass
from typing import Any, Callable, Dict, Mapping, Optional, Sequence, Tuple


MANIFEST_SCHEMA_NAME = "lumen.persistent-performance-run"
MANIFEST_SCHEMA_VERSION = 1
REPORT_SCHEMA_NAME = "lumen.persistent-performance-report"
REPORT_SCHEMA_VERSION = 1
REPORT_TOOL_SCHEMA = f"{REPORT_SCHEMA_NAME}/{REPORT_SCHEMA_VERSION}"

CHECKPOINT_PRESENT = 2300
WINDOW_SIZE = 120
UINT64_MAX = (1 << 64) - 1
MANIFEST_MAX_BYTES = 1 << 20
ZERO = decimal.Decimal(0)
ONE_HUNDRED = decimal.Decimal(100)
ONE_THOUSAND = decimal.Decimal(1000)
ONE_BILLION = decimal.Decimal(1_000_000_000)

HEX_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
SOURCE_REVISION_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")

TOP_LEVEL_KEYS = frozenset(
    {
        "schema",
        "protocol",
        "comparability",
        "provenance",
        "lineage",
        "measurements",
        "lifecycle",
    }
)
PROTOCOL_KEYS = frozenset(
    {
        "protocol_id",
        "protocol_revision",
        "checkpoint_present",
        "window_size",
        "loading_metric",
        "announcement_clock",
        "fort_window_semantics",
        "in_game_window_semantics",
    }
)
PROTOCOL_CONSTANTS = {
    "checkpoint_present": CHECKPOINT_PRESENT,
    "window_size": WINDOW_SIZE,
    "loading_metric": "launch_to_observation_p2300_clock_uptime_raw_ns",
    "announcement_clock": "CLOCK_UPTIME_RAW",
    "fort_window_semantics": "m0=L+120;discard=m0;measure=m0+120",
    "in_game_window_semantics": "w0=L+120;discard=w0;measure=w0+120",
}

COMPARABILITY_KEYS = frozenset(
    {"host", "os", "power", "workload", "runtime", "scenes", "tool_schemas"}
)
COMPARABILITY_SHAPES = {
    "host": frozenset({"host_id", "model", "architecture"}),
    "os": frozenset({"name", "version", "build", "policy"}),
    "power": frozenset({"source", "mode", "policy"}),
    "workload": frozenset(
        {
            "rom_sha256",
            "lua_sha256",
            "save_template_sha256",
            "cache_policy",
            "cache_seed_sha256",
        }
    ),
    "runtime": frozenset(
        {"sterile_environment_sha256", "backend", "configuration_sha256"}
    ),
    "scenes": frozenset({"fort_mode_oracle", "in_game_oracle"}),
    "tool_schemas": frozenset({"harness", "announcement_sidecar", "report"}),
}

PROVENANCE_KEYS = frozenset(
    {
        "record_id",
        "source_revision",
        "binary_sha256",
        "diff_sha256",
        "harness_sha256",
        "announcement_sidecar_sha256",
        "report_tool_sha256",
        "run_receipt",
        "selector_receipt_sha256",
        "selectors",
        "artifacts_sha256",
    }
)
REQUIRED_ARTIFACT_HASHES = frozenset(
    {
        "stdout_log",
        "stderr_log",
        "exit_status",
        "fort_anchor_receipt",
        "in_game_anchor_receipt",
        "save_special_before",
        "save_special_after",
        "save_template_before",
        "save_template_after",
        "start_config",
        "checkpoint_observation",
        "process_time",
        "exit_uptime_raw",
    }
)

CONFIGURATION_PROJECTION_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION"
)
CONFIGURATION_PROJECTION_STABLE_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
)
CONFIGURATION_PROJECTION_RECORD_ID = (
    "6cp-c-guest-memory-operation-snapshot-elision-g000002"
)
CONFIGURATION_PROJECTION_SCHEMA_NAME = (
    "lumen.cmake-cache-production-projection"
)
CONFIGURATION_PROJECTION_SCHEMA_VERSION = 1
CONFIGURATION_PROJECTION_ATTESTATION = (
    "CONFIRM_PHASE6CP_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
)
CONFIGURATION_PROJECTION_POLICY = "phase6cp-cmake-cache-production-v1"
CONFIGURATION_PROJECTION_MODE = (
    "strip-exact-inactive-phase6cm-phase6co-v1"
)
CONFIGURATION_PROJECTION_RECEIPT_NAME = "configuration-projection.json"
CONFIGURATION_PROJECTION_RAW_NAME = "configuration-cache.raw"
CONFIGURATION_PROJECTION_ARTIFACTS = {
    "configuration_projection_receipt": CONFIGURATION_PROJECTION_RECEIPT_NAME,
    "configuration_cache_raw": CONFIGURATION_PROJECTION_RAW_NAME,
}
CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
    {
        "schema",
        "attestation",
        "policy",
        "mode",
        "record_id",
        "candidate_selector",
        "raw_bytes",
        "raw_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_bytes",
        "removed_segment_sha256",
        "projected_bytes",
        "projected_sha256",
        "epoch_configuration_sha256",
    }
)
CONFIGURATION_PROJECTION_PHASE6CM_BLOCK = (
    b"//Compile scene-anchored sampled AArch64 ordinary-body diagnostic\n"
    b"// telemetry\n"
    b"LUMEN_A64_DYNAREC_PHASE6CM_PROFILE:BOOL=OFF\n\n"
)
CONFIGURATION_PROJECTION_PHASE6CO_BLOCK = (
    b"//Compile scene-anchored AArch64 operation-snapshot opportunity\n"
    b"// telemetry\n"
    b"LUMEN_A64_DYNAREC_PHASE6CO_PROFILE:BOOL=OFF\n\n"
)
CONFIGURATION_PROJECTION_REMOVED_SEGMENT = (
    CONFIGURATION_PROJECTION_PHASE6CM_BLOCK
    + CONFIGURATION_PROJECTION_PHASE6CO_BLOCK
)
CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": CONFIGURATION_PROJECTION_RECORD_ID,
    "generation": 2,
    "parent_record_id": "6cl-c-stable-negative-g000001",
    "original_role": "candidate",
    "candidate_selector": CONFIGURATION_PROJECTION_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
    },
    "raw_bytes": 49_567,
    "raw_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
    "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
    "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
    "removed_bytes": 246,
    "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
    "projected_bytes": 49_321,
    "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
}

PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION"
)
PHASE6CQ_CONFIGURATION_PROJECTION_RECORD_ID = (
    "6cq-c-scalar-memory-fallthrough-admission-g000003"
)
PHASE6CQ_CONFIGURATION_RESTORE_CONTRACT_PREFIX = (
    b"lumen.phase6cq.configuration-restore-operations.v1\0"
)
PHASE6CQ_CONFIGURATION_RESTORE_CXX_SOURCE = (
    b"//CXX compiler\n"
    b"CMAKE_CXX_COMPILER:STRING=/usr/bin/clang++\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_CXX_VALUE = (
    b"//Full path to the compiler used for language <LANG>. Set by CMake\n"
    b"// during compiler detection on the first configure; users may\n"
    b"// override on the command line to select a specific toolchain.\n"
    b"CMAKE_CXX_COMPILER:UNINITIALIZED=clang++\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_C_SOURCE = (
    b"//C compiler\n"
    b"CMAKE_C_COMPILER:STRING=/usr/bin/clang\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_C_VALUE = (
    b"//Full path to the compiler used for language <LANG>. Set by CMake\n"
    b"// during compiler detection on the first configure; users may\n"
    b"// override on the command line to select a specific toolchain.\n"
    b"CMAKE_C_COMPILER:UNINITIALIZED=clang\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_PREFIX_ANCHOR = (
    b"//Value Computed by CMake\n"
    b"CMAKE_PROJECT_COMPAT_VERSION:STATIC=\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_PREFIX_VALUE = (
    b"//Semicolon-separated list of installation prefixes searched by\n"
    b"// find_package(), find_program(), find_library(), find_file(),\n"
    b"// and find_path(). Empty by default; intended to be set by the\n"
    b"// project or the user.\n"
    b"CMAKE_PREFIX_PATH:UNINITIALIZED=/opt/homebrew/opt/qt@5\n\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_PKG_ANCHOR = (
    b"//Compile bounded AArch64 dynarec cross-owner compile diagnostic\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_PKG_VALUE = (
    b"//Build the clean-room PS4 PKG core and CLI tests\n"
    b"LUMENPKG_BUILD_TESTS:BOOL=ON\n\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_MOLTENVK_VALUE = (
    b"\n//Path to a library.\n"
    b"LUMEN_MOLTENVK_APPKIT_FRAMEWORK:FILEPATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/AppKit.framework\n\n"
    b"//Path to a library.\n"
    b"LUMEN_MOLTENVK_COREGRAPHICS_FRAMEWORK:FILEPATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/CoreGraphics.framework\n\n"
    b"//Path to a library.\n"
    b"LUMEN_MOLTENVK_FOUNDATION_FRAMEWORK:FILEPATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/Foundation.framework\n\n"
    b"//Path to a library.\n"
    b"LUMEN_MOLTENVK_IOKIT_FRAMEWORK:FILEPATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/IOKit.framework\n\n"
    b"//Path to a library.\n"
    b"LUMEN_MOLTENVK_IOSURFACE_FRAMEWORK:FILEPATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/IOSurface.framework\n\n"
    b"//Path to a library.\n"
    b"LUMEN_MOLTENVK_METAL_FRAMEWORK:FILEPATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/Metal.framework\n\n"
    b"//Path to a library.\n"
    b"LUMEN_MOLTENVK_QUARTZCORE_FRAMEWORK:FILEPATH=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/QuartzCore.framework\n\n"
    b"//Path to a library.\n"
    b"LUMEN_MOLTENVK_STATIC_LIBRARY:FILEPATH=/opt/homebrew/lib/libMoltenVK.a\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_VULKAN_ANCHOR = (
    b"//Dependencies for the target\n"
    b"spirv-cross-glsl_LIB_DEPENDS:STATIC=general;spirv-cross-core;\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_VULKAN_VALUE = (
    b"//Vulkan/MoltenVK library\n"
    b"Vulkan_Lib:FILEPATH=/opt/homebrew/lib/libMoltenVK.a\n\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_MAKEFILES_SOURCE = (
    b"CMAKE_NUMBER_OF_MAKEFILES:INTERNAL=24\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_MAKEFILES_VALUE = (
    b"CMAKE_NUMBER_OF_MAKEFILES:INTERNAL=25\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_ADVANCED_ANCHOR = (
    b"//ADVANCED property for variable: PKG_CONFIG_ARGN\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_ADVANCED_VALUE = (
    b"//ADVANCED property for variable: LUMEN_MOLTENVK_APPKIT_FRAMEWORK\n"
    b"LUMEN_MOLTENVK_APPKIT_FRAMEWORK-ADVANCED:INTERNAL=1\n"
    b"//ADVANCED property for variable: LUMEN_MOLTENVK_COREGRAPHICS_FRAMEWORK\n"
    b"LUMEN_MOLTENVK_COREGRAPHICS_FRAMEWORK-ADVANCED:INTERNAL=1\n"
    b"//ADVANCED property for variable: LUMEN_MOLTENVK_FOUNDATION_FRAMEWORK\n"
    b"LUMEN_MOLTENVK_FOUNDATION_FRAMEWORK-ADVANCED:INTERNAL=1\n"
    b"//ADVANCED property for variable: LUMEN_MOLTENVK_IOKIT_FRAMEWORK\n"
    b"LUMEN_MOLTENVK_IOKIT_FRAMEWORK-ADVANCED:INTERNAL=1\n"
    b"//ADVANCED property for variable: LUMEN_MOLTENVK_IOSURFACE_FRAMEWORK\n"
    b"LUMEN_MOLTENVK_IOSURFACE_FRAMEWORK-ADVANCED:INTERNAL=1\n"
    b"//ADVANCED property for variable: LUMEN_MOLTENVK_METAL_FRAMEWORK\n"
    b"LUMEN_MOLTENVK_METAL_FRAMEWORK-ADVANCED:INTERNAL=1\n"
    b"//ADVANCED property for variable: LUMEN_MOLTENVK_QUARTZCORE_FRAMEWORK\n"
    b"LUMEN_MOLTENVK_QUARTZCORE_FRAMEWORK-ADVANCED:INTERNAL=1\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_PREFIX_CURRENT = (
    b"CMAKE_OSX_SYSROOT:STRING=\n\n"
    + PHASE6CQ_CONFIGURATION_RESTORE_PREFIX_ANCHOR
)
PHASE6CQ_CONFIGURATION_RESTORE_PKG_CURRENT = (
    b"\n" + PHASE6CQ_CONFIGURATION_RESTORE_PKG_ANCHOR
)
PHASE6CQ_CONFIGURATION_RESTORE_MOLTENVK_CURRENT = (
    b"LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT:BOOL=ON\n\n"
)
PHASE6CQ_CONFIGURATION_RESTORE_VULKAN_CURRENT = (
    b"SPIRV_CROSS_WERROR:BOOL=OFF\n\n"
    + PHASE6CQ_CONFIGURATION_RESTORE_VULKAN_ANCHOR
)
PHASE6CQ_CONFIGURATION_RESTORE_ADVANCED_CURRENT = (
    b"LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK_PATH-ADVANCED:INTERNAL=1\n"
    + PHASE6CQ_CONFIGURATION_RESTORE_ADVANCED_ANCHOR
)
PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS = (
    (
        "cxx_compiler_cache_serialization",
        PHASE6CQ_CONFIGURATION_RESTORE_CXX_SOURCE,
        PHASE6CQ_CONFIGURATION_RESTORE_CXX_VALUE,
    ),
    (
        "c_compiler_cache_serialization",
        PHASE6CQ_CONFIGURATION_RESTORE_C_SOURCE,
        PHASE6CQ_CONFIGURATION_RESTORE_C_VALUE,
    ),
    (
        "cmake_prefix_path_stale_serialization",
        PHASE6CQ_CONFIGURATION_RESTORE_PREFIX_CURRENT,
        PHASE6CQ_CONFIGURATION_RESTORE_PREFIX_CURRENT[:27]
        + PHASE6CQ_CONFIGURATION_RESTORE_PREFIX_VALUE
        + PHASE6CQ_CONFIGURATION_RESTORE_PREFIX_CURRENT[27:],
    ),
    (
        "retired_pkg_test_option",
        PHASE6CQ_CONFIGURATION_RESTORE_PKG_CURRENT,
        PHASE6CQ_CONFIGURATION_RESTORE_PKG_CURRENT[:1]
        + PHASE6CQ_CONFIGURATION_RESTORE_PKG_VALUE
        + PHASE6CQ_CONFIGURATION_RESTORE_PKG_CURRENT[1:],
    ),
    (
        "inactive_moltenvk_values",
        PHASE6CQ_CONFIGURATION_RESTORE_MOLTENVK_CURRENT,
        PHASE6CQ_CONFIGURATION_RESTORE_MOLTENVK_CURRENT[:45]
        + PHASE6CQ_CONFIGURATION_RESTORE_MOLTENVK_VALUE
        + PHASE6CQ_CONFIGURATION_RESTORE_MOLTENVK_CURRENT[45:],
    ),
    (
        "inactive_vulkan_library_value",
        PHASE6CQ_CONFIGURATION_RESTORE_VULKAN_CURRENT,
        PHASE6CQ_CONFIGURATION_RESTORE_VULKAN_CURRENT[:29]
        + PHASE6CQ_CONFIGURATION_RESTORE_VULKAN_VALUE
        + PHASE6CQ_CONFIGURATION_RESTORE_VULKAN_CURRENT[29:],
    ),
    (
        "retired_pkg_makefile_count",
        PHASE6CQ_CONFIGURATION_RESTORE_MAKEFILES_SOURCE,
        PHASE6CQ_CONFIGURATION_RESTORE_MAKEFILES_VALUE,
    ),
    (
        "inactive_moltenvk_advanced_metadata",
        PHASE6CQ_CONFIGURATION_RESTORE_ADVANCED_CURRENT,
        PHASE6CQ_CONFIGURATION_RESTORE_ADVANCED_CURRENT[:63]
        + PHASE6CQ_CONFIGURATION_RESTORE_ADVANCED_VALUE
        + PHASE6CQ_CONFIGURATION_RESTORE_ADVANCED_CURRENT[63:],
    ),
)
PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES = 4_788
PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256 = (
    "ee36e477af8d1218a7b254d078edf9841a4a233597790bba8bcffa0177c3c04c"
)
# The owner-preregistered named operations are fully embedded; no historical
# configuration artifact is read at runtime.
PHASE6CQ_CONFIGURATION_PROJECTION_KIND = (
    "phase6cq-restore-eight-then-strip-v1"
)
PHASE6CQ_CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
    {
        "schema",
        "attestation",
        "policy",
        "mode",
        "record_id",
        "candidate_selector",
        "raw_bytes",
        "raw_sha256",
        "restore_operation_count",
        "restore_operations_contract_bytes",
        "restore_operations_contract_sha256",
        "restored_bytes",
        "restored_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_bytes",
        "removed_segment_sha256",
        "projected_bytes",
        "projected_sha256",
        "epoch_configuration_sha256",
    }
)
PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": PHASE6CQ_CONFIGURATION_PROJECTION_RECORD_ID,
    "generation": 3,
    "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR: True,
    },
    "raw_bytes": 46_394,
    "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
    "projection_kind": PHASE6CQ_CONFIGURATION_PROJECTION_KIND,
    "attestation": "CONFIRM_PHASE6CQ_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
    "policy": "phase6cq-cmake-cache-production-v1",
    "mode": (
        "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-"
        "phase6cm-phase6co-v1"
    ),
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": (
        PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
    ),
    "restore_operations_contract_sha256": (
        PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256
    ),
    "restored_bytes": 49_567,
    "restored_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
    "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
    "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
    "removed_bytes": 246,
    "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
    "projected_bytes": 49_321,
    "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
    "projection_configured": True,
}
PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR = (
    "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION"
)
PHASE6CS_CONFIGURATION_PROJECTION_RECORD_ID = (
    "6cs-c-dead-rflags-elision-g000003"
)
PHASE6CS_CONFIGURATION_RESTORE_CONTRACT_PREFIX = (
    b"lumen.phase6cs.configuration-restore-operations.v1\0"
)
PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS = (
    PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS
)
PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES = 4_788
PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256 = (
    "15a48324c22c27e8959a0d8706965110199d7ca1d8203f10d94fd823fe6e57f9"
)
PHASE6CS_CONFIGURATION_PROJECTION_KIND = (
    "phase6cs-restore-eight-then-strip-v1"
)
PHASE6CS_CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
    PHASE6CQ_CONFIGURATION_PROJECTION_RECEIPT_KEYS
)
PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": PHASE6CS_CONFIGURATION_PROJECTION_RECORD_ID,
    "generation": 3,
    "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR: True,
    },
    "raw_bytes": 46_394,
    "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
    "projection_kind": PHASE6CS_CONFIGURATION_PROJECTION_KIND,
    "attestation": "CONFIRM_PHASE6CS_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
    "policy": "phase6cs-cmake-cache-production-v1",
    "mode": (
        "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-"
        "phase6cm-phase6co-v1"
    ),
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": (
        PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
    ),
    "restore_operations_contract_sha256": (
        PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256
    ),
    "restored_bytes": 49_567,
    "restored_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
    "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
    "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
    "removed_bytes": 246,
    "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
    "projected_bytes": 49_321,
    "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
    "projection_configured": True,
}
PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY"
)
PHASE6DA_CONFIGURATION_PROJECTION_RECORD_ID = (
    "6da-c-fast-block-dense-directory-g000003"
)
PHASE6DA_CONFIGURATION_RESTORE_CONTRACT_PREFIX = (
    b"lumen.phase6da.configuration-restore-operations.v1\0"
)
PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS = (
    PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS
)
PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES = 4_788
PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256 = (
    "ca3126b513628e42a655bff5649ab3f7d4c915d89c2f881e68b07b023cea1c14"
)
PHASE6DA_CONFIGURATION_PROJECTION_KIND = (
    "phase6da-restore-eight-then-strip-v1"
)
PHASE6DA_CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
    PHASE6CQ_CONFIGURATION_PROJECTION_RECEIPT_KEYS
)
PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": PHASE6DA_CONFIGURATION_PROJECTION_RECORD_ID,
    "generation": 3,
    "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
    },
    "raw_bytes": 46_394,
    "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
    "projection_kind": PHASE6DA_CONFIGURATION_PROJECTION_KIND,
    "attestation": "CONFIRM_PHASE6DA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
    "policy": "phase6da-cmake-cache-production-v1",
    "mode": (
        "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-"
        "phase6cm-phase6co-v1"
    ),
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": (
        PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
    ),
    "restore_operations_contract_sha256": (
        PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256
    ),
    "restored_bytes": 49_567,
    "restored_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
    "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
    "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
    "removed_bytes": 246,
    "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
    "projected_bytes": 49_321,
    "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
    "projection_configured": True,
}
PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID = (
    "ifsm-c-fast-block-inline-fixed-signature-match-g000002"
)
PHASEIFSM_DIRECT_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH"
)
PHASEALC_DIRECT_CANDIDATE_RECORD_ID = (
    "alc-c-arm-loading-composite-g000003"
)
PHASEALC_DIRECT_SELECTOR = (
    "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT"
)
PHASEALC_DIRECT_CANDIDATE_MARKER_LINE = (
    "[A64_ARM_LOADING_COMPOSITE_FINAL] purge=positive "
    "adaptive_snapshot=positive omit_cpu_frame_pointer=positive "
    "ifsm_rollback=positive requested=positive status=clean "
    "coverage=positive"
)
PHASEALC_CONFIGURATION_STRIP_CONTRACT_PREFIX = (
    b"lumen.alc.configuration-strip-operations.v1\0"
)
PHASEALC_CONFIGURATION_AHC_BLOCK = (
    b"//Add compact FastBlock snapshots and omit CPU frame pointers on\n"
    b"// top of the purge\n"
    b"LUMEN_A64_ARM_HOTPATH_COMPOSITE:BOOL=ON\n\n"
)
PHASEALC_CONFIGURATION_ALC_BLOCK = (
    b"//Use the retained 6DA matcher inside the ARM hot-path composite\n"
    b"LUMEN_A64_ARM_LOADING_COMPOSITE:BOOL=ON\n\n"
)
PHASEALC_CONFIGURATION_PURGE_BLOCK = (
    b"//Remove rejected UFSB/IMSE/FCFOS paths from the macOS ARM64 Release\n"
    b"// binary\n"
    b"LUMEN_A64_REJECTED_FASTBLOCK_PURGE:BOOL=ON\n\n"
)
PHASEALC_CONFIGURATION_STRIP_OPERATIONS = (
    ("hotpath", PHASEALC_CONFIGURATION_AHC_BLOCK),
    ("loading", PHASEALC_CONFIGURATION_ALC_BLOCK),
    ("purge", PHASEALC_CONFIGURATION_PURGE_BLOCK),
)
PHASEALC_CONFIGURATION_STRIP_OPERATIONS_CONTRACT_BYTES = 421
PHASEALC_CONFIGURATION_STRIP_OPERATIONS_CONTRACT_SHA256 = (
    "704c751375758603328e79b34678e591ad4e52a419d18b35293f252516988e26"
)
PHASEALC_CONFIGURATION_REMOVED_SEGMENT = b"".join(
    block for _, block in PHASEALC_CONFIGURATION_STRIP_OPERATIONS
)
PHASEALC_CONFIGURATION_PROJECTION_KIND = (
    "alc-strip-three-build-options-v1"
)
PHASEALC_CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
    {
        "schema",
        "attestation",
        "policy",
        "mode",
        "record_id",
        "candidate_selector",
        "raw_bytes",
        "raw_sha256",
        "strip_operation_count",
        "strip_operations_contract_bytes",
        "strip_operations_contract_sha256",
        "removed_bytes",
        "removed_segment_sha256",
        "projected_bytes",
        "projected_sha256",
        "epoch_configuration_sha256",
    }
)
PHASEALC_CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
    "generation": 3,
    "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": PHASEALC_DIRECT_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASEIFSM_DIRECT_SELECTOR: True,
        PHASEALC_DIRECT_SELECTOR: True,
    },
    "raw_bytes": 46_749,
    "raw_sha256": "3d42b0edc9be9efa0156a02400de50ae4afb127ca30b7a74df0a36c1e2291e71",
    "projection_kind": PHASEALC_CONFIGURATION_PROJECTION_KIND,
    "attestation": "CONFIRM_ALC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
    "policy": "alc-cmake-cache-production-v1",
    "mode": "strip-exact-active-alc-build-options-v1",
    "strip_operation_count": 3,
    "strip_operations_contract_bytes": (
        PHASEALC_CONFIGURATION_STRIP_OPERATIONS_CONTRACT_BYTES
    ),
    "strip_operations_contract_sha256": (
        PHASEALC_CONFIGURATION_STRIP_OPERATIONS_CONTRACT_SHA256
    ),
    "removed_bytes": 355,
    "removed_segment_sha256": (
        "c0ec59ce9c4b95ef3bf3643b6fccbdf41aba1c1c034c8f8e630ad0ccc2b60e78"
    ),
    "projected_bytes": 46_394,
    "projected_sha256": (
        "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
    ),
    "projection_configured": True,
}
PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID = (
    "msore-msfc-mdssc-c-metal-runtime-bundle-g000004"
)
PHASEMSORE_DIRECT_SELECTOR = "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION"
PHASEMSFC_COMPANION_SELECTOR = "LUMEN_A64_METAL_SHADER_FUNCTION_CACHE"
PHASEMDSSC_COMPANION_SELECTOR = (
    "LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE"
)
PHASEMETAL_RUNTIME_BUNDLE_SELECTORS = frozenset(
    {
        PHASEMSORE_DIRECT_SELECTOR,
        PHASEMSFC_COMPANION_SELECTOR,
        PHASEMDSSC_COMPANION_SELECTOR,
    }
)
PHASEMSORE_DIRECT_CANDIDATE_MARKER_LINE = (
    "[A64_METAL_STATE_ONLY_RECORD_ELISION_FINAL] requested=positive "
    "effective=positive state_only=positive snapshot_consumed=positive "
    "status=clean coverage=positive"
)
PHASEMSFC_COMPANION_MARKER_LINE = (
    "[A64_METAL_SHADER_FUNCTION_CACHE_FINAL] lookup=positive hit=positive "
    "compile=positive requested=positive status=clean coverage=positive"
)
PHASEMDSSC_COMPANION_MARKER_LINE = (
    "[A64_METAL_DEPTH_STENCIL_STATE_CACHE_FINAL] requested=positive "
    "compiled=positive lookup=positive hit=positive miss=positive "
    "create_match=positive status=clean coverage=positive"
)
PHASENODA_DIRECT_CANDIDATE_RECORD_ID = (
    "noda-c-a64-dynarec-native-only-admission-g000005"
)
PHASENODA_DIRECT_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION"
)
PHASEPBA_DIRECT_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION"
)
PHASENODA_DIRECT_CANDIDATE_MARKER_LINE = (
    "[A64_NATIVE_ONLY_ADMISSION_FINAL] effective=positive "
    "ordinary_routed=positive native_probe=bypassed "
    "native_only_executed=positive "
    "class=fastblock_primary_native_only_dynarec status=clean "
    "coverage=positive"
)
PHASENODA_STABLE_SUPERSEDED_MARKER_LINE = (
    "[A64_STABLE_NEGATIVE_ROUTE_FINAL] classified=inapplicable "
    "live_mismatch=none status=clean coverage=superseded "
    "class=native_only_admission"
)
PHASENODA_SNAPSHOT_RETAINED_MARKER_LINE = (
    "[A64_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION_FINAL] "
    "elided=positive signature_snapshot=preserved status=clean "
    "coverage=positive"
)
PHASENODA_DENSE_RETAINED_MARKER_LINE = (
    "[A64_FAST_BLOCK_DENSE_DIRECTORY_FINAL] lookup=positive hit=positive "
    "mismatch=none status=clean coverage=positive"
)
PHASENODA_IFSM_RETAINED_MARKER_LINE = (
    "[A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH_FINAL] "
    "requested=positive effective=positive compared=positive "
    "status=clean coverage=positive"
)
PHASENODA_RETAINED_MARKER_LINES = (
    PHASENODA_STABLE_SUPERSEDED_MARKER_LINE,
    PHASENODA_SNAPSHOT_RETAINED_MARKER_LINE,
    PHASENODA_DENSE_RETAINED_MARKER_LINE,
    PHASENODA_IFSM_RETAINED_MARKER_LINE,
    PHASEALC_DIRECT_CANDIDATE_MARKER_LINE,
    PHASEMSFC_COMPANION_MARKER_LINE,
    PHASEMDSSC_COMPANION_MARKER_LINE,
    PHASEMSORE_DIRECT_CANDIDATE_MARKER_LINE,
)
PHASEMETAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_RECEIPT_KEYS = (
    PHASEALC_CONFIGURATION_PROJECTION_RECEIPT_KEYS
)
# The Metal bundle is runtime-only relative to ALC, so it intentionally uses
# the same three-block CMake-cache projection implementation and identities.
PHASEMETAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT = {
    **PHASEALC_CONFIGURATION_PROJECTION_CONTRACT,
    "record_id": PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
    "generation": 4,
    "parent_record_id": PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
    "candidate_selector": PHASEMSORE_DIRECT_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASEIFSM_DIRECT_SELECTOR: True,
        PHASEALC_DIRECT_SELECTOR: True,
        PHASEMSFC_COMPANION_SELECTOR: True,
        PHASEMDSSC_COMPANION_SELECTOR: True,
        PHASEMSORE_DIRECT_SELECTOR: True,
    },
    "attestation": (
        "CONFIRM_MSORE_MSFC_MDSSC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
    ),
    "policy": "msore-msfc-mdssc-cmake-cache-production-v1",
}
PHASENODA_CONFIGURATION_PROJECTION_CONTRACT = {
    **PHASEALC_CONFIGURATION_PROJECTION_CONTRACT,
    "record_id": PHASENODA_DIRECT_CANDIDATE_RECORD_ID,
    "generation": 5,
    "parent_record_id": PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
    "candidate_selector": PHASENODA_DIRECT_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASEIFSM_DIRECT_SELECTOR: True,
        PHASEALC_DIRECT_SELECTOR: True,
        PHASEMSFC_COMPANION_SELECTOR: True,
        PHASEMDSSC_COMPANION_SELECTOR: True,
        PHASEMSORE_DIRECT_SELECTOR: True,
        PHASENODA_DIRECT_SELECTOR: True,
    },
    "attestation": (
        "CONFIRM_NODA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
    ),
    "policy": "noda-cmake-cache-production-v1",
}
CONFIGURATION_PROJECTION_CONTRACTS = (
    CONFIGURATION_PROJECTION_CONTRACT,
    PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT,
    PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT,
    PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT,
    PHASEALC_CONFIGURATION_PROJECTION_CONTRACT,
    PHASEMETAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT,
    PHASENODA_CONFIGURATION_PROJECTION_CONTRACT,
)

PHASE6DA_DIRECT_PROTOCOL_ID = (
    "macos-m1-worms-p2300-announced-p120-20260816-v1"
)
PHASE6DA_DIRECT_OS_VERSION = "26.6.2"
PHASE6DA_DIRECT_OS_BUILD = "25G83"
PHASE6DA_DIRECT_OS_POLICY = "fixed-version-and-build-within-epoch"
PHASE6DA_DIRECT_CONFIGURATION_BYTES = 46_394
PHASE6DA_DIRECT_CONFIGURATION_SHA256 = (
    "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
)
PHASE6DA_DIRECT_BASELINE_RECORD_ID = "6da-b-production-g000000"
PHASE6DA_DIRECT_CANDIDATE_RECORD_ID = (
    "6da-c-fast-block-dense-directory-g000001"
)
PHASEFCFOS_DIRECT_CANDIDATE_RECORD_ID = (
    "fcfos-c-fast-block-callback-free-ordinary-specialization-g000003"
)
PHASEFCFOS_DIRECT_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION"
)
PHASEFCFOS_DIRECT_CANDIDATE_MARKER_LINE = (
    "[A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION_FINAL] "
    "requested=positive effective=positive reentry_safe=positive "
    "snapshot_elision=positive status=clean coverage=positive"
)
PHASEUFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID = (
    "ufsb-imse-c-fast-block-dispatch-bundle-g000003"
)
PHASEUFSB_DIRECT_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING"
)
PHASEIMSE_DIRECT_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION"
)
PHASEUFSB_IMSE_DIRECT_SELECTORS = frozenset(
    {PHASEUFSB_DIRECT_SELECTOR, PHASEIMSE_DIRECT_SELECTOR}
)
PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE = {
    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
    CONFIGURATION_PROJECTION_SELECTOR: True,
}
PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS = {
    "stable_negative_route": "1",
    "guest_memory_operation_snapshot_elision": "1",
    "scalar_memory_fallthrough_admission": "0",
    "fast_block_dead_rflags_elision": "0",
}
PHASENODA_EXPECTED_SELECTOR_BUNDLE = {
    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
    CONFIGURATION_PROJECTION_SELECTOR: True,
    PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
    PHASEIFSM_DIRECT_SELECTOR: True,
    PHASEALC_DIRECT_SELECTOR: True,
    PHASEMSFC_COMPANION_SELECTOR: True,
    PHASEMDSSC_COMPANION_SELECTOR: True,
    PHASEMSORE_DIRECT_SELECTOR: True,
    PHASENODA_DIRECT_SELECTOR: True,
}
PHASENODA_EFFECTIVE_SELECTOR_BUNDLE = {
    **PHASENODA_EXPECTED_SELECTOR_BUNDLE,
    CONFIGURATION_PROJECTION_STABLE_SELECTOR: False,
}
PHASENODA_REQUIRED_RECEIPT_FIELDS = {
    **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
    "stable_negative_route_requested": "1",
    "stable_negative_route": "0",
    "stable_negative_route_superseded": "1",
    "profitable_block_admission_requested": "0",
    "profitable_block_admission": "0",
    "profitable_block_admission_class": "direct_positive_then_ordinary",
    "native_only_admission_requested": "1",
    "native_only_admission": "1",
    "native_only_admission_class": "fastblock_primary_native_only_dynarec",
    "fast_block_dense_directory_requested": "1",
    "fast_block_dense_directory": "1",
    "fast_block_inline_fixed_signature_match_requested": "1",
    "fast_block_inline_fixed_signature_match": "1",
    "fast_block_unrolled_fixed_signature_batching_requested": "0",
    "fast_block_unrolled_fixed_signature_batching": "0",
    "fast_block_idempotent_mru_store_elision_requested": "0",
    "fast_block_idempotent_mru_store_elision": "0",
    "fast_block_callback_free_ordinary_specialization_requested": "0",
    "fast_block_callback_free_ordinary_specialization": "0",
    "arm_loading_composite_requested": "1",
    "arm_loading_composite": "1",
    "metal_shader_function_cache_requested": "1",
    "metal_shader_function_cache": "1",
    "metal_depth_stencil_state_cache_requested": "1",
    "metal_depth_stencil_state_cache": "1",
    "metal_state_only_record_elision_requested": "1",
    "metal_state_only_record_elision": "1",
}
PHASE6DA_DIRECT_CONTRACTS = (
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASE6DA_DIRECT_BASELINE_RECORD_ID,
        "generation": 0,
        "parent_record_id": None,
        "original_role": "production",
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: False,
        },
        "required_receipt_fields": {
            **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
            "fast_block_dense_directory_requested": "0",
            "fast_block_dense_directory": "0",
        },
        "candidate_marker": {
            "required": False,
            "expected_count": 0,
            "observed_count": 0,
            "status": "not_applicable",
            "coverage_required": False,
            "coverage_positive": False,
        },
    },
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASE6DA_DIRECT_CANDIDATE_RECORD_ID,
        "generation": 1,
        "parent_record_id": PHASE6DA_DIRECT_BASELINE_RECORD_ID,
        "original_role": "candidate",
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
        },
        "required_receipt_fields": {
            **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
            "fast_block_dense_directory_requested": "1",
            "fast_block_dense_directory": "1",
        },
        "candidate_marker": {
            "required": True,
            "expected_count": 1,
            "observed_count": 1,
            "status": "clean",
            "coverage_required": True,
            "coverage_positive": True,
        },
    },
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
        "generation": 2,
        "parent_record_id": PHASE6DA_DIRECT_CANDIDATE_RECORD_ID,
        "original_role": "candidate",
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
            PHASEIFSM_DIRECT_SELECTOR: True,
        },
        "required_receipt_fields": {
            **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
            "fast_block_dense_directory_requested": "1",
            "fast_block_dense_directory": "1",
            "fast_block_inline_fixed_signature_match_requested": "1",
            "fast_block_inline_fixed_signature_match": "1",
        },
        "candidate_marker": {
            "required": True,
            "expected_count": 1,
            "observed_count": 1,
            "status": "clean",
            "coverage_required": True,
            "coverage_positive": True,
        },
    },
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASEUFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID,
        "generation": 3,
        "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
        "original_role": "candidate",
        "primary_selector": PHASEUFSB_DIRECT_SELECTOR,
        "companion_selectors": frozenset({PHASEIMSE_DIRECT_SELECTOR}),
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
            PHASEIFSM_DIRECT_SELECTOR: True,
            PHASEUFSB_DIRECT_SELECTOR: True,
            PHASEIMSE_DIRECT_SELECTOR: True,
        },
        "required_receipt_fields": {
            **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
            "fast_block_dense_directory_requested": "1",
            "fast_block_dense_directory": "1",
            "fast_block_inline_fixed_signature_match_requested": "1",
            "fast_block_inline_fixed_signature_match": "1",
            "fast_block_unrolled_fixed_signature_batching_requested": "1",
            "fast_block_unrolled_fixed_signature_batching": "1",
            "fast_block_idempotent_mru_store_elision_requested": "1",
            "fast_block_idempotent_mru_store_elision": "1",
        },
        "candidate_marker": {
            "required": True,
            "expected_count": 1,
            "observed_count": 1,
            "status": "clean",
            "coverage_required": True,
            "coverage_positive": True,
        },
    },
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASEFCFOS_DIRECT_CANDIDATE_RECORD_ID,
        "generation": 3,
        "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
        "original_role": "candidate",
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
            PHASEIFSM_DIRECT_SELECTOR: True,
            PHASEFCFOS_DIRECT_SELECTOR: True,
        },
        "required_receipt_fields": {
            **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
            "fast_block_dense_directory_requested": "1",
            "fast_block_dense_directory": "1",
            "fast_block_inline_fixed_signature_match_requested": "1",
            "fast_block_inline_fixed_signature_match": "1",
            "fast_block_unrolled_fixed_signature_batching_requested": "0",
            "fast_block_unrolled_fixed_signature_batching": "0",
            "fast_block_idempotent_mru_store_elision_requested": "0",
            "fast_block_idempotent_mru_store_elision": "0",
            "fast_block_callback_free_ordinary_specialization_requested": "1",
            "fast_block_callback_free_ordinary_specialization": "1",
        },
        # The harness accepts this exact sealed-log line before reducing it to
        # the lifecycle result below.  Keep the interpretation frozen here so
        # positive coverage continues to attest both ordinary subpaths.
        "candidate_marker_line": PHASEFCFOS_DIRECT_CANDIDATE_MARKER_LINE,
        "candidate_marker": {
            "required": True,
            "expected_count": 1,
            "observed_count": 1,
            "status": "clean",
            "coverage_required": True,
            "coverage_positive": True,
            "line": PHASEFCFOS_DIRECT_CANDIDATE_MARKER_LINE,
        },
    },
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
        "generation": 3,
        "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
        "original_role": "candidate",
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
            PHASEIFSM_DIRECT_SELECTOR: True,
            PHASEALC_DIRECT_SELECTOR: True,
        },
        "required_receipt_fields": {
            **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
            "fast_block_dense_directory_requested": "1",
            "fast_block_dense_directory": "1",
            "fast_block_inline_fixed_signature_match_requested": "1",
            "fast_block_inline_fixed_signature_match": "1",
            "fast_block_unrolled_fixed_signature_batching_requested": "0",
            "fast_block_unrolled_fixed_signature_batching": "0",
            "fast_block_idempotent_mru_store_elision_requested": "0",
            "fast_block_idempotent_mru_store_elision": "0",
            "fast_block_callback_free_ordinary_specialization_requested": "0",
            "fast_block_callback_free_ordinary_specialization": "0",
            "arm_loading_composite_requested": "1",
            "arm_loading_composite": "1",
        },
        "candidate_marker_line": PHASEALC_DIRECT_CANDIDATE_MARKER_LINE,
        "candidate_marker": {
            "required": True,
            "expected_count": 1,
            "observed_count": 1,
            "status": "clean",
            "coverage_required": True,
            "coverage_positive": True,
            "line": PHASEALC_DIRECT_CANDIDATE_MARKER_LINE,
        },
    },
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
        "generation": 4,
        "parent_record_id": PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
        "original_role": "candidate",
        "primary_selector": PHASEMSORE_DIRECT_SELECTOR,
        "companion_selectors": frozenset(
            {PHASEMSFC_COMPANION_SELECTOR, PHASEMDSSC_COMPANION_SELECTOR}
        ),
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
            PHASEIFSM_DIRECT_SELECTOR: True,
            PHASEALC_DIRECT_SELECTOR: True,
            PHASEMSFC_COMPANION_SELECTOR: True,
            PHASEMDSSC_COMPANION_SELECTOR: True,
            PHASEMSORE_DIRECT_SELECTOR: True,
        },
        "required_receipt_fields": {
            **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
            "fast_block_dense_directory_requested": "1",
            "fast_block_dense_directory": "1",
            "fast_block_inline_fixed_signature_match_requested": "1",
            "fast_block_inline_fixed_signature_match": "1",
            "fast_block_unrolled_fixed_signature_batching_requested": "0",
            "fast_block_unrolled_fixed_signature_batching": "0",
            "fast_block_idempotent_mru_store_elision_requested": "0",
            "fast_block_idempotent_mru_store_elision": "0",
            "fast_block_callback_free_ordinary_specialization_requested": "0",
            "fast_block_callback_free_ordinary_specialization": "0",
            "arm_loading_composite_requested": "1",
            "arm_loading_composite": "1",
            "metal_shader_function_cache_requested": "1",
            "metal_shader_function_cache": "1",
            "metal_depth_stencil_state_cache_requested": "1",
            "metal_depth_stencil_state_cache": "1",
            "metal_state_only_record_elision_requested": "1",
            "metal_state_only_record_elision": "1",
        },
        "candidate_marker_line": PHASEMSORE_DIRECT_CANDIDATE_MARKER_LINE,
        "candidate_marker": {
            "required": True,
            "expected_count": 1,
            "observed_count": 1,
            "status": "clean",
            "coverage_required": True,
            "coverage_positive": True,
            "line": PHASEMSORE_DIRECT_CANDIDATE_MARKER_LINE,
        },
    },
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASENODA_DIRECT_CANDIDATE_RECORD_ID,
        "generation": 5,
        "parent_record_id": PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
        "original_role": "candidate",
        "selector_bundle": PHASENODA_EXPECTED_SELECTOR_BUNDLE,
        "effective_selector_bundle": PHASENODA_EFFECTIVE_SELECTOR_BUNDLE,
        "required_receipt_fields": PHASENODA_REQUIRED_RECEIPT_FIELDS,
        # The historical manifest schema stores only the primary candidate
        # line.  The harness validates these retained lines exact-once against
        # the raw, hash-bound logs before it seals the manifest.
        "retained_marker_lines": PHASENODA_RETAINED_MARKER_LINES,
        "candidate_marker_line": PHASENODA_DIRECT_CANDIDATE_MARKER_LINE,
        "candidate_marker": {
            "required": True,
            "expected_count": 1,
            "observed_count": 1,
            "status": "clean",
            "coverage_required": True,
            "coverage_positive": True,
            "line": PHASENODA_DIRECT_CANDIDATE_MARKER_LINE,
        },
    },
)
PHASE6DA_DIRECT_RECORD_IDS = frozenset(
    contract["record_id"] for contract in PHASE6DA_DIRECT_CONTRACTS
)
A64_CONFIGURATION_PREFIX = "A64 dynarec configuration:"

LINEAGE_KEYS = frozenset({"generation", "parent_record_id", "original_role"})
MEASUREMENTS_KEYS = frozenset({"loading", "fort_mode", "in_game", "supporting"})
LOADING_KEYS = frozenset(
    {
        "launch_uptime_raw_ns",
        "checkpoint_observed_uptime_raw_ns",
        "wall_ns",
        "checkpoint",
    }
)
CHECKPOINT_KEYS = frozenset(
    {
        "present",
        "target",
        "cumulative_ms",
        "cumulative_intervals",
        "partial_ms",
        "partial_intervals",
        "terminal",
        "count",
    }
)
ANCHOR_KEYS = frozenset(
    {
        "receipt_uptime_raw_ns",
        "endpoint_before",
        "endpoint_after",
        "anchor_endpoint",
        "measured_endpoint",
        "avg_interval_ms",
    }
)
SUPPORTING_KEYS = frozenset({"process_time_seconds"})
PROCESS_TIME_KEYS = frozenset({"real", "user", "sys"})

LIFECYCLE_KEYS = frozenset(
    {
        "status",
        "exit_status_bytes",
        "receipt_count",
        "checkpoint_count",
        "sdl_quit_count",
        "residual_process_count",
        "fatal_count",
        "assert_count",
        "dynarec_unhealthy_count",
        "descriptor_mismatch_count",
        "device_lost_count",
        "glitch_observed",
        "save_special_unchanged",
        "save_template_unchanged",
        "sterile_environment_confirmed",
        "fresh_save_confirmed",
        "frame_trace_enabled",
        "present_limit_absent",
        "manual_input_only",
        "no_input_before_p2300",
        "fort_window_input_free",
        "in_game_window_input_free",
        "fort_mode_oracle_confirmed",
        "in_game_oracle",
        "candidate_marker",
    }
)
IN_GAME_ORACLE_KEYS = frozenset(
    {
        "objective_visible",
        "terrain_island_visible",
        "boggy_hp",
        "hud_intact",
        "timer_advanced",
    }
)
CANDIDATE_MARKER_KEYS = frozenset(
    {
        "required",
        "expected_count",
        "observed_count",
        "status",
        "coverage_required",
        "coverage_positive",
    }
)
EXACT_LINE_CANDIDATE_MARKER_KEYS = frozenset(
    {*CANDIDATE_MARKER_KEYS, "line"}
)
# Retain the historical internal name for downstream fixture imports.
FCFOS_CANDIDATE_MARKER_KEYS = EXACT_LINE_CANDIDATE_MARKER_KEYS


class ReportError(Exception):
    """A manifest or comparison is incomplete, ambiguous, or inconsistent."""


class _DuplicateKeyError(ValueError):
    pass


@dataclass(frozen=True)
class RunManifest:
    path: pathlib.Path
    content_sha256: str
    raw: Mapping[str, Any]

    @property
    def protocol(self) -> Mapping[str, Any]:
        return _mapping(self.raw["protocol"], self.path, "protocol")

    @property
    def comparability(self) -> Mapping[str, Any]:
        return _mapping(self.raw["comparability"], self.path, "comparability")

    @property
    def provenance(self) -> Mapping[str, Any]:
        return _mapping(self.raw["provenance"], self.path, "provenance")

    @property
    def lineage(self) -> Mapping[str, Any]:
        return _mapping(self.raw["lineage"], self.path, "lineage")

    @property
    def measurements(self) -> Mapping[str, Any]:
        return _mapping(self.raw["measurements"], self.path, "measurements")

    @property
    def lifecycle(self) -> Mapping[str, Any]:
        return _mapping(self.raw["lifecycle"], self.path, "lifecycle")


def _require(condition: bool, source: object, message: str) -> None:
    if not condition:
        raise ReportError(f"{source}: {message}")


def _object_without_duplicates(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateKeyError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _reject_json_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON number {value}")


def _mapping(value: Any, source: object, path: str) -> Mapping[str, Any]:
    _require(type(value) is dict, source, f"{path} must be a JSON object")
    return value


def _exact_keys(
    value: Any, expected: frozenset[str], source: object, path: str
) -> Mapping[str, Any]:
    mapping = _mapping(value, source, path)
    actual = frozenset(mapping)
    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    _require(not missing, source, f"{path} is missing keys: {', '.join(missing)}")
    _require(not extra, source, f"{path} has unknown keys: {', '.join(extra)}")
    return mapping


def _nonempty_string(value: Any, source: object, path: str) -> str:
    _require(type(value) is str, source, f"{path} must be a string")
    _require(value != "", source, f"{path} must not be empty")
    _require(value == value.strip(), source, f"{path} must not have edge whitespace")
    _require(
        all(ord(character) >= 0x20 for character in value),
        source,
        f"{path} contains a control character",
    )
    return value


def _bool(value: Any, source: object, path: str) -> bool:
    _require(type(value) is bool, source, f"{path} must be a JSON boolean")
    return value


def _uint(value: Any, source: object, path: str) -> int:
    _require(type(value) is int, source, f"{path} must be a JSON integer")
    _require(0 <= value <= UINT64_MAX, source, f"{path} must fit uint64")
    return value


def _positive_decimal(value: Any, source: object, path: str) -> decimal.Decimal:
    _require(
        type(value) in (int, decimal.Decimal) and type(value) is not bool,
        source,
        f"{path} must be a JSON number",
    )
    parsed = decimal.Decimal(value)
    _require(parsed.is_finite(), source, f"{path} must be finite")
    _require(parsed > ZERO, source, f"{path} must be positive")
    return parsed


def _nonnegative_decimal(value: Any, source: object, path: str) -> decimal.Decimal:
    _require(
        type(value) in (int, decimal.Decimal) and type(value) is not bool,
        source,
        f"{path} must be a JSON number",
    )
    parsed = decimal.Decimal(value)
    _require(parsed.is_finite(), source, f"{path} must be finite")
    _require(parsed >= ZERO, source, f"{path} must be non-negative")
    return parsed


def _sha256(value: Any, source: object, path: str) -> str:
    parsed = _nonempty_string(value, source, path)
    _require(
        HEX_SHA256_RE.fullmatch(parsed) is not None,
        source,
        f"{path} must be lowercase SHA-256",
    )
    return parsed


def _source_revision(value: Any, source: object, path: str) -> str:
    parsed = _nonempty_string(value, source, path)
    _require(
        SOURCE_REVISION_RE.fullmatch(parsed) is not None,
        source,
        f"{path} must be a lowercase 40- or 64-hex revision",
    )
    return parsed


def _validate_json_receipt_value(value: Any, source: object, path: str) -> None:
    if value is None or type(value) in (str, bool, int):
        if type(value) is int:
            _uint(value, source, path)
        elif type(value) is str:
            _nonempty_string(value, source, path)
        return
    if type(value) is list:
        for index, item in enumerate(value):
            _validate_json_receipt_value(item, source, f"{path}[{index}]")
        return
    if type(value) is dict:
        for key, item in value.items():
            _nonempty_string(key, source, f"{path} key")
            _validate_json_receipt_value(item, source, f"{path}.{key}")
        return
    raise ReportError(f"{source}: {path} contains an unsupported JSON value")


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _canonical_sha256(value: Any) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _read_sealed_projection_artifact(
    path: pathlib.Path,
    *,
    maximum_bytes: int,
    _after_open_hook: Optional[Callable[[], None]] = None,
) -> bytes:
    try:
        path_before = os.lstat(path)
    except OSError as error:
        raise ReportError(f"{path}: cannot inspect projection artifact: {error}") from error
    _require(
        stat.S_ISREG(path_before.st_mode),
        path,
        "projection artifact pathname is not a regular non-symlink file",
    )
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise ReportError(
            f"{path}: cannot open sealed regular non-symlink projection artifact: {error}"
        ) from error
    try:
        before = os.fstat(descriptor)
        _require(stat.S_ISREG(before.st_mode), path, "projection artifact is not regular")
        _require(
            stat.S_IMODE(before.st_mode) == 0o444,
            path,
            "projection artifact mode must be exactly 0444",
        )
        _require(
            before.st_size <= maximum_bytes,
            path,
            f"projection artifact exceeds {maximum_bytes} bytes",
        )
        if _after_open_hook is not None:
            _after_open_hook()
        chunks = []
        total = 0
        while True:
            chunk = os.read(descriptor, 64 * 1024)
            if not chunk:
                break
            total += len(chunk)
            _require(
                total <= maximum_bytes,
                path,
                f"projection artifact exceeds {maximum_bytes} bytes",
            )
            chunks.append(chunk)
        after = os.fstat(descriptor)
        path_after = os.lstat(path)
    except OSError as error:
        raise ReportError(f"{path}: cannot read projection artifact: {error}") from error
    finally:
        os.close(descriptor)
    def identity(value: os.stat_result) -> Tuple[int, ...]:
        return (
            value.st_dev,
            value.st_ino,
            value.st_mode,
            value.st_size,
            value.st_mtime_ns,
            value.st_ctime_ns,
        )

    payload = b"".join(chunks)
    _require(
        identity(path_before)
        == identity(before)
        == identity(after)
        == identity(path_after)
        and len(payload) == after.st_size,
        path,
        "projection artifact pathname or opened inode changed while being read",
    )
    return payload


def _strict_json_object_payload(
    payload: bytes, source: pathlib.Path, path: str
) -> Mapping[str, Any]:
    try:
        value = json.loads(
            payload.decode("utf-8", errors="strict"),
            parse_float=decimal.Decimal,
            parse_int=int,
            parse_constant=_reject_json_constant,
            object_pairs_hook=_object_without_duplicates,
        )
    except (UnicodeError, json.JSONDecodeError, ValueError) as error:
        raise ReportError(f"{source}: cannot read strict JSON {path}: {error}") from error
    return _mapping(value, source, path)


def _phase6cq_restore_operations_payload() -> bytes:
    payload = bytearray(PHASE6CQ_CONFIGURATION_RESTORE_CONTRACT_PREFIX)
    names = []
    for index, operation in enumerate(
        PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS
    ):
        _require(
            type(operation) in (list, tuple) and len(operation) == 3,
            "compiled phase-6CQ restore operations",
            f"operation {index} must contain name/current/restored",
        )
        name, current, restored = operation
        _require(
            type(name) is str
            and re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            "compiled phase-6CQ restore operations",
            f"operation {index} has a non-canonical name",
        )
        try:
            name_bytes = name.encode("ascii", errors="strict")
        except UnicodeError as error:  # pragma: no cover - regex is ASCII-only.
            raise ReportError(
                f"compiled phase-6CQ restore operation {index} name is not ASCII: {error}"
            ) from error
        _require(
            type(current) is bytes
            and type(restored) is bytes
            and bool(current)
            and bool(restored)
            and current != restored,
            "compiled phase-6CQ restore operations",
            f"operation {name} has invalid byte literals",
        )
        names.append(name)
        payload.extend(len(name_bytes).to_bytes(4, "big"))
        payload.extend(name_bytes)
        payload.extend(len(current).to_bytes(8, "big"))
        payload.extend(current)
        payload.extend(len(restored).to_bytes(8, "big"))
        payload.extend(restored)
    _require(
        len(names) == len(set(names)),
        "compiled phase-6CQ restore operations",
        "operation names must be unique",
    )
    return bytes(payload)


def _phase6cq_restore_configuration(
    raw_payload: bytes,
    source: pathlib.Path,
    contract: Mapping[str, Any],
) -> bytes:
    operation_count = _uint(
        contract["restore_operation_count"],
        source,
        "contract.restore_operation_count",
    )
    operations_contract_bytes = _uint(
        contract["restore_operations_contract_bytes"],
        source,
        "contract.restore_operations_contract_bytes",
    )
    operations_contract_sha256 = _sha256(
        contract["restore_operations_contract_sha256"],
        source,
        "contract.restore_operations_contract_sha256",
    )
    compiled_operations_payload = _phase6cq_restore_operations_payload()
    _require(
        operation_count == 8
        and operation_count == len(PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS)
        and len(compiled_operations_payload) == operations_contract_bytes
        and operations_contract_bytes
        == PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
        and hashlib.sha256(compiled_operations_payload).hexdigest()
        == operations_contract_sha256
        and operations_contract_sha256
        == PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256,
        source,
        "compiled phase-6CQ restore operations differ from the preregistered contract",
    )

    restored_payload = raw_payload
    for name, current, restored in PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS:
        restored_count = restored_payload.count(restored)
        _require(
            restored_count == 0,
            source,
            (
                f"phase-6CQ restore operation {name} replacement is already "
                f"present {restored_count} time(s); pre-restored input is forbidden"
            ),
        )
        source_count = restored_payload.count(current)
        _require(
            source_count == 1,
            source,
            (
                f"phase-6CQ restore operation {name} source literal must be "
                f"exact-once, observed {source_count}"
            ),
        )
        restored_payload = restored_payload.replace(current, restored, 1)

    _require(
        len(restored_payload)
        == _uint(
            contract["restored_bytes"],
            source,
            "contract.restored_bytes",
        )
        and hashlib.sha256(restored_payload).hexdigest()
        == _sha256(
            contract["restored_sha256"],
            source,
            "contract.restored_sha256",
        ),
        source,
        "phase-6CQ restored configuration does not reproduce the sealed 6CP raw identity",
    )
    return restored_payload


def _validate_phase6cq_configuration_projection(
    root: Mapping[str, Any],
    source: pathlib.Path,
    *,
    contract: Mapping[str, Any],
    candidate_selector: str,
    raw_path: pathlib.Path,
    receipt_path: pathlib.Path,
    raw_payload: bytes,
    receipt_payload: bytes,
) -> None:
    receipt = _exact_keys(
        _strict_json_object_payload(
            receipt_payload, receipt_path, "phase-6CQ configuration projection receipt"
        ),
        PHASE6CQ_CONFIGURATION_PROJECTION_RECEIPT_KEYS,
        receipt_path,
        "phase-6CQ configuration projection receipt",
    )
    schema = _exact_keys(
        receipt["schema"],
        frozenset({"name", "version"}),
        receipt_path,
        "phase-6CQ configuration projection receipt.schema",
    )
    _require(
        schema["name"] == CONFIGURATION_PROJECTION_SCHEMA_NAME
        and _uint(
            schema["version"],
            receipt_path,
            "phase-6CQ configuration projection receipt.schema.version",
        )
        == CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        receipt_path,
        "unsupported phase-6CQ configuration projection receipt schema",
    )
    for field in (
        "attestation",
        "policy",
        "mode",
        "record_id",
        "candidate_selector",
        "raw_sha256",
        "restore_operations_contract_sha256",
        "restored_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _nonempty_string(
            receipt[field],
            receipt_path,
            f"phase-6CQ configuration projection receipt.{field}",
        )
    for field in (
        "raw_bytes",
        "restore_operation_count",
        "restore_operations_contract_bytes",
        "restored_bytes",
        "removed_bytes",
        "projected_bytes",
    ):
        _uint(
            receipt[field],
            receipt_path,
            f"phase-6CQ configuration projection receipt.{field}",
        )
    for field in (
        "raw_sha256",
        "restore_operations_contract_sha256",
        "restored_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _sha256(
            receipt[field],
            receipt_path,
            f"phase-6CQ configuration projection receipt.{field}",
        )

    projected_configuration_sha256 = _sha256(
        root["comparability"]["runtime"]["configuration_sha256"],
        source,
        "comparability.runtime.configuration_sha256",
    )
    expected_receipt = {
        "schema": {
            "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
            "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        },
        "attestation": contract["attestation"],
        "policy": contract["policy"],
        "mode": contract["mode"],
        "record_id": contract["record_id"],
        "candidate_selector": candidate_selector,
        "raw_bytes": contract["raw_bytes"],
        "raw_sha256": contract["raw_sha256"],
        "restore_operation_count": contract["restore_operation_count"],
        "restore_operations_contract_bytes": contract[
            "restore_operations_contract_bytes"
        ],
        "restore_operations_contract_sha256": contract[
            "restore_operations_contract_sha256"
        ],
        "restored_bytes": contract["restored_bytes"],
        "restored_sha256": contract["restored_sha256"],
        "phase6cm_block_sha256": contract["phase6cm_block_sha256"],
        "phase6co_block_sha256": contract["phase6co_block_sha256"],
        "removed_bytes": contract["removed_bytes"],
        "removed_segment_sha256": contract["removed_segment_sha256"],
        "projected_bytes": contract["projected_bytes"],
        "projected_sha256": contract["projected_sha256"],
        "epoch_configuration_sha256": projected_configuration_sha256,
    }
    _require(
        receipt == expected_receipt,
        receipt_path,
        "phase-6CQ configuration projection receipt differs from the preregistered contract",
    )
    _require(
        len(raw_payload) == contract["raw_bytes"]
        and hashlib.sha256(raw_payload).hexdigest() == contract["raw_sha256"],
        raw_path,
        "phase-6CQ configuration raw snapshot differs from the preregistered bytes",
    )

    restored_payload = _phase6cq_restore_configuration(
        raw_payload, raw_path, contract
    )
    phase6cm_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_PHASE6CM_BLOCK
    ).hexdigest()
    phase6co_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_PHASE6CO_BLOCK
    ).hexdigest()
    removed_segment_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_REMOVED_SEGMENT
    ).hexdigest()
    _require(
        len(CONFIGURATION_PROJECTION_PHASE6CM_BLOCK) == 124
        and len(CONFIGURATION_PROJECTION_PHASE6CO_BLOCK) == 122
        and phase6cm_sha256 == contract["phase6cm_block_sha256"]
        and phase6co_sha256 == contract["phase6co_block_sha256"]
        and len(CONFIGURATION_PROJECTION_REMOVED_SEGMENT)
        == contract["removed_bytes"]
        and removed_segment_sha256 == contract["removed_segment_sha256"],
        receipt_path,
        "compiled phase-6CQ strip blocks differ from the preregistered receipt",
    )
    _require(
        restored_payload.count(CONFIGURATION_PROJECTION_PHASE6CM_BLOCK) == 1
        and restored_payload.count(CONFIGURATION_PROJECTION_PHASE6CO_BLOCK) == 1
        and restored_payload.count(CONFIGURATION_PROJECTION_REMOVED_SEGMENT) == 1,
        raw_path,
        "phase-6CQ strip blocks are not contiguous and exact-once in the restored configuration",
    )
    projected_payload = restored_payload.replace(
        CONFIGURATION_PROJECTION_REMOVED_SEGMENT, b"", 1
    )
    _require(
        len(projected_payload) == contract["projected_bytes"]
        and hashlib.sha256(projected_payload).hexdigest()
        == contract["projected_sha256"]
        and contract["projected_sha256"] == projected_configuration_sha256,
        raw_path,
        "phase-6CQ projected configuration does not reproduce the immutable epoch digest",
    )


def _phase6cs_restore_operations_payload() -> bytes:
    payload = bytearray(PHASE6CS_CONFIGURATION_RESTORE_CONTRACT_PREFIX)
    names = []
    for index, operation in enumerate(
        PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS
    ):
        _require(
            type(operation) in (list, tuple) and len(operation) == 3,
            "compiled phase-6CS restore operations",
            f"operation {index} must contain name/current/restored",
        )
        name, current, restored = operation
        _require(
            type(name) is str
            and re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            "compiled phase-6CS restore operations",
            f"operation {index} has a non-canonical name",
        )
        try:
            name_bytes = name.encode("ascii", errors="strict")
        except UnicodeError as error:  # pragma: no cover - regex is ASCII-only.
            raise ReportError(
                f"compiled phase-6CS restore operation {index} name is not ASCII: {error}"
            ) from error
        _require(
            type(current) is bytes
            and type(restored) is bytes
            and bool(current)
            and bool(restored)
            and current != restored,
            "compiled phase-6CS restore operations",
            f"operation {name} has invalid byte literals",
        )
        names.append(name)
        payload.extend(len(name_bytes).to_bytes(4, "big"))
        payload.extend(name_bytes)
        payload.extend(len(current).to_bytes(8, "big"))
        payload.extend(current)
        payload.extend(len(restored).to_bytes(8, "big"))
        payload.extend(restored)
    _require(
        len(names) == len(set(names)),
        "compiled phase-6CS restore operations",
        "operation names must be unique",
    )
    return bytes(payload)


def _phase6cs_restore_configuration(
    raw_payload: bytes,
    source: pathlib.Path,
    contract: Mapping[str, Any],
) -> bytes:
    operation_count = _uint(
        contract["restore_operation_count"],
        source,
        "contract.restore_operation_count",
    )
    operations_contract_bytes = _uint(
        contract["restore_operations_contract_bytes"],
        source,
        "contract.restore_operations_contract_bytes",
    )
    operations_contract_sha256 = _sha256(
        contract["restore_operations_contract_sha256"],
        source,
        "contract.restore_operations_contract_sha256",
    )
    compiled_operations_payload = _phase6cs_restore_operations_payload()
    _require(
        operation_count == 8
        and operation_count == len(PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS)
        and len(compiled_operations_payload) == operations_contract_bytes
        and operations_contract_bytes
        == PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
        and hashlib.sha256(compiled_operations_payload).hexdigest()
        == operations_contract_sha256
        and operations_contract_sha256
        == PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256,
        source,
        "compiled phase-6CS restore operations differ from the preregistered contract",
    )

    restored_payload = raw_payload
    for name, current, restored in PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS:
        restored_count = restored_payload.count(restored)
        _require(
            restored_count == 0,
            source,
            (
                f"phase-6CS restore operation {name} replacement is already "
                f"present {restored_count} time(s); pre-restored input is forbidden"
            ),
        )
        source_count = restored_payload.count(current)
        _require(
            source_count == 1,
            source,
            (
                f"phase-6CS restore operation {name} source literal must be "
                f"exact-once, observed {source_count}"
            ),
        )
        restored_payload = restored_payload.replace(current, restored, 1)

    _require(
        len(restored_payload)
        == _uint(
            contract["restored_bytes"],
            source,
            "contract.restored_bytes",
        )
        and hashlib.sha256(restored_payload).hexdigest()
        == _sha256(
            contract["restored_sha256"],
            source,
            "contract.restored_sha256",
        ),
        source,
        "phase-6CS restored configuration does not reproduce the sealed 6CP raw identity",
    )
    return restored_payload


def _validate_phase6cs_configuration_projection(
    root: Mapping[str, Any],
    source: pathlib.Path,
    *,
    contract: Mapping[str, Any],
    candidate_selector: str,
    raw_path: pathlib.Path,
    receipt_path: pathlib.Path,
    raw_payload: bytes,
    receipt_payload: bytes,
) -> None:
    receipt = _exact_keys(
        _strict_json_object_payload(
            receipt_payload, receipt_path, "phase-6CS configuration projection receipt"
        ),
        PHASE6CS_CONFIGURATION_PROJECTION_RECEIPT_KEYS,
        receipt_path,
        "phase-6CS configuration projection receipt",
    )
    schema = _exact_keys(
        receipt["schema"],
        frozenset({"name", "version"}),
        receipt_path,
        "phase-6CS configuration projection receipt.schema",
    )
    _require(
        schema["name"] == CONFIGURATION_PROJECTION_SCHEMA_NAME
        and _uint(
            schema["version"],
            receipt_path,
            "phase-6CS configuration projection receipt.schema.version",
        )
        == CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        receipt_path,
        "unsupported phase-6CS configuration projection receipt schema",
    )
    for field in (
        "attestation",
        "policy",
        "mode",
        "record_id",
        "candidate_selector",
        "raw_sha256",
        "restore_operations_contract_sha256",
        "restored_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _nonempty_string(
            receipt[field],
            receipt_path,
            f"phase-6CS configuration projection receipt.{field}",
        )
    for field in (
        "raw_bytes",
        "restore_operation_count",
        "restore_operations_contract_bytes",
        "restored_bytes",
        "removed_bytes",
        "projected_bytes",
    ):
        _uint(
            receipt[field],
            receipt_path,
            f"phase-6CS configuration projection receipt.{field}",
        )
    for field in (
        "raw_sha256",
        "restore_operations_contract_sha256",
        "restored_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _sha256(
            receipt[field],
            receipt_path,
            f"phase-6CS configuration projection receipt.{field}",
        )

    projected_configuration_sha256 = _sha256(
        root["comparability"]["runtime"]["configuration_sha256"],
        source,
        "comparability.runtime.configuration_sha256",
    )
    expected_receipt = {
        "schema": {
            "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
            "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        },
        "attestation": contract["attestation"],
        "policy": contract["policy"],
        "mode": contract["mode"],
        "record_id": contract["record_id"],
        "candidate_selector": candidate_selector,
        "raw_bytes": contract["raw_bytes"],
        "raw_sha256": contract["raw_sha256"],
        "restore_operation_count": contract["restore_operation_count"],
        "restore_operations_contract_bytes": contract[
            "restore_operations_contract_bytes"
        ],
        "restore_operations_contract_sha256": contract[
            "restore_operations_contract_sha256"
        ],
        "restored_bytes": contract["restored_bytes"],
        "restored_sha256": contract["restored_sha256"],
        "phase6cm_block_sha256": contract["phase6cm_block_sha256"],
        "phase6co_block_sha256": contract["phase6co_block_sha256"],
        "removed_bytes": contract["removed_bytes"],
        "removed_segment_sha256": contract["removed_segment_sha256"],
        "projected_bytes": contract["projected_bytes"],
        "projected_sha256": contract["projected_sha256"],
        "epoch_configuration_sha256": projected_configuration_sha256,
    }
    _require(
        receipt == expected_receipt,
        receipt_path,
        "phase-6CS configuration projection receipt differs from the preregistered contract",
    )
    _require(
        len(raw_payload) == contract["raw_bytes"]
        and hashlib.sha256(raw_payload).hexdigest() == contract["raw_sha256"],
        raw_path,
        "phase-6CS configuration raw snapshot differs from the preregistered bytes",
    )

    restored_payload = _phase6cs_restore_configuration(
        raw_payload, raw_path, contract
    )
    phase6cm_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_PHASE6CM_BLOCK
    ).hexdigest()
    phase6co_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_PHASE6CO_BLOCK
    ).hexdigest()
    removed_segment_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_REMOVED_SEGMENT
    ).hexdigest()
    _require(
        len(CONFIGURATION_PROJECTION_PHASE6CM_BLOCK) == 124
        and len(CONFIGURATION_PROJECTION_PHASE6CO_BLOCK) == 122
        and phase6cm_sha256 == contract["phase6cm_block_sha256"]
        and phase6co_sha256 == contract["phase6co_block_sha256"]
        and len(CONFIGURATION_PROJECTION_REMOVED_SEGMENT)
        == contract["removed_bytes"]
        and removed_segment_sha256 == contract["removed_segment_sha256"],
        receipt_path,
        "compiled phase-6CS strip blocks differ from the preregistered receipt",
    )
    _require(
        restored_payload.count(CONFIGURATION_PROJECTION_PHASE6CM_BLOCK) == 1
        and restored_payload.count(CONFIGURATION_PROJECTION_PHASE6CO_BLOCK) == 1
        and restored_payload.count(CONFIGURATION_PROJECTION_REMOVED_SEGMENT) == 1,
        raw_path,
        "phase-6CS strip blocks are not contiguous and exact-once in the restored configuration",
    )
    projected_payload = restored_payload.replace(
        CONFIGURATION_PROJECTION_REMOVED_SEGMENT, b"", 1
    )
    _require(
        len(projected_payload) == contract["projected_bytes"]
        and hashlib.sha256(projected_payload).hexdigest()
        == contract["projected_sha256"]
        and contract["projected_sha256"] == projected_configuration_sha256,
        raw_path,
        "phase-6CS projected configuration does not reproduce the immutable epoch digest",
    )


def _phase6da_restore_operations_payload() -> bytes:
    payload = bytearray(PHASE6DA_CONFIGURATION_RESTORE_CONTRACT_PREFIX)
    names = []
    for index, operation in enumerate(
        PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS
    ):
        _require(
            type(operation) in (list, tuple) and len(operation) == 3,
            "compiled phase-6DA restore operations",
            f"operation {index} must contain name/current/restored",
        )
        name, current, restored = operation
        _require(
            type(name) is str
            and re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            "compiled phase-6DA restore operations",
            f"operation {index} has a non-canonical name",
        )
        try:
            name_bytes = name.encode("ascii", errors="strict")
        except UnicodeError as error:  # pragma: no cover - regex is ASCII-only.
            raise ReportError(
                f"compiled phase-6DA restore operation {index} name is not ASCII: {error}"
            ) from error
        _require(
            type(current) is bytes
            and type(restored) is bytes
            and bool(current)
            and bool(restored)
            and current != restored,
            "compiled phase-6DA restore operations",
            f"operation {name} has invalid byte literals",
        )
        names.append(name)
        payload.extend(len(name_bytes).to_bytes(4, "big"))
        payload.extend(name_bytes)
        payload.extend(len(current).to_bytes(8, "big"))
        payload.extend(current)
        payload.extend(len(restored).to_bytes(8, "big"))
        payload.extend(restored)
    _require(
        len(names) == len(set(names)),
        "compiled phase-6DA restore operations",
        "operation names must be unique",
    )
    return bytes(payload)


def _phase6da_restore_configuration(
    raw_payload: bytes,
    source: pathlib.Path,
    contract: Mapping[str, Any],
) -> bytes:
    operation_count = _uint(
        contract["restore_operation_count"],
        source,
        "contract.restore_operation_count",
    )
    operations_contract_bytes = _uint(
        contract["restore_operations_contract_bytes"],
        source,
        "contract.restore_operations_contract_bytes",
    )
    operations_contract_sha256 = _sha256(
        contract["restore_operations_contract_sha256"],
        source,
        "contract.restore_operations_contract_sha256",
    )
    compiled_operations_payload = _phase6da_restore_operations_payload()
    _require(
        operation_count == 8
        and operation_count == len(PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS)
        and len(compiled_operations_payload) == operations_contract_bytes
        and operations_contract_bytes
        == PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
        and hashlib.sha256(compiled_operations_payload).hexdigest()
        == operations_contract_sha256
        and operations_contract_sha256
        == PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256,
        source,
        "compiled phase-6DA restore operations differ from the preregistered contract",
    )

    restored_payload = raw_payload
    for name, current, restored in PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS:
        restored_count = restored_payload.count(restored)
        _require(
            restored_count == 0,
            source,
            (
                f"phase-6DA restore operation {name} replacement is already "
                f"present {restored_count} time(s); pre-restored input is forbidden"
            ),
        )
        source_count = restored_payload.count(current)
        _require(
            source_count == 1,
            source,
            (
                f"phase-6DA restore operation {name} source literal must be "
                f"exact-once, observed {source_count}"
            ),
        )
        restored_payload = restored_payload.replace(current, restored, 1)

    _require(
        len(restored_payload)
        == _uint(
            contract["restored_bytes"],
            source,
            "contract.restored_bytes",
        )
        and hashlib.sha256(restored_payload).hexdigest()
        == _sha256(
            contract["restored_sha256"],
            source,
            "contract.restored_sha256",
        ),
        source,
        "phase-6DA restored configuration does not reproduce the sealed 6CP raw identity",
    )
    return restored_payload


def _validate_phase6da_configuration_projection(
    root: Mapping[str, Any],
    source: pathlib.Path,
    *,
    contract: Mapping[str, Any],
    candidate_selector: str,
    raw_path: pathlib.Path,
    receipt_path: pathlib.Path,
    raw_payload: bytes,
    receipt_payload: bytes,
) -> None:
    receipt = _exact_keys(
        _strict_json_object_payload(
            receipt_payload, receipt_path, "phase-6DA configuration projection receipt"
        ),
        PHASE6DA_CONFIGURATION_PROJECTION_RECEIPT_KEYS,
        receipt_path,
        "phase-6DA configuration projection receipt",
    )
    schema = _exact_keys(
        receipt["schema"],
        frozenset({"name", "version"}),
        receipt_path,
        "phase-6DA configuration projection receipt.schema",
    )
    _require(
        schema["name"] == CONFIGURATION_PROJECTION_SCHEMA_NAME
        and _uint(
            schema["version"],
            receipt_path,
            "phase-6DA configuration projection receipt.schema.version",
        )
        == CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        receipt_path,
        "unsupported phase-6DA configuration projection receipt schema",
    )
    for field in (
        "attestation",
        "policy",
        "mode",
        "record_id",
        "candidate_selector",
        "raw_sha256",
        "restore_operations_contract_sha256",
        "restored_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _nonempty_string(
            receipt[field],
            receipt_path,
            f"phase-6DA configuration projection receipt.{field}",
        )
    for field in (
        "raw_bytes",
        "restore_operation_count",
        "restore_operations_contract_bytes",
        "restored_bytes",
        "removed_bytes",
        "projected_bytes",
    ):
        _uint(
            receipt[field],
            receipt_path,
            f"phase-6DA configuration projection receipt.{field}",
        )
    for field in (
        "raw_sha256",
        "restore_operations_contract_sha256",
        "restored_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _sha256(
            receipt[field],
            receipt_path,
            f"phase-6DA configuration projection receipt.{field}",
        )

    projected_configuration_sha256 = _sha256(
        root["comparability"]["runtime"]["configuration_sha256"],
        source,
        "comparability.runtime.configuration_sha256",
    )
    expected_receipt = {
        "schema": {
            "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
            "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        },
        "attestation": contract["attestation"],
        "policy": contract["policy"],
        "mode": contract["mode"],
        "record_id": contract["record_id"],
        "candidate_selector": candidate_selector,
        "raw_bytes": contract["raw_bytes"],
        "raw_sha256": contract["raw_sha256"],
        "restore_operation_count": contract["restore_operation_count"],
        "restore_operations_contract_bytes": contract[
            "restore_operations_contract_bytes"
        ],
        "restore_operations_contract_sha256": contract[
            "restore_operations_contract_sha256"
        ],
        "restored_bytes": contract["restored_bytes"],
        "restored_sha256": contract["restored_sha256"],
        "phase6cm_block_sha256": contract["phase6cm_block_sha256"],
        "phase6co_block_sha256": contract["phase6co_block_sha256"],
        "removed_bytes": contract["removed_bytes"],
        "removed_segment_sha256": contract["removed_segment_sha256"],
        "projected_bytes": contract["projected_bytes"],
        "projected_sha256": contract["projected_sha256"],
        "epoch_configuration_sha256": projected_configuration_sha256,
    }
    _require(
        receipt == expected_receipt,
        receipt_path,
        "phase-6DA configuration projection receipt differs from the preregistered contract",
    )
    _require(
        len(raw_payload) == contract["raw_bytes"]
        and hashlib.sha256(raw_payload).hexdigest() == contract["raw_sha256"],
        raw_path,
        "phase-6DA configuration raw snapshot differs from the preregistered bytes",
    )

    restored_payload = _phase6da_restore_configuration(
        raw_payload, raw_path, contract
    )
    phase6cm_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_PHASE6CM_BLOCK
    ).hexdigest()
    phase6co_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_PHASE6CO_BLOCK
    ).hexdigest()
    removed_segment_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_REMOVED_SEGMENT
    ).hexdigest()
    _require(
        len(CONFIGURATION_PROJECTION_PHASE6CM_BLOCK) == 124
        and len(CONFIGURATION_PROJECTION_PHASE6CO_BLOCK) == 122
        and phase6cm_sha256 == contract["phase6cm_block_sha256"]
        and phase6co_sha256 == contract["phase6co_block_sha256"]
        and len(CONFIGURATION_PROJECTION_REMOVED_SEGMENT)
        == contract["removed_bytes"]
        and removed_segment_sha256 == contract["removed_segment_sha256"],
        receipt_path,
        "compiled phase-6DA strip blocks differ from the preregistered receipt",
    )
    _require(
        restored_payload.count(CONFIGURATION_PROJECTION_PHASE6CM_BLOCK) == 1
        and restored_payload.count(CONFIGURATION_PROJECTION_PHASE6CO_BLOCK) == 1
        and restored_payload.count(CONFIGURATION_PROJECTION_REMOVED_SEGMENT) == 1,
        raw_path,
        "phase-6DA strip blocks are not contiguous and exact-once in the restored configuration",
    )
    projected_payload = restored_payload.replace(
        CONFIGURATION_PROJECTION_REMOVED_SEGMENT, b"", 1
    )
    _require(
        len(projected_payload) == contract["projected_bytes"]
        and hashlib.sha256(projected_payload).hexdigest()
        == contract["projected_sha256"]
        and contract["projected_sha256"] == projected_configuration_sha256,
        raw_path,
        "phase-6DA projected configuration does not reproduce the immutable epoch digest",
    )


def _phasealc_strip_operations_payload() -> bytes:
    payload = bytearray(PHASEALC_CONFIGURATION_STRIP_CONTRACT_PREFIX)
    names = []
    for index, operation in enumerate(PHASEALC_CONFIGURATION_STRIP_OPERATIONS):
        _require(
            type(operation) in (list, tuple) and len(operation) == 2,
            "compiled ALC strip operations",
            f"operation {index} must contain name/block",
        )
        name, block = operation
        _require(
            type(name) is str
            and re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            "compiled ALC strip operations",
            f"operation {index} has a non-canonical name",
        )
        try:
            name_bytes = name.encode("ascii", errors="strict")
        except UnicodeError as error:  # pragma: no cover - regex is ASCII-only.
            raise ReportError(
                f"compiled ALC strip operation {index} name is not ASCII: {error}"
            ) from error
        _require(
            type(block) is bytes and bool(block),
            "compiled ALC strip operations",
            f"operation {name} has an invalid byte block",
        )
        names.append(name)
        payload.extend(name_bytes)
        payload.extend(b"\0")
        payload.extend(block)
    _require(
        len(names) == len(set(names)),
        "compiled ALC strip operations",
        "operation names must be unique",
    )
    return bytes(payload)


def _validate_phasealc_configuration_projection(
    root: Mapping[str, Any],
    source: pathlib.Path,
    *,
    contract: Mapping[str, Any],
    candidate_selector: str,
    raw_path: pathlib.Path,
    receipt_path: pathlib.Path,
    raw_payload: bytes,
    receipt_payload: bytes,
) -> None:
    receipt = _exact_keys(
        _strict_json_object_payload(
            receipt_payload, receipt_path, "ALC configuration projection receipt"
        ),
        PHASEALC_CONFIGURATION_PROJECTION_RECEIPT_KEYS,
        receipt_path,
        "ALC configuration projection receipt",
    )
    schema = _exact_keys(
        receipt["schema"],
        frozenset({"name", "version"}),
        receipt_path,
        "ALC configuration projection receipt.schema",
    )
    _require(
        schema["name"] == CONFIGURATION_PROJECTION_SCHEMA_NAME
        and _uint(
            schema["version"],
            receipt_path,
            "ALC configuration projection receipt.schema.version",
        )
        == CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        receipt_path,
        "unsupported ALC configuration projection receipt schema",
    )
    for field in (
        "attestation",
        "policy",
        "mode",
        "record_id",
        "candidate_selector",
        "raw_sha256",
        "strip_operations_contract_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _nonempty_string(
            receipt[field],
            receipt_path,
            f"ALC configuration projection receipt.{field}",
        )
    for field in (
        "raw_bytes",
        "strip_operation_count",
        "strip_operations_contract_bytes",
        "removed_bytes",
        "projected_bytes",
    ):
        _uint(
            receipt[field],
            receipt_path,
            f"ALC configuration projection receipt.{field}",
        )
    for field in (
        "raw_sha256",
        "strip_operations_contract_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _sha256(
            receipt[field],
            receipt_path,
            f"ALC configuration projection receipt.{field}",
        )

    projected_configuration_sha256 = _sha256(
        root["comparability"]["runtime"]["configuration_sha256"],
        source,
        "comparability.runtime.configuration_sha256",
    )
    expected_receipt = {
        "schema": {
            "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
            "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        },
        "attestation": contract["attestation"],
        "policy": contract["policy"],
        "mode": contract["mode"],
        "record_id": contract["record_id"],
        "candidate_selector": candidate_selector,
        "raw_bytes": contract["raw_bytes"],
        "raw_sha256": contract["raw_sha256"],
        "strip_operation_count": contract["strip_operation_count"],
        "strip_operations_contract_bytes": contract[
            "strip_operations_contract_bytes"
        ],
        "strip_operations_contract_sha256": contract[
            "strip_operations_contract_sha256"
        ],
        "removed_bytes": contract["removed_bytes"],
        "removed_segment_sha256": contract["removed_segment_sha256"],
        "projected_bytes": contract["projected_bytes"],
        "projected_sha256": contract["projected_sha256"],
        "epoch_configuration_sha256": projected_configuration_sha256,
    }
    _require(
        receipt == expected_receipt,
        receipt_path,
        "ALC configuration projection receipt differs from the preregistered contract",
    )
    _require(
        len(raw_payload) == contract["raw_bytes"]
        and hashlib.sha256(raw_payload).hexdigest() == contract["raw_sha256"],
        raw_path,
        "ALC configuration raw snapshot differs from the preregistered bytes",
    )

    operations_payload = _phasealc_strip_operations_payload()
    removed_segment = b"".join(
        block for _, block in PHASEALC_CONFIGURATION_STRIP_OPERATIONS
    )
    _require(
        contract["strip_operation_count"] == 3
        and contract["strip_operation_count"]
        == len(PHASEALC_CONFIGURATION_STRIP_OPERATIONS)
        and len(operations_payload)
        == contract["strip_operations_contract_bytes"]
        == PHASEALC_CONFIGURATION_STRIP_OPERATIONS_CONTRACT_BYTES
        and hashlib.sha256(operations_payload).hexdigest()
        == contract["strip_operations_contract_sha256"]
        == PHASEALC_CONFIGURATION_STRIP_OPERATIONS_CONTRACT_SHA256
        and len(removed_segment)
        == contract["removed_bytes"]
        == len(PHASEALC_CONFIGURATION_REMOVED_SEGMENT)
        and hashlib.sha256(removed_segment).hexdigest()
        == contract["removed_segment_sha256"],
        receipt_path,
        "compiled ALC strip operations differ from the preregistered contract",
    )
    _require(
        len(PHASEALC_CONFIGURATION_AHC_BLOCK) == 126
        and hashlib.sha256(PHASEALC_CONFIGURATION_AHC_BLOCK).hexdigest()
        == "ce5de28a9496702deaf942acd859a0b8b55befbbe172284a22af52a3dbd960f8"
        and len(PHASEALC_CONFIGURATION_ALC_BLOCK) == 106
        and hashlib.sha256(PHASEALC_CONFIGURATION_ALC_BLOCK).hexdigest()
        == "953114130970c8adf91bd3c7851857540484bbd1b282a11379872a9565fafbdb"
        and len(PHASEALC_CONFIGURATION_PURGE_BLOCK) == 123
        and hashlib.sha256(PHASEALC_CONFIGURATION_PURGE_BLOCK).hexdigest()
        == "8214513341e73e45fdb734c91c8302287c4e7a15c286c02bf32061937a4a95de",
        receipt_path,
        "compiled ALC CMake-cache blocks differ from their sealed identities",
    )

    projected_payload = raw_payload
    for name, block in PHASEALC_CONFIGURATION_STRIP_OPERATIONS:
        block_count = projected_payload.count(block)
        _require(
            block_count == 1,
            raw_path,
            f"ALC strip operation {name} block must be exact-once, observed {block_count}",
        )
        projected_payload = projected_payload.replace(block, b"", 1)
    _require(
        len(projected_payload) == contract["projected_bytes"]
        and hashlib.sha256(projected_payload).hexdigest()
        == contract["projected_sha256"]
        and contract["projected_sha256"] == projected_configuration_sha256,
        raw_path,
        "ALC projected configuration does not reproduce the immutable epoch digest",
    )


def _validate_selected_configuration_projection(
    root: Mapping[str, Any],
    source: pathlib.Path,
    *,
    contract: Mapping[str, Any],
) -> None:
    provenance = _mapping(root["provenance"], source, "provenance")
    selectors = _mapping(
        provenance["selectors"], source, "provenance.selectors"
    )
    expected_selectors = _mapping(
        selectors["expected"], source, "provenance.selectors.expected"
    )
    artifacts = _mapping(
        provenance["artifacts_sha256"],
        source,
        "provenance.artifacts_sha256",
    )
    projection_artifact_names = frozenset(CONFIGURATION_PROJECTION_ARTIFACTS)
    present_artifact_names = projection_artifact_names & frozenset(artifacts)
    _require(
        not present_artifact_names
        or present_artifact_names == projection_artifact_names,
        source,
        "configuration projection artifact receipts must be present all-or-none",
    )

    record_root = source.parent
    projection_paths = {
        name: record_root / filename
        for name, filename in CONFIGURATION_PROJECTION_ARTIFACTS.items()
    }
    physical_projection_artifacts = {
        name
        for name, path in projection_paths.items()
        if path.exists() or path.is_symlink()
    }
    candidate_selector = _nonempty_string(
        contract["candidate_selector"], source, "contract.candidate_selector"
    )
    contract_record_id = _nonempty_string(
        contract["record_id"], source, "contract.record_id"
    )

    selector_bundle = _mapping(
        contract["selector_bundle"], source, "contract.selector_bundle"
    )
    _require(
        selector_bundle.get(candidate_selector) is True
        and candidate_selector in expected_selectors
        and type(expected_selectors[candidate_selector]) is bool
        and expected_selectors[candidate_selector] is True,
        source,
        "configuration projection candidate selector must be the exact JSON boolean true",
    )
    protocol = _mapping(root["protocol"], source, "protocol")
    lineage = _mapping(root["lineage"], source, "lineage")
    scope_matches = (
        protocol["protocol_id"]
        == _nonempty_string(
            contract["protocol_id"], source, "contract.protocol_id"
        )
        and provenance["record_id"] == contract_record_id
        and lineage["generation"]
        == _uint(contract["generation"], source, "contract.generation")
        and lineage["parent_record_id"] == contract["parent_record_id"]
        and lineage["original_role"]
        == _nonempty_string(
            contract["original_role"], source, "contract.original_role"
        )
        and set(expected_selectors) == set(selector_bundle)
        and all(
            type(expected_selectors[name]) is type(expected)
            and expected_selectors[name] == expected
            for name, expected in selector_bundle.items()
        )
    )
    _require(
        scope_matches,
        source,
        "configuration projection is outside its exact protocol, lineage, and typed selector-bundle scope",
    )
    projection_configured = contract.get("projection_configured", True)
    _require(
        type(projection_configured) is bool,
        source,
        "configuration projection contract projection_configured must be a JSON boolean",
    )
    _require(
        projection_configured,
        source,
        (
            f"configuration projection for {contract_record_id!r} is not "
            "preregistered: the exact byte transformation is not configured"
        ),
    )
    _require(
        present_artifact_names == projection_artifact_names
        and physical_projection_artifacts == projection_artifact_names,
        source,
        f"{contract_record_id} manifest is missing configuration projection artifacts",
    )

    raw_path = projection_paths["configuration_cache_raw"]
    receipt_path = projection_paths["configuration_projection_receipt"]
    raw_payload = _read_sealed_projection_artifact(
        raw_path, maximum_bytes=_uint(contract["raw_bytes"], source, "contract.raw_bytes")
    )
    receipt_payload = _read_sealed_projection_artifact(
        receipt_path, maximum_bytes=64 * 1024
    )
    _require(
        hashlib.sha256(raw_payload).hexdigest()
        == artifacts["configuration_cache_raw"],
        raw_path,
        "configuration raw snapshot hash differs from manifest",
    )
    _require(
        hashlib.sha256(receipt_payload).hexdigest()
        == artifacts["configuration_projection_receipt"],
        receipt_path,
        "configuration projection receipt hash differs from manifest",
    )

    projection_kind = contract.get("projection_kind")
    if projection_kind is not None:
        projection_kind = _nonempty_string(
            projection_kind, source, "contract.projection_kind"
        )
        if projection_kind == PHASE6CQ_CONFIGURATION_PROJECTION_KIND:
            _validate_phase6cq_configuration_projection(
                root,
                source,
                contract=contract,
                candidate_selector=candidate_selector,
                raw_path=raw_path,
                receipt_path=receipt_path,
                raw_payload=raw_payload,
                receipt_payload=receipt_payload,
            )
        elif projection_kind == PHASE6CS_CONFIGURATION_PROJECTION_KIND:
            _validate_phase6cs_configuration_projection(
                root,
                source,
                contract=contract,
                candidate_selector=candidate_selector,
                raw_path=raw_path,
                receipt_path=receipt_path,
                raw_payload=raw_payload,
                receipt_payload=receipt_payload,
            )
        elif projection_kind == PHASE6DA_CONFIGURATION_PROJECTION_KIND:
            _validate_phase6da_configuration_projection(
                root,
                source,
                contract=contract,
                candidate_selector=candidate_selector,
                raw_path=raw_path,
                receipt_path=receipt_path,
                raw_payload=raw_payload,
                receipt_payload=receipt_payload,
            )
        elif projection_kind == PHASEALC_CONFIGURATION_PROJECTION_KIND:
            _validate_phasealc_configuration_projection(
                root,
                source,
                contract=contract,
                candidate_selector=candidate_selector,
                raw_path=raw_path,
                receipt_path=receipt_path,
                raw_payload=raw_payload,
                receipt_payload=receipt_payload,
            )
        else:
            raise ReportError(
                f"{source}: unsupported configuration projection kind"
            )
        return

    receipt = _exact_keys(
        _strict_json_object_payload(
            receipt_payload, receipt_path, "configuration projection receipt"
        ),
        CONFIGURATION_PROJECTION_RECEIPT_KEYS,
        receipt_path,
        "configuration projection receipt",
    )
    schema = _exact_keys(
        receipt["schema"],
        frozenset({"name", "version"}),
        receipt_path,
        "configuration projection receipt.schema",
    )
    _require(
        schema["name"] == CONFIGURATION_PROJECTION_SCHEMA_NAME
        and _uint(
            schema["version"],
            receipt_path,
            "configuration projection receipt.schema.version",
        )
        == CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        receipt_path,
        "unsupported configuration projection receipt schema",
    )
    for field in (
        "attestation",
        "policy",
        "mode",
        "record_id",
        "candidate_selector",
        "raw_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _nonempty_string(
            receipt[field], receipt_path, f"configuration projection receipt.{field}"
        )
    for field in ("raw_bytes", "removed_bytes", "projected_bytes"):
        _uint(
            receipt[field], receipt_path, f"configuration projection receipt.{field}"
        )
    for field in (
        "raw_sha256",
        "phase6cm_block_sha256",
        "phase6co_block_sha256",
        "removed_segment_sha256",
        "projected_sha256",
        "epoch_configuration_sha256",
    ):
        _sha256(
            receipt[field], receipt_path, f"configuration projection receipt.{field}"
        )

    phase6cm_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_PHASE6CM_BLOCK
    ).hexdigest()
    phase6co_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_PHASE6CO_BLOCK
    ).hexdigest()
    removed_segment_sha256 = hashlib.sha256(
        CONFIGURATION_PROJECTION_REMOVED_SEGMENT
    ).hexdigest()
    projected_configuration_sha256 = _sha256(
        root["comparability"]["runtime"]["configuration_sha256"],
        source,
        "comparability.runtime.configuration_sha256",
    )
    attestation = _nonempty_string(
        contract.get("attestation", CONFIGURATION_PROJECTION_ATTESTATION),
        source,
        "contract.attestation",
    )
    policy = _nonempty_string(
        contract.get("policy", CONFIGURATION_PROJECTION_POLICY),
        source,
        "contract.policy",
    )
    mode = _nonempty_string(
        contract.get("mode", CONFIGURATION_PROJECTION_MODE),
        source,
        "contract.mode",
    )
    expected_receipt = {
        "schema": {
            "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
            "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
        },
        "attestation": attestation,
        "policy": policy,
        "mode": mode,
        "record_id": contract["record_id"],
        "candidate_selector": candidate_selector,
        "raw_bytes": contract["raw_bytes"],
        "raw_sha256": contract["raw_sha256"],
        "phase6cm_block_sha256": contract["phase6cm_block_sha256"],
        "phase6co_block_sha256": contract["phase6co_block_sha256"],
        "removed_bytes": contract["removed_bytes"],
        "removed_segment_sha256": contract["removed_segment_sha256"],
        "projected_bytes": contract["projected_bytes"],
        "projected_sha256": contract["projected_sha256"],
        "epoch_configuration_sha256": projected_configuration_sha256,
    }
    _require(
        receipt == expected_receipt,
        receipt_path,
        "configuration projection receipt differs from the preregistered contract",
    )
    _require(
        len(raw_payload) == contract["raw_bytes"]
        and hashlib.sha256(raw_payload).hexdigest() == contract["raw_sha256"],
        raw_path,
        "configuration raw snapshot differs from the preregistered bytes",
    )
    _require(
        len(CONFIGURATION_PROJECTION_PHASE6CM_BLOCK) == 124
        and len(CONFIGURATION_PROJECTION_PHASE6CO_BLOCK) == 122
        and phase6cm_sha256 == contract["phase6cm_block_sha256"]
        and phase6co_sha256 == contract["phase6co_block_sha256"]
        and len(CONFIGURATION_PROJECTION_REMOVED_SEGMENT)
        == contract["removed_bytes"]
        and removed_segment_sha256 == contract["removed_segment_sha256"],
        receipt_path,
        "compiled configuration projection blocks differ from the receipt",
    )
    _require(
        raw_payload.count(CONFIGURATION_PROJECTION_PHASE6CM_BLOCK) == 1
        and raw_payload.count(CONFIGURATION_PROJECTION_PHASE6CO_BLOCK) == 1
        and raw_payload.count(CONFIGURATION_PROJECTION_REMOVED_SEGMENT) == 1,
        raw_path,
        "configuration projection blocks are not contiguous and exact-once",
    )
    projected_payload = raw_payload.replace(
        CONFIGURATION_PROJECTION_REMOVED_SEGMENT, b"", 1
    )
    _require(
        len(projected_payload) == contract["projected_bytes"]
        and hashlib.sha256(projected_payload).hexdigest()
        == contract["projected_sha256"]
        and contract["projected_sha256"] == projected_configuration_sha256,
        raw_path,
        "projected configuration does not reproduce the immutable epoch digest",
    )


def _configuration_projection_contracts(
    source: pathlib.Path,
    *,
    contract: Optional[Mapping[str, Any]],
    contracts: Optional[Sequence[Mapping[str, Any]]],
) -> Tuple[Mapping[str, Any], ...]:
    _require(
        contract is None or contracts is None,
        source,
        "configuration projection contract overrides are mutually exclusive",
    )
    if contract is not None:
        selected = (contract,)
    elif contracts is not None:
        selected = tuple(contracts)
    else:
        selected = CONFIGURATION_PROJECTION_CONTRACTS
    _require(selected, source, "configuration projection contract set is empty")

    record_ids = []
    candidate_selectors = []
    for index, item in enumerate(selected):
        item = _mapping(item, source, f"configuration projection contract[{index}]")
        record_ids.append(
            _nonempty_string(
                item.get("record_id"),
                source,
                f"configuration projection contract[{index}].record_id",
            )
        )
        candidate_selector = _nonempty_string(
            item.get("candidate_selector"),
            source,
            f"configuration projection contract[{index}].candidate_selector",
        )
        candidate_selectors.append(candidate_selector)
        selector_bundle = _mapping(
            item.get("selector_bundle"),
            source,
            f"configuration projection contract[{index}].selector_bundle",
        )
        _require(
            selector_bundle.get(candidate_selector) is True,
            source,
            (
                f"configuration projection contract[{index}] candidate selector "
                "must be enabled by exact JSON boolean true"
            ),
        )
        for selector_name, expected in selector_bundle.items():
            _nonempty_string(
                selector_name,
                source,
                f"configuration projection contract[{index}].selector_bundle key",
            )
            _require(
                type(expected) is bool,
                source,
                (
                    f"configuration projection contract[{index}].selector_bundle."
                    f"{selector_name} must be a JSON boolean"
                ),
            )
    _require(
        len(record_ids) == len(set(record_ids)),
        source,
        "configuration projection contracts contain duplicate record IDs",
    )
    _require(
        len(candidate_selectors) == len(set(candidate_selectors)),
        source,
        "configuration projection contracts contain duplicate candidate selectors",
    )
    return selected


def _structured_receipt_fields(
    receipt: Any, source: pathlib.Path
) -> Mapping[str, str]:
    line = _nonempty_string(
        receipt, source, "provenance.run_receipt"
    )
    _require(
        line.startswith(A64_CONFIGURATION_PREFIX),
        source,
        "phase-6DA direct run receipt is not the structured A64 configuration line",
    )
    fields: Dict[str, str] = {}
    for token in line[len(A64_CONFIGURATION_PREFIX) :].strip().split():
        _require(
            token.count("=") == 1,
            source,
            f"malformed phase-6DA direct receipt token {token!r}",
        )
        name, value = token.split("=", 1)
        _require(
            re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None and value,
            source,
            f"non-canonical phase-6DA direct receipt token {token!r}",
        )
        _require(
            name not in fields,
            source,
            f"duplicate phase-6DA direct receipt field {name}",
        )
        fields[name] = value
    _require(fields, source, "empty phase-6DA direct run receipt")
    return fields


def _validate_phase6da_direct_record(
    root: Mapping[str, Any], source: pathlib.Path
) -> bool:
    """Validate a preregistered direct-config B/C record, if signalled."""

    provenance = _mapping(root["provenance"], source, "provenance")
    record_id = provenance["record_id"]
    selectors = _mapping(
        provenance["selectors"], source, "provenance.selectors"
    )
    expected_selectors = _mapping(
        selectors["expected"], source, "provenance.selectors.expected"
    )
    effective_selectors = _mapping(
        selectors["effective"], source, "provenance.selectors.effective"
    )
    record_signal = record_id in PHASE6DA_DIRECT_RECORD_IDS
    ifsm_selector_signal = (
        PHASEIFSM_DIRECT_SELECTOR in expected_selectors
        or PHASEIFSM_DIRECT_SELECTOR in effective_selectors
    )
    fcfos_selector_signal = (
        PHASEFCFOS_DIRECT_SELECTOR in expected_selectors
        or PHASEFCFOS_DIRECT_SELECTOR in effective_selectors
    )
    alc_selector_signal = (
        PHASEALC_DIRECT_SELECTOR in expected_selectors
        or PHASEALC_DIRECT_SELECTOR in effective_selectors
    )
    metal_bundle_selector_signal = bool(
        PHASEMETAL_RUNTIME_BUNDLE_SELECTORS
        & (set(expected_selectors) | set(effective_selectors))
    )
    noda_selector_signal = (
        PHASENODA_DIRECT_SELECTOR in expected_selectors
        or PHASENODA_DIRECT_SELECTOR in effective_selectors
    )
    ufsb_imse_selector_signal = bool(
        PHASEUFSB_IMSE_DIRECT_SELECTORS
        & (set(expected_selectors) | set(effective_selectors))
    )
    if (
        not record_signal
        and not ifsm_selector_signal
        and not fcfos_selector_signal
        and not alc_selector_signal
        and not metal_bundle_selector_signal
        and not noda_selector_signal
        and not ufsb_imse_selector_signal
    ):
        return False
    if not record_signal:
        _require(
            not noda_selector_signal,
            source,
            "NODA selector is outside its preregistered g5 record",
        )
        _require(
            not metal_bundle_selector_signal,
            source,
            "MSORE/MSFC/MDSSC selectors are outside their preregistered g4 record",
        )
        _require(
            not alc_selector_signal,
            source,
            "ALC direct selector is outside its preregistered g3 record",
        )
        _require(
            not fcfos_selector_signal,
            source,
            "FCFOS direct selector is outside its preregistered g3 record",
        )
        _require(
            not ufsb_imse_selector_signal,
            source,
            "UFSB/IMSE direct selector is outside its preregistered combined g3 record",
        )
        _require(
            False,
            source,
            "IFSM direct selector is outside its preregistered g2 record",
        )

    protocol = _mapping(root["protocol"], source, "protocol")
    comparability = _mapping(
        root["comparability"], source, "comparability"
    )
    os_comparability = _mapping(
        comparability["os"], source, "comparability.os"
    )
    runtime = _mapping(
        comparability["runtime"], source, "comparability.runtime"
    )
    _require(
        protocol["protocol_id"] == PHASE6DA_DIRECT_PROTOCOL_ID,
        source,
        "phase-6DA direct record is outside the retained protocol",
    )
    _require(
        os_comparability["name"] == "macOS"
        and os_comparability["version"] == PHASE6DA_DIRECT_OS_VERSION
        and os_comparability["build"] == PHASE6DA_DIRECT_OS_BUILD
        and os_comparability["policy"] == PHASE6DA_DIRECT_OS_POLICY
        and (
            record_id
            in {
                PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
                PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
                PHASENODA_DIRECT_CANDIDATE_RECORD_ID,
            }
            or runtime["configuration_sha256"]
            == PHASE6DA_DIRECT_CONFIGURATION_SHA256
        ),
        source,
        "phase-6DA direct record differs from its preregistered OS/build or raw configuration identity",
    )

    matches = [
        contract
        for contract in PHASE6DA_DIRECT_CONTRACTS
        if contract["record_id"] == record_id
    ]
    _require(
        len(matches) == 1,
        source,
        "phase-6DA direct record contract is absent or ambiguous",
    )
    contract = matches[0]
    lineage = _mapping(root["lineage"], source, "lineage")
    selector_bundle = _mapping(
        contract["selector_bundle"], source, "phase-6DA direct selector_bundle"
    )
    effective_selector_bundle = _mapping(
        contract.get("effective_selector_bundle", selector_bundle),
        source,
        "phase-6DA direct effective_selector_bundle",
    )
    if "primary_selector" in contract:
        primary_selector = contract["primary_selector"]
        companion_selectors = contract["companion_selectors"]
        expected_primary = (
            PHASEMSORE_DIRECT_SELECTOR
            if record_id == PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
            else PHASEUFSB_DIRECT_SELECTOR
        )
        expected_companions = (
            frozenset(
                {PHASEMSFC_COMPANION_SELECTOR, PHASEMDSSC_COMPANION_SELECTOR}
            )
            if record_id == PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
            else frozenset({PHASEIMSE_DIRECT_SELECTOR})
        )
        _require(
            primary_selector == expected_primary
            and companion_selectors == expected_companions
            and selector_bundle.get(primary_selector) is True
            and all(
                selector_bundle.get(name) is True
                for name in companion_selectors
            ),
            source,
            "combined direct record primary/companion selector contract drifted",
        )
    if "candidate_marker_line" in contract:
        expected_marker_lines = {
            PHASEFCFOS_DIRECT_CANDIDATE_RECORD_ID: (
                PHASEFCFOS_DIRECT_CANDIDATE_MARKER_LINE
            ),
            PHASEALC_DIRECT_CANDIDATE_RECORD_ID: (
                PHASEALC_DIRECT_CANDIDATE_MARKER_LINE
            ),
            PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID: (
                PHASEMSORE_DIRECT_CANDIDATE_MARKER_LINE
            ),
            PHASENODA_DIRECT_CANDIDATE_RECORD_ID: (
                PHASENODA_DIRECT_CANDIDATE_MARKER_LINE
            ),
        }
        _require(
            contract.get("candidate_marker_line")
            == expected_marker_lines.get(record_id),
            source,
            "direct exact candidate-marker contract drifted",
        )
    if record_id == PHASENODA_DIRECT_CANDIDATE_RECORD_ID:
        _require(
            contract.get("retained_marker_lines")
            == PHASENODA_RETAINED_MARKER_LINES
            and PHASEPBA_DIRECT_SELECTOR not in selector_bundle
            and PHASEPBA_DIRECT_SELECTOR not in effective_selector_bundle,
            source,
            "NODA retained-marker or PBA-absence contract drifted",
        )
    _require(
        lineage["generation"] == contract["generation"]
        and lineage["parent_record_id"] == contract["parent_record_id"]
        and lineage["original_role"] == contract["original_role"]
        and set(expected_selectors) == set(selector_bundle)
        and set(effective_selectors) == set(effective_selector_bundle)
        and all(
            type(expected_selectors[name]) is bool
            and expected_selectors[name] is expected
            for name, expected in selector_bundle.items()
        )
        and all(
            type(effective_selectors[name]) is bool
            and effective_selectors[name] is expected
            for name, expected in effective_selector_bundle.items()
        ),
        source,
        "phase-6DA direct record is outside its exact lineage and typed selector-bundle scope",
    )

    receipt_fields = _structured_receipt_fields(
        provenance["run_receipt"], source
    )
    _require(
        all(
            receipt_fields.get(name) == expected
            for name, expected in contract["required_receipt_fields"].items()
        ),
        source,
        "phase-6DA direct run receipt differs from its role-specific contract",
    )
    lifecycle = _mapping(root["lifecycle"], source, "lifecycle")
    marker = _mapping(
        lifecycle["candidate_marker"], source, "lifecycle.candidate_marker"
    )
    if "candidate_marker_line" in contract:
        _require(
            marker.get("line")
            == contract.get("candidate_marker_line"),
            source,
            "direct exact candidate-marker line differs from the sealed manifest contract",
        )
    _require(
        marker == contract["candidate_marker"],
        source,
        "phase-6DA direct candidate-marker lifecycle differs from its role-specific contract",
    )
    return True


def _validate_configuration_projection(
    root: Mapping[str, Any],
    source: pathlib.Path,
    *,
    contract: Optional[Mapping[str, Any]] = None,
    contracts: Optional[Sequence[Mapping[str, Any]]] = None,
) -> None:
    projection_contracts = _configuration_projection_contracts(
        source, contract=contract, contracts=contracts
    )
    provenance = _mapping(root["provenance"], source, "provenance")
    selectors = _mapping(
        provenance["selectors"], source, "provenance.selectors"
    )
    expected_selectors = _mapping(
        selectors["expected"], source, "provenance.selectors.expected"
    )
    effective_selectors = _mapping(
        selectors["effective"], source, "provenance.selectors.effective"
    )
    selector_names = frozenset(expected_selectors) | frozenset(effective_selectors)
    artifacts = _mapping(
        provenance["artifacts_sha256"],
        source,
        "provenance.artifacts_sha256",
    )
    projection_artifact_names = frozenset(CONFIGURATION_PROJECTION_ARTIFACTS)
    present_artifact_names = projection_artifact_names & frozenset(artifacts)
    _require(
        not present_artifact_names
        or present_artifact_names == projection_artifact_names,
        source,
        "configuration projection artifact receipts must be present all-or-none",
    )
    projection_paths = {
        name: source.parent / filename
        for name, filename in CONFIGURATION_PROJECTION_ARTIFACTS.items()
    }
    physical_projection_artifacts = {
        name
        for name, path in projection_paths.items()
        if path.exists() or path.is_symlink()
    }
    artifact_signal = bool(present_artifact_names or physical_projection_artifacts)

    direct_phase6da_record = _validate_phase6da_direct_record(root, source)
    if direct_phase6da_record:
        if provenance["record_id"] not in {
            PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
            PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
            PHASENODA_DIRECT_CANDIDATE_RECORD_ID,
        }:
            _require(
                not present_artifact_names and not physical_projection_artifacts,
                source,
                "phase-6DA direct records forbid configuration projection artifacts",
            )
            return

    record_matches = [
        item
        for item in projection_contracts
        if provenance["record_id"] == item["record_id"]
    ]
    selector_matches = [
        item
        for item in projection_contracts
        if item["candidate_selector"] in selector_names
    ]
    if record_matches:
        _require(
            len(record_matches) == 1,
            source,
            "configuration projection record signal is ambiguous",
        )
        selected_contract = record_matches[0]
        selected_bundle = _mapping(
            selected_contract["selector_bundle"],
            source,
            "selected configuration projection contract.selector_bundle",
        )
        conflicting_selectors = sorted(
            item["candidate_selector"]
            for item in selector_matches
            if item["candidate_selector"] not in selected_bundle
        )
        _require(
            not conflicting_selectors,
            source,
            (
                "configuration projection record and candidate-selector signals "
                f"conflict: {', '.join(conflicting_selectors)}"
            ),
        )
    elif selector_matches:
        # Selector bundles are cumulative across generations.  Select the one
        # whose bundle subsumes every signalled candidate selector; reject
        # incomparable contracts rather than relying on tuple ordering.
        candidates = []
        signalled_selectors = {
            item["candidate_selector"] for item in selector_matches
        }
        for item in selector_matches:
            bundle = _mapping(
                item["selector_bundle"],
                source,
                "signalled configuration projection contract.selector_bundle",
            )
            if signalled_selectors <= set(bundle):
                candidates.append(item)
        _require(
            len(candidates) == 1,
            source,
            "configuration projection candidate-selector signals are ambiguous",
        )
        selected_contract = candidates[0]
    else:
        _require(
            not artifact_signal,
            source,
            (
                "configuration projection artifacts are present without a "
                "recognized record or candidate-selector contract signal"
            ),
        )
        return

    _validate_selected_configuration_projection(
        root, source, contract=selected_contract
    )


def _first_difference(left: Any, right: Any, path: str) -> Optional[str]:
    if type(left) is not type(right):
        return f"{path}: type {type(left).__name__} != {type(right).__name__}"
    if type(left) is dict:
        left_keys = set(left)
        right_keys = set(right)
        if left_keys != right_keys:
            missing = sorted(left_keys - right_keys)
            extra = sorted(right_keys - left_keys)
            return f"{path}: candidate missing={missing} extra={extra}"
        for key in sorted(left):
            difference = _first_difference(left[key], right[key], f"{path}.{key}")
            if difference is not None:
                return difference
        return None
    if type(left) is list:
        if len(left) != len(right):
            return f"{path}: length {len(left)} != {len(right)}"
        for index, (left_item, right_item) in enumerate(zip(left, right)):
            difference = _first_difference(left_item, right_item, f"{path}[{index}]")
            if difference is not None:
                return difference
        return None
    if left != right:
        return f"{path}: baseline={left!r} candidate={right!r}"
    return None


def _validate_schema(root: Mapping[str, Any], source: pathlib.Path) -> None:
    schema = _exact_keys(root["schema"], frozenset({"name", "version"}), source, "schema")
    _require(schema["name"] == MANIFEST_SCHEMA_NAME, source, "unsupported schema.name")
    _require(
        _uint(schema["version"], source, "schema.version") == MANIFEST_SCHEMA_VERSION,
        source,
        "unsupported schema.version",
    )


def _validate_protocol(root: Mapping[str, Any], source: pathlib.Path) -> None:
    protocol = _exact_keys(root["protocol"], PROTOCOL_KEYS, source, "protocol")
    _nonempty_string(protocol["protocol_id"], source, "protocol.protocol_id")
    _require(
        _uint(protocol["protocol_revision"], source, "protocol.protocol_revision") > 0,
        source,
        "protocol.protocol_revision must be positive",
    )
    for key, expected in PROTOCOL_CONSTANTS.items():
        actual = (
            _uint(protocol[key], source, f"protocol.{key}")
            if type(expected) is int
            else _nonempty_string(protocol[key], source, f"protocol.{key}")
        )
        _require(actual == expected, source, f"protocol.{key} must equal {expected!r}")


def _validate_comparability(root: Mapping[str, Any], source: pathlib.Path) -> None:
    comparability = _exact_keys(
        root["comparability"], COMPARABILITY_KEYS, source, "comparability"
    )
    sections: Dict[str, Mapping[str, Any]] = {}
    for section, keys in COMPARABILITY_SHAPES.items():
        sections[section] = _exact_keys(
            comparability[section], keys, source, f"comparability.{section}"
        )

    for key in ("host_id", "model"):
        _nonempty_string(sections["host"][key], source, f"comparability.host.{key}")
    _require(
        sections["host"]["architecture"] == "arm64",
        source,
        "comparability.host.architecture must be arm64",
    )
    for key in COMPARABILITY_SHAPES["os"]:
        _nonempty_string(sections["os"][key], source, f"comparability.os.{key}")
    _require(sections["os"]["name"] == "macOS", source, "comparability.os.name must be macOS")
    for key in COMPARABILITY_SHAPES["power"]:
        _nonempty_string(sections["power"][key], source, f"comparability.power.{key}")

    workload = sections["workload"]
    for key in ("rom_sha256", "lua_sha256", "save_template_sha256"):
        _sha256(workload[key], source, f"comparability.workload.{key}")
    _nonempty_string(workload["cache_policy"], source, "comparability.workload.cache_policy")
    if workload["cache_seed_sha256"] is not None:
        _sha256(
            workload["cache_seed_sha256"],
            source,
            "comparability.workload.cache_seed_sha256",
        )

    runtime = sections["runtime"]
    _sha256(
        runtime["sterile_environment_sha256"],
        source,
        "comparability.runtime.sterile_environment_sha256",
    )
    _sha256(runtime["configuration_sha256"], source, "comparability.runtime.configuration_sha256")
    _nonempty_string(runtime["backend"], source, "comparability.runtime.backend")

    for key in COMPARABILITY_SHAPES["scenes"]:
        _nonempty_string(sections["scenes"][key], source, f"comparability.scenes.{key}")
    for key in COMPARABILITY_SHAPES["tool_schemas"]:
        _nonempty_string(
            sections["tool_schemas"][key], source, f"comparability.tool_schemas.{key}"
        )
    _require(
        sections["tool_schemas"]["report"] == REPORT_TOOL_SCHEMA,
        source,
        f"comparability.tool_schemas.report must be {REPORT_TOOL_SCHEMA!r}",
    )


def _validate_provenance(root: Mapping[str, Any], source: pathlib.Path) -> None:
    provenance = _exact_keys(root["provenance"], PROVENANCE_KEYS, source, "provenance")
    _nonempty_string(provenance["record_id"], source, "provenance.record_id")
    _source_revision(provenance["source_revision"], source, "provenance.source_revision")
    for key in (
        "binary_sha256",
        "diff_sha256",
        "harness_sha256",
        "announcement_sidecar_sha256",
        "report_tool_sha256",
        "selector_receipt_sha256",
    ):
        _sha256(provenance[key], source, f"provenance.{key}")
    _nonempty_string(provenance["run_receipt"], source, "provenance.run_receipt")

    selectors = _exact_keys(
        provenance["selectors"],
        frozenset({"expected", "effective"}),
        source,
        "provenance.selectors",
    )
    expected = _mapping(selectors["expected"], source, "provenance.selectors.expected")
    effective = _mapping(selectors["effective"], source, "provenance.selectors.effective")
    _require(expected, source, "provenance.selectors.expected must not be empty")
    _validate_json_receipt_value(expected, source, "provenance.selectors.expected")
    _validate_json_receipt_value(effective, source, "provenance.selectors.effective")
    difference = _first_difference(expected, effective, "provenance.selectors")
    if difference is not None:
        # NODA deliberately requests the retained 6CL selector but supersedes
        # its ordinary-native route.  This is the only preregistered
        # expected/effective selector mismatch; every key and typed value is
        # bound here before the direct-record validator checks the rest of g5.
        _require(
            provenance["record_id"]
            == PHASENODA_DIRECT_CANDIDATE_RECORD_ID
            and expected == PHASENODA_EXPECTED_SELECTOR_BUNDLE
            and effective == PHASENODA_EFFECTIVE_SELECTOR_BUNDLE
            and PHASEPBA_DIRECT_SELECTOR not in expected
            and PHASEPBA_DIRECT_SELECTOR not in effective,
            source,
            (
                "selector receipt mismatch outside the exact NODA 6CL "
                f"supersession: {difference}"
            ),
        )

    artifacts = _mapping(provenance["artifacts_sha256"], source, "provenance.artifacts_sha256")
    missing = sorted(REQUIRED_ARTIFACT_HASHES - frozenset(artifacts))
    _require(not missing, source, f"provenance.artifacts_sha256 is missing: {', '.join(missing)}")
    _require(artifacts, source, "provenance.artifacts_sha256 must not be empty")
    for name, digest in artifacts.items():
        _nonempty_string(name, source, "provenance.artifacts_sha256 key")
        _sha256(digest, source, f"provenance.artifacts_sha256.{name}")
    _require(
        artifacts["save_special_before"] == artifacts["save_special_after"],
        source,
        "Special save artifact changed",
    )
    _require(
        artifacts["save_template_before"] == artifacts["save_template_after"],
        source,
        "authoritative save template artifact changed",
    )


def _validate_lineage(root: Mapping[str, Any], source: pathlib.Path) -> None:
    lineage = _exact_keys(root["lineage"], LINEAGE_KEYS, source, "lineage")
    generation = _uint(lineage["generation"], source, "lineage.generation")
    parent = lineage["parent_record_id"]
    if generation == 0:
        _require(
            parent is None and lineage["original_role"] == "production",
            source,
            "generation zero must be parentless production",
        )
    else:
        _nonempty_string(parent, source, "lineage.parent_record_id")
        _require(
            lineage["original_role"] == "candidate",
            source,
            "nonzero generation must be candidate",
        )


def _validate_anchor(
    value: Any,
    source: pathlib.Path,
    path: str,
    anchor_name: str,
) -> Mapping[str, Any]:
    anchor = _exact_keys(value, ANCHOR_KEYS, source, path)
    receipt = _uint(anchor["receipt_uptime_raw_ns"], source, f"{path}.receipt_uptime_raw_ns")
    _require(receipt > 0, source, f"{path}.receipt_uptime_raw_ns must be positive")
    before = _uint(anchor["endpoint_before"], source, f"{path}.endpoint_before")
    after = _uint(anchor["endpoint_after"], source, f"{path}.endpoint_after")
    _require(before == after, source, f"{path} endpoint reads raced")
    _require(before > CHECKPOINT_PRESENT, source, f"{path} L must follow p2300")
    _require(before % WINDOW_SIZE == 0, source, f"{path} L must be a p120 endpoint")
    anchored = _uint(anchor["anchor_endpoint"], source, f"{path}.anchor_endpoint")
    measured = _uint(anchor["measured_endpoint"], source, f"{path}.measured_endpoint")
    _require(anchored == before + WINDOW_SIZE, source, f"{anchor_name} must equal L+120")
    _require(measured == anchored + WINDOW_SIZE, source, f"{path} must measure {anchor_name}+120")
    _positive_decimal(anchor["avg_interval_ms"], source, f"{path}.avg_interval_ms")
    return anchor


def _validate_measurements(root: Mapping[str, Any], source: pathlib.Path) -> None:
    measurements = _exact_keys(root["measurements"], MEASUREMENTS_KEYS, source, "measurements")
    loading = _exact_keys(measurements["loading"], LOADING_KEYS, source, "measurements.loading")
    launch = _uint(
        loading["launch_uptime_raw_ns"], source, "measurements.loading.launch_uptime_raw_ns"
    )
    observed = _uint(
        loading["checkpoint_observed_uptime_raw_ns"],
        source,
        "measurements.loading.checkpoint_observed_uptime_raw_ns",
    )
    wall = _uint(loading["wall_ns"], source, "measurements.loading.wall_ns")
    _require(launch > 0, source, "loading launch uptime must be positive")
    _require(observed > launch, source, "p2300 observation uptime must follow launch")
    _require(wall == observed - launch, source, "loading wall_ns does not equal observed-launch")

    checkpoint = _exact_keys(
        loading["checkpoint"], CHECKPOINT_KEYS, source, "measurements.loading.checkpoint"
    )
    checkpoint_present = _uint(
        checkpoint["present"], source, "measurements.loading.checkpoint.present"
    )
    checkpoint_target = _uint(
        checkpoint["target"], source, "measurements.loading.checkpoint.target"
    )
    checkpoint_terminal = _uint(
        checkpoint["terminal"], source, "measurements.loading.checkpoint.terminal"
    )
    checkpoint_count = _uint(
        checkpoint["count"], source, "measurements.loading.checkpoint.count"
    )
    cumulative_intervals = _uint(
        checkpoint["cumulative_intervals"],
        source,
        "measurements.loading.checkpoint.cumulative_intervals",
    )
    partial_intervals = _uint(
        checkpoint["partial_intervals"],
        source,
        "measurements.loading.checkpoint.partial_intervals",
    )
    _require(checkpoint_present == CHECKPOINT_PRESENT, source, "checkpoint present must be 2300")
    _require(checkpoint_target == CHECKPOINT_PRESENT, source, "checkpoint target must be 2300")
    _require(checkpoint_terminal == 0, source, "checkpoint must be non-terminal")
    _require(checkpoint_count == 1, source, "checkpoint count must be exact-once")
    _require(
        cumulative_intervals == CHECKPOINT_PRESENT - 1,
        source,
        "checkpoint cumulative_intervals must be 2299",
    )
    _require(
        partial_intervals == CHECKPOINT_PRESENT % WINDOW_SIZE,
        source,
        "checkpoint partial_intervals must be 20",
    )
    cumulative = _positive_decimal(
        checkpoint["cumulative_ms"], source, "measurements.loading.checkpoint.cumulative_ms"
    )
    partial = _positive_decimal(
        checkpoint["partial_ms"], source, "measurements.loading.checkpoint.partial_ms"
    )
    _require(partial <= cumulative, source, "checkpoint partial_ms exceeds cumulative_ms")

    fort = _validate_anchor(measurements["fort_mode"], source, "measurements.fort_mode", "m0")
    in_game = _validate_anchor(measurements["in_game"], source, "measurements.in_game", "w0")
    fort_receipt = fort["receipt_uptime_raw_ns"]
    game_receipt = in_game["receipt_uptime_raw_ns"]
    _require(fort_receipt > observed, source, "Fort receipt must follow p2300 observation")
    _require(game_receipt > fort_receipt, source, "in-game receipt must follow Fort receipt")
    _require(
        in_game["endpoint_before"] > fort["measured_endpoint"],
        source,
        "in-game anchor must follow the measured Fort window",
    )

    supporting = _exact_keys(
        measurements["supporting"], SUPPORTING_KEYS, source, "measurements.supporting"
    )
    process_times = _exact_keys(
        supporting["process_time_seconds"],
        PROCESS_TIME_KEYS,
        source,
        "measurements.supporting.process_time_seconds",
    )
    for name in ("real", "user", "sys"):
        parser = _positive_decimal if name == "real" else _nonnegative_decimal
        parser(
            process_times[name],
            source,
            f"measurements.supporting.process_time_seconds.{name}",
        )


def _validate_lifecycle(root: Mapping[str, Any], source: pathlib.Path) -> None:
    lifecycle = _exact_keys(root["lifecycle"], LIFECYCLE_KEYS, source, "lifecycle")
    _require(lifecycle["status"] == "clean", source, "lifecycle.status must be clean")
    _require(lifecycle["exit_status_bytes"] == "0\n", source, "exit status must be byte-exact 0\\n")
    for key in ("receipt_count", "checkpoint_count", "sdl_quit_count"):
        _require(
            _uint(lifecycle[key], source, f"lifecycle.{key}") == 1,
            source,
            f"lifecycle.{key} must be exact-once",
        )
    for key in (
        "residual_process_count",
        "fatal_count",
        "assert_count",
        "dynarec_unhealthy_count",
        "descriptor_mismatch_count",
        "device_lost_count",
    ):
        _require(
            _uint(lifecycle[key], source, f"lifecycle.{key}") == 0,
            source,
            f"lifecycle.{key} must be zero",
        )
    _require(
        _bool(lifecycle["glitch_observed"], source, "lifecycle.glitch_observed")
        is False,
        source,
        "glitch observed",
    )
    for key in (
        "save_special_unchanged",
        "save_template_unchanged",
        "sterile_environment_confirmed",
        "fresh_save_confirmed",
        "frame_trace_enabled",
        "present_limit_absent",
        "manual_input_only",
        "no_input_before_p2300",
        "fort_window_input_free",
        "in_game_window_input_free",
        "fort_mode_oracle_confirmed",
    ):
        _require(
            _bool(lifecycle[key], source, f"lifecycle.{key}") is True,
            source,
            f"lifecycle.{key} must be true",
        )

    oracle = _exact_keys(
        lifecycle["in_game_oracle"], IN_GAME_ORACLE_KEYS, source, "lifecycle.in_game_oracle"
    )
    for key in ("objective_visible", "terrain_island_visible", "hud_intact", "timer_advanced"):
        _require(
            _bool(oracle[key], source, f"lifecycle.in_game_oracle.{key}") is True,
            source,
            f"in-game oracle {key} failed",
        )
    _require(
        _uint(oracle["boggy_hp"], source, "lifecycle.in_game_oracle.boggy_hp") == 100,
        source,
        "in-game oracle requires Boggy at 100 HP",
    )

    record_id = _mapping(root["provenance"], source, "provenance")[
        "record_id"
    ]
    marker_keys = (
        EXACT_LINE_CANDIDATE_MARKER_KEYS
        if record_id
        in {
            PHASEFCFOS_DIRECT_CANDIDATE_RECORD_ID,
            PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
            PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
            PHASENODA_DIRECT_CANDIDATE_RECORD_ID,
        }
        else CANDIDATE_MARKER_KEYS
    )
    marker = _exact_keys(
        lifecycle["candidate_marker"], marker_keys, source,
        "lifecycle.candidate_marker"
    )
    if record_id == PHASEFCFOS_DIRECT_CANDIDATE_RECORD_ID:
        _require(
            marker["line"] == PHASEFCFOS_DIRECT_CANDIDATE_MARKER_LINE,
            source,
            "FCFOS candidate marker is not the exact sealed line",
        )
    elif record_id == PHASEALC_DIRECT_CANDIDATE_RECORD_ID:
        _require(
            marker["line"] == PHASEALC_DIRECT_CANDIDATE_MARKER_LINE,
            source,
            "ALC candidate marker is not the exact sealed line",
        )
    elif record_id == PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID:
        _require(
            marker["line"] == PHASEMSORE_DIRECT_CANDIDATE_MARKER_LINE,
            source,
            "MSORE candidate marker is not the exact sealed line",
        )
    elif record_id == PHASENODA_DIRECT_CANDIDATE_RECORD_ID:
        _require(
            marker["line"] == PHASENODA_DIRECT_CANDIDATE_MARKER_LINE,
            source,
            "NODA candidate marker is not the exact sealed line",
        )
    required = _bool(marker["required"], source, "lifecycle.candidate_marker.required")
    expected = _uint(marker["expected_count"], source, "lifecycle.candidate_marker.expected_count")
    observed = _uint(marker["observed_count"], source, "lifecycle.candidate_marker.observed_count")
    coverage_required = _bool(
        marker["coverage_required"], source, "lifecycle.candidate_marker.coverage_required"
    )
    coverage_positive = _bool(
        marker["coverage_positive"], source, "lifecycle.candidate_marker.coverage_positive"
    )
    if required:
        _require(
            expected > 0,
            source,
            "required candidate marker must have positive expected_count",
        )
        _require(observed == expected, source, "candidate marker count mismatch")
        _require(marker["status"] == "clean", source, "candidate marker status must be clean")
    else:
        _require(
            expected == 0 and observed == 0,
            source,
            "non-required candidate marker counts must be zero",
        )
        _require(
            marker["status"] == "not_applicable",
            source,
            "non-required marker status must be not_applicable",
        )
    if coverage_required:
        _require(required, source, "coverage cannot be required without a candidate marker")
        _require(coverage_positive, source, "required candidate coverage is zero")
    elif not required:
        _require(
            not coverage_positive,
            source,
            "non-required marker cannot claim positive coverage",
        )


def load_manifest(
    path: pathlib.Path,
    *,
    _projection_contract: Optional[Mapping[str, Any]] = None,
    _projection_contracts: Optional[Sequence[Mapping[str, Any]]] = None,
) -> RunManifest:
    _require(path.is_file(), path, "manifest is not a regular file")
    try:
        encoded = path.read_bytes()
        _require(
            len(encoded) <= MANIFEST_MAX_BYTES,
            path,
            f"manifest exceeds {MANIFEST_MAX_BYTES} bytes",
        )
        text = encoded.decode("utf-8", errors="strict")
        raw = json.loads(
            text,
            parse_float=decimal.Decimal,
            parse_int=int,
            parse_constant=_reject_json_constant,
            object_pairs_hook=_object_without_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        raise ReportError(f"{path}: cannot read strict JSON manifest: {error}") from error

    root = _exact_keys(raw, TOP_LEVEL_KEYS, path, "root")
    _validate_schema(root, path)
    _validate_protocol(root, path)
    _validate_comparability(root, path)
    _validate_provenance(root, path)
    _validate_lineage(root, path)
    _validate_configuration_projection(
        root,
        path,
        contract=_projection_contract,
        contracts=_projection_contracts,
    )
    _validate_measurements(root, path)
    _validate_lifecycle(root, path)
    return RunManifest(
        path=path.resolve(),
        content_sha256=hashlib.sha256(encoded).hexdigest(),
        raw=root,
    )


def _validate_pair(baseline: RunManifest, candidate: RunManifest) -> None:
    _require(
        baseline.path != candidate.path,
        "comparison",
        "baseline and candidate paths must differ",
    )
    protocol_difference = _first_difference(baseline.protocol, candidate.protocol, "protocol")
    _require(
        protocol_difference is None,
        "comparison",
        f"protocol mismatch (new epoch required, no implicit baseline run): {protocol_difference}",
    )
    comparability_difference = _first_difference(
        baseline.comparability, candidate.comparability, "comparability"
    )
    _require(
        comparability_difference is None,
        "comparison",
        f"comparability mismatch (comparison blocked): {comparability_difference}",
    )

    baseline_record = baseline.provenance["record_id"]
    candidate_record = candidate.provenance["record_id"]
    _require(baseline_record != candidate_record, "comparison", "record_id must be unique")
    _require(
        baseline.provenance["run_receipt"] != candidate.provenance["run_receipt"],
        "comparison",
        "run_receipt must be unique",
    )
    if baseline.lineage["generation"] == 0:
        _require(
            baseline.provenance["binary_sha256"]
            == candidate.provenance["binary_sha256"],
            "comparison",
            "initial B/C comparison requires the same Mach-O SHA-256",
        )
    _require(
        candidate.lineage["original_role"] == "candidate",
        "comparison",
        "candidate original_role must be candidate",
    )
    _require(
        candidate.lifecycle["candidate_marker"]["required"] is True,
        "comparison",
        "candidate manifest must require its candidate-specific marker",
    )
    _require(
        candidate.lineage["generation"] == baseline.lineage["generation"] + 1,
        "comparison",
        "candidate generation must be baseline generation + 1",
    )
    _require(
        candidate.lineage["parent_record_id"] == baseline_record,
        "comparison",
        "candidate parent_record_id must name the baseline record",
    )


def _divide(numerator: decimal.Decimal, denominator: decimal.Decimal) -> decimal.Decimal:
    _require(denominator > ZERO, "calculation", "division denominator must be positive")
    with decimal.localcontext() as context:
        context.prec = 50
        return numerator / denominator


def _decimal_text(value: decimal.Decimal, places: int) -> str:
    quantum = decimal.Decimal(1).scaleb(-places)
    integer_digits = max(1, value.copy_abs().adjusted() + 1) if value != ZERO else 1
    with decimal.localcontext() as context:
        context.prec = max(50, integer_digits + places + 4)
        rounded = value.quantize(quantum, rounding=decimal.ROUND_HALF_UP)
    return format(rounded, "f")


def _numeric_text(value: Any) -> str:
    if type(value) is int:
        return str(value)
    return format(decimal.Decimal(value), "f")


def _comparison(baseline: Any, candidate: Any, places: int) -> Mapping[str, Any]:
    baseline_decimal = decimal.Decimal(baseline)
    candidate_decimal = decimal.Decimal(candidate)
    ratio = _divide(candidate_decimal, baseline_decimal)
    delta = candidate_decimal - baseline_decimal
    return {
        "baseline": _numeric_text(baseline),
        "candidate": _numeric_text(candidate),
        "candidate_minus_baseline": _decimal_text(delta, places),
        "candidate_over_baseline": _decimal_text(ratio, 9),
        "candidate_delta_percent": _decimal_text((ratio - decimal.Decimal(1)) * ONE_HUNDRED, 6),
        "baseline_over_candidate": _decimal_text(_divide(baseline_decimal, candidate_decimal), 9),
    }


def _fps(interval_ms: Any) -> decimal.Decimal:
    return _divide(ONE_THOUSAND, decimal.Decimal(interval_ms))


def _run_summary(run: RunManifest) -> Mapping[str, Any]:
    measurements = run.measurements
    loading = measurements["loading"]
    checkpoint = loading["checkpoint"]
    fort = measurements["fort_mode"]
    in_game = measurements["in_game"]
    process_times = measurements["supporting"]["process_time_seconds"]
    return {
        "manifest": str(run.path),
        "manifest_sha256": run.content_sha256,
        "record_id": run.provenance["record_id"],
        "source_revision": run.provenance["source_revision"],
        "binary_sha256": run.provenance["binary_sha256"],
        "run_receipt": run.provenance["run_receipt"],
        "provenance": run.provenance,
        "generation": run.lineage["generation"],
        "parent_record_id": run.lineage["parent_record_id"],
        "original_role": run.lineage["original_role"],
        "loading": {
            "launch_uptime_raw_ns": loading["launch_uptime_raw_ns"],
            "checkpoint_observed_uptime_raw_ns": loading["checkpoint_observed_uptime_raw_ns"],
            "wall_ns": loading["wall_ns"],
            "wall_seconds": _decimal_text(
                _divide(decimal.Decimal(loading["wall_ns"]), ONE_BILLION), 9
            ),
            "checkpoint_cumulative_ms": _numeric_text(checkpoint["cumulative_ms"]),
        },
        "fort_mode": {
            "receipt_uptime_raw_ns": fort["receipt_uptime_raw_ns"],
            "L": fort["endpoint_before"],
            "m0": fort["anchor_endpoint"],
            "measured_endpoint": fort["measured_endpoint"],
            "avg_interval_ms": _numeric_text(fort["avg_interval_ms"]),
            "fps": _decimal_text(_fps(fort["avg_interval_ms"]), 6),
        },
        "in_game": {
            "receipt_uptime_raw_ns": in_game["receipt_uptime_raw_ns"],
            "L": in_game["endpoint_before"],
            "w0": in_game["anchor_endpoint"],
            "measured_endpoint": in_game["measured_endpoint"],
            "avg_interval_ms": _numeric_text(in_game["avg_interval_ms"]),
            "fps": _decimal_text(_fps(in_game["avg_interval_ms"]), 6),
        },
        "process_time_seconds_descriptive": {
            name: _numeric_text(process_times[name]) for name in ("real", "user", "sys")
        },
        "lifecycle": "clean",
    }


def build_report(baseline: RunManifest, candidate: RunManifest) -> Mapping[str, Any]:
    _validate_pair(baseline, candidate)
    baseline_measurements = baseline.measurements
    candidate_measurements = candidate.measurements
    baseline_loading = baseline_measurements["loading"]
    candidate_loading = candidate_measurements["loading"]
    baseline_checkpoint = baseline_loading["checkpoint"]
    candidate_checkpoint = candidate_loading["checkpoint"]
    baseline_fort = baseline_measurements["fort_mode"]
    candidate_fort = candidate_measurements["fort_mode"]
    baseline_game = baseline_measurements["in_game"]
    candidate_game = candidate_measurements["in_game"]
    baseline_process = baseline_measurements["supporting"]["process_time_seconds"]
    candidate_process = candidate_measurements["supporting"]["process_time_seconds"]

    return {
        "schema": {"name": REPORT_SCHEMA_NAME, "version": REPORT_SCHEMA_VERSION},
        "valid": True,
        "offline": True,
        "protocol": baseline.protocol,
        "protocol_equal": True,
        "protocol_sha256": _canonical_sha256(baseline.protocol),
        "comparability": baseline.comparability,
        "comparability_equal": True,
        "comparability_sha256": _canonical_sha256(baseline.comparability),
        "provenance_equal_required": False,
        "promotion": {
            "automatic": False,
            "performed": False,
            "message": "comparison only; baseline promotion is a separate explicit operation",
        },
        "lineage": {
            "baseline_generation": baseline.lineage["generation"],
            "candidate_generation": candidate.lineage["generation"],
            "candidate_parent_record_id": candidate.lineage["parent_record_id"],
        },
        "records": {
            "baseline": _run_summary(baseline),
            "candidate": _run_summary(candidate),
        },
        "comparison": {
            "loading_wall_ns_lower_is_better": _comparison(
                baseline_loading["wall_ns"], candidate_loading["wall_ns"], 0
            ),
            "checkpoint_cumulative_ms_lower_is_better": _comparison(
                baseline_checkpoint["cumulative_ms"], candidate_checkpoint["cumulative_ms"], 3
            ),
            "fort_mode": {
                "avg_interval_ms_lower_is_better": _comparison(
                    baseline_fort["avg_interval_ms"], candidate_fort["avg_interval_ms"], 3
                ),
                "fps_higher_is_better": _comparison(
                    _fps(baseline_fort["avg_interval_ms"]),
                    _fps(candidate_fort["avg_interval_ms"]),
                    6,
                ),
            },
            "in_game": {
                "avg_interval_ms_lower_is_better": _comparison(
                    baseline_game["avg_interval_ms"], candidate_game["avg_interval_ms"], 3
                ),
                "fps_higher_is_better": _comparison(
                    _fps(baseline_game["avg_interval_ms"]),
                    _fps(candidate_game["avg_interval_ms"]),
                    6,
                ),
            },
            "process_time_seconds_descriptive": {
                name: _comparison(baseline_process[name], candidate_process[name], 3)
                for name in ("real", "user", "sys")
            },
        },
    }


def _render_table(report: Mapping[str, Any]) -> str:
    records = report["records"]
    comparison = report["comparison"]
    baseline = records["baseline"]
    candidate = records["candidate"]

    lines = [
        "PERSISTENT PERFORMANCE REPORT: PASS",
        (
            f"protocol={report['protocol']['protocol_id']} "
            f"comparability={report['comparability_sha256']}"
        ),
        f"baseline generation={baseline['generation']} record={baseline['record_id']}",
        (
            f"candidate generation={candidate['generation']} "
            f"parent={candidate['parent_record_id']} record={candidate['record_id']}"
        ),
        "promotion=NOT_PERFORMED (separate explicit operation)",
        "",
        "PRIMARY METRICS",
        (
            "metric                         baseline          candidate        "
            "delta C/B       ratio C/B"
        ),
    ]

    metric_rows = (
        ("loading wall ns", comparison["loading_wall_ns_lower_is_better"]),
        ("p2300 cumulative ms", comparison["checkpoint_cumulative_ms_lower_is_better"]),
        ("Fort avg interval ms", comparison["fort_mode"]["avg_interval_ms_lower_is_better"]),
        ("Fort FPS", comparison["fort_mode"]["fps_higher_is_better"]),
        ("in-game avg interval ms", comparison["in_game"]["avg_interval_ms_lower_is_better"]),
        ("in-game FPS", comparison["in_game"]["fps_higher_is_better"]),
    )
    for label, values in metric_rows:
        lines.append(
            f"{label:<29} {values['baseline']:>16} {values['candidate']:>18} "
            f"{values['candidate_delta_percent']:>13}% {values['candidate_over_baseline']:>15}"
        )

    lines.extend(
        [
            "",
            "ANNOUNCEMENT WINDOWS",
            "arm          receipt raw ns       L     anchor    measured       avg ms          FPS",
        ]
    )
    for label, record in (("baseline", baseline), ("candidate", candidate)):
        for scene, anchor_name in (("fort_mode", "m0"), ("in_game", "w0")):
            values = record[scene]
            lines.append(
                f"{label + ' ' + scene:<20} {values['receipt_uptime_raw_ns']:>16} "
                f"p{values['L']:<5} p{values[anchor_name]:<7} p{values['measured_endpoint']:<8} "
                f"{values['avg_interval_ms']:>10} {values['fps']:>12}"
            )

    lines.extend(
        [
            "",
            "PROCESS TIME (DESCRIPTIVE ONLY)",
            "metric                         baseline          candidate        delta C/B",
        ]
    )
    for name in ("real", "user", "sys"):
        values = comparison["process_time_seconds_descriptive"][name]
        lines.append(
            f"{name + ' seconds':<29} {values['baseline']:>16} {values['candidate']:>18} "
            f"{values['candidate_delta_percent']:>13}%"
        )
    return "\n".join(lines)


def manifest_schema() -> Mapping[str, Any]:
    """Return the explicit version-1 JSON Schema for interchange tooling."""

    sha = {"type": "string", "pattern": "^[0-9a-f]{64}$"}
    uint = {"type": "integer", "minimum": 0, "maximum": UINT64_MAX}
    positive_number = {"type": "number", "exclusiveMinimum": 0}

    def object_schema(
        properties: Mapping[str, Any], *, additional: Any = False
    ) -> Mapping[str, Any]:
        return {
            "type": "object",
            "required": list(properties),
            "properties": properties,
            "additionalProperties": additional,
        }

    candidate_marker_properties = {
        "required": {"type": "boolean"},
        "expected_count": uint,
        "observed_count": uint,
        "status": {"enum": ["clean", "not_applicable"]},
        "coverage_required": {"type": "boolean"},
        "coverage_positive": {"type": "boolean"},
    }
    candidate_marker_schema = {
        "oneOf": [
            object_schema(candidate_marker_properties),
            object_schema(
                {
                    **candidate_marker_properties,
                    "line": {"type": "string", "minLength": 1},
                }
            ),
        ]
    }

    anchor = object_schema(
        {
            "receipt_uptime_raw_ns": uint,
            "endpoint_before": uint,
            "endpoint_after": uint,
            "anchor_endpoint": uint,
            "measured_endpoint": uint,
            "avg_interval_ms": positive_number,
        }
    )
    lineage = dict(
        object_schema(
            {
                "generation": uint,
                "parent_record_id": {
                    "anyOf": [
                        {"type": "string", "minLength": 1},
                        {"type": "null"},
                    ]
                },
                "original_role": {"enum": ["production", "candidate"]},
            }
        )
    )
    lineage["oneOf"] = [
        {
            "properties": {
                "generation": {"const": 0},
                "parent_record_id": {"type": "null"},
                "original_role": {"const": "production"},
            }
        },
        {
            "properties": {
                "generation": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": UINT64_MAX,
                },
                "parent_record_id": {"type": "string", "minLength": 1},
                "original_role": {"const": "candidate"},
            }
        },
    ]
    return {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": f"urn:{MANIFEST_SCHEMA_NAME}:v{MANIFEST_SCHEMA_VERSION}",
        "title": "Lumen persistent performance run manifest",
        **object_schema(
            {
                "schema": object_schema(
                    {
                        "name": {"const": MANIFEST_SCHEMA_NAME},
                        "version": {"const": MANIFEST_SCHEMA_VERSION},
                    }
                ),
                "protocol": object_schema(
                    {
                        "protocol_id": {"type": "string", "minLength": 1},
                        "protocol_revision": {"type": "integer", "minimum": 1},
                        "checkpoint_present": {"const": CHECKPOINT_PRESENT},
                        "window_size": {"const": WINDOW_SIZE},
                        "loading_metric": {"const": PROTOCOL_CONSTANTS["loading_metric"]},
                        "announcement_clock": {"const": PROTOCOL_CONSTANTS["announcement_clock"]},
                        "fort_window_semantics": {
                            "const": PROTOCOL_CONSTANTS["fort_window_semantics"]
                        },
                        "in_game_window_semantics": {
                            "const": PROTOCOL_CONSTANTS["in_game_window_semantics"]
                        },
                    }
                ),
                "comparability": object_schema(
                    {
                        "host": object_schema(
                            {
                                "host_id": {"type": "string", "minLength": 1},
                                "model": {"type": "string", "minLength": 1},
                                "architecture": {"const": "arm64"},
                            }
                        ),
                        "os": object_schema(
                            {
                                "name": {"const": "macOS"},
                                "version": {"type": "string", "minLength": 1},
                                "build": {"type": "string", "minLength": 1},
                                "policy": {"type": "string", "minLength": 1},
                            }
                        ),
                        "power": object_schema(
                            {
                                "source": {"type": "string", "minLength": 1},
                                "mode": {"type": "string", "minLength": 1},
                                "policy": {"type": "string", "minLength": 1},
                            }
                        ),
                        "workload": object_schema(
                            {
                                "rom_sha256": sha,
                                "lua_sha256": sha,
                                "save_template_sha256": sha,
                                "cache_policy": {"type": "string", "minLength": 1},
                                "cache_seed_sha256": {"anyOf": [sha, {"type": "null"}]},
                            }
                        ),
                        "runtime": object_schema(
                            {
                                "sterile_environment_sha256": sha,
                                "backend": {"type": "string", "minLength": 1},
                                "configuration_sha256": sha,
                            }
                        ),
                        "scenes": object_schema(
                            {
                                "fort_mode_oracle": {"type": "string", "minLength": 1},
                                "in_game_oracle": {"type": "string", "minLength": 1},
                            }
                        ),
                        "tool_schemas": object_schema(
                            {
                                "harness": {"type": "string", "minLength": 1},
                                "announcement_sidecar": {"type": "string", "minLength": 1},
                                "report": {"const": REPORT_TOOL_SCHEMA},
                            }
                        ),
                    }
                ),
                "provenance": object_schema(
                    {
                        "record_id": {"type": "string", "minLength": 1},
                        "source_revision": {
                            "type": "string",
                            "pattern": "^(?:[0-9a-f]{40}|[0-9a-f]{64})$",
                        },
                        "binary_sha256": sha,
                        "diff_sha256": sha,
                        "harness_sha256": sha,
                        "announcement_sidecar_sha256": sha,
                        "report_tool_sha256": sha,
                        "run_receipt": {"type": "string", "minLength": 1},
                        "selector_receipt_sha256": sha,
                        "selectors": object_schema(
                            {
                                "expected": {"type": "object", "minProperties": 1},
                                "effective": {"type": "object"},
                            }
                        ),
                        "artifacts_sha256": {
                            "type": "object",
                            "required": sorted(REQUIRED_ARTIFACT_HASHES),
                            "minProperties": len(REQUIRED_ARTIFACT_HASHES),
                            "additionalProperties": sha,
                        },
                    }
                ),
                "lineage": lineage,
                "measurements": object_schema(
                    {
                        "loading": object_schema(
                            {
                                "launch_uptime_raw_ns": uint,
                                "checkpoint_observed_uptime_raw_ns": uint,
                                "wall_ns": uint,
                                "checkpoint": object_schema(
                                    {
                                        "present": {"const": CHECKPOINT_PRESENT},
                                        "target": {"const": CHECKPOINT_PRESENT},
                                        "cumulative_ms": positive_number,
                                        "cumulative_intervals": {"const": CHECKPOINT_PRESENT - 1},
                                        "partial_ms": positive_number,
                                        "partial_intervals": {
                                            "const": CHECKPOINT_PRESENT % WINDOW_SIZE
                                        },
                                        "terminal": {"const": 0},
                                        "count": {"const": 1},
                                    }
                                ),
                            }
                        ),
                        "fort_mode": anchor,
                        "in_game": anchor,
                        "supporting": object_schema(
                            {
                                "process_time_seconds": object_schema(
                                    {
                                        "real": positive_number,
                                        "user": {"type": "number", "minimum": 0},
                                        "sys": {"type": "number", "minimum": 0},
                                    }
                                )
                            }
                        ),
                    }
                ),
                "lifecycle": object_schema(
                    {
                        "status": {"const": "clean"},
                        "exit_status_bytes": {"const": "0\n"},
                        "receipt_count": {"const": 1},
                        "checkpoint_count": {"const": 1},
                        "sdl_quit_count": {"const": 1},
                        "residual_process_count": {"const": 0},
                        "fatal_count": {"const": 0},
                        "assert_count": {"const": 0},
                        "dynarec_unhealthy_count": {"const": 0},
                        "descriptor_mismatch_count": {"const": 0},
                        "device_lost_count": {"const": 0},
                        "glitch_observed": {"const": False},
                        "save_special_unchanged": {"const": True},
                        "save_template_unchanged": {"const": True},
                        "sterile_environment_confirmed": {"const": True},
                        "fresh_save_confirmed": {"const": True},
                        "frame_trace_enabled": {"const": True},
                        "present_limit_absent": {"const": True},
                        "manual_input_only": {"const": True},
                        "no_input_before_p2300": {"const": True},
                        "fort_window_input_free": {"const": True},
                        "in_game_window_input_free": {"const": True},
                        "fort_mode_oracle_confirmed": {"const": True},
                        "in_game_oracle": object_schema(
                            {
                                "objective_visible": {"const": True},
                                "terrain_island_visible": {"const": True},
                                "boggy_hp": {"const": 100},
                                "hud_intact": {"const": True},
                                "timer_advanced": {"const": True},
                            }
                        ),
                        "candidate_marker": candidate_marker_schema,
                    }
                ),
            }
        ),
    }


def _serialize_report(report: Mapping[str, Any]) -> bytes:
    try:
        text = json.dumps(
            report,
            indent=2,
            sort_keys=True,
            ensure_ascii=False,
            allow_nan=False,
        )
    except (TypeError, ValueError) as error:
        raise ReportError(f"report is not strict JSON: {error}") from error
    return (text + "\n").encode("utf-8")


def _publish_report_exclusive(output: pathlib.Path, payload: bytes) -> None:
    _require(bool(output.name), output, "output must name a file")
    parent = output.parent if str(output.parent) else pathlib.Path(".")
    directory_flags = os.O_RDONLY
    directory_flags |= getattr(os, "O_CLOEXEC", 0)
    directory_flags |= getattr(os, "O_DIRECTORY", 0)
    try:
        directory_fd = os.open(str(parent), directory_flags)
    except OSError as error:
        raise ReportError(f"{parent}: cannot open output directory: {error}") from error

    file_fd: Optional[int] = None
    created = False
    cleanup_error: Optional[OSError] = None
    try:
        file_flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        file_flags |= getattr(os, "O_CLOEXEC", 0)
        file_flags |= getattr(os, "O_NOFOLLOW", 0)
        file_fd = os.open(output.name, file_flags, 0o600, dir_fd=directory_fd)
        created = True
        offset = 0
        while offset < len(payload):
            written = os.write(file_fd, payload[offset:])
            if written == 0:
                raise OSError(errno.EIO, "zero-length write")
            offset += written
        os.fsync(file_fd)
        os.fchmod(file_fd, 0o444)
        os.fsync(file_fd)
        os.close(file_fd)
        file_fd = None
        os.fsync(directory_fd)
    except OSError as error:
        if file_fd is not None:
            try:
                os.close(file_fd)
            except OSError:
                pass
            file_fd = None
        if created:
            try:
                os.unlink(output.name, dir_fd=directory_fd)
                os.fsync(directory_fd)
            except OSError as cleanup:
                cleanup_error = cleanup
        detail = f"{output}: exclusive immutable publication failed: {error}"
        if cleanup_error is not None:
            detail += f"; partial-file cleanup failed: {cleanup_error}"
        raise ReportError(detail) from error
    finally:
        try:
            os.close(directory_fd)
        except OSError:
            pass


def _fixture_manifest(
    *,
    record_id: str,
    receipt: str,
    generation: int,
    parent: Optional[str],
    role: str,
    wall_ns: int,
    cumulative_ms: int,
    fort_interval_ms: int,
    game_interval_ms: int,
    revision_digit: str,
    selector_enabled: bool,
) -> Mapping[str, Any]:
    digest = "a" * 64
    special = "b" * 64
    template = "c" * 64
    launch = 1_000_000_000
    observed = launch + wall_ns
    artifacts = {name: digest for name in REQUIRED_ARTIFACT_HASHES}
    artifacts["save_special_before"] = special
    artifacts["save_special_after"] = special
    artifacts["save_template_before"] = template
    artifacts["save_template_after"] = template
    return {
        "schema": {"name": MANIFEST_SCHEMA_NAME, "version": MANIFEST_SCHEMA_VERSION},
        "protocol": {
            "protocol_id": "mac-m1-announced-v1",
            "protocol_revision": 1,
            **PROTOCOL_CONSTANTS,
        },
        "comparability": {
            "host": {"host_id": "fixture-m1", "model": "Mac mini M1", "architecture": "arm64"},
            "os": {"name": "macOS", "version": "fixture", "build": "fixture", "policy": "frozen"},
            "power": {"source": "AC", "mode": "normal", "policy": "AC normal"},
            "workload": {
                "rom_sha256": digest,
                "lua_sha256": digest,
                "save_template_sha256": template,
                "cache_policy": "fresh frozen seed",
                "cache_seed_sha256": digest,
            },
            "runtime": {
                "sterile_environment_sha256": digest,
                "backend": "NativeMetal",
                "configuration_sha256": digest,
            },
            "scenes": {
                "fort_mode_oracle": "Fort Mode stable panel",
                "in_game_oracle": "Training Navigation objective terrain Boggy 100 HP HUD timer",
            },
            "tool_schemas": {
                "harness": "lumen.persistent-performance-harness/1",
                "announcement_sidecar": "lumen.persistent-performance-anchor/1",
                "report": REPORT_TOOL_SCHEMA,
            },
        },
        "provenance": {
            "record_id": record_id,
            "source_revision": revision_digit * 40,
            "binary_sha256": digest,
            "diff_sha256": digest,
            "harness_sha256": digest,
            "announcement_sidecar_sha256": digest,
            "report_tool_sha256": digest,
            "run_receipt": receipt,
            "selector_receipt_sha256": digest,
            "selectors": {
                "expected": {"fixture_candidate_enabled": selector_enabled},
                "effective": {"fixture_candidate_enabled": selector_enabled},
            },
            "artifacts_sha256": artifacts,
        },
        "lineage": {
            "generation": generation,
            "parent_record_id": parent,
            "original_role": role,
        },
        "measurements": {
            "loading": {
                "launch_uptime_raw_ns": launch,
                "checkpoint_observed_uptime_raw_ns": observed,
                "wall_ns": wall_ns,
                "checkpoint": {
                    "present": 2300,
                    "target": 2300,
                    "cumulative_ms": cumulative_ms,
                    "cumulative_intervals": 2299,
                    "partial_ms": 2000,
                    "partial_intervals": 20,
                    "terminal": 0,
                    "count": 1,
                },
            },
            "fort_mode": {
                "receipt_uptime_raw_ns": observed + 1_000_000,
                "endpoint_before": 2400,
                "endpoint_after": 2400,
                "anchor_endpoint": 2520,
                "measured_endpoint": 2640,
                "avg_interval_ms": fort_interval_ms,
            },
            "in_game": {
                "receipt_uptime_raw_ns": observed + 2_000_000,
                "endpoint_before": 3600,
                "endpoint_after": 3600,
                "anchor_endpoint": 3720,
                "measured_endpoint": 3840,
                "avg_interval_ms": game_interval_ms,
            },
            "supporting": {"process_time_seconds": {"real": 1000, "user": 700, "sys": 100}},
        },
        "lifecycle": {
            "status": "clean",
            "exit_status_bytes": "0\n",
            "receipt_count": 1,
            "checkpoint_count": 1,
            "sdl_quit_count": 1,
            "residual_process_count": 0,
            "fatal_count": 0,
            "assert_count": 0,
            "dynarec_unhealthy_count": 0,
            "descriptor_mismatch_count": 0,
            "device_lost_count": 0,
            "glitch_observed": False,
            "save_special_unchanged": True,
            "save_template_unchanged": True,
            "sterile_environment_confirmed": True,
            "fresh_save_confirmed": True,
            "frame_trace_enabled": True,
            "present_limit_absent": True,
            "manual_input_only": True,
            "no_input_before_p2300": True,
            "fort_window_input_free": True,
            "in_game_window_input_free": True,
            "fort_mode_oracle_confirmed": True,
            "in_game_oracle": {
                "objective_visible": True,
                "terrain_island_visible": True,
                "boggy_hp": 100,
                "hud_intact": True,
                "timer_advanced": True,
            },
            "candidate_marker": {
                "required": role == "candidate",
                "expected_count": 1 if role == "candidate" else 0,
                "observed_count": 1 if role == "candidate" else 0,
                "status": "clean" if role == "candidate" else "not_applicable",
                "coverage_required": role == "candidate",
                "coverage_positive": role == "candidate",
            },
        },
    }


def _write_fixture(path: pathlib.Path, value: Mapping[str, Any]) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _expect_failure(action: Callable[[], object], description: str) -> None:
    try:
        action()
    except ReportError:
        return
    raise ReportError(f"self-test accepted {description}")


def _expect_failure_containing(
    action: Callable[[], object], description: str, expected_message: str
) -> None:
    try:
        action()
    except ReportError as error:
        _require(
            expected_message in str(error),
            "self-test",
            (
                f"{description} failed for the wrong reason: expected "
                f"{expected_message!r} in {str(error)!r}"
            ),
        )
        return
    raise ReportError(f"self-test accepted {description}")


def self_test() -> None:
    with tempfile.TemporaryDirectory(
        prefix="persistent-performance-report-self-test-"
    ) as temporary:
        root = pathlib.Path(temporary)
        baseline_path = root / "baseline.json"
        candidate_path = root / "candidate.json"
        baseline_value = _fixture_manifest(
            record_id="baseline-record",
            receipt="baseline-receipt",
            generation=0,
            parent=None,
            role="production",
            wall_ns=500_000_000_000,
            cumulative_ms=480_000,
            fort_interval_ms=500,
            game_interval_ms=250,
            revision_digit="1",
            selector_enabled=False,
        )
        candidate_value = _fixture_manifest(
            record_id="candidate-record",
            receipt="candidate-receipt",
            generation=1,
            parent="baseline-record",
            role="candidate",
            wall_ns=400_000_000_000,
            cumulative_ms=384_000,
            fort_interval_ms=400,
            game_interval_ms=200,
            revision_digit="2",
            selector_enabled=True,
        )
        _write_fixture(baseline_path, baseline_value)
        _write_fixture(candidate_path, candidate_value)

        baseline = load_manifest(baseline_path)
        candidate = load_manifest(candidate_path)
        report = build_report(baseline, candidate)
        _require(report["valid"] is True, "self-test", "positive report is not valid")
        _require(
            report["provenance_equal_required"] is False
            and baseline.provenance != candidate.provenance,
            "self-test",
            "variable provenance was incorrectly required to match",
        )
        _require(
            report["comparison"]["loading_wall_ns_lower_is_better"]["candidate_delta_percent"]
            == "-20.000000",
            "self-test",
            "loading delta mismatch",
        )
        _require(
            report["comparison"]["fort_mode"]["fps_higher_is_better"]["candidate_delta_percent"]
            == "25.000000",
            "self-test",
            "Fort FPS delta mismatch",
        )
        _require(
            report["promotion"]["performed"] is False
            and "promotion=NOT_PERFORMED" in _render_table(report),
            "self-test",
            "report implies an automatic promotion",
        )
        _require(
            CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": CONFIGURATION_PROJECTION_RECORD_ID,
                "generation": 2,
                "parent_record_id": "6cl-c-stable-negative-g000001",
                "original_role": "candidate",
                "candidate_selector": CONFIGURATION_PROJECTION_SELECTOR,
                "selector_bundle": {
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                    CONFIGURATION_PROJECTION_SELECTOR: True,
                },
                "raw_bytes": 49_567,
                "raw_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
                "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
                "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
                "removed_bytes": 246,
                "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
                "projected_bytes": 49_321,
                "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
            },
            "self-test",
            "production configuration projection constants drifted",
        )
        _require(
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": PHASE6CQ_CONFIGURATION_PROJECTION_RECORD_ID,
                "generation": 3,
                "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
                "original_role": "candidate",
                "candidate_selector": PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR,
                "selector_bundle": {
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                    CONFIGURATION_PROJECTION_SELECTOR: True,
                    PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR: True,
                },
                "raw_bytes": 46_394,
                "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
                "projection_kind": "phase6cq-restore-eight-then-strip-v1",
                "attestation": "CONFIRM_PHASE6CQ_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
                "policy": "phase6cq-cmake-cache-production-v1",
                "mode": "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-phase6cm-phase6co-v1",
                "restore_operation_count": 8,
                "restore_operations_contract_bytes": 4_788,
                "restore_operations_contract_sha256": "ee36e477af8d1218a7b254d078edf9841a4a233597790bba8bcffa0177c3c04c",
                "restored_bytes": 49_567,
                "restored_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
                "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
                "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
                "removed_bytes": 246,
                "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
                "projected_bytes": 49_321,
                "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
                "projection_configured": True,
            }
            and CONFIGURATION_PROJECTION_CONTRACTS
            == (
                CONFIGURATION_PROJECTION_CONTRACT,
                PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT,
                PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT,
                PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT,
                PHASEALC_CONFIGURATION_PROJECTION_CONTRACT,
                PHASEMETAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT,
                PHASENODA_CONFIGURATION_PROJECTION_CONTRACT,
            )
            and len(PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS) == 8
            and len(_phase6cq_restore_operations_payload())
            == PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
            and hashlib.sha256(
                _phase6cq_restore_operations_payload()
            ).hexdigest()
            == PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256,
            "self-test",
            "phase 6CQ production projection contract drifted",
        )
        _require(
            PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": PHASE6CS_CONFIGURATION_PROJECTION_RECORD_ID,
                "generation": 3,
                "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
                "original_role": "candidate",
                "candidate_selector": PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR,
                "selector_bundle": {
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                    CONFIGURATION_PROJECTION_SELECTOR: True,
                    PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR: True,
                },
                "raw_bytes": 46_394,
                "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
                "projection_kind": "phase6cs-restore-eight-then-strip-v1",
                "attestation": "CONFIRM_PHASE6CS_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
                "policy": "phase6cs-cmake-cache-production-v1",
                "mode": "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-phase6cm-phase6co-v1",
                "restore_operation_count": 8,
                "restore_operations_contract_bytes": 4_788,
                "restore_operations_contract_sha256": "15a48324c22c27e8959a0d8706965110199d7ca1d8203f10d94fd823fe6e57f9",
                "restored_bytes": 49_567,
                "restored_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
                "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
                "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
                "removed_bytes": 246,
                "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
                "projected_bytes": 49_321,
                "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
                "projection_configured": True,
            }
            and len(PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS) == 8
            and len(_phase6cs_restore_operations_payload())
            == PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
            and hashlib.sha256(
                _phase6cs_restore_operations_payload()
            ).hexdigest()
            == PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256,
            "self-test",
            "phase 6CS production projection contract drifted",
        )
        _require(
            PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": PHASE6DA_CONFIGURATION_PROJECTION_RECORD_ID,
                "generation": 3,
                "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
                "original_role": "candidate",
                "candidate_selector": PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR,
                "selector_bundle": {
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                    CONFIGURATION_PROJECTION_SELECTOR: True,
                    PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                },
                "raw_bytes": 46_394,
                "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
                "projection_kind": "phase6da-restore-eight-then-strip-v1",
                "attestation": "CONFIRM_PHASE6DA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
                "policy": "phase6da-cmake-cache-production-v1",
                "mode": "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-phase6cm-phase6co-v1",
                "restore_operation_count": 8,
                "restore_operations_contract_bytes": 4_788,
                "restore_operations_contract_sha256": "ca3126b513628e42a655bff5649ab3f7d4c915d89c2f881e68b07b023cea1c14",
                "restored_bytes": 49_567,
                "restored_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
                "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
                "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
                "removed_bytes": 246,
                "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
                "projected_bytes": 49_321,
                "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
                "projection_configured": True,
            }
            and PHASE6DA_CONFIGURATION_RESTORE_CONTRACT_PREFIX
            == b"lumen.phase6da.configuration-restore-operations.v1\0"
            and len(PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS) == 8
            and len(_phase6da_restore_operations_payload())
            == PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_BYTES
            and hashlib.sha256(
                _phase6da_restore_operations_payload()
            ).hexdigest()
            == PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS_CONTRACT_SHA256,
            "self-test",
            "phase 6DA production projection contract drifted",
        )
        _require(
            PHASEALC_CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
                "record_id": PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
                "generation": 3,
                "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
                "original_role": "candidate",
                "candidate_selector": PHASEALC_DIRECT_SELECTOR,
                "selector_bundle": {
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                    CONFIGURATION_PROJECTION_SELECTOR: True,
                    PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                    PHASEIFSM_DIRECT_SELECTOR: True,
                    PHASEALC_DIRECT_SELECTOR: True,
                },
                "raw_bytes": 46_749,
                "raw_sha256": "3d42b0edc9be9efa0156a02400de50ae4afb127ca30b7a74df0a36c1e2291e71",
                "projection_kind": "alc-strip-three-build-options-v1",
                "attestation": "CONFIRM_ALC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
                "policy": "alc-cmake-cache-production-v1",
                "mode": "strip-exact-active-alc-build-options-v1",
                "strip_operation_count": 3,
                "strip_operations_contract_bytes": 421,
                "strip_operations_contract_sha256": "704c751375758603328e79b34678e591ad4e52a419d18b35293f252516988e26",
                "removed_bytes": 355,
                "removed_segment_sha256": "c0ec59ce9c4b95ef3bf3643b6fccbdf41aba1c1c034c8f8e630ad0ccc2b60e78",
                "projected_bytes": 46_394,
                "projected_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
                "projection_configured": True,
            }
            and len(PHASEALC_CONFIGURATION_STRIP_OPERATIONS) == 3
            and len(_phasealc_strip_operations_payload()) == 421
            and hashlib.sha256(
                _phasealc_strip_operations_payload()
            ).hexdigest()
            == PHASEALC_CONFIGURATION_STRIP_OPERATIONS_CONTRACT_SHA256
            and len(PHASEALC_CONFIGURATION_REMOVED_SEGMENT) == 355
            and hashlib.sha256(
                PHASEALC_CONFIGURATION_REMOVED_SEGMENT
            ).hexdigest()
            == PHASEALC_CONFIGURATION_PROJECTION_CONTRACT[
                "removed_segment_sha256"
            ],
            "self-test",
            "ALC production projection contract drifted",
        )
        _require(
            PHASENODA_CONFIGURATION_PROJECTION_CONTRACT
            == {
                **PHASEALC_CONFIGURATION_PROJECTION_CONTRACT,
                "record_id": PHASENODA_DIRECT_CANDIDATE_RECORD_ID,
                "generation": 5,
                "parent_record_id": (
                    PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
                ),
                "candidate_selector": PHASENODA_DIRECT_SELECTOR,
                "selector_bundle": PHASENODA_EXPECTED_SELECTOR_BUNDLE,
                "attestation": (
                    "CONFIRM_NODA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
                ),
                "policy": "noda-cmake-cache-production-v1",
            }
            and PHASENODA_CONFIGURATION_PROJECTION_CONTRACT["raw_bytes"]
            == 46_749
            and PHASENODA_CONFIGURATION_PROJECTION_CONTRACT["raw_sha256"]
            == "3d42b0edc9be9efa0156a02400de50ae4afb127ca30b7a74df0a36c1e2291e71"
            and PHASENODA_CONFIGURATION_PROJECTION_CONTRACT[
                "projected_bytes"
            ]
            == 46_394
            and PHASENODA_CONFIGURATION_PROJECTION_CONTRACT[
                "projected_sha256"
            ]
            == "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
            "self-test",
            "NODA production projection contract drifted",
        )

        _require(
            PHASE6DA_DIRECT_CONFIGURATION_BYTES == 46_394
            and PHASE6DA_DIRECT_CONFIGURATION_SHA256
            == "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
            and PHASE6DA_DIRECT_RECORD_IDS
            == {
                PHASE6DA_DIRECT_BASELINE_RECORD_ID,
                PHASE6DA_DIRECT_CANDIDATE_RECORD_ID,
                PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
                PHASEUFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID,
                PHASEFCFOS_DIRECT_CANDIDATE_RECORD_ID,
                PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
                PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
                PHASENODA_DIRECT_CANDIDATE_RECORD_ID,
            }
            and PHASE6DA_DIRECT_CONTRACTS[0]["generation"] == 0
            and PHASE6DA_DIRECT_CONTRACTS[0]["parent_record_id"] is None
            and PHASE6DA_DIRECT_CONTRACTS[1]["generation"] == 1
            and PHASE6DA_DIRECT_CONTRACTS[1]["parent_record_id"]
            == PHASE6DA_DIRECT_BASELINE_RECORD_ID
            and PHASE6DA_DIRECT_CONTRACTS[2]["generation"] == 2
            and PHASE6DA_DIRECT_CONTRACTS[2]["parent_record_id"]
            == PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
            and PHASE6DA_DIRECT_CONTRACTS[2]["selector_bundle"]
            == {
                CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                CONFIGURATION_PROJECTION_SELECTOR: True,
                PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                PHASEIFSM_DIRECT_SELECTOR: True,
            }
            and PHASE6DA_DIRECT_CONTRACTS[2]["required_receipt_fields"]
            == {
                **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
                "fast_block_dense_directory_requested": "1",
                "fast_block_dense_directory": "1",
                "fast_block_inline_fixed_signature_match_requested": "1",
                "fast_block_inline_fixed_signature_match": "1",
            }
            and PHASE6DA_DIRECT_CONTRACTS[2]["candidate_marker"]
            == {
                "required": True,
                "expected_count": 1,
                "observed_count": 1,
                "status": "clean",
                "coverage_required": True,
                "coverage_positive": True,
            }
            and PHASE6DA_DIRECT_CONTRACTS[3]["generation"] == 3
            and PHASE6DA_DIRECT_CONTRACTS[3]["parent_record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            and PHASE6DA_DIRECT_CONTRACTS[3]["record_id"]
            == "ufsb-imse-c-fast-block-dispatch-bundle-g000003"
            and PHASE6DA_DIRECT_CONTRACTS[3]["primary_selector"]
            == PHASEUFSB_DIRECT_SELECTOR
            and PHASE6DA_DIRECT_CONTRACTS[3]["companion_selectors"]
            == frozenset({PHASEIMSE_DIRECT_SELECTOR})
            and PHASE6DA_DIRECT_CONTRACTS[3]["selector_bundle"]
            == {
                CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                CONFIGURATION_PROJECTION_SELECTOR: True,
                PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                PHASEIFSM_DIRECT_SELECTOR: True,
                PHASEUFSB_DIRECT_SELECTOR: True,
                PHASEIMSE_DIRECT_SELECTOR: True,
            }
            and PHASE6DA_DIRECT_CONTRACTS[3]["required_receipt_fields"]
            == {
                **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
                "fast_block_dense_directory_requested": "1",
                "fast_block_dense_directory": "1",
                "fast_block_inline_fixed_signature_match_requested": "1",
                "fast_block_inline_fixed_signature_match": "1",
                "fast_block_unrolled_fixed_signature_batching_requested": "1",
                "fast_block_unrolled_fixed_signature_batching": "1",
                "fast_block_idempotent_mru_store_elision_requested": "1",
                "fast_block_idempotent_mru_store_elision": "1",
            }
            and PHASE6DA_DIRECT_CONTRACTS[3]["candidate_marker"]
            == {
                "required": True,
                "expected_count": 1,
                "observed_count": 1,
                "status": "clean",
                "coverage_required": True,
                "coverage_positive": True,
            }
            and PHASE6DA_DIRECT_CONTRACTS[4]["generation"] == 3
            and PHASE6DA_DIRECT_CONTRACTS[4]["parent_record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            and PHASE6DA_DIRECT_CONTRACTS[4]["record_id"]
            == PHASEFCFOS_DIRECT_CANDIDATE_RECORD_ID
            and PHASE6DA_DIRECT_CONTRACTS[4]["selector_bundle"]
            == {
                CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                CONFIGURATION_PROJECTION_SELECTOR: True,
                PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                PHASEIFSM_DIRECT_SELECTOR: True,
                PHASEFCFOS_DIRECT_SELECTOR: True,
            }
            and PHASE6DA_DIRECT_CONTRACTS[4]["required_receipt_fields"]
            == {
                **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
                "fast_block_dense_directory_requested": "1",
                "fast_block_dense_directory": "1",
                "fast_block_inline_fixed_signature_match_requested": "1",
                "fast_block_inline_fixed_signature_match": "1",
                "fast_block_unrolled_fixed_signature_batching_requested": "0",
                "fast_block_unrolled_fixed_signature_batching": "0",
                "fast_block_idempotent_mru_store_elision_requested": "0",
                "fast_block_idempotent_mru_store_elision": "0",
                "fast_block_callback_free_ordinary_specialization_requested": "1",
                "fast_block_callback_free_ordinary_specialization": "1",
            }
            and PHASE6DA_DIRECT_CONTRACTS[4]["candidate_marker_line"]
            == (
                "[A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION_FINAL] "
                "requested=positive effective=positive reentry_safe=positive "
                "snapshot_elision=positive status=clean coverage=positive"
            )
            and PHASE6DA_DIRECT_CONTRACTS[4]["candidate_marker"]
            == {
                "required": True,
                "expected_count": 1,
                "observed_count": 1,
                "status": "clean",
                "coverage_required": True,
                "coverage_positive": True,
                "line": PHASEFCFOS_DIRECT_CANDIDATE_MARKER_LINE,
            }
            and PHASE6DA_DIRECT_CONTRACTS[5]["generation"] == 3
            and PHASE6DA_DIRECT_CONTRACTS[5]["parent_record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            and PHASE6DA_DIRECT_CONTRACTS[5]["record_id"]
            == PHASEALC_DIRECT_CANDIDATE_RECORD_ID
            and PHASE6DA_DIRECT_CONTRACTS[5]["selector_bundle"]
            == {
                CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                CONFIGURATION_PROJECTION_SELECTOR: True,
                PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                PHASEIFSM_DIRECT_SELECTOR: True,
                PHASEALC_DIRECT_SELECTOR: True,
            }
            and PHASE6DA_DIRECT_CONTRACTS[5]["required_receipt_fields"]
            == {
                **PHASE6DA_DIRECT_COMMON_RECEIPT_FIELDS,
                "fast_block_dense_directory_requested": "1",
                "fast_block_dense_directory": "1",
                "fast_block_inline_fixed_signature_match_requested": "1",
                "fast_block_inline_fixed_signature_match": "1",
                "fast_block_unrolled_fixed_signature_batching_requested": "0",
                "fast_block_unrolled_fixed_signature_batching": "0",
                "fast_block_idempotent_mru_store_elision_requested": "0",
                "fast_block_idempotent_mru_store_elision": "0",
                "fast_block_callback_free_ordinary_specialization_requested": "0",
                "fast_block_callback_free_ordinary_specialization": "0",
                "arm_loading_composite_requested": "1",
                "arm_loading_composite": "1",
            }
            and PHASE6DA_DIRECT_CONTRACTS[5]["candidate_marker_line"]
            == PHASEALC_DIRECT_CANDIDATE_MARKER_LINE
            and PHASE6DA_DIRECT_CONTRACTS[5]["candidate_marker"]
            == {
                "required": True,
                "expected_count": 1,
                "observed_count": 1,
                "status": "clean",
                "coverage_required": True,
                "coverage_positive": True,
                "line": PHASEALC_DIRECT_CANDIDATE_MARKER_LINE,
            },
            "self-test",
            "phase-6DA/IFSM/UFSB+IMSE/FCFOS/ALC direct lineage contract drifted",
        )

        def phase6da_direct_value(
            contract: Mapping[str, Any], *, candidate: bool
        ) -> Mapping[str, Any]:
            value = _fixture_manifest(
                record_id=contract["record_id"],
                receipt="phase6da-direct-placeholder",
                generation=contract["generation"],
                parent=contract["parent_record_id"],
                role=contract["original_role"],
                wall_ns=(
                    400_000_000_000 if candidate else 500_000_000_000
                ),
                cumulative_ms=384_000 if candidate else 480_000,
                fort_interval_ms=400 if candidate else 500,
                game_interval_ms=200 if candidate else 250,
                revision_digit="5" if candidate else "4",
                selector_enabled=candidate,
            )
            value["protocol"]["protocol_id"] = PHASE6DA_DIRECT_PROTOCOL_ID
            value["comparability"]["os"] = {
                "name": "macOS",
                "version": PHASE6DA_DIRECT_OS_VERSION,
                "build": PHASE6DA_DIRECT_OS_BUILD,
                "policy": PHASE6DA_DIRECT_OS_POLICY,
            }
            value["comparability"]["runtime"][
                "configuration_sha256"
            ] = PHASE6DA_DIRECT_CONFIGURATION_SHA256
            value["provenance"]["selectors"] = {
                "expected": dict(contract["selector_bundle"]),
                "effective": dict(
                    contract.get(
                        "effective_selector_bundle",
                        contract["selector_bundle"],
                    )
                ),
            }
            value["provenance"]["run_receipt"] = (
                A64_CONFIGURATION_PREFIX
                + " "
                + " ".join(
                    f"{name}={expected}"
                    for name, expected in contract[
                        "required_receipt_fields"
                    ].items()
                )
            )
            value["lifecycle"]["candidate_marker"] = dict(
                contract["candidate_marker"]
            )
            return value

        phase6da_direct_root = root / "phase6da-direct"
        phase6da_direct_baseline_root = phase6da_direct_root / "baseline"
        phase6da_direct_candidate_root = phase6da_direct_root / "candidate"
        phase6da_direct_baseline_root.mkdir(parents=True)
        phase6da_direct_candidate_root.mkdir()
        phase6da_direct_baseline_value = phase6da_direct_value(
            PHASE6DA_DIRECT_CONTRACTS[0], candidate=False
        )
        phase6da_direct_candidate_value = phase6da_direct_value(
            PHASE6DA_DIRECT_CONTRACTS[1], candidate=True
        )
        phase6da_direct_baseline_path = (
            phase6da_direct_baseline_root / "manifest.json"
        )
        phase6da_direct_candidate_path = (
            phase6da_direct_candidate_root / "manifest.json"
        )
        _write_fixture(
            phase6da_direct_baseline_path, phase6da_direct_baseline_value
        )
        _write_fixture(
            phase6da_direct_candidate_path, phase6da_direct_candidate_value
        )
        phase6da_direct_baseline = load_manifest(
            phase6da_direct_baseline_path
        )
        phase6da_direct_candidate = load_manifest(
            phase6da_direct_candidate_path
        )
        phase6da_direct_report = build_report(
            phase6da_direct_baseline, phase6da_direct_candidate
        )
        _require(
            phase6da_direct_report["valid"] is True
            and phase6da_direct_report["records"]["baseline"]["record_id"]
            == PHASE6DA_DIRECT_BASELINE_RECORD_ID
            and phase6da_direct_report["records"]["candidate"]["record_id"]
            == PHASE6DA_DIRECT_CANDIDATE_RECORD_ID,
            "self-test",
            "phase-6DA direct initial pair did not produce a valid report",
        )

        phaseifsm_direct_root = phase6da_direct_root / "ifsm-candidate"
        phaseifsm_direct_root.mkdir()
        phaseifsm_direct_value = phase6da_direct_value(
            PHASE6DA_DIRECT_CONTRACTS[2], candidate=True
        )
        phaseifsm_direct_path = phaseifsm_direct_root / "manifest.json"
        _write_fixture(phaseifsm_direct_path, phaseifsm_direct_value)
        phaseifsm_direct_candidate = load_manifest(phaseifsm_direct_path)
        phaseifsm_direct_report = build_report(
            phase6da_direct_candidate, phaseifsm_direct_candidate
        )
        _require(
            phaseifsm_direct_report["valid"] is True
            and phaseifsm_direct_report["records"]["baseline"]["record_id"]
            == PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
            and phaseifsm_direct_report["records"]["candidate"]["record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            and phaseifsm_direct_report["records"]["candidate"]["generation"]
            == 2
            and phaseifsm_direct_report["records"]["candidate"][
                "parent_record_id"
            ]
            == PHASE6DA_DIRECT_CANDIDATE_RECORD_ID,
            "self-test",
            "persistent IFSM generation-2 pair did not produce a valid report",
        )

        phaseufsb_imse_direct_root = (
            phase6da_direct_root / "ufsb-imse-candidate"
        )
        phaseufsb_imse_direct_root.mkdir()
        phaseufsb_imse_direct_value = phase6da_direct_value(
            PHASE6DA_DIRECT_CONTRACTS[3], candidate=True
        )
        phaseufsb_imse_direct_path = (
            phaseufsb_imse_direct_root / "manifest.json"
        )
        _write_fixture(
            phaseufsb_imse_direct_path, phaseufsb_imse_direct_value
        )
        phaseufsb_imse_direct_candidate = load_manifest(
            phaseufsb_imse_direct_path
        )
        phaseufsb_imse_direct_report = build_report(
            phaseifsm_direct_candidate, phaseufsb_imse_direct_candidate
        )
        _require(
            phaseufsb_imse_direct_report["valid"] is True
            and phaseufsb_imse_direct_report["records"]["baseline"][
                "record_id"
            ]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            and phaseufsb_imse_direct_report["records"]["candidate"][
                "record_id"
            ]
            == PHASEUFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID
            and phaseufsb_imse_direct_report["records"]["candidate"][
                "generation"
            ]
            == 3
            and phaseufsb_imse_direct_report["records"]["candidate"][
                "parent_record_id"
            ]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
            "self-test",
            "combined UFSB+IMSE generation-3 pair did not produce a valid report",
        )

        phasefcfos_direct_root = phase6da_direct_root / "fcfos-candidate"
        phasefcfos_direct_root.mkdir()
        phasefcfos_direct_value = phase6da_direct_value(
            PHASE6DA_DIRECT_CONTRACTS[4], candidate=True
        )
        phasefcfos_direct_path = phasefcfos_direct_root / "manifest.json"
        _write_fixture(phasefcfos_direct_path, phasefcfos_direct_value)
        phasefcfos_direct_candidate = load_manifest(phasefcfos_direct_path)
        phasefcfos_direct_report = build_report(
            phaseifsm_direct_candidate, phasefcfos_direct_candidate
        )
        _require(
            phasefcfos_direct_report["valid"] is True
            and phasefcfos_direct_report["records"]["baseline"]["record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            and phasefcfos_direct_report["records"]["candidate"]["record_id"]
            == PHASEFCFOS_DIRECT_CANDIDATE_RECORD_ID
            and phasefcfos_direct_report["records"]["candidate"]["generation"]
            == 3
            and phasefcfos_direct_report["records"]["candidate"][
                "parent_record_id"
            ]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
            "self-test",
            "FCFOS generation-3 pair did not produce a valid report",
        )

        def phasealc_projection_receipt(
            contract: Mapping[str, Any], epoch_sha256: str
        ) -> Mapping[str, Any]:
            return {
                "schema": {
                    "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
                    "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
                },
                "attestation": contract["attestation"],
                "policy": contract["policy"],
                "mode": contract["mode"],
                "record_id": contract["record_id"],
                "candidate_selector": contract["candidate_selector"],
                "raw_bytes": contract["raw_bytes"],
                "raw_sha256": contract["raw_sha256"],
                "strip_operation_count": contract["strip_operation_count"],
                "strip_operations_contract_bytes": contract[
                    "strip_operations_contract_bytes"
                ],
                "strip_operations_contract_sha256": contract[
                    "strip_operations_contract_sha256"
                ],
                "removed_bytes": contract["removed_bytes"],
                "removed_segment_sha256": contract[
                    "removed_segment_sha256"
                ],
                "projected_bytes": contract["projected_bytes"],
                "projected_sha256": contract["projected_sha256"],
                "epoch_configuration_sha256": epoch_sha256,
            }

        phasealc_direct_root = phase6da_direct_root / "alc-candidate"
        phasealc_direct_root.mkdir()
        phasealc_projected_fixture = (
            b"alc-fixture-prefix\nalc-fixture-middle\nalc-fixture-suffix\n"
        )
        phasealc_raw_fixture = (
            b"alc-fixture-prefix\n"
            + PHASEALC_CONFIGURATION_AHC_BLOCK
            + PHASEALC_CONFIGURATION_ALC_BLOCK
            + b"alc-fixture-middle\n"
            + PHASEALC_CONFIGURATION_PURGE_BLOCK
            + b"alc-fixture-suffix\n"
        )
        phasealc_fixture_contract = dict(
            PHASEALC_CONFIGURATION_PROJECTION_CONTRACT
        )
        phasealc_fixture_contract.update(
            {
                "raw_bytes": len(phasealc_raw_fixture),
                "raw_sha256": hashlib.sha256(
                    phasealc_raw_fixture
                ).hexdigest(),
                "projected_bytes": len(phasealc_projected_fixture),
                "projected_sha256": hashlib.sha256(
                    phasealc_projected_fixture
                ).hexdigest(),
            }
        )
        phasealc_direct_value = phase6da_direct_value(
            PHASE6DA_DIRECT_CONTRACTS[5], candidate=True
        )
        phasealc_direct_value["comparability"]["runtime"][
            "configuration_sha256"
        ] = phasealc_fixture_contract["projected_sha256"]
        phasealc_raw_path = (
            phasealc_direct_root / CONFIGURATION_PROJECTION_RAW_NAME
        )
        phasealc_receipt_path = (
            phasealc_direct_root / CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        phasealc_raw_path.write_bytes(phasealc_raw_fixture)
        phasealc_raw_path.chmod(0o444)
        _write_fixture(
            phasealc_receipt_path,
            phasealc_projection_receipt(
                phasealc_fixture_contract,
                phasealc_fixture_contract["projected_sha256"],
            ),
        )
        phasealc_receipt_path.chmod(0o444)
        phasealc_artifacts = phasealc_direct_value["provenance"][
            "artifacts_sha256"
        ]
        phasealc_artifacts["configuration_cache_raw"] = hashlib.sha256(
            phasealc_raw_path.read_bytes()
        ).hexdigest()
        phasealc_artifacts[
            "configuration_projection_receipt"
        ] = hashlib.sha256(phasealc_receipt_path.read_bytes()).hexdigest()
        phasealc_direct_path = phasealc_direct_root / "manifest.json"
        _write_fixture(phasealc_direct_path, phasealc_direct_value)
        phasealc_direct_candidate = load_manifest(
            phasealc_direct_path,
            _projection_contract=phasealc_fixture_contract,
        )
        _require(
            phasealc_direct_candidate.provenance["record_id"]
            == PHASEALC_DIRECT_CANDIDATE_RECORD_ID
            and phasealc_direct_candidate.lineage["generation"] == 3
            and phasealc_direct_candidate.lineage["parent_record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
            "self-test",
            "ALC generation-3 direct/projection fixture was not accepted",
        )

        metal_direct_root = phase6da_direct_root / "metal-runtime-candidate"
        metal_direct_root.mkdir()
        metal_fixture_contract = dict(
            PHASEMETAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT
        )
        metal_fixture_contract.update(
            {
                "raw_bytes": len(phasealc_raw_fixture),
                "raw_sha256": hashlib.sha256(
                    phasealc_raw_fixture
                ).hexdigest(),
                "projected_bytes": len(phasealc_projected_fixture),
                "projected_sha256": hashlib.sha256(
                    phasealc_projected_fixture
                ).hexdigest(),
            }
        )
        metal_direct_value = phase6da_direct_value(
            PHASE6DA_DIRECT_CONTRACTS[6], candidate=True
        )
        metal_direct_value["comparability"]["runtime"][
            "configuration_sha256"
        ] = metal_fixture_contract["projected_sha256"]
        metal_raw_path = (
            metal_direct_root / CONFIGURATION_PROJECTION_RAW_NAME
        )
        metal_receipt_path = (
            metal_direct_root / CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        metal_raw_path.write_bytes(phasealc_raw_fixture)
        metal_raw_path.chmod(0o444)
        _write_fixture(
            metal_receipt_path,
            phasealc_projection_receipt(
                metal_fixture_contract,
                metal_fixture_contract["projected_sha256"],
            ),
        )
        metal_receipt_path.chmod(0o444)
        metal_artifacts = metal_direct_value["provenance"][
            "artifacts_sha256"
        ]
        metal_artifacts["configuration_cache_raw"] = hashlib.sha256(
            metal_raw_path.read_bytes()
        ).hexdigest()
        metal_artifacts[
            "configuration_projection_receipt"
        ] = hashlib.sha256(metal_receipt_path.read_bytes()).hexdigest()
        metal_direct_path = metal_direct_root / "manifest.json"
        _write_fixture(metal_direct_path, metal_direct_value)
        metal_direct_candidate = load_manifest(
            metal_direct_path,
            _projection_contract=metal_fixture_contract,
        )
        metal_direct_report = build_report(
            phasealc_direct_candidate, metal_direct_candidate
        )
        _require(
            metal_direct_report["valid"] is True
            and metal_direct_candidate.provenance["record_id"]
            == PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
            and metal_direct_candidate.lineage["generation"] == 4
            and metal_direct_candidate.lineage["parent_record_id"]
            == PHASEALC_DIRECT_CANDIDATE_RECORD_ID
            and metal_fixture_contract["attestation"]
            == "CONFIRM_MSORE_MSFC_MDSSC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
            and metal_fixture_contract["policy"]
            == "msore-msfc-mdssc-cmake-cache-production-v1"
            and metal_fixture_contract["mode"]
            == "strip-exact-active-alc-build-options-v1"
            and PHASEMSORE_DIRECT_CANDIDATE_MARKER_LINE
            == (
                "[A64_METAL_STATE_ONLY_RECORD_ELISION_FINAL] "
                "requested=positive effective=positive state_only=positive "
                "snapshot_consumed=positive status=clean coverage=positive"
            )
            and PHASEMSFC_COMPANION_MARKER_LINE
            == (
                "[A64_METAL_SHADER_FUNCTION_CACHE_FINAL] lookup=positive "
                "hit=positive compile=positive requested=positive "
                "status=clean coverage=positive"
            )
            and PHASEMDSSC_COMPANION_MARKER_LINE
            == (
                "[A64_METAL_DEPTH_STENCIL_STATE_CACHE_FINAL] "
                "requested=positive compiled=positive lookup=positive "
                "hit=positive miss=positive create_match=positive "
                "status=clean coverage=positive"
            ),
            "self-test",
            "MSORE+MSFC+MDSSC generation-4 direct/projection fixture was not accepted",
        )

        # NODA is the sole generation-5 child of the Metal bundle.  Its
        # effective selector receipt intentionally differs from its expected
        # bundle only for the retained 6CL selector, which NODA supersedes.
        historical_metal_report_bytes = _canonical_bytes(metal_direct_report)
        noda_direct_root = phase6da_direct_root / "noda-candidate"
        noda_direct_root.mkdir()
        noda_fixture_contract = dict(
            PHASENODA_CONFIGURATION_PROJECTION_CONTRACT
        )
        noda_fixture_contract.update(
            {
                "raw_bytes": len(phasealc_raw_fixture),
                "raw_sha256": hashlib.sha256(
                    phasealc_raw_fixture
                ).hexdigest(),
                "projected_bytes": len(phasealc_projected_fixture),
                "projected_sha256": hashlib.sha256(
                    phasealc_projected_fixture
                ).hexdigest(),
            }
        )
        noda_direct_value = phase6da_direct_value(
            PHASE6DA_DIRECT_CONTRACTS[7], candidate=True
        )
        noda_direct_value["comparability"]["runtime"][
            "configuration_sha256"
        ] = noda_fixture_contract["projected_sha256"]
        noda_raw_path = noda_direct_root / CONFIGURATION_PROJECTION_RAW_NAME
        noda_receipt_path = (
            noda_direct_root / CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        noda_raw_path.write_bytes(phasealc_raw_fixture)
        noda_raw_path.chmod(0o444)
        _write_fixture(
            noda_receipt_path,
            phasealc_projection_receipt(
                noda_fixture_contract,
                noda_fixture_contract["projected_sha256"],
            ),
        )
        noda_receipt_path.chmod(0o444)
        noda_artifacts = noda_direct_value["provenance"][
            "artifacts_sha256"
        ]
        noda_artifacts["configuration_cache_raw"] = hashlib.sha256(
            noda_raw_path.read_bytes()
        ).hexdigest()
        noda_artifacts[
            "configuration_projection_receipt"
        ] = hashlib.sha256(noda_receipt_path.read_bytes()).hexdigest()
        noda_direct_path = noda_direct_root / "manifest.json"
        _write_fixture(noda_direct_path, noda_direct_value)
        noda_direct_candidate = load_manifest(
            noda_direct_path,
            _projection_contract=noda_fixture_contract,
        )
        noda_direct_report = build_report(
            metal_direct_candidate, noda_direct_candidate
        )
        _require(
            noda_direct_report["valid"] is True
            and noda_direct_candidate.provenance["record_id"]
            == PHASENODA_DIRECT_CANDIDATE_RECORD_ID
            and noda_direct_candidate.lineage["generation"] == 5
            and noda_direct_candidate.lineage["parent_record_id"]
            == PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
            and noda_direct_candidate.provenance["selectors"]["expected"]
            == PHASENODA_EXPECTED_SELECTOR_BUNDLE
            and noda_direct_candidate.provenance["selectors"]["effective"]
            == PHASENODA_EFFECTIVE_SELECTOR_BUNDLE
            and PHASEPBA_DIRECT_SELECTOR
            not in noda_direct_candidate.provenance["selectors"]["expected"]
            and PHASEPBA_DIRECT_SELECTOR
            not in noda_direct_candidate.provenance["selectors"]["effective"]
            and PHASE6DA_DIRECT_CONTRACTS[7]["retained_marker_lines"]
            == PHASENODA_RETAINED_MARKER_LINES
            and PHASE6DA_DIRECT_CONTRACTS[7]["candidate_marker_line"]
            == PHASENODA_DIRECT_CANDIDATE_MARKER_LINE,
            "self-test",
            "NODA generation-5 direct/projection fixture was not accepted",
        )
        _require(
            _canonical_bytes(
                build_report(
                    phasealc_direct_candidate, metal_direct_candidate
                )
            )
            == historical_metal_report_bytes,
            "self-test",
            "NODA support changed the historical ALC-vs-Metal report bytes",
        )

        noda_manifest_negatives = []
        for label, key, replacement in (
            ("wrong-generation", "generation", 4),
            (
                "wrong-parent",
                "parent_record_id",
                PHASEALC_DIRECT_CANDIDATE_RECORD_ID,
            ),
        ):
            value = json.loads(json.dumps(noda_direct_value))
            value["lineage"][key] = replacement
            noda_manifest_negatives.append((label, value))

        noda_selector_off = json.loads(json.dumps(noda_direct_value))
        for selector_map in noda_selector_off["provenance"]["selectors"].values():
            selector_map[PHASENODA_DIRECT_SELECTOR] = False
        noda_manifest_negatives.append(("selector-off", noda_selector_off))

        noda_selector_missing = json.loads(json.dumps(noda_direct_value))
        for selector_map in noda_selector_missing["provenance"][
            "selectors"
        ].values():
            del selector_map[PHASENODA_DIRECT_SELECTOR]
        noda_manifest_negatives.append(
            ("selector-missing", noda_selector_missing)
        )

        noda_expected_6cl_off = json.loads(json.dumps(noda_direct_value))
        noda_expected_6cl_off["provenance"]["selectors"]["expected"][
            CONFIGURATION_PROJECTION_STABLE_SELECTOR
        ] = False
        noda_manifest_negatives.append(
            ("expected-6cl-off", noda_expected_6cl_off)
        )

        noda_effective_6cl_on = json.loads(json.dumps(noda_direct_value))
        noda_effective_6cl_on["provenance"]["selectors"]["effective"][
            CONFIGURATION_PROJECTION_STABLE_SELECTOR
        ] = True
        noda_manifest_negatives.append(
            ("effective-6cl-on", noda_effective_6cl_on)
        )

        noda_other_effective_off = json.loads(json.dumps(noda_direct_value))
        noda_other_effective_off["provenance"]["selectors"]["effective"][
            CONFIGURATION_PROJECTION_SELECTOR
        ] = False
        noda_manifest_negatives.append(
            ("non-6cl-effective-mismatch", noda_other_effective_off)
        )

        noda_retained_missing = json.loads(json.dumps(noda_direct_value))
        for selector_map in noda_retained_missing["provenance"][
            "selectors"
        ].values():
            del selector_map[PHASEMSFC_COMPANION_SELECTOR]
        noda_manifest_negatives.append(
            ("retained-selector-missing", noda_retained_missing)
        )

        noda_pba_present = json.loads(json.dumps(noda_direct_value))
        for selector_map in noda_pba_present["provenance"]["selectors"].values():
            selector_map[PHASEPBA_DIRECT_SELECTOR] = False
        noda_manifest_negatives.append(("pba-selector-present", noda_pba_present))

        noda_wrong_scope = json.loads(json.dumps(noda_direct_value))
        noda_wrong_scope["provenance"]["record_id"] = (
            PHASEMETAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
        )
        noda_manifest_negatives.append(
            ("6cl-exception-outside-g5", noda_wrong_scope)
        )

        for field, expected, replacement in (
            ("stable_negative_route_requested", "1", "0"),
            ("stable_negative_route", "0", "1"),
            ("stable_negative_route_superseded", "1", "0"),
            ("profitable_block_admission_requested", "0", "1"),
            ("profitable_block_admission", "0", "1"),
            (
                "profitable_block_admission_class",
                "direct_positive_then_ordinary",
                "wrong_class",
            ),
            ("native_only_admission_requested", "1", "0"),
            ("native_only_admission", "1", "0"),
            (
                "native_only_admission_class",
                "fastblock_primary_native_only_dynarec",
                "wrong_class",
            ),
            ("fast_block_dense_directory", "1", "0"),
            ("metal_state_only_record_elision", "1", "0"),
        ):
            value = json.loads(json.dumps(noda_direct_value))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}={expected}", f"{field}={replacement}")
            noda_manifest_negatives.append((f"receipt-{field}", value))

        noda_wrong_marker = json.loads(json.dumps(noda_direct_value))
        noda_wrong_marker["lifecycle"]["candidate_marker"]["line"] = (
            PHASENODA_DIRECT_CANDIDATE_MARKER_LINE.replace(
                "effective=positive ordinary_routed=positive",
                "ordinary_routed=positive effective=positive",
            )
        )
        noda_manifest_negatives.append(
            ("marker-line-reordered", noda_wrong_marker)
        )
        noda_inexact_marker = json.loads(json.dumps(noda_direct_value))
        noda_inexact_marker["lifecycle"]["candidate_marker"].update(
            {"expected_count": 2, "observed_count": 2}
        )
        noda_manifest_negatives.append(
            ("marker-not-exact-once", noda_inexact_marker)
        )

        for label, value in noda_manifest_negatives:
            path = noda_direct_root / f"negative-{label}.json"
            _write_fixture(path, value)
            _expect_failure(
                lambda path=path: load_manifest(
                    path, _projection_contract=noda_fixture_contract
                ),
                f"invalid NODA direct/projection record {label}",
            )

        metal_manifest_negatives = []
        for label, key, replacement in (
            ("wrong-generation", "generation", 3),
            (
                "wrong-parent",
                "parent_record_id",
                PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
            ),
        ):
            value = json.loads(json.dumps(metal_direct_value))
            value["lineage"][key] = replacement
            metal_manifest_negatives.append((label, value))
        for selector in PHASEMETAL_RUNTIME_BUNDLE_SELECTORS:
            for label, replacement in (("off", False), ("integer", 1)):
                value = json.loads(json.dumps(metal_direct_value))
                for selector_map in value["provenance"]["selectors"].values():
                    selector_map[selector] = replacement
                metal_manifest_negatives.append(
                    (f"{selector}-{label}", value)
                )
            missing = json.loads(json.dumps(metal_direct_value))
            for selector_map in missing["provenance"]["selectors"].values():
                del selector_map[selector]
            metal_manifest_negatives.append(
                (f"{selector}-missing", missing)
            )
        mixed_sibling = json.loads(json.dumps(metal_direct_value))
        for selector_map in mixed_sibling["provenance"]["selectors"].values():
            selector_map[PHASEUFSB_DIRECT_SELECTOR] = False
        metal_manifest_negatives.append(("mixed-sibling", mixed_sibling))
        for field in (
            "metal_shader_function_cache_requested",
            "metal_shader_function_cache",
            "metal_depth_stencil_state_cache_requested",
            "metal_depth_stencil_state_cache",
            "metal_state_only_record_elision_requested",
            "metal_state_only_record_elision",
        ):
            value = json.loads(json.dumps(metal_direct_value))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}=1", f"{field}=0")
            metal_manifest_negatives.append((f"receipt-{field}", value))
        wrong_metal_marker = json.loads(json.dumps(metal_direct_value))
        wrong_metal_marker["lifecycle"]["candidate_marker"]["line"] = (
            PHASEMSORE_DIRECT_CANDIDATE_MARKER_LINE.replace(
                "requested=positive effective=positive",
                "effective=positive requested=positive",
            )
        )
        metal_manifest_negatives.append(
            ("marker-line-reordered", wrong_metal_marker)
        )
        for label, value in metal_manifest_negatives:
            path = metal_direct_root / f"negative-{label}.json"
            _write_fixture(path, value)
            _expect_failure(
                lambda path=path: load_manifest(
                    path, _projection_contract=metal_fixture_contract
                ),
                f"invalid MSORE+MSFC+MDSSC direct/projection record {label}",
            )

        phasealc_manifest_negatives = []
        for label, replacement in (("off", False), ("integer", 1)):
            value = json.loads(json.dumps(phasealc_direct_value))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[PHASEALC_DIRECT_SELECTOR] = replacement
            phasealc_manifest_negatives.append(
                (f"selector-{label}", value)
            )
        wrong_phasealc_receipt = json.loads(
            json.dumps(phasealc_direct_value)
        )
        wrong_phasealc_receipt["provenance"]["run_receipt"] = (
            wrong_phasealc_receipt["provenance"]["run_receipt"].replace(
                "arm_loading_composite_requested=1",
                "arm_loading_composite_requested=0",
            )
        )
        phasealc_manifest_negatives.append(
            ("requested-receipt-off", wrong_phasealc_receipt)
        )
        for sibling_field in (
            "fast_block_unrolled_fixed_signature_batching_requested",
            "fast_block_unrolled_fixed_signature_batching",
            "fast_block_idempotent_mru_store_elision_requested",
            "fast_block_idempotent_mru_store_elision",
            "fast_block_callback_free_ordinary_specialization_requested",
            "fast_block_callback_free_ordinary_specialization",
        ):
            active_sibling = json.loads(json.dumps(phasealc_direct_value))
            active_sibling["provenance"]["run_receipt"] = active_sibling[
                "provenance"
            ]["run_receipt"].replace(
                f"{sibling_field}=0", f"{sibling_field}=1"
            )
            phasealc_manifest_negatives.append(
                (f"active-sibling-{sibling_field}", active_sibling)
            )
        wrong_phasealc_marker = json.loads(json.dumps(phasealc_direct_value))
        wrong_phasealc_marker["lifecycle"]["candidate_marker"]["line"] = (
            PHASEALC_DIRECT_CANDIDATE_MARKER_LINE.replace(
                "purge=positive adaptive_snapshot=positive",
                "adaptive_snapshot=positive purge=positive",
            )
        )
        phasealc_manifest_negatives.append(
            ("marker-line-reordered", wrong_phasealc_marker)
        )
        wrong_phasealc_parent = json.loads(json.dumps(phasealc_direct_value))
        wrong_phasealc_parent["lineage"]["parent_record_id"] = (
            PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
        )
        phasealc_manifest_negatives.append(
            ("wrong-parent", wrong_phasealc_parent)
        )
        for label, value in phasealc_manifest_negatives:
            path = phasealc_direct_root / f"negative-{label}.json"
            _write_fixture(path, value)
            _expect_failure(
                lambda path=path: load_manifest(
                    path, _projection_contract=phasealc_fixture_contract
                ),
                f"invalid ALC direct/projection record {label}",
            )

        for label, malformed_raw in (
            (
                "duplicate-hotpath-block",
                phasealc_raw_fixture + PHASEALC_CONFIGURATION_AHC_BLOCK,
            ),
            (
                "missing-hotpath-block",
                phasealc_raw_fixture.replace(
                    PHASEALC_CONFIGURATION_AHC_BLOCK, b"", 1
                ),
            ),
        ):
            malformed_contract = dict(phasealc_fixture_contract)
            malformed_contract.update(
                {
                    "raw_bytes": len(malformed_raw),
                    "raw_sha256": hashlib.sha256(malformed_raw).hexdigest(),
                }
            )
            malformed_receipt = _canonical_bytes(
                phasealc_projection_receipt(
                    malformed_contract,
                    malformed_contract["projected_sha256"],
                )
            )
            _expect_failure_containing(
                lambda malformed_raw=malformed_raw,
                malformed_contract=malformed_contract,
                malformed_receipt=malformed_receipt: (
                    _validate_phasealc_configuration_projection(
                        phasealc_direct_value,
                        phasealc_direct_path,
                        contract=malformed_contract,
                        candidate_selector=PHASEALC_DIRECT_SELECTOR,
                        raw_path=phasealc_raw_path,
                        receipt_path=phasealc_receipt_path,
                        raw_payload=malformed_raw,
                        receipt_payload=malformed_receipt,
                    )
                ),
                f"an ALC projection with {label}",
                "block must be exact-once",
            )

        phase6da_direct_negative_root = (
            phase6da_direct_root / "negative"
        )
        phase6da_direct_negative_root.mkdir()
        phase6da_direct_negatives = []

        wrong_direct_protocol = json.loads(
            json.dumps(phase6da_direct_baseline_value)
        )
        wrong_direct_protocol["protocol"]["protocol_id"] = "wrong-protocol"
        phase6da_direct_negatives.append(
            ("wrong-protocol", wrong_direct_protocol)
        )
        for label, section, key, replacement in (
            ("wrong-os-version", "os", "version", "26.6.1"),
            ("wrong-os-build", "os", "build", "25G82"),
            ("wrong-os-policy", "os", "policy", "floating"),
            ("wrong-configuration", "runtime", "configuration_sha256", "0" * 64),
        ):
            value = json.loads(json.dumps(phase6da_direct_baseline_value))
            value["comparability"][section][key] = replacement
            phase6da_direct_negatives.append((label, value))

        for label, source_value, replacement in (
            ("baseline-selector-on", phase6da_direct_baseline_value, True),
            ("candidate-selector-off", phase6da_direct_candidate_value, False),
            ("integer-selector", phase6da_direct_candidate_value, 1),
        ):
            value = json.loads(json.dumps(source_value))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[
                    PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR
                ] = replacement
            phase6da_direct_negatives.append((label, value))

        missing_direct_selector = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        for selector_map in missing_direct_selector["provenance"][
            "selectors"
        ].values():
            del selector_map[CONFIGURATION_PROJECTION_STABLE_SELECTOR]
        phase6da_direct_negatives.append(
            ("missing-retained-selector", missing_direct_selector)
        )
        extra_direct_selector = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        for selector_map in extra_direct_selector["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negatives.append(
            ("retired-selector-present", extra_direct_selector)
        )
        wrong_direct_record = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        wrong_direct_record["provenance"]["record_id"] = (
            "unknown-phase6da-direct-record"
        )
        phase6da_direct_negatives.append(
            ("wrong-record", wrong_direct_record)
        )
        wrong_direct_parent = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        wrong_direct_parent["lineage"]["parent_record_id"] = "wrong-parent"
        phase6da_direct_negatives.append(
            ("wrong-parent", wrong_direct_parent)
        )
        wrong_direct_generation = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        wrong_direct_generation["lineage"]["generation"] = 2
        phase6da_direct_negatives.append(
            ("wrong-generation", wrong_direct_generation)
        )
        wrong_direct_receipt = json.loads(
            json.dumps(phase6da_direct_baseline_value)
        )
        wrong_direct_receipt["provenance"]["run_receipt"] = (
            wrong_direct_receipt["provenance"]["run_receipt"].replace(
                "fast_block_dense_directory_requested=0",
                "fast_block_dense_directory_requested=1",
            )
        )
        phase6da_direct_negatives.append(
            ("wrong-receipt", wrong_direct_receipt)
        )
        duplicate_direct_receipt = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        duplicate_direct_receipt["provenance"]["run_receipt"] += (
            " fast_block_dense_directory=1"
        )
        phase6da_direct_negatives.append(
            ("duplicate-receipt-field", duplicate_direct_receipt)
        )
        wrong_direct_marker = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        wrong_direct_marker["lifecycle"]["candidate_marker"][
            "coverage_positive"
        ] = False
        phase6da_direct_negatives.append(
            ("wrong-marker", wrong_direct_marker)
        )
        declared_direct_projection = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        for name in CONFIGURATION_PROJECTION_ARTIFACTS:
            declared_direct_projection["provenance"]["artifacts_sha256"][
                name
            ] = "0" * 64
        phase6da_direct_negatives.append(
            ("declared-projection-artifacts", declared_direct_projection)
        )

        for label, replacement in (
            ("ifsm-selector-off", False),
            ("ifsm-selector-integer", 1),
        ):
            value = json.loads(json.dumps(phaseifsm_direct_value))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[PHASEIFSM_DIRECT_SELECTOR] = replacement
            phase6da_direct_negatives.append((label, value))

        missing_ifsm_selector = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        for selector_map in missing_ifsm_selector["provenance"][
            "selectors"
        ].values():
            del selector_map[PHASEIFSM_DIRECT_SELECTOR]
        phase6da_direct_negatives.append(
            ("ifsm-selector-missing", missing_ifsm_selector)
        )

        disabled_ifsm_retained_6da = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        for selector_map in disabled_ifsm_retained_6da["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negatives.append(
            ("ifsm-retained-6da-off", disabled_ifsm_retained_6da)
        )

        extra_ifsm_selector = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        for selector_map in extra_ifsm_selector["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negatives.append(
            ("ifsm-extra-selector", extra_ifsm_selector)
        )

        wrong_ifsm_parent = json.loads(json.dumps(phaseifsm_direct_value))
        wrong_ifsm_parent["lineage"]["parent_record_id"] = (
            PHASE6DA_DIRECT_BASELINE_RECORD_ID
        )
        phase6da_direct_negatives.append(
            ("ifsm-wrong-parent", wrong_ifsm_parent)
        )
        wrong_ifsm_generation = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        wrong_ifsm_generation["lineage"]["generation"] = 3
        phase6da_direct_negatives.append(
            ("ifsm-wrong-generation", wrong_ifsm_generation)
        )

        for label, field in (
            (
                "ifsm-requested-receipt-off",
                "fast_block_inline_fixed_signature_match_requested",
            ),
            (
                "ifsm-effective-receipt-off",
                "fast_block_inline_fixed_signature_match",
            ),
        ):
            value = json.loads(json.dumps(phaseifsm_direct_value))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}=1", f"{field}=0")
            phase6da_direct_negatives.append((label, value))

        missing_ifsm_receipt = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        missing_ifsm_receipt["provenance"]["run_receipt"] = (
            missing_ifsm_receipt["provenance"]["run_receipt"].replace(
                " fast_block_inline_fixed_signature_match_requested=1", ""
            )
        )
        phase6da_direct_negatives.append(
            ("ifsm-receipt-missing", missing_ifsm_receipt)
        )

        duplicate_ifsm_receipt = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        duplicate_ifsm_receipt["provenance"]["run_receipt"] += (
            " fast_block_inline_fixed_signature_match=1"
        )
        phase6da_direct_negatives.append(
            ("ifsm-receipt-duplicate", duplicate_ifsm_receipt)
        )

        inexact_ifsm_marker = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        inexact_ifsm_marker["lifecycle"]["candidate_marker"].update(
            {"expected_count": 2, "observed_count": 2}
        )
        phase6da_direct_negatives.append(
            ("ifsm-marker-not-exact-once", inexact_ifsm_marker)
        )

        masquerading_ifsm_record = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        masquerading_ifsm_record["provenance"]["record_id"] = (
            PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
        )
        phase6da_direct_negatives.append(
            ("ifsm-masquerades-as-6da", masquerading_ifsm_record)
        )

        for selector_label, selector_name in (
            ("ufsb", PHASEUFSB_DIRECT_SELECTOR),
            ("imse", PHASEIMSE_DIRECT_SELECTOR),
        ):
            for value_label, replacement in (
                ("off", False),
                ("integer", 1),
            ):
                value = json.loads(
                    json.dumps(phaseufsb_imse_direct_value)
                )
                for selector_map in value["provenance"]["selectors"].values():
                    selector_map[selector_name] = replacement
                phase6da_direct_negatives.append(
                    (f"{selector_label}-selector-{value_label}", value)
                )

            missing_selector = json.loads(
                json.dumps(phaseufsb_imse_direct_value)
            )
            for selector_map in missing_selector["provenance"][
                "selectors"
            ].values():
                del selector_map[selector_name]
            phase6da_direct_negatives.append(
                (f"{selector_label}-selector-missing", missing_selector)
            )

        mismatched_ufsb_selector = json.loads(
            json.dumps(phaseufsb_imse_direct_value)
        )
        mismatched_ufsb_selector["provenance"]["selectors"]["effective"][
            PHASEUFSB_DIRECT_SELECTOR
        ] = False
        phase6da_direct_negatives.append(
            ("ufsb-selector-expected-effective-mismatch", mismatched_ufsb_selector)
        )

        disabled_bundle_retained_ifsm = json.loads(
            json.dumps(phaseufsb_imse_direct_value)
        )
        for selector_map in disabled_bundle_retained_ifsm["provenance"][
            "selectors"
        ].values():
            selector_map[PHASEIFSM_DIRECT_SELECTOR] = False
        phase6da_direct_negatives.append(
            ("ufsb-imse-retained-ifsm-off", disabled_bundle_retained_ifsm)
        )

        extra_bundle_selector = json.loads(
            json.dumps(phaseufsb_imse_direct_value)
        )
        for selector_map in extra_bundle_selector["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negatives.append(
            ("ufsb-imse-extra-selector", extra_bundle_selector)
        )

        wrong_bundle_parent = json.loads(
            json.dumps(phaseufsb_imse_direct_value)
        )
        wrong_bundle_parent["lineage"]["parent_record_id"] = (
            PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
        )
        phase6da_direct_negatives.append(
            ("ufsb-imse-wrong-parent", wrong_bundle_parent)
        )
        wrong_bundle_generation = json.loads(
            json.dumps(phaseufsb_imse_direct_value)
        )
        wrong_bundle_generation["lineage"]["generation"] = 4
        phase6da_direct_negatives.append(
            ("ufsb-imse-wrong-generation", wrong_bundle_generation)
        )

        for field in (
            "fast_block_unrolled_fixed_signature_batching_requested",
            "fast_block_unrolled_fixed_signature_batching",
            "fast_block_idempotent_mru_store_elision_requested",
            "fast_block_idempotent_mru_store_elision",
        ):
            receipt_off = json.loads(
                json.dumps(phaseufsb_imse_direct_value)
            )
            receipt_off["provenance"]["run_receipt"] = receipt_off[
                "provenance"
            ]["run_receipt"].replace(f"{field}=1", f"{field}=0")
            phase6da_direct_negatives.append(
                (f"ufsb-imse-{field}-receipt-off", receipt_off)
            )

            receipt_missing = json.loads(
                json.dumps(phaseufsb_imse_direct_value)
            )
            receipt_missing["provenance"]["run_receipt"] = receipt_missing[
                "provenance"
            ]["run_receipt"].replace(f" {field}=1", "")
            phase6da_direct_negatives.append(
                (f"ufsb-imse-{field}-receipt-missing", receipt_missing)
            )

        duplicate_bundle_receipt = json.loads(
            json.dumps(phaseufsb_imse_direct_value)
        )
        duplicate_bundle_receipt["provenance"]["run_receipt"] += (
            " fast_block_unrolled_fixed_signature_batching=1"
        )
        phase6da_direct_negatives.append(
            ("ufsb-imse-receipt-duplicate", duplicate_bundle_receipt)
        )

        inexact_bundle_marker = json.loads(
            json.dumps(phaseufsb_imse_direct_value)
        )
        inexact_bundle_marker["lifecycle"]["candidate_marker"].update(
            {"expected_count": 2, "observed_count": 2}
        )
        phase6da_direct_negatives.append(
            ("ufsb-imse-marker-not-exact-once", inexact_bundle_marker)
        )

        masquerading_bundle_record = json.loads(
            json.dumps(phaseufsb_imse_direct_value)
        )
        masquerading_bundle_record["provenance"]["record_id"] = (
            PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
        )
        phase6da_direct_negatives.append(
            ("ufsb-imse-masquerades-as-ifsm", masquerading_bundle_record)
        )

        for value_label, replacement in (("off", False), ("integer", 1)):
            value = json.loads(json.dumps(phasefcfos_direct_value))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[PHASEFCFOS_DIRECT_SELECTOR] = replacement
            phase6da_direct_negatives.append(
                (f"fcfos-selector-{value_label}", value)
            )

        missing_fcfos_selector = json.loads(
            json.dumps(phasefcfos_direct_value)
        )
        for selector_map in missing_fcfos_selector["provenance"][
            "selectors"
        ].values():
            del selector_map[PHASEFCFOS_DIRECT_SELECTOR]
        phase6da_direct_negatives.append(
            ("fcfos-selector-missing", missing_fcfos_selector)
        )

        for sibling_label, sibling_selector in (
            ("ufsb", PHASEUFSB_DIRECT_SELECTOR),
            ("imse", PHASEIMSE_DIRECT_SELECTOR),
        ):
            sibling_selector_present = json.loads(
                json.dumps(phasefcfos_direct_value)
            )
            for selector_map in sibling_selector_present["provenance"][
                "selectors"
            ].values():
                selector_map[sibling_selector] = False
            phase6da_direct_negatives.append(
                (
                    f"fcfos-sibling-{sibling_label}-selector-present",
                    sibling_selector_present,
                )
            )

        wrong_fcfos_parent = json.loads(
            json.dumps(phasefcfos_direct_value)
        )
        wrong_fcfos_parent["lineage"]["parent_record_id"] = (
            PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
        )
        phase6da_direct_negatives.append(
            ("fcfos-wrong-parent", wrong_fcfos_parent)
        )
        wrong_fcfos_generation = json.loads(
            json.dumps(phasefcfos_direct_value)
        )
        wrong_fcfos_generation["lineage"]["generation"] = 4
        phase6da_direct_negatives.append(
            ("fcfos-wrong-generation", wrong_fcfos_generation)
        )

        for field, expected, replacement in (
            (
                "fast_block_callback_free_ordinary_specialization_requested",
                "1",
                "0",
            ),
            (
                "fast_block_callback_free_ordinary_specialization",
                "1",
                "0",
            ),
            (
                "fast_block_unrolled_fixed_signature_batching_requested",
                "0",
                "1",
            ),
            (
                "fast_block_unrolled_fixed_signature_batching",
                "0",
                "1",
            ),
            (
                "fast_block_idempotent_mru_store_elision_requested",
                "0",
                "1",
            ),
            (
                "fast_block_idempotent_mru_store_elision",
                "0",
                "1",
            ),
        ):
            wrong_fcfos_receipt = json.loads(
                json.dumps(phasefcfos_direct_value)
            )
            wrong_fcfos_receipt["provenance"]["run_receipt"] = (
                wrong_fcfos_receipt["provenance"]["run_receipt"].replace(
                    f"{field}={expected}", f"{field}={replacement}"
                )
            )
            phase6da_direct_negatives.append(
                (f"fcfos-{field}-receipt-wrong", wrong_fcfos_receipt)
            )

        missing_fcfos_receipt = json.loads(
            json.dumps(phasefcfos_direct_value)
        )
        missing_fcfos_receipt["provenance"]["run_receipt"] = (
            missing_fcfos_receipt["provenance"]["run_receipt"].replace(
                " fast_block_callback_free_ordinary_specialization_requested=1",
                "",
            )
        )
        phase6da_direct_negatives.append(
            ("fcfos-receipt-missing", missing_fcfos_receipt)
        )

        inexact_fcfos_marker = json.loads(
            json.dumps(phasefcfos_direct_value)
        )
        inexact_fcfos_marker["lifecycle"]["candidate_marker"].update(
            {"coverage_positive": False}
        )
        phase6da_direct_negatives.append(
            ("fcfos-marker-not-positive", inexact_fcfos_marker)
        )

        reordered_fcfos_marker = json.loads(
            json.dumps(phasefcfos_direct_value)
        )
        reordered_fcfos_marker["lifecycle"]["candidate_marker"]["line"] = (
            PHASEFCFOS_DIRECT_CANDIDATE_MARKER_LINE.replace(
                "reentry_safe=positive snapshot_elision=positive",
                "snapshot_elision=positive reentry_safe=positive",
            )
        )
        phase6da_direct_negatives.append(
            ("fcfos-marker-line-reordered", reordered_fcfos_marker)
        )

        masquerading_fcfos_record = json.loads(
            json.dumps(phasefcfos_direct_value)
        )
        masquerading_fcfos_record["provenance"]["record_id"] = (
            PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
        )
        phase6da_direct_negatives.append(
            ("fcfos-masquerades-as-ifsm", masquerading_fcfos_record)
        )

        for label, value in phase6da_direct_negatives:
            path = phase6da_direct_negative_root / f"{label}.json"
            _write_fixture(path, value)
            _expect_failure(
                lambda path=path: load_manifest(path),
                f"invalid phase-6DA direct record {label}",
            )

        # Isolate the IFSM selector as the only recognized candidate signal.
        # Without selector-driven direct scoping this malformed g2 manifest
        # bypassed both the direct and projection contracts and loaded as valid.
        unregistered_ifsm_selector_record = json.loads(
            json.dumps(phaseifsm_direct_value)
        )
        unregistered_ifsm_selector_record["provenance"]["record_id"] = (
            "ifsm-c-unregistered-record-g000002"
        )
        for selector_map in unregistered_ifsm_selector_record["provenance"][
            "selectors"
        ].values():
            selector_map.clear()
            selector_map[PHASEIFSM_DIRECT_SELECTOR] = True
        unregistered_ifsm_selector_path = (
            phase6da_direct_negative_root
            / "ifsm-selector-unregistered-record.json"
        )
        _write_fixture(
            unregistered_ifsm_selector_path,
            unregistered_ifsm_selector_record,
        )
        _expect_failure_containing(
            lambda: load_manifest(unregistered_ifsm_selector_path),
            "an IFSM selector outside its preregistered g2 record",
            "IFSM direct selector is outside its preregistered g2 record",
        )

        unregistered_fcfos_selector_record = json.loads(
            json.dumps(phasefcfos_direct_value)
        )
        unregistered_fcfos_selector_record["provenance"]["record_id"] = (
            "fcfos-c-unregistered-record-g000003"
        )
        for selector_map in unregistered_fcfos_selector_record["provenance"][
            "selectors"
        ].values():
            selector_map.clear()
            selector_map[PHASEFCFOS_DIRECT_SELECTOR] = True
        unregistered_fcfos_selector_path = (
            phase6da_direct_negative_root
            / "fcfos-selector-unregistered-record.json"
        )
        _write_fixture(
            unregistered_fcfos_selector_path,
            unregistered_fcfos_selector_record,
        )
        _expect_failure_containing(
            lambda: load_manifest(unregistered_fcfos_selector_path),
            "an FCFOS selector outside its preregistered g3 record",
            "FCFOS direct selector is outside its preregistered g3 record",
        )

        unregistered_alc_selector_record = json.loads(
            json.dumps(phasealc_direct_value)
        )
        unregistered_alc_selector_record["provenance"]["record_id"] = (
            "alc-c-unregistered-record-g000003"
        )
        for selector_map in unregistered_alc_selector_record["provenance"][
            "selectors"
        ].values():
            selector_map.clear()
            selector_map[PHASEALC_DIRECT_SELECTOR] = True
        unregistered_alc_selector_path = (
            phase6da_direct_negative_root
            / "alc-selector-unregistered-record.json"
        )
        _write_fixture(
            unregistered_alc_selector_path,
            unregistered_alc_selector_record,
        )
        _expect_failure_containing(
            lambda: load_manifest(unregistered_alc_selector_path),
            "an ALC selector outside its preregistered g3 record",
            "ALC direct selector is outside its preregistered g3 record",
        )

        for selector_label, selector_name in (
            ("ufsb", PHASEUFSB_DIRECT_SELECTOR),
            ("imse", PHASEIMSE_DIRECT_SELECTOR),
        ):
            unregistered_bundle_selector_record = json.loads(
                json.dumps(phaseufsb_imse_direct_value)
            )
            unregistered_bundle_selector_record["provenance"]["record_id"] = (
                f"{selector_label}-c-unregistered-record-g000003"
            )
            for selector_map in unregistered_bundle_selector_record[
                "provenance"
            ]["selectors"].values():
                selector_map.clear()
                selector_map[selector_name] = True
            unregistered_bundle_selector_path = (
                phase6da_direct_negative_root
                / f"{selector_label}-selector-unregistered-record.json"
            )
            _write_fixture(
                unregistered_bundle_selector_path,
                unregistered_bundle_selector_record,
            )
            _expect_failure_containing(
                lambda path=unregistered_bundle_selector_path: load_manifest(
                    path
                ),
                (
                    f"a {selector_label.upper()} selector outside its "
                    "preregistered combined g3 record"
                ),
                (
                    "UFSB/IMSE direct selector is outside its "
                    "preregistered combined g3 record"
                ),
            )

        for filename in CONFIGURATION_PROJECTION_ARTIFACTS.values():
            (phase6da_direct_candidate_root / filename).write_text(
                "spurious direct projection artifact\n", encoding="ascii"
            )
        try:
            _expect_failure(
                lambda: load_manifest(phase6da_direct_candidate_path),
                "physical projection artifacts on a phase-6DA direct record",
            )
        finally:
            for filename in CONFIGURATION_PROJECTION_ARTIFACTS.values():
                (phase6da_direct_candidate_root / filename).unlink()

        different_macho_value = json.loads(
            json.dumps(phase6da_direct_candidate_value)
        )
        different_macho_value["provenance"]["binary_sha256"] = "d" * 64
        different_macho_path = (
            phase6da_direct_candidate_root / "different-macho.json"
        )
        _write_fixture(different_macho_path, different_macho_value)
        different_macho_candidate = load_manifest(different_macho_path)
        _expect_failure_containing(
            lambda: build_report(
                phase6da_direct_baseline, different_macho_candidate
            ),
            "an initial phase-6DA pair using different Mach-O images",
            "same Mach-O SHA-256",
        )

        projection_root = root / "projection-record"
        projection_root.mkdir()
        projected_fixture = b"fixture-cache-prefix\nfixture-cache-suffix\n"
        raw_fixture = (
            b"fixture-cache-prefix\n"
            + CONFIGURATION_PROJECTION_REMOVED_SEGMENT
            + b"fixture-cache-suffix\n"
        )
        projection_contract = dict(CONFIGURATION_PROJECTION_CONTRACT)
        projection_contract.update(
            {
                "raw_bytes": len(raw_fixture),
                "raw_sha256": hashlib.sha256(raw_fixture).hexdigest(),
                "projected_bytes": len(projected_fixture),
                "projected_sha256": hashlib.sha256(projected_fixture).hexdigest(),
            }
        )
        projection_value = _fixture_manifest(
            record_id=CONFIGURATION_PROJECTION_RECORD_ID,
            receipt="projection-receipt",
            generation=2,
            parent="6cl-c-stable-negative-g000001",
            role="candidate",
            wall_ns=300_000_000_000,
            cumulative_ms=300_000,
            fort_interval_ms=300,
            game_interval_ms=150,
            revision_digit="3",
            selector_enabled=True,
        )
        projection_contract.update(
            {
                "protocol_id": projection_value["protocol"]["protocol_id"],
                "record_id": projection_value["provenance"]["record_id"],
                "generation": projection_value["lineage"]["generation"],
                "parent_record_id": projection_value["lineage"][
                    "parent_record_id"
                ],
                "original_role": projection_value["lineage"]["original_role"],
            }
        )
        projection_value["comparability"]["runtime"][
            "configuration_sha256"
        ] = projection_contract["projected_sha256"]
        projection_selectors = dict(projection_contract["selector_bundle"])
        projection_value["provenance"]["selectors"] = {
            "expected": dict(projection_selectors),
            "effective": dict(projection_selectors),
        }
        projection_receipt = {
            "schema": {
                "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
                "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
            },
            "attestation": CONFIGURATION_PROJECTION_ATTESTATION,
            "policy": CONFIGURATION_PROJECTION_POLICY,
            "mode": CONFIGURATION_PROJECTION_MODE,
            "record_id": projection_contract["record_id"],
            "candidate_selector": projection_contract["candidate_selector"],
            "raw_bytes": projection_contract["raw_bytes"],
            "raw_sha256": projection_contract["raw_sha256"],
            "phase6cm_block_sha256": projection_contract[
                "phase6cm_block_sha256"
            ],
            "phase6co_block_sha256": projection_contract[
                "phase6co_block_sha256"
            ],
            "removed_bytes": projection_contract["removed_bytes"],
            "removed_segment_sha256": projection_contract[
                "removed_segment_sha256"
            ],
            "projected_bytes": projection_contract["projected_bytes"],
            "projected_sha256": projection_contract["projected_sha256"],
            "epoch_configuration_sha256": projection_contract[
                "projected_sha256"
            ],
        }
        projection_raw_path = (
            projection_root / CONFIGURATION_PROJECTION_RAW_NAME
        )
        projection_receipt_path = (
            projection_root / CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        projection_raw_path.write_bytes(raw_fixture)
        projection_raw_path.chmod(0o444)
        _write_fixture(projection_receipt_path, projection_receipt)
        projection_receipt_path.chmod(0o444)
        projection_artifacts = projection_value["provenance"][
            "artifacts_sha256"
        ]
        projection_artifacts["configuration_cache_raw"] = hashlib.sha256(
            raw_fixture
        ).hexdigest()
        projection_artifacts[
            "configuration_projection_receipt"
        ] = hashlib.sha256(projection_receipt_path.read_bytes()).hexdigest()
        projection_manifest_path = projection_root / "manifest.json"
        _write_fixture(projection_manifest_path, projection_value)
        load_manifest(
            projection_manifest_path,
            _projection_contract=projection_contract,
        )
        load_manifest(
            projection_manifest_path,
            _projection_contracts=(
                projection_contract,
                PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT,
            ),
        )

        synthetic_g3_root = root / "synthetic-g3-projection-record"
        synthetic_g3_root.mkdir()
        synthetic_g3_raw = (
            b"fixture-phase6cq-prefix\n"
            + b"\nfixture-operation-boundary\n".join(
                current
                for _, current, _ in PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS
            )
            + b"\n"
            + CONFIGURATION_PROJECTION_REMOVED_SEGMENT
            + b"fixture-phase6cq-suffix\n"
        )
        synthetic_g3_restored = synthetic_g3_raw
        for _, current, restored in (
            PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS
        ):
            _require(
                synthetic_g3_restored.count(current) == 1
                and synthetic_g3_restored.count(restored) == 0,
                "self-test",
                "synthetic phase-6CQ restore fixture is not exact-once",
            )
            synthetic_g3_restored = synthetic_g3_restored.replace(
                current, restored, 1
            )
        _require(
            synthetic_g3_restored.count(
                CONFIGURATION_PROJECTION_REMOVED_SEGMENT
            )
            == 1,
            "self-test",
            "synthetic phase-6CQ restored bytes lack the exact strip segment",
        )
        synthetic_g3_projected = synthetic_g3_restored.replace(
            CONFIGURATION_PROJECTION_REMOVED_SEGMENT, b"", 1
        )
        synthetic_g3_contract = dict(
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT
        )
        synthetic_g3_contract.update(
            {
                "protocol_id": projection_value["protocol"]["protocol_id"],
                "raw_bytes": len(synthetic_g3_raw),
                "raw_sha256": hashlib.sha256(synthetic_g3_raw).hexdigest(),
                "restored_bytes": len(synthetic_g3_restored),
                "restored_sha256": hashlib.sha256(
                    synthetic_g3_restored
                ).hexdigest(),
                "projected_bytes": len(synthetic_g3_projected),
                "projected_sha256": hashlib.sha256(
                    synthetic_g3_projected
                ).hexdigest(),
            }
        )
        _require(
            _phase6cq_restore_configuration(
                synthetic_g3_raw,
                pathlib.Path("self-test synthetic phase-6CQ raw"),
                synthetic_g3_contract,
            )
            == synthetic_g3_restored,
            "self-test",
            "synthetic phase-6CQ restore did not reproduce its restored bytes",
        )
        synthetic_g3_value = json.loads(json.dumps(projection_value))
        synthetic_g3_value["provenance"][
            "record_id"
        ] = PHASE6CQ_CONFIGURATION_PROJECTION_RECORD_ID
        synthetic_g3_value["lineage"] = {
            "generation": 3,
            "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
            "original_role": "candidate",
        }
        synthetic_g3_selectors = dict(
            synthetic_g3_contract["selector_bundle"]
        )
        synthetic_g3_value["provenance"]["selectors"] = {
            "expected": dict(synthetic_g3_selectors),
            "effective": dict(synthetic_g3_selectors),
        }
        synthetic_g3_value["comparability"]["runtime"][
            "configuration_sha256"
        ] = synthetic_g3_contract["projected_sha256"]
        synthetic_g3_receipt = {
            "schema": {
                "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
                "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
            },
            "attestation": synthetic_g3_contract["attestation"],
            "policy": synthetic_g3_contract["policy"],
            "mode": synthetic_g3_contract["mode"],
            "record_id": synthetic_g3_contract["record_id"],
            "candidate_selector": synthetic_g3_contract[
                "candidate_selector"
            ],
            "raw_bytes": synthetic_g3_contract["raw_bytes"],
            "raw_sha256": synthetic_g3_contract["raw_sha256"],
            "restore_operation_count": synthetic_g3_contract[
                "restore_operation_count"
            ],
            "restore_operations_contract_bytes": synthetic_g3_contract[
                "restore_operations_contract_bytes"
            ],
            "restore_operations_contract_sha256": synthetic_g3_contract[
                "restore_operations_contract_sha256"
            ],
            "restored_bytes": synthetic_g3_contract[
                "restored_bytes"
            ],
            "restored_sha256": synthetic_g3_contract[
                "restored_sha256"
            ],
            "phase6cm_block_sha256": synthetic_g3_contract[
                "phase6cm_block_sha256"
            ],
            "phase6co_block_sha256": synthetic_g3_contract[
                "phase6co_block_sha256"
            ],
            "removed_bytes": synthetic_g3_contract["removed_bytes"],
            "removed_segment_sha256": synthetic_g3_contract[
                "removed_segment_sha256"
            ],
            "projected_bytes": synthetic_g3_contract["projected_bytes"],
            "projected_sha256": synthetic_g3_contract["projected_sha256"],
            "epoch_configuration_sha256": synthetic_g3_contract[
                "projected_sha256"
            ],
        }
        synthetic_g3_raw_path = (
            synthetic_g3_root / CONFIGURATION_PROJECTION_RAW_NAME
        )
        synthetic_g3_receipt_path = (
            synthetic_g3_root / CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        synthetic_g3_raw_path.write_bytes(synthetic_g3_raw)
        synthetic_g3_raw_path.chmod(0o444)
        _write_fixture(synthetic_g3_receipt_path, synthetic_g3_receipt)
        synthetic_g3_receipt_path.chmod(0o444)
        synthetic_g3_artifacts = synthetic_g3_value["provenance"][
            "artifacts_sha256"
        ]
        synthetic_g3_artifacts["configuration_cache_raw"] = hashlib.sha256(
            synthetic_g3_raw_path.read_bytes()
        ).hexdigest()
        synthetic_g3_artifacts[
            "configuration_projection_receipt"
        ] = hashlib.sha256(synthetic_g3_receipt_path.read_bytes()).hexdigest()
        synthetic_g3_manifest_path = synthetic_g3_root / "manifest.json"
        _write_fixture(synthetic_g3_manifest_path, synthetic_g3_value)
        load_manifest(
            synthetic_g3_manifest_path,
            _projection_contracts=(projection_contract, synthetic_g3_contract),
        )

        synthetic_6cs_contract = dict(
            PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT
        )
        synthetic_6cs_contract.update(
            {
                "protocol_id": projection_value["protocol"]["protocol_id"],
                "raw_bytes": len(synthetic_g3_raw),
                "raw_sha256": hashlib.sha256(synthetic_g3_raw).hexdigest(),
                "restored_bytes": len(synthetic_g3_restored),
                "restored_sha256": hashlib.sha256(
                    synthetic_g3_restored
                ).hexdigest(),
                "projected_bytes": len(synthetic_g3_projected),
                "projected_sha256": hashlib.sha256(
                    synthetic_g3_projected
                ).hexdigest(),
            }
        )
        _require(
            _phase6cs_restore_configuration(
                synthetic_g3_raw,
                pathlib.Path("self-test synthetic phase-6CS raw"),
                synthetic_6cs_contract,
            )
            == synthetic_g3_restored,
            "self-test",
            "synthetic phase-6CS restore did not reproduce its restored bytes",
        )
        synthetic_6cs_root = root / "synthetic-6cs-projection-record"
        synthetic_6cs_root.mkdir()
        synthetic_6cs_value = json.loads(json.dumps(projection_value))
        synthetic_6cs_value["provenance"][
            "record_id"
        ] = PHASE6CS_CONFIGURATION_PROJECTION_RECORD_ID
        synthetic_6cs_value["lineage"] = {
            "generation": 3,
            "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
            "original_role": "candidate",
        }
        synthetic_6cs_selectors = dict(
            synthetic_6cs_contract["selector_bundle"]
        )
        synthetic_6cs_value["provenance"]["selectors"] = {
            "expected": dict(synthetic_6cs_selectors),
            "effective": dict(synthetic_6cs_selectors),
        }
        synthetic_6cs_value["comparability"]["runtime"][
            "configuration_sha256"
        ] = synthetic_6cs_contract["projected_sha256"]
        synthetic_6cs_receipt = {
            "schema": {
                "name": CONFIGURATION_PROJECTION_SCHEMA_NAME,
                "version": CONFIGURATION_PROJECTION_SCHEMA_VERSION,
            },
            "attestation": synthetic_6cs_contract["attestation"],
            "policy": synthetic_6cs_contract["policy"],
            "mode": synthetic_6cs_contract["mode"],
            "record_id": synthetic_6cs_contract["record_id"],
            "candidate_selector": synthetic_6cs_contract[
                "candidate_selector"
            ],
            "raw_bytes": synthetic_6cs_contract["raw_bytes"],
            "raw_sha256": synthetic_6cs_contract["raw_sha256"],
            "restore_operation_count": synthetic_6cs_contract[
                "restore_operation_count"
            ],
            "restore_operations_contract_bytes": synthetic_6cs_contract[
                "restore_operations_contract_bytes"
            ],
            "restore_operations_contract_sha256": synthetic_6cs_contract[
                "restore_operations_contract_sha256"
            ],
            "restored_bytes": synthetic_6cs_contract["restored_bytes"],
            "restored_sha256": synthetic_6cs_contract["restored_sha256"],
            "phase6cm_block_sha256": synthetic_6cs_contract[
                "phase6cm_block_sha256"
            ],
            "phase6co_block_sha256": synthetic_6cs_contract[
                "phase6co_block_sha256"
            ],
            "removed_bytes": synthetic_6cs_contract["removed_bytes"],
            "removed_segment_sha256": synthetic_6cs_contract[
                "removed_segment_sha256"
            ],
            "projected_bytes": synthetic_6cs_contract["projected_bytes"],
            "projected_sha256": synthetic_6cs_contract["projected_sha256"],
            "epoch_configuration_sha256": synthetic_6cs_contract[
                "projected_sha256"
            ],
        }
        synthetic_6cs_raw_path = (
            synthetic_6cs_root / CONFIGURATION_PROJECTION_RAW_NAME
        )
        synthetic_6cs_receipt_path = (
            synthetic_6cs_root / CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        synthetic_6cs_raw_path.write_bytes(synthetic_g3_raw)
        synthetic_6cs_raw_path.chmod(0o444)
        _write_fixture(synthetic_6cs_receipt_path, synthetic_6cs_receipt)
        synthetic_6cs_receipt_path.chmod(0o444)
        synthetic_6cs_artifacts = synthetic_6cs_value["provenance"][
            "artifacts_sha256"
        ]
        synthetic_6cs_artifacts["configuration_cache_raw"] = hashlib.sha256(
            synthetic_6cs_raw_path.read_bytes()
        ).hexdigest()
        synthetic_6cs_artifacts[
            "configuration_projection_receipt"
        ] = hashlib.sha256(synthetic_6cs_receipt_path.read_bytes()).hexdigest()
        synthetic_6cs_manifest_path = synthetic_6cs_root / "manifest.json"
        _write_fixture(synthetic_6cs_manifest_path, synthetic_6cs_value)
        synthetic_projection_contracts = (
            projection_contract,
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT,
            synthetic_6cs_contract,
        )
        load_manifest(
            synthetic_6cs_manifest_path,
            _projection_contracts=synthetic_projection_contracts,
        )

        missing_6cs_artifacts_value = json.loads(
            json.dumps(synthetic_6cs_value)
        )
        for artifact_name in CONFIGURATION_PROJECTION_ARTIFACTS:
            del missing_6cs_artifacts_value["provenance"]["artifacts_sha256"][
                artifact_name
            ]
        missing_6cs_artifacts_path = (
            synthetic_6cs_root / "missing-projection-artifacts.json"
        )
        _write_fixture(missing_6cs_artifacts_path, missing_6cs_artifacts_value)
        _expect_failure_containing(
            lambda: load_manifest(
                missing_6cs_artifacts_path,
                _projection_contracts=synthetic_projection_contracts,
            ),
            "a 6CS record without sealed projection artifacts",
            "manifest is missing configuration projection artifacts",
        )

        for scalar_state in (False, True):
            mixed_6cq_6cs_value = json.loads(json.dumps(synthetic_6cs_value))
            for selector_map in mixed_6cq_6cs_value["provenance"][
                "selectors"
            ].values():
                selector_map[
                    PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR
                ] = scalar_state
            mixed_6cq_6cs_path = (
                synthetic_6cs_root / f"mixed-6cq-6cs-{scalar_state}.json"
            )
            _write_fixture(mixed_6cq_6cs_path, mixed_6cq_6cs_value)
            _expect_failure_containing(
                lambda path=mixed_6cq_6cs_path: load_manifest(
                    path,
                    _projection_contracts=synthetic_projection_contracts,
                ),
                f"a mixed 6CQ+6CS selector bundle with scalar={scalar_state}",
                "record and candidate-selector signals conflict",
            )

        wrong_6cs_parent_value = json.loads(json.dumps(synthetic_6cs_value))
        wrong_6cs_parent_value["lineage"]["parent_record_id"] = "wrong-parent"
        wrong_6cs_parent_path = synthetic_6cs_root / "wrong-parent.json"
        _write_fixture(wrong_6cs_parent_path, wrong_6cs_parent_value)
        _expect_failure_containing(
            lambda: load_manifest(
                wrong_6cs_parent_path,
                _projection_contracts=synthetic_projection_contracts,
            ),
            "a 6CS record with the wrong parent",
            "outside its exact protocol, lineage, and typed selector-bundle scope",
        )

        cross_family_6cs_receipt = dict(synthetic_6cs_receipt)
        cross_family_6cs_receipt["attestation"] = (
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT["attestation"]
        )
        synthetic_6cs_receipt_path.chmod(0o644)
        _write_fixture(synthetic_6cs_receipt_path, cross_family_6cs_receipt)
        synthetic_6cs_receipt_path.chmod(0o444)
        cross_family_6cs_value = json.loads(json.dumps(synthetic_6cs_value))
        cross_family_6cs_value["provenance"]["artifacts_sha256"][
            "configuration_projection_receipt"
        ] = hashlib.sha256(synthetic_6cs_receipt_path.read_bytes()).hexdigest()
        cross_family_6cs_path = synthetic_6cs_root / "cross-family-receipt.json"
        _write_fixture(cross_family_6cs_path, cross_family_6cs_value)
        try:
            _expect_failure_containing(
                lambda: load_manifest(
                    cross_family_6cs_path,
                    _projection_contracts=synthetic_projection_contracts,
                ),
                "a 6CS receipt carrying the 6CQ attestation",
                "receipt differs from the preregistered contract",
            )
        finally:
            synthetic_6cs_receipt_path.chmod(0o644)
            _write_fixture(synthetic_6cs_receipt_path, synthetic_6cs_receipt)
            synthetic_6cs_receipt_path.chmod(0o444)

        synthetic_6da_contract = dict(
            PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT
        )
        synthetic_6da_contract.update(
            {
                "protocol_id": projection_value["protocol"]["protocol_id"],
                "raw_bytes": len(synthetic_g3_raw),
                "raw_sha256": hashlib.sha256(synthetic_g3_raw).hexdigest(),
                "restored_bytes": len(synthetic_g3_restored),
                "restored_sha256": hashlib.sha256(
                    synthetic_g3_restored
                ).hexdigest(),
                "projected_bytes": len(synthetic_g3_projected),
                "projected_sha256": hashlib.sha256(
                    synthetic_g3_projected
                ).hexdigest(),
            }
        )
        _require(
            _phase6da_restore_configuration(
                synthetic_g3_raw,
                pathlib.Path("self-test synthetic phase-6DA raw"),
                synthetic_6da_contract,
            )
            == synthetic_g3_restored,
            "self-test",
            "synthetic phase-6DA restore did not reproduce its restored bytes",
        )
        synthetic_6da_root = root / "synthetic-6da-projection-record"
        synthetic_6da_root.mkdir()
        synthetic_6da_value = json.loads(json.dumps(projection_value))
        synthetic_6da_value["provenance"][
            "record_id"
        ] = PHASE6DA_CONFIGURATION_PROJECTION_RECORD_ID
        synthetic_6da_value["lineage"] = {
            "generation": 3,
            "parent_record_id": CONFIGURATION_PROJECTION_RECORD_ID,
            "original_role": "candidate",
        }
        synthetic_6da_selectors = dict(
            synthetic_6da_contract["selector_bundle"]
        )
        synthetic_6da_value["provenance"]["selectors"] = {
            "expected": dict(synthetic_6da_selectors),
            "effective": dict(synthetic_6da_selectors),
        }
        synthetic_6da_value["comparability"]["runtime"][
            "configuration_sha256"
        ] = synthetic_6da_contract["projected_sha256"]
        synthetic_6da_receipt = dict(synthetic_6cs_receipt)
        synthetic_6da_receipt.update(
            {
                "attestation": synthetic_6da_contract["attestation"],
                "policy": synthetic_6da_contract["policy"],
                "mode": synthetic_6da_contract["mode"],
                "record_id": synthetic_6da_contract["record_id"],
                "candidate_selector": synthetic_6da_contract[
                    "candidate_selector"
                ],
                "restore_operations_contract_bytes": synthetic_6da_contract[
                    "restore_operations_contract_bytes"
                ],
                "restore_operations_contract_sha256": synthetic_6da_contract[
                    "restore_operations_contract_sha256"
                ],
            }
        )
        synthetic_6da_raw_path = (
            synthetic_6da_root / CONFIGURATION_PROJECTION_RAW_NAME
        )
        synthetic_6da_receipt_path = (
            synthetic_6da_root / CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        synthetic_6da_raw_path.write_bytes(synthetic_g3_raw)
        synthetic_6da_raw_path.chmod(0o444)
        _write_fixture(synthetic_6da_receipt_path, synthetic_6da_receipt)
        synthetic_6da_receipt_path.chmod(0o444)
        synthetic_6da_artifacts = synthetic_6da_value["provenance"][
            "artifacts_sha256"
        ]
        synthetic_6da_artifacts["configuration_cache_raw"] = hashlib.sha256(
            synthetic_6da_raw_path.read_bytes()
        ).hexdigest()
        synthetic_6da_artifacts[
            "configuration_projection_receipt"
        ] = hashlib.sha256(synthetic_6da_receipt_path.read_bytes()).hexdigest()
        synthetic_6da_manifest_path = synthetic_6da_root / "manifest.json"
        _write_fixture(synthetic_6da_manifest_path, synthetic_6da_value)
        synthetic_6da_projection_contracts = (
            projection_contract,
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT,
            synthetic_6cs_contract,
            synthetic_6da_contract,
        )
        load_manifest(
            synthetic_6da_manifest_path,
            _projection_contracts=synthetic_6da_projection_contracts,
        )

        phase6da_scope_negatives = []
        for label, replacement in (("false", False), ("integer", 1)):
            value = json.loads(json.dumps(synthetic_6da_value))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[
                    PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR
                ] = replacement
            phase6da_scope_negatives.append(
                (f"candidate-selector-{label}", value)
            )
        missing_6da_selector = json.loads(json.dumps(synthetic_6da_value))
        for selector_map in missing_6da_selector["provenance"][
            "selectors"
        ].values():
            del selector_map[PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR]
        phase6da_scope_negatives.append(
            ("missing-candidate-selector", missing_6da_selector)
        )
        for retired_selector in (
            PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR,
            PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR,
        ):
            for retired_state in (False, True):
                value = json.loads(json.dumps(synthetic_6da_value))
                for selector_map in value["provenance"][
                    "selectors"
                ].values():
                    selector_map[retired_selector] = retired_state
                phase6da_scope_negatives.append(
                    (
                        f"mixed-{retired_selector}-{retired_state}",
                        value,
                    )
                )
        wrong_6da_parent = json.loads(json.dumps(synthetic_6da_value))
        wrong_6da_parent["lineage"]["parent_record_id"] = "wrong-parent"
        phase6da_scope_negatives.append(("wrong-parent", wrong_6da_parent))
        wrong_6da_generation = json.loads(json.dumps(synthetic_6da_value))
        wrong_6da_generation["lineage"]["generation"] = 4
        phase6da_scope_negatives.append(
            ("wrong-generation-g000004", wrong_6da_generation)
        )
        reused_6cs_identity = json.loads(json.dumps(synthetic_6da_value))
        reused_6cs_identity["provenance"]["record_id"] = (
            PHASE6CS_CONFIGURATION_PROJECTION_RECORD_ID
        )
        phase6da_scope_negatives.append(
            ("reused-6CS-record", reused_6cs_identity)
        )
        for label, value in phase6da_scope_negatives:
            path = synthetic_6da_root / f"negative-{label}.json"
            _write_fixture(path, value)
            _expect_failure(
                lambda path=path: load_manifest(
                    path,
                    _projection_contracts=synthetic_6da_projection_contracts,
                ),
                f"invalid phase-6DA scope {label}",
            )

        missing_6da_artifacts = json.loads(json.dumps(synthetic_6da_value))
        for artifact_name in CONFIGURATION_PROJECTION_ARTIFACTS:
            del missing_6da_artifacts["provenance"]["artifacts_sha256"][
                artifact_name
            ]
        missing_6da_artifacts_path = (
            synthetic_6da_root / "missing-projection-artifacts.json"
        )
        _write_fixture(missing_6da_artifacts_path, missing_6da_artifacts)
        _expect_failure_containing(
            lambda: load_manifest(
                missing_6da_artifacts_path,
                _projection_contracts=synthetic_6da_projection_contracts,
            ),
            "a 6DA record without sealed projection artifacts",
            "manifest is missing configuration projection artifacts",
        )

        cross_family_6da_receipt = dict(synthetic_6da_receipt)
        cross_family_6da_receipt["attestation"] = synthetic_6cs_contract[
            "attestation"
        ]
        synthetic_6da_receipt_path.chmod(0o644)
        _write_fixture(synthetic_6da_receipt_path, cross_family_6da_receipt)
        synthetic_6da_receipt_path.chmod(0o444)
        cross_family_6da_value = json.loads(json.dumps(synthetic_6da_value))
        cross_family_6da_value["provenance"]["artifacts_sha256"][
            "configuration_projection_receipt"
        ] = hashlib.sha256(synthetic_6da_receipt_path.read_bytes()).hexdigest()
        cross_family_6da_path = synthetic_6da_root / "cross-family-receipt.json"
        _write_fixture(cross_family_6da_path, cross_family_6da_value)
        try:
            _expect_failure_containing(
                lambda: load_manifest(
                    cross_family_6da_path,
                    _projection_contracts=synthetic_6da_projection_contracts,
                ),
                "a 6DA receipt carrying the 6CS attestation",
                "receipt differs from the preregistered contract",
            )
        finally:
            synthetic_6da_receipt_path.chmod(0o644)
            _write_fixture(synthetic_6da_receipt_path, synthetic_6da_receipt)
            synthetic_6da_receipt_path.chmod(0o444)

        wrong_6da_restore_digest_contract = dict(synthetic_6da_contract)
        wrong_6da_restore_digest_contract[
            "restore_operations_contract_sha256"
        ] = "0" * 64
        _expect_failure_containing(
            lambda: _phase6da_restore_configuration(
                synthetic_g3_raw,
                pathlib.Path("self-test wrong phase-6DA operations digest"),
                wrong_6da_restore_digest_contract,
            ),
            "a wrong phase-6DA operations digest",
            "compiled phase-6DA restore operations differ",
        )

        tampered_g3_raw = synthetic_g3_raw.replace(
            b"fixture-phase6cq-prefix", b"Fixture-phase6cq-prefix", 1
        )
        tampered_g3_raw_value = json.loads(json.dumps(synthetic_g3_value))
        tampered_g3_raw_value["provenance"]["artifacts_sha256"][
            "configuration_cache_raw"
        ] = hashlib.sha256(tampered_g3_raw).hexdigest()
        tampered_g3_raw_manifest = synthetic_g3_root / "tampered-raw.json"
        _write_fixture(tampered_g3_raw_manifest, tampered_g3_raw_value)
        synthetic_g3_raw_path.chmod(0o644)
        synthetic_g3_raw_path.write_bytes(tampered_g3_raw)
        synthetic_g3_raw_path.chmod(0o444)
        try:
            _expect_failure_containing(
                lambda: load_manifest(
                    tampered_g3_raw_manifest,
                    _projection_contract=synthetic_g3_contract,
                ),
                "a phase-6CQ raw snapshot with the wrong preregistered bytes",
                "raw snapshot differs from the preregistered bytes",
            )
        finally:
            synthetic_g3_raw_path.chmod(0o644)
            synthetic_g3_raw_path.write_bytes(synthetic_g3_raw)
            synthetic_g3_raw_path.chmod(0o444)

        tampered_g3_receipt = dict(synthetic_g3_receipt)
        tampered_g3_receipt["restore_operation_count"] = 7
        synthetic_g3_receipt_path.chmod(0o644)
        _write_fixture(synthetic_g3_receipt_path, tampered_g3_receipt)
        synthetic_g3_receipt_path.chmod(0o444)
        tampered_g3_receipt_value = json.loads(json.dumps(synthetic_g3_value))
        tampered_g3_receipt_value["provenance"]["artifacts_sha256"][
            "configuration_projection_receipt"
        ] = hashlib.sha256(synthetic_g3_receipt_path.read_bytes()).hexdigest()
        tampered_g3_receipt_manifest = (
            synthetic_g3_root / "tampered-receipt.json"
        )
        _write_fixture(tampered_g3_receipt_manifest, tampered_g3_receipt_value)
        try:
            _expect_failure_containing(
                lambda: load_manifest(
                    tampered_g3_receipt_manifest,
                    _projection_contract=synthetic_g3_contract,
                ),
                "a semantically altered phase-6CQ projection receipt",
                "receipt differs from the preregistered contract",
            )
        finally:
            synthetic_g3_receipt_path.chmod(0o644)
            _write_fixture(synthetic_g3_receipt_path, synthetic_g3_receipt)
            synthetic_g3_receipt_path.chmod(0o444)

        missing_restore_source = synthetic_g3_raw.replace(
            PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS[0][1], b"", 1
        )
        duplicate_restore_source = (
            synthetic_g3_raw
            + PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS[0][1]
        )
        _expect_failure_containing(
            lambda: _phase6cq_restore_configuration(
                synthetic_g3_restored,
                pathlib.Path("self-test pre-restored phase-6CQ raw"),
                synthetic_g3_contract,
            ),
            "a pre-restored phase-6CQ input",
            "pre-restored input is forbidden",
        )
        _expect_failure_containing(
            lambda: _phase6cq_restore_configuration(
                missing_restore_source,
                pathlib.Path("self-test missing phase-6CQ restore source"),
                synthetic_g3_contract,
            ),
            "a missing phase-6CQ restore source",
            "source literal must be exact-once, observed 0",
        )
        _expect_failure_containing(
            lambda: _phase6cq_restore_configuration(
                duplicate_restore_source,
                pathlib.Path("self-test duplicate phase-6CQ restore source"),
                synthetic_g3_contract,
            ),
            "a duplicate phase-6CQ restore source",
            "source literal must be exact-once, observed 2",
        )
        wrong_restore_digest_contract = dict(synthetic_g3_contract)
        wrong_restore_digest_contract[
            "restore_operations_contract_sha256"
        ] = "0" * 64
        _expect_failure_containing(
            lambda: _phase6cq_restore_configuration(
                synthetic_g3_raw,
                pathlib.Path("self-test wrong phase-6CQ operations digest"),
                wrong_restore_digest_contract,
            ),
            "a wrong phase-6CQ operations digest",
            "compiled phase-6CQ restore operations differ",
        )
        wrong_restored_contract = dict(synthetic_g3_contract)
        wrong_restored_contract["restored_sha256"] = "0" * 64
        _expect_failure_containing(
            lambda: _phase6cq_restore_configuration(
                synthetic_g3_raw,
                pathlib.Path("self-test wrong phase-6CQ restored bytes"),
                wrong_restored_contract,
            ),
            "a wrong phase-6CQ restored digest",
            "restored configuration does not reproduce",
        )

        g3_signal_root = root / "production-g3-signal-no-artifacts"
        g3_signal_root.mkdir()
        g3_signal_value = json.loads(json.dumps(synthetic_g3_value))
        g3_signal_value["protocol"]["protocol_id"] = (
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT["protocol_id"]
        )
        g3_signal_value["comparability"]["runtime"][
            "configuration_sha256"
        ] = PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT["projected_sha256"]
        for artifact_name in CONFIGURATION_PROJECTION_ARTIFACTS:
            del g3_signal_value["provenance"]["artifacts_sha256"][
                artifact_name
            ]
        g3_signal_path = g3_signal_root / "manifest.json"
        _write_fixture(g3_signal_path, g3_signal_value)
        _expect_failure_containing(
            lambda: load_manifest(g3_signal_path),
            "a production 6CQ projection without artifacts",
            "manifest is missing configuration projection artifacts",
        )

        g3_signal_negatives = []
        for candidate_state in (False, 1, None):
            value = json.loads(json.dumps(g3_signal_value))
            for selector_map in value["provenance"]["selectors"].values():
                if candidate_state is None:
                    del selector_map[PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR]
                else:
                    selector_map[
                        PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR
                    ] = candidate_state
            g3_signal_negatives.append(
                (f"candidate-selector-{candidate_state!r}", value)
            )
        for selector_name in (
            CONFIGURATION_PROJECTION_STABLE_SELECTOR,
            CONFIGURATION_PROJECTION_SELECTOR,
        ):
            missing_value = json.loads(json.dumps(g3_signal_value))
            false_value = json.loads(json.dumps(g3_signal_value))
            integer_value = json.loads(json.dumps(g3_signal_value))
            for selector_map in missing_value["provenance"]["selectors"].values():
                del selector_map[selector_name]
            for selector_map in false_value["provenance"]["selectors"].values():
                selector_map[selector_name] = False
            for selector_map in integer_value["provenance"]["selectors"].values():
                selector_map[selector_name] = 1
            g3_signal_negatives.extend(
                (
                    (f"missing-retained-{selector_name}", missing_value),
                    (f"false-retained-{selector_name}", false_value),
                    (f"integer-retained-{selector_name}", integer_value),
                )
            )
        wrong_parent_g3_value = json.loads(json.dumps(g3_signal_value))
        wrong_parent_g3_value["lineage"]["parent_record_id"] = "wrong-parent"
        g3_signal_negatives.append(("wrong-parent", wrong_parent_g3_value))
        wrong_generation_g3_value = json.loads(json.dumps(g3_signal_value))
        wrong_generation_g3_value["lineage"]["generation"] = 4
        g3_signal_negatives.append(("wrong-generation", wrong_generation_g3_value))
        selector_only_g3_value = json.loads(json.dumps(g3_signal_value))
        selector_only_g3_value["provenance"]["record_id"] = "wrong-record"
        g3_signal_negatives.append(
            ("selector-with-wrong-record", selector_only_g3_value)
        )
        for name, value in g3_signal_negatives:
            path = g3_signal_root / f"negative-{name}.json"
            _write_fixture(path, value)
            _expect_failure(
                lambda path=path: load_manifest(path),
                f"the 6CQ production signal {name}",
            )

        cross_contract_value = json.loads(json.dumps(g3_signal_value))
        cross_contract_value["provenance"][
            "record_id"
        ] = CONFIGURATION_PROJECTION_RECORD_ID
        cross_contract_value["lineage"] = {
            "generation": 2,
            "parent_record_id": "6cl-c-stable-negative-g000001",
            "original_role": "candidate",
        }
        cross_contract_path = g3_signal_root / "cross-contract-signals.json"
        _write_fixture(cross_contract_path, cross_contract_value)
        _expect_failure_containing(
            lambda: load_manifest(cross_contract_path),
            "a 6CP record mixed with the 6CQ candidate selector",
            "record and candidate-selector signals conflict",
        )

        projection_signal_root = root / "projection-signal-no-artifacts"
        projection_signal_root.mkdir()
        exact_record_without_artifacts = json.loads(json.dumps(projection_value))
        for artifact_name in CONFIGURATION_PROJECTION_ARTIFACTS:
            del exact_record_without_artifacts["provenance"][
                "artifacts_sha256"
            ][artifact_name]
        exact_record_false_selector = json.loads(
            json.dumps(exact_record_without_artifacts)
        )
        for selector_map in exact_record_false_selector["provenance"][
            "selectors"
        ].values():
            selector_map[CONFIGURATION_PROJECTION_SELECTOR] = False
        exact_record_absent_selector = json.loads(
            json.dumps(exact_record_without_artifacts)
        )
        for selector_map in exact_record_absent_selector["provenance"][
            "selectors"
        ].values():
            del selector_map[CONFIGURATION_PROJECTION_SELECTOR]
        selector_signal_integer = json.loads(json.dumps(baseline_value))
        for selector_map in selector_signal_integer["provenance"][
            "selectors"
        ].values():
            selector_map[CONFIGURATION_PROJECTION_SELECTOR] = 1
        for name, value in (
            ("exact-record-false-selector", exact_record_false_selector),
            ("exact-record-absent-selector", exact_record_absent_selector),
            ("integer-selector-without-artifacts", selector_signal_integer),
        ):
            path = projection_signal_root / f"{name}.json"
            _write_fixture(path, value)
            _expect_failure(
                lambda path=path: load_manifest(
                    path,
                    _projection_contract=projection_contract,
                ),
                f"the 6CP signal {name}",
            )

        swapped_projection_raw_path = projection_raw_path.with_suffix(".swapped")

        def swap_projection_raw_path() -> None:
            projection_raw_path.rename(swapped_projection_raw_path)
            projection_raw_path.write_bytes(raw_fixture)
            projection_raw_path.chmod(0o444)

        _expect_failure(
            lambda: _read_sealed_projection_artifact(
                projection_raw_path,
                maximum_bytes=len(raw_fixture),
                _after_open_hook=swap_projection_raw_path,
            ),
            "a projection artifact pathname swap after open",
        )
        projection_raw_path.unlink()
        swapped_projection_raw_path.rename(projection_raw_path)

        scope_negative_values = []
        wrong_protocol_value = json.loads(json.dumps(projection_value))
        wrong_protocol_value["protocol"]["protocol_id"] = "wrong-protocol"
        scope_negative_values.append(("protocol", wrong_protocol_value))
        wrong_record_value = json.loads(json.dumps(projection_value))
        wrong_record_value["provenance"]["record_id"] = "wrong-record"
        scope_negative_values.append(("record", wrong_record_value))
        wrong_generation_value = json.loads(json.dumps(projection_value))
        wrong_generation_value["lineage"]["generation"] = 3
        scope_negative_values.append(("generation", wrong_generation_value))
        wrong_parent_value = json.loads(json.dumps(projection_value))
        wrong_parent_value["lineage"]["parent_record_id"] = "wrong-parent"
        scope_negative_values.append(("parent", wrong_parent_value))
        missing_stable_value = json.loads(json.dumps(projection_value))
        for selector_map in missing_stable_value["provenance"]["selectors"].values():
            del selector_map[CONFIGURATION_PROJECTION_STABLE_SELECTOR]
        scope_negative_values.append(("missing-stable-selector", missing_stable_value))
        disabled_stable_value = json.loads(json.dumps(projection_value))
        for selector_map in disabled_stable_value["provenance"]["selectors"].values():
            selector_map[CONFIGURATION_PROJECTION_STABLE_SELECTOR] = False
        scope_negative_values.append(("disabled-stable-selector", disabled_stable_value))
        integer_candidate_value = json.loads(json.dumps(projection_value))
        for selector_map in integer_candidate_value["provenance"]["selectors"].values():
            selector_map[CONFIGURATION_PROJECTION_SELECTOR] = 1
        scope_negative_values.append(("integer-candidate-selector", integer_candidate_value))
        extra_selector_value = json.loads(json.dumps(projection_value))
        for selector_map in extra_selector_value["provenance"]["selectors"].values():
            selector_map["EXTRA_SELECTOR"] = True
        scope_negative_values.append(("extra-selector", extra_selector_value))
        for name, value in scope_negative_values:
            path = projection_root / f"scope-{name}.json"
            _write_fixture(path, value)
            _expect_failure(
                lambda path=path: load_manifest(
                    path,
                    _projection_contract=projection_contract,
                ),
                f"a configuration projection outside {name} scope",
            )

        wrong_role_contract = dict(projection_contract)
        wrong_role_contract["original_role"] = "production"
        _expect_failure(
            lambda: load_manifest(
                projection_manifest_path,
                _projection_contract=wrong_role_contract,
            ),
            "a configuration projection outside original-role scope",
        )

        partial_projection_value = json.loads(json.dumps(projection_value))
        del partial_projection_value["provenance"]["artifacts_sha256"][
            "configuration_cache_raw"
        ]
        partial_projection_path = projection_root / "partial.json"
        _write_fixture(partial_projection_path, partial_projection_value)
        _expect_failure(
            lambda: load_manifest(
                partial_projection_path,
                _projection_contract=projection_contract,
            ),
            "a partial configuration projection artifact pair",
        )

        hidden_projection_raw_path = projection_raw_path.with_suffix(".hidden")
        projection_raw_path.rename(hidden_projection_raw_path)
        try:
            _expect_failure(
                lambda: load_manifest(
                    projection_manifest_path,
                    _projection_contract=projection_contract,
                ),
                "a missing configuration raw snapshot",
            )
        finally:
            hidden_projection_raw_path.rename(projection_raw_path)

        projection_raw_path.chmod(0o644)
        _expect_failure(
            lambda: load_manifest(
                projection_manifest_path,
                _projection_contract=projection_contract,
            ),
            "a writable configuration raw snapshot",
        )
        projection_raw_path.chmod(0o444)
        projection_raw_path.chmod(0o644)
        projection_raw_path.write_bytes(raw_fixture + b"tampered")
        projection_raw_path.chmod(0o444)
        _expect_failure(
            lambda: load_manifest(
                projection_manifest_path,
                _projection_contract=projection_contract,
            ),
            "a tampered configuration raw snapshot",
        )
        projection_raw_path.chmod(0o644)
        projection_raw_path.write_bytes(raw_fixture)
        projection_raw_path.chmod(0o444)

        semantic_receipt = dict(projection_receipt)
        semantic_receipt["mode"] = "unexpected-mode"
        projection_receipt_path.chmod(0o644)
        _write_fixture(projection_receipt_path, semantic_receipt)
        projection_receipt_path.chmod(0o444)
        semantic_projection_value = json.loads(json.dumps(projection_value))
        semantic_projection_value["provenance"]["artifacts_sha256"][
            "configuration_projection_receipt"
        ] = hashlib.sha256(projection_receipt_path.read_bytes()).hexdigest()
        semantic_projection_path = projection_root / "semantic.json"
        _write_fixture(semantic_projection_path, semantic_projection_value)
        _expect_failure(
            lambda: load_manifest(
                semantic_projection_path,
                _projection_contract=projection_contract,
            ),
            "a semantically altered configuration projection receipt",
        )
        projection_receipt_path.chmod(0o644)
        _write_fixture(projection_receipt_path, projection_receipt)
        projection_receipt_path.chmod(0o444)

        spurious_root = root / "legacy-spurious"
        spurious_root.mkdir()
        spurious_value = json.loads(json.dumps(baseline_value))
        spurious_raw_path = spurious_root / CONFIGURATION_PROJECTION_RAW_NAME
        spurious_receipt_path = (
            spurious_root / CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        spurious_raw_path.write_bytes(raw_fixture)
        spurious_raw_path.chmod(0o444)
        _write_fixture(spurious_receipt_path, projection_receipt)
        spurious_receipt_path.chmod(0o444)
        spurious_value["provenance"]["artifacts_sha256"].update(
            {
                "configuration_cache_raw": hashlib.sha256(
                    spurious_raw_path.read_bytes()
                ).hexdigest(),
                "configuration_projection_receipt": hashlib.sha256(
                    spurious_receipt_path.read_bytes()
                ).hexdigest(),
            }
        )
        spurious_manifest_path = spurious_root / "manifest.json"
        _write_fixture(spurious_manifest_path, spurious_value)
        _expect_failure(
            lambda: load_manifest(
                spurious_manifest_path,
                _projection_contract=projection_contract,
            ),
            "spurious configuration projection artifacts on a legacy record",
        )
        _expect_failure_containing(
            lambda: load_manifest(spurious_manifest_path),
            "artifact-only configuration projection signals",
            "artifacts are present without a recognized record or candidate-selector contract signal",
        )

        schema = manifest_schema()
        _require(
            schema["$id"] == f"urn:{MANIFEST_SCHEMA_NAME}:v{MANIFEST_SCHEMA_VERSION}"
            and schema["additionalProperties"] is False
            and len(schema["properties"]["lineage"]["oneOf"]) == 2,
            "self-test",
            "explicit schema identity/closure/lineage mismatch",
        )

        pointer_path = root / "baseline.current"
        registry_path = root / "registry.json"
        pointer_path.write_bytes(b"baseline-record\n")
        registry_path.write_bytes(b'{"baseline":"baseline-record"}\n')
        pointer_before = pointer_path.read_bytes()
        registry_before = registry_path.read_bytes()
        pointer_metadata = pointer_path.stat()
        registry_metadata = registry_path.stat()
        output_path = root / "authoritative-report.json"
        report_payload = _serialize_report(report)
        _publish_report_exclusive(output_path, report_payload)
        _require(
            output_path.read_bytes() == report_payload
            and stat.S_IMODE(output_path.stat().st_mode) == 0o444,
            "self-test",
            "authoritative report bytes or immutable mode mismatch",
        )
        _expect_failure(
            lambda: _publish_report_exclusive(output_path, b"overwrite\n"),
            "an overwrite of an existing report",
        )
        _require(
            output_path.read_bytes() == report_payload,
            "self-test",
            "failed overwrite changed the authoritative report",
        )
        race_path = root / "authoritative-race.json"
        race_barrier = threading.Barrier(3)
        race_results = []
        race_lock = threading.Lock()

        def race_publish(payload: bytes) -> None:
            race_barrier.wait()
            try:
                _publish_report_exclusive(race_path, payload)
            except ReportError:
                result = "rejected"
            else:
                result = "published"
            with race_lock:
                race_results.append(result)

        race_threads = [
            threading.Thread(target=race_publish, args=(payload,), daemon=True)
            for payload in (b'{"winner":1}\n', b'{"winner":2}\n')
        ]
        for thread in race_threads:
            thread.start()
        race_barrier.wait()
        for thread in race_threads:
            thread.join()
        _require(
            sorted(race_results) == ["published", "rejected"]
            and race_path.read_bytes() in (b'{"winner":1}\n', b'{"winner":2}\n')
            and stat.S_IMODE(race_path.stat().st_mode) == 0o444,
            "self-test",
            "exclusive report publication race did not preserve one sealed winner",
        )
        pointer_after = pointer_path.stat()
        registry_after = registry_path.stat()
        _require(
            pointer_path.read_bytes() == pointer_before
            and registry_path.read_bytes() == registry_before
            and (
                pointer_after.st_ino,
                pointer_after.st_mode,
                pointer_after.st_size,
                pointer_after.st_mtime_ns,
            )
            == (
                pointer_metadata.st_ino,
                pointer_metadata.st_mode,
                pointer_metadata.st_size,
                pointer_metadata.st_mtime_ns,
            )
            and (
                registry_after.st_ino,
                registry_after.st_mode,
                registry_after.st_size,
                registry_after.st_mtime_ns,
            )
            == (
                registry_metadata.st_ino,
                registry_metadata.st_mode,
                registry_metadata.st_size,
                registry_metadata.st_mtime_ns,
            ),
            "self-test",
            "report publication touched the baseline pointer or registry",
        )

        mismatch_value = json.loads(json.dumps(candidate_value))
        mismatch_value["comparability"]["power"]["mode"] = "low-power"
        mismatch_path = root / "comparability-mismatch.json"
        _write_fixture(mismatch_path, mismatch_value)
        mismatch = load_manifest(mismatch_path)
        _expect_failure(
            lambda: build_report(baseline, mismatch),
            "a comparability mismatch",
        )

        protocol_mismatch_value = json.loads(json.dumps(candidate_value))
        protocol_mismatch_value["protocol"]["protocol_id"] = "new-epoch"
        protocol_mismatch_path = root / "protocol-mismatch.json"
        _write_fixture(protocol_mismatch_path, protocol_mismatch_value)
        protocol_mismatch = load_manifest(protocol_mismatch_path)
        _expect_failure(
            lambda: build_report(baseline, protocol_mismatch),
            "a protocol mismatch",
        )

        generation_zero_candidate_value = json.loads(json.dumps(candidate_value))
        generation_zero_candidate_value["lineage"] = {
            "generation": 0,
            "parent_record_id": None,
            "original_role": "candidate",
        }
        generation_zero_candidate_path = root / "generation-zero-candidate.json"
        _write_fixture(generation_zero_candidate_path, generation_zero_candidate_value)
        _expect_failure(
            lambda: load_manifest(generation_zero_candidate_path),
            "a generation-zero candidate",
        )

        nonzero_production_value = json.loads(json.dumps(baseline_value))
        nonzero_production_value["lineage"] = {
            "generation": 1,
            "parent_record_id": "baseline-record",
            "original_role": "production",
        }
        nonzero_production_path = root / "nonzero-production.json"
        _write_fixture(nonzero_production_path, nonzero_production_value)
        _expect_failure(
            lambda: load_manifest(nonzero_production_path),
            "a nonzero-generation production record",
        )

        for artifact_name in sorted(REQUIRED_ARTIFACT_HASHES):
            missing_artifact_value = json.loads(json.dumps(candidate_value))
            del missing_artifact_value["provenance"]["artifacts_sha256"][artifact_name]
            missing_artifact_path = root / f"missing-{artifact_name}.json"
            _write_fixture(missing_artifact_path, missing_artifact_value)
            _expect_failure(
                lambda path=missing_artifact_path: load_manifest(path),
                f"a manifest missing required artifact {artifact_name}",
            )

        bad_parent_value = json.loads(json.dumps(candidate_value))
        bad_parent_value["lineage"]["parent_record_id"] = "wrong-record"
        bad_parent_path = root / "bad-parent.json"
        _write_fixture(bad_parent_path, bad_parent_value)
        bad_parent = load_manifest(bad_parent_path)
        _expect_failure(lambda: build_report(baseline, bad_parent), "a wrong lineage parent")

        bad_wall_value = json.loads(json.dumps(candidate_value))
        bad_wall_value["measurements"]["loading"]["wall_ns"] += 1
        bad_wall_path = root / "bad-wall.json"
        _write_fixture(bad_wall_path, bad_wall_value)
        _expect_failure(lambda: load_manifest(bad_wall_path), "an inconsistent raw loading wall")

        raced_anchor_value = json.loads(json.dumps(candidate_value))
        raced_anchor_value["measurements"]["fort_mode"]["endpoint_after"] = 2520
        raced_anchor_path = root / "raced-anchor.json"
        _write_fixture(raced_anchor_path, raced_anchor_value)
        _expect_failure(lambda: load_manifest(raced_anchor_path), "a raced announcement anchor")

        bad_measure_value = json.loads(json.dumps(candidate_value))
        bad_measure_value["measurements"]["in_game"]["measured_endpoint"] = 3960
        bad_measure_path = root / "bad-measure.json"
        _write_fixture(bad_measure_path, bad_measure_value)
        _expect_failure(
            lambda: load_manifest(bad_measure_path),
            "an in-game window other than w0+120",
        )

        bad_lifecycle_value = json.loads(json.dumps(candidate_value))
        bad_lifecycle_value["lifecycle"]["sdl_quit_count"] = 0
        bad_lifecycle_path = root / "bad-lifecycle.json"
        _write_fixture(bad_lifecycle_path, bad_lifecycle_value)
        _expect_failure(lambda: load_manifest(bad_lifecycle_path), "an incomplete lifecycle")

        early_input_value = json.loads(json.dumps(candidate_value))
        early_input_value["lifecycle"]["no_input_before_p2300"] = False
        early_input_path = root / "early-input.json"
        _write_fixture(early_input_path, early_input_value)
        _expect_failure(
            lambda: load_manifest(early_input_path),
            "input before p2300",
        )

        boolean_count_value = json.loads(json.dumps(candidate_value))
        boolean_count_value["lifecycle"]["sdl_quit_count"] = True
        boolean_count_path = root / "boolean-count.json"
        _write_fixture(boolean_count_path, boolean_count_value)
        _expect_failure(
            lambda: load_manifest(boolean_count_path),
            "a boolean spoofing an integer count",
        )

        zero_coverage_value = json.loads(json.dumps(candidate_value))
        zero_coverage_value["lifecycle"]["candidate_marker"]["coverage_positive"] = False
        zero_coverage_path = root / "zero-coverage.json"
        _write_fixture(zero_coverage_path, zero_coverage_value)
        _expect_failure(
            lambda: load_manifest(zero_coverage_path),
            "required zero candidate coverage",
        )

        selector_mismatch_value = json.loads(json.dumps(candidate_value))
        selector_mismatch_value["provenance"]["selectors"]["effective"][
            "fixture_candidate_enabled"
        ] = False
        selector_mismatch_path = root / "selector-mismatch.json"
        _write_fixture(selector_mismatch_path, selector_mismatch_value)
        _expect_failure(
            lambda: load_manifest(selector_mismatch_path),
            "a selector receipt mismatch",
        )

        extra_key_value = json.loads(json.dumps(candidate_value))
        extra_key_value["unexpected"] = True
        extra_key_path = root / "extra-key.json"
        _write_fixture(extra_key_path, extra_key_value)
        _expect_failure(lambda: load_manifest(extra_key_path), "an unknown top-level key")

        duplicate_path = root / "duplicate.json"
        duplicate_path.write_text('{"schema":{},"schema":{}}\n', encoding="utf-8")
        _expect_failure(lambda: load_manifest(duplicate_path), "a duplicate JSON key")


def _main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "validate and compare one immutable persistent baseline manifest and one "
            "candidate manifest offline; publish JSON O_EXCL with --output, mirror it "
            "to stdout, and write the table to stderr"
        )
    )
    parser.add_argument("baseline_manifest", nargs="?", metavar="BASELINE.json")
    parser.add_argument("candidate_manifest", nargs="?", metavar="CANDIDATE.json")
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        metavar="REPORT.json",
        help="new authoritative report path (must not already exist)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic positive and fail-closed synthetic fixtures",
    )
    parser.add_argument(
        "--print-schema",
        action="store_true",
        help="print the explicit input JSON Schema and exit",
    )
    arguments = parser.parse_args(argv)

    if arguments.self_test or arguments.print_schema:
        if (
            arguments.baseline_manifest is not None
            or arguments.candidate_manifest is not None
            or arguments.output is not None
        ):
            parser.error("--self-test/--print-schema do not accept paths or --output")
        if arguments.self_test and arguments.print_schema:
            parser.error("choose exactly one of --self-test or --print-schema")
        if arguments.self_test:
            self_test()
            print("persistent-performance-report-self-test: PASS")
        else:
            json.dump(manifest_schema(), sys.stdout, indent=2, sort_keys=True)
            sys.stdout.write("\n")
        return 0

    if arguments.baseline_manifest is None or arguments.candidate_manifest is None:
        parser.error("BASELINE.json and CANDIDATE.json are required")
    if arguments.output is None:
        parser.error("--output REPORT.json is required for authoritative comparison")
    baseline = load_manifest(pathlib.Path(arguments.baseline_manifest))
    candidate = load_manifest(pathlib.Path(arguments.candidate_manifest))
    report = build_report(baseline, candidate)
    payload = _serialize_report(report)
    _publish_report_exclusive(arguments.output, payload)
    print(_render_table(report), file=sys.stderr)
    sys.stdout.write(payload.decode("utf-8"))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except ReportError as error:
        print(f"persistent-performance-report: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
