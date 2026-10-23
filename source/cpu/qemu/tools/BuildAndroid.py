#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare pinned sources, then build the API35 Android ARM64 TCG library.

Source downloads require the explicit --prepare --download invocation. Builds
are offline, use only the selected NDK and a supplied host Python environment,
and never install software outside --work. Existing source trees are preserved
and checked against the archives and exact checked patches before reuse.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def run(arguments, directory, log_path, env=None):
    with log_path.open("a") as log:
        log.write("\n" + shlex.join(map(str, arguments)) + "\n")
        log.flush()
        result = subprocess.run(list(map(str, arguments)), cwd=directory,
                                env=env, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}); see {log_path}")


def check_archive(path, entry):
    if not path.is_file() or path.stat().st_size != entry["bytes"] or digest(path) != entry["sha256"]:
        raise ValueError(f"Archive does not match the lock: {path}")


def obtain(work, entry, download):
    path = work / "downloads" / entry["archive"]
    if not path.exists() and not download:
        # The initial probe kept the QEMU archive beside its work directory.
        previous = work.parent / entry["archive"]
        if previous.is_file():
            check_archive(previous, entry)
            return previous
        raise ValueError(f"Missing {path}; prepare with --download or provide the locked archive")
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary_name = tempfile.mkstemp(prefix=".locked-download-", dir=path.parent)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(fd, "wb") as output:
                with urllib.request.urlopen(entry["url"], timeout=60) as response:
                    shutil.copyfileobj(response, output)
            check_archive(temporary, entry)
            temporary.rename(path)
        finally:
            temporary.unlink(missing_ok=True)
    check_archive(path, entry)
    return path


def tree_manifest(archive, destination, entry, patch_entry=None):
    """Verify all archive files, including the exact post-patch file hashes."""
    prefix = entry.get("prefix", f"qemu-{entry['version']}")
    excluded = set(entry.get("excluded_members", []))
    changed = (patch_entry or {}).get("files", {})
    manifest = {}
    archive_paths = set()
    with tarfile.open(archive) as source:
        for member in source:
            if member.name in excluded:
                continue
            parts = Path(member.name).parts
            if not parts or parts[0] != prefix or ".." in parts:
                raise ValueError(f"Unsafe source archive path: {member.name}")
            relative = Path(*parts[1:])
            archive_paths.add(str(relative))
            path = destination / relative
            if member.isfile() or member.islnk():
                if not path.is_file() or path.is_symlink():
                    raise ValueError(f"Missing or substituted source file: {path}")
                original = source.extractfile(member)
                original_digest = hashlib.sha256(original.read()).hexdigest()
                expected = changed.get(str(relative), {}).get("after_sha256", original_digest)
                if str(relative) in changed and original_digest != changed[str(relative)]["before_sha256"]:
                    raise ValueError(f"Patch input does not match the lock: {relative}")
                observed = digest(path)
                if observed != expected:
                    raise ValueError(f"Existing source changed; preserving it: {path}")
                manifest[str(relative)] = observed
            elif member.issym():
                if (not path.is_symlink()
                        or os.path.normpath(os.readlink(path)) != os.path.normpath(member.linkname)
                        or not path.resolve().is_relative_to(destination.resolve())):
                    raise ValueError(f"Source symlink changed; preserving it: {path}")
    for relative, hashes in changed.items():
        path = destination / relative
        if Path(relative).is_absolute() or ".." in Path(relative).parts or \
                not path.resolve().is_relative_to(destination.resolve()):
            raise ValueError(f"Unsafe checked patch path: {relative}")
        if hashes["before_sha256"] is None:
            if relative in archive_paths:
                raise ValueError(f"Added patch file already exists in the archive: {relative}")
            if path.is_symlink() or not path.is_file() or digest(path) != hashes["after_sha256"]:
                raise ValueError(f"Added patch file does not match the lock: {relative}")
            manifest[relative] = hashes["after_sha256"]
        elif relative not in manifest:
            raise ValueError(f"Checked patch input is absent from the archive: {relative}")
    return manifest


