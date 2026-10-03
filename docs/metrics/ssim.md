<!-- markdownlint-disable MD013 MD060 -->
# SSIM

SSIM (Structural Similarity Index Measure) quantifies perceptual image quality
by comparing luminance, contrast, and structure between a reference and a
distorted frame. The fork's integer SSIM extractor scores the luma plane, uses
fixed-point arithmetic compatible with the upstream Netflix reference, and
feeds the `ssim` feature that VMAF model JSON files list.

## How to run

The extractor is registered as `ssim`. The CLI also accepts `integer_ssim` as
an alias for it; the C API (`vmaf_use_feature(ctx, "integer_ssim")`) does not.

```bash
# SSIM (default, linear)
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature ssim --output /dev/stdout

# SSIM in dB, capped for identical frames
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature ssim=enable_db=true:clip_db=true \
    --output /dev/stdout

# The same on the SYCL twin (integer_ssim_sycl)
build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 1920 --height 1080 --pixel_format 420 --bitdepth 8 \
    --backend sycl --no_prediction \
    --feature ssim=enable_db=true:clip_db=true --output /dev/stdout
```

Options follow the feature name after the first `=`, separated by `:`.

## Output features

| Feature name | Description | Condition |
|---|---|---|
| `ssim` | SSIM of the luma (Y) plane, or its dB form with `enable_db` | Always |

The extractor is luma-only; it has no chroma option.

## Options

| Option | Type | Default | Effect |
|---|---|---|---|
| `enable_db` | bool | `false` | Report `-10 * log10(1 - ssim)` instead of the linear score. A perfect score (identical frames) is `+inf`. |
| `clip_db` | bool | `false` | Cap the dB value at `ceil(10 * log10(peak^2 / (0.5 / (w * h))))`, the dB of half a sample of error over the frame. Needs `enable_db` to have an effect. |

The CPU extractor and the CUDA, HIP, SYCL and Metal twins accept both options.
A model that sets an option the active backend's twin lacks computes `ssim` on
the CPU ([ADR-1183](../adr/1183-model-options-gate-gpu-twin-selection.md)).

## Variants

| Registered name | Backend | Algorithm | Precision vs CPU |
|---|---|---|---|
| `ssim` | CPU | Integer fixed-point | Reference |
| `integer_ssim_cuda` | CUDA | Real int64 moments + double SSIM, summed in the CPU's order | Bit-identical at every frame size |
| `integer_ssim_hip` | HIP | Real int64 moments + double SSIM, summed in the CPU's order | Bit-identical at every frame size |
| `integer_ssim_sycl` | SYCL | Real int64 moments + the CPU's double SSIM term computed in 64-bit integers, summed in the CPU's order | Bit-identical at every frame size and bit depth |
| `integer_ssim_metal` | Metal | Fixed-point, two-pass separable Gaussian | places=4 (target, ADR-0214) |

All twins publish the feature name `ssim`, so existing VMAF model JSON files
work unchanged. Their sources are `ssim_cuda.c` (CUDA), `integer_ssim_hip.c`
(HIP), `integer_ssim_sycl.cpp` (SYCL) and `integer_ssim_metal.mm` +
`integer_ssim.metal` (Metal, ADR-0564 cross-backend completion).

### Selecting a twin

- **By model.** A model that lists the `ssim` feature gets the twin of the
  active backend (`--backend cuda|hip|sycl|metal`) automatically.
- **By feature name.** `--backend cuda|hip|sycl|metal --feature ssim` runs the
  same twin when it can honour the options you set, and the CPU extractor
  otherwise ([ADR-1359](../adr/1359-cli-feature-backend-twin.md)).
- **By twin name.** Naming a twin, for example
  `--backend hip --feature integer_ssim_hip`, always registers that twin.

!!! note
    There is also a `float_ssim` variant on CUDA (11-tap floating-point
    Gaussian, a distinct algorithm). Before ADR-0564 the CUDA and HIP backends
    silently returned `float_ssim` scores in the `"ssim"` field. This bug is
    fixed.

## GPU twins

The CUDA, HIP and SYCL twins give the CPU's value for every frame, down to the
last bit of the `--precision max` output, with and without `enable_db` and
`clip_db`. Each computes every per-pixel term as the CPU does, reads the terms
back and adds them on the host in the CPU's order. They report the CPU's value
exactly, finite or `+inf`, and apply the options on the host to the
device-reduced score
([ADR-1365](../adr/1365-sycl-twin-cpu-option-parity.md),
[ADR-1373](../adr/1373-cuda-twin-cpu-option-parity.md),
[ADR-1382](../adr/1382-hip-twin-cpu-option-parity.md); the HIP twin's gfx1036
run is in `T-BUG048-GPU-OPTION-PARITY-REMAINDER-2026-09-26` in
[`state.md`](../state.md)).

