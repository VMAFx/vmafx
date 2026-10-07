---
paths:
  - core/src/feature/ciede.c
  - core/src/feature/cuda/integer_ciede/ciede_device.h
  - core/src/feature/ciede_ff_math.h
  - core/test/test_ciede_upstream_products.c
invariant: ciede.c squares = products (ADR-1467); its chroma and rotation products = float, no double cast (ADR-1476).
---
<!-- markdownlint-disable MD013 -->
# `ciede.c` writes squares as products (ADR-1467)

- Upstream: `powf(degrees, 2)` in `get_r_sub_t()`, `pow(x, 2)` 13 times in `ciede2000()`. Fork: `degrees * degrees`, `square(x)` (`static double square(const float x)`).
- Why: clang folds `powf(x, 2)` / `pow(x, 2)` to product, GCC calls libm. glibc `powf(x, 2)` != product on 0.12 % of arguments (ties) -> GCC build != clang build, 65 of 180 frames, up to 2.0e-11. Product = correctly rounded on every compiler and libm.
- `square(float)`: fp64 product of float, exact (48 bits). Argument stays float: `l_bar - 50` evaluates in float, like upstream `pow()` argument. Square of double expression = other case, measure first.
- Upstream sync: keep fork side. Reverting to `powf(degrees, 2)` -> `test_ciede_device_math` fails under GCC (640x360 16-bit case).
- Twins already product: `ciede_device.h::ciede_r_sub_t()` (`-(degrees * degrees)`), `ciede_sq()`; `ciede_ff_math.h::r_sub_t()`, `sq()`. Change one -> change all, same PR. Pinned lines: `core/test/test_sycl_ciede_exact_contract.py`, `core/test/test_cuda_ciede_exact_contract.py`.
- Two float products = upstream's, no cast (ADR-1476; Netflix `libvmaf/src/feature/ciede.c:224-225`, `:235-236`): `sqrt(c_prime_1 * c_prime_2)` and `+ r_sub_t * chroma * hue` round to float BEFORE fp64 expression widens them. PR #552 (CodeQL sweep) cast first operand to `double`: 265 of 327 measured frames off upstream, up to 1.3e-9. Never cast operand (`(double)c_prime_1 * c_prime_2`). CodeQL's `cpp/integer-multiplication-cast-to-long` is answered by converting product's RESULT explicitly, `sqrt((double)(c_prime_1 * c_prime_2))` and `+ (double)(r_sub_t * chroma * hue)`: same object code as implicit form (checked with `objdump`), and query reports only implicit widenings. Upstream sync: take upstream's side of these two expressions.
- Twins carry same two float products: `ciede_device.h` (`chroma_product`, `rotation`), `ciede_ff_math.h` (`from_float(c_prime_1 * c_prime_2)`, `rotation * chroma * hue` into `add_f`). Not `two_prod()` / `mul_f(two_prod())`: that is exact product. Metal kernel is fp32 throughout, already float products.
- Guards for products: `test_ciede_upstream_products` (replay of `ciede_delta_e()` on header's helpers, float vs widened, 20 000 colour pairs: header == float form), `test_ciede_device_math` (header == CPU, bit for bit), both contract tests (pinned lines + planted `(double)`). cast in `ciede.c` alone fails second, in both first.
- Left as libm call on purpose: `powf(x, 7)` (no compiler folds; glibc not correctly rounded on 0.07 % -> whole CPU-vs-twin residual, `T-CUDA-CIEDE-LIBM-RESIDUAL-2026-10-01`), `pow(x, 7)`, `pow(x, 2.4)`, `pow(x, 1.0 / 3.0)`.
- gates: `python3 scripts/ci/run_meson_test.py -- -C build test_ciede_device_math test_ciede test_ciede_upstream_products test_sycl_ciede_exact_contract test_cuda_ciede_exact_contract`; two builds: `diff` of `--feature ciede --precision max` reports, GCC vs clang.
