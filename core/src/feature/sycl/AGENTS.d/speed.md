---
paths:
  - core/src/feature/sycl/speed_*_sycl.cpp
  - core/src/feature/sycl/speed_sycl_*
  - core/test/test_sycl_speed_*
  - core/src/feature/sycl/sycl_speed_cov_math.h
invariant: SpEED pipeline arithmetic contract and singular-covariance contract; device-resident twins.
---
<!-- markdownlint-disable MD013 MD060 -->
# SpEED pipeline arithmetic and singular covariance

- **SpEED pipeline arithmetic contract ([ADR-1358](../../../../../docs/adr/1358-sycl-speed-device-resident-linalg.md)).**
  Every SpEED kernel lives in `speed_sycl_pipeline.cpp`; two
  extractor TUs and `speed_sycl_host.cpp` hold none and never wait on
  queue outside `pipeline_collect()` / `pipeline_wait()`. four
  TUs are built with contraction off like every feature TU
  (`sycl_strict_fp_args`, ADR-1367). In
  pipeline, every division and square root goes through `div_rn()` /
  `sqrt_rn()` (shared with ssimulacra2 in `sycl_exact_fp.h`, ADR-1363),
  every product feeding add sits in named
  temporary, and fp64 comparisons of `speed.c` go through
  `below_eps()` / `below_eps_scaled()`. file must not mention
  fp64 type at all (`core/test/test_sycl_kernel_source_contract.py`).
  Entropy + score = host tail (ADR-1477): device chain ends at
  `launch_solve()` (variances); `enqueue_frame()` copies tail block
  (`SpeedGpuTailLayout`: status, eigenvalues, variances; one USM
  allocation) and `pipeline_collect()` waits, then calls
  `speed_internal_gpu_tail_scores()` (`speed_internal.c`) = `speed.c`'s own
  fp64 `log2()` statements on host's libm. No kernel evaluates
  logarithm; twin == CPU bit for bit on any libm (icx or GCC build). Gate
  cells `speed_chroma.sycl`, `speed_temporal.sycl`
  (`scripts/ci/exact_twins.d/`); parity tests assert `==`.
  Givens rotation = `speed_givens_unit()` (`feature/speed_givens.h`, shared
  with CUDA + HIP): upstream's `1.0 / sqrt(1 + t * t)` in fp32 from
  `sqrt_rn` / `div_rn` / `sycl::fma`; proven on every input by
  `test_speed_upstream_form`. Not `div_rn(1.0f, sqrt_rn(u))`.
  `lanczos4` prescale weights = host table, never device sine: CPU
  rounds each weight once from fp64 `sin()`, `sycl::sinpi()` is ulps off
  and SpEED amplifies (5.8e-5 relative on A380,
  `T-GPU-SPEED-LANCZOS4-PRESCALE-DRIFT-2026-09-30`). `upload_lanczos()`
  fills `Pipeline::lanczos` (device USM) at init from
  `speed_internal_gpu_lanczos_weights()`; `scale_lanczos()` reads `wx` /
  `wy` from it, so no private `wx[9]` / `wy[9]` (scratch, ADR-1395).
  Contract test plants `sycl::sinpi`, missing table, private array;
  `test_sycl_speed_lanczos4_parity` = device check.
  On rebase: plain `/` or `sycl::sqrt` added to pipeline kernel, or
  reduction reordered, breaks bit-exact parity
  `test_sycl_speed_*_parity` measures; keep order of every sum
  identical to its `speed.c` / `vif_tools.c` reference.

- **SpEED singular-covariance contract** — see canonical note in
  [`../cuda/AGENTS.md`](../../cuda/AGENTS.md). Since ADR-1358 SYCL
  twins decide singularity on device: `linalg_store()` in
  `speed_sycl_pipeline.cpp` writes per-channel flag, and
  `block_statistics()` solves into zero-initialised private solution
  that stays zero on singular channel, so no device buffer is read
  before it is written. host tail applies one-sided rule and
  copies flags into `FrameResult.singular`. ADR-1218, ADR-1477.

