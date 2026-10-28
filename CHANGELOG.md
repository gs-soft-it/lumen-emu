# Changelog

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
