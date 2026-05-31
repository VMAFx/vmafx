# Research: NaN Propagation Guards in ai/src/vmaf_train — Round-25 Audit

**Date**: 2026-05-31
**ADR**: [ADR-0963](../adr/0963-ai-nan-propagation-guards-round25.md)
**Scope**: `ai/src/vmaf_train/eval.py`, `ai/src/vmaf_train/tune.py`

## Summary

Round-25 audit identified two paths through the `vmaf_train` Python package
where NaN values propagate silently into downstream consumers, causing
incorrect behavior without any diagnostic signal. Both paths were fixed in the
same PR.

## Finding C.1 — `eval.correlations` degenerate input handling

### Root cause

`correlations(pred, target)` called `pearsonr` and `spearmanr` directly
without validating:

1. **Empty arrays**: `pearsonr(np.zeros(0), np.zeros(0))` raises or returns
   NaN (depends on scipy version). `np.mean()` on an empty array returns NaN
   with a runtime warning. The resulting `EvalReport` contained `plcc=NaN,
   srocc=NaN` with no error raised.
2. **Constant arrays**: `pearsonr(np.ones(N), x)` returns NaN because the
   correlation coefficient requires non-zero variance in both inputs. This can
   happen during a degenerate training run where the model collapses to
   predicting a constant.

### Impact on bisect_model_quality

`bisect_model_quality._gate` evaluates `report.plcc >= value`. IEEE-754
specifies that any comparison involving NaN returns False. Therefore:

- A degenerate model (constant output) produces `report.plcc = NaN`.
- `NaN >= 0.9` → False, model is marked as "failing the gate".
- `NaN >= 0.0` → False, even a zero threshold fails.
- The bisect concludes "first model already bad; nothing to bisect" for every
  run, regardless of actual model quality.

This is a silent wrong answer: the bisect returns a definitive-sounding
verdict with no indication that the evaluation itself was degenerate.

### Decision rationale: 0.0 vs NaN for degenerate correlation

Two options were considered:

- Return `NaN` and require callers to check `np.isnan(report.plcc)`.
- Return `0.0` as the worst-case sentinel.

The `0.0` choice is justified because:

1. All current gate comparisons in the codebase use `>=` or `<=`, not
   equality. `0.0 >= 0.9` is False (model fails gate, expected), while
   `NaN >= 0.9` is also False but without indicating why.
2. Returning `0.0` does not NaN-propagate through arithmetic that callers
   might do with the value (e.g., computing a mean PLCC over a LOSO fold).
3. The `warnings.warn(RuntimeWarning)` call makes the degenerate condition
   visible to operators without requiring callers to change their comparison
   logic.

A `ValueError` is raised for the empty-input case (rather than returning 0.0)
because empty inputs are a data-pipeline bug — not a degenerate training
result — and should halt execution at the call site.

## Finding C.2 — `tune.objective` all-NaN metric handling

### Root cause

When a training run diverges from epoch 0 (gradient explosion, wrong
learning rate, etc.), Lightning may write `NaN` to every row of the
`val/mse` column in `metrics.csv`. The expression:

```python
float(df["val/mse"].dropna().min())
```

produces `float(NaN)` when `dropna()` returns an empty series (all values
were NaN, so all were dropped, leaving an empty series whose `.min()` is NaN).

Optuna receives `NaN` as the trial objective value. The Optuna `Study` object
stores it as a completed trial. `study.best_trial` then compares NaN against
other trial values — in practice, `study.best_value` becomes NaN and remains
NaN for all subsequent trials, corrupting the entire sweep's best-trial
tracking.

### Reproducer (pre-fix)

```python
import pandas as pd, numpy as np
df = pd.DataFrame({"val/mse": [float("nan"), float("nan")]})
result = float(df["val/mse"].dropna().min())
print(result)          # nan — this was passed to Optuna
print(result >= 0.5)   # False — NaN comparison
print(np.isnan(result))  # True — confirms the bug
```

### Fix

`_read_best_metric(df, col)` checks whether `dropna()` produces an empty
series (or if the minimum is NaN after dropping), and returns `float("inf")`
in that case. `float("inf")` is the unambiguous worst-case value for a
minimization study and will not corrupt Optuna's best-trial tracking.

The `WARNING` log message includes enough context for an operator to identify
the diverged training run and investigate the root cause.

### Note on study direction

The current `sweep()` function hardcodes `direction="minimize"`. The
`_read_best_metric` helper returns `float("inf")` unconditionally. If a future
`maximize` direction is added, the helper will need to return `float("-inf")`
for that direction. The source comment and this digest document the assumption.

## Verification

All 19 new tests pass under `pytest` with `PYTHONPATH=ai/src`. The two
pre-existing callers of `correlations()` (`bisect_model_quality` and
`cli.py`) are not affected: `bisect_model_quality` benefits directly from the
fix, and `cli.py` evaluates real ONNX models that produce non-degenerate
output in normal operation.
