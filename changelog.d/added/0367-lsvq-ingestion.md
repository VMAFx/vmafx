- **LSVQ MOS-corpus ingestion
  ([ADR-0367](../docs/adr/0367-lsvq-corpus-ingestion.md),
  [Research-0086](../docs/research/0086-tiny-ai-sota-deep-dive-2026-05-08.md)).**
  Add the LIVE Large-Scale Social Video Quality dataset (Ying et
  al. ICCV 2021, ~39 K UGC videos, ~5.5 M ratings, CC-BY-4.0) as a
  third MOS-corpus shard alongside KonViD-150k (ADR-0325 Phase 2)
  and BVI-DVC (ADR-0310). New
  `ai/scripts/lsvq_to_corpus_jsonl.py` adapter mirrors the
  KonViD-150k Phase 2 shape verbatim — resumable per-URL `curl`
  downloads with atomic tempfile-rename progress writes
  (`Ctrl-C` + re-run is lossless), ffprobe-driven
  width / height / fps / duration / pix_fmt / upstream-codec
  geometry probe, MOS / SD / rating-count round-trip from the
  canonical Hugging Face split CSV
  ([`teowu/LSVQ-videos`](https://huggingface.co/datasets/teowu/LSVQ-videos)),
  and the same JSONL row contract modulo `corpus = "lsvq"` and
  `corpus_version = "lsvq-2021"`. Refuses (exit 2) when handed a
  `<` 1 000-row CSV; defaults to a 500-row laptop-class subset
  with `--full` opting into whole-corpus ingestion (~500 GB
  working set on `LSVQ_whole_train`). MOS is recorded verbatim on
  the dataset's native 1.0–5.0 Likert scale (no rescaling at
  ingest time), matching the KonViD-150k convention. Records
  `encoder_upstream` from ffprobe verbatim — the
  `"ugc-mixed"` collapse for ENCODER_VOCAB v4 stays trainer-side.
  Both `curl` and `ffprobe` go through an injectable `runner`
  seam so unit tests run without either binary or the corpus on
  disk. License posture follows ADR-0310 / ADR-0325: corpus +
  per-clip MOS stay local under `.workingdir2/lsvq/`; only the
  derived `nr_metric_v1_*.onnx` weights ship, with CC-BY-4.0
  attribution travelling alongside. User docs:
  [`docs/ai/lsvq-ingestion.md`](../docs/ai/lsvq-ingestion.md).
  Tests under `ai/tests/test_lsvq.py` (17 cases) cover
  resumable-resume, attrition tolerance, refuse-tiny cutoff,
  atomic progress-file writes, ffprobe geometry parse,
  broken-clip skip, MOS-column round-trip (canonical + alias
  headers), bare-stem `name` → `.mp4` suffix, append+dedup on
  re-run, and `--max-rows` / `--full` cap behaviour.
