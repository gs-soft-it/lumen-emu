#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Original archive bootstrap added for Lumen's core source export, 2026-10-23.
"""Prepare the exact source dependencies recorded beside the core archive.

Requires Python 3.9+ and Git. Fresh checkouts retain their own Git metadata,
origins and pinned commits for the existing core build/overlay verifiers.
Existing checkouts are never reset, cleaned, switched or replaced.
"""

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile


class PreparationError(Exception):
    pass


def git(directory, *arguments):
    env = os.environ.copy()
    # Read-only checks must not refresh index metadata or prompt for credentials.
    env["GIT_OPTIONAL_LOCKS"] = "0"
    env["GIT_TERMINAL_PROMPT"] = "0"
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise PreparationError(f"Git failed in {directory}: {detail}")
    return result.stdout.strip()


def checked_path(root, relative):
    parts = PurePosixPath(relative)
    if parts.is_absolute() or not parts.parts or ".." in parts.parts:
        raise PreparationError(f"Unsafe dependency path in lock: {relative!r}")
    path = root
    for part in parts.parts:
        path = path / part
        if path.is_symlink():
            raise PreparationError(f"Preserving symbolic link; refusing dependency path: {path}")
    return path


def normalized_url(url):
    return url.rstrip("/").removesuffix(".git")


def validate_checkout(path, entry):
    if not path.is_dir() or not (path / ".git").exists():
        raise PreparationError(f"Preserving existing non-repository path: {path}")
    if Path(git(path, "rev-parse", "--show-toplevel")).resolve() != path.resolve():
        raise PreparationError(f"Preserving path that is not its own checkout: {path}")
    actual = git(path, "rev-parse", "HEAD")
    if actual != entry["commit"]:
        raise PreparationError(
            f"Preserving checkout at the wrong revision: {path}\n"
            f"  expected {entry['commit']}, found {actual}"
        )
    remote = git(path, "remote", "get-url", "origin")
    if normalized_url(remote) != normalized_url(entry["url"]):
        raise PreparationError(f"Preserving checkout with unexpected origin: {path}")
    if git(path, "status", "--porcelain", "--untracked-files=all", "--ignore-submodules=none"):
        raise PreparationError(f"Preserving dirty checkout (including untracked files): {path}")


def validate_gitlink(parent, child):
    expected = f"160000 commit {child['commit']}\t{child['path']}"
    actual = git(parent, "ls-tree", "HEAD", "--", child["path"])
    if actual != expected:
        raise PreparationError(f"Pinned submodule does not match parent Git tree: {parent / child['path']}")


def inspect(root, dependencies):
    """Validate every existing checkout before any network access or mutation."""
    missing = []
    for entry in dependencies:
        parent = checked_path(root, entry["path"])
        if not parent.exists():
            missing.append(parent)
            continue
        validate_checkout(parent, entry)
        for child in entry.get("submodules", []):
            validate_gitlink(parent, child)
            path = checked_path(parent, child["path"])
            if path.exists():
                validate_checkout(path, child)
            else:
                missing.append(path)
    return missing


def prepare_checkout(path, entry):
    """Build in a new temporary directory, then install at an absent path."""
    if path.exists() or path.is_symlink():
        raise PreparationError(f"Preserving existing path; refusing replacement: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{path.name}-download-", dir=path.parent))
    try:
        print(f"Fetching {entry['path']} at {entry['commit']}", flush=True)
        git(stage, "init", "--quiet")
        git(stage, "remote", "add", "origin", entry["url"])
        git(stage, "-c", "fetch.recurseSubmodules=false", "fetch", "--depth", "1", "origin", entry["commit"])
        git(stage, "-c", "submodule.recurse=false", "checkout", "--quiet", "--detach", entry["commit"])
        for child in entry.get("submodules", []):
            validate_gitlink(stage, child)
            nested = checked_path(stage, child["path"])
            # Git creates empty gitlink placeholders in this NEW temporary tree.
            # No pre-existing user directory is removed by this operation.
            if nested.is_dir() and not any(nested.iterdir()):
                nested.rmdir()
            prepare_checkout(nested, child)
        validate_checkout(stage, entry)
        if path.exists() or path.is_symlink():
            raise PreparationError(f"Target appeared during download; preserving it: {path}")
        stage.rename(path)
        stage = None
    finally:
        if stage is not None:
            shutil.rmtree(stage)


def load_lock(path):
    with path.open(encoding="utf-8") as stream:
        lock = json.load(stream)
    if lock.get("schema") != 1 or not isinstance(lock.get("dependencies"), list):
        raise PreparationError(f"Unsupported dependency lock: {path}")
    for dependency in lock["dependencies"]:
        for entry in [dependency, *dependency.get("submodules", [])]:
            if not re.fullmatch(r"[0-9a-f]{40}", entry.get("commit", "")):
                raise PreparationError("Each dependency must have an exact 40-character commit pin")
            if not entry.get("url", "").startswith("https://github.com/"):
                raise PreparationError("Expected a public HTTPS GitHub dependency URL")
            checked_path(Path("."), entry["path"])
    return lock["dependencies"]


def main():
    default_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=default_root,
                        help="source tree to prepare/check (default: this archive's root)")
    parser.add_argument("--check", action="store_true",
                        help="validate pins, origins and clean worktrees without writes or network access")
    args = parser.parse_args()
    if shutil.which("git") is None:
        raise PreparationError("Git is required and was not found on PATH")
    root = args.root.resolve()
    if not root.is_dir():
        raise PreparationError(f"Source root does not exist: {root}")
    # The shipped lock stays authoritative even when --root points to another tree.
    dependencies = load_lock(default_root / "source-dependencies.lock.json")
    missing = inspect(root, dependencies)
    if args.check:
        if missing:
            raise PreparationError("Missing dependencies:\n" + "\n".join(f"  {path}" for path in missing))
    else:
        for entry in dependencies:
            parent = checked_path(root, entry["path"])
            if not parent.exists():
                prepare_checkout(parent, entry)
            else:
                for child in entry.get("submodules", []):
                    nested = checked_path(parent, child["path"])
                    if not nested.exists():
                        prepare_checkout(nested, child)
        remaining = inspect(root, dependencies)
        if remaining:
            raise PreparationError("Dependency preparation is incomplete")
    count = sum(1 + len(entry.get("submodules", [])) for entry in dependencies)
    print(f"OK: {count} clean source checkouts match the locked revisions.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (PreparationError, OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
