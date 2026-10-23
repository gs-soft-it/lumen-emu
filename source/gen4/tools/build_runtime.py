#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build the PS4 runtime using an already built common CPU/host closure."""
import argparse
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--parent', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--deps', type=Path)
    parser.add_argument('--ffmpeg', default='')
    parser.add_argument('--stage', type=Path)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--configuration', default='',
                        choices=('', 'Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel'))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    repository = Path(__file__).resolve().parents[3]
    parent = args.parent.resolve()
    cache = {}
    for line in (parent / 'CMakeCache.txt').read_text().splitlines():
        if not line.startswith(('#', '//')) and '=' in line and ':' in line.split('=', 1)[0]:
            key, value = line.split('=', 1)
            cache[key.split(':', 1)[0]] = value
    android = cache.get('CMAKE_SYSTEM_NAME') == 'Android' or bool(cache.get('ANDROID_NDK'))
    cmake = os.environ.get('LUMEN_GEN4_CMAKE') or shutil.which('cmake')
    if not cmake:
        parser.error('CMake 3.25 or later is required for the PS4 dependencies')
    capabilities = json.loads(subprocess.check_output([cmake, '-E', 'capabilities'], text=True))
    if (capabilities['version']['major'], capabilities['version']['minor']) < (3, 25):
        parser.error('Set LUMEN_GEN4_CMAKE to CMake 3.25 or later')
    deps = (args.deps or repository.parent / 'lumen-deps/gen4/sources').resolve()
    if not (deps / 'vulkan-headers/include/vulkan/vulkan_core.h').exists():
        parser.error('Run source/gen4/tools/prepare_dependencies.py before building')
    build = args.build.resolve()
    command = [cmake, '-S', str(repository / 'source/gen4'), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=' + (args.configuration or cache.get('CMAKE_BUILD_TYPE', 'Release')),
        '-DLUMEN_GEN4_PARENT_BUILD=' + str(parent), '-DLUMEN_GEN4_DEPS_DIR=' + str(deps),
        '-DLUMEN_GEN4_FFMPEG_PREBUILT_DIR=' + (args.ffmpeg or cache.get('FFMPEG_PREBUILT_DIR', ''))]
    if android:
        for key in ('CMAKE_TOOLCHAIN_FILE', 'ANDROID_NDK', 'ANDROID_ABI', 'ANDROID_PLATFORM',
                    'CMAKE_MAKE_PROGRAM', 'ANDROID_STL', 'LUMEN_PS5_ADRENOTOOLS_SOURCE_DIR', 'LUMEN_QEMU_TCG_LIBRARY'):
            if cache.get(key): command.append('-D' + key + '=' + cache[key])
        command.append('-DLUMEN_GEN4_ENTRY_SOURCES=' + ';'.join(str(repository / 'source/gen4/src/lumen' / name)
            for name in ('android_main.cpp', 'android_ui.cpp')))
    elif platform.system() == 'Darwin':
        command += ['-DCMAKE_OSX_ARCHITECTURES=arm64', '-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0',
                    '-DCMAKE_AR=' + str(repository / 'source/tools/macos-ar.sh')]
    subprocess.run(command, check=True)
    subprocess.run([cmake, '--build', str(build), '--target', 'lumen_gen4_runtime', '--parallel', str(args.jobs)], check=True)
    output = build / ('liblumen_android_gen4.so' if android else 'lumen_gen4_runtime' + ('.exe' if platform.system() == 'Windows' else ''))
    if not output.is_file(): raise RuntimeError('PS4 runtime output missing: ' + str(output))
    if args.stage:
        destination = args.stage.resolve() / ('arm64-v8a' if android else '')
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output, destination / output.name)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
