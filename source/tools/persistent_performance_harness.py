#!/usr/bin/env python3
"""Fail-closed macOS persistent-performance run harness.

The harness owns one emulator process, observes the non-terminal p2300
checkpoint, and records the two manual announcement windows.  It deliberately
contains no controller, keyboard, LLDB, AppleScript, or signal-based input
facility.  Baseline comparison and promotion are separate tools.
"""

from __future__ import annotations

import argparse
import decimal
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import pwd
import re
import resource
import selectors
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Callable, Dict, Iterable, Mapping, Optional, Sequence, Tuple


HARNESS_SCHEMA = "lumen.persistent-performance-harness/1"
ANCHOR_SCHEMA = "lumen.persistent-performance-anchor/1"
START_SCHEMA_NAME = "lumen.persistent-performance-start"
START_SCHEMA_VERSION = 1
EPOCH_SCHEMA_NAME = "lumen.persistent-performance-epoch"
EPOCH_SCHEMA_VERSION = 1
RUN_MANIFEST_SCHEMA_NAME = "lumen.persistent-performance-run"
RUN_MANIFEST_SCHEMA_VERSION = 1
REPORT_TOOL_SCHEMA = "lumen.persistent-performance-report/1"

CHECKPOINT_PRESENT = 2300
WINDOW_SIZE = 120
UINT64_MAX = (1 << 64) - 1
ZERO = decimal.Decimal(0)

CHECKPOINT_OBSERVATION_SCHEMA = (
    "lumen.persistent-performance-checkpoint-observation/1"
)
CHECKPOINT_DECIMAL_MIGRATION_SCHEMA = (
    "lumen.persistent-performance-checkpoint-decimal-migration/1"
)
CHECKPOINT_DECIMAL_MIGRATION_ATTESTATION = (
    "CONFIRM_CHECKPOINT_DECIMAL_MIGRATION_NO_RAW_REWRITE"
)
CHECKPOINT_DECIMAL_MIGRATION_REASON = (
    "legacy-decimal-vs-float-checkpoint-comparison-only"
)
# This is the one released harness whose direct Decimal-vs-float dictionary
# comparison can reject otherwise identical checkpoint evidence at seal time.
# No other frozen harness digest is migration-eligible.
CHECKPOINT_DECIMAL_MIGRATION_LEGACY_HARNESS_SHA256 = (
    "ec4df9af0905beac0d14da4daca1e3c0b03515f48b89a2dc47c742927799ba6c"
)
CHECKPOINT_DECIMAL_MIGRATION_RECEIPT_NAME = "checkpoint-decimal-migration.json"
CHECKPOINT_DECIMAL_MIGRATION_SEAL_HARNESS_NAME = (
    "checkpoint-decimal-migration-seal-harness.py"
)

HEX_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REVISION_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
SAFE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
DECIMAL_RE = r"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?"
UNSIGNED_RE = r"(?:0|[1-9][0-9]*)"
SIGNED_RE = r"(?:0|-?[1-9][0-9]*)"
PIPELINE_RE = re.compile(
    rf"^\[FRAME_PIPELINE\] present=(?P<present>{UNSIGNED_RE}) "
    rf"avg_interval=(?P<interval>{DECIMAL_RE}) ms "
    rf"avg_latency=(?P<latency>{DECIMAL_RE}) ms "
    rf"pending=(?P<pending>{UNSIGNED_RE}) "
    rf"index=(?P<index>{SIGNED_RE}) arg=(?P<arg>{SIGNED_RE})$"
)
CHECKPOINT_RE = re.compile(
    rf"^\[FRAME_CHECKPOINT\] present=(?P<present>{UNSIGNED_RE}) "
    rf"target=(?P<target>{UNSIGNED_RE}) "
    rf"cumulative_ms=(?P<cumulative>{DECIMAL_RE}) "
    rf"cumulative_intervals=(?P<cumulative_intervals>{UNSIGNED_RE}) "
    rf"partial_ms=(?P<partial>{DECIMAL_RE}) "
    rf"partial_intervals=(?P<partial_intervals>{UNSIGNED_RE}) "
    rf"terminal=(?P<terminal>{UNSIGNED_RE})$"
)

CHECKPOINT_VALUE_KEYS = frozenset(
    {
        "present",
        "target",
        "cumulative_ms",
        "cumulative_intervals",
        "partial_ms",
        "partial_intervals",
        "terminal",
    }
)
CHECKPOINT_OBSERVATION_KEYS = frozenset(
    {
        "schema",
        "launch_uptime_raw_ns",
        "checkpoint_observed_uptime_raw_ns",
        "wall_ns",
        "checkpoint",
        "line_sha256",
    }
)
CHECKPOINT_DECIMAL_MIGRATION_KEYS = frozenset(
    {
        "schema",
        "record_id",
        "attestation",
        "reason",
        "legacy_harness_sha256",
        "seal_harness_sha256",
        "freeze_sha256",
        "checkpoint_observation_sha256",
        "checkpoint_line_sha256",
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
START_KEYS = frozenset(
    {
        "schema",
        "protocol",
        "comparability",
        "provenance",
        "lineage",
        "candidate_marker",
        "paths",
        "launch",
    }
)

FORT_ANCHOR_ATTESTATIONS = frozenset(
    {
        "CONFIRM_NO_INPUT_BEFORE_P2300",
        "CONFIRM_FORT_MODE_ANNOUNCEMENT",
        "CONFIRM_MANUAL_INPUT_ONLY",
    }
)
FORT_COMPLETE_ATTESTATIONS = frozenset(
    {
        "CONFIRM_SAME_FORT_MODE_SCENE",
        "CONFIRM_ZERO_INPUT_SINCE_FORT_ANNOUNCEMENT",
    }
)
GAME_ANCHOR_ATTESTATIONS = frozenset(
    {
        "CONFIRM_NAVIGATION_OBJECTIVE_BOGGY_100HP_HUD_TERRAIN",
        "CONFIRM_TIMER_ADVANCED",
        "CONFIRM_MANUAL_INPUT_ONLY",
    }
)
GAME_COMPLETE_ATTESTATIONS = frozenset(
    {
        "CONFIRM_SAME_NAVIGATION_SCENE",
        "CONFIRM_ZERO_INPUT_SINCE_INGAME_ANNOUNCEMENT",
    }
)
ANCHOR_PENDING_KEYS = frozenset(
    {
        "schema",
        "scene",
        "receipt_uptime_raw_ns",
        "endpoint_before",
        "endpoint_after",
        "anchor_endpoint",
        "measured_endpoint",
        "stderr_size_after",
        "attestations",
        "anchor_evidence",
        "timer_before",
        "timer_after",
    }
)
ANCHOR_FINAL_KEYS = ANCHOR_PENDING_KEYS | frozenset(
    {
        "avg_interval_ms",
        "completed_uptime_raw_ns",
        "completion_attestations",
        "completion_evidence",
    }
)

A64_CONFIGURATION_PREFIX = "A64 dynarec configuration:"
GFX_CONFIGURATION_PREFIX = "GFX descriptor configuration:"
GFX_REQUIRED_RECEIPT_FIELDS = {
    "null_sampled_image": "1",
    "null_sampled_scope": "zero8_readonly_ps_gen4",
    "null_sampled_backend": "shared_zero_rgba8",
}
GFX_FINAL_PREFIX = "[GFX_NULL_SAMPLED_FINAL]"
A64_REQUIRED_RECEIPT_FIELDS = {
    "build": "release",
    "easy_profiler_compiled": "0",
    "fast_blocks": "1",
    "config_requested": "1",
    "requested": "1",
    "enabled": "1",
    "healthy": "1",
    "interpreter_diagnostic_history": "0",
    "diagnostic_history": "0",
    "trace_rip": "0",
    "module_trace": "0",
    "watch_arm": "0",
    "profile": "0",
    "fast_profile": "0",
    "memory_watch": "0",
    "memory_trace": "0",
    "host_wait_trace": "0",
    "custom_log": "0",
    "progress_log": "0",
    "frame_trace": "1",
    "video_readback": "0",
    "metal_debug_layer_present": "0",
    "malloc_nano_zone_present": "0",
    "batch_direct_retirement": "1",
    "direct_leaf_abi": "1",
    "direct_context_reuse": "1",
    "compact_immediates": "0",
    "packed_256": "0",
    "direct_block_binding": "0",
    "block_rflags_cache": "0",
    "block_gpr_cache": "0",
    "multi_segment_artifact": "0",
    "dense_resident_cache": "1",
    "dense_victim_archive": "0",
    "dense_async_compilation": "0",
    "dense_async_stability_admission": "0",
    "hybrid_direct_entry": "0",
    "contextless_dense_entry": "0",
}
SELECTOR_RECEIPT_ALLOWLIST = {
    # Candidate-specific selectors must name their own emitted receipt field;
    # a caller cannot bind an arbitrary selector to an unrelated value=1
    # field.  New candidate families extend this audited table explicitly.
    "LUMEN_X64_A64_DYNAREC_DENSE_SYNC_ADMISSION": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "dense_sync_admission",
    },
    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "stable_negative_route",
    },
    "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "native_only_admission",
    },
    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "guest_memory_operation_snapshot_elision",
    },
    "LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "scalar_memory_fallthrough_admission",
    },
    "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "fast_block_dead_rflags_elision",
    },
    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "fast_block_dense_directory",
    },
    "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "fast_block_inline_fixed_signature_match",
    },
    "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "fast_block_unrolled_fixed_signature_batching",
    },
    "LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "fast_block_idempotent_mru_store_elision",
    },
    "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "fast_block_callback_free_ordinary_specialization",
    },
    "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "arm_loading_composite",
    },
    "LUMEN_A64_METAL_SHADER_FUNCTION_CACHE": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "metal_shader_function_cache",
    },
    "LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "metal_depth_stencil_state_cache",
    },
    "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "metal_state_only_record_elision",
    },
}
# Only NODA needs to distinguish a retained selector's request from its
# effective route.  Historical 6CL records continue to bind their selector
# directly to stable_negative_route; g5 binds the request here and separately
# attests the one allowed effective=false supersession below.
NODA_SELECTOR_RECEIPT_OVERRIDES = {
    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": {
        "prefix": A64_CONFIGURATION_PREFIX,
        "field": "stable_negative_route_requested",
    },
}
CANDIDATE_MARKER_ALLOWLIST = {
    "LUMEN_X64_A64_DYNAREC_DENSE_SYNC_ADMISSION": {
        "prefix": "[A64_DENSE_SYNC_ADMISSION_FINAL]",
        "required_tokens": ["status=clean", "coverage=positive"],
        "coverage_token": "coverage=positive",
    },
    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": {
        "prefix": "[A64_STABLE_NEGATIVE_ROUTE_FINAL]",
        "required_tokens": ["status=clean", "coverage=positive"],
        "coverage_token": "coverage=positive",
    },
    "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION": {
        "prefix": "[A64_NATIVE_ONLY_ADMISSION_FINAL]",
        "required_tokens": [
            "effective=positive",
            "ordinary_routed=positive",
            "native_probe=bypassed",
            "native_only_executed=positive",
            "class=fastblock_primary_native_only_dynarec",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": {
        "prefix": "[A64_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION_FINAL]",
        "required_tokens": [
            "elided=positive",
            "signature_snapshot=preserved",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
    },
    "LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION": {
        "prefix": "[A64_SCALAR_MEMORY_FALLTHROUGH_ADMISSION_FINAL]",
        "required_tokens": [
            "admitted=positive",
            "native_executed=positive",
            "live_mismatch=none",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        # Phase-6CQ preregisters the complete marker, including field order.
        # Earlier marker contracts retain their historical token semantics.
        "exact_line": True,
    },
    "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION": {
        "prefix": "[FAST_BLOCK_DEAD_RFLAGS_ELISION_FINAL]",
        "required_tokens": [
            "ordinary=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "line_pattern": (
            r"\[FAST_BLOCK_DEAD_RFLAGS_ELISION_FINAL\] "
            r"blocks=[1-9][0-9]* operations=[1-9][0-9]* "
            r"ordinary=positive status=clean coverage=positive"
        ),
    },
    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": {
        "prefix": "[A64_FAST_BLOCK_DENSE_DIRECTORY_FINAL]",
        "required_tokens": [
            "lookup=positive",
            "hit=positive",
            "mismatch=none",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": {
        "prefix": "[A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH_FINAL]",
        "required_tokens": [
            "requested=positive",
            "effective=positive",
            "compared=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING": {
        "prefix": "[A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING_FINAL]",
        "required_tokens": [
            "dependency=positive",
            "compared=positive",
            "chunks=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    # IMSE is the exact-line companion marker of the combined direct g3
    # contract. It is not the primary candidate selector for that record.
    "LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION": {
        "prefix": "[A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION_FINAL]",
        "required_tokens": [
            "requested=positive",
            "effective=positive",
            "elided=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION": {
        "prefix": (
            "[A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION_FINAL]"
        ),
        "required_tokens": [
            "requested=positive",
            "effective=positive",
            "reentry_safe=positive",
            "snapshot_elision=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT": {
        "prefix": "[A64_ARM_LOADING_COMPOSITE_FINAL]",
        "required_tokens": [
            "purge=positive",
            "adaptive_snapshot=positive",
            "omit_cpu_frame_pointer=positive",
            "ifsm_rollback=positive",
            "requested=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    "LUMEN_A64_METAL_SHADER_FUNCTION_CACHE": {
        "prefix": "[A64_METAL_SHADER_FUNCTION_CACHE_FINAL]",
        "required_tokens": [
            "lookup=positive",
            "hit=positive",
            "compile=positive",
            "requested=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    "LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE": {
        "prefix": "[A64_METAL_DEPTH_STENCIL_STATE_CACHE_FINAL]",
        "required_tokens": [
            "requested=positive",
            "compiled=positive",
            "lookup=positive",
            "hit=positive",
            "miss=positive",
            "create_match=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
    "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION": {
        "prefix": "[A64_METAL_STATE_ONLY_RECORD_ELISION_FINAL]",
        "required_tokens": [
            "requested=positive",
            "effective=positive",
            "state_only=positive",
            "snapshot_consumed=positive",
            "status=clean",
            "coverage=positive",
        ],
        "coverage_token": "coverage=positive",
        "exact_line": True,
    },
}
CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST = {
    # Phase-6CP is a g2 continuation of the retained 6CL route, not a fresh
    # one-selector experiment.  The exact selector map is bound below to the
    # exact audited receipt keys and exact experiment environment keys.
    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
    },
    # Phase-6CQ is candidate-only generation 3 and retains both admitted
    # production routes from generations 1 and 2.
    "LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION": True,
    },
    # Phase-6CS is a separate generation-3 sibling of 6CQ.  The retired 6CQ
    # selector is deliberately absent, so adding it even as false is a mixed
    # cross-family bundle and fails closed.
    "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION": True,
    },
    # Phase-6DA is a new generation-3 sibling after the rejected 6CS record.
    # Only retained production 6CL+6CP and the new selector are admitted;
    # adding retired 6CQ/6CS even as false is a mixed-family bundle.
    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
    },
    # IFSM is the direct generation-2 continuation of the promoted 6DA g1
    # baseline.  Every retained production selector remains ON and IFSM is the
    # only new experiment key; retired sibling candidates remain absent.
    "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
        "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": True,
    },
    # The direct g3 bundle has UFSB as its primary selector and IMSE as an
    # inseparable companion. Both post-IFSM selectors must be present and true;
    # neither a UFSB-only nor an IMSE-only persistent record is admitted.
    "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
        "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": True,
        "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING": True,
        "LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION": True,
    },
    # FCFOS is the other direct generation-3 child of IFSM.  UFSB and IMSE
    # are sibling experiments, so neither key is admitted even as false.
    "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
        "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": True,
        "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION": True,
    },
    # ALC is the direct generation-3 child of IFSM which combines the screened
    # ARM hot-path changes with the retained 6DA matcher.  Rejected post-IFSM
    # sibling selectors are absent, including when explicitly false.
    "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
        "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": True,
        "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT": True,
    },
    # Generation 4 is one inseparable Metal runtime bundle on top of the
    # promoted ALC baseline.  MSORE is the primary selector; MSFC and MDSSC
    # are exact-line companion contracts.  Rejected post-IFSM siblings remain
    # absent, including when explicitly false.
    "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
        "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": True,
        "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT": True,
        "LUMEN_A64_METAL_SHADER_FUNCTION_CACHE": True,
        "LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE": True,
        "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION": True,
    },
    # Generation 5 keeps the complete promoted Metal g4 bundle and changes
    # only CPU admission: ordinary-compatible FastBlocks route directly to
    # their existing C++ executor while the Dynarec remains available to the
    # native-only families.  PBA is deliberately absent, including as false.
    "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION": {
        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
        "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
        "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
        "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": True,
        "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT": True,
        "LUMEN_A64_METAL_SHADER_FUNCTION_CACHE": True,
        "LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE": True,
        "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION": True,
        "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION": True,
    },
}

CANDIDATE_REQUIRED_RECEIPT_FIELDS = {
    # Scalar fallthrough remains absent from the 6CS environment/bundle and
    # therefore defaults OFF.  Bind that effective default to the emitted
    # runtime configuration receipt without changing the sterile environment.
    "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION": {
        "scalar_memory_fallthrough_admission": "0",
    },
    # Requested and effective must both be positive. The two retired sibling
    # candidates remain absent from the environment and explicitly OFF in the
    # structured runtime receipt.
    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": {
        "scalar_memory_fallthrough_admission": "0",
        "fast_block_dead_rflags_elision": "0",
        "fast_block_dense_directory_requested": "1",
        "fast_block_dense_directory": "1",
    },
    "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": {
        "stable_negative_route": "1",
        "guest_memory_operation_snapshot_elision": "1",
        "scalar_memory_fallthrough_admission": "0",
        "fast_block_dead_rflags_elision": "0",
        "fast_block_dense_directory_requested": "1",
        "fast_block_dense_directory": "1",
        "fast_block_inline_fixed_signature_match_requested": "1",
        "fast_block_inline_fixed_signature_match": "1",
    },
    "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING": {
        "stable_negative_route": "1",
        "guest_memory_operation_snapshot_elision": "1",
        "scalar_memory_fallthrough_admission": "0",
        "fast_block_dead_rflags_elision": "0",
        "fast_block_dense_directory_requested": "1",
        "fast_block_dense_directory": "1",
        "fast_block_inline_fixed_signature_match_requested": "1",
        "fast_block_inline_fixed_signature_match": "1",
        "fast_block_unrolled_fixed_signature_batching_requested": "1",
        "fast_block_unrolled_fixed_signature_batching": "1",
        "fast_block_idempotent_mru_store_elision_requested": "1",
        "fast_block_idempotent_mru_store_elision": "1",
    },
    "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION": {
        "stable_negative_route": "1",
        "guest_memory_operation_snapshot_elision": "1",
        "scalar_memory_fallthrough_admission": "0",
        "fast_block_dead_rflags_elision": "0",
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
    "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT": {
        "stable_negative_route": "1",
        "guest_memory_operation_snapshot_elision": "1",
        "scalar_memory_fallthrough_admission": "0",
        "fast_block_dead_rflags_elision": "0",
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
    "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION": {
        "stable_negative_route": "1",
        "guest_memory_operation_snapshot_elision": "1",
        "scalar_memory_fallthrough_admission": "0",
        "fast_block_dead_rflags_elision": "0",
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
    "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION": {
        "stable_negative_route_requested": "1",
        "stable_negative_route": "0",
        "stable_negative_route_superseded": "1",
        "profitable_block_admission_requested": "0",
        "profitable_block_admission": "0",
        "profitable_block_admission_class": "direct_positive_then_ordinary",
        "native_only_admission_requested": "1",
        "native_only_admission": "1",
        "native_only_admission_class": (
            "fastblock_primary_native_only_dynarec"
        ),
        "guest_memory_operation_snapshot_elision": "1",
        "scalar_memory_fallthrough_admission": "0",
        "fast_block_dead_rflags_elision": "0",
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
}

PHASE6CP_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION"
)
PHASE6CP_CONFIGURATION_PROJECTION_SCHEMA = {
    "name": "lumen.cmake-cache-production-projection",
    "version": 1,
}
PHASE6CP_CONFIGURATION_PROJECTION_ATTESTATION = (
    "CONFIRM_PHASE6CP_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
)
PHASE6CP_CONFIGURATION_PROJECTION_POLICY_ID = (
    "phase6cp-cmake-cache-production-v1"
)
PHASE6CP_CONFIGURATION_PROJECTION_MODE = (
    "strip-exact-inactive-phase6cm-phase6co-v1"
)
PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME = (
    "configuration-projection.json"
)
PHASE6CP_CONFIGURATION_CACHE_RAW_NAME = "configuration-cache.raw"
PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
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
PHASE6CP_PHASE6CM_CACHE_BLOCK = (
    b"//Compile scene-anchored sampled AArch64 ordinary-body diagnostic\n"
    b"// telemetry\n"
    b"LUMEN_A64_DYNAREC_PHASE6CM_PROFILE:BOOL=OFF\n"
    b"\n"
)
PHASE6CP_PHASE6CO_CACHE_BLOCK = (
    b"//Compile scene-anchored AArch64 operation-snapshot opportunity\n"
    b"// telemetry\n"
    b"LUMEN_A64_DYNAREC_PHASE6CO_PROFILE:BOOL=OFF\n"
    b"\n"
)
PHASE6CP_CONFIGURATION_PROJECTION_POLICY = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": "6cp-c-guest-memory-operation-snapshot-elision-g000002",
    "generation": 2,
    "parent_record_id": "6cl-c-stable-negative-g000001",
    "original_role": "candidate",
    "candidate_selector": PHASE6CP_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        PHASE6CP_CANDIDATE_SELECTOR
    ],
    "raw_bytes": 49567,
    "raw_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
    "phase6cm_block": PHASE6CP_PHASE6CM_CACHE_BLOCK,
    "phase6cm_block_sha256": "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e",
    "phase6co_block": PHASE6CP_PHASE6CO_CACHE_BLOCK,
    "phase6co_block_sha256": "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963",
    "removed_bytes": 246,
    "removed_segment_sha256": "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554",
    "projected_bytes": 49321,
    "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
}
PHASE6CQ_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION"
)
PHASE6CQ_CONFIGURATION_RAW_BYTES = 46_394
PHASE6CQ_CONFIGURATION_RAW_SHA256 = (
    "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
)
PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": "6cq-c-scalar-memory-fallthrough-admission-g000003",
    "generation": 3,
    "parent_record_id": (
        "6cp-c-guest-memory-operation-snapshot-elision-g000002"
    ),
    "original_role": "candidate",
    "candidate_selector": PHASE6CQ_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        PHASE6CQ_CANDIDATE_SELECTOR
    ],
    "raw_bytes": PHASE6CQ_CONFIGURATION_RAW_BYTES,
    "raw_sha256": PHASE6CQ_CONFIGURATION_RAW_SHA256,
    "projection_configured": True,
}
PHASE6CQ_CONFIGURATION_PROJECTION_SCHEMA = {
    "name": "lumen.cmake-cache-production-projection",
    "version": 1,
}
PHASE6CQ_CONFIGURATION_PROJECTION_ATTESTATION = (
    "CONFIRM_PHASE6CQ_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
)
PHASE6CQ_CONFIGURATION_PROJECTION_POLICY_ID = (
    "phase6cq-cmake-cache-production-v1"
)
PHASE6CQ_CONFIGURATION_PROJECTION_MODE = (
    "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-"
    "phase6cm-phase6co-v1"
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
PHASE6CQ_CONFIGURATION_RESTORE_CONTRACT_PREFIX = (
    b"lumen.phase6cq.configuration-restore-operations.v1\0"
)
PHASE6CQ_CMAKE_PREFIX_PATH_CURRENT = (
    b"CMAKE_OSX_SYSROOT:STRING=\n\n"
    b"//Value Computed by CMake\n"
    b"CMAKE_PROJECT_COMPAT_VERSION:STATIC=\n"
)
PHASE6CQ_CMAKE_PREFIX_PATH_INSERT = (
    b"//Semicolon-separated list of installation prefixes searched by\n"
    b"// find_package(), find_program(), find_library(), find_file(),\n"
    b"// and find_path(). Empty by default; intended to be set by the\n"
    b"// project or the user.\n"
    b"CMAKE_PREFIX_PATH:UNINITIALIZED=/opt/homebrew/opt/qt@5\n\n"
)
PHASE6CQ_PKG_OPTION_CURRENT = (
    b"\n//Compile bounded AArch64 dynarec cross-owner compile diagnostic\n"
)
PHASE6CQ_PKG_OPTION_INSERT = (
    b"//Build the clean-room PS4 PKG core and CLI tests\n"
    b"LUMENPKG_BUILD_TESTS:BOOL=ON\n\n"
)
PHASE6CQ_MOLTENVK_VALUES_CURRENT = (
    b"LUMEN_GFX_NULL_SAMPLED_IMAGE_DEFAULT:BOOL=ON\n\n"
)
PHASE6CQ_MOLTENVK_VALUES_INSERT = (
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
PHASE6CQ_VULKAN_LIBRARY_CURRENT = (
    b"SPIRV_CROSS_WERROR:BOOL=OFF\n\n"
    b"//Dependencies for the target\n"
    b"spirv-cross-glsl_LIB_DEPENDS:STATIC=general;spirv-cross-core;\n"
)
PHASE6CQ_VULKAN_LIBRARY_INSERT = (
    b"//Vulkan/MoltenVK library\n"
    b"Vulkan_Lib:FILEPATH=/opt/homebrew/lib/libMoltenVK.a\n\n"
)
PHASE6CQ_MOLTENVK_ADVANCED_CURRENT = (
    b"LUMEN_APPLE_NATIVE_METAL_UI_FRAMEWORK_PATH-ADVANCED:INTERNAL=1\n"
    b"//ADVANCED property for variable: PKG_CONFIG_ARGN\n"
)
PHASE6CQ_MOLTENVK_ADVANCED_INSERT = (
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
PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS = (
    (
        "cxx_compiler_cache_serialization",
        b"//CXX compiler\nCMAKE_CXX_COMPILER:STRING=/usr/bin/clang++\n",
        b"//Full path to the compiler used for language <LANG>. Set by CMake\n"
        b"// during compiler detection on the first configure; users may\n"
        b"// override on the command line to select a specific toolchain.\n"
        b"CMAKE_CXX_COMPILER:UNINITIALIZED=clang++\n",
    ),
    (
        "c_compiler_cache_serialization",
        b"//C compiler\nCMAKE_C_COMPILER:STRING=/usr/bin/clang\n",
        b"//Full path to the compiler used for language <LANG>. Set by CMake\n"
        b"// during compiler detection on the first configure; users may\n"
        b"// override on the command line to select a specific toolchain.\n"
        b"CMAKE_C_COMPILER:UNINITIALIZED=clang\n",
    ),
    (
        "cmake_prefix_path_stale_serialization",
        PHASE6CQ_CMAKE_PREFIX_PATH_CURRENT,
        PHASE6CQ_CMAKE_PREFIX_PATH_CURRENT[:27]
        + PHASE6CQ_CMAKE_PREFIX_PATH_INSERT
        + PHASE6CQ_CMAKE_PREFIX_PATH_CURRENT[27:],
    ),
    (
        "retired_pkg_test_option",
        PHASE6CQ_PKG_OPTION_CURRENT,
        PHASE6CQ_PKG_OPTION_CURRENT[:1]
        + PHASE6CQ_PKG_OPTION_INSERT
        + PHASE6CQ_PKG_OPTION_CURRENT[1:],
    ),
    (
        "inactive_moltenvk_values",
        PHASE6CQ_MOLTENVK_VALUES_CURRENT,
        PHASE6CQ_MOLTENVK_VALUES_CURRENT[:45]
        + PHASE6CQ_MOLTENVK_VALUES_INSERT
        + PHASE6CQ_MOLTENVK_VALUES_CURRENT[45:],
    ),
    (
        "inactive_vulkan_library_value",
        PHASE6CQ_VULKAN_LIBRARY_CURRENT,
        PHASE6CQ_VULKAN_LIBRARY_CURRENT[:29]
        + PHASE6CQ_VULKAN_LIBRARY_INSERT
        + PHASE6CQ_VULKAN_LIBRARY_CURRENT[29:],
    ),
    (
        "retired_pkg_makefile_count",
        b"CMAKE_NUMBER_OF_MAKEFILES:INTERNAL=24\n",
        b"CMAKE_NUMBER_OF_MAKEFILES:INTERNAL=25\n",
    ),
    (
        "inactive_moltenvk_advanced_metadata",
        PHASE6CQ_MOLTENVK_ADVANCED_CURRENT,
        PHASE6CQ_MOLTENVK_ADVANCED_CURRENT[:63]
        + PHASE6CQ_MOLTENVK_ADVANCED_INSERT
        + PHASE6CQ_MOLTENVK_ADVANCED_CURRENT[63:],
    ),
)
PHASE6CQ_CONFIGURATION_PROJECTION_POLICY: Optional[Mapping[str, Any]] = {
    **PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE,
    "projection_configured": True,
    "restore_operations": PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS,
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": 4_788,
    "restore_operations_contract_sha256": (
        "ee36e477af8d1218a7b254d078edf9841a4a233597790bba8bcffa0177c3c04c"
    ),
    "restored_bytes": 49_567,
    "restored_sha256": (
        "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35"
    ),
    "phase6cm_block": PHASE6CP_PHASE6CM_CACHE_BLOCK,
    "phase6cm_block_sha256": (
        "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e"
    ),
    "phase6co_block": PHASE6CP_PHASE6CO_CACHE_BLOCK,
    "phase6co_block_sha256": (
        "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963"
    ),
    "removed_bytes": 246,
    "removed_segment_sha256": (
        "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554"
    ),
    "projected_bytes": 49_321,
    "projected_sha256": (
        "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a"
    ),
}
PHASE6CS_CANDIDATE_SELECTOR = "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION"
PHASE6CS_CONFIGURATION_RAW_BYTES = 46_394
PHASE6CS_CONFIGURATION_RAW_SHA256 = (
    "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
)
PHASE6CS_CONFIGURATION_PROJECTION_SCOPE = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": "6cs-c-dead-rflags-elision-g000003",
    "generation": 3,
    "parent_record_id": (
        "6cp-c-guest-memory-operation-snapshot-elision-g000002"
    ),
    "original_role": "candidate",
    "candidate_selector": PHASE6CS_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        PHASE6CS_CANDIDATE_SELECTOR
    ],
    "raw_bytes": PHASE6CS_CONFIGURATION_RAW_BYTES,
    "raw_sha256": PHASE6CS_CONFIGURATION_RAW_SHA256,
    "projection_configured": True,
}
PHASE6CS_CONFIGURATION_PROJECTION_SCHEMA = {
    "name": "lumen.cmake-cache-production-projection",
    "version": 1,
}
PHASE6CS_CONFIGURATION_PROJECTION_ATTESTATION = (
    "CONFIRM_PHASE6CS_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
)
PHASE6CS_CONFIGURATION_PROJECTION_POLICY_ID = (
    "phase6cs-cmake-cache-production-v1"
)
PHASE6CS_CONFIGURATION_PROJECTION_MODE = (
    "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-"
    "phase6cm-phase6co-v1"
)
PHASE6CS_CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
    PHASE6CQ_CONFIGURATION_PROJECTION_RECEIPT_KEYS
)
PHASE6CS_CONFIGURATION_RESTORE_CONTRACT_PREFIX = (
    b"lumen.phase6cs.configuration-restore-operations.v1\0"
)
PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS = (
    PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS
)
PHASE6CS_CONFIGURATION_PROJECTION_POLICY: Optional[Mapping[str, Any]] = {
    **PHASE6CS_CONFIGURATION_PROJECTION_SCOPE,
    "projection_configured": True,
    "restore_operations": PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS,
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": 4_788,
    "restore_operations_contract_sha256": (
        "15a48324c22c27e8959a0d8706965110199d7ca1d8203f10d94fd823fe6e57f9"
    ),
    "restored_bytes": 49_567,
    "restored_sha256": (
        "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35"
    ),
    "phase6cm_block": PHASE6CP_PHASE6CM_CACHE_BLOCK,
    "phase6cm_block_sha256": (
        "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e"
    ),
    "phase6co_block": PHASE6CP_PHASE6CO_CACHE_BLOCK,
    "phase6co_block_sha256": (
        "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963"
    ),
    "removed_bytes": 246,
    "removed_segment_sha256": (
        "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554"
    ),
    "projected_bytes": 49_321,
    "projected_sha256": (
        "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a"
    ),
}
PHASE6DA_CANDIDATE_SELECTOR = "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY"
PHASE6DA_CONFIGURATION_RAW_BYTES = 46_394
PHASE6DA_CONFIGURATION_RAW_SHA256 = (
    "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
)
PHASE6DA_CONFIGURATION_PROJECTION_SCOPE = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": "6da-c-fast-block-dense-directory-g000003",
    "generation": 3,
    "parent_record_id": (
        "6cp-c-guest-memory-operation-snapshot-elision-g000002"
    ),
    "original_role": "candidate",
    "candidate_selector": PHASE6DA_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        PHASE6DA_CANDIDATE_SELECTOR
    ],
    "raw_bytes": PHASE6DA_CONFIGURATION_RAW_BYTES,
    "raw_sha256": PHASE6DA_CONFIGURATION_RAW_SHA256,
    "projection_configured": True,
}
PHASE6DA_CONFIGURATION_PROJECTION_SCHEMA = {
    "name": "lumen.cmake-cache-production-projection",
    "version": 1,
}
PHASE6DA_CONFIGURATION_PROJECTION_ATTESTATION = (
    "CONFIRM_PHASE6DA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
)
PHASE6DA_CONFIGURATION_PROJECTION_POLICY_ID = (
    "phase6da-cmake-cache-production-v1"
)
PHASE6DA_CONFIGURATION_PROJECTION_MODE = (
    "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-"
    "phase6cm-phase6co-v1"
)
PHASE6DA_CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
    PHASE6CQ_CONFIGURATION_PROJECTION_RECEIPT_KEYS
)
PHASE6DA_CONFIGURATION_RESTORE_CONTRACT_PREFIX = (
    b"lumen.phase6da.configuration-restore-operations.v1\0"
)
PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS = (
    PHASE6CQ_CONFIGURATION_RESTORE_OPERATIONS
)
PHASE6DA_CONFIGURATION_PROJECTION_POLICY: Optional[Mapping[str, Any]] = {
    **PHASE6DA_CONFIGURATION_PROJECTION_SCOPE,
    "projection_configured": True,
    "restore_operations": PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS,
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": 4_788,
    "restore_operations_contract_sha256": (
        "ca3126b513628e42a655bff5649ab3f7d4c915d89c2f881e68b07b023cea1c14"
    ),
    "restored_bytes": 49_567,
    "restored_sha256": (
        "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35"
    ),
    "phase6cm_block": PHASE6CP_PHASE6CM_CACHE_BLOCK,
    "phase6cm_block_sha256": (
        "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e"
    ),
    "phase6co_block": PHASE6CP_PHASE6CO_CACHE_BLOCK,
    "phase6co_block_sha256": (
        "0369ef30dd7d0bf06c57e5a97bc7098f407cf694601d516cef33cf3fc063e963"
    ),
    "removed_bytes": 246,
    "removed_segment_sha256": (
        "25dce4fd6c3f091c9daaab5a3e1f4781328728750952fcf95f0a4a2a168ec554"
    ),
    "projected_bytes": 49_321,
    "projected_sha256": (
        "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a"
    ),
}
ALC_CANDIDATE_SELECTOR = "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT"
ALC_DIRECT_RECORD_ID = "alc-c-arm-loading-composite-g000003"
ALC_CONFIGURATION_RAW_BYTES = 46_749
ALC_CONFIGURATION_RAW_SHA256 = (
    "3d42b0edc9be9efa0156a02400de50ae4afb127ca30b7a74df0a36c1e2291e71"
)
ALC_CONFIGURATION_PROJECTION_SCOPE = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": ALC_DIRECT_RECORD_ID,
    "generation": 3,
    "parent_record_id": "ifsm-c-fast-block-inline-fixed-signature-match-g000002",
    "original_role": "candidate",
    "candidate_selector": ALC_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        ALC_CANDIDATE_SELECTOR
    ],
    "raw_bytes": ALC_CONFIGURATION_RAW_BYTES,
    "raw_sha256": ALC_CONFIGURATION_RAW_SHA256,
    "projection_configured": True,
}
ALC_CONFIGURATION_PROJECTION_SCHEMA = {
    "name": "lumen.cmake-cache-production-projection",
    "version": 1,
}
ALC_CONFIGURATION_PROJECTION_ATTESTATION = (
    "CONFIRM_ALC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
)
ALC_CONFIGURATION_PROJECTION_POLICY_ID = (
    "alc-cmake-cache-production-v1"
)
ALC_CONFIGURATION_PROJECTION_MODE = (
    "strip-exact-active-alc-build-options-v1"
)
ALC_CONFIGURATION_PROJECTION_RECEIPT_KEYS = frozenset(
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
ALC_CONFIGURATION_STRIP_CONTRACT_PREFIX = (
    b"lumen.alc.configuration-strip-operations.v1\0"
)
ALC_ARM_HOTPATH_COMPOSITE_CACHE_BLOCK = (
    b"//Add compact FastBlock snapshots and omit CPU frame pointers on\n"
    b"// top of the purge\n"
    b"LUMEN_A64_ARM_HOTPATH_COMPOSITE:BOOL=ON\n"
    b"\n"
)
ALC_ARM_LOADING_COMPOSITE_CACHE_BLOCK = (
    b"//Use the retained 6DA matcher inside the ARM hot-path composite\n"
    b"LUMEN_A64_ARM_LOADING_COMPOSITE:BOOL=ON\n"
    b"\n"
)
ALC_REJECTED_FASTBLOCK_PURGE_CACHE_BLOCK = (
    b"//Remove rejected UFSB/IMSE/FCFOS paths from the macOS ARM64 Release\n"
    b"// binary\n"
    b"LUMEN_A64_REJECTED_FASTBLOCK_PURGE:BOOL=ON\n"
    b"\n"
)
ALC_CONFIGURATION_STRIP_OPERATIONS = (
    ("hotpath", ALC_ARM_HOTPATH_COMPOSITE_CACHE_BLOCK),
    ("loading", ALC_ARM_LOADING_COMPOSITE_CACHE_BLOCK),
    ("purge", ALC_REJECTED_FASTBLOCK_PURGE_CACHE_BLOCK),
)
ALC_CONFIGURATION_PROJECTION_POLICY: Optional[Mapping[str, Any]] = {
    **ALC_CONFIGURATION_PROJECTION_SCOPE,
    "projection_configured": True,
    "strip_operations": ALC_CONFIGURATION_STRIP_OPERATIONS,
    "strip_operation_count": 3,
    "strip_operations_contract_bytes": 421,
    "strip_operations_contract_sha256": (
        "704c751375758603328e79b34678e591ad4e52a419d18b35293f252516988e26"
    ),
    "removed_bytes": 355,
    "removed_segment_sha256": (
        "c0ec59ce9c4b95ef3bf3643b6fccbdf41aba1c1c034c8f8e630ad0ccc2b60e78"
    ),
    "projected_bytes": 46_394,
    "projected_sha256": (
        "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
    ),
}
MSORE_CANDIDATE_SELECTOR = "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION"
MSFC_COMPANION_SELECTOR = "LUMEN_A64_METAL_SHADER_FUNCTION_CACHE"
MDSSC_COMPANION_SELECTOR = "LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE"
METAL_RUNTIME_BUNDLE_RECORD_ID = (
    "msore-msfc-mdssc-c-metal-runtime-bundle-g000004"
)
METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": METAL_RUNTIME_BUNDLE_RECORD_ID,
    "generation": 4,
    "parent_record_id": ALC_DIRECT_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": MSORE_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        MSORE_CANDIDATE_SELECTOR
    ],
    "raw_bytes": ALC_CONFIGURATION_RAW_BYTES,
    "raw_sha256": ALC_CONFIGURATION_RAW_SHA256,
    "projection_configured": True,
}
METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCHEMA = (
    ALC_CONFIGURATION_PROJECTION_SCHEMA
)
METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_ATTESTATION = (
    "CONFIRM_MSORE_MSFC_MDSSC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
)
METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_POLICY_ID = (
    "msore-msfc-mdssc-cmake-cache-production-v1"
)
METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_MODE = (
    ALC_CONFIGURATION_PROJECTION_MODE
)
METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_RECEIPT_KEYS = (
    ALC_CONFIGURATION_PROJECTION_RECEIPT_KEYS
)
# The g4 build retains the exact ALC CMake options.  Reuse the byte-exact ALC
# transformer and replace only the preregistered identity of its input record.
METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_POLICY: Optional[
    Mapping[str, Any]
] = {
    **ALC_CONFIGURATION_PROJECTION_POLICY,
    **METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE,
}
NODA_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION"
)
PBA_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION"
)
NODA_RECORD_ID = (
    "noda-c-a64-dynarec-native-only-admission-g000005"
)
NODA_CONFIGURATION_PROJECTION_SCOPE = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": NODA_RECORD_ID,
    "generation": 5,
    "parent_record_id": METAL_RUNTIME_BUNDLE_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": NODA_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        NODA_CANDIDATE_SELECTOR
    ],
    "raw_bytes": ALC_CONFIGURATION_RAW_BYTES,
    "raw_sha256": ALC_CONFIGURATION_RAW_SHA256,
    "projection_configured": True,
}
NODA_CONFIGURATION_PROJECTION_SCHEMA = ALC_CONFIGURATION_PROJECTION_SCHEMA
NODA_CONFIGURATION_PROJECTION_ATTESTATION = (
    "CONFIRM_NODA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
)
NODA_CONFIGURATION_PROJECTION_POLICY_ID = (
    "noda-cmake-cache-production-v1"
)
NODA_CONFIGURATION_PROJECTION_MODE = ALC_CONFIGURATION_PROJECTION_MODE
NODA_CONFIGURATION_PROJECTION_RECEIPT_KEYS = (
    ALC_CONFIGURATION_PROJECTION_RECEIPT_KEYS
)
# NODA is an environment-only g5 continuation.  Its build retains the same
# exact ALC cache options as promoted Metal g4, so the byte transform is
# intentionally identical while the attestation/policy identity is distinct.
NODA_CONFIGURATION_PROJECTION_POLICY: Optional[Mapping[str, Any]] = {
    **ALC_CONFIGURATION_PROJECTION_POLICY,
    **NODA_CONFIGURATION_PROJECTION_SCOPE,
}
NODA_STABLE_NEGATIVE_SUPERSEDED_MARKER = (
    "[A64_STABLE_NEGATIVE_ROUTE_FINAL] classified=inapplicable "
    "live_mismatch=none status=clean coverage=superseded "
    "class=native_only_admission"
)
NODA_RETAINED_MARKER_LINES = (
    NODA_STABLE_NEGATIVE_SUPERSEDED_MARKER,
    "[A64_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION_FINAL] "
    "elided=positive signature_snapshot=preserved status=clean "
    "coverage=positive",
    "[A64_FAST_BLOCK_DENSE_DIRECTORY_FINAL] lookup=positive hit=positive "
    "mismatch=none status=clean coverage=positive",
    "[A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH_FINAL] "
    "requested=positive effective=positive compared=positive status=clean "
    "coverage=positive",
    "[A64_ARM_LOADING_COMPOSITE_FINAL] purge=positive "
    "adaptive_snapshot=positive omit_cpu_frame_pointer=positive "
    "ifsm_rollback=positive requested=positive status=clean "
    "coverage=positive",
    "[A64_METAL_SHADER_FUNCTION_CACHE_FINAL] lookup=positive hit=positive "
    "compile=positive requested=positive status=clean coverage=positive",
    "[A64_METAL_DEPTH_STENCIL_STATE_CACHE_FINAL] requested=positive "
    "compiled=positive lookup=positive hit=positive miss=positive "
    "create_match=positive status=clean coverage=positive",
    "[A64_METAL_STATE_ONLY_RECORD_ELISION_FINAL] requested=positive "
    "effective=positive state_only=positive snapshot_consumed=positive "
    "status=clean coverage=positive",
)
CONFIGURATION_PROJECTION_SCOPES = (
    PHASE6CP_CONFIGURATION_PROJECTION_POLICY,
    PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE,
    PHASE6CS_CONFIGURATION_PROJECTION_SCOPE,
    PHASE6DA_CONFIGURATION_PROJECTION_SCOPE,
    ALC_CONFIGURATION_PROJECTION_SCOPE,
    METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE,
    NODA_CONFIGURATION_PROJECTION_SCOPE,
)

PHASE6DA_INITIAL_PROTOCOL_ID = (
    "macos-m1-worms-p2300-announced-p120-20260816-v1"
)
PHASE6DA_INITIAL_OS_VERSION = "26.6.2"
PHASE6DA_INITIAL_OS_BUILD = "25G83"
PHASE6DA_INITIAL_BASELINE_RECORD_ID = "6da-b-production-g000000"
PHASE6DA_INITIAL_CANDIDATE_RECORD_ID = (
    "6da-c-fast-block-dense-directory-g000001"
)
PHASE6DA_INITIAL_DIRECT_CONFIGURATION_BYTES = 46_394
PHASE6DA_INITIAL_DIRECT_CONFIGURATION_SHA256 = (
    "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
)
PHASE6DA_INITIAL_BASELINE_SELECTOR_BUNDLE = {
    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
    PHASE6DA_CANDIDATE_SELECTOR: False,
}
PHASE6DA_INITIAL_CANDIDATE_SELECTOR_BUNDLE = {
    **PHASE6DA_INITIAL_BASELINE_SELECTOR_BUNDLE,
    PHASE6DA_CANDIDATE_SELECTOR: True,
}
PHASE6DA_INITIAL_DIRECT_SCOPES = (
    {
        "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
        "record_id": PHASE6DA_INITIAL_BASELINE_RECORD_ID,
        "generation": 0,
        "parent_record_id": None,
        "original_role": "production",
        "candidate_selector": PHASE6DA_CANDIDATE_SELECTOR,
        "selector_bundle": PHASE6DA_INITIAL_BASELINE_SELECTOR_BUNDLE,
        "configuration_bytes": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_BYTES,
        "configuration_sha256": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_SHA256,
    },
    {
        "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
        "record_id": PHASE6DA_INITIAL_CANDIDATE_RECORD_ID,
        "generation": 1,
        "parent_record_id": PHASE6DA_INITIAL_BASELINE_RECORD_ID,
        "original_role": "candidate",
        "candidate_selector": PHASE6DA_CANDIDATE_SELECTOR,
        "selector_bundle": PHASE6DA_INITIAL_CANDIDATE_SELECTOR_BUNDLE,
        "configuration_bytes": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_BYTES,
        "configuration_sha256": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_SHA256,
    },
)
IFSM_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH"
)
IFSM_DIRECT_RECORD_ID = (
    "ifsm-c-fast-block-inline-fixed-signature-match-g000002"
)
IFSM_DIRECT_SCOPE = {
    "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
    "record_id": IFSM_DIRECT_RECORD_ID,
    "generation": 2,
    "parent_record_id": PHASE6DA_INITIAL_CANDIDATE_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": IFSM_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        IFSM_CANDIDATE_SELECTOR
    ],
    "configuration_bytes": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_BYTES,
    "configuration_sha256": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_SHA256,
}
FCFOS_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION"
)
FCFOS_DIRECT_RECORD_ID = (
    "fcfos-c-fast-block-callback-free-ordinary-specialization-g000003"
)
FCFOS_DIRECT_SCOPE = {
    "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
    "record_id": FCFOS_DIRECT_RECORD_ID,
    "generation": 3,
    "parent_record_id": IFSM_DIRECT_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": FCFOS_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        FCFOS_CANDIDATE_SELECTOR
    ],
    "configuration_bytes": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_BYTES,
    "configuration_sha256": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_SHA256,
}
UFSB_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING"
)
IMSE_COMPANION_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION"
)
UFSB_IMSE_DIRECT_RECORD_ID = (
    "ufsb-imse-c-fast-block-dispatch-bundle-g000003"
)
UFSB_IMSE_DIRECT_SCOPE = {
    "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
    "record_id": UFSB_IMSE_DIRECT_RECORD_ID,
    "generation": 3,
    "parent_record_id": IFSM_DIRECT_RECORD_ID,
    "original_role": "candidate",
    "candidate_selector": UFSB_CANDIDATE_SELECTOR,
    "selector_bundle": CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST[
        UFSB_CANDIDATE_SELECTOR
    ],
    "configuration_bytes": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_BYTES,
    "configuration_sha256": PHASE6DA_INITIAL_DIRECT_CONFIGURATION_SHA256,
}
COMMON_ENVIRONMENT_VALUES = {
    "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
    "TMPDIR": "/tmp",
    "LANG": "C",
    "LC_ALL": "C",
    "LUMEN_X64_A64_DYNAREC": "1",
    "LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT": "1",
    "LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI": "1",
    "LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE": "1",
    "LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING": "0",
    "LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES": "0",
    "LUMEN_X64_A64_DYNAREC_PACKED_256": "0",
    "LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE": "0",
    "LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE": "0",
    "LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT": "0",
    "LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE": "1",
    "LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE": "0",
    "LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION": "0",
    "LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION": "0",
    "LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY": "0",
    "LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY": "0",
    "LUMEN_GFX_COMMAND_PROVENANCE": "0",
    "LUMEN_GFX_ROOT_COMMAND_OWNERSHIP": "0",
    "LUMEN_GFX_NULL_SAMPLED_IMAGE": "1",
    "LUMEN_FRAME_PIPELINE_TRACE": "1",
    "LUMEN_FRAME_CHECKPOINT_PRESENT": "2300",
}
COMMON_IDENTITY_KEYS = frozenset({"HOME", "USER", "LOGNAME"})
FORBIDDEN_EXPERIMENT_FRAGMENTS = (
    "AUTOMATION",
    "CONTROLLER",
    "CUSTOM_LOG",
    "DEBUG",
    "DIAGNOSTIC",
    "HOST_WAIT_TRACE",
    "PRESENT_LIMIT",
    "PROFILE",
    "PROGRESS",
    "READBACK",
    "TELEMETRY",
    "VALIDATION",
    "WATCH",
)


class HarnessError(Exception):
    """A run prerequisite, live receipt, or sealed artifact is invalid."""


class _DuplicateKeyError(ValueError):
    pass


def _require(condition: bool, source: object, message: str) -> None:
    if not condition:
        raise HarnessError(f"{source}: {message}")


def _object_without_duplicates(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateKeyError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ValueError(f"non-finite JSON constant {value}")


def _load_json_payload(payload: bytes, source: object) -> Mapping[str, Any]:
    try:
        value = json.loads(
            payload.decode("utf-8", errors="strict"),
            parse_float=decimal.Decimal,
            parse_int=int,
            parse_constant=_reject_constant,
            object_pairs_hook=_object_without_duplicates,
        )
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise HarnessError(f"{source}: invalid strict JSON: {error}") from error
    _require(type(value) is dict, source, "JSON root must be an object")
    return value


def _load_json(path: pathlib.Path) -> Mapping[str, Any]:
    _require(path.is_file(), path, "missing regular JSON file")
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise HarnessError(f"{path}: cannot read JSON: {error}") from error
    return _load_json_payload(payload, path)


def _mapping(value: Any, source: object, name: str) -> Mapping[str, Any]:
    _require(type(value) is dict, source, f"{name} must be an object")
    return value


def _exact(value: Any, keys: frozenset[str], source: object, name: str) -> Mapping[str, Any]:
    result = _mapping(value, source, name)
    actual = frozenset(result)
    _require(actual == keys, source, f"{name} keys mismatch: missing={sorted(keys-actual)} extra={sorted(actual-keys)}")
    return result


def _string(value: Any, source: object, name: str) -> str:
    _require(type(value) is str and value != "" and value == value.strip(), source, f"{name} must be a non-empty edge-trimmed string")
    _require(all(ord(character) >= 0x20 for character in value), source, f"{name} contains a control character")
    return value


def _uint(value: Any, source: object, name: str) -> int:
    _require(type(value) is int and 0 <= value <= UINT64_MAX, source, f"{name} must be uint64")
    return value


def _decimal(value: Any, source: object, name: str) -> decimal.Decimal:
    _require(
        type(value) in (int, float, str, decimal.Decimal),
        source,
        f"{name} must be decimal-compatible",
    )
    try:
        result = decimal.Decimal(str(value))
    except decimal.InvalidOperation as error:
        raise HarnessError(f"{source}: {name} is not a decimal") from error
    _require(result.is_finite(), source, f"{name} must be finite")
    return result


def _bool(value: Any, source: object, name: str) -> bool:
    _require(type(value) is bool, source, f"{name} must be boolean")
    return value


def _sha(value: Any, source: object, name: str) -> str:
    result = _string(value, source, name)
    _require(HEX_SHA256_RE.fullmatch(result) is not None, source, f"{name} must be lowercase SHA-256")
    return result


def _canonical_bytes(value: Any) -> bytes:
    def normalize(item: Any) -> Any:
        if isinstance(item, decimal.Decimal):
            if item == item.to_integral_value():
                return int(item)
            return float(item)
        if type(item) is dict:
            return {key: normalize(child) for key, child in item.items()}
        if type(item) is list:
            return [normalize(child) for child in item]
        return item

    return json.dumps(
        normalize(value), ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


def _canonical_sha(value: Any) -> str:
    return hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _sha_file(path: pathlib.Path) -> str:
    _require(path.is_file() and not path.is_symlink(), path, "expected a regular non-symlink file")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise HarnessError(f"{path}: cannot hash: {error}") from error
    return digest.hexdigest()


def _sha_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _file_identity(value: os.stat_result) -> Tuple[int, int, int, int, int, int]:
    return (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )


def _capture_stable_regular_bytes(
    path: pathlib.Path,
    *,
    purpose: str,
    required_mode: Optional[int] = None,
    _after_open_hook: Optional[Callable[[], None]] = None,
) -> bytes:
    """Capture one non-symlink file while proving its pathname/inode stability."""

    try:
        path_before = os.lstat(path)
    except OSError as error:
        raise HarnessError(f"{path}: cannot inspect {purpose}: {error}") from error
    _require(stat.S_ISREG(path_before.st_mode), path, f"{purpose} must be a regular non-symlink file")
    _require(
        hasattr(os, "O_NOFOLLOW"),
        path,
        f"{purpose} requires O_NOFOLLOW support",
    )
    flags = os.O_RDONLY | os.O_NOFOLLOW
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise HarnessError(f"{path}: cannot open stable {purpose}: {error}") from error
    try:
        descriptor_before = os.fstat(descriptor)
        _require(
            stat.S_ISREG(descriptor_before.st_mode)
            and _file_identity(descriptor_before) == _file_identity(path_before),
            path,
            f"{purpose} pathname changed before capture",
        )
        if required_mode is not None:
            _require(
                stat.S_IMODE(descriptor_before.st_mode) == required_mode,
                path,
                f"{purpose} mode must be exactly {required_mode:04o}",
            )
        if _after_open_hook is not None:
            _after_open_hook()
        chunks = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        descriptor_after = os.fstat(descriptor)
        path_after = os.lstat(path)
    except OSError as error:
        raise HarnessError(f"{path}: cannot capture stable {purpose}: {error}") from error
    finally:
        os.close(descriptor)
    payload = b"".join(chunks)
    identity = _file_identity(descriptor_before)
    _require(
        identity == _file_identity(descriptor_after)
        and identity == _file_identity(path_after)
        and len(payload) == descriptor_after.st_size,
        path,
        f"{purpose} changed while it was being captured",
    )
    return payload


def _configuration_input_path(config: Mapping[str, Any]) -> pathlib.Path:
    path = pathlib.Path(config["paths"]["configuration"]).expanduser()
    if not path.is_absolute():
        path = pathlib.Path.cwd() / path
    return path.absolute()


def _configuration_scope_matches(
    config: Mapping[str, Any], scope: Mapping[str, Any]
) -> bool:
    provenance = config["provenance"]
    lineage = config["lineage"]
    selectors_value = provenance["selectors"]
    selector_bundle = scope["selector_bundle"]
    return (
        config["protocol"]["protocol_id"] == scope["protocol_id"]
        and provenance["record_id"] == scope["record_id"]
        and lineage["generation"] == scope["generation"]
        and lineage["parent_record_id"] == scope["parent_record_id"]
        and lineage["original_role"] == scope["original_role"]
        and provenance["candidate_selector"] == scope["candidate_selector"]
        and set(selectors_value) == set(selector_bundle)
        and all(
            type(selectors_value[name]) is type(expected)
            and selectors_value[name] == expected
            for name, expected in selector_bundle.items()
        )
    )


def _phase6da_initial_direct_scope_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    provenance = config["provenance"]
    record_id = provenance["record_id"]
    direct_record_ids = {
        scope["record_id"] for scope in PHASE6DA_INITIAL_DIRECT_SCOPES
    }
    signalled = record_id in direct_record_ids
    if not signalled:
        return None
    matches = [
        scope
        for scope in PHASE6DA_INITIAL_DIRECT_SCOPES
        if _configuration_scope_matches(config, scope)
    ]
    _require(
        len(matches) == 1,
        source,
        "phase-6DA initial direct configuration is outside its exact protocol, record, lineage, role, and typed selector-bundle scope",
    )
    scope = matches[0]
    os_comparability = config["comparability"]["os"]
    _require(
        os_comparability["name"] == "macOS"
        and os_comparability["version"] == PHASE6DA_INITIAL_OS_VERSION
        and os_comparability["build"] == PHASE6DA_INITIAL_OS_BUILD
        and os_comparability["policy"]
        == "fixed-version-and-build-within-epoch"
        and config["comparability"]["runtime"]["configuration_sha256"]
        == scope["configuration_sha256"],
        source,
        "phase-6DA initial direct configuration differs from its preregistered OS/build or raw configuration identity",
    )
    return scope


def _noda_projection_scope_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    signalled = (
        provenance["record_id"] == NODA_RECORD_ID
        or provenance["candidate_selector"] == NODA_CANDIDATE_SELECTOR
        or NODA_CANDIDATE_SELECTOR in selectors_value
    )
    if not signalled:
        return None
    _require(
        _configuration_scope_matches(config, NODA_CONFIGURATION_PROJECTION_SCOPE),
        source,
        "NODA configuration is outside its exact protocol, g5 record, g4 parent, role, primary selector, and typed g4-plus-NODA bundle scope",
    )
    _require(
        PBA_CANDIDATE_SELECTOR not in selectors_value,
        source,
        "NODA and PBA selectors cannot coexist in one persistent record",
    )
    os_comparability = config["comparability"]["os"]
    _require(
        os_comparability["name"] == "macOS"
        and os_comparability["version"] == PHASE6DA_INITIAL_OS_VERSION
        and os_comparability["build"] == PHASE6DA_INITIAL_OS_BUILD
        and os_comparability["policy"]
        == "fixed-version-and-build-within-epoch",
        source,
        "NODA configuration differs from the promoted Metal g4 epoch OS/build identity",
    )
    return NODA_CONFIGURATION_PROJECTION_SCOPE


def _metal_runtime_bundle_projection_scope_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    if (
        provenance["record_id"] == NODA_RECORD_ID
        or provenance["candidate_selector"] == NODA_CANDIDATE_SELECTOR
        or NODA_CANDIDATE_SELECTOR in selectors_value
    ):
        _noda_projection_scope_for(config, source)
        return None
    signalled = (
        provenance["record_id"] == METAL_RUNTIME_BUNDLE_RECORD_ID
        or provenance["candidate_selector"] == MSORE_CANDIDATE_SELECTOR
        or MSORE_CANDIDATE_SELECTOR in selectors_value
        or MSFC_COMPANION_SELECTOR in selectors_value
        or MDSSC_COMPANION_SELECTOR in selectors_value
    )
    if not signalled:
        return None
    _require(
        _configuration_scope_matches(
            config, METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE
        ),
        source,
        "MSORE+MSFC+MDSSC configuration is outside its exact protocol, record, lineage, role, primary selector, and typed retained-selector bundle scope",
    )
    os_comparability = config["comparability"]["os"]
    _require(
        os_comparability["name"] == "macOS"
        and os_comparability["version"] == PHASE6DA_INITIAL_OS_VERSION
        and os_comparability["build"] == PHASE6DA_INITIAL_OS_BUILD
        and os_comparability["policy"]
        == "fixed-version-and-build-within-epoch",
        source,
        "MSORE+MSFC+MDSSC configuration differs from the ALC epoch OS/build identity",
    )
    return METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE


def _alc_projection_scope_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    if (
        provenance["record_id"] == METAL_RUNTIME_BUNDLE_RECORD_ID
        or provenance["candidate_selector"] == MSORE_CANDIDATE_SELECTOR
        or MSORE_CANDIDATE_SELECTOR in selectors_value
        or MSFC_COMPANION_SELECTOR in selectors_value
        or MDSSC_COMPANION_SELECTOR in selectors_value
    ):
        return None
    signalled = (
        provenance["record_id"] == ALC_DIRECT_RECORD_ID
        or provenance["candidate_selector"] == ALC_CANDIDATE_SELECTOR
        or ALC_CANDIDATE_SELECTOR in selectors_value
    )
    if not signalled:
        return None
    _require(
        _configuration_scope_matches(config, ALC_CONFIGURATION_PROJECTION_SCOPE),
        source,
        "ALC configuration is outside its exact protocol, record, lineage, role, primary selector, and typed retained-selector bundle scope",
    )
    os_comparability = config["comparability"]["os"]
    _require(
        os_comparability["name"] == "macOS"
        and os_comparability["version"] == PHASE6DA_INITIAL_OS_VERSION
        and os_comparability["build"] == PHASE6DA_INITIAL_OS_BUILD
        and os_comparability["policy"]
        == "fixed-version-and-build-within-epoch",
        source,
        "ALC configuration differs from the IFSM epoch OS/build identity",
    )
    return ALC_CONFIGURATION_PROJECTION_SCOPE


def _fcfos_direct_scope_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    signalled = (
        provenance["record_id"] == FCFOS_DIRECT_RECORD_ID
        or provenance["candidate_selector"] == FCFOS_CANDIDATE_SELECTOR
        or FCFOS_CANDIDATE_SELECTOR in selectors_value
    )
    if not signalled:
        return None
    _require(
        _configuration_scope_matches(config, FCFOS_DIRECT_SCOPE),
        source,
        "FCFOS direct configuration is outside its exact protocol, record, lineage, role, primary selector, and typed retained-selector bundle scope",
    )
    os_comparability = config["comparability"]["os"]
    _require(
        os_comparability["name"] == "macOS"
        and os_comparability["version"] == PHASE6DA_INITIAL_OS_VERSION
        and os_comparability["build"] == PHASE6DA_INITIAL_OS_BUILD
        and os_comparability["policy"]
        == "fixed-version-and-build-within-epoch"
        and config["comparability"]["runtime"]["configuration_sha256"]
        == FCFOS_DIRECT_SCOPE["configuration_sha256"],
        source,
        "FCFOS direct configuration differs from the IFSM epoch OS/build or raw configuration identity",
    )
    return FCFOS_DIRECT_SCOPE


def _ufsb_imse_direct_scope_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    signalled = (
        provenance["record_id"] == UFSB_IMSE_DIRECT_RECORD_ID
        or provenance["candidate_selector"] == UFSB_CANDIDATE_SELECTOR
        or UFSB_CANDIDATE_SELECTOR in selectors_value
        or IMSE_COMPANION_SELECTOR in selectors_value
    )
    if not signalled:
        return None
    _require(
        _configuration_scope_matches(config, UFSB_IMSE_DIRECT_SCOPE),
        source,
        "UFSB+IMSE direct configuration is outside its exact protocol, record, lineage, role, primary selector, and typed retained-selector bundle scope",
    )
    os_comparability = config["comparability"]["os"]
    _require(
        os_comparability["name"] == "macOS"
        and os_comparability["version"] == PHASE6DA_INITIAL_OS_VERSION
        and os_comparability["build"] == PHASE6DA_INITIAL_OS_BUILD
        and os_comparability["policy"]
        == "fixed-version-and-build-within-epoch"
        and config["comparability"]["runtime"]["configuration_sha256"]
        == UFSB_IMSE_DIRECT_SCOPE["configuration_sha256"],
        source,
        "UFSB+IMSE direct configuration differs from the IFSM epoch OS/build or raw configuration identity",
    )
    return UFSB_IMSE_DIRECT_SCOPE


def _ifsm_direct_scope_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    metal_bundle_signalled = (
        provenance["record_id"] == METAL_RUNTIME_BUNDLE_RECORD_ID
        or provenance["candidate_selector"] == MSORE_CANDIDATE_SELECTOR
        or MSORE_CANDIDATE_SELECTOR in selectors_value
        or MSFC_COMPANION_SELECTOR in selectors_value
        or MDSSC_COMPANION_SELECTOR in selectors_value
    )
    if metal_bundle_signalled:
        _metal_runtime_bundle_projection_scope_for(config, source)
        return None
    alc_signalled = (
        provenance["record_id"] == ALC_DIRECT_RECORD_ID
        or provenance["candidate_selector"] == ALC_CANDIDATE_SELECTOR
        or ALC_CANDIDATE_SELECTOR in selectors_value
    )
    if alc_signalled:
        _alc_projection_scope_for(config, source)
        return None
    fcfos_signalled = (
        provenance["record_id"] == FCFOS_DIRECT_RECORD_ID
        or provenance["candidate_selector"] == FCFOS_CANDIDATE_SELECTOR
        or FCFOS_CANDIDATE_SELECTOR in selectors_value
    )
    if fcfos_signalled:
        _fcfos_direct_scope_for(config, source)
        return None
    combined_signalled = (
        provenance["record_id"] == UFSB_IMSE_DIRECT_RECORD_ID
        or provenance["candidate_selector"] == UFSB_CANDIDATE_SELECTOR
        or UFSB_CANDIDATE_SELECTOR in selectors_value
        or IMSE_COMPANION_SELECTOR in selectors_value
    )
    if combined_signalled:
        _ufsb_imse_direct_scope_for(config, source)
        return None
    signalled = (
        provenance["record_id"] == IFSM_DIRECT_RECORD_ID
        or provenance["candidate_selector"] == IFSM_CANDIDATE_SELECTOR
        or IFSM_CANDIDATE_SELECTOR in selectors_value
    )
    if not signalled:
        return None
    _require(
        _configuration_scope_matches(config, IFSM_DIRECT_SCOPE),
        source,
        "IFSM direct configuration is outside its exact protocol, record, lineage, role, and typed retained-selector bundle scope",
    )
    os_comparability = config["comparability"]["os"]
    _require(
        os_comparability["name"] == "macOS"
        and os_comparability["version"] == PHASE6DA_INITIAL_OS_VERSION
        and os_comparability["build"] == PHASE6DA_INITIAL_OS_BUILD
        and os_comparability["policy"]
        == "fixed-version-and-build-within-epoch"
        and config["comparability"]["runtime"]["configuration_sha256"]
        == IFSM_DIRECT_SCOPE["configuration_sha256"],
        source,
        "IFSM direct configuration differs from the promoted 6DA epoch OS/build or raw configuration identity",
    )
    return IFSM_DIRECT_SCOPE


def _phase6da_start_scope_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    if (
        config["provenance"]["candidate_selector"]
        != PHASE6DA_CANDIDATE_SELECTOR
    ):
        return None
    scopes = (
        PHASE6DA_CONFIGURATION_PROJECTION_SCOPE,
        *PHASE6DA_INITIAL_DIRECT_SCOPES,
    )
    matches = [
        scope for scope in scopes if _configuration_scope_matches(config, scope)
    ]
    _require(
        len(matches) == 1,
        source,
        "phase-6DA start is outside every preregistered historical or initial-pair scope",
    )
    if matches[0] in PHASE6DA_INITIAL_DIRECT_SCOPES:
        _phase6da_initial_direct_scope_for(config, source)
    return matches[0]


def _candidate_required_receipt_fields(
    config: Mapping[str, Any], source: object
) -> Mapping[str, str]:
    candidate_selector = config["provenance"]["candidate_selector"]
    result = dict(CANDIDATE_REQUIRED_RECEIPT_FIELDS.get(candidate_selector, {}))
    if candidate_selector == PHASE6DA_CANDIDATE_SELECTOR:
        selected = config["provenance"]["selectors"].get(candidate_selector)
        _require(
            type(selected) is bool,
            source,
            "phase-6DA candidate selector must be a JSON boolean",
        )
        expected = "1" if selected else "0"
        result["fast_block_dense_directory_requested"] = expected
        result["fast_block_dense_directory"] = expected
    return result


def _configuration_projection_contract_for(
    config: Mapping[str, Any], source: object
) -> Optional[Mapping[str, Any]]:
    """Resolve one preregistered contract from every provenance signal.

    Selector bundles are cumulative: the 6CQ bundle intentionally contains
    the retained 6CP selector.  Therefore selector-key discovery selects the
    highest-generation contract present, while an exact record-id or explicit
    candidate-selector signal must agree with it.
    """

    if (
        _phase6da_initial_direct_scope_for(config, source) is not None
        or _fcfos_direct_scope_for(config, source) is not None
        or _ufsb_imse_direct_scope_for(config, source) is not None
        or _ifsm_direct_scope_for(config, source) is not None
    ):
        return None

    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    by_selector = {
        contract["candidate_selector"]: contract
        for contract in CONFIGURATION_PROJECTION_SCOPES
    }
    signalled: Dict[str, Mapping[str, Any]] = {}

    for contract in CONFIGURATION_PROJECTION_SCOPES:
        if provenance["record_id"] == contract["record_id"]:
            signalled[contract["candidate_selector"]] = contract

    explicit_selector = provenance["candidate_selector"]
    if explicit_selector in by_selector:
        signalled[explicit_selector] = by_selector[explicit_selector]

    selector_contracts = [
        contract
        for contract in CONFIGURATION_PROJECTION_SCOPES
        if contract["candidate_selector"] in selectors_value
    ]
    if selector_contracts:
        highest_generation = max(
            contract["generation"] for contract in selector_contracts
        )
        highest = [
            contract
            for contract in selector_contracts
            if contract["generation"] == highest_generation
        ]
        primary_highest = [
            contract
            for contract in highest
            if contract["candidate_selector"] == explicit_selector
            or contract["record_id"] == provenance["record_id"]
        ]
        if (
            len(primary_highest) == 1
            and primary_highest[0]["candidate_selector"]
            == ALC_CANDIDATE_SELECTOR
            and {
                contract["candidate_selector"] for contract in highest
            }
            == {PHASE6DA_CANDIDATE_SELECTOR, ALC_CANDIDATE_SELECTOR}
        ):
            highest = primary_highest
        _require(
            len(highest) == 1,
            source,
            "configuration projection selector signals are ambiguous",
        )
        signalled[highest[0]["candidate_selector"]] = highest[0]

    _require(
        len(signalled) <= 1,
        source,
        "configuration projection record/selector signals name different contracts",
    )
    return next(iter(signalled.values())) if signalled else None


def _require_configuration_projection_scope(
    config: Mapping[str, Any], source: object, contract: Mapping[str, Any]
) -> None:
    lineage = config["lineage"]
    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    selector_bundle = contract["selector_bundle"]
    _require(
        config["protocol"]["protocol_id"] == contract["protocol_id"]
        and provenance["record_id"] == contract["record_id"]
        and lineage["generation"] == contract["generation"]
        and lineage["parent_record_id"] == contract["parent_record_id"]
        and lineage["original_role"] == contract["original_role"]
        and provenance["candidate_selector"]
        == contract["candidate_selector"]
        and set(selectors_value) == set(selector_bundle)
        and all(
            type(selectors_value[name]) is type(expected)
            and selectors_value[name] == expected
            for name, expected in selector_bundle.items()
        ),
        source,
        "configuration projection is outside its exact protocol, record, lineage, and typed selector-bundle scope",
    )


def _configuration_projection_policy_for(
    config: Mapping[str, Any],
    source: object,
    *,
    test_mode: bool,
    override: Optional[Mapping[str, Any]],
) -> Optional[Mapping[str, Any]]:
    contract = _configuration_projection_contract_for(config, source)
    if override is not None:
        _require(test_mode, source, "configuration projection policy override is test-only")
        _require(
            contract is not None
            and override.get("candidate_selector")
            == contract["candidate_selector"],
            source,
            "configuration projection override differs from the signalled contract",
        )
        return override
    if contract is None:
        return None

    _require_configuration_projection_scope(config, source, contract)
    if contract["candidate_selector"] == PHASE6CP_CANDIDATE_SELECTOR:
        policy = PHASE6CP_CONFIGURATION_PROJECTION_POLICY
    elif contract["candidate_selector"] == PHASE6CQ_CANDIDATE_SELECTOR:
        policy = PHASE6CQ_CONFIGURATION_PROJECTION_POLICY
        _require(
            policy is not None,
            source,
            "phase-6CQ byte-exact configuration projection transform is not preregistered",
        )
    elif contract["candidate_selector"] == PHASE6CS_CANDIDATE_SELECTOR:
        policy = PHASE6CS_CONFIGURATION_PROJECTION_POLICY
        _require(
            policy is not None,
            source,
            "phase-6CS byte-exact configuration projection transform is not preregistered",
        )
    elif contract["candidate_selector"] == PHASE6DA_CANDIDATE_SELECTOR:
        policy = PHASE6DA_CONFIGURATION_PROJECTION_POLICY
        _require(
            policy is not None,
            source,
            "phase-6DA byte-exact configuration projection transform is not preregistered",
        )
    elif contract["candidate_selector"] == ALC_CANDIDATE_SELECTOR:
        policy = ALC_CONFIGURATION_PROJECTION_POLICY
        _require(
            policy is not None,
            source,
            "ALC byte-exact configuration projection transform is not preregistered",
        )
    elif contract["candidate_selector"] == MSORE_CANDIDATE_SELECTOR:
        policy = METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_POLICY
        _require(
            policy is not None,
            source,
            "MSORE+MSFC+MDSSC byte-exact configuration projection transform is not preregistered",
        )
    elif contract["candidate_selector"] == NODA_CANDIDATE_SELECTOR:
        policy = NODA_CONFIGURATION_PROJECTION_POLICY
        _require(
            policy is not None,
            source,
            "NODA byte-exact configuration projection transform is not preregistered",
        )
    else:  # pragma: no cover - guarded by CONFIGURATION_PROJECTION_SCOPES.
        raise HarnessError(f"{source}: unsupported configuration projection contract")
    identity_keys = [
        "protocol_id",
        "record_id",
        "generation",
        "parent_record_id",
        "original_role",
        "candidate_selector",
        "selector_bundle",
    ]
    if "raw_bytes" in contract or "raw_sha256" in contract:
        identity_keys.extend(("raw_bytes", "raw_sha256"))
    if "projection_configured" in contract:
        identity_keys.append("projection_configured")
    _require(
        all(
            policy[key] == contract[key]
            for key in identity_keys
        ),
        source,
        "configuration projection policy identity differs from its preregistered scope",
    )
    return policy


def _phase6cp_configuration_projection_receipt(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    policy: Mapping[str, Any],
) -> Mapping[str, Any]:
    lineage = config["lineage"]
    provenance = config["provenance"]
    selectors_value = provenance["selectors"]
    selector_bundle = policy["selector_bundle"]
    _require(
        config["protocol"]["protocol_id"] == policy["protocol_id"]
        and provenance["record_id"] == policy["record_id"]
        and lineage["generation"] == policy["generation"]
        and lineage["parent_record_id"] == policy["parent_record_id"]
        and lineage["original_role"] == policy["original_role"]
        and provenance["candidate_selector"] == policy["candidate_selector"]
        and set(selectors_value) == set(selector_bundle)
        and all(
            type(selectors_value[name]) is type(expected)
            and selectors_value[name] == expected
            for name, expected in selector_bundle.items()
        ),
        source,
        "configuration projection is outside its exact 6CP g2 scope",
    )

    expected_epoch_sha256 = config["comparability"]["runtime"][
        "configuration_sha256"
    ]
    _require(
        expected_epoch_sha256 == policy["projected_sha256"],
        source,
        "configuration projection differs from the immutable epoch digest",
    )
    _require(
        len(raw) == policy["raw_bytes"]
        and _sha_bytes(raw) == policy["raw_sha256"],
        source,
        "raw configuration does not match the preregistered 6CP input",
    )

    phase6cm = policy["phase6cm_block"]
    phase6co = policy["phase6co_block"]
    removed = phase6cm + phase6co
    _require(
        len(phase6cm) == 124
        and _sha_bytes(phase6cm) == policy["phase6cm_block_sha256"]
        and len(phase6co) == 122
        and _sha_bytes(phase6co) == policy["phase6co_block_sha256"]
        and len(removed) == policy["removed_bytes"]
        and _sha_bytes(removed) == policy["removed_segment_sha256"],
        source,
        "configuration projection policy block constants are inconsistent",
    )
    _require(
        raw.count(phase6cm) == 1
        and raw.count(phase6co) == 1
        and raw.count(removed) == 1,
        source,
        "6CM/6CO OFF blocks must be contiguous, ordered, and exact-once",
    )
    projected = raw.replace(removed, b"", 1)
    _require(
        len(projected) == policy["projected_bytes"]
        and _sha_bytes(projected) == policy["projected_sha256"],
        source,
        "projected configuration does not reproduce the immutable epoch",
    )
    return {
        "schema": PHASE6CP_CONFIGURATION_PROJECTION_SCHEMA,
        "attestation": PHASE6CP_CONFIGURATION_PROJECTION_ATTESTATION,
        "policy": PHASE6CP_CONFIGURATION_PROJECTION_POLICY_ID,
        "mode": PHASE6CP_CONFIGURATION_PROJECTION_MODE,
        "record_id": provenance["record_id"],
        "candidate_selector": provenance["candidate_selector"],
        "raw_bytes": len(raw),
        "raw_sha256": _sha_bytes(raw),
        "phase6cm_block_sha256": _sha_bytes(phase6cm),
        "phase6co_block_sha256": _sha_bytes(phase6co),
        "removed_bytes": len(removed),
        "removed_segment_sha256": _sha_bytes(removed),
        "projected_bytes": len(projected),
        "projected_sha256": _sha_bytes(projected),
        "epoch_configuration_sha256": expected_epoch_sha256,
    }


def _phase6cq_restore_contract_payload(
    operations: Any, source: object
) -> bytes:
    _require(
        type(operations) in (list, tuple) and bool(operations),
        source,
        "6CQ restore operations must be a non-empty ordered sequence",
    )
    payload = bytearray(PHASE6CQ_CONFIGURATION_RESTORE_CONTRACT_PREFIX)
    names = []
    for index, operation in enumerate(operations):
        _require(
            type(operation) in (list, tuple) and len(operation) == 3,
            source,
            f"6CQ restore operation {index} must contain name/current/restored",
        )
        name, current, restored = operation
        _require(
            type(name) is str
            and re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            source,
            f"6CQ restore operation {index} has a non-canonical name",
        )
        try:
            name_bytes = name.encode("ascii", errors="strict")
        except UnicodeError as error:  # pragma: no cover - regex is ASCII-only.
            raise HarnessError(
                f"{source}: 6CQ restore operation {index} name is not ASCII: {error}"
            ) from error
        _require(
            type(current) is bytes
            and type(restored) is bytes
            and bool(current)
            and bool(restored)
            and current != restored,
            source,
            f"6CQ restore operation {name} has invalid byte literals",
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
        source,
        "6CQ restore operation names must be unique",
    )
    return bytes(payload)


def _phase6cq_configuration_projection_receipt(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    policy: Mapping[str, Any],
) -> Mapping[str, Any]:
    _require_configuration_projection_scope(config, source, policy)
    expected_epoch_sha256 = config["comparability"]["runtime"][
        "configuration_sha256"
    ]
    _require(
        expected_epoch_sha256 == policy["projected_sha256"],
        source,
        "6CQ configuration projection differs from the immutable epoch digest",
    )
    _require(
        len(raw) == policy["raw_bytes"]
        and _sha_bytes(raw) == policy["raw_sha256"],
        source,
        "raw configuration does not match the preregistered 6CQ input",
    )

    operations = policy["restore_operations"]
    contract_payload = _phase6cq_restore_contract_payload(operations, source)
    _require(
        len(operations) == policy["restore_operation_count"]
        and len(contract_payload)
        == policy["restore_operations_contract_bytes"]
        and _sha_bytes(contract_payload)
        == policy["restore_operations_contract_sha256"],
        source,
        "6CQ restore-operation literals differ from the preregistered contract",
    )

    restored = raw
    for name, current, replacement in operations:
        _require(
            restored.count(current) == 1,
            source,
            f"6CQ restore operation {name} current bytes are not exact-once",
        )
        _require(
            restored.count(replacement) == 0,
            source,
            f"6CQ restore operation {name} is already present before restoration",
        )
        restored = restored.replace(current, replacement, 1)
    _require(
        len(restored) == policy["restored_bytes"]
        and _sha_bytes(restored) == policy["restored_sha256"],
        source,
        "6CQ restored configuration does not reproduce the sealed 6CP raw identity",
    )

    phase6cm = policy["phase6cm_block"]
    phase6co = policy["phase6co_block"]
    removed = phase6cm + phase6co
    _require(
        len(phase6cm) == 124
        and _sha_bytes(phase6cm) == policy["phase6cm_block_sha256"]
        and len(phase6co) == 122
        and _sha_bytes(phase6co) == policy["phase6co_block_sha256"]
        and len(removed) == policy["removed_bytes"]
        and _sha_bytes(removed) == policy["removed_segment_sha256"],
        source,
        "6CQ strip block literals differ from the preregistered contract",
    )
    _require(
        restored.count(phase6cm) == 1
        and restored.count(phase6co) == 1
        and restored.count(removed) == 1,
        source,
        "6CQ 6CM/6CO OFF blocks must be contiguous, ordered, and exact-once",
    )
    projected = restored.replace(removed, b"", 1)
    _require(
        len(projected) == policy["projected_bytes"]
        and _sha_bytes(projected) == policy["projected_sha256"],
        source,
        "6CQ projected configuration does not reproduce the immutable epoch",
    )
    return {
        "schema": PHASE6CQ_CONFIGURATION_PROJECTION_SCHEMA,
        "attestation": PHASE6CQ_CONFIGURATION_PROJECTION_ATTESTATION,
        "policy": PHASE6CQ_CONFIGURATION_PROJECTION_POLICY_ID,
        "mode": PHASE6CQ_CONFIGURATION_PROJECTION_MODE,
        "record_id": config["provenance"]["record_id"],
        "candidate_selector": config["provenance"]["candidate_selector"],
        "raw_bytes": len(raw),
        "raw_sha256": _sha_bytes(raw),
        "restore_operation_count": len(operations),
        "restore_operations_contract_bytes": len(contract_payload),
        "restore_operations_contract_sha256": _sha_bytes(contract_payload),
        "restored_bytes": len(restored),
        "restored_sha256": _sha_bytes(restored),
        "phase6cm_block_sha256": _sha_bytes(phase6cm),
        "phase6co_block_sha256": _sha_bytes(phase6co),
        "removed_bytes": len(removed),
        "removed_segment_sha256": _sha_bytes(removed),
        "projected_bytes": len(projected),
        "projected_sha256": _sha_bytes(projected),
        "epoch_configuration_sha256": expected_epoch_sha256,
    }


def _phase6cs_restore_contract_payload(
    operations: Any, source: object
) -> bytes:
    _require(
        type(operations) in (list, tuple) and bool(operations),
        source,
        "6CS restore operations must be a non-empty ordered sequence",
    )
    payload = bytearray(PHASE6CS_CONFIGURATION_RESTORE_CONTRACT_PREFIX)
    names = []
    for index, operation in enumerate(operations):
        _require(
            type(operation) in (list, tuple) and len(operation) == 3,
            source,
            f"6CS restore operation {index} must contain name/current/restored",
        )
        name, current, restored = operation
        _require(
            type(name) is str
            and re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            source,
            f"6CS restore operation {index} has a non-canonical name",
        )
        try:
            name_bytes = name.encode("ascii", errors="strict")
        except UnicodeError as error:  # pragma: no cover - regex is ASCII-only.
            raise HarnessError(
                f"{source}: 6CS restore operation {index} name is not ASCII: {error}"
            ) from error
        _require(
            type(current) is bytes
            and type(restored) is bytes
            and bool(current)
            and bool(restored)
            and current != restored,
            source,
            f"6CS restore operation {name} has invalid byte literals",
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
        source,
        "6CS restore operation names must be unique",
    )
    return bytes(payload)


def _phase6cs_configuration_projection_receipt(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    policy: Mapping[str, Any],
) -> Mapping[str, Any]:
    _require_configuration_projection_scope(config, source, policy)
    expected_epoch_sha256 = config["comparability"]["runtime"][
        "configuration_sha256"
    ]
    _require(
        expected_epoch_sha256 == policy["projected_sha256"],
        source,
        "6CS configuration projection differs from the immutable epoch digest",
    )
    _require(
        len(raw) == policy["raw_bytes"]
        and _sha_bytes(raw) == policy["raw_sha256"],
        source,
        "raw configuration does not match the preregistered 6CS input",
    )

    operations = policy["restore_operations"]
    contract_payload = _phase6cs_restore_contract_payload(operations, source)
    _require(
        len(operations) == policy["restore_operation_count"]
        and len(contract_payload)
        == policy["restore_operations_contract_bytes"]
        and _sha_bytes(contract_payload)
        == policy["restore_operations_contract_sha256"],
        source,
        "6CS restore-operation literals differ from the preregistered contract",
    )

    restored = raw
    for name, current, replacement in operations:
        _require(
            restored.count(current) == 1,
            source,
            f"6CS restore operation {name} current bytes are not exact-once",
        )
        _require(
            restored.count(replacement) == 0,
            source,
            f"6CS restore operation {name} is already present before restoration",
        )
        restored = restored.replace(current, replacement, 1)
    _require(
        len(restored) == policy["restored_bytes"]
        and _sha_bytes(restored) == policy["restored_sha256"],
        source,
        "6CS restored configuration does not reproduce the sealed 6CP raw identity",
    )

    phase6cm = policy["phase6cm_block"]
    phase6co = policy["phase6co_block"]
    removed = phase6cm + phase6co
    _require(
        len(phase6cm) == 124
        and _sha_bytes(phase6cm) == policy["phase6cm_block_sha256"]
        and len(phase6co) == 122
        and _sha_bytes(phase6co) == policy["phase6co_block_sha256"]
        and len(removed) == policy["removed_bytes"]
        and _sha_bytes(removed) == policy["removed_segment_sha256"],
        source,
        "6CS strip block literals differ from the preregistered contract",
    )
    _require(
        restored.count(phase6cm) == 1
        and restored.count(phase6co) == 1
        and restored.count(removed) == 1,
        source,
        "6CS 6CM/6CO OFF blocks must be contiguous, ordered, and exact-once",
    )
    projected = restored.replace(removed, b"", 1)
    _require(
        len(projected) == policy["projected_bytes"]
        and _sha_bytes(projected) == policy["projected_sha256"],
        source,
        "6CS projected configuration does not reproduce the immutable epoch",
    )
    return {
        "schema": PHASE6CS_CONFIGURATION_PROJECTION_SCHEMA,
        "attestation": PHASE6CS_CONFIGURATION_PROJECTION_ATTESTATION,
        "policy": PHASE6CS_CONFIGURATION_PROJECTION_POLICY_ID,
        "mode": PHASE6CS_CONFIGURATION_PROJECTION_MODE,
        "record_id": config["provenance"]["record_id"],
        "candidate_selector": config["provenance"]["candidate_selector"],
        "raw_bytes": len(raw),
        "raw_sha256": _sha_bytes(raw),
        "restore_operation_count": len(operations),
        "restore_operations_contract_bytes": len(contract_payload),
        "restore_operations_contract_sha256": _sha_bytes(contract_payload),
        "restored_bytes": len(restored),
        "restored_sha256": _sha_bytes(restored),
        "phase6cm_block_sha256": _sha_bytes(phase6cm),
        "phase6co_block_sha256": _sha_bytes(phase6co),
        "removed_bytes": len(removed),
        "removed_segment_sha256": _sha_bytes(removed),
        "projected_bytes": len(projected),
        "projected_sha256": _sha_bytes(projected),
        "epoch_configuration_sha256": expected_epoch_sha256,
    }


def _phase6da_restore_contract_payload(
    operations: Any, source: object
) -> bytes:
    _require(
        type(operations) in (list, tuple) and bool(operations),
        source,
        "6DA restore operations must be a non-empty ordered sequence",
    )
    payload = bytearray(PHASE6DA_CONFIGURATION_RESTORE_CONTRACT_PREFIX)
    names = []
    for index, operation in enumerate(operations):
        _require(
            type(operation) in (list, tuple) and len(operation) == 3,
            source,
            f"6DA restore operation {index} must contain name/current/restored",
        )
        name, current, restored = operation
        _require(
            type(name) is str
            and re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            source,
            f"6DA restore operation {index} has a non-canonical name",
        )
        try:
            name_bytes = name.encode("ascii", errors="strict")
        except UnicodeError as error:  # pragma: no cover - regex is ASCII-only.
            raise HarnessError(
                f"{source}: 6DA restore operation {index} name is not ASCII: {error}"
            ) from error
        _require(
            type(current) is bytes
            and type(restored) is bytes
            and bool(current)
            and bool(restored)
            and current != restored,
            source,
            f"6DA restore operation {name} has invalid byte literals",
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
        source,
        "6DA restore operation names must be unique",
    )
    return bytes(payload)


def _phase6da_configuration_projection_receipt(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    policy: Mapping[str, Any],
) -> Mapping[str, Any]:
    _require_configuration_projection_scope(config, source, policy)
    expected_epoch_sha256 = config["comparability"]["runtime"][
        "configuration_sha256"
    ]
    _require(
        expected_epoch_sha256 == policy["projected_sha256"],
        source,
        "6DA configuration projection differs from the immutable epoch digest",
    )
    _require(
        len(raw) == policy["raw_bytes"]
        and _sha_bytes(raw) == policy["raw_sha256"],
        source,
        "raw configuration does not match the preregistered 6DA input",
    )

    operations = policy["restore_operations"]
    contract_payload = _phase6da_restore_contract_payload(operations, source)
    _require(
        len(operations) == policy["restore_operation_count"]
        and len(contract_payload)
        == policy["restore_operations_contract_bytes"]
        and _sha_bytes(contract_payload)
        == policy["restore_operations_contract_sha256"],
        source,
        "6DA restore-operation literals differ from the preregistered contract",
    )

    restored = raw
    for name, current, replacement in operations:
        _require(
            restored.count(current) == 1,
            source,
            f"6DA restore operation {name} current bytes are not exact-once",
        )
        _require(
            restored.count(replacement) == 0,
            source,
            f"6DA restore operation {name} is already present before restoration",
        )
        restored = restored.replace(current, replacement, 1)
    _require(
        len(restored) == policy["restored_bytes"]
        and _sha_bytes(restored) == policy["restored_sha256"],
        source,
        "6DA restored configuration does not reproduce the sealed 6CP raw identity",
    )

    phase6cm = policy["phase6cm_block"]
    phase6co = policy["phase6co_block"]
    removed = phase6cm + phase6co
    _require(
        len(phase6cm) == 124
        and _sha_bytes(phase6cm) == policy["phase6cm_block_sha256"]
        and len(phase6co) == 122
        and _sha_bytes(phase6co) == policy["phase6co_block_sha256"]
        and len(removed) == policy["removed_bytes"]
        and _sha_bytes(removed) == policy["removed_segment_sha256"],
        source,
        "6DA strip block literals differ from the preregistered contract",
    )
    _require(
        restored.count(phase6cm) == 1
        and restored.count(phase6co) == 1
        and restored.count(removed) == 1,
        source,
        "6DA 6CM/6CO OFF blocks must be contiguous, ordered, and exact-once",
    )
    projected = restored.replace(removed, b"", 1)
    _require(
        len(projected) == policy["projected_bytes"]
        and _sha_bytes(projected) == policy["projected_sha256"],
        source,
        "6DA projected configuration does not reproduce the immutable epoch",
    )
    return {
        "schema": PHASE6DA_CONFIGURATION_PROJECTION_SCHEMA,
        "attestation": PHASE6DA_CONFIGURATION_PROJECTION_ATTESTATION,
        "policy": PHASE6DA_CONFIGURATION_PROJECTION_POLICY_ID,
        "mode": PHASE6DA_CONFIGURATION_PROJECTION_MODE,
        "record_id": config["provenance"]["record_id"],
        "candidate_selector": config["provenance"]["candidate_selector"],
        "raw_bytes": len(raw),
        "raw_sha256": _sha_bytes(raw),
        "restore_operation_count": len(operations),
        "restore_operations_contract_bytes": len(contract_payload),
        "restore_operations_contract_sha256": _sha_bytes(contract_payload),
        "restored_bytes": len(restored),
        "restored_sha256": _sha_bytes(restored),
        "phase6cm_block_sha256": _sha_bytes(phase6cm),
        "phase6co_block_sha256": _sha_bytes(phase6co),
        "removed_bytes": len(removed),
        "removed_segment_sha256": _sha_bytes(removed),
        "projected_bytes": len(projected),
        "projected_sha256": _sha_bytes(projected),
        "epoch_configuration_sha256": expected_epoch_sha256,
    }


def _alc_configuration_strip_contract_payload(
    operations: Any, source: object
) -> bytes:
    _require(
        type(operations) in (list, tuple) and bool(operations),
        source,
        "ALC strip operations must be a non-empty ordered sequence",
    )
    payload = bytearray(ALC_CONFIGURATION_STRIP_CONTRACT_PREFIX)
    names = []
    for index, operation in enumerate(operations):
        _require(
            type(operation) in (list, tuple) and len(operation) == 2,
            source,
            f"ALC strip operation {index} must contain name/block",
        )
        name, block = operation
        _require(
            type(name) is str
            and re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None,
            source,
            f"ALC strip operation {index} has a non-canonical name",
        )
        _require(
            type(block) is bytes and bool(block),
            source,
            f"ALC strip operation {name} has invalid block bytes",
        )
        names.append(name)
        payload.extend(name.encode("ascii", errors="strict"))
        payload.extend(b"\0")
        payload.extend(block)
    _require(
        len(names) == len(set(names)),
        source,
        "ALC strip operation names must be unique",
    )
    return bytes(payload)


def _alc_configuration_projection_receipt(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    policy: Mapping[str, Any],
) -> Mapping[str, Any]:
    _require_configuration_projection_scope(config, source, policy)
    expected_epoch_sha256 = config["comparability"]["runtime"][
        "configuration_sha256"
    ]
    _require(
        expected_epoch_sha256 == policy["projected_sha256"],
        source,
        "ALC configuration projection differs from the immutable IFSM epoch digest",
    )
    _require(
        len(raw) == policy["raw_bytes"]
        and _sha_bytes(raw) == policy["raw_sha256"],
        source,
        "raw configuration does not match the preregistered ALC input",
    )

    operations = policy["strip_operations"]
    contract_payload = _alc_configuration_strip_contract_payload(
        operations, source
    )
    _require(
        len(operations) == policy["strip_operation_count"]
        and len(contract_payload)
        == policy["strip_operations_contract_bytes"]
        and _sha_bytes(contract_payload)
        == policy["strip_operations_contract_sha256"],
        source,
        "ALC strip-operation literals differ from the preregistered contract",
    )

    positions = []
    projected = raw
    removed_parts = []
    for name, block in operations:
        _require(
            raw.count(block) == 1,
            source,
            f"ALC strip operation {name} block is not exact-once",
        )
        positions.append(raw.index(block))
        removed_parts.append(block)
        projected = projected.replace(block, b"", 1)
    _require(
        positions == sorted(positions) and len(set(positions)) == len(positions),
        source,
        "ALC strip blocks are not present in preregistered source order",
    )
    removed = b"".join(removed_parts)
    _require(
        len(removed) == policy["removed_bytes"]
        and _sha_bytes(removed) == policy["removed_segment_sha256"],
        source,
        "ALC removed blocks differ from the preregistered contract",
    )
    _require(
        len(projected) == policy["projected_bytes"]
        and _sha_bytes(projected) == policy["projected_sha256"],
        source,
        "ALC projected configuration does not reproduce the immutable IFSM epoch",
    )
    return {
        "schema": ALC_CONFIGURATION_PROJECTION_SCHEMA,
        "attestation": ALC_CONFIGURATION_PROJECTION_ATTESTATION,
        "policy": ALC_CONFIGURATION_PROJECTION_POLICY_ID,
        "mode": ALC_CONFIGURATION_PROJECTION_MODE,
        "record_id": config["provenance"]["record_id"],
        "candidate_selector": config["provenance"]["candidate_selector"],
        "raw_bytes": len(raw),
        "raw_sha256": _sha_bytes(raw),
        "strip_operation_count": len(operations),
        "strip_operations_contract_bytes": len(contract_payload),
        "strip_operations_contract_sha256": _sha_bytes(contract_payload),
        "removed_bytes": len(removed),
        "removed_segment_sha256": _sha_bytes(removed),
        "projected_bytes": len(projected),
        "projected_sha256": _sha_bytes(projected),
        "epoch_configuration_sha256": expected_epoch_sha256,
    }


def _metal_runtime_bundle_configuration_projection_receipt(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    policy: Mapping[str, Any],
) -> Mapping[str, Any]:
    # The g4 record changes runtime-only selectors, not the ALC CMake-cache
    # delta.  Keep one byte-transform implementation and bind a distinct
    # attestation/policy identity to the resulting receipt.
    receipt = dict(
        _alc_configuration_projection_receipt(config, raw, source, policy)
    )
    receipt.update(
        {
            "schema": METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCHEMA,
            "attestation": (
                METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_ATTESTATION
            ),
            "policy": METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_POLICY_ID,
            "mode": METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_MODE,
        }
    )
    return receipt


def _noda_configuration_projection_receipt(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    policy: Mapping[str, Any],
) -> Mapping[str, Any]:
    receipt = dict(
        _alc_configuration_projection_receipt(config, raw, source, policy)
    )
    receipt.update(
        {
            "schema": NODA_CONFIGURATION_PROJECTION_SCHEMA,
            "attestation": NODA_CONFIGURATION_PROJECTION_ATTESTATION,
            "policy": NODA_CONFIGURATION_PROJECTION_POLICY_ID,
            "mode": NODA_CONFIGURATION_PROJECTION_MODE,
        }
    )
    return receipt


def _configuration_projection_receipt_for(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    policy: Mapping[str, Any],
) -> Mapping[str, Any]:
    selector = policy["candidate_selector"]
    if selector == PHASE6CP_CANDIDATE_SELECTOR:
        return _phase6cp_configuration_projection_receipt(
            config, raw, source, policy
        )
    if selector == PHASE6CQ_CANDIDATE_SELECTOR:
        return _phase6cq_configuration_projection_receipt(
            config, raw, source, policy
        )
    if selector == PHASE6CS_CANDIDATE_SELECTOR:
        return _phase6cs_configuration_projection_receipt(
            config, raw, source, policy
        )
    if selector == PHASE6DA_CANDIDATE_SELECTOR:
        return _phase6da_configuration_projection_receipt(
            config, raw, source, policy
        )
    if selector == ALC_CANDIDATE_SELECTOR:
        return _alc_configuration_projection_receipt(
            config, raw, source, policy
        )
    if selector == MSORE_CANDIDATE_SELECTOR:
        return _metal_runtime_bundle_configuration_projection_receipt(
            config, raw, source, policy
        )
    if selector == NODA_CANDIDATE_SELECTOR:
        return _noda_configuration_projection_receipt(
            config, raw, source, policy
        )
    raise HarnessError(
        f"{source}: unsupported configuration projection receipt contract"
    )


def _configuration_projection_receipt_keys_for(
    policy: Mapping[str, Any], source: object
) -> frozenset[str]:
    selector = policy["candidate_selector"]
    if selector == PHASE6CP_CANDIDATE_SELECTOR:
        return PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_KEYS
    if selector == PHASE6CQ_CANDIDATE_SELECTOR:
        return PHASE6CQ_CONFIGURATION_PROJECTION_RECEIPT_KEYS
    if selector == PHASE6CS_CANDIDATE_SELECTOR:
        return PHASE6CS_CONFIGURATION_PROJECTION_RECEIPT_KEYS
    if selector == PHASE6DA_CANDIDATE_SELECTOR:
        return PHASE6DA_CONFIGURATION_PROJECTION_RECEIPT_KEYS
    if selector == ALC_CANDIDATE_SELECTOR:
        return ALC_CONFIGURATION_PROJECTION_RECEIPT_KEYS
    if selector == MSORE_CANDIDATE_SELECTOR:
        return METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_RECEIPT_KEYS
    if selector == NODA_CANDIDATE_SELECTOR:
        return NODA_CONFIGURATION_PROJECTION_RECEIPT_KEYS
    raise HarnessError(
        f"{source}: unsupported configuration projection receipt-key contract"
    )


def _tree_digest(path: pathlib.Path) -> str:
    _require(path.is_dir() and not path.is_symlink(), path, "expected a regular directory")
    records = []
    for child in sorted(path.rglob("*"), key=lambda item: item.relative_to(path).as_posix()):
        relative = child.relative_to(path).as_posix()
        _require(not child.is_symlink(), child, "symlinks are forbidden in frozen trees")
        if child.is_dir():
            records.append({"path": relative, "type": "directory"})
        elif child.is_file():
            records.append({"path": relative, "type": "file", "size": child.stat().st_size, "sha256": _sha_file(child)})
        else:
            raise HarnessError(f"{child}: unsupported tree entry")
    return _canonical_sha(records)


def _file_set_digest(paths: Sequence[pathlib.Path]) -> str:
    _require(bool(paths), "rom_files", "at least one ROM file is required")
    records = []
    for path in sorted((item.resolve() for item in paths), key=str):
        records.append({"path": str(path), "size": path.stat().st_size, "sha256": _sha_file(path)})
    return _canonical_sha(records)


def _atomic_bytes(path: pathlib.Path, payload: bytes, *, replace: bool = False, mode: int = 0o444) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}.{time.time_ns()}")
    try:
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        descriptor = os.open(temporary, flags, 0o600)
        try:
            view = memoryview(payload)
            while view:
                written = os.write(descriptor, view)
                _require(written > 0, path, "short atomic write")
                view = view[written:]
            os.fsync(descriptor)
            os.fchmod(descriptor, mode)
        finally:
            os.close(descriptor)
        if replace:
            os.replace(temporary, path)
        else:
            try:
                # link(2) publishes the fully written inode only if the immutable
                # destination is still absent. Unlike check-then-replace, this is
                # an atomic no-clobber operation across competing processes.
                os.link(temporary, path, follow_symlinks=False)
            except FileExistsError as error:
                raise HarnessError(f"{path}: refusing to overwrite") from error
            temporary.unlink()
        directory_fd = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        # Ordinary I/O errors and interrupted Python execution must not leave
        # unindexed staging files behind. A hard process kill can still do so;
        # seal_run rejects such debris below before publishing the record.
        temporary.unlink(missing_ok=True)


def _write_fd_all(descriptor: int, payload: bytes, source: object) -> None:
    view = memoryview(payload)
    while view:
        written = os.write(descriptor, view)
        _require(written > 0, source, "short file write")
        view = view[written:]


def _json_bytes(value: Any) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            indent=2,
        ).encode("utf-8")
        + b"\n"
    )


def _atomic_json(path: pathlib.Path, value: Any, *, replace: bool = False, mode: int = 0o444) -> None:
    _atomic_bytes(path, _json_bytes(value), replace=replace, mode=mode)


def _publish_exact_bytes(path: pathlib.Path, payload: bytes, *, mode: int = 0o444) -> None:
    """Publish once, or accept an existing byte-identical immutable value."""

    def validate_existing() -> None:
        _require(
            path.is_file() and not path.is_symlink(),
            path,
            "existing immutable publication is not a regular non-symlink file",
        )
        try:
            existing = path.read_bytes()
        except OSError as error:
            raise HarnessError(f"{path}: cannot read immutable publication: {error}") from error
        _require(existing == payload, path, "conflicting immutable publication")
        _require(
            path.stat().st_mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)
            == 0,
            path,
            "existing immutable publication is writable",
        )

    if path.exists() or path.is_symlink():
        validate_existing()
        return
    try:
        _atomic_bytes(path, payload, mode=mode)
    except HarnessError:
        # A concurrent identical publisher may have won after the absence
        # check. It is safe only when the resulting immutable bytes match.
        if path.exists() or path.is_symlink():
            validate_existing()
            return
        raise


def _configuration_projection_preflight(
    config: Mapping[str, Any],
    raw: bytes,
    source: object,
    *,
    test_mode: bool,
    policy_override: Optional[Mapping[str, Any]],
) -> Optional[Mapping[str, Any]]:
    direct_scope = _phase6da_initial_direct_scope_for(config, source)
    if direct_scope is None:
        direct_scope = _fcfos_direct_scope_for(config, source)
    if direct_scope is None:
        direct_scope = _ufsb_imse_direct_scope_for(config, source)
    if direct_scope is None:
        direct_scope = _ifsm_direct_scope_for(config, source)
    policy = _configuration_projection_policy_for(
        config,
        source,
        test_mode=test_mode,
        override=policy_override,
    )
    if policy is None:
        if direct_scope is not None:
            direct_configuration_error = (
                "FCFOS generation-3 direct scope requires the exact preregistered CMake configuration"
                if direct_scope is FCFOS_DIRECT_SCOPE
                else (
                    "UFSB+IMSE generation-3 direct scope requires the exact preregistered CMake configuration"
                    if direct_scope is UFSB_IMSE_DIRECT_SCOPE
                    else (
                        "IFSM generation-2 direct scope requires the exact preregistered CMake configuration"
                        if direct_scope is IFSM_DIRECT_SCOPE
                        else "phase-6DA initial pair requires the exact preregistered direct CMake configuration"
                    )
                )
            )
            _require(
                len(raw) == direct_scope["configuration_bytes"]
                and _sha_bytes(raw) == direct_scope["configuration_sha256"],
                source,
                direct_configuration_error,
            )
        _require(
            _sha_bytes(raw)
            == config["comparability"]["runtime"]["configuration_sha256"],
            source,
            "runtime configuration hash mismatch",
        )
        return None
    return _configuration_projection_receipt_for(
        config, raw, source, policy
    )


def _publish_configuration_projection_artifacts(
    run_root: pathlib.Path,
    raw: bytes,
    receipt: Mapping[str, Any],
) -> Mapping[str, str]:
    raw_path = run_root / PHASE6CP_CONFIGURATION_CACHE_RAW_NAME
    receipt_path = run_root / PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME
    receipt_payload = _json_bytes(receipt)
    _atomic_bytes(raw_path, raw, mode=0o444)
    _atomic_bytes(receipt_path, receipt_payload, mode=0o444)
    published_raw = _capture_stable_regular_bytes(
        raw_path,
        purpose="published raw configuration evidence",
        required_mode=0o444,
    )
    published_receipt = _capture_stable_regular_bytes(
        receipt_path,
        purpose="published configuration projection receipt",
        required_mode=0o444,
    )
    _require(
        published_raw == raw and published_receipt == receipt_payload,
        run_root,
        "configuration projection publication changed before freeze",
    )
    return {
        "configuration_cache_raw_sha256": _sha_bytes(published_raw),
        "configuration_projection_receipt_sha256": _sha_bytes(
            published_receipt
        ),
    }


def _validate_configuration_projection_artifacts(
    run_root: pathlib.Path,
    config: Mapping[str, Any],
    freeze: Mapping[str, Any],
    *,
    test_mode: bool,
    policy_override: Optional[Mapping[str, Any]],
) -> Mapping[str, str]:
    policy = _configuration_projection_policy_for(
        config,
        run_root,
        test_mode=test_mode,
        override=policy_override,
    )
    raw_path = run_root / PHASE6CP_CONFIGURATION_CACHE_RAW_NAME
    receipt_path = run_root / PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME
    freeze_keys = {
        "configuration_cache_raw_sha256",
        "configuration_projection_receipt_sha256",
    }
    if policy is None:
        _require(
            not raw_path.exists()
            and not raw_path.is_symlink()
            and not receipt_path.exists()
            and not receipt_path.is_symlink()
            and not (freeze_keys & set(freeze)),
            run_root,
            "unexpected configuration projection evidence for a legacy raw comparison",
        )
        return {}

    _require(
        freeze_keys <= set(freeze),
        run_root,
        "configuration projection freeze binding is missing",
    )
    raw = _capture_stable_regular_bytes(
        raw_path,
        purpose="raw configuration evidence",
        required_mode=0o444,
    )
    receipt_payload = _capture_stable_regular_bytes(
        receipt_path,
        purpose="configuration projection receipt",
        required_mode=0o444,
    )
    receipt = _exact(
        _load_json_payload(receipt_payload, receipt_path),
        _configuration_projection_receipt_keys_for(policy, receipt_path),
        receipt_path,
        "configuration projection receipt",
    )
    expected = _configuration_projection_receipt_for(
        config, raw, receipt_path, policy
    )
    _require(
        receipt == expected,
        receipt_path,
        "configuration projection receipt does not reproduce from raw evidence",
    )
    raw_sha256 = _sha_bytes(raw)
    receipt_sha256 = _sha_bytes(receipt_payload)
    _require(
        raw_sha256
        == _sha(
            freeze["configuration_cache_raw_sha256"],
            run_root,
            "freeze.configuration_cache_raw_sha256",
        )
        and receipt_sha256
        == _sha(
            freeze["configuration_projection_receipt_sha256"],
            run_root,
            "freeze.configuration_projection_receipt_sha256",
        ),
        run_root,
        "configuration projection artifacts changed after launch",
    )
    current_raw = _capture_stable_regular_bytes(
        _configuration_input_path(config),
        purpose="runtime CMake configuration",
    )
    _require(
        current_raw == raw,
        _configuration_input_path(config),
        "runtime CMake configuration changed after projection capture",
    )
    return {
        "configuration_cache_raw": raw_sha256,
        "configuration_projection_receipt": receipt_sha256,
    }


def _uptime_raw_ns() -> int:
    _require(hasattr(time, "CLOCK_UPTIME_RAW"), "clock", "CLOCK_UPTIME_RAW is unavailable")
    return time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)


def _run_command(arguments: Sequence[str], *, cwd: Optional[pathlib.Path] = None) -> bytes:
    try:
        completed = subprocess.run(arguments, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    except OSError as error:
        raise HarnessError(f"cannot execute {arguments[0]}: {error}") from error
    _require(completed.returncode == 0, arguments[0], f"command failed: {completed.stderr.decode('utf-8', 'replace').strip()}")
    return completed.stdout


def _git_snapshot(repo_root: pathlib.Path) -> Tuple[str, str]:
    revision = _run_command(("/usr/bin/git", "rev-parse", "HEAD"), cwd=repo_root).decode("ascii").strip()
    _require(REVISION_RE.fullmatch(revision) is not None, repo_root, "invalid Git revision")
    diff = _run_command(("/usr/bin/git", "diff", "--binary", "HEAD"), cwd=repo_root)
    untracked_output = _run_command(("/usr/bin/git", "ls-files", "--others", "--exclude-standard", "-z"), cwd=repo_root)
    untracked = []
    for raw_name in untracked_output.split(b"\0"):
        if not raw_name:
            continue
        name = raw_name.decode("utf-8", "strict")
        candidate = (repo_root / name).resolve()
        _require(candidate.is_relative_to(repo_root.resolve()), candidate, "untracked path escapes repository")
        if candidate.is_file() and not candidate.is_symlink():
            untracked.append({"path": name, "sha256": _sha_file(candidate), "size": candidate.stat().st_size})
        else:
            raise HarnessError(f"{candidate}: unsupported untracked entry")
    digest = hashlib.sha256()
    digest.update(diff)
    digest.update(b"\0UNTRACKED\0")
    digest.update(_canonical_bytes(untracked))
    return revision, digest.hexdigest()


def _load_report_module(path: pathlib.Path) -> Any:
    _require(path.is_file(), path, "missing persistent performance report tool")
    spec = importlib.util.spec_from_file_location("_lumen_persistent_performance_report", path)
    _require(spec is not None and spec.loader is not None, path, "cannot load report module")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        previous_dont_write_bytecode = sys.dont_write_bytecode
        sys.dont_write_bytecode = True
        try:
            spec.loader.exec_module(module)
        finally:
            sys.dont_write_bytecode = previous_dont_write_bytecode
    except Exception as error:
        raise HarnessError(f"{path}: cannot import report validator: {error}") from error
    return module


def _validate_protocol(value: Any, source: object) -> Mapping[str, Any]:
    protocol = _exact(value, PROTOCOL_KEYS, source, "protocol")
    _string(protocol["protocol_id"], source, "protocol.protocol_id")
    _require(_uint(protocol["protocol_revision"], source, "protocol.protocol_revision") > 0, source, "protocol revision must be positive")
    for key, expected in PROTOCOL_CONSTANTS.items():
        _require(protocol[key] == expected, source, f"protocol.{key} must equal {expected!r}")
    return protocol


def _validate_comparability(value: Any, source: object) -> Mapping[str, Any]:
    comparability = _exact(value, frozenset(COMPARABILITY_SHAPES), source, "comparability")
    sections: Dict[str, Mapping[str, Any]] = {}
    for name, keys in COMPARABILITY_SHAPES.items():
        sections[name] = _exact(comparability[name], keys, source, f"comparability.{name}")
    for key in ("host_id", "model", "architecture"):
        _string(sections["host"][key], source, f"comparability.host.{key}")
    _require(sections["host"]["architecture"] == "arm64", source, "host architecture must be arm64")
    for key in sections["os"]:
        _string(sections["os"][key], source, f"comparability.os.{key}")
    _require(sections["os"]["name"] == "macOS", source, "OS must be macOS")
    for key in sections["power"]:
        _string(sections["power"][key], source, f"comparability.power.{key}")
    for key in ("rom_sha256", "lua_sha256", "save_template_sha256"):
        _sha(sections["workload"][key], source, f"comparability.workload.{key}")
    _string(sections["workload"]["cache_policy"], source, "comparability.workload.cache_policy")
    if sections["workload"]["cache_seed_sha256"] is not None:
        _sha(sections["workload"]["cache_seed_sha256"], source, "comparability.workload.cache_seed_sha256")
    for key in ("sterile_environment_sha256", "configuration_sha256"):
        _sha(sections["runtime"][key], source, f"comparability.runtime.{key}")
    _string(sections["runtime"]["backend"], source, "comparability.runtime.backend")
    for key in sections["scenes"]:
        _string(sections["scenes"][key], source, f"comparability.scenes.{key}")
    expected_schemas = {"harness": HARNESS_SCHEMA, "announcement_sidecar": ANCHOR_SCHEMA, "report": REPORT_TOOL_SCHEMA}
    _require(sections["tool_schemas"] == expected_schemas, source, "tool schema contract mismatch")
    _require(
        sections["workload"]["cache_policy"] == "fresh-empty"
        and sections["workload"]["cache_seed_sha256"] is None,
        source,
        "initial harness v1 supports only a fresh-empty runtime cache",
    )
    return comparability


def _validate_json_receipt(value: Any, source: object, name: str) -> None:
    if value is None or type(value) in (bool, str, int):
        if type(value) is str:
            _string(value, source, name)
        elif type(value) is int:
            _uint(value, source, name)
        return
    if type(value) is list:
        for index, item in enumerate(value):
            _validate_json_receipt(item, source, f"{name}[{index}]")
        return
    if type(value) is dict:
        for key, item in value.items():
            _string(key, source, f"{name} key")
            _validate_json_receipt(item, source, f"{name}.{key}")
        return
    raise HarnessError(f"{source}: unsupported selector value in {name}")


def _selector_env_text(value: Any) -> str:
    if type(value) is bool:
        return "1" if value else "0"
    if type(value) is int:
        return str(value)
    if type(value) is str:
        return value
    raise HarnessError("selector values must be bool, int, or string for the launch environment")


def _receipt_fields(line: str, prefix: str, source: object) -> Mapping[str, str]:
    _require(line.startswith(prefix), source, f"receipt does not start with {prefix!r}")
    fields: Dict[str, str] = {}
    for token in line[len(prefix) :].strip().split():
        _require(token.count("=") == 1, source, f"malformed receipt token {token!r}")
        name, value = token.split("=", 1)
        _require(
            re.fullmatch(r"[a-z][a-z0-9_]*", name) is not None and value != "",
            source,
            f"malformed receipt field {token!r}",
        )
        _require(name not in fields, source, f"duplicate receipt field {name}")
        fields[name] = value
    _require(bool(fields), source, "empty structured receipt")
    return fields


def _selector_value_from_receipt(expected: Any, text: str, source: object) -> Any:
    if type(expected) is bool:
        _require(text in ("0", "1"), source, "boolean selector receipt must be 0 or 1")
        return text == "1"
    if type(expected) is int:
        _require(re.fullmatch(r"(?:0|-?[1-9][0-9]*)", text) is not None, source, "integer selector receipt is not canonical")
        return int(text)
    if type(expected) is str:
        return text
    raise HarnessError(f"{source}: unsupported selector receipt type")


def _effective_selectors_from_logs(
    logs: Sequence[str], config: Mapping[str, Any], source: object
) -> Tuple[Mapping[str, Any], Mapping[str, str]]:
    expected = config["provenance"]["selectors"]
    contracts = config["provenance"]["selector_receipts"]
    noda_scope = _noda_projection_scope_for(config, source)
    effective: Dict[str, Any] = {}
    observed_lines: Dict[str, str] = {}
    all_lines = [line for text in logs for line in text.splitlines()]
    by_prefix: Dict[str, Tuple[str, Mapping[str, str]]] = {}
    for name in sorted(expected):
        contract = contracts[name]
        prefix = contract["prefix"]
        if prefix not in by_prefix:
            matches = [line for line in all_lines if line.startswith(prefix)]
            _require(len(matches) == 1, source, f"selector receipt prefix {prefix!r} is not exact-once")
            by_prefix[prefix] = (matches[0], _receipt_fields(matches[0], prefix, source))
        line, fields = by_prefix[prefix]
        field = contract["field"]
        _require(field in fields, source, f"selector receipt field {field!r} is missing for {name}")
        value = _selector_value_from_receipt(expected[name], fields[field], source)
        if (
            noda_scope is not None
            and name
            == "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
        ):
            _require(
                value is True
                and fields.get("stable_negative_route") == "0"
                and fields.get("stable_negative_route_superseded") == "1",
                source,
                "NODA requires 6CL requested=1, effective=0, and superseded=1",
            )
            effective[name] = False
        else:
            _require(
                value == expected[name],
                source,
                f"effective selector {name} differs from expected",
            )
            effective[name] = value
        observed_lines[prefix] = line
    mismatches = {
        name for name in expected if effective.get(name) != expected[name]
    }
    _require(
        mismatches
        == (
            {"LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"}
            if noda_scope is not None
            else set()
        ),
        source,
        "expected/effective selector mismatch is outside the sole NODA 6CL supersession",
    )
    if noda_scope is not None:
        _require(
            effective.get(NODA_CANDIDATE_SELECTOR) is True,
            source,
            "NODA candidate selector is not effectively enabled",
        )
    return effective, observed_lines


def _validate_start_config(
    path: pathlib.Path, *, payload: Optional[bytes] = None
) -> Mapping[str, Any]:
    loaded = _load_json(path) if payload is None else _load_json_payload(payload, path)
    root = _exact(loaded, START_KEYS, path, "root")
    schema = _exact(root["schema"], frozenset({"name", "version"}), path, "schema")
    _require(schema == {"name": START_SCHEMA_NAME, "version": START_SCHEMA_VERSION}, path, "unsupported start schema")
    _validate_protocol(root["protocol"], path)
    comparability = _validate_comparability(root["comparability"], path)
    provenance = _exact(
        root["provenance"],
        frozenset(
            {
                "record_id",
                "run_receipt",
                "selectors",
                "selector_receipts",
                "candidate_selector",
            }
        ),
        path,
        "provenance",
    )
    record_id = _string(provenance["record_id"], path, "provenance.record_id")
    _require(SAFE_ID_RE.fullmatch(record_id) is not None, path, "unsafe record_id")
    _string(provenance["run_receipt"], path, "provenance.run_receipt")
    selectors_value = _mapping(provenance["selectors"], path, "provenance.selectors")
    _require(bool(selectors_value), path, "at least one selector is required")
    _validate_json_receipt(selectors_value, path, "provenance.selectors")
    candidate_selector = _string(
        provenance["candidate_selector"], path, "provenance.candidate_selector"
    )
    _require(
        candidate_selector in selectors_value
        and candidate_selector in CANDIDATE_MARKER_ALLOWLIST,
        path,
        "candidate_selector is absent or has no audited marker contract",
    )
    _require(
        type(selectors_value[candidate_selector]) is bool,
        path,
        "candidate selector must be boolean",
    )
    selector_bundle = CANDIDATE_SELECTOR_BUNDLE_ALLOWLIST.get(candidate_selector)
    if (
        selector_bundle is not None
        and candidate_selector != PHASE6DA_CANDIDATE_SELECTOR
    ):
        _require(
            set(selectors_value) == set(selector_bundle)
            and all(
                type(selectors_value[name]) is type(expected)
                and selectors_value[name] == expected
                for name, expected in selector_bundle.items()
            ),
            path,
            "candidate selector set/values differ from the audited retained-selector bundle",
        )
    candidate_receipt_requirements = _candidate_required_receipt_fields(
        root, path
    )
    if candidate_receipt_requirements:
        run_receipt = provenance["run_receipt"]
        _require(
            run_receipt.startswith(A64_CONFIGURATION_PREFIX),
            path,
            "candidate run receipt must be the structured A64 configuration line",
        )
        configured_receipt_fields = _receipt_fields(
            run_receipt, A64_CONFIGURATION_PREFIX, path
        )
        for field, expected in candidate_receipt_requirements.items():
            _require(
                configured_receipt_fields.get(field) == expected,
                path,
                f"candidate run receipt requires {field}={expected}",
            )
    selector_receipts = _mapping(
        provenance["selector_receipts"], path, "provenance.selector_receipts"
    )
    _require(
        set(selector_receipts) == set(selectors_value),
        path,
        "selector receipt keys must match selector keys exactly",
    )
    for name, value in selector_receipts.items():
        _require(name in SELECTOR_RECEIPT_ALLOWLIST, path, f"selector {name} has no audited receipt contract")
        receipt = _exact(
            value,
            frozenset({"prefix", "field"}),
            path,
            f"provenance.selector_receipts.{name}",
        )
        prefix = _string(receipt["prefix"], path, f"selector_receipts.{name}.prefix")
        _require(prefix.endswith(":"), path, f"selector receipt prefix {prefix!r} must end with ':'")
        field = _string(receipt["field"], path, f"selector_receipts.{name}.field")
        _require(
            re.fullmatch(r"[a-z][a-z0-9_]*", field) is not None,
            path,
            f"selector receipt field {field!r} is not canonical",
        )
        expected_receipt_contract = (
            NODA_SELECTOR_RECEIPT_OVERRIDES.get(
                name, SELECTOR_RECEIPT_ALLOWLIST[name]
            )
            if candidate_selector == NODA_CANDIDATE_SELECTOR
            else SELECTOR_RECEIPT_ALLOWLIST[name]
        )
        _require(
            receipt == expected_receipt_contract,
            path,
            f"selector receipt contract for {name} differs from the audited mapping",
        )

    lineage = _exact(root["lineage"], frozenset({"generation", "parent_record_id", "original_role"}), path, "lineage")
    generation = _uint(lineage["generation"], path, "lineage.generation")
    role = lineage["original_role"]
    _require(role in ("production", "candidate"), path, "invalid original_role")
    if generation == 0:
        _require(lineage["parent_record_id"] is None and role == "production", path, "generation zero must be parentless production")
    else:
        _require(type(lineage["parent_record_id"]) is str and SAFE_ID_RE.fullmatch(lineage["parent_record_id"]) is not None, path, "nonzero generation requires safe parent_record_id")
        _require(role == "candidate", path, "nonzero generation must be candidate")
    _phase6da_start_scope_for(root, path)
    _noda_projection_scope_for(root, path)
    _metal_runtime_bundle_projection_scope_for(root, path)
    _alc_projection_scope_for(root, path)
    _fcfos_direct_scope_for(root, path)
    _ufsb_imse_direct_scope_for(root, path)
    _ifsm_direct_scope_for(root, path)

    marker = _exact(root["candidate_marker"], frozenset({"required", "prefix", "required_tokens", "coverage_required", "coverage_token"}), path, "candidate_marker")
    required = _bool(marker["required"], path, "candidate_marker.required")
    coverage_required = _bool(marker["coverage_required"], path, "candidate_marker.coverage_required")
    _require(required == (role == "candidate"), path, "candidate marker required must match role")
    _require(
        selectors_value[candidate_selector] == (role == "candidate"),
        path,
        "candidate selector must be OFF for production B and ON for candidate C",
    )
    if marker["prefix"] is not None:
        _string(marker["prefix"], path, "candidate_marker.prefix")
    _require(type(marker["required_tokens"]) is list, path, "candidate_marker.required_tokens must be a list")
    for token in marker["required_tokens"]:
        _string(token, path, "candidate_marker.required_tokens[]")
    if required:
        _require(marker["prefix"] is not None, path, "candidate marker prefix is required")
        _require("status=clean" in marker["required_tokens"], path, "candidate marker must require status=clean")
    else:
        _require(not marker["required_tokens"], path, "production marker tokens must be empty")
    if coverage_required:
        _require(required, path, "coverage requires candidate marker")
        coverage_token = _string(marker["coverage_token"], path, "candidate_marker.coverage_token")
        _require(coverage_token in marker["required_tokens"], path, "coverage token must be required explicitly")
    else:
        _require(marker["coverage_token"] is None, path, "unused coverage token must be null")
    marker_contract = CANDIDATE_MARKER_ALLOWLIST[candidate_selector]
    if required:
        _require(
            marker["prefix"] == marker_contract["prefix"]
            and marker["required_tokens"] == marker_contract["required_tokens"]
            and marker["coverage_required"] is True
            and marker["coverage_token"] == marker_contract["coverage_token"],
            path,
            "candidate marker differs from the audited selector contract",
        )
    else:
        _require(
            marker["prefix"] == marker_contract["prefix"]
            and marker["required_tokens"] == []
            and marker["coverage_required"] is False
            and marker["coverage_token"] is None,
            path,
            "production marker absence contract is not exact",
        )

    paths = _exact(root["paths"], frozenset({"binary", "lua", "save_template", "save_special_relative", "rom_files", "cache_seed", "configuration", "report_tool", "repo_root"}), path, "paths")
    for key in ("binary", "lua", "save_template", "save_special_relative", "configuration", "report_tool", "repo_root"):
        _string(paths[key], path, f"paths.{key}")
    _require(type(paths["rom_files"]) is list and bool(paths["rom_files"]), path, "paths.rom_files must be a non-empty list")
    for item in paths["rom_files"]:
        _string(item, path, "paths.rom_files[]")
    _require(paths["cache_seed"] is None, path, "initial harness v1 requires a null/fresh-empty cache seed")
    save_relative = pathlib.PurePosixPath(paths["save_special_relative"])
    _require(not save_relative.is_absolute() and ".." not in save_relative.parts, path, "unsafe save_special_relative")

    launch = _exact(root["launch"], frozenset({"arguments", "common_environment", "experiment_environment"}), path, "launch")
    _require(type(launch["arguments"]) is list, path, "launch.arguments must be a list")
    for argument in launch["arguments"]:
        _string(argument, path, "launch.arguments[]")
    common = _mapping(launch["common_environment"], path, "launch.common_environment")
    experiment = _mapping(launch["experiment_environment"], path, "launch.experiment_environment")
    _require(not (set(common) & set(experiment)), path, "common and experiment environment overlap")
    for name, value in {**common, **experiment}.items():
        _require(re.fullmatch(r"[A-Z_][A-Z0-9_]*", name) is not None, path, f"unsafe environment name {name!r}")
        _string(value, path, f"environment.{name}")
    required_common = set(COMMON_ENVIRONMENT_VALUES) | set(COMMON_IDENTITY_KEYS)
    common_extras = set(common) - required_common
    _require(
        all(name.startswith("TEST_") for name in common_extras),
        path,
        f"untracked common environment keys: {sorted(common_extras)}",
    )
    _require(
        required_common <= set(common),
        path,
        f"missing sterile common environment keys: {sorted(required_common-set(common))}",
    )
    for name, expected in COMMON_ENVIRONMENT_VALUES.items():
        _require(common.get(name) == expected, path, f"common environment {name} must be {expected!r}")
    account = pwd.getpwuid(os.getuid())
    _require(common.get("HOME") == account.pw_dir, path, "HOME differs from the launching account")
    _require(common.get("USER") == account.pw_name, path, "USER differs from the launching account")
    _require(common.get("LOGNAME") == account.pw_name, path, "LOGNAME differs from the launching account")
    _require("LUMEN_FRAME_PRESENT_LIMIT" not in common and "LUMEN_FRAME_PRESENT_LIMIT" not in experiment, path, "present limit is forbidden")
    _require(_canonical_sha(common) == comparability["runtime"]["sterile_environment_sha256"], path, "sterile environment hash mismatch")
    _require(
        set(experiment) == set(selectors_value),
        path,
        "experiment_environment must contain exactly the declared selectors",
    )
    for name, value in selectors_value.items():
        _require(
            not any(fragment in name for fragment in FORBIDDEN_EXPERIMENT_FRAGMENTS),
            path,
            f"diagnostic or input-affecting experiment selector is forbidden: {name}",
        )
        _require(
            not any(character.isspace() for character in _selector_env_text(value)),
            path,
            f"selector {name} cannot be represented by a receipt token",
        )
        _require(name in experiment and experiment[name] == _selector_env_text(value), path, f"selector {name} is not represented exactly in experiment_environment")
    return root


def _capture_start_config(path: pathlib.Path) -> Tuple[Mapping[str, Any], bytes]:
    """Capture once, canonicalize once, and validate the bytes later published."""

    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        raise HarnessError(f"{path}: cannot open start config snapshot: {error}") from error
    try:
        before = os.fstat(descriptor)
        _require(stat.S_ISREG(before.st_mode), path, "start config must be a regular file")
        chunks = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
        after = os.fstat(descriptor)
    except OSError as error:
        raise HarnessError(f"{path}: cannot capture start config snapshot: {error}") from error
    finally:
        os.close(descriptor)
    identity_before = (
        before.st_dev,
        before.st_ino,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    identity_after = (
        after.st_dev,
        after.st_ino,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    captured = b"".join(chunks)
    _require(
        identity_before == identity_after and len(captured) == after.st_size,
        path,
        "start config changed while it was being captured",
    )
    parsed = _load_json_payload(captured, path)
    canonical = _canonical_bytes(parsed) + b"\n"
    # Validation deliberately consumes the canonical representation.  Those
    # exact bytes, rather than a later pathname read, become start-config.json.
    return _validate_start_config(path, payload=canonical), canonical


def _resolved_paths(config: Mapping[str, Any]) -> Mapping[str, Any]:
    paths = config["paths"]
    result: Dict[str, Any] = {}
    for key in ("binary", "lua", "save_template", "report_tool", "repo_root"):
        result[key] = pathlib.Path(paths[key]).expanduser().resolve()
    result["configuration_input"] = _configuration_input_path(config)
    result["configuration"] = result["configuration_input"].resolve()
    result["rom_files"] = [pathlib.Path(item).expanduser().resolve() for item in paths["rom_files"]]
    result["cache_seed"] = pathlib.Path(paths["cache_seed"]).expanduser().resolve() if paths["cache_seed"] is not None else None
    result["save_special_relative"] = pathlib.PurePosixPath(paths["save_special_relative"])
    return result


def _platform_preflight(
    config: Mapping[str, Any],
    paths: Mapping[str, Any],
    *,
    test_mode: bool,
    configuration_projection_policy: Optional[Mapping[str, Any]] = None,
) -> Optional[Tuple[bytes, Mapping[str, Any]]]:
    for key in ("binary", "lua", "configuration", "report_tool"):
        _require(paths[key].is_file() and not paths[key].is_symlink(), paths[key], f"missing regular {key}")
    _require(os.access(paths["binary"], os.X_OK), paths["binary"], "binary is not executable")
    _require(paths["save_template"].is_dir(), paths["save_template"], "missing save template")
    for rom_file in paths["rom_files"]:
        _require(rom_file.is_file() and not rom_file.is_symlink(), rom_file, "missing ROM input")
    if paths["cache_seed"] is not None:
        _require(paths["cache_seed"].exists() and not paths["cache_seed"].is_symlink(), paths["cache_seed"], "missing cache seed")

    workload = config["comparability"]["workload"]
    _require(_sha_file(paths["lua"]) == workload["lua_sha256"], paths["lua"], "Lua hash mismatch")
    _require(_tree_digest(paths["save_template"]) == workload["save_template_sha256"], paths["save_template"], "save-template hash mismatch")
    _require(_file_set_digest(paths["rom_files"]) == workload["rom_sha256"], "ROM", "ROM hash mismatch")
    if paths["cache_seed"] is None:
        _require(workload["cache_seed_sha256"] is None, "cache", "cache seed hash must be null")
    else:
        observed_cache = _tree_digest(paths["cache_seed"]) if paths["cache_seed"].is_dir() else _sha_file(paths["cache_seed"])
        _require(observed_cache == workload["cache_seed_sha256"], paths["cache_seed"], "cache seed mismatch")
    configuration_raw = _capture_stable_regular_bytes(
        paths["configuration_input"], purpose="runtime CMake configuration"
    )
    configuration_receipt = _configuration_projection_preflight(
        config,
        configuration_raw,
        paths["configuration_input"],
        test_mode=test_mode,
        policy_override=configuration_projection_policy,
    )
    configuration_evidence = (
        (configuration_raw, configuration_receipt)
        if configuration_receipt is not None
        else None
    )
    _require(config["comparability"]["runtime"]["backend"] == "NativeMetal", "backend", "only NativeMetal is valid")
    if test_mode:
        return configuration_evidence
    _require(
        not any(name.startswith("TEST_") for name in config["launch"]["common_environment"]),
        "environment",
        "test-only environment is forbidden in a real run",
    )
    _require(platform.system() == "Darwin" and platform.machine() == "arm64", "host", "native macOS arm64 is required")
    translated = _run_command(("/usr/sbin/sysctl", "-in", "sysctl.proc_translated")).decode("ascii").strip()
    _require(translated == "0", "host", "Rosetta translation is forbidden")
    file_description = _run_command(("/usr/bin/file", "-b", str(paths["binary"]))).decode("utf-8", "strict")
    _require("Mach-O 64-bit executable arm64" in file_description, paths["binary"], "binary is not thin/native arm64")
    model = _run_command(("/usr/sbin/sysctl", "-n", "hw.model")).decode("utf-8", "strict").strip()
    _require(model == config["comparability"]["host"]["model"], "host", "hardware model differs from epoch")
    product_version = _run_command(("/usr/bin/sw_vers", "-productVersion")).decode("utf-8", "strict").strip()
    build_version = _run_command(("/usr/bin/sw_vers", "-buildVersion")).decode("utf-8", "strict").strip()
    _require(product_version == config["comparability"]["os"]["version"], "host", "macOS version differs from epoch")
    _require(build_version == config["comparability"]["os"]["build"], "host", "macOS build differs from epoch")
    try:
        configuration_text = configuration_raw.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise HarnessError(
            f"{paths['configuration_input']}: invalid UTF-8 runtime CMake configuration: {error}"
        ) from error
    configuration_lines = configuration_text.splitlines()
    _require("CMAKE_BUILD_TYPE:STRING=Release" in configuration_lines, paths["configuration"], "Release CMake build is required")
    _require("LUMEN_APPLE_NATIVE_METAL:BOOL=ON" in configuration_text.splitlines(), paths["configuration"], "NativeMetal CMake selector is not ON")
    enabled_diagnostics = [
        line
        for line in configuration_lines
        if re.fullmatch(r"LUMEN_[A-Z0-9_]*(?:TELEMETRY|PROFILE|PROFILER)[A-Z0-9_]*:BOOL=ON", line)
    ]
    _require(not enabled_diagnostics, paths["configuration"], f"diagnostic/profile CMake options are ON: {enabled_diagnostics}")
    dependencies = _run_command(("/usr/bin/otool", "-L", str(paths["binary"]))).decode("utf-8", "strict")
    _require("/Metal.framework/" in dependencies, paths["binary"], "binary does not link Metal.framework")
    _require(re.search(r"MoltenVK|libvulkan", dependencies, re.IGNORECASE) is None, paths["binary"], "MoltenVK/libvulkan dependency is forbidden")
    power = _run_command(("/usr/bin/pmset", "-g", "batt")).decode("utf-8", "strict")
    _require("AC Power" in power and config["comparability"]["power"]["source"] == "AC", "power", "AC power is required")
    power_configuration = _run_command(("/usr/bin/pmset", "-g", "custom")).decode("utf-8", "strict")
    _require(config["comparability"]["power"]["mode"] == "normal", "power", "epoch power mode must be normal")
    low_power = re.search(r"\blowpowermode\s+([0-9]+)\b", power_configuration)
    _require(low_power is None or low_power.group(1) == "0", "power", "Low Power Mode must be disabled when supported")
    return configuration_evidence


def _load_epoch(registry_root: pathlib.Path) -> Mapping[str, Any]:
    epoch_path = registry_root / "epoch.json"
    epoch = _exact(_load_json(epoch_path), frozenset({"schema", "protocol", "comparability"}), epoch_path, "epoch")
    schema = _exact(epoch["schema"], frozenset({"name", "version"}), epoch_path, "epoch.schema")
    _require(schema == {"name": EPOCH_SCHEMA_NAME, "version": EPOCH_SCHEMA_VERSION}, epoch_path, "unsupported epoch schema")
    _validate_protocol(epoch["protocol"], epoch_path)
    _validate_comparability(epoch["comparability"], epoch_path)
    return epoch


def _record_manifests(registry_root: pathlib.Path) -> Sequence[pathlib.Path]:
    return sorted((registry_root / "records").glob("*/manifest.json"))


def _record_has_reject_decision(registry_root: pathlib.Path, record: Any) -> bool:
    record_id = record.provenance["record_id"]
    matches = 0
    for path in sorted((registry_root / "decisions").glob("*.json")):
        decision = _exact(
            _load_json(path),
            frozenset({"schema", "decision_id", "decision", "protocol_id", "report", "report_sha256", "baseline", "candidate", "prior_pointer_sha256", "attestation", "created_utc"}),
            path,
            "decision",
        )
        schema = _exact(decision["schema"], frozenset({"name", "version"}), path, "decision schema")
        _require(schema == {"name": "lumen.persistent-performance-decision", "version": 1}, path, "invalid decision schema")
        candidate = decision.get("candidate")
        if decision.get("decision") != "reject" or type(candidate) is not dict or candidate.get("record_id") != record_id:
            continue
        _require(decision.get("attestation") == "CONFIRM_REJECT", path, "reject decision attestation mismatch")
        _require(
            candidate.get("generation") == record.lineage["generation"]
            and candidate.get("parent_record_id") == record.lineage["parent_record_id"]
            and candidate.get("manifest_sha256") == record.content_sha256,
            path,
            "reject decision does not bind the sealed candidate",
        )
        report_text = _string(decision.get("report"), path, "decision.report")
        reported_path = pathlib.Path(report_text)
        report_path = reported_path.resolve()
        report_digest = _sha(decision.get("report_sha256"), path, "decision.report_sha256")
        reports_root = (registry_root / "reports").resolve()
        _require(
            reported_path.is_absolute()
            and report_text == str(report_path)
            and report_path.parent == reports_root
            and report_path.name == f"{report_digest}.json"
            and report_path.is_file()
            and not report_path.is_symlink(),
            path,
            "reject decision report is not a canonical content-addressed internal archive",
        )
        _require(
            report_path.stat().st_mode
            & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)
            == 0,
            report_path,
            "reject decision report archive remains writable",
        )
        _require(_sha_file(report_path) == report_digest, path, "reject decision report is missing or changed")
        matches += 1
    _require(matches <= 1, registry_root, f"multiple reject decisions for record {record_id}")
    return matches == 1


def _require_immutable_registry_file(path: pathlib.Path, purpose: str) -> None:
    _require(
        path.is_file() and not path.is_symlink(),
        path,
        f"{purpose} is missing or is not a regular non-symlink file",
    )
    _require(
        path.stat().st_mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)
        == 0,
        path,
        f"{purpose} remains writable",
    )


def _validate_current_baseline_pointer(
    registry_root: pathlib.Path,
    config: Mapping[str, Any],
    records: Sequence[Any],
) -> Tuple[Mapping[str, Any], Any]:
    current_path = registry_root / "current-baseline.json"
    _require_immutable_registry_file(current_path, "baseline pointer")
    pointer = _exact(
        _load_json(current_path),
        frozenset(
            {
                "schema",
                "protocol_id",
                "generation",
                "record_id",
                "parent_record_id",
                "manifest",
                "manifest_sha256",
                "decision_id",
            }
        ),
        current_path,
        "baseline pointer",
    )
    pointer_schema = _exact(
        pointer["schema"],
        frozenset({"name", "version"}),
        current_path,
        "baseline pointer schema",
    )
    _require(
        pointer_schema
        == {
            "name": "lumen.persistent-performance-baseline-pointer",
            "version": 1,
        },
        current_path,
        "unsupported baseline pointer schema",
    )
    protocol_id = _string(
        pointer["protocol_id"], current_path, "pointer.protocol_id"
    )
    record_id = _string(
        pointer["record_id"], current_path, "pointer.record_id"
    )
    decision_id = _string(
        pointer["decision_id"], current_path, "pointer.decision_id"
    )
    for value, name in (
        (protocol_id, "pointer.protocol_id"),
        (record_id, "pointer.record_id"),
        (decision_id, "pointer.decision_id"),
    ):
        _require(
            SAFE_ID_RE.fullmatch(value) is not None,
            current_path,
            f"{name} is unsafe",
        )
    generation = _uint(
        pointer["generation"], current_path, "pointer.generation"
    )
    parent_record_id = pointer["parent_record_id"]
    if generation == 0:
        _require(
            parent_record_id is None,
            current_path,
            "generation-zero pointer must be parentless",
        )
    else:
        parent_record_id = _string(
            parent_record_id, current_path, "pointer.parent_record_id"
        )
        _require(
            SAFE_ID_RE.fullmatch(parent_record_id) is not None,
            current_path,
            "pointer.parent_record_id is unsafe",
        )
    _string(pointer["manifest"], current_path, "pointer.manifest")
    _sha(
        pointer["manifest_sha256"],
        current_path,
        "pointer.manifest_sha256",
    )

    parent_records = [
        record
        for record in records
        if record.provenance["record_id"] == record_id
    ]
    _require(
        len(parent_records) == 1,
        current_path,
        "baseline pointer record is absent or ambiguous",
    )
    parent_record = parent_records[0]
    manifest_text = f"records/{record_id}/manifest.json"
    manifest_path = registry_root.joinpath(
        *pathlib.PurePosixPath(manifest_text).parts
    )
    _require_immutable_registry_file(manifest_path, "baseline manifest")
    _require(
        parent_record.path == manifest_path.resolve(),
        manifest_path,
        "baseline manifest is outside its canonical registry path",
    )
    _require(
        parent_record.protocol == config["protocol"]
        and parent_record.comparability == config["comparability"],
        manifest_path,
        "baseline manifest differs from the start epoch",
    )
    manifest_sha256 = _sha_file(manifest_path)
    _require(
        manifest_sha256 == parent_record.content_sha256,
        manifest_path,
        "baseline manifest changed after validation",
    )
    descriptor = {
        "generation": parent_record.lineage["generation"],
        "record_id": parent_record.provenance["record_id"],
        "parent_record_id": parent_record.lineage["parent_record_id"],
        "manifest": manifest_text,
        "manifest_sha256": manifest_sha256,
    }
    expected_pointer = {
        "schema": {
            "name": "lumen.persistent-performance-baseline-pointer",
            "version": 1,
        },
        "protocol_id": config["protocol"]["protocol_id"],
        **descriptor,
        "decision_id": decision_id,
    }
    _require(
        pointer == expected_pointer,
        current_path,
        "baseline pointer differs from its complete canonical manifest descriptor",
    )

    generation_path = (
        registry_root / "generations" / f"g{generation:06d}.json"
    )
    _require_immutable_registry_file(
        generation_path, "baseline generation descriptor"
    )
    expected_generation = {
        "schema": {
            "name": "lumen.persistent-performance-generation",
            "version": 1,
        },
        "protocol_id": protocol_id,
        **descriptor,
        "decision_id": decision_id,
    }
    generation_value = _exact(
        _load_json(generation_path),
        frozenset(expected_generation),
        generation_path,
        "baseline generation descriptor",
    )
    _require(
        generation_value == expected_generation,
        generation_path,
        "baseline pointer differs from its immutable generation descriptor",
    )
    return pointer, parent_record


def _validate_registry_lineage(registry_root: pathlib.Path, config: Mapping[str, Any], report_module: Any) -> None:
    lineage = config["lineage"]
    records = [report_module.load_manifest(path) for path in _record_manifests(registry_root)]
    current_path = registry_root / "current-baseline.json"
    current_present = current_path.exists() or current_path.is_symlink()
    if lineage["original_role"] == "production":
        _require(not current_present and not records, registry_root, "production B is allowed only in an empty epoch")
        return
    if current_present:
        pointer, parent_record = _validate_current_baseline_pointer(
            registry_root, config, records
        )
        expected_parent = parent_record.provenance["record_id"]
        expected_generation = parent_record.lineage["generation"] + 1
        _require(lineage["parent_record_id"] == expected_parent and lineage["generation"] == expected_generation, registry_root, "candidate lineage does not extend current baseline")
        unresolved = [record for record in records if record.lineage["parent_record_id"] == expected_parent and record.lineage["generation"] == expected_generation]
        unresolved = [record for record in unresolved if not _record_has_reject_decision(registry_root, record)]
        _require(not unresolved, registry_root, "an unresolved candidate already extends the current baseline")
        return
    productions = [record for record in records if record.lineage["generation"] == 0 and record.lineage["original_role"] == "production"]
    candidates = [record for record in records if record.lineage["original_role"] == "candidate"]
    _require(len(productions) == 1 and not candidates, registry_root, "initial C requires exactly one sealed B and no prior C")
    production = productions[0]
    _require(lineage["generation"] == 1 and lineage["parent_record_id"] == production.provenance["record_id"], registry_root, "initial C lineage mismatch")
    _require(_sha_file(pathlib.Path(config["paths"]["binary"]).resolve()) == production.provenance["binary_sha256"], registry_root, "initial B/C must use the same Mach-O")


def _copy_tree(source: pathlib.Path, target: pathlib.Path) -> None:
    _require(not target.exists(), target, "refusing to overwrite work save")
    try:
        shutil.copytree(source, target, symlinks=False)
        # The authoritative template may be sealed 0444/0555.  Only its clone
        # is made owner-writable so guest autosave can update MainData while
        # Special and the template remain immutable evidence.
        for child in sorted(target.rglob("*")):
            current = stat.S_IMODE(child.stat().st_mode)
            if child.is_dir():
                os.chmod(child, current | stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
            elif child.is_file():
                os.chmod(child, current | stat.S_IRUSR | stat.S_IWUSR)
        current = stat.S_IMODE(target.stat().st_mode)
        os.chmod(target, current | stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
    except OSError as error:
        raise HarnessError(f"cannot clone save template: {error}") from error


def _checkpoint_from_line(line: str, source: object) -> Mapping[str, Any]:
    match = CHECKPOINT_RE.fullmatch(line)
    _require(match is not None, source, "malformed checkpoint line")
    assert match is not None
    result = {
        "present": int(match.group("present")),
        "target": int(match.group("target")),
        "cumulative_ms": decimal.Decimal(match.group("cumulative")),
        "cumulative_intervals": int(match.group("cumulative_intervals")),
        "partial_ms": decimal.Decimal(match.group("partial")),
        "partial_intervals": int(match.group("partial_intervals")),
        "terminal": int(match.group("terminal")),
    }
    _require(result["present"] == CHECKPOINT_PRESENT and result["target"] == CHECKPOINT_PRESENT, source, "checkpoint must target p2300")
    _require(result["cumulative_intervals"] == 2299 and result["partial_intervals"] == 20 and result["terminal"] == 0, source, "checkpoint geometry/terminal mismatch")
    _require(result["cumulative_ms"] > 0 and result["partial_ms"] > 0 and result["partial_ms"] <= result["cumulative_ms"], source, "invalid checkpoint durations")
    return result


def _checkpoint_json_value(checkpoint: Mapping[str, Any]) -> Mapping[str, Any]:
    """Return the numeric JSON representation used by the version-1 schema."""
    return {
        key: float(value) if key in ("cumulative_ms", "partial_ms") else value
        for key, value in checkpoint.items()
    }


def _checkpoint_raw_line(stderr_path: pathlib.Path) -> Tuple[str, bytes]:
    try:
        payload = stderr_path.read_bytes()
    except OSError as error:
        raise HarnessError(f"{stderr_path}: cannot read checkpoint log: {error}") from error
    raw_lines = [
        line
        for line in payload.splitlines(keepends=True)
        if line.startswith(b"[FRAME_CHECKPOINT]")
    ]
    _require(len(raw_lines) == 1, stderr_path, "checkpoint is not exact-once")
    raw_line = raw_lines[0]
    _require(raw_line.endswith(b"\n"), stderr_path, "checkpoint line is not newline-terminated")
    try:
        line = raw_line[:-1].decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise HarnessError(f"{stderr_path}: checkpoint line is not strict UTF-8: {error}") from error
    _checkpoint_from_line(line, stderr_path)
    return line, raw_line


def _validate_checkpoint_observation(
    value: Any,
    checkpoint: Mapping[str, Any],
    raw_line: bytes,
    source: object,
) -> Mapping[str, Any]:
    observation = _exact(
        value,
        CHECKPOINT_OBSERVATION_KEYS,
        source,
        "checkpoint observation",
    )
    _require(
        _string(observation["schema"], source, "checkpoint observation schema")
        == CHECKPOINT_OBSERVATION_SCHEMA,
        source,
        "checkpoint observation schema mismatch",
    )
    observed_checkpoint = _exact(
        observation["checkpoint"],
        CHECKPOINT_VALUE_KEYS,
        source,
        "checkpoint observation value",
    )
    for name in (
        "present",
        "target",
        "cumulative_intervals",
        "partial_intervals",
        "terminal",
    ):
        _require(
            _uint(observed_checkpoint[name], source, f"checkpoint observation {name}")
            == checkpoint[name],
            source,
            f"checkpoint observation/log {name} mismatch",
        )
    for name in ("cumulative_ms", "partial_ms"):
        observed_duration = observed_checkpoint[name]
        _require(
            type(observed_duration) in (int, decimal.Decimal),
            source,
            f"checkpoint observation {name} must be a JSON number",
        )
        _require(
            _decimal(
                observed_duration,
                source,
                f"checkpoint observation {name}",
            )
            == _decimal(checkpoint[name], source, f"checkpoint log {name}"),
            source,
            f"checkpoint observation/log {name} mismatch",
        )
    expected_line_sha256 = hashlib.sha256(raw_line).hexdigest()
    _require(
        _sha(observation["line_sha256"], source, "checkpoint observation line SHA-256")
        == expected_line_sha256,
        source,
        "checkpoint observation/raw line SHA-256 mismatch",
    )
    return observation


def _exec_child(receipt_fd: int, binary: str, arguments: Sequence[str]) -> int:
    try:
        launch_ns = _uptime_raw_ns()
        payload = f"{launch_ns}\n".encode("ascii")
        written = os.write(receipt_fd, payload)
        if written != len(payload):
            return 125
        os.close(receipt_fd)
        os.execve(binary, [binary, *arguments], dict(os.environ))
    except BaseException as error:
        try:
            os.write(2, f"persistent-performance exec failure: {error}\n".encode("utf-8", "replace"))
        except OSError:
            pass
        return 126
    return 126


def _read_pre_exec_receipt(descriptor: int, timeout_seconds: float = 10.0) -> bytes:
    os.set_blocking(descriptor, False)
    selector = selectors.DefaultSelector()
    selector.register(descriptor, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout_seconds
    payload = b""
    try:
        while time.monotonic() < deadline:
            remaining = max(0.0, deadline - time.monotonic())
            events = selector.select(timeout=remaining)
            if not events:
                break
            chunk = os.read(descriptor, 64 - len(payload))
            if not chunk:
                break
            payload += chunk
            _require(len(payload) < 64, "pre-exec receipt", "receipt exceeds 63 bytes")
            if b"\n" in payload:
                return payload
    finally:
        selector.close()
    raise HarnessError("pre-exec receipt timed out or closed before one complete line")


def _abort_launch(process: subprocess.Popen[bytes], run_root: pathlib.Path, reason: BaseException) -> None:
    killed = False
    cleanup_error: Optional[BaseException] = None
    stdout = b""
    stderr = b""
    try:
        if process.poll() is None:
            process.terminate()
        try:
            stdout, stderr = process.communicate(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            killed = True
            stdout, stderr = process.communicate(timeout=5.0)
    except BaseException as error:
        cleanup_error = error
        try:
            process.kill()
            killed = True
            stdout, stderr = process.communicate(timeout=5.0)
        except BaseException as final_error:
            cleanup_error = final_error
            stdout = b""
            stderr = f"launch cleanup failed: {final_error}\n".encode(
                "utf-8", "replace"
            )
    if process.poll() is None:
        try:
            process.kill()
            process.wait(timeout=5.0)
            killed = True
        except BaseException as final_error:
            cleanup_error = final_error
            stderr += f"launch cleanup failed: {cleanup_error}\n".encode(
                "utf-8", "replace"
            )
    return_code = process.returncode if type(process.returncode) is int else 125
    for path, payload in ((run_root / "stdout.log", stdout), (run_root / "stderr.log", stderr)):
        if not path.exists():
            _atomic_bytes(path, payload, mode=0o444)
    if not (run_root / ".exit_status").exists():
        _atomic_bytes(run_root / ".exit_status", f"{return_code}\n".encode("ascii"))
    if not (run_root / "launch-failure.json").exists():
        _atomic_json(
            run_root / "launch-failure.json",
            {
                "schema": "lumen.persistent-performance-launch-failure/1",
                "error": f"{type(reason).__name__}: {reason}",
                "return_code": return_code,
                "forced_kill": killed,
                "cleanup_error": None
                if cleanup_error is None
                else f"{type(cleanup_error).__name__}: {cleanup_error}",
                "recorded_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            },
        )


def _supervise_process(
    process: subprocess.Popen[bytes], run_root: pathlib.Path, launch_ns: int
) -> Tuple[int, resource.struct_rusage]:
    try:
        return _drain_process(process, run_root, launch_ns)
    except BaseException as error:
        _abort_launch(process, run_root, error)
        raise


def _drain_process(process: subprocess.Popen[bytes], run_root: pathlib.Path, launch_ns: int) -> Tuple[int, resource.struct_rusage]:
    selector = selectors.DefaultSelector()
    streams = {process.stdout: run_root / "stdout.log", process.stderr: run_root / "stderr.log"}
    descriptors: Dict[Any, int] = {}
    buffers: Dict[Any, bytes] = {}
    for stream, path in streams.items():
        assert stream is not None
        os.set_blocking(stream.fileno(), False)
        selector.register(stream, selectors.EVENT_READ)
        descriptors[stream] = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        buffers[stream] = b""
    checkpoint_path = run_root / "checkpoint-observation.json"
    checkpoint_seen = False
    try:
        while selector.get_map():
            for key, _ in selector.select(timeout=1.0):
                stream = key.fileobj
                try:
                    data = os.read(stream.fileno(), 65536)
                except BlockingIOError:
                    continue
                if not data:
                    selector.unregister(stream)
                    trailing = buffers[stream]
                    if trailing:
                        _write_fd_all(descriptors[stream], trailing, streams[stream])
                    os.close(descriptors[stream])
                    descriptors[stream] = -1
                    continue
                buffers[stream] += data
                while b"\n" in buffers[stream]:
                    raw_line, buffers[stream] = buffers[stream].split(b"\n", 1)
                    complete = raw_line + b"\n"
                    is_checkpoint = (
                        stream is process.stderr
                        and raw_line.startswith(b"[FRAME_CHECKPOINT]")
                    )
                    # Freeze the observation clock at complete-line detection,
                    # before log I/O, UTF-8 decoding, regex parsing or receipt
                    # publication can add host-side jitter.
                    observed_ns = _uptime_raw_ns() if is_checkpoint else None
                    _write_fd_all(descriptors[stream], complete, streams[stream])
                    if is_checkpoint:
                        line = raw_line.decode("utf-8", "strict")
                        checkpoint = _checkpoint_from_line(line, run_root / "stderr.log")
                        if not checkpoint_seen:
                            assert observed_ns is not None
                            _atomic_json(
                                checkpoint_path,
                                {
                                    "schema": CHECKPOINT_OBSERVATION_SCHEMA,
                                    "launch_uptime_raw_ns": launch_ns,
                                    "checkpoint_observed_uptime_raw_ns": observed_ns,
                                    "wall_ns": observed_ns - launch_ns,
                                    "checkpoint": _checkpoint_json_value(checkpoint),
                                    "line_sha256": hashlib.sha256(complete).hexdigest(),
                                },
                            )
                            checkpoint_seen = True
        pid, wait_status, usage = os.wait4(process.pid, 0)
        _require(pid == process.pid, run_root, "wait4 returned a different process")
        return_code = os.waitstatus_to_exitcode(wait_status)
        process.returncode = return_code
        return return_code, usage
    finally:
        selector.close()
        for descriptor in descriptors.values():
            if descriptor < 0:
                continue
            try:
                os.close(descriptor)
            except OSError:
                pass


def preflight_run(
    registry_root: pathlib.Path,
    config_path: pathlib.Path,
    *,
    test_mode: bool = False,
    _configuration_projection_policy: Optional[Mapping[str, Any]] = None,
) -> Mapping[str, Any]:
    """Validate that one run may start without claiming or launching it."""

    registry_root = registry_root.expanduser().absolute()
    config_path = config_path.expanduser().absolute()
    _require(
        registry_root.is_dir() and not registry_root.is_symlink(),
        registry_root,
        "registry must be a regular non-symlink directory",
    )
    pending_root = registry_root / "pending"
    records_root = registry_root / "records"
    for path, purpose in (
        (pending_root, "pending registry"),
        (records_root, "records registry"),
    ):
        _require(
            path.is_dir() and not path.is_symlink(),
            path,
            f"{purpose} must be a regular non-symlink directory",
        )

    registry_before = _tree_digest(registry_root)
    config_before = _capture_stable_regular_bytes(
        config_path, purpose="preflight start config"
    )
    config, config_snapshot = _capture_start_config(config_path)
    _require(
        _capture_stable_regular_bytes(
            config_path, purpose="preflight start config"
        )
        == config_before,
        config_path,
        "start config changed during preflight capture",
    )
    epoch = _load_epoch(registry_root)
    _require(
        epoch["protocol"] == config["protocol"]
        and epoch["comparability"] == config["comparability"],
        config_path,
        "start config does not match immutable epoch",
    )
    paths = _resolved_paths(config)
    _platform_preflight(
        config,
        paths,
        test_mode=test_mode,
        configuration_projection_policy=_configuration_projection_policy,
    )
    report_module = _load_report_module(paths["report_tool"])
    _validate_registry_lineage(registry_root, config, report_module)

    record_id = config["provenance"]["record_id"]
    pending_record = pending_root / record_id
    sealed_record = records_root / record_id
    _require(
        not any(pending_root.iterdir()),
        registry_root,
        "another pending run exists",
    )
    residual_processes = _process_names(
        paths["binary"].name, test_mode=test_mode
    )
    _require(
        not residual_processes,
        registry_root,
        "residual emulator process exists",
    )
    _require(
        not pending_record.exists()
        and not pending_record.is_symlink()
        and not sealed_record.exists()
        and not sealed_record.is_symlink(),
        pending_record,
        "record_id already exists",
    )

    current_pointer = registry_root / "current-baseline.json"
    current_pointer_sha256 = (
        _sha_file(current_pointer) if current_pointer.exists() else None
    )
    config_after = _capture_stable_regular_bytes(
        config_path, purpose="preflight start config"
    )
    _require(
        config_after == config_before,
        config_path,
        "start config changed during preflight",
    )
    _require(
        not any(pending_root.iterdir())
        and not pending_record.exists()
        and not pending_record.is_symlink()
        and not sealed_record.exists()
        and not sealed_record.is_symlink(),
        registry_root,
        "registry claim state changed during preflight",
    )
    _require(
        not _process_names(paths["binary"].name, test_mode=test_mode),
        registry_root,
        "emulator process appeared during preflight",
    )
    registry_after = _tree_digest(registry_root)
    _require(
        registry_after == registry_before,
        registry_root,
        "registry changed during read-only preflight",
    )

    return {
        "schema": "lumen.persistent-performance-preflight/1",
        "status": "clean",
        "registry_root": str(registry_root),
        "config_path": str(config_path),
        "record_id": record_id,
        "epoch_sha256": _canonical_sha(epoch),
        "comparability_sha256": _canonical_sha(config["comparability"]),
        "lineage_sha256": _canonical_sha(config["lineage"]),
        "start_config_sha256": _sha_bytes(config_snapshot),
        "configuration_input_sha256": _sha_bytes(config_before),
        "binary_sha256": _sha_file(paths["binary"]),
        "current_baseline_sha256": current_pointer_sha256,
        "registry_sha256": registry_before,
        "pending_count": 0,
        "residual_process_count": 0,
        "record_absent": True,
    }


def start_run(
    registry_root: pathlib.Path,
    config_path: pathlib.Path,
    *,
    test_mode: bool = False,
    _after_preflight_hook: Optional[Callable[[], None]] = None,
    _configuration_projection_policy: Optional[Mapping[str, Any]] = None,
) -> pathlib.Path:
    registry_root = registry_root.resolve()
    config_path = config_path.resolve()
    config, config_snapshot = _capture_start_config(config_path)
    epoch = _load_epoch(registry_root)
    _require(epoch["protocol"] == config["protocol"] and epoch["comparability"] == config["comparability"], config_path, "start config does not match immutable epoch")
    paths = _resolved_paths(config)
    configuration_evidence = _platform_preflight(
        config,
        paths,
        test_mode=test_mode,
        configuration_projection_policy=_configuration_projection_policy,
    )
    if _after_preflight_hook is not None:
        _after_preflight_hook()
    report_module = _load_report_module(paths["report_tool"])
    record_id = config["provenance"]["record_id"]
    run_root = registry_root / "pending" / record_id
    with _DirectoryLock(registry_root / ".registry.lock"):
        _validate_registry_lineage(registry_root, config, report_module)
        _require(not any((registry_root / "pending").iterdir()), registry_root, "another pending run exists")
        _require(not _process_names(paths["binary"].name, test_mode=test_mode), registry_root, "residual emulator process exists")
        _require(not run_root.exists() and not (registry_root / "records" / record_id).exists(), run_root, "record_id already exists")
        # The pending directory itself is the durable single-run claim.  The
        # registry lock closes the check/create race, then later starts see the
        # claim even after this short critical section ends.
        run_root.mkdir(mode=0o700)
    (run_root / "anchors").mkdir(mode=0o700)
    (run_root / "evidence").mkdir(mode=0o700)
    work_root = run_root / "work"
    work_root.mkdir(mode=0o700)
    _copy_tree(paths["save_template"], work_root / "_SaveData")
    _atomic_bytes(run_root / "start-config.json", config_snapshot)
    projection_freeze: Mapping[str, str] = {}
    if configuration_evidence is not None:
        configuration_raw, configuration_receipt = configuration_evidence
        projection_freeze = _publish_configuration_projection_artifacts(
            run_root, configuration_raw, configuration_receipt
        )

    special_source = paths["save_template"] / pathlib.Path(*paths["save_special_relative"].parts)
    special_work = work_root / "_SaveData" / pathlib.Path(*paths["save_special_relative"].parts)
    _require(special_source.is_file() and special_work.is_file(), special_source, "Special save is missing")
    template_before = _tree_digest(paths["save_template"])
    special_before = _sha_file(special_work)
    revision, diff_sha = _git_snapshot(paths["repo_root"])
    harness_path = pathlib.Path(__file__).resolve()
    freeze = {
        "schema": "lumen.persistent-performance-freeze/1",
        "registry_root": str(registry_root),
        "record_id": record_id,
        "epoch_sha256": _canonical_sha(epoch),
        "lineage_sha256": _canonical_sha(config["lineage"]),
        "source_revision": revision,
        "diff_sha256": diff_sha,
        "binary_sha256": _sha_file(paths["binary"]),
        "harness_sha256": _sha_file(harness_path),
        "announcement_sidecar_sha256": _sha_file(harness_path),
        "report_tool_sha256": _sha_file(paths["report_tool"]),
        "run_receipt_expected_sha256": hashlib.sha256(
            (config["provenance"]["run_receipt"] + "\n").encode("utf-8")
        ).hexdigest(),
        "start_config_sha256": hashlib.sha256(config_snapshot).hexdigest(),
        "save_template_before_sha256": template_before,
        "save_special_before_sha256": special_before,
        "binary_path": str(paths["binary"]),
        "report_tool_path": str(paths["report_tool"]),
        "save_template_path": str(paths["save_template"]),
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "test_mode": test_mode,
    }
    freeze.update(projection_freeze)
    _atomic_json(run_root / "freeze.json", freeze)
    _atomic_bytes(run_root / "save-special-before.sha256", (special_before + "\n").encode("ascii"))
    _atomic_bytes(run_root / "save-template-before.sha256", (template_before + "\n").encode("ascii"))

    environment = {**config["launch"]["common_environment"], **config["launch"]["experiment_environment"]}
    receipt_read, receipt_write = os.pipe()
    os.set_inheritable(receipt_write, True)
    command = [sys.executable, str(harness_path), "_exec-child", str(receipt_write), str(paths["binary"]), *config["launch"]["arguments"]]
    try:
        process = subprocess.Popen(
            command,
            cwd=work_root,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            pass_fds=(receipt_write,),
        )
    except OSError as error:
        os.close(receipt_read)
        os.close(receipt_write)
        raise HarnessError(f"cannot launch exec wrapper: {error}") from error
    os.close(receipt_write)
    try:
        with os.fdopen(receipt_read, "rb", closefd=True) as receipt_stream:
            receipt_payload = _read_pre_exec_receipt(receipt_stream.fileno())
        _require(
            re.fullmatch(rb"[1-9][0-9]*\n", receipt_payload) is not None,
            run_root,
            "missing immediate pre-exec clock receipt",
        )
    except BaseException as error:
        _abort_launch(process, run_root, error)
        raise
    launch_ns = int(receipt_payload)
    _atomic_bytes(run_root / "runner.pid", f"{process.pid}\n".encode("ascii"))
    _atomic_bytes(run_root / "launch-uptime-raw-ns.txt", f"{launch_ns}\n".encode("ascii"))
    return_code, usage = _supervise_process(process, run_root, launch_ns)
    exit_ns = _uptime_raw_ns()
    _atomic_bytes(run_root / ".exit_status", f"{return_code}\n".encode("ascii"))
    _atomic_bytes(run_root / "exit-uptime-raw-ns.txt", f"{exit_ns}\n".encode("ascii"))
    _atomic_json(
        run_root / "process-time.json",
        {
            "real": round((exit_ns - launch_ns) / 1_000_000_000, 9),
            "user": round(usage.ru_utime, 9),
            "sys": round(usage.ru_stime, 9),
        },
    )
    return run_root


def _read_complete_log(path: pathlib.Path) -> str:
    _require(path.is_file(), path, "missing log")
    data = path.read_bytes()
    complete = data[: data.rfind(b"\n") + 1] if b"\n" in data else b""
    try:
        return complete.decode("utf-8", "strict")
    except UnicodeError as error:
        raise HarnessError(f"{path}: log is not strict UTF-8: {error}") from error


def _parse_frames(path: pathlib.Path) -> Mapping[int, decimal.Decimal]:
    text = _read_complete_log(path)
    frames: Dict[int, decimal.Decimal] = {}
    ordered = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not line.startswith("[FRAME_PIPELINE] present="):
            continue
        match = PIPELINE_RE.fullmatch(line)
        _require(match is not None, path, f"malformed frame pipeline line {line_number}")
        assert match is not None
        present = int(match.group("present"))
        _require(present not in frames, path, f"duplicate p{present}")
        interval = decimal.Decimal(match.group("interval"))
        _require((present == 1 and interval == ZERO) or (present != 1 and interval > ZERO), path, f"invalid interval at p{present}")
        frames[present] = interval
        ordered.append(present)
    _require(bool(ordered) and ordered[0] == 1, path, "frame trace must start at p1")
    for index, present in enumerate(ordered[1:], start=1):
        _require(present == index * WINDOW_SIZE, path, "missing or non-monotonic p120 endpoint")
    return frames


def _last_stable_endpoint(stderr_path: pathlib.Path) -> Tuple[int, int, int, int]:
    for _ in range(8):
        before_stat = stderr_path.stat()
        before_frames = _parse_frames(stderr_path)
        before = max(before_frames)
        receipt = _uptime_raw_ns()
        after_frames = _parse_frames(stderr_path)
        after_stat = stderr_path.stat()
        after = max(after_frames)
        if before == after and before_stat.st_dev == after_stat.st_dev and before_stat.st_ino == after_stat.st_ino and after_stat.st_size >= before_stat.st_size:
            _require(before > CHECKPOINT_PRESENT and before % WINDOW_SIZE == 0, stderr_path, "announcement L must be a post-p2300 p120 endpoint")
            return before, receipt, after, after_stat.st_size
    raise HarnessError(f"{stderr_path}: p120 endpoint raced across announcement clock")


def _pid_alive(run_root: pathlib.Path) -> bool:
    try:
        pid = int((run_root / "runner.pid").read_text(encoding="ascii").strip())
        os.kill(pid, 0)
        return True
    except (OSError, ValueError):
        return False


def _validate_pngs(paths: Sequence[pathlib.Path], count: int, source: object) -> Sequence[Tuple[pathlib.Path, str]]:
    _require(len(paths) >= count, source, f"at least {count} PNG evidence files are required")
    result = []
    for path in paths:
        resolved = path.resolve()
        _require(resolved.is_file() and not resolved.is_symlink(), resolved, "missing PNG evidence")
        with resolved.open("rb") as evidence:
            _require(evidence.read(8) == b"\x89PNG\r\n\x1a\n", resolved, "evidence is not PNG")
        result.append((resolved, _sha_file(resolved)))
    if count > 1:
        _require(len({digest for _, digest in result}) == len(result), source, "PNG evidence must be distinct")
    return result


def _publish_evidence(run_root: pathlib.Path, scene: str, stage: str, paths: Sequence[pathlib.Path]) -> Sequence[Mapping[str, Any]]:
    result = []
    for index, (path, digest) in enumerate(_validate_pngs(paths, 2 if scene == "in_game" and stage == "anchor" else 1, scene), start=1):
        target = run_root / "evidence" / f"{scene}-{stage}-{index}.png"
        _require(not target.exists(), target, "refusing to overwrite evidence")
        temporary = target.with_name(f".{target.name}.tmp.{os.getpid()}")
        shutil.copyfile(path, temporary)
        _require(_sha_file(temporary) == digest, target, "evidence changed while copying")
        os.chmod(temporary, 0o444)
        os.replace(temporary, target)
        result.append({"file": str(target.relative_to(run_root)), "sha256": digest})
    return result


def _validate_archived_evidence(
    run_root: pathlib.Path,
    scene: str,
    stage: str,
    value: Any,
) -> Sequence[Mapping[str, Any]]:
    _require(type(value) is list, scene, f"{stage} evidence must be a list")
    minimum = 2 if scene == "in_game" and stage == "anchor" else 1
    _require(len(value) >= minimum, scene, f"{stage} requires at least {minimum} evidence PNG(s)")
    result = []
    digests = []
    evidence_root = (run_root / "evidence").resolve()
    for index, item in enumerate(value, start=1):
        receipt = _exact(
            item,
            frozenset({"file", "sha256"}),
            scene,
            f"{stage} evidence {index}",
        )
        relative_text = _string(receipt["file"], scene, f"{stage} evidence file")
        relative = pathlib.PurePosixPath(relative_text)
        _require(
            not relative.is_absolute()
            and ".." not in relative.parts
            and relative.parts
            and relative.parts[0] == "evidence",
            scene,
            f"unsafe {stage} evidence path",
        )
        path = (run_root / pathlib.Path(*relative.parts)).resolve()
        try:
            path.relative_to(evidence_root)
        except ValueError as error:
            raise HarnessError(f"{path}: evidence escapes the run archive") from error
        _require(path.is_file() and not path.is_symlink(), path, "archived evidence is missing or is a symlink")
        with path.open("rb") as stream:
            _require(stream.read(8) == b"\x89PNG\r\n\x1a\n", path, "archived evidence is not PNG")
        digest = _sha(receipt["sha256"], scene, f"{stage} evidence sha256")
        _require(_sha_file(path) == digest, path, "archived evidence hash mismatch")
        result.append(receipt)
        digests.append(digest)
    if minimum > 1:
        _require(len(set(digests)) == len(digests), scene, "in-game anchor evidence must be distinct")
    return result


def _validate_anchor_value(
    run_root: pathlib.Path,
    scene: str,
    value: Any,
    *,
    final: bool,
) -> Mapping[str, Any]:
    _require(scene in ("fort_mode", "in_game"), scene, "unsupported scene")
    anchor = _exact(
        value,
        ANCHOR_FINAL_KEYS if final else ANCHOR_PENDING_KEYS,
        scene,
        "anchor receipt",
    )
    _require(anchor["schema"] == ANCHOR_SCHEMA and anchor["scene"] == scene, scene, "anchor schema/scene mismatch")
    receipt_ns = _uint(anchor["receipt_uptime_raw_ns"], scene, "anchor receipt clock")
    before = _uint(anchor["endpoint_before"], scene, "endpoint_before")
    after = _uint(anchor["endpoint_after"], scene, "endpoint_after")
    anchor_endpoint = _uint(anchor["anchor_endpoint"], scene, "anchor_endpoint")
    measured_endpoint = _uint(anchor["measured_endpoint"], scene, "measured_endpoint")
    stderr_size = _uint(anchor["stderr_size_after"], scene, "stderr_size_after")
    _require(receipt_ns > 0 and stderr_size > 0, scene, "anchor clock/log size must be positive")
    _require(before == after and before > CHECKPOINT_PRESENT and before % WINDOW_SIZE == 0, scene, "anchor endpoint is not a stable post-p2300 p120")
    _require(anchor_endpoint == before + WINDOW_SIZE and measured_endpoint == anchor_endpoint + WINDOW_SIZE, scene, "anchor geometry mismatch")
    required_anchor = FORT_ANCHOR_ATTESTATIONS if scene == "fort_mode" else GAME_ANCHOR_ATTESTATIONS
    _require(anchor["attestations"] == sorted(required_anchor), scene, "anchor attestations mismatch")
    _validate_archived_evidence(run_root, scene, "anchor", anchor["anchor_evidence"])
    if scene == "fort_mode":
        _require(anchor["timer_before"] is None and anchor["timer_after"] is None, scene, "Fort receipt must not contain timer values")
    else:
        timer_before = _uint(anchor["timer_before"], scene, "timer_before")
        timer_after = _uint(anchor["timer_after"], scene, "timer_after")
        _require(timer_after > timer_before, scene, "in-game timer must advance")
    if not final:
        return anchor
    completed_ns = _uint(anchor["completed_uptime_raw_ns"], scene, "completion clock")
    _require(completed_ns > receipt_ns, scene, "completion clock must follow announcement")
    interval = _decimal(anchor["avg_interval_ms"], scene, "avg_interval_ms")
    _require(interval > ZERO, scene, "measured interval must be positive")
    required_completion = FORT_COMPLETE_ATTESTATIONS if scene == "fort_mode" else GAME_COMPLETE_ATTESTATIONS
    _require(anchor["completion_attestations"] == sorted(required_completion), scene, "completion attestations mismatch")
    _validate_archived_evidence(run_root, scene, "complete", anchor["completion_evidence"])
    return anchor


class _DirectoryLock:
    def __init__(self, path: pathlib.Path):
        self.path = path

    def __enter__(self) -> "_DirectoryLock":
        try:
            self.path.mkdir()
        except FileExistsError as error:
            raise HarnessError(f"{self.path}: operation already active or stale lock present") from error
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        try:
            self.path.rmdir()
        except OSError:
            pass


def anchor_scene(
    run_root: pathlib.Path,
    scene: str,
    attestations: Sequence[str],
    evidence_paths: Sequence[pathlib.Path],
    *,
    timer_before: Optional[int] = None,
    timer_after: Optional[int] = None,
) -> pathlib.Path:
    run_root = run_root.resolve()
    _require(scene in ("fort_mode", "in_game"), scene, "unsupported scene")
    required = FORT_ANCHOR_ATTESTATIONS if scene == "fort_mode" else GAME_ANCHOR_ATTESTATIONS
    _require(frozenset(attestations) == required and len(attestations) == len(required), scene, "anchor attestations mismatch")
    _require((run_root / "checkpoint-observation.json").is_file(), run_root, "p2300 has not been observed")
    _require(_pid_alive(run_root), run_root, "emulator is not alive")
    if scene == "in_game":
        _require((run_root / "anchors" / "fort_mode.json").is_file(), run_root, "Fort window must complete first")
        _require(type(timer_before) is int and type(timer_after) is int and timer_after > timer_before >= 0, scene, "strictly advancing numeric timer evidence is required")
    else:
        _require(timer_before is None and timer_after is None, scene, "Fort anchor does not accept timer values")
    pending = run_root / "anchors" / f"{scene}.pending.json"
    final = run_root / "anchors" / f"{scene}.json"
    _require(not pending.exists() and not final.exists(), scene, "scene anchor already exists")
    with _DirectoryLock(run_root / f".{scene}-anchor.lock"):
        before, receipt, after, stderr_size = _last_stable_endpoint(run_root / "stderr.log")
        if scene == "in_game":
            fort = _load_json(run_root / "anchors" / "fort_mode.json")
            _require(before > fort["measured_endpoint"], scene, "in-game L must follow measured Fort window")
        anchor_endpoint = before + WINDOW_SIZE
        measured_endpoint = anchor_endpoint + WINDOW_SIZE
        evidence = _publish_evidence(run_root, scene, "anchor", evidence_paths)
        value = {
            "schema": ANCHOR_SCHEMA,
            "scene": scene,
            "receipt_uptime_raw_ns": receipt,
            "endpoint_before": before,
            "endpoint_after": after,
            "anchor_endpoint": anchor_endpoint,
            "measured_endpoint": measured_endpoint,
            "stderr_size_after": stderr_size,
            "attestations": sorted(attestations),
            "anchor_evidence": evidence,
            "timer_before": timer_before,
            "timer_after": timer_after,
        }
        _validate_anchor_value(run_root, scene, value, final=False)
        _atomic_json(pending, value)
    return pending


def complete_scene(
    run_root: pathlib.Path,
    scene: str,
    attestations: Sequence[str],
    evidence_paths: Sequence[pathlib.Path],
) -> pathlib.Path:
    run_root = run_root.resolve()
    _require(scene in ("fort_mode", "in_game"), scene, "unsupported scene")
    required = FORT_COMPLETE_ATTESTATIONS if scene == "fort_mode" else GAME_COMPLETE_ATTESTATIONS
    _require(frozenset(attestations) == required and len(attestations) == len(required), scene, "completion attestations mismatch")
    pending = run_root / "anchors" / f"{scene}.pending.json"
    final = run_root / "anchors" / f"{scene}.json"
    _require(pending.is_file() and not final.exists(), scene, "no unique pending anchor")
    _require(_pid_alive(run_root), run_root, "emulator is not alive")
    with _DirectoryLock(run_root / f".{scene}-complete.lock"):
        value = dict(
            _validate_anchor_value(
                run_root, scene, _load_json(pending), final=False
            )
        )
        frames = _parse_frames(run_root / "stderr.log")
        anchor_endpoint = value["anchor_endpoint"]
        measured_endpoint = value["measured_endpoint"]
        _require(anchor_endpoint in frames and measured_endpoint in frames, scene, "discard/measurement endpoints are not both complete")
        interval = frames[measured_endpoint]
        _require(interval > ZERO, scene, "measured interval must be positive")
        completion_evidence = _publish_evidence(run_root, scene, "complete", evidence_paths)
        value.update(
            {
                "avg_interval_ms": float(interval),
                "completed_uptime_raw_ns": _uptime_raw_ns(),
                "completion_attestations": sorted(attestations),
                "completion_evidence": completion_evidence,
            }
        )
        _require(value["completed_uptime_raw_ns"] > value["receipt_uptime_raw_ns"], scene, "completion clock must follow announcement")
        _validate_anchor_value(run_root, scene, value, final=True)
        _atomic_json(final, value)
        pending.unlink()
    return final


def _count_exact_line(texts: Iterable[str], expected: str) -> int:
    return sum(1 for text in texts for line in text.splitlines() if line == expected)


def _validate_candidate_marker_lines(
    logs: Sequence[str], config: Mapping[str, Any], source: object
) -> Tuple[int, bool]:
    marker_config = config["candidate_marker"]
    marker_lines = []
    if marker_config["prefix"] is not None:
        marker_lines = [
            line
            for text in logs
            for line in text.splitlines()
            if line.startswith(marker_config["prefix"])
        ]
    marker_count = len(marker_lines)
    coverage_positive = False
    if marker_config["required"]:
        _require(marker_count == 1, source, "candidate marker is not exact-once")
        _require(
            all(
                token in marker_lines[0]
                for token in marker_config["required_tokens"]
            ),
            source,
            "candidate marker is missing required status/contract tokens",
        )
        marker_contract = CANDIDATE_MARKER_ALLOWLIST[
            config["provenance"]["candidate_selector"]
        ]
        if "line_pattern" in marker_contract:
            _require(
                re.fullmatch(marker_contract["line_pattern"], marker_lines[0])
                is not None,
                source,
                "candidate marker differs from its exact structured line contract",
            )
        elif marker_contract.get("exact_line") is True:
            expected_marker_line = " ".join(
                [marker_config["prefix"], *marker_config["required_tokens"]]
            )
            _require(
                marker_lines[0] == expected_marker_line,
                source,
                "candidate marker differs from its exact preregistered line",
            )
        coverage_positive = (
            not marker_config["coverage_required"]
            or marker_config["coverage_token"] in marker_lines[0]
        )
        _require(
            coverage_positive, source, "required candidate coverage is absent"
        )
    else:
        _require(
            marker_count == 0,
            source,
            "candidate marker appeared in production arm",
        )
    return marker_count, coverage_positive


def _validate_companion_candidate_marker_lines(
    logs: Sequence[str], config: Mapping[str, Any], source: object
) -> int:
    if _noda_projection_scope_for(config, source) is not None:
        companion_count = 0
        all_lines = [line for text in logs for line in text.splitlines()]
        for expected_line in NODA_RETAINED_MARKER_LINES:
            prefix = expected_line.split("]", 1)[0] + "]"
            marker_lines = [
                line for line in all_lines if line.startswith(prefix)
            ]
            _require(
                marker_lines == [expected_line],
                source,
                "NODA retained marker is not exact-line and exact-once: "
                f"{prefix}",
            )
            companion_count += 1
        _require(
            not any(
                line.startswith("[A64_PROFITABLE_BLOCK_ADMISSION_FINAL]")
                for line in all_lines
            ),
            source,
            "PBA marker appeared in the mutually exclusive NODA record",
        )
        return companion_count
    if _metal_runtime_bundle_projection_scope_for(config, source) is not None:
        companion_count = 0
        for selector in (MSFC_COMPANION_SELECTOR, MDSSC_COMPANION_SELECTOR):
            contract = CANDIDATE_MARKER_ALLOWLIST[selector]
            expected_line = " ".join(
                [contract["prefix"], *contract["required_tokens"]]
            )
            marker_lines = [
                line
                for text in logs
                for line in text.splitlines()
                if line.startswith(contract["prefix"])
            ]
            _require(
                marker_lines == [expected_line],
                source,
                "MSORE+MSFC+MDSSC companion marker is not exact-line and exact-once",
            )
            companion_count += 1
        return companion_count
    if _ufsb_imse_direct_scope_for(config, source) is None:
        return 0
    contract = CANDIDATE_MARKER_ALLOWLIST[IMSE_COMPANION_SELECTOR]
    expected_line = " ".join(
        [contract["prefix"], *contract["required_tokens"]]
    )
    marker_lines = [
        line
        for text in logs
        for line in text.splitlines()
        if line.startswith(contract["prefix"])
    ]
    _require(
        marker_lines == [expected_line],
        source,
        "UFSB+IMSE companion marker is not exact-line and exact-once",
    )
    return 1


def _process_names(name: str, *, test_mode: bool) -> Sequence[int]:
    if test_mode:
        return []
    try:
        completed = subprocess.run(("/usr/bin/pgrep", "-x", name), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False, text=True)
    except OSError as error:
        raise HarnessError(f"cannot check residual process: {error}") from error
    _require(completed.returncode in (0, 1), name, "pgrep failed")
    if completed.returncode == 1:
        return []
    result = []
    for line in completed.stdout.splitlines():
        _require(line.isdigit(), name, "malformed pgrep output")
        result.append(int(line))
    return result


def _readonly_tree(path: pathlib.Path) -> None:
    for child in sorted(path.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        if child.is_symlink():
            raise HarnessError(f"{child}: symlink in sealed record")
        os.chmod(child, 0o555 if child.is_dir() else 0o444)
    os.chmod(path, 0o555)


def _checkpoint_decimal_migration_receipt(
    run_root: pathlib.Path,
    freeze: Mapping[str, Any],
    seal_harness_sha256: str,
    checkpoint_observation_sha256: str,
    checkpoint_line_sha256: str,
) -> Mapping[str, Any]:
    return {
        "schema": CHECKPOINT_DECIMAL_MIGRATION_SCHEMA,
        "record_id": _string(freeze["record_id"], run_root, "freeze.record_id"),
        "attestation": CHECKPOINT_DECIMAL_MIGRATION_ATTESTATION,
        "reason": CHECKPOINT_DECIMAL_MIGRATION_REASON,
        "legacy_harness_sha256": CHECKPOINT_DECIMAL_MIGRATION_LEGACY_HARNESS_SHA256,
        "seal_harness_sha256": seal_harness_sha256,
        "freeze_sha256": _sha_file(run_root / "freeze.json"),
        "checkpoint_observation_sha256": checkpoint_observation_sha256,
        "checkpoint_line_sha256": checkpoint_line_sha256,
    }


def create_checkpoint_decimal_migration(
    run_root: pathlib.Path, attestation: str
) -> pathlib.Path:
    """Publish a narrow, immutable receipt; never rewrite captured run data."""
    run_root = run_root.resolve()
    _require(
        attestation == CHECKPOINT_DECIMAL_MIGRATION_ATTESTATION,
        run_root,
        "explicit checkpoint decimal migration attestation is required",
    )
    freeze = _load_json(run_root / "freeze.json")
    registry_root = pathlib.Path(freeze["registry_root"]).resolve()
    _require(
        run_root.parent == registry_root / "pending",
        run_root,
        "migration is restricted to a pending registry run",
    )
    _require(
        run_root.name == _string(freeze["record_id"], run_root, "freeze.record_id"),
        run_root,
        "migration record_id mismatch",
    )
    legacy_harness_sha256 = _sha(
        freeze["harness_sha256"], run_root, "freeze.harness_sha256"
    )
    _require(
        legacy_harness_sha256
        == CHECKPOINT_DECIMAL_MIGRATION_LEGACY_HARNESS_SHA256,
        run_root,
        "frozen harness is not the single migration-eligible legacy build",
    )
    seal_harness_sha256 = _sha_file(pathlib.Path(__file__).resolve())
    _require(
        seal_harness_sha256 != legacy_harness_sha256,
        run_root,
        "migration requires a corrected seal harness",
    )
    _require(
        (run_root / ".exit_status").read_bytes() == b"0\n",
        run_root,
        "migration requires a completed clean process status",
    )
    _require(not _pid_alive(run_root), run_root, "migration requires an exited emulator")
    checkpoint_line, raw_line = _checkpoint_raw_line(run_root / "stderr.log")
    checkpoint = _checkpoint_from_line(checkpoint_line, run_root / "stderr.log")
    observation_path = run_root / "checkpoint-observation.json"
    observation = _load_json(observation_path)
    _validate_checkpoint_observation(observation, checkpoint, raw_line, observation_path)
    _require(
        observation["checkpoint"] != _checkpoint_json_value(checkpoint),
        run_root,
        "legacy Decimal-vs-float checkpoint failure does not reproduce",
    )
    receipt = _checkpoint_decimal_migration_receipt(
        run_root,
        freeze,
        seal_harness_sha256,
        _sha_file(observation_path),
        hashlib.sha256(raw_line).hexdigest(),
    )
    receipt_path = run_root / CHECKPOINT_DECIMAL_MIGRATION_RECEIPT_NAME
    seal_harness_snapshot_path = (
        run_root / CHECKPOINT_DECIMAL_MIGRATION_SEAL_HARNESS_NAME
    )
    try:
        seal_harness_payload = pathlib.Path(__file__).resolve().read_bytes()
    except OSError as error:
        raise HarnessError(f"cannot snapshot seal harness: {error}") from error
    _require(
        hashlib.sha256(seal_harness_payload).hexdigest() == seal_harness_sha256,
        run_root,
        "seal harness changed while preparing migration",
    )
    # Each publication is independently idempotent only for exact immutable
    # bytes. This lets an interrupted migration resume from either valid
    # prefix without permitting overwrite or conflicting evidence.
    _publish_exact_bytes(
        seal_harness_snapshot_path, seal_harness_payload, mode=0o444
    )
    _publish_exact_bytes(receipt_path, _json_bytes(receipt), mode=0o444)
    return receipt_path


def _validate_checkpoint_decimal_migration(
    run_root: pathlib.Path,
    freeze: Mapping[str, Any],
    seal_harness_sha256: str,
    checkpoint_observation_sha256: str,
    checkpoint_line_sha256: str,
) -> str:
    receipt_path = run_root / CHECKPOINT_DECIMAL_MIGRATION_RECEIPT_NAME
    receipt = _exact(
        _load_json(receipt_path),
        CHECKPOINT_DECIMAL_MIGRATION_KEYS,
        receipt_path,
        "checkpoint decimal migration receipt",
    )
    expected = _checkpoint_decimal_migration_receipt(
        run_root,
        freeze,
        seal_harness_sha256,
        checkpoint_observation_sha256,
        checkpoint_line_sha256,
    )
    _require(
        receipt == expected,
        receipt_path,
        "checkpoint decimal migration receipt binding mismatch",
    )
    seal_harness_snapshot_path = (
        run_root / CHECKPOINT_DECIMAL_MIGRATION_SEAL_HARNESS_NAME
    )
    _require(
        _sha_file(seal_harness_snapshot_path) == seal_harness_sha256,
        seal_harness_snapshot_path,
        "archived migration seal harness mismatch",
    )
    return _sha_file(receipt_path)


def seal_run(
    run_root: pathlib.Path,
    no_glitch_attestation: str,
    *,
    test_mode: bool = False,
    checkpoint_decimal_migration: bool = False,
    _configuration_projection_policy: Optional[Mapping[str, Any]] = None,
) -> pathlib.Path:
    run_root = run_root.resolve()
    _require(no_glitch_attestation == "CONFIRM_NO_GLITCH_OBSERVED", run_root, "explicit no-glitch attestation is required")
    config = _validate_start_config(run_root / "start-config.json")
    freeze = _load_json(run_root / "freeze.json")
    _require(
        _bool(freeze.get("test_mode"), run_root, "freeze.test_mode") == test_mode,
        run_root,
        "seal mode differs from the frozen launch mode",
    )
    _require(
        _sha_file(run_root / "start-config.json") == freeze["start_config_sha256"],
        run_root,
        "start config changed after launch",
    )
    registry_root = pathlib.Path(freeze["registry_root"]).resolve()
    _require(run_root.parent == registry_root / "pending", run_root, "run is not in the registry pending directory")
    frozen_record_id = _string(freeze["record_id"], run_root, "freeze.record_id")
    _require(
        config["provenance"]["record_id"] == frozen_record_id
        and run_root.name == frozen_record_id,
        run_root,
        "start config/run directory record_id differs from freeze",
    )
    epoch = _load_epoch(registry_root)
    _require(
        _canonical_sha(epoch)
        == _sha(freeze["epoch_sha256"], run_root, "freeze.epoch_sha256")
        and epoch["protocol"] == config["protocol"]
        and epoch["comparability"] == config["comparability"],
        run_root,
        "start config or registry epoch differs from freeze",
    )
    _require(
        _canonical_sha(config["lineage"])
        == _sha(freeze["lineage_sha256"], run_root, "freeze.lineage_sha256"),
        run_root,
        "start config lineage differs from freeze",
    )
    configuration_artifacts = _validate_configuration_projection_artifacts(
        run_root,
        config,
        freeze,
        test_mode=test_mode,
        policy_override=_configuration_projection_policy,
    )
    for required in ("stdout.log", "stderr.log", ".exit_status", "process-time.json", "checkpoint-observation.json", "anchors/fort_mode.json", "anchors/in_game.json"):
        _require((run_root / required).is_file(), run_root, f"missing {required}")
    _require(not (run_root / "anchors" / "fort_mode.pending.json").exists() and not (run_root / "anchors" / "in_game.pending.json").exists(), run_root, "pending anchor remains")
    status_bytes = (run_root / ".exit_status").read_bytes()
    _require(status_bytes == b"0\n", run_root, "exit status must be byte-exact 0\\n")
    _require(not _pid_alive(run_root), run_root, "recorded emulator PID is still alive")
    binary_path = pathlib.Path(freeze["binary_path"])
    residual = _process_names(binary_path.name, test_mode=test_mode)
    _require(not residual, run_root, f"residual emulator processes: {residual}")
    atomic_debris = sorted(
        str(path.relative_to(run_root))
        for path in run_root.rglob(".*.tmp.*")
    )
    _require(
        not atomic_debris,
        run_root,
        f"unindexed atomic staging files remain: {atomic_debris}",
    )
    _require(_sha_file(binary_path) == freeze["binary_sha256"], binary_path, "binary changed after launch")
    harness_path = pathlib.Path(__file__).resolve()
    seal_harness_sha256 = _sha_file(harness_path)
    frozen_harness_sha256 = _sha(
        freeze["harness_sha256"], run_root, "freeze.harness_sha256"
    )
    harness_changed = seal_harness_sha256 != frozen_harness_sha256
    if harness_changed:
        _require(
            checkpoint_decimal_migration
            and frozen_harness_sha256
            == CHECKPOINT_DECIMAL_MIGRATION_LEGACY_HARNESS_SHA256,
            harness_path,
            "harness changed after launch without the explicit eligible checkpoint decimal migration",
        )
    else:
        _require(
            not checkpoint_decimal_migration,
            harness_path,
            "checkpoint decimal migration was requested but the frozen harness is current",
        )
        _require(
            not (run_root / CHECKPOINT_DECIMAL_MIGRATION_RECEIPT_NAME).exists()
            and not (
                run_root / CHECKPOINT_DECIMAL_MIGRATION_SEAL_HARNESS_NAME
            ).exists(),
            run_root,
            "unexpected checkpoint decimal migration evidence on a normal seal",
        )
    report_tool_path = pathlib.Path(freeze["report_tool_path"])
    _require(_sha_file(report_tool_path) == freeze["report_tool_sha256"], report_tool_path, "report tool changed after launch")

    stdout = (run_root / "stdout.log").read_text(encoding="utf-8", errors="strict")
    stderr = (run_root / "stderr.log").read_text(encoding="utf-8", errors="strict")
    logs = (stdout, stderr)
    frames = _parse_frames(run_root / "stderr.log")
    checkpoint_line, checkpoint_raw_line = _checkpoint_raw_line(
        run_root / "stderr.log"
    )
    checkpoint = dict(
        _checkpoint_json_value(
            _checkpoint_from_line(checkpoint_line, run_root / "stderr.log")
        )
    )
    checkpoint["count"] = 1
    stderr_lines = stderr.splitlines()
    checkpoint_index = stderr_lines.index(checkpoint_line)
    p2280_indices = [index for index, line in enumerate(stderr_lines) if line.startswith("[FRAME_PIPELINE] present=2280 ")]
    p2400_indices = [index for index, line in enumerate(stderr_lines) if line.startswith("[FRAME_PIPELINE] present=2400 ")]
    _require(
        len(p2280_indices) == 1
        and len(p2400_indices) == 1
        and p2280_indices[0] < checkpoint_index < p2400_indices[0],
        run_root,
        "checkpoint is not ordered between p2280 and p2400",
    )
    _require(not any(line.startswith("[FRAME_LIMIT]") for line in stderr.splitlines()), run_root, "terminal present limit appeared")
    observation_path = run_root / "checkpoint-observation.json"
    observation = _validate_checkpoint_observation(
        _load_json(observation_path),
        _checkpoint_from_line(checkpoint_line, run_root / "stderr.log"),
        checkpoint_raw_line,
        observation_path,
    )
    checkpoint_decimal_migration_receipt_sha256 = None
    if harness_changed:
        _require(
            observation["checkpoint"]
            != {key: checkpoint[key] for key in checkpoint if key != "count"},
            run_root,
            "legacy Decimal-vs-float checkpoint failure does not reproduce",
        )
        checkpoint_decimal_migration_receipt_sha256 = (
            _validate_checkpoint_decimal_migration(
                run_root,
                freeze,
                seal_harness_sha256,
                _sha_file(observation_path),
                hashlib.sha256(checkpoint_raw_line).hexdigest(),
            )
        )
    launch_ns = _uint(observation["launch_uptime_raw_ns"], run_root, "launch clock")
    observed_ns = _uint(observation["checkpoint_observed_uptime_raw_ns"], run_root, "checkpoint observation clock")
    _require(observed_ns > launch_ns and observation["wall_ns"] == observed_ns - launch_ns, run_root, "invalid p2300 wall clock")

    receipt = config["provenance"]["run_receipt"]
    receipt_count = _count_exact_line(logs, receipt)
    _require(receipt_count == 1, run_root, "run receipt is not exact-once")
    _require(
        receipt.startswith(A64_CONFIGURATION_PREFIX),
        run_root,
        "run receipt must be the structured A64 dynarec configuration line",
    )
    receipt_prefix = A64_CONFIGURATION_PREFIX
    prefixed_receipts = [line for text in logs for line in text.splitlines() if line.startswith(receipt_prefix)]
    _require(prefixed_receipts == [receipt], run_root, "run receipt prefix is duplicated or mismatched")
    a64_fields = _receipt_fields(receipt, receipt_prefix, run_root)
    for field, expected in A64_REQUIRED_RECEIPT_FIELDS.items():
        _require(
            a64_fields.get(field) == expected,
            run_root,
            f"A64 production receipt requires {field}={expected}",
        )
    for field, expected in _candidate_required_receipt_fields(
        config, run_root
    ).items():
        _require(
            a64_fields.get(field) == expected,
            run_root,
            f"candidate runtime receipt requires {field}={expected}",
        )
    effective_selectors, selector_receipt_lines = _effective_selectors_from_logs(
        logs, config, run_root
    )
    selector_receipt_sha256 = _canonical_sha(selector_receipt_lines)
    all_log_lines = [line for text in logs for line in text.splitlines()]
    gfx_receipts = [
        line for line in all_log_lines if line.startswith(GFX_CONFIGURATION_PREFIX)
    ]
    _require(len(gfx_receipts) == 1, run_root, "GFX descriptor receipt is not exact-once")
    gfx_fields = _receipt_fields(
        gfx_receipts[0], GFX_CONFIGURATION_PREFIX, run_root
    )
    for field, expected in GFX_REQUIRED_RECEIPT_FIELDS.items():
        _require(
            gfx_fields.get(field) == expected,
            run_root,
            f"GFX production receipt requires {field}={expected}",
        )
    gfx_finals = [
        line for line in all_log_lines if line.startswith(GFX_FINAL_PREFIX)
    ]
    _require(len(gfx_finals) == 1, run_root, "6CJ final marker is not exact-once")
    gfx_final = _receipt_fields(gfx_finals[0], GFX_FINAL_PREFIX, run_root)
    _require(
        gfx_final.get("enabled") == "1"
        and gfx_final.get("backend") == "shared_zero_rgba8"
        and gfx_final.get("status") == "clean",
        run_root,
        "6CJ final marker is not enabled/clean on the expected backend",
    )
    _require(
        re.fullmatch(UNSIGNED_RE, gfx_final.get("prepared", "")) is not None
        and gfx_final.get("dummy_created") in ("0", "1")
        and (
            int(gfx_final["prepared"]) == 0
            or gfx_final["dummy_created"] == "1"
        ),
        run_root,
        "6CJ prepared/dummy coverage is inconsistent",
    )
    sdl_quit_count = _count_exact_line(logs, "[WINDOW_EXIT] SDL_QUIT")
    _require(sdl_quit_count == 1, run_root, "SDL_QUIT is not exact-once")
    marker_config = config["candidate_marker"]
    marker_count, coverage_positive = _validate_candidate_marker_lines(
        logs, config, run_root
    )
    _validate_companion_candidate_marker_lines(logs, config, run_root)
    candidate_marker_lifecycle = {
        "required": marker_config["required"],
        "expected_count": 1 if marker_config["required"] else 0,
        "observed_count": marker_count,
        "status": "clean" if marker_config["required"] else "not_applicable",
        "coverage_required": marker_config["coverage_required"],
        "coverage_positive": coverage_positive,
    }
    if config["provenance"]["candidate_selector"] in (
        FCFOS_CANDIDATE_SELECTOR,
        ALC_CANDIDATE_SELECTOR,
        MSORE_CANDIDATE_SELECTOR,
        NODA_CANDIDATE_SELECTOR,
    ):
        exact_marker_lines = [
            line
            for line in all_log_lines
            if line.startswith(marker_config["prefix"])
        ]
        _require(
            len(exact_marker_lines) == 1,
            run_root,
            "candidate marker line is not available for the sealed manifest",
        )
        # Preserve the exact line that the harness already validated against
        # the raw log.  The standalone report can then fail closed without
        # trusting an internally reconstructed marker constant.
        candidate_marker_lifecycle["line"] = exact_marker_lines[0]

    lower_logs = (stdout + "\n" + stderr).lower()
    fatal_count = len(re.findall(r"fatal error|status=(?:unhealthy|invalid)", lower_logs))
    assert_count = len(re.findall(r"assertion failed", lower_logs))
    unhealthy_count = len(re.findall(r"a64 dynarec disabled|healthy=0", lower_logs))
    descriptor_count = len(re.findall(r"gfx_descriptor_mismatch|descriptor mismatch", lower_logs))
    device_lost_count = len(re.findall(r"device[- ]lost", lower_logs))
    _require(fatal_count == assert_count == unhealthy_count == descriptor_count == device_lost_count == 0, run_root, "fatal/unhealthy/mismatch evidence")

    fort = _validate_anchor_value(
        run_root,
        "fort_mode",
        _load_json(run_root / "anchors" / "fort_mode.json"),
        final=True,
    )
    game = _validate_anchor_value(
        run_root,
        "in_game",
        _load_json(run_root / "anchors" / "in_game.json"),
        final=True,
    )
    for scene, value in (("fort_mode", fort), ("in_game", game)):
        _require(value["endpoint_before"] == value["endpoint_after"], scene, "anchor endpoint raced")
        _require(value["anchor_endpoint"] == value["endpoint_before"] + WINDOW_SIZE and value["measured_endpoint"] == value["anchor_endpoint"] + WINDOW_SIZE, scene, "anchor geometry mismatch")
        measured = value["measured_endpoint"]
        _require(measured in frames and decimal.Decimal(str(value["avg_interval_ms"])) == frames[measured], scene, "stored measurement differs from raw log")
    _require(game["endpoint_before"] > fort["measured_endpoint"], run_root, "in-game anchor order mismatch")
    exit_ns = _uint(
        int((run_root / "exit-uptime-raw-ns.txt").read_text(encoding="ascii", errors="strict").strip()),
        run_root,
        "exit clock",
    )
    _require(
        launch_ns
        < observed_ns
        < fort["receipt_uptime_raw_ns"]
        < fort["completed_uptime_raw_ns"]
        < game["receipt_uptime_raw_ns"]
        < game["completed_uptime_raw_ns"]
        < exit_ns,
        run_root,
        "checkpoint/announcement/completion/exit clocks are not strictly ordered",
    )

    save_template = pathlib.Path(freeze["save_template_path"])
    template_after = _tree_digest(save_template)
    special_work = run_root / "work" / "_SaveData" / pathlib.Path(*pathlib.PurePosixPath(config["paths"]["save_special_relative"]).parts)
    special_after = _sha_file(special_work)
    template_unchanged = template_after == freeze["save_template_before_sha256"]
    special_unchanged = special_after == freeze["save_special_before_sha256"]
    _require(template_unchanged and special_unchanged, run_root, "Special or authoritative save template changed")
    _atomic_bytes(run_root / "save-special-after.sha256", (special_after + "\n").encode("ascii"))
    _atomic_bytes(run_root / "save-template-after.sha256", (template_after + "\n").encode("ascii"))

    process_times = _load_json(run_root / "process-time.json")
    for name in ("real", "user", "sys"):
        _require(name in process_times and decimal.Decimal(process_times[name]) >= 0, process_times, f"invalid {name} time")
    _require(decimal.Decimal(process_times["real"]) > 0, process_times, "real time must be positive")
    artifacts = {
        "stdout_log": _sha_file(run_root / "stdout.log"),
        "stderr_log": _sha_file(run_root / "stderr.log"),
        "exit_status": _sha_file(run_root / ".exit_status"),
        "fort_anchor_receipt": _sha_file(run_root / "anchors" / "fort_mode.json"),
        "in_game_anchor_receipt": _sha_file(run_root / "anchors" / "in_game.json"),
        "save_special_before": freeze["save_special_before_sha256"],
        "save_special_after": special_after,
        "save_template_before": freeze["save_template_before_sha256"],
        "save_template_after": template_after,
        "start_config": _sha_file(run_root / "start-config.json"),
        "checkpoint_observation": _sha_file(run_root / "checkpoint-observation.json"),
        "process_time": _sha_file(run_root / "process-time.json"),
        "exit_uptime_raw": _sha_file(run_root / "exit-uptime-raw-ns.txt"),
        "selector_receipts": selector_receipt_sha256,
    }
    artifacts.update(configuration_artifacts)
    if checkpoint_decimal_migration_receipt_sha256 is not None:
        artifacts.update(
            {
                "checkpoint_decimal_migration_receipt": checkpoint_decimal_migration_receipt_sha256,
                "checkpoint_decimal_migration_freeze": _sha_file(
                    run_root / "freeze.json"
                ),
                "checkpoint_decimal_migration_seal_harness": seal_harness_sha256,
            }
        )
    manifest = {
        "schema": {"name": RUN_MANIFEST_SCHEMA_NAME, "version": RUN_MANIFEST_SCHEMA_VERSION},
        "protocol": config["protocol"],
        "comparability": config["comparability"],
        "provenance": {
            "record_id": config["provenance"]["record_id"],
            "source_revision": freeze["source_revision"],
            "binary_sha256": freeze["binary_sha256"],
            "diff_sha256": freeze["diff_sha256"],
            "harness_sha256": freeze["harness_sha256"],
            "announcement_sidecar_sha256": freeze["announcement_sidecar_sha256"],
            "report_tool_sha256": freeze["report_tool_sha256"],
            "run_receipt": receipt,
            "selector_receipt_sha256": selector_receipt_sha256,
            "selectors": {
                "expected": config["provenance"]["selectors"],
                "effective": effective_selectors,
            },
            "artifacts_sha256": artifacts,
        },
        "lineage": config["lineage"],
        "measurements": {
            "loading": {
                "launch_uptime_raw_ns": launch_ns,
                "checkpoint_observed_uptime_raw_ns": observed_ns,
                "wall_ns": observed_ns - launch_ns,
                "checkpoint": checkpoint,
            },
            "fort_mode": {
                "receipt_uptime_raw_ns": fort["receipt_uptime_raw_ns"],
                "endpoint_before": fort["endpoint_before"],
                "endpoint_after": fort["endpoint_after"],
                "anchor_endpoint": fort["anchor_endpoint"],
                "measured_endpoint": fort["measured_endpoint"],
                "avg_interval_ms": float(fort["avg_interval_ms"]),
            },
            "in_game": {
                "receipt_uptime_raw_ns": game["receipt_uptime_raw_ns"],
                "endpoint_before": game["endpoint_before"],
                "endpoint_after": game["endpoint_after"],
                "anchor_endpoint": game["anchor_endpoint"],
                "measured_endpoint": game["measured_endpoint"],
                "avg_interval_ms": float(game["avg_interval_ms"]),
            },
            "supporting": {"process_time_seconds": {name: float(process_times[name]) for name in ("real", "user", "sys")}},
        },
        "lifecycle": {
            "status": "clean",
            "exit_status_bytes": "0\n",
            "receipt_count": receipt_count,
            "checkpoint_count": 1,
            "sdl_quit_count": sdl_quit_count,
            "residual_process_count": len(residual),
            "fatal_count": fatal_count,
            "assert_count": assert_count,
            "dynarec_unhealthy_count": unhealthy_count,
            "descriptor_mismatch_count": descriptor_count,
            "device_lost_count": device_lost_count,
            "glitch_observed": False,
            "save_special_unchanged": special_unchanged,
            "save_template_unchanged": template_unchanged,
            "fort_mode_oracle_confirmed": True,
            "sterile_environment_confirmed": True,
            "fresh_save_confirmed": True,
            "frame_trace_enabled": config["launch"]["common_environment"].get("LUMEN_FRAME_PIPELINE_TRACE") == "1",
            "present_limit_absent": "LUMEN_FRAME_PRESENT_LIMIT" not in {**config["launch"]["common_environment"], **config["launch"]["experiment_environment"]},
            "manual_input_only": "CONFIRM_MANUAL_INPUT_ONLY" in fort["attestations"] and "CONFIRM_MANUAL_INPUT_ONLY" in game["attestations"],
            "no_input_before_p2300": "CONFIRM_NO_INPUT_BEFORE_P2300" in fort["attestations"],
            "fort_window_input_free": "CONFIRM_ZERO_INPUT_SINCE_FORT_ANNOUNCEMENT" in fort["completion_attestations"],
            "in_game_window_input_free": "CONFIRM_ZERO_INPUT_SINCE_INGAME_ANNOUNCEMENT" in game["completion_attestations"],
            "in_game_oracle": {
                "objective_visible": True,
                "terrain_island_visible": True,
                "boggy_hp": 100,
                "hud_intact": True,
                "timer_advanced": game["timer_after"] > game["timer_before"],
            },
            "candidate_marker": candidate_marker_lifecycle,
        },
    }
    temporary_manifest = run_root / ".manifest.json.validating"
    _atomic_json(temporary_manifest, manifest)
    report_module = _load_report_module(report_tool_path)
    try:
        report_load_options = (
            {"_projection_contract": _configuration_projection_policy}
            if test_mode and _configuration_projection_policy is not None
            else {}
        )
        report_module.load_manifest(temporary_manifest, **report_load_options)
    except Exception as error:
        raise HarnessError(f"manifest rejected by persistent report schema: {error}") from error
    manifest_path = run_root / "manifest.json"
    _atomic_json(manifest_path, manifest)
    temporary_manifest.unlink()
    record_root = registry_root / "records" / frozen_record_id
    with _DirectoryLock(registry_root / ".registry.lock"):
        _require(not record_root.exists(), record_root, "sealed record already exists")
        os.replace(run_root, record_root)
        _readonly_tree(record_root)
        directory = os.open(record_root.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    return record_root / "manifest.json"


def _wait_for(path: pathlib.Path, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.01)
    raise HarnessError(f"self-test timeout waiting for {path}")


def _write_png(path: pathlib.Path, payload: bytes) -> None:
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + payload)


def _fixture_protocol(protocol_id: str) -> Mapping[str, Any]:
    return {"protocol_id": protocol_id, "protocol_revision": 1, **PROTOCOL_CONSTANTS}


def _self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="persistent-performance-harness-self-test-") as temporary:
        root = pathlib.Path(temporary)
        repo = root / "repo"
        repo.mkdir()
        subprocess.run(("/usr/bin/git", "init", "-q"), cwd=repo, check=True)
        subprocess.run(("/usr/bin/git", "config", "user.email", "fixture@example.invalid"), cwd=repo, check=True)
        subprocess.run(("/usr/bin/git", "config", "user.name", "Fixture"), cwd=repo, check=True)
        report_source = pathlib.Path(__file__).with_name("persistent_performance_report.py")
        _require(report_source.is_file(), report_source, "report tool is required by harness self-test")
        report_tool = repo / "persistent_performance_report.py"
        shutil.copyfile(report_source, report_tool)
        configuration = repo / "CMakeCache.txt"
        projected_configuration = b"LUMEN_APPLE_NATIVE_METAL:BOOL=ON\n"
        fixture_configuration_raw = (
            projected_configuration
            + PHASE6CP_PHASE6CM_CACHE_BLOCK
            + PHASE6CP_PHASE6CO_CACHE_BLOCK
        )
        configuration.write_bytes(projected_configuration)
        lua = repo / "run.lua"
        lua.write_text("-- fixture\n", encoding="utf-8")
        rom = repo / "eboot.bin"
        rom.write_bytes(b"fixture-rom")
        save = repo / "_SaveData"
        (save / "SAVEDATA0").mkdir(parents=True)
        (save / "SAVEDATA0" / "MainData.0").write_bytes(b"main")
        (save / "SAVEDATA0" / "Special.0").write_bytes(b"special")
        fake = repo / "fake-emulator.py"
        fake.write_text(
            """#!/usr/bin/env python3
import os, pathlib, stat, sys, time
root = pathlib.Path(os.environ['TEST_RUN_ROOT'])
projection_paths = (
    root/'configuration-projection.json',
    root/'configuration-cache.raw',
)
if any(
    not path.is_file() or stat.S_IMODE(path.stat().st_mode) != 0o444
    for path in projection_paths
):
    raise RuntimeError('projection evidence was not frozen before exec')
def emit(line): print(line, file=sys.stderr, flush=True)
emit(os.environ['TEST_RECEIPT'])
emit('GFX descriptor configuration: null_sampled_image=1 null_sampled_scope=zero8_readonly_ps_gen4 null_sampled_backend=shared_zero_rgba8')
pathlib.Path('_SaveData/SAVEDATA0/MainData.0').write_bytes(b'autosave')
emit('[FRAME_PIPELINE] present=1 avg_interval=0.000 ms avg_latency=1.000 ms pending=0 index=0 arg=0')
for p in range(120, 2401, 120):
    emit(f'[FRAME_PIPELINE] present={p} avg_interval=10.000 ms avg_latency=1.000 ms pending=0 index=0 arg=0')
    if p == 2280:
        emit('[FRAME_CHECKPOINT] present=2300 target=2300 cumulative_ms=22990.001 cumulative_intervals=2299 partial_ms=200.003 partial_intervals=20 terminal=0')
while not (root/'anchors/fort_mode.pending.json').exists(): time.sleep(.005)
for p in (2520,2640): emit(f'[FRAME_PIPELINE] present={p} avg_interval=9.000 ms avg_latency=1.000 ms pending=0 index=0 arg=0')
while not (root/'anchors/fort_mode.json').exists(): time.sleep(.005)
for p in range(2760, 3601, 120): emit(f'[FRAME_PIPELINE] present={p} avg_interval=8.000 ms avg_latency=1.000 ms pending=0 index=0 arg=0')
while not (root/'anchors/in_game.pending.json').exists(): time.sleep(.005)
for p in (3720,3840): emit(f'[FRAME_PIPELINE] present={p} avg_interval=7.000 ms avg_latency=1.000 ms pending=0 index=0 arg=0')
while not (root/'anchors/in_game.json').exists(): time.sleep(.005)
emit(os.environ['TEST_MARKER'])
emit('[GFX_NULL_SAMPLED_FINAL] enabled=1 prepared=0 dummy_created=0 backend=shared_zero_rgba8 status=clean')
emit('[WINDOW_EXIT] SDL_QUIT')
""",
            encoding="utf-8",
        )
        fake.chmod(0o755)
        subprocess.run(("/usr/bin/git", "add", "."), cwd=repo, check=True)
        subprocess.run(("/usr/bin/git", "commit", "-qm", "fixture"), cwd=repo, check=True)
        for child in sorted(save.rglob("*"), reverse=True):
            child.chmod(0o555 if child.is_dir() else 0o444)
        save.chmod(0o555)

        registry = root / "registry"
        (registry / "pending").mkdir(parents=True)
        (registry / "records").mkdir()
        account = pwd.getpwuid(os.getuid())
        common_env = {
            "HOME": account.pw_dir,
            "USER": account.pw_name,
            "LOGNAME": account.pw_name,
            **COMMON_ENVIRONMENT_VALUES,
        }
        protocol = _fixture_protocol("fixture-announced-v1")
        digest = "a" * 64
        comparability = {
            "host": {"host_id": "fixture", "model": "fixture", "architecture": "arm64"},
            "os": {"name": "macOS", "version": "fixture", "build": "fixture", "policy": "frozen"},
            "power": {"source": "AC", "mode": "normal", "policy": "AC normal"},
            "workload": {
                "rom_sha256": _file_set_digest([rom]),
                "lua_sha256": _sha_file(lua),
                "save_template_sha256": _tree_digest(save),
                "cache_policy": "fresh-empty",
                "cache_seed_sha256": None,
            },
            "runtime": {
                "sterile_environment_sha256": _canonical_sha(common_env),
                "backend": "NativeMetal",
                "configuration_sha256": _sha_bytes(projected_configuration),
            },
            "scenes": {"fort_mode_oracle": "Fort Mode stable panel", "in_game_oracle": "Training Navigation objective terrain Boggy 100 HP HUD timer"},
            "tool_schemas": {"harness": HARNESS_SCHEMA, "announcement_sidecar": ANCHOR_SCHEMA, "report": REPORT_TOOL_SCHEMA},
        }
        _atomic_json(registry / "epoch.json", {"schema": {"name": EPOCH_SCHEMA_NAME, "version": EPOCH_SCHEMA_VERSION}, "protocol": protocol, "comparability": comparability})
        projection_receipt_fields = {
            **A64_REQUIRED_RECEIPT_FIELDS,
            "stable_negative_route": "1",
            "guest_memory_operation_snapshot_elision": "1",
        }
        projection_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}" for name, value in projection_receipt_fields.items()
        )
        projection_marker = (
            "[A64_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION_FINAL] "
            "elided=positive signature_snapshot=preserved "
            "status=clean coverage=positive"
        )
        projection_config = {
            "schema": {"name": START_SCHEMA_NAME, "version": START_SCHEMA_VERSION},
            "protocol": protocol,
            "comparability": comparability,
            "provenance": {
                "record_id": "candidate-fixture",
                "run_receipt": projection_receipt,
                "candidate_selector": "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION",
                "selectors": {
                    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
                },
                "selector_receipts": {
                    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": {
                        "prefix": A64_CONFIGURATION_PREFIX,
                        "field": "stable_negative_route",
                    },
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": {
                        "prefix": A64_CONFIGURATION_PREFIX,
                        "field": "guest_memory_operation_snapshot_elision",
                    },
                },
            },
            "lineage": {"generation": 1, "parent_record_id": "baseline-fixture", "original_role": "candidate"},
            "candidate_marker": {
                "required": True,
                "prefix": "[A64_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION_FINAL]",
                "required_tokens": [
                    "elided=positive",
                    "signature_snapshot=preserved",
                    "status=clean",
                    "coverage=positive",
                ],
                "coverage_required": True,
                "coverage_token": "coverage=positive",
            },
            "paths": {
                "binary": str(fake), "lua": str(lua), "save_template": str(save), "save_special_relative": "SAVEDATA0/Special.0",
                "rom_files": [str(rom)], "cache_seed": None, "configuration": str(configuration), "report_tool": str(report_tool), "repo_root": str(repo),
            },
            "launch": {
                "arguments": [],
                "common_environment": {**common_env, "TEST_RUN_ROOT": str(registry / "pending" / "candidate-fixture"), "TEST_RECEIPT": projection_receipt, "TEST_MARKER": projection_marker},
                "experiment_environment": {
                    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": "1",
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": "1",
                },
            },
        }
        fixture_projection_policy = {
            **PHASE6CP_CONFIGURATION_PROJECTION_POLICY,
            "protocol_id": protocol["protocol_id"],
            "record_id": projection_config["provenance"]["record_id"],
            "generation": projection_config["lineage"]["generation"],
            "parent_record_id": projection_config["lineage"]["parent_record_id"],
            "original_role": projection_config["lineage"]["original_role"],
            "raw_bytes": len(fixture_configuration_raw),
            "raw_sha256": _sha_bytes(fixture_configuration_raw),
            "projected_bytes": len(projected_configuration),
            "projected_sha256": _sha_bytes(projected_configuration),
        }
        phase6cs_restore_payload = _phase6cs_restore_contract_payload(
            PHASE6CS_CONFIGURATION_RESTORE_OPERATIONS,
            "self-test phase-6CS production restore contract",
        )
        phase6da_restore_payload = _phase6da_restore_contract_payload(
            PHASE6DA_CONFIGURATION_RESTORE_OPERATIONS,
            "self-test phase-6DA production restore contract",
        )
        alc_strip_payload = _alc_configuration_strip_contract_payload(
            ALC_CONFIGURATION_STRIP_OPERATIONS,
            "self-test ALC production strip contract",
        )
        _require(
            CONFIGURATION_PROJECTION_SCOPES
            == (
                PHASE6CP_CONFIGURATION_PROJECTION_POLICY,
                PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE,
                PHASE6CS_CONFIGURATION_PROJECTION_SCOPE,
                PHASE6DA_CONFIGURATION_PROJECTION_SCOPE,
                ALC_CONFIGURATION_PROJECTION_SCOPE,
                METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE,
                NODA_CONFIGURATION_PROJECTION_SCOPE,
            )
            and PHASE6CS_CONFIGURATION_PROJECTION_SCOPE
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": "6cs-c-dead-rflags-elision-g000003",
                "generation": 3,
                "parent_record_id": "6cp-c-guest-memory-operation-snapshot-elision-g000002",
                "original_role": "candidate",
                "candidate_selector": PHASE6CS_CANDIDATE_SELECTOR,
                "selector_bundle": {
                    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
                    "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION": True,
                },
                "raw_bytes": 46_394,
                "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
                "projection_configured": True,
            }
            and PHASE6CS_CONFIGURATION_PROJECTION_ATTESTATION
            == "CONFIRM_PHASE6CS_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
            and PHASE6CS_CONFIGURATION_PROJECTION_POLICY_ID
            == "phase6cs-cmake-cache-production-v1"
            and len(phase6cs_restore_payload) == 4_788
            and _sha_bytes(phase6cs_restore_payload)
            == "15a48324c22c27e8959a0d8706965110199d7ca1d8203f10d94fd823fe6e57f9",
            "self-test",
            "phase-6CS production projection contract drifted",
        )
        _require(
            PHASE6DA_CONFIGURATION_PROJECTION_SCOPE
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": "6da-c-fast-block-dense-directory-g000003",
                "generation": 3,
                "parent_record_id": "6cp-c-guest-memory-operation-snapshot-elision-g000002",
                "original_role": "candidate",
                "candidate_selector": PHASE6DA_CANDIDATE_SELECTOR,
                "selector_bundle": {
                    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
                    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
                },
                "raw_bytes": 46_394,
                "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
                "projection_configured": True,
            }
            and PHASE6DA_CONFIGURATION_PROJECTION_ATTESTATION
            == "CONFIRM_PHASE6DA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
            and PHASE6DA_CONFIGURATION_PROJECTION_POLICY_ID
            == "phase6da-cmake-cache-production-v1"
            and PHASE6DA_CONFIGURATION_RESTORE_CONTRACT_PREFIX
            == b"lumen.phase6da.configuration-restore-operations.v1\0"
            and len(phase6da_restore_payload) == 4_788
            and _sha_bytes(phase6da_restore_payload)
            == "ca3126b513628e42a655bff5649ab3f7d4c915d89c2f881e68b07b023cea1c14",
            "self-test",
            "phase-6DA production projection contract drifted",
        )
        _require(
            ALC_CONFIGURATION_PROJECTION_SCOPE
            == {
                "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
                "record_id": "alc-c-arm-loading-composite-g000003",
                "generation": 3,
                "parent_record_id": IFSM_DIRECT_RECORD_ID,
                "original_role": "candidate",
                "candidate_selector": ALC_CANDIDATE_SELECTOR,
                "selector_bundle": {
                    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
                    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
                    IFSM_CANDIDATE_SELECTOR: True,
                    ALC_CANDIDATE_SELECTOR: True,
                },
                "raw_bytes": 46_749,
                "raw_sha256": (
                    "3d42b0edc9be9efa0156a02400de50ae4afb127ca30b7a74df0a36c1e2291e71"
                ),
                "projection_configured": True,
            }
            and ALC_CONFIGURATION_PROJECTION_ATTESTATION
            == "CONFIRM_ALC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
            and ALC_CONFIGURATION_PROJECTION_POLICY_ID
            == "alc-cmake-cache-production-v1"
            and ALC_CONFIGURATION_PROJECTION_MODE
            == "strip-exact-active-alc-build-options-v1"
            and ALC_CONFIGURATION_STRIP_CONTRACT_PREFIX
            == b"lumen.alc.configuration-strip-operations.v1\0"
            and len(alc_strip_payload) == 421
            and _sha_bytes(alc_strip_payload)
            == "704c751375758603328e79b34678e591ad4e52a419d18b35293f252516988e26"
            and [len(block) for _, block in ALC_CONFIGURATION_STRIP_OPERATIONS]
            == [126, 106, 123]
            and [_sha_bytes(block) for _, block in ALC_CONFIGURATION_STRIP_OPERATIONS]
            == [
                "ce5de28a9496702deaf942acd859a0b8b55befbbe172284a22af52a3dbd960f8",
                "953114130970c8adf91bd3c7851857540484bbd1b282a11379872a9565fafbdb",
                "8214513341e73e45fdb734c91c8302287c4e7a15c286c02bf32061937a4a95de",
            ]
            and ALC_CONFIGURATION_PROJECTION_POLICY["removed_bytes"] == 355
            and ALC_CONFIGURATION_PROJECTION_POLICY["removed_segment_sha256"]
            == "c0ec59ce9c4b95ef3bf3643b6fccbdf41aba1c1c034c8f8e630ad0ccc2b60e78"
            and ALC_CONFIGURATION_PROJECTION_POLICY["projected_bytes"]
            == 46_394
            and ALC_CONFIGURATION_PROJECTION_POLICY["projected_sha256"]
            == "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
            "self-test",
            "ALC generation-3 projection contract drifted",
        )
        _require(
            IFSM_DIRECT_SCOPE
            == {
                "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
                "record_id": (
                    "ifsm-c-fast-block-inline-fixed-signature-match-g000002"
                ),
                "generation": 2,
                "parent_record_id": (
                    "6da-c-fast-block-dense-directory-g000001"
                ),
                "original_role": "candidate",
                "candidate_selector": IFSM_CANDIDATE_SELECTOR,
                "selector_bundle": {
                    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
                    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
                    IFSM_CANDIDATE_SELECTOR: True,
                },
                "configuration_bytes": 46_394,
                "configuration_sha256": (
                    "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
                ),
            }
            and CANDIDATE_MARKER_ALLOWLIST[IFSM_CANDIDATE_SELECTOR]
            == {
                "prefix": (
                    "[A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH_FINAL]"
                ),
                "required_tokens": [
                    "requested=positive",
                    "effective=positive",
                    "compared=positive",
                    "status=clean",
                    "coverage=positive",
                ],
                "coverage_token": "coverage=positive",
                "exact_line": True,
            },
            "self-test",
            "IFSM generation-2 direct contract drifted",
        )
        _require(
            FCFOS_DIRECT_SCOPE
            == {
                "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
                "record_id": (
                    "fcfos-c-fast-block-callback-free-ordinary-"
                    "specialization-g000003"
                ),
                "generation": 3,
                "parent_record_id": IFSM_DIRECT_RECORD_ID,
                "original_role": "candidate",
                "candidate_selector": FCFOS_CANDIDATE_SELECTOR,
                "selector_bundle": {
                    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": True,
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION": True,
                    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY": True,
                    IFSM_CANDIDATE_SELECTOR: True,
                    FCFOS_CANDIDATE_SELECTOR: True,
                },
                "configuration_bytes": 46_394,
                "configuration_sha256": (
                    "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
                ),
            }
            and CANDIDATE_MARKER_ALLOWLIST[FCFOS_CANDIDATE_SELECTOR]
            == {
                "prefix": (
                    "[A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_"
                    "SPECIALIZATION_FINAL]"
                ),
                "required_tokens": [
                    "requested=positive",
                    "effective=positive",
                    "reentry_safe=positive",
                    "snapshot_elision=positive",
                    "status=clean",
                    "coverage=positive",
                ],
                "coverage_token": "coverage=positive",
                "exact_line": True,
            }
            and SELECTOR_RECEIPT_ALLOWLIST[FCFOS_CANDIDATE_SELECTOR]
            == {
                "prefix": A64_CONFIGURATION_PREFIX,
                "field": "fast_block_callback_free_ordinary_specialization",
            },
            "self-test",
            "FCFOS generation-3 direct contract drifted",
        )

        g3_receipt_fields = {
            **projection_receipt_fields,
            "scalar_memory_fallthrough_admission": "1",
        }
        g3_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}" for name, value in g3_receipt_fields.items()
        )
        g3_marker = (
            "[A64_SCALAR_MEMORY_FALLTHROUGH_ADMISSION_FINAL] "
            "admitted=positive native_executed=positive live_mismatch=none "
            "status=clean coverage=positive"
        )
        g3_config = json.loads(json.dumps(projection_config))
        g3_config["protocol"] = _fixture_protocol(
            PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE["protocol_id"]
        )
        g3_config["provenance"] = {
            "record_id": PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE["record_id"],
            "run_receipt": g3_receipt,
            "candidate_selector": PHASE6CQ_CANDIDATE_SELECTOR,
            "selectors": dict(
                PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE["selector_bundle"]
            ),
            "selector_receipts": {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE[
                    "selector_bundle"
                ]
            },
        }
        g3_config["lineage"] = {
            "generation": PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE[
                "generation"
            ],
            "parent_record_id": PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE[
                "parent_record_id"
            ],
            "original_role": PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE[
                "original_role"
            ],
        }
        g3_config["candidate_marker"] = {
            "required": True,
            "prefix": CANDIDATE_MARKER_ALLOWLIST[PHASE6CQ_CANDIDATE_SELECTOR][
                "prefix"
            ],
            "required_tokens": list(
                CANDIDATE_MARKER_ALLOWLIST[PHASE6CQ_CANDIDATE_SELECTOR][
                    "required_tokens"
                ]
            ),
            "coverage_required": True,
            "coverage_token": "coverage=positive",
        }
        g3_config["launch"]["common_environment"].update(
            {"TEST_RECEIPT": g3_receipt, "TEST_MARKER": g3_marker}
        )
        g3_config["launch"]["experiment_environment"] = {
            name: "1"
            for name in PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE[
                "selector_bundle"
            ]
        }
        g3_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(g3_config["launch"]["common_environment"])
        g3_config_path = root / "g3-start-validation.json"
        _atomic_json(g3_config_path, g3_config)
        validated_g3_config = _validate_start_config(g3_config_path)
        _require(
            _validate_candidate_marker_lines(
                [g3_marker], validated_g3_config, "self-test exact g3 marker"
            )
            == (1, True),
            "self-test",
            "exact g3 marker was not observed as positive",
        )
        for label, marker_logs in (
            ("extra token", [g3_marker + " unexpected=1"]),
            (
                "reordered fields",
                [
                    g3_marker.replace(
                        "admitted=positive native_executed=positive",
                        "native_executed=positive admitted=positive",
                    )
                ],
            ),
            ("duplicate", [g3_marker, g3_marker]),
        ):
            try:
                _validate_candidate_marker_lines(
                    marker_logs,
                    validated_g3_config,
                    f"self-test g3 marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted non-exact 6CQ marker {label}"
                )

        _require(
            _configuration_projection_policy_for(
                validated_g3_config,
                "self-test g3 production policy",
                test_mode=False,
                override=None,
            )
            is PHASE6CQ_CONFIGURATION_PROJECTION_POLICY,
            "self-test",
            "g3 production policy did not resolve from its exact scope",
        )

        # Exercise the complete dispatch and byte-exact projection path with a
        # test-only synthetic policy whose one restore operation is small
        # enough for exhaustive negative fixtures.  Production remains bound
        # to the immutable eight-operation contract above.
        g3_fixture_current = b"LUMEN_APPLE_NATIVE_METAL:BOOL=OFF\n"
        g3_fixture_restored = projected_configuration
        g3_fixture_configuration_raw = (
            g3_fixture_current
            + PHASE6CP_PHASE6CM_CACHE_BLOCK
            + PHASE6CP_PHASE6CO_CACHE_BLOCK
        )
        g3_fixture_restored_raw = (
            g3_fixture_restored
            + PHASE6CP_PHASE6CM_CACHE_BLOCK
            + PHASE6CP_PHASE6CO_CACHE_BLOCK
        )
        g3_fixture_operations = (
            (
                "fixture_native_metal_value",
                g3_fixture_current,
                g3_fixture_restored,
            ),
        )
        g3_fixture_contract_payload = _phase6cq_restore_contract_payload(
            g3_fixture_operations, "self-test synthetic g3 contract"
        )
        g3_fixture_projection_policy = {
            **fixture_projection_policy,
            **PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE,
            "projection_configured": True,
            "raw_bytes": len(g3_fixture_configuration_raw),
            "raw_sha256": _sha_bytes(g3_fixture_configuration_raw),
            "restore_operations": g3_fixture_operations,
            "restore_operation_count": len(g3_fixture_operations),
            "restore_operations_contract_bytes": len(
                g3_fixture_contract_payload
            ),
            "restore_operations_contract_sha256": _sha_bytes(
                g3_fixture_contract_payload
            ),
            "restored_bytes": len(g3_fixture_restored_raw),
            "restored_sha256": _sha_bytes(g3_fixture_restored_raw),
        }
        g3_projection_receipt = _configuration_projection_preflight(
            validated_g3_config,
            g3_fixture_configuration_raw,
            "self-test synthetic g3 projection",
            test_mode=True,
            policy_override=g3_fixture_projection_policy,
        )
        _require(
            g3_projection_receipt is not None
            and g3_projection_receipt["record_id"]
            == PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE["record_id"]
            and g3_projection_receipt["candidate_selector"]
            == PHASE6CQ_CANDIDATE_SELECTOR,
            "self-test",
            "synthetic g3 projection did not bind the exact record/selector",
        )
        _require(
            set(g3_projection_receipt)
            == PHASE6CQ_CONFIGURATION_PROJECTION_RECEIPT_KEYS
            and g3_projection_receipt["restore_operation_count"] == 1
            and g3_projection_receipt["restored_sha256"]
            == _sha_bytes(g3_fixture_restored_raw),
            "self-test",
            "synthetic g3 projection receipt schema/intermediate mismatch",
        )

        def g3_policy_for_raw(raw: bytes) -> Mapping[str, Any]:
            return {
                **g3_fixture_projection_policy,
                "raw_bytes": len(raw),
                "raw_sha256": _sha_bytes(raw),
            }

        g3_projection_negative_cases = {
            "raw hash": (
                g3_fixture_configuration_raw + b"x",
                g3_fixture_projection_policy,
            ),
            "missing current bytes": (
                g3_fixture_restored_raw,
                g3_policy_for_raw(g3_fixture_restored_raw),
            ),
            "duplicate current bytes": (
                g3_fixture_current + g3_fixture_configuration_raw,
                g3_policy_for_raw(
                    g3_fixture_current + g3_fixture_configuration_raw
                ),
            ),
            "pre-restored bytes": (
                g3_fixture_current
                + g3_fixture_restored
                + PHASE6CP_PHASE6CM_CACHE_BLOCK
                + PHASE6CP_PHASE6CO_CACHE_BLOCK,
                g3_policy_for_raw(
                    g3_fixture_current
                    + g3_fixture_restored
                    + PHASE6CP_PHASE6CM_CACHE_BLOCK
                    + PHASE6CP_PHASE6CO_CACHE_BLOCK
                ),
            ),
            "restore contract digest": (
                g3_fixture_configuration_raw,
                {
                    **g3_fixture_projection_policy,
                    "restore_operations_contract_sha256": "0" * 64,
                },
            ),
            "restored intermediate": (
                g3_fixture_configuration_raw,
                {
                    **g3_fixture_projection_policy,
                    "restored_sha256": "0" * 64,
                },
            ),
        }
        for label, (bad_raw, bad_policy) in (
            g3_projection_negative_cases.items()
        ):
            try:
                _phase6cq_configuration_projection_receipt(
                    validated_g3_config,
                    bad_raw,
                    f"self-test g3 projection {label}",
                    bad_policy,
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted 6CQ projection {label}"
                )

        conflicting_contract_config = json.loads(json.dumps(g3_config))
        conflicting_contract_config["provenance"]["record_id"] = (
            PHASE6CP_CONFIGURATION_PROJECTION_POLICY["record_id"]
        )
        try:
            _configuration_projection_policy_for(
                conflicting_contract_config,
                "self-test cross-contract signals",
                test_mode=False,
                override=None,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted 6CP record identity with the 6CQ selector bundle"
            )

        for label, mutate in (
            (
                "missing retained 6CP selector",
                lambda value: (
                    value["provenance"]["selectors"].pop(
                        PHASE6CP_CANDIDATE_SELECTOR
                    ),
                    value["provenance"]["selector_receipts"].pop(
                        PHASE6CP_CANDIDATE_SELECTOR
                    ),
                    value["launch"]["experiment_environment"].pop(
                        PHASE6CP_CANDIDATE_SELECTOR
                    ),
                ),
            ),
            (
                "false 6CQ selector",
                lambda value: (
                    value["provenance"]["selectors"].update(
                        {PHASE6CQ_CANDIDATE_SELECTOR: False}
                    ),
                    value["launch"]["experiment_environment"].update(
                        {PHASE6CQ_CANDIDATE_SELECTOR: "0"}
                    ),
                ),
            ),
            (
                "integer 6CQ selector",
                lambda value: value["provenance"]["selectors"].update(
                    {PHASE6CQ_CANDIDATE_SELECTOR: 1}
                ),
            ),
            (
                "6CQ receipt remap",
                lambda value: value["provenance"]["selector_receipts"][
                    PHASE6CQ_CANDIDATE_SELECTOR
                ].update({"field": "guest_memory_operation_snapshot_elision"}),
            ),
            (
                "6CQ marker token omission",
                lambda value: value["candidate_marker"][
                    "required_tokens"
                ].remove("live_mismatch=none"),
            ),
        ):
            rejected_g3_config = json.loads(json.dumps(g3_config))
            mutate(rejected_g3_config)
            rejected_g3_path = root / f"rejected-g3-{label.replace(' ', '-')}.json"
            _atomic_json(rejected_g3_path, rejected_g3_config)
            try:
                _validate_start_config(rejected_g3_path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted {label}")

        wrong_g3_lineage = json.loads(json.dumps(g3_config))
        wrong_g3_lineage["lineage"]["generation"] = 4
        try:
            _configuration_projection_policy_for(
                wrong_g3_lineage,
                "self-test wrong g3 lineage",
                test_mode=False,
                override=None,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted wrong 6CQ lineage")

        phase6cs_receipt_fields = {
            **projection_receipt_fields,
            "scalar_memory_fallthrough_admission": "0",
            "fast_block_dead_rflags_elision": "1",
        }
        phase6cs_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}"
            for name, value in phase6cs_receipt_fields.items()
        )
        phase6cs_marker = (
            "[FAST_BLOCK_DEAD_RFLAGS_ELISION_FINAL] "
            "blocks=7 operations=11 ordinary=positive "
            "status=clean coverage=positive"
        )
        phase6cs_config = json.loads(json.dumps(projection_config))
        phase6cs_config["protocol"] = _fixture_protocol(
            PHASE6CS_CONFIGURATION_PROJECTION_SCOPE["protocol_id"]
        )
        phase6cs_config["provenance"] = {
            "record_id": PHASE6CS_CONFIGURATION_PROJECTION_SCOPE["record_id"],
            "run_receipt": phase6cs_receipt,
            "candidate_selector": PHASE6CS_CANDIDATE_SELECTOR,
            "selectors": dict(
                PHASE6CS_CONFIGURATION_PROJECTION_SCOPE["selector_bundle"]
            ),
            "selector_receipts": {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in PHASE6CS_CONFIGURATION_PROJECTION_SCOPE[
                    "selector_bundle"
                ]
            },
        }
        phase6cs_config["lineage"] = {
            "generation": PHASE6CS_CONFIGURATION_PROJECTION_SCOPE[
                "generation"
            ],
            "parent_record_id": PHASE6CS_CONFIGURATION_PROJECTION_SCOPE[
                "parent_record_id"
            ],
            "original_role": PHASE6CS_CONFIGURATION_PROJECTION_SCOPE[
                "original_role"
            ],
        }
        phase6cs_config["candidate_marker"] = {
            "required": True,
            "prefix": CANDIDATE_MARKER_ALLOWLIST[PHASE6CS_CANDIDATE_SELECTOR][
                "prefix"
            ],
            "required_tokens": list(
                CANDIDATE_MARKER_ALLOWLIST[PHASE6CS_CANDIDATE_SELECTOR][
                    "required_tokens"
                ]
            ),
            "coverage_required": True,
            "coverage_token": "coverage=positive",
        }
        phase6cs_config["launch"]["common_environment"].update(
            {"TEST_RECEIPT": phase6cs_receipt, "TEST_MARKER": phase6cs_marker}
        )
        phase6cs_config["launch"]["experiment_environment"] = {
            name: _selector_env_text(value)
            for name, value in PHASE6CS_CONFIGURATION_PROJECTION_SCOPE[
                "selector_bundle"
            ].items()
        }
        phase6cs_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(phase6cs_config["launch"]["common_environment"])
        phase6cs_config_path = root / "phase6cs-start-validation.json"
        _atomic_json(phase6cs_config_path, phase6cs_config)
        validated_phase6cs_config = _validate_start_config(
            phase6cs_config_path
        )
        _require(
            PHASE6CQ_CANDIDATE_SELECTOR
            not in validated_phase6cs_config["provenance"]["selectors"]
            and PHASE6CQ_CANDIDATE_SELECTOR
            not in validated_phase6cs_config["launch"][
                "experiment_environment"
            ]
            and _validate_candidate_marker_lines(
                [phase6cs_marker],
                validated_phase6cs_config,
                "self-test phase-6CS marker",
            )
            == (1, True),
            "self-test",
            "phase-6CS exact bundle/default-OFF/marker contract failed",
        )
        _require(
            _configuration_projection_policy_for(
                validated_phase6cs_config,
                "self-test phase-6CS production policy",
                test_mode=False,
                override=None,
            )
            is PHASE6CS_CONFIGURATION_PROJECTION_POLICY,
            "self-test",
            "phase-6CS production policy did not resolve from its exact scope",
        )
        for label, marker_logs in (
            (
                "missing ordinary coverage",
                [phase6cs_marker.replace(" ordinary=positive", "")],
            ),
            ("zero blocks", [phase6cs_marker.replace("blocks=7", "blocks=0")]),
            ("extra token", [phase6cs_marker + " unexpected=1"]),
            ("duplicate", [phase6cs_marker, phase6cs_marker]),
        ):
            try:
                _validate_candidate_marker_lines(
                    marker_logs,
                    validated_phase6cs_config,
                    f"self-test phase-6CS marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid phase-6CS marker {label}"
                )

        for scalar_state in (False, True):
            mixed_phase6cs = json.loads(json.dumps(phase6cs_config))
            mixed_phase6cs["provenance"]["selectors"][
                PHASE6CQ_CANDIDATE_SELECTOR
            ] = scalar_state
            mixed_phase6cs["provenance"]["selector_receipts"][
                PHASE6CQ_CANDIDATE_SELECTOR
            ] = dict(SELECTOR_RECEIPT_ALLOWLIST[PHASE6CQ_CANDIDATE_SELECTOR])
            mixed_phase6cs["launch"]["experiment_environment"][
                PHASE6CQ_CANDIDATE_SELECTOR
            ] = _selector_env_text(scalar_state)
            mixed_phase6cs_path = (
                root / f"phase6cs-mixed-6cq-{scalar_state}.json"
            )
            _atomic_json(mixed_phase6cs_path, mixed_phase6cs)
            try:
                _validate_start_config(mixed_phase6cs_path)
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted mixed 6CQ+6CS bundle scalar={scalar_state}"
                )
            try:
                _configuration_projection_contract_for(
                    mixed_phase6cs,
                    f"self-test ambiguous 6CQ+6CS scalar={scalar_state}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test resolved ambiguous 6CQ+6CS signals scalar={scalar_state}"
                )

        bad_phase6cs_receipt = json.loads(json.dumps(phase6cs_config))
        bad_phase6cs_receipt["provenance"]["run_receipt"] = (
            phase6cs_receipt.replace(
                "scalar_memory_fallthrough_admission=0",
                "scalar_memory_fallthrough_admission=1",
            )
        )
        bad_phase6cs_receipt_path = root / "phase6cs-scalar-on-receipt.json"
        _atomic_json(bad_phase6cs_receipt_path, bad_phase6cs_receipt)
        try:
            _validate_start_config(bad_phase6cs_receipt_path)
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted phase-6CS with scalar fallthrough receipt ON"
            )

        reused_6cq_identity = json.loads(json.dumps(phase6cs_config))
        reused_6cq_identity["provenance"]["record_id"] = (
            PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE["record_id"]
        )
        try:
            _configuration_projection_policy_for(
                reused_6cq_identity,
                "self-test reused 6CQ identity for phase-6CS",
                test_mode=False,
                override=None,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted the consumed 6CQ record ID for phase-6CS"
            )

        phase6cs_fixture_contract_payload = _phase6cs_restore_contract_payload(
            g3_fixture_operations, "self-test synthetic phase-6CS contract"
        )
        phase6cs_fixture_projection_policy = {
            **fixture_projection_policy,
            **PHASE6CS_CONFIGURATION_PROJECTION_SCOPE,
            "projection_configured": True,
            "raw_bytes": len(g3_fixture_configuration_raw),
            "raw_sha256": _sha_bytes(g3_fixture_configuration_raw),
            "restore_operations": g3_fixture_operations,
            "restore_operation_count": len(g3_fixture_operations),
            "restore_operations_contract_bytes": len(
                phase6cs_fixture_contract_payload
            ),
            "restore_operations_contract_sha256": _sha_bytes(
                phase6cs_fixture_contract_payload
            ),
            "restored_bytes": len(g3_fixture_restored_raw),
            "restored_sha256": _sha_bytes(g3_fixture_restored_raw),
        }
        phase6cs_projection_receipt = _configuration_projection_preflight(
            validated_phase6cs_config,
            g3_fixture_configuration_raw,
            "self-test synthetic phase-6CS projection",
            test_mode=True,
            policy_override=phase6cs_fixture_projection_policy,
        )
        _require(
            phase6cs_projection_receipt is not None
            and set(phase6cs_projection_receipt)
            == PHASE6CS_CONFIGURATION_PROJECTION_RECEIPT_KEYS
            and phase6cs_projection_receipt["record_id"]
            == PHASE6CS_CONFIGURATION_PROJECTION_SCOPE["record_id"]
            and phase6cs_projection_receipt["candidate_selector"]
            == PHASE6CS_CANDIDATE_SELECTOR
            and phase6cs_projection_receipt["attestation"]
            == PHASE6CS_CONFIGURATION_PROJECTION_ATTESTATION
            and phase6cs_projection_receipt["policy"]
            == PHASE6CS_CONFIGURATION_PROJECTION_POLICY_ID,
            "self-test",
            "synthetic phase-6CS projection receipt identity mismatch",
        )
        wrong_phase6cs_restore_digest = {
            **phase6cs_fixture_projection_policy,
            "restore_operations_contract_sha256": "0" * 64,
        }
        try:
            _phase6cs_configuration_projection_receipt(
                validated_phase6cs_config,
                g3_fixture_configuration_raw,
                "self-test phase-6CS wrong restore digest",
                wrong_phase6cs_restore_digest,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted phase-6CS restore contract tampering"
            )

        wrong_phase6cs_lineage = json.loads(json.dumps(phase6cs_config))
        wrong_phase6cs_lineage["lineage"]["parent_record_id"] = "wrong-parent"
        try:
            _configuration_projection_policy_for(
                wrong_phase6cs_lineage,
                "self-test wrong phase-6CS parent",
                test_mode=False,
                override=None,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted wrong phase-6CS parent")

        phase6da_receipt_fields = {
            **projection_receipt_fields,
            "scalar_memory_fallthrough_admission": "0",
            "fast_block_dead_rflags_elision": "0",
            "fast_block_dense_directory_requested": "1",
            "fast_block_dense_directory": "1",
        }
        phase6da_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}"
            for name, value in phase6da_receipt_fields.items()
        )
        phase6da_marker = (
            "[A64_FAST_BLOCK_DENSE_DIRECTORY_FINAL] "
            "lookup=positive hit=positive mismatch=none "
            "status=clean coverage=positive"
        )
        phase6da_config = json.loads(json.dumps(projection_config))
        phase6da_config["protocol"] = _fixture_protocol(
            PHASE6DA_CONFIGURATION_PROJECTION_SCOPE["protocol_id"]
        )
        phase6da_config["provenance"] = {
            "record_id": PHASE6DA_CONFIGURATION_PROJECTION_SCOPE["record_id"],
            "run_receipt": phase6da_receipt,
            "candidate_selector": PHASE6DA_CANDIDATE_SELECTOR,
            "selectors": dict(
                PHASE6DA_CONFIGURATION_PROJECTION_SCOPE["selector_bundle"]
            ),
            "selector_receipts": {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in PHASE6DA_CONFIGURATION_PROJECTION_SCOPE[
                    "selector_bundle"
                ]
            },
        }
        phase6da_config["lineage"] = {
            "generation": PHASE6DA_CONFIGURATION_PROJECTION_SCOPE[
                "generation"
            ],
            "parent_record_id": PHASE6DA_CONFIGURATION_PROJECTION_SCOPE[
                "parent_record_id"
            ],
            "original_role": PHASE6DA_CONFIGURATION_PROJECTION_SCOPE[
                "original_role"
            ],
        }
        phase6da_config["candidate_marker"] = {
            "required": True,
            "prefix": CANDIDATE_MARKER_ALLOWLIST[PHASE6DA_CANDIDATE_SELECTOR][
                "prefix"
            ],
            "required_tokens": list(
                CANDIDATE_MARKER_ALLOWLIST[PHASE6DA_CANDIDATE_SELECTOR][
                    "required_tokens"
                ]
            ),
            "coverage_required": True,
            "coverage_token": "coverage=positive",
        }
        phase6da_config["launch"]["common_environment"].update(
            {"TEST_RECEIPT": phase6da_receipt, "TEST_MARKER": phase6da_marker}
        )
        phase6da_config["launch"]["experiment_environment"] = {
            name: _selector_env_text(value)
            for name, value in PHASE6DA_CONFIGURATION_PROJECTION_SCOPE[
                "selector_bundle"
            ].items()
        }
        phase6da_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(phase6da_config["launch"]["common_environment"])
        phase6da_config_path = root / "phase6da-start-validation.json"
        _atomic_json(phase6da_config_path, phase6da_config)
        validated_phase6da_config = _validate_start_config(
            phase6da_config_path
        )
        _require(
            PHASE6CQ_CANDIDATE_SELECTOR
            not in validated_phase6da_config["provenance"]["selectors"]
            and PHASE6CS_CANDIDATE_SELECTOR
            not in validated_phase6da_config["provenance"]["selectors"]
            and _validate_candidate_marker_lines(
                [phase6da_marker],
                validated_phase6da_config,
                "self-test phase-6DA marker",
            )
            == (1, True)
            and _configuration_projection_policy_for(
                validated_phase6da_config,
                "self-test phase-6DA production policy",
                test_mode=False,
                override=None,
            )
            is PHASE6DA_CONFIGURATION_PROJECTION_POLICY,
            "self-test",
            "phase-6DA exact bundle/marker/projection contract failed",
        )

        for label, marker_logs in (
            (
                "missing positive hit",
                [phase6da_marker.replace(" hit=positive", "")],
            ),
            (
                "observed mismatch",
                [phase6da_marker.replace("mismatch=none", "mismatch=observed")],
            ),
            (
                "reordered fields",
                [
                    phase6da_marker.replace(
                        "lookup=positive hit=positive",
                        "hit=positive lookup=positive",
                    )
                ],
            ),
            ("extra token", [phase6da_marker + " unexpected=1"]),
            ("duplicate", [phase6da_marker, phase6da_marker]),
        ):
            try:
                _validate_candidate_marker_lines(
                    marker_logs,
                    validated_phase6da_config,
                    f"self-test phase-6DA marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid phase-6DA marker {label}"
                )

        for retired_selector in (
            PHASE6CQ_CANDIDATE_SELECTOR,
            PHASE6CS_CANDIDATE_SELECTOR,
        ):
            for retired_state in (False, True):
                mixed_phase6da = json.loads(json.dumps(phase6da_config))
                mixed_phase6da["provenance"]["selectors"][
                    retired_selector
                ] = retired_state
                mixed_phase6da["provenance"]["selector_receipts"][
                    retired_selector
                ] = dict(SELECTOR_RECEIPT_ALLOWLIST[retired_selector])
                mixed_phase6da["launch"]["experiment_environment"][
                    retired_selector
                ] = _selector_env_text(retired_state)
                mixed_phase6da_path = root / (
                    "phase6da-mixed-"
                    f"{retired_selector.lower()}-{retired_state}.json"
                )
                _atomic_json(mixed_phase6da_path, mixed_phase6da)
                try:
                    _validate_start_config(mixed_phase6da_path)
                except HarnessError:
                    pass
                else:
                    raise HarnessError(
                        "self-test accepted phase-6DA mixed with retired "
                        f"selector {retired_selector}={retired_state}"
                    )
                try:
                    _configuration_projection_contract_for(
                        mixed_phase6da,
                        "self-test ambiguous phase-6DA selector bundle",
                    )
                except HarnessError:
                    pass
                else:
                    raise HarnessError(
                        "self-test resolved phase-6DA mixed with retired "
                        f"selector {retired_selector}={retired_state}"
                    )

        phase6da_config_negative_cases = []
        for field, replacement in (
            ("scalar_memory_fallthrough_admission", "1"),
            ("fast_block_dead_rflags_elision", "1"),
            ("fast_block_dense_directory_requested", "0"),
            ("fast_block_dense_directory", "0"),
        ):
            value = json.loads(json.dumps(phase6da_config))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}={phase6da_receipt_fields[field]}", f"{field}={replacement}")
            phase6da_config_negative_cases.append((f"receipt-{field}", value))
        for label, replacement in (("false", False), ("integer", 1)):
            value = json.loads(json.dumps(phase6da_config))
            value["provenance"]["selectors"][PHASE6DA_CANDIDATE_SELECTOR] = (
                replacement
            )
            phase6da_config_negative_cases.append(
                (f"candidate-selector-{label}", value)
            )
        missing_phase6da_selector = json.loads(json.dumps(phase6da_config))
        for mapping in (
            missing_phase6da_selector["provenance"]["selectors"],
            missing_phase6da_selector["provenance"]["selector_receipts"],
            missing_phase6da_selector["launch"]["experiment_environment"],
        ):
            del mapping[PHASE6DA_CANDIDATE_SELECTOR]
        phase6da_config_negative_cases.append(
            ("missing-candidate-selector", missing_phase6da_selector)
        )
        remapped_phase6da_receipt = json.loads(json.dumps(phase6da_config))
        remapped_phase6da_receipt["provenance"]["selector_receipts"][
            PHASE6DA_CANDIDATE_SELECTOR
        ]["field"] = "fast_block_dense_directory_requested"
        phase6da_config_negative_cases.append(
            ("selector-receipt-remap", remapped_phase6da_receipt)
        )
        for label, value in phase6da_config_negative_cases:
            path = root / f"rejected-phase6da-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted phase-6DA {label}")

        for label, field, replacement in (
            ("generation-g000004", "generation", 4),
            ("wrong-parent", "parent_record_id", "wrong-parent"),
        ):
            value = json.loads(json.dumps(phase6da_config))
            value["lineage"][field] = replacement
            try:
                _configuration_projection_policy_for(
                    value,
                    f"self-test phase-6DA {label}",
                    test_mode=False,
                    override=None,
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted phase-6DA {label}")
        for reused_record in (
            PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE["record_id"],
            PHASE6CS_CONFIGURATION_PROJECTION_SCOPE["record_id"],
        ):
            value = json.loads(json.dumps(phase6da_config))
            value["provenance"]["record_id"] = reused_record
            try:
                _configuration_projection_policy_for(
                    value,
                    "self-test phase-6DA reused sibling record",
                    test_mode=False,
                    override=None,
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted phase-6DA reused record {reused_record}"
                )

        phase6da_fixture_contract_payload = _phase6da_restore_contract_payload(
            g3_fixture_operations, "self-test synthetic phase-6DA contract"
        )
        phase6da_fixture_projection_policy = {
            **PHASE6DA_CONFIGURATION_PROJECTION_POLICY,
            "raw_bytes": len(g3_fixture_configuration_raw),
            "raw_sha256": _sha_bytes(g3_fixture_configuration_raw),
            "restore_operations": g3_fixture_operations,
            "restore_operation_count": len(g3_fixture_operations),
            "restore_operations_contract_bytes": len(
                phase6da_fixture_contract_payload
            ),
            "restore_operations_contract_sha256": _sha_bytes(
                phase6da_fixture_contract_payload
            ),
            "restored_bytes": len(g3_fixture_restored_raw),
            "restored_sha256": _sha_bytes(g3_fixture_restored_raw),
            "projected_bytes": len(projected_configuration),
            "projected_sha256": _sha_bytes(projected_configuration),
        }
        phase6da_projection_receipt = _configuration_projection_preflight(
            validated_phase6da_config,
            g3_fixture_configuration_raw,
            "self-test synthetic phase-6DA projection",
            test_mode=True,
            policy_override=phase6da_fixture_projection_policy,
        )
        _require(
            phase6da_projection_receipt is not None
            and set(phase6da_projection_receipt)
            == PHASE6DA_CONFIGURATION_PROJECTION_RECEIPT_KEYS
            and phase6da_projection_receipt["record_id"]
            == "6da-c-fast-block-dense-directory-g000003"
            and phase6da_projection_receipt["candidate_selector"]
            == PHASE6DA_CANDIDATE_SELECTOR
            and phase6da_projection_receipt["attestation"]
            == PHASE6DA_CONFIGURATION_PROJECTION_ATTESTATION
            and phase6da_projection_receipt["policy"]
            == PHASE6DA_CONFIGURATION_PROJECTION_POLICY_ID,
            "self-test",
            "synthetic phase-6DA projection receipt identity mismatch",
        )
        wrong_phase6da_restore_digest = {
            **phase6da_fixture_projection_policy,
            "restore_operations_contract_sha256": "0" * 64,
        }
        try:
            _phase6da_configuration_projection_receipt(
                validated_phase6da_config,
                g3_fixture_configuration_raw,
                "self-test phase-6DA wrong restore digest",
                wrong_phase6da_restore_digest,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted phase-6DA restore contract tampering"
            )

        def phase6da_initial_config(candidate: bool) -> Mapping[str, Any]:
            value = json.loads(json.dumps(phase6da_config))
            scope = PHASE6DA_INITIAL_DIRECT_SCOPES[1 if candidate else 0]
            value["protocol"] = _fixture_protocol(PHASE6DA_INITIAL_PROTOCOL_ID)
            value["comparability"]["os"] = {
                "name": "macOS",
                "version": PHASE6DA_INITIAL_OS_VERSION,
                "build": PHASE6DA_INITIAL_OS_BUILD,
                "policy": "fixed-version-and-build-within-epoch",
            }
            value["comparability"]["runtime"]["configuration_sha256"] = (
                PHASE6DA_INITIAL_DIRECT_CONFIGURATION_SHA256
            )
            value["provenance"]["record_id"] = scope["record_id"]
            value["provenance"]["selectors"] = dict(scope["selector_bundle"])
            value["provenance"]["selector_receipts"] = {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in scope["selector_bundle"]
            }
            expected = "1" if candidate else "0"
            value["provenance"]["run_receipt"] = phase6da_receipt.replace(
                "fast_block_dense_directory_requested=1",
                f"fast_block_dense_directory_requested={expected}",
            ).replace(
                "fast_block_dense_directory=1",
                f"fast_block_dense_directory={expected}",
            )
            value["lineage"] = {
                "generation": scope["generation"],
                "parent_record_id": scope["parent_record_id"],
                "original_role": scope["original_role"],
            }
            value["candidate_marker"] = {
                "required": candidate,
                "prefix": CANDIDATE_MARKER_ALLOWLIST[
                    PHASE6DA_CANDIDATE_SELECTOR
                ]["prefix"],
                "required_tokens": (
                    list(
                        CANDIDATE_MARKER_ALLOWLIST[
                            PHASE6DA_CANDIDATE_SELECTOR
                        ]["required_tokens"]
                    )
                    if candidate
                    else []
                ),
                "coverage_required": candidate,
                "coverage_token": "coverage=positive" if candidate else None,
            }
            value["launch"]["experiment_environment"] = {
                name: _selector_env_text(selected)
                for name, selected in scope["selector_bundle"].items()
            }
            value["launch"]["common_environment"]["TEST_RECEIPT"] = value[
                "provenance"
            ]["run_receipt"]
            value["comparability"]["runtime"][
                "sterile_environment_sha256"
            ] = _canonical_sha(value["launch"]["common_environment"])
            return value

        phase6da_initial_b = phase6da_initial_config(False)
        phase6da_initial_c = phase6da_initial_config(True)
        validated_initial_pair = []
        for label, value, marker_logs in (
            ("b", phase6da_initial_b, []),
            ("c", phase6da_initial_c, [phase6da_marker]),
        ):
            path = root / f"phase6da-initial-{label}.json"
            _atomic_json(path, value)
            validated = _validate_start_config(path)
            validated_initial_pair.append(validated)
            expected_scope = PHASE6DA_INITIAL_DIRECT_SCOPES[
                1 if label == "c" else 0
            ]
            _require(
                _phase6da_initial_direct_scope_for(
                    validated, f"self-test phase-6DA initial {label} scope"
                )
                is expected_scope
                and _configuration_projection_policy_for(
                    validated,
                    f"self-test phase-6DA initial {label} direct policy",
                    test_mode=False,
                    override=None,
                )
                is None
                and _validate_candidate_marker_lines(
                    marker_logs,
                    validated,
                    f"self-test phase-6DA initial {label} marker",
                )
                == ((1, True) if label == "c" else (0, False)),
                "self-test",
                f"phase-6DA initial {label} direct contract failed",
            )

        initial_negative_cases = []
        for label, source_value, mutate in (
            (
                "b-selector-on",
                phase6da_initial_b,
                lambda value: value["provenance"]["selectors"].update(
                    {PHASE6DA_CANDIDATE_SELECTOR: True}
                ),
            ),
            (
                "c-selector-off",
                phase6da_initial_c,
                lambda value: value["provenance"]["selectors"].update(
                    {PHASE6DA_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "b-wrong-generation",
                phase6da_initial_b,
                lambda value: value["lineage"].update({"generation": 1}),
            ),
            (
                "c-wrong-parent",
                phase6da_initial_c,
                lambda value: value["lineage"].update(
                    {"parent_record_id": "wrong-parent"}
                ),
            ),
            (
                "c-wrong-record",
                phase6da_initial_c,
                lambda value: value["provenance"].update(
                    {"record_id": "wrong-record"}
                ),
            ),
            (
                "c-wrong-os-build",
                phase6da_initial_c,
                lambda value: value["comparability"]["os"].update(
                    {"build": "wrong-build"}
                ),
            ),
            (
                "c-wrong-direct-configuration",
                phase6da_initial_c,
                lambda value: value["comparability"]["runtime"].update(
                    {"configuration_sha256": "0" * 64}
                ),
            ),
        ):
            value = json.loads(json.dumps(source_value))
            mutate(value)
            initial_negative_cases.append((label, value))
        for label, value in initial_negative_cases:
            path = root / f"rejected-phase6da-initial-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted phase-6DA initial {label}"
                )

        for label, source_value, field, unexpected in (
            (
                "b-requested",
                phase6da_initial_b,
                "fast_block_dense_directory_requested",
                "1",
            ),
            (
                "b-effective",
                phase6da_initial_b,
                "fast_block_dense_directory",
                "1",
            ),
            (
                "c-requested",
                phase6da_initial_c,
                "fast_block_dense_directory_requested",
                "0",
            ),
            (
                "c-effective",
                phase6da_initial_c,
                "fast_block_dense_directory",
                "0",
            ),
        ):
            value = json.loads(json.dumps(source_value))
            receipt_fields = _receipt_fields(
                value["provenance"]["run_receipt"],
                A64_CONFIGURATION_PREFIX,
                "self-test phase-6DA initial receipt",
            )
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(
                f"{field}={receipt_fields[field]}", f"{field}={unexpected}"
            )
            path = root / f"rejected-phase6da-initial-receipt-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted phase-6DA initial receipt {label}"
                )

        try:
            _configuration_projection_preflight(
                validated_initial_pair[0],
                b"not-the-preregistered-direct-configuration",
                "self-test phase-6DA initial wrong direct bytes",
                test_mode=True,
                policy_override=None,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted phase-6DA initial wrong direct configuration bytes"
            )

        ifsm_receipt_fields = {
            **phase6da_receipt_fields,
            "fast_block_inline_fixed_signature_match_requested": "1",
            "fast_block_inline_fixed_signature_match": "1",
        }
        ifsm_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}" for name, value in ifsm_receipt_fields.items()
        )
        ifsm_marker = (
            "[A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH_FINAL] "
            "requested=positive effective=positive compared=positive "
            "status=clean coverage=positive"
        )
        ifsm_config = json.loads(json.dumps(phase6da_initial_c))
        ifsm_config["provenance"] = {
            "record_id": IFSM_DIRECT_SCOPE["record_id"],
            "run_receipt": ifsm_receipt,
            "candidate_selector": IFSM_CANDIDATE_SELECTOR,
            "selectors": dict(IFSM_DIRECT_SCOPE["selector_bundle"]),
            "selector_receipts": {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in IFSM_DIRECT_SCOPE["selector_bundle"]
            },
        }
        ifsm_config["lineage"] = {
            "generation": IFSM_DIRECT_SCOPE["generation"],
            "parent_record_id": IFSM_DIRECT_SCOPE["parent_record_id"],
            "original_role": IFSM_DIRECT_SCOPE["original_role"],
        }
        ifsm_config["candidate_marker"] = {
            "required": True,
            "prefix": CANDIDATE_MARKER_ALLOWLIST[IFSM_CANDIDATE_SELECTOR][
                "prefix"
            ],
            "required_tokens": list(
                CANDIDATE_MARKER_ALLOWLIST[IFSM_CANDIDATE_SELECTOR][
                    "required_tokens"
                ]
            ),
            "coverage_required": True,
            "coverage_token": "coverage=positive",
        }
        ifsm_config["launch"]["experiment_environment"] = {
            name: _selector_env_text(selected)
            for name, selected in IFSM_DIRECT_SCOPE["selector_bundle"].items()
        }
        ifsm_config["launch"]["common_environment"].update(
            {"TEST_RECEIPT": ifsm_receipt, "TEST_MARKER": ifsm_marker}
        )
        ifsm_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(ifsm_config["launch"]["common_environment"])
        ifsm_config_path = root / "ifsm-direct-start-validation.json"
        _atomic_json(ifsm_config_path, ifsm_config)
        validated_ifsm_config = _validate_start_config(ifsm_config_path)
        ifsm_effective, _ = _effective_selectors_from_logs(
            [ifsm_receipt], validated_ifsm_config, "self-test IFSM receipts"
        )
        _require(
            _ifsm_direct_scope_for(
                validated_ifsm_config, "self-test IFSM direct scope"
            )
            is IFSM_DIRECT_SCOPE
            and _configuration_projection_policy_for(
                validated_ifsm_config,
                "self-test IFSM direct policy",
                test_mode=False,
                override=None,
            )
            is None
            and ifsm_effective == IFSM_DIRECT_SCOPE["selector_bundle"]
            and _candidate_required_receipt_fields(
                validated_ifsm_config, "self-test IFSM required receipts"
            )
            == CANDIDATE_REQUIRED_RECEIPT_FIELDS[IFSM_CANDIDATE_SELECTOR]
            and _validate_candidate_marker_lines(
                [ifsm_marker], validated_ifsm_config, "self-test IFSM marker"
            )
            == (1, True),
            "self-test",
            "IFSM generation-2 direct selector/receipt/marker contract failed",
        )

        for label, marker_logs in (
            (
                "missing compared coverage",
                [ifsm_marker.replace(" compared=positive", "")],
            ),
            (
                "ineffective",
                [
                    ifsm_marker.replace(
                        "effective=positive", "effective=zero"
                    )
                ],
            ),
            (
                "reordered",
                [
                    ifsm_marker.replace(
                        "requested=positive effective=positive",
                        "effective=positive requested=positive",
                    )
                ],
            ),
            ("invalid status", [ifsm_marker.replace("status=clean", "status=invalid")]),
            (
                "zero coverage",
                [ifsm_marker.replace("coverage=positive", "coverage=zero")],
            ),
            ("extra token", [ifsm_marker + " unexpected=1"]),
            ("duplicate", [ifsm_marker, ifsm_marker]),
        ):
            try:
                _validate_candidate_marker_lines(
                    marker_logs,
                    validated_ifsm_config,
                    f"self-test IFSM marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid IFSM marker {label}"
                )

        ifsm_negative_cases = []
        for label, mutate in (
            (
                "wrong-record",
                lambda value: value["provenance"].update(
                    {"record_id": "wrong-record"}
                ),
            ),
            (
                "wrong-generation",
                lambda value: value["lineage"].update({"generation": 1}),
            ),
            (
                "wrong-parent",
                lambda value: value["lineage"].update(
                    {"parent_record_id": "wrong-parent"}
                ),
            ),
            (
                "wrong-os-build",
                lambda value: value["comparability"]["os"].update(
                    {"build": "wrong-build"}
                ),
            ),
            (
                "wrong-configuration",
                lambda value: value["comparability"]["runtime"].update(
                    {"configuration_sha256": "0" * 64}
                ),
            ),
            (
                "selector-off",
                lambda value: value["provenance"]["selectors"].update(
                    {IFSM_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "selector-integer",
                lambda value: value["provenance"]["selectors"].update(
                    {IFSM_CANDIDATE_SELECTOR: 1}
                ),
            ),
            (
                "selector-string",
                lambda value: value["provenance"]["selectors"].update(
                    {IFSM_CANDIDATE_SELECTOR: "1"}
                ),
            ),
            (
                "retained-6da-off",
                lambda value: value["provenance"]["selectors"].update(
                    {PHASE6DA_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "malformed-environment",
                lambda value: value["launch"]["experiment_environment"].update(
                    {IFSM_CANDIDATE_SELECTOR: "01"}
                ),
            ),
            (
                "selector-receipt-remap",
                lambda value: value["provenance"]["selector_receipts"][
                    IFSM_CANDIDATE_SELECTOR
                ].update(
                    {"field": "fast_block_inline_fixed_signature_match_requested"}
                ),
            ),
        ):
            value = json.loads(json.dumps(ifsm_config))
            mutate(value)
            ifsm_negative_cases.append((label, value))

        missing_ifsm_selector = json.loads(json.dumps(ifsm_config))
        for mapping in (
            missing_ifsm_selector["provenance"]["selectors"],
            missing_ifsm_selector["provenance"]["selector_receipts"],
            missing_ifsm_selector["launch"]["experiment_environment"],
        ):
            del mapping[IFSM_CANDIDATE_SELECTOR]
        ifsm_negative_cases.append(
            ("missing-candidate-selector", missing_ifsm_selector)
        )
        mixed_ifsm = json.loads(json.dumps(ifsm_config))
        mixed_ifsm["provenance"]["selectors"][PHASE6CS_CANDIDATE_SELECTOR] = (
            False
        )
        mixed_ifsm["provenance"]["selector_receipts"][
            PHASE6CS_CANDIDATE_SELECTOR
        ] = dict(SELECTOR_RECEIPT_ALLOWLIST[PHASE6CS_CANDIDATE_SELECTOR])
        mixed_ifsm["launch"]["experiment_environment"][
            PHASE6CS_CANDIDATE_SELECTOR
        ] = "0"
        ifsm_negative_cases.append(("mixed-retired-sibling", mixed_ifsm))

        for field, replacement in (
            ("stable_negative_route", "0"),
            ("guest_memory_operation_snapshot_elision", "0"),
            ("scalar_memory_fallthrough_admission", "1"),
            ("fast_block_dead_rflags_elision", "1"),
            ("fast_block_dense_directory_requested", "0"),
            ("fast_block_dense_directory", "0"),
            ("fast_block_inline_fixed_signature_match_requested", "0"),
            ("fast_block_inline_fixed_signature_match", "0"),
        ):
            value = json.loads(json.dumps(ifsm_config))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}={ifsm_receipt_fields[field]}", f"{field}={replacement}")
            ifsm_negative_cases.append((f"receipt-{field}", value))

        for label, value in ifsm_negative_cases:
            path = root / f"rejected-ifsm-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted IFSM {label}")

        try:
            _configuration_projection_preflight(
                validated_ifsm_config,
                b"not-the-preregistered-direct-configuration",
                "self-test IFSM wrong direct bytes",
                test_mode=True,
                policy_override=None,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted IFSM wrong direct configuration bytes"
            )

        alc_receipt_fields = {
            **ifsm_receipt_fields,
            "fast_block_unrolled_fixed_signature_batching_requested": "0",
            "fast_block_unrolled_fixed_signature_batching": "0",
            "fast_block_idempotent_mru_store_elision_requested": "0",
            "fast_block_idempotent_mru_store_elision": "0",
            "fast_block_callback_free_ordinary_specialization_requested": "0",
            "fast_block_callback_free_ordinary_specialization": "0",
            "arm_loading_composite_requested": "1",
            "arm_loading_composite": "1",
        }
        alc_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}" for name, value in alc_receipt_fields.items()
        )
        alc_contract = CANDIDATE_MARKER_ALLOWLIST[ALC_CANDIDATE_SELECTOR]
        alc_marker = " ".join(
            [alc_contract["prefix"], *alc_contract["required_tokens"]]
        )
        alc_config = json.loads(json.dumps(ifsm_config))
        alc_config["provenance"] = {
            "record_id": ALC_DIRECT_RECORD_ID,
            "run_receipt": alc_receipt,
            "candidate_selector": ALC_CANDIDATE_SELECTOR,
            "selectors": dict(
                ALC_CONFIGURATION_PROJECTION_SCOPE["selector_bundle"]
            ),
            "selector_receipts": {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in ALC_CONFIGURATION_PROJECTION_SCOPE[
                    "selector_bundle"
                ]
            },
        }
        alc_config["lineage"] = {
            "generation": ALC_CONFIGURATION_PROJECTION_SCOPE["generation"],
            "parent_record_id": ALC_CONFIGURATION_PROJECTION_SCOPE[
                "parent_record_id"
            ],
            "original_role": ALC_CONFIGURATION_PROJECTION_SCOPE[
                "original_role"
            ],
        }
        alc_config["candidate_marker"] = {
            "required": True,
            "prefix": alc_contract["prefix"],
            "required_tokens": list(alc_contract["required_tokens"]),
            "coverage_required": True,
            "coverage_token": alc_contract["coverage_token"],
        }
        alc_config["launch"]["experiment_environment"] = {
            name: _selector_env_text(selected)
            for name, selected in ALC_CONFIGURATION_PROJECTION_SCOPE[
                "selector_bundle"
            ].items()
        }
        alc_config["launch"]["common_environment"].update(
            {"TEST_RECEIPT": alc_receipt, "TEST_MARKER": alc_marker}
        )
        alc_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(alc_config["launch"]["common_environment"])
        alc_config_path = root / "alc-projection-start-validation.json"
        _atomic_json(alc_config_path, alc_config)
        validated_alc_config = _validate_start_config(alc_config_path)
        alc_effective, _ = _effective_selectors_from_logs(
            [alc_receipt], validated_alc_config, "self-test ALC receipts"
        )
        _require(
            _alc_projection_scope_for(
                validated_alc_config, "self-test ALC projection scope"
            )
            is ALC_CONFIGURATION_PROJECTION_SCOPE
            and _ifsm_direct_scope_for(
                validated_alc_config, "self-test ALC IFSM parent scope"
            )
            is None
            and _configuration_projection_policy_for(
                validated_alc_config,
                "self-test ALC projection policy",
                test_mode=False,
                override=None,
            )
            is ALC_CONFIGURATION_PROJECTION_POLICY
            and alc_effective
            == ALC_CONFIGURATION_PROJECTION_SCOPE["selector_bundle"]
            and _candidate_required_receipt_fields(
                validated_alc_config, "self-test ALC required receipts"
            )
            == CANDIDATE_REQUIRED_RECEIPT_FIELDS[ALC_CANDIDATE_SELECTOR]
            and _validate_candidate_marker_lines(
                [alc_marker], validated_alc_config, "self-test ALC marker"
            )
            == (1, True),
            "self-test",
            "ALC generation-3 selector/receipt/marker/projection contract failed",
        )

        for label, marker_logs in (
            ("missing", []),
            ("duplicate", [alc_marker, alc_marker]),
            (
                "missing-ifsm-rollback",
                [alc_marker.replace(" ifsm_rollback=positive", "")],
            ),
            (
                "reordered",
                [
                    alc_marker.replace(
                        "purge=positive adaptive_snapshot=positive",
                        "adaptive_snapshot=positive purge=positive",
                    )
                ],
            ),
            (
                "invalid-status",
                [alc_marker.replace("status=clean", "status=invalid")],
            ),
            ("extra-token", [alc_marker + " unexpected=1"]),
        ):
            try:
                _validate_candidate_marker_lines(
                    marker_logs,
                    validated_alc_config,
                    f"self-test ALC marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid ALC marker {label}"
                )

        alc_negative_cases = []
        for label, mutate in (
            (
                "wrong-record",
                lambda value: value["provenance"].update(
                    {"record_id": "wrong-record"}
                ),
            ),
            (
                "wrong-generation",
                lambda value: value["lineage"].update({"generation": 2}),
            ),
            (
                "wrong-parent",
                lambda value: value["lineage"].update(
                    {"parent_record_id": "wrong-parent"}
                ),
            ),
            (
                "selector-off",
                lambda value: value["provenance"]["selectors"].update(
                    {ALC_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "retained-ifsm-off",
                lambda value: value["provenance"]["selectors"].update(
                    {IFSM_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "selector-receipt-remap",
                lambda value: value["provenance"]["selector_receipts"][
                    ALC_CANDIDATE_SELECTOR
                ].update({"field": "arm_loading_composite_requested"}),
            ),
        ):
            value = json.loads(json.dumps(alc_config))
            mutate(value)
            alc_negative_cases.append((label, value))
        for sibling in (
            UFSB_CANDIDATE_SELECTOR,
            IMSE_COMPANION_SELECTOR,
            FCFOS_CANDIDATE_SELECTOR,
        ):
            value = json.loads(json.dumps(alc_config))
            value["provenance"]["selectors"][sibling] = False
            value["provenance"]["selector_receipts"][sibling] = dict(
                SELECTOR_RECEIPT_ALLOWLIST[sibling]
            )
            value["launch"]["experiment_environment"][sibling] = "0"
            alc_negative_cases.append((f"mixed-sibling-{sibling}", value))
        for field, expected in CANDIDATE_REQUIRED_RECEIPT_FIELDS[
            ALC_CANDIDATE_SELECTOR
        ].items():
            value = json.loads(json.dumps(alc_config))
            replacement = "0" if expected == "1" else "1"
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}={expected}", f"{field}={replacement}")
            alc_negative_cases.append((f"receipt-{field}", value))
        for label, value in alc_negative_cases:
            path = root / f"rejected-alc-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted ALC {label}")

        alc_fixture_raw = (
            b"alc-fixture-prefix\n"
            + b"".join(
                block for _, block in ALC_CONFIGURATION_STRIP_OPERATIONS
            )
            + b"alc-fixture-suffix\n"
        )
        alc_fixture_projected = (
            b"alc-fixture-prefix\n" + b"alc-fixture-suffix\n"
        )
        alc_projection_config = json.loads(json.dumps(validated_alc_config))
        alc_projection_config["comparability"]["runtime"][
            "configuration_sha256"
        ] = _sha_bytes(alc_fixture_projected)
        alc_fixture_projection_policy = {
            **ALC_CONFIGURATION_PROJECTION_POLICY,
            "raw_bytes": len(alc_fixture_raw),
            "raw_sha256": _sha_bytes(alc_fixture_raw),
            "projected_bytes": len(alc_fixture_projected),
            "projected_sha256": _sha_bytes(alc_fixture_projected),
        }
        alc_projection_receipt = _configuration_projection_preflight(
            alc_projection_config,
            alc_fixture_raw,
            "self-test synthetic ALC projection",
            test_mode=True,
            policy_override=alc_fixture_projection_policy,
        )
        _require(
            alc_projection_receipt is not None
            and set(alc_projection_receipt)
            == ALC_CONFIGURATION_PROJECTION_RECEIPT_KEYS
            and alc_projection_receipt["raw_sha256"]
            == _sha_bytes(alc_fixture_raw)
            and alc_projection_receipt["projected_sha256"]
            == _sha_bytes(alc_fixture_projected)
            and alc_projection_receipt["strip_operation_count"] == 3,
            "self-test",
            "synthetic ALC projection receipt mismatch",
        )

        alc_projection_negative_cases = []
        for label, bad_raw in (
            (
                "missing-block",
                alc_fixture_raw.replace(
                    ALC_ARM_LOADING_COMPOSITE_CACHE_BLOCK, b"", 1
                ),
            ),
            (
                "duplicate-block",
                alc_fixture_raw + ALC_ARM_HOTPATH_COMPOSITE_CACHE_BLOCK,
            ),
            (
                "reordered-blocks",
                b"alc-fixture-prefix\n"
                + ALC_ARM_LOADING_COMPOSITE_CACHE_BLOCK
                + ALC_ARM_HOTPATH_COMPOSITE_CACHE_BLOCK
                + ALC_REJECTED_FASTBLOCK_PURGE_CACHE_BLOCK
                + b"alc-fixture-suffix\n",
            ),
        ):
            bad_policy = {
                **alc_fixture_projection_policy,
                "raw_bytes": len(bad_raw),
                "raw_sha256": _sha_bytes(bad_raw),
            }
            alc_projection_negative_cases.append((label, bad_raw, bad_policy))
        alc_projection_negative_cases.append(
            (
                "contract-digest",
                alc_fixture_raw,
                {
                    **alc_fixture_projection_policy,
                    "strip_operations_contract_sha256": "0" * 64,
                },
            )
        )
        for label, bad_raw, bad_policy in alc_projection_negative_cases:
            try:
                _alc_configuration_projection_receipt(
                    alc_projection_config,
                    bad_raw,
                    f"self-test ALC projection {label}",
                    bad_policy,
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid ALC projection {label}"
                )

        metal_receipt_fields = {
            **alc_receipt_fields,
            "metal_shader_function_cache_requested": "1",
            "metal_shader_function_cache": "1",
            "metal_depth_stencil_state_cache_requested": "1",
            "metal_depth_stencil_state_cache": "1",
            "metal_state_only_record_elision_requested": "1",
            "metal_state_only_record_elision": "1",
        }
        metal_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}" for name, value in metal_receipt_fields.items()
        )
        metal_primary_contract = CANDIDATE_MARKER_ALLOWLIST[
            MSORE_CANDIDATE_SELECTOR
        ]
        metal_primary_marker = " ".join(
            [
                metal_primary_contract["prefix"],
                *metal_primary_contract["required_tokens"],
            ]
        )
        metal_companion_markers = []
        for selector in (MSFC_COMPANION_SELECTOR, MDSSC_COMPANION_SELECTOR):
            companion_contract = CANDIDATE_MARKER_ALLOWLIST[selector]
            metal_companion_markers.append(
                " ".join(
                    [
                        companion_contract["prefix"],
                        *companion_contract["required_tokens"],
                    ]
                )
            )
        metal_config = json.loads(json.dumps(alc_config))
        metal_config["provenance"] = {
            "record_id": METAL_RUNTIME_BUNDLE_RECORD_ID,
            "run_receipt": metal_receipt,
            "candidate_selector": MSORE_CANDIDATE_SELECTOR,
            "selectors": dict(
                METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE[
                    "selector_bundle"
                ]
            ),
            "selector_receipts": {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE[
                    "selector_bundle"
                ]
            },
        }
        metal_config["lineage"] = {
            "generation": 4,
            "parent_record_id": ALC_DIRECT_RECORD_ID,
            "original_role": "candidate",
        }
        metal_config["candidate_marker"] = {
            "required": True,
            "prefix": metal_primary_contract["prefix"],
            "required_tokens": list(
                metal_primary_contract["required_tokens"]
            ),
            "coverage_required": True,
            "coverage_token": metal_primary_contract["coverage_token"],
        }
        metal_config["launch"]["experiment_environment"] = {
            name: _selector_env_text(selected)
            for name, selected in METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE[
                "selector_bundle"
            ].items()
        }
        metal_config["launch"]["common_environment"].update(
            {
                "TEST_RECEIPT": metal_receipt,
                "TEST_MARKER": metal_primary_marker,
            }
        )
        metal_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(metal_config["launch"]["common_environment"])
        metal_config_path = root / "metal-runtime-bundle-start-validation.json"
        _atomic_json(metal_config_path, metal_config)
        validated_metal_config = _validate_start_config(metal_config_path)
        metal_effective, _ = _effective_selectors_from_logs(
            [metal_receipt],
            validated_metal_config,
            "self-test MSORE+MSFC+MDSSC receipts",
        )
        metal_marker_logs = [
            metal_primary_marker,
            *metal_companion_markers,
        ]
        _require(
            _metal_runtime_bundle_projection_scope_for(
                validated_metal_config, "self-test Metal bundle scope"
            )
            is METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE
            and _alc_projection_scope_for(
                validated_metal_config, "self-test Metal ALC parent scope"
            )
            is None
            and _ifsm_direct_scope_for(
                validated_metal_config, "self-test Metal IFSM ancestor scope"
            )
            is None
            and _configuration_projection_policy_for(
                validated_metal_config,
                "self-test Metal projection policy",
                test_mode=False,
                override=None,
            )
            is METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_POLICY
            and metal_effective
            == METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE[
                "selector_bundle"
            ]
            and _candidate_required_receipt_fields(
                validated_metal_config,
                "self-test Metal required receipts",
            )
            == CANDIDATE_REQUIRED_RECEIPT_FIELDS[MSORE_CANDIDATE_SELECTOR]
            and _validate_candidate_marker_lines(
                metal_marker_logs,
                validated_metal_config,
                "self-test MSORE primary marker",
            )
            == (1, True)
            and _validate_companion_candidate_marker_lines(
                metal_marker_logs,
                validated_metal_config,
                "self-test MSFC/MDSSC companion markers",
            )
            == 2,
            "self-test",
            "MSORE+MSFC+MDSSC generation-4 selector/receipt/marker/projection contract failed",
        )

        metal_negative_cases = []
        for selector in (
            MSORE_CANDIDATE_SELECTOR,
            MSFC_COMPANION_SELECTOR,
            MDSSC_COMPANION_SELECTOR,
        ):
            selector_off = json.loads(json.dumps(metal_config))
            selector_off["provenance"]["selectors"][selector] = False
            selector_off["launch"]["experiment_environment"][selector] = "0"
            metal_negative_cases.append((f"selector-off-{selector}", selector_off))

            selector_missing = json.loads(json.dumps(metal_config))
            for mapping in (
                selector_missing["provenance"]["selectors"],
                selector_missing["provenance"]["selector_receipts"],
                selector_missing["launch"]["experiment_environment"],
            ):
                del mapping[selector]
            metal_negative_cases.append(
                (f"selector-missing-{selector}", selector_missing)
            )
        remapped_metal_receipt = json.loads(json.dumps(metal_config))
        remapped_metal_receipt["provenance"]["selector_receipts"][
            MSORE_CANDIDATE_SELECTOR
        ]["field"] = "metal_state_only_record_elision_requested"
        metal_negative_cases.append(
            ("selector-receipt-remap", remapped_metal_receipt)
        )
        mixed_metal_sibling = json.loads(json.dumps(metal_config))
        mixed_metal_sibling["provenance"]["selectors"][
            UFSB_CANDIDATE_SELECTOR
        ] = False
        mixed_metal_sibling["provenance"]["selector_receipts"][
            UFSB_CANDIDATE_SELECTOR
        ] = dict(SELECTOR_RECEIPT_ALLOWLIST[UFSB_CANDIDATE_SELECTOR])
        mixed_metal_sibling["launch"]["experiment_environment"][
            UFSB_CANDIDATE_SELECTOR
        ] = "0"
        metal_negative_cases.append(("mixed-sibling", mixed_metal_sibling))
        for field in (
            "metal_shader_function_cache_requested",
            "metal_shader_function_cache",
            "metal_depth_stencil_state_cache_requested",
            "metal_depth_stencil_state_cache",
            "metal_state_only_record_elision_requested",
            "metal_state_only_record_elision",
        ):
            receipt_off = json.loads(json.dumps(metal_config))
            receipt_off["provenance"]["run_receipt"] = receipt_off[
                "provenance"
            ]["run_receipt"].replace(f"{field}=1", f"{field}=0")
            metal_negative_cases.append((f"receipt-{field}", receipt_off))
        for label, value in metal_negative_cases:
            path = root / f"rejected-metal-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted Metal bundle {label}")

        for label, logs in (
            ("missing-msfc", [metal_primary_marker, metal_companion_markers[1]]),
            (
                "duplicate-msfc",
                [metal_primary_marker, *metal_companion_markers, metal_companion_markers[0]],
            ),
            (
                "reordered-mdssc",
                [
                    metal_primary_marker,
                    metal_companion_markers[0],
                    metal_companion_markers[1].replace(
                        "requested=positive compiled=positive",
                        "compiled=positive requested=positive",
                    ),
                ],
            ),
        ):
            try:
                _validate_companion_candidate_marker_lines(
                    logs,
                    validated_metal_config,
                    f"self-test Metal companion marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid Metal companion marker {label}"
                )

        metal_projection_config = json.loads(
            json.dumps(validated_metal_config)
        )
        metal_projection_config["comparability"]["runtime"][
            "configuration_sha256"
        ] = _sha_bytes(alc_fixture_projected)
        metal_fixture_projection_policy = {
            **METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_POLICY,
            "raw_bytes": len(alc_fixture_raw),
            "raw_sha256": _sha_bytes(alc_fixture_raw),
            "projected_bytes": len(alc_fixture_projected),
            "projected_sha256": _sha_bytes(alc_fixture_projected),
        }
        metal_projection_receipt = _configuration_projection_preflight(
            metal_projection_config,
            alc_fixture_raw,
            "self-test synthetic Metal bundle projection",
            test_mode=True,
            policy_override=metal_fixture_projection_policy,
        )
        _require(
            metal_projection_receipt is not None
            and set(metal_projection_receipt)
            == METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_RECEIPT_KEYS
            and metal_projection_receipt["attestation"]
            == METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_ATTESTATION
            and metal_projection_receipt["policy"]
            == METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_POLICY_ID
            and metal_projection_receipt["mode"]
            == METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_MODE
            and metal_projection_receipt["projected_sha256"]
            == _sha_bytes(alc_fixture_projected),
            "self-test",
            "synthetic MSORE+MSFC+MDSSC projection receipt mismatch",
        )

        noda_receipt_fields = {
            **metal_receipt_fields,
            "stable_negative_route_requested": "1",
            "stable_negative_route": "0",
            "stable_negative_route_superseded": "1",
            "profitable_block_admission_requested": "0",
            "profitable_block_admission": "0",
            "profitable_block_admission_class": (
                "direct_positive_then_ordinary"
            ),
            "native_only_admission_requested": "1",
            "native_only_admission": "1",
            "native_only_admission_class": (
                "fastblock_primary_native_only_dynarec"
            ),
        }
        noda_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}" for name, value in noda_receipt_fields.items()
        )
        noda_contract = CANDIDATE_MARKER_ALLOWLIST[
            NODA_CANDIDATE_SELECTOR
        ]
        noda_marker = " ".join(
            [noda_contract["prefix"], *noda_contract["required_tokens"]]
        )
        noda_config = json.loads(json.dumps(metal_config))
        noda_config["provenance"] = {
            "record_id": NODA_RECORD_ID,
            "run_receipt": noda_receipt,
            "candidate_selector": NODA_CANDIDATE_SELECTOR,
            "selectors": dict(
                NODA_CONFIGURATION_PROJECTION_SCOPE["selector_bundle"]
            ),
            "selector_receipts": {
                name: dict(
                    NODA_SELECTOR_RECEIPT_OVERRIDES.get(
                        name, SELECTOR_RECEIPT_ALLOWLIST[name]
                    )
                )
                for name in NODA_CONFIGURATION_PROJECTION_SCOPE[
                    "selector_bundle"
                ]
            },
        }
        noda_config["lineage"] = {
            "generation": 5,
            "parent_record_id": METAL_RUNTIME_BUNDLE_RECORD_ID,
            "original_role": "candidate",
        }
        noda_config["candidate_marker"] = {
            "required": True,
            "prefix": noda_contract["prefix"],
            "required_tokens": list(noda_contract["required_tokens"]),
            "coverage_required": True,
            "coverage_token": noda_contract["coverage_token"],
        }
        noda_config["launch"]["experiment_environment"] = {
            name: _selector_env_text(selected)
            for name, selected in NODA_CONFIGURATION_PROJECTION_SCOPE[
                "selector_bundle"
            ].items()
        }
        noda_config["launch"]["common_environment"].update(
            {"TEST_RECEIPT": noda_receipt, "TEST_MARKER": noda_marker}
        )
        noda_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(noda_config["launch"]["common_environment"])
        noda_config_path = root / "noda-g5-start-validation.json"
        _atomic_json(noda_config_path, noda_config)
        validated_noda_config = _validate_start_config(noda_config_path)
        noda_effective, _ = _effective_selectors_from_logs(
            [noda_receipt],
            validated_noda_config,
            "self-test NODA receipts",
        )
        expected_noda_effective = dict(
            NODA_CONFIGURATION_PROJECTION_SCOPE["selector_bundle"]
        )
        expected_noda_effective[
            "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
        ] = False
        noda_marker_logs = [noda_marker, *NODA_RETAINED_MARKER_LINES]
        _require(
            NODA_CONFIGURATION_PROJECTION_SCOPE
            == {
                "protocol_id": PHASE6DA_INITIAL_PROTOCOL_ID,
                "record_id": (
                    "noda-c-a64-dynarec-native-only-admission-g000005"
                ),
                "generation": 5,
                "parent_record_id": METAL_RUNTIME_BUNDLE_RECORD_ID,
                "original_role": "candidate",
                "candidate_selector": NODA_CANDIDATE_SELECTOR,
                "selector_bundle": {
                    **METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_SCOPE[
                        "selector_bundle"
                    ],
                    NODA_CANDIDATE_SELECTOR: True,
                },
                "raw_bytes": 46_749,
                "raw_sha256": (
                    "3d42b0edc9be9efa0156a02400de50ae4afb127ca30b7a74df0a36c1e2291e71"
                ),
                "projection_configured": True,
            }
            and NODA_CONFIGURATION_PROJECTION_ATTESTATION
            == "CONFIRM_NODA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
            and NODA_CONFIGURATION_PROJECTION_POLICY_ID
            == "noda-cmake-cache-production-v1"
            and NODA_CONFIGURATION_PROJECTION_MODE
            == "strip-exact-active-alc-build-options-v1"
            and NODA_SELECTOR_RECEIPT_OVERRIDES
            == {
                "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": {
                    "prefix": A64_CONFIGURATION_PREFIX,
                    "field": "stable_negative_route_requested",
                }
            }
            and PBA_CANDIDATE_SELECTOR
            not in validated_noda_config["provenance"]["selectors"]
            and PBA_CANDIDATE_SELECTOR
            not in validated_noda_config["launch"][
                "experiment_environment"
            ]
            and _noda_projection_scope_for(
                validated_noda_config, "self-test NODA scope"
            )
            is NODA_CONFIGURATION_PROJECTION_SCOPE
            and _metal_runtime_bundle_projection_scope_for(
                validated_noda_config, "self-test NODA Metal parent scope"
            )
            is None
            and _configuration_projection_policy_for(
                validated_noda_config,
                "self-test NODA projection policy",
                test_mode=False,
                override=None,
            )
            is NODA_CONFIGURATION_PROJECTION_POLICY
            and noda_effective == expected_noda_effective
            and _candidate_required_receipt_fields(
                validated_noda_config,
                "self-test NODA required receipts",
            )
            == CANDIDATE_REQUIRED_RECEIPT_FIELDS[NODA_CANDIDATE_SELECTOR]
            and _validate_candidate_marker_lines(
                noda_marker_logs,
                validated_noda_config,
                "self-test NODA candidate marker",
            )
            == (1, True)
            and _validate_companion_candidate_marker_lines(
                noda_marker_logs,
                validated_noda_config,
                "self-test NODA retained markers",
            )
            == len(NODA_RETAINED_MARKER_LINES),
            "self-test",
            "NODA generation-5 selector/receipt/marker/projection contract failed",
        )

        noda_negative_cases = []
        for label, mutate in (
            (
                "wrong-record",
                lambda value: value["provenance"].update(
                    {"record_id": "wrong-record"}
                ),
            ),
            (
                "wrong-generation",
                lambda value: value["lineage"].update({"generation": 4}),
            ),
            (
                "wrong-parent",
                lambda value: value["lineage"].update(
                    {"parent_record_id": ALC_DIRECT_RECORD_ID}
                ),
            ),
            (
                "selector-off",
                lambda value: (
                    value["provenance"]["selectors"].update(
                        {NODA_CANDIDATE_SELECTOR: False}
                    ),
                    value["launch"]["experiment_environment"].update(
                        {NODA_CANDIDATE_SELECTOR: "0"}
                    ),
                ),
            ),
            (
                "retained-6cl-off",
                lambda value: (
                    value["provenance"]["selectors"].update(
                        {
                            "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": False
                        }
                    ),
                    value["launch"]["experiment_environment"].update(
                        {
                            "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": "0"
                        }
                    ),
                ),
            ),
            (
                "retained-metal-off",
                lambda value: (
                    value["provenance"]["selectors"].update(
                        {MSORE_CANDIDATE_SELECTOR: False}
                    ),
                    value["launch"]["experiment_environment"].update(
                        {MSORE_CANDIDATE_SELECTOR: "0"}
                    ),
                ),
            ),
            (
                "historical-6cl-receipt-contract",
                lambda value: value["provenance"]["selector_receipts"].update(
                    {
                        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": dict(
                            SELECTOR_RECEIPT_ALLOWLIST[
                                "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
                            ]
                        )
                    }
                ),
            ),
            (
                "noda-receipt-remap",
                lambda value: value["provenance"]["selector_receipts"]
                [NODA_CANDIDATE_SELECTOR].update(
                    {"field": "native_only_admission_requested"}
                ),
            ),
        ):
            value = json.loads(json.dumps(noda_config))
            mutate(value)
            noda_negative_cases.append((label, value))
        pba_mixed = json.loads(json.dumps(noda_config))
        pba_mixed["provenance"]["selectors"][PBA_CANDIDATE_SELECTOR] = False
        pba_mixed["provenance"]["selector_receipts"][
            PBA_CANDIDATE_SELECTOR
        ] = {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "profitable_block_admission",
        }
        pba_mixed["launch"]["experiment_environment"][
            PBA_CANDIDATE_SELECTOR
        ] = "0"
        noda_negative_cases.append(("mixed-pba-even-off", pba_mixed))
        critical_noda_receipts = (
            "stable_negative_route_requested",
            "stable_negative_route",
            "stable_negative_route_superseded",
            "profitable_block_admission_requested",
            "profitable_block_admission",
            "profitable_block_admission_class",
            "native_only_admission_requested",
            "native_only_admission",
            "native_only_admission_class",
        )
        for field in critical_noda_receipts:
            value = json.loads(json.dumps(noda_config))
            expected = noda_receipt_fields[field]
            replacement = (
                "invalid"
                if field.endswith("_class")
                else ("0" if expected == "1" else "1")
            )
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}={expected}", f"{field}={replacement}")
            noda_negative_cases.append((f"receipt-{field}", value))
        for label, value in noda_negative_cases:
            path = root / f"rejected-noda-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted NODA {label}")

        historical_6cl_effective_off = metal_receipt.replace(
            "stable_negative_route=1", "stable_negative_route=0"
        )
        try:
            _effective_selectors_from_logs(
                [historical_6cl_effective_off],
                validated_metal_config,
                "self-test historical 6CL effective override",
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test admitted the NODA 6CL expected/effective override outside NODA"
            )
        noda_other_effective_off = noda_receipt.replace(
            "guest_memory_operation_snapshot_elision=1",
            "guest_memory_operation_snapshot_elision=0",
        )
        try:
            _effective_selectors_from_logs(
                [noda_other_effective_off],
                validated_noda_config,
                "self-test NODA non-6CL effective override",
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test admitted a non-6CL expected/effective override in NODA"
            )

        for label, marker_logs in (
            ("missing", []),
            ("duplicate", [noda_marker, noda_marker]),
            (
                "reordered",
                [
                    noda_marker.replace(
                        "effective=positive ordinary_routed=positive",
                        "ordinary_routed=positive effective=positive",
                    )
                ],
            ),
            (
                "invalid-coverage",
                [noda_marker.replace("coverage=positive", "coverage=zero")],
            ),
            ("extra-token", [noda_marker + " unexpected=1"]),
        ):
            try:
                _validate_candidate_marker_lines(
                    marker_logs,
                    validated_noda_config,
                    f"self-test NODA candidate marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid NODA candidate marker {label}"
                )

        valid_noda_companion_logs = [
            noda_marker,
            *NODA_RETAINED_MARKER_LINES,
        ]
        invalid_noda_companion_cases = (
            (
                "missing-6cp",
                [
                    line
                    for line in valid_noda_companion_logs
                    if not line.startswith(
                        "[A64_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION_FINAL]"
                    )
                ],
            ),
            (
                "duplicate-6da",
                [
                    *valid_noda_companion_logs,
                    NODA_RETAINED_MARKER_LINES[2],
                ],
            ),
            (
                "positive-not-superseded-6cl",
                [
                    (
                        "[A64_STABLE_NEGATIVE_ROUTE_FINAL] "
                        "classified=positive live_mismatch=none status=clean "
                        "coverage=positive"
                        if line == NODA_STABLE_NEGATIVE_SUPERSEDED_MARKER
                        else line
                    )
                    for line in valid_noda_companion_logs
                ],
            ),
            (
                "pba-marker-present",
                [
                    *valid_noda_companion_logs,
                    "[A64_PROFITABLE_BLOCK_ADMISSION_FINAL] "
                    "positive_armed=positive positive_bypassed=positive "
                    "native_only_executed=positive "
                    "class=direct_positive_then_ordinary status=clean "
                    "coverage=positive",
                ],
            ),
        )
        for label, logs in invalid_noda_companion_cases:
            try:
                _validate_companion_candidate_marker_lines(
                    logs,
                    validated_noda_config,
                    f"self-test NODA retained marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid NODA retained marker {label}"
                )

        noda_projection_config = json.loads(
            json.dumps(validated_noda_config)
        )
        noda_projection_config["comparability"]["runtime"][
            "configuration_sha256"
        ] = _sha_bytes(alc_fixture_projected)
        noda_fixture_projection_policy = {
            **NODA_CONFIGURATION_PROJECTION_POLICY,
            "raw_bytes": len(alc_fixture_raw),
            "raw_sha256": _sha_bytes(alc_fixture_raw),
            "projected_bytes": len(alc_fixture_projected),
            "projected_sha256": _sha_bytes(alc_fixture_projected),
        }
        noda_projection_receipt = _configuration_projection_preflight(
            noda_projection_config,
            alc_fixture_raw,
            "self-test synthetic NODA projection",
            test_mode=True,
            policy_override=noda_fixture_projection_policy,
        )
        _require(
            noda_projection_receipt is not None
            and set(noda_projection_receipt)
            == NODA_CONFIGURATION_PROJECTION_RECEIPT_KEYS
            and noda_projection_receipt["attestation"]
            == NODA_CONFIGURATION_PROJECTION_ATTESTATION
            and noda_projection_receipt["policy"]
            == NODA_CONFIGURATION_PROJECTION_POLICY_ID
            and noda_projection_receipt["mode"]
            == NODA_CONFIGURATION_PROJECTION_MODE
            and noda_projection_receipt["record_id"] == NODA_RECORD_ID
            and noda_projection_receipt["candidate_selector"]
            == NODA_CANDIDATE_SELECTOR
            and noda_projection_receipt["projected_sha256"]
            == _sha_bytes(alc_fixture_projected),
            "self-test",
            "synthetic NODA projection receipt mismatch",
        )

        fcfos_receipt_fields = {
            **ifsm_receipt_fields,
            "fast_block_unrolled_fixed_signature_batching_requested": "0",
            "fast_block_unrolled_fixed_signature_batching": "0",
            "fast_block_idempotent_mru_store_elision_requested": "0",
            "fast_block_idempotent_mru_store_elision": "0",
            "fast_block_callback_free_ordinary_specialization_requested": "1",
            "fast_block_callback_free_ordinary_specialization": "1",
        }
        fcfos_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}"
            for name, value in fcfos_receipt_fields.items()
        )
        fcfos_contract = CANDIDATE_MARKER_ALLOWLIST[
            FCFOS_CANDIDATE_SELECTOR
        ]
        fcfos_marker = " ".join(
            [fcfos_contract["prefix"], *fcfos_contract["required_tokens"]]
        )
        fcfos_config = json.loads(json.dumps(ifsm_config))
        fcfos_config["provenance"] = {
            "record_id": FCFOS_DIRECT_SCOPE["record_id"],
            "run_receipt": fcfos_receipt,
            "candidate_selector": FCFOS_CANDIDATE_SELECTOR,
            "selectors": dict(FCFOS_DIRECT_SCOPE["selector_bundle"]),
            "selector_receipts": {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in FCFOS_DIRECT_SCOPE["selector_bundle"]
            },
        }
        fcfos_config["lineage"] = {
            "generation": FCFOS_DIRECT_SCOPE["generation"],
            "parent_record_id": FCFOS_DIRECT_SCOPE["parent_record_id"],
            "original_role": FCFOS_DIRECT_SCOPE["original_role"],
        }
        fcfos_config["candidate_marker"] = {
            "required": True,
            "prefix": fcfos_contract["prefix"],
            "required_tokens": list(fcfos_contract["required_tokens"]),
            "coverage_required": True,
            "coverage_token": fcfos_contract["coverage_token"],
        }
        fcfos_config["launch"]["experiment_environment"] = {
            name: _selector_env_text(selected)
            for name, selected in FCFOS_DIRECT_SCOPE[
                "selector_bundle"
            ].items()
        }
        fcfos_config["launch"]["common_environment"].update(
            {"TEST_RECEIPT": fcfos_receipt, "TEST_MARKER": fcfos_marker}
        )
        fcfos_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(fcfos_config["launch"]["common_environment"])
        fcfos_config_path = root / "fcfos-direct-start-validation.json"
        _atomic_json(fcfos_config_path, fcfos_config)
        validated_fcfos_config = _validate_start_config(fcfos_config_path)
        fcfos_effective, _ = _effective_selectors_from_logs(
            [fcfos_receipt],
            validated_fcfos_config,
            "self-test FCFOS receipts",
        )
        _require(
            _fcfos_direct_scope_for(
                validated_fcfos_config,
                "self-test FCFOS direct scope",
            )
            is FCFOS_DIRECT_SCOPE
            and _ifsm_direct_scope_for(
                validated_fcfos_config,
                "self-test FCFOS IFSM parent scope",
            )
            is None
            and _ufsb_imse_direct_scope_for(
                validated_fcfos_config,
                "self-test FCFOS sibling scope",
            )
            is None
            and _configuration_projection_policy_for(
                validated_fcfos_config,
                "self-test FCFOS direct policy",
                test_mode=False,
                override=None,
            )
            is None
            and fcfos_effective == FCFOS_DIRECT_SCOPE["selector_bundle"]
            and _candidate_required_receipt_fields(
                validated_fcfos_config,
                "self-test FCFOS required receipts",
            )
            == CANDIDATE_REQUIRED_RECEIPT_FIELDS[FCFOS_CANDIDATE_SELECTOR]
            and _validate_candidate_marker_lines(
                [fcfos_marker],
                validated_fcfos_config,
                "self-test FCFOS marker",
            )
            == (1, True),
            "self-test",
            "FCFOS generation-3 direct selector/receipt/marker contract failed",
        )

        for label, marker_logs in (
            ("missing", []),
            ("duplicate", [fcfos_marker, fcfos_marker]),
            (
                "unrequested",
                [
                    fcfos_marker.replace(
                        "requested=positive", "requested=zero"
                    )
                ],
            ),
            (
                "ineffective",
                [
                    fcfos_marker.replace(
                        "effective=positive", "effective=zero"
                    )
                ],
            ),
            (
                "missing-reentry-safe",
                [fcfos_marker.replace(" reentry_safe=positive", "")],
            ),
            (
                "missing-snapshot-elision",
                [fcfos_marker.replace(" snapshot_elision=positive", "")],
            ),
            (
                "reordered",
                [
                    fcfos_marker.replace(
                        "reentry_safe=positive snapshot_elision=positive",
                        "snapshot_elision=positive reentry_safe=positive",
                    )
                ],
            ),
            (
                "invalid-status",
                [fcfos_marker.replace("status=clean", "status=invalid")],
            ),
            (
                "zero-coverage",
                [fcfos_marker.replace("coverage=positive", "coverage=zero")],
            ),
            ("extra-token", [fcfos_marker + " unexpected=1"]),
        ):
            try:
                _validate_candidate_marker_lines(
                    marker_logs,
                    validated_fcfos_config,
                    f"self-test FCFOS marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted invalid FCFOS marker {label}"
                )

        fcfos_negative_cases = []
        for label, mutate in (
            (
                "wrong-record",
                lambda value: value["provenance"].update(
                    {"record_id": "wrong-record"}
                ),
            ),
            (
                "wrong-generation",
                lambda value: value["lineage"].update({"generation": 2}),
            ),
            (
                "wrong-parent",
                lambda value: value["lineage"].update(
                    {"parent_record_id": "wrong-parent"}
                ),
            ),
            (
                "wrong-os-build",
                lambda value: value["comparability"]["os"].update(
                    {"build": "wrong-build"}
                ),
            ),
            (
                "wrong-configuration",
                lambda value: value["comparability"]["runtime"].update(
                    {"configuration_sha256": "0" * 64}
                ),
            ),
            (
                "selector-off",
                lambda value: value["provenance"]["selectors"].update(
                    {FCFOS_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "selector-integer",
                lambda value: value["provenance"]["selectors"].update(
                    {FCFOS_CANDIDATE_SELECTOR: 1}
                ),
            ),
            (
                "selector-string",
                lambda value: value["provenance"]["selectors"].update(
                    {FCFOS_CANDIDATE_SELECTOR: "1"}
                ),
            ),
            (
                "retained-ifsm-off",
                lambda value: value["provenance"]["selectors"].update(
                    {IFSM_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "retained-6da-off",
                lambda value: value["provenance"]["selectors"].update(
                    {PHASE6DA_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "malformed-environment",
                lambda value: value["launch"][
                    "experiment_environment"
                ].update({FCFOS_CANDIDATE_SELECTOR: "01"}),
            ),
            (
                "selector-receipt-remap",
                lambda value: value["provenance"]["selector_receipts"][
                    FCFOS_CANDIDATE_SELECTOR
                ].update(
                    {
                        "field": (
                            "fast_block_callback_free_ordinary_"
                            "specialization_requested"
                        )
                    }
                ),
            ),
        ):
            value = json.loads(json.dumps(fcfos_config))
            mutate(value)
            fcfos_negative_cases.append((label, value))

        missing_fcfos_selector = json.loads(json.dumps(fcfos_config))
        for mapping in (
            missing_fcfos_selector["provenance"]["selectors"],
            missing_fcfos_selector["provenance"]["selector_receipts"],
            missing_fcfos_selector["launch"]["experiment_environment"],
        ):
            del mapping[FCFOS_CANDIDATE_SELECTOR]
        fcfos_negative_cases.append(
            ("missing-candidate-selector", missing_fcfos_selector)
        )

        for sibling in (
            UFSB_CANDIDATE_SELECTOR,
            IMSE_COMPANION_SELECTOR,
            PHASE6CS_CANDIDATE_SELECTOR,
        ):
            value = json.loads(json.dumps(fcfos_config))
            value["provenance"]["selectors"][sibling] = False
            value["provenance"]["selector_receipts"][sibling] = dict(
                SELECTOR_RECEIPT_ALLOWLIST[sibling]
            )
            value["launch"]["experiment_environment"][sibling] = "0"
            fcfos_negative_cases.append((f"mixed-sibling-{sibling}", value))

        for field, expected in CANDIDATE_REQUIRED_RECEIPT_FIELDS[
            FCFOS_CANDIDATE_SELECTOR
        ].items():
            value = json.loads(json.dumps(fcfos_config))
            replacement = "0" if expected == "1" else "1"
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}={expected}", f"{field}={replacement}")
            fcfos_negative_cases.append((f"receipt-{field}", value))

        for label, value in fcfos_negative_cases:
            path = root / f"rejected-fcfos-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted FCFOS {label}")

        try:
            _configuration_projection_preflight(
                validated_fcfos_config,
                b"not-the-preregistered-direct-configuration",
                "self-test FCFOS wrong direct bytes",
                test_mode=True,
                policy_override=None,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted FCFOS wrong direct configuration bytes"
            )

        combined_receipt_fields = {
            **ifsm_receipt_fields,
            "fast_block_unrolled_fixed_signature_batching_requested": "1",
            "fast_block_unrolled_fixed_signature_batching": "1",
            "fast_block_idempotent_mru_store_elision_requested": "1",
            "fast_block_idempotent_mru_store_elision": "1",
        }
        combined_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}"
            for name, value in combined_receipt_fields.items()
        )
        ufsb_contract = CANDIDATE_MARKER_ALLOWLIST[UFSB_CANDIDATE_SELECTOR]
        ufsb_marker = " ".join(
            [ufsb_contract["prefix"], *ufsb_contract["required_tokens"]]
        )
        imse_contract = CANDIDATE_MARKER_ALLOWLIST[IMSE_COMPANION_SELECTOR]
        imse_marker = " ".join(
            [imse_contract["prefix"], *imse_contract["required_tokens"]]
        )
        combined_config = json.loads(json.dumps(ifsm_config))
        combined_config["provenance"] = {
            "record_id": UFSB_IMSE_DIRECT_SCOPE["record_id"],
            "run_receipt": combined_receipt,
            "candidate_selector": UFSB_CANDIDATE_SELECTOR,
            "selectors": dict(UFSB_IMSE_DIRECT_SCOPE["selector_bundle"]),
            "selector_receipts": {
                name: dict(SELECTOR_RECEIPT_ALLOWLIST[name])
                for name in UFSB_IMSE_DIRECT_SCOPE["selector_bundle"]
            },
        }
        combined_config["lineage"] = {
            "generation": UFSB_IMSE_DIRECT_SCOPE["generation"],
            "parent_record_id": UFSB_IMSE_DIRECT_SCOPE["parent_record_id"],
            "original_role": UFSB_IMSE_DIRECT_SCOPE["original_role"],
        }
        combined_config["candidate_marker"] = {
            "required": True,
            "prefix": ufsb_contract["prefix"],
            "required_tokens": list(ufsb_contract["required_tokens"]),
            "coverage_required": True,
            "coverage_token": ufsb_contract["coverage_token"],
        }
        combined_config["launch"]["experiment_environment"] = {
            name: _selector_env_text(selected)
            for name, selected in UFSB_IMSE_DIRECT_SCOPE[
                "selector_bundle"
            ].items()
        }
        combined_config["launch"]["common_environment"].update(
            {"TEST_RECEIPT": combined_receipt, "TEST_MARKER": ufsb_marker}
        )
        combined_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(combined_config["launch"]["common_environment"])
        combined_config_path = root / "ufsb-imse-direct-start-validation.json"
        _atomic_json(combined_config_path, combined_config)
        validated_combined_config = _validate_start_config(combined_config_path)
        combined_effective, _ = _effective_selectors_from_logs(
            [combined_receipt],
            validated_combined_config,
            "self-test UFSB+IMSE receipts",
        )
        _require(
            _ufsb_imse_direct_scope_for(
                validated_combined_config,
                "self-test UFSB+IMSE direct scope",
            )
            is UFSB_IMSE_DIRECT_SCOPE
            and _ifsm_direct_scope_for(
                validated_combined_config,
                "self-test UFSB+IMSE IFSM parent scope",
            )
            is None
            and _configuration_projection_policy_for(
                validated_combined_config,
                "self-test UFSB+IMSE direct policy",
                test_mode=False,
                override=None,
            )
            is None
            and combined_effective
            == UFSB_IMSE_DIRECT_SCOPE["selector_bundle"]
            and _candidate_required_receipt_fields(
                validated_combined_config,
                "self-test UFSB+IMSE required receipts",
            )
            == CANDIDATE_REQUIRED_RECEIPT_FIELDS[UFSB_CANDIDATE_SELECTOR]
            and _validate_candidate_marker_lines(
                [ufsb_marker],
                validated_combined_config,
                "self-test UFSB+IMSE primary marker",
            )
            == (1, True)
            and _validate_companion_candidate_marker_lines(
                [ufsb_marker, imse_marker],
                validated_combined_config,
                "self-test UFSB+IMSE companion marker",
            )
            == 1,
            "self-test",
            "UFSB+IMSE generation-3 direct selector/receipt/marker contract failed",
        )

        combined_negative_cases = []
        for label, mutate in (
            (
                "wrong-record",
                lambda value: value["provenance"].update(
                    {"record_id": "wrong-record"}
                ),
            ),
            (
                "wrong-generation",
                lambda value: value["lineage"].update({"generation": 2}),
            ),
            (
                "wrong-parent",
                lambda value: value["lineage"].update(
                    {"parent_record_id": "wrong-parent"}
                ),
            ),
            (
                "wrong-primary",
                lambda value: value["provenance"].update(
                    {"candidate_selector": IMSE_COMPANION_SELECTOR}
                ),
            ),
            (
                "wrong-os-build",
                lambda value: value["comparability"]["os"].update(
                    {"build": "wrong-build"}
                ),
            ),
            (
                "wrong-configuration",
                lambda value: value["comparability"]["runtime"].update(
                    {"configuration_sha256": "0" * 64}
                ),
            ),
            (
                "ufsb-off",
                lambda value: value["provenance"]["selectors"].update(
                    {UFSB_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "ufsb-integer",
                lambda value: value["provenance"]["selectors"].update(
                    {UFSB_CANDIDATE_SELECTOR: 1}
                ),
            ),
            (
                "imse-off",
                lambda value: value["provenance"]["selectors"].update(
                    {IMSE_COMPANION_SELECTOR: False}
                ),
            ),
            (
                "imse-string",
                lambda value: value["provenance"]["selectors"].update(
                    {IMSE_COMPANION_SELECTOR: "1"}
                ),
            ),
            (
                "retained-ifsm-off",
                lambda value: value["provenance"]["selectors"].update(
                    {IFSM_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "retained-6da-off",
                lambda value: value["provenance"]["selectors"].update(
                    {PHASE6DA_CANDIDATE_SELECTOR: False}
                ),
            ),
            (
                "malformed-ufsb-environment",
                lambda value: value["launch"]["experiment_environment"].update(
                    {UFSB_CANDIDATE_SELECTOR: "01"}
                ),
            ),
            (
                "malformed-imse-environment",
                lambda value: value["launch"]["experiment_environment"].update(
                    {IMSE_COMPANION_SELECTOR: "01"}
                ),
            ),
            (
                "ufsb-receipt-remap",
                lambda value: value["provenance"]["selector_receipts"][
                    UFSB_CANDIDATE_SELECTOR
                ].update(
                    {"field": "fast_block_unrolled_fixed_signature_batching_requested"}
                ),
            ),
            (
                "imse-receipt-remap",
                lambda value: value["provenance"]["selector_receipts"][
                    IMSE_COMPANION_SELECTOR
                ].update(
                    {"field": "fast_block_idempotent_mru_store_elision_requested"}
                ),
            ),
        ):
            value = json.loads(json.dumps(combined_config))
            mutate(value)
            combined_negative_cases.append((label, value))

        for selector in (UFSB_CANDIDATE_SELECTOR, IMSE_COMPANION_SELECTOR):
            value = json.loads(json.dumps(combined_config))
            for mapping in (
                value["provenance"]["selectors"],
                value["provenance"]["selector_receipts"],
                value["launch"]["experiment_environment"],
            ):
                del mapping[selector]
            combined_negative_cases.append((f"missing-{selector}", value))

        mixed_combined = json.loads(json.dumps(combined_config))
        mixed_combined["provenance"]["selectors"][
            PHASE6CS_CANDIDATE_SELECTOR
        ] = False
        mixed_combined["provenance"]["selector_receipts"][
            PHASE6CS_CANDIDATE_SELECTOR
        ] = dict(SELECTOR_RECEIPT_ALLOWLIST[PHASE6CS_CANDIDATE_SELECTOR])
        mixed_combined["launch"]["experiment_environment"][
            PHASE6CS_CANDIDATE_SELECTOR
        ] = "0"
        combined_negative_cases.append(("mixed-retired-sibling", mixed_combined))

        for field, expected in CANDIDATE_REQUIRED_RECEIPT_FIELDS[
            UFSB_CANDIDATE_SELECTOR
        ].items():
            value = json.loads(json.dumps(combined_config))
            replacement = "0" if expected == "1" else "1"
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}={expected}", f"{field}={replacement}")
            combined_negative_cases.append((f"receipt-{field}", value))

        for label, value in combined_negative_cases:
            path = root / f"rejected-ufsb-imse-{label}.json"
            _atomic_json(path, value)
            try:
                _validate_start_config(path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted UFSB+IMSE {label}")

        for label, marker_logs in (
            ("missing", [imse_marker]),
            ("duplicate", [ufsb_marker, ufsb_marker]),
            ("missing-dependency", [ufsb_marker.replace("dependency=positive ", "")]),
            (
                "reordered",
                [
                    ufsb_marker.replace(
                        "dependency=positive compared=positive",
                        "compared=positive dependency=positive",
                    )
                ],
            ),
            ("invalid-status", [ufsb_marker.replace("status=clean", "status=invalid")]),
            ("zero-coverage", [ufsb_marker.replace("coverage=positive", "coverage=zero")]),
            ("extra-token", [ufsb_marker + " unexpected=1"]),
        ):
            try:
                _validate_candidate_marker_lines(
                    marker_logs,
                    validated_combined_config,
                    f"self-test UFSB+IMSE primary marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted UFSB+IMSE primary marker {label}"
                )

        for label, marker_logs in (
            ("missing", [ufsb_marker]),
            ("duplicate", [imse_marker, imse_marker]),
            ("ineffective", [imse_marker.replace("effective=positive", "effective=zero")]),
            (
                "reordered",
                [
                    imse_marker.replace(
                        "requested=positive effective=positive",
                        "effective=positive requested=positive",
                    )
                ],
            ),
            ("invalid-status", [imse_marker.replace("status=clean", "status=invalid")]),
            ("zero-coverage", [imse_marker.replace("coverage=positive", "coverage=zero")]),
            ("extra-token", [imse_marker + " unexpected=1"]),
        ):
            try:
                _validate_companion_candidate_marker_lines(
                    marker_logs,
                    validated_combined_config,
                    f"self-test UFSB+IMSE companion marker {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted UFSB+IMSE companion marker {label}"
                )

        try:
            _configuration_projection_preflight(
                validated_combined_config,
                b"not-the-preregistered-direct-configuration",
                "self-test UFSB+IMSE wrong direct bytes",
                test_mode=True,
                policy_override=None,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted UFSB+IMSE wrong direct configuration bytes"
            )

        expected_projection_receipt = _phase6cp_configuration_projection_receipt(
            projection_config,
            fixture_configuration_raw,
            "self-test projection positive",
            fixture_projection_policy,
        )
        _require(
            set(expected_projection_receipt)
            == PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_KEYS
            and expected_projection_receipt["raw_sha256"]
            == _sha_bytes(fixture_configuration_raw)
            and expected_projection_receipt["projected_sha256"]
            == _sha_bytes(projected_configuration),
            "self-test",
            "configuration projection positive receipt mismatch",
        )

        def projection_policy_for_raw(raw: bytes) -> Mapping[str, Any]:
            return {
                **fixture_projection_policy,
                "raw_bytes": len(raw),
                "raw_sha256": _sha_bytes(raw),
            }

        projection_negative_cases = {
            "raw hash": (
                fixture_configuration_raw + b"x",
                fixture_projection_policy,
            ),
            "ON": (
                fixture_configuration_raw.replace(
                    b"LUMEN_A64_DYNAREC_PHASE6CM_PROFILE:BOOL=OFF",
                    b"LUMEN_A64_DYNAREC_PHASE6CM_PROFILE:BOOL=ON",
                ),
                None,
            ),
            "missing": (
                fixture_configuration_raw.replace(
                    PHASE6CP_PHASE6CM_CACHE_BLOCK, b""
                ),
                None,
            ),
            "duplicate": (
                fixture_configuration_raw + PHASE6CP_PHASE6CM_CACHE_BLOCK,
                None,
            ),
            "order": (
                projected_configuration
                + PHASE6CP_PHASE6CO_CACHE_BLOCK
                + PHASE6CP_PHASE6CM_CACHE_BLOCK,
                None,
            ),
            "comment": (
                fixture_configuration_raw.replace(
                    b"ordinary-body diagnostic",
                    b"ordinary-body Diagnostic",
                ),
                None,
            ),
            "CRLF": (fixture_configuration_raw.replace(b"\n", b"\r\n"), None),
            "production delta": (
                b"UNRELATED_PRODUCTION_OPTION:BOOL=ON\n"
                + fixture_configuration_raw,
                None,
            ),
        }
        for label, (bad_raw, explicit_policy) in projection_negative_cases.items():
            bad_policy = (
                explicit_policy
                if explicit_policy is not None
                else projection_policy_for_raw(bad_raw)
            )
            try:
                _phase6cp_configuration_projection_receipt(
                    projection_config,
                    bad_raw,
                    f"self-test projection {label}",
                    bad_policy,
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted configuration projection {label}"
                )

        wrong_scope = json.loads(json.dumps(projection_config))
        wrong_scope["lineage"]["generation"] += 1
        try:
            _phase6cp_configuration_projection_receipt(
                wrong_scope,
                fixture_configuration_raw,
                "self-test projection scope",
                fixture_projection_policy,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted projection outside exact scope")

        legacy_config = json.loads(json.dumps(projection_config))
        legacy_config["provenance"]["candidate_selector"] = (
            "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
        )
        legacy_config["provenance"]["selectors"].pop(
            PHASE6CP_CANDIDATE_SELECTOR
        )
        legacy_config["provenance"]["selector_receipts"].pop(
            PHASE6CP_CANDIDATE_SELECTOR
        )
        legacy_config["launch"]["experiment_environment"].pop(
            PHASE6CP_CANDIDATE_SELECTOR
        )
        legacy_config["comparability"]["runtime"]["configuration_sha256"] = (
            _sha_bytes(projected_configuration)
        )
        _require(
            _configuration_projection_preflight(
                legacy_config,
                projected_configuration,
                "self-test legacy raw",
                test_mode=True,
                policy_override=None,
            )
            is None,
            "self-test",
            "legacy raw configuration comparison unexpectedly projected",
        )

        capture_fixture = root / "capture-fixture.txt"
        capture_fixture.write_bytes(b"stable\n")
        capture_link = root / "capture-link.txt"
        capture_link.symlink_to(capture_fixture)
        try:
            _capture_stable_regular_bytes(
                capture_link, purpose="self-test symlink capture"
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test captured a configuration symlink")
        try:
            _capture_stable_regular_bytes(
                capture_fixture,
                purpose="self-test mutated capture",
                _after_open_hook=lambda: capture_fixture.write_bytes(b"mutated\n"),
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted mutation during capture")
        capture_replacement = root / "capture-replacement.txt"
        capture_replacement.write_bytes(b"replacement\n")
        try:
            _capture_stable_regular_bytes(
                capture_fixture,
                purpose="self-test pathname-swap capture",
                _after_open_hook=lambda: os.replace(
                    capture_replacement, capture_fixture
                ),
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted a pathname swap during capture")

        projection_artifact_root = root / "projection-artifacts"
        projection_artifact_root.mkdir()
        configuration.write_bytes(fixture_configuration_raw)
        projection_freeze = _publish_configuration_projection_artifacts(
            projection_artifact_root,
            fixture_configuration_raw,
            expected_projection_receipt,
        )
        projection_artifacts = _validate_configuration_projection_artifacts(
            projection_artifact_root,
            projection_config,
            projection_freeze,
            test_mode=True,
            policy_override=fixture_projection_policy,
        )
        projection_raw_path = (
            projection_artifact_root / PHASE6CP_CONFIGURATION_CACHE_RAW_NAME
        )
        projection_receipt_path = (
            projection_artifact_root
            / PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME
        )
        _require(
            projection_artifacts
            == {
                "configuration_cache_raw": _sha_file(projection_raw_path),
                "configuration_projection_receipt": _sha_file(
                    projection_receipt_path
                ),
            },
            "self-test",
            "configuration projection artifact hashes mismatch",
        )
        projection_raw_payload = projection_raw_path.read_bytes()
        projection_receipt_payload = projection_receipt_path.read_bytes()

        def expect_projection_artifact_failure(label: str) -> None:
            try:
                _validate_configuration_projection_artifacts(
                    projection_artifact_root,
                    projection_config,
                    projection_freeze,
                    test_mode=True,
                    policy_override=fixture_projection_policy,
                )
            except HarnessError:
                return
            raise HarnessError(
                f"self-test accepted invalid configuration projection {label}"
            )

        for label, path, payload in (
            ("missing raw snapshot", projection_raw_path, projection_raw_payload),
            (
                "missing receipt",
                projection_receipt_path,
                projection_receipt_payload,
            ),
        ):
            path.unlink()
            try:
                expect_projection_artifact_failure(label)
            finally:
                _atomic_bytes(path, payload, mode=0o444)
        for label, path, payload in (
            ("modified raw snapshot", projection_raw_path, projection_raw_payload),
            (
                "modified receipt",
                projection_receipt_path,
                projection_receipt_payload,
            ),
        ):
            path.chmod(0o644)
            path.write_bytes(payload + b"tamper")
            path.chmod(0o444)
            try:
                expect_projection_artifact_failure(label)
            finally:
                path.chmod(0o644)
                path.write_bytes(payload)
                path.chmod(0o444)
        for label, path, mode in (
            ("writable raw snapshot", projection_raw_path, 0o644),
            ("writable receipt", projection_receipt_path, 0o644),
            ("non-canonical raw mode", projection_raw_path, 0o400),
            ("non-canonical receipt mode", projection_receipt_path, 0o440),
        ):
            path.chmod(mode)
            try:
                expect_projection_artifact_failure(label)
            finally:
                path.chmod(0o444)
        configuration.write_bytes(fixture_configuration_raw + b"changed")
        try:
            expect_projection_artifact_failure("changed external configuration")
        finally:
            configuration.write_bytes(fixture_configuration_raw)

        try:
            _configuration_projection_policy_for(
                projection_config,
                "self-test production override",
                test_mode=False,
                override=fixture_projection_policy,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted a configuration projection override outside test mode"
            )

        projection_preflight_registry = root / "projection-preflight-registry"
        (projection_preflight_registry / "pending").mkdir(parents=True)
        (projection_preflight_registry / "records").mkdir()
        projection_preflight_config = json.loads(json.dumps(projection_config))
        projection_preflight_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(
            projection_preflight_config["launch"]["common_environment"]
        )
        _atomic_json(
            projection_preflight_registry / "epoch.json",
            {
                "schema": {
                    "name": EPOCH_SCHEMA_NAME,
                    "version": EPOCH_SCHEMA_VERSION,
                },
                "protocol": projection_preflight_config["protocol"],
                "comparability": projection_preflight_config["comparability"],
            },
        )
        projection_preflight_path = root / "projection-preflight-start.json"
        _atomic_json(projection_preflight_path, projection_preflight_config)
        configuration.write_bytes(fixture_configuration_raw + b"unexpected")
        try:
            start_run(
                projection_preflight_registry,
                projection_preflight_path,
                test_mode=True,
                _configuration_projection_policy=fixture_projection_policy,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test launched an unregistered configuration projection"
            )
        finally:
            configuration.write_bytes(fixture_configuration_raw)
        _require(
            not any((projection_preflight_registry / "pending").iterdir()),
            "self-test",
            "configuration projection failure created a pending record",
        )

        g3_preflight_registry = root / "g3-preflight-registry"
        (g3_preflight_registry / "pending").mkdir(parents=True)
        (g3_preflight_registry / "records").mkdir()
        g3_preflight_config = json.loads(json.dumps(g3_config))
        g3_preflight_config["comparability"]["runtime"][
            "configuration_sha256"
        ] = PHASE6CQ_CONFIGURATION_PROJECTION_POLICY["projected_sha256"]
        g3_preflight_config["launch"]["common_environment"]["TEST_RUN_ROOT"] = (
            str(
                g3_preflight_registry
                / "pending"
                / PHASE6CQ_CONFIGURATION_PROJECTION_SCOPE["record_id"]
            )
        )
        g3_preflight_config["comparability"]["runtime"][
            "sterile_environment_sha256"
        ] = _canonical_sha(
            g3_preflight_config["launch"]["common_environment"]
        )
        _atomic_json(
            g3_preflight_registry / "epoch.json",
            {
                "schema": {
                    "name": EPOCH_SCHEMA_NAME,
                    "version": EPOCH_SCHEMA_VERSION,
                },
                "protocol": g3_preflight_config["protocol"],
                "comparability": g3_preflight_config["comparability"],
            },
        )
        g3_preflight_path = root / "g3-preflight-start.json"
        _atomic_json(g3_preflight_path, g3_preflight_config)
        try:
            start_run(
                g3_preflight_registry,
                g3_preflight_path,
                test_mode=True,
            )
        except HarnessError as error:
            _require(
                "raw configuration does not match the preregistered 6CQ input"
                in str(error),
                "self-test",
                f"g3 start failed for an unexpected reason before pending: {error}",
            )
        else:
            raise HarnessError(
                "self-test launched 6CQ with a non-preregistered raw configuration"
            )
        _require(
            not any((g3_preflight_registry / "pending").iterdir()),
            "self-test",
            "invalid 6CQ raw projection created a pending record",
        )

        config = json.loads(json.dumps(projection_config))

        # The test-only coordination variables are part of the sterile environment.
        config["comparability"]["runtime"]["sterile_environment_sha256"] = (
            _canonical_sha(config["launch"]["common_environment"])
        )
        comparability = config["comparability"]
        _atomic_json(registry / "epoch.json", {"schema": {"name": EPOCH_SCHEMA_NAME, "version": EPOCH_SCHEMA_VERSION}, "protocol": protocol, "comparability": comparability}, replace=True)
        config_path = root / "candidate-start.json"
        _atomic_json(config_path, config)
        for label, mutate in (
            (
                "6CP marker token omission",
                lambda value: value["candidate_marker"]["required_tokens"].remove(
                    "signature_snapshot=preserved"
                ),
            ),
            (
                "6CP receipt remap",
                lambda value: value["provenance"]["selector_receipts"][
                    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION"
                ].update({"field": "stable_negative_route"}),
            ),
            (
                "missing retained 6CL selector",
                lambda value: (
                    value["provenance"]["selectors"].pop(
                        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
                    ),
                    value["provenance"]["selector_receipts"].pop(
                        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
                    ),
                    value["launch"]["experiment_environment"].pop(
                        "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
                    ),
                ),
            ),
            (
                "integer retained 6CL selector",
                lambda value: value["provenance"]["selectors"].update(
                    {"LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE": 1}
                ),
            ),
        ):
            rejected_config = json.loads(json.dumps(projection_config))
            rejected_config["comparability"]["runtime"][
                "sterile_environment_sha256"
            ] = _canonical_sha(rejected_config["launch"]["common_environment"])
            mutate(rejected_config)
            rejected_path = root / f"rejected-{label.replace(' ', '-')}.json"
            _atomic_json(rejected_path, rejected_config)
            try:
                _validate_start_config(rejected_path)
            except HarnessError:
                pass
            else:
                raise HarnessError(f"self-test accepted {label}")
        expected_start_snapshot = _canonical_bytes(config) + b"\n"
        swapped_config = json.loads(json.dumps(config))
        swapped_config["provenance"]["record_id"] = "source-swapped-fixture"
        swapped_config["protocol"]["protocol_id"] = "source-swapped-protocol"
        swapped_config["lineage"]["parent_record_id"] = "source-swapped-parent"
        swapped_config["launch"]["common_environment"]["TEST_RECEIPT"] = (
            "source-swapped-receipt"
        )
        swapped_config["launch"]["experiment_environment"][
            "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION"
        ] = "0"
        snapshot_hook_calls = []

        def swap_source_after_preflight() -> None:
            snapshot_hook_calls.append("after-preflight")
            _atomic_json(config_path, swapped_config, replace=True)

        # Seed a valid production manifest so lineage checks admit the initial C.
        report_module = _load_report_module(report_tool)
        baseline_value = report_module._fixture_manifest(record_id="baseline-fixture", receipt="baseline-receipt", generation=0, parent=None, role="production", wall_ns=100, cumulative_ms=10000, fort_interval_ms=10, game_interval_ms=10, revision_digit="b", selector_enabled=False)
        baseline_value["protocol"] = protocol
        baseline_value["comparability"] = comparability
        baseline_dir = registry / "records" / "baseline-fixture"
        baseline_dir.mkdir()
        _atomic_json(baseline_dir / "manifest.json", baseline_value)
        baseline_binary_sha = baseline_value["provenance"]["binary_sha256"]
        baseline_value["provenance"]["binary_sha256"] = _sha_file(fake)
        _atomic_json(baseline_dir / "manifest.json", baseline_value, replace=True)
        _require(baseline_binary_sha != baseline_value["provenance"]["binary_sha256"], "self-test", "fixture binary hash did not change")

        parsed_preflight = _build_parser().parse_args(
            ["preflight", str(registry), str(config_path)]
        )
        preflight_registry_before = _tree_digest(registry)
        preflight_repo_before = _tree_digest(repo)
        preflight_config_before = _capture_stable_regular_bytes(
            config_path, purpose="self-test preflight config"
        )
        preflight_config_identity_before = _file_identity(os.lstat(config_path))
        preflight_receipt = preflight_run(
            registry,
            config_path,
            test_mode=True,
            _configuration_projection_policy=fixture_projection_policy,
        )
        _require(
            parsed_preflight.command == "preflight"
            and parsed_preflight.registry == str(registry)
            and parsed_preflight.config == str(config_path)
            and "read-only" in _build_parser().format_help()
            and preflight_receipt["schema"]
            == "lumen.persistent-performance-preflight/1"
            and preflight_receipt["status"] == "clean"
            and preflight_receipt["record_id"] == "candidate-fixture"
            and preflight_receipt["pending_count"] == 0
            and preflight_receipt["residual_process_count"] == 0
            and preflight_receipt["record_absent"] is True
            and preflight_receipt["current_baseline_sha256"] is None
            and preflight_receipt["registry_sha256"]
            == preflight_registry_before
            and _tree_digest(registry) == preflight_registry_before
            and _tree_digest(repo) == preflight_repo_before
            and _capture_stable_regular_bytes(
                config_path, purpose="self-test preflight config"
            )
            == preflight_config_before
            and _file_identity(os.lstat(config_path))
            == preflight_config_identity_before
            and not (registry / ".registry.lock").exists()
            and not any((registry / "pending").iterdir()),
            "self-test",
            "public preflight mutated state or returned an invalid receipt",
        )

        generations_root = registry / "generations"
        generations_root.mkdir()
        baseline_descriptor = {
            "generation": baseline_value["lineage"]["generation"],
            "record_id": baseline_value["provenance"]["record_id"],
            "parent_record_id": baseline_value["lineage"]["parent_record_id"],
            "manifest": "records/baseline-fixture/manifest.json",
            "manifest_sha256": _sha_file(
                baseline_dir / "manifest.json"
            ),
        }
        baseline_decision_id = "reject-g000001-candidate-fixture"
        valid_baseline_pointer = {
            "schema": {
                "name": "lumen.persistent-performance-baseline-pointer",
                "version": 1,
            },
            "protocol_id": protocol["protocol_id"],
            **baseline_descriptor,
            "decision_id": baseline_decision_id,
        }
        valid_baseline_generation = {
            "schema": {
                "name": "lumen.persistent-performance-generation",
                "version": 1,
            },
            "protocol_id": protocol["protocol_id"],
            **baseline_descriptor,
            "decision_id": baseline_decision_id,
        }
        baseline_pointer_path = registry / "current-baseline.json"
        baseline_generation_path = generations_root / "g000000.json"
        _atomic_json(baseline_pointer_path, valid_baseline_pointer)
        _atomic_json(baseline_generation_path, valid_baseline_generation)

        def assert_lineage_check_did_not_mutate(
            label: str,
            before_registry: str,
            before_repo: str,
            before_config: bytes,
            before_config_identity: Tuple[int, int, int, int, int, int],
            before_pointer_identity: Tuple[int, int, int, int, int, int],
            before_generation_identity: Tuple[int, int, int, int, int, int],
        ) -> None:
            _require(
                _tree_digest(registry) == before_registry
                and _tree_digest(repo) == before_repo
                and _capture_stable_regular_bytes(
                    config_path,
                    purpose="self-test pointer-tamper config",
                )
                == before_config
                and _file_identity(os.lstat(config_path))
                == before_config_identity
                and _file_identity(os.lstat(baseline_pointer_path))
                == before_pointer_identity
                and _file_identity(os.lstat(baseline_generation_path))
                == before_generation_identity
                and not (registry / ".registry.lock").exists()
                and not any((registry / "pending").iterdir()),
                "self-test",
                f"{label} mutated registry, repo, config, lock, or pending state",
            )

        def lineage_check_state() -> Tuple[
            str,
            str,
            bytes,
            Tuple[int, int, int, int, int, int],
            Tuple[int, int, int, int, int, int],
            Tuple[int, int, int, int, int, int],
        ]:
            return (
                _tree_digest(registry),
                _tree_digest(repo),
                _capture_stable_regular_bytes(
                    config_path,
                    purpose="self-test pointer-tamper config",
                ),
                _file_identity(os.lstat(config_path)),
                _file_identity(os.lstat(baseline_pointer_path)),
                _file_identity(os.lstat(baseline_generation_path)),
            )

        valid_pointer_state = lineage_check_state()
        pointer_preflight_receipt = preflight_run(
            registry,
            config_path,
            test_mode=True,
            _configuration_projection_policy=fixture_projection_policy,
        )
        _require(
            pointer_preflight_receipt["current_baseline_sha256"]
            == _sha_file(baseline_pointer_path),
            "self-test",
            "valid canonical baseline pointer was not bound by preflight",
        )
        assert_lineage_check_did_not_mutate(
            "valid baseline pointer preflight", *valid_pointer_state
        )

        def reject_lineage_tamper(label: str) -> None:
            for operation, check in (
                (
                    "preflight",
                    lambda: preflight_run(
                        registry,
                        config_path,
                        test_mode=True,
                        _configuration_projection_policy=(
                            fixture_projection_policy
                        ),
                    ),
                ),
                (
                    "start",
                    lambda: start_run(
                        registry,
                        config_path,
                        test_mode=True,
                        _configuration_projection_policy=(
                            fixture_projection_policy
                        ),
                    ),
                ),
            ):
                state = lineage_check_state()
                try:
                    check()
                except HarnessError as error:
                    _require(
                        "pointer" in str(error)
                        or "generation" in str(error),
                        "self-test",
                        f"{operation} rejected {label} for an unrelated reason: {error}",
                    )
                else:
                    raise HarnessError(
                        f"self-test {operation} accepted {label}"
                    )
                assert_lineage_check_did_not_mutate(
                    f"{operation} {label}", *state
                )

        for label, mutate in (
            (
                "pointer schema name tamper",
                lambda value: value["schema"].update({"name": "wrong"}),
            ),
            (
                "pointer schema version tamper",
                lambda value: value["schema"].update({"version": 2}),
            ),
            (
                "pointer protocol_id tamper",
                lambda value: value.update({"protocol_id": "wrong-protocol"}),
            ),
            (
                "pointer generation tamper",
                lambda value: value.update({"generation": 1}),
            ),
            (
                "pointer record_id tamper",
                lambda value: value.update({"record_id": "wrong-record"}),
            ),
            (
                "pointer parent_record_id tamper",
                lambda value: value.update(
                    {"parent_record_id": "wrong-parent"}
                ),
            ),
            (
                "pointer manifest tamper",
                lambda value: value.update(
                    {"manifest": "records/baseline-fixture/wrong.json"}
                ),
            ),
            (
                "pointer manifest_sha256 tamper",
                lambda value: value.update({"manifest_sha256": "0" * 64}),
            ),
            (
                "pointer decision_id tamper",
                lambda value: value.update(
                    {"decision_id": "reject-g000001-wrong-record"}
                ),
            ),
            (
                "pointer missing field",
                lambda value: value.pop("decision_id"),
            ),
            (
                "pointer extra field",
                lambda value: value.update({"unexpected": False}),
            ),
        ):
            tampered_pointer = json.loads(
                json.dumps(valid_baseline_pointer)
            )
            mutate(tampered_pointer)
            _atomic_json(
                baseline_pointer_path, tampered_pointer, replace=True
            )
            try:
                reject_lineage_tamper(label)
            finally:
                _atomic_json(
                    baseline_pointer_path,
                    valid_baseline_pointer,
                    replace=True,
                )

        detached_generation = json.loads(
            json.dumps(valid_baseline_generation)
        )
        detached_generation["decision_id"] = (
            "reject-g000001-wrong-record"
        )
        _atomic_json(
            baseline_generation_path, detached_generation, replace=True
        )
        try:
            reject_lineage_tamper("detached generation descriptor")
        finally:
            _atomic_json(
                baseline_generation_path,
                valid_baseline_generation,
                replace=True,
            )

        for label, path in (
            ("writable baseline pointer", baseline_pointer_path),
            ("writable generation descriptor", baseline_generation_path),
        ):
            path.chmod(0o644)
            try:
                reject_lineage_tamper(label)
            finally:
                path.chmod(0o444)

        outcome: Dict[str, Any] = {}
        def run_thread() -> None:
            try:
                outcome["run"] = start_run(
                    registry,
                    config_path,
                    test_mode=True,
                    _after_preflight_hook=swap_source_after_preflight,
                    _configuration_projection_policy=fixture_projection_policy,
                )
            except BaseException as error:
                outcome["error"] = error

        thread = threading.Thread(target=run_thread, daemon=True)
        thread.start()
        pending_root = registry / "pending" / "candidate-fixture"
        deadline = time.monotonic() + 10.0
        while not (pending_root / "checkpoint-observation.json").exists():
            if "error" in outcome:
                raise HarnessError(f"self-test start failed before checkpoint: {outcome['error']}")
            if time.monotonic() >= deadline:
                raise HarnessError("self-test timeout waiting for checkpoint observation")
            time.sleep(0.01)
        checkpoint_line, checkpoint_raw_line = _checkpoint_raw_line(
            pending_root / "stderr.log"
        )
        checkpoint_value = _checkpoint_from_line(
            checkpoint_line, pending_root / "stderr.log"
        )
        checkpoint_observation = _load_json(
            pending_root / "checkpoint-observation.json"
        )
        _require(
            checkpoint_observation["checkpoint"]
            != _checkpoint_json_value(checkpoint_value),
            "self-test",
            "checkpoint fixture did not reproduce the legacy Decimal-vs-float failure",
        )
        _validate_checkpoint_observation(
            checkpoint_observation,
            checkpoint_value,
            checkpoint_raw_line,
            "self-test checkpoint observation",
        )
        bad_checkpoint_observation = dict(checkpoint_observation)
        bad_checkpoint_value = dict(checkpoint_observation["checkpoint"])
        bad_checkpoint_value["cumulative_ms"] += decimal.Decimal("0.001")
        bad_checkpoint_observation["checkpoint"] = bad_checkpoint_value
        try:
            _validate_checkpoint_observation(
                bad_checkpoint_observation,
                checkpoint_value,
                checkpoint_raw_line,
                "self-test checkpoint mismatch",
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted a numeric checkpoint mismatch")
        bad_checkpoint_observation = dict(checkpoint_observation)
        bad_checkpoint_observation["line_sha256"] = "0" * 64
        try:
            _validate_checkpoint_observation(
                bad_checkpoint_observation,
                checkpoint_value,
                checkpoint_raw_line,
                "self-test checkpoint line hash",
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted a checkpoint raw-line hash mismatch")
        for label, mutate in (
            (
                "schema",
                lambda value: value.update({"schema": "unexpected/schema"}),
            ),
            (
                "extra key",
                lambda value: value.update({"unexpected": 1}),
            ),
            (
                "string duration",
                lambda value: value["checkpoint"].update(
                    {"cumulative_ms": "22990.001"}
                ),
            ),
            (
                "boolean duration",
                lambda value: value["checkpoint"].update(
                    {"partial_ms": True}
                ),
            ),
        ):
            bad_checkpoint_observation = dict(checkpoint_observation)
            bad_checkpoint_observation["checkpoint"] = dict(
                checkpoint_observation["checkpoint"]
            )
            mutate(bad_checkpoint_observation)
            try:
                _validate_checkpoint_observation(
                    bad_checkpoint_observation,
                    checkpoint_value,
                    checkpoint_raw_line,
                    f"self-test checkpoint {label}",
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test accepted checkpoint observation {label}"
                )
        published_start = pending_root / "start-config.json"
        published_config = _load_json(published_start)
        source_after_swap = _load_json(config_path)
        frozen_snapshot = _load_json(pending_root / "freeze.json")
        _require(
            snapshot_hook_calls == ["after-preflight"]
            and source_after_swap["provenance"]["record_id"]
            == "source-swapped-fixture"
            and published_start.read_bytes() == expected_start_snapshot
            and published_config["provenance"]["record_id"]
            == config["provenance"]["record_id"]
            and published_config["protocol"] == config["protocol"]
            and published_config["lineage"] == config["lineage"]
            and published_config["launch"] == config["launch"]
            and frozen_snapshot["record_id"] == config["provenance"]["record_id"]
            and frozen_snapshot["start_config_sha256"]
            == hashlib.sha256(expected_start_snapshot).hexdigest()
            and frozen_snapshot["epoch_sha256"]
            == _canonical_sha(_load_epoch(registry))
            and frozen_snapshot["lineage_sha256"]
            == _canonical_sha(config["lineage"])
            and frozen_snapshot["configuration_cache_raw_sha256"]
            == _sha_file(
                pending_root / PHASE6CP_CONFIGURATION_CACHE_RAW_NAME
            )
            and frozen_snapshot["configuration_projection_receipt_sha256"]
            == _sha_file(
                pending_root / PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME
            )
            and (
                pending_root / PHASE6CP_CONFIGURATION_CACHE_RAW_NAME
            ).read_bytes()
            == fixture_configuration_raw
            and _load_json(
                pending_root / PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME
            )
            == expected_projection_receipt
            and stat.S_IMODE(
                (
                    pending_root / PHASE6CP_CONFIGURATION_CACHE_RAW_NAME
                ).stat().st_mode
            )
            == 0o444
            and stat.S_IMODE(
                (
                    pending_root
                    / PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME
                ).stat().st_mode
            )
            == 0o444,
            "self-test",
            "post-preflight source swap escaped the authoritative start snapshot",
        )
        try:
            start_run(registry, config_path, test_mode=True)
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test admitted a second concurrent pending run")
        png1, png2, png3 = root / "one.png", root / "two.png", root / "three.png"
        _write_png(png1, b"one")
        _write_png(png2, b"two")
        _write_png(png3, b"three")
        anchor_scene(pending_root, "fort_mode", sorted(FORT_ANCHOR_ATTESTATIONS), [png1])
        _wait_for_endpoint(pending_root / "stderr.log", 2640)
        complete_scene(pending_root, "fort_mode", sorted(FORT_COMPLETE_ATTESTATIONS), [png2])
        _wait_for_endpoint(pending_root / "stderr.log", 3600)
        anchor_scene(pending_root, "in_game", sorted(GAME_ANCHOR_ATTESTATIONS), [png1, png2], timer_before=100, timer_after=101)
        _wait_for_endpoint(pending_root / "stderr.log", 3840)
        complete_scene(pending_root, "in_game", sorted(GAME_COMPLETE_ATTESTATIONS), [png3])
        thread.join(timeout=10)
        _require(not thread.is_alive() and "error" not in outcome, "self-test", f"start failed: {outcome.get('error')}")
        freeze_path = pending_root / "freeze.json"
        freeze_value = _load_json(freeze_path)

        def seal_fixture(
            target: pathlib.Path, attestation: str, **kwargs: Any
        ) -> pathlib.Path:
            kwargs.setdefault(
                "_configuration_projection_policy", fixture_projection_policy
            )
            return seal_run(target, attestation, **kwargs)

        mismatched_mode_freeze = dict(freeze_value)
        mismatched_mode_freeze["test_mode"] = False
        _atomic_json(freeze_path, mismatched_mode_freeze, replace=True)
        try:
            seal_fixture(
                pending_root,
                "CONFIRM_NO_GLITCH_OBSERVED",
                test_mode=True,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test sealed a test launch with a non-test freeze mode"
            )
        finally:
            _atomic_json(freeze_path, freeze_value, replace=True)

        for field, replacement in (
            ("record_id", "source-swapped-fixture"),
            ("epoch_sha256", "0" * 64),
            ("lineage_sha256", "0" * 64),
        ):
            tampered_freeze = dict(freeze_value)
            tampered_freeze[field] = replacement
            _atomic_json(freeze_path, tampered_freeze, replace=True)
            try:
                seal_fixture(
                    pending_root,
                    "CONFIRM_NO_GLITCH_OBSERVED",
                    test_mode=True,
                )
            except HarnessError:
                pass
            else:
                raise HarnessError(
                    f"self-test seal accepted mismatched freeze field {field}"
                )
            finally:
                _atomic_json(freeze_path, freeze_value, replace=True)
        _require(
            (pending_root / "work/_SaveData/SAVEDATA0/MainData.0").read_bytes()
            == b"autosave"
            and (save / "SAVEDATA0/MainData.0").read_bytes() == b"main"
            and stat.S_IMODE((save / "SAVEDATA0/MainData.0").stat().st_mode)
            == 0o444,
            "self-test",
            "read-only template was not cloned writable without mutation",
        )
        fort_receipt = _load_json(pending_root / "anchors/fort_mode.json")
        bad_attestation = dict(fort_receipt)
        bad_attestation["attestations"] = []
        try:
            _validate_anchor_value(
                pending_root, "fort_mode", bad_attestation, final=True
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted a forged Fort attestation")
        evidence_path = pending_root / fort_receipt["anchor_evidence"][0]["file"]
        evidence_payload = evidence_path.read_bytes()
        evidence_path.chmod(0o644)
        evidence_path.write_bytes(evidence_payload + b"tamper")
        try:
            _validate_anchor_value(
                pending_root, "fort_mode", fort_receipt, final=True
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted mutated PNG evidence")
        evidence_path.write_bytes(evidence_payload)
        evidence_path.chmod(0o444)
        # Preserve a full positive seal for the ordinary current-harness path;
        # the original pending tree remains available for the legacy recovery
        # cases below.
        normal_registry = root / "normal-registry"
        shutil.copytree(registry, normal_registry)
        normal_pending = (
            normal_registry / "pending" / config["provenance"]["record_id"]
        )
        normal_freeze_path = normal_pending / "freeze.json"
        normal_freeze = dict(_load_json(normal_freeze_path))
        normal_freeze["registry_root"] = str(normal_registry.resolve())
        _atomic_json(normal_freeze_path, normal_freeze, replace=True)
        normal_manifest_path = seal_fixture(
            normal_pending,
            "CONFIRM_NO_GLITCH_OBSERVED",
            test_mode=True,
        )
        report_module.load_manifest(
            normal_manifest_path,
            _projection_contract=fixture_projection_policy,
        )
        spurious_migration_path = (
            pending_root / CHECKPOINT_DECIMAL_MIGRATION_RECEIPT_NAME
        )
        _atomic_json(spurious_migration_path, {"unexpected": True})
        try:
            seal_fixture(
                pending_root,
                "CONFIRM_NO_GLITCH_OBSERVED",
                test_mode=True,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted unindexed migration evidence on a normal seal"
            )
        finally:
            spurious_migration_path.unlink()
        raw_evidence_before_migration = {
            "stderr": _sha_file(pending_root / "stderr.log"),
            "observation": _sha_file(
                pending_root / "checkpoint-observation.json"
            ),
        }
        try:
            seal_fixture(
                pending_root,
                "CONFIRM_NO_GLITCH_OBSERVED",
                test_mode=True,
                checkpoint_decimal_migration=True,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted migration for the current frozen harness"
            )
        ineligible_freeze = dict(freeze_value)
        ineligible_freeze["harness_sha256"] = "f" * 64
        _atomic_json(freeze_path, ineligible_freeze, replace=True)
        try:
            create_checkpoint_decimal_migration(
                pending_root, CHECKPOINT_DECIMAL_MIGRATION_ATTESTATION
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted a non-allowlisted frozen harness migration"
            )
        migration_freeze = dict(freeze_value)
        migration_freeze["harness_sha256"] = (
            CHECKPOINT_DECIMAL_MIGRATION_LEGACY_HARNESS_SHA256
        )
        _atomic_json(freeze_path, migration_freeze, replace=True)
        try:
            seal_fixture(
                pending_root,
                "CONFIRM_NO_GLITCH_OBSERVED",
                test_mode=True,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted a legacy frozen harness without migration"
            )
        try:
            seal_fixture(
                pending_root,
                "CONFIRM_NO_GLITCH_OBSERVED",
                test_mode=True,
                checkpoint_decimal_migration=True,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted a legacy frozen harness without a receipt"
            )
        try:
            create_checkpoint_decimal_migration(pending_root, "CONFIRM_WRONG")
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted a checkpoint migration without attestation"
            )
        migration_seal_harness_path = (
            pending_root / CHECKPOINT_DECIMAL_MIGRATION_SEAL_HARNESS_NAME
        )
        current_harness_payload = pathlib.Path(__file__).resolve().read_bytes()
        # Simulate interruption after the first of the two immutable writes.
        _atomic_bytes(
            migration_seal_harness_path, current_harness_payload, mode=0o444
        )
        migration_receipt_path = create_checkpoint_decimal_migration(
            pending_root, CHECKPOINT_DECIMAL_MIGRATION_ATTESTATION
        )
        migration_receipt = _load_json(migration_receipt_path)
        # A byte-identical retry is idempotent, including the inverse partial
        # state in which the receipt exists but the harness snapshot is absent.
        migration_seal_harness_path.unlink()
        _require(
            create_checkpoint_decimal_migration(
                pending_root, CHECKPOINT_DECIMAL_MIGRATION_ATTESTATION
            )
            == migration_receipt_path
            and migration_seal_harness_path.read_bytes()
            == current_harness_payload,
            "self-test",
            "checkpoint migration did not resume an exact partial publication",
        )
        _require(
            create_checkpoint_decimal_migration(
                pending_root, CHECKPOINT_DECIMAL_MIGRATION_ATTESTATION
            )
            == migration_receipt_path,
            "self-test",
            "checkpoint migration exact retry was not idempotent",
        )
        migration_seal_harness_path.chmod(0o644)
        migration_seal_harness_path.write_bytes(b"conflict\n")
        try:
            create_checkpoint_decimal_migration(
                pending_root, CHECKPOINT_DECIMAL_MIGRATION_ATTESTATION
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted conflicting checkpoint migration evidence"
            )
        finally:
            migration_seal_harness_path.write_bytes(current_harness_payload)
            migration_seal_harness_path.chmod(0o444)
        tampered_migration_receipt = dict(migration_receipt)
        tampered_migration_receipt["checkpoint_observation_sha256"] = "0" * 64
        _atomic_json(
            migration_receipt_path, tampered_migration_receipt, replace=True
        )
        try:
            seal_fixture(
                pending_root,
                "CONFIRM_NO_GLITCH_OBSERVED",
                test_mode=True,
                checkpoint_decimal_migration=True,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError(
                "self-test accepted a checkpoint migration receipt binding mismatch"
            )
        finally:
            _atomic_json(migration_receipt_path, migration_receipt, replace=True)
        atomic_debris = pending_root / ".migration.tmp.interrupted"
        _atomic_bytes(atomic_debris, b"orphan\n")
        try:
            seal_fixture(
                pending_root,
                "CONFIRM_NO_GLITCH_OBSERVED",
                test_mode=True,
                checkpoint_decimal_migration=True,
            )
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test sealed unindexed atomic staging debris")
        finally:
            atomic_debris.unlink()
        manifest_path = seal_fixture(
            pending_root,
            "CONFIRM_NO_GLITCH_OBSERVED",
            test_mode=True,
            checkpoint_decimal_migration=True,
        )
        sealed_candidate = report_module.load_manifest(
            manifest_path,
            _projection_contract=fixture_projection_policy,
        )
        sealed_baseline = report_module.load_manifest(baseline_dir / "manifest.json")
        compatibility_report = report_module.build_report(sealed_baseline, sealed_candidate)
        _require(
            compatibility_report["schema"]
            == {"name": "lumen.persistent-performance-report", "version": 1}
            and compatibility_report["records"]["candidate"]["manifest_sha256"]
            == sealed_candidate.content_sha256,
            "self-test",
            "sealed manifest is not byte-compatible with report/1",
        )
        migration_artifacts = sealed_candidate.provenance["artifacts_sha256"]
        _require(
            migration_artifacts["configuration_cache_raw"]
            == _sha_file(
                manifest_path.parent / PHASE6CP_CONFIGURATION_CACHE_RAW_NAME
            )
            and migration_artifacts["configuration_projection_receipt"]
            == _sha_file(
                manifest_path.parent
                / PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME
            )
            and (
                manifest_path.parent / PHASE6CP_CONFIGURATION_CACHE_RAW_NAME
            ).read_bytes()
            == fixture_configuration_raw
            and _load_json(
                manifest_path.parent
                / PHASE6CP_CONFIGURATION_PROJECTION_RECEIPT_NAME
            )
            == expected_projection_receipt,
            "self-test",
            "sealed manifest did not bind the configuration projection artifacts",
        )
        _require(
            migration_artifacts["checkpoint_decimal_migration_receipt"]
            == _sha_file(manifest_path.parent / "checkpoint-decimal-migration.json")
            and migration_artifacts["checkpoint_decimal_migration_freeze"]
            == _sha_file(manifest_path.parent / "freeze.json")
            and migration_artifacts["checkpoint_decimal_migration_seal_harness"]
            == _sha_file(
                manifest_path.parent
                / CHECKPOINT_DECIMAL_MIGRATION_SEAL_HARNESS_NAME
            )
            == _sha_file(pathlib.Path(__file__).resolve()),
            "self-test",
            "sealed manifest did not bind all migration artifacts",
        )
        _require(
            _sha_file(manifest_path.parent / "stderr.log")
            == raw_evidence_before_migration["stderr"]
            and _sha_file(
                manifest_path.parent / "checkpoint-observation.json"
            )
            == raw_evidence_before_migration["observation"],
            "self-test",
            "checkpoint migration changed captured raw evidence",
        )
        _require(manifest_path.parent.parent == (registry / "records").resolve(), "self-test", "sealed record was not atomically published")

        try:
            anchor_scene(manifest_path.parent, "fort_mode", sorted(FORT_ANCHOR_ATTESTATIONS), [png1])
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted an overwritten/sealed Fort anchor")

        immutable = root / "atomic-no-clobber.txt"
        _atomic_bytes(immutable, b"first\n")
        try:
            _atomic_bytes(immutable, b"second\n")
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test allowed immutable atomic overwrite")
        _require(
            immutable.read_bytes() == b"first\n",
            "self-test",
            "failed atomic publish changed the destination",
        )
        receipt_read, receipt_write = os.pipe()
        try:
            try:
                _read_pre_exec_receipt(receipt_read, timeout_seconds=0.01)
            except HarnessError:
                pass
            else:
                raise HarnessError("self-test accepted a missing pre-exec receipt")
        finally:
            os.close(receipt_read)
            os.close(receipt_write)

        malformed_root = root / "malformed-supervision"
        malformed_root.mkdir()
        malformed = subprocess.Popen(
            (
                sys.executable,
                "-c",
                "import sys,time; print('[FRAME_CHECKPOINT] malformed', file=sys.stderr, flush=True); time.sleep(30)",
            ),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        supervision_started = time.monotonic()
        try:
            _supervise_process(malformed, malformed_root, _uptime_raw_ns())
        except HarnessError:
            pass
        else:
            raise HarnessError("self-test accepted a malformed live checkpoint")
        _require(
            malformed.poll() is not None
            and time.monotonic() - supervision_started < 10.0
            and (malformed_root / "launch-failure.json").is_file(),
            "self-test",
            "malformed checkpoint did not terminate/reap and archive failure",
        )


def _wait_for_endpoint(stderr_path: pathlib.Path, endpoint: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            if endpoint in _parse_frames(stderr_path):
                return
        except HarnessError:
            pass
        time.sleep(0.01)
    raise HarnessError(f"timeout waiting for p{endpoint}")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="run and seal one persistent macOS performance record; never sends input")
    parser.add_argument("--self-test", action="store_true", help="run deterministic live and fail-closed fixtures")
    subparsers = parser.add_subparsers(dest="command")
    preflight = subparsers.add_parser(
        "preflight",
        help="validate one start read-only without claiming pending state or launching",
    )
    preflight.add_argument("registry")
    preflight.add_argument("config")
    start = subparsers.add_parser("start", help="launch one frozen run and supervise it until exit")
    start.add_argument("registry")
    start.add_argument("config")
    anchor = subparsers.add_parser("anchor", help="record a manual Fort/in-game announcement")
    anchor.add_argument("run")
    anchor.add_argument("scene", choices=("fort_mode", "in_game"))
    anchor.add_argument("--attest", action="append", required=True)
    anchor.add_argument("--evidence", action="append", required=True)
    anchor.add_argument("--timer-before", type=int)
    anchor.add_argument("--timer-after", type=int)
    complete = subparsers.add_parser("complete", help="close one announcement measurement window")
    complete.add_argument("run")
    complete.add_argument("scene", choices=("fort_mode", "in_game"))
    complete.add_argument("--attest", action="append", required=True)
    complete.add_argument("--evidence", action="append", required=True)
    migrate = subparsers.add_parser(
        "migrate-checkpoint-decimal",
        help="add an immutable no-raw-rewrite receipt for the one eligible legacy seal bug",
    )
    migrate.add_argument("run")
    migrate.add_argument("--attest", required=True)
    seal = subparsers.add_parser("seal", help="validate and immutably publish a completed run")
    seal.add_argument("run")
    seal.add_argument("--attest", required=True)
    seal.add_argument(
        "--checkpoint-decimal-migration",
        action="store_true",
        help="accept only the explicit receipt for the allowlisted frozen harness bug",
    )
    return parser


def _main(argv: Sequence[str]) -> int:
    if argv and argv[0] == "_exec-child":
        _require(len(argv) >= 3 and argv[1].isdigit(), "exec wrapper", "malformed private exec-child invocation")
        return _exec_child(int(argv[1]), argv[2], argv[3:])
    parser = _build_parser()
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        _require(arguments.command is None, "arguments", "--self-test accepts no command")
        _self_test()
        print("persistent-performance-harness-self-test: PASS")
        return 0
    if arguments.command is None:
        parser.error("a command is required")
    if arguments.command == "preflight":
        print(
            json.dumps(
                preflight_run(
                    pathlib.Path(arguments.registry),
                    pathlib.Path(arguments.config),
                ),
                sort_keys=True,
                separators=(",", ":"),
            )
        )
    elif arguments.command == "start":
        print(start_run(pathlib.Path(arguments.registry), pathlib.Path(arguments.config)))
    elif arguments.command == "anchor":
        print(anchor_scene(pathlib.Path(arguments.run), arguments.scene, arguments.attest, [pathlib.Path(item) for item in arguments.evidence], timer_before=arguments.timer_before, timer_after=arguments.timer_after))
    elif arguments.command == "complete":
        print(complete_scene(pathlib.Path(arguments.run), arguments.scene, arguments.attest, [pathlib.Path(item) for item in arguments.evidence]))
    elif arguments.command == "migrate-checkpoint-decimal":
        print(
            create_checkpoint_decimal_migration(
                pathlib.Path(arguments.run), arguments.attest
            )
        )
    elif arguments.command == "seal":
        print(
            seal_run(
                pathlib.Path(arguments.run),
                arguments.attest,
                checkpoint_decimal_migration=arguments.checkpoint_decimal_migration,
            )
        )
    else:
        parser.error("unsupported command")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except HarnessError as error:
        print(f"persistent-performance-harness: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
