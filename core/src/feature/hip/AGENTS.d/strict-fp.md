---
paths:
  - core/meson.build
  - core/src/meson.build
invariant: Every HIP kernel builds with strict IEEE-754 floating-point argument flags.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Every kernel is IEEE-strict: `hip_strict_fp_args` (ADR-1407)

hipcc / amdclang++ default to `-ffp-contract=fast` on device side,
silently fusing `a * b + c` into FMAs, across statements too. CPU
reference build does not contract. One list for every kernel in
`core/src/meson.build`, between `VMAF HIP strict FP policy` markers:
`hip_strict_fp_args = ['-ffp-contract=off',
'-fhip-fp32-correctly-rounded-divide-sqrt']` (second flag = hipcc default,
pinned). No per-kernel table (`hip_cu_extra_flags` of ADR-0594 is gone):
new kernel in `hip_kernel_sources` gets policy automatically. Never add per-kernel exemption or
`#pragma clang fp contract(on|fast)`; need fused op -> write `fmaf()` /
`fma()` explicitly. Guards: `test_hip_strict_fp_policy.py` (device-free,
planted regressions), `test_hip_fp_arith_contract` (device: 1 M random
`a * b + c`, `/`, `sqrtf` vs correctly rounded host; probe built with
same list). gfx1036: hipcc default fails it with 126577 fused multiply-adds;
`-fno-hip-fp32-correctly-rounded-divide-sqrt` with 303181 divisions and
158765 square roots off.
