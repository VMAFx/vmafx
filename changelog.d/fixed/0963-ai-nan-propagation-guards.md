**ai/src:** guard NaN propagation in `eval.correlations` and `tune._read_best_metric` (round-25 audit C.1 + C.2, ADR-0963).

- `correlations()` now raises `ValueError` on empty inputs (length 0) and
  returns `plcc=0.0, srocc=0.0` with a `RuntimeWarning` for constant-valued
  (zero-variance) inputs instead of propagating NaN. The 0.0 sentinel
  prevents `bisect_model_quality._gate` from silently reporting "first model
  already bad" when the evaluation input is degenerate.
- `tune._read_best_metric()` (extracted from the `objective` closure) returns
  `float("inf")` when all training-epoch metrics are NaN, preventing Optuna
  study best-trial corruption.
- `from .train import` in `tune.py` moved inside `sweep()` to keep
  `_read_best_metric` importable without pytorch_lightning (enables
  lightweight unit tests).
- 19 new unit tests in `ai/tests/test_eval_correlations.py` and
  `ai/tests/test_tune_objective.py`.
