---
name: ai-run-manifest
description: Add or audit replay-manifest sidecars for AI scripts that create datasets, feature tables, model artifacts, evaluation reports, or fetch/download roots.
---
# /ai-run-manifest

## When to use

- Script under `ai/scripts/` writes durable artifact: parquet, JSONL, ONNX,
  registry update, model-card sidecar, evaluation report, calibration JSON, or
  downloaded dataset/cache root.
- PR changes inputs, filtering, schema, or defaults for artifact-producing
  script.
- Audit stale AI artifacts -> prove command that produced each local file.

## Required Pattern

1. Script executable as `python ai/scripts/<name>.py`: call
   `bootstrap_ai_script(__file__)` from `ai/scripts/_script_bootstrap.py`
   before importing `aiutils` or sibling `ai/scripts` modules. Add only
   optional roots script needs (`include_repo_root`, `include_ai_scripts`,
   `include_vmaf_tune_src`).
2. New standalone sidecar: call `aiutils.run_manifest.write_run_manifest()`.
   Pass:
   - `schema`: script-specific, versioned, kebab-case plus `-v1`.
   - `entrypoint`: current script path.
   - `repo_root`: repository root.
   - `argv`: raw CLI arguments.
   - `args`: parsed CLI namespace or mapping.
   - `inputs` / `outputs`: named paths, even when optional/missing.
   - `sections`: adapter-specific counters, selected features, gates, config,
     schemas, or status.
3. Script already has stable report schema: embed only `build_run_provenance()`
   in existing report instead of renaming report to new manifest schema.
4. Default new sidecars beside artifact (`<output>.manifest.json`) unless
   artifact has stronger local convention.
5. Batch manifest runner: import `make_argument_parser()`,
   `collect_cli_argv()`, and `add_batch_manifest_arguments()` from
   `aiutils.cli_helpers` instead of duplicating common `--manifest` /
   `--base-dir` / report-output / fail-fast flags.
6. Add tests: assert script writes sidecar and
   `run_provenance.schema == "ai-run-provenance-v1"`.

## Guardrails

- Do not hand-roll path hashing, JSON sorting, or `run_provenance` structure in
  script.
- Do not add new ad hoc `sys.path.insert(...)` blocks to AI scripts; extend
  `_script_bootstrap.py` when new repo-local import root genuinely needed.
- Keep row schemas stable. Put run-level evidence in sidecar, not every row.
- Preserve existing machine-readable report keys unless PR explicitly bumps
  report schema.
- Fetchers record URLs, archives, selected rows, cache roots. Feature-table
  builders record selected columns, row/source counts, source artifacts.
  Model exporters record input checkpoint/config, ONNX outputs, gate status.
- Update `ai/AGENTS.md`, relevant docs under `docs/ai/`, and model card or
  runbook for artifact reproduction.
