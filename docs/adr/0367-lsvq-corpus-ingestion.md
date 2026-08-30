<!-- markdownlint-disable MD060 -->
# ADR-0367: LSVQ corpus ingestion for `nr_metric_v1`

- **Status**: Accepted
- **Date**: 2026-05-08
- **Deciders**: @Lusoris
- **Tags**: ai, training, corpus, license, fork-local

## Context

`nr_metric_v1` (~19 K params) is the fork's tiny no-reference VQA
model. To compete with DOVER-Mobile (~9.86 M params, PLCC 0.853 on
KoNViD / 0.867 on LSVQ_test) on published leaderboards we need a
training corpus broad enough to span community-uploaded video
content. ADR-0325 Phase 2 ingests KonViD-150k (~150 k clips) on
its own; the
[Tiny-AI SOTA digest](../research/0086-tiny-ai-sota-deep-dive-2026-05-08.md)
flags **LSVQ** (Ying et al. ICCV 2021, ~39 K videos / ~5.5 M
ratings) as *the* canonical large NR-VQA training corpus the field
trains on. KonViD-150k alone has too narrow a content distribution
to land on the LSVQ_test / LSVQ_test_1080p leaderboards every
modern NR-VQA paper benchmarks against.

LSVQ is hosted on Hugging Face under
[`teowu/LSVQ-videos`](https://huggingface.co/datasets/teowu/LSVQ-videos),
mirroring the original
[Patch-VQ](https://github.com/baidut/PatchVQ) author drop. The
corpus is large (~500 GB end-to-end raw); the licence is
CC-BY-4.0 per the dataset card, so derived ONNX models can ship
with attribution but the raw clips and per-clip MOS values must
stay local-only on this fork (same posture as ADR-0310 BVI-DVC
and ADR-0325 KonViD-150k).

What is missing today is a JSONL adapter that bridges the LSVQ
split CSV(s) to the same MOS-corpus row schema the KonViD-150k
adapter emits, so the trainer can consume both shards through
one loader without per-corpus branching.

## Decision

We will adopt LSVQ as a third training shard for `nr_metric_v1`
under three constraints:

1. The LSVQ archive, extracted MP4s, and any cached features stay
   **local-only** (`.workingdir2/lsvq/`). The fork redistributes
   only derived `nr_metric_v1_*.onnx` weights, with CC-BY-4.0
   attribution travelling alongside.
2. The MOS-corpus row schema (introduced for KonViD-150k Phase 2)
   is the merge contract. A new adapter
   (`ai/scripts/lsvq_to_corpus_jsonl.py`) emits one JSONL row per
   surviving clip with `corpus = "lsvq"`. The schema is
   byte-identical to the KonViD-150k adapter's modulo the
   `corpus` and `corpus_version` literals.
3. Laptop-class development is the default path. The script
   ingests the first `--max-rows=500` clips by default; the
   ~500 GB whole-corpus run is opt-in via `--full`. The
   resumable-download contract from ADR-0325 Phase 2 carries
   over verbatim (`.download-progress.json`, atomic
   tempfile-rename writes, non-retriable failure persistence).

The ENCODER_VOCAB v4 collapse to `"ugc-mixed"` is **not** done
here — this script records `encoder_upstream` from ffprobe
verbatim, identical to KonViD-150k. The trainer-side collapse
lands in a separate PR.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| KonViD-150k only | Smallest surface; one ingestion adapter; one license review. | Cannot benchmark against the LSVQ_test / LSVQ_test_1080p leaderboards every modern NR-VQA paper publishes; content distribution narrower than the field's de-facto training corpus. | The fork has the LSVQ Hugging Face mirror available and the KonViD-150k adapter shape ports trivially; not using it leaves measured signal on the table. |
| KonViD-150k + LSVQ | Adds the canonical NR-VQA corpus; the leaderboards we want to publish against use LSVQ_test; CC-BY-4.0 is permissively redistributable for derived weights. | Working set ~500 GB; CSV column-name drift across mirrors; partial-corpus runs need explicit operator opt-in. | **Chosen.** The marginal infra (one adapter mirroring KonViD-150k Phase 2 + tests + a `--max-rows` / `--full` CLI knob) is small. |
| KonViD-150k + LSVQ + LIVE-VQC + YouTube-UGC | Largest possible training corpus; broadest content; matches the exact union DOVER trains on. | LIVE-VQC redistribution licence is research-only-non-commercial; YouTube-UGC clip URLs degrade over time; LOSO partition explodes; ADR-0287 already showed marginal ensemble gains past two-corpus regime. | Premature without a clean KonViD-150k + LSVQ measurement. The two-public-shard regime is the next step; broader corpora work returns when (and if) the two-shard run leaves PLCC headroom. |

## Consequences

- **Positive**: `nr_metric_v1` becomes trainable on the same
  large-scale UGC corpus DOVER / FAST-VQA / Q-Align use, so
  cross-paper comparison on LSVQ_test / LSVQ_test_1080p becomes
  possible. The KonViD-150k adapter pattern becomes the canonical
  shape for any future MOS-corpus ingestion (one more adapter
  per dataset, no schema drift).
- **Negative**: Operators who want the whole corpus need ~500 GB
  free under `.workingdir2/`. The `--max-rows` default avoids
  surprise disk-fill but means a default run is *not* a full
  ingestion — operators must read the CLI help.
- **Neutral / follow-ups**: The ENCODER_VOCAB v4 trainer-side
  collapse to `"ugc-mixed"` is still pending; landing it is
  decoupled from this PR. A future PR may also add the LSVQ
  test-split CSV (`LSVQ_test`, `LSVQ_test_1080p`) explicitly to
  the held-out evaluation harness once the trainer can consume
  the new shard.

## References

- Ying, Z., Mandal, M., Ghadiyaram, D., Bovik, A. C., "Patch-VQ:
  'Patching Up' the Video Quality Problem," ICCV 2021.
- Hugging Face dataset card:
  [`teowu/LSVQ-videos`](https://huggingface.co/datasets/teowu/LSVQ-videos)
  (CC-BY-4.0, verified 2026-05-08).
- Patch-VQ author drop: <https://github.com/baidut/PatchVQ>.
- Tiny-AI SOTA deep-dive digest:
  [`docs/research/0086-tiny-ai-sota-deep-dive-2026-05-08.md`](../research/0086-tiny-ai-sota-deep-dive-2026-05-08.md).
- Prior corpus ingestion ADRs:
  [ADR-0310](0310-bvi-dvc-corpus-ingestion.md) (BVI-DVC),
  ADR-0325 Phase 2 (KonViD-150k, in flight as PR #447).
- Source: `req` — implementation task spec routed through the
  agent harness 2026-05-08, citing the SOTA digest #449 and the
  KonViD-150k Phase 2 PR #447 as the pattern source.
