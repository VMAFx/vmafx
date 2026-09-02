# Research 0703: AI Materializer CLI Helper Sweep

## Question

After the import-bootstrap sweep, which active single-table AI scripts should
use the shared CLI helper layer from ADR-0680?

## Inputs Reviewed

- `ai/scripts/materialize_saliency_features.py`
- `ai/scripts/materialize_second_opinion_features.py`
- `ai/scripts/materialize_mos_labels.py`
- `ai/scripts/signal_mix_audit.py`
- `ai/scripts/feature_correlation.py`
- `ai/src/aiutils/cli_helpers.py`
- `ai/tests/test_materialize_saliency_features.py`
- `ai/tests/test_second_opinion_features.py`
- `ai/tests/test_materialize_mos_labels.py`
- `ai/tests/test_signal_mix_audit.py`
- `ai/tests/test_feature_correlation.py`

## Findings

These scripts still built `argparse.ArgumentParser` instances directly and
captured raw `sys.argv` locally even though the batch wrappers now use
`make_argument_parser()` and `collect_cli_argv()`. That leaves the same
materializer stack with two parser/provenance idioms.

The shared helper is intentionally small: it does not change CLI options,
manifest schemas, materializer behavior, or report payloads. It only standardizes
parser formatting and raw-argv capture for ADR-0661 provenance.

## Scope Chosen

Migrate the three materializers plus signal-mix and feature-correlation reports
to `make_argument_parser()` and `collect_cli_argv()`.

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

This does not migrate every historical AI training/export script. Those can
move to the helper when their owning model or report surface is already being
touched.
