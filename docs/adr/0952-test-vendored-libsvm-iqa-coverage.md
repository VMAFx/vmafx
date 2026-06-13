<!-- markdownlint-disable MD060 -->
# ADR-0952: Push test coverage on vendored libsvm + IQA paths the fork uses

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris, Claude (agent run)
- **Tags**: test, coverage, vendored, security, libsvm, iqa

## Context

The fork ships three vendored C/C++ trees that the production VMAF
pipeline depends on every frame:

1. `core/src/svm.cpp` — vendored libsvm 3.24 (Chih-Chung Chang / Chih-Jen
   Lin). Drives the VMAF score regressor.
2. `core/src/feature/iqa/` — vendored tdistler.com IQA helpers
   (`math_utils.c`, `decimate.c`, `convolve.c`, `ssim_tools.c`) with a
   2016 Netflix update. Drives SSIM / MS-SSIM scoring.

Both trees were wrapped in `NOLINTBEGIN/NOLINTEND` cordons so the
fork's lint-clean rule (CLAUDE.md §12 r12) does not re-flow the
vendored body. PR #381 (ADR-0889) added a libsvm vendored-audit pass
plus 9 regression tests in `core/test/test_svm_parser.c` that pin the
**parser-rejection** paths (oversized header counts, missing
`nr_class` before SV-bearing sections, unknown svm types). After that
PR the coverage breakdown on `core/` master was:

| File                                  | Lines | Cover | Functions covered |
|---------------------------------------|------:|------:|------------------:|
| `core/src/svm.cpp`                    |  1682 |  9.6% | 13 / 95           |
| `core/src/feature/iqa/convolve.c`     |   102 | 41.2% | partial           |
| `core/src/feature/iqa/decimate.c`     |    13 |  0.0% | 0                 |
| `core/src/feature/iqa/math_utils.c`   |    21 |  0.0% | 0                 |
| `core/src/feature/iqa/ssim_tools.c`   |   101 |  0.0% | 0                 |

The libsvm gap is the entire **runtime side** of the library —
`svm_train`, `svm_predict`, `svm_predict_values`,
`svm_predict_probability`, `svm_check_parameter`, `svm_save_model`,
the inspector family (`svm_get_*`). The IQA gap is the entire
**scalar-helper surface**: the existing `test_iqa_convolve.c` runs
SIMD-vs-scalar bit-exactness on `iqa_convolve` only and skips when no
SIMD is present, leaving `iqa_filter_pixel`, `iqa_img_filter`,
`iqa_decimate`, the three `KBND_*` border handlers, and `iqa_ssim`
itself unmeasured.

Both surfaces are user-visible (any model load, any SSIM frame) and
vendored — they cannot be refactored, so the only defence against
silent breakage during an upstream re-pin or a sanitizer-find
follow-up is *observation tests*.

## Decision

Add two new fast-suite test executables that are observation-only
against the vendored APIs:

- `core/test/test_svm_api.c` — drives `svm_train` on a tiny linearly
  separable 2-class fixture, exercises `svm_check_parameter`'s
  rejection branches not already touched by PR #381's parser tests,
  pins all inspector outputs, calls `svm_predict` /
  `svm_predict_values` / `svm_predict_probability`, trains an
  `EPSILON_SVR` model to drive the regression branch +
  `svm_get_svr_probability`, and round-trips a model through
  `svm_save_model` / `svm_load_model`.
- `core/test/test_iqa_helpers.c` — drives every public symbol in
  `math_utils.c`, every border handler in `convolve.c`,
  `iqa_filter_pixel` (NULL-kernel + interior + edge-replicate),
  `iqa_img_filter` (writes-result + reject-no-bnd_opt + in-place),
  `iqa_decimate` (factor=2 no-kernel + in-place + odd-dimension), and
  drives `iqa_ssim` end-to-end on identical and on randomised frames
  to hit `ssim_tools.c`'s scalar precompute / variance / accumulate
  fallback paths.

