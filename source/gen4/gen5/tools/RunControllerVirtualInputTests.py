#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the isolated host controller fixture without configuring a runtime build."""

import argparse
import pathlib
import subprocess
import sys

from PrepareCompatibilitySources import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-dir", type=pathlib.Path, required=True,
                        help="Existing PS5 CMake directory containing cmake_config.h (read only)")
    parser.add_argument("--output-dir", type=pathlib.Path, required=True,
                        help="Separate scratch directory for the fixture executable")
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This isolated runner currently validates the macOS host fixture only")
    config = args.config_dir.resolve()
    output = args.output_dir.resolve()
    if not (config / "cmake_config.h").is_file():
        parser.error("--config-dir must contain cmake_config.h from an existing PS5 configuration")
    if output == config or config in output.parents:
        parser.error("--output-dir must be outside the existing CMake build directory")
    gen5_root = pathlib.Path(__file__).resolve().parents[1]
    dependencies = gen5_root / "compatibility/3rdparty"
    engine = prepare(gen5_root, output / "compatibility-sources")
    output.mkdir(parents=True, exist_ok=True)
    binary = output / "ControllerVirtualInputTests"
    command = [args.cxx, "-std=c++20", "-O2", "-UNDEBUG", "-ffunction-sections",
               "-fdata-sections", "-Wl,-dead_strip"]
    # Unused guest HLE and device-output methods are discarded. The production
    # input state, event history and lifecycle code are exercised by the fixture.
    for directory in (engine / "src", config, dependencies / "SDL3/include",
                      dependencies / "fmt/include"):
        command.extend(["-I", str(directory)])
    command.extend([str(engine / "tests/ControllerVirtualInputTests.cpp"), "-o", str(binary)])
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
