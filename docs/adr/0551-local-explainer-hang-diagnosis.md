<!-- markdownlint-disable MD013 MD060 -->
# ADR-0551: VCQ-223 LocalExplainer CI timeout — root cause and fix path

- **Status**: Proposed
- **Date**: 2026-05-18
- **Deciders**: lusoris, Claude (Anthropic)
- **Tags**: `python`, `test`, `local-explainer`, `performance`, `bugfix`, `fork-local`

## Context

`python/test/local_explainer_test.py:252`
(`QualityRunnerTest::test_run_vmaf_runner_local_explainer_with_bootstrap_model`) was
skipped with `@unittest.skip("[VCQ-223] FIXME: This test hangs and times out CI.")` in
commit `e3827e4dd` (2026-05-06, "python/test: adopt MyTestCase and port new tests in
asset, bootstrap, and local explainer test files"). The skip comment attributed the hang
to an unspecified cause. `docs/state.md` opened tracking item
**T-VCQ-223-LOCAL-EXPLAINER-HANG** with the hypothesis that a condition variable or
subprocess exit was involved.

This ADR documents the results of a systematic investigation (see
[research-0551](../research/0551-local-explainer-hang.md)) and proposes a fix path.

### What the test does

The test creates a `VmafQualityRunnerWithLocalExplainer` with the
`vmafplus_v0.5.2_test.json` model but passes **no `optional_dict2` argument**, so the
runner falls back to constructing `LocalExplainer()` internally with **default
parameters** (`neighbor_samples=5000`). It then calls `runner.run()` over two assets
of the 576×324, 48-frame YUV fixture.

### The hang root cause

The hang is **not a deadlock, condition-variable starvation, stdin read, subprocess hang,
or thread join issue**. It is a **CPU-bound computation timeout** caused by the product
of four large multiplicands:

| Factor | Value |
|---|---|
| Frames per asset (YUV fixture) | 48 |
| Assets | 2 |
| `neighbor_samples + 1` (rows per `_predict` call) | 5 001 |
| libsvm `svm_predict_values` loop (Python-C boundary per row) | 5 001 iterations |

Total native-C `svm_predict_values` calls: **2 × 48 × 5 001 = 480 096**

Each call is a Python-C boundary round-trip into libsvm's LIBSVR kernel evaluator
(210 support vectors, RBF kernel). On the test machine this takes approximately
4–8 minutes wall time — enough to exceed both the local `--timeout=30` gate and the
CI runner's default timeout.

The same code path in `test_explain_vmaf_results` (the non-skip sibling test) uses
`LocalExplainer(neighbor_samples=100)`, producing only 2 × 48 × 101 = 9 696 calls —
roughly 50× fewer — which completes in seconds.

The stack trace at hang time (confirmed via `SIGALRM`-driven traceback):

```text
runner.run()
  executor.py: _run_on_asset
    quality_runner_extra.py: exps = explainer.explain(model, xs)
      local_explainer.py: ys_label_pred_neighbor = train_test_model._predict(model, xs_2d_neighbor)
        train_test_model.py: score, _, _ = svmutil.svm_predict([0]*len(f), f, model)
          svmutil.py: label = libsvm.svm_predict_values(m, xi, dec_values)  <-- spinning here
```

### Why `fifo_mode=False` did not trigger a pipe stall

The commit that introduced the skip also changed `fifo_mode=True` → `fifo_mode=False`,
which eliminates any FIFO/pipe stall root cause. The hang was already present with
`fifo_mode=True` (that was the upstream state), but for different reasons. Post the
`fifo_mode=False` change, the only remaining cause is the computation volume.

## Decision

The fix for the implementation is to pass an explicit
`optional_dict2={"explainer": LocalExplainer(neighbor_samples=100)}` in the test, or
alternatively to lower `LocalExplainer.DEFAULT_NEIGHBOR_SAMPLES` from 5 000 to a value
safe for test use and document that the default is intended for production use only.

The **preferred fix** (< 20-line diff, safe for a single follow-up PR) is:

```python
# in test_run_vmaf_runner_local_explainer_with_bootstrap_model:
optional_dict2={"explainer": LocalExplainer(neighbor_samples=100)},
```

This matches the pattern in `test_explain_vmaf_results` (line 95) which passes the same
`neighbor_samples=100` to make the test tractable. The fix:

1. Removes the `@unittest.skip` decorator.
2. Adds `optional_dict2={"explainer": LocalExplainer(neighbor_samples=100)}` to the
   runner constructor call.
3. Updates the score assertions to the values produced by `neighbor_samples=100`
   (requires a single local run to capture).

The `@unittest.skip` decorator is **not removed in this PR** — that is deferred to the
follow-up fix PR so CI can confirm the scores before the skip is lifted.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Lower `LocalExplainer.DEFAULT_NEIGHBOR_SAMPLES` to 100 | Single-point change | Breaks production use where 5 000 samples are expected for stable explanations | Not chosen — the default is part of the public API surface |
| Add a `--fast` / `CI_MODE` envvar guard in the test | Allows both performance modes | Adds test complexity; obscures the real production behavior | Not chosen — test parametrization via `optional_dict2` is the existing pattern |
| Skip the test permanently | Zero effort | Leaves a known-good code path unexercised in CI | Rejected — the code path is valuable to test |
| Use `pytest-timeout` with a higher limit | Avoids code change | Would still run 8 minutes on slow runners; flaky on resource-constrained CI | Not chosen — root cause should be fixed, not worked around |

## Consequences

- **Positive**: The `QualityRunnerTest::test_run_vmaf_runner_local_explainer_with_bootstrap_model`
  test will run and complete in < 5 s. The `LocalExplainer` + `VmafQualityRunnerWithLocalExplainer`
  code path will be exercised in CI.
- **Negative**: The follow-up fix PR must capture the correct score assertions at
  `neighbor_samples=100` (which differ from the `neighbor_samples=5000` values in the
  skip-decorated test's comments).
- **Neutral / follow-ups**: The `@unittest.skip` remains in place until the follow-up
  PR lands. The `docs/state.md` row T-VCQ-223-LOCAL-EXPLAINER-HANG is updated from
  "root cause unknown" to "root cause identified — computation timeout; fix proposed."

## References

- Research digest: [research-0551](../research/0551-local-explainer-hang.md)
- Skip introduced: commit `e3827e4dd` (2026-05-06)
- Related test (passing sibling): `LocalExplainerTest::test_explain_vmaf_results`
  (`python/test/local_explainer_test.py:85`) — uses `neighbor_samples=100`
- `VmafQualityRunnerWithLocalExplainer._run_on_asset`:
  `python/vmaf/core/quality_runner_extra.py:25–45`
- `LocalExplainer.__init__` default: `python/vmaf/core/local_explainer.py:40–62`
- `LibsvmNusvrTrainTestModel._predict` (the bottleneck):
  `python/vmaf/core/train_test_model.py:1184–1192`
- `docs/state.md` tracking item: T-VCQ-223-LOCAL-EXPLAINER-HANG
- req: per the agent brief "research only, do NOT fix yet — open a sharp PR with the diagnosis + a proposed fix sketch"
