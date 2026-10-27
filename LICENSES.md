# Lumen Emu: licences, credits and source availability

Updated 22 October 2026.

Lumen Emu is an independent emulator port optimised for Android, iOS and Mac (Apple Silicon), developed by
GS-Soft using work from **shadPS4**, **Kyty** and **KytyPS5**. We thank their
authors and contributors. Lumen's integration and platform optimisations do
not replace the ownership or licences of that work. Lumen is not an official
release of, or endorsed by, these projects.

Mac (Apple Silicon) and iOS versions are coming soon; this platform scope
does not declare a public release or equal compatibility across platforms.

The core source is planned for publication soon in a new public repository,
with a single initial snapshot commit. The repository URL will be added once
published. No public core source URL is available yet.

## Emulator provenance

| Component | Origin and revision | Applicable terms and retained records |
| --- | --- | --- |
| PS4 core | [shadPS4](https://github.com/shadps4-emu/shadPS4), through [shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), `be6bc2e9c60799e071dd2fafa6216e8d80ec619c` | Original shadPS4 Emulator Project and contributor copyrights; GPL-2.0-or-later files select version 2 for this integration. [Provenance](source/gen4/UPSTREAM.json), [GPL text](source/gen4/LICENSES/GPL-2.0-or-later.txt) and individual source notices. MIT and other per-file terms remain applicable. |
| Original emulator ancestry | [Kyty](https://github.com/InoriRus/Kyty) | MIT, Copyright (c) 2021 InoriRus. The [complete Kyty MIT notice](source/gen4/gen5/compatibility/LICENSES/Kyty-MIT.txt) remains applicable to inherited portions, including those inherited through KytyPS5. |
| PS5 core | [KytyPS5](https://github.com/KytyPS5/KytyPS5) | GPL-2.0-only with inherited Kyty MIT notices. [Original import and subsequent selections](source/gen4/gen5/UPSTREAM.json): `b3e419ff1101999525fa2d061ada1d102cf788b1`; [active Android direct engine](source/gen4/gen5/engine/UPSTREAM.json): `fe3857a1715551e4f554b2ec29cc1ca3286287a1`. [GPL text](source/gen4/gen5/compatibility/LICENSE). |
| Original Lumen integration and adapters | GS-Soft / Lumen contributors | GPL-2.0-only unless the individual file states another applicable licence. [LICENSE](LICENSE) and [NOTICE](NOTICE). Changes to pre-existing MIT or other licensed files retain their applicable terms. |

These records identify provenance; they do not assert that the entire checkout
has one uniform licence. The GPLv2 text's example about “any later version” does
not change KytyPS5 or Lumen's GPL-2.0-only declaration. Third-party licences
must not be replaced by a new project-wide SPDX label.

## Dependency notices and packaging

The detailed inventory is kept with the source and the actual package inputs,
rather than replaced by this summary:

| Scope | Records and complete texts |
| --- | --- |
| Shared attribution summary | [THIRD_PARTY_NOTICES.txt](source/licenses/THIRD_PARTY_NOTICES.txt) |
| Gen4 dependencies and embedded fonts | [Notice hashes and origins](source/licenses/gen4/manifest.json), [dependency pins](source/licenses/gen4/dependencies.lock.json), [Android packaging map](android/gen4-legal-sources.json); complete texts under [source/licenses/gen4](source/licenses/gen4/) |
| Gen5 dependencies | [Fetched-dependency provenance](source/licenses/ps5/PROVENANCE.json), [Android packaging map](android/ps5-legal-sources.json) and original texts in the vendored dependency trees |
| Active direct FEX CPU, shared by Android Gen4 and Gen5 | MIT FEX `f2b679f6028ce1c38875233aecfcf5d3f8ebecec`; [CPU notices](source/licenses/gen4/cpu/NOTICE.txt), [FEX licence](source/licenses/gen4/cpu/FEX/LICENSE) and component texts in the same directory |
| Retained legacy/facade FEX | MIT FEX `e17fdf9e90a49614e16ac8414a687477fb9e4f7c`; [flavour notice and dependency texts](android/app/src/fexLab/assets/Legal/) |
| Optional QEMU TCG | [Source lock](source/cpu/qemu/source-lock.json), [dependency lock](source/cpu/qemu/dependency-lock.json); the native recipe supplies the exact build's licences and source/header notice manifest under `Legal/QEMU-TCG` |
| FFmpeg static libraries | [Licence](source/licenses/ps5/FFmpeg/LICENSE.md), [LGPL-2.1](source/licenses/ps5/FFmpeg/COPYING.LGPLv2.1), [copyright](source/licenses/ps5/FFmpeg/copyright) and [source/build record](source/licenses/ps5/FFmpeg/SOURCE.txt); qualify the exact archive used by each platform |
| Optional Adreno driver loader | [libadrenotools BSD-2-Clause](source/licenses/ps5/AdrenoTools/LICENSE), [liblinkernsbypass BSD-2-Clause](source/licenses/ps5/AdrenoTools/liblinkernsbypass-LICENSE) and [source record](source/licenses/ps5/AdrenoTools/SOURCE.txt) |
| Mesa Turnip Mojo/Bionic driver | [Source lock](android/gpu-drivers/turnip-mojo-bionic/source-lock.json), [local-change notice](android/gpu-drivers/turnip-mojo-bionic/NOTICE.txt) and the verified external package's complete licence directory; loader-enabled APKs copy these materials to `Legal/Vulkan/turnip-mojo-bionic` |
| Metal software tessellator | Microsoft D3D11 reference through Mesa 26.1.0, MIT, with Valve wrapper definitions; [modified MSL source and upstream SHA](source/emulator/src/Graphics/Metal/MetalSoftwareTessellatorSource.inc), full retained terms in [THIRD_PARTY_NOTICES.txt](source/licenses/THIRD_PARTY_NOTICES.txt) |
| Desktop/shared additional components | Original notices under `source/3rdparty`, [desktop packaging list](source/legal_notices.cmake), and [supplemental macOS texts](macos/packaging/legal/notices.json) |

The Gen4 and Gen5 inventories retain, among others, SDL, Dear ImGui, fmt,
spdlog, Vulkan headers and VMA, SPIR-V components, Boost, Zydis/Zycore,
LibAtrac9, xxHash, Zstandard, FreeType, OpenAL Soft and their applicable
transitive notices. Optional, development-only or platform-specific records
are not proof that a component is linked in every build.

Android stages these canonical project documents under `assets/Legal/lumen`,
Kyty's MIT notice under `assets/Legal/kyty`, and the two dependency inventories
under `assets/Legal/gen4` and `assets/Legal/ps5`. The legal-asset audit checks
packaged bytes against their canonical files. Updating these source documents
does not update an APK or application already built or installed.

## Unresolved release issues

The current notice update is not a declaration that the combined binaries can
be publicly distributed. The following concrete issues remain:

- **PS5 SPIRV-Tools / GPLv2-only:** the Gen5 compatibility build links
  `kyty_spirv_tools` and `kyty_spirv_tools_opt`; the active direct renderer also
  uses SPIRV-Tools. Its Apache-2.0 licence is incompatible with GPLv2-only for
  a combined work. A compatible replacement or sufficient permission from the
  relevant rightsholders is needed; merely retaining notices does not solve
  this. See the [Apache licence FAQ](https://www.apache.org/foundation/license-faq.html#GPL).
- **Gen4 FDK AAC / GPL:** `source/gen4/CMakeLists.txt` links `fdk-aac`. The
  [Fraunhofer FDK licence](source/licenses/gen4/dependencies/aacdec/fdk-aac/NOTICE)
  is not GPL-compatible; its additional source and patent provisions must not
  be described as MIT, BSD or covered by Lumen's GPL. See the
  [FFmpeg licensing explanation](https://ffmpeg.org/doxygen/trunk/md_LICENSE.html).
  A compatible replacement or sufficient permissions are needed for the
  affected combined distribution.
- **Actual platform closure:** assess the exact selected header/source licences,
  optional Vulkan/MoltenVK components, desktop Qt modules and plugins, and all
  linked static LGPL dependencies. Existing historical audits and packaged
  texts do not establish compatibility for a different build. LGPL components
  may require corresponding library sources and materials allowing relinking;
  FFmpeg, OpenAL Soft, FreeType and optional QEMU dependencies must be assessed
  under the licence option actually used.

The upstream licence texts and copyright notices remain intact. No licence
exception, relicensing permission or public-binary qualification has been
created by this documentation update.

## Preparing the future source snapshot

A single initial Git commit must still preserve the upstream copyright and
licence headers, complete licence texts, provenance revisions and Lumen's
modification records. Check modified files for prominent change notices and
dates required by GPLv2 section 2(a); an attribution page alone is not a
substitute. Do not label inherited source as wholly original Lumen code.

For a binary release, provide complete corresponding source or another
applicable GPLv2 section 3 mechanism, including the relevant interface files
and scripts used to control compilation and installation. A promise to publish
the core later, a link to unmodified upstream code, or a snapshot omitting the
modified covered integration is not a substitute. Preserve exact dependency
revisions, local patches, build recipes and the source/relinking materials
required by their individual licences. See the [GPLv2 text](LICENSE).

Publication of the new public core repository remains the project owner’s
responsibility. This licence-documentation update does not create that
repository or publish a binary release.
