# FEXCore Android laboratory notice

This private, debug-only Lumen laboratory APK statically links portions of the
official FEX project from commit
`e17fdf9e90a49614e16ac8414a687477fb9e4f7c`, with Lumen's local
Android host-page port. The port preserves 4-KiB x86 guest pages and adapts
host allocations, guard pages, thread state and code-cache storage for
4-KiB or 16-KiB Android kernels. The verified local overlay also includes
decoded-block lifecycle corrections and Lumen's shared non-AFP SSE/AVX
denormal legalizer, including its bounded IR-use replacement helper. This
legalizer is original local AI-assisted code under the retained FEX MIT
terms, reused unchanged from Lumen's macOS overlay. These modifications are
not an upstream FEX release.

Source: https://github.com/FEX-Emu/FEX

The build uses these pinned submodules:

- fmt `1be298e1bd68957e4cd352e1f676f00e07dcfb57`;
- xxHash `e626a72bc2321cd320e953a0ccf1584cad60f363`;
- unordered_dense `3234af2c03549bc85656bfd3a86993bf1cd8aef1`;
- rpmalloc `1d85c246cd827ead6865f4f880d4fef53f2b1864`.

FEX's in-tree SoftFloat-3e and cephes sources are also compiled. Complete
applicable license texts are included in the sibling directories. FEX,
FEXHeaderUtils and CodeEmitter use MIT terms; fmt and unordered_dense use MIT;
xxHash uses BSD-2-Clause; SoftFloat-3e uses BSD-3-Clause; rpmalloc carries its
upstream permissive permission-without-fee notice; cephes carries the complete
upstream license and permission record.

FEX and the named projects are independent upstream projects. Their names do
not imply endorsement of Lumen.
