<!-- markdownlint-disable MD060 -->
# Research 0676 — Phase F calibration provenance

## Context

`ai/scripts/calibrate_phase_f_recipes.py` writes the calibrated content-recipe
JSON consumed by `vmaf-tune auto`. The file is not just an investigation report:
it changes operator-facing recipe thresholds for animation, screen content,
live-action HDR, and UGC. Before this pass the JSON contained recipe metadata,
but not the shared command/input/output provenance block used by the refreshed
AI reports.

## Decision

Attach ADR-0661 `run_provenance` to the calibration output. The block records:

- entrypoint: `ai/scripts/calibrate_phase_f_recipes.py`
- inputs: source corpus JSONL
- args: row cap, log level, output path, and original argv
- outputs: calibrated recipe JSON path

The runtime recipe loader remains unchanged because it ignores unknown
top-level keys and reads `recipes` / `metadata` by name.

## Alternatives Considered

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Keep the existing JSON shape | No diff | Recipe thresholds still lose corpus snapshot and row-cap lineage | Rejected |
| Store provenance only in `metadata.corpus` | Keeps one metadata namespace | Recreates script-local manifest shape and omits argv/output target | Rejected |
| Attach ADR-0661 `run_provenance` | Shared schema; no loader change; records input/output/argv | Adds one top-level key | Chosen |

## Validation

- `.venv/bin/ruff check ai/scripts/calibrate_phase_f_recipes.py ai/tests/test_calibrate_phase_f_recipes.py`
- `.venv/bin/python -m pytest ai/tests/test_calibrate_phase_f_recipes.py -q`
- `.venv/bin/mkdocs build --strict`
- `make format-check`
