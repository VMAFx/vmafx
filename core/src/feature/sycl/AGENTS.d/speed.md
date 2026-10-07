---
paths:
  - core/src/feature/sycl/speed_*_sycl.cpp
  - core/src/feature/sycl/speed_sycl_*
  - core/test/test_sycl_speed_*
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

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `speed_chroma_sycl.cpp` + `speed_sycl_pipeline.cpp` | `speed.c` | `test_sycl_speed_chroma_parity.c`, `test_sycl_speed_singular_parity.c` | ADR-0957 (round 4), ADR-1358 |
| `speed_temporal_sycl.cpp` + `speed_sycl_pipeline.cpp` | `speed.c` | `test_sycl_speed_temporal_parity.c`, `test_sycl_speed_singular_parity.c` | ADR-0957 (round 4), ADR-1358 |

> **SpEED twins are wired and device-resident (ADR-0964, ADR-1358).**
> Both extractors are in `sycl_feature_sources` with shared
> `speed_sycl_pipeline.cpp` and `speed_sycl_host.cpp`. Their parity tests
> are live gates; twins match CPU bit for bit (see
> `docs/metrics/speed_qa.md`).

- Covariance divisor = exact element count `sub_w * sub_h` as fp32 pair
  (`count_ff()`, `sycl_exact_fp.h`) into pair-divisor `ff_div_to_float()`. Never
  `(float)(sub_w * sub_h)`: above 2^24 (prescale > 2 past 16K) odd count
  has no fp32 value; speed.c divides by exact `size_t`. With `lo == 0`
  division = old one-float form bit for bit. Means divisor stays fp32
  (speed.c rounds it too). Guards: `test_speed_cov_count_division` (HIP
  header on host), `test_speed_cov_count_contract.py` (CUDA, HIP, SYCL).
