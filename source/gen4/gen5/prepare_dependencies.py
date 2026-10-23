#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Materialize all pinned Gen5 upstream submodule sources from a clean official clone."""

import argparse
import hashlib
import io
import json
import subprocess
import tarfile
from pathlib import Path


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()


def inventory(root):
    result = {}
    for path in sorted(root.rglob("*")):
        if path.name == ".lumen-dependency.json":
            continue
        relative = str(path.relative_to(root))
        if path.is_symlink():
            result[relative] = "symlink:" + str(path.readlink())
        elif path.is_file():
            result[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Official Gen5 upstream Git clone with initialized submodules")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / "UPSTREAM.json").read_text())
    source = args.source.resolve(strict=True)
    urls = [git(source, "remote", "get-url", name)
            for name in git(source, "remote").splitlines()]
    if manifest["repository"] not in urls:
        raise ValueError("Source must identify the official Gen5 upstream repository")
    if git(source, "rev-parse", "HEAD") != manifest["commit"]:
        raise ValueError("Gen5 upstream source pin mismatch")
    if git(source, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("Gen5 upstream source checkout must be clean")
    for item in manifest["submodules"]:
        relative = Path(item["path"])
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Invalid submodule path")
        upstream = source / relative
        if git(upstream, "rev-parse", "HEAD") != item["commit"]:
            raise ValueError("Submodule pin mismatch: " + str(relative))
        if git(upstream, "status", "--porcelain", "--untracked-files=all"):
            raise ValueError("Submodule checkout must be clean: " + str(relative))
        target = here / "compatibility" / relative
        receipt = target / ".lumen-dependency.json"
        if receipt.exists():
            expected = json.loads(receipt.read_text())
            if expected["commit"] != item["commit"] or expected["files"] != inventory(target):
                raise ValueError("Existing dependency changed; preserved: " + str(target))
            continue
        if target.exists() and any(target.iterdir()):
            raise ValueError("Refusing to overwrite nonempty dependency: " + str(target))
        target.mkdir(parents=True, exist_ok=True)
        archive = subprocess.check_output(
            ["git", "-C", str(upstream), "archive", "--format=tar", item["commit"]])
        with tarfile.open(fileobj=io.BytesIO(archive)) as entries:
            entries.extractall(target, filter="data")
        receipt.write_text(json.dumps({"commit": item["commit"],
            "archive_sha256": hashlib.sha256(archive).hexdigest(),
            "files": inventory(target)}, indent=2, sort_keys=True) + "\n")
        print(str(relative) + " " + item["commit"])
    print("LUMEN_GEN5_DEPENDENCIES_VERIFIED count=" + str(len(manifest["submodules"])))


if __name__ == "__main__":
    main()
