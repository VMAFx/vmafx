---
paths:
  - core/src/feature/third_party/xiph/psnr_hvs.c
  - core/src/feature/psnr_hvs_score.c
invariant: Masking threshold = upstream float product (ADR-1488); host scoring tail and SIMD DCT stay bit-exact.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# PSNR-HVS Host Scoring Tail and SIMD DCT Parity

- **Masking threshold = upstream's statement** (ADR-1488):
  `s_mask = sqrt(s_mask * s_gvar) / 32.f` in `calc_psnrhvs()`, Netflix
  `libvmaf/src/feature/third_party/xiph/psnr_hvs.c:316-317`. Float product
  (rounded to `float`), root in `double`, stored `float`. No `(double)` in
  front of product: PR #552 (CodeQL sweep) added one, 27 of 319 measured
  frames left upstream by up to 9.4e-7 dB. CodeQL's
  `cpp/integer-multiplication-cast-to-long` is answered by writing
  conversion of product's RESULT: `sqrt((double)(s_mask * s_gvar))` (same
  object code as implicit form; query reports only implicit
  widenings; `// codeql[...]` comments do not suppress here). Same statement in `x86/psnr_hvs_avx2.c` and
  `arm64/psnr_hvs_neon.c` (`compute_masks()`), same value in CUDA, HIP
  and SYCL kernels (`hvs_threshold()`). Change one -> all six, same PR.
  Upstream sync: take upstream's side. Guards:
  `test_psnr_hvs_dispatch_invariance` (recorded blocks scored as Netflix
  master scores them), `test_psnr_hvs_simd`,
  `test_psnr_hvs_twin_exact_sum_contract.py`.
