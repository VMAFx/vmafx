# Research-0008: MS-SSIM decimate SIMD — FLOP accounting, summation order, bit-exactness

- **Status**: Active
- **Workstream**: [ADR-0125](../adr/0125-ms-ssim-decimate-simd.md)
- **Last updated**: 2026-04-20

## Question

Can the MS-SSIM decimate step be accelerated on x86 (AVX2 + AVX-512)
while still producing byte-identical float output to a scalar
reference, without modifying the vendored BSD-2011 Tom Distler
`core/src/feature/iqa/` subtree? What summation order does the
scalar reference need to use for that to hold?

## Sources

- [`core/src/feature/ms_ssim.c`](../../core/src/feature/ms_ssim.c)
  — defines the 9×9 2-D LPF `g_lpf` (lines 35–54) and the separable
  1-D forms `g_lpf_h` / `g_lpf_v` (lines 56–60). The 1-D coefficients
  are already present but unused by `_iqa_decimate`.
- [`core/src/feature/iqa/decimate.c`](../../core/src/feature/iqa/decimate.c)
  — scalar reference: calls `_iqa_filter_pixel` once per destination
  pixel with the 2-D kernel; invokes the kernel-wide function-pointer
  boundary handler `KBND_SYMMETRIC`.
- [`core/src/feature/iqa/convolve.c`](../../core/src/feature/iqa/convolve.c)
  — `_iqa_filter_pixel` implementation; confirms 81 multiply-adds per
  output pixel for a 9×9 kernel.
- [`core/src/feature/iqa/ssim_tools.c`](../../core/src/feature/iqa/ssim_tools.c)
  — existing AVX2 / AVX-512 dispatch pattern for SSIM primitives via
  `_iqa_ssim_set_dispatch`. This is the template the MS-SSIM decimate
  dispatch follows.
- Existing `core/src/feature/x86/*_avx2.c` files — established
  convention: one kernel per file, `<feature>_avx{2,512}.h` declares
  the entry point, dispatch lives in the caller.
- Rouse, D. & Hemami, S. — *Analyzing the Role of Visual Structure in
  the Recognition of Natural Image Content with Multi-Scale SSIM*.
  Cited in the `ms_ssim.c` comment block next to the 9/7 wavelet LPF
  definition.
- Wang, Simoncelli, Bovik — *Multi-scale structural similarity for
  image quality assessment*. The "Wang" path in `_compute_msssim`
  that the MS-SSIM* ("Rouse") path replaced.

## Findings

### FLOP accounting

- **2-D kernel (current path)**: 9 × 9 = 81 multiply-adds per
  destination pixel. At factor=2 on 1920 × 1080 input, the first
  scale produces 960 × 540 = 518 400 destination pixels × 81 MACs =
  42 M FMAs per scale. Count in source-image-pixel units:
  `20.25 × w × h` MACs.
- **Separable with stride-2 on horizontal pass (chosen form)**:
  horizontal pass produces `w_out × h` outputs (factor-2 subsampling
  applied during the horizontal pass only), each with 9 MACs →
  `w_out × h × 9 = 4.5 × w × h` MACs. Vertical pass then produces
  `w_out × h_out` outputs with 9 MACs each →
  `w_out × h_out × 9 = 2.25 × w × h` MACs. Total:
  `6.75 × w × h` MACs — a **~3× FLOP reduction** vs. the 2-D form,
  not 9× as initially overstated (earlier draft double-counted the
  stride-2 savings).
- **Why not full stride-2 on both passes?** The vertical pass needs
  all source rows, not just even-indexed rows, because the 9-tap
  vertical kernel at `y_out * 2` reads rows `y_out*2 - 4 .. y_out*2 +
  4`, which spans both even and odd source rows. So horizontal pass
  must produce output for every source row `y`, just subsampled in
  `x`. No further reduction is safe.
- **SIMD lane multiplier**: AVX2 ymm (8-lane float32) and AVX-512 zmm
  (16-lane float32) parallelise across *destination columns* in both
  passes. Inner-region speedup vs. scalar-separable: ~8× (AVX2),
  ~16× (AVX-512). Total vs. scalar 2-D: **~24× (AVX2)**, **~48×
  (AVX-512)** on the inner region.
