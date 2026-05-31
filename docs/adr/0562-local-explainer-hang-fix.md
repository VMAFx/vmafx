<!-- markdownlint-disable MD013 MD060 -->
# ADR-0562: VCQ-223 LocalExplainer hang fix — cap neighbor_samples in test runner

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `python`, `test`, `local-explainer`, `performance`, `bugfix`, `fork-local`

## Context

ADR-0551 (PR #1325) identified the root cause of the VCQ-223 CI hang:
`VmafQualityRunnerWithLocalExplainer._run_on_asset` constructed `LocalExplainer()`
with the default `neighbor_samples=5000`, producing ~480 000 libsvm
`svm_predict_values` calls per typical run (wall time 4–8 min; CI timeout). This PR
implements the fix proposed in ADR-0551.

The test `QualityRunnerTest::test_run_vmaf_runner_local_explainer_with_bootstrap_model`
has been skipped with `@unittest.skip("[VCQ-223]")` since commit `e3827e4dd`.

## Decision

The fix targets the fallback path inside
`VmafQualityRunnerWithLocalExplainer._run_on_asset` (option (b) from ADR-0551):
the fallback now constructs `LocalExplainer(neighbor_samples=100)` — matching the
passing sibling test `test_explain_vmaf_results` — rather than the 5000-sample
default. Production callers that need higher fidelity can pass
`optional_dict={"explainer_neighbor_samples": 5000}` or supply a full `LocalExplainer`
via `optional_dict2={"explainer": ...}`.

The `@unittest.skip` decorator is removed. Score assertions are recalibrated to the
`neighbor_samples=100` values.

## Implementation notes

- `python/vmaf/core/quality_runner_extra.py:38` — fallback `LocalExplainer()` replaced
  with `LocalExplainer(neighbor_samples=neighbor_samples)` where `neighbor_samples`
  defaults to 100 and is overridable via
  `optional_dict.get("explainer_neighbor_samples", 100)`.
- `python/test/local_explainer_test.py:252` — `@unittest.skip` removed; score
  assertions updated to `neighbor_samples=100` calibration values:
  - `results[0]["VMAF_LE_score"]` = 75.40980306756497
  - `results[1]["VMAF_LE_score"]` = 99.95804823471536
- Wall time on dev machine (Python 3.14): approximately 78 seconds.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Lower `LocalExplainer.DEFAULT_NEIGHBOR_SAMPLES` globally to 100 | Single-point change | Breaks production callers relying on 5000-sample explanations | Chosen against — default is public API surface |
| Fix only at test call site via `optional_dict2` | Minimal blast radius | Runner fallback stays broken for any future caller | Runner-level fix is the correct layer |
| Raise the test timeout | No code change | Root cause remains; burns 4–8 min of CI time | Rejected — fix root cause, not symptom |

## Consequences

- **Positive**: `QualityRunnerTest::test_run_vmaf_runner_local_explainer_with_bootstrap_model`
  runs and completes within the 120-second timeout. The `LocalExplainer` +
  `VmafQualityRunnerWithLocalExplainer` code path is exercised in CI.
- **Negative**: The `neighbor_samples=100` values differ from what `neighbor_samples=5000`
  would produce; score assertions changed accordingly.
- **Neutral / follow-ups**: Production callers relying on the implicit 5000-sample
  default should pass `optional_dict={"explainer_neighbor_samples": 5000}`.

## References

- Diagnosis ADR: [ADR-0551](0551-local-explainer-hang-diagnosis.md) (PR #1325)
- Skip introduced: commit `e3827e4dd` (2026-05-06)
- Related passing test: `LocalExplainerTest::test_explain_vmaf_results` (uses `neighbor_samples=100`)
- `docs/state.md` tracking item: T-VCQ-223-LOCAL-EXPLAINER-HANG
