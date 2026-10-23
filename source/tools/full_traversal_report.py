#!/usr/bin/env python3
"""Validate and compare two complete production/candidate traversal logs.

The tool is deliberately offline: it reads two existing run directories and
never launches the emulator.  JSON is written to stdout and the human-readable
table to stderr, so callers can redirect the JSON without losing the summary.
"""

from __future__ import annotations

import argparse
import decimal
import json
import pathlib
import re
import sys
import tempfile
from dataclasses import dataclass
from typing import Callable, Dict, List, Mapping, Optional, Sequence, Tuple


CHECKPOINT_PRESENT = 2300
WINDOW_SIZE = 120
LAST_PRE_INPUT_P120 = 2280
NAVIGATION_WINDOW_OFFSETS = (600, 720, 840)
MENU_WINDOW_OFFSETS = (120, 240, 360)
UINT64_MAX = (1 << 64) - 1
ZERO = decimal.Decimal(0)
ONE_HUNDRED = decimal.Decimal(100)
ONE_THOUSAND = decimal.Decimal(1000)

UNSIGNED_RE = r"(?:0|[1-9][0-9]*)"
SIGNED_RE = r"(?:0|-?[1-9][0-9]*)"
DECIMAL_RE = r"(?:0|[1-9][0-9]*)(?:\.[0-9]+)?"

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
    rf"cumulative_ms=(?P<cumulative_ms>{DECIMAL_RE}) "
    rf"cumulative_intervals=(?P<cumulative_intervals>{UNSIGNED_RE}) "
    rf"partial_ms=(?P<partial_ms>{DECIMAL_RE}) "
    rf"partial_intervals=(?P<partial_intervals>{UNSIGNED_RE}) "
    rf"terminal=(?P<terminal>{UNSIGNED_RE})$"
)
PROCESS_TIME_RE = re.compile(
    rf"^(?P<name>real|user|sys) (?P<seconds>{DECIMAL_RE})$"
)


class ReportError(Exception):
    """The run artifacts are incomplete, ambiguous, or malformed."""


@dataclass(frozen=True)
class FrameWindow:
    present: int
    avg_interval_ms: decimal.Decimal
    line_number: int


@dataclass(frozen=True)
class Checkpoint:
    present: int
    target: int
    cumulative_ms: decimal.Decimal
    cumulative_intervals: int
    partial_ms: decimal.Decimal
    partial_intervals: int
    terminal: int
    line_number: int


@dataclass(frozen=True)
class RunReport:
    label: str
    run_dir: pathlib.Path
    w0: int
    menu_m0: Optional[int]
    checkpoint: Checkpoint
    frames: Tuple[FrameWindow, ...]
    cumulative_ms: Mapping[int, decimal.Decimal]
    process_times: Mapping[str, decimal.Decimal]

    @property
    def frame_map(self) -> Mapping[int, FrameWindow]:
        return {frame.present: frame for frame in self.frames}

    @property
    def measured_navigation_presents(self) -> Tuple[int, int, int]:
        return (
            self.w0 + NAVIGATION_WINDOW_OFFSETS[0],
            self.w0 + NAVIGATION_WINDOW_OFFSETS[1],
            self.w0 + NAVIGATION_WINDOW_OFFSETS[2],
        )

    @property
    def measured_menu_presents(self) -> Optional[Tuple[int, int, int]]:
        if self.menu_m0 is None:
            return None
        return (
            self.menu_m0 + MENU_WINDOW_OFFSETS[0],
            self.menu_m0 + MENU_WINDOW_OFFSETS[1],
            self.menu_m0 + MENU_WINDOW_OFFSETS[2],
        )


def _require(condition: bool, source: object, message: str) -> None:
    if not condition:
        raise ReportError(f"{source}: {message}")


def _parse_uint(value: str, source: object, line_number: int, field: str) -> int:
    _require(
        re.fullmatch(UNSIGNED_RE, value) is not None,
        source,
        f"line {line_number}: {field} is not canonical unsigned decimal",
    )
    _require(len(value) <= 20, source, f"line {line_number}: {field} exceeds uint64")
    parsed = int(value, 10)
    _require(parsed <= UINT64_MAX, source, f"line {line_number}: {field} exceeds uint64")
    return parsed


def _parse_decimal(
    value: str, source: object, line_number: int, field: str
) -> decimal.Decimal:
    _require(
        re.fullmatch(DECIMAL_RE, value) is not None,
        source,
        f"line {line_number}: {field} is not canonical non-negative decimal",
    )
    parsed = decimal.Decimal(value)
    _require(parsed.is_finite(), source, f"line {line_number}: {field} is not finite")
    return parsed


