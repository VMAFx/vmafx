---
name: bisect-model-quality
description: Binary-search a timeline of ONNX checkpoints for the first one that falls below a PLCC / SROCC / RMSE gate on a held-out set. Companion to /bisect-regression (which bisects code commits).
---

<!-- markdownlint-disable MD013 -->

# /bisect-model-quality

## When to use

- You have an ordered list of model checkpoints (training-run intermediates, release history).
- A held-out feature parquet with a `mos` target column.
- You want to find the *first* checkpoint that broke quality — not just that *something* broke.

Unlike `/bisect-regression`, this skill does not rebuild anything; it only runs ORT inference against each candidate. Runs in O(log N) evaluations.

## Invocation

```text
vmaf-train bisect-model-quality \
  <model_0.onnx> <model_1.onnx> ... <model_N.onnx> \
  --features path/to/holdout.parquet \
  --min-plcc 0.9 \
  [--min-srocc 0.8 | --max-rmse 5.0] \
  [--input-name features] \
  [--json out/bisect.json] \
  [--fail-on-first-bad]
```

Exactly one of `--min-plcc`, `--min-srocc`, `--max-rmse` is required. The
model list is interpreted head → tail as assumed-good → assumed-bad;
pass checkpoints in training order.

## Outputs

- A rendered table of every model visited with its PLCC / SROCC / RMSE.
- A `verdict` line identifying the first-bad index, or one of:
  - `"no regression detected"` — tail still passes the gate.
  - `"nothing to bisect"` — head already fails the gate.
- Optional JSON report via `--json`.

## Workflow suggestion

1. `ls checkpoints/ | sort > list.txt` to fix an order.
2. Run this skill with a tight gate (e.g. `--min-plcc 0.95`).
3. If it localises, feed the good/bad pair into `/bisect-regression` with a
   `score-delta` predicate to find the underlying code change.

## Guardrails

- Needs at least 2 models and a parquet with a `mos` column.
- Assumes monotonic quality; if both endpoints are good or both bad, the
  tool emits a verdict and skips the binary search rather than producing
  a nonsense answer.

## Shared helpers

The driver script (`scaffold.sh`) sources
[`.claude/skills/lib/bisect-common.sh`](../lib/bisect-common.sh) for the
clean-tree gate, verdict rendering, and structured-log helpers. The companion
skill `/bisect-regression` sources the same library — keep changes backwards
compatible so the code-commit bisect flow does not silently regress.
