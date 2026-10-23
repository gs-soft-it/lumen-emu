#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the pinned native ARM64 CPU library with a macOS 12 deployment target.

All dependency libraries are built as static PIC archives under --work. No
Homebrew runtime library is linked and no software is installed on the host.
Source downloads require --prepare --download; ordinary builds are offline.
--smoke executes the shared library's CPU/VM fixture on the current host and
does not qualify execution on an older macOS release.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys

from BuildAndroid import (check_archive, digest, meson_tool, obtain,
                          prepare_tree, run, verify_jit_receipt_rejection,
                          verify_tb_receipt_rejection, verify_ir_receipt_rejection,
                          verify_ir_identity_receipt_rejection, write_json)

ROOT = Path(__file__).resolve().parents[1]
DEPLOYMENT_TARGET = "12.0"


def prepare(work, lock, qemu_lock, directories, download):
    entries = dict(lock["dependencies"])
    entries["qemu"] = qemu_lock
    archives = {}
    for name, entry in entries.items():
        candidate = next((directory / entry["archive"] for directory in directories
                          if (directory / entry["archive"]).is_file()), None)
        if candidate is None:
            candidate = obtain(work, entry, download)
        check_archive(candidate, entry)
        archives[name] = candidate
    manifests = {}
    for name, entry in entries.items():
        destination = work / "upstream" if name == "qemu" else work / "sources" / entry["prefix"]
        patches = {"glib": ["glib-core-only.patch"],
                   "qemu": ["qemu-virtual-tsc-clock.patch",
                            "qemu-code-ram-offset.patch",
                            "qemu-jit-execute-scope.patch",
                            "qemu-tb-exec-boundary.patch",
                            "qemu-persistent-ir-cache.patch"]}.get(name)
        manifests[name] = prepare_tree(work, archives[name], destination, entry,
                                       patches, lock)
    write_json(work / "source-trees.json", {
        "archives": entries, "sha256_by_source": manifests,
        "license_verdict": "NOT CERTIFIED",
    })


def native_environment(prefix):
    env = os.environ.copy()
    flags = (f"-O2 -fPIC -arch arm64 -mmacosx-version-min={DEPLOYMENT_TARGET} "
             f"-Werror=unguarded-availability-new -I{prefix}/include")
    env.update(MACOSX_DEPLOYMENT_TARGET=DEPLOYMENT_TARGET,
               PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"), PKG_CONFIG_PATH="",
               CC=shutil.which("clang"), CXX=shutil.which("clang++"),
               CFLAGS=flags, CXXFLAGS=flags, OBJCFLAGS=flags,
               LDFLAGS=f"-arch arm64 -mmacosx-version-min={DEPLOYMENT_TARGET} -L{prefix}/lib "
                       "-framework CoreFoundation -framework CoreServices")
    return env


def target_function_availability(work, build, env):
    # GLib's headerless has_function probe finds SDK symbols introduced after
    # the deployment target. Check the actual declaration and retain its
    # existing portable fallback when that declaration is unavailable.
    source = '#include <unistd.h>\nint main(void) { int fd[2]; return pipe2(fd, 0); }\n'
    probe = work / "pipe2-availability.c"
    probe.write_text(source)
    arguments = [env["CC"], *shlex.split(env["CFLAGS"]), "-Werror=implicit-function-declaration",
                 "-c", str(probe), "-o", str(work / "pipe2-availability.o")]
    result = subprocess.run(arguments, capture_output=True, text=True, env=env)
    configuration = build / "config.h"
    text = configuration.read_text()
    override = result.returncode != 0 and "#define HAVE_PIPE2 1" in text
    if result.returncode != 0 and "only available on macOS" not in result.stderr and "undeclared function 'pipe2'" not in result.stderr:
        raise RuntimeError(f"Unexpected pipe2 availability probe failure: {result.stderr}")
    if override:
        configuration.write_text(text.replace("#define HAVE_PIPE2 1", "/* #undef HAVE_PIPE2 */"))
    write_json(work / "target-function-availability.json", {
        "deployment_target": DEPLOYMENT_TARGET, "probe_arguments": arguments,
        "probe_source": source, "probe_exit_code": result.returncode,
        "probe_stderr": result.stderr, "glib_have_pipe2_disabled": override,
        "configuration_sha256": digest(configuration),
    })