def prepare_tree(work, archive, destination, entry, patch_names, lock):
    patch_names = patch_names or []
    patch_files = {}
    for name in patch_names:
        if digest(ROOT / "patches" / name) != lock["patches"][name]["sha256"]:
            raise ValueError(f"Patch does not match dependency-lock.json: {name}")
        for path, checks in lock["patches"][name]["files"].items():
            if path in patch_files:
                if patch_files[path]["after_sha256"] != checks["before_sha256"]:
                    raise ValueError(f"Checked patches do not compose: {path}")
                patch_files[path]["after_sha256"] = checks["after_sha256"]
            else:
                patch_files[path] = dict(checks)
    patch_entry = {"files": patch_files}
    if destination.exists():
        return tree_manifest(archive, destination, entry, patch_entry)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".tcg-source-", dir=destination.parent) as directory:
        staging = Path(directory)
        prefix = entry.get("prefix", f"qemu-{entry['version']}")
        excluded = set(entry.get("excluded_members", []))
        skipped = set()
        with tarfile.open(archive) as source:
            for member in source:
                if member.name in excluded:
                    skipped.add(member.name)
                    continue
                parts = Path(member.name).parts
                if not parts or parts[0] != prefix or ".." in parts:
                    raise ValueError(f"Unsafe source archive path: {member.name}")
                source.extract(member, staging, filter="data")
        if skipped != excluded:
            raise ValueError("Excluded source archive members changed")
        extracted = staging / prefix
        for name in patch_names:
            patch = ROOT / "patches" / name
            if digest(patch) != lock["patches"][name]["sha256"]:
                raise ValueError(f"Patch does not match dependency-lock.json: {patch}")
            run(["patch", "-p1", "--batch", "--forward", "-i", patch], extracted,
                work / "prepare.log")
        manifest = tree_manifest(archive, extracted, entry, patch_entry)
        write_json(extracted / "lumen-source-provenance.json", {
            "version": entry["version"], "source_url": entry["url"],
            "archive_sha256": entry["sha256"], "signature_verified": False,
            "patches": {name: lock["patches"][name]["sha256"] for name in patch_names},
        })
        extracted.rename(destination)
        return manifest


def prepare(work, lock, qemu_lock, download):
    for name, entry in lock["patches"].items():
        if digest(ROOT / "patches" / name) != entry["sha256"]:
            raise ValueError(f"Patch does not match dependency-lock.json: {name}")
    entries = dict(lock["dependencies"])
    entries["qemu"] = qemu_lock
    # Check every input archive before publishing any extracted source tree.
    archives = {name: obtain(work, entry, download) for name, entry in entries.items()}
    manifest = {}
    for name, entry in entries.items():
        destination = work / "upstream" if name == "qemu" else work / "sources" / entry["prefix"]
        patch = {"glib": ["glib-core-only.patch"],
                 "qemu": ["qemu-android-bionic.patch", "qemu-virtual-tsc-clock.patch",
                          "qemu-code-ram-offset.patch",
                          "qemu-jit-execute-scope.patch",
                          "qemu-tb-exec-boundary.patch",
                          "qemu-persistent-ir-cache.patch"]}.get(name)
        manifest[name] = prepare_tree(work, archives[name], destination, entry, patch, lock)
    write_json(work / "source-trees.json", {"archives": entries, "sha256_by_source": manifest,
                                           "license_verdict": "NOT CERTIFIED"})


