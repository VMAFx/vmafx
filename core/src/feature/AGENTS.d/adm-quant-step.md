---
paths:
  - core/src/feature/integer_adm_kernels.h
  - core/src/feature/sycl/integer_adm_sycl.cpp
  - core/src/feature/metal/integer_adm_metal.mm
  - core/src/feature/metal/integer_adm_metal_host.c
  - core/test/test_integer_adm_quant_step.c
  - core/test/test_integer_adm_quant_step_contract.py
invariant: dwt_quant_step() exponent = float product of k, temp, temp, as upstream; CPU and SYCL copies change together.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer ADM quantisation step (ADR-1475)

- **`dwt_quant_step()` = upstream statement, verbatim.**
  `pow(10.0, params->k * temp * temp)`: product in `float`, promoted for
  `pow()` only. Upstream: `libvmaf/src/feature/integer_adm.c`. Never cast
  product operand (`(double)temp`, `(double)params->k`). #552 cast one: CSF
  weights of scales 1..3 moved 1..3 ulp, `adm2` up to 8.5e-8 and
  `vmaf_v0.6.1` up to 1.83e-5 off Netflix on 94 % of frames. Golden gate
  blind (4..5 decimals).
- **CodeQL `cpp/integer-multiplication-cast-to-long` on that line: convert
  RESULT explicitly.** `pow(10.0, (double)(params->k * temp * temp))` is
  same operation as upstream's implicit promotion (same object code, checked
  with `objdump`); query reports only implicit widenings. `// codeql[...]`
  comments do not suppress in this repository's setup. Never cast operand.
- **Two copies, one edit.** CPU header (CUDA + HIP hosts include),
  `sycl/integer_adm_sycl.cpp::dwt_quant_step()`. SYCL copy: exponent in named
  `float`, then `pow(10.0, (double)exponent)`. Twins exact
  (`scripts/ci/exact_twins.d/adm.*`): copy left behind = red cell. Metal has
  no copy since T-METAL-INTEGER-ADM-TWIN-DEFECTS-2026-10-05:
  `metal/integer_adm_metal_host.c` takes `adm_csf_factors()` through CPU
  contexts; never bring local step back.
- **Guards.** `test_integer_adm_quant_step`: values == float-product form on
  5 geometries x 4 scales x 2 bands; double-product form differs (test keeps
  teeth); on glibc == bits of Netflix `cea2b4d8` build.
  `test_integer_adm_quant_step_contract.py`: reads both copies, planted
  cast per copy, planted Metal copy refused.
- **Upstream changes formula or `dwt_7_9_YCbCr_threshold`:** port into both
  copies, regenerate bit table of C test from upstream build,
  regenerate `testdata/scores_cpu_*.json`.
- **icx build:** step = GCC bits. Per-frame `powf()` in `adm_num_scale()` =
  Intel `libimf`, rounds rare arguments unlike glibc (1 of 240 measured
  frames, `integer_adm_scale1` 7.9e-8). No twin defect: `adm_sycl` == CPU of
  same build.
- Float ADM: other copy in `adm_tools.h`, see `adm-csf-weights-float.md`.
