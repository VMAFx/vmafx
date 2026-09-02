# Research 0684: CHUG extraction report provenance

## Question

Which CHUG feature-extraction artifacts still lacked ADR-0661 provenance before
HDR MOS training starts?

## Findings

- `chug_extract_features.py --split-manifest` writes the local
  content-to-train/validation/test map that defines the CHUG holdout boundary.
- `chug_extract_features.py --audit-output` writes the HDR metadata preflight
  used to catch malformed PQ/HLG rows before training.
- Both files are durable local evidence, but neither recorded the exact CHUG
  JSONL, clip/cache directory, VMAF binary, argv, or extraction arguments that
  produced them.
- The feature JSONL rows already carry model-facing metadata; the missing piece
  is report-level provenance for the split and audit JSON files.

## Decision

Add optional `run_provenance` plumbing to `write_split_manifest()` and
`audit_chug_hdr_metadata()`, and have the CLI pass an ADR-0661 block covering
the input JSONL, clips/cache directories, VMAF binary, split/audit targets,
feature output, argv, and parsed arguments. Keep feature-row JSONL output
unchanged.

## Commands

```bash
rg -n "split-manifest|audit-output|run_provenance" ai/scripts/chug_extract_features.py ai/tests/test_chug.py docs/ai/chug-ingestion.md
.venv/bin/python -m pytest ai/tests/test_chug.py -q
```