def _read_status(run_dir: pathlib.Path) -> None:
    status_path = run_dir / ".exit_status"
    _require(status_path.is_file(), run_dir, "missing regular .exit_status")
    try:
        status = status_path.read_bytes()
    except OSError as error:
        raise ReportError(f"{status_path}: cannot read status: {error}") from error
    _require(status == b"0\n", status_path, "status must be byte-exact 0\\n")


def _read_stderr(run_dir: pathlib.Path) -> Tuple[pathlib.Path, str]:
    stderr_path = run_dir / "stderr.log"
    _require(stderr_path.is_file(), run_dir, "missing regular stderr.log")
    try:
        return stderr_path, stderr_path.read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as error:
        raise ReportError(f"{stderr_path}: cannot read strict UTF-8 log: {error}") from error


def _parse_log(
    source: pathlib.Path, text: str
) -> Tuple[Checkpoint, Tuple[FrameWindow, ...], Mapping[str, decimal.Decimal]]:
    frames: List[FrameWindow] = []
    checkpoint_records: List[Checkpoint] = []
    process_times: Dict[str, decimal.Decimal] = {}

    for line_number, line in enumerate(text.splitlines(), start=1):
        if line.startswith("[FRAME_PIPELINE] present="):
            match = PIPELINE_RE.fullmatch(line)
            _require(match is not None, source, f"line {line_number}: malformed present record")
            assert match is not None
            present = _parse_uint(match.group("present"), source, line_number, "present")
            interval = _parse_decimal(match.group("interval"), source, line_number, "avg_interval")
            latency = _parse_decimal(match.group("latency"), source, line_number, "avg_latency")
            _require(latency >= ZERO, source, f"line {line_number}: negative latency")
            frames.append(FrameWindow(present, interval, line_number))
            continue

        if line.startswith("[FRAME_CHECKPOINT]"):
            match = CHECKPOINT_RE.fullmatch(line)
            _require(match is not None, source, f"line {line_number}: malformed checkpoint record")
            assert match is not None
            checkpoint_records.append(
                Checkpoint(
                    present=_parse_uint(match.group("present"), source, line_number, "present"),
                    target=_parse_uint(match.group("target"), source, line_number, "target"),
                    cumulative_ms=_parse_decimal(
                        match.group("cumulative_ms"), source, line_number, "cumulative_ms"
                    ),
                    cumulative_intervals=_parse_uint(
                        match.group("cumulative_intervals"),
                        source,
                        line_number,
                        "cumulative_intervals",
                    ),
                    partial_ms=_parse_decimal(
                        match.group("partial_ms"), source, line_number, "partial_ms"
                    ),
                    partial_intervals=_parse_uint(
                        match.group("partial_intervals"),
                        source,
                        line_number,
                        "partial_intervals",
                    ),
                    terminal=_parse_uint(match.group("terminal"), source, line_number, "terminal"),
                    line_number=line_number,
                )
            )
            continue

        if line.startswith("[FRAME_LIMIT]"):
            raise ReportError(f"{source}: line {line_number}: terminal frame limit is forbidden")

        if re.match(r"^(?:real|user|sys)(?:\s|$)", line) is not None:
            match = PROCESS_TIME_RE.fullmatch(line)
            _require(match is not None, source, f"line {line_number}: malformed process time")
            assert match is not None
            name = match.group("name")
            _require(name not in process_times, source, f"line {line_number}: duplicate {name} time")
            process_times[name] = _parse_decimal(
                match.group("seconds"), source, line_number, f"{name} seconds"
            )

    _require(len(checkpoint_records) == 1, source, "expected exactly one FRAME_CHECKPOINT record")
    _require(frames, source, "missing FRAME_PIPELINE present records")
    _require(
        set(process_times) == {"real", "user", "sys"},
        source,
        "expected exactly one real, user, and sys time",
    )
    _require(process_times["real"] > ZERO, source, "real time must be positive")
    return checkpoint_records[0], tuple(frames), process_times


def _validate_checkpoint(
    checkpoint: Checkpoint, frame_map: Mapping[int, FrameWindow], source: pathlib.Path
) -> None:
    _require(
        checkpoint.present == CHECKPOINT_PRESENT,
        source,
        f"checkpoint present must be {CHECKPOINT_PRESENT}",
    )
    _require(checkpoint.target == checkpoint.present, source, "checkpoint target/present mismatch")
    _require(checkpoint.terminal == 0, source, "checkpoint must be non-terminal")
    _require(checkpoint.cumulative_ms > ZERO, source, "checkpoint cumulative_ms must be positive")
    _require(
        checkpoint.cumulative_intervals == checkpoint.present - 1,
        source,
        "checkpoint cumulative_intervals mismatch",
    )
    lower = checkpoint.present - (checkpoint.present % WINDOW_SIZE)
    upper = lower + WINDOW_SIZE
    _require(lower in frame_map, source, f"missing p{lower} before checkpoint")
    _require(upper in frame_map, source, f"missing p{upper} after checkpoint")
    _require(
        frame_map[lower].line_number < checkpoint.line_number < frame_map[upper].line_number,
        source,
        "checkpoint is not ordered between its adjacent p120 records",
    )
    _require(
        checkpoint.partial_intervals == checkpoint.present - lower,
        source,
        "checkpoint partial_intervals mismatch",
    )
    _require(checkpoint.partial_ms > ZERO, source, "checkpoint partial_ms must be positive")
    _require(
        checkpoint.partial_ms <= checkpoint.cumulative_ms,
        source,
        "checkpoint partial_ms exceeds cumulative_ms",
    )


