<!-- markdownlint-disable MD013 MD060 -->
# Research digest — Vendored libsvm + IQA coverage uplift (ADR-0952)

- **Date**: 2026-05-31
- **Branch**: `test/vendored-libsvm-iqa-coverage`
- **ADR**: [ADR-0952](../adr/0952-test-vendored-libsvm-iqa-coverage.md)
- **Companion PR**: #381 (libsvm parser audit, ADR-0889) — in flight

## 1. Trigger

Operator task brief (2026-05-31, verbatim translated to English):

> Push test coverage on vendored `libsvm` (svm.cpp) + `iqa` paths that
> the fork actively uses. PR #381 (libsvm audit added 9 tests via
> `test_svm_parser.c`) already covers parser-rejection branches; do
> not overlap. Add 5–10 focused tests. Never modify upstream Netflix
> vendored content semantically — only add coverage. Document why if
> a path can't be tested without modifying vendored code.

## 2. Method

1. Build a clean coverage-instrumented tree from the worktree:
   `meson setup build-cov core -Db_coverage=true -Denable_cuda=false -Denable_sycl=false`
2. Run the fast suite to baseline coverage:
   `meson test -C build-cov --suite=fast` (49 tests, all pass).
3. Collect per-file coverage with `gcovr`:
   `gcovr -r .. --print-summary --gcov-ignore-parse-errors=negative_hits.warn_once_per_file -f '.*svm\.cpp' -f '.*feature/iqa/'`
4. Inspect uncovered line ranges to identify which **functions** were
   uncovered (not just lines); narrow the test design to those.
5. Add tests; rebuild; clear `*.gcda`; re-run fast suite; re-measure.
6. Diff coverage and document the per-file delta.

## 3. Baseline (master @ `e81b13352`, before this PR)

| File                                  | Lines | Cover | Notes |
|---------------------------------------|------:|------:|-------|
| `core/src/svm.cpp`                    |  1682 |  9.6% | Only 13 / 95 functions hit. Parser-only via existing predict path in feature_collector tests. |
| `core/src/feature/iqa/convolve.c`     |   102 | 41.2% | Only `iqa_convolve` exercised (via `test_iqa_convolve.c` SIMD diff). `iqa_img_filter`, `iqa_filter_pixel`, `KBND_*` untouched. |
| `core/src/feature/iqa/decimate.c`     |    13 |  0.0% | `iqa_decimate` never called from any test. |
| `core/src/feature/iqa/math_utils.c`   |    21 |  0.0% | `_round`, `_max`, `_min`, `_cmp_float`, `_matrix_cmp` all unreferenced from tests. |
| `core/src/feature/iqa/ssim_tools.c`   |   101 |  0.0% | `iqa_ssim` only reachable from float-feature extractors; not exercised in any pure unit test. |

## 4. Gap analysis

### 4a. libsvm runtime (svm.cpp)

PR #381 adds `test_svm_parser.c` which targets **parser rejection**
only: the eight `if (!stream)` / `if (n > MAX_*)` early-return paths
plus `unknown svm_type`. That maxes out `svm_load_model` /
`svm_parse_model_from_buffer` failure coverage but does not touch:

- `svm_train` (the SMO solver + decision-function construction)
- `svm_predict`, `svm_predict_values`, `svm_predict_probability`
  (Platt scaling + sigmoid_predict + multiclass_probability)
- `svm_check_parameter` (~12 rejection branches, plus the NU-SVC
  feasibility scan)
