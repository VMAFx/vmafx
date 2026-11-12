---
paths:
  - core/src/feature/sycl/sycl_soft_double.h
  - core/src/feature/sycl/sycl_soft_signed.h
invariant: fp64-free kernels non-negotiable; double emulation through soft double on hardware without native fp64.
area: build-and-parity
---
<!-- markdownlint-disable MD013 MD060 -->
# fp64-free kernels and emulation fallback

- **fp64-free kernels non-negotiable** ([ADR-0220](../../../../../docs/adr/0220-sycl-fp64-fallback.md)).
  Every SYCL feature-kernel lambda captures, operates on `float`
  / integer types only. **No `double` operand inside `parallel_for`
  body**, no `sycl::reduction<double>`, no `sycl::plus<double>`.
  Hard rule, not soft: single fp64 instruction anywhere in
  TU's SPIR-V module causes Level Zero runtime to reject
  entire module on Intel Arc A-series and other fp64-less devices —
  even when offending kernel never submitted.
  - `double` allowed **outside** kernel lambda — host-side
    post-processing in `extract` / `flush` callbacks, score
    aggregation, log10 normalisation.
  - ADM gain limiting: `adm_gain_limit_product()` (`../adm_gain_limit.h`,
    64-bit integer only) = the CPU's truncated double product, exact for
    every limit in [1, 100] (ADR-1413); limit split on the host by
    `adm_gain_limit_split()`.
  - VIF gain limiting uses fp32 `sycl::fmin`.

- [ADR-0220](../../../../../docs/adr/0220-sycl-fp64-fallback.md) — SYCL
  feature kernels unconditionally fp64-free (T7-17).
