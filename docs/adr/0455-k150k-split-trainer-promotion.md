<!-- markdownlint-disable MD013 -->
# ADR-0455: KonViD-150k k150ka/k150kb split promotion into the MOS-head trainer

- **Status**: Accepted
- **Date**: 2026-05-16
- **Deciders**: @Lusoris
- **Tags**: ai, training, corpus, konvid, fork-local

## Context

[ADR-0325](0325-konvid-150k-corpus-ingestion.md) materialized the
KonViD-150k corpus under `.corpus/konvid-150k/` (2026-05-15 status
update).  The Phase 2 adapter
(`ai/scripts/konvid_150k_to_corpus_jsonl.py`) emits one JSONL row per
clip and tags every row with `"split": "k150ka"` or `"split": "k150kb"`
— the two canonical score-drop sub-sets shipped by the MMSP group:

- **k150ka** (~148 000 rows) — the large training partition.
- **k150kb** (~2 000 rows) — the smaller held-out validation partition.

The MOS-head trainer
([`ai/scripts/train_konvid_mos_head.py`](../../ai/scripts/train_konvid_mos_head.py))
reads these labels through `_normalise_split()`, which only accepts the
vocabulary `{"train", "val", "test"}`.  Any other string — including
`"k150ka"` and `"k150kb"` — is silently mapped to `""` (unlabelled), so
the trainer falls back to random k-fold cross-validation and **ignores
the canonical split boundary entirely**.  This means:

1. The k150kb clips can appear in a training fold, leaking held-out data.
2. The production-flip gate (PLCC ≥ 0.85 on the held-out fold, per
   ADR-0325 / ADR-0303) fires against a randomly-partitioned fold rather
   than the researchers' intended split, making the gate verdict
   non-reproducible.
3. The `validation_policy` field in the emitted manifest reads
   `"5-fold-random"` instead of `"explicit-corpus-split"`, which
   obscures the split provenance from downstream consumers.

[OPEN.md § KonViD-150k corpus ingestion](../../.workingdir2/OPEN.md)
(lines 56–60) flagged this as the outstanding "downstream feature /
trainer promotion" work after the basic JSONL ingestion landed.

## Decision

Extend `_normalise_split()` in `train_konvid_mos_head.py` with a
translation table that maps the K150K identifiers to the trainer
vocabulary before the canonical acceptor runs:

```text
"k150ka" → "train"
"k150kb" → "val"
```

The mapping is applied before the `{"train", "val", "test"}` check so
that any future third-party split labels follow the same extension point.
Standard labels and unknown strings are unaffected.

No schema change to the JSONL format is needed; the adapter continues
to emit `"split": "k150ka"` / `"split": "k150kb"` as documented in
ADR-0325 §Phase 2.

## Consequences

### Positive

- The canonical k150ka/k150kb split boundary is honoured: k150kb clips
  are never seen during training, removing the data-leakage risk.
- The production-flip gate fires against the researchers' intended
  held-out partition, making the PLCC / SROCC / RMSE verdict
  reproducible across seeds and machines.
- The manifest field `validation_policy` records
  `"explicit-corpus-split"` when real corpus rows are loaded, giving
  downstream consumers auditable provenance.

### Negative

- None of significance.  The change is a four-line translation table
  inside a single `_normalise_split()` call.

### Neutral

- Existing synthetic-corpus smoke runs are unaffected because synthetic
  rows carry empty split labels and `_kfold_indices` is unchanged.
- The `konvid_150k_to_corpus_jsonl.py` adapter is unchanged; its split
  naming convention (`k150ka` / `k150kb`) becomes the stable interface
  this mapping translates from.

## Alternatives considered

- **Rewrite the adapter to emit `"train"` / `"val"` directly.**  Cleaner
  at the producer side but it breaks the traceability contract: the JSONL
  row's `split` field would no longer carry the researchers' original
  sub-set identity.  Downstream consumers (data lineage tools, future
  ensemble kit) would lose the ability to distinguish k150ka from k150kb
  rows.  Rejected.
- **Add a `--k150k-split-map` CLI flag to the trainer.**  Unnecessary
  indirection — the mapping is deterministic and follows the dataset's
  own naming.  Rejected.
- **Do nothing; document that the trainer needs `--konvid-150k` rows
  pre-relabelled.**  Punts a footgun onto every operator.  Rejected.

## References

- [ADR-0325](0325-konvid-150k-corpus-ingestion.md) — K150K corpus ingestion plan
- [ADR-0336](0336-konvid-mos-head-v1.md) — KonViD MOS head v1 trainer
- [ADR-0303](0303-fr-regressor-v2-ensemble-prod-flip.md) — production-flip gate protocol
- OPEN.md lines 56–60 (2026-05-16 snapshot): "remaining work is downstream feature / trainer promotion, not basic JSONL ingestion"
