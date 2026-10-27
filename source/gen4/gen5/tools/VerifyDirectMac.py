#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Audit an already built native Gen5/direct-FEX graph without executing it."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import plistlib
import re
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
PIN = "f2b679f6028ce1c38875233aecfcf5d3f8ebecec"
MSL_PIN = "d8e3e2b141b8c8a167b2e3984736a6baacff316c"


def capture(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip()


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_cache(path):
    result = {}
    for line in path.read_text().splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if ":" in key:
            result[key.split(":", 1)[0]] = value
    return result


def resolve(build, value):
    path = Path(value)
    return (path if path.is_absolute() else build / path).resolve()


def verify(args):
    repository = Path(__file__).resolve().parents[4]
    build = args.build.resolve(strict=True)
    cache = read_cache(build / "CMakeCache.txt")
    require(platform.system() == "Darwin" and platform.machine() == "arm64" and
            capture("getconf", "PAGESIZE") == "16384", "native Darwin arm64/16 KiB host required")
    require(cache.get("CMAKE_OSX_ARCHITECTURES") == "arm64", "build is not thin arm64")
    require(cache.get("CMAKE_OSX_DEPLOYMENT_TARGET") == "12.0", "unexpected deployment target")
    require(cache.get("LUMEN_PS5_DIRECT_FEX") == "ON" and cache.get("LUMEN_PS5_FEXCORE") == "ON",
            "build has not selected the Gen5 direct FEX engine")
    require(Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() ==
            repository / "source/gen4/gen5/compatibility", "build belongs to another source graph")
    require(cache.get("LUMEN_GEN4_NATIVE_DARWIN_JIT") == "1", "Darwin JIT/W^X configure gate absent")
    native_metal = cache.get("LUMEN_PS5_NATIVE_METAL") == "ON"

    source = Path(cache["LUMEN_GEN4_DIRECT_FEX_SOURCE_DIR"]).resolve(strict=True)
    overlay = Path(cache["LUMEN_PS5_DIRECT_FEX_SOURCE_REAL"]).resolve(strict=True)
    manifest_path = overlay / ".lumen-overlay.json"
    manifest = json.loads(manifest_path.read_text())
    preparer_path = repository / "source/gen4/cmake/direct_fex/prepare_darwin.py"
    spec = importlib.util.spec_from_file_location("direct_darwin_manifest", preparer_path)
    preparer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(preparer)
    require(preparer.PIN == PIN, "Darwin preparer changed its expected FEX pin")
    identity = {"schema": 1, "pin": PIN, "submodules": preparer.SUBMODULES,
                "darwin_patch_sha256": digest(preparer_path.with_name("fex-f2b679f-darwin.patch")),
                "preparer_sha256": digest(preparer_path)}
    require(manifest["identity"] == identity, "Darwin overlay identity differs from the selected pin/patch")
    require(capture("git", "-C", str(source), "remote", "get-url", "origin") ==
            "https://github.com/FEX-Emu/FEX.git", "FEX source origin mismatch")
    for relative, pin in {"": PIN, **preparer.SUBMODULES}.items():
        component = source / relative
        require(capture("git", "-C", str(component), "rev-parse", "HEAD") == pin,
                f"FEX source pin mismatch: {relative}")
        require(not capture("git", "-C", str(component), "status", "--porcelain",
                            "--untracked-files=all", "--ignore-submodules=none"),
                f"FEX upstream component has local changes: {relative}")

    binary = build / "kyty_emulator"
    require(binary.is_file(), "build kyty_emulator before auditing")
    require(capture("/usr/bin/lipo", "-archs", str(binary)) == "arm64", "executable is not thin arm64")
    commands = capture("ninja", "-C", str(build), "-t", "commands", "kyty_emulator").splitlines()
    compiled = {}
    link_tokens = None
    for line in commands:
        tokens = shlex.split(line)
        if "-c" in tokens and "-o" in tokens:
            unit = resolve(build, tokens[tokens.index("-c") + 1])
            output = resolve(build, tokens[tokens.index("-o") + 1])
            compiled[output] = (unit, tokens)
        elif "-o" in tokens and resolve(build, tokens[tokens.index("-o") + 1]) == binary:
            link_tokens = tokens
    require(link_tokens is not None and compiled, "could not inspect the executable's Ninja dependency closure")
    for output, (unit, _) in compiled.items():
        require(output.is_file(), f"compiled object missing: {output}")
        require(unit.stat().st_mtime_ns <= output.stat().st_mtime_ns,
                f"source is newer than its object; rebuild before auditing: {unit}")
        require(output.stat().st_mtime_ns <= binary.stat().st_mtime_ns,
                f"object is newer than the executable; relink before auditing: {output}")
    compile_database = json.loads((build / "compile_commands.json").read_text())
    database_pairs = {(resolve(Path(row["directory"]), row["output"]),
                       resolve(Path(row["directory"]), row["file"]))
                      for row in compile_database}
    require(all((output, unit) in database_pairs for output, (unit, _) in compiled.items()),
            "Ninja compile closure differs from compile_commands.json")
    units = {unit for unit, _ in compiled.values()}
    gen5 = repository / "source/gen4/gen5"
    required = {gen5 / relative for relative in (
        "direct_fex/DirectFexRuntime.cpp", "direct_fex/DirectFexProviderSelection.cpp",
        "engine/src/loader/fexRuntime.cpp", "engine/src/loader/guestExecution.cpp",
        "engine/src/kernel/memory.cpp", "engine/src/emulator.cpp",
        "compatibility/src/main.cpp")}
    require(required <= units, "direct bridge, production engine or CLI is absent from compile closure")
    metal_directory = repository / "source/emulator/src/Graphics/Metal"
    if native_metal:
        require({metal_directory / "MetalVulkanCore.mm", metal_directory / "MetalVulkanCommands.mm"} <= units,
                "native Metal implementation is absent from the executable closure")
    forbidden = ("/macos/fexcore-runtime/", "/gen5-compatibility-sources/", "/source/cpu/qemu/")
    for output, (unit, tokens) in compiled.items():
        require(not any(part in str(unit) for part in forbidden), f"legacy CPU/engine source compiled: {unit}")
        require(unit.name != "AndroidFexCoreRuntime.cpp", "legacy FEX facade source compiled")
        if unit.is_relative_to(gen5 / "compatibility/src"):
            require(unit == gen5 / "compatibility/src/main.cpp", f"legacy Gen5 source compiled: {unit}")
        if unit.is_relative_to(gen5 / "engine/src") or unit.is_relative_to(gen5 / "direct_fex"):
            require("-DLUMEN_PS5_DIRECT_FEX=1" in tokens, f"direct define missing: {unit}")
        if native_metal and (unit.is_relative_to(gen5 / "engine/src") or unit.is_relative_to(metal_directory)):
            require("-DLUMEN_PS5_NATIVE_METAL=1" in tokens, f"native Metal selection missing: {unit}")
        if output.is_relative_to(build / "direct-fex/gen4"):
            require(unit.is_relative_to(overlay), f"translator/dependency compiled outside its verified overlay: {unit}")

    # Only archives actually present in the final link command count. Inventory
    # the translator/dependency closure once, not every third-party source tree.
    link_archives = [resolve(build, token) for token in link_tokens if token.endswith(".a")]
    expected_names = ("liblumen_ps5_direct_engine.a", "liblumen_ps5_direct_fex.a", "libFEXCore.a",
                      "libFEXCore_Base.a", "liblumen_gen4_fex_host_features.a", "liblumen_gen4_fex_config.a",
                      "libfmt.a", "libxxhash.a", "libsoftfloat_3e.a", "libcephes_128bit.a",
                      "libJemallocLibs.a", "librpmalloc.a", "libtiny-json.a")
    if native_metal:
        expected_names += ("liblumen_ps5_native_metal.a", "libspirv-cross-msl.a",
                           "libspirv-cross-glsl.a", "libspirv-cross-core.a")
    archives = {}
    for name in expected_names:
        matches = [path for path in link_archives if path.name == name]
        require(len(matches) == 1, f"expected exactly one linked {name}")
        path = matches[0]
        require(path.is_relative_to(build), f"direct closure imports an external archive: {path}")
        require(capture("/usr/bin/lipo", "-archs", str(path)) == "arm64", f"archive is not thin arm64: {path}")
        require(path.stat().st_mtime_ns <= binary.stat().st_mtime_ns,
                f"archive is newer than the executable; relink before auditing: {path}")
        archives[name] = {"path": str(path), "sha256": digest(path), "architecture": "arm64"}
    require(not any("macos_fexcore" in str(path) or "qemu" in str(path) for path in link_archives),
            "legacy or alternate CPU archive linked")
    graphics = {"backend": "external-vulkan", "native_metal": False}
    if native_metal:
        require(not any("moltenvk" in token.lower() or "libvulkan" in token.lower() for token in link_tokens),
                "native Metal graph links an external Vulkan implementation")
        imports = capture("/usr/bin/otool", "-L", str(binary))
        require("Metal.framework" in imports and "QuartzCore.framework" in imports,
                "native Metal frameworks are absent from the executable imports")
        require(not re.search(r"moltenvk|libvulkan", imports, re.IGNORECASE),
                "executable imports an external Vulkan implementation")
        msl_units = [unit for unit in units if unit.name == "spirv_msl.cpp"]
        require(len(msl_units) == 1, "expected exactly one SPIRV-Cross MSL compiler")
        msl_source = msl_units[0].parent
        require(capture("git", "-C", str(msl_source), "rev-parse", "HEAD") == MSL_PIN,
                "SPIRV-Cross MSL revision differs from the shared Metal pin")
        require(not capture("git", "-C", str(msl_source), "status", "--porcelain", "--untracked-files=all"),
                "SPIRV-Cross MSL checkout has local changes")
        graphics = {"backend": "native-metal", "native_metal": True,
                    "spirv_cross_msl_pin": MSL_PIN, "spirv_cross_msl_source": str(msl_source),
                    "dynamic_imports": imports}
    fex_objects = [output for output in compiled if "FEXCore_object.dir" in str(output)]
    require(fex_objects, "no FEXCore object compilation found")
    members = Counter(capture("/usr/bin/ar", "-t", archives["libFEXCore.a"]["path"]).splitlines())
    require(not (Counter(path.name for path in fex_objects) - members),
            "compiled FEXCore objects do not match the linked FEXCore archive members")

    # Inspect includes from the actual compiled object dependency records. This
    # verifies the used Darwin overlay bytes, including headers, against its
    # immutable manifest without rehashing unused upstream tooling/tests.
    dependencies = set(units)
    selected = None
    objects_with_dependencies = set()
    cli_headers = set()
    for line in capture("ninja", "-C", str(build), "-t", "deps").splitlines():
        if line and not line.startswith(" "):
            candidate = resolve(build, line.split(": #deps", 1)[0])
            selected = candidate if candidate in compiled else None
            if selected is not None:
                require(line.endswith("(VALID)"), f"Ninja dependency record is stale: {selected}")
                objects_with_dependencies.add(selected)
        elif selected and line.startswith("    "):
            dependency = resolve(build, line.strip())
            require(dependency.stat().st_mtime_ns <= selected.stat().st_mtime_ns,
                    f"include is newer than its object; rebuild before auditing: {dependency}")
            dependencies.add(dependency)
            if compiled[selected][0] == gen5 / "compatibility/src/main.cpp":
                cli_headers.add(dependency)
                require(not dependency.is_relative_to(gen5 / "compatibility/src") or
                        dependency == gen5 / "compatibility/src/main.cpp",
                        f"direct CLI includes a legacy header with a different runtime ABI: {dependency}")
    require({gen5 / "engine/src/emulator.h", gen5 / "engine/src/common/emulatorConfig.h"} <= cli_headers,
            "direct CLI must compile against the production engine RunOptions/ConfigOptions headers")
    # CMake's glob verifier and Git-version target always run, even on a fresh
    # graph; a Ninja dry run cannot establish freshness. Check the actual
    # objects and dependency records instead, without executing build commands.
    require(set(fex_objects) <= objects_with_dependencies,
            "compiled FEXCore object dependency records are missing")
    overlay_files = sorted(path for path in dependencies if path.is_relative_to(overlay))
    require(overlay_files, "no compiled overlay files in Ninja dependency records")
    for path in overlay_files:
        relative = path.relative_to(overlay).as_posix()
        require(manifest["files"].get(relative) == digest(path), f"used FEX overlay file changed: {relative}")

    link_map = build / "kyty_emulator.map"
    # Apple's map also contains literal C-string symbols with arbitrary bytes.
    # Preserve them losslessly while parsing the object-path section as text.
    map_text = link_map.read_text(errors="surrogateescape")
    object_lines = re.findall(r"^\[\s*\d+\]\s+(.+)$", map_text, re.MULTILINE)
    require(any("liblumen_ps5_direct_fex.a" in line and "DirectFexRuntime.cpp.o" in line for line in object_lines),
            "link map has no direct runtime bridge object")
    require(any("libFEXCore.a" in line and "Core.cpp.o" in line for line in object_lines),
            "link map has no real FEX core object")
    require(not any("macos_fexcore_runtime" in line or "AndroidFexCoreRuntime.cpp.o" in line for line in object_lines),
            "link map includes the legacy facade")
    if native_metal:
        require(all(any("liblumen_ps5_native_metal.a" in line and member in line for line in object_lines)
                    for member in ("MetalVulkanCore.mm.o", "MetalVulkanCommands.mm.o")),
                "link map has no real Metal core/command objects")
    # The static Metal entry point is the only exported symbol. CPU/engine
    # symbols remain defined locally; inspect them as well as the export trie.
    symbols = capture("/usr/bin/nm", "-U", str(binary))
    require(not re.search(r"\b_LumenAndroidFexCore\w*", symbols), "legacy facade symbols are present")
    demangled = subprocess.check_output(["/usr/bin/c++filt"], input=symbols, text=True)
    required_symbols = ("Lumen::Ps5::DirectFex::Probe(", "Lumen::Ps5::DirectFex::RunGuestV2WithStack(",
                        "Core::Fex::GuestEngine::Create(", "FEXCore::Context::Context::CreateNewContext(")
    require(all(symbol in demangled for symbol in required_symbols), "direct translator symbols missing")
    # Mach-O may contain non-UTF-8 byte strings. Only the ASCII identity marker
    # is relevant here; do not interpret arbitrary binary strings as paths.
    strings = subprocess.check_output(["/usr/bin/strings", "-a", str(binary)])
    require(b"LUMEN_PS5_DIRECT_FEX_RUNTIME_V1" in strings and b"pin=f2b679f6 engine=gen4-direct" in strings,
            "direct provider identity is absent from executable")

    signature = capture("/usr/bin/codesign", "--verify", "--strict", "--verbose=2", str(binary))
    signing_details = capture("/usr/bin/codesign", "-dvv", str(binary))
    require("runtime" in signing_details, "executable lacks Hardened Runtime signing")
    entitlements = plistlib.loads(subprocess.check_output(
        ["/usr/bin/codesign", "--display", "--entitlements", ":-", str(binary)], stderr=subprocess.DEVNULL))
    require(entitlements.get("com.apple.security.cs.allow-jit") is True, "allow-jit entitlement absent")
    require(not entitlements.get("com.apple.security.cs.allow-unsigned-executable-memory", False),
            "direct runtime must not require unsigned executable memory")
    if native_metal:
        require(not entitlements.get("com.apple.security.cs.disable-library-validation", False),
                "static Metal runtime must retain library validation")

    # Includes record the shared Gen4 implementation included by the bridge as
    # well as Gen5 headers; third-party sources are represented by the pinned
    # manifest and archive hashes above.
    project_files = {path.relative_to(repository).as_posix(): digest(path)
                     for path in sorted(dependencies)
                     if path.is_file() and (path.is_relative_to(repository / "source/gen4") or
                                            path.is_relative_to(metal_directory)) and
                     not path.is_relative_to(gen5 / "compatibility/3rdparty")}
    receipt = {
        "schema": 1, "result": "PASS", "verified_at_utc": datetime.now(timezone.utc).isoformat(),
        "repository": str(repository), "source_head": capture("git", "-C", str(repository), "rev-parse", "HEAD"),
        "build": str(build), "configuration": cache["CMAKE_BUILD_TYPE"], "architecture": "arm64",
        "host_page_size": 16384, "provider": "gen4-direct", "fex_pin": PIN,
        "graphics": graphics,
        "upstream_source": str(source), "darwin_overlay": str(overlay), "overlay_identity": identity,
        "overlay_manifest_sha256": digest(manifest_path), "verified_used_overlay_files": len(overlay_files),
        "compiled_fex_core_objects": len(fex_objects), "compiled_engine_units": sum(
            unit.is_relative_to(gen5 / "engine/src") for unit in units),
        "archives": archives, "project_files": project_files,
        "executable": {"path": str(binary), "sha256": digest(binary), "direct_symbols": list(required_symbols)},
        "link_map": {"path": str(link_map), "sha256": digest(link_map)},
        "signature": {"verification": signature, "details": signing_details, "entitlements": entitlements},
        "scope": "Build/source/link/signature audit only; no runtime, ROM, graphics or performance qualification",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"Verified native Gen5 direct FEX closure: {args.output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        verify(args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"Gen5 direct Mac audit rejected: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
