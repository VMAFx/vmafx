# Research-0717 — vmaf-tune CLI strict JSON helper

## Context

Research-0715 and Research-0716 moved report-style `vmaf-tune` JSON emitters
onto `vmaftune.jsonio.dumps_strict()`. A CLI sweep still found direct
`json.dumps()` calls in operator-facing stdout and file-output paths such as
`recommend --json`, `tune-per-shot --plan-out`, `recommend-saliency`,
`compare --no-bisect --crf-sweep`, `fast`, `sidecar`, `report`, and
`encode-profile`.

## Findings

- Python's default `json.dumps()` accepts non-finite floats and writes `NaN` or
  `Infinity`. That output is rejected by strict JSON decoders even though it is
  easy for tune paths to carry diagnostic `NaN` values.
- The CLI layer had repeated "dump then add newline" boilerplate with slightly
  different sort-key choices. A local `_write_json_stdout()` helper keeps the
  strict JSON policy central while preserving compact vs pretty and sorted vs
  insertion-order output where the old code relied on it.
- Corpus JSONL emission remains separate: those rows are the training
  interchange and keep their existing schema-specific missing-feature rules.

## Alternatives considered

- **Only fix known failing subcommands.** Rejected because the remaining direct
  emitters had the same default-`json.dumps()` parser hazard.
- **Replace every JSON write in `vmaf-tune`, including internal caches.**
  Rejected for this slice; cache and corpus writers have separate contracts and
  should be audited with their consumers.
- **Route CLI output through report helpers.** Rejected because the report
  module should not become the dependency root for unrelated CLI surfaces.

## Smoke

```bash
.venv/bin/python -m pytest tools/vmaf-tune/tests/test_recommend.py tools/vmaf-tune/tests/test_cli_sidecar.py tools/vmaf-tune/tests/test_per_shot.py tools/vmaf-tune/tests/test_compare_no_bisect.py tools/vmaf-tune/tests/test_encoder_profile.py tools/vmaf-tune/tests/test_cli_fast.py -q
```
