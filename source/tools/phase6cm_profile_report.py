#!/usr/bin/env python3
"""Validate and report one scene-anchored phase-6CM ordinary diagnostic.

The input is the stderr stream emitted by a Release, thin-arm64 phase-6CM
build.  CONFIG and PROFILE are strict ordered ``key=value`` records.  The
tool validates checksums and cumulative accounting before subtracting the
three preregistered boundaries: start->p2300, Fort anchor->complete, and
in-game anchor->complete.

The result is diagnostic-only.  It can identify a material phase-6CN
population or a consistently dominant measured component, but never claims a
speedup and can never initialize or replace the persistent baseline.
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
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Mapping, Optional, Sequence, Tuple


REPORT_SCHEMA_NAME = "lumen.phase6cm-profile-report"
REPORT_SCHEMA_VERSION = 1
LIVE_BINDING_SCHEMA = "lumen.phase6cm-profile-live-binding/1"
DIAGNOSTIC_SCOPE = "diagnostic_only_not_persistent_baseline"
PROFILE_VERSION = 1
CHECKPOINT_PRESENT = 2300
WINDOW_SIZE = 120
SAMPLE_INTERVAL = 65536
SAMPLE_WINDOW_BLOCKS = 256
ROUTE_BATCH_BLOCKS = 4096
MATERIAL_MIN_SAMPLES = 10000
MATERIAL_MIN_BLOCK_BP = 100
MATERIAL_MIN_STEP_BP = 100
IDENTITY_SKETCH_BITS = 1024
UINT64_MAX = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
UINT64_MASK = UINT64_MAX
FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
ZERO = decimal.Decimal(0)
ONE_THOUSAND = decimal.Decimal(1000)

UNSIGNED_RE = re.compile(r"^(?:0|[1-9][0-9]*)$")
SIGNED_RE = re.compile(r"^(?:0|-?[1-9][0-9]*)$")
DECIMAL_RE = re.compile(r"^(?:0|[1-9][0-9]*)(?:\.[0-9]+)?$")
CHECKSUM_RE = re.compile(r"^[0-9a-f]{16}$")
REASON_RE = re.compile(r"^[a-z]+(?:_[a-z]+)*$")

CONFIG_PREFIX = "[A64_ORDINARY_PHASE6CM_CONFIG] "
PROFILE_PREFIX = "[A64_ORDINARY_PHASE6CM_PROFILE] "
MARKER_PREFIX = "[A64_ORDINARY_PHASE6CM_"
PIPELINE_PREFIX = "[FRAME_PIPELINE] present="
CHECKPOINT_PREFIX = "[FRAME_CHECKPOINT]"
FRAME_LIMIT_PREFIX = "[FRAME_LIMIT]"

CONFIG_FIELDS = (
    "version",
    "compiled",
    "requested",
    "effective",
    "production_shape",
    "signal",
    "signal_installed",
    "sample_interval",
    "sample_window_blocks",
    "route_batch_blocks",
    "material_min_samples",
    "material_min_block_bp",
    "material_min_step_bp",
    "stable_negative_route",
    "clock_floor_ns",
    "thread_clock_floor_ns",
    "body_attribution",
    "mixed_body",
)
CONFIG_EXPECTED: Mapping[str, Any] = {
    "version": 1,
    "compiled": 1,
    "requested": 1,
    "effective": 1,
    "production_shape": 1,
    "signal": "SIGUSR2",
    "signal_installed": 1,
    "sample_interval": SAMPLE_INTERVAL,
    "sample_window_blocks": SAMPLE_WINDOW_BLOCKS,
    "route_batch_blocks": ROUTE_BATCH_BLOCKS,
    "material_min_samples": MATERIAL_MIN_SAMPLES,
    "material_min_block_bp": MATERIAL_MIN_BLOCK_BP,
    "material_min_step_bp": MATERIAL_MIN_STEP_BP,
    "stable_negative_route": 1,
    # Positive placeholders used by deterministic fixtures.  Live values are
    # calibrated at process start and are validated for positivity, not exact
    # equality with these placeholders.
    "clock_floor_ns": 1,
    "thread_clock_floor_ns": 1,
    "body_attribution": "pure_prefix_strata",
    "mixed_body": "component_residual_ns",
}

PROFILE_HEADER_FIELDS = (
    "version",
    "sequence",
    "reason",
    "present",
    "uptime_raw_ns",
    "final",
    "valid",
    "pending_request",
    "signal_owned",
    "snapshot_consistent",
    "accounting_consistent",
)
ROUTE_COUNTER_FIELDS = (
    "route_stable_blocks",
    "route_stable_steps",
    "route_first_negative_blocks",
    "route_first_negative_steps",
    "route_transient_blocks",
    "route_transient_steps",
)
TRANSIENT_COUNTER_FIELDS = (
    "transient_prefer_ordinary_blocks",
    "transient_busy_blocks",
    "transient_capacity_blocks",
    "transient_unhealthy_blocks",
    "transient_delayed_blocks",
    "transient_bypass_blocks",
    "transient_publication_blocks",
    "transient_other_blocks",
)
WINDOW_COUNTER_FIELDS = (
    "windows_scheduled",
    "windows_complete",
    "windows_hle_excluded",
    "windows_reentrant_excluded",
    "windows_dropped",
    "windows_active",
)
SAMPLED_COUNTER_FIELDS = (
    "sampled_blocks",
    "sampled_steps",
    "sampled_wall_ns",
    "sampled_thread_cpu_ns",
)
COMPONENT_COUNTER_FIELDS = (
    "component_dispatch_ns",
    "component_rflags_ns",
    "component_guest_helper_ns",
    "component_signature_guard_ns",
    "component_residual_ns",
)
LOWERING_COUNTER_FIELDS = ("lowered_blocks", "lowering_fail_blocks")
OP_COUNTER_FIELDS = (
    "op_dispatch",
    "op_rflags",
    "op_guest_memory",
    "op_register_helper",
    "op_excluded_helper",
    "op_unsupported",
)
CANDIDATE_COUNTER_FIELDS = (
    "candidate_blocks",
    "candidate_steps",
    "candidate_direct_ops",
    "candidate_helper_ops",
)
HEALTH_COUNTER_FIELDS = ("invalid", "saturated", "active_owners", "flushes")
COUNTER_FIELDS = (
    ROUTE_COUNTER_FIELDS
    + TRANSIENT_COUNTER_FIELDS
    + WINDOW_COUNTER_FIELDS
    + SAMPLED_COUNTER_FIELDS
    + COMPONENT_COUNTER_FIELDS
    + LOWERING_COUNTER_FIELDS
    + OP_COUNTER_FIELDS
    + CANDIDATE_COUNTER_FIELDS
    + HEALTH_COUNTER_FIELDS
)
DERIVED_FIELDS = (
    "candidate_identity_bits",
    "sampled_unflushed_blocks_bound",
    "route_unflushed_blocks_bound",
)
PROFILE_FIELDS = PROFILE_HEADER_FIELDS + COUNTER_FIELDS + DERIVED_FIELDS + ("checksum",)

GAUGE_FIELDS = (
    "windows_active",
    "active_owners",
    "sampled_unflushed_blocks_bound",
    "route_unflushed_blocks_bound",
)
CUMULATIVE_COUNTER_FIELDS = tuple(
    field for field in COUNTER_FIELDS if field not in ("windows_active", "active_owners")
) + ("candidate_identity_bits",)

ALLOWED_REASONS = frozenset(
    {"start", "checkpoint", "signal", "checkpoint_signal", "final"}
)
REASON_CODES = {
    "start": 1,
    "checkpoint": 2,
    "signal": 3,
    "checkpoint_signal": 4,
    "final": 5,
}

PIPELINE_RE = re.compile(
    r"^\[FRAME_PIPELINE\] present=(?P<present>(?:0|[1-9][0-9]*)) "
    r"avg_interval=(?P<interval>(?:0|[1-9][0-9]*)(?:\.[0-9]+)?) ms "
    r"avg_latency=(?P<latency>(?:0|[1-9][0-9]*)(?:\.[0-9]+)?) ms "
    r"pending=(?P<pending>(?:0|[1-9][0-9]*)) "
    r"index=(?P<index>(?:0|-?[1-9][0-9]*)) "
    r"arg=(?P<arg>(?:0|-?[1-9][0-9]*))$"
)
CHECKPOINT_RE = re.compile(
    r"^\[FRAME_CHECKPOINT\] present=(?P<present>(?:0|[1-9][0-9]*)) "
    r"target=(?P<target>(?:0|[1-9][0-9]*)) "
    r"cumulative_ms=(?P<cumulative_ms>(?:0|[1-9][0-9]*)(?:\.[0-9]+)?) "
    r"cumulative_intervals=(?P<cumulative_intervals>(?:0|[1-9][0-9]*)) "
    r"partial_ms=(?P<partial_ms>(?:0|[1-9][0-9]*)(?:\.[0-9]+)?) "
    r"partial_intervals=(?P<partial_intervals>(?:0|[1-9][0-9]*)) "
    r"terminal=(?P<terminal>(?:0|[1-9][0-9]*))$"
)


class ReportError(Exception):
    """The profile stream, accounting, or lifecycle is not trustworthy."""


@dataclass(frozen=True)
class Snapshot:
    sequence: int
    reason: str
    present: int
    uptime_raw_ns: int
    final: int
    valid: int
    pending_request: int
    signal_owned: int
    snapshot_consistent: int
    accounting_consistent: int
    counters: Mapping[str, int]
    derived: Mapping[str, int]
    checksum: str
    line_number: int


@dataclass(frozen=True)
class FrameWindow:
    present: int
    avg_interval_ms: decimal.Decimal
    avg_latency_ms: decimal.Decimal
    pending: int
    index: int
    arg: int
    line_number: int


@dataclass(frozen=True)
class FrameCheckpoint:
    present: int
    target: int
    cumulative_ms: decimal.Decimal
    cumulative_intervals: int
    partial_ms: decimal.Decimal
    partial_intervals: int
    terminal: int
    line_number: int


@dataclass(frozen=True)
class ProfileLog:
    path: pathlib.Path
    sha256: str
    config: Mapping[str, Any]
    config_line_number: int
    snapshots: Tuple[Snapshot, ...]
    frames: Tuple[FrameWindow, ...]
    checkpoint: FrameCheckpoint

    @property
    def by_sequence(self) -> Mapping[int, Snapshot]:
        return {snapshot.sequence: snapshot for snapshot in self.snapshots}


def _require(condition: bool, source: object, message: str) -> None:
    if not condition:
        raise ReportError(f"{source}: {message}")


def _parse_uint(value: str, source: object, field: str) -> int:
    _require(
        UNSIGNED_RE.fullmatch(value) is not None,
        source,
        f"{field} is not canonical unsigned decimal",
    )
    _require(len(value) <= 20, source, f"{field} exceeds uint64")
    parsed = int(value, 10)
    _require(parsed <= UINT64_MAX, source, f"{field} exceeds uint64")
    return parsed


def _parse_int64(value: str, source: object, field: str) -> int:
    _require(SIGNED_RE.fullmatch(value) is not None, source, f"{field} is not int64")
    parsed = int(value, 10)
    _require(INT64_MIN <= parsed <= INT64_MAX, source, f"{field} exceeds int64")
    return parsed


def _parse_decimal(value: str, source: object, field: str) -> decimal.Decimal:
    _require(DECIMAL_RE.fullmatch(value) is not None, source, f"{field} is not decimal")
    parsed = decimal.Decimal(value)
    _require(parsed.is_finite(), source, f"{field} is not finite")
    return parsed


def _parse_pipeline(line: str, source: object, line_number: int) -> FrameWindow:
    match = PIPELINE_RE.fullmatch(line)
    _require(match is not None, source, "malformed FRAME_PIPELINE present record")
    assert match is not None
    return FrameWindow(
        present=_parse_uint(match.group("present"), source, "present"),
        avg_interval_ms=_parse_decimal(match.group("interval"), source, "avg_interval"),
        avg_latency_ms=_parse_decimal(match.group("latency"), source, "avg_latency"),
        pending=_parse_uint(match.group("pending"), source, "pending"),
        index=_parse_int64(match.group("index"), source, "index"),
        arg=_parse_int64(match.group("arg"), source, "arg"),
        line_number=line_number,
    )


def _parse_checkpoint(line: str, source: object, line_number: int) -> FrameCheckpoint:
    match = CHECKPOINT_RE.fullmatch(line)
    _require(match is not None, source, "malformed FRAME_CHECKPOINT record")
    assert match is not None
    return FrameCheckpoint(
        present=_parse_uint(match.group("present"), source, "present"),
        target=_parse_uint(match.group("target"), source, "target"),
        cumulative_ms=_parse_decimal(match.group("cumulative_ms"), source, "cumulative_ms"),
        cumulative_intervals=_parse_uint(
            match.group("cumulative_intervals"), source, "cumulative_intervals"
        ),
        partial_ms=_parse_decimal(match.group("partial_ms"), source, "partial_ms"),
        partial_intervals=_parse_uint(
            match.group("partial_intervals"), source, "partial_intervals"
        ),
        terminal=_parse_uint(match.group("terminal"), source, "terminal"),
        line_number=line_number,
    )


def _parse_ordered_fields(
    line: str, prefix: str, expected: Sequence[str], source: object
) -> Mapping[str, str]:
    _require(line.startswith(prefix), source, "internal marker prefix mismatch")
    tokens = line[len(prefix) :].split(" ")
    _require(
        len(tokens) == len(expected) and all(tokens),
        source,
        f"expected {len(expected)} ordered fields",
    )
    result: Dict[str, str] = {}
    for index, (token, expected_name) in enumerate(zip(tokens, expected), start=1):
        _require("=" in token, source, f"field {index} lacks '='")
        name, value = token.split("=", 1)
        _require(name == expected_name, source, f"field {index} must be {expected_name}")
        _require(value != "", source, f"field {expected_name} is empty")
        result[name] = value
    return result


def _parse_config(line: str, source: object) -> Mapping[str, Any]:
    raw = _parse_ordered_fields(line, CONFIG_PREFIX, CONFIG_FIELDS, source)
    string_fields = {"signal", "body_attribution", "mixed_body"}
    parsed: Dict[str, Any] = {
        field: raw[field] if field in string_fields else _parse_uint(raw[field], source, field)
        for field in CONFIG_FIELDS
    }
    for field, expected in CONFIG_EXPECTED.items():
        if field in ("clock_floor_ns", "thread_clock_floor_ns"):
            continue
        _require(parsed[field] == expected, source, f"{field} must equal {expected!r}")
    _require(parsed["clock_floor_ns"] > 0, source, "clock_floor_ns must be positive")
    _require(
        parsed["thread_clock_floor_ns"] > 0,
        source,
        "thread_clock_floor_ns must be positive",
    )
    return parsed


def _parse_profile(line: str, source: object, line_number: int) -> Mapping[str, Any]:
    raw = _parse_ordered_fields(line, PROFILE_PREFIX, PROFILE_FIELDS, source)
    _require(REASON_RE.fullmatch(raw["reason"]) is not None, source, "malformed reason")
    _require(CHECKSUM_RE.fullmatch(raw["checksum"]) is not None, source, "malformed checksum")
    parsed: Dict[str, Any] = {
        field: _parse_uint(raw[field], source, field)
        for field in PROFILE_FIELDS
        if field not in ("reason", "checksum")
    }
    parsed["reason"] = raw["reason"]
    parsed["checksum"] = raw["checksum"]
    parsed["line_number"] = line_number
    _require(parsed["version"] == PROFILE_VERSION, source, "unsupported PROFILE version")
    return parsed


def _checksum_mix(checksum: int, value: int) -> int:
    """Match the runtime's byte-wise little-endian FNV-1a value mix."""

    for byte in range(8):
        checksum = ((checksum ^ ((value >> (byte * 8)) & 0xFF)) * FNV_PRIME) & UINT64_MASK
    return checksum


