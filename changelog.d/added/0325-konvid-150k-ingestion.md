- **KonViD-150k MOS-corpus ingestion (Phase 2 of
  [ADR-0325](../docs/adr/0325-konvid-150k-corpus-ingestion.md),
  [Research-0086](../docs/research/0086-konvid-150k-corpus-feasibility.md)).**
  Scale up the Konstanz UGC ingestion from KonViD-1k (Phase 1, ~1.2 k
  clips) to the full KonViD-150k corpus (~150 k clips, ~120-200 GB
  working set). New `ai/scripts/konvid_150k_to_corpus_jsonl.py`
  pulls each clip per-URL via `curl`, records progress in
  `.workingdir2/konvid-150k/.download-progress.json` (atomic
  tempfile-rename writes) so `Ctrl-C` + re-run is lossless, runs
  ffprobe per clip for the standard
  width / height / fps / duration / pix_fmt / upstream-codec
  geometry, joins with the manifest CSV's MOS / SD / rating-count
  columns, and emits one JSONL row per surviving clip. Tolerates the
  cited 5-8 % YouTube-takedown / region-block attrition: each
  download failure is logged with a reason and persisted to the
  progress JSON as non-retriable; an advisory WARNING fires when the
  failure rate exceeds `--attrition-warn-threshold` (default 10 %).
  Refuses (exit 2 with a Phase-1 hint) when handed a `<` 5 000-row
  manifest CSV, the inverse of the Phase-1 1 500-row mis-mount
  guard. Same JSONL schema as Phase 1 modulo
  `corpus = "konvid-150k"` and `corpus_version = "konvid-150k-2019"`.
  Records `encoder_upstream` from ffprobe verbatim — the
  `"ugc-mixed"` collapse for ENCODER_VOCAB v4 is a separate PR per
  ADR-0325 §Phase 2. Both `curl` and `ffprobe` go through an
  injectable `runner` seam so unit tests run without either binary
  or the corpus on disk. License posture follows ADR-0310 /
  ADR-0325: corpus + per-clip MOS stay local under
  `.workingdir2/`; only the derived ONNX models redistribute. User
  docs: [`docs/ai/konvid-150k-ingestion.md`](../docs/ai/konvid-150k-ingestion.md).
  Tests under `ai/tests/test_konvid_150k.py` cover resumable-resume
  (partial progress JSON + restart picks up where the prior run left
  off), attrition tolerance (8 % failure rate completes with WARNING),
  refuse-1k cutoff (1.2 k-row CSV exits with Phase-1 hint), atomic
  progress-file writes, ffprobe geometry parse, broken-clip skip,
  MOS-column round-trip (canonical + alias headers), append+dedup on
  re-run, and pinned `corpus` / `corpus_version` metadata.
