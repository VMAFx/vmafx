- **BVI-DVC corpus ingestion for `fr_regressor_v2`
  ([ADR-0310](../docs/adr/0310-bvi-dvc-corpus-ingestion.md),
  [Research-0082](../docs/research/0082-bvi-dvc-corpus-feasibility.md)).**
  Adopt the Bristol VI Lab BVI-DVC reference corpus (Ma, Zhang, Bull
  2021) as a second training shard alongside the Netflix Public drop.
  New `ai/scripts/bvi_dvc_to_corpus_jsonl.py` re-shapes the existing
  parquet pipeline's cached libvmaf JSON into vmaf-tune Phase A
  `CORPUS_ROW_KEYS` rows; new `ai/scripts/merge_corpora.py`
  concatenates Netflix + BVI-DVC shards with `(src_sha256, encoder,
  preset, crf)` deduplication and schema validation. Triples training
  corpus and expands LOSO partitioning from 9 source-folds to 9 + N.
  License is research-only — corpus stays local under
  `.workingdir2/`; only derived `fr_regressor_v2_*.onnx` weights ship.
  Production-weights flip stays gated on
  [ADR-0303](../docs/adr/0303-fr-regressor-v2-ensemble-flip.md). User
  docs:
  [`docs/ai/bvi-dvc-corpus-ingestion.md`](../docs/ai/bvi-dvc-corpus-ingestion.md).
  Tests under `ai/tests/test_merge_corpora.py` cover concat-with-dedup
  and schema-violation rejection on synthetic fixtures (no GPU / heavy
  feature extraction in CI).
