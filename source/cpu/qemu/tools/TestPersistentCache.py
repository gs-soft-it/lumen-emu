#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify QEMU IR cache effects across native process restarts and revocation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time


CACHE_HEADER_SIZE = 144


def require(condition, explanation):
    if not condition:
        raise RuntimeError(explanation)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cache_entries(directory):
    entries = sorted(directory.glob("*/*.lir"))
    for path in entries:
        metadata = path.lstat()
        require(stat.S_ISREG(metadata.st_mode), f"Unexpected cache entry: {path}")
        require(metadata.st_uid == os.getuid(), f"Wrong cache file owner: {path}")
        require(metadata.st_mode & 0o077 == 0, f"Cache file is not private: {path}")
    return entries


def run_fixture(fixture, scratch, name, cache_dir=None, pretranslate=None,
                variant=0, writable=False, replace=False, expect_rejection=False,
                extra_environment=None, disturb_query_jit=False, unproven_code=False,
                fill_cache=False, overflow_unsupported=False, auto_cache=False,
                relocated=False, local_warm=False):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("LUMEN_QEMU_TCG_")}
    environment.update({"LUMEN_QEMU_TCG_CHAINING": "1",
                        "LUMEN_QEMU_TCG_PARALLEL": "1"})
    if cache_dir is None and not auto_cache:
        environment["LUMEN_QEMU_TCG_IR_CACHE"] = "0"
    if pretranslate is not None:
        environment["LUMEN_QEMU_TCG_PRETRANSLATE"] = "1" if pretranslate else "0"
    if cache_dir is not None:
        environment["LUMEN_QEMU_TCG_IR_CACHE_DIR"] = str(cache_dir)
    environment.update(extra_environment or {})
    arguments = [str(fixture), "--run", "--variant", str(variant)]
    if writable:
        arguments.append("--writable-code")
    if replace:
        arguments.append("--replace-code")
    if expect_rejection:
        arguments.append("--expect-init-rejected")
    if disturb_query_jit:
        arguments.append("--disturb-query-jit")
    if unproven_code:
        arguments.append("--unproven-code")
    if fill_cache:
        arguments.append("--fill-cache")
    if overflow_unsupported:
        arguments.append("--overflow-unsupported")
    if relocated:
        arguments.append("--relocate-code")
    if local_warm:
        arguments.append("--quota-local-warm")
    started = time.monotonic_ns()
    try:
        completed = subprocess.run(arguments, cwd=scratch, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, timeout=60, check=False)
    except subprocess.TimeoutExpired as error:
        def timeout_text(value):
            return value.decode(errors="replace") if isinstance(value, bytes) else value or ""
        timeout_record = {"name": name, "arguments": arguments,
                          "returncode": "TIMEOUT", "timeout_seconds": error.timeout,
                          "process_elapsed_ns": time.monotonic_ns() - started,
                          "stdout": timeout_text(error.stdout),
                          "stderr": timeout_text(error.stderr)}
        timeout_path = scratch / f"{name}.json"
        timeout_path.write_text(json.dumps(timeout_record, indent=2) + "\n")
        raise RuntimeError(f"{name}: process timeout; preserved output in {timeout_path}") from error
    record = {"name": name, "arguments": arguments,
              "returncode": completed.returncode,
              "process_elapsed_ns": time.monotonic_ns() - started,
              "stdout": completed.stdout, "stderr": completed.stderr}
    (scratch / f"{name}.json").write_text(json.dumps(record, indent=2) + "\n")
    if expect_rejection:
        require(completed.returncode == 0,
                f"{name}: initialization did not reject safely: {completed.stderr}")
        rejection = [json.loads(line) for line in completed.stdout.splitlines()
                     if line.startswith('{"result":"REJECTED"')]
        require(len(rejection) == 1 and rejection[0]["initialization"] == 6,
                f"{name}: invalid configuration was not explicitly rejected")
        return record
    require(completed.returncode == 0,
            f"{name}: fixture failed ({completed.returncode}): {completed.stderr}")
    results = []
    for line in completed.stdout.splitlines():
        if line.startswith('{"result":"PASS"'):
            results.append(json.loads(line))
    require(len(results) == 1, f"{name}: missing or duplicate fixture result")
    result = results[0]
    expected_variant = 0 if local_warm else 103 if fill_cache else variant ^ 1 if replace else variant
    executions = 104 + int(local_warm) if fill_cache else 2 if replace else 1
    require(result["variant"] == expected_variant,
            f"{name}: wrong code variant executed")
    require(result["immutable_proof"] == (not writable and not unproven_code),
            f"{name}: wrong immutable code promise")
    require(result["quota_fill"] == fill_cache and result["simd_exercised"] == overflow_unsupported,
            f"{name}: requested fixture workload did not execute")
    require(result["code_pc"] == (0x3000 if relocated else 0x1000) and
            result["quota_local_warm"] == local_warm,
            f"{name}: requested placement or local publication replay did not execute")
    require(result["hle_calls"] == executions,
            f"{name}: missing HLE boundary")
    require(result["host_boundary_queries"] == executions,
            f"{name}: speculative guest execution reached an extra HLE boundary")
    if disturb_query_jit:
        require(result["query_jit_changes"] > 0,
                f"{name}: JIT protection callback control did not run")
    for counter in ("hits", "misses", "stores", "rejected", "pretranslated"):
        require(isinstance(result["stats"][counter], int) and
                result["stats"][counter] >= 0, f"{name}: bad {counter} counter")
    require(result["stats"]["pretranslated"] == 0,
            f"{name}: unavailable pretranslation path was executed")
    record["fixture"] = result
    return record


