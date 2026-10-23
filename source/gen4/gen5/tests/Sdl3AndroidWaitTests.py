#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify the guarded Android SDL copy and its actual production wait bodies."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[4]
SDL = ROOT / 'source/gen4/gen5/compatibility/3rdparty/SDL3'
HELPER = ROOT / 'source/gen4/gen5/cmake/ApplyAndroidSdl3WaitPatch.cmake'
PIN = 'fa2c02bb6e21974a89ea9824bc53c9932abe5f9c'
SOURCE_FILES = {
    'events': ('src/events/SDL_events.c', 'ef6301f9416c6960c5f37fc9d8ed38a5624fbd246fa4da0ac02a8351bcb172d7'),
    'core': ('src/core/android/SDL_android.c', '4fc5381b992c95e437e4485a79d3d54e3e92a31629e0c02f2f950b03707631de'),
    'android': ('src/video/android/SDL_androidevents.c', '4f098fb93060186756d7f2c0f999ce13f3dcb849a0316ad2fea59f4859d12b61'),
}
FUNCTIONS = {
    'events': [
        'SDL_AddEvent', 'SDL_CutEvent', 'SDL_SendWakeupEvent',
        'SDL_PeepEventsInternal', 'SDL_PeepEvents',
        'SDL_CreateMainThreadCallback', 'SDL_DestroyMainThreadCallback',
        'SDL_RunMainThreadCallbacks', 'SDL_RunOnMainThread',
        'SDL_PumpEventsInternal', 'SDL_WaitEventTimeoutNS',
        'SDL_WaitEvent', 'SDL_WaitEventTimeout', 'SDL_PollEvent', 'SDL_PushEvent',
    ],
    'core': ['FindLifecycleEvent', 'RemoveLifecycleEvent',
             'Android_SendLifecycleEvent', 'Android_WaitLifecycleEvent'],
    'android': ['Android_HandleLifecycleEvent', 'GetLifecycleEventTimeout',
                'Android_PumpEvents'],
}
PATCHED_SHA = '1f47de40fd3d9cdc7bc224a74b57bc782bb8ae76148338f02c9130ef6790e5c6'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract(source, name):
    pattern = r'(?m)^(?:static )?[^\n;{}]*\b' + name + r'\([^;{}]*?\)\n\{'
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        raise RuntimeError(f'unexpected production function count: {name}')
    start = matches[0].start()
    pos = matches[0].end() - 1
    depth = 0
    # The pinned selected function bodies have no braces in comments/strings.
    while pos < len(source):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if not depth:
                return source[start:pos + 1] + '\n'
        pos += 1
    raise RuntimeError(f'unterminated production function: {name}')


def cmake_quote(path):
    value = str(path).replace('\\', '\\\\').replace('"', '\\"')
    return '"' + value + '"'


def prepare_case(work, name):
    case = work / name
    original = case / 'original'
    generated = case / 'generated'
    for directory in (original, generated):
        (directory / 'src/events').mkdir(parents=True)
        shutil.copy2(SDL / 'src/events/SDL_events.c', directory / 'src/events/SDL_events.c')
    shutil.copy2(SDL / 'CMakeLists.txt', original / 'CMakeLists.txt')
    manifest = case / 'UPSTREAM.json'
    shutil.copy2(ROOT / 'source/gen4/gen5/UPSTREAM.json', manifest)
    return case, original, generated, manifest