def verify_prepared_sources(work, lock, qemu_lock):
    receipt = json.loads((work / "source-trees.json").read_text())
    expected_archives = dict(lock["dependencies"])
    expected_archives["qemu"] = qemu_lock
    if receipt["archives"] != expected_archives:
        raise ValueError("Prepared sources do not match the current locks; rerun --prepare")
    for name, manifest in receipt["sha256_by_source"].items():
        entry = expected_archives[name]
        source = work / "upstream" if name == "qemu" else work / "sources" / entry["prefix"]
        for relative, expected in manifest.items():
            path = source / relative
            if path.is_symlink() or not path.is_file() or digest(path) != expected:
                raise ValueError(f"Prepared source changed; preserving it: {path}")


def build_dependencies(work, python, lock, jobs):
    prefix = work / "prefix"
    for directory in ("lib/pkgconfig", "include"):
        (prefix / directory).mkdir(parents=True, exist_ok=True)
    # Darwin's SDK provides zlib and libiconv. Keep package discovery confined
    # to this prefix rather than inheriting a newer Homebrew deployment floor.
    (prefix / "lib/pkgconfig/zlib.pc").write_text(
        "Name: zlib\nDescription: macOS SDK system zlib\nVersion: 1.2.12\n"
        "Libs: -lz\nCflags:\n")
    env = native_environment(prefix)
    pcre = work / "build-pcre2"
    pcre.mkdir(exist_ok=True)
    source = work / "sources" / lock["dependencies"]["pcre2"]["prefix"]
    run([source / "configure", f"--prefix={prefix}", "--enable-pcre2-8",
         "--disable-pcre2-16", "--disable-pcre2-32", "--disable-jit",
         "--disable-shared", "--enable-static", "--disable-pcre2grep",
         "--disable-pcre2test", "--disable-pcre2-posix"],
        pcre, work / "configure-pcre2.log", env)
    run(["make", f"-j{jobs}", "libpcre2-8.la"], pcre, work / "build-pcre2.log", env)
    for original, relative in ((pcre / ".libs/libpcre2-8.a", "lib/libpcre2-8.a"),
                               (pcre / "src/pcre2.h", "include/pcre2.h"),
                               (pcre / "libpcre2-8.pc", "lib/pkgconfig/libpcre2-8.pc")):
        shutil.copy2(original, prefix / relative)
    gettext_build = work / "build-gettext"
    gettext_build.mkdir(parents=True, exist_ok=True)
    intl = gettext_build / "intl"
    source = work / "sources" / lock["dependencies"]["gettext"]["prefix"] / "gettext-runtime"
    run([source / "configure", f"--prefix={prefix}", "--disable-shared",
         "--enable-static", "--without-libiconv-prefix", "--disable-dependency-tracking",
         "--disable-java", "--disable-csharp", "--disable-libasprintf"],
        gettext_build, work / "configure-intl.log", env)
    run(["make", f"-j{jobs}"], intl, work / "build-intl.log", env)
    shutil.copy2(intl / ".libs/libintl.a", prefix / "lib/libintl.a")
    shutil.copy2(intl / "libintl.h", prefix / "include/libintl.h")
    (prefix / "lib/pkgconfig/intl.pc").write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n"
        "Name: GNU libintl\nDescription: GNU gettext standalone intl runtime\nVersion: 1.0\n"
        "Libs: -L${libdir} -lintl\nLibs.private: -liconv -framework CoreFoundation -framework CoreServices\n"
        "Cflags: -I${includedir}\n")
    meson, meson_env = meson_tool(work, python)
    meson_env.update(env)
    build = work / "build-glib"
    source = work / "sources" / lock["dependencies"]["glib"]["prefix"]
    options = ["--prefix", prefix, "--libdir", "lib", "--wrap-mode=nodownload",
               "--buildtype=release", "-Ddefault_library=static", "-Dtests=false",
               "-Dinstalled_tests=false", "-Ddocumentation=false", "-Dman-pages=disabled",
               "-Dintrospection=disabled", "-Dselinux=disabled", "-Dlibmount=disabled",
               "-Dsysprof=disabled", "-Ddtrace=disabled", "-Dsystemtap=disabled", "-Dnls=disabled"]
    reconfigure = ["--reconfigure"] if (build / "meson-private/coredata.dat").exists() else []
    run([*meson, "setup", *reconfigure, build, source, *options],
        work, work / "configure-glib.log", meson_env)
    target_function_availability(work, build, env)
    run(["ninja", "-C", build, f"-j{jobs}", "glib/libglib-2.0.a", "gmodule/libgmodule-2.0.a"],
        work, work / "build-glib.log", meson_env)
    run([*meson, "install", "-C", build, "--no-rebuild", "--tags", "runtime,devel"],
        work, work / "install-glib.log", meson_env)