- **SpEED covariance entry is the reference's sequential fp64 sum
  (ADR-2690, `T-SPEED-CHROMA-SYCL-COV-1ULP-2026-10-06`).**
  `speed.c::compute_cov_kernel_scalar()` adds `(x - mean_x) * (y - mean_y)`
  into one fp64 running sum in raster order, rounding every add, and
  `compute_covariance_row()` stores `(float)(sum / (w * h))`. A parallel or
  compensated sum rounded once is not that value: on a cancelling
  off-diagonal entry it stores the neighbouring fp32 value (a real 3840x1600
  10-bit frame, `speed_chroma_u`, 1 ulp). `sycl_speed_cov_math.h`
  holds the reference's operations in 64-bit integers (`sycl_soft_signed.h`):
  `covariance_difference()`, `covariance_term()`, `covariance_chain()` and
  `covariance_store()` (the fp64 quotient by the exact count and the fp32
  conversion); `covariance_entry()` is the whole entry in one function and
  calls the same helpers. `launch_covariance()` runs them split across
  launches (ADR-2690): the differences (25 per pixel) and the products in
  parallel, stored as fp64 bit patterns, term-major, then one sequential
  chain per (channel, entry) over the stored terms in raster order, in slices
  of submatrix rows with the running sum kept as bits. Only work that does
  not depend on the running sum leaves the chain; the operations and their
  order stay the reference's. `test_sycl_speed_cov_chain` (device-free) and
  the split device half of `test_sycl_speed_cov_math` check it with `==`. On
  rebase: never bring back a pair accumulator, a group reduction or an `ff_*`
  quotient for this sum, and never give the entry a tolerance. An optimised
  kernel is allowed only if `test_sycl_speed_cov_math`
  (`==` against `compute_cov_kernel_scalar()`, fixture blocks a near-exact sum
  stores differently) still passes on a device
  (`T-SYCL-SPEED-COV-EXACT-SEQUENTIAL-COST-2026-10-06`).
  `test_sycl_speed_cov_exact_contract.py` pins the source shape, and
  `test_sycl_speed_chroma_parity`, `test_sycl_speed_temporal_parity` and
  `test_sycl_speed_singular_parity` compare every output with `==`. Measured
  cost on the A380 (`vmaf_v1.0.16_3d0h`, 3840x1600): 18.83 ms per frame before
  the fix, 28.53 ms with the one-work-item entry, 23.06 ms with the split
  chain; the chain is the remaining 4.2 ms. The CUDA and HIP twins have the
  old design (`T-CUDA-SPEED-COV-PAIR-SUM-SUSPECTED-2026-10-06`,
  `T-HIP-SPEED-COV-PAIR-SUM-SUSPECTED-2026-10-06`).

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `speed_chroma_sycl.cpp` + `speed_sycl_pipeline.cpp` | `speed.c` | `test_sycl_speed_chroma_parity.c`, `test_sycl_speed_singular_parity.c` | ADR-0957 (round 4), ADR-1358 |
| `speed_temporal_sycl.cpp` + `speed_sycl_pipeline.cpp` | `speed.c` | `test_sycl_speed_temporal_parity.c`, `test_sycl_speed_singular_parity.c` | ADR-0957 (round 4), ADR-1358 |

> **SpEED twins are wired and device-resident (ADR-0964, ADR-1358).**
> Both extractors are in `sycl_feature_sources` with shared
> `speed_sycl_pipeline.cpp` and `speed_sycl_host.cpp`. Their parity tests
> are live gates; twins match CPU bit for bit (see
> `docs/metrics/speed_qa.md`).

- Covariance divisor = exact element count `sub_w * sub_h`. SYCL since
  ADR-2690: `covariance_entry()` divides the soft-fp64 sum by the `uint64_t`
  count widened exactly (`signed_div()`, `signed_from_exact()`), the
  reference's fp64 quotient; `count_ff()` / `ff_div_to_float()` no longer
  store a covariance. Never `(float)(sub_w * sub_h)`: above 2^24 (prescale
  > 2 past 16K) odd count has no fp32 value; speed.c divides by exact
  `size_t`. Means divisor stays fp32 (speed.c rounds it too). Guards:
  `test_speed_cov_count_division` (HIP header on host),
  `test_speed_cov_count_contract.py` (CUDA, HIP pair form; SYCL soft-fp64
  quotient, no pair store in the pipeline).
