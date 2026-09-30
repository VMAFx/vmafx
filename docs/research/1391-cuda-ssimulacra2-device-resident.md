<!-- markdownlint-disable MD013 MD060 -->
# Research-1391: Device-resident ssimulacra2 on CUDA

- **Status**: Active
- **Workstream**: [ADR-1391](../adr/1391-cuda-ssimulacra2-device-resident.md), [ADR-1363](../adr/1363-sycl-ssimulacra2-msssim-device-resident.md), [ADR-0206](../adr/0206-ssimulacra2-cuda-sycl.md), [ADR-0456](../adr/0456-ssimulacra2-cuda-blur-fusion-transpose.md)
- **Last updated**: 2026-10-01

## Question

`ssimulacra2_cuda` blurred on the device and did the rest of every scale on the
host (`T-CUDA-SSIMULACRA2-HOST-COMBINE-2026-09-29`). Can the whole frame stay on
the device, as [ADR-1363](../adr/1363-sycl-ssimulacra2-msssim-device-resident.md)
did for SYCL, within 1e-9 of the CPU extractor, and which blur layout is faster
on an RTX 4090?

## Sources

- `core/src/feature/ssimulacra2.c` (CPU reference) and the pre-change
  `core/src/feature/cuda/ssimulacra2_cuda.c` (master `10f27efe2`).
- `core/src/feature/sycl/ssimulacra2_sycl.cpp` (the ADR-1363 chain) and
  [Research-1363](1363-sycl-ssimulacra2-msssim-device-resident.md).
- Host `ryzen-4090-arc`: RTX 4090 (driver with CUDA 13, `/opt/cuda`), release
  build without LTO, `--precision max`. CPU runs use `--threads 16`.
- Fixtures: the Netflix 576x324 pair (48 frames), BBB 3840x2160 (50 frames),
  a 1920x1080 pair (50 frames), and pairs made from the Netflix pair with
  ffmpeg 9.0.2: 853x481 4:4:4 8-bit (8 frames), 1000x563 4:2:2 10-bit
  (8 frames) and 576x324 4:2:0 12-bit (16 frames). The CLI rejects odd widths
  for subsampled formats, so the 4:2:2 case has an odd height only.
- Timing: `scripts/dev/speed_gpu_parity.py` (median of 3 of
  `(t(22) - t(2)) / 20` ms per frame) and the same method at 1920x1080 through
  a scratch harness; device time through a CUPTI activity-trace injection
  library (per-kernel GPU time summed over the run).

## Findings

1. **The old twin was exact and slow.** Master is bit-identical to the CPU on
   every frame (48/48 at 576x324, 50/50 at 3840x2160), because the host
   combined in fp64 in the CPU's order. It took 16.5 to 24.3 ms per 576x324
   frame, 702.8 to 735.2 ms per 3840x2160 frame (three runs) and 238.7 ms at
   1920x1080, several times the CPU extractor's time.
2. **Everything up to the sums ports bit for bit.** With `--fmad=false` on both
   device TUs, products in their own expressions, `__fmaf_rn` for the YUV
   matrix in the ADR-0891 order, the shared `vmaf_ss2_srgb_eotf` (its table
   compiled into device memory through `VMAF_SS2_EOTF_LUT_STORAGE`) and
   `vmaf_ss2_cbrtf` dividing through `__fdiv_rn`, the device reproduces YUV
   conversion, XYB, the products, both IIR passes and the downsample. Evidence
   is indirect but strong: any per-pixel difference upstream of the sums grows
   to 1e-3 or more in the score (ADR-1205), and every measured difference is at
   the 1e-13 to 1e-12 level, the size of the sum reordering alone.
3. **fp64 terms in a fixed tree stay within 1.5e-12.** CUDA has fp64, so the
   per-pixel SSIM and edge terms are the CPU's own expressions; only the order
   of the sums differs. Measured max abs differences of the per-frame score
   against `--backend cpu`: 1.279e-13 (Netflix 576x324, 8 of 48 frames
   identical), 1.492e-12 (BBB 3840x2160, 0 of 50), 2.700e-13 (1920x1080,
   0 of 50), 9.948e-14 (853x481 4:4:4), 1.705e-13 (1000x563 4:2:2 10-bit) and
   1.279e-13 (576x324 12-bit). The tree shape depends only on the plane size,
   so repeated runs give identical scores: three runs of the twin on the same
   50 BBB 3840x2160 frames produced bit-identical per-frame scores, and three
   runs of the parity script reported the same differences.
