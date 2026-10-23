#!/usr/bin/env python3
"""Run the preregistered phase-6AO verified-IR mechanical A/B matrix."""

from __future__ import annotations

import argparse
import dataclasses
import decimal
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time
from typing import Dict, List, Mapping, Optional, Sequence, Tuple


PHASE = "6AO"
TIMEOUT_SECONDS = 120
SELECTOR = "LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE"
MAXIMUM_MEDIAN_REGRESSION_PERCENT = decimal.Decimal("2")
MAXIMUM_PAIR_REGRESSION_PERCENT = decimal.Decimal("5")
PREFIX = b"VERIFIED_IR_BENCH "
EXPECTED_ITERATIONS = 200000
EXPECTED_OPERATIONS = 16
EXPECTED_ENCODED_BYTES = 1352
EXPECTED_DIRECT_INSTRUCTIONS = 16
EXPECTED_CHECKSUM = "86fb64d3cc7d1437"

MATRIX: Tuple[Tuple[str, str, int], ...] = (
    ("B1", "B", 0),
    ("C1", "C", 1),
    ("C2", "C", 1),
    ("B2", "B", 0),
    ("B3", "B", 0),
    ("C3", "C", 1),
)

FIELDS = (
    "variant",
    "iterations",
    "operations",
    "elapsed_ns",
    "status",
    "encoded_bytes",
    "total_encoded_bytes",
    "direct_instructions",
    "validations",
    "full",
    "structure",
    "checksum",
)
INTEGER_FIELDS = (
    "iterations",
    "operations",
    "elapsed_ns",
    "encoded_bytes",
    "total_encoded_bytes",
    "direct_instructions",
    "validations",
    "full",
    "structure",
)
CANONICAL_INTEGER = re.compile(r"0|[1-9][0-9]*\Z")
CHECKSUM = re.compile(r"[0-9a-f]{16}\Z")

# Keep the ordinary launch environment, but strip inherited benchmark controls,
# profilers, interposers, sanitizers, and graphics diagnostics. The selector is
# the only LUMEN_* variable admitted to the child.
CONTAMINATING_PREFIXES = (
    "LUMEN_",
    "DYLD_",
    "LLVM_PROFILE",
    "CPUPROFILE",
    "HEAPPROFILE",
    "ASAN_",
    "LSAN_",
    "MSAN_",
    "TSAN_",
    "UBSAN_",
    "VK_",
    "MVK_",
    "MTL_",
    "METAL_",
)


class BenchError(RuntimeError):
    """A fail-closed phase-6AO benchmark error."""


@dataclasses.dataclass(frozen=True)
class RunSpec:
    label: str
    arm: str
    selector: int


@dataclasses.dataclass(frozen=True)
class RunOutcome:
    spec: RunSpec
    valid: bool
    record: Optional[Mapping[str, object]]
    result_sha256: str = ""


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _contains_bytes(path: pathlib.Path, needle: bytes) -> bool:
    overlap = max(0, len(needle) - 1)
    previous = b""
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                return False
            candidate = previous + chunk
            if needle in candidate:
                return True
            previous = candidate[-overlap:] if overlap else b""


def _write_json(path: pathlib.Path, value: Mapping[str, object]) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def _sterile_environment(
    selector: int, inherited: Optional[Mapping[str, str]] = None
) -> Tuple[Mapping[str, str], List[str]]:
    if selector not in (0, 1):
        raise BenchError(f"invalid selector: {selector}")
    source = os.environ if inherited is None else inherited
    removed = sorted(
        name
        for name in source
        if any(name.startswith(prefix) for prefix in CONTAMINATING_PREFIXES)
    )
    environment = {name: value for name, value in source.items() if name not in removed}
    environment[SELECTOR] = str(selector)
    if not _environment_is_sterile(environment, selector):
        raise BenchError("failed to construct a sterile child environment")
    return environment, removed


def _environment_is_sterile(environment: Mapping[str, str], selector: int) -> bool:
    lumen = {name: value for name, value in environment.items() if name.startswith("LUMEN_")}
    if lumen != {SELECTOR: str(selector)}:
        return False
    return not any(
        name != SELECTOR
        and any(name.startswith(prefix) for prefix in CONTAMINATING_PREFIXES)
        for name in environment
    )


