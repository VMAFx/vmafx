---
paths:
  - core/src/feature/adm_tools.h
  - core/src/feature/barten_csf_tools.h
  - core/src/feature/metal/float_adm_metal.mm
  - core/test/test_float_adm_csf_upstream.c
  - core/test/test_float_adm_csf_upstream_contract.py
  - core/test/barten_csf_cxx.cpp
invariant: float ADM CSF weights (Watson step, Barten CSF) = upstream float arithmetic; Barten header same bits in C and C++.
area: adm
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Float ADM CSF weights: Watson step and Barten CSF (ADR-1489)

- **`adm_tools.h::dwt_quant_step()` = upstream statements.** `float r`,
  `float temp`, `float Q`; exponent `params->k * temp * temp` = `float`
  product, promoted for `pow()` only. Upstream:
  `libvmaf/src/feature/adm_tools.h`. No `double` local, no cast on product
  operand. #552 cast exponent operand, #760 widened all three locals: 32 of
  40 probed steps left Netflix bits, every `float_adm` score too.
- **CodeQL `cpp/integer-multiplication-cast-to-long` on `Q` line: convert the
  RESULT explicitly.** `pow(10.0, (double)(params->k * temp * temp))` is
  upstream's implicit promotion written out (same object code); the query
  reports only implicit widenings and `// codeql[...]` comments do not
  suppress here. Never cast an operand.
- **`barten_csf_tools.h` = upstream float products.** `linear_interpolate()`
  all `float`. `p_0 * f`, `f / 7`, `a * b`, `-b[i] * f`, `csf * mtf * rod`:
  formed in `float`, result promoted. Never promote operand
  (`(double)p_0 * f`): #44 form, 138 of 144 probed weight sets off Netflix.
- **Promotion of each result = explicit cast.** `pow((double)(p_0 * f),
  (double)p_1)`, `exp((double)(-b[i] * f))`. Header compiled as C++ by
  `sycl/integer_adm_sycl.cpp` and `metal/integer_adm_metal.mm`; C++
  `pow(float, float)` / `exp(float)` = float functions. Upstream text
  verbatim, compiled as C++: 135 of 144 weight sets differ from own C value.
  Explicit cast: same bits both languages, no CodeQL finding (query skips
  explicit conversions).
- **Locals of `barten_csf_tools.h` = `const`** (18, fork-only; C++
  `misc-const-correctness`, sycl tidy baseline 18 -> 0). Sync: keep.
- **Metal copy.** `metal/float_adm_metal.mm::fadm_dwt_quant_step()`:
  exponent in named `float`, then `pow(10.0, (double)exponent)`. CPU
  statement changes -> copy, same PR. CUDA / HIP / SYCL `float_adm` hosts:
  no copy, call `adm_csf_rfactor_s()`.
- **Barten weights feed integer `adm` too** (`adm_csf_mode=1`,
  `integer_adm_kernels.h::adm_csf_factors()`, SYCL + Metal copies). Change
  here -> rerun `test_adm_csf_representable`, `test_barten_csf*`, integer
  ADM twin tests in Barten mode.
- **Guards.** `test_float_adm_csf_upstream`: step == float form (5
  geometries x 4 scales x 2 bands), double form differs;
  `linear_interpolate()` == float form, double slope differs; `barten_csf()`
  C == C++ on 168 values; on glibc step + Barten == bits of Netflix
  `cea2b4d8` build. `test_float_adm_csf_upstream_contract.py`: source
  shapes, planted regressions, Metal copy.
- **Residual vs Netflix on x86 = division only** (ADR-1442: fork divides,
  Netflix multiplies by `RCPSS` estimate). Netflix built with plain quotient
  == fork `float_adm`.
- **Upstream changes formula, tables or Barten parameters:** port here + Metal
  copy, regenerate both bit tables of C test from upstream build.
- Integer ADM step: `adm-quant-step.md`.
