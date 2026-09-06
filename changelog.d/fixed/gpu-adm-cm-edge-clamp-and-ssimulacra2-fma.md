- **`float_adm` GPU twins read the wrong sample at the far edge of the
  contrast-masking neighbourhood.** The CPU closed form
  (`adm_cm_thresh3x3_s`) has an asymmetric edge policy — the near edge
  mirrors to index 1, the far edge *clamps* to the last index — while
  the CUDA, SYCL, HIP and Metal twins mirrored both edges
  (`2 * half_w - x - 2`). The mismatch only shows when the ADM border
  crop `(int)(dim * 0.1 - 0.5)` collapses to 0, i.e. for band
  dimensions ≤ 14, because only then are the first and last row and
  column inside the summation region. On an RTX 4090 that moved
  `adm_scale3` by 8.58e-04 and `adm2` by 4.78e-04 against the places=4
  (1e-4) cross-backend gate, so `test_cuda_float_adm_parity` was red on
  any host with a GPU — and green in CI only because runners have no
  CUDA device and the test skips. Fixed by clamping the far edge in all
  four twins; `adm_scale3` agreement improves to 4.00e-08.

- **`ssimulacra2` scored differently on hosts with and without SIMD, and
  on every GPU backend.** The ADR-0891 FMA unification of the
  YCbCr → linear-RGB conversion reached the AVX2 / AVX-512 / NEON / SVE2
  kernels and the SIMD test's own private scalar reference, but not the
  five shipped non-SIMD copies — `core/src/feature/ssimulacra2.c` and
  the CUDA, HIP, Metal and SYCL host conversions — which kept plain
  mul-then-add. Because the test compares the kernels against its
  private reference rather than the shipped scalar function, it asserted
  bit-exactness and still passed. The resulting ~1 ULP difference is
  amplified by an ill-conditioned pipeline (the edge-diff term is a
  catastrophic cancellation, and pooling takes a 4-norm) into a 2.62e-03
  score delta against the same 1e-4 gate. Fixed by using `fmaf()` in the
  ADR-0891 positions in all five copies: CPU-vs-CUDA agreement improves
  from 2.62e-03 to ~2.8e-09, and the scalar fallback now matches the
  SIMD paths. Scores on AVX2/AVX-512/NEON/SVE2 hosts are unchanged, so
  no snapshot moved.

- **CUDA parity tests now also run against a 960x540 fixture.** All of
  them pinned one small size, which put the SSIM/MS-SSIM auto-scale
  (`max(1, round(min(w, h) / 256))`) permanently at 1 and kept every
  resolution-dependent branch unreachable. Both bugs above hid behind
  that gap, as did the earlier speed_chroma 4K defect.
