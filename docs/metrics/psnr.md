<!-- markdownlint-disable MD013 MD060 -->
# PSNR

Peak Signal-to-Noise Ratio: the log-ratio of the maximum possible sample
value to the mean squared error between the reference and the distorted
frame. Higher is better; identical frames are `+inf`.

Two extractors ship it. `psnr` is the fixed-point (integer-accumulated)
path and is what VMAF model JSON files and the CLI's `--feature psnr`
select. `float_psnr` converts both planes to `float` first and is kept for
parity with upstream consumers of the float pipeline; it emits a luma-only
score.

## Variants

| Extractor name | Backend | Feature names | Source |
|---|---|---|---|
| `psnr` | CPU (+ AVX2 / AVX-512 / NEON) | `psnr_y`, `psnr_cb`, `psnr_cr` | `core/src/feature/integer_psnr.c` |
| `psnr_cuda` | CUDA | same | `core/src/feature/cuda/integer_psnr_cuda.c` |
| `psnr_sycl` | SYCL | same | `core/src/feature/sycl/integer_psnr_sycl.cpp` |
| `psnr_hip` | HIP | same | `core/src/feature/hip/integer_psnr_hip.c` |
| `integer_psnr_metal` | Metal | same | `core/src/feature/metal/integer_psnr_metal.mm` |
| `float_psnr` | CPU (+ AVX2 / AVX-512 / NEON) | `float_psnr` | `core/src/feature/float_psnr.c` |
| `float_psnr_cuda` / `_sycl` / `_hip` / `_metal` | CUDA / SYCL / HIP / Metal | `float_psnr` | `core/src/feature/{cuda,sycl,hip,metal}/float_psnr_*` |

GPU twins are selected automatically when the corresponding `--backend` is
active. They emit the same feature names as the CPU extractor, so a model
JSON referencing `psnr` works unchanged. GPU scores are *not* bit-identical
to the CPU reference — see [Backends](../backends/index.md).

## How the score is computed

For each plane `p`:

```text
sse_p = sum over samples of (ref - dis)^2
mse_p = sse_p / (w_p * h_p)
psnr_p = 10 * log10(peak^2 / mse_p)
```

`peak` is `(1 << bpc) - 1` for the integer extractor (255 at 8 bpc, 1023 at
10 bpc, …). `float_psnr` normalises high bit depths back onto an 8-bit
scale and uses `peak` = 255.0 / 255.75 / 255.9375 / 255.99609375 for
8 / 10 / 12 / 16 bpc.

### The `psnr_max` ceiling and the `uncapped` option

When the two planes are byte-identical the SSE is zero and the true PSNR is
`+inf`, which no output schema can carry. Both extractors therefore report a
finite stand-in, `psnr_max`:

| Bit depth | `psnr` (`6 × bpc + 12`) | `float_psnr` |
|---|---|---|
| 8 | 60 dB | 60 dB |
| 10 | 72 dB | 72 dB |
| 12 | 84 dB | 84 dB |
| 16 | 108 dB | 108 dB |

Historically the same ceiling was *also* applied to every genuinely computed
value, so any frame whose true PSNR exceeded it was silently reported as the
ceiling. A 576x324 8-bit pair differing by a single luma step (SSE 1 over
186624 samples, true PSNR 100.840479 dB) reported `psnr_y = 60.000000`.

Since [ADR-1193](../adr/1193-psnr-uncapped-option.md) the two roles are
separate. The `uncapped` option (bool, default `false`) drops the truncation
and keeps the sentinel:

| Case | default | `uncapped=true` |
|---|---|---|
| `mse == 0` (identical planes) | `psnr_max` | `psnr_max` |
| `mse > 0`, true PSNR below `psnr_max` | true value | true value |
| `mse > 0`, true PSNR above `psnr_max` | `psnr_max` (**truncated**) | true value |

The default is unchanged and bit-identical to previous releases, so
`uncapped` never moves an existing score unless you ask for it. The option
exists on `psnr` and `float_psnr` and on all eight GPU twins under the same
name and default. It does not change any feature name.

