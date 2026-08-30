<!-- markdownlint-disable MD060 -->
# ADR-0413: YouTube UGC corpus ingestion for `nr_metric_v1`

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: @Lusoris
- **Tags**: ai, training, corpus, license, fork-local

## Context

`nr_metric_v1` (~19 K params) is the fork's tiny no-reference VQA
model. ADR-0325 Phase 2 ingests KonViD-150k and ADR-0333 ingests
LSVQ; the [contributor-pack research digest #465][research-0091]
flags **YouTube UGC** (Wang, Inguva, Adsumilli; MMSP 2019) as the
under-represented *content-distribution* axis of the training mix.
KonViD draws content from Flickr, LSVQ from Internet Archive
social-video; YouTube UGC adds the genuine YouTube content
distribution (gaming, vlogs, lyric-videos, HDR clips, animation,
…) that the production scoring path cares about most.

The corpus is hosted by Google in the public-readable Google Cloud
Storage bucket `gs://ugc-dataset/`. The bucket has the
`allUsers:objectViewer` IAM role applied — there is no request
form, no sign-up, no API key. License is Creative Commons
Attribution per the bucket-root `ATTRIBUTION` file. Working set
is ~2 TB end-to-end (~1500 originals at up to 4 K + the
transcoded ladder), so the ingestion adapter follows the LSVQ
posture: laptop-class default subset, whole-corpus opt-in via
`--full`.

What is missing today is a JSONL adapter that bridges the YouTube
UGC manifest CSV to the same MOS-corpus row schema the LSVQ /
KonViD-150k adapters emit, so the trainer can consume all three
shards through one loader without per-corpus branching.

## Decision

We will adopt YouTube UGC as a fourth training shard for
`nr_metric_v1` under three constraints:

1. The YouTube UGC archive, extracted clips, and any cached
   features stay **local-only** (`.workingdir2/youtube-ugc/`).
   The fork redistributes only derived `nr_metric_v1_*.onnx`
   weights, with CC-BY attribution travelling alongside.
2. The MOS-corpus row schema (introduced for KonViD-150k Phase 2
   and reused by ADR-0333 LSVQ) is the merge contract. A new
   adapter (`ai/scripts/youtube_ugc_to_corpus_jsonl.py`) emits
   one JSONL row per surviving clip with `corpus = "youtube-ugc"`.
   The schema is byte-identical to the LSVQ / KonViD-150k
   adapters' modulo the `corpus` and `corpus_version` literals.
3. Laptop-class development is the default path. The script
   ingests the first `--max-rows=300` clips by default; the
   ~2 TB whole-corpus run is opt-in via `--full`. The
   resumable-download contract from ADR-0325 Phase 2 / ADR-0333
   carries over verbatim (`.download-progress.json`, atomic
   tempfile-rename writes, non-retriable failure persistence).

The ENCODER_VOCAB v4 collapse to `"ugc-mixed"` is **not** done
here — this script records `encoder_upstream` from ffprobe
verbatim, identical to LSVQ / KonViD-150k. The trainer-side
collapse lands in a separate PR (and applies uniformly across the
three UGC shards once it does).

Per-clip scoring methodology: YouTube UGC's 2019 release (Wang
et al. MMSP 2019) provides per-original-clip MOS values on the
same 1.0-5.0 Likert scale as LSVQ / KonViD. The 2020 transcoded
follow-up (Wang et al. CVPR 2021) adds per-bitrate ratings on
transcoded outputs at four rate points (`orig` / `cbr` / `vod` /
`vodlb`); operators wanting those ratings pre-aggregate them into
a one-row-per-`orig` CSV with `corpus_version =
"ugc-2020-transcoded-mean"`. The adapter records whatever the
manifest's MOS column contains, without rescaling (matching LSVQ
/ KonViD-150k).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| KonViD-150k + LSVQ only (skip UGC) | Smallest surface; two ingestion adapters; two licence reviews. | Misses the genuine YouTube content distribution; under-represents gaming / animation / HDR; the contributor-pack digest #465 specifically flags UGC as the under-weighted axis. | Leaves measurable signal on the table; UGC is the third leg of the public-corpus tripod every modern NR-VQA paper trains on. |
| Add YouTube UGC alongside LSVQ + KonViD-150k | Adds the canonical UGC distribution; CC-BY is permissively redistributable for derived weights; adapter shape is a verbatim port of ADR-0333. | Working set ~2 TB; per-clip MOS is split across a 2019 originals release and a 2020 transcoded follow-up so operators must understand which release they have; partial-corpus runs need explicit operator opt-in. | **Chosen.** The marginal infra (one adapter mirroring ADR-0333 + 18 tests + a `--max-rows` / `--full` CLI knob + a `--bucket-prefix` knob for synthesised URLs) is small. |
| Scrape YouTube directly via `yt-dlp` | No 2 TB GCS pull; can target newer / longer-tail content. | Violates YouTube ToS; per-clip MOS values do not exist outside Google's curated release; reproducibility nil; licence posture indeterminate per clip. | Hard rejected. The Google YouTube UGC release is the only legal-and-MOS-bearing source for this content. |
| KonViD-150k + LSVQ + LIVE-VQC + YouTube UGC | Largest possible training corpus; broadest content. | LIVE-VQC redistribution licence is research-only-non-commercial; LOSO partition explodes; ADR-0287 already showed marginal ensemble gains past three-corpus regime. | Premature without a clean three-corpus measurement. The three-public-shard regime is the next step; LIVE-VQC remains gated behind a separate licence review if the three-shard run leaves PLCC headroom. |

