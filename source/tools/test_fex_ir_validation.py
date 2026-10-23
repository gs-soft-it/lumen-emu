#!/usr/bin/env python3
"""Compare real FEX IR validators using an existing macOS ARM64 build closure.

Only the validator and original synthetic fixture are compiled. The existing
build graph, pinned FEX checkout and its verified overlays are never changed.
The optional benchmark measures this pass only; it is not an FPS measurement.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import statistics
import subprocess


IR_PATH = Path("FEXCore/Source/Interface/IR/Passes/IRValidation.cpp")
PATCH_NAME = "fex-c9c5a132-ir-validation-warning-format.patch"
PATCH_SHA = "6d33f17ac268bc00cf330cbb03a15d3f8ca2b8b7cc7167ed3fde8318732512dd"
LICENSE_SHA = "f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee"
MAC_PIN = "c9c5a132c18175656936b8c25c1d453fe6e262a0"
ANDROID_PIN = "e17fdf9e90a49614e16ac8414a687477fb9e4f7c"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, cwd=None):
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=True)


def provenance(source, pin):
    def git(*args):
        return run(["git", "-C", str(source), *args]).stdout.strip()
    assert git("rev-parse", "HEAD") == pin, "unexpected FEX pin"
    assert git("remote", "get-url", "origin") == "https://github.com/FEX-Emu/FEX.git", "unexpected FEX origin"
    assert not git("status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none"), "FEX checkout is modified"
    for name in ("LICENSE", "FEXCore/LICENSE"):
        assert digest(source / name) == LICENSE_SHA, "FEX MIT license mismatch"
    return {"source": str(source), "pin": pin, "ir_validation_sha256": digest(source / IR_PATH)}


def compilation_flags(entry):
    command = entry.get("arguments") or shlex.split(entry["command"])
    flags = []
    index = 1
    while index < len(command):
        token = command[index]
        if token in ("-o", "-c", "-MT", "-MF", "-MQ"):
            index += 2
            continue
        if token in ("-MD", "-MMD"):
            index += 1
            continue
        flags.append(token)
        index += 1
    return command[0], flags


def normalized(text):
    # The patch moves source lines, but hard assertions keep their messages.
    return re.sub(r"IRValidation\.cpp:\d+,", "IRValidation.cpp:LINE,", text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fex-source", type=Path, required=True)
    parser.add_argument("--android-source", type=Path)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument("--nodes", type=int, default=1024)
    args = parser.parse_args()
    assert 3 <= args.repetitions <= 100
    assert 1 <= args.iterations <= 1000000 and 1 <= args.nodes <= 8192
    repository = Path(__file__).resolve().parents[2]
    source = args.fex_source.resolve()
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    patch = repository / "macos/fexcore-runtime/patches" / PATCH_NAME
    fixture = repository / "macos/fexcore-runtime/FexIRValidationWarningTest.cpp"
    assert digest(patch) == PATCH_SHA, "reviewed patch hash mismatch"
    sources = [provenance(source, MAC_PIN)]
    if args.android_source:
        android = args.android_source.resolve()
        sources.append(provenance(android, ANDROID_PIN))
        assert (android / IR_PATH).read_bytes() == (source / IR_PATH).read_bytes(), "pins no longer share this pass"

    commands_path = build / "compile_commands.json"
    entries = json.loads(commands_path.read_text())
    entry = next(e for e in entries if Path(e["file"]).as_posix().endswith(str(IR_PATH)))
    compiler, flags = compilation_flags(entry)
    # Static libraries are snapshotted so concurrent integrated builds cannot
    # change the isolated differential's link inputs after this point.
    library_paths = [
        "fex-core/Source/libFEXCore.a", "fex-core/Source/libFEXCore_Base.a",
        "fex-fmt/libfmt.a", "fex-xxhash/libxxhash.a", "fex-cephes/libcephes_128bit.a",
        "fex-softfloat/libsoftfloat_3e.a", "fex-core/Source/libJemallocLibs.a",
        "fex-rpmalloc/librpmalloc.a",
    ]
    libraries = []
    library_inventory = {}
    closure = build / "macos-fexcore-runtime"
    for relative in library_paths:
        path = closure / relative
        snapshot = output / "libraries" / relative
        snapshot.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, snapshot)
        libraries.append(str(snapshot))
        library_inventory[relative] = digest(snapshot)
    binaries = {}
    compile_log = []
    fixture_object = output / "fixture.o"
    command = [compiler, *flags, "-c", str(fixture), "-o", str(fixture_object)]
    result = run(command, entry["directory"])
    compile_log.append({"command": command, "stdout": result.stdout, "stderr": result.stderr})
    for name in ("baseline", "candidate"):
        root = output / name
        translation_unit = root / IR_PATH
        translation_unit.parent.mkdir(parents=True, exist_ok=True)
        translation_unit.write_bytes((source / IR_PATH).read_bytes())
        if name == "candidate":
            run(["git", "apply", "--check", str(patch)], root)
            run(["git", "apply", str(patch)], root)
        object_path = root / "validator.o"
        command = [compiler, *flags, "-c", str(translation_unit), "-o", str(object_path)]
        result = run(command, entry["directory"])
        compile_log.append({"command": command, "stdout": result.stdout, "stderr": result.stderr})
        binary = root / "ir-validation-test"
        command = [compiler, "-arch", "arm64", "-mmacosx-version-min=12.0",
                   str(fixture_object), str(object_path), *libraries, "-ldl", "-o", str(binary)]
        result = run(command, entry["directory"])
        compile_log.append({"command": command, "stdout": result.stdout, "stderr": result.stderr})
        binaries[name] = binary
    (output / "compile.json").write_text(json.dumps(compile_log, indent=2) + "\n")

    cases = {
        "clean": (0, "PASS:clean"),
        "unused-pre": (0, "PASS:unused-pre"),
        "mismatch-post": (0, "PASS:mismatch-post"),
        # Dumping an invalid-sized op is itself a hard assertion at these pins.
        "unsized": (86, "Invalid Size"),
        "bad-uses": (86, "Uses, but reports"),
        "cross-block-argument": (86, "references invalid"),
        "invalid-class": (86, "Had destination but with no register class assigned"),
        "invalid-register": (86, "Had destination but with no register assigned"),
        "missing-end": (86, "Failed to end block with EndBlock"),
        "missing-exit": (86, "Didn't have a block exit IR op"),
        "invalid-header": (86, "Expected Node to be 'IRHeader'"),
    }
    results = []
    for case, (expected_exit, expected_message) in cases.items():
        pair = {}
        for name, binary in binaries.items():
            result = subprocess.run([str(binary), case], text=True, capture_output=True)
            assert result.returncode == expected_exit, (case, name, result.returncode, result.stdout, result.stderr)
            assert expected_message in result.stdout, (case, name, result.stdout)
            assert "Warnings:" not in result.stdout
            pair[name] = {"exit": result.returncode, "stdout": normalized(result.stdout), "stderr": result.stderr}
        assert pair["baseline"] == pair["candidate"], (case, pair)
        results.append({"case": case, "result": "PASS", **pair})
    receipt = {
        "result": "PASS", "scope": "real IR pass with original unexecuted synthetic graphs",
        "sources": sources, "patch_sha256": digest(patch), "fixture_sha256": digest(fixture),
        "runner_sha256": digest(Path(__file__)), "compile_commands_sha256": digest(commands_path),
        "libraries_sha256": library_inventory, "cases": results,
    }
    if args.benchmark:
        measurements = []
        for mode in ("clean", "unused-pre", "mismatch-post"):
            samples = {"baseline": [], "candidate": []}
            order = []
            for repetition in range(args.repetitions):
                pair_order = ["baseline", "candidate"] if repetition % 2 == 0 else ["candidate", "baseline"]
                order.append(pair_order)
                for name in pair_order:
                    result = run([str(binaries[name]), mode, str(args.nodes), str(args.iterations)])
                    samples[name].append(float(result.stdout.strip()))
            baseline = statistics.median(samples["baseline"])
            candidate = statistics.median(samples["candidate"])
            measurements.append({"fixture": mode, "nodes": args.nodes, "iterations": args.iterations,
                                 "repetitions": args.repetitions, "warmup_iterations": 101,
                                 "metric": "nanoseconds per IRValidation::Run", "order": order,
                                 "samples": samples, "baseline_median": baseline,
                                 "candidate_median": candidate,
                                 "reduction_percent": (baseline - candidate) / baseline * 100})
        receipt["benchmark"] = {"scope": "local pass-only screening; no guest execution or FPS claim",
                                "measurements": measurements}
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"PASS: {len(results)} differential fixtures; receipt={output / 'receipt.json'}")
    for item in receipt.get("benchmark", {}).get("measurements", []):
        print(f"{item['fixture']}: {item['baseline_median']:.1f} -> {item['candidate_median']:.1f} ns/pass "
              f"({item['reduction_percent']:.2f}% reduction)")


if __name__ == "__main__":
    main()
