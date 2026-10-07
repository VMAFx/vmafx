---
paths:
  - core/src/feature/cuda/speed_cuda_pipeline.c
  - core/src/feature/cuda/speed_cuda_pipeline.h
  - core/src/feature/cuda/speed/speed_score.cu
invariant: SpEED singular covariance, global matching, CPU-exact fp32, host tail for entropy and score.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Device-resident SpEED covariance and host tail

- **GPU SpEED means/cov must match CPU GLOBAL covariance, and ref/dis
  must use SEPARATE eigenvalue bases** (PR #1029,
  `research-1120-gpu-speed-covariance-eigenbasis-correctness`). Since
  ADR-1380 `speed/speed_score.cu` holds it by construction: every channel
  (U ref, U dis, V ref, V dis; temporal: ref, dis) owns its `means[25]`
  (global mean per phase-shift element over full submatrix, never
  per tile), its one covariance sweep divided by `N` once, and its own
  `speed_linalg_kernel` block (eigenvalues + QR). host tail reads
  channel's own eigenvalues. Never share basis between ref
  and dis channels: ~2× chroma error (masked on temporal, `ref ≈ dis`).
  Any change to SpEED kernel math mirrors CPU (`speed.c`), CUDA, HIP and
  SYCL in same PR. See §"Device-resident CAMBI and SpEED" below.

- **SpEED's singular-covariance path has TWO obligations** (ADR-1202 for
  chroma, ADR-1218 for both families). 25x25 SpEED covariance =
  regular only if EVERY eigenvalue >= 1e-6; CPU treats
  singular one as routine numerical condition, not failure. Twins
  must therefore: (1) zero **DEVICE** solution `d_sol`, never
  host `h_indterm` staging buffer. Score kernel reads `d_sol`;
  `h_indterm` re-downloaded from `d_indterm` at top of every
  pipeline run, so host memset = dead code, leaves device
  solution holding previous frame's result (or, on first frame,
  raw allocator memory). (2) Report singularity through
  `singular_out` out-parameter, keeping return value reserved for
  hard device failures. Caller then applies CPU's rule in
  `speed_extract_score()` — score `0` when exactly one of ref/dis
  singular — and, for chroma, imputes `speed_chroma_uv` from
  surviving channel. Twins in scope: `speed_chroma_cuda.c`,
  `speed_temporal_cuda.c`, `../sycl/speed_chroma_sycl.cpp`,
  `../sycl/speed_temporal_sycl.cpp`, `../hip/speed_chroma_hip.c`,
  `../hip/speed_temporal_hip.c`. Guarded by
  `test_{cuda,sycl,hip}_speed_singular_parity`. Note: *existing*
  `test_*_speed_{chroma,temporal}_parity` fixtures = 768x432, whose
  chroma planes give 4x2 = 8 blocks for 25x25 covariance — singular on
  every frame, so never reach regular path at all. Any new
  SpEED test needing regular frame must be at least 960x960.
- **Entropy + score = host tail; no device logarithm** (ADR-1477, replaces
  ADR-1430's bound). Device chain ends at `speed_solve_kernel` (variances).
  One readback per frame = tail block `SPEED_BUF_TAIL` (status, eigenvalues,
  variances; `SpeedGpuTailLayout`). `speed_cuda_pipeline_collect()` waits,
  then `speed_internal_gpu_tail_scores()` (`speed_internal.c`) = `speed.c`'s
  own fp64 `log2()` statements on host's libm. Twin == CPU bit for bit
  on any libm (glibc, libimf). Gate cells `speed_chroma.cuda`,
  `speed_temporal.cuda` in `scripts/ci/exact_twins.d/`; parity tests `==`.
  Never add `log2` / score kernel / second readback to device: contract
  test plants each. `test_cuda_speed_chroma_parity`: 960x960 texture (regular
  covariance), three scores, every frame; fixture + CPU run + comparison in
  `core/test/speed_chroma_twin_parity.h`, shared with HIP test.
- **Givens rotation = `speed_givens_unit()`** (`feature/speed_givens.h`,
  shared with HIP + SYCL): upstream's `1.0 / sqrt(1 + t * t)` (fp64 root and
  quotient, one rounding to fp32) from `rn_sqrt` / `rn_div` / `exact_fma`.
  Not `rn_div(1.0f, rn_sqrt(u))`: differs on 2.9M of 8.4M inputs. Proven on
  every input by `test_speed_upstream_form` (no device).

- **SpEED = one pipeline** (`speed_cuda_pipeline.c`), only TU loading
  `speed_score.cu` / launching its kernels (module owner in
  `test_cuda_module_lifecycle_contract.py`). Init constants from
  `speed_internal_gpu_configure()` (`speed_internal.c`), shared with SYCL
  (`speed_sycl_host.cpp`); contract types in `feature/speed_gpu_common.h`,
  SYCL aliases them. Never add second config routine (HISS-19).
- **CPU-exact fp32 in `speed_score.cu`.** Every rounding-relevant op =
  `__fadd_rn` / `__fsub_rn` / `__fmul_rn` / `__fdiv_rn` / `__fsqrt_rn`
  (never contracted, correctly rounded) + TU built `--fmad=false`
  (every fatbin, ADR-1403). No fp64 type in file, no `sqrtf` / `log2f` /
  `__fdividef`. `EIGENVALUE_EPS` compared as `0x1.0c6f7ap-20f` +
  `0x1.6bdb1ap-49f`. Only `exact_fma()` = error-free transforms.
- **`lanczos4` prescale weights = host table, never device sine.** CPU
  rounds each weight once from fp64 `sin()`; fp32 `sinpif()` is ulps off and
  SpEED amplifies (8.8e-3 relative on smooth field,
  `T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`).
  `speed_upload_lanczos()` fills `SPEED_BUF_LANCZOS` at init from
  `speed_internal_gpu_lanczos_weights()` (`vif_scale_lanczos4_axis_weights()`,
  CPU scaler's own routine): 9 taps per scaled column, then per scaled
  row; `scale_lanczos()` reads `wx` / `wy` from it. Buffer exists only for
  `lanczos4` with resample. Guards: contract test (planted `sinpif`,
  planted missing table), `test_speed_lanczos4_weights` (no device),
  `test_cuda_speed_lanczos4_parity`.
- **Init failure:** `speed_cuda_pipeline_open()` publishes pipeline before
  first allocation; extractor close (owed after failed init, ADR-1336)
  releases partial state. Do not self-close inside init.
- Guards: `test_cuda_device_resident_contract.py` (planted regression per
  rule), `test_cuda_cambi_parity{,_large}`,
  `test_cuda_speed_{chroma,temporal,singular}_parity`, smoke tests (all
  exit 77 without device).
- Covariance divisor = exact element count `sub_w * sub_h` as fp32 pair
  (`count_ff()`) into pair-divisor `ff_div_to_float()`. Never
  `(float)(sub_w * sub_h)`: above 2^24 (prescale > 2 past 16K) odd count
  has no fp32 value; speed.c divides by exact `size_t`. With `lo == 0`
  division = old one-float form bit for bit. Means divisor stays fp32
  (speed.c rounds it too). Guards: `test_speed_cov_count_division` (HIP
  header on host), `test_speed_cov_count_contract.py` (CUDA, HIP, SYCL).
