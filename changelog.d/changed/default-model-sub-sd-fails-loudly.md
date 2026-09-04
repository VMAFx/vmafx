- The `vmaf` CLI now refuses, with a specific error, to auto-load the default
  model on an input it cannot score: `model 'vmaf_v1.0.16_3d0h' requires
  feature 'cambi', which needs width or height >= 216; got 160x90`. The
  message names the model, the feature and the constraint, is printed even
  under `--quiet`, and the process exits non-zero. Previously such inputs
  failed with a misleading `no frames decoded`. Pass `--model` explicitly to
  score a clip below the default model's limits. The thresholds are read from
  the extractors' own headers via `core/src/feature/feature_dimensions.h`.
