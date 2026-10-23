#!/usr/bin/env python3
"""Run the preregistered phase-6AO raw/verified-IR live A/B matrix on macOS."""

from __future__ import annotations

import argparse
import ctypes
import dataclasses
import decimal
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from typing import Callable, Dict, List, Mapping, Optional, Sequence, Tuple


PRESENT_CUTOFF = 120
TIMEOUT_SECONDS = 600
EXIT_TIMEOUT_SECONDS = 60
COOLDOWN_SECONDS = 10
HOST_POLL_INTERVAL_MS = 100
MINIMUM_MEDIAN_REDUCTION_PERCENT = decimal.Decimal("5")
FRAME_TRACE_VARIABLE = "LUMEN_FRAME_PIPELINE_TRACE"
SELECTOR_VARIABLE = "LUMEN_X64_A64_DYNAREC_VERIFIED_IR_PIPELINE"
DYNAREC_VARIABLE = "LUMEN_X64_A64_DYNAREC"
ARCHIVE_VARIABLE = "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE"
ARCHIVE_ENTRIES_VARIABLE = "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES"
ADMISSION_VARIABLE = "LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION"
PRODUCTION_DEFAULT_ABSENT_VARIABLES = (
    DYNAREC_VARIABLE,
    ARCHIVE_VARIABLE,
    ARCHIVE_ENTRIES_VARIABLE,
    ADMISSION_VARIABLE,
)
SDL_QUIT_MARKER = b"[WINDOW_EXIT] SDL_QUIT"

MATRIX: Tuple[Tuple[str, str, int], ...] = (
    ("B1", "B", 0),
    ("C1", "C", 1),
    ("C2", "C", 1),
    ("B2", "B", 0),
    ("B3", "B", 0),
    ("C3", "C", 1),
)

# These non-Lumen variables can inject profilers, sanitizers, validation layers,
# or interposers. They are removed in addition to every inherited LUMEN_* value.
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
    "MSAN_",
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
    "ASAN_",
    "LSAN_",
    "TSAN_",
    "UBSAN_",
)
COMPILER_EXECUTABLE_PATTERN = re.compile(
    r"(?:clang(?:\+\+)?|gcc|g\+\+)(?:-[0-9]+(?:\.[0-9]+)*)?\Z"
)
CONFLICT_EXECUTABLE_NAMES = frozenset(
    (
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
        "ld-classic",
        "ld-prime",
        "ld.gold",
        "ld.lld",
        "ld64",
        "ld64.lld",
        "leaks",
        "libtool",
        "link",
        "lld",
        "lld-link",
        "lldb",
        "lldb-rpc-server",
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
    )
)
SYSTEM_SPINDUMP_PATH = pathlib.Path("/usr/sbin/spindump")

PRESENT_LINE_PATTERN = re.compile(
    rb"^\[FRAME_PIPELINE\] present=120(?:[ \t]|$)"
)
PRESENT_METRIC_PATTERN = re.compile(
    rb"^\[FRAME_PIPELINE\] present=120[ \t]+"
    rb"avg_interval=([0-9]+(?:\.[0-9]+)?)[ \t]+ms(?:[ \t].*)?$"
)
ERROR_PATTERN = re.compile(
    rb"fatal|assert|memory fault|device[._ ]lost|segmentation fault",
    re.IGNORECASE,
)


class ABError(RuntimeError):
    """A fail-closed phase-6AO runner or validation error."""


@dataclasses.dataclass(frozen=True)
class RunSpec:
    label: str
    arm: str
    selector: int


@dataclasses.dataclass(frozen=True)
class RunOutcome:
    spec: RunSpec
    valid: bool
    avg_interval_ms: Optional[decimal.Decimal]
    artifact_hashes: Mapping[str, str]


@dataclasses.dataclass(frozen=True)
class ProcessRow:
    pid: int
    ppid: int
    reported_executable: str


@dataclasses.dataclass(frozen=True)
class HostScan:
    conflicts: Sequence[Mapping[str, object]]
    permitted_system_processes: Sequence[Mapping[str, object]]


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _tree_snapshot(root: pathlib.Path, content_hashes: bool) -> Mapping[str, object]:
    records: List[Mapping[str, object]] = []
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            record: Mapping[str, object] = {
                "path": relative,
                "kind": "symlink",
                "target": os.readlink(path),
            }
        elif path.is_dir():
            record = {"path": relative, "kind": "directory"}
        elif path.is_file():
            stat = path.stat()
            mutable: Dict[str, object] = {
                "path": relative,
                "kind": "file",
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
            }
            if content_hashes:
                mutable["sha256"] = _sha256_file(path)
            record = mutable
        else:
            record = {"path": relative, "kind": "other"}
        encoded = json.dumps(record, sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
        digest.update(encoded)
        digest.update(b"\n")
        records.append(record)
    return {"digest": digest.hexdigest(), "records": records}


def _matrix_freeze(
    binary: pathlib.Path,
    lua: pathlib.Path,
    runner: pathlib.Path,
    save_template: pathlib.Path,
    rom_root: pathlib.Path,
) -> Mapping[str, str]:
    shader_library = (
        rom_root / "Orbis" / "DataPS4" / "Bundles" / "ShaderProgLibrary.xom"
    )
    return {
        "binary_sha256": _sha256_file(binary),
        "lua_sha256": _sha256_file(lua),
        "runner_sha256": _sha256_file(runner),
        "save_template_digest": str(
            _tree_snapshot(save_template, True)["digest"]
        ),
        "rom_tree_digest": str(_tree_snapshot(rom_root, False)["digest"]),
        "eboot_sha256": _sha256_file(rom_root / "eboot.bin"),
        "shader_library_sha256": _sha256_file(shader_library),
        "crash_roster_digest": _canonical_json_digest(_crash_snapshot()),
    }


def _freeze_matches(
    expected: Mapping[str, str], observed: Mapping[str, str]
) -> Mapping[str, bool]:
    return {
        key: key in observed and observed[key] == value
        for key, value in expected.items()
    }


def _canonical_json_digest(value: object) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )
    return hashlib.sha256(payload).hexdigest()


def _crash_snapshot() -> List[Mapping[str, object]]:
    crash_root = pathlib.Path.home() / "Library" / "Logs" / "DiagnosticReports"
    result: List[Mapping[str, object]] = []
    if not crash_root.is_dir():
        return result
    paths = set(crash_root.glob("fc_script*.ips"))
    paths.update(crash_root.glob("fc_script*.crash"))
    for path in sorted(paths, key=lambda item: item.as_posix()):
        if not path.is_file():
            continue
        stat = path.stat()
        result.append(
            {
                "path": str(path),
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
            }
        )
    return result


def _process_executable(pid: int) -> pathlib.Path:
    if sys.platform != "darwin":
        raise ABError("phase6AO process identity requires macOS libproc")
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_pidpath.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    length = library.proc_pidpath(pid, buffer, len(buffer))
    if length <= 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return pathlib.Path(os.fsdecode(buffer.value)).resolve(strict=True)


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _parse_process_rows(text: str) -> List[ProcessRow]:
    rows: List[ProcessRow] = []
    for line in text.splitlines():
        pieces = line.strip().split(None, 2)
        if len(pieces) != 3:
            continue
        try:
            pid = int(pieces[0])
            ppid = int(pieces[1])
        except ValueError:
            continue
        rows.append(ProcessRow(pid, ppid, pieces[2]))
    return rows