def _snapshot_checksum(profile: Mapping[str, Any]) -> str:
    checksum = FNV_OFFSET
    for field in PROFILE_FIELDS[:-1]:
        value = REASON_CODES.get(profile[field], 0) if field == "reason" else profile[field]
        checksum = _checksum_mix(checksum, value)
    return f"{checksum:016x}"


def _saturating_product(left: int, right: int) -> int:
    return UINT64_MAX if left > UINT64_MAX // right else left * right


def _validate_snapshot_equations(snapshot: Snapshot, source: object) -> None:
    counters = snapshot.counters
    derived = snapshot.derived
    scheduled_accounted = sum(
        counters[field]
        for field in (
            "windows_complete",
            "windows_hle_excluded",
            "windows_reentrant_excluded",
            "windows_dropped",
            "windows_active",
        )
    )
    _require(
        counters["windows_scheduled"] == scheduled_accounted,
        source,
        "window scheduling accounting mismatch",
    )
    _require(
        counters["windows_active"] <= counters["active_owners"],
        source,
        "active windows exceed active owners",
    )
    _require(
        derived["sampled_unflushed_blocks_bound"]
        == _saturating_product(counters["windows_active"], SAMPLE_WINDOW_BLOCKS),
        source,
        "sampled unflushed bound mismatch",
    )
    _require(
        derived["route_unflushed_blocks_bound"]
        == _saturating_product(counters["active_owners"], ROUTE_BATCH_BLOCKS),
        source,
        "route unflushed bound mismatch",
    )
    _require(
        counters["windows_complete"] <= UINT64_MAX // SAMPLE_WINDOW_BLOCKS
        and counters["sampled_blocks"]
        == counters["windows_complete"] * SAMPLE_WINDOW_BLOCKS,
        source,
        "complete-window/sample-block accounting mismatch",
    )
    _require(
        counters["sampled_blocks"] <= UINT64_MAX // 16
        and counters["sampled_steps"] <= counters["sampled_blocks"] * 16,
        source,
        "sampled steps exceed the 16-op FastBlock bound",
    )
    components = sum(counters[field] for field in COMPONENT_COUNTER_FIELDS)
    if counters["saturated"] == 0:
        _require(
            components == counters["sampled_wall_ns"],
            source,
            "component timings do not close sampled_wall_ns exactly",
        )
    else:
        _require(
            components <= counters["sampled_wall_ns"],
            source,
            "saturated component timings exceed sampled_wall_ns",
        )
    _require(
        counters["sampled_thread_cpu_ns"] <= counters["sampled_wall_ns"],
        source,
        "sampled thread CPU exceeds sampled wall time",
    )
    if counters["sampled_blocks"] == 0:
        _require(
            counters["sampled_wall_ns"] == counters["sampled_thread_cpu_ns"] == 0,
            source,
            "zero sampled blocks carry nonzero timing",
        )
    _require(
        counters["lowered_blocks"] + counters["lowering_fail_blocks"]
        == counters["sampled_blocks"],
        source,
        "lowering result accounting mismatch",
    )
    _require(
        sum(counters[field] for field in OP_COUNTER_FIELDS) == counters["sampled_steps"],
        source,
        "operation buckets do not equal sampled_steps",
    )
    _require(
        sum(counters[field] for field in TRANSIENT_COUNTER_FIELDS)
        == counters["route_transient_blocks"],
        source,
        "transient-reason histogram does not equal transient route blocks",
    )
    candidate_blocks = counters["candidate_blocks"]
    _require(candidate_blocks <= counters["lowered_blocks"], source, "candidate blocks exceed lowered blocks")
    _require(
        counters["candidate_steps"]
        == counters["candidate_direct_ops"] + counters["candidate_helper_ops"],
        source,
        "candidate step accounting mismatch",
    )
    _require(
        counters["candidate_helper_ops"] == candidate_blocks,
        source,
        "candidate helper count is not exactly one per block",
    )
    _require(
        candidate_blocks <= UINT64_MAX // 6
        and counters["candidate_direct_ops"] >= candidate_blocks * 6,
        source,
        "candidate direct-op floor is below 6:1",
    )
    _require(
        candidate_blocks <= UINT64_MAX // 16
        and counters["candidate_steps"] <= candidate_blocks * 16,
        source,
        "candidate population exceeds the 16-op FastBlock bound",
    )
    _require(
        counters["candidate_steps"] <= counters["sampled_steps"]
        and counters["candidate_helper_ops"] <= counters["op_register_helper"],
        source,
        "candidate population exceeds sampled operation population",
    )
    _require(
        0 <= derived["candidate_identity_bits"] <= IDENTITY_SKETCH_BITS,
        source,
        "candidate identity sketch popcount exceeds its fixed bound",
    )


