#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the pinned Kyty FFmpeg source bundle for Android without downloads."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

SOURCE_SHA256 = "3222a17b900456acca57d46cf495867a353a37ae8ee2a5d3c8317f8c266d3699"
CORE_COMMIT = "9ac4cfd195f192ed8b08566f49c28dbb48d08341"
LIBRARIES = ("avformat", "avcodec", "swscale", "avutil", "avfilter", "swresample")


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or digest(args.source_archive) != SOURCE_SHA256:
        parser.error("invalid jobs or source archive SHA-256")
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = work / "source"
    build = work / "build"
    install = work / "install"
    if source.exists() or build.exists() or install.exists():
        parser.error("use a fresh work directory; prior source/build/install is preserved")
    source.mkdir()
    with tarfile.open(args.source_archive) as archive:
        archive.extractall(source, filter="data")
    recipe = source / "recipe" / "ffmpeg.patch"
    canonical = Path(__file__).resolve().parents[1] / "compatibility/3rdparty/ffmpeg-core/ffmpeg.patch"
    if recipe.read_bytes() != canonical.read_bytes():
        raise RuntimeError("source bundle recipe differs from pinned Kyty recipe")
    toolchain = args.ndk.resolve() / "toolchains/llvm/prebuilt/darwin-x86_64/bin"
    if not toolchain.is_dir():
        toolchain = args.ndk.resolve() / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    compiler = toolchain / "aarch64-linux-android26-clang"
    if not compiler.is_file():
        parser.error("Android NDK AArch64 API26 toolchain is missing")
    build.mkdir()
    configure = [str(source / "ffmpeg/configure"), "--target-os=android",
                 "--arch=aarch64", "--enable-cross-compile", "--enable-pic",
                 "--enable-static", "--disable-shared", "--disable-doc",
                 "--disable-programs", "--disable-autodetect", "--disable-everything",
                 "--disable-avdevice", "--disable-postproc", "--enable-pthreads",
                 "--enable-runtime-cpudetect", "--extra-cflags=-O2 -fPIC",
                 "--extra-ldflags=-Wl,-z,max-page-size=16384",
                 f"--cc={compiler}", f"--cxx={toolchain / 'aarch64-linux-android26-clang++'}",
                 f"--ar={toolchain / 'llvm-ar'}", f"--ranlib={toolchain / 'llvm-ranlib'}",
                 f"--nm={toolchain / 'llvm-nm'}", f"--strip={toolchain / 'llvm-strip'}",
                 f"--prefix={install}"]
    # The upstream core's shared codec/format recipe applies to every ROM.
    # Its dshow input device is Windows-only and is excluded on Android.
    for flag in re.findall(r"--enable-(?:decoder|encoder|muxer|demuxer|parser|protocol|bsf)=[a-z0-9_]+",
                           recipe.read_text()):
        configure.append(flag)
    for library in LIBRARIES:
        configure.append(f"--enable-{library}")
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
    shutil.copy2(build / "config.h", notices / "config.h")
    shutil.copy2(build / "config_components.h", notices / "config_components.h")
    for name in ("COPYING.LGPLv2.1", "LICENSE.md"):
        shutil.copy2(source / "ffmpeg" / name, notices / name)
    shutil.copy2(source / "build-info/copyright", notices / "copyright")
    receipt = {"schema_version": 1, "core_commit": CORE_COMMIT,
               "source_url": "https://github.com/KytyPS5/ext-ffmpeg-core/releases/download/9ac4cfd195f1/ffmpeg-source-macos-arm64.tar.gz",
               "source_sha256": SOURCE_SHA256, "target": "Android arm64-v8a API26",
               "commands": commands, "license": "LGPL-2.1-or-later",
               "license_options": {name: False for name in ("GPL", "GPLV3", "NONFREE", "VERSION3")},
               "configuration_sha256": digest(notices / "config.h"),
               "archives": {name: digest(install / f"lib/lib{name}.a") for name in LIBRARIES},
               "public_binary_distribution_qualified": False}
    (notices / "SOURCE.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(install)


if __name__ == "__main__":
    main()
