- **YouTube UGC MOS-corpus ingestion
  ([ADR-0368](../docs/adr/0368-youtube-ugc-corpus-ingestion.md),
  [Research-0091](../docs/research/0091-youtube-ugc-corpus-feasibility.md)).**
  Add the Google YouTube UGC dataset (Wang, Inguva, Adsumilli;
  MMSP 2019 + CVPR 2021 transcoded follow-up; ~1500 community
  UGC originals + four-rate transcoded ladder; CC-BY) as a
  fourth MOS-corpus shard alongside LSVQ (ADR-0333),
  KonViD-150k (ADR-0325 Phase 2), and BVI-DVC (ADR-0310). New
  `ai/scripts/youtube_ugc_to_corpus_jsonl.py` adapter mirrors
  the LSVQ shape verbatim — resumable per-URL `curl` downloads
  with atomic tempfile-rename progress writes (`Ctrl-C` +
  re-run is lossless), ffprobe-driven
  width / height / fps / duration / pix_fmt / upstream-codec
  geometry probe, MOS / SD / rating-count round-trip from the
  canonical original-video listing CSV
  (<https://storage.googleapis.com/ugc-dataset/original_videos.csv>),
  and the same JSONL row contract modulo `corpus = "youtube-ugc"`
  and `corpus_version = "ugc-2019-orig"` (or
  `ugc-2020-transcoded-mean` for the transcoded-mean variant).
  The dataset is hosted in the public-readable Google Cloud
  Storage bucket `gs://ugc-dataset/` with the
  `allUsers:objectViewer` IAM role applied — no sign-up, no
  request form, no API key. The adapter synthesises download
  URLs from the public bucket prefix when the manifest lacks
  a `url` column (the canonical `original_videos.csv` does
  not). Refuses (exit 2) when handed a `<` 200-row CSV;
  defaults to a 300-row laptop-class subset with `--full`
  opting into whole-corpus ingestion (~2 TB working set on the
  full original-videos manifest). MOS is recorded verbatim on
  the dataset's native 1.0-5.0 Likert scale (no rescaling at
  ingest time), matching the LSVQ / KonViD-150k convention.
  Records `encoder_upstream` from ffprobe verbatim — the
  `"ugc-mixed"` collapse for ENCODER_VOCAB v4 stays
  trainer-side. Both `curl` and `ffprobe` go through an
  injectable `runner` seam so unit tests run without either
  binary or the corpus on disk. License posture follows
  ADR-0310 / ADR-0325 / ADR-0333: corpus + per-clip MOS stay
  local under `.workingdir2/youtube-ugc/`; only the derived
  `nr_metric_v1_*.onnx` weights ship, with CC-BY attribution
  travelling alongside. User docs:
  [`docs/ai/youtube-ugc-ingestion.md`](../docs/ai/youtube-ugc-ingestion.md).
  Tests under `ai/tests/test_youtube_ugc.py` (18 cases) cover
  resumable-resume, attrition tolerance, refuse-tiny cutoff,
  atomic progress-file writes, ffprobe geometry parse,
  broken-clip skip, MOS-column round-trip (canonical + alias
  headers including `DMOS`), bare-stem `vid` -> `.mp4` suffix,
  append+dedup on re-run, `--max-rows` / `--full` cap
  behaviour, and the synthesised-bucket-URL path for manifests
  that omit the `url` column.