def _validate_frame_stream(
    frames: Tuple[FrameWindow, ...],
    checkpoints: Tuple[FrameCheckpoint, ...],
    source: object,
) -> FrameCheckpoint:
    _require(len(checkpoints) == 1, source, "expected exactly one FRAME_CHECKPOINT")
    _require(frames, source, "missing FRAME_PIPELINE records")
    labels = [frame.present for frame in frames]
    _require(len(labels) == len(set(labels)), source, "duplicate FRAME_PIPELINE endpoint")
    _require(labels[0] == 1 and frames[0].avg_interval_ms == ZERO, source, "invalid p1 record")
    for index, frame in enumerate(frames[1:], start=1):
        _require(frame.present == index * WINDOW_SIZE, source, "p120 frame sequence has a gap")
        _require(frame.avg_interval_ms > ZERO, source, "p120 interval is not positive")
    checkpoint = checkpoints[0]
    _require(
        checkpoint.present == checkpoint.target == CHECKPOINT_PRESENT,
        source,
        "checkpoint is not p2300",
    )
    _require(checkpoint.terminal == 0, source, "p2300 checkpoint must be non-terminal")
    _require(
        checkpoint.cumulative_ms > ZERO
        and checkpoint.cumulative_intervals == CHECKPOINT_PRESENT - 1,
        source,
        "checkpoint cumulative accounting mismatch",
    )
    lower = CHECKPOINT_PRESENT - CHECKPOINT_PRESENT % WINDOW_SIZE
    upper = lower + WINDOW_SIZE
    frame_map = {frame.present: frame for frame in frames}
    _require(lower in frame_map and upper in frame_map, source, "checkpoint lacks adjacent p120 records")
    _require(
        frame_map[lower].line_number < checkpoint.line_number < frame_map[upper].line_number,
        source,
        "checkpoint is not between its adjacent p120 records",
    )
    _require(
        checkpoint.partial_intervals == CHECKPOINT_PRESENT - lower
        and ZERO < checkpoint.partial_ms <= checkpoint.cumulative_ms,
        source,
        "checkpoint partial accounting mismatch",
    )
    return checkpoint


def _validate_stream(snapshots: Tuple[Snapshot, ...], source: object) -> None:
    _require(
        len(snapshots) == 7,
        source,
        "expected exactly start, checkpoint, four signals, and final",
    )
    first, checkpoint, final = snapshots[0], snapshots[1], snapshots[-1]
    _require(
        first.reason == "start" and first.present == 0 and first.final == 0,
        source,
        "sequence one must be start",
    )
    _require(
        all(value == 0 for value in first.counters.values())
        and all(value == 0 for value in first.derived.values()),
        source,
        "start counters and derived bounds must be zero",
    )
    _require(
        checkpoint.reason in ("checkpoint", "checkpoint_signal")
        and checkpoint.present == CHECKPOINT_PRESENT
        and checkpoint.final == 0,
        source,
        "sequence two must be the p2300 checkpoint",
    )
    _require(
        final.reason == "final" and final.present == 0 and final.final == 1,
        source,
        "sequence seven must be final",
    )
    previous = snapshots[0]
    for index, snapshot in enumerate(snapshots):
        _require(snapshot.reason in ALLOWED_REASONS, source, "unknown snapshot reason")
        _require(snapshot.uptime_raw_ns > 0, source, "zero snapshot uptime")
        _require(
            snapshot.valid == snapshot.signal_owned == snapshot.snapshot_consistent
            == snapshot.accounting_consistent
            == 1,
            source,
            f"sequence {snapshot.sequence} is not a clean snapshot",
        )
        _require(snapshot.pending_request == 0, source, "snapshot request remains pending")
        _require(snapshot.counters["invalid"] == 0, source, "invalid counter is nonzero")
        _require(snapshot.counters["saturated"] == 0, source, "saturated counter is nonzero")
        _require(snapshot.final == (1 if index == len(snapshots) - 1 else 0), source, "final flag position mismatch")
        if index > 0:
            _require(snapshot.uptime_raw_ns > previous.uptime_raw_ns, source, "snapshot uptime regressed")
            for field in CUMULATIVE_COUNTER_FIELDS:
                current = snapshot.derived[field] if field in DERIVED_FIELDS else snapshot.counters[field]
                prior = previous.derived[field] if field in DERIVED_FIELDS else previous.counters[field]
                _require(current >= prior, source, f"cumulative field {field} regressed")
        previous = snapshot
    for snapshot in snapshots[2:-1]:
        _require(
            snapshot.reason == "signal" and snapshot.present > CHECKPOINT_PRESENT,
            source,
            "intermediate snapshots must be post-p2300 SIGUSR2 receipts",
        )
    for earlier, later in zip(snapshots[:-2], snapshots[1:-1]):
        _require(later.present > earlier.present, source, "non-final present count is not increasing")


def parse_log(path: pathlib.Path) -> ProfileLog:
    _require(path.is_file() and not path.is_symlink(), path, "log is not a regular file")
    digest = hashlib.sha256()
    config: Optional[Mapping[str, Any]] = None
    config_line = 0
    profiles: Dict[int, Mapping[str, Any]] = {}
    marker_order: List[Tuple[str, int]] = []
    frames: List[FrameWindow] = []
    checkpoints: List[FrameCheckpoint] = []
    try:
        with path.open("rb") as stream:
            for line_number, encoded in enumerate(stream, start=1):
                digest.update(encoded)
                if encoded.startswith(FRAME_LIMIT_PREFIX.encode("ascii")):
                    raise ReportError(f"{path}: line {line_number}: FRAME_LIMIT is forbidden")
                relevant = encoded.startswith(
                    (MARKER_PREFIX.encode("ascii"), b"[FRAME_PIPELINE] present", b"[FRAME_CHECKPOINT]")
                )
                if not relevant:
                    continue
                source = f"{path}: line {line_number}"
                _require(encoded.endswith(b"\n"), source, "marker is not newline-terminated")
                _require(len(encoded) <= 32768, source, "marker line is unexpectedly large")
                try:
                    line = encoded[:-1].decode("utf-8", errors="strict")
                except UnicodeError as error:
                    raise ReportError(f"{source}: marker is not UTF-8: {error}") from error
                _require(not line.endswith("\r"), source, "CRLF marker is not canonical")
                if line.startswith(PIPELINE_PREFIX):
                    frames.append(_parse_pipeline(line, source, line_number))
                elif line.startswith(CHECKPOINT_PREFIX):
                    checkpoints.append(_parse_checkpoint(line, source, line_number))
                elif line.startswith(CONFIG_PREFIX):
                    _require(config is None, source, "duplicate CONFIG marker")
                    config = _parse_config(line, source)
                    config_line = line_number
                    marker_order.append(("config", 0))
                elif line.startswith(PROFILE_PREFIX):
                    profile = _parse_profile(line, source, line_number)
                    sequence = profile["sequence"]
                    _require(sequence not in profiles, source, "duplicate PROFILE sequence")
                    profiles[sequence] = profile
                    marker_order.append(("profile", sequence))
                elif line.startswith(MARKER_PREFIX):
                    raise ReportError(f"{source}: unknown phase-6CM marker")
    except OSError as error:
        raise ReportError(f"{path}: cannot read log: {error}") from error

    _require(config is not None, path, "expected exactly one CONFIG marker")
    _require(sorted(profiles) == list(range(1, len(profiles) + 1)), path, "PROFILE sequence gap")
    expected_order = [("config", 0)] + [("profile", sequence) for sequence in sorted(profiles)]
    _require(marker_order == expected_order, path, "CONFIG/PROFILE marker order mismatch")
    snapshots: List[Snapshot] = []
    for sequence in sorted(profiles):
        profile = profiles[sequence]
        expected_checksum = _snapshot_checksum(profile)
        _require(
            profile["checksum"] == expected_checksum,
            path,
            f"sequence {sequence} checksum mismatch: expected {expected_checksum}",
        )
        snapshot = Snapshot(
            sequence=sequence,
            reason=profile["reason"],
            present=profile["present"],
            uptime_raw_ns=profile["uptime_raw_ns"],
            final=profile["final"],
            valid=profile["valid"],
            pending_request=profile["pending_request"],
            signal_owned=profile["signal_owned"],
            snapshot_consistent=profile["snapshot_consistent"],
            accounting_consistent=profile["accounting_consistent"],
            counters={field: profile[field] for field in COUNTER_FIELDS},
            derived={field: profile[field] for field in DERIVED_FIELDS},
            checksum=profile["checksum"],
            line_number=profile["line_number"],
        )
        _validate_snapshot_equations(snapshot, f"{path}: sequence {sequence}")
        snapshots.append(snapshot)
    frozen = tuple(snapshots)
    _validate_stream(frozen, path)
    checkpoint = _validate_frame_stream(tuple(frames), tuple(checkpoints), path)
    _require(config_line < frozen[0].line_number, path, "CONFIG does not precede START")
    _require(
        checkpoint.line_number < frozen[1].line_number,
        path,
        "FRAME_CHECKPOINT must precede the checkpoint PROFILE",
    )
    return ProfileLog(
        path=path.resolve(),
        sha256=digest.hexdigest(),
        config=config,
        config_line_number=config_line,
        snapshots=frozen,
        frames=tuple(frames),
        checkpoint=checkpoint,
    )


def _ratio_decimal(numerator: int, denominator: int, scale: int = 100) -> Optional[str]:
    if denominator == 0:
        return None
    with decimal.localcontext() as context:
        context.prec = 60
        value = decimal.Decimal(numerator) * decimal.Decimal(scale) / decimal.Decimal(denominator)
    return format(value.quantize(decimal.Decimal("0.000001")), "f")


