---
paths:
  - core/src/feature/ssimulacra2.c
  - core/test/test_ssimulacra2_simd.c
invariant: SSIMULACRA 2 regression gates, linear RGB conversion, blur, and SIMD bit-exactness.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# SSIMULACRA 2 Regression Gates and SIMD Exactness

- `ssimulacra2.c` is fork-local (not upstream). It embeds several
  constant tables that must stay in lock-step with libjxl even across
  rebase:
  - **Non-finite score semantics (ADR-1302)** are also lock-step across scalar,
    AVX2, AVX-512, NEON, SVE2, CUDA, HIP, SYCL and Metal host code. Edge
    differences go through `ssimulacra2_score.h` before accumulation and every
    polynomial pool goes through its finalizer; each extractor rejects
    non-finite result before collector publication. Do not restore inline
    ordered comparisons: `NaN > 0` and `NaN < 0` are both false, which erases
    edge failure, and old final `else` mapped NaN to perfect `100.0`.
  - **Opsin absorbance matrix** (`kM00`…`kM22`) and bias `kB` — see
    libjxl `lib/jxl/opsin_params.h`.
  - **`MakePositiveXYB` offsets** — `B=(B-Y)+0.55`, `X*=14`, `X+=0.42`,
    `Y+=0.01`.
  - **108 pooling weights (`kWeights[]`)** and final polynomial
    transform (`0.9562382…`, `2.326765…`, `-0.0208845…`,
    `6.2484966e-05`, `0.6276336…`) — from `tools/ssimulacra2.cc`.
  - **FastGaussian coefficient derivation** — `3.2795·σ + 0.2546`
    radius, k∈{1,3,5}, Cramer's-rule 3×3 solve for β. Any drift from
    libjxl's `lib/jxl/gauss_blur.cc` formulas breaks bit-exactness of
    scalar blur.
  If libjxl changes any of these upstream, update scalar extractor
  in same PR (same for SIMD follow-ups, which will mirror
  same coefficient path).
- **SSIMULACRA 2 end-to-end regression gate** (fork-local, ADR-0164):
  [`python/test/ssimulacra2_test.py`](../../../../python/test/ssimulacra2_test.py)
  pins pooled + per-frame `--feature ssimulacra2` output on two
  checked-in YUV fixtures. **On rebase**: if scalar or any SIMD
  path changes semantically (should never happen per ADR-0161's
  bit-exact contract), test will fail with values that differ
  by more than 1e-4. Don't update pinned floats unilaterally —
  figure out which kernel drifted and fix it. Netflix golden
  assertions in `quality_runner_test.py` et al. remain untouched.
- **SSIMULACRA 2 `picture_to_linear_rgb` SIMD** (fork-local, ADR-0163):
  `ssimulacra2_picture_to_linear_rgb_{avx2,avx512,neon}` vectorises
  last scalar hot path (2×/frame). Strategy: per-lane scalar
  reads (all chroma ratios + 8/16-bit), SIMD matmul + normalise +
  clamp, per-lane scalar `powf` for sRGB EOTF. New decoupling
  header `ssimulacra2_simd_common.h` defines `simd_plane_t`;
  dispatch wrapper in `ssimulacra2.c` unpacks `VmafPicture` into it.
  **On rebase**: (1) keep scalar-order matmul chain
  `G = Yn + cb_g*Un; G += cr_g*Vn;` — regrouping drifts ~1 ulp;
  (2) per-lane scalar `powf` is load-bearing — no vector
  polynomial; (3) `simd_plane_t` layout `{data, stride, w, h}`
  is assumed by all three SIMD TUs; (4) arbitrary chroma ratios
  (non-420/422/444) must still work — don't delete `int64_t`
  fallback branch. SSIMULACRA 2 now has **zero scalar hot paths**.
  See
  [ADR-0163](../../../../docs/adr/0163-ssimulacra2-ptlr-simd.md) and
  [rebase-notes 0055](../../../../docs/rebase-notes.md).
- **SSIMULACRA 2 FastGaussian IIR blur SIMD** (fork-local, ADR-0162):
  `ssimulacra2_blur_plane_{avx2,avx512,neon}` vectorises 30×/frame
  2-pass separable IIR blur. Horizontal pass batches rows (AVX2: 8,
  AVX-512: 16, NEON: 4) and uses gather/lane-set loads to pull
  column-n values from N rows into SIMD vector; vertical pass
  SIMD-iterates columns over per-column `prev1_*`/`prev2_*`
  state arrays. **On rebase**: (1) preserve left-to-right summation
  `(o0 + o1) + o2` and `n2*sum - d1*prev1 - prev2` chaining — any
  re-grouping drifts by ~1 ulp; (2) `col_state` layout is
  `[prev1_0|prev1_1|prev1_2|prev2_0|prev2_1|prev2_2]` in 6×w
  contiguous floats; SIMD loads assume this; (3) NEON lane-set
  pattern (4 `vsetq_lane_f32` per input) replaces
  non-existent aarch64 gather intrinsic; (4) row-batching lane
  layout: lane i holds row (y_base + i). Regression test
  `test_blur` in `test_ssimulacra2_simd.c` catches all four. See
  [ADR-0162](../../../../docs/adr/0162-ssimulacra2-iir-blur-simd.md)
  and [rebase-notes 0054](../../../../docs/rebase-notes.md).
