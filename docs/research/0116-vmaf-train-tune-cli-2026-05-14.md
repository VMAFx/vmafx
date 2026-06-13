# Research-0116: vmaf-train tune CLI closure

- **Status**: Active
- **Workstream**: vmaf-train hyperparameter sweep CLI
- **Last updated**: 2026-05-14

## Question

Can the documented `vmaf-train tune` surface be wired without expanding the
core dependency set or inventing a second training path?

## Sources

- [`ai/src/vmaf_train/tune.py`](../../ai/src/vmaf_train/tune.py) — existing
  Optuna sweep helper, lazily importing the optional dependency and dispatching
  each trial through `train(cfg)`.
- [`ai/src/vmaf_train/cli.py`](../../ai/src/vmaf_train/cli.py) — Typer CLI
  entry point already exposing `fit`, `export`, `eval`, registry, profiling,
  quantization, and quality-bisect commands.
- [`docs/ai/training.md`](../ai/training.md) — stale text saying the command
  was planned but not wired.

## Findings

The helper already has the right separation: core installs can import
`vmaf_train.tune` because Optuna is imported inside `sweep()`, not at module
import time. The missing piece was a CLI adapter that parses a compact search
space, loads the same YAML config as `vmaf-train fit`, and hands a `suggest`
callable to `sweep()`. This keeps the training path single-source: every trial
still goes through `TrainConfig` and `train(cfg)`.

## Alternatives explored

Embedding a YAML search-space schema in the base config was rejected for this
first closure because it would change config semantics for every existing
training recipe. A repeatable `--param name=kind:...` flag is explicit,
small, and enough for common sweeps while leaving room for a future
config-native schema.

Adding Ray Tune orchestration was rejected for this pass. The optional extra
already lists Ray for future distributed sweeps, but the in-tree helper is
Optuna-first and local-trial oriented; wiring Ray would introduce a second
scheduler contract before the local command is proven.

## Open questions

- Whether to add a config-native `sweep:` block once real training runs need
  larger search spaces than repeatable CLI flags can comfortably express.

## Related

- Docs: [`docs/ai/training.md`](../ai/training.md)
- Code: [`ai/src/vmaf_train/cli.py`](../../ai/src/vmaf_train/cli.py),
  [`ai/src/vmaf_train/tune.py`](../../ai/src/vmaf_train/tune.py)