def run_adapter(cmake, case, original, generated, manifest, expected_error=None):
    script = case / 'apply.cmake'
    script.write_text('cmake_minimum_required(VERSION 3.22)\nset(ANDROID TRUE)\n'
                      f'include({cmake_quote(HELPER)})\n'
                      'lumen_ps5_patch_android_sdl_wait('
                      f'{cmake_quote(original)} {cmake_quote(generated)} {cmake_quote(manifest)})\n')
    result = subprocess.run([cmake, '-P', str(script)], capture_output=True, text=True, timeout=15)
    text = result.stdout + result.stderr
    (case / ('negative.log' if expected_error else 'apply.log')).write_text(text)
    if expected_error:
        if result.returncode == 0 or expected_error not in text:
            raise RuntimeError(f'{case.name}: expected rejection {expected_error!r}, got {text}')
    elif result.returncode != 0:
        raise RuntimeError(f'{case.name}: adapter failed: {text}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--cmake', default='cmake')
    args = parser.parse_args()
    work = args.work_dir.resolve()
    if work.exists():
        parser.error('use a fresh work directory; existing evidence is preserved')
    work.mkdir(parents=True)
    hashes = {}
    for relative, expected in SOURCE_FILES.values():
        actual = digest(SDL / relative)
        if actual != expected:
            raise RuntimeError(f'pinned SDL source hash changed: {relative}')
        hashes[relative] = actual
    positive = prepare_case(work, 'positive')
    run_adapter(args.cmake, *positive)
    output = positive[2] / 'src/events/SDL_events.c'
    if digest(output) != PATCHED_SHA:
        raise RuntimeError('actual CMake-generated body differs from reviewed proposal')
    run_adapter(args.cmake, *positive)
    if digest(output) != PATCHED_SHA:
        raise RuntimeError('adapter re-entry changed its reviewed output')
    negative_cases = []
    for name, failure in [
        ('wrong-pin', 'upstream pin mismatch'),
        ('missing-pin', 'exactly one upstream SDL3 pin'),
        ('duplicate-pin', 'exactly one upstream SDL3 pin'),
        ('wrong-release', 'release tag/version mismatch'),
        ('changed-source', 'upstream events SHA-256 mismatch'),
        ('changed-copy', 'generated copy differs from reviewed upstream'),
        ('vendored-output', 'must not modify the vendored source'),
        ('symlink-copy', 'requires regular reviewed input files'),
    ]:
        case, original, generated, manifest = prepare_case(work, name)
        if name in ('wrong-pin', 'missing-pin', 'duplicate-pin'):
            data = json.loads(manifest.read_text())
            sdl = next(p for p in data['submodules'] if p['path'] == '3rdparty/SDL3')
            if name == 'wrong-pin':
                sdl['commit'] = '0' * 40
            elif name == 'missing-pin':
                data['submodules'].remove(sdl)
            else:
                data['submodules'].append(dict(sdl))
            manifest.write_text(json.dumps(data))
        elif name == 'wrong-release':
            path = original / 'CMakeLists.txt'
            path.write_text(path.read_text().replace('VERSION "3.4.16"', 'VERSION "0.0.0"'))
        elif name == 'changed-source':
            path = original / 'src/events/SDL_events.c'
            path.write_bytes(path.read_bytes() + b'\n')
        elif name == 'changed-copy':
            path = generated / 'src/events/SDL_events.c'
            path.write_bytes(path.read_bytes() + b'\n')
        elif name == 'vendored-output':
            generated = original
        elif name == 'symlink-copy':
            path = generated / 'src/events/SDL_events.c'
            path.unlink()
            path.symlink_to(original / 'src/events/SDL_events.c')
        before = digest(original / 'src/events/SDL_events.c')
        run_adapter(args.cmake, case, original, generated, manifest, failure)
        if digest(original / 'src/events/SDL_events.c') != before:
            raise RuntimeError(f'{name}: negative adapter changed original source')
        negative_cases.append(name)
    bodies_sha = {}
    upstream_notice = (SDL / 'src/events/SDL_events.c').read_text().split('*/', 1)[0] + '*/\n'
    for variant in ('baseline', 'proposed'):
        bodies = []
        for key, names in FUNCTIONS.items():
            source = (output.read_text() if key == 'events' and variant == 'proposed'
                      else (SDL / SOURCE_FILES[key][0]).read_text())
            bodies.extend(extract(source, name) for name in names)
        body = '\n'.join(bodies)
        bodies_sha[variant] = hashlib.sha256(body.encode()).hexdigest()
        (work / f'production-{variant}.inc').write_text(upstream_notice + body)
    fixture = Path(__file__).with_name('Sdl3AndroidWaitFixture.cpp')
    shutil.copy2(fixture, work / 'Fixture.cpp')
    commands = []
    for variant in ('baseline', 'proposed'):
        compiler = [args.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-parameter', '-pthread']
        if variant == 'baseline':
            compiler.append('-DTEST_BASELINE')
        compiler.extend(['Fixture.cpp', '-o', f'fixture-{variant}'])
        result = subprocess.run(compiler, cwd=work, capture_output=True, text=True, timeout=30)
        (work / f'{variant}-build.log').write_text(' '.join(compiler) + '\n' + result.stdout + result.stderr)
        result.check_returncode()
        commands.append(compiler)
        result = subprocess.run([str(work / f'fixture-{variant}')], capture_output=True, text=True, timeout=3)
        (work / f'{variant}-run.log').write_text(result.stdout + result.stderr)
        result.check_returncode()
        print(variant + ':\n' + result.stdout, end='')
    if any(digest(SDL / relative) != expected for relative, expected in SOURCE_FILES.values()):
        raise RuntimeError('vendored source changed during fixture run')
    receipt = {'schema': 'LUMEN_SDL3_ANDROID_WAIT_FIXTURE_V1', 'upstream_commit': PIN,
               'source_sha256': hashes, 'generated_events_sha256': digest(output),
               'production_bodies_sha256': bodies_sha, 'functions': FUNCTIONS,
               'negative_guards_passed': negative_cases, 'idempotent_reentry': True,
               'vendored_unchanged': True, 'compiler_commands': commands,
               'scope': 'host synchronization/queue fixture; APK/device, input, performance and black-screen unqualified',
               'shim_limits': ['host synchronization replaces SDL platform primitives',
                               'memory, filter, maintenance and lifecycle effect bodies are stubs']}
    (work / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('adapter positive/re-entry and eight rejection guards: PASS')


if __name__ == '__main__':
    main()