def _validate_frames(
    frames: Sequence[FrameWindow], source: pathlib.Path
) -> Mapping[int, FrameWindow]:
    labels = [frame.present for frame in frames]
    _require(len(set(labels)) == len(labels), source, "duplicate FRAME_PIPELINE present label")
    _require(labels[0] == 1, source, "first present label must be p1")
    _require(frames[0].avg_interval_ms == ZERO, source, "p1 avg_interval must be zero")
    _require(labels[-1] >= WINDOW_SIZE, source, "trace does not reach p120")
    for index, frame in enumerate(frames[1:], start=1):
        _require(
            frame.present == index * WINDOW_SIZE,
            source,
            "missing, unexpected, or non-monotonic p120 label",
        )
        _require(
            frame.avg_interval_ms > ZERO,
            source,
            f"p{frame.present} avg_interval must be positive",
        )
    return {frame.present: frame for frame in frames}


def _cumulative_curve(frames: Sequence[FrameWindow]) -> Mapping[int, decimal.Decimal]:
    cumulative: Dict[int, decimal.Decimal] = {}
    elapsed = ZERO
    for frame in frames:
        if frame.present == 1:
            continue
        interval_count = WINDOW_SIZE - 1 if frame.present == WINDOW_SIZE else WINDOW_SIZE
        elapsed += frame.avg_interval_ms * interval_count
        cumulative[frame.present] = elapsed
    return cumulative


def _validate_w0(w0: int, source: object) -> None:
    _require(w0 > CHECKPOINT_PRESENT, source, "w0 must follow the p2300 checkpoint")
    _require(w0 % WINDOW_SIZE == 0, source, "w0 must be a positive p120 endpoint")


def _validate_menu_m0(menu_m0: int, w0: int, source: object) -> None:
    _require(
        menu_m0 > CHECKPOINT_PRESENT,
        source,
        "menu m0 must follow the p2300 checkpoint",
    )
    _require(
        menu_m0 % WINDOW_SIZE == 0,
        source,
        "menu m0 must be a positive p120 endpoint",
    )
    _require(
        menu_m0 + MENU_WINDOW_OFFSETS[-1] < w0,
        source,
        "menu measurement windows must precede Navigation w0",
    )


def parse_run(
    run_dir: pathlib.Path,
    label: str,
    w0: int,
    menu_m0: Optional[int] = None,
) -> RunReport:
    _validate_w0(w0, label)
    if menu_m0 is not None:
        _validate_menu_m0(menu_m0, w0, label)
    _require(run_dir.is_dir(), run_dir, "run directory does not exist")
    _read_status(run_dir)
    stderr_path, text = _read_stderr(run_dir)
    checkpoint, frames, process_times = _parse_log(stderr_path, text)
    frame_map = _validate_frames(frames, stderr_path)
    _validate_checkpoint(checkpoint, frame_map, stderr_path)

    _require(w0 in frame_map, stderr_path, f"missing w0 endpoint p{w0}")
    for present in (w0,) + tuple(
        w0 + offset for offset in NAVIGATION_WINDOW_OFFSETS
    ):
        _require(present in frame_map, stderr_path, f"missing required Navigation endpoint p{present}")
    if menu_m0 is not None:
        for present in (menu_m0,) + tuple(
            menu_m0 + offset for offset in MENU_WINDOW_OFFSETS
        ):
            _require(
                present in frame_map,
                stderr_path,
                f"missing required menu endpoint p{present}",
            )

    return RunReport(
        label=label,
        run_dir=run_dir.resolve(),
        w0=w0,
        menu_m0=menu_m0,
        checkpoint=checkpoint,
        frames=frames,
        cumulative_ms=_cumulative_curve(frames),
        process_times=process_times,
    )


def _divide(numerator: decimal.Decimal, denominator: decimal.Decimal) -> decimal.Decimal:
    _require(denominator > ZERO, "calculation", "division denominator must be positive")
    with decimal.localcontext() as context:
        context.prec = 40
        return numerator / denominator


def _fps(interval_ms: decimal.Decimal) -> decimal.Decimal:
    return _divide(ONE_THOUSAND, interval_ms)


