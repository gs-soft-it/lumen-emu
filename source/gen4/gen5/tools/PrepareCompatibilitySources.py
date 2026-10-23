#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compose the desktop/facade source view from the shared Gen5 core and overrides."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import tempfile


SCHEMA = "lumen.gen5.compatibility-sources.v1"


def inventory(root):
    return {
        path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for subtree in ("src", "tests")
        for path in sorted((root / subtree).rglob("*")) if path.is_file()
    }


def sources(source_root):
    manifest = json.loads((source_root / "compatibility/shared-sources.json").read_text())
    if manifest.get("schema") != SCHEMA:
        raise ValueError("Unsupported compatibility source manifest")
    selected = {}
    for name in manifest["shared_files"]:
        relative = PurePosixPath(name)
        if (relative.is_absolute() or ".." in relative.parts or not relative.parts
                or relative.parts[0] not in ("src", "tests") or relative.as_posix() != name
                or name in selected):
            raise ValueError(f"Invalid shared source path: {name}")
        path = source_root / "engine" / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"Missing regular shared source: {path}")
        selected[name] = path
    overlay = source_root / "compatibility"
    for subtree in ("src", "tests"):
        for path in sorted((overlay / subtree).rglob("*")):
            if path.is_symlink():
                raise ValueError(f"Compatibility source symlink is not supported: {path}")
            if not path.is_file():
                continue
            name = path.relative_to(overlay).as_posix()
            if name in selected:
                raise ValueError(f"Duplicate shared source in compatibility overlay: {name}")
            selected[name] = path
    return selected


def prepare(source_root, output_root):
    source_root = source_root.resolve()
    output_root = output_root.resolve()
    if output_root == source_root or source_root in output_root.parents:
        raise ValueError("Generated compatibility sources must remain outside the source tree")
    selected = sources(source_root)
    expected = {name: hashlib.sha256(path.read_bytes()).hexdigest()
                for name, path in selected.items()}
    identity = hashlib.sha256(json.dumps(expected, sort_keys=True).encode()).hexdigest()
    destination = output_root / identity[:24]
    if destination.exists():
        if inventory(destination) != expected:
            raise ValueError(f"Generated compatibility sources were modified: {destination}")
        return destination
    output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="preparing-", dir=output_root) as temporary:
        stage = Path(temporary)
        for name, path in sorted(selected.items()):
            target = stage / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        if inventory(stage) != expected:
            raise ValueError("Compatibility source changed during preparation; configure again")
        (stage / "receipt.json").write_text(json.dumps({
            "schema": SCHEMA, "identity": identity, "files": expected,
        }, indent=2, sort_keys=True) + "\n")
        stage.rename(destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output-root", required=True, type=Path)
    args = parser.parse_args()
    try:
        print(prepare(args.source_root, args.output_root))
    except (ValueError, OSError, KeyError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
