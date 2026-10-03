---
paths:
  - core/src/feature/sycl/float_adm_sycl.cpp
  - core/src/feature/sycl/sycl_float_adm_math.h
  - core/test/test_sycl_float_adm_parity.c
invariant: float_adm_sycl.cpp = CPU float_adm, bit for bit; options must be captured, not hardcoded.
---
<!-- markdownlint-disable MD013 MD060 -->
# Float ADM extractor and kernels

- **`float_adm_sycl.cpp` options must be captured, not hardcoded**
  (ADR-1220) — see canonical note in
  [`../cuda/AGENTS.md`](../../cuda/AGENTS.md). `launch_terms` captures
  `adm_p_norm` (`TermArgs.p_norm`, `.is_cube`) and `adm_bypass_cm`
  (`TermArgs.bypass_cm` -> thresholds 0); host pooling =
  `adm_pool_bands_s()` with `adm_noise_weight` + `adm_p_norm`.
- **`float_adm_sycl.cpp` = CPU `float_adm`, bit for bit
  ([ADR-1434](../../../../../docs/adr/1434-sycl-float-adm-cpu-arithmetic.md),
  after ADR-1420 for CUDA).** Per-work-item code =
  `sycl_float_adm_math.h` (`decouple_sample()`, `terms_sample()`,
  `row_item()`), function for function with
  `../float_adm_gpu_common.h` (the CUDA + HIP twins' arithmetic, ADR-1458;
  was `../cuda/float_adm/float_adm_device.h`); the `.cpp` only launches.
  `divs()` = fp32 `n / d` = CPU `DIVS()` since ADR-1442 (reference
  divides on every host; no reciprocal, no probe, no table; device `/`
  correctly rounded under ADR-1367's flag line, checked per value by
  `test_decouple_csf_device`). Never again: a reciprocal in `divs()`;
  `cos^2 * (o^2 * t^2)` (reference:
  `(cos^2 * o^2) * t^2`; this alone was 1.28e-5 on BBB 4K); fp32 1/30,
  1/15 or gain (reference: `double`; no fp64 on device, ADR-0220 ->
  `times_constant()` / `add_scaled()` = exact fp32 pair, zone 2^-18 of a
  step, else integer replay on `SoftDouble`; `gain_limited()` = fp32
  product when the limit is an fp32 value, replay otherwise); centre tap
  anywhere but fifth; group reduction or a `double` fold (terms kernel
  stores nine terms per region sample, `row_item()` adds a row left to
  right at SG 8, host `fold_rows()` in fp32); own CSF weights, region,
  pooling root or floor (`adm_csf_rfactor_s()`, `adm_border_s()`,
  `adm_pool_bands_s()`, `1e-10 * (w * h) / (1920.0 * 1080.0)`).
  `adm_p_norm`: 3 = `(x * x) * x`, 1 = `x`, else device `pow` (1.8e-7
  from CPU, not exact). Options = CPU's incl. `adm_f1sN` / `adm_f2sN`,
  `adm_skip_aim_scale`, `adm_skip_scale0`; `adm_csf_mode` != 0 still
  `-EINVAL`. Upstream change to `adm_decouple_s()`, `adm_csf_s()`,
  `adm_cm_thresh3x3_s()`, `adm_csf_den_scale_s()`, `adm_cm_s()` ->
  header + CUDA header same PR. Exact vs the CPU extractor of the SAME
  build: host `powf` in `adm_pool_bands_s()` is libimf under icx, glibc
  under GCC (`aim` 1.6e-9 on 2 of 200 BBB frames between the two CPU
  builds, `T-ICX-LIBIMF-HOST-MATH-2026-10-01`). 4K frame 15.1 -> 12.3 ms.
  Scratch-free (ADR-1395).
  Exact twin: `scripts/ci/exact_twins.d/float_adm.sycl`. Guards:
  `test_sycl_float_adm_math` (host + device vs `adm_tools.c`),
  `test_sycl_float_adm_parity` (`==`, cases in
  `core/test/float_adm_twin_parity.h`),
  `test_sycl_float_adm_exact_contract.py` (19 planted regressions).
- **Term kernel shape = `VmafSyclKernelShape<kTermsSubGroup = 0, kTermsGrf
  = 256>` ([ADR-1501](../../../../../docs/adr/1501-sycl-float-adm-terms-large-grf-xe2.md)),
  twin `FadmTermsKernel` + probe `TermsKernel`.** Plain lambda = SIMD-32 on
  Xe2, 2 regs spilled (128 B, Arc B580, `test_sycl_kernel_scratch` fail).
  SG 16 spills 58-91 regs on the 16 other targets; SG 16 + GRF 256 spills
  on Xe-LP (no large GRF). Never a plain lambda, never a fixed SG here.
  Spill check without device: default-list build log, `spilled around N`
  per target. Probe queue in order (`on_default_gpu()`): its three kernels
  read each other's output; out of order the B580 ran them concurrently
  (row sums before terms, wrong and changing values).

- [ADR-0202](../../../../../docs/adr/0202-float-adm-cuda-sycl.md) +
  [ADR-0206](../../../../../docs/adr/0206-ssimulacra2-cuda-sycl.md) —
  CUDA + SYCL ports pinning `-fp-model=precise` as load-bearing.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `float_adm_sycl.cpp` | `float_adm.c` | `test_sycl_float_adm_parity.c` (bit-exact, every output, 8 to 16 bit), `test_sycl_float_adm_math.c` | ADR-0946 (round 3), ADR-1434 |