def _median_three(values: Sequence[decimal.Decimal]) -> decimal.Decimal:
    _require(len(values) == 3, "calculation", "median requires exactly three values")
    return sorted(values)[1]


def _format(value: decimal.Decimal, places: int) -> str:
    quantum = decimal.Decimal(1).scaleb(-places)
    integer_digits = max(1, value.copy_abs().adjusted() + 1) if value != ZERO else 1
    with decimal.localcontext() as context:
        context.prec = max(40, integer_digits + places + 4)
        rounded = value.quantize(quantum, rounding=decimal.ROUND_HALF_UP)
    return format(rounded, "f")


def _comparison(
    baseline: decimal.Decimal,
    candidate: decimal.Decimal,
    places: int,
) -> Mapping[str, str]:
    ratio = _divide(candidate, baseline)
    delta_percent = (ratio - decimal.Decimal(1)) * ONE_HUNDRED
    return {
        "baseline": _format(baseline, places),
        "candidate": _format(candidate, places),
        "candidate_over_baseline": _format(ratio, 9),
        "candidate_delta_percent": _format(delta_percent, 6),
    }


def _window_summary_json(
    run: RunReport, measured: Sequence[int]
) -> Mapping[str, object]:
    frame_map = run.frame_map
    intervals = [frame_map[present].avg_interval_ms for present in measured]
    median_interval = _median_three(intervals)
    return {
        "measured_windows": [
            {
                "ordinal": ordinal,
                "present": present,
                "avg_interval_ms": _format(frame_map[present].avg_interval_ms, 3),
                "fps": _format(_fps(frame_map[present].avg_interval_ms), 6),
            }
            for ordinal, present in enumerate(measured, start=1)
        ],
        "median_interval_ms": _format(median_interval, 3),
        "median_fps": _format(_fps(median_interval), 6),
    }


def _run_json(run: RunReport) -> Mapping[str, object]:
    menu_measured = run.measured_menu_presents
    return {
        "run_dir": str(run.run_dir),
        "status": 0,
        "w0": run.w0,
        "menu_m0": run.menu_m0,
        "checkpoint": {
            "present": run.checkpoint.present,
            "target": run.checkpoint.target,
            "terminal": run.checkpoint.terminal,
            "cumulative_ms": _format(run.checkpoint.cumulative_ms, 3),
            "cumulative_intervals": run.checkpoint.cumulative_intervals,
            "partial_ms": _format(run.checkpoint.partial_ms, 3),
            "partial_intervals": run.checkpoint.partial_intervals,
        },
        "p120_pre_input": [
            {
                "present": frame.present,
                "avg_interval_ms": _format(frame.avg_interval_ms, 3),
                "fps": _format(_fps(frame.avg_interval_ms), 6),
                "cumulative_ms": _format(run.cumulative_ms[frame.present], 3),
            }
            for frame in run.frames
            if frame.present != 1 and frame.present <= LAST_PRE_INPUT_P120
        ],
        "menu": (
            _window_summary_json(run, menu_measured)
            if menu_measured is not None
            else None
        ),
        "navigation": _window_summary_json(
            run, run.measured_navigation_presents
        ),
        "process_time_seconds_descriptive": {
            name: _format(run.process_times[name], 3) for name in ("real", "user", "sys")
        },
    }


def _window_comparison(
    baseline: RunReport,
    candidate: RunReport,
    baseline_measured: Sequence[int],
    candidate_measured: Sequence[int],
) -> Mapping[str, object]:
    baseline_frames = baseline.frame_map
    candidate_frames = candidate.frame_map
    windows = []
    for ordinal, (baseline_present, candidate_present) in enumerate(
        zip(baseline_measured, candidate_measured), start=1
    ):
        baseline_interval = baseline_frames[baseline_present].avg_interval_ms
        candidate_interval = candidate_frames[candidate_present].avg_interval_ms
        windows.append(
            {
                "ordinal": ordinal,
                "baseline_present": baseline_present,
                "candidate_present": candidate_present,
                "avg_interval_ms": _comparison(
                    baseline_interval, candidate_interval, 3
                ),
                "fps": _comparison(
                    _fps(baseline_interval), _fps(candidate_interval), 6
                ),
            }
        )

    baseline_median_interval = _median_three(
        [baseline_frames[present].avg_interval_ms for present in baseline_measured]
    )
    candidate_median_interval = _median_three(
        [candidate_frames[present].avg_interval_ms for present in candidate_measured]
    )
    return {
        "windows": windows,
        "median_interval_ms": _comparison(
            baseline_median_interval, candidate_median_interval, 3
        ),
        "median_fps": _comparison(
            _fps(baseline_median_interval),
            _fps(candidate_median_interval),
            6,
        ),
    }


