<!-- markdownlint-disable MD025 -->
# Research 0113: FR-from-NR CUDA Feature Split

# Research-0113

## Summary

Local CHUG FULL_FEATURES extraction used `ai/scripts/extract_k150k_features.py`
with `core/build-cuda/tools/vmaf --backend cuda`. The all-feature bundle
failed on 10-bit CHUG clips with duplicate feature-key warnings and
`context could not be synchronized`.

Direct repro on a decoded CHUG clip showed:

- the MP4 decode and raw YUV size were valid;
- explicit CUDA extractor names worked for `adm_cuda`, `vif_cuda`,
  `motion_cuda`, `motion_v2_cuda`, `psnr_cuda`, `ciede_cuda`,
  `float_ms_ssim_cuda`, `psnr_hvs_cuda`, and `ssimulacra2_cuda`;
- `float_ssim_cuda` failed independently on the repro clip;
- `cambi_cuda` was not loadable by feature name on the local binary;
- CPU `float_ssim` and CPU `cambi` worked on the same YUV.

## Decision Input

The stable local routing is therefore:

1. CUDA pass with explicit CUDA feature names for the working CUDA set.
2. CPU residual pass for `float_ssim` and `cambi`.
3. Merge per-frame metrics before existing aggregation.

This keeps the 22-feature parquet schema stable and avoids retrying a known
bad generic `--backend cuda` command.

## Smoke

```bash
PYTHONPATH=ai/src .venv/bin/python ai/scripts/extract_k150k_features.py \
  --clips-dir .workingdir2/chug/clips \
  --scores .workingdir2/chug/chug_scores.csv \
  --vmaf-bin core/build-cuda/tools/vmaf \
  --cpu-vmaf-bin build-cpu/tools/vmaf \
  --out .workingdir2/chug/debug/split_real.parquet \
  --threads 2 --threads-cuda 1 --flush-every 1 \
  --scratch-dir .workingdir2/chug/debug/split_real_scratch \
  --limit 1
```

Result: `ok=1 fail=0` on the same local CHUG path that previously failed.
