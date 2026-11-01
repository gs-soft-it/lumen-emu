#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fetch the pinned Android PanVK package into the external dependency tree."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import urllib.request
import zipfile


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=repository.parent / 'lumen-deps/panvk-jim-r8')
    args = parser.parse_args()
    destination = args.output.resolve()
    if destination == repository or repository in destination.parents:
        parser.error('Driver binaries must remain outside the repository')
    lock = json.loads((repository / 'android/gpu-drivers/panvk/source-lock.json').read_text())
    with urllib.request.urlopen(lock['archive_url'], timeout=60) as response:
        archive = response.read()
    if hashlib.sha256(archive).hexdigest() != lock['archive_sha256']:
        raise RuntimeError('PanVK archive SHA256 mismatch')
    files = {}
    with zipfile.ZipFile(io.BytesIO(archive)) as package:
        if sorted(package.namelist()) != sorted(lock['files']):
            raise RuntimeError('PanVK archive contents differ from the pinned package')
        for name, expected in lock['files'].items():
            payload = package.read(name)
            if len(payload) != expected['bytes'] or hashlib.sha256(payload).hexdigest() != expected['sha256']:
                raise RuntimeError('PanVK file integrity failed: ' + name)
            files[name] = payload
    # Verify the complete input before publishing it. Preserve any different
    # existing artifact instead of silently replacing a developer's candidate.
    for name, payload in files.items():
        path = destination / name
        if path.exists() and path.read_bytes() != payload:
            raise RuntimeError('Refusing to replace a different existing file: ' + str(path))
    destination.mkdir(parents=True, exist_ok=True)
    for name, payload in files.items():
        with (destination / name).open('wb') as output:
            output.write(payload)
    print('Verified PanVK package: ' + str(destination))


if __name__ == '__main__':
    main()
