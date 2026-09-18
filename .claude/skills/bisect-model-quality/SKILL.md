---
name: bisect-model-quality
description: Binary-search a timeline of ONNX checkpoints for the first one that falls below a PLCC / SROCC / RMSE gate on a held-out set. Companion to /bisect-regression (which bisects code commits).
---
<!-- markdownlint-disable MD013 -->

# /bisect-model-quality

## When to use

- Ordered list of model checkpoints (training-run intermediates, release
  history).
- Held-out feature parquet with `mos` target column.
- Goal: find *first* checkpoint breaking quality, not just that *something*
  broke.

Differs from `/bisect-regression`: does not rebuild anything; runs only ORT
inference against each candidate. Runs in O(log N) evaluations.

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

Exactly one of `--min-plcc`, `--min-srocc`, `--max-rmse` required.
Model list interpreted head -> tail as assumed-good -> assumed-bad; pass
checkpoints in training order.

## Outputs

- Rendered table of every model visited with PLCC / SROCC / RMSE.
- `verdict` line identifying first-bad index, or one of:
  - `"no regression detected"` -> tail still passes gate.
  - `"nothing to bisect"` -> head already fails gate.
- Optional JSON report via `--json`.

## Workflow suggestion

1. `ls checkpoints/ | sort > list.txt` to fix order.
2. Run skill with tight gate (e.g. `--min-plcc 0.95`).
3. If localised, feed good/bad pair into `/bisect-regression` with
   `score-delta` predicate to find underlying code change.

## Guardrails

- Needs at least 2 models and parquet with `mos` column.
- Assumes monotonic quality. If both endpoints good or both bad -> tool emits
  verdict, skips binary search (no nonsense answer).

## Shared helpers

Driver script (`scaffold.sh`) sources
[`.claude/skills/lib/bisect-common.sh`](../lib/bisect-common.sh)
for clean-tree gate, verdict rendering, structured-log helpers.
Companion skill `/bisect-regression` sources same library -> keep changes
backwards compatible so code-commit bisect flow does not silently regress.