def _process_rows() -> List[ProcessRow]:
    completed = subprocess.run(
        ["/bin/ps", "-axo", "pid=,ppid=,comm=", "-ww"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
        check=True,
    )
    return _parse_process_rows(completed.stdout)


def _is_conflict_executable_name(name: str) -> bool:
    lowered = name.lower()
    return bool(
        lowered in CONFLICT_EXECUTABLE_NAMES
        or COMPILER_EXECUTABLE_PATTERN.fullmatch(lowered) is not None
        or (lowered.startswith("lumen_cpu_") and lowered.endswith("_test"))
    )


def _host_scan_from_rows(
    rows: Sequence[ProcessRow],
    guest_pid: Optional[int],
    runner_pid: int,
    resolver: Callable[[int], pathlib.Path],
) -> HostScan:
    if not any(row.pid == runner_pid for row in rows):
        raise ABError(f"host process roster does not contain runner PID {runner_pid}")
    excluded = {runner_pid}
    if guest_pid is not None:
        excluded.add(guest_pid)
    conflicts: List[Mapping[str, object]] = []
    permitted: List[Mapping[str, object]] = []
    for row in rows:
        if row.pid in excluded:
            continue
        reported_name = pathlib.Path(row.reported_executable).name
        if not _is_conflict_executable_name(reported_name):
            continue
        try:
            executable = resolver(row.pid)
        except (ABError, OSError) as error:
            conflicts.append(
                {
                    "pid": row.pid,
                    "ppid": row.ppid,
                    "reported_executable": row.reported_executable,
                    "executable": None,
                    "name": reported_name,
                    "identity_error": str(error),
                }
            )
            continue
        name = executable.name
        if not _is_conflict_executable_name(name):
            continue
        record: Mapping[str, object] = {
            "pid": row.pid,
            "ppid": row.ppid,
            "reported_executable": row.reported_executable,
            "executable": str(executable),
            "name": name,
            "identity_error": None,
        }
        if (
            name.lower() == "spindump"
            and row.ppid == 1
            and executable == SYSTEM_SPINDUMP_PATH
        ):
            permitted.append(record)
        else:
            conflicts.append(record)
    return HostScan(conflicts, permitted)


def _scan_host_conflicts(guest_pid: Optional[int] = None) -> HostScan:
    return _host_scan_from_rows(
        _process_rows(), guest_pid, os.getpid(), _process_executable
    )


def _host_scan_payload(scan: HostScan) -> Mapping[str, object]:
    return {
        "conflict_count": len(scan.conflicts),
        "conflicts": list(scan.conflicts),
        "permitted_system_processes": list(scan.permitted_system_processes),
    }


def _matching_processes(binary: pathlib.Path, lua: pathlib.Path) -> List[int]:
    completed = subprocess.run(
        ["/bin/ps", "-axo", "pid=,ppid=,ucomm=,command=", "-ww"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
        check=True,
    )
    rows: List[Tuple[int, int, str]] = []
    parents: Dict[int, int] = {}
    for line in completed.stdout.splitlines():
        pieces = line.strip().split(None, 3)
        if len(pieces) != 4:
            continue
        try:
            pid = int(pieces[0])
            ppid = int(pieces[1])
        except ValueError:
            continue
        rows.append((pid, ppid, pieces[3]))
        parents[pid] = ppid

    excluded = {os.getpid()}
    ancestor = parents.get(os.getpid())
    while ancestor is not None and ancestor > 0 and ancestor not in excluded:
        excluded.add(ancestor)
        ancestor = parents.get(ancestor)

    binary = binary.resolve()
    lua = lua.resolve()
    matches: List[int] = []
    for pid, _ppid, command in rows:
        if pid in excluded:
            continue
        try:
            arguments = shlex.split(command)
        except ValueError:
            continue
        if len(arguments) < 2:
            continue
        try:
            lua_argument = pathlib.Path(arguments[1]).resolve(strict=True)
        except OSError:
            continue
        if lua_argument != lua:
            continue
        try:
            executable = _process_executable(pid)
        except OSError:
            # A matching command can disappear between ps and proc_pidpath.
            continue
        if executable == binary:
            matches.append(pid)
    return sorted(matches)


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


def _write_json(path: pathlib.Path, value: object) -> None:
    with path.open("x", encoding="utf-8") as output:
        json.dump(value, output, indent=2, sort_keys=True)
        output.write("\n")


def _format_decimal(value: decimal.Decimal) -> str:
    return format(value, "f")


def _parse_metric_line(line: bytes) -> decimal.Decimal:
    match = PRESENT_METRIC_PATTERN.fullmatch(line.rstrip(b"\r"))
    if match is None:
        raise ABError("present=120 does not contain a canonical avg_interval metric")
    try:
        value = decimal.Decimal(match.group(1).decode("ascii"))
    except (decimal.InvalidOperation, UnicodeError) as error:
        raise ABError("present=120 avg_interval is not numeric") from error
    if not value.is_finite() or value <= 0:
        raise ABError("present=120 avg_interval must be finite and positive")
    return value


def _parse_unique_present_metric(log: bytes) -> decimal.Decimal:
    present_lines = [
        line
        for line in log.splitlines()
        if PRESENT_LINE_PATTERN.match(line.rstrip(b"\r")) is not None
    ]
    if len(present_lines) != 1:
        raise ABError(
            "expected exactly one FRAME_PIPELINE present=120 line, observed "
            f"{len(present_lines)}"
        )
    return _parse_metric_line(present_lines[0])


def _monitor_cutoff(
    process: subprocess.Popen[bytes],
    stderr_path: pathlib.Path,
    timeout_seconds: int,
    host_monitor: Optional[Dict[str, object]] = None,
    scanner: Optional[Callable[[Optional[int]], HostScan]] = None,
) -> Tuple[bool, Optional[decimal.Decimal], Optional[str], Optional[str], bool]:
    deadline = time.monotonic() + timeout_seconds
    pending = b""
    with stderr_path.open("rb") as source:
        while time.monotonic() < deadline:
            if host_monitor is not None and not _poll_host_monitor(
                host_monitor, process.pid, scanner
            ):
                return False, None, None, "host conflict monitor veto", False
            chunk = source.read()
            if chunk:
                pending += chunk
                lines = pending.split(b"\n")
                pending = lines.pop()
                for line in lines:
                    stripped = line.rstrip(b"\r")
                    if PRESENT_LINE_PATTERN.match(stripped) is None:
                        continue
                    try:
                        return True, _parse_metric_line(stripped), None, None, False
                    except ABError as error:
                        return True, None, str(error), None, False
            if process.poll() is not None:
                break
            time.sleep(HOST_POLL_INTERVAL_MS / 1000.0)
    timed_out = process.poll() is None and time.monotonic() >= deadline
    return False, None, None, None, timed_out


def _outside_save_entries(work_root: pathlib.Path) -> List[str]:
    save_root = work_root / "_SaveData"
    result: List[str] = []
    for path in sorted(work_root.rglob("*"), key=lambda item: item.as_posix()):
        if path == save_root:
            continue
        try:
            path.relative_to(save_root)
        except ValueError:
            result.append(path.relative_to(work_root).as_posix())
    return result


def _sterile_environment(
    selector: int, inherited: Optional[Mapping[str, str]] = None
) -> Tuple[Mapping[str, str], List[str]]:
    if type(selector) is not int or selector not in (0, 1):
        raise ABError("phase6AO selector must be the integer 0 or 1")
    environment = dict(os.environ if inherited is None else inherited)
    removed = {
        name
        for name in environment
        if any(
            name.startswith(prefix)
            for prefix in CONTAMINATING_ENVIRONMENT_PREFIXES
        )
    }
    removed.update(
        name for name in EXTERNAL_DIAGNOSTIC_ENVIRONMENT if name in environment
    )
    for name in removed:
        environment.pop(name, None)
    environment[FRAME_TRACE_VARIABLE] = "1"
    environment[SELECTOR_VARIABLE] = str(selector)
    return environment, sorted(removed)


def _is_within(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _append_unique_records(
    destination: List[Mapping[str, object]],
    records: Sequence[Mapping[str, object]],
) -> None:
    for record in records:
        if record not in destination:
            destination.append(record)


def _guarded_cooldown(
    seconds: int,
    scanner: Optional[Callable[[Optional[int]], HostScan]] = None,
) -> Mapping[str, object]:
    if seconds < 0:
        raise ABError("cooldown duration must not be negative")
    scan = _scan_host_conflicts if scanner is None else scanner
    start_utc = _utc_now()
    start_monotonic_ns = time.monotonic_ns()
    deadline_ns = start_monotonic_ns + seconds * 1_000_000_000
    poll_count = 0
    conflicts: List[Mapping[str, object]] = []
    permitted: List[Mapping[str, object]] = []
    scan_errors: List[str] = []
    while True:
        try:
            observed = scan(None)
            poll_count += 1
            _append_unique_records(conflicts, observed.conflicts)
            _append_unique_records(permitted, observed.permitted_system_processes)
        except (ABError, OSError, subprocess.SubprocessError) as error:
            poll_count += 1
            scan_errors.append(str(error))
        if conflicts or scan_errors:
            break
        remaining_ns = deadline_ns - time.monotonic_ns()
        if remaining_ns <= 0:
            break
        time.sleep(
            min(HOST_POLL_INTERVAL_MS / 1000.0, remaining_ns / 1_000_000_000)
        )
    end_monotonic_ns = time.monotonic_ns()
    veto = ""
    if conflicts:
        veto = "host conflicts: " + json.dumps(conflicts, sort_keys=True)
    elif scan_errors:
        veto = "host scan error: " + "; ".join(scan_errors)
    valid = (
        not conflicts
        and not scan_errors
        and poll_count >= 1
        and end_monotonic_ns - start_monotonic_ns >= seconds * 1_000_000_000
    )
    return {
        "schema": 1,
        "required_seconds": seconds,
        "poll_interval_ms": HOST_POLL_INTERVAL_MS,
        "poll_count": poll_count,
        "conflict_count": len(conflicts),
        "conflicts": conflicts,
        "permitted_system_processes": permitted,
        "scan_errors": scan_errors,
        "veto": veto,
        "valid": valid,
        "start_utc": start_utc,
        "start_monotonic_ns": start_monotonic_ns,
        "end_utc": _utc_now(),
        "end_monotonic_ns": end_monotonic_ns,
    }


def _new_host_monitor(guest_pid: int) -> Dict[str, object]:
    return {
        "schema": 1,
        "guest_pid": guest_pid,
        "poll_interval_ms": HOST_POLL_INTERVAL_MS,
        "poll_count": 0,
        "conflict_poll_count": 0,
        "conflicts": [],
        "permitted_system_processes": [],
        "scan_errors": [],
        "start_utc": _utc_now(),
        "start_monotonic_ns": time.monotonic_ns(),
    }


def _poll_host_monitor(
    monitor: Dict[str, object],
    guest_pid: Optional[int],
    scanner: Optional[Callable[[Optional[int]], HostScan]] = None,
) -> bool:
    scan = _scan_host_conflicts if scanner is None else scanner
    monitor["poll_count"] = int(monitor["poll_count"]) + 1
    try:
        observed = scan(guest_pid)
    except (ABError, OSError, subprocess.SubprocessError) as error:
        errors = monitor["scan_errors"]
        assert isinstance(errors, list)
        if str(error) not in errors:
            errors.append(str(error))
        return False
    conflicts = monitor["conflicts"]
    permitted = monitor["permitted_system_processes"]
    assert isinstance(conflicts, list)
    assert isinstance(permitted, list)
    _append_unique_records(conflicts, observed.conflicts)
    _append_unique_records(permitted, observed.permitted_system_processes)
    if observed.conflicts:
        monitor["conflict_poll_count"] = int(monitor["conflict_poll_count"]) + 1
        return False
    return True


def _finalize_host_monitor(
    monitor: Dict[str, object], cutoff_timed_out: bool
) -> Mapping[str, object]:
    conflicts = monitor["conflicts"]
    scan_errors = monitor["scan_errors"]
    assert isinstance(conflicts, list)
    assert isinstance(scan_errors, list)
    veto = ""
    if conflicts:
        veto = "host conflicts: " + json.dumps(conflicts, sort_keys=True)
    elif scan_errors:
        veto = "host scan error: " + "; ".join(str(item) for item in scan_errors)
    monitor.update(
        {
            "cutoff_timed_out": cutoff_timed_out,
            "veto": veto,
            "valid": (
                int(monitor["poll_count"]) >= 1
                and not conflicts
                and not scan_errors
            ),
            "end_utc": _utc_now(),
            "end_monotonic_ns": time.monotonic_ns(),
        }
    )
    return dict(monitor)


def _wait_for_exit_monitored(
    process: subprocess.Popen[bytes],
    host_monitor: Dict[str, object],
) -> Tuple[int, bool]:
    deadline = time.monotonic() + EXIT_TIMEOUT_SECONDS
    while process.poll() is None and time.monotonic() < deadline:
        _poll_host_monitor(host_monitor, process.pid)
        if process.poll() is not None:
            break
        remaining = deadline - time.monotonic()
        if remaining > 0:
            time.sleep(min(HOST_POLL_INTERVAL_MS / 1000.0, remaining))
    forced_kill = False
    if process.poll() is None:
        process.kill()
        forced_kill = True
    exit_code = process.wait(timeout=10)
    _poll_host_monitor(host_monitor, None)
    return exit_code, forced_kill


def _is_ac_power_output(output: str) -> bool:
    return any(
        line.rstrip("\r") == "Now drawing from 'AC Power'"
        for line in output.splitlines()
    )


def _capture_pmset(mode: str) -> Mapping[str, object]:
    if mode not in ("batt", "therm"):
        raise ABError(f"unsupported pmset capture mode: {mode}")
    command = ["/usr/bin/pmset", "-g", mode]
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="strict",
            check=False,
        )
        stdout = completed.stdout
        stderr = completed.stderr
        exit_code: Optional[int] = completed.returncode
        command_error: Optional[str] = None
    except (OSError, subprocess.SubprocessError, UnicodeError) as error:
        stdout = ""
        stderr = ""
        exit_code = None
        command_error = str(error)
    capture_valid = (
        command_error is None
        and exit_code == 0
        and stderr == ""
        and bool(stdout.strip())
    )
    return {
        "command": command,
        "exit_code": exit_code,
        "stdout": stdout,
        "stderr": stderr,
        "command_error": command_error,
        "capture_valid": capture_valid,
        "ac_power": _is_ac_power_output(stdout) if mode == "batt" else None,
    }


def _write_capture(path: pathlib.Path, capture: Mapping[str, object]) -> None:
    stdout = capture.get("stdout")
    if not isinstance(stdout, str):
        raise ABError("pmset capture stdout is not text")
    with path.open("x", encoding="utf-8") as output:
        output.write(stdout)


def _host_power_valid(
    power_before: Mapping[str, object],
    thermal_before: Mapping[str, object],
    power_after: Mapping[str, object],
    thermal_after: Mapping[str, object],
) -> bool:
    return bool(
        power_before.get("capture_valid")
        and power_before.get("ac_power") is True
        and thermal_before.get("capture_valid")
        and power_after.get("capture_valid")
        and power_after.get("ac_power") is True
        and thermal_after.get("capture_valid")
    )


def _artifact_hashes(run_root: pathlib.Path) -> Mapping[str, str]:
    return {
        name: _sha256_file(run_root / name)
        for name in (
            "cooldown-before.json",
            "preflight.json",
            "monitor.json",
            "result.json",
            "stdout.log",
            "stderr.log",
            "power-before.txt",
            "thermal-before.txt",
            "power-after.txt",
            "thermal-after.txt",
        )
    }


def _run_one(
    spec: RunSpec,
    binary: pathlib.Path,
    lua: pathlib.Path,
    save_template: pathlib.Path,
    rom_root: pathlib.Path,
    output_root: pathlib.Path,
    binary_description: str,
    matrix_binary_sha256: str,
    matrix_lua_sha256: str,
    runner: pathlib.Path,
    matrix_runner_sha256: str,
    matrix_freeze_expected: Mapping[str, str],
) -> RunOutcome:
    run_root = output_root / spec.label
    run_root.mkdir()
    cooldown = _guarded_cooldown(COOLDOWN_SECONDS)
    _write_json(run_root / "cooldown-before.json", cooldown)

    work_root = run_root / "work"
    work_root.mkdir()
    shutil.copytree(
        save_template,
        work_root / "_SaveData",
        symlinks=True,
        copy_function=shutil.copy2,
    )
    stdout_path = run_root / "stdout.log"
    stderr_path = run_root / "stderr.log"

    power_before = _capture_pmset("batt")
    thermal_before = _capture_pmset("therm")
    _write_capture(run_root / "power-before.txt", power_before)
    _write_capture(run_root / "thermal-before.txt", thermal_before)

    eboot = rom_root / "eboot.bin"
    shader_library = (
        rom_root / "Orbis" / "DataPS4" / "Bundles" / "ShaderProgLibrary.xom"
    )
    binary_before = _sha256_file(binary)
    lua_before = _sha256_file(lua)
    runner_before = _sha256_file(runner)
    eboot_before = _sha256_file(eboot)
    shader_before = _sha256_file(shader_library)
    save_template_before = _tree_snapshot(save_template, True)
    save_before = _tree_snapshot(work_root / "_SaveData", True)
    rom_before = _tree_snapshot(rom_root, False)
    crash_before = _crash_snapshot()
    freeze_observed_before = {
        "binary_sha256": binary_before,
        "lua_sha256": lua_before,
        "runner_sha256": runner_before,
        "save_template_digest": str(save_template_before["digest"]),
        "rom_tree_digest": str(rom_before["digest"]),
        "eboot_sha256": eboot_before,
        "shader_library_sha256": shader_before,
        "crash_roster_digest": _canonical_json_digest(crash_before),
    }
    freeze_matches_before = _freeze_matches(
        matrix_freeze_expected, freeze_observed_before
    )
    freeze_before_valid = (
        set(matrix_freeze_expected) == set(freeze_observed_before)
        and all(freeze_matches_before.values())
    )
    save_copy_matches_matrix = (
        str(save_before["digest"])
        == matrix_freeze_expected.get("save_template_digest")
    )
    matching_before = _matching_processes(binary, lua)
    prelaunch_scan_error: Optional[str] = None
    try:
        prelaunch_host_scan = _scan_host_conflicts(None)
    except (ABError, OSError, subprocess.SubprocessError) as error:
        prelaunch_scan_error = str(error)
        prelaunch_host_scan = HostScan((), ())
    environment, removed_environment = _sterile_environment(spec.selector)
    environment_set = {
        FRAME_TRACE_VARIABLE: environment[FRAME_TRACE_VARIABLE],
        SELECTOR_VARIABLE: environment[SELECTOR_VARIABLE],
    }
    production_default_overrides_present = {
        name: name in environment for name in PRODUCTION_DEFAULT_ABSENT_VARIABLES
    }
    preflight = {
        "version": 1,
        "phase": "6AO",
        "run": spec.label,
        "arm": spec.arm,
        "selector_value": spec.selector,
        "matrix_order": [label for label, _arm, _selector in MATRIX],
        "cooldown": cooldown,
        "cooldown_seconds": COOLDOWN_SECONDS,
        "host_poll_interval_ms": HOST_POLL_INTERVAL_MS,
        "host_prelaunch_scan": _host_scan_payload(prelaunch_host_scan),
        "host_prelaunch_scan_error": prelaunch_scan_error,
        "binary": str(binary),
        "binary_file": binary_description,
        "binary_sha256": binary_before,
        "matrix_binary_sha256": matrix_binary_sha256,
        "lua": str(lua),
        "lua_sha256": lua_before,
        "matrix_lua_sha256": matrix_lua_sha256,
        "runner": str(runner),
        "runner_sha256": runner_before,
        "matrix_runner_sha256": matrix_runner_sha256,
        "runner_matches_matrix_freeze": runner_before == matrix_runner_sha256,
        "matrix_freeze": dict(matrix_freeze_expected),
        "matrix_freeze_observed_before": freeze_observed_before,
        "matrix_freeze_matches_before": freeze_matches_before,
        "matrix_freeze_valid_before": freeze_before_valid,
        "rom_root": str(rom_root),
        "eboot_sha256": eboot_before,
        "shader_library_sha256": shader_before,
        "rom_tree": rom_before,
        "save_template": str(save_template),
        "save_template_snapshot": save_template_before,
        "save_before": save_before,
        "save_copy_matches_template": (
            save_before["digest"] == save_template_before["digest"]
        ),
        "save_copy_matches_matrix": save_copy_matches_matrix,
        "environment_remove_all_lumen": True,
        "environment_removed_prefixes": list(CONTAMINATING_ENVIRONMENT_PREFIXES),
        "environment_removed": removed_environment,
        "environment_set": environment_set,
        "selector_variable": SELECTOR_VARIABLE,
        "production_default_overrides_present": (
            production_default_overrides_present
        ),
        "show_window_early_present": "LUMEN_SHOW_WINDOW_EARLY" in environment,
        "archive_override_present": ARCHIVE_VARIABLE in environment,
        "archive_entries_override_present": (
            ARCHIVE_ENTRIES_VARIABLE in environment
        ),
        "admission_override_present": ADMISSION_VARIABLE in environment,
        "dynarec_override_present": DYNAREC_VARIABLE in environment,
        "present_cutoff": PRESENT_CUTOFF,
        "timeout_seconds": TIMEOUT_SECONDS,
        "matching_processes_before": matching_before,
        "power_before": power_before,
        "thermal_before": thermal_before,
        "crash_before": crash_before,
        "started_unix_ns": time.time_ns(),
    }
    _write_json(run_root / "preflight.json", preflight)

    process: Optional[subprocess.Popen[bytes]] = None
    pid: Optional[int] = None
    process_executable: Optional[str] = None
    process_identity_error: Optional[str] = None
    process_identity_matches = False
    launch_error: Optional[str] = None
    monitor_error: Optional[str] = None
    cutoff_present = False
    monitored_interval: Optional[decimal.Decimal] = None
    monitored_metric_error: Optional[str] = None
    cutoff_timed_out = False
    sigterm_count = 0
    sigterm_error: Optional[str] = None
    forced_kill = False
    exit_code: Optional[int] = None
    host_monitor: Optional[Dict[str, object]] = None

    pre_power_valid = bool(
        power_before.get("capture_valid")
        and power_before.get("ac_power") is True
        and thermal_before.get("capture_valid")
    )
    launch_blockers: List[str] = []
    if cooldown.get("valid") is not True:
        launch_blockers.append("monitored cooldown failed")
    if matching_before:
        launch_blockers.append(f"matching guest process already running: {matching_before}")
    if prelaunch_scan_error is not None:
        launch_blockers.append(f"host prelaunch scan failed: {prelaunch_scan_error}")
    if prelaunch_host_scan.conflicts:
        launch_blockers.append(
            "host prelaunch conflict: "
            + json.dumps(list(prelaunch_host_scan.conflicts), sort_keys=True)
        )
    if not pre_power_valid:
        launch_blockers.append("AC/thermal preflight failed")
    if runner_before != matrix_runner_sha256:
        launch_blockers.append("runner changed before replica launch")
    if not freeze_before_valid:
        launch_blockers.append(
            "matrix input freeze mismatch: "
            + json.dumps(freeze_matches_before, sort_keys=True)
        )
    if not save_copy_matches_matrix:
        launch_blockers.append("fresh save copy does not match the matrix freeze")

    with stdout_path.open("xb") as stdout_log, stderr_path.open("xb") as stderr_log:
        if launch_blockers:
            launch_error = "; ".join(launch_blockers)
        else:
            try:
                process = subprocess.Popen(
                    [str(binary), str(lua)],
                    cwd=work_root,
                    env=environment,
                    stdin=subprocess.DEVNULL,
                    stdout=stdout_log,
                    stderr=stderr_log,
                )
                pid = process.pid
                host_monitor = _new_host_monitor(process.pid)
                try:
                    resolved_executable = _process_executable(process.pid)
                    process_executable = str(resolved_executable)
                    process_identity_matches = resolved_executable == binary
                except (ABError, OSError) as error:
                    process_identity_error = str(error)
                try:
                    (
                        cutoff_present,
                        monitored_interval,
                        monitored_metric_error,
                        host_veto,
                        cutoff_timed_out,
                    ) = _monitor_cutoff(
                        process,
                        stderr_path,
                        TIMEOUT_SECONDS,
                        host_monitor,
                    )
                    if host_veto is not None:
                        monitor_error = host_veto
                except (ABError, OSError) as error:
                    monitor_error = str(error)
            except (OSError, subprocess.SubprocessError) as error:
                launch_error = str(error)
            finally:
                if process is not None and process.poll() is None:
                    try:
                        process.send_signal(signal.SIGTERM)
                        sigterm_count += 1
                    except OSError as error:
                        sigterm_error = str(error)
                if process is not None:
                    try:
                        assert host_monitor is not None
                        if process.poll() is None:
                            exit_code, forced_kill = _wait_for_exit_monitored(
                                process, host_monitor
                            )
                        else:
                            exit_code = process.wait()
                            _poll_host_monitor(host_monitor, None)
                    except (OSError, subprocess.SubprocessError) as error:
                        monitor_error = (
                            str(error)
                            if monitor_error is None
                            else monitor_error + "; " + str(error)
                        )
                        if process.poll() is None:
                            process.kill()
                            forced_kill = True
                        exit_code = process.wait(timeout=10)

    power_after = _capture_pmset("batt")
    thermal_after = _capture_pmset("therm")
    _write_capture(run_root / "power-after.txt", power_after)
    _write_capture(run_root / "thermal-after.txt", thermal_after)
    host_power_valid = _host_power_valid(
        power_before, thermal_before, power_after, thermal_after
    )

    if host_monitor is None:
        monitor_evidence: Dict[str, object] = {
            "schema": 1,
            "guest_pid": None,
            "poll_interval_ms": HOST_POLL_INTERVAL_MS,
            "poll_count": 0,
            "conflict_poll_count": 0,
            "conflicts": [],
            "permitted_system_processes": [],
            "scan_errors": [],
            "cutoff_timed_out": False,
            "veto": launch_error or "guest was not launched",
            "valid": False,
            "start_utc": None,
            "start_monotonic_ns": None,
            "end_utc": _utc_now(),
            "end_monotonic_ns": time.monotonic_ns(),
        }
    else:
        monitor_evidence = dict(
            _finalize_host_monitor(host_monitor, cutoff_timed_out)
        )
    monitor_evidence.update(
        {
            "cutoff_present": cutoff_present,
            "monitored_avg_interval_ms": (
                _format_decimal(monitored_interval)
                if monitored_interval is not None
                else None
            ),
            "monitored_metric_error": monitored_metric_error,
        }
    )
    _write_json(run_root / "monitor.json", monitor_evidence)

    stdout_bytes = stdout_path.read_bytes()
    stderr_bytes = stderr_path.read_bytes()
    combined_bytes = stdout_bytes + b"\n" + stderr_bytes
    present_count = sum(
        PRESENT_LINE_PATTERN.match(line.rstrip(b"\r")) is not None
        for line in combined_bytes.splitlines()
    )
    parser_error: Optional[str] = None
    parsed_interval: Optional[decimal.Decimal] = None
    try:
        parsed_interval = _parse_unique_present_metric(combined_bytes)
    except ABError as error:
        parser_error = str(error)
    monitor_matches_parser = (
        monitored_interval is not None
        and parsed_interval is not None
        and monitored_interval == parsed_interval
    )

    save_root_exists = (work_root / "_SaveData").is_dir()
    save_after = _tree_snapshot(work_root / "_SaveData", True)
    save_template_after = _tree_snapshot(save_template, True)
    rom_after = _tree_snapshot(rom_root, False)
    crash_after = _crash_snapshot()
    binary_after = _sha256_file(binary)
    lua_after = _sha256_file(lua)
    runner_after = _sha256_file(runner)
    eboot_after = _sha256_file(eboot)
    shader_after = _sha256_file(shader_library)
    freeze_observed_after = {
        "binary_sha256": binary_after,
        "lua_sha256": lua_after,
        "runner_sha256": runner_after,
        "save_template_digest": str(save_template_after["digest"]),
        "rom_tree_digest": str(rom_after["digest"]),
        "eboot_sha256": eboot_after,
        "shader_library_sha256": shader_after,
        "crash_roster_digest": _canonical_json_digest(crash_after),
    }
    freeze_matches_after = _freeze_matches(
        matrix_freeze_expected, freeze_observed_after
    )
    freeze_after_valid = (
        set(matrix_freeze_expected) == set(freeze_observed_after)
        and all(freeze_matches_after.values())
    )
    residues = _matching_processes(binary, lua)
    outside_save = _outside_save_entries(work_root)
    sdl_quit = combined_bytes.count(SDL_QUIT_MARKER)
    error_matches = [
        match.decode("ascii", errors="replace")
        for match in ERROR_PATTERN.findall(combined_bytes)
    ]

    save_copy_matches_template = (
        save_before["digest"] == save_template_before["digest"]
    )
    save_unchanged = save_before["digest"] == save_after["digest"]
    save_template_unchanged = (
        save_template_before["digest"] == save_template_after["digest"]
    )
    rom_unchanged = (
        rom_before["digest"] == rom_after["digest"]
        and eboot_before == eboot_after
        and shader_before == shader_after
    )
    crash_unchanged = crash_before == crash_after
    inputs_unchanged = (
        binary_before == matrix_binary_sha256
        and binary_after == matrix_binary_sha256
        and lua_before == matrix_lua_sha256
        and lua_after == matrix_lua_sha256
    )
    runner_unchanged = (
        runner_before == matrix_runner_sha256
        and runner_after == matrix_runner_sha256
    )
    allowed_environment = {FRAME_TRACE_VARIABLE, SELECTOR_VARIABLE}
    unexpected_contaminating_environment = sorted(
        name
        for name in environment
        if name not in allowed_environment
        and (
            name in EXTERNAL_DIAGNOSTIC_ENVIRONMENT
            or any(
                name.startswith(prefix)
                for prefix in CONTAMINATING_ENVIRONMENT_PREFIXES
            )
        )
    )
    environment_valid = (
        environment_set
        == {
            FRAME_TRACE_VARIABLE: "1",
            SELECTOR_VARIABLE: str(spec.selector),
        }
        and "LUMEN_SHOW_WINDOW_EARLY" not in environment
        and not any(production_default_overrides_present.values())
        and not unexpected_contaminating_environment
    )
    valid = all(
        (
            cooldown.get("valid") is True,
            prelaunch_scan_error is None,
            not prelaunch_host_scan.conflicts,
            not matching_before,
            launch_error is None,
            monitor_error is None,
            monitor_evidence.get("valid") is True,
            cutoff_present,
            not cutoff_timed_out,
            monitored_metric_error is None,
            parser_error is None,
            present_count == 1,
            monitor_matches_parser,
            process_identity_error is None,
            process_identity_matches,
            sigterm_count == 1,
            sigterm_error is None,
            not forced_kill,
            exit_code == 0,
            sdl_quit == 1,
            not error_matches,
            save_copy_matches_template,
            save_copy_matches_matrix,
            save_root_exists,
            save_unchanged,
            save_template_unchanged,
            rom_unchanged,
            crash_unchanged,
            inputs_unchanged,
            runner_unchanged,
            freeze_before_valid,
            freeze_after_valid,
            host_power_valid,
            not residues,
            not outside_save,
            environment_valid,
        )
    )
    result = {
        "version": 1,
        "phase": "6AO",
        "run": spec.label,
        "arm": spec.arm,
        "selector_value": spec.selector,
        "valid": valid,
        "cooldown_valid": cooldown.get("valid") is True,
        "cooldown": cooldown,
        "host_prelaunch_scan": _host_scan_payload(prelaunch_host_scan),
        "host_prelaunch_scan_error": prelaunch_scan_error,
        "host_monitor_valid": monitor_evidence.get("valid") is True,
        "host_monitor": monitor_evidence,
        "pid": pid,
        "process_executable": process_executable,
        "process_identity_matches": process_identity_matches,
        "process_identity_error": process_identity_error,
        "launch_error": launch_error,
        "monitor_error": monitor_error,
        "cutoff_present": cutoff_present,
        "cutoff_timed_out": cutoff_timed_out,
        "present_count": present_count,
        "monitored_avg_interval_ms": (
            _format_decimal(monitored_interval)
            if monitored_interval is not None
            else None
        ),
        "monitored_metric_error": monitored_metric_error,
        "parsed_avg_interval_ms": (
            _format_decimal(parsed_interval) if parsed_interval is not None else None
        ),
        "parser_error": parser_error,
        "monitor_matches_parser": monitor_matches_parser,
        "sigterm_count": sigterm_count,
        "sigterm_error": sigterm_error,
        "forced_kill": forced_kill,
        "exit_code": exit_code,
        "sdl_quit": sdl_quit,
        "error_matches": error_matches,
        "environment_valid": environment_valid,
        "environment_set": environment_set,
        "selector_variable": SELECTOR_VARIABLE,
        "production_default_overrides_present": (
            production_default_overrides_present
        ),
        "unexpected_contaminating_environment": (
            unexpected_contaminating_environment
        ),
        "host_power_valid": host_power_valid,
        "power_before": power_before,
        "thermal_before": thermal_before,
        "power_after": power_after,
        "thermal_after": thermal_after,
        "save_copy_matches_template": save_copy_matches_template,
        "save_copy_matches_matrix": save_copy_matches_matrix,
        "save_root_exists": save_root_exists,
        "save_unchanged": save_unchanged,
        "save_after": save_after,
        "save_template_unchanged": save_template_unchanged,
        "save_template_after": save_template_after,
        "rom_unchanged": rom_unchanged,
        "rom_after": rom_after,
        "critical_rom_hashes_after": {
            "eboot.bin": eboot_after,
            "ShaderProgLibrary.xom": shader_after,
        },
        "crash_unchanged": crash_unchanged,
        "crash_after": crash_after,
        "inputs_unchanged": inputs_unchanged,
        "runner_unchanged": runner_unchanged,
        "runner_sha256_before": runner_before,
        "runner_sha256_after": runner_after,
        "matrix_runner_sha256": matrix_runner_sha256,
        "matrix_freeze": dict(matrix_freeze_expected),
        "matrix_freeze_observed_before": freeze_observed_before,
        "matrix_freeze_matches_before": freeze_matches_before,
        "matrix_freeze_valid_before": freeze_before_valid,
        "matrix_freeze_observed_after": freeze_observed_after,
        "matrix_freeze_matches_after": freeze_matches_after,
        "matrix_freeze_valid_after": freeze_after_valid,
        "binary_sha256_after": binary_after,
        "lua_sha256_after": lua_after,
        "matching_process_residues": residues,
        "outside_save_entries": outside_save,
        "finished_unix_ns": time.time_ns(),
    }
    _write_json(run_root / "result.json", result)
    artifact_hashes = _artifact_hashes(run_root)
    _write_json(
        run_root / "hashes.json",
        {
            "version": 1,
            "phase": "6AO",
            "run": spec.label,
            "sha256": artifact_hashes,
        },
    )
    return RunOutcome(spec, valid, parsed_interval, artifact_hashes)


def _median(values: Sequence[decimal.Decimal]) -> decimal.Decimal:
    if len(values) != 3:
        raise ABError(f"phase6AO median requires exactly three values, got {len(values)}")
    return sorted(values)[1]


def _evaluate_decision(
    outcomes: Sequence[RunOutcome],
    runner_sha256_initial: str,
    runner_sha256_final: str,
    matrix_freeze_initial: Mapping[str, str],
    matrix_freeze_final: Mapping[str, str],
) -> Mapping[str, object]:
    expected = [RunSpec(*item) for item in MATRIX]
    observed_specs = [outcome.spec for outcome in outcomes]
    matrix_complete = observed_specs == expected
    by_label = {outcome.spec.label: outcome for outcome in outcomes}
    expected_labels = [spec.label for spec in expected]
    labels_unique = len(by_label) == len(outcomes)
    runner_unchanged = runner_sha256_final == runner_sha256_initial
    matrix_freeze_matches_final = _freeze_matches(
        matrix_freeze_initial, matrix_freeze_final
    )
    matrix_inputs_unchanged = (
        set(matrix_freeze_initial) == set(matrix_freeze_final)
        and all(matrix_freeze_matches_final.values())
    )
    functional_gates_valid = (
        matrix_complete
        and labels_unique
        and runner_unchanged
        and matrix_inputs_unchanged
        and all(by_label[label].valid for label in expected_labels)
        and all(by_label[label].avg_interval_ms is not None for label in expected_labels)
    )

    pair_results: List[Mapping[str, object]] = []
    pair_improvements = 0
    for index in (1, 2, 3):
        baseline = by_label.get(f"B{index}")
        candidate = by_label.get(f"C{index}")
        baseline_value = baseline.avg_interval_ms if baseline is not None else None
        candidate_value = candidate.avg_interval_ms if candidate is not None else None
        improved = (
            baseline_value is not None
            and candidate_value is not None
            and candidate_value < baseline_value
        )
        if improved:
            pair_improvements += 1
        pair_results.append(
            {
                "pair": f"B{index}/C{index}",
                "baseline_ms": (
                    _format_decimal(baseline_value)
                    if baseline_value is not None
                    else None
                ),
                "candidate_ms": (
                    _format_decimal(candidate_value)
                    if candidate_value is not None
                    else None
                ),
                "candidate_strictly_lower": improved,
            }
        )

    baseline_values = [
        by_label[label].avg_interval_ms
        for label in ("B1", "B2", "B3")
        if label in by_label and by_label[label].avg_interval_ms is not None
    ]
    candidate_values = [
        by_label[label].avg_interval_ms
        for label in ("C1", "C2", "C3")
        if label in by_label and by_label[label].avg_interval_ms is not None
    ]
    median_baseline: Optional[decimal.Decimal] = None
    median_candidate: Optional[decimal.Decimal] = None
    reduction_percent: Optional[decimal.Decimal] = None
    if len(baseline_values) == 3 and len(candidate_values) == 3:
        median_baseline = _median(baseline_values)  # type: ignore[arg-type]
        median_candidate = _median(candidate_values)  # type: ignore[arg-type]
        if median_baseline > 0:
            reduction_percent = (
                (median_baseline - median_candidate)
                * decimal.Decimal("100")
                / median_baseline
            )

    median_gate = (
        reduction_percent is not None
        and reduction_percent >= MINIMUM_MEDIAN_REDUCTION_PERCENT
    )
    pairs_gate = pair_improvements == 3
    if not functional_gates_valid:
        verdict = "INVALID"
    elif median_gate and pairs_gate:
        verdict = "GO"
    else:
        verdict = "NO_GO"
    return {
        "version": 1,
        "phase": "6AO",
        "verdict": verdict,
        "matrix_order_expected": expected_labels,
        "matrix_order_observed": [spec.label for spec in observed_specs],
        "matrix_complete": matrix_complete,
        "runner_sha256_initial": runner_sha256_initial,
        "runner_sha256_final": runner_sha256_final,
        "runner_unchanged": runner_unchanged,
        "matrix_freeze_initial": dict(matrix_freeze_initial),
        "matrix_freeze_final": dict(matrix_freeze_final),
        "matrix_freeze_matches_final": matrix_freeze_matches_final,
        "matrix_inputs_unchanged": matrix_inputs_unchanged,
        "functional_gates_valid": functional_gates_valid,
        "runs": [
            {
                "run": outcome.spec.label,
                "arm": outcome.spec.arm,
                "selector_value": outcome.spec.selector,
                "valid": outcome.valid,
                "avg_interval_ms": (
                    _format_decimal(outcome.avg_interval_ms)
                    if outcome.avg_interval_ms is not None
                    else None
                ),
                "artifact_sha256": dict(outcome.artifact_hashes),
            }
            for outcome in outcomes
        ],
        "baseline_median_ms": (
            _format_decimal(median_baseline) if median_baseline is not None else None
        ),
        "candidate_median_ms": (
            _format_decimal(median_candidate) if median_candidate is not None else None
        ),
        "median_reduction_percent": (
            _format_decimal(reduction_percent)
            if reduction_percent is not None
            else None
        ),
        "minimum_median_reduction_percent": _format_decimal(
            MINIMUM_MEDIAN_REDUCTION_PERCENT
        ),
        "median_gate": median_gate,
        "pairs": pair_results,
        "pair_improvements": pair_improvements,
        "pairs_gate": pairs_gate,
        "finished_unix_ns": time.time_ns(),
    }


def _self_test_parser() -> None:
    valid = (
        b"noise\n"
        b"[FRAME_PIPELINE] present=120 avg_interval=95.250 ms "
        b"avg_latency=1.000 ms pending=0\n"
    )
    if _parse_unique_present_metric(valid) != decimal.Decimal("95.250"):
        raise ABError("parser did not preserve the p120 decimal metric")
    invalid_logs = (
        valid
        + b"[FRAME_PIPELINE] present=120 avg_interval=96.000 ms avg_latency=1 ms\n",
        b"[FRAME_PIPELINE] present=120 avg_interval=NaN ms\n",
        b"[FRAME_PIPELINE] present=120 avg_interval=0 ms\n",
        b"[FRAME_PIPELINE] present=120 avg_interval_ms=1\n",
        b"[FRAME_PIPELINE] present=240 avg_interval=1.000 ms\n",
    )
    for invalid in invalid_logs:
        try:
            _parse_unique_present_metric(invalid)
        except ABError:
            continue
        raise ABError("parser accepted an invalid or non-unique p120 metric")


def _self_test_cutoff() -> None:
    class ExitedProcess:
        @staticmethod
        def poll() -> int:
            return 0

    with tempfile.TemporaryDirectory(prefix="phase6ao-cutoff-self-test-") as temporary:
        stderr_path = pathlib.Path(temporary) / "stderr.log"
        stderr_path.write_bytes(
            b"[FRAME_PIPELINE] present=120 avg_interval=87.125 ms "
            b"avg_latency=1.000 ms\n"
        )
        seen, value, error, host_veto, timed_out = _monitor_cutoff(  # type: ignore[arg-type]
            ExitedProcess(), stderr_path, 1
        )
        if (
            not seen
            or value != decimal.Decimal("87.125")
            or error is not None
            or host_veto is not None
            or timed_out
        ):
            raise ABError(
                f"cutoff oracle mismatch: "
                f"{(seen, value, error, host_veto, timed_out)}"
            )

        stderr_path.write_bytes(
            b"[FRAME_PIPELINE] present=120 avg_interval=NaN ms\n"
        )
        seen, value, error, host_veto, timed_out = _monitor_cutoff(  # type: ignore[arg-type]
            ExitedProcess(), stderr_path, 1
        )
        if (
            not seen
            or value is not None
            or error is None
            or host_veto is not None
            or timed_out
        ):
            raise ABError("cutoff accepted a malformed p120 metric")

        stderr_path.write_bytes(
            b"[FRAME_PIPELINE] present=240 avg_interval=1.000 ms\n"
        )
        seen, value, error, host_veto, timed_out = _monitor_cutoff(  # type: ignore[arg-type]
            ExitedProcess(), stderr_path, 1
        )
        if (
            seen
            or value is not None
            or error is not None
            or host_veto is not None
            or timed_out
        ):
            raise ABError("cutoff selected a non-p120 line")


def _self_test_environment() -> None:
    inherited = {
        "PATH": "/usr/bin:/bin",
        "SAFE_VALUE": "keep",
        "LUMEN_SHOW_WINDOW_EARLY": "1",
        DYNAREC_VARIABLE: "0",
        ARCHIVE_VARIABLE: "0",
        ARCHIVE_ENTRIES_VARIABLE: "512",
        ADMISSION_VARIABLE: "0",
        SELECTOR_VARIABLE: "malformed",
        "DYLD_LIBRARY_PATH": "/tmp/injected",
        "LLVM_PROFILE_FILE": "/tmp/profile",
        "CPUPROFILE": "/tmp/cpu",
        "HEAPPROFILE_ALLOCATION_INTERVAL": "1",
        "MSAN_OPTIONS": "halt_on_error=1",
        "VK_INSTANCE_LAYERS": "validation",
        "VK_CUSTOM_LAYER_POLICY": "1",
        "MVK_CONFIG_DEBUG": "1",
        "MTL_DEBUG_LAYER": "1",
        "MTL_CAPTURE_SCOPE": "all",
        "METAL_DEVICE_WRAPPER_TYPE": "1",
        "METAL_VALIDATION_MODE": "enabled",
        "SDL_VIDEODRIVER": "dummy",
        "OS_ACTIVITY_MODE": "debug",
        "OS_SIGNPOST_MODE": "enabled",
        "MallocStackLogging": "1",
        "MallocScribble": "1",
        "NSZombieEnabled": "YES",
        "NSZombieCustomPolicy": "YES",
        "NSAutoreleaseFreedObjectCheckEnabled": "YES",
        "NSAutoreleaseFreedObjectCheckCustom": "YES",
        "ASAN_OPTIONS": "halt_on_error=1",
        "ASAN_SYMBOLIZER_PATH": "/tmp/symbolizer",
        "LSAN_OPTIONS": "report_objects=1",
        "LSAN_CUSTOM_POLICY": "1",
        "TSAN_OPTIONS": "halt_on_error=1",
        "TSAN_CUSTOM_POLICY": "1",
        "UBSAN_OPTIONS": "halt_on_error=1",
        "UBSAN_CUSTOM_POLICY": "1",
    }
    environment, removed = _sterile_environment(1, inherited)
    expected_removed = sorted(set(inherited) - {"PATH", "SAFE_VALUE"})
    if removed != expected_removed:
        raise ABError(f"environment removal mismatch: {removed}")
    expected = {
        "PATH": "/usr/bin:/bin",
        "SAFE_VALUE": "keep",
        FRAME_TRACE_VARIABLE: "1",
        SELECTOR_VARIABLE: "1",
    }
    if environment != expected:
        raise ABError(f"sterile environment mismatch: {environment}")


def _self_test_matrix_and_selector() -> None:
    literal_matrix = (
        RunSpec("B1", "B", 0),
        RunSpec("C1", "C", 1),
        RunSpec("C2", "C", 1),
        RunSpec("B2", "B", 0),
        RunSpec("B3", "B", 0),
        RunSpec("C3", "C", 1),
    )
    observed_matrix = tuple(RunSpec(*item) for item in MATRIX)
    if observed_matrix != literal_matrix:
        raise ABError(
            f"literal B1,C1,C2,B2,B3,C3 matrix mismatch: {observed_matrix}"
        )
    if any(
        spec.selector != (0 if spec.arm == "B" else 1)
        for spec in observed_matrix
    ):
        raise ABError("matrix arm does not map exactly to selector B=0/C=1")
    for invalid_selector in (-1, 2, True, "1"):
        try:
            _sterile_environment(invalid_selector)  # type: ignore[arg-type]
        except ABError:
            continue
        raise ABError(f"accepted invalid selector: {invalid_selector!r}")


def _self_test_conflicts_and_cooldown() -> None:
    extended_names = (
        "lumen_cpu_a64_dynarec_test",
        "lumen_cpu_x64_test",
        "lumenemu",
        "ctest",
        "actool",
        "ar",
        "bazel",
        "buck",
        "buck2",
        "make",
        "gmake",
        "cc",
        "c++",
        "cc1",
        "cc1plus",
        "ccache",
        "codesign",
        "collect2",
        "dsymutil",
        "dtrace",
        "gdb",
        "heap",
        "ibtool",
        "Instruments",
        "leaks",
        "libtool",
        "link",
        "lld-link",
        "swiftc",
        "swift-frontend",
        "metal",
        "metalfe",
        "metallib",
        "meson",
        "perf",
        "powermetrics",
        "ranlib",
        "sccache",
        "scons",
        "vmmap",
        "wasm-ld",
        "lldb",
        "lldb-rpc-server",
        "debugserver",
        "ld64",
        "ld-classic",
        "ld-prime",
        "ld.lld",
        "ld64.lld",
        "ld.gold",
        "gold",
        "mold",
        "lld",
        "clang-18",
        "clang++-17.0",
        "gcc-14",
        "g++-13.2",
    )
    rows = [
        ProcessRow(1, 0, "/sbin/launchd"),
        ProcessRow(9, 1, "/usr/bin/xctrace"),
        ProcessRow(10, 9, "/opt/homebrew/bin/cmake"),
        ProcessRow(20, 10, "/usr/bin/python3"),
        ProcessRow(30, 20, "/tmp/fc_script"),
        ProcessRow(40, 1, "/opt/homebrew/bin/ninja"),
        ProcessRow(50, 1, "/tmp/notninja"),
        ProcessRow(60, 1, "/usr/bin/xctrace"),
        ProcessRow(70, 1, "/usr/sbin/spindump"),
        ProcessRow(71, 2, "/tmp/spindump"),
        ProcessRow(72, 1, "/usr/bin/clang"),
        ProcessRow(73, 1, "/tmp/notmake"),
        ProcessRow(74, 1, "/tmp/debugserver-helper"),
        ProcessRow(75, 1, "/tmp/clang-format"),
        ProcessRow(76, 1, "/tmp/gcc-helper"),
        ProcessRow(77, 1, "/tmp/lumen_cpu_test_helper"),
    ]
    rows.extend(
        ProcessRow(pid, 1, f"/fixture/bin/{name}")
        for pid, name in enumerate(extended_names, start=80)
    )
    resolved = {
        9: pathlib.Path("/usr/bin/xctrace"),
        10: pathlib.Path("/opt/homebrew/bin/cmake"),
        40: pathlib.Path("/opt/homebrew/bin/ninja"),
        60: pathlib.Path("/usr/bin/xctrace"),
        70: SYSTEM_SPINDUMP_PATH,
        71: pathlib.Path("/tmp/spindump"),
    }
    resolved.update(
        {
            pid: pathlib.Path(f"/fixture/bin/{name}")
            for pid, name in enumerate(extended_names, start=80)
        }
    )

    def resolver(pid: int) -> pathlib.Path:
        if pid == 72:
            raise OSError(1, "fixture identity failure")
        return resolved[pid]

    scan = _host_scan_from_rows(rows, 30, 20, resolver)
    expected_conflicts = [9, 10, 40, 60, 71, 72, *range(80, 80 + len(extended_names))]
    if [record["pid"] for record in scan.conflicts] != expected_conflicts:
        raise ABError(f"exact executable conflict scan mismatch: {scan.conflicts}")
    if [record["pid"] for record in scan.permitted_system_processes] != [70]:
        raise ABError("system spindump exception was not exact")

    clean = _guarded_cooldown(0, lambda _guest: HostScan((), ()))
    if clean.get("valid") is not True or clean.get("poll_count") != 1:
        raise ABError(f"zero-second cooldown fixture failed: {clean}")
    vetoed = _guarded_cooldown(0, lambda _guest: scan)
    if (
        vetoed.get("valid") is not False
        or vetoed.get("conflict_count") != len(expected_conflicts)
    ):
        raise ABError(f"cooldown did not fail closed on conflicts: {vetoed}")

    if not _is_ac_power_output("Now drawing from 'AC Power'\n"):
        raise ABError("AC power parser rejected the canonical pmset line")
    if _is_ac_power_output("status: not AC Power\n"):
        raise ABError("AC power parser accepted an ambiguous substring")


def _self_test_process_scan() -> None:
    script = pathlib.Path(__file__).resolve()
    executable = _process_executable(os.getpid())
    with tempfile.TemporaryDirectory(prefix="phase6ao-process-self-test-") as temporary:
        child_script = pathlib.Path(temporary) / "phase6ao_ab_child.py"
        shutil.copy2(script, child_script)
        environment, _removed = _sterile_environment(0)
        child = subprocess.Popen(
            [sys.executable, str(child_script), "--process-scan-child"],
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            deadline = time.monotonic() + 5
            matches: List[int] = []
            while time.monotonic() < deadline:
                matches = _matching_processes(executable, child_script)
                if child.pid in matches:
                    break
                time.sleep(0.05)
            if matches != [child.pid]:
                raise ABError(
                    f"process scan expected only child {child.pid}, observed {matches}"
                )
        finally:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)
        if _matching_processes(executable, child_script):
            raise ABError("process scan retained a completed child")
    if os.getpid() in _matching_processes(executable, script):
        raise ABError("process scan matched the runner itself")


def _test_outcomes(
    values: Mapping[str, decimal.Decimal], invalid_label: Optional[str] = None
) -> List[RunOutcome]:
    return [
        RunOutcome(
            RunSpec(label, arm, selector),
            label != invalid_label,
            values[label],
            {},
        )
        for label, arm, selector in MATRIX
    ]


def _self_test_decision() -> None:
    boundary_values = {
        "B1": decimal.Decimal("100"),
        "C1": decimal.Decimal("95"),
        "C2": decimal.Decimal("104.5"),
        "B2": decimal.Decimal("110"),
        "B3": decimal.Decimal("120"),
        "C3": decimal.Decimal("114"),
    }
    frozen = {
        "input": "same",
        "crash_roster_digest": _canonical_json_digest([]),
    }

    def evaluate(
        outcomes: Sequence[RunOutcome],
        runner_final: str = "runner",
        freeze_final: Optional[Mapping[str, str]] = None,
    ) -> Mapping[str, object]:
        return _evaluate_decision(
            outcomes,
            "runner",
            runner_final,
            frozen,
            frozen if freeze_final is None else freeze_final,
        )

    decision = evaluate(_test_outcomes(boundary_values))
    if (
        decision["verdict"] != "GO"
        or decimal.Decimal(str(decision["median_reduction_percent"]))
        != MINIMUM_MEDIAN_REDUCTION_PERCENT
        or decision["pair_improvements"] != 3
    ):
        raise ABError(f"decision rejected the inclusive five-percent boundary: {decision}")

    below_threshold = dict(boundary_values)
    below_threshold["C2"] = decimal.Decimal("105")
    decision = evaluate(_test_outcomes(below_threshold))
    if decision["verdict"] != "NO_GO" or decision["median_gate"]:
        raise ABError("decision accepted a median reduction below five percent")

    failed_pair = dict(boundary_values)
    failed_pair["C1"] = decimal.Decimal("101")
    failed_pair["C2"] = decimal.Decimal("90")
    decision = evaluate(_test_outcomes(failed_pair))
    if decision["verdict"] != "NO_GO" or decision["pairs_gate"]:
        raise ABError("decision accepted fewer than three favorable pairs")

    decision = evaluate(_test_outcomes(boundary_values, "B2"))
    if decision["verdict"] != "INVALID":
        raise ABError("decision did not invalidate a failed functional run")

    decision = evaluate(_test_outcomes(boundary_values)[:-1])
    if decision["verdict"] != "INVALID":
        raise ABError("decision did not invalidate an incomplete fixed matrix")

    decision = evaluate(_test_outcomes(boundary_values), "runner-after")
    if decision["verdict"] != "INVALID" or decision["runner_unchanged"]:
        raise ABError("decision did not invalidate a changed runner")

    changed_crash_freeze = dict(frozen)
    changed_crash_freeze["crash_roster_digest"] = _canonical_json_digest(
        [{"path": "fc_script-delayed.ips", "size": 1, "mtime_ns": 1}]
    )
    decision = evaluate(
        _test_outcomes(boundary_values), freeze_final=changed_crash_freeze
    )
    if decision["verdict"] != "INVALID" or decision["matrix_inputs_unchanged"]:
        raise ABError("decision did not invalidate an inter-replica crash change")


def _validate_live_inputs(
    arguments: argparse.Namespace,
) -> Tuple[pathlib.Path, pathlib.Path, pathlib.Path, pathlib.Path, pathlib.Path, str]:
    if sys.platform != "darwin":
        raise ABError("phase6AO live A/B is restricted to macOS")
    binary = pathlib.Path(arguments.binary).resolve(strict=True)
    lua = pathlib.Path(arguments.lua).resolve(strict=True)
    save_template = pathlib.Path(arguments.save_template).resolve(strict=True)
    rom_root = pathlib.Path(arguments.rom_root).resolve(strict=True)
    output_root = pathlib.Path(arguments.output).resolve()
    if output_root.exists():
        raise ABError(f"output already exists: {output_root}")
    if signal.getsignal(signal.SIGTERM) != signal.SIG_DFL:
        raise ABError("runner SIGTERM disposition is not SIG_DFL")
    if signal.SIGTERM in signal.pthread_sigmask(signal.SIG_BLOCK, []):
        raise ABError("runner inherits a blocked SIGTERM")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ABError(f"binary is not an executable file: {binary}")
    if not lua.is_file():
        raise ABError(f"Lua input is not a file: {lua}")
    if not save_template.is_dir():
        raise ABError(f"save template is not a directory: {save_template}")
    if not rom_root.is_dir():
        raise ABError(f"ROM root is not a directory: {rom_root}")
    if _is_within(output_root, save_template) or _is_within(output_root, rom_root):
        raise ABError("output root must not be inside the save template or ROM root")
    eboot = rom_root / "eboot.bin"
    shader_library = (
        rom_root / "Orbis" / "DataPS4" / "Bundles" / "ShaderProgLibrary.xom"
    )
    if not eboot.is_file() or not shader_library.is_file():
        raise ABError("ROM root does not contain the preregistered eboot/shader library")
    binary_description = _file_description(binary)
    if (
        "Mach-O 64-bit executable arm64" not in binary_description
        or "universal" in binary_description.lower()
    ):
        raise ABError(f"binary is not thin arm64: {binary_description}")
    if not _contains_bytes(binary, SELECTOR_VARIABLE.encode("ascii")):
        raise ABError("binary does not contain the phase-6AO verified-IR selector")
    return (
        binary,
        lua,
        save_template,
        rom_root,
        output_root,
        binary_description,
    )


def run_matrix(arguments: argparse.Namespace) -> int:
    (
        binary,
        lua,
        save_template,
        rom_root,
        output_root,
        binary_description,
    ) = _validate_live_inputs(arguments)
    runner = pathlib.Path(__file__).resolve(strict=True)
    matrix_freeze_initial = _matrix_freeze(
        binary, lua, runner, save_template, rom_root
    )
    matrix_binary_sha256 = matrix_freeze_initial["binary_sha256"]
    matrix_lua_sha256 = matrix_freeze_initial["lua_sha256"]
    matrix_runner_sha256 = matrix_freeze_initial["runner_sha256"]
    output_root.mkdir(parents=True)

    outcomes: List[RunOutcome] = []
    for label, arm, selector in MATRIX:
        outcome = _run_one(
            RunSpec(label, arm, selector),
            binary,
            lua,
            save_template,
            rom_root,
            output_root,
            binary_description,
            matrix_binary_sha256,
            matrix_lua_sha256,
            runner,
            matrix_runner_sha256,
            matrix_freeze_initial,
        )
        outcomes.append(outcome)
        interval_text = (
            _format_decimal(outcome.avg_interval_ms)
            if outcome.avg_interval_ms is not None
            else "none"
        )
        print(
            "phase6ao-ab-run: "
            f"{'PASS' if outcome.valid else 'INVALID'} run={label} "
            f"selector={selector} avg_interval_ms={interval_text}"
        )

    matrix_freeze_final = _matrix_freeze(
        binary, lua, runner, save_template, rom_root
    )
    final_runner_sha256 = matrix_freeze_final["runner_sha256"]
    decision = _evaluate_decision(
        outcomes,
        matrix_runner_sha256,
        final_runner_sha256,
        matrix_freeze_initial,
        matrix_freeze_final,
    )
    _write_json(output_root / "decision.json", decision)
    print(
        "phase6ao-ab: "
        f"{decision['verdict']} B_median_ms={decision['baseline_median_ms']} "
        f"C_median_ms={decision['candidate_median_ms']} "
        f"reduction_percent={decision['median_reduction_percent']} "
        f"pairs={decision['pair_improvements']}/3"
    )
    return 0 if decision["verdict"] == "GO" else 1


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "run the fixed phase-6AO B1,C1,C2,B2,B3,C3 raw/verified-IR "
            "A/B matrix (selector B=0/C=1; K1024 and production policies "
            "remain unset)"
        )
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help=(
            "run sterile-environment, literal-matrix, selector, parser, cutoff, "
            "process-scan, and decision oracles"
        ),
    )
    parser.add_argument("--process-scan-child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--binary", help="production-like thin-arm64 fc_script")
    parser.add_argument("--lua", help="frozen Lua launch script")
    parser.add_argument(
        "--save-template",
        help="baseline _SaveData directory copied into every fresh run root",
    )
    parser.add_argument("--rom-root", help="read-only game install root")
    parser.add_argument("--output", help="new output root for the complete matrix")
    return parser


