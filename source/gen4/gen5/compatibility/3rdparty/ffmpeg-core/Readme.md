## FFmpeg builtins

This repository builds static FFmpeg libraries for KytyPS5.

The Windows version is built using clang-cl, this is done so that inline assembly optimisations (which are not supported by MSVC) can be enabled.

VP8, VP9, Opus and Vorbis decoding and the Matroska/WebM demuxer are enabled.
