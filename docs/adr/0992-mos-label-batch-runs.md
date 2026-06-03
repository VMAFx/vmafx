<!-- markdownlint-disable MD013 MD060 -->
# ADR-0992: MOS-label batch-run manifests for KonViD and CHUG

- **Status**: Accepted
- **Date**: 2026-06-03
- **Deciders**: Lusoris
- **Tags**: ai, mos, training, corpus, konvid, chug, fork-local

## Context

The `batch_materialize_mos_labels.py` script (ADR-0675) was merged
(archived PR #1498), but no corpus-specific batch manifests ship with
the code. Operators who want to join MOS labels onto their KonViD or
CHUG feature extracts must either write a manifest from scratch or
remember the correct column names, key regexes, and path conventions
for each corpus.

In addition, the `test_batch_materialize_mos_labels.py` tests were
broken: the `_load_module()` helper executed `batch_materialize_mos_labels.py`
without inserting `ai/scripts/` into `sys.path`, so `_script_bootstrap`
was not importable when pytest ran from the repo root.

## Decision

1. Add `ai/configs/mos-label-batch-konvid.json` — the batch manifest
   for joining KonViD-1k and KonViD-150k MOS labels onto extracted
   feature parquets. Defaults set `key_normalize: basename`,
   `feature_key_regex` and `label_key_regex` to `([0-9]{6,})` (KonViD
   clip IDs are 6+ digit integers), and `min_match_rate: 0.95`.
   Table entries wire `feature_key_column: key` to
   `label_key_column: src` for the conventional per-corpus JSONL shapes.

2. Add `ai/configs/mos-label-batch-chug.json` — the batch manifest for
   joining CHUG UGC-HDR MOS labels. Uses `key_normalize: raw` because
   CHUG video IDs are opaque strings (not file paths), and joins on
   `chug_video_id` on both the feature and label sides.

3. Add `ai/tests/test_mos_label_batch_runs_smoke.py` — validates that
   both manifests are well-formed JSON, parse correctly via
   `load_batch_manifest()`, encode the expected corpus-specific key
   columns and normalisation policy, and run end-to-end with synthetic
   data (no real corpus files required on CI).

4. Fix the pre-existing `sys.path` bug in
   `ai/tests/test_batch_materialize_mos_labels.py`: insert `ai/scripts/`
   before executing the script module so the tests pass when pytest is
   invoked from the repo root (the standard CI invocation).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Ship manifests as YAML | Consistent with other configs in `ai/configs/` | `batch_materialize_mos_labels.py` reads JSON; a YAML manifest would need a new parser path | Rejected: avoid adding a parser path for a config format the tool does not support |
| Embed corpus manifests in a single `mos-label-batch-all.json` | Single file for all corpora | Different corpora have different key schemas and path conventions; a single file mixes them without per-corpus clarity | Rejected: operator intent is clearer with one manifest per corpus family |
| Store manifests under `.workingdir2/` | Keeps config next to corpus artifacts | `.workingdir2/` is gitignored; operators on fresh clones would have no starting point | Rejected: manifests encode code-time policy (column names, regex, thresholds), not runtime state |

## Consequences

- **Positive**: Operators can run `python ai/scripts/batch_materialize_mos_labels.py
  --manifest ai/configs/mos-label-batch-konvid.json` without constructing
  the manifest by hand. The manifests are version-controlled and stable
  across machines. The smoke test catches manifest regressions and
  documents the expected corpus key schemas.
- **Positive**: Pre-existing `test_batch_materialize_mos_labels.py` breakage
  is fixed; all three existing tests now pass from the repo root.
- **Negative**: Manifest paths use defaults relative to `ai/configs/` which
  expect the standard `runs/` and `.workingdir2/` layout; operators with
  non-default corpus layouts must pass `--base-dir`.
- **Neutral / follow-ups**: Real batch runs (with actual corpus files) remain
  operator-local; the manifests document the expected paths but do not ship
  the corpus data. The CHUG manifest points to `.corpus/chug/` for the
  input JSONL/parquet (convention from `chug_extract_features.py`) and
  `.workingdir2/chug/runs/` for output (convention from
  `train_chug_hdr_mos_head.py`).

## References

- [ADR-0663](0663-mos-label-materializer.md) — table-side MOS label
  materializer.
- [ADR-0675](0675-mos-label-materializer-batch-manifest.md) — batch
  manifest schema and runner.
- [ADR-0325](0325-konvid-corpus-ingestion.md) — KonViD-1k / KonViD-150k
  corpus ingestion.
- Source: req — "MOS-label batch runs (KonViD/CHUG) — Script merged (PR #1498 archived) but no *.mos_label.* artifacts."