def find_ndk(requested, version):
    candidates = [requested, os.environ.get("ANDROID_NDK_ROOT"), os.environ.get("ANDROID_NDK_HOME")]
    for sdk in (os.environ.get("ANDROID_SDK_ROOT"), os.environ.get("ANDROID_HOME"),
                Path.home() / "Library/Android/sdk", Path("/opt/homebrew/share/android-commandlinetools")):
        if sdk:
            candidates.append(Path(sdk) / "ndk" / version)
    for candidate in candidates:
        if candidate:
            ndk = Path(candidate).expanduser().resolve()
            properties = ndk / "source.properties"
            if properties.is_file() and f"Pkg.Revision = {version}" in properties.read_text():
                return ndk
    raise ValueError(f"NDK {version} not found; pass --ndk (no installation is performed)")


def meson_tool(work, python):
    # Use the verified QEMU-bundled wheel as local build tooling without pip or
    # installation into the supplied host Python environment.
    wheels = list((work / "upstream/python/wheels").glob("meson-*-py3-none-any.whl"))
    if len(wheels) != 1:
        raise ValueError("Expected one pinned QEMU-bundled Meson wheel")
    destination = work / "host-tools/meson"
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".meson-", dir=destination.parent) as temporary:
            with zipfile.ZipFile(wheels[0]) as source:
                for member in source.namelist():
                    parts = Path(member).parts
                    if Path(member).is_absolute() or ".." in parts:
                        raise ValueError("Unsafe bundled Meson wheel path")
                source.extractall(temporary)
            Path(temporary).rename(destination)
    env = os.environ.copy()
    env["PYTHONPATH"] = str(destination) + os.pathsep + env.get("PYTHONPATH", "")
    return [str(python), "-m", "mesonbuild.mesonmain"], env


