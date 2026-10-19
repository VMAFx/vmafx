---
paths:
  - core/src/feature/hip/speed_hip_pipeline.c
  - core/src/feature/hip/speed_hip_pipeline.h
  - core/src/feature/hip/speed/speed_pipeline.hip
  - core/src/feature/hip/speed/speed_hip_device.h
invariant: SpEED singular-covariance contract and device-resident CPU fp32 arithmetic flags must hold.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# SpEED singular-covariance contract (ADR-1202, ADR-1218)

25x25 SpEED covariance matrix regular only if **every** eigenvalue
at least `1e-6`. CPU treats singular one as routine numerical
condition, not failure; device chain (ADR-1384) matches it on two
counts.

1. **Zero solution on device.** `speed_hd_block_statistics()` starts
   every block's solution at 0 and solves only when channel's
   `status` slot says regular, so singular channel scores from zero
   solution, never from previous frame's memory.
2. **Singularity travels out-of-band.** Per-channel `status` words ride in
   tail block; errors are return codes.
   `speed_internal_gpu_tail_scores()` copies them into `SpeedGpuFrameResult`
   and applies `speed_extract_score()`'s rule (score `0` when exactly one of
   ref/dis singular) on host (ADR-1477);
   `speed_chroma_hip.c::combine_chroma_uv()` imputes
   `speed_chroma_uv` from surviving channel on host, from flags only.

Guarded by `core/test/test_hip_speed_singular_parity.c`. Older
`test_hip_speed_temporal_parity.c` fixture is 768x432, whose chroma
planes give 4x2 = 8 blocks for 25x25 covariance — singular on every
frame — never exercises regular path. SpEED test needing regular frame
must be at least 960x960 and textured: `test_hip_speed_chroma_parity`
uses 960x960 splatter fixture of `speed_chroma_twin_parity.h`
(ADR-1452).

## SpEED device-resident: CPU fp32 arithmetic by build flag (ADR-1384)

- `speed_chroma_hip` / `speed_temporal_hip` run ADR-1358 chain through one
  pipeline, `speed_hip_pipeline.{h,c}`. Twins: configure, bindings, upload,
  submit, collect. No `hipModuleLaunchKernel`, no sync, no host SpEED stage
  (`picture_copy`, `speed_internal_filter_and_downscale`, eigen / QR helpers)
  in twin TUs or pipeline.
- Per frame: `speed_hip_pipeline_upload()` (staged, no wait), seven kernels
  (last = `speed_hip_solve`, variances), one copy of tail block
  (`SpeedGpuTailLayout`: status, eigenvalues, variances).
  `speed_hip_pipeline_collect()` / `_wait()` = only wait
  (`vmaf_hip_kernel_collect_wait`).
- Init-time geometry, taps, scoring = `speed_internal_gpu_configure()`
  (`speed_internal.c`), shared with SYCL; types = `speed_gpu_common.h`.
  Parameter block = `speed_hip_params_fill()` / `speed_hip_taps_fill()` /
  `speed_hip_bindings_*()`; replay test calls same.
- Exact arithmetic = `hip_strict_fp_args` in `core/src/meson.build`
  (`-ffp-contract=off`, `-fhip-fp32-correctly-rounded-divide-sqrt`; every
  kernel, ADR-1407) + plain `*` `+` `/` `sqrtf()`. Never `__fmul_rn` / `__fadd_rn` / `__fdiv_rn` / `__fsqrt_rn`:
  without `OCML_BASIC_ROUNDED_OPERATIONS` = plain (contracting) operators /
  native approximate sqrt. Explicit `fmaf()` only for exact two-product.
- Entropy + score = host tail (ADR-1477, replaces ADR-1452's bound):
  `speed_hip_pipeline_collect()` waits, then
  `speed_internal_gpu_tail_scores()` (`speed_internal.c`) = `speed.c`'s own
  fp64 `log2()` statements on host's libm. No device logarithm, no
  `speed_hip_score` kernel, no libm test seam. Twin == CPU bit for bit on any
  libm. Gate cells `speed_chroma.hip`, `speed_temporal.hip`
  (`scripts/ci/exact_twins.d/`); `test_hip_speed_*_parity` assert `==`
  (`core/test/speed_chroma_twin_parity.h`, shared with CUDA test).
- Givens rotation = `speed_givens_unit()` (`feature/speed_givens.h`, shared
  with CUDA + SYCL): upstream's `1.0 / sqrt(1 + t * t)` in fp32 from `sqrtf`,
  `/`, `fmaf`. Not `1.0f / sqrtf(u)`. Proven on every input by
  `test_speed_upstream_form`.
- No fp64 in `speed/`. lanczos4 prescale weights = host table
  (`speed_hip_upload_lanczos()`, `speed_internal_gpu_lanczos_weights()`,
  CPU scaler's own routine), 9 taps per scaled column then per scaled row,
  read through `SpeedHipParams::lanczos`. No device sine: fp32 `sinpif`
  weights were 8.8e-3 relative off CPU on smooth content
  (T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30).
- Guards: `test_hip_speed_device_math` (replay of kernels + host
  tail vs CPU extractor, `==`, no device),
  `test_hip_device_resident_contract.py`, `test_hip_speed_*_parity` on
  device.
- Covariance divisor = exact element count `sub_w * sub_h` as fp32 pair
  (`speed_hd_count_ff()`) into pair-divisor `speed_hd_ff_div_to_float()`. Never
  `(float)(sub_w * sub_h)`: above 2^24 (prescale > 2 past 16K) odd count
  has no fp32 value; speed.c divides by exact `size_t`. With `lo == 0`
  division = old one-float form bit for bit. Means divisor stays fp32
  (speed.c rounds it too). Guards: `test_speed_cov_count_division` (HIP
  header on host), `test_speed_cov_count_contract.py` (CUDA, HIP, SYCL).
