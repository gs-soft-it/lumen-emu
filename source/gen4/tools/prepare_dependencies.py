#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare the immutable Gen4 library dependencies outside the source tree."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path
import subprocess
import sys


def git(root, *args, check=True):
    result = subprocess.run(["git", "-C", str(root), *args], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and result.returncode:
        raise RuntimeError(result.stderr.strip())
    return result


def prepare(record, cache, seed):
    name, pin, url = record["name"], record["commit"], record["url"]
    if "repository_path" in record:
        repository = Path(__file__).resolve().parents[3]
        checkout = repository / record["repository_path"]
        if not (checkout / "CMakeLists.txt").exists():
            git(repository, "submodule", "update", "--init", "--", record["repository_path"])
        if git(checkout, "rev-parse", "HEAD").stdout.strip() != pin:
            raise RuntimeError("repository dependency revision changed; preserved: " + str(checkout))
        if git(checkout, "status", "--porcelain", "--untracked-files=all").stdout:
            raise RuntimeError("repository dependency has local changes; preserved: " + str(checkout))
        git(checkout, "submodule", "update", "--init", "--recursive")
        return name
    checkout = cache / "checkouts" / (name.replace("/", "-") + "-" + pin[:12])
    checkout.parent.mkdir(parents=True, exist_ok=True)
    if not checkout.exists():
        checkout.mkdir()
        git(checkout, "init", "--quiet")
        git(checkout, "remote", "add", "origin", url)
    if not (checkout / ".git").exists():
        raise RuntimeError("existing dependency is not a checkout: " + str(checkout))
    observed = git(checkout, "rev-parse", "--verify", "HEAD", check=False)
    if observed.returncode:
        local = seed / "externals" / name if seed else None
        if local and (local / ".git").exists() and git(local, "cat-file", "-e", pin, check=False).returncode == 0:
            git(checkout, "-c", "protocol.file.allow=always", "fetch", "--no-tags", str(local), pin)
        else:
            git(checkout, "fetch", "--depth=1", "--no-tags", "origin", pin)
        git(checkout, "checkout", "--detach", pin)
    elif observed.stdout.strip() != pin:
        raise RuntimeError("dependency revision changed; preserved: " + str(checkout))
    if git(checkout, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none").stdout:
        raise RuntimeError("dependency has local changes; preserved: " + str(checkout))
    if git(checkout, "remote", "get-url", "origin").stdout.strip() != url:
        raise RuntimeError("dependency origin mismatch: " + name)
    git(checkout, "submodule", "update", "--init", "--recursive", "--depth=1")
    destination = cache / "sources" / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink():
        if destination.resolve() != checkout.resolve():
            raise RuntimeError("dependency link points elsewhere: " + str(destination))
    elif destination.exists():
        raise RuntimeError("dependency destination already exists: " + str(destination))
    else:
        destination.symlink_to(checkout, target_is_directory=True)
    return name


def prepare_direct_cpu(repository):
    relative = "source/3rdparty/fex-gen4"
    source = repository / relative
    pin = "f2b679f6028ce1c38875233aecfcf5d3f8ebecec"
    if not (source / "FEXCore/CMakeLists.txt").exists():
        git(repository, "submodule", "update", "--init", "--", relative)
    if git(source, "rev-parse", "HEAD").stdout.strip() != pin:
        raise RuntimeError("direct CPU revision changed; preserved: " + str(source))
    if git(source, "status", "--porcelain", "--untracked-files=all").stdout:
        raise RuntimeError("direct CPU has local changes; preserved: " + str(source))
    git(source, "submodule", "update", "--init", "--",
        "External/fmt", "External/xxhash", "External/unordered_dense",
        "External/rpmalloc", "External/range-v3")
    print("Prepared direct Android CPU " + pin, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=Path(__file__).resolve().parents[4] / "lumen-deps/gen4")
    parser.add_argument("--platform", choices=("macos", "android", "windows"), default="macos")
    parser.add_argument("--seed", type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.platform == "android":
        prepare_direct_cpu(Path(__file__).resolve().parents[3])
    lock = json.loads(Path(__file__).with_name("dependencies.lock.json").read_text())
    records = [record for record in lock["dependencies"] if args.platform in record["platforms"]]
    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {pool.submit(prepare, record, args.cache.resolve(), args.seed): record for record in records}
        for future in as_completed(pending):
            try:
                print("Prepared " + future.result(), flush=True)
            except Exception as error:
                failures.append(str(error))
                print(str(error), file=sys.stderr, flush=True)
    if failures:
        return 1
    print(str((args.cache / "sources").resolve()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
