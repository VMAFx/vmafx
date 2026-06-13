# Research-0086: KonViD-150k corpus feasibility for the lusoris fork

- **Status**: Draft
- **Date**: 2026-05-07
- **Author**: @Lusoris
- **Tags**: ai, training, corpus, license, fork-local

## Question

Should the lusoris fork ingest [KonViD-150k](https://database.mmsp-kn.de/konvid-150k-vqa-database.html)
(University of Konstanz) as a *third* training shard for
`fr_regressor_v2` / `vmaf_tiny` alongside the Netflix Public Drop
(9 ref + 70 dis YUVs, [ADR-0309](../adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md))
and BVI-DVC (~120 ref YUVs, [ADR-0310](../adr/0310-bvi-dvc-corpus-ingestion.md))?

## What KonViD-150k is

- **Size**: ~150,000 short videos (typically 5–10 s each), each with
  ≥ 5 crowdworker MOS (Mean Opinion Score) ratings on a 1–5 scale.
  The clips were sampled from YouTube-UGC and Vimeo, so the content
  distribution is *user-generated* — phone footage, screen recording,
  vlogs, sports, animation, gaming — not the cinematic / studio
  content the Netflix and BVI-DVC corpora over-index on.
- **Form**: pre-encoded MP4 distorted clips with their per-clip
  subjective MOS. **There is no separate raw reference**; each clip
  is its own opinion datum.
- **License**: Research / non-commercial. Distribution is by URL list +
  a downloader script the dataset team publishes; clips are pulled
  from YouTube/Vimeo, so download time depends on each contributor's
  network (the dataset is *not* a single mirrored archive).
- **Companion**: KonViD-1k (the predecessor, 1,200 clips with the same
  MOS protocol; widely cited; fits in ~5 GB).

## What it gives the fork that we don't already have

1. **User-generated content (UGC) coverage.** Both Netflix (cinematic)
   and BVI-DVC (controlled-resolution Bristol-VI-Lab references) miss
   the long tail of *real-world* content the fork's users actually
   encode (per the colleague's note in
   [docs/research/0061-vmaf-tune-capability-audit.md](0061-vmaf-tune-capability-audit.md)
   and the ChatGPT-vision text the user shared
   2026-05-07). KonViD's YouTube-UGC distribution is the closest
   open-licence proxy for "what private encoders see on a typical
   day" the field has.
2. **MOS ground truth, not VMAF.** KonViD's labels are *subjective
   scores from humans*, not algorithmic VMAF outputs. That's a
   different kind of signal:
   - Used as a **regression target** it lets us train a head that
     predicts MOS directly — the next step the ChatGPT-vision text
     calls "alternative perceptual metrics that approach VMAF-level
     correlation".
   - Used as a **held-out validation set** for the existing VMAF-
     prediction stack it lets us measure how well our predictions
     correlate with humans (PLCC / SROCC / RMSE on MOS), not just
     with libvmaf-CPU's per-frame teacher.
3. **Scale.** 150 k samples vs the Netflix drop's 70 distorteds and
   BVI-DVC's ~720 distorteds. Different bias / variance trade-off.

## What it *doesn't* give us

- **Bit-exact reference.** Every clip is the user-uploaded encode, so
  we can't run libvmaf against it (no reference). KonViD samples are
  for MOS-correlation work, not for bit-exact distortion measurement.
- **Encoder distribution control.** The clips come from arbitrary
  upload pipelines (YouTube-VP9, x264, x265, varying CRFs) — they
  exercise *real-world* encoders but we can't pin "what encoder"
  per row. For codec-aware regressors (`fr_regressor_v2_ensemble`,
  ENCODER_VOCAB v3) this means the *encoder* one-hot collapses or
  takes a new "ugc-mixed" slot.

## Disk and bandwidth budget

- KonViD-150k full: published numbers vary; the maintainers cite
  ~80 GB after deduplication, but with the YouTube/Vimeo originals at
  full resolution (mostly 540p / 720p / 1080p) the realistic working
  set is **~120–200 GB** including transcode-to-yuv working files,
  comparable in size to BVI-DVC + Netflix combined.
- Download wall-clock: hours-to-days depending on YouTube
  rate-limiting; not all clips are still available (typical
  sub-set hit rate is 92–98 % per recent papers).
- Compressed-tarball transit: at HEVC-lossless we'd expect ~50 GB
  (consistent with the
  [`prepare-gdrive-bundle.sh`](../../tools/ensemble-training-kit/prepare-gdrive-bundle.sh)
  ratio for VP9-decoded UGC).

## Open questions to resolve in the ADR

1. **Target signal**: do we predict MOS or use it as a held-out validator?
   Recommendation: both, but in stages. Phase 1 = held-out validator
   for the VMAF-prediction stack; Phase 2 = train a sibling MOS head.
2. **Codec-aware regressor**: how does the clips' opaque encoder
   distribution interact with ENCODER_VOCAB v3? Recommendation:
   collapse into a single `"ugc-mixed"` slot; do not try to recover
   per-clip encoder.
3. **Storage policy**: same as ADR-0310 — local-only, gitignored;
   compressed-tarball share via gdrive (Research-0086 piggybacks on
   the prepare-gdrive-bundle.sh / extract-corpus.sh contract).
4. **Lighter alternative**: KonViD-1k as a sanity-check first.
   1.2 k clips, ~5 GB, same protocol, used for CV. Drop this in tree
   first, validate the pipeline, then scale to 150 k.
5. **Ethics / consent**: the KonViD MOS labels are crowdworker
   opinions; the source clips were public uploads. Standard
   academic-fair-use covers the labels; we redistribute neither
   the clips nor the MOS — only derived statistics + the ONNX models
   trained on them. Same shape as ADR-0310.

## Recommendation

Adopt KonViD-150k under a phased plan, codified in
[ADR-0325](../adr/0325-konvid-150k-corpus-ingestion.md):

- **Phase 0** (this digest): land the ADR + ingestion plan; no code.
- **Phase 1**: ship `ai/scripts/konvid_1k_to_corpus_jsonl.py` (the
  small predecessor — fast iteration, validates the parquet shape).
- **Phase 2**: scale to KonViD-150k via `ai/scripts/konvid_150k_to_corpus_jsonl.py`,
  add the `"ugc-mixed"` `ENCODER_VOCAB` slot, run held-out PLCC/SROCC
  vs `fr_regressor_v2_ensemble`'s MOS head.
- **Phase 3**: train a sibling MOS regressor; gate via the same
  ADR-0303 production-flip protocol the VMAF regressor uses.

## Alternatives considered

- **YouTube-UGC dataset (Google, 1.5k clips, MOS + VMAF-CPU labels)** —
  smaller, simpler license, has VMAF for free. Considered as a
  "Phase 1.5" in the plan; not a replacement for KonViD because the
  scale is two orders of magnitude smaller.
- **LIVE-VQC / LIVE-Qualcomm** — comparable academic UGC datasets;
  smaller (585 / 208 clips) and similar license. KonViD has the best
  scale.
- **Skip subjective MOS entirely; rely on the existing VMAF teacher**
  — the user explicitly asked for "alternative perceptual metrics
  that can approach VMAF-level reliability and human-perception
  correlation" (ChatGPT-vision text 2026-05-07). MOS *is* the
  human-perception ground truth; skipping it concedes that goal.

## References

- KonViD-150k: <https://database.mmsp-kn.de/konvid-150k-vqa-database.html>
- KonViD-1k: <https://database.mmsp-kn.de/konvid-1k-database.html>
- ADR-0309 (Netflix Public Drop): [docs/adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md](../adr/0309-fr-regressor-v2-ensemble-real-corpus-retrain.md)
- ADR-0310 (BVI-DVC ingestion): [docs/adr/0310-bvi-dvc-corpus-ingestion.md](../adr/0310-bvi-dvc-corpus-ingestion.md)
- ADR-0302 (ENCODER_VOCAB v3): [docs/adr/0302-encoder-vocab-v3-schema-expansion.md](../adr/0302-encoder-vocab-v3-schema-expansion.md)
- Research-0061 (vmaf-tune capability audit): [docs/research/0061-vmaf-tune-capability-audit.md](0061-vmaf-tune-capability-audit.md)
