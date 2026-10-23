#!/usr/bin/env python3
"""Launch and seal one manual phase-6CO diagnostic, never a B/C run.

The phase-specific adapter reuses the audited phase-6CM/6CK lifecycle: an
exclusive archive, sterile save clone, exact-once non-terminal p2300 receipt,
manual Fort and Navigation announcements, L+120 discard, L+240 measurement,
four SIGUSR2 snapshots, clean cooperative exit, hashing and immutable sealing.

No controller, keyboard, mouse, AppleScript or debugger input is generated.
The only profiler selector is the exact string
``LUMEN_X64_A64_DYNAREC_SNAPSHOT_PROFILE=1``; stable-negative routing remains
enabled and every result is ineligible for the persistent baseline.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import os
import pathlib
import re
import stat
import sys
import tempfile
from typing import Any, Sequence

import phase6ck_profile_live as _lifecycle
import phase6co_profile_report as profile_report


RUN_SCHEMA = "lumen.phase6co-profile-live-run/1"
RECEIPT_SCHEMA = "lumen.phase6co-profile-live-receipt/1"
CHECKPOINT_SCHEMA = "lumen.phase6co-profile-live-checkpoint/1"
MANIFEST_SCHEMA = "lumen.phase6co-profile-live-manifest/1"
DIAGNOSTIC_SCOPE = profile_report.DIAGNOSTIC_SCOPE
LAUNCH_CLOCK_SEMANTICS = "pre_popen_diagnostic_only_not_baseline_loading_metric"
LIFECYCLE_ENGINE_SCHEMA = "lumen.phase6co-profile-live-lifecycle-engine/1"
REPORT_ENGINE_SCHEMA = "lumen.phase6co-profile-report-engine/1"

_WRAPPER_PATH = pathlib.Path(__file__).resolve()
_ENGINE_PATH = pathlib.Path(_lifecycle.__file__).resolve()
_REPORT_ENGINE_PATH = profile_report.REPORT_ENGINE_PATH
_ORIGINAL_PUBLISH_JSON = _lifecycle._publish_json
_ORIGINAL_CONTAINS = _lifecycle._contains
_ORIGINAL_START_RUN = _lifecycle.start_run
_ORIGINAL_SEAL_RUN = _lifecycle.seal_run
_ORIGINAL_VERIFY_ARCHIVE = _lifecycle.verify_archive


def _sha_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _phase6co_contains(path: pathlib.Path, needle: bytes) -> bool:
    if needle == b"LUMEN_X64_A64_DYNAREC_DENSE_PROFILE":
        return _ORIGINAL_CONTAINS(
            path, b"LUMEN_X64_A64_DYNAREC_SNAPSHOT_PROFILE"
        )
    if needle == b"[A64_DENSE_PHASE6CK_PROFILE]":
        return _ORIGINAL_CONTAINS(
            path, b"[A64_SNAPSHOT_PHASE6CO_CONFIG]"
        ) and _ORIGINAL_CONTAINS(path, b"[A64_SNAPSHOT_PHASE6CO_PROFILE]")
    return _ORIGINAL_CONTAINS(path, needle)


def _publish_json_with_engine(path: pathlib.Path, value: Any) -> None:
    if path.name == "freeze.json" and type(value) is dict:
        enriched = dict(value)
        enriched["lifecycle_engine"] = {
            "schema": LIFECYCLE_ENGINE_SCHEMA,
            "path": str(_ENGINE_PATH),
            "sha256": _sha_file(_ENGINE_PATH),
        }
        enriched["report_engine"] = {
            "schema": REPORT_ENGINE_SCHEMA,
            "path": str(_REPORT_ENGINE_PATH),
            "sha256": _sha_file(_REPORT_ENGINE_PATH),
        }
        value = enriched
    _ORIGINAL_PUBLISH_JSON(path, value)


def _validate_engine_receipt(run_root: pathlib.Path) -> None:
    freeze = _lifecycle._load_json(run_root / "freeze.json")
    engine = freeze.get("lifecycle_engine")
    report_engine = freeze.get("report_engine")
    _lifecycle._require(
        type(engine) is dict
        and set(engine) == {"schema", "path", "sha256"}
        and engine["schema"] == LIFECYCLE_ENGINE_SCHEMA,
        run_root,
        "phase-6CO lifecycle-engine receipt is missing or malformed",
    )
    _lifecycle._require(
        type(report_engine) is dict
        and set(report_engine) == {"schema", "path", "sha256"}
        and report_engine["schema"] == REPORT_ENGINE_SCHEMA,
        run_root,
        "phase-6CO report-engine receipt is missing or malformed",
    )
    assert isinstance(engine, dict)
    assert isinstance(report_engine, dict)
    try:
        recorded_path = pathlib.Path(str(engine["path"])).resolve(strict=True)
        recorded_report_path = pathlib.Path(str(report_engine["path"])).resolve(
            strict=True
        )
    except OSError as error:
        raise _lifecycle.LiveError(
            f"{run_root}: lifecycle engine path cannot be resolved: {error}"
        ) from error
    _lifecycle._require(
        recorded_path == _ENGINE_PATH
        and type(engine["sha256"]) is str
        and engine["sha256"] == _sha_file(_ENGINE_PATH),
        run_root,
        "phase-6CK lifecycle engine changed after phase-6CO launch",
    )
    _lifecycle._require(
        recorded_report_path == _REPORT_ENGINE_PATH
        and type(report_engine["sha256"]) is str
        and report_engine["sha256"] == _sha_file(_REPORT_ENGINE_PATH),
        run_root,
        "phase-6CM report engine changed after phase-6CO launch",
    )


def _validate_phase6co_sealed_metadata(run_root: pathlib.Path) -> None:
    manifest_path = run_root / "diagnostic-manifest.json"
    report_path = run_root / "profile-report.json"
    manifest = _lifecycle._load_json(manifest_path)
    report = _lifecycle._load_json(report_path)
    _lifecycle._require(
        type(manifest) is dict and type(report) is dict,
        run_root,
        "sealed manifest/report must be objects",
    )
    assert isinstance(manifest, dict) and isinstance(report, dict)
    live_binding = report.get("live_binding")
    diagnostic_gate = report.get("diagnostic_gate")
    snapshots = report.get("snapshots")
    _lifecycle._require(
        type(live_binding) is dict
        and type(diagnostic_gate) is dict
        and diagnostic_gate.get("passed") is True
        and diagnostic_gate.get("status") == "PASS_DIAGNOSTIC_ONLY"
        and report.get("persistent_baseline_eligible") is False
        and report.get("loading_metric_eligible") is False
        and type(snapshots) is list
        and bool(snapshots),
        report_path,
        "sealed report is not a lifecycle-bound diagnostic-only PASS",
    )
    assert isinstance(live_binding, dict) and isinstance(snapshots, list)
    final = snapshots[-1]
    _lifecycle._require(
        type(final) is dict
        and final.get("final") == 1
        and type(final.get("uptime_raw_ns")) is int
        and 0 < final["uptime_raw_ns"] <= profile_report.UINT64_MAX,
        report_path,
        "sealed report lacks a canonical final snapshot",
    )
    _lifecycle._require(
        manifest.get("lifecycle") == live_binding.get("lifecycle"),
        manifest_path,
        "manifest lifecycle differs from the bound profile report",
    )
    try:
        exit_bytes = (run_root / "exit-uptime-raw-ns.txt").read_bytes()
    except OSError as error:
        raise _lifecycle.LiveError(f"{run_root}: cannot read exit uptime: {error}") from error
    _lifecycle._require(
        re.fullmatch(rb"[1-9][0-9]*\n", exit_bytes) is not None,
        run_root,
        "exit uptime is not canonical positive decimal",
    )
    exit_ns = int(exit_bytes[:-1], 10)
    sealed_ns = manifest.get("sealed_uptime_raw_ns")
    _lifecycle._require(
        type(sealed_ns) is int
        and 0 < sealed_ns <= profile_report.UINT64_MAX
        and final["uptime_raw_ns"] <= exit_ns <= sealed_ns,
        manifest_path,
        "final/exit/seal uptime ordering is invalid",
    )
    for path in (run_root, *sorted(run_root.rglob("*"))):
        expected_mode = 0o555 if path.is_dir() else 0o444
        _lifecycle._require(
            stat.S_IMODE(path.stat().st_mode) == expected_mode,
            path,
            f"sealed mode must be {expected_mode:04o}",
        )


class _PhaseNameWriter:
    def __init__(self, target: Any) -> None:
        self._target = target

    def write(self, value: str) -> int:
        return self._target.write(
            value.replace("phase6ck", "phase6co").replace("phase-6CK", "phase-6CO")
        )

    def flush(self) -> None:
        self._target.flush()

    def __getattr__(self, name: str) -> Any:
        return getattr(self._target, name)


def start_run(arguments: argparse.Namespace) -> pathlib.Path:
    with contextlib.redirect_stdout(_PhaseNameWriter(sys.stdout)):
        return _ORIGINAL_START_RUN(arguments)


def verify_archive(run_root: pathlib.Path) -> pathlib.Path:
    resolved = run_root.resolve()
    _validate_engine_receipt(resolved)
    manifest = _ORIGINAL_VERIFY_ARCHIVE(resolved)
    _validate_phase6co_sealed_metadata(resolved)
    return manifest


def seal_run(
    run_root: pathlib.Path, no_glitch_attestation: str, *, test_mode: bool = False
) -> pathlib.Path:
    resolved = run_root.resolve()
    _validate_engine_receipt(resolved)
    return _ORIGINAL_SEAL_RUN(resolved, no_glitch_attestation, test_mode=test_mode)


def _configure_lifecycle_engine() -> None:
    environment = dict(_lifecycle.LIVE_ENVIRONMENT)
    environment.pop("LUMEN_X64_A64_DYNAREC_DENSE_PROFILE", None)
    environment["LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"] = "1"
    environment["LUMEN_X64_A64_DYNAREC_SNAPSHOT_PROFILE"] = "1"

    _lifecycle.profile_report = profile_report
    _lifecycle.RUN_SCHEMA = RUN_SCHEMA
    _lifecycle.RECEIPT_SCHEMA = RECEIPT_SCHEMA
    _lifecycle.CHECKPOINT_SCHEMA = CHECKPOINT_SCHEMA
    _lifecycle.MANIFEST_SCHEMA = MANIFEST_SCHEMA
    _lifecycle.DIAGNOSTIC_SCOPE = DIAGNOSTIC_SCOPE
    _lifecycle.LAUNCH_CLOCK_SEMANTICS = LAUNCH_CLOCK_SEMANTICS
    _lifecycle.LIVE_ENVIRONMENT = environment
    _lifecycle.__file__ = str(_WRAPPER_PATH)
    _lifecycle._contains = _phase6co_contains
    _lifecycle._publish_json = _publish_json_with_engine
    _lifecycle.seal_run = seal_run
    _lifecycle.verify_archive = verify_archive


_configure_lifecycle_engine()

LiveError = _lifecycle.LiveError
FORT_ANCHOR_ATTESTATIONS = _lifecycle.FORT_ANCHOR_ATTESTATIONS
FORT_COMPLETE_ATTESTATIONS = _lifecycle.FORT_COMPLETE_ATTESTATIONS
GAME_ANCHOR_ATTESTATIONS = _lifecycle.GAME_ANCHOR_ATTESTATIONS
GAME_COMPLETE_ATTESTATIONS = _lifecycle.GAME_COMPLETE_ATTESTATIONS
anchor_scene = _lifecycle.anchor_scene
complete_scene = _lifecycle.complete_scene


def self_test() -> None:
    profile_report.self_test()
    _lifecycle.self_test()
    _lifecycle._require(
        _lifecycle.LIVE_ENVIRONMENT.get("LUMEN_X64_A64_DYNAREC_SNAPSHOT_PROFILE") == "1"
        and _lifecycle.LIVE_ENVIRONMENT.get("LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE") == "1"
        and "LUMEN_X64_A64_DYNAREC_DENSE_PROFILE" not in _lifecycle.LIVE_ENVIRONMENT,
        "self-test",
        "phase-6CO live selector environment is not exact and isolated",
    )
    with tempfile.TemporaryDirectory(prefix="phase6co-profile-live-seal-self-test-") as temporary:
        temporary_root = pathlib.Path(temporary)
        engine_root = temporary_root / "engine-receipt"
        engine_root.mkdir()
        marker_binary = temporary_root / "marker-binary"
        marker_binary.write_bytes(
            b"LUMEN_X64_A64_DYNAREC_SNAPSHOT_PROFILE\0"
            b"[A64_SNAPSHOT_PHASE6CO_CONFIG]\0"
            b"[A64_SNAPSHOT_PHASE6CO_PROFILE]\0"
        )
        _lifecycle._require(
            _phase6co_contains(
                marker_binary, b"LUMEN_X64_A64_DYNAREC_DENSE_PROFILE"
            )
            and _phase6co_contains(
                marker_binary, b"[A64_DENSE_PHASE6CK_PROFILE]"
            ),
            "self-test",
            "phase-6CO binary selector/marker preflight rejected the exact contract",
        )
        marker_binary.write_bytes(
            b"LUMEN_X64_A64_DYNAREC_SNAPSHOT_PROFILE\0"
            b"[A64_SNAPSHOT_PHASE6CO_PROFILE]\0"
        )
        _lifecycle._require(
            not _phase6co_contains(
                marker_binary, b"[A64_DENSE_PHASE6CK_PROFILE]"
            ),
            "self-test",
            "phase-6CO binary preflight accepted a missing CONFIG marker",
        )
        _ORIGINAL_PUBLISH_JSON(
            engine_root / "freeze.json",
            {
                "lifecycle_engine": {
                    "schema": LIFECYCLE_ENGINE_SCHEMA,
                    "path": str(_ENGINE_PATH),
                    "sha256": _sha_file(_ENGINE_PATH),
                },
                "report_engine": {
                    "schema": REPORT_ENGINE_SCHEMA,
                    "path": str(_REPORT_ENGINE_PATH),
                    "sha256": _sha_file(_REPORT_ENGINE_PATH),
                },
            },
        )
        _validate_engine_receipt(engine_root)
        freeze_path = engine_root / "freeze.json"
        freeze_payload = freeze_path.read_bytes()
        report_digest = _sha_file(_REPORT_ENGINE_PATH).encode("ascii")
        os.chmod(freeze_path, 0o644)
        freeze_path.write_bytes(freeze_payload.replace(report_digest, b"0" * 64, 1))
        try:
            _validate_engine_receipt(engine_root)
        except LiveError:
            pass
        else:
            raise LiveError("engine self-test accepted a changed phase-6CM report engine")

        run_root = temporary_root / "archive"
        run_root.mkdir()
        lifecycle = {"final_clean": True}
        _ORIGINAL_PUBLISH_JSON(
            run_root / "profile-report.json",
            {
                "persistent_baseline_eligible": False,
                "loading_metric_eligible": False,
                "diagnostic_gate": {"passed": True, "status": "PASS_DIAGNOSTIC_ONLY"},
                "live_binding": {"lifecycle": lifecycle},
                "snapshots": [{"final": 1, "uptime_raw_ns": 10}],
            },
        )
        _ORIGINAL_PUBLISH_JSON(
            run_root / "diagnostic-manifest.json",
            {"lifecycle": lifecycle, "sealed_uptime_raw_ns": 30},
        )
        (run_root / "exit-uptime-raw-ns.txt").write_bytes(b"20\n")
        _lifecycle._readonly_tree(run_root)
        _validate_phase6co_sealed_metadata(run_root)
        os.chmod(run_root / "profile-report.json", 0o644)
        try:
            _validate_phase6co_sealed_metadata(run_root)
        except LiveError:
            pass
        else:
            raise LiveError("seal self-test accepted a writable artifact")
        finally:
            os.chmod(run_root / "profile-report.json", 0o444)
            os.chmod(run_root, 0o755)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "manual-only phase-6CO operation-snapshot diagnostic; SIGUSR2 is "
            "profiling, never controller input"
        )
    )
    parser.add_argument("--self-test", action="store_true")
    subparsers = parser.add_subparsers(dest="command")
    start = subparsers.add_parser("start", help="launch one isolated diagnostic")
    start.add_argument("--run", required=True)
    start.add_argument("--binary", required=True)
    start.add_argument("--lua", required=True)
    start.add_argument("--save-template", required=True)
    start.add_argument("--rom-root", required=True)
    start.add_argument("--save-special-relative", default="SAVEDATA0/Special.0")
    anchor = subparsers.add_parser("anchor", help="bind a manual scene announcement")
    anchor.add_argument("run")
    anchor.add_argument("scene", choices=("fort", "in_game"))
    anchor.add_argument("--attest", action="append", required=True)
    anchor.add_argument("--evidence", action="append", required=True)
    anchor.add_argument("--timer-before", type=int)
    anchor.add_argument("--timer-after", type=int)
    complete = subparsers.add_parser("complete", help="close first full p120 window")
    complete.add_argument("run")
    complete.add_argument("scene", choices=("fort", "in_game"))
    complete.add_argument("--attest", action="append", required=True)
    complete.add_argument("--evidence", action="append", required=True)
    seal = subparsers.add_parser("seal", help="validate and freeze the archive")
    seal.add_argument("run")
    seal.add_argument("--attest", required=True)
    verify = subparsers.add_parser("verify", help="rehash a sealed archive")
    verify.add_argument("run")
    return parser


def _main(argv: Sequence[str]) -> int:
    parser = _parser()
    arguments = parser.parse_args(argv)
    if arguments.self_test:
        _lifecycle._require(arguments.command is None, "arguments", "--self-test does not accept a command")
        self_test()
        print("phase6co-profile-live-self-test: PASS")
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
        print(f"phase6co-profile-live: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
