### `vmaf-tune --fast-nr`: NR early-elimination for 2–4× bisect wall-time cut

Added `--fast-nr` flag to `vmaf-tune compare` and `vmaf-tune tune-per-shot`
(ADR-0624, implementing ADR-0615).  At each bisect midpoint the cheap
`nr_metric_v1.onnx` model (~200 ms CPU, <50 ms GPU EP) scores the distorted
stream; when `|NR − target| > δ_fast` (default 8.0 VMAF, calibrated via
`ai/scripts/calibrate_nr_threshold.py`) the full-reference VMAF call is
skipped and the bisect window advances in the NR-implied direction.  The
final accepted CRF always receives a full-reference confirmation call.

New: `NRProxyBackend` class in `score_backend.py` with per-CRF cache +
CUDA/ROCm EP auto-selection; `calibrate_nr_threshold.py` calibration script;
`fr_calls_total` / `fr_calls_saved` telemetry in `BisectResult`;
`docs/usage/vmaf-tune-fast-nr.md` user guide.
