- **Multi-corpus MOS aggregation for the FR-regressor / predictor v2
  trainer ([ADR-0340](../docs/adr/0340-multi-corpus-aggregation.md)).**
  New `ai/scripts/aggregate_corpora.py` reads multiple per-corpus MOS
  JSONLs (KonViD-1k / KonViD-150k / LSVQ / Waterloo IVC 4K-VQA /
  YouTube UGC / Netflix Public), applies a per-corpus *affine* scale
  conversion onto the unified 0–100 (VMAF-aligned) axis, tags every
  row with `corpus_source` provenance, and deduplicates cross-corpus
  duplicates by keeping the row with tighter `mos_std_dev`. Per
  `feedback_no_test_weakening`, rows with out-of-range native MOS or
  unknown `corpus` labels are dropped (counted, never silently
  rescaled) so the unified training-target distribution is never
  silently widened. New `ai/scripts/run_aggregated_training.sh`
  discovers conventional `.workingdir2/` JSONL locations, runs the
  aggregator on whatever shards are present, and forwards the
  unified output to `train_predictor_v2_realcorpus.py` (PR #487).
  Tests under `ai/tests/test_aggregate_corpora.py` cover per-corpus
  conversion accuracy, cross-corpus dedup, partial-corpus runs,
  missing-input degradation, schema violations, and unknown-label
  handling. User docs:
  [`docs/ai/multi-corpus-aggregation.md`](../docs/ai/multi-corpus-aggregation.md).
