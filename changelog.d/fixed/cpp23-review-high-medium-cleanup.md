### fix(core): cpp23 adversarial review — HIGH + MEDIUM findings cleanup

Addresses 2 HIGH and 10 MEDIUM findings from the adversarial code review of the
C→C++23 conversion wave (PRs #41–#58, review PR #78).

**HIGH fixes:**
- `log.cpp` (PR #45): Add `assert()` enforcing NUL-termination on `string_view::data()`
  before passing to `fprintf %s` (CERT STR32-C, finding #7).
- `mkdirp.cpp` (PR #51): Replace recursive `mkdirp()` with an iterative prefix-walk
  bounded by path length (Power of 10 #1, finding #11).

**MEDIUM fixes:**
- `opt.h` (PR #43): Add `[[nodiscard]]` to `vmaf_option_set` header declaration so C++
  TUs see the attribute (CERT ERR33-C, finding #4).
- `fex_ctx_vector.cpp` (PR #44): Remove dead try/catch + abandoned vector from
  `feature_extractor_vector_init`; add `SIZE_MAX` overflow guard before capacity
  doubling (Power of 10 #3/#2, findings #5/#6).
- `dict.cpp` + `test_dict.cpp` (PR #48): Extract `isnumeric` to `dict_internal.h`
  as `inline`; replace `#include "dict.cpp"` ODR risk in test (Power of 10 #8, finding #10).
- `luminance_tools.cpp` (PR #51): Remove redundant `static` inside anonymous namespace
  (-Wredundant-decls, finding #12).
- `feature_name.cpp` (PR #54): Check `vmaf_dictionary_copy` return value before
  dereferencing result (CERT MEM31-C, finding #15).
- `picture_copy.cpp` (PR #54): Document intentional negative-stride semantics (finding #14).
- `output.cpp` (PR #56): Log warning when `LocaleGuard` locale-push fails; add
  `static_assert` on `pool_method_name` array size (findings #16/#17).
- `cpu.cpp` (PR #58): Add `constinit` to `g_flags` / `g_flags_mask` atomics to prevent
  static-initialisation-order fiasco (C++20/23, finding #19).

Rollup PR targeting master; rebases trivially after the cpp23 PRs land.