- **SSIMULACRA 2 SIMD bit-exactness** (fork-local, ADR-0161):
  [`x86/ssimulacra2_avx2.c`](../x86/ssimulacra2_avx2.c),
  [`x86/ssimulacra2_avx512.c`](../x86/ssimulacra2_avx512.c),
  [`arm64/ssimulacra2_neon.c`](../arm64/ssimulacra2_neon.c) and
  [`arm64/ssimulacra2_sve2.c`](../arm64/ssimulacra2_sve2.c) (T7-38,
  ADR-0213) all produce byte-identical output to scalar on 5
  vectorised kernels (`multiply_3plane`, `linear_rgb_to_xyb`,
  `downsample_2x2`, `ssim_map`, `edge_diff_map`) under
  `FLT_EVAL_METHOD == 0`, plus IIR blur and PTLR ports
  (ADR-0162 / ADR-0163). **On rebase**: (1) preserve left-to-right
  scalar summation order in every matmul + downsample chain —
  `(a+b)+(c+d)` pairing drifts by 1 ULP and regression test
  `test_ssimulacra2_simd` catches it; (2) `cbrtf` stays per-lane
  scalar libm — no vector polynomial; (3) reductions in
  `ssim_map`/`edge_diff_map` use ADR-0139 per-lane `double`
  scalar tail; (4) SVE2 sister TU is locked to fixed 4-lane
  predicate (`svwhilelt_b32(0, 4)`) so its arithmetic order
  matches NEON sibling regardless of runtime vector length. Never
  widen to `svptrue_b32()` without separate ADR plus snapshot
  regen, even if it reads as free perf win. See
  [ADR-0161](../../../../docs/adr/0161-ssimulacra2-simd-bitexact.md),
  [ADR-0213](../../../../docs/adr/0213-ssimulacra2-sve2.md), and
  [rebase-notes 0053](../../../../docs/rebase-notes.md) /
  [rebase-notes 0074](../../../../docs/rebase-notes.md).
- **SSIMULACRA 2 Vulkan host-path SIMD** (fork-local, ADR-0252):
  [`x86/ssimulacra2_host_avx2.c`](../x86/ssimulacra2_host_avx2.c) and
  [`arm64/ssimulacra2_host_neon.c`](../arm64/ssimulacra2_host_neon.c)
  are `plane_stride`-parameterised variants of `linear_rgb_to_xyb`
  and `downsample_2x2` for Vulkan pyramid layout (channel slot
  size = full-resolution frame, fixed across downsampled scales).
  These two TUs carry **same ADR-0161 bit-exactness contract**
  as their CPU-extractor siblings: per-lane scalar `vmaf_ss2_cbrtf`,
  `#pragma STDC FP_CONTRACT OFF`, `-ffp-contract=off`, left-to-right
  addition order. **On rebase**: if upstream or follow-up PR
  changes scalar `ss2v_host_linear_rgb_to_xyb` or
  `ss2v_downsample_2x2` arithmetic order in `ssimulacra2_vulkan.c`:
  update SIMD TUs and their `test_host_xyb` / `test_host_downsample`
  scalar references in lockstep. Byte-exact contract breaks
  silently if scalar changes without SIMD.
  See [ADR-0252](../../../../docs/adr/0252-ssimulacra2-host-xyb-simd.md)
  and [rebase-notes 0106](../../../../docs/rebase-notes.md).

## ssimulacra2's YCbCr -> linear-RGB conversion is FMA everywhere (ADR-0891, ADR-1205)

There are six copies of these three lines — scalar fallback in
`ssimulacra2.c`, CUDA / HIP / Metal / SYCL host conversions, and
AVX2 / AVX-512 / NEON / SVE2 kernels with their scalar tails. All of them must
use single-rounded fused multiply-add, in this order:

```c
R = fmaf(cr_r, Vn, Yn);
G = fmaf(cb_g, Un, Yn);
G = fmaf(cr_g, Vn, G);
B = fmaf(cb_b, Un, Yn);
```

Do not assume 1 ULP deviation here is harmless. pipeline is
ill-conditioned downstream: edge-diff term computes `|img - blur(img)|`,
catastrophic cancellation, and pooling takes 4-norm dominated by largest
survivors. Measured, one ULP in linear RGB became **2.62e-03** score delta.

`core/test/test_ssimulacra2_simd.c` compares SIMD kernels against
**private** scalar reference, not against shipped function, so it will not
catch shipped copy that drifts. Change shipped copies and that reference
together.
