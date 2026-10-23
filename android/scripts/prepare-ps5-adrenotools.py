#!/usr/bin/env python3
"""Acquire reviewed upstream source only, preserving a hash-bound local closure."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request

SOURCES = (
    ("libadrenotools", "8fae8ce254dfc1344527e05301e43f37dea2df80",
     "ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb"),
    ("liblinkernsbypass", "aa3975893d83ef1bc84c321ec60c65fbf1287887",
     "da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638"),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(destination):
    destination = destination.resolve()
    if destination.exists():
        raise ValueError(f"destination already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ps5-adrenotools-", dir=destination.parent) as temporary:
        work = Path(temporary)
        roots = []
        records = []
        for name, pin, expected in SOURCES:
            url = f"https://codeload.github.com/bylaws/{name}/tar.gz/{pin}"
            archive = work / f"{name}-{pin}.tar.gz"
            with urllib.request.urlopen(url, timeout=60) as source, archive.open("wb") as target:
                shutil.copyfileobj(source, target)
            if sha(archive) != expected:
                raise ValueError(f"source archive SHA256 mismatch: {name}")
            with tarfile.open(archive) as source:
                members = source.getmembers()
                prefix = f"{name}-{pin}/"
                if not members or any(not (item.name == prefix.rstrip("/") or item.name.startswith(prefix))
                        or item.issym() or item.islnk() or not (item.isfile() or item.isdir())
                        for item in members):
                    raise ValueError(f"unexpected source archive layout: {name}")
                source.extractall(work / "unpack", filter="data")
            roots.append(work / "unpack" / f"{name}-{pin}")
            records.append({"name": name, "commit": pin, "url": url,
                            "archive_sha256": expected, "license": "BSD-2-Clause"})
        root, linker = roots
        submodule = root / "lib/linkernsbypass"
        if submodule.exists():
            submodule.rmdir()  # codeload excludes the pinned Git submodule.
        shutil.copytree(linker, submodule)
        hashes = {str(path.relative_to(root)): sha(path)
                  for path in sorted(root.rglob("*")) if path.is_file()}
        tree = hashlib.sha256("".join(f"{path}:{digest}\n" for path, digest in
            sorted(hashes.items())).encode()).hexdigest()
        if len(hashes) != 50 or tree != "d73b46ba72829dc3771a6017713d4d5561dd05789a9dfc06b8d57a1ff6e626e7":
            raise ValueError("reviewed source tree seal mismatch")
        receipt = {"schema": "LUMEN_PS5_ADRENOTOOLS_SOURCE_V1", "sources": records,
                   "files": hashes, "tree_sha256": tree, "upstream_modified": False,
                   "public_binary_distribution_qualified": False}
        (root / "SOURCE.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
        (root / "SOURCE.txt").write_text(
            "Lumen PS5 Android per-process Vulkan host loader.\n"
            "Upstream source is unmodified; integration is maintained separately in Lumen.\n"
            + "\n".join(f"{item['name']} {item['commit']}\n{item['url']}\n"
                         f"SHA256 {item['archive_sha256']}\nBSD-2-Clause"
                         for item in records)
            + "\nPublic binary distribution qualification: false.\n")
        shutil.copytree(root, destination)
    print(f"PS5_ADRENOTOOLS_SOURCE PASS directory={destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(
        "_Build/PS5AndroidAdrenoTools_20261002/source"))
    args = parser.parse_args()
    try:
        prepare(args.output)
    except Exception as error:
        parser.exit(1, f"PS5_ADRENOTOOLS_SOURCE FAIL {error}\n")