def build_dependencies(work, toolchain, python, lock, jobs):
    prefix = work / "prefix"
    for directory in ("lib/pkgconfig", "include"):
        (prefix / directory).mkdir(parents=True, exist_ok=True)
    cc = toolchain / f"bin/aarch64-linux-android{lock['android_api']}-clang"
    cxx = Path(str(cc) + "++")
    ar, ranlib = toolchain / "bin/llvm-ar", toolchain / "bin/llvm-ranlib"
    configure_tools = [f"CC={cc}", f"AR={ar}", f"RANLIB={ranlib}", "CFLAGS=-O2 -fPIC"]
    pcre = work / "build-pcre2"
    pcre.mkdir(exist_ok=True)
    source = work / "sources" / lock["dependencies"]["pcre2"]["prefix"]
    run([source / "configure", "--host=aarch64-linux-android", f"--prefix={prefix}",
         "--enable-pcre2-8", "--disable-pcre2-16", "--disable-pcre2-32", "--disable-jit",
         "--disable-shared", "--enable-static", "--disable-pcre2grep", "--disable-pcre2test",
         *configure_tools], pcre, work / "configure-pcre2.log")
    run(["make", f"-j{jobs}", "libpcre2-8.la"], pcre, work / "build-pcre2.log")
    for original, relative in ((pcre / ".libs/libpcre2-8.a", "lib/libpcre2-8.a"),
                               (pcre / "src/pcre2.h", "include/pcre2.h"),
                               (pcre / "libpcre2-8.pc", "lib/pkgconfig/libpcre2-8.pc")):
        shutil.copy2(original, prefix / relative)
    gettext = work / "build-gettext"
    gettext.mkdir(parents=True, exist_ok=True)
    intl = gettext / "intl"
    source = work / "sources" / lock["dependencies"]["gettext"]["prefix"] / "gettext-runtime"
    run([source / "configure", "--host=aarch64-linux-android", f"--prefix={prefix}",
         "--disable-shared", "--enable-static", "--without-libiconv-prefix",
         "--disable-java", "--disable-csharp", "--disable-libasprintf",
         "--disable-dependency-tracking", *configure_tools], gettext, work / "configure-intl.log")
    # This is only the LGPL intl runtime and its runtime gnulib library.
    run(["make", f"-j{jobs}"], intl, work / "build-intl.log")
    shutil.copy2(intl / ".libs/libintl.a", prefix / "lib/libintl.a")
    shutil.copy2(intl / "libintl.h", prefix / "include/libintl.h")
    (prefix / "lib/pkgconfig/intl.pc").write_text(
        f"prefix={prefix}\nlibdir=${{prefix}}/lib\nincludedir=${{prefix}}/include\n"
        "Name: GNU libintl\nDescription: GNU gettext standalone intl runtime\nVersion: 1.0\n"
        "Libs: -L${libdir} -lintl\nLibs.private: -lm\nCflags: -I${includedir}\n")
    cross = work / "android-aarch64-api35.ini"
    c_args = ["-fPIC", f"-I{prefix}/include"]
    ld_args = ["-Wl,-z,max-page-size=16384", f"-L{prefix}/lib", "-lm"]
    cross.write_text(
        f"[binaries]\nc = {str(cc)!r}\ncpp = {str(cxx)!r}\nar = {str(ar)!r}\n"
        f"strip = {str(toolchain / 'bin/llvm-strip')!r}\npkg-config = {shutil.which('pkg-config')!r}\n"
        "[host_machine]\nsystem = 'android'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n"
        f"[properties]\nneeds_exe_wrapper = true\npkg_config_libdir = [{str(prefix / 'lib/pkgconfig')!r}]\n"
        f"[built-in options]\nc_args = {c_args!r}\ncpp_args = {c_args!r}\n"
        f"c_link_args = {ld_args!r}\ncpp_link_args = {ld_args!r}\n")
    meson, env = meson_tool(work, python)
    build = work / "build-glib"
    source = work / "sources" / lock["dependencies"]["glib"]["prefix"]
    options = ["--cross-file", cross, "--prefix", prefix, "--libdir", "lib", "--wrap-mode=nodownload",
               "--buildtype=release", "-Ddefault_library=static", "-Dtests=false", "-Dinstalled_tests=false",
               "-Ddocumentation=false", "-Dman-pages=disabled", "-Dintrospection=disabled",
               "-Dselinux=disabled", "-Dlibmount=disabled", "-Dsysprof=disabled", "-Ddtrace=disabled",
               "-Dsystemtap=disabled", "-Dnls=disabled"]
    reconfigure = ["--reconfigure"] if (build / "meson-private/coredata.dat").exists() else []
    run([*meson, "setup", *reconfigure, build, source, *options], work, work / "configure-glib.log", env)
    run(["ninja", "-C", build, f"-j{jobs}", "glib/libglib-2.0.a", "gmodule/libgmodule-2.0.a"],
        work, work / "build-glib.log")
    run([*meson, "install", "-C", build, "--no-rebuild", "--tags", "runtime,devel"],
        work, work / "install-glib.log", env)


