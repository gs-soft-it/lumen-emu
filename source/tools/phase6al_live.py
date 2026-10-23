#!/usr/bin/env python3
"""Run one preregistered phase-6AL live diagnostic replica on macOS."""

from __future__ import annotations

import argparse
import ctypes
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
from typing import Dict, List, Mapping, Optional, Sequence, Tuple

import phase6al_report


INTERVAL = 100000
PRESENT_CUTOFF = 120
DEFAULT_TIMEOUT_SECONDS = 600
EXIT_TIMEOUT_SECONDS = 60
MAIN_PREFIX = b"[A64_DYNAREC_PHASE6AL] "
END_PREFIX = b"[A64_DYNAREC_PHASE6AL_END] "
PRESENT_PREFIX = b"[FRAME_PIPELINE] present=120 "
ERROR_PATTERN = re.compile(
    r"fatal|assert|memory fault|device[._ ]lost|segmentation fault",
    re.IGNORECASE,
)

UNSET_ENVIRONMENT = (
    "LUMEN_CUSTOM_LOG",
    "LUMEN_AVPLAYER_TRACE",
    "LUMEN_PAD_STATE_TRACE",
    "LUMEN_VIDEOOUT_READBACK_FRAME",
    "LUMEN_X64_PROFILE_SAMPLE_INTERVAL",
    "LUMEN_X64_FAST_BLOCK_PROFILE_INTERVAL",
    "LUMEN_X64_FAST_BLOCK_TELEMETRY",
    "LUMEN_X64_FAST_BLOCK_TELEMETRY_WAIT_FOR_SIGNAL",
    "LUMEN_X64_FAST_BLOCK_CHAIN",
    "LUMEN_X64_A64_DYNAREC",
    "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE",
    "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES",
    "LUMEN_X64_A64_DYNAREC_DEFAULT_REQUESTED",
    "LUMEN_X64_A64_DYNAREC_DELAYED_POSITIVE_ADMISSION",
    "LUMEN_X64_A64_DYNAREC_SEPARATE_NEGATIVE_CACHE",
    "LUMEN_X64_A64_DYNAREC_STARTUP_TELEMETRY",
    "LUMEN_X64_A64_DYNAREC_TELEMETRY",
    "LUMEN_X64_A64_DYNAREC_PHASE6AF_TELEMETRY",
    "LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY",
    "LUMEN_X64_A64_DYNAREC_PHASE6AL_INTERVAL",
)

SET_ENVIRONMENT = {
    "LUMEN_X64_A64_DYNAREC_COMPILED_ARCHIVE_ENTRIES": "512",
    "LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY": "1",
    "LUMEN_X64_A64_DYNAREC_PHASE6AL_INTERVAL": str(INTERVAL),
    "LUMEN_FRAME_PIPELINE_TRACE": "1",
    "LUMEN_SHOW_WINDOW_EARLY": "1",
}


class LiveError(RuntimeError):
    pass


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
        encoded = json.dumps(record, sort_keys=True, separators=(",", ":")).encode("utf-8")
        digest.update(encoded)
        digest.update(b"\n")
        records.append(record)
    return {"digest": digest.hexdigest(), "records": records}


def _crash_snapshot() -> List[Mapping[str, object]]:
    crash_root = pathlib.Path.home() / "Library" / "Logs" / "DiagnosticReports"
    result: List[Mapping[str, object]] = []
    if not crash_root.is_dir():
        return result
    for path in sorted(crash_root.glob("fc_script*.ips")):
        stat = path.stat()
        result.append(
            {
                "path": str(path),
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
            }
        )
    return result


def _matching_processes(binary: pathlib.Path, lua: pathlib.Path) -> List[int]:
    completed = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,ucomm=,command=", "-ww"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="strict",
        check=True,
    )
    rows: List[Tuple[int, int, str, str]] = []
    parents: Dict[int, int] = {}
    for line in completed.stdout.splitlines():
        pieces = line.strip().split(None, 3)
        if len(pieces) != 4:
            continue
        pid = int(pieces[0])
        ppid = int(pieces[1])
        rows.append((pid, ppid, pieces[2], pieces[3]))
        parents[pid] = ppid

    excluded = {os.getpid()}
    ancestor = parents.get(os.getpid())
    while ancestor is not None and ancestor > 0 and ancestor not in excluded:
        excluded.add(ancestor)
        ancestor = parents.get(ancestor)

    binary = binary.resolve()
    lua = lua.resolve()
    matches: List[int] = []
    for pid, _ppid, _ucomm, command in rows:
        if pid in excluded:
            continue
        try:
            arguments = shlex.split(command)
        except ValueError:
            continue
        if len(arguments) < 2:
            continue
        try:
            first_argument = pathlib.Path(arguments[1]).resolve(strict=True)
        except OSError:
            continue
        if first_argument != lua:
            continue
        try:
            executable = _process_executable(pid)
        except OSError:
            continue
        if executable == binary and first_argument == lua:
            matches.append(pid)
    return matches


