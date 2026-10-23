#!/usr/bin/env python3
# Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
"""Report complete PS5_FRAME_PIPELINE_V1 windows from one emulator session.

Accepted guest flip submissions are separated from host repeats and blanks;
they do not establish unique pixels or simulation progress. Windows intersecting
the requested boundary are excluded, never prorated. This tool is offline.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re
import sys
from typing import Any, Iterable

import fex_frame_path_report as critical_path


MARKER = re.compile(r"\bPS5_FRAME_PIPELINE_V1\b")
READY = re.compile(r"\bPS5_FRAME_PIPELINE_TRACE_V1\b")
PAIR = re.compile(r"\b([a-zA-Z_][a-zA-Z_0-9]*)=([^\s]+)")
COUNTERS = ("present", "guest_submission", "host_repeat", "host_blank", "failed")
FLOATS = ("elapsed_ms", "avg_interval_ms", "min_interval_ms", "max_interval_ms",
          "acquire_total_ms", "submit_present_total_ms")
UINT64_MAX = (1 << 64) - 1
GPU_MARKER = re.compile(r"\bPS5_GPU_SLICE_TIMING_V1\b")
PRESENT_JOIN_MARKER = re.compile(r"\bPS5_GPU_PRESENT_JOIN_V1\b")
GPU_PHASES = ("admission_ns", "process_ns", "finish_slice_ns", "close_ns")
GPU_NESTED = ("finish", "native_wait", "priority_wait")
GPU_HOST_IMPORT = ("image_cpu_copyout_bytes", "image_host_import_considered",
                   "image_host_import_attempts", "image_host_import_bytes",
                   "image_host_import_fallbacks", "image_host_import_denial_mask",
                   "image_host_import_drain_ns", "image_host_import_ns")
GPU_HOST_IMPORT_BATCH = ("image_host_import_batches", "image_host_import_batch_owners",
                         "image_host_import_batch_cap_stops")
GPU_BUFFER_HASH = ("buffer_hash_considered", "buffer_hash_attempts", "buffer_hash_pages",
                   "buffer_hash_bytes", "buffer_hash_copy_avoided_bytes", "buffer_hash_fallbacks",
                   "buffer_hash_fallback_mask", "buffer_revalidation_copy_bytes")
HLE_MARKER = re.compile(r"\bFEX_HLE_(WINDOW|TARGET)_V1\b")
HLE_IDENTITY = ("pid", "run", "thread", "window")
HLE_METRICS = ("calls", "finished", "failed", "short_calls", "short_fallback", "ordinary_calls",
               "admission_wall_ns", "admission_cpu_ns", "body_wall_ns", "body_cpu_ns",
               "readmission_wall_ns", "readmission_cpu_ns", "clock_failures")
HLE_SHORT_OUTCOMES = ("short_completed", "short_demoted", "short_calls_are_entries")


class ReportError(ValueError):
    """The input cannot establish an unambiguous measurement window."""


def parse_lines(lines: Iterable[str], source: str = "<log>") -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    seen: dict[int, dict[str, Any]] = {}
    duplicates = 0
    ready_count = 0
    for line_number, line in enumerate(lines, 1):
        ready_count += bool(READY.search(line))
        if ready_count > 1:
            raise ReportError("multiple trace initializations; provide a single-session log")
        marker = MARKER.search(line)
        if not marker:
            continue
        location = f"{source}:{line_number}"
        fields: dict[str, Any] = {}
        for key, value in PAIR.findall(line[marker.end():]):
            if key in fields:
                raise ReportError(f"{location}: duplicate field {key}")
            if key in FLOATS:
                try:
                    fields[key] = float(value)
                except ValueError as error:
                    raise ReportError(f"{location}: invalid {key}") from error
                if not math.isfinite(fields[key]) or fields[key] < 0:
                    raise ReportError(f"{location}: nonfinite or negative {key}")
            elif not re.fullmatch(r"\d+", value) or int(value) > UINT64_MAX:
                raise ReportError(f"{location}: invalid unsigned field {key}")
            else:
                fields[key] = int(value)
        required = ("window", "start_ticks", "end_ticks", "frequency", "window_present",
                    "guest_submission", "host_repeat", "host_blank", "failed", "interval_count",
                    "cumulative_acquire_ticks", "cumulative_submit_present_ticks",
                    *FLOATS, *("cumulative_" + key for key in COUNTERS))
        for key in required:
            if key not in fields:
                raise ReportError(f"{location}: missing field {key}")
        if not fields["window"] or not fields["frequency"] or fields["end_ticks"] <= fields["start_ticks"]:
            raise ReportError(f"{location}: invalid window or clock")
        elapsed_ms = (fields["end_ticks"] - fields["start_ticks"]) * 1000 / fields["frequency"]
        if not math.isclose(elapsed_ms, fields["elapsed_ms"], rel_tol=1e-9, abs_tol=0.000002):
            raise ReportError(f"{location}: elapsed time disagrees with clock ticks")
        for prefix, present in (("", "window_present"), ("cumulative_", "cumulative_present")):
            if fields[present] != sum(fields[prefix + key] for key in COUNTERS[1:4]):
                raise ReportError(f"{location}: accepted presentation categories do not sum")
        if fields["interval_count"] > fields["window_present"]:
            raise ReportError(f"{location}: too many presentation intervals")
        if fields["interval_count"] and not (fields["min_interval_ms"] <= fields["avg_interval_ms"] <= fields["max_interval_ms"]):
            raise ReportError(f"{location}: interval bounds disagree")
        for key in COUNTERS:
            local_key = "window_present" if key == "present" else key
            if fields["cumulative_" + key] < fields[local_key]:
                raise ReportError(f"{location}: cumulative {key} smaller than window")
        if fields["window"] in seen:
            if fields != seen[fields["window"]]:
                raise ReportError(f"{location}: conflicting window identity or mixed sessions")
            duplicates += 1
            continue
        if records:
            previous = records[-1]
            if (fields["window"] <= previous["window"] or fields["start_ticks"] < previous["end_ticks"] or
                    fields["frequency"] != previous["frequency"]):
                raise ReportError(f"{location}: unordered/overlapping windows or changed clock")
            contiguous = (fields["window"] == previous["window"] + 1 and
                          fields["start_ticks"] == previous["end_ticks"])
            for key in COUNTERS:
                delta = fields["cumulative_" + key] - previous["cumulative_" + key]
                local = fields["window_present" if key == "present" else key]
                if delta < local or (contiguous and delta != local):
                    raise ReportError(f"{location}: inconsistent cumulative {key}")
            for key in ("acquire", "submit_present"):
                delta = fields[f"cumulative_{key}_ticks"] - previous[f"cumulative_{key}_ticks"]
                local_ms = fields[f"{key}_total_ms"]
                if delta < 0 or (contiguous and not math.isclose(delta * 1000 / fields["frequency"], local_ms,
                                                                rel_tol=1e-9, abs_tol=0.000002)):
                    raise ReportError(f"{location}: inconsistent cumulative {key} time")
        seen[fields["window"]] = fields.copy()
        records.append({**fields, "source": source, "line": line_number})
    if not records:
        raise ReportError("no PS5_FRAME_PIPELINE_V1 windows found")
    return {"windows": records, "duplicates_ignored": duplicates, "trace_initialization_seen": bool(ready_count)}


def marker_line(lines: list[str], marker: str | None) -> int | None:
    if marker is None:
        return None
    matches = [index for index, line in enumerate(lines, 1) if marker in line]
    if len(matches) != 1:
        raise ReportError(f"boundary marker must occur exactly once: {marker!r} ({len(matches)} matches)")
    return matches[0]


def selection_ranges(windows: list[dict[str, Any]]) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    for record in windows:
        if ranges and record["start_ticks"] == ranges[-1][1]:
            ranges[-1] = ranges[-1][0], record["end_ticks"]
        else:
            ranges.append((record["start_ticks"], record["end_ticks"]))
    return ranges


def parse_gpu_lines(lines: Iterable[str], source: str = "<gpu-log>") -> dict[str, Any]:
    lines = list(lines)
    slices: list[dict[str, Any]] = []
    seen: dict[int, dict[str, int]] = {}
    clock = None
    limit_reached = False
    duplicate_count = 0
    initialized = False
    for line_number, line in enumerate(lines, 1):
        if "PS5_GPU_SLICE_TRACE_V1" in line:
            header = dict(PAIR.findall(line))
            if header.get("enabled") == "1":
                if initialized:
                    raise ReportError("multiple GPU trace initializations; provide one session")
                initialized = True
                clock = header.get("clock")
            limit_reached |= header.get("limit_reached") == "1"
        marker = GPU_MARKER.search(line)
        if not marker:
            continue
        fields: dict[str, int] = {}
        location = f"{source}:{line_number}"
        for key, value in PAIR.findall(line[marker.end():]):
            if key in fields:
                raise ReportError(f"{location}: duplicate GPU field {key}")
            if key == "private_eligible" and value == "-1":
                fields[key] = -1
            elif re.fullmatch(r"\d+", value) and int(value) <= UINT64_MAX:
                fields[key] = int(value)
            else:
                raise ReportError(f"{location}: invalid GPU field {key}")
        required = ("sequence", "tid", "start_ns", "end_ns", "submit_id", "queue_id", "type", "epoch_token",
                    "scheduler_tick_begin", "scheduler_tick_end", *GPU_PHASES,
                    *(name + suffix for name in GPU_NESTED for suffix in ("_calls", "_ns")),
                    "native_submits", "buffer_readback_bytes", "image_readback_bytes", "image_seed_bytes",
                    "queued_submissions", "ready_queue_heads", "pending_commands", "private_eligible", "complete")
        if any(key not in fields for key in required):
            raise ReportError(f"{location}: incomplete GPU slice record")
        import_fields = sum(key in fields for key in GPU_HOST_IMPORT)
        if import_fields not in (0, len(GPU_HOST_IMPORT)):
            raise ReportError(f"{location}: incomplete optional GPU host-import counters")
        batch_fields = sum(key in fields for key in GPU_HOST_IMPORT_BATCH)
        if batch_fields not in (0, len(GPU_HOST_IMPORT_BATCH)) or (batch_fields and not import_fields):
            raise ReportError(f"{location}: incomplete optional GPU host-import batch counters")
        hash_fields = sum(key in fields for key in GPU_BUFFER_HASH)
        if hash_fields not in (0, len(GPU_BUFFER_HASH)):
            raise ReportError(f"{location}: incomplete optional GPU buffer-hash counters")
        if (not fields["sequence"] or fields["end_ns"] < fields["start_ns"] or
                sum(fields[key] for key in GPU_PHASES) != fields["end_ns"] - fields["start_ns"] or
                fields["complete"] not in (0, 1) or fields["private_eligible"] not in (-1, 0, 1)):
            raise ReportError(f"{location}: inconsistent GPU slice phases or flags")
        identity = fields["sequence"]
        if identity in seen:
            if fields != seen[identity]:
                raise ReportError(f"{location}: conflicting GPU sequence or mixed sessions")
            duplicate_count += 1
            continue
        seen[identity] = fields
        slices.append({**fields, "source": source, "line": line_number})
    if not slices:
        raise ReportError("no PS5_GPU_SLICE_TIMING_V1 records found")
    return {"slices": slices, "clock": clock, "limit_reached": limit_reached,
            "duplicates_ignored": duplicate_count, "present_joins": parse_present_joins(lines, source)}


def parse_present_joins(lines: Iterable[str], source: str = "<gpu-log>") -> dict[str, Any]:
    joins = []
    seen: dict[int, dict[str, Any]] = {}
    duplicate_count = 0
    limit_reached = False
    for line_number, line in enumerate(lines, 1):
        if "PS5_GPU_PRESENT_JOIN_TRACE_V1" in line:
            limit_reached |= dict(PAIR.findall(line)).get("limit_reached") == "1"
        marker = PRESENT_JOIN_MARKER.search(line)
        if not marker:
            continue
        fields: dict[str, Any] = {}
        location = f"{source}:{line_number}"
        for key, value in PAIR.findall(line[marker.end():]):
            if key in fields:
                raise ReportError(f"{location}: duplicate present join field {key}")
            if key == "outcome":
                if value != "accepted":
                    raise ReportError(f"{location}: unsupported present outcome")
                fields[key] = value
            elif key == "bus" and re.fullmatch(r"-?\d+", value) and -(1 << 31) <= int(value) < (1 << 31):
                fields[key] = int(value)
            elif re.fullmatch(r"\d+", value) and int(value) <= UINT64_MAX:
                fields[key] = int(value)
            else:
                raise ReportError(f"{location}: invalid present join field {key}")
        required = ("sequence", "accepted_id", "request_id", "slice_sequence", "submit_id", "epoch_token",
                    "producer_tick", "present_ns", "present_tick", "bus", "outcome")
        if any(key not in fields for key in required) or not all(fields[key] for key in ("sequence", "accepted_id", "request_id", "slice_sequence")):
            raise ReportError(f"{location}: incomplete present join identity")
        previous = seen.get(fields["sequence"])
        if previous is not None:
            if previous != fields:
                raise ReportError(f"{location}: conflicting present join sequence")
            duplicate_count += 1
            continue
        seen[fields["sequence"]] = fields.copy()
        joins.append({**fields, "source": source, "line": line_number})
    return {"records": joins, "duplicates_ignored": duplicate_count, "limit_reached": limit_reached}


def present_dependency_report(parsed: dict[str, Any], ranges: list[tuple[int, int]]) -> dict[str, Any]:
    present = parsed["present_joins"]
    slices = {record["sequence"]: record for record in parsed["slices"]}
    selected = [r for r in present["records"] if any(start <= r["present_ns"] < end for start, end in ranges)]
    edges, unresolved = [], []
    accepted: dict[int, set[int]] = defaultdict(set)
    for join in selected:
        accepted[join["accepted_id"]].add(join["present_ns"])
        producer = slices.get(join["slice_sequence"])
        reason = None
        if producer is None:
            reason = "missing_slice_record"
        elif producer["submit_id"] != join["submit_id"] or producer["epoch_token"] != join["epoch_token"]:
            reason = "mismatched_submission_or_epoch"
        elif not producer["scheduler_tick_begin"] <= join["producer_tick"] <= producer["scheduler_tick_end"]:
            reason = "producer_tick_outside_slice"
        elif join["present_ns"] < producer["start_ns"]:
            reason = "presentation_precedes_producer_slice"
        if reason:
            unresolved.append({"join": join, "reason": reason})
            continue
        edges.append({"accepted_id": join["accepted_id"], "request_id": join["request_id"], "bus": join["bus"],
                      "slice_sequence": join["slice_sequence"], "submit_id": join["submit_id"], "epoch_token": join["epoch_token"],
                      "producer_tick": join["producer_tick"], "present_tick": join["present_tick"],
                      "slice_start_ns": producer["start_ns"], "slice_end_ns": producer["end_ns"],
                      "present_ns": join["present_ns"], "presented_before_slice_closed": join["present_ns"] < producer["end_ns"],
                      "dependency_contract": "exact_flip_request_carries_gpu_slice_and_native_producer_tick"})
    timestamps = {identity: next(iter(stamps)) for identity, stamps in accepted.items() if len(stamps) == 1}
    ambiguous = sorted(identity for identity, stamps in accepted.items() if len(stamps) != 1)
    intervals, gaps = [], []
    ordered = sorted(timestamps.items())
    for (previous_id, previous_ns), (identity, stamp) in zip(ordered, ordered[1:]):
        if stamp < previous_ns:
            raise ReportError("accepted presentation timestamps regress")
        if identity != previous_id + 1 or not any(start <= previous_ns <= stamp < end for start, end in ranges):
            gaps.append({"after_accepted_id": previous_id, "before_accepted_id": identity})
            continue
        intervals.append({"from_accepted_id": previous_id, "to_accepted_id": identity,
                          "start_ns": previous_ns, "end_ns": stamp, "interval_ns": stamp - previous_ns})
    return {
        "resolved_edges": edges, "unresolved_edges": unresolved,
        "join_limit_reached": present["limit_reached"], "duplicate_join_records_ignored": present["duplicates_ignored"],
        "observed_accepted_guest_events": len(accepted), "multi_layer_events_counted_once": True,
        "ambiguous_event_timestamps": ambiguous, "intervals": intervals, "interval_gaps": gaps,
        "guest_accepted_interval_statistics": critical_path.interval_statistics([r["interval_ns"] for r in intervals]),
        "full_guest_frame_critical_path_verified": False, "pixel_unique_measured": False,
        "limitations": ["Edges come only from explicit carried flip-request identities, never time proximity.",
                        "A priority callback may accept the flip before its producing GPU slice closes.",
                        "Only consecutive captured accepted guest IDs yield intervals; missing or ambiguous events are excluded.",
                        "Accepted guest IDs count submissions, not distinct pixel content or guest simulation progress.",
                        "A slice can produce several flips; its full duration cannot be charged independently to every flip.",
                        "Host acceptance does not prove scanout, GPU busy time or preceding CPU/HLE dependencies."],
    }


def host_import_report(slices: list[dict[str, Any]]) -> dict[str, Any]:
    covered = [r for r in slices if all(key in r for key in GPU_HOST_IMPORT)]
    batch_covered = [r for r in covered if all(key in r for key in GPU_HOST_IMPORT_BATCH)]
    mask = 0
    for record in covered:
        mask |= record["image_host_import_denial_mask"]
    return {
        "available": bool(covered), "covered_slices": len(covered),
        "unavailable_slices": len(slices) - len(covered),
        "totals": {key: (sum(r[key] for r in covered) if covered else None)
                   for key in GPU_HOST_IMPORT if key != "image_host_import_denial_mask"},
        "denial_mask_or": mask if covered else None,
        "attempted_slices": sum(r["image_host_import_attempts"] > 0 for r in covered) if covered else None,
        "imported_slices": sum(r["image_host_import_bytes"] > 0 for r in covered) if covered else None,
        "batch_counters": {
            "available": bool(batch_covered), "covered_slices": len(batch_covered),
            "unavailable_slices": len(slices) - len(batch_covered),
            "totals": {key: (sum(r[key] for r in batch_covered) if batch_covered else None)
                       for key in GPU_HOST_IMPORT_BATCH},
        },
        "limitations": ["Absent fields in older trace versions are unavailable, not zero.",
                        "The denial mask is a bitwise union of observed reasons, not a count.",
                        "Imported bytes describe owner spans including padding, not GPU transfer bytes.",
                        "Copyout bytes are scheduled staged copies; import envelope and drain times are inclusive.",
                        "Batch counters are optional; older single-owner versions have no inferred batch count.",
                        "Partial-version coverage must not be compared with whole-capture readback totals."],
    }


def buffer_hash_report(slices: list[dict[str, Any]]) -> dict[str, Any]:
    covered = [r for r in slices if all(key in r for key in GPU_BUFFER_HASH)]
    mask = 0
    for record in covered:
        mask |= record["buffer_hash_fallback_mask"]
    return {
        "available": bool(covered), "covered_slices": len(covered),
        "unavailable_slices": len(slices) - len(covered),
        "totals": {key: (sum(r[key] for r in covered) if covered else None)
                   for key in GPU_BUFFER_HASH if key != "buffer_hash_fallback_mask"},
        "fallback_mask_or": mask if covered else None,
        "attempted_slices": sum(r["buffer_hash_attempts"] > 0 for r in covered) if covered else None,
        "copy_avoiding_slices": sum(r["buffer_hash_copy_avoided_bytes"] > 0 for r in covered) if covered else None,
        "limitations": ["Absent fields in older trace versions are unavailable, not zero.",
                        "The fallback mask is a bitwise union of observed reasons, not a count.",
                        "Copy bytes describe buffer revalidation, not image publication or GPU transfer bytes.",
                        "Counters establish uptake and copied bytes; they do not establish elapsed time saved.",
                        "Totals cover only slices with all eight counters; mixed versions retain explicit coverage."],
    }


def gpu_report(parsed: dict[str, Any], windows: list[dict[str, Any]]) -> dict[str, Any]:
    if parsed["clock"] != "monotonic_ns" or windows[0]["frequency"] != 1_000_000_000:
        raise ReportError("GPU overlap requires declared clock=monotonic_ns and pipeline frequency=1000000000")
    ranges = selection_ranges(windows)
    overlapping = [r for r in parsed["slices"] if any(r["start_ns"] < end and r["end_ns"] > start for start, end in ranges)]
    contained = [r for r in overlapping if any(start <= r["start_ns"] and r["end_ns"] <= end for start, end in ranges)]
    sequences = sorted(r["sequence"] for r in parsed["slices"])
    dependencies = present_dependency_report(parsed, ranges)
    return {
        "causal_frame_join": bool(dependencies["resolved_edges"]), "clock": parsed["clock"],
        "full_frame_critical_path_verified": False,
        "fully_contained_slices": len(contained), "window_straddling_slices": len(overlapping) - len(contained),
        "outside_selection_slices": len(parsed["slices"]) - len(overlapping),
        "all_observed_sequence_gaps": [{"after": a, "before": b} for a, b in zip(sequences, sequences[1:]) if b > a + 1],
        "leading_prefix_missing": sequences[0] != 1,
        "trace_limit_reached": parsed["limit_reached"], "duplicates_ignored": parsed["duplicates_ignored"],
        "phase_wall_time": {key: {"sum_ns": sum(r[key] for r in contained),
                                   **critical_path.interval_statistics([r[key] for r in contained])} for key in GPU_PHASES},
        "nested_inclusive_wall_time": {name: {"calls": sum(r[name + "_calls"] for r in contained),
                                               "sum_ns": sum(r[name + "_ns"] for r in contained)} for name in GPU_NESTED},
        "scheduled_work": {key: sum(r[key] for r in contained) for key in
                           ("native_submits", "buffer_readback_bytes", "image_readback_bytes", "image_seed_bytes")},
        "host_backing_import": host_import_report(contained),
        "buffer_hash_revalidation": buffer_hash_report(contained),
        "private_eligible_slices": sum(r["private_eligible"] == 1 for r in contained),
        "completed_command_slices": sum(r["complete"] for r in contained),
        "explicit_present_dependencies": dependencies,
        "limitations": ["Only whole completed trace scopes inside the selected time ranges contribute to totals.",
                        "A trace scope may complete with guest commands suspended; complete is the command result.",
                        "Nested Finish/native/priority times overlap and cannot be added to the four phase times.",
                        "The four phases partition individual slices; concurrent slices still cannot form a frame budget.",
                        "Readback and seed counters count scheduled bytes, not completed transfers or GPU busy time.",
                        "Missing and in-flight scopes remain unknown; the bounded prefix is not a complete census.",
                        "Only separately validated carried flip-request records connect slices to presentations."],
        "slices": contained,
    }


def hle_short_outcome_report(rows: list[dict[str, int]]) -> dict[str, Any]:
    covered = [row for row in rows if all(key in row for key in HLE_SHORT_OUTCOMES)]
    return {
        "available": bool(covered), "covered_target_windows": len(covered),
        "unavailable_target_windows": len(rows) - len(covered),
        "short_calls_are_entries": True if covered else None,
        "totals": {key: (sum(row[key] for row in covered) if covered else None)
                   for key in ("short_calls", "short_completed", "short_demoted")},
    }


def hle_report(lines: Iterable[str], windows: list[dict[str, Any]], *, source: str = "<hle-log>",
               pid: int | None = None) -> dict[str, Any]:
    census: dict[tuple[int, ...], dict[str, int]] = {}
    targets: dict[tuple[int, ...], dict[int, dict[str, int]]] = defaultdict(dict)
    duplicates = 0
    for line_number, line in enumerate(lines, 1):
        marker = HLE_MARKER.search(line)
        if not marker:
            continue
        fields: dict[str, int] = {}
        location = f"{source}:{line_number}"
        for key, value in PAIR.findall(line[marker.end():]):
            if key in fields:
                raise ReportError(f"{location}: duplicate HLE field {key}")
            if key == "target" and re.fullmatch(r"0x[0-9a-fA-F]+", value):
                fields[key] = int(value, 16)
            elif re.fullmatch(r"\d+", value):
                fields[key] = int(value)
            else:
                raise ReportError(f"{location}: invalid HLE field {key}")
            if fields[key] > UINT64_MAX:
                raise ReportError(f"{location}: HLE field {key} exceeds uint64")
        is_window = marker.group(1) == "WINDOW"
        required = (*HLE_IDENTITY, "start_ns", "end_ns", "inclusive_thread_time", "frame_aligned",
                    *(("attempts", "finished", "failed", "pending", "overflow_calls", "clock_valid", "final")
                      if is_window else ("target", *HLE_METRICS)))
        if any(key not in fields for key in required):
            raise ReportError(f"{location}: incomplete HLE record")
        if (fields["inclusive_thread_time"] != 1 or fields["frame_aligned"] != 0 or
                fields["failed"] > fields["finished"]):
            raise ReportError(f"{location}: invalid HLE timing/count contract")
        identity = tuple(fields[key] for key in HLE_IDENTITY)
        if is_window:
            if (fields["clock_valid"] not in (0, 1) or fields["final"] not in (0, 1) or
                    fields["finished"] + fields["pending"] != fields["attempts"] or
                    fields["overflow_calls"] > fields["attempts"] or
                    (fields["clock_valid"] and fields["end_ns"] < fields["start_ns"])):
                raise ReportError(f"{location}: inconsistent HLE window")
            previous = census.get(identity)
            if previous is not None and previous != fields:
                raise ReportError(f"{location}: conflicting HLE window identity")
            duplicates += previous is not None
            census[identity] = fields
        else:
            if (fields["finished"] > fields["calls"] or fields["short_fallback"] > fields["ordinary_calls"] or
                    fields["short_calls"] + fields["ordinary_calls"] > fields["calls"]):
                raise ReportError(f"{location}: inconsistent HLE target counts")
            outcome_fields = sum(key in fields for key in HLE_SHORT_OUTCOMES)
            if outcome_fields not in (0, len(HLE_SHORT_OUTCOMES)):
                raise ReportError(f"{location}: incomplete optional HLE short-outcome counters")
            if outcome_fields and (fields["short_calls_are_entries"] != 1 or
                    fields["short_completed"] > fields["finished"] or
                    fields["short_completed"] + fields["short_demoted"] > fields["short_calls"]):
                raise ReportError(f"{location}: inconsistent HLE short-outcome counts")
            previous = targets[identity].get(fields["target"])
            if previous is not None and previous != fields:
                raise ReportError(f"{location}: conflicting HLE target identity")
            duplicates += previous is not None
            targets[identity][fields["target"]] = fields
    pids = {identity[0] for identity in set(census) | set(targets)}
    if not pids:
        raise ReportError("no FEX_HLE_WINDOW_V1 or FEX_HLE_TARGET_V1 records found")
    if pid is None and len(pids) > 1:
        raise ReportError("multiple HLE processes; select --hle-pid")
    pid = next(iter(pids)) if pid is None else pid
    if pid not in pids:
        raise ReportError("selected HLE pid is absent")
    if windows[0]["frequency"] != 1_000_000_000:
        raise ReportError("HLE overlap requires pipeline frequency=1000000000")
    ranges = selection_ranges(windows)
    counts = {key: 0 for key in ("fully_contained_windows", "window_straddling_windows", "outside_selection_windows",
                                "invalid_clock_windows", "incomplete_target_windows", "orphan_target_records")}
    aggregate: dict[int, dict[str, int]] = defaultdict(lambda: {key: 0 for key in HLE_METRICS})
    selected_targets: dict[int, list[dict[str, int]]] = defaultdict(list)
    selected = []
    for identity, header in census.items():
        if identity[0] != pid:
            continue
        rows = targets.get(identity, {})
        for row in rows.values():
            if row["start_ns"] != header["start_ns"] or row["end_ns"] != header["end_ns"]:
                raise ReportError("HLE target and window time bounds disagree")
        if not header["clock_valid"]:
            counts["invalid_clock_windows"] += 1
            continue
        if not any(header["start_ns"] < end and header["end_ns"] > start for start, end in ranges):
            counts["outside_selection_windows"] += 1
            continue
        if not any(start <= header["start_ns"] and header["end_ns"] <= end for start, end in ranges):
            counts["window_straddling_windows"] += 1
            continue
        actual_calls = sum(row["calls"] for row in rows.values())
        if actual_calls > header["attempts"]:
            raise ReportError("HLE target calls exceed the window census")
        if actual_calls != header["attempts"]:
            counts["incomplete_target_windows"] += 1
            continue
        if (sum(row["finished"] for row in rows.values()) != header["finished"] or
                sum(row["failed"] for row in rows.values()) != header["failed"] or
                rows.get(0, {}).get("calls", 0) != header["overflow_calls"]):
            raise ReportError("HLE target totals disagree with the window census")
        counts["fully_contained_windows"] += 1
        selected.append(header)
        for target, row in rows.items():
            selected_targets[target].append(row)
            for key in HLE_METRICS:
                aggregate[target][key] += row[key]
    counts["orphan_target_records"] = sum(len(rows) for identity, rows in targets.items()
                                         if identity[0] == pid and identity not in census)
    sequences: dict[tuple[int, int], list[int]] = defaultdict(list)
    for identity in census:
        if identity[0] == pid:
            sequences[identity[1], identity[2]].append(identity[3])
    sequence_coverage = []
    for (run, thread), values in sorted(sequences.items()):
        ordered = sorted(values)
        sequence_coverage.append({"run": run, "thread": thread, "leading_windows_missing": ordered[0] != 1,
                                  "gaps": [{"after": a, "before": b} for a, b in zip(ordered, ordered[1:]) if b > a + 1]})
    return {
        "pid": pid, "causal_frame_join": False, "duplicates_ignored": duplicates, **counts,
        "capture_census_complete": False, "window_sequence_coverage": sequence_coverage,
        "window_totals": {key: sum(row[key] for row in selected) for key in
                          ("attempts", "finished", "failed", "pending", "overflow_calls")},
        "short_outcomes": hle_short_outcome_report([row for rows in selected_targets.values() for row in rows]),
        "targets": [{"target": hex(target), "overflow_bucket": target == 0, **metrics,
                     "short_outcomes": hle_short_outcome_report(selected_targets[target])}
                    for target, metrics in sorted(aggregate.items(), key=lambda item:
                    sum(item[1][key] for key in ("admission_wall_ns", "body_wall_ns", "readmission_wall_ns")), reverse=True)],
        "limitations": ["Only full valid-clock windows with all target rows contribute; boundary windows are not prorated.",
                        "Times are inclusive per-thread sums; nested calls and concurrent threads are not a frame budget.",
                        "The dispatch census starts after veneer/target validation and stack scratch preparation.",
                        "Pending calls did not return normally; their already-recorded phases remain partial.",
                        "Short calls count entries, not completed short dispatches; demotions are not additional ordinary calls.",
                        "Optional short-outcome totals cover only target windows that emit all three fields; legacy outcomes remain unknown.",
                        "Missing windows and owner threads not present in the capture cannot be recovered.",
                        "No matching timestamp establishes a dependency on a particular guest flip."],
        "windows": selected,
    }


def build_report(parsed: dict[str, Any], *, start_ticks: int | None = None, end_ticks: int | None = None,
                 first_window: int | None = None, last_window: int | None = None,
                 after_line: int | None = None, before_line: int | None = None) -> dict[str, Any]:
    if ((start_ticks is not None and start_ticks < 0) or
            (end_ticks is not None and end_ticks < 0) or
            (start_ticks is not None and end_ticks is not None and end_ticks <= start_ticks) or
            (first_window is not None and first_window < 1) or
            (last_window is not None and last_window < 1) or
            (first_window is not None and last_window is not None and last_window < first_window) or
            (after_line is not None and before_line is not None and before_line <= after_line)):
        raise ReportError("invalid or reversed selection bounds")
    windows = parsed["windows"]
    after_window = next((r["window"] for r in windows if after_line is not None and r["line"] > after_line), None)
    selected = [r for r in windows if
                (start_ticks is None or r["start_ticks"] >= start_ticks) and
                (end_ticks is None or r["end_ticks"] <= end_ticks) and
                (first_window is None or r["window"] >= first_window) and
                (last_window is None or r["window"] <= last_window) and
                (after_line is None or (after_window is not None and r["window"] > after_window)) and
                (before_line is None or r["line"] < before_line)]
    if not selected:
        raise ReportError("selection contains no complete windows")
    gaps = [{"after_window": a["window"], "before_window": b["window"],
             "missing_ticks": b["start_ticks"] - a["end_ticks"]}
            for a, b in zip(selected, selected[1:])
            if b["window"] != a["window"] + 1 or b["start_ticks"] != a["end_ticks"]]
    elapsed = sum(r["end_ticks"] - r["start_ticks"] for r in selected) / selected[0]["frequency"]
    counts = {key: sum(r["window_present" if key == "present" else key] for r in selected) for key in COUNTERS}
    intervals = sum(r["interval_count"] for r in selected)
    return {
        "schema_version": 1, "metric": "accepted_presentations", "guest_metric": "accepted_guest_flip_submissions",
        "selection": {"first_window": selected[0]["window"], "last_window": selected[-1]["window"],
                      "start_ticks": selected[0]["start_ticks"], "end_ticks": selected[-1]["end_ticks"],
                      "frequency": selected[0]["frequency"], "windows": len(selected),
                      "excluded_windows": len(windows) - len(selected), "whole_windows_only": True,
                      "after_marker_crossing_window_excluded": after_window, "gaps": gaps},
        "observed_complete_windows": {
            "elapsed_seconds": elapsed, **counts,
            "accepted_presentations_per_second": counts["present"] / elapsed,
            "guest_flip_submissions_per_second": counts["guest_submission"] / elapsed,
            "host_repeats_per_second": counts["host_repeat"] / elapsed,
            "host_blanks_per_second": counts["host_blank"] / elapsed,
            "acquire_wall_ms": sum(r["acquire_total_ms"] for r in selected),
            "submit_present_wall_ms": sum(r["submit_present_total_ms"] for r in selected),
            "interval_count": intervals,
            "reported_interval_weighted_mean_ms": sum(r["avg_interval_ms"] * r["interval_count"] for r in selected) / intervals if intervals else None,
            "reported_interval_min_ms": min((r["min_interval_ms"] for r in selected if r["interval_count"]), default=None),
            "reported_interval_max_ms": max((r["max_interval_ms"] for r in selected if r["interval_count"]), default=None),
            "p50_ms": None, "p95_ms": None, "p99_ms": None,
        },
        "coverage": {"single_session_required": True, "trace_initialization_seen": parsed["trace_initialization_seen"],
                     "duplicates_ignored": parsed["duplicates_ignored"], "contiguous_windows": not gaps,
                     "pixel_unique_measured": False, "gameplay_progress_measured": False,
                     "per_frame_dependencies_available": False, "gpu_busy_time_measured": False,
                     "trace_overhead_measured": False, "pending_unreported_tail_included": False,
                     "limitations": [
                         "Guest flip submissions can repeat pixels or use special blank buffers.",
                         "Acquire and submit/present values are host wall-time sums, not GPU busy time.",
                         "Interval extrema/means may include the interval crossing the first selected boundary.",
                         "Per-frame percentiles cannot be reconstructed from window aggregates.",
                         "No causal join to FEX, publication or GPU slices is implied by overlapping timestamps.",
                         "Rates cover only recorded whole windows; gaps and the unreported tail are excluded.",
                     ]},
        "windows": selected,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="One existing emulator session log; no runtime is started")
    parser.add_argument("--start-ticks", type=int, help="Include only windows starting at or after this counter")
    parser.add_argument("--end-ticks", type=int, help="Include only windows ending at or before this counter")
    parser.add_argument("--first-window", type=int)
    parser.add_argument("--last-window", type=int)
    parser.add_argument("--after-marker", help="Unique literal line marker; discard the first window completing after it")
    parser.add_argument("--before-marker", help="Unique literal line marker; include only windows logged before it")
    parser.add_argument("--critical-path-log", type=Path, help="Optional HLE/span log, reported as temporal overlap only")
    parser.add_argument("--gpu-slice-log", type=Path, help="Optional PS5 GPU slice trace, reported as temporal overlap only")
    parser.add_argument("--hle-log", type=Path, help="Optional FEX HLE target census; include whole valid windows only")
    parser.add_argument("--hle-pid", type=int)
    parser.add_argument("--critical-pid", type=int)
    parser.add_argument("--monotonic-ns-clock", action="store_true", help="Confirm pipeline ticks use CLOCK_MONOTONIC ns (macOS/Android)")
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args(argv)
    try:
        lines = args.log.read_text(encoding="utf-8").splitlines()
        parsed = parse_lines(lines, str(args.log))
        report = build_report(parsed, start_ticks=args.start_ticks, end_ticks=args.end_ticks,
                              first_window=args.first_window, last_window=args.last_window,
                              after_line=marker_line(lines, args.after_marker), before_line=marker_line(lines, args.before_marker))
        if args.critical_path_log:
            if not args.monotonic_ns_clock or report["selection"]["frequency"] != 1_000_000_000:
                raise ReportError("critical-path overlap requires --monotonic-ns-clock and frequency=1000000000")
            critical = critical_path.parse_lines(args.critical_path_log.read_text(encoding="utf-8").splitlines(), str(args.critical_path_log))
            if not critical["records"]:
                raise ReportError("critical-path log contains no completed-span records")
            report["critical_path_temporal_overlap"] = {
                "causal_frame_join": False,
                "ranges": [critical_path.build_report(critical, start, end, args.critical_pid)
                           for start, end in selection_ranges(report["windows"])],
            }
        if args.gpu_slice_log:
            if not args.monotonic_ns_clock:
                raise ReportError("GPU overlap requires --monotonic-ns-clock")
            gpu = parse_gpu_lines(args.gpu_slice_log.read_text(encoding="utf-8").splitlines(), str(args.gpu_slice_log))
            report["gpu_slice_temporal_overlap"] = gpu_report(gpu, report["windows"])
            report["coverage"]["per_frame_dependencies_available"] = report["gpu_slice_temporal_overlap"]["causal_frame_join"]
        if args.hle_log:
            if not args.monotonic_ns_clock:
                raise ReportError("HLE overlap requires --monotonic-ns-clock")
            report["hle_temporal_overlap"] = hle_report(args.hle_log.read_text(encoding="utf-8").splitlines(),
                                                        report["windows"], source=str(args.hle_log), pid=args.hle_pid)
        payload = json.dumps(report, indent=2, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as handle:
                handle.write(payload)
        else:
            sys.stdout.write(payload)
    except (ReportError, critical_path.ReportError, OSError, UnicodeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
