#!/usr/bin/env python3
"""Validate and segment phase-6CK cumulative dynarec profile snapshots.

The tool is offline and standard-library only.  It reads an existing stderr
log, validates the v1 CONFIG/PROFILE/REUSE stream and its checksums, then
subtracts cumulative counters at three named boundaries:

* loading: the mandatory ``start`` snapshot through the mandatory p2300
  ``checkpoint`` snapshot;
* Fort Mode: two caller-selected SIGUSR2 snapshot sequences;
* in-game: two caller-selected SIGUSR2 snapshot sequences.

JSON is published exactly once through ``--output`` and may also be previewed
on stdout; a compact table goes to stderr.  The report exposes evidence for
choosing synchronous admission at encounter two or three, but it does not elect
a threshold automatically.
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
from dataclasses import dataclass, replace
from typing import Any, Callable, Dict, List, Mapping, Optional, Sequence, Tuple


REPORT_SCHEMA_NAME = "lumen.phase6ck-profile-report"
REPORT_SCHEMA_VERSION = 1
LIVE_BINDING_SCHEMA = "lumen.phase6ck-profile-live-binding/1"
DIAGNOSTIC_SCOPE = "diagnostic_only_not_persistent_baseline"
PROFILE_VERSION = 1
CHECKPOINT_PRESENT = 2300
FALLBACK_SAMPLE_INTERVAL = 256
FALLBACK_BATCH_SIZE = 256
UINT64_MAX = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
UINT64_MASK = UINT64_MAX
FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
ZERO = decimal.Decimal(0)
ONE_HUNDRED = decimal.Decimal(100)
ONE_THOUSAND = decimal.Decimal(1000)

UNSIGNED_RE = re.compile(r"^(?:0|[1-9][0-9]*)$")
SIGNED_RE = re.compile(r"^(?:0|-?[1-9][0-9]*)$")
DECIMAL_RE = re.compile(r"^(?:0|[1-9][0-9]*)(?:\.[0-9]+)?$")
CHECKSUM_RE = re.compile(r"^[0-9a-f]{16}$")
REASON_RE = re.compile(r"^[a-z]+(?:_[a-z]+)*$")

CONFIG_PREFIX = "[A64_DENSE_PHASE6CK_CONFIG] "
PROFILE_PREFIX = "[A64_DENSE_PHASE6CK_PROFILE] "
REUSE_PREFIX = "[A64_DENSE_PHASE6CK_REUSE] "
MARKER_PREFIX = "[A64_DENSE_PHASE6CK_"
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
    "direct_binding_compiled",
    "direct_binding_selector",
    "dense_resident",
    "dense_victim",
    "dense_async",
    "dense_async_admission",
    "hybrid",
    "contextless",
)
CONFIG_EXPECTED: Mapping[str, Any] = {
    "version": 1,
    "compiled": 1,
    "requested": 1,
    "effective": 1,
    "production_shape": 1,
    "signal": "SIGUSR2",
    "signal_installed": 1,
    "direct_binding_compiled": 1,
    "direct_binding_selector": 0,
    "dense_resident": 1,
    "dense_victim": 0,
    "dense_async": 0,
    "dense_async_admission": 0,
    "hybrid": 0,
    "contextless": 0,
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
BUILD_COUNTER_FIELDS = (
    "lower_attempts",
    "lower_ok",
    "lower_fail",
    "lower_ns",
    "compile_attempts",
    "compile_ok",
    "compile_capacity",
    "compile_fail",
    "compile_ns",
    "staging_ns",
    "emit_ns",
    "allocate_ns",
    "publish_ns",
    "code_bytes",
    "code_capacity",
    "identity_first",
    "identity_second",
    "identity_third",
    "retired_once",
    "retired_twice",
    "retired_three_plus",
    "live_once",
    "live_twice",
    "live_three_plus",
    "bypasses",
    "capacity_fallbacks",
    "fallback_sample_interval",
    "fallback_batch_size",
    "fallback_active_owners",
    "fallback_unflushed_blocks_bound",
    "fallback_ordinary_blocks",
    "fallback_ordinary_steps",
    "fallback_ordinary_samples",
    "fallback_ordinary_sample_steps",
    "fallback_ordinary_sample_ns",
    "fallback_ordinary_flushes",
    "fallback_precise_blocks",
    "fallback_precise_steps",
    "fallback_precise_samples",
    "fallback_precise_sample_steps",
    "fallback_precise_sample_ns",
    "fallback_precise_flushes",
)
RESET_COUNTER_FIELDS = (
    "reset_other",
    "reset_identity",
    "reset_capacity",
    "reset_eviction",
    "reset_cache",
    "reset_busy",
    "invalid",
)
COUNTER_FIELDS = BUILD_COUNTER_FIELDS + RESET_COUNTER_FIELDS
CUMULATIVE_COUNTER_FIELDS = tuple(
    field
    for field in COUNTER_FIELDS
    if field
    not in (
        "live_once",
        "live_twice",
        "live_three_plus",
        "fallback_sample_interval",
        "fallback_batch_size",
        "fallback_active_owners",
        "fallback_unflushed_blocks_bound",
    )
)
LIVE_GAUGE_FIELDS = ("live_once", "live_twice", "live_three_plus")
FALLBACK_GAUGE_FIELDS = (
    "fallback_active_owners",
    "fallback_unflushed_blocks_bound",
)
PROFILE_FIELDS = PROFILE_HEADER_FIELDS + COUNTER_FIELDS + ("checksum",)

REUSE_FIELDS = (
    "version",
    "sequence",
    "encounter",
    "d1",
    "d2",
    "d3",
    "d4_7",
    "d8_15",
    "d16_31",
    "d32_127",
    "d128_plus",
)
REUSE_BUCKET_FIELDS = REUSE_FIELDS[3:]
REUSE_ENCOUNTERS = (2, 3)
ALLOWED_REASONS = frozenset({"start", "checkpoint", "signal", "final"})

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
    """The profile stream or selected boundary is not trustworthy."""


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
    checksum: str
    reuse: Mapping[int, Mapping[str, int]]
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
    _require(
        SIGNED_RE.fullmatch(value) is not None,
        source,
        f"{field} is not canonical signed decimal",
    )
    _require(len(value) <= 20, source, f"{field} exceeds int64")
    parsed = int(value, 10)
    _require(INT64_MIN <= parsed <= INT64_MAX, source, f"{field} exceeds int64")
    return parsed


def _parse_decimal(value: str, source: object, field: str) -> decimal.Decimal:
    _require(
        DECIMAL_RE.fullmatch(value) is not None,
        source,
        f"{field} is not canonical non-negative decimal",
    )
    parsed = decimal.Decimal(value)
    _require(parsed.is_finite(), source, f"{field} is not finite")
    return parsed


def _parse_pipeline(line: str, source: object, line_number: int) -> FrameWindow:
    match = PIPELINE_RE.fullmatch(line)
    _require(match is not None, source, "malformed FRAME_PIPELINE present record")
    assert match is not None
    return FrameWindow(
        present=_parse_uint(match.group("present"), source, "present"),
        avg_interval_ms=_parse_decimal(
            match.group("interval"), source, "avg_interval"
        ),
        avg_latency_ms=_parse_decimal(
            match.group("latency"), source, "avg_latency"
        ),
        pending=_parse_uint(match.group("pending"), source, "pending"),
        index=_parse_int64(match.group("index"), source, "index"),
        arg=_parse_int64(match.group("arg"), source, "arg"),
        line_number=line_number,
    )


def _parse_checkpoint(
    line: str, source: object, line_number: int
) -> FrameCheckpoint:
    match = CHECKPOINT_RE.fullmatch(line)
    _require(match is not None, source, "malformed FRAME_CHECKPOINT record")
    assert match is not None
    return FrameCheckpoint(
        present=_parse_uint(match.group("present"), source, "present"),
        target=_parse_uint(match.group("target"), source, "target"),
        cumulative_ms=_parse_decimal(
            match.group("cumulative_ms"), source, "cumulative_ms"
        ),
        cumulative_intervals=_parse_uint(
            match.group("cumulative_intervals"), source, "cumulative_intervals"
        ),
        partial_ms=_parse_decimal(
            match.group("partial_ms"), source, "partial_ms"
        ),
        partial_intervals=_parse_uint(
            match.group("partial_intervals"), source, "partial_intervals"
        ),
        terminal=_parse_uint(match.group("terminal"), source, "terminal"),
        line_number=line_number,
    )


def _parse_ordered_fields(
    line: str,
    prefix: str,
    expected: Sequence[str],
    source: object,
) -> Mapping[str, str]:
    _require(line.startswith(prefix), source, "internal marker prefix mismatch")
    payload = line[len(prefix) :]
    tokens = payload.split(" ")
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
    parsed: Dict[str, Any] = {}
    for field in CONFIG_FIELDS:
        parsed[field] = (
            raw[field]
            if field == "signal"
            else _parse_uint(raw[field], source, field)
        )
    for field, expected in CONFIG_EXPECTED.items():
        _require(
            type(parsed[field]) is type(expected) and parsed[field] == expected,
            source,
            f"{field} must equal {expected!r}",
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


def _parse_reuse(line: str, source: object) -> Mapping[str, int]:
    raw = _parse_ordered_fields(line, REUSE_PREFIX, REUSE_FIELDS, source)
    parsed = {field: _parse_uint(raw[field], source, field) for field in REUSE_FIELDS}
    _require(parsed["version"] == PROFILE_VERSION, source, "unsupported REUSE version")
    _require(
        parsed["encounter"] in REUSE_ENCOUNTERS,
        source,
        "REUSE encounter must be 2 or 3",
    )
    return parsed


def _checksum_mix(checksum: int, value: int) -> int:
    return ((checksum ^ value) * FNV_PRIME) & UINT64_MASK


def _snapshot_checksum(
    profile: Mapping[str, Any], reuse: Mapping[int, Mapping[str, int]]
) -> str:
    checksum = FNV_OFFSET
    for field in (
        "sequence",
        "present",
        "uptime_raw_ns",
        "snapshot_consistent",
    ) + BUILD_COUNTER_FIELDS:
        checksum = _checksum_mix(checksum, profile[field])
    for encounter in REUSE_ENCOUNTERS:
        for field in REUSE_BUCKET_FIELDS:
            checksum = _checksum_mix(checksum, reuse[encounter][field])
    for field in RESET_COUNTER_FIELDS[:-2]:
        checksum = _checksum_mix(checksum, profile[field])
    checksum = _checksum_mix(checksum, profile["reset_busy"])
    checksum = _checksum_mix(checksum, profile["invalid"])
    return f"{checksum:016x}"


def _validate_snapshot_equations(snapshot: Snapshot, source: object) -> None:
    counters = snapshot.counters
    _require(
        counters["fallback_sample_interval"] == FALLBACK_SAMPLE_INTERVAL,
        source,
        f"fallback_sample_interval must be {FALLBACK_SAMPLE_INTERVAL}",
    )
    _require(
        counters["fallback_batch_size"] == FALLBACK_BATCH_SIZE,
        source,
        f"fallback_batch_size must be {FALLBACK_BATCH_SIZE}",
    )
    fallback_bound = counters["fallback_active_owners"] * (
        (FALLBACK_BATCH_SIZE - 1) * 2
    )
    _require(fallback_bound <= UINT64_MAX, source, "fallback owner bound overflow")
    _require(
        counters["fallback_unflushed_blocks_bound"] == fallback_bound,
        source,
        "fallback unflushed block bound mismatch",
    )
    _require(
        counters["lower_attempts"]
        == counters["lower_ok"] + counters["lower_fail"],
        source,
        "lower_attempts != lower_ok + lower_fail",
    )
    _require(
        counters["compile_attempts"]
        == counters["compile_ok"]
        + counters["compile_capacity"]
        + counters["compile_fail"],
        source,
        "compile attempt accounting mismatch",
    )
    first_population = sum(
        counters[field]
        for field in (
            "retired_once",
            "retired_twice",
            "retired_three_plus",
            "live_once",
            "live_twice",
            "live_three_plus",
        )
    )
    second_population = sum(
        counters[field]
        for field in (
            "retired_twice",
            "retired_three_plus",
            "live_twice",
            "live_three_plus",
        )
    )
    third_population = counters["retired_three_plus"] + counters["live_three_plus"]
    _require(
        counters["identity_first"] == first_population,
        source,
        "identity_first/live+retired accounting mismatch",
    )
    _require(
        counters["identity_second"] == second_population,
        source,
        "identity_second/live+retired accounting mismatch",
    )
    _require(
        counters["identity_third"] == third_population,
        source,
        "identity_third/live+retired accounting mismatch",
    )
    _require(
        counters["identity_first"] <= counters["compile_ok"],
        source,
        "first encounters exceed successful compiles",
    )
    _require(
        counters["capacity_fallbacks"] <= counters["compile_capacity"],
        source,
        "capacity fallbacks exceed capacity compile results",
    )
    compile_subparts = sum(
        counters[field]
        for field in ("staging_ns", "emit_ns", "allocate_ns", "publish_ns")
    )
    _require(
        compile_subparts <= counters["compile_ns"],
        source,
        "compile sub-timings exceed inclusive compile_ns",
    )
    _require(
        counters["code_bytes"] <= counters["code_capacity"],
        source,
        "code_bytes exceeds code_capacity",
    )
    reset_total = sum(counters[field] for field in RESET_COUNTER_FIELDS[:5])
    retired_total = sum(
        counters[field]
        for field in ("retired_once", "retired_twice", "retired_three_plus")
    )
    _require(retired_total <= reset_total, source, "retired identities exceed resets")
    for kind in ("ordinary", "precise"):
        blocks = counters[f"fallback_{kind}_blocks"]
        steps = counters[f"fallback_{kind}_steps"]
        samples = counters[f"fallback_{kind}_samples"]
        sample_steps = counters[f"fallback_{kind}_sample_steps"]
        flushes = counters[f"fallback_{kind}_flushes"]
        _require(
            samples <= blocks,
            source,
            f"fallback {kind} samples exceed blocks",
        )
        _require(
            sample_steps <= steps,
            source,
            f"fallback {kind} sample steps exceed total steps",
        )
        _require(
            flushes <= blocks,
            source,
            f"fallback {kind} flushes exceed blocks",
        )
        if blocks == 0:
            _require(
                steps == 0
                and samples == 0
                and sample_steps == 0
                and counters[f"fallback_{kind}_sample_ns"] == 0
                and flushes == 0,
                source,
                f"fallback {kind} has data without flushed blocks",
            )
        if samples == 0:
            _require(
                sample_steps == 0
                and counters[f"fallback_{kind}_sample_ns"] == 0,
                source,
                f"fallback {kind} has timing data without samples",
            )
    for encounter, identity_field in (
        (2, "identity_second"),
        (3, "identity_third"),
    ):
        reuse_total = sum(snapshot.reuse[encounter][field] for field in REUSE_BUCKET_FIELDS)
        _require(
            reuse_total == counters[identity_field],
            source,
            f"REUSE encounter {encounter} total does not match {identity_field}",
        )


def parse_log(path: pathlib.Path) -> ProfileLog:
    _require(path.is_file(), path, "log is not a regular file")
    digest = hashlib.sha256()
    config: Optional[Mapping[str, Any]] = None
    config_line_number = 0
    profiles: Dict[int, Mapping[str, Any]] = {}
    reuse: Dict[int, Dict[int, Mapping[str, int]]] = {}
    marker_order: List[Tuple[str, int, int]] = []
    frames: List[FrameWindow] = []
    checkpoint_records: List[FrameCheckpoint] = []

    try:
        with path.open("rb") as stream:
            for line_number, encoded in enumerate(stream, start=1):
                digest.update(encoded)
                if encoded.startswith(b"[FRAME_LIMIT]"):
                    raise ReportError(
                        f"{path}: line {line_number}: terminal FRAME_LIMIT is forbidden"
                    )
                relevant = (
                    encoded.startswith(b"[A64_DENSE_PHASE6CK_")
                    or encoded.startswith(b"[FRAME_PIPELINE] present")
                    or encoded.startswith(b"[FRAME_CHECKPOINT]")
                )
                if not relevant:
                    continue
                source = f"{path}: line {line_number}"
                _require(encoded.endswith(b"\n"), source, "marker is not newline-terminated")
                _require(len(encoded) <= 16384, source, "marker line is unexpectedly large")
                try:
                    line = encoded[:-1].decode("utf-8", errors="strict")
                except UnicodeError as error:
                    raise ReportError(f"{source}: marker is not strict UTF-8: {error}") from error
                _require(not line.endswith("\r"), source, "CRLF marker is not canonical")

                if line.startswith("[FRAME_PIPELINE] present"):
                    frames.append(_parse_pipeline(line, source, line_number))
                elif line.startswith(CHECKPOINT_PREFIX):
                    checkpoint_records.append(
                        _parse_checkpoint(line, source, line_number)
                    )
                elif line.startswith(CONFIG_PREFIX):
                    _require(config is None, source, "duplicate CONFIG marker")
                    config = _parse_config(line, source)
                    config_line_number = line_number
                    marker_order.append(("config", 0, 0))
                elif line.startswith(PROFILE_PREFIX):
                    profile = _parse_profile(line, source, line_number)
                    sequence = profile["sequence"]
                    _require(sequence not in profiles, source, "duplicate PROFILE sequence")
                    profiles[sequence] = profile
                    marker_order.append(("profile", sequence, 0))
                elif line.startswith(REUSE_PREFIX):
                    record = _parse_reuse(line, source)
                    sequence = record["sequence"]
                    encounter = record["encounter"]
                    sequence_reuse = reuse.setdefault(sequence, {})
                    _require(encounter not in sequence_reuse, source, "duplicate REUSE encounter")
                    sequence_reuse[encounter] = record
                    marker_order.append(("reuse", sequence, encounter))
                elif line.startswith(MARKER_PREFIX):
                    raise ReportError(f"{source}: unknown phase-6CK marker")
    except OSError as error:
        raise ReportError(f"{path}: cannot read log: {error}") from error

    _require(config is not None, path, "expected exactly one CONFIG marker")
    _require(profiles, path, "missing PROFILE markers")
    ordered_sequences = sorted(profiles)
    _require(
        ordered_sequences == list(range(1, len(ordered_sequences) + 1)),
        path,
        "PROFILE sequences must be contiguous from one",
    )

    expected_order: List[Tuple[str, int, int]] = [("config", 0, 0)]
    snapshots: List[Snapshot] = []
    for sequence in ordered_sequences:
        expected_order.append(("profile", sequence, 0))
        for encounter in REUSE_ENCOUNTERS:
            expected_order.append(("reuse", sequence, encounter))
        profile = profiles[sequence]
        sequence_reuse = reuse.get(sequence, {})
        _require(
            set(sequence_reuse) == set(REUSE_ENCOUNTERS),
            path,
            f"sequence {sequence} must have exactly REUSE encounters 2,3",
        )
        expected_checksum = _snapshot_checksum(profile, sequence_reuse)
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
            checksum=profile["checksum"],
            reuse={
                encounter: {
                    field: sequence_reuse[encounter][field]
                    for field in REUSE_BUCKET_FIELDS
                }
                for encounter in REUSE_ENCOUNTERS
            },
            line_number=profile["line_number"],
        )
        _validate_snapshot_equations(snapshot, f"{path}: sequence {sequence}")
        snapshots.append(snapshot)

    _require(marker_order == expected_order, path, "CONFIG/PROFILE/REUSE marker order mismatch")
    _validate_stream(tuple(snapshots), path)
    checkpoint_record = _validate_frame_stream(
        tuple(frames), tuple(checkpoint_records), path
    )
    _require(
        checkpoint_record.line_number < snapshots[1].line_number,
        path,
        "FRAME_CHECKPOINT must precede the phase-6CK checkpoint snapshot",
    )
    return ProfileLog(
        path=path.resolve(),
        sha256=digest.hexdigest(),
        config=config,
        config_line_number=config_line_number,
        snapshots=tuple(snapshots),
        frames=tuple(frames),
        checkpoint=checkpoint_record,
    )


def _validate_frame_stream(
    frames: Tuple[FrameWindow, ...],
    checkpoint_records: Tuple[FrameCheckpoint, ...],
    source: object,
) -> FrameCheckpoint:
    _require(
        len(checkpoint_records) == 1,
        source,
        "expected exactly one FRAME_CHECKPOINT record",
    )
    _require(frames, source, "missing FRAME_PIPELINE present records")
    labels = [frame.present for frame in frames]
    _require(len(set(labels)) == len(labels), source, "duplicate FRAME_PIPELINE label")
    _require(labels[0] == 1, source, "first FRAME_PIPELINE label must be p1")
    _require(frames[0].avg_interval_ms == ZERO, source, "p1 interval must be zero")
    for index, frame in enumerate(frames[1:], start=1):
        _require(
            frame.present == index * 120,
            source,
            "missing, unexpected, or non-monotonic p120 FRAME_PIPELINE label",
        )
        _require(
            frame.avg_interval_ms > ZERO,
            source,
            f"p{frame.present} avg_interval must be positive",
        )

    checkpoint = checkpoint_records[0]
    _require(
        checkpoint.present == CHECKPOINT_PRESENT
        and checkpoint.target == CHECKPOINT_PRESENT,
        source,
        "FRAME_CHECKPOINT present and target must both be p2300",
    )
    _require(checkpoint.terminal == 0, source, "FRAME_CHECKPOINT must be non-terminal")
    _require(
        checkpoint.cumulative_ms > ZERO,
        source,
        "FRAME_CHECKPOINT cumulative_ms must be positive",
    )
    _require(
        checkpoint.cumulative_intervals == CHECKPOINT_PRESENT - 1,
        source,
        "FRAME_CHECKPOINT cumulative_intervals mismatch",
    )
    lower = CHECKPOINT_PRESENT - (CHECKPOINT_PRESENT % 120)
    upper = lower + 120
    frame_map = {frame.present: frame for frame in frames}
    _require(lower in frame_map, source, f"missing p{lower} before checkpoint")
    _require(upper in frame_map, source, f"missing p{upper} after checkpoint")
    _require(
        frame_map[lower].line_number
        < checkpoint.line_number
        < frame_map[upper].line_number,
        source,
        "FRAME_CHECKPOINT is not between adjacent p120 records",
    )
    _require(
        checkpoint.partial_intervals == CHECKPOINT_PRESENT - lower,
        source,
        "FRAME_CHECKPOINT partial_intervals mismatch",
    )
    _require(checkpoint.partial_ms > ZERO, source, "checkpoint partial_ms must be positive")
    _require(
        checkpoint.partial_ms <= checkpoint.cumulative_ms,
        source,
        "checkpoint partial_ms exceeds cumulative_ms",
    )
    return checkpoint


def _validate_stream(snapshots: Tuple[Snapshot, ...], source: object) -> None:
    _require(len(snapshots) >= 7, source, "expected start, checkpoint, four signals, and final")
    first = snapshots[0]
    checkpoint = snapshots[1]
    final = snapshots[-1]
    _require(
        first.reason == "start" and first.present == 0 and first.final == 0,
        source,
        "sequence one must be the non-final start snapshot",
    )
    _require(
        all(
            value
            == (
                FALLBACK_SAMPLE_INTERVAL
                if field == "fallback_sample_interval"
                else FALLBACK_BATCH_SIZE
                if field == "fallback_batch_size"
                else 0
            )
            for field, value in first.counters.items()
        ),
        source,
        "start counters must be zero except fixed fallback sampling parameters",
    )
    _require(
        all(
            value == 0
            for encounter in REUSE_ENCOUNTERS
            for value in first.reuse[encounter].values()
        ),
        source,
        "start reuse counters must all be zero",
    )
    _require(
        checkpoint.reason == "checkpoint"
        and checkpoint.present == CHECKPOINT_PRESENT
        and checkpoint.final == 0,
        source,
        "sequence two must be the p2300 checkpoint without a coalesced signal",
    )
    _require(
        final.reason == "final" and final.present == 0 and final.final == 1,
        source,
        "last sequence must be the final snapshot",
    )

    previous = snapshots[0]
    for index, snapshot in enumerate(snapshots):
        _require(
            snapshot.reason in ALLOWED_REASONS,
            source,
            f"sequence {snapshot.sequence}: invalid reason",
        )
        _require(
            snapshot.uptime_raw_ns > 0,
            source,
            f"sequence {snapshot.sequence}: zero uptime",
        )
        _require(
            snapshot.valid == 1,
            source,
            f"sequence {snapshot.sequence}: valid must be one",
        )
        _require(
            snapshot.counters["invalid"] == 0,
            source,
            f"sequence {snapshot.sequence}: invalid counter is nonzero",
        )
        _require(
            snapshot.pending_request == 0,
            source,
            f"sequence {snapshot.sequence}: request remains pending",
        )
        _require(
            snapshot.signal_owned == 1,
            source,
            f"sequence {snapshot.sequence}: SIGUSR2 ownership lost",
        )
        _require(
            snapshot.snapshot_consistent == 1,
            source,
            f"sequence {snapshot.sequence}: snapshot is not consistent",
        )
        _require(
            snapshot.accounting_consistent == 1,
            source,
            f"sequence {snapshot.sequence}: identity accounting is inconsistent",
        )
        if index != len(snapshots) - 1:
            _require(snapshot.final == 0, source, f"sequence {snapshot.sequence}: early final flag")
        if index > 0:
            _require(
                snapshot.uptime_raw_ns > previous.uptime_raw_ns,
                source,
                f"sequence {snapshot.sequence}: uptime is not strictly increasing",
            )
            for field in CUMULATIVE_COUNTER_FIELDS:
                _require(
                    snapshot.counters[field] >= previous.counters[field],
                    source,
                    f"sequence {snapshot.sequence}: counter {field} regressed",
                )
            for encounter in REUSE_ENCOUNTERS:
                for field in REUSE_BUCKET_FIELDS:
                    _require(
                        snapshot.reuse[encounter][field]
                        >= previous.reuse[encounter][field],
                        source,
                        f"sequence {snapshot.sequence}: REUSE {encounter}/{field} regressed",
                    )
        previous = snapshot

    for snapshot in snapshots[2:-1]:
        _require(
            snapshot.reason == "signal" and snapshot.present > CHECKPOINT_PRESENT,
            source,
            f"sequence {snapshot.sequence}: intermediate snapshot must be post-p2300 signal",
        )
    non_final = snapshots[:-1]
    for earlier, later in zip(non_final, non_final[1:]):
        _require(
            later.present > earlier.present,
            source,
            f"sequence {later.sequence}: present is not strictly increasing",
        )


def _ratio_percent(numerator: Any, denominator: Any) -> Optional[str]:
    if denominator == 0:
        return None
    with decimal.localcontext() as context:
        context.prec = 50
        value = decimal.Decimal(numerator) / decimal.Decimal(denominator) * ONE_HUNDRED
    return _format_decimal(value, 6)


def _ratio_value(numerator: int, denominator: int) -> Optional[str]:
    if denominator == 0:
        return None
    with decimal.localcontext() as context:
        context.prec = 50
        value = decimal.Decimal(numerator) / decimal.Decimal(denominator)
    return _format_decimal(value, 6)


def _format_decimal(value: decimal.Decimal, places: int) -> str:
    quantum = decimal.Decimal(1).scaleb(-places)
    integer_digits = max(1, value.copy_abs().adjusted() + 1) if value != ZERO else 1
    with decimal.localcontext() as context:
        context.prec = max(50, integer_digits + places + 4)
        rounded = value.quantize(quantum, rounding=decimal.ROUND_HALF_UP)
    return format(rounded, "f")


def _counter_delta(start: Snapshot, end: Snapshot) -> Mapping[str, int]:
    return {
        field: end.counters[field] - start.counters[field]
        for field in CUMULATIVE_COUNTER_FIELDS
    }


def _reuse_delta(
    start: Snapshot, end: Snapshot
) -> Mapping[int, Mapping[str, int]]:
    return {
        encounter: {
            field: end.reuse[encounter][field] - start.reuse[encounter][field]
            for field in REUSE_BUCKET_FIELDS
        }
        for encounter in REUSE_ENCOUNTERS
    }


def _fallback_analysis(
    counters: Mapping[str, int],
    start: Snapshot,
    end: Snapshot,
    elapsed_ns: int,
) -> Mapping[str, Any]:
    lanes: Dict[str, Mapping[str, Any]] = {}
    total_estimated_ns = ZERO
    estimate_complete = True
    for kind in ("ordinary", "precise"):
        blocks = counters[f"fallback_{kind}_blocks"]
        steps = counters[f"fallback_{kind}_steps"]
        samples = counters[f"fallback_{kind}_samples"]
        sample_steps = counters[f"fallback_{kind}_sample_steps"]
        sample_ns = counters[f"fallback_{kind}_sample_ns"]
        flushes = counters[f"fallback_{kind}_flushes"]
        _require(samples <= blocks, kind, "segment samples exceed flushed blocks")
        _require(sample_steps <= steps, kind, "segment sampled steps exceed steps")
        _require(flushes <= blocks, kind, "segment flushes exceed flushed blocks")
        if blocks == 0:
            _require(
                steps == 0
                and samples == 0
                and sample_steps == 0
                and sample_ns == 0
                and flushes == 0,
                kind,
                "segment has fallback data without flushed blocks",
            )
        if samples == 0:
            _require(
                sample_steps == 0 and sample_ns == 0,
                kind,
                "segment has fallback timing without samples",
            )
        estimate: Optional[decimal.Decimal]
        estimate_basis: Optional[str]
        if sample_steps > 0:
            with decimal.localcontext() as context:
                context.prec = 80
                estimate = (
                    decimal.Decimal(sample_ns)
                    * decimal.Decimal(steps)
                    / decimal.Decimal(sample_steps)
                )
            estimate_basis = "sample_ns/sample_steps scaled by total steps"
        elif samples > 0:
            with decimal.localcontext() as context:
                context.prec = 80
                estimate = (
                    decimal.Decimal(sample_ns)
                    * decimal.Decimal(blocks)
                    / decimal.Decimal(samples)
                )
            estimate_basis = "sample_ns/samples scaled by total blocks"
        elif blocks == 0:
            estimate = ZERO
            estimate_basis = "no flushed fallback blocks"
        else:
            estimate = None
            estimate_basis = None
            estimate_complete = False
        if estimate is not None:
            total_estimated_ns += estimate
        lanes[kind] = {
            "scope": (
                "ExecuteFastBlock after an exact-RIP dense fallback"
                if kind == "ordinary"
                else "precise decoder/interpreter after an exact-RIP dense fallback"
            ),
            "blocks": blocks,
            "steps": steps,
            "samples": samples,
            "sample_steps": sample_steps,
            "sample_ns": sample_ns,
            "flushes": flushes,
            "steps_per_block": _ratio_value(steps, blocks),
            "sample_coverage_blocks_percent": _ratio_percent(samples, blocks),
            "sampled_ns_per_block": _ratio_value(sample_ns, samples),
            "sampled_ns_per_step": _ratio_value(sample_ns, sample_steps),
            "estimated_total_ns": (
                _format_decimal(estimate, 6) if estimate is not None else None
            ),
            "estimated_elapsed_share_percent": (
                _ratio_percent(estimate, elapsed_ns)
                if estimate is not None
                else None
            ),
            "estimate_basis": estimate_basis,
        }
    return {
        "sample_interval_blocks_per_tls_lane": FALLBACK_SAMPLE_INTERVAL,
        "flush_batch_blocks_per_tls_lane": FALLBACK_BATCH_SIZE,
        "ordinary": lanes["ordinary"],
        "precise": lanes["precise"],
        "combined_estimated_total_ns": (
            _format_decimal(total_estimated_ns, 6) if estimate_complete else None
        ),
        "combined_estimated_elapsed_share_percent": (
            _ratio_percent(total_estimated_ns, elapsed_ns)
            if estimate_complete
            else None
        ),
        "endpoint_unflushed_uncertainty": {
            "start_active_owners": start.counters["fallback_active_owners"],
            "start_blocks_upper_bound": start.counters[
                "fallback_unflushed_blocks_bound"
            ],
            "end_active_owners": end.counters["fallback_active_owners"],
            "end_blocks_upper_bound": end.counters[
                "fallback_unflushed_blocks_bound"
            ],
        },
        "interpretation": (
            "fallback totals include flushed TLS batches only; periodic timing samples "
            "produce descriptive estimates, while endpoint bounds expose the combined "
            "ordinary+precise blocks that may remain unflushed"
        ),
    }


def _median_decimal(values: Sequence[decimal.Decimal]) -> decimal.Decimal:
    _require(bool(values), "frame curve", "cannot take median of an empty sequence")
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / decimal.Decimal(2)


def _frame_curve(
    name: str,
    start: Snapshot,
    end: Snapshot,
    frames: Sequence[FrameWindow],
) -> Mapping[str, Any]:
    selected = tuple(
        frame
        for frame in frames
        if frame.present != 1 and start.present < frame.present <= end.present
    )
    _require(
        bool(selected),
        name,
        "selected snapshot boundaries contain no complete p120 renderer window",
    )
    rows: List[Mapping[str, Any]] = []
    intervals: List[decimal.Decimal] = []
    latencies: List[decimal.Decimal] = []
    residuals: List[decimal.Decimal] = []
    fps_values: List[decimal.Decimal] = []
    for frame in selected:
        interval = frame.avg_interval_ms
        latency = frame.avg_latency_ms
        residual = max(interval - latency, ZERO)
        fps = ONE_THOUSAND / interval
        intervals.append(interval)
        latencies.append(latency)
        residuals.append(residual)
        fps_values.append(fps)
        rows.append(
            {
                "present": frame.present,
                "avg_interval_ms": format(interval, "f"),
                "derived_fps": _format_decimal(fps, 6),
                "avg_latency_ms": format(latency, "f"),
                "renderer_latency_share_percent": _ratio_percent(
                    latency, interval
                ),
                "upstream_residual_ms": _format_decimal(residual, 6),
                "pending": frame.pending,
                "index": frame.index,
                "arg": frame.arg,
                "line_number": frame.line_number,
            }
        )
    return {
        "selection": "p120 endpoints with start.present < endpoint <= end.present",
        "first_present": selected[0].present,
        "last_present": selected[-1].present,
        "window_count": len(selected),
        "median_interval_ms": _format_decimal(_median_decimal(intervals), 6),
        "median_fps": _format_decimal(_median_decimal(fps_values), 6),
        "median_latency_ms": _format_decimal(_median_decimal(latencies), 6),
        "median_upstream_residual_ms": _format_decimal(
            _median_decimal(residuals), 6
        ),
        "max_pending": max(frame.pending for frame in selected),
        "interpretation": (
            "avg_latency is the submit-to-present renderer component; "
            "max(avg_interval-avg_latency,0) is a descriptive upstream residual"
        ),
        "windows": rows,
    }


def _segment(
    name: str,
    start: Snapshot,
    end: Snapshot,
    frames: Optional[Sequence[FrameWindow]] = None,
) -> Mapping[str, Any]:
    _require(end.sequence > start.sequence, name, "end sequence must follow start sequence")
    elapsed_ns = end.uptime_raw_ns - start.uptime_raw_ns
    _require(elapsed_ns > 0, name, "boundary elapsed time must be positive")
    counters = _counter_delta(start, end)
    reuse = _reuse_delta(start, end)
    compile_excluding_publish = counters["compile_ns"] - counters["publish_ns"]
    _require(compile_excluding_publish >= 0, name, "publish delta exceeds compile delta")
    identity = {
        "compile_ok": counters["compile_ok"],
        "first": counters["identity_first"],
        "second": counters["identity_second"],
        "third": counters["identity_third"],
        "second_per_first_percent": _ratio_percent(
            counters["identity_second"], counters["identity_first"]
        ),
        "third_per_second_percent": _ratio_percent(
            counters["identity_third"], counters["identity_second"]
        ),
        "threshold_2_compile_events": counters["identity_second"],
        "threshold_3_compile_events": counters["identity_third"],
        "threshold_2_minus_3_event_delta": (
            counters["identity_second"] - counters["identity_third"]
        ),
        "interpretation": (
            "cumulative transition deltas may cross a selected boundary; "
            "threshold population evidence is therefore reported for the full run"
        ),
    }
    result: Dict[str, Any] = {
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
        "live_gauges": {
            field: {
                "start": start.counters[field],
                "end": end.counters[field],
                "net_change": end.counters[field] - start.counters[field],
            }
            for field in LIVE_GAUGE_FIELDS
        },
        "fallback_endpoint_gauges": {
            field: {
                "start": start.counters[field],
                "end": end.counters[field],
                "net_change": end.counters[field] - start.counters[field],
            }
            for field in FALLBACK_GAUGE_FIELDS
        },
        "compile_publication": {
            "compile_ns_including_publish": counters["compile_ns"],
            "publish_ns_subset": counters["publish_ns"],
            "compile_excluding_publish_ns": compile_excluding_publish,
            "compile_and_publish_ns_non_overlapping": counters["compile_ns"],
            "compile_and_publish_wall_share_percent": _ratio_percent(
                counters["compile_ns"], elapsed_ns
            ),
            "publish_wall_share_percent": _ratio_percent(
                counters["publish_ns"], elapsed_ns
            ),
            "publish_within_compile_percent": _ratio_percent(
                counters["publish_ns"], counters["compile_ns"]
            ),
            "lower_plus_compile_wall_share_percent": _ratio_percent(
                counters["lower_ns"] + counters["compile_ns"], elapsed_ns
            ),
            "formula": (
                "compile_ns surrounds CompileVerified and already includes publish_ns; "
                "combined non-overlapping compile+publication is therefore compile_ns"
            ),
        },
        "identity_recurrence": identity,
        "fallback_cost": _fallback_analysis(counters, start, end, elapsed_ns),
        "reuse_distance": {
            str(encounter): {
                "total": sum(reuse[encounter].values()),
                "buckets": reuse[encounter],
            }
            for encounter in REUSE_ENCOUNTERS
        },
    }
    if frames is not None:
        result["frame_pipeline_curve"] = _frame_curve(name, start, end, frames)
    return result


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
        "checksum": snapshot.checksum,
        "line_number": snapshot.line_number,
        "counters": snapshot.counters,
        "reuse": {str(key): value for key, value in snapshot.reuse.items()},
    }


def _exact_mapping(
    value: object, expected: Sequence[str], source: object
) -> Mapping[str, Any]:
    _require(type(value) is dict, source, "expected an object")
    assert isinstance(value, dict)
    _require(
        set(value) == set(expected),
        source,
        "object keys do not match the live-binding schema",
    )
    return value


def _binding_uint(value: object, source: object, field: str) -> int:
    _require(
        type(value) is int and 0 <= value <= UINT64_MAX,
        source,
        f"{field} must be uint64",
    )
    assert isinstance(value, int)
    return value


def _binding_bool(value: object, source: object, field: str) -> bool:
    _require(type(value) is bool, source, f"{field} must be boolean")
    assert isinstance(value, bool)
    return value


def _validate_bound_stage(
    profile: ProfileLog,
    scene: str,
    stage: str,
    value: object,
    expected_sequence: int,
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
    receipt = _binding_uint(
        stage_value["receipt_uptime_raw_ns"], source, "receipt_uptime_raw_ns"
    )
    sent = _binding_uint(
        stage_value["signal_sent_uptime_raw_ns"],
        source,
        "signal_sent_uptime_raw_ns",
    )
    observed = _binding_uint(
        stage_value["signal_observed_uptime_raw_ns"],
        source,
        "signal_observed_uptime_raw_ns",
    )
    sequence = _binding_uint(stage_value["sequence"], source, "sequence")
    snapshot_present = _binding_uint(
        stage_value["snapshot_present"], source, "snapshot_present"
    )
    snapshot_uptime = _binding_uint(
        stage_value["snapshot_uptime_raw_ns"], source, "snapshot_uptime_raw_ns"
    )
    checksum = stage_value["snapshot_checksum"]
    _require(
        type(checksum) is str and CHECKSUM_RE.fullmatch(checksum) is not None,
        source,
        "snapshot_checksum is malformed",
    )
    _require(sequence == expected_sequence, source, "unexpected signal sequence")
    snapshot = profile.by_sequence.get(sequence)
    _require(snapshot is not None, source, "bound signal snapshot is absent")
    assert snapshot is not None
    _require(snapshot.reason == "signal", source, "bound snapshot is not a signal")
    _require(
        snapshot.present == snapshot_present
        and snapshot.uptime_raw_ns == snapshot_uptime
        and snapshot.checksum == checksum,
        source,
        "bound signal identity does not match the profile log",
    )
    _require(
        0 < receipt <= sent <= snapshot_uptime <= observed,
        source,
        "receipt/signal/snapshot clocks are not ordered",
    )
    _require(
        _binding_bool(
            stage_value["evidence_verified"], source, "evidence_verified"
        ),
        source,
        "stage evidence was not verified",
    )
    return stage_value


def _validate_live_binding(
    profile: ProfileLog,
    fort_sequences: Tuple[int, int],
    in_game_sequences: Tuple[int, int],
    binding: object,
) -> Mapping[str, Any]:
    root = _exact_mapping(
        binding,
        ("schema", "scope", "checkpoint", "scenes", "lifecycle"),
        "live binding",
    )
    _require(root["schema"] == LIVE_BINDING_SCHEMA, "live binding", "schema mismatch")
    _require(root["scope"] == DIAGNOSTIC_SCOPE, "live binding", "scope mismatch")

    checkpoint = _exact_mapping(
        root["checkpoint"],
        ("count", "terminal", "observed_uptime_raw_ns"),
        "live binding checkpoint",
    )
    _require(
        _binding_uint(checkpoint["count"], "live binding checkpoint", "count") == 1
        and _binding_uint(
            checkpoint["terminal"], "live binding checkpoint", "terminal"
        )
        == 0,
        "live binding checkpoint",
        "p2300 must be exact-once and non-terminal",
    )
    checkpoint_observed = _binding_uint(
        checkpoint["observed_uptime_raw_ns"],
        "live binding checkpoint",
        "observed_uptime_raw_ns",
    )

    scenes = _exact_mapping(root["scenes"], ("fort", "in_game"), "live binding scenes")
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
        anchor = _validate_bound_stage(
            profile, scene, "anchor", scene_value["anchor"], start_sequence
        )
        complete = _validate_bound_stage(
            profile, scene, "complete", scene_value["complete"], end_sequence
        )
        before = _binding_uint(scene_value["endpoint_before"], source, "endpoint_before")
        after = _binding_uint(scene_value["endpoint_after"], source, "endpoint_after")
        discard = _binding_uint(
            scene_value["discard_endpoint"], source, "discard_endpoint"
        )
        measured = _binding_uint(
            scene_value["measured_endpoint"], source, "measured_endpoint"
        )
        _require(
            before == after
            and before > CHECKPOINT_PRESENT
            and before % 120 == 0,
            source,
            "announcement endpoint is not a stable post-p2300 p120 endpoint",
        )
        _require(
            discard == before + 120 and measured == discard + 120,
            source,
            "first-full-p120 geometry mismatch",
        )
        _require(
            discard in frame_map and measured in frame_map,
            source,
            "discard/measurement renderer endpoints are missing",
        )
        interval_text = scene_value["avg_interval_ms"]
        _require(type(interval_text) is str, source, "avg_interval_ms must be a string")
        interval = _parse_decimal(interval_text, source, "avg_interval_ms")
        _require(
            interval > ZERO and interval == frame_map[measured].avg_interval_ms,
            source,
            "stored first-full interval does not match the raw p120 record",
        )
        start_snapshot = profile.by_sequence[start_sequence]
        end_snapshot = profile.by_sequence[end_sequence]
        _require(
            start_snapshot.present <= measured
            and end_snapshot.present >= measured,
            source,
            "signal pair does not bracket the first-full measurement endpoint",
        )
        _require(
            previous_observed
            < anchor["receipt_uptime_raw_ns"]
            <= anchor["signal_observed_uptime_raw_ns"]
            < complete["receipt_uptime_raw_ns"]
            <= complete["signal_observed_uptime_raw_ns"],
            source,
            "scene sidecar clocks are not strictly ordered",
        )
        previous_observed = complete["signal_observed_uptime_raw_ns"]
        for field in (
            "manual_input_only_attested",
            "zero_input_window_attested",
            "oracle_attested",
        ):
            _require(
                _binding_bool(scene_value[field], source, field),
                source,
                f"{field} is false",
            )
        with decimal.localcontext() as context:
            context.prec = 50
            fps = ONE_THOUSAND / interval
        bound_scenes[scene] = {
            **scene_value,
            "derived_fps": _format_decimal(fps, 6),
            "semantics": (
                "L stable; L+120 discarded; L+240 is the first complete "
                "p120 measurement after the manual announcement"
            ),
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
        "live binding lifecycle",
    )
    _require(
        lifecycle["exit_status_bytes"] == "0\n",
        "live binding lifecycle",
        "exit status bytes are not exact 0\\n",
    )
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
        _require(
            _binding_uint(lifecycle[field], "live binding lifecycle", field)
            == expected,
            "live binding lifecycle",
            f"{field} must equal {expected}",
        )
    for field in (
        "final_clean",
        "no_input_before_p2300_attested",
        "no_glitch_attested",
        "evidence_verified",
    ):
        _require(
            _binding_bool(lifecycle[field], "live binding lifecycle", field),
            "live binding lifecycle",
            f"{field} is false",
        )
    _require(
        len(profile.snapshots) == 7
        and [snapshot.reason for snapshot in profile.snapshots[2:-1]]
        == ["signal"] * 4
        and profile.snapshots[-1].sequence == 7,
        "live binding lifecycle",
        "profile stream is not start/checkpoint/four-signals/final exactly",
    )
    _require(
        previous_observed < profile.snapshots[-1].uptime_raw_ns,
        "live binding lifecycle",
        "clean final snapshot does not follow the last sidecar observation",
    )
    return {
        "schema": LIVE_BINDING_SCHEMA,
        "scope": DIAGNOSTIC_SCOPE,
        "checkpoint": checkpoint,
        "scenes": bound_scenes,
        "lifecycle": lifecycle,
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
        _require(pair[0] < pair[1], name, "start sequence must precede end sequence")
        for sequence in pair:
            _require(
                snapshots[sequence].reason == "signal",
                name,
                f"sequence {sequence} is not a SIGUSR2 snapshot",
            )
    _require(
        checkpoint.sequence
        < fort_sequences[0]
        < fort_sequences[1]
        < in_game_sequences[0]
        < in_game_sequences[1]
        < final.sequence,
        "boundaries",
        "expected checkpoint < Fort start/end < in-game start/end < final",
    )

    segments = {
        "loading": _segment("loading", first, checkpoint, profile.frames),
        "fort": _segment(
            "fort",
            snapshots[fort_sequences[0]],
            snapshots[fort_sequences[1]],
            profile.frames,
        ),
        "in_game": _segment(
            "in_game",
            snapshots[in_game_sequences[0]],
            snapshots[in_game_sequences[1]],
            profile.frames,
        ),
    }
    full = _segment("full_run", first, final)
    full_counters = full["counters"]
    retired_once = full_counters["retired_once"]
    retired_twice = full_counters["retired_twice"]
    live_once = final.counters["live_once"]
    live_twice = final.counters["live_twice"]
    never_second = retired_once + live_once
    second_without_third = retired_twice + live_twice
    threshold_evidence = {
        "automatic_election": False,
        "compile_ok": full_counters["compile_ok"],
        "identity_first": full_counters["identity_first"],
        "identity_second": full_counters["identity_second"],
        "identity_third": full_counters["identity_third"],
        "second_per_first_percent": _ratio_percent(
            full_counters["identity_second"], full_counters["identity_first"]
        ),
        "third_per_second_percent": _ratio_percent(
            full_counters["identity_third"], full_counters["identity_second"]
        ),
        "observed_second_without_third": (
            full_counters["identity_second"] - full_counters["identity_third"]
        ),
        "retired_after_initial_compile_only": retired_once,
        "retired_after_second_before_third": retired_twice,
        "live_after_initial_compile_only": live_once,
        "live_after_second_before_third": live_twice,
        "threshold_2_avoided_compile_observed_population": never_second,
        "threshold_3_avoided_compile_observed_population": (
            never_second + second_without_third
        ),
        "threshold_3_incremental_avoidance_observed_population_vs_2": (
            second_without_third
        ),
        "limitation": (
            "the consistent final snapshot covers retired and still-live positive "
            "identities, but recurrence alone does not elect encounter 2 or 3"
        ),
    }

    frame_checkpoint = profile.checkpoint

    validated_binding = (
        _validate_live_binding(
            profile, fort_sequences, in_game_sequences, live_binding
        )
        if live_binding is not None
        else None
    )
    diagnostic_gate: Mapping[str, Any]
    if validated_binding is None:
        diagnostic_gate = {
            "passed": False,
            "status": "LIFECYCLE_UNBOUND",
            "scope": DIAGNOSTIC_SCOPE,
            "persistent_baseline_eligible": False,
            "interpretation": (
                "offline profile arithmetic only; use phase6ck_profile_live.py "
                "seal to bind manual receipts, evidence, signals, and lifecycle"
            ),
        }
    else:
        diagnostic_gate = {
            "passed": True,
            "status": "PASS_DIAGNOSTIC_ONLY",
            "scope": DIAGNOSTIC_SCOPE,
            "persistent_baseline_eligible": False,
            "interpretation": (
                "phase-6CK profiling evidence is lifecycle-bound and clean; it is "
                "not a persistent performance baseline or promotion record"
            ),
        }

    return {
        "schema": {"name": REPORT_SCHEMA_NAME, "version": REPORT_SCHEMA_VERSION},
        "valid": True,
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
                "semantics": "start -> checkpoint(p2300)",
            },
            "fort": {
                "start_sequence": fort_sequences[0],
                "end_sequence": fort_sequences[1],
                "semantics": "selected SIGUSR2 -> selected SIGUSR2",
            },
            "in_game": {
                "start_sequence": in_game_sequences[0],
                "end_sequence": in_game_sequences[1],
                "semantics": "selected SIGUSR2 -> selected SIGUSR2",
            },
        },
        "snapshots": [_snapshot_json(snapshot) for snapshot in profile.snapshots],
        "segments": segments,
        "full_run": full,
        "threshold_2_vs_3_evidence": threshold_evidence,
        "live_binding": validated_binding,
        "diagnostic_gate": diagnostic_gate,
    }


def _value_or_na(value: Optional[str]) -> str:
    return value if value is not None else "n/a"


def _render_table(report: Mapping[str, Any]) -> str:
    gate = report["diagnostic_gate"]
    lines = [
        (
            "PHASE-6CK DIAGNOSTIC GATE: PASS "
            "(DIAGNOSTIC ONLY; NOT PERSISTENT BASELINE)"
            if gate["passed"]
            else "PHASE-6CK PROFILE REPORT: PASS "
            "(OFFLINE ONLY; LIFECYCLE UNBOUND)"
        ),
        f"log={report['source']['log']}",
        f"sha256={report['source']['sha256']}",
        "",
        "SEGMENT DELTAS",
        (
            "segment      seq       present          elapsed ns  compile+publish/wall  "
            "publish/compile     second/first   third/second"
        ),
    ]
    for name in ("loading", "fort", "in_game"):
        segment = report["segments"][name]
        compile_publication = segment["compile_publication"]
        identity = segment["identity_recurrence"]
        lines.append(
            f"{name:<11} "
            f"{segment['start']['sequence']:>3}->{segment['end']['sequence']:<3} "
            f"p{segment['start']['present']:<5}->p{segment['end']['present']:<5} "
            f"{segment['elapsed_ns']:>15} "
            f"{_value_or_na(compile_publication['compile_and_publish_wall_share_percent']):>21}% "
            f"{_value_or_na(compile_publication['publish_within_compile_percent']):>16}% "
            f"{_value_or_na(identity['second_per_first_percent']):>16}% "
            f"{_value_or_na(identity['third_per_second_percent']):>14}%"
        )
        curve = segment["frame_pipeline_curve"]
        lines.append(
            " " * 12
            + f"renderer p{curve['first_present']}..p{curve['last_present']} "
            + f"windows={curve['window_count']} median interval={curve['median_interval_ms']}ms "
            + f"latency={curve['median_latency_ms']}ms "
            + f"upstream={curve['median_upstream_residual_ms']}ms "
            + f"fps={curve['median_fps']} pending_max={curve['max_pending']}"
        )
        fallback = segment["fallback_cost"]
        lines.append(
            " " * 12
            + "fallback estimated elapsed ordinary/precise/combined="
            + f"{_value_or_na(fallback['ordinary']['estimated_elapsed_share_percent'])}%/"
            + f"{_value_or_na(fallback['precise']['estimated_elapsed_share_percent'])}%/"
            + f"{_value_or_na(fallback['combined_estimated_elapsed_share_percent'])}% "
            + "unflushed_bound(start/end)="
            + f"{fallback['endpoint_unflushed_uncertainty']['start_blocks_upper_bound']}/"
            + f"{fallback['endpoint_unflushed_uncertainty']['end_blocks_upper_bound']}"
        )
        if gate["passed"] and name in ("fort", "in_game"):
            bound = report["live_binding"]["scenes"][name]
            lines.append(
                " " * 12
                + f"announcement L=p{bound['endpoint_before']} "
                + f"discard=p{bound['discard_endpoint']} "
                + f"measure=p{bound['measured_endpoint']} "
                + f"interval={bound['avg_interval_ms']}ms "
                + f"fps={bound['derived_fps']}"
            )

    evidence = report["threshold_2_vs_3_evidence"]
    lines.extend(
        [
            "",
            "THRESHOLD 2 VS 3 (FULL-RUN EVIDENCE; NO AUTOMATIC ELECTION)",
            f"compile_ok={evidence['compile_ok']} first={evidence['identity_first']} "
            f"second={evidence['identity_second']} third={evidence['identity_third']}",
            (
                "second/first="
                f"{_value_or_na(evidence['second_per_first_percent'])}% "
                "third/second="
                f"{_value_or_na(evidence['third_per_second_percent'])}%"
            ),
            (
                f"once retired/live={evidence['retired_after_initial_compile_only']}/"
                f"{evidence['live_after_initial_compile_only']} "
                f"twice retired/live={evidence['retired_after_second_before_third']}/"
                f"{evidence['live_after_second_before_third']}"
            ),
            (
                "avoided-compile observed population: "
                "threshold2="
                f"{evidence['threshold_2_avoided_compile_observed_population']} "
                "threshold3="
                f"{evidence['threshold_3_avoided_compile_observed_population']} "
                "incremental3vs2="
                f"{evidence['threshold_3_incremental_avoidance_observed_population_vs_2']}"
            ),
        ]
    )
    return "\n".join(lines)


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


def _sequence_pair(value: str) -> Tuple[int, int]:
    match = re.fullmatch(r"([^:]+):([^:]+)", value)
    if match is None:
        raise argparse.ArgumentTypeError("expected canonical START:END sequences")
    try:
        start = _parse_uint(match.group(1), "argument", "start sequence")
        end = _parse_uint(match.group(2), "argument", "end sequence")
    except ReportError as error:
        raise argparse.ArgumentTypeError(str(error)) from error
    if start == 0 or end == 0 or start >= end:
        raise argparse.ArgumentTypeError("sequences must be positive with START < END")
    return start, end


def _fixture_counters(scale: int) -> Mapping[str, int]:
    second_values = (0, 6, 7, 9, 10, 12, 13)
    third_values = (0, 4, 5, 6, 7, 8, 9)
    retired_once_values = (0, 1, 1, 1, 2, 2, 3)
    retired_twice_values = (0, 0, 1, 1, 1, 2, 2)
    retired_three_values = (0, 0, 0, 1, 1, 1, 1)
    compiles = (0, 10, 12, 15, 17, 20, 22)[scale]
    second = second_values[scale]
    third = third_values[scale]
    retired_once = retired_once_values[scale]
    retired_twice = retired_twice_values[scale]
    retired_three = retired_three_values[scale]
    live_once = compiles - second - retired_once
    live_twice = second - third - retired_twice
    live_three = third - retired_three
    reset_identity = retired_once + retired_twice + retired_three
    return {
        "lower_attempts": compiles + scale,
        "lower_ok": compiles,
        "lower_fail": scale,
        "lower_ns": compiles * 5_000,
        "compile_attempts": compiles,
        "compile_ok": compiles,
        "compile_capacity": 0,
        "compile_fail": 0,
        "compile_ns": compiles * 10_000,
        "staging_ns": compiles * 2_000,
        "emit_ns": compiles * 3_000,
        "allocate_ns": compiles * 1_000,
        "publish_ns": compiles * 2_000,
        "code_bytes": compiles * 64,
        "code_capacity": compiles * 128,
        "identity_first": compiles,
        "identity_second": second,
        "identity_third": third,
        "retired_once": retired_once,
        "retired_twice": retired_twice,
        "retired_three_plus": retired_three,
        "live_once": live_once,
        "live_twice": live_twice,
        "live_three_plus": live_three,
        "bypasses": scale * 2,
        "capacity_fallbacks": 0,
        "fallback_sample_interval": FALLBACK_SAMPLE_INTERVAL,
        "fallback_batch_size": FALLBACK_BATCH_SIZE,
        "fallback_active_owners": 0 if scale == 0 else 1,
        "fallback_unflushed_blocks_bound": 0 if scale == 0 else 510,
        "fallback_ordinary_blocks": scale * 256,
        "fallback_ordinary_steps": scale * 512,
        "fallback_ordinary_samples": scale,
        "fallback_ordinary_sample_steps": scale * 2,
        "fallback_ordinary_sample_ns": scale * 1_000,
        "fallback_ordinary_flushes": scale,
        "fallback_precise_blocks": scale * 256,
        "fallback_precise_steps": scale * 768,
        "fallback_precise_samples": scale,
        "fallback_precise_sample_steps": scale * 3,
        "fallback_precise_sample_ns": scale * 1_500,
        "fallback_precise_flushes": scale,
        "reset_other": 0,
        "reset_identity": reset_identity,
        "reset_capacity": 0,
        "reset_eviction": 0,
        "reset_cache": 0,
        "reset_busy": 0,
        "invalid": 0,
    }


def _fixture_reuse(counters: Mapping[str, int]) -> Mapping[int, Mapping[str, int]]:
    values = {
        2: counters["identity_second"],
        3: counters["identity_third"],
    }
    return {
        encounter: {
            field: values[encounter] if field == "d1" else 0
            for field in REUSE_BUCKET_FIELDS
        }
        for encounter in REUSE_ENCOUNTERS
    }


def _fixture_snapshot_lines(
    sequence: int, reason: str, present: int, scale: int
) -> List[str]:
    counters = _fixture_counters(scale)
    reuse = _fixture_reuse(counters)
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
    }
    profile["checksum"] = _snapshot_checksum(profile, reuse)
    profile_line = PROFILE_PREFIX + " ".join(
        f"{field}={profile[field]}" for field in PROFILE_FIELDS
    )

    def reuse_value(field: str, encounter: int) -> int:
        if field == "version":
            return 1
        if field == "sequence":
            return sequence
        if field == "encounter":
            return encounter
        return reuse[encounter][field]

    reuse_lines = [
        REUSE_PREFIX
        + " ".join(f"{field}={reuse_value(field, encounter)}" for field in REUSE_FIELDS)
        for encounter in REUSE_ENCOUNTERS
    ]
    return [profile_line, *reuse_lines]


def _fixture_log() -> str:
    config = CONFIG_PREFIX + " ".join(
        f"{field}={CONFIG_EXPECTED[field]}" for field in CONFIG_FIELDS
    )
    lines = ["fixture noise", config]
    lines.extend(_fixture_snapshot_lines(1, "start", 0, 0))
    lines.append(
        "[FRAME_PIPELINE] present=1 avg_interval=0.000 ms "
        "avg_latency=1.000 ms pending=0 index=0 arg=0"
    )
    for present in range(120, 4081, 120):
        lines.append(
            f"[FRAME_PIPELINE] present={present} avg_interval=10.000 ms "
            "avg_latency=2.000 ms pending=0 index=1 arg=0"
        )
        if present == 2280:
            lines.append(
                "[FRAME_CHECKPOINT] present=2300 target=2300 "
                "cumulative_ms=22990.000 cumulative_intervals=2299 "
                "partial_ms=200.000 partial_intervals=20 terminal=0"
            )
            lines.extend(_fixture_snapshot_lines(2, "checkpoint", 2300, 1))
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
    def stage(
        sequence: int, receipt: int, sent: int, observed: int
    ) -> Mapping[str, Any]:
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

    def scene(
        anchor: Mapping[str, Any],
        complete: Mapping[str, Any],
        endpoint: int,
    ) -> Mapping[str, Any]:
        return {
            "anchor": anchor,
            "complete": complete,
            "endpoint_before": endpoint,
            "endpoint_after": endpoint,
            "discard_endpoint": endpoint + 120,
            "measured_endpoint": endpoint + 240,
            "avg_interval_ms": "10.000",
            "manual_input_only_attested": True,
            "zero_input_window_attested": True,
            "oracle_attested": True,
        }

    return {
        "schema": LIVE_BINDING_SCHEMA,
        "scope": DIAGNOSTIC_SCOPE,
        "checkpoint": {
            "count": 1,
            "terminal": 0,
            "observed_uptime_raw_ns": 2_100_000,
        },
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


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="phase6ck-profile-report-self-test-") as temporary:
        root = pathlib.Path(temporary)
        valid_path = root / "valid.log"
        valid_text = _fixture_log()
        valid_path.write_text(valid_text, encoding="utf-8")
        profile = parse_log(valid_path)
        report = build_report(profile, (3, 4), (5, 6))
        _require(report["valid"] is True, "self-test", "positive report is not valid")
        _require(
            report["segments"]["loading"]["compile_publication"][
                "compile_and_publish_wall_share_percent"
            ]
            == "10.000000",
            "self-test",
            "loading compile share mismatch",
        )
        _require(
            report["threshold_2_vs_3_evidence"][
                "threshold_3_incremental_avoidance_observed_population_vs_2"
            ]
            == 4,
            "self-test",
            "threshold evidence mismatch",
        )
        _require(
            report["segments"]["fort"]["frame_pipeline_curve"][
                "median_upstream_residual_ms"
            ]
            == "8.000000"
            and report["segments"]["in_game"]["frame_pipeline_curve"][
                "median_latency_ms"
            ]
            == "2.000000"
            and report["frame_trace"]["checkpoint"]["terminal"] == 0,
            "self-test",
            "renderer/upstream curve mismatch",
        )
        _require(
            report["segments"]["loading"]["fallback_cost"][
                "combined_estimated_elapsed_share_percent"
            ]
            == "64.000000"
            and report["segments"]["loading"]["fallback_cost"][
                "endpoint_unflushed_uncertainty"
            ]["end_blocks_upper_bound"]
            == 510,
            "self-test",
            "sampled fallback cost or unflushed bound mismatch",
        )
        json.dumps(report, sort_keys=True)
        output_path = root / "profile-report.json"
        report_payload = _serialize_report(report)
        _publish_report_exclusive(output_path, report_payload)
        _require(
            output_path.read_bytes() == report_payload
            and stat.S_IMODE(output_path.stat().st_mode) == 0o444,
            "self-test",
            "authoritative profile report bytes or mode mismatch",
        )
        _expect_failure(
            lambda: _publish_report_exclusive(output_path, b"overwrite\n"),
            "an overwrite of an archived profile report",
        )
        _require(
            output_path.read_bytes() == report_payload,
            "self-test",
            "failed overwrite changed the archived profile report",
        )
        _require(
            "NO AUTOMATIC ELECTION" in _render_table(report)
            and "renderer p" in _render_table(report),
            "self-test",
            "table omits renderer evidence or implies an automatic election",
        )
        _require(
            report["diagnostic_gate"]["status"] == "LIFECYCLE_UNBOUND"
            and report["diagnostic_gate"]["passed"] is False
            and "LIFECYCLE UNBOUND" in _render_table(report),
            "self-test",
            "raw arithmetic was presented as a live diagnostic gate",
        )

        live_binding = _fixture_live_binding(profile)
        bound_report = build_report(
            profile, (3, 4), (5, 6), live_binding=live_binding
        )
        _require(
            bound_report["diagnostic_gate"]["status"]
            == "PASS_DIAGNOSTIC_ONLY"
            and bound_report["diagnostic_gate"]["passed"] is True
            and bound_report["diagnostic_gate"]["persistent_baseline_eligible"]
            is False
            and bound_report["live_binding"]["scenes"]["fort"]["derived_fps"]
            == "100.000000"
            and "NOT PERSISTENT BASELINE" in _render_table(bound_report),
            "self-test",
            "lifecycle-bound diagnostic result is missing or overclaims baseline use",
        )
        bad_live_binding = json.loads(json.dumps(live_binding))
        bad_live_binding["scenes"]["fort"]["anchor"]["snapshot_checksum"] = (
            "0000000000000000"
        )
        _expect_failure(
            lambda: build_report(
                profile, (3, 4), (5, 6), live_binding=bad_live_binding
            ),
            "a sidecar-to-snapshot checksum mismatch",
        )

        checksum_path = root / "bad-checksum.log"
        checksum_path.write_text(
            valid_text.replace("checksum=", "checksum=0", 1), encoding="utf-8"
        )
        _expect_failure(lambda: parse_log(checksum_path), "a malformed checksum")

        sequence_gap_path = root / "sequence-gap.log"
        sequence_gap_path.write_text(
            "\n".join(
                line
                for line in valid_text.splitlines()
                if not (
                    line.startswith(MARKER_PREFIX) and "sequence=3 " in line
                )
            )
            + "\n",
            encoding="utf-8",
        )
        _expect_failure(
            lambda: parse_log(sequence_gap_path), "a PROFILE sequence gap"
        )

        bad_reason_path = root / "bad-reason.log"
        bad_reason_path.write_text(
            valid_text.replace("reason=signal", "reason=manual", 1),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(bad_reason_path), "an unknown reason")

        pending_path = root / "pending-request.log"
        pending_path.write_text(
            valid_text.replace("pending_request=0", "pending_request=1", 1),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: parse_log(pending_path), "an uncleared snapshot request"
        )

        missing_reuse_path = root / "missing-reuse.log"
        missing_reuse_path.write_text(
            "\n".join(
                line
                for line in valid_text.splitlines()
                if not (
                    line.startswith(REUSE_PREFIX)
                    and "sequence=3 " in line
                    and "encounter=3 " in line
                )
            )
            + "\n",
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(missing_reuse_path), "a missing REUSE record")

        invalid_path = root / "invalid.log"
        invalid_path.write_text(
            valid_text.replace("final=0 valid=1", "final=0 valid=0", 1),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(invalid_path), "an invalid snapshot")

        bad_config_path = root / "bad-config.log"
        bad_config_path.write_text(
            valid_text.replace("production_shape=1", "production_shape=0", 1),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(bad_config_path), "a non-production shape")

        inconsistent_path = root / "inconsistent.log"
        inconsistent_path.write_text(
            valid_text.replace(
                "accounting_consistent=1", "accounting_consistent=0", 1
            ),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: parse_log(inconsistent_path), "an inconsistent identity snapshot"
        )

        checkpoint_line = next(
            line
            for line in valid_text.splitlines()
            if line.startswith(CHECKPOINT_PREFIX)
        )
        duplicate_checkpoint_path = root / "duplicate-checkpoint.log"
        duplicate_checkpoint_path.write_text(
            valid_text + checkpoint_line + "\n", encoding="utf-8"
        )
        _expect_failure(
            lambda: parse_log(duplicate_checkpoint_path), "a duplicate checkpoint"
        )

        terminal_checkpoint_path = root / "terminal-checkpoint.log"
        terminal_checkpoint_path.write_text(
            valid_text.replace("partial_intervals=20 terminal=0", "partial_intervals=20 terminal=1"),
            encoding="utf-8",
        )
        _expect_failure(
            lambda: parse_log(terminal_checkpoint_path), "a terminal checkpoint"
        )

        frame_limit_path = root / "frame-limit.log"
        frame_limit_path.write_text(
            valid_text
            + "[FRAME_LIMIT] configured limit=4200 domain=present action=SDL_QUIT\n",
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(frame_limit_path), "a FRAME_LIMIT marker")

        missing_frame_path = root / "missing-frame.log"
        missing_frame_path.write_text(
            "\n".join(
                line
                for line in valid_text.splitlines()
                if not line.startswith("[FRAME_PIPELINE] present=3120 ")
            )
            + "\n",
            encoding="utf-8",
        )
        _expect_failure(
            lambda: parse_log(missing_frame_path), "a missing p120 renderer window"
        )

        bad_fallback = dict(profile.snapshots[1].counters)
        bad_fallback["fallback_ordinary_samples"] = (
            bad_fallback["fallback_ordinary_blocks"] + 1
        )
        _expect_failure(
            lambda: _validate_snapshot_equations(
                replace(profile.snapshots[1], counters=bad_fallback),
                "self-test bad fallback",
            ),
            "fallback samples exceeding flushed blocks",
        )

        bad_bound = dict(profile.snapshots[1].counters)
        bad_bound["fallback_unflushed_blocks_bound"] -= 1
        _expect_failure(
            lambda: _validate_snapshot_equations(
                replace(profile.snapshots[1], counters=bad_bound),
                "self-test bad fallback bound",
            ),
            "an incorrect unflushed fallback bound",
        )

        bad_present_snapshots = list(profile.snapshots)
        bad_present_snapshots[2] = replace(
            bad_present_snapshots[2], present=CHECKPOINT_PRESENT
        )
        _expect_failure(
            lambda: _validate_stream(tuple(bad_present_snapshots), "self-test"),
            "a non-post-checkpoint signal present",
        )

        _expect_failure(
            lambda: build_report(profile, (2, 4), (5, 6)),
            "a non-SIGUSR2 Fort boundary",
        )
        _expect_failure(
            lambda: build_report(profile, (5, 6), (3, 4)),
            "out-of-order named boundaries",
        )


def _main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "validate phase-6CK CONFIG/PROFILE/REUSE v1 and subtract loading, "
            "Fort Mode, and in-game cumulative counters; publish JSON O_EXCL "
            "with --output and write the table to stderr"
        )
    )
    parser.add_argument("log", nargs="?", metavar="STDERR.log")
    parser.add_argument(
        "--fort",
        type=_sequence_pair,
        metavar="START:END",
        help="two selected post-p2300 SIGUSR2 snapshot sequences",
    )
    parser.add_argument(
        "--ingame",
        type=_sequence_pair,
        metavar="START:END",
        help="two selected post-Fort SIGUSR2 snapshot sequences",
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        metavar="REPORT.json",
        help="new authoritative report path (must not already exist)",
    )
    parser.add_argument(
        "--no-stdout",
        action="store_true",
        help="archive and print the table without mirroring JSON to stdout",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic positive and fail-closed synthetic fixtures",
    )
    arguments = parser.parse_args(argv)

    if arguments.self_test:
        if (
            arguments.log is not None
            or arguments.fort is not None
            or arguments.ingame is not None
            or arguments.output is not None
            or arguments.no_stdout
        ):
            parser.error("--self-test does not accept paths, boundaries, or output options")
        self_test()
        print("phase6ck-profile-report-self-test: PASS")
        return 0

    if arguments.log is None:
        parser.error("STDERR.log is required")
    if arguments.fort is None or arguments.ingame is None:
        parser.error("--fort START:END and --ingame START:END are required")
    if arguments.output is None:
        parser.error("--output REPORT.json is required to archive the profile report")
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
        print(f"phase6ck-profile-report: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