def build_report(baseline: RunReport, candidate: RunReport) -> Mapping[str, object]:
    _require(
        baseline.run_dir != candidate.run_dir,
        "comparison",
        "B and C run directories must be distinct",
    )
    _require(
        (baseline.menu_m0 is None) == (candidate.menu_m0 is None),
        "comparison",
        "menu m0 anchors must be supplied for both runs or neither run",
    )
    baseline_frames = baseline.frame_map
    candidate_frames = candidate.frame_map
    common_presents = sorted(
        present
        for present in (set(baseline_frames) & set(candidate_frames)) - {1}
        if present <= LAST_PRE_INPUT_P120
    )
    _require(
        common_presents,
        "comparison",
        "runs have no common pre-input p120 endpoints",
    )

    pre_input = []
    for present in common_presents:
        baseline_interval = baseline_frames[present].avg_interval_ms
        candidate_interval = candidate_frames[present].avg_interval_ms
        pre_input.append(
            {
                "present": present,
                "avg_interval_ms": _comparison(baseline_interval, candidate_interval, 3),
                "fps": _comparison(_fps(baseline_interval), _fps(candidate_interval), 6),
                "cumulative_ms": _comparison(
                    baseline.cumulative_ms[present], candidate.cumulative_ms[present], 3
                ),
            }
        )

    navigation = _window_comparison(
        baseline,
        candidate,
        baseline.measured_navigation_presents,
        candidate.measured_navigation_presents,
    )
    baseline_menu = baseline.measured_menu_presents
    candidate_menu = candidate.measured_menu_presents
    menu = (
        _window_comparison(baseline, candidate, baseline_menu, candidate_menu)
        if baseline_menu is not None and candidate_menu is not None
        else None
    )

    return {
        "schema": 2,
        "valid": True,
        "checkpoint_present": CHECKPOINT_PRESENT,
        "absolute_p120_comparison_end": LAST_PRE_INPUT_P120,
        "post_input_absolute_comparison": False,
        "menu_anchor_semantics": "m0 is discarded; measure m0+120,+240,+360",
        "cumulative_curve_method": (
            "pre-input only: 119*avg_interval(p120)+"
            "120*sum(avg_interval(p240..pN)), pN<=2280"
        ),
        "arms": {
            "B": _run_json(baseline),
            "C": _run_json(candidate),
        },
        "comparison": {
            "checkpoint_cumulative_ms": _comparison(
                baseline.checkpoint.cumulative_ms,
                candidate.checkpoint.cumulative_ms,
                3,
            ),
            "p120_pre_input": pre_input,
            "menu": menu,
            "navigation": navigation,
            "process_time_seconds_descriptive": {
                name: _comparison(
                    baseline.process_times[name], candidate.process_times[name], 3
                )
                for name in ("real", "user", "sys")
            },
        },
    }


