# Lumen Emu — core source

Experimental PS4 and PS5 emulator cores and native platform integration by **GS-Soft**, published at [gs-soft-it/lumen-emu](https://github.com/gs-soft-it/lumen-emu). This repository starts from a source snapshot exported on 23 October 2026, without the original project's Git history.

Lumen builds on **[shadPS4](https://github.com/shadps4-emu/shadPS4)**, through [shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), **[Kyty](https://github.com/InoriRus/Kyty)** and **[KytyPS5](https://github.com/KytyPS5/KytyPS5)**. Their copyrights, licences, provenance records and individual source notices remain applicable. Lumen is an independent derivative and is not endorsed by those projects.

## Scope

Included: emulator engines, CPU translation and virtual memory, rendering, native host bridges, build scripts, dependency locks, local patches, tests and licence records. Runtime input, IME and in-game guest dialogs remain part of the core.

The Lumen dashboard, game-library UI, Android Java/resources/Gradle application, Qt desktop frontend, UIKit application and application packaging are excluded. There is no `lumen_dashboard` target. Native Android entry points still need a host application to supply their window, input and lifecycle callbacks.

| Path | Contents |
| --- | --- |
| `source/gen4` | PS4 engine, direct FEX integration and native runtime |
| `source/gen4/gen5` | PS5 engine, compatibility import and FEX integration |
| `source/emulator`, `source/lib`, `source/cpu` | Shared and retained runtime, CPU and host components |
| `android/app/src/main/cpp` | Android native libraries and host interfaces |
| `macos`, `_Build` | Darwin FEX adapters, patches and platform build helpers |
| `source/licenses`, `LICENSES.md`, `NOTICE` | Licences, provenance and dependency records |

[SOURCE_SNAPSHOT.json](SOURCE_SNAPSHOT.json) records the export selection and excluded application files. Some inherited integration notes describe the original application's workflows; this README defines the public snapshot's scope.

## Android download

**[Download the Android APK](https://github.com/gs-soft-it/lumen-emu/releases/download/v0.1.0-spike-fexlab/lumen-emu-release.apk)** ([experimental release 0.1.0-spike-fexlab](https://github.com/gs-soft-it/lumen-emu/releases/tag/v0.1.0-spike-fexlab)). Requires Android 15/API 35 or newer and an ARM64 device (`arm64-v8a`). This project-provided APK is the only application binary offered here. It is separate from this core snapshot: dashboard and application-shell sources are excluded, so the instructions below do not rebuild the complete APK.

## Prepare the source

Use Git, Python 3, CMake 3.25 or newer, Ninja and a suitable native/cross C/C++ toolchain. The Gen4 engine uses C++23; the macOS dependency workflow calls for Python 3.12 or newer. Apple targets require Xcode and its platform SDKs. Android requires an NDK supporting API 35.

```sh
git clone https://github.com/gs-soft-it/lumen-emu.git
cd lumen-emu
python3 tools/prepare_source_dependencies.py
python3 tools/prepare_source_dependencies.py --check
```

On Windows, use `python` if that is your Python command. The bootstrap restores the exact FEX and Sirit revisions and required nested dependencies from [source-dependencies.lock.json](source-dependencies.lock.json), without fetching upstream test corpora. `--check` validates existing pins, origins and clean worktrees without downloads. Existing modified checkouts are preserved and reported, rather than reset.

Gen4 also needs its platform dependency set, prepared below. Its default cache is the sibling directory `../lumen-deps/gen4`; `--cache` changes that location, and `LUMEN_GEN4_DEPS_DIR` must then point to its `sources` directory.

**FFmpeg is a separate build input.** Supply a static package built for the target architecture and ABI, with `include/libavcodec/avcodec.h` and `lib/libavformat.a`, `libavcodec.a`, `libswscale.a`, `libavutil.a`, `libavfilter.a` and `libswresample.a`. The source bootstrap does not build that package. Preserve the exact FFmpeg source, configuration and licence records for your build; see [the recorded FFmpeg source information](source/licenses/ps5/FFmpeg/SOURCE.txt).

## Build entry points

The commands below reflect the checked-in CMake targets and scripts. They are development build recipes, not a claim of successful clean builds on every platform. Replace `/absolute/path/...` with your own toolchain/package paths. Use a separate build directory for each platform, ABI and configuration.

### macOS Apple Silicon

Run from a native arm64 macOS shell. The FEX path requires 16 KiB host pages, a thin arm64 build and the macOS 12.0 deployment baseline. It does not support Rosetta configuration or a universal/x86_64 macOS build. Gen4 uses the native Metal adapter; its child build imports the parent's built Metal archives.

```sh
python3 source/gen4/tools/prepare_dependencies.py --platform macos
cmake -S . -B build/macos -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=macosx \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DLUMEN_BUILD_PS4=ON -DLUMEN_BUILD_PS5=ON \
  -DLUMEN_GEN4_FFMPEG_PREBUILT_DIR=/absolute/path/ffmpeg-macos-arm64 \
  -DLUMEN_PS5_FFMPEG_PREBUILT_DIR=/absolute/path/ffmpeg-macos-arm64
cmake --build build/macos --target lumen_gen4_runtime --parallel 4
cmake --build build/macos --target lumen_ps5_runtime --parallel 4
```

The PS4 executable is `build/macos/gen4/lumen_gen4_runtime`; the full PS5 child build produces `build/macos/ps5-full/kyty_emulator`. The PS5 target uses [build-full-kyty.sh](source/gen4/gen5/tools/build-full-kyty.sh) and verifies its FEX linkage. These are native runtimes, with no Lumen dashboard or app bundle.

### Android ARM64

Configure the native entry point directly, without Gradle. Only `arm64-v8a` and API 35 or newer are accepted. The example selects the direct PS5 FEX path; the PS4 runtime also uses direct FEX. Runtime support currently requires 4 KiB host pages: 16 KiB ELF segment alignment does not establish support for a 16 KiB-page Android device.

```sh
python3 source/gen4/tools/prepare_dependencies.py --platform android
cmake -S android/app/src/main/cpp -B build/android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/absolute/path/android-ndk/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-35 \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLUMEN_ANDROID_FULL_RUNTIME=ON -DLUMEN_ANDROID_FEXCORE_LAB=ON \
  -DLUMEN_PS5_DIRECT_FEX=ON \
  -DFFMPEG_PREBUILT_DIR=/absolute/path/ffmpeg-android-arm64
cmake --build build/android-arm64 \
  --target lumen_android_gen4_build lumen_android_ps5 --parallel 4
```

The PS4 library is staged at `build/android-arm64/gen4-stage/arm64-v8a/liblumen_android_gen4.so`; the PS5 target produces `liblumen_android_ps5.so` in its CMake output tree. Additional JNI glue is available as `lumen_android_spike`. These libraries alone are not an Android application. Optional AdrenoTools support needs the separately prepared, verified source closure described by [prepare-ps5-adrenotools.py](android/scripts/prepare-ps5-adrenotools.py).

### iOS ARM64: CPU/VM components only

The retained iOS entry point builds standalone CPU and virtual-memory components. It does **not** build a complete PS4/PS5 iOS emulator or an IPA. `LUMEN_WITH_IOS_RUNTIME=ON` is explicitly rejected because the required application shell is excluded; the PS5 FEX embedding supports macOS/Android ARM64 only.

On macOS with the iPhoneOS SDK:

```sh
cmake -S source -B build/ios-core -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
  -DLUMEN_WITH_IOS_RUNTIME=OFF
cmake --build build/ios-core \
  --target lumen_x64_cpu_backend lumen_apple_virtual_memory --parallel 4
```

`lumen_ios_bootstrap` is an additional synthetic executable probe, not a game compatibility test or a replacement for the omitted window/input integration.

### Windows and Linux

The source retains Windows x64 and Linux code and legacy build helpers in `_Build`. Windows has a pinned Gen4 dependency profile. A PS4-only Gen4 build can be configured from a developer shell with a C++23 compiler and Ninja:

```sh
python source/gen4/tools/prepare_dependencies.py --platform windows
cmake -S source/gen4 -B build/windows-gen4 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLUMEN_GEN4_FFMPEG_PREBUILT_DIR=C:/absolute/path/ffmpeg-windows-x64
cmake --build build/windows-gen4 --target lumen_gen4_runtime --parallel 4
```

The example uses POSIX line continuation; in PowerShell put the configure command on one line. Supply a Windows FFmpeg package compatible with the selected compiler and the archive layout above. No fresh Windows build is certified by this snapshot. Linux has no dedicated profile in the Gen4 dependency preparer, and the active Gen4 ARM64 implementation accepts only macOS and Android. The presence of inherited Linux/Windows scripts does not establish a complete supported PS5 build or equivalent platform compatibility.

## Validation and limitations

The export record reports CMake parsing, the native Metal build graph, source-manifest paths and dependency-bootstrap checks. It explicitly records that a full compilation and game execution were **not** performed for the export. Platform support, runtime correctness and individual game compatibility need separate validation on the target hardware.

[CONTENTS.sha256](CONTENTS.sha256) records exported file bytes. Check it with `sha256sum -c CONTENTS.sha256` where available, or `shasum -a 256 -c CONTENTS.sha256` on macOS. This is an integrity check, not a build test; line-ending conversion or local edits can change hashes. The separately downloaded sources are checked by the dependency bootstrap.

## Licences and provenance

Read [LICENSE](LICENSE), [LICENSES.md](LICENSES.md) and [NOTICE](NOTICE), along with the per-file notices and dependency records. Original Lumen integration is GPL-2.0-only unless a file says otherwise. This does not replace inherited MIT, GPL-2.0-or-later or other component terms.

The existing licence inventory identifies unresolved combined-binary release issues, including PS5 SPIRV-Tools/Apache-2.0 with GPLv2-only components, Gen4 FDK AAC with GPL components, and the exact static LGPL dependency closure. Publishing this source snapshot does not resolve those issues or qualify a binary release. Preserve corresponding sources, local modifications, compilation scripts and any required relinking materials when distributing covered binaries. Some retained notice documents describe the pre-publication state; this repository is the public core snapshot.