def configure_qemu(work, toolchain, python, lock):
    prefix, build = work / "prefix", work / "build-qemu"
    build.mkdir(exist_ok=True)
    pkg = shutil.which("pkg-config")
    if not pkg:
        raise ValueError("pkg-config is required; no installation is performed")
    wrapper = work / "pkg-config-android-static"
    wrapper.write_text(f"#!/bin/sh\nexec {shlex.quote(pkg)} --static \"$@\"\n")
    wrapper.chmod(0o755)
    env = os.environ.copy()
    for variable, binary in (("AR", "llvm-ar"), ("RANLIB", "llvm-ranlib"), ("NM", "llvm-nm"),
                             ("STRIP", "llvm-strip"), ("READELF", "llvm-readelf"), ("LD", "ld.lld"),
                             ("OBJCOPY", "llvm-objcopy")):
        env[variable] = str(toolchain / "bin" / binary)
    env.update(PKG_CONFIG=str(wrapper), PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"), PKG_CONFIG_PATH="")
    actual_cc = toolchain / f"bin/aarch64-linux-android{lock['android_api']}-clang"
    # QEMU's executable target adds -fPIE after its extra C flags. The CPU
    # boundary is a DSO, so every reused object must end with PIC and dynamic
    # TLS rather than executable-only local-exec TLS relocations.
    cc, cxx = work / "android-clang", work / "android-clang++"
    for wrapper, compiler in ((cc, actual_cc), (cxx, Path(str(actual_cc) + "++"))):
        wrapper.write_text(f"#!/bin/sh\nexec {shlex.quote(str(compiler))} \"$@\" -fPIC -ftls-model=global-dynamic\n")
        wrapper.chmod(0o755)
    argv = [work / "upstream/configure", f"--python={python}", "--target-list=x86_64-softmmu",
            "--without-default-features", "--without-default-devices", "--enable-system", "--enable-tcg",
            "--disable-user", "--disable-docs", "--disable-tools", "--disable-guest-agent", "--disable-plugins",
            "--disable-rust", "--disable-werror", "--disable-download", "--disable-install-blobs",
            "--disable-containers", f"--cc={cc}", f"--cxx={cxx}", f"--host-cc={shutil.which('clang')}",
            "--cross-prefix=aarch64-linux-android-", f"--extra-cflags=-fPIC -I{prefix}/include",
            f"--extra-ldflags=-Wl,-z,max-page-size=16384 -L{prefix}/lib"]
    write_json(work / "qemu-configure-request.json", {"cwd": str(build), "argv": list(map(str, argv)),
               "env": {key: env[key] for key in ("AR", "RANLIB", "NM", "STRIP", "READELF", "LD",
                                                "OBJCOPY", "PKG_CONFIG", "PKG_CONFIG_LIBDIR", "PKG_CONFIG_PATH")}})
    run(argv, build, work / "configure-qemu.log", env)


def verify_patch_receipt_rejection(work, output, host, receipt_prefix, label, log_tag,
                                   extra_arguments=()):
    """Exercise the real admission CLI without changing the library receipt."""
    results = {}
    with tempfile.TemporaryDirectory(prefix=f".{log_tag}-receipt-negative-", dir=work) as directory:
        isolated = Path(directory) / output.name
        os.link(output, isolated)
        cases = ("patch_hash", "source_hash", "abi_version") if receipt_prefix == "persistent_ir_cache" else \
            ("patch_hash", "source_hash")
        for case in cases:
            receipt = json.loads(Path(str(output) + ".json").read_text())
            if case == "patch_hash":
                receipt[receipt_prefix + "_patch_sha256"] = "0" * 64
            elif case == "source_hash":
                sources = receipt[receipt_prefix + "_source_sha256"]
                sources[next(iter(sources))] = "0" * 64
            else:
                receipt[receipt_prefix + "_abi_version"] = 0
            write_json(Path(str(isolated) + ".json"), receipt)
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/VerifyLibrary.py"),
                 "--library", str(isolated), "--host", host, *extra_arguments],
                cwd=work, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                check=False)
            (work / f"verify-{log_tag}-receipt-negative-{case}.log").write_text(result.stdout)
            if result.returncode == 0 or f"QEMU {label} does not match" not in result.stdout:
                raise RuntimeError(f"Altered {label} receipt was not rejected by its admission gate: {case}")
            results[case] = "rejected"
    return results


def verify_jit_receipt_rejection(work, output, host, extra_arguments=()):
    return verify_patch_receipt_rejection(work, output, host, "jit_execute_scope",
                                         "JIT execute scope", "jit", extra_arguments)


def verify_tb_receipt_rejection(work, output, host, extra_arguments=()):
    return verify_patch_receipt_rejection(work, output, host, "tb_exec_boundary",
                                         "TB execution boundary", "tb", extra_arguments)


def verify_ir_receipt_rejection(work, output, host, extra_arguments=()):
    return verify_patch_receipt_rejection(work, output, host, "persistent_ir_cache",
                                         "persistent IR cache", "ir-cache", extra_arguments)