def _render_table(report: Mapping[str, object]) -> str:
    arms = report["arms"]
    comparison = report["comparison"]
    assert isinstance(arms, Mapping)
    assert isinstance(comparison, Mapping)
    baseline = arms["B"]
    candidate = arms["C"]
    assert isinstance(baseline, Mapping)
    assert isinstance(candidate, Mapping)

    baseline_menu_suffix = (
        f"  menu_m0={baseline['menu_m0']}" if baseline["menu_m0"] is not None else ""
    )
    candidate_menu_suffix = (
        f"  menu_m0={candidate['menu_m0']}" if candidate["menu_m0"] is not None else ""
    )
    lines = [
        "FULL TRAVERSAL REPORT: PASS",
        f"B: {baseline['run_dir']}  w0={baseline['w0']}{baseline_menu_suffix}",
        f"C: {candidate['run_dir']}  w0={candidate['w0']}{candidate_menu_suffix}",
        "",
        "CHECKPOINT (lower is faster)",
        "metric                 B             C          C/B       delta C/B",
    ]
    checkpoint = comparison["checkpoint_cumulative_ms"]
    assert isinstance(checkpoint, Mapping)
    lines.append(
        f"p2300 cumulative ms  {checkpoint['baseline']:>12}  "
        f"{checkpoint['candidate']:>12}  {checkpoint['candidate_over_baseline']:>11}  "
        f"{checkpoint['candidate_delta_percent']:>10}%"
    )

    lines.extend(
        [
            "",
            "P120 PRE-INPUT (ABSOLUTE LABELS THROUGH p2280)",
            "present    B interval    C interval    delta int    B cumulative    C cumulative    delta cum",
        ]
    )
    pre_input = comparison["p120_pre_input"]
    assert isinstance(pre_input, Sequence)
    for row in pre_input:
        assert isinstance(row, Mapping)
        interval = row["avg_interval_ms"]
        cumulative = row["cumulative_ms"]
        assert isinstance(interval, Mapping)
        assert isinstance(cumulative, Mapping)
        lines.append(
            f"p{row['present']:<6}  {interval['baseline']:>10}  {interval['candidate']:>10}  "
            f"{interval['candidate_delta_percent']:>9}%  {cumulative['baseline']:>12}  "
            f"{cumulative['candidate']:>12}  {cumulative['candidate_delta_percent']:>9}%"
        )

    for title, key in (
        ("MENU STEADY STATE (RELATIVE TO PER-ARM m0)", "menu"),
        ("NAVIGATION STEADY STATE (RELATIVE TO PER-ARM w0)", "navigation"),
    ):
        section = comparison[key]
        if section is None:
            continue
        assert isinstance(section, Mapping)
        windows = section["windows"]
        assert isinstance(windows, Sequence)
        lines.extend(
            [
                "",
                title,
                "window    B present    C present      B ms      C ms    delta int      B FPS      C FPS    delta FPS",
            ]
        )
        for row in windows:
            assert isinstance(row, Mapping)
            interval = row["avg_interval_ms"]
            fps = row["fps"]
            assert isinstance(interval, Mapping)
            assert isinstance(fps, Mapping)
            lines.append(
                f"{row['ordinal']:>6}  p{row['baseline_present']:<9}  p{row['candidate_present']:<9}  "
                f"{interval['baseline']:>8}  {interval['candidate']:>8}  "
                f"{interval['candidate_delta_percent']:>9}%  {fps['baseline']:>9}  "
                f"{fps['candidate']:>9}  {fps['candidate_delta_percent']:>9}%"
            )

        median_interval = section["median_interval_ms"]
        median_fps = section["median_fps"]
        assert isinstance(median_interval, Mapping)
        assert isinstance(median_fps, Mapping)
        lines.append(
            f"median{'':>29}  {median_interval['baseline']:>8}  {median_interval['candidate']:>8}  "
            f"{median_interval['candidate_delta_percent']:>9}%  {median_fps['baseline']:>9}  "
            f"{median_fps['candidate']:>9}  {median_fps['candidate_delta_percent']:>9}%"
        )

    lines.extend(
        [
            "",
            "PROCESS TIME, DESCRIPTIVE ONLY",
            "metric                 B             C          C/B       delta C/B",
        ]
    )
    process_times = comparison["process_time_seconds_descriptive"]
    assert isinstance(process_times, Mapping)
    for name in ("real", "user", "sys"):
        values = process_times[name]
        assert isinstance(values, Mapping)
        lines.append(
            f"{name + ' seconds':<20}  {values['baseline']:>12}  {values['candidate']:>12}  "
            f"{values['candidate_over_baseline']:>11}  {values['candidate_delta_percent']:>10}%"
        )
    return "\n".join(lines)


def _fixture_log(
    interval_ms: int, real: int, user: int, system: int, w0: int = 2400
) -> str:
    last_present = w0 + NAVIGATION_WINDOW_OFFSETS[-1]
    lines = [
        "fixture noise",
        "[FRAME_PIPELINE] present=1 avg_interval=0.000 ms avg_latency=1.000 ms pending=0 index=0 arg=0",
    ]
    for present in range(WINDOW_SIZE, last_present + 1, WINDOW_SIZE):
        lines.append(
            f"[FRAME_PIPELINE] present={present} avg_interval={interval_ms}.000 ms "
            "avg_latency=1.000 ms pending=0 index=1 arg=0"
        )
        if present == 2280:
            cumulative = interval_ms * (CHECKPOINT_PRESENT - 1)
            partial = interval_ms * (CHECKPOINT_PRESENT - present)
            lines.append(
                f"[FRAME_CHECKPOINT] present=2300 target=2300 cumulative_ms={cumulative}.000 "
                f"cumulative_intervals=2299 partial_ms={partial}.000 partial_intervals=20 terminal=0"
            )
    lines.extend((f"real {real}.00", f"user {user}.00", f"sys {system}.00"))
    return "\n".join(lines) + "\n"


def _write_fixture(run_dir: pathlib.Path, log: str, status: bytes = b"0\n") -> None:
    run_dir.mkdir()
    (run_dir / "stderr.log").write_text(log, encoding="utf-8")
    (run_dir / ".exit_status").write_bytes(status)


