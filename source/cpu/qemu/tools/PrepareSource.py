#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Prepare the pinned, pristine QEMU source without installing host software."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request


LOCK_PATH = Path(__file__).resolve().parents[1] / "source-lock.json"


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def prepare(archive, destination, lock):
    if archive.stat().st_size != lock["bytes"] or digest(archive) != lock["sha256"]:
        raise ValueError("QEMU archive does not match the source lock")
    if destination.exists():
        raise ValueError(f"Destination already exists; preserving it: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".qemu-source-", dir=destination.parent) as temporary:
        staging = Path(temporary)
        prefix = f"qemu-{lock['version']}"
        excluded = set(lock["excluded_members"])
        skipped = []
        with tarfile.open(archive, "r:xz") as source:
            for member in source:
                if member.name in excluded:
                    skipped.append(member.name)
                    continue
                parts = Path(member.name).parts
                if not parts or parts[0] != prefix or ".." in parts:
                    raise ValueError(f"Unexpected archive path: {member.name}")
                # Reject escaping links, special files, and unsafe metadata.
                source.extract(member, staging, filter="data")
        if set(skipped) != excluded:
            raise ValueError("Excluded archive members changed")
        extracted = staging / prefix
        if (extracted / "VERSION").read_text().strip() != lock["version"]:
            raise ValueError("QEMU VERSION does not match the source lock")
        for notice in ("LICENSE", "COPYING", "COPYING.LIB"):
            if not (extracted / notice).is_file():
                raise ValueError(f"Required upstream notice missing: {notice}")
        receipt = {
            "version": lock["version"],
            "source_url": lock["url"],
            "archive_sha256": lock["sha256"],
            "archive_bytes": lock["bytes"],
            "excluded_members": skipped,
            "signature_verified": False,
            "source_modified": False,
        }
        (extracted / "lumen-source-provenance.json").write_text(
            json.dumps(receipt, indent=2) + "\n")
        # Same-filesystem rename publishes the complete tree only on success.
        extracted.rename(destination)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--download", action="store_true",
                        help="Download the locked official archive if absent")
    args = parser.parse_args()
    lock = json.loads(LOCK_PATH.read_text())
    archive = args.archive.expanduser().resolve()
    destination = args.destination.expanduser().resolve()
    if not archive.exists() and args.download:
        archive.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(prefix=".qemu-download-", dir=archive.parent)
        try:
            with os.fdopen(descriptor, "wb") as output:
                with urllib.request.urlopen(lock["url"], timeout=60) as response:
                    shutil.copyfileobj(response, output)
            temporary = Path(temporary_name)
            if temporary.stat().st_size != lock["bytes"] or digest(temporary) != lock["sha256"]:
                raise ValueError("Downloaded QEMU archive does not match the source lock")
            temporary.rename(archive)
        finally:
            Path(temporary_name).unlink(missing_ok=True)
    try:
        receipt = prepare(archive, destination, lock)
    except (OSError, ValueError, tarfile.TarError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
