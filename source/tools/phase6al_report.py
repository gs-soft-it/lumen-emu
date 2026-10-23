#!/usr/bin/env python3
"""Parse and validate phase-6AL compiled-archive telemetry reports.

The verifier uses only the Python standard library.  It accepts a bare report
or a larger log containing one or more complete, contiguous phase-6AL frames;
unrelated lines are ignored.  Every line carrying a phase-6AL marker is parsed
strictly so that a damaged or future schema cannot silently pass as version 2.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Mapping, Optional, Sequence, Tuple, Union


UINT64_MAX = (1 << 64) - 1
ACTUAL_ENTRIES = 512
ACTUAL_BUDGET = 16 * 1024 * 1024
HISTORY_ENTRIES = 65536
SCHEMA_VERSION = 2

PHASE_MARKER = "[A64_DYNAREC_PHASE6AL"
PREFIXES = {
    "MAIN": "[A64_DYNAREC_PHASE6AL]",
    "CAUSE": "[A64_DYNAREC_PHASE6AL_CAUSE]",
    "MODEL": "[A64_DYNAREC_PHASE6AL_MODEL]",
    "END": "[A64_DYNAREC_PHASE6AL_END]",
}

MAIN_FIELDS = (
    "version",
    "owner",
    "owners",
    "role",
    "primary_markers",
    "primary_allocations",
    "primary_owner",
    "role_invalid",
    "process_invalid",
    "process_valid",
    "final",
    "report",
    "interval",
    "valid",
    "actual_probe",
    "actual_hit",
    "actual_miss",
    "compile_ok",
    "compile_fail",
    "compile_ns",
    "put",
    "stored",
    "set_drop",
    "budget_drop",
    "reset",
    "resident",
    "peak",
    "capacity",
    "capacity_peak",
    "global_capacity",
    "global_peak",
    "global_invalid",
    "history",
    "history_drop",
    "one_shot",
    "one_shot_ns",
    "sig_admission",
    "sig_pre_restore",
    "sig_post_compile",
    "wasted_build_ns",
    "duplicate",
    "token_stale",
    "unclassified",
    "invalid",
)
CAUSE_FIELDS = (
    "version",
    "owner",
    "cause",
    "events",
    "build_ns",
    "code_capacity",
    "fa_hit",
    "fa_saved_ns",
    "k16_hit",
    "k16_saved_ns",
    "k32_hit",
    "k32_saved_ns",
    "k32_only_hit",
    "k32_only_saved_ns",
    "k16_only_hit",
    "k16_only_saved_ns",
)
MODEL_FIELDS = (
    "version",
    "owner",
    "model",
    "entries",
    "ways",
    "budget",
    "probe",
    "hit",
    "miss",
    "put",
    "stored",
    "drop",
    "budget_drop",
    "resident",
    "peak",
    "capacity",
    "capacity_peak",
    "saved",
    "saved_ns",
    "invalid",
    "global_capacity",
    "global_peak",
    "global_invalid",
)
END_FIELDS = ("version", "owner", "complete", "valid", "report")

MAIN_CUMULATIVE_FIELDS = (
    "primary_markers", "primary_allocations", "role_invalid", "process_invalid",
    "actual_probe", "actual_hit", "actual_miss", "compile_ok",
    "compile_fail", "compile_ns", "put", "stored", "set_drop",
    "budget_drop", "reset", "peak", "capacity_peak", "global_peak",
    "global_invalid", "history", "history_drop", "sig_admission",
    "sig_pre_restore", "sig_post_compile", "wasted_build_ns", "duplicate",
    "token_stale", "unclassified", "invalid",
)
MODEL_CUMULATIVE_FIELDS = (
    "probe", "hit", "miss", "put", "stored", "drop", "budget_drop",
    "peak", "capacity_peak", "saved", "saved_ns", "invalid",
    "global_peak", "global_invalid",
)
CAUSE_ROSTER = ("cold", "variant", "conflict", "capacity", "budget")
MODEL_ROSTER = ("FA512", "K1024_16", "K1024_32")
MODEL_GEOMETRY = {
    "FA512": (512, 512, 0),
    "K1024_16": (1024, 4, 16 * 1024 * 1024),
    "K1024_32": (1024, 4, 32 * 1024 * 1024),
}

ROLE_ROSTER = ("standalone", "primary", "worker")
TEXT_FIELDS = {"cause", "model", "role"}
DECIMAL_RE = re.compile(r"(?:0|[1-9][0-9]*)\Z")

Value = Union[int, str]
Record = Dict[str, Value]


class ValidationError(Exception):
    """A report is not canonical or violates a frozen invariant."""


@dataclass(frozen=True)
class Frame:
    source: str
    start_line: int
    main: Record
    causes: Tuple[Record, ...]
    models: Tuple[Record, ...]
    end: Record

    @property
    def owner(self) -> int:
        return _number(self.main, "owner")

    @property
    def report_number(self) -> int:
        return _number(self.main, "report")

    @property
    def final(self) -> bool:
        return _number(self.main, "final") == 1

    @property
    def role(self) -> str:
        return _text(self.main, "role")


@dataclass(frozen=True)
class ReportLog:
    source: str
    frames: Tuple[Frame, ...]

    @property
    def final_frames(self) -> Tuple[Frame, ...]:
        return tuple(frame for frame in self.frames if frame.final)

    @property
    def last(self) -> Frame:
        if not self.frames:
            raise AssertionError("validated report log has no frames")
        return self.frames[-1]


def _number(record: Mapping[str, Value], field: str) -> int:
    value = record[field]
    if not isinstance(value, int):
        raise AssertionError(f"internal type mismatch for {field}")
    return value


def _text(record: Mapping[str, Value], field: str) -> str:
    value = record[field]
    if not isinstance(value, str):
        raise AssertionError(f"internal type mismatch for {field}")
    return value


def _require(condition: bool, source: str, message: str) -> None:
    if not condition:
        raise ValidationError(f"{source}: {message}")


def _sum_u64(values: Sequence[int], source: str, name: str) -> int:
    total = sum(values)
    _require(total <= UINT64_MAX, source, f"{name} overflows uint64")
    return total


def _parse_decimal(value: str, source: str, line_number: int, field: str) -> int:
    _require(
        DECIMAL_RE.fullmatch(value) is not None,
        source,
        f"line {line_number}: {field} is not canonical unsigned decimal",
    )
    _require(
        len(value) <= 20,
        source,
        f"line {line_number}: {field} exceeds uint64",
    )
    parsed = int(value, 10)
    _require(
        parsed <= UINT64_MAX,
        source,
        f"line {line_number}: {field} exceeds uint64",
    )
    return parsed


def _parse_record(
    line: str,
    line_number: int,
    source: str,
    kind: str,
    fields: Sequence[str],
) -> Record:
    _require(line.isascii(), source, f"line {line_number}: record is not ASCII")
    _require(
        line == line.strip() and "\t" not in line and "  " not in line,
        source,
        f"line {line_number}: non-canonical record whitespace",
    )
    tokens = line.split(" ")
    _require(
        len(tokens) == len(fields) + 1,
        source,
        f"line {line_number}: {kind} has extra or missing fields",
    )
    _require(
        tokens[0] == PREFIXES[kind],
        source,
        f"line {line_number}: unexpected {kind} prefix",
    )
    record: Record = {}
    for token, field in zip(tokens[1:], fields):
        expected = field + "="
        _require(
            token.startswith(expected),
            source,
            f"line {line_number}: expected field {field}",
        )
        raw = token[len(expected) :]
        _require(raw != "", source, f"line {line_number}: empty {field}")
        if field in TEXT_FIELDS:
            record[field] = raw
        else:
            record[field] = _parse_decimal(raw, source, line_number, field)
    _require(
        _number(record, "version") == SCHEMA_VERSION,
        source,
        f"line {line_number}: unsupported {kind} version",
    )
    return record


def _validate_main_bounds(main: Mapping[str, Value], source: str) -> None:
    _require(
        0 < _number(main, "owner") <= _number(main, "owners"),
        source,
        "owner is outside the allocated-owner roster",
    )
    _require(_number(main, "interval") > 0, source, "interval is zero")
    role = _text(main, "role")
    owner = _number(main, "owner")
    owners = _number(main, "owners")
    primary_markers = _number(main, "primary_markers")
    primary_allocations = _number(main, "primary_allocations")
    primary_owner = _number(main, "primary_owner")
    _require(role in ROLE_ROSTER, source, f"unknown owner role {role}")
    _require(
        _number(main, "process_valid") in (0, 1),
        source,
        "process_valid is not boolean",
    )
    _require(
        primary_owner <= owners,
        source,
        "primary_owner is outside the allocated-owner roster",
    )
    _require(primary_markers <= 1, source, "primary_markers exceeds one")
    _require(primary_allocations <= 1, source, "primary_allocations exceeds one")
    _require(
        (primary_allocations == 0) == (primary_owner == 0),
        source,
        "primary allocation/owner emptiness mismatch",
    )
    if role != "standalone":
        _require(
            primary_markers == 1,
            source,
            f"{role} owner does not report exactly one primary marker",
        )
    if role == "primary":
        _require(
            primary_allocations == 1 and primary_owner != 0,
            source,
            "primary owner is missing its primary allocation",
        )
        _require(owner == primary_owner, source, "primary owner does not match primary_owner")
    if role == "worker" and primary_allocations == 1:
        _require(owner != primary_owner, source, "worker owner matches primary_owner")
    probes = _number(main, "actual_probe")
    hits = _number(main, "actual_hit")
    misses = _number(main, "actual_miss")
    compile_ok = _number(main, "compile_ok")
    compile_fail = _number(main, "compile_fail")
    puts = _number(main, "put")
    stored = _number(main, "stored")
    set_drops = _number(main, "set_drop")
    budget_drops = _number(main, "budget_drop")
    resets = _number(main, "reset")
    resident = _number(main, "resident")
    peak = _number(main, "peak")
    capacity = _number(main, "capacity")
    capacity_peak = _number(main, "capacity_peak")
    global_capacity = _number(main, "global_capacity")
    global_peak = _number(main, "global_peak")

    _require(probes == hits + misses, source, "actual_probe != actual_hit + actual_miss")
    _require(
        compile_ok == misses and compile_fail == 0,
        source,
        "actual_miss != compile_ok or compile_fail != 0",
    )
    _require(puts == stored + budget_drops, source, "put != stored + budget_drop")
    _require(set_drops <= stored, source, "set_drop exceeds stored")
    _require(
        stored == resident + hits + set_drops + resets,
        source,
        "stored != resident + actual_hit + set_drop + reset",
    )
    _require(resident <= peak <= ACTUAL_ENTRIES, source, "actual resident/peak exceeds entry bound")
    _require(peak <= stored, source, "actual peak exceeds stored")
    _require(capacity >= resident, source, "actual capacity is smaller than resident count")
    _require(
        (resident == 0) == (capacity == 0),
        source,
        "actual resident/capacity emptiness mismatch",
    )
    _require(
        capacity <= capacity_peak <= ACTUAL_BUDGET,
        source,
        "actual capacity/capacity_peak exceeds 16 MiB",
    )
    _require(peak <= capacity_peak, source, "actual peak exceeds capacity_peak")
    _require(
        global_capacity <= global_peak <= ACTUAL_BUDGET,
        source,
        "actual global capacity/global_peak exceeds 16 MiB",
    )
    _require(
        capacity <= global_capacity and capacity_peak <= global_peak,
        source,
        "actual local capacity exceeds process-global capacity",
    )
    _require(
        _number(main, "history") <= HISTORY_ENTRIES,
        source,
        "history exceeds fixed history capacity",
    )
    _require(
        _number(main, "one_shot") <= _number(main, "history"),
        source,
        "one_shot exceeds history",
    )
    _require(
        _number(main, "one_shot") != 0 or _number(main, "one_shot_ns") == 0,
        source,
        "one_shot_ns is nonzero without one_shot events",
    )
    _require(
        _number(main, "sig_post_compile") <= compile_ok,
        source,
        "sig_post_compile exceeds compile_ok",
    )
    _require(
        _number(main, "wasted_build_ns") <= _number(main, "compile_ns"),
        source,
        "wasted_build_ns exceeds compile_ns",
    )
    _require(
        _number(main, "sig_post_compile") != 0 or
        _number(main, "wasted_build_ns") == 0,
        source,
        "wasted_build_ns is nonzero without sig_post_compile",
    )


def _validate_model(
    model: Mapping[str, Value],
    main: Mapping[str, Value],
    expected_name: str,
    source: str,
    final: bool,
) -> None:
    name = _text(model, "model")
    _require(name == expected_name, source, f"expected model {expected_name}, got {name}")
    entries, ways, budget = MODEL_GEOMETRY[expected_name]
    _require(_number(model, "entries") == entries, source, f"{name} entries mismatch")
    _require(_number(model, "ways") == ways, source, f"{name} ways mismatch")
    _require(_number(model, "budget") == budget, source, f"{name} budget mismatch")

    probes = _number(model, "probe")
    hits = _number(model, "hit")
    misses = _number(model, "miss")
    puts = _number(model, "put")
    stored = _number(model, "stored")
    drops = _number(model, "drop")
    budget_drops = _number(model, "budget_drop")
    resident = _number(model, "resident")
    peak = _number(model, "peak")
    capacity = _number(model, "capacity")
    capacity_peak = _number(model, "capacity_peak")
    saved = _number(model, "saved")
    saved_ns = _number(model, "saved_ns")
    global_capacity = _number(model, "global_capacity")
    global_peak = _number(model, "global_peak")

    _require(probes == hits + misses, source, f"{name} probe != hit + miss")
    _require(puts == stored + budget_drops, source, f"{name} put != stored + budget_drop")
    _require(probes == _number(main, "actual_probe"), source, f"{name} probe != actual_probe")
    _require(puts == _number(main, "put"), source, f"{name} put != actual put")
    _require(drops <= stored, source, f"{name} drop exceeds stored")
    _require(hits <= stored - drops, source, f"{name} hit + drop exceeds stored")
    if not final:
        _require(
            resident == stored - hits - drops,
            source,
            f"{name} stored != resident + hit + drop",
        )
    _require(resident <= peak <= entries, source, f"{name} resident/peak exceeds entry bound")
    _require(peak <= stored, source, f"{name} peak exceeds stored")
    _require(saved <= hits, source, f"{name} saved exceeds hit")
    _require(saved_ns <= _number(main, "compile_ns"), source, f"{name} saved_ns exceeds compile_ns")
    _require(
        saved != 0 or saved_ns == 0,
        source,
        f"{name} saved_ns is nonzero without saved events",
    )

    if budget == 0:
        _require(budget_drops == 0, source, f"{name} has a budget drop without a budget")
        _require(
            capacity == 0 and capacity_peak == 0,
            source,
            f"{name} reports code capacity",
        )
        _require(
            global_capacity == 0 and global_peak == 0,
            source,
            f"{name} reports global capacity",
        )
    else:
        _require(capacity >= resident, source, f"{name} capacity is smaller than resident count")
        _require(
            (resident == 0) == (capacity == 0),
            source,
            f"{name} resident/capacity emptiness mismatch",
        )
        _require(
            capacity <= capacity_peak <= budget,
            source,
            f"{name} capacity/capacity_peak exceeds budget",
        )
        _require(peak <= capacity_peak, source, f"{name} peak exceeds capacity_peak")
        _require(
            global_capacity <= global_peak <= budget,
            source,
            f"{name} global capacity/global_peak exceeds budget",
        )
        _require(
            capacity <= global_capacity and capacity_peak <= global_peak,
            source,
            f"{name} local capacity exceeds process-global capacity",
        )

    _require(_number(model, "invalid") == 0, source, f"{name} invalid is nonzero")
    _require(
        _number(model, "global_invalid") == 0,
        source,
        f"{name} global_invalid is nonzero",
    )


def _validate_frame(frame: Frame) -> None:
    source = frame.source
    main = frame.main
    end = frame.end
    owner = _number(main, "owner")
    _require(owner != 0, source, "owner is zero")

    _require(len(frame.causes) == len(CAUSE_ROSTER), source, "cause row count is not five")
    _require(len(frame.models) == len(MODEL_ROSTER), source, "model row count is not three")
    for cause, expected_name in zip(frame.causes, CAUSE_ROSTER):
        _require(_number(cause, "owner") == owner, source, f"{expected_name} owner mismatch")
        _require(
            _text(cause, "cause") == expected_name,
            source,
            f"expected cause {expected_name}, got {_text(cause, 'cause')}",
        )
        events = _number(cause, "events")
        code_capacity = _number(cause, "code_capacity")
        _require(
            events != 0 or all(
                _number(cause, field) == 0
                for field in CAUSE_FIELDS
                if field not in ("version", "owner", "cause", "events")
            ),
            source,
            f"{expected_name} has payload without events",
        )
        _require(
            events == 0 or code_capacity >= events,
            source,
            f"{expected_name} code_capacity is below event count",
        )
        build_ns = _number(cause, "build_ns")
        for hit_field, saved_ns_field in (
            ("fa_hit", "fa_saved_ns"),
            ("k16_hit", "k16_saved_ns"),
            ("k32_hit", "k32_saved_ns"),
        ):
            _require(
                _number(cause, hit_field) <= events,
                source,
                f"{expected_name} {hit_field} exceeds events",
            )
            _require(
                _number(cause, saved_ns_field) <= build_ns,
                source,
                f"{expected_name} {saved_ns_field} exceeds build_ns",
            )
            _require(
                _number(cause, hit_field) != 0 or
                _number(cause, saved_ns_field) == 0,
                source,
                f"{expected_name} {saved_ns_field} is nonzero without a hit",
            )
        k16_hit = _number(cause, "k16_hit")
        k32_hit = _number(cause, "k32_hit")
        k32_only_hit = _number(cause, "k32_only_hit")
        k16_only_hit = _number(cause, "k16_only_hit")
        k16_saved_ns = _number(cause, "k16_saved_ns")
        k32_saved_ns = _number(cause, "k32_saved_ns")
        k32_only_saved_ns = _number(cause, "k32_only_saved_ns")
        k16_only_saved_ns = _number(cause, "k16_only_saved_ns")
        _require(
            k32_only_hit <= k32_hit and k16_only_hit <= k16_hit,
            source,
            f"{expected_name} only-hit exceeds its marginal hit count",
        )
        _require(
            k32_only_hit <= events - k16_hit and
            k16_only_hit <= events - k32_hit,
            source,
            f"{expected_name} only-hit overlaps the opposite marginal",
        )
        _require(
            k16_hit - k16_only_hit == k32_hit - k32_only_hit,
            source,
            f"{expected_name} joint hit marginals are inconsistent",
        )
        _require(
            k32_only_saved_ns <= k32_saved_ns and
            k16_only_saved_ns <= k16_saved_ns,
            source,
            f"{expected_name} only-hit saved time exceeds its marginal",
        )
        _require(
            (k32_only_hit != 0 or k32_only_saved_ns == 0) and
            (k16_only_hit != 0 or k16_only_saved_ns == 0),
            source,
            f"{expected_name} only-hit saved time is nonzero without a hit",
        )
        _require(
            k16_saved_ns - k16_only_saved_ns ==
            k32_saved_ns - k32_only_saved_ns,
            source,
            f"{expected_name} joint saved-time marginals are inconsistent",
        )
        _require(
            k32_only_saved_ns <= build_ns - k16_saved_ns and
            k16_only_saved_ns <= build_ns - k32_saved_ns,
            source,
            f"{expected_name} only-hit saved-time union exceeds build_ns",
        )
        if expected_name in ("cold", "variant"):
            _require(
                all(
                    _number(cause, field) == 0
                    for field in (
                        "fa_hit", "fa_saved_ns", "k16_hit", "k16_saved_ns",
                        "k32_hit", "k32_saved_ns", "k32_only_hit",
                        "k32_only_saved_ns", "k16_only_hit",
                        "k16_only_saved_ns",
                    )
                ),
                source,
                f"{expected_name} unexpectedly recovered in a shadow model",
            )
        elif expected_name == "conflict":
            _require(
                _number(cause, "fa_hit") == events and
                _number(cause, "fa_saved_ns") == build_ns,
                source,
                "conflict is not an exact FA512 recovery",
            )
        elif expected_name == "capacity":
            _require(
                _number(cause, "fa_hit") == 0 and
                _number(cause, "fa_saved_ns") == 0,
                source,
                "capacity unexpectedly recovered in FA512",
            )

    _require(_number(end, "owner") == owner, source, "END owner mismatch")
    _require(_number(main, "report") != 0, source, "report number is zero")
    _require(_number(end, "report") == _number(main, "report"), source, "END report mismatch")
    _require(_number(main, "final") in (0, 1), source, "final is not boolean")
    _require(_number(main, "valid") == 1, source, "MAIN valid is not 1")
    _require(_number(end, "valid") == 1, source, "END valid is not 1")
    _require(_number(end, "complete") == 1, source, "END complete is not 1")

    _validate_main_bounds(main, source)

    cause_events = _sum_u64(
        [_number(cause, "events") for cause in frame.causes],
        source,
        "cause events",
    )
    cause_build_ns = _sum_u64(
        [_number(cause, "build_ns") for cause in frame.causes],
        source,
        "cause build_ns",
    )
    _require(_number(main, "compile_ok") == cause_events, source, "compile_ok != sum cause events")
    _require(_number(main, "compile_ns") == cause_build_ns, source, "compile_ns != sum cause build_ns")

    cold_variant_events = _sum_u64(
        [_number(frame.causes[index], "events") for index in (0, 1)],
        source,
        "cold + variant events",
    )
    cold_variant_ns = _sum_u64(
        [_number(frame.causes[index], "build_ns") for index in (0, 1)],
        source,
        "cold + variant build_ns",
    )
    _require(
        _number(main, "history") == cold_variant_events,
        source,
        "history != cold + variant events",
    )
    _require(
        _number(main, "one_shot") <= cold_variant_events,
        source,
        "one_shot exceeds cold + variant events",
    )
    _require(
        _number(main, "one_shot_ns") <= cold_variant_ns,
        source,
        "one_shot_ns exceeds cold + variant build_ns",
    )

    for model, expected_name in zip(frame.models, MODEL_ROSTER):
        _require(
            _number(model, "owner") == owner,
            source,
            f"{expected_name} owner mismatch",
        )
        _validate_model(
            model,
            main,
            expected_name,
            source,
            _number(main, "final") == 1,
        )

    for model_index, (hit_field, saved_ns_field) in enumerate(
        (
            ("fa_hit", "fa_saved_ns"),
            ("k16_hit", "k16_saved_ns"),
            ("k32_hit", "k32_saved_ns"),
        )
    ):
        model = frame.models[model_index]
        cause_hits = _sum_u64(
            [_number(cause, hit_field) for cause in frame.causes],
            source,
            f"cause {hit_field}",
        )
        cause_saved_ns = _sum_u64(
            [_number(cause, saved_ns_field) for cause in frame.causes],
            source,
            f"cause {saved_ns_field}",
        )
        _require(
            _number(model, "saved") == cause_hits,
            source,
            f"{_text(model, 'model')} saved != sum cause {hit_field}",
        )
        _require(
            _number(model, "saved_ns") == cause_saved_ns,
            source,
            f"{_text(model, 'model')} saved_ns != sum cause {saved_ns_field}",
        )

    for field in (
        "role_invalid",
        "process_invalid",
        "global_invalid",
        "history_drop",
        "duplicate",
        "token_stale",
        "unclassified",
        "invalid",
    ):
        _require(_number(main, field) == 0, source, f"{field} is nonzero")
    _require(_number(main, "process_valid") == 1, source, "process_valid is not 1")


def _parse_kind(line: str) -> Optional[str]:
    for kind, prefix in PREFIXES.items():
        if line.startswith(prefix + " "):
            return kind
    return None


def _validate_monotonic(previous: Frame, current: Frame, source: str) -> None:
    _require(
        current.role == previous.role,
        source,
        f"owner {current.owner} role changed from {previous.role} to {current.role}",
    )
    _require(
        _number(current.main, "owners") >= _number(previous.main, "owners"),
        source,
        f"owner {current.owner} allocated-owner roster decreased",
    )
    _require(
        _number(current.main, "interval") == _number(previous.main, "interval"),
        source,
        f"owner {current.owner} interval changed",
    )
    for field in MAIN_CUMULATIVE_FIELDS:
        _require(
            _number(current.main, field) >= _number(previous.main, field),
            source,
            f"owner {current.owner} cumulative MAIN field {field} decreased",
        )
    cause_numeric_fields = tuple(
        field
        for field in CAUSE_FIELDS
        if field not in ("version", "owner", "cause")
    )
    for previous_cause, current_cause in zip(previous.causes, current.causes):
        for field in cause_numeric_fields:
            _require(
                _number(current_cause, field) >= _number(previous_cause, field),
                source,
                f"owner {current.owner} cumulative cause "
                f"{_text(current_cause, 'cause')}.{field} decreased",
            )
    for previous_model, current_model in zip(previous.models, current.models):
        for field in MODEL_CUMULATIVE_FIELDS:
            _require(
                _number(current_model, field) >= _number(previous_model, field),
                source,
                f"owner {current.owner} cumulative model "
                f"{_text(current_model, 'model')}.{field} decreased",
            )


def parse_report_text(text: str, source: str = "<stdin>") -> ReportLog:
    """Parse all phase-6AL v2 frames found in *text* and validate them."""

    frames: List[Frame] = []
    main: Optional[Record] = None
    causes: List[Record] = []
    models: List[Record] = []
    expected = "MAIN"
    frame_start = 0

    for line_number, line in enumerate(text.splitlines(), start=1):
        if PHASE_MARKER not in line:
            _require(
                expected == "MAIN",
                source,
                f"line {line_number}: unrelated output splits a phase-6AL frame",
            )
            continue
        kind = _parse_kind(line)
        _require(kind is not None, source, f"line {line_number}: unknown phase-6AL record")
        if expected == "MAIN":
            _require(kind == "MAIN", source, f"line {line_number}: expected MAIN")
            main = _parse_record(line, line_number, source, "MAIN", MAIN_FIELDS)
            causes = []
            models = []
            expected = "CAUSE"
            frame_start = line_number
            continue
        if expected == "CAUSE":
            _require(kind == "CAUSE", source, f"line {line_number}: expected CAUSE")
            causes.append(_parse_record(line, line_number, source, "CAUSE", CAUSE_FIELDS))
            if len(causes) == len(CAUSE_ROSTER):
                expected = "MODEL"
            continue
        if expected == "MODEL":
            _require(kind == "MODEL", source, f"line {line_number}: expected MODEL")
            models.append(_parse_record(line, line_number, source, "MODEL", MODEL_FIELDS))
            if len(models) == len(MODEL_ROSTER):
                expected = "END"
            continue

        _require(expected == "END", source, "internal parser state is invalid")
        _require(kind == "END", source, f"line {line_number}: expected END")
        end = _parse_record(line, line_number, source, "END", END_FIELDS)
        if main is None:
            raise AssertionError("END reached without MAIN")
        frame_source = f"{source}: frame starting line {frame_start}"
        frame = Frame(frame_source, frame_start, main, tuple(causes), tuple(models), end)
        _validate_frame(frame)
        frames.append(frame)
        main = None
        causes = []
        models = []
        expected = "MAIN"

    _require(
        expected == "MAIN",
        source,
        f"truncated phase-6AL frame starting at line {frame_start}",
    )
    _require(bool(frames), source, "no phase-6AL report frame found")

    last_report_by_owner: Dict[int, int] = {}
    last_frame_by_owner: Dict[int, Frame] = {}
    final_owners = set()
    seen_keys = set()
    last_allocated_owners = 0
    last_actual_global_peak = 0
    last_model_global_peaks = [0] * len(MODEL_ROSTER)
    last_primary_markers = 0
    primary_allocation_seen = False
    established_primary_owner = 0
    for frame in frames:
        key = (frame.owner, frame.report_number)
        _require(key not in seen_keys, source, f"duplicate frame owner={key[0]} report={key[1]}")
        seen_keys.add(key)
        allocated_owners = _number(frame.main, "owners")
        _require(
            allocated_owners >= last_allocated_owners,
            source,
            "process allocated-owner roster decreased",
        )
        last_allocated_owners = allocated_owners
        primary_markers = _number(frame.main, "primary_markers")
        _require(
            primary_markers >= last_primary_markers,
            source,
            "process primary marker count regressed",
        )
        last_primary_markers = primary_markers
        primary_allocations = _number(frame.main, "primary_allocations")
        primary_owner = _number(frame.main, "primary_owner")
        if primary_allocation_seen:
            _require(
                primary_allocations == 1 and
                primary_owner == established_primary_owner,
                source,
                "process primary allocation regressed or changed owner",
            )
        elif primary_allocations == 1:
            primary_allocation_seen = True
            established_primary_owner = primary_owner
        actual_global_peak = _number(frame.main, "global_peak")
        _require(
            actual_global_peak >= last_actual_global_peak,
            source,
            "actual process-global peak decreased",
        )
        last_actual_global_peak = actual_global_peak
        for model_index, model in enumerate(frame.models):
            model_global_peak = _number(model, "global_peak")
            _require(
                model_global_peak >= last_model_global_peaks[model_index],
                source,
                f"{_text(model, 'model')} process-global peak decreased",
            )
            last_model_global_peaks[model_index] = model_global_peak
        _require(
            frame.owner not in final_owners,
            source,
            f"frame follows final report for owner {frame.owner}",
        )
        previous = last_report_by_owner.get(frame.owner)
        _require(
            frame.report_number == (1 if previous is None else previous + 1),
            source,
            f"report sequence is not contiguous for owner {frame.owner}",
        )
        previous_frame = last_frame_by_owner.get(frame.owner)
        if previous_frame is not None:
            _validate_monotonic(previous_frame, frame, source)
        last_report_by_owner[frame.owner] = frame.report_number
        last_frame_by_owner[frame.owner] = frame
        if frame.final:
            final_owners.add(frame.owner)

    return ReportLog(source, tuple(frames))


def require_final_report(report: ReportLog) -> Frame:
    """Return the terminal frame after every observed owner has finalized."""

    _require(bool(report.final_frames), report.source, "no final complete=1 frame found")
    observed_owners = {frame.owner for frame in report.frames}
    final_owners = {frame.owner for frame in report.final_frames}
    allocated_owners = max(_number(frame.main, "owners") for frame in report.frames)
    _require(
        len(observed_owners) == allocated_owners and
        min(observed_owners) == 1 and
        max(observed_owners) == allocated_owners,
        report.source,
        "final report roster omits an allocated telemetry owner",
    )
    _require(
        final_owners == observed_owners,
        report.source,
        "not every observed telemetry owner has a final frame",
    )
    for final in report.final_frames:
        _require(
            _number(final.main, "resident") == 0 and
            _number(final.main, "capacity") == 0,
            report.source,
            f"final actual archive is not empty for owner {final.owner}",
        )
        for model in final.models:
            _require(
                _number(model, "resident") == 0 and
                _number(model, "capacity") == 0,
                report.source,
                f"final {_text(model, 'model')} is not empty for owner {final.owner}",
            )
    frame = report.final_frames[-1]
    _require(_number(frame.end, "complete") == 1, report.source, "final END complete is not 1")
    _require(_number(frame.end, "valid") == 1, report.source, "final END valid is not 1")
    _require(
        _number(frame.main, "global_capacity") == 0,
        report.source,
        "last final actual global capacity is not zero",
    )
    for model in frame.models:
        _require(
            _number(model, "global_capacity") == 0,
            report.source,
            f"last final {_text(model, 'model')} global capacity is not zero",
        )
    return frame


def require_interval(report: ReportLog, expected: int) -> None:
    """Require one positive, preregistered report cadence for every frame."""

    _require(expected > 0, report.source, "expected interval must be positive")
    for frame in report.frames:
        _require(
            _number(frame.main, "interval") == expected,
            report.source,
            f"owner {frame.owner} report {frame.report_number} interval mismatch",
        )


def require_single_owner(report: ReportLog) -> int:
    owners = {frame.owner for frame in report.frames}
    _require(len(owners) == 1, report.source, "report contains more than one owner")
    _require(
        all(_number(frame.main, "owners") == 1 for frame in report.frames),
        report.source,
        "process allocated more than one telemetry owner",
    )
    return next(iter(owners))


def select_first_frame_after_present(
    report: ReportLog, text: str, present: int
) -> Frame:
    _require(present > 0, report.source, "present cutoff must be positive")
    primary_owners = {
        frame.owner for frame in report.frames if frame.role == "primary"
    }
    _require(
        len(primary_owners) == 1,
        report.source,
        "live log does not contain exactly one observed primary owner",
    )
    primary_owner = next(iter(primary_owners))
    pattern = re.compile(
        rf"^\[FRAME_PIPELINE\] present={present}(?:\s|$)"
    )
    present_lines = [
        line_number
        for line_number, line in enumerate(text.splitlines(), start=1)
        if pattern.match(line) is not None
    ]
    _require(
        len(present_lines) == 1,
        report.source,
        f"expected exactly one FRAME_PIPELINE present={present} line",
    )
    selected = next(
        (
            frame
            for frame in report.frames
            if frame.start_line > present_lines[0] and frame.role == "primary"
        ),
        None,
    )
    _require(
        selected is not None,
        report.source,
        f"no complete primary phase-6AL frame follows present={present}",
    )
    _require(
        selected.owner == primary_owner,
        report.source,
        "selected owner is not the observed primary owner",
    )
    _require(
        selected.owner == _number(selected.main, "primary_owner"),
        report.source,
        "selected owner does not match primary_owner",
    )
    _require(
        _number(selected.main, "primary_markers") == 1,
        report.source,
        "live cutoff primary_markers is not 1",
    )
    _require(
        _number(selected.main, "primary_allocations") == 1,
        report.source,
        "live cutoff primary_allocations is not 1",
    )
    _require(
        _number(selected.main, "role_invalid") == 0,
        report.source,
        "live cutoff role_invalid is not 0",
    )
    _require(
        _number(selected.main, "process_invalid") == 0,
        report.source,
        "live cutoff process_invalid is not 0",
    )
    _require(
        _number(selected.main, "process_valid") == 1,
        report.source,
        "live cutoff process_valid is not 1",
    )
    _require(not selected.final, report.source, "live cutoff selected a final frame")
    _require(
        _number(selected.main, "interval") == 100000,
        report.source,
        "live cutoff interval is not the preregistered 100000",
    )
    return selected


def _fixture(
    final: bool = True,
    report_number: int = 1,
    *,
    owner: int = 1,
    owners: int = 1,
    role: str = "primary",
    primary_markers: Optional[int] = None,
    primary_allocations: Optional[int] = None,
    primary_owner: Optional[int] = None,
) -> str:
    if primary_markers is None:
        primary_markers = 0 if role == "standalone" else 1
    if primary_allocations is None:
        primary_allocations = 0 if role == "standalone" else 1
    if primary_owner is None:
        primary_owner = 0 if role == "standalone" else (owner if role == "primary" else 1)
    final_number = 1 if final else 0
    actual_reset = 2 if final else 0
    actual_resident = 0 if final else 2
    actual_capacity = 0 if final else 2000
    fa_resident = 0 if final else 5
    k16_resident = 0 if final else 2
    k32_resident = 0 if final else 2
    k16_capacity = 0 if final else 2000
    k32_capacity = 0 if final else 2000
    return "\n".join(
        (
            "unrelated-before",
            f"[A64_DYNAREC_PHASE6AL] version=2 owner={owner} owners={owners} "
            f"role={role} primary_markers={primary_markers} "
            f"primary_allocations={primary_allocations} primary_owner={primary_owner} "
            f"role_invalid=0 process_invalid=0 process_valid=1 "
            f"final={final_number} report={report_number} "
            "interval=100000 valid=1 "
            "actual_probe=10 actual_hit=3 actual_miss=7 compile_ok=7 "
            "compile_fail=0 compile_ns=7000 put=8 stored=7 set_drop=2 "
            f"budget_drop=1 reset={actual_reset} resident={actual_resident} "
            f"peak=5 capacity={actual_capacity} capacity_peak=5000 "
            f"global_capacity={actual_capacity} global_peak=5000 global_invalid=0 history=3 "
            "history_drop=0 one_shot=3 one_shot_ns=2500 sig_admission=0 "
            "sig_pre_restore=0 sig_post_compile=1 wasted_build_ns=1000 "
            "duplicate=0 token_stale=0 unclassified=0 invalid=0",
            f"[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner={owner} cause=cold "
            "events=2 build_ns=1800 code_capacity=2000 fa_hit=0 fa_saved_ns=0 "
            "k16_hit=0 k16_saved_ns=0 k32_hit=0 k32_saved_ns=0 "
            "k32_only_hit=0 k32_only_saved_ns=0 k16_only_hit=0 k16_only_saved_ns=0",
            f"[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner={owner} cause=variant "
            "events=1 build_ns=700 code_capacity=1000 fa_hit=0 fa_saved_ns=0 "
            "k16_hit=0 k16_saved_ns=0 k32_hit=0 k32_saved_ns=0 "
            "k32_only_hit=0 k32_only_saved_ns=0 k16_only_hit=0 k16_only_saved_ns=0",
            f"[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner={owner} cause=conflict "
            "events=1 build_ns=1200 code_capacity=1200 fa_hit=1 fa_saved_ns=1200 "
            "k16_hit=1 k16_saved_ns=1200 k32_hit=0 k32_saved_ns=0 "
            "k32_only_hit=0 k32_only_saved_ns=0 k16_only_hit=1 k16_only_saved_ns=1200",
            f"[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner={owner} cause=capacity "
            "events=2 build_ns=2300 code_capacity=2300 fa_hit=0 fa_saved_ns=0 "
            "k16_hit=0 k16_saved_ns=0 k32_hit=1 k32_saved_ns=1300 "
            "k32_only_hit=1 k32_only_saved_ns=1300 k16_only_hit=0 k16_only_saved_ns=0",
            f"[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner={owner} cause=budget "
            "events=1 build_ns=1000 code_capacity=1000 fa_hit=0 fa_saved_ns=0 "
            "k16_hit=0 k16_saved_ns=0 k32_hit=1 k32_saved_ns=1000 "
            "k32_only_hit=1 k32_only_saved_ns=1000 k16_only_hit=0 k16_only_saved_ns=0",
            f"[A64_DYNAREC_PHASE6AL_MODEL] version=2 owner={owner} model=FA512 entries=512 ways=512 "
            "budget=0 probe=10 hit=2 miss=8 put=8 stored=8 drop=1 budget_drop=0 "
            f"resident={fa_resident} peak=5 capacity=0 capacity_peak=0 "
            "saved=1 saved_ns=1200 "
            "invalid=0 global_capacity=0 global_peak=0 global_invalid=0",
            f"[A64_DYNAREC_PHASE6AL_MODEL] version=2 owner={owner} model=K1024_16 entries=1024 ways=4 "
            "budget=16777216 probe=10 hit=4 miss=6 put=8 stored=7 drop=1 "
            f"budget_drop=1 resident={k16_resident} peak=5 capacity={k16_capacity} "
            "capacity_peak=6000 saved=1 "
            f"saved_ns=1200 invalid=0 global_capacity={k16_capacity} global_peak=6000 "
            "global_invalid=0",
            f"[A64_DYNAREC_PHASE6AL_MODEL] version=2 owner={owner} model=K1024_32 entries=1024 ways=4 "
            "budget=33554432 probe=10 hit=5 miss=5 put=8 stored=8 drop=1 "
            f"budget_drop=0 resident={k32_resident} peak=6 capacity={k32_capacity} "
            "capacity_peak=7000 saved=2 "
            f"saved_ns=2300 invalid=0 global_capacity={k32_capacity} global_peak=7000 "
            "global_invalid=0",
            f"[A64_DYNAREC_PHASE6AL_END] version=2 owner={owner} "
            f"complete=1 valid=1 report={report_number}",
            "unrelated-after",
            "",
        )
    )


def _replace_once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise AssertionError(f"self-test replacement is not unique: {old!r}")
    return text.replace(old, new, 1)


def _expect_invalid(text: str, expected: str) -> None:
    try:
        parse_report_text(text, "self-test-invalid")
    except ValidationError as error:
        if expected not in str(error):
            raise AssertionError(
                f"unexpected validation error for {expected!r}: {error!s}"
            ) from error
    else:
        raise AssertionError(f"invalid fixture was accepted: {expected}")


def _expect_final_invalid(text: str, expected: str) -> None:
    parsed = parse_report_text(text, "self-test-final-invalid")
    try:
        require_final_report(parsed)
    except ValidationError as error:
        if expected not in str(error):
            raise AssertionError(
                f"unexpected final validation error for {expected!r}: {error!s}"
            ) from error
    else:
        raise AssertionError(f"invalid final lifecycle was accepted: {expected}")


def self_test() -> None:
    valid = _fixture()
    parsed = parse_report_text(valid, "self-test-valid")
    if len(parsed.frames) != 1 or require_final_report(parsed).report_number != 1:
        raise AssertionError("valid final fixture did not round-trip")

    periodic = parse_report_text(_fixture(False), "self-test-periodic")
    if periodic.last.final:
        raise AssertionError("periodic fixture was interpreted as final")
    try:
        require_final_report(periodic)
    except ValidationError as error:
        if "no final" not in str(error):
            raise AssertionError(f"unexpected missing-final error: {error!s}") from error
    else:
        raise AssertionError("periodic-only log passed the final-report gate")

    combined = _fixture(False, 1).replace("unrelated-after\n", "") + _fixture(True, 2)
    parsed_combined = parse_report_text(combined, "self-test-multiple")
    if len(parsed_combined.frames) != 2 or require_final_report(parsed_combined).report_number != 2:
        raise AssertionError("multiple-frame fixture did not round-trip")

    changed_interval = (
        _fixture(False, 1).replace("unrelated-after\n", "") +
        _fixture(True, 2).replace("interval=100000", "interval=200000", 1)
    )
    _expect_invalid(changed_interval, "owner 1 interval changed")

    decreasing = (
        _fixture(False, 1)
        .replace("unrelated-after\n", "")
        .replace("sig_admission=0", "sig_admission=1", 1) +
        _fixture(True, 2)
    )
    _expect_invalid(decreasing, "cumulative MAIN field sig_admission decreased")

    role_changed = (
        _fixture(False, 1).replace("unrelated-after\n", "") +
        _fixture(True, 2).replace("role=primary", "role=standalone", 1)
    )
    _expect_invalid(role_changed, "owner 1 role changed from primary to standalone")

    live_text = (
        _fixture(
            False,
            1,
            owner=1,
            owners=1,
            role="worker",
            primary_allocations=0,
            primary_owner=0,
        ).replace("unrelated-after\n", "") +
        "[FRAME_PIPELINE] present=120 avg_interval_ms=1\n" +
        _fixture(False, 1, owner=2, owners=3)
    )
    live_report = parse_report_text(live_text, "self-test-live-cutoff")
    live_frame = select_first_frame_after_present(live_report, live_text, 120)
    if live_frame.owner != 2 or live_frame.report_number != 1 or live_frame.final:
        raise AssertionError("periodic live cutoff selection failed")

    final_live_text = (
        "[FRAME_PIPELINE] present=120 avg_interval_ms=1\n" +
        _fixture(True, 1)
    )
    final_live_report = parse_report_text(final_live_text, "self-test-final-live-cutoff")
    try:
        select_first_frame_after_present(final_live_report, final_live_text, 120)
    except ValidationError as error:
        if "selected a final frame" not in str(error):
            raise AssertionError(f"unexpected final-cutoff error: {error!s}") from error
    else:
        raise AssertionError("a final frame passed the periodic live cutoff")

    wrong_interval_parts = live_text.rsplit("interval=100000", 1)
    if len(wrong_interval_parts) != 2:
        raise AssertionError("live fixture does not contain the selected interval")
    wrong_interval_live_text = "interval=200000".join(wrong_interval_parts)
    wrong_interval_live_report = parse_report_text(
        wrong_interval_live_text, "self-test-wrong-live-interval"
    )
    try:
        select_first_frame_after_present(
            wrong_interval_live_report, wrong_interval_live_text, 120
        )
    except ValidationError as error:
        if "preregistered 100000" not in str(error):
            raise AssertionError(f"unexpected live-interval error: {error!s}") from error
    else:
        raise AssertionError("the wrong live interval passed the cutoff selector")

    no_primary_text = (
        "[FRAME_PIPELINE] present=120 avg_interval_ms=1\n" +
        _fixture(False, role="standalone")
    )
    no_primary_report = parse_report_text(no_primary_text, "self-test-no-primary")
    try:
        select_first_frame_after_present(no_primary_report, no_primary_text, 120)
    except ValidationError as error:
        if "exactly one observed primary owner" not in str(error):
            raise AssertionError(f"unexpected no-primary error: {error!s}") from error
    else:
        raise AssertionError("a live log without a primary owner passed")

    multiple_primary_text = (
        "[FRAME_PIPELINE] present=120 avg_interval_ms=1\n" +
        _fixture(False, owner=1, owners=2) +
        _fixture(False, owner=2, owners=2)
    )
    _expect_invalid(
        multiple_primary_text,
        "process primary allocation regressed or changed owner",
    )

    hidden_primary_text = (
        "[FRAME_PIPELINE] present=120 avg_interval_ms=1\n" +
        _fixture(
            False,
            owner=2,
            owners=3,
            role="worker",
            primary_owner=1,
        )
    )
    hidden_primary_report = parse_report_text(
        hidden_primary_text, "self-test-hidden-primary-live"
    )
    try:
        select_first_frame_after_present(
            hidden_primary_report, hidden_primary_text, 120
        )
    except ValidationError as error:
        if "exactly one observed primary owner" not in str(error):
            raise AssertionError(f"unexpected hidden-primary error: {error!s}") from error
    else:
        raise AssertionError("an allocated but unobserved primary passed the live gate")
    _expect_invalid(
        hidden_primary_text.replace("process_invalid=0", "process_invalid=1", 1),
        "process_invalid is nonzero",
    )
    _expect_invalid(
        _fixture(
            False,
            owner=2,
            owners=2,
            role="worker",
            primary_owner=2,
        ),
        "worker owner matches primary_owner",
    )

    allocation_regression = (
        _fixture(
            False,
            1,
            owner=1,
            owners=1,
            role="worker",
            primary_allocations=0,
            primary_owner=0,
        ).replace("unrelated-after\n", "") +
        _fixture(False, 1, owner=2, owners=2).replace("unrelated-after\n", "") +
        _fixture(
            False,
            2,
            owner=1,
            owners=2,
            role="worker",
            primary_allocations=0,
            primary_owner=0,
        )
    )
    _expect_invalid(
        allocation_regression,
        "process primary allocation regressed or changed owner",
    )

    primary_owner_change = (
        _fixture(
            False,
            1,
            owner=1,
            owners=1,
            role="worker",
            primary_allocations=0,
            primary_owner=0,
        ).replace("unrelated-after\n", "") +
        _fixture(False, 1, owner=2, owners=2).replace("unrelated-after\n", "") +
        _fixture(
            False,
            2,
            owner=1,
            owners=3,
            role="worker",
            primary_allocations=1,
            primary_owner=3,
        )
    )
    _expect_invalid(
        primary_owner_change,
        "process primary allocation regressed or changed owner",
    )
    _expect_invalid(
        _fixture(False, primary_allocations=2),
        "primary_allocations exceeds one",
    )
    marker_regression = (
        _fixture(
            False,
            owner=1,
            owners=2,
            role="worker",
            primary_allocations=0,
            primary_owner=0,
        ).replace("unrelated-after\n", "") +
        _fixture(False, owner=2, owners=2, role="standalone")
    )
    _expect_invalid(marker_regression, "process primary marker count regressed")

    duplicate_marker_text = (
        "[FRAME_PIPELINE] present=120 avg_interval_ms=1\n" +
        _fixture(False, owners=3, primary_markers=2)
    )
    _expect_invalid(duplicate_marker_text, "primary_markers exceeds one")

    mutations = (
        (
            "[A64_DYNAREC_PHASE6AL] version=2",
            "[A64_DYNAREC_PHASE6AL] version=1",
            "unsupported MAIN version",
        ),
        (
            "[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner=1 cause=cold",
            "[A64_DYNAREC_PHASE6AL_CAUSE] version=1 owner=1 cause=cold",
            "unsupported CAUSE version",
        ),
        (
            "[A64_DYNAREC_PHASE6AL_MODEL] version=2 owner=1 model=FA512",
            "[A64_DYNAREC_PHASE6AL_MODEL] version=1 owner=1 model=FA512",
            "unsupported MODEL version",
        ),
        (
            "[A64_DYNAREC_PHASE6AL_END] version=2 owner=1",
            "[A64_DYNAREC_PHASE6AL_END] version=1 owner=1",
            "unsupported END version",
        ),
        ("role=primary", "role=unknown", "unknown owner role unknown"),
        ("role_invalid=0", "role_invalid=1", "role_invalid is nonzero"),
        ("process_invalid=0", "process_invalid=1", "process_invalid is nonzero"),
        ("process_valid=1", "process_valid=0", "process_valid is not 1"),
        ("cause=variant", "cause=cold", "expected cause variant"),
        ("model=K1024_16", "model=K1024_32", "expected model K1024_16"),
        ("entries=512 ways=512", "entries=511 ways=512", "FA512 entries mismatch"),
        ("budget=16777216", "budget=16777215", "K1024_16 budget mismatch"),
        ("interval=100000", "interval=0", "interval is zero"),
        ("actual_hit=3", "actual_hit=4", "actual_probe != actual_hit + actual_miss"),
        (
            "cause=variant events=1 build_ns=700",
            "cause=variant events=0 build_ns=700",
            "variant has payload without events",
        ),
        ("compile_fail=0", "compile_fail=1", "actual_miss != compile_ok or compile_fail != 0"),
        ("cause=capacity events=2", "cause=capacity events=1", "compile_ok != sum cause events"),
        ("compile_ns=7000", "compile_ns=6999", "compile_ns != sum cause build_ns"),
        ("fa_hit=1 fa_saved_ns=1200", "fa_hit=2 fa_saved_ns=1200", "conflict fa_hit exceeds events"),
        (
            "k32_hit=1 k32_saved_ns=1300 k32_only_hit=1 k32_only_saved_ns=1300",
            "k32_hit=1 k32_saved_ns=2301 k32_only_hit=1 k32_only_saved_ns=2301",
            "capacity k32_saved_ns exceeds build_ns",
        ),
        (
            "capacity_peak=0 saved=1 saved_ns=1200",
            "capacity_peak=0 saved=0 saved_ns=0",
            "FA512 saved != sum cause fa_hit",
        ),
        (
            "k32_only_hit=1 k32_only_saved_ns=1300",
            "k32_only_hit=0 k32_only_saved_ns=0",
            "capacity joint hit marginals are inconsistent",
        ),
        (
            "k32_only_hit=0 k32_only_saved_ns=0 k16_only_hit=1 k16_only_saved_ns=1200",
            "k32_only_hit=0 k32_only_saved_ns=0 k16_only_hit=0 k16_only_saved_ns=0",
            "conflict joint hit marginals are inconsistent",
        ),
        ("probe=10 hit=2", "probe=11 hit=2", "FA512 probe != hit + miss"),
        (
            "miss=8 put=8 stored=8",
            "miss=8 put=9 stored=8",
            "FA512 put != stored + budget_drop",
        ),
        (
            "resident=0 peak=5 capacity=0 capacity_peak=5000",
            "resident=0 peak=513 capacity=0 capacity_peak=5000",
            "actual resident/peak",
        ),
        (
            "resident=0 peak=5 capacity=0 capacity_peak=5000",
            "resident=0 peak=8 capacity=0 capacity_peak=5000",
            "actual peak exceeds stored",
        ),
        (
            "budget_drop=1 reset=2 resident=0 peak=5 capacity=0",
            "budget_drop=1 reset=1 resident=1 peak=5 capacity=0",
            "actual capacity is smaller than resident count",
        ),
        (
            "resident=0 peak=5 capacity=0 capacity_peak=5000",
            "resident=0 peak=5 capacity=1 capacity_peak=5000",
            "actual resident/capacity emptiness mismatch",
        ),
        ("capacity_peak=5000", "capacity_peak=16777217", "actual capacity/capacity_peak"),
        ("capacity_peak=5000", "capacity_peak=4", "actual peak exceeds capacity_peak"),
        ("history=3", "history=4", "history != cold + variant events"),
        ("one_shot=3", "one_shot=4", "one_shot exceeds history"),
        (
            "one_shot=3 one_shot_ns=2500",
            "one_shot=0 one_shot_ns=2500",
            "one_shot_ns is nonzero without one_shot events",
        ),
        (
            "sig_post_compile=1 wasted_build_ns=1000",
            "sig_post_compile=0 wasted_build_ns=1000",
            "wasted_build_ns is nonzero without sig_post_compile",
        ),
        ("history_drop=0", "history_drop=1", "history_drop is nonzero"),
        ("duplicate=0", "duplicate=1", "duplicate is nonzero"),
        ("token_stale=0", "token_stale=1", "token_stale is nonzero"),
        ("unclassified=0", "unclassified=1", "unclassified is nonzero"),
        ("global_invalid=0 history=3", "global_invalid=1 history=3", "global_invalid is nonzero"),
        (
            "capacity_peak=0 saved=1 saved_ns=1200 invalid=0 global_capacity=0",
            "capacity_peak=0 saved=1 saved_ns=1200 invalid=1 global_capacity=0",
            "FA512 invalid is nonzero",
        ),
        ("global_invalid=0\n[A64_DYNAREC_PHASE6AL_END]", "global_invalid=1\n[A64_DYNAREC_PHASE6AL_END]", "K1024_32 global_invalid is nonzero"),
        ("complete=1 valid=1 report=1", "complete=0 valid=1 report=1", "END complete"),
        ("complete=1 valid=1 report=1", "complete=1 valid=0 report=1", "END valid is not 1"),
        (
            "final=1 report=1 interval=100000 valid=1",
            "final=1 report=1 interval=100000 valid=0",
            "MAIN valid is not 1",
        ),
    )
    for old, new, expected in mutations:
        _expect_invalid(_replace_once(valid, old, new), expected)

    final_model_overconsumed = _replace_once(
        valid,
        "probe=10 hit=2 miss=8 put=8 stored=8 drop=1",
        "probe=10 hit=8 miss=2 put=8 stored=8 drop=1",
    )
    _expect_invalid(
        final_model_overconsumed,
        "FA512 hit + drop exceeds stored",
    )
    periodic_model_bad_resident = _replace_once(
        _fixture(False),
        "resident=5 peak=5 capacity=0 capacity_peak=0",
        "resident=4 peak=5 capacity=0 capacity_peak=0",
    )
    _expect_invalid(
        periodic_model_bad_resident,
        "FA512 stored != resident + hit + drop",
    )

    cold_shadow = _replace_once(
        valid,
        "cause=cold events=2 build_ns=1800 code_capacity=2000 "
        "fa_hit=0 fa_saved_ns=0 k16_hit=0 k16_saved_ns=0 "
        "k32_hit=0 k32_saved_ns=0",
        "cause=cold events=2 build_ns=1800 code_capacity=2000 "
        "fa_hit=0 fa_saved_ns=0 k16_hit=1 k16_saved_ns=100 "
        "k32_hit=1 k32_saved_ns=100",
    )
    _expect_invalid(cold_shadow, "cold unexpectedly recovered in a shadow model")

    conflict_without_fa = _replace_once(
        valid,
        "cause=conflict events=1 build_ns=1200 code_capacity=1200 "
        "fa_hit=1 fa_saved_ns=1200",
        "cause=conflict events=1 build_ns=1200 code_capacity=1200 "
        "fa_hit=0 fa_saved_ns=0",
    )
    _expect_invalid(conflict_without_fa, "conflict is not an exact FA512 recovery")

    _expect_invalid(_fixture(True, 2), "report sequence is not contiguous")
    interleaved = _replace_once(
        valid,
        "[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner=1 cause=cold",
        "unrelated-inside-frame\n"
        "[A64_DYNAREC_PHASE6AL_CAUSE] version=2 owner=1 cause=cold",
    )
    _expect_invalid(interleaved, "unrelated output splits a phase-6AL frame")

    second_owner = _fixture(
        True,
        1,
        owner=2,
        owners=2,
        role="worker",
        primary_owner=1,
    )
    periodic_owner = _fixture(False, 1, owners=2).replace("unrelated-after\n", "")
    _expect_final_invalid(
        periodic_owner + second_owner,
        "not every observed telemetry owner has a final frame",
    )
    owner_roster_decrease = (
        _fixture(False, 1, owners=3).replace("unrelated-after\n", "") +
        _fixture(
            True,
            1,
            owner=2,
            owners=2,
            role="worker",
            primary_owner=1,
        )
    )
    _expect_invalid(
        owner_roster_decrease,
        "process allocated-owner roster decreased",
    )
    hidden_owner = parse_report_text(
        valid.replace("owners=1", "owners=2", 1),
        "self-test-hidden-owner",
    )
    try:
        require_single_owner(hidden_owner)
    except ValidationError as error:
        if "allocated more than one" not in str(error):
            raise AssertionError(f"unexpected hidden-owner error: {error!s}") from error
    else:
        raise AssertionError("an allocated but non-reporting owner passed the live gate")
    _expect_final_invalid(
        valid.replace("owners=1", "owners=2", 1),
        "final report roster omits an allocated telemetry owner",
    )
    try:
        require_interval(parsed, 99999)
    except ValidationError as error:
        if "interval mismatch" not in str(error):
            raise AssertionError(f"unexpected interval-gate error: {error!s}") from error
    else:
        raise AssertionError("a mismatched preregistered interval passed")
    actual_peak_decrease = (
        _fixture(False, 1)
        .replace("global_peak=5000", "global_peak=6000", 1)
        .replace("unrelated-after\n", "") +
        second_owner
    )
    _expect_invalid(actual_peak_decrease, "actual process-global peak decreased")
    model_peak_decrease = (
        _fixture(False, 1)
        .replace(
            "global_capacity=2000 global_peak=7000 global_invalid=0",
            "global_capacity=2000 global_peak=8000 global_invalid=0",
            1,
        )
        .replace("unrelated-after\n", "") +
        second_owner
    )
    _expect_invalid(model_peak_decrease, "K1024_32 process-global peak decreased")
    nonempty_final = _replace_once(
        valid,
        "budget_drop=0 resident=0 peak=5 capacity=0 capacity_peak=0",
        "budget_drop=0 resident=1 peak=5 capacity=0 capacity_peak=0",
    )
    _expect_final_invalid(nonempty_final, "final FA512 is not empty")

    lines = valid.splitlines()
    cause_line = next(index for index, line in enumerate(lines) if "cause=budget" in line)
    del lines[cause_line]
    _expect_invalid("\n".join(lines), "expected CAUSE")


def _read_path(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    try:
        return pathlib.Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ValidationError(f"{path}: cannot read UTF-8 report log: {error}") from error


def _clean_binary_environment() -> Dict[str, str]:
    environment = os.environ.copy()
    for variable in (
        "LUMEN_X64_A64_DYNAREC",
        "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE",
        "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES",
        "LUMEN_X64_A64_DYNAREC_DEFAULT_REQUESTED",
        "LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION",
        "LUMEN_X64_A64_DYNAREC_SEPARATE_NEGATIVE_CACHE",
        "LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY",
        "LUMEN_X64_A64_DYNAREC_TELEMETRY",
        "LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY",
        "LUMEN_X64_A64_DYNAREC_PHASE6AL_INTERVAL",
    ):
        environment.pop(variable, None)
    return environment


def _run_report_binary(path: str, primary: bool = False) -> ReportLog:
    mode = (
        "--a64-dynarec-phase6al-telemetry-primary-report-only"
        if primary
        else "--a64-dynarec-phase6al-telemetry-report-only"
    )
    command = [path, mode]
    environment = _clean_binary_environment()
    environment["LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY"] = "1"
    environment["LUMEN_X64_A64_DYNAREC_PHASE6AL_INTERVAL"] = "100000"
    environment["LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES"] = "512"
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            env=environment,
            timeout=60,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ValidationError(f"{path}: report binary timed out") from error
    except (OSError, UnicodeError) as error:
        raise ValidationError(f"{path}: cannot run report binary: {error}") from error
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        if len(diagnostic) > 1000:
            diagnostic = diagnostic[-1000:]
        suffix = f": {diagnostic}" if diagnostic else ""
        raise ValidationError(
            f"{path}: report binary exited with {completed.returncode}{suffix}"
        )
    report = parse_report_text(completed.stderr, f"{path} stderr")
    require_single_owner(report)
    require_interval(report, 100000 if primary else UINT64_MAX)
    require_final_report(report)
    expected_role = "primary" if primary else "standalone"
    _require(
        all(frame.role == expected_role for frame in report.frames),
        report.source,
        f"fresh report binary owner role is not {expected_role}",
    )
    if primary:
        owner = report.frames[0].owner
        for frame in report.frames:
            _require(
                _number(frame.main, "primary_markers") == 1 and
                _number(frame.main, "primary_allocations") == 1 and
                _number(frame.main, "primary_owner") == owner and
                _number(frame.main, "role_invalid") == 0 and
                _number(frame.main, "process_invalid") == 0 and
                _number(frame.main, "process_valid") == 1,
                report.source,
                "fresh primary report binary role oracle failed",
            )
    else:
        for frame in report.frames:
            _require(
                _number(frame.main, "primary_markers") == 0 and
                _number(frame.main, "primary_allocations") == 0 and
                _number(frame.main, "primary_owner") == 0 and
                _number(frame.main, "role_invalid") == 0 and
                _number(frame.main, "process_invalid") == 0 and
                _number(frame.main, "process_valid") == 1,
                report.source,
                "fresh standalone report binary role oracle failed",
            )
    return report


def _run_primary_after_worker_binary(path: str) -> Tuple[ReportLog, Frame]:
    command = [path, "--a64-dynarec-phase6al-primary-after-worker-only"]
    environment = _clean_binary_environment()
    environment["LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY"] = "1"
    environment["LUMEN_X64_A64_DYNAREC_PHASE6AL_INTERVAL"] = "100000"
    environment["LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES"] = "512"
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            env=environment,
            timeout=60,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ValidationError(f"{path}: primary-after-worker binary timed out") from error
    except (OSError, UnicodeError) as error:
        raise ValidationError(
            f"{path}: cannot run primary-after-worker binary: {error}"
        ) from error
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        if len(diagnostic) > 1000:
            diagnostic = diagnostic[-1000:]
        suffix = f": {diagnostic}" if diagnostic else ""
        raise ValidationError(
            f"{path}: primary-after-worker binary exited with "
            f"{completed.returncode}{suffix}"
        )
    report = parse_report_text(completed.stderr, f"{path} stderr")
    require_interval(report, 100000)
    selected = select_first_frame_after_present(report, completed.stderr, 120)
    _require(
        selected.owner == 2 and selected.report_number == 1 and
        selected.role == "primary",
        report.source,
        "primary-after-worker selector did not elect owner 2 report 1",
    )
    _require(
        len(report.frames) == 2 and not report.final_frames and
        report.frames[0].owner == 1 and report.frames[0].role == "worker" and
        _number(report.frames[0].main, "primary_allocations") == 0 and
        _number(report.frames[0].main, "primary_owner") == 0,
        report.source,
        "primary-after-worker periodic prefix lifecycle is not canonical",
    )
    return report, selected


def _run_rollback_binary(path: str) -> None:
    command = [path, "--a64-dynarec-phase6al-telemetry-archive-rollback-only"]
    environment = _clean_binary_environment()
    environment["LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY"] = "1"
    environment["LUMEN_X64_A64_DYNAREC_PHASE6AL_INTERVAL"] = "100000"
    environment["LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES"] = "512"
    environment["LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE"] = "0"
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            env=environment,
            timeout=60,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ValidationError(f"{path}: rollback binary timed out") from error
    except (OSError, UnicodeError) as error:
        raise ValidationError(f"{path}: cannot run rollback binary: {error}") from error
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        if len(diagnostic) > 1000:
            diagnostic = diagnostic[-1000:]
        suffix = f": {diagnostic}" if diagnostic else ""
        raise ValidationError(
            f"{path}: rollback binary exited with {completed.returncode}{suffix}"
        )
    _require(
        PHASE_MARKER not in completed.stderr and PHASE_MARKER not in completed.stdout,
        path,
        "phase-6AL report emitted while production archive was rolled back",
    )


def _summary(label: str, report: ReportLog, selected: Optional[Frame] = None) -> str:
    if selected is None:
        selected = report.final_frames[-1] if report.final_frames else report.last
    main = selected.main
    suffix = "" if selected.final else " warning=periodic-cutoff"
    return (
        f"{label}: PASS frames={len(report.frames)} finals={len(report.final_frames)} "
        f"owner={selected.owner} report={selected.report_number} "
        f"role={selected.role} "
        f"interval={_number(main, 'interval')} "
        f"actual_probe={_number(main, 'actual_probe')} "
        f"compile_ok={_number(main, 'compile_ok')} "
        f"compile_ns={_number(main, 'compile_ns')}{suffix}"
    )


def _main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="validate phase-6AL compiled-archive telemetry report frames"
    )
    parser.add_argument(
        "report",
        nargs="?",
        default="-",
        metavar="REPORT",
        help="UTF-8 report log; omit or use - for stdin",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic positive and mutated negative fixtures",
    )
    parser.add_argument(
        "--run-report-binary",
        metavar="BINARY",
        help=(
            "run a CpuX64 test binary's phase-6AL report-only mode with the "
            "runtime selector set to 1 and validate its stderr"
        ),
    )
    parser.add_argument(
        "--run-primary-report-binary",
        metavar="BINARY",
        help=(
            "run a CpuX64 test binary's fresh-process phase-6AL primary-role "
            "report mode and validate its stderr"
        ),
    )
    parser.add_argument(
        "--run-primary-after-worker-binary",
        metavar="BINARY",
        help=(
            "run the deterministic pending-worker/primary-owner phase-6AL "
            "lifecycle and select the primary frame after present=120"
        ),
    )
    parser.add_argument(
        "--run-rollback-binary",
        metavar="BINARY",
        help=(
            "run the clean CpuX64 archive-disabled rollback mode and require "
            "that no phase-6AL report is emitted"
        ),
    )
    parser.add_argument(
        "--require-single-owner",
        action="store_true",
        help="reject logs with more than one allocated or observed telemetry owner",
    )
    parser.add_argument(
        "--after-present",
        type=int,
        metavar="N",
        help="select the first complete primary-owner frame after FRAME_PIPELINE present=N",
    )
    parser.add_argument(
        "--expect-interval",
        type=int,
        metavar="N",
        help="require every frame to report the preregistered positive interval N",
    )
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        if (arguments.report != "-" or
                arguments.run_report_binary is not None or
                arguments.run_primary_report_binary is not None or
                arguments.run_primary_after_worker_binary is not None or
                arguments.run_rollback_binary is not None or
                arguments.require_single_owner or
                arguments.after_present is not None or
                arguments.expect_interval is not None):
            parser.error("--self-test does not accept a report or report binary")
        self_test()
        print("phase6al-report-self-test: PASS")
        return 0
    if arguments.run_report_binary is not None:
        if (arguments.report != "-" or
                arguments.run_primary_report_binary is not None or
                arguments.run_primary_after_worker_binary is not None or
                arguments.run_rollback_binary is not None or
                arguments.after_present is not None or
                arguments.expect_interval is not None):
            parser.error("--run-report-binary does not accept a report path")
        report = _run_report_binary(arguments.run_report_binary)
        print(_summary("phase6al-report-binary", report))
        return 0
    if arguments.run_primary_report_binary is not None:
        if (arguments.report != "-" or
                arguments.run_primary_after_worker_binary is not None or
                arguments.run_rollback_binary is not None or
                arguments.require_single_owner or
                arguments.after_present is not None or
                arguments.expect_interval is not None):
            parser.error("--run-primary-report-binary does not accept a report path")
        report = _run_report_binary(arguments.run_primary_report_binary, primary=True)
        print(_summary("phase6al-primary-report-binary", report))
        return 0
    if arguments.run_primary_after_worker_binary is not None:
        if (arguments.report != "-" or
                arguments.run_rollback_binary is not None or
                arguments.require_single_owner or
                arguments.after_present is not None or
                arguments.expect_interval is not None):
            parser.error(
                "--run-primary-after-worker-binary does not accept a report path"
            )
        report, selected = _run_primary_after_worker_binary(
            arguments.run_primary_after_worker_binary
        )
        print(_summary("phase6al-primary-after-worker-binary", report, selected))
        return 0
    if arguments.run_rollback_binary is not None:
        if (arguments.report != "-" or
                arguments.run_primary_report_binary is not None or
                arguments.run_primary_after_worker_binary is not None or
                arguments.require_single_owner or
                arguments.after_present is not None or
                arguments.expect_interval is not None):
            parser.error("--run-rollback-binary does not accept a report path")
        _run_rollback_binary(arguments.run_rollback_binary)
        print("phase6al-rollback-binary: PASS no-report=1")
        return 0

    report_text = _read_path(arguments.report)
    report = parse_report_text(
        report_text,
        "<stdin>" if arguments.report == "-" else arguments.report,
    )
    selected = None
    if arguments.after_present is not None:
        selected = select_first_frame_after_present(
            report, report_text, arguments.after_present
        )
    if arguments.require_single_owner:
        require_single_owner(report)
    if arguments.expect_interval is not None:
        require_interval(report, arguments.expect_interval)
    print(_summary("phase6al-report", report, selected))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except ValidationError as error:
        print(f"phase6al-report: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
