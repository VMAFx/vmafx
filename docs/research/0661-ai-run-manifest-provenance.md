<!-- markdownlint-disable MD013 -->
# Research-0661: AI run manifest provenance

## Question

How should local AI training and export scripts record enough provenance to
reproduce MOS-head artifacts without turning gitignored corpus paths into a
committed CI contract?

## Findings

- The active AI refresh work produces many local artifacts: KonViD MOS heads,
  CHUG HDR MOS heads, feature JSONL/parquet tables, model cards, and manifest
  sidecars.
- Existing sidecars record model and metric metadata, but the exact user command
  and input/output path set are inconsistent across script families.
- CHUG HDR training is intentionally exposed through
  `ai/scripts/train_chug_hdr_mos_head.py`, while implementation is shared with
  `ai/scripts/train_konvid_mos_head.py`. A provenance block must preserve that
  user-facing identity instead of making all CHUG runs look KonViD-named.
- Full environment captures are too noisy for this fork's local workflow. Host
  package lists, absolute cache directories, and device state would make
  sidecars hard to compare while still not guaranteeing reproducibility.
- A small helper can provide the stable part every script needs: schema name,
  entrypoint path/hash, normalized CLI arguments, named inputs, named outputs,
  and hashes for files that already exist.

## Decision pressure

The useful operator question is "what command and inputs produced this file?"
not "what was the entire local machine state?" The sidecar should therefore be
deterministic, compact, and honest about missing future output paths.

## Implementation shape

- Add `aiutils.run_manifest` as the shared provenance helper.
- Write `run_provenance` into MOS-head JSON manifests.
- Record CHUG wrapper entrypoint identity and add `shared_trainer` only when the
  wrapper delegates into the shared KonViD training loop.
- Keep local corpus paths visible in gitignored artifacts, but do not require
  those paths to exist in CI.

## Validation targets

- `.venv/bin/python -m pytest ai/tests/test_run_manifest.py ai/tests/test_train_konvid_mos_head.py -q`
- `.venv/bin/ruff check ai/src/aiutils/run_manifest.py ai/tests/test_run_manifest.py ai/scripts/train_konvid_mos_head.py ai/scripts/train_chug_hdr_mos_head.py ai/tests/test_train_konvid_mos_head.py`
- `make format-check`
- `scripts/docs/concat-adr-index.sh --check`
- `.venv/bin/mkdocs build --strict`

## References

- [ADR-0661](../adr/0661-ai-run-manifest-provenance.md)
- [ADR-0658](../adr/0658-project-modernization-audit.md)