def _basis_points(numerator: int, denominator: int) -> Optional[str]:
    return _ratio_decimal(numerator, denominator, 10000)


def _counter_delta(start: Snapshot, end: Snapshot) -> Mapping[str, int]:
    return {
        field: end.counters[field] - start.counters[field]
        for field in COUNTER_FIELDS
        if field not in ("windows_active", "active_owners")
    }


def _segment_accounting(
    name: str, start: Snapshot, end: Snapshot, counters: Mapping[str, int]
) -> None:
    _require(
        counters["windows_scheduled"]
        == counters["windows_complete"]
        + counters["windows_hle_excluded"]
        + counters["windows_reentrant_excluded"]
        + counters["windows_dropped"]
        + end.counters["windows_active"]
        - start.counters["windows_active"],
        name,
        "delta window scheduling accounting mismatch",
    )
    _require(
        counters["windows_complete"] <= UINT64_MAX // SAMPLE_WINDOW_BLOCKS
        and counters["sampled_blocks"]
        == counters["windows_complete"] * SAMPLE_WINDOW_BLOCKS,
        name,
        "delta complete-window/sample-block accounting mismatch",
    )
    _require(
        counters["sampled_blocks"] <= UINT64_MAX // 16
        and counters["sampled_steps"] <= counters["sampled_blocks"] * 16,
        name,
        "delta sampled steps exceed FastBlock bound",
    )
    _require(
        sum(counters[field] for field in COMPONENT_COUNTER_FIELDS)
        == counters["sampled_wall_ns"],
        name,
        "delta component timings do not close sampled wall exactly",
    )
    _require(
        counters["sampled_thread_cpu_ns"] <= counters["sampled_wall_ns"],
        name,
        "delta thread CPU exceeds sampled wall",
    )
    _require(
        counters["lowered_blocks"] + counters["lowering_fail_blocks"]
        == counters["sampled_blocks"],
        name,
        "delta lowering accounting mismatch",
    )
    _require(
        sum(counters[field] for field in OP_COUNTER_FIELDS) == counters["sampled_steps"],
        name,
        "delta operation accounting mismatch",
    )
    _require(
        sum(counters[field] for field in TRANSIENT_COUNTER_FIELDS)
        == counters["route_transient_blocks"],
        name,
        "delta transient histogram mismatch",
    )
    candidate_blocks = counters["candidate_blocks"]
    _require(candidate_blocks <= counters["lowered_blocks"], name, "delta candidate exceeds lowered")
    _require(
        counters["candidate_steps"]
        == counters["candidate_direct_ops"] + counters["candidate_helper_ops"]
        and counters["candidate_helper_ops"] == candidate_blocks,
        name,
        "delta candidate accounting mismatch",
    )
    _require(
        candidate_blocks <= UINT64_MAX // 16
        and candidate_blocks * 7 <= counters["candidate_steps"]
        <= candidate_blocks * 16,
        name,
        "delta candidate is outside the preregistered 6+1..16-op shape",
    )


def _dominant(values: Mapping[str, int]) -> Optional[str]:
    if not values:
        return None
    maximum = max(values.values())
    winners = [field for field, value in values.items() if value == maximum]
    return winners[0] if maximum > 0 and len(winners) == 1 else None


def _candidate_material(
    counters: Mapping[str, int],
    config: Mapping[str, Any],
    start_unflushed_blocks_bound: int,
    end_unflushed_blocks_bound: int,
) -> Mapping[str, Any]:
    candidate_blocks = counters["candidate_blocks"]
    candidate_steps = counters["candidate_steps"]
    sampled_blocks = counters["sampled_blocks"]
    sampled_steps = counters["sampled_steps"]
    _require(
        start_unflushed_blocks_bound <= UINT64_MAX // 16
        and end_unflushed_blocks_bound <= UINT64_MAX // 16,
        "candidate materiality",
        "endpoint step uncertainty bound overflows uint64",
    )
    start_step_uncertainty = start_unflushed_blocks_bound * 16
    end_step_uncertainty = end_unflushed_blocks_bound * 16
    _require(
        sampled_blocks <= UINT64_MAX - end_unflushed_blocks_bound
        and sampled_steps <= UINT64_MAX - end_step_uncertainty,
        "candidate materiality",
        "conservative sampled-population upper bound overflows uint64",
    )
    candidate_blocks_lower = max(
        candidate_blocks - start_unflushed_blocks_bound, 0
    )
    sampled_blocks_upper = sampled_blocks + end_unflushed_blocks_bound
    candidate_steps_lower = max(candidate_steps - start_step_uncertainty, 0)
    sampled_steps_upper = sampled_steps + end_step_uncertainty

    raw_sample_pass = candidate_blocks >= config["material_min_samples"]
    raw_block_pass = (
        sampled_blocks > 0
        and candidate_blocks * 10000
        >= config["material_min_block_bp"] * sampled_blocks
    )
    raw_step_pass = (
        sampled_steps > 0
        and candidate_steps * 10000
        >= config["material_min_step_bp"] * sampled_steps
    )
    sample_pass = candidate_blocks_lower >= config["material_min_samples"]
    block_pass = (
        sampled_blocks_upper > 0
        and candidate_blocks_lower * 10000
        >= config["material_min_block_bp"] * sampled_blocks_upper
    )
    step_pass = (
        sampled_steps_upper > 0
        and candidate_steps_lower * 10000
        >= config["material_min_step_bp"] * sampled_steps_upper
    )
    robust_pass = sample_pass and block_pass and step_pass
    return {
        "candidate_blocks": candidate_blocks,
        "candidate_steps": candidate_steps,
        "sampled_blocks": sampled_blocks,
        "sampled_steps": sampled_steps,
        "raw_candidate_block_share_basis_points": _basis_points(
            candidate_blocks, sampled_blocks
        ),
        "raw_candidate_step_share_basis_points": _basis_points(
            candidate_steps, sampled_steps
        ),
        "candidate_block_share_basis_points": _basis_points(
            candidate_blocks_lower, sampled_blocks_upper
        ),
        "candidate_step_share_basis_points": _basis_points(
            candidate_steps_lower, sampled_steps_upper
        ),
        "endpoint_uncertainty": {
            "start_unflushed_blocks_bound": start_unflushed_blocks_bound,
            "end_unflushed_blocks_bound": end_unflushed_blocks_bound,
            "start_unflushed_steps_bound": start_step_uncertainty,
            "end_unflushed_steps_bound": end_step_uncertainty,
        },
        "conservative_intervals": {
            "candidate_blocks": {"lower": candidate_blocks_lower, "raw": candidate_blocks},
            "sampled_blocks": {"raw": sampled_blocks, "upper": sampled_blocks_upper},
            "candidate_steps": {"lower": candidate_steps_lower, "raw": candidate_steps},
            "sampled_steps": {"raw": sampled_steps, "upper": sampled_steps_upper},
        },
        "minimum_candidate_samples": config["material_min_samples"],
        "minimum_block_share_basis_points": config["material_min_block_bp"],
        "minimum_step_share_basis_points": config["material_min_step_bp"],
        "raw_sample_threshold_pass": raw_sample_pass,
        "raw_block_share_threshold_pass": raw_block_pass,
        "raw_step_share_threshold_pass": raw_step_pass,
        "raw_all_three_thresholds_pass": (
            raw_sample_pass and raw_block_pass and raw_step_pass
        ),
        "sample_threshold_pass": sample_pass,
        "block_share_threshold_pass": block_pass,
        "step_share_threshold_pass": step_pass,
        "uncertainty_robust_all_three_pass": robust_pass,
        "all_three_thresholds_pass": robust_pass,
        "gate_semantics": (
            "candidate numerators subtract the start bound; sampled denominators "
            "add the end bound; any endpoint uncertainty that can change a threshold "
            "forces NO_ELECTION"
        ),
    }


