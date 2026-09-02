<!-- markdownlint-disable MD040 -->
# Research-0991: Second-Opinion Batch Materializer — Smoke-Run Scaffold

## Problem

`ai/scripts/batch_materialize_second_opinion_features.py` was merged in PR #1497
(archived) but no run artifacts exist under `.workingdir2/` or `runs/`. The
task is to verify the script is wired correctly and scaffold a committed
smoke-run path that operators can use before pointing the batch runner at real
corpus data.

Two issues found during verification:

### Issue 1: pytest fails without `PYTHONPATH=ai/scripts`

Running `pytest ai/tests/` from the repo root fails for the second-opinion batch
tests and the saliency batch tests with:

```text
ModuleNotFoundError: No module named '_script_bootstrap'
```

Root cause: `test_batch_materialize_second_opinion_features.py` uses
`importlib.spec_from_file_location` to load the script at its absolute path.
When Python executes the script body, it tries `from _script_bootstrap import
bootstrap_ai_script` — but `_script_bootstrap.py` lives in `ai/scripts/` and
`ai/scripts/` is not on `sys.path` when pytest is invoked from the repo root.

`ai/pyproject.toml` has no `pythonpath` entry for pytest, so the issue affects
every CI invocation of `pytest ai/tests/` unless an explicit
`PYTHONPATH=ai/scripts` is prepended. The same bug affects the saliency batch
tests.

**Fix**: add `pythonpath = ["scripts"]` to `[tool.pytest.ini_options]` in
`ai/pyproject.toml`. This is the standard pytest mechanism for adding
non-installed directories to `sys.path` during test collection; it requires
pytest ≥ 7.0, which the existing `pytest>=9.0.3` floor satisfies.

Verification: `pytest ai/tests/test_batch_materialize_second_opinion_features.py
ai/tests/test_batch_materialize_saliency_features.py` — all 7 tests pass after
the fix, zero tests pass before.

### Issue 2: No committed smoke scaffold

The `runs/` directory is gitignored, so run manifests and outputs from
exploratory invocations are not available in the repo. The analogous saliency
batch runner also lacks a committed smoke path.

**Fix**: commit a minimal smoke scaffold under
`ai/testdata/smoke-second-opinion-batch/`:

- `batch.json` — two-table manifest; relative paths anchor to the repo root via
  `--base-dir .`; outputs go to `/tmp/vmafx-smoke-second-opinion/` so no
  generated artifacts are committed.
- `fixtures/features_a.jsonl`, `fixtures/features_b.jsonl` — 3-row and 2-row
  synthetic feature tables keyed on `video_id`.
- `fixtures/scores_fork_nr_a.jsonl`, `fixtures/scores_fork_nr_b.jsonl` —
  matching `fork-nr` score rows with scalar `score` and `runtime_ms`.
- `README.md` — exact invocation, expected stderr, and inspection checklist.

Smoke run verified end-to-end:

```bash
PYTHONPATH=ai/scripts python ai/scripts/batch_materialize_second_opinion_features.py \
    --manifest ai/testdata/smoke-second-opinion-batch/batch.json \
    --base-dir . \
    --report-json /tmp/vmafx-smoke-second-opinion/smoke.report.json \
    --report-md   /tmp/vmafx-smoke-second-opinion/smoke.report.md
```

Stderr: `tables=2 input_rows=5 output_rows=5 failed_tables=0`

Report JSON:

- `schema: second-opinion-materializer-batch-v1`
- `status: ok`
- `summary.tables: 2`, `summary.output_rows: 5`
- `run_provenance.schema: ai-run-provenance-v1`

Output tables contain `second_opinion_fork_nr_score`, `_status=ok`, and
`_runtime_ms` columns, confirming the full join pipeline.

## Decision Drivers

- Fix a pre-existing CI latent failure before it causes confusion on a future
  PR that adds more batch script tests.
- Give operators a single `PYTHONPATH=ai/scripts python ...` command with known
  output to verify the pipeline before connecting real corpus data.

## Follow-Up

Generate real scorer sidecars for CHUG, KoNViD, UGC, Netflix, and BVI tables,
run the batch manifest, then rerun the signal-mix audit and retrain candidate
models.
