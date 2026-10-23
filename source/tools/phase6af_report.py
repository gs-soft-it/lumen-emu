#!/usr/bin/env python3
"""Validate canonical phase-6AF telemetry reports and the R1/R2 gate.

The verifier uses only the Python standard library.  A report may be a bare
phase-6AF frame or a larger log containing exactly one such frame.  Unrelated
log lines are ignored; every line containing the phase-6AF marker is parsed
strictly.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Mapping, Optional, Sequence, Tuple, Union


UINT64_MAX = (1 << 64) - 1
FNV_OFFSET_BASIS = 14695981039346656037
FNV_PRIME = 1099511628211
BLOCK_SIZE = 65536
STRATA_COUNT = 16
CLOCK_PAIRS = 256
MAX_OWNERS = 64

PHASE_MARKER = "[A64_DYNAREC_PHASE6AF_"
PREFIXES = {
    "SUMMARY": "[A64_DYNAREC_PHASE6AF_SUMMARY]",
    "COST": "[A64_DYNAREC_PHASE6AF_COST]",
    "STRATUM": "[A64_DYNAREC_PHASE6AF_STRATUM]",
    "OWNER": "[A64_DYNAREC_PHASE6AF_OWNER]",
    "END": "[A64_DYNAREC_PHASE6AF_END]",
}

SUMMARY_FIELDS = (
    "version",
    "seed",
    "generation",
    "begin_ns",
    "cutoff_ns",
    "wall_ns",
    "clock_floor_ns",
    "clock_pairs",
    "block_size",
    "strata",
    "scheduled",
    "complete",
    "paired",
    "unpaired",
    "hle_excluded",
    "reentrant_excluded",
    "cutoff_excluded",
    "record_drop",
    "cutoff_clipped",
    "h_ppm",
    "represented_calls",
    "paired_steps",
    "paired_direct_steps",
    "all_direct",
    "helper_backed",
    "zero_step",
    "shards",
    "shard_drops",
    "run_configurations",
    "fast_blocks_enabled",
    "dynarec_requested",
    "dynarec_enabled",
    "diagnostic_history_enabled",
    "delayed_admission_enabled",
    "archive_enabled",
    "max_sample_raw_ns",
    "max_sample_corrected_ns",
    "overflow",
    "underflow",
    "invalid",
    "q_raw_ppm",
    "q_corrected_ppm",
    "class",
    "classifiable",
    "eligible",
)
COST_FIELDS = ("version", "cost", "events", "raw_ns", "corrected_ns")
STRATUM_FIELDS = (
    "version",
    "index",
    "paired",
    "try_raw_ns",
    "try_corrected_ns",
    "compiled_raw_ns",
    "compiled_corrected_ns",
    "surrounding_raw_ns",
    "surrounding_corrected_ns",
    "q_raw_ppm",
    "q_corrected_ppm",
)
OWNER_FIELDS = (
    "version",
    "ordinal",
    "generation",
    "derived_seed",
    "block_index",
    "draw_count",
    "position",
    "represented_calls",
)
END_FIELDS = ("version", "complete", "checksum", "reports")

COST_NAMES = (
    "try_all",
    "try_paired",
    "compiled_execute",
    "paired_surrounding",
    "try_unpaired",
)
CLASS_NAMES = {
    "NATIVE_EXECUTE_DOMINANT",
    "TRY_PAIRED_SURROUNDING_DOMINANT",
    "MIXED",
    "INCONCLUSIVE",
}
DECIMAL_RE = re.compile(r"(?:0|[1-9][0-9]*)\Z")
CHECKSUM_RE = re.compile(r"[0-9a-f]{16}\Z")

Value = Union[int, str]
Record = Dict[str, Value]


class ValidationError(Exception):
    """A report is not canonical or violates a frozen invariant."""


@dataclass(frozen=True)
class Report:
    source: str
    summary: Record
    costs: Tuple[Record, ...]
    strata: Tuple[Record, ...]
    owners: Tuple[Record, ...]
    end: Record

    @property
    def report_class(self) -> str:
        return _text(self.summary, "class")

    @property
    def eligible(self) -> bool:
        return _number(self.summary, "eligible") == 1

    @property
    def q_corrected_ppm(self) -> int:
        return _number(self.summary, "q_corrected_ppm")


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
        f"line {line_number}: expected {PREFIXES[kind]}",
    )

    record: Record = {}
    text_fields = {"cost", "class", "checksum"}
    for expected, token in zip(fields, tokens[1:]):
        key, separator, raw_value = token.partition("=")
        _require(
            separator == "=" and key == expected and raw_value != "",
            source,
            f"line {line_number}: expected field {expected} in canonical order",
        )
        if expected in text_fields:
            record[expected] = raw_value
        else:
            record[expected] = _parse_decimal(
                raw_value, source, line_number, expected
            )

    _require(
        _number(record, "version") == 1,
        source,
        f"line {line_number}: version must be 1",
    )
    return record


def parse_report_text(text: str, source: str = "<memory>") -> Report:
    phase_lines: List[Tuple[int, str, str]] = []
    prefix_to_kind = {prefix: kind for kind, prefix in PREFIXES.items()}
    for line_number, line in enumerate(text.splitlines(), start=1):
        if PHASE_MARKER not in line:
            continue
        _require(
            line.startswith(PHASE_MARKER),
            source,
            f"line {line_number}: phase-6AF marker is not at record start",
        )
        prefix = line.split(" ", 1)[0]
        kind = prefix_to_kind.get(prefix)
        _require(
            kind is not None,
            source,
            f"line {line_number}: unknown phase-6AF record {prefix}",
        )
        phase_lines.append((line_number, line, kind))

    _require(phase_lines, source, "no phase-6AF report records found")
    cursor = 0

    def consume(expected_kind: str, fields: Sequence[str]) -> Record:
        nonlocal cursor
        _require(
            cursor < len(phase_lines),
            source,
            f"missing {expected_kind} record",
        )
        line_number, line, actual_kind = phase_lines[cursor]
        _require(
            actual_kind == expected_kind,
            source,
            f"line {line_number}: expected {expected_kind}, found {actual_kind}",
        )
        cursor += 1
        return _parse_record(line, line_number, source, expected_kind, fields)

    summary = consume("SUMMARY", SUMMARY_FIELDS)
    costs = tuple(consume("COST", COST_FIELDS) for _ in COST_NAMES)
    strata = tuple(consume("STRATUM", STRATUM_FIELDS) for _ in range(STRATA_COUNT))
    owners: List[Record] = []
    while cursor < len(phase_lines) and phase_lines[cursor][2] == "OWNER":
        _require(
            len(owners) < MAX_OWNERS,
            source,
            "more than 64 OWNER records",
        )
        owners.append(consume("OWNER", OWNER_FIELDS))
    end = consume("END", END_FIELDS)
    _require(
        cursor == len(phase_lines),
        source,
        "records remain after the canonical END",
    )

    report = Report(source, summary, costs, strata, tuple(owners), end)
    _validate_report(report)
    return report


def _ratio_ppm(numerator: int, denominator: int) -> int:
    return 0 if denominator == 0 else (1_000_000 * numerator) // denominator


def _classify(q_ppm: int) -> str:
    if q_ppm >= 600_000:
        return "NATIVE_EXECUTE_DOMINANT"
    if q_ppm <= 400_000:
        return "TRY_PAIRED_SURROUNDING_DOMINANT"
    return "MIXED"


def _mix_seed(replica_seed: int, generation: int, owner_ordinal: int) -> int:
    value = replica_seed
    value ^= (generation * 0x9E3779B97F4A7C15) & UINT64_MAX
    value ^= ((owner_ordinal + 1) * 0xBF58476D1CE4E5B9) & UINT64_MAX
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & UINT64_MAX
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & UINT64_MAX
    value ^= value >> 31
    value &= UINT64_MAX
    return value if value != 0 else 0x9E3779B97F4A7C15


def _xorshift_state(value: int) -> int:
    value ^= value >> 12
    value ^= (value << 25) & UINT64_MAX
    value ^= value >> 27
    return value & UINT64_MAX


def _apply_linear(columns: Sequence[int], value: int) -> int:
    result = 0
    column = 0
    while value:
        if value & 1:
            result ^= columns[column]
        value >>= 1
        column += 1
    return result


def _xorshift_powers() -> Tuple[Tuple[int, ...], ...]:
    powers: List[Tuple[int, ...]] = [
        tuple(_xorshift_state(1 << bit) for bit in range(64))
    ]
    for _ in range(1, 64):
        previous = powers[-1]
        powers.append(tuple(_apply_linear(previous, column) for column in previous))
    return tuple(powers)


XORSHIFT_POWERS = _xorshift_powers()


def _position_for_draw(derived_seed: int, draw_count: int) -> int:
    state = derived_seed
    bit = 0
    remaining = draw_count
    while remaining:
        if remaining & 1:
            state = _apply_linear(XORSHIFT_POWERS[bit], state)
        remaining >>= 1
        bit += 1
    output = (state * 2685821657736338717) & UINT64_MAX
    return output & 0xFFFF


def _checksum(
    summary: Mapping[str, Value],
    costs: Sequence[Mapping[str, Value]],
    strata: Sequence[Mapping[str, Value]],
    owners: Sequence[Mapping[str, Value]],
) -> int:
    checksum = FNV_OFFSET_BASIS

    def mix(value: int) -> None:
        nonlocal checksum
        for byte_index in range(8):
            checksum ^= (value >> (8 * byte_index)) & 0xFF
            checksum = (checksum * FNV_PRIME) & UINT64_MAX

    for field in SUMMARY_FIELDS[1:]:
        if field != "class":
            mix(_number(summary, field))
    for cost in costs:
        for field in ("events", "raw_ns", "corrected_ns"):
            mix(_number(cost, field))
    for stratum in strata:
        for field in STRATUM_FIELDS[1:]:
            mix(_number(stratum, field))
    for owner in owners:
        for field in OWNER_FIELDS[1:]:
            mix(_number(owner, field))
    return checksum


def _validate_report(report: Report) -> None:
    source = report.source
    summary = report.summary
    costs = report.costs
    strata = report.strata
    owners = report.owners
    end = report.end

    _require(
        [_text(cost, "cost") for cost in costs] == list(COST_NAMES),
        source,
        "COST labels are not in canonical order",
    )
    _require(
        [_number(stratum, "index") for stratum in strata]
        == list(range(STRATA_COUNT)),
        source,
        "STRATUM indices are not exactly 0..15",
    )
    _require(
        _text(summary, "class") in CLASS_NAMES,
        source,
        "unknown class label",
    )
    _require(
        CHECKSUM_RE.fullmatch(_text(end, "checksum")) is not None,
        source,
        "checksum must be sixteen lowercase hexadecimal digits",
    )
    for field in ("classifiable", "eligible"):
        _require(
            _number(summary, field) in (0, 1),
            source,
            f"{field} must be 0 or 1",
        )
    _require(_number(end, "complete") == 1, source, "END complete must be 1")
    _require(_number(end, "reports") == 1, source, "END reports must be 1")

    seed = _number(summary, "seed")
    generation = _number(summary, "generation")
    begin_ns = _number(summary, "begin_ns")
    cutoff_ns = _number(summary, "cutoff_ns")
    _require(seed != 0, source, "seed must be nonzero")
    _require(generation != 0, source, "generation must be nonzero")
    _require(cutoff_ns >= begin_ns, source, "cutoff_ns precedes begin_ns")
    _require(
        _number(summary, "wall_ns") == cutoff_ns - begin_ns,
        source,
        "wall_ns does not equal cutoff_ns-begin_ns",
    )
    _require(
        _number(summary, "clock_floor_ns") > 0,
        source,
        "clock_floor_ns must be positive",
    )
    _require(
        _number(summary, "clock_pairs") == CLOCK_PAIRS,
        source,
        "clock_pairs must be 256",
    )
    _require(
        _number(summary, "block_size") == BLOCK_SIZE,
        source,
        "block_size must be 65536",
    )
    _require(
        _number(summary, "strata") == STRATA_COUNT,
        source,
        "strata must be 16",
    )

    scheduled = _number(summary, "scheduled")
    complete = _number(summary, "complete")
    paired = _number(summary, "paired")
    unpaired = _number(summary, "unpaired")
    excluded_total = _sum_u64(
        [
            complete,
            _number(summary, "hle_excluded"),
            _number(summary, "reentrant_excluded"),
            _number(summary, "cutoff_excluded"),
            _number(summary, "record_drop"),
        ],
        source,
        "scheduled reconciliation",
    )
    _require(scheduled == excluded_total, source, "scheduled reconciliation failed")
    _require(
        complete == _sum_u64([paired, unpaired], source, "complete reconciliation"),
        source,
        "complete != paired+unpaired",
    )
    _require(
        _number(summary, "cutoff_clipped") <= complete,
        source,
        "cutoff_clipped exceeds complete",
    )
    expected_h = _ratio_ppm(complete, scheduled)
    _require(
        _number(summary, "h_ppm") == expected_h,
        source,
        "h_ppm does not match complete/scheduled",
    )

    shards = _number(summary, "shards")
    _require(shards <= MAX_OWNERS, source, "shards exceeds 64")
    _require(shards == len(owners), source, "shards does not match OWNER count")
    _require(
        _number(summary, "cutoff_clipped") <= shards,
        source,
        "cutoff_clipped exceeds the one-active-sample-per-owner bound",
    )
    owner_calls: List[int] = []
    for expected_ordinal, owner in enumerate(owners, start=1):
        ordinal = _number(owner, "ordinal")
        block_index = _number(owner, "block_index")
        draw_count = _number(owner, "draw_count")
        owner_represented_calls = _number(owner, "represented_calls")
        _require(ordinal == expected_ordinal, source, "OWNER ordinals are not contiguous")
        _require(
            _number(owner, "generation") == generation,
            source,
            f"OWNER {ordinal} generation mismatch",
        )
        _require(
            _number(owner, "derived_seed") == _mix_seed(seed, generation, ordinal),
            source,
            f"OWNER {ordinal} derived_seed mismatch",
        )
        _require(
            draw_count == block_index + 1,
            source,
            f"OWNER {ordinal} draw_count/block_index mismatch",
        )
        _require(
            _number(owner, "position")
            == _position_for_draw(_number(owner, "derived_seed"), draw_count),
            source,
            f"OWNER {ordinal} position does not match its deterministic draw",
        )
        _require(
            owner_represented_calls % BLOCK_SIZE == 0,
            source,
            f"OWNER {ordinal} represented_calls is not block-aligned",
        )
        if owner_represented_calls == 0:
            _require(
                block_index == 0 and draw_count == 1,
                source,
                f"OWNER {ordinal} has advanced metadata without a committed sample",
            )
        else:
            _require(
                draw_count <= UINT64_MAX // BLOCK_SIZE
                and owner_represented_calls == draw_count * BLOCK_SIZE,
                source,
                f"OWNER {ordinal} represented_calls/draw_count mismatch",
            )
        owner_calls.append(owner_represented_calls)
    represented_calls = _number(summary, "represented_calls")
    _require(
        represented_calls == _sum_u64(owner_calls, source, "OWNER represented_calls"),
        source,
        "OWNER represented_calls do not match SUMMARY",
    )
    _require(
        scheduled <= UINT64_MAX // BLOCK_SIZE,
        source,
        "scheduled*block_size overflows uint64",
    )
    _require(
        represented_calls == scheduled * BLOCK_SIZE,
        source,
        "represented_calls != scheduled*65536",
    )

    cost_by_name = {_text(cost, "cost"): cost for cost in costs}
    try_all = cost_by_name["try_all"]
    try_paired = cost_by_name["try_paired"]
    compiled = cost_by_name["compiled_execute"]
    surrounding = cost_by_name["paired_surrounding"]
    try_unpaired = cost_by_name["try_unpaired"]
    for cost in costs:
        _require(
            _number(cost, "corrected_ns") <= _number(cost, "raw_ns"),
            source,
            f"COST {_text(cost, 'cost')} corrected_ns exceeds raw_ns",
        )
        _require(
            _number(cost, "raw_ns")
            <= _number(cost, "events") * _number(summary, "wall_ns"),
            source,
            f"COST {_text(cost, 'cost')} exceeds the epoch wall bound",
        )
    _require(_number(try_all, "events") == complete, source, "try_all events mismatch")
    for cost_name in ("try_paired", "compiled_execute", "paired_surrounding"):
        _require(
            _number(cost_by_name[cost_name], "events") == paired,
            source,
            f"{cost_name} events mismatch",
        )
    _require(
        _number(try_unpaired, "events") == unpaired,
        source,
        "try_unpaired events mismatch",
    )
    for field in ("raw_ns", "corrected_ns"):
        _require(
            _number(try_all, field)
            == _sum_u64(
                [_number(try_paired, field), _number(try_unpaired, field)],
                source,
                f"try_all {field}",
            ),
            source,
            f"try_all {field} reconciliation failed",
        )
        _require(
            _number(try_paired, field)
            == _sum_u64(
                [_number(compiled, field), _number(surrounding, field)],
                source,
                f"try_paired {field}",
            ),
            source,
            f"try_paired {field} reconciliation failed",
        )

    _require(
        _number(summary, "paired_direct_steps")
        <= _number(summary, "paired_steps"),
        source,
        "paired_direct_steps exceeds paired_steps",
    )
    _require(
        paired
        == _sum_u64(
            [
                _number(summary, "all_direct"),
                _number(summary, "helper_backed"),
                _number(summary, "zero_step"),
            ],
            source,
            "paired step classes",
        ),
        source,
        "paired step-class reconciliation failed",
    )
    positive_step_samples = paired - _number(summary, "zero_step")
    _require(
        _number(summary, "paired_steps") >= positive_step_samples,
        source,
        "paired_steps cannot represent every positive-step paired sample",
    )
    _require(
        _number(summary, "paired_direct_steps")
        >= _number(summary, "all_direct"),
        source,
        "paired_direct_steps cannot represent every all-direct sample",
    )
    _require(
        _number(summary, "paired_steps")
        - _number(summary, "paired_direct_steps")
        >= _number(summary, "helper_backed"),
        source,
        "paired helper deficit is smaller than helper_backed",
    )
    if _number(summary, "helper_backed") == 0:
        _require(
            _number(summary, "paired_steps")
            == _number(summary, "paired_direct_steps"),
            source,
            "paired steps must equal direct steps without helper-backed samples",
        )
    if positive_step_samples == 0:
        _require(
            _number(summary, "paired_steps") == 0
            and _number(summary, "paired_direct_steps") == 0,
            source,
            "zero-step-only paired samples must publish zero step sums",
        )

    stratum_totals = {
        field: _sum_u64(
            [_number(stratum, field) for stratum in strata],
            source,
            f"STRATUM {field}",
        )
        for field in STRATUM_FIELDS[2:9]
    }
    for index, stratum in enumerate(strata):
        for corrected_field, raw_field in (
            ("try_corrected_ns", "try_raw_ns"),
            ("compiled_corrected_ns", "compiled_raw_ns"),
            ("surrounding_corrected_ns", "surrounding_raw_ns"),
        ):
            _require(
                _number(stratum, corrected_field) <= _number(stratum, raw_field),
                source,
                f"STRATUM {index} {corrected_field} exceeds {raw_field}",
            )
        for raw_field in (
            "try_raw_ns",
            "compiled_raw_ns",
            "surrounding_raw_ns",
        ):
            _require(
                _number(stratum, raw_field)
                <= _number(stratum, "paired")
                * _number(summary, "wall_ns"),
                source,
                f"STRATUM {index} {raw_field} exceeds the epoch wall bound",
            )
        for suffix in ("raw_ns", "corrected_ns"):
            _require(
                _number(stratum, f"try_{suffix}")
                == _sum_u64(
                    [
                        _number(stratum, f"compiled_{suffix}"),
                        _number(stratum, f"surrounding_{suffix}"),
                    ],
                    source,
                    f"STRATUM {index} {suffix}",
                ),
                source,
                f"STRATUM {index} {suffix} reconciliation failed",
            )
        q_raw = _ratio_ppm(
            _number(stratum, "compiled_raw_ns"),
            _number(stratum, "try_raw_ns"),
        )
        q_corrected = _ratio_ppm(
            _number(stratum, "compiled_corrected_ns"),
            _number(stratum, "try_corrected_ns"),
        )
        _require(
            _number(stratum, "q_raw_ppm") == q_raw,
            source,
            f"STRATUM {index} q_raw_ppm mismatch",
        )
        _require(
            _number(stratum, "q_corrected_ppm") == q_corrected,
            source,
            f"STRATUM {index} q_corrected_ppm mismatch",
        )
    if _number(summary, "record_drop") == 0:
        quotient, remainder = divmod(paired, STRATA_COUNT)
        expected_counts = [
            quotient + (1 if index < remainder else 0)
            for index in range(STRATA_COUNT)
        ]
        _require(
            [_number(stratum, "paired") for stratum in strata]
            == expected_counts,
            source,
            "STRATUM counts do not match zero-based modulo-16 assignment",
        )
    expected_stratum_totals = {
        "paired": _number(try_paired, "events"),
        "try_raw_ns": _number(try_paired, "raw_ns"),
        "try_corrected_ns": _number(try_paired, "corrected_ns"),
        "compiled_raw_ns": _number(compiled, "raw_ns"),
        "compiled_corrected_ns": _number(compiled, "corrected_ns"),
        "surrounding_raw_ns": _number(surrounding, "raw_ns"),
        "surrounding_corrected_ns": _number(surrounding, "corrected_ns"),
    }
    _require(
        stratum_totals == expected_stratum_totals,
        source,
        "STRATUM totals do not match paired COST records",
    )

    clock_floor_ns = _number(summary, "clock_floor_ns")
    expected_compiled_floor = paired * clock_floor_ns
    compiled_floor = _number(compiled, "raw_ns") - _number(
        compiled, "corrected_ns"
    )
    _require(
        compiled_floor <= expected_compiled_floor,
        source,
        "compiled_execute raw/corrected gap exceeds one floor per paired sample",
    )
    expected_unpaired_floor = unpaired * clock_floor_ns
    unpaired_floor = _number(try_unpaired, "raw_ns") - _number(
        try_unpaired, "corrected_ns"
    )
    _require(
        unpaired_floor <= expected_unpaired_floor,
        source,
        "try_unpaired raw/corrected gap exceeds one floor per sample",
    )
    surrounding_floor = _number(surrounding, "raw_ns") - _number(
        surrounding, "corrected_ns"
    )
    maximum_surrounding_floor = paired * 2 * clock_floor_ns
    _require(
        surrounding_floor <= maximum_surrounding_floor,
        source,
        "paired_surrounding raw/corrected gap exceeds two floors per sample",
    )
    strict_clock_accounting = (
        _number(summary, "underflow") == 0
        and _number(summary, "invalid") == 0
    )
    if strict_clock_accounting:
        _require(
            compiled_floor % clock_floor_ns == 0,
            source,
            "compiled_execute raw/corrected gap is not floor-aligned",
        )
        _require(
            unpaired_floor % clock_floor_ns == 0,
            source,
            "try_unpaired raw/corrected gap is not floor-aligned",
        )
        _require(
            surrounding_floor % clock_floor_ns == 0,
            source,
            "paired_surrounding raw/corrected gap is not floor-aligned",
        )
        _require(
            _number(compiled, "raw_ns") == 0
            or compiled_floor >= clock_floor_ns,
            source,
            "compiled_execute positive raw time lacks a clock-floor gap",
        )
        _require(
            _number(try_unpaired, "raw_ns") == 0
            or unpaired_floor >= clock_floor_ns,
            source,
            "try_unpaired positive raw time lacks a clock-floor gap",
        )
        _require(
            _number(surrounding, "raw_ns") == 0
            or surrounding_floor >= clock_floor_ns,
            source,
            "paired_surrounding positive raw time lacks a clock-floor gap",
        )
    for index, stratum in enumerate(strata):
        events = _number(stratum, "paired")
        stratum_compiled_floor = _number(stratum, "compiled_raw_ns") - _number(
            stratum, "compiled_corrected_ns"
        )
        stratum_compiled_limit = events * clock_floor_ns
        _require(
            stratum_compiled_floor <= stratum_compiled_limit,
            source,
            f"STRATUM {index} compiled clock-floor gap exceeds its bound",
        )
        stratum_surrounding_floor = _number(
            stratum, "surrounding_raw_ns"
        ) - _number(stratum, "surrounding_corrected_ns")
        stratum_surrounding_limit = events * 2 * clock_floor_ns
        _require(
            stratum_surrounding_floor <= stratum_surrounding_limit,
            source,
            f"STRATUM {index} surrounding clock-floor gap exceeds its bound",
        )
        if strict_clock_accounting:
            _require(
                stratum_compiled_floor % clock_floor_ns == 0,
                source,
                f"STRATUM {index} compiled clock-floor gap is not aligned",
            )
            _require(
                stratum_surrounding_floor % clock_floor_ns == 0,
                source,
                f"STRATUM {index} surrounding clock-floor gap is not aligned",
            )
            _require(
                _number(stratum, "compiled_raw_ns") == 0
                or stratum_compiled_floor >= clock_floor_ns,
                source,
                f"STRATUM {index} positive compiled raw time lacks a floor gap",
            )
            _require(
                _number(stratum, "surrounding_raw_ns") == 0
                or stratum_surrounding_floor >= clock_floor_ns,
                source,
                f"STRATUM {index} positive surrounding raw time lacks a floor gap",
            )

    q_raw = _ratio_ppm(_number(compiled, "raw_ns"), _number(try_paired, "raw_ns"))
    q_corrected = _ratio_ppm(
        _number(compiled, "corrected_ns"), _number(try_paired, "corrected_ns")
    )
    _require(_number(summary, "q_raw_ppm") == q_raw, source, "q_raw_ppm mismatch")
    _require(
        _number(summary, "q_corrected_ppm") == q_corrected,
        source,
        "q_corrected_ppm mismatch",
    )
    classifiable = (
        _number(try_paired, "raw_ns") > 0
        and _number(try_paired, "corrected_ns") > 0
        and _classify(q_raw) == _classify(q_corrected)
        and abs(q_raw - q_corrected) <= 50_000
    )
    expected_class = _classify(q_corrected) if classifiable else "INCONCLUSIVE"
    _require(
        _number(summary, "classifiable") == int(classifiable),
        source,
        "classifiable does not match recomputed value",
    )
    _require(
        _text(summary, "class") == expected_class,
        source,
        "class does not match recomputed value",
    )

    participating = [stratum for stratum in strata if _number(stratum, "paired") >= 128]
    aggregate_class = _classify(q_corrected)
    strata_classes_valid = all(
        _number(stratum, "try_raw_ns") > 0
        and _number(stratum, "try_corrected_ns") > 0
        and _classify(_number(stratum, "q_raw_ppm")) == aggregate_class
        and _classify(_number(stratum, "q_corrected_ppm")) == aggregate_class
        for stratum in participating
    )
    stratum_range_valid = False
    if participating:
        corrected_values = [_number(stratum, "q_corrected_ppm") for stratum in participating]
        stratum_range_valid = max(corrected_values) - min(corrected_values) <= 100_000
    max_raw_valid = (
        1_000_000 * _number(summary, "max_sample_raw_ns")
        <= 10_000 * _number(try_paired, "raw_ns")
    )
    max_corrected_valid = (
        1_000_000 * _number(summary, "max_sample_corrected_ns")
        <= 10_000 * _number(try_paired, "corrected_ns")
    )
    max_sample_raw = _number(summary, "max_sample_raw_ns")
    max_sample_corrected = _number(summary, "max_sample_corrected_ns")
    _require(
        max_sample_raw <= _number(summary, "wall_ns"),
        source,
        "max_sample_raw_ns exceeds wall_ns",
    )
    if paired == 0:
        _require(
            max_sample_raw == 0 and max_sample_corrected == 0,
            source,
            "max_sample fields must be zero without paired samples",
        )
    else:
        minimum_raw_maximum = (
            _number(try_paired, "raw_ns") + paired - 1
        ) // paired
        minimum_corrected_maximum = (
            _number(try_paired, "corrected_ns") + paired - 1
        ) // paired
        _require(
            minimum_raw_maximum <= max_sample_raw <= _number(try_paired, "raw_ns"),
            source,
            "max_sample_raw_ns is impossible for the paired total",
        )
        _require(
            minimum_corrected_maximum
            <= max_sample_corrected
            <= _number(try_paired, "corrected_ns"),
            source,
            "max_sample_corrected_ns is impossible for the paired total",
        )
        _require(
            max_sample_corrected <= max_sample_raw,
            source,
            "max_sample_corrected_ns exceeds max_sample_raw_ns",
        )
    run_configurations = _number(summary, "run_configurations")
    for field in (
        "fast_blocks_enabled",
        "dynarec_requested",
        "dynarec_enabled",
        "diagnostic_history_enabled",
        "delayed_admission_enabled",
        "archive_enabled",
    ):
        _require(
            _number(summary, field) <= run_configurations,
            source,
            f"{field} exceeds run_configurations",
        )

    eligible = (
        classifiable
        and paired >= 4096
        and scheduled != 0
        and expected_h >= 950_000
        and len(participating) >= 12
        and strata_classes_valid
        and stratum_range_valid
        and max_raw_valid
        and max_corrected_valid
        and run_configurations != 0
        and _number(summary, "fast_blocks_enabled") == run_configurations
        and _number(summary, "dynarec_requested") == run_configurations
        and _number(summary, "dynarec_enabled") == run_configurations
        and _number(summary, "diagnostic_history_enabled") == 0
        and _number(summary, "delayed_admission_enabled") == run_configurations
        and _number(summary, "archive_enabled") == 0
        and _number(summary, "shard_drops") == 0
        and _number(summary, "cutoff_excluded") == 0
        and _number(summary, "record_drop") == 0
        and _number(summary, "zero_step") == 0
        and _number(summary, "overflow") == 0
        and _number(summary, "underflow") == 0
        and _number(summary, "invalid") == 0
    )
    _require(
        _number(summary, "eligible") == int(eligible),
        source,
        "eligible does not match recomputed value",
    )

    expected_checksum = _checksum(summary, costs, strata, owners)
    emitted_checksum = int(_text(end, "checksum"), 16)
    _require(
        emitted_checksum == expected_checksum,
        source,
        f"checksum mismatch: expected {expected_checksum:016x}",
    )


def validate_replica_gate(first: Report, second: Report) -> Tuple[str, int]:
    source = f"{first.source} + {second.source}"
    _require(
        _number(first.summary, "seed") == 1
        and _number(second.summary, "seed") == 2,
        source,
        "replica gate requires R1 seed=1 followed by R2 seed=2",
    )
    _require(
        _number(first.summary, "generation") == 1
        and _number(second.summary, "generation") == 1,
        source,
        "fresh replica gate requires generation=1 in R1 and R2",
    )
    _require(first.eligible and second.eligible, source, "both replicas must be eligible")
    _require(
        first.report_class == second.report_class,
        source,
        "replica classes differ",
    )
    _require(
        first.report_class != "INCONCLUSIVE",
        source,
        "replica class is inconclusive",
    )
    delta = abs(first.q_corrected_ppm - second.q_corrected_ppm)
    _require(delta <= 50_000, source, "replica Qcorrected delta exceeds 50000 ppm")
    return first.report_class, delta


def _render_record(prefix: str, fields: Sequence[str], record: Mapping[str, Value]) -> str:
    return prefix + " " + " ".join(f"{field}={record[field]}" for field in fields)


def _synthetic_report(
    seed: int,
    q_ppm: int,
    eligible_override: Optional[int] = None,
    surrounding_floor_segments: int = 2,
    cutoff_clipped: int = 0,
) -> str:
    generation = 1
    paired = 4096
    per_stratum = paired // STRATA_COUNT
    try_raw_per_stratum = 256_000
    compiled_raw_per_stratum = try_raw_per_stratum * q_ppm // 1_000_000
    floor_per_stratum = per_stratum * 10
    compiled_corrected_per_stratum = compiled_raw_per_stratum - floor_per_stratum
    surrounding_raw_per_stratum = (
        try_raw_per_stratum - compiled_raw_per_stratum
    )
    surrounding_corrected_per_stratum = (
        surrounding_raw_per_stratum
        - surrounding_floor_segments * floor_per_stratum
    )
    try_corrected_per_stratum = (
        compiled_corrected_per_stratum + surrounding_corrected_per_stratum
    )
    try_raw = try_raw_per_stratum * STRATA_COUNT
    try_corrected = try_corrected_per_stratum * STRATA_COUNT
    compiled_raw = compiled_raw_per_stratum * STRATA_COUNT
    compiled_corrected = compiled_corrected_per_stratum * STRATA_COUNT
    surrounding_raw = try_raw - compiled_raw
    surrounding_corrected = try_corrected - compiled_corrected
    q_corrected_ppm = _ratio_ppm(compiled_corrected, try_corrected)
    report_class = _classify(q_corrected_ppm)
    eligible = 1 if eligible_override is None else eligible_override
    summary: Record = {
        "version": 1,
        "seed": seed,
        "generation": generation,
        "begin_ns": 100,
        "cutoff_ns": 1_000_100,
        "wall_ns": 1_000_000,
        "clock_floor_ns": 10,
        "clock_pairs": CLOCK_PAIRS,
        "block_size": BLOCK_SIZE,
        "strata": STRATA_COUNT,
        "scheduled": paired,
        "complete": paired,
        "paired": paired,
        "unpaired": 0,
        "hle_excluded": 0,
        "reentrant_excluded": 0,
        "cutoff_excluded": 0,
        "record_drop": 0,
        "cutoff_clipped": cutoff_clipped,
        "h_ppm": 1_000_000,
        "represented_calls": paired * BLOCK_SIZE,
        "paired_steps": paired * 4,
        "paired_direct_steps": paired * 4,
        "all_direct": paired,
        "helper_backed": 0,
        "zero_step": 0,
        "shards": 1,
        "shard_drops": 0,
        "run_configurations": 1,
        "fast_blocks_enabled": 1,
        "dynarec_requested": 1,
        "dynarec_enabled": 1,
        "diagnostic_history_enabled": 0,
        "delayed_admission_enabled": 1,
        "archive_enabled": 0,
        "max_sample_raw_ns": 1_000,
        "max_sample_corrected_ns": 1_000
        - (1 + surrounding_floor_segments) * 10,
        "overflow": 0,
        "underflow": 0,
        "invalid": 0,
        "q_raw_ppm": q_ppm,
        "q_corrected_ppm": q_corrected_ppm,
        "class": report_class,
        "classifiable": 1,
        "eligible": eligible,
    }
    costs: List[Record] = [
        {"version": 1, "cost": "try_all", "events": paired, "raw_ns": try_raw, "corrected_ns": try_corrected},
        {"version": 1, "cost": "try_paired", "events": paired, "raw_ns": try_raw, "corrected_ns": try_corrected},
        {"version": 1, "cost": "compiled_execute", "events": paired, "raw_ns": compiled_raw, "corrected_ns": compiled_corrected},
        {"version": 1, "cost": "paired_surrounding", "events": paired, "raw_ns": surrounding_raw, "corrected_ns": surrounding_corrected},
        {"version": 1, "cost": "try_unpaired", "events": 0, "raw_ns": 0, "corrected_ns": 0},
    ]
    strata: List[Record] = []
    for index in range(STRATA_COUNT):
        strata.append(
            {
                "version": 1,
                "index": index,
                "paired": per_stratum,
                "try_raw_ns": try_raw_per_stratum,
                "try_corrected_ns": try_corrected_per_stratum,
                "compiled_raw_ns": compiled_raw_per_stratum,
                "compiled_corrected_ns": compiled_corrected_per_stratum,
                "surrounding_raw_ns": surrounding_raw_per_stratum,
                "surrounding_corrected_ns": surrounding_corrected_per_stratum,
                "q_raw_ppm": q_ppm,
                "q_corrected_ppm": q_corrected_ppm,
            }
        )
    owners: List[Record] = [
        {
            "version": 1,
            "ordinal": 1,
            "generation": generation,
            "derived_seed": _mix_seed(seed, generation, 1),
            "block_index": paired - 1,
            "draw_count": paired,
            "position": _position_for_draw(
                _mix_seed(seed, generation, 1), paired
            ),
            "represented_calls": paired * BLOCK_SIZE,
        }
    ]
    checksum = _checksum(summary, costs, strata, owners)
    end: Record = {
        "version": 1,
        "complete": 1,
        "checksum": f"{checksum:016x}",
        "reports": 1,
    }
    lines = [_render_record(PREFIXES["SUMMARY"], SUMMARY_FIELDS, summary)]
    lines.extend(_render_record(PREFIXES["COST"], COST_FIELDS, record) for record in costs)
    lines.extend(
        _render_record(PREFIXES["STRATUM"], STRATUM_FIELDS, record)
        for record in strata
    )
    lines.extend(_render_record(PREFIXES["OWNER"], OWNER_FIELDS, record) for record in owners)
    lines.append(_render_record(PREFIXES["END"], END_FIELDS, end))
    return "\n".join(lines) + "\n"


def _edit_synthetic_report(
    text: str, edits: Sequence[Tuple[str, int, str, Value]]
) -> str:
    lines = text.splitlines()
    source = "self-test-editor"
    summary = _parse_record(lines[0], 1, source, "SUMMARY", SUMMARY_FIELDS)
    costs = [
        _parse_record(lines[index + 1], index + 2, source, "COST", COST_FIELDS)
        for index in range(len(COST_NAMES))
    ]
    strata_start = 1 + len(COST_NAMES)
    strata = [
        _parse_record(
            lines[strata_start + index],
            strata_start + index + 1,
            source,
            "STRATUM",
            STRATUM_FIELDS,
        )
        for index in range(STRATA_COUNT)
    ]
    owner_start = strata_start + STRATA_COUNT
    owners = [
        _parse_record(
            lines[index], index + 1, source, "OWNER", OWNER_FIELDS
        )
        for index in range(owner_start, len(lines) - 1)
    ]
    end = _parse_record(lines[-1], len(lines), source, "END", END_FIELDS)
    records = {
        "SUMMARY": [summary],
        "COST": costs,
        "STRATUM": strata,
        "OWNER": owners,
        "END": [end],
    }
    for kind, index, field, value in edits:
        if kind not in records or not 0 <= index < len(records[kind]):
            raise AssertionError("bad synthetic edit record")
        if field not in records[kind][index]:
            raise AssertionError("bad synthetic edit field")
        records[kind][index][field] = value
    end["checksum"] = f"{_checksum(summary, costs, strata, owners):016x}"
    rendered = [_render_record(PREFIXES["SUMMARY"], SUMMARY_FIELDS, summary)]
    rendered.extend(
        _render_record(PREFIXES["COST"], COST_FIELDS, record)
        for record in costs
    )
    rendered.extend(
        _render_record(PREFIXES["STRATUM"], STRATUM_FIELDS, record)
        for record in strata
    )
    rendered.extend(
        _render_record(PREFIXES["OWNER"], OWNER_FIELDS, record)
        for record in owners
    )
    rendered.append(_render_record(PREFIXES["END"], END_FIELDS, end))
    return "\n".join(rendered) + "\n"


def _expect_invalid(text: str, expected_fragment: str) -> None:
    try:
        parse_report_text(text, "self-test-invalid")
    except ValidationError as error:
        if expected_fragment not in str(error):
            raise AssertionError(
                f"expected error containing {expected_fragment!r}, got {error!s}"
            ) from error
    else:
        raise AssertionError("invalid self-test report was accepted")


def self_test() -> None:
    derived_seed = _mix_seed(1, 1, 1)
    if derived_seed != 7642374066461616266:
        raise AssertionError("known MixSeed vector changed")
    if _position_for_draw(derived_seed, 4096) != 25267:
        raise AssertionError("known xorshift64* draw-4096 vector changed")
    iterative_state = derived_seed
    for draw_count in range(1, 258):
        iterative_state = _xorshift_state(iterative_state)
        iterative_position = (
            (iterative_state * 2685821657736338717) & UINT64_MAX
        ) & 0xFFFF
        if _position_for_draw(derived_seed, draw_count) != iterative_position:
            raise AssertionError("xorshift64* jump-ahead mismatch")

    r1_text = _synthetic_report(1, 700_000)
    r2_text = _synthetic_report(2, 700_000)
    if "checksum=5c13c2a448893e49" not in r1_text:
        raise AssertionError("known canonical R1 checksum changed")
    if "checksum=223dc2647c932b9b" not in r2_text:
        raise AssertionError("known canonical R2 checksum changed")
    r1 = parse_report_text("unrelated log line\n" + r1_text, "self-test-r1")
    r2 = parse_report_text(r2_text, "self-test-r2")
    report_class, delta = validate_replica_gate(r1, r2)
    if report_class != "NATIVE_EXECUTE_DOMINANT" or delta != 0:
        raise AssertionError("valid replica gate returned unexpected result")

    end_line = r1_text.splitlines()[-1]
    checksum_match = re.search(r"checksum=([0-9a-f]{16})", end_line)
    if checksum_match is None:
        raise AssertionError("synthetic END has no checksum")
    checksum_text = checksum_match.group(1)
    replacement_checksum = checksum_text[:-1] + (
        "0" if checksum_text[-1] != "0" else "1"
    )
    corrupted_checksum = r1_text.replace(
        end_line,
        end_line.replace(checksum_text, replacement_checksum, 1),
    )
    _expect_invalid(corrupted_checksum, "checksum mismatch")
    _expect_invalid(r1_text.replace("seed=1", "seed=01", 1), "canonical unsigned decimal")
    _expect_invalid(r1_text.replace(" eligible=1", " extra=0 eligible=1", 1), "extra or missing")

    reordered = r1_text.splitlines()
    reordered[1], reordered[2] = reordered[2], reordered[1]
    _expect_invalid("\n".join(reordered) + "\n", "COST labels")
    _expect_invalid(
        _synthetic_report(1, 700_000, eligible_override=0),
        "eligible does not match recomputed value",
    )
    _expect_invalid(
        _edit_synthetic_report(
            r1_text,
            [
                ("SUMMARY", 0, "paired_steps", 0),
                ("SUMMARY", 0, "paired_direct_steps", 0),
            ],
        ),
        "paired_steps cannot represent",
    )
    _expect_invalid(
        _edit_synthetic_report(
            r1_text,
            [
                ("SUMMARY", 0, "paired_steps", 8192),
                ("SUMMARY", 0, "paired_direct_steps", 4096),
            ],
        ),
        "must equal direct steps",
    )
    _expect_invalid(
        _edit_synthetic_report(
            r1_text,
            [
                ("STRATUM", 0, "paired", 257),
                ("STRATUM", 1, "paired", 255),
            ],
        ),
        "modulo-16 assignment",
    )
    _expect_invalid(
        _edit_synthetic_report(
            r1_text,
            [
                ("SUMMARY", 0, "max_sample_raw_ns", 0),
                ("SUMMARY", 0, "max_sample_corrected_ns", 0),
            ],
        ),
        "max_sample_raw_ns is impossible",
    )
    _expect_invalid(
        _edit_synthetic_report(
            r1_text,
            [
                ("COST", 2, "corrected_ns", 2_826_241),
                ("COST", 3, "corrected_ns", 1_146_879),
                ("STRATUM", 0, "compiled_corrected_ns", 176_641),
                ("STRATUM", 0, "surrounding_corrected_ns", 71_679),
                ("STRATUM", 0, "q_corrected_ppm", 711_344),
            ],
        ),
        "raw/corrected gap",
    )
    zero_surrounding = parse_report_text(
        _synthetic_report(1, 1_000_000, surrounding_floor_segments=0),
        "self-test-zero-surrounding",
    )
    if (
        not zero_surrounding.eligible
        or _number(zero_surrounding.costs[3], "raw_ns") != 0
        or _number(zero_surrounding.costs[3], "corrected_ns") != 0
    ):
        raise AssertionError("zero-tick surrounding fixture was not preserved")
    partial_zero_text = _synthetic_report(
        1, 700_000, surrounding_floor_segments=1
    )
    partial_zero = parse_report_text(
        partial_zero_text, "self-test-partial-zero-surrounding"
    )
    if not partial_zero.eligible:
        raise AssertionError("partial zero-tick fixture became ineligible")
    shifted_compiled = _number(partial_zero.costs[2], "corrected_ns") + 1
    shifted_surrounding = _number(partial_zero.costs[3], "corrected_ns") - 1
    shifted_summary_q = _ratio_ppm(
        shifted_compiled, _number(partial_zero.costs[1], "corrected_ns")
    )
    shifted_stratum_compiled = (
        _number(partial_zero.strata[0], "compiled_corrected_ns") + 1
    )
    shifted_stratum_surrounding = (
        _number(partial_zero.strata[0], "surrounding_corrected_ns") - 1
    )
    shifted_stratum_q = _ratio_ppm(
        shifted_stratum_compiled,
        _number(partial_zero.strata[0], "try_corrected_ns"),
    )
    _expect_invalid(
        _edit_synthetic_report(
            partial_zero_text,
            [
                ("SUMMARY", 0, "q_corrected_ppm", shifted_summary_q),
                ("COST", 2, "corrected_ns", shifted_compiled),
                ("COST", 3, "corrected_ns", shifted_surrounding),
                (
                    "STRATUM",
                    0,
                    "compiled_corrected_ns",
                    shifted_stratum_compiled,
                ),
                (
                    "STRATUM",
                    0,
                    "surrounding_corrected_ns",
                    shifted_stratum_surrounding,
                ),
                ("STRATUM", 0, "q_corrected_ppm", shifted_stratum_q),
            ],
        ),
        "not floor-aligned",
    )
    _expect_invalid(
        _edit_synthetic_report(
            r1_text, [("SUMMARY", 0, "class", "MIXED")]
        ),
        "class does not match",
    )
    _expect_invalid(
        _edit_synthetic_report(
            r1_text, [("SUMMARY", 0, "fast_blocks_enabled", 0)]
        ),
        "eligible does not match",
    )
    _expect_invalid(
        r1_text.replace("seed=1", "seed=" + "9" * 1000, 1),
        "exceeds uint64",
    )

    distant_r2 = parse_report_text(_synthetic_report(2, 600_000), "self-test-r2-distant")
    try:
        validate_replica_gate(r1, distant_r2)
    except ValidationError as error:
        if "delta exceeds 50000" not in str(error):
            raise AssertionError(f"unexpected replica gate error: {error!s}") from error
    else:
        raise AssertionError("replica delta violation was accepted")
    different_class_r2 = parse_report_text(
        _synthetic_report(2, 300_000), "self-test-r2-different-class"
    )
    try:
        validate_replica_gate(r1, different_class_r2)
    except ValidationError as error:
        if "classes differ" not in str(error):
            raise AssertionError(f"unexpected replica class error: {error!s}") from error
    else:
        raise AssertionError("replica class disagreement was accepted")
    try:
        validate_replica_gate(r2, r1)
    except ValidationError as error:
        if "R1 seed=1" not in str(error):
            raise AssertionError(f"unexpected replica seed error: {error!s}") from error
    else:
        raise AssertionError("reversed replica seeds were accepted")


def _read_path(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    try:
        return pathlib.Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ValidationError(f"{path}: cannot read UTF-8 report log: {error}") from error


def _run_report_binary(path: str) -> Report:
    command = [path, "--a64-dynarec-phase6af-telemetry-report-only"]
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            timeout=60,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise ValidationError(f"{path}: report binary timed out") from error
    except (OSError, UnicodeError) as error:
        raise ValidationError(f"{path}: cannot run report binary: {error}") from error
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or completed.stdout.strip()
        if len(diagnostic) > 500:
            diagnostic = diagnostic[-500:]
        suffix = f": {diagnostic}" if diagnostic else ""
        raise ValidationError(
            f"{path}: report binary exited with {completed.returncode}{suffix}"
        )
    return parse_report_text(completed.stderr, f"{path} stderr")


def _main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="validate one phase-6AF report or the canonical R1/R2 gate"
    )
    parser.add_argument("reports", nargs="*", metavar="REPORT", help="one report, or R1 then R2; use - for stdin")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run deterministic synthetic positive and negative fixtures",
    )
    parser.add_argument(
        "--run-report-binary",
        metavar="BINARY",
        help=(
            "run a CpuX64 test binary's phase-6AF report-only mode and "
            "validate its stderr"
        ),
    )
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        if arguments.reports or arguments.run_report_binary is not None:
            parser.error("--self-test does not accept reports or a report binary")
        self_test()
        print("phase6af-report-self-test: PASS")
        return 0
    if arguments.run_report_binary is not None:
        if arguments.reports:
            parser.error("--run-report-binary does not accept report paths")
        report = _run_report_binary(arguments.run_report_binary)
        summary = report.summary
        print(
            "phase6af-report-binary: PASS "
            f"class={report.report_class} "
            f"classifiable={_number(summary, 'classifiable')} "
            f"eligible={_number(summary, 'eligible')} "
            f"checksum={_text(report.end, 'checksum')}"
        )
        return 0
    if len(arguments.reports) not in (1, 2):
        parser.error("provide one report, or R1 followed by R2")
    if arguments.reports.count("-") > 1:
        parser.error("stdin may be used for only one report")

    reports = [
        parse_report_text(_read_path(path), "<stdin>" if path == "-" else path)
        for path in arguments.reports
    ]
    for report in reports:
        summary = report.summary
        print(
            f"{report.source}: VALID class={report.report_class} "
            f"classifiable={_number(summary, 'classifiable')} "
            f"eligible={_number(summary, 'eligible')} "
            f"q_raw_ppm={_number(summary, 'q_raw_ppm')} "
            f"q_corrected_ppm={report.q_corrected_ppm} "
            f"paired={_number(summary, 'paired')} "
            f"checksum={_text(report.end, 'checksum')}"
        )
    if len(reports) == 2:
        report_class, delta = validate_replica_gate(reports[0], reports[1])
        print(
            f"phase6af-replica-gate: PASS class={report_class} "
            f"q_corrected_ppm_r1={reports[0].q_corrected_ppm} "
            f"q_corrected_ppm_r2={reports[1].q_corrected_ppm} "
            f"delta_ppm={delta}"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except ValidationError as error:
        print(f"phase6af-report: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
