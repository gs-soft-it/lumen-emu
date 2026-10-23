#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate and stage the optional macOS TCG library's legal provenance."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    require(path.is_file(), f"Missing QEMU legal provenance: {path}")
    return json.loads(path.read_text())


def collect(library, deployment_target):
    library = library.resolve()
    result = subprocess.run([sys.executable, str(ROOT / "tools/VerifyLibrary.py"),
                             "--library", str(library), "--host", "macos",
                             "--deployment-target", deployment_target],
                            capture_output=True, text=True)
    require(result.returncode == 0, f"QEMU legal staging verification failed: {result.stderr.strip()}")
    receipt_path = Path(str(library) + ".json")
    audit_path = Path(str(library) + ".macos.json")
    receipt, audit = read_json(receipt_path), read_json(audit_path)
    for name in ("qemu_version", "library_sha256", "bridge_sha256", "header_sha256",
                 "cache_source_sha256", "cache_header_sha256", "ir_cache_build_id",
                 "persistent_ir_cache_abi_version"):
        require(audit.get(name) == receipt[name], f"QEMU macOS audit mismatch: {name}")
    for key, source in (("source_lock_sha256", ROOT / "source-lock.json"),
                        ("dependency_lock_sha256", ROOT / "dependency-lock.json"),
                        ("recipe_sha256", ROOT / "tools/BuildMac.py"),
                        ("build_recipe_sha256", ROOT / "tools/BuildLibrary.py"),
                        ("verifier_sha256", ROOT / "tools/VerifyLibrary.py")):
        require(audit.get(key) == digest(source), f"QEMU macOS provenance changed: {source}")
    require(audit.get("architecture") == "arm64" and
            audit.get("binary_distribution_qualified") is False,
            "QEMU macOS audit must preserve its native/local qualification scope")
    notices = library.parent / "licenses"
    notice_manifest_path = notices / "manifest.json"
    notice_manifest = read_json(notice_manifest_path)
    require(audit.get("notice_manifest_sha256") == digest(notice_manifest_path) and
            notice_manifest.get("library_sha256") == receipt["library_sha256"],
            "QEMU notice manifest is not bound to this macOS library")
    required = {"qemu/LICENSE", "qemu/COPYING", "qemu/COPYING.LIB", "glib/COPYING",
                "glib/LICENSES/LGPL-2.1-or-later.txt", "pcre2/COPYING", "pcre2/LICENCE.md",
                "libintl/COPYING.LIB"}
    entries = notice_manifest.get("files", {})
    require(required.issubset(entries), "QEMU transitive upstream notices are incomplete")
    files = {"build-receipt.json": receipt_path, "macos-audit.json": audit_path,
             "licenses/manifest.json": notice_manifest_path}
    for relative, expected in entries.items():
        path = notices / relative
        require(not Path(relative).is_absolute() and ".." not in Path(relative).parts and
                path.resolve().is_relative_to(notices.resolve()) and path.is_file() and
                digest(path) == expected, f"QEMU legal notice mismatch: {relative}")
        files["licenses/" + relative] = path
    for relative in ("LumenQemuTcg.c", "LumenQemuTcg.h", "LumenQemuTcgCache.c", "LumenQemuTcgCache.h",
                     "source-lock.json", "dependency-lock.json",
                     "tools/BuildMac.py", "tools/BuildLibrary.py", "tools/VerifyLibrary.py",
                     "tools/BuildAndroid.py", "tools/StageMacLegal.py", "patches/glib-core-only.patch",
                     "patches/qemu-virtual-tsc-clock.patch", "patches/qemu-code-ram-offset.patch",
                     "patches/qemu-jit-execute-scope.patch", "patches/qemu-tb-exec-boundary.patch",
                     "patches/qemu-persistent-ir-cache.patch"):
        source = ROOT / relative
        require(source.is_file(), f"Missing QEMU build provenance: {source}")
        files["source/" + relative] = source
    hashes = {relative: digest(path) for relative, path in files.items()}
    provenance = {"schema": "LUMEN_MACOS_QEMU_LEGAL_V1", "qemu_version": receipt["qemu_version"],
                  "source_library": str(library), "source_library_sha256": receipt["library_sha256"],
                  "deployment_target": deployment_target, "files": hashes,
                  "source_files": {str(path): hashes[relative] for relative, path in files.items()},
                  "license_verdict": "NOT CERTIFIED", "full_per_file_license_closure_qualified": False,
                  "binary_distribution_qualified": False}
    return files, provenance


def stage(library, deployment_target, output, refresh=False):
    files, provenance = collect(library, deployment_target)
    require(not output.is_symlink(), f"QEMU legal destination cannot be a symlink: {output}")
    output.mkdir(parents=True, exist_ok=True)
    for relative, source in files.items():
        target = output / relative
        require(not target.is_symlink() and target.resolve().is_relative_to(output.resolve()),
                f"QEMU legal destination escapes its staging root: {target}")
        require(refresh or not target.exists() or digest(target) == provenance["files"][relative],
                f"Existing QEMU legal provenance differs; rebuild the source bundle: {target}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    (output / "provenance.json").write_text(json.dumps(provenance, indent=2, sort_keys=True) + "\n")
    return provenance


def verify_staged(output, source_sha256):
    provenance = read_json(output / "provenance.json")
    require(provenance.get("source_library_sha256") == source_sha256 and
            provenance.get("binary_distribution_qualified") is False and
            provenance.get("full_per_file_license_closure_qualified") is False,
            "Packaged QEMU provenance/qualification mismatch")
    for relative, expected in provenance["files"].items():
        path = output / relative
        require(not Path(relative).is_absolute() and ".." not in Path(relative).parts and
                path.resolve().is_relative_to(output.resolve()) and path.is_file() and
                digest(path) == expected, f"Packaged QEMU legal resource mismatch: {relative}")
    receipt = read_json(output / "build-receipt.json")
    audit = read_json(output / "macos-audit.json")
    require(receipt["library_sha256"] == source_sha256 and audit["library_sha256"] == source_sha256,
            "Packaged QEMU source receipts do not match the original binary")
    require(digest(output / "source/LumenQemuTcg.c") == receipt["bridge_sha256"] and
            digest(output / "source/LumenQemuTcg.h") == receipt["header_sha256"] and
            digest(output / "source/LumenQemuTcgCache.c") == receipt["cache_source_sha256"] and
            digest(output / "source/LumenQemuTcgCache.h") == receipt["cache_header_sha256"],
            "Packaged QEMU source/ABI receipt mismatch")
    return provenance


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--deployment-target", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cmake-output", type=Path)
    parser.add_argument("--refresh", action="store_true", help="Refresh CMake's generated staging copies")
    args = parser.parse_args()
    output = args.output.resolve()
    provenance = stage(args.library, args.deployment_target, output, args.refresh)
    if args.cmake_output:
        resources = []
        for path in sorted(output / relative for relative in [*provenance["files"], "provenance.json"]):
            relative = path.relative_to(output)
            component = "qemu-tcg" + ("/" + str(relative.parent) if str(relative.parent) != "." else "")
            value = f"{component}|{path}".replace("\\", "\\\\").replace('"', '\\"').replace(";", "\\;")
            resources.append('  "' + value + '"')
        args.cmake_output.write_text("set(LUMEN_QEMU_LEGAL_RESOURCES\n" + "\n".join(resources) + "\n)\n")
    print(json.dumps({"output": str(output), "files": len(provenance["files"]),
                      "source_library_sha256": provenance["source_library_sha256"],
                      "binary_distribution_qualified": False}))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
