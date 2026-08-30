- KonViD-150k corpus is materialized locally at `.workingdir2/konvid-150k/`
  (179 GB, 307 682 extracted MP4 clips, k150ka/k150kb scores CSV, JSONL,
  manifest) — confirmed 2026-05-15. The 2026-05-15 deep audit (slices C
  + E) and `docs/state.md` `T-MOS-HEAD-PRODFLIP` row had treated corpus
  availability as the blocker; this PR flips
  [ADR-0325](../docs/adr/0325-konvid-150k-corpus-ingestion.md) from
  Proposed → Accepted, refreshes
  [`docs/ai/konvid-150k-ingestion.md`](../docs/ai/konvid-150k-ingestion.md)
  with the local-inventory section, and rewrites the
  [T-MOS-HEAD-PRODFLIP `state.md`](../docs/state.md) row to reflect that
  only the real-corpus PLCC verification gate remains. Real-corpus
  training pass is queued as Batch 22 of
  `.workingdir/GAP-FILL-PLAN-2026-05-15.md`, blocked behind the in-flight
  CHUG feature extraction's GPU usage.
