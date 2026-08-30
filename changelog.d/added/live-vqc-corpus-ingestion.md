- **LIVE-VQC MOS-corpus ingestion
  ([ADR-0370](../docs/adr/0370-live-vqc-corpus-ingestion.md)).** Adds the
  LIVE Video Quality Challenge dataset (Sinno & Bovik, IEEE TIP 2019; 585
  consumer-device UGC clips, 0–100 continuous MOS scale) as the sixth
  training shard for `nr_metric_v1`. New
  `ai/scripts/live_vqc_to_corpus_jsonl.py` adapter mirrors the LSVQ /
  Waterloo IVC shape: resumable per-clip curl downloads with atomic
  tempfile-rename progress writes, ffprobe geometry probe, verbatim MOS on
  the native 0–100 scale, same corpus_v3 JSONL row contract with
  `corpus = "live-vqc"`. Accepts two manifest shapes: the canonical
  headerless two-column `<filename>,<mos>` (minimal MOS spreadsheet export)
  and the standard named-column adapter CSV used by other corpora. 18-test
  suite in `ai/tests/test_live_vqc.py` (all pass; no real corpus download
  required). Operator guide at `docs/ai/live-vqc-ingestion.md`.