4. **A tiled horizontal pass beats the ADR-0456 layout by about 2x.** Two
   layouts, bit-identical to each other on every frame:
   "v1" ports the ADR-0456 shape (separate multiply kernel, one thread per row,
   transpose, vertical pass on the transposed buffer); "v2" stages 32x32 tiles
   through shared memory with one warp per 32 rows, forms the products on
   load, and runs the vertical pass one column per thread on row-major data.
   Median ms per frame on a quiet host (1-minute load 1.9), two interleaved
   pairs each:

   | Size | v1 | v2 |
   |---|---|---|
   | 576x324 | 1.09, 1.20 | 0.40, 0.50 |
   | 1920x1080 | 4.44, 4.52 | 1.87, 1.88 |
   | 3840x2160 | 16.17, 16.06 | 7.73, 8.18 |

   CUPTI at 3840x2160 (22 frames): v1 spends 350 ms in kernels (blur H 134,
   blur V on the transposed buffer 75, transpose 32, multiply 22, combine 62),
   v2 169 ms (blur H 47, blur V 35, combine 62). v2 ships.
5. **The fp64 combine is now the largest kernel.** Device time per frame of the
   shipped chain (CUPTI, 48 or 50 frames): 0.44 ms at 576x324, 1.99 ms at
   1920x1080, 7.70 ms at 3840x2160. At 4K: `combine_partials` 2.77 ms (36 %),
   `blur_h` 2.16 ms (28 %), `blur_v` 1.63 ms (21 %), `xyb` 0.54 ms,
   `downsample` 0.33 ms, `yuv_to_linear` 0.26 ms, `combine_final` 0.02 ms.
   The GeForce fp64 rate (1/64 of fp32) makes the combine the first tuning
   target (exact fp32 pairs, as on SYCL, are the obvious experiment).
6. **Wall-clock timing on a shared host is noisy at these sizes.** The parity
   script's timing subtracts two short runs; with other agents building on
   the host (1-minute load between 2 and 38 over the runs) its median at
   3840x2160 came out 4.33, 7.10 and 7.40 ms per frame in three runs, and
   0.20, 0.75 and 0.19 ms at 576x324; master's came out 702.81, 735.23 and 721.89 ms, and 16.50, 16.86
   and 24.32 ms. The CUPTI device time (finding 5) is the steadier number; the
   wall-clock medians are consistent with it except for the first 4K run.

| ms per frame, RTX 4090 | Before (master `10f27efe2`) | After (ADR-1391) | CPU, 16 threads |
|---|---|---|---|
| 576x324, parity script, three runs | 16.50 / 16.86 / 24.32 | 0.20 / 0.75 / 0.19 | 1.70 to 2.57 |
| 1920x1080, same method, one run | 238.68 | 2.15 | 33.20 |
| 3840x2160, parity script, three runs | 702.81 / 735.23 / 721.89 | 4.33 / 7.10 / 7.40 | 171.36 to 433.77 |
| device time (CUPTI) 576x324 / 1080p / 4K | n/a | 0.44 / 1.99 / 7.70 | n/a |

Each run entry is the script's median of 3; the CPU column varies with the
host load of each run.

## Alternatives explored

- **v1 blur layout** (finding 4): measured, about 2x slower, not shipped.
- **fp32 pairs for the combine**: not measured; fp64 keeps the CPU's own
  expressions and is within 1.5e-12. First follow-up.
- **fp64 atomics for the sums**: rejected without measurement; the order would
  change from run to run.

## Open questions

- Pairs versus fp64 for `combine_partials` on GeForce, and whether the gain
  holds on data-centre parts with full-rate fp64.
- The vertical pass could run in place with a short register ring and drop the
  five horizontal-pass buffers (about 0.5 GB at 4K).
- 36 launches per frame at six scales; a merged `combine_final` or CUDA graph
  capture once the fork implements graph dispatch.

## Related

- [ADR-1391](../adr/1391-cuda-ssimulacra2-device-resident.md),
  [ADR-1363](../adr/1363-sycl-ssimulacra2-msssim-device-resident.md),
  [Research-1363](1363-sycl-ssimulacra2-msssim-device-resident.md).
- `core/test/test_cuda_ssimulacra2_parity.c`,
  `scripts/dev/speed_gpu_parity.py`.
