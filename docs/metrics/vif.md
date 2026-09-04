# VIF

VIF (Visual Information Fidelity) measures how much of the reference image's
visual information is preserved in the distorted image, using a statistical
model of natural scenes and human visual system sensitivity.

VIF is a **luma-only metric by design** (Sheikh & Bovik, 2006 — defined on a
single luminance channel). Every libvmaf backend — CPU (AVX2, AVX-512, NEON),
CUDA, HIP, SYCL, Metal — and upstream Netflix/vmaf reads only the Y
plane (`data[0]`) of the input picture. There are no per-chroma-plane VIF
features. See [ADR-0597](../adr/0597-integer-vif-luma-only-clarification.md)
for the disposition of the 2026-05-18 deep-audit finding that questioned this
design.

## Variant

| Extractor name | Algorithm | Options |
| --- | --- | --- |
| `integer_vif` | Integer fixed-point multi-scale implementation | `vif_enhn_gain_limit`, `vif_skip_scale0`, `debug` |
| `float_vif` | Floating-point 4-scale Gaussian-pyramid implementation (feature `float_vif` + `float_vif_scale0..3`) | `vif_enhn_gain_limit`, `vif_kernelscale`, `vif_sigma_nsq`, `vif_skip_scale0`, `debug` |

`float_vif` is implemented on every GPU backend: CUDA (`float_vif_cuda`), SYCL
(`float_vif_sycl`), HIP (`float_vif_hip`), and **Metal (`float_vif_metal`,
`feature/metal/float_vif.metal` + `_metal.mm`)** — a 4-scale separable-Gaussian
pyramid with per-scale mean/variance/covariance statistics, parity-checked vs
the CPU `float_vif` on the macOS Apple-Silicon CI lane (`places=4`, ADR-0214).

The CUDA twin (`vif_cuda`) additionally carries a vestigial `enable_chroma`
option (see Options table below); it is a documented no-op.

## `integer_vif` extractor

The extractor uses an integer fixed-point implementation of the VIF algorithm
across multiple spatial scales. It is the extractor invoked when VMAF model
JSON files reference `"integer_vif"`.

`integer_vif` (feature `vif`) is implemented on every GPU backend: CUDA
(`vif_cuda`), SYCL (`vif_sycl`), HIP (`vif_hip`), and **Metal
(`integer_vif_metal`, `feature/metal/integer_vif.metal` + `_metal.mm`)** — a
4-scale fixed-point Gaussian pyramid with int64 moment accumulators and the
integer log2-LUT, mirroring the proven `float_vif_metal` scaffold. Parity vs the
CPU `vif` is checked on the macOS Apple-Silicon CI lane (`places=4`, ADR-0214);
because Apple GPUs lack fp64 the per-pixel gain is computed in float (the same
fp64-free trade-off as the SYCL twin, ADR-0220).

### Output features

All features are computed on the luma (Y) plane only.

| Feature name | Description | Condition |
| --- | --- | --- |
| `integer_vif` | Fused VIF score (luma) | `debug=true` |
| `VMAF_integer_feature_vif_scale0_score` | VIF at scale 0 (finest, luma) | Always (or `0.0` when `vif_skip_scale0=true`) |
| `VMAF_integer_feature_vif_scale1_score` | VIF at scale 1 (luma) | Always |
| `VMAF_integer_feature_vif_scale2_score` | VIF at scale 2 (luma) | Always |
| `VMAF_integer_feature_vif_scale3_score` | VIF at scale 3 (coarsest, luma) | Always |
| `integer_vif_num`, `integer_vif_den` | Fused numerator/denominator | `debug=true` |
| `integer_vif_num_scaleN`, `integer_vif_den_scaleN` (N=0..3) | Per-scale num/den | `debug=true` |

## Options

| Option | Alias | Type | Default | Range | Effect |
| --- | --- | --- | --- | --- | --- |
| `vif_enhn_gain_limit` | `egl` | double | `100.0` | `1.0–100.0` | Cap the per-pixel enhancement-gain ratio so over-sharpened content cannot saturate the score. Set to `1.0` to disable enhancement gain entirely. |
| `vif_skip_scale0` | `ssclz` | bool | `false` | — | Skip scale-0 (finest pyramid level) calculations; scale-0 outputs are forced to `0.0` and excluded from the fused score. Matches the CPU path for GPU backends. |
| `debug` | — | bool | `false` | — | Emit additional per-scale numerator/denominator debug metrics. |
| `enable_chroma` (CUDA twin only) | — | bool | `false` | — | **No-op** retained for backward compatibility with callers that pass the option on the CLI or in a model JSON. VIF is luma-only by design; setting `enable_chroma=true` emits a one-shot warning during init and otherwise has no effect. See [ADR-0597](../adr/0597-integer-vif-luma-only-clarification.md). |

### How to run

```bash
# Luma-only VIF (the only mode that exists across every backend)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature integer_vif --output /dev/stdout

# Skip scale-0 (GPU-parity mode)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature 'integer_vif:vif_skip_scale0=true' --output /dev/stdout
```

## Minimum frame size

`float_vif` runs a four-scale pyramid: each scale halves the working dimension
and then convolves at that size with the scale's own separable Gaussian —
17, 9, 5 and 3 taps at the default `vif_kernelscale` of 1.0. Every one of those
convolutions needs at least `filter_width / 2 + 1` samples in each axis for the
reflect-101 mirror padding to stay inside the plane, so the frame minimum is
the largest `(filter_width_s / 2 + 1) << s` over the ladder:

| Scale | Filter width | Needs at that scale | Implies at full size |
| --- | --- | --- | --- |
| 0 | 17 | 9 | 9 |
| 1 | 9 | 5 | 10 |
| 2 | 5 | 3 | 12 |
| 3 | 3 | 2 | **16** |

**`float_vif` therefore rejects any frame below 16x16** with `-EINVAL` at
`init()`:

```text
libvmaf ERROR float_vif requires width >= 16 and height >= 16 for the
four-scale ladder (got 12x12)
```

The bound is derived from `vif_kernelscale`, not hard-coded, so a non-default
kernel scale moves it. When `vif_prescale != 1.0` the check is applied to the
*scaled* dimensions — the ones actually handed to the pyramid — as well as to
the raw input.

Before ADR-1166 the guard only covered scale 0 and admitted anything at or
above 9x9, so frames in 9..15 px reached the scale-3 convolution with a
sub-minimum plane and read out of bounds
([Netflix/vmaf#1582](https://github.com/Netflix/vmaf/issues/1582)). If you were
scoring 9..15 px input, that run was reading uninitialised memory and its
scores were not meaningful; upscale the input or use a smaller
`vif_kernelscale`.

## Cross-backend parity

The `core/test/test_integer_vif_cpu_cuda_parity.c` smoke test (suite
`fast`, runs when `enable_cuda=true`) asserts CPU vs CUDA `vif_scaleN_score`
agreement within `1e-5` on the Netflix `src01_hrc00_576x324` reference pair
— making the luma-only parity claim a regression gate.

## See also

- [Features](features.md) - full feature extractor reference
- [ADR-0597](../adr/0597-integer-vif-luma-only-clarification.md) - why
  `enable_chroma` is a documented no-op on the CUDA twin.