def _expect_failure(action: Callable[[], object], description: str) -> None:
    try:
        action()
    except ReportError:
        return
    raise ReportError(f"self-test: accepted {description}")


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="full-traversal-report-self-test-") as temporary:
        root = pathlib.Path(temporary)
        baseline_dir = root / "B"
        candidate_dir = root / "C"
        baseline_w0 = 3600
        candidate_w0 = 3720
        baseline_menu_m0 = 2640
        candidate_menu_m0 = 2760
        baseline_log = _fixture_log(10, 100, 200, 10, w0=baseline_w0)
        candidate_log = _fixture_log(8, 90, 160, 8, w0=candidate_w0)
        _write_fixture(baseline_dir, baseline_log)
        _write_fixture(candidate_dir, candidate_log)

        baseline = parse_run(
            baseline_dir, "B", baseline_w0, menu_m0=baseline_menu_m0
        )
        candidate = parse_run(
            candidate_dir, "C", candidate_w0, menu_m0=candidate_menu_m0
        )
        report = build_report(baseline, candidate)
        comparison = report["comparison"]
        arms = report["arms"]
        assert isinstance(comparison, Mapping)
        assert isinstance(arms, Mapping)
        checkpoint = comparison["checkpoint_cumulative_ms"]
        navigation = comparison["navigation"]
        menu = comparison["menu"]
        pre_input = comparison["p120_pre_input"]
        assert isinstance(checkpoint, Mapping)
        assert isinstance(navigation, Mapping)
        assert isinstance(menu, Mapping)
        assert isinstance(pre_input, Sequence)
        median_interval = navigation["median_interval_ms"]
        median_fps = navigation["median_fps"]
        assert isinstance(median_interval, Mapping)
        assert isinstance(median_fps, Mapping)
        _require(
            checkpoint["candidate_over_baseline"] == "0.800000000",
            "self-test",
            "checkpoint ratio mismatch",
        )
        _require(
            median_interval["candidate_delta_percent"] == "-20.000000",
            "self-test",
            "Navigation interval delta mismatch",
        )
        _require(
            median_fps["candidate_delta_percent"] == "25.000000",
            "self-test",
            "Navigation FPS delta mismatch",
        )
        menu_median_interval = menu["median_interval_ms"]
        menu_median_fps = menu["median_fps"]
        assert isinstance(menu_median_interval, Mapping)
        assert isinstance(menu_median_fps, Mapping)
        _require(
            menu_median_interval["candidate_delta_percent"] == "-20.000000"
            and menu_median_fps["candidate_delta_percent"] == "25.000000",
            "self-test",
            "menu median comparison mismatch",
        )
        _require(
            len(pre_input) == LAST_PRE_INPUT_P120 // WINDOW_SIZE
            and pre_input[-1]["present"] == LAST_PRE_INPUT_P120,
            "self-test",
            "absolute p120 comparison extends beyond pre-input p2280",
        )
        baseline_json = arms["B"]
        assert isinstance(baseline_json, Mapping)
        baseline_p120 = baseline_json["p120_pre_input"]
        assert isinstance(baseline_p120, Sequence)
        _require(
            baseline_p120[-1]["present"] == LAST_PRE_INPUT_P120,
            "self-test",
            "per-run p120 output contains post-input cumulative data",
        )
        rendered = _render_table(report)
        _require(
            "FULL TRAVERSAL REPORT: PASS" in rendered
            and "MENU STEADY STATE" in rendered
            and "ABSOLUTE LABELS THROUGH p2280" in rendered,
            "self-test",
            "text table is missing pre-input/menu sections",
        )

        baseline_without_menu = parse_run(baseline_dir, "B", baseline_w0)
        candidate_without_menu = parse_run(candidate_dir, "C", candidate_w0)
        report_without_menu = build_report(
            baseline_without_menu, candidate_without_menu
        )
        no_menu_comparison = report_without_menu["comparison"]
        assert isinstance(no_menu_comparison, Mapping)
        _require(
            no_menu_comparison["menu"] is None
            and "MENU STEADY STATE" not in _render_table(report_without_menu),
            "self-test",
            "optional menu anchors were not omitted cleanly",
        )
        _expect_failure(
            lambda: build_report(baseline, candidate_without_menu),
            "a menu anchor supplied for only one arm",
        )
        duplicate_baseline = parse_run(
            baseline_dir, "C", baseline_w0, menu_m0=baseline_menu_m0
        )
        _expect_failure(
            lambda: build_report(baseline, duplicate_baseline),
            "the same resolved directory used for B and C",
        )
        _expect_failure(
            lambda: parse_run(
                baseline_dir, "B", baseline_w0, menu_m0=baseline_w0 - WINDOW_SIZE
            ),
            "menu measurements overlapping Navigation w0",
        )

        stderr_path = baseline_dir / "stderr.log"
        original = stderr_path.read_text(encoding="utf-8")
        checkpoint_line = next(
            line for line in original.splitlines() if line.startswith("[FRAME_CHECKPOINT]")
        )

        stderr_path.write_text(original + checkpoint_line + "\n", encoding="utf-8")
        _expect_failure(
            lambda: parse_run(baseline_dir, "B", baseline_w0), "duplicate checkpoint"
        )

        stderr_path.write_text(
            original.replace(" terminal=0", " terminal=1"), encoding="utf-8"
        )
        _expect_failure(
            lambda: parse_run(baseline_dir, "B", baseline_w0), "terminal checkpoint"
        )

        missing = "\n".join(
            line
            for line in original.splitlines()
            if not line.startswith("[FRAME_PIPELINE] present=1800 ")
        ) + "\n"
        stderr_path.write_text(missing, encoding="utf-8")
        _expect_failure(
            lambda: parse_run(baseline_dir, "B", baseline_w0),
            "missing p120 label",
        )

        duplicate = next(
            line
            for line in original.splitlines()
            if line.startswith("[FRAME_PIPELINE] present=1800 ")
        )
        stderr_path.write_text(original + duplicate + "\n", encoding="utf-8")
        _expect_failure(
            lambda: parse_run(baseline_dir, "B", baseline_w0),
            "duplicate p120 label",
        )

        stderr_path.write_text(
            original.replace("sys 10.00\n", ""), encoding="utf-8"
        )
        _expect_failure(
            lambda: parse_run(baseline_dir, "B", baseline_w0), "missing sys time"
        )

        stderr_path.write_text(original, encoding="utf-8")
        (baseline_dir / ".exit_status").write_bytes(b"65\n")
        _expect_failure(
            lambda: parse_run(baseline_dir, "B", baseline_w0), "nonzero status"
        )


