#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the Lumen C boundary using a configured, pinned QEMU CPU graph."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PATCH_RECEIPTS = (
    ("qemu-code-ram-offset.patch", "code_ram_hook", "Code-RAM hook"),
    ("qemu-jit-execute-scope.patch", "jit_execute_scope", "JIT execute scope"),
    ("qemu-tb-exec-boundary.patch", "tb_exec_boundary", "TB execution boundary"),
    ("qemu-persistent-ir-cache.patch", "persistent_ir_cache", "persistent IR cache"),
)
CACHE_SOURCE_FIELDS = {
    "bridge_sha256": "LumenQemuTcg.c", "header_sha256": "LumenQemuTcg.h",
    "cache_source_sha256": "LumenQemuTcgCache.c", "cache_header_sha256": "LumenQemuTcgCache.h",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cache_build_identity(arguments, build, upstream, output_directory, host_abi):
    """Bind portable IR to the reviewed translator and compilation ABI."""
    replacements = sorted(((str(ROOT), "<LUMEN>"), (str(upstream), "<QEMU>"),
                           (str(build), "<BUILD>"), (str(output_directory), "<OUTPUT>")),
                          key=lambda item: len(item[0]), reverse=True)
    profile = []
    skip = False
    for argument in arguments:
        if skip:
            skip = False
            continue
        if argument in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if argument in ("-c", "-MD", "-MMD", "-MP") or \
                argument in (str(ROOT / "LumenQemuTcg.c"), str(ROOT / "LumenQemuTcgCache.c")) or \
                argument.startswith("-DLUMEN_QEMU_IR_CACHE_BUILD_ID="):
            continue
        if argument.startswith(("-MF", "-MT", "-MQ")):
            continue
        for path, token in replacements:
            argument = argument.replace(path, token)
        profile.append(argument)
    dependency_lock = json.loads((ROOT / "dependency-lock.json").read_text())
    return {
        "schema": 1, "host_abi": host_abi,
        "source_lock_sha256": digest(ROOT / "source-lock.json"),
        "dependency_lock_sha256": digest(ROOT / "dependency-lock.json"),
        "patches": [[name, dependency_lock["patches"][name]["sha256"]]
                    for name, _, _ in PATCH_RECEIPTS],
        "sources": {field: digest(ROOT / relative) for field, relative in CACHE_SOURCE_FIELDS.items()},
        "compilation_profile": profile,
    }


def cache_build_id(identity):
    return hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def command(arguments, directory, log):
    result = subprocess.run(arguments, cwd=directory, stdout=log,
                            stderr=subprocess.STDOUT, check=False)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {shlex.join(arguments)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True,
                        help="Configured QEMU x86_64-softmmu build")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--verify-code-ram", action="store_true",
                        help="Build a separate native verification library; never use for game measurements")
    args = parser.parse_args()
    build_recipe_sha256 = digest(Path(__file__))
    build = args.build.resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / "source-lock.json").read_text())
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = next(item for item in commands
                 if item["file"].endswith("target/i386/tcg/tcg-cpu.c"))
    source = Path(entry["file"])
    if not source.is_absolute():
        source = Path(entry["directory"]) / source
    upstream = source.resolve().parents[3]
    if (upstream / "VERSION").read_text().strip() != lock["version"]:
        raise ValueError("Configured QEMU version does not match source-lock.json")
    for notice in ("LICENSE", "COPYING", "COPYING.LIB"):
        if not (upstream / notice).is_file():
            raise ValueError(f"Missing QEMU notice: {notice}")
    dependency_lock = json.loads((ROOT / "dependency-lock.json").read_text())
    checked_patches = {}
    final_source_hashes = {}
    for patch_name, _, _ in PATCH_RECEIPTS:
        patch = dependency_lock["patches"][patch_name]
        if digest(ROOT / "patches" / patch_name) != patch["sha256"]:
            raise ValueError(f"Patch does not match dependency-lock.json: {patch_name}")
        for relative, hashes in patch["files"].items():
            if relative in final_source_hashes and final_source_hashes[relative] != hashes["before_sha256"]:
                raise ValueError(f"Checked QEMU patches do not compose: {relative}")
            final_source_hashes[relative] = hashes["after_sha256"]
        checked_patches[patch_name] = patch
    for relative, expected in final_source_hashes.items():
        if digest(upstream / relative) != expected:
            raise ValueError(f"Configured QEMU is missing exact checked patches: {relative}")
    ir_patch = checked_patches["qemu-persistent-ir-cache.patch"]
    ir_header = (upstream / "include/tcg/lumen-ir-cache.h").read_text()
    if ir_patch.get("abi_version") != 1 or \
            re.findall(r"^#define\s+LUMEN_TCG_IR_CACHE_ABI_VERSION\s+(\d+)\s*$",
                       ir_header, re.MULTILINE) != ["1"]:
        raise ValueError("QEMU persistent IR cache helper ABI is not the reviewed version")
    darwin = output.suffix == ".dylib"
    target = "qemu-system-x86_64-unsigned" if darwin else "qemu-system-x86_64"
    obj = output.parent / "LumenQemuTcg.o"
    cache_obj = output.parent / "LumenQemuTcgCache.o"
    compile_args = list(entry.get("arguments") or shlex.split(entry["command"]))
    compile_args[compile_args.index("-o") + 1] = str(obj)
    compile_args[-1] = str(ROOT / "LumenQemuTcg.c")
    compile_args += ["-fPIC", "-I", str(ROOT)]
    if args.verify_code_ram:
        compile_args += ["-DLUMEN_QEMU_TCG_CODE_RAM_VERIFY"]
    build_identity = cache_build_identity(compile_args, build, upstream, output.parent,
                                         "macos-arm64" if darwin else "android-aarch64")
    ir_build_id = cache_build_id(build_identity)
    compile_args += ['-DLUMEN_QEMU_IR_CACHE_BUILD_ID="' + ir_build_id + '"']
    cache_compile_args = list(compile_args)
    cache_compile_args[cache_compile_args.index("-o") + 1] = str(cache_obj)
    cache_compile_args[cache_compile_args.index(str(ROOT / "LumenQemuTcg.c"))] = str(ROOT / "LumenQemuTcgCache.c")
    log_path = output.with_suffix(output.suffix + ".build.log")
    with log_path.open("w") as log:
        command(["ninja", f"-j{args.jobs}", target], build, log)
        command(compile_args, build, log)
        command(cache_compile_args, build, log)
        # compdb -x expands Ninja's link response files from its build graph,
        # including when the transient .rsp file was removed after linking.
        database = json.loads(subprocess.check_output(
            ["ninja", "-t", "compdb", "-x"], cwd=build, text=True))
        link_entry = next(item for item in database if item.get("output") == target)
        link_args = shlex.split(link_entry["command"])
        link_args[link_args.index("-o") + 1] = str(output)
        original_main = f"{target}.p/system_main.c.o"
        link_args[link_args.index(original_main)] = str(obj)
        link_args.insert(link_args.index(str(obj)) + 1, str(cache_obj))
        link_args = [arg for arg in link_args
                     if arg not in ("-pie", "-Wl,-pie", "-fPIE", "-fpie")]
        link_map = output.with_suffix(output.suffix + ".map")
        functions = re.findall(r"\b(LumenQemuTcg\w+)\s*\(", (ROOT / "LumenQemuTcg.h").read_text())
        if args.verify_code_ram:
            functions += re.findall(r"\b(LumenQemuTcg\w+)\s*\(",
                                    (ROOT / "LumenQemuTcgCodeRamTest.h").read_text())
        if darwin:
            link_args += ["-dynamiclib", "-Wl,-install_name,@rpath/" + output.name]
            link_args += ["-Wl,-map," + str(link_map)]
            link_args += ["-Wl,-exported_symbol,_" + name for name in sorted(set(functions))]
        else:
            version_script = output.parent / "lumen-qemu-exports.map"
            version_script.write_text("{ global: " + "; ".join(sorted(set(functions)))
                                      + "; local: *; };\n")
            link_args += ["-shared", "-Wl,-soname," + output.name,
                          "-Wl,--version-script=" + str(version_script),
                          "-Wl,-Map," + str(link_map),
                          "-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384"]
        command(link_args, build, log)
    if cache_build_identity(compile_args, build, upstream, output.parent,
                            build_identity["host_abi"]) != build_identity or \
            digest(Path(__file__)) != build_recipe_sha256:
        raise ValueError("QEMU IR cache build inputs changed during compilation")
    for relative, expected in final_source_hashes.items():
        if digest(upstream / relative) != expected:
            raise ValueError(f"Checked QEMU source changed during compilation: {relative}")
    receipt = {
        "qemu_version": lock["version"],
        "build_recipe_sha256": build_recipe_sha256,
        "qemu_source": str(upstream),
        "compile_arguments": compile_args,
        "cache_compile_arguments": cache_compile_args,
        "build_directory": str(build),
        "ir_cache_build_id": ir_build_id,
        "ir_cache_build_identity": build_identity,
        "link_arguments": link_args,
        "bridge_sha256": digest(ROOT / "LumenQemuTcg.c"),
        "header_sha256": digest(ROOT / "LumenQemuTcg.h"),
        "cache_source_sha256": digest(ROOT / "LumenQemuTcgCache.c"),
        "cache_header_sha256": digest(ROOT / "LumenQemuTcgCache.h"),
        "library": str(output),
        "library_sha256": digest(output),
        "link_map": str(link_map),
        "link_map_sha256": digest(link_map),
        "runtime_tested": False,
        "code_ram_hook_patch_sha256": checked_patches["qemu-code-ram-offset.patch"]["sha256"],
        "code_ram_hook_source_sha256": {relative: digest(upstream / relative)
            for relative in checked_patches["qemu-code-ram-offset.patch"]["files"]},
        "jit_execute_scope_patch_sha256": checked_patches["qemu-jit-execute-scope.patch"]["sha256"],
        "jit_execute_scope_source_sha256": {relative: digest(upstream / relative)
            for relative in checked_patches["qemu-jit-execute-scope.patch"]["files"]},
        "tb_exec_boundary_patch_sha256": checked_patches["qemu-tb-exec-boundary.patch"]["sha256"],
        "tb_exec_boundary_source_sha256": {relative: digest(upstream / relative)
            for relative in checked_patches["qemu-tb-exec-boundary.patch"]["files"]},
        "persistent_ir_cache_patch_sha256": checked_patches["qemu-persistent-ir-cache.patch"]["sha256"],
        "persistent_ir_cache_abi_version": ir_patch["abi_version"],
        "persistent_ir_cache_source_sha256": {relative: digest(upstream / relative)
            for relative in checked_patches["qemu-persistent-ir-cache.patch"]["files"]},
        "code_ram_verification_only": args.verify_code_ram,
    }
    output.with_suffix(output.suffix + ".json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"library": str(output), "sha256": receipt["library_sha256"]}))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, StopIteration, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
