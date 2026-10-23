# Changelog

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
