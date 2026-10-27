#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the direct runtime's CLI admission without running guest code."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> None:
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and sys.argv[2] != "--native-metal"):
        raise SystemExit("usage: DirectCliTests.py <direct-runtime> [--native-metal]")
    native_metal = len(sys.argv) == 3
    binary = Path(sys.argv[1]).resolve(strict=True)
    environment = os.environ.copy()
    environment.pop("LUMEN_CPU_TRANSLATOR", None)
    with tempfile.TemporaryDirectory(prefix="lumen-gen5-cli-") as directory:
        work = Path(directory)
        root = work / "extracted root"
        root.mkdir()
        # This deliberately is not an executable. Accepted argument tests use
        # --help; rejected requests must exit before opening the guest loader.
        (root / "eboot.bin").write_bytes(b"not guest code")
        empty = work / "empty"
        empty.mkdir()
        cases = [
            ([], 0, "extracted-root", {}),
            (["--help"], 0, "Direct FEX is the only CPU provider", {}),
            (["--cpu-translator", "fex", "--game", str(root), "--help"],
             0, "extracted-root", {}),
            (["--game", "extracted root", "--screen-width", "1280", "--screen-height", "720",
              "--present-mode", "Fifo", "--upstream-import-fallback", "--help"],
             0, "extracted-root", {}),
            (["--game"], 1, "missing value", {}),
            (["--game", str(empty)], 1, "containing eboot.bin", {}),
            (["--game", str(root / "eboot.bin")], 1, "containing eboot.bin", {}),
            (["--game", str(work / "missing")], 1, "containing eboot.bin", {}),
            (["--game", str(root), "--game", str(root)], 1, "only be specified once", {}),
            (["--cpu-translator", "fex", "--cpu-translator", "fex", "--help"],
             1, "only be specified once", {}),
            (["--cpu-translator", "qemu-tcg", "--game", str(root)],
             1, "CPU translator selection rejected", {}),
            (["--game", str(root)], 1, "selected=qemu-tcg admitted=0",
             {"LUMEN_CPU_TRANSLATOR": "qemu-tcg"}),
            (["--game", str(root)], 1, "selected=invalid admitted=0",
             {"LUMEN_CPU_TRANSLATOR": "unknown"}),
            (["--screen-width", "0", "--help"], 1, "invalid screen width", {}),
            (["--console-language", "30", "--help"], 1, "invalid console language", {}),
            (["--not-an-option", "value"], 1, "unknown option", {}),
        ]
        if native_metal:
            cases += [
                (["--help"], 0, "Graphics backend: native Metal", {}),
                (["--rd"], 1, "unavailable with the native Metal backend", {}),
                (["--vulkan-validation", "true"], 1, "unavailable with the native Metal backend", {}),
                (["--gpu-assisted-validation", "true"], 1, "unavailable with the native Metal backend", {}),
            ]
        for arguments, status, marker, overrides in cases:
            result = subprocess.run([str(binary), *arguments], cwd=work,
                                    env=environment | overrides, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=15, check=False)
            if result.returncode != status or marker not in result.stdout:
                raise AssertionError(f"{arguments}: exit={result.returncode}, expected={status}, "
                                     f"marker={marker!r}\n{result.stdout}")
            if "LUMEN_PS5_DIRECT_FEX" in result.stdout:
                raise AssertionError(f"CLI admission initialized the CPU: {arguments}\n{result.stdout}")
        if set(work.iterdir()) != {root, empty} or (root / "eboot.bin").read_bytes() != b"not guest code":
            raise AssertionError("CLI admission modified guest input or created runtime data")
        print(f"DIRECT_CLI_TEST PASS {len(cases)} admission/help cases; no guest execution")


if __name__ == "__main__":
    main()