def _segment(
    name: str, start: Snapshot, end: Snapshot, config: Mapping[str, Any]
) -> Mapping[str, Any]:
    _require(end.sequence > start.sequence, name, "end sequence must follow start")
    elapsed_ns = end.uptime_raw_ns - start.uptime_raw_ns
    _require(elapsed_ns > 0, name, "boundary elapsed time must be positive")
    counters = _counter_delta(start, end)
    _segment_accounting(name, start, end, counters)
    components = {field: counters[field] for field in COMPONENT_COUNTER_FIELDS}
    operations = {field: counters[field] for field in OP_COUNTER_FIELDS}
    route_blocks = sum(
        counters[field]
        for field in ("route_stable_blocks", "route_first_negative_blocks", "route_transient_blocks")
    )
    route_steps = sum(
        counters[field]
        for field in ("route_stable_steps", "route_first_negative_steps", "route_transient_steps")
    )
    sampled_blocks = counters["sampled_blocks"]
    wall_floor = config["clock_floor_ns"]
    cpu_floor = config["thread_clock_floor_ns"]
    _require(
        sampled_blocks <= UINT64_MAX // wall_floor
        and sampled_blocks <= UINT64_MAX // cpu_floor,
        name,
        "calibrated per-block clock-floor product overflows uint64",
    )
    wall_overhead_lower_bound = sampled_blocks * wall_floor
    cpu_overhead_lower_bound = sampled_blocks * cpu_floor
    _require(
        counters["sampled_wall_ns"] >= wall_overhead_lower_bound
        and counters["sampled_thread_cpu_ns"] >= cpu_overhead_lower_bound,
        name,
        "raw sampled timing is below its calibrated per-block clock-floor lower bound",
    )
    wall_net_upper_bound = counters["sampled_wall_ns"] - wall_overhead_lower_bound
    cpu_net_upper_bound = counters["sampled_thread_cpu_ns"] - cpu_overhead_lower_bound
    return {
        "name": name,
        "start": {
            "sequence": start.sequence,
            "reason": start.reason,
            "present": start.present,
            "uptime_raw_ns": start.uptime_raw_ns,
        },
        "end": {
            "sequence": end.sequence,
            "reason": end.reason,
            "present": end.present,
            "uptime_raw_ns": end.uptime_raw_ns,
        },
        "elapsed_ns": elapsed_ns,
        "counters": counters,
        "endpoint_gauges": {
            "windows_active": {
                "start": start.counters["windows_active"],
                "end": end.counters["windows_active"],
                "net_change": end.counters["windows_active"] - start.counters["windows_active"],
            },
            "active_owners": {
                "start": start.counters["active_owners"],
                "end": end.counters["active_owners"],
                "net_change": end.counters["active_owners"] - start.counters["active_owners"],
            },
            "sampled_unflushed_blocks_bound": {
                "start": start.derived["sampled_unflushed_blocks_bound"],
                "end": end.derived["sampled_unflushed_blocks_bound"],
            },
            "route_unflushed_blocks_bound": {
                "start": start.derived["route_unflushed_blocks_bound"],
                "end": end.derived["route_unflushed_blocks_bound"],
            },
        },
        "route_accounting": {
            "total_flushed_blocks": route_blocks,
            "total_flushed_steps": route_steps,
            "stable_negative": {
                "blocks": counters["route_stable_blocks"],
                "steps": counters["route_stable_steps"],
                "block_share_percent": _ratio_decimal(counters["route_stable_blocks"], route_blocks),
                "step_share_percent": _ratio_decimal(counters["route_stable_steps"], route_steps),
            },
            "first_negative": {
                "blocks": counters["route_first_negative_blocks"],
                "steps": counters["route_first_negative_steps"],
                "block_share_percent": _ratio_decimal(counters["route_first_negative_blocks"], route_blocks),
                "step_share_percent": _ratio_decimal(counters["route_first_negative_steps"], route_steps),
            },
            "transient": {
                "blocks": counters["route_transient_blocks"],
                "steps": counters["route_transient_steps"],
                "block_share_percent": _ratio_decimal(counters["route_transient_blocks"], route_blocks),
                "step_share_percent": _ratio_decimal(counters["route_transient_steps"], route_steps),
                "reason_histogram": {field: counters[field] for field in TRANSIENT_COUNTER_FIELDS},
            },
            "uncertainty": "flushed route totals; endpoint bounds cover registered-owner batches not yet flushed",
        },
        "window_accounting": {
            "scheduled": counters["windows_scheduled"],
            "complete": counters["windows_complete"],
            "hle_excluded": counters["windows_hle_excluded"],
            "reentrant_excluded": counters["windows_reentrant_excluded"],
            "dropped": counters["windows_dropped"],
            "active_net_change": end.counters["windows_active"] - start.counters["windows_active"],
            "equation": "scheduled=complete+hle_excluded+reentrant_excluded+dropped+active_end-active_start",
        },
        "sampled_cost": {
            "blocks": counters["sampled_blocks"],
            "steps": counters["sampled_steps"],
            "wall_ns": counters["sampled_wall_ns"],
            "thread_cpu_ns": counters["sampled_thread_cpu_ns"],
            "thread_cpu_over_wall_percent": _ratio_decimal(
                counters["sampled_thread_cpu_ns"], counters["sampled_wall_ns"]
            ),
            "sampled_wall_over_boundary_wall_percent": _ratio_decimal(
                counters["sampled_wall_ns"], elapsed_ns
            ),
            "calibrated_overhead_lower_bound_ns": {
                "wall": wall_overhead_lower_bound,
                "thread_cpu": cpu_overhead_lower_bound,
            },
            "net_cost_upper_bound_ns": {
                "wall": wall_net_upper_bound,
                "thread_cpu": cpu_net_upper_bound,
            },
            "ratio_of_net_cost_upper_bounds_percent": _ratio_decimal(
                cpu_net_upper_bound, wall_net_upper_bound
            ),
            "clock_floor_semantics": (
                "raw counters remain authoritative; per-block calibrated floors "
                "are exposed separately as lower-bound overhead and are not used "
                "by the count-based phase6CN materiality gate"
            ),
        },
        "components": {
            "timings_ns": components,
            "share_of_sampled_wall_percent": {
                field: _ratio_decimal(value, counters["sampled_wall_ns"])
                for field, value in components.items()
            },
            "dominant_bucket": _dominant(components),
            "accounting": "exact sum equals sampled_wall_ns; component_residual_ns closes the sum",
            "body_attribution": config["body_attribution"],
            "mixed_body": config["mixed_body"],
        },
        "operations": {
            "buckets": operations,
            "total": sum(operations.values()),
            "dominant_bucket": _dominant(operations),
            "accounting": "exact sum equals sampled_steps",
        },
        "lowering": {
            "lowered_blocks": counters["lowered_blocks"],
            "lowering_fail_blocks": counters["lowering_fail_blocks"],
        },
        "candidate": {
            **_candidate_material(
                counters,
                config,
                start.derived["sampled_unflushed_blocks_bound"],
                end.derived["sampled_unflushed_blocks_bound"],
            ),
            "direct_ops": counters["candidate_direct_ops"],
            "helper_ops": counters["candidate_helper_ops"],
            "identity_bits_start": start.derived["candidate_identity_bits"],
            "identity_bits_end": end.derived["candidate_identity_bits"],
            "identity_bits_added": end.derived["candidate_identity_bits"]
            - start.derived["candidate_identity_bits"],
            "shape": "exactly one typed register-only helper, at least six production-direct ops",
        },
    }


def _snapshot_json(snapshot: Snapshot) -> Mapping[str, Any]:
    return {
        "sequence": snapshot.sequence,
        "reason": snapshot.reason,
        "present": snapshot.present,
        "uptime_raw_ns": snapshot.uptime_raw_ns,
        "final": snapshot.final,
        "valid": snapshot.valid,
        "pending_request": snapshot.pending_request,
        "signal_owned": snapshot.signal_owned,
        "snapshot_consistent": snapshot.snapshot_consistent,
        "accounting_consistent": snapshot.accounting_consistent,
        "counters": snapshot.counters,
        "derived": snapshot.derived,
        "checksum": snapshot.checksum,
        "line_number": snapshot.line_number,
    }


def _exact_mapping(value: object, expected: Sequence[str], source: object) -> Mapping[str, Any]:
    _require(type(value) is dict, source, "expected an object")
    assert isinstance(value, dict)
    _require(set(value) == set(expected), source, "object keys do not match schema")
    return value


def _binding_uint(value: object, source: object, field: str) -> int:
    _require(type(value) is int and 0 <= value <= UINT64_MAX, source, f"{field} must be uint64")
    assert isinstance(value, int)
    return value


def _binding_bool(value: object, source: object, field: str) -> bool:
    _require(type(value) is bool, source, f"{field} must be boolean")
    assert isinstance(value, bool)
    return value


def _validate_bound_stage(
    profile: ProfileLog, scene: str, stage: str, value: object, expected_sequence: int
) -> Mapping[str, Any]:
    source = f"live binding {scene}/{stage}"
    stage_value = _exact_mapping(
        value,
        (
            "receipt_uptime_raw_ns",
            "signal_sent_uptime_raw_ns",
            "signal_observed_uptime_raw_ns",
            "sequence",
            "snapshot_present",
            "snapshot_uptime_raw_ns",
            "snapshot_checksum",
            "evidence_verified",
        ),
        source,
    )
    receipt = _binding_uint(stage_value["receipt_uptime_raw_ns"], source, "receipt")
    sent = _binding_uint(stage_value["signal_sent_uptime_raw_ns"], source, "signal_sent")
    observed = _binding_uint(stage_value["signal_observed_uptime_raw_ns"], source, "signal_observed")
    sequence = _binding_uint(stage_value["sequence"], source, "sequence")
    snapshot_present = _binding_uint(stage_value["snapshot_present"], source, "snapshot_present")
    snapshot_uptime = _binding_uint(stage_value["snapshot_uptime_raw_ns"], source, "snapshot_uptime")
    checksum = stage_value["snapshot_checksum"]
    _require(sequence == expected_sequence, source, "unexpected bound sequence")
    _require(type(checksum) is str and CHECKSUM_RE.fullmatch(checksum) is not None, source, "bad checksum")
    snapshot = profile.by_sequence.get(sequence)
    _require(snapshot is not None and snapshot.reason == "signal", source, "bound snapshot is not signal")
    assert snapshot is not None
    _require(
        snapshot.present == snapshot_present
        and snapshot.uptime_raw_ns == snapshot_uptime
        and snapshot.checksum == checksum,
        source,
        "bound signal identity differs from log",
    )
    _require(0 < receipt <= sent <= snapshot_uptime <= observed, source, "signal clocks are unordered")
    _require(_binding_bool(stage_value["evidence_verified"], source, "evidence_verified"), source, "evidence false")
    return stage_value