def verify_ir_identity_receipt_rejection(work, output, host, extra_arguments=()):
    results = {}
    with tempfile.TemporaryDirectory(prefix=".ir-identity-receipt-negative-", dir=work) as directory:
        isolated = Path(directory) / output.name
        os.link(output, isolated)
        for case in ("build_id", "host_abi", "compile_profile", "missing_identity"):
            receipt = json.loads(Path(str(output) + ".json").read_text())
            if case == "build_id":
                receipt["ir_cache_build_id"] = "0" * 64
            elif case == "host_abi":
                receipt["ir_cache_build_identity"]["host_abi"] = "unknown-host"
            elif case == "compile_profile":
                receipt["compile_arguments"].append("-DLUMEN_UNREVIEWED_IR_ABI=1")
            else:
                receipt.pop("ir_cache_build_identity")
            write_json(Path(str(isolated) + ".json"), receipt)
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/VerifyLibrary.py"), "--library", str(isolated),
                 "--host", host, *extra_arguments], cwd=work, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, check=False)
            (work / f"verify-ir-identity-receipt-negative-{case}.log").write_text(result.stdout)
            if result.returncode == 0 or "QEMU persistent IR cache build identity does not match" not in result.stdout:
                raise RuntimeError(f"Altered IR cache build identity was not rejected: {case}")
            results[case] = "rejected"
    return results


