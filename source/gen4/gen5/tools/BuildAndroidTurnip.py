#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build pinned official Mesa Turnip for Android offline; install no host tools."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile

VERSION = "26.1.0"
SOURCE_URL = f"https://archive.mesa3d.org/mesa-{VERSION}.tar.xz"
SOURCE_SHA256 = "a5095e6dc2986c78f0cef4c5555dc803e93b6bfe5670e991f9e8bd49395bae19"
NDK_VERSION = "29.0.14206865"
API = 35
OPTIONS = [
    "-Dplatforms=android", "-Dplatform-sdk-version=35", "-Dandroid-stub=true",
    "-Dandroid-libbacktrace=disabled", "-Dgallium-drivers=", "-Dvulkan-drivers=freedreno",
    "-Dfreedreno-kmds=kgsl", "-Dopengl=false", "-Dgles1=disabled", "-Dgles2=disabled",
    "-Degl=disabled", "-Dglx=disabled", "-Dgbm=disabled", "-Dllvm=disabled",
    "-Dshared-glapi=disabled", "-Dlibunwind=disabled", "-Dbuild-tests=false",
    "-Dzstd=disabled", "-Dxmlconfig=disabled", "-Dexpat=disabled", "-Dspirv-tools=disabled",
    "-Dcpp_args=-Wno-error=c++11-narrowing",
]
NEEDED = ["libcutils.so", "libhardware.so", "liblog.so", "libnativewindow.so",
          "libsync.so", "libm.so", "libz.so", "libdl.so", "libc.so"]


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n")


def output(command, env=None):
    return subprocess.check_output([str(arg) for arg in command], env=env,
                                   text=True, stderr=subprocess.STDOUT).strip()


def executable(value, name):
    candidate = str(value) if value else shutil.which(name)
    if not candidate or not Path(candidate).is_file():
        raise RuntimeError(f"missing host tool {name}; install it separately")
    return Path(candidate).absolute()