```bash
# Truncated at the 60 dB ceiling (the default, unchanged behaviour)
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr --feature float_psnr --output /dev/stdout
#   "psnr_y": 60.000000   "float_psnr": 60.000000

# True value reported; identical chroma planes still report the 60 dB sentinel
core/build/tools/vmaf \
    --reference ref.yuv --distorted dist.yuv \
    --width 576 --height 324 --pixel_format 420 --bitdepth 8 \
    --no_prediction --feature psnr=uncapped=true \
    --feature float_psnr=uncapped=true --output /dev/stdout
#   "psnr_y": 100.840479  "psnr_cb": 60.000000  "float_psnr": 100.840479
```

Use `uncapped=true` whenever you compare against another PSNR implementation
(FFmpeg's `psnr` filter reports 100.840479 on that same pair), or whenever
you score near-lossless encodes where clipping at 60 dB destroys the ranking
between candidates. Leave it off when you need scores comparable with
previously published VMAF/PSNR numbers, and note that VMAF model JSON files
consume the *capped* feature — turning it on changes what a model that
includes a PSNR term sees.

### `min_sse` — the older escape hatch

`min_sse` (double, default `0.0`) constrains the minimum MSE, which raises
`psnr_max` to `ceil(10 * log10(peak^2 / (min_sse / n_samples)))`. It also
lifts the score of *identical* planes, because it moves the sentinel rather
than removing the truncation: on the pair above,
`--feature psnr=min_sse=0.000001` gives `psnr_y = 100.840479` but reports
`psnr_cb = 155.000000` for byte-identical chroma. Prefer `uncapped` unless
you specifically want a raised sentinel; `min_sse` is integer-extractor-only
and is not mirrored on the GPU twins.

## Options

### `psnr`

| Option | Type | Default | Effect |
|---|---|---|---|
| `enable_chroma` | bool | `true` | Emit `psnr_cb` / `psnr_cr` as well as `psnr_y`. Forced `false` for YUV400P. |
| `enable_mse` | bool | `false` | Also emit `mse_y` / `mse_cb` / `mse_cr`. |
| `enable_apsnr` | bool | `false` | Also emit the clip-aggregate `apsnr_y/cb/cr` at flush. |
| `reduced_hbd_peak` | bool | `false` | Use `255 << (bpc - 8)` as the peak, matching HBD content that was scaled up from 8-bit. |
| `min_sse` | double | `0.0` | Constrain the minimum MSE, raising both the ceiling and the identical-plane sentinel. |
| `uncapped` | bool | `false` | Report the true PSNR instead of truncating at `psnr_max`. The `mse == 0` sentinel is unaffected. |

The GPU twins implement `enable_chroma` and `uncapped`. `enable_mse`,
`enable_apsnr`, `reduced_hbd_peak` and `min_sse` remain CPU-only — see
`core/AGENTS.md` for that standing divergence.

### `float_psnr`

| Option | Type | Default | Effect |
|---|---|---|---|
| `uncapped` | bool | `false` | As above. |

`float_psnr` is luma-only on every backend.

## Output

**Metrics** — `psnr_y`, `psnr_cb`, `psnr_cr` (fixed); `float_psnr` (float).
Plus `mse_*` and `apsnr_*` when the corresponding option is on.

**Range** — dB. Lower bound is unbounded in principle (a fully inverted
frame at 8 bpc gives ~0 dB); upper bound is `psnr_max` unless `uncapped` is
set, in which case only the `mse == 0` case reports `psnr_max`.

**Input formats** — YUV 4:2:0 / 4:2:2 / 4:4:4 / 4:0:0 at 8 / 10 / 12 / 16 bpc.

## Notes and limitations

- The `psnr` extractor sets the temporal flag only because `apsnr`
  accumulates across the clip; the per-frame PSNR itself is stateless.
- `apsnr_*` has its own ceiling, `ceil(10 * log10(peak^2 * n_pixels))`,
  which is a true theoretical maximum rather than a truncation and is not
  affected by `uncapped`.
- A PSNR gap that looks like "28 dB where I expected 72 dB" is almost never
  this ceiling — a `MIN` can only lower a value. Check frame alignment in
  the decode graph first.

## See also

- [Feature overview](features.md) — the full extractor table.
- [ADR-1193](../adr/1193-psnr-uncapped-option.md) — why `uncapped` is opt-in.
- [PSNR-HVS](psnr-hvs.md) — the perceptually weighted variant.
