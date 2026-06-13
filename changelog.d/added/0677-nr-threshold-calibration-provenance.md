`ai/scripts/calibrate_nr_threshold.py` now records affine NR-to-VMAF
calibration fields plus ADR-0661 `run_provenance` inside refreshed
`nr_metric_v1.json` calibration sidecars so `--fast-nr` thresholds can be
traced back to their corpus, model, CRF grid, CLI arguments, and Markdown
report.
`NRProxyBackend` applies `calibration_slope * nr_raw + calibration_intercept`
before comparing the NR prediction to a VMAF target.
The calibration CLI also accepts `--nr-ep cpu` to pin NR inference to CPU when
CUDA/ROCm is reserved for long-running feature extraction.
The default `.corpus/netflix/` path now sweeps `ref/` only and recognises the
local Netflix public source names as 1080p YUVs.
