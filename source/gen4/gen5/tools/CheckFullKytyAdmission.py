#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the full executable's closed ARM64 admission gate, without running a guest."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("game_directory", type=Path)
    parser.add_argument("evidence_directory", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    game = args.game_directory.resolve(strict=True)
    evidence = args.evidence_directory.resolve()
    executable = game / "eboot.bin"
    if not executable.is_file():
        parser.error("provide a single extracted game root containing eboot.bin")
    if subprocess.check_output(["/usr/bin/lipo", "-archs", str(binary)], text=True).strip() != "arm64":
        parser.error("the admission test requires the thin native arm64 executable")
    evidence.mkdir(parents=True, exist_ok=True)
    started = time.time()
    before_hash = digest(executable)
    command = [str(binary), "--game", str(game)]
    with tempfile.TemporaryDirectory(prefix="lumen-ps5-admission-", dir=evidence) as temporary:
        timed_out = False
        try:
            result = subprocess.run(command, cwd=temporary, capture_output=True, timeout=15)
            code, stdout, stderr = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as error:
            timed_out = True
            code, stdout, stderr = None, error.stdout or b"", error.stderr or b""
        created_paths = sorted(str(path.relative_to(temporary)) for path in Path(temporary).rglob("*"))
    stdout_path = evidence / "stdout.log"
    stderr_path = evidence / "stderr.log"
    stdout_path.write_bytes(stdout)
    stderr_path.write_bytes(stderr)
    diagnostics = stderr.decode("utf-8", errors="replace")
    witnesses = {
        "nonzero_exit_one": code == 1,
        "admission_denied": "PS5_RUNTIME_ADMISSION_DENIED:" in diagnostics,
        "guest_not_run": "guest_execution=NOT_RUN" in diagnostics,
        "guest_memory_not_loaded": "guest_memory_loaded=false" in diagnostics,
        "graphics_not_initialized": "graphics_initialized=false" in diagnostics,
        "audio_not_initialized": "audio_initialized=false" in diagnostics,
        "controller_not_initialized": "controller_initialized=false" in diagnostics,
        "working_directory_unmodified": not created_paths,
        "input_executable_unmodified": digest(executable) == before_hash,
        "bounded_exit": not timed_out,
    }
    passed = all(witnesses.values())
    receipt = {
        "test": "full-native-arm64-ps5-startup-admission",
        "passed": passed, "qualification": "closed runtime admission only",
        "commercial_game_qualified": False, "guest_execution": "NOT_RUN" if passed else "UNVERIFIED",
        "binary": str(binary), "binary_sha256": digest(binary),
        "game_root": str(game), "input_executable_sha256": before_hash,
        "argv": command, "exit_code": code, "elapsed_seconds": time.time() - started,
        "witnesses": witnesses, "working_directory_created_paths": created_paths,
        "stdout": str(stdout_path), "stderr": str(stderr_path),
    }
    output = evidence / "receipt.json"
    output.write_text(json.dumps(receipt, indent=2) + "\n")
    print(("PASS" if passed else "FAIL") + ": full ARM64 runtime admission; " + str(output))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