- **Amdahl**: end-to-end MS-SSIM speedup depends on decimate's
  runtime share. The ~40% figure cited in the ADR is unverified;
  verified with `perf record` during implementation and this digest
  gets updated. If decimate is only 20% of MS-SSIM wall time, the
  end-to-end gain caps near 1.24× even with AVX-512.

### Summation-order problem (bit-exactness)

- The scalar 2-D path evaluates row-major:
  `sum = sum + g_lpf[ky][kx] * img[y+ky][x+kx]` for
  `ky=0..8, kx=0..8`. 81 accumulations in row-major order.
- A separable scalar path evaluates
  `tmp[kx] = sum_over_kx(g_lpf_h[kx] * img[y+ky][x+kx])` per row,
  then `sum = sum_over_ky(g_lpf_v[ky] * tmp[ky])`. Same coefficients
  mathematically (under the assumption that `g_lpf[i][j] ==
  g_lpf_h[i] * g_lpf_v[j]`, which is *true by construction* for a
  separable wavelet — verified inline by `ms_ssim.c` defining both),
  but the **summation order differs**.
- IEEE-754 addition is not associative. The two orders produce
  outputs that may differ by ≤ 1 ULP per pixel. That is a *real*
  numerical difference, not a bug — it just means the 2-D scalar
  output cannot be the oracle for the SIMD path.
- **Resolution**: bit-exactness is enforced between the SIMD path
  (AVX2 and AVX-512) and a **new scalar-separable reference**, not
  against the 2-D scalar. The scalar-separable reference replaces
  the 2-D path as the decimate oracle for MS-SSIM on CPU; MS-SSIM
  end-to-end scores in `testdata/scores_cpu_ms_ssim.json` are
  refreshed once and then locked against all three implementations.

### Invariant set (SIMD fast-path precondition)

The MS-SSIM call site is the only consumer of `_iqa_decimate` in
tree. That call site's kernel configuration is:

- `factor == 2`
- `k->w == k->h == LPF_LEN == 9`
- `k->normalized == 1`
- `k->bnd_opt == KBND_SYMMETRIC`
- `k->kernel_h` and `k->kernel_v` both non-NULL (MS-SSIM sets them
  before calling)
- `img`, `result` non-overlapping (guaranteed by `_alloc_buffers` in
  `ms_ssim.c`)

Any caller violating these invariants falls back to the scalar
`_iqa_decimate`. The dispatching function checks the full invariant
set before branching into SIMD; the SIMD kernel itself then asserts
the invariants (`VMAF_ASSERT_DEBUG`) for defence in depth (Power-of-10
§5).

### Boundary handling

- `KBND_SYMMETRIC` mirrors around the border: for `x < 0`, it
  returns `img[y][-x]`; for `x >= w`, it returns `img[y][2w-2-x]`.
- For the inner region where the 9-tap kernel fits entirely
  in-bounds (columns `4..w-5`, rows `4..h-5`), no boundary handling
  is needed and the SIMD path runs at full speed.
- For the 4-pixel border on each side, a **boundary-aware variant**
  of the horizontal pass is needed. Three options:
  1. Run the scalar-separable path on the border rows/columns,
     SIMD only on the inner region. Simplest; small border cost
     amortises to negligible on 1080p.
  2. Emit a mirrored scratch row with explicit `_mm256_blendv_ps`
     reflection. Higher complexity; marginal speed-up.
  3. Pre-pad the input with 4 mirrored columns on each side in a
     scratch buffer. Simple but allocates.
- **Chosen (pending implementation)**: option 1 — scalar border,
  SIMD inner. Matches the approach used in
  `core/src/feature/x86/adm_avx2.c` and keeps the bit-exactness
  story simple: the border uses the same scalar-separable reference
  the tests compare against.

### Prior art / existing SIMD convolution patterns in this tree

