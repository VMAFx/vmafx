<!-- markdownlint-disable MD060 -->
# Research-0720 — vmaf-tune executor strict JSONL results

## Question

Can the shared `vmaftune.jsonio` serializer cover Phase-F executor result
JSONL without changing the in-memory caller contract or the Phase-A corpus
schema?

## Findings

- `run_plan`, `run_plan_per_shot`, and `run_plan_saliency` append
  human-facing result logs under `tune_results*.jsonl`; these files are read
  by report tooling and future encoder-profile consumers rather than by the
  Phase-A trainer interchange.
- Failed score runs intentionally leave `ScoreResult.vmaf_score` as
  `float("nan")`, and all-failed per-shot runs leave `weighted_vmaf` as
  `float("nan")` so Python callers can still distinguish "missing numeric
  result" from a real zero.
- Raw `json.dumps(..., sort_keys=True)` writes those values as JavaScript-only
  `NaN` tokens, which strict JSONL parsers reject.

## Decision

Route executor result-row writes through a small local `_write_jsonl_row()`
helper backed by `dumps_strict(indent=None, sort_keys=True)`. Keep rows
unchanged in memory; only the serialized JSONL maps non-finite diagnostics to
`null`.

## Alternatives considered

| Option | Pros | Cons | Verdict |
|---|---|---|---|
| Leave executor rows on raw `json.dumps()` | Smallest diff | Non-standard `NaN` leaks into user-facing result files | Rejected |
| Normalize row dicts before returning | Serialized and in-memory shapes match | Hides failure sentinels from Python callers | Rejected |
| Strict serialization only at the write boundary | Portable files and unchanged caller math | Disk rows use `null` while returned rows keep `NaN` | Chosen |

## Validation

```bash
.venv/bin/python -m pytest tools/vmaf-tune/tests/test_executor.py tools/vmaf-tune/tests/test_executor_pershot_saliency.py -q
.venv/bin/ruff check tools/vmaf-tune/src/vmaftune/executor.py tools/vmaf-tune/tests/test_executor.py tools/vmaf-tune/tests/test_executor_pershot_saliency.py
.venv/bin/black --check tools/vmaf-tune/src/vmaftune/executor.py tools/vmaf-tune/tests/test_executor.py tools/vmaf-tune/tests/test_executor_pershot_saliency.py
```
