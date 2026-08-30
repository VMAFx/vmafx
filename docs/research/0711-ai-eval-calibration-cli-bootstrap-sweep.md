<!-- markdownlint-disable MD013 -->
# AI evaluation and calibration CLI bootstrap sweep

Date: 2026-05-22

## Scope

This sweep extends the accepted AI CLI/bootstrap helper pattern to evaluation,
calibration, and diagnostic utilities:

- `ai/scripts/eval_saliency_per_mb.py`
- `ai/scripts/eval_probabilistic_proxy.py`
- `ai/scripts/calibrate_phase_f_recipes.py`
- `ai/scripts/calibrate_nr_threshold.py`
- `ai/scripts/analyze_knob_sweep.py`
- `ai/scripts/hardware_caps_loader.py`

## Findings

These scripts already had useful tests and report/provenance coverage, but they
still used a mix of local `sys.path` mutation, direct
`argparse.ArgumentParser(...)` construction, and script-specific `sys.argv`
capture. That made their manifests slightly inconsistent with the fetch,
materializer, exporter, and full-feature batches.

The utilities do not need behavior changes. The valuable part is keeping their
CLI entrypoint path resolution, help formatting, and manifest `argv` capture
identical to the rest of `ai/scripts/`.

## Decision

Use `bootstrap_ai_script()` for script import setup and
`make_argument_parser()` / `collect_cli_argv()` for CLI setup. Keep each
script's existing output schema, validation semantics, and report contents.

No new ADR is needed because this implements ADR-0680 and ADR-0681.

## Validation

- `.venv/bin/python -m pytest ai/tests/test_eval_saliency_per_mb.py ai/tests/test_legacy_eval_report_run_provenance.py ai/tests/test_calibrate_phase_f_recipes.py ai/tests/test_calibrate_nr_threshold.py ai/tests/test_knob_sweep_analysis.py ai/tests/test_hardware_caps.py -q`
- `.venv/bin/ruff check ai/scripts/eval_saliency_per_mb.py ai/scripts/eval_probabilistic_proxy.py ai/scripts/calibrate_phase_f_recipes.py ai/scripts/calibrate_nr_threshold.py ai/scripts/analyze_knob_sweep.py ai/scripts/hardware_caps_loader.py`
- `.venv/bin/black --check ai/scripts/eval_saliency_per_mb.py ai/scripts/eval_probabilistic_proxy.py ai/scripts/calibrate_phase_f_recipes.py ai/scripts/calibrate_nr_threshold.py ai/scripts/analyze_knob_sweep.py ai/scripts/hardware_caps_loader.py`