def audit_library(work, output, toolchain, lock, verify_code_ram=False):
    run([sys.executable, ROOT / "tools/VerifyLibrary.py", "--library", output, "--host", "android",
         *(["--allow-code-ram-verification"] if verify_code_ram else [])],
        work, output.with_suffix(".verify.log"))
    jit_receipt_rejections = verify_jit_receipt_rejection(
        work, output, "android", ["--allow-code-ram-verification"] if verify_code_ram else [])
    tb_receipt_rejections = verify_tb_receipt_rejection(
        work, output, "android", ["--allow-code-ram-verification"] if verify_code_ram else [])
    ir_receipt_rejections = verify_ir_receipt_rejection(
        work, output, "android", ["--allow-code-ram-verification"] if verify_code_ram else [])
    ir_identity_rejections = verify_ir_identity_receipt_rejection(
        work, output, "android", ["--allow-code-ram-verification"] if verify_code_ram else [])
    elf = subprocess.check_output([str(toolchain / "bin/llvm-readelf"), "-h", "-l", "-d", "--dyn-syms",
                                   str(output)], text=True)
    output.with_suffix(".elf.txt").write_text(elf)
    needed = [line.split("[")[1].split("]")[0] for line in elf.splitlines() if "(NEEDED)" in line]
    if set(needed) - {"libc.so", "libm.so", "libz.so", "libdl.so", "liblog.so"}:
        raise ValueError(f"Unexpected Android shared-library imports: {needed}")
    inputs = {}
    link_receipt = json.loads(Path(str(output) + ".json").read_text())
    linked_files = {}
    for argument in link_receipt["link_arguments"]:
        if argument.endswith((".a", ".o")):
            path = Path(argument)
            if not path.is_absolute():
                path = work / "build-qemu" / path
            if not path.is_file():
                raise ValueError(f"Linked input is missing: {path}")
            linked_files[str(path.resolve())] = digest(path)
    for build in (work / "build-qemu", work / "build-glib"):
        for entry in json.loads((build / "compile_commands.json").read_text()):
            obj = build / entry["output"]
            source = Path(entry["directory"]) / entry["file"]
            if obj.is_file() and source.is_file():
                inputs[str(source.resolve())] = digest(source)
        # Ninja retains compiler-reported header dependencies after removing
        # transient .d files. Preserve their notices as well as C entry points.
        dependencies = subprocess.check_output(["ninja", "-t", "deps"], cwd=build, text=True)
        for line in dependencies.splitlines():
            if line.startswith("    "):
                source = (build / line.strip()).resolve()
                if str(source) not in inputs and source.is_file() and source.is_relative_to(work):
                    inputs[str(source)] = digest(source)
    # Record the actual input archives' member names and corresponding runtime
    # source files. Headers below are a conservative superset; final selected
    # member closure still requires the link map and independent legal review.
    archive_members = {}
    runtime_source = work / "sources/gettext-1.0/gettext-runtime/intl"
    pcre_source = work / "sources/pcre2-10.47/src"
    for archive, source in ((work / "prefix/lib/libintl.a", runtime_source),
                            (work / "prefix/lib/libpcre2-8.a", pcre_source)):
        members = subprocess.check_output([str(toolchain / "bin/llvm-ar"), "t", str(archive)], text=True).splitlines()
        archive_members[str(archive)] = members
        for member in members:
            filename = member.rsplit("_la-", 1)[-1].removesuffix(".o") + ".c"
            candidates = list(source.rglob(filename))
            if archive.name == "libpcre2-8.a" and filename == "pcre2_chartables.c":
                generated = work / "build-pcre2/src/pcre2_chartables.c"
                distributed = pcre_source / "pcre2_chartables.c.dist"
                if not generated.is_file() or digest(generated) != digest(distributed):
                    raise ValueError("Generated PCRE2 chartables do not match the pinned source")
                candidates = [distributed]
            if len(candidates) != 1:
                raise ValueError(f"Runtime archive member source is ambiguous or missing: {member}")
            path = candidates[0].resolve()
            inputs[str(path)] = digest(path)
        for path in source.rglob("*.h"):
            inputs[str(path.resolve())] = digest(path)
    for path in (work / "prefix/lib").glob("*.a"):
        inputs[str(path)] = digest(path)
    for relative in ("LumenQemuTcg.c", "LumenQemuTcg.h",
                     "LumenQemuTcgCache.c", "LumenQemuTcgCache.h"):
        inputs[str(ROOT / relative)] = digest(ROOT / relative)
    write_json(output.with_suffix(".inputs.json"), {"linked_files_sha256": linked_files,
               "observed_build_inputs_sha256": inputs, "dependency_archive_members": archive_members,
               "scope": "Built QEMU/GLib sources and dependencies, runtime archive sources and header superset; broader than final linked object closure",
               "license_verdict": "NOT CERTIFIED"})
    notices = output.parent / "licenses"
    selections = {"qemu": (work / "upstream", ["LICENSE", "COPYING", "COPYING.LIB"]),
                  "glib": (work / "sources/glib-2.88.3", ["COPYING", "LICENSES"]),
                  "pcre2": (work / "sources/pcre2-10.47", ["COPYING", "LICENCE.md"]),
                  "libintl": (work / "sources/gettext-1.0/gettext-runtime/intl", ["COPYING.LIB"]),
                  "ndk-llvm": (toolchain, ["NOTICE", "sysroot/NOTICE"])}
    for name, (source, paths) in selections.items():
        target = notices / name
        target.mkdir(parents=True, exist_ok=True)
        for relative in paths:
            original = source / relative
            if original.is_dir():
                shutil.copytree(original, target / relative, dirs_exist_ok=True)
            else:
                (target / relative).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(original, target / relative)
    # Keep the original per-file copyright/license preambles for the observed
    # compiled closure; these raw notices are evidence, not a legal verdict.
    source_notices = {}
    for path in inputs:
        source = Path(path)
        if (source.suffix not in (".c", ".h") and
                not source.name.endswith((".c.dist", ".c.inc"))) or not source.is_relative_to(work):
            continue
        lines = source.read_text(errors="replace").splitlines(True)
        preamble, in_comment = [], False
        for line in lines:
            stripped = line.strip()
            if in_comment:
                preamble.append(line)
                in_comment = "*/" not in line
            elif not stripped or stripped.startswith("//"):
                preamble.append(line)
            elif stripped.startswith("/*"):
                preamble.append(line)
                in_comment = "*/" not in line
            else:
                break
        source_notices[str(source.relative_to(work))] = {
            "source_sha256": inputs[path], "original_preamble": "".join(preamble).strip(),
            "scope": "Original leading comments; consult preserved source for all notices",
        }
    write_json(notices / "compiled-source-notices.json", source_notices)
    exported = {str(path.relative_to(notices)): {"sha256": digest(path), "bytes": path.stat().st_size}
                for path in sorted(notices.rglob("*")) if path.is_file() and path.name != "manifest.json"}
    write_json(notices / "manifest.json", {
        "schema": 1, "library_sha256": digest(output),
        "source_lock_sha256": digest(ROOT / "source-lock.json"),
        "dependency_lock_sha256": digest(ROOT / "dependency-lock.json"),
        "files": exported, "binary_distribution_qualified": False,
    })
    receipt = {"qemu_version": "11.1.2", "ndk": lock["ndk_version"], "android_api": lock["android_api"],
               "architecture": "aarch64", "elf_load_alignment": lock["elf_load_alignment"],
               "needed": needed, "library_sha256": digest(output),
               "cache_source_sha256": link_receipt["cache_source_sha256"],
               "cache_header_sha256": link_receipt["cache_header_sha256"],
               "ir_cache_build_id": link_receipt["ir_cache_build_id"],
               "persistent_ir_cache_abi_version": link_receipt["persistent_ir_cache_abi_version"],
               "dependency_lock_sha256": digest(ROOT / "dependency-lock.json"),
               "recipe_sha256": digest(Path(__file__)),
               "build_recipe_sha256": link_receipt["build_recipe_sha256"],
               "jit_execute_scope_receipt_negative_tests": jit_receipt_rejections,
               "tb_exec_boundary_receipt_negative_tests": tb_receipt_rejections,
               "persistent_ir_cache_receipt_negative_tests": ir_receipt_rejections,
               "persistent_ir_cache_identity_negative_tests": ir_identity_rejections,
               "notice_manifest_sha256": digest(notices / "manifest.json"),
               "runtime_qualified": False, "android_16k_runtime_qualified": False,
               "full_lumen_link_closure_certified": False, "binary_distribution_qualified": False}
    write_json(output.with_suffix(".android.json"), receipt)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--prepare", action="store_true", help="Prepare and check pinned sources only")
    parser.add_argument("--download", action="store_true", help="Allow source downloads during --prepare")
    parser.add_argument("--library-only", action="store_true", help="Reuse an already configured Android build")
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--python", type=Path, default=Path(sys.executable), help="Existing host build Python")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1 or (args.download and not args.prepare) or (args.prepare and args.library_only):
        parser.error("Use positive --jobs; --download requires --prepare; preparation and library-only are separate")
    work = args.work.expanduser().resolve()
    work.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / "dependency-lock.json").read_text())
    qemu_lock = json.loads((ROOT / "source-lock.json").read_text())
    prepare(work, lock, qemu_lock, args.download)
    if args.prepare:
        print(json.dumps({"prepared": str(work), "downloads_allowed": args.download, "signature_verified": False}))
        return
    ndk = find_ndk(args.ndk, lock["ndk_version"])
    host = "darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64"
    toolchain = ndk / "toolchains/llvm/prebuilt" / host
    if not toolchain.is_dir():
        raise ValueError(f"Unsupported or missing NDK host toolchain: {toolchain}")
    # Preserve the selected virtual environment's interpreter symlink.
    python = Path(os.path.abspath(args.python.expanduser()))
    if not args.library_only:
        build_dependencies(work, toolchain, python, lock, args.jobs)
        configure_qemu(work, toolchain, python, lock)
    output = (args.output or work / "lib/liblumen_qemu_tcg.so").expanduser().resolve()
    run([python, ROOT / "tools/BuildLibrary.py", "--build", work / "build-qemu", "--output", output,
         "--jobs", args.jobs], work, work / "build-library.log")
    print(json.dumps(audit_library(work, output, toolchain, lock)))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError, tarfile.TarError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
