# Research-0091: Waterloo IVC 4K-VQA corpus feasibility for `nr_metric_v1`

- **Date**: 2026-05-08
- **Author**: agent (Waterloo IVC ingestion task)
- **Companion ADR**: [ADR-0369](../adr/0369-waterloo-ivc-4k-corpus-ingestion.md)

## TL;DR

Waterloo IVC 4K-VQA (Li, Duanmu, Liu, Wang; ICIAR 2019)
is the only public, permissively-licensed,
controlled-subjective-study video-quality corpus with
2160p coverage that the fork's training stack can ingest
without registration / NDA / paperwork. Twenty pristine
4K source sequences re-encoded with five contemporary
codecs (H.264/AVC, H.265/HEVC, VP9, AVS2, AV1) at three
resolutions (540p / 1080p / 2160p) and four distortion
levels yield 1 200 distorted clips with per-clip MOS on a
**0–100 raw scale**. Hosted at
[`ivc.uwaterloo.ca/database/4KVQA/201908/`](https://ivc.uwaterloo.ca/database/4KVQA/201908/);
the companion `scores.txt` is a headerless 5-tuple
(`encoder, video_number, resolution, distortion_level,
mos`). Permissive academic licence — attribution only;
no NDA, no password gate, no request form, no
registration. Ingestion adapter defaults to a 100-row
laptop-class subset and gates whole-corpus runs behind an
explicit `--full` opt-in (working set is multi-TB at
4K).

## Why Waterloo IVC on top of BVI-DVC + KonViD-150k + LSVQ

The fork's `nr_metric_v1` training corpus stack today
covers BVI-DVC (ADR-0310), KonViD-150k Phase 2 (ADR-0325
Phase 2), and LSVQ (ADR-0333). The union has a
distribution gap flagged in research digest #465: **none
of the three shards populate the 2160p resolution bin**.
BVI-DVC tops out at 1080p; KonViD-150k / LSVQ are
dominantly sub-1080p UGC. Without 2160p coverage, every
PLCC-against-4K-encoded-content number we publish is a
guess.

Waterloo IVC 4K-VQA closes that gap with a
permissively-licensed, direct-download corpus the same
field references (cited in DOVER, FAST-VQA, and the
broader 4K-VQA literature). The licence text is the
text-book IVC permissive academic clause: free use /
copy / modify / redistribute, attribution required.

## Manifest CSV: where it lives

The canonical scores table is published at
[`https://ivc.uwaterloo.ca/database/4KVQA/201908/scores.txt`](https://ivc.uwaterloo.ca/database/4KVQA/201908/scores.txt)
(verified 2026-05-08). It is **headerless**, with five
comma-separated columns:

```text
encoder, video_number, resolution, distortion_level, mos
```

First five rows verbatim from the upstream file
(verified 2026-05-08):

```text
HEVC, 1, 540p, 1, 18.21
HEVC, 1, 540p, 2, 39.46
HEVC, 1, 540p, 3, 50.23
HEVC, 1, 540p, 4, 77.26
HEVC, 1, 1080p, 1,  7.61
```

The adapter auto-detects this shape by sniffing the first
row (5 fields, last field a float in [0, 100]) and falls
back to the standard LSVQ / KonViD-150k named-column CSV
shape when the first row looks header-like.

## License

The IVC 4K-VQA dataset is published under the Image and
Vision Computing Laboratory permissive academic licence
(verified at
[`https://ivc.uwaterloo.ca/database/4KVQA.html`](https://ivc.uwaterloo.ca/database/4KVQA.html)
2026-05-08):

> Permission is granted, without written agreement and
> without license or royalty fees, to use, copy, modify,
> and distribute this database and its documentation for
> any purpose, provided that the copyright notice in its
> entirity appear in all copies and the Image and Vision
> Computing Laboratory (IVC) at the University of
> Waterloo is acknowledged in any publication using the
> database.

Citation requirement: Li, Z., Duanmu, Z., Liu, W., Wang,
Z., "AVC, HEVC, VP9, AVS2 or AV1? — A Comparative Study
of State-of-the-art Video Encoders on 4K Videos," ICIAR
2019.

This is permissive enough to ship derived
`nr_metric_v1_*.onnx` weights with attribution; the raw
clips and per-clip MOS values stay local-only on this
fork to avoid bundling the dataset itself with the repo
(same posture as ADR-0310 BVI-DVC, ADR-0325
KonViD-150k, ADR-0333 LSVQ).

## Public-availability verification

The dataset is genuinely public — verified directly
against the official IVC dataset card 2026-05-08:

- **No NDA, no request form, no registration**: the
  download links on
  [`ivc.uwaterloo.ca/database/4KVQA.html`](https://ivc.uwaterloo.ca/database/4KVQA.html)
  point directly at archive ZIPs (Sources, H264, HEVC,
  VP9 single-archive each; AVS2 and AV1 split into 4
  parts each).
- **No password gate**: HTTPS direct fetch from the
  archive base
  [`https://ivc.uwaterloo.ca/database/4KVQA/201908/`](https://ivc.uwaterloo.ca/database/4KVQA/201908/).
- **No paperwork**: only attribution required (per the
  license clause quoted above).

A contact email (`z777li@uwaterloo.ca`) is listed for
operators who want to acknowledge use, but contact is
not a precondition of download.

## MOS scale: native 0–100 (divergent from KonViD / LSVQ)

The published `scores.txt` records per-clip MOS on a
0–100 raw scale (verified by fetching the canonical file
2026-05-08 and reading the first rows). This is **not**
the 1–5 Likert scale used by KonViD-150k and LSVQ.
Sample rows above span 7.61 → 77.26 in a single
resolution shard, well outside the 1–5 band.

The ingestion adapter records the MOS verbatim on its
0–100 native scale. Cross-corpus rescaling is a
trainer-side concern; ADR-0369 §Consequences flags it as
a follow-up PR.

## Decision matrix

See [ADR-0369 §Alternatives considered](../adr/0369-waterloo-ivc-4k-corpus-ingestion.md#alternatives-considered).

## Open follow-ups

- Cross-corpus MOS rescaler (0–100 → 1–5 or equivalent)
  in the trainer-side data loader. Required before the
  four-corpus union (BVI-DVC + KonViD-150k + LSVQ +
  Waterloo IVC) trains as one shard.
- ENCODER_VOCAB v4 trainer-side routing of
  `corpus = "waterloo-ivc-4k"` rows to the
  `"professional-graded"` slot rather than the
  `"ugc-mixed"` slot KonViD-150k / LSVQ use.
- Optional `mos_scale_native` JSONL row field — explicit
  scale tagging rather than implicit `corpus`-based
  lookup. Defer until the trainer-side scaler exists.

## References

- Li, Z., Duanmu, Z., Liu, W., Wang, Z., "AVC, HEVC,
  VP9, AVS2 or AV1? — A Comparative Study of
  State-of-the-art Video Encoders on 4K Videos," ICIAR
  2019.
- Waterloo IVC 4K-VQA dataset card:
  [`https://ivc.uwaterloo.ca/database/4KVQA.html`](https://ivc.uwaterloo.ca/database/4KVQA.html)
  (permissive academic licence, verified 2026-05-08).
- Canonical archive base:
  [`https://ivc.uwaterloo.ca/database/4KVQA/201908/`](https://ivc.uwaterloo.ca/database/4KVQA/201908/),
  scores table at
  [`https://ivc.uwaterloo.ca/database/4KVQA/201908/scores.txt`](https://ivc.uwaterloo.ca/database/4KVQA/201908/scores.txt).
- Contributor-pack research digest #465 (PR #465) —
  flagged the 2160p-bin gap in the BVI-DVC + KonViD +
  LSVQ union.
- Tiny-AI SOTA deep-dive digest:
  [`docs/research/0086-tiny-ai-sota-deep-dive-2026-05-08.md`](0086-tiny-ai-sota-deep-dive-2026-05-08.md).
