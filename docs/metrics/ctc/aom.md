<!-- markdownlint-disable MD013 MD059 -->
# AOM CTC

[AOM](http://aomedia.org/) has specified  [`vmaf`](../../../core/tools/README.md) to be the standard implementation metrics tool according to the AOM common test conditions (CTC).

## Metrics

The versioned `--aom_ctc` preset registers the metrics below with the options
the AOM CTC specifies. Each version adds to or changes the one before it (see
[the version history](#aom-ctc-version-history)).

| Metric | Registered as | Options set by the preset |
| --- | --- | --- |
| VMAF | model `vmaf_v0.6.1` (output `vmaf`) | pinned, from v1.0 |
| VMAF NEG | model `vmaf_v0.6.1neg` (output `vmaf_neg`) | pinned, from v1.0 |
| PSNR, APSNR | `psnr` | `reduced_hbd_peak=true`, `enable_apsnr=true`, `min_sse=0.5` |
| SSIM | `float_ssim` | `enable_db=true`, `clip_db=true`; `scale=1` from v7.0 |
| MS-SSIM | `float_ms_ssim` | `enable_db=true`, `clip_db=true` |
| CIEDE-2000 | `ciede` | none |
| PSNR-HVS | `psnr_hvs` | none |
| CAMBI | `cambi` | none, from v3.0 |

The two models are pinned to the v0.6.1 family regardless of the default
model of the build.

## Usage

Basic usage of the tool is described in the [`vmaf`
README](../../../core/tools/README.md). Use the versioned `--aom_ctc` presets to
register and configure all metrics according to the AOM CTC. Basic AOM CTC usage
is as follows:

```bash
./build/tools/vmaf \
    --reference reference.y4m \
    --distorted distorted.y4m \
    --aom_ctc v1.0 \
    --output output.xml
```

There are also a few optional command-line settings you may find useful.

- Use `--threads` to set the thread count to be used for multi-threaded
  computation. This will decrease the overall latency.
- If you prefer a JSON log over the default XML log, use the `--json` flag.

## Output

`XML` and `JSON` logging formats provide per-frame metrics, pooled metrics, and
aggregate metrics.

## AOM CTC Version History

| Version | Date | Change | libvmaf release |
| --- | --- | --- | --- |
| `--aom_ctc v1.0` | 2020-12-22 | Initial CTC release; `--aom_ctc proposed` deprecated. | [v2.1.0](https://github.com/Netflix/vmaf/releases/tag/v2.1.0) |
| `--aom_ctc v1.0` (revised) | 2021-01-13 | Fix for lossless comparisons; dB clipping for PSNR, APSNR, SSIM and MS-SSIM according to the AOM CTC. | [v2.1.1](https://github.com/Netflix/vmaf/releases/tag/v2.1.1) |
| `--aom_ctc v2.0` | not recorded | Same metric set as v1.0. | [v2.2.1](https://github.com/Netflix/vmaf/releases/tag/v2.2.1) |
| `--aom_ctc v3.0` | 2022-04-05 | Add CAMBI. | [v2.3.1](https://github.com/Netflix/vmaf/releases/tag/v2.3.1) |
| `--aom_ctc v4.0` | not recorded | Identical to v3.0. | not recorded |
| `--aom_ctc v5.0` | not recorded | Identical to v4.0. | not recorded |
| `--aom_ctc v6.0` | 2023-12-07 | Support bit depth conversion for Y4M inputs. | [v3.0.0](https://github.com/Netflix/vmaf/releases/tag/v3.0.0) |
| `--aom_ctc v7.0` | fork addition | Replace the SSIM entry with `float_ssim` at `scale=1`, still with dB clipping. | none (added in this fork) |

Precompiled static binaries of the upstream releases are attached to the
release pages linked above.
