<!-- markdownlint-disable MD060 -->
# Research 0728: Train / Aggregate Scripts Bootstrap Sweep

## Question

Which remaining train and aggregate scripts in `ai/scripts/` and `ai/train/`
still use ad hoc `sys.path` mutation or `sys.argv[1:]` provenance capture
after the shared AI script helpers landed in the #1505–#1510 series?

## Inputs Reviewed

- `ai/scripts/train_konvid.py` (193 lines)
- `ai/scripts/train_konvid_mos_head.py` (1585 lines)
- `ai/scripts/aggregate_corpora.py` (619 lines)
- `ai/train/train.py`
- `ai/scripts/_script_bootstrap.py` — shared helper
- `ai/src/aiutils/cli_helpers.py` — `collect_cli_argv`, `make_argument_parser`
- Reference conversions: `ai/scripts/chug_to_corpus_jsonl.py`,
  `ai/scripts/batch_materialize_mos_labels.py`,
  `ai/scripts/feature_correlation.py`

## Findings

All four scripts predated the `_script_bootstrap` helper and used one of two
local patterns:

**Pattern A — manual `REPO_ROOT` / `sys.path.insert`:**

```python
REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
```

Used in: `train_konvid.py`, `train_konvid_mos_head.py`.

**Pattern B — no path setup (aiutils imported at module top):**

`aggregate_corpora.py` imported `aiutils.*` directly without any `sys.path`
manipulation. This only worked because `ai/src` was already on `sys.path` from
the test harness; a direct `python ai/scripts/aggregate_corpora.py` invocation
would have failed.

**Pattern C — conditional `sys.path.insert` for `__package__` compat:**

`ai/train/train.py` uses a guarded block (`if __package__ in (None, "")`) to
add the repo root to `sys.path` and then set `__package__` so that relative
imports inside the `ai.train` package work. The block is not module-top-level
in the conventional sense; it executes conditionally at import time.

Additionally, `train_konvid_mos_head.py` already had `raw_argv` correctly
assigned (`list(sys.argv[1:] if argv is None else argv)`) but did not use
`collect_cli_argv`, and `aggregate_corpora.py` passed `sys.argv[1:] if argv is
None else argv` inline to `build_run_provenance` at the call site.

## Scope Chosen

Migrate all four scripts to:

- `bootstrap_ai_script(__file__)` (or `include_repo_root=True` where needed);
- `collect_cli_argv(argv)` for raw-argv capture in scripts with provenance;
- `make_argument_parser(...)` where the parser construction is simple
  (no `RawDescriptionHelpFormatter`, no `parents=`);
- Bootstrap-provided `SCRIPT_PATH` / `REPO_ROOT` values used in
  `build_run_provenance` calls instead of re-deriving from `__file__`.

For `ai/train/train.py` the conditional guard is preserved because it also
sets `__package__` — that assignment cannot be moved into the bootstrap
helper. The bootstrap is loaded via a one-shot `sys.path.insert` pointing at
`ai/scripts/` (which is narrower than the original repo-root insertion), then
called inside the conditional to add both the repo root and `ai/src`.

## Alternatives Considered

| Option | Trade-off |
|--------|-----------|
| Leave `ai/train/train.py` unchanged | Avoids conditional complexity but leaves the only remaining manual `sys.path` block in the ai/ tree |
| Move bootstrap into `ai/train/` | Duplicates the helper; maintenance burden |
| Use `importlib.util` to load bootstrap | Correct but verbose; adds ~6 lines of boilerplate vs. the chosen 3-line approach |
| Replace `aggregate_corpora._build_parser()` with `make_argument_parser` | Chosen; the description is simple and the formatter upgrade (`ArgumentDefaultsHelpFormatter`) is a usability improvement |

## Reproducer / Smoke

```bash
/home/kilian/dev/vmaf/.venv/bin/python -m pytest \
  ai/tests/test_aggregate_corpora.py \
  ai/tests/test_train_konvid_mos_head.py \
  ai/tests/test_train_smoke.py -q
```

## Limits

This is a behavior-preserving script hygiene sweep. It does not retrain,
re-export, or promote any ONNX checkpoint. No model-card values change.
