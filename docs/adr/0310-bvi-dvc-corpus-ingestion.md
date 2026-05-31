<!-- markdownlint-disable MD013 MD060 -->
# ADR-0310: BVI-DVC corpus ingestion for `fr_regressor_v2`

- **Status**: Accepted
- **Date**: 2026-05-05
- **Deciders**: @Lusoris
- **Tags**: ai, training, corpus, license, fork-local

## Context

`fr_regressor_v2` (ADR-0235 / ADR-0272 / ADR-0303) is the codec-aware
full-reference VMAF regressor that consumes the vmaf-tune Phase A
JSONL corpus. Its training corpus today is the Netflix Public drop
(`.workingdir2/netflix/`, 9 reference sources × 70 distorted variants
= 216 rows after pairing). LOSO mean PLCC against the
`vmaf_v0.6.1` per-frame teacher is high but the variance band is wide
because each fold trains on only 8 sources.

BVI-DVC (Ma, Zhang, Bull 2021) is a research video-quality reference
corpus from the University of Bristol's Visual Information Lab —
4-tier (UHD / HD / 540p / 270p) 4:2:0 10-bit YCbCr clips, distributed
as a Zenodo / Bristol-portal archive. Tier-D alone carries ~120
sources, and content diversity (urban walks, sports, natural scenes,
texture-heavy material) is wider than the Netflix drop's predominantly
cinematic content. A user-local copy of the archive sits at
`.workingdir2/BVI-DVC Part 1.zip`; the parquet feature pipeline
(`ai/scripts/bvi_dvc_to_full_features.py`) already exists in tree.

What is missing is the bridge from the parquet feature corpus to the
vmaf-tune-shaped JSONL the `fr_regressor_v2` trainer consumes, plus
a multi-shard merge utility so Netflix and BVI-DVC corpus rows can
flow into one training run with deterministic deduplication.

## Decision

We will adopt BVI-DVC as a second training shard for `fr_regressor_v2`
under three constraints:

1. The BVI-DVC archive, extracted MP4s / YUVs, and any cached libvmaf
   JSON stay **local-only** (`.workingdir2/`, `~/.cache/`, `runs/`).
   The fork redistributes only the *derived* `fr_regressor_v2_*.onnx`
   weights — never the source corpus.
2. The fr_regressor_v2 schema (`CORPUS_ROW_KEYS`) is the merge
   contract. A new adapter
   (`ai/scripts/bvi_dvc_to_corpus_jsonl.py`) transforms the cached
   per-clip libvmaf JSON into vmaf-tune corpus rows; a new merge
   utility (`ai/scripts/merge_corpora.py`) concatenates Netflix and
   BVI-DVC shards with `(src_sha256, encoder, preset, crf)` dedup.
3. A production-weights flip is **gated separately** on a multi-seed
   LOSO comparison against the Netflix-only baseline. This ADR ships
   the ingestion infrastructure; the flip decision lives with
   ADR-0303's ensemble-flip rule.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep Netflix-only corpus | Smallest surface; matches every shipped `fr_regressor_v*.onnx`; no license question. | Variance band on per-fold LOSO PLCC stays wide; no high-motion / texture-heavy content; corpus growth is gated on Netflix-only sweeps. | The fork has BVI-DVC locally and the parquet pipeline already extracts features; not using it leaves measured signal on the table. |
| Netflix + BVI-DVC | ~3× corpus size; +N LOSO folds; widens content distribution; reuses already-extracted parquet cache. | License is research-only — corpus stays local; no end-to-end CI retraining; introduces a merge utility. | **Chosen.** Local-only redistribution posture is the same posture already in place for the Netflix Public drop; the marginal infra (one adapter + one merge utility + tests) is small. |
| Netflix + BVI-DVC + all-public-corpora (KoNViD, YouTube-UGC, LIVE-Qualcomm, BVI-CC) | Largest possible corpus; broadest content; hits saturation on the 6-feature regime. | License audit per corpus; KoNViD is YouTube-derived (unstable URLs); UGC content distribution mismatches the codec-controlled production setting; LOSO partition explodes; ADR-0287 already showed marginal v5 gains. | Premature without a clean Netflix + BVI-DVC measurement. The two-shard regime is the next step; broader public-corpora work returns when (and if) the two-shard run leaves PLCC headroom. |

## Consequences

- **Positive**: Triples the `fr_regressor_v2` training corpus and
  expands LOSO partitioning from 9 source-folds to 9 + N. Adds
  reusable corpus-shard merge tooling (`merge_corpora.py`) that any
  future shard (KoNViD, UGC, …) can plug into without changing the
  trainer. Decouples corpus growth from production-weights ship
  decisions.
- **Negative**: License posture forces local-only handling; CI cannot
  retrain end-to-end. Anyone reproducing the fr_regressor_v2 result
  must obtain BVI-DVC themselves from the upstream Bristol portal.
  Two-shard provenance complicates per-source attribution in the
  sidecar JSON (we will record `corpus_shard ∈ {netflix, bvi-dvc}` at
  load time, not store BVI-DVC clip names verbatim).
- **Neutral / follow-ups**:
  - Keep the BVI-DVC archive and extracted artefacts gitignored;
    confirm `.gitignore` covers `runs/full_features_bvi_dvc_*.parquet`
    and `runs/bvi_dvc_corpus.jsonl`.
  - Wire the JSONL adapter and merge utility into a multi-seed LOSO
    sweep (deferred — heavy CPU / GPU work; not part of this PR).
  - Production flip stays gated on the ADR-0303 ensemble criterion;
    do **not** retrain and ship `fr_regressor_v2` weights without
    the gate clearing.
  - Adapter exists for `libx264` only today (matches the existing
    parquet pipeline). Adding hw codecs (NVENC, QSV, AMF) for BVI-DVC
    is a follow-on under ADR-0237 Phase A's multi-codec runner —
    keep the JSONL adapter encoder-agnostic so a multi-codec sweep
    drops in without changes.

## References

- [ADR-0235](0235-codec-aware-fr-regressor.md) — codec-aware FR regressor.
- [ADR-0237](0237-quality-aware-encode-automation.md) — vmaf-tune Phase A
  corpus schema (the merge contract).
- [ADR-0272](0272-fr-regressor-v2-codec-aware-scaffold.md) — fr_regressor_v2 scaffold.
- [ADR-0303](0303-fr-regressor-v2-ensemble-flip.md) — ensemble-flip ship gate.
- [Research-0082](../research/0082-bvi-dvc-corpus-feasibility.md) — feasibility digest.
- Ma, Zhang, Bull. *BVI-DVC: A Training Database for Deep Video
  Compression*. IEEE Transactions on Multimedia, 2021.
- Source: `req` — user direction 2026-05-05 to triple the
  fr_regressor_v2 training corpus by ingesting BVI-DVC alongside the
  existing Netflix Public drop.
