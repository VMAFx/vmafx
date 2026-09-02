<!-- markdownlint-disable MD013 MD060 -->
# PSNR-HVS

PSNR-HVS (Peak Signal-to-Noise Ratio - Human Visual System) extends traditional
PSNR with contrast sensitivity function (CSF) weighting applied in the DCT
domain, making it more sensitive to perceptually significant distortions.

## Variants

| Extractor name | Algorithm | Options |
|---|---|---|
| `psnr_hvs` | CSF-weighted DCT-domain PSNR | `enable_chroma` |

## `psnr_hvs` extractor

The extractor computes PSNR in the 8x8 DCT domain with per-coefficient
weighting derived from a human visual system contrast sensitivity model
(Ponomarenko et al.). It is the extractor invoked when VMAF model JSON files
reference `"psnr_hvs"`.

### Output features

| Feature name | Description | Condition |
|---|---|---|
| `psnr_hvs` | HVS-weighted PSNR on the luma (Y) plane | Always |
| `psnr_hvs_cb` | HVS-weighted PSNR on the Cb (U) plane | `enable_chroma=true` only |
| `psnr_hvs_cr` | HVS-weighted PSNR on the Cr (V) plane | `enable_chroma=true` only |

## Options

- `enable_chroma` (bool, default `false`): emit per-plane `_cb` and `_cr` scores in addition to luma. YUV400P sources are always luma-only.

### How to run

```bash
# Luma-only PSNR-HVS (default)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr_hvs --output /dev/stdout

# Per-channel PSNR-HVS (luma + Cb + Cr)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature 'psnr_hvs:enable_chroma=true' --output /dev/stdout
```

## Backend parity (chroma plane dimensions)

`psnr_hvs` runs on CPU, CUDA, HIP, and SYCL with runtime backend selection.
For chroma planes (`enable_chroma=true`) on subsampled formats (YUV420 /
YUV422) whose luma width or height is **odd**, each chroma plane dimension is
the **ceiling** of the half-resolution, not the floor — e.g. a 1921-wide 4:2:0
frame has a 961-sample-wide chroma plane (`(1921 + 1) / 2`), not 960. All
backends compute the ceiling so that `psnr_hvs_cb` / `psnr_hvs_cr` agree across
CPU / CUDA / HIP / SYCL on odd-dimension frames. (The SYCL path previously
floored the chroma dimensions, dropping the last chroma column/row on odd
inputs and diverging from the other backends.)

## See also

- [Features](features.md) - full feature extractor reference