def _validate_live_binding(
    profile: ProfileLog,
    fort_sequences: Tuple[int, int],
    in_game_sequences: Tuple[int, int],
    binding: object,
) -> Mapping[str, Any]:
    root = _exact_mapping(binding, ("schema", "scope", "checkpoint", "scenes", "lifecycle"), "live binding")
    _require(root["schema"] == LIVE_BINDING_SCHEMA, "live binding", "schema mismatch")
    _require(root["scope"] == DIAGNOSTIC_SCOPE, "live binding", "scope mismatch")
    checkpoint = _exact_mapping(root["checkpoint"], ("count", "terminal", "observed_uptime_raw_ns"), "binding checkpoint")
    _require(
        _binding_uint(checkpoint["count"], "binding checkpoint", "count") == 1
        and _binding_uint(checkpoint["terminal"], "binding checkpoint", "terminal") == 0,
        "binding checkpoint",
        "p2300 is not exact-once and non-terminal",
    )
    checkpoint_observed = _binding_uint(
        checkpoint["observed_uptime_raw_ns"], "binding checkpoint", "observed_uptime_raw_ns"
    )
    scenes = _exact_mapping(root["scenes"], ("fort", "in_game"), "binding scenes")
    expected_pairs = {"fort": fort_sequences, "in_game": in_game_sequences}
    frame_map = {frame.present: frame for frame in profile.frames}
    bound_scenes: Dict[str, Mapping[str, Any]] = {}
    previous_observed = checkpoint_observed
    for scene in ("fort", "in_game"):
        source = f"live binding {scene}"
        scene_value = _exact_mapping(
            scenes[scene],
            (
                "anchor",
                "complete",
                "endpoint_before",
                "endpoint_after",
                "discard_endpoint",
                "measured_endpoint",
                "avg_interval_ms",
                "manual_input_only_attested",
                "zero_input_window_attested",
                "oracle_attested",
            ),
            source,
        )
        start_sequence, end_sequence = expected_pairs[scene]
        anchor = _validate_bound_stage(profile, scene, "anchor", scene_value["anchor"], start_sequence)
        complete = _validate_bound_stage(profile, scene, "complete", scene_value["complete"], end_sequence)
        before = _binding_uint(scene_value["endpoint_before"], source, "endpoint_before")
        after = _binding_uint(scene_value["endpoint_after"], source, "endpoint_after")
        discard = _binding_uint(scene_value["discard_endpoint"], source, "discard_endpoint")
        measured = _binding_uint(scene_value["measured_endpoint"], source, "measured_endpoint")
        _require(before == after and before > CHECKPOINT_PRESENT and before % WINDOW_SIZE == 0, source, "unstable L")
        _require(discard == before + WINDOW_SIZE and measured == discard + WINDOW_SIZE, source, "L/L+120/L+240 geometry mismatch")
        _require(discard in frame_map and measured in frame_map, source, "scene p120 records missing")
        interval_text = scene_value["avg_interval_ms"]
        _require(type(interval_text) is str, source, "avg_interval_ms must be a string")
        interval = _parse_decimal(interval_text, source, "avg_interval_ms")
        _require(interval > ZERO and interval == frame_map[measured].avg_interval_ms, source, "interval differs from raw p120")
        start_snapshot = profile.by_sequence[start_sequence]
        end_snapshot = profile.by_sequence[end_sequence]
        _require(start_snapshot.present <= measured <= end_snapshot.present, source, "signals do not bracket measurement")
        _require(
            previous_observed < anchor["receipt_uptime_raw_ns"]
            <= anchor["signal_observed_uptime_raw_ns"]
            < complete["receipt_uptime_raw_ns"]
            <= complete["signal_observed_uptime_raw_ns"],
            source,
            "scene receipts are unordered",
        )
        previous_observed = complete["signal_observed_uptime_raw_ns"]
        for field in ("manual_input_only_attested", "zero_input_window_attested", "oracle_attested"):
            _require(_binding_bool(scene_value[field], source, field), source, f"{field} is false")
        with decimal.localcontext() as context:
            context.prec = 50
            fps = ONE_THOUSAND / interval
        bound_scenes[scene] = {
            **scene_value,
            "derived_fps": format(fps.quantize(decimal.Decimal("0.000001")), "f"),
            "semantics": "L stable; L+120 discarded; L+240 first complete p120 after announcement",
        }
    lifecycle = _exact_mapping(
        root["lifecycle"],
        (
            "exit_status_bytes",
            "sdl_quit_count",
            "fatal_count",
            "assert_count",
            "dynarec_unhealthy_count",
            "descriptor_mismatch_count",
            "device_lost_count",
            "frame_limit_count",
            "signal_snapshot_count",
            "final_sequence",
            "final_clean",
            "no_input_before_p2300_attested",
            "no_glitch_attested",
            "evidence_verified",
        ),
        "binding lifecycle",
    )
    _require(lifecycle["exit_status_bytes"] == "0\n", "binding lifecycle", "exit is not byte-exact 0\\n")
    expected_counts = {
        "sdl_quit_count": 1,
        "fatal_count": 0,
        "assert_count": 0,
        "dynarec_unhealthy_count": 0,
        "descriptor_mismatch_count": 0,
        "device_lost_count": 0,
        "frame_limit_count": 0,
        "signal_snapshot_count": 4,
        "final_sequence": 7,
    }
    for field, expected in expected_counts.items():
        _require(_binding_uint(lifecycle[field], "binding lifecycle", field) == expected, "binding lifecycle", f"{field} mismatch")
    for field in ("final_clean", "no_input_before_p2300_attested", "no_glitch_attested", "evidence_verified"):
        _require(_binding_bool(lifecycle[field], "binding lifecycle", field), "binding lifecycle", f"{field} false")
    _require(previous_observed < profile.snapshots[-1].uptime_raw_ns, "binding lifecycle", "final precedes last receipt")
    return {
        "schema": LIVE_BINDING_SCHEMA,
        "scope": DIAGNOSTIC_SCOPE,
        "checkpoint": checkpoint,
        "scenes": bound_scenes,
        "lifecycle": lifecycle,
    }


def _election(
    segments: Mapping[str, Mapping[str, Any]], *, lifecycle_bound: bool
) -> Mapping[str, Any]:
    fort = segments["fort"]
    in_game = segments["in_game"]
    fort_bucket = fort["components"]["dominant_bucket"]
    game_bucket = in_game["components"]["dominant_bucket"]
    observed_common_bucket = (
        fort_bucket
        if fort_bucket is not None and fort_bucket == game_bucket
        else None
    )
    if not lifecycle_bound:
        return {
            "status": "NO_ELECTION",
            "elected": None,
            "phase6cn_material": False,
            "observed_common_dominant_bucket": observed_common_bucket,
            "speedup_claim": False,
            "speedup_estimate": None,
            "reason": (
                "the offline profile is not bound to a complete live lifecycle; "
                "materiality remains descriptive until seal validation"
            ),
        }
    if (
        fort["candidate"]["all_three_thresholds_pass"]
        and in_game["candidate"]["all_three_thresholds_pass"]
    ):
        return {
            "status": "PHASE6CN_MATERIAL",
            "elected": "single_register_helper_mixed_block_admission",
            "phase6cn_material": True,
            "observed_common_dominant_bucket": observed_common_bucket,
            "speedup_claim": False,
            "speedup_estimate": None,
            "reason": "all three preregistered materiality thresholds pass independently in Fort and in-game",
        }
    return {
        "status": "NO_ELECTION",
        "elected": None,
        "phase6cn_material": False,
        "observed_common_dominant_bucket": observed_common_bucket,
        "speedup_claim": False,
        "speedup_estimate": None,
        "reason": (
            "at least one preregistered phase6CN threshold failed in Fort or in-game; "
            "timing buckets remain descriptive because no alternative election threshold was preregistered"
        ),
    }


def build_report(
    profile: ProfileLog,
    fort_sequences: Tuple[int, int],
    in_game_sequences: Tuple[int, int],
    live_binding: Optional[Mapping[str, Any]] = None,
) -> Mapping[str, Any]:
    snapshots = profile.by_sequence
    first = profile.snapshots[0]
    checkpoint = profile.snapshots[1]
    final = profile.snapshots[-1]
    for name, pair in (("fort", fort_sequences), ("in_game", in_game_sequences)):
        _require(pair[0] in snapshots and pair[1] in snapshots, name, "unknown boundary sequence")
        _require(pair[0] < pair[1], name, "start must precede end")
        for sequence in pair:
            _require(snapshots[sequence].reason == "signal", name, "boundary is not SIGUSR2")
    _require(
        checkpoint.sequence
        < fort_sequences[0]
        < fort_sequences[1]
        < in_game_sequences[0]
        < in_game_sequences[1]
        < final.sequence,
        "boundaries",
        "expected checkpoint < Fort anchor/complete < in-game anchor/complete < final",
    )
    segments = {
        "loading": _segment("loading", first, checkpoint, profile.config),
        "fort": _segment(
            "fort", snapshots[fort_sequences[0]], snapshots[fort_sequences[1]], profile.config
        ),
        "in_game": _segment(
            "in_game",
            snapshots[in_game_sequences[0]],
            snapshots[in_game_sequences[1]],
            profile.config,
        ),
    }
    full_run = _segment("full_run", first, final, profile.config)
    validated_binding = (
        _validate_live_binding(profile, fort_sequences, in_game_sequences, live_binding)
        if live_binding is not None
        else None
    )
    decision = _election(segments, lifecycle_bound=validated_binding is not None)
    diagnostic_gate = (
        {
            "passed": False,
            "status": "LIFECYCLE_UNBOUND",
            "scope": DIAGNOSTIC_SCOPE,
            "persistent_baseline_eligible": False,
            "loading_metric_eligible": False,
            "interpretation": "offline arithmetic only; seal with phase6cm_profile_live.py to bind evidence and lifecycle",
        }
        if validated_binding is None
        else {
            "passed": True,
            "status": "PASS_DIAGNOSTIC_ONLY",
            "scope": DIAGNOSTIC_SCOPE,
            "persistent_baseline_eligible": False,
            "loading_metric_eligible": False,
            "interpretation": "clean lifecycle-bound phase-6CM evidence; not a baseline or promotion record",
        }
    )
    frame_checkpoint = profile.checkpoint
    return {
        "schema": {"name": REPORT_SCHEMA_NAME, "version": REPORT_SCHEMA_VERSION},
        "valid": True,
        "scope": DIAGNOSTIC_SCOPE,
        "persistent_baseline_eligible": False,
        "loading_metric_eligible": False,
        "source": {"log": str(profile.path), "sha256": profile.sha256},
        "profile_schema_version": PROFILE_VERSION,
        "config": profile.config,
        "frame_trace": {
            "frame_limit_absent": True,
            "present_record_count": len(profile.frames),
            "checkpoint": {
                "present": frame_checkpoint.present,
                "target": frame_checkpoint.target,
                "cumulative_ms": format(frame_checkpoint.cumulative_ms, "f"),
                "cumulative_intervals": frame_checkpoint.cumulative_intervals,
                "partial_ms": format(frame_checkpoint.partial_ms, "f"),
                "partial_intervals": frame_checkpoint.partial_intervals,
                "terminal": frame_checkpoint.terminal,
                "line_number": frame_checkpoint.line_number,
            },
        },
        "boundaries": {
            "loading": {
                "start_sequence": first.sequence,
                "end_sequence": checkpoint.sequence,
                "semantics": "start -> checkpoint(p2300 terminal=0)",
            },
            "fort": {
                "start_sequence": fort_sequences[0],
                "end_sequence": fort_sequences[1],
                "semantics": "manual anchor SIGUSR2 -> complete SIGUSR2",
            },
            "in_game": {
                "start_sequence": in_game_sequences[0],
                "end_sequence": in_game_sequences[1],
                "semantics": "manual anchor SIGUSR2 -> complete SIGUSR2",
            },
        },
        "snapshots": [_snapshot_json(snapshot) for snapshot in profile.snapshots],
        "segments": segments,
        "full_run": full_run,
        "decision": decision,
        "live_binding": validated_binding,
        "diagnostic_gate": diagnostic_gate,
        "limitations": [
            "one diagnostic run; no B/C performance comparison",
            "sampled timing and route counters include explicit endpoint unflushed bounds",
            "an elected target is not a measured or predicted speedup",
        ],
    }