- **`psnr_hvs_score.c` = host tail of `calc_psnrhvs()` for GPU twins**
  (fork-local, ADR-1397). `vmaf_psnr_hvs_plane_score()` adds plane's
  terms one by one into single `float` (CPU's `ret`), then
  `/ pixels`, `/ samplemax^2` in `float`; `vmaf_psnr_hvs_combined_score()`
  and `vmaf_psnr_hvs_score_db()` = CPU `extract()` expressions. Built in
  `libvmaf_psnr_hvs_scalar_static_lib` (strict FP: combined score must
  not contract). Loop must stay sequential, `float`, index order: no
  vectorising, pairwise sum, `double`, or per-block subtotal (each
  changes rounding; guards `test_psnr_hvs_score`,
  `test_psnr_hvs_twin_exact_sum_contract.py`). Zero terms may be skipped
  (`x + 0.0f == x`); nothing else. **On rebase**: upstream change to
  tail of `calc_psnrhvs()` or to `extract()` in
  `third_party/xiph/psnr_hvs.c` -> same change here + every twin that
  calls it (`cuda/integer_psnr_hvs_cuda.c`,
  `sycl/integer_psnr_hvs_sycl.cpp`, `hip/integer_psnr_hvs_hip.c`;
  ADR-1401). `log10` behind dB value = host libm: icx-built
  binary (libimf) and gcc-built one (glibc) differ by one ulp on some
  frames, CPU extractor and twins alike, so compare twin and CPU from
  one binary.
- **`psnr_hvs` AVX2 DCT bit-exactness** (fork-local, ADR-0159):
  [`x86/psnr_hvs_avx2.c`](../x86/psnr_hvs_avx2.c) vectorizes
  Xiph/Daala 8×8 integer DCT across 8 rows in parallel
  (`__m256i`, 8× int32) via **butterfly → transpose → butterfly
  → transpose**. Byte-identical `od_coeff` output to scalar
  under `FLT_EVAL_METHOD == 0`; float accumulators (means /
  variances / mask / error) kept scalar by construction per
  ADR-0139 precedent. **On rebase**: never introduce
  horizontal-reduce vectorization of float accumulators
  without replicating per-lane scalar-float reduction
  pattern. Keep `#pragma STDC FP_CONTRACT OFF` at TU
  header — removing it allows `fmaf` and breaks 1-ulp
  guarantee. scalar TU
  [`third_party/xiph/psnr_hvs.c`](../third_party/xiph/psnr_hvs.c)
  is bit-exact reference; don't touch its butterfly block
  without matching changes in AVX2 TU. See
  [ADR-0159](../../../../docs/adr/0159-psnr-hvs-avx2-bitexact.md)
  and [rebase-notes 0052](../../../../docs/rebase-notes.md).
- **`psnr_hvs` NEON DCT bit-exactness** (fork-local, ADR-0160):
  [`arm64/psnr_hvs_neon.c`](../arm64/psnr_hvs_neon.c) is aarch64
  sister port to AVX2 TU. NEON's 4-wide `int32x4_t` splits
  each 8-column row into `r_k_lo` (cols 0-3) + `r_k_hi` (cols
  4-7); 30-butterfly runs twice per DCT pass, and 8×8
  transpose = four `transpose4x4_s32` (via `vtrn1q_s32` /
  `vtrn2q_s32` / `vtrn1q_s64` / `vtrn2q_s64`) + top-right
  ↔ bottom-left block swap. **On rebase**: two SIMD TUs
  (AVX2 + NEON) must move in lockstep with scalar Xiph
  reference — any change to butterfly in `psnr_hvs.c`
  requires matched edits to both SIMD TUs and re-run of
  `test_psnr_hvs_{avx2,neon}`. `accumulate_error()` must keep
  threading outer `ret` by pointer (ADR-0159 summation-order
  lesson; local float accumulator would drift Netflix
  golden by ~5.5e-5). `#pragma STDC FP_CONTRACT OFF` is ignored
  by aarch64 GCC (non-fatal `-Wunknown-pragmas`) but kept for
  portability; aarch64 GCC does not contract `a + b * c` across
  statements at default optimization anyway.
  **IMPORTANT — Intel icx (`intel-llvm`)**: `#pragma STDC FP_CONTRACT
  OFF` is also silently ignored by icx unless `-fp-model=precise` is
  also on command line. `vmaf_fp_model_args` and `vmaf_strict_fp_args` in
  `core/src/meson.build` are shared compiler-ID policy for x86 and AArch64
  carve-outs, scalar references, and `core/test/meson.build`'s
  `_simd_strict_fp_args`. Unix icx uses `-fp-model=precise` followed by
  `-ffp-contract=off`; `icx-cl` uses `/fp:precise /Qfma-`; MSVC uses
  `/fp:precise`; clang-cl forwards `/clang:-ffp-contract=off`. Do not copy raw
  strict-FP literals back into individual targets or duplicate mapping in
  test build. `vmaf_cuda_host_strict_fp_args` separately forwards
  native host spelling through nvcc (`/fp:precise` on Windows). Do not remove
  these flags without re-running `--suite=fast --suite=simd` under icx. Traced
  via 2026-05-30
  all-backends CI failure and closed by
  `T-MSVC-FFP-CONTRACT-D9002-2026-09-19`. See
  [ADR-0160](../../../../docs/adr/0160-psnr-hvs-neon-bitexact.md)
  and [rebase-notes 0052](../../../../docs/rebase-notes.md).
  **two flags are order-sensitive and must not be re-sorted.**
  `-fp-model=precise` implies `-ffp-contract=on`, so it goes FIRST and
  `-ffp-contract=off` LAST; other order re-enables contraction
  pair exists to disable. Measured on
  `speed_matmul_avx2` scalar tail with icx 2026.0: `-mfma
  -ffp-contract=off` emits zero `vfmadd`, adding `-fp-model=precise`
  after it emits nine, and putting `-fp-model=precise` before it emits
  zero again. `core/test/meson.build` must continue to alias shared
  `vmaf_strict_fp_args` variable: SIMD tests compile their own copies of
  scalar references, so replacing alias with divergent list puts two
  sides of every bit-exactness comparison on different contraction settings.
  That is what broke `test_ssimulacra2_simd` first time reorder
  was tried; see `T-ICX-FP-CONTRACT-FLAG-ORDER-2026-09-07` in
  [state.md](../../../../docs/state.md).