def source_manifest(directory):
    return [{"path": str(path.relative_to(directory)), "size": path.stat().st_size,
             "sha256": digest(path)} for path in sorted(directory.rglob("*"))
            if path.is_file() and not path.is_symlink()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--meson", type=Path)
    parser.add_argument("--bison", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--verify-only", action="store_true",
                        help="check pinned source/tool prerequisites without building or writing")
    args = parser.parse_args()
    if args.jobs < 1 or digest(args.source_archive) != SOURCE_SHA256:
        parser.error("invalid jobs or source archive SHA-256")
    ndk = args.ndk.resolve()
    if not re.search(rf"^Pkg.Revision\s*=\s*{re.escape(NDK_VERSION)}\s*$",
                     (ndk / "source.properties").read_text(), re.M):
        parser.error(f"requires the qualified NDK {NDK_VERSION}")
    candidates = [ndk / f"toolchains/llvm/prebuilt/{host}/bin"
                  for host in ("darwin-x86_64", "linux-x86_64")]
    toolchain = next((path for path in candidates if path.is_dir()), None)
    if toolchain is None or not (toolchain / f"aarch64-linux-android{API}-clang").is_file():
        parser.error("Android NDK AArch64 API35 toolchain is missing")
    meson = executable(args.meson, "meson")
    bison = executable(args.bison, "bison")
    pkgconfig = executable(None, "pkg-config")
    ninja = executable(None, "ninja")
    flex = executable(None, "flex")
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join([str(bison.parent), str(meson.parent), env.get("PATH", "")])
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    meson_python = executable(None, "python3")
    if (meson.parent / "python3").is_file():
        meson_python = meson.parent / "python3"
    output([meson_python, "-c", "import mako, yaml, ply, packaging"], env)
    bison_version = output([bison, "--version"]).splitlines()[0]
    match = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", bison_version)
    if not match or tuple(int(part or 0) for part in match.groups()) < (3, 0, 0):
        parser.error("Bison >=3 is required; macOS /usr/bin/bison 2.3 is unsupported")
    tools = {
        "meson": output([meson, "--version"], env), "python": output([meson_python, "--version"]),
        "python_packages": output([meson_python, "-m", "pip", "freeze"]),
        "bison": bison_version, "bison_sha256": digest(bison),
        "ninja": output([ninja, "--version"]), "flex": output([flex, "--version"]),
        "pkgconfig": output([pkgconfig, "--version"]),
        "clang": output([toolchain / "clang", "--version"]).splitlines()[0],
    }
    if args.verify_only:
        print(json.dumps({"status": "SOURCE_AND_HOST_PREREQUISITES_PASS", "source_sha256": SOURCE_SHA256,
                          "tools": tools, "build": "NOT_RUN"}, indent=2))
        return
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    if any((work / name).exists() for name in ("source", "build", "install", "prebuilt")):
        parser.error("use a fresh work directory; prior source/build/install/prebuilt is preserved")
    source_root = work / "source"
    source_root.mkdir()
    with tarfile.open(args.source_archive) as archive:
        archive_members = [{"name": item.name, "size": item.size, "type": item.type.decode("ascii"),
                            "linkname": item.linkname} for item in archive.getmembers()]
        archive.extractall(source_root, filter="data")
    source = source_root / f"mesa-{VERSION}"
    original_files = source_manifest(source)
    original_links = {str(path.relative_to(source)): str(path.readlink())
                      for path in source.rglob("*") if path.is_symlink()}
    empty_pkgconfig = work / "target-pkgconfig-empty"
    empty_pkgconfig.mkdir()
    env["PKG_CONFIG_PATH"] = ""
    env["PKG_CONFIG_LIBDIR"] = str(empty_pkgconfig)
    cross = work / "android-aarch64-api35.cross"
    alignment = ["-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384"]
    cross.write_text(
        f"[binaries]\nc = {[str(toolchain / 'aarch64-linux-android35-clang')]!r}\n"
        f"cpp = {[str(toolchain / 'aarch64-linux-android35-clang++'), '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-static-libstdc++']!r}\n"
        f"ar = {str(toolchain / 'llvm-ar')!r}\nstrip = {str(toolchain / 'llvm-strip')!r}\n"
        f"c_ld = 'lld'\ncpp_ld = 'lld'\npkg-config = {str(pkgconfig)!r}\n"
        "\n[host_machine]\nsystem = 'android'\ncpu_family = 'aarch64'\ncpu = 'armv8'\nendian = 'little'\n"
        f"\n[properties]\nneeds_exe_wrapper = true\npkg_config_libdir = {[str(empty_pkgconfig)]!r}\n"
        f"\n[built-in options]\nc_link_args = {alignment!r}\ncpp_link_args = {alignment!r}\n")
    build, install, package = (work / name for name in ("build", "install", "prebuilt"))
    commands = [
        [meson, "setup", build, source, "--cross-file", cross, "--prefix", install,
         "--buildtype", "release", "--wrap-mode", "nofallback", *OPTIONS],
        [meson, "compile", "-C", build, f"-j{args.jobs}"],
        [meson, "install", "-C", build, "--no-rebuild"],
    ]
    for number, command in enumerate(commands):
        with (work / f"{number}-build.log").open("w") as log:
            subprocess.run([str(arg) for arg in command], env=env, cwd=work,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    if source_manifest(source) != original_files or original_links != {
            str(path.relative_to(source)): str(path.readlink())
            for path in source.rglob("*") if path.is_symlink()}:
        raise RuntimeError("upstream source tree changed during build")
    archives = []
    for archive in sorted(build.rglob("*.a")):
        thin = archive.read_bytes().startswith(b"!<thin>\n")
        members = []
        for name in output([toolchain / "llvm-ar", "t", archive]).splitlines():
            if thin:
                member_path = Path(name).resolve()
                if not member_path.is_relative_to(build):
                    raise RuntimeError(f"archive member outside target build: {name}")
                data = member_path.read_bytes()
                name = str(member_path.relative_to(work))
            else:
                data = subprocess.check_output([str(toolchain / "llvm-ar"), "p", str(archive), name])
            if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", data, 16) != (1, 183):
                raise RuntimeError(f"archive member is not ELF64LE REL AArch64: {archive}:{name}")
            members.append({"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                            "elf": "ELF64LE REL AArch64"})
        archives.append({"path": str(archive.relative_to(work)), "sha256": digest(archive), "members": members})
    for directory in ("driver", "configuration", "sources", "licenses"):
        (package / directory).mkdir(parents=True)
    driver = package / "driver/libvulkan_freedreno.so"
    shutil.copy2(install / "lib/libvulkan_freedreno.so", driver)
    driver_bytes = driver.read_bytes()
    if driver_bytes[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<HH", driver_bytes, 16) != (3, 183):
        raise RuntimeError("driver is not ELF64LE DYN AArch64")
    elf = output([toolchain / "llvm-readelf", "--file-header", "--program-headers", "--dynamic", "--wide", driver])
    exports = output([toolchain / "llvm-nm", "-D", "--defined-only", driver])
    loads = [line.split() for line in elf.splitlines() if line.split()[:1] == ["LOAD"]]
    needed = re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", elf)
    if not loads or any(int(item[-1], 0) != 16384 or "".join(item[6:-1]) == "RWE" for item in loads):
        raise RuntimeError("driver segment alignment or permissions failed")
    if needed != NEEDED or "RPATH" in elf or "RUNPATH" in elf or [line.split()[-1] for line in exports.splitlines()] != ["HMI"]:
        raise RuntimeError("unexpected driver dependencies, search path or HAL exports")
    target_links = [line for line in (build / "build.ninja").read_text().splitlines()
                    if line.startswith(" LINK_ARGS =")]
    if any("-L/opt/homebrew" in line or "/opt/homebrew/Cellar" in line for line in target_links):
        raise RuntimeError("host library linkage detected")
    configuration = package / "configuration"
    shutil.copy2(cross, configuration / cross.name)
    shutil.copy2(args.source_archive, package / f"sources/mesa-{VERSION}.tar.xz")
    shutil.copytree(source / "licenses", package / "licenses/mesa")
    shutil.copy2(source / "docs/license.rst", package / "licenses/mesa-license-and-copyright.rst")
    write_json(configuration / "source-archive-members.json", archive_members)
    write_json(configuration / "source-files-sha256.json", original_files)
    write_json(configuration / "source-symbolic-links.json", original_links)
    write_json(configuration / "static-archive-audit.json", archives)
    write_json(configuration / "host-tools.json", tools)
    (configuration / "driver-elf.txt").write_text(elf + "\n")
    (configuration / "driver-exports.txt").write_text(exports + "\n")
    (configuration / "target-link-arguments.txt").write_text("\n".join(target_links) + "\n")
    for kind in ("buildoptions", "dependencies"):
        (configuration / f"meson-{kind}.json").write_text(output([meson, "introspect", build, f"--{kind}"], env) + "\n")
    for number in range(3):
        shutil.copy2(work / f"{number}-build.log", configuration)
    receipt = {
        "schema": "lumen.mesa.turnip.android.offline-build.v1", "status": "BUILD_AND_STATIC_AUDIT_PASS",
        "source_url": SOURCE_URL, "source_sha256": SOURCE_SHA256, "source_patch_count": 0,
        "source_regular_files_verified": len(original_files), "source_archive_members": len(archive_members),
        "target": "Android arm64-v8a API35 KGSL", "ndk_version": NDK_VERSION,
        "commands": [[str(arg) for arg in command] for command in commands],
        "host_path_prefix": [str(bison.parent), str(meson.parent)],
        "cpp_args": ["-Wno-error=c++11-narrowing"],
        "compiler_note": "Original Mesa source unchanged; Clang21 aggregate narrowing diagnostics remain visible warnings.",
        "driver_sha256": digest(driver), "needed": needed, "exports": ["HMI"],
        "pt_load_alignment": 16384, "rwx_segments": False, "rpath": None, "runpath": None,
        "static_archive_count": len(archives), "aarch64_archive_members": sum(len(item["members"]) for item in archives),
        "target_pkgconfig": "empty directory", "android_link_stubs_packaged": False,
        "loader": "libadrenotools and Android system Vulkan loader; driver exports HAL HMI",
        "device_runtime": "NOT_RUN", "vulkan_capabilities": "NOT_QUALIFIED",
        "public_binary_distribution_qualified": False,
        "files": source_manifest(package),
    }
    write_json(package / "SOURCE.json", receipt)
    print(package)


if __name__ == "__main__":
    main()
