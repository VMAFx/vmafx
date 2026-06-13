- **FR-from-NR corpus adapter for KonViD-150k and other no-reference shards
  ([ADR-0346](../docs/adr/0346-fr-features-from-nr-corpus.md),
  [ADR-0325](../docs/adr/0325-konvid-150k-corpus-ingestion.md)).**
  Bridges the FR predictor schema (`fr_regressor_v2_ensemble`,
  `fr_regressor_v3`) to no-reference corpora that carry MOS but no
  clean reference YUV. New
  [`tools/vmaf-tune/src/vmaftune/fr_from_nr_adapter.py`](../tools/vmaf-tune/src/vmaftune/fr_from_nr_adapter.py)
  implements the *decode-original-as-reference* pattern: ffprobe each
  upload, ffmpeg-decode it to raw YUV, treat that decoded YUV as the
  FR reference, re-encode at a configurable CRF sweep (default
  `(18, 23, 28, 33, 38)`), score the canonical-6 against each
  re-encode via the existing `vmaftune.corpus.iter_rows` Phase A
  pipeline. Each NR input produces `len(crf_sweep)` FR corpus rows
  (5x multiplier on the K150K shard, 148k → ~742k FR rows). Output
  rows match the existing :data:`vmaftune.CORPUS_ROW_KEYS` schema
  (no schema bump) plus `nr_source` / `nr_mos` / `fr_from_nr`
  provenance keys so downstream trainers can stratify by reference
  pristineness. Companion runbook
  [`ai/scripts/extract_k150k_features.sh`](../ai/scripts/extract_k150k_features.sh)
  wraps the adapter for the full overnight K150K pass. Honest caveat
  documented in the ADR §Consequences §Negative and in
  [`docs/ai/fr-from-nr-adapter.md`](../docs/ai/fr-from-nr-adapter.md):
  the "reference" is the re-decoded upload, not a pristine master,
  so FR scores measure delta-vs-already-distorted-source — methodology
  matches LIVE-VQA / LIVE-VQC / KonViD-1k synthetic-distortion
  precedent. Smoke test:
  `python -m pytest tools/vmaf-tune/tests/test_fr_from_nr_adapter.py`
  (13 tests, ffprobe / ffmpeg / vmaf all mocked at the runner seam).