def test_quota(fixture, scratch):
    cache = scratch / "quota-cache"
    cache.mkdir(mode=0o700)
    records = []

    def run(name, **options):
        record = run_fixture(fixture, scratch, name, cache_dir=cache, **options)
        records.append(record)
        return record

    fill = run("quota_fill", fill_cache=True, local_warm=True)["fixture"]
    entries = cache_entries(cache)
    require(len(entries) == 512 and fill["stats"]["stores"] == 512,
            "Real guest translations did not enforce the 512-entry quota")
    require(fill["hle_calls"] == 105 and fill["checksum"] == fill["first_checksum"] and
            fill["stats"]["hits"] == 5,
            "Guest effects or local record imports failed after writer quota exhaustion")
    warm = run("quota_warm")["fixture"]
    require(warm["checksum"] == fill["first_checksum"] and
            warm["stats"]["hits"] == 5 and warm["stats"]["stores"] == 0,
            "Full cache did not retain valid imports without new writer work")
    overflow_record = run("quota_unsupported_overflow", overflow_unsupported=True,
                          extra_environment={"LUMEN_QEMU_TCG_IR_CACHE_TRACE": "1"})
    overflow = overflow_record["fixture"]
    summaries = [line for line in overflow_record["stderr"].splitlines()
                 if line.startswith("LUMEN_QEMU_TCG_IR_CACHE_STATS_V1")]
    require(len(summaries) == 1 and " unsupported_ir=0 " in summaries[0],
            "Full-cache admission still attempted unsupported IR export work")
    require(overflow["checksum"] == fill["first_checksum"] and
            overflow["stats"]["hits"] == 0 and overflow["stats"]["stores"] == 0 and
            overflow["stats"]["misses"] > 0 and len(cache_entries(cache)) == 512,
            "SIMD guest fallback did not preserve effects under a full cache")

    def key_queries(record):
        lines = [line for line in record["stderr"].splitlines()
                 if line.startswith("LUMEN_QEMU_TCG_IR_CACHE_STATS_V1")]
        require(len(lines) == 1, "Missing full-cache diagnostic summary")
        fields = dict(field.split("=", 1) for field in lines[0].split()[1:])
        return int(fields["key_queries"])

    relocated_record = run("quota_relocated", relocated=True,
                           extra_environment={"LUMEN_QEMU_TCG_IR_CACHE_TRACE": "1"})
    relocated = relocated_record["fixture"]
    require(relocated["checksum"] == fill["first_checksum"] and
            relocated["stats"]["hits"] == 0 and relocated["stats"]["stores"] == 0 and
            relocated["stats"]["misses"] == 5 and key_queries(relocated_record) == 0,
            "Full-cache static misses still proved or hashed code, or changed guest effects")

    # An uninspectable snapshot must preserve the ordinary checked lookup.
    # Do not let a malformed header turn the filter into an import authority.
    original = entries[0].read_bytes()
    malformed_header = bytearray(original)
    malformed_header[0] ^= 0x80
    entries[0].write_bytes(malformed_header)
    uncertain_record = run("quota_uncertain_index", relocated=True,
                           extra_environment={"LUMEN_QEMU_TCG_IR_CACHE_TRACE": "1"})
    uncertain = uncertain_record["fixture"]
    require(uncertain["checksum"] == fill["first_checksum"] and
            uncertain["stats"]["hits"] == 0 and uncertain["stats"]["stores"] == 0 and
            uncertain["stats"]["misses"] == 5 and key_queries(uncertain_record) == 5,
            "An uncertain header snapshot did not use the ordinary checked JIT lookup")
    entries[0].write_bytes(original)

    # Full admission disables all writers for this process. A corrupted
    # existing record must remain an ordinary JIT miss without repair work.
    for path in entries:
        payload = bytearray(path.read_bytes())
        require(len(payload) > CACHE_HEADER_SIZE, "Full-cache entry lacks payload")
        payload[-1] ^= 0x80
        path.write_bytes(payload)
    corrupt = run("quota_corrupt")["fixture"]
    require(corrupt["checksum"] == fill["first_checksum"] and
            corrupt["stats"]["hits"] == 0 and corrupt["stats"]["stores"] == 0 and
            corrupt["stats"]["rejected"] > 0 and len(cache_entries(cache)) == 512,
            "Full-cache corruption did not preserve guest effects without writer work")
    return {"result": "PASS", "entry_count": len(entries), "records": records,
            "unsupported_overflow_trace": summaries[0]}