def configure_qemu(work, python):
    prefix, build = work / "prefix", work / "build-qemu"
    build.mkdir(exist_ok=True)
    pkg = shutil.which("pkg-config")
    wrapper = work / "pkg-config-macos-static"
    wrapper.write_text(f"#!/bin/sh\nexec {shlex.quote(pkg)} --static \"$@\"\n")
    wrapper.chmod(0o755)
    env = native_environment(prefix)
    env["PKG_CONFIG"] = str(wrapper)
    argv = [work / "upstream/configure", f"--python={python}", "--target-list=x86_64-softmmu",
            "--without-default-features", "--without-default-devices", "--enable-system", "--enable-tcg",
            "--disable-user", "--disable-docs", "--disable-tools", "--disable-guest-agent", "--disable-plugins",
            "--disable-rust", "--disable-werror", "--disable-download", "--disable-install-blobs",
            "--disable-containers", f"--cc={shutil.which('clang')}", f"--cxx={shutil.which('clang++')}",
            f"--extra-cflags=-fPIC -arch arm64 -mmacosx-version-min={DEPLOYMENT_TARGET} "
            f"-Werror=unguarded-availability-new -I{prefix}/include",
            f"--extra-ldflags=-arch arm64 -mmacosx-version-min={DEPLOYMENT_TARGET} -L{prefix}/lib"]
    write_json(work / "qemu-configure-request.json", {
        "cwd": str(build), "argv": list(map(str, argv)),
        "env": {key: env[key] for key in ("MACOSX_DEPLOYMENT_TARGET", "PKG_CONFIG",
                                          "PKG_CONFIG_LIBDIR", "PKG_CONFIG_PATH")},
    })
    run(argv, build, work / "configure-qemu.log", env)
    return env