def _canonical_integer(name: str, value: str) -> int:
    if CANONICAL_INTEGER.fullmatch(value) is None:
        raise BenchError(f"non-canonical {name}: {value!r}")
    return int(value)


def _parse_output(
    stdout: bytes, stderr: bytes, expected_variant: str
) -> Mapping[str, object]:
    if stderr:
        raise BenchError("benchmark wrote to stderr")
    lines = stdout.splitlines()
    if len(lines) != 1 or stdout != lines[0] + b"\n":
        raise BenchError("expected exactly one newline-terminated stdout line")
    line = lines[0]
    if not line.startswith(PREFIX):
        raise BenchError("missing VERIFIED_IR_BENCH prefix")
    try:
        tokens = line[len(PREFIX) :].decode("ascii").split(" ")
    except UnicodeDecodeError as error:
        raise BenchError("benchmark record is not ASCII") from error
    if not tokens or any(not token or token.count("=") != 1 for token in tokens):
        raise BenchError("malformed benchmark fields")
    pairs = [token.split("=", 1) for token in tokens]
    names = tuple(name for name, _value in pairs)
    if names != FIELDS:
        raise BenchError(f"unexpected field order or roster: {names}")
    values = {name: value for name, value in pairs}
    if values["variant"] != expected_variant:
        raise BenchError(
            f"variant mismatch: expected {expected_variant}, got {values['variant']}"
        )
    if values["status"] != "Ok":
        raise BenchError(f"benchmark status is not Ok: {values['status']}")
    if CHECKSUM.fullmatch(values["checksum"]) is None:
        raise BenchError(f"non-canonical checksum: {values['checksum']!r}")

    record: Dict[str, object] = {
        "variant": values["variant"],
        "status": values["status"],
        "checksum": values["checksum"],
    }
    for name in INTEGER_FIELDS:
        record[name] = _canonical_integer(name, values[name])

    iterations = int(record["iterations"])
    operations = int(record["operations"])
    elapsed_ns = int(record["elapsed_ns"])
    encoded_bytes = int(record["encoded_bytes"])
    direct = int(record["direct_instructions"])
    validations = int(record["validations"])
    full = int(record["full"])
    structure = int(record["structure"])
    if iterations <= 0 or operations <= 0 or elapsed_ns <= 0 or encoded_bytes <= 0:
        raise BenchError("iterations, operations, elapsed_ns, and encoded_bytes must be positive")
    if direct < 0:
        raise BenchError("direct instruction count must be non-negative")
    if int(record["total_encoded_bytes"]) != iterations * encoded_bytes:
        raise BenchError("total_encoded_bytes does not match iterations * encoded_bytes")
    exact_oracle = {
        "iterations": EXPECTED_ITERATIONS,
        "operations": EXPECTED_OPERATIONS,
        "encoded_bytes": EXPECTED_ENCODED_BYTES,
        "direct_instructions": EXPECTED_DIRECT_INSTRUCTIONS,
        "checksum": EXPECTED_CHECKSUM,
    }
    observed_oracle = {name: record[name] for name in exact_oracle}
    if observed_oracle != exact_oracle:
        raise BenchError(
            f"benchmark corpus oracle mismatch: expected={exact_oracle} "
            f"observed={observed_oracle}"
        )

    if expected_variant not in ("baseline", "candidate"):
        raise BenchError(f"unsupported expected variant: {expected_variant}")
    # The dedicated Release benchmark deliberately omits the test-counter
    # macro. Exact scan counts are proved by the separately instrumented unit
    # suite; any non-zero value here means the timed binary was contaminated.
    expected_counters = (0, 0, 0)
    observed_counters = (validations, full, structure)
    if observed_counters != expected_counters:
        raise BenchError(
            "verification counters mismatch: "
            f"expected={expected_counters} observed={observed_counters}"
        )
    return record


def _format_percent(value: decimal.Decimal) -> str:
    return format(value.quantize(decimal.Decimal("0.000001")), "f")


