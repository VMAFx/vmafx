<!-- markdownlint-disable MD013 -->
# ADR-0325: KonViD-150k corpus ingestion

- **Status**: Accepted (2026-05-15 — corpus materialized at
  `.workingdir2/konvid-150k/`)
- **Date**: 2026-05-07
- **Deciders**: @Lusoris
- **Tags**: ai, training, corpus, license, fork-local

## Status update (2026-05-15)

Corpus is materialized locally. `.workingdir2/konvid-150k/` contains:

- `clips/` — 307 682 extracted MP4 files (~150 GB)
- `k150ka_scores.csv` (4.9 MB) — k150k-A score-drop
- `k150kb_scores.csv` (59 KB) — k150k-B score-drop
- `k150ka_votes.csv` (94 MB) + `k150kb_votes.csv` (28 MB) — per-vote
  raw data
- `konvid_150k.jsonl` (64 MB) — corpus JSONL
- `manifest.csv` (4.9 MB) — manifest

Phase 2 (JSONL adapter) was already shipped via
`ai/scripts/konvid_150k_to_corpus_jsonl.py`. Phase 3 (real-corpus MOS
head training) becomes the next gate; tracked in
[ADR-0336](0336-konvid-mos-head-v1.md). Status flipped from Proposed
→ Accepted as the corpus-availability blocker is removed.

## Context

`fr_regressor_v2_ensemble` ([ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md))
trains today on the Netflix Public Drop (9 ref × 70 dis = 216 rows).
[ADR-0310](0310-bvi-dvc-corpus-ingestion.md) extended it with the
BVI-DVC research corpus (~720 distorteds). Both shards are
*cinematic / studio-curated content with bit-exact references* —
exactly the distribution the user-base **isn't** encoding.

