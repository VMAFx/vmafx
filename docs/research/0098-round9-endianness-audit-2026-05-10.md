<!-- markdownlint-disable MD013 -->
# Research-0098: Round-9 endianness audit — 10-bit YUV read path on big-endian hosts

**Date**: 2026-05-10
**Round**: 9, angle 2
**Status**: Deferred — pre-existing upstream debt; fork targets LE-only platforms

## Finding

The 10-bit (HBD) YUV pixel path in `core/tools/vmaf.c` reads raw bytes from
the YUV file via `fread(buf, 1, N, fin)` and then accesses them as `uint16_t *`
(e.g. lines 139–148, 176–187). Standard YUV HBD files store pixels as
little-endian 16-bit samples. On a big-endian host, the `uint16_t *` cast
would produce byte-swapped pixel values without any byte-swap correction,
yielding silently wrong VMAF scores.

The same pattern appears in:

- `core/tools/vmaf.c` (HBD branch in `video_open_yuv` and `video_open_y4m`)
- `core/src/dnn/tensor_io.h` (documented as "little-endian per pixel" at line 73)
- `core/src/feature/hip/float_psnr/float_psnr_score.hip` (comment line 95)

The DNN tensor_io surface documents the assumption explicitly; the YUV reader does not.

## Scope

This is not a fork-local regression. The upstream Netflix/vmaf codebase has the
same assumption throughout. The fork has never targeted a big-endian platform
and the CI matrix has no big-endian runners (s390x, SPARC, PowerPC BE). The fork
runs exclusively on x86_64 (little-endian) and arm64 (little-endian).

## Action

No immediate fix required. If a big-endian CI lane is added in the future (e.g.
IBM Z / s390x via QEMU), the fix is straightforward: add
`bswap16` / `le16toh` in the 10-bit read loop in `vmaf.c`, document the
assumption via `static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, ...)` at
the YUV reader entry points, and add a corresponding note in `AGENTS.md`.

The decision to add (or not add) a big-endian lane should be an ADR when it
happens, citing this research note.
