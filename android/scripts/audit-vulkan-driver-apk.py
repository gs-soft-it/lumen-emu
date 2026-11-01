#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify the pinned private Vulkan drivers and their packaged notice bytes."""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
RECIPE = ROOT / "android/gpu-drivers/turnip-mojo-bionic"
DRIVER = "assets/GpuDrivers/turnip-mojo-bionic/libvulkan_freedreno.so"
LEGAL = "assets/Legal/Vulkan/turnip-mojo-bionic/"
PANVK_RECIPE = ROOT / "android/gpu-drivers/panvk"
PANVK_DRIVER = "assets/GpuDrivers/panvk/libvulkan_panfrost.so"
PANVK_LEGAL = "assets/Legal/Vulkan/panvk/"
BANNERS_RECIPE = ROOT / "android/gpu-drivers/turnip-banners"
BANNERS_DRIVER = "assets/GpuDrivers/turnip-banners/libvulkan_freedreno.so"
BANNERS_LEGAL = "assets/Legal/Vulkan/turnip-banners/"
HOOKS = {"lib/arm64-v8a/libmain_hook.so", "lib/arm64-v8a/libhook_impl.so"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def check(condition, message):
    if not condition:
        raise ValueError(message)


def inspect(apk):
    lock = json.loads((RECIPE / "source-lock.json").read_text())
    manifest = json.loads((RECIPE / "package-notices.json").read_text())
    check(manifest["schema"] == 1, "Invalid Mojo/Bionic package notice manifest")
    records = dict(manifest["files"])
    for name in ("source-lock.json", "package-notices.json", "NOTICE.txt", "LICENSE-APACHE-2.0.txt"):
        records[name] = sha((RECIPE / name).read_bytes())
    panvk_lock_bytes = (PANVK_RECIPE / "source-lock.json").read_bytes()
    panvk_lock = json.loads(panvk_lock_bytes)
    check(panvk_lock["schema"] == 1, "Invalid PanVK source lock")
    check(set(panvk_lock["files"]) == {"libvulkan_panfrost.so", "NOTICE.txt", "meta.json"},
          "Unexpected PanVK source lock file set")
    panvk_records = {name: panvk_lock["files"][name] for name in ("NOTICE.txt", "meta.json")}
    panvk_records["source-lock.json"] = {"sha256": sha(panvk_lock_bytes), "bytes": len(panvk_lock_bytes)}
    banners_lock_bytes = (BANNERS_RECIPE / "source-lock.json").read_bytes()
    banners_lock = json.loads(banners_lock_bytes)
    check(banners_lock["schema"] == 1, "Invalid Banners source lock")
    check(set(banners_lock["files"]) == {"libvulkan_freedreno.so", "meta.json"},
          "Unexpected Banners source lock file set")
    banners_records = dict(banners_lock["legal_files"])
    banners_records["meta.json"] = banners_lock["files"]["meta.json"]
    banners_records["source-lock.json"] = {"sha256": sha(banners_lock_bytes), "bytes": len(banners_lock_bytes)}
    with zipfile.ZipFile(apk) as archive:
        counts = Counter(archive.namelist())
        packaged = {name for name in counts if name.startswith(("assets/GpuDrivers/", LEGAL, PANVK_LEGAL, BANNERS_LEGAL))}
        if not packaged and not HOOKS.intersection(counts):
            return {}
        expected = {DRIVER, PANVK_DRIVER, BANNERS_DRIVER, *(LEGAL + name for name in records),
                    *(PANVK_LEGAL + name for name in panvk_records),
                    *(BANNERS_LEGAL + name for name in banners_records)}
        check(packaged == expected, "Unexpected private Vulkan driver or notice asset set")
        check(HOOKS.issubset(counts), "Packaged Vulkan drivers require both private loader hooks")
        check(all(counts[name] == 1 for name in expected | HOOKS), "Duplicate private Vulkan driver asset or hook")
        driver = archive.read(DRIVER)
        check(len(driver) == lock["accepted_driver"]["bytes"]
              and sha(driver) == lock["accepted_driver"]["sha256"],
              "Packaged Mojo/Bionic driver differs from the accepted baseline")
        for name, expected_hash in records.items():
            check(sha(archive.read(LEGAL + name)) == expected_hash,
                  f"Packaged Mojo/Bionic notice SHA256 mismatch: {name}")
        panvk_driver = archive.read(PANVK_DRIVER)
        panvk_pin = panvk_lock["files"]["libvulkan_panfrost.so"]
        check(len(panvk_driver) == panvk_pin["bytes"] and sha(panvk_driver) == panvk_pin["sha256"],
              "Packaged PanVK driver differs from the pinned source lock")
        for name, pin in panvk_records.items():
            data = archive.read(PANVK_LEGAL + name)
            check(len(data) == pin["bytes"] and sha(data) == pin["sha256"],
                  f"Packaged PanVK notice differs from the pinned source lock: {name}")
        banners_driver = archive.read(BANNERS_DRIVER)
        banners_pin = banners_lock["files"]["libvulkan_freedreno.so"]
        check(len(banners_driver) == banners_pin["bytes"] and sha(banners_driver) == banners_pin["sha256"],
              "Packaged Banners driver differs from the accepted A8xx baseline")
        for name, pin in banners_records.items():
            data = archive.read(BANNERS_LEGAL + name)
            check(len(data) == pin["bytes"] and sha(data) == pin["sha256"],
                  f"Packaged Banners notice differs from the pinned source lock: {name}")
    return {**{LEGAL + name: digest for name, digest in records.items()},
            **{PANVK_LEGAL + name: pin["sha256"] for name, pin in panvk_records.items()},
            **{BANNERS_LEGAL + name: pin["sha256"] for name, pin in banners_records.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--legal-records", action="store_true")
    args = parser.parse_args()
    records = inspect(args.apk)
    if args.legal_records:
        for path, digest in sorted(records.items()):
            print(f"{path}|{digest}")
    else:
        print("VULKAN_DRIVER_APK PASS "
              + (f"drivers=mojo-bionic,banners,panvk notices={len(records)}" if records else "driver=system-only"))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        print(f"VULKAN_DRIVER_APK FAIL: {error}", file=sys.stderr)
        sys.exit(1)
