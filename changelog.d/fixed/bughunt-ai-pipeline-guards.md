Harden three AI training-pipeline data-integrity paths surfaced by the
2026-06-27 bug-hunt sweep (cluster T-BUGHUNT-AI-2026-06-27):

- `ai/scripts/aggregate_corpora.py`: a `null` (or otherwise non-numeric)
  `mos` cell satisfied the required-key schema check but raised `TypeError`
  in `float()` before `convert_mos` could run, crashing the entire
  multi-corpus aggregation. The driver now catches `(ValueError, TypeError)`
  so malformed-MOS rows are counted in `dropped_bad_scale` and skipped.
- `ai/train/konvid_pair_dataset.py`: non-finite (`NaN` / `inf`) `vmaf` or
  feature values are now dropped at load time (with a warning) instead of
  becoming `NaN` training targets that silently poison the regressor's loss;
  `numpy_arrays()` asserts finiteness as a safety net.
- `ai/scripts/materialize_saliency_features.py`: the cached decode/model
  failure replay now stamps sibling rows with the original failure status
  (`decode-failed` / `model-failed` / `missing-source` / `missing-geometry`)
  instead of leaving a blank `saliency_status` column, restoring provenance
  and keeping the missing-source summary count accurate.
- `ai/scripts/extract_k150k_features.py`: duplicate `video_name` keys in the
  scores CSV are now detected and hard-fail (exit 2) before any GPU time —
  previously `dict(zip(..., strict=True))` only checked equal length, so
  duplicate keys silently collapsed the MOS map and dropped a clip's label.

Training-harness only; no libvmaf C-API, CLI, model, or Netflix golden-data
impact. Regression tests added in `ai/tests/`.
