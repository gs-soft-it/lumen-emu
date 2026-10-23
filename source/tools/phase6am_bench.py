#!/usr/bin/env python3
"""Run the preregistered phase-6AM mechanical archive guard on macOS."""

from __future__ import annotations

import argparse
import ctypes
import dataclasses
import decimal
import errno
import hashlib
import json
import os
import pathlib
import platform
import re
import shlex
import subprocess
import sys
import time
from typing import Callable, Dict, List, Mapping, Optional, Sequence, Tuple


TIMEOUT_SECONDS = 120
COOLDOWN_SECONDS = 10
COOLDOWN_POLL_SECONDS = 0.1
MAXIMUM_MEDIAN_REGRESSION_PERCENT = decimal.Decimal("2")
MAXIMUM_PAIR_REGRESSION_PERCENT = decimal.Decimal("5")

ARCHIVE_VARIABLE = "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE"
ADMISSION_VARIABLE = "LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION"
ENTRIES_VARIABLE = "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES"
DYNAREC_VARIABLE = "LUMEN_X64_A64_DYNAREC"

MATRIX: Tuple[Tuple[str, str, int], ...] = (
    ("B1", "B", 512),
    ("C1", "C", 1024),
    ("C2", "C", 1024),
    ("B2", "B", 512),
    ("B3", "B", 512),
    ("C3", "C", 1024),
)

EXTERNAL_DIAGNOSTIC_ENVIRONMENT = (
    "ASAN_OPTIONS",
    "DYLD_INSERT_LIBRARIES",
    "LSAN_OPTIONS",
    "MallocStackLogging",
    "MallocStackLoggingNoCompact",
    "METAL_DEVICE_WRAPPER_TYPE",
    "MTL_CAPTURE_ENABLED",
    "MTL_DEBUG_LAYER",
    "NSZombieEnabled",
    "TSAN_OPTIONS",
    "UBSAN_OPTIONS",
    "VK_INSTANCE_LAYERS",
    "VK_LAYER_PATH",
    "VK_LOADER_DEBUG",
)
CONTAMINATING_ENVIRONMENT_PREFIXES = (
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
    "SDL_",
    "OS_ACTIVITY_",
    "OS_SIGNPOST_",
    "Malloc",
    "NSZombie",
    "NSAutoreleaseFreedObjectCheck",
)

BENCHMARK_PREFIX = b"[A64_COMPILED_ARCHIVE_BENCH] "
BENCHMARK_FIELDS = (
    "schema",
    "phase",
    "policy",
    "duration_ns",
    "executions",
    "checksum",
    "build_attempts",
    "compile_attempts",
    "positive_hits",
    "evictions",
    "eviction_pp",
    "negative_hits",
    "negative_evictions",
    "deferrals",
    "promotions",
    "archive_probe",
    "archive_hit",
    "archive_miss",
    "archive_put",
    "archive_stored",
    "archive_drop",
    "archive_budget_drop",
    "archive_invalid",
    "archive_resident_before",
    "archive_resident",
    "archive_peak",
    "archive_code_resident_before",
    "archive_code_resident",
    "archive_code_peak",
    "archive_code_stored",
    "archive_code_hit",
    "archive_code_drop",
    "archive_capacity_resident_before",
    "archive_capacity_resident",
    "archive_capacity_peak",
    "archive_capacity_stored",
    "archive_capacity_hit",
    "archive_capacity_drop",
    "archive_global_capacity_before",
    "archive_global_capacity",
    "archive_global_peak",
    "archive_global_invalid",
    "archive_requested",
    "archive_enabled",
    "healthy",
)
CANONICAL_INTEGER = re.compile(r"0|[1-9][0-9]*")

COMPILER_PATTERN = re.compile(
    r"(?:clang(?:\+\+)?|gcc|g\+\+)(?:-[0-9]+(?:\.[0-9]+)*)?\Z"
)
CONFLICT_EXECUTABLES = {
    "actool",
    "ar",
    "bazel",
    "buck",
    "buck2",
    "c++",
    "cc",
    "cc1",
    "cc1plus",
    "ccache",
    "cmake",
    "codesign",
    "collect2",
    "ctest",
    "debugserver",
    "dsymutil",
    "dtrace",
    "fc_script",
    "gdb",
    "gold",
    "gmake",
    "heap",
    "ibtool",
    "instruments",
    "ld",
    "ld.lld",
    "ld-classic",
    "ld-prime",
    "ld64",
    "ld64.lld",
    "leaks",
    "lld",
    "lld-link",
    "lldb",
    "lldb-rpc-server",
    "link",
    "libtool",
    "lumen_cpu_a64_dynarec_test",
    "lumen_cpu_x64_test",
    "lumenemu",
    "make",
    "meson",
    "metal",
    "metalfe",
    "metallib",
    "mold",
    "ninja",
    "perf",
    "powermetrics",
    "ranlib",
    "sample",
    "sccache",
    "scons",
    "spindump",
    "swift-frontend",
    "swiftc",
    "vmmap",
    "wasm-ld",
    "xcodebuild",
    "xctrace",
}
SYSTEM_SPINDUMP = "/usr/sbin/spindump"


class BenchError(RuntimeError):
    """A fail-closed phase-6AM mechanical runner error."""


@dataclasses.dataclass(frozen=True)
class RunSpec:
    label: str
    arm: str
    entries: int


@dataclasses.dataclass(frozen=True)
class RunOutcome:
    spec: RunSpec
    valid: bool
    durations_ns: Mapping[str, int]
    artifact_hashes: Mapping[str, str]


@dataclasses.dataclass(frozen=True)
class ProcessRow:
    pid: int
    ppid: int
    command: str


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _sha256_or_error(path: pathlib.Path) -> Tuple[Optional[str], Optional[str]]:
    try:
        return _sha256_file(path), None
    except OSError as error:
        return None, str(error)


def _contains_bytes(path: pathlib.Path, needle: bytes) -> bool:
    overlap = max(0, len(needle) - 1)
    previous = b""
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                return False
            data = previous + chunk
            if needle in data:
                return True
            previous = data[-overlap:] if overlap else b""


