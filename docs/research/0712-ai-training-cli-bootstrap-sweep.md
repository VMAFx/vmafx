<!-- markdownlint-disable MD013 -->
# Research 0712: AI Training CLI Bootstrap Sweep

Date: 2026-05-22

## Scope

Audit the remaining AI training entrypoints that still carried their own import-path and
argument-vector boilerplate after the helper sweeps in Research 0701-0711.

Covered scripts:

- `ai/scripts/train_saliency_student.py`
- `ai/scripts/train_saliency_student_v2.py`
- `ai/scripts/train_predictor_v2_realcorpus.py`

`ai/scripts/extract_k150k_features.py` is intentionally left untouched because the local K150K
refresh job is running and may reload that module from worker processes.

## Findings

The saliency trainers duplicated the same pattern:

- derive `SCRIPT_PATH` and `REPO_ROOT` by hand
- prepend `ai/src` to `sys.path`
- construct a raw `argparse.ArgumentParser`
- capture `sys.argv[1:]` directly for run provenance

The predictor-v2 real-corpus trainer added a second copy of the path logic for
`tools/vmaf-tune/src`, once during codec resolution and once during lazy trainer import. That
made it easier for future corpus/training scripts to drift from ADR-0680/ADR-0681.

## Change

All three entrypoints now use:

- `bootstrap_ai_script(...)` for repo-root / `ai/src` / vmaf-tune path setup
- `make_argument_parser(...)` for consistent CLI formatter defaults
- `collect_cli_argv(...)` before parsing and before report provenance capture

The predictor-v2 trainer uses `include_vmaf_tune_src=True` once at bootstrap time and removes
the two local `sys.path` mutations.

## Alternatives Considered

| Option | Result | Rationale |
| --- | --- | --- |
| Leave training scripts for a later full-training rewrite | Rejected | These scripts write provenance used by long-running model refreshes; keeping boilerplate drift here is exactly what the helper pattern is meant to remove. |
| Refactor every remaining AI script in one PR | Rejected | The active K150K worker is running; touching its extractor during the job is avoidable risk. |
| Add a new training-specific parser helper | Rejected | The accepted generic helper already covers argv capture and formatter defaults. No new policy decision is needed. |

## Validation

Focused tests:

```bash
.venv/bin/python -m pytest \
    ai/tests/test_saliency_student_metrics_provenance.py \
    ai/tests/test_train_predictor_v2_realcorpus.py -q
```

Static checks:

```bash
.venv/bin/ruff check \
    ai/scripts/train_saliency_student.py \
    ai/scripts/train_saliency_student_v2.py \
    ai/scripts/train_predictor_v2_realcorpus.py
.venv/bin/black --check \
    ai/scripts/train_saliency_student.py \
    ai/scripts/train_saliency_student_v2.py \
    ai/scripts/train_predictor_v2_realcorpus.py
```

## Rebase Notes

No rebase-sensitive invariant changes. This applies ADR-0680/ADR-0681 to more existing scripts.