The user's
[ChatGPT-vision text (2026-05-07)](../research/0086-konvid-150k-corpus-feasibility.md)
articulated the gap: real-world private-encoding ecosystems live on
remuxes, WEB-DLs, fansubs, anime, grain-heavy sources, and
edge-case content the academic / commercial datasets under-sample.
[KonViD-150k](https://database.mmsp-kn.de/konvid-150k-vqa-database.html)
is the closest open-licence proxy: ~150,000 short YouTube-UGC clips
with ≥ 5 crowdworker MOS ratings each. Two reasons to adopt:

1. Its **distribution** matches what fork users actually encode (UGC,
   not Netflix). New blind spots get covered.
2. Its **labels** are subjective MOS — the human-perception ground
   truth the ChatGPT-vision text calls out as the next axis beyond
   "predict VMAF directly". MOS lets us measure how *well-correlated
   our VMAF predictions are with humans*, then optionally train a
   sibling MOS head.

## Decision

Adopt KonViD-150k as a third training shard for the
`fr_regressor_v2` family under four constraints (mirrors ADR-0310):

1. The KonViD archive, downloaded clips, and any cached parquet stay
   **local-only** (`.workingdir2/konvid/`, `~/.cache/konvid/`,
   `runs/`). The fork redistributes only the *derived* ONNX models +
   the per-clip aggregate statistics (PLCC / SROCC / RMSE on the held-
   out fold), never the clips or the per-clip MOS.
2. **Phased rollout** — start with the 1.2k-clip predecessor
   (KonViD-1k, ~5 GB) to validate the parquet conversion pipeline;
   then scale to 150 k.
3. **ENCODER_VOCAB extension** — add a single new slot
   `"ugc-mixed"` ([ADR-0302](0302-encoder-vocab-v3-schema-expansion.md))
   bumping the vocab from 16 → 17 slots. We do *not* try to recover
   per-clip encoder identity from YouTube-VP9 / x264 / x265 / etc.
4. **Production-flip gate** — train + register MOS-head ONNX models
   via the existing
   [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) protocol
   (mean PLCC ≥ 0.85 against a held-out KonViD fold; spread ≤ 0.005
   across seeds). PLCC threshold is lower than the VMAF-prediction
   gate because subjective MOS has inherent inter-rater noise.

## Phased deliverables

### Phase 0 — this ADR + research digest

Land [Research-0086](../research/0086-konvid-150k-corpus-feasibility.md)

- this ADR. No code. Disk impact: ~30 KB of docs.

### Phase 1 — KonViD-1k pipeline

Ship `ai/scripts/konvid_1k_to_corpus_jsonl.py` that:

- Downloads the 1.2k-clip set (~5 GB) into `.workingdir2/konvid-1k/`
  via the dataset's official downloader (gitignored).
- Verifies sha256 against the dataset's published manifest.
- For each clip: probes geometry via ffprobe, decodes-to-yuv (or keeps
  as VP9-MP4 if the trainer supports it), pairs with MOS from the
  bundled CSV, writes one row per clip into a vmaf-tune-shaped JSONL
  with `mos` as a new column.
- Writes a small ADR-0303-style `MOS_PROMOTE.json` / `MOS_HOLD.json`
  if invoked from the trainer harness.

Output: ~1.2k rows, ~5 GB local working set, ~50 MB JSONL.
Smoke-test via `pytest ai/tests/test_konvid_1k.py`.

### Phase 2 — KonViD-150k scale-up

Ship `ai/scripts/konvid_150k_to_corpus_jsonl.py`:

- Same pipeline, scaled up. Resumable (download state in
  `.workingdir2/konvid-150k/.download-progress.json`).
- Handles the ~5–8 % of clips that fail to download (YouTube takedown,
  region-block, etc.) — logs them, continues.
- Adds the `"ugc-mixed"` slot to ENCODER_VOCAB; bump to v4.

Output: ~140k–145k rows after attrition, ~120–200 GB local working
set, ~6 GB JSONL.

### Phase 3 — MOS head + held-out validation

Train a sibling `mos_regressor_v1` ONNX via the existing
ensemble-training-kit harness ([ADR-0324](0324-ensemble-training-kit.md)),
co-located with `fr_regressor_v2_ensemble`. Use KonViD-150k as the
training corpus + a held-out 10 % fold for production-flip gating.

Concurrently use the held-out fold to measure how well *the existing
VMAF predictor* correlates with MOS on UGC content — that's a
free-of-cost robustness check. Surface the PLCC / SROCC numbers in
[`docs/state.md`](../state.md).

## Alternatives considered

- **Skip MOS entirely; stay on libvmaf-CPU teacher labels.** Loses the
  human-perception alignment the ChatGPT-vision text calls for; ships
  faster but doesn't move the per-title-quality story.
- **YouTube-UGC dataset (Google, 1.5k clips with MOS + VMAF labels).**
  Smaller and simpler licence; useful as a sanity-check companion.
  Plan keeps the door open to land it as Phase 1.5 alongside KonViD-1k.
- **LIVE-VQC, LIVE-Qualcomm, KoNViD-1k alone, Waterloo-IVC-4k.** All
  comparable academic UGC corpora. KonViD has best scale.
- **Crowdsource the labels ourselves.** Months of recruiting + an
  IRB-style protocol; out of scope for a fork.

## Consequences

### Positive

- Closes the cinematic-vs-UGC gap in the training distribution.
- Adds the only widely-cited subjective MOS axis to the fork's
  perceptual-metric story.
- Establishes the pattern for future community-uploaded datasets
  (the ChatGPT-vision text's Section 4 / "Community Learning
  Potential") — same `local-only + redistribute-only-models`
  contract.

### Negative

- ~120–200 GB local disk per contributor on the full 150k path; the
  ensemble-training-kit gdrive bundler will need to compress this
  separately or contributors source it directly.
- Adds a third training corpus to maintain. ENCODER_VOCAB bumps to
  v4 (one new slot). Existing v3 ONNX models keep working — the
  `"ugc-mixed"` column is zero-padded for them.
- KonViD download wall-clock can take days; not all clips remain
  available. Pipeline must tolerate ~5–8 % attrition silently.

### Neutral

- KonViD download ToS forbids commercial redistribution but allows
  research use; the fork stays research-mode-only on this shard
  (consistent with ADR-0310).

## References

- [Research-0086](../research/0086-konvid-150k-corpus-feasibility.md) (this PR's research digest)
- KonViD-150k home: <https://database.mmsp-kn.de/konvid-150k-vqa-database.html>
- KonViD-1k home: <https://database.mmsp-kn.de/konvid-1k-database.html>
- [ADR-0302](0302-encoder-vocab-v3-schema-expansion.md) — ENCODER_VOCAB schema
- [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) — production-flip gate
- [ADR-0309](0309-fr-regressor-v2-ensemble-real-corpus-retrain.md) — Netflix Public Drop
- [ADR-0310](0310-bvi-dvc-corpus-ingestion.md) — BVI-DVC ingestion
- [ADR-0324](0324-ensemble-training-kit.md) — ensemble-training-kit harness
- User direction (2026-05-07, this conversation): "I think we also need these two datasets? <https://database.mmsp-kn.de/konvid-150k-vqa-database.html>"