def _regression_percent(baseline: int, candidate: int) -> decimal.Decimal:
    if baseline <= 0 or candidate <= 0:
        raise BenchError("timings must be positive")
    with decimal.localcontext() as context:
        context.prec = 50
        return (
            decimal.Decimal(candidate - baseline)
            * decimal.Decimal(100)
            / decimal.Decimal(baseline)
        )


def _evaluate_decision(
    outcomes: Sequence[RunOutcome], frozen_binary_sha256: str, final_binary_sha256: str
) -> Mapping[str, object]:
    expected_specs = [RunSpec(*item) for item in MATRIX]
    observed_specs = [outcome.spec for outcome in outcomes]
    by_label = {outcome.spec.label: outcome for outcome in outcomes}
    expected_labels = [spec.label for spec in expected_specs]
    labels_unique = len(by_label) == len(outcomes)
    records = [outcome.record for outcome in outcomes if outcome.record is not None]
    functional = all(
        (
            observed_specs == expected_specs,
            labels_unique,
            len(records) == len(MATRIX),
            all(outcome.valid for outcome in outcomes),
            final_binary_sha256 == frozen_binary_sha256,
        )
    )

    equivalence_fields = (
        "iterations",
        "operations",
        "encoded_bytes",
        "direct_instructions",
        "checksum",
    )
    equivalence_reference: Optional[Mapping[str, object]] = None
    equivalence_gate = False
    if functional:
        equivalence_reference = {
            name: records[0][name] for name in equivalence_fields
        }
        exact_reference = {
            "iterations": EXPECTED_ITERATIONS,
            "operations": EXPECTED_OPERATIONS,
            "encoded_bytes": EXPECTED_ENCODED_BYTES,
            "direct_instructions": EXPECTED_DIRECT_INSTRUCTIONS,
            "checksum": EXPECTED_CHECKSUM,
        }
        equivalence_gate = equivalence_reference == exact_reference and all(
            all(record[name] == equivalence_reference[name] for name in equivalence_fields)
            for record in records
        )
    functional = functional and equivalence_gate

    performance: Mapping[str, object] = {}
    performance_gate = False
    if functional:
        baseline_values = [
            int(by_label[f"B{index}"].record["elapsed_ns"])  # type: ignore[index]
            for index in (1, 2, 3)
        ]
        candidate_values = [
            int(by_label[f"C{index}"].record["elapsed_ns"])  # type: ignore[index]
            for index in (1, 2, 3)
        ]
        baseline_median = sorted(baseline_values)[1]
        candidate_median = sorted(candidate_values)[1]
        median_regression = _regression_percent(baseline_median, candidate_median)
        median_gate = candidate_median * 100 <= baseline_median * 102
        pairs: List[Mapping[str, object]] = []
        for index in (1, 2, 3):
            baseline = int(by_label[f"B{index}"].record["elapsed_ns"])  # type: ignore[index]
            candidate = int(by_label[f"C{index}"].record["elapsed_ns"])  # type: ignore[index]
            regression = _regression_percent(baseline, candidate)
            gate = candidate * 100 <= baseline * 105
            pairs.append(
                {
                    "pair": f"B{index}/C{index}",
                    "baseline_elapsed_ns": baseline,
                    "candidate_elapsed_ns": candidate,
                    "regression_percent": _format_percent(regression),
                    "improvement_percent": _format_percent(-regression),
                    "maximum_regression_percent": _format_percent(
                        MAXIMUM_PAIR_REGRESSION_PERCENT
                    ),
                    "gate": gate,
                }
            )
        pair_gate = all(bool(pair["gate"]) for pair in pairs)
        performance_gate = median_gate and pair_gate
        performance = {
            "baseline_values_ns": baseline_values,
            "candidate_values_ns": candidate_values,
            "baseline_median_ns": baseline_median,
            "candidate_median_ns": candidate_median,
            "median_regression_percent": _format_percent(median_regression),
            "median_improvement_percent": _format_percent(-median_regression),
            "maximum_median_regression_percent": _format_percent(
                MAXIMUM_MEDIAN_REGRESSION_PERCENT
            ),
            "median_gate": median_gate,
            "pairs": pairs,
            "pairs_gate": pair_gate,
            "gate": performance_gate,
        }

    if not functional:
        verdict = "INVALID"
    elif performance_gate:
        verdict = "PASS"
    else:
        verdict = "REGRESSION"
    return {
        "version": 1,
        "phase": PHASE,
        "kind": "verified-ir-mechanical-ab",
        "verdict": verdict,
        "matrix_order_expected": expected_labels,
        "matrix_order_observed": [spec.label for spec in observed_specs],
        "matrix_complete": observed_specs == expected_specs,
        "labels_unique": labels_unique,
        "functional_gates_valid": functional,
        "equivalence_fields": list(equivalence_fields),
        "equivalence_reference": equivalence_reference,
        "equivalence_gate": equivalence_gate,
        "frozen_binary_sha256": frozen_binary_sha256,
        "final_binary_sha256": final_binary_sha256,
        "binary_unchanged": final_binary_sha256 == frozen_binary_sha256,
        "runs": [
            {
                "run": outcome.spec.label,
                "arm": outcome.spec.arm,
                "selector": outcome.spec.selector,
                "valid": outcome.valid,
                "record": outcome.record,
                "result_sha256": outcome.result_sha256,
            }
            for outcome in outcomes
        ],
        "performance": performance,
        "finished_unix_ns": time.time_ns(),
    }


