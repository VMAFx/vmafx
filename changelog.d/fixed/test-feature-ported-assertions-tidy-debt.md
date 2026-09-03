- **The seven assertions ported into `core/test/test_feature.cpp` by PR #1219
  raised that file's clang-tidy debt from 9 warnings to 34.** The port
  (ADR-1153, rescuing the dead C twin's unique coverage before deleting it)
  carried the twin's C idioms across into a C++ translation unit: `NULL`
  instead of `nullptr`, `typedef struct` instead of a plain `struct`,
  `{0}` option-table sentinels instead of `{}`, and file-scope `static`
  test functions. The whole-tree ratchet (ADR-1142) accepted the increase
  because #1219's baseline was regenerated after the port rather than
  measured against its merge base, so the regression rode into `master`
  unflagged. The file is now at **0 warnings** — below its pre-#1219 level —
  and its baseline entry is removed rather than raised. `nullptr` is correct
  here and does not contradict ADR-1138, whose `NULL` rule is scoped to **C**
  translation units for MSVC `/std:clatest` reasons; this is a C++ TU.
  The pass also closed nine `clang-analyzer-unix.Malloc` leak paths that only
  became visible once the 122-line test function was split: `mu_assert`
  expands to an early `return`, so every assertion made while a heap pointer
  was live leaked it on failure. Each case now frees before it asserts, and
  the comparisons are null-guarded, so a `nullptr` return from
  `vmaf_feature_name_from_options()` reports a failed assertion instead of
  dereferencing. No assertion was weakened, added or removed; the seven
  ported behaviours are still covered, now across seven named test cases
  instead of one oversized one.