def _crash_snapshot() -> Mapping[str, object]:
    root = pathlib.Path.home() / "Library" / "Logs" / "DiagnosticReports"
    records: List[Mapping[str, object]] = []
    paths: set[pathlib.Path] = set()
    if root.is_dir():
        for stem in ("fc_script", "lumen_cpu_x64_test"):
            paths.update(root.glob(f"{stem}*.ips"))
            paths.update(root.glob(f"{stem}*.crash"))
    for path in sorted(paths, key=lambda item: item.as_posix()):
        stat_before = path.lstat()
        if path.is_symlink():
            record: Mapping[str, object] = {
                "path": str(path),
                "kind": "symlink",
                "target": os.readlink(path),
                "mtime_ns": stat_before.st_mtime_ns,
            }
        elif path.is_file():
            sha256 = _sha256_file(path)
            stat_after = path.stat()
            if (
                stat_before.st_size != stat_after.st_size
                or stat_before.st_mtime_ns != stat_after.st_mtime_ns
            ):
                raise BenchError(f"crash report changed during snapshot: {path}")
            record = {
                "path": str(path),
                "kind": "file",
                "size": stat_after.st_size,
                "mtime_ns": stat_after.st_mtime_ns,
                "sha256": sha256,
            }
        else:
            record = {
                "path": str(path),
                "kind": "other",
                "size": stat_before.st_size,
                "mtime_ns": stat_before.st_mtime_ns,
            }
        records.append(record)
    encoded = json.dumps(records, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    return {
        "root": str(root),
        "digest": hashlib.sha256(encoded).hexdigest(),
        "records": records,
    }


def _write_json(path: pathlib.Path, value: object) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")


def _write_text(path: pathlib.Path, value: str) -> None:
    with path.open("x", encoding="utf-8") as output:
        output.write(value)


def _format_decimal(value: decimal.Decimal) -> str:
    return format(value, "f")


def _sterile_environment(entries: int) -> Tuple[Mapping[str, str], List[str]]:
    if entries not in (512, 1024):
        raise BenchError(f"unsupported archive entry selector: {entries}")
    environment = os.environ.copy()
    removed = {
        name
        for name in environment
        if any(
            name.startswith(prefix) for prefix in CONTAMINATING_ENVIRONMENT_PREFIXES
        )
    }
    removed.update(
        name for name in EXTERNAL_DIAGNOSTIC_ENVIRONMENT if name in environment
    )
    for name in removed:
        environment.pop(name, None)
    environment[ARCHIVE_VARIABLE] = "1"
    environment[ADMISSION_VARIABLE] = "1"
    environment[ENTRIES_VARIABLE] = str(entries)
    return environment, sorted(removed)


def _environment_is_valid(environment: Mapping[str, str], entries: int) -> bool:
    expected = {
        ARCHIVE_VARIABLE: "1",
        ADMISSION_VARIABLE: "1",
        ENTRIES_VARIABLE: str(entries),
    }
    observed = {
        name: value
        for name, value in environment.items()
        if name.startswith("LUMEN_")
    }
    contaminating = {
        name
        for name in environment
        if name not in expected
        and (
            name in EXTERNAL_DIAGNOSTIC_ENVIRONMENT
            or any(
                name.startswith(prefix)
                for prefix in CONTAMINATING_ENVIRONMENT_PREFIXES
            )
        )
    }
    return (
        observed == expected
        and DYNAREC_VARIABLE not in environment
        and not contaminating
    )


def _parse_benchmark_line(line: bytes) -> Mapping[str, object]:
    if not line.startswith(BENCHMARK_PREFIX):
        raise BenchError("benchmark output contains a non-canonical line")
    try:
        text = line[len(BENCHMARK_PREFIX) :].decode("ascii")
    except UnicodeDecodeError as error:
        raise BenchError("benchmark line is not ASCII") from error
    tokens = text.split(" ")
    if len(tokens) != len(BENCHMARK_FIELDS) or any(not token for token in tokens):
        raise BenchError(
            "benchmark line does not contain the exact ordered field count"
        )
    record: Dict[str, object] = {}
    for expected_name, token in zip(BENCHMARK_FIELDS, tokens):
        name, separator, value = token.partition("=")
        if separator != "=" or name != expected_name or not value:
            raise BenchError(
                f"benchmark field order mismatch at expected {expected_name}"
            )
        if name == "phase":
            if value not in ("hot", "churn"):
                raise BenchError(f"invalid benchmark phase: {value}")
            record[name] = value
        else:
            if CANONICAL_INTEGER.fullmatch(value) is None:
                raise BenchError(f"non-canonical integer for {name}: {value}")
            record[name] = int(value)
    return record


def _require_values(
    record: Mapping[str, object], expected: Mapping[str, int], phase: str
) -> None:
    for name, expected_value in expected.items():
        if record.get(name) != expected_value:
            raise BenchError(
                f"{phase} semantic mismatch for {name}: "
                f"expected {expected_value}, observed {record.get(name)}"
            )


def _validate_hot_record(record: Mapping[str, object]) -> None:
    expected = {
        name: 0 for name in BENCHMARK_FIELDS if name not in ("phase",)
    }
    expected.update(
        {
            "schema": 1,
            "policy": 1,
            "executions": 5_000_000,
            "checksum": 7_500_000,
            "positive_hits": 5_000_000,
            "archive_requested": 1,
            "archive_enabled": 1,
            "healthy": 1,
        }
    )
    expected.pop("duration_ns")
    _require_values(record, expected, "hot")
    if not isinstance(record.get("duration_ns"), int) or record["duration_ns"] <= 0:
        raise BenchError("hot duration_ns must be positive")


def _validate_churn_record(record: Mapping[str, object]) -> None:
    expected = {
        "schema": 1,
        "policy": 1,
        "executions": 300_000,
        "checksum": 600_000,
        "build_attempts": 300_000,
        "compile_attempts": 0,
        "positive_hits": 0,
        "evictions": 150_000,
        "eviction_pp": 150_000,
        "negative_hits": 0,
        "negative_evictions": 0,
        "deferrals": 150_000,
        "promotions": 150_000,
        "archive_probe": 150_000,
        "archive_hit": 150_000,
        "archive_miss": 0,
        "archive_put": 150_000,
        "archive_stored": 150_000,
        "archive_drop": 0,
        "archive_budget_drop": 0,
        "archive_invalid": 0,
        "archive_resident_before": 1,
        "archive_resident": 1,
        "archive_peak": 1,
        "archive_code_drop": 0,
        "archive_capacity_drop": 0,
        "archive_global_invalid": 0,
        "archive_requested": 1,
        "archive_enabled": 1,
        "healthy": 1,
    }
    _require_values(record, expected, "churn")
    if not isinstance(record.get("duration_ns"), int) or record["duration_ns"] <= 0:
        raise BenchError("churn duration_ns must be positive")
    code_before = int(record["archive_code_resident_before"])
    code_after = int(record["archive_code_resident"])
    code_peak = int(record["archive_code_peak"])
    code_stored = int(record["archive_code_stored"])
    code_hit = int(record["archive_code_hit"])
    capacity_before = int(record["archive_capacity_resident_before"])
    capacity_after = int(record["archive_capacity_resident"])
    capacity_peak = int(record["archive_capacity_peak"])
    capacity_stored = int(record["archive_capacity_stored"])
    capacity_hit = int(record["archive_capacity_hit"])
    global_before = int(record["archive_global_capacity_before"])
    global_after = int(record["archive_global_capacity"])
    global_peak = int(record["archive_global_peak"])
    if not (
        code_before > 0
        and code_after == code_before
        and code_peak >= code_after
        and code_stored == code_hit
        and code_stored > 0
    ):
        raise BenchError("churn archive code-byte conservation failed")
    if not (
        capacity_before > 0
        and capacity_after == capacity_before
        and capacity_peak >= capacity_after
        and capacity_stored == capacity_hit
        and capacity_stored > 0
    ):
        raise BenchError("churn archive capacity-byte conservation failed")
    if not (
        global_before == capacity_before
        and global_after == capacity_after
        and global_after <= global_peak <= 16 * 1024 * 1024
    ):
        raise BenchError("churn global archive budget conservation failed")


def _parse_benchmark_output(
    stdout: bytes, stderr: bytes
) -> Mapping[str, Mapping[str, object]]:
    if stderr:
        raise BenchError("benchmark emitted stderr bytes")
    lines = stdout.splitlines()
    if len(lines) != 2:
        raise BenchError(
            f"expected exactly two benchmark output lines, observed {len(lines)}"
        )
    records = [_parse_benchmark_line(line.rstrip(b"\r")) for line in lines]
    phases = [record["phase"] for record in records]
    if phases != ["hot", "churn"]:
        raise BenchError(f"benchmark phases are not exactly hot,churn: {phases}")
    _validate_hot_record(records[0])
    _validate_churn_record(records[1])
    return {str(record["phase"]): record for record in records}


def _file_description(path: pathlib.Path) -> str:
    completed = subprocess.run(
        ["/usr/bin/file", "-b", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
        check=True,
    )
    return completed.stdout.strip()


def _release_cache(binary: pathlib.Path) -> pathlib.Path:
    for directory in binary.parents:
        cache = directory / "CMakeCache.txt"
        if not cache.is_file():
            continue
        contents = cache.read_text(encoding="utf-8", errors="strict")
        if re.search(r"^CMAKE_BUILD_TYPE:STRING=Release$", contents, re.MULTILINE):
            return cache.resolve(strict=True)
        raise BenchError(f"nearest CMake cache is not Release: {cache}")
    raise BenchError("cannot prove a Release configuration from CMakeCache.txt")


def _host_is_native_arm64() -> Mapping[str, str]:
    machine = platform.machine()
    completed = subprocess.run(
        ["/usr/sbin/sysctl", "-in", "sysctl.proc_translated"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
        check=True,
    )
    translated = completed.stdout.strip()
    if machine != "arm64" or translated != "0":
        raise BenchError(
            f"host must be native arm64, observed machine={machine} "
            f"translated={translated}"
        )
    return {"machine": machine, "translated": translated}


def _process_executable(pid: int) -> pathlib.Path:
    if sys.platform != "darwin":
        raise BenchError("process identity requires macOS libproc")
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_pidpath.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    length = library.proc_pidpath(pid, buffer, len(buffer))
    if length <= 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return pathlib.Path(os.fsdecode(buffer.value)).resolve(strict=True)


def _parse_process_table(output: str) -> List[ProcessRow]:
    rows: List[ProcessRow] = []
    for line in output.splitlines():
        pieces = line.strip().split(None, 2)
        if len(pieces) != 3:
            continue
        try:
            pid = int(pieces[0])
            ppid = int(pieces[1])
        except ValueError:
            continue
        if pid > 0 and ppid >= 0:
            rows.append(ProcessRow(pid, ppid, pieces[2]))
    return rows


def _command_arguments(command: str) -> List[str]:
    try:
        return shlex.split(command)
    except ValueError:
        return []


def _is_conflict_executable(name: str) -> bool:
    lowered = name.lower()
    return (
        lowered in CONFLICT_EXECUTABLES
        or COMPILER_PATTERN.fullmatch(lowered) is not None
        or (lowered.startswith("lumen_cpu_") and lowered.endswith("_test"))
    )


def _classify_process_rows(
    rows: Sequence[ProcessRow],
    resolver: Callable[[int], pathlib.Path],
    excluded_pids: Sequence[int] = (),
) -> Tuple[List[Mapping[str, object]], List[Mapping[str, object]]]:
    excluded = set(excluded_pids)
    allowed: List[Mapping[str, object]] = []
    conflicts: List[Mapping[str, object]] = []
    for row in rows:
        if row.pid in excluded:
            continue
        arguments = _command_arguments(row.command)
        if not arguments:
            continue
        hinted_name = pathlib.Path(arguments[0]).name
        if not _is_conflict_executable(hinted_name):
            continue
        try:
            executable = resolver(row.pid)
        except OSError as error:
            if error.errno == errno.ESRCH:
                continue
            conflicts.append(
                {
                    "pid": row.pid,
                    "ppid": row.ppid,
                    "command": row.command,
                    "executable": None,
                    "identity_error": str(error),
                }
            )
            continue
        executable_text = str(executable)
        if not _is_conflict_executable(executable.name):
            continue
        record = {
            "pid": row.pid,
            "ppid": row.ppid,
            "command": row.command,
            "executable": executable_text,
        }
        exact_system_spindump = (
            executable_text == SYSTEM_SPINDUMP
            and row.ppid == 1
        )
        if exact_system_spindump:
            allowed.append(record)
        else:
            conflicts.append(record)
    return allowed, conflicts


def _scan_host_processes(
    excluded_pids: Sequence[int] = (),
) -> Mapping[str, object]:
    try:
        completed = subprocess.run(
            ["/bin/ps", "-axo", "pid=,ppid=,comm=", "-ww"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            check=True,
        )
        rows = _parse_process_table(completed.stdout)
        excluded = set(excluded_pids)
        excluded.add(os.getpid())
        allowed, conflicts = _classify_process_rows(
            rows, _process_executable, sorted(excluded)
        )
        return {
            "valid": not conflicts,
            "error": None,
            "allowed_system_spindump": allowed,
            "conflicts": conflicts,
        }
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        return {
            "valid": False,
            "error": str(error),
            "allowed_system_spindump": [],
            "conflicts": [],
        }


def _parse_ac_status(output: str) -> bool:
    first_line = output.splitlines()[0] if output.splitlines() else ""
    match = re.fullmatch(r"Now drawing from '([^']+)'", first_line)
    if match is None:
        raise BenchError("pmset did not emit a canonical power-source line")
    return match.group(1) == "AC Power"


def _power_snapshot() -> Mapping[str, object]:
    try:
        completed = subprocess.run(
            ["/usr/bin/pmset", "-g", "batt"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            check=True,
        )
        if completed.stderr or not completed.stdout.strip():
            raise BenchError("pmset batt capture is empty or emitted stderr")
        on_ac = _parse_ac_status(completed.stdout)
        return {
            "valid": on_ac,
            "on_ac": on_ac,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "error": None,
        }
    except (BenchError, OSError, subprocess.SubprocessError, UnicodeError) as error:
        return {
            "valid": False,
            "on_ac": False,
            "stdout": None,
            "stderr": None,
            "error": str(error),
        }


def _thermal_snapshot() -> Mapping[str, object]:
    try:
        completed = subprocess.run(
            ["/usr/bin/pmset", "-g", "therm"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            check=True,
        )
        if completed.stderr or not completed.stdout.strip():
            raise BenchError("pmset therm capture is empty or emitted stderr")
        return {
            "valid": True,
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "error": None,
        }
    except (BenchError, OSError, subprocess.SubprocessError, UnicodeError) as error:
        return {
            "valid": False,
            "stdout": None,
            "stderr": None,
            "error": str(error),
        }


def _host_snapshot(excluded_pids: Sequence[int] = ()) -> Mapping[str, object]:
    power = _power_snapshot()
    thermal = _thermal_snapshot()
    processes = _scan_host_processes(excluded_pids)
    return {
        "valid": bool(power["valid"] and thermal["valid"] and processes["valid"]),
        "captured_unix_ns": time.time_ns(),
        "power": power,
        "thermal": thermal,
        "processes": processes,
    }


def _record_key(record: Mapping[str, object]) -> str:
    return json.dumps(record, sort_keys=True, separators=(",", ":"))


def _cooldown() -> Mapping[str, object]:
    start_monotonic = time.monotonic()
    deadline = start_monotonic + COOLDOWN_SECONDS
    start_snapshot = _host_snapshot()
    ac_samples = 0
    ac_failures: List[Mapping[str, object]] = []
    process_errors: List[str] = []
    allowed_by_key: Dict[str, Mapping[str, object]] = {}
    conflicts_by_key: Dict[str, Mapping[str, object]] = {}

    while True:
        power = _power_snapshot()
        processes = _scan_host_processes()
        ac_samples += 1
        if not power["valid"]:
            ac_failures.append(
                {
                    "sample": ac_samples,
                    "captured_unix_ns": time.time_ns(),
                    "power": power,
                }
            )
        if processes["error"] is not None:
            process_errors.append(str(processes["error"]))
        for record in processes["allowed_system_spindump"]:  # type: ignore[union-attr]
            allowed_by_key[_record_key(record)] = record
        for record in processes["conflicts"]:  # type: ignore[union-attr]
            conflicts_by_key[_record_key(record)] = record
        now = time.monotonic()
        if now >= deadline:
            break
        time.sleep(min(COOLDOWN_POLL_SECONDS, deadline - now))

    end_snapshot = _host_snapshot()
    elapsed = time.monotonic() - start_monotonic
    valid = all(
        (
            elapsed >= COOLDOWN_SECONDS,
            bool(start_snapshot["valid"]),
            bool(end_snapshot["valid"]),
            not ac_failures,
            not process_errors,
            not conflicts_by_key,
        )
    )
    return {
        "valid": valid,
        "required_seconds": COOLDOWN_SECONDS,
        "poll_seconds": COOLDOWN_POLL_SECONDS,
        "elapsed_seconds": elapsed,
        "ac_samples": ac_samples,
        "ac_failures": ac_failures,
        "process_scan_errors": process_errors,
        "allowed_system_spindump": list(allowed_by_key.values()),
        "conflicts": list(conflicts_by_key.values()),
        "start_snapshot": start_snapshot,
        "end_snapshot": end_snapshot,
    }


def _monitor_process(
    process: subprocess.Popen[bytes], deadline: float
) -> Tuple[int, bool, Mapping[str, object]]:
    poll_count = 0
    allowed_by_key: Dict[str, Mapping[str, object]] = {}
    conflicts_by_key: Dict[str, Mapping[str, object]] = {}
    scan_errors: List[str] = []
    started_unix_ns = time.time_ns()
    started_monotonic = time.monotonic()
    timed_out = False
    while True:
        scan = _scan_host_processes((process.pid,))
        poll_count += 1
        if scan["error"] is not None and str(scan["error"]) not in scan_errors:
            scan_errors.append(str(scan["error"]))
        for record in scan["allowed_system_spindump"]:  # type: ignore[union-attr]
            allowed_by_key[_record_key(record)] = record
        for record in scan["conflicts"]:  # type: ignore[union-attr]
            conflicts_by_key[_record_key(record)] = record
        if process.poll() is not None:
            break
        now = time.monotonic()
        if now >= deadline:
            timed_out = True
            process.kill()
            break
        time.sleep(min(COOLDOWN_POLL_SECONDS, deadline - now))
    exit_code = process.wait(timeout=10)
    elapsed = time.monotonic() - started_monotonic
    monitor = {
        "valid": bool(poll_count >= 1 and not conflicts_by_key and not scan_errors),
        "guest_pid": process.pid,
        "poll_seconds": COOLDOWN_POLL_SECONDS,
        "poll_count": poll_count,
        "conflicts": list(conflicts_by_key.values()),
        "scan_errors": scan_errors,
        "allowed_system_spindump": list(allowed_by_key.values()),
        "timed_out": timed_out,
        "started_unix_ns": started_unix_ns,
        "finished_unix_ns": time.time_ns(),
        "elapsed_seconds": elapsed,
    }
    return exit_code, timed_out, monitor


def _matching_binary_processes(binary: pathlib.Path) -> List[int]:
    completed = subprocess.run(
        ["/bin/ps", "-axo", "pid=,ppid=,comm=", "-ww"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
        check=True,
    )
    matches: List[int] = []
    for row in _parse_process_table(completed.stdout):
        arguments = _command_arguments(row.command)
        if not arguments or pathlib.Path(arguments[0]).name != binary.name:
            continue
        try:
            executable = _process_executable(row.pid)
        except OSError as error:
            if error.errno == errno.ESRCH:
                continue
            raise
        if executable == binary:
            matches.append(row.pid)
    return sorted(matches)


def _artifact_hashes(run_root: pathlib.Path) -> Mapping[str, str]:
    return {
        name: _sha256_file(run_root / name)
        for name in ("preflight.json", "result.json", "stdout.log", "stderr.log")
    }


def _failed_checks(checks: Mapping[str, bool]) -> List[str]:
    return [name for name, passed in checks.items() if not passed]


def _run_one(
    spec: RunSpec,
    binary: pathlib.Path,
    runner: pathlib.Path,
    output_root: pathlib.Path,
    binary_description: str,
    release_cache: pathlib.Path,
    frozen_binary_sha256: str,
    frozen_runner_sha256: str,
    frozen_crash_snapshot: Mapping[str, object],
) -> RunOutcome:
    run_root = output_root / spec.label
    run_root.mkdir()
    cooldown = _cooldown()
    environment, removed_environment = _sterile_environment(spec.entries)
    binary_before, binary_before_error = _sha256_or_error(binary)
    runner_before, runner_before_error = _sha256_or_error(runner)
    crash_before = _crash_snapshot()
    crash_matches_matrix_before = crash_before == frozen_crash_snapshot
    matching_before = _matching_binary_processes(binary)
    environment_set = {
        ARCHIVE_VARIABLE: environment[ARCHIVE_VARIABLE],
        ADMISSION_VARIABLE: environment[ADMISSION_VARIABLE],
        ENTRIES_VARIABLE: environment[ENTRIES_VARIABLE],
    }
    preflight_valid = all(
        (
            bool(cooldown["valid"]),
            _environment_is_valid(environment, spec.entries),
            binary_before_error is None,
            runner_before_error is None,
            binary_before == frozen_binary_sha256,
            runner_before == frozen_runner_sha256,
            crash_matches_matrix_before,
            not matching_before,
        )
    )
    preflight = {
        "version": 1,
        "phase": "6AM",
        "kind": "compiled-archive-mechanical-guard",
        "run": spec.label,
        "arm": spec.arm,
        "selector_entries": spec.entries,
        "matrix_order": [label for label, _arm, _entries in MATRIX],
        "valid": preflight_valid,
        "binary": str(binary),
        "binary_file": binary_description,
        "binary_sha256": binary_before,
        "binary_sha256_error": binary_before_error,
        "frozen_binary_sha256": frozen_binary_sha256,
        "runner": str(runner),
        "runner_sha256": runner_before,
        "runner_sha256_error": runner_before_error,
        "frozen_runner_sha256": frozen_runner_sha256,
        "matrix_crash_snapshot": frozen_crash_snapshot,
        "crash_before": crash_before,
        "crash_matches_matrix_before": crash_matches_matrix_before,
        "release_cache": str(release_cache),
        "release_cache_sha256": _sha256_file(release_cache),
        "timeout_seconds": TIMEOUT_SECONDS,
        "cooldown": cooldown,
        "environment_remove_all_lumen": True,
        "environment_removed_prefixes": list(CONTAMINATING_ENVIRONMENT_PREFIXES),
        "environment_removed": removed_environment,
        "environment_set": environment_set,
        "environment_valid": _environment_is_valid(environment, spec.entries),
        "dynarec_override_present": DYNAREC_VARIABLE in environment,
        "matching_binary_processes_before": matching_before,
        "started_unix_ns": time.time_ns(),
    }
    _write_json(run_root / "preflight.json", preflight)

    stdout_path = run_root / "stdout.log"
    stderr_path = run_root / "stderr.log"
    launch_error: Optional[str] = None
    process_identity_error: Optional[str] = None
    process_identity_matches = False
    pid: Optional[int] = None
    exit_code: Optional[int] = None
    timed_out = False
    elapsed_seconds: Optional[float] = None
    launched = False
    host_monitor: Mapping[str, object] = {
        "valid": False,
        "guest_pid": None,
        "poll_seconds": COOLDOWN_POLL_SECONDS,
        "poll_count": 0,
        "conflicts": [],
        "scan_errors": ["process was not launched"],
        "allowed_system_spindump": [],
        "timed_out": False,
    }
    with stdout_path.open("xb") as stdout_log, stderr_path.open("xb") as stderr_log:
        if preflight_valid:
            process: Optional[subprocess.Popen[bytes]] = None
            started = time.monotonic()
            try:
                process = subprocess.Popen(
                    [str(binary), "--a64-dynarec-compiled-archive-benchmark"],
                    cwd=run_root,
                    env=environment,
                    stdin=subprocess.DEVNULL,
                    stdout=stdout_log,
                    stderr=stderr_log,
                )
                launched = True
                pid = process.pid
                try:
                    process_identity_matches = _process_executable(pid) == binary
                except (BenchError, OSError) as error:
                    process_identity_error = str(error)
                exit_code, timed_out, host_monitor = _monitor_process(
                    process, started + TIMEOUT_SECONDS
                )
            except (OSError, subprocess.SubprocessError) as error:
                launch_error = str(error)
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
            finally:
                elapsed_seconds = time.monotonic() - started

    post_host = _host_snapshot()
    binary_after, binary_after_error = _sha256_or_error(binary)
    runner_after, runner_after_error = _sha256_or_error(runner)
    crash_after = _crash_snapshot()
    crash_matches_matrix_after = crash_after == frozen_crash_snapshot
    try:
        matching_after = _matching_binary_processes(binary)
        residue_error: Optional[str] = None
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        matching_after = []
        residue_error = str(error)
    stdout = stdout_path.read_bytes()
    stderr = stderr_path.read_bytes()
    records: Mapping[str, Mapping[str, object]] = {}
    parser_error: Optional[str] = None
    try:
        records = _parse_benchmark_output(stdout, stderr)
    except BenchError as error:
        parser_error = str(error)
    unexpected_entries = sorted(
        path.name
        for path in run_root.iterdir()
        if path.name not in ("preflight.json", "stdout.log", "stderr.log")
    )
    checks = {
        "preflight_valid": preflight_valid,
        "launched": launched,
        "launch_error_absent": launch_error is None,
        "process_identity_error_absent": process_identity_error is None,
        "process_identity_matches": process_identity_matches,
        "timeout_absent": not timed_out,
        "exit_zero": exit_code == 0,
        "parser_valid": parser_error is None,
        "hot_present": "hot" in records,
        "churn_present": "churn" in records,
        "host_monitor_valid": bool(host_monitor["valid"]),
        "post_host_valid": bool(post_host["valid"]),
        "binary_unchanged": (
            binary_after_error is None and binary_after == frozen_binary_sha256
        ),
        "runner_unchanged": (
            runner_after_error is None and runner_after == frozen_runner_sha256
        ),
        "crash_roster_matches_matrix": crash_matches_matrix_after,
        "residue_scan_valid": residue_error is None,
        "no_binary_residues": not matching_after,
        "no_unexpected_files": not unexpected_entries,
    }
    valid = all(checks.values())
    durations_ns = {
        phase: int(records[phase]["duration_ns"])
        for phase in ("hot", "churn")
        if phase in records
    }
    result = {
        "version": 1,
        "phase": "6AM",
        "kind": "compiled-archive-mechanical-guard",
        "run": spec.label,
        "arm": spec.arm,
        "selector_entries": spec.entries,
        "valid": valid,
        "checks": checks,
        "failed_checks": _failed_checks(checks),
        "launched": launched,
        "pid": pid,
        "process_identity_matches": process_identity_matches,
        "process_identity_error": process_identity_error,
        "launch_error": launch_error,
        "timeout_seconds": TIMEOUT_SECONDS,
        "timed_out": timed_out,
        "elapsed_seconds": elapsed_seconds,
        "exit_code": exit_code,
        "host_monitor": host_monitor,
        "parser_error": parser_error,
        "benchmark_records": records,
        "durations_ns": durations_ns,
        "stdout_size": len(stdout),
        "stderr_size": len(stderr),
        "post_host": post_host,
        "binary_sha256_after": binary_after,
        "binary_sha256_after_error": binary_after_error,
        "runner_sha256_after": runner_after,
        "runner_sha256_after_error": runner_after_error,
        "crash_after": crash_after,
        "crash_matches_matrix_after": crash_matches_matrix_after,
        "matching_binary_processes_after": matching_after,
        "residue_scan_error": residue_error,
        "unexpected_run_entries": unexpected_entries,
        "finished_unix_ns": time.time_ns(),
    }
    _write_json(run_root / "result.json", result)
    artifact_hashes = _artifact_hashes(run_root)
    _write_json(
        run_root / "hashes.json",
        {
            "version": 1,
            "phase": "6AM",
            "kind": "compiled-archive-mechanical-guard",
            "run": spec.label,
            "sha256": artifact_hashes,
        },
    )
    return RunOutcome(spec, valid, durations_ns, artifact_hashes)


def _median_three(values: Sequence[int]) -> int:
    if len(values) != 3:
        raise BenchError(f"median requires exactly three values, got {len(values)}")
    return sorted(values)[1]


def _regression_percent(baseline: int, candidate: int) -> decimal.Decimal:
    if baseline <= 0 or candidate <= 0:
        raise BenchError("duration values must be positive")
    with decimal.localcontext() as context:
        context.prec = 50
        return (
            decimal.Decimal(candidate - baseline)
            * decimal.Decimal(100)
            / decimal.Decimal(baseline)
        )


def _evaluate_phase(
    phase: str, by_label: Mapping[str, RunOutcome]
) -> Mapping[str, object]:
    baseline_values = [by_label[f"B{index}"].durations_ns[phase] for index in (1, 2, 3)]
    candidate_values = [by_label[f"C{index}"].durations_ns[phase] for index in (1, 2, 3)]
    baseline_median = _median_three(baseline_values)
    candidate_median = _median_three(candidate_values)
    median_regression = _regression_percent(baseline_median, candidate_median)
    median_gate = candidate_median * 100 <= baseline_median * 102
    pairs: List[Mapping[str, object]] = []
    for index in (1, 2, 3):
        baseline = by_label[f"B{index}"].durations_ns[phase]
        candidate = by_label[f"C{index}"].durations_ns[phase]
        regression = _regression_percent(baseline, candidate)
        pair_gate = candidate * 100 <= baseline * 105
        pairs.append(
            {
                "pair": f"B{index}/C{index}",
                "baseline_duration_ns": baseline,
                "candidate_duration_ns": candidate,
                "regression_percent": _format_decimal(regression),
                "maximum_regression_percent": _format_decimal(
                    MAXIMUM_PAIR_REGRESSION_PERCENT
                ),
                "gate": pair_gate,
            }
        )
    pairs_gate = all(bool(pair["gate"]) for pair in pairs)
    return {
        "phase": phase,
        "baseline_values_ns": baseline_values,
        "candidate_values_ns": candidate_values,
        "baseline_median_ns": baseline_median,
        "candidate_median_ns": candidate_median,
        "median_regression_percent": _format_decimal(median_regression),
        "maximum_median_regression_percent": _format_decimal(
            MAXIMUM_MEDIAN_REGRESSION_PERCENT
        ),
        "median_gate": median_gate,
        "pairs": pairs,
        "pairs_gate": pairs_gate,
        "gate": median_gate and pairs_gate,
    }


def _evaluate_decision(
    outcomes: Sequence[RunOutcome],
    frozen_binary_sha256: str,
    frozen_runner_sha256: str,
    frozen_crash_digest: str = "self-test",
    final_crash_digest: str = "self-test",
) -> Mapping[str, object]:
    expected_specs = [RunSpec(*item) for item in MATRIX]
    observed_specs = [outcome.spec for outcome in outcomes]
    labels_unique = len({outcome.spec.label for outcome in outcomes}) == len(outcomes)
    by_label = {outcome.spec.label: outcome for outcome in outcomes}
    expected_labels = [spec.label for spec in expected_specs]
    functional_gates_valid = all(
        (
            observed_specs == expected_specs,
            labels_unique,
            all(label in by_label for label in expected_labels),
            all(outcome.valid for outcome in outcomes),
            all(
                set(outcome.durations_ns) == {"hot", "churn"}
                and all(value > 0 for value in outcome.durations_ns.values())
                for outcome in outcomes
            ),
            final_crash_digest == frozen_crash_digest,
        )
    )
    phase_decisions: Dict[str, Mapping[str, object]] = {}
    if functional_gates_valid:
        phase_decisions = {
            phase: _evaluate_phase(phase, by_label) for phase in ("hot", "churn")
        }
    if not functional_gates_valid:
        verdict = "INVALID"
    elif all(bool(value["gate"]) for value in phase_decisions.values()):
        verdict = "PASS"
    else:
        verdict = "REGRESSION"
    return {
        "version": 1,
        "phase": "6AM",
        "kind": "compiled-archive-mechanical-guard",
        "verdict": verdict,
        "matrix_order_expected": expected_labels,
        "matrix_order_observed": [spec.label for spec in observed_specs],
        "matrix_complete": observed_specs == expected_specs,
        "labels_unique": labels_unique,
        "functional_gates_valid": functional_gates_valid,
        "frozen_binary_sha256": frozen_binary_sha256,
        "frozen_runner_sha256": frozen_runner_sha256,
        "frozen_crash_digest": frozen_crash_digest,
        "final_crash_digest": final_crash_digest,
        "final_crash_freeze_valid": final_crash_digest == frozen_crash_digest,
        "runs": [
            {
                "run": outcome.spec.label,
                "arm": outcome.spec.arm,
                "selector_entries": outcome.spec.entries,
                "valid": outcome.valid,
                "durations_ns": dict(outcome.durations_ns),
                "artifact_sha256": dict(outcome.artifact_hashes),
            }
            for outcome in outcomes
        ],
        "phase_gates": phase_decisions,
        "finished_unix_ns": time.time_ns(),
    }


def _sample_records() -> Mapping[str, Mapping[str, object]]:
    hot: Dict[str, object] = {
        name: 0 for name in BENCHMARK_FIELDS if name != "phase"
    }
    hot.update(
        {
            "phase": "hot",
            "schema": 1,
            "policy": 1,
            "duration_ns": 100,
            "executions": 5_000_000,
            "checksum": 7_500_000,
            "positive_hits": 5_000_000,
            "archive_requested": 1,
            "archive_enabled": 1,
            "healthy": 1,
        }
    )
    churn: Dict[str, object] = {
        "phase": "churn",
        "schema": 1,
        "policy": 1,
        "duration_ns": 200,
        "executions": 300_000,
        "checksum": 600_000,
        "build_attempts": 300_000,
        "compile_attempts": 0,
        "positive_hits": 0,
        "evictions": 150_000,
        "eviction_pp": 150_000,
        "negative_hits": 0,
        "negative_evictions": 0,
        "deferrals": 150_000,
        "promotions": 150_000,
        "archive_probe": 150_000,
        "archive_hit": 150_000,
        "archive_miss": 0,
        "archive_put": 150_000,
        "archive_stored": 150_000,
        "archive_drop": 0,
        "archive_budget_drop": 0,
        "archive_invalid": 0,
        "archive_resident_before": 1,
        "archive_resident": 1,
        "archive_peak": 1,
        "archive_code_resident_before": 64,
        "archive_code_resident": 64,
        "archive_code_peak": 64,
        "archive_code_stored": 9_600_000,
        "archive_code_hit": 9_600_000,
        "archive_code_drop": 0,
        "archive_capacity_resident_before": 128,
        "archive_capacity_resident": 128,
        "archive_capacity_peak": 128,
        "archive_capacity_stored": 19_200_000,
        "archive_capacity_hit": 19_200_000,
        "archive_capacity_drop": 0,
        "archive_global_capacity_before": 128,
        "archive_global_capacity": 128,
        "archive_global_peak": 128,
        "archive_global_invalid": 0,
        "archive_requested": 1,
        "archive_enabled": 1,
        "healthy": 1,
    }
    return {"hot": hot, "churn": churn}


def _serialize_record(record: Mapping[str, object]) -> bytes:
    return BENCHMARK_PREFIX + " ".join(
        f"{name}={record[name]}" for name in BENCHMARK_FIELDS
    ).encode("ascii")


def _self_test_parser() -> None:
    records = _sample_records()
    output = b"\n".join(
        (_serialize_record(records["hot"]), _serialize_record(records["churn"]))
    ) + b"\n"
    parsed = _parse_benchmark_output(output, b"")
    if parsed["hot"]["duration_ns"] != 100 or parsed["churn"]["duration_ns"] != 200:
        raise BenchError("parser did not preserve both duration fields")

    invalid_outputs = [
        output + b"noise\n",
        _serialize_record(records["churn"]) + b"\n" + _serialize_record(records["hot"]) + b"\n",
        output.replace(b"policy=1", b"policy=0", 1),
        output.replace(b"duration_ns=100", b"duration_ns=0100", 1),
        output.replace(b"positive_hits=5000000", b"positive_hits=4999999", 1),
        output.replace(b" healthy=1\n", b" extra=1 healthy=1\n", 1),
    ]
    for invalid in invalid_outputs:
        try:
            _parse_benchmark_output(invalid, b"")
        except BenchError:
            continue
        raise BenchError("parser accepted malformed or semantically invalid output")
    try:
        _parse_benchmark_output(output, b"warning\n")
    except BenchError:
        pass
    else:
        raise BenchError("parser accepted stderr bytes")


def _outcomes_for_self_test(
    hot: Mapping[str, int],
    churn: Mapping[str, int],
    invalid_label: Optional[str] = None,
) -> List[RunOutcome]:
    return [
        RunOutcome(
            RunSpec(label, arm, entries),
            label != invalid_label,
            {"hot": hot[label], "churn": churn[label]},
            {},
        )
        for label, arm, entries in MATRIX
    ]


def _self_test_decision() -> None:
    baseline = {"B1": 100, "B2": 100, "B3": 100}
    inclusive = {**baseline, "C1": 102, "C2": 102, "C3": 102}
    decision = _evaluate_decision(
        _outcomes_for_self_test(inclusive, inclusive), "binary", "runner"
    )
    if decision["verdict"] != "PASS":
        raise BenchError("decision rejected the inclusive two-percent boundary")

    pair_boundary = {**baseline, "C1": 105, "C2": 100, "C3": 99}
    decision = _evaluate_decision(
        _outcomes_for_self_test(pair_boundary, pair_boundary), "binary", "runner"
    )
    if decision["verdict"] != "PASS":
        raise BenchError("decision rejected the inclusive five-percent pair boundary")

    improvement = {**baseline, "C1": 90, "C2": 90, "C3": 90}
    decision = _evaluate_decision(
        _outcomes_for_self_test(improvement, improvement), "binary", "runner"
    )
    if decision["verdict"] != "PASS":
        raise BenchError("decision rejected candidate improvements")

    median_failure = {**baseline, "C1": 103, "C2": 103, "C3": 103}
    decision = _evaluate_decision(
        _outcomes_for_self_test(median_failure, inclusive), "binary", "runner"
    )
    if decision["verdict"] != "REGRESSION":
        raise BenchError("decision accepted a median regression over two percent")

    pair_failure = {**baseline, "C1": 106, "C2": 99, "C3": 99}
    decision = _evaluate_decision(
        _outcomes_for_self_test(pair_failure, inclusive), "binary", "runner"
    )
    if decision["verdict"] != "REGRESSION":
        raise BenchError("decision accepted a pair regression over five percent")

    decision = _evaluate_decision(
        _outcomes_for_self_test(inclusive, inclusive, "C2"), "binary", "runner"
    )
    if decision["verdict"] != "INVALID":
        raise BenchError("decision accepted an invalid functional run")
    decision = _evaluate_decision(
        _outcomes_for_self_test(inclusive, inclusive)[:-1], "binary", "runner"
    )
    if decision["verdict"] != "INVALID":
        raise BenchError("decision accepted an incomplete fixed matrix")
    decision = _evaluate_decision(
        _outcomes_for_self_test(inclusive, inclusive),
        "binary",
        "runner",
        "crash-before",
        "crash-after",
    )
    if decision["verdict"] != "INVALID":
        raise BenchError("decision accepted matrix-level crash-roster drift")


def _self_test_environment() -> None:
    snapshots = dict(os.environ)
    try:
        os.environ[ARCHIVE_VARIABLE] = "0"
        os.environ[ADMISSION_VARIABLE] = "0"
        os.environ[ENTRIES_VARIABLE] = "garbage"
        os.environ[DYNAREC_VARIABLE] = "0"
        os.environ["LUMEN_UNRELATED_DIAGNOSTIC"] = "1"
        os.environ["ASAN_OPTIONS"] = "detect_leaks=1"
        os.environ["DYLD_LIBRARY_PATH"] = "/tmp/injected"
        os.environ["LLVM_PROFILE_FILE"] = "/tmp/profile"
        os.environ["ASAN_CUSTOM_SETTING"] = "1"
        os.environ["LSAN_CUSTOM_SETTING"] = "1"
        os.environ["TSAN_CUSTOM_SETTING"] = "1"
        os.environ["UBSAN_CUSTOM_SETTING"] = "1"
        os.environ["VK_CUSTOM_LAYER"] = "1"
        os.environ["MVK_CUSTOM_CONFIG"] = "1"
        os.environ["MTL_CUSTOM_CAPTURE"] = "1"
        os.environ["METAL_CUSTOM_DEVICE"] = "1"
        os.environ["SDL_CUSTOM_DRIVER"] = "1"
        os.environ["OS_ACTIVITY_CUSTOM_MODE"] = "1"
        os.environ["OS_SIGNPOST_CUSTOM_MODE"] = "1"
        os.environ["MallocNanoZone"] = "1"
        os.environ["NSZombieCustomSetting"] = "1"
        os.environ["NSAutoreleaseFreedObjectCheck"] = "YES"
        environment, removed = _sterile_environment(1024)
    finally:
        os.environ.clear()
        os.environ.update(snapshots)
    if not _environment_is_valid(environment, 1024):
        raise BenchError("sterile environment retained or changed a LUMEN variable")
    contaminated = dict(environment)
    contaminated["VK_LATE_INJECTION"] = "1"
    if _environment_is_valid(contaminated, 1024):
        raise BenchError("environment validation accepted a broad-prefix injection")
    required_removed = {
        ARCHIVE_VARIABLE,
        ADMISSION_VARIABLE,
        ENTRIES_VARIABLE,
        DYNAREC_VARIABLE,
        "LUMEN_UNRELATED_DIAGNOSTIC",
        "ASAN_OPTIONS",
        "DYLD_LIBRARY_PATH",
        "LLVM_PROFILE_FILE",
        "ASAN_CUSTOM_SETTING",
        "LSAN_CUSTOM_SETTING",
        "TSAN_CUSTOM_SETTING",
        "UBSAN_CUSTOM_SETTING",
        "VK_CUSTOM_LAYER",
        "MVK_CUSTOM_CONFIG",
        "MTL_CUSTOM_CAPTURE",
        "METAL_CUSTOM_DEVICE",
        "SDL_CUSTOM_DRIVER",
        "OS_ACTIVITY_CUSTOM_MODE",
        "OS_SIGNPOST_CUSTOM_MODE",
        "MallocNanoZone",
        "NSZombieCustomSetting",
        "NSAutoreleaseFreedObjectCheck",
    }
    if not required_removed.issubset(removed):
        raise BenchError("sterile environment removal roster is incomplete")


def _self_test_host_oracles() -> None:
    if not _parse_ac_status("Now drawing from 'AC Power'\n"):
        raise BenchError("AC parser rejected AC Power")
    if _parse_ac_status("Now drawing from 'Battery Power'\n"):
        raise BenchError("AC parser accepted Battery Power")
    try:
        _parse_ac_status("unknown\n")
    except BenchError:
        pass
    else:
        raise BenchError("AC parser accepted malformed pmset output")

    ancestor_rows = _parse_process_table(
        "1 0 /sbin/launchd\n"
        "10 1 /opt/homebrew/bin/cmake --build build\n"
        "20 10 /usr/bin/python3 phase6am_bench.py --self-test\n"
        "30 20 /tmp/lumen_cpu_x64_test --a64-dynarec-compiled-archive-benchmark\n"
    )
    ancestor_paths = {10: pathlib.Path("/opt/homebrew/bin/cmake")}
    _ancestor_allowed, ancestor_conflicts = _classify_process_rows(
        ancestor_rows, ancestor_paths.__getitem__, (20, 30)
    )
    if [record["pid"] for record in ancestor_conflicts] != [10]:
        raise BenchError("host guard improperly exempted a conflicting ancestor")

    rows = _parse_process_table(
        "101 1 /usr/sbin/spindump\n"
        "102 2 /usr/sbin/spindump -i 10\n"
        "103 50 /opt/homebrew/bin/ninja -C build\n"
        "104 50 /tmp/fc_script game.lua\n"
        "105 1 /usr/libexec/unrelated-daemon\n"
        "106 1 /usr/sbin/spindump --system-daemon-option\n"
        "107 50 /usr/bin/make -j4\n"
        "108 50 /usr/bin/c++ input.cpp\n"
        "109 50 /usr/bin/swift-frontend input.swift\n"
        "110 50 /usr/bin/metallib input.air\n"
        "111 50 /usr/bin/debugserver localhost:1234\n"
        "112 50 /usr/bin/ld-prime input.o\n"
        "113 50 /opt/toolchain/bin/clang-18 input.cpp\n"
        "114 50 /tmp/lumen_cpu_a64_dynarec_test --host-supported\n"
        "115 50 /opt/homebrew/bin/ctest --test-dir build\n"
        "116 50 /usr/bin/powermetrics --samplers cpu_power\n"
        "117 50 /usr/bin/lldb-rpc-server --fd 3\n"
        "118 50 /tmp/lumenemu game\n"
    )
    paths = {
        101: pathlib.Path(SYSTEM_SPINDUMP),
        102: pathlib.Path(SYSTEM_SPINDUMP),
        103: pathlib.Path("/opt/homebrew/bin/ninja"),
        104: pathlib.Path("/tmp/fc_script"),
        105: pathlib.Path("/usr/libexec/unrelated-daemon"),
        106: pathlib.Path(SYSTEM_SPINDUMP),
        107: pathlib.Path("/usr/bin/make"),
        108: pathlib.Path("/usr/bin/c++"),
        109: pathlib.Path("/usr/bin/swift-frontend"),
        110: pathlib.Path("/usr/bin/metallib"),
        111: pathlib.Path("/usr/bin/debugserver"),
        112: pathlib.Path("/usr/bin/ld-prime"),
        113: pathlib.Path("/opt/toolchain/bin/clang-18"),
        114: pathlib.Path("/tmp/lumen_cpu_a64_dynarec_test"),
        115: pathlib.Path("/opt/homebrew/bin/ctest"),
        116: pathlib.Path("/usr/bin/powermetrics"),
        117: pathlib.Path("/usr/bin/lldb-rpc-server"),
        118: pathlib.Path("/tmp/lumenemu"),
    }
    allowed, conflicts = _classify_process_rows(rows, paths.__getitem__)
    if [record["pid"] for record in allowed] != [101, 106]:
        raise BenchError("host guard did not isolate exact PPID-1 system spindump")
    if [record["pid"] for record in conflicts] != [
        102,
        103,
        104,
        107,
        108,
        109,
        110,
        111,
        112,
        113,
        114,
        115,
        116,
        117,
        118,
    ]:
        raise BenchError("host guard conflict classification mismatch")


def _validate_live_inputs(
    arguments: argparse.Namespace,
) -> Tuple[pathlib.Path, pathlib.Path, pathlib.Path, str, pathlib.Path]:
    if sys.platform != "darwin":
        raise BenchError("phase6AM mechanical guard is restricted to macOS")
    _host_is_native_arm64()
    binary = pathlib.Path(arguments.binary).resolve(strict=True)
    runner = pathlib.Path(__file__).resolve(strict=True)
    output_root = pathlib.Path(arguments.output).resolve()
    if os.path.lexists(os.fspath(output_root)):
        raise BenchError(f"output root already exists: {output_root}")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise BenchError(f"binary is not executable: {binary}")
    if binary.name != "lumen_cpu_x64_test":
        raise BenchError(f"unexpected benchmark binary name: {binary.name}")
    description = _file_description(binary)
    if (
        "Mach-O 64-bit executable arm64" not in description
        or "universal" in description.lower()
    ):
        raise BenchError(f"binary is not thin arm64: {description}")
    release_cache = _release_cache(binary)
    if not _contains_bytes(binary, BENCHMARK_PREFIX.rstrip()):
        raise BenchError("binary does not contain the compiled-archive benchmark marker")
    if not _contains_bytes(binary, ENTRIES_VARIABLE.encode("ascii")):
        raise BenchError("binary does not contain the archive-entry selector")
    return binary, runner, output_root, description, release_cache


def run_matrix(arguments: argparse.Namespace) -> int:
    binary, runner, output_root, description, release_cache = _validate_live_inputs(
        arguments
    )
    frozen_binary_sha256 = _sha256_file(binary)
    frozen_runner_sha256 = _sha256_file(runner)
    frozen_crash_snapshot = _crash_snapshot()
    output_root.mkdir(parents=True)
    outcomes: List[RunOutcome] = []
    for label, arm, entries in MATRIX:
        outcome = _run_one(
            RunSpec(label, arm, entries),
            binary,
            runner,
            output_root,
            description,
            release_cache,
            frozen_binary_sha256,
            frozen_runner_sha256,
            frozen_crash_snapshot,
        )
        outcomes.append(outcome)
        hot = outcome.durations_ns.get("hot")
        churn = outcome.durations_ns.get("churn")
        print(
            "phase6am-bench-run: "
            f"{'PASS' if outcome.valid else 'INVALID'} run={label} "
            f"entries={entries} hot_ns={hot} churn_ns={churn}"
        )
    final_crash_snapshot = _crash_snapshot()
    decision = _evaluate_decision(
        outcomes,
        frozen_binary_sha256,
        frozen_runner_sha256,
        str(frozen_crash_snapshot["digest"]),
        str(final_crash_snapshot["digest"]),
    )
    decision_path = output_root / "decision.json"
    _write_json(decision_path, decision)
    decision_sha256 = _sha256_file(decision_path)
    _write_text(
        output_root / "decision.sha256", f"{decision_sha256}  decision.json\n"
    )
    print(
        "phase6am-bench: "
        f"{decision['verdict']} decision_sha256={decision_sha256}"
    )
    return 0 if decision["verdict"] == "PASS" else 1


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "run the fixed B1,C1,C2,B2,B3,C3 K512/K1024 mechanical "
            "compiled-archive guard"
        )
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run parser, environment, host-control, and decision oracles",
    )
    parser.add_argument("--binary", help="Release thin-arm64 lumen_cpu_x64_test")
    parser.add_argument("--output", help="new output root for the complete matrix")
    return parser


def main(argv: Sequence[str]) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        if arguments.binary is not None or arguments.output is not None:
            parser.error("--self-test does not accept live-run arguments")
        try:
            _self_test_parser()
            _self_test_environment()
            _self_test_host_oracles()
            _self_test_decision()
        except (BenchError, OSError, UnicodeError, subprocess.SubprocessError) as error:
            print(f"phase6am-bench-self-test: FAIL: {error}", file=sys.stderr)
            return 1
        print("phase6am-bench-self-test: PASS")
        return 0
    required = {"--binary": arguments.binary, "--output": arguments.output}
    missing = [name for name, value in required.items() if value is None]
    if missing:
        parser.error("the following arguments are required: " + ", ".join(missing))
    try:
        return run_matrix(arguments)
    except (BenchError, OSError, UnicodeError, subprocess.SubprocessError) as error:
        print(f"phase6am-bench: INVALID: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
