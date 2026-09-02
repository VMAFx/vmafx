# Research 0707: AI Quant/Export Script Bootstrap Sweep

## Question

Which quantization and DNN-export scripts still use local import setup, direct
parser construction, or raw `sys.argv` provenance capture after the shared AI
helpers landed?

## Inputs Reviewed

- `ai/scripts/qat_train.py`
- `ai/scripts/ptq_dynamic.py`
- `ai/scripts/ptq_static.py`
- `ai/scripts/measure_quant_drop.py`
- `ai/scripts/measure_quant_drop_per_ep.py`
- `ai/scripts/export_tiny_models.py`
- `ai/tests/test_qat_smoke.py`
- `ai/tests/test_ptq_scripts.py`
- `ai/tests/test_measure_quant_drop_per_ep.py`
- `ai/tests/test_dnn_exporter_run_provenance.py`

## Findings

The quantization and exporter scripts already wrote provenance-backed reports,
but each still carried a local version of the setup pattern:

- direct `sys.path.insert(...)` blocks for `ai/src` or repo-root imports;
- `argparse.ArgumentParser(...)` construction in each script;
- direct `sys.argv[1:]` capture for replay metadata.

`qat_train.py` also needs the repository root on `sys.path` for its
`ai.train.*` imports when invoked as `python ai/scripts/qat_train.py`.
That makes it the clearest proof that the bootstrap helper needs per-script
import-root flags rather than a single hard-coded default.

## Scope Chosen

Migrate the quant/export group to:

- `bootstrap_ai_script(__file__)`;
- `bootstrap_ai_script(__file__, include_repo_root=True)` for `qat_train.py`;
- `make_argument_parser(...)`;
- `collect_cli_argv(argv)`;
- normalized `run_provenance["argv"]` values.

## Reproducer / Smoke

```bash
.venv/bin/python -m pytest \
  ai/tests/test_qat_smoke.py \
  ai/tests/test_ptq_scripts.py \
  ai/tests/test_measure_quant_drop_per_ep.py \
  ai/tests/test_dnn_exporter_run_provenance.py -q
```

## Limits

This is a script hygiene sweep only. It does not re-export ONNX artefacts,
change quantization mode, or update model registry rows.
