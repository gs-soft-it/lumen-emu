# LibAtrac9 provenance

- Upstream storico: fork del vecchio progetto
- Pinned commit: `946e05a9212976626a9f5e52f29c0a7202871f29`
- Imported subtree: `C/src`
- License: MIT; see `LICENSE` in this directory.
- Original project: <https://github.com/Thealexbarney/LibAtrac9>

The source snapshot is kept in-tree so supported builds remain reproducible
and do not require network access. `CMakeLists.txt` is LUMEN-specific build
integration and is not copied from either upstream project.

## Local safety patch

- `C/src/utility.c`: return zero when reversing zero bits. This avoids a
  right-shift by 32 detected by UndefinedBehaviorSanitizer; widths 1 through
  32 retain the upstream behavior. The same file implements sign extension
  with unsigned masks and a widened subtraction, avoiding signed left-shift
  overflow while retaining the two's-complement result for widths 1 through
  32.
- `C/src/unpack.c` and `C/src/band_extension.c`: reject or defensively skip
  band-extension groups unless the quantization-unit count is in the table's
  supported range 13 through 20, preventing out-of-bounds table access on
  malformed frames.
- `C/src/structures.h`: size the gradient workspace for all 48 unit indices
  accepted by the bitstream parser.
- `C/src/bit_reader.c`: assemble bit windows with unsigned arithmetic so
  high-bit input bytes cannot trigger signed left-shift undefined behavior.
- `C/src/bit_allocation.c` and `C/src/error_codes.h`: normalize trailing
  whitespace only; no semantic change.

## Original local decoder-state extension

- `C/src/libatrac9.c` and `C/src/libatrac9.h`: add `Atrac9CloneHandle`, an
  original local LUMEN extension against the MIT-licensed source snapshot
  identified above. No new upstream implementation or console ABI/NID was
  imported. The upstream copyright, license and attribution are retained
  unchanged.
