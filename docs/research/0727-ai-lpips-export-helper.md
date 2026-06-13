<!-- markdownlint-disable MD060 -->
# Research-0727 — LPIPS exporter helper alignment

## Problem

`ai/lpips_export.py` predated the shared AI helper sweep. It still owned local
parser construction, hash calculation, and sidecar JSON writing, and the human
model card documented `--out` / `--sidecar` even though the script only
accepted `--output` and always wrote `<output>.json`.

That made LPIPS the remaining visible exception to the current exporter
contract: direct AI tools should bootstrap `ai/src`, capture raw argv for
provenance, use shared hashing/strict JSON helpers, and keep docs aligned with
the actual CLI.

## Decision

Extend `ai/scripts/_script_bootstrap.py` so the same bootstrap helper supports
top-level legacy exporters under `ai/*.py`, then update `lpips_export.py` to:

- use `make_argument_parser()` and `collect_cli_argv()`;
- accept `--out` as a compatibility alias for `--output`;
- accept a documented `--sidecar` path;
- write sidecar JSON through `write_manifest_json()`;
- embed `run_provenance` in the sidecar; and
- use `aiutils.file_utils.sha256()` instead of a local hash helper.

## Alternatives considered

| Option | Benefit | Cost | Outcome |
|---|---|---|---|
| Only fix the docs | Smallest diff | Leaves LPIPS as helper-drift exception and still no provenance | Rejected |
| Only add `--sidecar` | Fixes the broken documented flag | Leaves local parser/hash/JSON code in place | Rejected |
| Move LPIPS into `ai/scripts/` | Stronger layout consistency | Path is already user-facing in docs/model cards | Deferred |
| Extend the shared bootstrap and modernize in place | Fixes the user-facing CLI and aligns helper policy without moving the script | Small bootstrap generalization | Chosen |

## Validation

```bash
.venv/bin/python -m pytest ai/tests/test_script_bootstrap.py ai/tests/test_lpips_export.py -q
.venv/bin/ruff check ai/scripts/_script_bootstrap.py ai/lpips_export.py ai/tests/test_script_bootstrap.py ai/tests/test_lpips_export.py
.venv/bin/black --check ai/scripts/_script_bootstrap.py ai/lpips_export.py ai/tests/test_script_bootstrap.py ai/tests/test_lpips_export.py
```
