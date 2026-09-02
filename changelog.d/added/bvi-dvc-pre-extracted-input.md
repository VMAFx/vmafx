# `bvi_dvc_to_full_features.py` accepts `--bvi-dir` for pre-extracted YUVs

`ai/scripts/bvi_dvc_to_full_features.py` gains a `--bvi-dir PATH` argument
(ADR-0527) that accepts a directory of already-extracted BVI-DVC `.mp4` or
`.yuv` files, skipping the zip streaming-extraction step entirely. Raw `.yuv`
inputs are fed directly to libvmaf as the reference without an intermediate
decode step. `--bvi-dir` and `--bvi-zip` are mutually exclusive; omitting both
preserves the legacy zip-default path via `$VMAF_BVI_DVC_ZIP` or the hard-coded
`.workingdir2/` fallback.
