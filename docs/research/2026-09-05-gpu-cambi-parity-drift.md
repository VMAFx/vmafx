<!-- markdownlint-disable MD013 -->
# Research digest — the shared CUDA/SYCL CAMBI parity drift (2026-09-05)

Branch: `fix/gpu-cambi-parity-drift`. Closes `docs/state.md` rows
`T-SYCL-CAMBI-PARITY-DRIFT-2026-09-05` and
`T-CUDA-CAMBI-PARITY-DRIFT-2026-09-05`.

## The observation that started this

`T-SYCL-CAMBI-PARITY-DRIFT-2026-09-05` recorded that pooled
`cambi_hrs_1080_cmxv_17_vlt_0.06` on the 576x324 src01 pair was 0.262341
under `--backend sycl` and 0.259678 on the CPU — 2.66e-3 pooled, all 48
frames differing in the same direction. The CUDA twin produced the *same*
0.262341. Two independent GPU backends agreeing with each other but not
with the CPU is not floating-point rounding: CAMBI's GPU stages are
integer-only and the c-value / pooling residual is literally the CPU code
called through `cambi_internal.h`. The divergence therefore had to be in
the *content* the twins handed to that residual, i.e. in one of the three
GPU kernels or in the host-side option resolution.

## Method — stage bisect, not inspection

Both twins and `cambi.c` were instrumented behind a `VMAF_DEBUG_CAMBI`
environment gate (removed before commit) to print, for frame 0 of the
src01 pair, four checksums per scale: the spatial-mask population count,
the decimated image sum, the post-`filter_mode` image sum, and the
c-value sum plus the pooled per-scale score. Running the same pair
through `--backend cpu`, `--backend sycl` and `--backend cuda` then makes
the first divergent stage a matter of reading a table rather than of
arguing about kernels.

Pre-fix, frame 0, scale 0:

| Stage | CPU | SYCL / CUDA (pre-fix) |
| --- | --- | --- |
| spatial mask population | 7418 | **7489** |
| post-`filter_mode` image sum | 38774022 | 38739906 |
| c-values sum | 112339.282302 | 113400.099559 |
| pooled scale-0 score | 1.003262 | 1.012736 |
| final frame score | 0.339087 | 0.341433 |

The mask population is the **first** divergent stage, so everything after
it is downstream noise. A second build with the mask fix applied and the
`filter_mode` fix still reverted isolated the second defect: mask
population came back to 7418 and the c-value sums matched, but the
post-`filter_mode` image sum stayed at 38739906 against the CPU's
38774022 — a second, independent divergence that this particular frame's
mask happened to hide.

## Defect 1 — the spatial mask zero-pads, the twins clamped

`get_spatial_mask_for_index()` (`core/src/feature/cambi.c:1233`) builds a
summed-area table over the zero-derivative field. Rows outside the image
enter through `compute_dp_row()` with `deriv_valid == false`, which sets
`actual_width = 0` and therefore contributes **zero**
(`core/src/feature/cambi.c:1198-1213`); columns outside the image are
handled by the `pad_size` margin on either side of `dp_offset`, which is
`memset` to zero and never advanced past the row's prefix. So a pixel
within three of an edge sums a 7x7 window in which the out-of-image cells
count as 0.

Both twins instead *clamped* the sample coordinate to the nearest edge
pixel and summed a replicated neighbour:
`launch_spatial_mask()` (`core/src/feature/sycl/integer_cambi_sycl.cpp:223`)
and `cambi_spatial_mask_kernel()`
(`core/src/feature/cuda/integer_cambi/cambi_score.cu:101`). Since edge
pixels are `zero_derivative = 1` by construction (the CPU's
`get_derivative_data_for_row()` treats the last row and column as
"equal"), replication systematically *inflates* the box sum near the
border and pushes pixels over `mask_index` — which is exactly the
"SYCL consistently higher" signature in the original bug row. Fixed by
skipping out-of-image taps (contributing zero) in both kernels.

## Defect 2 — `filter_mode` leaves rows 0 and height-1 unfiltered

