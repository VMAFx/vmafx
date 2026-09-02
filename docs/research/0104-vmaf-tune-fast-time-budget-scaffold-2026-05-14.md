# Research 0104 — `vmaf-tune fast --time-budget-s` Scaffold Closure

Date: 2026-05-14

## Question

`vmaf-tune fast` exposes `--time-budget-s`, but the code and docs still
described it as advisory. What is the smallest implementation that makes the
flag real without interrupting probe encodes mid-flight?

## Findings

- The fast path already centralises the TPE search in `fast._run_tpe`.
- Optuna's `study.optimize()` accepts a `timeout=` argument that stops
  scheduling new trials after the budget expires.
- Interrupting an in-flight encode would require runner-specific cancellation
  and cleanup; that is not necessary for a soft wall-clock cap.
- The result payload previously reported the requested trial count. Once a
  timeout is real, callers need the completed trial count instead.

## Decision

Pass `time_budget_s` to `study.optimize(timeout=...)`, allow in-flight trials
to finish, and report `len(study.trials)` as `n_trials` in the JSON payload.

## Alternatives considered

- Hard-kill probe encodes at the exact timeout. Rejected because it would need
  subprocess cancellation semantics in every runner and would leave partial
  encode artefacts to clean up.
- Keep the timeout as documentation only. Rejected because the CLI flag is
  user-discoverable and advertised as a budget control.

## Validation

```bash
PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m pytest \
  tools/vmaf-tune/tests/test_fast.py \
  tools/vmaf-tune/tests/test_cli_fast.py -q
```
