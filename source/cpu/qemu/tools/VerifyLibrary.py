#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check the native C-boundary binary against its source and build receipt."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

from BuildLibrary import (CACHE_SOURCE_FIELDS, PATCH_RECEIPTS, cache_build_id,
                          cache_build_identity)

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def version_tuple(value):
    if not re.fullmatch(r"\d+(?:\.\d+){0,2}", value):
        raise ValueError(f"Invalid macOS deployment version: {value}")
    parts = tuple(map(int, value.split(".")))
    return parts + (0,) * (3 - len(parts))


def verify_macos(library, deployment_target):
    if deployment_target is None:
        raise ValueError("macOS verification requires --deployment-target")
    expected = version_tuple(deployment_target)
    arch = subprocess.check_output(["lipo", "-archs", str(library)], text=True).strip()
    if arch != "arm64":
        raise ValueError(f"QEMU requires thin native ARM64, observed {arch}")
    commands = subprocess.check_output(["otool", "-l", str(library)], text=True)
    blocks = re.split(r"\nLoad command \d+\n", commands)[1:]
    versions, identifiers, imports = [], [], []
    for block in blocks:
        command = re.search(r"^\s*cmd\s+(\S+)\s*$", block, re.MULTILINE)
        if command is None:
            raise ValueError("Malformed Mach-O load command")
        kind = command[1]
        if kind == "LC_BUILD_VERSION":
            platform = re.search(r"^\s*platform\s+(\S+)\s*$", block, re.MULTILINE)
            minimum = re.search(r"^\s*minos\s+(\S+)\s*$", block, re.MULTILINE)
            if platform is None or platform[1].lower() not in ("1", "macos") or minimum is None:
                raise ValueError("QEMU requires an explicit macOS LC_BUILD_VERSION")
            versions.append(minimum[1])
        elif kind == "LC_ID_DYLIB" or kind in (
                "LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
                "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"):
            name = re.search(r"^\s*name\s+(.+?)\s+\(offset\s+\d+\)\s*$", block, re.MULTILINE)
            if name is None:
                raise ValueError("Malformed Mach-O library name")
            (identifiers if kind == "LC_ID_DYLIB" else imports).append(name[1])
    if len(versions) != 1:
        raise ValueError(f"QEMU requires one macOS LC_BUILD_VERSION, observed {versions}")
    if version_tuple(versions[0]) > expected:
        raise ValueError(f"QEMU macOS minimum {versions} exceeds deployment target {deployment_target}")
    if identifiers != ["@rpath/liblumen_qemu_tcg.dylib"]:
        raise ValueError(f"QEMU install name mismatch: {identifiers}")
    if not imports or any(not path.startswith(("/usr/lib/", "/System/Library/Frameworks/"))
                          or ".." in Path(path).parts for path in imports):
        raise ValueError(f"QEMU macOS requires system library imports only: {imports}")
    return {"architecture": arch, "deployment_target": deployment_target,
            "minimum_macos": versions[0], "install_name": identifiers[0],
            "dynamic_dependencies": imports}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--host", choices=("macos", "android"), required=True)
    parser.add_argument("--deployment-target", help="Maximum admitted macOS LC_BUILD_VERSION minimum")
    parser.add_argument("--allow-code-ram-verification", action="store_true",
                        help="Admit a test-only library for native fixtures, never for app configuration")
    args = parser.parse_args()
    if args.host == "android" and args.deployment_target is not None:
        raise ValueError("--deployment-target applies to macOS verification only")
    library = args.library.resolve()
    receipt = json.loads(Path(str(library) + ".json").read_text())
    lock = json.loads((ROOT / "source-lock.json").read_text())
    patches = json.loads((ROOT / "dependency-lock.json").read_text())["patches"]
    if receipt.get("code_ram_verification_only") and not args.allow_code_ram_verification:
        raise ValueError("Code-RAM verification library cannot be used by the app")
    patch_receipts = PATCH_RECEIPTS
    final_source_hashes = {}
    for patch_name, _, _ in patch_receipts:
        for relative, hashes in patches[patch_name]["files"].items():
            if relative in final_source_hashes and final_source_hashes[relative] != hashes["before_sha256"]:
                raise ValueError(f"Checked QEMU patches do not compose: {relative}")
            final_source_hashes[relative] = hashes["after_sha256"]
    for patch_name, receipt_prefix, label in patch_receipts:
        hook = patches[patch_name]
        if digest(ROOT / "patches" / patch_name) != hook["sha256"] or \
                receipt.get(receipt_prefix + "_patch_sha256") != hook["sha256"] or \
                receipt.get(receipt_prefix + "_source_sha256") != {
                    relative: final_source_hashes[relative] for relative in hook["files"]}:
            raise ValueError(f"QEMU {label} does not match the pinned source/build receipt")
    if patches["qemu-persistent-ir-cache.patch"].get("abi_version") != 1 or \
            receipt.get("persistent_ir_cache_abi_version") != 1:
        raise ValueError("QEMU persistent IR cache does not match the reviewed helper ABI")
    identity = cache_build_identity(receipt["compile_arguments"], Path(receipt["build_directory"]),
                                    Path(receipt["qemu_source"]), Path(receipt["library"]).parent,
                                    "macos-arm64" if args.host == "macos" else "android-aarch64")
    cache_identity = cache_build_identity(receipt["cache_compile_arguments"],
                                         Path(receipt["build_directory"]), Path(receipt["qemu_source"]),
                                         Path(receipt["library"]).parent, identity["host_abi"])
    build_id = cache_build_id(identity)
    expected_macro = '-DLUMEN_QEMU_IR_CACHE_BUILD_ID="' + build_id + '"'
    if receipt.get("ir_cache_build_identity") != identity or cache_identity != identity or \
            receipt.get("ir_cache_build_id") != build_id or \
            any([argument for argument in receipt[field]
                 if argument.startswith("-DLUMEN_QEMU_IR_CACHE_BUILD_ID=")] != [expected_macro]
                for field in ("compile_arguments", "cache_compile_arguments")) or \
            build_id.encode("ascii") not in library.read_bytes():
        raise ValueError("QEMU persistent IR cache build identity does not match the source/build receipt")
    macos = verify_macos(library, args.deployment_target) if args.host == "macos" else {}
    checks = ((receipt["qemu_version"], lock["version"], "version"),
              (receipt["build_recipe_sha256"], digest(ROOT / "tools/BuildLibrary.py"), "library build recipe"),
              *((receipt[field], digest(ROOT / relative), relative)
                for field, relative in CACHE_SOURCE_FIELDS.items()),
              (receipt["library_sha256"], digest(library), "library"))
    for observed, expected, name in checks:
        if observed != expected:
            raise ValueError(f"QEMU {name} does not match the source/build receipt")
    if args.host == "macos":
        arch = macos["architecture"]
    else:
        blob = library.read_bytes()
        if blob[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", blob, 18)[0] != 183:
            raise ValueError("QEMU Android library must be little-endian AArch64 ELF64")
        offset = struct.unpack_from("<Q", blob, 32)[0]
        stride, count = struct.unpack_from("<HH", blob, 54)
        if stride < 56 or count == 0 or offset + stride * count > len(blob):
            raise ValueError("Malformed ELF program-header table")
        loads = 0
        for index in range(count):
            header = offset + stride * index
            if struct.unpack_from("<I", blob, header)[0] == 1:
                loads += 1
                align = struct.unpack_from("<Q", blob, header + 48)[0]
                if align < 16384:
                    raise ValueError("QEMU Android LOAD segment is not 16 KiB aligned")
        if loads == 0:
            raise ValueError("QEMU Android library has no LOAD segments")
        arch = "aarch64"
    print(json.dumps({"host": args.host, "architecture": arch,
                      **macos,
                      "library_sha256": receipt["library_sha256"],
                      "ir_cache_build_id": build_id,
                      "code_ram_verification_only": receipt.get("code_ram_verification_only", False),
                      "runtime_qualified": False, "distribution_qualified": False}))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, struct.error, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
