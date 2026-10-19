---
paths:
  - core/src/libvmaf.c
  - core/src/percentile.h
  - core/src/bootstrap_names.h
invariant: Pooling accumulators remain O(1) float-exact; bootstrap score names stay centralized.
---
<!-- markdownlint-disable MD013 -->
# Pooling accumulators, percentile statistics, and bootstrap names

## Pooling: accumulators stay O(1) and byte-identical; percentiles buffer (ADR-1188)

`pool_accumulate()` in `libvmaf.c` feeds two consumers from one frame walk:
O(1) `PoolAccumulators` used by `MIN` / `MAX` / `MEAN` / `HARMONIC_MEAN`, and —
only when caller asked for `MEDIAN` / `PERC5` / `PERC10` / `PERC20` —
`PoolSamples` buffer holding every pooled per-frame score.

Three invariants rebase or follow-up branch must preserve:

1. **Never derive accumulator methods from sample buffer.** Their float
   expressions are upstream ones and are pinned by Netflix golden gate
   (ADR-1118 golden-gate isolation). Summing sorted vector instead would
   change summation order and move golden numbers.
2. **Never allocate sample buffer unconditionally.** Callers legitimately
   pass `index_high == UINT_MAX`; buffer must stay opt-in per method and
   geometrically grown, never sized from `index_high - index_low`.
3. **Percentiles are order statistics and ignore perceptual weighting** —
   ADR-1118 weights change `MEAN` / `HARMONIC_MEAN` only, exactly as `MIN` /
   `MAX` are unaffected. Never weight ranks.

`vmaf_percentile()` / `vmaf_score_compare()` live in `percentile.h` as `static
inline` on purpose: `predict.c` computes the golden-asserted bootstrap `ci_p95`
bounds with same expression. Keeping it header-inline keeps that
arithmetic inside `predict.c`'s own translation unit rather than behind
cross-TU call whose contraction could differ under `-flto` (ADR-1172). Never
"clean this up" into `percentile.c`.

XML / JSON writers in `output.cpp` iterate `pool_report_order[]`, **not**
`[1, VMAF_POOL_METHOD_NB)`. Appending pooling enumerator must not widen
`pooled_metrics` schema by accident; add method to that table only as
deliberate, documented output change.

## Bootstrap score names have one owner (ADR-0480)

`bootstrap_names.h` owns four collection-score suffixes and
`BOOTSTRAP_NAME_BUF_SZ()`. Both `libvmaf.c`'s pooled-score path and
`predict.c`'s per-index append path include that header and use its symbols.
Do not restore translation-unit-local string literals: two paths would
again be able to publish different feature names. loops stay separate
because their callees and ownership contracts differ. fast source-contract
test is `core/test/test_bootstrap_name_contract.py`.

## Collection per-frame score reads stored values first (T-MODEL-SET-SCORE-NOT-IDEMPOTENT-2026-10-05)

`vmaf_score_at_index_model_collection()` calls `read_predicted_collection_score()` before predicting: four named scores (`bootstrap_names.h` suffixes) already in collector -> return them. Prediction writes members' + named scores once per frame; collector refuses rewrite (`cannot be overwritten`), so without the read a second per-frame call, or `vmaf_score_pooled_model_collection()` (predicts every frame of its range) after a per-frame call, returned `-EINVAL`. Same rule as `vmaf_score_at_index()` for one model. Keep on upstream sync (upstream lacks it). Test: `core/test/test_model_collection_score_repeat.c`.