def _value_or_na(value: Optional[str]) -> str:
    return value if value is not None else "n/a"


def _render_table(report: Mapping[str, Any]) -> str:
    gate = report["diagnostic_gate"]
    lines = [
        (
            "PHASE-6CM DIAGNOSTIC GATE: PASS (DIAGNOSTIC ONLY; NOT PERSISTENT BASELINE)"
            if gate["passed"]
            else "PHASE-6CM PROFILE REPORT: PASS (OFFLINE ONLY; LIFECYCLE UNBOUND)"
        ),
        f"log={report['source']['log']}",
        f"sha256={report['source']['sha256']}",
        "",
        "SEGMENT DELTAS",
        "segment      seq       sampled blocks/steps   sampled wall ns  cpu/wall   candidate block/step bp  dominant",
    ]
    for name in ("loading", "fort", "in_game"):
        segment = report["segments"][name]
        sample = segment["sampled_cost"]
        candidate = segment["candidate"]
        lines.append(
            f"{name:<11} {segment['start']['sequence']:>3}->{segment['end']['sequence']:<3} "
            f"{sample['blocks']:>10}/{sample['steps']:<10} "
            f"{sample['wall_ns']:>15} "
            f"{_value_or_na(sample['thread_cpu_over_wall_percent']):>8}% "
            f"{_value_or_na(candidate['candidate_block_share_basis_points']):>9}/"
            f"{_value_or_na(candidate['candidate_step_share_basis_points']):<9} "
            f"{segment['components']['dominant_bucket'] or 'tie/none'}"
        )
        route = segment["route_accounting"]
        bounds = segment["endpoint_gauges"]
        lines.append(
            " " * 12
            + f"routes stable/first/transient={route['stable_negative']['blocks']}/"
            + f"{route['first_negative']['blocks']}/{route['transient']['blocks']} "
            + "unflushed(sample route)="
            + f"{bounds['sampled_unflushed_blocks_bound']['start']}->{bounds['sampled_unflushed_blocks_bound']['end']} "
            + f"{bounds['route_unflushed_blocks_bound']['start']}->{bounds['route_unflushed_blocks_bound']['end']}"
        )
        if gate["passed"] and name in ("fort", "in_game"):
            bound = report["live_binding"]["scenes"][name]
            lines.append(
                " " * 12
                + f"L=p{bound['endpoint_before']} discard=p{bound['discard_endpoint']} "
                + f"measure=p{bound['measured_endpoint']} interval={bound['avg_interval_ms']}ms "
                + f"fps={bound['derived_fps']}"
            )
    decision = report["decision"]
    lines.extend(
        [
            "",
            "ELECTION (NO SPEEDUP CLAIM)",
            f"status={decision['status']} elected={decision['elected'] or 'none'}",
            decision["reason"],
        ]
    )
    return "\n".join(lines)


