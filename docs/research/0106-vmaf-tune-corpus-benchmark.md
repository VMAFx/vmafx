# Research-0106: `vmaf-tune` corpus benchmark gap check

- **Date**: 2026-05-14
- **Scope**: `tools/vmaf-tune` Phase-G backlog item
- **Result**: implement a read-only corpus report; do not add another encode
  runner.

## Inputs

- `.workingdir2/BACKLOG.md` still listed Phase G, cross-codec corpus
  benchmark, as not started.
- `vmaf-tune compare` already runs real Phase-B bisect work per encoder.
- `vmaf-tune recommend` already selects one row for a target predicate.

## Findings

The missing workflow is offline analysis of a corpus that already exists. A
Phase-A JSONL sweep may contain several encoders, presets, CRFs, source clips,
and score backends. Users need a compact table that answers which encoder
cleared a target VMAF at the lowest bitrate, while still showing encoders that
never cleared the target.

Adding this to `compare` would make a single command mix "run new encodes" and
"read saved rows" behaviours. Adding it to `recommend` would overload a
single-row picker with a per-encoder report contract. A separate `benchmark`
subcommand has the smallest behavioural surface and can be tested entirely
from synthetic JSONL fixtures.

## Implementation Notes

- Filter rows where `exit_status != 0` or `vmaf_score` / `bitrate_kbps` is not
  finite.
- Group by `encoder`.
- For each encoder, choose the lowest-bitrate row whose `vmaf_score` clears the
  target. If none clear, report the highest-VMAF miss as `status="unmet"`.
- Compute bitrate deltas against `--baseline-encoder` when provided, otherwise
  against the lowest-bitrate clearing encoder.
- Emit markdown, JSON, and CSV.

## Validation

Smoke command:

```bash
PYTHONPATH=tools/vmaf-tune/src .venv/bin/python -m pytest \
  tools/vmaf-tune/tests/test_benchmark.py -q
```