- [`core/src/feature/x86/adm_avx2.c`](../../core/src/feature/x86/adm_avx2.c)
  does a separable 5-tap horizontal + vertical convolution for the
  ADM CSF filter. Same pattern, shorter kernel. The horizontal pass
  uses `_mm256_fmadd_ps` with pre-broadcast coefficients; the
  vertical pass accumulates 8 rows at a time.
- [`core/src/feature/x86/integer_adm_avx512.c`](../../core/src/feature/x86/integer_adm_avx512.c)
  has the 16-lane AVX-512 variant of the same pattern. Direct
  structural template for our AVX-512 decimate.
- `core/src/feature/common/convolution.c` has a generic scalar
  separable convolution but it is integer-typed (`uint16_t` inputs).
  Not directly reusable for the `float` decimate path.

### Expected speed-up

Back-of-envelope (subject to benchmark verification):

- Scalar 2-D: baseline.
- Scalar separable: ~4× faster (FLOP reduction + better cache
  behaviour on the vertical pass).
- AVX2 separable: ~8× faster than scalar separable on the inner
  region (8-lane float32), ~30× vs. baseline.
- AVX-512 separable: ~14× faster than scalar separable inner
  region (16-lane), ~50× vs. baseline.

End-to-end MS-SSIM wall-time improvement depends on decimate-share
of total runtime; profiling will update this digest.

## Alternatives explored

- **Modify `_iqa_decimate` itself in the vendored iqa/ file**.
  Rejected per ADR-0125: only one in-tree caller, and mixing SIMD
  intrinsics into BSD-2011 Tom Distler code creates rebase friction
  if Netflix ever re-syncs iqa/ from upstream.
- **Keep 2-D kernel, SIMD the 81-MAC inner loop directly**. 81 is
  not a clean SIMD width; would need 9× 9-lane register tiles with
  masking. Same MAC count as scalar; no FLOP saving, just a lane
  multiplier. Strictly worse than the separable form.
- **Precompute a 2-D kernel `_mm256` broadcast table at module
  init**. Saves the coefficient load but not the 81-MAC count. Not
  worth the extra static data if we switch to the separable form
  anyway.
- **Use `_mm256_dp_ps` (horizontal dot-product intrinsic)**. High
  latency (~11 cycles Skylake), only 4-lane-wide, not available in
  AVX-512 in a useful form. Consistently slower than explicit
  FMA reductions on all measured μarchs.

## Open questions

- Does the border-scalar, inner-SIMD split meet the Power-of-10 §5
  assertion density bar (≥ 1 `VMAF_ASSERT_DEBUG` per function
  ≥ 20 LOC)? Expected yes — precondition asserts at dispatch entry
  already cover the invariants. Confirm with
  `scripts/ci/assertion-density.sh` during implementation.
- Is the observed decimate-share actually ~40 % of MS-SSIM wall time
  on the fork-added benchmark YUVs, or is that figure from a
  different codebase? Verify with
  `perf record -g -- ./build/core/tools/vmaf_bench --feature
  ms_ssim …` before/after and record numbers here.
- Does AVX-512 deliver the expected 14× over scalar separable, or
  does the downclocking penalty (Skylake-X behaviour) flatten the
  curve on short kernels? May need to gate AVX-512 behind a runtime
  benchmark rather than pure `vmaf_get_cpu()` detection.

## Related

- [ADR-0125](../adr/0125-ms-ssim-decimate-simd.md) — the decision
  this digest supports.
- [ADR-0106](../adr/0106-adr-maintenance-rule.md) — written-first
  ADR rule (this digest is iteration-time context for the ADR).
- [ADR-0108](../adr/0108-deep-dive-deliverables-rule.md) — the rule
  requiring this digest.
- [ADR-0012](../adr/0012-coding-standards-jpl-cert-misra.md) —
  Power-of-10 §5 assertion density, applied to new SIMD files.
- Sibling SIMD implementations in
  [`core/src/feature/x86/`](../../core/src/feature/x86/) —
  structural templates.
- PR for this workstream: TBD (opened after scalar-separable,
  AVX2, and AVX-512 land on `feat/ms-ssim-decimate-simd`).
