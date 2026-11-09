#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the pinned shared PS4/PS5 FFmpeg recipe for one iOS ARM64 slice."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

from BuildAndroidFfmpeg import CORE_COMMIT, LIBRARIES, SOURCE_SHA256, digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--sdk", choices=("iphoneos", "iphonesimulator"), required=True)
    parser.add_argument("--deployment-target", default="17.4")
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or digest(args.source_archive) != SOURCE_SHA256:
        parser.error("invalid jobs or source archive SHA-256")
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", args.deployment_target):
        parser.error("invalid deployment target")
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source, build, install = (work / name for name in ("source", "build", "install"))
    if any(path.exists() for path in (source, build, install)):
        parser.error("use a fresh work directory; previous artifacts are preserved")
    source.mkdir()
    with tarfile.open(args.source_archive) as archive:
        archive.extractall(source, filter="data")
    recipe = source / "recipe/ffmpeg.patch"
    canonical = Path(__file__).resolve().parents[1] / "compatibility/3rdparty/ffmpeg-core/ffmpeg.patch"
    if recipe.read_bytes() != canonical.read_bytes():
        raise RuntimeError("source bundle recipe differs from pinned Kyty recipe")

    def xcrun(*arguments):
        return subprocess.check_output(["xcrun", "--sdk", args.sdk, *arguments], text=True).strip()

    sysroot = xcrun("--show-sdk-path")
    target = "arm64-apple-ios" + args.deployment_target
    if args.sdk == "iphonesimulator":
        target += "-simulator"
    flags = f"-target {target} -isysroot {sysroot} -O2 -fPIC"
    build.mkdir()
    configure = [str(source / "ffmpeg/configure"), "--target-os=darwin", "--arch=aarch64",
                 "--enable-cross-compile", "--enable-pic", "--enable-static", "--disable-shared",
                 "--disable-doc", "--disable-programs", "--disable-autodetect", "--disable-everything",
                 "--disable-avdevice", "--disable-postproc", "--enable-pthreads",
                 "--enable-runtime-cpudetect", f"--extra-cflags={flags}", f"--extra-ldflags={flags}",
                 f"--cc={xcrun('--find', 'clang')}", f"--cxx={xcrun('--find', 'clang++')}",
                 f"--ar={xcrun('--find', 'ar')}", f"--ranlib={xcrun('--find', 'ranlib')}",
                 f"--nm={xcrun('--find', 'nm')}", f"--strip={xcrun('--find', 'strip')}",
                 f"--sysroot={sysroot}", f"--prefix={install}"]
    configure.extend(re.findall(
        r"--enable-(?:decoder|encoder|muxer|demuxer|parser|protocol|bsf)=[a-z0-9_]+", recipe.read_text()))
    configure.extend(f"--enable-{name}" for name in LIBRARIES)
    commands = (configure, ["make", f"-j{args.jobs}"], ["make", "install"])
    for number, command in enumerate(commands):
        with (work / f"{number}-build.log").open("w") as log:
            subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    config = (build / "config.h").read_text()
    for option in ("GPL", "GPLV3", "NONFREE", "VERSION3"):
        if re.search(rf"^#define CONFIG_{option} 1$", config, re.M):
            raise RuntimeError(f"unexpected FFmpeg license option {option}")
    notices = install / "share/ffmpeg"
    notices.mkdir(parents=True, exist_ok=True)
    for name in ("config.h", "config_components.h"):
        shutil.copy2(build / name, notices / name)
    for name in ("COPYING.LGPLv2.1", "LICENSE.md"):
        shutil.copy2(source / "ffmpeg" / name, notices / name)
    shutil.copy2(source / "build-info/copyright", notices / "copyright")
    receipt = {"schema_version": 1, "core_commit": CORE_COMMIT, "source_sha256": SOURCE_SHA256,
               "source_url": "https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/9ac4cfd195f1/ffmpeg-source-macos-arm64.tar.gz",
               "sdk": args.sdk, "target": target, "commands": commands, "license": "LGPL-2.1-or-later",
               "archives": {name: digest(install / f"lib/lib{name}.a") for name in LIBRARIES},
               "public_binary_distribution_qualified": False}
    (notices / "SOURCE.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(install)


if __name__ == "__main__":
    main()
