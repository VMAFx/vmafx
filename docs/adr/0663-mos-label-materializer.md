<!-- markdownlint-disable MD060 -->
# ADR-0663: MOS Label Materializer

- **Status**: Accepted
- **Date**: 2026-05-21
- **Deciders**: Lusoris agents
- **Tags**: ai, mos, training, corpus

## Context

The AI refresh surfaced a bad failure mode: a KoNViD full-feature parquet had
features and split labels but no `mos` column, and
`train_konvid_mos_head.py` silently fell back to synthetic data. That created a
real-looking output filename for a synthetic checkpoint and hid the actual
problem: MOS labels were never joined onto the feature table.

The fork now has several MOS-labelled corpora and refreshed feature tables.
Operators need an explicit, testable join step that materialises labels before
training, with enough audit metadata to catch weak coverage and stale keys.

## Decision

Add `ai/scripts/materialize_mos_labels.py`, a table-side MOS label joiner for
parquet, JSONL/NDJSON, JSON, and CSV tables. The script joins subjective MOS
labels onto already-extracted feature tables by explicit or inferred clip keys,
supports regex key extraction, writes `mos`, `mos_raw_0_100`,
`mos_label_status`, and provenance columns, rejects conflicting duplicate label
keys, and fails by default below 95% unique-key coverage.

`train_konvid_mos_head.py` will now fail with exit code `2` when a real run
loads zero MOS-labelled rows. Synthetic data is available through explicit
`--smoke`; `--allow-synthetic-fallback` exists only as a hidden legacy/debug
escape hatch.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Keep synthetic fallback in the trainer | Existing scripts keep running | Real-data mistakes produce plausible synthetic artefacts and waste GPU time | Rejected; real training must fail loudly when labels are absent |
| Join labels inside each trainer | Fewer operator steps for one model | Duplicates key-matching and coverage policy across KoNViD, CHUG, and future MOS heads | Rejected; table materialisation is easier to audit and reuse |
| Require exact key equality only | Simple implementation | Public corpus feature tables often carry numeric ids inside filenames or paths | Rejected; regex extraction is needed for KoNViD/CHUG-style joins |
| Average duplicate labels | Handles repeated label files | Masks stale reruns and conflicting source rows | Rejected; conflicting duplicates are data poisoning |

## Consequences

- **Positive**: MOS-head training can no longer silently turn an empty real
  corpus into a synthetic checkpoint.
- **Positive**: the same labelled feature parquet can feed training,
  signal-mix audits, and later retrain comparisons.
- **Positive**: match coverage and label provenance are recorded as an audit
  JSON rather than inferred from trainer logs.
- **Negative**: operators must run one extra materialisation command when
  feature extraction and MOS labels live in separate tables.
- **Neutral / follow-ups**: use the materializer for the KoNViD refresh and
  CHUG-derived HDR tables, then retrain the MOS heads with real labelled rows.

## References

- [docs/ai/mos-label-materializer.md](../ai/mos-label-materializer.md)
- [docs/ai/models/konvid_mos_head_v1.md](../ai/models/konvid_mos_head_v1.md)
- User request: "and for sure ai trainings to start? ... wake up ffs"
- User request: "and implement everything that is not blocked by the model"
