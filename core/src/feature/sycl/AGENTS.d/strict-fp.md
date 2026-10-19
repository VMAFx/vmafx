---
paths:
  - core/src/feature/sycl/sycl_exact_fp.h
  - core/src/meson.build
invariant: Every TU is strict-clean; SYCL strict FP line load-bearing, one line for every TU.
---
<!-- markdownlint-disable MD013 MD060 -->
# Strict floating-point flags

- **Every TU is strict-clean regardless of origin or age.** file ported from
  Netflix, copied from another backend, or present before ratchet has no
  warning exemption. Its oneAPI compile, SYCL clang-tidy projection, cppcheck,
  and HISS audit must report no file-local diagnostic when touched. Split
  oversized helpers at cohesive phase boundaries; do not add `NOLINT`, lower
  baseline, or filter diagnostic to make lane green. Regenerate SYCL
  database with `scripts/ci/gen-sycl-compile-commands.py` before clang-tidy.
- **SYCL strict FP line load-bearing, one line for every TU
  ([ADR-1367](../../../../../docs/adr/1367-sycl-strict-fp-every-feature-tu.md)).**
  `sycl_strict_fp_args` = `-fp-model=precise -ffp-contract=off
  -foffload-fp32-prec-div -foffload-fp32-prec-sqrt`, in that order.
  precise alone leaves `a * b + c` contracted into FMA inside kernel
  lambdas and fp32 `/` and `sqrt` approximate (ADR-1358); without precise,
  icpx's fast model drifts `float_adm_sycl` past `places=4` (ADR-0202).
  With line, kernel's `+ - * / sqrt` round like CPU reference's;
  transcendentals (`sycl::log2`, `exp`, `pow`, `cbrt`), reduction order and
  fp32 stand-ins for fp64 reference expressions still differ. Where
  kernel approximates fp64 reference expression, write fused form it
  needs as `sycl::fma()` (`integer_vif_sycl.cpp` `sv_sq`); never rely on
  contraction. No TU gets private FP list; precision pair also rides
  `sycl_dependency` to link for SPIR-V JIT image (MSVC: explicit
  device link, ADR-1364, takes whole line).
  `test_sycl_fp_arith_contract` checks device result on hardware.