- `svm_save_model` (write side of the parser's read-back contract)
- The inspector family: `svm_get_svm_type`, `svm_get_nr_class`,
  `svm_get_labels`, `svm_get_sv_indices`, `svm_get_nr_sv`,
  `svm_get_svr_probability`
- `svm_free_and_destroy_model` (nulls the pointer; observable
  invariant)

### 4b. IQA helpers

`test_iqa_convolve.c` runs SIMD-vs-scalar bit-exactness on
`iqa_convolve` only — and *skips entirely* when the host has neither
AVX2 nor AVX-512 nor NEON. So `iqa_convolve` could be at 41% on a
host with both AVX2 and AVX-512 and at 0% on an ARM CI runner that
isn't NEON-tagged. No other IQA helper has any direct test.

`iqa_ssim` is reached only by float-SSIM extractor smoke tests, which
all live under the `enable_float=true` gate and don't exercise the
function with controlled inputs whose `l/c/s` outputs can be
asserted.

## 5. Approach

Two new fast-suite test executables — observation-only against the
public API. No vendored source is modified:

### 5a. `core/test/test_svm_api.c` (8 tests)

- `test_check_param_accepts_default` — happy path on the linearly
  separable 2-class fixture.
- `test_check_param_rejects_unknown_svm_type` — bumps `svm_type` to
  99, asserts the "svm type" error string.
- `test_check_param_rejects_unknown_kernel` — same for `kernel_type`.
- `test_check_param_rejects_bad_numeric` — 10 separate sub-assertions
  for `cache_size<=0`, `eps<=0`, `C<=0`, `shrinking != 0/1`,
  `probability != 0/1`, RBF + `gamma<0`, POLY + `degree<0`,
  ONE_CLASS + `probability=1`, NU_SVC + `nu=0`, EPSILON_SVR +
  `p<0`.
- `test_train_csvc_inspectors_and_predict` — trains a C-SVC,
  inspects `svm_type`, `nr_class`, `nr_sv`, `labels`, `sv_indices`,
  `check_probability_model` (negative when probability=0),
  `svm_get_svr_probability` (returns 0 for classifier), predicts on
  side and − side, asserts `svm_predict == svm_predict_values`,
  drives `svm_free_and_destroy_model` and asserts pointer nulled.
- `test_predict_probability_csvc` — re-trains with `probability=1`,
  asserts `check_probability_model == 1`, calls
  `svm_predict_probability`, asserts probs sum to 1 and lie in [0,1].
- `test_train_epsilon_svr` — drives the SVR solver branch + the
  probA-only `check_probability_model` path + `svm_get_svr_probability`
  for the regressor.
- `test_save_load_roundtrip` — `svm_save_model` then
  `svm_load_model`; asserts inspector outputs match and predictions
  on the same query agree byte-for-byte.

### 5b. `core/test/test_iqa_helpers.c` (21 tests)

- 5 math_utils tests: `_round` (positive half + negative trunc-toward-
  zero asymmetry), `_min` / `_max`, `_cmp_float` (including
  rounding-by-scale boundary), `_matrix_cmp`.
- 5 KBND_* tests: in-bounds passthrough, negative reflect, positive
  reflect, replicate clamp, constant fallthrough.
- 6 filter tests: `iqa_filter_pixel` (NULL kernel + interior 3x3 box
  edge with REPLICATE border), `iqa_img_filter` (writes result,
  rejects NULL bnd_opt with rc=1, in-place when result==NULL).
- 3 decimate tests: factor-2 with no kernel + in-place + odd
  dimension (sw = w/factor + (w&1) — the half-pixel ceiling).
- 2 iqa_ssim tests: identical frames (ssim ≈ 1.0, all of l/c/s ≈ 1.0)
  random frames (asserts finite values + ssim < 0.99). These drive
  `ssim_tools.c`'s scalar precompute / variance / accumulate
  fallbacks (since no SIMD dispatch is installed in the test process).

## 6. Hypothesis check — _round() asymmetry

Initial test draft assumed `_round(-0.5) == -1` (round-away-from-zero
on .5). Built and ran; assertion failed. Constructed a standalone
verifier of the vendored body:

```c
int _round(float a) {
    int sign_a = a > 0.0f ? 1 : -1;
    return a - (int)a >= 0.5 ? (int)a + sign_a : (int)a;
}
```

For `a = -0.5`: `(int)a = 0`, `a - 0 = -0.5`, `-0.5 >= 0.5` is false,
so returns `(int)a = 0`. The function is asymmetric — it rounds away
from zero only on the positive side. Test was rewritten to lock the
observed behaviour in as a regression assertion; the rounding rule
becomes documented via the assertion.

Same hypothesise → check → act flow caught a wrong `_cmp_float(1.234,
1.235, 2)` expectation: `123.4` truncates to `123` but `123.5` adds
the sign and becomes `124`, so the two **differ** at digits=2, not
**agree**. Test was rewritten with a verified pair (`1.2 vs 1.21 @
digits=1`) for the equality case.

## 7. Result

Re-measured after the two test files land + 29 new assertions pass:

| File                                  | Baseline | After   | Δ       |
|---------------------------------------|---------:|--------:|--------:|
| `core/src/svm.cpp`                    |     9.6% |    71%  | +61 pp  |
| `core/src/feature/iqa/convolve.c`     |     41%  |   100%  | +59 pp  |
| `core/src/feature/iqa/decimate.c`     |      0%  |   100%  | +100 pp |
| `core/src/feature/iqa/math_utils.c`   |      0%  |   100%  | +100 pp |
| `core/src/feature/iqa/ssim_tools.c`   |      0%  |    84%  | +84 pp  |
| **Aggregate (svm.cpp + iqa)**         |   ≈14%   |   74%   | +60 pp  |
| **Aggregate functions covered**       |   <20%   |   78%   | +58 pp  |

Branch coverage on the same surface went from 8% to 54.6% — over half
of the branch arms in the vendored bodies are now exercised.

## 8. Untested-without-modifying-vendored

Two surfaces remain uncovered and would require modifying vendored
code to test:

1. **`svm_cross_validation`** — sits at ~30 lines, allocates a
   training subset and re-trains. Coverable without modifying
   vendored code, but the time cost (~5 retraining loops on a small
   fixture) pushes the test toward the `slow` suite. Defer to a
   follow-up.
2. **OOM allocator hooks inside `svm_train`** — `Malloc` /
   `realloc` failure paths are unreachable without an LD_PRELOAD
   shim or a vendored-side hook. Out of scope for an
   observation-only audit.
3. **`ssim_accumulate_lane.h`** SIMD-accumulator inline at 0% — only
   reached when `iqa_ssim_set_dispatch` installs a SIMD fn, which is
   gated by feature-build choices the test process doesn't pick up.
   Covered transitively when run inside the full extractor; not
   coverable from a standalone test without faking the dispatch.

## 9. Reproducer

```bash
git checkout test/vendored-libsvm-iqa-coverage
meson setup build-cov core -Db_coverage=true -Denable_cuda=false -Denable_sycl=false
ninja -C build-cov
meson test -C build-cov --suite=fast               # 51/51 pass
cd build-cov
gcovr -r .. --print-summary \
    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
    -f '.*svm\.cpp' -f '.*feature/iqa/'
```

## 10. References

- `req` — operator task brief, 2026-05-31 (full text §1 above).
- `core/src/AGENTS.md` §10 — vendored libsvm fork-patch invariants
  that this test pass leaves untouched.
- ADR-0889 (in flight via PR #381) — libsvm parser audit; this work
  complements it on the runtime side.
