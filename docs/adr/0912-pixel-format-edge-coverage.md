<!-- markdownlint-disable MD060 -->
# ADR-0912: Pixel-format edge coverage at the libvmaf unit-test layer

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris, Claude (Anthropic)
- **Tags**: test, coverage, fork-local, pixel-format, hbd

## Context

The CPU extractor surface ships smoke / regression tests under
`core/test/test_*.c` for every registered extractor, but a sweep on
2026-05-31 found that nearly all of them allocate `VmafPicture`
instances at `(VMAF_PIX_FMT_YUV420P, 8)` — the Netflix golden gate's
common case. The non-default pixel-format and bit-depth combinations
are exercised only at three places:

1. `test_picture.c` — pure allocation-shape checks for YUV422P /
   YUV444P 8-bit; does not run an extractor.
2. `test_pic_preallocation.c::test_picture_pool_yuv444` — runs the
   full VMAF model on YUV444P 8-bit, but does not isolate a single
   extractor and so cannot localise a chroma-stride regression.
3. `test_psnr.c::test_16b_large_diff` — runs `psnr_hbd` on a 2×2
   YUV420P 16-bit input by `#include`-ing `integer_psnr.c` directly,
   bypassing the public extractor surface.

Net effect: PSNR / SSIM / CIEDE on 4:2:2 input, PSNR / SSIM on 10/12-bit
input, and CIEDE's `scale_chroma_planes` upscale path on any non-444
input had **zero end-to-end test coverage**. Bugs in those paths would
only be caught by the (slow) Python harness or the cross-backend
parity gate.

The chroma-stride and bit-depth-shift code paths are where bugs
historically hide:

- The Research-0094 / commit `b4f7a96e` fix to
  `picture.c::picture_compute_geometry` corrected a floor→ceiling
  division for odd-dimension YUV420P chroma planes that had silently
  under-allocated by one row for years.
- ADR-0186 / Vulkan PSNR chroma-geometry derived the same ceiling
  formula independently; `test_psnr_vulkan_chroma_geom.c` pins the
  math in isolation but not against an extractor.
- The CIEDE `init()` chroma-upscale scratch allocation (a pair of full
  YUV444P pictures + 6 aligned float scratch buffers) is taken on
  every non-444 input but has no smoke beyond CI's integration paths.

## Decision

Add `core/test/test_pixel_format_edge_coverage.c`, a single file
holding five end-to-end smoke tests that run the **public** extractor
surface (`vmaf_get_feature_extractor_by_name` +
`vmaf_feature_extractor_context_create / _extract / _close /
_destroy`, `vmaf_feature_collector_get_score`) against identical
ref / dist pairs at the high-value gap intersections:

| test                              | extractor | pix_fmt | bpc | exercises |
| --------------------------------- | --------- | ------- | --- | --- |
| `psnr_yuv422p_8bit_identical`     | `psnr`    | 4:2:2   | 8   | `ss_hor=1, ss_ver=0` 8-bit chroma stride |
| `psnr_yuv444p_10bit_identical`    | `psnr`    | 4:4:4   | 10  | full-resolution HBD chroma scoring |
| `psnr_yuv420p_12bit_identical`    | `psnr`    | 4:2:0   | 12  | 12-bit peak math (no other 12-bit test) |
| `ssim_yuv422p_8bit_identical`     | `ssim`    | 4:2:2   | 8   | integer_ssim 4:2:2 init / decimation |
| `ciede_yuv422p_8bit_identical`    | `ciede`   | 4:2:2   | 8   | scratch 4:4:4 scale_chroma_planes path |

Each test allocates 320×240 identical-pair input, fills both pictures
with the same deterministic rolling pattern (defensive against any
future extractor that special-cases flat input), runs one extract pass,
and asserts the per-plane PSNR equals `6 * bpc + 12` (the default
`psnr_max` ceiling for mse=0) / SSIM equals 1.0 / CIEDE2000 score is
positive (identical-pair de00=0 → score=+inf via
`45 - 20*log10(de00)`). The numeric expectations are not the point —
they are enforced more rigorously by the Netflix golden gate. The
point is that `init` / `extract` complete without an `-EINVAL` /
`-ENOMEM` / OOB-read fault for these combinations.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Status quo (no extra coverage) | zero effort | hides chroma-stride / HBD regressions until Python harness or cross-backend gate catches them | rejected — defeats the purpose of the unit-test layer |
| Extend `test_psnr.c` / `test_ciede.c` / etc. in place | minimum file count | mixes scopes (existing files exercise inner-helper math via `#include`); existing tests are organised by extractor not by pixel-format axis | rejected — keeping the new file scoped to the cross-cutting axis makes the gap visible and avoids inflating the existing tests' line count past the readability-function-size threshold |
| One file per (extractor × pix_fmt) combination | maximum locality | 5+ near-duplicate files for 5 cases; each one would need a copy of the alloc+pattern+extract scaffold | rejected — duplication outweighs locality at this scale |
| Generate YUV422P / YUV444P / 10-bit YUV fixtures via ffmpeg | reuses existing benchmark patterns | adds ~5 MB of binary fixtures, requires ffmpeg at build time, and the tests still need a pattern fill to defeat all-zero short-circuits | rejected — synthetic in-memory patterns are deterministic, sub-millisecond, and bit-exact across architectures |

## Consequences

- **Positive**: every CPU extractor that emits per-plane chroma scores
  (PSNR, SSIM) and the only extractor with a pixel-format-conditional
  init path (CIEDE) now has at least one fast-suite test on
  YUV422P input; PSNR also has one test at every supported bit
  depth (8 / 10 / 12; 16-bit was already covered by
  `test_psnr.c::test_16b_large_diff`).
- **Positive**: pre-push gate (`meson test -C build --suite=fast`)
  catches any future regression in the chroma-stride
  (`picture_compute_geometry`) or HBD scoring loops within ~50 ms.
- **Negative**: adds one more test executable to the fast suite
  (~40 ms wall-clock for all five cases combined, dominated by
  picture allocation).
- **Neutral / follow-ups**: the matrix can be extended over time —
  MS-SSIM, ADM, VIF, motion, psnr_hvs at non-default formats are
  still uncovered. Each is a small follow-up bundle once the
  pattern in this file is in tree.

## References

- See [ADR-0108](0108-deep-dive-deliverables-rule.md) for the
  six deliverables rule.
- See [ADR-0186](0186-vulkan-image-import-impl.md) for the
  Vulkan PSNR chroma-geometry derivation that motivates the same
  ceiling-division concern on the CPU path.
- See [`core/src/picture.c`](../../core/src/picture.c)
  (`picture_compute_geometry`) for the chroma plane dimension
  formula exercised by every test in this file.
- See [`core/src/feature/ciede.c`](../../core/src/feature/ciede.c)
  (`init`, `extract`) for the YUV444P fast-path /
  `scale_chroma_planes` upscale branch exercised by the ciede 4:2:2
  test.
- Source: user request 2026-05-31 ("audit test coverage for
  non-default pixel formats: 4:2:2, 4:4:4, 10-bit, 12-bit, 16-bit;
  add focused tests for high-value uncovered combinations").
