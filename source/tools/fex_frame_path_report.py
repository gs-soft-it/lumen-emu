#!/usr/bin/env python3
"""Offline report for bounded LUMEN_CRITICAL_PATH_*_V1 Android PS4 logs.

Raw spans are inclusive and emitted only on completion above a duration
threshold. Their sums are diagnostic thread sums, never a frame budget.
Frame hints never establish edges. Only recording identities explicitly
published by the GPU submission/fence instrumentation establish dependencies.
Present dependencies name the exact blit submission whose semaphore is waited;
host acceptance does not establish GPU completion or distinct guest images.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import statistics
import sys
from collections import defaultdict
from typing import Any, Iterable


MARKER = re.compile(r"LUMEN_CRITICAL_PATH_(COMPLETE|SUMMARY|TOTAL)_V1\b")
PAIR = re.compile(r"\b([a-zA-Z_][a-zA-Z_0-9]*)=([^\s]+)")
PIPELINE = re.compile(
    r"\[FRAME_PIPELINE\] present=(\d+) avg_interval=(\d+(?:\.\d+)?) ms "
    r"avg_latency=(\d+(?:\.\d+)?) ms pending=(\d+) index=(-?\d+) arg=(-?\d+)"
)
UINT64_MAX = (1 << 64) - 1
HLE_REASONS = {"hle_admission", "hle_call", "hle_readmission"}
HISTOGRAM = ("hist_lt1us", "hist_lt10us", "hist_lt100us", "hist_lt1ms",
             "hist_lt10ms", "hist_lt100ms", "hist_lt1s", "hist_ge1s")
LOSS_FIELDS = ("dropped_spans", "dropped_events", "dropped_rate", "dropped_lifetime",
               "write_failures", "reports_suppressed", "target_overflow_calls",
               "invalid_intervals", "invalid_reasons", "frame_overflow", "counter_overflow")


class ReportError(ValueError):
    """A trace record is malformed or ambiguous."""


def number(fields: dict[str, str], key: str, location: str, default: int | None = None) -> int:
    value = fields.get(key)
    if value is None and default is not None:
        return default
    if value is None or not re.fullmatch(r"\d+", value) or int(value) > UINT64_MAX:
        raise ReportError(f"{location}: invalid or missing unsigned field {key}")
    return int(value)


def parse_lines(lines: Iterable[str], source: str = "<log>") -> dict[str, Any]:
    records: list[dict[str, Any]] = []
    pipeline: list[dict[str, Any]] = []
    seen: dict[tuple[Any, ...], dict[str, Any]] = {}
    duplicate_count = 0
    for line_number, line in enumerate(lines, 1):
        location = f"{source}:{line_number}"
        marker = MARKER.search(line)
        if marker:
            fields: dict[str, str] = {}
            for key, value in PAIR.findall(line[marker.end():]):
                if key in fields:
                    raise ReportError(f"{location}: duplicate field {key}")
                fields[key] = value
            kind = marker.group(1).lower()
            record: dict[str, Any] = {"record_type": kind, "source": source, "line": line_number}
            text_fields = {"kind", "reason"}
            for key in fields:
                record[key] = fields[key] if key in text_fields else number(fields, key, location)
            for key in ("pid", "tid", "completed_only" if kind != "total" else "cumulative",
                        "census_complete", "inclusive" if kind != "summary" else "frame_hint_noncausal"):
                number(fields, key, location)
            # V1 explicitly cannot certify a complete census or causal frame hints.
            if record["census_complete"] != 0:
                raise ReportError(f"{location}: unsupported census_complete flag")
            if kind == "complete":
                for key in ("sequence", "object", "dependency", "value", "start_ns", "end_ns",
                            "wall_ns", "thread_cpu_start_ns", "thread_cpu_end_ns", "thread_cpu_ns",
                            "frame_hint_start", "frame_hint_end", "frame_hint_noncausal"):
                    number(fields, key, location)
                if fields.get("kind") not in {"span", "event"} or not fields.get("reason"):
                    raise ReportError(f"{location}: invalid complete kind/reason")
                if record["completed_only"] != 1 or record["inclusive"] != 1 or record["frame_hint_noncausal"] != 1:
                    raise ReportError(f"{location}: unsupported span/frame contract")
                if (record["end_ns"] < record["start_ns"] or
                    record["wall_ns"] != record["end_ns"] - record["start_ns"] or
                    record["thread_cpu_end_ns"] < record["thread_cpu_start_ns"] or
                    record["thread_cpu_ns"] != record["thread_cpu_end_ns"] - record["thread_cpu_start_ns"]):
                    raise ReportError(f"{location}: inconsistent span duration")
                if record["kind"] == "event" and (record["wall_ns"] or record["thread_cpu_ns"]):
                    raise ReportError(f"{location}: event has nonzero duration")
                record.setdefault("frame_event_id", 0)  # Early V1 has only noncausal hints.
                identity = kind, record["pid"], record["tid"], record["sequence"]
            elif kind == "summary":
                for key in ("report", "monotonic_ns", "final"):
                    number(fields, key, location)
                if record["completed_only"] != 1 or record["frame_hint_noncausal"] != 1:
                    raise ReportError(f"{location}: unsupported summary contract")
                identity = kind, record["pid"], record["tid"], record["report"]
            else:
                for key in ("report", "target", "calls", "thread_wall_ns", "thread_cpu_ns", "max_wall_ns"):
                    number(fields, key, location)
                if fields.get("kind") not in {"reason", "hle_target"} or not fields.get("reason"):
                    raise ReportError(f"{location}: invalid total kind/reason")
                if record["cumulative"] != 1 or record["inclusive"] != 1:
                    raise ReportError(f"{location}: unsupported cumulative contract")
                identity = kind, record["pid"], record["tid"], record["report"], record["kind"], record["reason"], record["target"]
            comparison = {key: value for key, value in record.items() if key not in {"source", "line"}}
            if identity in seen:
                if seen[identity] != comparison:
                    raise ReportError(f"{location}: conflicting duplicate record {identity}")
                duplicate_count += 1
            else:
                seen[identity] = comparison
                records.append(record)
        match = PIPELINE.search(line)
        if match:
            present, interval, latency, pending, index, arg = match.groups()
            interval_ms, latency_ms = float(interval), float(latency)
            if not math.isfinite(interval_ms) or not math.isfinite(latency_ms):
                raise ReportError(f"{location}: nonfinite pipeline timing")
            pipeline.append({"source": source, "line": line_number, "present": int(present),
                             "avg_interval_ms": interval_ms, "avg_latency_ms": latency_ms,
                             "pending": int(pending), "index": int(index), "arg": int(arg),
                             "fps_from_mean_interval": 1000.0 / interval_ms if interval_ms > 0 else None,
                             "window_attribution": "unavailable_without_monotonic_stamp",
                             "p50_ns": None, "p95_ns": None, "p99_ns": None})
    return {"records": records, "pipeline": pipeline, "duplicate_records": duplicate_count}


def reference(record: dict[str, Any]) -> dict[str, int]:
    return {key: record[key] for key in ("pid", "tid", "sequence")}


def interval_statistics(values: list[int]) -> dict[str, Any]:
    if not values:
        return {"count": 0, "mean_ns": None, "p50_ns": None, "p95_ns": None, "p99_ns": None, "max_ns": None}
    ordered = sorted(values)
    return {"count": len(values), "mean_ns": statistics.mean(values), "p50_ns": statistics.median(values),
            "p95_ns": ordered[math.ceil(len(values) * 0.95) - 1], "max_ns": max(values),
            "p99_ns": ordered[math.ceil(len(values) * 0.99) - 1],
            "percentile_method": "p50_median_p95_p99_nearest_rank",
            "sample_size": len(values), "small_sample_under_20_intervals": len(values) < 20,
            "observed_raw_intervals_only": True}


def span_metrics(records: list[dict[str, Any]], start: int, end: int) -> dict[str, Any]:
    inside = [r for r in records if r["start_ns"] >= start and r["end_ns"] <= end]
    crossing = [r for r in records if r["start_ns"] < start or r["end_ns"] > end]
    longest = max(records, key=lambda r: r["wall_ns"], default=None)
    return {"completed_records_observed": len(records), "fully_contained_count": len(inside),
            "window_straddling_count": len(crossing),
            "fully_contained_inclusive_wall_ns_sum": sum(r["wall_ns"] for r in inside),
            "fully_contained_thread_cpu_ns_sum": sum(r["thread_cpu_ns"] for r in inside),
            "overlap_inclusive_wall_ns_sum": sum(min(r["end_ns"], end) - max(r["start_ns"], start) for r in records),
            "straddling_thread_cpu_ns": None if crossing else 0,
            "longest_observed_full_wall_ns": longest["wall_ns"] if longest else None,
            "longest_record": reference(longest) if longest else None,
            "short_spans_missing_from_detail": True, "inclusive_thread_sums_not_frame_budget": True}


def cumulative_report(records: list[dict[str, Any]], start: int, end: int) -> list[dict[str, Any]]:
    summaries: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
    totals: dict[tuple[int, int, int], dict[tuple[str, str, int], dict[str, Any]]] = defaultdict(dict)
    for record in records:
        if record["record_type"] == "summary":
            summaries[record["pid"], record["tid"]].append(record)
        elif record["record_type"] == "total":
            totals[record["pid"], record["tid"], record["report"]][record["kind"], record["reason"], record["target"]] = record
    result = []
    for (pid, tid), snapshots in sorted(summaries.items()):
        snapshots.sort(key=lambda r: (r["monotonic_ns"], r["report"]))
        before_start = [r for r in snapshots if r["monotonic_ns"] <= start]
        before_end = [r for r in snapshots if r["monotonic_ns"] <= end]
        left = before_start[-1] if before_start else None
        right = before_end[-1] if before_end else None
        entry: dict[str, Any] = {"pid": pid, "tid": tid, "start_snapshot": left, "end_snapshot": right,
                                 "deltas": [], "delta_available": bool(left and right and left != right),
                                 "requested_window_exact": bool(left and right and left["monotonic_ns"] == start and right["monotonic_ns"] == end),
                                 "completed_only": True, "inclusive_thread_sums_not_frame_budget": True,
                                 "snapshot_total_completeness_verified": False,
                                 "end_cumulative_totals": list(totals[pid, tid, right["report"]].values()) if right else [],
                                 "end_cumulative_bounds": {
                                     "start_ns": right.get("aggregation_start_ns"),
                                     "end_ns": right.get("aggregation_end_ns"),
                                 } if right else None}
        if entry["delta_available"]:
            left_totals = totals[pid, tid, left["report"]]
            right_totals = totals[pid, tid, right["report"]]
            for key in sorted(set(left_totals) | set(right_totals)):
                previous, current = left_totals.get(key), right_totals.get(key)
                if current is None:
                    raise ReportError(f"cumulative total disappeared for {pid}/{tid}: {key}")
                delta = {"kind": key[0], "reason": key[1], "target": key[2],
                         "start_ns": left["monotonic_ns"], "end_ns": right["monotonic_ns"],
                         "end_cumulative_max_wall_ns": current["max_wall_ns"], "delta_longest_wall_ns": None}
                for field in ("calls", "thread_wall_ns", "thread_cpu_ns", *HISTOGRAM, "counter_overflow"):
                    old, new = previous.get(field, 0) if previous else 0, current.get(field, 0)
                    if new < old:
                        raise ReportError(f"cumulative {field} regressed for {pid}/{tid}: {key}")
                    delta[field] = new - old
                delta["max_is_end_cumulative_not_window_max"] = True
                entry["deltas"].append(delta)
        result.append(entry)
    return result


def build_report(parsed: dict[str, Any], start: int, end: int, pid: int | None = None) -> dict[str, Any]:
    if start < 0 or end > UINT64_MAX or end <= start:
        raise ReportError("window must satisfy 0 <= start-ns < end-ns <= UINT64_MAX")
    pids = sorted({r["pid"] for r in parsed["records"]})
    if pid is None and len(pids) > 1:
        raise ReportError("multiple processes in trace; select --pid")
    if pid is None and pids:
        pid = pids[0]
    records = [r for r in parsed["records"] if r["pid"] == pid]
    complete = [r for r in records if r["record_type"] == "complete"]
    details = [r for r in complete if
               (start <= r["end_ns"] < end if r["kind"] == "event" else
                r["start_ns"] < end and r["end_ns"] > start)]
    spans = [r for r in details if r["kind"] == "span"]
    frames = sorted([r for r in details if r["kind"] == "event" and r["reason"] == "accepted_present"],
                    key=lambda r: (r["end_ns"], r["frame_event_id"], r["tid"], r["sequence"]))
    intervals, gaps = [], []
    for previous, current in zip(frames, frames[1:]):
        if not previous["frame_event_id"] or current["frame_event_id"] != previous["frame_event_id"] + 1:
            gaps.append({"from": reference(previous), "to": reference(current),
                         "start_frame_event_id": previous["frame_event_id"], "end_frame_event_id": current["frame_event_id"],
                         "elapsed_ns": current["end_ns"] - previous["end_ns"],
                         "reason": "missing_or_nonconsecutive_exact_frame_identity"})
        else:
            intervals.append({"from_frame_event_id": previous["frame_event_id"], "to_frame_event_id": current["frame_event_id"],
                              "start_ns": previous["end_ns"], "end_ns": current["end_ns"],
                              "interval_ns": current["end_ns"] - previous["end_ns"]})
    reasons: dict[str, list[dict[str, Any]]] = defaultdict(list)
    targets: dict[tuple[int, str], list[dict[str, Any]]] = defaultdict(list)
    for record in spans:
        reasons[record["reason"]].append(record)
        if record["reason"] in HLE_REASONS:
            targets[record["object"], record["reason"]].append(record)

    producers: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for record in complete:
        if record["kind"] == "event" and record["reason"] == "gpu_submission_accepted" and record["object"]:
            producers[record["object"]].append(record)
    edges, unresolved = [], []
    for consumer in details:
        if consumer["reason"] == "gpu_fence_wait" and consumer["dependency"]:
            identity, relation = consumer["dependency"], "submission_to_explicit_fence_wait"
            deadline = consumer["start_ns"]
        elif consumer["kind"] == "event" and consumer["reason"] == "gpu_fence_completed" and consumer["object"]:
            identity, relation = consumer["object"], "submission_to_successful_fence_completion"
            deadline = consumer["end_ns"]
        elif consumer["kind"] == "span" and consumer["reason"] == "vulkan_present" and consumer["dependency"]:
            identity, relation = consumer["dependency"], "submission_to_explicit_present_wait"
            deadline = consumer["start_ns"]
        elif consumer["kind"] == "event" and consumer["reason"] == "accepted_present" and consumer["dependency"]:
            identity, relation = consumer["dependency"], "submission_to_accepted_present"
            deadline = consumer["end_ns"]
        else:
            continue
        matches = [r for r in producers[identity] if r["end_ns"] <= deadline]
        item = {"recording_id": identity, "relation": relation, "consumer": reference(consumer)}
        if len(matches) != 1:
            item["reason"] = "missing_producer" if not matches else "ambiguous_producer_identity"
            unresolved.append(item)
        else:
            producer = matches[0]
            item.update({"producer": reference(producer), "producer_ns": producer["end_ns"],
                         "consumer_start_ns": consumer["start_ns"], "consumer_end_ns": consumer["end_ns"],
                         "producer_outside_window": not start <= producer["end_ns"] < end,
                         "submission_acceptance_is_not_gpu_completion": True})
            if relation in {"submission_to_explicit_present_wait", "submission_to_accepted_present"}:
                item.update({"dependency_contract": "exact_blit_submission_signals_present_wait_semaphore",
                             "host_accepted_present_is_not_gpu_completion": True,
                             "distinct_guest_images_verified": False,
                             "preceding_guest_gpu_hle_dependencies_verified": False})
            edges.append(item)

    # Each counter is cumulative per thread: never sum every snapshot.
    loss_by_thread: dict[tuple[int, int], dict[str, int]] = defaultdict(lambda: {field: 0 for field in LOSS_FIELDS})
    summary_stamps = {(r["pid"], r["tid"], r["report"]): r["monotonic_ns"]
                      for r in records if r["record_type"] == "summary"}
    for record in records:
        stamp = record.get("monotonic_ns", record.get("end_ns"))
        if record["record_type"] == "total":
            stamp = summary_stamps.get((record["pid"], record["tid"], record["report"]))
        if stamp is None or stamp > end:
            continue
        thread = loss_by_thread[record["pid"], record["tid"]]
        for field in LOSS_FIELDS:
            thread[field] = max(thread[field], record.get(field, 0))
    losses = [{"pid": key[0], "tid": key[1], **value} for key, value in sorted(loss_by_thread.items())]
    emitted_by_thread: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
    for record in complete:
        if record["end_ns"] < end:
            emitted_by_thread[record["pid"], record["tid"]].append(record)
    sequence_coverage = []
    for (thread_pid, tid), emitted in sorted(emitted_by_thread.items()):
        emitted.sort(key=lambda r: r["sequence"])
        missing = [{"after_sequence": left["sequence"], "before_sequence": right["sequence"],
                    "missing_records": right["sequence"] - left["sequence"] - 1}
                   for left, right in zip(emitted, emitted[1:]) if right["sequence"] > left["sequence"] + 1]
        sequence_coverage.append({"pid": thread_pid, "tid": tid,
                                  "leading_emitted_history_not_captured": emitted[0]["sequence"] != 1,
                                  "missing_emitted_record_ranges": missing})
    orphan_totals = [r for r in records if r["record_type"] == "total" and
                     (r["pid"], r["tid"], r["report"]) not in summary_stamps]
    return {"schema_version": 1, "window": {"start_ns": start, "end_ns": end, "half_open": True, "pid": pid},
            "coverage": {"critical_path_complete": False, "census_complete": False,
                         "completed_only": True, "inflight_scopes_at_capture_end_unknown": True,
                         "short_spans_aggregated_only": True, "bounded_detail": True,
                         "window_straddling_spans_observed": any(r["start_ns"] < start or r["end_ns"] > end for r in spans),
                         "frame_hint_used_for_causal_join": False,
                         "inclusive_thread_sums_are_frame_percentages": False,
                         "explicit_dependencies_only": True,
                         "any_reported_loss_before_window_end": any(v for row in losses for key, v in row.items() if key in LOSS_FIELDS),
                         "losses_are_cumulative_not_requested_window_deltas": True,
                         "limitations": ["Completed records do not enumerate scopes still open at capture end.",
                                         "Inclusive nested and concurrent spans cannot be added to a frame budget.",
                                         "Frame hints are temporal observations, never causal dependencies.",
                                         "Accepted presents do not certify distinct images or simulation progress.",
                                         "Present edges identify the waited blit submission; host acceptance does not certify GPU completion or preceding guest GPU/HLE dependencies.",
                                         "Raw interval statistics exclude missing frame identities and window-boundary intervals."]},
            "duplicate_records_ignored": parsed["duplicate_records"], "loss_counters_by_thread": losses,
            "emitted_record_sequence_coverage": sequence_coverage,
            "unpaired_cumulative_totals_without_timestamp": orphan_totals,
            "detail_limits_observed": {field: sorted({r[field] for r in complete if field in r})
                                       for field in ("long_threshold_ns", "detail_per_second", "detail_per_thread")},
            "accepted_presents": {"observed_count": len(frames), "distinct_images_verified": False,
                                  "records": frames, "intervals": intervals, "gaps": gaps,
                                  "interval_statistics": interval_statistics([r["interval_ns"] for r in intervals])},
            "detail_spans": {"by_reason": {key: span_metrics(value, start, end) for key, value in sorted(reasons.items())},
                             "hle_targets": [{"target": target, "reason": reason, **span_metrics(value, start, end)}
                                             for (target, reason), value in sorted(targets.items())],
                             "window_straddling_records": [r for r in spans if r["start_ns"] < start or r["end_ns"] > end]},
            "cumulative_thread_snapshots": cumulative_report(records, start, end),
            "explicit_gpu_dependency_edges": edges, "unresolved_explicit_gpu_dependencies": unresolved,
            "frame_pipeline_unattributed_to_monotonic_window": parsed["pipeline"]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=pathlib.Path, help="Existing logcat/native trace; never starts the emulator")
    parser.add_argument("--start-ns", required=True, type=int, help="CLOCK_MONOTONIC window start")
    parser.add_argument("--end-ns", required=True, type=int, help="CLOCK_MONOTONIC window end, exclusive")
    parser.add_argument("--pid", type=int, help="Required when trace contains multiple processes")
    parser.add_argument("-o", "--output", type=pathlib.Path, help="JSON output; defaults to stdout")
    args = parser.parse_args(argv)
    try:
        with args.log.open(encoding="utf-8") as handle:
            parsed = parse_lines(handle, str(args.log))
        report = build_report(parsed, args.start_ns, args.end_ns, args.pid)
        payload = json.dumps(report, indent=2, allow_nan=False) + "\n"
        if args.output:
            args.output.write_text(payload, encoding="utf-8")
        else:
            sys.stdout.write(payload)
    except (ReportError, OSError, UnicodeError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