def _process_executable(pid: int) -> pathlib.Path:
    if sys.platform != "darwin":
        raise LiveError("phase6AL live process identity requires macOS libproc")
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_pidpath.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    length = library.proc_pidpath(pid, buffer, len(buffer))
    if length <= 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return pathlib.Path(os.fsdecode(buffer.value)).resolve(strict=True)


def _self_test_process_scan() -> None:
    script = pathlib.Path(__file__).resolve()
    executable = _process_executable(os.getpid())
    with tempfile.TemporaryDirectory(prefix="phase6al-live-self-test-") as temporary:
        child_script = pathlib.Path(temporary) / "phase6al_live_child.py"
        shutil.copy2(script, child_script)
        environment = os.environ.copy()
        python_path = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = (
            str(script.parent) if not python_path
            else str(script.parent) + os.pathsep + python_path
        )
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
                raise LiveError(
                    f"process scan expected only child {child.pid}, observed {matches}"
                )
        finally:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)
        if _matching_processes(executable, child_script):
            raise LiveError("process scan retained a completed child")
    runner_matches = _matching_processes(executable, script)
    if os.getpid() in runner_matches:
        raise LiveError("process scan matched the runner itself")


def _self_test_monitor_cutoff() -> None:
    class ExitedProcess:
        @staticmethod
        def poll() -> int:
            return 0

    worker_main = (
        b"[A64_DYNAREC_PHASE6AL] version=2 owner=1 owners=1 role=worker "
        b"primary_markers=1 primary_allocations=0 primary_owner=0 "
        b"role_invalid=0 process_invalid=0 process_valid=1 final=0 report=9\n"
    )
    primary_main = (
        b"[A64_DYNAREC_PHASE6AL] version=2 owner=2 owners=2 role=primary "
        b"primary_markers=1 primary_allocations=1 primary_owner=2 "
        b"role_invalid=0 process_invalid=0 process_valid=1 final=0 report=7\n"
    )
    with tempfile.TemporaryDirectory(prefix="phase6al-monitor-self-test-") as temporary:
        stderr_path = pathlib.Path(temporary) / "stderr.log"
        stderr_path.write_bytes(
            PRESENT_PREFIX + b"avg_interval_ms=1\n" +
            worker_main +
            b"[A64_DYNAREC_PHASE6AL_END] version=2 owner=1 complete=1 valid=1 report=9\n" +
            primary_main +
            b"[A64_DYNAREC_PHASE6AL_END] version=2 owner=2 complete=1 valid=1 report=7\n"
        )
        observed = _monitor_cutoff(ExitedProcess(), stderr_path, 1)  # type: ignore[arg-type]
        if observed != (True, True, 1, 2, 7):
            raise LiveError(f"primary monitor oracle mismatch: {observed}")

        stderr_path.write_bytes(
            PRESENT_PREFIX + b"avg_interval_ms=1\n" +
            primary_main +
            b"[A64_DYNAREC_PHASE6AL_END] version=2 owner=2 complete=1 valid=1 report=8\n"
        )
        observed = _monitor_cutoff(ExitedProcess(), stderr_path, 1)  # type: ignore[arg-type]
        if observed != (True, False, 1, 2, 7):
            raise LiveError(f"mismatched END passed the primary monitor: {observed}")