def _file_description(binary: pathlib.Path) -> str:
    file_tool = pathlib.Path("/usr/bin/file")
    executable = str(file_tool) if file_tool.is_file() else shutil.which("file")
    if executable is None:
        raise BenchError("cannot locate the file utility")
    completed = subprocess.run(
        [executable, "-b", str(binary)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=10,
    )
    if completed.returncode != 0 or completed.stderr:
        raise BenchError(f"file utility failed: {completed.stderr.strip()}")
    return completed.stdout.strip()


def _release_cache(binary: pathlib.Path) -> pathlib.Path:
    for directory in (binary.parent, *binary.parents):
        cache = directory / "CMakeCache.txt"
        if not cache.is_file():
            continue
        contents = cache.read_text(encoding="utf-8", errors="strict")
        if re.search(r"^CMAKE_BUILD_TYPE:STRING=Release$", contents, re.MULTILINE):
            return cache
        raise BenchError(f"nearest CMake cache is not Release: {cache}")
    raise BenchError("cannot prove a Release build from CMakeCache.txt")


def _validate_inputs(
    arguments: argparse.Namespace,
) -> Tuple[pathlib.Path, pathlib.Path, str, pathlib.Path]:
    if sys.platform != "darwin" or platform.machine() != "arm64":
        raise BenchError("live phase-6AO benchmark requires native arm64 macOS")
    binary = pathlib.Path(arguments.binary).resolve(strict=True)
    output = pathlib.Path(arguments.output).resolve()
    if os.path.lexists(os.fspath(output)):
        raise BenchError(f"output root already exists: {output}")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise BenchError(f"binary is not executable: {binary}")
    if binary.name != "lumen_verified_ir_benchmark":
        raise BenchError(f"unexpected benchmark binary name: {binary.name}")
    description = _file_description(binary)
    if "Mach-O 64-bit executable arm64" not in description or "universal" in description.lower():
        raise BenchError(f"binary is not thin arm64: {description}")
    cache = _release_cache(binary)
    if not _contains_bytes(binary, PREFIX.rstrip()):
        raise BenchError("binary does not contain the benchmark marker")
    if not _contains_bytes(binary, SELECTOR.encode("ascii")):
        raise BenchError("binary does not contain the verified-IR selector")
    return binary, output, description, cache


def _run_one(
    spec: RunSpec,
    binary: pathlib.Path,
    output_root: pathlib.Path,
    frozen_binary_sha256: str,
) -> RunOutcome:
    run_root = output_root / spec.label
    run_root.mkdir()
    stdout_path = run_root / "stdout.log"
    stderr_path = run_root / "stderr.log"
    environment, removed = _sterile_environment(spec.selector)
    expected_variant = "baseline" if spec.arm == "B" else "candidate"
    before_sha256 = _sha256_file(binary)
    launched = False
    timed_out = False
    exit_code: Optional[int] = None
    launch_error: Optional[str] = None
    started_ns = time.time_ns()
    with stdout_path.open("xb") as stdout_log, stderr_path.open("xb") as stderr_log:
        if before_sha256 == frozen_binary_sha256:
            try:
                completed = subprocess.run(
                    [str(binary), "--verified-ir-benchmark"],
                    cwd=run_root,
                    env=environment,
                    stdin=subprocess.DEVNULL,
                    stdout=stdout_log,
                    stderr=stderr_log,
                    check=False,
                    timeout=TIMEOUT_SECONDS,
                )
                launched = True
                exit_code = completed.returncode
            except subprocess.TimeoutExpired:
                launched = True
                timed_out = True
                launch_error = f"timeout after {TIMEOUT_SECONDS} seconds"
            except (OSError, subprocess.SubprocessError) as error:
                launch_error = str(error)
        else:
            launch_error = "binary hash changed before launch"

    after_sha256 = _sha256_file(binary)
    stdout = stdout_path.read_bytes()
    stderr = stderr_path.read_bytes()
    record: Optional[Mapping[str, object]] = None
    parser_error: Optional[str] = None
    try:
        record = _parse_output(stdout, stderr, expected_variant)
    except BenchError as error:
        parser_error = str(error)
    checks = {
        "binary_unchanged_before": before_sha256 == frozen_binary_sha256,
        "binary_unchanged_after": after_sha256 == frozen_binary_sha256,
        "environment_sterile": _environment_is_sterile(environment, spec.selector),
        "launched": launched,
        "launch_error_absent": launch_error is None,
        "timeout_absent": not timed_out,
        "exit_zero": exit_code == 0,
        "parser_valid": parser_error is None,
    }
    valid = all(checks.values())
    result: Mapping[str, object] = {
        "version": 1,
        "phase": PHASE,
        "kind": "verified-ir-mechanical-ab",
        "run": spec.label,
        "arm": spec.arm,
        "selector": spec.selector,
        "valid": valid,
        "checks": checks,
        "failed_checks": [name for name, passed in checks.items() if not passed],
        "binary": str(binary),
        "binary_sha256_before": before_sha256,
        "binary_sha256_after": after_sha256,
        "environment_removed": removed,
        "environment_set": {SELECTOR: str(spec.selector)},
        "launched": launched,
        "timed_out": timed_out,
        "timeout_seconds": TIMEOUT_SECONDS,
        "exit_code": exit_code,
        "launch_error": launch_error,
        "parser_error": parser_error,
        "stdout_size": len(stdout),
        "stderr_size": len(stderr),
        "record": record,
        "started_unix_ns": started_ns,
        "finished_unix_ns": time.time_ns(),
    }
    result_path = run_root / "result.json"
    _write_json(result_path, result)
    return RunOutcome(spec, valid, record, _sha256_file(result_path))


def run_matrix(arguments: argparse.Namespace) -> int:
    binary, output_root, description, release_cache = _validate_inputs(arguments)
    frozen_binary_sha256 = _sha256_file(binary)
    output_root.mkdir(parents=True)
    _write_json(
        output_root / "preflight.json",
        {
            "version": 1,
            "phase": PHASE,
            "kind": "verified-ir-mechanical-ab",
            "binary": str(binary),
            "binary_file": description,
            "binary_sha256": frozen_binary_sha256,
            "release_cache": str(release_cache),
            "release_cache_sha256": _sha256_file(release_cache),
            "matrix": [
                {"run": label, "arm": arm, "selector": selector}
                for label, arm, selector in MATRIX
            ],
            "timeout_seconds": TIMEOUT_SECONDS,
        },
    )
    outcomes: List[RunOutcome] = []
    for label, arm, selector in MATRIX:
        outcome = _run_one(
            RunSpec(label, arm, selector),
            binary,
            output_root,
            frozen_binary_sha256,
        )
        outcomes.append(outcome)
        elapsed = outcome.record.get("elapsed_ns") if outcome.record else None
        print(
            "phase6ao-bench-run: "
            f"{'PASS' if outcome.valid else 'INVALID'} run={label} "
            f"selector={selector} elapsed_ns={elapsed}"
        )
    final_binary_sha256 = _sha256_file(binary)
    decision = _evaluate_decision(
        outcomes, frozen_binary_sha256, final_binary_sha256
    )
    decision_path = output_root / "decision.json"
    _write_json(decision_path, decision)
    decision_sha256 = _sha256_file(decision_path)
    (output_root / "decision.sha256").write_text(
        f"{decision_sha256}  decision.json\n", encoding="ascii"
    )
    print(
        "phase6ao-bench: "
        f"{decision['verdict']} decision_sha256={decision_sha256}"
    )
    return 0 if decision["verdict"] == "PASS" else 1


def _sample_record(variant: str, elapsed_ns: int = 100) -> Mapping[str, object]:
    iterations = EXPECTED_ITERATIONS
    operations = EXPECTED_OPERATIONS
    return {
        "variant": variant,
        "iterations": iterations,
        "operations": operations,
        "elapsed_ns": elapsed_ns,
        "status": "Ok",
        "encoded_bytes": EXPECTED_ENCODED_BYTES,
        "total_encoded_bytes": iterations * EXPECTED_ENCODED_BYTES,
        "direct_instructions": EXPECTED_DIRECT_INSTRUCTIONS,
        "validations": 0,
        "full": 0,
        "structure": 0,
        "checksum": EXPECTED_CHECKSUM,
    }


def _serialize(record: Mapping[str, object]) -> bytes:
    return PREFIX + " ".join(f"{name}={record[name]}" for name in FIELDS).encode("ascii") + b"\n"


def _self_test_parser() -> None:
    baseline = _serialize(_sample_record("baseline"))
    candidate = _serialize(_sample_record("candidate"))
    if _parse_output(baseline, b"", "baseline")["elapsed_ns"] != 100:
        raise BenchError("parser did not preserve elapsed_ns")
    _parse_output(candidate, b"", "candidate")
    invalid = (
        (baseline + b"\n", b"", "baseline"),
        (baseline, b"warning\n", "baseline"),
        (baseline.replace(b"elapsed_ns=100", b"elapsed_ns=0100"), b"", "baseline"),
        (baseline.replace(b" validations=0", b" validations=1"), b"", "baseline"),
        (candidate.replace(b" structure=0", b" structure=1"), b"", "candidate"),
        (baseline.replace(b"iterations=200000", b"iterations=200001"), b"", "baseline"),
        (baseline.replace(b"operations=16", b"operations=15"), b"", "baseline"),
        (baseline.replace(b"encoded_bytes=1352", b"encoded_bytes=1353"), b"", "baseline"),
        (baseline.replace(b"direct_instructions=16", b"direct_instructions=15"), b"", "baseline"),
        (baseline.replace(EXPECTED_CHECKSUM.encode("ascii"), b"0123456789abcdef"), b"", "baseline"),
        (baseline.replace(b" checksum=", b" extra=1 checksum="), b"", "baseline"),
        (baseline, b"", "candidate"),
    )
    for stdout, stderr, variant in invalid:
        try:
            _parse_output(stdout, stderr, variant)
        except BenchError:
            continue
        raise BenchError("parser accepted malformed output")


def _self_test_environment() -> None:
    inherited = {
        "PATH": "/usr/bin:/bin",
        "LUMEN_UNRELATED": "1",
        SELECTOR: "garbage",
        "DYLD_INSERT_LIBRARIES": "/tmp/interposer.dylib",
        "ASAN_OPTIONS": "detect_leaks=1",
    }
    environment, removed = _sterile_environment(1, inherited)
    if not _environment_is_sterile(environment, 1):
        raise BenchError("sterile environment oracle rejected its output")
    if environment.get("PATH") != inherited["PATH"]:
        raise BenchError("sterile environment removed a safe variable")
    if set(removed) != {
        "LUMEN_UNRELATED",
        SELECTOR,
        "DYLD_INSERT_LIBRARIES",
        "ASAN_OPTIONS",
    }:
        raise BenchError("sterile environment removal roster mismatch")
    contaminated = dict(environment)
    contaminated["LUMEN_LATE_INJECTION"] = "1"
    if _environment_is_sterile(contaminated, 1):
        raise BenchError("environment oracle accepted a second LUMEN variable")


def _self_test_matrix() -> None:
    specs = [RunSpec(*item) for item in MATRIX]
    if [spec.label for spec in specs] != ["B1", "C1", "C2", "B2", "B3", "C3"]:
        raise BenchError("fixed matrix order mismatch")
    if [spec.selector for spec in specs] != [0, 1, 1, 0, 0, 1]:
        raise BenchError("fixed matrix selector mismatch")
    if any((spec.arm == "B") != (spec.selector == 0) for spec in specs):
        raise BenchError("matrix arm/selector mapping mismatch")


def _self_test_outcomes(values: Mapping[str, int]) -> List[RunOutcome]:
    outcomes: List[RunOutcome] = []
    for label, arm, selector in MATRIX:
        variant = "baseline" if arm == "B" else "candidate"
        record = dict(_sample_record(variant, values[label]))
        outcomes.append(RunOutcome(RunSpec(label, arm, selector), True, record))
    return outcomes


def _self_test_decision() -> None:
    baseline = {"B1": 100, "B2": 100, "B3": 100}
    cases = (
        ({**baseline, "C1": 102, "C2": 102, "C3": 102}, "PASS"),
        ({**baseline, "C1": 105, "C2": 100, "C3": 99}, "PASS"),
        ({**baseline, "C1": 103, "C2": 103, "C3": 103}, "REGRESSION"),
        ({**baseline, "C1": 106, "C2": 99, "C3": 99}, "REGRESSION"),
    )
    for values, expected in cases:
        decision = _evaluate_decision(_self_test_outcomes(values), "same", "same")
        if decision["verdict"] != expected:
            raise BenchError(f"decision expected {expected}, got {decision['verdict']}")
    inclusive = _self_test_outcomes(cases[0][0])
    if _evaluate_decision(inclusive, "before", "after")["verdict"] != "INVALID":
        raise BenchError("decision accepted a changed binary")
    if _evaluate_decision(inclusive[:-1], "same", "same")["verdict"] != "INVALID":
        raise BenchError("decision accepted an incomplete matrix")
    reordered = list(inclusive)
    reordered[0], reordered[1] = reordered[1], reordered[0]
    if _evaluate_decision(reordered, "same", "same")["verdict"] != "INVALID":
        raise BenchError("decision accepted a reordered matrix")
    mismatched = list(inclusive)
    changed = dict(mismatched[1].record or {})
    changed["checksum"] = "fedcba9876543210"
    mismatched[1] = dataclasses.replace(mismatched[1], record=changed)
    if _evaluate_decision(mismatched, "same", "same")["verdict"] != "INVALID":
        raise BenchError("decision accepted non-equivalent output")
    uniformly_wrong = []
    for outcome in inclusive:
        changed = dict(outcome.record or {})
        changed["checksum"] = "0123456789abcdef"
        uniformly_wrong.append(dataclasses.replace(outcome, record=changed))
    if _evaluate_decision(uniformly_wrong, "same", "same")["verdict"] != "INVALID":
        raise BenchError("decision accepted a uniformly wrong corpus")
    performance = _evaluate_decision(inclusive, "same", "same")["performance"]
    if performance["median_improvement_percent"] != "-2.000000":
        raise BenchError("decision did not report descriptive median improvement")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="run the fixed phase-6AO baseline/candidate verified-IR benchmark matrix"
    )
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--binary", help="Release thin-arm64 lumen_verified_ir_benchmark")
    parser.add_argument("--output", help="new output directory")
    return parser


def main(argv: Sequence[str]) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        if arguments.binary is not None or arguments.output is not None:
            parser.error("--self-test does not accept --binary or --output")
        try:
            _self_test_parser()
            _self_test_environment()
            _self_test_matrix()
            _self_test_decision()
        except (BenchError, OSError, UnicodeError, subprocess.SubprocessError) as error:
            print(f"phase6ao-bench-self-test: FAIL: {error}", file=sys.stderr)
            return 1
        print("phase6ao-bench-self-test: PASS")
        return 0
    missing = [
        name
        for name, value in (("--binary", arguments.binary), ("--output", arguments.output))
        if value is None
    ]
    if missing:
        parser.error("the following arguments are required: " + ", ".join(missing))
    try:
        return run_matrix(arguments)
    except (BenchError, OSError, UnicodeError, subprocess.SubprocessError) as error:
        print(f"phase6ao-bench: INVALID: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
