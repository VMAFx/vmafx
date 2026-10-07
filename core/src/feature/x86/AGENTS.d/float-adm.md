---
paths:
  - core/src/feature/x86/float_adm_avx2.c
  - core/src/feature/x86/float_adm_avx512.c
  - core/src/feature/adm_tools.c
  - core/src/feature/adm.c
invariant: Float ADM x86 kernels return scalar bits; sums start at +0, multiply then add, CSF filter in double.
---
# Float ADM x86 kernels

State 2026-10-02 (ADR-1473): wavelet + CSF kernels dispatched from `adm.c`
(`adm_dwt2_dispatch()`, `adm_csf_plane_select()`) by `vmaf_get_cpu_flags()`;
`--cpumask 63` scalar, `48` AVX2, `0` AVX-512. Scores identical on all three.
Libraries `x86_float_adm_avx2` / `x86_float_adm_avx512` build with
`vmaf_strict_fp_args`.

| Kernel | Reference | Must match |
| --- | --- | --- |
| `float_adm_dwt2_avx2()` / `_avx512()` | `adm_dwt2_s()` | every band element, return value (`-ENOMEM`) |
| `float_adm_csf_avx2()` / `_avx512()` | `adm_csf_plane_s()` | `dst` and `flt`, nothing outside `w` x `h` |

Rules:

- Four-tap sum = `dwt2_tap4()` / `dwt2_tap4_avx2()` / `dwt2_tap4_avx512()`
  only: start at `+0`, add one product per step, tap order 0..3. Starting at
  first product returns `-0` where scalar returns `+0`.
- Vector first step = `dwt2_plus_zero_avx2()` / `dwt2_plus_zero_avx512()`
  (compare `_CMP_NEQ_UQ` + mask), never `_mm*_add_ps(_mm*_setzero_ps(), p)`:
  MSVC 19.51 drops intrinsic `+0 +` even under `/fp:precise`, keeps scalar
  `+0 +` -> `-0` vs `+0`. GCC / Clang keep both; only MSVC lanes show defect
  (`Windows MSVC+CUDA (full)` runs test, Windows tester zip).
  `T-MSVC-FLOAT-ADM-X86-TEST-FAILS-2026-10-04`.
- Multiply, then add. No `_mm*_fmadd_ps`: scalar `adm_dwt2_s()` carries
  contraction guard (ADR-1057).
- Horizontal vector loop: taps `tmp[2j - 1 .. 2j + 2]` = what `ind_x` holds
  without reflection. Bound `2 * j + 16 < w` (AVX2) / `2 * j + 32 < w`
  (AVX-512) keeps last tap inside row; column 0 and row's end go
  through `dwt2_horizontal_scalar()` + `ind_x`.
- CSF `flt` = double product narrowed to float (`FLOAT_ONE_BY_30` is double
  literal in `adm_tools.c`). Float product differs on 0.9 % of values.
  `adm_csf_plane_s()` keeps upstream's statement verbatim (twin contract tests
  pin it) and ignores its `one_by_30` argument; kernels use argument.
- No reduction kernels. `adm_csf_den_scale_s()` adds fp32 in column order;
  kernel with another order changes golden-gated bits. Removed kernels
  (`float_adm_csf_den_scale_*`, `float_adm_sum_cube_*`) stay removed unless
  reference changes (`T-FLOAT-ADM-X86-SCALAR-STAGES-2026-10-02`).
- NaN: sign / payload of NaN result may differ from scalar when two NaNs
  meet (operand order). Test compares NaN positions only. Not defect.

Guard: `core/test/test_float_adm_x86.c` (byte compare, stride padding
included, sizes 17..576, signed zeros, infinities, denormals; `compute_adm()`
across three dispatch levels). 17 planted changes fail it. Change kernel ->
run it, then Netflix golden gate.