def audit_library(work, output, lock, verify_code_ram=False):
    run([sys.executable, ROOT / "tools/VerifyLibrary.py", "--library", output, "--host", "macos",
         "--deployment-target", DEPLOYMENT_TARGET,
         *(["--allow-code-ram-verification"] if verify_code_ram else [])],
        work, work / "verify-library.log")
    jit_receipt_rejections = verify_jit_receipt_rejection(work, output, "macos", [
        "--deployment-target", DEPLOYMENT_TARGET,
        *(["--allow-code-ram-verification"] if verify_code_ram else [])])
    tb_receipt_rejections = verify_tb_receipt_rejection(work, output, "macos", [
        "--deployment-target", DEPLOYMENT_TARGET,
        *(["--allow-code-ram-verification"] if verify_code_ram else [])])
    ir_receipt_rejections = verify_ir_receipt_rejection(work, output, "macos", [
        "--deployment-target", DEPLOYMENT_TARGET,
        *(["--allow-code-ram-verification"] if verify_code_ram else [])])
    ir_identity_rejections = verify_ir_identity_receipt_rejection(work, output, "macos", [
        "--deployment-target", DEPLOYMENT_TARGET,
        *(["--allow-code-ram-verification"] if verify_code_ram else [])])
    load_commands = subprocess.check_output(["otool", "-l", str(output)], text=True)
    imports = subprocess.check_output(["otool", "-L", str(output)], text=True)
    (work / "library-load-commands.txt").write_text(load_commands)
    (work / "library-imports.txt").write_text(imports)
    match = re.search(r"\bminos\s+(\S+)", load_commands)
    if not match or match[1] != DEPLOYMENT_TARGET:
        raise ValueError(f"Library does not declare macOS {DEPLOYMENT_TARGET}")
    needed = [line.strip().split(" (", 1)[0] for line in imports.splitlines()[2:]]
    if any(not path.startswith(("/usr/lib/", "/System/Library/Frameworks/")) for path in needed):
        raise ValueError(f"Unexpected non-system macOS library imports: {needed}")
    archives = {str(path): digest(path) for path in (work / "prefix/lib").glob("*.a")}
    archive_versions = {}
    for archive in archives:
        commands = subprocess.check_output(["otool", "-l", archive], text=True)
        versions = sorted(set(re.findall(r"\bminos\s+(\S+)", commands)))
        if not versions or any(tuple(map(int, version.split("."))) > (12, 0, 0)
                               for version in versions):
            raise ValueError(f"Static dependency exceeds macOS 12: {archive}: {versions}")
        archive_versions[archive] = versions
    linked = {}
    library_receipt = json.loads(Path(str(output) + ".json").read_text())
    for argument in library_receipt["link_arguments"]:
        if argument.endswith((".a", ".o")):
            path = Path(argument)
            if not path.is_absolute():
                path = work / "build-qemu" / path
            linked[str(path.resolve())] = digest(path)
    notices = output.parent / "licenses"
    selections = {"qemu": (work / "upstream", ["LICENSE", "COPYING", "COPYING.LIB"]),
                  "glib": (work / "sources/glib-2.88.3", ["COPYING", "LICENSES"]),
                  "pcre2": (work / "sources/pcre2-10.47", ["COPYING", "LICENCE.md"]),
                  "libintl": (work / "sources/gettext-1.0/gettext-runtime/intl", ["COPYING.LIB"])}
    for name, (source, paths) in selections.items():
        target = notices / name
        target.mkdir(parents=True, exist_ok=True)
        for relative in paths:
            original = source / relative
            if original.is_dir():
                shutil.copytree(original, target / relative, dirs_exist_ok=True)
            else:
                shutil.copy2(original, target / relative)
    write_json(notices / "manifest.json", {
        "library_sha256": digest(output),
        "files": {str(path.relative_to(notices)): digest(path)
                  for path in sorted(notices.rglob("*")) if path.is_file() and path.name != "manifest.json"},
        "scope": "Upstream notices; full selected per-file license closure remains unqualified",
        "license_verdict": "NOT CERTIFIED", "binary_distribution_qualified": False,
    })
    audit = {
        "qemu_version": library_receipt["qemu_version"],
        "library_sha256": digest(output), "bridge_sha256": library_receipt["bridge_sha256"],
        "header_sha256": library_receipt["header_sha256"],
        "cache_source_sha256": library_receipt["cache_source_sha256"],
        "cache_header_sha256": library_receipt["cache_header_sha256"],
        "ir_cache_build_id": library_receipt["ir_cache_build_id"],
        "persistent_ir_cache_abi_version": library_receipt["persistent_ir_cache_abi_version"],
        "source_lock_sha256": digest(ROOT / "source-lock.json"),
        "dependency_lock_sha256": digest(ROOT / "dependency-lock.json"),
        "recipe_sha256": digest(Path(__file__)),
        "verifier_sha256": digest(ROOT / "tools/VerifyLibrary.py"),
        "build_recipe_sha256": library_receipt["build_recipe_sha256"],
        "jit_execute_scope_receipt_negative_tests": jit_receipt_rejections,
        "tb_exec_boundary_receipt_negative_tests": tb_receipt_rejections,
        "persistent_ir_cache_receipt_negative_tests": ir_receipt_rejections,
        "persistent_ir_cache_identity_negative_tests": ir_identity_rejections,
        "deployment_target": DEPLOYMENT_TARGET, "architecture": "arm64",
        "build_host_macos": platform.mac_ver()[0], "dynamic_dependencies": needed,
        "static_dependency_archives": archives,
        "static_dependency_deployment_targets": archive_versions,
        "linked_object_archives_sha256": linked,
        "notice_manifest_sha256": digest(notices / "manifest.json"),
        "target_function_availability_sha256": digest(work / "target-function-availability.json"),
        "dependency_versions": {name: entry["version"] for name, entry in lock["dependencies"].items()},
        "native_fixture_tested": False, "macos_12_runtime_tested": False,
        "full_lumen_link_closure_certified": False, "binary_distribution_qualified": False,
    }
    write_json(work / "macos-library-audit.json", audit)
    write_json(Path(str(output) + ".macos.json"), audit)


