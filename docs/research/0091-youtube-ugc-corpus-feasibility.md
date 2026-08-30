# Research-0091: YouTube UGC corpus feasibility for `nr_metric_v1`

- **Date**: 2026-05-08
- **Author**: agent (YouTube UGC ingestion task)
- **Companion ADR**: [ADR-0368](../adr/0368-youtube-ugc-corpus-ingestion.md)

## TL;DR

The Google YouTube UGC dataset (Wang, Inguva, Adsumilli; MMSP
2019) is the canonical large-scale UGC corpus paired with crowd
quality annotations. It is hosted in the public-readable Google
Cloud Storage bucket `gs://ugc-dataset/` under
Creative Commons Attribution, with no request form, no sign-up,
and no API key — the bucket has the `allUsers:objectViewer` IAM
role. Per-clip MOS is on the same 1.0-5.0 Likert scale as LSVQ
/ KonViD; per-clip `mos_std_dev` and `n_ratings` columns travel
alongside. Total raw working set is ~2 TB end-to-end (~1500
originals at up to 4 K plus the four-rate transcoded ladder), so
the ingestion adapter defaults to a 300-row laptop-class subset
and gates whole-corpus runs behind an explicit `--full` opt-in.

## Why YouTube UGC on top of LSVQ + KonViD-150k

The contributor-pack research digest #465 flags YouTube UGC as
the under-represented *content-distribution* axis of the fork's
`nr_metric_v1` training mix. KonViD-150k draws content from
Flickr (skewed toward photographic / outdoor / travel content);
LSVQ draws from Internet Archive social-video. YouTube UGC adds
the genuine YouTube content distribution: gaming captures,
vlogs, lyric-videos, animation, sports, HDR clips, and the long
tail of community-uploaded mobile-first content. Adding it makes
the trainer-side data distribution a closer match to what the
production scoring path actually sees.

## Manifest CSV: where does it live

The canonical Google release distributes a per-clip listing CSV
at the bucket root (`original_videos.csv`, plus per-resolution
slices `original_videos_360P.csv` etc.). The 2019 originals CSV
typically has no URL column — every clip lives at the canonical
bucket path
`https://storage.googleapis.com/ugc-dataset/original_videos/<filename>`,
so the adapter synthesises the URL from a configurable bucket
prefix when the manifest does not carry one explicitly.

Header columns vary across the 2019 MMSP release, the 2020
transcoded-quality follow-up, and the academic redistributions
(DOVER and FAST-VQA both ship variants). The adapter accepts
every observed alias for filename / URL / MOS / SD /
rating-count columns. Bare-stem `vid` columns
(`Gaming_720P-25aa_orig` rather than `Gaming_720P-25aa_orig.mp4`)
are common; the adapter transparently appends `--clip-suffix`
(default `.mp4`).

## License

Creative Commons Attribution per the bucket-root `ATTRIBUTION`
file (verified 2026-05-08). This is permissive enough to ship
derived `nr_metric_v1_*.onnx` weights with attribution, but the
raw clips and per-clip MOS values stay local-only on this fork
to avoid bundling the dataset itself with the repo (same posture
as ADR-0310 BVI-DVC, ADR-0325 KonViD-150k, and ADR-0333 LSVQ).

## Per-clip scoring methodology

Two distinct subjective releases sit under the same dataset
umbrella:

1. **2019 originals release** (Wang et al. MMSP 2019) — per-clip
   crowd MOS on the 1.0-5.0 Likert scale across 1380 of the
   ~1500 originals. This is the default `--corpus-version =
   "ugc-2019-orig"` mode. Pass-through identical to LSVQ /
   KonViD-150k: the manifest's MOS column lands verbatim in the
   row's `mos` field.
2. **2020 transcoded follow-up** (Wang et al. CVPR 2021) —
   per-bitrate crowd ratings on transcoded outputs at four rate
   points (`orig` / `cbr` / `vod` / `vodlb`). Operators wanting
   the transcoded ratings pre-aggregate them into a one-row-per-
   `orig` CSV with the per-clip mean across the four levels and
   pin `--corpus-version = "ugc-2020-transcoded-mean"`. The
   adapter records the mean verbatim; documenting the
   methodology behind the manifest's MOS column is the
   operator's responsibility.

## Decision matrix

See [ADR-0368 §Alternatives considered](../adr/0368-youtube-ugc-corpus-ingestion.md#alternatives-considered).

## Open follow-ups

- ENCODER_VOCAB v4 trainer-side collapse to `"ugc-mixed"` for
  KonViD-150k + LSVQ + YouTube UGC rows (separate PR).
- Held-out evaluation harness wiring for YouTube UGC's
  per-resolution slices so PLCC / SROCC / KRCC vs the bucket
  becomes a CI-comparable number rather than an ad-hoc one.
- Corpus-level rescaling audit if the cross-corpus distribution
  (KonViD MOS vs LSVQ MOS vs YouTube UGC MOS) turns out to need
  a per-shard normaliser. None applied at ingest time today.

## References

- Wang, Y., Inguva, S., Adsumilli, B., "YouTube UGC Dataset for
  Video Compression Research," IEEE MMSP 2019.
- Wang, Y. et al., "Rich features for perceptual quality
  assessment of UGC videos," CVPR 2021.
- Public-readable GCS bucket:
  <https://storage.googleapis.com/ugc-dataset/> (license:
  Creative Commons Attribution, verified 2026-05-08).
- Bucket-root attribution file:
  <https://storage.googleapis.com/ugc-dataset/ATTRIBUTION>.
- Original-video listing CSV:
  <https://storage.googleapis.com/ugc-dataset/original_videos.csv>.
