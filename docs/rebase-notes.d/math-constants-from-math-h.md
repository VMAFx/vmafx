## Netflix/vmaf 4e150067b (+ a8f536b9a): M_PI / M_E from <math.h> (2026-10-08)

- `core/meson.build`: `-D_USE_MATH_DEFINES` as a C and C++ project argument
  and in `test_args` on Windows hosts, beside `_GNU_SOURCE` (Linux) and
  `_DARWIN_C_SOURCE` (macOS). Upstream adds it for `cc.get_id() == 'msvc'`
  only; the fork also needs it for clang-cl, icx-cl and MinGW-w64 (whose
  `<math.h>` hides the constants under `__STRICT_ANSI__`, set by `-std=c23`).
- Removed every `#ifndef M_PI` / `#ifndef M_E` fallback and every in-file
  `#define _USE_MATH_DEFINES`: `adm_csf_tools.h`, `adm_tools.c`, `adm_tools.h`,
  `barten_csf_tools.h`, `ciede.c`, `integer_adm.h`, `integer_ssim.c`,
  `speed.c`, `speed_internal.c`, `speed_qa.c`, `vif_tools.c`,
  `y_funque_plus.c`, `sycl/float_adm_sycl.cpp`, `sycl/integer_adm_sycl.cpp`,
  and the tests `test_adm_angle_flag.c`, `test_float_adm_csf_upstream.c`,
  `test_integer_adm_quant_step.c`, `test_speed_upstream_form.c`. Upstream's
  `a8f536b9a` (guards) is superseded by this. **On sync**: delete a fallback a
  sync brings back (`core/src/feature/AGENTS.d/math-constants.md`).