`filter_mode()` (`core/src/feature/cambi.c:1138`) writes the horizontal
mode into a rolling three-row buffer and only writes the vertical mode
back into `data[(i - 1) * stride + j]` when `i > 1`. For an image of
`height` rows that writes output rows `1 .. height-2`; **rows 0 and
`height-1` are never written at all**, so they keep their *pre-filter*
pixel values — not even the horizontally filtered ones. The twins ran a
separable H pass into a scratch buffer and a V pass back into the image
over the full grid with clamped row neighbours, so they wrote a filtered
value into both border rows.

Fixed by returning early from the vertical pass for `y == 0` and
`y == height - 1` in both twins
(`core/src/feature/sycl/integer_cambi_sycl.cpp:340`,
`core/src/feature/cuda/integer_cambi/cambi_score.cu:262`). This is
correct because the V pass writes back into the buffer the H pass read
from, so those two rows already hold exactly the pre-filter pixels the
CPU leaves in place.

## Defect 3 — the CUDA twin ignored `cambi_high_res_speedup`

`cambi.c` resolves `cambi_high_res_speedup` against the encode pixel
count in `init()` (`core/src/feature/cambi.c:622-640`), halves the
adjusted window when it survives (`adjust_window_size()`,
`core/src/feature/cambi.c:471`), and runs one extra decimation before
scale 0 (`cambi_score()`, `core/src/feature/cambi.c:1621`). The SYCL twin
gained all three in PR #1307; the CUDA twin declared the option (so the
feature name matched the model key) but its state field was commented
`reserved; v1 ignores it`. Because the default model
`vmaf_v1.0.16_3d0h` sets `hrs=1080`, every `>= 1080p` CUDA run executed a
different pipeline than the CPU — the origin of the larger 1.76e-2 `vmaf`
drift the bug row recorded on the 1080p Tennis pair, as opposed to the
2.66e-3 seen at 576x324 where `hrs` resolves to 0 on both sides.

## Why the existing parity gates stayed green

`core/test/{test_sycl_cambi_parity.c,test_cuda_cambi_parity.c}` used a
256x256 quantised-gradient fixture: constant down every column, constant
along every border, and identical in ref and dist apart from a salt.
Neither defect is observable on it — the fixture is flat exactly where
both defects live. Both tests now run a second "textured" fixture
(horizontal bands + a vertical ramp + a deterministic LCG dither + an
inverted border ring) alongside the original. Against the pre-fix SYCL
kernel that fixture fails with `delta=2.11e-02` while the original
fixture still passes, which is the coverage gap stated as a test.

## Verification

Per-frame `cambi_hrs_1080_cmxv_17_vlt_0.06` compared at `--precision max`
(`%.17g`) across `--backend cpu|sycl|cuda`:

| Fixture | Frames | pooled cambi (all three backends) | max per-frame delta |
| --- | --- | --- | --- |
| src01 576x324 | 48 | 0.2596781483085728 | 0 (0 frames differ) |
| Tennis 1920x1080 | 10 | 0.5670459080762581 | 0 (0 frames differ) |
| checkerboard 1px | 3 | 0 | 0 |
| checkerboard 10px | 3 | 0 | 0 |

The two checkerboard pairs score `cambi = 0` on every backend, so they
confirm no regression but carry no parity signal for this feature.

Pooled `vmaf` is *not* identical, because ADM / VIF / motion twins carry
their own documented deltas: src01 82.81606248 (CPU) / 82.81606028 (SYCL)
/ 82.81606251 (CUDA); Tennis 41.45989351 / 41.45990311 / 41.45990417.
That is 2.2e-6 and 1.1e-5 respectively, against the 2.0e-3 and 1.76e-2
recorded before this branch.

## Rejected alternative

Relaxing `PARITY_TOL` in the two parity tests to cover the observed drift
was rejected outright: the drift is a semantic mismatch with `cambi.c`,
not device rounding, and `cambi.c` is pinned by the Netflix golden gate
so the twins are the side that must move.
