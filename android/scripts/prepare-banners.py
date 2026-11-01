#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify and stage the pinned Banners A8xx driver and notices outside Git."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import urllib.request
import zipfile


def verified(payload, expected, name):
    if (len(payload) != expected['bytes'] or
            hashlib.sha256(payload).hexdigest() != expected['sha256']):
        raise RuntimeError('Banners file integrity failed: ' + name)
    return payload


def safe_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or str(path) != name:
        raise RuntimeError('Unsafe Banners package path: ' + name)
    return name


def main():
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=repository.parent / 'lumen-deps/turnip-banners-20261018-r3-astro')
    parser.add_argument('--archive', type=Path,
                        help='Use a local copy of the pinned upstream ZIP')
    parser.add_argument('--check', action='store_true',
                        help='Verify the prepared package without downloading or writing')
    args = parser.parse_args()
    destination = args.output.resolve()
    if destination == repository or repository in destination.parents:
        parser.error('Driver binaries must remain outside the repository')
    recipe = repository / 'android/gpu-drivers/turnip-banners'
    lock_payload = (recipe / 'source-lock.json').read_bytes()
    lock = json.loads(lock_payload)
    names = list(lock['files']) + list(lock['legal_files']) + ['source-lock.json']
    if len(names) != len(set(names)):
        raise RuntimeError('Overlapping Banners package records')
    for name in names:
        safe_name(name)
    files = {'source-lock.json': lock_payload}
    for name, expected in lock['legal_files'].items():
        files[name] = verified((recipe / name).read_bytes(), expected, name)
    if args.check:
        for name, expected in lock['files'].items():
            verified((destination / name).read_bytes(), expected, name)
        for name, payload in files.items():
            if (destination / name).read_bytes() != payload:
                raise RuntimeError('Prepared Banners notice differs: ' + name)
        print('Verified existing Banners A8xx package: ' + str(destination))
        return
    if args.archive:
        archive = args.archive.read_bytes()
    else:
        with urllib.request.urlopen(lock['archive_url'], timeout=60) as response:
            archive = response.read()
    verified(archive, {'bytes': lock['archive_bytes'],
                       'sha256': lock['archive_sha256']}, 'upstream archive')
    with zipfile.ZipFile(io.BytesIO(archive)) as package:
        if sorted(package.namelist()) != sorted(lock['files']):
            raise RuntimeError('Banners archive contents differ from the pinned package')
        for name, expected in lock['files'].items():
            files[name] = verified(package.read(name), expected, name)
    # Validate all inputs and existing paths before writing. Preserve a different
    # developer candidate; preparing this package never executes or rebuilds it.
    for name, payload in files.items():
        path = destination / name
        if path.is_symlink() or destination not in path.resolve().parents:
            raise RuntimeError('Refusing a linked Banners destination: ' + str(path))
        if path.exists() and path.read_bytes() != payload:
            raise RuntimeError('Refusing to replace a different existing file: ' + str(path))
    for name, payload in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
    print('Verified Banners A8xx package: ' + str(destination))


if __name__ == '__main__':
    main()
