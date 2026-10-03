<!-- markdownlint-disable MD013 -->
# NFLX CTC

## Metrics

`--nflx_ctc v1.0` registers the metrics below. The models are the 4K VMAF
models, so the preset is meant for 3840x2160 content.

| Metric | Registered as | Options set by the preset |
| --- | --- | --- |
| VMAF | model `vmaf_4k_v0.6.1` (output `vmaf`) | none |
| VMAF NEG | model `vmaf_4k_v0.6.1neg` (output `vmaf_neg`) | none |
| PSNR, APSNR | `psnr` | `enable_chroma=true`, `enable_apsnr=true` |
| SSIM | `float_ssim` | `enable_db=true`, `clip_db=true` |
| CAMBI | `cambi` | none |

## Usage

Basic usage of the tool is described in the [`vmaf`
README](../../../core/tools/README.md). Use the versioned `--nflx_ctc` presets
to register and configure all metrics according to the NFLX CTC. Basic usage is
as follows:

```bash
./build/tools/vmaf \
    --reference reference.y4m \
    --distorted distorted.y4m \
    --nflx_ctc v1.0 \
    --json \
    --output output.json
```

There are also a few optional command-line settings you may find useful.

- Use `--threads` to set the thread count to be used for multi-threaded
  computation. This will decrease the overall latency.

## Output

The preset is intended for the JSON log (`--json`), which provides per-frame
metrics, pooled metrics and aggregate metrics.

## NFLX CTC Version History

- v1.0: `--nflx_ctc v1.0`
  - 2023-01-01
  - Initial CTC release
