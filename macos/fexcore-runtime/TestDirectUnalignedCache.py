#!/usr/bin/env python3
# Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
"""Prove split-granule scalar helpers in fresh cold/warm native-cache processes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--evidence-root", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    root = args.evidence_root.resolve(strict=True)
    evidence = Path(tempfile.mkdtemp(prefix="direct-unaligned-cache-", dir=root)).resolve()
    evidence.chmod(0o700)
    cache = evidence / "cache"
    records = []
    receipt = {"schema": "LUMEN_FEX_DIRECT_UNALIGNED_CACHE_ROUNDTRIP_V1",
               "binary": str(binary), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
               "evidence": str(evidence), "runs": records,
               "performance_qualified": False}
    passed = False
    try:
        for selector, mode in ((0, "small"), (1, "small"), (None, "small"), (1, "dense"), (1, "dense-ro")):
            direct = 1 if selector is None else selector
            dense = mode != "small"
            denied = mode == "dense-ro"
            program = bytes.fromhex("48 8b 47 0f 48 89 47 1f 48 8b 4f 1f") * (1000 if dense else 1)
            if dense:
                program += bytes.fromhex("44 0f b6 16 4d 01 d3") * 500
            expected_fault_rip = 0x7000000000 + len(program)
            if denied:
                program += bytes.fromhex("c6 06 7f")
            program += bytes.fromhex("c3")
            page = bytearray(b"\xcc" * os.sysconf("SC_PAGESIZE"))
            require(len(program) <= len(page), "dense fixture exceeds the owned code page")
            page[:len(program)] = program
            content_id = hashlib.sha256(page).hexdigest()
            # An absent selector must reuse the explicit-ON native translation,
            # execute its STATE callback, and retain the full payload oracle.
            for warm in ((1,) if selector is None else (0, 1)):
                name = f"{mode}-{'default' if selector is None else f'direct{direct}'}-{'warm' if warm else 'cold'}"
                directory = evidence / name
                directory.mkdir()
                environment = {key: os.environ[key] for key in ("HOME", "PATH", "TMPDIR") if key in os.environ}
                environment.update({"LUMEN_CPU_TRANSLATOR": "fex", "LUMEN_FEX_MACOS_RCPC": "0",
                    "LUMEN_FEX_PERFORMANCE_TRACE": "1",
                    "LUMEN_FEX_TRANSLATION_CACHE": "1", "LUMEN_FEX_TRANSLATION_CACHE_DIR": str(cache),
                    "LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID": content_id, "LUMEN_FEX_TRANSLATION_CACHE_TRACE": "1"})
                if selector is not None:
                    environment["LUMEN_FEX_MACOS_DIRECT_UNALIGNED"] = str(selector)
                if denied:
                    environment["LUMEN_MACOS_FEXCORE_FAULT_CONTEXT"] = "1"
                command = [str(binary)] + ([f"--{mode}"] if dense else [])
                completed = subprocess.run(command, cwd=directory, env=environment,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=40, check=False)
                (directory / "stdout.log").write_bytes(completed.stdout)
                (directory / "stderr.log").write_bytes(completed.stderr)
                output = completed.stdout.decode("utf-8", errors="replace")
                trace = completed.stderr.decode("utf-8", errors="replace")
                fixture = json.loads(output.strip())
                roots = re.findall(r"LUMEN_FEX_TRANSLATION_CACHE_ROOT_V1 rip=(0x[0-9a-f]+) native_hit=([01])", trace)
                hits = [int(hit) for rip, hit in roots if int(rip, 16) == 0x7000000000]
                record = {"name": name, "exit_code": completed.returncode, "fixture": fixture,
                    "environment": environment, "command": command, "content_id": content_id,
                    "root_native_hits": hits,
                    "snapshot_loaded": re.findall(r"snapshot_loaded=([01])", trace),
                    "stdout": str(directory / "stdout.log"), "stderr": str(directory / "stderr.log")}
                records.append(record)
                require(completed.returncode == 0 and fixture.get("pass") is True,
                    f"{name}: split-granule scalar result, canary or counter contract failed")
                require(fixture.get("direct") == direct and fixture.get("payload_canaries") is True,
                    f"{name}: missing direct mode or payload witness")
                require(fixture.get("selector_absent") == (selector is None),
                        f"{name}: the selector environment does not match the default contract")
                require(fixture.get("dense") == dense and fixture.get("denied") == denied and
                        fixture.get("protected_canaries") is True, f"{name}: mode or protected payload witness missing")
                if dense:
                    require(fixture.get("emitter_restarts") == 0 if warm else fixture.get("emitter_restarts", 0) > 0,
                        f"{name}: cold fixture must exercise buffer restart; warm must consume the native cache")
                if denied:
                    recovered = re.findall(r"MACOS_FEXCORE_FAULT_CONTEXT_V4[^\n]*scope=guest[^\n]*guestRip=(0x[0-9a-f]+)", trace)
                    record["recovered_guest_rips"] = recovered
                    require([int(rip, 16) for rip in recovered] == [expected_fault_rip] and
                            int(fixture["expected_fault_rip"], 16) == expected_fault_rip,
                            f"{name}: native byte fault did not reconstruct the exact guest RIP after restart")
                require(record["snapshot_loaded"] == [str(warm)] and hits == [warm],
                    f"{name}: expected distinct cold domain or warm native hit, got {record}")
                if warm:
                    counters = re.findall(r"LUMEN_FEX_TRANSLATION_CACHE_RUN_V1 rip=(0x[0-9a-f]+) jit_compilations=(\d+) native_loads=(\d+)", trace)
                    root_counters = [(int(jit), int(loads)) for rip, jit, loads in counters if int(rip, 16) == 0x7000000000]
                    require(root_counters and root_counters[0][0] == 0 and root_counters[0][1] >= 1,
                        f"{name}: warm root did not consume native translation exclusively")
        require(len(list(cache.glob("*/current"))) == 4,
                "small selector pair and two dense programs must create four independent cache domains")
        require(hashlib.sha256(binary.read_bytes()).hexdigest() == receipt["binary_sha256"], "fixture binary changed")
        passed = True
    finally:
        receipt["pass"] = passed
        (evidence / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
        print(json.dumps({"schema": receipt["schema"], "pass": passed, "receipt": str(evidence / "receipt.json")}))


if __name__ == "__main__":
    main()