## Consequences

- **Positive**: `nr_metric_v1` becomes trainable on the same
  large-scale UGC corpus the field uses for content-distribution
  breadth (DOVER, FAST-VQA, Q-Align, MaxVQA all train on the
  YouTube UGC bucket). The LSVQ adapter pattern becomes the
  canonical shape for any future MOS-corpus ingestion (one more
  adapter per dataset, no schema drift).
- **Negative**: Operators who want the whole corpus need ~2 TB
  free under `.workingdir2/`. The `--max-rows=300` default
  avoids surprise disk-fill but means a default run is *not* a
  full ingestion — operators must read the CLI help. The
  per-clip-MOS-source ambiguity (2019 originals vs 2020
  transcoded) is documented but propagates as the operator's
  responsibility to pin `--corpus-version` correctly.
- **Neutral / follow-ups**: The ENCODER_VOCAB v4 trainer-side
  collapse to `"ugc-mixed"` is still pending; landing it is
  decoupled from this PR. A future PR may also wire the YouTube
  UGC `_test` per-resolution slices explicitly to the held-out
  evaluation harness once the trainer can consume the new shard.

## References

- Wang, Y., Inguva, S., Adsumilli, B., "YouTube UGC Dataset for
  Video Compression Research," IEEE Workshop on Multimedia
  Signal Processing (MMSP) 2019.
- Wang, Y. et al., "Rich features for perceptual quality
  assessment of UGC videos," CVPR 2021 (transcoded-quality
  follow-up release).
- Public-readable GCS bucket:
  <https://storage.googleapis.com/ugc-dataset/> (license:
  Creative Commons Attribution, verified 2026-05-08).
- Bucket-root attribution file:
  <https://storage.googleapis.com/ugc-dataset/ATTRIBUTION>.
- Original-video listing CSV:
  <https://storage.googleapis.com/ugc-dataset/original_videos.csv>
  (verified 2026-05-08).
- Companion research digest:
  [Research-0091][research-0091].
- Prior corpus ingestion ADRs:
  [ADR-0310](0310-bvi-dvc-corpus-ingestion.md) (BVI-DVC),
  ADR-0325 Phase 2 (KonViD-150k, in flight as PR #447),
  [ADR-0333](0333-lsvq-corpus-ingestion.md) (LSVQ, in flight
  as PR #471).
- Source: `req` — implementation task spec routed through the
  agent harness 2026-05-08, citing the contributor-pack digest
  #465 and PR #471 (LSVQ) as the pattern source.

[research-0091]: ../research/0091-youtube-ugc-corpus-feasibility.md
