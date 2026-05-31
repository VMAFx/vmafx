<!-- markdownlint-disable MD060 -->
# ADR-0963: ai/src: guard NaN propagation in eval + tune (round-25 audit C.1 + C.2)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `ai`, `correctness`, `bisect`

## Context

Round-25 correctness audit identified two NaN-propagation paths in the
`vmaf_train` package that silently corrupted downstream consumers.

**C.1 — `eval.correlations` (ai/src/vmaf_train/eval.py):**

`correlations()` passed inputs directly to `pearsonr`/`spearmanr` without
checking for empty or degenerate (constant-valued) inputs.

- Empty arrays (`len == 0`) cause `pearsonr`/`spearmanr` to raise or return
  NaN, and `np.mean()` on an empty array returns NaN with a runtime warning.
  The resulting `EvalReport` contained NaN fields without any indication of a
  data-pipeline error.
- Constant-valued arrays have zero variance; `pearsonr`/`spearmanr` return
  NaN for zero-variance inputs because the correlation coefficient is
  mathematically undefined.

`bisect_model_quality._gate` evaluates `report.plcc >= value`. For IEEE-754,
`NaN >= x` is always `False`, so every model "fails" the gate, causing the
bisect to report "first model already bad" with no diagnostic, regardless of
the actual model quality. This is the most harmful failure mode: it produces a
definitive-sounding wrong answer.

**C.2 — `tune.objective` (ai/src/vmaf_train/tune.py):**

When all training epochs produce NaN for the `val/mse` (or `val/l1`) metric
column — which happens when a training run diverges from epoch 0 — the
expression `df["val/mse"].dropna().min()` returns NaN (empty-series min).
`float(NaN)` was passed to Optuna as the trial objective. Optuna records it as
a completed trial with an undefined comparison value, which corrupts the
study's best-trial tracking.

The study in `sweep()` always uses `direction="minimize"`. There is no
maximize path, so the sentinel is `float("inf")` unconditionally.

**Correctness principle (feedback_correctness_first):** investigate the root
cause; never lower thresholds or weaken correctness gates.

## Decision

**C.1:** Add two guards to `correlations()`:

1. Raise `ValueError("empty inputs …")` if `len(pred) == 0`. Empty inputs are
   a data-pipeline bug; raising is the correct signal.
2. Check `np.var(pred) < 1e-12` or `np.var(target) < 1e-12` (constant array).
   Emit a `RuntimeWarning` and return `EvalReport(plcc=0.0, srocc=0.0, …)`.
   Use `0.0` rather than `NaN` because gate logic uses `>=` and NaN would
   silently fail every comparison. The comment in the source explains the
   choice so future readers do not re-introduce NaN.

`evaluate_onnx` (the entry point used by `bisect_model_quality`) delegates to
`correlations`, so both callers are fixed automatically.

**C.2:** Extract the metric-reading logic into `_read_best_metric(df, col)`.
After `dropna()`, if the result is empty or the min is NaN, log a
`WARNING` explaining the divergence and return `float("inf")` (worst-case for
the minimisation study). Move the `from .train import TrainConfig, train`
import inside `sweep()` to keep `_read_best_metric` importable without
pulling in pytorch_lightning (enabling lightweight unit tests).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Return NaN for empty/degenerate (status quo) | No code change | Silently fails gate comparisons; bisect returns wrong answer | Correctness violation |
| Return `Result[EvalReport, str]` type | Explicit error path | Introduces a new algebraic type not used elsewhere in the codebase | Inconsistent with existing `ValueError` patterns |
| Return `plcc=float("-inf")` for degenerate | Also fails `>=` gates | Still has NaN-propagation risk if consumer does arithmetic | 0.0 is the conventional "worst correlation" and does not NaN-propagate |
| Use `warnings.warn` for tune divergence (instead of `logging.warning`) | Catchable with `pytest.warns` | Training-run divergence is an operator diagnostic, not a calling-code invariant; logging is the right channel | Using `caplog` in tests is the standard pytest idiom for logging |

## Consequences

- **Positive**: `bisect_model_quality` no longer reports "first model already
  bad" when the evaluation input is degenerate. Optuna study best-trial
  tracking is not corrupted by NaN objectives from diverged training runs.
- **Positive**: Empty-input bugs in data pipelines are now raised immediately
  at the evaluation call site rather than silently propagating as NaN.
- **Negative**: Callers that previously received NaN (and may have been
  checking `np.isnan(report.plcc)`) will now get `0.0` for degenerate inputs
  and a `ValueError` for empty inputs. No such callers exist in-tree as of
  2026-05-31 (verified by grep).
- **Neutral**: `tune.sweep()` now lazily imports `TrainConfig` / `train`,
  which is a minor import-order change with no runtime effect on the non-test
  path.

## References

- `feedback_correctness_first` (user memory note): investigate root cause;
  never lower thresholds or weaken correctness gates.
- `bisect_model_quality._gate` in `ai/src/vmaf_train/bisect_model_quality.py`.
- `evaluate_onnx` in `ai/src/vmaf_train/eval.py`.
- [ADR-0109](0109-nightly-bisect-model-quality.md) — nightly bisect workflow.
- Round-25 correctness audit C.1 + C.2.
