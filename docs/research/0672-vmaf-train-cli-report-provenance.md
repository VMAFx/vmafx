<!-- markdownlint-disable MD060 -->
# Research-0672: vmaf-train CLI Report Provenance

## Summary

The ADR-0661 provenance sweep covered many one-off AI scripts, but the
user-facing `vmaf-train` CLI still wrote several durable JSON reports with
direct `json.dumps()` calls. These reports are the artifacts operators attach
to model cards and promotion PRs: normalization drift checks, latency profiles,
learned-filter audits, INT8 drift reports, ORT execution-provider diffs, and
model-quality bisection results.

Without `run_provenance`, those reports preserved metrics but not the command,
parsed thresholds, model inputs, feature/calibration inputs, or generated
output targets that made the metrics reproducible.

## Files Audited

- `ai/src/vmaf_train/cli.py`
- `ai/tests/test_tune_cli.py`
- `docs/usage/vmaf-train.md`
- ADR-0661 and `aiutils.run_manifest`

## Findings

- `validate-norm`, `profile`, `audit-learned-filter`, `quantize-int8`,
  `cross-backend`, and `bisect-model-quality` all accept `--json` and emit
  durable reports.
- The report payloads already have stable `to_dict()` shapes; they only needed
  a shared write path that appends provenance before serialization.
- `quantize-int8` also writes a generated model, so its provenance should record
  both the JSON report and the INT8 model target.

## Decision Matrix

| Option | Pros | Cons | Result |
|---|---|---|---|
| Keep CLI reports as plain JSON | Smallest diff | User-facing evidence remains disconnected from inputs and thresholds | Rejected |
| Add bespoke `command` fields per subcommand | Localized and explicit | Duplicates ADR-0661 normalisation and path hashing | Rejected |
| Add a shared CLI report writer using ADR-0661 | One helper covers all JSON report commands; matches other AI artifacts | Slightly larger report JSON | Chosen |

## Outcome

`ai/src/vmaf_train/cli.py` now writes `--json` report payloads through a shared
helper that attaches ADR-0661 `run_provenance`. The helper records the CLI
entrypoint, argv, parsed options, report inputs, JSON report target, and
generated model target where applicable.

## Validation

```bash
.venv/bin/ruff check ai/src/vmaf_train/cli.py ai/tests/test_tune_cli.py
.venv/bin/python -m pytest ai/tests/test_tune_cli.py -q
```
