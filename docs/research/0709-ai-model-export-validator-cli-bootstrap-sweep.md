# Research 0709: AI Model Export/Validator CLI Bootstrap Sweep

## Question

Which AI model exporter and validator scripts still hand-rolled
direct-invocation setup after the shared AI helpers landed?

## Inputs Reviewed

- `ai/scripts/validate_model_registry.py`
- `ai/scripts/validate_saliency_student.py`
- `ai/scripts/export_u2netp_mirror.py`
- `ai/scripts/export_fastdvdnet_pre.py`
- `ai/scripts/export_fastdvdnet_pre_placeholder.py`
- `ai/scripts/export_transnet_v2.py`
- `ai/scripts/export_transnet_v2_placeholder.py`
- `ai/scripts/export_ensemble_v2_seeds.py`
- `ai/scripts/validate_ensemble_seeds.py`
- `ai/tests/test_validation_report_provenance.py`
- `ai/tests/test_export_u2netp_mirror.py`
- `ai/tests/test_dnn_exporter_run_provenance.py`
- `ai/tests/test_export_ensemble_v2_seeds_provenance.py`
- `ai/tests/test_validate_ensemble_seeds.py`

## Findings

The exporter/validator group already records run provenance in sidecars or
reports, but the entrypoints still had local setup variants:

- direct `sys.path.insert(...)` blocks for `ai/src`, `ai/scripts`, or
  `scripts/ci`;
- direct `argparse.ArgumentParser(...)` construction;
- raw `sys.argv[1:]` capture before writing `run_provenance`;
- direct `Path(__file__).resolve()` path resolution before the shared
  bootstrap helper existed.

`validate_saliency_student.py` and `export_ensemble_v2_seeds.py` need the
`ai/scripts` path because they reuse sibling training modules. The ensemble
validator still adds `scripts/ci` explicitly because the helper is intentionally
AI-scoped and the production gate remains owned by CI.

## Scope Chosen

Migrate the group to:

- `bootstrap_ai_script(__file__)` or
  `bootstrap_ai_script(__file__, include_ai_scripts=True)`;
- `make_argument_parser(...)`;
- `collect_cli_argv(argv)`;
- normalized `run_provenance["argv"]` values in exporter sidecars and
  validation reports.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_validation_report_provenance.py \
  ai/tests/test_export_u2netp_mirror.py \
  ai/tests/test_dnn_exporter_run_provenance.py \
  ai/tests/test_export_ensemble_v2_seeds_provenance.py \
  ai/tests/test_validate_ensemble_seeds.py -q
```

## Limits

This is a script hygiene sweep only. It does not export ONNX artefacts, update
registry rows, change production thresholds, or alter model contracts.
