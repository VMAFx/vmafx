- `ai/scripts/calibrate_nr_threshold.py` now refuses to write weak fast-NR
  calibration sidecars unless `--allow-weak-calibration` is passed, preventing
  non-predictive NR fits from driving `vmaf-tune --fast-nr` early elimination.
