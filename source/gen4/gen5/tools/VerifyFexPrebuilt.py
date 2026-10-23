#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify and inventory the isolated, already built macOS FEX link closure."""
import argparse
import hashlib
import importlib.util
import json
import platform
from pathlib import Path
import re
import shlex
import subprocess
import sys

from PrepareCompatibilitySources import prepare

sys.dont_write_bytecode = True


PIN = "c9c5a132c18175656936b8c25c1d453fe6e262a0"
ARCHIVES = {
    "runtime": "liblumen_macos_fexcore_runtime.a",
    "core": "fex-core/Source/libFEXCore.a",
    "base": "fex-core/Source/libFEXCore_Base.a",
    "fmt": "fex-fmt/libfmt.a",
    "xxhash": "fex-xxhash/libxxhash.a",
    "cephes": "fex-cephes/libcephes_128bit.a",
    "softfloat": "fex-softfloat/libsoftfloat_3e.a",
    "jemalloc": "fex-core/Source/libJemallocLibs.a",
    "rpmalloc": "fex-rpmalloc/librpmalloc.a",
}


def capture(*argv):
    return subprocess.check_output(argv, text=True, stderr=subprocess.STDOUT).strip()


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def read_cache(path):
    result = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(("#", "//")) and "=" in line and ":" in line.split("=", 1)[0]:
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def verify(args):
    repository = args.repository.resolve(strict=True)
    parent = args.parent.resolve(strict=True)
    cache = read_cache(parent / "CMakeCache.txt")
    if platform.system() != "Darwin" or platform.machine() != "arm64" or capture("getconf", "PAGESIZE") != "16384":
        raise ValueError("native macOS arm64 with 16 KiB host pages is required")
    if Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() != repository:
        raise ValueError("the parent build belongs to a different source repository")
    if cache.get("CMAKE_BUILD_TYPE") != "Release":
        raise ValueError("the prebuilt PS5 FEX closure currently requires a Release parent build; "
                         "configure the parent with -DCMAKE_BUILD_TYPE=Release")
    if cache.get("CMAKE_OSX_ARCHITECTURES") != "arm64" or cache.get("CMAKE_OSX_DEPLOYMENT_TARGET") != "12.0":
        raise ValueError("parent target architecture/deployment differs from the audited closure")
    if cache.get("LUMEN_MACOS_FEX_HAS_PUBLIC_MAP_JIT_WX") != "1":
        raise ValueError("parent build lacks the required public MAP_JIT W^X check")
    source = Path(cache["LUMEN_MACOS_FEXCORE_SOURCE_DIR"]).resolve(strict=True)
    if source.is_relative_to(repository):
        raise ValueError("FEX base must be external to the new repository")
    git = lambda *argv: capture("git", "-C", str(source), *argv)
    if git("rev-parse", "HEAD") != PIN or git("remote", "get-url", "origin") != "https://github.com/FEX-Emu/FEX.git":
        raise ValueError("FEX base origin or exact commit mismatch")
    if git("status", "--porcelain=v1", "--untracked-files=all", "--ignore-submodules=none"):
        raise ValueError("FEX base must remain clean")
    overlay = Path(cache["FEXCore_SOURCE_DIR"]).resolve(strict=True).parent
    manifest = json.loads((overlay / ".lumen-overlay.json").read_text())
    port = repository / "macos/fexcore-runtime"
    port_cmake = (port / "CMakeLists.txt").read_text()
    patch_specs = re.findall(r'"\$\{(LUMEN_FEX_[A-Z0-9_]+)\}=([0-9a-f]{64})"', port_cmake)
    reviewed_hashes = [expected for _, expected in patch_specs]
    for variable, expected in patch_specs:
        definition = re.search(r'set\(' + re.escape(variable) + r'\s+"\$\{CMAKE_CURRENT_LIST_DIR\}/([^"]+)"\)', port_cmake)
        if not definition or digest(port / definition.group(1)) != expected:
            raise ValueError("current reviewed patch content differs: " + variable)
    if not reviewed_hashes or manifest["identity"] != {"base": PIN, "source": str(source), "patches": reviewed_hashes}:
        raise ValueError("overlay identity differs from the current reviewed parent patch set")
    # Reuse the parent's content verifier, including its symlink inventory.
    spec = importlib.util.spec_from_file_location("lumen_overlay_verifier", port / "prepare_source_overlay.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if manifest["files"] != module.inventory(overlay):
        raise ValueError("reviewed source overlay content has changed")
    # The combined graph places FEX at the root when PS4 is enabled and inside
    # ps5 for a PS5-only configure. Never mix closures from the two layouts.
    candidates = [path for path in (parent / "macos-fexcore-runtime", parent / "ps5/fexcore-runtime")
                  if (path / ARCHIVES["runtime"]).is_file()]
    if len(candidates) != 1:
        raise ValueError("expected exactly one completed parent FEX closure at "
                         "macos-fexcore-runtime or ps5/fexcore-runtime; "
                         "build lumen_macos_fexcore_runtime in a clean parent build")
    archive_root = candidates[0]
    archives = {}
    for name, relative in ARCHIVES.items():
        path = (archive_root / relative).resolve(strict=True)
        arch = capture("/usr/bin/lipo", "-archs", str(path))
        if arch != "arm64":
            raise ValueError(f"{name}: archive must contain only native arm64, got {arch}")
        archives[name] = {"path": str(path), "sha256": digest(path), "architecture": arch}
    compatibility = prepare(repository / "source/gen4/gen5",
                            parent / "gen5-compatibility-sources")
    source_files = {str(relative): digest(repository / relative) for relative in (
        Path("android/app/src/main/cpp/AndroidFexCoreRuntime.h"),
        Path("android/app/src/main/cpp/AndroidFexCoreRuntime.cpp"))}
    source_files.update({str(compatibility / relative): digest(compatibility / relative)
                        for relative in ("src/loader/guestExecution.h",
                            "src/loader/guestExecution.cpp", "src/loader/runtimeLinker.cpp",
                            "src/emulator.cpp")})
    result = {
        "parent_build": str(parent), "repository": str(repository),
        "archive_directory": str(archive_root),
        "configuration": "Release",
        "native_architecture": "arm64", "host_page_size": 16384,
        "fex_base": str(source), "fex_commit": PIN, "fex_overlay": str(overlay),
        "reviewed_patch_hashes": reviewed_hashes, "archives": archives,
        "full_game_runtime_ready": False, "commercial_game_qualified": False,
        "compatibility_source_root": str(compatibility),
        "source_files": source_files,
    }
    if args.binary:
        binary = args.binary.resolve(strict=True)
        if capture("/usr/bin/lipo", "-archs", str(binary)) != "arm64":
            raise ValueError("full executable is not thin native arm64")
        symbols = capture("/usr/bin/nm", "-g", str(binary)).splitlines()
        required = ("_LumenAndroidFexCoreRunGuestV3", "_LumenAndroidFexCoreRootReturnAddressV1")
        defined = {line.split()[-1] for line in symbols if re.search(r"\s[Tt]\s", line)}
        for symbol in required:
            if symbol not in defined:
                raise ValueError("real FEX embedding symbol missing from full executable: " + symbol)
        commands = capture("ninja", "-C", str(binary.parent), "-t", "commands", "kyty_emulator")
        link = next((line for line in reversed(commands.splitlines()) if " -o kyty_emulator " in line), None)
        if not link:
            raise ValueError("cannot inspect the actual full executable link command")
        for name in ("fmt", "xxhash"):
            other = [token for token in shlex.split(link) if token.endswith("/lib" + name + ".a") and token != archives[name]["path"]]
            if other:
                raise ValueError("duplicate unaudited dependency in full link command: " + ", ".join(other))
        result["executable"] = {"path": str(binary), "sha256": digest(binary),
                                "architecture": "arm64", "defined_fex_symbols": list(required),
                                "link_command": link}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if args.cmake_output:
        lines = []
        for name, item in archives.items():
            lines.append(f'set(LUMEN_PS5_IMPORTED_{name} [==[{item["path"]}]==])')
        lines.extend((f'set(LUMEN_PS5_IMPORTED_FMT_INCLUDE [==[{overlay / "External/fmt/include"}]==])',
                      f'set(LUMEN_PS5_IMPORTED_XXHASH_INCLUDE [==[{overlay / "External/xxhash"}]==])'))
        args.cmake_output.write_text("\n".join(lines) + "\n")
    print("Verified native FEX link closure: " + str(args.output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--parent", required=True, type=Path)
    parser.add_argument("--repository", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cmake-output", type=Path)
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    try:
        verify(args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print("FEX prebuilt verification rejected: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
