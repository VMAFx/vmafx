---
paths:
  - core/test/float_bits.h
  - core/test/test_float_bits.c
  - core/test/*_twin_parity.h
  - core/test/test_metal_*_parity.c
  - core/test/test_speed_upstream_form.c
invariant: Exact results are compared with float_bits.h, never `==` or new memcpy copy; test variants keep static bodies equal.
---
<!-- markdownlint-disable MD013 -->
# Bit identity of floating-point results (ADR-1502)

test that asserts result has another computation's bits (twin against
CPU extractor, replay against reference, recorded value) uses
`core/test/float_bits.h`:

- `vmaf_test_identical_f64(a, b)` / `_f32`: same bit pattern and not NaN.
  It is strictly stronger than `==` (a ±0 mismatch fails, NaN still fails),
  so swapping it in never weakens test. Do not "simplify" it back to `==`,
  to `memcmp()`, or to bit compare without NaN rule:
  `test_float_bits` fails for each of those.
- `vmaf_test_expect_identical_f64(what, a, b)` / `_f32`: same, and one
  line with both values at `%.17g` and their bits on mismatch. Use it where
  assertion prints nothing of its own.
- test that shows two forms differ (fixture tells fork's form from
  wrong one) negates helper. comparison with constant (sentinel,
  literal) stays `==`; CodeQL `cpp/equality-on-floats` does not report it.
- Do not add private `memcpy` bit helper to test. 40 files still carry
  one from before ADR-1502; convert file's copy when you edit file.

test source compiled into two meson variants (for example
`test_speed_upstream_form` and its `_foreign_libm` twin) must keep body
of every static function same in both: put difference in
file-scope constant, not in `#if` inside function. CodeQL keys static
function by its body, so `#if` gives two variants two functions of one
name, and it reports one merged `run_tests()` does not call as
unreachable (`cpp/unused-static-function`, alert 1367).
