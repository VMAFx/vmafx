# Research 0702: AI Materializer Bootstrap Sweep

## Question

Which scripts should migrate immediately after the new AI bootstrap helper lands
so the batch runners and their single-table tools do not use different import
patterns?

## Inputs Reviewed

- `ai/scripts/materialize_saliency_features.py`
- `ai/scripts/materialize_second_opinion_features.py`
- `ai/scripts/materialize_mos_labels.py`
- `ai/scripts/signal_mix_audit.py`
- `ai/scripts/feature_correlation.py`
- `ai/scripts/_script_bootstrap.py`
- `ai/tests/test_materialize_saliency_features.py`
- `ai/tests/test_second_opinion_features.py`
- `ai/tests/test_materialize_mos_labels.py`
- `ai/tests/test_signal_mix_audit.py`
- `ai/tests/test_feature_correlation.py`

## Findings

The batch manifest runners now use `bootstrap_ai_script()`, but the underlying
single-table materializers still had local `Path(__file__)` plus
`sys.path.insert(...)` blocks. That split would make future edits harder to
review because wrapper scripts and their worker scripts would bootstrap imports
differently even though they live in the same materialization stack.

`signal_mix_audit.py` and `feature_correlation.py` are in the same active AI
artifact-analysis path: both emit durable reports with run provenance and both
use the same repo-root plus `ai/src` import shape. Migrating them in the same
sweep keeps the signal-audit tools aligned with the materializer stack without
touching model training behavior.

## Scope Chosen

Migrate these five scripts to `bootstrap_ai_script(__file__)` only. Keep their
schemas, report payloads, materializer behavior, and CLI arguments unchanged.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_materialize_saliency_features.py \
  ai/tests/test_second_opinion_features.py \
  ai/tests/test_materialize_mos_labels.py \
  ai/tests/test_signal_mix_audit.py \
  ai/tests/test_feature_correlation.py -q
```

## Limits

The remaining historical AI scripts can migrate later when their owning
workstream is already touched. This sweep deliberately avoids training/export
entrypoints with larger dependency surfaces.
