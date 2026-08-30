- **`set_option_double` in `core/src/opt.c` silently accepted `NaN`
  as a valid feature parameter.** IEEE 754 ordered comparisons with NaN
  always evaluate to false, so the bounds check `n < min` and `n > max`
  both returned false for any NaN input produced by `strtod("nan", …)`.
  The value was then stored unmodified, propagating NaN through every
  downstream `powf(area * NaN, 1/3)` call in the ADM noise-floor
  computation and silently producing `null` scores in the JSON output.
  An explicit `isnan(n)` guard is now inserted before the bounds check,
  returning `-EINVAL` on any non-finite parse result that is not already
  caught by `errno == ERANGE` or the finite bounds. Two regression tests
  (`test_double_nan_is_rejected`, `test_double_inf_rejected_when_max_finite`)
  added to `core/test/test_opt.c`. Surfaced by round-8 bug-hunt,
  angle 5 — negative test for ADM noise_weight extreme values
  (T-ROUND8-OPT-NAN-BYPASS / CWE-704).
