<!-- markdownlint-disable MD060 -->
# Research 0677 — NR threshold calibration provenance

## Question

`ai/scripts/calibrate_nr_threshold.py` updates `model/tiny/nr_metric_v1.json`
with the affine raw-NR-to-VMAF calibration and calibrated
`calibration_threshold` used by `vmaf-tune --fast-nr`. Because those values
change when the corpus, CRF grid, codec, or NR model changes, the sidecar needs
the same ADR-0661 provenance block as the other durable AI calibration outputs.

## Findings

- The script already writes a human-facing Markdown report under
  `docs/ai/models/`, but the machine-consumed JSON sidecar only stored scalar
  calibration values.
- `NRProxyBackend` reads the JSON sidecar automatically, so the threshold is an
  operator-visible tuning input rather than a scratch report.
- Smoke calibration showed `nr_metric_v1` returns MOS-like raw scores around
  `3.x`, not VMAF-scale values. The runtime must apply
  `calibration_slope * nr_raw + calibration_intercept` before comparing to a
  target VMAF; otherwise fast-NR skips in the wrong direction.
- Recording `requested_corpus`, `actual_corpus`, `model_onnx`, parsed CLI args,
  and the Markdown report output is enough to replay the calibration without
  embedding large per-sample tables in the JSON sidecar.
- Calibration is often run while CUDA extraction jobs are active. A CPU-pinned
  NR inference mode keeps the run reproducible and avoids contending with
  long-lived CUDA/ROCm workers.
- The documented `.corpus/netflix/` default was not robust enough: it recursed
  into `dis/` bitrate-ladder files and the Netflix source names omit
  `1920x1080`. Calibration needs the reference corpus only, with explicit
  geometry handling for those source names.

## Alternatives considered

| Option | Benefit | Risk | Decision |
|---|---|---|---|
| Keep only the Markdown report | No JSON schema delta | The runtime sidecar loses the corpus/model/CRF context that produced the threshold | Rejected |
| Add a custom `calibration_metadata` object | Small local diff | Duplicates ADR-0661 path hashing and argument normalization | Rejected |
| Use ADR-0661 `run_provenance` | Matches the rest of the AI refresh sidecars | Slightly larger `nr_metric_v1.json` after calibration | Accepted |
| Add `--nr-ep cpu` | Lets operators calibrate while CUDA/ROCm is busy and records the provider policy in provenance | Slightly larger calibration CLI | Accepted |
| Prefer `ref/` and recognise Netflix 1080p names | Makes the documented default corpus path runnable | Encodes local Netflix public corpus knowledge in the script | Accepted |
| Apply affine calibration at runtime | Keeps MOS-scale NR output comparable to VMAF targets | Requires two extra sidecar fields and runtime reads | Accepted |

## Validation

- `.venv/bin/ruff check ai/scripts/calibrate_nr_threshold.py ai/tests/test_calibrate_nr_threshold.py`
- `.venv/bin/python -m pytest ai/tests/test_calibrate_nr_threshold.py -q`
- `.venv/bin/mkdocs build --strict`