def _file_description(path: pathlib.Path) -> str:
    completed = subprocess.run(
        ["file", "-b", str(path)],
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


def _monitor_cutoff(
    process: subprocess.Popen[bytes], stderr_path: pathlib.Path, timeout_seconds: int
) -> Tuple[bool, bool, int, Optional[int], Optional[int]]:
    deadline = time.monotonic() + timeout_seconds
    pending = b""
    present_count = 0
    selected_owner: Optional[int] = None
    selected_report: Optional[int] = None
    selected_end = False
    with stderr_path.open("rb") as source:
        while time.monotonic() < deadline:
            chunk = source.read()
            if chunk:
                pending += chunk
                lines = pending.split(b"\n")
                pending = lines.pop()
                for line in lines:
                    if line.startswith(PRESENT_PREFIX):
                        present_count += 1
                    elif present_count == 1 and line.startswith(MAIN_PREFIX):
                        tokens = line.split(b" ")
                        if (
                            selected_owner is None and
                            len(tokens) >= 13 and
                            tokens[1] == b"version=2" and
                            tokens[4] == b"role=primary" and
                            tokens[2].startswith(b"owner=") and
                            tokens[12].startswith(b"report=")
                        ):
                            try:
                                selected_owner = int(tokens[2][len(b"owner="):], 10)
                                selected_report = int(tokens[12][len(b"report="):], 10)
                            except ValueError:
                                selected_owner = None
                                selected_report = None
                    elif present_count == 1 and line.startswith(END_PREFIX):
                        tokens = line.split(b" ")
                        if (
                            selected_owner is not None and
                            selected_report is not None and
                            len(tokens) == 6 and
                            tokens[1] == b"version=2" and
                            tokens[2] == f"owner={selected_owner}".encode("ascii") and
                            tokens[5] == f"report={selected_report}".encode("ascii")
                        ):
                            selected_end = True
                            break
                if selected_end:
                    break
            if process.poll() is not None:
                break
            time.sleep(0.05)
    return (
        present_count == 1,
        selected_end,
        present_count,
        selected_owner,
        selected_report,
    )


def _selected_payload(frame: phase6al_report.Frame) -> Mapping[str, object]:
    return {
        "source": frame.source,
        "start_line": frame.start_line,
        "owner": frame.owner,
        "report": frame.report_number,
        "final": frame.final,
        "main": dict(frame.main),
        "causes": [dict(cause) for cause in frame.causes],
        "models": [dict(model) for model in frame.models],
        "end": dict(frame.end),
    }


def _outside_save_files(work_root: pathlib.Path) -> List[str]:
    save_root = work_root / "_SaveData"
    result: List[str] = []
    for path in sorted(work_root.rglob("*"), key=lambda item: item.as_posix()):
        if not path.is_file():
            continue
        try:
            path.relative_to(save_root)
        except ValueError:
            result.append(path.relative_to(work_root).as_posix())
    return result


def run(arguments: argparse.Namespace) -> int:
    binary = pathlib.Path(arguments.binary).resolve(strict=True)
    lua = pathlib.Path(arguments.lua).resolve(strict=True)
    save_template = pathlib.Path(arguments.save_template).resolve(strict=True)
    rom_root = pathlib.Path(arguments.rom_root).resolve(strict=True)
    output_root = pathlib.Path(arguments.output).resolve()
    if output_root.exists():
        raise LiveError(f"output already exists: {output_root}")
    if signal.getsignal(signal.SIGTERM) != signal.SIG_DFL:
        raise LiveError("runner SIGTERM disposition is not SIG_DFL")
    if signal.SIGTERM in signal.pthread_sigmask(signal.SIG_BLOCK, []):
        raise LiveError("runner inherits a blocked SIGTERM")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise LiveError(f"binary is not an executable file: {binary}")
    if not lua.is_file():
        raise LiveError(f"Lua input is not a file: {lua}")
    if not save_template.is_dir():
        raise LiveError(f"save template is not a directory: {save_template}")

    eboot = rom_root / "eboot.bin"
    shader_library = rom_root / "Orbis" / "DataPS4" / "Bundles" / "ShaderProgLibrary.xom"
    if not eboot.is_file() or not shader_library.is_file():
        raise LiveError("ROM root does not contain the preregistered eboot/shader library")
    binary_description = _file_description(binary)
    if "Mach-O 64-bit executable arm64" not in binary_description or "universal" in binary_description:
        raise LiveError(f"binary is not thin arm64: {binary_description}")
    if not _contains_bytes(binary, b"LUMEN_X64_A64_DYNAREC_PHASE6AL_TELEMETRY"):
        raise LiveError("binary does not contain the phase-6AL runtime selector")
    if not _contains_bytes(binary, b"[A64_DYNAREC_PHASE6AL]"):
        raise LiveError("binary does not contain the phase-6AL report label")

    existing_processes = _matching_processes(binary, lua)
    if existing_processes:
        raise LiveError(f"matching guest process already running: {existing_processes}")

    output_root.mkdir(parents=True)
    work_root = output_root / "work"
    work_root.mkdir()
    shutil.copytree(save_template, work_root / "_SaveData", copy_function=shutil.copy2)
    stdout_path = output_root / "stdout.log"
    stderr_path = output_root / "stderr.log"

    save_before = _tree_snapshot(work_root / "_SaveData", True)
    rom_before = _tree_snapshot(rom_root, False)
    crash_before = _crash_snapshot()
    preflight = {
        "version": 1,
        "replica": arguments.replica,
        "binary": str(binary),
        "binary_file": binary_description,
        "binary_sha256": _sha256_file(binary),
        "lua": str(lua),
        "lua_sha256": _sha256_file(lua),
        "rom_root": str(rom_root),
        "eboot_sha256": _sha256_file(eboot),
        "shader_library_sha256": _sha256_file(shader_library),
        "rom_tree": rom_before,
        "save_template": str(save_template),
        "save_before": save_before,
        "environment_unset": list(UNSET_ENVIRONMENT),
        "environment_set": dict(SET_ENVIRONMENT),
        "interval": INTERVAL,
        "present_cutoff": PRESENT_CUTOFF,
        "timeout_seconds": DEFAULT_TIMEOUT_SECONDS,
        "crash_before": crash_before,
        "started_unix_ns": time.time_ns(),
    }
    _write_json(output_root / "preflight.json", preflight)

    environment = os.environ.copy()
    for variable in UNSET_ENVIRONMENT:
        environment.pop(variable, None)
    environment.update(SET_ENVIRONMENT)

    process: Optional[subprocess.Popen[bytes]] = None
    sigterm_count = 0
    forced_kill = False
    cutoff_present = False
    selected_end = False
    present_count = 0
    monitored_owner: Optional[int] = None
    monitored_report: Optional[int] = None
    exit_code: Optional[int] = None
    launch_error: Optional[str] = None
    with stdout_path.open("xb") as stdout_log, stderr_path.open("xb") as stderr_log:
        try:
            process = subprocess.Popen(
                [str(binary), str(lua)],
                cwd=work_root,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=stdout_log,
                stderr=stderr_log,
            )
            (
                cutoff_present,
                selected_end,
                present_count,
                monitored_owner,
                monitored_report,
            ) = _monitor_cutoff(process, stderr_path, DEFAULT_TIMEOUT_SECONDS)
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                sigterm_count += 1
            try:
                exit_code = process.wait(timeout=EXIT_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                forced_kill = True
                exit_code = process.wait(timeout=10)
        except (OSError, subprocess.SubprocessError) as error:
            launch_error = str(error)
            if (
                process is not None and
                process.poll() is None and
                sigterm_count == 0
            ):
                process.send_signal(signal.SIGTERM)
                sigterm_count += 1
                try:
                    exit_code = process.wait(timeout=EXIT_TIMEOUT_SECONDS)
                except subprocess.TimeoutExpired:
                    process.kill()
                    forced_kill = True
                    exit_code = process.wait(timeout=10)

    stdout_text = stdout_path.read_text(encoding="utf-8", errors="strict")
    stderr_text = stderr_path.read_text(encoding="utf-8", errors="strict")
    combined_text = stdout_text + "\n" + stderr_text
    save_after = _tree_snapshot(work_root / "_SaveData", True)
    rom_after = _tree_snapshot(rom_root, False)
    crash_after = _crash_snapshot()
    residues = _matching_processes(binary, lua)
    outside_save = _outside_save_files(work_root)
    sdl_quit = combined_text.count("[WINDOW_EXIT] SDL_QUIT")
    error_matches = ERROR_PATTERN.findall(combined_text)

    parser_error: Optional[str] = None
    selected_payload: Optional[Mapping[str, object]] = None
    monitor_matches_selected = False
    try:
        report = phase6al_report.parse_report_text(stderr_text, str(stderr_path))
        phase6al_report.require_interval(report, INTERVAL)
        selected = phase6al_report.select_first_frame_after_present(
            report, stderr_text, PRESENT_CUTOFF
        )
        selected_payload = _selected_payload(selected)
        monitor_matches_selected = (
            selected.owner == monitored_owner and
            selected.report_number == monitored_report
        )
        _write_json(output_root / "selected.json", selected_payload)
    except (phase6al_report.ValidationError, OSError) as error:
        parser_error = str(error)

    save_unchanged = save_before["digest"] == save_after["digest"]
    rom_unchanged = rom_before["digest"] == rom_after["digest"]
    crash_unchanged = crash_before == crash_after
    valid = all(
        (
            launch_error is None,
            cutoff_present,
            present_count == 1,
            selected_end,
            sigterm_count == 1,
            not forced_kill,
            exit_code == 0,
            sdl_quit == 1,
            not error_matches,
            parser_error is None,
            selected_payload is not None,
            monitor_matches_selected,
            save_unchanged,
            rom_unchanged,
            crash_unchanged,
            not residues,
            not outside_save,
        )
    )
    result = {
        "version": 1,
        "replica": arguments.replica,
        "valid": valid,
        "launch_error": launch_error,
        "cutoff_present": cutoff_present,
        "present_count": present_count,
        "selected_end_seen_before_signal": selected_end,
        "monitored_primary_owner": monitored_owner,
        "monitored_primary_report": monitored_report,
        "monitor_matches_selected": monitor_matches_selected,
        "sigterm_count": sigterm_count,
        "forced_kill": forced_kill,
        "exit_code": exit_code,
        "sdl_quit": sdl_quit,
        "error_matches": error_matches,
        "parser_error": parser_error,
        "save_unchanged": save_unchanged,
        "save_after": save_after,
        "rom_unchanged": rom_unchanged,
        "rom_after": rom_after,
        "crash_unchanged": crash_unchanged,
        "crash_after": crash_after,
        "matching_process_residues": residues,
        "outside_save_files": outside_save,
        "finished_unix_ns": time.time_ns(),
    }
    _write_json(output_root / "result.json", result)
    print(
        "phase6al-live: "
        f"{'PASS' if valid else 'FAIL'} replica={arguments.replica} "
        f"present={present_count} end={int(selected_end)} exit={exit_code} "
        f"sdl_quit={sdl_quit} parser={'ok' if parser_error is None else 'fail'}"
    )
    return 0 if valid else 1


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="run one isolated phase-6AL live replica through the p120 cutoff"
    )
    parser.add_argument("--self-test", action="store_true", help="run the sterile process-scan oracle")
    parser.add_argument("--process-scan-child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--binary", help="diagnostic thin-arm64 fc_script")
    parser.add_argument("--lua", help="frozen Lua launch script")
    parser.add_argument(
        "--save-template",
        help="baseline _SaveData directory copied into the isolated run root",
    )
    parser.add_argument("--rom-root", help="read-only game install root")
    parser.add_argument("--output", help="new output directory")
    parser.add_argument("--replica", choices=("R1", "R2"))
    return parser


def main(argv: Sequence[str]) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if arguments.process_scan_child:
        time.sleep(30)
        return 0
    if arguments.self_test:
        normal_values = (
            arguments.binary,
            arguments.lua,
            arguments.save_template,
            arguments.rom_root,
            arguments.output,
            arguments.replica,
        )
        if any(value is not None for value in normal_values):
            parser.error("--self-test does not accept live-run arguments")
        try:
            _self_test_monitor_cutoff()
            _self_test_process_scan()
        except (LiveError, OSError, subprocess.SubprocessError) as error:
            print(f"phase6al-live-self-test: FAIL: {error}", file=sys.stderr)
            return 1
        print("phase6al-live-self-test: PASS")
        return 0
    required = {
        "--binary": arguments.binary,
        "--lua": arguments.lua,
        "--save-template": arguments.save_template,
        "--rom-root": arguments.rom_root,
        "--output": arguments.output,
        "--replica": arguments.replica,
    }
    missing = [name for name, value in required.items() if value is None]
    if missing:
        parser.error("the following arguments are required: " + ", ".join(missing))
    try:
        return run(arguments)
    except (LiveError, OSError, UnicodeError, subprocess.SubprocessError) as error:
        print(f"phase6al-live: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