def test_cache(fixture, scratch, include_quota=False):
    cache = scratch / "cache"
    cache.mkdir(mode=0o700)
    require(stat.S_IMODE(cache.stat().st_mode) == 0o700, "Cache root is not mode 0700")
    records = []

    def run(name, **options):
        record = run_fixture(fixture, scratch, name, **options)
        records.append(record)
        return record.get("fixture")

    control = run("uncached")
    require(all(value == 0 for value in control["stats"].values()),
            "Cache disabled: persistent cache counters changed")
    explicitly_disabled = scratch / "disabled-cache"
    explicitly_disabled.mkdir(mode=0o700)
    disabled = run("explicit_directory_disabled", cache_dir=explicitly_disabled,
                   extra_environment={"LUMEN_QEMU_TCG_IR_CACHE": "0"})
    require(disabled["checksum"] == control["checksum"] and
            all(value == 0 for value in disabled["stats"].values()) and
            not cache_entries(explicitly_disabled),
            "Explicit cache-off selector did not disable a configured directory")

    if sys.platform == "darwin":
        native_cache_home = scratch / "native-cache-home"
        native_cache_home.mkdir(mode=0o700)
        native_options = {"auto_cache": True,
                          "extra_environment": {"XDG_CACHE_HOME": str(native_cache_home)}}
        native_cold = run("native_default_cold", **native_options)
        native_cache = native_cache_home / "lumen-qemu-tcg-ir"
        require(native_cold["checksum"] == control["checksum"] and
                native_cold["stats"]["stores"] == 5 and
                native_cold["stats"]["misses"] == 5 and
                native_cold["stats"]["hits"] == 0 and
                native_cold["stats"]["pretranslated"] == 0,
                "Default native cache did not store IR with pretranslation disabled")
        require(native_cache.is_dir() and
                stat.S_IMODE(native_cache.stat().st_mode) == 0o700 and
                len(cache_entries(native_cache)) == 5,
                "Default native cache did not use the isolated private cache path")
        native_warm = run("native_default_warm", **native_options)
        require(native_warm["checksum"] == control["checksum"] and
                native_warm["stats"]["hits"] == 5 and
                native_warm["stats"]["stores"] == 0 and
                native_warm["stats"]["pretranslated"] == 0,
                "Default native cache did not reuse IR with pretranslation disabled")

    cold = run("cold", cache_dir=cache)
    require(cold["checksum"] == control["checksum"], "Cold cache changed guest effects")
    require(cold["stats"]["stores"] > 0 and cold["stats"]["misses"] > 0,
            "Cold process did not store translated IR")
    require(cold["stats"]["hits"] == 0 and cold["stats"]["pretranslated"] == 0,
            "Empty cache reported reuse or pretranslation")
    original_entries = cache_entries(cache)
    require(original_entries, "No persistent IR files created by cold process")

    # Explicit zero remains a supported compatibility configuration.
    warm = run("warm", cache_dir=cache, pretranslate=False)
    require(warm["checksum"] == control["checksum"], "Warm cache changed guest effects")
    require(warm["stats"]["hits"] > 0 and warm["stats"]["pretranslated"] == 0,
            "Warm process did not reuse IR through execution-time translation")
    changed = run("changed_bytes", cache_dir=cache, variant=1)
    require(changed["checksum"] != control["checksum"], "Changed code reused stale effects")
    require(changed["stats"]["hits"] == 0 and changed["stats"]["stores"] > 0,
            "Changed page bytes did not invalidate the old IR identity")
    restored = run("restored_bytes", cache_dir=cache)
    require(restored["checksum"] == control["checksum"] and
            restored["stats"]["hits"] > 0,
            "Code identities did not coexist correctly in the persistent cache")
    replaced = run("replace_live_code", cache_dir=cache, replace=True)
    require(replaced["first_checksum"] == control["checksum"] and
            replaced["checksum"] == changed["checksum"],
            "Live VM invalidation failed to expose the replacement guest code")
    writable = run("writable_code", cache_dir=cache, writable=True)
    require(writable["checksum"] == control["checksum"], "Writable code changed guest effects")
    require(writable["stats"]["hits"] == 0 and writable["stats"]["stores"] == 0 and
            writable["stats"]["pretranslated"] == 0,
            "Writable executable bytes entered the immutable persistent cache")
    unproven = run("unproven_code", cache_dir=cache, unproven_code=True)
    require(unproven["checksum"] == control["checksum"] and
            all(value == 0 for value in unproven["stats"].values()),
            "Code without a complete alias immutability proof entered the persistent cache")

    if sys.platform == "darwin":
        jit_control = run("jit_callback_control", disturb_query_jit=True)
        require(jit_control["checksum"] == control["checksum"] and
                all(value == 0 for value in jit_control["stats"].values()),
                "Cache-off JIT callback control changed guest effects")
        jit_cache = scratch / "jit-callback-cache"
        jit_cache.mkdir(mode=0o700)
        for name in ("jit_callback_cold", "jit_callback_warm"):
            jit_result = run(name, cache_dir=jit_cache,
                             disturb_query_jit=True)
            require(jit_result["checksum"] == control["checksum"],
                    f"{name}: memory query JIT mode changed guest effects")
            if name == "jit_callback_cold":
                require(jit_result["stats"]["stores"] > 0,
                        "JIT callback cold process did not export IR")
            else:
                require(jit_result["stats"]["hits"] > 0,
                        f"{name}: JIT callback process did not import IR")

    corrupted = []
    for path in cache_entries(cache):
        payload = bytearray(path.read_bytes())
        require(len(payload) > CACHE_HEADER_SIZE, f"Cache entry has no IR payload: {path}")
        old_digest = digest(path)
        payload[-1] ^= 0x80
        path.write_bytes(payload)
        corrupted.append({"entry": str(path.relative_to(cache)),
                          "before_sha256": old_digest, "after_sha256": digest(path)})
    require(corrupted, "No IR payload was available for tampering")
    recovered = run("corrupt_payload", cache_dir=cache)
    require(recovered["checksum"] == control["checksum"],
            "Rejected IR did not preserve guest effects through JIT")
    require(recovered["stats"]["rejected"] > 0 and recovered["stats"]["stores"] > 0 and
            recovered["stats"]["hits"] == 0,
            "Tampered payload was not rejected and regenerated")
    recovered_warm = run("recovered_warm", cache_dir=cache)
    require(recovered_warm["checksum"] == control["checksum"] and
            recovered_warm["stats"]["hits"] > 0,
            "Regenerated IR could not be reused by a fresh process")

    # All malformed entries below must fail before import. Each negative
    # exercises the real guest program and repairs its records through JIT.
    for path in cache_entries(cache):
        path.write_bytes(path.read_bytes()[:CACHE_HEADER_SIZE - 1])
    truncated = run("truncated_header", cache_dir=cache)
    require(truncated["checksum"] == control["checksum"] and
            truncated["stats"]["rejected"] > 0 and
            truncated["stats"]["stores"] > 0 and truncated["stats"]["hits"] == 0,
            "Truncated header did not safely regenerate guest translations")

    for path in cache_entries(cache):
        payload = bytearray(path.read_bytes())
        # Declared length is little endian at offset 104. Deliberately keep
        # the actual file small: no untrusted length may drive allocation.
        payload[104:108] = b"\xff\xff\xff\xff"
        path.write_bytes(payload)
    oversized = run("oversized_length", cache_dir=cache)
    require(oversized["checksum"] == control["checksum"] and
            oversized["stats"]["rejected"] > 0 and
            oversized["stats"]["stores"] > 0 and oversized["stats"]["hits"] == 0,
            "Oversized payload declaration did not safely regenerate through JIT")

    entry_paths = cache_entries(cache)
    canary = scratch / "symlink-canary"
    canary.write_bytes(b"Borrowed host file: cache storage must not modify this.\n")
    canary_hash = digest(canary)
    for path in entry_paths:
        path.unlink()
        path.symlink_to(canary)
    symlink_entry = run("symlink_entry", cache_dir=cache)
    require(symlink_entry["checksum"] == control["checksum"] and
            symlink_entry["stats"]["rejected"] > 0 and
            symlink_entry["stats"]["stores"] > 0 and symlink_entry["stats"]["hits"] == 0,
            "Symlink entry was not rejected and regenerated through JIT")
    require(digest(canary) == canary_hash, "Cache followed a symlink and changed its target")
    # Unexecuted variant records can remain symlinks. Remove only the exact
    # links created above; the regeneration of accessed records is preserved.
    for path in entry_paths:
        if path.is_symlink():
            require(path.readlink() == canary, "Unexpected cache symlink replacement")
            path.unlink()

    for path in cache_entries(cache):
        entry = bytearray(path.read_bytes())
        payload = entry[CACHE_HEADER_SIZE:]
        require(payload[:4] == b"LIR1", "Unexpected IR wire schema in generated entry")
        globals_count = int.from_bytes(payload[8:12], "little")
        temps_count = int.from_bytes(payload[12:16], "little")
        labels_count = int.from_bytes(payload[16:20], "little")
        # IR v1: header52, temp28, label4, then operation records. Corrupt
        # an opcode while recomputing the outer digest, so this test proves
        # structural IR rejection independently of the integrity checksum.
        first_op = 52 + (temps_count - globals_count) * 28 + labels_count * 4
        require(temps_count >= globals_count and first_op + 4 <= len(payload),
                "Generated IR has inconsistent operation bounds")
        entry[CACHE_HEADER_SIZE + first_op:CACHE_HEADER_SIZE + first_op + 4] = b"\xff" * 4
        entry[112:144] = hashlib.sha256(entry[CACHE_HEADER_SIZE:]).digest()
        path.write_bytes(entry)
    invalid_ir = run("invalid_ir_opcode", cache_dir=cache)
    require(invalid_ir["checksum"] == control["checksum"] and
            invalid_ir["stats"]["rejected"] > 0 and
            invalid_ir["stats"]["stores"] > 0 and invalid_ir["stats"]["hits"] == 0,
            "Structurally invalid IR was not rejected and regenerated through JIT")

    symlink_root = scratch / "symlink-cache"
    symlink_root.symlink_to(cache, target_is_directory=True)
    run("symlink_directory", cache_dir=symlink_root, expect_rejection=True)
    file_root = scratch / "file-cache"
    file_root.write_text("Cache roots must be directories.\n")
    run("file_directory", cache_dir=file_root, expect_rejection=True)

    unsafe = scratch / "unsafe-cache"
    unsafe.mkdir(mode=0o700)
    unsafe.chmod(0o777)
    run("unsafe_directory", cache_dir=unsafe, expect_rejection=True)
    malformed = scratch / "malformed-cache"
    malformed.mkdir(mode=0o700)
    run("malformed_cache_selector", cache_dir=malformed, expect_rejection=True,
        extra_environment={"LUMEN_QEMU_TCG_IR_CACHE": "invalid"})
    run("unavailable_pretranslate_cache_on", cache_dir=cache,
        pretranslate=True, expect_rejection=True)
    run("unavailable_pretranslate_cache_off", pretranslate=True, expect_rejection=True)
    run("malformed_pretranslate", cache_dir=malformed, expect_rejection=True,
        extra_environment={"LUMEN_QEMU_TCG_PRETRANSLATE": "invalid"})
    quota = test_quota(fixture, scratch) if include_quota else None
    if quota:
        records.extend(quota["records"])
    expected_process_count = (27 if sys.platform == "darwin" else 22) + (6 if quota else 0)
    require(len(records) == expected_process_count,
            "Native cache verification did not execute the complete process matrix")
    receipt = {"result": "PASS", "scope": "native_synthetic_guest_effects",
               "fixture": str(fixture), "fixture_sha256": digest(fixture),
               "process_count": len(records), "records": records,
               "tampered_entries": corrupted,
               "game_loading_performance_qualified": False,
               "game_fps_qualified": False}
    if quota:
        receipt["quota"] = {key: value for key, value in quota.items() if key != "records"}
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True,
                        help="Native LumenQemuTcgCacheTest executable linked to the candidate core")
    parser.add_argument("--work", type=Path,
                        help="Retain a newly created private scratch directory under this parent")
    parser.add_argument("--receipt", type=Path,
                        help="Write the complete process outputs and verification receipt here")
    parser.add_argument("--quota", action="store_true",
                        help="Also verify real guest effects and writer gating at the 512-entry quota")
    args = parser.parse_args()
    fixture = args.fixture.resolve(strict=True)
    require(fixture.is_file() and os.access(fixture, os.X_OK), "Fixture is not executable")
    if args.work:
        args.work.mkdir(parents=True, exist_ok=True)
        scratch = Path(tempfile.mkdtemp(prefix="persistent-cache-", dir=args.work.resolve()))
        receipt = test_cache(fixture, scratch, args.quota)
        receipt["scratch"] = str(scratch)
        (scratch / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    else:
        with tempfile.TemporaryDirectory(prefix="lumen-qemu-persistent-cache-") as temporary:
            receipt = test_cache(fixture, Path(temporary), args.quota)
    if args.receipt:
        args.receipt.parent.mkdir(parents=True, exist_ok=True)
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
