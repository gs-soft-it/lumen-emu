#!/usr/bin/env python3
"""Validate one fail-closed phase-6CO snapshot-elision diagnostic.

This tool is an adapter over the internal phase-6CM report primitives.  It
does not import an external parser or alter the sealed 6CM tool.  The core
contract is intentionally declared here as ordered fields so the producer and
consumer cannot silently drift:

* ``[A64_SNAPSHOT_PHASE6CO_CONFIG]`` proves an exact-one requested/effective
  diagnostic, the 6CL production shape and stable-negative route, a contextual
  production translator with an explicit ``non_reentrant`` capability,
  diagnostic history off, zero skipped operation snapshots, a positive clock
  floor, and the fixed five-boundary sequence that forms four timing buckets.
* ``[A64_SNAPSHOT_PHASE6CO_PROFILE]`` uses the phase-6CM lifecycle header,
  bounded 256-block windows, candidate-only counters and five monotonic clock
  boundaries forming four timing buckets, the exact ``candidate_wall_ns``
  closure, a conservative active-window bound, and an FNV-1a checksum over
  every ordered field except ``checksum`` itself.

The diagnostic never authorizes elision in the measured run.  Election is
count-only, requires both manually bound scenes, and fails closed on endpoint
uncertainty, a candidate reentry, a skipped snapshot, malformed accounting,
overflow, saturation, or an unbound lifecycle.  It is never baseline evidence.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import stat
import sys
import tempfile
from typing import Any, Callable, Dict, Mapping, Optional, Sequence, Tuple

import phase6cm_profile_report as _base


REPORT_SCHEMA_NAME = "lumen.phase6co-profile-report"
REPORT_SCHEMA_VERSION = 1
LIVE_BINDING_SCHEMA = "lumen.phase6co-profile-live-binding/1"
DIAGNOSTIC_SCOPE = "diagnostic_only_not_persistent_baseline"
PROFILE_VERSION = 1
CHECKPOINT_PRESENT = 2300
WINDOW_SIZE = 120
SAMPLE_INTERVAL = 65536
SAMPLE_WINDOW_BLOCKS = 256
MATERIAL_MIN_SAMPLES = 10000
MATERIAL_MIN_BLOCK_BP = 100
MATERIAL_MIN_STEP_BP = 100
UINT64_MAX = _base.UINT64_MAX
CHECKSUM_RE = _base.CHECKSUM_RE
REPORT_ENGINE_PATH = pathlib.Path(_base.__file__).resolve()

CONFIG_PREFIX = "[A64_SNAPSHOT_PHASE6CO_CONFIG] "
PROFILE_PREFIX = "[A64_SNAPSHOT_PHASE6CO_PROFILE] "
MARKER_PREFIX = "[A64_SNAPSHOT_PHASE6CO_"

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
    "material_min_samples",
    "material_min_block_bp",
    "material_min_step_bp",
    "stable_negative_route",
    "translator_contextual",
    "translator_capability",
    "translator_non_reentrant",
    "diagnostic_history",
    "operation_snapshot_skipped",
    "clock_floor_ns",
    "timing_sequence",
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
    "material_min_samples": MATERIAL_MIN_SAMPLES,
    "material_min_block_bp": MATERIAL_MIN_BLOCK_BP,
    "material_min_step_bp": MATERIAL_MIN_STEP_BP,
    "stable_negative_route": 1,
    "translator_contextual": 1,
    "translator_capability": "non_reentrant",
    "translator_non_reentrant": 1,
    "diagnostic_history": 0,
    "operation_snapshot_skipped": 0,
    # A live value is calibrated before writers become effective.  One is a
    # deterministic fixture placeholder; parsing requires strict positivity.
    "clock_floor_ns": 1,
    "timing_sequence": "snapshot_copy_code_view_write_guard_setup_execute",
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
WINDOW_COUNTER_FIELDS = (
    "windows_scheduled",
    "windows_complete",
    "windows_hle_excluded",
    "windows_reentrant_excluded",
    "windows_dropped",
    "windows_active",
)
CANDIDATE_COUNTER_FIELDS = (
    "candidate_reentry_events",
    "sampled_blocks",
    "sampled_steps",
    "candidate_blocks",
    "candidate_steps",
    "candidate_known_ops",
    "candidate_guest_memory_ops",
    "candidate_wall_ns",
    "candidate_snapshot_copy_ns",
    "candidate_code_view_ns",
    "candidate_write_guard_setup_ns",
    "candidate_execute_ns",
    "operation_snapshot_skipped",
)
HEALTH_COUNTER_FIELDS = ("invalid", "saturated", "active_owners")
COUNTER_FIELDS = WINDOW_COUNTER_FIELDS + CANDIDATE_COUNTER_FIELDS + HEALTH_COUNTER_FIELDS
TIMING_COMPONENT_FIELDS = (
    "candidate_snapshot_copy_ns",
    "candidate_code_view_ns",
    "candidate_write_guard_setup_ns",
    "candidate_execute_ns",
)
DERIVED_FIELDS = ("sampled_unflushed_blocks_bound",)
PROFILE_FIELDS = PROFILE_HEADER_FIELDS + COUNTER_FIELDS + DERIVED_FIELDS + ("checksum",)
GAUGE_FIELDS = ("windows_active", "active_owners", "sampled_unflushed_blocks_bound")
CUMULATIVE_COUNTER_FIELDS = tuple(
    field for field in COUNTER_FIELDS if field not in ("windows_active", "active_owners")
)

ReportError = _base.ReportError
Snapshot = _base.Snapshot
FrameWindow = _base.FrameWindow
FrameCheckpoint = _base.FrameCheckpoint
ProfileLog = _base.ProfileLog


def _require(condition: bool, source: object, message: str) -> None:
    _base._require(condition, source, message)


def _checked_sum(values: Sequence[int], source: object, label: str) -> int:
    total = 0
    for value in values:
        _require(0 <= value <= UINT64_MAX - total, source, f"{label} overflows uint64")
        total += value
    return total


def _checked_product(left: int, right: int, source: object, label: str) -> int:
    _require(
        0 <= left <= UINT64_MAX and 0 <= right <= UINT64_MAX,
        source,
        f"{label} operand is outside uint64",
    )
    _require(right == 0 or left <= UINT64_MAX // right, source, f"{label} overflows uint64")
    return left * right


def _parse_config(line: str, source: object) -> Mapping[str, Any]:
    raw = _base._parse_ordered_fields(line, CONFIG_PREFIX, CONFIG_FIELDS, source)
    string_fields = {"signal", "translator_capability", "timing_sequence"}
    parsed: Dict[str, Any] = {
        field: raw[field]
        if field in string_fields
        else _base._parse_uint(raw[field], source, field)
        for field in CONFIG_FIELDS
    }
    for field, expected in CONFIG_EXPECTED.items():
        if field == "clock_floor_ns":
            continue
        _require(parsed[field] == expected, source, f"{field} must equal {expected!r}")
    _require(parsed["clock_floor_ns"] > 0, source, "clock_floor_ns must be positive")
    return parsed


def _validate_candidate_equations(values: Mapping[str, int], source: object) -> None:
    sampled_blocks = values["sampled_blocks"]
    sampled_steps = values["sampled_steps"]
    candidate_blocks = values["candidate_blocks"]
    candidate_steps = values["candidate_steps"]
    _require(
        sampled_steps <= _checked_product(sampled_blocks, 16, source, "sampled step bound"),
        source,
        "sampled steps exceed the 16-op FastBlock bound",
    )
    _require(candidate_blocks <= sampled_blocks, source, "candidate blocks exceed sampled blocks")
    _require(candidate_steps <= sampled_steps, source, "candidate steps exceed sampled steps")
    _require(
        candidate_blocks <= candidate_steps
        <= _checked_product(candidate_blocks, 16, source, "candidate step bound"),
        source,
        "candidate steps are outside the complete 1..16-op block bound",
    )
    _require(
        values["candidate_known_ops"] == candidate_steps,
        source,
        "candidate known-op accounting does not equal candidate_steps",
    )
    _require(
        candidate_blocks <= values["candidate_guest_memory_ops"] <= candidate_steps,
        source,
        "candidate guest-memory ops do not prove at least one per complete block",
    )
    component_total = _checked_sum(
        [values[field] for field in TIMING_COMPONENT_FIELDS], source, "candidate timing closure"
    )
    _require(
        component_total == values["candidate_wall_ns"],
        source,
        "candidate timings do not close candidate_wall_ns exactly",
    )
    _require(
        values["operation_snapshot_skipped"] == 0,
        source,
        "operation_snapshot_skipped must remain hard-zero",
    )
    if candidate_blocks == 0:
        for field in (
            "candidate_steps",
            "candidate_known_ops",
            "candidate_guest_memory_ops",
            "candidate_wall_ns",
            *TIMING_COMPONENT_FIELDS,
        ):
            _require(values[field] == 0, source, f"zero candidate blocks carry nonzero {field}")


def _validate_snapshot_equations(snapshot: Snapshot, source: object) -> None:
    counters = snapshot.counters
    accounted = _checked_sum(
        [
            counters["windows_complete"],
            counters["windows_hle_excluded"],
            counters["windows_reentrant_excluded"],
            counters["windows_dropped"],
            counters["windows_active"],
        ],
        source,
        "window scheduling accounting",
    )
    _require(counters["windows_scheduled"] == accounted, source, "window scheduling accounting mismatch")
    _require(counters["windows_active"] <= counters["active_owners"], source, "active windows exceed active owners")
    expected_bound = _checked_product(
        counters["windows_active"], SAMPLE_WINDOW_BLOCKS, source, "active-window bound"
    )
    _require(
        snapshot.derived["sampled_unflushed_blocks_bound"] == expected_bound,
        source,
        "sampled unflushed bound mismatch",
    )
    expected_blocks = _checked_product(
        counters["windows_complete"], SAMPLE_WINDOW_BLOCKS, source, "complete-window block count"
    )
    _require(counters["sampled_blocks"] == expected_blocks, source, "complete-window/sample-block accounting mismatch")
    _validate_candidate_equations(counters, source)


def _configure_base_parser() -> None:
    # phase6CM's generic ordered-marker, frame, lifecycle and publication
    # machinery resolves module globals at call time.  Rebind only its parser
    # contract in this process; the source file and sealed 6CM behavior remain
    # untouched.
    bindings = {
        "REPORT_SCHEMA_NAME": REPORT_SCHEMA_NAME,
        "REPORT_SCHEMA_VERSION": REPORT_SCHEMA_VERSION,
        "LIVE_BINDING_SCHEMA": LIVE_BINDING_SCHEMA,
        "DIAGNOSTIC_SCOPE": DIAGNOSTIC_SCOPE,
        "PROFILE_VERSION": PROFILE_VERSION,
        "CHECKPOINT_PRESENT": CHECKPOINT_PRESENT,
        "WINDOW_SIZE": WINDOW_SIZE,
        "SAMPLE_INTERVAL": SAMPLE_INTERVAL,
        "SAMPLE_WINDOW_BLOCKS": SAMPLE_WINDOW_BLOCKS,
        "MATERIAL_MIN_SAMPLES": MATERIAL_MIN_SAMPLES,
        "MATERIAL_MIN_BLOCK_BP": MATERIAL_MIN_BLOCK_BP,
        "MATERIAL_MIN_STEP_BP": MATERIAL_MIN_STEP_BP,
        "CONFIG_PREFIX": CONFIG_PREFIX,
        "PROFILE_PREFIX": PROFILE_PREFIX,
        "MARKER_PREFIX": MARKER_PREFIX,
        "CONFIG_FIELDS": CONFIG_FIELDS,
        "CONFIG_EXPECTED": CONFIG_EXPECTED,
        "PROFILE_HEADER_FIELDS": PROFILE_HEADER_FIELDS,
        "COUNTER_FIELDS": COUNTER_FIELDS,
        "DERIVED_FIELDS": DERIVED_FIELDS,
        "PROFILE_FIELDS": PROFILE_FIELDS,
        "GAUGE_FIELDS": GAUGE_FIELDS,
        "CUMULATIVE_COUNTER_FIELDS": CUMULATIVE_COUNTER_FIELDS,
    }
    for name, value in bindings.items():
        setattr(_base, name, value)
    _base._parse_config = _parse_config
    _base._validate_snapshot_equations = _validate_snapshot_equations


_configure_base_parser()

_parse_decimal = _base._parse_decimal
_parse_pipeline = _base._parse_pipeline
_parse_checkpoint = _base._parse_checkpoint
_parse_profile = _base._parse_profile
_snapshot_checksum = _base._snapshot_checksum
_ratio_decimal = _base._ratio_decimal
_basis_points = _base._basis_points
_serialize_report = _base._serialize_report
_publish_report_exclusive = _base._publish_report_exclusive
_sequence_pair = _base._sequence_pair


def parse_log(path: pathlib.Path) -> ProfileLog:
    try:
        return _base.parse_log(path)
    except ReportError as error:
        raise ReportError(str(error).replace("phase-6CM", "phase-6CO")) from error


def _counter_delta(start: Snapshot, end: Snapshot) -> Mapping[str, int]:
    return {
        field: end.counters[field] - start.counters[field]
        for field in COUNTER_FIELDS
        if field not in ("windows_active", "active_owners")
    }


def _segment_accounting(
    name: str, start: Snapshot, end: Snapshot, counters: Mapping[str, int]
) -> None:
    active_change = end.counters["windows_active"] - start.counters["windows_active"]
    completed = _checked_sum(
        [
            counters["windows_complete"],
            counters["windows_hle_excluded"],
            counters["windows_reentrant_excluded"],
            counters["windows_dropped"],
        ],
        name,
        "delta window accounting",
    )
    if active_change >= 0:
        completed = _checked_sum([completed, active_change], name, "delta active-window accounting")
        _require(counters["windows_scheduled"] == completed, name, "delta window scheduling accounting mismatch")
    else:
        _require(
            completed >= -active_change
            and counters["windows_scheduled"] == completed + active_change,
            name,
            "delta window scheduling accounting mismatch",
        )
    expected_blocks = _checked_product(
        counters["windows_complete"], SAMPLE_WINDOW_BLOCKS, name, "delta sampled block count"
    )
    _require(
        counters["sampled_blocks"] == expected_blocks,
        name,
        "delta complete-window/sample-block accounting mismatch",
    )
    _validate_candidate_equations(counters, name)


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
    start_step_bound = _checked_product(
        start_unflushed_blocks_bound, 16, "candidate materiality", "start step uncertainty"
    )
    end_step_bound = _checked_product(
        end_unflushed_blocks_bound, 16, "candidate materiality", "end step uncertainty"
    )
    candidate_blocks_lower = max(candidate_blocks - start_unflushed_blocks_bound, 0)
    candidate_steps_lower = max(candidate_steps - start_step_bound, 0)
    _require(
        sampled_blocks <= UINT64_MAX - end_unflushed_blocks_bound,
        "candidate materiality",
        "sampled block upper bound overflows uint64",
    )
    _require(
        sampled_steps <= UINT64_MAX - end_step_bound,
        "candidate materiality",
        "sampled step upper bound overflows uint64",
    )
    sampled_blocks_upper = sampled_blocks + end_unflushed_blocks_bound
    sampled_steps_upper = sampled_steps + end_step_bound

    def threshold_products(numerator: int, denominator: int, threshold: int, label: str) -> bool:
        left = _checked_product(numerator, 10000, "candidate materiality", f"{label} numerator")
        right = _checked_product(denominator, threshold, "candidate materiality", f"{label} denominator")
        return denominator > 0 and left >= right

    raw_sample_pass = candidate_blocks >= config["material_min_samples"]
    raw_block_pass = threshold_products(
        candidate_blocks, sampled_blocks, config["material_min_block_bp"], "raw block share"
    )
    raw_step_pass = threshold_products(
        candidate_steps, sampled_steps, config["material_min_step_bp"], "raw step share"
    )
    sample_pass = candidate_blocks_lower >= config["material_min_samples"]
    block_pass = threshold_products(
        candidate_blocks_lower,
        sampled_blocks_upper,
        config["material_min_block_bp"],
        "conservative block share",
    )
    step_pass = threshold_products(
        candidate_steps_lower,
        sampled_steps_upper,
        config["material_min_step_bp"],
        "conservative step share",
    )
    zero_reentry = counters["candidate_reentry_events"] == 0
    zero_skips = counters["operation_snapshot_skipped"] == 0
    all_pass = sample_pass and block_pass and step_pass and zero_reentry and zero_skips
    return {
        "candidate_blocks": candidate_blocks,
        "candidate_steps": candidate_steps,
        "sampled_blocks": sampled_blocks,
        "sampled_steps": sampled_steps,
        "candidate_known_ops": counters["candidate_known_ops"],
        "candidate_guest_memory_ops": counters["candidate_guest_memory_ops"],
        "candidate_reentry_events": counters["candidate_reentry_events"],
        "operation_snapshot_skipped": counters["operation_snapshot_skipped"],
        "raw_candidate_block_share_basis_points": _basis_points(candidate_blocks, sampled_blocks),
        "raw_candidate_step_share_basis_points": _basis_points(candidate_steps, sampled_steps),
        "candidate_block_share_basis_points": _basis_points(candidate_blocks_lower, sampled_blocks_upper),
        "candidate_step_share_basis_points": _basis_points(candidate_steps_lower, sampled_steps_upper),
        "endpoint_uncertainty": {
            "start_unflushed_blocks_bound": start_unflushed_blocks_bound,
            "end_unflushed_blocks_bound": end_unflushed_blocks_bound,
            "start_unflushed_steps_bound": start_step_bound,
            "end_unflushed_steps_bound": end_step_bound,
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
        "sample_threshold_pass": sample_pass,
        "block_share_threshold_pass": block_pass,
        "step_share_threshold_pass": step_pass,
        "zero_candidate_reentry_pass": zero_reentry,
        "zero_operation_snapshot_skipped_pass": zero_skips,
        "uncertainty_robust_all_requirements_pass": all_pass,
        "all_requirements_pass": all_pass,
        "gate_semantics": (
            "candidate numerators subtract the start active-window bound; sampled "
            "denominators add the end bound; counts alone decide and any uncertainty "
            "that can change a threshold forces NO_ELECTION"
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
    floor_total = _checked_product(
        counters["candidate_blocks"], config["clock_floor_ns"], name, "candidate clock floor"
    )
    _require(
        counters["candidate_wall_ns"] >= floor_total,
        name,
        "candidate wall timing is below its calibrated sequence-floor lower bound",
    )
    net_upper = counters["candidate_wall_ns"] - floor_total
    components = {
        "snapshot_copy_ns": counters["candidate_snapshot_copy_ns"],
        "code_view_ns": counters["candidate_code_view_ns"],
        "write_guard_setup_ns": counters["candidate_write_guard_setup_ns"],
        "execute_ns": counters["candidate_execute_ns"],
    }
    candidate = _candidate_material(
        counters,
        config,
        start.derived["sampled_unflushed_blocks_bound"],
        end.derived["sampled_unflushed_blocks_bound"],
    )
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
        "sampled_population": {
            "blocks": counters["sampled_blocks"],
            "steps": counters["sampled_steps"],
        },
        "candidate": candidate,
        "candidate_timing": {
            "candidate_wall_ns": counters["candidate_wall_ns"],
            "components_ns": components,
            "share_of_candidate_wall_percent": {
                field: _ratio_decimal(value, counters["candidate_wall_ns"])
                for field, value in components.items()
            },
            "calibrated_sequence_floor_lower_bound_ns": floor_total,
            "net_cost_upper_bound_ns": net_upper,
            "closure": (
                "candidate_wall_ns=snapshot_copy_ns+code_view_ns+"
                "write_guard_setup_ns+execute_ns"
            ),
            "floor_semantics": (
                "raw counters remain authoritative; candidate_blocks*clock_floor_ns "
                "is shown separately and is never distributed across buckets"
            ),
        },
    }


def _election(
    segments: Mapping[str, Mapping[str, Any]],
    *,
    full_run: Mapping[str, Any],
    lifecycle_bound: bool,
) -> Mapping[str, Any]:
    process_reentries = full_run["counters"]["candidate_reentry_events"]
    no_run_reentry = process_reentries == 0
    if not lifecycle_bound:
        return {
            "status": "NO_ELECTION",
            "elected": None,
            "snapshot_elision_material": False,
            "process_wide_candidate_reentry_events": process_reentries,
            "process_wide_zero_candidate_reentry_pass": no_run_reentry,
            "speedup_claim": False,
            "speedup_estimate": None,
            "reason": "offline arithmetic is not bound to the complete manual lifecycle",
        }
    fort_pass = segments["fort"]["candidate"]["all_requirements_pass"]
    game_pass = segments["in_game"]["candidate"]["all_requirements_pass"]
    if fort_pass and game_pass and no_run_reentry:
        return {
            "status": "ELECTED",
            "elected": "guest_memory_operation_snapshot_elision_candidate",
            "snapshot_elision_material": True,
            "process_wide_candidate_reentry_events": process_reentries,
            "process_wide_zero_candidate_reentry_pass": True,
            "speedup_claim": False,
            "speedup_estimate": None,
            "reason": (
                "all conservative count thresholds and zero snapshot skips pass "
                "independently in Fort and in-game, with zero candidate reentry "
                "across the complete process"
            ),
        }
    return {
        "status": "NO_ELECTION",
        "elected": None,
        "snapshot_elision_material": False,
        "process_wide_candidate_reentry_events": process_reentries,
        "process_wide_zero_candidate_reentry_pass": no_run_reentry,
        "speedup_claim": False,
        "speedup_estimate": None,
        "reason": (
            "at least one preregistered count, uncertainty, process-wide candidate "
            "reentry or copy-preservation requirement failed"
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
        "fort": _segment("fort", snapshots[fort_sequences[0]], snapshots[fort_sequences[1]], profile.config),
        "in_game": _segment(
            "in_game", snapshots[in_game_sequences[0]], snapshots[in_game_sequences[1]], profile.config
        ),
    }
    full_run = _segment("full_run", first, final, profile.config)
    validated_binding = (
        _base._validate_live_binding(profile, fort_sequences, in_game_sequences, live_binding)
        if live_binding is not None
        else None
    )
    decision = _election(
        segments,
        full_run=full_run,
        lifecycle_bound=validated_binding is not None,
    )
    diagnostic_gate = (
        {
            "passed": False,
            "status": "LIFECYCLE_UNBOUND",
            "scope": DIAGNOSTIC_SCOPE,
            "persistent_baseline_eligible": False,
            "loading_metric_eligible": False,
            "interpretation": "offline arithmetic only; seal with phase6co_profile_live.py",
        }
        if validated_binding is None
        else {
            "passed": True,
            "status": "PASS_DIAGNOSTIC_ONLY",
            "scope": DIAGNOSTIC_SCOPE,
            "persistent_baseline_eligible": False,
            "loading_metric_eligible": False,
            "interpretation": "clean lifecycle-bound phase-6CO evidence; never a baseline or speedup record",
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
        "core_contract": {
            "config_marker": CONFIG_PREFIX.rstrip(),
            "profile_marker": PROFILE_PREFIX.rstrip(),
            "candidate_complete_blocks_only": True,
            "operation_snapshot_must_execute": True,
            "translation_fault_cross_page_smc_write_guard_signature_unchanged": True,
        },
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
        "snapshots": [_base._snapshot_json(snapshot) for snapshot in profile.snapshots],
        "segments": segments,
        "full_run": full_run,
        "decision": decision,
        "live_binding": validated_binding,
        "diagnostic_gate": diagnostic_gate,
        "limitations": [
            "one diagnostic run; no B/C performance comparison",
            "the measured run executes every operation_snapshot memcpy",
            "timing is descriptive and cannot change the count-based election",
            "an election only authorizes preregistration of a distinct future candidate",
        ],
    }


def _render_table(report: Mapping[str, Any]) -> str:
    gate = report["diagnostic_gate"]
    lines = [
        (
            "PHASE-6CO DIAGNOSTIC GATE: PASS (DIAGNOSTIC ONLY; NOT PERSISTENT BASELINE)"
            if gate["passed"]
            else "PHASE-6CO PROFILE REPORT: PASS (OFFLINE ONLY; LIFECYCLE UNBOUND)"
        ),
        f"log={report['source']['log']}",
        f"sha256={report['source']['sha256']}",
        "",
        "SEGMENT DELTAS",
        "segment      seq       sampled blocks/steps   candidate blocks/steps  block/step bp    copy/wall ns",
    ]
    for name in ("loading", "fort", "in_game"):
        segment = report["segments"][name]
        candidate = segment["candidate"]
        timing = segment["candidate_timing"]
        block_bp = candidate["candidate_block_share_basis_points"] or "n/a"
        step_bp = candidate["candidate_step_share_basis_points"] or "n/a"
        lines.append(
            f"{name:<11} {segment['start']['sequence']:>3}->{segment['end']['sequence']:<3} "
            f"{candidate['sampled_blocks']:>10}/{candidate['sampled_steps']:<10} "
            f"{candidate['candidate_blocks']:>10}/{candidate['candidate_steps']:<10} "
            f"{block_bp:>7}/{step_bp:<7} "
            f"{timing['components_ns']['snapshot_copy_ns']}/"
            f"{timing['candidate_wall_ns']}"
        )
    decision = report["decision"]
    lines.extend(
        [
            "",
            "ELECTION (COUNT-ONLY; NO SPEEDUP CLAIM)",
            f"status={decision['status']} elected={decision['elected'] or 'none'}",
            decision["reason"],
        ]
    )
    return "\n".join(lines)


def _fixture_counters(
    scale: int, *, candidate_blocks_per_scale: int = 12000, reentry_per_scale: int = 0
) -> Mapping[str, int]:
    complete = scale * 100
    sampled_blocks = complete * SAMPLE_WINDOW_BLOCKS
    sampled_steps = sampled_blocks * 8
    candidate_blocks = scale * candidate_blocks_per_scale
    candidate_steps = candidate_blocks * 8
    candidate_wall = scale * 50_000_000 if candidate_blocks else 0
    counters = {field: 0 for field in COUNTER_FIELDS}
    counters.update(
        {
            "windows_scheduled": complete,
            "windows_complete": complete,
            "candidate_reentry_events": scale * reentry_per_scale,
            "sampled_blocks": sampled_blocks,
            "sampled_steps": sampled_steps,
            "candidate_blocks": candidate_blocks,
            "candidate_steps": candidate_steps,
            "candidate_known_ops": candidate_steps,
            "candidate_guest_memory_ops": candidate_blocks,
            "candidate_wall_ns": candidate_wall,
            "candidate_snapshot_copy_ns": scale * 10_000_000 if candidate_blocks else 0,
            "candidate_code_view_ns": scale * 5_000_000 if candidate_blocks else 0,
            "candidate_write_guard_setup_ns": scale * 15_000_000 if candidate_blocks else 0,
            "candidate_execute_ns": scale * 20_000_000 if candidate_blocks else 0,
        }
    )
    return counters


def _fixture_snapshot_lines(
    sequence: int,
    reason: str,
    present: int,
    scale: int,
    *,
    candidate_blocks_per_scale: int = 12000,
    reentry_per_scale: int = 0,
) -> Sequence[str]:
    counters = _fixture_counters(
        scale,
        candidate_blocks_per_scale=candidate_blocks_per_scale,
        reentry_per_scale=reentry_per_scale,
    )
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
        "sampled_unflushed_blocks_bound": 0,
    }
    profile["checksum"] = _snapshot_checksum(profile)
    return [PROFILE_PREFIX + " ".join(f"{field}={profile[field]}" for field in PROFILE_FIELDS)]


def _fixture_log(
    checkpoint_reason: str = "checkpoint",
    *,
    candidate_blocks_per_scale: int = 12000,
    reentry_per_scale: int = 0,
) -> str:
    config_values = {**CONFIG_EXPECTED, "clock_floor_ns": 20}
    config = CONFIG_PREFIX + " ".join(f"{field}={config_values[field]}" for field in CONFIG_FIELDS)
    lines = ["fixture noise", config]

    def snapshot(sequence: int, reason: str, present: int, scale: int) -> None:
        lines.extend(
            _fixture_snapshot_lines(
                sequence,
                reason,
                present,
                scale,
                candidate_blocks_per_scale=candidate_blocks_per_scale,
                reentry_per_scale=reentry_per_scale,
            )
        )

    snapshot(1, "start", 0, 0)
    lines.append("[FRAME_PIPELINE] present=1 avg_interval=0.000 ms avg_latency=1.000 ms pending=0 index=0 arg=0")
    for present in range(120, 4201, 120):
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
            snapshot(2, checkpoint_reason, 2300, 1)
        elif present == 3000:
            snapshot(3, "signal", 3000, 2)
        elif present == 3120:
            snapshot(4, "signal", 3120, 3)
        elif present == 3960:
            snapshot(5, "signal", 4000, 4)
        elif present == 4080:
            snapshot(6, "signal", 4120, 5)
    snapshot(7, "final", 0, 6)
    return "\n".join(lines) + "\n"


def _fixture_live_binding(profile: ProfileLog) -> Mapping[str, Any]:
    # The manual lifecycle schema and exact clock ordering are inherited from
    # 6CM; only the phase-specific schema string changes.
    binding = json.loads(json.dumps(_base._fixture_live_binding(profile)))
    binding["schema"] = LIVE_BINDING_SCHEMA
    binding["scope"] = DIAGNOSTIC_SCOPE
    return binding


def _expect_failure(action: Callable[[], object], description: str) -> None:
    try:
        action()
    except ReportError:
        return
    raise ReportError(f"self-test accepted {description}")


def _mutate_profile(
    text: str,
    sequence: int,
    field: str,
    value: int,
    *,
    repair_checksum: bool = True,
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


def _fixture_set_active_bounds(text: str, sequences: Sequence[int]) -> str:
    selected = set(sequences)
    lines = text.splitlines()
    seen = set()
    for index, line in enumerate(lines):
        if not line.startswith(PROFILE_PREFIX):
            continue
        raw = dict(token.split("=", 1) for token in line[len(PROFILE_PREFIX) :].split(" "))
        sequence = int(raw["sequence"], 10)
        if sequence not in selected:
            continue
        raw["windows_scheduled"] = str(int(raw["windows_scheduled"], 10) + 1)
        raw["windows_active"] = "1"
        raw["active_owners"] = "1"
        raw["sampled_unflushed_blocks_bound"] = str(SAMPLE_WINDOW_BLOCKS)
        parsed: Dict[str, Any] = {
            name: raw[name] if name == "reason" else int(raw[name], 10)
            for name in PROFILE_FIELDS
            if name != "checksum"
        }
        raw["checksum"] = _snapshot_checksum(parsed)
        lines[index] = PROFILE_PREFIX + " ".join(
            f"{name}={raw[name]}" for name in PROFILE_FIELDS
        )
        seen.add(sequence)
    _require(seen == selected, "self-test fixture", "active-bound sequence is missing")
    return "\n".join(lines) + "\n"


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="phase6co-profile-report-self-test-") as temporary:
        root = pathlib.Path(temporary)
        valid_text = _fixture_log()
        valid_path = root / "valid.log"
        valid_path.write_text(valid_text, encoding="utf-8")
        profile = parse_log(valid_path)
        offline = build_report(profile, (3, 4), (5, 6))
        _require(
            offline["decision"]["status"] == "NO_ELECTION"
            and offline["persistent_baseline_eligible"] is False,
            "self-test",
            "unbound report elected or became baseline eligible",
        )
        bound = build_report(profile, (3, 4), (5, 6), _fixture_live_binding(profile))
        _require(
            bound["diagnostic_gate"]["status"] == "PASS_DIAGNOSTIC_ONLY"
            and bound["decision"]["status"] == "ELECTED"
            and bound["decision"]["speedup_claim"] is False
            and bound["persistent_baseline_eligible"] is False,
            "self-test",
            "valid bound material fixture did not produce diagnostic-only election",
        )
        fort_timing = bound["segments"]["fort"]["candidate_timing"]
        _require(
            fort_timing["candidate_wall_ns"]
            == sum(fort_timing["components_ns"].values())
            == fort_timing["calibrated_sequence_floor_lower_bound_ns"]
            + fort_timing["net_cost_upper_bound_ns"],
            "self-test",
            "candidate timing/floor accounting mismatch",
        )

        low_path = root / "low.log"
        low_path.write_text(_fixture_log(candidate_blocks_per_scale=100), encoding="utf-8")
        low_profile = parse_log(low_path)
        low_report = build_report(low_profile, (3, 4), (5, 6), _fixture_live_binding(low_profile))
        _require(low_report["decision"]["status"] == "NO_ELECTION", "self-test", "sub-threshold candidate elected")

        active_path = root / "active-bounds.log"
        active_path.write_text(
            _fixture_set_active_bounds(valid_text, (3, 4, 5, 6)),
            encoding="utf-8",
        )
        active_profile = parse_log(active_path)
        active_report = build_report(
            active_profile,
            (3, 4),
            (5, 6),
            _fixture_live_binding(active_profile),
        )
        active_candidate = active_report["segments"]["fort"]["candidate"]
        _require(
            active_report["decision"]["status"] == "ELECTED"
            and active_candidate["conservative_intervals"]["candidate_blocks"]
            == {"lower": 11744, "raw": 12000}
            and active_candidate["conservative_intervals"]["sampled_blocks"]
            == {"raw": 25600, "upper": 25856}
            and active_candidate["conservative_intervals"]["candidate_steps"]
            == {"lower": 91904, "raw": 96000}
            and active_candidate["conservative_intervals"]["sampled_steps"]
            == {"raw": 204800, "upper": 208896},
            "self-test",
            "active endpoint bounds were not applied to both blocks and steps",
        )

        edge_path = root / "active-bound-edge.log"
        edge_path.write_text(
            _fixture_set_active_bounds(
                _fixture_log(candidate_blocks_per_scale=10000),
                (3, 4, 5, 6),
            ),
            encoding="utf-8",
        )
        edge_profile = parse_log(edge_path)
        edge_report = build_report(
            edge_profile,
            (3, 4),
            (5, 6),
            _fixture_live_binding(edge_profile),
        )
        _require(
            edge_report["decision"]["status"] == "NO_ELECTION"
            and edge_report["segments"]["fort"]["candidate"]["raw_sample_threshold_pass"] is True
            and edge_report["segments"]["fort"]["candidate"]["sample_threshold_pass"] is False,
            "self-test",
            "active endpoint uncertainty did not fail-close the 10000-block edge",
        )

        reentry_path = root / "reentry.log"
        reentry_path.write_text(_fixture_log(reentry_per_scale=1), encoding="utf-8")
        reentry_profile = parse_log(reentry_path)
        reentry_report = build_report(reentry_profile, (3, 4), (5, 6), _fixture_live_binding(reentry_profile))
        _require(
            reentry_report["decision"]["status"] == "NO_ELECTION"
            and reentry_report["segments"]["fort"]["candidate"]["zero_candidate_reentry_pass"] is False,
            "self-test",
            "candidate reentry did not force NO_ELECTION",
        )

        outside_reentry_text = valid_text
        for sequence in range(2, 8):
            outside_reentry_text = _mutate_profile(
                outside_reentry_text,
                sequence,
                "candidate_reentry_events",
                1,
            )
        outside_reentry_path = root / "outside-scene-reentry.log"
        outside_reentry_path.write_text(outside_reentry_text, encoding="utf-8")
        outside_reentry_profile = parse_log(outside_reentry_path)
        outside_reentry_report = build_report(
            outside_reentry_profile,
            (3, 4),
            (5, 6),
            _fixture_live_binding(outside_reentry_profile),
        )
        _require(
            outside_reentry_report["segments"]["fort"]["candidate"]["candidate_reentry_events"] == 0
            and outside_reentry_report["segments"]["in_game"]["candidate"]["candidate_reentry_events"] == 0
            and outside_reentry_report["full_run"]["counters"]["candidate_reentry_events"] == 1
            and outside_reentry_report["decision"]["status"] == "NO_ELECTION",
            "self-test",
            "candidate reentry outside measured scenes did not confute the run-wide capability",
        )

        config_line = next(line for line in valid_text.splitlines() if line.startswith(CONFIG_PREFIX))
        absent = root / "capability-absent.log"
        absent.write_text(valid_text.replace(" translator_capability=non_reentrant", "", 1), encoding="utf-8")
        _expect_failure(lambda: parse_log(absent), "absent translator capability")
        malformed = root / "capability-malformed.log"
        malformed.write_text(
            valid_text.replace(
                "translator_capability=non_reentrant",
                "translator_capability=may_reenter",
                1,
            ),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(malformed), "non-effective translator capability")
        skip_config = root / "config-skip.log"
        skip_config.write_text(
            valid_text.replace(
                "operation_snapshot_skipped=0", "operation_snapshot_skipped=1", 1
            ),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(skip_config), "CONFIG snapshot skip")

        checksum = root / "checksum.log"
        checksum.write_text(valid_text.replace("checksum=", "checksum=0", 1), encoding="utf-8")
        _expect_failure(lambda: parse_log(checksum), "corrupt checksum")
        accounting = root / "accounting.log"
        accounting.write_text(
            _mutate_profile(
                valid_text,
                3,
                "candidate_known_ops",
                profile.by_sequence[3].counters["candidate_known_ops"] + 1,
            ),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(accounting), "candidate op accounting mismatch")
        closure = root / "closure.log"
        closure.write_text(
            _mutate_profile(
                valid_text,
                3,
                "candidate_execute_ns",
                profile.by_sequence[3].counters["candidate_execute_ns"] + 1,
            ),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(closure), "candidate timing closure mismatch")
        skipped = root / "skipped.log"
        skipped.write_text(_mutate_profile(valid_text, 3, "operation_snapshot_skipped", 1), encoding="utf-8")
        _expect_failure(lambda: parse_log(skipped), "nonzero skipped snapshot")
        bad_bound = root / "bound.log"
        bad_bound.write_text(_mutate_profile(valid_text, 3, "sampled_unflushed_blocks_bound", 1), encoding="utf-8")
        _expect_failure(lambda: parse_log(bad_bound), "bad active-window bound")

        overflow = root / "overflow.log"
        overflow.write_text(
            _mutate_profile(valid_text, 3, "candidate_execute_ns", UINT64_MAX),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(overflow), "timing-sum uint64 overflow")
        active_overflow_text = valid_text
        overflowing_active = UINT64_MAX // SAMPLE_WINDOW_BLOCKS + 1
        active_overflow_text = _mutate_profile(
            active_overflow_text, 3, "windows_active", overflowing_active
        )
        active_overflow_text = _mutate_profile(
            active_overflow_text, 3, "active_owners", overflowing_active
        )
        active_overflow_text = _mutate_profile(
            active_overflow_text,
            3,
            "windows_scheduled",
            profile.by_sequence[3].counters["windows_complete"] + overflowing_active,
        )
        active_overflow_text = _mutate_profile(
            active_overflow_text, 3, "sampled_unflushed_blocks_bound", UINT64_MAX
        )
        active_overflow_path = root / "active-bound-overflow.log"
        active_overflow_path.write_text(active_overflow_text, encoding="utf-8")
        _expect_failure(
            lambda: parse_log(active_overflow_path),
            "active-window bound multiplication overflow",
        )
        overflow_config = dict(profile.config)
        overflow_config["clock_floor_ns"] = UINT64_MAX
        _expect_failure(
            lambda: _segment("overflow-floor", profile.by_sequence[3], profile.by_sequence[4], overflow_config),
            "clock-floor product overflow",
        )
        _expect_failure(
            lambda: _candidate_material(
                {
                    "candidate_blocks": UINT64_MAX,
                    "candidate_steps": UINT64_MAX,
                    "sampled_blocks": UINT64_MAX,
                    "sampled_steps": UINT64_MAX,
                    "candidate_known_ops": UINT64_MAX,
                    "candidate_guest_memory_ops": UINT64_MAX,
                    "candidate_reentry_events": 0,
                    "operation_snapshot_skipped": 0,
                },
                profile.config,
                0,
                0,
            ),
            "basis-point multiplication overflow",
        )

        terminal = root / "terminal.log"
        terminal.write_text(
            valid_text.replace(
                "partial_intervals=20 terminal=0",
                "partial_intervals=20 terminal=1",
            ),
            encoding="utf-8",
        )
        _expect_failure(lambda: parse_log(terminal), "terminal p2300")
        duplicate = root / "duplicate.log"
        duplicate.write_text(valid_text + config_line + "\n", encoding="utf-8")
        _expect_failure(lambda: parse_log(duplicate), "duplicate CONFIG")

        output = root / "report.json"
        payload = _serialize_report(bound)
        _publish_report_exclusive(output, payload)
        _require(
            output.read_bytes() == payload and stat.S_IMODE(output.stat().st_mode) == 0o444,
            "self-test",
            "exclusive report publication mismatch",
        )
        _expect_failure(lambda: _publish_report_exclusive(output, b"overwrite\n"), "report overwrite")


def _main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "validate phase-6CO CONFIG/PROFILE v1, subtract loading/Fort/in-game "
            "candidate counters, and publish a fail-closed diagnostic-only report"
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
            for value in (
                arguments.log,
                arguments.fort,
                arguments.ingame,
                arguments.output,
            )
        ) or arguments.no_stdout:
            parser.error("--self-test does not accept paths or report options")
        self_test()
        print("phase6co-profile-report-self-test: PASS")
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
        print(f"phase6co-profile-report: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