def smoke(work, python, output, verify_code_ram=False):
    test = work / "lumen-qemu-tcg-test"
    env = native_environment(work / "prefix")
    run([shutil.which("clang"), "-std=gnu11", "-arch", "arm64",
         f"-mmacosx-version-min={DEPLOYMENT_TARGET}", "-I", ROOT,
         ROOT / "LumenQemuTcgTest.c", output, "-Wl,-rpath," + str(output.parent),
         *(["-DLUMEN_QEMU_TCG_CODE_RAM_VERIFY"] if verify_code_ram else []),
         "-o", test], work, work / "build-fixture.log", env)
    with (work / "fixture.log").open("w") as log:
        result = subprocess.run([str(test)], cwd=work, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=30)
    if result.returncode:
        raise RuntimeError(f"Shared-library CPU fixture failed ({result.returncode}); see {work / 'fixture.log'}")
    audit_path = work / "macos-library-audit.json"
    audit = json.loads(audit_path.read_text())
    audit["native_fixture_tested"] = True
    audit["fixture_sha256"] = digest(test)
    audit["fixture_source_sha256"] = digest(ROOT / "LumenQemuTcgTest.c")
    audit["fixture_stdout"] = (work / "fixture.log").read_text()
    write_json(audit_path, audit)
    write_json(Path(str(output) + ".macos.json"), audit)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True,
                        help="Existing host Python with setuptools/packaging; never modified")
    parser.add_argument("--archive-dir", action="append", type=Path, default=[])
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if sys.platform != "darwin" or platform.machine() != "arm64":
        raise ValueError("BuildMac requires native Apple Silicon; no Rosetta")
    if args.download and not args.prepare:
        raise ValueError("--download requires --prepare")
    if args.jobs < 1:
        raise ValueError("--jobs must be positive")
    for name in ("clang", "clang++", "ninja", "pkg-config", "make", "otool", "lipo"):
        if not shutil.which(name):
            raise ValueError(f"Missing host tool {name}; no installation is performed")
    work = args.work.expanduser().resolve()
    work.mkdir(parents=True, exist_ok=True)
    # Preserve virtualenv executable symlinks: resolving them would select the
    # parent interpreter and discard the supplied environment's build tooling.
    python = args.python.expanduser().absolute()
    if not python.is_file():
        raise ValueError("The supplied host Python does not exist")
    lock = json.loads((ROOT / "dependency-lock.json").read_text())
    qemu_lock = json.loads((ROOT / "source-lock.json").read_text())
    if args.prepare:
        prepare(work, lock, qemu_lock,
                [path.expanduser().resolve() for path in args.archive_dir], args.download)
    elif not (work / "source-trees.json").is_file():
        raise ValueError("Prepare the locked sources first with --prepare")
    verify_prepared_sources(work, lock, qemu_lock)
    build_dependencies(work, python, lock, args.jobs)
    env = configure_qemu(work, python)
    output = work / "lib/liblumen_qemu_tcg.dylib"
    run([python, ROOT / "tools/BuildLibrary.py", "--build", work / "build-qemu",
         "--output", output, "--jobs", args.jobs], work, work / "build-library.log", env)
    audit_library(work, output, lock)
    if args.smoke:
        smoke(work, python, output)
    print(json.dumps({"library": str(output), "deployment_target": DEPLOYMENT_TARGET,
                      "native_fixture_tested": args.smoke, "macos_12_runtime_tested": False}))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
