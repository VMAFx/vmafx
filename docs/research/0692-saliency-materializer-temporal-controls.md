<!-- markdownlint-disable MD013 -->
# Research-0692: Saliency Materializer Temporal Controls

- **Status**: Completed
- **Date**: 2026-05-21
- **Tags**: ai, saliency, materializer, temporal, provenance

Companion to [ADR-0672](../adr/0672-saliency-materializer-temporal-controls.md).

## Question

The signal-mix audit identified saliency / ROI as a missing feature family in
the refreshed AI tables. The existing materializer could compute
`saliency_mean` and `saliency_var`, but it always used the historical mean
temporal reducer and wrote no row-level model/reducer attribution.

That shape is not enough for the current experiment queue:

- `vmaf-tune` already has `mean`, `ema`, `max`, and `motion-weighted` reducers;
- the U2NetP mirror exporter makes a larger saliency candidate available for
  local experiments;
- refreshed feature tables may outlive their audit sidecars once they are
  joined, filtered, or aggregated for MOS-head training.

## Findings

The reusable code already existed in `tools/vmaf-tune/src/vmaftune/saliency.py`.
The missing piece was the table materializer's call boundary and output
contract:

- pass temporal reducer options through to `compute_saliency_map()`;
- record the effective model id on rows, not only in the audit JSON;
- record the reducer and EMA alpha on rows so `mean`/`ema`/`max` experiments
  remain distinguishable after table merges;
- preserve unknown provenance on skipped rows by not backfilling metadata for
  existing anonymous saliency columns.

## Decision Input

The implementation extends `ai/scripts/materialize_saliency_features.py` with:

- `--temporal-aggregator {mean,ema,max,motion-weighted}`;
- `--ema-alpha`;
- `--model-id`;
- output metadata columns
  `saliency_model_id`, `saliency_aggregator`, and `saliency_ema_alpha`, each
  independently suppressible for compatibility.

The default remains compatible with the historical behaviour:
`saliency_student_v1`, `mean`, `ema_alpha=0.6`. The additional columns are
metadata only; `saliency_mean` / `saliency_var` keep their existing names.

## Reproducer

```bash
PYTHONPATH=. .venv/bin/python -m pytest ai/tests/test_materialize_saliency_features.py -q
```

## Follow-Ups

- Run saliency materialization on refreshed CHUG, KoNViD/UGC, Netflix, and BVI
  tables with explicit model ids.
- Compare `saliency_student_v1`, `saliency_student_v2`, and local
  `u2netp_mirror_v1` exports once U2NetP weights are available.
- Rerun `ai/scripts/signal_mix_audit.py` after the saliency columns exist.

## References

- [ADR-0396](../adr/0396-video-saliency-extension.md)
- [ADR-0650](../adr/0650-signal-mix-audit.md)
- [ADR-0655](../adr/0655-saliency-feature-materializer.md)
- [ADR-0671](../adr/0671-u2netp-mirror-exporter.md)
- Source: req — "well that means do u2netp (experimental)... we can only learn i guess lol"
