#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare the immutable Gen4 FEX pin with Darwin host adaptations only."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


PIN = "f2b679f6028ce1c38875233aecfcf5d3f8ebecec"
SUBMODULES = {
    "External/fmt": "407c905e45ad75fc29bf0f9bb7c5c2fd3475976f",
    "External/range-v3": "ca1388fb9da8e69314dda222dc7b139ca84e092f",
    "External/rpmalloc": "1d85c246cd827ead6865f4f880d4fef53f2b1864",
    "External/unordered_dense": "3234af2c03549bc85656bfd3a86993bf1cd8aef1",
    "External/xxhash": "e626a72bc2321cd320e953a0ccf1584cad60f363",
}


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args], text=True).strip()


def inventory(root):
    return {
        str(path.relative_to(root)): (
            "link:" + os.readlink(path) if path.is_symlink()
            else hashlib.sha256(path.read_bytes()).hexdigest()
        )
        for path in sorted(root.rglob("*"))
        if path.name != ".lumen-overlay.json" and (path.is_file() or path.is_symlink())
    }


def archive(source, revision, destination, temporary):
    packed = temporary / "source.tar"
    subprocess.run(["git", "-C", str(source), "archive", "--format=tar", "-o", str(packed), revision], check=True)
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(packed) as contents:
        contents.extractall(destination, filter="data")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    source, destination = args.source.resolve(), args.destination.resolve()
    if git(source, "remote", "get-url", "origin") != "https://github.com/FEX-Emu/FEX.git":
        raise ValueError("FEX source must use the official origin")
    for relative, revision in {"": PIN, **SUBMODULES}.items():
        component = source / relative
        if git(component, "rev-parse", "HEAD") != revision:
            raise ValueError("FEX revision mismatch: " + str(component))
        if git(component, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none"):
            raise ValueError("FEX component must be clean: " + str(component))
        if relative and git(source, "rev-parse", "HEAD:" + relative) != revision:
            raise ValueError("FEX gitlink mismatch: " + relative)
    patch = Path(__file__).with_name("fex-f2b679f-darwin.patch")
    identity = {"schema": 1, "pin": PIN, "submodules": SUBMODULES,
        "darwin_patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
        "preparer_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    manifest = destination / ".lumen-overlay.json"
    if destination.exists():
        if json.loads(manifest.read_text()) != {"identity": identity, "files": inventory(destination)}:
            raise ValueError("Existing FEX overlay differs; preserved for inspection")
        print(destination)
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lumen-fex-import-") as name:
        temporary = Path(name)
        root = temporary / "source"
        archive(source, PIN, root, temporary)
        for relative, revision in SUBMODULES.items():
            archive(source / relative, revision, root / relative, temporary)
        subprocess.run(["git", "apply", "--check", str(patch)], cwd=root, check=True)
        subprocess.run(["git", "apply", str(patch)], cwd=root, check=True)
        (root / ".lumen-overlay.json").write_text(json.dumps(
            {"identity": identity, "files": inventory(root)}, indent=2, sort_keys=True) + "\n")
        # The temporary directory is outside any Git checkout so apply cannot
        # discover and accidentally address the embedding repository.
        shutil.move(str(root), str(destination))
    print(destination)


if __name__ == "__main__":
    main()
