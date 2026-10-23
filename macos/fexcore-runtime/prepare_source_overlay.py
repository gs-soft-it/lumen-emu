#!/usr/bin/env python3
"""Materialize reviewed patches from a verified FEX Git tree, preserving its checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inventory(root):
    result = {}
    for path in sorted(root.rglob('*')):
        if path.name == '.lumen-overlay.json':
            continue
        if path.is_symlink():
            result[str(path.relative_to(root))] = 'link:' + os.readlink(path)
        elif path.is_file():
            result[str(path.relative_to(root))] = digest(path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('pin')
    parser.add_argument('patches', nargs='+', help='absolute-path=sha256')
    args = parser.parse_args()
    source = args.source.resolve()
    def git(*cmd):
        return subprocess.check_output(['git', '-C', str(source), *cmd], text=True).strip()
    if git('rev-parse', 'HEAD') != args.pin or git('status', '--porcelain', '--untracked-files=all', '--ignore-submodules=none'):
        raise ValueError('FEX base must match the clean pinned checkout')
    if git('remote', 'get-url', 'origin') != 'https://github.com/FEX-Emu/FEX.git':
        raise ValueError('FEX origin mismatch')
    for name in ('LICENSE', 'FEXCore/LICENSE'):
        if digest(source / name) != 'f34a779f56b36d22b20e1b990d23e583a6a7ca071331925fa46156441c77a1ee':
            raise ValueError('FEX MIT license mismatch')
    patches = []
    for spec in args.patches:
        name, expected = spec.rsplit('=', 1)
        path = Path(name).resolve()
        if digest(path) != expected:
            raise ValueError('reviewed patch hash mismatch: ' + str(path))
        patches.append((path, expected))
    identity = {'base': args.pin, 'source': str(source), 'patches': [sha for _, sha in patches]}
    key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    destination = source.parent / ('fex-macos-overlay-' + key[:20])
    manifest = destination / '.lumen-overlay.json'
    if destination.exists():
        observed = json.loads(manifest.read_text())
        if observed != {'identity': identity, 'files': inventory(destination)}:
            raise ValueError('existing FEX overlay has changed; preserved for inspection')
        print(destination)
        return
    # A new unique directory is published only after all patches succeed.
    with tempfile.TemporaryDirectory(prefix='.lumen-fex-overlay-', dir=source.parent) as temporary:
        root = Path(temporary) / 'source'
        root.mkdir()
        archive = Path(temporary) / 'base.tar'
        subprocess.run(['git', '-C', str(source), 'archive', '--format=tar',
                        '-o', str(archive), args.pin], check=True)
        with tarfile.open(archive) as entries:
            entries.extractall(root, filter='data')
        for path, _ in patches:
            # An extracted tree has no Git metadata; --unsafe-paths is never used.
            subprocess.run(['git', 'apply', '--check', str(path)], cwd=root, check=True)
            subprocess.run(['git', 'apply', str(path)], cwd=root, check=True)
        # Only the four admitted submodules are linked; tracked SoftFloat/cephes
        # already came from the pinned Git archive.
        for name in ('fmt', 'xxhash', 'unordered_dense', 'rpmalloc'):
            link = root / 'External' / name
            link.rmdir()
            link.symlink_to(source / 'External' / name, target_is_directory=True)
        (root / '.lumen-overlay.json').write_text(json.dumps(
            {'identity': identity, 'files': inventory(root)}, sort_keys=True, indent=2) + '\n')
        root.rename(destination)
    print(destination)


if __name__ == '__main__':
    main()