def main(argv: Sequence[str]) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if arguments.process_scan_child:
        time.sleep(30)
        return 0
    if arguments.self_test:
        live_values = (
            arguments.binary,
            arguments.lua,
            arguments.save_template,
            arguments.rom_root,
            arguments.output,
        )
        if any(value is not None for value in live_values):
            parser.error("--self-test does not accept live-run arguments")
        try:
            _self_test_parser()
            _self_test_cutoff()
            _self_test_environment()
            _self_test_matrix_and_selector()
            _self_test_conflicts_and_cooldown()
            _self_test_process_scan()
            _self_test_decision()
        except (ABError, OSError, subprocess.SubprocessError) as error:
            print(f"phase6ao-ab-self-test: FAIL: {error}", file=sys.stderr)
            return 1
        print("phase6ao-ab-self-test: PASS")
        return 0
    required = {
        "--binary": arguments.binary,
        "--lua": arguments.lua,
        "--save-template": arguments.save_template,
        "--rom-root": arguments.rom_root,
        "--output": arguments.output,
    }
    missing = [name for name, value in required.items() if value is None]
    if missing:
        parser.error("the following arguments are required: " + ", ".join(missing))
    try:
        return run_matrix(arguments)
    except (ABError, OSError, UnicodeError, subprocess.SubprocessError) as error:
        print(f"phase6ao-ab: INVALID: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
