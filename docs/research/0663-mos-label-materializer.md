# Research-0663: MOS Label Materializer

## Question

How should refreshed feature tables receive subjective MOS labels so real
MOS-head training cannot accidentally become synthetic training?

## Findings

- Refreshed feature parquets can be structurally valid while still lacking a
  `mos` or `mos_raw_0_100` column. The trainer's old fallback made that error
  look like a successful run.
- Feature extraction and label ingestion are different lifecycle steps. Feature
  tables may be per-frame, per-clip, parquet, JSONL, or CSV-derived; labels may
  use filenames, numeric clip ids, or corpus-specific `src` fields.
- A useful join must report coverage by unique feature key, not just row count,
  because a per-frame table can repeat the same clip key many times.
- Duplicate label keys with different MOS values indicate stale or mismatched
  inputs. Averaging them would hide data poisoning before training.
- Synthetic training is still useful as a pipeline smoke, but it needs to be an
  explicit operator choice (`--smoke`), not an implicit recovery path.

## Implementation Shape

`ai/scripts/materialize_mos_labels.py` reads feature and label tables, infers
or accepts key columns, optionally applies regex extraction to both keys, and
writes:

- `mos`
- `mos_raw_0_100`
- `mos_label_status`
- `mos_label_source`
- optional `mos_std_dev`, `mos_n_ratings`, `split`, `corpus`, and
  `corpus_version`

It supports parquet, JSONL/NDJSON, JSON rows, and CSV label files. It fails by
default below 95% unique-key coverage and requires `--overwrite` before
replacing existing MOS columns.

The KoNViD MOS trainer now exits `2` when explicit real-corpus paths produce
zero labelled rows. The hidden `--allow-synthetic-fallback` flag preserves a
debug escape hatch, but documented training uses `--smoke` for synthetic runs.

## Validation

Focused tests:

```bash
.venv/bin/python -m pytest \
  ai/tests/test_materialize_mos_labels.py \
  ai/tests/test_train_konvid_mos_head.py::test_main_rejects_empty_real_corpus_without_synthetic_fallback \
  -q
```

The tests cover regex joins, 0-100 MOS conversion, low-coverage rejection,
missing-label marking, conflicting duplicate rejection, overwrite protection,
parquet/CSV round-trip, and the trainer's real-path zero-row guard.

## Limitations

- The materializer does not know whether a low match rate is expected for an
  exploratory subset. Operators can lower `--min-match-rate`, but the default
  remains promotion-oriented.
- It joins labels; it does not calibrate unrelated MOS scales beyond the
  documented 1-5 and 0-100 conversions.
- It does not deduplicate feature rows. Repeated feature keys are allowed so
  per-frame tables can receive the same clip-level MOS label.

## Recommendation

Use the materializer as the mandatory preflight for real MOS-head feature
parquets. Treat trainer exit code `2` as a data-prep failure: fix the label
join, do not use a synthetic fallback for production-like artefacts.
