- **KonViD-1k MOS-corpus ingestion (Phase 1 of
  [ADR-0325](../docs/adr/0325-konvid-150k-corpus-ingestion.md),
  [Research-0086](../docs/research/0086-konvid-150k-corpus-feasibility.md)).**
  Adopt the Konstanz UGC KonViD-1k dataset (Hosu et al., QoMEX 2017)
  as the small-scale predecessor that validates the JSONL pipeline
  before scaling to KonViD-150k in Phase 2. New
  `ai/scripts/konvid_1k_to_corpus_jsonl.py` walks a local
  `.workingdir2/konvid-1k/` extraction, runs ffprobe per clip
  (width / height / fps / duration / pix_fmt / upstream codec), joins
  with the attribute CSV's MOS / standard-deviation / rating-count
  columns, and emits one JSONL row per clip. The schema is disjoint
  from the existing vmaf-tune Phase A `CORPUS_ROW_KEYS` (carries
  `mos` / `mos_std_dev` / `n_ratings` instead of `vmaf_score` /
  `encoder` / `preset` / `crf`) — the two corpora merge at the
  trainer level, not at the JSONL level. Refuses (exit 2 with a
  pointer to Phase 2) when handed a > 1500-row attributes CSV, the
  KonViD-150k mis-mount sentinel. ffprobe is wired through an
  injectable `runner` seam so unit tests run without ffprobe / the
  corpus on disk. License posture follows ADR-0310 / ADR-0325:
  corpus + per-clip MOS stay local under `.workingdir2/`; only the
  derived ONNX models redistribute. User docs:
  [`docs/ai/konvid-1k-ingestion.md`](../docs/ai/konvid-1k-ingestion.md).
  Tests under `ai/tests/test_konvid_1k.py` cover ffprobe geometry
  parse, broken-clip skip-and-continue, 150k CSV refusal, all three
  MOS columns surviving round-trip, alias-header tolerance, append /
  dedup on re-run, and pinned `corpus` / `corpus_version` metadata.
