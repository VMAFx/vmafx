# SSIMULACRA 2 Extractor

SSIMULACRA 2 is a full-reference perceptual similarity metric from the JPEG XL
ecosystem. It compares reference and distorted frames in an XYB-inspired colour
space, combines multi-scale SSIM-style structural terms, and applies an
asymmetric penalty for lost texture energy. The fork ships it as a normal
libvmaf feature extractor named `ssimulacra2`.

## Output

| Field | Value |
| --- | --- |
| Feature name | `ssimulacra2` |
| Output metric | `ssimulacra2` |
| Direction | Higher is better |
| Range | `[0, 100]`; identical frames return `100` |
| Snapshot gate | `python/test/ssimulacra2_test.py` |

Practical score bands:

| Score | Meaning |
| --- | --- |
| `90-100` | Visually lossless |
| `70-90` | High quality |
| `50-70` | Medium quality, clearly lossy |
| `30-50` | Low quality |
| `0-30` | Very low quality |

## Usage

```bash
vmaf \
    --reference ref.yuv \
    --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --feature ssimulacra2 \
    --output score.json
```

The per-frame JSON metric key is `ssimulacra2`; pooled values appear under the
same key in `pooled_metrics`.

## Options

| Option | Type | Default | Values | Effect |
| --- | --- | --- | --- | --- |
| `yuv_matrix` | int | `0` | `0..3` | `0`: BT.709 limited, `1`: BT.601 limited, `2`: BT.709 full, `3`: BT.601 full |

Example:

```bash
vmaf ... --feature ssimulacra2=yuv_matrix=2
```

## Inputs And Backends

- Pixel formats: YUV 4:2:0, 4:2:2, and 4:4:4.
- Bit depths: 8, 10, and 12 bpc.
- CPU SIMD: AVX2, AVX-512, NEON, and SVE2 when the host advertises it.
- GPU twins: `ssimulacra2_vulkan`, `ssimulacra2_cuda`, and
  `ssimulacra2_sycl`.

The CPU scalar/SIMD path is bit-exact across the fork's host matrix. The GPU
twins offload the pyramid blur and per-pixel multiply stages while keeping the
XYB conversion and final combine on the host.

## Limitations

- Chroma is nearest-neighbour upsampled to luma resolution before colour
  conversion.
- The recursive Gaussian coefficient derivation is pinned to the shipped
  sigma used by the libjxl reference path; arbitrary sigma values are not a
  user option.
- Scores are useful as a perceptual ranking signal, not as an MOS-calibrated
  VMAF replacement.

## See Also

- [Feature extractor matrix](features.md#ssimulacra-2-perceptual-similarity-in-xyb-space)
- [ADR-0130](../adr/0130-ssimulacra2-scalar-implementation.md)
- [ADR-0164](../adr/0164-ssimulacra2-snapshot-gate.md)
- [ADR-0201](../adr/0201-ssimulacra2-vulkan-kernel.md)
- [ADR-0206](../adr/0206-ssimulacra2-cuda-sycl.md)
