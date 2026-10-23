#!/usr/bin/env python3
# Lumen, 2026-10-02. SPDX-License-Identifier: GPL-2.0-only
"""Retain a bounded PS5 engine attempt and immutable input fingerprints."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import time


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def executable_paths(game):
    paths = [game / "eboot.bin"] + list(game.glob("*.prx"))
    module_root = game / "sce_module"
    if module_root.is_symlink():
        raise ValueError(f"linked executable module directory: {module_root}")
    if module_root.exists():
        if not module_root.is_dir():
            raise ValueError(f"executable module path is not a directory: {module_root}")
        def unreadable(error):
            raise error
        for directory, children, files in os.walk(module_root, followlinks=False, onerror=unreadable):
            parent = Path(directory)
            for name in children + files:
                path = parent / name
                if path.is_symlink():
                    raise ValueError(f"linked executable module path: {path}")
            paths.extend(parent / name for name in files if name.endswith(".prx"))
    for path in paths:
        if path.is_symlink() or not path.is_file() or path.resolve(strict=True) != path:
            raise ValueError(f"executable module is missing, irregular, or linked: {path}")
    return sorted(paths, key=lambda path: path.relative_to(game).as_posix().encode("utf-8"))


def executable_content_id(game, executables, fingerprints):
    manifest = hashlib.sha256(b"lumen-fex-executable-manifest-v1\n")
    for path in executables:
        manifest.update(path.relative_to(game).as_posix().encode("utf-8"))
        manifest.update(b"\0")
        manifest.update(fingerprints[str(path)].encode("ascii"))
        manifest.update(b"\n")
    return manifest.hexdigest()


def prepare_fex_cache_environment(environment, game, executables, fingerprints, cpu_translator):
    provider = cpu_translator or environment.get("LUMEN_CPU_TRANSLATOR", "fex")
    if provider == "qemu-tcg":
        return
    selector = environment.setdefault("LUMEN_FEX_TRANSLATION_CACHE", "1")
    if selector not in ("0", "1"):
        raise ValueError("FEX translation cache selector must be 0 or 1")
    if selector == "0":
        return
    if "LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID" not in environment:
        environment["LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID"] = executable_content_id(game, executables, fingerprints)
    identity = environment["LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID"]
    if len(identity) != 64 or any(character not in "0123456789abcdefABCDEF" for character in identity):
        raise ValueError("FEX translation cache content identity must be a SHA256")
    if "LUMEN_FEX_TRANSLATION_CACHE_DIR" not in environment:
        if "XDG_CACHE_HOME" in environment:
            base = Path(environment["XDG_CACHE_HOME"])
        else:
            base = Path.home() / ("Library/Caches" if sys.platform == "darwin" else ".cache")
        if not base.is_absolute():
            raise ValueError("FEX translation cache root must be absolute")
        directory = base / "LumenEmu" / "FexTranslations"
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        if directory.is_symlink():
            raise ValueError("FEX translation cache directory must not be linked")
        environment["LUMEN_FEX_TRANSLATION_CACHE_DIR"] = str(directory.resolve(strict=True))
    directory = Path(environment["LUMEN_FEX_TRANSLATION_CACHE_DIR"])
    if not directory.is_absolute() or ".." in directory.parts:
        raise ValueError("FEX translation cache directory must be absolute without parent traversal")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("game", type=Path)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--cpu-translator", choices=("fex", "qemu-tcg"),
                        help="Select the common CPU provider for this attempt")
    parser.add_argument("--cpu-library", type=Path,
                        help="Fingerprint the embedded provider library; does not alter its loading")
    parser.add_argument("--save-template", type=Path,
                        help="Clone a save directory into the fresh working directory before launch")
    diagnostics = parser.add_mutually_exclusive_group()
    diagnostics.add_argument("--graphics-diagnostics", action="store_true",
                             help="Retain shader, PM4 and graphics diagnostics; no performance qualification")
    diagnostics.add_argument("--shader-diagnostics", action="store_true",
                             help="Retain shader artifacts with HLE/PM4 logs silent; no performance qualification")
    parser.add_argument("--upstream-import-fallback", action="store_true",
                        help="Enable the explicit unresolved-function zero-return fallback")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 600:
        parser.error("seconds must be between 1 and 600")
    binary = args.binary.resolve(strict=True)
    binary_before = digest(binary)
    game = args.game.resolve(strict=True)
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    working = evidence / "working"
    working.mkdir()
    save_template = args.save_template.resolve(strict=True) if args.save_template else None
    save_before = {}
    if save_template:
        if not save_template.is_dir():
            parser.error("save-template must be a directory")
        save_before = {str(path): digest(path) for path in sorted(save_template.rglob("*"))
                       if path.is_file()}
        shutil.copytree(save_template, working / "_SaveData")
    try:
        executables = executable_paths(game)
        inputs = executables + [game / "sce_sys" / "param.json"]
        before = {str(path): digest(path) for path in inputs if path.is_file()}
    except (OSError, ValueError) as error:
        parser.error(f"could not fingerprint executable content: {error}")
    environment = {key: os.environ[key] for key in (
        "SDL_VULKAN_LIBRARY", "KYTY_BORDERLESS", "LUMEN_MACOS_FEXCORE_FAULT_CONTEXT",
        "LUMEN_ANDROID_FEXCORE_FAULT_CONTEXT", "MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS",
        "LUMEN_QEMU_TCG_FAULT_CONTEXT",
        "LUMEN_QEMU_TCG_IR_CACHE", "LUMEN_QEMU_TCG_IR_CACHE_DIR", "LUMEN_QEMU_TCG_PRETRANSLATE",
        "LUMEN_QEMU_TCG_IR_CACHE_TRACE",
        "LUMEN_QEMU_TCG_CHAINING", "LUMEN_QEMU_TCG_PARALLEL",
        "LUMEN_QEMU_TCG_JIT_EXEC_CACHE", "LUMEN_QEMU_TCG_DEFER_RECLAIM",
        "LUMEN_QEMU_TCG_SCHEDULER", "LUMEN_QEMU_TCG_SCHEDULER_TRACE",
        "LUMEN_QEMU_TCG_RAW_QUANTUM_CLOCK", "LUMEN_PS5_LOADING_TRACE",
        "LUMEN_QEMU_TCG_TLB_COMMIT_COALESCING",
        "LUMEN_FEX_TRANSLATION_CACHE", "LUMEN_FEX_TRANSLATION_CACHE_DIR",
        "LUMEN_FEX_TRANSLATION_CACHE_CONTENT_ID",
        "LUMEN_FEX_TRANSLATION_CACHE_TRACE", "LUMEN_FEX_BARRIER_SPLIT_NOTIFICATIONS",
        "LUMEN_FEX_MACOS_RCPC", "LUMEN_FEX_MACOS_DIRECT_UNALIGNED",
        "LUMEN_FEX_PERFORMANCE_TRACE", "LUMEN_FEX_PERFORMANCE_TIMELINE",
        "LUMEN_CRITICAL_PATH_TRACE", "LUMEN_CRITICAL_PATH_STDERR", "LUMEN_CRITICAL_PATH_ARM_FILE",
        "LUMEN_PS5_FRAME_PIPELINE_TRACE", "LUMEN_PS5_GPU_SLICE_TRACE",
        "LUMEN_PS5_GPU_SLICE_TRACE_DELAY_MS",
        "LUMEN_PS5_GPU_SLICE_TRACE_ARM_FILE", "LUMEN_PS5_IMAGE_OWNER_TRACE",
        "LUMEN_PS5_HLE_STACK_READ",
        "LUMEN_PS5_MUTEX_RESOLVE_PIN",
        "LUMEN_PS5_GPU_HOST_BACKING_IMPORT",
        "LUMEN_PS5_BUFFER_CONTIGUOUS_HASH",
        "LUMEN_PS5_BUFFER_PUBLICATION_WITNESS",
        "LUMEN_FEX_HLE_SHORT_ADMISSION", "LUMEN_FEX_MAX_INST",
        "LUMEN_EXPERIMENTAL_FEX_HLE_SYNC_MEMORY_SHORT",
        "LUMEN_FEX_BARRIER_TARGETED_PROGRESS",
        "LUMEN_PS5_GFX_ASYNC_PRIVATE_BACKING", "LUMEN_PS5_GFX_ASYNC_CPU_VISIBLE_EXPERIMENT",
        "LUMEN_PS5_GFX_ASYNC_CPU_CONCURRENT_EXPERIMENT",
        "LUMEN_PS5_IMAGE_PUBLICATION_BLOCK_SEED", "LUMEN_PS5_IMAGE_PUBLICATION_TRACE",
        "XDG_CACHE_HOME",
        "MVK_CONFIG_SHADER_DUMP_DIR", "MVK_CONFIG_LOG_LEVEL", "MVK_CONFIG_DEBUG")
                   if key in os.environ}
    try:
        prepare_fex_cache_environment(environment, game, executables, before, args.cpu_translator)
    except (OSError, ValueError) as error:
        parser.error(f"could not prepare FEX translation cache: {error}")
    host_dependencies = {}
    if args.cpu_library:
        cpu_library = args.cpu_library.resolve(strict=True)
        host_dependencies[str(cpu_library)] = digest(cpu_library)
    if "SDL_VULKAN_LIBRARY" in environment:
        library = Path(environment["SDL_VULKAN_LIBRARY"]).resolve(strict=True)
        host_dependencies[str(library)] = digest(library)
    command = [str(binary), "--game", str(game)]
    if args.cpu_translator:
        command += ["--cpu-translator", args.cpu_translator]
    if args.upstream_import_fallback:
        command.append("--upstream-import-fallback")
    if args.graphics_diagnostics:
        command += ["--graphics-debug-dump", "true", "--shader-log-direction", "File",
                    "--shader-log-folder", str(evidence / "shaders"),
                    "--command-buffer-dump", "true", "--command-buffer-dump-folder",
                    str(evidence / "pm4"), "--printf-direction", "File",
                    "--printf-output-file", str(evidence / "runtime.log")]
    elif args.shader_diagnostics:
        command += ["--graphics-debug-dump", "true", "--shader-log-direction", "File",
                    "--shader-log-folder", str(evidence / "shaders"),
                    "--command-buffer-dump", "false", "--printf-direction", "Silent"]
    started = time.monotonic()
    started_wall_ns = time.time_ns()
    timed_out = False
    with (evidence / "stdout.log").open("wb") as stdout, (evidence / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen(command, cwd=working, stdout=stdout, stderr=stderr,
                                   env=environment, start_new_session=True)
        (evidence / "launch.json").write_text(json.dumps({
            "pid": process.pid, "argv": command, "start_monotonic": started,
            "start_wall_ns": started_wall_ns, "environment_overrides": environment,
            "binary_sha256": binary_before,
        }, indent=2) + "\n")
        print(f"PS5 probe pid={process.pid} evidence={evidence}", flush=True)
        try:
            code = process.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                code = process.wait()
    after = {path: digest(Path(path)) for path in before}
    binary_after = digest(binary)
    dependencies_after = {path: digest(Path(path)) for path in host_dependencies}
    receipt = {
        "binary": str(binary), "binary_sha256": binary_before,
        "binary_sha256_after": binary_after, "binary_unchanged": binary_before == binary_after,
        "argv": command,
        "cpu_translator": args.cpu_translator or "default",
        "save_template": str(save_template) if save_template else None,
        "save_template_sha256": save_before,
        "save_template_unchanged": all(digest(Path(path)) == expected
                                       for path, expected in save_before.items()),
        "import_mode": "upstream_function_fallback" if args.upstream_import_fallback else "strict",
        "pid": process.pid, "exit_code": code, "timed_out": timed_out,
        "start_monotonic": started, "start_wall_ns": started_wall_ns,
        "environment_overrides": environment, "host_dependencies_sha256": host_dependencies,
        "host_dependencies_sha256_after": dependencies_after,
        "host_dependencies_unchanged": host_dependencies == dependencies_after,
        "elapsed_seconds": time.monotonic() - started,
        "input_sha256_before": before, "input_sha256_after": after,
        "inputs_unchanged": before == after,
        "stdout": str(evidence / "stdout.log"), "stderr": str(evidence / "stderr.log"),
        "created_paths": sorted(str(path.relative_to(working)) for path in working.rglob("*")),
        "commercial_game_qualified": False,
        "visual_menu_input_audio_gameplay_fps": "NOT_QUALIFIED_BY_THIS_PROBE",
    }
    (evidence / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"PS5 probe exit={code} timeout={timed_out} inputs_unchanged={before == after}", flush=True)
    return 0 if (code == 0 and before == after and binary_before == binary_after
                 and host_dependencies == dependencies_after and not timed_out) else 1


if __name__ == "__main__":
    raise SystemExit(main())
