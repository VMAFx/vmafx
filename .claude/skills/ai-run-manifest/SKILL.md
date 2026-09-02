---
name: ai-run-manifest
description: Add or audit replay-manifest sidecars for AI scripts that create datasets, feature tables, model artifacts, evaluation reports, or fetch/download roots.
---

# /ai-run-manifest

## When to use

- A script under `ai/scripts/` writes a durable artifact: parquet, JSONL, ONNX,
  registry update, model-card sidecar, evaluation report, calibration JSON, or
  downloaded dataset/cache root.
- A PR changes the inputs, filtering, schema, or defaults for one of those
  artifact-producing scripts.
- You are auditing stale AI artifacts and need to prove which command produced
  each local file.

## Required Pattern

1. For scripts executable as `python ai/scripts/<name>.py`, call
   `bootstrap_ai_script(__file__)` from `ai/scripts/_script_bootstrap.py`
   before importing `aiutils` or sibling `ai/scripts` modules. Add only the
   optional roots the script actually needs (`include_repo_root`,
   `include_ai_scripts`, `include_vmaf_tune_src`).
2. For a new standalone sidecar, call `aiutils.run_manifest.write_run_manifest()`.
   Pass:
   - `schema`: script-specific, versioned, kebab-case plus `-v1`.
   - `entrypoint`: the current script path.
   - `repo_root`: the repository root.
   - `argv`: the raw CLI arguments.
   - `args`: parsed CLI namespace or mapping.
   - `inputs` / `outputs`: named paths, even when optional/missing.
   - `sections`: adapter-specific counters, selected features, gates, config,
     schemas, or status.
3. If the script already has a stable report schema, embed only
   `build_run_provenance()` in that existing report instead of renaming the
   report to a new manifest schema.
4. Default new sidecars beside the artifact (`<output>.manifest.json`) unless
   the artifact has a stronger local convention.
5. For a batch manifest runner, import `make_argument_parser()`,
   `collect_cli_argv()`, and `add_batch_manifest_arguments()` from
   `aiutils.cli_helpers` instead of duplicating the common
   `--manifest` / `--base-dir` / report-output / fail-fast flags.
6. Add tests that assert the script writes the sidecar and that
   `run_provenance.schema == "ai-run-provenance-v1"`.

## Guardrails

- Do not hand-roll path hashing, JSON sorting, or `run_provenance` structure in
  the script.
- Do not add new ad hoc `sys.path.insert(...)` blocks to AI scripts; extend
  `_script_bootstrap.py` when a new repo-local import root is genuinely needed.
- Keep row schemas stable. Put run-level evidence in the sidecar, not every row.
- Preserve existing machine-readable report keys unless the PR explicitly bumps
  that report schema.
- Fetchers record URLs, archives, selected rows, and cache roots. Feature-table
  builders record selected columns, row/source counts, and source artifacts.
  Model exporters record input checkpoint/config, ONNX outputs, and gate status.
- Update `ai/AGENTS.md`, the relevant docs under `docs/ai/`, and the model card
  or runbook that tells humans how to reproduce the artifact.
