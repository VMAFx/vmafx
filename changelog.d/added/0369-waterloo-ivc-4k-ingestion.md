- **Waterloo IVC 4K-VQA MOS-corpus ingestion
  ([ADR-0369](../docs/adr/0369-waterloo-ivc-4k-corpus-ingestion.md),
  [Research-0091](../docs/research/0091-waterloo-ivc-4k-corpus-feasibility.md)).**
  Add the University of Waterloo Image and Vision Computing
  Laboratory 4K Video Quality Database (Li, Duanmu, Liu, Wang;
  ICIAR 2019, 20 pristine 4K source sequences × 5 codecs
  (H.264/AVC, H.265/HEVC, VP9, AVS2, AV1) × 3 resolutions
  (540p / 1080p / 2160p) × 4 distortion levels = 1 200
  distorted clips with controlled-subjective-study MOS) as a
  fourth MOS-corpus shard alongside BVI-DVC (ADR-0310),
  KonViD-150k (ADR-0325 Phase 2), and LSVQ (ADR-0333). New
  `ai/scripts/waterloo_ivc_to_corpus_jsonl.py` adapter mirrors
  the LSVQ / KonViD-150k Phase 2 shape — resumable per-URL
  `curl` downloads with atomic tempfile-rename progress writes
  (`Ctrl-C` + re-run is lossless), ffprobe-driven
  width / height / fps / duration / pix_fmt / upstream-codec
  geometry probe, MOS / SD / rating-count round-trip, and the
  same JSONL row contract modulo `corpus = "waterloo-ivc-4k"`
  and `corpus_version = "waterloo-ivc-4k-201908"`. Closes the
  2160p resolution-bin gap in the BVI-DVC + KonViD-150k +
  LSVQ union flagged in research digest #465. Auto-detects
  between the upstream **canonical headerless 5-tuple**
  (`encoder, video_number, resolution, distortion_level, mos`
  — the shape of
  [`scores.txt`](https://ivc.uwaterloo.ca/database/4KVQA/201908/scores.txt))
  and the standard LSVQ-shape named-column CSV when an
  operator pre-mangles. Refuses (exit 2) when handed a
  `<` 100-row CSV; defaults to a 100-row laptop-class subset
  with `--full` opting into whole-corpus ingestion (~multi-TB
  working set on the 1 200-clip + 20-source 2160p set). MOS
  is recorded **verbatim on the dataset's native 0–100 raw
  scale** (NOT 1–5 like KonViD / LSVQ) — cross-corpus
  rescaling is a trainer-side concern; see ADR-0369
  §Consequences and `docs/rebase-notes.md`. Records
  `encoder_upstream` from ffprobe verbatim — the
  `"professional-graded"` ENCODER_VOCAB v4 slot routing for
  Waterloo's studio-captured sources stays trainer-side.
  Both `curl` and `ffprobe` go through an injectable
  `runner` seam so unit tests run without either binary or
  the corpus on disk. License posture follows ADR-0310 /
  ADR-0325 / ADR-0333: corpus + per-clip MOS stay local
  under `.workingdir2/waterloo-ivc-4k/`; only the derived
  `nr_metric_v1_*.onnx` weights ship, with the
  Image-and-Vision-Computing-Laboratory attribution
  travelling alongside (per the permissive academic
  licence). User docs:
  [`docs/ai/waterloo-ivc-4k-ingestion.md`](../docs/ai/waterloo-ivc-4k-ingestion.md).
  Tests under `ai/tests/test_waterloo_ivc.py` (20 cases)
  cover canonical-headerless auto-detect, standard-CSV
  parse, alias headers, native 0–100 MOS round-trip,
  resumable resume, attrition tolerance, refuse-tiny
  cutoff, atomic progress-file writes, ffprobe geometry
  parse (HEVC / AV1 at 4K), broken-clip skip, append+dedup
  on re-run, encoder_upstream verbatim, and `--max-rows` /
  `--full` cap behaviour.
