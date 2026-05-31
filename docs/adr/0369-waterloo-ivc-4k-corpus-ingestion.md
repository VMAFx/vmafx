<!-- markdownlint-disable MD018 MD060 -->
# ADR-0369: Waterloo IVC 4K-VQA corpus ingestion for `nr_metric_v1`

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: @Lusoris
- **Tags**: ai, training, corpus, license, fork-local

## Context

`nr_metric_v1` (~19 K params) is the fork's tiny no-reference
VQA model. Its in-flight training corpus stack already
covers BVI-DVC (ADR-0310), KonViD-150k Phase 2 (ADR-0325, in
flight as PR #447), and LSVQ (ADR-0333, in flight as PR
#471). Per the contributor-pack research digest #465 the
union of these three shards has a glaring distribution gap:
**none of them populate the 2160p resolution bin**. BVI-DVC
tops out at 1080p; KonViD-150k and LSVQ are predominantly
sub-1080p UGC. Without 2160p coverage, every PLCC-versus-
4K-encoded-content number we publish becomes a guess.

The University of Waterloo's
[Image and Vision Computing Laboratory 4K Video Quality
Database](https://ivc.uwaterloo.ca/database/4KVQA.html)
(Li, Duanmu, Liu, Wang; ICIAR 2019) fills exactly that gap:
twenty pristine 4K source sequences re-encoded with five
contemporary codecs (H.264/AVC, H.265/HEVC, VP9, AVS2, AV1)
at three resolutions (540p / 1080p / 2160p) and four
distortion levels — 1 200 distorted clips with controlled-
study per-clip MOS. The dataset is genuinely public: direct
ZIP download (no NDA, no password gate, no request form, no
registration), permissive academic licence (attribution
only). The author drop is hosted at
`https://ivc.uwaterloo.ca/database/4KVQA/201908/` with a
companion `scores.txt` table.

What is missing today is a JSONL adapter that bridges the
upstream `scores.txt` to the same MOS-corpus row schema the
KonViD-150k and LSVQ adapters emit, so the trainer can
consume all four shards through one loader without per-
corpus branching.

## Decision

We will adopt Waterloo IVC 4K-VQA as a fourth training shard
for `nr_metric_v1` under three constraints:

1. The upstream archives, extracted clips, and any cached
   features stay **local-only** (`.workingdir2/waterloo-ivc-4k/`).
   The fork redistributes only derived `nr_metric_v1_*.onnx`
   weights, with the IVC attribution string travelling
   alongside (per the licence text). Same posture as
   ADR-0310 / ADR-0325 / ADR-0333.
2. The MOS-corpus row schema (introduced for KonViD-150k
   Phase 2, mirrored for LSVQ in ADR-0333) is the merge
   contract. A new adapter
   (`ai/scripts/waterloo_ivc_to_corpus_jsonl.py`) emits one
   JSONL row per surviving clip with
   `corpus = "waterloo-ivc-4k"` and
   `corpus_version = "waterloo-ivc-4k-201908"`. The schema
   is byte-identical to the KonViD-150k / LSVQ adapters'
   modulo the `corpus` and `corpus_version` literals.
3. Laptop-class development is the default path. The script
   ingests the first `--max-rows=100` clips by default; the
   ~multi-TB whole-corpus run is opt-in via `--full`. The
   resumable-download contract from ADR-0325 / ADR-0333
   carries over verbatim.

**MOS scale divergence — recorded verbatim, normaliser
deferred.** Waterloo IVC 4K-VQA's published `scores.txt`
records per-clip MOS on a **0–100 raw scale**, not the 1–5
Likert scale used by KonViD-150k and LSVQ. The adapter
records the score verbatim on its native 0–100 scale (no
rescaling at ingest time), matching the ingest-time policy
of LSVQ / KonViD-150k. Cross-corpus rescaling — mapping
0–100 to 1–5 via `1 + 4·(x/100)` or the equivalent
distribution-matched rescaling — is a trainer-side concern
and lands in a separate PR.

**ENCODER_VOCAB v4 — `professional-graded` slot, deferred
to trainer.** Waterloo IVC sources are studio-captured
(20 pristine 4K reference sequences) and the 1 200 derived
clips are re-encoded by mainstream codecs at controlled
distortion levels. This is the "professionally-graded"
encoder regime in ENCODER_VOCAB v4 — distinct from the
`"ugc-mixed"` slot KonViD-150k / LSVQ collapse to. **This
adapter does not modify ENCODER_VOCAB itself** — that's a
separate PR. The script records the upstream codec name
ffprobe reports (`h264` / `hevc` / `vp9` / `av1`) in
`encoder_upstream` verbatim; the trainer is responsible for
collapsing it at consumption time and routing the
`waterloo-ivc-4k`-tagged rows to the
`"professional-graded"` slot rather than `"ugc-mixed"`.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| BVI-DVC + KonViD-150k + LSVQ (skip Waterloo) | Smallest surface; three ingestion adapters; one license-review fewer. | The 2160p resolution bin stays empty. Cross-paper PLCC numbers against 4K-encoded content stay aspirational. The fork already has a 4K validation gap flagged in research digest #465. | The marginal infra (one adapter mirroring LSVQ's shape + tests + a `--max-rows` / `--full` knob) is small; the dataset is genuinely public with no licence drama; the 2160p coverage is otherwise unobtainable. |
| BVI-DVC + KonViD-150k + LSVQ + Waterloo IVC 4K-VQA | Closes the 2160p resolution bin; permissive academic licence; direct download (no NDA / form gate); 1 200 distorted clips × MOS adds bona-fide subjective signal at 4K. | Working set ~multi-TB; canonical scores.txt is headerless 5-tuple (different shape from LSVQ / KonViD); MOS scale is 0–100, not 1–5 (cross-corpus normaliser becomes a follow-up PR). | **Chosen.** The shape divergences are bounded: the adapter auto-detects between the canonical headerless shape and the standard CSV header; the 0–100 vs 1–5 divergence is a single per-corpus normaliser in the trainer. |
| Waterloo IVC 4K-VQA only (replace KonViD / LSVQ) | One ingestion path; controlled subjective study; clean licence. | 1 200 clips is far too small a training corpus for `nr_metric_v1`; loses content-diversity coverage from KonViD / LSVQ; UGC distribution disappears entirely. | Premature — DOVER / FAST-VQA / Q-Align all train on the union, not on Waterloo alone. The fork follows the field. |
| Waterloo IVC 3D-VQ database (sister corpus) | Same lab; same licence; same author tooling. | Stereoscopic 3D content; not what `nr_metric_v1` consumes; 4K dataset is the relevant one for the resolution-bin gap. | Wrong dataset for the gap being closed. |

## Consequences

- **Positive**: `nr_metric_v1`'s training corpus union now
  populates the 2160p resolution bin with real subjective
  signal. Cross-paper PLCC comparison on 4K-encoded content
  becomes achievable for the first time on this fork. The
  KonViD-150k / LSVQ adapter pattern is now four-corpus
  proven; per-corpus adapter cost is bounded.
- **Negative**: Operators who want the whole corpus need
  multi-TB of free disk under `.workingdir2/`. The
  `--max-rows=100` default avoids surprise disk-fill but
  means a default run is *not* a full ingestion — operators
  must read the CLI help. Cross-corpus MOS rescaling becomes
  a trainer-side concern that must land before the four-
  corpus union is trained on.
- **Neutral / follow-ups**: The cross-corpus MOS rescaler
  (0–100 → 1–5 or equivalent) is still pending; landing it
  is decoupled from this PR. A future PR may also add a
  per-corpus `mos_scale_native` field to the JSONL row
  schema if the trainer's loader benefits from explicit
  scale tagging rather than implicit `corpus`-based
  lookup. The ENCODER_VOCAB v4
  `"professional-graded"` slot remains trainer-side and is
  unchanged by this PR.

## References

- Li, Z., Duanmu, Z., Liu, W., Wang, Z., "AVC, HEVC, VP9,
  AVS2 or AV1? — A Comparative Study of State-of-the-art
  Video Encoders on 4K Videos," ICIAR 2019.
- Waterloo IVC 4K-VQA dataset card:
  [`https://ivc.uwaterloo.ca/database/4KVQA.html`](https://ivc.uwaterloo.ca/database/4KVQA.html)
  (permissive academic licence, verified 2026-05-08; no NDA,
  no password gate, no registration form).
- Canonical archive base:
  [`https://ivc.uwaterloo.ca/database/4KVQA/201908/`](https://ivc.uwaterloo.ca/database/4KVQA/201908/);
  scores table at
  [`https://ivc.uwaterloo.ca/database/4KVQA/201908/scores.txt`](https://ivc.uwaterloo.ca/database/4KVQA/201908/scores.txt)
  (headerless 5-tuple `encoder, vid, res, dist, mos`,
  verified 2026-05-08).
- Companion research digest:
  [`docs/research/0091-waterloo-ivc-4k-corpus-feasibility.md`](../research/0091-waterloo-ivc-4k-corpus-feasibility.md).
- Prior corpus ingestion ADRs:
  [ADR-0310](0310-bvi-dvc-corpus-ingestion.md) (BVI-DVC),
  ADR-0325 Phase 2 (KonViD-150k, in flight as PR #447),
  ADR-0333 (LSVQ, in flight as PR #471).
- Source: `req` — implementation task spec routed through
  the agent harness 2026-05-08, citing the contributor-pack
  research digest #465 as the 2160p-gap source and the LSVQ
  PR #471 / KonViD-150k PR #447 as the adapter-pattern
  sources.