| Twin | Exact vs CPU | ADR | Evidence (fragment) | Cost at 3840x2160 | Extra memory at 3840x2160 |
|---|---|---|---|---|---|
| `integer_ssim_cuda` | Yes | [ADR-1424](../adr/1424-cuda-ssim-cpu-frame-sum.md) | `scripts/ci/exact_twins.d/ssim.cuda` | 9.7 ms instead of 2.2 ms (RTX 4090); not measurable at 576x324 | 66 MB device, as much pinned host |
| `integer_ssim_hip` | Yes | [ADR-1438](../adr/1438-hip-ssim-cpu-frame-sum.md) | `scripts/ci/exact_twins.d/ssim.hip` | 98.1 ms instead of 94.3 ms (gfx1036); 30.0 ms instead of 28.2 ms at 1920x1080 | 66 MB device, as much pinned host |
| `integer_ssim_sycl` | Yes | [ADR-1443](../adr/1443-sycl-ssim-cpu-arithmetic.md) | `scripts/ci/exact_twins.d/ssim.sycl` | 31.9 ms instead of 17.8 ms (Arc A380; 0.78 ms instead of 0.45 ms at 576x324); the CPU extractor takes 111 ms | 66 MB pinned host; device memory unchanged |
| `integer_ssim_metal` | Tolerance (`FEATURE_TOLERANCE`, ADR-0214) | [ADR-0564](../adr/0564-integer-ssim-gpu-real-kernels.md) | none (not declared exact) | no figure recorded | no figure recorded |

The CPU's sum is sequential, so an exact twin pays for reading the terms back:
that is the cost column. At large frames the twin is slower than a device
reduction would be.

### What exactness means when you use it

`--backend cuda --feature ssim` gives the same number as
`--backend cpu --feature ssim` for every frame, down to the last bit of the
`--precision max` output, with and without `enable_db` / `clip_db` (the same
holds for `--backend hip` and `--backend sycl`).

- You can mix CPU and twin `ssim` results in one data set.
- Stored outputs from before the exact-sum ADRs differ from new ones: up to
  1.1e-11 (3.6e-10 in dB) for CUDA, up to 1.1e-11 on frames above 4096 pixels
  for HIP, and 7e-9 to 3e-7 on video for SYCL.

### Measured coverage

| Twin | Device | Measured on |
|---|---|---|
| CUDA | RTX 4090 | Netflix 576x324 pair at 8, 10, 12 and 16 bits, both 1920x1080 checkerboard pairs, 3840x2160, frames down to 1x1 |
| HIP | gfx1036 | Netflix 576x324 pair at 8, 10, 12 and 16 bits and as 10-bit 4:2:2, both 1920x1080 checkerboard pairs, 48 frames of 3840x2160, full-range noise at four depths, frames down to 1x1 |
| SYCL | Arc A380 | Netflix 576x324 pair at 8, 10, 12 and 16 bits and 4:2:2, both 1920x1080 checkerboard pairs, 200 frames of 3840x2160, frames down to 1x1 |

### SYCL specifics

A SYCL kernel has no `double` on the GPUs the backend targets. The twin
therefore runs the CPU's double-precision operations for each pixel in 64-bit
integers, which gives the CPU's `double` exactly, and adds the per-pixel
values on the host in the CPU's order.

- **16-bit input works.** Before ADR-1443 the twin's single-precision term
  overflowed on 16-bit frames and the run stopped with `invalid ratio`.
- **dB values.** With `enable_db`, the linear score is the CPU's and the dB
  value is computed from it on the host. It equals the CPU's when both come
  from the same build. Between a build made with the Intel compiler and one
  made with GCC the last digit of the dB value can differ (3.6e-15 measured),
  because the two link different `log10` implementations; that affects the CPU
  extractor in the same way.

## History

- **2026-09-18, HIP twin enters dispatch.** It was kept out of dispatch
  because its kernel was an 11-tap float Gaussian 4.5e-3 away from the CPU. It
  now runs the CPU's 9-tap int64 kernel and adds the per-pixel terms in the
  CPU's order, so its score is the CPU's bit for bit; see the
  [HIP backend page](../backends/hip/overview.md#integer_ssim_hip).
- **ADR-1443, SYCL twin exact.** Before it the twin computed in single
  precision and was 7e-9 to 3e-7 from the CPU on video.
- **ADR-1438, HIP twin exact.** Before it, frames above 4096 pixels differed
  from the CPU in the last digits (up to 1.1e-11).
- **ADR-1424, CUDA twin exact.** Before it the twin differed from the CPU in
  the last digits (up to 1.1e-11, and 3.6e-10 in dB).
- **ADR-0564.** Before it, the CUDA and HIP backends returned `float_ssim`
  scores in the `"ssim"` field.

## See also

- [MS-SSIM](ms-ssim.md) - multi-scale structural similarity
- [SSIMULACRA2](ssimulacra2.md) - perceptually tuned alternative
