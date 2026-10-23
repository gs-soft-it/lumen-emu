#!/usr/bin/env python3
"""Launch and seal one manual phase-6CM diagnostic, never a new B/C run.

Phase-6CM deliberately reuses the audited phase-6CK live lifecycle engine:
exclusive artifacts, sterile save clone, stable L receipt, L+120 discard,
L+240 measurement, exact SIGUSR2 snapshots, evidence hashing, clean exit, and
immutable sealing.  This adapter replaces every phase-specific schema,
environment selector, marker parser, and provenance receipt with phase-6CM.

No controller, keyboard, mouse, AppleScript, or debugger input is generated.
SIGUSR2 only requests a profiler snapshot after a manual scene announcement.
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
from typing import Any, Mapping, Sequence

import phase6ck_profile_live as _lifecycle
import phase6cm_profile_report as profile_report


RUN_SCHEMA = "lumen.phase6cm-profile-live-run/1"
RECEIPT_SCHEMA = "lumen.phase6cm-profile-live-receipt/1"
CHECKPOINT_SCHEMA = "lumen.phase6cm-profile-live-checkpoint/1"
MANIFEST_SCHEMA = "lumen.phase6cm-profile-live-manifest/1"
DIAGNOSTIC_SCOPE = profile_report.DIAGNOSTIC_SCOPE
LAUNCH_CLOCK_SEMANTICS = "pre_popen_diagnostic_only_not_baseline_loading_metric"
LIFECYCLE_ENGINE_SCHEMA = "lumen.phase6cm-profile-live-lifecycle-engine/1"

_WRAPPER_PATH = pathlib.Path(__file__).resolve()
_ENGINE_PATH = pathlib.Path(_lifecycle.__file__).resolve()
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


def _phase6cm_contains(path: pathlib.Path, needle: bytes) -> bool:
    replacements = {
        b"LUMEN_X64_A64_DYNAREC_DENSE_PROFILE": b"LUMEN_X64_A64_DYNAREC_ORDINARY_PROFILE",
        b"[A64_DENSE_PHASE6CK_PROFILE]": b"[A64_ORDINARY_PHASE6CM_PROFILE]",
    }
    return _ORIGINAL_CONTAINS(path, replacements.get(needle, needle))


def _publish_json_with_engine(path: pathlib.Path, value: Any) -> None:
    if path.name == "freeze.json" and type(value) is dict:
        enriched = dict(value)
        enriched["lifecycle_engine"] = {
            "schema": LIFECYCLE_ENGINE_SCHEMA,
            "path": str(_ENGINE_PATH),
            "sha256": _sha_file(_ENGINE_PATH),
        }
        value = enriched
    _ORIGINAL_PUBLISH_JSON(path, value)


def _validate_engine_receipt(run_root: pathlib.Path) -> None:
    freeze = _lifecycle._load_json(run_root / "freeze.json")
    engine = freeze.get("lifecycle_engine")
    _lifecycle._require(
        type(engine) is dict
        and set(engine) == {"schema", "path", "sha256"}
        and engine["schema"] == LIFECYCLE_ENGINE_SCHEMA,
        run_root,
        "phase-6CM lifecycle-engine receipt is missing or malformed",
    )
    assert isinstance(engine, dict)
    try:
        recorded_path = pathlib.Path(str(engine["path"])).resolve(strict=True)
    except OSError as error:
        raise _lifecycle.LiveError(
            f"{run_root}: lifecycle engine path cannot be resolved: {error}"
        ) from error
    _lifecycle._require(
        recorded_path == _ENGINE_PATH
        and type(engine["sha256"]) is str
        and engine["sha256"] == _sha_file(_ENGINE_PATH),
        run_root,
        "phase-6CK lifecycle engine changed after phase-6CM launch",
    )


def _validate_phase6cm_sealed_metadata(run_root: pathlib.Path) -> None:
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
        and type(snapshots) is list
        and bool(snapshots),
        report_path,
        "sealed report is not a lifecycle-bound diagnostic PASS",
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
        raise _lifecycle.LiveError(
            f"{run_root}: cannot read exit uptime: {error}"
        ) from error
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
            value.replace("phase6ck", "phase6cm").replace("phase-6CK", "phase-6CM")
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
    _validate_phase6cm_sealed_metadata(resolved)
    return manifest


def seal_run(
    run_root: pathlib.Path, no_glitch_attestation: str, *, test_mode: bool = False
) -> pathlib.Path:
    resolved = run_root.resolve()
    _validate_engine_receipt(resolved)
    return _ORIGINAL_SEAL_RUN(
        resolved, no_glitch_attestation, test_mode=test_mode
    )


def _configure_lifecycle_engine() -> None:
    environment = dict(_lifecycle.LIVE_ENVIRONMENT)
    environment.pop("LUMEN_X64_A64_DYNAREC_DENSE_PROFILE", None)
    environment["LUMEN_X64_A64_DYNAREC_STABLE_NEGATIVE_ROUTE"] = "1"
    environment["LUMEN_X64_A64_DYNAREC_ORDINARY_PROFILE"] = "1"

    # Functions in the imported module resolve globals at call time.  Binding
    # these values makes the reused engine phase-6CM-specific, while changing
    # __file__ ensures its runner receipt hashes this actual entry point.
    _lifecycle.profile_report = profile_report
    _lifecycle.RUN_SCHEMA = RUN_SCHEMA
    _lifecycle.RECEIPT_SCHEMA = RECEIPT_SCHEMA
    _lifecycle.CHECKPOINT_SCHEMA = CHECKPOINT_SCHEMA
    _lifecycle.MANIFEST_SCHEMA = MANIFEST_SCHEMA
    _lifecycle.DIAGNOSTIC_SCOPE = DIAGNOSTIC_SCOPE
    _lifecycle.LAUNCH_CLOCK_SEMANTICS = LAUNCH_CLOCK_SEMANTICS
    _lifecycle.LIVE_ENVIRONMENT = environment
    _lifecycle.__file__ = str(_WRAPPER_PATH)
    _lifecycle._contains = _phase6cm_contains
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
    with tempfile.TemporaryDirectory(
        prefix="phase6cm-profile-live-seal-self-test-"
    ) as temporary:
        run_root = pathlib.Path(temporary) / "archive"
        run_root.mkdir()
        lifecycle = {"final_clean": True}
        _ORIGINAL_PUBLISH_JSON(
            run_root / "profile-report.json",
            {
                "diagnostic_gate": {
                    "passed": True,
                    "status": "PASS_DIAGNOSTIC_ONLY",
                },
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
        _validate_phase6cm_sealed_metadata(run_root)
        os.chmod(run_root / "profile-report.json", 0o644)
        try:
            _validate_phase6cm_sealed_metadata(run_root)
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
            "manual-only phase-6CM ordinary profile diagnostic; SIGUSR2 is "
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
        _lifecycle._require(
            arguments.command is None,
            "arguments",
            "--self-test does not accept a command",
        )
        self_test()
        print("phase6cm-profile-live-self-test: PASS")
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
        print(f"phase6cm-profile-live: FAIL: {error}", file=sys.stderr)
        raise SystemExit(2)
