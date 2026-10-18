<!-- markdownlint-disable MD013 MD060 -->
# ADR-2145: One input format table generates the import layouts, the FFmpeg and GStreamer lists and the CLI names; AYUV, UYVY and V210 join the layouts

- **Status**: Accepted
- **Date**: 2026-10-06
- **Deciders**: maintainer (popup Q-049); RC4 WP13
- **Tags**: api, rc4, gpu, zero-copy, abi, cli, docs

## Context

[ADR-2133](2133-vmafx-import-422-444-formats.md) put the semi-planar and packed
layouts into the import, with one CPU reference
(`vmafx_import_read_plane()`) and a hand-written layout table in
`frame_import.c`. The host path of `vmafx_frame_import()` already reads those
layouts from HOST memory. What was missing: AYUV, UYVY and V210 (the layouts
decoders and capture cards emit besides those), a list of formats the FFmpeg
filter and the GStreamer element accept that cannot drift from what the library
reads, and the command line, whose raw `.yuv` reader took 4:2:0 / 4:2:2 / 4:4:4
planar at 8, 10, 12 and 16 bits only (no luma-only, no 9 or 14 bits; the y4m
reader had no `p9`, `p14` or `p16` tag).

## Decision

1. **One table.** `core/api/vmafx.toml` gains `[[pixel_formats]]`: per
   `VmafxPixelFormat` value the layout class, the planar frame it makes, chroma
   format and siting, plane count, bit depths, packing, element positions, the
   devices that import it and its FFmpeg and GStreamer names per bit depth. The
   generator refuses a table that disagrees with the enum (a value without a row,
   a row without a value, a duplicate name, a planar format that does not match
   the chroma, a depth outside the row).
2. **Generated surfaces.** `core/src/vmafx/import_layouts_gen.h` (the layout
   table, replacing the hand-written one), `core/api/generated/vmafx_ffmpeg_formats.h`
   (`AV_PIX_FMT_*` lists: host, CUDA and HIP software formats; YUV and RGB apart),
   `core/api/generated/vmafx_gstreamer_formats.h` (format lists and caps strings)
   and `docs/usage/pixel-formats.md`. A drift check regenerates all of them; a
   planted extra row fails it. Tests verify the names against FFmpeg's
   `-pix_fmts`, `libavutil/pixfmt.h` and GStreamer's format list when installed.
3. **New layouts (ABI 0.1.11, additive).** `AYUV` (bytes A Y Cb Cr, FFmpeg
   `ayuv`), `UYVY422` (Cb Y0 Cr Y1), `V210` (ten-bit samples, six pixels in four
   little-endian words, FFmpeg has no pixel format for it; its row is the group
   of 16 bytes, padding as the producer likes). AYUV and UYVY are table rows
   (`uyv4` / `yuyv` packings with their own element positions); V210 is a new
   read form of `VmafxImportRead` (a period, a group size and a nibble pattern
   that names, per sample, the word of the group and the shift), executed by
   the same reference function and the same device gather kernel.
4. **One reference for the host readers.** The layout row type, the row extents
   and the read plan moved into the header-only `import_layout.h`, so the
   library, the device lanes and the `vmaf` command line's raw reader call the
   same code (HISS-19). The CLI reads the producer frame and converts it with
   `vmafx_import_read_plane()`.
5. **CLI.** Raw `.yuv`: `--pixel_format 400` and the layout names of the table;
   `--bitdepth` 8 to 16 (a layout that fixes the depth implies it; a conflicting
   value is refused naming the layout); y4m `420pN`, `422pN`, `444pN` and `monoN`
   for N = 9, 10, 12, 14, 16 (mono reads as 4:2:0 with neutral chroma, as the
   8-bit `mono` already did).
6. **Devices.** CUDA and HIP take the new layouts (the gather kernel gains the
   grouped form). SYCL and Metal follow in their lanes
   (`requests/WP13-1.md`, `WP13-2.md`).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Generated table from the definition (chosen) | One list; a surface cannot offer what the import refuses | Generator and emitters to maintain | Chosen |
| Hand-written lists per surface | No generator work | Three lists drift (the audit found the FFmpeg and GStreamer lists already narrower than the library) | Rejected |
| CLI through the FFmpeg binary | No CLI code | The vmaf CLI stops being self-contained; second conversion path | Rejected |
| V210 as a one-off kernel | Simple reference | Second code path next to the gather plan | Not chosen: the grouped plan fits the same kernel |

## Consequences

- **Positive**: host input in every semi-planar and packed layout FFmpeg writes,
  verified sample for sample against FFmpeg's own bytes
  (`test_vmafx_import_ffmpeg_oracle`) and bit for bit through the scoring cells
  (`test_vmafx_import_formats_cpu`); the filter and element lists are
  generated; the CLI reads what the API reads.
- **Negative**: ABI 0.1.11; every format row needs a device test before it
  lists a device.
- **Neutral / follow-ups**: V210 and the other new rows on SYCL and Metal;
  FFmpeg has no V210 pixel format, so the filter does not list it.

## References

- [ADR-1897](1897-vmafx-abi-0x-numbering.md), ADR-1880 (format envelope, PR #2185), [ADR-2133](2133-vmafx-import-422-444-formats.md), [ADR-2146](2146-vmafx-rgb-input-explicit-matrix.md).
- Q-049 (popup, 2026-10-06), answer "RC4, in the new API (Recommended)": semi-planar and packed host input in the new API, the FFmpeg filter and GStreamer element format lists generated from the definition, CLI gaps fixed with it.
- Tests: `core/test/test_vmafx_import_ffmpeg_oracle.c`, `test_vmafx_import_formats_cpu.c`, `test_vmafx_import_convert.c`, `scripts/codegen/tests/test_vmafx_api_formats.py`.
