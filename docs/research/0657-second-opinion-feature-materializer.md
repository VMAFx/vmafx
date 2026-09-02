# Research Digest: Second-Opinion Feature Materializer

## Question

How should refreshed AI feature tables receive no-reference and subjective-MOS
second opinions without turning the corpus materialisation path into a wrapper
for every third-party VQA project?

## Findings

- The signal-mix audit already identifies NR/MOS as a missing family in
  refreshed tables. A retrain cannot use DOVER, Q-Align, FAST-VQA, MUSIQ,
  CLIP-IQA, or the fork's own NR scorer unless their scores become columns.
- The useful output shape is a clip-level table join, not a new feature
  extractor. Many scorers operate on compressed clips, decoded frames, or
  private model stacks that do not fit the libvmaf extractor ABI.
- `tools/external-bench/` is intentionally wrapper-only. Keeping scorer
  execution outside the materializer avoids importing external project licences
  and keeps unit tests independent of optional binaries.
- Silent duplicate joins are dangerous. Two score rows for the same scorer and
  clip usually mean a stale rerun or mismatched key, so the materializer should
  fail instead of averaging.

## Implementation Shape

`ai/scripts/materialize_second_opinion_features.py` reads parquet, JSONL, NDJSON,
or JSON feature tables and one or more score JSON/JSONL files. It infers or
accepts an explicit feature-table key, normalises path-like keys, and appends
five columns per scorer:

- `second_opinion_<scorer>_score`
- `second_opinion_<scorer>_status`
- `second_opinion_<scorer>_runtime_ms`
- `second_opinion_<scorer>_frames`
- `second_opinion_<scorer>_source`

The script accepts scalar score rows and external-bench wrapper-style payloads
with `frames[].predicted_vmaf_or_mos`. The signal-mix audit recognises these
columns as NR/MOS evidence.

## Validation

Unit tests use synthetic JSONL/parquet inputs only:

```bash
.venv/bin/python -m pytest ai/tests/test_second_opinion_features.py \
  ai/tests/test_signal_mix_audit.py -q
```

The tests cover row-key joins, wrapper-payload averaging, missing-score marking,
`--missing-policy fail`, duplicate rejection, parquet round-tripping, and the
updated signal-family classifier.

## Limitations

- The materializer does not run scorers. Operators must create score files with
  a stable key.
- Score semantics are scorer-specific. The script records the values but does
  not calibrate DOVER/Q-Align/MUSIQ onto a common MOS scale.
- A missing score can be marked or failed, but the script cannot determine
  whether the scorer skipped a row for a valid reason.

## Recommendation

Ship the joiner now, use it for CHUG/KoNViD/UGC/Netflix refreshed tables, and
measure whether the added `second_opinion_*` columns improve MOS-head and
predictor retrain gates before promoting any new checkpoint.
