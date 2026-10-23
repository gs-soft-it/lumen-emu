#!/usr/bin/env python3
# Lumen, 2026-10-05. SPDX-License-Identifier: GPL-2.0-only
"""Execute original x86 FEX fixtures across processes and retain cache evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time

SCHEMA = "LUMEN_FEX_TRANSLATION_CACHE_TEST_V1"
ROOT_ADDRESS = 0x7000000000
ROOT_TRACE = re.compile(
    r"LUMEN_FEX_TRANSLATION_CACHE_ROOT_V1\s+rip=(0x[0-9a-fA-F]+)\s+native_hit=([01])")
INIT_TRACE = re.compile(
    r"LUMEN_FEX_TRANSLATION_CACHE_V1\s+enabled=([01])(?:\s+snapshot_loaded=([01]))?")
COMMIT_TRACE = re.compile(r"LUMEN_FEX_TRANSLATION_CACHE_COMMIT_V1\s+sealed=([01])")
RUN_TRACE = re.compile(
    r"LUMEN_FEX_TRANSLATION_CACHE_RUN_V1\s+rip=(0x[0-9a-fA-F]+)\s+"
    r"jit_compilations=(\d+)\s+native_loads=(\d+)")


def fixture_content_id(value, uncacheable_relocation=False):
    # This is the immutable code page published by FexTranslationCacheTest.
    # Host callback/data pointers are supplied in guest GPRs, never in its bytes.
    program = bytearray.fromhex(
        "b8 00 00 00 00 66 0f 6e c0 66 0f 70 c0 00 66 0f ef c9 "
        "31 d2 b9 00 00 00 00 66 0f fe c8 01 c2 ff c9 75 f6 "
        "f3 0f 7f 0f 66 0f 7e c8 01 d0 41 f6 c0 01 74 05 "
        "83 c0 07 eb 03 83 c0 03 48 89 c6 41 ff d1 c3")
    program[1:5] = struct.pack("<I", value)
    program[21:25] = struct.pack("<I", 4096)
    if uncacheable_relocation:
        # LEA rbx,[rip+disp32] addresses the fixture's non-executable output page.
        program = bytes.fromhex("48 8d 1d") + struct.pack("<i", 4 * os.sysconf("SC_PAGESIZE") - 7) + program
    page = bytearray(b"\xcc" * os.sysconf("SC_PAGESIZE"))
    page[:len(program)] = program
    page[256:262] = bytes.fromhex("b8 49 00 00 00 c3")
    return hashlib.sha256(page).hexdigest()


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def fingerprints(directory):
    result = {}
    if not directory.exists():
        return result
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            result[str(path.relative_to(directory))] = {"symlink": os.readlink(path)}
        elif path.is_file():
            result[str(path.relative_to(directory))] = {"size": path.stat().st_size,
                                                       "sha256": digest(path)}
    return result


def require(condition, detail):
    if not condition:
        raise AssertionError(detail)


def current_snapshot(cache):
    pointers = list(cache.glob("*/current"))
    require(len(pointers) == 1, "expected exactly one build bucket/current pointer")
    pointer = pointers[0]
    name = pointer.read_text()
    require(re.fullmatch(r"snapshot-[A-Za-z0-9-]+", name), "unexpected snapshot pointer")
    snapshot = pointer.parent / name
    require(snapshot.is_dir() and not snapshot.is_symlink(), "missing sealed snapshot")
    return snapshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--evidence-root", type=Path,
                        help="Parent directory for a new private evidence directory; never reused")
    parser.add_argument("--timeout", type=float, default=90,
                        help="Maximum seconds for each synthetic fixture process")
    args = parser.parse_args()
    if not 1 <= args.timeout <= 600:
        parser.error("timeout must be between 1 and 600 seconds")
    binary = args.binary.resolve(strict=True)
    parent = args.evidence_root.resolve(strict=True) if args.evidence_root else None
    evidence = Path(tempfile.mkdtemp(prefix="lumen-fex-translation-cache-", dir=parent)).resolve()
    evidence.chmod(0o700)
    binary_before = digest(binary)
    content_ids = {value: fixture_content_id(value) for value in (42, 43)}
    records = []
    receipt = {"schema": "LUMEN_FEX_TRANSLATION_CACHE_ROUNDTRIP_V1",
               "binary": str(binary), "binary_sha256": binary_before,
               "evidence": str(evidence), "cases": records,
               "content_id_contract": "SHA256 of published immutable fixture x86 code page",
               "fixture_content_ids": content_ids,
               "performance_qualified": False,
               "scope": "original synthetic x86 scalar/SIMD/HLE/nested callbacks; no ROM"}

    def invoke(name, cache_directory, *, value=42, selector="1", freshness=False,
               revoke=False, denied=False, loaded=None, first_hit=None,
               content_id="fixture", exit_mode="normal", uncacheable_relocation=False):
        directory = evidence / "runs" / name
        directory.mkdir(parents=True)
        environment = dict(os.environ)
        for key in tuple(environment):
            if key.startswith("LUMEN_FEX_TRANSLATION_CACHE"):
                del environment[key]
        environment.update({"LUMEN_CPU_TRANSLATOR": "fex",
                            "LUMEN_FEX_TRANSLATION_CACHE": selector,
                            "LUMEN_FEX_TRANSLATION_CACHE_DIR": str(cache_directory),
                            "LUMEN_FEX_TRANSLATION_CACHE_TRACE": "1"})
        if content_id is not None:
            environment["LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID"] = (
                fixture_content_id(value, uncacheable_relocation) if content_id == "fixture" else content_id)
        command = [str(binary), "--value", str(value)]
        if freshness:
            command.append("--freshness")
        if revoke:
            command.append("--revoke")
        if exit_mode == "quick_exit":
            command.append("--quick-exit")
        elif exit_mode == "checkpoint_abrupt":
            command.append("--wait-checkpoint")
        if uncacheable_relocation:
            command.append("--uncacheable-relocation")
        cache = Path(cache_directory)
        # Relative invalid-path tests must not inspect a same-named path in the
        # caller's cwd. All fixture processes run in their own evidence directory.
        inspected_cache = cache if cache.is_absolute() else directory / cache
        record = {"name": name, "argv": command,
                  "cache_directory": str(cache_directory),
                  "environment_overrides": {key: environment[key] for key in (
                      "LUMEN_CPU_TRANSLATOR", "LUMEN_FEX_TRANSLATION_CACHE",
                      "LUMEN_FEX_TRANSLATION_CACHE_DIR", "LUMEN_FEX_TRANSLATION_CACHE_TRACE",
                      "LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID") if key in environment},
                  "cache_before": fingerprints(inspected_cache)}
        records.append(record)
        started = time.monotonic()
        try:
            completed = subprocess.run(command, cwd=directory, env=environment,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       timeout=args.timeout, check=False)
            stdout, stderr = completed.stdout, completed.stderr
            record["exit_code"] = completed.returncode
            record["timed_out"] = False
        except subprocess.TimeoutExpired as error:
            stdout, stderr = error.stdout or b"", error.stderr or b""
            record["exit_code"] = None
            record["timed_out"] = True
        (directory / "stdout.log").write_bytes(stdout)
        (directory / "stderr.log").write_bytes(stderr)
        record["elapsed_seconds"] = time.monotonic() - started
        record["stdout"] = str(directory / "stdout.log")
        record["stderr"] = str(directory / "stderr.log")
        record["cache_after"] = fingerprints(inspected_cache)
        parsed = []
        for line in stdout.decode("utf-8", errors="replace").splitlines():
            try:
                candidate = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(candidate, dict) and candidate.get("schema") == SCHEMA:
                parsed.append(candidate)
        record["fixture"] = parsed[-1] if len(parsed) == 1 else None
        trace = stderr.decode("utf-8", errors="replace")
        record["initialization"] = [dict(enabled=int(enabled),
                                        snapshot_loaded=int(snapshot) if snapshot else None)
                                    for enabled, snapshot in INIT_TRACE.findall(trace)]
        record["roots"] = [dict(rip=address.lower(), native_hit=int(hit))
                           for address, hit in ROOT_TRACE.findall(trace)]
        record["commits"] = [int(sealed) for sealed in COMMIT_TRACE.findall(trace)]
        record["translated_runs"] = [dict(rip=address.lower(), jit_compilations=int(compilations),
                                           native_loads=int(loads))
                                      for address, compilations, loads in RUN_TRACE.findall(trace)]
        record["binary_unchanged"] = digest(binary) == binary_before
        (directory / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        require(not record["timed_out"], f"{name}: fixture timed out")
        require(record["binary_unchanged"], f"{name}: binary changed during test")
        require(len(parsed) == 1,
                f"{name}: expected one fixture JSON record, exit={record['exit_code']}; see {record['stderr']}")
        result = parsed[0]
        if denied:
            require(record["exit_code"] != 0 and result.get("pass") is False and
                    result.get("admission_code") == 16,
                    f"{name}: invalid cache configuration was admitted")
            require(record["cache_before"] == record["cache_after"],
                    f"{name}: denied configuration modified its cache destination")
            return record
        require(record["exit_code"] == 0 and result.get("pass") is True,
                f"{name}: execution/checksum failed")
        require(result["checksum"] == result["expected_checksum"] and
                result["callbacks"] == 1 and result["nested_calls"] == 1,
                f"{name}: scalar/SIMD/HLE result contract")
        require(result["handoff_calls"] > 0 and
                result["handoff_nested_faults"] == result["handoff_calls"],
                f"{name}: shared handoff callback and nested signal were not exercised")
        roots = [item["native_hit"] for item in record["roots"]
                 if int(item["rip"], 16) == ROOT_ADDRESS]
        if selector == "1":
            require(len(record["initialization"]) == 1 and
                    record["initialization"][0]["enabled"] == 1,
                    f"{name}: missing cache initialization trace")
            if loaded is not None:
                require(record["initialization"][0]["snapshot_loaded"] == loaded,
                        f"{name}: unexpected snapshot load state")
            if first_hit is not None:
                require(roots and roots[0] == first_hit,
                        f"{name}: expected first native cache hit={first_hit}, got {roots}")
            if first_hit == 1:
                translated = [item for item in record["translated_runs"]
                              if int(item["rip"], 16) == ROOT_ADDRESS]
                require(translated and translated[0]["native_loads"] > 0,
                        f"{name}: available native cache entry was not loaded during execution")
            if exit_mode == "checkpoint_abrupt":
                require(not record["commits"], f"{name}: abrupt exit ran a shutdown handler")
                current_snapshot(inspected_cache)
            else:
                require(record["commits"] == [1], f"{name}: completed cache was not sealed")
        if freshness:
            require(result["changed_checksum"] == result["changed_expected_checksum"] and
                    result["changed_checksum"] != result["checksum"],
                    f"{name}: mutation reused stale native code")
            require(len(roots) >= 2 and roots[1] == 0,
                    f"{name}: changed bytes under the same content identity did not miss")
        if revoke:
            require(result["revoked_code"] == 12, f"{name}: revoked Execute was not denied")
        if uncacheable_relocation:
            require(result["uncacheable_relocation"] is True and
                    result["relocation_target"] == ROOT_ADDRESS + 4 * os.sysconf("SC_PAGESIZE"),
                    f"{name}: RIP-relative non-executable address result changed")
            translated = [item for item in record["translated_runs"]
                          if int(item["rip"], 16) == ROOT_ADDRESS]
            require(translated and translated[0]["jit_compilations"] > 0 and
                    translated[0]["native_loads"] == 0,
                    f"{name}: uncacheable root was unexpectedly loaded from native cache")
        return record

    try:
        off = evidence / "off-cache"
        off.mkdir(mode=0o700)
        invoke("off", off, selector="0")
        require(not list(off.iterdir()), "cache OFF created files")
        cache = evidence / "cache"
        cold = invoke("cold-42", cache, loaded=0, first_hit=0)
        warm = invoke("warm-42", cache, loaded=1, first_hit=1)
        require(cold["fixture"]["checksum"] == warm["fixture"]["checksum"],
                "cross-process warm checksum differs from cold")
        cold_counts = next(item for item in cold["translated_runs"] if int(item["rip"], 16) == ROOT_ADDRESS)
        warm_counts = next(item for item in warm["translated_runs"] if int(item["rip"], 16) == ROOT_ADDRESS)
        require(warm_counts["jit_compilations"] < cold_counts["jit_compilations"],
                "warm native loading did not reduce actual new JIT compilations")
        receipt["cold_warm_counts"] = {"cold": cold_counts, "warm": warm_counts}
        invoke("uppercase-content-id-warm", cache, loaded=1, first_hit=1,
               content_id=content_ids[42].upper())
        current_snapshot(cache)
        seed = evidence / "sealed-42-seed"
        shutil.copytree(cache, seed)
        # A different immutable content identity gets its own native key space.
        invoke("changed-43", cache, value=43, loaded=0, first_hit=0)
        invoke("warm-43", cache, value=43, loaded=1, first_hit=1)
        freshness_cache = evidence / "freshness-cache"
        shutil.copytree(seed, freshness_cache)
        invoke("freshness-revoke", freshness_cache, freshness=True, revoke=True,
               loaded=1, first_hit=1)
        uncacheable = evidence / "uncacheable-relocation-cache"
        invoke("uncacheable-relocation-cold", uncacheable, loaded=0, first_hit=0,
               uncacheable_relocation=True)
        invoke("uncacheable-relocation-repeat", uncacheable, loaded=1, first_hit=0,
               uncacheable_relocation=True)

        for name in ("truncated-data", "truncated-index", "tampered-data", "tampered-seal"):
            damaged = evidence / name
            shutil.copytree(seed, damaged)
            snapshot = current_snapshot(damaged)
            if name == "truncated-data":
                target = snapshot / "RWCacheDB.foz"
                target.write_bytes(target.read_bytes()[:16])
            elif name == "truncated-index":
                target = snapshot / "RWCacheDB_idx.foz"
                target.write_bytes(target.read_bytes()[:16])
            elif name == "tampered-data":
                target = snapshot / "RWCacheDB.foz"
                content = bytearray(target.read_bytes())
                require(len(content) > 32, "native cache database unexpectedly empty")
                content[-1] ^= 0x80
                target.write_bytes(content)
            else:
                target = snapshot / "seal"
                target.write_text("LUMEN_FEX_CACHE_V1\n" + "0" * 32 + " 1\n" + "0" * 32 + " 1\n")
            invoke(name, damaged, loaded=0, first_hit=0)

        symlink_cache = evidence / "symlink-snapshot-file"
        shutil.copytree(seed, symlink_cache)
        snapshot = current_snapshot(symlink_cache)
        sentinel = evidence / "external-blob-sentinel"
        sentinel.write_bytes(b"cache must never modify this target\n")
        sentinel_before = digest(sentinel)
        target = snapshot / "RWCacheDB.foz"
        target.unlink()
        target.symlink_to(sentinel)
        invoke("symlink-snapshot-file", symlink_cache, loaded=0, first_hit=0)
        require(digest(sentinel) == sentinel_before, "snapshot symlink target was modified")

        destination = evidence / "symlink-directory-target"
        destination.mkdir(mode=0o700)
        link = evidence / "symlink-directory"
        link.symlink_to(destination, target_is_directory=True)
        invoke("symlink-directory-denied", link, denied=True)
        require(not list(destination.iterdir()), "invalid directory symlink wrote through to target")
        invoke("relative-directory-denied", "relative-cache", denied=True)
        invoke("malformed-selector-denied", evidence / "malformed-cache", selector="01", denied=True)
        invoke("missing-content-id-denied", evidence / "missing-id-cache", content_id=None, denied=True)
        invoke("malformed-content-id-denied", evidence / "malformed-id-cache", content_id="42", denied=True)
        quick_cache = evidence / "quick-exit-cache"
        invoke("quick-exit-cold", quick_cache, loaded=0, first_hit=0, exit_mode="quick_exit")
        invoke("quick-exit-warm", quick_cache, loaded=1, first_hit=1)
        checkpoint_cache = evidence / "checkpoint-cache"
        invoke("checkpoint-abrupt-cold", checkpoint_cache, loaded=0, first_hit=0,
               exit_mode="checkpoint_abrupt")
        invoke("checkpoint-abrupt-warm", checkpoint_cache, loaded=1, first_hit=1)
        receipt["pass"] = True
    except (AssertionError, OSError, ValueError) as error:
        receipt["pass"] = False
        receipt["failure"] = str(error)
    receipt["binary_sha256_after"] = digest(binary)
    receipt["binary_unchanged"] = receipt["binary_sha256_after"] == binary_before
    receipt["pass"] = receipt.get("pass", False) and receipt["binary_unchanged"]
    (evidence / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"FEX translation cache roundtrip: {'PASS' if receipt['pass'] else 'FAIL'} evidence={evidence}")
    if not receipt["pass"]:
        print(receipt.get("failure", "binary changed during fixture"))
    return 0 if receipt["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
