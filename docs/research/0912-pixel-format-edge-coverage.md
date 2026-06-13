<!-- markdownlint-disable MD060 -->
# Research-0912: Pixel-format edge coverage audit

- **Date**: 2026-05-31
- **Author**: Claude (Anthropic) on behalf of Lusoris
- **Related**: [ADR-0912](../adr/0912-pixel-format-edge-coverage.md)

## Context

Most libvmaf unit tests under `core/test/` allocate `VmafPicture`
instances at `(VMAF_PIX_FMT_YUV420P, 8)`. The chroma-stride and
bit-depth-shift code paths that diverge for 4:2:2 / 4:4:4 /
10-bit / 12-bit / 16-bit inputs are largely unexercised at the
unit-test layer — regressions there are only caught downstream
(Python harness or cross-backend parity gate).

## Method

1. Enumerate every test that references a non-default pixel format
   or bit depth:

   ```bash
   grep -rIn -E '(VMAF_PIX_FMT_YUV4|yuv4(22|44)|10le|12le|16le)' core/test
   grep -rIn -E 'vmaf_picture_alloc\([^,]+,[^,]+,\s*(10|12|16)' core/test
   ```

2. For each registered CPU extractor (`psnr`, `ssim`, `ms_ssim`,
   `ciede`, `cambi`, `psnr_hvs`, `adm`, `vif`, `motion`), assess
   whether any existing test reaches its `init` / `extract` path
   with each of `{YUV422P, YUV444P}` × `{8, 10, 12, 16}`.

## Findings

### Coverage matrix before this PR (CPU extractor surface)

| extractor   | 420p8 | 420p10 | 420p12 | 420p16 | 422p8 | 422p10/12/16 | 444p8 | 444p10/12/16 |
|-------------|-------|--------|--------|--------|-------|--------------|-------|--------------|
| `psnr`      | ✓ many | (test_picture only) | — | ✓ `test_16b_large_diff` (2×2) | — | — | (full-model only) | — |
| `ssim`      | ✓ smoke | — | — | — | — | — | (full-model only) | — |
| `ms_ssim`   | ✓ smoke | — | — | — | — | — | (full-model only) | — |
| `ciede`     | (math only) | — | — | — | — | — | (full-model only) | — |
| `cambi`     | ✓ smoke + 10-bit YUV400 cases | ✓ via YUV400P 10 | — | — | — | — | — | — |
| `psnr_hvs`  | ✓ SIMD bit-exact | — | — | — | — | — | — | — |
| `adm`       | ✓ smoke | — | — | — | — | — | (full-model only) | — |
| `vif`       | ✓ smoke | — | — | — | — | — | (full-model only) | — |
| `motion`    | ✓ smoke | — | — | — | — | — | (full-model only) | — |

Notes:

- "✓ smoke" = at least one end-to-end smoke test wired into the
  fast suite at the extractor's public surface.
- "math only" = inner helper exercised via `#include` of the .c
  file (`test_ciede.c` only calls `ciede2000(LABColor, ...)`).
- "(full-model only)" = exercised by
  `test_pic_preallocation::test_picture_pool_yuv444` which runs
  the full `vmaf_v0.6.1` model on YUV444P 8-bit but does not
  isolate a single extractor.
- "(test_picture only)" = referenced by an allocation-shape test
  that does not run an extractor.

### Gap intersections by severity

**S0 — High-value, low-cost (this PR)**:

1. PSNR on 4:2:2 8-bit — exercises the `ss_hor=1, ss_ver=0`
   chroma-stride path; no other CPU test runs an extractor on
   YUV422P input.
2. PSNR on 4:4:4 10-bit — exercises HBD scoring with chroma
   plane dimensions equal to luma; the only existing HBD smoke
   uses 4:2:0 16-bit at 2×2.
3. PSNR on 4:2:0 12-bit — fills the 12-bit hole (the most
   common HDR bit depth in production pipelines).
4. SSIM on 4:2:2 8-bit — exercises asymmetric chroma geometry
   under the integer SSIM decimation / window machinery.
5. CIEDE on 4:2:2 8-bit — exercises `ciede.c::init` chroma
   upscaling (`scale_chroma_planes` + scratch YUV444 picture
   allocation) that the 4:4:4 fast-path bypasses entirely.

**S1 — Worth filing as follow-ups**:

- MS-SSIM at 10-bit / 12-bit.
- ADM at 10-bit (HDR pipelines feed the model with HBD ADM).
- VIF at 10-bit.
- `psnr_hvs` on 4:4:4 (no chroma path, but the JPEG-DCT block
  loop should be smoked).
- `motion` / `motion_v2` at 10-bit (allocates 16-bit blur
  scratch pictures regardless of input bpc, but the upcast path
  isn't smoked).

**S2 — Specialised; leave alone**:

- `cambi` already covers HBD via the 10-bit YUV400 tests in
  `test_cambi.c`.
- `tad`, `lpips`, `dists`, `transnet_v2`, `mobilesal`,
  `fastdvdnet_pre`, `ssimulacra2` — research-grade extractors;
  pix-fmt edge testing is lower priority than fork-original
  audits already on backlog.

## Decision

Plug the S0 cluster in one fast test (`test_pixel_format_edge_coverage.c`,
5 cases, ~40 ms wall-clock combined). S1 / S2 can land as follow-up
bundles using the same pattern.

## Reproducer

```bash
git worktree add -b test/pixel-format-edge-coverage \
    /tmp/wt-pixfmt origin/master
cd /tmp/wt-pixfmt
meson setup core/build-cpu core \
    -Denable_cuda=false -Denable_sycl=false \
    -Denable_hip=false -Denable_metal=disabled
ninja -C core/build-cpu test/test_pixel_format_edge_coverage
core/build-cpu/test/test_pixel_format_edge_coverage
# → "5 tests run, 5 passed"
meson test -C core/build-cpu --suite=fast
# → "Ok: 50  Fail: 0"
```
