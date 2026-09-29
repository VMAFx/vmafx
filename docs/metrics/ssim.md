<!-- markdownlint-disable MD013 MD060 -->
# SSIM

SSIM (Structural Similarity Index Measure) quantifies perceptual image quality
by comparing luminance, contrast, and structure between a reference and a
distorted frame.

## Variants

| Extractor name | Backend | Algorithm | Feature name | Precision vs CPU |
|---|---|---|---|---|
| `integer_ssim` | CPU | Integer fixed-point | `ssim` | Reference |
| `vmaf_fex_integer_ssim_cuda` | CUDA | Real int64 moments + double SSIM | `ssim` | bit-exact (diff=0, places=6) |
| `vmaf_fex_integer_ssim_hip` | HIP | Real int64 moments + double SSIM | `ssim` | ≤ 1.1e-11 measured (summation order only) |
| `vmaf_fex_integer_ssim_sycl` | SYCL | int64 moments + float32 SSIM | `ssim` | places=4–5 (fp64-free, ADR-0220) |
| `vmaf_fex_integer_ssim_metal` | Metal | Fixed-point, two-pass separable Gaussian | `ssim` | places=4 (target, ADR-0214) |

A model that lists the `ssim` feature gets the twin of the active backend
(`--backend cuda/hip/sycl/metal`) automatically. The twins publish the same `"ssim"`
feature name as the CPU extractor, so existing VMAF model JSON files work unchanged.
On the CLI, `--backend cuda/hip/sycl/metal --feature ssim` runs the same twin when it
can honour the options you set, and the CPU extractor otherwise
([ADR-1359](../adr/1359-cli-feature-backend-twin.md)). Naming a twin, for example
`--backend hip --feature integer_ssim_hip`, always registers that twin.

The HIP twin was kept out of dispatch until 2026-09-18, because its kernel was an
11-tap float Gaussian 4.5e-3 away from the CPU. It now runs the CPU's 9-tap int64
kernel. Against the scalar CPU its worst measured per-frame delta is 1.06e-11, the
same as the CUDA twin, over 8- to 16-bit inputs from 1x1 to 1920x1080; see the
[HIP backend page](../backends/hip/overview.md#integer_ssim_hip).

> **Note**: There is also a `float_ssim` variant on CUDA (11-tap floating-point Gaussian,
> distinct algorithm). Prior to ADR-0564, the CUDA and HIP backends silently returned
> float_ssim scores in the `"ssim"` field. This bug is now fixed.

## `integer_ssim` extractor

The extractor uses an integer fixed-point computation compatible with the
upstream Netflix reference. It is the extractor invoked when VMAF model JSON
files reference `"integer_ssim"`. On GPU backends the same algorithm is
implemented in `ssim_cuda.c` (CUDA), `integer_ssim_hip.c` (HIP),
`integer_ssim_sycl.cpp` (SYCL), and `integer_ssim_metal.mm` +
`integer_ssim.metal` (Metal, ADR-0564 cross-backend completion).

### Output features

| Feature name | Description | Condition |
|---|---|---|
| `ssim` | SSIM of the luma (Y) plane, or its dB form with `enable_db` | Always |

The extractor is luma-only; it has no chroma option.

## Options

| Option | Type | Default | Effect |
|---|---|---|---|
| `enable_db` | bool | `false` | Report `-10 * log10(1 - ssim)` instead of the linear score. A perfect score (identical frames) is `+inf`. |
| `clip_db` | bool | `false` | Cap the dB value at `ceil(10 * log10(peak^2 / (0.5 / (w * h))))`, the dB of half a sample of error over the frame. Needs `enable_db` to have an effect. |

Backend support: the CPU extractor, `integer_ssim_sycl`, `integer_ssim_hip` and
`integer_ssim_metal` accept both options; `ssim_cuda` accepts neither. The SYCL
twin applies them on the host to the device-reduced score and reports the
CPU's `+inf` / ceiling for identical frames
([ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md)). A model that sets
an option the active backend's twin lacks computes `ssim` on the CPU
([ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md)).

### How to run

```bash
# SSIM (default, linear)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature integer_ssim --output /dev/stdout

# SSIM in dB, capped for identical frames
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature integer_ssim=enable_db=true:clip_db=true \
    --output /dev/stdout

# The same on the SYCL twin (integer_ssim_sycl)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --backend sycl --no_prediction \
    --feature integer_ssim=enable_db=true:clip_db=true --output /dev/stdout
```

## See also

- [MS-SSIM](ms-ssim.md) - multi-scale structural similarity
- [SSIMULACRA2](ssimulacra2.md) - perceptually tuned alternative
