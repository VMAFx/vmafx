<!-- markdownlint-disable MD060 -->
# ADR-0340: Multi-corpus aggregation for the FR-regressor / predictor v2 trainer

- **Status**: Accepted
- **Status update 2026-05-15**: implemented;
  `ai/scripts/aggregate_corpora.py` present; `SCALE_CONVERSIONS`
  table + dedup logic active; aggregates BVI-DVC, Netflix Public,
  K150K, and KonViD.
- **Date**: 2026-05-09
- **Deciders**: @Lusoris
- **Tags**: ai, training, corpus, fork-local

## Context

The fork ingests a fan of MOS-labelled VQA corpora — KonViD-1k
(ADR-0325 Phase 1), KonViD-150k (ADR-0325 Phase 2, in flight on
PR #447), LSVQ (ADR-0333, PR #471), Waterloo IVC 4K-VQA (ADR-0334,
PR #485), YouTube UGC (ADR-0334, PR #481), plus the Netflix Public
drop already on disk under `.workingdir2/netflix/`. Each ingestion
adapter emits a corpus-specific JSONL.

The trainers downstream of these adapters
(`train_predictor_v2_realcorpus.py` from PR #487,
`train_konvid.py` evolving on PR #491) want to learn from **every**
shard simultaneously — broader content coverage, less per-corpus
overfit, more LOSO folds — but the source MOS scales are
incompatible. KonViD / LSVQ / YouTube UGC publish a 1–5 ACR Likert;
Waterloo IVC 4K-VQA uses a continuous 0–100 numerical-category
scale; the Netflix Public drop's `vmaf_v0.6.1` per-frame scores are
on the 0–100 VMAF axis already. Naive concatenation gives the
trainer three different target distributions and the regression head
learns the wrong thing.

What is missing is an aggregation step that (1) re-bases every
shard onto a single canonical 0–100 axis via a per-corpus *affine*
conversion, (2) tags each row with explicit `corpus_source`
provenance, and (3) deduplicates clips that appear in multiple
corpora by picking the row with the tighter MOS uncertainty rather
than last-write-wins. The fork's existing `merge_corpora.py`
(ADR-0310) handles encode-grid concatenation but not subjective-MOS
scale unification or uncertainty-weighted dedup.

## Decision

We will ship `ai/scripts/aggregate_corpora.py` as the single
aggregation step between per-corpus MOS JSONLs and the v2 trainers.
Three constraints govern the implementation:

1. **Affine, documented, citation-pinned scale conversions.** Every
   per-corpus conversion is an affine map (`unified = slope *
   native + intercept`), pinned in a `SCALE_CONVERSIONS` table whose
   entries cite the source dataset's published scale definition with
   a 2026-05-09 access date. Compression / clipping / saturating
   conversions are explicitly forbidden — they would silently warp
   the training-target distribution. Per the fork's
   `feedback_no_test_weakening` rule, rows whose native MOS falls
   outside the published range are *dropped* (not clipped), and
   rows whose `corpus` label is not in `SCALE_CONVERSIONS` are
   dropped (no guessed conversion).
2. **Cross-corpus dedup by MOS uncertainty.** A clip with the same
   `src_sha256` in two corpora collapses to the row with the
   smaller `mos_std_dev`. Ties keep the first-seen row, which is
   deterministic given a stable `--inputs` ordering. A missing or
   zero `mos_std_dev` is treated as "unknown uncertainty" and loses
   to any row reporting a positive std-dev.
3. **Graceful degradation across partial corpus availability.**
   Operators on different machines hold different shards. Missing
   `--inputs` paths are logged `WARNING` and skipped; the run fails
   hard only when **zero** inputs survive the existence check. The
   companion `run_aggregated_training.sh` discovers conventional
   JSONL locations and forwards whichever shards are on disk.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Per-corpus head per dataset (no aggregation) | Each head trained on native scale; no conversion bias. | Negates the multi-corpus motivation: each head sees its own narrow distribution; LOSO folds stay small; cross-corpus generalisation never gets exercised. | Defeats the user's stated goal ("learn from all of them simultaneously"). |
| Z-score normalisation per corpus | Removes scale-incompatibility without committing to a target axis. | Loses the absolute-quality semantics — a z-score of 0 means "median for *this* corpus", not a fixed quality level. The trainer cannot calibrate against the VMAF reference axis. | Throws away information the VMAF-aligned axis preserves. |
| Quantile-mapping each corpus to the reference (Netflix) distribution | Compensates for non-affine scale differences. | Documented evidence that any of these scales is *non-affine* w.r.t. the others is thin; quantile-mapping introduces dataset-specific compression that's hard to explain to future maintainers. | Affine is simpler and the published scales support it. |
| Last-write-wins dedup (mirror `merge_corpora.py` behaviour) | Trivial to implement. | Silently drops the corpus with tighter raters' agreement whenever the operator reorders `--inputs`. Non-deterministic across machine layouts. | Loses information for no operational benefit. |

## Consequences

- **Positive.** A single canonical row stream the v2 trainers can
  consume without per-corpus pre-processing. Provenance is
  explicit (`corpus_source`, `mos_native`, `mos_native_scale`) so
  ablation studies don't need a side-channel mapping. Dedup is
  uncertainty-weighted and deterministic.
- **Negative.** The 1–5 ACR → 0–100 affine map embeds an assumption
  that the four ACR-scale corpora are mutually comparable on a
  linear axis. The literature does not strictly prove this; if a
  follow-up audit finds non-affine drift, the conversion table
  becomes the single point to revise. The aggregator does not
  attempt to cross-calibrate corpora against a shared reference
  (e.g. by running VMAF on each clip and aligning) — that is left
  for a future, explicitly-flagged ADR.
- **Neutral / follow-ups.**
  - The trainer in PR #487 needs to read `corpus_source` for
    per-corpus loss weighting / ablation; that wiring is its
    own PR.
  - `run_aggregated_training.sh` exits non-zero when the trainer
    entrypoint is absent (PR #487 not yet merged); operators on
    pre-#487 machines should set `VMAF_AGG_DRY_RUN=1` or override
    `VMAF_AGG_TRAINER`.
  - If we later ship a non-affine corpus (e.g. a paired-comparison
    dataset producing Bradley–Terry scores), it gets a new entry in
    `SCALE_CONVERSIONS` with the affine assumption explicitly
    documented as not applicable.

## References

- ADR-0310 — BVI-DVC corpus ingestion + `merge_corpora.py` sibling.
- ADR-0325 — KonViD ingestion (Phase 1 + Phase 2).
- ADR-0333 — LSVQ ingestion (in flight on PR #471).
- ADR-0334 — YouTube UGC + Waterloo IVC ingestion (in flight on
  PRs #481 / #485).
- ADR-0303 — `fr_regressor_v2` ensemble flip gate (downstream
  consumer).
- PR #487 — predictor v2 real-corpus LOSO trainer.
- PR #491 — KonViD MOS head v1.
- Source: `req` (operator brief, 2026-05-09: aggregate the multiple
  ingestion-PR JSONLs into one trainer-consumable stream via
  per-corpus normalization).