def _p120_anchor_argument(value: str, name: str) -> int:
    if re.fullmatch(UNSIGNED_RE, value) is None:
        raise argparse.ArgumentTypeError(
            f"{name} must be canonical unsigned decimal"
        )
    if len(value) > 20:
        raise argparse.ArgumentTypeError(f"{name} exceeds uint64")
    parsed = int(value, 10)
    if parsed > UINT64_MAX:
        raise argparse.ArgumentTypeError(f"{name} exceeds uint64")
    if parsed <= CHECKPOINT_PRESENT or parsed % WINDOW_SIZE != 0:
        raise argparse.ArgumentTypeError(
            f"{name} must be a p120 endpoint greater than {CHECKPOINT_PRESENT}"
        )
    return parsed


def _w0_argument(value: str) -> int:
    return _p120_anchor_argument(value, "w0")


def _menu_m0_argument(value: str) -> int:
    return _p120_anchor_argument(value, "menu m0")


def _main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "validate and compare offline production/candidate full traversal run directories; "
            "JSON goes to stdout and the text table to stderr"
        )
    )
    parser.add_argument("baseline_run", nargs="?", metavar="B_RUN_DIR")
    parser.add_argument("candidate_run", nargs="?", metavar="C_RUN_DIR")
    parser.add_argument("--baseline-w0", type=_w0_argument, metavar="PRESENT")
    parser.add_argument("--candidate-w0", type=_w0_argument, metavar="PRESENT")
    parser.add_argument(
        "--baseline-menu-m0",
        type=_menu_m0_argument,
        metavar="PRESENT",
        help="discarded first p120 endpoint after the B menu anchor",
    )
    parser.add_argument(
        "--candidate-menu-m0",
        type=_menu_m0_argument,
        metavar="PRESENT",
        help="discarded first p120 endpoint after the C menu anchor",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic positive and fail-closed synthetic fixtures",
    )
    arguments = parser.parse_args(argv)

    if arguments.self_test:
        if (
            arguments.baseline_run is not None
            or arguments.candidate_run is not None
            or arguments.baseline_w0 is not None
            or arguments.candidate_w0 is not None
            or arguments.baseline_menu_m0 is not None
            or arguments.candidate_menu_m0 is not None
        ):
            parser.error(
                "--self-test does not accept run directories or anchor values"
            )
        self_test()
        print("full-traversal-report-self-test: PASS")
        return 0

    if arguments.baseline_run is None or arguments.candidate_run is None:
        parser.error("B_RUN_DIR and C_RUN_DIR are required")
    if arguments.baseline_w0 is None or arguments.candidate_w0 is None:
        parser.error("--baseline-w0 and --candidate-w0 are required")
    if (arguments.baseline_menu_m0 is None) != (
        arguments.candidate_menu_m0 is None
    ):
        parser.error(
            "--baseline-menu-m0 and --candidate-menu-m0 must be supplied together"
        )

    baseline = parse_run(
        pathlib.Path(arguments.baseline_run),
        "B",
        arguments.baseline_w0,
        menu_m0=arguments.baseline_menu_m0,
    )
    candidate = parse_run(
        pathlib.Path(arguments.candidate_run),
        "C",
        arguments.candidate_w0,
        menu_m0=arguments.candidate_menu_m0,
    )
    report = build_report(baseline, candidate)
    print(_render_table(report), file=sys.stderr)
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except ReportError as error:
        print(f"full-traversal-report: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
