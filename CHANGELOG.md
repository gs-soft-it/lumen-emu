# Changelog

## iOS and iPadOS preview 0.1.0 build 5

9 November 2026 · [IPA and corresponding source](https://github.com/gs-soft-it/lumen-emu/releases/tag/v0.1.0-ios-build5)

- The PS4 core uses FFmpeg for AAC and Mbed TLS for HTTPS on iOS. HTTPS verifies certificate trust and hostnames through Apple Security.
- The PS5 iOS core reads SPIR-V operands from tables generated from the MIT-licensed Khronos grammar.
- The on-device JIT helper uses a source-built StikJIT framework. The corresponding-source archive records the `idevice` revision, Rust dependency lock and build recipe.
- The matching Simulator build installed and passed FEX JIT probes for PS4 and PS5. The PS4 Succi demo rendered its title screen.

Build 5 is an experimental ARM64 sideload preview with a minimum deployment version of iOS/iPadOS 17.4. Physical AltStore Classic installation and JIT after re-signing remain unverified for this build.

## iOS / iPadOS 0.1.0 (build 3) — development preview

9 November 2026 · [iOS requirements](README.md#ios-and-ipados)

- Experimental ARM64 iOS/iPadOS app with FEX JIT and native Metal for PS4 and PS5. The minimum deployment version is iOS/iPadOS 17.4.
- Updated PS4 and PS5 runtime sources, including Metal shader and tessellation fixes, FEX memory handling, and PS4 audio and world-creation fixes.
- **Playable in developer testing on iPhone 15 Pro** — Super Meat Boy (PS4), Brotato (PS4) and Dreaming Sarah (PS5).
- Other titles are pending compatibility testing. 3D game compatibility and performance need further fixes.

These playability reports come from development builds on the developer's device. The integrated JIT path was exercised with development signing on iOS 27.2.

Build 3 was kept local while the licence compatibility issues recorded in [LICENSES.md](LICENSES.md) were reviewed.

## 0.1.7-spike-fexlab — Android version code 10

4 November 2026 · [Release and APK](https://github.com/gs-soft-it/lumen-emu/releases/tag/v0.1.7-spike-fexlab)

### Highlights

- Quick folder browsing and direct game-file access in the GitHub APK, using Android’s all-files permission. Games remain in the selected folder without being copied into private app storage.
- Preserve the active folder from earlier direct-access releases, plus existing settings and saves. GitHub skips the Play channel’s one-time folder-selection reset.
- Improved NGS2 audio streaming and callbacks, including the PCM path that restored audible Brotato audio in the documented Android development test.
- Current cumulative PS4/PS5 core updates, an English dashboard and an expanded compatibility catalog.
- README compatibility reports updated with the latest device-specific results, including developer-reported West of Dead version 01.00 on realme GT 8 Pro. These reports retain their original device and tested-build context.

### Library access and distribution channels

The GitHub APK offers a fast filesystem browser for the folder containing extracted games. Grant Android’s all-files access when requested; the selected folder becomes the active library and is read directly. Existing direct-filesystem selections and supported legacy document bindings remain valid when available. Cancelling selection preserves the previous library, and pending transfers must be resolved before another library change.

The Google Play package uses a separate storage workflow: select missing games, copy or move them into private app storage, and optionally verify their contents or remove transferred source files. Both options start disabled. That workflow includes transfer recovery and explicit cancellation. The shared core and bundled assets are identical across the two channels; the GitHub release asset is the direct-access APK, while the Play AAB is prepared separately.

### Audio and cumulative core changes

The NGS2 mixer now handles the supported streamed PCM formats and render layouts, processes buffer-completion callbacks through the guest runtime, and accepts the common four-byte voice-state query as well as the full state structure. These are general runtime corrections rather than title-specific patches. The documented Brotato test confirmed audible output; it is not a claim of complete audio fidelity or compatibility for every game.

Six selected KytyPS5 changes improve shader intermediate-representation handling, resource tracking, shader identity and interval merging. Existing Lumen defaults, validation boundaries and ARM64/Metal adaptations are retained. The cumulative update also includes controller input-delivery fixes, instruction-qualified sampler comparison and restoration of FEX architectural state after signal callbacks. No new FPS or startup-speed measurement is attributed to this release.

Mojo/Bionic, Turnip Banners and JimVulkan PanVK remain bundled with their pinned provenance and licence notices. Existing automatic defaults and saved explicit driver choices are preserved; Mali support remains experimental.

The public repository contains the refreshed native core, relevant tests and provenance records described by `SOURCE_SNAPSHOT.json`. Existing public build adaptations and licence notices are preserved. Dashboard and Android application-shell sources remain outside this core repository; their changes are included in the APK.

### Android package

- APK: `lumenemu-0.1.7-spike-fexlab-v10-github.apk`
- Package: `it.gssoft.lumenemu`
- Version: `0.1.7-spike-fexlab` (version code `10`)
- Android 13 (API 33) or newer, ARM64 (`arm64-v8a`); target API 36.
- Non-debuggable FEX Release build, signed with the existing GitHub publication certificate.
- Same package and signing certificate, and a higher version code, than public 0.1.5/build 6 and 0.1.6/build 7. This identity check does not establish upgrade compatibility with differently signed development or Google Play installations.

APK SHA-256:

```text
0dc2aa834c03458a63474b8713717386e2a30bc3c4042edb52a17e2263da5e15
```

### Verification and limits

Both signed APK variants passed native PS4/PS5 audits, package identity, signing, permission and 16 KiB alignment checks. The Play AAB also passed bundletool and signature checks. All three artifacts contain the same six native libraries and 195 assets, including the pinned graphics drivers, licence notices and SUCCI demo. Compiled storage-policy flags and manifests were checked for each channel.

Each channel passed 84 JVM tests, with three existing skips, and the Release `lintVital` packaging gate. Full lint reports 54 pre-existing errors and is not a passing gate. Instrumentation tests compiled for both Release channels, including the new upgrade checks, but were not executed on a device during this preparation. The exact final packages have no new device installation, gameplay or upgrade trial. Binary alignment alone does not establish runtime compatibility with every 16 KiB-page device.

A static diagnostics audit found no bundled debugger, diagnostic wrapper, Vulkan validation layer or malloc-debug tool. Heavy tracing and telemetry are disabled. Optional diagnostic implementations, including NGS2 audio tracing, remain compiled but disabled by default; their Android debug extras are ignored by the Release app. Normal warning/error logs, launch/preflight support and the SUCCI demo are retained. The Play bundle’s native-symbol metadata is passive crash-analysis data.

## 0.1.6-spike-fexlab — Android version code 7

1 November 2026 · [Release and APK](https://github.com/gs-soft-it/lumen-emu/releases/tag/v0.1.6-spike-fexlab)

### Highlights

- Fixed a bug with the controller.
- **Massive loading speedup, especially on PS5:** restored direct filesystem access to local ROM folders, shared by PS4 and PS5. The tested PS5 startup scenario showed substantially shorter initial loading delays.
- **Preliminary PanVK support for compatible Mali GPUs:** the Android package includes JimVulkan PanVK r8 and selects it automatically on detected compatible Mali devices. The system driver remains available as a manual option.
- **Bundled, selectable Turnip Banners:** the recommended configuration on supported Snapdragon/Adreno 8xx devices uses Mojo/Bionic for PS4 and Banners for PS5. Other Snapdragon devices retain Mojo/Bionic. Saved explicit driver choices are preserved.
- **Android 13 support for the FEX package:** the minimum is now API 33 on ARM64, with target API 36. Builds containing the pinned QEMU backend still require API 35 or newer.

### Loading and ROM folders

Local ROM folders now use direct filesystem reads after the user grants Android's all-files access. Recognised Android filesystem-provider folders resolve to the real local path; other supported document providers continue to use the document bridge. The single active library, optional verified moves and existing ROM/save data are preserved.

For document-backed libraries, the update reduces repeated provider acquisition, path-resolution and directory-enumeration work. Native document requests no longer hold a global lock while waiting on the provider, and certified read-only file handles can obtain their size directly without changing their read position. These changes reduce storage overhead without caching failed path lookups across guest filesystem calls.

In the adopted Release baseline on the realme GT 8 Pro, using Turnip Banners `20261018-r3-astro`, Astro Bot showed its Sony logo within 22 seconds and reached the title prompt within 198 seconds; the preceding document-backed path was still black at 233 seconds. These are observed startup timings with the tested driver and library, not a controlled speedup ratio, an FPS measurement or a guarantee for every game/device. The same pinned Banners driver is now included in both the APK and AAB, alongside Mojo/Bionic and PanVK.

### Mali and graphics compatibility

The **PanVK (Experimental for Mali)** option is selected automatically on detected compatible Mali/kbase devices without a saved explicit choice, and applies to both PS4 and PS5. System Vulkan remains selectable manually; a custom-driver loading error is reported rather than silently falling back to another driver. Compatibility depends on the GPU, kernel interface, Vulkan features and game. The bundled driver is JimVulkan r8; private diagnostic driver variants are not part of this release.

The initial Ulefone Note 16 Pro / Unisoc T606 / Mali-G57 MC1 tests reached the SUCCI menu with both System and PanVK, and completed a PanVK 3D benchmark in a Debug development build. This is preliminary device support, not general PS4/PS5 game compatibility on Mali. The core also adds targeted shader handling for smaller native GPU subgroups, capability checks for shaders that actually need optional features, and Android surface/landscape corrections.

### Updated public core

The original 0.1.6 public core snapshot was refreshed from source revision `b9cde237`, including the native document-filesystem bridge, current PS4/PS5 runtime and shader changes, Android API 33 FEX support, tests and provenance records. Existing public build adaptations and licence notices are preserved. Dashboard, Android Java/resources and application packaging remain outside the core repository; their changes are included in the APK. The later controller package update does not change this native source snapshot.

### Android package

- APK: `lumenemu-0.1.6-spike-fexlab-v7.apk`
- Android App Bundle: `lumenemu-0.1.6-spike-fexlab-v7.aab`
- Package: `it.gssoft.lumenemu`
- Version: `0.1.6-spike-fexlab` (version code `7`)
- Android 13 (API 33) or newer, ARM64 (`arm64-v8a`); target API 36.
- Non-debuggable FEX Release build, signed with the existing GitHub publication certificate.

SHA-256:

```text
ea30e363fd205d7bac7c35d94125b5f83e09a197158d6bc3913f20315a0ae7fb
```

### Verification

The signed APK and AAB Release builds passed identity, signing-continuity and packaging checks, including bundletool validation and 16 KiB native alignment. Both contain the same native libraries, all three pinned graphics drivers and all 45 expected licence/provenance files. The native PS4/PS5 APK audit and 25 JVM Release tests for hardware detection, driver choices, defaults and preserved preferences passed.

The earlier adopted source baseline had 30 passing JVM tests (3 existing skips) and 14 passing Android instrumentation tests; those results are reused for the unchanged paths. The updated packages passed the current signing and native-packaging checks. No new FPS benchmark or connected-Mali validation is claimed. Alignment alone does not establish runtime compatibility with a 16 KiB-page Android device.

AAB SHA-256:

```text
0095c9c5d3abf6e85ed0260f68af07cf3e86c5ace58513e5d56eb7873ac470ad
```


## 0.1.5-spike-fexlab — Android version code 6

28 October 2026 · [Release and APK](https://github.com/gs-soft-it/lumen-emu/releases/tag/v0.1.5-spike-fexlab)

Fixed ROM folder handling and improved emulation speed.

### Android package

- File: `lumenemu-0.1.5-spike-fexlab-v6.apk`
- Package: `it.gssoft.lumenemu`
- Android 15 (API 35) or newer, ARM64 (`arm64-v8a`); target API 36.
- Signature verified and unchanged from the previous GitHub APK.
- Final Release build with temporary diagnostic tracing removed.

SHA-256:

```text
6dc5f2adeba2dece7d1136cb8ebd67b4db8b28092e3ae5c3e85b35daf5747bcb
```

The existing release checks passed for build, package identity, signature, native packaging, diagnostic removal and all 21 Android document fixtures. Checksum, signing continuity and 16 KiB ZIP alignment were verified again for publication. The final APK was not rebuilt or retested in gameplay for this upload; no new FPS claim is made. This publication updates the APK and download documentation; the public native source snapshot remains the 27 October export described by `SOURCE_SNAPSHOT.json`.

## 0.1.2-spike-fexlab — Android version code 3

27 October 2026 · [Release and APK](https://github.com/gs-soft-it/lumen-emu/releases/tag/v0.1.2-spike-fexlab)

### Changes

- Import the entire prepared library with Android's system folder picker into a private copy, with a space check, progress and cancellation. The source remains unchanged and can be disconnected after import.
- New imports create separate libraries without merging contents or automatically synchronising source changes. Existing shared-storage selections require an explicit re-import.
- Remove broad storage permissions. Clarify that content must already be extracted: the app does not open, extract or install PKG files.
- Include the current common-runtime fixes for shared memory, graphics resources, shader compilation/rendering and Android surface recovery, preserving the adopted defaults and leaving withdrawn experiments disabled.
- Show the active graphics driver in the included SUCCI benchmark result.

The public core is updated from source revision `d4040176`, retaining its native build adaptations, dependency pins and licence/provenance records. Android application changes are included in the APK; dashboard and application-shell sources remain outside this public repository.

### Android package

- File: `lumenemu-0.1.2-spike-fexlab-v3.apk`
- Package: `it.gssoft.lumenemu`
- Version: `0.1.2-spike-fexlab` (version code `3`)
- Requires Android 15 (API 35) or later and ARM64 (`arm64-v8a`); target API 36.
- Signature and checksum verified; the signing certificate matches the previous GitHub APK.
- No `MANAGE_EXTERNAL_STORAGE`, `READ_EXTERNAL_STORAGE` or `WRITE_EXTERNAL_STORAGE` permission.

SHA-256:

```text
d70d03dff98ce73b3e77a45f6d1802ccbc6e59024ac30b6a457490688e301a9d
```

### Verification and limits

This is the already-built final APK from 27 October; publication does not rebuild it. Its release records report a successful Release build including `lintVital`, 124 passing JVM tests with 3 skipped, and 11 passing Android instrumentation tests using a fixture document provider. Signature, package identity, native packaging, bundled SUCCI assets and 16 KiB ZIP/ELF alignment were verified. Checksum, signature continuity and ZIP alignment were checked again for this publication.

The separate full lint run reported 53 pre-existing errors in generated SDL Gen4 sources; it is not a passing gate. Instrumentation did not exercise the external system picker/cloud providers, native gameplay or the final benchmark result screen. No new FPS claim or complete device/game compatibility qualification is made for this exact APK. Binary alignment alone does not establish runtime compatibility on every 16 KiB-page device. Existing README compatibility reports remain tied to their original build.

## 0.1.1-spike-fexlab — Android version code 2

23 October 2026 · [Release and APK](https://github.com/gs-soft-it/lumen-emu/releases/tag/v0.1.1-spike-fexlab)

### Changes

- Fixed a startup failure affecting some PS5 games on Adreno 730 GPUs when the Vulkan `shaderBufferInt64Atomics` feature was required unconditionally.
- Enable that feature only when supported by the GPU. Check shaders that require it before use, with an explicit diagnostic when support is unavailable.
- Improved fatal-error diagnostics in release builds.

The public core update includes the matching PS5 Vulkan feature checks, shader admission checks, regression tests and provenance records. Existing public documentation and the exclusions of dashboard/application-shell sources are preserved.

### Android package

- File: `lumenemu-0.1.1-spike-fexlab-v2.apk`
- Package: `it.gssoft.lumenemu`
- Version: `0.1.1-spike-fexlab` (version code `2`)
- Requires Android 15 (API 35) or later and an ARM64 device (`arm64-v8a`).
- The APK's signature and checksum were verified. Its signing certificate matches the previous release.

SHA-256:

```text
d75cc19ed47a443d0b74fc4bf3a8242776ef6d55f7932c3624d465e7d2a4eb34
```

The supplied package changelog also reports AAB/APK integrity checks, 16 KB ZIP/ELF alignment and matching native libraries between the bundle and APK. Those bundle comparisons were not repeated during publication because the supplied archive contains the APK and changelog, without the AAB. Binary alignment alone does not establish runtime support for 16 KB-page devices.

### Earlier testing of the fix

The supplied changelog reports tests on a **Xiaomi 2201123G, Adreno 730, Android 15**:

| Game | Observed result |
| --- | --- |
| Dreaming Sarah | Reached the title screen at approximately 34 FPS. |
| Minecraft PS5 | Reached the initial screen at approximately 10 FPS. |

These observations come from the corrected build installed before version 2 was rebuilt. They do not establish full gameplay compatibility or long-term stability. **This release APK was not retested on the phone.** According to the supplied changelog, its native PS5 engine is byte-for-byte identical to the corrected build that was tested. These observations do not replace the separate device-specific compatibility reports in the README.