Neither file modifies any vendored source. The libsvm
`NOLINTBEGIN/NOLINTEND` cordon and the IQA `tdistler.com` header
attribution stay byte-identical.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Extend `test_svm_parser.c` from PR #381 | Single file to maintain | PR #381 is in flight (not merged); editing the same file would create a merge conflict; parser tests and runtime tests have different concerns | Separate file isolates concerns and lands independently |
| Coverage-target the iqa scalar paths via an existing benchmark | No new code | Existing benchmarks don't observe correctness; coverage is incidental, not a gate | Need explicit assertions, not just execution |
| Fold all 8 svm + 21 iqa tests into one mega-file | Smallest meson churn | Different link surfaces (svm needs only `libsvm_static_lib + thread_locale`; iqa needs the full feature lib + cpu lib + dnn shim) | Forced into two separate executables by link topology |
| Skip the test pass; rely on the Netflix golden gate | Zero work | Golden gate runs only on the 3 canonical YUV pairs; it does not exercise the trained-and-predict round-trip on libsvm or the scalar fallback paths of iqa | Observed coverage delta (≈14% → ≈74% aggregate) shows the golden gate was missing huge swaths |

## Consequences

**Positive**:

- libsvm 3.24 runtime coverage lifts from **9.6% → 71% lines** /
  **13.7% → ≈80% functions**. The Platt-scaling probability path,
  decision-function construction, and the save/load round-trip are
  now pinned regression assertions.
- IQA helper coverage lifts from **0–41% → 84–100% lines** across
  `math_utils.c`, `decimate.c`, `convolve.c`, `ssim_tools.c`. Every
  border-handler edge case (positive overshoot, negative reflect,
  constant fall-through) is asserted.
- Aggregate coverage across the vendored libsvm + iqa surface:
  **≈14% → 74% lines**.
- Future upstream re-pin (libsvm 3.36, IQA fork bump) lands against
  29 new assertions that surface behavioural drift immediately,
  instead of waiting for an integration-level SSIM/VMAF anomaly to
  bisect.

**Negative**:

- Two new fast-suite executables to keep building. Per-test cost is
  <20 ms each; aggregate impact on `meson test --suite=fast` is below
  noise.
- The `test_iqa_helpers.c` build pulls the same DNN shim sources as
  the other iqa-feature tests; if `dnn_sources` is split in the
  future, this file follows.

**Neutral / follow-ups**:

- The two test files become a template for future vendored-surface
  audits (cJSON, pdjson, libsvm major version bump). They prove the
  observation-only pattern works without touching the vendored body.
- The `_round()` and `_cmp_float()` asymmetry tests double as
  behavioural documentation — the rounding rule is "trunc toward
  zero, add sign when |frac| >= 0.5", asymmetric across zero. Any
  future refactor that "fixes" this to symmetric IEEE-754 round-half-
  to-even will trip the test, surfacing the unintended numerical
  change.

## References

- ADR-0889 (in flight, PR #381) — companion vendored libsvm audit
  that this file extends from the parser side to the runtime side.
  This ADR (0952) intentionally does not modify the parser tests
  introduced by ADR-0889; the two sets are designed to land
  independently and merge cleanly in either order.
- [ADR-0138](0138-iqa-convolve-avx2-bitexact-double.md) — IQA SIMD
  bit-exact contract that the existing `test_iqa_convolve.c` already
  pins.
- [ADR-0700](0700-vmafx-repo-layout.md) — `libvmaf/` → `core/`
  layout used by the meson wiring.
- `core/src/AGENTS.md` §10 — vendored libsvm fork-patch invariants
  that this test file leaves untouched.
- Related PRs: `#381` (libsvm parser audit), this PR.
- Source: `req` — operator-provided task brief, 2026-05-31:
  "Push test coverage on vendored libsvm (svm.cpp) + iqa paths that
  the fork actively uses."
