#!/usr/bin/env python3
"""Immutable lineage and atomic baseline pointer for persistent performance.

This is the only tool allowed to change ``current-baseline.json``.  It accepts
only reports that can be canonically regenerated from two sealed manifests
by ``persistent_performance_report.py``.  Every decision first imports the
validated report into the registry's immutable content-addressed archive;
producing a report never promotes a candidate.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import stat
import sys
import tempfile
import threading
import time
from typing import Any, Dict, Mapping, Optional, Sequence, Tuple


EPOCH_SCHEMA_NAME = "lumen.persistent-performance-epoch"
EPOCH_SCHEMA_VERSION = 1
POINTER_SCHEMA_NAME = "lumen.persistent-performance-baseline-pointer"
POINTER_SCHEMA_VERSION = 1
GENERATION_SCHEMA_NAME = "lumen.persistent-performance-generation"
GENERATION_SCHEMA_VERSION = 1
DECISION_SCHEMA_NAME = "lumen.persistent-performance-decision"
DECISION_SCHEMA_VERSION = 1
REPORT_SCHEMA_NAME = "lumen.persistent-performance-report"
REPORT_SCHEMA_VERSION = 1
REPORT_MAX_BYTES = 4 * 1024 * 1024
SAFE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
HEX_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
CHECKPOINT_DECIMAL_MIGRATION_ARTIFACTS = {
    "checkpoint_decimal_migration_receipt": "checkpoint-decimal-migration.json",
    "checkpoint_decimal_migration_freeze": "freeze.json",
    "checkpoint_decimal_migration_seal_harness": "checkpoint-decimal-migration-seal-harness.py",
}
CONFIGURATION_PROJECTION_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION"
)
CONFIGURATION_PROJECTION_STABLE_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"
)
CONFIGURATION_PROJECTION_ARTIFACTS = {
    "configuration_projection_receipt": "configuration-projection.json",
    "configuration_cache_raw": "configuration-cache.raw",
}
CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": "6cp-c-guest-memory-operation-snapshot-elision-g000002",
    "generation": 2,
    "parent_record_id": "6cl-c-stable-negative-g000001",
    "original_role": "candidate",
    "candidate_selector": CONFIGURATION_PROJECTION_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
    },
}
PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_SCALAR_MEMORY_FALLTHROUGH_ADMISSION"
)
PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": "6cq-c-scalar-memory-fallthrough-admission-g000003",
    "generation": 3,
    "parent_record_id": (
        "6cp-c-guest-memory-operation-snapshot-elision-g000002"
    ),
    "original_role": "candidate",
    "candidate_selector": PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR: True,
    },
    "raw_bytes": 46_394,
    "raw_sha256": (
        "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
    ),
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": 4_788,
    "restore_operations_contract_sha256": (
        "ee36e477af8d1218a7b254d078edf9841a4a233597790bba8bcffa0177c3c04c"
    ),
    "restored_bytes": 49_567,
    "restored_sha256": (
        "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35"
    ),
    "projected_bytes": 49_321,
    "projected_sha256": (
        "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a"
    ),
    "projection_configured": True,
}
PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR = (
    "LUMEN_X64_FAST_BLOCK_DEAD_RFLAGS_ELISION"
)
PHASE6CS_CONFIGURATION_RESTORE_CONTRACT_PREFIX = (
    b"lumen.phase6cs.configuration-restore-operations.v1\0"
)
PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": "6cs-c-dead-rflags-elision-g000003",
    "generation": 3,
    "parent_record_id": (
        "6cp-c-guest-memory-operation-snapshot-elision-g000002"
    ),
    "original_role": "candidate",
    "candidate_selector": PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR: True,
    },
    "raw_bytes": 46_394,
    "raw_sha256": (
        "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
    ),
    "projection_kind": "phase6cs-restore-eight-then-strip-v1",
    "attestation": "CONFIRM_PHASE6CS_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
    "policy": "phase6cs-cmake-cache-production-v1",
    "mode": (
        "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-"
        "phase6cm-phase6co-v1"
    ),
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": 4_788,
    "restore_operations_contract_sha256": (
        "15a48324c22c27e8959a0d8706965110199d7ca1d8203f10d94fd823fe6e57f9"
    ),
    "restored_bytes": 49_567,
    "restored_sha256": (
        "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35"
    ),
    "phase6cm_block_sha256": (
        "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e"
    ),
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
    "projection_configured": True,
}
PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_DENSE_DIRECTORY"
)
PHASE6DA_CONFIGURATION_RESTORE_CONTRACT_PREFIX = (
    b"lumen.phase6da.configuration-restore-operations.v1\0"
)
PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": "6da-c-fast-block-dense-directory-g000003",
    "generation": 3,
    "parent_record_id": (
        "6cp-c-guest-memory-operation-snapshot-elision-g000002"
    ),
    "original_role": "candidate",
    "candidate_selector": PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
    },
    "raw_bytes": 46_394,
    "raw_sha256": (
        "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
    ),
    "projection_kind": "phase6da-restore-eight-then-strip-v1",
    "attestation": "CONFIRM_PHASE6DA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION",
    "policy": "phase6da-cmake-cache-production-v1",
    "mode": (
        "restore-exact-inert-fresh-cache-delta-then-strip-exact-inactive-"
        "phase6cm-phase6co-v1"
    ),
    "restore_operation_count": 8,
    "restore_operations_contract_bytes": 4_788,
    "restore_operations_contract_sha256": (
        "ca3126b513628e42a655bff5649ab3f7d4c915d89c2f881e68b07b023cea1c14"
    ),
    "restored_bytes": 49_567,
    "restored_sha256": (
        "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35"
    ),
    "phase6cm_block_sha256": (
        "c859ef6ee8e50cf3b9f2a4ec013e8eda06eb2affdb185afa71db05c1478f6f8e"
    ),
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
    "projection_configured": True,
}
CONFIGURATION_PROJECTION_CONTRACTS = (
    CONFIGURATION_PROJECTION_CONTRACT,
    PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT,
    PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT,
    PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT,
)

ALC_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_ARM_LOADING_COMPOSITE_RECEIPT"
)
ALC_CANDIDATE_RECORD_ID = "alc-c-arm-loading-composite-g000003"
ALC_MARKER_PREFIX = "[A64_ARM_LOADING_COMPOSITE_FINAL]"
ALC_MARKER_TOKENS = (
    "purge=positive",
    "adaptive_snapshot=positive",
    "omit_cpu_frame_pointer=positive",
    "ifsm_rollback=positive",
    "requested=positive",
    "status=clean",
    "coverage=positive",
)
ALC_MARKER_LINE = " ".join((ALC_MARKER_PREFIX, *ALC_MARKER_TOKENS))
ALC_CONFIGURATION_PROJECTION_CONTRACT = {
    "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
    "record_id": ALC_CANDIDATE_RECORD_ID,
    "generation": 3,
    "parent_record_id": "ifsm-c-fast-block-inline-fixed-signature-match-g000002",
    "original_role": "candidate",
    "candidate_selector": ALC_CANDIDATE_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
        "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": True,
        ALC_CANDIDATE_SELECTOR: True,
    },
    "raw_bytes": 46_749,
    "raw_sha256": (
        "3d42b0edc9be9efa0156a02400de50ae4afb127ca30b7a74df0a36c1e2291e71"
    ),
    "projection_kind": "alc-strip-three-build-options-v1",
    "attestation": (
        "CONFIRM_ALC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
    ),
    "policy": "alc-cmake-cache-production-v1",
    "mode": "strip-exact-active-alc-build-options-v1",
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
    "projection_configured": True,
    "required_receipt_fields": {
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
    "candidate_marker": {
        "required": True,
        "expected_count": 1,
        "observed_count": 1,
        "status": "clean",
        "coverage_required": True,
        "coverage_positive": True,
        "line": ALC_MARKER_LINE,
    },
}
CONFIGURATION_PROJECTION_CONTRACTS += (
    ALC_CONFIGURATION_PROJECTION_CONTRACT,
)
MSORE_CANDIDATE_SELECTOR = "LUMEN_A64_METAL_STATE_ONLY_RECORD_ELISION"
MSFC_COMPANION_SELECTOR = "LUMEN_A64_METAL_SHADER_FUNCTION_CACHE"
MDSSC_COMPANION_SELECTOR = "LUMEN_A64_METAL_DEPTH_STENCIL_STATE_CACHE"
METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID = (
    "msore-msfc-mdssc-c-metal-runtime-bundle-g000004"
)
METAL_RUNTIME_BUNDLE_SELECTORS = frozenset(
    {
        MSORE_CANDIDATE_SELECTOR,
        MSFC_COMPANION_SELECTOR,
        MDSSC_COMPANION_SELECTOR,
    }
)
MSORE_MARKER_PREFIX = "[A64_METAL_STATE_ONLY_RECORD_ELISION_FINAL]"
MSORE_MARKER_TOKENS = (
    "requested=positive",
    "effective=positive",
    "state_only=positive",
    "snapshot_consumed=positive",
    "status=clean",
    "coverage=positive",
)
MSFC_MARKER_PREFIX = "[A64_METAL_SHADER_FUNCTION_CACHE_FINAL]"
MSFC_MARKER_TOKENS = (
    "lookup=positive",
    "hit=positive",
    "compile=positive",
    "requested=positive",
    "status=clean",
    "coverage=positive",
)
MDSSC_MARKER_PREFIX = "[A64_METAL_DEPTH_STENCIL_STATE_CACHE_FINAL]"
MDSSC_MARKER_TOKENS = (
    "requested=positive",
    "compiled=positive",
    "lookup=positive",
    "hit=positive",
    "miss=positive",
    "create_match=positive",
    "status=clean",
    "coverage=positive",
)
MSORE_MARKER_LINE = " ".join((MSORE_MARKER_PREFIX, *MSORE_MARKER_TOKENS))
MSFC_MARKER_LINE = " ".join((MSFC_MARKER_PREFIX, *MSFC_MARKER_TOKENS))
MDSSC_MARKER_LINE = " ".join(
    (MDSSC_MARKER_PREFIX, *MDSSC_MARKER_TOKENS)
)
# Generation 4 adds runtime-only Metal selectors to the ALC binary.  The
# projection portion deliberately reuses the exact ALC CMake-cache contract.
METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT = {
    **ALC_CONFIGURATION_PROJECTION_CONTRACT,
    "record_id": METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
    "generation": 4,
    "parent_record_id": ALC_CANDIDATE_RECORD_ID,
    "candidate_selector": MSORE_CANDIDATE_SELECTOR,
    "selector_bundle": {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
        CONFIGURATION_PROJECTION_SELECTOR: True,
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
        "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH": True,
        ALC_CANDIDATE_SELECTOR: True,
        MSFC_COMPANION_SELECTOR: True,
        MDSSC_COMPANION_SELECTOR: True,
        MSORE_CANDIDATE_SELECTOR: True,
    },
    "attestation": (
        "CONFIRM_MSORE_MSFC_MDSSC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
    ),
    "policy": "msore-msfc-mdssc-cmake-cache-production-v1",
    "required_receipt_fields": {
        **ALC_CONFIGURATION_PROJECTION_CONTRACT["required_receipt_fields"],
        "metal_shader_function_cache_requested": "1",
        "metal_shader_function_cache": "1",
        "metal_depth_stencil_state_cache_requested": "1",
        "metal_depth_stencil_state_cache": "1",
        "metal_state_only_record_elision_requested": "1",
        "metal_state_only_record_elision": "1",
    },
    "candidate_marker": {
        "required": True,
        "expected_count": 1,
        "observed_count": 1,
        "status": "clean",
        "coverage_required": True,
        "coverage_positive": True,
        "line": MSORE_MARKER_LINE,
    },
}
CONFIGURATION_PROJECTION_CONTRACTS += (
    METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT,
)

PBA_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_PROFITABLE_BLOCK_ADMISSION"
)
NODA_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_DYNAREC_NATIVE_ONLY_ADMISSION"
)
NODA_CANDIDATE_RECORD_ID = (
    "noda-c-a64-dynarec-native-only-admission-g000005"
)
NODA_MARKER_PREFIX = "[A64_NATIVE_ONLY_ADMISSION_FINAL]"
NODA_MARKER_TOKENS = (
    "effective=positive",
    "ordinary_routed=positive",
    "native_probe=bypassed",
    "native_only_executed=positive",
    "class=fastblock_primary_native_only_dynarec",
    "status=clean",
    "coverage=positive",
)
NODA_MARKER_LINE = " ".join((NODA_MARKER_PREFIX, *NODA_MARKER_TOKENS))
NODA_SUPERSEDED_6CL_MARKER_PREFIX = "[A64_STABLE_NEGATIVE_ROUTE_FINAL]"
NODA_SUPERSEDED_6CL_MARKER_LINE = (
    "[A64_STABLE_NEGATIVE_ROUTE_FINAL] classified=inapplicable "
    "live_mismatch=none status=clean coverage=superseded "
    "class=native_only_admission"
)
NODA_RETAINED_6CP_MARKER_PREFIX = (
    "[A64_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION_FINAL]"
)
NODA_RETAINED_6CP_MARKER_LINE = (
    "[A64_GUEST_MEMORY_OPERATION_SNAPSHOT_ELISION_FINAL] elided=positive "
    "signature_snapshot=preserved status=clean coverage=positive"
)
NODA_RETAINED_6DA_MARKER_PREFIX = "[A64_FAST_BLOCK_DENSE_DIRECTORY_FINAL]"
NODA_RETAINED_6DA_MARKER_LINE = (
    "[A64_FAST_BLOCK_DENSE_DIRECTORY_FINAL] lookup=positive hit=positive "
    "mismatch=none status=clean coverage=positive"
)
NODA_RETAINED_IFSM_MARKER_PREFIX = (
    "[A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH_FINAL]"
)
NODA_RETAINED_IFSM_MARKER_LINE = (
    "[A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH_FINAL] "
    "requested=positive effective=positive compared=positive "
    "status=clean coverage=positive"
)
PBA_MARKER_PREFIX = "[A64_PROFITABLE_BLOCK_ADMISSION_FINAL]"
NODA_REQUIRED_MARKER_LINES = {
    NODA_MARKER_PREFIX: NODA_MARKER_LINE,
    NODA_SUPERSEDED_6CL_MARKER_PREFIX: NODA_SUPERSEDED_6CL_MARKER_LINE,
    NODA_RETAINED_6CP_MARKER_PREFIX: NODA_RETAINED_6CP_MARKER_LINE,
    NODA_RETAINED_6DA_MARKER_PREFIX: NODA_RETAINED_6DA_MARKER_LINE,
    NODA_RETAINED_IFSM_MARKER_PREFIX: NODA_RETAINED_IFSM_MARKER_LINE,
    ALC_MARKER_PREFIX: ALC_MARKER_LINE,
    MSFC_MARKER_PREFIX: MSFC_MARKER_LINE,
    MDSSC_MARKER_PREFIX: MDSSC_MARKER_LINE,
    MSORE_MARKER_PREFIX: MSORE_MARKER_LINE,
}
# Generation 5 changes only runtime routing.  It reuses the exact projected
# g4 build, requires every retained g4 selector, and names the single semantic
# exception: 6CL remains requested by the launch but is superseded by NODA.
NODA_CONFIGURATION_PROJECTION_CONTRACT = {
    **METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT,
    "record_id": NODA_CANDIDATE_RECORD_ID,
    "generation": 5,
    "parent_record_id": METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID,
    "candidate_selector": NODA_CANDIDATE_SELECTOR,
    "selector_bundle": {
        **METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT[
            "selector_bundle"
        ],
        NODA_CANDIDATE_SELECTOR: True,
    },
    "effective_selector_bundle": {
        **METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT[
            "selector_bundle"
        ],
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: False,
        NODA_CANDIDATE_SELECTOR: True,
    },
    "attestation": (
        "CONFIRM_NODA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
    ),
    "policy": "noda-cmake-cache-production-v1",
    "required_receipt_fields": {
        **METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT[
            "required_receipt_fields"
        ],
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
    },
    "candidate_marker": {
        "required": True,
        "expected_count": 1,
        "observed_count": 1,
        "status": "clean",
        "coverage_required": True,
        "coverage_positive": True,
        "line": NODA_MARKER_LINE,
    },
}
CONFIGURATION_PROJECTION_CONTRACTS += (
    NODA_CONFIGURATION_PROJECTION_CONTRACT,
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
PHASEIFSM_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH"
)
PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID = (
    "ifsm-c-fast-block-inline-fixed-signature-match-g000002"
)
FCFOS_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION"
)
FCFOS_DIRECT_CANDIDATE_RECORD_ID = (
    "fcfos-c-fast-block-callback-free-ordinary-specialization-g000003"
)
FCFOS_MARKER_PREFIX = (
    "[A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION_FINAL]"
)
FCFOS_MARKER_TOKENS = (
    "requested=positive",
    "effective=positive",
    "reentry_safe=positive",
    "snapshot_elision=positive",
    "status=clean",
    "coverage=positive",
)
FCFOS_MARKER_LINE = " ".join(
    (FCFOS_MARKER_PREFIX, *FCFOS_MARKER_TOKENS)
)
UFSB_CANDIDATE_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING"
)
IMSE_COMPANION_SELECTOR = (
    "LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION"
)
UFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID = (
    "ufsb-imse-c-fast-block-dispatch-bundle-g000003"
)
UFSB_PRIMARY_MARKER_PREFIX = (
    "[A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING_FINAL]"
)
UFSB_PRIMARY_MARKER_TOKENS = (
    "dependency=positive",
    "compared=positive",
    "chunks=positive",
    "status=clean",
    "coverage=positive",
)
IMSE_COMPANION_MARKER_PREFIX = (
    "[A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION_FINAL]"
)
IMSE_COMPANION_MARKER_TOKENS = (
    "requested=positive",
    "effective=positive",
    "elided=positive",
    "status=clean",
    "coverage=positive",
)
UFSB_PRIMARY_MARKER_LINE = " ".join(
    (UFSB_PRIMARY_MARKER_PREFIX, *UFSB_PRIMARY_MARKER_TOKENS)
)
IMSE_COMPANION_MARKER_LINE = " ".join(
    (IMSE_COMPANION_MARKER_PREFIX, *IMSE_COMPANION_MARKER_TOKENS)
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
PHASE6DA_DIRECT_CONTRACTS = (
    {
        "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
        "record_id": PHASE6DA_DIRECT_BASELINE_RECORD_ID,
        "generation": 0,
        "parent_record_id": None,
        "original_role": "production",
        "os": {
            "name": "macOS",
            "version": PHASE6DA_DIRECT_OS_VERSION,
            "build": PHASE6DA_DIRECT_OS_BUILD,
            "policy": PHASE6DA_DIRECT_OS_POLICY,
        },
        "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
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
        "os": {
            "name": "macOS",
            "version": PHASE6DA_DIRECT_OS_VERSION,
            "build": PHASE6DA_DIRECT_OS_BUILD,
            "policy": PHASE6DA_DIRECT_OS_POLICY,
        },
        "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
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
        "os": {
            "name": "macOS",
            "version": PHASE6DA_DIRECT_OS_VERSION,
            "build": PHASE6DA_DIRECT_OS_BUILD,
            "policy": PHASE6DA_DIRECT_OS_POLICY,
        },
        "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
            PHASEIFSM_CANDIDATE_SELECTOR: True,
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
        "record_id": UFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID,
        "generation": 3,
        "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
        "original_role": "candidate",
        "os": {
            "name": "macOS",
            "version": PHASE6DA_DIRECT_OS_VERSION,
            "build": PHASE6DA_DIRECT_OS_BUILD,
            "policy": PHASE6DA_DIRECT_OS_POLICY,
        },
        "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
        "primary_selector": UFSB_CANDIDATE_SELECTOR,
        "companion_selectors": frozenset({IMSE_COMPANION_SELECTOR}),
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
            PHASEIFSM_CANDIDATE_SELECTOR: True,
            UFSB_CANDIDATE_SELECTOR: True,
            IMSE_COMPANION_SELECTOR: True,
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
        "record_id": FCFOS_DIRECT_CANDIDATE_RECORD_ID,
        "generation": 3,
        "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
        "original_role": "candidate",
        "os": {
            "name": "macOS",
            "version": PHASE6DA_DIRECT_OS_VERSION,
            "build": PHASE6DA_DIRECT_OS_BUILD,
            "policy": PHASE6DA_DIRECT_OS_POLICY,
        },
        "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
        "selector_bundle": {
            **PHASE6DA_DIRECT_COMMON_SELECTOR_BUNDLE,
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
            PHASEIFSM_CANDIDATE_SELECTOR: True,
            FCFOS_CANDIDATE_SELECTOR: True,
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
        "candidate_marker": {
            "required": True,
            "expected_count": 1,
            "observed_count": 1,
            "status": "clean",
            "coverage_required": True,
            "coverage_positive": True,
            "line": FCFOS_MARKER_LINE,
        },
    },
)
PHASE6DA_DIRECT_RECORD_IDS = frozenset(
    contract["record_id"] for contract in PHASE6DA_DIRECT_CONTRACTS
)
A64_CONFIGURATION_PREFIX = "A64 dynarec configuration:"


class RegistryError(Exception):
    """The epoch, report, lineage, or atomic decision is invalid."""


class _DuplicateKeyError(ValueError):
    pass


def _require(condition: bool, source: object, message: str) -> None:
    if not condition:
        raise RegistryError(f"{source}: {message}")


def _pairs(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
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
            parse_constant=_reject_constant,
            object_pairs_hook=_pairs,
        )
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise RegistryError(f"{source}: invalid strict JSON: {error}") from error
    _require(type(value) is dict, source, "JSON root must be an object")
    return value


def _load_json(path: pathlib.Path) -> Mapping[str, Any]:
    _require(path.is_file(), path, "missing regular JSON file")
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise RegistryError(f"{path}: cannot read JSON: {error}") from error
    return _load_json_payload(payload, path)


def _exact(value: Any, keys: frozenset[str], source: object, name: str) -> Mapping[str, Any]:
    _require(type(value) is dict, source, f"{name} must be an object")
    actual = frozenset(value)
    _require(actual == keys, source, f"{name} keys mismatch: missing={sorted(keys-actual)} extra={sorted(actual-keys)}")
    return value


def _string(value: Any, source: object, name: str) -> str:
    _require(type(value) is str and value != "" and value == value.strip(), source, f"{name} must be a non-empty edge-trimmed string")
    _require(all(ord(character) >= 0x20 for character in value), source, f"{name} contains a control character")
    return value


def _safe_id(value: Any, source: object, name: str) -> str:
    result = _string(value, source, name)
    _require(SAFE_ID_RE.fullmatch(result) is not None, source, f"{name} is not a safe identifier")
    return result


def _sha(value: Any, source: object, name: str) -> str:
    result = _string(value, source, name)
    _require(HEX_SHA256_RE.fullmatch(result) is not None, source, f"{name} must be lowercase SHA-256")
    return result


def _sha_file(path: pathlib.Path) -> str:
    _require(path.is_file() and not path.is_symlink(), path, "expected regular non-symlink file")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise RegistryError(f"{path}: cannot hash: {error}") from error
    return digest.hexdigest()


def _absolute_without_symlink_resolution(path: pathlib.Path) -> pathlib.Path:
    return pathlib.Path(os.path.abspath(os.fspath(path.expanduser())))


def _read_stable_report_payload(path: pathlib.Path) -> Tuple[pathlib.Path, bytes]:
    absolute = _absolute_without_symlink_resolution(path)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(absolute, flags)
    except OSError as error:
        raise RegistryError(f"{absolute}: cannot open regular non-symlink report: {error}") from error
    try:
        before = os.fstat(descriptor)
        _require(stat.S_ISREG(before.st_mode), absolute, "report is not a regular file")
        _require(
            before.st_mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH) == 0,
            absolute,
            "report origin must be sealed read-only before import",
        )
        _require(before.st_size <= REPORT_MAX_BYTES, absolute, "report exceeds size limit")
        chunks = []
        total = 0
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            _require(total <= REPORT_MAX_BYTES, absolute, "report exceeds size limit")
            chunks.append(chunk)
        after = os.fstat(descriptor)
    finally:
        os.close(descriptor)
    stable_before = (
        before.st_dev,
        before.st_ino,
        before.st_mode,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    stable_after = (
        after.st_dev,
        after.st_ino,
        after.st_mode,
        after.st_size,
        after.st_mtime_ns,
        after.st_ctime_ns,
    )
    payload = b"".join(chunks)
    _require(stable_before == stable_after, absolute, "report changed while being imported")
    _require(len(payload) == after.st_size, absolute, "report size changed while being imported")
    return absolute, payload


def _canonical_bytes(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _fsync_directory(path: pathlib.Path) -> None:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_sealed_descriptor(descriptor: int, payload: bytes, source: object) -> None:
    view = memoryview(payload)
    while view:
        written = os.write(descriptor, view)
        _require(written > 0, source, "short write")
        view = view[written:]
    os.fsync(descriptor)
    os.fchmod(descriptor, 0o444)
    os.fsync(descriptor)


def _atomic_bytes(path: pathlib.Path, payload: bytes, *, replace: bool = False) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    if not replace:
        descriptor: Optional[int] = None
        created = False
        try:
            descriptor = os.open(path, flags, 0o600)
            created = True
            _write_sealed_descriptor(descriptor, payload, path)
            os.close(descriptor)
            descriptor = None
            _fsync_directory(path.parent)
        except FileExistsError as error:
            raise RegistryError(f"{path}: refusing to overwrite immutable file") from error
        except BaseException:
            if descriptor is not None:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
            if created:
                try:
                    path.unlink()
                    _fsync_directory(path.parent)
                except OSError:
                    pass
            raise
        return

    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}.{time.time_ns()}")
    descriptor = None
    try:
        descriptor = os.open(temporary, flags, 0o600)
        _write_sealed_descriptor(descriptor, payload, temporary)
        os.close(descriptor)
        descriptor = None
        os.replace(temporary, path)
        _fsync_directory(path.parent)
    except BaseException:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def _atomic_json(path: pathlib.Path, value: Any, *, replace: bool = False) -> None:
    _atomic_bytes(path, json.dumps(value, ensure_ascii=False, allow_nan=False, sort_keys=True, indent=2).encode("utf-8") + b"\n", replace=replace)


def _load_module(path: pathlib.Path, name: str) -> Any:
    _require(path.is_file(), path, f"missing {name}")
    spec = importlib.util.spec_from_file_location(f"_lumen_{name.replace('-', '_')}", path)
    _require(spec is not None and spec.loader is not None, path, f"cannot load {name}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    except Exception as error:
        raise RegistryError(f"{path}: cannot import {name}: {error}") from error
    return module


def _tool_paths() -> Tuple[pathlib.Path, pathlib.Path]:
    directory = pathlib.Path(__file__).resolve().parent
    return directory / "persistent_performance_harness.py", directory / "persistent_performance_report.py"


def _validate_epoch_value(value: Mapping[str, Any], source: object) -> Mapping[str, Any]:
    epoch = _exact(value, frozenset({"schema", "protocol", "comparability"}), source, "epoch")
    schema = _exact(epoch["schema"], frozenset({"name", "version"}), source, "epoch.schema")
    _require(schema == {"name": EPOCH_SCHEMA_NAME, "version": EPOCH_SCHEMA_VERSION}, source, "unsupported epoch schema")
    harness_path, report_path = _tool_paths()
    harness = _load_module(harness_path, "persistent-performance-harness")
    harness._validate_protocol(epoch["protocol"], source)
    harness._validate_comparability(epoch["comparability"], source)
    _require(report_path.is_file(), report_path, "report tool must exist before epoch initialization")
    return epoch


def init_registry(root: pathlib.Path, epoch_config: pathlib.Path) -> pathlib.Path:
    root = root.expanduser().resolve()
    epoch_config = epoch_config.expanduser().resolve()
    epoch = _validate_epoch_value(_load_json(epoch_config), epoch_config)
    _require(not root.exists(), root, "registry already exists; refusing to overwrite")
    parent = root.parent
    _require(parent.is_dir(), parent, "registry parent does not exist")
    temporary = parent / f".{root.name}.initializing.{os.getpid()}.{time.time_ns()}"
    temporary.mkdir(mode=0o700)
    try:
        for name in ("pending", "records", "reports", "generations", "decisions"):
            (temporary / name).mkdir(mode=0o700)
        _atomic_json(temporary / "epoch.json", epoch)
        os.replace(temporary, root)
        directory = os.open(parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except BaseException:
        if temporary.exists():
            import shutil
            shutil.rmtree(temporary)
        raise
    return root / "epoch.json"


def _load_epoch(root: pathlib.Path) -> Mapping[str, Any]:
    return _validate_epoch_value(_load_json(root / "epoch.json"), root / "epoch.json")


def _inside(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _manifest_readonly(path: pathlib.Path) -> None:
    _require(path.is_file() and not path.is_symlink(), path, "sealed artifact is missing or is not a regular non-symlink file")
    mode = path.stat().st_mode
    _require(mode & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH) == 0, path, "sealed file remains writable")


def _verify_anchor_evidence(record_root: pathlib.Path, receipt_path: pathlib.Path) -> None:
    receipt = _load_json(receipt_path)
    evidence_root = (record_root / "evidence").resolve()
    declared_evidence_root = record_root / "evidence"
    _require(
        declared_evidence_root.is_dir() and not declared_evidence_root.is_symlink(),
        declared_evidence_root,
        "sealed evidence root is missing or is a symlink",
    )
    for field in ("anchor_evidence", "completion_evidence"):
        entries = receipt.get(field)
        _require(type(entries) is list and entries, receipt_path, f"{field} must be a non-empty list")
        for index, entry in enumerate(entries):
            item = _exact(
                entry,
                frozenset({"file", "sha256"}),
                receipt_path,
                f"{field}[{index}]",
            )
            relative_text = _string(item["file"], receipt_path, f"{field}[{index}].file")
            relative = pathlib.PurePosixPath(relative_text)
            _require(
                not relative.is_absolute()
                and relative.parts
                and relative.parts[0] == "evidence"
                and ".." not in relative.parts
                and relative.as_posix() == relative_text,
                receipt_path,
                f"{field}[{index}] is not a canonical path below evidence/",
            )
            declared = record_root.joinpath(*relative.parts)
            cursor = record_root
            for component in relative.parts:
                cursor = cursor / component
                _require(not cursor.is_symlink(), cursor, "evidence path contains a symlink")
            resolved = declared.resolve()
            _require(
                _inside(resolved, evidence_root),
                declared,
                "evidence path escapes the sealed record evidence directory",
            )
            _require(resolved.suffix.lower() == ".png", resolved, "anchor evidence must be PNG")
            _manifest_readonly(resolved)
            try:
                with resolved.open("rb") as stream:
                    signature = stream.read(8)
            except OSError as error:
                raise RegistryError(f"{resolved}: cannot read evidence PNG: {error}") from error
            _require(signature == b"\x89PNG\r\n\x1a\n", resolved, "invalid PNG signature")
            expected = _sha(item["sha256"], receipt_path, f"{field}[{index}].sha256")
            _require(_sha_file(resolved) == expected, resolved, "sealed evidence PNG hash mismatch")


def _configuration_projection_contracts(
    source: object,
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
        _require(
            type(item) is dict,
            source,
            f"configuration projection contract[{index}] must be an object",
        )
        record_id = _string(
            item.get("record_id"),
            source,
            f"configuration projection contract[{index}].record_id",
        )
        candidate_selector = _string(
            item.get("candidate_selector"),
            source,
            f"configuration projection contract[{index}].candidate_selector",
        )
        selector_bundle = item.get("selector_bundle")
        _require(
            type(selector_bundle) is dict
            and selector_bundle.get(candidate_selector) is True,
            source,
            f"configuration projection contract[{index}] has an invalid selector bundle",
        )
        _require(
            all(type(value) is bool for value in selector_bundle.values()),
            source,
            f"configuration projection contract[{index}] selector values must be JSON booleans",
        )
        effective_selector_bundle = item.get(
            "effective_selector_bundle", selector_bundle
        )
        _require(
            type(effective_selector_bundle) is dict
            and set(effective_selector_bundle) == set(selector_bundle)
            and all(
                type(value) is bool
                for value in effective_selector_bundle.values()
            )
            and effective_selector_bundle.get(candidate_selector) is True,
            source,
            f"configuration projection contract[{index}] has an invalid effective selector bundle",
        )
        if effective_selector_bundle != selector_bundle:
            _require(
                record_id == NODA_CANDIDATE_RECORD_ID
                and candidate_selector == NODA_CANDIDATE_SELECTOR
                and item.get("generation") == 5
                and item.get("parent_record_id")
                == METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
                and set(
                    name
                    for name in selector_bundle
                    if selector_bundle[name]
                    is not effective_selector_bundle[name]
                )
                == {CONFIGURATION_PROJECTION_STABLE_SELECTOR}
                and selector_bundle[
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR
                ]
                is True
                and effective_selector_bundle[
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR
                ]
                is False,
                source,
                "only NODA g5 may supersede requested 6CL in its effective selector bundle",
            )
        record_ids.append(record_id)
        candidate_selectors.append(candidate_selector)
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
    receipt: Any, source: object
) -> Mapping[str, str]:
    line = _string(receipt, source, "provenance.run_receipt")
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


def _sealed_prefixed_log_lines(
    paths: Sequence[pathlib.Path],
    prefixes: Sequence[str],
    source: object,
) -> Mapping[str, Sequence[str]]:
    result: Dict[str, list[str]] = {prefix: [] for prefix in prefixes}
    try:
        for path in paths:
            with path.open("r", encoding="utf-8", errors="strict") as stream:
                for raw_line in stream:
                    line = raw_line.rstrip("\r\n")
                    for prefix in prefixes:
                        if line.startswith(prefix):
                            result[prefix].append(line)
    except (OSError, UnicodeError) as error:
        raise RegistryError(
            f"{source}: cannot read sealed candidate marker logs: {error}"
        ) from error
    return result


def _validate_ufsb_imse_sealed_identity(
    manifest: Any, contract: Mapping[str, Any]
) -> None:
    """Bind the combined record to its sealed primary/companion evidence."""

    source = manifest.path
    primary_selector = contract.get("primary_selector")
    companion_selectors = contract.get("companion_selectors")
    selector_bundle = contract["selector_bundle"]
    _require(
        primary_selector == UFSB_CANDIDATE_SELECTOR
        and companion_selectors == frozenset({IMSE_COMPANION_SELECTOR})
        and selector_bundle.get(primary_selector) is True
        and all(selector_bundle.get(name) is True for name in companion_selectors),
        source,
        "UFSB+IMSE primary/companion selector identity drifted",
    )

    record_root = source.parent
    start_path = record_root / "start-config.json"
    start = _load_json(start_path)
    start_schema = _exact(
        start.get("schema"),
        frozenset({"name", "version"}),
        start_path,
        "start-config.schema",
    )
    _require(
        start_schema
        == {"name": "lumen.persistent-performance-start", "version": 1},
        start_path,
        "unsupported sealed start-config schema",
    )
    start_provenance = _exact(
        start.get("provenance"),
        frozenset(
            {
                "record_id",
                "run_receipt",
                "candidate_selector",
                "selectors",
                "selector_receipts",
            }
        ),
        start_path,
        "start-config.provenance",
    )
    start_lineage = _exact(
        start.get("lineage"),
        frozenset({"generation", "parent_record_id", "original_role"}),
        start_path,
        "start-config.lineage",
    )
    start_marker = _exact(
        start.get("candidate_marker"),
        frozenset(
            {
                "required",
                "prefix",
                "required_tokens",
                "coverage_required",
                "coverage_token",
            }
        ),
        start_path,
        "start-config.candidate_marker",
    )
    start_launch = _exact(
        start.get("launch"),
        frozenset({"arguments", "common_environment", "experiment_environment"}),
        start_path,
        "start-config.launch",
    )
    start_selectors = start_provenance["selectors"]
    selector_receipts = start_provenance["selector_receipts"]
    experiment_environment = start_launch["experiment_environment"]
    expected_primary_marker = {
        "required": True,
        "prefix": UFSB_PRIMARY_MARKER_PREFIX,
        "required_tokens": list(UFSB_PRIMARY_MARKER_TOKENS),
        "coverage_required": True,
        "coverage_token": "coverage=positive",
    }
    _require(
        start.get("protocol") == manifest.protocol
        and start.get("comparability") == manifest.comparability
        and start_lineage == manifest.lineage
        and start_provenance["record_id"]
        == manifest.provenance["record_id"]
        and start_provenance["run_receipt"]
        == manifest.provenance["run_receipt"]
        and start_provenance["candidate_selector"] == primary_selector
        and type(start_selectors) is dict
        and start_selectors == selector_bundle
        and all(type(value) is bool for value in start_selectors.values())
        and type(selector_receipts) is dict
        and set(selector_receipts) == set(selector_bundle)
        and selector_receipts.get(primary_selector)
        == {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_unrolled_fixed_signature_batching",
        }
        and selector_receipts.get(IMSE_COMPANION_SELECTOR)
        == {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_idempotent_mru_store_elision",
        }
        and type(experiment_environment) is dict
        and set(experiment_environment) == set(selector_bundle)
        and all(
            experiment_environment[name] == "1" for name in selector_bundle
        )
        and start_marker == expected_primary_marker,
        start_path,
        "sealed UFSB+IMSE start-config does not bind the exact record, primary selector, companion selector, launch bundle, and primary marker identity",
    )

    marker_lines = _sealed_prefixed_log_lines(
        (record_root / "stderr.log",),
        (UFSB_PRIMARY_MARKER_PREFIX, IMSE_COMPANION_MARKER_PREFIX),
        source,
    )
    primary_lines = marker_lines[UFSB_PRIMARY_MARKER_PREFIX]
    companion_lines = marker_lines[IMSE_COMPANION_MARKER_PREFIX]
    _require(
        primary_lines == [UFSB_PRIMARY_MARKER_LINE]
        and manifest.lifecycle["candidate_marker"]["observed_count"]
        == len(primary_lines),
        source,
        "sealed UFSB primary marker is not selector-specific, exact-line, exact-once, and coverage-positive",
    )
    _require(
        companion_lines == [IMSE_COMPANION_MARKER_LINE],
        source,
        "sealed IMSE companion marker is not selector-specific, exact-line, exact-once, and coverage-positive",
    )


def _validate_fcfos_sealed_identity(
    manifest: Any, contract: Mapping[str, Any]
) -> None:
    """Bind FCFOS to its exact sibling-free launch and final marker."""

    source = manifest.path
    selector_bundle = contract["selector_bundle"]
    _require(
        selector_bundle.get(FCFOS_CANDIDATE_SELECTOR) is True
        and UFSB_CANDIDATE_SELECTOR not in selector_bundle
        and IMSE_COMPANION_SELECTOR not in selector_bundle,
        source,
        "FCFOS selector identity is not the exact sibling-free retained bundle",
    )

    record_root = source.parent
    start_path = record_root / "start-config.json"
    start = _load_json(start_path)
    start_schema = _exact(
        start.get("schema"),
        frozenset({"name", "version"}),
        start_path,
        "start-config.schema",
    )
    _require(
        start_schema
        == {"name": "lumen.persistent-performance-start", "version": 1},
        start_path,
        "unsupported sealed start-config schema",
    )
    start_provenance = _exact(
        start.get("provenance"),
        frozenset(
            {
                "record_id",
                "run_receipt",
                "candidate_selector",
                "selectors",
                "selector_receipts",
            }
        ),
        start_path,
        "start-config.provenance",
    )
    start_lineage = _exact(
        start.get("lineage"),
        frozenset({"generation", "parent_record_id", "original_role"}),
        start_path,
        "start-config.lineage",
    )
    start_marker = _exact(
        start.get("candidate_marker"),
        frozenset(
            {
                "required",
                "prefix",
                "required_tokens",
                "coverage_required",
                "coverage_token",
            }
        ),
        start_path,
        "start-config.candidate_marker",
    )
    start_launch = _exact(
        start.get("launch"),
        frozenset(
            {"arguments", "common_environment", "experiment_environment"}
        ),
        start_path,
        "start-config.launch",
    )
    expected_selector_receipts = {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "stable_negative_route",
        },
        CONFIGURATION_PROJECTION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "guest_memory_operation_snapshot_elision",
        },
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_dense_directory",
        },
        PHASEIFSM_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_inline_fixed_signature_match",
        },
        FCFOS_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_callback_free_ordinary_specialization",
        },
    }
    start_selectors = start_provenance["selectors"]
    selector_receipts = start_provenance["selector_receipts"]
    experiment_environment = start_launch["experiment_environment"]
    expected_marker = {
        "required": True,
        "prefix": FCFOS_MARKER_PREFIX,
        "required_tokens": list(FCFOS_MARKER_TOKENS),
        "coverage_required": True,
        "coverage_token": "coverage=positive",
    }
    _require(
        start.get("protocol") == manifest.protocol
        and start.get("comparability") == manifest.comparability
        and start_lineage == manifest.lineage
        and start_provenance["record_id"]
        == manifest.provenance["record_id"]
        and start_provenance["run_receipt"]
        == manifest.provenance["run_receipt"]
        and start_provenance["candidate_selector"]
        == FCFOS_CANDIDATE_SELECTOR
        and type(start_selectors) is dict
        and start_selectors == selector_bundle
        and all(type(value) is bool for value in start_selectors.values())
        and type(selector_receipts) is dict
        and selector_receipts == expected_selector_receipts
        and type(experiment_environment) is dict
        and set(experiment_environment) == set(selector_bundle)
        and all(
            experiment_environment[name] == "1" for name in selector_bundle
        )
        and start_marker == expected_marker,
        start_path,
        "sealed FCFOS start-config does not bind the exact record, selector, sibling-free launch bundle, receipts, and marker identity",
    )

    marker_lines = _sealed_prefixed_log_lines(
        (record_root / "stderr.log",),
        (FCFOS_MARKER_PREFIX,),
        source,
    )[FCFOS_MARKER_PREFIX]
    _require(
        marker_lines == [FCFOS_MARKER_LINE]
        and manifest.lifecycle["candidate_marker"]["observed_count"]
        == len(marker_lines),
        source,
        "sealed FCFOS marker is not selector-specific, exact-line, exact-once, and coverage-positive",
    )


def _validate_alc_sealed_identity(
    manifest: Any, contract: Mapping[str, Any]
) -> None:
    """Bind ALC to its projected build, retained launch, and exact marker."""

    source = manifest.path
    selector_bundle = contract["selector_bundle"]
    _require(
        contract.get("record_id") == ALC_CANDIDATE_RECORD_ID
        and contract.get("candidate_selector") == ALC_CANDIDATE_SELECTOR
        and selector_bundle.get(ALC_CANDIDATE_SELECTOR) is True
        and selector_bundle.get(PHASEIFSM_CANDIDATE_SELECTOR) is True
        and UFSB_CANDIDATE_SELECTOR not in selector_bundle
        and IMSE_COMPANION_SELECTOR not in selector_bundle
        and FCFOS_CANDIDATE_SELECTOR not in selector_bundle,
        source,
        "ALC selector identity is not the exact sibling-free retained bundle",
    )

    record_root = source.parent
    start_path = record_root / "start-config.json"
    start = _load_json(start_path)
    start_schema = _exact(
        start.get("schema"),
        frozenset({"name", "version"}),
        start_path,
        "start-config.schema",
    )
    _require(
        start_schema
        == {"name": "lumen.persistent-performance-start", "version": 1},
        start_path,
        "unsupported sealed start-config schema",
    )
    start_provenance = _exact(
        start.get("provenance"),
        frozenset(
            {
                "record_id",
                "run_receipt",
                "candidate_selector",
                "selectors",
                "selector_receipts",
            }
        ),
        start_path,
        "start-config.provenance",
    )
    start_lineage = _exact(
        start.get("lineage"),
        frozenset({"generation", "parent_record_id", "original_role"}),
        start_path,
        "start-config.lineage",
    )
    start_marker = _exact(
        start.get("candidate_marker"),
        frozenset(
            {
                "required",
                "prefix",
                "required_tokens",
                "coverage_required",
                "coverage_token",
            }
        ),
        start_path,
        "start-config.candidate_marker",
    )
    start_launch = _exact(
        start.get("launch"),
        frozenset(
            {"arguments", "common_environment", "experiment_environment"}
        ),
        start_path,
        "start-config.launch",
    )
    expected_selector_receipts = {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "stable_negative_route",
        },
        CONFIGURATION_PROJECTION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "guest_memory_operation_snapshot_elision",
        },
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_dense_directory",
        },
        PHASEIFSM_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_inline_fixed_signature_match",
        },
        ALC_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "arm_loading_composite",
        },
    }
    start_selectors = start_provenance["selectors"]
    selector_receipts = start_provenance["selector_receipts"]
    experiment_environment = start_launch["experiment_environment"]
    expected_marker = {
        "required": True,
        "prefix": ALC_MARKER_PREFIX,
        "required_tokens": list(ALC_MARKER_TOKENS),
        "coverage_required": True,
        "coverage_token": "coverage=positive",
    }
    _require(
        start.get("protocol") == manifest.protocol
        and start.get("comparability") == manifest.comparability
        and start_lineage == manifest.lineage
        and start_provenance["record_id"]
        == manifest.provenance["record_id"]
        and start_provenance["run_receipt"]
        == manifest.provenance["run_receipt"]
        and start_provenance["candidate_selector"]
        == ALC_CANDIDATE_SELECTOR
        and type(start_selectors) is dict
        and start_selectors == selector_bundle
        and all(type(value) is bool for value in start_selectors.values())
        and type(selector_receipts) is dict
        and selector_receipts == expected_selector_receipts
        and type(experiment_environment) is dict
        and set(experiment_environment) == set(selector_bundle)
        and all(
            experiment_environment[name] == "1" for name in selector_bundle
        )
        and start_marker == expected_marker,
        start_path,
        "sealed ALC start-config does not bind the exact record, selector, retained launch bundle, receipts, and marker identity",
    )

    receipt_fields = _structured_receipt_fields(
        manifest.provenance["run_receipt"], source
    )
    _require(
        all(
            receipt_fields.get(name) == expected
            for name, expected in contract["required_receipt_fields"].items()
        ),
        source,
        "sealed ALC run receipt differs from its role-specific contract",
    )
    _require(
        manifest.lifecycle["candidate_marker"] == contract["candidate_marker"],
        source,
        "sealed ALC candidate-marker lifecycle differs from its exact contract",
    )
    marker_lines = _sealed_prefixed_log_lines(
        (record_root / "stderr.log",),
        (ALC_MARKER_PREFIX,),
        source,
    )[ALC_MARKER_PREFIX]
    _require(
        marker_lines == [ALC_MARKER_LINE]
        and manifest.lifecycle["candidate_marker"]["observed_count"]
        == len(marker_lines),
        source,
        "sealed ALC marker is not selector-specific, exact-line, exact-once, and coverage-positive",
    )


def _validate_metal_runtime_bundle_sealed_identity(
    manifest: Any, contract: Mapping[str, Any]
) -> None:
    """Bind g4 to its exact ALC parent, selectors, receipts, and three finals."""

    source = manifest.path
    selector_bundle = contract["selector_bundle"]
    _require(
        contract.get("record_id") == METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
        and contract.get("candidate_selector") == MSORE_CANDIDATE_SELECTOR
        and contract.get("generation") == 4
        and contract.get("parent_record_id") == ALC_CANDIDATE_RECORD_ID
        and selector_bundle.get(ALC_CANDIDATE_SELECTOR) is True
        and selector_bundle.get(PHASEIFSM_CANDIDATE_SELECTOR) is True
        and all(
            selector_bundle.get(selector) is True
            for selector in METAL_RUNTIME_BUNDLE_SELECTORS
        )
        and UFSB_CANDIDATE_SELECTOR not in selector_bundle
        and IMSE_COMPANION_SELECTOR not in selector_bundle
        and FCFOS_CANDIDATE_SELECTOR not in selector_bundle,
        source,
        "MSORE+MSFC+MDSSC selector identity is not the exact sibling-free ALC g4 bundle",
    )

    record_root = source.parent
    start_path = record_root / "start-config.json"
    start = _load_json(start_path)
    start_schema = _exact(
        start.get("schema"),
        frozenset({"name", "version"}),
        start_path,
        "start-config.schema",
    )
    _require(
        start_schema
        == {"name": "lumen.persistent-performance-start", "version": 1},
        start_path,
        "unsupported sealed start-config schema",
    )
    start_provenance = _exact(
        start.get("provenance"),
        frozenset(
            {
                "record_id",
                "run_receipt",
                "candidate_selector",
                "selectors",
                "selector_receipts",
            }
        ),
        start_path,
        "start-config.provenance",
    )
    start_lineage = _exact(
        start.get("lineage"),
        frozenset({"generation", "parent_record_id", "original_role"}),
        start_path,
        "start-config.lineage",
    )
    start_marker = _exact(
        start.get("candidate_marker"),
        frozenset(
            {
                "required",
                "prefix",
                "required_tokens",
                "coverage_required",
                "coverage_token",
            }
        ),
        start_path,
        "start-config.candidate_marker",
    )
    start_launch = _exact(
        start.get("launch"),
        frozenset(
            {"arguments", "common_environment", "experiment_environment"}
        ),
        start_path,
        "start-config.launch",
    )
    expected_selector_receipts = {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "stable_negative_route",
        },
        CONFIGURATION_PROJECTION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "guest_memory_operation_snapshot_elision",
        },
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_dense_directory",
        },
        PHASEIFSM_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_inline_fixed_signature_match",
        },
        ALC_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "arm_loading_composite",
        },
        MSFC_COMPANION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "metal_shader_function_cache",
        },
        MDSSC_COMPANION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "metal_depth_stencil_state_cache",
        },
        MSORE_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "metal_state_only_record_elision",
        },
    }
    expected_marker = {
        "required": True,
        "prefix": MSORE_MARKER_PREFIX,
        "required_tokens": list(MSORE_MARKER_TOKENS),
        "coverage_required": True,
        "coverage_token": "coverage=positive",
    }
    start_selectors = start_provenance["selectors"]
    selector_receipts = start_provenance["selector_receipts"]
    experiment_environment = start_launch["experiment_environment"]
    _require(
        start.get("protocol") == manifest.protocol
        and start.get("comparability") == manifest.comparability
        and start_lineage == manifest.lineage
        and start_provenance["record_id"]
        == manifest.provenance["record_id"]
        and start_provenance["run_receipt"]
        == manifest.provenance["run_receipt"]
        and start_provenance["candidate_selector"] == MSORE_CANDIDATE_SELECTOR
        and type(start_selectors) is dict
        and start_selectors == selector_bundle
        and all(type(value) is bool for value in start_selectors.values())
        and type(selector_receipts) is dict
        and selector_receipts == expected_selector_receipts
        and type(experiment_environment) is dict
        and set(experiment_environment) == set(selector_bundle)
        and all(
            experiment_environment[name] == "1" for name in selector_bundle
        )
        and start_marker == expected_marker,
        start_path,
        "sealed MSORE+MSFC+MDSSC start-config does not bind the exact g4 record, ALC parent, selector bundle, receipts, launch environment, and primary marker",
    )

    receipt_fields = _structured_receipt_fields(
        manifest.provenance["run_receipt"], source
    )
    _require(
        all(
            receipt_fields.get(name) == expected
            for name, expected in contract["required_receipt_fields"].items()
        ),
        source,
        "sealed MSORE+MSFC+MDSSC run receipt differs from its role-specific contract",
    )
    _require(
        manifest.lifecycle["candidate_marker"] == contract["candidate_marker"],
        source,
        "sealed MSORE candidate-marker lifecycle differs from its exact contract",
    )
    marker_lines = _sealed_prefixed_log_lines(
        (record_root / "stderr.log",),
        (MSORE_MARKER_PREFIX, MSFC_MARKER_PREFIX, MDSSC_MARKER_PREFIX),
        source,
    )
    _require(
        marker_lines[MSORE_MARKER_PREFIX] == [MSORE_MARKER_LINE]
        and manifest.lifecycle["candidate_marker"]["observed_count"] == 1,
        source,
        "sealed MSORE primary marker is not exact-line, exact-once, and coverage-positive",
    )
    _require(
        marker_lines[MSFC_MARKER_PREFIX] == [MSFC_MARKER_LINE],
        source,
        "sealed MSFC companion marker is not exact-line, exact-once, and coverage-positive",
    )
    _require(
        marker_lines[MDSSC_MARKER_PREFIX] == [MDSSC_MARKER_LINE],
        source,
        "sealed MDSSC companion marker is not exact-line, exact-once, and coverage-positive",
    )


def _validate_noda_sealed_identity(
    manifest: Any, contract: Mapping[str, Any]
) -> None:
    """Bind g5 to exact g4+NODA launch, 6CL supersession, and finals."""

    source = manifest.path
    selector_bundle = contract["selector_bundle"]
    effective_selector_bundle = contract.get("effective_selector_bundle")
    retained_g4_bundle = (
        METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT[
            "selector_bundle"
        ]
    )
    _require(
        contract.get("record_id") == NODA_CANDIDATE_RECORD_ID
        and contract.get("candidate_selector") == NODA_CANDIDATE_SELECTOR
        and contract.get("generation") == 5
        and contract.get("parent_record_id")
        == METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
        and selector_bundle
        == {**retained_g4_bundle, NODA_CANDIDATE_SELECTOR: True}
        and effective_selector_bundle
        == {
            **retained_g4_bundle,
            CONFIGURATION_PROJECTION_STABLE_SELECTOR: False,
            NODA_CANDIDATE_SELECTOR: True,
        }
        and PBA_CANDIDATE_SELECTOR not in selector_bundle
        and PBA_CANDIDATE_SELECTOR not in effective_selector_bundle
        and UFSB_CANDIDATE_SELECTOR not in selector_bundle
        and IMSE_COMPANION_SELECTOR not in selector_bundle
        and FCFOS_CANDIDATE_SELECTOR not in selector_bundle,
        source,
        "NODA selector identity is not exact g4+NODA with only 6CL superseded and PBA absent",
    )

    record_root = source.parent
    start_path = record_root / "start-config.json"
    start = _load_json(start_path)
    start_schema = _exact(
        start.get("schema"),
        frozenset({"name", "version"}),
        start_path,
        "start-config.schema",
    )
    _require(
        start_schema
        == {"name": "lumen.persistent-performance-start", "version": 1},
        start_path,
        "unsupported sealed start-config schema",
    )
    start_provenance = _exact(
        start.get("provenance"),
        frozenset(
            {
                "record_id",
                "run_receipt",
                "candidate_selector",
                "selectors",
                "selector_receipts",
            }
        ),
        start_path,
        "start-config.provenance",
    )
    start_lineage = _exact(
        start.get("lineage"),
        frozenset({"generation", "parent_record_id", "original_role"}),
        start_path,
        "start-config.lineage",
    )
    start_marker = _exact(
        start.get("candidate_marker"),
        frozenset(
            {
                "required",
                "prefix",
                "required_tokens",
                "coverage_required",
                "coverage_token",
            }
        ),
        start_path,
        "start-config.candidate_marker",
    )
    start_launch = _exact(
        start.get("launch"),
        frozenset(
            {"arguments", "common_environment", "experiment_environment"}
        ),
        start_path,
        "start-config.launch",
    )
    expected_selector_receipts = {
        CONFIGURATION_PROJECTION_STABLE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "stable_negative_route_requested",
        },
        CONFIGURATION_PROJECTION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "guest_memory_operation_snapshot_elision",
        },
        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_dense_directory",
        },
        PHASEIFSM_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "fast_block_inline_fixed_signature_match",
        },
        ALC_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "arm_loading_composite",
        },
        MSFC_COMPANION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "metal_shader_function_cache",
        },
        MDSSC_COMPANION_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "metal_depth_stencil_state_cache",
        },
        MSORE_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "metal_state_only_record_elision",
        },
        NODA_CANDIDATE_SELECTOR: {
            "prefix": A64_CONFIGURATION_PREFIX,
            "field": "native_only_admission",
        },
    }
    expected_marker = {
        "required": True,
        "prefix": NODA_MARKER_PREFIX,
        "required_tokens": list(NODA_MARKER_TOKENS),
        "coverage_required": True,
        "coverage_token": "coverage=positive",
    }
    start_selectors = start_provenance["selectors"]
    selector_receipts = start_provenance["selector_receipts"]
    experiment_environment = start_launch["experiment_environment"]
    _require(
        start.get("protocol") == manifest.protocol
        and start.get("comparability") == manifest.comparability
        and start_lineage == manifest.lineage
        and start_provenance["record_id"]
        == manifest.provenance["record_id"]
        and start_provenance["run_receipt"]
        == manifest.provenance["run_receipt"]
        and start_provenance["candidate_selector"]
        == NODA_CANDIDATE_SELECTOR
        and type(start_selectors) is dict
        and start_selectors == selector_bundle
        and all(type(value) is bool for value in start_selectors.values())
        and type(selector_receipts) is dict
        and selector_receipts == expected_selector_receipts
        and type(experiment_environment) is dict
        and set(experiment_environment) == set(selector_bundle)
        and all(
            experiment_environment[name] == "1" for name in selector_bundle
        )
        and PBA_CANDIDATE_SELECTOR not in experiment_environment
        and start_marker == expected_marker,
        start_path,
        "sealed NODA start-config does not bind exact g5 lineage, g4+NODA launch, requested 6CL receipt, PBA absence, and marker identity",
    )

    receipt_fields = _structured_receipt_fields(
        manifest.provenance["run_receipt"], source
    )
    _require(
        all(
            receipt_fields.get(name) == expected
            for name, expected in contract["required_receipt_fields"].items()
        ),
        source,
        "sealed NODA run receipt differs from its exact retained/superseded contract",
    )
    _require(
        manifest.lifecycle["candidate_marker"] == contract["candidate_marker"],
        source,
        "sealed NODA candidate-marker lifecycle differs from its exact contract",
    )
    marker_prefixes = tuple(NODA_REQUIRED_MARKER_LINES) + (
        PBA_MARKER_PREFIX,
    )
    marker_lines = _sealed_prefixed_log_lines(
        (record_root / "stderr.log",), marker_prefixes, source
    )
    for prefix, expected_line in NODA_REQUIRED_MARKER_LINES.items():
        _require(
            marker_lines[prefix] == [expected_line],
            source,
            f"sealed NODA finalizer {prefix} is not exact-line and exact-once",
        )
    _require(
        marker_lines[PBA_MARKER_PREFIX] == [],
        source,
        "sealed NODA logs contain a forbidden PBA finalizer",
    )


def _validate_phase6da_direct_record(manifest: Any) -> bool:
    """Validate the composite protocol/record scope for the direct 25G83 epoch."""

    source = manifest.path
    protocol_id = manifest.protocol["protocol_id"]
    record_id = manifest.provenance["record_id"]
    record_signal = record_id in PHASE6DA_DIRECT_RECORD_IDS
    expected_selectors = manifest.provenance["selectors"]["expected"]
    effective_selectors = manifest.provenance["selectors"]["effective"]
    noda_selector_signal = (
        NODA_CANDIDATE_SELECTOR in expected_selectors
        or NODA_CANDIDATE_SELECTOR in effective_selectors
    )
    noda_record_signal = record_id == NODA_CANDIDATE_RECORD_ID
    if noda_selector_signal or noda_record_signal:
        _require(
            noda_record_signal,
            source,
            "NODA selector is outside its preregistered projected g5 record",
        )
        return False
    metal_bundle_selector_signal = bool(
        METAL_RUNTIME_BUNDLE_SELECTORS
        & (set(expected_selectors) | set(effective_selectors))
    )
    metal_bundle_record_signal = (
        record_id == METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
    )
    if metal_bundle_selector_signal or metal_bundle_record_signal:
        _require(
            metal_bundle_record_signal,
            source,
            "MSORE/MSFC/MDSSC selector is outside its preregistered projected g4 record",
        )
        return False
    alc_selector_signal = (
        ALC_CANDIDATE_SELECTOR in expected_selectors
        or ALC_CANDIDATE_SELECTOR in effective_selectors
    )
    alc_record_signal = record_id == ALC_CANDIDATE_RECORD_ID
    if alc_selector_signal or alc_record_signal:
        _require(
            alc_record_signal,
            source,
            "ALC selector is outside its preregistered projected g3 record",
        )
        return False
    direct_successor_selector_signal = any(
        selector in expected_selectors or selector in effective_selectors
        for selector in (
            PHASEIFSM_CANDIDATE_SELECTOR,
            FCFOS_CANDIDATE_SELECTOR,
            UFSB_CANDIDATE_SELECTOR,
            IMSE_COMPANION_SELECTOR,
        )
    )
    if not record_signal and not direct_successor_selector_signal:
        return False
    _require(
        record_signal,
        source,
        "direct successor selector is outside its preregistered record",
    )
    _require(
        protocol_id == PHASE6DA_DIRECT_PROTOCOL_ID,
        source,
        "phase-6DA direct record is outside the retained protocol",
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
    _require(
        manifest.comparability["os"] == contract["os"]
        and manifest.comparability["runtime"]["configuration_sha256"]
        == contract["configuration_sha256"],
        source,
        "phase-6DA direct record differs from its preregistered OS/build or raw configuration identity",
    )
    lineage = manifest.lineage
    selector_bundle = contract["selector_bundle"]
    _require(
        lineage["generation"] == contract["generation"]
        and lineage["parent_record_id"] == contract["parent_record_id"]
        and lineage["original_role"] == contract["original_role"]
        and set(expected_selectors) == set(selector_bundle)
        and set(effective_selectors) == set(selector_bundle)
        and all(
            type(expected_selectors[name]) is bool
            and expected_selectors[name] is expected
            and type(effective_selectors[name]) is bool
            and effective_selectors[name] is expected
            for name, expected in selector_bundle.items()
        ),
        source,
        "phase-6DA direct record is outside its exact lineage and typed selector-bundle scope",
    )
    receipt_fields = _structured_receipt_fields(
        manifest.provenance["run_receipt"], source
    )
    _require(
        all(
            receipt_fields.get(name) == expected
            for name, expected in contract["required_receipt_fields"].items()
        ),
        source,
        "phase-6DA direct run receipt differs from its role-specific contract",
    )
    marker = manifest.lifecycle["candidate_marker"]
    _require(
        marker == contract["candidate_marker"],
        source,
        "phase-6DA direct candidate-marker lifecycle differs from its role-specific contract",
    )
    if "primary_selector" in contract or "companion_selectors" in contract:
        _validate_ufsb_imse_sealed_identity(manifest, contract)
    if record_id == FCFOS_DIRECT_CANDIDATE_RECORD_ID:
        _validate_fcfos_sealed_identity(manifest, contract)
    return True


def _verify_record_artifacts(
    manifest: Any,
    *,
    _projection_contract: Optional[Mapping[str, Any]] = None,
    _projection_contracts: Optional[Sequence[Mapping[str, Any]]] = None,
) -> None:
    projection_contracts = _configuration_projection_contracts(
        manifest.path,
        contract=_projection_contract,
        contracts=_projection_contracts,
    )
    root = manifest.path.parent
    artifacts = manifest.provenance["artifacts_sha256"]
    file_map = {
        "stdout_log": root / "stdout.log",
        "stderr_log": root / "stderr.log",
        "exit_status": root / ".exit_status",
        "fort_anchor_receipt": root / "anchors" / "fort_mode.json",
        "in_game_anchor_receipt": root / "anchors" / "in_game.json",
        "start_config": root / "start-config.json",
        "checkpoint_observation": root / "checkpoint-observation.json",
        "process_time": root / "process-time.json",
        "exit_uptime_raw": root / "exit-uptime-raw-ns.txt",
    }
    for name, path in file_map.items():
        _require(name in artifacts, manifest.path, f"sealed manifest is missing core artifact receipt {name}")
        _manifest_readonly(path)
        _require(_sha_file(path) == artifacts[name], path, f"sealed artifact hash mismatch for {name}")
    receipt_map = {
        "save_special_before": root / "save-special-before.sha256",
        "save_special_after": root / "save-special-after.sha256",
        "save_template_before": root / "save-template-before.sha256",
        "save_template_after": root / "save-template-after.sha256",
    }
    for name, path in receipt_map.items():
        _require(name in artifacts, manifest.path, f"sealed manifest is missing core artifact receipt {name}")
        _manifest_readonly(path)
        try:
            receipt = path.read_text(encoding="ascii", errors="strict")
        except (OSError, UnicodeError) as error:
            raise RegistryError(f"{path}: invalid artifact receipt: {error}") from error
        _require(receipt == artifacts[name] + "\n", path, f"sealed artifact receipt mismatch for {name}")
    for path in (
        root / "anchors" / "fort_mode.json",
        root / "anchors" / "in_game.json",
    ):
        _verify_anchor_evidence(root, path)

    projection_names = frozenset(CONFIGURATION_PROJECTION_ARTIFACTS)
    present_projection_names = projection_names & frozenset(artifacts)
    _require(
        not present_projection_names
        or present_projection_names == projection_names,
        manifest.path,
        "configuration projection artifacts must be present all-or-none",
    )
    projection_paths = {
        name: root / filename
        for name, filename in CONFIGURATION_PROJECTION_ARTIFACTS.items()
    }
    physical_projection_names = {
        name
        for name, path in projection_paths.items()
        if path.exists() or path.is_symlink()
    }
    direct_phase6da_record = _validate_phase6da_direct_record(manifest)
    if direct_phase6da_record:
        _require(
            not present_projection_names and not physical_projection_names,
            manifest.path,
            "phase-6DA direct records forbid configuration projection artifacts",
        )
    expected_selectors = manifest.provenance["selectors"]["expected"]
    effective_selectors = manifest.provenance["selectors"]["effective"]
    selector_names = frozenset(expected_selectors) | frozenset(
        effective_selectors
    )
    artifact_signal = bool(present_projection_names or physical_projection_names)
    record_matches = [] if direct_phase6da_record else [
        contract
        for contract in projection_contracts
        if manifest.provenance["record_id"] == contract["record_id"]
    ]
    selector_matches = [] if direct_phase6da_record else [
        contract
        for contract in projection_contracts
        if contract["candidate_selector"] in selector_names
    ]
    selected_contract: Optional[Mapping[str, Any]] = None
    if record_matches:
        _require(
            len(record_matches) == 1,
            manifest.path,
            "configuration projection record signal is ambiguous",
        )
        selected_contract = record_matches[0]
        selected_bundle = selected_contract["selector_bundle"]
        conflicting_selectors = sorted(
            contract["candidate_selector"]
            for contract in selector_matches
            if contract["candidate_selector"] not in selected_bundle
        )
        _require(
            not conflicting_selectors,
            manifest.path,
            "configuration projection record and candidate-selector signals conflict",
        )
    elif selector_matches:
        signalled_selectors = {
            contract["candidate_selector"] for contract in selector_matches
        }
        candidates = [
            contract
            for contract in selector_matches
            if signalled_selectors <= set(contract["selector_bundle"])
        ]
        _require(
            len(candidates) == 1,
            manifest.path,
            "configuration projection candidate-selector signals are ambiguous",
        )
        selected_contract = candidates[0]
    else:
        _require(
            not artifact_signal,
            manifest.path,
            "configuration projection artifacts lack a recognized record or selector signal",
        )

    if selected_contract is not None:
        candidate_selector = selected_contract["candidate_selector"]
        selector_bundle = selected_contract["selector_bundle"]
        effective_selector_bundle = selected_contract.get(
            "effective_selector_bundle", selector_bundle
        )
        _require(
            candidate_selector in expected_selectors
            and type(expected_selectors[candidate_selector]) is bool
            and expected_selectors[candidate_selector] is True
            and candidate_selector in effective_selectors
            and type(effective_selectors[candidate_selector]) is bool
            and effective_selectors[candidate_selector] is True,
            manifest.path,
            "configuration projection candidate selector must be the exact JSON boolean true",
        )
        lineage = manifest.lineage
        scope_matches = (
            manifest.protocol["protocol_id"]
            == selected_contract["protocol_id"]
            and manifest.provenance["record_id"]
            == selected_contract["record_id"]
            and lineage["generation"] == selected_contract["generation"]
            and lineage["parent_record_id"]
            == selected_contract["parent_record_id"]
            and lineage["original_role"]
            == selected_contract["original_role"]
            and set(expected_selectors) == set(selector_bundle)
            and set(effective_selectors) == set(effective_selector_bundle)
            and all(
                type(expected_selectors[name]) is type(expected)
                and expected_selectors[name] == expected
                for name, expected in selector_bundle.items()
            )
            and all(
                type(effective_selectors[name]) is type(expected)
                and effective_selectors[name] == expected
                for name, expected in effective_selector_bundle.items()
            )
        )
        _require(
            scope_matches,
            manifest.path,
            "configuration projection is outside its exact protocol, record, lineage, and typed selector-bundle scope",
        )
        _require(
            selected_contract.get("projection_configured", True) is True,
            manifest.path,
            "configuration projection transform is not preregistered",
        )
        _require(
            present_projection_names == projection_names
            and physical_projection_names == projection_names,
            manifest.path,
            "configuration projection record is missing its sealed artifacts",
        )
        for name in sorted(projection_names):
            path = projection_paths[name]
            _manifest_readonly(path)
            _require(
                stat.S_IMODE(path.stat().st_mode) == 0o444,
                path,
                "configuration projection artifact mode must be exactly 0444",
            )
            _require(
                _sha_file(path) == artifacts[name],
                path,
                f"sealed configuration projection artifact hash mismatch for {name}",
            )
        if manifest.provenance["record_id"] == ALC_CANDIDATE_RECORD_ID:
            _require(
                selected_contract == ALC_CONFIGURATION_PROJECTION_CONTRACT,
                manifest.path,
                "ALC record did not resolve its exact projected g3 contract",
            )
            _validate_alc_sealed_identity(manifest, selected_contract)
        elif (
            manifest.provenance["record_id"]
            == METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
        ):
            _require(
                selected_contract
                == METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT,
                manifest.path,
                "MSORE+MSFC+MDSSC record did not resolve its exact projected g4 contract",
            )
            _validate_metal_runtime_bundle_sealed_identity(
                manifest, selected_contract
            )
        elif manifest.provenance["record_id"] == NODA_CANDIDATE_RECORD_ID:
            _require(
                selected_contract == NODA_CONFIGURATION_PROJECTION_CONTRACT,
                manifest.path,
                "NODA record did not resolve its exact projected g5 contract",
            )
            _validate_noda_sealed_identity(manifest, selected_contract)
    migration_names = frozenset(CHECKPOINT_DECIMAL_MIGRATION_ARTIFACTS)
    present_migration_names = migration_names & frozenset(artifacts)
    _require(
        not present_migration_names or present_migration_names == migration_names,
        manifest.path,
        "checkpoint decimal migration artifacts must be present all-or-none",
    )
    for name in sorted(present_migration_names):
        path = root / CHECKPOINT_DECIMAL_MIGRATION_ARTIFACTS[name]
        _manifest_readonly(path)
        _require(
            _sha_file(path) == artifacts[name],
            path,
            f"sealed migration artifact hash mismatch for {name}",
        )


def _rebuild_report_value(
    root: pathlib.Path,
    report_path: pathlib.Path,
    raw: Mapping[str, Any],
) -> Tuple[Mapping[str, Any], Any, Any, Any]:
    schema = _exact(raw.get("schema"), frozenset({"name", "version"}), report_path, "report.schema")
    _require(schema == {"name": REPORT_SCHEMA_NAME, "version": REPORT_SCHEMA_VERSION}, report_path, "unsupported report schema")
    _require(raw.get("valid") is True and raw.get("offline") is True, report_path, "report is not a valid offline comparison")
    promotion = _exact(raw.get("promotion"), frozenset({"automatic", "performed", "message"}), report_path, "report.promotion")
    _require(promotion["automatic"] is False and promotion["performed"] is False, report_path, "report claims a promotion")
    records = _exact(raw.get("records"), frozenset({"baseline", "candidate"}), report_path, "report.records")
    baseline_summary = records["baseline"]
    candidate_summary = records["candidate"]
    _require(type(baseline_summary) is dict and type(candidate_summary) is dict, report_path, "record summaries must be objects")
    baseline_path = pathlib.Path(_string(baseline_summary.get("manifest"), report_path, "baseline manifest")).resolve()
    candidate_path = pathlib.Path(_string(candidate_summary.get("manifest"), report_path, "candidate manifest")).resolve()
    records_root = (root / "records").resolve()
    _require(_inside(baseline_path, records_root) and _inside(candidate_path, records_root), report_path, "report manifests must be sealed records in this registry")
    _manifest_readonly(baseline_path)
    _manifest_readonly(candidate_path)
    _, report_tool_path = _tool_paths()
    report_tool = _load_module(report_tool_path, "persistent-performance-report")
    try:
        baseline = report_tool.load_manifest(baseline_path)
        candidate = report_tool.load_manifest(candidate_path)
        _verify_record_artifacts(baseline)
        _verify_record_artifacts(candidate)
        expected = report_tool.build_report(baseline, candidate)
    except Exception as error:
        raise RegistryError(f"{report_path}: report inputs are invalid: {error}") from error
    _require(_canonical_bytes(raw) == _canonical_bytes(expected), report_path, "report does not reproduce byte-for-byte from sealed manifests")
    _require(baseline_summary.get("manifest_sha256") == _sha_file(baseline_path), report_path, "baseline manifest hash mismatch")
    _require(candidate_summary.get("manifest_sha256") == _sha_file(candidate_path), report_path, "candidate manifest hash mismatch")
    return raw, report_tool, baseline, candidate


def _load_and_rebuild_report(root: pathlib.Path, report_path: pathlib.Path) -> Tuple[Mapping[str, Any], Any, Any, Any]:
    absolute, payload = _read_stable_report_payload(report_path)
    return _rebuild_report_value(root, absolute, _load_json_payload(payload, absolute))


def _archive_and_rebuild_report(
    root: pathlib.Path,
    report_path: pathlib.Path,
) -> Tuple[pathlib.Path, str, Mapping[str, Any], Any, Any, Any]:
    origin, payload = _read_stable_report_payload(report_path)
    raw, report_tool, baseline, candidate = _rebuild_report_value(
        root, origin, _load_json_payload(payload, origin)
    )
    digest = hashlib.sha256(payload).hexdigest()
    archive = root / "reports" / f"{digest}.json"
    if archive.exists():
        archived_path, archived_payload = _read_stable_report_payload(archive)
        _require(archived_path == archive.resolve(), archive, "archive path resolution mismatch")
        _require(archived_payload == payload, archive, "content-addressed report archive conflicts")
    else:
        _atomic_bytes(archive, payload)
        _, archived_payload = _read_stable_report_payload(archive)
        _require(archived_payload == payload, archive, "archived report readback mismatch")
    _require(_sha_file(archive) == digest, archive, "archived report hash mismatch")
    return archive.resolve(), digest, raw, report_tool, baseline, candidate


def _pointer_value(root: pathlib.Path) -> Optional[Mapping[str, Any]]:
    path = root / "current-baseline.json"
    if not path.exists():
        return None
    _manifest_readonly(path)
    pointer = _exact(
        _load_json(path),
        frozenset({"schema", "protocol_id", "generation", "record_id", "parent_record_id", "manifest", "manifest_sha256", "decision_id"}),
        path,
        "pointer",
    )
    schema = _exact(pointer["schema"], frozenset({"name", "version"}), path, "pointer.schema")
    _require(schema == {"name": POINTER_SCHEMA_NAME, "version": POINTER_SCHEMA_VERSION}, path, "unsupported pointer schema")
    protocol_id = _safe_id(pointer["protocol_id"], path, "pointer.protocol_id")
    record_id = _safe_id(pointer["record_id"], path, "pointer.record_id")
    generation = pointer["generation"]
    _require(type(generation) is int and generation >= 0, path, "invalid pointer generation")
    parent_record_id = pointer["parent_record_id"]
    if generation == 0:
        _require(
            parent_record_id is None,
            path,
            "generation-zero pointer must be parentless",
        )
    else:
        _safe_id(parent_record_id, path, "pointer.parent_record_id")
    manifest_text = _string(pointer["manifest"], path, "pointer.manifest")
    expected_manifest_text = f"records/{record_id}/manifest.json"
    _require(
        manifest_text == expected_manifest_text,
        path,
        "pointer manifest is not the canonical record descriptor path",
    )
    manifest_sha256 = _sha(
        pointer["manifest_sha256"], path, "pointer.manifest_sha256"
    )
    decision_id = _safe_id(pointer["decision_id"], path, "pointer.decision_id")
    manifest_path = root.joinpath(*pathlib.PurePosixPath(manifest_text).parts)
    _manifest_readonly(manifest_path)
    _require(
        _sha_file(manifest_path) == manifest_sha256,
        manifest_path,
        "pointer manifest hash differs from its canonical descriptor",
    )
    descriptor = {
        "generation": generation,
        "record_id": record_id,
        "parent_record_id": parent_record_id,
        "manifest": manifest_text,
        "manifest_sha256": manifest_sha256,
    }
    generation_path = root / "generations" / f"g{generation:06d}.json"
    _manifest_readonly(generation_path)
    _require(
        _load_json(generation_path)
        == _generation_value(root, protocol_id, descriptor, decision_id),
        generation_path,
        "pointer differs from its immutable generation descriptor",
    )
    return pointer


def _record_descriptor(root: pathlib.Path, manifest: Any) -> Mapping[str, Any]:
    path = manifest.path.resolve()
    record_id = _safe_id(
        manifest.provenance["record_id"], manifest.path, "provenance.record_id"
    )
    expected_path = (
        root.resolve() / "records" / record_id / "manifest.json"
    )
    _require(
        path == expected_path,
        manifest.path,
        "manifest is outside its canonical registry record descriptor path",
    )
    return {
        "generation": manifest.lineage["generation"],
        "record_id": record_id,
        "parent_record_id": manifest.lineage["parent_record_id"],
        "manifest": path.relative_to(root.resolve()).as_posix(),
        "manifest_sha256": _sha_file(path),
    }


def _validate_decision_context(root: pathlib.Path, epoch: Mapping[str, Any], baseline: Any, candidate: Any) -> Optional[Mapping[str, Any]]:
    _require(baseline.protocol == epoch["protocol"] and candidate.protocol == epoch["protocol"], root, "report protocol differs from epoch")
    _require(baseline.comparability == epoch["comparability"] and candidate.comparability == epoch["comparability"], root, "report comparability differs from epoch")
    pointer = _pointer_value(root)
    if pointer is None:
        _require(baseline.lineage["generation"] == 0 and baseline.lineage["parent_record_id"] is None and baseline.lineage["original_role"] == "production", root, "initial decision requires generation-zero production B")
        _require(
            baseline.provenance["binary_sha256"]
            == candidate.provenance["binary_sha256"],
            root,
            "initial B/C decision requires the same Mach-O SHA-256",
        )
    else:
        baseline_descriptor = _record_descriptor(root, baseline)
        _require(
            pointer
            == _pointer_for(
                epoch["protocol"]["protocol_id"],
                baseline_descriptor,
                pointer["decision_id"],
            ),
            root,
            "current baseline pointer differs from the report's complete canonical baseline descriptor",
        )
    _require(candidate.lineage["generation"] == baseline.lineage["generation"] + 1, root, "candidate generation is not parent+1")
    _require(candidate.lineage["parent_record_id"] == baseline.provenance["record_id"], root, "candidate parent is not current baseline")
    return pointer


class _Lock:
    def __init__(self, path: pathlib.Path):
        self.path = path

    def __enter__(self) -> "_Lock":
        try:
            self.path.mkdir()
        except FileExistsError as error:
            raise RegistryError(f"{self.path}: registry mutation already active or stale lock present") from error
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        try:
            self.path.rmdir()
        except OSError:
            pass


def _generation_value(root: pathlib.Path, protocol_id: str, descriptor: Mapping[str, Any], decision_id: str) -> Mapping[str, Any]:
    return {
        "schema": {"name": GENERATION_SCHEMA_NAME, "version": GENERATION_SCHEMA_VERSION},
        "protocol_id": protocol_id,
        **descriptor,
        "decision_id": decision_id,
    }


def _ensure_generation(root: pathlib.Path, protocol_id: str, descriptor: Mapping[str, Any], decision_id: str) -> pathlib.Path:
    path = root / "generations" / f"g{descriptor['generation']:06d}.json"
    expected = _generation_value(root, protocol_id, descriptor, decision_id)
    if path.exists():
        _require(_load_json(path) == expected, path, "existing generation conflicts with decision")
    else:
        _atomic_json(path, expected)
    return path


def _pointer_for(protocol_id: str, descriptor: Mapping[str, Any], decision_id: str) -> Mapping[str, Any]:
    return {
        "schema": {"name": POINTER_SCHEMA_NAME, "version": POINTER_SCHEMA_VERSION},
        "protocol_id": protocol_id,
        **descriptor,
        "decision_id": decision_id,
    }


def _decision_value(
    decision: str,
    decision_id: str,
    protocol_id: str,
    report_path: pathlib.Path,
    baseline: Mapping[str, Any],
    candidate: Mapping[str, Any],
    prior_pointer_sha256: Optional[str],
    attestation: str,
) -> Mapping[str, Any]:
    return {
        "schema": {"name": DECISION_SCHEMA_NAME, "version": DECISION_SCHEMA_VERSION},
        "decision_id": decision_id,
        "decision": decision,
        "protocol_id": protocol_id,
        "report": str(report_path.resolve()),
        "report_sha256": _sha_file(report_path),
        "baseline": baseline,
        "candidate": candidate,
        "prior_pointer_sha256": prior_pointer_sha256,
        "attestation": attestation,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }


def _validate_existing_decision(
    path: pathlib.Path,
    *,
    decision: str,
    decision_id: str,
    protocol_id: str,
    report_path: pathlib.Path,
    report_sha256: str,
    baseline: Mapping[str, Any],
    candidate: Mapping[str, Any],
    attestation: str,
) -> Mapping[str, Any]:
    value = _exact(
        _load_json(path),
        frozenset(
            {
                "schema",
                "decision_id",
                "decision",
                "protocol_id",
                "report",
                "report_sha256",
                "baseline",
                "candidate",
                "prior_pointer_sha256",
                "attestation",
                "created_utc",
            }
        ),
        path,
        "decision",
    )
    schema = _exact(value["schema"], frozenset({"name", "version"}), path, "decision.schema")
    _require(
        schema == {"name": DECISION_SCHEMA_NAME, "version": DECISION_SCHEMA_VERSION},
        path,
        "unsupported decision schema",
    )
    _require(value["decision"] == decision, path, "existing decision kind differs from retry")
    _require(value["decision_id"] == decision_id, path, "existing decision id differs from retry")
    _require(value["protocol_id"] == protocol_id, path, "existing decision protocol differs from retry")
    _require(value["report"] == str(report_path.resolve()), path, "existing decision report archive differs from retry")
    _require(value["report_sha256"] == report_sha256, path, "existing decision report hash differs from retry")
    _require(value["baseline"] == baseline and value["candidate"] == candidate, path, "existing decision lineage differs from retry")
    _require(value["attestation"] == attestation, path, "existing decision attestation differs from retry")
    _require(
        value["prior_pointer_sha256"] is None
        or (
            type(value["prior_pointer_sha256"]) is str
            and HEX_SHA256_RE.fullmatch(value["prior_pointer_sha256"]) is not None
        ),
        path,
        "invalid prior pointer hash",
    )
    _string(value["created_utc"], path, "decision.created_utc")
    return value


def decide(
    root: pathlib.Path,
    report_path: pathlib.Path,
    decision: str,
    attestation: str,
    *,
    fail_before_pointer: bool = False,
    fail_after_pointer: bool = False,
) -> pathlib.Path:
    root = root.expanduser().resolve()
    report_path = report_path.expanduser().resolve()
    _require(decision in ("promote", "reject"), decision, "unsupported decision")
    expected_attestation = "CONFIRM_PROMOTE" if decision == "promote" else "CONFIRM_REJECT"
    _require(attestation == expected_attestation, decision, f"exact {expected_attestation} attestation is required")
    epoch = _load_epoch(root)
    with _Lock(root / ".registry.lock"):
        _require(not any((root / "pending").iterdir()), root, "cannot decide while a run is pending")
        archived_report, report_sha, _raw_report, _, baseline, candidate = _archive_and_rebuild_report(root, report_path)
        baseline_descriptor = _record_descriptor(root, baseline)
        candidate_descriptor = _record_descriptor(root, candidate)
        decision_id = f"{decision}-g{candidate_descriptor['generation']:06d}-{candidate_descriptor['record_id']}"
        _safe_id(decision_id, root, "decision_id")
        prior_pointer_path = root / "current-baseline.json"
        pointer = _pointer_value(root)
        decision_path = root / "decisions" / f"{decision_id}.json"
        opposite = "reject" if decision == "promote" else "promote"
        opposite_decision_id = (
            f"{opposite}-g{candidate_descriptor['generation']:06d}-{candidate_descriptor['record_id']}"
        )
        opposite_decision_path = root / "decisions" / f"{opposite_decision_id}.json"
        _require(
            not opposite_decision_path.exists(),
            opposite_decision_path,
            "candidate already has the opposite immutable decision",
        )
        if decision_path.exists():
            _validate_existing_decision(
                decision_path,
                decision=decision,
                decision_id=decision_id,
                protocol_id=epoch["protocol"]["protocol_id"],
                report_path=archived_report,
                report_sha256=report_sha,
                baseline=baseline_descriptor,
                candidate=candidate_descriptor,
                attestation=attestation,
            )
            if decision == "reject" and pointer is not None:
                _require(
                    pointer["generation"] > baseline_descriptor["generation"]
                    or (
                        pointer["generation"] == baseline_descriptor["generation"]
                        and pointer["record_id"] == baseline_descriptor["record_id"]
                    ),
                    prior_pointer_path,
                    "retry observes a pointer older than or divergent from rejected baseline",
                )
                return decision_path
            if decision == "promote" and pointer is not None:
                if pointer["generation"] > candidate_descriptor["generation"]:
                    _ensure_generation(root, epoch["protocol"]["protocol_id"], candidate_descriptor, decision_id)
                    return prior_pointer_path
                if (
                    pointer["generation"] == candidate_descriptor["generation"]
                    and pointer["record_id"] == candidate_descriptor["record_id"]
                ):
                    _require(
                        pointer == _pointer_for(epoch["protocol"]["protocol_id"], candidate_descriptor, decision_id),
                        prior_pointer_path,
                        "retry candidate pointer differs from committed decision",
                    )
                    _ensure_generation(root, epoch["protocol"]["protocol_id"], candidate_descriptor, decision_id)
                    return prior_pointer_path
            pointer = _validate_decision_context(root, epoch, baseline, candidate)
        else:
            pointer = _validate_decision_context(root, epoch, baseline, candidate)
            prior_pointer_sha = _sha_file(prior_pointer_path) if prior_pointer_path.exists() else None
            value = _decision_value(decision, decision_id, epoch["protocol"]["protocol_id"], archived_report, baseline_descriptor, candidate_descriptor, prior_pointer_sha, attestation)
            _atomic_json(decision_path, value)

        if pointer is None:
            _ensure_generation(root, epoch["protocol"]["protocol_id"], baseline_descriptor, decision_id)

        if decision == "reject":
            if pointer is None:
                _atomic_json(prior_pointer_path, _pointer_for(epoch["protocol"]["protocol_id"], baseline_descriptor, decision_id), replace=True)
                if fail_after_pointer:
                    raise RegistryError("injected failure after atomic baseline pointer replace")
            else:
                existing_decision = _load_json(decision_path)
                _require(
                    _sha_file(prior_pointer_path) == existing_decision["prior_pointer_sha256"],
                    prior_pointer_path,
                    "reject changed current baseline",
                )
            return decision_path

        _ensure_generation(root, epoch["protocol"]["protocol_id"], candidate_descriptor, decision_id)
        if fail_before_pointer:
            raise RegistryError("injected failure before atomic baseline pointer replace")
        target_pointer = _pointer_for(epoch["protocol"]["protocol_id"], candidate_descriptor, decision_id)
        if prior_pointer_path.exists():
            current = _pointer_value(root)
            if current is not None and current["record_id"] == candidate_descriptor["record_id"] and current["generation"] == candidate_descriptor["generation"]:
                return prior_pointer_path
        _atomic_json(prior_pointer_path, target_pointer, replace=True)
        if fail_after_pointer:
            raise RegistryError("injected failure after atomic baseline pointer replace")
        return prior_pointer_path


def promote(root: pathlib.Path, report_path: pathlib.Path, attestation: str) -> pathlib.Path:
    return decide(root, report_path, "promote", attestation)


def reject(root: pathlib.Path, report_path: pathlib.Path, attestation: str) -> pathlib.Path:
    return decide(root, report_path, "reject", attestation)


def _write_manifest(root: pathlib.Path, report_tool: Any, value: Mapping[str, Any]) -> pathlib.Path:
    record_id = value["provenance"]["record_id"]
    directory = root / "records" / record_id
    directory.mkdir()
    (directory / "anchors").mkdir()
    (directory / "evidence").mkdir()
    evidence_payloads = {
        "fort-anchor.png": b"\x89PNG\r\n\x1a\nfort-anchor-" + record_id.encode("ascii"),
        "fort-complete.png": b"\x89PNG\r\n\x1a\nfort-complete-" + record_id.encode("ascii"),
        "game-anchor-a.png": b"\x89PNG\r\n\x1a\ngame-anchor-a-" + record_id.encode("ascii"),
        "game-anchor-b.png": b"\x89PNG\r\n\x1a\ngame-anchor-b-" + record_id.encode("ascii"),
        "game-complete.png": b"\x89PNG\r\n\x1a\ngame-complete-" + record_id.encode("ascii"),
    }
    evidence_receipts = {}
    for name, payload in evidence_payloads.items():
        evidence_path = directory / "evidence" / name
        _atomic_bytes(evidence_path, payload)
        evidence_receipts[name] = {
            "file": f"evidence/{name}",
            "sha256": _sha_file(evidence_path),
        }
    anchor_payloads = {
        "fort_anchor_receipt": {
            "anchor_evidence": [evidence_receipts["fort-anchor.png"]],
            "completion_evidence": [evidence_receipts["fort-complete.png"]],
        },
        "in_game_anchor_receipt": {
            "anchor_evidence": [
                evidence_receipts["game-anchor-a.png"],
                evidence_receipts["game-anchor-b.png"],
            ],
            "completion_evidence": [evidence_receipts["game-complete.png"]],
        },
    }
    stdout_payload = f"stdout {record_id}\n".encode()
    stderr_payload = f"stderr {record_id}\n".encode()
    start_config_payload = b"{}\n"
    if record_id in {
        UFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID,
        FCFOS_DIRECT_CANDIDATE_RECORD_ID,
    }:
        selector_receipt_fields = {
            CONFIGURATION_PROJECTION_STABLE_SELECTOR: "stable_negative_route",
            CONFIGURATION_PROJECTION_SELECTOR: (
                "guest_memory_operation_snapshot_elision"
            ),
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: (
                "fast_block_dense_directory"
            ),
            PHASEIFSM_CANDIDATE_SELECTOR: (
                "fast_block_inline_fixed_signature_match"
            ),
            UFSB_CANDIDATE_SELECTOR: (
                "fast_block_unrolled_fixed_signature_batching"
            ),
            IMSE_COMPANION_SELECTOR: (
                "fast_block_idempotent_mru_store_elision"
            ),
            FCFOS_CANDIDATE_SELECTOR: (
                "fast_block_callback_free_ordinary_specialization"
            ),
        }
        selectors = value["provenance"]["selectors"]["expected"]
        if record_id == UFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID:
            candidate_selector = UFSB_CANDIDATE_SELECTOR
            marker_prefix = UFSB_PRIMARY_MARKER_PREFIX
            marker_tokens = UFSB_PRIMARY_MARKER_TOKENS
            marker_lines = (
                UFSB_PRIMARY_MARKER_LINE,
                IMSE_COMPANION_MARKER_LINE,
            )
        else:
            candidate_selector = FCFOS_CANDIDATE_SELECTOR
            marker_prefix = FCFOS_MARKER_PREFIX
            marker_tokens = FCFOS_MARKER_TOKENS
            marker_lines = (FCFOS_MARKER_LINE,)
        start_config = {
            "schema": {
                "name": "lumen.persistent-performance-start",
                "version": 1,
            },
            "protocol": value["protocol"],
            "comparability": value["comparability"],
            "provenance": {
                "record_id": record_id,
                "run_receipt": value["provenance"]["run_receipt"],
                "candidate_selector": candidate_selector,
                "selectors": dict(selectors),
                "selector_receipts": {
                    name: {
                        "prefix": A64_CONFIGURATION_PREFIX,
                        "field": selector_receipt_fields[name],
                    }
                    for name in selectors
                },
            },
            "lineage": value["lineage"],
            "candidate_marker": {
                "required": True,
                "prefix": marker_prefix,
                "required_tokens": list(marker_tokens),
                "coverage_required": True,
                "coverage_token": "coverage=positive",
            },
            "launch": {
                "arguments": [],
                "common_environment": {},
                "experiment_environment": {
                    name: "1" for name in selectors
                },
            },
        }
        start_config_payload = (
            json.dumps(
                start_config,
                ensure_ascii=False,
                allow_nan=False,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")
            + b"\n"
        )
        stderr_payload += ("\n".join(marker_lines) + "\n").encode("utf-8")
    payloads = {
        "stdout_log": (directory / "stdout.log", stdout_payload),
        "stderr_log": (directory / "stderr.log", stderr_payload),
        "exit_status": (directory / ".exit_status", b"0\n"),
        "fort_anchor_receipt": (
            directory / "anchors" / "fort_mode.json",
            json.dumps(anchor_payloads["fort_anchor_receipt"], sort_keys=True, indent=2).encode("utf-8") + b"\n",
        ),
        "in_game_anchor_receipt": (
            directory / "anchors" / "in_game.json",
            json.dumps(anchor_payloads["in_game_anchor_receipt"], sort_keys=True, indent=2).encode("utf-8") + b"\n",
        ),
        "start_config": (directory / "start-config.json", start_config_payload),
        "checkpoint_observation": (
            directory / "checkpoint-observation.json",
            b"{}\n",
        ),
        "process_time": (directory / "process-time.json", b"{}\n"),
        "exit_uptime_raw": (directory / "exit-uptime-raw-ns.txt", b"999999999\n"),
    }
    artifacts = value["provenance"]["artifacts_sha256"]
    for name, (artifact_path, payload) in payloads.items():
        _atomic_bytes(artifact_path, payload)
        artifacts[name] = _sha_file(artifact_path)
    receipts = {
        "save_special_before": directory / "save-special-before.sha256",
        "save_special_after": directory / "save-special-after.sha256",
        "save_template_before": directory / "save-template-before.sha256",
        "save_template_after": directory / "save-template-after.sha256",
    }
    for name, receipt_path in receipts.items():
        _atomic_bytes(receipt_path, (artifacts[name] + "\n").encode("ascii"))
    path = directory / "manifest.json"
    _atomic_json(path, value)
    report_tool.load_manifest(path)
    return path


def _write_report(path: pathlib.Path, report_tool: Any, baseline_path: pathlib.Path, candidate_path: pathlib.Path) -> None:
    report = report_tool.build_report(report_tool.load_manifest(baseline_path), report_tool.load_manifest(candidate_path))
    _atomic_json(path, report)


def _fixture_epoch(report_tool: Any, protocol_id: str) -> Mapping[str, Any]:
    fixture = report_tool._fixture_manifest(
        record_id="epoch-fixture",
        receipt="epoch-receipt",
        generation=0,
        parent=None,
        role="production",
        wall_ns=10,
        cumulative_ms=10000,
        fort_interval_ms=10,
        game_interval_ms=10,
        revision_digit="a",
        selector_enabled=False,
    )
    protocol = dict(fixture["protocol"])
    protocol["protocol_id"] = protocol_id
    comparability = dict(fixture["comparability"])
    comparability["workload"] = dict(comparability["workload"])
    comparability["workload"]["cache_policy"] = "fresh-empty"
    comparability["workload"]["cache_seed_sha256"] = None
    return {
        "schema": {"name": EPOCH_SCHEMA_NAME, "version": EPOCH_SCHEMA_VERSION},
        "protocol": protocol,
        "comparability": comparability,
    }


def _fixture_pair(
    root: pathlib.Path,
    report_tool: Any,
    epoch: Mapping[str, Any],
    suffix: str = "",
    *,
    same_mach_o: bool = True,
) -> Tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
    baseline_id = f"baseline{suffix}"
    candidate_id = f"candidate{suffix}"
    baseline = report_tool._fixture_manifest(
        record_id=baseline_id,
        receipt=f"baseline-receipt{suffix}",
        generation=0,
        parent=None,
        role="production",
        wall_ns=100,
        cumulative_ms=10000,
        fort_interval_ms=10,
        game_interval_ms=10,
        revision_digit="b",
        selector_enabled=False,
    )
    candidate = report_tool._fixture_manifest(
        record_id=candidate_id,
        receipt=f"candidate-receipt{suffix}",
        generation=1,
        parent=baseline_id,
        role="candidate",
        wall_ns=80,
        cumulative_ms=8000,
        fort_interval_ms=8,
        game_interval_ms=8,
        revision_digit="c",
        selector_enabled=True,
    )
    for value in (baseline, candidate):
        value["protocol"] = epoch["protocol"]
        value["comparability"] = epoch["comparability"]
    if same_mach_o:
        candidate["provenance"]["binary_sha256"] = baseline["provenance"][
            "binary_sha256"
        ]
    baseline_path = _write_manifest(root, report_tool, baseline)
    candidate_path = _write_manifest(root, report_tool, candidate)
    report_path = root / "reports" / f"initial{suffix}.json"
    _write_report(report_path, report_tool, baseline_path, candidate_path)
    return baseline_path, candidate_path, report_path


def _self_test() -> None:
    _, report_path = _tool_paths()
    report_tool = _load_module(report_path, "persistent-performance-report")
    with tempfile.TemporaryDirectory(prefix="persistent-performance-registry-self-test-") as temporary:
        parent = pathlib.Path(temporary)

        _require(
            CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": "6cp-c-guest-memory-operation-snapshot-elision-g000002",
                "generation": 2,
                "parent_record_id": "6cl-c-stable-negative-g000001",
                "original_role": "candidate",
                "candidate_selector": CONFIGURATION_PROJECTION_SELECTOR,
                "selector_bundle": {
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                    CONFIGURATION_PROJECTION_SELECTOR: True,
                },
            },
            "self-test",
            "configuration projection registry scope drifted",
        )
        _require(
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": "6cq-c-scalar-memory-fallthrough-admission-g000003",
                "generation": 3,
                "parent_record_id": "6cp-c-guest-memory-operation-snapshot-elision-g000002",
                "original_role": "candidate",
                "candidate_selector": PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR,
                "selector_bundle": {
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                    CONFIGURATION_PROJECTION_SELECTOR: True,
                    PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR: True,
                },
                "raw_bytes": 46_394,
                "raw_sha256": "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a",
                "restore_operation_count": 8,
                "restore_operations_contract_bytes": 4_788,
                "restore_operations_contract_sha256": "ee36e477af8d1218a7b254d078edf9841a4a233597790bba8bcffa0177c3c04c",
                "restored_bytes": 49_567,
                "restored_sha256": "6d4b7dd83b09e0999208ad8787634be07ed8079713ca20ece0c41ce497ea5c35",
                "projected_bytes": 49_321,
                "projected_sha256": "45959a4495ea2a835853f6af52e2d1f43cf60d8437cb2bbe63520da8324ed29a",
                "projection_configured": True,
            },
            "self-test",
            "6CQ configuration projection registry scope drifted",
        )
        _require(
            PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": "6cs-c-dead-rflags-elision-g000003",
                "generation": 3,
                "parent_record_id": "6cp-c-guest-memory-operation-snapshot-elision-g000002",
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
            and PHASE6CS_CONFIGURATION_RESTORE_CONTRACT_PREFIX
            == b"lumen.phase6cs.configuration-restore-operations.v1\0",
            "self-test",
            "6CS configuration projection registry scope drifted",
        )
        _require(
            PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": "6da-c-fast-block-dense-directory-g000003",
                "generation": 3,
                "parent_record_id": "6cp-c-guest-memory-operation-snapshot-elision-g000002",
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
            == b"lumen.phase6da.configuration-restore-operations.v1\0",
            "self-test",
            "6DA configuration projection registry scope drifted",
        )
        _require(
            ALC_CONFIGURATION_PROJECTION_CONTRACT
            == {
                "protocol_id": "macos-m1-worms-p2300-announced-p120-20260816-v1",
                "record_id": "alc-c-arm-loading-composite-g000003",
                "generation": 3,
                "parent_record_id": "ifsm-c-fast-block-inline-fixed-signature-match-g000002",
                "original_role": "candidate",
                "candidate_selector": ALC_CANDIDATE_SELECTOR,
                "selector_bundle": {
                    CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                    CONFIGURATION_PROJECTION_SELECTOR: True,
                    PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                    PHASEIFSM_CANDIDATE_SELECTOR: True,
                    ALC_CANDIDATE_SELECTOR: True,
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
                "required_receipt_fields": {
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
                "candidate_marker": {
                    "required": True,
                    "expected_count": 1,
                    "observed_count": 1,
                    "status": "clean",
                    "coverage_required": True,
                    "coverage_positive": True,
                    "line": ALC_MARKER_LINE,
                },
            }
            and ALC_MARKER_LINE
            == (
                "[A64_ARM_LOADING_COMPOSITE_FINAL] purge=positive "
                "adaptive_snapshot=positive omit_cpu_frame_pointer=positive "
                "ifsm_rollback=positive requested=positive status=clean "
                "coverage=positive"
            ),
            "self-test",
            "ALC configuration projection registry contract drifted",
        )
        _require(
            CONFIGURATION_PROJECTION_CONTRACTS
            == (
                CONFIGURATION_PROJECTION_CONTRACT,
                PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT,
                PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT,
                PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT,
                ALC_CONFIGURATION_PROJECTION_CONTRACT,
                METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT,
                NODA_CONFIGURATION_PROJECTION_CONTRACT,
            ),
            "self-test",
            "configuration projection contract dispatch order drifted",
        )
        _require(
            PHASE6DA_DIRECT_CONTRACTS
            == (
                {
                    "protocol_id": PHASE6DA_DIRECT_PROTOCOL_ID,
                    "record_id": PHASE6DA_DIRECT_BASELINE_RECORD_ID,
                    "generation": 0,
                    "parent_record_id": None,
                    "original_role": "production",
                    "os": {
                        "name": "macOS",
                        "version": PHASE6DA_DIRECT_OS_VERSION,
                        "build": PHASE6DA_DIRECT_OS_BUILD,
                        "policy": PHASE6DA_DIRECT_OS_POLICY,
                    },
                    "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
                    "selector_bundle": {
                        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                        CONFIGURATION_PROJECTION_SELECTOR: True,
                        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: False,
                    },
                    "required_receipt_fields": {
                        "stable_negative_route": "1",
                        "guest_memory_operation_snapshot_elision": "1",
                        "scalar_memory_fallthrough_admission": "0",
                        "fast_block_dead_rflags_elision": "0",
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
                    "os": {
                        "name": "macOS",
                        "version": PHASE6DA_DIRECT_OS_VERSION,
                        "build": PHASE6DA_DIRECT_OS_BUILD,
                        "policy": PHASE6DA_DIRECT_OS_POLICY,
                    },
                    "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
                    "selector_bundle": {
                        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                        CONFIGURATION_PROJECTION_SELECTOR: True,
                        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                    },
                    "required_receipt_fields": {
                        "stable_negative_route": "1",
                        "guest_memory_operation_snapshot_elision": "1",
                        "scalar_memory_fallthrough_admission": "0",
                        "fast_block_dead_rflags_elision": "0",
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
                    "os": {
                        "name": "macOS",
                        "version": PHASE6DA_DIRECT_OS_VERSION,
                        "build": PHASE6DA_DIRECT_OS_BUILD,
                        "policy": PHASE6DA_DIRECT_OS_POLICY,
                    },
                    "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
                    "selector_bundle": {
                        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                        CONFIGURATION_PROJECTION_SELECTOR: True,
                        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                        PHASEIFSM_CANDIDATE_SELECTOR: True,
                    },
                    "required_receipt_fields": {
                        "stable_negative_route": "1",
                        "guest_memory_operation_snapshot_elision": "1",
                        "scalar_memory_fallthrough_admission": "0",
                        "fast_block_dead_rflags_elision": "0",
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
                    "record_id": UFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID,
                    "generation": 3,
                    "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
                    "original_role": "candidate",
                    "os": {
                        "name": "macOS",
                        "version": PHASE6DA_DIRECT_OS_VERSION,
                        "build": PHASE6DA_DIRECT_OS_BUILD,
                        "policy": PHASE6DA_DIRECT_OS_POLICY,
                    },
                    "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
                    "primary_selector": UFSB_CANDIDATE_SELECTOR,
                    "companion_selectors": frozenset(
                        {IMSE_COMPANION_SELECTOR}
                    ),
                    "selector_bundle": {
                        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                        CONFIGURATION_PROJECTION_SELECTOR: True,
                        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                        PHASEIFSM_CANDIDATE_SELECTOR: True,
                        UFSB_CANDIDATE_SELECTOR: True,
                        IMSE_COMPANION_SELECTOR: True,
                    },
                    "required_receipt_fields": {
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
                    "record_id": FCFOS_DIRECT_CANDIDATE_RECORD_ID,
                    "generation": 3,
                    "parent_record_id": PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
                    "original_role": "candidate",
                    "os": {
                        "name": "macOS",
                        "version": PHASE6DA_DIRECT_OS_VERSION,
                        "build": PHASE6DA_DIRECT_OS_BUILD,
                        "policy": PHASE6DA_DIRECT_OS_POLICY,
                    },
                    "configuration_sha256": PHASE6DA_DIRECT_CONFIGURATION_SHA256,
                    "selector_bundle": {
                        CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
                        CONFIGURATION_PROJECTION_SELECTOR: True,
                        PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: True,
                        PHASEIFSM_CANDIDATE_SELECTOR: True,
                        FCFOS_CANDIDATE_SELECTOR: True,
                    },
                    "required_receipt_fields": {
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
                    "candidate_marker": {
                        "required": True,
                        "expected_count": 1,
                        "observed_count": 1,
                        "status": "clean",
                        "coverage_required": True,
                        "coverage_positive": True,
                        "line": FCFOS_MARKER_LINE,
                    },
                },
            ),
            "self-test",
            "phase-6DA direct epoch contract drifted",
        )
        _require(
            PHASE6DA_DIRECT_PROTOCOL_ID
            == "macos-m1-worms-p2300-announced-p120-20260816-v1"
            and PHASE6DA_DIRECT_OS_VERSION == "26.6.2"
            and PHASE6DA_DIRECT_OS_BUILD == "25G83"
            and PHASE6DA_DIRECT_OS_POLICY
            == "fixed-version-and-build-within-epoch"
            and PHASE6DA_DIRECT_CONFIGURATION_BYTES == 46_394
            and PHASE6DA_DIRECT_CONFIGURATION_SHA256
            == "cce9fda0892f647b618ec82043eca7bb8ff3630a183841c18965cdff163ae07a"
            and PHASEIFSM_CANDIDATE_SELECTOR
            == "LUMEN_X64_A64_FAST_BLOCK_INLINE_FIXED_SIGNATURE_MATCH"
            and PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            == "ifsm-c-fast-block-inline-fixed-signature-match-g000002"
            and FCFOS_CANDIDATE_SELECTOR
            == "LUMEN_X64_A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION"
            and FCFOS_DIRECT_CANDIDATE_RECORD_ID
            == "fcfos-c-fast-block-callback-free-ordinary-specialization-g000003"
            and FCFOS_MARKER_LINE
            == (
                "[A64_FAST_BLOCK_CALLBACK_FREE_ORDINARY_SPECIALIZATION_FINAL] "
                "requested=positive effective=positive "
                "reentry_safe=positive snapshot_elision=positive "
                "status=clean coverage=positive"
            )
            and UFSB_CANDIDATE_SELECTOR
            == "LUMEN_X64_A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING"
            and IMSE_COMPANION_SELECTOR
            == "LUMEN_X64_A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION"
            and UFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID
            == "ufsb-imse-c-fast-block-dispatch-bundle-g000003"
            and UFSB_PRIMARY_MARKER_LINE
            == (
                "[A64_FAST_BLOCK_UNROLLED_FIXED_SIGNATURE_BATCHING_FINAL] "
                "dependency=positive compared=positive chunks=positive "
                "status=clean coverage=positive"
            )
            and IMSE_COMPANION_MARKER_LINE
            == (
                "[A64_FAST_BLOCK_IDEMPOTENT_MRU_STORE_ELISION_FINAL] "
                "requested=positive effective=positive elided=positive "
                "status=clean coverage=positive"
            ),
            "self-test",
            "phase-6DA direct retained-protocol/OS epoch drifted",
        )

        epoch = _fixture_epoch(report_tool, "registry-promote-v1")
        epoch_config = parent / "epoch-promote.json"
        _atomic_json(epoch_config, epoch)
        root = parent / "promote-registry"
        init_registry(root, epoch_config)
        try:
            init_registry(root, epoch_config)
        except RegistryError:
            pass
        else:
            raise RegistryError("self-test accepted registry overwrite")

        # Two immutable publishers racing for one target must produce exactly
        # one winner; the loser may never replace the winner's inode.
        race_path = parent / "immutable-race.json"
        race_barrier = threading.Barrier(3)
        race_results = []
        race_results_lock = threading.Lock()

        def race_publish(payload: bytes) -> None:
            race_barrier.wait()
            try:
                _atomic_bytes(race_path, payload)
            except RegistryError:
                result = "rejected"
            else:
                result = "published"
            with race_results_lock:
                race_results.append(result)

        race_threads = [
            threading.Thread(target=race_publish, args=(payload,), daemon=True)
            for payload in (b"first\n", b"second\n")
        ]
        for thread in race_threads:
            thread.start()
        race_barrier.wait()
        for thread in race_threads:
            thread.join()
        _require(
            sorted(race_results) == ["published", "rejected"]
            and race_path.read_bytes() in (b"first\n", b"second\n")
            and race_path.stat().st_mode
            & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)
            == 0,
            "self-test",
            "immutable O_EXCL publication race did not have one sealed winner",
        )

        baseline_path, candidate_path, comparison = _fixture_pair(root, report_tool, epoch)
        missing_receipt_manifest = report_tool.load_manifest(baseline_path)
        missing_receipt_artifacts = missing_receipt_manifest.raw["provenance"][
            "artifacts_sha256"
        ]
        for artifact_name in (
            "stdout_log",
            "stderr_log",
            "exit_status",
            "fort_anchor_receipt",
            "in_game_anchor_receipt",
            "start_config",
            "checkpoint_observation",
            "process_time",
            "exit_uptime_raw",
            "save_special_before",
            "save_special_after",
            "save_template_before",
            "save_template_after",
        ):
            artifact_digest = missing_receipt_artifacts.pop(artifact_name)
            try:
                try:
                    _verify_record_artifacts(missing_receipt_manifest)
                except RegistryError:
                    pass
                else:
                    raise RegistryError(
                        f"self-test accepted missing core artifact receipt {artifact_name}"
                    )
            finally:
                missing_receipt_artifacts[artifact_name] = artifact_digest
        migration_root = missing_receipt_manifest.path.parent
        migration_payloads = {
            "checkpoint_decimal_migration_receipt": b'{"schema":"migration-fixture"}\n',
            "checkpoint_decimal_migration_freeze": b'{"schema":"freeze-fixture"}\n',
            "checkpoint_decimal_migration_seal_harness": b"# migration harness fixture\n",
        }
        for name, payload in migration_payloads.items():
            migration_path = migration_root / CHECKPOINT_DECIMAL_MIGRATION_ARTIFACTS[name]
            _atomic_bytes(migration_path, payload)
            missing_receipt_artifacts[name] = _sha_file(migration_path)
        _verify_record_artifacts(missing_receipt_manifest)
        missing_name = "checkpoint_decimal_migration_receipt"
        missing_digest = missing_receipt_artifacts.pop(missing_name)
        try:
            try:
                _verify_record_artifacts(missing_receipt_manifest)
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    "self-test accepted a partial checkpoint migration artifact set"
                )
        finally:
            missing_receipt_artifacts[missing_name] = missing_digest
        migration_harness_path = (
            migration_root
            / CHECKPOINT_DECIMAL_MIGRATION_ARTIFACTS[
                "checkpoint_decimal_migration_seal_harness"
            ]
        )
        migration_harness_path.chmod(0o644)
        migration_harness_path.write_bytes(b"tampered migration harness\n")
        try:
            _verify_record_artifacts(missing_receipt_manifest)
        except RegistryError:
            pass
        else:
            raise RegistryError("self-test accepted a mutated migration artifact")
        finally:
            migration_harness_path.write_bytes(
                migration_payloads["checkpoint_decimal_migration_seal_harness"]
            )
            migration_harness_path.chmod(0o444)
        for name in migration_payloads:
            del missing_receipt_artifacts[name]

        projection_root = missing_receipt_manifest.path.parent
        projection_payloads = {
            "configuration_projection_receipt": b'{"fixture":"projection"}\n',
            "configuration_cache_raw": b"configuration cache fixture\n",
        }
        projection_raw = json.loads(json.dumps(missing_receipt_manifest.raw))
        projection_selectors = {
            CONFIGURATION_PROJECTION_STABLE_SELECTOR: True,
            CONFIGURATION_PROJECTION_SELECTOR: True,
        }
        projection_raw["provenance"]["selectors"] = {
            "expected": dict(projection_selectors),
            "effective": dict(projection_selectors),
        }
        projection_artifacts = projection_raw["provenance"]["artifacts_sha256"]
        projection_manifest = report_tool.RunManifest(
            path=missing_receipt_manifest.path,
            content_sha256=missing_receipt_manifest.content_sha256,
            raw=projection_raw,
        )
        projection_contract = dict(CONFIGURATION_PROJECTION_CONTRACT)
        projection_contract.update(
            {
                "protocol_id": projection_manifest.protocol["protocol_id"],
                "record_id": projection_manifest.provenance["record_id"],
                "generation": projection_manifest.lineage["generation"],
                "parent_record_id": projection_manifest.lineage[
                    "parent_record_id"
                ],
                "original_role": projection_manifest.lineage["original_role"],
                "selector_bundle": dict(projection_selectors),
            }
        )

        exact_record_false_selector = json.loads(json.dumps(projection_raw))
        for selector_map in exact_record_false_selector["provenance"][
            "selectors"
        ].values():
            selector_map[CONFIGURATION_PROJECTION_SELECTOR] = False
        exact_record_absent_selector = json.loads(json.dumps(projection_raw))
        for selector_map in exact_record_absent_selector["provenance"][
            "selectors"
        ].values():
            del selector_map[CONFIGURATION_PROJECTION_SELECTOR]
        selector_signal_integer = json.loads(json.dumps(projection_raw))
        selector_signal_integer["provenance"]["record_id"] = "legacy-record"
        for selector_map in selector_signal_integer["provenance"][
            "selectors"
        ].values():
            selector_map[CONFIGURATION_PROJECTION_SELECTOR] = 1
        for name, raw in (
            ("exact-record-false-selector", exact_record_false_selector),
            ("exact-record-absent-selector", exact_record_absent_selector),
            ("integer-selector-without-artifacts", selector_signal_integer),
        ):
            signaled_manifest = report_tool.RunManifest(
                path=projection_manifest.path,
                content_sha256=projection_manifest.content_sha256,
                raw=raw,
            )
            try:
                _verify_record_artifacts(
                    signaled_manifest,
                    _projection_contract=projection_contract,
                )
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted the 6CP signal {name}"
                )

        for name, payload in projection_payloads.items():
            path = projection_root / CONFIGURATION_PROJECTION_ARTIFACTS[name]
            _atomic_bytes(path, payload)
            projection_artifacts[name] = _sha_file(path)
        _verify_record_artifacts(
            projection_manifest, _projection_contract=projection_contract
        )

        g3_projection_raw = json.loads(json.dumps(projection_raw))
        g3_projection_raw["protocol"]["protocol_id"] = (
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT["protocol_id"]
        )
        g3_projection_raw["provenance"]["record_id"] = (
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT["record_id"]
        )
        g3_projection_raw["lineage"] = {
            "generation": PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT[
                "generation"
            ],
            "parent_record_id": PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT[
                "parent_record_id"
            ],
            "original_role": PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT[
                "original_role"
            ],
        }
        g3_selector_bundle = dict(
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT["selector_bundle"]
        )
        g3_projection_raw["provenance"]["selectors"] = {
            "expected": dict(g3_selector_bundle),
            "effective": dict(g3_selector_bundle),
        }

        def g3_manifest(raw: Mapping[str, Any]) -> Any:
            return report_tool.RunManifest(
                path=projection_manifest.path,
                content_sha256=projection_manifest.content_sha256,
                raw=raw,
            )

        _verify_record_artifacts(g3_manifest(g3_projection_raw))

        g3_negative_cases = []
        for label, replacement in (("false", False), ("integer", 1)):
            value = json.loads(json.dumps(g3_projection_raw))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR] = (
                    replacement
                )
            g3_negative_cases.append((f"candidate-selector-{label}", value))
        missing_g3 = json.loads(json.dumps(g3_projection_raw))
        for selector_map in missing_g3["provenance"]["selectors"].values():
            del selector_map[PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR]
        g3_negative_cases.append(("missing-candidate-selector", missing_g3))
        for selector, label in (
            (CONFIGURATION_PROJECTION_STABLE_SELECTOR, "missing-retained-6CL"),
            (CONFIGURATION_PROJECTION_SELECTOR, "missing-retained-6CP"),
        ):
            value = json.loads(json.dumps(g3_projection_raw))
            for selector_map in value["provenance"]["selectors"].values():
                del selector_map[selector]
            g3_negative_cases.append((label, value))
        wrong_g3_parent = json.loads(json.dumps(g3_projection_raw))
        wrong_g3_parent["lineage"]["parent_record_id"] = "wrong-parent"
        g3_negative_cases.append(("wrong-parent", wrong_g3_parent))
        wrong_g3_generation = json.loads(json.dumps(g3_projection_raw))
        wrong_g3_generation["lineage"]["generation"] = 4
        g3_negative_cases.append(("wrong-generation", wrong_g3_generation))
        cross_contract = json.loads(json.dumps(g3_projection_raw))
        cross_contract["provenance"]["record_id"] = (
            CONFIGURATION_PROJECTION_CONTRACT["record_id"]
        )
        g3_negative_cases.append(("6CP-record-with-6CQ-selector", cross_contract))
        selector_only_g3 = json.loads(json.dumps(g3_projection_raw))
        selector_only_g3["provenance"]["record_id"] = "unknown-record"
        g3_negative_cases.append(("selector-only-wrong-record", selector_only_g3))
        for label, raw in g3_negative_cases:
            try:
                _verify_record_artifacts(g3_manifest(raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted invalid 6CQ registry scope {label}"
                )

        phase6cs_projection_raw = json.loads(json.dumps(projection_raw))
        phase6cs_projection_raw["protocol"]["protocol_id"] = (
            PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT["protocol_id"]
        )
        phase6cs_projection_raw["provenance"]["record_id"] = (
            PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT["record_id"]
        )
        phase6cs_projection_raw["lineage"] = {
            "generation": PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT[
                "generation"
            ],
            "parent_record_id": PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT[
                "parent_record_id"
            ],
            "original_role": PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT[
                "original_role"
            ],
        }
        phase6cs_selector_bundle = dict(
            PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT["selector_bundle"]
        )
        phase6cs_projection_raw["provenance"]["selectors"] = {
            "expected": dict(phase6cs_selector_bundle),
            "effective": dict(phase6cs_selector_bundle),
        }
        _verify_record_artifacts(g3_manifest(phase6cs_projection_raw))

        phase6cs_negative_cases = []
        for label, replacement in (("false", False), ("integer", 1)):
            value = json.loads(json.dumps(phase6cs_projection_raw))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR] = (
                    replacement
                )
            phase6cs_negative_cases.append(
                (f"candidate-selector-{label}", value)
            )
        missing_phase6cs = json.loads(json.dumps(phase6cs_projection_raw))
        for selector_map in missing_phase6cs["provenance"][
            "selectors"
        ].values():
            del selector_map[PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR]
        phase6cs_negative_cases.append(
            ("missing-candidate-selector", missing_phase6cs)
        )
        for scalar_state in (False, True):
            mixed = json.loads(json.dumps(phase6cs_projection_raw))
            for selector_map in mixed["provenance"]["selectors"].values():
                selector_map[
                    PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR
                ] = scalar_state
            phase6cs_negative_cases.append(
                (f"mixed-6CQ-selector-{scalar_state}", mixed)
            )
        wrong_phase6cs_parent = json.loads(json.dumps(phase6cs_projection_raw))
        wrong_phase6cs_parent["lineage"]["parent_record_id"] = "wrong-parent"
        phase6cs_negative_cases.append(
            ("wrong-parent", wrong_phase6cs_parent)
        )
        reused_6cq_record = json.loads(json.dumps(phase6cs_projection_raw))
        reused_6cq_record["provenance"]["record_id"] = (
            PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT["record_id"]
        )
        phase6cs_negative_cases.append(
            ("reused-6CQ-record", reused_6cq_record)
        )
        for label, raw in phase6cs_negative_cases:
            try:
                _verify_record_artifacts(g3_manifest(raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted invalid 6CS registry scope {label}"
                )

        phase6da_projection_raw = json.loads(json.dumps(projection_raw))
        phase6da_projection_raw["protocol"]["protocol_id"] = (
            PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT["protocol_id"]
        )
        phase6da_projection_raw["provenance"]["record_id"] = (
            PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT["record_id"]
        )
        phase6da_projection_raw["lineage"] = {
            "generation": PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT[
                "generation"
            ],
            "parent_record_id": PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT[
                "parent_record_id"
            ],
            "original_role": PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT[
                "original_role"
            ],
        }
        phase6da_selector_bundle = dict(
            PHASE6DA_CONFIGURATION_PROJECTION_CONTRACT["selector_bundle"]
        )
        phase6da_projection_raw["provenance"]["selectors"] = {
            "expected": dict(phase6da_selector_bundle),
            "effective": dict(phase6da_selector_bundle),
        }
        _verify_record_artifacts(g3_manifest(phase6da_projection_raw))

        phase6da_negative_cases = []
        for label, replacement in (("false", False), ("integer", 1)):
            value = json.loads(json.dumps(phase6da_projection_raw))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[
                    PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR
                ] = replacement
            phase6da_negative_cases.append(
                (f"candidate-selector-{label}", value)
            )
        missing_phase6da = json.loads(json.dumps(phase6da_projection_raw))
        for selector_map in missing_phase6da["provenance"][
            "selectors"
        ].values():
            del selector_map[PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR]
        phase6da_negative_cases.append(
            ("missing-candidate-selector", missing_phase6da)
        )
        for selector, label in (
            (CONFIGURATION_PROJECTION_STABLE_SELECTOR, "missing-retained-6CL"),
            (CONFIGURATION_PROJECTION_SELECTOR, "missing-retained-6CP"),
        ):
            value = json.loads(json.dumps(phase6da_projection_raw))
            for selector_map in value["provenance"]["selectors"].values():
                del selector_map[selector]
            phase6da_negative_cases.append((label, value))
        for retired_selector in (
            PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR,
            PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR,
        ):
            for retired_state in (False, True):
                value = json.loads(json.dumps(phase6da_projection_raw))
                for selector_map in value["provenance"][
                    "selectors"
                ].values():
                    selector_map[retired_selector] = retired_state
                phase6da_negative_cases.append(
                    (
                        f"mixed-{retired_selector}-{retired_state}",
                        value,
                    )
                )
        wrong_phase6da_parent = json.loads(json.dumps(phase6da_projection_raw))
        wrong_phase6da_parent["lineage"]["parent_record_id"] = "wrong-parent"
        phase6da_negative_cases.append(
            ("wrong-parent", wrong_phase6da_parent)
        )
        wrong_phase6da_generation = json.loads(
            json.dumps(phase6da_projection_raw)
        )
        wrong_phase6da_generation["lineage"]["generation"] = 4
        phase6da_negative_cases.append(
            ("wrong-generation-g000004", wrong_phase6da_generation)
        )
        for sibling_contract, label in (
            (PHASE6CQ_CONFIGURATION_PROJECTION_CONTRACT, "reused-6CQ-record"),
            (PHASE6CS_CONFIGURATION_PROJECTION_CONTRACT, "reused-6CS-record"),
        ):
            value = json.loads(json.dumps(phase6da_projection_raw))
            value["provenance"]["record_id"] = sibling_contract["record_id"]
            phase6da_negative_cases.append((label, value))
        for label, raw in phase6da_negative_cases:
            try:
                _verify_record_artifacts(g3_manifest(raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted invalid 6DA registry scope {label}"
                )

        projection_scope_negatives = []
        wrong_protocol_raw = json.loads(json.dumps(projection_raw))
        wrong_protocol_raw["protocol"]["protocol_id"] = "wrong-protocol"
        projection_scope_negatives.append(("protocol", wrong_protocol_raw))
        wrong_record_raw = json.loads(json.dumps(projection_raw))
        wrong_record_raw["provenance"]["record_id"] = "wrong-record"
        projection_scope_negatives.append(("record", wrong_record_raw))
        wrong_generation_raw = json.loads(json.dumps(projection_raw))
        wrong_generation_raw["lineage"]["generation"] += 1
        projection_scope_negatives.append(("generation", wrong_generation_raw))
        wrong_parent_raw = json.loads(json.dumps(projection_raw))
        wrong_parent_raw["lineage"]["parent_record_id"] = "wrong-parent"
        projection_scope_negatives.append(("parent", wrong_parent_raw))
        wrong_role_raw = json.loads(json.dumps(projection_raw))
        wrong_role_raw["lineage"]["original_role"] = "candidate"
        projection_scope_negatives.append(("role", wrong_role_raw))
        missing_stable_raw = json.loads(json.dumps(projection_raw))
        for selector_map in missing_stable_raw["provenance"]["selectors"].values():
            del selector_map[CONFIGURATION_PROJECTION_STABLE_SELECTOR]
        projection_scope_negatives.append(("selector-bundle", missing_stable_raw))
        integer_candidate_raw = json.loads(json.dumps(projection_raw))
        for selector_map in integer_candidate_raw["provenance"]["selectors"].values():
            selector_map[CONFIGURATION_PROJECTION_SELECTOR] = 1
        projection_scope_negatives.append(("selector-type", integer_candidate_raw))
        for name, raw in projection_scope_negatives:
            scoped_manifest = report_tool.RunManifest(
                path=projection_manifest.path,
                content_sha256=projection_manifest.content_sha256,
                raw=raw,
            )
            try:
                _verify_record_artifacts(
                    scoped_manifest,
                    _projection_contract=projection_contract,
                )
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted configuration projection outside {name} scope"
                )

        missing_projection_name = "configuration_cache_raw"
        missing_projection_digest = projection_artifacts.pop(
            missing_projection_name
        )
        try:
            try:
                _verify_record_artifacts(
                    projection_manifest,
                    _projection_contract=projection_contract,
                )
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    "self-test accepted a partial configuration projection artifact set"
                )
        finally:
            projection_artifacts[
                missing_projection_name
            ] = missing_projection_digest

        projection_raw_path = (
            projection_root
            / CONFIGURATION_PROJECTION_ARTIFACTS["configuration_cache_raw"]
        )
        projection_raw_path.chmod(0o644)
        projection_raw_path.write_bytes(b"tampered configuration cache\n")
        projection_raw_path.chmod(0o444)
        try:
            _verify_record_artifacts(
                projection_manifest, _projection_contract=projection_contract
            )
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted a tampered configuration projection artifact"
            )
        projection_raw_path.chmod(0o644)
        projection_raw_path.write_bytes(
            projection_payloads["configuration_cache_raw"]
        )
        projection_raw_path.chmod(0o444)

        projection_receipt_path = (
            projection_root
            / CONFIGURATION_PROJECTION_ARTIFACTS[
                "configuration_projection_receipt"
            ]
        )
        projection_receipt_path.chmod(0o644)
        try:
            _verify_record_artifacts(
                projection_manifest, _projection_contract=projection_contract
            )
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted a writable configuration projection artifact"
            )
        projection_receipt_path.chmod(0o400)
        try:
            _verify_record_artifacts(
                projection_manifest, _projection_contract=projection_contract
            )
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted a projection artifact whose mode was not 0444"
            )
        projection_receipt_path.chmod(0o444)

        spurious_projection_raw = json.loads(json.dumps(projection_raw))
        spurious_projection_raw["provenance"]["selectors"] = {
            "expected": {"fixture_candidate_enabled": False},
            "effective": {"fixture_candidate_enabled": False},
        }
        spurious_projection_manifest = report_tool.RunManifest(
            path=missing_receipt_manifest.path,
            content_sha256=missing_receipt_manifest.content_sha256,
            raw=spurious_projection_raw,
        )
        try:
            _verify_record_artifacts(spurious_projection_manifest)
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted spurious projection receipts on a legacy record"
            )
        try:
            _verify_record_artifacts(missing_receipt_manifest)
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted spurious unreceipted projection files on a legacy record"
            )
        for filename in CONFIGURATION_PROJECTION_ARTIFACTS.values():
            (projection_root / filename).unlink()

        alc_identity_root = parent / "alc-sealed-identity"
        (alc_identity_root / "records").mkdir(parents=True)
        alc_base_raw = json.loads(json.dumps(missing_receipt_manifest.raw))
        alc_base_raw["provenance"]["record_id"] = "alc-artifact-base"
        alc_base_path = _write_manifest(
            alc_identity_root, report_tool, alc_base_raw
        )
        alc_raw = json.loads(
            json.dumps(report_tool.load_manifest(alc_base_path).raw)
        )
        alc_contract = ALC_CONFIGURATION_PROJECTION_CONTRACT
        alc_bundle = dict(alc_contract["selector_bundle"])
        alc_raw["protocol"]["protocol_id"] = alc_contract["protocol_id"]
        alc_raw["comparability"]["os"] = {
            "name": "macOS",
            "version": PHASE6DA_DIRECT_OS_VERSION,
            "build": PHASE6DA_DIRECT_OS_BUILD,
            "policy": PHASE6DA_DIRECT_OS_POLICY,
        }
        alc_raw["comparability"]["runtime"]["configuration_sha256"] = (
            alc_contract["projected_sha256"]
        )
        alc_raw["provenance"]["record_id"] = alc_contract["record_id"]
        alc_raw["provenance"]["selectors"] = {
            "expected": dict(alc_bundle),
            "effective": dict(alc_bundle),
        }
        alc_run_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}"
            for name, value in alc_contract[
                "required_receipt_fields"
            ].items()
        )
        alc_raw["provenance"]["run_receipt"] = alc_run_receipt
        alc_raw["lineage"] = {
            "generation": alc_contract["generation"],
            "parent_record_id": alc_contract["parent_record_id"],
            "original_role": alc_contract["original_role"],
        }
        alc_raw["lifecycle"]["candidate_marker"] = dict(
            alc_contract["candidate_marker"]
        )
        alc_record_root = alc_base_path.parent
        alc_selector_receipt_fields = {
            CONFIGURATION_PROJECTION_STABLE_SELECTOR: "stable_negative_route",
            CONFIGURATION_PROJECTION_SELECTOR: (
                "guest_memory_operation_snapshot_elision"
            ),
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR: (
                "fast_block_dense_directory"
            ),
            PHASEIFSM_CANDIDATE_SELECTOR: (
                "fast_block_inline_fixed_signature_match"
            ),
            ALC_CANDIDATE_SELECTOR: "arm_loading_composite",
        }
        alc_start = {
            "schema": {
                "name": "lumen.persistent-performance-start",
                "version": 1,
            },
            "protocol": alc_raw["protocol"],
            "comparability": alc_raw["comparability"],
            "provenance": {
                "record_id": alc_contract["record_id"],
                "run_receipt": alc_run_receipt,
                "candidate_selector": ALC_CANDIDATE_SELECTOR,
                "selectors": dict(alc_bundle),
                "selector_receipts": {
                    name: {
                        "prefix": A64_CONFIGURATION_PREFIX,
                        "field": alc_selector_receipt_fields[name],
                    }
                    for name in alc_bundle
                },
            },
            "lineage": alc_raw["lineage"],
            "candidate_marker": {
                "required": True,
                "prefix": ALC_MARKER_PREFIX,
                "required_tokens": list(ALC_MARKER_TOKENS),
                "coverage_required": True,
                "coverage_token": "coverage=positive",
            },
            "launch": {
                "arguments": [],
                "common_environment": {},
                "experiment_environment": {
                    name: "1" for name in alc_bundle
                },
            },
        }
        alc_start_path = alc_record_root / "start-config.json"
        _atomic_json(alc_start_path, alc_start, replace=True)
        alc_stderr_path = alc_record_root / "stderr.log"
        alc_stderr_payload = (
            "stderr ALC sealed identity\n" + ALC_MARKER_LINE + "\n"
        ).encode("utf-8")
        _atomic_bytes(alc_stderr_path, alc_stderr_payload, replace=True)
        alc_artifacts = alc_raw["provenance"]["artifacts_sha256"]
        alc_artifacts["start_config"] = _sha_file(alc_start_path)
        alc_artifacts["stderr_log"] = _sha_file(alc_stderr_path)
        for name, payload in (
            ("configuration_projection_receipt", b'{"alc":"receipt"}\n'),
            ("configuration_cache_raw", b"ALC raw cache fixture\n"),
        ):
            path = alc_record_root / CONFIGURATION_PROJECTION_ARTIFACTS[name]
            _atomic_bytes(path, payload)
            alc_artifacts[name] = _sha_file(path)

        def alc_manifest(raw: Mapping[str, Any]) -> Any:
            return report_tool.RunManifest(
                path=alc_base_path,
                content_sha256=_sha_file(alc_base_path),
                raw=raw,
            )

        _verify_record_artifacts(alc_manifest(alc_raw))

        alc_scope_negatives = []
        for label, replacement in (("false", False), ("integer", 1)):
            value = json.loads(json.dumps(alc_raw))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[ALC_CANDIDATE_SELECTOR] = replacement
            alc_scope_negatives.append((f"selector-{label}", value))
        missing_alc_selector = json.loads(json.dumps(alc_raw))
        for selector_map in missing_alc_selector["provenance"][
            "selectors"
        ].values():
            del selector_map[ALC_CANDIDATE_SELECTOR]
        alc_scope_negatives.append(
            ("selector-missing", missing_alc_selector)
        )
        wrong_alc_parent = json.loads(json.dumps(alc_raw))
        wrong_alc_parent["lineage"]["parent_record_id"] = (
            PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
        )
        alc_scope_negatives.append(("wrong-parent", wrong_alc_parent))
        alc_sibling = json.loads(json.dumps(alc_raw))
        for selector_map in alc_sibling["provenance"]["selectors"].values():
            selector_map[FCFOS_CANDIDATE_SELECTOR] = False
        alc_scope_negatives.append(("sibling-present", alc_sibling))
        wrong_alc_receipt = json.loads(json.dumps(alc_raw))
        wrong_alc_receipt["provenance"]["run_receipt"] = (
            wrong_alc_receipt["provenance"]["run_receipt"].replace(
                "arm_loading_composite=1", "arm_loading_composite=0"
            )
        )
        alc_scope_negatives.append(("effective-receipt-zero", wrong_alc_receipt))
        for label, raw in alc_scope_negatives:
            try:
                _verify_record_artifacts(alc_manifest(raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted invalid ALC registry identity {label}"
                )

        bad_alc_start = json.loads(json.dumps(alc_start))
        bad_alc_start["provenance"]["selector_receipts"][
            ALC_CANDIDATE_SELECTOR
        ]["field"] = "fast_block_inline_fixed_signature_match"
        _atomic_json(alc_start_path, bad_alc_start, replace=True)
        bad_alc_start_raw = json.loads(json.dumps(alc_raw))
        bad_alc_start_raw["provenance"]["artifacts_sha256"][
            "start_config"
        ] = _sha_file(alc_start_path)
        try:
            _verify_record_artifacts(alc_manifest(bad_alc_start_raw))
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted remapped ALC selector receipt"
            )
        _atomic_json(alc_start_path, alc_start, replace=True)

        duplicate_alc_marker = (
            "stderr ALC duplicate marker\n"
            + ALC_MARKER_LINE
            + "\n"
            + ALC_MARKER_LINE
            + "\n"
        ).encode("utf-8")
        _atomic_bytes(
            alc_stderr_path, duplicate_alc_marker, replace=True
        )
        duplicate_alc_raw = json.loads(json.dumps(alc_raw))
        duplicate_alc_raw["provenance"]["artifacts_sha256"][
            "stderr_log"
        ] = _sha_file(alc_stderr_path)
        duplicate_alc_raw["provenance"]["artifacts_sha256"][
            "start_config"
        ] = _sha_file(alc_start_path)
        try:
            _verify_record_artifacts(alc_manifest(duplicate_alc_raw))
        except RegistryError:
            pass
        else:
            raise RegistryError("self-test accepted duplicate ALC marker")
        _atomic_bytes(alc_stderr_path, alc_stderr_payload, replace=True)

        metal_identity_root = parent / "metal-runtime-sealed-identity"
        (metal_identity_root / "records").mkdir(parents=True)
        metal_base_raw = json.loads(json.dumps(missing_receipt_manifest.raw))
        metal_base_raw["provenance"]["record_id"] = "metal-artifact-base"
        metal_base_path = _write_manifest(
            metal_identity_root, report_tool, metal_base_raw
        )
        metal_raw = json.loads(
            json.dumps(report_tool.load_manifest(metal_base_path).raw)
        )
        metal_contract = (
            METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT
        )
        metal_bundle = dict(metal_contract["selector_bundle"])
        metal_raw["protocol"]["protocol_id"] = metal_contract["protocol_id"]
        metal_raw["comparability"]["os"] = {
            "name": "macOS",
            "version": PHASE6DA_DIRECT_OS_VERSION,
            "build": PHASE6DA_DIRECT_OS_BUILD,
            "policy": PHASE6DA_DIRECT_OS_POLICY,
        }
        metal_raw["comparability"]["runtime"]["configuration_sha256"] = (
            metal_contract["projected_sha256"]
        )
        metal_raw["provenance"]["record_id"] = metal_contract["record_id"]
        metal_raw["provenance"]["selectors"] = {
            "expected": dict(metal_bundle),
            "effective": dict(metal_bundle),
        }
        metal_run_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}"
            for name, value in metal_contract[
                "required_receipt_fields"
            ].items()
        )
        metal_raw["provenance"]["run_receipt"] = metal_run_receipt
        metal_raw["lineage"] = {
            "generation": 4,
            "parent_record_id": ALC_CANDIDATE_RECORD_ID,
            "original_role": "candidate",
        }
        metal_raw["lifecycle"]["candidate_marker"] = dict(
            metal_contract["candidate_marker"]
        )
        metal_record_root = metal_base_path.parent
        metal_selector_receipt_fields = {
            **alc_selector_receipt_fields,
            MSFC_COMPANION_SELECTOR: "metal_shader_function_cache",
            MDSSC_COMPANION_SELECTOR: "metal_depth_stencil_state_cache",
            MSORE_CANDIDATE_SELECTOR: "metal_state_only_record_elision",
        }
        metal_start = {
            "schema": {
                "name": "lumen.persistent-performance-start",
                "version": 1,
            },
            "protocol": metal_raw["protocol"],
            "comparability": metal_raw["comparability"],
            "provenance": {
                "record_id": metal_contract["record_id"],
                "run_receipt": metal_run_receipt,
                "candidate_selector": MSORE_CANDIDATE_SELECTOR,
                "selectors": dict(metal_bundle),
                "selector_receipts": {
                    name: {
                        "prefix": A64_CONFIGURATION_PREFIX,
                        "field": metal_selector_receipt_fields[name],
                    }
                    for name in metal_bundle
                },
            },
            "lineage": metal_raw["lineage"],
            "candidate_marker": {
                "required": True,
                "prefix": MSORE_MARKER_PREFIX,
                "required_tokens": list(MSORE_MARKER_TOKENS),
                "coverage_required": True,
                "coverage_token": "coverage=positive",
            },
            "launch": {
                "arguments": [],
                "common_environment": {},
                "experiment_environment": {
                    name: "1" for name in metal_bundle
                },
            },
        }
        metal_start_path = metal_record_root / "start-config.json"
        _atomic_json(metal_start_path, metal_start, replace=True)
        metal_stderr_path = metal_record_root / "stderr.log"
        metal_stderr_payload = (
            "stderr Metal sealed identity\n"
            + MSORE_MARKER_LINE
            + "\n"
            + MSFC_MARKER_LINE
            + "\n"
            + MDSSC_MARKER_LINE
            + "\n"
        ).encode("utf-8")
        _atomic_bytes(metal_stderr_path, metal_stderr_payload, replace=True)
        metal_artifacts = metal_raw["provenance"]["artifacts_sha256"]
        metal_artifacts["start_config"] = _sha_file(metal_start_path)
        metal_artifacts["stderr_log"] = _sha_file(metal_stderr_path)
        for name, payload in (
            ("configuration_projection_receipt", b'{"metal":"receipt"}\n'),
            ("configuration_cache_raw", b"Metal raw cache fixture\n"),
        ):
            path = metal_record_root / CONFIGURATION_PROJECTION_ARTIFACTS[name]
            _atomic_bytes(path, payload)
            metal_artifacts[name] = _sha_file(path)

        def metal_manifest(raw: Mapping[str, Any]) -> Any:
            return report_tool.RunManifest(
                path=metal_base_path,
                content_sha256=_sha_file(metal_base_path),
                raw=raw,
            )

        _verify_record_artifacts(metal_manifest(metal_raw))
        _require(
            metal_contract["record_id"]
            == "msore-msfc-mdssc-c-metal-runtime-bundle-g000004"
            and metal_contract["generation"] == 4
            and metal_contract["parent_record_id"] == ALC_CANDIDATE_RECORD_ID
            and metal_contract["candidate_selector"]
            == MSORE_CANDIDATE_SELECTOR
            and metal_contract["attestation"]
            == "CONFIRM_MSORE_MSFC_MDSSC_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
            and metal_contract["policy"]
            == "msore-msfc-mdssc-cmake-cache-production-v1"
            and metal_contract["mode"]
            == "strip-exact-active-alc-build-options-v1",
            "self-test",
            "MSORE+MSFC+MDSSC projected g4 registry contract drifted",
        )

        metal_scope_negatives = []
        for selector in METAL_RUNTIME_BUNDLE_SELECTORS:
            for label, replacement in (("false", False), ("integer", 1)):
                value = json.loads(json.dumps(metal_raw))
                for selector_map in value["provenance"]["selectors"].values():
                    selector_map[selector] = replacement
                metal_scope_negatives.append(
                    (f"{selector}-{label}", value)
                )
            missing = json.loads(json.dumps(metal_raw))
            for selector_map in missing["provenance"]["selectors"].values():
                del selector_map[selector]
            metal_scope_negatives.append((f"{selector}-missing", missing))
        wrong_metal_parent = json.loads(json.dumps(metal_raw))
        wrong_metal_parent["lineage"]["parent_record_id"] = (
            PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
        )
        metal_scope_negatives.append(("wrong-parent", wrong_metal_parent))
        wrong_metal_generation = json.loads(json.dumps(metal_raw))
        wrong_metal_generation["lineage"]["generation"] = 3
        metal_scope_negatives.append(
            ("wrong-generation", wrong_metal_generation)
        )
        mixed_metal_sibling = json.loads(json.dumps(metal_raw))
        for selector_map in mixed_metal_sibling["provenance"][
            "selectors"
        ].values():
            selector_map[UFSB_CANDIDATE_SELECTOR] = False
        metal_scope_negatives.append(("sibling-present", mixed_metal_sibling))
        for field in (
            "metal_shader_function_cache_requested",
            "metal_shader_function_cache",
            "metal_depth_stencil_state_cache_requested",
            "metal_depth_stencil_state_cache",
            "metal_state_only_record_elision_requested",
            "metal_state_only_record_elision",
        ):
            value = json.loads(json.dumps(metal_raw))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}=1", f"{field}=0")
            metal_scope_negatives.append((f"receipt-{field}", value))
        for label, raw in metal_scope_negatives:
            try:
                _verify_record_artifacts(metal_manifest(raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted invalid Metal registry identity {label}"
                )

        bad_metal_start = json.loads(json.dumps(metal_start))
        bad_metal_start["provenance"]["selector_receipts"][
            MSORE_CANDIDATE_SELECTOR
        ]["field"] = "metal_state_only_record_elision_requested"
        _atomic_json(metal_start_path, bad_metal_start, replace=True)
        bad_metal_start_raw = json.loads(json.dumps(metal_raw))
        bad_metal_start_raw["provenance"]["artifacts_sha256"][
            "start_config"
        ] = _sha_file(metal_start_path)
        try:
            _verify_record_artifacts(metal_manifest(bad_metal_start_raw))
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted remapped MSORE selector receipt"
            )
        _atomic_json(metal_start_path, metal_start, replace=True)

        for label, bad_stderr in (
            (
                "missing-msfc",
                metal_stderr_payload.replace(
                    (MSFC_MARKER_LINE + "\n").encode("utf-8"), b"", 1
                ),
            ),
            (
                "duplicate-mdssc",
                metal_stderr_payload
                + (MDSSC_MARKER_LINE + "\n").encode("utf-8"),
            ),
            (
                "reordered-msore",
                metal_stderr_payload.replace(
                    MSORE_MARKER_LINE.encode("utf-8"),
                    MSORE_MARKER_LINE.replace(
                        "requested=positive effective=positive",
                        "effective=positive requested=positive",
                    ).encode("utf-8"),
                    1,
                ),
            ),
        ):
            _atomic_bytes(metal_stderr_path, bad_stderr, replace=True)
            bad_marker_raw = json.loads(json.dumps(metal_raw))
            bad_marker_raw["provenance"]["artifacts_sha256"][
                "stderr_log"
            ] = _sha_file(metal_stderr_path)
            bad_marker_raw["provenance"]["artifacts_sha256"][
                "start_config"
            ] = _sha_file(metal_start_path)
            try:
                _verify_record_artifacts(metal_manifest(bad_marker_raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted invalid Metal marker {label}"
                )
        _atomic_bytes(metal_stderr_path, metal_stderr_payload, replace=True)

        # NODA g5 is the one named expected/effective exception: its launch
        # retains 6CL=1 while the runtime receipt proves 6CL superseded and
        # effective=0.  PBA is absent, rather than mixed into the selector set.
        noda_contract = NODA_CONFIGURATION_PROJECTION_CONTRACT
        noda_bundle = dict(noda_contract["selector_bundle"])
        noda_effective_bundle = dict(
            noda_contract["effective_selector_bundle"]
        )
        noda_raw = json.loads(json.dumps(metal_raw))
        noda_raw["provenance"]["record_id"] = noda_contract["record_id"]
        noda_raw["provenance"]["selectors"] = {
            "expected": dict(noda_bundle),
            "effective": dict(noda_effective_bundle),
        }
        noda_run_receipt = A64_CONFIGURATION_PREFIX + " " + " ".join(
            f"{name}={value}"
            for name, value in noda_contract[
                "required_receipt_fields"
            ].items()
        )
        noda_raw["provenance"]["run_receipt"] = noda_run_receipt
        noda_raw["lineage"] = {
            "generation": noda_contract["generation"],
            "parent_record_id": noda_contract["parent_record_id"],
            "original_role": noda_contract["original_role"],
        }
        noda_raw["lifecycle"]["candidate_marker"] = dict(
            noda_contract["candidate_marker"]
        )
        noda_selector_receipt_fields = {
            **metal_selector_receipt_fields,
            CONFIGURATION_PROJECTION_STABLE_SELECTOR: (
                "stable_negative_route_requested"
            ),
            NODA_CANDIDATE_SELECTOR: "native_only_admission",
        }
        noda_start = {
            "schema": {
                "name": "lumen.persistent-performance-start",
                "version": 1,
            },
            "protocol": noda_raw["protocol"],
            "comparability": noda_raw["comparability"],
            "provenance": {
                "record_id": noda_contract["record_id"],
                "run_receipt": noda_run_receipt,
                "candidate_selector": NODA_CANDIDATE_SELECTOR,
                "selectors": dict(noda_bundle),
                "selector_receipts": {
                    name: {
                        "prefix": A64_CONFIGURATION_PREFIX,
                        "field": noda_selector_receipt_fields[name],
                    }
                    for name in noda_bundle
                },
            },
            "lineage": noda_raw["lineage"],
            "candidate_marker": {
                "required": True,
                "prefix": NODA_MARKER_PREFIX,
                "required_tokens": list(NODA_MARKER_TOKENS),
                "coverage_required": True,
                "coverage_token": "coverage=positive",
            },
            "launch": {
                "arguments": [],
                "common_environment": {},
                "experiment_environment": {
                    name: "1" for name in noda_bundle
                },
            },
        }
        noda_start_path = metal_start_path
        _atomic_json(noda_start_path, noda_start, replace=True)
        noda_stderr_path = metal_stderr_path
        noda_stderr_payload = (
            "stderr NODA sealed identity\n"
            + "\n".join(NODA_REQUIRED_MARKER_LINES.values())
            + "\n"
        ).encode("utf-8")
        _atomic_bytes(noda_stderr_path, noda_stderr_payload, replace=True)
        noda_artifacts = noda_raw["provenance"]["artifacts_sha256"]
        noda_artifacts["start_config"] = _sha_file(noda_start_path)
        noda_artifacts["stderr_log"] = _sha_file(noda_stderr_path)

        def noda_manifest(raw: Mapping[str, Any]) -> Any:
            return report_tool.RunManifest(
                path=metal_base_path,
                content_sha256=_sha_file(metal_base_path),
                raw=raw,
            )

        _verify_record_artifacts(noda_manifest(noda_raw))
        _require(
            noda_contract["record_id"]
            == "noda-c-a64-dynarec-native-only-admission-g000005"
            and noda_contract["generation"] == 5
            and noda_contract["parent_record_id"]
            == METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
            and noda_contract["candidate_selector"]
            == NODA_CANDIDATE_SELECTOR
            and PBA_CANDIDATE_SELECTOR not in noda_bundle
            and PBA_CANDIDATE_SELECTOR not in noda_effective_bundle
            and noda_bundle[CONFIGURATION_PROJECTION_STABLE_SELECTOR]
            is True
            and noda_effective_bundle[
                CONFIGURATION_PROJECTION_STABLE_SELECTOR
            ]
            is False
            and noda_contract["attestation"]
            == "CONFIRM_NODA_CANONICAL_PRODUCTION_CONFIGURATION_PROJECTION"
            and noda_contract["policy"]
            == "noda-cmake-cache-production-v1"
            and noda_contract["mode"]
            == "strip-exact-active-alc-build-options-v1"
            and noda_contract["projected_sha256"]
            == METAL_RUNTIME_BUNDLE_CONFIGURATION_PROJECTION_CONTRACT[
                "projected_sha256"
            ]
            and NODA_MARKER_LINE
            == (
                "[A64_NATIVE_ONLY_ADMISSION_FINAL] effective=positive "
                "ordinary_routed=positive native_probe=bypassed "
                "native_only_executed=positive "
                "class=fastblock_primary_native_only_dynarec "
                "status=clean coverage=positive"
            )
            and NODA_SUPERSEDED_6CL_MARKER_LINE
            == (
                "[A64_STABLE_NEGATIVE_ROUTE_FINAL] "
                "classified=inapplicable live_mismatch=none status=clean "
                "coverage=superseded class=native_only_admission"
            ),
            "self-test",
            "NODA projected g5 registry contract drifted",
        )

        noda_scope_negatives = []
        for selector_map_name in ("expected", "effective"):
            for label, replacement in (("false", False), ("integer", 1)):
                value = json.loads(json.dumps(noda_raw))
                value["provenance"]["selectors"][selector_map_name][
                    NODA_CANDIDATE_SELECTOR
                ] = replacement
                noda_scope_negatives.append(
                    (f"NODA-{selector_map_name}-{label}", value)
                )
            missing = json.loads(json.dumps(noda_raw))
            del missing["provenance"]["selectors"][selector_map_name][
                NODA_CANDIDATE_SELECTOR
            ]
            noda_scope_negatives.append(
                (f"NODA-{selector_map_name}-missing", missing)
            )
        wrong_noda_parent = json.loads(json.dumps(noda_raw))
        wrong_noda_parent["lineage"]["parent_record_id"] = (
            ALC_CANDIDATE_RECORD_ID
        )
        noda_scope_negatives.append(("wrong-parent", wrong_noda_parent))
        wrong_noda_generation = json.loads(json.dumps(noda_raw))
        wrong_noda_generation["lineage"]["generation"] = 4
        noda_scope_negatives.append(
            ("wrong-generation", wrong_noda_generation)
        )
        wrong_noda_role = json.loads(json.dumps(noda_raw))
        wrong_noda_role["lineage"]["original_role"] = "production"
        noda_scope_negatives.append(("wrong-role", wrong_noda_role))
        wrong_expected_6cl = json.loads(json.dumps(noda_raw))
        wrong_expected_6cl["provenance"]["selectors"]["expected"][
            CONFIGURATION_PROJECTION_STABLE_SELECTOR
        ] = False
        noda_scope_negatives.append(
            ("6CL-not-requested", wrong_expected_6cl)
        )
        wrong_effective_6cl = json.loads(json.dumps(noda_raw))
        wrong_effective_6cl["provenance"]["selectors"]["effective"][
            CONFIGURATION_PROJECTION_STABLE_SELECTOR
        ] = True
        noda_scope_negatives.append(
            ("6CL-not-superseded", wrong_effective_6cl)
        )
        for pba_value in (False, True):
            mixed_pba = json.loads(json.dumps(noda_raw))
            for selector_map in mixed_pba["provenance"]["selectors"].values():
                selector_map[PBA_CANDIDATE_SELECTOR] = pba_value
            noda_scope_negatives.append(
                (f"PBA-mixed-{pba_value}", mixed_pba)
            )
        mixed_sibling = json.loads(json.dumps(noda_raw))
        for selector_map in mixed_sibling["provenance"]["selectors"].values():
            selector_map[UFSB_CANDIDATE_SELECTOR] = False
        noda_scope_negatives.append(("sibling-mixed", mixed_sibling))
        missing_retained = json.loads(json.dumps(noda_raw))
        for selector_map in missing_retained["provenance"][
            "selectors"
        ].values():
            del selector_map[MSORE_CANDIDATE_SELECTOR]
        noda_scope_negatives.append(("retained-missing", missing_retained))
        wrong_noda_record = json.loads(json.dumps(noda_raw))
        wrong_noda_record["provenance"]["record_id"] = (
            METAL_RUNTIME_BUNDLE_CANDIDATE_RECORD_ID
        )
        noda_scope_negatives.append(("record-mix", wrong_noda_record))
        for field, invalid in (
            ("stable_negative_route_requested", "0"),
            ("stable_negative_route", "1"),
            ("stable_negative_route_superseded", "0"),
            ("profitable_block_admission_requested", "1"),
            ("profitable_block_admission", "1"),
            ("native_only_admission_requested", "0"),
            ("native_only_admission", "0"),
            ("guest_memory_operation_snapshot_elision", "0"),
            ("metal_state_only_record_elision", "0"),
        ):
            value = json.loads(json.dumps(noda_raw))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(
                f"{field}={noda_contract['required_receipt_fields'][field]}",
                f"{field}={invalid}",
            )
            noda_scope_negatives.append((f"receipt-{field}", value))
        wrong_noda_lifecycle = json.loads(json.dumps(noda_raw))
        wrong_noda_lifecycle["lifecycle"]["candidate_marker"][
            "line"
        ] = NODA_MARKER_LINE.replace(
            "native_probe=bypassed", "native_probe=positive"
        )
        noda_scope_negatives.append(
            ("candidate-marker-lifecycle", wrong_noda_lifecycle)
        )
        for label, raw in noda_scope_negatives:
            try:
                _verify_record_artifacts(noda_manifest(raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted invalid NODA registry identity {label}"
                )

        noda_start_negatives = []
        remapped_6cl_start = json.loads(json.dumps(noda_start))
        remapped_6cl_start["provenance"]["selector_receipts"][
            CONFIGURATION_PROJECTION_STABLE_SELECTOR
        ]["field"] = "stable_negative_route"
        noda_start_negatives.append(("6CL-receipt-remap", remapped_6cl_start))
        wrong_candidate_start = json.loads(json.dumps(noda_start))
        wrong_candidate_start["provenance"]["candidate_selector"] = (
            MSORE_CANDIDATE_SELECTOR
        )
        noda_start_negatives.append(("candidate-mix", wrong_candidate_start))
        pba_launch_start = json.loads(json.dumps(noda_start))
        pba_launch_start["launch"]["experiment_environment"][
            PBA_CANDIDATE_SELECTOR
        ] = "1"
        noda_start_negatives.append(("PBA-launch-mix", pba_launch_start))
        wrong_parent_start = json.loads(json.dumps(noda_start))
        wrong_parent_start["lineage"]["parent_record_id"] = (
            ALC_CANDIDATE_RECORD_ID
        )
        noda_start_negatives.append(("start-lineage", wrong_parent_start))
        wrong_marker_start = json.loads(json.dumps(noda_start))
        wrong_marker_start["candidate_marker"]["prefix"] = MSORE_MARKER_PREFIX
        noda_start_negatives.append(("start-marker-mix", wrong_marker_start))
        for label, bad_start in noda_start_negatives:
            _atomic_json(noda_start_path, bad_start, replace=True)
            bad_start_raw = json.loads(json.dumps(noda_raw))
            bad_start_raw["provenance"]["artifacts_sha256"][
                "start_config"
            ] = _sha_file(noda_start_path)
            try:
                _verify_record_artifacts(noda_manifest(bad_start_raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted tampered NODA start-config {label}"
                )
        _atomic_json(noda_start_path, noda_start, replace=True)

        noda_marker_negatives = (
            (
                "missing-6CL",
                noda_stderr_payload.replace(
                    (NODA_SUPERSEDED_6CL_MARKER_LINE + "\n").encode("utf-8"),
                    b"",
                    1,
                ),
            ),
            (
                "duplicate-NODA",
                noda_stderr_payload + (NODA_MARKER_LINE + "\n").encode("utf-8"),
            ),
            (
                "mutated-NODA",
                noda_stderr_payload.replace(
                    b"native_probe=bypassed", b"native_probe=positive", 1
                ),
            ),
            (
                "mutated-retained-6CP",
                noda_stderr_payload.replace(
                    b"signature_snapshot=preserved",
                    b"signature_snapshot=missing",
                    1,
                ),
            ),
            (
                "forbidden-PBA-final",
                noda_stderr_payload
                + (
                    PBA_MARKER_PREFIX
                    + " effective=positive status=clean coverage=positive\n"
                ).encode("utf-8"),
            ),
        )
        for label, bad_stderr in noda_marker_negatives:
            _atomic_bytes(noda_stderr_path, bad_stderr, replace=True)
            bad_marker_raw = json.loads(json.dumps(noda_raw))
            bad_marker_raw["provenance"]["artifacts_sha256"][
                "stderr_log"
            ] = _sha_file(noda_stderr_path)
            bad_marker_raw["provenance"]["artifacts_sha256"][
                "start_config"
            ] = _sha_file(noda_start_path)
            try:
                _verify_record_artifacts(noda_manifest(bad_marker_raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted tampered NODA finalizer {label}"
                )
        _atomic_bytes(noda_stderr_path, noda_stderr_payload, replace=True)

        def phase6da_direct_raw(contract: Mapping[str, Any]) -> Mapping[str, Any]:
            raw = json.loads(json.dumps(missing_receipt_manifest.raw))
            raw["protocol"]["protocol_id"] = PHASE6DA_DIRECT_PROTOCOL_ID
            raw["comparability"]["os"] = dict(contract["os"])
            raw["comparability"]["runtime"]["configuration_sha256"] = (
                contract["configuration_sha256"]
            )
            raw["provenance"]["record_id"] = contract["record_id"]
            raw["provenance"]["selectors"] = {
                "expected": dict(contract["selector_bundle"]),
                "effective": dict(contract["selector_bundle"]),
            }
            raw["provenance"]["run_receipt"] = (
                A64_CONFIGURATION_PREFIX
                + " "
                + " ".join(
                    f"{name}={value}"
                    for name, value in contract[
                        "required_receipt_fields"
                    ].items()
                )
            )
            raw["lineage"] = {
                "generation": contract["generation"],
                "parent_record_id": contract["parent_record_id"],
                "original_role": contract["original_role"],
            }
            raw["lifecycle"]["candidate_marker"] = dict(
                contract["candidate_marker"]
            )
            return raw

        phase6da_direct_baseline_raw = phase6da_direct_raw(
            PHASE6DA_DIRECT_CONTRACTS[0]
        )
        phase6da_direct_candidate_raw = phase6da_direct_raw(
            PHASE6DA_DIRECT_CONTRACTS[1]
        )
        phaseifsm_direct_candidate_raw = phase6da_direct_raw(
            PHASE6DA_DIRECT_CONTRACTS[2]
        )
        ufsb_imse_direct_candidate_raw = phase6da_direct_raw(
            PHASE6DA_DIRECT_CONTRACTS[3]
        )
        fcfos_direct_candidate_raw = phase6da_direct_raw(
            PHASE6DA_DIRECT_CONTRACTS[4]
        )
        phase6da_direct_candidate_raw["provenance"]["binary_sha256"] = (
            phase6da_direct_baseline_raw["provenance"]["binary_sha256"]
        )
        phaseifsm_direct_candidate_raw["provenance"]["binary_sha256"] = (
            "1" * 64
        )
        ufsb_imse_direct_candidate_raw["provenance"]["binary_sha256"] = (
            "2" * 64
        )
        fcfos_direct_candidate_raw["provenance"]["binary_sha256"] = "3" * 64
        combined_identity_root = parent / "ufsb-imse-sealed-identity-positive"
        (combined_identity_root / "records").mkdir(parents=True)
        combined_identity_path = _write_manifest(
            combined_identity_root,
            report_tool,
            ufsb_imse_direct_candidate_raw,
        )
        ufsb_imse_direct_candidate_raw = json.loads(
            json.dumps(report_tool.load_manifest(combined_identity_path).raw)
        )
        fcfos_identity_root = parent / "fcfos-sealed-identity-positive"
        (fcfos_identity_root / "records").mkdir(parents=True)
        fcfos_identity_path = _write_manifest(
            fcfos_identity_root,
            report_tool,
            fcfos_direct_candidate_raw,
        )
        fcfos_direct_candidate_raw = json.loads(
            json.dumps(report_tool.load_manifest(fcfos_identity_path).raw)
        )

        def phase6da_direct_manifest(raw: Mapping[str, Any]) -> Any:
            expected_selectors = raw["provenance"]["selectors"]["expected"]
            fcfos_signal = (
                raw["provenance"]["record_id"]
                == FCFOS_DIRECT_CANDIDATE_RECORD_ID
                or FCFOS_CANDIDATE_SELECTOR in expected_selectors
            )
            combined_signal = (
                raw["provenance"]["record_id"]
                == UFSB_IMSE_DIRECT_CANDIDATE_RECORD_ID
                or UFSB_CANDIDATE_SELECTOR in expected_selectors
                or IMSE_COMPANION_SELECTOR in expected_selectors
            )
            if fcfos_signal:
                selected_path = fcfos_identity_path
            elif combined_signal:
                selected_path = combined_identity_path
            else:
                selected_path = missing_receipt_manifest.path
            return report_tool.RunManifest(
                path=selected_path,
                content_sha256=_sha_file(selected_path),
                raw=raw,
            )

        phase6da_direct_baseline = phase6da_direct_manifest(
            phase6da_direct_baseline_raw
        )
        phase6da_direct_candidate = phase6da_direct_manifest(
            phase6da_direct_candidate_raw
        )
        phaseifsm_direct_candidate = phase6da_direct_manifest(
            phaseifsm_direct_candidate_raw
        )
        ufsb_imse_direct_candidate = report_tool.load_manifest(
            combined_identity_path
        )
        fcfos_direct_candidate = report_tool.load_manifest(
            fcfos_identity_path
        )
        _verify_record_artifacts(phase6da_direct_baseline)
        _verify_record_artifacts(phase6da_direct_candidate)
        _verify_record_artifacts(phaseifsm_direct_candidate)
        _verify_record_artifacts(ufsb_imse_direct_candidate)
        _verify_record_artifacts(fcfos_direct_candidate)

        def reject_combined_sealed_evidence(
            label: str,
            *,
            mutate_start: Optional[Any] = None,
            stderr_lines: Optional[Sequence[str]] = None,
        ) -> None:
            case_root = parent / f"ufsb-imse-sealed-negative-{label}"
            (case_root / "records").mkdir(parents=True)
            raw = json.loads(json.dumps(ufsb_imse_direct_candidate_raw))
            manifest_path = _write_manifest(case_root, report_tool, raw)
            record_root = manifest_path.parent
            artifacts = raw["provenance"]["artifacts_sha256"]
            if mutate_start is not None:
                start_path = record_root / "start-config.json"
                start_value = json.loads(json.dumps(_load_json(start_path)))
                mutate_start(start_value)
                _atomic_json(start_path, start_value, replace=True)
                artifacts["start_config"] = _sha_file(start_path)
            if stderr_lines is not None:
                stderr_path = record_root / "stderr.log"
                payload = (
                    "stderr combined sealed negative\n"
                    + "\n".join(stderr_lines)
                    + ("\n" if stderr_lines else "")
                ).encode("utf-8")
                _atomic_bytes(stderr_path, payload, replace=True)
                artifacts["stderr_log"] = _sha_file(stderr_path)
            _atomic_json(manifest_path, raw, replace=True)
            try:
                _verify_record_artifacts(
                    report_tool.load_manifest(manifest_path)
                )
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted UFSB+IMSE sealed evidence {label}"
                )

        reject_combined_sealed_evidence(
            "wrong-primary-identity",
            mutate_start=lambda value: value["provenance"].update(
                {"candidate_selector": IMSE_COMPANION_SELECTOR}
            ),
        )
        reject_combined_sealed_evidence(
            "missing-companion-identity",
            mutate_start=lambda value: value["provenance"][
                "selectors"
            ].pop(IMSE_COMPANION_SELECTOR),
        )
        reject_combined_sealed_evidence(
            "wrong-primary-marker-identity",
            mutate_start=lambda value: value["candidate_marker"].update(
                {"prefix": IMSE_COMPANION_MARKER_PREFIX}
            ),
        )
        for label, stderr_lines in (
            ("primary-missing", (IMSE_COMPANION_MARKER_LINE,)),
            (
                "primary-duplicate",
                (
                    UFSB_PRIMARY_MARKER_LINE,
                    UFSB_PRIMARY_MARKER_LINE,
                    IMSE_COMPANION_MARKER_LINE,
                ),
            ),
            (
                "primary-inexact-line",
                (
                    UFSB_PRIMARY_MARKER_LINE.replace(
                        "dependency=positive compared=positive",
                        "compared=positive dependency=positive",
                    ),
                    IMSE_COMPANION_MARKER_LINE,
                ),
            ),
            (
                "primary-zero-coverage",
                (
                    UFSB_PRIMARY_MARKER_LINE.replace(
                        "coverage=positive", "coverage=zero"
                    ),
                    IMSE_COMPANION_MARKER_LINE,
                ),
            ),
            ("companion-missing", (UFSB_PRIMARY_MARKER_LINE,)),
            (
                "companion-duplicate",
                (
                    UFSB_PRIMARY_MARKER_LINE,
                    IMSE_COMPANION_MARKER_LINE,
                    IMSE_COMPANION_MARKER_LINE,
                ),
            ),
            (
                "companion-inexact-line",
                (
                    UFSB_PRIMARY_MARKER_LINE,
                    IMSE_COMPANION_MARKER_LINE.replace(
                        "requested=positive effective=positive",
                        "effective=positive requested=positive",
                    ),
                ),
            ),
            (
                "companion-zero-coverage",
                (
                    UFSB_PRIMARY_MARKER_LINE,
                    IMSE_COMPANION_MARKER_LINE.replace(
                        "coverage=positive", "coverage=zero"
                    ),
                ),
            ),
        ):
            reject_combined_sealed_evidence(
                label, stderr_lines=stderr_lines
            )

        def reject_fcfos_sealed_evidence(
            label: str,
            *,
            mutate_start: Optional[Any] = None,
            stderr_lines: Optional[Sequence[str]] = None,
        ) -> None:
            case_root = parent / f"fcfos-sealed-negative-{label}"
            (case_root / "records").mkdir(parents=True)
            raw = json.loads(json.dumps(fcfos_direct_candidate_raw))
            manifest_path = _write_manifest(case_root, report_tool, raw)
            record_root = manifest_path.parent
            artifacts = raw["provenance"]["artifacts_sha256"]
            if mutate_start is not None:
                start_path = record_root / "start-config.json"
                start_value = json.loads(json.dumps(_load_json(start_path)))
                mutate_start(start_value)
                _atomic_json(start_path, start_value, replace=True)
                artifacts["start_config"] = _sha_file(start_path)
            if stderr_lines is not None:
                stderr_path = record_root / "stderr.log"
                payload = (
                    "stderr FCFOS sealed negative\n"
                    + "\n".join(stderr_lines)
                    + ("\n" if stderr_lines else "")
                ).encode("utf-8")
                _atomic_bytes(stderr_path, payload, replace=True)
                artifacts["stderr_log"] = _sha_file(stderr_path)
            _atomic_json(manifest_path, raw, replace=True)
            try:
                _verify_record_artifacts(
                    report_tool.load_manifest(manifest_path)
                )
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted FCFOS sealed evidence {label}"
                )

        reject_fcfos_sealed_evidence(
            "wrong-primary-identity",
            mutate_start=lambda value: value["provenance"].update(
                {"candidate_selector": UFSB_CANDIDATE_SELECTOR}
            ),
        )
        reject_fcfos_sealed_evidence(
            "sibling-selector-present",
            mutate_start=lambda value: value["provenance"][
                "selectors"
            ].update({UFSB_CANDIDATE_SELECTOR: False}),
        )
        reject_fcfos_sealed_evidence(
            "wrong-selector-receipt",
            mutate_start=lambda value: value["provenance"][
                "selector_receipts"
            ][FCFOS_CANDIDATE_SELECTOR].update(
                {"field": "fast_block_inline_fixed_signature_match"}
            ),
        )
        reject_fcfos_sealed_evidence(
            "wrong-marker-identity",
            mutate_start=lambda value: value["candidate_marker"].update(
                {"prefix": UFSB_PRIMARY_MARKER_PREFIX}
            ),
        )
        for label, stderr_lines in (
            ("missing", ()),
            ("duplicate", (FCFOS_MARKER_LINE, FCFOS_MARKER_LINE)),
            (
                "inexact-line",
                (
                    FCFOS_MARKER_LINE.replace(
                        "reentry_safe=positive snapshot_elision=positive",
                        "snapshot_elision=positive reentry_safe=positive",
                    ),
                ),
            ),
            (
                "reentry-not-positive",
                (
                    FCFOS_MARKER_LINE.replace(
                        "reentry_safe=positive", "reentry_safe=zero"
                    ),
                ),
            ),
            (
                "snapshot-elision-not-positive",
                (
                    FCFOS_MARKER_LINE.replace(
                        "snapshot_elision=positive",
                        "snapshot_elision=zero",
                    ),
                ),
            ),
            (
                "unclean",
                (FCFOS_MARKER_LINE.replace("status=clean", "status=dirty"),),
            ),
            (
                "zero-coverage",
                (
                    FCFOS_MARKER_LINE.replace(
                        "coverage=positive", "coverage=zero"
                    ),
                ),
            ),
        ):
            reject_fcfos_sealed_evidence(label, stderr_lines=stderr_lines)

        phase6da_direct_negative_cases = []
        wrong_direct_protocol = json.loads(
            json.dumps(phase6da_direct_baseline_raw)
        )
        wrong_direct_protocol["protocol"]["protocol_id"] = "wrong-protocol"
        phase6da_direct_negative_cases.append(
            ("record-with-wrong-protocol", wrong_direct_protocol)
        )
        wrong_direct_os = json.loads(json.dumps(phase6da_direct_baseline_raw))
        wrong_direct_os["comparability"]["os"]["build"] = "wrong-build"
        phase6da_direct_negative_cases.append(
            ("record-with-wrong-os", wrong_direct_os)
        )
        wrong_direct_os_name = json.loads(
            json.dumps(phase6da_direct_baseline_raw)
        )
        wrong_direct_os_name["comparability"]["os"]["name"] = "Darwin"
        phase6da_direct_negative_cases.append(
            ("record-with-wrong-os-name", wrong_direct_os_name)
        )
        wrong_direct_os_policy = json.loads(
            json.dumps(phase6da_direct_baseline_raw)
        )
        wrong_direct_os_policy["comparability"]["os"]["policy"] = (
            "floating-version"
        )
        phase6da_direct_negative_cases.append(
            ("record-with-wrong-os-policy", wrong_direct_os_policy)
        )
        wrong_direct_configuration = json.loads(
            json.dumps(phase6da_direct_baseline_raw)
        )
        wrong_direct_configuration["comparability"]["runtime"][
            "configuration_sha256"
        ] = "0" * 64
        phase6da_direct_negative_cases.append(
            (
                "record-with-wrong-configuration",
                wrong_direct_configuration,
            )
        )
        unknown_direct_record = json.loads(
            json.dumps(phase6da_direct_baseline_raw)
        )
        unknown_direct_record["provenance"]["record_id"] = "unknown-record"
        phase6da_direct_negative_cases.append(
            ("protocol-with-unknown-record", unknown_direct_record)
        )
        baseline_selector_on = json.loads(
            json.dumps(phase6da_direct_baseline_raw)
        )
        for selector_map in baseline_selector_on["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR] = True
        phase6da_direct_negative_cases.append(
            ("baseline-selector-on", baseline_selector_on)
        )
        candidate_selector_off = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        for selector_map in candidate_selector_off["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negative_cases.append(
            ("candidate-selector-off", candidate_selector_off)
        )
        integer_direct_selector = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        for selector_map in integer_direct_selector["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR] = 1
        phase6da_direct_negative_cases.append(
            ("integer-candidate-selector", integer_direct_selector)
        )
        mismatched_effective = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        mismatched_effective["provenance"]["selectors"]["effective"][
            PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR
        ] = False
        phase6da_direct_negative_cases.append(
            ("expected-effective-mismatch", mismatched_effective)
        )
        retired_direct_selector = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        for selector_map in retired_direct_selector["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6CQ_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negative_cases.append(
            ("retired-selector-present", retired_direct_selector)
        )
        missing_retained_selector = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        for selector_map in missing_retained_selector["provenance"][
            "selectors"
        ].values():
            del selector_map[CONFIGURATION_PROJECTION_STABLE_SELECTOR]
        phase6da_direct_negative_cases.append(
            ("missing-retained-selector", missing_retained_selector)
        )
        wrong_direct_parent = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        wrong_direct_parent["lineage"]["parent_record_id"] = "wrong-parent"
        phase6da_direct_negative_cases.append(
            ("wrong-candidate-parent", wrong_direct_parent)
        )
        wrong_direct_generation = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        wrong_direct_generation["lineage"]["generation"] = 2
        phase6da_direct_negative_cases.append(
            ("wrong-candidate-generation", wrong_direct_generation)
        )
        wrong_direct_receipt = json.loads(
            json.dumps(phase6da_direct_baseline_raw)
        )
        wrong_direct_receipt["provenance"]["run_receipt"] = (
            wrong_direct_receipt["provenance"]["run_receipt"].replace(
                "fast_block_dense_directory_requested=0",
                "fast_block_dense_directory_requested=1",
            )
        )
        phase6da_direct_negative_cases.append(
            ("wrong-baseline-receipt", wrong_direct_receipt)
        )
        wrong_direct_marker = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        wrong_direct_marker["lifecycle"]["candidate_marker"][
            "coverage_positive"
        ] = False
        phase6da_direct_negative_cases.append(
            ("candidate-marker-not-positive", wrong_direct_marker)
        )
        direct_projection_artifacts = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        for name in CONFIGURATION_PROJECTION_ARTIFACTS:
            direct_projection_artifacts["provenance"]["artifacts_sha256"][
                name
            ] = "0" * 64
        phase6da_direct_negative_cases.append(
            ("projection-artifacts-declared", direct_projection_artifacts)
        )

        wrong_ifsm_parent = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        wrong_ifsm_parent["lineage"]["parent_record_id"] = (
            PHASE6DA_DIRECT_BASELINE_RECORD_ID
        )
        phase6da_direct_negative_cases.append(
            ("ifsm-wrong-parent", wrong_ifsm_parent)
        )
        wrong_ifsm_generation = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        wrong_ifsm_generation["lineage"]["generation"] = 3
        phase6da_direct_negative_cases.append(
            ("ifsm-wrong-generation", wrong_ifsm_generation)
        )
        ifsm_selector_off = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        for selector_map in ifsm_selector_off["provenance"][
            "selectors"
        ].values():
            selector_map[PHASEIFSM_CANDIDATE_SELECTOR] = False
        phase6da_direct_negative_cases.append(
            ("ifsm-selector-off", ifsm_selector_off)
        )
        integer_ifsm_selector = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        for selector_map in integer_ifsm_selector["provenance"][
            "selectors"
        ].values():
            selector_map[PHASEIFSM_CANDIDATE_SELECTOR] = 1
        phase6da_direct_negative_cases.append(
            ("ifsm-selector-integer", integer_ifsm_selector)
        )
        ifsm_effective_mismatch = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        ifsm_effective_mismatch["provenance"]["selectors"]["effective"][
            PHASEIFSM_CANDIDATE_SELECTOR
        ] = False
        phase6da_direct_negative_cases.append(
            ("ifsm-expected-effective-mismatch", ifsm_effective_mismatch)
        )
        ifsm_dense_selector_off = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        for selector_map in ifsm_dense_selector_off["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6DA_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negative_cases.append(
            ("ifsm-retained-6da-off", ifsm_dense_selector_off)
        )
        ifsm_retired_selector_present = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        for selector_map in ifsm_retired_selector_present["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negative_cases.append(
            ("ifsm-retired-selector-present", ifsm_retired_selector_present)
        )
        ifsm_selector_wrong_record = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        ifsm_selector_wrong_record["provenance"]["record_id"] = (
            "ifsm-c-wrong-record-g000002"
        )
        phase6da_direct_negative_cases.append(
            ("ifsm-selector-wrong-record", ifsm_selector_wrong_record)
        )
        wrong_ifsm_receipt = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        wrong_ifsm_receipt["provenance"]["run_receipt"] = (
            wrong_ifsm_receipt["provenance"]["run_receipt"].replace(
                "fast_block_inline_fixed_signature_match=1",
                "fast_block_inline_fixed_signature_match=0",
            )
        )
        phase6da_direct_negative_cases.append(
            ("ifsm-wrong-effective-receipt", wrong_ifsm_receipt)
        )
        wrong_ifsm_requested_receipt = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        wrong_ifsm_requested_receipt["provenance"]["run_receipt"] = (
            wrong_ifsm_requested_receipt["provenance"][
                "run_receipt"
            ].replace(
                "fast_block_inline_fixed_signature_match_requested=1",
                "fast_block_inline_fixed_signature_match_requested=0",
            )
        )
        phase6da_direct_negative_cases.append(
            ("ifsm-wrong-requested-receipt", wrong_ifsm_requested_receipt)
        )
        wrong_ifsm_marker = json.loads(
            json.dumps(phaseifsm_direct_candidate_raw)
        )
        wrong_ifsm_marker["lifecycle"]["candidate_marker"]["status"] = (
            "invalid"
        )
        phase6da_direct_negative_cases.append(
            ("ifsm-marker-invalid", wrong_ifsm_marker)
        )
        wrong_bundle_parent = json.loads(
            json.dumps(ufsb_imse_direct_candidate_raw)
        )
        wrong_bundle_parent["lineage"]["parent_record_id"] = (
            PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
        )
        phase6da_direct_negative_cases.append(
            ("ufsb-imse-wrong-parent", wrong_bundle_parent)
        )
        for label, selector in (
            ("ufsb-off", UFSB_CANDIDATE_SELECTOR),
            ("imse-off", IMSE_COMPANION_SELECTOR),
        ):
            value = json.loads(json.dumps(ufsb_imse_direct_candidate_raw))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[selector] = False
            phase6da_direct_negative_cases.append((label, value))
        for label, field in (
            (
                "ufsb-effective-receipt-zero",
                "fast_block_unrolled_fixed_signature_batching",
            ),
            (
                "imse-effective-receipt-zero",
                "fast_block_idempotent_mru_store_elision",
            ),
        ):
            value = json.loads(json.dumps(ufsb_imse_direct_candidate_raw))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}=1", f"{field}=0")
            phase6da_direct_negative_cases.append((label, value))
        extra_bundle_selector = json.loads(
            json.dumps(ufsb_imse_direct_candidate_raw)
        )
        for selector_map in extra_bundle_selector["provenance"][
            "selectors"
        ].values():
            selector_map[PHASE6CS_CONFIGURATION_PROJECTION_SELECTOR] = False
        phase6da_direct_negative_cases.append(
            ("ufsb-imse-retired-selector-present", extra_bundle_selector)
        )
        unknown_bundle_record = json.loads(
            json.dumps(ufsb_imse_direct_candidate_raw)
        )
        unknown_bundle_record["provenance"]["record_id"] = (
            "ufsb-imse-c-wrong-record-g000003"
        )
        phase6da_direct_negative_cases.append(
            ("ufsb-imse-selector-wrong-record", unknown_bundle_record)
        )
        wrong_fcfos_parent = json.loads(
            json.dumps(fcfos_direct_candidate_raw)
        )
        wrong_fcfos_parent["lineage"]["parent_record_id"] = (
            PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
        )
        phase6da_direct_negative_cases.append(
            ("fcfos-wrong-parent", wrong_fcfos_parent)
        )
        wrong_fcfos_generation = json.loads(
            json.dumps(fcfos_direct_candidate_raw)
        )
        wrong_fcfos_generation["lineage"]["generation"] = 4
        phase6da_direct_negative_cases.append(
            ("fcfos-wrong-generation", wrong_fcfos_generation)
        )
        fcfos_selector_off = json.loads(
            json.dumps(fcfos_direct_candidate_raw)
        )
        for selector_map in fcfos_selector_off["provenance"][
            "selectors"
        ].values():
            selector_map[FCFOS_CANDIDATE_SELECTOR] = False
        phase6da_direct_negative_cases.append(
            ("fcfos-selector-off", fcfos_selector_off)
        )
        integer_fcfos_selector = json.loads(
            json.dumps(fcfos_direct_candidate_raw)
        )
        for selector_map in integer_fcfos_selector["provenance"][
            "selectors"
        ].values():
            selector_map[FCFOS_CANDIDATE_SELECTOR] = 1
        phase6da_direct_negative_cases.append(
            ("fcfos-selector-integer", integer_fcfos_selector)
        )
        for label, selector in (
            ("fcfos-ufsb-sibling-present", UFSB_CANDIDATE_SELECTOR),
            ("fcfos-imse-sibling-present", IMSE_COMPANION_SELECTOR),
        ):
            value = json.loads(json.dumps(fcfos_direct_candidate_raw))
            for selector_map in value["provenance"]["selectors"].values():
                selector_map[selector] = False
            phase6da_direct_negative_cases.append((label, value))
        for label, field, expected, invalid in (
            (
                "fcfos-requested-receipt-zero",
                "fast_block_callback_free_ordinary_specialization_requested",
                "1",
                "0",
            ),
            (
                "fcfos-effective-receipt-zero",
                "fast_block_callback_free_ordinary_specialization",
                "1",
                "0",
            ),
            (
                "fcfos-ufsb-sibling-receipt-on",
                "fast_block_unrolled_fixed_signature_batching_requested",
                "0",
                "1",
            ),
            (
                "fcfos-imse-sibling-receipt-on",
                "fast_block_idempotent_mru_store_elision",
                "0",
                "1",
            ),
        ):
            value = json.loads(json.dumps(fcfos_direct_candidate_raw))
            value["provenance"]["run_receipt"] = value["provenance"][
                "run_receipt"
            ].replace(f"{field}={expected}", f"{field}={invalid}")
            phase6da_direct_negative_cases.append((label, value))
        wrong_fcfos_marker = json.loads(
            json.dumps(fcfos_direct_candidate_raw)
        )
        wrong_fcfos_marker["lifecycle"]["candidate_marker"][
            "coverage_positive"
        ] = False
        phase6da_direct_negative_cases.append(
            ("fcfos-marker-not-positive", wrong_fcfos_marker)
        )
        unknown_fcfos_record = json.loads(
            json.dumps(fcfos_direct_candidate_raw)
        )
        unknown_fcfos_record["provenance"]["record_id"] = (
            "fcfos-c-wrong-record-g000003"
        )
        phase6da_direct_negative_cases.append(
            ("fcfos-selector-wrong-record", unknown_fcfos_record)
        )
        for label, raw in phase6da_direct_negative_cases:
            try:
                _verify_record_artifacts(phase6da_direct_manifest(raw))
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted invalid phase-6DA direct record {label}"
                )

        for name, filename in CONFIGURATION_PROJECTION_ARTIFACTS.items():
            _atomic_bytes(
                projection_root / filename,
                f"spurious direct projection {name}\n".encode("ascii"),
            )
        try:
            _verify_record_artifacts(phaseifsm_direct_candidate)
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted physical projection artifacts on a phase-6DA direct record"
            )
        finally:
            for filename in CONFIGURATION_PROJECTION_ARTIFACTS.values():
                (projection_root / filename).unlink()

        direct_decision_root = parent / "phase6da-direct-decision-context"
        direct_decision_root.mkdir()
        (direct_decision_root / "records").mkdir()
        (direct_decision_root / "generations").mkdir()
        direct_baseline_path = _write_manifest(
            direct_decision_root,
            report_tool,
            json.loads(json.dumps(phase6da_direct_baseline_raw)),
        )
        direct_phase6da_path = _write_manifest(
            direct_decision_root,
            report_tool,
            json.loads(json.dumps(phase6da_direct_candidate_raw)),
        )
        direct_ifsm_path = _write_manifest(
            direct_decision_root,
            report_tool,
            json.loads(json.dumps(phaseifsm_direct_candidate_raw)),
        )
        direct_bundle_path = _write_manifest(
            direct_decision_root,
            report_tool,
            json.loads(json.dumps(ufsb_imse_direct_candidate_raw)),
        )
        direct_fcfos_path = _write_manifest(
            direct_decision_root,
            report_tool,
            json.loads(json.dumps(fcfos_direct_candidate_raw)),
        )
        direct_baseline = report_tool.load_manifest(direct_baseline_path)
        direct_phase6da = report_tool.load_manifest(direct_phase6da_path)
        direct_ifsm = report_tool.load_manifest(direct_ifsm_path)
        direct_bundle = report_tool.load_manifest(direct_bundle_path)
        direct_fcfos = report_tool.load_manifest(direct_fcfos_path)
        direct_epoch = {
            "protocol": direct_baseline.protocol,
            "comparability": direct_baseline.comparability,
        }
        _require(
            _validate_decision_context(
                direct_decision_root,
                direct_epoch,
                direct_baseline,
                direct_phase6da,
            )
            is None,
            "self-test",
            "phase-6DA direct initial decision did not retain initial context",
        )
        different_mach_o_raw = json.loads(
            json.dumps(phase6da_direct_candidate_raw)
        )
        different_mach_o_raw["provenance"]["binary_sha256"] = "0" * 64
        try:
            _validate_decision_context(
                direct_decision_root,
                direct_epoch,
                direct_baseline,
                phase6da_direct_manifest(different_mach_o_raw),
            )
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted an initial phase-6DA B/C decision with different Mach-O hashes"
            )

        direct_phase6da_descriptor = _record_descriptor(
            direct_decision_root, direct_phase6da
        )
        direct_phase6da_decision_id = (
            "promote-g000001-" + PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
        )
        _ensure_generation(
            direct_decision_root,
            PHASE6DA_DIRECT_PROTOCOL_ID,
            direct_phase6da_descriptor,
            direct_phase6da_decision_id,
        )
        direct_pointer_path = direct_decision_root / "current-baseline.json"
        valid_g1_pointer = _pointer_for(
            PHASE6DA_DIRECT_PROTOCOL_ID,
            direct_phase6da_descriptor,
            direct_phase6da_decision_id,
        )
        _atomic_json(direct_pointer_path, valid_g1_pointer)
        subsequent_pointer = _validate_decision_context(
            direct_decision_root,
            direct_epoch,
            direct_phase6da,
            direct_ifsm,
        )
        _require(
            subsequent_pointer is not None
            and subsequent_pointer["record_id"]
            == PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
            and direct_ifsm.provenance["binary_sha256"]
            != direct_phase6da.provenance["binary_sha256"],
            "self-test",
            "IFSM g2 did not retain candidate-only lineage from the promoted 6DA g1 baseline",
        )

        def reject_pointer_tamper(label: str, mutate: Any) -> None:
            value = json.loads(json.dumps(valid_g1_pointer))
            mutate(value)
            _atomic_json(direct_pointer_path, value, replace=True)
            try:
                _validate_decision_context(
                    direct_decision_root,
                    direct_epoch,
                    direct_phase6da,
                    direct_ifsm,
                )
            except RegistryError:
                pass
            else:
                raise RegistryError(
                    f"self-test accepted current pointer tamper {label}"
                )
            finally:
                _atomic_json(
                    direct_pointer_path, valid_g1_pointer, replace=True
                )

        for label, mutate in (
            (
                "schema-name",
                lambda value: value["schema"].update({"name": "wrong"}),
            ),
            (
                "schema-version",
                lambda value: value["schema"].update({"version": 2}),
            ),
            (
                "protocol-id",
                lambda value: value.update({"protocol_id": "wrong-protocol"}),
            ),
            ("generation", lambda value: value.update({"generation": 2})),
            (
                "record-id",
                lambda value: value.update({"record_id": "wrong-record"}),
            ),
            (
                "parent-record-id",
                lambda value: value.update(
                    {"parent_record_id": "wrong-parent"}
                ),
            ),
            (
                "manifest",
                lambda value: value.update(
                    {
                        "manifest": (
                            "records/"
                            + PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
                            + "/wrong.json"
                        )
                    }
                ),
            ),
            (
                "manifest-sha256",
                lambda value: value.update({"manifest_sha256": "0" * 64}),
            ),
            (
                "decision-id",
                lambda value: value.update(
                    {"decision_id": "promote-g000001-wrong-record"}
                ),
            ),
            ("missing-key", lambda value: value.pop("decision_id")),
            (
                "extra-key",
                lambda value: value.update({"unexpected": False}),
            ),
        ):
            reject_pointer_tamper(label, mutate)

        direct_g1_generation_path = (
            direct_decision_root / "generations" / "g000001.json"
        )
        valid_g1_generation = _generation_value(
            direct_decision_root,
            PHASE6DA_DIRECT_PROTOCOL_ID,
            direct_phase6da_descriptor,
            direct_phase6da_decision_id,
        )
        wrong_g1_generation = json.loads(json.dumps(valid_g1_generation))
        wrong_g1_generation["decision_id"] = "promote-g000001-wrong-record"
        _atomic_json(
            direct_g1_generation_path, wrong_g1_generation, replace=True
        )
        try:
            _validate_decision_context(
                direct_decision_root,
                direct_epoch,
                direct_phase6da,
                direct_ifsm,
            )
        except RegistryError:
            pass
        else:
            raise RegistryError(
                "self-test accepted a pointer detached from its generation descriptor"
            )
        finally:
            _atomic_json(
                direct_g1_generation_path, valid_g1_generation, replace=True
            )

        direct_ifsm_descriptor = _record_descriptor(
            direct_decision_root, direct_ifsm
        )
        direct_ifsm_decision_id = (
            "promote-g000002-" + PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
        )
        _ensure_generation(
            direct_decision_root,
            PHASE6DA_DIRECT_PROTOCOL_ID,
            direct_ifsm_descriptor,
            direct_ifsm_decision_id,
        )
        _atomic_json(
            direct_pointer_path,
            _pointer_for(
                PHASE6DA_DIRECT_PROTOCOL_ID,
                direct_ifsm_descriptor,
                direct_ifsm_decision_id,
            ),
            replace=True,
        )
        bundle_pointer = _validate_decision_context(
            direct_decision_root,
            direct_epoch,
            direct_ifsm,
            direct_bundle,
        )
        _require(
            bundle_pointer is not None
            and bundle_pointer["record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            and direct_bundle.lineage["generation"] == 3
            and direct_bundle.lineage["parent_record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
            "self-test",
            "UFSB+IMSE g3 did not retain candidate-only lineage from IFSM g2",
        )
        fcfos_pointer = _validate_decision_context(
            direct_decision_root,
            direct_epoch,
            direct_ifsm,
            direct_fcfos,
        )
        _require(
            fcfos_pointer is not None
            and fcfos_pointer["record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            and direct_fcfos.lineage["generation"] == 3
            and direct_fcfos.lineage["parent_record_id"]
            == PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID,
            "self-test",
            "FCFOS g3 did not retain candidate-only sibling lineage from IFSM g2",
        )

        # Exercise the registered IFSM successor through the public decision
        # path, not only through its context validator.  Promotion must publish
        # g2 and advance the pointer; rejection must archive the same report
        # while leaving the promoted 6DA g1 pointer and generation set intact.
        direct_epoch_config = {
            "schema": {
                "name": EPOCH_SCHEMA_NAME,
                "version": EPOCH_SCHEMA_VERSION,
            },
            "protocol": direct_epoch["protocol"],
            "comparability": direct_epoch["comparability"],
        }

        def exercise_ifsm_g2_decision(decision: str) -> None:
            registry_root = parent / f"phaseifsm-{decision}-registry"
            epoch_path = parent / f"phaseifsm-{decision}-epoch.json"
            _atomic_json(epoch_path, direct_epoch_config)
            init_registry(registry_root, epoch_path)

            baseline_path = _write_manifest(
                registry_root,
                report_tool,
                json.loads(json.dumps(phase6da_direct_baseline_raw)),
            )
            phase6da_path = _write_manifest(
                registry_root,
                report_tool,
                json.loads(json.dumps(phase6da_direct_candidate_raw)),
            )
            ifsm_path = _write_manifest(
                registry_root,
                report_tool,
                json.loads(json.dumps(phaseifsm_direct_candidate_raw)),
            )
            phase6da_manifest = report_tool.load_manifest(phase6da_path)
            ifsm_manifest = report_tool.load_manifest(ifsm_path)

            initial_report = registry_root / "reports" / "phase6da-g1.json"
            _write_report(
                initial_report,
                report_tool,
                baseline_path,
                phase6da_path,
            )
            promoted_g1_pointer_path = promote(
                registry_root,
                initial_report,
                "CONFIRM_PROMOTE",
            )
            promoted_g1_pointer = _pointer_value(registry_root)
            phase6da_descriptor = _record_descriptor(
                registry_root, phase6da_manifest
            )
            phase6da_decision_id = (
                "promote-g000001-" + PHASE6DA_DIRECT_CANDIDATE_RECORD_ID
            )
            _require(
                promoted_g1_pointer
                == _pointer_for(
                    PHASE6DA_DIRECT_PROTOCOL_ID,
                    phase6da_descriptor,
                    phase6da_decision_id,
                )
                and _load_json(
                    registry_root / "generations" / "g000001.json"
                )
                == _generation_value(
                    registry_root,
                    PHASE6DA_DIRECT_PROTOCOL_ID,
                    phase6da_descriptor,
                    phase6da_decision_id,
                ),
                "self-test",
                f"IFSM {decision} fixture did not establish promoted 6DA g1",
            )
            pointer_before = promoted_g1_pointer_path.read_bytes()

            ifsm_report = registry_root / "reports" / "ifsm-g2.json"
            _write_report(
                ifsm_report,
                report_tool,
                phase6da_path,
                ifsm_path,
            )
            ifsm_descriptor = _record_descriptor(
                registry_root, ifsm_manifest
            )
            ifsm_decision_id = (
                f"{decision}-g000002-"
                + PHASEIFSM_DIRECT_CANDIDATE_RECORD_ID
            )
            if decision == "promote":
                result_path = promote(
                    registry_root,
                    ifsm_report,
                    "CONFIRM_PROMOTE",
                )
            else:
                result_path = reject(
                    registry_root,
                    ifsm_report,
                    "CONFIRM_REJECT",
                )

            decision_path = (
                registry_root / "decisions" / f"{ifsm_decision_id}.json"
            )
            decision_value = _load_json(decision_path)
            archived_report = pathlib.Path(decision_value["report"])
            _require(
                decision_value["baseline"] == phase6da_descriptor
                and decision_value["candidate"] == ifsm_descriptor
                and archived_report.name
                == f"{decision_value['report_sha256']}.json"
                and _inside(
                    archived_report, (registry_root / "reports").resolve()
                )
                and archived_report != ifsm_report.resolve()
                and _sha_file(archived_report)
                == decision_value["report_sha256"]
                and archived_report.stat().st_mode
                & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)
                == 0,
                "self-test",
                f"IFSM g2 {decision} did not bind its immutable report archive",
            )

            generation_path = (
                registry_root / "generations" / "g000002.json"
            )
            if decision == "promote":
                _require(
                    result_path == promoted_g1_pointer_path
                    and _pointer_value(registry_root)
                    == _pointer_for(
                        PHASE6DA_DIRECT_PROTOCOL_ID,
                        ifsm_descriptor,
                        ifsm_decision_id,
                    )
                    and _load_json(generation_path)
                    == _generation_value(
                        registry_root,
                        PHASE6DA_DIRECT_PROTOCOL_ID,
                        ifsm_descriptor,
                        ifsm_decision_id,
                    )
                    and promoted_g1_pointer_path.read_bytes()
                    != pointer_before,
                    "self-test",
                    "IFSM g2 promotion did not publish generation and pointer",
                )
            else:
                _require(
                    result_path == decision_path.resolve()
                    and not generation_path.exists()
                    and _pointer_value(registry_root) == promoted_g1_pointer
                    and promoted_g1_pointer_path.read_bytes()
                    == pointer_before,
                    "self-test",
                    "IFSM g2 rejection changed generation or promoted 6DA g1 pointer",
                )

        exercise_ifsm_g2_decision("promote")
        exercise_ifsm_g2_decision("reject")

        report_before = _sha_file(comparison)
        pointer = promote(root, comparison, "CONFIRM_PROMOTE")
        pointer_value = _pointer_value(root)
        _require(pointer_value is not None and pointer_value["record_id"] == "candidate" and pointer_value["generation"] == 1, "self-test", "promotion pointer mismatch")
        _require(_sha_file(comparison) == report_before, "self-test", "promotion mutated report")
        _require((root / "generations" / "g000000.json").is_file() and (root / "generations" / "g000001.json").is_file(), "self-test", "initial lineage generations missing")
        promote_decision = root / "decisions" / "promote-g000001-candidate.json"
        promote_decision_value = _load_json(promote_decision)
        promoted_archive = pathlib.Path(promote_decision_value["report"])
        _require(
            _inside(promoted_archive, (root / "reports").resolve())
            and promoted_archive != comparison.resolve()
            and _sha_file(promoted_archive) == promote_decision_value["report_sha256"]
            and promoted_archive.stat().st_mode
            & (stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH)
            == 0,
            "self-test",
            "promotion decision is not bound to an internal immutable report archive",
        )
        pointer_committed = pointer.read_bytes()
        _require(
            promote(root, comparison, "CONFIRM_PROMOTE") == pointer
            and pointer.read_bytes() == pointer_committed,
            "self-test",
            "promotion retry after pointer replace was not idempotent",
        )
        os.chmod(comparison, 0o600)
        comparison.write_bytes(b"modified origin\n")
        _require(
            promote(root, promoted_archive, "CONFIRM_PROMOTE") == pointer
            and pointer.read_bytes() == pointer_committed,
            "self-test",
            "promotion lineage depended on modified report origin",
        )
        comparison.unlink()
        _require(
            promote(root, promoted_archive, "CONFIRM_PROMOTE") == pointer
            and pointer.read_bytes() == pointer_committed,
            "self-test",
            "promotion lineage depended on deleted report origin",
        )

        # The manifest hashes the anchor receipts, and each receipt in turn
        # binds its archived PNGs.  Re-check that second layer for every later
        # report/decision use instead of trusting only the receipt JSON hash.
        candidate_evidence = candidate_path.parent / "evidence" / "fort-anchor.png"
        candidate_evidence_before = candidate_evidence.read_bytes()
        os.chmod(candidate_evidence, 0o600)
        candidate_evidence.write_bytes(candidate_evidence_before + b"tampered")
        os.chmod(candidate_evidence, 0o444)
        evidence_tamper_rejected = False
        try:
            promote(root, promoted_archive, "CONFIRM_PROMOTE")
        except RegistryError:
            evidence_tamper_rejected = True
        finally:
            os.chmod(candidate_evidence, 0o600)
            candidate_evidence.write_bytes(candidate_evidence_before)
            os.chmod(candidate_evidence, 0o444)
        _require(
            evidence_tamper_rejected,
            "self-test",
            "later report use accepted a tampered anchor evidence PNG",
        )
        _require(
            promote(root, promoted_archive, "CONFIRM_PROMOTE") == pointer
            and pointer.read_bytes() == pointer_committed,
            "self-test",
            "restored anchor evidence did not reproduce the committed decision",
        )
        try:
            reject(root, promoted_archive, "CONFIRM_REJECT")
        except RegistryError as error:
            _require(
                "opposite immutable decision" in str(error),
                "self-test",
                "opposite retry failed for a reason other than the committed promotion",
            )
        else:
            raise RegistryError("self-test accepted reject after promote for one candidate")
        _require(pointer.read_bytes() == pointer_committed, "self-test", "opposite decision attempt changed promoted pointer")

        # A rejected successor must leave the current pointer byte-identical.
        successor = report_tool._fixture_manifest(
            record_id="candidate-two", receipt="candidate-two-receipt", generation=2, parent="candidate", role="candidate",
            wall_ns=70, cumulative_ms=7000, fort_interval_ms=7, game_interval_ms=7, revision_digit="d", selector_enabled=True,
        )
        successor["protocol"] = epoch["protocol"]
        successor["comparability"] = epoch["comparability"]
        successor_path = _write_manifest(root, report_tool, successor)
        successor_report = root / "reports" / "successor.json"
        _write_report(successor_report, report_tool, candidate_path, successor_path)
        pointer_before = pointer.read_bytes()
        reject_decision_path = reject(root, successor_report, "CONFIRM_REJECT")
        _require(pointer.read_bytes() == pointer_before, "self-test", "reject mutated current pointer")
        reject_decision_value = _load_json(reject_decision_path)
        rejected_archive = pathlib.Path(reject_decision_value["report"])
        _require(
            _inside(rejected_archive, (root / "reports").resolve())
            and rejected_archive != successor_report.resolve()
            and _sha_file(rejected_archive) == reject_decision_value["report_sha256"],
            "self-test",
            "reject decision is not bound to its internal report archive",
        )
        _require(
            reject(root, successor_report, "CONFIRM_REJECT") == reject_decision_path
            and pointer.read_bytes() == pointer_before,
            "self-test",
            "reject retry was not idempotent",
        )
        try:
            promote(root, rejected_archive, "CONFIRM_PROMOTE")
        except RegistryError as error:
            _require(
                "opposite immutable decision" in str(error),
                "self-test",
                "opposite retry failed for a reason other than the committed rejection",
            )
        else:
            raise RegistryError("self-test accepted promote after reject for one candidate")
        _require(pointer.read_bytes() == pointer_before, "self-test", "opposite decision attempt changed rejected pointer")

        # A rejected record no longer blocks the next candidate at the same
        # prospective generation; an undecided record still does.
        harness_path, _ = _tool_paths()
        harness = _load_module(harness_path, "persistent-performance-harness")
        next_config = {
            "protocol": json.loads(json.dumps(epoch["protocol"])),
            "comparability": json.loads(json.dumps(epoch["comparability"])),
            "lineage": {
                "generation": 2,
                "parent_record_id": "candidate",
                "original_role": "candidate",
            }
        }
        harness._validate_registry_lineage(root, next_config, report_tool)
        reject_decision = next((root / "decisions").glob("reject-g000002-candidate-two.json"))
        os.chmod(successor_report, 0o600)
        successor_report.write_bytes(b"modified reject origin\n")
        harness._validate_registry_lineage(root, next_config, report_tool)
        successor_report.unlink()
        harness._validate_registry_lineage(root, next_config, report_tool)

        archived_before_tamper = rejected_archive.read_bytes()
        os.chmod(rejected_archive, 0o600)
        rejected_archive.write_bytes(b"tampered internal archive\n")
        try:
            harness._validate_registry_lineage(root, next_config, report_tool)
        except Exception:
            pass
        else:
            raise RegistryError("self-test accepted a tampered internal report archive")
        rejected_archive.write_bytes(archived_before_tamper)
        os.chmod(rejected_archive, 0o444)
        _require(
            _sha_file(rejected_archive) == reject_decision_value["report_sha256"],
            "self-test",
            "internal report archive restoration mismatch",
        )
        harness._validate_registry_lineage(root, next_config, report_tool)

        hidden_decision = reject_decision.with_suffix(".hidden")
        os.replace(reject_decision, hidden_decision)
        try:
            try:
                harness._validate_registry_lineage(root, next_config, report_tool)
            except Exception:
                pass
            else:
                raise RegistryError("self-test accepted a second candidate while C2 was undecided")
        finally:
            os.replace(hidden_decision, reject_decision)

        # Initial rejection installs B, never C.
        reject_epoch = _fixture_epoch(report_tool, "registry-reject-v1")
        reject_config = parent / "epoch-reject.json"
        _atomic_json(reject_config, reject_epoch)
        reject_root = parent / "reject-registry"
        init_registry(reject_root, reject_config)
        _, _, reject_report = _fixture_pair(reject_root, report_tool, reject_epoch, "-r")
        reject(reject_root, reject_report, "CONFIRM_REJECT")
        reject_pointer = _pointer_value(reject_root)
        _require(reject_pointer is not None and reject_pointer["record_id"] == "baseline-r" and reject_pointer["generation"] == 0, "self-test", "initial reject did not install B")

        # A crash before os.replace leaves no pointer; the same decision resumes.
        recovery_epoch = _fixture_epoch(report_tool, "registry-recovery-v1")
        recovery_config = parent / "epoch-recovery.json"
        _atomic_json(recovery_config, recovery_epoch)
        recovery_root = parent / "recovery-registry"
        init_registry(recovery_root, recovery_config)
        _, _, recovery_report = _fixture_pair(recovery_root, report_tool, recovery_epoch, "-x")
        try:
            decide(recovery_root, recovery_report, "promote", "CONFIRM_PROMOTE", fail_before_pointer=True)
        except RegistryError:
            pass
        else:
            raise RegistryError("self-test failure injection did not interrupt promotion")
        _require(not (recovery_root / "current-baseline.json").exists(), "self-test", "pre-replace failure changed pointer")
        recovery_decision = next((recovery_root / "decisions").glob("promote-*.json"))
        recovery_archive = pathlib.Path(_load_json(recovery_decision)["report"])
        recovery_report.unlink()
        promote(recovery_root, recovery_archive, "CONFIRM_PROMOTE")
        recovered = _pointer_value(recovery_root)
        _require(recovered is not None and recovered["record_id"] == "candidate-x", "self-test", "promotion recovery failed")

        # A lost response after the atomic pointer replace must be harmless:
        # the same archived decision is a no-op retry, even without its origin.
        post_epoch = _fixture_epoch(report_tool, "registry-post-replace-v1")
        post_config = parent / "epoch-post-replace.json"
        _atomic_json(post_config, post_epoch)
        post_root = parent / "post-replace-registry"
        init_registry(post_root, post_config)
        _, _, post_report = _fixture_pair(post_root, report_tool, post_epoch, "-p")
        try:
            decide(
                post_root,
                post_report,
                "promote",
                "CONFIRM_PROMOTE",
                fail_after_pointer=True,
            )
        except RegistryError:
            pass
        else:
            raise RegistryError("self-test post-replace failure injection did not fire")
        post_pointer = post_root / "current-baseline.json"
        post_pointer_before = post_pointer.read_bytes()
        post_decision = next((post_root / "decisions").glob("promote-*.json"))
        post_archive = pathlib.Path(_load_json(post_decision)["report"])
        post_report.unlink()
        post_retry = promote(post_root, post_archive, "CONFIRM_PROMOTE")
        _require(
            post_retry == post_pointer.resolve(),
            "self-test",
            f"post-replace retry returned {post_retry} instead of {post_pointer.resolve()}",
        )
        _require(
            post_pointer.read_bytes() == post_pointer_before,
            "self-test",
            "post-replace promotion retry was not idempotent",
        )

        pending_claim = post_root / "pending" / "active-run"
        pending_claim.mkdir()
        try:
            try:
                promote(post_root, post_archive, "CONFIRM_PROMOTE")
            except RegistryError:
                pass
            else:
                raise RegistryError("self-test allowed a decision during an active run")
        finally:
            pending_claim.rmdir()
        _require(
            post_pointer.read_bytes() == post_pointer_before,
            "self-test",
            "pending-run decision veto changed the pointer",
        )

        # Report tampering must fail before any mutation.
        tampered = root / "reports" / "tampered.json"
        tampered_value = dict(_load_json(rejected_archive))
        tampered_value["valid"] = False
        _atomic_json(tampered, tampered_value)
        current_before = pointer.read_bytes()
        try:
            promote(root, tampered, "CONFIRM_PROMOTE")
        except RegistryError:
            pass
        else:
            raise RegistryError("self-test accepted a tampered report")
        _require(pointer.read_bytes() == current_before, "self-test", "tampered report changed pointer")


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="initialize and explicitly mutate a persistent performance baseline registry")
    parser.add_argument("--self-test", action="store_true", help="run deterministic lineage, tamper, and atomic recovery fixtures")
    subparsers = parser.add_subparsers(dest="command")
    init_parser = subparsers.add_parser("init", help="create one immutable protocol epoch")
    init_parser.add_argument("root")
    init_parser.add_argument("epoch_config")
    promote_parser = subparsers.add_parser("promote", help="explicitly promote one report candidate")
    promote_parser.add_argument("root")
    promote_parser.add_argument("report")
    promote_parser.add_argument("--attest", required=True)
    reject_parser = subparsers.add_parser("reject", help="explicitly reject one report candidate")
    reject_parser.add_argument("root")
    reject_parser.add_argument("report")
    reject_parser.add_argument("--attest", required=True)
    return parser


def _main(argv: Sequence[str]) -> int:
    parser = _build_parser()
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        _require(arguments.command is None, "arguments", "--self-test accepts no command")
        _self_test()
        print("persistent-performance-registry-self-test: PASS")
        return 0
    if arguments.command is None:
        parser.error("a command is required")
    if arguments.command == "init":
        print(init_registry(pathlib.Path(arguments.root), pathlib.Path(arguments.epoch_config)))
    elif arguments.command == "promote":
        print(promote(pathlib.Path(arguments.root), pathlib.Path(arguments.report), arguments.attest))
    elif arguments.command == "reject":
        print(reject(pathlib.Path(arguments.root), pathlib.Path(arguments.report), arguments.attest))
    else:
        parser.error("unsupported command")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except RegistryError as error:
        print(f"persistent-performance-registry: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
