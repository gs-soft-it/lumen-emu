<p align="center">
  <img src="https://lumen-emu.com/assets/lumen-icon.webp" width="144" height="144" alt="Lumen Emu icon">
</p>

<h1 align="center">Lumen Emu</h1>

<p align="center"><strong>Console worlds. New horizons.</strong></p>

Experimental **PS4 and PS5 emulation for Android**, with **macOS (Apple Silicon) and iOS versions in development**, by **GS-Soft**. This page presents the full Lumen Emu application, including its dashboard, alongside the public emulator-core sources.

<p align="center">
  <a href="https://lumen-emu.com"><strong>Official website</strong></a> ·
  <a href="https://github.com/gs-soft-it/lumen-emu/releases/download/v0.1.1-spike-fexlab/lumenemu-0.1.1-spike-fexlab-v2.apk"><strong>Download Android APK</strong></a> ·
  <a href="https://lumen-emu.com/support/"><strong>Support development</strong></a> ·
  <a href="#compatibility"><strong>Compatibility list</strong></a> ·
  <a href="#prepare-the-source"><strong>Build the cores</strong></a>
</p>

Lumen builds on **[shadPS4](https://github.com/shadps4-emu/shadPS4)**, through [shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), **[Kyty](https://github.com/InoriRus/Kyty)** and **[KytyPS5](https://github.com/KytyPS5/KytyPS5)**. Their copyrights, licences, provenance records and individual source notices remain applicable. Lumen is an independent derivative and is not endorsed by those projects.

## The application and dashboard

Lumen Emu brings PS4 and PS5 software into one Android library. Its dashboard provides the application interface, while the two generations run in separate runtime processes. The application is free, with no ads or in-app purchases; contributions to development are voluntary.

| Feature | Application experience |
| --- | --- |
| Dashboard and library | Browse prepared PS4 and PS5 content in one Android interface. |
| Touch controls | Shared controls with one virtual joystick, an opaque menu and a centred 16:9 game surface. |
| Physical controllers | Compatible Bluetooth and USB-C gamepads; DualSense and Xbox Series X\|S controllers have been tested by the developer. |
| Graphics drivers | A custom Turnip Mojo/Bionic path for compatible Snapdragon/Adreno devices, with a selectable system Vulkan driver. Non-Snapdragon or unidentified chipsets automatically use the system driver. |
| Included benchmark | SUCCI, a homebrew demo benchmark, helps assess the phone's performance. |

Development currently focuses on Snapdragon smartphones because available test hardware is limited. The system-driver path is reported to be slower; performance depends on the device, driver, game and emulator build. A custom-driver loading failure stops startup rather than silently selecting another driver.

**Mac (Apple Silicon) and iOS application versions are coming soon.** No Mac or iOS application binary is offered by this repository. Its iOS source build currently exposes CPU/VM components, as described in the build instructions below.

### App preview

<p align="center">
  <a href="https://lumen-emu.com/index.html#preview"><img src="https://lumen-emu.com/assets/screenshots/lumen-library.webp?v=20261023-realme" width="960" alt="Lumen Emu Android dashboard showing the PS4 and PS5 library"></a>
</p>

*Dashboard captured on a realme GT 8 Pro 5G (RMX5210), Android 16, on 23 October 2026. These images show the full application; its dashboard sources are not included in the public core snapshot.*

<details>
<summary>View the Astro Bot captures</summary>

![Astro Bot title screen, with 8.0 FPS visible](https://lumen-emu.com/assets/screenshots/astro-bot-title.webp)

*Title screen: 8.0 FPS visible.*

![Astro Bot opening sequence, with 2.0 FPS visible](https://lumen-emu.com/assets/screenshots/astro-bot-opening.webp)

*Opening sequence: 2.0 FPS visible.*

![Astro Bot close-up, with 2.0 FPS visible](https://lumen-emu.com/assets/screenshots/astro-bot-closeup.webp)

*All captures were taken on the same phone on 23 October 2026. Individual frames are not an average FPS measurement or evidence of normal playability.*

</details>

### SUCCI benchmark

The Android package includes **SUCCI** to test the phone's performance. Its indicative thresholds are **35 FPS or higher: playable**, **20–34 FPS: almost playable**, and **below 20 FPS: boots only**. These thresholds apply to the benchmark and do not predict the same performance in other games.

### Content and hardware

The application reads **already-extracted ROMs**; it does not open, extract or install PKG files. Prepare content you are legally authorised to use before selecting its folder. No commercial games, firmware, BIOS files or keys are provided. See the [content requirements and responsible-use information](https://lumen-emu.com/legal/).

Use a suitably powerful device with adequate cooling, and stop if it becomes excessively hot or shows a temperature warning. Android gaming consoles have not yet been tested. See the [hardware guidance](https://lumen-emu.com/legal/#hardware-safety).

## Compatibility

**8 reported games · 4 playable · 2 almost playable · 2 boots only**

[Open the full compatibility list and report form](https://lumen-emu.com/compatibility/).

These are developer-reported application results, updated **23 October 2026**, on a **realme GT 8 Pro 5G, Snapdragon 8 Elite Gen 5, 16 GB RAM / 512 GB storage, Android 16**.

| Game | Platform | Tested version | Title ID | Reported status |
| --- | --- | --- | --- | --- |
| Astro Bot | PS5 | `01.018.000` | `PPSA21564` | 🔴 Boots only |
| Bloodborne | PS4 | `01.00` | `CUSA03173` | 🟡 Almost playable |
| Cat Quest III | PS5 | `01.000.001` | `PPSA15065` | 🟡 Almost playable |
| Dreaming Sarah | PS5 | `01.000.000` | `PPSA02929` | 🟢 Playable |
| Minecraft | PS5 | `01.044.000` | `PPSA17221` | 🔴 Boots only |
| Minecraft: PlayStation 4 Edition | PS4 | `01.00` | `CUSA00265` | 🟢 Playable |
| Super Meat Boy | PS4 | `01.02` | `CUSA03454` | 🟢 Playable |
| Worms W.M.D. | PS4 | `01.11` | `CUSA04022` | 🟢 Playable |

- **Playable:** reported playable in the developer's test; issues may remain and completion is not guaranteed.
- **Almost playable:** runs with noticeable slowdown.
- **Boots only:** starts, but severe slowdown prevents normal play.

The initial reports did not record measured FPS, completion coverage or the exact emulator build. They are not independent certification and are not attributed specifically to the downloadable APK or a build of this source snapshot. Results can change with device, driver, game version, settings and emulator build. The SUCCI thresholds are separate from these qualitative game reports.

To contribute a report, use the [website report form](https://lumen-emu.com/compatibility/#report) or email [info@lumen-emu.com](mailto:info@lumen-emu.com). Include the game and exact version, device and Android version, emulator build, observed behaviour and reproduction steps. Screenshots and crash logs are useful; remove personal information and do not send game files or keys.

## Support development

**[Contribute on Ko-fi](https://ko-fi.com/M4J429BFNH)** · **[Official support page](https://lumen-emu.com/support/)**

Voluntary contributions help fund development and additional test smartphones, so we can investigate chipset-specific issues and improve compatibility beyond the current Snapdragon focus. Useful compatibility reports are another way to help.

A contribution does not guarantee a requested feature, game or device compatibility, priority or a delivery date. Contributions support ongoing development; they do not purchase games or access to copyrighted content.

## Public source scope

The emulator-core sources are public at [gs-soft-it/lumen-emu](https://github.com/gs-soft-it/lumen-emu), starting from a snapshot exported on 23 October 2026 without the original project's Git history. **Documenting and showing the full application above does not include its dashboard or application-shell sources in this repository.**

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

**Latest update — 0.1.1-spike-fexlab (version code 2):** fixes a PS5 startup failure on Adreno 730 devices by enabling Vulkan `shaderBufferInt64Atomics` only when supported, checks shader requirements before use and improves fatal-error diagnostics. See the [changelog](CHANGELOG.md) for details and the limits of the reported device tests.

**[Download the Android APK](https://github.com/gs-soft-it/lumen-emu/releases/download/v0.1.1-spike-fexlab/lumenemu-0.1.1-spike-fexlab-v2.apk)** ([experimental release 0.1.1-spike-fexlab](https://github.com/gs-soft-it/lumen-emu/releases/tag/v0.1.1-spike-fexlab)). Requires Android 15/API 35 or newer and an ARM64 device (`arm64-v8a`). This project-provided APK is the only application binary offered here. It is separate from this core snapshot: dashboard and application-shell sources are excluded, so the instructions below do not rebuild the complete APK.

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
