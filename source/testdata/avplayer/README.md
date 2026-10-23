# Synthetic AvPlayer fixtures

These files are frozen Base64 representations of tiny QuickTime movies used to
exercise the macOS AvPlayer backend. They contain only programmatically
generated integer patterns. No game data, artwork, music, fonts, network input,
or third-party media was used.

The decoded movies and their generation logic are project-owned synthetic test
assets. Base64 is kept in Git instead of binary MOV data so the committed
representation remains inspectable and patch-friendly. Decoding is an offline
test preparation step; no media generator, FFmpeg executable, or external
process is required by the emulator at runtime.

## Frozen manifest

| Base64 source | Decoded movie | Decoded bytes | Decoded MOV SHA-256 | Duration | Tracks |
| --- | --- | ---: | --- | ---: | --- |
| `avplayer_synthetic_av.mov.base64` | `avplayer_synthetic_av.mov` | 79,596 | `1f629efa902f56be8e8ee0b6d062afb3c3b8da02ebf96817149415c6a1165beb` | 0.400 s | raw `2vuy` video and raw `sowt` audio |
| `avplayer_synthetic_video_only.mov.base64` | `avplayer_synthetic_video_only.mov` | 2,303 | `420917d48b30fe4427099c6b4d309cdfded7eab4c2016d91bebd677dc23d4f93` | 0.300 s | raw `2vuy` video only |

The same decoded-file hashes are available in `manifest.sha256`.

## Stream oracles

Both movies carry the same uncompressed video stream:

- QuickTime sample-entry type: `2vuy` (packed Cb, Y0, Cr, Y1).
- Dimensions: 16 by 16 pixels.
- Three frames at 10 frames per second.
- Presentation timestamps: 0/10, 1/10, and 2/10 seconds.
- Video media duration: 3/10 second.
- Cb and Cr are 128 in every pixel pair. The three constant-frame Y values are
  respectively 32, 112, and 208.
- Concatenated active 2vuy bytes: 1,536 bytes, FNV-1a-64
  `5aa3dd39d54f9b25`.

The audiovisual movie additionally carries this uncompressed audio stream:

- QuickTime sample-entry type: `sowt` (signed 16-bit little-endian PCM).
- Stereo, interleaved, 48,000 sample frames per second, four bytes per frame.
- 19,200 sample frames starting at PTS zero, for a duration of 2/5 second.
- The audio is 100 ms longer than the video. This intentionally exercises the
  state in which video has reached EOF while audio is still active.
- For zero-based sample-frame index `n`, the stored unsigned 16-bit bit patterns
  are `L(n) = (n * 257 + 0x1234) & 0xffff` and
  `R(n) = (n * 521 + 0x4321) & 0xffff`. Each value is stored low byte first and
  interpreted as signed two's-complement PCM by the reader.
- Concatenated PCM payload: 76,800 bytes, SHA-256
  `962f523c39acfde7217fe44cad561edb523500a52e435cba6f680c286d08dc63`,
  FNV-1a-64 `4c39dbe379c9d186`.

Tests must concatenate nonempty decoded audio payloads before applying the
whole-stream oracle. AVFoundation is allowed to divide the PCM into a different
number of sample buffers and can emit zero-sample marker buffers.

## Reconstruction

On macOS, decode the frozen representations with:

```sh
base64 -D -i avplayer_synthetic_av.mov.base64 -o avplayer_synthetic_av.mov
base64 -D -i avplayer_synthetic_video_only.mov.base64 -o avplayer_synthetic_video_only.mov
shasum -a 256 avplayer_synthetic_av.mov avplayer_synthetic_video_only.mov
```

GNU systems use `base64 --decode` instead of the macOS `-D` option. The two
reported hashes must match `manifest.sha256`. Tests should decode into their
build or temporary directory, not into the source tree.

## Generation provenance

The frozen files were produced offline on arm64 macOS 26.6 (build 25G72) with
Apple clang 21.0.0 and the system AVFoundation framework:

1. A transient Objective-C++ generator constructed each 2vuy frame directly
   from the byte oracle above and appended three timed `CMSampleBuffer` objects
   to an `AVAssetWriter` QuickTime session.
2. For the audiovisual fixture, the generator constructed the interleaved PCM
   bytes directly from the integer formulas above and appended one timed audio
   sample buffer. The video-only writer never added an audio input.
3. AVAssetWriter emitted a version-2 `lpcm` sample description for the raw audio.
   A transient deterministic finalizer replaced it with the documented
   version-0 QuickTime `sowt` description: data-reference index 1, two channels,
   16 bits per sample, compression ID 0, packet size 0, and 48,000 Hz in 16.16
   fixed-point representation. Audio payload and sample tables were unchanged.
4. The finalizer set creation and modification fields in `mvhd`, `tkhd`, and
   `mdhd` atoms to zero and adjusted all affected atom sizes. This removes clock
   input from the frozen media bytes.
5. The temporary generators, verifier, decoded MOV files, and PCM dump were not
   retained in the repository.

No external codec library was linked, no third-party encoder was used, and no
source media was read.

## Offline validation record

The Base64-decoded files were inspected, then read to EOF with
`AVAssetReaderTrackOutput`:

- AVFoundation reported one 16 by 16 video track in both files and one stereo
  48 kHz, 16-bit PCM track only in the audiovisual file.
- All three video PTS values and every active 2vuy byte matched the oracle.
- The audiovisual reader returned 19,200 nonempty audio sample frames; every
  little-endian channel value and both whole-stream hashes matched the oracle.
- Track durations were 0.300 s for video and 0.400 s for audio. Asset durations
  were therefore 0.300 s and 0.400 s respectively.
- The QuickTime container stores the audio sample entry as `sowt`.
  AVFoundation normalizes that uncompressed description to an `lpcm` ASBD when
  exposing the track, which is expected.
- `mdls` identified both outputs as `com.apple.quicktime-movie`. A forced
  CoreMedia metadata import (`mdimport -d2 -t`) additionally reported 16 by 16,
  durations 0.300/0.400 s, packed YCbCr 4:2:2 video, and Linear PCM audio only
  for the audiovisual fixture.