def _serialize_report(report: Mapping[str, Any]) -> bytes:
    try:
        return (
            json.dumps(
                report,
                indent=2,
                sort_keys=True,
                ensure_ascii=False,
                allow_nan=False,
            )
            + "\n"
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise ReportError(f"report is not strict JSON: {error}") from error


def _publish_report_exclusive(output: pathlib.Path, payload: bytes) -> None:
    _require(output.parent.is_dir(), output, "output parent is missing")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor: Optional[int] = None
    created = False
    try:
        descriptor = os.open(output, flags, 0o600)
        created = True
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError(errno.EIO, "zero-length write")
            view = view[written:]
        os.fsync(descriptor)
        os.fchmod(descriptor, 0o444)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = None
        directory = os.open(output.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except OSError as error:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        if created:
            try:
                output.unlink()
            except OSError:
                pass
        raise ReportError(f"{output}: exclusive publication failed: {error}") from error


def _sequence_pair(value: str) -> Tuple[int, int]:
    match = re.fullmatch(r"([1-9][0-9]*):([1-9][0-9]*)", value)
    if match is None:
        raise argparse.ArgumentTypeError("expected START:END positive sequences")
    return int(match.group(1)), int(match.group(2))


def _fixture_counters(scale: int) -> Mapping[str, int]:
    complete = scale * 100
    sampled_blocks = complete * SAMPLE_WINDOW_BLOCKS
    sampled_steps = sampled_blocks * 8
    candidate_blocks = scale * 12000
    candidate_direct = candidate_blocks * 6
    candidate_helper = candidate_blocks
    transient = scale * 200
    counters = {field: 0 for field in COUNTER_FIELDS}
    counters.update(
        {
            "route_stable_blocks": scale * 1000,
            "route_stable_steps": scale * 7000,
            "route_first_negative_blocks": scale * 400,
            "route_first_negative_steps": scale * 2600,
            "route_transient_blocks": transient,
            "route_transient_steps": scale * 900,
            "transient_prefer_ordinary_blocks": scale * 60,
            "transient_busy_blocks": scale * 20,
            "transient_capacity_blocks": scale * 20,
            "transient_unhealthy_blocks": 0,
            "transient_delayed_blocks": scale * 20,
            "transient_bypass_blocks": scale * 20,
            "transient_publication_blocks": scale * 20,
            "transient_other_blocks": scale * 40,
            "windows_scheduled": complete,
            "windows_complete": complete,
            "sampled_blocks": sampled_blocks,
            "sampled_steps": sampled_steps,
            "sampled_wall_ns": scale * 100_000_000,
            "sampled_thread_cpu_ns": scale * 90_000_000,
            "component_dispatch_ns": scale * 40_000_000,
            "component_rflags_ns": scale * 20_000_000,
            "component_guest_helper_ns": scale * 15_000_000,
            "component_signature_guard_ns": scale * 5_000_000,
            "component_residual_ns": scale * 20_000_000,
            "lowered_blocks": sampled_blocks - scale * 100,
            "lowering_fail_blocks": scale * 100,
            "op_dispatch": sampled_steps - scale * 90_000,
            "op_rflags": scale * 20_000,
            "op_guest_memory": scale * 10_000,
            "op_register_helper": scale * 20_000,
            "op_excluded_helper": scale * 10_000,
            "op_unsupported": scale * 30_000,
            "candidate_blocks": candidate_blocks,
            "candidate_steps": candidate_direct + candidate_helper,
            "candidate_direct_ops": candidate_direct,
            "candidate_helper_ops": candidate_helper,
            "flushes": scale,
        }
    )
    return counters


def _fixture_snapshot_lines(sequence: int, reason: str, present: int, scale: int) -> List[str]:
    counters = _fixture_counters(scale)
    profile: Dict[str, Any] = {
        "version": 1,
        "sequence": sequence,
        "reason": reason,
        "present": present,
        "uptime_raw_ns": sequence * 1_000_000,
        "final": 1 if reason == "final" else 0,
        "valid": 1,
        "pending_request": 0,
        "signal_owned": 1,
        "snapshot_consistent": 1,
        "accounting_consistent": 1,
        **counters,
        "candidate_identity_bits": scale * 100,
        "sampled_unflushed_blocks_bound": 0,
        "route_unflushed_blocks_bound": 0,
    }
    profile["checksum"] = _snapshot_checksum(profile)
    return [PROFILE_PREFIX + " ".join(f"{field}={profile[field]}" for field in PROFILE_FIELDS)]


def _fixture_log(checkpoint_reason: str = "checkpoint") -> str:
    config_values = {
        **CONFIG_EXPECTED,
        "clock_floor_ns": 20,
        "thread_clock_floor_ns": 20,
    }
    config = CONFIG_PREFIX + " ".join(f"{field}={config_values[field]}" for field in CONFIG_FIELDS)
    lines = ["fixture noise", config]
    lines.extend(_fixture_snapshot_lines(1, "start", 0, 0))
    lines.append(
        "[FRAME_PIPELINE] present=1 avg_interval=0.000 ms avg_latency=1.000 ms pending=0 index=0 arg=0"
    )
    for present in range(120, 4201, 120):
        lines.append(
            f"[FRAME_PIPELINE] present={present} avg_interval=10.000 ms "
            "avg_latency=2.000 ms pending=0 index=1 arg=0"
        )
        if present == 2280:
            lines.append(
                "[FRAME_CHECKPOINT] present=2300 target=2300 cumulative_ms=22990.000 "
                "cumulative_intervals=2299 partial_ms=200.000 partial_intervals=20 terminal=0"
            )
            lines.extend(_fixture_snapshot_lines(2, checkpoint_reason, 2300, 1))
        elif present == 3000:
            lines.extend(_fixture_snapshot_lines(3, "signal", 3000, 2))
        elif present == 3120:
            lines.extend(_fixture_snapshot_lines(4, "signal", 3120, 3))
        elif present == 3960:
            lines.extend(_fixture_snapshot_lines(5, "signal", 4000, 4))
        elif present == 4080:
            lines.extend(_fixture_snapshot_lines(6, "signal", 4120, 5))
    lines.extend(_fixture_snapshot_lines(7, "final", 0, 6))
    return "\n".join(lines) + "\n"


def _fixture_live_binding(profile: ProfileLog) -> Mapping[str, Any]:
    def stage(sequence: int, receipt: int, sent: int, observed: int) -> Mapping[str, Any]:
        snapshot = profile.by_sequence[sequence]
        return {
            "receipt_uptime_raw_ns": receipt,
            "signal_sent_uptime_raw_ns": sent,
            "signal_observed_uptime_raw_ns": observed,
            "sequence": sequence,
            "snapshot_present": snapshot.present,
            "snapshot_uptime_raw_ns": snapshot.uptime_raw_ns,
            "snapshot_checksum": snapshot.checksum,
            "evidence_verified": True,
        }

    def scene(anchor: Mapping[str, Any], complete: Mapping[str, Any], endpoint: int) -> Mapping[str, Any]:
        return {
            "anchor": anchor,
            "complete": complete,
            "endpoint_before": endpoint,
            "endpoint_after": endpoint,
            "discard_endpoint": endpoint + WINDOW_SIZE,
            "measured_endpoint": endpoint + 2 * WINDOW_SIZE,
            "avg_interval_ms": "10.000",
            "manual_input_only_attested": True,
            "zero_input_window_attested": True,
            "oracle_attested": True,
        }

    return {
        "schema": LIVE_BINDING_SCHEMA,
        "scope": DIAGNOSTIC_SCOPE,
        "checkpoint": {"count": 1, "terminal": 0, "observed_uptime_raw_ns": 2_100_000},
        "scenes": {
            "fort": scene(
                stage(3, 2_200_000, 2_300_000, 3_100_000),
                stage(4, 3_200_000, 3_300_000, 4_100_000),
                2880,
            ),
            "in_game": scene(
                stage(5, 4_200_000, 4_300_000, 5_100_000),
                stage(6, 5_200_000, 5_300_000, 6_100_000),
                3840,
            ),
        },
        "lifecycle": {
            "exit_status_bytes": "0\n",
            "sdl_quit_count": 1,
            "fatal_count": 0,
            "assert_count": 0,
            "dynarec_unhealthy_count": 0,
            "descriptor_mismatch_count": 0,
            "device_lost_count": 0,
            "frame_limit_count": 0,
            "signal_snapshot_count": 4,
            "final_sequence": 7,
            "final_clean": True,
            "no_input_before_p2300_attested": True,
            "no_glitch_attested": True,
            "evidence_verified": True,
        },
    }


def _expect_failure(action: Callable[[], object], description: str) -> None:
    try:
        action()
    except ReportError:
        return
    raise ReportError(f"self-test accepted {description}")


def _mutate_profile(
    text: str, sequence: int, field: str, value: int, *, repair_checksum: bool = True
) -> str:
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if not line.startswith(PROFILE_PREFIX) or f" sequence={sequence} " not in line:
            continue
        raw = dict(token.split("=", 1) for token in line[len(PROFILE_PREFIX) :].split(" "))
        raw[field] = str(value)
        if repair_checksum:
            parsed: Dict[str, Any] = {
                name: raw[name] if name == "reason" else int(raw[name], 10)
                for name in PROFILE_FIELDS
                if name != "checksum"
            }
            raw["checksum"] = _snapshot_checksum(parsed)
        lines[index] = PROFILE_PREFIX + " ".join(f"{name}={raw[name]}" for name in PROFILE_FIELDS)
        return "\n".join(lines) + "\n"
    raise ReportError(f"self-test fixture lacks sequence {sequence}")


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="phase6cm-profile-report-self-test-") as temporary:
        root = pathlib.Path(temporary)
        valid_path = root / "valid.log"
        valid_text = _fixture_log()
        valid_path.write_text(valid_text, encoding="utf-8")
        profile = parse_log(valid_path)
        report = build_report(profile, (3, 4), (5, 6))
        _require(report["valid"] is True, "self-test", "valid report rejected")
        _require(
            report["decision"]["status"] == "NO_ELECTION"
            and report["decision"]["elected"] is None
            and report["decision"]["speedup_claim"] is False,
            "self-test",
            "unbound lifecycle produced an election",
        )
        fort_cost = report["segments"]["fort"]["sampled_cost"]
        _require(
            fort_cost["calibrated_overhead_lower_bound_ns"]["wall"] > 0
            and fort_cost["calibrated_overhead_lower_bound_ns"]["thread_cpu"] > 0
            and fort_cost["wall_ns"]
            == fort_cost["calibrated_overhead_lower_bound_ns"]["wall"]
            + fort_cost["net_cost_upper_bound_ns"]["wall"]
            and fort_cost["thread_cpu_ns"]
            == fort_cost["calibrated_overhead_lower_bound_ns"]["thread_cpu"]
            + fort_cost["net_cost_upper_bound_ns"]["thread_cpu"],
            "self-test",
            "clock-floor/raw/net-upper-bound accounting mismatch",
        )
        uncertainty_gate = _candidate_material(
            {
                "candidate_blocks": 10000,
                "candidate_steps": 70000,
                "sampled_blocks": 1000000,
                "sampled_steps": 7000000,
            },
            profile.config,
            256,
            256,
        )
        _require(
            uncertainty_gate["raw_all_three_thresholds_pass"] is True
            and uncertainty_gate["uncertainty_robust_all_three_pass"] is False,
            "self-test",
            "endpoint uncertainty did not fail-close a raw threshold pass",
        )
        non_material_segments = json.loads(json.dumps(report["segments"]))
        non_material_segments["fort"]["candidate"]["all_three_thresholds_pass"] = False
        non_material = _election(non_material_segments, lifecycle_bound=True)
        _require(
            non_material["status"] == "NO_ELECTION"
            and non_material["elected"] is None
            and non_material["speedup_claim"] is False
            and non_material["observed_common_dominant_bucket"]
            == "component_dispatch_ns",
            "self-test",
            "a descriptive timing bucket became an unregistered election",
        )
        bound = build_report(
            profile, (3, 4), (5, 6), live_binding=_fixture_live_binding(profile)
        )
        _require(
            bound["diagnostic_gate"]["status"] == "PASS_DIAGNOSTIC_ONLY"
            and bound["decision"]["status"] == "PHASE6CN_MATERIAL"
            and bound["persistent_baseline_eligible"] is False
            and bound["loading_metric_eligible"] is False,
            "self-test",
            "bound report overclaims baseline eligibility",
        )
        output = root / "report.json"
        payload = _serialize_report(bound)
        _publish_report_exclusive(output, payload)
        _require(
            output.read_bytes() == payload and stat.S_IMODE(output.stat().st_mode) == 0o444,
            "self-test",
            "exclusive report publication mismatch",
        )
        _expect_failure(lambda: _publish_report_exclusive(output, b"overwrite\n"), "report overwrite")

        corrupt = root / "corrupt.log"
        corrupt.write_text(
            valid_text.replace("checksum=", "checksum=0", 1), encoding="utf-8"
        )
        _expect_failure(lambda: parse_log(corrupt), "corrupt checksum")

        accounting = root / "accounting.log"
        profile_three = profile.by_sequence[3]
        accounting.write_text(
            _mutate_profile(
                valid_text,
                3,
                "op_dispatch",
                profile_three.counters["op_dispatch"] + 1,
                repair_checksum=True,
            ),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(accounting), "checksum-valid op accounting failure")

        transient = root / "transient.log"
        transient.write_text(
            _mutate_profile(
                valid_text,
                4,
                "transient_other_blocks",
                profile.by_sequence[4].counters["transient_other_blocks"] + 1,
            ),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(transient), "transient histogram mismatch")

        bound_error = root / "bound.log"
        bound_error.write_text(
            _mutate_profile(valid_text, 5, "route_unflushed_blocks_bound", 1),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(bound_error), "bad unflushed route bound")

        impossible_floor = dict(profile.config)
        impossible_floor["clock_floor_ns"] = 1_000_000
        impossible_floor["thread_clock_floor_ns"] = 1_000_000
        _expect_failure(
            lambda: _segment(
                "impossible-floor",
                profile.by_sequence[3],
                profile.by_sequence[4],
                impossible_floor,
            ),
            "raw timing below calibrated clock-floor product",
        )

        terminal = root / "terminal.log"
        terminal.write_text(
            valid_text.replace("partial_intervals=20 terminal=0", "partial_intervals=20 terminal=1"),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(terminal), "terminal p2300")

        duplicate = root / "duplicate.log"
        config_line = next(line for line in valid_text.splitlines() if line.startswith(CONFIG_PREFIX))
        duplicate.write_text(valid_text + config_line + "\n", encoding="utf-8")
        _expect_failure(lambda: parse_log(duplicate), "duplicate CONFIG")

        coalesced = root / "checkpoint-signal.log"
        coalesced.write_text(_fixture_log("checkpoint_signal"), encoding="utf-8")
        _require(
            parse_log(coalesced).snapshots[1].reason == "checkpoint_signal",
            "self-test",
            "checkpoint_signal was not accepted",
        )

        bad_binding = json.loads(json.dumps(_fixture_live_binding(profile)))
        bad_binding["scenes"]["fort"]["anchor"]["snapshot_checksum"] = "0" * 16
        _expect_failure(
            lambda: build_report(profile, (3, 4), (5, 6), live_binding=bad_binding),
            "live-binding checksum mismatch",
        )


def _main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "validate phase-6CM CONFIG/PROFILE v1, subtract loading/Fort/in-game "
            "counters, and publish a diagnostic-only report"
        )
    )
    parser.add_argument("log", nargs="?", metavar="STDERR.log")
    parser.add_argument("--fort", type=_sequence_pair, metavar="START:END")
    parser.add_argument("--ingame", type=_sequence_pair, metavar="START:END")
    parser.add_argument("--output", type=pathlib.Path, metavar="REPORT.json")
    parser.add_argument("--no-stdout", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        if any(
            value is not None
            for value in (arguments.log, arguments.fort, arguments.ingame, arguments.output)
        ) or arguments.no_stdout:
            parser.error("--self-test does not accept paths or report options")
        self_test()
        print("phase6cm-profile-report-self-test: PASS")
        return 0
    if arguments.log is None:
        parser.error("STDERR.log is required")
    if arguments.fort is None or arguments.ingame is None:
        parser.error("--fort START:END and --ingame START:END are required")
    if arguments.output is None:
        parser.error("--output REPORT.json is required")
    profile = parse_log(pathlib.Path(arguments.log))
    report = build_report(profile, arguments.fort, arguments.ingame)
    payload = _serialize_report(report)
    _publish_report_exclusive(arguments.output, payload)
    print(_render_table(report), file=sys.stderr)
    if not arguments.no_stdout:
        sys.stdout.write(payload.decode("utf-8"))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except ReportError as error:
        print(f"phase6cm-profile-report: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
