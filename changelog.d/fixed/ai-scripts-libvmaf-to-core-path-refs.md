Fixed stale `libvmaf/build-cpu/tools/vmaf` default binary paths in four files
(`ai/scripts/extract_k150k_features.py`, `bvi_dvc_to_full_features.py`,
`konvid_to_vmaf_pairs.py`, `ai/data/feature_extractor.py`) to `core/build-cpu/tools/vmaf`
following the ADR-0700 `libvmaf/ → core/` directory rename.  Without this fix the
default `--vmaf-bin` resolved to a nonexistent path, causing an immediate pre-flight
failure for any invocation that did not pass an explicit binary path.
