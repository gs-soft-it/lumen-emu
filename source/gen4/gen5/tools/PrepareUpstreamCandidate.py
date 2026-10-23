#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Apply the reviewed PS5 update to a separate, content-addressed source tree."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def inventory(root):
    result = {}
    for subtree in ("src", "tests"):
        for path in sorted((root / subtree).rglob("*")):
            if path.is_symlink():
                raise ValueError(f"Source symlink is not supported: {path}")
            if path.is_file():
                result[path.relative_to(root).as_posix()] = sha256(path.read_bytes())
    return result


def prepare(source, manifest_path, output_root):
    manifest_bytes = manifest_path.read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest.get("schema") != "lumen.ps5.upstream-candidate.v1":
        raise ValueError("Unsupported PS5 update manifest")
    patch_path = manifest_path.parent / "kyty.patch"
    patch_bytes = patch_path.read_bytes()
    if sha256(patch_bytes) != manifest["patch_sha256"]:
        raise ValueError("PS5 update patch hash mismatch")
    original = inventory(source)
    expected = dict(original)
    seen = set()
    for entry in manifest["files"]:
        name = entry["path"]
        path = PurePosixPath(name)
        if (path.is_absolute() or ".." in path.parts or
                path.parts[0] not in ("src", "tests") or path.as_posix() != name or name in seen):
            raise ValueError(f"Invalid update path: {name}")
        seen.add(name)
        if original.get(name) != entry["before_sha256"]:
            raise ValueError(f"Baseline changed; review the candidate again: {name}")
        after = entry["after_sha256"]
        if after is None:
            expected.pop(name, None)
        else:
            expected[name] = after
    identity = sha256(manifest_bytes + json.dumps(original, sort_keys=True).encode())
    destination = output_root / identity[:24]
    if destination.exists():
        if inventory(destination) != expected:
            raise ValueError(f"Generated PS5 candidate was modified: {destination}")
        return destination
    output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="preparing-", dir=output_root) as temporary:
        stage = Path(temporary)
        for subtree in ("src", "tests"):
            shutil.copytree(source / subtree, stage / subtree)
        # git apply also rejects paths escaping the generated tree.
        environment = dict(os.environ)
        for key in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE"):
            environment.pop(key, None)
        environment["GIT_CEILING_DIRECTORIES"] = str(stage.parent.resolve())
        for options in (["--check"], []):
            subprocess.run(["git", "apply", *options, str(patch_path.resolve())],
                           cwd=stage, env=environment, check=True, capture_output=True, text=True)
        if inventory(stage) != expected:
            raise ValueError("PS5 candidate does not match its complete reviewed inventory")
        (stage / "receipt.json").write_text(json.dumps({
            "schema": manifest["schema"], "identity": identity,
            "upstream_commit": manifest["upstream_commit"],
            "patch_sha256": manifest["patch_sha256"], "files": expected,
        }, indent=2) + "\n")
        stage.rename(destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    args = parser.parse_args()
    try:
        print(prepare(args.source.resolve(), args.manifest.resolve(), args.output_root.resolve()))
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", "")
        parser.exit(1, f"{error}\n{detail}")


if __name__ == "__main__":
    main()
