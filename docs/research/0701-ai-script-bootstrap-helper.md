# Research 0701: AI Script Bootstrap Helper

## Question

Which AI script startup boilerplate is repeated enough to extract after the CLI
helper pass, without hiding each script's artifact-specific behavior?

## Inputs Reviewed

- `ai/scripts/batch_materialize_saliency_features.py`
- `ai/scripts/batch_materialize_second_opinion_features.py`
- `ai/scripts/batch_materialize_mos_labels.py`
- `ai/scripts/extract_k150k_features.py`
- `ai/scripts/combine_full_feature_parquets.py`
- `ai/scripts/enrich_k150k_parquet_metadata.py`
- `ai/src/aiutils/cli_helpers.py`
- `.claude/skills/ai-run-manifest/SKILL.md`

## Findings

The manifest-producing AI scripts still repeat a direct-invocation bootstrap:
resolve `Path(__file__)`, compute the repository root, prepend `ai/src`, and
sometimes also prepend the repo root, `ai/scripts`, or `tools/vmaf-tune/src`.
That startup code is not part of the individual table, MOS, saliency, or K150K
schemas; it is only there so `python ai/scripts/<name>.py` works outside the
pytest `pythonpath`.

The bootstrap cannot live inside `aiutils`, because a directly executed script
must add `ai/src` before importing `aiutils`. A script-local helper under
`ai/scripts/_script_bootstrap.py` keeps the import timing correct while giving
future AI scripts one documented pattern.

## Decision Input

Add `bootstrap_ai_script(__file__)` with explicit options for repo root,
`ai/scripts`, and `vmaf-tune` import roots. Migrate the current batch
materializers and the refreshed K150K/table provenance scripts that already
write durable manifests, then document the rule in `ai/AGENTS.md`,
`ai/src/aiutils/AGENTS.md`, `docs/ai/training.md`, and the `/ai-run-manifest`
skill.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_script_bootstrap.py \
  ai/tests/test_extract_k150k_features.py \
  ai/tests/test_extract_k150k_perf.py \
  ai/tests/test_enrich_k150k_parquet_metadata.py \
  ai/tests/test_combine_full_feature_parquets.py \
  ai/tests/test_batch_materialize_saliency_features.py \
  ai/tests/test_batch_materialize_second_opinion_features.py \
  ai/tests/test_batch_materialize_mos_labels.py -q
```

## Limits

This does not rewrite every AI script with historical `sys.path` setup. The
first batch covers scripts on the active manifest/materializer stack and the
live K150K refresh path. The remaining scripts can migrate in follow-up sweeps
as their owning package areas are touched.
