- `ruff` bumped 0.15.17 → **0.16.5**, in `.pre-commit-config.yaml` and in the
  `make lint-tools` pin so the local gate and the CI hooks cannot disagree about
  what counts as a violation.
- The bump auto-fixed **611 findings across 158 files**: 509 with safe fixes and
  a further 102 with `--unsafe-fixes`, applied only for rules where the
  transformation is mechanical (`RUF046` redundant `int()` around `round()` /
  `math.ceil()`, which already return `int` on Python 3; `RUF059` unused
  unpacked bindings; `PIE810`, `C408`, `C409`, `FLY002`, `ISC004`, `SIM*`,
  `PLC0206`, `DTZ011`, `RUF007`, `RUF022`). `black` and `isort` were re-run
  afterwards so the three formatters agree; `pre-commit run --all-files` now
  reaches a fixed point.
- **34 findings are deferred, not fixed**, listed with a per-rule rationale in
  the `[tool.ruff.lint] ignore` block of the root, `ai/` and `tools/vmaf-tune/`
  configs. The intent is to keep the gate exactly as strict as it was *before*
  the bump, so a version upgrade does not smuggle in a refactor. Notably
  `PLR0917` (too-many-positional-arguments) joins the `PLR0913` entry already
  ignored for the same stated reason, and `BLE001` is deferred because the batch
  drivers under `ai/scripts/` catch `Exception` per item on purpose so one bad
  row cannot kill a multi-hour run — the same posture `predictor.py` uses to
  fall back to the analytical curve when `onnxruntime` is absent.
