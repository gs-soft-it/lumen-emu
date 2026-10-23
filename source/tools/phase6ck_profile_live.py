#!/usr/bin/env python3
"""Run and immutably seal one manual phase-6CK profile diagnostic on macOS.

The tool never sends controller, keyboard, mouse, AppleScript, or debugger
input.  Its only runtime signal is SIGUSR2, used by the phase-6CK profiler to
emit the four snapshots bound to the operator's Fort Mode and in-game
announcements.  A sealed result is diagnostic evidence only: it is never a
persistent performance baseline or a promotion record.
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import decimal
import errno
import fcntl
import hashlib
import json
import os
import pathlib
import pwd
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Callable, Dict, Iterable, Iterator, List, Mapping, Optional, Sequence, Tuple

import phase6ck_profile_report as profile_report


RUN_SCHEMA = "lumen.phase6ck-profile-live-run/1"
RECEIPT_SCHEMA = "lumen.phase6ck-profile-live-receipt/1"
CHECKPOINT_SCHEMA = "lumen.phase6ck-profile-live-checkpoint/1"
MANIFEST_SCHEMA = "lumen.phase6ck-profile-live-manifest/1"
DIAGNOSTIC_SCOPE = profile_report.DIAGNOSTIC_SCOPE
CHECKPOINT_PRESENT = 2300
WINDOW_SIZE = 120
SIGNAL_TIMEOUT_SECONDS = 30.0
ZERO = decimal.Decimal(0)
LAUNCH_CLOCK_SEMANTICS = "pre_popen_diagnostic_only_not_baseline_loading_metric"

HEX_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
PID_RE = re.compile(r"^[1-9][0-9]*$")
ERROR_PATTERNS = {
    "fatal_count": re.compile(r"\bfatal\b(?!\s*=\s*0\b)", re.IGNORECASE),
    "assert_count": re.compile(r"assertion failed", re.IGNORECASE),
    "dynarec_unhealthy_count": re.compile(
        r"a64 dynarec disabled|dynarec unhealthy|healthy=0|status=(?:unhealthy|invalid)",
        re.IGNORECASE,
    ),
    "descriptor_mismatch_count": re.compile(
        r"gfx_descriptor_mismatch|descriptor mismatch", re.IGNORECASE
    ),
    "device_lost_count": re.compile(
        r"device[-_. ]lost|memory fault|segmentation fault", re.IGNORECASE
    ),
}

FORT_ANCHOR_ATTESTATIONS = frozenset(
    {
        "CONFIRM_NO_INPUT_BEFORE_P2300",
        "CONFIRM_FORT_MODE_ANNOUNCEMENT",
        "CONFIRM_MANUAL_INPUT_ONLY",
    }
)
FORT_COMPLETE_ATTESTATIONS = frozenset(
    {
        "CONFIRM_SAME_FORT_MODE_SCENE",
        "CONFIRM_ZERO_INPUT_SINCE_FORT_ANNOUNCEMENT",
    }
)
GAME_ANCHOR_ATTESTATIONS = frozenset(
    {
        "CONFIRM_NAVIGATION_OBJECTIVE_BOGGY_100HP_HUD_TERRAIN",
        "CONFIRM_TIMER_ADVANCED",
        "CONFIRM_MANUAL_INPUT_ONLY",
    }
)
GAME_COMPLETE_ATTESTATIONS = frozenset(
    {
        "CONFIRM_SAME_NAVIGATION_SCENE",
        "CONFIRM_ZERO_INPUT_SINCE_INGAME_ANNOUNCEMENT",
    }
)

LIVE_ENVIRONMENT = {
    "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
    "TMPDIR": "/tmp",
    "LANG": "C",
    "LC_ALL": "C",
    "LUMEN_X64_A64_DYNAREC": "1",
    "LUMEN_X64_A64_DYNAREC_BATCH_DIRECT_RETIREMENT": "1",
    "LUMEN_X64_A64_DYNAREC_DIRECT_LEAF_ABI": "1",
    "LUMEN_X64_A64_DYNAREC_DIRECT_CONTEXT_REUSE": "1",
    "LUMEN_X64_A64_DYNAREC_DIRECT_BLOCK_BINDING": "0",
    "LUMEN_X64_A64_DYNAREC_COMPACT_IMMEDIATES": "0",
    "LUMEN_X64_A64_DYNAREC_PACKED_256": "0",
    "LUMEN_X64_A64_DYNAREC_BLOCK_RFLAGS_CACHE": "0",
    "LUMEN_X64_A64_DYNAREC_BLOCK_GPR_CACHE": "0",
    "LUMEN_X64_A64_DYNAREC_MULTI_SEGMENT_ARTIFACT": "0",
    "LUMEN_X64_A64_DYNAREC_DENSE_RESIDENT_CACHE": "1",
    "LUMEN_X64_A64_DYNAREC_DENSE_VICTIM_ARCHIVE": "0",
    "LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_COMPILATION": "0",
    "LUMEN_X64_A64_DYNAREC_DENSE_ASYNC_STABILITY_ADMISSION": "0",
    "LUMEN_X64_A64_DYNAREC_HYBRID_DIRECT_ENTRY": "0",
    "LUMEN_X64_A64_DYNAREC_CONTEXTLESS_DENSE_ENTRY": "0",
    "LUMEN_X64_A64_DYNAREC_DENSE_PROFILE": "1",
    "LUMEN_GFX_COMMAND_PROVENANCE": "0",
    "LUMEN_GFX_ROOT_COMMAND_OWNERSHIP": "0",
    "LUMEN_GFX_NULL_SAMPLED_IMAGE": "1",
    "LUMEN_FRAME_PIPELINE_TRACE": "1",
    "LUMEN_FRAME_CHECKPOINT_PRESENT": "2300",
    "LUMEN_SHOW_WINDOW_EARLY": "1",
}


class LiveError(Exception):
    """The diagnostic launch, sidecar receipt, or lifecycle is not trustworthy."""


class _DuplicateKeyError(ValueError):
    pass


def _require(condition: bool, source: object, message: str) -> None:
    if not condition:
        raise LiveError(f"{source}: {message}")


def _uptime_raw_ns() -> int:
    _require(
        hasattr(time, "CLOCK_UPTIME_RAW"), "clock", "CLOCK_UPTIME_RAW is unavailable"
    )
    return time.clock_gettime_ns(time.CLOCK_UPTIME_RAW)


def _sha_file(path: pathlib.Path) -> str:
    _require(path.is_file() and not path.is_symlink(), path, "expected a regular file")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise LiveError(f"{path}: cannot hash file: {error}") from error
    return digest.hexdigest()


def _canonical_bytes(value: Any) -> bytes:
    try:
        return (
            json.dumps(
                value,
                ensure_ascii=False,
                allow_nan=False,
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n"
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise LiveError(f"value is not strict JSON: {error}") from error


def _pretty_bytes(value: Any) -> bytes:
    try:
        return (
            json.dumps(
                value,
                ensure_ascii=False,
                allow_nan=False,
                sort_keys=True,
                indent=2,
            )
            + "\n"
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise LiveError(f"value is not strict JSON: {error}") from error


def _publish_exclusive(path: pathlib.Path, payload: bytes, mode: int = 0o444) -> None:
    _require(path.parent.is_dir(), path, "parent directory is missing")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor: Optional[int] = None
    created = False
    try:
        descriptor = os.open(path, flags, 0o600)
        created = True
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise OSError(errno.EIO, "zero-length write")
            view = view[written:]
        os.fsync(descriptor)
        os.fchmod(descriptor, mode)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = None
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except OSError as error:
        if descriptor is not None:
            try:
                os.close(descriptor)
            except OSError:
                pass
        if created:
            try:
                path.unlink()
            except OSError:
                pass
        raise LiveError(f"{path}: exclusive publication failed: {error}") from error


def _publish_json(path: pathlib.Path, value: Any) -> None:
    _publish_exclusive(path, _pretty_bytes(value))


def _object_without_duplicates(pairs: Sequence[Tuple[str, Any]]) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateKeyError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _load_json(path: pathlib.Path) -> Mapping[str, Any]:
    _require(path.is_file() and not path.is_symlink(), path, "JSON file is missing")
    try:
        value = json.loads(
            path.read_text(encoding="utf-8", errors="strict"),
            object_pairs_hook=_object_without_duplicates,
            parse_constant=lambda token: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON constant {token}")
            ),
        )
    except (OSError, UnicodeError, ValueError, _DuplicateKeyError) as error:
        raise LiveError(f"{path}: invalid strict JSON: {error}") from error
    _require(type(value) is dict, path, "JSON root must be an object")
    return value


def _tree_digest(root: pathlib.Path) -> str:
    _require(root.is_dir() and not root.is_symlink(), root, "expected a directory")
    records: List[Mapping[str, Any]] = []
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix()
        _require(not path.is_symlink(), path, "symlinks are forbidden")
        if path.is_dir():
            records.append({"path": relative, "type": "directory"})
        elif path.is_file():
            records.append(
                {
                    "path": relative,
                    "type": "file",
                    "size": path.stat().st_size,
                    "sha256": _sha_file(path),
                }
            )
        else:
            raise LiveError(f"{path}: unsupported tree entry")
    return hashlib.sha256(_canonical_bytes(records)).hexdigest()


def _copy_tree(source: pathlib.Path, target: pathlib.Path) -> None:
    _require(source.is_dir() and not source.is_symlink(), source, "save template is invalid")
    for path in source.rglob("*"):
        _require(not path.is_symlink(), path, "save-template symlinks are forbidden")
    try:
        shutil.copytree(source, target, copy_function=shutil.copy2)
    except OSError as error:
        raise LiveError(f"{source}: cannot clone save template: {error}") from error
    # The authoritative template may be archived 0444/0555.  The isolated
    # clone must still admit ordinary guest autosaves without making the
    # template itself writable.
    for path in sorted(target.rglob("*"), key=lambda item: len(item.parts)):
        _require(not path.is_symlink(), path, "save-clone symlinks are forbidden")
        mode = stat.S_IMODE(path.stat().st_mode)
        if path.is_dir():
            os.chmod(path, mode | stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        elif path.is_file():
            os.chmod(path, mode | stat.S_IRUSR | stat.S_IWUSR)
        else:
            raise LiveError(f"{path}: unsupported save-clone entry")
    target_mode = stat.S_IMODE(target.stat().st_mode)
    os.chmod(
        target,
        target_mode | stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR,
    )


def _file_description(path: pathlib.Path) -> str:
    try:
        completed = subprocess.run(
            ("/usr/bin/file", "-b", str(path)),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            text=True,
            encoding="utf-8",
            errors="strict",
        )
    except OSError as error:
        raise LiveError(f"cannot inspect {path}: {error}") from error
    _require(completed.returncode == 0, path, "file(1) failed")
    return completed.stdout.strip()


def _contains(path: pathlib.Path, needle: bytes) -> bool:
    overlap = max(0, len(needle) - 1)
    previous = b""
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                return False
            value = previous + chunk
            if needle in value:
                return True
            previous = value[-overlap:] if overlap else b""


def _process_executable(pid: int) -> pathlib.Path:
    _require(sys.platform == "darwin", pid, "process identity requires macOS libproc")
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_pidpath.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    length = library.proc_pidpath(pid, buffer, len(buffer))
    if length <= 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return pathlib.Path(os.fsdecode(buffer.value)).resolve(strict=True)


def _process_names(name: str, *, test_mode: bool = False) -> Sequence[int]:
    if test_mode:
        return []
    try:
        completed = subprocess.run(
            ("/usr/bin/pgrep", "-x", name),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
            text=True,
            encoding="ascii",
            errors="strict",
        )
    except OSError as error:
        raise LiveError(f"cannot check residual process: {error}") from error
    _require(completed.returncode in (0, 1), name, "pgrep failed")
    if completed.returncode == 1:
        return []
    return [int(line) for line in completed.stdout.splitlines() if line]


def _read_pid(run_root: pathlib.Path) -> int:
    path = run_root / "runner.pid"
    try:
        text = path.read_text(encoding="ascii", errors="strict")
    except (OSError, UnicodeError) as error:
        raise LiveError(f"{path}: cannot read PID: {error}") from error
    _require(text.endswith("\n") and PID_RE.fullmatch(text[:-1]) is not None, path, "PID is not canonical")
    return int(text[:-1], 10)


def _require_live_process(run_root: pathlib.Path, *, test_mode: bool = False) -> int:
    pid = _read_pid(run_root)
    if test_mode:
        return pid
    try:
        os.kill(pid, 0)
    except OSError as error:
        raise LiveError(f"{run_root}: emulator PID {pid} is not alive: {error}") from error
    freeze = _load_json(run_root / "freeze.json")
    expected = pathlib.Path(str(freeze["binary_path"])).resolve(strict=True)
    try:
        actual = _process_executable(pid)
    except OSError as error:
        raise LiveError(f"{run_root}: cannot identify emulator PID {pid}: {error}") from error
    _require(actual == expected, run_root, "recorded PID executable identity changed")
    return pid


@contextlib.contextmanager
def _sidecar_lock(run_root: pathlib.Path) -> Iterator[None]:
    path = run_root / ".sidecar.lock"
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def _complete_lines(path: pathlib.Path) -> Tuple[List[str], int]:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise LiveError(f"{path}: cannot read live log: {error}") from error
    complete = payload[: payload.rfind(b"\n") + 1] if b"\n" in payload else b""
    try:
        text = complete.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise LiveError(f"{path}: live log is not strict UTF-8: {error}") from error
    lines = text.splitlines()
    _require(all(not line.endswith("\r") for line in lines), path, "CRLF log is not canonical")
    return lines, len(payload)


def _frames(path: pathlib.Path) -> Mapping[int, profile_report.FrameWindow]:
    lines, _ = _complete_lines(path)
    result: Dict[int, profile_report.FrameWindow] = {}
    ordered: List[int] = []
    for line_number, line in enumerate(lines, start=1):
        if not line.startswith("[FRAME_PIPELINE] present="):
            continue
        try:
            frame = profile_report._parse_pipeline(line, f"{path}: line {line_number}", line_number)
        except profile_report.ReportError as error:
            raise LiveError(str(error)) from error
        _require(frame.present not in result, path, "duplicate FRAME_PIPELINE endpoint")
        result[frame.present] = frame
        ordered.append(frame.present)
    _require(bool(ordered) and ordered[0] == 1, path, "missing p1 renderer record")
    for index, present in enumerate(ordered[1:], start=1):
        _require(present == index * WINDOW_SIZE, path, "p120 renderer sequence has a gap")
    return result


def _profile_headers(path: pathlib.Path) -> Sequence[Mapping[str, Any]]:
    lines, _ = _complete_lines(path)
    result: List[Mapping[str, Any]] = []
    for line_number, line in enumerate(lines, start=1):
        if not line.startswith(profile_report.PROFILE_PREFIX):
            continue
        try:
            value = profile_report._parse_profile(
                line, f"{path}: line {line_number}", line_number
            )
        except profile_report.ReportError as error:
            raise LiveError(str(error)) from error
        result.append(value)
    _require(
        [value["sequence"] for value in result] == list(range(1, len(result) + 1)),
        path,
        "live PROFILE sequences are not contiguous from one",
    )
    return result


def _last_stable_endpoint(
    path: pathlib.Path, clock: Callable[[], int]
) -> Tuple[int, int, int, int, int]:
    before_frames = _frames(path)
    _, size_before = _complete_lines(path)
    endpoint_before = max(before_frames)
    receipt = clock()
    after_frames = _frames(path)
    _, size_after = _complete_lines(path)
    endpoint_after = max(after_frames)
    _require(
        endpoint_before == endpoint_after
        and endpoint_before > CHECKPOINT_PRESENT
        and endpoint_before % WINDOW_SIZE == 0,
        path,
        "announcement L is not a stable post-p2300 p120 endpoint",
    )
    return endpoint_before, endpoint_after, receipt, size_before, size_after


def _validate_png_sources(paths: Sequence[pathlib.Path], minimum: int, source: object) -> Sequence[Tuple[pathlib.Path, str, int]]:
    _require(len(paths) >= minimum, source, f"at least {minimum} PNG evidence file(s) required")
    result: List[Tuple[pathlib.Path, str, int]] = []
    for raw in paths:
        path = raw.resolve(strict=True)
        _require(path.is_file() and not path.is_symlink(), path, "PNG evidence is missing")
        with path.open("rb") as stream:
            _require(stream.read(8) == b"\x89PNG\r\n\x1a\n", path, "evidence is not PNG")
        result.append((path, _sha_file(path), path.stat().st_size))
    _require(len({digest for _, digest, _ in result}) == len(result), source, "evidence PNGs must be distinct")
    return result


def _publish_evidence(
    run_root: pathlib.Path,
    scene: str,
    stage: str,
    sources: Sequence[Tuple[pathlib.Path, str, int]],
) -> Sequence[Mapping[str, Any]]:
    evidence_root = run_root / "evidence"
    _require(
        evidence_root.is_dir() and not evidence_root.is_symlink(),
        evidence_root,
        "evidence directory is invalid",
    )
    result: List[Mapping[str, Any]] = []
    for index, (source, digest, size) in enumerate(sources, start=1):
        target = run_root / "evidence" / f"{scene}-{stage}-{index}.png"
        payload = source.read_bytes()
        _require(len(payload) == size and hashlib.sha256(payload).hexdigest() == digest, source, "evidence changed after validation")
        _publish_exclusive(target, payload)
        result.append(
            {
                "file": target.relative_to(run_root).as_posix(),
                "sha256": digest,
                "size": size,
            }
        )
    return result


def _validate_archived_evidence(
    run_root: pathlib.Path, value: object, minimum: int, source: object
) -> None:
    _require(type(value) is list and len(value) >= minimum, source, "archived evidence list is incomplete")
    digests: List[str] = []
    for index, item in enumerate(value, start=1):
        _require(type(item) is dict and set(item) == {"file", "sha256", "size"}, source, f"evidence {index} schema mismatch")
        relative = pathlib.PurePosixPath(item["file"])
        _require(not relative.is_absolute() and ".." not in relative.parts and relative.parts[0] == "evidence", source, "unsafe evidence path")
        path = run_root / pathlib.Path(*relative.parts)
        _require(path.is_file() and not path.is_symlink(), path, "archived evidence is missing")
        digest = item["sha256"]
        _require(type(digest) is str and HEX_SHA256_RE.fullmatch(digest) is not None, source, "invalid evidence hash")
        _require(type(item["size"]) is int and item["size"] == path.stat().st_size, source, "evidence size mismatch")
        with path.open("rb") as stream:
            _require(stream.read(8) == b"\x89PNG\r\n\x1a\n", path, "archived evidence is not PNG")
        _require(_sha_file(path) == digest, path, "archived evidence hash mismatch")
        digests.append(digest)
    _require(len(set(digests)) == len(digests), source, "archived evidence PNGs are not distinct")


def _signal_snapshot(
    run_root: pathlib.Path,
    expected_sequence: int,
    pid: int,
    clock: Callable[[], int],
    signal_sender: Callable[[int, int], None],
    timeout_seconds: float,
) -> Mapping[str, Any]:
    stderr_path = run_root / "stderr.log"
    before = _profile_headers(stderr_path)
    _require(len(before) == expected_sequence - 1, run_root, "unexpected pre-signal PROFILE count")
    sent = clock()
    try:
        signal_sender(pid, signal.SIGUSR2)
    except OSError as error:
        raise LiveError(f"{run_root}: SIGUSR2 delivery failed: {error}") from error
    deadline = time.monotonic() + timeout_seconds
    while True:
        headers = _profile_headers(stderr_path)
        if len(headers) > expected_sequence:
            raise LiveError(f"{run_root}: more than one PROFILE snapshot followed one sidecar signal")
        if len(headers) == expected_sequence:
            observed = clock()
            snapshot = headers[-1]
            _require(
                snapshot["sequence"] == expected_sequence
                and snapshot["reason"] == "signal"
                and snapshot["final"] == 0
                and snapshot["valid"] == 1
                and snapshot["pending_request"] == 0
                and snapshot["signal_owned"] == 1
                and snapshot["snapshot_consistent"] == 1
                and snapshot["accounting_consistent"] == 1,
                run_root,
                "new PROFILE record is not a clean SIGUSR2 snapshot",
            )
            _require(
                sent <= snapshot["uptime_raw_ns"] <= observed,
                run_root,
                "signal/snapshot/observation clocks are not ordered",
            )
            return {
                "signal_sent_uptime_raw_ns": sent,
                "signal_observed_uptime_raw_ns": observed,
                "sequence": snapshot["sequence"],
                "snapshot_present": snapshot["present"],
                "snapshot_uptime_raw_ns": snapshot["uptime_raw_ns"],
                "snapshot_checksum": snapshot["checksum"],
            }
        _require(_require_live_process(run_root) == pid, run_root, "emulator exited before signal snapshot")
        if time.monotonic() >= deadline:
            raise LiveError(f"{run_root}: timed out waiting for PROFILE sequence {expected_sequence}")
        time.sleep(0.02)


def _scene_paths(run_root: pathlib.Path, scene: str) -> Tuple[pathlib.Path, pathlib.Path]:
    return (
        run_root / "receipts" / f"{scene}-anchor.json",
        run_root / "receipts" / f"{scene}-complete.json",
    )


def anchor_scene(
    run_root: pathlib.Path,
    scene: str,
    attestations: Sequence[str],
    evidence_paths: Sequence[pathlib.Path],
    *,
    timer_before: Optional[int] = None,
    timer_after: Optional[int] = None,
    clock: Callable[[], int] = _uptime_raw_ns,
    signal_sender: Callable[[int, int], None] = os.kill,
    timeout_seconds: float = SIGNAL_TIMEOUT_SECONDS,
    test_mode: bool = False,
) -> pathlib.Path:
    run_root = run_root.resolve()
    _require(scene in ("fort", "in_game"), scene, "unsupported scene")
    required = FORT_ANCHOR_ATTESTATIONS if scene == "fort" else GAME_ANCHOR_ATTESTATIONS
    _require(frozenset(attestations) == required and len(attestations) == len(required), scene, "anchor attestations mismatch")
    checkpoint_path = run_root / "checkpoint-observation.json"
    _require(checkpoint_path.is_file(), run_root, "p2300 has not been observed")
    checkpoint = _load_json(checkpoint_path)
    _require(checkpoint.get("terminal") == 0 and checkpoint.get("present") == CHECKPOINT_PRESENT, run_root, "p2300 receipt is not non-terminal")
    if scene == "fort":
        _require(timer_before is None and timer_after is None, scene, "Fort anchor does not accept timer values")
        expected_sequence = 3
    else:
        _require(type(timer_before) is int and type(timer_after) is int and timer_after > timer_before >= 0, scene, "strictly advancing timer values are required")
        fort_anchor, fort_complete = _scene_paths(run_root, "fort")
        _require(fort_anchor.is_file() and fort_complete.is_file(), scene, "Fort receipts must complete first")
        expected_sequence = 5
    anchor_path, complete_path = _scene_paths(run_root, scene)
    _require(not anchor_path.exists() and not complete_path.exists(), scene, "scene receipt already exists")
    minimum = 2 if scene == "in_game" else 1
    sources = _validate_png_sources(evidence_paths, minimum, scene)
    pid = _require_live_process(run_root, test_mode=test_mode)
    with _sidecar_lock(run_root):
        _require(not anchor_path.exists(), scene, "anchor raced with another sidecar")
        before, after, receipt, size_before, size_after = _last_stable_endpoint(
            run_root / "stderr.log", clock
        )
        if scene == "in_game":
            fort = _load_json(_scene_paths(run_root, "fort")[1])
            _require(before > fort["measured_endpoint"], scene, "in-game L does not follow the Fort measurement")
        signal_value = _signal_snapshot(
            run_root,
            expected_sequence,
            pid,
            clock,
            signal_sender,
            timeout_seconds,
        )
        evidence = _publish_evidence(run_root, scene, "anchor", sources)
        value = {
            "schema": RECEIPT_SCHEMA,
            "scope": DIAGNOSTIC_SCOPE,
            "scene": scene,
            "stage": "anchor",
            "receipt_uptime_raw_ns": receipt,
            "endpoint_before": before,
            "endpoint_after": after,
            "discard_endpoint": before + WINDOW_SIZE,
            "measured_endpoint": before + 2 * WINDOW_SIZE,
            "stderr_size_before": size_before,
            "stderr_size_after": size_after,
            "attestations": sorted(attestations),
            "evidence": evidence,
            "timer_before": timer_before,
            "timer_after": timer_after,
            "signal": signal_value,
        }
        _publish_json(anchor_path, value)
    return anchor_path


def complete_scene(
    run_root: pathlib.Path,
    scene: str,
    attestations: Sequence[str],
    evidence_paths: Sequence[pathlib.Path],
    *,
    clock: Callable[[], int] = _uptime_raw_ns,
    signal_sender: Callable[[int, int], None] = os.kill,
    timeout_seconds: float = SIGNAL_TIMEOUT_SECONDS,
    test_mode: bool = False,
) -> pathlib.Path:
    run_root = run_root.resolve()
    _require(scene in ("fort", "in_game"), scene, "unsupported scene")
    required = FORT_COMPLETE_ATTESTATIONS if scene == "fort" else GAME_COMPLETE_ATTESTATIONS
    _require(frozenset(attestations) == required and len(attestations) == len(required), scene, "completion attestations mismatch")
    expected_sequence = 4 if scene == "fort" else 6
    anchor_path, complete_path = _scene_paths(run_root, scene)
    _require(anchor_path.is_file() and not complete_path.exists(), scene, "no unique pending scene receipt")
    sources = _validate_png_sources(evidence_paths, 1, scene)
    pid = _require_live_process(run_root, test_mode=test_mode)
    with _sidecar_lock(run_root):
        anchor = _load_json(anchor_path)
        frames = _frames(run_root / "stderr.log")
        discard = anchor["discard_endpoint"]
        measured = anchor["measured_endpoint"]
        _require(discard in frames and measured in frames, scene, "discard and first-full p120 endpoints are not both complete")
        interval = frames[measured].avg_interval_ms
        _require(interval > ZERO, scene, "first-full p120 interval is not positive")
        receipt = clock()
        signal_value = _signal_snapshot(
            run_root,
            expected_sequence,
            pid,
            clock,
            signal_sender,
            timeout_seconds,
        )
        evidence = _publish_evidence(run_root, scene, "complete", sources)
        value = {
            "schema": RECEIPT_SCHEMA,
            "scope": DIAGNOSTIC_SCOPE,
            "scene": scene,
            "stage": "complete",
            "receipt_uptime_raw_ns": receipt,
            "discard_endpoint": discard,
            "measured_endpoint": measured,
            "avg_interval_ms": format(interval, "f"),
            "attestations": sorted(attestations),
            "evidence": evidence,
            "signal": signal_value,
        }
        _publish_json(complete_path, value)
    return complete_path


def _observe_checkpoint(
    run_root: pathlib.Path, launch_ns: int, already_seen: bool
) -> bool:
    lines, _ = _complete_lines(run_root / "stderr.log")
    checkpoint_lines = [line for line in lines if line.startswith("[FRAME_CHECKPOINT]")]
    _require(len(checkpoint_lines) <= 1, run_root, "p2300 checkpoint is duplicated")
    if not checkpoint_lines:
        return already_seen
    try:
        checkpoint = profile_report._parse_checkpoint(
            checkpoint_lines[0], run_root / "stderr.log", 0
        )
    except profile_report.ReportError as error:
        raise LiveError(str(error)) from error
    _require(
        checkpoint.present == CHECKPOINT_PRESENT
        and checkpoint.target == CHECKPOINT_PRESENT
        and checkpoint.terminal == 0,
        run_root,
        "p2300 checkpoint is not exact and non-terminal",
    )
    if not already_seen:
        observed = _uptime_raw_ns()
        _require(observed > launch_ns, run_root, "checkpoint observation clock is invalid")
        _publish_json(
            run_root / "checkpoint-observation.json",
            {
                "schema": CHECKPOINT_SCHEMA,
                "present": checkpoint.present,
                "target": checkpoint.target,
                "terminal": checkpoint.terminal,
                "launch_uptime_raw_ns": launch_ns,
                "observed_uptime_raw_ns": observed,
                "wall_ns": observed - launch_ns,
                "line_sha256": hashlib.sha256(
                    (checkpoint_lines[0] + "\n").encode("utf-8")
                ).hexdigest(),
            },
        )
        print(
            f"phase6ck-profile-live: CHECKPOINT_OBSERVED run={run_root} p2300 terminal=0",
            flush=True,
        )
    return True


def start_run(arguments: argparse.Namespace) -> pathlib.Path:
    _require(sys.platform == "darwin", "platform", "live phase-6CK diagnostics require macOS")
    _require(
        signal.getsignal(signal.SIGUSR2) == signal.SIG_DFL,
        "SIGUSR2",
        "runner disposition must be SIG_DFL before launch",
    )
    if hasattr(signal, "pthread_sigmask"):
        inherited_mask = signal.pthread_sigmask(signal.SIG_BLOCK, [])
        _require(
            signal.SIGUSR2 not in inherited_mask,
            "SIGUSR2",
            "runner inherited a blocked profiling signal",
        )
    binary = pathlib.Path(arguments.binary).resolve(strict=True)
    lua = pathlib.Path(arguments.lua).resolve(strict=True)
    save_template = pathlib.Path(arguments.save_template).resolve(strict=True)
    rom_root = pathlib.Path(arguments.rom_root).resolve(strict=True)
    run_root = pathlib.Path(arguments.run).resolve()
    _require(binary.is_file() and os.access(binary, os.X_OK), binary, "binary is not executable")
    _require(lua.is_file() and not lua.is_symlink(), lua, "Lua launch file is invalid")
    _require(save_template.is_dir() and not save_template.is_symlink(), save_template, "save template is invalid")
    _require(rom_root.is_dir() and not rom_root.is_symlink(), rom_root, "ROM root is invalid")
    eboot = rom_root / "eboot.bin"
    param = rom_root / "sce_sys" / "param.sfo"
    _require(eboot.is_file() and param.is_file(), rom_root, "ROM root lacks eboot.bin or param.sfo")
    description = _file_description(binary)
    _require("Mach-O 64-bit executable arm64" in description and "universal" not in description, binary, "binary is not thin arm64")
    _require(_contains(binary, b"LUMEN_X64_A64_DYNAREC_DENSE_PROFILE") and _contains(binary, b"[A64_DENSE_PHASE6CK_PROFILE]"), binary, "binary lacks phase-6CK profile selector or markers")
    _require(not _process_names(binary.name), binary, "another emulator process with this name is running")
    _require(not run_root.exists(), run_root, "run output already exists")
    run_root.parent.mkdir(parents=True, exist_ok=True)
    run_root.mkdir(mode=0o700)
    for name in ("receipts", "evidence", "work"):
        (run_root / name).mkdir(mode=0o700)
    _copy_tree(save_template, run_root / "work" / "_SaveData")
    special_relative = pathlib.PurePosixPath(arguments.save_special_relative)
    _require(not special_relative.is_absolute() and ".." not in special_relative.parts, arguments.save_special_relative, "unsafe Special save path")
    special = run_root / "work" / "_SaveData" / pathlib.Path(*special_relative.parts)
    _require(special.is_file() and not special.is_symlink(), special, "Special save is missing")
    rom_files = [eboot, param]
    shader = rom_root / "Orbis" / "DataPS4" / "Bundles" / "ShaderProgLibrary.xom"
    if shader.is_file():
        rom_files.append(shader)
    identity = pwd.getpwuid(os.getuid())
    home = pathlib.Path(identity.pw_dir).resolve()
    environment = {
        **LIVE_ENVIRONMENT,
        "HOME": str(home),
        "USER": identity.pw_name,
        "LOGNAME": identity.pw_name,
    }
    _require("LUMEN_FRAME_PRESENT_LIMIT" not in environment, run_root, "terminal frame limit leaked into environment")
    runner_path = pathlib.Path(__file__).resolve()
    report_path = pathlib.Path(profile_report.__file__).resolve()
    freeze = {
        "schema": RUN_SCHEMA,
        "scope": DIAGNOSTIC_SCOPE,
        "binary_path": str(binary),
        "binary_sha256": _sha_file(binary),
        "binary_file": description,
        "lua_path": str(lua),
        "lua_sha256": _sha_file(lua),
        "rom_root": str(rom_root),
        "rom_files": [
            {"path": str(path), "sha256": _sha_file(path), "size": path.stat().st_size}
            for path in rom_files
        ],
        "save_template_path": str(save_template),
        "save_template_sha256": _tree_digest(save_template),
        "save_special_relative": special_relative.as_posix(),
        "save_special_sha256": _sha_file(special),
        "runner_sha256": _sha_file(runner_path),
        "report_tool_sha256": _sha_file(report_path),
        "launch_clock_semantics": LAUNCH_CLOCK_SEMANTICS,
        "loading_metric_eligible": False,
        "environment": environment,
        "command": [str(binary), str(lua)],
    }
    _publish_json(run_root / "freeze.json", freeze)
    launch_ns = _uptime_raw_ns()
    process: Optional[subprocess.Popen[bytes]] = None
    checkpoint_seen = False
    return_code: Optional[int] = None
    stdout_path = run_root / "stdout.log"
    stderr_path = run_root / "stderr.log"
    with stdout_path.open("xb") as stdout_log, stderr_path.open("xb") as stderr_log:
        try:
            process = subprocess.Popen(
                (str(binary), str(lua)),
                cwd=run_root / "work",
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=stdout_log,
                stderr=stderr_log,
            )
            _publish_exclusive(run_root / "runner.pid", f"{process.pid}\n".encode("ascii"))
            _publish_json(
                run_root / "launch.json",
                {
                    "schema": RUN_SCHEMA,
                    "scope": DIAGNOSTIC_SCOPE,
                    "launch_uptime_raw_ns": launch_ns,
                    "launch_clock_semantics": LAUNCH_CLOCK_SEMANTICS,
                    "loading_metric_eligible": False,
                    "pid": process.pid,
                },
            )
            print(
                f"phase6ck-profile-live: RUNNING run={run_root} pid={process.pid} manual_input_only=1",
                flush=True,
            )
            while process.poll() is None:
                checkpoint_seen = _observe_checkpoint(run_root, launch_ns, checkpoint_seen)
                time.sleep(0.02)
            return_code = process.wait()
            checkpoint_seen = _observe_checkpoint(run_root, launch_ns, checkpoint_seen)
        except BaseException:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    return_code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    return_code = process.wait(timeout=5)
            raise
    exit_ns = _uptime_raw_ns()
    _publish_exclusive(
        run_root / ".exit_status", f"{return_code if return_code is not None else 125}\n".encode("ascii")
    )
    _publish_exclusive(run_root / "exit-uptime-raw-ns.txt", f"{exit_ns}\n".encode("ascii"))
    _require(checkpoint_seen, run_root, "process exited without p2300 observation")
    print(
        f"phase6ck-profile-live: EXITED run={run_root} status={return_code}",
        flush=True,
    )
    return run_root


def _count_exact_line(texts: Iterable[str], expected: str) -> int:
    return sum(1 for text in texts for line in text.splitlines() if line == expected)


def _uint(value: object, source: object, field: str) -> int:
    _require(
        type(value) is int and 0 <= value <= profile_report.UINT64_MAX,
        source,
        f"{field} must be uint64",
    )
    assert isinstance(value, int)
    return value


def _receipt_stage(
    run_root: pathlib.Path,
    scene: str,
    stage: str,
    snapshot: profile_report.Snapshot,
) -> Mapping[str, Any]:
    path = _scene_paths(run_root, scene)[0 if stage == "anchor" else 1]
    receipt = _load_json(path)
    expected_keys = (
        {
            "schema",
            "scope",
            "scene",
            "stage",
            "receipt_uptime_raw_ns",
            "endpoint_before",
            "endpoint_after",
            "discard_endpoint",
            "measured_endpoint",
            "stderr_size_before",
            "stderr_size_after",
            "attestations",
            "evidence",
            "timer_before",
            "timer_after",
            "signal",
        }
        if stage == "anchor"
        else {
            "schema",
            "scope",
            "scene",
            "stage",
            "receipt_uptime_raw_ns",
            "discard_endpoint",
            "measured_endpoint",
            "avg_interval_ms",
            "attestations",
            "evidence",
            "signal",
        }
    )
    _require(set(receipt) == expected_keys and receipt["schema"] == RECEIPT_SCHEMA and receipt["scope"] == DIAGNOSTIC_SCOPE and receipt["scene"] == scene and receipt["stage"] == stage, path, "receipt schema mismatch")
    minimum = 2 if scene == "in_game" and stage == "anchor" else 1
    _validate_archived_evidence(run_root, receipt["evidence"], minimum, path)
    receipt_ns = _uint(
        receipt["receipt_uptime_raw_ns"], path, "receipt_uptime_raw_ns"
    )
    _require(
        type(receipt["attestations"]) is list
        and all(type(item) is str for item in receipt["attestations"]),
        path,
        "attestations must be a string list",
    )
    signal_value = receipt["signal"]
    _require(type(signal_value) is dict and set(signal_value) == {"signal_sent_uptime_raw_ns", "signal_observed_uptime_raw_ns", "sequence", "snapshot_present", "snapshot_uptime_raw_ns", "snapshot_checksum"}, path, "signal receipt schema mismatch")
    sent = _uint(
        signal_value["signal_sent_uptime_raw_ns"],
        path,
        "signal_sent_uptime_raw_ns",
    )
    observed = _uint(
        signal_value["signal_observed_uptime_raw_ns"],
        path,
        "signal_observed_uptime_raw_ns",
    )
    sequence = _uint(signal_value["sequence"], path, "sequence")
    present = _uint(signal_value["snapshot_present"], path, "snapshot_present")
    snapshot_uptime = _uint(
        signal_value["snapshot_uptime_raw_ns"], path, "snapshot_uptime_raw_ns"
    )
    checksum = signal_value["snapshot_checksum"]
    _require(
        type(checksum) is str
        and profile_report.CHECKSUM_RE.fullmatch(checksum) is not None,
        path,
        "snapshot checksum is malformed",
    )
    _require(
        sequence == snapshot.sequence
        and present == snapshot.present
        and snapshot_uptime == snapshot.uptime_raw_ns
        and checksum == snapshot.checksum,
        path,
        "receipt is not bound to the selected snapshot",
    )
    _require(
        0 < receipt_ns <= sent <= snapshot.uptime_raw_ns <= observed,
        path,
        "receipt/signal clocks are not ordered",
    )
    return receipt


def _readonly_tree(root: pathlib.Path) -> None:
    for path in sorted(root.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        _require(not path.is_symlink(), path, "symlink in diagnostic archive")
        os.chmod(path, 0o555 if path.is_dir() else 0o444)
    os.chmod(root, 0o555)


def _require_no_symlinks(root: pathlib.Path) -> None:
    _require(root.is_dir() and not root.is_symlink(), root, "archive root is invalid")
    for path in root.rglob("*"):
        _require(not path.is_symlink(), path, "symlink in diagnostic archive")


def verify_archive(run_root: pathlib.Path) -> pathlib.Path:
    run_root = run_root.resolve()
    _require_no_symlinks(run_root)
    manifest_path = run_root / "diagnostic-manifest.json"
    manifest = _load_json(manifest_path)
    _require(
        set(manifest)
        == {
            "schema",
            "scope",
            "valid",
            "persistent_baseline_eligible",
            "diagnostic_gate_status",
            "profile_report_sha256",
            "launch_clock_semantics",
            "loading_metric_eligible",
            "artifacts_sha256",
            "evidence_index",
            "evidence_index_sha256",
            "lifecycle",
            "sealed_uptime_raw_ns",
        }
        and manifest["schema"] == MANIFEST_SCHEMA
        and manifest["scope"] == DIAGNOSTIC_SCOPE
        and manifest["valid"] is True
        and manifest["persistent_baseline_eligible"] is False
        and manifest["diagnostic_gate_status"] == "PASS_DIAGNOSTIC_ONLY"
        and manifest["launch_clock_semantics"] == LAUNCH_CLOCK_SEMANTICS
        and manifest["loading_metric_eligible"] is False,
        manifest_path,
        "manifest schema or diagnostic-only contract mismatch",
    )
    artifacts = manifest["artifacts_sha256"]
    _require(type(artifacts) is dict and bool(artifacts), manifest_path, "artifact index is empty")
    actual_files = {
        path.relative_to(run_root).as_posix()
        for path in run_root.rglob("*")
        if path.is_file() and path != manifest_path
    }
    _require(
        actual_files == set(artifacts),
        manifest_path,
        "archive file set differs from the manifest artifact index",
    )
    for relative_text, expected in artifacts.items():
        _require(
            type(relative_text) is str
            and type(expected) is str
            and HEX_SHA256_RE.fullmatch(expected) is not None,
            manifest_path,
            "artifact index entry is malformed",
        )
        relative = pathlib.PurePosixPath(relative_text)
        _require(
            not relative.is_absolute() and ".." not in relative.parts,
            manifest_path,
            "unsafe artifact path",
        )
        path = run_root / pathlib.Path(*relative.parts)
        _require(
            path.is_file()
            and not path.is_symlink()
            and _sha_file(path) == expected,
            path,
            "archived artifact hash mismatch",
        )
    report_sha = artifacts.get("profile-report.json")
    _require(
        report_sha == manifest["profile_report_sha256"],
        manifest_path,
        "profile report digest mismatch",
    )
    evidence_index = manifest["evidence_index"]
    _require(type(evidence_index) is list and bool(evidence_index), manifest_path, "evidence index is empty")
    expected_evidence = []
    for relative_text, digest in sorted(artifacts.items()):
        if not relative_text.startswith("evidence/"):
            continue
        path = run_root / pathlib.Path(*pathlib.PurePosixPath(relative_text).parts)
        expected_evidence.append(
            {"file": relative_text, "sha256": digest, "size": path.stat().st_size}
        )
    actual_evidence = {
        path.relative_to(run_root).as_posix()
        for path in (run_root / "evidence").glob("*.png")
    }
    _require(
        actual_evidence == {item["file"] for item in expected_evidence},
        manifest_path,
        "PNG evidence set differs from the manifest artifact index",
    )
    _require(evidence_index == expected_evidence, manifest_path, "evidence index differs from archived PNG artifacts")
    evidence_digest = hashlib.sha256(_canonical_bytes(evidence_index)).hexdigest()
    _require(
        evidence_digest == manifest["evidence_index_sha256"],
        manifest_path,
        "canonical evidence-index digest mismatch",
    )
    return manifest_path


def seal_run(
    run_root: pathlib.Path,
    no_glitch_attestation: str,
    *,
    test_mode: bool = False,
) -> pathlib.Path:
    run_root = run_root.resolve()
    _require(no_glitch_attestation == "CONFIRM_NO_GLITCH_OBSERVED", run_root, "explicit no-glitch attestation is required")
    _require_no_symlinks(run_root)
    freeze = _load_json(run_root / "freeze.json")
    _require(
        freeze.get("schema") == RUN_SCHEMA
        and freeze.get("scope") == DIAGNOSTIC_SCOPE
        and freeze.get("launch_clock_semantics") == LAUNCH_CLOCK_SEMANTICS
        and freeze.get("loading_metric_eligible") is False,
        run_root,
        "freeze schema/scope/diagnostic-clock contract mismatch",
    )
    for relative in (
        "stdout.log",
        "stderr.log",
        ".exit_status",
        "exit-uptime-raw-ns.txt",
        "checkpoint-observation.json",
        "receipts/fort-anchor.json",
        "receipts/fort-complete.json",
        "receipts/in_game-anchor.json",
        "receipts/in_game-complete.json",
    ):
        _require((run_root / relative).is_file(), run_root, f"missing {relative}")
    _require((run_root / ".exit_status").read_bytes() == b"0\n", run_root, "exit status must be byte-exact 0\\n")
    pid = _read_pid(run_root)
    if not test_mode:
        try:
            os.kill(pid, 0)
        except OSError:
            pass
        else:
            raise LiveError(f"{run_root}: recorded emulator PID remains alive")
    binary = pathlib.Path(str(freeze["binary_path"])).resolve(strict=True)
    _require(not _process_names(binary.name, test_mode=test_mode), run_root, "residual emulator process exists")
    runner_path = pathlib.Path(__file__).resolve()
    report_path = pathlib.Path(profile_report.__file__).resolve()
    _require(_sha_file(binary) == freeze["binary_sha256"], binary, "binary changed after launch")
    lua = pathlib.Path(str(freeze["lua_path"])).resolve(strict=True)
    _require(_sha_file(lua) == freeze["lua_sha256"], lua, "Lua changed after launch")
    _require(_sha_file(runner_path) == freeze["runner_sha256"] and _sha_file(report_path) == freeze["report_tool_sha256"], run_root, "live/report tool changed after launch")
    for item in freeze["rom_files"]:
        path = pathlib.Path(item["path"]).resolve(strict=True)
        _require(path.stat().st_size == item["size"] and _sha_file(path) == item["sha256"], path, "frozen ROM file changed")
    save_template = pathlib.Path(str(freeze["save_template_path"])).resolve(strict=True)
    _require(_tree_digest(save_template) == freeze["save_template_sha256"], save_template, "authoritative save template changed")
    special = run_root / "work" / "_SaveData" / pathlib.Path(*pathlib.PurePosixPath(freeze["save_special_relative"]).parts)
    _require(_sha_file(special) == freeze["save_special_sha256"], special, "Special save changed")

    try:
        stdout = (run_root / "stdout.log").read_text(encoding="utf-8", errors="strict")
        stderr = (run_root / "stderr.log").read_text(encoding="utf-8", errors="strict")
    except (OSError, UnicodeError) as error:
        raise LiveError(f"{run_root}: logs are not strict UTF-8: {error}") from error
    logs = (stdout, stderr)
    frame_limit_count = sum(
        1
        for text in logs
        for line in text.splitlines()
        if line.startswith("[FRAME_LIMIT]")
    )
    _require(frame_limit_count == 0, run_root, "FRAME_LIMIT appeared")
    sdl_quit_count = _count_exact_line(logs, "[WINDOW_EXIT] SDL_QUIT")
    _require(sdl_quit_count == 1, run_root, "SDL_QUIT is not exact-once")
    combined = stdout + "\n" + stderr
    error_counts = {name: len(pattern.findall(combined)) for name, pattern in ERROR_PATTERNS.items()}
    _require(all(count == 0 for count in error_counts.values()), run_root, "fatal/unhealthy/device-lost evidence appeared")

    try:
        profile = profile_report.parse_log(run_root / "stderr.log")
    except profile_report.ReportError as error:
        raise LiveError(str(error)) from error
    _require(len(profile.snapshots) == 7 and [snapshot.reason for snapshot in profile.snapshots] == ["start", "checkpoint", "signal", "signal", "signal", "signal", "final"], run_root, "profile stream is not exactly start/checkpoint/four-signals/final")
    final = profile.snapshots[-1]
    _require(final.final == final.valid == final.signal_owned == final.snapshot_consistent == final.accounting_consistent == 1 and final.pending_request == final.counters["invalid"] == 0, run_root, "profile final is not clean")
    checkpoint = _load_json(run_root / "checkpoint-observation.json")
    _require(set(checkpoint) == {"schema", "present", "target", "terminal", "launch_uptime_raw_ns", "observed_uptime_raw_ns", "wall_ns", "line_sha256"} and checkpoint["schema"] == CHECKPOINT_SCHEMA, run_root, "checkpoint observation schema mismatch")
    checkpoint_present = _uint(checkpoint["present"], run_root, "checkpoint present")
    checkpoint_target = _uint(checkpoint["target"], run_root, "checkpoint target")
    checkpoint_terminal = _uint(checkpoint["terminal"], run_root, "checkpoint terminal")
    launch_ns = _uint(checkpoint["launch_uptime_raw_ns"], run_root, "launch_uptime_raw_ns")
    checkpoint_observed_ns = _uint(checkpoint["observed_uptime_raw_ns"], run_root, "observed_uptime_raw_ns")
    checkpoint_wall_ns = _uint(checkpoint["wall_ns"], run_root, "checkpoint wall_ns")
    _require(checkpoint_present == checkpoint_target == CHECKPOINT_PRESENT and checkpoint_terminal == 0 and checkpoint_observed_ns > launch_ns and checkpoint_wall_ns == checkpoint_observed_ns - launch_ns, run_root, "checkpoint observation value or clock mismatch")
    _require(type(checkpoint["line_sha256"]) is str and HEX_SHA256_RE.fullmatch(checkpoint["line_sha256"]) is not None, run_root, "checkpoint line hash is malformed")
    checkpoint_lines = [line for line in stderr.splitlines() if line.startswith("[FRAME_CHECKPOINT]")]
    _require(len(checkpoint_lines) == 1 and checkpoint["line_sha256"] == hashlib.sha256((checkpoint_lines[0] + "\n").encode("utf-8")).hexdigest(), run_root, "checkpoint observation is not bound to the exact log line")

    fort_anchor = _receipt_stage(run_root, "fort", "anchor", profile.snapshots[2])
    fort_complete = _receipt_stage(run_root, "fort", "complete", profile.snapshots[3])
    game_anchor = _receipt_stage(run_root, "in_game", "anchor", profile.snapshots[4])
    game_complete = _receipt_stage(run_root, "in_game", "complete", profile.snapshots[5])
    _require(fort_anchor["attestations"] == sorted(FORT_ANCHOR_ATTESTATIONS) and fort_complete["attestations"] == sorted(FORT_COMPLETE_ATTESTATIONS) and game_anchor["attestations"] == sorted(GAME_ANCHOR_ATTESTATIONS) and game_complete["attestations"] == sorted(GAME_COMPLETE_ATTESTATIONS), run_root, "manual scene attestations mismatch")
    for scene, anchor, complete in (("fort", fort_anchor, fort_complete), ("in_game", game_anchor, game_complete)):
        endpoint_before = _uint(anchor["endpoint_before"], scene, "endpoint_before")
        endpoint_after = _uint(anchor["endpoint_after"], scene, "endpoint_after")
        discard = _uint(anchor["discard_endpoint"], scene, "discard_endpoint")
        measured = _uint(anchor["measured_endpoint"], scene, "measured_endpoint")
        _require(endpoint_before == endpoint_after and discard == endpoint_before + WINDOW_SIZE and measured == discard + WINDOW_SIZE, scene, "stable L/first-full geometry mismatch")
        _require(_uint(complete["discard_endpoint"], scene, "complete discard_endpoint") == discard and _uint(complete["measured_endpoint"], scene, "complete measured_endpoint") == measured, scene, "completion geometry differs from announcement")
        frames = _frames(run_root / "stderr.log")
        _require(type(complete["avg_interval_ms"]) is str, scene, "avg_interval_ms must be a string")
        try:
            interval = profile_report._parse_decimal(
                complete["avg_interval_ms"], scene, "avg_interval_ms"
            )
        except profile_report.ReportError as error:
            raise LiveError(str(error)) from error
        _require(discard in frames and measured in frames and interval == frames[measured].avg_interval_ms, scene, "first-full p120 measurement differs from raw log")
    _require(_uint(game_anchor["endpoint_before"], run_root, "game endpoint_before") > _uint(fort_anchor["measured_endpoint"], run_root, "Fort measured_endpoint"), run_root, "in-game announcement does not follow Fort measurement")
    _require(
        fort_anchor["timer_before"] is None
        and fort_anchor["timer_after"] is None,
        run_root,
        "Fort receipt unexpectedly contains timer values",
    )
    timer_before = _uint(game_anchor["timer_before"], run_root, "timer_before")
    timer_after = _uint(game_anchor["timer_after"], run_root, "timer_after")
    _require(timer_after > timer_before, run_root, "in-game timer did not advance")
    exit_text = (run_root / "exit-uptime-raw-ns.txt").read_text(
        encoding="ascii", errors="strict"
    )
    _require(
        exit_text.endswith("\n")
        and PID_RE.fullmatch(exit_text[:-1]) is not None,
        run_root,
        "exit CLOCK_UPTIME_RAW receipt is not canonical",
    )
    exit_ns = int(exit_text[:-1], 10)
    _require(checkpoint_observed_ns < fort_anchor["receipt_uptime_raw_ns"] < fort_complete["receipt_uptime_raw_ns"] < game_anchor["receipt_uptime_raw_ns"] < game_complete["receipt_uptime_raw_ns"] < final.uptime_raw_ns <= exit_ns, run_root, "checkpoint/sidecar/final/exit clocks are not ordered")

    def stage(receipt: Mapping[str, Any]) -> Mapping[str, Any]:
        return {
            "receipt_uptime_raw_ns": receipt["receipt_uptime_raw_ns"],
            **receipt["signal"],
            "evidence_verified": True,
        }

    def scene_binding(anchor: Mapping[str, Any], complete: Mapping[str, Any]) -> Mapping[str, Any]:
        return {
            "anchor": stage(anchor),
            "complete": stage(complete),
            "endpoint_before": anchor["endpoint_before"],
            "endpoint_after": anchor["endpoint_after"],
            "discard_endpoint": anchor["discard_endpoint"],
            "measured_endpoint": anchor["measured_endpoint"],
            "avg_interval_ms": complete["avg_interval_ms"],
            "manual_input_only_attested": True,
            "zero_input_window_attested": True,
            "oracle_attested": True,
        }

    live_binding = {
        "schema": profile_report.LIVE_BINDING_SCHEMA,
        "scope": DIAGNOSTIC_SCOPE,
        "checkpoint": {
            "count": 1,
            "terminal": 0,
            "observed_uptime_raw_ns": checkpoint_observed_ns,
        },
        "scenes": {
            "fort": scene_binding(fort_anchor, fort_complete),
            "in_game": scene_binding(game_anchor, game_complete),
        },
        "lifecycle": {
            "exit_status_bytes": "0\n",
            "sdl_quit_count": sdl_quit_count,
            **error_counts,
            "frame_limit_count": frame_limit_count,
            "signal_snapshot_count": 4,
            "final_sequence": final.sequence,
            "final_clean": True,
            "no_input_before_p2300_attested": True,
            "no_glitch_attested": True,
            "evidence_verified": True,
        },
    }
    try:
        report = profile_report.build_report(
            profile, (3, 4), (5, 6), live_binding=live_binding
        )
        report_payload = profile_report._serialize_report(report)
    except profile_report.ReportError as error:
        raise LiveError(str(error)) from error
    _require(report["diagnostic_gate"]["passed"] is True and report["diagnostic_gate"]["persistent_baseline_eligible"] is False, run_root, "report did not produce a diagnostic-only PASS")
    report_output = run_root / "profile-report.json"
    profile_report._publish_report_exclusive(report_output, report_payload)
    artifacts = {
        path.relative_to(run_root).as_posix(): _sha_file(path)
        for path in sorted(
            run_root.rglob("*"), key=lambda item: item.relative_to(run_root).as_posix()
        )
        if path.is_file()
    }
    evidence_index = [
        {
            "file": relative,
            "sha256": digest,
            "size": (run_root / pathlib.Path(*pathlib.PurePosixPath(relative).parts)).stat().st_size,
        }
        for relative, digest in sorted(artifacts.items())
        if relative.startswith("evidence/")
    ]
    _require(bool(evidence_index), run_root, "sealed archive has no PNG evidence")
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "scope": DIAGNOSTIC_SCOPE,
        "valid": True,
        "persistent_baseline_eligible": False,
        "diagnostic_gate_status": "PASS_DIAGNOSTIC_ONLY",
        "profile_report_sha256": hashlib.sha256(report_payload).hexdigest(),
        "launch_clock_semantics": LAUNCH_CLOCK_SEMANTICS,
        "loading_metric_eligible": False,
        "artifacts_sha256": artifacts,
        "evidence_index": evidence_index,
        "evidence_index_sha256": hashlib.sha256(
            _canonical_bytes(evidence_index)
        ).hexdigest(),
        "lifecycle": live_binding["lifecycle"],
        "sealed_uptime_raw_ns": _uptime_raw_ns(),
    }
    manifest_path = run_root / "diagnostic-manifest.json"
    _publish_json(manifest_path, manifest)
    _readonly_tree(run_root)
    verify_archive(run_root)
    return manifest_path


def _clock_values(*values: int) -> Callable[[], int]:
    iterator = iter(values)
    return lambda: next(iterator)


def self_test() -> None:
    with tempfile.TemporaryDirectory(prefix="phase6ck-profile-live-self-test-") as temporary:
        root = pathlib.Path(temporary)
        run_root = root / "run"
        for path in (
            run_root,
            run_root / "receipts",
            run_root / "evidence",
            run_root / "work",
        ):
            path.mkdir()
        binary = root / "fc_script"
        binary.write_bytes(b"fixture-binary")
        binary.chmod(0o755)
        lua = root / "run.lua"
        lua.write_text("fixture\n", encoding="utf-8")
        rom = root / "rom"
        (rom / "sce_sys").mkdir(parents=True)
        (rom / "eboot.bin").write_bytes(b"eboot")
        (rom / "sce_sys" / "param.sfo").write_bytes(b"sfo")
        save_template = root / "save-template"
        (save_template / "SAVEDATA0").mkdir(parents=True)
        (save_template / "SAVEDATA0" / "Special.0").write_bytes(b"special")
        (save_template / "SAVEDATA0" / "MainData.0").write_bytes(b"main-before")
        for path in save_template.rglob("*"):
            os.chmod(path, 0o555 if path.is_dir() else 0o444)
        os.chmod(save_template, 0o555)
        _copy_tree(save_template, run_root / "work" / "_SaveData")
        special = run_root / "work" / "_SaveData" / "SAVEDATA0" / "Special.0"
        main_data = run_root / "work" / "_SaveData" / "SAVEDATA0" / "MainData.0"
        _require(
            stat.S_IMODE(special.stat().st_mode) & stat.S_IWUSR
            and stat.S_IMODE(main_data.parent.stat().st_mode) & stat.S_IWUSR,
            "self-test",
            "readonly template did not produce an owner-writable clone",
        )
        main_data.write_bytes(b"autosave-after")
        _require(
            stat.S_IMODE((save_template / "SAVEDATA0" / "MainData.0").stat().st_mode)
            == 0o444
            and (save_template / "SAVEDATA0" / "MainData.0").read_bytes()
            == b"main-before",
            "self-test",
            "save clone mutation changed the readonly template",
        )
        freeze = {
            "schema": RUN_SCHEMA,
            "scope": DIAGNOSTIC_SCOPE,
            "binary_path": str(binary),
            "binary_sha256": _sha_file(binary),
            "lua_path": str(lua),
            "lua_sha256": _sha_file(lua),
            "rom_files": [
                {
                    "path": str(rom / "eboot.bin"),
                    "sha256": _sha_file(rom / "eboot.bin"),
                    "size": 5,
                }
            ],
            "save_template_path": str(save_template),
            "save_template_sha256": _tree_digest(save_template),
            "save_special_relative": "SAVEDATA0/Special.0",
            "save_special_sha256": _sha_file(special),
            "runner_sha256": _sha_file(pathlib.Path(__file__).resolve()),
            "report_tool_sha256": _sha_file(pathlib.Path(profile_report.__file__).resolve()),
            "launch_clock_semantics": LAUNCH_CLOCK_SEMANTICS,
            "loading_metric_eligible": False,
        }
        _publish_json(run_root / "freeze.json", freeze)
        _publish_exclusive(run_root / "runner.pid", f"{os.getpid()}\n".encode("ascii"))
        (run_root / "stdout.log").write_bytes(b"")

        config = profile_report.CONFIG_PREFIX + " ".join(
            f"{field}={profile_report.CONFIG_EXPECTED[field]}"
            for field in profile_report.CONFIG_FIELDS
        )
        lines = [config]
        lines.extend(profile_report._fixture_snapshot_lines(1, "start", 0, 0))
        lines.append(
            "[FRAME_PIPELINE] present=1 avg_interval=0.000 ms "
            "avg_latency=1.000 ms pending=0 index=0 arg=0"
        )
        for present in range(120, 2881, 120):
            lines.append(
                f"[FRAME_PIPELINE] present={present} avg_interval=10.000 ms "
                "avg_latency=2.000 ms pending=0 index=1 arg=0"
            )
            if present == 2280:
                checkpoint_line = (
                    "[FRAME_CHECKPOINT] present=2300 target=2300 "
                    "cumulative_ms=22990.000 cumulative_intervals=2299 "
                    "partial_ms=200.000 partial_intervals=20 terminal=0"
                )
                lines.append(checkpoint_line)
                lines.extend(
                    profile_report._fixture_snapshot_lines(
                        2, "checkpoint", 2300, 1
                    )
                )
        stderr_path = run_root / "stderr.log"
        stderr_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        _publish_json(
            run_root / "checkpoint-observation.json",
            {
                "schema": CHECKPOINT_SCHEMA,
                "present": 2300,
                "target": 2300,
                "terminal": 0,
                "launch_uptime_raw_ns": 1_500_000,
                "observed_uptime_raw_ns": 2_100_000,
                "wall_ns": 600_000,
                "line_sha256": hashlib.sha256(
                    (checkpoint_line + "\n").encode("utf-8")
                ).hexdigest(),
            },
        )
        png1 = root / "one.png"
        png2 = root / "two.png"
        png1.write_bytes(b"\x89PNG\r\n\x1a\nfixture-one")
        png2.write_bytes(b"\x89PNG\r\n\x1a\nfixture-two")

        def append_signal(_pid: int, _sig: int) -> None:
            sequence = len(_profile_headers(stderr_path)) + 1
            presents = {3: 3000, 4: 3120, 5: 4000, 6: 4120}
            with stderr_path.open("a", encoding="utf-8") as output:
                output.write(
                    "\n".join(
                        profile_report._fixture_snapshot_lines(
                            sequence, "signal", presents[sequence], sequence - 1
                        )
                    )
                    + "\n"
                )

        anchor_scene(
            run_root,
            "fort",
            sorted(FORT_ANCHOR_ATTESTATIONS),
            [png1],
            clock=_clock_values(2_200_000, 2_300_000, 3_100_000),
            signal_sender=append_signal,
            test_mode=True,
        )
        with stderr_path.open("a", encoding="utf-8") as output:
            for present in (3000, 3120):
                output.write(
                    f"[FRAME_PIPELINE] present={present} avg_interval=10.000 ms "
                    "avg_latency=2.000 ms pending=0 index=1 arg=0\n"
                )
        complete_scene(
            run_root,
            "fort",
            sorted(FORT_COMPLETE_ATTESTATIONS),
            [png1],
            clock=_clock_values(3_200_000, 3_300_000, 4_100_000),
            signal_sender=append_signal,
            test_mode=True,
        )
        with stderr_path.open("a", encoding="utf-8") as output:
            for present in range(3240, 3841, 120):
                output.write(
                    f"[FRAME_PIPELINE] present={present} avg_interval=10.000 ms "
                    "avg_latency=2.000 ms pending=0 index=1 arg=0\n"
                )
        anchor_scene(
            run_root,
            "in_game",
            sorted(GAME_ANCHOR_ATTESTATIONS),
            [png1, png2],
            timer_before=100,
            timer_after=101,
            clock=_clock_values(4_200_000, 4_300_000, 5_100_000),
            signal_sender=append_signal,
            test_mode=True,
        )
        with stderr_path.open("a", encoding="utf-8") as output:
            for present in (3960, 4080):
                output.write(
                    f"[FRAME_PIPELINE] present={present} avg_interval=10.000 ms "
                    "avg_latency=2.000 ms pending=0 index=1 arg=0\n"
                )
        complete_scene(
            run_root,
            "in_game",
            sorted(GAME_COMPLETE_ATTESTATIONS),
            [png1],
            clock=_clock_values(5_200_000, 5_300_000, 6_100_000),
            signal_sender=append_signal,
            test_mode=True,
        )
        with stderr_path.open("a", encoding="utf-8") as output:
            output.write(
                "\n".join(
                    profile_report._fixture_snapshot_lines(7, "final", 0, 6)
                )
                + "\n[WINDOW_EXIT] SDL_QUIT\n"
            )
        _publish_exclusive(run_root / ".exit_status", b"0\n")
        _publish_exclusive(run_root / "exit-uptime-raw-ns.txt", b"7100000\n")
        bad_root = root / "bad-run"
        shutil.copytree(run_root, bad_root, copy_function=shutil.copy2)
        with (bad_root / "stderr.log").open("a", encoding="utf-8") as output:
            output.write(
                "[FRAME_LIMIT] configured limit=4200 domain=present action=SDL_QUIT\n"
            )
        try:
            seal_run(bad_root, "CONFIRM_NO_GLITCH_OBSERVED", test_mode=True)
        except LiveError:
            pass
        else:
            raise LiveError("self-test accepted a FRAME_LIMIT lifecycle")
        manifest_path = seal_run(
            run_root, "CONFIRM_NO_GLITCH_OBSERVED", test_mode=True
        )
        manifest = _load_json(manifest_path)
        report = _load_json(run_root / "profile-report.json")
        _require(
            manifest["valid"] is True
            and manifest["persistent_baseline_eligible"] is False
            and report["diagnostic_gate"]["status"]
            == "PASS_DIAGNOSTIC_ONLY"
            and stat.S_IMODE((run_root / "profile-report.json").stat().st_mode)
            == 0o444
            and stat.S_IMODE(run_root.stat().st_mode) == 0o555,
            "self-test",
            "sealed diagnostic archive is not immutable and diagnostic-only",
        )
        try:
            profile_report._publish_report_exclusive(
                run_root / "profile-report.json", b"overwrite\n"
            )
        except profile_report.ReportError:
            pass
        else:
            raise LiveError("self-test accepted profile-report overwrite")
        evidence_path = next((run_root / "evidence").glob("*.png"))
        evidence_payload = evidence_path.read_bytes()
        os.chmod(evidence_path, 0o644)
        evidence_path.write_bytes(evidence_payload + b"tamper")
        try:
            verify_archive(run_root)
        except LiveError:
            pass
        else:
            raise LiveError("self-test accepted tampered PNG evidence")
        evidence_path.write_bytes(evidence_payload)
        os.chmod(evidence_path, 0o444)
        verify_archive(run_root)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "manual-only phase-6CK profile diagnostic; SIGUSR2 is profiling, "
            "never controller input"
        )
    )
    parser.add_argument("--self-test", action="store_true")
    subparsers = parser.add_subparsers(dest="command")
    start = subparsers.add_parser("start", help="launch one isolated live diagnostic")
    start.add_argument("--run", required=True)
    start.add_argument("--binary", required=True)
    start.add_argument("--lua", required=True)
    start.add_argument("--save-template", required=True)
    start.add_argument("--rom-root", required=True)
    start.add_argument(
        "--save-special-relative", default="SAVEDATA0/Special.0"
    )
    anchor = subparsers.add_parser("anchor", help="bind a manual scene announcement")
    anchor.add_argument("run")
    anchor.add_argument("scene", choices=("fort", "in_game"))
    anchor.add_argument("--attest", action="append", required=True)
    anchor.add_argument("--evidence", action="append", required=True)
    anchor.add_argument("--timer-before", type=int)
    anchor.add_argument("--timer-after", type=int)
    complete = subparsers.add_parser(
        "complete", help="close the first full p120 measurement window"
    )
    complete.add_argument("run")
    complete.add_argument("scene", choices=("fort", "in_game"))
    complete.add_argument("--attest", action="append", required=True)
    complete.add_argument("--evidence", action="append", required=True)
    seal = subparsers.add_parser("seal", help="validate and freeze the live archive")
    seal.add_argument("run")
    seal.add_argument("--attest", required=True)
    verify = subparsers.add_parser("verify", help="rehash a sealed diagnostic archive")
    verify.add_argument("run")
    return parser


def _main(argv: Sequence[str]) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        _require(arguments.command is None, "arguments", "--self-test does not accept a command")
        self_test()
        print("phase6ck-profile-live-self-test: PASS")
        return 0
    if arguments.command is None:
        parser.error("a command is required")
    if arguments.command == "start":
        print(start_run(arguments))
    elif arguments.command == "anchor":
        print(
            anchor_scene(
                pathlib.Path(arguments.run),
                arguments.scene,
                arguments.attest,
                [pathlib.Path(path) for path in arguments.evidence],
                timer_before=arguments.timer_before,
                timer_after=arguments.timer_after,
            )
        )
    elif arguments.command == "complete":
        print(
            complete_scene(
                pathlib.Path(arguments.run),
                arguments.scene,
                arguments.attest,
                [pathlib.Path(path) for path in arguments.evidence],
            )
        )
    elif arguments.command == "seal":
        print(seal_run(pathlib.Path(arguments.run), arguments.attest))
    elif arguments.command == "verify":
        print(verify_archive(pathlib.Path(arguments.run)))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(_main(sys.argv[1:]))
    except (LiveError, profile_report.ReportError, OSError, UnicodeError) as error:
        print(f"phase6ck-profile-live: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
